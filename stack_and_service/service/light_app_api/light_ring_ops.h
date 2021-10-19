#ifndef __LIGHT_RING_OPS_H__
#define __LIGHT_RING_OPS_H__

#include "../../../light_debug.h"
#include <light_log.h>
#include <unistd.h>

#define MAX_CORES_NUM 32

typedef struct _local_socket_descriptor_
{
    struct rte_ring *tx_ring;
    struct rte_ring *rx_ring;
    struct rte_ring *local_cache;

    struct rte_mbuf *shadow;
    struct rte_mbuf *shadow_next;
    int shadow_len_remainder;
    int shadow_len_delievered;

    light_socket_t *socket;

    int family;
    int type;
    int protocol;
    int is_init;

    // for setsockopt
    int is_set_sockopt;
    int level;
    int optname;
    int optlen;
    char optval[128];

    int proc_id;

    TAILQ_ENTRY(_local_socket_descriptor_) socket_list_entry;
    int any_event_received;

    unsigned int local_ipaddr;
    unsigned int local_port;
    unsigned int remote_ipaddr;
    unsigned int remote_port;
    int present_in_ready_cache;
    int local_mask;
    TAILQ_ENTRY(_local_socket_descriptor_) local_ready_cache_entry;
} local_socket_descriptor_t;

extern struct rte_ring *free_connections_ring;

extern struct rte_ring *command_ring;
extern struct rte_ring *command_rings[MAX_CORES_NUM];
extern struct rte_ring *urgent_command_ring;
extern struct rte_ring *urgent_command_rings[MAX_CORES_NUM];

extern struct rte_mempool *tx_bufs_pool;
extern struct rte_mempool *free_command_pool;
extern struct rte_mempool *free_return_value_pool;
extern struct rte_mempool *free_epoll_node_pool;



extern local_socket_descriptor_t local_socket_descriptors[LIGHT_CONNECTION_POOL_SIZE];

extern uint64_t light_stats_rx_kicks_sent;
extern uint64_t light_stats_rx_full;
extern uint64_t light_stats_rx_dequeued;
extern uint64_t light_stats_rx_dequeued_local;
extern uint64_t light_stats_flush_local;
extern uint64_t light_stats_flush;

static inline int light_enqueue_command_buf(light_cmd_t *cmd)
{
    if (rte_ring_enqueue(command_ring, (void *)cmd) == -ENOBUFS) {
        printf("Error: rte_ring_enqueue(command_ring, (void *)cmd) == -ENOBUFS\n");
        return 1;
    } else {
        return 0;
    }
    // return (rte_ring_enqueue(command_ring, (void *)cmd) == -ENOBUFS);
}

static inline int light_enqueue_command_buf_by_proc_id(light_cmd_t *cmd, int id)
{
    static int command_total_num = 0;
    static int command_urgent_num = 0;
    static int command_common_num = 0;
    static int command_num[30];
    int i, ret;
    // printf("@ In light_enqueue_command_buf_by_proc_id, cmd->cmd = %d, id = %d\n", cmd->cmd, id);
    // if ((cmd->cmd == 6) || (cmd->cmd == 7)){
    //     // urgent command!
    //     ret = (rte_ring_enqueue(urgent_command_rings[id], (void *)cmd) == -ENOBUFS);
    //     command_urgent_num++;
    // }else{
    //     ret = (rte_ring_enqueue(command_rings[id], (void *)cmd) == -ENOBUFS);
    //     command_common_num++;
    // }
    ret = (rte_ring_enqueue(command_rings[id], (void *)cmd) == -ENOBUFS);
    if (ret == 1) {
        printf("Error: rte_ring_enqueue(command_rings[id], (void *)cmd) == -ENOBUFS\n");
    }
    
    #ifdef COMMAND_DEBUG
    command_num[cmd->cmd]++;
    command_total_num++;
    if (command_total_num%10000 == 9999){
        printf("command_total_num = %d, command_urgent_num = %d, command_common_num = %d\n", command_total_num, command_urgent_num, command_common_num);
        for (i = 0; i < 30; i++){
            printf("command_num[%d] = %d\n", i, command_num[i]);
        }
    }
    #endif

    return ret; //if ret = 0, succeed.
}

