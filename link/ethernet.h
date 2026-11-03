#ifndef MYPROGRAM_ETHERNET_H
#define MYPROGRAM_ETHERNET_H

#include <stdint.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

/* Consumes frame, either by freeing it or transferring it to KNI. */
void ethernet_input(struct rte_mbuf *frame);
unsigned ethernet_frame_length(unsigned payload_length);
/* Borrowed views; frame data offset is never advanced. */
uint8_t *ethernet_payload_data(struct rte_mbuf *frame);
/* Consumes an allocated frame with its payload already encoded. */
int ethernet_output(struct rte_mbuf *frame, const uint8_t *destination_mac,
                    uint16_t ether_type);

#endif /* MYPROGRAM_ETHERNET_H */
