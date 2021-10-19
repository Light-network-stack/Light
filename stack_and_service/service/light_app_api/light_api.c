#include "../../../light_debug.h"
#define _GNU_SOURCE
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <rte_atomic.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_memcpy.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include "../../light_common.h"
#include "light_ring_ops.h"
#include "light_api.h"
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <light_log.h>
#include <sched.h>
#include <sys/fcntl.h>
// #include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <dlfcn.h>
#include "../ll.h"

#ifndef EPOLL_SUPPORT
#define EPOLL_SUPPORT
#endif

#ifndef RETURN_VALUE_SUPPORT
#define RETURN_VALUE_SUPPORT
#endif

unsigned num_procs = 0;
local_socket_descriptor_t local_socket_descriptors[LIGHT_CONNECTION_POOL_SIZE];

struct rte_ring *rx_bufs_ring = NULL;
struct rte_ring *free_connections_ring = NULL;
struct rte_ring *free_clients_ring = NULL;
struct rte_ring *client_ring = NULL;
struct rte_ring *command_ring = NULL;
struct rte_ring *command_rings[MAX_CORES_NUM];
struct rte_ring *urgent_command_ring = NULL;
struct rte_ring *urgent_command_rings[MAX_CORES_NUM];
struct rte_ring *fd_mapping_ring = NULL;

struct rte_mempool *tx_bufs_pool = NULL;
struct rte_mempool *free_command_pool = NULL;
struct rte_mempool *free_return_value_pool = NULL;
struct rte_mempool *free_connections_pool = NULL;
struct rte_mempool *free_event_info_pool = NULL;
struct rte_mempool *free_command_pools[MAX_CORES_NUM];
struct rte_mempool *free_return_value_pools[MAX_CORES_NUM];
struct rte_mempool *free_epoll_node_pool = NULL;
struct rte_mempool *free_fd_mapping_storage_pool = NULL;

//group binding related
__thread int stack_proc_id;
int worker_id;
light_worker_info_t* workers_info;

// FD mapping
int full_true_fd[TRUE_FD_MAX_SIZE];
int light_to_true[TRUE_FD_MAX_SIZE];
int true_to_light[TRUE_FD_MAX_SIZE];
// fd_mapping_t fd_mapping_storage[TRUE_FD_NUM];

//store the original glibc api
void *libc_handle = NULL;
void *pthread_handle = NULL;
ssize_t (*real_read)(int fd, void *buf, size_t count);
int (*real_write)(int sockfd, const void *buf, size_t nbytes);
int (*real_epoll_create) (int size);
int (*real_epoll_ctl) (int epfd, int op, int fd, struct epoll_event *event);
int (*real_epoll_wait) (int epfd, struct epoll_event *events, int maxevents, int timeout);
int (*real_close) (int fd);
ssize_t (*real_writev)(int fd, const struct iovec *iov, int iovcnt);
int (*real_connect)(int sockfd,const struct sockaddr *serv_addr, socklen_t addrlen);
int (*real_socket)(int domain, int type, int protocol);
pid_t (*real_fork)();
int (*real_ioctl)(int, int, ...);
int (*real_fcntl)(int, int, ...);
int (*real_pthread_create)(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);

static inline int _add_epoll_event_to_shadow(light_epoll_t *, light_socket_t *, uint32_t);
static inline int _epoll_close(int sock);

typedef struct
{
    light_epoll_t *private_data;
    TAILQ_HEAD(socket_list_head, _local_socket_descriptor_) socket_list;
} epoll_t;


struct rte_ring *epoll_ring = NULL;

static epoll_t epolls[LIGHT_EPOLL_POOL_SIZE];

uint64_t light_stats_rx_kicks_sent = 0;
uint64_t light_stats_tx_kicks_sent = 0;
uint64_t light_stats_rx_full = 0;
uint64_t light_stats_rx_dequeued = 0;
uint64_t light_stats_rx_dequeued_local = 0;
uint64_t light_stats_tx_buf_allocation_failure = 0;
uint64_t light_stats_send_failure = 0;
uint64_t light_stats_recv_failure = 0;
uint64_t light_stats_buffers_sent = 0;
uint64_t light_stats_buffers_allocated = 0;
uint64_t light_stats_cannot_allocate_cmd = 0;
uint64_t light_stats_rx_returned = 0;
uint64_t light_stats_accepted = 0;
uint64_t light_stats_flush_local = 0;
uint64_t light_stats_flush = 0;
pthread_t stats_thread;
uint8_t g_print_stats_loop = 1;
uint32_t g_client_ringset_idx = LIGHT_CONNECTION_POOL_SIZE;
light_update_cbk_t light_update_cbk = NULL;

void print_stats()
{
    // light_log(LIGHT_LOG_INFO, "light_stats_receive_called %lu light_stats_send_called %lu \n\t\
    //             light_stats_rx_kicks_sent %lu light_stats_tx_kicks_sent %lu light_stats_cannot_allocate_cmd %lu  \n\t\
    //             light_stats_rx_full %lu light_stats_rx_dequeued %lu light_stats_rx_dequeued_local %lu \n\t\
    //             light_stats_tx_buf_allocation_failure %lu \n\t\
    //             light_stats_send_failure %lu light_stats_recv_failure %lu light_stats_buffers_sent %lu light_stats_buffers_allocated %lu light_stats_rx_dequeued %lu\n",
    //                  light_stats_receive_called, light_stats_send_called, light_stats_rx_kicks_sent,
    //                  light_stats_tx_kicks_sent, light_stats_cannot_allocate_cmd, light_stats_rx_full, light_stats_rx_dequeued,
    //                  light_stats_rx_dequeued_local, light_stats_tx_buf_allocation_failure,
    //                  light_stats_send_failure, light_stats_recv_failure,
    //                  light_stats_buffers_sent,
    //                  light_stats_buffers_allocated,
    //                  light_stats_rx_dequeued);
}

void print_rings_stats()
{
    int i;

    for (i = 0; i < LIGHT_CONNECTION_POOL_SIZE; i++)
    {
        if (local_socket_descriptors[i].socket
                && (rte_ring_count(local_socket_descriptors[i].rx_ring) || rte_ring_count(local_socket_descriptors[i].local_cache)))
        {
            light_log(LIGHT_LOG_INFO, "Connection #%d\n", i);
            light_log(LIGHT_LOG_INFO, "rx ring count %d tx ring count %d ",
                             rte_ring_count(local_socket_descriptors[i].rx_ring),
                             rte_ring_count(local_socket_descriptors[i].tx_ring));
            light_log(LIGHT_LOG_INFO, "local cache count %d ",
                             rte_ring_count(local_socket_descriptors[i].local_cache));
            // light_log(LIGHT_LOG_INFO, "read_ready  write_ready %d\n",
            //                  //         local_socket_descriptors[i].socket->read_ready_to_app.cnt, //erased by gjk_sem
            //                  local_socket_descriptors[i].socket->write_ready_to_app.cnt);
        }
    }
}


static inline void _light_free_command_buf_by_proc_id(light_cmd_t *cmd, int id)
{
    rte_mempool_put(free_command_pools[id], (void *)cmd);
}

static inline void _light_free_command_buf(light_cmd_t *cmd)
{
    rte_mempool_put(free_command_pool, (void *)cmd);
}

static inline void _light_free_return_value_buf_by_proc_id(light_return_value_t *res, int id)
{
    rte_mempool_put(free_return_value_pools[id], (void *)res);
}

static inline void _light_free_return_value_buf(light_return_value_t *res)
{
    rte_mempool_put(free_return_value_pool, (void *)res);
}

static inline void _light_free_epoll_node_buf(light_epoll_node_t *node)
{
    rte_mempool_put(free_epoll_node_pool, (void *)node);
}


void sig_handler(int signum)
{
    uint32_t i;
    // Todo: LIGHT_DISCONNECT_CLIENT cmd used to send to backend server, but how to get 
    // the specific command ring here in multicore scenario is a big deal.
    if (signum == SIGUSR1)
    {
//        puts("Here is interrupted.");
        /* T.B.D. do something to wake up the thread */
        return;
    }
    //light_set_log_level(0);
    light_log(LIGHT_LOG_INFO, "terminating on signal %d\n", signum);
    print_rings_stats();

    printf("signal !!!\n");
    for (i = 0; i < LIGHT_CONNECTION_POOL_SIZE; i++)
    {
        if (local_socket_descriptors[i].socket)
        {
            _light_close(local_socket_descriptors[i].socket->connection_idx);
        }
    }
    signal(signum, SIG_DFL);
    g_print_stats_loop = 0;
    print_stats();
    kill(getpid(), signum);
}

int app_crash_fd;
struct sockaddr_un app_crash_un;
char *app_crash_socket_path="/tmp/crash_detect.socket";

