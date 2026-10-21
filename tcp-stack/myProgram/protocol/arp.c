#include "arp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>

#include "common/app_state.h"
#include "config.h"
#include "common/net_arch.h"

#define LL_ADD(item, list)                                                                         \
    do {                                                                                           \
        item->prev = NULL;                                                                         \
        item->next = list;                                                                         \
        if (list != NULL)                                                                          \
            list->prev = item;                                                                     \
        list = item;                                                                               \
    } while (0)

static uint8_t broadcast_arp_mac[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static struct arp_table *arp_table = NULL;
static int arp_request_count = 1;

struct arp_table *arp_table_instance(void) {
    if (arp_table == NULL) {
        arp_table = rte_malloc("arp table", sizeof(struct arp_table), 0);

        if (arp_table == NULL) {
            rte_exit(EXIT_FAILURE, "rte_malloc arp table failed\n");
        }

        memset(arp_table, 0, sizeof(struct arp_table));
    }

    return arp_table;
}

uint8_t *arp_lookup_mac(uint32_t destination_ip) {
    struct arp_table_item *iter;
    struct arp_table *table = arp_table_instance();

    for (iter = table->top_table; iter != NULL; iter = iter->next) {
        if (destination_ip == iter->ip) {
            return iter->mac_addr;
        }
    }

    return NULL;
}

static int encode_arp_packet(uint8_t *packet_data, uint16_t opcode, uint8_t *destination_mac,
                             uint32_t source_ip, uint32_t destination_ip) {
    // 1 ethhdr
    struct rte_ether_hdr *ethernet_header = (struct rte_ether_hdr *)packet_data;
    rte_memcpy(ethernet_header->s_addr.addr_bytes, local_mac, RTE_ETHER_ADDR_LEN);

    if (!strncmp((const char *)destination_mac, (const char *)broadcast_arp_mac,
                 RTE_ETHER_ADDR_LEN)) {
        uint8_t mac[RTE_ETHER_ADDR_LEN];
        rte_memcpy(mac, broadcast_arp_mac, RTE_ETHER_ADDR_LEN);
        rte_memcpy(ethernet_header->d_addr.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
    } else {
        rte_memcpy(ethernet_header->d_addr.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);
    }
    ethernet_header->ether_type = htons(RTE_ETHER_TYPE_ARP);

    // 2 arp
    struct rte_arp_hdr *arp_header = (struct rte_arp_hdr *)(ethernet_header + 1);
    arp_header->arp_hardware = htons(1);
    arp_header->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);
    arp_header->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp_header->arp_plen = sizeof(uint32_t);
    arp_header->arp_opcode = htons(opcode);

    rte_memcpy(arp_header->arp_data.arp_sha.addr_bytes, local_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(arp_header->arp_data.arp_tha.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);

    arp_header->arp_data.arp_sip = source_ip;
    arp_header->arp_data.arp_tip = destination_ip;

    return 0;
}

struct rte_mbuf *build_arp_packet(struct rte_mempool *mbuf_pool, uint16_t opcode,
                                  uint8_t *destination_mac, uint32_t source_ip,
                                  uint32_t destination_ip) {
    const unsigned total_length = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_alloc\n");
    }

    mbuf->pkt_len = total_length;
    mbuf->data_len = total_length;

    uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    encode_arp_packet(pkt_data, opcode, destination_mac, source_ip, destination_ip);

    return mbuf;
}

void arp_request_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg) {
    struct rte_mempool *mbuf_pool = (struct rte_mempool *)arg;
    /* Build /24 targets in host byte order, then convert for ARP and lookup. */
    const uint32_t subnet = ntohl(local_host_ip) & 0xFFFFFF00U;

    int i = 0;
    printf("\nNotice:arp table is been searching: %d\n", arp_request_count++);
    for (i = 1; i <= 254; i++) {
        uint32_t destination_ip = htonl(subnet | (uint32_t)i);

        struct in_addr addr;
        addr.s_addr = destination_ip;

        struct rte_mbuf *arpbuf = NULL;
        uint8_t *destination_mac = arp_lookup_mac(destination_ip);
        if (destination_mac == NULL) {
            arpbuf =
                build_arp_packet(mbuf_pool, RTE_ARP_OP_REQUEST, broadcast_arp_mac, local_host_ip,
                                 destination_ip); // ARP table dont have dstmac, send arp request
        } else {
            printf("target ip and its mac address is searched in arp table successfully: %s \n",
                   inet_ntoa(addr));
            arpbuf = build_arp_packet(mbuf_pool, RTE_ARP_OP_REQUEST, destination_mac, local_host_ip,
                                      destination_ip); // ARP table have dstmac, send arp request
        }

        enqueue_tx_packet(arpbuf);
    }
}

void process_arp_packet(struct rte_mempool *mbuf_pool, struct rte_arp_hdr *arp_header) {
    int ret;
    if (arp_header->arp_data.arp_tip == local_host_ip) {
        struct in_addr addr;
        addr.s_addr = arp_header->arp_data.arp_tip;
        printf("[Received ARP message to local IP: %s\n", inet_ntoa(addr));

        if (arp_header->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)) {
            addr.s_addr = arp_header->arp_data.arp_sip;
            printf(" Received ARP request from IP: %s\n", inet_ntoa(addr));
            // ARP reply
            // check table

            struct rte_mbuf *arp_reply_buf = build_arp_packet(
                mbuf_pool, RTE_ARP_OP_REPLY, arp_header->arp_data.arp_sha.addr_bytes,
                arp_header->arp_data.arp_tip, arp_header->arp_data.arp_sip);

            ret = enqueue_tx_packet(arp_reply_buf);

            if (ret == 0) {
                printf(" ARP reply queued for IP: %s ] \n", inet_ntoa(addr));
            } else {
                printf(" Failed to queue ARP reply ]\n");
            }
        } else if (arp_header->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY)) {
            addr.s_addr = arp_header->arp_data.arp_sip;
            printf(" Received ARP reply: ");
            printf(" ARP reply from IP: %s]\n", inet_ntoa(addr));
            // save in ARP table
            struct arp_table *table = arp_table_instance();
            uint8_t *mac_addr = arp_lookup_mac(arp_header->arp_data.arp_sip);

            if (mac_addr == NULL) {
                struct arp_table_item *iter =
                    rte_malloc("arp_table_item", sizeof(struct arp_table_item), 0);

                if (iter == NULL) {
                    rte_exit(EXIT_FAILURE, "rte_malloc arp_table_item failed\n");
                }

                iter->ip = arp_header->arp_data.arp_sip;
                rte_memcpy(iter->mac_addr, arp_header->arp_data.arp_sha.addr_bytes,
                           RTE_ETHER_ADDR_LEN);
                LL_ADD(iter, table->top_table);

                // check if the iter is added to the table
                for (iter = table->top_table; iter != NULL; iter = iter->next) {
                    struct in_addr addr;
                    addr.s_addr = iter->ip;

                    if (iter->ip == arp_header->arp_data.arp_sip) {
                        char mac_str[32];
                        rte_ether_format_addr(mac_str, sizeof(mac_str),
                                              (const struct rte_ether_addr *)iter->mac_addr);
                        printf("\nNotice: ARP table have been added, IP:%s, MAC: %s\n",
                               inet_ntoa(addr), mac_str);
                        break;
                    }
                }
            }
        }
    }
}
