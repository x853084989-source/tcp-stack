/*
 * 协议lcore入口：取接收帧、运行协议栈、推进应用发送和计时任务。
 * 这里轮询公共队列；应用线程的accept/recv等待不会阻止本循环继续运行。
 */

#include "protocol_worker.h"

#include <rte_cycles.h>
#include <rte_timer.h>

#include "config.h"
#include "link/frame_queue.h"
#include "link/ethernet.h"
#include "dpdk/kni.h"
#include "transport/udp_socket.h"
#include "transport/tcp_socket.h"

/* arg由main传入mbuf池；每轮即使没有RX包，也要推进发送及TCP计时，避免空闲连接无法重传。 */
int protocol_worker(void *arg) {
    struct rte_mempool *mbuf_pool = arg;
    struct frame_queues *frame_queues = frame_queues_instance();
    uint64_t last_timer_manage_tsc = 0;

    while (1) {
        struct rte_mbuf *frames[BURST_SIZE];

        /* burst出队返回实际取到的帧数，空队列返回0，不等待新包；因此后续计时仍会执行。 */
        unsigned frame_count =
            rte_ring_dequeue_burst(frame_queues->receive, (void **)frames, BURST_SIZE, NULL);
        for (unsigned i = 0; i < frame_count; i++) {
            ethernet_input(frames[i]);
        }

        kni_receive();
        udp_socket_output(mbuf_pool);

        /* 即使frame_count为0也推进TCP输出和RTO；否则应用静默、没有入包时无法恢复丢包。 */
        tcp_socket_output(mbuf_pool);

        /* rte_rdtsc读取TSC绝对计数；与上次值相减得到经过的计数，不是直接得到秒数。 */
        uint64_t current_tsc = rte_rdtsc();
        if (current_tsc - last_timer_manage_tsc > TIMER_RESOLUTION_CYCLES) {
            /*
             * 只检查分配给当前lcore的DPDK定时器，并在此线程同步执行到期回调。
             * 调用频率影响ARP回调实际延迟；TCP RTO并不注册在这个DPDK定时器表中。
             */
            rte_timer_manage();
            last_timer_manage_tsc = current_tsc;
        }
    }

    return 0;
}
