#ifndef __LIGHT__COMMON_H__
#define __LIGHT__COMMON_H__

#include <rte_common.h>
#include <rte_rwlock.h>
#include "service/ll.h"
#include "../light_debug.h"
#include <rte_errno.h>
#include <rte_debug.h>

#ifndef EPOLL_SUPPORT
#define EPOLL_SUPPORT
#endif

#ifndef CCJ_API
#define CCJ_API
#endif

// Only choose one of these two
#define LIGHT_FD_MAPPING
// #define TWO_SIDE_FD_SEPARATION

#define MAX_CORES_NUM 32

#define LIGHT_FD_THRESHOLD 1000000000
#define APP_LOG_ENABLE 1
#define PKT_PAYLOAD_MAX_SIZE 1448
// #define LIGHT_MAX_SOCKETS 1000
#define MAX_APPS 20
#define MAX_CMD_DEQUEUE_NUM 100

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define COMMAND_POOL_SIZE 16384*2
#define LIGHT_CONNECTION_POOL_SIZE 2048 // light socket number
#define LIGHT_EVENT_INFO_POOL_SIZE (1<<16)
#define LIGHT_CLIENTS_POOL_SIZE 64

#define FREE_CONNECTIONS_POOL_NAME "free_connections_pool"
#define FREE_EVENT_INFO_POOL_NAME "free_event_info_pool"
#define FREE_CONNECTIONS_RING "free_connections_ring"
#define FREE_CLIENTS_RING "free_clients_ring"
#define COMMAND_RING_NAME "command_ring"
#define FREE_COMMAND_POOL_NAME "free_command_pool"

#define FREE_RETURN_VALUE_POOL_NAME "free_return_value_pool"
#define FREE_EPOLL_NODE_POOL_NAME "free_epoll_node_pool"

#define RX_RING_NAME_BASE "rx_ring"
#define TX_RING_NAME_BASE "tx_ring"
#define ACCEPTED_RING_NAME "accepted_ring"
#define FREE_ACCEPTED_POOL_NAME "free_accepted_pool"
#define COMMON_NOTIFICATIONS_POOL_NAME "common_notifications_pool_name"
#define COMMON_NOTIFICATIONS_RING_NAME "common_notifications_ring_name"

#define FREE_FD_MAPPING_STORAGE_POOL_NAME "free_fd_mapping_storage" // can not be too long
#define TRUE_FD_NUM 2000
#define TRUE_FD_MAX_SIZE 3000
#define LIGHT_SOCKET_INDEX 0
#define LIGHT_EPOLL_INDEX 1

#define LIGHT_EPOLL_POOL_SIZE 64
#define LIGHT_EPOLL_RING_NAME "epoll_ring"
#define LIGHT_EPOLL_POOL_NAME "epoll_pool"

#define MAX_ADDR_LEN    32
#define MAX_WORKER 1024

#define SOCKET_READABLE_BIT 1
#define SOCKET_WRITABLE_BIT 2
#define SOCKET_CLOSED_BIT   4
#define SOCKET_READY_SHIFT 16
#define SOCKET_READY_MASK 0xFFFF

#define POLL_NIC_PORT_NUM 1

#define MIN_2(a, b) ((a) < (b)) ? (a) : (b)

typedef union light_epoll_data
{
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} light_epoll_data_t;


// struct light_epoll_event
// {
//     uint32_t events;
//     light_epoll_data_t data;
// } __attribute__((packed));

typedef struct
{
    uint32_t events;
    light_epoll_data_t data;
} __attribute__((packed)) light_epoll_event; // todo: when "packed" is converted to "aligned(4)" or removed, nginx crashes. Why?

typedef struct
{
    struct rte_ring *ready_connections;
    struct rte_ring *shadow_connections;
    unsigned long epoll_idx;
    uint8_t is_active, is_sleeping;
    rte_rwlock_t ep_lock;
    //添加了原生epoll和有名管道
    //管道用O_RDWR标志打开，否则会阻塞
    int kernel_epoll_fd;
    //非网络fd的个数，这里注意超过64个会出问题
    light_epoll_event epoll_events[64];
    int fifo_write_fds[MAX_CORES_NUM];
    int fifo_read_fds[MAX_CORES_NUM];
    pid_t pid;
} light_epoll_t;

enum light_epoll_op
{
    LIGHT_EPOLL_CTL_ADD = 1,
    LIGHT_EPOLL_CTL_DEL = 2,
    LIGHT_EPOLL_CTL_MOD = 3,
};

