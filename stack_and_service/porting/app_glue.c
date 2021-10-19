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
#include <specific_includes/net/tcp.h>
#include <specific_includes/linux/err.h>
#include <specific_includes/linux/if_arp.h>
#include <specific_includes/linux/if_vlan.h>
#include <specific_includes/linux/ip.h>
#include <specific_includes/net/ip.h>
#include <specific_includes/linux/ipv6.h>
#include <specific_includes/linux/in.h>
#include <specific_includes/linux/inetdevice.h>
#include <specific_includes/linux/hashtable.h>
#include <specific_includes/linux/if_macvlan.h>
#include <specific_includes/linux/if_arp.h>
#include <specific_includes/dpdk_drv_iface.h>
#include <string.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <api.h>
#include <porting/libinit.h>
#include "../light_common.h"
#include "../light_server_side.h"
#include <user_callbacks.h>
#include <light_log.h>
#include <time.h>

TAILQ_HEAD(read_ready_socket_list_head, socket) read_ready_socket_list_head;
uint64_t read_sockets_queue_len = 0;
TAILQ_HEAD(closed_socket_list_head, socket) closed_socket_list_head;
TAILQ_HEAD(write_ready_socket_list_head, socket) write_ready_socket_list_head;
uint64_t write_sockets_queue_len = 0;
TAILQ_HEAD(accept_ready_socket_list_head, socket) accept_ready_socket_list_head;
uint64_t working_cycles_stat = 0;
uint64_t total_cycles_stat = 0;
uint64_t work_prev = 0;
uint64_t total_prev = 0;
uint64_t app_glue_sock_readable_called = 0;
uint64_t app_glue_sock_writable_called = 0;

extern struct net_device *g_netdev[POLL_NIC_PORT_NUM];

/*
 * This callback function is invoked when data arrives to socket.
 * It inserts the socket into a list of readable sockets
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock, len (dummy)
 * Returns: void
 *
 */
void app_glue_sock_readable(struct sock *sk, int len)
{
    //printf("in app_glue_sock_readable:read_sockets_queue_len=%d  read_queue_present=%d\n",
    //        read_sockets_queue_len,sk->sk_socket->read_queue_present);
    const struct tcp_sock *tp = tcp_sk(sk);

    if (!sk->sk_socket)
    {
        light_log(LIGHT_LOG_DEBUG, "app_glue_sock_readable: sk->sk_socket NULL");
        return;
    }
    //printf("sk_state=%d  socket_type=%d\n",sk->sk_state,sk->sk_socket->type);
    /*
     * updated by gjk_sem
     * 加入了一重判断条件，sk->sk_state!=TCP_CLOSE_WAIT
     * 当状态是TCP_CLOSE_WAIT，我们也得让他向下走，使得read_sockets_queue_len++，
     * 这样在process_rx_ring里面才会进一步调用user_data_available_cbk
     */
    if ((sk->sk_state != TCP_ESTABLISHED && sk->sk_state != TCP_CLOSE_WAIT) && (sk->sk_socket->type == SOCK_STREAM))
    {
        return;
    }
    if (sk->sk_socket->read_queue_present)
    {
        if (read_sockets_queue_len == 0)
        {
            light_log(LIGHT_LOG_ERR, "%s %d\n", __FILE__, __LINE__);
            exit(0);
        }
        return;
    }
    app_glue_sock_readable_called++;
    #ifdef CLOSE_DEBUG
    printf("In app_glue_sock_readable, sock_hold(sk)\n");
    #endif
    sock_hold(sk);
    sk->sk_socket->read_queue_present = 1;
    TAILQ_INSERT_TAIL(&read_ready_socket_list_head, sk->sk_socket, read_queue_entry);
    read_sockets_queue_len++;

    //printf("Over read_sockets_queue_len=%d  read_queue_present=%d\n",
    //    read_sockets_queue_len,sk->sk_socket->read_queue_present);
}
/*
 * This callback function is invoked when data canbe transmitted on socket.
 * It inserts the socket into a list of writable sockets
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock
 * Returns: void
 *
 */
void app_glue_sock_write_space(struct sock *sk)
{
    if (!sk->sk_socket)
    {
        light_log(LIGHT_LOG_DEBUG, "app_glue_sock_write_space: sk->sk_socket NULL");
        return;
    }
    if ((sk->sk_state != TCP_ESTABLISHED) && (sk->sk_socket->type == SOCK_STREAM))
    {
        return;
    }

    if (sk_stream_is_writeable(sk))
    {
        clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
        if (sk->sk_socket->write_queue_present)
        {
            light_log(LIGHT_LOG_INFO, "app_glue_sock_write_space: write_queue is processing now");
            return;
        }
        app_glue_sock_writable_called++;
        #ifdef CLOSE_DEBUG
        printf("In app_glue_sock_write_space, sock_hold(sk)\n");
        #endif
        sock_hold(sk);
        sk->sk_socket->write_queue_present = 1;
        TAILQ_INSERT_TAIL(&write_ready_socket_list_head, sk->sk_socket, write_queue_entry);
        write_sockets_queue_len++;
    }
}
/*
 * This callback function is invoked when an error occurs on socket.
 * It inserts the socket into a list of closable sockets
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock
 * Returns: void
 *
 */
