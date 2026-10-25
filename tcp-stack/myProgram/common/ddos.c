#include "ddos.h"

#include <math.h>
#include <stdlib.h>

#include <rte_eal.h>

/* Reference formula, including its length term; this is not a validated classifier. */
static double calculate_entropy(uint64_t set_bits, uint64_t total_bits) {
    if (total_bits == 0) {
        return 0.0;
    }
    double set = (double)set_bits;
    double total = (double)total_bits;
    double unset = total - set;
    double entropy = log2(total);
    if (set_bits != 0) {
        entropy -= set * (log2(set) - log2(total));
    }
    if (set_bits != total_bits) {
        entropy -= unset * (log2(unset) - log2(total));
    }
    return entropy;
}

int ddos_sample_packet(struct ddos_detector *detector, const struct rte_mbuf *mbuf,
                       double *score) {
    if (detector == NULL || mbuf == NULL || score == NULL) {
        return -1;
    }
    *score = 0.0;
    uint64_t set_bits = 0;
    uint64_t total_bits = 0;
    uint32_t remaining_length = rte_pktmbuf_pkt_len(mbuf);
    const struct rte_mbuf *segment = mbuf;
    unsigned segment_count = 0;
    while (segment != NULL && segment_count < mbuf->nb_segs) {
        uint32_t data_length = rte_pktmbuf_data_len(segment);
        if (segment->data_off > segment->buf_len ||
            data_length > (uint32_t)segment->buf_len - segment->data_off ||
            data_length > remaining_length) {
            return -1;
        }
        const uint8_t *data = rte_pktmbuf_mtod(segment, const uint8_t *);
        for (uint32_t i = 0; i < data_length; i++) {
            set_bits += (unsigned)__builtin_popcount((unsigned)data[i]);
        }
        total_bits += (uint64_t)data_length * 8;
        remaining_length -= data_length;
        segment = segment->next;
        segment_count++;
    }
    if (remaining_length != 0 || segment != NULL || segment_count != mbuf->nb_segs) {
        return -1;
    }
    struct ddos_sample *sample = &detector->samples[detector->next_sample];
    sample->set_bits = set_bits;
    sample->total_bits = total_bits;
    sample->entropy = calculate_entropy(set_bits, total_bits);
    detector->next_sample = (detector->next_sample + 1) % DDOS_WINDOW_SIZE;
    /* Match the reference: first evaluate on the packet after the window fills. */
    if (detector->sample_count < DDOS_WINDOW_SIZE) {
        detector->sample_count++;
        return 0;
    }
    uint64_t window_set_bits = 0;
    uint64_t window_total_bits = 0;
    double entropy_sum = 0.0;
    for (unsigned i = 0; i < DDOS_WINDOW_SIZE; i++) {
        window_set_bits += detector->samples[i].set_bits;
        window_total_bits += detector->samples[i].total_bits;
        entropy_sum += detector->samples[i].entropy;
    }
    *score = entropy_sum - calculate_entropy(window_set_bits, window_total_bits);
    if (*score > DDOS_THRESHOLD) {
        return 1;
    }
    return 0;
}

void ddos_detect(struct rte_mbuf *mbuf) {
    static struct ddos_detector detector;
    double score;
    if (ddos_sample_packet(&detector, mbuf, &score) == 1) {
        rte_exit(EXIT_FAILURE, "Reference DDoS threshold exceeded: score=%.3f > %.3f\n",
                 score, DDOS_THRESHOLD);
    }
}
