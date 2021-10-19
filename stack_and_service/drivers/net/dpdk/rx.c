
#include "../../../../light_debug.h"
#include <specific_includes/dummies.h>
#include <specific_includes/linux/types.h>
#include <string.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>

uint64_t received = 0;

extern int proc_id;

int dpdk_dev_get_received(int port_num, struct rte_mbuf **tbl, int tbl_size)
{
    unsigned nb_rx = 0;
    nb_rx = rte_eth_rx_burst((uint8_t)port_num, (uint16_t) proc_id/*0*/, tbl, tbl_size);
    /*    ++cnt;
        used = rte_eth_rx_queue_count(port_num, 0);
        if(used != pre) {
            printf("used mbufs = %d, time = %d\n", used, cnt);
            pre = used;
        }*/
    // received += nb_rx;
    #ifdef RX_COUNT
    // printf("test\n");
    static int nb_rx_times;
    static long nb_rx_total;
    float nb_rx_avg;
    nb_rx_times ++;
    nb_rx_total += nb_rx;
    if (unlikely(nb_rx_times == 1000)) {
    	nb_rx_avg = (float)nb_rx_total / nb_rx_times;
    	printf("tbl_size = %d, nb_rx_avg = %f\n", tbl_size, nb_rx_avg);
    	nb_rx_times = 0;
    	nb_rx_total = 0;
    }
    #endif

    return nb_rx;
}