void app_glue_sock_error_report(struct sock *sk)
{
    /**
     * Add by CCJ_API
     * add socket error
     */
//    printf("app glue error report\n");
    if (sk && sk->sk_socket)
    {
        if (sk->sk_socket->closed_queue_present)
        {
            return;
        }
        #ifdef CLOSE_DEBUG
        printf("In app_glue_sock_error_report, sock_hold(sk)\n");
        #endif
        sock_hold(sk);
        sk->sk_socket->closed_queue_present = 1;
        TAILQ_INSERT_TAIL(&closed_socket_list_head, sk->sk_socket, closed_queue_entry);
    }
}
/*
 * This callback function is invoked when a new connection can be accepted on socket.
 * It looks up the parent (listening) socket for the newly established connection
 * and inserts it into the accept queue
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock
 * Returns: void
 *
 */
static void app_glue_sock_wakeup(struct sock *sk)
{
    light_log(LIGHT_LOG_DEBUG, "app_glue_sock_wakeup function\n");
    struct sock *sock;
    struct tcp_sock *tp;
    tp = tcp_sk(sk);
    sock = __inet_lookup_listener(&init_net/*sk->sk_net*/,
                                  &tcp_hashinfo,
                                  sk->sk_daddr,
                                  sk->sk_dport/*__be16 sport*/,
                                  sk->sk_rcv_saddr,
                                  ntohs(tp->inet_conn.icsk_inet.inet_sport),//sk->sk_num/*const unsigned short hnum*/,
                                  sk->sk_bound_dev_if);
    if (sock && sk->sk_state == TCP_ESTABLISHED)
    {
        // printf("new connection arrive\n");
        if (sock->sk_socket->accept_queue_present)
        {
            // printf("present\n");
            return;
        }
        #ifdef CLOSE_DEBUG
        printf("In app_glue_sock_wakeup(1), sock_hold(sock), sock = %x, atomic_read(&sock->sk_refcnt) = %d\n", sock, atomic_read(&sock->sk_refcnt));
        #endif
        sock_hold(sock);
        sock->sk_socket->accept_queue_present = 1;
        TAILQ_INSERT_TAIL(&accept_ready_socket_list_head, sock->sk_socket, accept_queue_entry);
        /*
         * when sk_state change to TCP_CLOSE_WAIT or TCP_CLOSE, we need to notify api process
         */
    }
    else if ( (sk->sk_state == TCP_ESTABLISHED) && (sk->sk_socket->type == SOCK_STREAM) ) // connection established success
    {
        printf("In app_glue_sock_wakeup, (sk->sk_state == TCP_ESTABLISHED) && (sk->sk_socket->type == SOCK_STREAM)\n");
        socket_satelite_data_t *socket_satelite_data = sk->sk_user_data;
        if (socket_satelite_data != NULL)
        {
            int idx = socket_satelite_data->ringset_idx;
            rte_atomic16_set( &(g_light_sockets[idx].connect_close_signal), 2);
        }
        else
        {
            light_log(LIGHT_LOG_DEBUG, "sk_user_data is null\n");
        }
    }
    else if (sk && sk->sk_socket && (sk->sk_state == TCP_CLOSE_WAIT || sk->sk_state == TCP_CLOSE))
    {
        light_log(LIGHT_LOG_DEBUG, "close wait or close is notified\n");
        if (sk->sk_socket->closed_queue_present)
        {
            return;
        }
        #ifdef CLOSE_DEBUG
        printf("In app_glue_sock_wakeup(2), sock_hold(sk)\n");
        #endif
        sock_hold(sk);
        sk->sk_socket->closed_queue_present = 1;
        TAILQ_INSERT_TAIL(&closed_socket_list_head, sk->sk_socket, closed_queue_entry);
    }
    else
    {
        app_glue_sock_write_space(sk);
    }
    sock_reset_flag(sk, SOCK_USE_WRITE_QUEUE);
    sk->sk_data_ready = app_glue_sock_readable;
    sk->sk_write_space = app_glue_sock_write_space;
    sk->sk_error_report = app_glue_sock_error_report;
}

