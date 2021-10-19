#ifndef __API_H_
#define __API_H_

#ifndef CCJ_API
	#define CCJ_API
#endif

void *app_glue_create_socket(int family,int type);

int app_glue_v4_bind(struct socket *sock,unsigned int ipaddr, unsigned short port);

int app_glue_v4_connect(struct socket *sock,unsigned int ipaddr,unsigned short port);

#ifndef CCJ_API
int app_glue_v4_listen(struct socket *sock);
#else
int app_glue_v4_listen(struct socket *sock,int server_backlog);
#endif

/*
 * This function must be called by application to initialize.
 * the rate of polling for driver, timer, readable & writable socket lists
 * Paramters: drv_poll_interval,timer_poll_interval,tx_ready_sockets_poll_interval,
 * rx_ready_sockets_poll_interval - all in micros
 * Returns: None
 *
 */
extern void app_glue_init_poll_intervals(int drv_poll_interval,
		int timer_poll_interval,
        int tx_ready_sockets_poll_interval,
        int rx_ready_sockets_poll_interval,int crash_detect_interval);

/*
 * This function must be called by application periodically.
 * This is the heart of the system, it performs all the driver/IP stack work
 * and timers
 * Paramters: call_flush_queues - if non-zero, the readable, closable and writable queues
 * are processed and user's functions are called.
 * Alternatively, call_flush_queues can be 0 and the application may call
 * app_glue_get_next* functions to get readable, acceptable, closable and writable sockets
 * ports_to_poll - an array of port numbers to poll
 * ports_to_poll_count - asize of array of ports to poll
 * Returns: None
 *
 */
inline void app_glue_periodic(int call_flush_queues,uint8_t *ports_to_poll,int ports_to_poll_count);

/*
 * This function may be called to attach user's data to the socket.
 * Paramters: a pointer  to socket (returned, for example, by create_*_socket)
 * a pointer to data to be attached to the socket
 * Returns: None
 *
 */
extern void app_glue_set_user_data(void *socket,void *data);

/*
 * This function may be called to get attached to the socket user's data .
 * Paramters: a pointer  to socket (returned, for example, by create_*_socket,)
 * Returns: pointer to data to be attached to the socket
 *
 */
extern void *app_glue_get_user_data(void *socket);

/*
 * This function may be called to get next closable socket .
 * Paramters: None
 * Returns: pointer to socket to be closed
 *
 */
extern void *app_glue_get_next_closed();

/*
 * This function may be called to get next writable socket .
 * Paramters: None
 * Returns: pointer to socket to be written
 *
 */
extern void *app_glue_get_next_writer();

/*
 * This function may be called to get next readable socket .
 * Paramters: None
 * Returns: pointer to socket to be read
 *
 */
extern void *app_glue_get_next_reader();

/*
 * This function may be called to get next acceptable socket .
 * Paramters: None
 * Returns: pointer to socket on which to accept a new connection
 *
 */
extern void *app_glue_get_next_listener();

/*
 * This function may be called to close socket .
 * Paramters: a pointer to socket structure
 * Returns: None
 *
 */
extern void app_glue_close_socket(void *socket);

/*
 * This function may be called to estimate amount of data can be sent .
 * Paramters: a pointer to socket structure
 * Returns: number of bytes the application can send
 *
 */
extern int app_glue_calc_size_of_data_to_send(void *sock);

/*
 * This function must be called prior any other in this package.
 * It initializes all the DPDK libs, reads the configuration, initializes the stack's
 * subsystems, allocates mbuf pools etc.
 * Parameters: refer to DPDK EAL parameters.
 * For example -c <core mask> -n <memory channels> -- -p <port mask>
 */
extern int dpdk_linux_tcpip_init(int argc,char **argv);

/*
 * This function may be called to allocate rte_mbuf from existing pool.
 * Paramters: None
 * Returns: a pointer to rte_mbuf, if succeeded, NULL if failed
 *
 */
extern struct rte_mbuf *app_glue_get_buffer();

#endif /* __API_H_ */
