/*
 * UDP端点、数据报队列与并发回收。注册表锁管理查找/绑定/寿命，端点锁管理本端点状态。
 * 活跃操作先取得引用，即使阻塞等待时另一个线程close，也不能立即销毁对象。
 */

#include "udp_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_ring.h>

#include "network_interface.h"
#include "config.h"
#include "transport/udp.h"

struct udp_socket {
    int fd;
    uint32_t local_ip;
    uint16_t local_port;
    int bound;

    /* 关闭标志禁止新查找和继续入队；已有引用操作退出前对象仍需存在。 */
    int closing;

    /* 活跃操作/输出快照持有的引用数，包括cond睡眠期间；归零后才可销毁已关闭对象。 */
    unsigned references;

    /* 协议线程复制正文后入队，应用recvfrom出队消费；每节点对应一个完整数据报。 */
    struct rte_ring *receive_queue;

    /* 应用复制正文后入队，协议线程取出组包；ARP等待时记录可重新入队。 */
    struct rte_ring *send_queue;

    /* 保护该端点状态与队列谓词；注册表锁另行保护端点查找和生命周期。 */
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    struct udp_socket *next;
};

/* Registry protects lookup/lifetime/bind; each socket mutex protects its queues. */
static struct udp_socket *socket_list;
static pthread_mutex_t socket_list_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 释放该UDP记录独立拥有的正文与结构体；入队成功后由队列消费方负责调用。 */
static void free_segment(struct udp_segment *segment) {
    if (segment != NULL) {
        rte_free(segment->payload);
        rte_free(segment);
    }
}

/* 复制应用或RX正文，使记录寿命独立于调用方缓冲区；0字节数据报仍有一个记录。 */
static struct udp_segment *create_segment(const void *payload, size_t payload_length) {
    struct udp_segment *segment = rte_zmalloc("udp_segment", sizeof(*segment), 0);
    if (segment == NULL) {
        return NULL;
    }

    if (payload_length != 0) {
        segment->payload = rte_malloc("udp_payload", payload_length, 0);
        if (segment->payload == NULL) {
            rte_free(segment);
            return NULL;
        }

        rte_memcpy(segment->payload, payload, payload_length);
    }

    segment->payload_length = (uint16_t)payload_length;

    return segment;
}

/* 端点已关闭且无引用时，释放剩余收发数据报、ring、端点锁与条件变量。 */
static void destroy_socket(struct udp_socket *socket) {
    struct udp_segment *segment;
    while (rte_ring_dequeue(socket->receive_queue, (void **)&segment) == 0) {
        free_segment(segment);
    }
    while (rte_ring_dequeue(socket->send_queue, (void **)&segment) == 0) {
        free_segment(segment);
    }

    rte_ring_free(socket->receive_queue);
    rte_ring_free(socket->send_queue);
    pthread_cond_destroy(&socket->cond);
    pthread_mutex_destroy(&socket->mutex);
    rte_free(socket);
}

/* Active calls keep a reference, including while udp_socket_receive is asleep. */
/* 持注册表锁归还活跃引用；close只标记关闭，最后一个引用归还后才实际销毁。 */
static void release_socket(struct udp_socket *socket) {
    int destroy = 0;

    pthread_mutex_lock(&socket_list_mutex);
    socket->references--;

    /* close只设置closing；最后一个活跃调用离开后才摘表销毁，避免阻塞接收者醒来访问已释放对象。 */
    if (socket->closing && socket->references == 0) {
        struct udp_socket **link = &socket_list;
        while (*link != socket) {
            link = &(*link)->next;
        }

        *link = socket->next;
        destroy = 1;
    }

    if (destroy) {
        /* Keep this fd reserved until its uniquely named rings have been freed. */
        destroy_socket(socket);
    }

    pthread_mutex_unlock(&socket_list_mutex);
}

/* 按应用fd查未关闭端点并增加引用，随后才能离开注册表锁安全使用对象。 */
static struct udp_socket *acquire_socket(int fd) {
    pthread_mutex_lock(&socket_list_mutex);

    struct udp_socket *socket;
    for (socket = socket_list; socket != NULL; socket = socket->next) {
        /* 查找成功时就在注册表锁内加引用，不能先解锁再加，否则中间可能被close销毁。 */
        if (socket->fd == fd && !socket->closing) {
            socket->references++;
            break;
        }
    }

    pthread_mutex_unlock(&socket_list_mutex);
    if (socket == NULL) {
        errno = EBADF;
    }

    return socket;
}

/* 协议线程按目的地址和端口查绑定端点并取得引用，支持绑定ANY地址。 */
static struct udp_socket *acquire_bound_socket(uint32_t ip, uint16_t port) {
    pthread_mutex_lock(&socket_list_mutex);