static inline int light_enqueue_tx_buf(int ringset_idx, struct rte_mbuf *mbuf)
{
    if (rte_ring_sp_enqueue_bulk(local_socket_descriptors[ringset_idx].tx_ring, (void **)&mbuf, 1) == -ENOBUFS) {
        printf("(Error: rte_ring_sp_enqueue_bulk(local_socket_descriptors[ringset_idx].tx_ring, (void **)&mbuf, 1) == -ENOBUFS)\n");
        return 1;
    } else {
        return 0;
    }
    // return (rte_ring_sp_enqueue_bulk(local_socket_descriptors[ringset_idx].tx_ring, (void **)&mbuf, 1) == -ENOBUFS);
}

static inline int light_enqueue_tx_bufs_bulk(int ringset_idx, struct rte_mbuf **mbufs, int buffer_count)
{
    if (rte_ring_sp_enqueue_bulk(local_socket_descriptors[ringset_idx].tx_ring, (void **)mbufs, buffer_count) == -ENOBUFS) {
        printf("Error: (rte_ring_sp_enqueue_bulk(local_socket_descriptors[ringset_idx].tx_ring, (void **)mbufs, buffer_count) == -ENOBUFS)\n");
        return 1;
    } else {
        return 0;
    }
    // return (rte_ring_sp_enqueue_bulk(local_socket_descriptors[ringset_idx].tx_ring, (void **)mbufs, buffer_count) == -ENOBUFS);
}

static inline int light_socket_tx_space(int ringset_idx)
{
    return rte_ring_free_count(local_socket_descriptors[ringset_idx].tx_ring);
}

/*
   time_substract   time_cmp
*/
int time_substract(struct timeval* time_begin, struct timeval* time_end, struct timeval* result)
{
    if (time_begin->tv_sec > time_end->tv_sec)
    {
        return -1;
    }
    if (time_begin->tv_sec == time_end->tv_sec && time_begin->tv_usec > time_end->tv_usec)
    {
        return -1;
    }
    result->tv_sec = time_end->tv_sec - time_begin->tv_sec;
    result->tv_usec = time_end->tv_usec - time_begin->tv_sec;
    if (result->tv_usec < 0 )
    {
        result->tv_usec += 1000000;
        result->tv_sec--;
    }
    return 0;
}
int time_cmp(struct timeval* maxtime, struct timeval* mintime)
{
    if (maxtime->tv_sec > mintime->tv_sec)
    {
        return 1;
    }
    else if (maxtime->tv_sec == mintime->tv_sec && maxtime->tv_usec > mintime->tv_usec)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

/*
updated by gjk_sem_04
**/
static inline struct rte_mbuf *light_dequeue_rx_buf_by_proc_id(int ringset_idx, int proc_id)
{
    printf("In light_dequeue_rx_buf_by_proc_id\n");

    struct rte_mbuf *mbuf = NULL, *mbufs[MAX_PKT_BURST];
    light_cmd_t *cmd;
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (likely(cmd))
    {
        cmd->cmd = LIGHT_SOCKET_RX_KICK_COMMAND;
        cmd->ringset_idx = ringset_idx;
        light_enqueue_command_buf_by_proc_id(cmd, proc_id);
        // light_stats_rx_kicks_sent++;
    }

    // struct timeval time_begin, time_end, time_use;
    // gettimeofday(&time_begin, NULL);

    while (
        rte_ring_empty(local_socket_descriptors[ringset_idx].local_cache) == 1
        && rte_ring_empty(local_socket_descriptors[ringset_idx].rx_ring) == 1
        && rte_atomic16_read(&local_socket_descriptors[ringset_idx].socket->connect_close_signal) < 3
    )
    {
    }

    // /*
    //  * 先从local cache里面取，没有的话，从rx_ring里面往local cache里放，然后再从local cache里面取
    //  */
    // if (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0) {
    //     printf("Error (0): rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0\n");
    // }

    // light_socket_t *skt = local_socket_descriptors[ringset_idx].socket;
    //local cahce 需要的数目,MAX_PKT_BURST  should be concerned
    
    // int dequeued = rte_ring_free_count(local_socket_descriptors[ringset_idx].local_cache) > MAX_PKT_BURST ? MAX_PKT_BURST :
    //                rte_ring_free_count(local_socket_descriptors[ringset_idx].local_cache);

    // //现在rx_ring里面有的entry数目
    // int ring_cnt = rte_ring_count(local_socket_descriptors[ringset_idx].rx_ring);
    // //我们需要从rx_ring里面出队的数目
    // dequeued = dequeued < ring_cnt ? dequeued : ring_cnt;

    // int real_dequeued = 0;
    // if (dequeued > 0)
    // {
    //     real_dequeued = rte_ring_sc_dequeue_burst(
    //                         local_socket_descriptors[ringset_idx].rx_ring,
    //                         (void **)mbufs,
    //                         dequeued);
    //     //real_dequeued 是真正dequeue出来的entry数目，由于某种原因可能会小于dequeued
    //     //还要继续判断
    //     if (real_dequeued == 0)
    //     {
    //         printf("Error: in light_dequeue_rx_buf_by_proc_id, real_dequeued == 0\n");
    //     }

    //     if (real_dequeued > 0)
    //     {
    //         // light_stats_rx_dequeued++;
    //         if (rte_ring_sp_enqueue_burst(local_socket_descriptors[ringset_idx].local_cache, (void **)mbufs, real_dequeued) == 0) {
    //             printf("Error: rte_ring_sp_enqueue_burst(local_socket_descriptors[ringset_idx].local_cache, (void **)mbufs, real_dequeued) == 0\n");
    //         }
    //     }
    // }

    // if (mbuf == NULL)
    if (1)
    {
        //接着上面，走到这里说明之前从local_cache里面出队的时候local cache里面是空的
        //没拿出东西来，这里经过从rx_ring里面向local cache里面放了一次东西，我们再从local cache里面拿
        if (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) == 0)
        {
            // light_stats_rx_dequeued_local += mbuf->nb_segs;
        } else {
            printf("Error (1): rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0\n");
        }
    }
    // else
    // {
    //     light_stats_rx_dequeued_local += mbuf->nb_segs;
    // }
    return mbuf;
}

