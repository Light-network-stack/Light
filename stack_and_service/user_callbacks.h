/*
 *  user_callbacks.h
 *  Contains function prototypes for building applications
 *  on the top of Linux TCP/IP ported to userland and integrated with DPDK 1.6
 *  These functions have to be implemented by the application(s)
 */

#ifndef __USER_CALLBACKS_H_
#define __USER_CALLBACKS_H_

#include <light_log.h>
#include "../light_debug.h"

extern uint64_t user_on_tx_opportunity_called;
extern uint64_t user_on_tx_opportunity_api_nothing_to_tx;
extern uint64_t user_on_tx_opportunity_api_failed;
extern uint64_t user_on_rx_opportunity_called;
extern uint64_t user_on_rx_opportunity_called_exhausted;
extern uint64_t user_rx_mbufs;
extern uint64_t user_rx_ring_full;
extern uint64_t user_on_tx_opportunity_socket_send_error;
extern uint64_t user_flush_rx_cnt;
extern uint64_t user_flush_tx_cnt;

void app_glue_sock_readable(struct sock *sk, int len);
void app_glue_sock_write_space(struct sock *sk);
void app_glue_sock_error_report(struct sock *sk);

extern int *g_monitor_info;

static inline __attribute__ ((always_inline)) void *get_user_data(void *socket) {
    struct socket *sock = (struct socket *)socket;
    if(!sock) {
        light_log(LIGHT_LOG_ERR,"PANIC: socket NULL %s %d\n",__FILE__,__LINE__);
        exit(1);
    }
    if(!sock->sk) {
        light_log(LIGHT_LOG_ERR,"PANIC: get_user_data failure socket_state=%d\n", sock->state);
        return NULL;
	//exit(1);
    }
    return sock->sk->sk_user_data;
}

static inline __attribute__ ((always_inline)) int user_on_accept(struct socket *sock) {
    struct socket *newsock = NULL;
    light_cmd_t *cmd;
    void *parent_descriptor;
    void *socket_satelite_data = get_user_data(sock);
    if(!socket_satelite_data) {
        //light_log(LIGHT_LOG_WARNING, "user_on_accept get_user_data failure");
        return -1;
    }
    int ringfree = light_rx_buf_free_count(socket_satelite_data);
    int exhausted = 0;
    while(ringfree > 0) {
//      newsock->sk->sk_route_caps |= NETIF_F_SG |NETIF_F_ALL_CSUM|NETIF_F_GSO;
        if(unlikely(kernel_accept(sock, &newsock, 0) != 0)) {
            exhausted = 1;
            break;
        }
        cmd = light_get_free_command_buf();
        if(cmd) {
            cmd->cmd = LIGHT_SOCKET_ACCEPTED_COMMAND;
            parent_descriptor = get_user_data(sock);
            cmd->u.accepted_socket.socket_descr = newsock;
            cmd->u.accepted_socket.ipaddr = newsock->sk->sk_daddr;
            cmd->u.accepted_socket.port = newsock->sk->sk_dport;
            app_glue_set_user_data(newsock,NULL);
            light_post_accepted(cmd, parent_descriptor);
            --ringfree;
        } else {
            light_log(LIGHT_LOG_ERR, "user_on_accept no cmd buff");
        }
        sock_reset_flag(newsock->sk,SOCK_USE_WRITE_QUEUE);
        newsock->sk->sk_data_ready = app_glue_sock_readable;
        newsock->sk->sk_write_space = app_glue_sock_write_space;
        newsock->sk->sk_error_report = app_glue_sock_error_report;
    }
    if(!ringfree && !exhausted) {
        light_log(LIGHT_LOG_ERR, "user_on_accept no free ring or exhausted");
        return -2;
    }
    return 0;
}

