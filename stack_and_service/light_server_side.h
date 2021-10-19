#ifndef __LIGHT_SERVER_SIDE_H__
#define __LIGHT_SERVER_SIDE_H__

#include <fcntl.h>
#include <assert.h>
#include <light_log.h>

#include "light_common.h"
#include "service/ll.h"
#include "../light_debug.h"

#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

#define PKTMBUF_HEADROOM 128
#define LIGHT_BUFSIZE (PKTMBUF_HEADROOM + 1448)
#define MAX_CORES_NUM 32

#ifndef EPOLL_SUPPORT
#define EPOLL_SUPPORT
#endif

struct light_clients
{
    TAILQ_ENTRY(light_clients) queue_entry;
    struct rte_ring *client_ring;
    int is_busy;
};

typedef struct
{
    struct socket *socket;
    int ringset_idx;
    int parent_idx;
    struct rte_ring *tx_ring;
    struct rte_ring *rx_ring;
    pid_t apppid;
} socket_satelite_data_t;

extern int proc_id;
extern unsigned num_procs;
extern struct light_clients light_clients[LIGHT_CLIENTS_POOL_SIZE];
extern socket_satelite_data_t *socket_satelite_data;
extern light_socket_t *g_light_sockets;
extern uint64_t user_pending_accept;
extern light_epoll_t *g_light_epolls;

struct rte_ring *command_rings[MAX_CORES_NUM];
extern struct rte_ring *command_ring;
struct rte_ring *urgent_command_rings[MAX_CORES_NUM];
extern struct rte_ring *urgent_command_ring;

extern struct rte_ring *free_connections_ring;
extern struct rte_ring *free_clients_ring;
extern struct rte_ring *rx_mbufs_ring;
extern struct rte_ring *epolls_ring;
extern struct rte_ring *fd_mapping_ring;

struct rte_mempool *free_command_pools[MAX_CORES_NUM];
extern struct rte_mempool *free_connections_pool;
extern struct rte_mempool *free_command_pool;
extern struct rte_mempool *free_return_value_pool;
extern struct rte_mempool *free_epoll_node_pool;
extern struct rte_mempool *free_event_info_pool;
extern struct rte_mempool *free_socket_satelite_data_pool;
extern struct rte_mempool *epolls_pool;
extern struct rte_mempool *free_fd_mapping_storage_pool;
extern struct rte_mempool *free_monitor_pool;

static inline struct light_memory *light_service_api_init(int command_bufs_count,
        int rx_bufs_count,
        int tx_bufs_count)
{
    char ringname[1024];
    int ringset_idx, i;
    light_socket_t *light_socket;
    enum rte_proc_type_t proc_type;
    proc_type = rte_eal_process_type();
    memset(light_clients, 0, sizeof(light_clients));

    if (proc_type == RTE_PROC_PRIMARY)
    {
        free_clients_ring = rte_ring_create(FREE_CLIENTS_RING, LIGHT_CLIENTS_POOL_SIZE, rte_socket_id(), 0);
        if (!free_clients_ring)
        {
            light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
            exit(0);
        }
        light_log(LIGHT_LOG_INFO, "FREE CLIENTS RING CREATED\n");
        for (ringset_idx = 0; ringset_idx < LIGHT_CLIENTS_POOL_SIZE; ringset_idx++)
        {
            if (rte_ring_enqueue(free_clients_ring, (void*)ringset_idx) != 0) {
                printf("Warning: In light_service_api_init, rte_ring_enqueue error\n");
            }
            sprintf(ringname, "%s%d", FREE_CLIENTS_RING, ringset_idx);
            light_clients[ringset_idx].client_ring = rte_ring_create(ringname, 64, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
            if (!light_clients[ringset_idx].client_ring)
            {
                light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
                exit(0);
            }
        }
    }
    else
    {
        free_clients_ring = rte_ring_lookup(FREE_CLIENTS_RING);
        if (!free_clients_ring)
        {
            light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
            exit(0);
        }
    }

    if (proc_type == RTE_PROC_PRIMARY)
    {
        sprintf(ringname, "FREE_SOCKET_SAT_DATA_POOL");
        free_socket_satelite_data_pool = rte_mempool_create(ringname, 1,
                                         sizeof(socket_satelite_data_t) * LIGHT_CONNECTION_POOL_SIZE, 0,
                                         0,
                                         NULL, NULL,
                                         NULL, NULL,
                                         rte_socket_id(), 0);
    }
    else
    {
        sprintf(ringname, "FREE_SOCKET_SAT_DATA_POOL");
        free_socket_satelite_data_pool = rte_mempool_lookup(ringname);
    }

    if (!free_socket_satelite_data_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot create mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }

    light_log(LIGHT_LOG_INFO, "FREE SOCKET SATELITE DATA_POOL CREATED\n");
    if (rte_mempool_get(free_socket_satelite_data_pool, (void **)&socket_satelite_data))
    {
        light_log(LIGHT_LOG_ERR, "cannot get from mempool %s %d \n", __FILE__, __LINE__);
        exit(0);
    }

    rte_mempool_put(free_socket_satelite_data_pool, (void *)socket_satelite_data);

    if (proc_type == RTE_PROC_PRIMARY)
    {
        memset(socket_satelite_data, 0, sizeof(socket_satelite_data_t)*LIGHT_CONNECTION_POOL_SIZE);
    }

    if (proc_type == RTE_PROC_PRIMARY)
    {
        struct rte_ring *ring;
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "COMMAND_RING%d", i);
            ring = rte_ring_create(ringname, command_bufs_count, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
            command_rings[i] = ring;
            if (i == proc_id)
            {
                command_ring = ring;
            }
        }
    }
    else
    {
        sprintf(ringname, "COMMAND_RING%d", proc_id);
        command_ring = rte_ring_lookup(ringname);
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "COMMAND_RING%d", i);
            command_rings[i] = rte_ring_lookup(ringname);
        }
    }

    if (!command_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "COMMAND RING CREATED\n");

