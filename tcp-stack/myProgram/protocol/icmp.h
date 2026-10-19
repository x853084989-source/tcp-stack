#ifndef MYPROGRAM_ICMP_H
#define MYPROGRAM_ICMP_H

#include <stdint.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct rte_mbuf *build_icmp_packet(struct rte_mempool *mbuf_pool, uint8_t *destination_mac,
                                   uint32_t source_ip, uint32_t destination_ip, uint16_t id,
                                   uint16_t seqnb);

/* Borrows mbuf and its headers; process_packets() releases the received mbuf. */
void process_icmp_packet(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                         struct rte_ether_hdr *ethernet_header, struct rte_ipv4_hdr *ipv4_header);

#endif /* MYPROGRAM_ICMP_H */
