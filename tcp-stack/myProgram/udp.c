#include "udp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>

#include <rte_cycles.h>
#include <rte_memcpy.h>
#include <rte_udp.h>

#include "app_state.h"
#include "arp.h"
#include "config.h"
#include "net_arch.h"
#include "udp_socket.h"

static int encode_udp_pkt(uint8_t *msg, const uint8_t *data, uint16_t length,
                          const uint8_t *destination_mac, uint32_t sip, uint32_t dip,
                          uint16_t sport, uint16_t dport) {
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth->s_addr.addr_bytes, src_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth->d_addr.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);
    eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    ip->version_ihl = 0x45;
    ip->type_of_service = 0;
    ip->total_length = htons(sizeof(*ip) + sizeof(struct rte_udp_hdr) + length);
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = sip;
    ip->dst_addr = dip;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    udp->src_port = sport;
    udp->dst_port = dport;
    udp->dgram_len = htons(sizeof(*udp) + length);
    if (length != 0) {
        rte_memcpy((uint8_t *)(udp + 1), data, length);
    }
    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
    return 0;
}

static struct rte_mbuf *build_udp_packet(struct rte_mempool *mbuf_pool, const uint8_t *data,
                                         uint16_t length, const uint8_t *destination_mac,
                                         uint32_t sip, uint32_t dip, uint16_t sport,
                                         uint16_t dport) {
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                               sizeof(struct rte_udp_hdr) + length;
    if (total_len > UINT16_MAX) {
        return NULL;
    }
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (mbuf == NULL) {
        return NULL;
    }
    uint8_t *packet_data = (uint8_t *)rte_pktmbuf_append(mbuf, (uint16_t)total_len);
    if (packet_data == NULL) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }
    encode_udp_pkt(packet_data, data, length, destination_mac, sip, dip, sport, dport);
    return mbuf;
}

/* The 06-only configuration keeps the original direct echo path. */
struct rte_mbuf *send_udp(struct rte_mempool *mbuf_pool, const uint8_t *data, uint16_t length) {
    return build_udp_packet(mbuf_pool, data, length, dst_mac, src_ip, dst_ip, src_port, dst_port);
}

void process_udp_packet(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                        struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr) {
    const uint16_t ip_hdr_len = (iphdr->version_ihl & 0x0f) * 4;
    const uint16_t ip_len = ntohs(iphdr->total_length);
    const uint16_t frag_off = ntohs(iphdr->fragment_offset);

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
    const uint8_t *payload = (const uint8_t *)(udphdr + 1);

#if ENABLE_UDP_APP
    (void)mbuf_pool;
    (void)ehdr;
    if (iphdr->dst_addr == local_host_ip) {
        udp_socket_deliver(iphdr->src_addr, udphdr->src_port, iphdr->dst_addr, udphdr->dst_port,
                           payload, payload_len);
    }
#else
    rte_memcpy(dst_mac, ehdr->s_addr.addr_bytes, RTE_ETHER_ADDR_LEN);
    src_ip = iphdr->dst_addr;
    dst_ip = iphdr->src_addr;
    src_port = udphdr->dst_port;
    dst_port = udphdr->src_port;
    struct rte_mbuf *reply = send_udp(mbuf_pool, payload, payload_len);
    if (queue_tx_packet(reply) != 0) {
        printf("Failed to queue UDP reply\n");
    }
#endif
}

int udp_output(struct rte_mempool *mbuf_pool, struct udp_datagram *datagram) {
    const uint64_t now = rte_rdtsc();
    if (now >= datagram->expire_tsc) {
        return -1;
    }
    uint8_t *destination_mac = get_dst_macaddr(datagram->dip);
    if (destination_mac == NULL) {
        if (now >= datagram->next_arp_tsc) {
            static uint8_t broadcast_mac[RTE_ETHER_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            struct rte_mbuf *request = send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, broadcast_mac,
                                                datagram->sip, datagram->dip);
            queue_tx_packet(request);
            datagram->next_arp_tsc = now + rte_get_timer_hz() * ARP_RETRY_SECONDS;
        }
        return 1;
    }

    struct rte_mbuf *mbuf =
        build_udp_packet(mbuf_pool, datagram->data, datagram->length, destination_mac,
                         datagram->sip, datagram->dip, datagram->sport, datagram->dport);
    return queue_tx_packet(mbuf);
}
