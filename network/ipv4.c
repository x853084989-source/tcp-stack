/*
 * IPv4层保留完整以太网帧，TCP/UDP借用其中的首部与正文进行处理。
 * TCP/UDP处理后本层释放输入mbuf；其他协议转交KNI，不能再重复释放。
 */

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

/* 先验证IP长度，再按协议分流；TCP/UDP只借用输入帧，本函数在它们返回后统一释放。 */
void ipv4_input(struct rte_mbuf *frame, const struct rte_ether_hdr *ethernet_header) {
    if (rte_pktmbuf_data_len(frame) <
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
        rte_pktmbuf_free(frame);
        return;
    }

    struct rte_ipv4_hdr *ipv4_header = (struct rte_ipv4_hdr *)ethernet_payload_data(frame);
    unsigned ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;
    unsigned ipv4_datagram_length = rte_be_to_cpu_16(ipv4_header->total_length);

    /* IHL低4位以4字节为单位，total_length包含IP头；所有后续首部必须落在实际收到的帧内。 */
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

    /* UDP/TCP入口会复制应用需要的正文而不接管RX mbuf，因此返回后由本层释放整帧。 */
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

/*
 * 返回1表示当前简化传输层可以继续解析，0表示不支持或边界非法。
 * 这里拒绝IP分片；TCP要求固定20字节IP头，UDP保留其可变头长处理范围。
 */
int ipv4_validate_transport(const struct rte_mbuf *frame,
                            const struct rte_ipv4_hdr *ipv4_header, uint8_t protocol) {
    const uint16_t ipv4_datagram_length = ntohs(ipv4_header->total_length);
    const uint16_t fragment_offset = ntohs(ipv4_header->fragment_offset);

    /* TCP仅支持连续内存、固定IP头、本机目的地址和无分片；不是完整IP重组/选项实现。 */
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

/* 上层传输段长度加IP头，再由以太网层计入MAC头，得到完整帧长度。 */
unsigned ipv4_frame_length(unsigned payload_length) {
    return ethernet_frame_length(sizeof(struct rte_ipv4_hdr) + payload_length);
}

/* 输出组包用固定20字节IP头后的地址，与输入可变头长视图分开。 */
uint8_t *ipv4_payload_data(struct rte_mbuf *frame) {
    return ethernet_payload_data(frame) + sizeof(struct rte_ipv4_hdr);
}

/* 根据输入IHL计算传输首部地址；调用前必须完成长度检查，返回指针仍借用原mbuf。 */
uint8_t *ipv4_input_payload(struct rte_mbuf *frame,
                                  const struct rte_ipv4_hdr *ipv4_header) {
    const unsigned ipv4_header_length = (ipv4_header->version_ihl & 0x0f) * 4;

    return rte_pktmbuf_mtod_offset(frame, uint8_t *,
                                   sizeof(struct rte_ether_hdr) + ipv4_header_length);
}

/* 用临时IP伪首部提供地址、协议和长度，计算TCP/UDP校验和；不把该伪首部发送上网。 */
uint16_t ipv4_payload_checksum(const void *payload, uint16_t payload_length,
                               uint32_t source_ip, uint32_t destination_ip, uint8_t protocol) {
    /* pseudo_header是临时校验和输入，不是另一份真实IP报文；库函数据此组合TCP/UDP伪首部。 */
    struct rte_ipv4_hdr pseudo_header = {0};
    pseudo_header.total_length = htons(sizeof(pseudo_header) + payload_length);
    pseudo_header.next_proto_id = protocol;
    pseudo_header.src_addr = source_ip;
    pseudo_header.dst_addr = destination_ip;

    return rte_ipv4_udptcp_cksum(&pseudo_header, payload);
}

/* 补固定IPv4首部及首部校验和，再交以太网输出；frame由下层入队或释放。 */
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
