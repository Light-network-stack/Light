#ifndef __LIGHT_API_H__
#define __LIGHT_API_H__

#include <sys/time.h>
#include <netinet/in.h>
#include <stdarg.h>
#define LIGHT_MAX_SOCKETS 1000
/*
 *add by Shang Zhantong
 *if someone don't want to enable epoll, just delete the define of EPOLL_SUPPORT
 *the define also exists in light_service_side.h, please delete them together.
 */

#ifndef EPOLL_SUPPORT
#define EPOLL_SUPPORT

#endif

/*
* STRUCTURE:
*    data_and_descriptor
* DESCRIPTION:
*    This structure contains a pointer to buffers data (where the user can write data to send) and buffers descriptors (required when sending the buffers)
* FIELDS:
*    - pdata - a pointer to data
*    - pdesc - buffer's descriptor
*/
struct data_and_descriptor
{
    void *pdata;
    void *pdesc;
};

/*
* STRUCTURE:
*    light_fdset
* DESCRIPTION:
*    This structure contains an array of light socket descriptors, its masks and size
* FIELDS:
*    - sock_idx - an array of socket descriptors indexes and its masks
*    - size
*/
struct light_fdset
{
    uint8_t interested_flags[LIGHT_MAX_SOCKETS];
    uint8_t returned_flags[LIGHT_MAX_SOCKETS];/* needed to not count same event on same socket multiple*/
    int returned_sockets[LIGHT_MAX_SOCKETS];
    int returned_idx;
};

static inline void light_fdzero(struct light_fdset *fdset)
{
    int idx;
    for (idx = 0; idx < LIGHT_MAX_SOCKETS; idx++)
    {
        fdset->interested_flags[idx] = 0;
        fdset->returned_flags[idx] = 0;
        fdset->returned_sockets[idx] = 0;
    }
    fdset->returned_idx = 0;
}
static inline int light_fdisset(int sock, struct light_fdset *fdset)
{
    return (fdset->returned_flags[sock] != 0);
}

static inline int light_fd_idx2sock(struct light_fdset *fdset, int idx)
{
    return fdset->returned_sockets[idx];
}
typedef void (*light_update_cbk_t)(unsigned char command, unsigned char *buffer, int len);

/*
* FUNCTION:
*    int light_app_init(int argc, char **argv,char *app_unique_id)
* DESCRIPTION:
*    This function must be called by an application before any other light
*    API function is called. This will map the data structures used for communicating
*    light service and initialize the data structures used by API library.
* PARAMETERS:
*    - argc - a number of command line parameters passed in argv
*    - argv - an array of pointers to string options, the form is "-c <core mask> -n <number of memory channels> --proc-type secondary". Example: "-c 0xc -n 1 --proc-type secondary". This means
*    the application will run on cores 2 & 3 with one memory channel. Note that --proc-type secondary
*    is mandatory
*    - app_unique_id - string identifying the application (to distinct several applications communicating light service)
* RETURNS:
*    0 if succeeded. Otherwise the process will exit
*/
extern inline int light_app_init(int argc, char **argv, char *app_unique_id, int log_level);

extern inline void fd_mapping_init();

extern inline int is_light_fd(int *fd_num);

extern inline int light_fd_to_socket_epoll_index(int *fd_num, int *type_flag);

extern inline int socket_epoll_index_to_light_fd(int *socket_epoll_index, int type_flag);

extern inline int get_new_light_fd(int *socket_epoll_index, int type_flag);

extern inline int put_back_light_fd(int *light_fd);

extern inline int _light_create_client(light_update_cbk_t update_cbk);

extern inline int _light_read_updates(void);

/*
* FUNCTION:
*    int light_open_socket(int family,int type,int parent)
* DESCRIPTION:
*    This function will open a socket (similar to POSIX socket call)
* PARAMETERS:
*    - family: may be AF_INET, AF_PACKET
*    - type: may be SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
*    - parent: selector's descriptor the socket will be associated with (consult light_open_select)
* RETURNS:
*    If succeeded, >= 0 - opened socket's descriptor. Otherwise negative value is returned
*/