#ifdef LIGHT_SERVER_SIDE_DEBUG
    printf("proc_id = %d, address of command_ring = %x\n", proc_id, command_ring);
#endif

    //********************* start of urgent command ring ***************************
    if (proc_type == RTE_PROC_PRIMARY)
    {
        struct rte_ring *ring;
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "URGENT_COMMAND_RING%d", i);
            ring = rte_ring_create(ringname, command_bufs_count, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
            urgent_command_rings[i] = ring;
            if (i == proc_id)
            {
                urgent_command_ring = ring;
            }
        }
    }
    else
    {
        sprintf(ringname, "URGENT_COMMAND_RING%d", proc_id);
        urgent_command_ring = rte_ring_lookup(ringname);
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "URGENT_COMMAND_RING%d", i);
            urgent_command_rings[i] = rte_ring_lookup(ringname);
        }
    }

    if (!urgent_command_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot create urgent command ring %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "URGENT COMMAND RING CREATED\n");
    //********************* end of urgent command ring ***************************

    if (proc_type == RTE_PROC_PRIMARY)
    {
        struct rte_ring *ring;
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "rx_mbufs_ring%d", i);
            ring = rte_ring_create(ringname, rx_bufs_count * LIGHT_CONNECTION_POOL_SIZE, rte_socket_id(), 0);

            if (i == proc_id)
            {
                rx_mbufs_ring = ring;
            }
        }
    }
    else
    {
        sprintf(ringname, "rx_mbufs_ring%d", proc_id);
        rx_mbufs_ring = rte_ring_lookup(ringname);
    }

    if (!rx_mbufs_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "RX RING CREATED\n");

    if (proc_type == RTE_PROC_PRIMARY)
    {
        struct rte_mempool *mp;
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "CMD_POOL%d", i);
#ifdef LIGHT_SERVER_SIDE_DEBUG
            printf("%s : element_size = %d, number = %d\n", ringname, sizeof(light_cmd_t), command_bufs_count);
