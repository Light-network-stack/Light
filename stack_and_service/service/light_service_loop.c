#include "../../light_debug.h"
#include <specific_includes/dummies.h>
#include <specific_includes/linux/types.h>
#include <specific_includes/linux/bitops.h>
#include <specific_includes/linux/slab.h>
#include <specific_includes/linux/hash.h>
#include <specific_includes/linux/socket.h>
#include <specific_includes/linux/sockios.h>
#include <specific_includes/linux/if_ether.h>
#include <specific_includes/linux/netdevice.h>
#include <specific_includes/linux/etherdevice.h>
#include <specific_includes/linux/ethtool.h>
#include <specific_includes/linux/skbuff.h>
#include <specific_includes/net/net_namespace.h>
#include <specific_includes/net/sock.h>
#include <specific_includes/linux/rtnetlink.h>
#include <specific_includes/net/dst.h>
#include <specific_includes/net/checksum.h>
#include <specific_includes/linux/err.h>
#include <specific_includes/linux/if_arp.h>
#include <specific_includes/linux/if_vlan.h>
#include <specific_includes/linux/ip.h>
#include <specific_includes/net/ip.h>
#include <specific_includes/linux/ipv6.h>
#include <specific_includes/linux/in.h>
#include <specific_includes/net/netlink.h>

#include <string.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_ring.h>
#include <rte_memory.h>
#include <api.h>
#include <porting/libinit.h>
#include <rte_atomic.h>
#include "../light_common.h"
#include "../light_server_side.h"
#include "../user_callbacks.h"

#include <light_log.h>
#include <stdlib.h>
#include <fcntl.h>

#include <dlfcn.h>
#include <specific_includes/linux/netfilter.h>
#include <specific_includes/uapi/linux/netfilter_ipv4.h>
#include <sys/un.h>
#include <sys/epoll.h>
// #include <sys/time.h>

#ifndef RETURN_VALUE_SUPPORT
#define RETURN_VALUE_SUPPORT
#endif

uint64_t user_on_tx_opportunity_cycles = 0;
uint64_t user_on_tx_opportunity_called = 0;
uint64_t user_on_tx_opportunity_getbuff_called = 0;
uint64_t user_on_tx_opportunity_api_nothing_to_tx = 0;
uint64_t user_on_tx_opportunity_api_failed = 0;
uint64_t user_on_tx_opportunity_api_mbufs_sent = 0;
uint64_t user_on_tx_opportunity_socket_full = 0;
uint64_t user_on_tx_opportunity_cannot_get_buff = 0;
uint64_t user_on_rx_opportunity_called = 0;
uint64_t user_on_rx_opportunity_called_exhausted = 0;
uint64_t user_rx_mbufs = 0;

/************************************/
uint64_t user_kick_tx = 0;
uint64_t user_kick_rx = 0;
uint64_t user_on_tx_opportunity_cannot_send = 0;
uint64_t user_rx_ring_full = 0;
uint64_t user_on_tx_opportunity_socket_send_error = 0;
uint64_t user_client_app_accepted = 0;
uint64_t user_pending_accept = 0;
uint64_t user_sockets_closed = 0;
uint64_t user_sockets_shutdown = 0;
uint64_t user_flush_tx_cnt = 0;
uint64_t user_flush_rx_cnt = 0;
uint64_t g_last_time_transmitted = 0;

struct rte_ring *command_ring = NULL;
struct rte_ring *urgent_command_ring = NULL;

struct rte_mempool *free_connections_pool = NULL;
struct rte_ring *free_connections_ring = NULL;
struct rte_ring *free_clients_ring = NULL;
struct rte_ring *rx_mbufs_ring = NULL;
struct rte_mempool *free_command_pool = NULL;
struct rte_mempool *free_command_pools[MAX_CORES_NUM];

struct rte_mempool *free_return_value_pool = NULL;
struct rte_mempool *free_epoll_node_pool = NULL;
struct rte_mempool *free_socket_satelite_data_pool = NULL;
struct rte_mempool *free_event_info_pool = NULL;
struct rte_mempool *free_fd_mapping_storage_pool = NULL;
struct rte_mempool *free_monitor_pool = NULL;

//socket_satelite_data_t socket_satelite_data[LIGHT_CONNECTION_POOL_SIZE];
socket_satelite_data_t *socket_satelite_data;

light_socket_t *g_light_sockets = NULL;
//unsigned long app_pid = 0;

struct rte_ring *epolls_ring = NULL;
struct rte_mempool *epolls_pool = NULL;
light_epoll_t *g_light_epolls = NULL;


extern int proc_id;
extern unsigned num_procs;
extern enum rte_proc_type_t proc_type;

struct net_device *g_netdev[POLL_NIC_PORT_NUM];

//应用程序崩溃检测
int crash_listen_socket_fd;
char *crash_listen_socket_path="/tmp/crash_detect.socket";
struct sockaddr_un crash_listen_socket_un;

struct epoll_event crash_ev, crash_events[MAX_APPS];
int crash_epollfd;