void app_crash_detect_init(){
    if ((app_crash_fd = real_socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        printf("socket error");
        exit(1);
    }

    memset(&app_crash_un, 0, sizeof(app_crash_un));
    app_crash_un.sun_family = AF_UNIX;
    strncpy(app_crash_un.sun_path, app_crash_socket_path, sizeof(app_crash_un.sun_path) - 1);

    if (real_connect(app_crash_fd, (struct sockaddr *) &app_crash_un, sizeof(app_crash_un)) < 0) {
        printf("connect error");
        exit(1);
    }
}

inline void fd_mapping_init(){
    int i;
    char ringname[1024];
    fd_mapping_t *fd_mapping_storage_pointer;
    printf("@@ FD mapping initialization starts...\n");
    // ringname = "FD_MAPPING_RING";
    for (i = 0; i < TRUE_FD_MAX_SIZE; i ++){
        full_true_fd[i] = -1;
        light_to_true[i] = -1;
        true_to_light[i] = -1;
    }

    free_fd_mapping_storage_pool = rte_mempool_lookup(FREE_FD_MAPPING_STORAGE_POOL_NAME);
    if (free_fd_mapping_storage_pool == NULL)
    {
        light_log(LIGHT_LOG_ERR, "cannot create free_fd_mapping_storage_pool\n", __FILE__, __LINE__);
        printf("cannot create free_fd_mapping_storage_pool\n");
        exit(0);
    }

    sprintf(ringname, "%s", "FD_MAPPING_RING");
    fd_mapping_ring = rte_ring_create(ringname, LIGHT_CONNECTION_POOL_SIZE, rte_socket_id(), 0);
    // printf("1\n");
    if (!fd_mapping_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot create fd_mapping_ring\n", __FILE__, __LINE__);
        printf("cannot create fd_mapping_ring\n");
        exit(0);
    }

    // printf("2\n");
    for (i = 0; i < TRUE_FD_NUM; i++){
        if (rte_mempool_get(free_fd_mapping_storage_pool, (void **)(&fd_mapping_storage_pointer)) != 0){
            light_log(LIGHT_LOG_ERR, "rte_mempool_get error\n", __FILE__, __LINE__);
            printf("rte_mempool_get error: %s(errno: %d)\n",strerror(errno),errno);  
            exit(0);
        }
        // fd_mapping_storage_pointer = &(fd_mapping_storage[i]);
        // printf("2.1\n");
        // (fd_mapping_storage[i]).true_fd = 666;
        // printf("2.2\n");
        fd_mapping_storage_pointer->true_fd = real_socket(AF_INET, SOCK_STREAM, 0);
        // printf("2.3\n");
        if (fd_mapping_storage_pointer->true_fd == -1){
            light_log(LIGHT_LOG_ERR, "create socket error\n", __FILE__, __LINE__);
            printf("create socket error: %s(errno: %d)\n",strerror(errno),errno);  
            exit(0);
        }
        full_true_fd[fd_mapping_storage_pointer->true_fd] = 1;
        fd_mapping_storage_pointer->used = 0;
        if ((rte_ring_enqueue(fd_mapping_ring, (void *)fd_mapping_storage_pointer)) != 0){
            light_log(LIGHT_LOG_ERR, "rte_ring_enqueue error\n", __FILE__, __LINE__);
            printf("rte_ring_enqueue error: %s(errno: %d)\n", strerror(errno), errno);  
            exit(0);
        }
        if((i == 0) || (i == TRUE_FD_NUM - 1)){
            printf("i = %d, fd_mapping_storage_pointer->true_fd = %d, fd_mapping_storage_pointer->used = %d\n", i, fd_mapping_storage_pointer->true_fd, fd_mapping_storage_pointer->used);
        }
    }
    printf("rte_mempool_in_use_count(free_fd_mapping_storage_pool) = %d\n", rte_mempool_in_use_count(free_fd_mapping_storage_pool));
    printf("rte_ring_count(fd_mapping_ring) = %u\n", rte_ring_count(fd_mapping_ring));
    // for test
    // rte_ring_dequeue(fd_mapping_ring, (void **)(&fd_mapping_storage_pointer));
    // printf("Test: take out a fd_mapping_storage, fd_mapping_storage_pointer->true_fd = %d, fd_mapping_storage_pointer->used = %d\n", fd_mapping_storage_pointer->true_fd, fd_mapping_storage_pointer->used);
    // printf("rte_ring_count(fd_mapping_ring) = %u\n", rte_ring_count(fd_mapping_ring));
    // rte_mempool_put(free_fd_mapping_storage_pool, (void *)fd_mapping_storage_pointer);
    // printf("rte_mempool_in_use_count(free_fd_mapping_storage_pool) = %d\n", rte_mempool_in_use_count(free_fd_mapping_storage_pool));
    // printf("3\n");
    
    printf("@@ FD mapping initialized.\n");
}

inline int is_light_fd(int *fd_num){

#ifdef API_DEBUG
    if ((*fd_num < 0)||(*fd_num > TRUE_FD_MAX_SIZE)){
        light_log(LIGHT_LOG_ERR, "invalid fd number\n", __FILE__, __LINE__);
        printf("invalid fd number: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
    printf("@ One Line of is_light_fd: full_true_fd[%d] = %d\n", *fd_num, full_true_fd[*fd_num]);
#endif

    if (full_true_fd[*fd_num] == 1){
        return 1; // is light fd
    } else {
        return 0; // is not light fd
    }
}

inline int light_fd_to_socket_epoll_index(int *fd_num, int *type_flag){
    int ret;
#ifdef API_DEBUG
    printf("@ Enter light_fd_to_socket_epoll_index\n");
    if ((*fd_num < 0)||(*fd_num > TRUE_FD_MAX_SIZE)){
        light_log(LIGHT_LOG_ERR, "invalid fd number\n", __FILE__, __LINE__);
        printf("invalid fd number: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif
    ret = true_to_light[*fd_num];

#ifdef API_DEBUG
    if (ret == -1){
        light_log(LIGHT_LOG_ERR, "no fd mapping record\n", __FILE__, __LINE__);
        printf("no fd mapping record: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif

    if (ret >= LIGHT_CONNECTION_POOL_SIZE){
        *type_flag = LIGHT_EPOLL_INDEX;
#ifdef API_DEBUG
        printf("  LIGHT_EPOLL: *type_flag = %d, true_to_light[%d] = %d\n", *type_flag, *fd_num, true_to_light[*fd_num]);
#endif
        return (ret - LIGHT_CONNECTION_POOL_SIZE);
    } else {
        *type_flag = LIGHT_SOCKET_INDEX;
#ifdef API_DEBUG
        printf("  LIGHT_SOCKET: *type_flag = %d, true_to_light[%d] = %d\n", *type_flag, *fd_num, true_to_light[*fd_num]);
#endif
        return (ret);
    }
}

inline int socket_epoll_index_to_light_fd(int *socket_epoll_index, int type_flag){
    // socket_epoll_index => light_index => light_fd
    
    // if (light_to_true[*light_index] == -1){
    //     light_log(LIGHT_LOG_ERR, "no fd mapping record\n", __FILE__, __LINE__);
    //     printf("no fd mapping record: %s(errno: %d)\n",strerror(errno),errno);  
    //     exit(0);
    // }
    int ret;
#ifdef API_DEBUG
    printf("@ Enter socket_epoll_index_to_light_fd, *socket_epoll_index = %d, type_flag = %d\n",  *socket_epoll_index, type_flag);

    if ((*socket_epoll_index < 0)||(*socket_epoll_index > LIGHT_CONNECTION_POOL_SIZE)){
        light_log(LIGHT_LOG_ERR, "invalid socket_epoll_index\n", __FILE__, __LINE__);
        printf("invalid socket_epoll_index: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif

    if (type_flag == LIGHT_SOCKET_INDEX){
        ret = light_to_true[*socket_epoll_index];
    } else if (type_flag == LIGHT_EPOLL_INDEX) {
        ret = light_to_true[*socket_epoll_index + LIGHT_CONNECTION_POOL_SIZE];
    } else {
        light_log(LIGHT_LOG_ERR, "invalid type_flag\n", __FILE__, __LINE__);
        printf("Error: invalid type_flag: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#ifdef API_DEBUG
    printf("  Return value = %d (socket_epoll_index_to_light_fd)\n", ret);
#endif
    return ret;
}

inline int get_new_light_fd(int *socket_epoll_index, int type_flag){
    int light_fd, ret;
    fd_mapping_t *fd_mapping_storage_pointer;
#ifdef API_DEBUG
    printf("@ Enter get_new_light_fd, type_flag = %d (0: socket; 1: epoll)\n", type_flag);
    if ((*socket_epoll_index < 0)||(*socket_epoll_index > LIGHT_CONNECTION_POOL_SIZE)){
        light_log(LIGHT_LOG_ERR, "invalid socket_epoll_index\n", __FILE__, __LINE__);
        printf("invalid socket_epoll_index: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif

    if (rte_ring_dequeue(fd_mapping_ring, (void **)(&fd_mapping_storage_pointer)) != 0) {
        printf("Error: rte_ring_dequeue(fd_mapping_ring, (void **)(&fd_mapping_storage_pointer)) != 0\n");
    }

#ifdef API_DEBUG
    printf("  Test: take out a fd_mapping_storage, fd_mapping_storage_pointer->true_fd = %d, fd_mapping_storage_pointer->used = %d\n", fd_mapping_storage_pointer->true_fd, fd_mapping_storage_pointer->used);
    printf("  rte_ring_count(fd_mapping_ring) = %u\n", rte_ring_count(fd_mapping_ring));
#endif
    light_fd = fd_mapping_storage_pointer->true_fd;
    rte_mempool_put(free_fd_mapping_storage_pool, (void *)fd_mapping_storage_pointer);
#ifdef API_DEBUG
    printf("  rte_mempool_in_use_count(free_fd_mapping_storage_pool) = %d\n", rte_mempool_in_use_count(free_fd_mapping_storage_pool));
#endif
    if (type_flag == LIGHT_SOCKET_INDEX){
        light_to_true[*socket_epoll_index] = light_fd;
        true_to_light[light_fd] = *socket_epoll_index;
#ifdef API_DEBUG
        printf("  light_to_true[%d] = %d, true_to_light[%d] = %d\n", *socket_epoll_index, light_to_true[*socket_epoll_index], light_fd, true_to_light[light_fd]);
#endif
        ret = light_fd;
    } else if (type_flag == LIGHT_EPOLL_INDEX) {
        light_to_true[*socket_epoll_index + LIGHT_CONNECTION_POOL_SIZE] = light_fd;
        true_to_light[light_fd] = *socket_epoll_index + LIGHT_CONNECTION_POOL_SIZE;
#ifdef API_DEBUG
        printf("  light_to_true[%d] = %d, true_to_light[%d] = %d\n", *socket_epoll_index + LIGHT_CONNECTION_POOL_SIZE, light_to_true[*socket_epoll_index + LIGHT_CONNECTION_POOL_SIZE], light_fd, true_to_light[light_fd]);
#endif
        ret = light_fd;
    } else {
        light_log(LIGHT_LOG_ERR, "invalid type_flag\n", __FILE__, __LINE__);
        printf("Error: in get_new_light_fd, invalid type_flag: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#ifdef API_DEBUG
    printf("  Return value = %d (get_new_light_fd)\n", ret);
#endif
    return ret;
}

inline int put_back_light_fd(int *light_fd){
    fd_mapping_t *fd_mapping_storage_pointer;
    int light_index;

#ifdef API_DEBUG
    printf("@ Enter put_back_light_fd\n");

    if ((*light_fd < 0)||(*light_fd > TRUE_FD_MAX_SIZE)){
        light_log(LIGHT_LOG_ERR, "invalid fd number\n", __FILE__, __LINE__);
        printf("invalid fd number: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif

    if (rte_mempool_get(free_fd_mapping_storage_pool, (void **)(&fd_mapping_storage_pointer)) != 0){
            light_log(LIGHT_LOG_ERR, "rte_mempool_get error\n", __FILE__, __LINE__);
            printf("rte_mempool_get error: %s(errno: %d)\n",strerror(errno),errno);  
            exit(0);
    }
    
    fd_mapping_storage_pointer->true_fd = *light_fd;
    
    if ((rte_ring_enqueue(fd_mapping_ring, (void *)fd_mapping_storage_pointer)) != 0){
        light_log(LIGHT_LOG_ERR, "rte_ring_enqueue error\n", __FILE__, __LINE__);
        printf("rte_ring_enqueue error: %s(errno: %d)\n", strerror(errno), errno);  
        exit(0);
    }
#ifdef API_DEBUG
    printf("  rte_ring_count(fd_mapping_ring) = %u\n", rte_ring_count(fd_mapping_ring));
    printf("  rte_mempool_in_use_count(free_fd_mapping_storage_pool) = %d\n", rte_mempool_in_use_count(free_fd_mapping_storage_pool));
#endif

    light_index = true_to_light[*light_fd];
    true_to_light[*light_fd] = -1;
    light_to_true[light_index] = -1;
#ifdef API_DEBUG
    printf("  light_to_true[%d] = %d, true_to_light[%d] = %d\n", light_index, light_to_true[light_index], *light_fd, true_to_light[*light_fd]);
    printf("@@ End of put_back_light_fd\n");
#endif

    return 0;
}

/* must be called per app */
inline int light_app_init(int argc, char **argv, char *app_unique_id, int log_level)
{
    printf("@ Enter light_app_init\n");

    int i;
    char ringname[1024];
    rte_cpuset_t cpuset;
    unsigned cpu;
    srand((unsigned)time(NULL));

#ifdef APP_LOG_ENABLE
    light_log_init(LOG_TO_SYSLOG);
#endif
    light_set_log_level(log_level);
    if (rte_eal_init(argc, argv) < 0)
    {
        light_log(LIGHT_LOG_ERR, "cannot initialize rte_eal");
        return -1;
    }
    light_log(LIGHT_LOG_INFO, "EAL initialized\n");
    light_log(LIGHT_LOG_INFO, "num_procs = %u\n", num_procs);

    printf("sizeof(local_socket_descriptor_t) = %d\n", sizeof(local_socket_descriptor_t));
    printf("sizeof(epoll_t) = %d\n", sizeof(epoll_t));
    printf("sizeof(light_epoll_t) = %d\n", sizeof(light_epoll_t));
    printf("sizeof(socket) = %d\n", sizeof(socket));
    printf("sizeof(int) = %d\n", sizeof(int));

    printf("Please Wait... ...\n");

    free_clients_ring = rte_ring_lookup(FREE_CLIENTS_RING);
    if (!free_clients_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot find ring %s %d\n", __FILE__, __LINE__);
        exit(0);
    }

    free_connections_ring = rte_ring_lookup(FREE_CONNECTIONS_RING);
    if (!free_connections_ring)
    {
        light_log(LIGHT_LOG_ERR, "cannot find free connections ring\n");
        return -1;
    }

    free_connections_pool = rte_mempool_lookup(FREE_CONNECTIONS_POOL_NAME);
    if (!free_connections_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot find free connections pool\n");
        return -1;
    }

    free_event_info_pool = rte_mempool_lookup(FREE_EVENT_INFO_POOL_NAME);
    if (!free_event_info_pool) {
        light_log(LIGHT_LOG_ERR, "cannot find free event info pool\n");
        return -1;
    }

    memset(local_socket_descriptors, 0, sizeof(local_socket_descriptors));
    for (i = 0; i < LIGHT_CONNECTION_POOL_SIZE; i++)
    {
        sprintf(ringname, RX_RING_NAME_BASE"%d", i);
        local_socket_descriptors[i].rx_ring = rte_ring_lookup(ringname);
        if (!local_socket_descriptors[i].rx_ring)
        {
            light_log(LIGHT_LOG_ERR, "%s %d\n", __FILE__, __LINE__);
            exit(0);
        }

        sprintf(ringname, RX_RING_NAME_BASE"%d", i);
        local_socket_descriptors[i].local_cache = rte_ring_lookup(ringname);
        if (!local_socket_descriptors[i].local_cache)
        {
        	printf("  Error: can not find local_cache\n");
            light_log(LIGHT_LOG_ERR, "can not find local_cache, %s %d\n", __FILE__, __LINE__);
            exit(0);
        }

        sprintf(ringname, TX_RING_NAME_BASE"%d", i);
        local_socket_descriptors[i].tx_ring = rte_ring_lookup(ringname);
        if (!local_socket_descriptors[i].tx_ring)
        {
            light_log(LIGHT_LOG_ERR, "%s %d\n", __FILE__, __LINE__);
            exit(0);
        }
        local_socket_descriptors[i].socket = NULL;

        // sprintf(ringname, "lrxcache%s_%d", app_unique_id, i);
        // local_socket_descriptors[i].local_cache = rte_ring_create(ringname, 16384, rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
        // if (!local_socket_descriptors[i].local_cache)
        // {
        //     light_log(LIGHT_LOG_WARNING, "cannot create local cache\n");
        //     local_socket_descriptors[i].local_cache = rte_ring_lookup(ringname);
        //     if (!local_socket_descriptors[i].local_cache)
        //     {
        //         light_log(LIGHT_LOG_ERR, "and cannot find\n");
        //         exit(0);
        //     }
        // }

        local_socket_descriptors[i].any_event_received = 0;
    }

    tx_bufs_pool = rte_mempool_lookup("mbufs_mempool");
    if (!tx_bufs_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot find tx bufs pool\n");
        return -1;
    }

    for (i = 0; i < MAX_CORES_NUM; ++i)
    {
        sprintf(ringname, "CMD_POOL%d", i);
        free_command_pools[i] = rte_mempool_lookup(ringname);
        if (!free_command_pools[i])
        {
            num_procs = i > 0 ? i : 0;
            if (num_procs == 0)
            {
                light_log(LIGHT_LOG_ERR, "cannot find free command pool in core %d\n", i);
                return -1;
            }
            break;
        }
    }

    for ( i = 0; i < num_procs; ++i)
    {
        sprintf(ringname, "RETURN_VALUE%d", i);
        free_return_value_pools[i] = rte_mempool_lookup(ringname);
        if (!free_return_value_pools[i])
        {
            light_log(LIGHT_LOG_ERR, "cannot find return value pool\n");
            return -1;
        }
    }

    free_epoll_node_pool = rte_mempool_lookup(FREE_EPOLL_NODE_POOL_NAME);
    if (!free_epoll_node_pool)
    {
        light_log(LIGHT_LOG_ERR, "cannot find epoll node pool\n");
        return -1;
    }

    for ( i = 0; i < num_procs; ++i)
    {
        sprintf(ringname, "COMMAND_RING%d", i);
        command_rings[i] = rte_ring_lookup(ringname);
        if (!command_rings[i])
        {
            light_log(LIGHT_LOG_ERR, "cannot find command ring\n");
            return -1;
        }
    }

    for ( i = 0; i < num_procs; ++i)
    {
        sprintf(ringname, "URGENT_COMMAND_RING%d", i);
        urgent_command_rings[i] = rte_ring_lookup(ringname);
        if (!urgent_command_rings[i])
        {
            light_log(LIGHT_LOG_ERR, "cannot find urgent command ring\n");
            return -1;
        }
    }

    epoll_ring = rte_ring_lookup(LIGHT_EPOLL_RING_NAME);
    for (i = 0; i < LIGHT_EPOLL_POOL_SIZE; ++i)
    {
        epolls[i].private_data = NULL;
        TAILQ_INIT(&epolls[i].socket_list);
    }

    struct rte_mempool *workers_info_pool = rte_mempool_lookup("WORKER_INFO");
    int res;
    do{
        res = rte_mempool_get(workers_info_pool, (void**)&workers_info);
        if(res == 0){
            update_worker_info();
            rte_mempool_put(workers_info_pool, (void*)workers_info);
        }
    }while(res != 0);

    //use dlopen and dlsym to store the original api
    libc_handle = dlopen("libc.so.6", RTLD_LAZY);
    if (!libc_handle)
    {
        light_log(LIGHT_LOG_ERR, "libc_handle dlopen fail %s %d\n", __FILE__, __LINE__);
        return -1;
    }

    pthread_handle = dlopen("libpthread-2.23.so", RTLD_LAZY);
    if (!pthread_handle)
    {
        fprintf(stderr, "pthread_handle dlopen fail\n");
        return -1;
    }

    real_read = dlsym(libc_handle, "read");
    real_epoll_create = dlsym(libc_handle, "epoll_create");
    real_epoll_wait = dlsym(libc_handle, "epoll_wait");
    real_epoll_ctl = dlsym(libc_handle, "epoll_ctl");
    real_close = dlsym(libc_handle, "close");
    real_writev = dlsym(libc_handle, "writev");
    real_write = dlsym(libc_handle, "write");
    real_connect= dlsym(libc_handle, "connect");
    real_socket= dlsym(libc_handle, "socket");
    real_fork = dlsym(libc_handle, "fork");
    real_ioctl = dlsym(libc_handle, "ioctl");
    real_fcntl = dlsym(libc_handle, "fcntl");
    real_pthread_create = dlsym(pthread_handle, "pthread_create");

    signal(SIGHUP, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGILL, sig_handler);
    signal(SIGABRT, sig_handler);
    signal(SIGFPE, sig_handler);
    signal(SIGFPE, sig_handler);
    signal(SIGSEGV, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sig_handler);

    app_crash_detect_init();
    #ifdef LIGHT_FD_MAPPING
        // must be after real API
        fd_mapping_init();
    #endif

    printf("**** light_app_init finish! ****\n");
    
    return ((tx_bufs_pool == NULL) || (command_rings[0] == NULL) || (urgent_command_rings[0] == NULL) || (free_command_pools[0] == NULL) || (free_return_value_pools[0] == NULL) || (free_epoll_node_pool == NULL));
}

// Todo: what this function for
inline int _light_create_client(light_update_cbk_t update_cbk)
{
    int ringset_idx;
    light_cmd_t *cmd;
    char ringname[1024];

    if (rte_ring_dequeue(free_clients_ring, (void **)&ringset_idx))
    {
        printf("Error: rte_ring_dequeue(free_clients_ring, (void **)&ringset_idx) != 0\n");
        light_log(LIGHT_LOG_ERR, "%s %d\n", __FILE__, __LINE__);
        return -1;
    }
    sprintf(ringname, "%s%d", FREE_CLIENTS_RING, ringset_idx);
    client_ring = rte_ring_lookup(ringname);
    if (!client_ring)
    {
        light_log(LIGHT_LOG_ERR, "%s %d\n", __FILE__, __LINE__);
        return -2;
    }
    cmd = light_get_free_command_buf();
    if (!cmd)
    {
        light_stats_cannot_allocate_cmd++;
        return -3;
    }

    cmd->cmd = LIGHT_CONNECT_CLIENT;
    cmd->ringset_idx = ringset_idx;
    if (light_enqueue_command_buf(cmd))
    {
        _light_free_command_buf(cmd);
        return -3;
    }
    g_client_ringset_idx = (uint32_t)ringset_idx;
    light_update_cbk = update_cbk;
    return 0;
}

inline int _light_read_updates(void)
{
    struct rte_mbuf *mbuf = NULL;
    unsigned char cmd = 0;

    if (rte_ring_dequeue(client_ring, (void **)&mbuf))
    {
        printf("Error: rte_ring_dequeue(client_ring, (void **)&mbuf) != 0\n");
        //light_log(LIGHT_LOG_ERR,"%s %d\n",__FILE__,__LINE__);
        return -1;
    }
    unsigned char *p = rte_pktmbuf_mtod(mbuf, unsigned char *);
    switch (*p)
    {
    case LIGHT_NEW_IFACES:
        if (light_update_cbk)
        {
            cmd = 1;
            p++;
            light_update_cbk(cmd, p, rte_pktmbuf_data_len(mbuf) - 1);
        }
        break;
    case LIGHT_NEW_ADDRESSES:
        if (light_update_cbk)
        {
            cmd = 3;
            p++;
            light_update_cbk(cmd, p, rte_pktmbuf_data_len(mbuf) - 1);
        }
        break;
    case LIGHT_END_OF_RECORD:
        return 0;
    }
    return -1;
}

static inline void _reset_local_socket_data(light_socket_t *pSocket)
{
    int i;
    light_epoll_node_t *o, *next_o = NULL;
    int N = LL_SIZE(light_epoll_node_list, &(pSocket->epoll_node_list));
    if ( N != 0)
    {
        for (i = 0, o = LL_FIRST(light_epoll_node_list, &(pSocket->epoll_node_list));
                i < N;
                i++, o = next_o)
        {
            next_o = LL_NEXT(light_epoll_node_list, &(pSocket->epoll_node_list), o);
            LL_UNLINK(light_epoll_node_list, &(pSocket->epoll_node_list), o);
        }
    }
}

static inline void _light_reset_local_descriptor(int connection_idx) // 这里没初始化multicore_socket_copies，因为不影响
{
    local_socket_descriptors[connection_idx].is_init = 0;
    local_socket_descriptors[connection_idx].is_set_sockopt = 0;
    local_socket_descriptors[connection_idx].local_mask = 0;
    local_socket_descriptors[connection_idx].present_in_ready_cache = 0;
    local_socket_descriptors[connection_idx].shadow_len_delievered = 0;
    local_socket_descriptors[connection_idx].shadow_len_remainder = 0;
    local_socket_descriptors[connection_idx].shadow = NULL;
    local_socket_descriptors[connection_idx].shadow_next = NULL;
    memset(&local_socket_descriptors[connection_idx].local_ready_cache_entry,
           0,
           sizeof(local_socket_descriptors[connection_idx].local_ready_cache_entry));
    memset(&local_socket_descriptors[connection_idx].socket_list_entry, 0,
           sizeof(local_socket_descriptors[connection_idx].socket_list_entry));
}


inline int light_open_socket(int family, int type, int protocol) // semi POSIX API
{
    int light_fd;
    int sock = _light_get_socket(family, type, protocol); // light socket index
#ifdef API_DEBUG
    printf("In light_open_socket: (Before mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    return sock < 0 ? sock : (INT_MAX - sock);
#endif

#ifdef LIGHT_FD_MAPPING
    light_fd = get_new_light_fd(&sock, LIGHT_SOCKET_INDEX);
#ifdef API_DEBUG
    printf("In light_open_socket: (After mapping) light_fd = %d\n", light_fd);
#endif
    return sock < 0 ? sock : light_fd;
#endif
}


inline int _light_get_socket(int family, int type, int protocol)
{
    light_socket_t *light_socket;
    int sock;
    
    if (rte_ring_dequeue(free_connections_ring, (void **)&light_socket))
    {
        printf("Error: rte_ring_dequeue(free_connections_ring, (void **)&light_socket) != 0\n");
        light_log(LIGHT_LOG_ERR, "can't get free socket line=%d\n",__LINE__);
        return -1;
    }
    sock = light_socket->connection_idx; // light socket index
    rte_atomic16_set( &(light_socket->is_multicore), 0);

    //只在本地初始化，不去协议栈初始化
    local_socket_descriptors[sock].socket = light_socket;
    local_socket_descriptors[sock].family = family;
    local_socket_descriptors[sock].type = type;
    local_socket_descriptors[sock].protocol = protocol;
    _light_reset_local_descriptor(sock);
#ifdef API_DEBUG
    printf("In _light_get_socket: socket index = %d\n", sock);
#endif

    return sock; // light socket index
}


//此函数不将is_nonblock置0
inline int _light_socket_init(int sock, int proc_id)
{
    int light_fd;
    if (sock < 0)
        return -1;
    light_socket_t *light_socket = local_socket_descriptors[sock].socket;
    light_cmd_t *cmd;

#ifdef RETURN_VALUE_SUPPORT
    light_return_value_t *res;
#endif
    /* allocate a ringset (cmd/tx/rx) here */
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return -2;
    }
    cmd->cmd = LIGHT_OPEN_SOCKET_COMMAND;
    cmd->ringset_idx = light_socket->connection_idx;
    cmd->u.open_sock.family = local_socket_descriptors[sock].family;
    cmd->parent_idx = -1;
    cmd->u.open_sock.type = local_socket_descriptors[sock].type;
    cmd->u.open_sock.pid = getpid();

#ifdef RETURN_VALUE_SUPPORT
    res = light_get_free_return_value_buf_by_proc_id(proc_id);
    if (!res)
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
        light_stats_cannot_allocate_cmd++;
        if (rte_ring_enqueue(free_connections_ring, (void *)light_socket) != 0) {
            printf("rte_ring_enqueue(free_connections_ring, (void *)light_socket) != 0\n");
        }
        //light_close(light_socket->connection_idx);
        return -2;
    }
    cmd->res = res;
    rte_atomic16_set( &(res->ready_signal), 0);
#endif
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
#ifdef RETURN_VALUE_SUPPORT
        _light_free_return_value_buf_by_proc_id(res, proc_id);
#endif
        return -3;
    }
    //-1 means unlimited
    light_socket->send_timeout.tv_sec = -1;
    light_socket->send_timeout.tv_usec = -1;
    light_socket->receive_timeout.tv_sec = -1;
    light_socket->receive_timeout.tv_usec = -1;
    _reset_local_socket_data(light_socket);
    struct light_epoll_node_list* list = &(light_socket->epoll_node_list);
    list->ll_head.size = (size_t)0;
    list->ll_head.q.succ = (uintptr_t) & (list->ll_head);
    list->ll_head.q.pred = (uintptr_t) & (list->ll_head);
    list->ll_head.q.refcnt = (size_t)2;
    rte_atomic16_set( &(light_socket->connect_close_signal), 0);
    //light_app_init初始化的时候为0，在close的时候也设置为0，此处不设置
    //rte_atomic16_set( &(light_socket->is_nonblock), 0);
#ifdef RETURN_VALUE_SUPPORT
    while (!rte_atomic16_read( &(res->ready_signal)))
    {
        rte_pause();
    }
    if (res->return_value != 0)
    {
        _light_free_return_value_buf_by_proc_id(res, proc_id);
        _light_close(light_socket->connection_idx); // By Ljf
        return -1;
    }
    _light_free_return_value_buf_by_proc_id(res, proc_id);
#endif
    local_socket_descriptors[sock].is_init = 1;

#ifdef TWO_SIDE_FD_SEPARATION
    light_setsockopt(INT_MAX - sock, local_socket_descriptors[sock].level, local_socket_descriptors[sock].optname, &(local_socket_descriptors[sock].optval), local_socket_descriptors[sock].optlen);
#endif

#ifdef LIGHT_FD_MAPPING
    light_fd = socket_epoll_index_to_light_fd(&sock, LIGHT_SOCKET_INDEX);
    light_setsockopt(light_fd, local_socket_descriptors[sock].level, local_socket_descriptors[sock].optname, &(local_socket_descriptors[sock].optval), local_socket_descriptors[sock].optlen);
#endif    
    return 0;
}


//for errors caused by light-related operations
#define LIGHT_ERRNO 10000

//bind就必须是multicore socket，如果是multicore，必须去取multicore_socket_copies[0]
inline int light_bind(int sock, struct sockaddr *addr, __rte_unused int addrlen)
{
    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_bind, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sock = %d\n", sock);
#endif

    int i, j;
    rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_multicore), 1);
    local_socket_descriptors[sock].socket->multicore_socket_copies[0] = sock;

    local_socket_descriptors[sock].proc_id = 0;
    if (_light_socket_init(sock, 0) != 0)
    {
        light_log(LIGHT_LOG_ERR, "socket init failure line=%d\n", __LINE__);
        return -1;
    }
    int is_nonblock = rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) );
    light_log(LIGHT_LOG_INFO, "in bind set is_nonblock(%d)", is_nonblock);
    for ( i = 1; i < num_procs; ++i)
    {
        int copied_sock = _light_get_socket(local_socket_descriptors[sock].family, local_socket_descriptors[sock].type, local_socket_descriptors[sock].protocol);
        if (_light_socket_init(copied_sock, i) != 0)
        {
            //失败的话去multicore化，只留0号socket
            light_log(LIGHT_LOG_ERR, "socket init failure line=%d\n", __LINE__);
            for ( j = 1; j < i; ++j)
            {
                _light_close(local_socket_descriptors[sock].socket->multicore_socket_copies[j]); //By Ljf
            }
            rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_multicore), 0);
            return -1;
        }
        local_socket_descriptors[sock].socket->multicore_socket_copies[i] = copied_sock;
        local_socket_descriptors[copied_sock].proc_id = i;
        rte_atomic16_set( &(local_socket_descriptors[copied_sock].socket->is_multicore), 1);
        local_socket_descriptors[copied_sock].socket->multicore_socket_copies[0] = sock;
        //设置从核的is_nonblock选项
        rte_atomic16_set( &(local_socket_descriptors[copied_sock].socket->is_nonblock), is_nonblock);
        light_log(LIGHT_LOG_INFO, "in bind set copied_sock(%d) is_nonblock(%d)",copied_sock,is_nonblock);
    }
    //让从核的listenfd的multicore_socket_copies
    for(i = 1; i <num_procs; ++i)
    {
        int copied_sock = local_socket_descriptors[sock].socket->multicore_socket_copies[i];
        int j;
        for(j = 1; j <num_procs; ++j)
        {
            local_socket_descriptors[copied_sock].socket->multicore_socket_copies[j] = 
                local_socket_descriptors[sock].socket->multicore_socket_copies[j];
        }
    }
    for (i = 0; i < num_procs; ++i)
    {
        int res = _light_bind(local_socket_descriptors[sock].socket->multicore_socket_copies[i], addr, addrlen, i);
        if ( res != 0)
        {
            light_log(LIGHT_LOG_ERR, "_internal_bind failure line=%d\n", __LINE__);
            return res;
        }
    }
    return 0;
}

inline int _light_bind(int sock, struct sockaddr *addr, __rte_unused int addrlen, int proc_id)
{
    light_cmd_t *cmd;
    struct sockaddr_in *sockaddr_in = (struct sockaddr_in *)addr;

#ifdef RETURN_VALUE_SUPPORT
    light_return_value_t *res;
    errno = LIGHT_ERRNO;
#endif
    if (sockaddr_in->sin_family != AF_INET) /*only IPv4 for now*/
        return -1;
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return -1;
    }
    cmd->cmd = LIGHT_SOCKET_CONNECT_BIND_COMMAND;
    cmd->ringset_idx = sock;
    cmd->parent_idx = -1;
    cmd->u.socket_connect_bind.ipaddr = sockaddr_in->sin_addr.s_addr;
    cmd->u.socket_connect_bind.port = sockaddr_in->sin_port;
    cmd->u.socket_connect_bind.is_connect = 0;

#ifdef RETURN_VALUE_SUPPORT
    res = light_get_free_return_value_buf_by_proc_id(proc_id);
    if (!res)
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
        light_stats_cannot_allocate_cmd++;
        return -2;
    }
    cmd->res = res;
    rte_atomic16_set( &(res->ready_signal), 0);
#endif
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
#ifdef RETURN_VALUE_SUPPORT
        _light_free_return_value_buf_by_proc_id(res, proc_id);
#endif
        return -2;
    }

    local_socket_descriptors[sock].local_ipaddr = sockaddr_in->sin_addr.s_addr;
    local_socket_descriptors[sock].local_port = sockaddr_in->sin_port;

#ifdef RETURN_VALUE_SUPPORT
    while (!rte_atomic16_read( &(res->ready_signal)))
    {
        rte_pause();
    }
    errno = res->return_errno;
    if (res->return_value != 0)
    {
        //do something when bind fail
        _light_free_return_value_buf_by_proc_id(res, proc_id);
        return -1;
    }
    _light_free_return_value_buf_by_proc_id(res, proc_id);
#endif
    return 0;
}

// Todo: connect has free_command_pool operation bug
inline int light_connect(int sock, struct sockaddr *addr, __rte_unused int addrlen)
{
    //注意multicore copy的[0]应该为真的copy id
    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_connect, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sock = %d\n", sock);
#endif

    light_cmd_t *cmd;

    struct sockaddr_in *sockaddr_in = (struct sockaddr_in *)addr;

    //add by ld
#ifdef RETURN_VALUE_SUPPORT
    light_return_value_t *res;
    errno = LIGHT_ERRNO;
#endif


    if (sockaddr_in->sin_family != AF_INET) /*only IPv4 for now*/
        return -1;
    cmd = light_get_free_command_buf();
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return -2;
    }
    cmd->cmd = LIGHT_SOCKET_CONNECT_BIND_COMMAND;
    cmd->ringset_idx = sock;
    cmd->parent_idx = -1;
    cmd->u.socket_connect_bind.ipaddr = sockaddr_in->sin_addr.s_addr;
    cmd->u.socket_connect_bind.port = sockaddr_in->sin_port;
    cmd->u.socket_connect_bind.is_connect = 1;

#ifdef RETURN_VALUE_SUPPORT
    //add by ld
    res = light_get_free_return_value_buf();
    if (!res)
    {
        _light_free_command_buf(cmd);
        light_stats_cannot_allocate_cmd++;
        return -2;
    }
    cmd->res = res;
    rte_atomic16_set( &(res->ready_signal), 0);
#endif

    if (light_enqueue_command_buf(cmd))
    {
        _light_free_command_buf(cmd);
#ifdef RETURN_VALUE_SUPPORT
        _light_free_return_value_buf(res);
#endif
        return -2;
    }
    else
    {
        /*
         * add by gjk_sem_03
         */
        int time_wait = -1; //调通后作为connect接口的一个参数,-1代表无限超时等待

        //modified by ld
        int isblocked = rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) );

        if (isblocked == 0)
        {
            //非阻塞connect
            while (1)
            {
                int16_t connect_signal = rte_atomic16_read( &(local_socket_descriptors[sock].socket->connect_close_signal) );
                if (connect_signal == 1 || connect_signal == 2 )
                {
                    //当前确实是非阻塞connect接口
                    //根据POSIX接口标准，connect连接成功，返回0
                    //当然，要把connect_signal 再置回默认初始值，0
                    //                  printf("Unblocked Connect Succeed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
                    local_socket_descriptors[sock].remote_ipaddr = sockaddr_in->sin_addr.s_addr;
                    local_socket_descriptors[sock].remote_port = sockaddr_in->sin_port;
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    errno = EINPROGRESS;
                    return -1;
#endif
                }
                else if (connect_signal == 0)
                {
                    //默认值，继续
                    continue;
                }
                else
                {
                    //非阻塞connect都失败了
                    //                  printf("Unblocked Connect Failed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    return -1;
#endif
                }
            }
        }
        else
        {
            //阻塞式connect

            //int time_cnt=0;
            //for(time_cnt=0;(time_cnt<time_wait || time_wait==-1);time_cnt++){

            struct timeval time_begin, time_end, time_use;
            gettimeofday(&time_begin, NULL);
            //for dcebug
            //local_socket_descriptors[sock].socket->send_timeout.tv_sec=10;
            //local_socket_descriptors[sock].socket->send_timeout.tv_usec=0;

            while (1)
            {
                //printf("time_cnt=%d  time_wait=%d\n",time_cnt, time_wait);
                gettimeofday(&time_end, NULL);
                int res = time_substract(&time_begin, &time_end, &time_use);
                if (res != 0)
                {
                    continue;
                }
                else
                {
                    if ( local_socket_descriptors[sock].socket->send_timeout.tv_sec >= 0
                            && local_socket_descriptors[sock].socket->send_timeout.tv_usec >= 0
                            && time_cmp(&time_use, &(local_socket_descriptors[sock].socket->send_timeout) ) > 0)
                    {
                        break;
                    }
                }
//              sleep(1);
                rte_pause();
                //add by gjk_sem_03
                int16_t connect_signal = rte_atomic16_read( &(local_socket_descriptors[sock].socket->connect_close_signal) );
                if ( 0 == connect_signal )
                {
                    //connect信号还是默认值，故我们还不知道连接是成功还是失败，继续等
                    continue;
                }
                else if (1 == connect_signal)
                {
                    //当前是阻塞connect接口，1(SYN_SENT还不能结束，继续等)
                    continue;

                }
                else if ( 2 ==  connect_signal )
                {
                    // 阻塞connect 成功了
                    //根据POSIX接口标准，connect连接成功返回0
                    //当然，要把connect_signal 再置回默认初始值，0
                    //                  printf("blocked Connect Succeed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
                    local_socket_descriptors[sock].remote_ipaddr = sockaddr_in->sin_addr.s_addr;
                    local_socket_descriptors[sock].remote_port = sockaddr_in->sin_port;
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    return 0;
#endif
                }
                else
                {
                    //connect失败了
                    //                 printf("blocked Connect Failed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    return -1;
#endif
                    //return -3;

                }
            }
            //如果运行到这里，说明超时了，也是没连接成功，
            rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
#ifndef RETURN_VALUE_SUPPORT
            return -1;
#endif
        }


#ifdef RETURN_VALUE_SUPPORT
        //add by ld
        //waiting return value
        while (!rte_atomic16_read( &(res->ready_signal)))
        {
        }
        //set errno
        errno = res->return_errno;
//       printf("connect errno:%d\n", errno);
//       printf("connect return value:%d\n", res->return_value);
        int rv = res->return_value;
        _light_free_return_value_buf(res);
        return rv;
#else
        return 0;
#endif

    }

    //return 0;
}

#if 0
inline int _light_connect(int sock, struct sockaddr *addr, __rte_unused int addrlen, int proc_id) // not used
{
    //sock = INT_MAX - sock;

    light_cmd_t *cmd;

    struct sockaddr_in *sockaddr_in = (struct sockaddr_in *)addr;

    //add by ld
#ifdef RETURN_VALUE_SUPPORT
    light_return_value_t *res;
    errno = LIGHT_ERRNO;
#endif


    if (sockaddr_in->sin_family != AF_INET) /*only IPv4 for now*/
        return -1;
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return -2;
    }
    cmd->cmd = LIGHT_SOCKET_CONNECT_BIND_COMMAND;
    cmd->ringset_idx = sock;
    cmd->parent_idx = -1;
    cmd->u.socket_connect_bind.ipaddr = sockaddr_in->sin_addr.s_addr;
    cmd->u.socket_connect_bind.port = sockaddr_in->sin_port;
    cmd->u.socket_connect_bind.is_connect = 1;

#ifdef RETURN_VALUE_SUPPORT
    //add by ld
    res = light_get_free_return_value_buf_by_proc_id(proc_id);
    if (!res)
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
        light_stats_cannot_allocate_cmd++;
        return -2;
    }
    cmd->res = res;
    rte_atomic16_set( &(res->ready_signal), 0);
#endif

    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
#ifdef RETURN_VALUE_SUPPORT
        _light_free_return_value_buf_by_proc_id(res, proc_id);
#endif
        return -2;
    }
    else
    {
        /*
         * add by gjk_sem_03
         */
        int time_wait = -1; //调通后作为connect接口的一个参数,-1代表无限超时等待

        //modified by ld
        int isblocked = rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) );

        if (isblocked == 0)
        {
            //非阻塞connect
            while (1)
            {
                int16_t connect_signal = rte_atomic16_read( &(local_socket_descriptors[sock].socket->connect_close_signal) );
                if (connect_signal == 1 || connect_signal == 2 )
                {
                    //当前确实是非阻塞connect接口
                    //根据POSIX接口标准，connect连接成功，返回0
                    //当然，要把connect_signal 再置回默认初始值，0
//                   printf("Unblocked Connect Succeed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
                    local_socket_descriptors[sock].remote_ipaddr = sockaddr_in->sin_addr.s_addr;
                    local_socket_descriptors[sock].remote_port = sockaddr_in->sin_port;
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    errno = EINPROGRESS;
                    return -1;
#endif
                }
                else if (connect_signal == 0)
                {
                    //默认值，继续
                    continue;
                }
                else
                {
                    //非阻塞connect都失败了
//                   printf("Unblocked Connect Failed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    return -1;
#endif
                }
            }
        }
        else
        {
            //阻塞式connect

            //int time_cnt=0;
            //for(time_cnt=0;(time_cnt<time_wait || time_wait==-1);time_cnt++){

            struct timeval time_begin, time_end, time_use;
            gettimeofday(&time_begin, NULL);
            //for dcebug
            //local_socket_descriptors[sock].socket->send_timeout.tv_sec=10;
            //local_socket_descriptors[sock].socket->send_timeout.tv_usec=0;

            while (1)
            {
                //printf("time_cnt=%d  time_wait=%d\n",time_cnt, time_wait);
                gettimeofday(&time_end, NULL);
                int res = time_substract(&time_begin, &time_end, &time_use);
                if (res != 0)
                {
                    continue;
                }
                else
                {
                    if ( local_socket_descriptors[sock].socket->send_timeout.tv_sec >= 0
                            && local_socket_descriptors[sock].socket->send_timeout.tv_usec >= 0
                            && time_cmp(&time_use, &(local_socket_descriptors[sock].socket->send_timeout) ) > 0)
                    {
                        break;
                    }
                }
//              sleep(1);
                rte_pause();
                //add by gjk_sem_03
                int16_t connect_signal = rte_atomic16_read( &(local_socket_descriptors[sock].socket->connect_close_signal) );
                if ( 0 == connect_signal )
                {
                    //connect信号还是默认值，故我们还不知道连接是成功还是失败，继续等
                    continue;
                }
                else if (1 == connect_signal)
                {
                    //当前是阻塞connect接口，1(SYN_SENT还不能结束，继续等)
                    continue;

                }
                else if ( 2 ==  connect_signal )
                {
                    // 阻塞connect 成功了
                    //根据POSIX接口标准，connect连接成功返回0
                    //当然，要把connect_signal 再置回默认初始值，0
//                   printf("blocked Connect Succeed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
                    local_socket_descriptors[sock].remote_ipaddr = sockaddr_in->sin_addr.s_addr;
                    local_socket_descriptors[sock].remote_port = sockaddr_in->sin_port;
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    return 0;
#endif
                }
                else
                {
                    //connect失败了
                    //                  printf("blocked Connect Failed\n");
                    rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
#ifdef RETURN_VALUE_SUPPORT
                    //退出循环，返回由协议栈端写入的返回值
                    break;
#else
                    return -1;
#endif
                    //return -3;

                }
            }
            //如果运行到这里，说明超时了，也是没连接成功，
            rte_atomic16_set(&(local_socket_descriptors[sock].socket->connect_close_signal), 0);
#ifndef RETURN_VALUE_SUPPORT
            return -1;
#endif
        }


