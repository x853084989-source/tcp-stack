#include "ipv4.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <rte_byteorder.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "network_interface.h"
#include "link/arp.h"
#include "link/ethernet.h"
#include "dpdk/kni.h"
#include "transport/tcp.h"
#include "transport/udp.h"

void ipv4_input(struct rte_mbuf *frame, const struct rte_ether_hdr *ethernet_header) {
    if (rte_pktmbuf_data_len(frame) <
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
        rte_pktmbuf_free(frame);
        return;
    }
    struct rte_ipv4_hdr *ipv4_header = (struct rte_ipv4_hdr *)ethernet_payload_data(frame);
    unsigned ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;
    unsigned ipv4_datagram_length = rte_be_to_cpu_16(ipv4_header->total_length);
    if ((ipv4_header->version_ihl >> 4) != 4 ||
        ipv4_header_length < sizeof(*ipv4_header) ||
        ipv4_datagram_length < ipv4_header_length ||
        ipv4_datagram_length > rte_pktmbuf_data_len(frame) - sizeof(struct rte_ether_hdr)) {
        rte_pktmbuf_free(frame);
        return;
    }
    arp_learn(ipv4_header->src_addr, ethernet_header->s_addr.addr_bytes);
    struct in_addr addr;
    addr.s_addr = ipv4_header->src_addr;
    printf("[Received IPv4 message from source IP: %s\n", inet_ntoa(addr));
    if (ipv4_header->next_proto_id == IPPROTO_UDP) {
        udp_input(frame, ipv4_header);
        rte_pktmbuf_free(frame);
        return;
    }
    if (ipv4_header->next_proto_id == IPPROTO_TCP) {
        tcp_input(frame, ipv4_header);
        rte_pktmbuf_free(frame);
        return;
    }
    kni_send(frame);
}

int ipv4_validate_transport(const struct rte_mbuf *frame,
                            const struct rte_ipv4_hdr *ipv4_header, uint8_t protocol) {
    const uint16_t ipv4_datagram_length = ntohs(ipv4_header->total_length);
    const uint16_t fragment_offset = ntohs(ipv4_header->fragment_offset);
    if (protocol == IPPROTO_TCP) {
        if (!rte_pktmbuf_is_contiguous(frame) || ipv4_header->version_ihl != 0x45 ||
            ipv4_header->dst_addr != local_host_ip ||
            ipv4_datagram_length < sizeof(*ipv4_header) + sizeof(struct rte_tcp_hdr) ||
            ipv4_datagram_length + sizeof(struct rte_ether_hdr) > rte_pktmbuf_pkt_len(frame) ||
            (fragment_offset & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
            return 0;
        }
    } else {
        const uint16_t ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;
        if (!rte_pktmbuf_is_contiguous(frame) || (ipv4_header->version_ihl >> 4) != 4 ||
            ipv4_header_length < sizeof(struct rte_ipv4_hdr) ||
            ipv4_datagram_length < ipv4_header_length + sizeof(struct rte_udp_hdr) ||
            ipv4_datagram_length > rte_pktmbuf_pkt_len(frame) - sizeof(struct rte_ether_hdr) ||
            (fragment_offset & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
            return 0;
        }
    }
    return 1;
}

unsigned ipv4_frame_length(unsigned payload_length) {
    return ethernet_frame_length(sizeof(struct rte_ipv4_hdr) + payload_length);
}

uint8_t *ipv4_payload_data(struct rte_mbuf *frame) {
    return ethernet_payload_data(frame) + sizeof(struct rte_ipv4_hdr);
}

uint8_t *ipv4_input_payload(struct rte_mbuf *frame,
                                  const struct rte_ipv4_hdr *ipv4_header) {
    const unsigned ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;
    return rte_pktmbuf_mtod_offset(frame, uint8_t *,
                                   sizeof(struct rte_ether_hdr) + ipv4_header_length);
}

uint16_t ipv4_payload_checksum(const void *payload, uint16_t payload_length,
                               uint32_t source_ip, uint32_t destination_ip, uint8_t protocol) {
    struct rte_ipv4_hdr pseudo_header = {0};
    pseudo_header.total_length = htons(sizeof(pseudo_header) + payload_length);
    pseudo_header.next_proto_id = protocol;
    pseudo_header.src_addr = source_ip;
    pseudo_header.dst_addr = destination_ip;
    return rte_ipv4_udptcp_cksum(&pseudo_header, payload);
}

int ipv4_output(struct rte_mbuf *frame, const uint8_t *destination_mac,
                uint32_t source_ip, uint32_t destination_ip, uint8_t protocol,
                uint16_t payload_length) {
    struct rte_ipv4_hdr *ipv4_header = (struct rte_ipv4_hdr *)ethernet_payload_data(frame);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->type_of_service = 0;
    ipv4_header->total_length = htons(sizeof(*ipv4_header) + payload_length);
    ipv4_header->packet_id = 0;
    ipv4_header->fragment_offset = 0;
    ipv4_header->time_to_live = 64;
    ipv4_header->next_proto_id = protocol;
    ipv4_header->src_addr = source_ip;
    ipv4_header->dst_addr = destination_ip;
    ipv4_header->hdr_checksum = 0;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);
    return ethernet_output(frame, destination_mac, RTE_ETHER_TYPE_IPV4);
}