void crash_detect() {
    int crash_nfds, crash_conn_sock;
    crash_nfds = epoll_wait(crash_epollfd, crash_events, MAX_APPS, 0);
    //if (crash_nfds == -1) {
    //        perror("epoll_pwait");
    //        exit(EXIT_FAILURE);
    //}

    for (int n = 0; n < crash_nfds; ++n) {
        if (crash_events[n].data.fd == crash_listen_socket_fd) {
            crash_conn_sock = accept(crash_listen_socket_fd,NULL, NULL);
            if (crash_conn_sock == -1) {
                printf("accept error");
                exit(1);
            }
            printf("fd=%d建立连接",crash_conn_sock);
            int flags = fcntl(crash_conn_sock, F_GETFL, 0);
            fcntl(crash_conn_sock, F_SETFL, flags | O_NONBLOCK);

            crash_ev.events = EPOLLIN;
            crash_ev.data.fd = crash_conn_sock;
            if (epoll_ctl(crash_epollfd, EPOLL_CTL_ADD, crash_conn_sock, &crash_ev) == -1) {
                printf("epoll_ctl: crash_conn_sock error");
                exit(1);
            }
        } else {
            printf("fd=%d关闭了",crash_events[n].data.fd);
            //todo 关闭这个进程的socket

            //最后关闭掉unix域socket
            epoll_ctl(crash_epollfd, EPOLL_CTL_DEL, crash_events[n].data.fd, NULL);
            close(crash_events[n].data.fd);
        }
    }
}