#ifdef RETURN_VALUE_SUPPORT
        //add by ld
        //waiting return value
        while (!rte_atomic16_read( &(res->ready_signal)))
        {
        }
        //set errno
        errno = res->return_errno;
//       printf("connect errno:%d\n", errno);
//       printf("connect return value:%d\n", res->return_value);
        int rv = res->return_value;
        _light_free_return_value_buf_by_proc_id(res, proc_id);
        return rv;
#else
        return 0;
#endif

    }

    //return 0;
}
#endif

inline int light_listen_socket(int sock, int backlog)
{
    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_listen_socket, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sock = %d\n", sock);
#endif

    int i;
    for (i = 0; i < num_procs; ++i)
    {
        int res = _light_listen_socket_by_proc_id(local_socket_descriptors[sock].socket->multicore_socket_copies[i], backlog,  i);
        if ( res != 0)
        {
            light_log(LIGHT_LOG_ERR, "internal listen failure line=%d\n", __LINE__);
            return res;
        }
    }
    return 0;
}

inline int _light_listen_socket_by_proc_id(int sock, int backlog, int proc_id)
{
    light_cmd_t *cmd;
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return -1;
    }
    cmd->cmd = LIGHT_LISTEN_SOCKET_COMMAND;
    cmd->ringset_idx = sock;
    cmd->parent_idx = -1;
    cmd->u.listen_backlog.server_backlog = backlog;
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
        return -2;
    }
    return 0;
}

inline int light_close(int sock) // semi POSIX API
{
    //printf("light_close sock = %d\n", sock);
    int old_light_fd, ret, fd_type;
    old_light_fd = sock;
#ifdef API_DEBUG
    printf("@ In light_close, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    if (sock > LIGHT_FD_THRESHOLD)
#endif
#ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sock) == 1)
#endif
    {
#ifdef TWO_SIDE_FD_SEPARATION
        sock = INT_MAX - sock;
#endif
#ifdef LIGHT_FD_MAPPING
        sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
        printf("  (After fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
        if (sock < LIGHT_CONNECTION_POOL_SIZE)
        {
            ret = _light_close(sock);
        }
        else
        {
            sock -= LIGHT_CONNECTION_POOL_SIZE;
            ret = _epoll_close(sock);
        }
#endif
#ifdef LIGHT_FD_MAPPING
        if (fd_type == LIGHT_SOCKET_INDEX)
        {
            ret = _light_close(sock);
        }
        else if (fd_type == LIGHT_EPOLL_INDEX)
        {
            ret = _epoll_close(sock);
        }
#endif

    }
    else
    {
#ifdef API_DEBUG
        printf("  Use real_close\n");
#endif
        ret = real_close(sock);
    }
#ifdef API_DEBUG
    printf("  Return value = %d\n", ret);
#endif
    return ret;
}


inline int _light_close(int sock) 
{
    light_cmd_t *cmd;
    int light_fd;
#ifdef API_DEBUG
    printf("@ In _light_close\n");
#endif

#ifdef LIGHT_FD_MAPPING
    light_fd = socket_epoll_index_to_light_fd(&sock, LIGHT_SOCKET_INDEX);
    if ((put_back_light_fd(&light_fd)) != 0){
        light_log(LIGHT_LOG_ERR, "put_back_light_fd error\n", __FILE__, __LINE__);
        printf("put_back_light_fd error: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif

    if (!rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_multicore)))
    {
        if (!local_socket_descriptors[sock].is_init)
        {
            //这句话不保证正确
            if (rte_ring_enqueue(free_connections_ring, (void *)local_socket_descriptors[sock].socket) != 0) {
                printf("Error: rte_ring_enqueue(free_connections_ring, (void *)local_socket_descriptors[sock].socket) != 0\n");
            }
            return 0;
        }
        int proc_id = local_socket_descriptors[sock].proc_id;
        cmd = light_get_free_command_buf_by_proc_id(proc_id);
        if (!cmd)
        {
            light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
            light_stats_cannot_allocate_cmd++;
            return -1;
        }
        cmd->cmd = LIGHT_SOCKET_CLOSE_COMMAND;
        cmd->ringset_idx = sock;
        if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
        {
            _light_free_command_buf_by_proc_id(cmd, proc_id);
            return -1;
        }
        light_flush_rx(sock);
        rte_atomic16_init(&local_socket_descriptors[sock].socket->connect_close_signal);
        rte_atomic16_init(&local_socket_descriptors[sock].socket->is_nonblock);
        //将is_nonblock置0
        rte_atomic16_set(&(local_socket_descriptors[sock].socket->is_nonblock), 0);
        light_socket_t *sk = local_socket_descriptors[sock].socket;

        sk->send_timeout.tv_sec = -1;
        sk->send_timeout.tv_usec = -1;
        sk->receive_timeout.tv_sec = -1;
        sk->receive_timeout.tv_usec = -1;

        if (sk && !LL_EMPTY(light_epoll_node_list, &(sk->epoll_node_list)) ) //sk->epoll_node_list_head != NULL)//sk->epoll_idx >= 0)
        {

            //未完成
            //int N = LL_SIZE(light_epoll_node_list, &(sk->epoll_node_list));
            //printf("LLLLokc List\n\n\n");
            light_epoll_node_t *o, *next_o;

            for
            (
                o = LL_FIRST(light_epoll_node_list, &(sk->epoll_node_list));
                o != (void *)0;
                o = next_o
            )
            {
                TAILQ_REMOVE(&epolls[o->epoll_idx].socket_list, &local_socket_descriptors[sock], socket_list_entry);

                next_o = LL_NEXT(light_epoll_node_list, &(sk->epoll_node_list), o);
                LL_UNLINK(light_epoll_node_list, &(sk->epoll_node_list), o);

                o->epoll_idx = -1;
                o->epoll_events = 0;
                o->ready_events = 0;

                _light_free_epoll_node_buf(o);
            }
        }
        local_socket_descriptors[sock].proc_id = 0;
    }
    else
    {
        int main_sock = sock;
        int main_sock_proc_id = local_socket_descriptors[sock].proc_id;

        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sock = local_socket_descriptors[main_sock].socket->multicore_socket_copies[i];

            if (!local_socket_descriptors[sock].is_init)
            {
                //这句话不保证正确
                if (rte_ring_enqueue(free_connections_ring, (void *)local_socket_descriptors[sock].socket) != 0) {
                    printf("Error: rte_ring_enqueue(free_connections_ring, (void *)local_socket_descriptors[sock].socket) != 0\n");
                }
                continue;
            }
            cmd = light_get_free_command_buf_by_proc_id(i);
            if (!cmd)
            {
                light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
                light_stats_cannot_allocate_cmd++;
                return -1;
            }
            cmd->cmd = LIGHT_SOCKET_CLOSE_COMMAND;
            cmd->ringset_idx = sock;
            if (light_enqueue_command_buf_by_proc_id(cmd, i))
            {
                _light_free_command_buf_by_proc_id(cmd, i);
                return -1;
            }

            light_flush_rx(sock);
            rte_atomic16_init(&local_socket_descriptors[sock].socket->connect_close_signal);
            rte_atomic16_init(&local_socket_descriptors[sock].socket->is_nonblock);
            light_socket_t *sk = local_socket_descriptors[sock].socket;

            sk->send_timeout.tv_sec = -1;
            sk->send_timeout.tv_usec = -1;
            sk->receive_timeout.tv_sec = -1;
            sk->receive_timeout.tv_usec = -1;

            if (i == main_sock_proc_id && sk && !LL_EMPTY(light_epoll_node_list, &(sk->epoll_node_list)) )//sk->epoll_node_list_head != NULL) //sk->epoll_idx >= 0)
            {

                light_epoll_node_t *o, *next_o;

                for
                (
                    o = LL_FIRST(light_epoll_node_list, &(sk->epoll_node_list));
                    o != (void *)0;
                    o = next_o
                )
                {

                    TAILQ_REMOVE(&epolls[o->epoll_idx].socket_list, &local_socket_descriptors[sock], socket_list_entry);
                    o->epoll_idx = -1;
                    o->epoll_events = 0;
                    o->ready_events = 0;
                    next_o = LL_NEXT(light_epoll_node_list, &(sk->epoll_node_list), o);
                    LL_UNLINK(light_epoll_node_list, &(sk->epoll_node_list), o);

                    _light_free_epoll_node_buf(o);
                }
            }

            local_socket_descriptors[sock].is_init = 0;
        }
        //local_socket_descriptors[main_sock].socket->is_multicore = 0;
        rte_atomic16_set( &(local_socket_descriptors[main_sock].socket->is_multicore), 0);
        //local_socket_descriptors[main_sock].is_listen_socket = 0;
        local_socket_descriptors[main_sock].proc_id = 0;
    }
    return 0;
}

static inline void _light_notify_empty_tx_buffers_by_proc_id(int sock, int proc_id)
{
    light_cmd_t *cmd;
    if (sock == -1) //anonimous allocation
        return;
    light_socket_t *sk = local_socket_descriptors[sock].socket;
    if (sk == NULL || sk->presented_in_chain) return ;

    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return;
    }
    cmd->cmd = LIGHT_SOCKET_TX_POOL_EMPTY_COMMAND;
    cmd->ringset_idx = sock;
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
    }
}

