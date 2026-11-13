/*
 * TCP连接状态、应用队列与可靠传输记录。
 * 应用线程调用socket接口；协议线程处理输入、新发送和每连接RTO，共用全局TCP锁。
 * 当前仅有基础重传，未实现动态RTT、滑动窗口、拥塞控制或完整主动关闭状态机。
 */

#include "tcp_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_ring.h>

#include "network_interface.h"
#include "config.h"
#include "transport/tcp.h"
#include "socket/event_poll.h"
#include <sys/queue.h>

enum tcp_state {
    TCP_STATE_CLOSED,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_RCVD,
    TCP_STATE_ESTABLISHED,

    /* 已收到对端FIN，本端仍可回显/发送；应用读完已有正文后才看到EOF。 */
    TCP_STATE_CLOSE_WAIT,

    /* 本简化实现已排队本端FIN，等待对方确认后回收。 */
    TCP_STATE_LAST_ACK
};

TAILQ_HEAD(tcp_unack_list, tcp_segment);

struct tcp_socket {
    int fd;

    /* 被动子连接所属监听fd；-1表示不是等待此监听器accept的子连接。 */
    int listener_fd;
    int bound;

    /* Visible through application fd lookup; application-created endpoints start at 1. */
    /* 是否允许应用按fd查找；被动子连接握手后还需accept置1，并非“握手已完成”标志。 */
    int accepted;

    /* 应用已经放弃该fd；TCP状态可能仍需等待FIN确认，故不等同对象已释放。 */
    int application_closed;

    /* 已从可用集合退休；引用归零后才能实际free。 */
    int retired;

    /* 跨条件等待等活跃调用持有的对象引用数，防止close/超时造成悬空指针。 */
    unsigned references;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    /* Queued sequence end, submitted sequence end and oldest unacknowledged byte. */
    /* 已排队范围末尾：下一条首次发送记录预留的序号；入队成功即推进，尚不保证输出。 */
    uint32_t send_next;

    /* 成功提交公共帧TX队列的范围末尾；是ACK合法上界，不是包数或网卡已发出标志。 */
    uint32_t send_sent;

    /* 已确认前缀末尾，即最早未确认序号；部分ACK只推进这里，原记录不裁剪。 */
    uint32_t send_unacked;

    /* 排队+pending+unack中占序号的记录总数，限制总保留量；不是字节数或链表节点数。 */
    unsigned send_buffered;

    /* 本端下一期待接收的对端序号，也是发出ACK时应填写的确认边界。 */
    uint32_t receive_next;

    /* 当前RTO时长，单位是默认计时源ticks；创建时1秒，成功重传后倍增。 */
    uint64_t rto_cycles;

    /* ACK等待的绝对到期时刻；0为停表。名字含tsc，但此RTO路径实际使用默认计时源。 */
    uint64_t retransmit_deadline_tsc;

    /* 已到期的重传若本地输出失败，下次允许再尝试的绝对时刻；不是ACK期限。 */
    uint64_t retransmit_retry_tsc;

    /* 首次输出本地硬失败后的最早重试时刻；与重传的重试等待分开。 */
    uint64_t send_retry_tsc;

    /* 自上次ACK推进以来成功提交的重传次数；首发、ARP等待、本地失败不计入。 */
    unsigned retransmit_count;

    /* 连接连续本地硬失败数；任何成功输出（含纯ACK）清零，不是逐记录计数。 */
    unsigned output_failures;

    /* ACK等待已到期，重传任务尚未成功提交；可能还在等本地重试时刻。 */
    int retransmit_pending;

    /* 连接保存的终止错误；应用接口再把它复制到当前调用线程的errno。 */
    int error_code;
    enum tcp_state state;

    struct rte_ring *receive_queue;
    struct rte_ring *send_queue;

    /* 应用上次未读完的接收记录；下一次recv继续读其剩余正文。 */
    struct tcp_segment *receive_pending;

    /* 协议线程已出队、尚未成功提交的首次发送记录；失败仍在这里保留原对象。 */
    struct tcp_segment *send_pending;

    /* receive_pending中已被应用读走的字节数，不是TCP网络序号。 */
    uint16_t receive_offset;
    pthread_cond_t cond;

    struct tcp_socket *next;

    /* 持有成功提交且未完全确认的原记录，按首次发送序号排列；与poll就绪链表无关。 */
    struct tcp_unack_list unack_list;
};

/* One mutex protects registry, TCP state, queue predicates and object lifetime. */
static struct tcp_socket *socket_list;
static pthread_mutex_t socket_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t socket_ready_events(struct tcp_socket *socket);

/* 释放记录拥有的正文与结构体；摘链必须由调用方先完成，临时借用view不能传入这里。 */
static void free_segment(struct tcp_segment *segment) {
    if (segment != NULL) {
        rte_free(segment->payload);
        rte_free(segment);
    }
}

/* Outstanding sequence space is bounded below half the uint32_t range. */
/*
 * TCP序号会在32位空间回绕，不能直接用普通小于比较。
 * 有符号差值用于判断前后，前提是比较跨度小于2^31；当前未确认记录总量受限。
 */
static int sequence_before(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) < 0;
}

/* 计算占用的序号数量，而不是首部长度：正文每字节一个，SYN/FIN各一个，纯ACK为0。 */
static uint32_t segment_sequence_length(const struct tcp_segment *segment) {
    uint32_t length = segment->payload_length;

    /* SYN虽无正文也占一个序号，否则握手确认边界会少算1。 */
    if ((segment->tcp_flags & RTE_TCP_SYN_FLAG) != 0) {
        length++;
    }

    /* FIN同样占一个序号；纯ACK无SYN/FIN/正文时占用长度为0。 */
    if ((segment->tcp_flags & RTE_TCP_FIN_FLAG) != 0) {
        length++;
    }

    return length;
}

