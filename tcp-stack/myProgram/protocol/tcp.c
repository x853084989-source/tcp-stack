#include "tcp.h"

#include <arpa/inet.h>
#include <stdint.h>

#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_memcpy.h>
#include <rte_tcp.h>

#include "common/app_state.h"
#include "common/net_arch.h"
#include "config.h"
#include "protocol/arp.h"
#include "socket/tcp_socket.h"

static void encode_tcp_packet(uint8_t *packet_data, const uint8_t *destination_mac,
                              const struct tcp_fragment *fragment) {
    struct rte_ether_hdr *ethernet_header = (struct rte_ether_hdr *)packet_data;
    rte_memcpy(ethernet_header->s_addr.addr_bytes, local_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(ethernet_header->d_addr.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);
    ethernet_header->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ipv4_header = (struct rte_ipv4_hdr *)(ethernet_header + 1);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->type_of_service = 0;
    ipv4_header->total_length =
        htons(sizeof(*ipv4_header) + sizeof(struct rte_tcp_hdr) + fragment->payload_length);
    ipv4_header->packet_id = 0;
    ipv4_header->fragment_offset = 0;
    ipv4_header->time_to_live = 64;
    ipv4_header->next_proto_id = IPPROTO_TCP;
    ipv4_header->src_addr = fragment->source_ip;
    ipv4_header->dst_addr = fragment->destination_ip;
    ipv4_header->hdr_checksum = 0;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);

    struct rte_tcp_hdr *tcp_header = (struct rte_tcp_hdr *)(ipv4_header + 1);
    tcp_header->src_port = fragment->source_port;
    tcp_header->dst_port = fragment->destination_port;
    tcp_header->sent_seq = htonl(fragment->sequence_number);
    tcp_header->recv_ack = htonl(fragment->acknowledgement_number);
    tcp_header->data_off = 0x50;
    tcp_header->tcp_flags = fragment->tcp_flags;
    tcp_header->rx_win = htons(TCP_INITIAL_WINDOW);
    tcp_header->tcp_urp = 0;
    if (fragment->payload_length != 0) {
        rte_memcpy((uint8_t *)(tcp_header + 1), fragment->payload, fragment->payload_length);
    }
    tcp_header->cksum = 0;
    tcp_header->cksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
}

static struct rte_mbuf *build_tcp_packet(struct rte_mempool *mbuf_pool,
                                         const uint8_t *destination_mac,
                                         const struct tcp_fragment *fragment) {
    const uint16_t total_length = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                                  sizeof(struct rte_tcp_hdr) + fragment->payload_length;
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (mbuf == NULL) {
        return NULL;
    }
    uint8_t *packet_data = (uint8_t *)rte_pktmbuf_append(mbuf, total_length);
    if (packet_data == NULL) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }
    encode_tcp_packet(packet_data, destination_mac, fragment);
    return mbuf;
}

void process_tcp_packet(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header) {
    const uint16_t ip_length = ntohs(ipv4_header->total_length);
    const uint16_t fragment_offset = ntohs(ipv4_header->fragment_offset);
    if (!rte_pktmbuf_is_contiguous(mbuf) || ipv4_header->version_ihl != 0x45 ||
        ipv4_header->dst_addr != local_host_ip ||
        ip_length < sizeof(*ipv4_header) + sizeof(struct rte_tcp_hdr) ||
        ip_length + sizeof(struct rte_ether_hdr) > rte_pktmbuf_pkt_len(mbuf) ||
        (fragment_offset & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
        return;
    }

    struct rte_tcp_hdr *tcp_header = (struct rte_tcp_hdr *)(ipv4_header + 1);
    const uint16_t tcp_length = ip_length - sizeof(*ipv4_header);
    const uint16_t header_length = (tcp_header->data_off >> 4) * 4;
    if (header_length < sizeof(*tcp_header) || header_length > tcp_length) {
        return;
    }
    const uint16_t received_checksum = tcp_header->cksum;
    tcp_header->cksum = 0;
    const uint16_t checksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    tcp_header->cksum = received_checksum;
    if (checksum != received_checksum) {
        return;
    }
    const uint8_t *payload = (const uint8_t *)tcp_header + header_length;
    tcp_socket_deliver(ipv4_header, tcp_header, payload, tcp_length - header_length);
}

int tcp_output(struct rte_mempool *mbuf_pool, struct tcp_fragment *fragment) {
    uint8_t *destination_mac = arp_lookup_mac(fragment->destination_ip);
    if (destination_mac == NULL) {
        const uint64_t now = rte_rdtsc();
        if (now >= fragment->next_arp_retry_tsc) {
            static uint8_t broadcast_mac[RTE_ETHER_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            struct rte_mbuf *request =
                build_arp_packet(mbuf_pool, RTE_ARP_OP_REQUEST, broadcast_mac, fragment->source_ip,
                                 fragment->destination_ip);
            queue_tx_packet(request);
            fragment->next_arp_retry_tsc = now + rte_get_timer_hz() * ARP_RETRY_SECONDS;
        }
        return 1;
    }
    return queue_tx_packet(build_tcp_packet(mbuf_pool, destination_mac, fragment));
}
