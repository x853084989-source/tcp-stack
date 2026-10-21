#include "net_arch.h"

#include <stddef.h>

#include <rte_ethdev.h>

#include "config.h"

static struct inout_ring packet_rings;

struct inout_ring *packet_rings_instance(void) {
    return &packet_rings;
}

int net_arch_init(void) {
    packet_rings.in =
        rte_ring_create("myProgram_in", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (packet_rings.in == NULL) {
        return -1;
    }

    /* Protocol processing and its ARP timer run on the same lcore. */
    packet_rings.out =
        rte_ring_create("myProgram_out", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (packet_rings.out == NULL) {
        rte_ring_free(packet_rings.in);
        packet_rings.in = NULL;
        return -1;
    }

    return 0;
}

int enqueue_tx_packet(struct rte_mbuf *mbuf) {
    if (mbuf == NULL) {
        return -1;
    }
    if (packet_rings.out == NULL || rte_ring_enqueue(packet_rings.out, mbuf) != 0) {
        rte_pktmbuf_free(mbuf);
        return -1;
    }
    return 0;
}

int io_worker(__attribute__((unused)) void *arg) {
    while (1) {
        struct rte_mbuf *rx[BURST_SIZE];
        unsigned num_recvd = rte_eth_rx_burst(DPDK_PORT_ID, 0, rx, BURST_SIZE);
        unsigned queued = rte_ring_enqueue_burst(packet_rings.in, (void **)rx, num_recvd, NULL);
        for (unsigned i = queued; i < num_recvd; i++) {
            rte_pktmbuf_free(rx[i]);
        }

        struct rte_mbuf *tx[BURST_SIZE];
        unsigned count = rte_ring_dequeue_burst(packet_rings.out, (void **)tx, BURST_SIZE, NULL);
        if (count != 0) {
            unsigned sent = rte_eth_tx_burst(DPDK_PORT_ID, 0, tx, count);
            /* The NIC owns [0, sent); only unsent mbufs remain ours. */
            for (unsigned i = sent; i < count; i++) {
                rte_pktmbuf_free(tx[i]);
            }
        }
    }

    return 0;
}