/*
 * 分配记录并复制正文，避免应用缓冲区或输入mbuf释放后正文失效。
 * 这里只填payload及长度；发送的地址/序号等由后续代码补齐。
 */
static struct tcp_segment *create_segment(const void *payload, uint16_t payload_length) {
    struct tcp_segment *segment = rte_zmalloc("tcp_segment", sizeof(*segment), 0);
    if (segment == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    if (payload_length != 0) {
        segment->payload = rte_malloc("tcp_payload", payload_length, 0);
        if (segment->payload == NULL) {
            rte_free(segment);
            errno = ENOMEM;
            return NULL;
        }

        rte_memcpy(segment->payload, payload, payload_length);
    }

    segment->payload_length = payload_length;

    return segment;
}

/* Registry/state helpers are called with socket_list_mutex held. */
/* 持TCP锁查应用可见端点；尚未accept、应用已close或已退休对象不再允许通过fd操作。 */
static struct tcp_socket *find_socket_by_fd(int fd) {
    for (struct tcp_socket *socket = socket_list; socket != NULL; socket = socket->next) {
        if (socket->fd == fd && socket->accepted && !socket->application_closed &&
            !socket->retired) {
            return socket;
        }
    }

    errno = EBADF;
    return NULL;
}

/* 连接终止/销毁时释放三处互斥持有的发送记录，并关闭发送计时；不销毁整个socket。 */
static void clear_send_records(struct tcp_socket *socket) {
    struct tcp_segment *segment;
    while (rte_ring_dequeue(socket->send_queue, (void **)&segment) == 0) {
        free_segment(segment);
    }

    /* 先从链表摘下再释放；始终重新取表头，不用普通FOREACH访问已释放节点的next。 */
    while ((segment = TAILQ_FIRST(&socket->unack_list)) != NULL) {
        TAILQ_REMOVE(&socket->unack_list, segment, unack_link);
        free_segment(segment);
    }

    free_segment(socket->send_pending);
    socket->send_pending = NULL;
    socket->send_buffered = 0;
    socket->retransmit_deadline_tsc = 0;
    socket->retransmit_retry_tsc = 0;
    socket->send_retry_tsc = 0;
    socket->retransmit_pending = 0;
}

/* 从注册表摘除并释放收发记录、ring和条件变量；仅在引用已安全归零时执行。 */
static void destroy_socket(struct tcp_socket *socket) {
    struct tcp_socket **link = &socket_list;
    while (*link != socket) {
        link = &(*link)->next;
    }

    *link = socket->next;

    struct tcp_segment *segment;
    while (rte_ring_dequeue(socket->receive_queue, (void **)&segment) == 0) {
        free_segment(segment);
    }

    clear_send_records(socket);
    free_segment(socket->receive_pending);
    rte_ring_free(socket->receive_queue);
    rte_ring_free(socket->send_queue);
    pthread_cond_destroy(&socket->cond);
    rte_free(socket);
}

/* 撤销应用可见性并唤醒阻塞者；仍有活跃调用引用时推迟实际销毁。 */
static void retire_socket(struct tcp_socket *socket) {
    event_poll_remove(socket->fd);
    socket->retired = 1;
    socket->application_closed = 1;
    pthread_cond_broadcast(&socket->cond);

    /* 退休只禁止新调用；已有阻塞调用醒来前还会访问socket，所以必须等引用归零。 */
    if (socket->references == 0) {
        destroy_socket(socket);
    }
}

/* Keep an accepted application fd alive until close so it can report ETIMEDOUT. */
/*
 * 重试预算耗尽后终止连接，并统一保存ETIMEDOUT。
 * 应用仍持fd时保留对象供recv/send读出错误；不能立即销毁后只剩EBADF。
 */
static void fail_socket(struct tcp_socket *socket) {
    socket->error_code = ETIMEDOUT;
    socket->state = TCP_STATE_CLOSED;
    clear_send_records(socket);

    /* 没有应用持有该fd时无需保留错误壳对象；否则保留到应用读取ETIMEDOUT并close。 */
    if (!socket->accepted || socket->application_closed) {
        retire_socket(socket);
        return;
    }

    pthread_cond_broadcast(&socket->cond);
    event_poll_update(socket->fd, socket_ready_events(socket));
}

/* 持TCP锁释放活跃调用引用；阻塞accept/recv醒来后也必须归还引用。 */
static void release_socket(struct tcp_socket *socket) {
    socket->references--;
    if (socket->retired && socket->references == 0) {
        destroy_socket(socket);
    }
}

/* 在本项目TCP fd区间创建端点及收发ring，初始化每连接RTO状态与未确认链表。 */
static struct tcp_socket *create_socket(void) {
    int fd;
    for (fd = TCP_FIRST_FD; fd < TCP_FIRST_FD + MAX_TCP_SOCKETS; fd++) {
        struct tcp_socket *iter;
        for (iter = socket_list; iter != NULL && iter->fd != fd; iter = iter->next) {
        }

        if (iter == NULL) {
            break;
        }
    }

    if (fd == TCP_FIRST_FD + MAX_TCP_SOCKETS) {
        errno = EMFILE;
        return NULL;
    }

