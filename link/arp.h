/* ARP表节点保存IP到MAC的映射，地址保持网络序；由协议lcore串行访问。 */

#ifndef MYPROGRAM_ARP_H
#define MYPROGRAM_ARP_H

#include <stdint.h>

#include <rte_arp.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#define ARP_ENTRY_STATUS_DYNAMIC 0
#define ARP_ENTRY_STATUS_STATIC 1

struct arp_table_item {
    uint32_t ip;
    uint8_t mac_addr[RTE_ETHER_ADDR_LEN];

    uint8_t type;

    struct arp_table_item *next;
    struct arp_table_item *prev;
};

struct arp_table {
    struct arp_table_item *top_table;
    int count;
};

/* 懒创建唯一ARP表；正常运行中由协议lcore访问，不另加跨线程锁。 */
struct arp_table *arp_table_instance(void);

/* The returned MAC address is borrowed from the ARP table. */
/* 查询网络序目标IP，命中返回表内MAC的借用指针；未命中让上层进入ARP等待路径。 */
uint8_t *arp_lookup_mac(uint32_t destination_ip);

/* Protocol lcore learns mappings without generating a second ARP reply. */
/* 从已检查的以太网/IPv4来源学习映射；已有IP则更新MAC，否则新增节点。 */
void arp_learn(uint32_t source_ip, const uint8_t *source_mac);

/* 从mbuf_pool取得一帧并编码ARP；返回后由调用方交给发送队列，分配失败沿用退出策略。 */
struct rte_mbuf *build_arp_frame(struct rte_mempool *mbuf_pool, uint16_t opcode,
                                  uint8_t *destination_mac, uint32_t source_ip,
                                  uint32_t destination_ip);

/* Borrows the received header; the packet loop releases the received mbuf. */
/* 保留的旧ARP请求应答/回复入表逻辑；当前ethernet_input主要学习后交KNI，并不调用此入口。 */
void arp_input(struct rte_mempool *mbuf_pool, struct rte_arp_hdr *arp_header);

/* rte_timer回调：tim是到期定时器，arg是main注册时传入的mbuf池。 */
void arp_request_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg);

#endif /* MYPROGRAM_ARP_H */
