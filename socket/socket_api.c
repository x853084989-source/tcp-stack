/*
 * 应用可见的n*接口：按类型或本程序分配的fd区间转到TCP/UDP实现。
 * 这些fd不是Linux内核fd，不能直接交给系统read/write/epoll操作。
 */

#include "socket_api.h"

#include <errno.h>
#include <netinet/in.h>

#include "config.h"
#include "transport/tcp_socket.h"
#include "socket/event_poll.h"
#include "transport/udp_socket.h"

/* 按IPv4与套接字类型选择本项目TCP/UDP端点，不调用Linux socket。 */
int nsocket(int domain, int type, int protocol) {
    if (domain == AF_INET && type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) {
        return udp_socket_create(domain, type, protocol);
    }

    if (domain == AF_INET && type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) {
        return tcp_socket_create();
    }

    errno = EPROTONOSUPPORT;
    return -1;
}

/* 按本程序的fd区间分流，合法性再由具体端点检查；不是从操作系统查询fd类型。 */
int nbind(int socket_fd, const struct sockaddr *address, socklen_t address_length) {
    if (socket_fd >= TCP_FIRST_FD) {
        return tcp_socket_bind(socket_fd, address, address_length);
    }

    return udp_socket_bind(socket_fd, address, address_length);
}

/* 转交TCP监听状态设置；UDP不走连接监听流程。 */
int nlisten(int socket_fd, int backlog) {
    return tcp_socket_listen(socket_fd, backlog);
}

/* 从监听端点取得已握手、尚未交给应用的连接；没有可接受连接时可阻塞。 */
int naccept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length) {
    return tcp_socket_accept(socket_fd, client_address, address_length);
}

/* TCP字节流读取入口；短缓冲区的剩余正文保存在端点中供下次读取。 */
ssize_t nrecv(int socket_fd, void *buffer, size_t buffer_length, int flags) {
    return tcp_socket_receive(socket_fd, buffer, buffer_length, flags);
}

/* TCP发送接纳入口；可能只接纳部分正文，返回长度不表示已经上网卡或收到ACK。 */
ssize_t nsend(int socket_fd, const void *payload, size_t payload_length, int flags) {
    return tcp_socket_send(socket_fd, payload, payload_length, flags);
}

/* UDP数据报读取入口，同时可返回发送者地址；短缓冲区会丢弃该报文剩余正文。 */
ssize_t nrecvfrom(int socket_fd, void *buffer, size_t buffer_length, int flags,
                  struct sockaddr *source_address, socklen_t *address_length) {
    return udp_socket_receive(socket_fd, buffer, buffer_length, flags, source_address,
                              address_length);
}

/* 把一个UDP数据报副本加入目的端点发送队列，后续由协议线程组包。 */
ssize_t nsendto(int socket_fd, const void *payload, size_t payload_length, int flags,
                const struct sockaddr *destination_address, socklen_t address_length) {
    return udp_socket_send(socket_fd, payload, payload_length, flags, destination_address,
                           address_length);
}

/* 先识别poll专用fd，再按TCP/UDP区间关闭，避免把poll误当成普通TCP连接。 */
int nclose(int socket_fd) {
    if (socket_fd == EVENT_POLL_FD) {
        return event_poll_close(socket_fd);
    }

    if (socket_fd >= TCP_FIRST_FD) {
        return tcp_socket_close(socket_fd);
    }

    return udp_socket_close(socket_fd);
}

/* 创建本项目唯一的TCP可读事件实例，size仅参与参数检查。 */
int nepoll_create(int size) {
    return event_poll_create(size);
}

/* 经TCP端点锁同步注册和当前可读状态，避免注册时漏掉已经到达的数据。 */
int nepoll_ctl(int epoll_fd, int operation, int socket_fd, struct epoll_event *event) {
    return tcp_socket_poll_control(epoll_fd, operation, socket_fd, event);
}

/* 等待自制就绪链表并复制事件给应用；timeout_ms为毫秒，负数等待，0立即返回。 */
int nepoll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout_ms) {
    return event_poll_wait(epoll_fd, events, max_events, timeout_ms);
}
