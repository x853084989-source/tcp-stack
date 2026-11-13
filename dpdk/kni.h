/* KNI接口：send把接收帧交给内核，receive把内核待发帧交给公共发送队列。 */

#ifndef MYPROGRAM_KNI_H
#define MYPROGRAM_KNI_H

#include <rte_mbuf.h>
#include <rte_mempool.h>

/* 创建vEth虚拟接口并注册控制回调；ops保存回调地址，此时并非直接执行up/down回调。 */
int kni_init(struct rte_mempool *mbuf_pool);

/* Consumes one packet: transfers it to the kernel, or frees it on failure. */
/* 接管一个输入mbuf：成功交给内核，未被KNI接纳则释放，调用方不能再使用它。 */
int kni_send(struct rte_mbuf *mbuf);

/* Protocol lcore: enqueues kernel frames for sending; does not access NIC queues. */
/* 协议lcore从内核取出待发mbuf，转入公共发送队列；网卡TX仍只由I/O lcore执行。 */
void kni_receive(void);

/* I/O lcore only: control callbacks may stop/restart its NIC. */
/* I/O lcore主动处理内核控制请求；回调由本调用驱动，并没有另建控制线程。 */
void kni_handle_requests(void);

/* 向I/O循环报告端口当前能否收发；状态由同一lcore的控制回调维护。 */
int kni_port_is_running(void);

#endif /* MYPROGRAM_KNI_H */