void crash_detect_init()
{
    if ((crash_listen_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        printf("crash_listen_socket_fd<0\n");
        exit(1);
    }
    memset(&crash_listen_socket_un, 0, sizeof(crash_listen_socket_un));
    crash_listen_socket_un.sun_family = AF_UNIX;
    strncpy(crash_listen_socket_un.sun_path, crash_listen_socket_path, sizeof(crash_listen_socket_un.sun_path) - 1);
    unlink(crash_listen_socket_path);

    if (bind(crash_listen_socket_fd, (struct sockaddr *) &crash_listen_socket_un, sizeof(crash_listen_socket_un)) < 0) {
        printf("bind error\n");
        exit(1);
    }
    printf("UNIX Domain Socket bound\n");

    if (listen(crash_listen_socket_fd, 100) < 0) {
        printf("listen error");
        exit(1);
    }

    //设置非阻塞
    int flags = fcntl(crash_listen_socket_fd, F_GETFL, 0);
    fcntl(crash_listen_socket_fd, F_SETFL, flags | O_NONBLOCK);

    crash_epollfd = epoll_create(MAX_APPS);
    if (crash_epollfd == -1) {
        printf("epoll_create failed");
        exit(1);
    }
    printf("crash_epoll_create\n");

    crash_ev.events = EPOLLIN;
    crash_ev.data.fd = crash_listen_socket_fd;
    if (epoll_ctl(crash_epollfd, EPOLL_CTL_ADD, crash_listen_socket_fd, &crash_ev) == -1) {
        printf("epoll_ctl: crash_listen_socket_fd fail");
        exit(1);
    }
    printf("epoll_ctl ok\n");

}

void *libc_handle = NULL;
int (*real_close) (int fd);

int store_original_api()    // use dlopen and dlsym to store the original api
{
    libc_handle = dlopen("libc.so.6", RTLD_LAZY);
    if (!libc_handle)
    {
        printf("dlopen fail\n");
        return -1;
    }
    real_close = dlsym(libc_handle, "close");
}

TAILQ_HEAD(buffers_available_notification_socket_list_head, socket) buffers_available_notification_socket_list_head;

struct light_clients light_clients[LIGHT_CLIENTS_POOL_SIZE];
TAILQ_HEAD(light_clients_list_head, light_clients) light_clients_list_head;

int get_all_devices(unsigned char *buf);

int get_all_addresses(unsigned char *buf);

void on_iface_up(char *name)
{
}

void on_new_addr(char *iface_name, unsigned int ipaddr, unsigned int mask)
{
}

static inline void user_increment_socket_tx_space(rte_atomic32_t *tx_space,int count) {
    rte_atomic32_add(tx_space,count);
}

static inline void user_set_socket_tx_space(rte_atomic32_t *tx_space,int count) {
    rte_atomic32_set(tx_space, count);
}

static inline void user_decrement_socket_occupied_mbuf_num(rte_atomic16_t *occupied_mbuf_num, int count) {
    rte_atomic16_sub(occupied_mbuf_num, count);
}

static void on_client_connect(int client_idx)
{
    struct rte_mbuf *buffer = get_buffer();
    if (!buffer)
    {
        return;
    }
    unsigned char *data = rte_pktmbuf_mtod(buffer, unsigned char *);
    *data = LIGHT_NEW_IFACES;
    data++;
    rte_pktmbuf_data_len(buffer) = get_all_devices(data) + 1;
    if (rte_ring_enqueue(light_clients[client_idx].client_ring, (void *)buffer) != 0) {
        printf("Error: rte_ring_enqueue(light_clients[client_idx].client_ring, (void *)buffer) != 0\n");
    }
    buffer = get_buffer();
    if (!buffer)
    {
        return;
    }
    data = rte_pktmbuf_mtod(buffer, unsigned char *);
    *data = LIGHT_NEW_ADDRESSES;
    data++;
    rte_pktmbuf_data_len(buffer) = get_all_addresses(data) + 1;
    if (rte_ring_enqueue(light_clients[client_idx].client_ring, (void *)buffer) != 0) {
        printf("Error: rte_ring_enqueue(light_clients[client_idx].client_ring, (void *)buffer) != 0\n");
    }
    buffer = get_buffer();
    if (!buffer)
    {
        return;
    }
    data = rte_pktmbuf_mtod(buffer, unsigned char *);
    *data = LIGHT_END_OF_RECORD;
    if (rte_ring_enqueue(light_clients[client_idx].client_ring, (void *)buffer) != 0) {
        printf("Error: rte_ring_enqueue(light_clients[client_idx].client_ring, (void *)buffer) != 0\n");
    }
}

void user_transmitted_callback(struct rte_mbuf *mbuf, struct socket *sock)
{
    int last = /*(rte_mbuf_refcnt_read(mbuf) == 1)*/1;
    if ((sock) && (last))
    {
        socket_satelite_data_t *socket_satelite_data = get_user_data(sock);
        if (socket_satelite_data)
        {
            // printf(" In user_transmitted_callback, socket_satelite_data->ringset_idx = %d\n", socket_satelite_data->ringset_idx);
            user_increment_socket_tx_space(&g_light_sockets[socket_satelite_data->ringset_idx].tx_space, rte_pktmbuf_data_len(mbuf));
            #ifdef BUFFER_MANAGE
            user_decrement_socket_occupied_mbuf_num(&g_light_sockets[socket_satelite_data->ringset_idx].occupied_mbuf_num, 1);
            #endif
        } else {
            light_log(LIGHT_LOG_ERR,"user_transmitted_callback get_user_data failure\n");
        }
    }
    rte_pktmbuf_free_seg(mbuf);
}

static inline int _process_command(light_cmd_t *cmd) {
    int ringset_idx;
    // light_cmd_t *cmd;
    struct rte_mbuf *mbuf;
    struct socket *sock;
    char *p;
    struct sockaddr_in addr;
    struct sockaddr_in *p_sockaddr;
    struct rtentry rtentry;
    int len;

    #ifdef COMMAND_TIME 
    struct timeval tv1, tv2;
    long time;
    static int socket_close_num;
    static long socket_close_time_total;
    float socket_close_time_avg;

    static int set_socket_num;
    static long set_socket_time_total;
    float set_socket_time_avg;

    static int tx_kick_num;
    static long tx_kick_time_total;
    float tx_kick_time_avg;

    static int rx_kick_num;
    static long rx_kick_time_total;
    float rx_kick_time_avg;
    #endif

    // cmd = light_dequeue_command_buf();
    if (!cmd)
        return -1;
    switch (cmd->cmd)
    {
    case LIGHT_ADD_ARP_COMMAND:
    {
        light_log(LIGHT_LOG_DEBUG, "cmd add arp port num:%d\n", cmd->u.add_arp.port_number);
        process_arp_command(cmd->u.add_arp.port_number, cmd->u.add_arp.sip, cmd->u.add_arp.sha, cmd->u.add_arp.creat, cmd->u.add_arp.state, cmd->u.add_arp.override);
        break;
    }

    case LIGHT_EPOLL_CREATE_COMMAND:
    {
        light_log(LIGHT_LOG_DEBUG, "cmd epoll create idx=%d, pid=%d\n", cmd->u.epoll_create.epoll_idx, cmd->u.epoll_create.pid);
        char fifo_name[1024];
        sprintf(fifo_name, "/tmp/epoll_fifo_%d%d%d", cmd->u.epoll_create.epoll_idx, cmd->u.epoll_create.pid, proc_id);
        light_epoll_t *pEpoll = &g_light_epolls[cmd->u.epoll_create.epoll_idx];
        if (unlikely((pEpoll->fifo_write_fds[proc_id] = open(fifo_name, O_RDWR | O_NONBLOCK)) < 0))
        {
            light_log(LIGHT_LOG_ERR, "Fail to open fifo %s %d\n", __FILE__, __LINE__);  // 加返回值的时候注意回收epoll
            return -1;
        }
        break;
    }

    case LIGHT_EPOLL_CLOSE_COMMAND:
    {
        light_log(LIGHT_LOG_DEBUG, "cmd epoll close ringset_idx:%u epoll_idx:%d\n", cmd->ringset_idx, cmd->u.epoll_create.epoll_idx);
        light_epoll_t *pEpoll = &g_light_epolls[cmd->u.epoll_close.epoll_idx];
        return real_close(pEpoll->fifo_write_fds[proc_id]);
        break;
    }

    case LIGHT_OPEN_SOCKET_COMMAND:
        light_log(LIGHT_LOG_DEBUG, "cmd open_sock ringset_idx: %u\n", cmd->ringset_idx);
        sock = app_glue_create_socket(cmd->u.open_sock.family, cmd->u.open_sock.type);
        if (likely(sock))
        {
            light_log(LIGHT_LOG_DEBUG, "setting user data in open socket cmd\n");
            socket_satelite_data[cmd->ringset_idx].ringset_idx = cmd->ringset_idx;
            socket_satelite_data[cmd->ringset_idx].parent_idx = cmd->parent_idx;
            socket_satelite_data[cmd->ringset_idx].apppid = cmd->u.open_sock.pid;
            app_glue_set_user_data(sock, (void *)&socket_satelite_data[cmd->ringset_idx]);
            socket_satelite_data[cmd->ringset_idx].socket = sock;
            user_set_socket_tx_space(&g_light_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].tx_space, sk_stream_wspace(sock->sk));

            #ifdef RETURN_VALUE_SUPPORT
            cmd->res->return_value = 0;
            #endif
        }
        #ifdef RETURN_VALUE_SUPPORT
        else
        {
            cmd->res->return_value = -1;
        }
        rte_atomic16_set( &(cmd->res->ready_signal), 1);
        #endif
        break;

    case LIGHT_SOCKET_CONNECT_BIND_COMMAND:
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            if (likely(cmd->u.socket_connect_bind.is_connect))
            {
                light_log(LIGHT_LOG_DEBUG, "connect %u\n", cmd->ringset_idx);
                int rv = app_glue_v4_connect(socket_satelite_data[cmd->ringset_idx].socket,
                                             cmd->u.socket_connect_bind.ipaddr,
                                             cmd->u.socket_connect_bind.port);

                //rv not zero, could be a failure non-blocking connect, if so, rv = -EINPROGRESS
                if (unlikely(rv))
                {
                    light_log(LIGHT_LOG_ERR, "failed to connect socket\n");
                    #ifdef RETURN_VALUE_SUPPORT
                    cmd->res->return_value = -1;
                    cmd->res->return_errno = -rv;
                    #endif
                }
                else
                {
                    light_log(LIGHT_LOG_DEBUG, "socket connected\n");
                    len = sizeof(addr);
                    inet_getname(socket_satelite_data[cmd->ringset_idx].socket, &addr, &len, 0);
                    g_light_sockets[cmd->ringset_idx].local_ipaddr = addr.sin_addr.s_addr;
                    g_light_sockets[cmd->ringset_idx].local_port = addr.sin_port;
                    #ifdef RETURN_VALUE_SUPPORT
                    cmd->res->return_value = 0;
                    cmd->res->return_errno = -rv;
                    #endif
                }
            }
            else
            {
                light_log(LIGHT_LOG_DEBUG, "cmd bind ringset_idx: %u\n", cmd->ringset_idx);
                int rv = app_glue_v4_bind(socket_satelite_data[cmd->ringset_idx].socket,
                                          cmd->u.socket_connect_bind.ipaddr,
                                          cmd->u.socket_connect_bind.port);
                if (unlikely(rv))
                {
                    light_log(LIGHT_LOG_ERR, "cannot bind \n");
                    #ifdef RETURN_VALUE_SUPPORT
                    cmd->res->return_value = -1;
                    cmd->res->return_errno = -rv;
                    #endif
                }
                else
                {
                    #ifdef RETURN_VALUE_SUPPORT
                    cmd->res->return_value = 0;
                    cmd->res->return_errno = -rv;
                    #endif
                }
            }
        }
        else
        {
            light_log(LIGHT_LOG_ERR, "no socket to invoke command!!!\n");
            #ifdef RETURN_VALUE_SUPPORT
            cmd->res->return_value = -1;   // no socket
            #endif
        }
        #ifdef RETURN_VALUE_SUPPORT
        rte_atomic16_set( &(cmd->res->ready_signal), 1);
        #endif
        break;

    case LIGHT_LISTEN_SOCKET_COMMAND:
        light_log(LIGHT_LOG_DEBUG, "cmd listen %u\n", cmd->ringset_idx);
        //By ccj: we add listen backlog
        #ifndef CCJ_API

        if (unlikely(app_glue_v4_listen(socket_satelite_data[cmd->ringset_idx].socket)))
        {
            light_log(LIGHT_LOG_ERR, "failed listening\n");
        }

        #else
        if (unlikely(app_glue_v4_listen(socket_satelite_data[cmd->ringset_idx].socket, cmd->u.listen_backlog.server_backlog)))
        {
            light_log(LIGHT_LOG_ERR, "failed listening\n");
        }
        #endif
        break;

    case LIGHT_SOCKET_CLOSE_COMMAND:
        #ifdef COMMAND_TIME
        gettimeofday(&tv1, NULL);
        #endif

        light_log(LIGHT_LOG_DEBUG, "cmd socket close %u\n", cmd->ringset_idx);
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            user_flush_rx_tx((struct socket *)socket_satelite_data[cmd->ringset_idx].socket);
            app_glue_close_socket((struct socket *)socket_satelite_data[cmd->ringset_idx].socket);
            socket_satelite_data[cmd->ringset_idx].socket = NULL;
            socket_satelite_data[cmd->ringset_idx].ringset_idx = -1;
            socket_satelite_data[cmd->ringset_idx].parent_idx = -1;
            light_free_socket(cmd->ringset_idx);
            user_sockets_closed++;
        }

        #ifdef COMMAND_TIME
        gettimeofday(&tv2, NULL);
        socket_close_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        socket_close_num++;
        if (unlikely(socket_close_num == 10000)) {
            socket_close_time_avg = (float)socket_close_time_total / socket_close_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("LIGHT_SOCKET_CLOSE_COMMAND, socket_close_time_avg = %f us\n", socket_close_time_avg);
            socket_close_time_total = 0;
            socket_close_num = 0;
        }
        #endif
        
        break;

    case LIGHT_SOCKET_TX_KICK_COMMAND:
        #ifdef COMMAND_TIME
        gettimeofday(&tv1, NULL);
        #endif

        light_log(LIGHT_LOG_DEBUG, "cmd socket tx kick ringset_idx: %u\n", cmd->ringset_idx);
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            user_kick_tx++;
            user_on_transmission_opportunity(socket_satelite_data[cmd->ringset_idx].socket, 1);
            light_log(LIGHT_LOG_DEBUG, "real tx space %d\n", sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk));
        }

        #ifdef COMMAND_TIME
        gettimeofday(&tv2, NULL);
        tx_kick_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        tx_kick_num++;
        if (unlikely(tx_kick_num == 10000)) {
            tx_kick_time_avg = (float)tx_kick_time_total / tx_kick_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("LIGHT_SOCKET_TX_KICK_COMMAND, tx_kick_time_avg = %f us\n", tx_kick_time_avg);
            tx_kick_time_total = 0;
            tx_kick_num = 0;
        }
        #endif

        break;

    case LIGHT_SOCKET_RX_KICK_COMMAND:
        #ifdef COMMAND_TIME
        gettimeofday(&tv1, NULL);
        #endif

        light_log(LIGHT_LOG_DEBUG, "cmd socket rx kick ringset_idx: %u\n", cmd->ringset_idx);
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            user_kick_rx++;
            user_data_available_cbk(socket_satelite_data[cmd->ringset_idx].socket);
        }

        #ifdef COMMAND_TIME
        gettimeofday(&tv2, NULL);
        rx_kick_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        rx_kick_num++;
        if (unlikely(rx_kick_num == 10000)) {
            rx_kick_time_avg = (float)rx_kick_time_total / rx_kick_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("LIGHT_SOCKET_RX_KICK_COMMAND, rx_kick_time_avg = %f us\n", rx_kick_time_avg);
            rx_kick_time_total = 0;
            rx_kick_num = 0;
        }
        #endif

        break;

    case LIGHT_SET_SOCKET_RING_COMMAND:
        #ifdef COMMAND_TIME
        gettimeofday(&tv1, NULL);
        #endif

        light_log(LIGHT_LOG_DEBUG,"cmd set socket ring ringset_idx: %u\n",cmd->ringset_idx);
        socket_satelite_data[cmd->ringset_idx].ringset_idx = cmd->ringset_idx;
        if (unlikely(cmd->parent_idx != -1)) {
            // printf("LIGHT_SET_SOCKET_RING_COMMAND: cmd->parent_idx != -1\n");
            socket_satelite_data[cmd->ringset_idx].parent_idx = cmd->parent_idx;
        }
        socket_satelite_data[cmd->ringset_idx].apppid = cmd->u.set_socket_ring.pid;
        app_glue_set_user_data(cmd->u.set_socket_ring.socket_descr, &socket_satelite_data[cmd->ringset_idx]);
        socket_satelite_data[cmd->ringset_idx].socket = cmd->u.set_socket_ring.socket_descr;
        light_log(LIGHT_LOG_DEBUG,"setting tx space: %d connidx %d\n",sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk),g_light_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].connection_idx);
        user_set_socket_tx_space(&g_light_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].tx_space, sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk));
        user_data_available_cbk(socket_satelite_data[cmd->ringset_idx].socket);
        user_client_app_accepted++;

        #ifdef COMMAND_TIME
        gettimeofday(&tv2, NULL);
        set_socket_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        set_socket_num++;
        if (unlikely(set_socket_num == 10000)) {
            set_socket_time_avg = (float)set_socket_time_total / set_socket_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("LIGHT_SET_SOCKET_RING_COMMAND, set_socket_time_avg = %f us\n", set_socket_time_avg);
            set_socket_time_total = 0;
            set_socket_num = 0;
        }
        #endif

        break;

    case LIGHT_SOCKET_TX_POOL_EMPTY_COMMAND:
        light_log(LIGHT_LOG_DEBUG,"cmd socket tx pool empty ringset_idx: %u\n",cmd->ringset_idx);
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            if (likely(!socket_satelite_data[cmd->ringset_idx].socket->buffers_available_notification_queue_present))
            {
                TAILQ_INSERT_TAIL(&buffers_available_notification_socket_list_head, socket_satelite_data[cmd->ringset_idx].socket, buffers_available_notification_queue_entry);
                socket_satelite_data[cmd->ringset_idx].socket->buffers_available_notification_queue_present = 1;
                if (socket_satelite_data[cmd->ringset_idx].socket->type == SOCK_DGRAM)
                    user_set_socket_tx_space(&g_light_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].tx_space, sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk));
            }
        }
        break;

    case LIGHT_ROUTE_ADD_COMMAND:
        memset((void *)&rtentry, 0, sizeof(rtentry));
        rtentry.rt_metric = cmd->u.route.metric;
        rtentry.rt_flags = RTF_UP | RTF_GATEWAY;
        p_sockaddr = (struct sockaddr_in *)&rtentry.rt_dst;
        p_sockaddr->sin_family = AF_INET;
        p_sockaddr->sin_addr.s_addr = cmd->u.route.dest_ipaddr;
        p_sockaddr = (struct sockaddr_in *)&rtentry.rt_gateway;
        p_sockaddr->sin_family = AF_INET;
        p_sockaddr->sin_addr.s_addr = cmd->u.route.next_hop;
        p_sockaddr = (struct sockaddr_in *)&rtentry.rt_genmask;
        p_sockaddr->sin_family = AF_INET;
        p_sockaddr->sin_addr.s_addr = cmd->u.route.dest_mask;
        if (unlikely(ip_rt_ioctl(&init_net, SIOCADDRT, &rtentry)))
        {
            light_log(LIGHT_LOG_ERR, "CANNOT ADD ROUTE ENTRY %x %x %x\n",
                             ((struct sockaddr_in *)&rtentry.rt_dst)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_gateway)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_genmask)->sin_addr.s_addr);
        }
        else
        {
            light_log(LIGHT_LOG_INFO, "ROUTE ENTRY %x %x %x is added\n",
                             ((struct sockaddr_in *)&rtentry.rt_dst)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_gateway)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_genmask)->sin_addr.s_addr);
        }
        break;
    case LIGHT_ROUTE_DEL_COMMAND:
        memset((void *)&rtentry, 0, sizeof(rtentry));
        p_sockaddr = (struct sockaddr_in *)&rtentry.rt_dst;
        p_sockaddr->sin_family = AF_INET;
        p_sockaddr->sin_addr.s_addr = cmd->u.route.dest_ipaddr;
        p_sockaddr = (struct sockaddr_in *)&rtentry.rt_gateway;
        p_sockaddr->sin_family = AF_INET;
        p_sockaddr->sin_addr.s_addr = cmd->u.route.next_hop;
        p_sockaddr = (struct sockaddr_in *)&rtentry.rt_genmask;
        p_sockaddr->sin_family = AF_INET;
        p_sockaddr->sin_addr.s_addr = cmd->u.route.dest_mask;
        if (unlikely(ip_rt_ioctl(&init_net, SIOCDELRT, &rtentry)))
        {
            light_log(LIGHT_LOG_ERR, "CANNOT DELETE ROUTE ENTRY %x %x %x\n",
                             ((struct sockaddr_in *)&rtentry.rt_dst)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_gateway)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_genmask)->sin_addr.s_addr);
        }
        else
        {
            light_log(LIGHT_LOG_INFO, "ROUTE ENTRY %x %x %x is deleted\n",
                             ((struct sockaddr_in *)&rtentry.rt_dst)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_gateway)->sin_addr.s_addr,
                             ((struct sockaddr_in *)&rtentry.rt_genmask)->sin_addr.s_addr);
        }
        break;
    case LIGHT_CONNECT_CLIENT:
        light_log(LIGHT_LOG_DEBUG,"cmd connect client ringset_idx: %u\n",cmd->ringset_idx);
        if (unlikely(cmd->ringset_idx >= LIGHT_CLIENTS_POOL_SIZE))
        {
            break;
        }
        if (likely(!light_clients[cmd->ringset_idx].is_busy))
        {
            TAILQ_INSERT_TAIL(&light_clients_list_head, &light_clients[cmd->ringset_idx], queue_entry);
            light_clients[cmd->ringset_idx].is_busy = 1;
            on_client_connect(cmd->ringset_idx);
        }
        break;
    case LIGHT_DISCONNECT_CLIENT:
        light_log(LIGHT_LOG_DEBUG,"cmd disconnect client ringset_idx: %u\n",cmd->ringset_idx);
        if (unlikely(cmd->ringset_idx >= LIGHT_CLIENTS_POOL_SIZE))
        {
            break;
        }
        if (likely(light_clients[cmd->ringset_idx].is_busy))
        {
            TAILQ_REMOVE(&light_clients_list_head, &light_clients[cmd->ringset_idx], queue_entry);
            light_clients[cmd->ringset_idx].is_busy = 0;
        }
        break;
    case LIGHT_SETSOCKOPT_COMMAND:
        light_log(LIGHT_LOG_DEBUG,"cmd setsocketopt ringset_idx: %u\n",cmd->ringset_idx);
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            sock_setsockopt(socket_satelite_data[cmd->ringset_idx].socket, cmd->u.setsockopt.level, cmd->u.setsockopt.optname, cmd->u.setsockopt.optval, cmd->u.setsockopt.optlen);
            user_set_socket_tx_space(&g_light_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].tx_space, sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk));