extern inline int light_open_socket(int family, int type, int protocol);


/*
* FUNCTION:
*    int light_bind(int sock,unsigned int ipaddr,unsigned short port,int is_connect)
* DESCRIPTION:
*    This function binds the socket (returned by light_open_socket) to the provided address
* PARAMETERS:
*    - sock - socket's descriptor, returned by light_open_socket
*    - addr - IPv4 address & port
*    - addrlen - length of address, not used
* RETURNS:
*    0 if succeeded, negative if failed
*/
extern inline int light_bind(int sock, struct sockaddr *addr, int addrlen);

/*
* FUNCTION:
*    int light_connect(int sock,unsigned int ipaddr,unsigned short port,int is_connect)
* DESCRIPTION:
*    This function connects the socket (returned by light_open_socket) to the provided address
* PARAMETERS:
*    - sock - socket's descriptor, returned by light_open_socket
*    - addr - IPv4 address
*    - addrlen - length of address, not used
* RETURNS:
*    0 if succeeded, negative if failed
*/
extern inline int light_connect(int sock, struct sockaddr *addr, int addrlen);

/*
* FUNCTION:
*    light_listen_socket(int sock)
* DESCRIPTION:
*    This function will put the socket into the listening state (must be bound before, see above), acts like POSIX listen call
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
* RETURNS:
*    - 0 if succeeded, otherwise negative
*/

extern inline int light_listen_socket(int sock, int backlog);

/*
* FUNCTION:
*    light_close(int sock)
* DESCRIPTION:
*    This function will close the socket (like POSIX close call)
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
* RETURNS:
*    0 if succeeded, oterwise negative
*/
extern inline int light_close(int sock);

/*
* FUNCTION:
*    _light_get_socket_tx_space(int sock)
* DESCRIPTION:
*    This function returns a number of buffers that can be allocated and sent.
*    This is a min of currently available buffers and socket's buffer space
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
* RETURNS:
*    A number of buffers can be allocated and sent on the socket (0 if none)
*/
extern inline int _light_get_socket_tx_space(int sock);

/*
* FUNCTION:
*    _light_send_mbuf(int sock,void *pdesc,int offset,int length)
* DESCRIPTION:
*    This function is used to send data on a connected (TCP or connected UDP) socket
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
*    - pdesc - buffer's descriptor (returned when a buffer is allocated)
*    - offset - offset in the buffer
*    - length - buffer's length
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_send_mbuf(int sock, void *pdesc, int offset, int length);

/*
* FUNCTION:
*    _light_send_bulk(int sock,struct data_and_descriptor *bufs_and_desc,int *offsets,int *lengths,int buffer_count)
* DESCRIPTION:
*    This function is used to send data on a connected (TCP or connected UDP) socket in bulk
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
*    - bufs_and_desc - an array of struct data_and_descriptor (size of array is buffer_count)
*    - offsets - an array of offsets (size of array is buffer_count)
*    - lengths - an array of lengths (size of array is buffer_count)
*    - buffer_count - size of bufs_and_desc, offsets and lengths arrays
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_send_bulk(int sock, struct data_and_descriptor *bufs_and_desc, int *offsets, int *lengths, int buffer_count);

/*
* FUNCTION:
*    light_send(int sockfd, const void *buf, size_t nbytes, int flags)
* DESCRIPTION:
*    This function is used to send data on a connected (TCP or connected UDP) socket
* PARAMETERS:
*   - sockfd - socket's descriptor (returned by light_open_socket)
*   - buf - a pointer pointed to an empty buffer which is used to store data that is ready to send
*   - nbytes - IN/OUT, in case nbytes is 0, specify the length of data that is ready to send.
*   Note that there can be less data then requested to send.
*   - flags - control how to send data
* RETURNS:
*    the length of data that is send succeeded, otherwise negative
*/
extern inline ssize_t light_send(int sockfd, const void *buf, size_t nbytes, int flags);

