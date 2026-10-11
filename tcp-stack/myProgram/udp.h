#ifndef MYPROGRAM_UDP_H
#define MYPROGRAM_UDP_H

#include <stdint.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct rte_mbuf *send_udp(struct rte_mempool *mbuf_pool, uint8_t *data, uint16_t length);

/* Borrows mbuf and its headers; process_packets() releases the received mbuf. */
void process_udp_packet(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                        struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr);

#endif /* MYPROGRAM_UDP_H */
