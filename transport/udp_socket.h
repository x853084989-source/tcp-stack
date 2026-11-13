/* UDP记录保存完整数据报边界及地址；正文是独立副本，截断接收后剩余部分被丢弃。 */

#ifndef MYPROGRAM_UDP_SOCKET_H
#define MYPROGRAM_UDP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <rte_mempool.h>

struct udp_segment {
    /* Addresses and ports stay in network byte order; payload_length is payload bytes. */
    uint32_t source_ip;
    uint32_t destination_ip;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t payload_length;
    uint8_t *payload;

    /* 单条记录下一次允许发ARP请求的时刻，避免高频广播；不等于总发送期限。 */
    uint64_t next_arp_retry_tsc;

    /* 从send接纳时起的本地输出总截止时刻；过期丢弃，不是对端ACK超时。 */
    uint64_t send_deadline_tsc;
};

/* Internal UDP branches used by the shared application interface. */
/* 创建IPv4 UDP端点、互斥锁与收发ring；资源全就绪后才加入全局注册表。 */
int udp_socket_create(int domain, int type, int protocol);

/* 校验显式端口与本机地址，按注册表锁→端点锁的顺序完成冲突检查和绑定。 */
int udp_socket_bind(int socket_fd, const struct sockaddr *addr, socklen_t address_length);

/* Receives into the caller buffer; a short buffer discards the datagram tail. */
/* 一次读取一个数据报；缓冲区较小时丢掉该报文剩余部分，不留到下次recvfrom。 */
ssize_t udp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags,
                           struct sockaddr *source_address, socklen_t *address_length);

/* Copies payload into send_queue; success does not imply delivery to the peer. */
/* 复制一份数据报、设置来源/目标和本地发送总期限，再交发送队列持有。 */
ssize_t udp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags,
                        const struct sockaddr *destination_address, socklen_t address_length);

/* 标记closing并唤醒接收者；取得引用的并发操作完成之前，不能直接free端点。 */
int udp_socket_close(int socket_fd);

/* Borrows payload; makes a private copy for the selected application's receive_queue. */
/* 匹配绑定端点并复制输入正文，成功入接收队列后唤醒应用；原RX帧仍归上层。 */
int udp_socket_input(uint32_t source_ip, uint16_t source_port, uint32_t destination_ip,
                     uint16_t destination_port, const uint8_t *payload, uint16_t payload_length);

/* Protocol lcore advances a bounded send budget; ARP misses remain queued. */
/* 先取得端点引用快照，再逐端点按有限预算输出；ARP等待的记录重新入队。 */
void udp_socket_output(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_UDP_SOCKET_H */
