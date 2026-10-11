#ifndef MYPROGRAM_ICMP_H
#define MYPROGRAM_ICMP_H

#include <stdint.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct rte_mbuf *send_icmp(struct rte_mempool *mbuf_pool, uint8_t *dst_mac, uint32_t sip,
                           uint32_t dip, uint16_t id, uint16_t seqnb);

/* Borrows mbuf and its headers; process_packets() releases the received mbuf. */
void process_icmp_packet(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                         struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr);

#endif /* MYPROGRAM_ICMP_H */
