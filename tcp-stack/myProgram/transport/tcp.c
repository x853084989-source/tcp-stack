#include "tcp.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <rte_cycles.h>
#include <rte_memcpy.h>
#include <rte_tcp.h>

#include "config.h"
#include "link/arp.h"
#include "link/frame_queue.h"
#include "network/ipv4.h"
#include "transport/tcp_socket.h"

static void tcp_encode_segment(uint8_t *segment_data,
                                const struct tcp_segment *segment) {
    struct rte_tcp_hdr *tcp_header = (struct rte_tcp_hdr *)segment_data;
    tcp_header->src_port = segment->source_port;
    tcp_header->dst_port = segment->destination_port;
    tcp_header->sent_seq = htonl(segment->seq_num);
    tcp_header->recv_ack = htonl(segment->ack_num);
    tcp_header->data_off = 0x50;
    tcp_header->tcp_flags = segment->tcp_flags;
    tcp_header->rx_win = htons(TCP_INITIAL_WINDOW);
    tcp_header->tcp_urp = 0;
    if (segment->payload_length != 0) {
        rte_memcpy((uint8_t *)(tcp_header + 1), segment->payload, segment->payload_length);
    }
    tcp_header->cksum = 0;
    tcp_header->cksum = ipv4_payload_checksum(tcp_header,
        sizeof(*tcp_header) + segment->payload_length, segment->source_ip,
        segment->destination_ip, IPPROTO_TCP);
}

void tcp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header) {
    const uint16_t ipv4_datagram_length = ntohs(ipv4_header->total_length);
    if (!ipv4_validate_transport(mbuf, ipv4_header, IPPROTO_TCP)) {
        return;
    }

    struct rte_tcp_hdr *tcp_header = (struct rte_tcp_hdr *)ipv4_input_payload(mbuf, ipv4_header);
    const uint16_t tcp_segment_length = ipv4_datagram_length - sizeof(*ipv4_header);
    const uint16_t tcp_header_length = (tcp_header->data_off >> 4) * 4;
    if (tcp_header_length < sizeof(*tcp_header) || tcp_header_length > tcp_segment_length) {
        return;
    }
    const uint16_t received_checksum = tcp_header->cksum;
    tcp_header->cksum = 0;
    const uint16_t checksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    tcp_header->cksum = received_checksum;
    if (checksum != received_checksum) {
        return;
    }
    const uint8_t *payload = (const uint8_t *)tcp_header + tcp_header_length;
    tcp_socket_input(ipv4_header, tcp_header, payload, tcp_segment_length - tcp_header_length);
}

int tcp_output(struct rte_mempool *mbuf_pool, struct tcp_segment *segment) {
    uint8_t *destination_mac = arp_lookup_mac(segment->destination_ip);
    if (destination_mac == NULL) {
        const uint64_t now = rte_rdtsc();
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
    const uint16_t frame_length =
        ipv4_frame_length(sizeof(struct rte_tcp_hdr) + segment->payload_length);
    struct rte_mbuf *frame = rte_pktmbuf_alloc(mbuf_pool);
    if (frame == NULL) {
        return -1;
    }
    if (rte_pktmbuf_append(frame, frame_length) == NULL) {
        rte_pktmbuf_free(frame);
        return -1;
    }
    tcp_encode_segment(ipv4_payload_data(frame), segment);
    return ipv4_output(frame, destination_mac, segment->source_ip,
                       segment->destination_ip, IPPROTO_TCP,
                       sizeof(struct rte_tcp_hdr) + segment->payload_length);
}