enum light_event_type
{
    LIGHT_EPOLLNONE  = 0x000,
    LIGHT_EPOLLIN    = 0x001,
    LIGHT_EPOLLPRI   = 0x002,
    LIGHT_EPOLLOUT   = 0x004,
    LIGHT_EPOLLRDNORM    = 0x040,
    LIGHT_EPOLLRDBAND    = 0x080,
    LIGHT_EPOLLWRNORM    = 0x100,
    LIGHT_EPOLLWRBAND    = 0x200,
    LIGHT_EPOLLMSG       = 0x400,
    LIGHT_EPOLLERR       = 0x008,
    LIGHT_EPOLLHUP       = 0x010,
    LIGHT_EPOLLRDHUP     = 0x2000,
    LIGHT_EPOLLONESHOT   = (1 << 30),
    LIGHT_EPOLLET        = (1 << 31)
};

struct light_epoll_cache
{
    int ready_mask;
    struct light_epoll_cache *next;
};

enum
{
    LIGHT_DUMMY_COMMAND = 0,
    LIGHT_OPEN_SOCKET_COMMAND,
    LIGHT_SOCKET_CONNECT_BIND_COMMAND,
    LIGHT_LISTEN_SOCKET_COMMAND,
    LIGHT_SETSOCKOPT_COMMAND,
    LIGHT_OPEN_SOCKET_FEEDBACK,
    LIGHT_SOCKET_TX_KICK_COMMAND,
    LIGHT_SOCKET_RX_KICK_COMMAND,
    LIGHT_SOCKET_ACCEPTED_COMMAND,
    LIGHT_SET_SOCKET_RING_COMMAND,
    LIGHT_SOCKET_READY_FEEDBACK,
    LIGHT_SOCKET_CLOSE_COMMAND,
    LIGHT_SOCKET_TX_POOL_EMPTY_COMMAND,
    LIGHT_ROUTE_ADD_COMMAND,
    LIGHT_ROUTE_DEL_COMMAND,
    LIGHT_CONNECT_CLIENT,
    LIGHT_DISCONNECT_CLIENT,
    LIGHT_NEW_IFACES,
    LIGHT_NEW_ADDRESSES,
    LIGHT_END_OF_RECORD,
    LIGHT_SOCKET_SHUTDOWN_COMMAND,
    LIGHT_SOCKET_DECLINE_COMMAND,
    LIGHT_EPOLL_CREATE_COMMAND,
    LIGHT_EPOLL_CLOSE_COMMAND,
    LIGHT_ADD_ARP_COMMAND,
};

typedef struct
{
    int port_number;
    uint32_t sip;
    unsigned char sha[MAX_ADDR_LEN];
    int creat;
    int state;
    int override;
} light_add_arp_cmd_t;


typedef struct
{
    int epoll_idx;
    pid_t pid;
} light_epoll_create_cmd_t;

typedef struct
{
    int epoll_idx;
    pid_t pid;
} light_epoll_close_cmd_t;

typedef struct
{
    int family;
    int type;
    unsigned long pid;
} light_open_sock_cmd_t;

typedef struct
{
    unsigned long socket_descr;
    unsigned int ipaddr;
    unsigned short port;
} light_open_accepted_socket_t;

typedef struct
{
    unsigned long socket_descr;
} light_decline_accepted_socket_t;

typedef struct
{
    unsigned long socket_descr;
} light_socket_kick_cmd_t;

#ifdef CCJ_API
//we add listen backlog
typedef struct
{
    unsigned int server_backlog;
} light_socket_listen_cmd_t;
#endif

typedef struct
{
    unsigned long socket_descr;
    unsigned long pid;
} light_set_socket_ring_cmd_t;

typedef struct
{
    unsigned int ipaddr;
    unsigned short port;
    unsigned short is_connect;
} light_socket_connect_bind_cmd_t;

typedef struct
{
    unsigned int dest_ipaddr;
    unsigned int dest_mask;
    unsigned int next_hop;
    short        metric;
} light_route_cmd_t;

typedef struct
{
    int level;
    int optname;
    int optlen;
    char optval[128];
} light_setsockopt_cmd_t;

typedef struct
{
    unsigned int bitmask;
} light_socket_ready_feedback_t;

typedef struct
{
    int how;
} light_socket_shutdown_t;

typedef struct
{
    rte_atomic16_t ready_signal;//置1表示函数处理完了，可以取返回值了
    int return_value;
    int return_errno;

} light_return_value_t;