void *app_glue_create_socket(int family, int type)
{
    struct timeval tv;
    struct socket *sock = NULL;
    if (sock_create_kern(family, type, 0, &sock))
    {
        light_log(LIGHT_LOG_ERR, "cannot create socket %s %d\n", __FILE__, __LINE__);
        return NULL;
    }
    tv.tv_sec = -1;
    tv.tv_usec = 0;
    if (sock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)))
    {
        light_log(LIGHT_LOG_ERR, "%s %d cannot set notimeout option\n", __FILE__, __LINE__);
    }
    tv.tv_sec = -1;
    tv.tv_usec = 0;
    if (sock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)))
    {
        light_log(LIGHT_LOG_ERR, "%s %d cannot set notimeout option\n", __FILE__, __LINE__);
    }
    if (type != SOCK_STREAM)
    {
        if (sock->sk)
        {
            sock_reset_flag(sock->sk, SOCK_USE_WRITE_QUEUE);
            sock->sk->sk_data_ready = app_glue_sock_readable;
            sock->sk->sk_write_space = app_glue_sock_write_space;
            app_glue_sock_write_space(sock->sk);
        }
    }
    return sock;
}

int app_glue_v4_bind(struct socket *sock, unsigned int ipaddr, unsigned short port)
{
    struct sockaddr_in sin;

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = ipaddr;
    sin.sin_port = port;

    return kernel_bind(sock, (struct sockaddr *)&sin, sizeof(sin));
}

int app_glue_v4_connect(struct socket *sock, unsigned int ipaddr, unsigned short port)
{
    struct sockaddr_in sin;

    if (!sock->sk)
        return -1;
    struct inet_sock *inet = inet_sk(sock->sk);
    while (!inet->inet_num) //inet_num - Local port
    {
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = /*my_ip_addr*/0;
        sin.sin_port = htons(rand() & 0xffff);
        if (kernel_bind(sock, (struct sockaddr *)&sin, sizeof(sin)))
        {
            light_log(LIGHT_LOG_ERR, "cannot bind %s %d %d\n", __FILE__, __LINE__, sin.sin_port);
            continue;
        }
        break;
    }
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = ipaddr;
    sin.sin_port = port;

//    printf("dst port %d \n",port);
    if (sock->sk)
    {
        sock->sk->sk_state_change = app_glue_sock_wakeup;
    }
    //kernel_connect(sock, (struct sockaddr *)&sin,sizeof(sin), 0);
    int connect_res = kernel_connect(sock, (struct sockaddr *)&sin, sizeof(sin), 0);

    if (connect_res == -EINPROGRESS) //The socket socket is non-blocking and the connection could not be established immediately. 
        //You can determine when the connection is completely established with select; see Waiting for I/O. 
        //Another connect call on the same socket, before the connection is completely established, will fail with EALREADY.
    {
//        printf("after: STATE=%d   TCP_ESTABLISHED=%d\n",sock->sk->sk_state, TCP_ESTABLISHED);
        socket_satelite_data_t *socket_satelite_data = sock->sk->sk_user_data;
        int idx = socket_satelite_data->ringset_idx;
        rte_atomic16_set( &(g_light_sockets[idx].connect_close_signal), 1);
    }
    else if (connect_res == 0)
    {
        socket_satelite_data_t *socket_satelite_data = sock->sk->sk_user_data;
        int idx = socket_satelite_data->ringset_idx;
        rte_atomic16_set( &(g_light_sockets[idx].connect_close_signal), 2);
    }
    else
    {
        socket_satelite_data_t *socket_satelite_data = sock->sk->sk_user_data;
        int idx = socket_satelite_data->ringset_idx;
        rte_atomic16_set(&(g_light_sockets[idx].connect_close_signal), 4);
    }
    return connect_res;
}

#ifndef CCJ_API

int app_glue_v4_listen(struct socket *sock)
{
    if (sock->sk)
    {
        sock->sk->sk_state_change = app_glue_sock_wakeup;
    }
    else
    {
        light_log(LIGHT_LOG_ERR, "FATAL in glue listen sock->sk in null %d\n",__LINE__);
        exit(0);
    }
    if (kernel_listen(sock, 32000))
    {
        light_log(LIGHT_LOG_ERR, "cannot listen %s %d\n", __FILE__, __LINE__);
        return -1;
    }
    return 0;
}

#else

int app_glue_v4_listen(struct socket *sock, int server_backlog)
{
#ifdef APP_GLUE_DEBUG
    printf("start app_glue_v4_listen\n");
#endif
    if (sock->sk)
    {
        sock->sk->sk_state_change = app_glue_sock_wakeup;
    }
    else
    {
        light_log(LIGHT_LOG_ERR, "FATAL in glue listen sock->sk in null %d\n",__LINE__);
        exit(0);
    }
    //By ccj: We add server_backlog, originally is 32000
    if (kernel_listen(sock, server_backlog))
    {
        light_log(LIGHT_LOG_ERR, "cannot listen %s %d\n", __FILE__, __LINE__);
        return -1;
    }
    return 0;
}

