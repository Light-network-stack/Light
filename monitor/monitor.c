#include <stdio.h>
#include <unistd.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_eal.h>
#include "../stack_and_service/light_common.h"

#include <time.h>
#include <sys/time.h>

#define RTE_ARG_COUNT 5

struct rte_ring *free_connections_ring = NULL;
struct rte_ring *free_clients_ring = NULL;
struct rte_ring *epolls_ring = NULL;
struct rte_ring *command_rings[MAX_CORES_NUM];
struct rte_ring *tx_rings[LIGHT_CONNECTION_POOL_SIZE];
struct rte_ring *rx_rings[LIGHT_CONNECTION_POOL_SIZE];
struct rte_ring *local_cache[LIGHT_CONNECTION_POOL_SIZE];
struct rte_ring *epoll_ready_connections[LIGHT_EPOLL_POOL_SIZE];
struct rte_ring *epoll_shadow_connections[LIGHT_EPOLL_POOL_SIZE];

struct rte_mempool *tx_bufs_pool = NULL;
struct rte_mempool *free_event_info_pool = NULL;
struct rte_mempool *free_epoll_node_pool = NULL;
struct rte_mempool *free_fd_mapping_storage_pool = NULL;
struct rte_mempool *free_command_pools[MAX_CORES_NUM];
struct rte_mempool *TCP_SLAB = NULL;
struct rte_mempool *free_monitor_pool = NULL;
struct rte_mempool *free_connections_pool;
struct rte_mempool *epolls_pool;

light_socket_t *g_light_sockets;
light_epoll_t *g_light_epolls;

int *g_monitor_info = NULL;

void dpdk_init();
void lookup_dpdk_resources();
void print_stat();

int main() {
    dpdk_init();
    lookup_dpdk_resources();

    struct timespec tim, tim2;
    static struct timeval tv1;

    tim.tv_sec = 0;
    tim.tv_nsec = 100 * 1000; // ns
    
    while(1) {
        gettimeofday(&tv1, NULL);
        printf("~~~~~~~~ Timestamp: %d s, %d us ~~~~~~~~\n", tv1.tv_sec, tv1.tv_usec);
        
        print_stat();
        // sleep(1);
        if(nanosleep(&tim , &tim2) < 0 ) {
            printf("Nano sleep system call failed \n");
            return -1;
        }
    }
}

void dpdk_init()
{
    int argc = RTE_ARG_COUNT;
    char *argv[RTE_ARG_COUNT] = {"monitor", "-n", "1", "--proc-type", "secondary"};
    if (rte_eal_init(argc, argv) < 0)
    {
        printf("cannot initialize rte_eal");
        exit(1);
    }
}

