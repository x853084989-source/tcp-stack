/* 旧ICMP组包/处理接口；默认接收分流并未调用icmp_input。 */

#ifndef MYPROGRAM_ICMP_H
#define MYPROGRAM_ICMP_H

#include <stdint.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

/* Legacy self-stack echo reply; default RX still hands ICMP frames to KNI. */
/* 构造一份新的固定长度回显回复并交IP层；输入请求缓冲区不被此函数接管。 */
int icmp_output(struct rte_mempool *mbuf_pool, uint8_t *destination_mac,
                uint32_t source_ip, uint32_t destination_ip, uint16_t id, uint16_t seqnb);

/* 旧用户态ICMP入口：仅对Echo Request构造回复；当前默认接收路径交KNI处理。 */
void icmp_input(struct rte_mempool *mbuf_pool, struct rte_mbuf *mbuf,
                struct rte_ether_hdr *ethernet_header, struct rte_ipv4_hdr *ipv4_header);

#endif /* MYPROGRAM_ICMP_H */