static inline void _light_notify_empty_tx_buffers(int sock, int proc_id)
{
    light_cmd_t *cmd;
    if (sock == -1) //anonimous allocation
        return;
    light_socket_t *sk = local_socket_descriptors[sock].socket;
    if (sk == NULL || sk->presented_in_chain) return ;

    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return;
    }
    cmd->cmd = LIGHT_SOCKET_TX_POOL_EMPTY_COMMAND;
    cmd->ringset_idx = sock;
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
    }
}

inline int _light_get_socket_tx_space(int sock)
{
    int ring_space = light_socket_tx_space(sock);
    int free_bufs_count = rte_mempool_count(tx_bufs_pool);
    int rc = (ring_space > free_bufs_count) ? (free_bufs_count > 0 ? free_bufs_count : 0)  : ring_space;
    int tx_space = rte_atomic32_read(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->tx_space)) / PKT_PAYLOAD_MAX_SIZE;

#ifdef LIGHT_DEBUG
    fprintf(stderr, "sock %d ring space %d free bufs %d tx space %d\n", sock, ring_space, free_bufs_count, tx_space);
#endif

    if (rc > tx_space)
        rc = tx_space;

    // Todo: tx_ring is going to exhaust, but this way has problem now.
    // if (rc < 10)
    // {
    //     light_log(LIGHT_LOG_WARNING, "no enough tx_space for socket\n");
    //     rte_atomic16_set(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->write_ready_to_app), 0);
    //     _light_notify_empty_tx_buffers(sock, local_socket_descriptors[sock].proc_id);
    // }
    return rc;
}

/* TCP or connected UDP */
inline int _light_send_mbuf(int sock, void *pdesc, int offset, int length)
{
    /*#ifdef EPOLL_SUPPORT
    //       printf("Before testing write_ready_to_app.\n");
        while(rte_atomic16_read(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->write_ready_to_app)) == 0);
        printf("After testing write_ready_to_app.\n");
    #endif*/

    int rc;
    struct rte_mbuf *mbuf = (struct rte_mbuf *)pdesc;
    rte_pktmbuf_data_len(mbuf) = length;
    mbuf->next = NULL;
    //rte_atomic16_set(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->write_ready_to_app),0);
    rc = light_enqueue_tx_buf(sock, mbuf);
    if (rc == 0)
        rte_atomic32_sub(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->tx_space), length);
    light_stats_send_failure += (rc != 0);
    return rc;
}

inline int _light_send_bulk(int sock, struct data_and_descriptor *bufs_and_desc, int *offsets, int *lengths, int buffer_count)
{
    int rc, idx, total_length = 0;
    struct rte_mbuf *mbufs[buffer_count];

    for (idx = 0; idx < buffer_count; idx++)
    {
        /* TODO: set offsets */
        mbufs[idx] = (struct rte_mbuf *)bufs_and_desc[idx].pdesc;
        rte_pktmbuf_data_len(mbufs[idx]) = lengths[idx];
        total_length += lengths[idx];
    }
    // light_stats_buffers_sent += buffer_count;
    // rte_atomic16_set(&(local_socket_descriptors[sock].socket->write_ready_to_app), 0);
    rc = light_enqueue_tx_bufs_bulk(sock, mbufs, buffer_count);
    if (rc == 0)
        rte_atomic32_sub(&(local_socket_descriptors[sock].socket->tx_space), total_length);
    // light_stats_send_failure += (rc != 0);
    return rc;
}

inline ssize_t light_send(int sockfd, const void *buf, size_t nbytes, int flags)
{
    int fd_type;
    #ifdef API_DEBUG
    printf("@ In light_send, (Before fd mapping) sockfd = %d\n", sockfd);
    #endif

    #ifdef TWO_SIDE_FD_SEPARATION
    sockfd = INT_MAX - sockfd;
    #endif

    #ifdef LIGHT_FD_MAPPING
    sockfd = light_fd_to_socket_epoll_index(&sockfd, &fd_type); // the type must be socket
    #endif

    #ifdef API_DEBUG
    printf("(After fd mapping) sockfd = %d\n", sockfd);
    #endif

    // struct timespec tim, tim2;
    // tim.tv_sec = 0;
    // tim.tv_nsec = 100 * 1000;
    // if(nanosleep(&tim , &tim2) < 0 )   
    // {
    //     printf("Nano sleep system call failed \n");
    //     return -1;
    // }

    // find the index of socket (for multi-core version, each light process has its separate socket).
    int proc_id = local_socket_descriptors[sockfd].proc_id;
    sockfd = local_socket_descriptors[sockfd].socket->multicore_socket_copies[proc_id];
    
    int tx_space = _light_get_socket_tx_space(sockfd); // Note: tx_space here is the number of packets that could be sent out at most. (different from the meaning of tx_space in local_socket_t struct)
    struct timeval time_begin, time_end, time_use;
    gettimeofday(&time_begin, NULL);
    while (tx_space == 0 && rte_atomic16_read(&local_socket_descriptors[sockfd].socket->connect_close_signal) < 3) // don't have any space to send and wait for more time until timeout.
    {
        gettimeofday(&time_end, NULL);
        int res = time_substract(&time_begin, &time_end, &time_use);
        if (res != 0)
        {
            continue;
        }
        else
        {
            if (local_socket_descriptors[sockfd].socket->receive_timeout.tv_sec >= 0
                    && local_socket_descriptors[sockfd].socket->receive_timeout.tv_usec >= 0
                    && time_cmp(&time_use, &(local_socket_descriptors[sockfd].socket->send_timeout)) > 0)
            {
                return -1;
            }
        }
        rte_pause();
        tx_space = _light_get_socket_tx_space(sockfd);
    }

    int buf_count = nbytes / PKT_PAYLOAD_MAX_SIZE;
    int remain_bytes = nbytes % PKT_PAYLOAD_MAX_SIZE;
    if (remain_bytes > 0)
        buf_count++;
    if (buf_count > tx_space)
        buf_count = tx_space;

    struct data_and_descriptor bulk_bufs[buf_count];
    int offsets[buf_count];
    int lengths[buf_count];
    int ret_len = 0;
    void* pbuff = buf;

    int connect_close_signal = rte_atomic16_read(&local_socket_descriptors[sockfd].socket->connect_close_signal);
    if (connect_close_signal >= 3) {
         light_log(LIGHT_LOG_DEBUG, "In %s(): exiting, sockfd=%d, connect_close_signal = %d", __func__, sockfd, connect_close_signal);
         return -1;
    }

    if (!_light_get_buffers_bulk_by_proc_id(PKT_PAYLOAD_MAX_SIZE, sockfd, buf_count, bulk_bufs, proc_id))
    {
        int i = 0;
        for (i = 0; i < buf_count; i++)
        {
            if (nbytes > PKT_PAYLOAD_MAX_SIZE)
            {
                rte_memcpy(bulk_bufs[i].pdata, pbuff, PKT_PAYLOAD_MAX_SIZE);
                nbytes -= PKT_PAYLOAD_MAX_SIZE;
                offsets[i] = 0;
                lengths[i] = PKT_PAYLOAD_MAX_SIZE;
                ret_len += PKT_PAYLOAD_MAX_SIZE;
                pbuff += PKT_PAYLOAD_MAX_SIZE;
            }
            else
            {
                rte_memcpy(bulk_bufs[i].pdata, pbuff, nbytes);
                offsets[i] = 0;
                lengths[i] = nbytes;
                ret_len += nbytes;
                pbuff += nbytes;
                nbytes = 0;
                break;
            }
        }

        if (_light_send_bulk(sockfd, bulk_bufs, offsets, lengths, buf_count))
        {
            for (i = 0; i < tx_space; i++)
                _light_release_tx_buffer(bulk_bufs[i].pdesc);
            return -1;
        }

        while (_light_socket_kick_by_proc_id(sockfd, proc_id) == -1)
        {
            rte_pause();
        }
        return ret_len;
    }
    else
    {
        return -1;
    }
}

inline ssize_t light_write(int sockfd, const void *buf, size_t nbytes)
{
#ifdef API_DEBUG
    printf("@ In light_write\n");
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    if (sockfd > LIGHT_FD_THRESHOLD)
#endif

#ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sockfd) == 1)
#endif
    {
        return light_send(sockfd, buf, nbytes, 0);
    }
    else
    {
        return real_write(sockfd, buf, nbytes);
    }
}

inline ssize_t light_writev(int sockfd, const struct iovec* iov, int iovcnt)
{
#ifdef API_DEBUG
    printf("@ In light_writev\n");
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    if (sockfd > LIGHT_FD_THRESHOLD)
#endif

#ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sockfd) == 1)
#endif
    {
        light_log(LIGHT_LOG_DEBUG, "Entering %s(), sockfd=%d.", __func__, INT_MAX-sockfd);
        int retlen = 0;
        for (int i = 0; i < iovcnt; i++)
        {
//           printf("Before light_send\n");
            int templen = light_send(sockfd, iov[i].iov_base, iov[i].iov_len, 0);
            //          printf("After light_send\n");
            if (templen <= 0)
                if (retlen > 0)
                    return retlen;
                else
                    return templen;
            else
            {
                retlen += templen;
                if (templen < iov[i].iov_len)
                    return retlen;
            }
        }

        light_log(LIGHT_LOG_DEBUG, "Leaving %s(), sockfd=%d, retlen=%d.", __func__, INT_MAX-sockfd, retlen);
        return retlen;
    }
    else
    {
        return real_writev(sockfd, iov, iovcnt);
    }
}



/* UDP or RAW */
inline int _light_sendto_mbuf(int sock, void *pdesc, int offset, int length, struct sockaddr *addr, __rte_unused int addrlen) // not used
{
    int rc;
    struct rte_mbuf *mbuf = (struct rte_mbuf *)pdesc;
    char *p_addr = rte_pktmbuf_mtod(mbuf, char *);
    struct sockaddr_in *p_addr_in, *in_addr = (struct sockaddr_in *)addr;
    rte_pktmbuf_data_len(mbuf) = length;
    p_addr -= sizeof(struct sockaddr_in);
    p_addr_in = (struct sockaddr_in *)p_addr;
    *p_addr_in = *in_addr;
    // rte_atomic16_set(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->write_ready_to_app), 0);
    //printf("mbuf size: %d \n",sizeof(*mbuf));
    rc = light_enqueue_tx_buf(sock, mbuf);
    if (rc == 0)
        rte_atomic32_sub(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->tx_space), length);
    light_stats_send_failure += (rc != 0);
    return rc;
}



inline int _light_sendto_bulk(int sock, struct data_and_descriptor *bufs_and_desc, int *offsets, int *lengths, struct sockaddr *addr, __rte_unused int addrlen, int buffer_count)
{
    int rc, idx, total_length = 0;
    struct rte_mbuf *mbufs[buffer_count];
    struct sockaddr_in *in_addr;

    for (idx = 0; idx < buffer_count; idx++)
    {
        char *p_addr;
        struct sockaddr_in *p_addr_in;
        /* TODO: set offsets */
        mbufs[idx] = (struct rte_mbuf *)bufs_and_desc[idx].pdesc;
        rte_pktmbuf_data_len(mbufs[idx]) = lengths[idx];
        total_length += lengths[idx];
        p_addr = rte_pktmbuf_mtod(mbufs[idx], char *);

        rte_pktmbuf_data_len(mbufs[idx]) = lengths[idx];
        p_addr -= sizeof(struct sockaddr_in);
        p_addr_in = (struct sockaddr_in *)p_addr;
        in_addr = (struct sockaddr_in *)addr;
        *p_addr_in = *in_addr;
        addr++;
    }
    light_stats_buffers_sent += buffer_count;
    // rte_atomic16_set(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->write_ready_to_app), 0);
    rc = light_enqueue_tx_bufs_bulk(sock, mbufs, buffer_count);
    if (rc == 0)
        rte_atomic32_sub(&(local_socket_descriptors[sock & SOCKET_READY_MASK].socket->tx_space), total_length);

    light_stats_send_failure += (rc != 0);
    return rc;
}

// Todo: sendto api has free_command_pool operation bug
extern inline ssize_t light_sendto(int sockfd, const void *buf, size_t nbytes, int flags,
                                   const struct sockaddr *destaddr, socklen_t destlen)
{
    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_sendto, (Before fd mapping) sockfd = %d\n", sockfd);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sockfd = INT_MAX - sockfd;
#endif

#ifdef LIGHT_FD_MAPPING
    sockfd = light_fd_to_socket_epoll_index(&sockfd, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sockfd = %d\n", sockfd);
#endif

    // find the real sockfd
    int proc_id = local_socket_descriptors[sockfd].proc_id;
    sockfd = local_socket_descriptors[sockfd].socket->multicore_socket_copies[proc_id];

    struct timeval time_begin, time_end, time_use;
    gettimeofday(&time_begin, NULL);

    int tx_space = _light_get_socket_tx_space(sockfd);
    while (tx_space == 0 && rte_atomic16_read(&local_socket_descriptors[sockfd].socket->connect_close_signal) < 3)
    {

        gettimeofday(&time_end, NULL);
        int res = time_substract(&time_begin, &time_end, &time_use);
        if (res != 0)
        {
            continue;
        }
        else
        {
            if (local_socket_descriptors[sockfd].socket->send_timeout.tv_sec >= 0
                    && local_socket_descriptors[sockfd].socket->send_timeout.tv_usec >= 0
                    && time_cmp(&time_use, &(local_socket_descriptors[sockfd].socket->send_timeout)) > 0)
            {
                return -1;
            }
        }
        rte_pause();
        tx_space = _light_get_socket_tx_space(sockfd);
    }

    int buf_count = nbytes / PKT_PAYLOAD_MAX_SIZE;
    int remain_bytes = nbytes % PKT_PAYLOAD_MAX_SIZE;
    if (remain_bytes > 0)
        buf_count++;
    if (buf_count > tx_space)
        buf_count = tx_space;

    struct data_and_descriptor bulk_bufs[buf_count];
    int offsets[buf_count];
    int lengths[buf_count];
    int ret_len = 0;
    void* pbuff = buf;

    if (rte_atomic16_read(&local_socket_descriptors[sockfd].socket->connect_close_signal) >= 3)
        return -1;

    if (!_light_get_buffers_bulk_by_proc_id(PKT_PAYLOAD_MAX_SIZE, sockfd, buf_count, bulk_bufs, proc_id))
    {
        int i = 0;
        for (i = 0; i < buf_count; i++)
        {
            if (nbytes > PKT_PAYLOAD_MAX_SIZE)
            {
                rte_memcpy(bulk_bufs[i].pdata, pbuff, PKT_PAYLOAD_MAX_SIZE);
                nbytes -= PKT_PAYLOAD_MAX_SIZE;
                offsets[i] = 0;
                lengths[i] = PKT_PAYLOAD_MAX_SIZE;
                ret_len += PKT_PAYLOAD_MAX_SIZE;
                pbuff += PKT_PAYLOAD_MAX_SIZE;
            }
            else
            {
                rte_memcpy(bulk_bufs[i].pdata, pbuff, nbytes);
                lengths[i] = nbytes;
                ret_len += nbytes;
                pbuff += nbytes;
                offsets[i] = 0;
                nbytes = 0;
                break;
            }
        }
        if (_light_sendto_bulk(sockfd, bulk_bufs, offsets, lengths, destaddr, destlen, buf_count))
        {
            for (i = 0; i < tx_space; i++)
                _light_release_tx_buffer(bulk_bufs[i].pdesc);
            return -1;
        }
        while (_light_socket_kick_by_proc_id(sockfd, proc_id) == -1)
        {
            rte_pause();
        }
        return ret_len;
    } else {
        return -1;
    }
}

static inline struct rte_mbuf *_light_get_from_shadow(int sock)
{
    struct rte_mbuf *mbuf = NULL;

    if (local_socket_descriptors[sock].shadow)
    {
        mbuf = local_socket_descriptors[sock].shadow;
        local_socket_descriptors[sock].shadow = NULL;
        rte_pktmbuf_data_len(mbuf) = local_socket_descriptors[sock].shadow_len_remainder;
        mbuf->data_off += local_socket_descriptors[sock].shadow_len_delievered;
        mbuf->next = local_socket_descriptors[sock].shadow_next;
        local_socket_descriptors[sock].shadow_next = NULL;
        if (mbuf->next)
        {
            rte_pktmbuf_pkt_len(mbuf) = rte_pktmbuf_data_len(mbuf) + rte_pktmbuf_pkt_len(mbuf->next);
        }
        else
        {
            rte_pktmbuf_pkt_len(mbuf) = rte_pktmbuf_data_len(mbuf);
        }
    }
    return mbuf;
}

// set the real length of data in mbuf chain. If over original length, the left first segment will be stored in shadow
static inline void _light_try_read_exact_amount(struct rte_mbuf *mbuf, int sock, int *total_len, int *first_segment_len)
{
    struct rte_mbuf *tmp = mbuf, *prev = NULL;
    int curr_len = 0;
    while ((tmp) && ((rte_pktmbuf_data_len(tmp) + curr_len) < *total_len))
    {
        curr_len += rte_pktmbuf_data_len(tmp);
        prev = tmp;
        tmp = tmp->next;
    }
    if (tmp)
    {
        printf("buf doesn't have enough room for data, shadow will work\n");
        *first_segment_len = rte_pktmbuf_data_len(mbuf);
        if ((curr_len + rte_pktmbuf_data_len(tmp)) > *total_len)  /* more data remains */
        {
            local_socket_descriptors[sock].shadow = tmp;
            local_socket_descriptors[sock].shadow_next = tmp->next;
            local_socket_descriptors[sock].shadow_len_remainder = (curr_len + rte_pktmbuf_data_len(tmp)) - *total_len;
            local_socket_descriptors[sock].shadow_len_delievered =
                rte_pktmbuf_data_len(tmp) - local_socket_descriptors[sock].shadow_len_remainder;
            rte_pktmbuf_data_len(tmp) = local_socket_descriptors[sock].shadow_len_delievered;
        } else { // accumulated length exactly equals original length
            if (tmp->next)
            {
                local_socket_descriptors[sock].shadow = tmp->next;
                local_socket_descriptors[sock].shadow_next = tmp->next->next;
                if (local_socket_descriptors[sock].shadow)
                {
                    local_socket_descriptors[sock].shadow_len_remainder =
                        rte_pktmbuf_data_len(local_socket_descriptors[sock].shadow);
                    local_socket_descriptors[sock].shadow_len_delievered = 0;
                }
            }
        }
        tmp->next = NULL;
        if (unlikely(curr_len == *total_len))
        {
            printf("Warning: curr_len == *total_len\n");
            if (prev)
                prev->next = NULL;
        }
    } else {
        *total_len = curr_len;
        *first_segment_len = rte_pktmbuf_data_len(mbuf);
    }
}

static inline int _light_receive_buffer_chain_by_proc_id(int sock, void **pbuffer, int *total_len, int *first_segment_len, void **pdesc, int is_nonblock, int proc_id)
{
    #ifdef LIGHT_DEBUG
    fprintf(stderr, "Before _light_get_from_shadow\n");
    #endif
    struct rte_mbuf *mbuf;

    /* first try to look shadow. shadow pointer saved when last mbuf delievered partially */
    mbuf = _light_get_from_shadow(sock);

    #ifdef LIGHT_DEBUG
    fprintf(stderr, "After _light_get_from_shadow, total_len = %d, mbuf = %p\n", (*total_len), (void*)mbuf);
    #endif

    if (mbuf) /* shadow has mbuf */
    {
        if (*total_len > 0) /* means user restricts total read count */
        {
            int total_len2 = *total_len;
            _light_try_read_exact_amount(mbuf, sock, &total_len2, first_segment_len);
            *pbuffer = rte_pktmbuf_mtod(mbuf, void *);
            *pdesc = mbuf;
            if ((total_len2 > 0) && (total_len2 < *total_len)) /* read less than user requested, try ring */
            {
                struct rte_mbuf *mbuf2 = light_dequeue_rx_buf_nonblock_by_proc_id(sock, proc_id);
                if (!mbuf2)  /* ring is empty */
                {
                    *total_len = total_len2;
                } else {  /* now try to find an mbuf to be delievered partially in the chain */
                    int total_len3 = *total_len - total_len2;
                    int first_segment_len_dummy;
                    _light_try_read_exact_amount(mbuf2, sock, &total_len3, &first_segment_len_dummy);
                    struct rte_mbuf *last_mbuf = rte_pktmbuf_lastseg(mbuf);
                    last_mbuf->next = mbuf2;
                    *total_len = total_len2 + total_len3;
                }
            } else {
                printf("unexpected: In _light_receive_buffer_chain_by_proc_id, total_len2 = %d, *total_len = %d\n", total_len2, *total_len);
                //goto read_from_ring;
            }
            return 0;
        } else { /* means user does not care how much to read. Try to read all */
            struct rte_mbuf *mbuf2 = light_dequeue_rx_buf_nonblock_by_proc_id(sock, proc_id);
            struct rte_mbuf *last_mbuf = rte_pktmbuf_lastseg(mbuf);
            last_mbuf->next = mbuf2;
            if (mbuf2)
                rte_pktmbuf_pkt_len(mbuf) += rte_pktmbuf_pkt_len(mbuf2);
            *total_len = rte_pktmbuf_pkt_len(mbuf);
            *first_segment_len = rte_pktmbuf_data_len(mbuf);
            *pbuffer = rte_pktmbuf_mtod(mbuf, void *);
            *pdesc = mbuf;
            // rte_atomic16_set(&(local_socket_descriptors[sock].socket->read_ready_to_app), 0);
            return 0;
        }
    }

    /* shadow does not have mbuf, then read_from_ring */
    mbuf = is_nonblock ? light_dequeue_rx_buf_nonblock_by_proc_id(sock, proc_id)
                       : light_dequeue_rx_buf_by_proc_id(sock, proc_id);
    if (mbuf)
    {
        if (*total_len > 0)  /* means user restricts total read count */
        {
            int total_len2 = *total_len;
            _light_try_read_exact_amount(mbuf, sock, &total_len2, first_segment_len);
            *total_len = total_len2;
        } else {
            *total_len = rte_pktmbuf_pkt_len(mbuf);
            *first_segment_len = rte_pktmbuf_data_len(mbuf);
            light_stats_rx_returned += mbuf->nb_segs;
        }
        *pbuffer = rte_pktmbuf_mtod(mbuf, void *);
        *pdesc = mbuf;
        //rte_atomic16_set(&(local_socket_descriptors[sock].socket->read_ready_to_app), 0);//erased by gjk_sem
        return 0;
    }

    if (rte_ring_empty(local_socket_descriptors[sock].local_cache) == 1
            && rte_ring_empty(local_socket_descriptors[sock].rx_ring) == 1
            && rte_atomic16_read(&local_socket_descriptors[sock].socket->connect_close_signal) >= 3)
    {
    	printf("In _light_receive_buffer_chain_by_proc_id, packet not ready\n");
        light_log(LIGHT_LOG_WARNING, "receive_buffer_chain: packet not ready will return -1");
        return -1;
    }
    // light_stats_recv_failure++;
    printf("In _light_receive_buffer_chain_by_proc_id, error\n");
    light_log(LIGHT_LOG_WARNING, "receive_buffer_chain: packet not ready return -2");
    return -2;
}

inline ssize_t light_recv(int sock, void *buf, size_t nbytes, int flags)
{
    int fd_type;
	#ifdef API_DEBUG
    printf("@ In light_recv, (Before fd mapping) sock = %d\n", sock);
	#endif

	#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
	#endif

	#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
	#endif

	#ifdef API_DEBUG
    printf("(After fd mapping) sock = %d\n", sock);
	#endif

    // sock is converted to the index of array.
    int proc_id = local_socket_descriptors[sock].proc_id;

    // To support multicore, each stack process on different cpu core has its individual socket structure with different sock (array index).
    sock = local_socket_descriptors[sock].socket->multicore_socket_copies[proc_id];
    int is_nonblock;
    is_nonblock = rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) );

	#ifdef LIGHT_DEBUG
    fprintf(stderr, "Enter light_recv\n");
	#endif

    int recv_result, first_seg_len = 0, tempBytes = nbytes;
    void *rxbuff, *pdesc, *tempbuf = buf;
	#ifdef LIGHT_DEBUG
    fprintf(stderr, "Before light_receive_buffer_chain\n");
	#endif
    recv_result = _light_receive_buffer_chain_by_proc_id(sock, &rxbuff, &tempBytes, &first_seg_len, &pdesc, is_nonblock, proc_id);
	#ifdef LIGHT_DEBUG
    fprintf(stderr, "recv_result=%d\n", recv_result);
	#endif

    if (recv_result == 0) // get the mbuf chain
    {
        void *porigdesc = pdesc;
        if (tempBytes > 0)
        {
            while (rxbuff)
            {
                rte_memcpy(tempbuf, rxbuff, first_seg_len);
                tempbuf += first_seg_len;
                rxbuff = _light_get_next_buffer_segment(&pdesc, &first_seg_len);
            }
        }
        _light_release_rx_buffer(porigdesc, sock);
		#ifdef LIGHT_DEBUG
        fprintf(stderr, "tempBytes=%d\n", tempBytes  );
		#endif
		
        return tempBytes;
    }
    else if (recv_result == -1)
    {
        return 0;
    }
    else
    {
        //收到不到数据，errno置位
        if (is_nonblock)
            errno = EAGAIN;

        return -1;
    }
}