/*
* FUNCTION:
*    light_write(int sockfd, const void *buf, size_t nbytes)
* DESCRIPTION:
*    This function is used to send data on a connected (TCP or connected UDP) socket
* PARAMETERS:
*   - sockfd - socket's descriptor (returned by light_open_socket)
*   - buf - a pointer pointed to an empty buffer which is used to store data that is ready to send
*   - nbytes - IN/OUT, in case nbytes is 0, specify the length of data that is ready to send.
*   Note that there can be less data then requested to send.
* RETURNS:
*    the length of data that is send succeeded, otherwise negative
*/
extern inline ssize_t light_write(int sockfd, const void *buf, size_t nbytes);

/*
* FUNCTION:
*    light_writev(int sockfd, const struct iovec *iov, int iovcnt)
* DESCRIPTION:
*    This function is used to send data on a connected (TCP or connected UDP) socket from a buffer vector
* PARAMETERS:
*   - sockfd - socket's descriptor (returned by light_open_socket)
*   - iov - a pointer pointed to an array of iovec structures, defined in <sys/uio.h> as:
*               struct iovec {
*                   void *iov_base;
*                   size_t iov_len;
*               }
*   - iovcnt - specify the number of the iovec blocks.
* RETURNS:
*    the length of data that is send succeeded, otherwise negative
*/
extern inline ssize_t light_writev(int sockfd, const struct iovec* iov, int iovcnt);

/*
* FUNCTION:
*    int _light_sendto_mbuf(int sock,void *pdesc,int offset,int length,unsigned int ipaddr,unsigned short port)
* DESCRIPTION:
*    This function is used to send data on UDP/RAW socket to a specified destination
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
*    - pdesc - buffer's descriptor (returned when a buffer is allocated)
*    - offset - offset in the buffer
*    - length - length of the buffer
*    - ipaddr - IPv4 address
*    - port - port
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_sendto_mbuf(int sock, void *pdesc, int offset, int length, struct sockaddr *addr, int addrlen);

/*
* FUNCTION:
*    _light_sendto_bulk(int sock,struct data_and_descriptor *bufs_and_desc,int *offsets,int *lengths,unsigned int *ipaddrs,unsigned short *ports, int buffer_count)
* DESCRIPTION:
*    This function is used to send data on a UDP/RAW socket in bulk to specified destinations
* PARAMETERS:
*    - sock - socket's descriptor (returned by light_open_socket)
*    - bufs_and_desc - an array of struct data_and_descriptor (size of array is buffer_count)
*    - offsets - an array of offsets (size of array is buffer_count)
*    - lengths - an array of lengths (size of array is buffer_count)
*    - ipaddrs - an array of IPv4 addresses (size of array is buffer_count)
*    - ports - an array of ports (size of array is buffer_count)
*    - buffer_count - size of bufs_and_desc, offsets and lengths arrays
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_sendto_bulk(int sock, struct data_and_descriptor *bufs_and_desc, int *offsets, int *lengths, struct sockaddr *addr, int addrlen, int buffer_count);

/*
* FUNCTION:
*    light_sendto(int sockfd, const void *buf, size_t nbytes, int flags,
*    const struct sockaddr *destaddr, socklen_t destlen)
* DESCRIPTION:
*    This function is used to send data on UDP/RAW socket to a specified destination
* PARAMETERS:
*   - sockfd - socket's descriptor (returned by light_open_socket)
*   - buf - a pointer pointed to an empty buffer which is used to store data that is ready to send
*   - nbytes - IN/OUT, in case nbytes is 0, specify the length of data that is ready to send.
*   Note that there can be less data then requested to send.
*   - flags - control how to send data.
*   - destaddr A null pointer, or points to a sockaddr structure in which the destination address is to be stored.
*    The length and format of the address depend on the address family of the socket.
*   - destlen specifies the length of the sockaddr structure pointed to by the destination address argument
* RETURNS:
*    the length of data that is send succeeded, otherwise negative
*/
extern inline ssize_t light_sendto(int sockfd, const void *buf, size_t nbytes, int flags,
                                   const struct sockaddr *destaddr, socklen_t destlen);

