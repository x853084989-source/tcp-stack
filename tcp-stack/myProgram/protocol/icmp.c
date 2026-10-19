#include "icmp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_eal.h>
#include <rte_icmp.h>
#include <rte_memcpy.h>

#include "common/app_state.h"
#include "config.h"
#include "common/net_arch.h"

static uint16_t checksum(uint16_t *addr, int count) {
    register long sum = 0;

    while (count > 1) {
        sum += *(unsigned short *)addr++;
        count -= 2;
    }

    if (count > 0) {
        sum += *(unsigned char *)addr;
    }

    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return ~sum;
}

static int encode_icmp_packet(uint8_t *packet_data, uint8_t *destination_mac, uint32_t source_ip,
                              uint32_t destination_ip, uint16_t id, uint16_t seqnb) {
    // 1 ether
    struct rte_ether_hdr *ethernet_header = (struct rte_ether_hdr *)packet_data;
    rte_memcpy(ethernet_header->s_addr.addr_bytes, local_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(ethernet_header->d_addr.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);
    ethernet_header->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    // 2 ip
    struct rte_ipv4_hdr *ipv4_header =
        (struct rte_ipv4_hdr *)(packet_data + sizeof(struct rte_ether_hdr));
    ipv4_header->version_ihl = 0x45;
    ipv4_header->type_of_service = 0;
    ipv4_header->total_length = htons(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr));
    ipv4_header->packet_id = 0;
    ipv4_header->fragment_offset = 0;
    ipv4_header->time_to_live = 64; // ttl = 64
    ipv4_header->next_proto_id = IPPROTO_ICMP;
    ipv4_header->src_addr = source_ip;
    ipv4_header->dst_addr = destination_ip;

    ipv4_header->hdr_checksum = 0;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);

    // 3 icmp
    struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(packet_data + sizeof(struct rte_ether_hdr) +
                                                        sizeof(struct rte_ipv4_hdr));
    icmp->icmp_type = RTE_IP_ICMP_ECHO_REPLY;
    icmp->icmp_code = 0;
    icmp->icmp_ident = id;
    icmp->icmp_seq_nb = seqnb;

    icmp->icmp_cksum = 0;
    icmp->icmp_cksum = checksum((uint16_t *)icmp, sizeof(struct rte_icmp_hdr));

    return 0;
}

struct rte_mbuf *build_icmp_packet(struct rte_mempool *mbuf_pool, uint8_t *destination_mac,
                                   uint32_t source_ip, uint32_t destination_ip, uint16_t id,
                                   uint16_t seqnb) {
    const unsigned total_length =
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr);

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_alloc\n");
    }

    mbuf->pkt_len = total_length;
    mbuf->data_len = total_length;

    uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    encode_icmp_packet(pkt_data, destination_mac, source_ip, destination_ip, id, seqnb);

    return mbuf;
}

void process_icmp_packet(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                         struct rte_ether_hdr *ethernet_header, struct rte_ipv4_hdr *ipv4_header) {
    int ret;
    struct in_addr addr;

    struct rte_icmp_hdr *icmp_header = rte_pktmbuf_mtod_offset(
        mbuf, struct rte_icmp_hdr *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    addr.s_addr = ipv4_header->src_addr;
    printf(" Received ICMP message from source IP: %s\n", inet_ntoa(addr));

    if (icmp_header->icmp_type == RTE_IP_ICMP_ECHO_REQUEST) {
        struct rte_mbuf *icmp_reply_buf = build_icmp_packet(
            mbuf_pool, ethernet_header->s_addr.addr_bytes, ipv4_header->dst_addr,
            ipv4_header->src_addr, icmp_header->icmp_ident, icmp_header->icmp_seq_nb);

        ret = queue_tx_packet(icmp_reply_buf);
        if (ret == 0) {
            printf(" ICMP reply queued for IP: %s]\n", inet_ntoa(addr));
        } else {
            printf(" Failed to queue ICMP reply]\n");
        }
    }
}
