/* IPv4层的数据视图、校验和与分层组包接口；输入输出的对象归属由接口约定决定。 */

#ifndef MYPROGRAM_IPV4_H
#define MYPROGRAM_IPV4_H

#include <stdint.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

/* Consumes the frame; keeps full Ethernet bytes intact on the KNI branch. */
/* 先验证IP长度，再按协议分流；TCP/UDP只借用输入帧，本函数在它们返回后统一释放。 */
void ipv4_input(struct rte_mbuf *frame, const struct rte_ether_hdr *ethernet_header);

/* Retains the existing TCP/UDP-specific acceptance rules. */
/* 返回1表示当前简化传输层可以继续解析，0表示不支持或边界非法。 */
int ipv4_validate_transport(const struct rte_mbuf *frame,
                            const struct rte_ipv4_hdr *ipv4_header, uint8_t protocol);

/* 上层传输段长度加IP头，再由以太网层计入MAC头，得到完整帧长度。 */
unsigned ipv4_frame_length(unsigned payload_length);

/* 输出组包用固定20字节IP头后的地址，与输入可变头长视图分开。 */
uint8_t *ipv4_payload_data(struct rte_mbuf *frame);

/* Borrow the input payload after the validated, possibly variable-length IP header. */
/* 根据输入IHL计算传输首部地址；调用前必须完成长度检查，返回指针仍借用原mbuf。 */
uint8_t *ipv4_input_payload(struct rte_mbuf *frame,
                                  const struct rte_ipv4_hdr *ipv4_header);

/* Pseudo-header checksum over a transport payload; does not write an IP header. */
/* 用临时IP伪首部提供地址、协议和长度，计算TCP/UDP校验和；不把该伪首部发送上网。 */
uint16_t ipv4_payload_checksum(const void *payload, uint16_t payload_length,
                               uint32_t source_ip, uint32_t destination_ip, uint8_t protocol);

/* Consumes an allocated frame whose transport payload is already encoded. */
/* 补固定IPv4首部及首部校验和，再交以太网输出；frame由下层入队或释放。 */
int ipv4_output(struct rte_mbuf *frame, const uint8_t *destination_mac,
                uint32_t source_ip, uint32_t destination_ip, uint8_t protocol,
                uint16_t payload_length);

#endif /* MYPROGRAM_IPV4_H */
