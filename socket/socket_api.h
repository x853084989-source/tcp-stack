/* 用户态socket门面。send成功表示本地接纳数据；接收和就绪通知由本项目实现。 */

#ifndef MYPROGRAM_SOCKET_API_H
#define MYPROGRAM_SOCKET_API_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>

/* User-space IPv4 interfaces; descriptors belong to this program, not Linux. */
/* 按IPv4与套接字类型选择本项目TCP/UDP端点，不调用Linux socket。 */
int nsocket(int domain, int type, int protocol);

/* 按本程序的fd区间分流，合法性再由具体端点检查；不是从操作系统查询fd类型。 */
int nbind(int socket_fd, const struct sockaddr *address, socklen_t address_length);

/* 转交TCP监听状态设置；UDP不走连接监听流程。 */
int nlisten(int socket_fd, int backlog);

/* 从监听端点取得已握手、尚未交给应用的连接；没有可接受连接时可阻塞。 */
int naccept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length);

/* TCP字节流读取入口；短缓冲区的剩余正文保存在端点中供下次读取。 */
ssize_t nrecv(int socket_fd, void *buffer, size_t buffer_length, int flags);

/* TCP发送接纳入口；可能只接纳部分正文，返回长度不表示已经上网卡或收到ACK。 */
ssize_t nsend(int socket_fd, const void *payload, size_t payload_length, int flags);

/* UDP数据报读取入口，同时可返回发送者地址；短缓冲区会丢弃该报文剩余正文。 */
ssize_t nrecvfrom(int socket_fd, void *buffer, size_t buffer_length, int flags,
                  struct sockaddr *source_address, socklen_t *address_length);

/* 把一个UDP数据报副本加入目的端点发送队列，后续由协议线程组包。 */
ssize_t nsendto(int socket_fd, const void *payload, size_t payload_length, int flags,
                const struct sockaddr *destination_address, socklen_t address_length);

/* 先识别poll专用fd，再按TCP/UDP区间关闭，避免把poll误当成普通TCP连接。 */
int nclose(int socket_fd);

/* User-space TCP readable notifications; timeout is milliseconds, negative means wait. */
/* 创建本项目唯一的TCP可读事件实例，size仅参与参数检查。 */
int nepoll_create(int size);

/* 经TCP端点锁同步注册和当前可读状态，避免注册时漏掉已经到达的数据。 */
int nepoll_ctl(int epoll_fd, int operation, int socket_fd, struct epoll_event *event);

/* 等待自制就绪链表并复制事件给应用；timeout_ms为毫秒，负数等待，0立即返回。 */
int nepoll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout_ms);

#endif /* MYPROGRAM_SOCKET_API_H */
