#ifndef MYPROGRAM_UDP_H
#define MYPROGRAM_UDP_H

#include <stdint.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct udp_segment;

/* Borrows mbuf and its headers; ethernet_input() releases the received mbuf. */
void udp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header);

/* 0: output queued, 1: retry after ARP, -1: dropped. Caller owns segment. */
int udp_output(struct rte_mempool *mbuf_pool, struct udp_segment *segment);

#endif /* MYPROGRAM_UDP_H */
