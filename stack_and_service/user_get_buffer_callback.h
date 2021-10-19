
#ifndef __USER_GET_BUFFER_CALLBACK__
#define __USER_GET_BUFFER_CALLBACK__

#include "light_common.h"
#include "light_server_side.h"
#include "../light_debug.h"

#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

extern uint64_t user_on_tx_opportunity_cycles;
extern uint64_t user_on_tx_opportunity_called;
extern uint64_t user_on_tx_opportunity_getbuff_called;
extern uint64_t user_on_tx_opportunity_api_mbufs_sent;
extern uint64_t user_on_tx_opportunity_cannot_get_buff;
extern uint64_t user_rx_mbufs;

static inline __attribute__ ((always_inline)) struct rte_mbuf *user_get_buffer(struct sock *sk, int *copy)
{
    struct rte_mbuf *mbuf, *first = NULL, *prev;
    void *socket_satelite_data;
    // unsigned int i = 0;
    // unsigned count_this_time = 0;

    #ifdef LIGHT_RTE_RING_DEBUG
    printf("In user_get_buffer\n");
    #endif

    // user_on_tx_opportunity_getbuff_called++;
    if (unlikely(sk->sk_socket == NULL))
        return NULL;
    socket_satelite_data = sk->sk_user_data;
    
    while (*copy >= 1448) { // since copy = 1448, run it for once.
        // printf("*copy >= 1448\n");
        int ring_entries = light_tx_buf_count(socket_satelite_data);

        #ifdef LIGHT_RTE_RING_DEBUG
        printf("socket_satelite_data->ringset_idx = %d, ring_entries = %u\n", ((socket_satelite_data_t *)socket_satelite_data)->ringset_idx, ring_entries);
        #endif

        mbuf = light_dequeue_tx_buf(socket_satelite_data);
        if (unlikely(mbuf == NULL)) {
            // user_on_tx_opportunity_cannot_get_buff++;
            #ifdef LIGHT_RTE_RING_DEBUG
            printf("Error: in user_get_buffer, light_dequeue_tx_buf(socket_satelite_data) = NULL\n");
            #endif
            
            return first;
        }
        (*copy) -= rte_pktmbuf_data_len(mbuf);
        // printf("*copy = %d\n", *copy);
        if (!first)
            first = mbuf;
        else
            prev->next = mbuf;
        prev = mbuf;
        // user_on_tx_opportunity_api_mbufs_sent++;
//        break;
    }
    return first;
}

#endif /* __USER_GET_BUFFER_CALLBACK__ */