#endif
            mp = rte_mempool_create(ringname, command_bufs_count,
                                    sizeof(light_cmd_t), 0,
                                    0,
                                    NULL, NULL,
                                    NULL, NULL,
                                    rte_socket_id(), 0);
            free_command_pools[i] = mp;
            if (i == proc_id)
            {
                free_command_pool = mp;
            }
        }
    }
    else
    {
        sprintf(ringname, "CMD_POOL%d", proc_id);
        free_command_pool = rte_mempool_lookup(ringname);

        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "CMD_POOL%d", i);
            free_command_pools[i] = rte_mempool_lookup(ringname);
        }
    }

    if (!free_command_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot create mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "COMMAND POOL CREATED\n");


    if (proc_type == RTE_PROC_PRIMARY)
    {
        struct rte_mempool *mp;
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sprintf(ringname, "RETURN_VALUE%d", i);
#ifdef LIGHT_SERVER_SIDE_DEBUG
            printf("%s : element_size = %d, number = %d\n", ringname, sizeof(light_return_value_t), command_bufs_count);
#endif
            mp = rte_mempool_create(ringname, command_bufs_count,
                                    sizeof(light_return_value_t), 0,
                                    0,
                                    NULL, NULL,
                                    NULL, NULL,
                                    rte_socket_id(), 0);
            if (i == proc_id)
            {
                free_return_value_pool = mp;
            }
        }
    }
    else
    {
        sprintf(ringname, "RETURN_VALUE%d", proc_id);
        free_return_value_pool = rte_mempool_lookup(ringname);
    }

    if (!free_return_value_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot create mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "RETURN VALUE POOL CREATED\n");

    if (proc_type == RTE_PROC_PRIMARY)
    {
#ifdef LIGHT_SERVER_SIDE_DEBUG
        printf("%s : element_size = %d, number = %d\n", FREE_EPOLL_NODE_POOL_NAME, sizeof(light_epoll_node_t), command_bufs_count);
#endif
        free_epoll_node_pool = rte_mempool_create(FREE_EPOLL_NODE_POOL_NAME, command_bufs_count,
                               sizeof(light_epoll_node_t), 0,
                               0,
                               NULL, NULL,
                               NULL, NULL,
                               rte_socket_id(), 0);
    }
    else
    {
        free_epoll_node_pool = rte_mempool_lookup(FREE_EPOLL_NODE_POOL_NAME);
    }
    if (!free_epoll_node_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot create mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "EPOLL NODE POOL CREATED\n");

    if (proc_type == RTE_PROC_PRIMARY)
    {
        sprintf(ringname, FREE_CONNECTIONS_POOL_NAME);
#ifdef LIGHT_SERVER_SIDE_DEBUG
        printf("%s : element_size = %d, number = %d\n", ringname, sizeof(light_socket_t), LIGHT_CONNECTION_POOL_SIZE);
#endif
        free_connections_pool = rte_mempool_create(ringname, 1,
                                sizeof(light_socket_t) * LIGHT_CONNECTION_POOL_SIZE, 0,
                                0,
                                NULL, NULL,
                                NULL, NULL,
                                rte_socket_id(), 0);
    }
    else
    {
        sprintf(ringname, FREE_CONNECTIONS_POOL_NAME);
        free_connections_pool = rte_mempool_lookup(ringname);
    }

    if (!free_connections_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot create mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }

    light_log(LIGHT_LOG_INFO, "FREE CONNECTIONS POOL CREATED\n");

    if (rte_mempool_get(free_connections_pool, (void **)&g_light_sockets))
    {
        light_log(LIGHT_LOG_ERR, "cannot get from mempool %s %d \n", __FILE__, __LINE__);
        exit(0);
    }

    rte_mempool_put(free_connections_pool, (void *)g_light_sockets);
    light_socket = g_light_sockets;
    if (proc_type == RTE_PROC_PRIMARY)
    {
        free_connections_ring = rte_ring_create(FREE_CONNECTIONS_RING, LIGHT_CONNECTION_POOL_SIZE, rte_socket_id(), 0);
    }
    else
    {
        free_connections_ring = rte_ring_lookup(FREE_CONNECTIONS_RING);
    }

    if (!free_connections_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
        exit(0);
    }

    light_log(LIGHT_LOG_INFO, "FREE CONNECTIONS RING CREATED\n");
    if (proc_type == RTE_PROC_PRIMARY)
    {
        for (ringset_idx = 0; ringset_idx < LIGHT_CONNECTION_POOL_SIZE; ringset_idx++, light_socket++)
        {

            memset(light_socket, 0, sizeof(light_socket_t));
            light_socket->connection_idx = ringset_idx;
            rte_atomic16_init(&light_socket->connect_close_signal);
            // rte_atomic16_init(&light_socket->write_ready_to_app);
            rte_atomic16_init(&light_socket->occupied_mbuf_num);
            rte_atomic32_init(&light_socket->tx_space);
            if (rte_ring_enqueue(free_connections_ring, (void*)light_socket) != 0) {
                printf("Warning: In light_service_api_init, rte_ring_enqueue(free_connections_ring, (void*)light_socket) != 0\n");
            }

            struct light_epoll_node_list* list = &(light_socket->epoll_node_list);
            list->ll_head.size = (size_t)0;
            list->ll_head.q.succ = (uintptr_t) & (list->ll_head);
            list->ll_head.q.pred = (uintptr_t) & (list->ll_head);
            list->ll_head.q.refcnt = (size_t)2;

            sprintf(ringname, TX_RING_NAME_BASE"%d", ringset_idx);
            socket_satelite_data[ringset_idx].tx_ring = rte_ring_create(ringname, tx_bufs_count, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);

            if (!socket_satelite_data[ringset_idx].tx_ring)
            {
                light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
                exit(0);
            }

            // rte_ring_set_water_mark(socket_satelite_data[ringset_idx].tx_ring,/*tx_bufs_count/10*/1);
            sprintf(ringname, RX_RING_NAME_BASE"%d", ringset_idx);
            socket_satelite_data[ringset_idx].rx_ring = rte_ring_create(ringname, rx_bufs_count, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
            if (!socket_satelite_data[ringset_idx].rx_ring)
            {
                light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
                exit(0);
            }
            // rte_ring_set_water_mark(socket_satelite_data[ringset_idx].rx_ring,/*rx_bufs_count/10*/1);
            socket_satelite_data[ringset_idx].ringset_idx = -1;
            socket_satelite_data[ringset_idx].parent_idx = -1;
            socket_satelite_data[ringset_idx].socket = NULL;
            socket_satelite_data[ringset_idx].apppid = 0;
        }
    }

    light_log(LIGHT_LOG_INFO, "CONNECTIONS Tx/Rx RINGS CREATED\n");


    if (proc_type == RTE_PROC_PRIMARY)
    {
        epolls_pool = rte_mempool_create(LIGHT_EPOLL_POOL_NAME, 1,
                                         sizeof(light_epoll_t) * LIGHT_EPOLL_POOL_SIZE, 0,
                                         0,
                                         NULL, NULL,
                                         NULL, NULL,
                                         rte_socket_id(), 0);
    }
    else
    {
        epolls_pool = rte_mempool_lookup(LIGHT_EPOLL_POOL_NAME);
    }

    if (!epolls_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot create mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "epoll pool created\n");

    if (rte_mempool_get(epolls_pool, (void **)(&g_light_epolls)))
    {
        light_log(LIGHT_LOG_ERR, "cannnot get from mempool %s %d\n", __FILE__, __LINE__);
        exit(0);
    }

    rte_mempool_put(epolls_pool, (void *)g_light_epolls);

    if (proc_type == RTE_PROC_PRIMARY)
    {
        epolls_ring = rte_ring_create(LIGHT_EPOLL_RING_NAME, LIGHT_EPOLL_POOL_SIZE, rte_socket_id(), 0);
    }
    else
    {
        epolls_ring = rte_ring_lookup(LIGHT_EPOLL_RING_NAME);
    }

    if (!epolls_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "EPOLL RING CREATED\n");

    if (proc_type == RTE_PROC_PRIMARY)
    {
        light_epoll_t *light_epoll = g_light_epolls;

        for (ringset_idx = 0; ringset_idx < LIGHT_EPOLL_POOL_SIZE; ringset_idx++)
        {
            sprintf(ringname, "EPOLL_RING_NAME%d", ringset_idx);
            light_epoll[ringset_idx].ready_connections = rte_ring_create(ringname, tx_bufs_count << 1, rte_socket_id(), 0);
            if (!light_epoll[ringset_idx].ready_connections)
            {
                light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
                exit(0);
            }
            sprintf(ringname, "EPOLL_SHADOW_RING_NAME%d", ringset_idx);
            light_epoll[ringset_idx].shadow_connections = rte_ring_create(ringname, tx_bufs_count << 1, rte_socket_id(), 0);
            if (!light_epoll[ringset_idx].shadow_connections)
            {
                light_log(LIGHT_LOG_ERR, "cannot create ring %s %d\n", __FILE__, __LINE__);
                exit(0);
            }
            light_log(LIGHT_LOG_INFO, "EPOLL READY RING#%d CREATED\n", ringset_idx);
            light_epoll[ringset_idx].epoll_idx = ringset_idx;
            light_epoll[ringset_idx].is_active = 0;
            light_epoll[ringset_idx].is_sleeping = 0;
            rte_rwlock_init(&light_epoll[ringset_idx].ep_lock);
            if (rte_ring_enqueue(epolls_ring, (void*)(&light_epoll[ringset_idx])) != 0) {
                printf("rte_ring_enqueue(epolls_ring, (void*)(&light_epoll[ringset_idx])) != 0\n");
            }
        }
    }

    if (proc_type == RTE_PROC_PRIMARY) {
        free_event_info_pool = rte_mempool_create(FREE_EVENT_INFO_POOL_NAME,
                                                    LIGHT_EVENT_INFO_POOL_SIZE, sizeof(light_event_info_t),
                                                    0, 0,
                                                    NULL, NULL,
                                                    NULL, NULL,
                                                    rte_socket_id(), 0);
    } else {
        free_event_info_pool = rte_mempool_lookup(FREE_EVENT_INFO_POOL_NAME);
    }

    if (!free_event_info_pool) {
        light_log(LIGHT_LOG_ERR, "%s:%d cannot create event info pool.\n", __FILE__, __LINE__);
        exit(0);
    }
    light_log(LIGHT_LOG_INFO, "event info pool created");

    printf("sizeof(fd_mapping_t) = %d\n", sizeof(fd_mapping_t));
    if (proc_type == RTE_PROC_PRIMARY) {
        free_fd_mapping_storage_pool = rte_mempool_create(FREE_FD_MAPPING_STORAGE_POOL_NAME,
                                                    TRUE_FD_NUM, sizeof(fd_mapping_t),
                                                    0, 0,
                                                    NULL, NULL,
                                                    NULL, NULL,
                                                    rte_socket_id(), 0);
    } else {
        free_fd_mapping_storage_pool = rte_mempool_lookup(FREE_FD_MAPPING_STORAGE_POOL_NAME);
    }
    if (free_fd_mapping_storage_pool == NULL)
    {
        light_log(LIGHT_LOG_ERR, "cannot create free_fd_mapping_storage_pool\n", __FILE__, __LINE__);
        printf("cannot create free_fd_mapping_storage_pool\n");
        rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));
    }

    //create workers_info
    if(proc_type == RTE_PROC_PRIMARY){
        struct rte_mempool *workers_info_pool =
                         rte_mempool_create("WORKER_INFO", 1,
                         sizeof(light_worker_info_t), 0,
                         0,
                         NULL, NULL,
                         NULL, NULL,
                         rte_socket_id(), 0);
        int res;
        do{
            light_worker_info_t* workers_info;
            res = rte_mempool_get(workers_info_pool, (void**)&workers_info);
            if(res == 0){

                rte_rwlock_init( &(workers_info->register_worker_lock) );

                //conflict frequency is very low 
                rte_rwlock_write_lock( &(workers_info->register_worker_lock) );
                workers_info->register_worker = 0;
                rte_rwlock_write_unlock( &(workers_info->register_worker_lock) );
                

                rte_rwlock_init( &(workers_info->workers_served_lock) );

                int core_id;
                //conflict frequency is very low 
                rte_rwlock_write_lock( &(workers_info->workers_served_lock) );
                for(core_id = 0; core_id < num_procs; core_id ++){
                    workers_info->workers_served[core_id] = 0;  //served 0 worker
                }
                rte_rwlock_write_unlock( &(workers_info->workers_served_lock) );

                rte_mempool_put(workers_info_pool, (void*)workers_info);
            }
        }while(res != 0);
    }

    light_log(LIGHT_LOG_INFO, "DONE\n");
    return 0;
}