/*
* FUNCTION:
*    _light_socket_kick(int sock)
* DESCRIPTION:
*    Once one of light_send* functions is called, to ensure light service is notified, this function is called.
*    The mechanism is similar to vhost/virtio kicks
* PARAMETERS:
*    - sock - a socket's descriptor whose rings the light service is supposed to read.
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_socket_kick(int sock);

/*
* FUNCTION:
*    void *_light_get_next_buffer_segment(void **pdesc,int *len)
* DESCRIPTION:
*    This function returns a pointer to the data of the next (chained) buffer's segment, its descriptor and its length.
*    This function is supposed to be called for the buffers returned by light_receive and light_receivefrom
* PARAMETERS:
*    - pdesc (IN and OUT) - an address of buffer's descriptor (returned by light_receive*). In case the return value is not NULL,
*    an address of the next buffer's descriptor is written here
*    - len (OUT) - an address where the returned buffer length is written
* RETURNS:
*    If there is a next segment in the chained buffer, an address of the data in that buffer is returned. In this case,
*    the buffer descriptor is returned in pdesc and the buffer length is returned in len. If there is no more segments,
*    returns NULL, in this case pdesc and len are untouched
*/
extern inline void *_light_get_next_buffer_segment(void **pdesc, int *len);

/*
* FUNCTION:
*    void *_light_get_next_buffer_segment_and_detach_first(void **pdesc,int *len)
* DESCRIPTION:
*    This function returns a pointer to the data of the next (chained) buffer's segment, its descriptor and its length.
*    The buffer to which *pdesc pointed when this function is called is detached (i.e. does not point to next anymore)
*    This function is supposed to be called for the buffers returned by light_receive and light_receivefrom
* PARAMETERS:
*    - pdesc (IN and OUT) - an address of buffer's descriptor (returned by light_receive*). In case the return value is not NULL,
*    an address of the next buffer's descriptor is written here
*    - len (OUT) - an address where the returned buffer length is written
* RETURNS:
*    If there is a next segment in the chained buffer, an address of the data in that buffer is returned. In this case,
*    the buffer descriptor is returned in pdesc and the buffer length is returned in len. If there is no more segments,
*    returns NULL, in this case pdesc and len are untouched
*/
extern inline void *_light_get_next_buffer_segment_and_detach_first(void **pdesc, int *len);

/*
* FUNCTION:
*    void _light_release_rx_buffer(void *pdesc,int sock)
* DESCRIPTION:
*    This function is called to free the buffer's chain returned in light_receive*
* PARAMETERS:
*    - pdesc - a buffer descriptor, returned by light_receive*
*    - sock - a socket on which light_receive* was called
* RETURNS:
*    None
*/
extern inline void _light_release_rx_buffer(void *pdesc, int sock);

