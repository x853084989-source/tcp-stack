#include "udp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>

#include <rte_cycles.h>
#include <rte_memcpy.h>
#include <rte_udp.h>

#include "common/app_state.h"
#include "protocol/arp.h"
#include "config.h"
#include "common/net_arch.h"
#include "socket/udp_socket.h"

static int encode_udp_packet(uint8_t *packet_data, const uint8_t *payload, uint16_t payload_length,
                             const uint8_t *destination_mac, uint32_t source_ip,
                             uint32_t destination_ip, uint16_t source_port,
                             uint16_t destination_port) {
    struct rte_ether_hdr *ethernet_header = (struct rte_ether_hdr *)packet_data;
    rte_memcpy(ethernet_header->s_addr.addr_bytes, local_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(ethernet_header->d_addr.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);
    ethernet_header->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ipv4_header = (struct rte_ipv4_hdr *)(ethernet_header + 1);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->type_of_service = 0;
    ipv4_header->total_length =
        htons(sizeof(*ipv4_header) + sizeof(struct rte_udp_hdr) + payload_length);
    ipv4_header->packet_id = 0;
    ipv4_header->fragment_offset = 0;
    ipv4_header->time_to_live = 64;
    ipv4_header->next_proto_id = IPPROTO_UDP;
    ipv4_header->src_addr = source_ip;
    ipv4_header->dst_addr = destination_ip;
    ipv4_header->hdr_checksum = 0;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);

    struct rte_udp_hdr *udp_header = (struct rte_udp_hdr *)(ipv4_header + 1);
    udp_header->src_port = source_port;
    udp_header->dst_port = destination_port;
    udp_header->dgram_len = htons(sizeof(*udp_header) + payload_length);
    if (payload_length != 0) {
        rte_memcpy((uint8_t *)(udp_header + 1), payload, payload_length);
    }
    udp_header->dgram_cksum = 0;
    udp_header->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_header, udp_header);
    return 0;
}

static struct rte_mbuf *build_udp_packet(struct rte_mempool *mbuf_pool, const uint8_t *payload,
                                         uint16_t payload_length, const uint8_t *destination_mac,
                                         uint32_t source_ip, uint32_t destination_ip,
                                         uint16_t source_port, uint16_t destination_port) {
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                               sizeof(struct rte_udp_hdr) + payload_length;
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
    encode_udp_packet(packet_data, payload, payload_length, destination_mac, source_ip,
                      destination_ip, source_port, destination_port);
    return mbuf;
}

void udp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header) {
    const uint16_t ip_hdr_len = (ipv4_header->version_ihl & 0x0f) * 4;
    const uint16_t ip_len = ntohs(ipv4_header->total_length);
    const uint16_t frag_off = ntohs(ipv4_header->fragment_offset);

    if (!rte_pktmbuf_is_contiguous(mbuf) || (ipv4_header->version_ihl >> 4) != 4 ||
        ip_hdr_len < sizeof(struct rte_ipv4_hdr) ||
        ip_len < ip_hdr_len + sizeof(struct rte_udp_hdr) ||
        ip_len > rte_pktmbuf_pkt_len(mbuf) - sizeof(struct rte_ether_hdr) ||
        (frag_off & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
        return;
    }

    struct rte_udp_hdr *udp_header = rte_pktmbuf_mtod_offset(
        mbuf, struct rte_udp_hdr *, sizeof(struct rte_ether_hdr) + ip_hdr_len);
    const uint16_t udp_len = ntohs(udp_header->dgram_len);
    if (udp_len < sizeof(struct rte_udp_hdr) || udp_len > ip_len - ip_hdr_len) {
        return;
    }
    const uint16_t payload_len = udp_len - sizeof(struct rte_udp_hdr);
    const uint8_t *payload = (const uint8_t *)(udp_header + 1);

    if (ipv4_header->dst_addr == local_host_ip) {
        udp_socket_input(ipv4_header->src_addr, udp_header->src_port, ipv4_header->dst_addr,
                         udp_header->dst_port, payload, payload_len);
    }
}

int udp_output(struct rte_mempool *mbuf_pool, struct udp_datagram *datagram) {
    const uint64_t now = rte_rdtsc();
    if (now >= datagram->send_deadline_tsc) {
        return -1;
    }
    uint8_t *destination_mac = arp_lookup_mac(datagram->destination_ip);
    if (destination_mac == NULL) {
        if (now >= datagram->next_arp_retry_tsc) {
            static uint8_t broadcast_mac[RTE_ETHER_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            struct rte_mbuf *request =
                build_arp_packet(mbuf_pool, RTE_ARP_OP_REQUEST, broadcast_mac, datagram->source_ip,
                                 datagram->destination_ip);
            enqueue_tx_packet(request);
            datagram->next_arp_retry_tsc = now + rte_get_timer_hz() * ARP_RETRY_SECONDS;
        }
        return 1;
    }

    struct rte_mbuf *mbuf =
        build_udp_packet(mbuf_pool, datagram->payload, datagram->payload_length, destination_mac,
                         datagram->source_ip, datagram->destination_ip, datagram->source_port,
                         datagram->destination_port);
    return enqueue_tx_packet(mbuf);
}