#endif

/*
 * This function polls the driver for the received packets. Called from app_glue_periodic
 * Paramters: ethernet port number.
 * Returns: None
 *
 */
static inline void app_glue_poll(int port_num)
{
    // struct net_device *netdev = (struct net_device *)get_dpdk_dev_by_port_num(port_num);

    if (unlikely(!g_netdev[port_num]))
    {
        light_log(LIGHT_LOG_ERR, "Cannot get netdev %s %d\n", __FILE__, __LINE__);
        return;
    }
    g_netdev[port_num]->netdev_ops->ndo_poll_controller(g_netdev[port_num]);
}

/*
 * This function must be called before app_glue_periodic is called for the first time.
 * It initializes the readable, writable and acceptable (listeners) lists
 * Paramters: None
 * Returns: None
 *
 */
void app_glue_init()
{
    TAILQ_INIT(&read_ready_socket_list_head);
    TAILQ_INIT(&write_ready_socket_list_head);
    TAILQ_INIT(&accept_ready_socket_list_head);
    TAILQ_INIT(&closed_socket_list_head);
}
/*
 * This function walks on closable, acceptable and readable lists and calls.
 * the application's (user's) function. Called from app_glue_periodic
 * Paramters: None
 * Returns: None
 *
 */
static inline void process_rx_ready_sockets()
{
    struct socket *sock;
    uint64_t idx, limit;

    while (!TAILQ_EMPTY(&closed_socket_list_head))
    {
        sock = TAILQ_FIRST(&closed_socket_list_head);
        user_on_socket_fatal(sock);
        sock->closed_queue_present = 0;
        TAILQ_REMOVE(&closed_socket_list_head, sock, closed_queue_entry);
        
        #ifdef CLOSE_DEBUG
        printf("In process_rx_ready_sockets(1), sock_put(sock->sk)\n");
        #endif

        sock_put(sock->sk);
        kernel_close(sock);
    }
#if 1
    struct socket *next;
    sock = TAILQ_FIRST(&accept_ready_socket_list_head);
    while (sock != NULL)
    {
        next = TAILQ_NEXT(sock, accept_queue_entry);
        int ret = user_on_accept(sock);
        if (ret != -2)
        {
            sock->accept_queue_present = 0;
            TAILQ_REMOVE(&accept_ready_socket_list_head, sock, accept_queue_entry);
            
            #ifdef CLOSE_DEBUG
            printf("In process_rx_ready_sockets(2), sock_put(sock->sk)\n");
            #endif

            sock_put(sock->sk);
        }
        sock = next;
    }
#else

    while (!TAILQ_EMPTY(&accept_ready_socket_list_head))
    {
        sock = TAILQ_FIRST(&accept_ready_socket_list_head);
        user_on_accept(sock);
        sock->accept_queue_present = 0;
        TAILQ_REMOVE(&accept_ready_socket_list_head, sock, accept_queue_entry);
        sock_put(sock->sk);
    }

#endif
    idx = 0;
    limit = read_sockets_queue_len;
    static read_cnt = 0;
    while ((idx < limit) && (!TAILQ_EMPTY(&read_ready_socket_list_head)))
    {
        light_log(LIGHT_LOG_DEBUG, "read ready socket list %d\n", ++read_cnt);
        sock = TAILQ_FIRST(&read_ready_socket_list_head);
        sock->read_queue_present = 0;
        TAILQ_REMOVE(&read_ready_socket_list_head, sock, read_queue_entry);
        user_data_available_cbk(sock);

        #ifdef CLOSE_DEBUG
        printf("In process_rx_ready_sockets(3), sock_put(sock->sk)\n");
        #endif

        sock_put(sock->sk);
        read_sockets_queue_len--;
        idx++;
    }
}

/*
 * This function walks on writable lists and calls.
 * the application's (user's) function. Called from app_glue_periodic
 * Paramters: None
 * Returns: None
 *
 */
static inline void process_tx_ready_sockets()
{
    struct socket *sock;
    uint64_t idx, limit;

    idx = 0;
    limit = write_sockets_queue_len;
    while ((idx < limit) && (!TAILQ_EMPTY(&write_ready_socket_list_head)))
    {
        sock = TAILQ_FIRST(&write_ready_socket_list_head);
        TAILQ_REMOVE(&write_ready_socket_list_head, sock, write_queue_entry);
        sock->write_queue_present = 0;
        user_on_transmission_opportunity(sock, 2);
        set_bit(SOCK_NOSPACE, &sock->flags);

        #ifdef CLOSE_DEBUG
        printf("In process_tx_ready_sockets, sock_put(sock->sk)\n");
        #endif

        sock_put(sock->sk);
        write_sockets_queue_len--;
        // printf("  write_sockets_queue_len--\n");
        idx++;
    }
}