inline ssize_t light_read(int sock, void *buf, size_t nbytes)
{
#ifdef TWO_SIDE_FD_SEPARATION
    if (sock > LIGHT_FD_THRESHOLD)
#endif

#ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sock) == 1)
#endif
    {
        return light_recv(sock, buf, nbytes, 0);
    }
    else
    {
        return real_read(sock, buf, nbytes);
    }
}

inline ssize_t light_readv(int sockfd, const struct iovec* iov, int iovcnt)
{
#ifdef TWO_SIDE_FD_SEPARATION
    if (sockfd > LIGHT_FD_THRESHOLD)
#endif

#ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sockfd) == 1)
#endif
    {
        int i = 0, retlen = 0;
        for (; i < iovcnt; i++)
        {
            int templen = light_recv(sockfd, iov[i].iov_base, iov[i].iov_len, 0);
            if (templen <= 0)
                if (retlen > 0)
                    return retlen;
                else
                    return templen;
            else
            {
                retlen += templen;
                if (templen < iov[i].iov_len)
                    return retlen;
            }
        }
        return retlen;
    }
    else
    {
        return readv(sockfd, iov, iovcnt);
    }

}

/* UDP or RAW */
inline int _light_receivefrom_mbuf(int sock, void **buffer, int *len, struct sockaddr *addr, __rte_unused int *addrlen, void **pdesc)
{
    struct rte_mbuf *mbuf = light_dequeue_rx_buf(sock);
    struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;
    if (!mbuf)
    {
        light_log(LIGHT_LOG_ERR, "udp receive mbuf is null.socket = %d", sock);
        light_stats_recv_failure++;
        return -1;
    }
    *buffer = rte_pktmbuf_mtod(mbuf, void *);
    *pdesc = mbuf;
    *len = rte_pktmbuf_data_len(mbuf);
    char *p_addr = rte_pktmbuf_mtod(mbuf, char *);
    p_addr -= sizeof(struct sockaddr_in);
    struct sockaddr_in *p_addr_in = (struct sockaddr_in *)p_addr;
    *in_addr = *p_addr_in;
    return 0;
}

// Todo: recvfrom api has free_command_pool operation bug
inline ssize_t light_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
{
    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_recvfrom, (Before fd mapping) sockfd = %d\n", sockfd);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sockfd = INT_MAX - sockfd;
#endif

#ifdef LIGHT_FD_MAPPING
    sockfd = light_fd_to_socket_epoll_index(&sockfd, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sockfd = %d\n", sockfd);
#endif

    void *rxbuff, *pdesc;
    int ret_len;
    int recv_result = -1;
    struct rte_mbuf *mbuf;
    if (local_socket_descriptors[sockfd].shadow)
    {
        mbuf = local_socket_descriptors[sockfd].shadow_next;
        ret_len = local_socket_descriptors[sockfd].shadow_len_delievered;
        if (ret_len > len)
            ret_len = len;
        light_log(LIGHT_LOG_DEBUG, "udp debug! socket:%d shadow length=%d, buff len=%d", sockfd, ret_len, len);
        void* tempbuf = rte_pktmbuf_mtod(mbuf, void*);
        rte_memcpy(buf, tempbuf, ret_len);
        struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;
        char *p_addr = rte_pktmbuf_mtod(mbuf, char *);
        p_addr -= sizeof(struct sockaddr_in);
        struct sockaddr_in *p_addr_in = (struct sockaddr_in *)p_addr;
        *in_addr = *p_addr_in;
        if (!mbuf->next)
        {
            void *porigdesc = (void*) local_socket_descriptors[sockfd].shadow;
            local_socket_descriptors[sockfd].shadow = NULL;
            local_socket_descriptors[sockfd].shadow_next = NULL;
            local_socket_descriptors[sockfd].shadow_len_delievered = 0;
            _light_release_rx_buffer(porigdesc, sockfd);
        }
        else
        {
            _light_get_next_buffer_segment(&local_socket_descriptors[sockfd].shadow_next,
                                                  &local_socket_descriptors[sockfd].shadow_len_delievered);
        }
        return ret_len;
    }
    else
    {
        if (rte_atomic16_read(&local_socket_descriptors[sockfd].socket->connect_close_signal) >= 3)
            return -1;
        recv_result = _light_receivefrom_mbuf(sockfd, &rxbuff, &ret_len, addr, addrlen, &pdesc);
        if (recv_result == 0)
        {
            void *porigdesc = pdesc;
            light_log(LIGHT_LOG_DEBUG, "udp debug! socket:%d mbuf receive data %d length, buff len= %d", sockfd, ret_len, len);
            if (ret_len > len)
                ret_len = len;
            if (rxbuff && ret_len > 0)
                rte_memcpy(buf, rxbuff, ret_len);
            mbuf = (struct rte_mbuf *)pdesc;
            if (!mbuf->next)
            {
                _light_release_rx_buffer(porigdesc, sockfd);
            }
            else
            {
                local_socket_descriptors[sockfd].shadow = mbuf;
                local_socket_descriptors[sockfd].shadow_next = mbuf->next;
                local_socket_descriptors[sockfd].shadow_len_delievered =
                    rte_pktmbuf_data_len(local_socket_descriptors[sockfd].shadow_next);
            }
            return ret_len;
        }
        else
        {
            return -1;
        }
    }
}

inline int _light_get_buffers_bulk_by_proc_id(int length, int owner_sock, int count, struct data_and_descriptor *bufs_and_desc, int proc_id)
{
    struct rte_mbuf *mbufs[count];
    int idx;

    #ifdef BUFFER_MANAGE
    unsigned int extra_mbuf_num = 0; // the number of mbufs of extra connections
    if (rte_mempool_in_use_count(tx_bufs_pool) > EXP_MBUF_POOL_SIZE) {
        #ifdef BUFFER_MANAGE_DEBUG
        printf("Note: total mbuf exceed! sock = %d\n", owner_sock);
        #endif

        return 1;
    }

    // if (owner_sock <= GRT_CONN_NUM) { // for bandwidth-gurantee connections, the max size of mbuf can not exceed EXP_MBUF_POOL_SIZE / GRT_CONN_NUM
    //     while (local_socket_descriptors[owner_sock].socket->occupied_mbuf_num > EXP_MBUF_POOL_SIZE / GRT_CONN_NUM) {
    //         rte_pause();
    //     }
    // }

    if (owner_sock > GRT_CONN_NUM) { // for other connections, the total size of their mbufs can not exceed EXP_MBUF_POOL_SIZE * 3%
        if (1) {
            for (idx = GRT_CONN_NUM + 1; idx <= LIGHT_CONNECTION_POOL_SIZE - rte_ring_count(free_connections_ring)
    -2; idx++) {
                #ifdef BUFFER_MANAGE_DEBUG
                printf("rte_atomic16_read(&local_socket_descriptors[%d].socket->occupied_mbuf_num) = %d\n", idx, rte_atomic16_read(&local_socket_descriptors[idx].socket->occupied_mbuf_num));
                #endif

                extra_mbuf_num += rte_atomic16_read(&local_socket_descriptors[idx].socket->occupied_mbuf_num);
            }
            #ifdef BUFFER_MANAGE_DEBUG
            printf("socket = %d, extra_mbuf_num = %d\n", owner_sock, extra_mbuf_num);
            #endif

            if (extra_mbuf_num > EXP_MBUF_POOL_SIZE * 0.03) {
                #ifdef BUFFER_MANAGE_DEBUG
                printf("Note: extra_mbuf_num exceed! Wait! sock = %d\n", owner_sock);
                #endif

                return 1;
            }
        }

    }
    #endif // BUFFER_MANAGE

    if (rte_mempool_get_bulk(tx_bufs_pool, mbufs, count))
    {
        printf("Warning: tx_bufs_pool is nearly empty.\n");
        _light_notify_empty_tx_buffers_by_proc_id(owner_sock, proc_id);
        // light_stats_tx_buf_allocation_failure++;
        return 1;
    }

    #ifdef BUFFER_MANAGE
    rte_atomic16_add(&local_socket_descriptors[owner_sock].socket->occupied_mbuf_num, count);
    #endif

    for (idx = 0; idx < count; idx++)
    {
        //if(rte_mbuf_refcnt_read(mbufs[idx])!=0)
        //                printf("mbuf ref cnt : %d\n",rte_mbuf_refcnt_read(mbufs[idx]));
        rte_pktmbuf_reset(mbufs[idx]);
        rte_mbuf_refcnt_set(mbufs[idx], 1);
        bufs_and_desc[idx].pdata = rte_pktmbuf_mtod(mbufs[idx], void *);
        bufs_and_desc[idx].pdesc = mbufs[idx];
    }
    // light_stats_buffers_allocated += count;
    return 0;
}

/* release buffer when either send is complete or receive has done with the buffer */
inline void _light_release_tx_buffer(void *pdesc)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)pdesc;

    rte_pktmbuf_free_seg(mbuf);
}

inline void _light_release_rx_buffer(void *pdesc, int sock)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)pdesc;
#if 1 /* this will work if only all mbufs are guaranteed from the same mempool */
    struct rte_mbuf *next;
    void *mbufs[MAX_PKT_BURST];
    int count;
    while (mbuf)
    {
        for (count = 0; (count < MAX_PKT_BURST) && (mbuf);)
        {
            if (mbuf == local_socket_descriptors[sock].shadow)
            {
                mbuf = NULL; /* to stop the outer loop after freeing */
                break;
            }
            next = mbuf->next;
            if (likely(__rte_pktmbuf_prefree_seg(mbuf) != NULL))
            {
                mbufs[count++] = mbuf;
                mbuf->next = NULL;
            }
            mbuf = next;
        }

        if (count > 0)
            rte_mempool_put_bulk(((struct rte_mbuf *)mbufs[0])->pool, mbufs, count);
    }
#else
    rte_pktmbuf_free(mbuf);
#endif
}

inline int _light_socket_kick_by_proc_id(int sock, int proc_id)
{
    light_cmd_t *cmd;
    if (!rte_atomic16_test_and_set(&(local_socket_descriptors[sock].socket->write_done_from_app)))
    {
        return 0;
    }
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        printf("Error: can't get free cmd in _light_socket_kick_by_proc_id\n");
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        // light_stats_cannot_allocate_cmd++;
        return -1;
    }
    cmd->cmd = LIGHT_SOCKET_TX_KICK_COMMAND;
    cmd->ringset_idx = sock;
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
    }
    else
        // light_stats_tx_kicks_sent++;
    return 0;
}

inline int light_accept4(int sock, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
#ifdef API_DEBUG
    printf("@ In light_accept4\n");
#endif

    if(flags & O_NONBLOCK)
    {
        int fd = light_accept(sock, addr, addrlen);
        if(fd  != -1)//set new socket non_block
        {
            int non_block = 1;
            light_ioctl(fd,FIONBIO,&non_block);
            light_log(LIGHT_LOG_INFO, "in light_accept4,after ioctl, errno(%d)", errno);
        }
        return fd;
    }
    else
    {
        return light_accept(sock, addr, addrlen);
    }
}

inline int light_accept(int sock, struct sockaddr *addr, __rte_unused socklen_t *addrlen)
{
    static int accept_index = 0;
    int start_index = accept_index;
    int current_index;
    int current_socket;
    
    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_accept, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sock = %d\n", sock);
#endif

    int is_nonblock  = rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) );

    light_log(LIGHT_LOG_INFO, "accept listenfd(%d), is_nonblock(%d)\n", sock, is_nonblock);
    if (is_nonblock)
    {
        /*
        light_log(LIGHT_LOG_ERR, "nonblock listenfd(%d)\n", sock);
        int is_empty = 1;
        for (int i = 1; i <= num_procs; ++i)
        {
            current_index = (start_index + i) % num_procs;
            current_socket = local_socket_descriptors[sock].socket->multicore_socket_copies[current_index];
            if (rte_ring_empty(local_socket_descriptors[current_socket].rx_ring ) != 1 )
            {
                is_empty = 0;
                accept_index = current_index;
                break;
            }
        }
        if (is_empty)
        {
            light_log(LIGHT_LOG_ERR, "nonblock listenfd(%d), no new connection\n", sock);
            errno = EAGAIN;
            return -1;
        }
        */
        current_index = stack_proc_id;
        current_socket = local_socket_descriptors[sock].socket->multicore_socket_copies[current_index];
        if(rte_ring_empty(local_socket_descriptors[current_socket].rx_ring ) == 1){
            errno = EAGAIN;
            return -1;
        }
    }
    else
    {
        /*
        light_log(LIGHT_LOG_ERR, "block listenfd(%d)\n", sock);
        int is_empty = 1;
        while (is_empty)
        {
            for (int i = 1; i <= num_procs; ++i)
            {
                current_index = (start_index + i) % num_procs;
                current_socket = local_socket_descriptors[sock].socket->multicore_socket_copies[current_index];
                int res = rte_ring_empty(local_socket_descriptors[current_socket].rx_ring );
                if ( res != 1 )
                {
                    is_empty = 0;
                    accept_index = current_index;
                    break;
                }
            }
            rte_pause();
        }
        light_log(LIGHT_LOG_ERR, "block listenfd(%d) done\n", sock);
        */
        current_index = stack_proc_id;
        current_socket = local_socket_descriptors[sock].socket->multicore_socket_copies[current_index];
        while( rte_ring_empty(local_socket_descriptors[current_socket].rx_ring) ){
            rte_pause();
        }
    }
    light_log(LIGHT_LOG_INFO, "accept sock(%d), proc_id(%d)\n", current_socket, current_index);
    return _light_accept( current_socket, addr, addrlen, current_index);
}



inline int _light_accept(int sock, struct sockaddr *addr, __rte_unused socklen_t *addrlen, int proc_id)
{
    int light_fd;
    light_cmd_t *cmd;
    light_socket_t *light_socket;
    unsigned long accepted_socket;
    struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;

    light_log(LIGHT_LOG_DEBUG, "socket: %d accept at core:%d\n", sock, proc_id);
    light_socket_t *skt = local_socket_descriptors[sock].socket;
    if (rte_ring_dequeue(local_socket_descriptors[sock].rx_ring, (void **)&cmd))
    {
        printf("Error: rte_ring_dequeue(local_socket_descriptors[sock].rx_ring, (void **)&cmd) != 0\n");
        light_log(LIGHT_LOG_ERR, "socket:%d no cmd in rx_ring. line=%d\n", sock, __LINE__);
        return -1;
    }
    int ret;
    if (ret = rte_ring_dequeue(free_connections_ring, (void **)&light_socket))
    {
        printf("Error: rte_ring_dequeue(free_connections_ring, (void **)&light_socket) != 0\n");
        light_log(LIGHT_LOG_ERR, "NO FREE CONNECTIONS line=%d\n", __LINE__);
        cmd->cmd = LIGHT_SOCKET_DECLINE_COMMAND;
        cmd->u.socket_decline.socket_descr = cmd->u.accepted_socket.socket_descr;
        if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
        {
            _light_free_command_buf_by_proc_id(cmd, proc_id);
            light_log(LIGHT_LOG_ERR, "CANNOT ENQUEUE SET_RING_COMMAND\n");
            return -1;
        }
        return -1;
    }
    accepted_socket = cmd->u.accepted_socket.socket_descr;
    if (in_addr != NULL)
    {
        in_addr->sin_family = AF_INET;
        in_addr->sin_addr.s_addr = cmd->u.accepted_socket.ipaddr;
        in_addr->sin_port = cmd->u.accepted_socket.port;
        local_socket_descriptors[light_socket->connection_idx].remote_ipaddr = in_addr->sin_addr.s_addr;
        local_socket_descriptors[light_socket->connection_idx].remote_port = in_addr->sin_port;
    }

    local_socket_descriptors[light_socket->connection_idx].socket = light_socket;
    _light_reset_local_descriptor(light_socket->connection_idx);
 
    local_socket_descriptors[light_socket->connection_idx].local_ipaddr =
        local_socket_descriptors[sock].local_ipaddr;
    local_socket_descriptors[light_socket->connection_idx].local_port =
        local_socket_descriptors[sock].local_port;

    //-1 means unlimited
    local_socket_descriptors[light_socket->connection_idx].socket->send_timeout.tv_sec = -1;
    local_socket_descriptors[light_socket->connection_idx].socket->send_timeout.tv_usec = -1;
    local_socket_descriptors[light_socket->connection_idx].socket->receive_timeout.tv_sec = -1;
    local_socket_descriptors[light_socket->connection_idx].socket->receive_timeout.tv_usec = -1;

    //ld注意这两行，多核加上的代码
    local_socket_descriptors[light_socket->connection_idx].proc_id = proc_id;
    local_socket_descriptors[light_socket->connection_idx].socket->multicore_socket_copies[proc_id] = light_socket->connection_idx;
    //local_socket_descriptors[light_socket->connection_idx].is_accept_socket = 1;


    //light_socket->is_multicore = 0;
    rte_atomic16_set( &(light_socket->is_multicore), 0);

    rte_atomic16_set( &(light_socket->connect_close_signal), 0);
    rte_atomic16_set( &(light_socket->is_nonblock), 0);

    cmd->cmd = LIGHT_SET_SOCKET_RING_COMMAND;
    cmd->ringset_idx = light_socket->connection_idx;
    cmd->parent_idx = -1;
    cmd->u.set_socket_ring.socket_descr = accepted_socket;
    cmd->u.set_socket_ring.pid = getpid();

    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
        return -1;
    }
    light_stats_accepted++;
    local_socket_descriptors[light_socket->connection_idx].is_init = 1;
    
#ifdef TWO_SIDE_FD_SEPARATION
    return INT_MAX - light_socket->connection_idx;
#endif

