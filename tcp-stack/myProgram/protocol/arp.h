#ifndef MYPROGRAM_ARP_H
#define MYPROGRAM_ARP_H

#include <stdint.h>

#include <rte_arp.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#define ARP_ENTRY_STATUS_DYNAMIC 0
#define ARP_ENTRY_STATUS_STATIC 1

struct arp_table_item {
    uint32_t ip;
    uint8_t mac_addr[RTE_ETHER_ADDR_LEN];

    uint8_t type;

    struct arp_table_item *next;
    struct arp_table_item *prev;
};

struct arp_table {
    struct arp_table_item *top_table;
    int count;
};

struct arp_table *arp_table_instance(void);

/* The returned MAC address is borrowed from the ARP table. */
uint8_t *arp_lookup_mac(uint32_t destination_ip);

struct rte_mbuf *build_arp_packet(struct rte_mempool *mbuf_pool, uint16_t opcode,
                                  uint8_t *destination_mac, uint32_t source_ip,
                                  uint32_t destination_ip);

/* Borrows the received header; the packet loop releases the received mbuf. */
void process_arp_packet(struct rte_mempool *mbuf_pool, struct rte_arp_hdr *arp_header);
void arp_request_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg);

#endif /* MYPROGRAM_ARP_H */