static inline void debug_ref(light_epoll_node_t *n)
{
    printf("&n->refcnt=%d\n", atomic_load_explicit(&(&n->entry)->refcnt, memory_order_relaxed) );
}

static inline void _print_epoll_node_in_socket(light_socket_t *socket)
{
    light_epoll_node_t *o;
    printf("epoll_node_list\n" );
    LL_FOREACH(o, light_epoll_node_list, &(socket->epoll_node_list))
    {
        printf("%d\n" , o->epoll_idx);
        debug_ref(o);
    }
}

static inline int add_epoll_event_to_queue(int sockid, uint32_t event)
{
    #ifdef EPOLL_DEBUG
    printf("In add_epoll_event_to_queue, sockid = %d\n", sockid);
    // only for debug
    static int event_add_count;
    static int event_count;
    static struct timeval tv1;
    static long epoll_enqueue_timestamp;
    #endif

    static int batch_backlog;
    static int round_robin_counter;

    #ifdef EPOLL_DEBUG
    gettimeofday(&tv1, NULL);
    epoll_enqueue_timestamp = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
    printf("  start of add_epoll_event_to_queue = %ld us\n", epoll_enqueue_timestamp);
    #endif

    // #ifdef BACKEND_PERF
    // static int connection_count;
    // static struct timeval tv2;
    // static long backend_perf_timestamp1, backend_perf_timestamp2;
    // if (connection_count == 0) {
    //     gettimeofday(&tv2, NULL);
    //     backend_perf_timestamp1 = (tv2.tv_sec) * 1000000 + (tv2.tv_usec);
    // }
    // connection_count ++;
    // if (connection_count % 100 == 99) {
    //     gettimeofday(&tv2, NULL);
    //     backend_perf_timestamp2 = (tv2.tv_sec) * 1000000 + (tv2.tv_usec);
    //     printf("number of new connections per second = %f\n", (float)(connection_count + 1) * 1000000 / (backend_perf_timestamp2 - backend_perf_timestamp1));
    //     connection_count = 0;
    // }
    // return 0;
    // #endif

    light_socket_t *sk = &(g_light_sockets[sockid]);
    if (rte_atomic16_read( &(sk->is_multicore)))
    {
        sockid = sk->multicore_socket_copies[proc_id];
        sk = &(g_light_sockets[sockid]);
    }
    light_log(LIGHT_LOG_INFO, "add_epoll_event_to_queue real socket = %d, event = %d\n", sockid, event);
    light_epoll_node_t *epoll_node = NULL;
    // rte_rwlock_read_lock(&sk->ep_sock_lock);
    int N = LL_SIZE(light_epoll_node_list, &(sk->epoll_node_list));
    // rte_rwlock_read_unlock(&sk->ep_sock_lock);

    #ifdef LIGHT_SERVER_SIDE_DEBUG
        printf("sockid=%d epoll_node_list size=%d\n", sockid, N);
        _print_epoll_node_in_socket(sk);
    #endif

    #ifdef EPOLL_DEBUG_COUNT
    event_add_count++;
    #endif

    if (N == 0) 
    {
        #ifdef EPOLL_DEBUG
        printf("N == 0\n");
        #endif

        return -1;
    }
    else if (N == 1) 
    {
        epoll_node = LL_FIRST(light_epoll_node_list, &(sk->epoll_node_list));
    }
    else // randomly take out one of epoll_nodes. TODO: to be modified to take out all epoll_nodes.
    {
        // int step = rand() % N; // at random
        int step = round_robin_counter % N; // round robin
        round_robin_counter ++;
        
        int i = 0;
        LL_FOREACH(epoll_node, light_epoll_node_list, &(sk->epoll_node_list))
        {
            if (i == step)
            {
                break;
            }
            ++i;
        }
    }

    if (epoll_node != NULL)
    {
        #ifdef EPOLL_DEBUG
        printf("In add_epoll_event_to_queue, epoll_node != NULL\n");
        #endif

        light_epoll_t *ep = &(g_light_epolls[epoll_node->epoll_idx]);
        if (!sk || !ep)
        {
            light_log(LIGHT_LOG_ERR, "add_epoll_event_to_queue: sk or ep is NULL. %d\n", __LINE__);
            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
            return -1;
        }
        // rte_rwlock_write_lock(&ep->ep_lock); //origin
        if (!(epoll_node->epoll_events & event))
        {
            light_log(LIGHT_LOG_ERR, "add_epoll_event_to_queue: event doesn't match. %d\n", __LINE__);
            // rte_rwlock_write_unlock(&ep->ep_lock);
            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
            return -1;
        }
        // rte_rwlock_write_lock(&ep->ep_lock);
        // if (epoll_node->ready_events & event)
        // {
        //     light_log(LIGHT_LOG_INFO, "add_epoll_event_to_queue: event has been ready. %d\n", __LINE__);
        //     printf("  add_epoll_event_to_queue: event has been ready.\n");
        //     if(ep->is_sleeping && socket_satelite_data[sockid].apppid){
        //         errno = 0;
        //         int rres = write(ep->fifo_write_fds[proc_id], "1", 1);
        //     }
        //     // rte_rwlock_write_unlock(&ep->ep_lock);
        //     LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
        //     return 0;
        // }
        light_event_info_t *event_info;
        // rte_rwlock_write_lock(&ep->ep_lock);
        if (rte_mempool_get(free_event_info_pool, (void **)&event_info)) {
            light_log(LIGHT_LOG_ERR, "add_epoll_event_to_queue: cannot get event info node. %d\n", __LINE__);
            // rte_rwlock_write_unlock(&ep->ep_lock);
            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
            return -1;
        }
        event_info->sockfd = sockid;
        event_info->event = event;
        
        // rte_rwlock_write_lock(&ep->ep_lock);
        int ret = rte_ring_enqueue(ep->ready_connections, (void *)event_info); //do not need lock
        // rte_rwlock_write_unlock(&ep->ep_lock);



        if (ret != 0)
        {
            light_log(LIGHT_LOG_ERR, "  Error: add_epoll_event_to_queue: cannot enqueue ready_conections sockid = %d. %d\n", sockid,  __LINE__);
            printf("  Error: add_epoll_event_to_queue: cannot enqueue ready_conections sockid = %d. %d\n", sockid,  __LINE__);
            // rte_rwlock_write_unlock(&ep->ep_lock);
            rte_mempool_put(free_event_info_pool, (void *)event_info);
            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
            return -1;
        }
        
        batch_backlog ++;
        rte_rwlock_write_lock(&ep->ep_lock);
        if (ep->is_sleeping && socket_satelite_data[sockid].apppid && (batch_backlog >= 1)) // batch
        {
            batch_backlog = 0;
            // rte_rwlock_write_unlock(&ep->ep_lock);
            errno = 0;

            // rte_rwlock_write_lock(&ep->ep_lock);
            #ifndef BACKEND_PERF
            int rres = write(ep->fifo_write_fds[proc_id], "1", 1);
            // rte_rwlock_write_unlock(&ep->ep_lock);

            light_log(LIGHT_LOG_INFO, "after write fifo(%d,%d), return=%d, errorno=%d  epfd=%d, pid=%d\n", proc_id, \
                ep->fifo_write_fds[proc_id], rres, errno, ep->epoll_idx, ep->pid);
            #endif
        }

        // rte_rwlock_write_lock(&ep->ep_lock);
        epoll_node->ready_events |= event;
        rte_rwlock_write_unlock(&ep->ep_lock);

        light_log(LIGHT_LOG_INFO, "add_epoll_event_to_queue: ep=%d is_sleeping=%d sockid=%d apppid=%d\n", epoll_node->epoll_idx, \
            ep->is_sleeping, sockid, socket_satelite_data[sockid].apppid);

        #ifdef EPOLL_DEBUG_COUNT
        event_count++;
        if (event_count % 1000 == 999){
            printf("event_count = %d and event_add_count = %d\n", event_count, event_add_count);
        }
        // gettimeofday(&tv1, NULL);
        // epoll_enqueue_timestamp = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
        // printf("epoll_enqueue_timestamp = %ld us\n", epoll_enqueue_timestamp);
        #endif

        LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);

        #ifdef BACKEND_PERF
        static int connection_count;
        static struct timeval tv2;
        static long backend_perf_timestamp1, backend_perf_timestamp2;
        if (connection_count == 0) {
            gettimeofday(&tv2, NULL);
            backend_perf_timestamp1 = (tv2.tv_sec) * 1000000 + (tv2.tv_usec);
        }
        connection_count ++;
        if (connection_count % 100 == 99) {
            gettimeofday(&tv2, NULL);
            backend_perf_timestamp2 = (tv2.tv_sec) * 1000000 + (tv2.tv_usec);
            printf("number of new connections per second = %f\n", (float)(connection_count + 1) * 1000000 / (backend_perf_timestamp2 - backend_perf_timestamp1));
            connection_count = 0;
        }
        #endif
        
        return 0;
    }
    light_log(LIGHT_LOG_WARNING, "add_epoll_event_to_queue: epoll_node is NULL\n");
    printf("warning: add_epoll_event_to_queue: epoll_node is NULL\n");
    return -1;
}

