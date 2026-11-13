/*
 * 主lcore独占网卡收发：RX帧交协议队列，TX帧从公共发送队列取出。
 * DPDK批量接口返回实际处理数量，未交接的mbuf仍需本线程负责释放。
 */

#include "io_worker.h"

#include <stddef.h>
#include <rte_ethdev.h>

#include "config.h"
#include "link/frame_queue.h"
#include "monitor/ddos.h"
#include "dpdk/kni.h"

/* 主lcore的持续收发循环；RX入队后由协议线程消费，TX提交成功的mbuf交由驱动持有。 */
int io_worker(__attribute__((unused)) void *arg) {
    struct frame_queues *frame_queues = frame_queues_instance();
    while (1) {
        kni_handle_requests();

        /* 接口down期间不访问网卡队列；下一轮仍先处理KNI请求，才能接收后续up请求。 */
        if (!kni_port_is_running()) {
            continue;
        }

        struct rte_mbuf *receive_frames[BURST_SIZE];
        unsigned received_count = rte_eth_rx_burst(DPDK_PORT_ID, 0, receive_frames, BURST_SIZE);
        for (unsigned i = 0; i < received_count; i++) {
            ddos_detect(receive_frames[i]);
        }

        unsigned enqueued_count = rte_ring_enqueue_burst(
            frame_queues->receive, (void **)receive_frames, received_count, NULL);

        /* 接收ring可能只接纳前面一部分；未入队尾部仍归I/O线程，释放以免泄漏mbuf池。 */
        for (unsigned i = enqueued_count; i < received_count; i++) {
            rte_pktmbuf_free(receive_frames[i]);
        }

        struct rte_mbuf *send_frames[BURST_SIZE];
        unsigned dequeued_count =
            rte_ring_dequeue_burst(frame_queues->send, (void **)send_frames, BURST_SIZE, NULL);

        /* 有待发帧才调用网卡TX；返回sent_count之前的帧归驱动，尾部未接纳帧仍需自行释放。 */
        if (dequeued_count != 0) {
            unsigned sent_count = rte_eth_tx_burst(DPDK_PORT_ID, 0, send_frames, dequeued_count);

            /* The NIC owns [0, sent_count); only unsent mbufs remain ours. */
            for (unsigned i = sent_count; i < dequeued_count; i++) {
                rte_pktmbuf_free(send_frames[i]);
            }
        }
    }

    return 0;
}
