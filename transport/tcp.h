/* TCP报文层契约；mbuf与长期保存的tcp_segment是不同对象，释放责任不能混用。 */

#ifndef MYPROGRAM_TCP_H
#define MYPROGRAM_TCP_H

#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct tcp_segment;

/* Borrows mbuf; ethernet_input retains ownership. */
/* 借用已分流的IPv4帧，检查TCP头长与校验和，再把首部/正文视图交连接状态机。 */
void tcp_input(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ipv4_header);

/* 0: queued to the send frame queue, 1: waiting for ARP, -1: dropped; caller owns segment. */
/* 把segment所描述的范围编码成新的mbuf；始终不接管或释放segment。 */
int tcp_output(struct rte_mempool *mbuf_pool, struct tcp_segment *segment);

#endif /* MYPROGRAM_TCP_H */
