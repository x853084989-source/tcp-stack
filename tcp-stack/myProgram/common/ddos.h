#ifndef MYPROGRAM_DDOS_H
#define MYPROGRAM_DDOS_H

#include <stdint.h>
#include <rte_mbuf.h>

#define DDOS_WINDOW_SIZE 256
#define DDOS_THRESHOLD 1200.0

struct ddos_sample {
    uint64_t set_bits;
    uint64_t total_bits;
    double entropy;
};

struct ddos_detector {
    struct ddos_sample samples[DDOS_WINDOW_SIZE];
    unsigned next_sample;
    unsigned sample_count;
};

/* Borrows all packet segments; returns 1 above threshold, 0 below/not full, -1 invalid. */
int ddos_sample_packet(struct ddos_detector *detector, const struct rte_mbuf *mbuf,
                       double *score);
/* RX I/O owner: retains the reference's threshold-triggered process exit. */
void ddos_detect(struct rte_mbuf *mbuf);

#endif /* MYPROGRAM_DDOS_H */