typedef struct
{
    int register_worker;                //worker process count
    rte_rwlock_t register_worker_lock;

    int worker_appids[MAX_WORKER];      // worker_id -> pid
    int worker_procids[MAX_WORKER];     // worker_id -> proc_id

    rte_rwlock_t workers_served_lock;
    int workers_served[MAX_CORES_NUM];  // proc_id -> num of workers served by stack proc_id
} light_worker_info_t;

typedef struct epoll_node
{
    int epoll_idx;
    uint32_t epoll_events;
    uint32_t ready_events;

    light_epoll_data_t ep_data;


    struct ll_elem entry;

    struct epoll_node *prev;
    struct epoll_node *next;
} light_epoll_node_t;//(aligned(4))

LL_HEAD(light_epoll_node_list, epoll_node);
LL_GENERATE(light_epoll_node_list, epoll_node, entry);



typedef struct
{
    int cmd;
    unsigned int ringset_idx;
    int          parent_idx;
    light_return_value_t *res;
    union
    {
        light_open_sock_cmd_t open_sock;
        light_socket_kick_cmd_t socket_kick_cmd;
        light_open_accepted_socket_t accepted_socket;
        light_set_socket_ring_cmd_t set_socket_ring;
        light_socket_ready_feedback_t socket_ready_feedback;
        light_socket_connect_bind_cmd_t socket_connect_bind;
        light_route_cmd_t route;
        light_setsockopt_cmd_t setsockopt;
        light_socket_shutdown_t socket_shutdown;
        light_decline_accepted_socket_t socket_decline;
		light_epoll_create_cmd_t epoll_create;
        light_epoll_close_cmd_t epoll_close;
        light_add_arp_cmd_t add_arp;
#ifdef CCJ_API
        light_socket_listen_cmd_t listen_backlog;	// We add listen backlog
#endif
    } u;
} light_cmd_t;

typedef struct
{
    unsigned long connection_idx; /* to be aligned */
    //rte_atomic16_t  read_ready_to_app; //erased by gjk_sem
    // rte_atomic16_t  write_ready_to_app; //erased by ljf
    rte_atomic16_t write_done_from_app;
    rte_atomic16_t occupied_mbuf_num; // the number of occupied mbufs. Note: it should decrease in many places, but NOW it ONLY updates at one place (Normally send packets).
    unsigned int local_ipaddr;
    unsigned short local_port;
    rte_atomic32_t tx_space;
    //add by gjk timeout mechanism
    struct timeval send_timeout;
    struct timeval receive_timeout;
    rte_atomic16_t is_nonblock;//置1表示非阻塞socket
    rte_atomic16_t connect_close_signal;
    /*removed by gjk_sem_04   **/
    int presented_in_chain;
    rte_rwlock_t ep_sock_lock;
    struct light_epoll_node_list epoll_node_list;// = LL_HEAD_INITIALIZER(epoll_node_list);
    rte_atomic16_t is_multicore;
    int multicore_socket_copies[MAX_CORES_NUM];
} light_socket_t;


typedef struct {
    int sockfd;
    int event;
} light_event_info_t;

typedef struct
{
    int true_fd;
    int used;
} fd_mapping_t;


extern struct rte_mempool *free_command_pool;
extern struct rte_mempool *free_command_pools[MAX_CORES_NUM];

extern struct rte_mempool *free_return_value_pool;
extern struct rte_mempool *free_return_value_pools[MAX_CORES_NUM];


extern struct rte_mempool *free_epoll_node_pool;

static inline light_epoll_node_t *light_get_free_epoll_node_buf()
{
    light_epoll_node_t *node;
    if (rte_mempool_get(free_epoll_node_pool, (void **)&node))
        return NULL;

    return node;
}

static inline light_return_value_t *light_get_free_return_value_buf()
{
    light_return_value_t *res;
    if (rte_mempool_get(free_return_value_pool, (void **)&res))
        return NULL;

    return res;
}

static inline light_cmd_t *light_get_free_return_value_buf_by_proc_id(int id)
{
    light_return_value_t *res;
    if (rte_mempool_get(free_return_value_pools[id], (void **)&res))
        return NULL;

    return res;
}

static inline light_cmd_t *light_get_free_command_buf()
{
    light_cmd_t *cmd;
    if (rte_mempool_get(free_command_pool, (void **)&cmd))
        return NULL;

    return cmd;
}

static inline light_cmd_t *light_get_free_command_buf_by_proc_id(int id)
{
    light_cmd_t *cmd;
    if (rte_mempool_get(free_command_pools[id], (void **)&cmd))
        return NULL;

    return cmd;
}

#endif /* __LIGHT_MEMORY_COMMON_H__ */
