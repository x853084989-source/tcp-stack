#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <stdio.h>
#include <arpa/inet.h>

#include "enable.h"
#include "main.h"



#define NUM_TX_QUEUE 1
#define NUM_RX_QUEUE 1
#define DPDK_PORT_ID 0

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {.max_rx_pkt_len = RTE_ETHER_MAX_LEN }
};

int port_init(struct rte_mempool* mbuf_pool){

    uint16_t nb_sys_ports= rte_eth_dev_count_avail(); 

	if (nb_sys_ports == 0) {
		rte_exit(EXIT_FAILURE, "No Supported eth found\n");
	}

	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(DPDK_PORT_ID, &dev_info); 
	
	const int num_rx_queues = NUM_RX_QUEUE; 
	const int num_tx_queues = NUM_TX_QUEUE;

	struct rte_eth_conf port_conf = port_conf_default;
	rte_eth_dev_configure(DPDK_PORT_ID, num_rx_queues, num_tx_queues, &port_conf);

    int ret = rte_eth_rx_queue_setup(DPDK_PORT_ID, 0, 1024, rte_eth_dev_socket_id(DPDK_PORT_ID), NULL, mbuf_pool);

    if (ret < 0) rte_exit(EXIT_FAILURE, "Could not setup RX queue\n");

	
#if ENABLE_SEND

	struct rte_eth_txconf txq_conf = dev_info.default_txconf;
	txq_conf.offloads = port_conf.rxmode.offloads;

    ret = rte_eth_tx_queue_setup(DPDK_PORT_ID, 0 , 1024, rte_eth_dev_socket_id(DPDK_PORT_ID), &txq_conf);
	if (ret < 0) rte_exit(EXIT_FAILURE, "Could not setup TX queue\n");

#endif

    ret = rte_eth_dev_start(DPDK_PORT_ID);
	if (ret < 0 ) rte_exit(EXIT_FAILURE, "Could not start\n");

    return 0;
}