    struct tcp_socket *socket = rte_zmalloc("tcp_socket", sizeof(*socket), 0);
    if (socket == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    int error = pthread_cond_init(&socket->cond, NULL);
    if (error != 0) {
        rte_free(socket);
        errno = error;
        return NULL;
    }

    socket->fd = fd;

    /* hz为默认计时源每秒ticks，乘秒数得到时长；零初始化的deadline表示还没开始等ACK。 */
    socket->rto_cycles = rte_get_timer_hz() * TCP_RTO_INITIAL_SECONDS;
    socket->listener_fd = -1;
    socket->accepted = 1;

    char name[32];
    snprintf(name, sizeof(name), "myProgram_tcp_rx_%d", fd);
    socket->receive_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
    snprintf(name, sizeof(name), "myProgram_tcp_tx_%d", fd);
    socket->send_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
    if (socket->receive_queue == NULL || socket->send_queue == NULL) {
        if (socket->receive_queue != NULL) {
            rte_ring_free(socket->receive_queue);
        }

        if (socket->send_queue != NULL) {
            rte_ring_free(socket->send_queue);
        }

        pthread_cond_destroy(&socket->cond);
        rte_free(socket);
        errno = ENOMEM;
        return NULL;
    }

    socket->next = socket_list;
    socket_list = socket;

    /* TAILQ空表还需要初始化尾指针位置，不能只依赖rte_zmalloc把内存清零。 */
    TAILQ_INIT(&socket->unack_list);

    return socket;
}

/*
 * 创建首次发送记录并排队，入队成功才预留序号，防止连续send使用相同seq。
 * 重传不得走此入口，否则会分配新序号、增加计数并重复保留同一段数据。
 */
static int enqueue_tx_segment(struct tcp_socket *socket, uint8_t flags, const void *payload,
                              uint16_t payload_length) {
    /* Bound queued plus unacknowledged records; reserve one slot for FIN. */
    unsigned limit = RING_SIZE - 1;

    /* 普通正文少占一个记录额度，给FIN留下总量额度；这不保证ring自身永远有空位。 */
    if (payload_length != 0) {
        limit--;
    }

    /* 限制所有尚未释放的占序号记录；若只限制send_queue，出队转unack后仍可无限积压。 */
    if ((payload_length != 0 || (flags & (RTE_TCP_SYN_FLAG | RTE_TCP_FIN_FLAG)) != 0) &&
        socket->send_buffered >= limit) {
        errno = ENOBUFS;
        return -1;
    }

    struct tcp_segment *segment = create_segment(payload, payload_length);
    if (segment == NULL) {
        return -1;
    }

    segment->source_ip = socket->local_ip;
    segment->destination_ip = socket->remote_ip;
    segment->source_port = socket->local_port;
    segment->destination_port = socket->remote_port;

    /* 给首次发送预留连续序号；segment的ack_num则确认对方数据，与将来收到的ACK方向相反。 */
    segment->seq_num = socket->send_next;
    segment->ack_num = socket->receive_next;
    segment->tcp_flags = flags;
    if (rte_ring_enqueue(socket->send_queue, segment) != 0) {
        free_segment(segment);
        errno = ENOBUFS;
        return -1;
    }

    /* Reserve sequence space when queued so consecutive sends never reuse it. */
    uint32_t sequence_length = segment_sequence_length(segment);
    socket->send_next += sequence_length;
    if (sequence_length != 0) {
        socket->send_buffered++;
    }

    return 0;
}

/* 应用创建TCP端点的持锁入口；返回本程序fd，尚未绑定或进入连接状态。 */
int tcp_socket_create(void) {
    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = create_socket();
    int fd;
    if (socket == NULL) {
        fd = -1;
    } else {
        fd = socket->fd;
    }

    pthread_mutex_unlock(&socket_list_mutex);
    return fd;
}

/* 仅允许本机/ANY地址和显式非0端口；端口占用检查与写入状态在同一把锁内完成。 */
int tcp_socket_bind(int socket_fd, const struct sockaddr *address, socklen_t address_length) {
    if (address == NULL || address_length < sizeof(struct sockaddr_in) ||
        address->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }

    const struct sockaddr_in *local_address = (const struct sockaddr_in *)address;

    /* 当前没有自动临时端口分配，端口0不能被当作已经成功绑定。 */
    if (local_address->sin_port == 0) {
        errno = EINVAL;
        return -1;
    }

    if (local_address->sin_addr.s_addr != local_host_ip &&
        local_address->sin_addr.s_addr != htonl(INADDR_ANY)) {
        errno = EADDRNOTAVAIL;
        return -1;
    }

    int error = 0;

    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket == NULL) {
        error = EBADF;
    } else if (socket->bound || socket->state != TCP_STATE_CLOSED) {
        error = EINVAL;
    } else {
        for (struct tcp_socket *iter = socket_list; iter != NULL; iter = iter->next) {
            if (iter != socket && !iter->retired && iter->bound && iter->listener_fd == -1 &&
                iter->local_port == local_address->sin_port) {
                error = EADDRINUSE;
                break;
            }
        }

        if (error == 0) {
            socket->local_ip = local_address->sin_addr.s_addr;
            socket->local_port = local_address->sin_port;
            socket->bound = 1;
        }
    }