/* These are in translation of micros to cycles */
static uint64_t app_glue_drv_poll_interval = 0;
static uint64_t app_glue_timer_poll_interval = 0;
static uint64_t app_glue_tx_ready_sockets_poll_interval = 0;
static uint64_t app_glue_rx_ready_sockets_poll_interval = 0;
static uint64_t app_glue_crash_detect_interval = 0;

static uint64_t app_glue_crash_detect_ts = 0;
static uint64_t app_glue_drv_last_poll_ts = 0;
static uint64_t app_glue_timer_last_poll_ts = 0;
static uint64_t app_glue_tx_ready_sockets_last_poll_ts = 0;
static uint64_t app_glue_rx_ready_sockets_last_poll_ts = 0;

/*
 * This function must be called by application to initialize.
 * the rate of polling for driver, timer, readable & writable socket lists
 */
void app_glue_init_poll_intervals(int drv_poll_interval,
                                  int timer_poll_interval,
                                  int tx_ready_sockets_poll_interval,
                                  int rx_ready_sockets_poll_interval,
                                  int crash_detect_interval)
{
    uint64_t cycle_count_per_us = rte_get_tsc_hz() / 1000000;
    app_glue_drv_poll_interval = cycle_count_per_us * drv_poll_interval;
    app_glue_timer_poll_interval = cycle_count_per_us * timer_poll_interval;
    app_glue_tx_ready_sockets_poll_interval = cycle_count_per_us * tx_ready_sockets_poll_interval;
    app_glue_rx_ready_sockets_poll_interval = cycle_count_per_us * rx_ready_sockets_poll_interval;
    app_glue_crash_detect_interval = cycle_count_per_us * crash_detect_interval;
    fprintf(stderr, "app_glue_drv_poll_interval = %"PRIu64"\n", app_glue_drv_poll_interval);
    fprintf(stderr, "app_glue_timer_poll_interval = %"PRIu64"\n", app_glue_timer_poll_interval);
    fprintf(stderr, "app_glue_tx_ready_sockets_poll_interval = %"PRIu64"\n", app_glue_tx_ready_sockets_poll_interval);
    fprintf(stderr, "app_glue_rx_ready_sockets_poll_interval = %"PRIu64"\n", app_glue_rx_ready_sockets_poll_interval);
    fprintf(stderr, "app_glue_crash_detect_interval = %"PRIu64"\n", app_glue_crash_detect_interval);
}

// uint64_t app_glue_periodic_called = 0;
// uint64_t app_glue_tx_queues_process = 0;
// uint64_t app_glue_rx_queues_process = 0;
/*
 * This function must be called by application periodically.
 * This is the heart of the system, it performs all the driver/IP stack work
 * and timers
 * Paramters: call_flush_queues - if non-zero, the readable, closable and writable queues
 * are processed and user's functions are called.
 * Alternatively, call_flush_queues can be 0 and the application may call
 * app_glue_get_next* functions to get readable, acceptable, closable and writable sockets
 * Returns: None
 *
 */