//         printf("tx_space %d\n",&g_light_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].tx_space);
        }
        break;
    case LIGHT_SOCKET_SHUTDOWN_COMMAND:
        light_log(LIGHT_LOG_DEBUG,"cmd shutdown ringset_idx: %u\n",cmd->ringset_idx);
        if (likely(socket_satelite_data[cmd->ringset_idx].socket))
        {
            if (cmd->u.socket_shutdown.how == 0) {
                printf("LIGHT_SOCKET_SHUTDOWN_COMMAND: cmd->u.socket_shutdown.how == 0\n");
                user_flush_rx((struct socket *)socket_satelite_data[cmd->ringset_idx].socket);
            }
            if (cmd->u.socket_shutdown.how == 1) {
                printf("LIGHT_SOCKET_SHUTDOWN_COMMAND: cmd->u.socket_shutdown.how == 1\n");
                user_flush_tx((struct socket *)socket_satelite_data[cmd->ringset_idx].socket);
            }
            if (cmd->u.socket_shutdown.how == 2)
            {
                printf("LIGHT_SOCKET_SHUTDOWN_COMMAND: cmd->u.socket_shutdown.how == 2\n");
                user_flush_rx_tx((struct socket *)socket_satelite_data[cmd->ringset_idx].socket);
                app_glue_close_socket((struct socket *)socket_satelite_data[cmd->ringset_idx].socket);
                socket_satelite_data[cmd->ringset_idx].socket = NULL;
                socket_satelite_data[cmd->ringset_idx].ringset_idx = -1;
                socket_satelite_data[cmd->ringset_idx].parent_idx = -1;
                light_free_socket(cmd->ringset_idx);
            }

            inet_shutdown(socket_satelite_data[cmd->ringset_idx].socket, cmd->u.socket_shutdown.how);
            user_sockets_shutdown++;
        }
        break;
    case LIGHT_SOCKET_DECLINE_COMMAND:
        light_log(LIGHT_LOG_DEBUG,"cmd socket decline ringset_idx: %u\n",cmd->ringset_idx);
        user_flush_rx_tx((struct socket *)cmd->u.socket_decline.socket_descr);
        app_glue_close_socket((struct socket *)cmd->u.socket_decline.socket_descr);
        user_sockets_closed++;
        break;
    default:
        light_log(LIGHT_LOG_ERR, "unknown cmd %d\n", cmd->cmd);
        break;
    }
    _light_free_command_buf(cmd);
    return 0;
}