static inline struct rte_mbuf *light_dequeue_rx_buf(int ringset_idx)
{
    printf("In light_dequeue_rx_buf\n");
    struct rte_mbuf *mbuf = NULL, *mbufs[MAX_PKT_BURST];
    light_cmd_t *cmd;
    cmd = light_get_free_command_buf();
    if (cmd)
    {
        cmd->cmd = LIGHT_SOCKET_RX_KICK_COMMAND;
        cmd->ringset_idx = ringset_idx;
        light_enqueue_command_buf(cmd);
        light_stats_rx_kicks_sent++;
    }

    struct timeval time_begin, time_end, time_use;
    gettimeofday(&time_begin, NULL);

    while (
        rte_ring_empty(local_socket_descriptors[ringset_idx].local_cache) == 1
        && rte_ring_empty(local_socket_descriptors[ringset_idx].rx_ring) == 1
        && rte_atomic16_read(&local_socket_descriptors[ringset_idx].socket->connect_close_signal) < 3
    ){}
    if (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0) {
        printf("Error (2): rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0\n");
    }
    light_socket_t *skt = local_socket_descriptors[ringset_idx].socket;
    int dequeued = rte_ring_free_count(local_socket_descriptors[ringset_idx].local_cache) > MAX_PKT_BURST ? MAX_PKT_BURST :
                   rte_ring_free_count(local_socket_descriptors[ringset_idx].local_cache);

    int ring_cnt = rte_ring_count(local_socket_descriptors[ringset_idx].rx_ring);
    dequeued = dequeued < ring_cnt ? dequeued : ring_cnt;

    int real_dequeued = 0;
    if (dequeued > 0)
    {
        real_dequeued = rte_ring_sc_dequeue_burst(
                            local_socket_descriptors[ringset_idx].rx_ring,
                            (void **)mbufs,
                            dequeued);
        if (real_dequeued == 0)
        {
            printf("Error: in light_dequeue_rx_buf, real_dequeued == 0\n");
        }

        if (real_dequeued > 0)
        {
            light_stats_rx_dequeued++;
            if (rte_ring_sp_enqueue_burst(local_socket_descriptors[ringset_idx].local_cache, (void **)mbufs, real_dequeued) == 0) {
                printf("Error: rte_ring_sp_enqueue_burst(local_socket_descriptors[ringset_idx].local_cache, (void **)mbufs, real_dequeued) == 0\n");
            }
        }
    }

    if (mbuf == NULL)
    {
        if (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) == 0)
        {
            light_stats_rx_dequeued_local += mbuf->nb_segs;
        } else {
            printf("Error (3): rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0\n");
        }
    }
    else
    {
        light_stats_rx_dequeued_local += mbuf->nb_segs;
    }
    return mbuf;
}