void lookup_dpdk_resources()
{
    char ringname[1024] = {0};
    free_connections_ring = rte_ring_lookup(FREE_CONNECTIONS_RING);
    if (!free_connections_ring)
    {
        printf("cannot find free connections ring\n");
        exit(1);
    }

    free_clients_ring = rte_ring_lookup(FREE_CLIENTS_RING);
    if (!free_clients_ring) {
        printf("cannot find free clients ring\n");
        exit(1);
    }

    epolls_ring = rte_ring_lookup(LIGHT_EPOLL_RING_NAME);
    if (!epolls_ring)
    {
        printf("cannot find epolls ring\n");
        exit(1);
    }

    int i;
    for (i = 0; i < MAX_CORES_NUM; ++i)
    {
        sprintf(ringname, "COMMAND_RING%d", i);
        command_rings[i] = rte_ring_lookup(ringname);
        if(command_rings[i] == NULL) break;
    }
    if (i == 0)
    {
        printf("cannot find command rings\n");
        exit(1);
    }

    int ring_idx;
    for (ring_idx = 0; ring_idx < LIGHT_CONNECTION_POOL_SIZE; ring_idx++) 
    {
        sprintf(ringname, TX_RING_NAME_BASE"%d", ring_idx);
        tx_rings[ring_idx] = rte_ring_lookup(ringname);
        if (tx_rings[ring_idx] == NULL)
        {
            printf("cannot find tx ring%d\n", ring_idx);
            exit(1);
        }

        sprintf(ringname, RX_RING_NAME_BASE"%d", ring_idx);
        rx_rings[ring_idx] = rte_ring_lookup(ringname);
        if (rx_rings[ring_idx] == NULL)
        {
            printf("cannot find rx ring%d\n", ring_idx);
            exit(1);
        }

        sprintf(ringname, "lrxcacheapp_nginx_%d", ring_idx);
        local_cache[ring_idx] = rte_ring_lookup(ringname);
        if (local_cache[ring_idx] == NULL)
        {
            printf("cannot find local_cache ring%d\n", ring_idx);
            exit(1);
        }
    }

    int epfd;
    for (epfd = 0; epfd < LIGHT_EPOLL_POOL_SIZE; epfd++)
    {
        sprintf(ringname, "EPOLL_RING_NAME%d", epfd);
        epoll_ready_connections[epfd] = rte_ring_lookup(ringname); // rte_ring of ready connections in one epoll.
        if (epoll_ready_connections[epfd] == NULL)
        {
            printf("cannot find epoll_ready_connections ring%d\n", epfd);
            exit(1);
        }

        sprintf(ringname, "EPOLL_SHADOW_RING_NAME%d", epfd);
        epoll_shadow_connections[epfd] = rte_ring_lookup(ringname); // rte_ring of shadow connections in one epoll.
        if (epoll_shadow_connections[epfd] == NULL)
        {
            printf("cannot find epoll_shadow_connections ring%d\n", epfd);
            exit(1);
        }
    }

    tx_bufs_pool = rte_mempool_lookup("mbufs_mempool");
    if (tx_bufs_pool == NULL)
    {
        printf("cannot find tx mbufs_mempool\n");
        exit(1);
    }

    free_event_info_pool = rte_mempool_lookup(FREE_EVENT_INFO_POOL_NAME);
    if (!free_event_info_pool) {
        printf("cannot find free_event_info_pool\n");
        exit(1);
    }

    free_epoll_node_pool = rte_mempool_lookup(FREE_EPOLL_NODE_POOL_NAME);
    if (!free_epoll_node_pool)
    {
        printf("cannot find free_epoll_node_pool\n");
        exit(1);
    }

    free_fd_mapping_storage_pool = rte_mempool_lookup(FREE_FD_MAPPING_STORAGE_POOL_NAME);
    if (free_fd_mapping_storage_pool == NULL)
    {
        printf("cannot find free_fd_mapping_storage_pool\n");
        exit(1);
    }

    free_connections_pool = rte_mempool_lookup(FREE_CONNECTIONS_POOL_NAME);
    if (free_connections_pool == NULL)
    {
        printf("cannot find free_connections_pool\n");
        exit(1);
    }

    if (rte_mempool_get(free_connections_pool, (void **)&g_light_sockets))
    {
        printf("cannot find g_light_sockets\n");
        exit(1);
    }

    epolls_pool = rte_mempool_lookup(LIGHT_EPOLL_POOL_NAME);
    if (epolls_pool == NULL)
    {
        printf("cannot find epolls_pool\n");
        exit(1);
    }

    if (rte_mempool_get(epolls_pool, (void **)&g_light_epolls))
    {
        printf("cannot find g_light_epolls\n");
        exit(1);
    }

    for (i = 0; i < MAX_CORES_NUM; ++i)
    {
        sprintf(ringname, "CMD_POOL%d", i);
        free_command_pools[i] = rte_mempool_lookup(ringname);
        if(free_command_pools[i] == NULL) break;
    }
    if (i == 0)
    {
        printf("cannot find free_command_pools\n");
        exit(1);
    }

    TCP_SLAB = rte_mempool_lookup("TCP");
    if (TCP_SLAB == NULL)
    {
        printf("cannot find TCP_SLAB\n");
        exit(1);
    }

    free_monitor_pool = rte_mempool_lookup("MONITOR_POOL");
    if (!free_monitor_pool)
    {
        printf("cannot get free_monitor_pool\n");
        exit(0);
    }
    printf("free_monitor_pool get\n");
    if (rte_mempool_get(free_monitor_pool, (void **)&g_monitor_info))
    {
        printf("cannot get from mempool free_monitor_pool\n");
        exit(0);
    }
    rte_mempool_put(free_monitor_pool, (void *)g_monitor_info);

}

