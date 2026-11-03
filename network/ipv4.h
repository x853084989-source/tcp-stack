#ifndef MYPROGRAM_IPV4_H
#define MYPROGRAM_IPV4_H

#include <stdint.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

/* Consumes the frame; keeps full Ethernet bytes intact on the KNI branch. */
void ipv4_input(struct rte_mbuf *frame, const struct rte_ether_hdr *ethernet_header);
/* Retains the existing TCP/UDP-specific acceptance rules. */
int ipv4_validate_transport(const struct rte_mbuf *frame,
                            const struct rte_ipv4_hdr *ipv4_header, uint8_t protocol);
unsigned ipv4_frame_length(unsigned payload_length);
uint8_t *ipv4_payload_data(struct rte_mbuf *frame);
/* Borrow the input payload after the validated, possibly variable-length IP header. */
uint8_t *ipv4_input_payload(struct rte_mbuf *frame,
                                  const struct rte_ipv4_hdr *ipv4_header);
/* Pseudo-header checksum over a transport payload; does not write an IP header. */
uint16_t ipv4_payload_checksum(const void *payload, uint16_t payload_length,
                               uint32_t source_ip, uint32_t destination_ip, uint8_t protocol);
/* Consumes an allocated frame whose transport payload is already encoded. */
int ipv4_output(struct rte_mbuf *frame, const uint8_t *destination_mac,
                uint32_t source_ip, uint32_t destination_ip, uint8_t protocol,
                uint16_t payload_length);

#endif /* MYPROGRAM_IPV4_H */
