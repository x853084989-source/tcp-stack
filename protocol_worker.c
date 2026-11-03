#include "protocol_worker.h"

#include <rte_cycles.h>
#include <rte_timer.h>

#include "config.h"
#include "link/frame_queue.h"
#include "link/ethernet.h"
#include "dpdk/kni.h"
#include "transport/udp_socket.h"
#include "transport/tcp_socket.h"

int protocol_worker(void *arg) {
    struct rte_mempool *mbuf_pool = arg;
    struct frame_queues *frame_queues = frame_queues_instance();
    uint64_t last_timer_manage_tsc = 0;

    while (1) {
        struct rte_mbuf *frames[BURST_SIZE];
        unsigned frame_count =
            rte_ring_dequeue_burst(frame_queues->receive, (void **)frames, BURST_SIZE, NULL);
        for (unsigned i = 0; i < frame_count; i++) {
            ethernet_input(frames[i]);
        }
        kni_receive();
        udp_socket_output(mbuf_pool);
        tcp_socket_output(mbuf_pool);
        uint64_t current_tsc = rte_rdtsc();
        if (current_tsc - last_timer_manage_tsc > TIMER_RESOLUTION_CYCLES) {
            rte_timer_manage();
            last_timer_manage_tsc = current_tsc;
        }
    }
    return 0;
}