extern enum rte_proc_type_t proc_type;
extern void crash_detect();
void app_glue_periodic(int call_flush_queues, uint8_t *ports_to_poll, int ports_to_poll_count)    //周期性处理收到的数据
{
    // uint64_t ts;
    // uint64_t ts2, ts3, ts4,ts5;
    uint8_t port_idx;
    //app_glue_periodic_called++;
    // ts = rte_rdtsc(); //每经过一个时钟周期，tsc寄存器就自动加1， rte_rdtsc()只是获得tsc寄存器的值

    #ifdef APP_GLUE_PERIODIC_TIME
    struct timeval tv1, tv2;
    long time;
    static int app_glue_poll_num;
    static long app_glue_poll_time_total;
    float app_glue_poll_time_avg;

    static int rte_timer_manage_num;
    static long rte_timer_manage_time_total;
    float rte_timer_manage_time_avg;

    static int process_tx_ready_sockets_num;
    static long process_tx_ready_sockets_time_total;
    float process_tx_ready_sockets_time_avg;

    static int process_rx_ready_sockets_num;
    static long process_rx_ready_sockets_time_total;
    float process_rx_ready_sockets_time_avg;
    #endif

    // sleep for 10us
    // rte_delay_us_block(100);

    // struct timespec tim, tim2;
    // tim.tv_sec = 0;
    // tim.tv_nsec = 10 * 1000;
    // if(nanosleep(&tim , &tim2) < 0 )   
    // {
    //     printf("Nano sleep system call failed \n");
    //     return -1;
    // }

    //拉数据包
    // if ((ts - app_glue_drv_last_poll_ts) >= app_glue_drv_poll_interval)
    // {
    //     // ts4 = rte_rdtsc();
    //     for (port_idx = 0; port_idx < ports_to_poll_count; port_idx++)
    //     {
    //         app_glue_poll(ports_to_poll[port_idx]);
    //     }
    //     app_glue_drv_last_poll_ts = ts;
    //     //working_cycles_stat += rte_rdtsc() - ts4;
    // }

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv1, NULL);
    #endif

    for (port_idx = 0; port_idx < ports_to_poll_count; port_idx++){
        app_glue_poll(ports_to_poll[port_idx]);
    }

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv2, NULL);
    app_glue_poll_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
    app_glue_poll_num++;
    if (unlikely(app_glue_poll_num == 10000)) {
        app_glue_poll_time_avg = (float)app_glue_poll_time_total / app_glue_poll_num;
        // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
        printf("In app_glue_periodic(), app_glue_poll_time_avg = %f us\n", app_glue_poll_time_avg);
        app_glue_poll_time_total = 0;
        app_glue_poll_num = 0;
    }
    #endif

    // if (proc_type == RTE_PROC_PRIMARY)
    // {
    //     if ((ts - app_glue_crash_detect_ts) >= app_glue_crash_detect_interval)
    //     {
    //         // ts5 = rte_rdtsc();
    //         crash_detect();
    //         app_glue_crash_detect_ts = ts;
    //         //working_cycles_stat += rte_rdtsc() - ts5;
    //     }
    // }

    // if ((ts - app_glue_timer_last_poll_ts) >= app_glue_timer_poll_interval)
    // {
    //     // ts3 = rte_rdtsc();
    //     rte_timer_manage();
    //     app_glue_timer_last_poll_ts = ts;
    //     //working_cycles_stat += rte_rdtsc() - ts3;
    // }

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv1, NULL);
    #endif

    rte_timer_manage();

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv2, NULL);
    rte_timer_manage_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
    rte_timer_manage_num++;
    if (unlikely(rte_timer_manage_num == 10000)) {
        rte_timer_manage_time_avg = (float)rte_timer_manage_time_total / rte_timer_manage_num;
        // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
        printf("In app_glue_periodic(), rte_timer_manage_time_avg = %f us\n", rte_timer_manage_time_avg);
        rte_timer_manage_time_total = 0;
        rte_timer_manage_num = 0;
    }
    #endif

    // if (call_flush_queues)
    // {
    //     if ((ts - app_glue_tx_ready_sockets_last_poll_ts) >= app_glue_tx_ready_sockets_poll_interval)
    //     {
    //         // ts2 = rte_rdtsc();
    //         //app_glue_tx_queues_process++;
    //         process_tx_ready_sockets();
    //         //working_cycles_stat += rte_rdtsc() - ts2;
    //         app_glue_tx_ready_sockets_last_poll_ts = ts;
    //     }
    //     if ((ts - app_glue_rx_ready_sockets_last_poll_ts) >= app_glue_rx_ready_sockets_poll_interval)
    //     {
    //         // ts2 = rte_rdtsc();
    //         //app_glue_rx_queues_process++;
    //         process_rx_ready_sockets();
    //         //working_cycles_stat += rte_rdtsc() - ts2;
    //         app_glue_rx_ready_sockets_last_poll_ts = ts;
    //     }
    // }
    // else
    // {
    //     app_glue_tx_ready_sockets_last_poll_ts = ts;
    //     app_glue_rx_ready_sockets_last_poll_ts = ts;
    // }

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv1, NULL);
    #endif

    process_tx_ready_sockets();

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv2, NULL);
    process_tx_ready_sockets_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
    process_tx_ready_sockets_num++;
    if (unlikely(process_tx_ready_sockets_num == 10000)) {
        process_tx_ready_sockets_time_avg = (float)process_tx_ready_sockets_time_total / process_tx_ready_sockets_num;
        // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
        printf("In app_glue_periodic(), process_tx_ready_sockets_time_avg = %f us\n", process_tx_ready_sockets_time_avg);
        process_tx_ready_sockets_time_total = 0;
        process_tx_ready_sockets_num = 0;
    }
    #endif


    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv1, NULL);
    #endif

    process_rx_ready_sockets();

    #ifdef APP_GLUE_PERIODIC_TIME
    gettimeofday(&tv2, NULL);
    process_rx_ready_sockets_time_total += (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
    process_rx_ready_sockets_num++;
    if (unlikely(process_rx_ready_sockets_num == 10000)) {
        process_rx_ready_sockets_time_avg = (float)process_rx_ready_sockets_time_total / process_rx_ready_sockets_num;
        // printf("socket_close_time_total = %d, socket_close_num = %d\n", socket_close_time_total, socket_close_num);
        printf("In app_glue_periodic(), process_rx_ready_sockets_time_avg = %f us\n", process_rx_ready_sockets_time_avg);
        process_rx_ready_sockets_time_total = 0;
        process_rx_ready_sockets_num = 0;
    }
    #endif

    //total_cycles_stat += rte_rdtsc() - ts;
}

