#include "kni.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <rte_ethdev.h>
#include <rte_kni.h>

#include "common/net_arch.h"
#include "config.h"

static struct rte_kni *kernel_interface;
/* Read and changed only by the I/O lcore after initialization. */
static int port_running = 1;

static int configure_network_interface(uint16_t port_id, uint8_t interface_up) {
    if (port_id != DPDK_PORT_ID || !rte_eth_dev_is_valid_port(port_id)) {
        return -EINVAL;
    }
    rte_eth_dev_stop(port_id);
    port_running = 0;
    if (interface_up) {
        int result = rte_eth_dev_start(port_id);
        if (result < 0) {
            return result;
        }
        port_running = 1;
    }
    return 0;
}

int kni_init(struct rte_mempool *mbuf_pool) {
    if (rte_kni_init(1) < 0) {
        return -1;
    }
    struct rte_kni_conf configuration;
    memset(&configuration, 0, sizeof(configuration));
    snprintf(configuration.name, sizeof(configuration.name), "vEth%u", DPDK_PORT_ID);
    configuration.group_id = DPDK_PORT_ID;
    configuration.mbuf_size = rte_pktmbuf_data_room_size(mbuf_pool) - RTE_PKTMBUF_HEADROOM;
    rte_eth_macaddr_get(DPDK_PORT_ID, (struct rte_ether_addr *)configuration.mac_addr);
    if (rte_eth_dev_get_mtu(DPDK_PORT_ID, &configuration.mtu) < 0) {
        return -1;
    }
    struct rte_kni_ops operations;
    memset(&operations, 0, sizeof(operations));
    operations.port_id = DPDK_PORT_ID;
    operations.config_network_if = configure_network_interface;
    kernel_interface = rte_kni_alloc(mbuf_pool, &configuration, &operations);
    if (kernel_interface == NULL) {
        return -1;
    }
    printf("KNI interface %s created; configure its local IPv4 address before traffic.\n",
           configuration.name);
    return 0;
}

int kni_send(struct rte_mbuf *mbuf) {
    if (mbuf == NULL) {
        return -1;
    }
    if (kernel_interface == NULL || rte_kni_tx_burst(kernel_interface, &mbuf, 1) != 1) {
        rte_pktmbuf_free(mbuf);
        return -1;
    }
    return 0;
}

void kni_receive(void) {
    if (kernel_interface == NULL) {
        return;
    }
    struct rte_mbuf *mbufs[BURST_SIZE];
    unsigned packet_count = rte_kni_rx_burst(kernel_interface, mbufs, BURST_SIZE);
    for (unsigned i = 0; i < packet_count; i++) {
        enqueue_tx_packet(mbufs[i]);
    }
}

void kni_handle_requests(void) {
    if (kernel_interface != NULL) {
        rte_kni_handle_request(kernel_interface);
    }
}

int kni_port_is_running(void) {
    return port_running;
}