static inline void light_mark_readable(void *descriptor)
{
    light_log(LIGHT_LOG_DEBUG, "light_mark_readable: ring_idx=%d rx_ring count=%d\n", socket_satelite_data->ringset_idx, rte_ring_count(socket_satelite_data->rx_ring));
    // socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    // add_epoll_event_to_queue(socket_satelite_data->ringset_idx, LIGHT_EPOLLIN);
    add_epoll_event_to_queue(((socket_satelite_data_t *)descriptor)->ringset_idx, LIGHT_EPOLLIN);
}

// return value: 0 -> success; -1 -> failure
static inline int light_mark_writable(void *descriptor)
{
    // socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    // int sk_idx = socket_satelite_data->ringset_idx;
    // return add_epoll_event_to_queue(sk_idx, LIGHT_EPOLLOUT);
    return add_epoll_event_to_queue(((socket_satelite_data_t *)descriptor)->ringset_idx, LIGHT_EPOLLOUT);
}

static void light_mark_closed(void *descriptor, int close_num)
{
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    rte_atomic16_set(&g_light_sockets[socket_satelite_data->ringset_idx].connect_close_signal, close_num);
    if (add_epoll_event_to_queue(socket_satelite_data->ringset_idx, LIGHT_EPOLLHUP) < 0) {
        light_log(LIGHT_LOG_ERR, "light_mark_closed: add_epoll_event_to_queue failure\n");
    }
}

