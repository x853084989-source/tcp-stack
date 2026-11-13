/*
 * 启动与线程分工：初始化DPDK、网卡、共享队列和ARP定时器。
 * 协议lcore处理报文与TCP计时；主lcore负责网卡I/O；应用pthread可阻塞等待。
 */

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

/* pthread入口适配器：调用UDP应用循环；协议和网卡工作由其他执行角色完成。 */
static void *run_udp_application(void *arg) {
    udp_server_entry(arg);

    return NULL;
}

/* pthread入口适配器：调用TCP回显应用，不在此线程处理TCP报文状态机。 */
static void *run_tcp_application(void *arg) {
    tcp_server_entry(arg);

    return NULL;
}

/* 按依赖顺序创建共享资源，再安排协议lcore与应用线程，最后由主lcore进入I/O循环。 */
int main(int argc, char *argv[]) {
    /* EAL先解析-l/-n等DPDK参数并准备大页、lcore和设备；后面的DPDK资源依赖这一步。 */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL init\n");
    }

    /* 至少启用主I/O和协议两个lcore；lcore是DPDK执行单元，不是按源码函数数量新建线程。 */
    unsigned required_lcores = 2;
    if (rte_lcore_count() < required_lcores) {
        rte_exit(EXIT_FAILURE, "This configuration needs at least %u enabled lcores\n",
                 required_lcores);
    }

    /* 从编号起点寻找已启用工作lcore：第二个参数1跳过主lcore，第三个参数0表示不回绕搜索。 */
    unsigned protocol_lcore = rte_get_next_lcore(-1, 1, 0);

    /* mbuf_pool是全栈共用的报文缓冲池，网卡RX及ARP/TCP/UDP构包从中取得mbuf；不是某个连接的正文副本。 */
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

    /* 初始化DPDK定时器子系统；它维护定时器状态，不会自动创建一个不断执行回调的线程。 */
    rte_timer_subsystem_init();

    /*
     * ARP使用DPDK回调定时器，TCP RTO则由协议循环比较每连接截止时间，两者机制不同。
     * arp_timer放在main栈上；main随后进入持续I/O循环，因此定时器对象不会提前离开作用域。
     */
    struct rte_timer arp_timer;

    /* 把该定时器置为可注册的停止状态，不能把未初始化的栈内存直接交给reset。 */
    rte_timer_init(&arp_timer);

    /* The ARP table, lookup and timer callback have one protocol-lcore owner. */
    /*
     * 注册参数：&arp_timer为对象；hz个ticks为1秒；PERIODICAL为重复到期；
     * protocol_lcore指定执行回调的lcore；arp_request_timer_cb为回调地址；
     * mbuf_pool作为arg原样传给回调，用于构造ARP报文。reset返回0表示注册成功。
     * 实际执行还需该lcore调用rte_timer_manage；注册1秒不保证每秒准时执行。
     */
    ret = rte_timer_reset(&arp_timer, rte_get_timer_hz(), PERIODICAL, protocol_lcore,
                          arp_request_timer_cb, mbuf_pool);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Could not start ARP timer\n");
    }

    /* 向已经启用的工作lcore安排protocol_worker，并把mbuf_pool作为arg传入；不是普通同步调用。 */
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

    /* 应用线程长期运行且main不join；detach让线程退出时由系统回收线程资源。 */
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