#ifdef LIGHT_FD_MAPPING
    light_fd = get_new_light_fd(&(light_socket->connection_idx), LIGHT_SOCKET_INDEX);
    return light_fd;
#endif
}

inline void light_getsockname(int sock, int is_local, struct sockaddr *addr, __rte_unused int *addrlen)
{
    int fd_type;
#ifdef API_DEBUG
    printf("In light_getsockname, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("In light_getsockname, (After fd mapping) sock = %d\n", sock);
#endif

    struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;
    in_addr->sin_family = AF_INET;
    if (is_local)
    {
        if (local_socket_descriptors[sock].socket->local_ipaddr)
        {
            in_addr->sin_addr.s_addr = local_socket_descriptors[sock].socket->local_ipaddr;
            in_addr->sin_port = local_socket_descriptors[sock].socket->local_port;
        }
        else
        {
            in_addr->sin_addr.s_addr = local_socket_descriptors[sock].local_ipaddr;
            in_addr->sin_port = local_socket_descriptors[sock].local_port;
        }
    }
    else
    {
        in_addr->sin_addr.s_addr = local_socket_descriptors[sock].remote_ipaddr;
        in_addr->sin_port = local_socket_descriptors[sock].remote_port;
    }
}

static inline void _light_free_common_notification_buf(light_cmd_t *cmd)
{
    rte_mempool_put(free_command_pool, (void *)cmd);
}

static inline void _light_free_return_value_notification_buf(light_return_value_t *res)
{
    rte_mempool_put(free_return_value_pool, (void *)res);
}

static inline void _light_free_epoll_node_notification_buf(light_epoll_node_t *node)
{
    rte_mempool_put(free_epoll_node_pool, (void *)node);
}

inline int _light_is_connected(int sock)
{
    return local_socket_descriptors[sock].any_event_received;
}


static inline void _reset_epoll_object(light_epoll_t *pEpoll)
{
    light_event_info_t *temp_event_info;
    while (!rte_ring_dequeue(pEpoll->ready_connections, (void **)(&temp_event_info))) {
        rte_mempool_put(free_event_info_pool, (void *)temp_event_info);
    }

    while (!rte_ring_dequeue(pEpoll->shadow_connections, (void **)(&temp_event_info))) {
        rte_mempool_put(free_event_info_pool, (void *)temp_event_info);
    }
    pEpoll->is_sleeping = 0;
    pEpoll->is_active = 1;
    rte_rwlock_init(&(pEpoll->ep_lock));
}

static inline int _light_check_data(int sockid)
{

    if (rte_atomic16_read( &(local_socket_descriptors[sockid].socket->is_multicore)))
    {
        int i, socket;
        for (i = 0; i < num_procs; ++i)
        {
            socket = local_socket_descriptors[sockid].socket->multicore_socket_copies[i];
            if (rte_ring_count(local_socket_descriptors[socket].rx_ring) ||
                    rte_ring_count(local_socket_descriptors[socket].local_cache) ||
                    local_socket_descriptors[socket].shadow)
            {
                //light_log(LIGHT_LOG_ERR, "check sock(%d), have data", socket);
                return 1;
            }
            else
            {
                //light_log(LIGHT_LOG_ERR, "check sock(%d), no data", socket);
            }
        }
    }
    else
    {
        if (rte_ring_count(local_socket_descriptors[sockid].rx_ring) ||
                rte_ring_count(local_socket_descriptors[sockid].local_cache) ||
                local_socket_descriptors[sockid].shadow)
            return 1;
    }


    return 0;
}

static inline int _light_check_buffer(int sockid)
{
    if (_light_get_socket_tx_space(sockid) > 0)
        return 1;
    return 0;
}

static inline light_epoll_node_t* _search_epoll_node_in_socket(int epoll_index, light_socket_t *socket)
{
    int is_exist = 0;
    light_epoll_node_t *o;
    LL_FOREACH(o, light_epoll_node_list, &(socket->epoll_node_list))
    {
        if (o->epoll_idx == epoll_index)
        {
            is_exist = 1;
            break;
        }
    }

    if (is_exist)
        return o;
    else
        return NULL;
}

static inline int _delete_epoll_node_in_socket(int epoll_index, light_socket_t *socket)
{
    light_epoll_node_t *o;
    LL_FOREACH(o, light_epoll_node_list, &(socket->epoll_node_list))
    {
        if (o->epoll_idx == epoll_index)
        {
            //LL_REF(light_epoll_node_list, &(socket->epoll_node_list), o);
            LL_UNLINK(light_epoll_node_list, &(socket->epoll_node_list), o);
            o->epoll_idx = -1;
            o->epoll_events = 0;
            o->ready_events = 0;

            _light_free_epoll_node_buf(o);
            return 0;
        }
    }

    return -1;

}

static inline void _add_epoll_node_in_socket(light_epoll_node_t* epoll_node, light_socket_t *socket)
{
    LL_PUSH_FRONT(light_epoll_node_list, &(socket->epoll_node_list), epoll_node);
}



static inline int _add_epoll_event_to_shadow(light_epoll_t *ep, light_socket_t *pSock, uint32_t event)
{
    if (!ep || !pSock || !event)
    {
        return -1;
    }
    // rte_rwlock_write_lock(&ep->ep_lock);
    light_epoll_node_t* epoll_node = _search_epoll_node_in_socket(ep->epoll_idx, pSock);

    if (epoll_node == NULL) {
        // rte_rwlock_write_unlock(&ep->ep_lock);
        return -1;
    }

    // rte_rwlock_write_lock(&ep->ep_lock);
    if (epoll_node->ready_events & event)
    {
        LL_RELEASE(light_epoll_node_list, &(pSock->epoll_node_list), epoll_node);
        // rte_rwlock_write_unlock(&ep->ep_lock);
        return 0;
    }

    light_event_info_t *event_info;
    if (rte_mempool_get(free_event_info_pool, (void **)&event_info)) {
        light_log(LIGHT_LOG_ERR, "%s:%d cannot get node from free event info pool", __FILE__, __LINE__);
        // rte_rwlock_write_unlock(&ep->ep_lock);
        return -1;
    }

    event_info->sockfd = pSock->connection_idx;
    event_info->event = event;
    if (rte_ring_enqueue(ep->shadow_connections, (void *)event_info) == -ENOBUFS) {
        printf("Error: rte_ring_enqueue(ep->shadow_connections, (void *)event_info) == -ENOBUFS\n");
        rte_mempool_put(free_event_info_pool, (void *)event_info);
        LL_RELEASE(light_epoll_node_list, &(pSock->epoll_node_list), epoll_node);
        // rte_rwlock_write_unlock(&ep->ep_lock);
        return -1;
    }

    rte_rwlock_write_lock(&ep->ep_lock);
    epoll_node->ready_events |= event;
    LL_RELEASE(light_epoll_node_list, &(pSock->epoll_node_list), epoll_node);
    rte_rwlock_write_unlock(&ep->ep_lock);

    return 0;
}

static inline int _event_avaliable(int event, int sockid)
{
    if ((event & LIGHT_EPOLLIN) && _light_check_data(sockid)) return 1;
    else if ((event & LIGHT_EPOLLOUT) && _light_check_buffer(sockid)) return 1;
    else if ((event & LIGHT_EPOLLERR) || (event & LIGHT_EPOLLHUP)) return 1;
    return 0;
}

static inline int _epoll_close(int sock)
{
    light_epoll_t *ep = epolls[sock].private_data;
    light_socket_t *sk = NULL;
    int light_fd;
#ifdef API_DEBUG
    printf("@ In _epoll_close, sock = %d\n", sock);
#endif

#ifdef LIGHT_FD_MAPPING
    light_fd = socket_epoll_index_to_light_fd(&sock, LIGHT_EPOLL_INDEX);
    if ((put_back_light_fd(&light_fd)) != 0){
        light_log(LIGHT_LOG_ERR, "put_back_light_fd error\n", __FILE__, __LINE__);
        printf("put_back_light_fd error: %s(errno: %d)\n",strerror(errno),errno);  
        exit(0);
    }
#endif

    if (!ep || !ep->is_active)
    {
        return -1;
    }
    real_close(ep->kernel_epoll_fd);
    int i;
    for ( i = 0; i < num_procs; ++i)
    {
        real_close(ep->fifo_read_fds[i]);
    }
    light_cmd_t *cmd;
    int error = 0;
    for (i = 0; i < num_procs; ++i)
    {
        cmd = light_get_free_command_buf_by_proc_id(i);
        if (!cmd)
        {
            light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
            light_stats_cannot_allocate_cmd++;
            return -2;
        }
        cmd->cmd = LIGHT_EPOLL_CLOSE_COMMAND;
        cmd->ringset_idx = -1;
        cmd->parent_idx = -1;
        cmd->u.epoll_close.epoll_idx = ep->epoll_idx;
        cmd->u.epoll_close.pid = ep->pid;
        if (light_enqueue_command_buf_by_proc_id(cmd, i))
        {
            _light_free_command_buf_by_proc_id(cmd, i);
            error = 1;
        }
    }
    if (error)
        return -3;

    local_socket_descriptor_t *iter = TAILQ_FIRST(&epolls[sock].socket_list), *next;
    while (iter != NULL)
    {
        next = TAILQ_NEXT(iter, socket_list_entry);
        sk = iter->socket;

        light_epoll_node_t* epoll_node = _search_epoll_node_in_socket(ep->epoll_idx,  sk);
        if (epoll_node != NULL)
        {
            LL_UNLINK(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);

        }

        sk->send_timeout.tv_sec = -1;
        sk->send_timeout.tv_usec = -1;
        sk->receive_timeout.tv_sec = -1;
        sk->receive_timeout.tv_usec = -1;

        TAILQ_REMOVE(&epolls[sock].socket_list, iter, socket_list_entry);
        iter = next;
    }
    epolls[sock].private_data = NULL;
    return 0;
}

inline int light_epoll_create(int size)
{
    int light_fd;
    if (size < 0)
    {
        errno = EINVAL;
        return -1;
    }
    light_epoll_t *epoll;
    if (rte_ring_dequeue(epoll_ring, (void **)&epoll))
    {
        printf("Error: rte_ring_dequeue(epoll_ring, (void **)&epoll) != 0\n");
        light_log(LIGHT_LOG_ERR, "cannot get epoll %s %d\n", __FILE__, __LINE__);
        return -1;
    }
    epolls[epoll->epoll_idx].private_data = epoll;
    _reset_epoll_object(epoll);
    TAILQ_INIT(&epolls[epoll->epoll_idx].socket_list);

    epoll->kernel_epoll_fd = real_epoll_create(1);
    epoll->pid = getpid();
    if (epoll->kernel_epoll_fd < 0)
    {
        light_log(LIGHT_LOG_ERR, "cannot get kernel_epoll_fd %s %d\n", __FILE__, __LINE__);
        //回收epoll
        return -1;
    }

    //放cmd通知协议栈open fifo_write_fd
    light_cmd_t *cmd;
    char fifo_name[1024];
    int i;
    for ( i = 0; i < num_procs; ++i)
    {
        sprintf(fifo_name, "/tmp/epoll_fifo_%d%d%d", epoll->epoll_idx, epoll->pid, i);
        if (mkfifo(fifo_name, 0666) < 0 && errno != EEXIST)
        {
            light_log(LIGHT_LOG_ERR, "cannot mkfifo %s %d\n", __FILE__, __LINE__);
            //回收epoll
            _epoll_close(epoll->epoll_idx);
            return -1;
        }
        if ((epoll->fifo_read_fds[i] = open(fifo_name, O_RDWR | O_NONBLOCK)) < 0)   // 管道用O_RDWR标志打开，否则会阻塞,这里只打开read
        {
            light_log(LIGHT_LOG_ERR, "Fail to open fifo %s %d\n", __FILE__, __LINE__);
            //回收epoll
            _epoll_close(epoll->epoll_idx);
            return -1;
        }

        cmd = light_get_free_command_buf_by_proc_id(i);
        if (!cmd)
        {
            light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
            light_stats_cannot_allocate_cmd++;
            return -2;
        }
        cmd->cmd = LIGHT_EPOLL_CREATE_COMMAND;
        cmd->ringset_idx = -1;
        cmd->parent_idx = -1;
        cmd->u.epoll_create.epoll_idx = epoll->epoll_idx;
        cmd->u.epoll_create.pid = epoll->pid;

        if (light_enqueue_command_buf_by_proc_id(cmd, i))
        {
            light_log(LIGHT_LOG_ERR, "epoll create command enqueue fail in proc_num: %d\n", i);
            _light_free_command_buf_by_proc_id(cmd, i);
            //回收epoll
            real_close(epoll->kernel_epoll_fd);
            int i;
            for ( i = 0; i < num_procs; ++i)
            {
                real_close(epoll->fifo_read_fds[i]);
            }
            unlink(fifo_name);
            _epoll_close(epoll->epoll_idx);
            return -3;
        }
    }

    for ( i = 0; i < num_procs; ++i)  // 把fifo加到原生epoll中
    {
        struct epoll_event event;
        event.data.fd = epoll->fifo_read_fds[i];
        event.events = EPOLLIN;
        if (real_epoll_ctl(epoll->kernel_epoll_fd, EPOLL_CTL_ADD, epoll->fifo_read_fds[i], &event) < 0)
        {
            light_log(LIGHT_LOG_ERR, "add fifo event to kernel epoll fail in proc_num: %d\n", i);
            real_close(epoll->kernel_epoll_fd);
            real_close(epoll->fifo_read_fds[i]);
            //unlink(fifo_name);
            _epoll_close(epoll->epoll_idx);
            return -3;
        }
    }
    light_log(LIGHT_LOG_INFO, "epoll create success epoll_idx:%d \n", epoll->epoll_idx);
    

#ifdef TWO_SIDE_FD_SEPARATION
    return INT_MAX - ((epoll->epoll_idx) + LIGHT_CONNECTION_POOL_SIZE);
#endif

#ifdef LIGHT_FD_MAPPING
    light_fd = get_new_light_fd(&(epoll->epoll_idx), LIGHT_EPOLL_INDEX);
    return light_fd;
#endif
}



inline int light_epoll_ctl(int epfd, int op, int fd, void* events)
{
    int fd_type;
    #ifdef EPOLL_DEBUG
    printf("@ In light_epoll_ctl, (Before fd mapping) epfd = %d, sockfd = %d\n", epfd, fd);
    #endif

    #ifdef TWO_SIDE_FD_SEPARATION
    epfd = INT_MAX - epfd;
    epfd -= LIGHT_CONNECTION_POOL_SIZE;
    #endif

    #ifdef LIGHT_FD_MAPPING
    epfd = light_fd_to_socket_epoll_index(&epfd, &fd_type);
    #endif

    #ifdef EPOLL_DEBUG
    printf("(After fd mapping) epfd = %d\n", epfd);
    #endif

    #ifdef TWO_SIDE_FD_SEPARATION
    if (fd > LIGHT_FD_THRESHOLD)
    #endif

    #ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&fd) == 1)
    #endif
    {
        light_log(LIGHT_LOG_INFO, "light fd=%d, pid=%d\n", fd, getpid());

        #ifdef TWO_SIDE_FD_SEPARATION
        fd = INT_MAX - fd;
        #endif

        #ifdef LIGHT_FD_MAPPING
        fd = light_fd_to_socket_epoll_index(&fd, &fd_type);
        #endif

        #ifdef EPOLL_DEBUG
        printf("(After fd mapping) fd = %d\n", fd);
        #endif
        // epfd -= LIGHT_CONNECTION_POOL_SIZE;
        if (epfd < 0)
        {
            errno = EINVAL;
            light_log(LIGHT_LOG_ERR, "%s:%d epoll fd illegal\n", __FILE__, __LINE__);
            return -1;
        }
        if (op < LIGHT_EPOLL_CTL_ADD || op > LIGHT_EPOLL_CTL_MOD)
        {
            errno = EINVAL;
            light_log(LIGHT_LOG_ERR, "%s:%d light epoll event illegal op=%d\n", __FILE__, __LINE__, op);
            return -1;
        }
        if (fd < 0 || fd >= LIGHT_CONNECTION_POOL_SIZE)
        {
            errno = EINVAL;
            light_log(LIGHT_LOG_ERR, "%s:%d fd illegal\n", __FILE__, __LINE__);
            printf("invalid fd number: %s(errno: %d)\n",strerror(errno),errno);  
            exit(0);
            return -1;
        }
        /*
        if (rte_atomic16_read( &(local_socket_descriptors[fd].socket->is_multicore)))//检查是否是listenfd
        {   
            int ret = 0, t_ret = 0;
            struct light_epoll_event *ev = (struct light_epoll_event *)events;
            for(int i = 0; i < num_procs; i ++)
            {
                t_ret = _light_epoll_ctl(epfd, op, local_socket_descriptors[fd].socket->multicore_socket_copies[i],ev);
                ret = (ret == 0 && t_ret == 0) ? 0 : -1;
                light_log(LIGHT_LOG_ERR, "%s:%d operate listenfd(%d)to epoll(%d)\n", __FILE__, __LINE__,
                                    local_socket_descriptors[fd].socket->multicore_socket_copies[i],epfd);
            }
            return ret;
        }
        else
        {
            return _light_epoll_ctl(epfd, op, fd, events);
        }
        */
        return _light_epoll_ctl(epfd, op, fd, events);
    }
    else
    {
        light_log(LIGHT_LOG_INFO,"kernel fd=%d, pid= %d\n", fd, getpid());
        // epfd -= LIGHT_CONNECTION_POOL_SIZE;
        if (epfd < 0)
        {
            errno = EINVAL;
            light_log(LIGHT_LOG_ERR, "%s:%d epoll fd illegal\n", __FILE__, __LINE__);
            return -1;
        }
        light_epoll_t *pEpoll = epolls[epfd].private_data;
        return real_epoll_ctl(pEpoll->kernel_epoll_fd, op, fd, events);
    }
}

inline int _light_epoll_ctl(int epfd, int op, int fd, void *events){
    //fd被转换为local_socket_descriptors的下标，且保证合法
    //epfd是epolls数组的合法下标
    //op是合法操作
    #ifdef EPOLL_DEBUG
    printf("@ In _light_epoll_ctl, epfd = %d, sockfd = %d\n", epfd, fd);
    #endif

    static struct timeval tv1;
    static long timestamp1;

    #ifdef EPOLL_DEBUG
    gettimeofday(&tv1, NULL);
    timestamp1 = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
    printf("  start time of _light_epoll_ctl = %ld us\n", timestamp1);
    #endif

    light_socket_t *pSock = local_socket_descriptors[fd].socket;
    
    if( rte_atomic16_read( &(local_socket_descriptors[fd].socket->is_multicore)) ){
        fd = pSock->multicore_socket_copies[ stack_proc_id ];
        pSock = local_socket_descriptors[ fd ].socket;
        #ifdef API_DEBUG
        printf("epoll ctl, add epoll node %d to sock %d\n", epfd, fd);
        #endif
    }

    light_epoll_t *pEpoll = epolls[epfd].private_data;
    // struct light_epoll_event *ev = (struct light_epoll_event *)events;
    light_epoll_event *ev = (light_epoll_event *)events;
    if (!pSock || !pEpoll || (op != LIGHT_EPOLL_CTL_DEL && !ev))
    {
        errno = EINVAL;
        light_log(LIGHT_LOG_ERR, "%s:%d epoll null error\n", __FILE__, __LINE__);
        return -1;
    }
    if (!pEpoll->is_active)
    {
        errno = EINVAL;
        light_log(LIGHT_LOG_ERR, "%s:%d epoll not active\n", __FILE__, __LINE__);
        return -1;
    }
    if (op == LIGHT_EPOLL_CTL_ADD)
    {
        light_log(LIGHT_LOG_INFO, "%s:%d add fd(%d) to epoll(%d), event(%x)\n", __func__, __LINE__,fd,epfd,ev->events);
        #ifdef EPOLL_DEBUG
        printf("  _light_epoll_ctl: add fd(%d) to epoll(%d)\n", fd, epfd);
        #endif
        light_epoll_node_t* node = _search_epoll_node_in_socket(epfd, pSock);
        if (node != NULL)
        {
            errno = EEXIST;
            light_log(LIGHT_LOG_ERR, "line=%d epoll node already exists.\n", __LINE__);
            printf("  _light_epoll_ctl: epoll node already exists.\n");
            LL_RELEASE(light_epoll_node_list, &(pSock->epoll_node_list), node);
            return -1;
        }
        uint32_t event = ev->events;
        event |= (LIGHT_EPOLLERR | LIGHT_EPOLLHUP);
        light_epoll_node_t *epoll_node;
        epoll_node = light_get_free_epoll_node_buf();
        if (!epoll_node)
        {
            light_stats_cannot_allocate_cmd++;
            light_log(LIGHT_LOG_ERR, "%s:%d can't allocate epoll node\n", __FILE__, __LINE__);
            return -2;
        }
        epoll_node->epoll_events = event;
        epoll_node->epoll_idx = epfd;
        epoll_node->ready_events = 0;
        epoll_node->ep_data = ev->data;

        LL_INIT_ENTRY( &(epoll_node->entry) );

        // rte_rwlock_write_lock(&pSock->ep_sock_lock);
        _add_epoll_node_in_socket(epoll_node, pSock);
        // rte_rwlock_write_unlock(&pSock->ep_sock_lock);

        #ifdef EPOLL_DEBUG
        gettimeofday(&tv1, NULL);
        timestamp1 = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
        printf("  end time of _add_epoll_node_in_socket = %ld us\n", timestamp1);
        #endif

        if ((event & LIGHT_EPOLLIN) && _light_check_data(fd))
        {
            int res = _add_epoll_event_to_shadow(pEpoll, pSock, LIGHT_EPOLLIN);
            if (res != 0) {
                light_log(LIGHT_LOG_ERR, "line=%d can't add target epoll node to shadow\n", __LINE__);
                printf("  can't add target epoll node to shadow\n");
            }
            #ifdef EPOLL_DEBUG
            printf("  _light_epoll_ctl: LIGHT_EPOLLIN, _add_epoll_event_to_shadow\n");
            #endif

            #ifdef EPOLL_DEBUG
            gettimeofday(&tv1, NULL);
            timestamp1 = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
            printf("  end time of _add_epoll_event_to_shadow = %ld us\n", timestamp1);
            #endif
        }

        if ((event & LIGHT_EPOLLOUT) && _light_check_buffer(fd))
        {
            int res = _add_epoll_event_to_shadow(pEpoll, pSock, LIGHT_EPOLLOUT);
            if (res != 0) {
                light_log(LIGHT_LOG_ERR, "%s:%d can't add target epoll node to shadow\n", __FILE__, __LINE__);
                printf("  can't add target epoll node to shadow\n");
            }
            #ifdef EPOLL_DEBUG
            printf("  _light_epoll_ctl: LIGHT_EPOLLOUT, _add_epoll_event_to_shadow\n");
            #endif

            #ifdef EPOLL_DEBUG
            gettimeofday(&tv1, NULL);
            timestamp1 = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
            printf("  end time of _add_epoll_event_to_shadow = %ld us\n", timestamp1);
            #endif
        }

        TAILQ_INSERT_TAIL(&epolls[epfd].socket_list, &local_socket_descriptors[fd], socket_list_entry);
    }
    else if (op == LIGHT_EPOLL_CTL_DEL)
    {

        int res = _delete_epoll_node_in_socket(epfd, pSock);
        if (res == -1) {
            light_log(LIGHT_LOG_ERR, "%s:%d can't delete target epoll node\n", __FILE__, __LINE__);
            return -1;
        }
        TAILQ_REMOVE(&epolls[epfd].socket_list, &local_socket_descriptors[fd], socket_list_entry);
    }
    else if (op == LIGHT_EPOLL_CTL_MOD)
    {
        light_epoll_node_t* epoll_node = _search_epoll_node_in_socket(epfd, pSock);
        if (epoll_node == NULL)
        {
            errno = EEXIST;
            light_log(LIGHT_LOG_ERR, "%s:%d can't find target epoll node\n", __FILE__, __LINE__);
            return -1;
        }
        uint32_t event = ev->events;
        event |= (LIGHT_EPOLLERR | LIGHT_EPOLLHUP);
        epoll_node->epoll_events = event;
        epoll_node->ep_data = ev->data;
        LL_RELEASE(light_epoll_node_list, &(pSock->epoll_node_list), epoll_node);
        if ((event & LIGHT_EPOLLIN) && _light_check_data(fd))
        {
            int res = _add_epoll_event_to_shadow(pEpoll, pSock, LIGHT_EPOLLIN);
            if (res != 0) {
                light_log(LIGHT_LOG_ERR, "%s:%d can't add target epoll node to shadow\n", __FILE__, __LINE__);
            }
        }
        if ((event & LIGHT_EPOLLOUT) && _light_check_buffer(fd))
        {
            int res = _add_epoll_event_to_shadow(pEpoll, pSock, LIGHT_EPOLLOUT);
            if (res != 0) {
                light_log(LIGHT_LOG_ERR, "%s:%d can't add target epoll node to shadow\n", __FILE__, __LINE__);
            }
        }
    }
#ifdef API_DEBUG
    printf("@ Successful End of _light_epoll_ctl\n");
#endif

    return 0;
}

inline int
light_epoll_wait(int epfd, void * events, int maxevents, int timeout)
{
    // only for debug
    static int event_count_api;
    static int ready_connections_count;
    static int wake_up_count; // the number of wake-up
    static int api_count; // the number of api calls
    static long counter_total;
    static long counter_wake_up; // the number of events that we get after wake-up.
    static float counter_average;
    static float counter_average_per_wake_up;
    static struct timeval tv1;
    static long epoll_return_timestamp;
    int wake_up_flag = 0; // if wake_up_flag = 1, epoll wakes up from sleep.
    static int idle_count; // the times that it polls no events.

    int fd_type;
    
    #ifdef EPOLL_DEBUG
    printf("@ In light_epoll_wait, (Before fd mapping) epfd = %d\n", epfd);
    #endif

    #ifdef TWO_SIDE_FD_SEPARATION
    epfd = INT_MAX - epfd;
    epfd -= LIGHT_CONNECTION_POOL_SIZE;
    #endif

    #ifdef LIGHT_FD_MAPPING
    epfd = light_fd_to_socket_epoll_index(&epfd, &fd_type);
    #endif

    #ifdef EPOLL_DEBUG
    printf("(After fd mapping) epfd = %d\n", epfd);
    #endif

    int sockets[maxevents];
    if (epfd < 0)
    {
        errno = EINVAL;
        light_log(LIGHT_LOG_ERR, "%s:%d epoll fd illegal\n", __FILE__, __LINE__);
        return -1;
    }
    // struct light_epoll_event *st = (struct light_epoll_event *)events;
    light_epoll_event *st = (light_epoll_event *)events;
    int ret, sockid, ready_event, valid, flag, i;
    int counter = 0;
    char buf[100];
    light_epoll_t *ep = epolls[epfd].private_data;
    light_socket_t *sk = NULL;

    if (!ep || !ep->is_active) // when epoll is created, ep->is_active = 1.
    {
        errno = EINVAL;
        light_log(LIGHT_LOG_ERR, "%s:%d epoll null error or epoll not active\n", __FILE__, __LINE__);
        return -1;
    }
    flag = 0;

wait:

    #ifdef BUSY_EPOLL
    while ((rte_ring_count(ep->shadow_connections) == 0) && (rte_ring_count(ep->ready_connections) == 0));
    #endif

    #ifdef ADAPT_EPOLL
    idle_count = 0;
    while ((rte_ring_count(ep->shadow_connections) == 0) && (rte_ring_count(ep->ready_connections) == 0)) {
        idle_count ++;
        if (idle_count >= 1000) {
            // if no event, go to sleep.
            ep->is_sleeping = 1;
            light_log(LIGHT_LOG_INFO, "kernel epoll waiting\n");
            ret = real_epoll_wait(ep->kernel_epoll_fd, ep->epoll_events, 64, timeout); // block
            
            // if new event comes, wake up.
            #ifdef EPOLL_DEBUG_COUNT
            wake_up_count ++;
            wake_up_flag = 1;
            if (wake_up_count % 1000 == 999){
                printf("wake_up_count = %d\n", wake_up_count);
            }
            #endif

            int i;
            for (i = 0; i < num_procs; ++i)
            {
                real_read(ep->fifo_read_fds[i], buf, 100);
            }

            #ifdef LIGHT_API_DEBUG
            printf("after real_read\n");
            buf[99] = '\0';
            printf("buf=%s\n", buf);
            #endif

            ep->is_sleeping = 0;
            if (!ret)
            {
                flag = 1;
            }
            break;
        }
    }
    #endif

    #ifdef SLEEP_EPOLL
    if (!rte_ring_count(ep->ready_connections) && !rte_ring_count(ep->shadow_connections) && timeout != 0)
    {
        // if no event, go to sleep.
        ep->is_sleeping = 1;
        light_log(LIGHT_LOG_INFO, "kernel epoll waiting\n");
        ret = real_epoll_wait(ep->kernel_epoll_fd, ep->epoll_events, 64, timeout); // block
        
        // if new event comes, wake up.
        #ifdef EPOLL_DEBUG_COUNT
        wake_up_count ++;
        wake_up_flag = 1;
        if (wake_up_count % 1000 == 999){
            printf("wake_up_count = %d\n", wake_up_count);
        }
        #endif

        int i;
        for (i = 0; i < num_procs; ++i)
        {
            real_read(ep->fifo_read_fds[i], buf, 100);
        }

        #ifdef LIGHT_API_DEBUG
        printf("after real_read\n");
        buf[99] = '\0';
        printf("buf=%s\n", buf);
        #endif

        ep->is_sleeping = 0;
        if (!ret)
        {
            flag = 1;
        }
    }
    else  // 如果网络有事件，还是需要检查一下文件是否有事件，所以走一次timeout为0的epoll_wait
    {
        ret = real_epoll_wait(ep->kernel_epoll_fd, ep->epoll_events, 64, 0); // non-block
        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            real_read(ep->fifo_read_fds[i], buf, 100);  //这里我还是把它读掉，因为反正不影响，这个read也是异步的
        }
    }
    #endif

get_event:
    // rte_rwlock_write_lock(&ep->ep_lock);
    // printf("test\n");
    light_log(LIGHT_LOG_DEBUG, "epoll %u get epoll lock\n", ep->epoll_idx);
    light_event_info_t *event_info;
    while (counter < maxevents && !rte_ring_dequeue(ep->shadow_connections, (void **)&event_info))
    {
        // printf("dequeue from ep->shadow_connections\n");
        sockid = event_info->sockfd;
        ready_event = event_info->event;
        rte_mempool_put(free_event_info_pool, (void *)event_info);
        if (sockid >= LIGHT_CONNECTION_POOL_SIZE)
        {
            light_log(LIGHT_LOG_ERR, "%s:%d socket fd error\n", __FILE__, __LINE__);
            exit(0);
        }
        sk = local_socket_descriptors[sockid].socket;
        if (!sk)
        {
            printf("%s,%d, %d\n", __func__, __LINE__, getpid());
            light_log(LIGHT_LOG_ERR, "%s:%d socket is NULL\n", __FILE__, __LINE__);
            continue;
        }
        valid = 1;
        light_epoll_node_t* epoll_node = _search_epoll_node_in_socket(epfd, sk);

        if (epoll_node != NULL)
        {
            if (!(epoll_node->epoll_events & ready_event))
            {
                valid = 0;
            }
            if (!(epoll_node->ready_events & ready_event))
            {
                valid = 0;
            }
            if (valid && _event_avaliable(ready_event, sockid))
            {
                //对listenfd进行处理，将从listenfd转为主listenfd返回
                /*
                if (rte_atomic16_read( &(sk->is_multicore)))
                {
                    light_epoll_data_t ep_data;
                    ep_data.fd = INT_MAX - local_socket_descriptors[ sockid ].socket->multicore_socket_copies[0];
                    st->data = ep_data;
                    light_log(LIGHT_LOG_ERR, "%s:%d in epoll_wait shadow, %d notified, return %d\n", __FILE__, __LINE__,sockid, ep_data.fd);
                }
                else
                {
                    st->data = epoll_node->ep_data;
                }
                */
                #ifdef EPOLL_DEBUG
                printf("add network event (shadow_connections): Before fd mapping, epoll_node->ep_data.fd = %d\n", (int)(epoll_node->ep_data.fd));
                #endif

                st->data = epoll_node->ep_data; // light socket index, we need to translate it to light_fd
                // st->data.fd = socket_epoll_index_to_light_fd(&(epoll_node->ep_data.fd), LIGHT_SOCKET_INDEX);
                // printf("After fd mapping, st->data.fd = %d\n", st->data.fd);
                st->events = ready_event;
                sockets[counter] = sockid;//light socket index
                ++st;
                ++counter;

                // event_count_api++;
                // if (event_count_api % 1000 == 999){
                //     printf("event_count_api = %d\n", event_count_api);
                // }
            }
            // printf("reset ready event, %s,%d, %d\n", __func__, __LINE__, getpid());
            rte_rwlock_write_lock(&ep->ep_lock);
            epoll_node->ready_events &= ~ready_event; // after taking out the event, clear the flag on bitmap.
            rte_rwlock_write_unlock(&ep->ep_lock);

            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
            // printf("%s,%d, %d\n", __func__, __LINE__, getpid());
        }
        else
        {
            #ifdef EPOLL_DEBUG
            printf("Error: can't find target epoll node: %s,%d, %d\n", __func__, __LINE__, getpid());
            #endif
            light_log(LIGHT_LOG_ERR, "line:%d can't find target epoll node\n", __LINE__);
        }
    }

    // rte_rwlock_write_lock(&ep->ep_lock);
    while (counter < maxevents && !rte_ring_dequeue(ep->ready_connections, (void **)&event_info))
    {
        sockid = event_info->sockfd;
        ready_event = event_info->event;
        light_log(LIGHT_LOG_DEBUG, "line:%d dealing event, sockid(%d), event(%d)\n", __LINE__, sockid, ready_event);

        #ifdef EPOLL_DEBUG
        gettimeofday(&tv1, NULL);
        epoll_return_timestamp = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
        printf("event_dequeue_timestamp = %ld us\n", epoll_return_timestamp);
        #endif

        #ifdef EPOLL_DEBUG_COUNT
        ready_connections_count ++;
        if (ready_connections_count % 1000 == 999){
            printf("ready_connections_count (frontend) = %d\n", ready_connections_count);
        }
        #endif

        rte_mempool_put(free_event_info_pool, (void *)event_info);
        if (sockid >= LIGHT_CONNECTION_POOL_SIZE)
        {
            light_log(LIGHT_LOG_ERR, "%s:%d socket fd error\n", __FILE__, __LINE__);
            exit(0);
        }
        sk = local_socket_descriptors[sockid].socket;
        if (!sk)
        {
            printf("%s,%d, %d\n", __func__, __LINE__, getpid());
            light_log(LIGHT_LOG_ERR, "%s:%d socket is NULL\n", __FILE__, __LINE__);
            continue;
        }
        valid = 1;
        light_epoll_node_t* epoll_node = _search_epoll_node_in_socket(epfd, sk);

        if (epoll_node != NULL)
        {
            if (!(epoll_node->epoll_events & ready_event))
            {
                light_log(LIGHT_LOG_ERR, "%s:%d epoll_events unvalid\n", __FILE__, __LINE__);
                valid = 0;
            }
            if (!(epoll_node->ready_events & ready_event))
            {
                light_log(LIGHT_LOG_ERR, "%s:%d ready_events unvalid\n", __FILE__, __LINE__);
                valid = 0;
            }
            
            if (valid && _event_avaliable(ready_event, sockid))
            {
                //对listenfd进行处理，将从listenfd转为主listenfd返回
                /*
                if (rte_atomic16_read( &(sk->is_multicore)))
                {
                    light_epoll_data_t ep_data;
                    ep_data.fd = INT_MAX - local_socket_descriptors[ sockid ].socket->multicore_socket_copies[0];
                    st->data = ep_data;
                    light_log(LIGHT_LOG_ERR, "%s:%d in epoll_wait ready, %d notified, return %d\n", __FILE__, __LINE__,sockid,ep_data.fd);
                }
                else
                {
                    st->data = epoll_node->ep_data;
                    light_log(LIGHT_LOG_ERR, "%s:%d not is_multicore\n", __FILE__, __LINE__);
                }
                */
                #ifdef EPOLL_DEBUG
                printf("add network event (ready_connections): Before fd mapping, epoll_node->ep_data.fd = %d\n", (int)(epoll_node->ep_data.fd));
                #endif

                st->data = epoll_node->ep_data; // light socket index, we need to translate it to light_fd
                // st->data.fd = socket_epoll_index_to_light_fd(&(epoll_node->ep_data.fd), LIGHT_SOCKET_INDEX);
                // printf("After fd mapping, st->data.fd = %d\n", st->data.fd);
                st->events = ready_event;

                sockets[counter] = sockid;//不改
                ++st;
                ++counter;
                light_log(LIGHT_LOG_DEBUG, "%s:%d in epoll_wait ready, %d notified\n", __FILE__, __LINE__,sockid);

                // event_count_api++;
                // if (event_count_api % 1000 == 999){
                //     printf("event_count_api = %d\n", event_count_api);
                // }
            }
            // printf("reset ready event, %s,%d, %d\n", __func__, __LINE__, getpid());
            rte_rwlock_write_lock(&ep->ep_lock);
            epoll_node->ready_events &= ~ready_event;
            rte_rwlock_write_unlock(&ep->ep_lock);
            // printf("%s,%d, %d\n", __func__, __LINE__, getpid());
            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
        }
        else
        {
            light_log(LIGHT_LOG_ERR, "line=%d can't find target epoll node\n", __LINE__);
        }
    }

    // rte_rwlock_write_lock(&ep->ep_lock);
    // put some events into shadow_connections
    for (i = 0, st = events; i < counter; ++i, ++st)
    {
        sk = local_socket_descriptors[sockets[i]].socket;
        light_epoll_node_t* epoll_node = _search_epoll_node_in_socket(epfd, sk);
        if (epoll_node != NULL)
        {
            if ((epoll_node->ready_events & st->events)
                    || (epoll_node->epoll_events & LIGHT_EPOLLET) //LIGHT_EPOLLET only notify for once; LIGHT_EPOLLET will keep notifying.
                    || (st->events & LIGHT_EPOLLHUP)
                    || (st->events & LIGHT_EPOLLERR))
            {
                LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
                continue;
            }

            if (rte_mempool_get(free_event_info_pool, (void **)&event_info)) {
                light_log(LIGHT_LOG_ERR, "%s:%d cannot get free event info node\n", __FILE__, __LINE__);
                break;
            }

            event_info->sockfd = sockets[i];
            event_info->event = st->events;
            if (rte_ring_enqueue(ep->shadow_connections, (void *)event_info) == -ENOBUFS)
            {
                printf("Error: rte_ring_enqueue(ep->shadow_connections, (void *)event_info) == -ENOBUFS\n");
                light_log(LIGHT_LOG_ERR, "%s:%d cannot enqueue to shadow ring\n", __FILE__, __LINE__);
                rte_mempool_put(free_event_info_pool, (void *)event_info);
                LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
                break;
            }
            else
            {
                rte_rwlock_write_lock(&ep->ep_lock);
                epoll_node->ready_events |= st->events;
                rte_rwlock_write_unlock(&ep->ep_lock);
            }
            LL_RELEASE(light_epoll_node_list, &(sk->epoll_node_list), epoll_node);
        }
        else
        {
            light_log(LIGHT_LOG_ERR, "line:%d can't find target epoll node\n", __LINE__);
        }
    }
    light_log(LIGHT_LOG_DEBUG, "epoll %u release epoll lock\n", ep->epoll_idx);
    // rte_rwlock_write_unlock(&ep->ep_lock);
    i = 0;
    
    // add non-network events
    int should_memcpy = 1;
    while (counter < maxevents && i < ret)
    {
        // deal with non-network events
        should_memcpy = 1;
        int j;
        for ( j = 0; j < num_procs; ++j)
        {
            #ifdef LIGHT_API_DEBUG
            printf("data.fd = %d  fifo_read_fds = %d\n", ep->epoll_events[i].data.fd, ep->fifo_read_fds[j]);
            #endif

            if (ep->epoll_events[i].data.fd == ep->fifo_read_fds[j])
            {
                should_memcpy = 0;
            }
        }
        if (should_memcpy == 1)
        {
            // rte_memcpy(st, &ep->epoll_events[i], sizeof(struct light_epoll_event));
            rte_memcpy(st, &ep->epoll_events[i], sizeof(light_epoll_event));
            ++st;
            ++counter;
            //light_log(LIGHT_LOG_ERR, "sys file event\n");
        }
        ++i;
    }
    
    #ifdef EPOLL_DEBUG
    light_log(LIGHT_LOG_INFO, "  event list of epfd(%d):\n", epfd);
    printf("  event list of epfd(%d):\n", epfd);
    for (i = 0, st = events; i < counter; ++i, ++st)
    {
       light_log(LIGHT_LOG_INFO, "sockfd = %d, return = %d, event = %zu\n", sockets[i], st->data.fd, st->events);
       printf("sockfd = %d, return = %d, event = %zu\n", sockets[i], st->data.fd, st->events);
    }
    #endif

    if ((flag || counter) && (counter >= 1)){
        light_log(LIGHT_LOG_INFO, "return counter:%d\n", counter);

        #ifdef EPOLL_DEBUG
        gettimeofday(&tv1, NULL);
        epoll_return_timestamp = (tv1.tv_sec) * 1000000 + (tv1.tv_usec);
        printf("epoll_return_timestamp = %ld us, flag = %d, counter = %d\n", epoll_return_timestamp, flag, counter);
        #endif

        #ifdef EPOLL_DEBUG_COUNT
        counter_total += counter;
        if(wake_up_flag){
            counter_wake_up += counter;
        }
        api_count++;
        if (api_count % 1000 == 999){
            counter_average = (float)counter_total / 1000;
            counter_average_per_wake_up = (float)counter_wake_up / wake_up_count;
            counter_total = 0;
            printf("counter_average = %f, counter_average_per_wake_up = %f, api_count = %d\n, counter = %d, counter_wake_up = %d\n", counter_average, counter_average_per_wake_up, api_count, counter, counter_wake_up);
        }
        #endif

        return counter;
    }
    goto wait;
}