static inline void _light_free_command_buf(light_cmd_t *cmd)
{
    rte_mempool_put(free_command_pool, (void *)cmd);
}

static inline void light_post_accepted(light_cmd_t *cmd, void *parent_descriptor)
{
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)parent_descriptor;
    if (rte_ring_sp_enqueue(socket_satelite_data->rx_ring, (void *)cmd) == -ENOBUFS)
    {
        printf("Error: rte_ring_sp_enqueue(socket_satelite_data->rx_ring, (void *)cmd) == -ENOBUFS\n");
        _light_free_command_buf(cmd);
        light_log(LIGHT_LOG_ERR, "light_post_accepted: cannot enqueue accept cmd\n");
    }
    else
    {
        light_log(LIGHT_LOG_DEBUG, "post accepted ring_idx=%d  rx_ring count=%d\n", socket_satelite_data->ringset_idx, rte_ring_count(socket_satelite_data->rx_ring));
        light_mark_readable(parent_descriptor);
    }
    user_pending_accept++;
}

// dequeue one cmd from cmd queue.
static inline light_cmd_t *light_dequeue_command_buf()
{
    // int num = rte_ring_count(command_ring);
    // static int overload_num = 0;
    // if (!num) {
    //     return NULL;
    // }
    // if (num > 1) {
    //     overload_num++;
    // }
    // if (overload_num%10000 == 99){
    //     printf("overload_num = %d\n", overload_num);
    //     printf("rte_ring_count(command_ring) = %d\n", num);
    // }

    light_cmd_t *cmd;
    if(rte_ring_sc_dequeue(command_ring, &cmd)) { // return 0: Success, objects dequeued.
        printf("Error: rte_ring_sc_dequeue(command_ring, &cmd) != 0\n");
        light_log(LIGHT_LOG_DEBUG, "light_dequeue_command_buf: no cmd in ring\n");
        return NULL;
    }
    return cmd;
}