    struct udp_socket *socket;
    for (socket = socket_list; socket != NULL; socket = socket->next) {
        if (!socket->closing && socket->bound && socket->local_port == port &&
            (socket->local_ip == ip || socket->local_ip == htonl(INADDR_ANY))) {
            socket->references++;
            break;
        }
    }

    pthread_mutex_unlock(&socket_list_mutex);
    return socket;
}

/* 创建IPv4 UDP端点、互斥锁与收发ring；资源全就绪后才加入全局注册表。 */
int udp_socket_create(int domain, int type, int protocol) {
    if (domain != AF_INET || type != SOCK_DGRAM || (protocol != 0 && protocol != IPPROTO_UDP)) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    struct udp_socket *socket = rte_zmalloc("udp_socket", sizeof(*socket), 0);
    if (socket == NULL) {
        errno = ENOMEM;
        return -1;
    }

    int error = pthread_mutex_init(&socket->mutex, NULL);
    if (error != 0) {
        rte_free(socket);
        errno = error;
        return -1;
    }

    error = pthread_cond_init(&socket->cond, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&socket->mutex);
        rte_free(socket);
        errno = error;
        return -1;
    }

    pthread_mutex_lock(&socket_list_mutex);
    for (socket->fd = 3; socket->fd < 3 + MAX_UDP_SOCKETS; socket->fd++) {
        struct udp_socket *iter;
        for (iter = socket_list; iter != NULL; iter = iter->next) {
            if (iter->fd == socket->fd) {
                break;
            }
        }

        if (iter == NULL) {
            break;
        }
    }

    if (socket->fd == 3 + MAX_UDP_SOCKETS) {
        error = EMFILE;
    } else {
        char name[32];
        snprintf(name, sizeof(name), "myProgram_udp_rx_%d", socket->fd);
        socket->receive_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
        snprintf(name, sizeof(name), "myProgram_udp_tx_%d", socket->fd);
        socket->send_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
        if (socket->receive_queue == NULL || socket->send_queue == NULL) {
            error = ENOMEM;
        } else {
            socket->next = socket_list;
            socket_list = socket;
        }
    }

    pthread_mutex_unlock(&socket_list_mutex);

    if (error != 0) {
        if (socket->receive_queue != NULL) {
            rte_ring_free(socket->receive_queue);
        }

        if (socket->send_queue != NULL) {
            rte_ring_free(socket->send_queue);
        }

        pthread_cond_destroy(&socket->cond);
        pthread_mutex_destroy(&socket->mutex);
        rte_free(socket);
        errno = error;
        return -1;
    }

    return socket->fd;
}

