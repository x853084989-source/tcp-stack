#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#include "network_interface.h"
#include "dpdk/kni.h"
#include "link/arp.h"
#include "config.h"
#include "link/frame_queue.h"
#include "link/io_worker.h"
#include "protocol_worker.h"
#include "dpdk/port_init.h"
#include "app/tcp_app.h"
#include "app/udp_app.h"

static void *run_udp_application(void *arg) {
    udp_server_entry(arg);
    return NULL;
}

static void *run_tcp_application(void *arg) {
    tcp_server_entry(arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL init\n");
    }

    unsigned required_lcores = 2;
    if (rte_lcore_count() < required_lcores) {
        rte_exit(EXIT_FAILURE, "This configuration needs at least %u enabled lcores\n",
                 required_lcores);
    }
    unsigned protocol_lcore = rte_get_next_lcore(-1, 1, 0);

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
        "mbuf pool", NUM_MBUFS, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Could not create mbuf pool\n");
    }

    if (frame_queues_init() != 0) {
        rte_exit(EXIT_FAILURE, "Could not create packet rings\n");
    }
    port_init(mbuf_pool);
    rte_eth_macaddr_get(DPDK_PORT_ID, (struct rte_ether_addr *)local_mac);
    if (kni_init(mbuf_pool) != 0) {
        rte_exit(EXIT_FAILURE, "Could not initialize KNI; check rte_kni and /dev/kni\n");
    }
    rte_timer_subsystem_init();

    struct rte_timer arp_timer;
    rte_timer_init(&arp_timer);
    /* The ARP table, lookup and timer callback have one protocol-lcore owner. */
    ret = rte_timer_reset(&arp_timer, rte_get_timer_hz(), PERIODICAL, protocol_lcore,
                          arp_request_timer_cb, mbuf_pool);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Could not start ARP timer\n");
    }

    ret = rte_eal_remote_launch(protocol_worker, mbuf_pool, protocol_lcore);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Could not launch protocol lcore\n");
    }

    /* A blocking application thread can share a CPU on a two-vCPU VM. */
    pthread_t udp_app_thread;
    ret = pthread_create(&udp_app_thread, NULL, run_udp_application, NULL);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Could not launch UDP application thread: %d\n", ret);
    }
    pthread_detach(udp_app_thread);

    pthread_t tcp_app_thread;
    ret = pthread_create(&tcp_app_thread, NULL, run_tcp_application, NULL);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Could not launch TCP application thread: %d\n", ret);
    }
    pthread_detach(tcp_app_thread);

    printf("Packet I/O lcore: %u, protocol lcore: %u\n", rte_lcore_id(), protocol_lcore);
    return io_worker(NULL);
}
