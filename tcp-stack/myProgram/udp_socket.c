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

#include "app_state.h"
#include "config.h"
#include "udp.h"

struct udp_socket {
    int fd;
    uint32_t localip;
    uint16_t localport;
    int bound;
    int closing;
    unsigned references;

    struct rte_ring *rcvbuf;
    struct rte_ring *sndbuf;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct udp_socket *next;
};

/* Registry protects lookup/lifetime/bind; each socket mutex protects its queues. */
static struct udp_socket *socket_list;
static pthread_mutex_t socket_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void free_datagram(struct udp_datagram *datagram) {
    if (datagram != NULL) {
        rte_free(datagram->data);
        rte_free(datagram);
    }
}

static struct udp_datagram *create_datagram(const void *data, size_t length) {
    struct udp_datagram *datagram = rte_zmalloc("udp_datagram", sizeof(*datagram), 0);
    if (datagram == NULL) {
        return NULL;
    }
    if (length != 0) {
        datagram->data = rte_malloc("udp_payload", length, 0);
        if (datagram->data == NULL) {
            rte_free(datagram);
            return NULL;
        }
        rte_memcpy(datagram->data, data, length);
    }
    datagram->length = (uint16_t)length;
    return datagram;
}

static void destroy_socket(struct udp_socket *socket) {
    struct udp_datagram *datagram;
    while (rte_ring_dequeue(socket->rcvbuf, (void **)&datagram) == 0) {
        free_datagram(datagram);
    }
    while (rte_ring_dequeue(socket->sndbuf, (void **)&datagram) == 0) {
        free_datagram(datagram);
    }
    rte_ring_free(socket->rcvbuf);
    rte_ring_free(socket->sndbuf);
    pthread_cond_destroy(&socket->cond);
    pthread_mutex_destroy(&socket->mutex);
    rte_free(socket);
}

