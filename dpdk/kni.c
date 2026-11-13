/*
 * KNI连接用户态DPDK与Linux内核网络栈，数据通道和设备控制通道分开。
 * 内核改接口状态时，由I/O lcore处理控制请求，避免其他线程停用正在收发的网卡。
 */

#include "kni.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <rte_ethdev.h>
#include <rte_kni.h>

#include "link/frame_queue.h"
#include "config.h"

static struct rte_kni *kernel_interface;

/* Read and changed only by the I/O lcore after initialization. */
static int port_running = 1;

/*
 * 由Linux接口up/down请求触发，实际在I/O lcore处理KNI请求时同步调用。
 * 先停端口并记录状态，再按请求决定是否启动，避免主循环继续访问已停的网卡队列。
 */
static int configure_network_interface(uint16_t port_id, uint8_t interface_up) {
    if (port_id != DPDK_PORT_ID || !rte_eth_dev_is_valid_port(port_id)) {
        return -EINVAL;
    }

    rte_eth_dev_stop(port_id);
    port_running = 0;

    /* Linux请求up时才重启端口；请求down时保持port_running=0，让I/O主循环暂停访问队列。 */
    if (interface_up) {
        int result = rte_eth_dev_start(port_id);
        if (result < 0) {
            return result;
        }

        port_running = 1;
    }

    return 0;
}

/* 创建vEth虚拟接口并注册控制回调；ops保存回调地址，此时并非直接执行up/down回调。 */
int kni_init(struct rte_mempool *mbuf_pool) {
    if (rte_kni_init(1) < 0) {
        return -1;
    }

    /* configuration是本次分配KNI接口的栈上配置；operations保存供后续控制请求使用的回调。 */
    struct rte_kni_conf configuration;
    memset(&configuration, 0, sizeof(configuration));
    snprintf(configuration.name, sizeof(configuration.name), "vEth%u", DPDK_PORT_ID);
    configuration.group_id = DPDK_PORT_ID;
    configuration.mbuf_size = rte_pktmbuf_data_room_size(mbuf_pool) - RTE_PKTMBUF_HEADROOM;
    rte_eth_macaddr_get(DPDK_PORT_ID, (struct rte_ether_addr *)configuration.mac_addr);
    if (rte_eth_dev_get_mtu(DPDK_PORT_ID, &configuration.mtu) < 0) {
        return -1;
    }

    struct rte_kni_ops operations;
    memset(&operations, 0, sizeof(operations));
    operations.port_id = DPDK_PORT_ID;
    operations.config_network_if = configure_network_interface;
    kernel_interface = rte_kni_alloc(mbuf_pool, &configuration, &operations);
    if (kernel_interface == NULL) {
        return -1;
    }

    printf("KNI interface %s created; configure its local IPv4 address before traffic.\n",
           configuration.name);

    return 0;
}

/* 接管一个输入mbuf：成功交给内核，未被KNI接纳则释放，调用方不能再使用它。 */
int kni_send(struct rte_mbuf *mbuf) {
    if (mbuf == NULL) {
        return -1;
    }

    /* burst返回真正接纳的数量；未接纳时mbuf仍归本函数，必须释放，不能假设调用就完成交接。 */
    if (kernel_interface == NULL || rte_kni_tx_burst(kernel_interface, &mbuf, 1) != 1) {
        rte_pktmbuf_free(mbuf);
        return -1;
    }

    return 0;
}

/* 协议lcore从内核取出待发mbuf，转入公共发送队列；网卡TX仍只由I/O lcore执行。 */
void kni_receive(void) {
    if (kernel_interface == NULL) {
        return;
    }

    struct rte_mbuf *mbufs[BURST_SIZE];
    unsigned packet_count = rte_kni_rx_burst(kernel_interface, mbufs, BURST_SIZE);
    for (unsigned i = 0; i < packet_count; i++) {
        enqueue_tx_frame(mbufs[i]);
    }
}

/* I/O lcore主动处理内核控制请求；回调由本调用驱动，并没有另建控制线程。 */
void kni_handle_requests(void) {
    if (kernel_interface != NULL) {
        rte_kni_handle_request(kernel_interface);
    }
}

/* 向I/O循环报告端口当前能否收发；状态由同一lcore的控制回调维护。 */
int kni_port_is_running(void) {
    return port_running;
}