/*
* FUNCTION:
*    _light_receive_buffer_chain(int sock,void **buffer,int *len,int *first_segment_len,void **pdesc)
* DESCRIPTION:
*    This function is called to receive a data (buffer's chain) on a TCP socket
* PARAMETERS:
*    - sock - socket's descriptor
*    - buffer - an address of the pointer which (in case of success) will point to the data in the first
*    segment of the buffer chain
*    - len - IN/OUT, in case *len is 0, all the pending data is read from the socket. Otherwise, only specified amount
*    is read. Note that there can be less data then requested to read. In this case, *len will contain an exact number of bytes retrieved (if any)
*    - first_segment_len - an address where the length of the first segment is written. Following segments are retrieved by calling
*    light_get_next_buffer_segment, which also returns buffer descriptor and buffer length for the segment.
*    - pdesc - an address of the buffer descriptor for the first buffer in the chain (and whole chain, save it before calling light_get_next_buffer_segment)
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_receive_buffer_chain(int sock, void **buffer, int *len, int *first_segment_len, void **pdesc, int is_nonblock);

/*
* FUNCTION:
*    light_recv(int sock, void *buf, size_t nbytes, int flags)
* DESCRIPTION:
*    This function is called to receive data flow on a TCP socket
* PARAMETERS:
*    - sock - socket's descriptor
*    - buf - a pointer pointed to an empty buffer which is used to store received data (in case of success)
*    - nbytes - IN/OUT, in case nbytes is 0, all the pending data is read from the socket. Otherwise, only specified amount
*    is read. Note that there can be less data then requested to read.
*    - flags - control how to receive data
* RETURNS:
*    the number of bytes received if succeeded,
*    0 if the socket peer has performed an orderly shutdown or the socket receives zero-length datagrams
*    or the requested number of bytes to receive from the socket is 0
*    -1 if an error occurred. In the event of an error, errno is set to indicate the error
*/
extern inline ssize_t light_recv(int sock, void *buf, size_t nbytes, int flags);

/*
* FUNCTION:
*    light_read(int sock, void *buf, size_t nbytes)
* DESCRIPTION:
*    This function is called to receive data flow on a TCP socket
* PARAMETERS:
*    - sock - socket's descriptor
*    - buf - a pointer pointed to an empty buffer which is used to store received data (in case of success)
*    - nbytes - IN/OUT, in case nbytes is 0, all the pending data is read from the socket. Otherwise, only specified amount
*    is read. Note that there can be less data then requested to read.
* RETURNS:
*    the number of bytes received if succeeded,
*    0 if the socket peer has performed an orderly shutdown or the socket receives zero-length datagrams
*    or the requested number of bytes to receive from the socket is 0
*    -1 if an error occurred. In the event of an error, errno is set to indicate the error
*/
extern inline ssize_t light_read(int sock, void *buf, size_t nbytes);

/*
* FUNCTION:
*    light_readv(int sockfd, const struct iovec *iov, int iovcnt)
* DESCRIPTION:
*    This function is used to receive data on a connected (TCP or connected UDP) socket and store them to
*     a buffer vector
* PARAMETERS:
*   - sockfd - socket's descriptor (returned by light_open_socket)
*   - iov - a pointer pointed to an array of iovec structures, defined in <sys/uio.h> as:
*               struct iovec {
*                   void *iov_base;
*                   size_t iov_len;
*               }
*   - iovcnt - specify the number of the iovec blocks.
* RETURNS:
*    the length of data that is send succeeded, otherwise negative
*/
extern inline ssize_t light_readv(int sockfd, const struct iovec* iov, int iovcnt);

