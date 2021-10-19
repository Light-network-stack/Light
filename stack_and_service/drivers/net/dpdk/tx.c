#include <string.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>

uint64_t transmitted = 0;
uint64_t tx_dropped = 0;

extern int proc_id;

extern int *g_monitor_info;

void dpdk_dev_enqueue_for_tx(int port_num, struct rte_mbuf *m)
{
//    printf("eth_tx_burst mbuf = %p\n", m);
	g_monitor_info[8]++;
	unsigned ret = rte_eth_tx_burst(port_num, (uint16_t) /*queue_id*/proc_id, &m, (uint16_t) 1);
	transmitted += ret;
	if (unlikely(ret < 1))
	{
        printf("tx_dropped.\n");
		tx_dropped ++;
		rte_pktmbuf_free(m);
	}
}
