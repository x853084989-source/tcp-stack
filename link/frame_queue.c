#include "frame_queue.h"

#include <stddef.h>


#include "config.h"

static struct frame_queues shared_frame_queues;

struct frame_queues *frame_queues_instance(void) {
    return &shared_frame_queues;
}

int frame_queues_init(void) {
    shared_frame_queues.receive =
        rte_ring_create("myProgram_in", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (shared_frame_queues.receive == NULL) {
        return -1;
    }

    /* Protocol processing and its ARP timer run on the same lcore. */
    shared_frame_queues.send =
        rte_ring_create("myProgram_out", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (shared_frame_queues.send == NULL) {
        rte_ring_free(shared_frame_queues.receive);
        shared_frame_queues.receive = NULL;
        return -1;
    }

    return 0;
}

int enqueue_tx_frame(struct rte_mbuf *frame) {
    if (frame == NULL) {
        return -1;
    }
    if (shared_frame_queues.send == NULL ||
        rte_ring_enqueue(shared_frame_queues.send, frame) != 0) {
        rte_pktmbuf_free(frame);
        return -1;
    }
    return 0;
}