static inline int _process_urgent_command(){
    int ringset_idx;
    light_cmd_t *cmd;
    struct rte_mbuf *mbuf;
    struct socket *sock;
    char *p;
    struct sockaddr_in addr;
    struct sockaddr_in *p_sockaddr;
    struct rtentry rtentry;
    int len;
    cmd = light_dequeue_urgent_command_buf();
    if (!cmd)
        return -1;
    switch (cmd->cmd)
    {
    case LIGHT_SOCKET_TX_KICK_COMMAND:
        light_log(LIGHT_LOG_DEBUG, "cmd socket tx kick ringset_idx: %u\n", cmd->ringset_idx);
        if (socket_satelite_data[cmd->ringset_idx].socket)
        {
            user_kick_tx++;
            user_on_transmission_opportunity(socket_satelite_data[cmd->ringset_idx].socket, 1);
            light_log(LIGHT_LOG_DEBUG, "real tx space %d\n", sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk));
        }
        break;

    case LIGHT_SOCKET_RX_KICK_COMMAND:
        light_log(LIGHT_LOG_DEBUG, "cmd socket rx kick ringset_idx: %u\n", cmd->ringset_idx);
        if (socket_satelite_data[cmd->ringset_idx].socket)
        {
            user_kick_rx++;
            user_data_available_cbk(socket_satelite_data[cmd->ringset_idx].socket);
        }
        break;

    
    default:
        printf("Error: unknown type of urgent cmd\n");
        break;
    }
    _light_free_command_buf(cmd);
    return 0;
}