/* once this function is called,
   user application-toward-socket ring is checked.
   If empty, the selector is kicked.
   Otherwise, the data is read from the ring and written to socket. If socket's space is not exhausted,
   selector is kicked.
*/
static inline __attribute__ ((always_inline)) void user_on_transmission_opportunity(struct socket *sock, int mode) {
    struct page page;
    int i = 0;
    uint32_t to_send_this_time;
    void *socket_satelite_data;
    uint64_t ring_entries;

    // user_on_tx_opportunity_called++;

    if (mode == 1){ // process_command.
        #ifdef LIGHT_RTE_RING_DEBUG
        printf("In user_on_transmission_opportunity, mode = %d, process_command.\n", mode);
        #endif
        g_monitor_info[4]++;
    } else { // process_tx_ready_sockets.
        #ifdef LIGHT_RTE_RING_DEBUG
        printf("In user_on_transmission_opportunity, mode = %d, process_tx_ready_sockets.\n", mode);
        #endif
        g_monitor_info[14]++;
    }

    if (unlikely(!sock)) {
        printf("user_on_transmission_opportunity: sock is NULL\n");
        return;
    }
    socket_satelite_data = get_user_data(sock);
    if ((!socket_satelite_data)) { // always happens.
        // light_log(LIGHT_LOG_ERR, "user_on_transmission_opportunity: get_user_data failure: socket->sk NULL\n");
        if (mode == 1){
            g_monitor_info[5]++;
        } else {
            g_monitor_info[15]++;
        }
        return;
    }
    // printf("user_on_transmission_opportunity: socket_satelite_data exist\n");
    if (sock->sk->sk_state == TCP_LISTEN) {
        light_log(LIGHT_LOG_ERR,"%s %d\n",__FILE__,__LINE__);
        exit(0);
    }
    if (sock->type == SOCK_STREAM) {
        ring_entries = light_tx_buf_count(socket_satelite_data);
        
        #ifdef LIGHT_RTE_RING_DEBUG
        printf("socket_satelite_data->ringset_idx = %d, ring_entries = %u\n", ((socket_satelite_data_t *)socket_satelite_data)->ringset_idx, ring_entries);
        #endif

        if (ring_entries == 0) { // always happens.
            // light_mark_writable(socket_satelite_data);
            // user_on_tx_opportunity_api_nothing_to_tx++;
            // printf("user_on_transmission_opportunity: ring_entries == 0\n");
            if (mode == 1){
                g_monitor_info[6]++;
            } else {
                g_monitor_info[16]++;
            }
            return;
        }
        while (sk_stream_wspace(sock->sk) > 0) {
            if (mode == 1){
                g_monitor_info[7]++;
            } else {
                g_monitor_info[17]++;
            }
            if ((i = kernel_sendpage(sock, &page, 0/*offset*/,sk_stream_wspace(sock->sk), 0 /*flags*/)) <= 0)
                break;
        }

        // ring_entries = light_tx_buf_count(socket_satelite_data);

        // if(ring_entries == 0) {
        //     light_mark_writable(socket_satelite_data);
        // } else {
        //     user_on_tx_opportunity_socket_send_error += (i<=0);
        // }
    } else if ((sock->type == SOCK_DGRAM)||(sock->type == SOCK_RAW)) {
        printf("  SOCK_DGRAM or SOCK_RAW\n");

        struct msghdr msghdr;
        struct iovec iov;
        struct sock *sk = sock->sk;
        int dequeued,rc = 0,exhausted = 0;

        msghdr.msg_namelen = sizeof(struct sockaddr_in);
        msghdr.msg_iov = &iov;
        msghdr.msg_iovlen = 1;
        msghdr.msg_controllen = 0;
        msghdr.msg_control = 0;
        msghdr.msg_flags = 0;

        do {
            ring_entries = light_tx_buf_count(socket_satelite_data);
            dequeued = 0;

            if(ring_entries > 0) {
                struct rte_mbuf *mbuf[ring_entries];
                int established = (sock->sk) ? (sock->sk->sk_state != TCP_ESTABLISHED) : 0;
                dequeued = light_dequeue_tx_buf_burst(socket_satelite_data, mbuf, ring_entries);
                for(i = 0; i < dequeued; i++) {
                    rte_prefetch0(rte_pktmbuf_mtod(mbuf[i],void *));
                }
                for(i = 0; i < dequeued; i++) {
                    char *p_addr;
                    if(established) {
                        p_addr = rte_pktmbuf_mtod(mbuf[i],char *);
                        p_addr -= sizeof(struct sockaddr_in);
                        msghdr.msg_name = p_addr;
                    } else
                        msghdr.msg_name = NULL;

                    iov.head = mbuf[i];

                    rc = udp_sendmsg(NULL, sk, &msghdr, rte_pktmbuf_data_len(mbuf[i]));
                    exhausted |= !(rc > 0);
                    if(exhausted) {
                        user_on_tx_opportunity_api_failed += dequeued - i;
                        for(; i < dequeued; i++) {
                            rte_pktmbuf_free(mbuf[i]);
                        }
                        break;
                    }
                }
//                    printf("ring entries %d dequeued %d\n",ring_entries,dequeued);
            } else {
                user_on_tx_opportunity_api_nothing_to_tx++;
            }
        } while((dequeued > 0) && (!exhausted));
        if(!exhausted)//may write more
            light_mark_writable(socket_satelite_data);
    }
}