static inline struct rte_mbuf *light_dequeue_rx_buf_nonblock_by_proc_id(int ringset_idx, int proc_id)
{
    // printf("In light_dequeue_rx_buf_nonblock_by_proc_id\n"); 

    struct rte_mbuf *mbuf = NULL, *mbufs[MAX_PKT_BURST];
    light_cmd_t *cmd;
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (likely(cmd))
    {
        cmd->cmd = LIGHT_SOCKET_RX_KICK_COMMAND;
        cmd->ringset_idx = ringset_idx;
        light_enqueue_command_buf_by_proc_id(cmd, proc_id);
        // light_stats_rx_kicks_sent++;
    }

    // struct timeval time_begin, time_end, time_use;
    // gettimeofday(&time_begin, NULL);

    if (rte_ring_empty(local_socket_descriptors[ringset_idx].local_cache) == 1
        && rte_ring_empty(local_socket_descriptors[ringset_idx].rx_ring) == 1
        && rte_atomic16_read(&local_socket_descriptors[ringset_idx].socket->connect_close_signal) < 3
    )
    {
        return NULL;
    }

    // if (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0) {
    //     // this is not error and later it will try to dequeue again.
        
    //     // printf("  rte_ring_empty(local_socket_descriptors[ringset_idx].local_cache) = %d\n", rte_ring_empty(local_socket_descriptors[ringset_idx].local_cache));
    //     // printf("  rte_ring_count(local_socket_descriptors[ringset_idx].local_cache) = %u\n", rte_ring_count(local_socket_descriptors[ringset_idx].local_cache));
    //     // printf("  Error (4): rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0\n");
    // }

    // light_socket_t *skt = local_socket_descriptors[ringset_idx].socket;

    // int dequeued = rte_ring_free_count(local_socket_descriptors[ringset_idx].local_cache) > MAX_PKT_BURST ? MAX_PKT_BURST :
    //                rte_ring_free_count(local_socket_descriptors[ringset_idx].local_cache);

    // int ring_cnt = rte_ring_count(local_socket_descriptors[ringset_idx].rx_ring);
    // dequeued = dequeued < ring_cnt ? dequeued : ring_cnt;

    // int real_dequeued = 0;
    // if (dequeued > 0)
    // {
    //     real_dequeued = rte_ring_sc_dequeue_burst(
    //                         local_socket_descriptors[ringset_idx].rx_ring,
    //                         (void **)mbufs,
    //                         dequeued);
    //     if (real_dequeued == 0)
    //     {
    //         printf("Error: in light_dequeue_rx_buf_nonblock_by_proc_id, real_dequeued == 0\n");
    //     }

    //     if (real_dequeued > 0)
    //     {
    //         light_stats_rx_dequeued++;
    //         if (rte_ring_sp_enqueue_burst(local_socket_descriptors[ringset_idx].local_cache, (void **)mbufs, real_dequeued) == 0) {
    //             printf("Error: rte_ring_sp_enqueue_burst(local_socket_descriptors[ringset_idx].local_cache, (void **)mbufs, real_dequeued) == 0\n");
    //         }
    //     }
    // }

    // if (mbuf == NULL) {
    if (1) {
        if (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0) {
            printf("Error (5): rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) != 0\n");
        }
    }
    
    return mbuf;
}

static inline void light_flush_rx(int ringset_idx)
{
    struct rte_mbuf *mbuf = NULL;

    while (rte_ring_dequeue(local_socket_descriptors[ringset_idx].local_cache, (void **)&mbuf) == 0)
    {
        light_stats_flush_local += mbuf->nb_segs;
        rte_pktmbuf_free(mbuf);
    }
    while (rte_ring_sc_dequeue_burst(
                local_socket_descriptors[ringset_idx].rx_ring,
                (void **)&mbuf,
                1) == 1)
    {
        light_stats_flush += mbuf->nb_segs;
        rte_pktmbuf_free(mbuf);
    }
    if (local_socket_descriptors[ringset_idx].shadow)
    {
        light_stats_flush += local_socket_descriptors[ringset_idx].shadow->nb_segs;
        rte_pktmbuf_free(local_socket_descriptors[ringset_idx].shadow);
        local_socket_descriptors[ringset_idx].shadow = NULL;
    }
    if (local_socket_descriptors[ringset_idx].shadow_next)
    {
        light_stats_flush += local_socket_descriptors[ringset_idx].shadow_next->nb_segs;
        rte_pktmbuf_free(local_socket_descriptors[ringset_idx].shadow_next);
        local_socket_descriptors[ringset_idx].shadow_next = NULL;
    }
    local_socket_descriptors[ringset_idx].shadow_len_delievered = 0;
    local_socket_descriptors[ringset_idx].shadow_len_remainder = 0;
}

#endif /* __LIGHT_RING_OPS_H__ */
