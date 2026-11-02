#ifndef MYPROGRAM_TCP_H
#define MYPROGRAM_TCP_H

#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct tcp_segment;

/* Borrows mbuf; ethernet_input retains ownership. */
void tcp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header);
/* 0: queued to the send frame queue, 1: waiting for ARP, -1: dropped; caller owns segment. */
int tcp_output(struct rte_mempool *mbuf_pool, struct tcp_segment *segment);

#endif /* MYPROGRAM_TCP_H */
