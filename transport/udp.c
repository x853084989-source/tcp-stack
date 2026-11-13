/*
 * UDP报文层：输出等待ARP可以重试，但不会等待UDP接收确认或进行协议重传。
 * 这里的发送截止时间限制本地排队/解析等待，不能理解成对端ACK超时。
 */

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

/* 编码UDP首部与正文，长度包含UDP头；利用IP伪首部计算传输校验和。 */
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

/* 借用输入帧验证UDP长度并取得正文，匹配本机目的地址后复制到应用接收队列。 */
void udp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header) {
    const uint16_t ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;
    const uint16_t ipv4_datagram_length = ntohs(ipv4_header->total_length);
    if (!ipv4_validate_transport(mbuf, ipv4_header, IPPROTO_UDP)) {
        return;
    }

    const struct rte_udp_hdr *udp_header =
        (const struct rte_udp_hdr *)ipv4_input_payload(mbuf, ipv4_header);

    const uint16_t udp_segment_length = ntohs(udp_header->dgram_len);

    /* UDP声明长度必须容纳自己的头且不能越过IP有效载荷；否则正文指针/长度不可信。 */
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

/*
 * 一次本地输出尝试：截止则-1，等待ARP则1，成功入公共帧队列则0。
 * 不会等待UDP确认；调用方依据返回值决定重排队还是释放原segment。
 */
int udp_output(struct rte_mempool *mbuf_pool, struct udp_segment *segment) {
    const uint64_t now = rte_rdtsc();

    /* 本地等待总期限已到就放弃该数据报；即使始终解析不到MAC，也不会无限保留。 */
    if (now >= segment->send_deadline_tsc) {
        return -1;
    }

    uint8_t *destination_mac = arp_lookup_mac(segment->destination_ip);

    /* 目的MAC未知时不能直接编码以太网头；返回1让上层保留/重排队，并节流ARP请求。 */
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