//对listenfd进行处理，将从listenfd转为主listenfd返回
/*
void _epoll_set_event_fd(light_socket_t *sk, light_epoll_node_t* epoll_node)
{   
    //sk保证合法，epoll_node保证合法
    if (rte_atomic16_read( &(sk->is_multicore)))
    {
        light_epoll_data_t ep_data;
        ep_data.fd = local_socket_descriptors[ epoll_node->ep_data.fd ]->multicore_socket_copies[0];
        st->data = ep_data;
    }
    else
    {
        st->data = epoll_node->ep_data;
    }
}
*/
/* receive functions return a chained buffer. this function
   retrieves a next chunk and its length */
inline void *_light_get_next_buffer_segment(void **pdesc, int *len)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)*pdesc;

    mbuf = mbuf->next;
    if (!mbuf)
    {
        return NULL;
    }
    *len = rte_pktmbuf_data_len(mbuf);
    *pdesc = mbuf;
    return rte_pktmbuf_mtod(mbuf, void *);
}

inline void *_light_get_next_buffer_segment_and_detach_first(void **pdesc, int *len)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)*pdesc;

    mbuf = mbuf->next;
    if (!mbuf)
    {
        return NULL;
    }
    *len = rte_pktmbuf_data_len(mbuf);
    *pdesc = mbuf;
    struct rte_mbuf *orig = (struct rte_mbuf *)*pdesc;
    orig->next = NULL;
    orig->nb_segs = 1;
    return rte_pktmbuf_mtod(mbuf, void *);
}