static inline void process_commands()
{
    // if (_process_urgent_command() != 0){ // No urgent cmd.
    //     _process_command();
    // }

    int i, cmd_n;

    #ifdef PROCESS_ALL_COMMAND // process all commands.
    light_cmd_t *cmd;
    while (1){
        cmd = light_dequeue_command_buf();
        if (cmd == NULL)
            break;
        _process_command(cmd);
    }
    #endif

    #ifdef PROCESS_N_COMMAND // process n commands.
    light_cmd_t *cmd[MAX_CMD_DEQUEUE_NUM];
    cmd_n = MIN_2(rte_ring_count(command_ring), 20);
    light_dequeue_command_buf_bulk(cmd, cmd_n);
    for (i = 0; i < cmd_n; i++) {
        if (unlikely(cmd[i] == NULL))
            break;
        _process_command(cmd[i]);
    }

    #ifdef CMD_COUNT
    static int cmd_times;
    static long cmd_total;
    float cmd_avg;
    cmd_times ++;
    cmd_total += cmd_n;
    if (unlikely(cmd_times == 10000)) {
        cmd_avg = (float)cmd_total / cmd_times;
        printf("cmd_avg = %f\n", cmd_avg);
        cmd_times = 0;
        cmd_total = 0;
    }
    #endif // #ifdef CMD_COUNT

    #endif // #ifdef PROCESS_N_COMMAND


    #ifdef PROCESS_ONE_COMMAND
    light_cmd_t *cmd;
    cmd = light_dequeue_command_buf();
    _process_command(cmd);
    #endif
    // _process_command();
}

