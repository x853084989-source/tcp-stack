#ifndef MYPROGRAM_NET_ARCH_H
#define MYPROGRAM_NET_ARCH_H

#include <rte_mbuf.h>
#include <rte_ring.h>

struct inout_ring {
    struct rte_ring *in;
    struct rte_ring *out;
};

int net_arch_init(void);
struct inout_ring *ring_instance(void);

/* Consumes mbuf: queues it on success, frees it on failure. */
int queue_tx_packet(struct rte_mbuf *mbuf);

/* Runs on the main lcore; it is the only owner of the NIC RX/TX queues. */
int io_worker(void *arg);

#endif /* MYPROGRAM_NET_ARCH_H */