/*
 * This function may be called to attach user's data to the socket.
 * Paramters: a pointer  to socket (returned, for example, by create_*_socket)
 * a pointer to data to be attached to the socket
 * Returns: None
 *
 */
void app_glue_set_user_data(void *socket, void *data)
{
    struct socket *sock = (struct socket *)socket;

    if (!sock)
    {
        light_log(LIGHT_LOG_ERR, "PANIC: socket NULL %s %d \n", __FILE__, __LINE__); while (1);
    }
//    if(sock->sk)
    sock->sk->sk_user_data = data;
//    else
//        light_log(LIGHT_LOG_INFO,"PANIC: socket->sk is NULL\n");while(1);
}
/*
 * This function may be called to get attached to the socket user's data .
 * Paramters: a pointer  to socket (returned, for example, by create_*_socket,)
 * Returns: pointer to data to be attached to the socket
 *
 */
inline void *app_glue_get_user_data(void *socket)
{
    struct socket *sock = (struct socket *)socket;
    if (!sock)
    {
        light_log(LIGHT_LOG_ERR, "PANIC: socket NULL %s %d\n", __FILE__, __LINE__); while (1);
    }
    if (!sock->sk)
    {
        light_log(LIGHT_LOG_ERR, "PANIC: socket->sk NULL\n"); while (1);
    }
    return sock->sk->sk_user_data;
}
/*
 * This function may be called to get next closable socket .
 * Paramters: None
 * Returns: pointer to socket to be closed
 *
 */
void *app_glue_get_next_closed()
{
    struct socket *sock;
    void *user_data;
    if (!TAILQ_EMPTY(&closed_socket_list_head))
    {
        sock = TAILQ_FIRST(&closed_socket_list_head);
        sock->closed_queue_present = 0;
        TAILQ_REMOVE(&closed_socket_list_head, sock, closed_queue_entry);
        if (sock->sk)
            user_data = sock->sk->sk_user_data;
        //kernel_close(sock);
        return user_data;
    }
    return NULL;
}
/*
 * This function may be called to get next writable socket .
 * Paramters: None
 * Returns: pointer to socket to be written
 *
 */
void *app_glue_get_next_writer()
{
    struct socket *sock;

    if (!TAILQ_EMPTY(&write_ready_socket_list_head))
    {
        sock = TAILQ_FIRST(&write_ready_socket_list_head);
        sock->write_queue_present = 0;
        TAILQ_REMOVE(&write_ready_socket_list_head, sock, write_queue_entry);
        #ifdef CLOSE_DEBUG
        printf("In app_glue_get_next_writer, TAILQ_REMOVE(&write_ready_socket_list_head, sock, write_queue_entry)\n");
        #endif
        sock_put(sock->sk);
        if (sock->sk)
            return sock->sk->sk_user_data;
        light_log(LIGHT_LOG_ERR, "PANIC: socket->sk is NULL\n");
    }
    return NULL;
}
/*
 * This function may be called to get next readable socket .
 * Paramters: None
 * Returns: pointer to socket to be read
 *
 */
void *app_glue_get_next_reader()
{
    struct socket *sock;
    if (!TAILQ_EMPTY(&read_ready_socket_list_head))
    {
        sock = TAILQ_FIRST(&read_ready_socket_list_head);
        sock->read_queue_present = 0;
        TAILQ_REMOVE(&read_ready_socket_list_head, sock, read_queue_entry);
        if (sock->sk)
            return sock->sk->sk_user_data;
        light_log(LIGHT_LOG_ERR, "PANIC: socket->sk is NULL\n");
    }
    return NULL;
}
/*
 * This function may be called to get next acceptable socket .
 * Paramters: None
 * Returns: pointer to socket on which to accept a new connection
 *
 */
void *app_glue_get_next_listener()
{
    struct socket *sock;
    if (!TAILQ_EMPTY(&accept_ready_socket_list_head))
    {
        sock = TAILQ_FIRST(&accept_ready_socket_list_head);
        sock->accept_queue_present = 0;
        TAILQ_REMOVE(&accept_ready_socket_list_head, sock, accept_queue_entry);
        if (sock->sk){
            printf("In app_glue_get_next_listener, sock->sk = %x\n", sock->sk);
            sock_put(sock->sk);
            return sock->sk->sk_user_data;
        }
        light_log(LIGHT_LOG_ERR, "In app_glue_get_next_listener, PANIC: socket->sk is NULL\n");
        return NULL;
    }
    return NULL;
}
void sock_def_wakeup(struct sock *sk);
void sock_def_readable(struct sock *sk, int len);
void sock_def_error_report(struct sock *sk);
void sock_def_write_space(struct sock *);
/*
 * This function may be called to close socket .
 * Paramters: a pointer to socket structure
 * Returns: None
 *
 */