// dequeue n or 0 cmds from cmd queue. (less overhead than light_dequeue_command_buf_burst)
static inline int light_dequeue_command_buf_bulk(void **cmd, int n)
{
    if (rte_ring_sc_dequeue_bulk(command_ring, cmd, n) == 0) {
        return 0;
    } else {
        printf("Error: rte_ring_sc_dequeue_bulk(command_ring, cmd, n) != 0\n");
        return -ENOENT;
    }
    // return rte_ring_sc_dequeue_bulk(command_ring, cmd, n); 
}

// dequeue at most n cmds from cmd queue.
static inline int light_dequeue_command_buf_burst(void **cmd, int n)
{
    // rte_ring_sc_dequeue_burst, return n: Actual number of objects dequeued;
    int ret = rte_ring_sc_dequeue_burst(command_ring, cmd, n);
    if (ret == 0) {
        printf("Error: rte_ring_sc_dequeue_burst(command_ring, cmd, n) == 0\n");
        return 0;
    } else {
        return ret;
    }
    // return rte_ring_sc_dequeue_burst(command_ring, cmd, n);
}

static inline light_cmd_t *light_dequeue_urgent_command_buf()
{
    int num = rte_ring_count(urgent_command_ring);
    static int overload_num = 0;
    if (!num) {
        return NULL;
    }
    // if (num > 1) {
    //     overload_num++;
    // }
    // if (overload_num%10000 == 99){
    //     printf("overload_num = %d\n", overload_num);
    //     printf("rte_ring_count(urgent_command_ring) = %d\n", num);
    // }

    light_cmd_t *cmd;
    if(rte_ring_sc_dequeue(urgent_command_ring, &cmd)) {
        printf("Error: rte_ring_sc_dequeue(urgent_command_ring, &cmd) != 0\n");
        light_log(LIGHT_LOG_DEBUG, "light_dequeue_command_buf: no cmd in ring\n");
        return NULL;
    }
    return cmd;
}

