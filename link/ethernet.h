/* 输入接口接管整帧；数据位置辅助函数只借用指针，不移动mbuf的data_off。 */

#ifndef MYPROGRAM_ETHERNET_H
#define MYPROGRAM_ETHERNET_H

#include <stdint.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

/* Consumes frame, either by freeing it or transferring it to KNI. */
/* 接管RX整帧：检查可读范围，学习来源映射，将IPv4交本栈，其他帧交KNI。 */
void ethernet_input(struct rte_mbuf *frame);

/* 把上层负载长度加上以太网首部长度，供mbuf预留完整帧空间。 */
unsigned ethernet_frame_length(unsigned payload_length);

/* Borrowed views; frame data offset is never advanced. */
/* 返回以太网首部后的可写地址；只是视图，不出队、不复制、不改变data_off。 */
uint8_t *ethernet_payload_data(struct rte_mbuf *frame);

/* Consumes an allocated frame with its payload already encoded. */
/* 上层已经写好IP和传输层内容；这里补MAC头并把mbuf所有权交给公共发送队列。 */
int ethernet_output(struct rte_mbuf *frame, const uint8_t *destination_mac,
                    uint16_t ether_type);

#endif /* MYPROGRAM_ETHERNET_H */