inline int light_setsockopt(int sock, int level, int optname, const void *optval, socklen_t optlen)
{
    if (optlen <= 0 || !optval) return -1;

    int fd_type;
#ifdef API_DEBUG
    printf("@ In light_setsockopt, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("(After fd mapping) sock = %d\n", sock);
#endif

    light_cmd_t *cmd;

    if (rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_multicore)))
    {
        light_log(LIGHT_LOG_INFO, "set multicore_socket(%d) opt(%d)\n", sock, optname);
        //是多核
        //is_multicore=1的情况
        int main_sock = sock;
        //int main_sock_proc_id = local_socket_descriptors[sock].proc_id;

        int i;
        for ( i = 0; i < num_procs; ++i)
        {
            sock = local_socket_descriptors[main_sock].socket->multicore_socket_copies[i];
            if (!local_socket_descriptors[sock].is_init)
            {
                light_log(LIGHT_LOG_ERR, "multi_copies(%d) not init opt\n", main_sock);
                continue;
            }

            cmd = light_get_free_command_buf_by_proc_id(i);
            if (!cmd)
            {
                light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
                light_stats_cannot_allocate_cmd++;
                return -1;
            }
            cmd->cmd = LIGHT_SETSOCKOPT_COMMAND;
            cmd->ringset_idx = sock;
            cmd->u.setsockopt.level = level;
            cmd->u.setsockopt.optname = optname;
            cmd->u.setsockopt.optlen = optlen;
            rte_memcpy(&cmd->u.setsockopt.optval, optval, optlen);
            if (light_enqueue_command_buf_by_proc_id(cmd, i))
            {
                _light_free_command_buf_by_proc_id(cmd, i);
                return -2;
            }
            if (level == SOL_SOCKET && optname == SO_SNDTIMEO)
            {
                const struct timeval* snd_time_out = (const struct timeval*)optval;
                light_log(LIGHT_LOG_INFO, "set sock(%d) send timeout(sec:%d,usec:%d)",sock,snd_time_out->tv_sec,snd_time_out->tv_usec);
                local_socket_descriptors[sock].socket->send_timeout.tv_sec = snd_time_out->tv_sec;
                local_socket_descriptors[sock].socket->send_timeout.tv_usec = snd_time_out->tv_usec;
            }
            if (level == SOL_SOCKET && optname == SO_RCVTIMEO)
            {
                const struct timeval* rcv_time_out = (const struct timeval*)optval;
                light_log(LIGHT_LOG_INFO, "set sock(%d) recv timeout(sec:%d,usec:%d)",sock,rcv_time_out->tv_sec,rcv_time_out->tv_usec);
                local_socket_descriptors[sock].socket->receive_timeout.tv_sec = rcv_time_out->tv_sec;
                local_socket_descriptors[sock].socket->receive_timeout.tv_usec = rcv_time_out->tv_usec;
            }
        }
    }
    else
    {
        //不是多核
        if (!local_socket_descriptors[sock].is_init)
        {
            local_socket_descriptors[sock].is_set_sockopt = 1;
            local_socket_descriptors[sock].level = level;
            local_socket_descriptors[sock].optname = optname;
            local_socket_descriptors[sock].optlen = optlen;
            rte_memcpy(&(local_socket_descriptors[sock].optval), optval, optlen);
            return 0;
        }
        int proc_id = local_socket_descriptors[sock].proc_id;
        cmd = light_get_free_command_buf_by_proc_id(proc_id);
        if (!cmd)
        {
            light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
            light_stats_cannot_allocate_cmd++;
            return -1;
        }
        cmd->cmd = LIGHT_SETSOCKOPT_COMMAND;
        cmd->ringset_idx = sock;
        cmd->u.setsockopt.level = level;
        cmd->u.setsockopt.optname = optname;
        cmd->u.setsockopt.optlen = optlen;
        rte_memcpy(&cmd->u.setsockopt.optval, optval, optlen);
        if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
        {
            _light_free_command_buf_by_proc_id(cmd, proc_id);
            return -2;
        }
        if (level == SOL_SOCKET && optname == SO_SNDTIMEO)
        {
            const struct timeval* snd_time_out = (const struct timeval*)optval;
            light_log(LIGHT_LOG_INFO, "set sock(%d) send timeout(sec:%d,usec:%d)",sock,snd_time_out->tv_sec,snd_time_out->tv_usec);
            local_socket_descriptors[sock].socket->send_timeout.tv_sec = snd_time_out->tv_sec;
            local_socket_descriptors[sock].socket->send_timeout.tv_usec = snd_time_out->tv_usec;
        }
        if (level == SOL_SOCKET && optname == SO_RCVTIMEO)
        {
            const struct timeval* rcv_time_out = (const struct timeval*)optval;
            light_log(LIGHT_LOG_INFO, "set sock(%d) recv timeout(sec:%d,usec:%d)",sock,rcv_time_out->tv_sec,rcv_time_out->tv_usec);
            local_socket_descriptors[sock].socket->receive_timeout.tv_sec = rcv_time_out->tv_sec;
            local_socket_descriptors[sock].socket->receive_timeout.tv_usec = rcv_time_out->tv_usec;
        }
    }
    return 0;
}

#ifdef TWO_SIDE_FD_SEPARATION
inline int light_ioctl(int sockfd, unsigned long request, void *argp)
{
    if (sockfd > LIGHT_FD_THRESHOLD)
    {
        sockfd = INT_MAX - sockfd;
        if (!argp)
            return -1;
        int* pargp = (int*) argp;
        switch (request)
        {
        case FIONREAD:
        {
            if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
            {
                int i;
                int sum = 0;
                for ( i = 0; i < num_procs; ++i)
                {
                    int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
                    sum += rte_ring_count(local_socket_descriptors[sock].rx_ring);
                }
                *pargp = sum;
                return 0;
            }
            else
            {
                *pargp = rte_ring_count(local_socket_descriptors[sockfd].rx_ring);
                return 0;
            }
        }

        case FIONBIO:
            if (*pargp)
            {
                if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
                        rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 1);
                        light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) nonblock\n",sock);
                    }
                }
                else
                {
                    rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 1);
                    light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) nonblock(%d)\n",sockfd,rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_nonblock)) );
                }
            } else {
                light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) block\n",sockfd);
                if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
                        rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 0);
                    }
                }
                else
                {
                    rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 0);
                }
            }
            return 0;
        default:
            return -1;
        }
    }
    else
    {
        //这里是有隐患的，因为ioctl的参数个数是变长的
        return ioctl(sockfd, request, argp);
    }
}
#endif

#ifdef TWO_SIDE_FD_SEPARATION
inline int light_fcntl(int sock, int cmd, long arg)
{
    if (sock > LIGHT_FD_THRESHOLD)
    {
        //需要判断下fd空间，非网络fd交给原生api
        sock = INT_MAX - sock;
        light_log(LIGHT_LOG_INFO,"in light_fcntl, fd(%d), cmd(%d)\n", sock,cmd);
        switch (cmd)
        {
        case F_GETFL:
            return rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) ) ? (2 | O_NONBLOCK) : 2;

        case F_SETFL:
            if (arg & O_NONBLOCK)
            {
                if (rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sockfd = local_socket_descriptors[sock].socket->multicore_socket_copies[i];
                        light_log(LIGHT_LOG_INFO,"set sock(%d) nonblock\n", sockfd);
                        rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 1);
                    }
                }
                else
                {
                    light_log(LIGHT_LOG_INFO,"set sock(%d) nonblock\n", sock);
                    rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 1);
                }
            }

            else
            {
                if (rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sockfd = local_socket_descriptors[sock].socket->multicore_socket_copies[i];
                        rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 0);
                    }
                }
                else
                {
                    rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 0);
                }
            }
            return 0;
        default:
            return -1;
        }
        return -1;
    }
    else
    {
        return fcntl(sock, cmd, arg);
    }
}
#endif

#ifdef LIGHT_FD_MAPPING
inline int light_ioctl(int sockfd, unsigned long request, void *argp)
{
    int fd_type, rc;
#ifdef API_DEBUG
    printf("@ Enter light_ioctl, sockfd = %d, request = %u\n", sockfd, request);
#endif

// #ifdef TWO_SIDE_FD_SEPARATION
//     if (sockfd > LIGHT_FD_THRESHOLD)
// #endif

// #ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sockfd))
// #endif
    {        
#ifdef API_DEBUG
        printf("  (Before fd mapping) sockfd = %d\n", sockfd);
#endif
// #ifdef TWO_SIDE_FD_SEPARATION
//         sockfd = INT_MAX - sockfd;
// #endif

// #ifdef LIGHT_FD_MAPPING
        sockfd = light_fd_to_socket_epoll_index(&sockfd, &fd_type);
// #endif
#ifdef API_DEBUG
        printf("  (After fd mapping) sockfd = %d\n", sockfd);
#endif

        if (!argp)
            return -1;
        int* pargp = (int*) argp;
        switch (request)
        {
        case FIONREAD:
        {
            if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
            {
                int i;
                int sum = 0;
                for ( i = 0; i < num_procs; ++i)
                {
                    int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
                    sum += rte_ring_count(local_socket_descriptors[sock].rx_ring);
                }
                *pargp = sum;
                return 0;
            }
            else
            {
                *pargp = rte_ring_count(local_socket_descriptors[sockfd].rx_ring);
                return 0;
            }
        }

        case FIONBIO:
            if (*pargp)
            {
                if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
                        rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 1);
                        light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) nonblock\n",sock);
                    }
                }
                else
                {
                    rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 1);
                    light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) nonblock(%d)\n",sockfd,rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_nonblock)) );
                }
            } else {
                light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) block\n",sockfd);
                if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
                        rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 0);
                    }
                }
                else
                {
                    rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 0);
                }
            }
            return 0;
        default:
            return -1;
        }
    }
    else
    {
        //这里是有隐患的，因为ioctl的参数个数是变长的
#ifdef API_DEBUG
        printf("  Use real_ioctl, sockfd = %d, request = %u\n", sockfd, request);
#endif
        rc = real_ioctl(sockfd, request, argp);
    }
#ifdef API_DEBUG
    printf("  Return value = %d\n", rc);
#endif
    return rc;
}
#endif


// int light_ioctl(int sockfd, unsigned long request, va_list args)
// {
//     printf("@ Enter light_ioctl, (Before fd mapping) sockfd = %d\n", sockfd);

// #ifdef TWO_SIDE_FD_SEPARATION
//     if (sockfd > LIGHT_FD_THRESHOLD)
// #endif
// #ifdef LIGHT_FD_MAPPING
//     if (is_light_fd(&sockfd) == 1)
// #endif
//     {
//         int fd_type;

// #ifdef TWO_SIDE_FD_SEPARATION
//         sockfd = INT_MAX - sockfd;
// #endif

// #ifdef LIGHT_FD_MAPPING
//         sockfd = light_fd_to_socket_epoll_index(&sockfd, &fd_type);
// #endif
//         printf("  (After fd mapping) sockfd = %d\n", sockfd);

//         if (!args)
//             return -1;
//         void *argp = va_arg(args, void*);
//         int* pargp = (int*) argp;
//         switch (request)
//         {
//         case FIONREAD:
//             {
//                 if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
//                 {
//                     int i;
//                     int sum = 0;
//                     for ( i = 0; i < num_procs; ++i)
//                     {
//                         int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
//                         sum += rte_ring_count(local_socket_descriptors[sock].rx_ring);
//                     }
//                     *pargp = sum;
//                     return 0;
//                 }
//                 else
//                 {
//                     *pargp = rte_ring_count(local_socket_descriptors[sockfd].rx_ring);
//                     return 0;
//                 }
//             }

//         case FIONBIO:
//             if (*pargp)
//             {
//                 if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
//                 {
//                     int i;
//                     for ( i = 0; i < num_procs; ++i)
//                     {
//                         int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
//                         rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 1);
//                         light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) nonblock\n",sock);
//                     }
//                 }
//                 else
//                 {
//                     rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 1);
//                     light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) nonblock(%d)\n",sockfd,rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_nonblock)) );
//                 }
//             } else {
//                 light_log(LIGHT_LOG_INFO, "ioctl set fd(%d) block\n",sockfd);
//                 if (rte_atomic16_read( &(local_socket_descriptors[sockfd].socket->is_multicore)))
//                 {
//                     int i;
//                     for ( i = 0; i < num_procs; ++i)
//                     {
//                         int sock = local_socket_descriptors[sockfd].socket->multicore_socket_copies[i];
//                         rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 0);
//                     }
//                 }
//                 else
//                 {
//                     rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 0);
//                 }
//             }
//             return 0;
//         default:
//             return -1;
//         }
//     }
//     else
//     {
//         printf("  Use real_ioctl\n");
//         return real_ioctl(sockfd, request, args);
//     }
// }


#ifdef LIGHT_FD_MAPPING
inline int light_fcntl(int sock, int cmd, long arg)
{
    int fd_type;
    int rc;
#ifdef API_DEBUG
    printf("@ Enter light_fcntl, sock = %d, cmd = %d, arg = %d\n", sock, cmd, arg);
#endif
// #ifdef TWO_SIDE_FD_SEPARATION
//     if (sock > LIGHT_FD_THRESHOLD)
// #endif

// #ifdef LIGHT_FD_MAPPING
    if (is_light_fd(&sock) == 1)
// #endif
    {
        //需要判断下fd空间，非网络fd交给原生api
#ifdef API_DEBUG
        printf("  (Before fd mapping) sock = %d\n", sock);
#endif
// #ifdef TWO_SIDE_FD_SEPARATION
//         sock = INT_MAX - sock;
// #endif

// #ifdef LIGHT_FD_MAPPING
        sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
// #endif
#ifdef API_DEBUG
        printf("  (After fd mapping) sock = %d\n", sock);
#endif

        light_log(LIGHT_LOG_INFO,"in light_fcntl, fd(%d), cmd(%d)\n", sock,cmd);
        switch (cmd)
        {
        case F_GETFL:
            return rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_nonblock) ) ? (2 | O_NONBLOCK) : 2;

        case F_SETFL:
            if (arg & O_NONBLOCK)
            {
                if (rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sockfd = local_socket_descriptors[sock].socket->multicore_socket_copies[i];
                        light_log(LIGHT_LOG_INFO,"set sock(%d) nonblock\n", sockfd);
                        rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 1);
                    }
                }
                else
                {
                    light_log(LIGHT_LOG_INFO,"set sock(%d) nonblock\n", sock);
                    rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 1);
                }
            }

            else
            {
                if (rte_atomic16_read( &(local_socket_descriptors[sock].socket->is_multicore)))
                {
                    int i;
                    for ( i = 0; i < num_procs; ++i)
                    {
                        int sockfd = local_socket_descriptors[sock].socket->multicore_socket_copies[i];
                        rte_atomic16_set( &(local_socket_descriptors[sockfd].socket->is_nonblock), 0);
                    }
                }
                else
                {
                    rte_atomic16_set( &(local_socket_descriptors[sock].socket->is_nonblock), 0);
                }
            }
            return 0;
        default:
            return -1;
        }
        return -1;
    }
    else
    {
#ifdef API_DEBUG
        printf("  Use real_fcntl, sock = %d, cmd = %d, arg = %d\n", sock, cmd, arg);
#endif
        rc = real_fcntl(sock, cmd, arg);
    }
#ifdef API_DEBUG
    printf("  Return value = %d\n", rc);
#endif
    return rc;
}
#endif


inline void _light_set_buffer_data_len(void *buffer, int len)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)buffer;
    rte_pktmbuf_data_len(mbuf) = len;
}

inline int _light_get_buffer_data_len(void *buffer)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)buffer;
    return rte_pktmbuf_data_len(mbuf);
}

inline void *_light_get_data_ptr(void *desc)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)desc;
    return rte_pktmbuf_mtod(mbuf, void *);
}

inline void _light_update_rfc(void *desc, signed delta)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)desc;
    rte_mbuf_refcnt_update(mbuf, delta);
}

// Todo: shutdown hasn't finished yet
inline int light_shutdown(int sock, int how)
{
    int fd_type;
#ifdef API_DEBUG
    printf("In light_shutdown, (Before fd mapping) sock = %d\n", sock);
#endif

#ifdef TWO_SIDE_FD_SEPARATION
    sock = INT_MAX - sock;
#endif

#ifdef LIGHT_FD_MAPPING
    sock = light_fd_to_socket_epoll_index(&sock, &fd_type);
#endif

#ifdef API_DEBUG
    printf("In light_shutdown, (After fd mapping) sock = %d\n", sock);
#endif
    int proc_id = local_socket_descriptors[sock].proc_id;
    light_cmd_t *cmd;
    cmd = light_get_free_command_buf_by_proc_id(proc_id);
    if (!cmd)
    {
        light_log(LIGHT_LOG_ERR, "can't get free cmd %d\n", __LINE__);
        light_stats_cannot_allocate_cmd++;
        return -1;
    }
    cmd->cmd = LIGHT_SOCKET_SHUTDOWN_COMMAND;
    cmd->ringset_idx = sock;
    cmd->u.socket_shutdown.how = how;
    if (light_enqueue_command_buf_by_proc_id(cmd, proc_id))
    {
        _light_free_command_buf_by_proc_id(cmd, proc_id);
    }
    return 0;
}

inline void *_light_create_mempool(const char *name, int element_size, int element_number)
{
    return rte_mempool_create(name, element_number, element_size, 32 /*cache_size*/, 0 /*private_data_size*/,
                              NULL /*rte_mempool_ctor_t *mp_init*/, NULL /*void *mp_init_arg*/,
                              NULL /*rte_mempool_obj_ctor_t *obj_init*/, NULL /*void *obj_init_arg*/,
                              SOCKET_ID_ANY, 0 /*unsigned flags*/);
}

inline void *_light_mempool_alloc(void *mempool)
{
    void *obj = NULL;
    return (rte_mempool_get(mempool, &obj)) ? NULL : obj;
}

inline void _light_mempool_free(void *mempool, void *obj)
{
    rte_mempool_put(mempool, obj);
}


inline int get_stack_proc_id(int workid, int num_procs){
    //return proc id with least workers_served

    int least_worker_num = MAX_WORKER+1, the_proc_id = 0;
    int cur_proc_id;
    
    //conflict frequency is very low
    rte_rwlock_read_lock( &(workers_info->workers_served_lock) );
    for(cur_proc_id = 0; cur_proc_id < num_procs; cur_proc_id ++){
        if( workers_info->workers_served[cur_proc_id] < least_worker_num ){
            least_worker_num = workers_info->workers_served[cur_proc_id];
            the_proc_id = cur_proc_id;
        }
    }
    rte_rwlock_read_unlock( &(workers_info->workers_served_lock) );

    rte_rwlock_write_lock( &(workers_info->workers_served_lock) );
    workers_info->workers_served[the_proc_id] ++;
    rte_rwlock_write_unlock( &(workers_info->workers_served_lock) );

    return the_proc_id;
}

inline void update_worker_info(){
    //conflict frequency is very low 
    rte_rwlock_write_lock( &(workers_info->register_worker_lock) );
    worker_id = workers_info->register_worker;
    workers_info->register_worker ++;
    rte_rwlock_write_unlock( &(workers_info->register_worker_lock) );

    stack_proc_id = get_stack_proc_id(worker_id, num_procs);
    printf("thread_id = %lu, stack_proc_id = %d\n", pthread_self(), stack_proc_id);
    workers_info->worker_appids[worker_id] = getpid();

    workers_info->worker_procids[worker_id] = stack_proc_id;

    light_log(LIGHT_LOG_INFO,"new worker process, workid:%d, pid:%d, stack proc id:%d\n", \
        worker_id, workers_info->worker_appids[worker_id], stack_proc_id);
}

inline pid_t light_fork(){
    pid_t fpid = real_fork();
    light_log(LIGHT_LOG_INFO,"light_fork: fpid = %d\n", fpid);
    printf("@ Enter light_fork, fpid = %d\n", fpid);
    if(fpid == 0){//child
        update_worker_info();
    }else{//parent
        //do nothing
    }
    return fpid;
}

struct thread_args
{
    int listen_sock;
    void (*before_start)();
};

inline int light_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine) (void *), void *arg)
{
    printf("light hajack pthread_create\n");
    struct thread_args *args = (struct thread_args *)arg;
    printf("light thread_args listen_sock = %d\n", args->listen_sock);
    args->before_start = &update_worker_info;
    return real_pthread_create(thread, attr, start_routine, args);
}