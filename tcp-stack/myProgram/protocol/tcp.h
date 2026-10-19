#ifndef MYPROGRAM_TCP_H
#define MYPROGRAM_TCP_H

#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct tcp_fragment;

/* Borrows mbuf; process_packets retains ownership. */
void process_tcp_packet(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header);
/* 0: queued to out, 1: waiting for ARP, -1: dropped; fragment stays caller-owned. */
int tcp_output(struct rte_mempool *mbuf_pool, struct tcp_fragment *fragment);

#endif /* MYPROGRAM_TCP_H */