void light_main_loop()
{
    // printf("Entering main loop\n");
    light_log(LIGHT_LOG_INFO, "In light_main_loop()\n");
    printf("In light_main_loop(), Please Wait... ...\n");

    int i;
    struct rte_mbuf *mbuf;
    uint8_t ports_to_poll[1] = { 0 };
    int drv_poll_interval = get_max_drv_poll_interval_in_micros(0);

    #ifdef LOOP_TIME
    struct timeval tv1, tv2;
    long time;
    static int process_commands_num;
    static long process_commands_time_total;
    float process_commands_time_avg;

    static int app_glue_periodic_num;
    static long app_glue_periodic_time_total;
    float app_glue_periodic_time_avg;

    static int process_buffers_available_notification_socket_list_num;
    static long process_buffers_available_notification_socket_list_time_total;
    float process_buffers_available_notification_socket_list_time_avg;
    #endif

    light_log(LIGHT_LOG_INFO, "max_drv_poll_interval_in_micros: %d MAX_PKT_BURST: %d\n", drv_poll_interval, MAX_PKT_BURST);
    printf("drv_poll_interval = %d, MAX_PKT_BURST = %d\n", drv_poll_interval, MAX_PKT_BURST);
    app_glue_init_poll_intervals(drv_poll_interval / (2 * MAX_PKT_BURST),  // driver_poll_interval
                                 100000,  //timer_poll_interval
                                 drv_poll_interval / (10 * MAX_PKT_BURST),  // tx_ready_sockets_poll_interval
                                 drv_poll_interval / (10 * MAX_PKT_BURST),  // rx_ready_sockets_poll_interval
                                 1000000 * 3  // crash_detect_interval 3s
                                 );

    light_service_api_init(COMMAND_POOL_SIZE, RX_RING_SIZE, TX_RING_SIZE);
    TAILQ_INIT(&buffers_available_notification_socket_list_head);
    TAILQ_INIT(&light_clients_list_head);
    // init_systick(rte_lcore_id());
    store_original_api();    // dlink for real_close api

    for (i = 0; i < POLL_NIC_PORT_NUM; i ++) {
        g_netdev[i] = (struct net_device *)get_dpdk_dev_by_port_num(i);
    }

    light_log(LIGHT_LOG_INFO, "light service initialized\n");

    printf("sizeof(socket_satelite_data_t) = %d\n", sizeof(socket_satelite_data_t));
    printf("sizeof(pid_t) = %d\n", sizeof(pid_t));
    printf("*** LIGHT_CONNECTION_POOL_SIZE = %d ***\n", LIGHT_CONNECTION_POOL_SIZE);

    if (proc_type == RTE_PROC_PRIMARY)
    {
        crash_detect_init();  // 崩溃应用检测
    }

    printf("Entering while (1)\n The First Core of Light is ready and you can run other Light Cores or APPs (such as Nginx).\n");

    while (1)
    {
        #ifdef LOOP_TIME
        gettimeofday(&tv1, NULL);
        #endif

        process_commands();

        #ifdef LOOP_TIME
        gettimeofday(&tv2, NULL);
        process_commands_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        process_commands_num++;
        if (unlikely(process_commands_num == 10000)) {
            process_commands_time_avg = (float)process_commands_time_total / process_commands_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("In light_main_loop(), process_commands_time_avg = %f us\n", process_commands_time_avg);
            process_commands_time_total = 0;
            process_commands_num = 0;
        }
        #endif


        #ifdef LOOP_TIME
        gettimeofday(&tv1, NULL);
        #endif

        app_glue_periodic(1, ports_to_poll, 1);

        #ifdef LOOP_TIME
        gettimeofday(&tv2, NULL);
        app_glue_periodic_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        app_glue_periodic_num++;
        if (unlikely(app_glue_periodic_num == 10000)) {
            app_glue_periodic_time_avg = (float)app_glue_periodic_time_total / app_glue_periodic_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("In light_main_loop(), app_glue_periodic_time_avg = %f us\n", app_glue_periodic_time_avg);
            app_glue_periodic_time_total = 0;
            app_glue_periodic_num = 0;
        }
        #endif


        #ifdef LOOP_TIME
        gettimeofday(&tv1, NULL);
        #endif

        while (!TAILQ_EMPTY(&buffers_available_notification_socket_list_head))
        {
            if (likely(get_buffer_count() > 0))
            {
                struct socket *sock = TAILQ_FIRST(&buffers_available_notification_socket_list_head);
                socket_satelite_data_t *socket_data = get_user_data(sock);
                if (unlikely(socket_data == NULL)) break;
                if (unlikely(socket_data->socket->type == SOCK_DGRAM)) {
                    user_set_socket_tx_space(&g_light_sockets[socket_data->ringset_idx].tx_space, sk_stream_wspace(socket_data->socket->sk));
                    // printf("socket_data->socket->type == SOCK_DGRAM\n");
                }
                //printf("%s %d %d %d %d\n",__FILE__,__LINE__,socket_data->ringset_idx,g_light_sockets[socket_data->ringset_idx].tx_space,sk_stream_wspace(socket_data->socket->sk));
                if (unlikely(!light_mark_writable(socket_data)))
                {
                    // printf("!light_mark_writable(socket_data)\n");
                    g_light_sockets[socket_data->ringset_idx].presented_in_chain = 0;  // ADD BY Shang Zhantong
                    sock->buffers_available_notification_queue_present = 0;
                    TAILQ_REMOVE(&buffers_available_notification_socket_list_head, sock, buffers_available_notification_queue_entry);

                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }

        #ifdef LOOP_TIME
        gettimeofday(&tv2, NULL);
        process_buffers_available_notification_socket_list_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
        process_buffers_available_notification_socket_list_num++;
        if (unlikely(process_buffers_available_notification_socket_list_num == 10000)) {
            process_buffers_available_notification_socket_list_time_avg = (float)process_buffers_available_notification_socket_list_time_total / process_buffers_available_notification_socket_list_num;
            // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
            printf("In light_main_loop(), process_buffers_available_notification_socket_list_time_avg = %f us\n", process_buffers_available_notification_socket_list_time_avg);
            process_buffers_available_notification_socket_list_time_total = 0;
            process_buffers_available_notification_socket_list_num = 0;
        }
        #endif

    }
}

/*this is called in non-data-path thread */
void print_user_stats()
{
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_called %"PRIu64"\n", user_on_tx_opportunity_called);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_api_nothing_to_tx %"PRIu64"user_on_tx_opportunity_socket_full %"PRIu64" \n",
                     user_on_tx_opportunity_api_nothing_to_tx, user_on_tx_opportunity_socket_full);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_cannot_send %"PRIu64"\n", user_on_tx_opportunity_cannot_send);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_socket_send_error %"PRIu64"\n", user_on_tx_opportunity_socket_send_error);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_cannot_get_buff %"PRIu64"\n", user_on_tx_opportunity_cannot_get_buff);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_getbuff_called %"PRIu64"\n", user_on_tx_opportunity_getbuff_called);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_api_failed %"PRIu64"\n", user_on_tx_opportunity_api_failed);
    light_log(LIGHT_LOG_INFO, "user_on_rx_opportunity_called %"PRIu64"\n", user_on_rx_opportunity_called);
    light_log(LIGHT_LOG_INFO, "user_on_rx_opportunity_called_exhausted %"PRIu64"\n", user_on_rx_opportunity_called_exhausted);
    light_log(LIGHT_LOG_INFO, "user_rx_mbufs %"PRIu64" user_rx_ring_full %"PRIu64"\n", user_rx_mbufs, user_rx_ring_full);
    light_log(LIGHT_LOG_INFO, "user_on_tx_opportunity_api_mbufs_sent %"PRIu64"\n", user_on_tx_opportunity_api_mbufs_sent);
    light_log(LIGHT_LOG_INFO, "user_client_app_accepted %"PRIu64" user_pending_accept %"PRIu64"\n", user_client_app_accepted, user_pending_accept);
    light_log(LIGHT_LOG_INFO, "user_sockets_closed %"PRIu64" user_sockets_shutdown %"PRIu64"\n",
                     user_sockets_closed, user_sockets_shutdown);
    light_log(LIGHT_LOG_INFO, "user_flush_rx_cnt %"PRIu64" user_flush_tx_cnt %"PRIu64"\n",
                     user_flush_rx_cnt, user_flush_tx_cnt);

}