/*
* FUNCTION:
*    light_receivefrom(int sock,void **buffer,int *len,unsigned int *ipaddr, unsigned short *port, int *first_segment_len,void **pdesc)
* DESCRIPTION:
*    This function is called to receive a data (buffer's chain) on a TCP socket
* PARAMETERS:
*    - sock - socket's descriptor
*    - buffer - an address of the pointer which (in case of success) will point to the data in the first
*    segment of the buffer chain
*    - len - IN/OUT, in case *len is 0, all the pending data is read from the socket. Otherwise, only specified amount
*    is read. Note that there can be less data then requested to read. In this case, *len will contain an exact number of bytes retrieved (if any)
*    - first_segment_len - an address where the length of the first segment is written. Following segments are retrieved by calling
*    light_get_next_buffer_segment, which also returns buffer descriptor and buffer length for the segment.
*     - pdesc - an address of the buffer descriptor for the first buffer in the chain (and whole chain, save it before calling light_get_next_buffer_segment)
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int _light_receivefrom_mbuf(int sock, void **buffer, int *len, struct sockaddr *addr, int *addrlen, void **pdesc);

/*
* FUNCTION:
*    light_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
* DESCRIPTION:
*    This function is called to receive a data on a UDP/RAW socket
* PARAMETERS:
*    - sock - socket's descriptor
*    - buf - a pointer pointed to an empty buffer which is used to store received data (in case of success)
*    - len - specifies the length in bytes of the buffer pointed to by the buffer argument.
*    - flags - specifies the type of message reception. Values of this argument are formed by logically OR'ing zero
*    or more values.
*    - addr A null pointer, or points to a sockaddr structure in which the sending address is to be stored.
*    The length and format of the address depend on the address family of the socket.
*    - addrlen specifies the length of the sockaddr structure pointed to by the address argument
* RETURNS:
*    the number of bytes received if succeeded,
*    0 if the socket peer has performed an orderly shutdown or the socket receives zero-length datagrams
*    or the requested number of bytes to receive from the socket is 0
*    -1 if an error occurred. In the event of an error, errno is set to indicate the error
*/
extern inline ssize_t light_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen);

/*
* FUNCTION:
*    _light_release_tx_buffer(void *pdesc)
* DESCRIPTION:
*    This function is called for a buffer that was allocated using _light_get_buffer/_light_get_buffers_bulk.
* PARAMETERS:
*    - pdesc - buffer's descriptor
* RETURNS:
*    None
*/
extern inline void _light_release_tx_buffer(void *pdesc);

/*
* FUNCTION:
*    light_accept(int sock,unsigned int *ipaddr,unsigned short *port)
* DESCRIPTION:
*    This function accepts a new socket connected to listening
* PARAMETERS:
*    - sock - socket's descriptor of the listening socket
*    - addr - a pointer to accepted connection's IPv4 address & port (sockaddr_in)
*    - addrlen - a pointer to address's length (now unused)
* RETURNS:
*    An accepted socket's descriptor, if succeeded. Otherwise -1
*/
extern inline int light_accept(int sock, struct sockaddr *addr, socklen_t *addrlen);


extern inline int light_accept4(int sock, struct sockaddr *addr, socklen_t *addrlen, int flags);

/*
* FUNCTION:
*    int _light_is_connected(int sock)
* DESCRIPTION:
*    Tests if a socket was connected
* PARAMETERS:
*     - sock - socket's descriptor
* RETURNS:
*    - 1 if connected, 0 if not
*/
extern inline int _light_is_connected(int sock);

/*
* FUNCTION:
*    void light_getsockname(int sock, int is_local,unsigned int *ipaddr,unsigned short *port)
* DESCRIPTION:
*    Gets either local socket IP address/port or remote peer's
* PARAMETERS:
*    - sock - socket's descriptor
*    - is_local - if 1, gets local address, otherwise gets remote peer's
*    - ipaddr - a pointer to IPv4 address
*    - port - a pointer to port
* RETURNS:
*    None
*/
extern inline void light_getsockname(int sock, int is_local, struct sockaddr *addr, int *addrlen);

/*
* FUNCTION:
    int light_setsockopt(int sock, int level, int optname,char *optval, unsigned int optlen)
* DESCRIPTION:
*    This function sets socket's options (similar to POSIX's one)
* PARAMETERS:
*    - sock - socket's descriptor
*    - level
*    - optname
*    - optval
*    - optlen - Consult Linux's setsockopt
* RETURNS:
*    0 if succeeded, otherwise negative
*/
extern inline int light_setsockopt(int sock, int level, int optname, const void *optval, socklen_t optlen);

