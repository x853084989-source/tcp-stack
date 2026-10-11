#include "udp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>

#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_udp.h>

#include "app_state.h"
#include "config.h"

static int encode_udp_pkt(uint8_t *msg, unsigned char *data, uint16_t total_len) {
    // encode

    // 1 ethhdr
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth->s_addr.addr_bytes, src_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    // 2 iphdr
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
    ip->version_ihl = 0x45;
    ip->type_of_service = 0;
    ip->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 64; // ttl = 64
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = src_ip;
    ip->dst_addr = dst_ip;

    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    // 3 udphdr

    struct rte_udp_hdr *udp =
        (struct rte_udp_hdr *)(msg + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    udp->src_port = src_port;
    udp->dst_port = dst_port;
    uint16_t udplen = total_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr);
    udp->dgram_len = htons(udplen);

    const uint16_t payload_len = udplen - sizeof(struct rte_udp_hdr);
    rte_memcpy((uint8_t *)(udp + 1), data, payload_len);

    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);

    return 0;
}

struct rte_mbuf *send_udp(struct rte_mempool *mbuf_pool, uint8_t *data, uint16_t length) {
    // mempool --> mbuf

    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                               sizeof(struct rte_udp_hdr) + length;
    if (total_len > UINT16_MAX) {
        return NULL;
    }

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
        return NULL;
    }
    uint8_t *pktdata = (uint8_t *)rte_pktmbuf_append(mbuf, (uint16_t)total_len);
    if (pktdata == NULL) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }

    encode_udp_pkt(pktdata, data, (uint16_t)total_len);

    return mbuf;
}

void process_udp_packet(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                        struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr) {
    int ret;
    struct in_addr addr;
    addr.s_addr = iphdr->src_addr;

    const uint16_t ip_hdr_len = (iphdr->version_ihl & 0x0f) * 4;
    const uint16_t ip_len = ntohs(iphdr->total_length);
    const uint16_t frag_off = ntohs(iphdr->fragment_offset);

    /* This echo path requires a complete, contiguous UDP datagram. */
    if (!rte_pktmbuf_is_contiguous(mbuf) || (iphdr->version_ihl >> 4) != 4 ||
        ip_hdr_len < sizeof(struct rte_ipv4_hdr) ||
        ip_len < ip_hdr_len + sizeof(struct rte_udp_hdr) ||
        ip_len > rte_pktmbuf_pkt_len(mbuf) - sizeof(struct rte_ether_hdr) ||
        (frag_off & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
        return;
    }

    struct rte_udp_hdr *udphdr = rte_pktmbuf_mtod_offset(mbuf, struct rte_udp_hdr *,
                                                         sizeof(struct rte_ether_hdr) + ip_hdr_len);
    const uint16_t udp_len = ntohs(udphdr->dgram_len);
    if (udp_len < sizeof(struct rte_udp_hdr) || udp_len > ip_len - ip_hdr_len) {
        return;
    }
    const uint16_t payload_len = udp_len - sizeof(struct rte_udp_hdr);
    uint8_t *payload = (uint8_t *)(udphdr + 1);

    rte_memcpy(&dst_mac, ehdr->s_addr.addr_bytes, RTE_ETHER_ADDR_LEN);

    rte_memcpy(&src_ip, &iphdr->dst_addr, sizeof(uint32_t));
    rte_memcpy(&dst_ip, &iphdr->src_addr, sizeof(uint32_t));

    rte_memcpy(&src_port, &udphdr->dst_port, sizeof(uint16_t));
    rte_memcpy(&dst_port, &udphdr->src_port, sizeof(uint16_t));

    struct rte_mbuf *udp_reply_buf = send_udp(mbuf_pool, payload, payload_len);
    if (udp_reply_buf == NULL) {
        printf(" Failed to allocate UDP reply buffer]\n");

        return;
    }
    ret = rte_eth_tx_burst(DPDK_PORT_ID, 0, &udp_reply_buf, 1);
    if (ret != 0) {
        printf(" UDP reply sent to IP: %s]\n", inet_ntoa(addr));
    } else {
        rte_pktmbuf_free(udp_reply_buf);
        printf(" Failed to send UDP reply]\n");
    }
}