/* 校验显式端口与本机地址，按注册表锁→端点锁的顺序完成冲突检查和绑定。 */
int udp_socket_bind(int socket_fd, const struct sockaddr *addr, socklen_t address_length) {
    if (addr == NULL || address_length < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }

    const struct sockaddr_in *local_address = (const struct sockaddr_in *)addr;

    /* UDP也没有实现自动选临时端口；先要求应用显式bind非0端口。 */
    if (local_address->sin_port == 0) {
        errno = EINVAL; /* Automatic ephemeral-port assignment is not implemented. */
        return -1;
    }

    if (local_address->sin_addr.s_addr != local_host_ip &&
        local_address->sin_addr.s_addr != htonl(INADDR_ANY)) {
        errno = EADDRNOTAVAIL;
        return -1;
    }

    struct udp_socket *socket = acquire_socket(socket_fd);
    if (socket == NULL) {
        return -1;
    }

    int error = 0;

    pthread_mutex_lock(&socket_list_mutex);
    pthread_mutex_lock(&socket->mutex);
    if (socket->closing || socket->bound) {
        error = EINVAL;
    } else {
        for (struct udp_socket *iter = socket_list; iter != NULL; iter = iter->next) {
            if (iter != socket && !iter->closing && iter->bound &&
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

    pthread_mutex_unlock(&socket->mutex);
    pthread_mutex_unlock(&socket_list_mutex);
    release_socket(socket);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

/*
 * 一次读取一个数据报；缓冲区较小时丢掉该报文剩余部分，不留到下次recvfrom。
 * 等待期间仍持对象引用，cond等待会释放端点锁；MSG_DONTWAIT空队列时返回EAGAIN。
 */
ssize_t udp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags,
                           struct sockaddr *source_address, socklen_t *address_length) {
    if ((buffer == NULL && buffer_length != 0) ||
        (source_address != NULL && address_length == NULL)) {
        errno = EINVAL;
        return -1;
    }

    if ((flags & ~MSG_DONTWAIT) != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }

    struct udp_socket *socket = acquire_socket(socket_fd);
    if (socket == NULL) {
        return -1;
    }

    int error = 0;
    struct udp_segment *segment = NULL;

    pthread_mutex_lock(&socket->mutex);

    /* 仍绑定且未关闭但队列为空时才等待；while处理虚假唤醒，也重新检查并发close。 */
    while (!socket->closing && socket->bound &&
           rte_ring_dequeue(socket->receive_queue, (void **)&segment) != 0) {
        /* 非阻塞调用没有数据就报告EAGAIN，不能在协议队列空时偷偷进入条件等待。 */
        if ((flags & MSG_DONTWAIT) != 0) {
            error = EAGAIN;
            break;
        }

        /* 原子释放端点锁并等待，醒来重新持锁；因此协议线程能取锁入队并发出signal。 */
        error = pthread_cond_wait(&socket->cond, &socket->mutex);
        if (error != 0) {
            break;
        }
    }

    if (socket->closing) {
        error = EBADF;
    } else if (!socket->bound) {
        error = EINVAL;
    }

    pthread_mutex_unlock(&socket->mutex);

    ssize_t copied = -1;
    if (error == 0 && segment != NULL) {
        size_t count;

        /* 只复制应用能容纳的部分；本次数据报随后整体释放，UDP不能像TCP一样把尾部留到下次读。 */
        if (buffer_length < segment->payload_length) {
            count = buffer_length;
        } else {
            count = segment->payload_length;
        }

        if (count != 0) {
            rte_memcpy(buffer, segment->payload, count);
        }

        if (source_address != NULL) {
            struct sockaddr_in source_ipv4_address = {0};
            source_ipv4_address.sin_family = AF_INET;
            source_ipv4_address.sin_addr.s_addr = segment->source_ip;
            source_ipv4_address.sin_port = segment->source_port;

            size_t addr_count;

            /* address_length输入表示调用方地址缓冲区容量，输出改为完整地址结构所需长度，避免越界复制。 */
            if (*address_length < sizeof(source_ipv4_address)) {
                addr_count = *address_length;
            } else {
                addr_count = sizeof(source_ipv4_address);
            }

            rte_memcpy(source_address, &source_ipv4_address, addr_count);
            *address_length = sizeof(source_ipv4_address);
        }

        copied = (ssize_t)count;
    }

    /* UDP preserves segment boundaries: a truncated tail is discarded. */
    free_segment(segment);
    release_socket(socket);
    if (error != 0) {
        errno = error;
    }

    return copied;
}

/*
 * 复制一份数据报、设置来源/目标和本地发送总期限，再交发送队列持有。
 * 返回长度只说明接纳入队，不说明ARP已完成、网卡已发出或对端已收到。
 */
ssize_t udp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags,
                        const struct sockaddr *destination_address, socklen_t address_length) {
    if ((payload == NULL && payload_length != 0) || destination_address == NULL ||
        address_length < sizeof(struct sockaddr_in) || destination_address->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }

    if (flags != 0 || payload_length > UDP_MAX_PAYLOAD_SIZE) {
        if (flags != 0) {
            errno = EOPNOTSUPP;
        } else {
            errno = EMSGSIZE;
        }

        return -1;
    }

    const struct sockaddr_in *destination_ipv4_address =
        (const struct sockaddr_in *)destination_address;
    if (destination_ipv4_address->sin_port == 0) {
        errno = EINVAL;
        return -1;
    }

    struct udp_socket *socket = acquire_socket(socket_fd);
    if (socket == NULL) {
        return -1;
    }

    struct udp_segment *segment = create_segment(payload, payload_length);
    if (segment == NULL) {
        release_socket(socket);
        errno = ENOMEM;
        return -1;
    }

    int error = 0;

    pthread_mutex_lock(&socket->mutex);
    if (socket->closing || !socket->bound) {
        if (socket->closing) {
            error = EBADF;
        } else {
            error = EINVAL;
        }
    } else {
        /* ANY用于匹配接收绑定；实际发包必须使用真实本机IP，不能把0.0.0.0作为这个回显源地址。 */
        if (socket->local_ip == htonl(INADDR_ANY)) {
            segment->source_ip = local_host_ip;
        } else {
            segment->source_ip = socket->local_ip;
        }

        segment->source_port = socket->local_port;
        segment->destination_ip = destination_ipv4_address->sin_addr.s_addr;
        segment->destination_port = destination_ipv4_address->sin_port;

        /*
         * 从应用接纳开始保存本地发送的绝对截止时刻，防止ARP一直无回复导致记录永久占内存。
         * 此旧路径以TSC读数配默认timer频率，依赖当前默认计时源为TSC，不能视为任意配置均可混用。
         */
        segment->send_deadline_tsc = rte_rdtsc() + rte_get_timer_hz() * UDP_SEND_TIMEOUT_SECONDS;
        if (rte_ring_enqueue(socket->send_queue, segment) != 0) {
            error = ENOBUFS;
        }
    }

    pthread_mutex_unlock(&socket->mutex);
    release_socket(socket);
    if (error != 0) {
        free_segment(segment);
        errno = error;
        return -1;
    }

    /* Accepted into send_queue; this does not mean that the peer has received it. */
    return (ssize_t)payload_length;
}

/* 标记closing并唤醒接收者；取得引用的并发操作完成之前，不能直接free端点。 */
int udp_socket_close(int socket_fd) {
    struct udp_socket *socket = acquire_socket(socket_fd);
    if (socket == NULL) {
        return -1;
    }

    int error = 0;

    pthread_mutex_lock(&socket_list_mutex);
    pthread_mutex_lock(&socket->mutex);
    if (socket->closing) {
        error = EBADF;
    } else {
        socket->closing = 1;
        pthread_cond_broadcast(&socket->cond);
    }

    pthread_mutex_unlock(&socket->mutex);
    pthread_mutex_unlock(&socket_list_mutex);
    release_socket(socket);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

/* Match an endpoint, copy payload, enqueue the segment and wake its receiver. */
/* 匹配绑定端点并复制输入正文，成功入接收队列后唤醒应用；原RX帧仍归上层。 */
int udp_socket_input(uint32_t source_ip, uint16_t source_port, uint32_t destination_ip,
                     uint16_t destination_port, const uint8_t *payload, uint16_t payload_length) {
    if (payload_length > UDP_MAX_PAYLOAD_SIZE) {
        return -1;
    }

    struct udp_socket *socket = acquire_bound_socket(destination_ip, destination_port);
    if (socket == NULL) {
        return -1;
    }

    struct udp_segment *segment = create_segment(payload, payload_length);
    if (segment == NULL) {
        release_socket(socket);
        return -1;
    }

    segment->source_ip = source_ip;
    segment->source_port = source_port;
    segment->destination_ip = destination_ip;
    segment->destination_port = destination_port;

    int ret = -1;

    pthread_mutex_lock(&socket->mutex);

    /* 只有入队成功才能把记录交给接收队列并唤醒应用；关闭/队列满时由本函数释放新副本。 */
    if (!socket->closing && rte_ring_enqueue(socket->receive_queue, segment) == 0) {
        pthread_cond_signal(&socket->cond);
        ret = 0;
    }

    pthread_mutex_unlock(&socket->mutex);
    release_socket(socket);
    if (ret != 0) {
        free_segment(segment);
    }

    return ret;
}

/* Process one bounded send budget per endpoint; ARP misses remain queued. */
/*
 * 先取得端点引用快照，再逐端点按有限预算输出；ARP等待的记录重新入队。
 * 预算避免同一条重排队记录在一轮内反复处理，拖住其他协议与连接。
 */
void udp_socket_output(struct rte_mempool *mbuf_pool) {
    /* sockets是本轮栈上的端点指针快照，不是复制socket对象；入快照时加引用，处理结束再释放。 */
    struct udp_socket *sockets[MAX_UDP_SOCKETS];
    unsigned count = 0;

    pthread_mutex_lock(&socket_list_mutex);
    for (struct udp_socket *socket = socket_list; socket != NULL; socket = socket->next) {
        if (!socket->closing && socket->bound) {
            socket->references++;
            sockets[count++] = socket;
        }
    }

    pthread_mutex_unlock(&socket_list_mutex);

    for (unsigned i = 0; i < count; i++) {
        struct udp_socket *socket = sockets[i];

        /* A retry returns to send_queue; never drain it repeatedly in the same pass. */
        /* budget只取本轮起始待发数并限到BURST_SIZE；重排队记录不增加本轮预算，避免忙循环。 */
        unsigned budget = rte_ring_count(socket->send_queue);
        if (budget > BURST_SIZE) {
            budget = BURST_SIZE;
        }

        for (unsigned j = 0; j < budget; j++) {
            struct udp_segment *segment = NULL;

            pthread_mutex_lock(&socket->mutex);

            int ret;
            if (socket->closing) {
                ret = -1;
            } else {
                ret = rte_ring_dequeue(socket->send_queue, (void **)&segment);
            }

            pthread_mutex_unlock(&socket->mutex);
            if (ret != 0) {
                break;
            }

            /* 仅ARP等待需要重排原记录；成功或硬失败都在后面释放，UDP没有等待确认的unack链表。 */
            if (udp_output(mbuf_pool, segment) == 1) {
                pthread_mutex_lock(&socket->mutex);
                if (socket->closing) {
                    ret = -1;
                } else {
                    ret = rte_ring_enqueue(socket->send_queue, segment);
                }

                pthread_mutex_unlock(&socket->mutex);
                if (ret == 0) {
                    continue;
                }
            }

            free_segment(segment);
        }

        release_socket(socket);
    }
}