/*
 * FUNCTION:
 *  int light_ioctl(int sockfd, unsigned long request, void *argp)
 * DESCRIPTION:
 *  This function manipulates the parameters of the socket
 * PARAMETERS:
 *   - sock - socket's descriptor
 *   - request request code
 *   - argp argument
 * RETURNS:
 *   0 if succeeded, otherwise negative
 */
extern inline int light_ioctl(int sockfd, unsigned long request, void *argp);
// extern int light_ioctl(int sockfd, unsigned long request, va_list args);

/* function:
    void _light_set_buffer_data_len(void *buffer, int len);
* description:
*   this function set a buffer's data length
* parameters:
*   - buffer - a buffer descriptor
*   - len - buffer's data length
* returns:
*   none
*/
inline void _light_set_buffer_data_len(void *buffer, int len);

/* function:
    int _light_get_buffer_data_len(void *buffer);
* description:
*   this function set a buffer's data length
* parameters:
*   - buffer - a buffer descriptor
* returns:
*   length of the buffer
*/

inline int _light_get_buffer_data_len(void *buffer); //for nginx, not be called

/*
 * add by gjk
 * function:
 *  int light_register_hook(char*soPath,char* init_funcsym,char*exec_reg_funcsym,int hook_pid,long hook_register_time);
 * description:
 *  这个函数负责注册自定义hook
 * parameters:
 *  soPath：so文件的路径
 *  init_funcsym:要取出的该so文件中的自定义的函数名
 *  exec_reg_funcsym 要取出的该so文件中的自定义的函数名
 *  必须取函数名，因为dlsym只能取出so文件中的函数，不能取出像变量，结构体之类的东西
 *  分两步，init函数负责传入主程序的注册函数指针和注销函数指针，这个init函数的签名类似这样
 *  init_handler(HANDLE_T reg_op,HANDLE_T unreg_op)
 *  其中HANDLE_T为定义的函数指针
 *  typedef int (*HANDLE_T)(struct nf_hook_ops *reg);
 *  这些都是写在so文件中的
 *  hook_pid:发起该Hook注册操作的应用进程的PID，
 *  hook_register_time 进程发起Hook注册时的时刻，微秒级别
 *  上述两个参数可以唯一标识一个Hook，从而便于在哈希表中存储Hook对应的sohandler，从而可以
 *  顺利调用注销接口查找到该句柄，关闭so文件，进而注销该Hook
 * returns:
 *  成功放入cmd队列，返回0，否则返回其他
 */
extern int light_register_hook(char*soPath, char* init_funcsym, char*exec_reg_funcsym, uint32_t hook_pid, uint64_t hook_register_time);

/*
 * add by gjk
 * function:
 *  int light_unregister_hook(void* sohdl);
 * description:
 *  这个函数负责注册自定义hook
 * parameters:
 *  sohdl：打开so文件后获得的handler
 * returns:
 *  成功放入cmd队列，返回0，否则返回其他
 */
// extern int light_unregister_hook(uint32_t hook_pid, uint64_t hook_register_time);

extern inline int _light_socket_init(int sock, int proc_id);

extern inline int light_epoll_create(int size);
extern inline int light_epoll_ctl(int epfd, int op, int fd, void *events);
extern inline int light_epoll_wait(int epfd, void *events, int maxevents, int timeout);

extern inline int _light_epoll_ctl(int epfd, int op, int fd, void *events);

extern inline void *_light_get_data_ptr(void *desc);

extern inline void _light_update_rfc(void *desc, signed delta);

extern inline int light_shutdown(int sock, int how);

extern inline void *_light_create_mempool(const char *name, int element_size, int element_number);

extern inline void *_light_mempool_alloc(void *mempool);

extern inline void _light_mempool_free(void *mempool, void *obj);

//add by hyk
extern inline int light_fcntl(int s, int cmd, long arg);

//add by ld
extern inline int get_stack_proc_id(int workid, int num_procs);

extern inline void update_worker_info();

extern inline pid_t light_fork();

extern inline int light_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine) (void *), void *arg);

#endif
