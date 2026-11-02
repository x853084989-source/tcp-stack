#include "udp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <rte_cycles.h>
#include <rte_memcpy.h>
#include <rte_udp.h>

#include "config.h"
#include "network_interface.h"
#include "link/arp.h"
#include "link/frame_queue.h"
#include "network/ipv4.h"
#include "transport/udp_socket.h"

static int udp_encode_segment(uint8_t *segment_data, const uint8_t *payload,
                              uint16_t payload_length, uint32_t source_ip,
                              uint32_t destination_ip, uint16_t source_port,
                              uint16_t destination_port) {
    struct rte_udp_hdr *udp_header = (struct rte_udp_hdr *)segment_data;
    udp_header->src_port = source_port;
    udp_header->dst_port = destination_port;
    udp_header->dgram_len = htons(sizeof(*udp_header) + payload_length);
    if (payload_length != 0) {
        rte_memcpy((uint8_t *)(udp_header + 1), payload, payload_length);
    }
    udp_header->dgram_cksum = 0;
    udp_header->dgram_cksum = ipv4_payload_checksum(udp_header,
        sizeof(*udp_header) + payload_length, source_ip, destination_ip, IPPROTO_UDP);
    return 0;
}

void udp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header) {
    const uint16_t ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;
    const uint16_t ipv4_datagram_length = ntohs(ipv4_header->total_length);
    if (!ipv4_validate_transport(mbuf, ipv4_header, IPPROTO_UDP)) {
        return;
    }

    const struct rte_udp_hdr *udp_header =
        (const struct rte_udp_hdr *)ipv4_input_payload(mbuf, ipv4_header);
    const uint16_t udp_segment_length = ntohs(udp_header->dgram_len);
    if (udp_segment_length < sizeof(struct rte_udp_hdr) || udp_segment_length > ipv4_datagram_length - ipv4_header_length) {
        return;
    }
    const uint16_t payload_length = udp_segment_length - sizeof(struct rte_udp_hdr);
    const uint8_t *payload = (const uint8_t *)(udp_header + 1);

    if (ipv4_header->dst_addr == local_host_ip) {
        udp_socket_input(ipv4_header->src_addr, udp_header->src_port, ipv4_header->dst_addr,
                         udp_header->dst_port, payload, payload_length);
    }
}

int udp_output(struct rte_mempool *mbuf_pool, struct udp_segment *segment) {
    const uint64_t now = rte_rdtsc();
    if (now >= segment->send_deadline_tsc) {
        return -1;
    }
    uint8_t *destination_mac = arp_lookup_mac(segment->destination_ip);
    if (destination_mac == NULL) {
        if (now >= segment->next_arp_retry_tsc) {
            static uint8_t broadcast_mac[RTE_ETHER_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            struct rte_mbuf *request =
                build_arp_frame(mbuf_pool, RTE_ARP_OP_REQUEST, broadcast_mac, segment->source_ip,
                                 segment->destination_ip);
            enqueue_tx_frame(request);
            segment->next_arp_retry_tsc = now + rte_get_timer_hz() * ARP_RETRY_SECONDS;
        }
        return 1;
    }

    const unsigned frame_length =
        ipv4_frame_length(sizeof(struct rte_udp_hdr) + segment->payload_length);
    if (frame_length > UINT16_MAX) {
        return -1;
    }
    struct rte_mbuf *frame = rte_pktmbuf_alloc(mbuf_pool);
    if (frame == NULL) {
        return -1;
    }
    if (rte_pktmbuf_append(frame, (uint16_t)frame_length) == NULL) {
        rte_pktmbuf_free(frame);
        return -1;
    }
    udp_encode_segment(ipv4_payload_data(frame), segment->payload, segment->payload_length,
                       segment->source_ip, segment->destination_ip,
                       segment->source_port, segment->destination_port);
    return ipv4_output(frame, destination_mac, segment->source_ip,
                       segment->destination_ip, IPPROTO_UDP,
                       sizeof(struct rte_udp_hdr) + segment->payload_length);
}
