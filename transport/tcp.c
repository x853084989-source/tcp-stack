/*
 * TCP报文解析与编码。输入借用mbuf，输出根据segment构造新的完整mbuf。
 * 输出返回0仅代表进入公共发送队列；1为等待ARP；-1为本地失败，segment仍归调用方。
 */

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

/* segment描述本次需要发的范围；在新mbuf中编码首部并复制正文，不修改原记录的正文。 */
static void tcp_encode_segment(uint8_t *segment_data,
                                const struct tcp_segment *segment) {
    struct rte_tcp_hdr *tcp_header = (struct rte_tcp_hdr *)segment_data;
    tcp_header->src_port = segment->source_port;
    tcp_header->dst_port = segment->destination_port;
    tcp_header->sent_seq = htonl(segment->seq_num);
    tcp_header->recv_ack = htonl(segment->ack_num);

    /* data_off高4位以4字节为单位描述头长，0x50表示20字节固定TCP头，当前不编码选项。 */
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

/* 借用已分流的IPv4帧，检查TCP头长与校验和，再把首部/正文视图交连接状态机。 */
void tcp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header) {
    const uint16_t ipv4_datagram_length = ntohs(ipv4_header->total_length);
    if (!ipv4_validate_transport(mbuf, ipv4_header, IPPROTO_TCP)) {
        return;
    }

    struct rte_tcp_hdr *tcp_header = (struct rte_tcp_hdr *)ipv4_input_payload(mbuf, ipv4_header);
    const uint16_t tcp_segment_length = ipv4_datagram_length - sizeof(*ipv4_header);
    const uint16_t tcp_header_length = (tcp_header->data_off >> 4) * 4;

    /* 头长不得小于固定头，也不能越过IP内有效负载；验证后才能安全定位正文。 */
    if (tcp_header_length < sizeof(*tcp_header) || tcp_header_length > tcp_segment_length) {
        return;
    }

    const uint16_t received_checksum = tcp_header->cksum;

    /* 保存收到的校验和后暂时清零以重算，再恢复原值；原mbuf仍可能被后续代码观察。 */
    tcp_header->cksum = 0;

    const uint16_t checksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    tcp_header->cksum = received_checksum;

    /* 损坏报文不能进入连接状态机，否则错误序号/ACK可能推进状态或释放未确认数据。 */
    if (checksum != received_checksum) {
        return;
    }

    const uint8_t *payload = (const uint8_t *)tcp_header + tcp_header_length;
    tcp_socket_input(ipv4_header, tcp_header, payload, tcp_segment_length - tcp_header_length);
}

/*
 * 把segment所描述的范围编码成新的mbuf；始终不接管或释放segment。
 * 返回0表示公共TX队列接纳；1表示等待ARP；-1表示本地失败，均不等同对端已经确认。
 */
int tcp_output(struct rte_mempool *mbuf_pool, struct tcp_segment *segment) {
    uint8_t *destination_mac = arp_lookup_mac(segment->destination_ip);

    /* 缺目的MAC就不能构造可用以太网帧；仅请求ARP并返回1，调用方保留原发送记录。 */
    if (destination_mac == NULL) {
        const uint64_t now = rte_rdtsc();

        /* 到节流时刻才广播；该ARP时刻使用TSC路径，不要当成TCP每连接ACK等待期限。 */
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

    /* append为本次帧增长有效数据区并检查尾部容量；失败时释放新mbuf，原segment仍保留。 */
    if (rte_pktmbuf_append(frame, frame_length) == NULL) {
        rte_pktmbuf_free(frame);
        return -1;
    }

    tcp_encode_segment(ipv4_payload_data(frame), segment);

    return ipv4_output(frame, destination_mac, segment->source_ip,
                       segment->destination_ip, IPPROTO_TCP,
                       sizeof(struct rte_tcp_hdr) + segment->payload_length);
}
