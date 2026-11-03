#include "io_worker.h"

#include <stddef.h>
#include <rte_ethdev.h>

#include "config.h"
#include "link/frame_queue.h"
#include "monitor/ddos.h"
#include "dpdk/kni.h"

int io_worker(__attribute__((unused)) void *arg) {
    struct frame_queues *frame_queues = frame_queues_instance();
    while (1) {
        kni_handle_requests();
        if (!kni_port_is_running()) {
            continue;
        }
        struct rte_mbuf *receive_frames[BURST_SIZE];
        unsigned received_count = rte_eth_rx_burst(DPDK_PORT_ID, 0, receive_frames, BURST_SIZE);
        for (unsigned i = 0; i < received_count; i++) {
            ddos_detect(receive_frames[i]);
        }
        unsigned enqueued_count = rte_ring_enqueue_burst(
            frame_queues->receive, (void **)receive_frames, received_count, NULL);
        for (unsigned i = enqueued_count; i < received_count; i++) {
            rte_pktmbuf_free(receive_frames[i]);
        }

        struct rte_mbuf *send_frames[BURST_SIZE];
        unsigned dequeued_count =
            rte_ring_dequeue_burst(frame_queues->send, (void **)send_frames, BURST_SIZE, NULL);
        if (dequeued_count != 0) {
            unsigned sent_count = rte_eth_tx_burst(DPDK_PORT_ID, 0, send_frames, dequeued_count);
            /* The NIC owns [0, sent_count); only unsent mbufs remain ours. */
            for (unsigned i = sent_count; i < dequeued_count; i++) {
                rte_pktmbuf_free(send_frames[i]);
            }
        }
    }

    return 0;
}
