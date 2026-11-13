/* 收发线程与协议线程之间的mbuf交接队列；队列传指针，不复制完整报文。 */

#include "frame_queue.h"

#include <stddef.h>


#include "config.h"

static struct frame_queues shared_frame_queues;

/* 返回进程内唯一的公共队列容器，只借用其地址，不分配新队列。 */
struct frame_queues *frame_queues_instance(void) {
    return &shared_frame_queues;
}

/* 在启动工作线程前创建两条ring；SP/SC分别限定单生产者和单消费者，依赖当前线程分工。 */
int frame_queues_init(void) {
    /* RX ring：I/O线程是唯一生产者，协议线程是唯一消费者，所以选择SP/SC。 */
    shared_frame_queues.receive =
        rte_ring_create("myProgram_in", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (shared_frame_queues.receive == NULL) {
        return -1;
    }

    /* Protocol processing and its ARP timer run on the same lcore. */
    shared_frame_queues.send =
        rte_ring_create("myProgram_out", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (shared_frame_queues.send == NULL) {
        rte_ring_free(shared_frame_queues.receive);
        shared_frame_queues.receive = NULL;
        return -1;
    }

    return 0;
}

/* 接管frame：入公共TX队列成功返回0；失败时内部释放frame并返回-1，调用方不再释放。 */
int enqueue_tx_frame(struct rte_mbuf *frame) {
    if (frame == NULL) {
        return -1;
    }

    /* ring未初始化或容量已满，都表示没有交接成功；此接口接管frame，因此统一在这里释放。 */
    if (shared_frame_queues.send == NULL ||
        rte_ring_enqueue(shared_frame_queues.send, frame) != 0) {
        rte_pktmbuf_free(frame);
        return -1;
    }

    return 0;
}