/* Active calls keep a reference, including while nrecvfrom is asleep. */
static void release_socket(struct udp_socket *socket) {
    int destroy = 0;
    pthread_mutex_lock(&socket_list_mutex);
    socket->references--;
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

static struct udp_socket *acquire_socket(int fd) {
    pthread_mutex_lock(&socket_list_mutex);
    struct udp_socket *socket;
    for (socket = socket_list; socket != NULL; socket = socket->next) {
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

static struct udp_socket *acquire_bound_socket(uint32_t ip, uint16_t port) {
    pthread_mutex_lock(&socket_list_mutex);
    struct udp_socket *socket;
    for (socket = socket_list; socket != NULL; socket = socket->next) {
        if (!socket->closing && socket->bound && socket->localport == port &&
            (socket->localip == ip || socket->localip == htonl(INADDR_ANY))) {
            socket->references++;
            break;
        }
    }
    pthread_mutex_unlock(&socket_list_mutex);
    return socket;
}

int nsocket(int domain, int type, int protocol) {
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
        socket->rcvbuf = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
        snprintf(name, sizeof(name), "myProgram_udp_tx_%d", socket->fd);
        socket->sndbuf = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
        if (socket->rcvbuf == NULL || socket->sndbuf == NULL) {
            error = ENOMEM;
        } else {
            socket->next = socket_list;
            socket_list = socket;
        }
    }
    pthread_mutex_unlock(&socket_list_mutex);

    if (error != 0) {
        if (socket->rcvbuf != NULL) {
            rte_ring_free(socket->rcvbuf);
        }
        if (socket->sndbuf != NULL) {
            rte_ring_free(socket->sndbuf);
        }
        pthread_cond_destroy(&socket->cond);
        pthread_mutex_destroy(&socket->mutex);
        rte_free(socket);
        errno = error;
        return -1;
    }
    return socket->fd;
}

int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (addr == NULL || addrlen < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *localaddr = (const struct sockaddr_in *)addr;
    if (localaddr->sin_port == 0) {
        errno = EINVAL; /* Automatic ephemeral-port assignment is not implemented. */
        return -1;
    }
    if (localaddr->sin_addr.s_addr != local_host_ip &&
        localaddr->sin_addr.s_addr != htonl(INADDR_ANY)) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    struct udp_socket *socket = acquire_socket(sockfd);
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
                iter->localport == localaddr->sin_port) {
                error = EADDRINUSE;
                break;
            }
        }
        if (error == 0) {
            socket->localip = localaddr->sin_addr.s_addr;
            socket->localport = localaddr->sin_port;
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

ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr,
                  socklen_t *addrlen) {
    if ((buf == NULL && len != 0) || (src_addr != NULL && addrlen == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if ((flags & ~MSG_DONTWAIT) != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    struct udp_socket *socket = acquire_socket(sockfd);
    if (socket == NULL) {
        return -1;
    }

    int error = 0;
    struct udp_datagram *datagram = NULL;
    pthread_mutex_lock(&socket->mutex);
    while (!socket->closing && socket->bound &&
           rte_ring_dequeue(socket->rcvbuf, (void **)&datagram) != 0) {
        if ((flags & MSG_DONTWAIT) != 0) {
            error = EAGAIN;
            break;
        }
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
    if (error == 0 && datagram != NULL) {
        size_t count = len < datagram->length ? len : datagram->length;
        if (count != 0) {
            rte_memcpy(buf, datagram->data, count);
        }
        if (src_addr != NULL) {
            struct sockaddr_in peer = {0};
            peer.sin_family = AF_INET;
            peer.sin_addr.s_addr = datagram->sip;
            peer.sin_port = datagram->sport;
            size_t addr_count = *addrlen < sizeof(peer) ? *addrlen : sizeof(peer);
            rte_memcpy(src_addr, &peer, addr_count);
            *addrlen = sizeof(peer);
        }
        copied = (ssize_t)count;
    }
    /* UDP preserves datagram boundaries: a truncated tail is discarded. */
    free_datagram(datagram);
    release_socket(socket);
    if (error != 0) {
        errno = error;
    }
    return copied;
}

ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen) {
    if ((buf == NULL && len != 0) || dest_addr == NULL || addrlen < sizeof(struct sockaddr_in) ||
        dest_addr->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    if (flags != 0 || len > UDP_MAX_PAYLOAD_SIZE) {
        errno = flags != 0 ? EOPNOTSUPP : EMSGSIZE;
        return -1;
    }
    const struct sockaddr_in *peer = (const struct sockaddr_in *)dest_addr;
    if (peer->sin_port == 0) {
        errno = EINVAL;
        return -1;
    }
    struct udp_socket *socket = acquire_socket(sockfd);
    if (socket == NULL) {
        return -1;
    }
    struct udp_datagram *datagram = create_datagram(buf, len);
    if (datagram == NULL) {
        release_socket(socket);
        errno = ENOMEM;
        return -1;
    }

    int error = 0;
    pthread_mutex_lock(&socket->mutex);
    if (socket->closing || !socket->bound) {
        error = socket->closing ? EBADF : EINVAL;
    } else {
        datagram->sip = socket->localip == htonl(INADDR_ANY) ? local_host_ip : socket->localip;
        datagram->sport = socket->localport;
        datagram->dip = peer->sin_addr.s_addr;
        datagram->dport = peer->sin_port;
        datagram->expire_tsc = rte_rdtsc() + rte_get_timer_hz() * UDP_SEND_TIMEOUT_SECONDS;
        if (rte_ring_enqueue(socket->sndbuf, datagram) != 0) {
            error = ENOBUFS;
        }
    }
    pthread_mutex_unlock(&socket->mutex);
    release_socket(socket);
    if (error != 0) {
        free_datagram(datagram);
        errno = error;
        return -1;
    }
    /* Accepted into sndbuf; this does not mean that the peer has received it. */
    return (ssize_t)len;
}

int nclose(int sockfd) {
    struct udp_socket *socket = acquire_socket(sockfd);
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

int udp_socket_deliver(uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport,
                       const uint8_t *payload, uint16_t length) {
    if (length > UDP_MAX_PAYLOAD_SIZE) {
        return -1;
    }
    struct udp_socket *socket = acquire_bound_socket(dip, dport);
    if (socket == NULL) {
        return -1;
    }
    struct udp_datagram *datagram = create_datagram(payload, length);
    if (datagram == NULL) {
        release_socket(socket);
        return -1;
    }
    datagram->sip = sip;
    datagram->sport = sport;
    datagram->dip = dip;
    datagram->dport = dport;

    int ret = -1;
    pthread_mutex_lock(&socket->mutex);
    if (!socket->closing && rte_ring_enqueue(socket->rcvbuf, datagram) == 0) {
        pthread_cond_signal(&socket->cond);
        ret = 0;
    }
    pthread_mutex_unlock(&socket->mutex);
    release_socket(socket);
    if (ret != 0) {
        free_datagram(datagram);
    }
    return ret;
}

void udp_socket_flush(struct rte_mempool *mbuf_pool) {
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
        /* A retry returns to sndbuf; never drain it repeatedly in the same pass. */
        unsigned budget = rte_ring_count(socket->sndbuf);
        if (budget > BURST_SIZE) {
            budget = BURST_SIZE;
        }
        for (unsigned j = 0; j < budget; j++) {
            struct udp_datagram *datagram = NULL;
            pthread_mutex_lock(&socket->mutex);
            int ret = socket->closing ? -1 : rte_ring_dequeue(socket->sndbuf, (void **)&datagram);
            pthread_mutex_unlock(&socket->mutex);
            if (ret != 0) {
                break;
            }
            if (udp_output(mbuf_pool, datagram) == 1) {
                pthread_mutex_lock(&socket->mutex);
                ret = socket->closing ? -1 : rte_ring_enqueue(socket->sndbuf, datagram);
                pthread_mutex_unlock(&socket->mutex);
                if (ret == 0) {
                    continue;
                }
            }
            free_datagram(datagram);
        }
        release_socket(socket);
    }
}
