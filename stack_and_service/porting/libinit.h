#ifndef __LIBINIT_H_
#define __LIBINIT_H_
/* This function returns a pointer to kernel's interface structure, required to access the driver
 * Parameter:ethernet port number
 */
void *get_dpdk_dev_by_port_num(int port_num);
/* This function allocates rte_mbuf */
void *get_buffer();
/* this function gets a pointer to data in the newly allocated rte_mbuf */
void *get_data_ptr(void *buf);
/* this function releases the rte_mbuf */
void release_buffer(void *buf);
/*
 * This function must be called prior any other in this package.
 * It initializes all the DPDK libs, reads the configuration, initializes the stack's
 * subsystems, allocates mbuf pools etc.
 * Parameters: refer to DPDK EAL parameters.
 * For example -c <core mask> -n <memory channels> -- -p <port mask>
 */
int dpdk_linux_tcpip_init(int argc,char **argv);
/* this function returns an available mbufs count */
int get_buffer_count();
/*
 * This function may be called to calculate driver's optimal polling interval .
 * Paramters: a pointer to socket structure
 * Returns: None
 *
 */
int get_max_drv_poll_interval_in_micros(int port_num);

struct mempool* get_direct_pool(uint16_t);

#endif /* __LIBINIT_H_ */
