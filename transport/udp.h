/* UDP组包与解析接口；调用者持续持有udp_segment，返回值决定保留重试还是释放。 */

#ifndef MYPROGRAM_UDP_H
#define MYPROGRAM_UDP_H

#include <stdint.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct udp_segment;

/* Borrows mbuf and its headers; ethernet_input() releases the received mbuf. */
/* 借用输入帧验证UDP长度并取得正文，匹配本机目的地址后复制到应用接收队列。 */
void udp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header);

/* 0: output queued, 1: retry after ARP, -1: dropped. Caller owns segment. */
/* 一次本地输出尝试：截止则-1，等待ARP则1，成功入公共帧队列则0。 */
int udp_output(struct rte_mempool *mbuf_pool, struct udp_segment *segment);

#endif /* MYPROGRAM_UDP_H */