static inline void _light_free_return_value_buf(light_return_value_t *res)
{
    rte_mempool_put(free_return_value_pool, (void *)res);
}

static inline void _light_free_epoll_node_buf(light_epoll_node_t *node)
{
    rte_mempool_put(free_epoll_node_pool, (void *)node);
}

static inline struct rte_mbuf *light_dequeue_tx_buf(void *descriptor)
{
    struct rte_mbuf *mbuf;
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    if (rte_ring_sc_dequeue(socket_satelite_data->tx_ring, (void **)&mbuf))
    {
        // printf("Error: in light_dequeue_tx_buf, rte_ring_sc_dequeue(socket_satelite_data->tx_ring, (void **)&mbuf) != 0\n");
        light_log(LIGHT_LOG_DEBUG, "light_dequeue_tx_buf: no tx_ring dequeue, ring_idx=%d\n", socket_satelite_data->ringset_idx);
        return NULL;
    }
    return mbuf;
}

static inline int light_dequeue_tx_buf_burst(void *descriptor, struct rte_mbuf **mbufs, int max_count)
{
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    int ret = rte_ring_sc_dequeue_burst(socket_satelite_data->tx_ring, (void **)mbufs, max_count);
    if (ret == 0) {
        printf("Error: rte_ring_sc_dequeue_burst(socket_satelite_data->tx_ring, (void **)mbufs, max_count) == 0\n");
    }
    // return rte_ring_sc_dequeue_burst(socket_satelite_data->tx_ring, (void **)mbufs, max_count);
    return ret;
}

static inline int light_tx_buf_count(void *descriptor)
{
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    rte_atomic16_set(&g_light_sockets[socket_satelite_data->ringset_idx].write_done_from_app, 0);
    return rte_ring_count(socket_satelite_data->tx_ring);
}

static inline int light_rx_buf_free_count(void *descriptor)
{
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    return rte_ring_free_count(socket_satelite_data->rx_ring);
}

static inline int light_submit_rx_buf(struct rte_mbuf *mbuf, void *descriptor)
{
    socket_satelite_data_t *socket_satelite_data = (socket_satelite_data_t *)descriptor;
    int rc = rte_ring_sp_enqueue(socket_satelite_data->rx_ring, mbuf);
    if (rc == 0) {
        light_mark_readable(descriptor);
    } else {
    	printf("Error: light_submit_rx_buf can not enqueue rx_ring\n");
    }
    return rc;
}

static inline void light_free_socket(int connidx)
{
    if (rte_ring_enqueue(free_connections_ring, (void *)&g_light_sockets[connidx]) != 0) {
        printf("rte_ring_enqueue(free_connections_ring, (void *)&g_light_sockets[connidx]) != 0\n");
    }
}

#endif