    pthread_mutex_unlock(&socket_list_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

/* 把已绑定的CLOSED端点转为LISTEN；当前backlog参数尚未参与容量控制。 */
int tcp_socket_listen(int socket_fd, __attribute__((unused)) int backlog) {
    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    int error;
    if (socket == NULL) {
        error = EBADF;
    } else {
        error = 0;
    }

    if (socket != NULL) {
        if (!socket->bound ||
            (socket->state != TCP_STATE_CLOSED && socket->state != TCP_STATE_LISTEN)) {
            error = EINVAL;
        } else {
            socket->state = TCP_STATE_LISTEN;
        }
    }

    pthread_mutex_unlock(&socket_list_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

/* 查找属于该监听器、已经完成握手但尚未accept的连接；CLOSE_WAIT也可能尚待应用接手。 */
static struct tcp_socket *find_accept_ready(int listener_fd) {
    for (struct tcp_socket *connection = socket_list; connection != NULL;connection = connection->next) {
        if (connection->listener_fd == listener_fd && !connection->accepted &&
            !connection->retired &&
            (connection->state == TCP_STATE_ESTABLISHED ||
             connection->state == TCP_STATE_CLOSE_WAIT)) {
            return connection;
        }
    }

    return NULL;
}

/* 把真实状态映射为可读通知：监听器有待accept连接，或连接有正文/EOF/终止错误。 */
static uint32_t socket_ready_events(struct tcp_socket *socket) {
    if (socket->application_closed || socket->retired) {
        return 0;
    }

    /* 自制poll目前只支持可读，用可读唤醒应用再由recv报告终止错误；不是完整EPOLLERR实现。 */
    if (socket->error_code != 0) {
        return EPOLLIN;
    }

    if (socket->state == TCP_STATE_LISTEN) {
        if (find_accept_ready(socket->fd) != NULL) {
            return EPOLLIN;
        }
    } else if (socket->receive_pending != NULL || !rte_ring_empty(socket->receive_queue) ||
               socket->state == TCP_STATE_CLOSE_WAIT) {
        /* 已有未读正文或对端FIN造成EOF时都应可读；通知描述状态，不能把通知本身当正文。 */
        return EPOLLIN;
    }

    return 0;
}

/*
 * 持TCP锁计算当前就绪并完成poll注册，避免先注册/后查询之间漏掉状态变化。
 * 锁顺序固定为TCP锁再poll锁，poll端不反向调用TCP取锁。
 */
int tcp_socket_poll_control(int epoll_fd, int operation, int socket_fd,
                            const struct epoll_event *event) {
    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket == NULL) {
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    int result = event_poll_control(epoll_fd, operation, socket_fd, event,
                                    socket_ready_events(socket));
    pthread_mutex_unlock(&socket_list_mutex);
    return result;
}

/* 持监听器引用等待握手完成；cond等待释放TCP锁，协议线程才能处理SYN/ACK并唤醒它。 */
int tcp_socket_accept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length) {
    if (client_address != NULL && address_length == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *listener = find_socket_by_fd(socket_fd);
    if (listener == NULL || listener->state != TCP_STATE_LISTEN) {
        if (listener == NULL) {
            errno = EBADF;
        } else {
            errno = EINVAL;
        }

        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    /* accept等待时会释放全局锁，先持引用避免另一个线程关闭监听器后直接销毁它。 */
    listener->references++;

    struct tcp_socket *connection;
    while (!listener->application_closed && (connection = find_accept_ready(socket_fd)) == NULL) {
        /* wait原子地释放锁并睡眠，醒来重新持锁；必须回到while重查，唤醒不保证已有连接。 */
        pthread_cond_wait(&listener->cond, &socket_list_mutex);
    }

    int fd = -1;
    if (listener->application_closed) {
        errno = EBADF;
    } else {
        connection->accepted = 1;
        event_poll_update(listener->fd, socket_ready_events(listener));
        fd = connection->fd;
        if (client_address != NULL) {
            struct sockaddr_in remote_address = {0};
            remote_address.sin_family = AF_INET;
            remote_address.sin_addr.s_addr = connection->remote_ip;
            remote_address.sin_port = connection->remote_port;

            size_t count;
            if (*address_length < sizeof(remote_address)) {
                count = *address_length;
            } else {
                count = sizeof(remote_address);
            }

            rte_memcpy(client_address, &remote_address, count);
            *address_length = sizeof(remote_address);
        }
    }

    release_socket(listener);
    pthread_mutex_unlock(&socket_list_mutex);
    return fd;
}

/*
 * TCP按字节流读取；receive_pending与receive_offset保存半读记录，读完才释放。
 * CLOSE_WAIT要先读完已有正文才返回EOF；终止错误则优先通过errno报告。
 */
ssize_t tcp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags) {
    if ((buffer == NULL && buffer_length != 0) || flags != 0) {
        if (flags != 0) {
            errno = EOPNOTSUPP;
        } else {
            errno = EINVAL;
        }

        return -1;
    }

    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket != NULL && socket->error_code != 0) {
        errno = socket->error_code;
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    if (socket == NULL ||
        (socket->state != TCP_STATE_ESTABLISHED && socket->state != TCP_STATE_CLOSE_WAIT)) {
        if (socket == NULL) {
            errno = EBADF;
        } else {
            errno = ENOTCONN;
        }

        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    if (buffer_length == 0) {
        pthread_mutex_unlock(&socket_list_mutex);
        return 0;
    }

    socket->references++;

    /*
     * 已有半读记录时短路，不再出队；没有记录且仍ESTABLISHED才等待。
     * 若已CLOSE_WAIT且队列为空，就退出等待返回EOF，不能继续等永远不会来的新正文。
     */
    while (!socket->application_closed && socket->receive_pending == NULL &&
           rte_ring_dequeue(socket->receive_queue, (void **)&socket->receive_pending) != 0 &&
           socket->state == TCP_STATE_ESTABLISHED) {
        pthread_cond_wait(&socket->cond, &socket_list_mutex);
    }

    ssize_t copied = 0;
    if (socket->error_code != 0) {
        errno = socket->error_code;
        copied = -1;
    } else if (socket->application_closed) {
        errno = EBADF;
        copied = -1;
    } else if (socket->receive_pending != NULL) {
        struct tcp_segment *segment = socket->receive_pending;
        size_t remaining = segment->payload_length - socket->receive_offset;
        size_t count;
        if (buffer_length < remaining) {
            count = buffer_length;
        } else {
            count = remaining;
        }

        rte_memcpy(buffer, segment->payload + socket->receive_offset, count);
        socket->receive_offset += (uint16_t)count;
        copied = (ssize_t)count;

        /* 只有整条接收记录读完才释放；缓冲区小导致的剩余字节须留到下次TCP recv。 */
        if (socket->receive_offset == segment->payload_length) {
            free_segment(segment);
            socket->receive_pending = NULL;
            socket->receive_offset = 0;
        }
    }

    event_poll_update(socket->fd, socket_ready_events(socket));

    /* CLOSE_WAIT returns EOF only after all bytes, including a partial head, are read. */
    release_socket(socket);
    pthread_mutex_unlock(&socket_list_mutex);
    return copied;
}

/* 把本次可接纳正文复制到发送队列；单次最多TCP_MAX_PAYLOAD_SIZE，应用需处理部分接纳。 */
ssize_t tcp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags) {
    if ((payload == NULL && payload_length != 0) || flags != 0) {
        if (flags != 0) {
            errno = EOPNOTSUPP;
        } else {
            errno = EINVAL;
        }

        return -1;
    }

    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket != NULL && socket->error_code != 0) {
        errno = socket->error_code;
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    if (socket == NULL ||
        (socket->state != TCP_STATE_ESTABLISHED && socket->state != TCP_STATE_CLOSE_WAIT)) {
        if (socket == NULL) {
            errno = EBADF;
        } else {
            errno = ENOTCONN;
        }

        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    uint16_t count;

    /* 单次最多接纳一段正文；调用者应按返回长度继续发送余下字节，不能假设一次send全接纳。 */
    if (payload_length > TCP_MAX_PAYLOAD_SIZE) {
        count = TCP_MAX_PAYLOAD_SIZE;
    } else {
        count = (uint16_t)payload_length;
    }

    int result;
    if (count == 0) {
        result = 0;
    } else {
        result = enqueue_tx_segment(socket, RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG, payload, count);
    }

    pthread_mutex_unlock(&socket_list_mutex);

    /* Only a private copy in send_queue is guaranteed, not delivery or acknowledgement. */
    if (result == 0) {
        return (ssize_t)count;
    } else {
        return -1;
    }
}

/*
 * 监听器关闭时清理尚未accept的子连接；连接关闭时排队FIN并退出应用可见集合。
 * 当前沿用简化LAST_ACK关闭路径，不能据此认为完整主动关闭状态机已经实现。
 */
int tcp_socket_close(int socket_fd) {
    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket == NULL) {
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }

    if (socket->state == TCP_STATE_LISTEN) {
        struct tcp_socket *iter = socket_list;
        while (iter != NULL) {
            struct tcp_socket *next = iter->next;

            /* 监听器关闭只销毁尚未交给应用的子连接；已accept的连接解除归属后继续独立存在。 */
            if (iter->listener_fd == socket_fd) {
                if (!iter->accepted) {
                    retire_socket(iter);
                } else {
                    iter->listener_fd = -1;
                }
            }

            iter = next;
        }

        retire_socket(socket);
    } else if (socket->state == TCP_STATE_CLOSED) {
        retire_socket(socket);
    } else {
        if (enqueue_tx_segment(socket, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
            pthread_mutex_unlock(&socket_list_mutex);
            return -1;
        }

        event_poll_remove(socket->fd);

        /* Same teaching scope as 10: the supported close path is peer FIN -> LAST_ACK. */
        socket->state = TCP_STATE_LAST_ACK;
        socket->application_closed = 1;
        pthread_cond_broadcast(&socket->cond);
    }

    pthread_mutex_unlock(&socket_list_mutex);
    return 0;
}

/* 先按四元组匹配连接，再查监听端口；失败连接保留fd报错，但不再接收网络入包。 */
static struct tcp_socket *find_socket_by_packet(const struct rte_ipv4_hdr *ipv4_header,
                                                const struct rte_tcp_hdr *tcp_header) {
    for (struct tcp_socket *socket = socket_list; socket != NULL; socket = socket->next) {
        if (!socket->retired && socket->error_code == 0 &&
            socket->state != TCP_STATE_LISTEN &&
            socket->local_ip == ipv4_header->dst_addr &&
            socket->remote_ip == ipv4_header->src_addr &&
            socket->local_port == tcp_header->dst_port &&
            socket->remote_port == tcp_header->src_port) {
            return socket;
        }
    }

    for (struct tcp_socket *socket = socket_list; socket != NULL; socket = socket->next) {
        if (!socket->application_closed && socket->state == TCP_STATE_LISTEN &&
            socket->local_port == tcp_header->dst_port &&
            (socket->local_ip == ipv4_header->dst_addr || socket->local_ip == htonl(INADDR_ANY))) {
            return socket;
        }
    }

    return NULL;
}

/* 收到合法初始SYN后创建尚未accept的子连接，选择本端初始序号并排队SYN+ACK。 */
static void handle_listen(struct tcp_socket *listener, const struct rte_ipv4_hdr *ipv4_header,
                          const struct rte_tcp_hdr *tcp_header) {
    if ((tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 ||
        (tcp_header->tcp_flags & (RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG)) != 0) {
        return;
    }

    struct tcp_socket *connection = create_socket();
    if (connection == NULL) {
        return;
    }

    connection->accepted = 0;
    connection->bound = 1;
    connection->listener_fd = listener->fd;
    connection->local_ip = ipv4_header->dst_addr;
    connection->remote_ip = ipv4_header->src_addr;
    connection->local_port = tcp_header->dst_port;
    connection->remote_port = tcp_header->src_port;

    unsigned seed = (unsigned)time(NULL) ^ (unsigned)connection->fd;
    connection->send_next = rand_r(&seed);

    /* 初始三个发送边界一致；排队SYN后只推进send_next，输出成功才推进send_sent。 */
    connection->send_sent = connection->send_next;
    connection->send_unacked = connection->send_next;
    connection->receive_next = ntohl(tcp_header->sent_seq) + 1;
    connection->state = TCP_STATE_SYN_RCVD;
    if (enqueue_tx_segment(connection, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
        retire_socket(connection);
    }
}

/* Caller has checked the ACK flag and acceptable receive sequence. */
/*
 * 调用方已检查ACK标志与接收序号；此处确认的是收到首部的recv_ack，而非旧发送记录的ack_num。
 * 返回0表示不能继续处理当前入包；1表示可继续，包括没有新进展的重复ACK。
 */
static int handle_ack(struct tcp_socket *connection, const struct rte_tcp_hdr *tcp_header) {
    /* 收到首部的ACK是对我方发送数据的确认；转换为主机序后才能和本地序号比较。 */
    uint32_t ack_num = ntohl(tcp_header->recv_ack);

    /* ACK越过实际提交边界，属于异常确认，不能释放尚未输出的数据；发纠正ACK并停止本包处理。 */
    if (sequence_before(connection->send_sent, ack_num)) {
        enqueue_tx_segment(connection, RTE_TCP_ACK_FLAG, NULL, 0);
        return 0;
    }

    /* 旧/重复ACK没有确认新序号，不重启RTO；返回1仍允许调用方接收同包携带的正文。 */
    if (!sequence_before(connection->send_unacked, ack_num)) {
        return 1;
    }

    connection->send_unacked = ack_num;

    struct tcp_segment *segment;
    while ((segment = TAILQ_FIRST(&connection->unack_list)) != NULL) {
        uint32_t sequence_end = segment->seq_num + segment_sequence_length(segment);

        /* ACK尚未覆盖整个队头（也可能只确认前缀），保留原节点；ACK等于末尾则已经完整确认。 */
        if (sequence_before(ack_num, sequence_end)) {
            break;
        }

        TAILQ_REMOVE(&connection->unack_list, segment, unack_link);
        connection->send_buffered--;
        free_segment(segment);
    }

    /* ACK progress cancels any pending retry of the former oldest range. */
    /* 新ACK推进取消旧的超时任务并重新计算无进展次数；重复ACK在前面已经返回，不会执行这里。 */
    connection->retransmit_count = 0;
    connection->retransmit_pending = 0;
    connection->retransmit_retry_tsc = 0;

    /* 全部确认后停表；仍有记录则从当前时刻按现有RTO重计，不能继续重传已确认的旧队头。 */
    if (TAILQ_EMPTY(&connection->unack_list)) {
        connection->retransmit_deadline_tsc = 0;
    } else {
        /* 不因一次ACK把RTO恢复1秒；当前没有RTT估计结果用于收缩退避后的等待时长。 */
        connection->retransmit_deadline_tsc = rte_get_timer_cycles() + connection->rto_cycles;
    }

    /* A partially acknowledged head stays intact; send_unacked tracks its prefix. */
    return 1;
}

/* 处理已建立连接的ACK、按序正文与FIN；ACK先处理，因此无正文的确认包也能释放发送记录。 */
static void handle_established(struct tcp_socket *connection, const struct rte_tcp_hdr *tcp_header,
                               const uint8_t *payload, uint16_t payload_length) {
    /* This example accepts only the next in-order segment; it never buffers gaps. */
    /*
     * 当前只接收恰好期待的下一段，不缓存乱序缺口；不合适的序号或意外SYN都丢弃本包，
     * 回复当前receive_next让对端知道仍期待哪里，而不是错误推进接收边界。
     */
    if (ntohl(tcp_header->sent_seq) != connection->receive_next ||
        (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) != 0) {
        enqueue_tx_segment(connection, RTE_TCP_ACK_FLAG, NULL, 0);
        return;
    }

    /* 没有ACK标志时短路，不读取无意义确认号；有ACK才调用handle_ack，返回0则停止处理本包正文。 */
    if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) == 0 ||
        !handle_ack(connection, tcp_header)) {
        return;
    }

    /* 到这里ACK已处理完；纯ACK可以返回，但FIN无正文仍需推进接收序号与关闭状态。 */
    const int has_fin = (tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0;
    if (payload_length == 0 && !has_fin) {
        return;
    }

    if (payload_length != 0) {
        /* 这是接收正文的私有副本，供应用队列持有；不是用于读取对端ACK的发送记录。 */
        struct tcp_segment *segment = create_segment(payload, payload_length);
        if (segment == NULL) {
            return;
        }

        if (rte_ring_enqueue(connection->receive_queue, segment) != 0) {
            free_segment(segment);
            return;
        }

        connection->receive_next += payload_length;
    }

    if (has_fin) {
        connection->receive_next++;
        connection->state = TCP_STATE_CLOSE_WAIT;
    }

    enqueue_tx_segment(connection, RTE_TCP_ACK_FLAG, NULL, 0);
    pthread_cond_broadcast(&connection->cond);
    event_poll_update(connection->fd, socket_ready_events(connection));
}

/* 协议线程持TCP锁推进状态机；首部和payload仍借用输入mbuf，接收正文需复制后入队。 */
void tcp_socket_input(const struct rte_ipv4_hdr *ipv4_header, const struct rte_tcp_hdr *tcp_header,
                      const uint8_t *payload, uint16_t payload_length) {
    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = find_socket_by_packet(ipv4_header, tcp_header);

    /* 找不到端点无法分派；当前RST处理尚未实现，收到RST直接忽略，不能误认为已完整支持复位。 */
    if (socket == NULL || (tcp_header->tcp_flags & RTE_TCP_RST_FLAG) != 0) {
        pthread_mutex_unlock(&socket_list_mutex);
        return;
    }

    switch (socket->state) {
    case TCP_STATE_LISTEN:
        handle_listen(socket, ipv4_header, tcp_header);
        break;
    case TCP_STATE_SYN_RCVD:
        /* 第三次握手须确认已经真正提交的SYN，并匹配对端下一序号；随后才可accept。 */
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 &&
            ntohl(tcp_header->recv_ack) == socket->send_next &&
            ntohl(tcp_header->recv_ack) == socket->send_sent &&
            ntohl(tcp_header->sent_seq) == socket->receive_next) {
            handle_ack(socket, tcp_header);
            socket->state = TCP_STATE_ESTABLISHED;

            struct tcp_socket *listener = find_socket_by_fd(socket->listener_fd);
            if (listener != NULL) {
                pthread_cond_broadcast(&listener->cond);
                event_poll_update(listener->fd, socket_ready_events(listener));
            }

            handle_established(socket, tcp_header, payload, payload_length);
        }

        break;
    case TCP_STATE_ESTABLISHED:
        handle_established(socket, tcp_header, payload, payload_length);
        break;
    case TCP_STATE_CLOSE_WAIT:
        /* 对端已关闭发送方向，本端仍可能有未确认数据，故继续处理ACK；重复FIN仍需回复确认。 */
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 &&
            ntohl(tcp_header->sent_seq) == socket->receive_next &&
            !handle_ack(socket, tcp_header)) {
            break;
        }

        if ((tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0) {
            enqueue_tx_segment(socket, RTE_TCP_ACK_FLAG, NULL, 0);
        }

        break;
    case TCP_STATE_LAST_ACK:
        /* 只有确认本端FIN且序号合适的ACK才能回收连接，不能仅因收到任意ACK就释放。 */
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 &&
            ntohl(tcp_header->recv_ack) == socket->send_next &&
            ntohl(tcp_header->recv_ack) == socket->send_sent &&
            ntohl(tcp_header->sent_seq) == socket->receive_next) {
            handle_ack(socket, tcp_header);
            retire_socket(socket);
        }

        break;
    case TCP_STATE_CLOSED:
        break;
    }

    pthread_mutex_unlock(&socket_list_mutex);
}

/* A borrowed view: no new sequence allocation, payload allocation or list insertion. */
/*
 * 填充调用方提供的临时view，只描述队头尚未确认的范围。
 * view借用原正文，不分配新正文、不入链、不拥有释放权；原节点保持完整以便最终安全释放。
 */
static void prepare_retransmit(struct tcp_socket *socket, struct tcp_segment *view) {
    const struct tcp_segment *segment = TAILQ_FIRST(&socket->unack_list);

    /*
     * 复制结构体字段到调用方的栈对象；payload只复制地址，未新建正文。
     * 复制来的链表链接不会使用，view绝不插入unack_list，也不能调用free_segment释放它。
     */
    *view = *segment;

    /* skip为该原记录已经确认的前缀序号数；例如seq=1000、ACK=1002时skip=2。 */
    uint32_t skip = socket->send_unacked - segment->seq_num;
    view->seq_num = socket->send_unacked;

    /* 重传时可能又收到了对端新数据，因此更新我方发出的ACK，不沿用首次发送时的旧确认号。 */
    view->ack_num = socket->receive_next;

    /* 前缀包含已确认SYN时先扣除其1个序号，并清掉SYN位；剩余skip才是正文跳过字节数。 */
    if ((view->tcp_flags & RTE_TCP_SYN_FLAG) != 0 && skip != 0) {
        view->tcp_flags &= ~RTE_TCP_SYN_FLAG;
        skip--;
    }

    /* 只移动view的正文指针并缩短长度：abcdef确认ab后，view指向cdef，原节点仍指向abcdef。 */
    if (skip != 0) {
        view->payload += skip;
        view->payload_length -= (uint16_t)skip;
    }
}

/* Returns zero if the connection has failed; caller must not access it again. */
/*
 * 处理tcp_output的非0结果；retry_tsc指向本次应更新的首次发送或重传重试时刻。
 * 返回0表示fail_socket可能已销毁对象，调用方不得再访问它；当前调用点随后直接continue。
 */
static int output_failed(struct tcp_socket *socket, int result, uint64_t now,
                         uint64_t *retry_tsc) {
    /* ARP requests are already paced inside tcp_output; retry promptly once resolved. */
    /* ARP未解析不是已提交后丢ACK，也不算本地硬失败；ARP请求已在tcp_output内节流。 */
    if (result == 1) {
        *retry_tsc = 0;
        return 1;
    }

    /*
     * retry_tsc是字段地址：首次输出传&send_retry_tsc，重传传&retransmit_retry_tsc。
     * 写入“现在+1秒”的绝对时刻；continue本身不产生任何延时或下次调度。
     */
    *retry_tsc = now + rte_get_timer_hz() * TCP_OUTPUT_RETRY_SECONDS;

    /* 达到连续本地硬失败预算就让整个连接失败，不能跳过这段字节继续流，否则破坏TCP连续性。 */
    if (++socket->output_failures >= TCP_OUTPUT_FAILURE_LIMIT) {
        fail_socket(socket);
        return 0;
    }

    return 1;
}

/* Protocol-lcore only; explicit time also permits deterministic in-process checks. */
/*
 * 仅协议lcore调用，内部持TCP锁扫描所有连接，优先处理到期重传再处理首次输出。
 * now是当前绝对计数；check_timers只控制RTO到期比较，显式传时间便于可控时间检查。
 */
static void socket_output_at(struct rte_mempool *mbuf_pool, uint64_t now, int check_timers) {
    pthread_mutex_lock(&socket_list_mutex);

    struct tcp_socket *socket = socket_list;
    while (socket != NULL) {
        /* current是本轮连接；先保存next，因为下面fail_socket可能立即回收未accept/已close的对象。 */
        struct tcp_socket *current = socket;
        socket = socket->next;

        /* 退休、监听或已终止对象不再推进发送；监听器本身没有等待确认的连接数据。 */
        if (current->retired || current->state == TCP_STATE_LISTEN || current->error_code != 0) {
            continue;
        }

        /*
         * 依次判断本轮检查、计时已启用、已到期。必须在空send_queue早退之前检查，
         * 否则没有新send的静默连接永远得不到重传。
         */
        if (check_timers && current->retransmit_deadline_tsc != 0 &&
            now >= current->retransmit_deadline_tsc) {
            /* 第4次重传后仍等最后一轮ACK；只有其期限也到达且没有新进展，才在这里终止。 */
            if (current->retransmit_count >= TCP_RETRANSMIT_LIMIT) {
                fail_socket(current);
                continue;
            }

            current->retransmit_pending = 1;
        }

        /* pending表示超时任务尚未成功输出，并非已经重传过；本地失败可以让任务跨多轮保留。 */
        if (current->retransmit_pending) {
            /* 上次重传的本地操作失败后尚未等满1秒，暂不重试，避免每轮耗尽mbuf或刷满发送队列。 */
            if (now < current->retransmit_retry_tsc) {
                continue;
            }

            /*
             * view是本轮栈上临时描述，不是新分配/入链的发送记录；其正文借用原unack队头。
             * prepare_retransmit填序号、ACK和未确认后缀；随后tcp_output同步复制内容到新的mbuf。
             */
            struct tcp_segment view;
            prepare_retransmit(current, &view);

            int result = tcp_output(mbuf_pool, &view);

            /* tcp_output可能修改view里的ARP节流时刻，写回原记录让下一轮继承，避免临时对象丢掉该状态。 */
            TAILQ_FIRST(&current->unack_list)->next_arp_retry_tsc = view.next_arp_retry_tsc;
            if (result != 0) {
                output_failed(current, result, now, &current->retransmit_retry_tsc);
                continue;
            }

            current->output_failures = 0;
            current->retransmit_pending = 0;
            current->retransmit_retry_tsc = 0;

            /* 只有成功进入公共TX队列才算一次重传；不增加send_next/send_sent，不重复入链或增加记录数。 */
            current->retransmit_count++;

            /* max_rto为60秒换算后的ticks；成功重传后把下一轮ACK等待时长倍增，最多到此上限。 */
            uint64_t max_rto = rte_get_timer_hz() * TCP_RTO_MAX_SECONDS;
            if (current->rto_cycles < max_rto / 2) {
                current->rto_cycles *= 2;
            } else {
                current->rto_cycles = max_rto;
            }

            current->retransmit_deadline_tsc = now + current->rto_cycles;
            continue;
        }

        /* 首次输出本地失败的等待独立保存；先前的重传分支已优先处理，不被此等待挡住。 */
        if (now < current->send_retry_tsc) {
            continue;
        }

        
        /* 已有pending就短路，不重复出队；只有没有pending时才尝试出队，空ring立即返回非0。 */
        if (current->send_pending == NULL &&
            rte_ring_dequeue(current->send_queue, (void **)&current->send_pending) != 0) {
            continue;
        }

        /* result描述本地输出结果：0已入公共帧队列，1等待ARP，-1硬失败；不是对端ACK结果。 */
        int result = tcp_output(mbuf_pool, current->send_pending);
        if (result != 0) {
            output_failed(current, result, now, &current->send_retry_tsc);
            continue;
        }

        current->output_failures = 0;
        current->send_retry_tsc = 0;

        uint32_t sequence_length = segment_sequence_length(current->send_pending);

        /* 正文/SYN/FIN需保留原记录等确认；纯ACK不占序号，不能再等待“对ACK的ACK”。 */
        if (sequence_length != 0) {
            /* 成功提交后才推进边界，例如seq=1000、占6个序号，send_sent赋为1006，不是发送次数加6。 */
            current->send_sent = current->send_pending->seq_num + sequence_length;
            TAILQ_INSERT_TAIL(&current->unack_list, current->send_pending, unack_link);

            /* 仅由停表转为计时时启动；追加新数据不刷新已有期限，否则持续send会无限推迟最老记录重传。 */
            if (current->retransmit_deadline_tsc == 0) {
                current->retransmit_deadline_tsc = now + current->rto_cycles;
            }
        } else {
            free_segment(current->send_pending);
        }

        /* 原对象已转交unack链表或作为纯ACK释放；这里只清临时持有指针，不会清空链表节点内容。 */
        current->send_pending = NULL;
    }

    pthread_mutex_unlock(&socket_list_mutex);
}

/*
 * 协议轮询入口：约每10ms允许一轮RTO到期检查，但每轮仍推进普通发送。
 * 这是读取时钟并比较截止时间，不是注册回调、sleep或创建定时线程。
 */
void tcp_socket_output(struct rte_mempool *mbuf_pool) {
    /* 静态值跨调用保留上次检查时刻；它控制整个扫描的检查频率，每连接deadline另行保存。 */
    static uint64_t last_timer_check_tsc;

    /*
     * 读取默认计时源的绝对ticks，并非秒数；与rte_get_timer_hz配对换算，不注册定时任务。
     * 该计时源可因DPDK配置不同而变化，字段虽沿用_tsc名字，这里不能假定只会使用TSC。
     */
    uint64_t now = rte_get_timer_cycles();

    /* 每秒ticks乘10再除1000，得到10ms的检查间隔；interval是时长，now/deadline是时刻。 */
    uint64_t interval = rte_get_timer_hz() * TCP_TIMER_CHECK_MS / 1000;

    /* 例如deadline为11秒，每10ms检查是否达到11秒；检查一次不会重置期限，也不会立即重发。 */
    int check_timers = now - last_timer_check_tsc >= interval;
    if (check_timers) {
        last_timer_check_tsc = now;
    }

    /* 无论本轮是否检查RTO都推进发送；本地重试也通过各自绝对时刻判断，不使用sleep阻塞协议线程。 */
    socket_output_at(mbuf_pool, now, check_timers);
}
