#ifndef MYPROGRAM_FRAME_QUEUE_H
#define MYPROGRAM_FRAME_QUEUE_H

#include <rte_mbuf.h>
#include <rte_ring.h>

struct frame_queues {
    struct rte_ring *receive;
    struct rte_ring *send;
};

int frame_queues_init(void);
struct frame_queues *frame_queues_instance(void);

/* Consumes frame: queues it on success, frees it on failure. */
int enqueue_tx_frame(struct rte_mbuf *frame);

#endif /* MYPROGRAM_FRAME_QUEUE_H */
