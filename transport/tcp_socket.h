/*
 * tcp_segment既可作为正文副本记录，也可描述待发送报文；它本身不是完整mbuf。
 * 发送记录由socket持有到确认/终止；重传可借用它构造临时视图，无需复制整份正文。
 */

#ifndef MYPROGRAM_TCP_SOCKET_H
#define MYPROGRAM_TCP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>

#include <rte_ip.h>
#include <rte_mempool.h>
#include <rte_tcp.h>
#include <sys/queue.h>

/* Addresses/ports are network order; sequence numbers and lengths are host order. */
struct tcp_segment {
    uint32_t source_ip;
    uint32_t destination_ip;
    uint16_t source_port;
    uint16_t destination_port;

    /* 本记录所描述范围的起始TCP序号；发送记录用主机序，编码首部时再转网络序。 */
    uint32_t seq_num;

    /* 我方向对方发出的累计确认号；不是对方对本记录的确认，后者从收到的TCP首部读取。 */
    uint32_t ack_num;

    /* SYN/ACK/FIN/PSH等位标志；SYN和FIN各占一个序号，ACK标志本身不占序号。 */
    uint8_t tcp_flags;

    /* 正文长度（字节），不包含TCP/IP/以太网头，也不包含SYN/FIN的序号占用。 */
    uint16_t payload_length;

    /* 原记录拥有的正文副本；重传view只借用其中一段，不能对借用指针单独释放。 */
    uint8_t *payload;

    /* 最早允许再次请求ARP的绝对计数；只限制ARP广播频率，不是等待TCP ACK的RTO。 */
    uint64_t next_arp_retry_tsc;

    /* 侵入式链表连接字段，节点就是本结构体；与链表表头及mbuf不是同一个对象。 */
    TAILQ_ENTRY(tcp_segment) unack_link;
};

/* 应用创建TCP端点的持锁入口；返回本程序fd，尚未绑定或进入连接状态。 */
int tcp_socket_create(void);

/* 仅允许本机/ANY地址和显式非0端口；端口占用检查与写入状态在同一把锁内完成。 */
int tcp_socket_bind(int socket_fd, const struct sockaddr *address, socklen_t address_length);

/* 把已绑定的CLOSED端点转为LISTEN；当前backlog参数尚未参与容量控制。 */
int tcp_socket_listen(int socket_fd, int backlog);

/* 持监听器引用等待握手完成；cond等待释放TCP锁，协议线程才能处理SYN/ACK并唤醒它。 */
int tcp_socket_accept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length);

/* TCP按字节流读取；receive_pending与receive_offset保存半读记录，读完才释放。 */
ssize_t tcp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags);

/* 把本次可接纳正文复制到发送队列；单次最多TCP_MAX_PAYLOAD_SIZE，应用需处理部分接纳。 */
ssize_t tcp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags);

/* 监听器关闭时清理尚未accept的子连接；连接关闭时排队FIN并退出应用可见集合。 */
int tcp_socket_close(int socket_fd);

/* Serializes registration plus current readiness with TCP state changes. */
/* 持TCP锁计算当前就绪并完成poll注册，避免先注册/后查询之间漏掉状态变化。 */
int tcp_socket_poll_control(int epoll_fd, int operation, int socket_fd,
                            const struct epoll_event *event);

/* Borrows checked input; updates connection state and copies accepted RX payload. */
/* 协议线程持TCP锁推进状态机；首部和payload仍借用输入mbuf，接收正文需复制后入队。 */
void tcp_socket_input(const struct rte_ipv4_hdr *ipv4_header, const struct rte_tcp_hdr *tcp_header,
                      const uint8_t *payload, uint16_t payload_length);

/* Protocol lcore advances new sends and checks RTO; records stay owned until ACK/close. */
/* 协议轮询入口：约每10ms允许一轮RTO到期检查，但每轮仍推进普通发送。 */
void tcp_socket_output(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_TCP_SOCKET_H */
