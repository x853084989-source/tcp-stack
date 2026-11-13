/* 公共帧队列中的对象是mbuf，与socket中的tcp_segment/udp_segment记录不同。 */

#ifndef MYPROGRAM_FRAME_QUEUE_H
#define MYPROGRAM_FRAME_QUEUE_H

#include <rte_mbuf.h>
#include <rte_ring.h>

struct frame_queues {
    /* I/O→协议的完整RX mbuf队列；出队后协议路径负责转交或释放。 */
    struct rte_ring *receive;

    /* 协议→I/O的完整TX mbuf队列；不能把tcp_segment指针直接放进来。 */
    struct rte_ring *send;
};

/* 在启动工作线程前创建两条ring；SP/SC分别限定单生产者和单消费者，依赖当前线程分工。 */
int frame_queues_init(void);

/* 返回进程内唯一的公共队列容器，只借用其地址，不分配新队列。 */
struct frame_queues *frame_queues_instance(void);

/* Consumes frame: queues it on success, frees it on failure. */
/* 接管frame：入公共TX队列成功返回0；失败时内部释放frame并返回-1，调用方不再释放。 */
int enqueue_tx_frame(struct rte_mbuf *frame);

#endif /* MYPROGRAM_FRAME_QUEUE_H */
