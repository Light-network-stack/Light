#ifndef _LIGHT_DEBUG_H
#define _LIGHT_DEBUG_H
//#define RTE_MBUF_DEBUG
//#define LIGHT_SERVICE_LOOP_DEBUG
// #define DEV_DEBUG
//#define APP_GLUE_DEBUG
//#define SOCKET_DEBUG
//#define ETH_DEBUG
//#define IP_INPUT_DEBUG
//#define RAW_DEBUG
//#define INET_HASHTABLES_DEBUG
//#define TCP_IPV4_DEBUG
//#define _LINUX_SKBUFF_H
//#define SKBUFF_DEBUG
//#define IP_OUTPUT_DEBUG
// #define ARP_DEBUG
//#define DEBUG
//#define INET_HASHTABLES_DEBUG
// #define NEIGHBOUR_DEBUG
//#define LIGHT_SERVER_SIDE_DEBUG

//#define SOCK_DEBUG
//#define TCP_INPUT_DEBUG
//#define TCP_OUTPUT_DEBUG
//#define LIGHT_API_DEBUG
//#define LIGHT_API_PROFILING
// #define API_DEBUG
// #define LIGHT_LOG
// #define CLOSE_DEBUG
// #define COMMAND_DEBUG

// #define COMMAND_TIME
// #define LOOP_TIME
// #define APP_GLUE_PERIODIC_TIME

/************ command processing configuration ************/
// #define PROCESS_ALL_COMMAND
#define PROCESS_N_COMMAND
// #define PROCESS_ONE_COMMAND
/**********************************************************/

/************ epoll mode configuration ************/
// #define ADAPT_EPOLL
// #define BUSY_EPOLL
#define SLEEP_EPOLL
/**************************************************/

/************ buffer manage configuration ************/
// #define BUFFER_MANAGE
#define GRT_CONN_NUM 20 // the number of bandwidth-gurantee connections
#define EXP_MBUF_POOL_SIZE 3401 // the size of mbuf pool of all connections, for experiment use.
/*****************************************************/

// #define LIGHT_RTE_RING_DEBUG
// #define EPOLL_DEBUG
// #define EPOLL_DEBUG_COUNT
// #define BUFFER_MANAGE_DEBUG
// #define EPOLL_APP_RPS_DEBUG
// #define RX_DEBUG // print the ratio of idle NIC poll number
// #define BACKEND_PERF // show the performance of backend (number of connections per second).
// #define RX_COUNT // add counter for average number of received packets
// #define CMD_COUNT // add counter for average number of proceeded commands

#endif // #ifndef _LIGHT_DEBUG_H