void print_stat()
{
    printf("==================print_stat==================\n");
    printf("*** The number of connections = %d ***\n", LIGHT_CONNECTION_POOL_SIZE - rte_ring_count(free_connections_ring));
    printf("free_connections_ring count = %u\n", rte_ring_count(free_connections_ring));
    printf("free_clients_ring count = %u\n", rte_ring_count(free_clients_ring));
    printf("epolls_ring count = %u\n", rte_ring_count(epolls_ring));
    int i;
    int rx_count_non_zero_num, tx_count_non_zero_num;
    int rx_count_array[10], tx_count_array[10];
    int rx_count_upper = 0, tx_count_upper = 0;

    for (i = 0; i < 10; i++) {
        rx_count_array[i] = 0;
        tx_count_array[i] = 0;
    }

    for (i = 0; i < MAX_CORES_NUM; i++)
    {
        if (command_rings[i] == NULL) 
            break;
        printf("command_rings core = %d, count = %u\n", i, rte_ring_count(command_rings[i]));
    }

    for (i = 0; i < LIGHT_CONNECTION_POOL_SIZE; i++)
    {
        unsigned int rx_count = rte_ring_count(rx_rings[i]);
        unsigned int tx_count = rte_ring_count(tx_rings[i]);

        /* print out the info of listen socket */
        if (i == 0) {
            printf("socket = %d, rx_ring count = %u\n", i, rx_count);
            printf("socket = %d, tx_ring count = %u\n", i, tx_count);
        }
    
        if (rx_count > 0) {
            if (rx_count < 10) {
                rx_count_array[rx_count] ++;
            } else {
                rx_count_upper ++;
                printf("rx_ring socket = %d, count = %u\n", i, rx_count);
            }
        }

        if (tx_count > 0) {
            if (tx_count < 10) {
                tx_count_array[tx_count] ++;
            } else {
                tx_count_upper ++;
                printf("tx_ring socket = %d, count = %u\n", i, tx_count);
            }
        }

        unsigned int occupied_mbuf_num = rte_atomic16_read(&g_light_sockets[i].occupied_mbuf_num);
        if (occupied_mbuf_num > 0) 
            printf("occupied_mbuf_num socket = %d, count = %u\n", i, occupied_mbuf_num);

        unsigned int local_cache_count = rte_ring_count(local_cache[i]);
        if (local_cache_count > 0) 
            printf("local_cache socket = %d, count = %u\n", i, local_cache_count);
    }

    for (i = 0; i < 3; i++) {
        printf("rx_count_array[%d] = %d\n", i, rx_count_array[i]);
    }
    printf("rx_count_upper = %d\n", rx_count_upper);

    for (i = 0; i < 3; i++) {
        printf("tx_count_array[%d] = %d\n", i, tx_count_array[i]);
    }
    printf("tx_count_upper = %d\n", tx_count_upper);

    printf("** epoll_ready_connections status **\n");
    for (i = 0; i < LIGHT_EPOLL_POOL_SIZE; i++)
    {
        unsigned int epoll_ready_count = rte_ring_count(epoll_ready_connections[i]);
        if (epoll_ready_count > 0) 
            printf("epoll_ready_connections: epfd = %d, count = %u\n", i, epoll_ready_count);

        unsigned int epoll_shadow_count = rte_ring_count(epoll_shadow_connections[i]);
        if (epoll_shadow_count > 0) 
            printf("epoll_shadow_connections: epfd = %d, count = %u\n", i, epoll_shadow_count);
    }
    printf("-- end of epoll_ready_connections status --\n");

    // Todo: tx_bufs_pool count exception
    unsigned int tx_bufs_pool_used_count = rte_mempool_in_use_count(tx_bufs_pool);
    // if (tx_bufs_pool_used_count > 0)
    // {
    //     printf("tx_bufs_pool_used_count = %u\n", tx_bufs_pool_used_count);
    //     printf("tx_bufs_pool_avail_count = %u\n", rte_mempool_avail_count(tx_bufs_pool));
    // }
    printf("tx_bufs_pool [used/unused] = [%u/%u]\n", tx_bufs_pool_used_count, rte_mempool_avail_count(tx_bufs_pool));

    unsigned int free_event_info_pool_used_count = rte_mempool_in_use_count(free_event_info_pool);
    printf("free_event_info_pool_used_count = %u\n", free_event_info_pool_used_count);

    unsigned int free_epoll_node_pool_used_count = rte_mempool_in_use_count(free_epoll_node_pool);
    printf("free_epoll_node_pool_used_count = %u\n", free_epoll_node_pool_used_count);

    unsigned int free_fd_mapping_storage_pool_used_count = rte_mempool_in_use_count(free_fd_mapping_storage_pool);
    if (free_fd_mapping_storage_pool_used_count > 0)
    {
        printf("free_fd_mapping_storage_pool_used_count = %u\n", free_fd_mapping_storage_pool_used_count);
    }

    for (i = 0; i < MAX_CORES_NUM; i++)
    {
        if (free_command_pools[i] == NULL) break;
        unsigned int free_command_pool_used_count = rte_mempool_in_use_count(free_command_pools[i]);
        if (free_command_pool_used_count > 0)
        {
            printf("free_command_pools: core_id = %d, used_count = %u\n", i, free_command_pool_used_count);
        }
    }

    unsigned int TCP_SLAB_used_count = rte_mempool_in_use_count(TCP_SLAB);
    printf("TCP_SLAB_used_count = %u\n", TCP_SLAB_used_count);
    printf("number of sk alloc = %d\n", g_monitor_info[1]);
    printf("number of failure in sk alloc = %d\n", g_monitor_info[2]);
    // printf("tcp_close: total = %d\n", g_monitor_info[3]);
    // printf("tcp_close: before sock_put = %d\n", g_monitor_info[9]);
    // printf("tcp_close: enter sk_free = %d\n", g_monitor_info[10]);
    // printf("tcp_close: enter __sk_free = %d\n", g_monitor_info[11]);
    // printf("sk_prot_free: total = %d\n", g_monitor_info[0]);
    printf("tcp_v4_conn_request: total = %d\n", g_monitor_info[12]);
    printf("tcp_v4_conn_request: (&inet_csk(sk)->icsk_accept_queue)->listen_opt->qlen = %d\n", g_monitor_info[13]);

    printf("LIGHT_SOCKET_TX_KICK_COMMAND:\n");
    printf("user_on_transmission_opportunity: total = %d\n", g_monitor_info[4]);
    printf("user_on_transmission_opportunity: return (socket_satelite_data = NULL) = %d\n", g_monitor_info[5]);
    printf("user_on_transmission_opportunity: return (ring_entries == 0) = %d\n", g_monitor_info[6]);
    printf("user_on_transmission_opportunity: while = %d\n", g_monitor_info[7]);
    printf("process_tx_ready_sockets():\n");
    printf("user_on_transmission_opportunity: total = %d\n", g_monitor_info[14]);
    printf("user_on_transmission_opportunity: return (socket_satelite_data = NULL) = %d\n", g_monitor_info[15]);
    printf("user_on_transmission_opportunity: return (ring_entries == 0) = %d\n", g_monitor_info[16]);
    printf("user_on_transmission_opportunity: while = %d\n", g_monitor_info[17]);
    printf("rte_eth_tx_burst: total = %d\n", g_monitor_info[8]);

    printf("==================stat_end==================\n\n");
}