/* once data is received, this function is called.
   - If there is no space  in the ring toward user application,
     don't read and kick the selector. Once user is awake, it reads
     the buffers from the ring and kicks the socket. On kick,
     this function is called again
   - If there is insufficient/enugh space, data is read from the socket as much as possible
     and is  placed in ring toward user application, selector is kicked
*/
static inline __attribute__ ((always_inline)) void user_data_available_cbk(struct socket *sock) {
    struct msghdr msg;
    struct iovec vec;
    struct rte_mbuf *mbuf;
    int ring_free,exhausted = 0;
    void *socket_satelite_data;
    unsigned int ringset_idx;
    struct sockaddr_in sockaddrin;

    user_on_rx_opportunity_called++;
    memset(&vec,0,sizeof(vec));
    if(unlikely(sock == NULL)) {
        return;
    }
    socket_satelite_data = get_user_data(sock);
    if(!socket_satelite_data) {
        //light_log(LIGHT_LOG_WARNING, "user_data_available_cbk get_user_data failure: socket->sk NULL\n");
        return;
    }

    if(sock->sk->sk_state == TCP_LISTEN) {
        light_log(LIGHT_LOG_ERR,"%s %d\n",__FILE__,__LINE__);
        exit(0);
    }

    if((sock->type == SOCK_DGRAM)||(sock->type == SOCK_RAW)) {
        msg.msg_namelen = sizeof(sockaddrin);
        msg.msg_name = &sockaddrin;
    }
    ring_free = light_rx_buf_free_count(socket_satelite_data);

    user_rx_ring_full += !ring_free;
    while(ring_free > 0) {
        memset(&vec,0,sizeof(vec));
        int rc;
        if(unlikely((rc = kernel_recvmsg(sock, &msg,&vec, 1 /*num*/, /*ring_free*1448*/-1 /*size*/, 0 /*flags*/)) <= 0)) {
            exhausted = 1;
            break;
        }
        if(msg.msg_iov->head == NULL)
//	    printf("rc == %d %lu %lu\n",rc,ring_free,user_rx_mbufs);
            ring_free--;
        if((sock->type == SOCK_DGRAM)||(sock->type == SOCK_RAW)) {
            char *p_addr = rte_pktmbuf_mtod(msg.msg_iov->head, char *);
            p_addr -= msg.msg_namelen;
            memcpy(p_addr,msg.msg_name,msg.msg_namelen);
        }
        int mbufs = msg.msg_iov->head->nb_segs;
        if(light_submit_rx_buf(msg.msg_iov->head,socket_satelite_data)) {
            light_log(LIGHT_LOG_ERR,"%s %d\n",__FILE__,__LINE__);//shoud not happen!!!
            rte_pktmbuf_free(msg.msg_iov->head);
            break;
        } else {
            user_rx_mbufs+=mbufs;
        }
    }
    user_on_rx_opportunity_called_exhausted += exhausted;
}

static inline void user_on_closure(struct socket *sock, int close_num)
{
    socket_satelite_data_t *socket_satelite_data = get_user_data(sock);
    if (!socket_satelite_data)
    {
        return;
    }
    light_mark_closed(socket_satelite_data, close_num);
}

static inline void user_flush_tx(struct socket *sock) {
    // printf("In user_flush_tx\n");
    void *socket_satelite_data = get_user_data(sock);

    if (!socket_satelite_data)
        return;
    struct rte_mbuf *mbuf;

    while((mbuf = light_dequeue_tx_buf(socket_satelite_data)) != NULL) {
        user_flush_tx_cnt += mbuf->nb_segs;
        rte_pktmbuf_free(mbuf);
    }
}

static inline void user_flush_rx(struct socket *sock) {
    socket_satelite_data_t *socket_satelite_data = get_user_data(sock);

    if (!socket_satelite_data)
        return;
    struct rte_mbuf *mbuf;
    
    while(rte_ring_dequeue(socket_satelite_data->rx_ring,&mbuf) == 0) {
        user_flush_rx_cnt += mbuf->nb_segs;
        rte_pktmbuf_free(mbuf);
    }
}

static inline void user_flush_rx_tx(struct socket *sock) {
    user_flush_rx(sock);
    user_flush_tx(sock);
}

/**
 * when connect_close_signal is 3, indicates that is TCP_CLOSE_WAIT
 * when connect_close_signal is 4, indicates that is TCP_CLOSE
 */
static inline __attribute__ ((always_inline)) void user_on_socket_fatal(struct socket *sock) {
    if(sock->sk->sk_state==TCP_CLOSE_WAIT) {
        user_on_closure(sock,3);
    } else {
        //user_flush_rx_tx(sock);/* flush data */
        user_on_closure(sock,4);
    }
}

#endif /* API_H_ */
