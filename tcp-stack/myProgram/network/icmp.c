#include "icmp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_icmp.h>

#include "network/ipv4.h"

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

static int icmp_encode(uint8_t *icmp_data, uint16_t id, uint16_t seqnb) {
    // 3 icmp
    struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)icmp_data;
    icmp->icmp_type = RTE_IP_ICMP_ECHO_REPLY;
    icmp->icmp_code = 0;
    icmp->icmp_ident = id;
    icmp->icmp_seq_nb = seqnb;

    icmp->icmp_cksum = 0;
    icmp->icmp_cksum = checksum((uint16_t *)icmp, sizeof(struct rte_icmp_hdr));

    return 0;
}

int icmp_output(struct rte_mempool *mbuf_pool, uint8_t *destination_mac,
                uint32_t source_ip, uint32_t destination_ip, uint16_t id, uint16_t seqnb) {
    const unsigned frame_length = ipv4_frame_length(sizeof(struct rte_icmp_hdr));
    struct rte_mbuf *frame = rte_pktmbuf_alloc(mbuf_pool);
    if (!frame) {
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_alloc\n");
    }
    /* Retain the legacy ICMP allocation and failure policy. */
    frame->pkt_len = frame_length;
    frame->data_len = frame_length;
    icmp_encode(ipv4_payload_data(frame), id, seqnb);
    return ipv4_output(frame, destination_mac, source_ip, destination_ip,
                       IPPROTO_ICMP, sizeof(struct rte_icmp_hdr));
}

void icmp_input(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                         struct rte_ether_hdr *ethernet_header, struct rte_ipv4_hdr *ipv4_header) {
    int ret;
    struct in_addr addr;

    struct rte_icmp_hdr *icmp_header = rte_pktmbuf_mtod_offset(
        mbuf, struct rte_icmp_hdr *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    addr.s_addr = ipv4_header->src_addr;
    printf(" Received ICMP message from source IP: %s\n", inet_ntoa(addr));

    if (icmp_header->icmp_type == RTE_IP_ICMP_ECHO_REQUEST) {
        ret = icmp_output(mbuf_pool, ethernet_header->s_addr.addr_bytes,
                          ipv4_header->dst_addr, ipv4_header->src_addr,
                          icmp_header->icmp_ident, icmp_header->icmp_seq_nb);

        if (ret == 0) {
            printf(" ICMP reply queued for IP: %s]\n", inet_ntoa(addr));
        } else {
            printf(" Failed to queue ICMP reply]\n");
        }
    }
}
