#ifndef MYPROGRAM_MAIN_H
#define MYPROGRAM_MAIN_H

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <stdio.h>
#include <arpa/inet.h>

#define DPDK_PORT_ID 0

#define NUM_MBUFS (4096-1)
#define BURST_SIZE 32
#define MAKE_IPV4_HOST_ADDR(a, b, c, d) ((a<<24) + (b<<16) + (c<<8) + (d))


extern uint32_t local_host_ip;
extern uint32_t src_ip;
extern uint32_t dst_ip;

extern uint8_t src_mac[RTE_ETHER_ADDR_LEN];
extern uint8_t dst_mac[RTE_ETHER_ADDR_LEN];

extern uint16_t src_port;
extern uint16_t dst_port;

int port_init(struct rte_mempool *mbuf_pool);

struct rte_mbuf *send_udp(struct rte_mempool *mbuf_pool, uint8_t *data, uint16_t length);
struct rte_mbuf *send_icmp(struct rte_mempool *mbuf_pool, uint8_t *dst_mac,
                           uint32_t sip, uint32_t dip, uint16_t id, uint16_t seqnb);

#endif