void app_glue_close_socket(void *sk)
{
    struct socket *sock = (struct socket *)sk;

    if (sock->read_queue_present)
    {
        TAILQ_REMOVE(&read_ready_socket_list_head, sock, read_queue_entry);
        sock->read_queue_present = 0;
        read_sockets_queue_len--;
    }
    if (sock->write_queue_present)
    {
        TAILQ_REMOVE(&write_ready_socket_list_head, sock, write_queue_entry);
        #ifdef CLOSE_DEBUG
        printf("In app_glue_close_socket, TAILQ_REMOVE(&write_ready_socket_list_head, sock, write_queue_entry)\n");
        #endif
        sock_put(sock->sk);
        sock->write_queue_present = 0;
        write_sockets_queue_len--;
    }
    if (sock->accept_queue_present)
    {
        struct socket *newsock = NULL;

        while (kernel_accept(sock, &newsock, 0) == 0)
        {
            kernel_close(newsock);
        }
        TAILQ_REMOVE(&accept_ready_socket_list_head, sock, accept_queue_entry);
        printf("In app_glue_close_socket, TAILQ_REMOVE(&accept_ready_socket_list_head, sock, accept_queue_entry)\n");
        sock_put(sock->sk);
        sock->accept_queue_present = 0;
    }
    if (sock->closed_queue_present)
    {
        TAILQ_REMOVE(&closed_socket_list_head, sock, closed_queue_entry);
        sock->closed_queue_present = 0;
    }
    if (sock->sk)
    {
        sock->sk->sk_user_data = NULL;
        sock->sk->sk_write_space = (sock->type == SOCK_STREAM) ? sk_stream_write_space : sock_def_write_space;
        sock->sk->sk_state_change = sock_def_wakeup;
        sock->sk->sk_data_ready = sock_def_readable;
        sock->sk->sk_error_report = sock_def_error_report;
    }
//    sock->sk->sk_destruct = (sock->sk->sk_socket->type == SOCK_STREAM) ? tcp_sock_destruct : sock_def_destruct;
    //printf("Before Kernel Close\n");
    kernel_close(sock);
}
/*
 * This function may be called to estimate amount of data can be sent .
 * Paramters: a pointer to socket structure
 * Returns: number of bytes the application can send
 *
 */
int app_glue_calc_size_of_data_to_send(void *sock)
{
    int bufs_count1, bufs_count2, bufs_count3, stream_space, bufs_min;
    struct sock *sk = ((struct socket *)sock)->sk;
    if (!sk_stream_is_writeable(sk))
    {
        return 0;
    }
    bufs_count1 = kmem_cache_get_free(get_fclone_cache());
    bufs_count2 = kmem_cache_get_free(get_header_cache());
    bufs_count3 = get_buffer_count();
    if (bufs_count1 > 2)
    {
        bufs_count1 -= 2;
    }
    if (bufs_count2 > 2)
    {
        bufs_count2 -= 2;
    }
    bufs_min = min(bufs_count1, bufs_count2);
    bufs_min = min(bufs_min, bufs_count3);
    if (bufs_min <= 0)
    {
        return 0;
    }
    stream_space = sk_stream_wspace(((struct socket *)sock)->sk);
    return min(bufs_min << 10, stream_space);
}
/*
 * This function may be called to allocate rte_mbuf from existing pool.
 * Paramters: None
 * Returns: a pointer to rte_mbuf, if succeeded, NULL if failed
 *
 */
struct rte_mbuf *app_glue_get_buffer()
{
    return get_buffer();
}

#if 0
void app_glue_print_stats()
{
    float ratio;

    ratio = (float)(total_cycles_stat - total_prev) / (float)(working_cycles_stat - work_prev);
    total_prev = total_cycles_stat;
    work_prev = working_cycles_stat;
    light_log(LIGHT_LOG_INFO, "total %"PRIu64" work %"PRIu64" ratio %f\n", total_cycles_stat, working_cycles_stat, ratio);
    light_log(LIGHT_LOG_INFO, "app_glue_periodic_called %"PRIu64"\n", app_glue_periodic_called);
    light_log(LIGHT_LOG_INFO, "app_glue_tx_queues_process %"PRIu64"\n", app_glue_tx_queues_process);
    light_log(LIGHT_LOG_INFO, "app_glue_rx_queues_process %"PRIu64"\n", app_glue_rx_queues_process);
    light_log(LIGHT_LOG_INFO, "app_glue_sock_readable_called %"PRIu64" app_glue_sock_writable_called %"PRIu64"\n", app_glue_sock_readable_called, app_glue_sock_writable_called);
}
#endif
