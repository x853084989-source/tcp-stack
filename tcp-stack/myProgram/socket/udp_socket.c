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

#include "common/app_state.h"
#include "config.h"
#include "protocol/udp.h"

struct udp_socket {
    int fd;
    uint32_t local_ip;
    uint16_t local_port;
    int bound;
    int closing;
    unsigned references;

    struct rte_ring *receive_queue;
    struct rte_ring *send_queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct udp_socket *next;
};

/* Registry protects lookup/lifetime/bind; each socket mutex protects its queues. */
static struct udp_socket *socket_list;
static pthread_mutex_t socket_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void free_datagram(struct udp_datagram *datagram) {
    if (datagram != NULL) {
        rte_free(datagram->payload);
        rte_free(datagram);
    }
}

static struct udp_datagram *create_datagram(const void *payload, size_t payload_length) {
    struct udp_datagram *datagram = rte_zmalloc("udp_datagram", sizeof(*datagram), 0);
    if (datagram == NULL) {
        return NULL;
    }
    if (payload_length != 0) {
        datagram->payload = rte_malloc("udp_payload", payload_length, 0);
        if (datagram->payload == NULL) {
            rte_free(datagram);
            return NULL;
        }
        rte_memcpy(datagram->payload, payload, payload_length);
    }
    datagram->payload_length = (uint16_t)payload_length;
    return datagram;
}

static void destroy_socket(struct udp_socket *socket) {
    struct udp_datagram *datagram;
    while (rte_ring_dequeue(socket->receive_queue, (void **)&datagram) == 0) {
        free_datagram(datagram);
    }
    while (rte_ring_dequeue(socket->send_queue, (void **)&datagram) == 0) {
        free_datagram(datagram);
    }
    rte_ring_free(socket->receive_queue);
    rte_ring_free(socket->send_queue);
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
        if (!socket->closing && socket->bound && socket->local_port == port &&
            (socket->local_ip == ip || socket->local_ip == htonl(INADDR_ANY))) {
            socket->references++;
            break;
        }
    }
    pthread_mutex_unlock(&socket_list_mutex);
    return socket;
}

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

int udp_socket_bind(int socket_fd, const struct sockaddr *addr, socklen_t address_length) {
    if (addr == NULL || address_length < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *local_address = (const struct sockaddr_in *)addr;
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

ssize_t nrecvfrom(int socket_fd, void *buffer, size_t buffer_length, int flags,
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
    struct udp_datagram *datagram = NULL;
    pthread_mutex_lock(&socket->mutex);
    while (!socket->closing && socket->bound &&
           rte_ring_dequeue(socket->receive_queue, (void **)&datagram) != 0) {
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
        size_t count =
            buffer_length < datagram->payload_length ? buffer_length : datagram->payload_length;
        if (count != 0) {
            rte_memcpy(buffer, datagram->payload, count);
        }
        if (source_address != NULL) {
            struct sockaddr_in source_ipv4_address = {0};
            source_ipv4_address.sin_family = AF_INET;
            source_ipv4_address.sin_addr.s_addr = datagram->source_ip;
            source_ipv4_address.sin_port = datagram->source_port;
            size_t addr_count = *address_length < sizeof(source_ipv4_address)
                                    ? *address_length
                                    : sizeof(source_ipv4_address);
            rte_memcpy(source_address, &source_ipv4_address, addr_count);
            *address_length = sizeof(source_ipv4_address);
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

ssize_t nsendto(int socket_fd, const void *payload, size_t payload_length, int flags,
                const struct sockaddr *destination_address, socklen_t address_length) {
    if ((payload == NULL && payload_length != 0) || destination_address == NULL ||
        address_length < sizeof(struct sockaddr_in) || destination_address->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    if (flags != 0 || payload_length > UDP_MAX_PAYLOAD_SIZE) {
        errno = flags != 0 ? EOPNOTSUPP : EMSGSIZE;
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
    struct udp_datagram *datagram = create_datagram(payload, payload_length);
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
        datagram->source_ip =
            socket->local_ip == htonl(INADDR_ANY) ? local_host_ip : socket->local_ip;
        datagram->source_port = socket->local_port;
        datagram->destination_ip = destination_ipv4_address->sin_addr.s_addr;
        datagram->destination_port = destination_ipv4_address->sin_port;
        datagram->send_deadline_tsc = rte_rdtsc() + rte_get_timer_hz() * UDP_SEND_TIMEOUT_SECONDS;
        if (rte_ring_enqueue(socket->send_queue, datagram) != 0) {
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
    /* Accepted into send_queue; this does not mean that the peer has received it. */
    return (ssize_t)payload_length;
}

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

int udp_socket_deliver(uint32_t source_ip, uint16_t source_port, uint32_t destination_ip,
                       uint16_t destination_port, const uint8_t *payload, uint16_t payload_length) {
    if (payload_length > UDP_MAX_PAYLOAD_SIZE) {
        return -1;
    }
    struct udp_socket *socket = acquire_bound_socket(destination_ip, destination_port);
    if (socket == NULL) {
        return -1;
    }
    struct udp_datagram *datagram = create_datagram(payload, payload_length);
    if (datagram == NULL) {
        release_socket(socket);
        return -1;
    }
    datagram->source_ip = source_ip;
    datagram->source_port = source_port;
    datagram->destination_ip = destination_ip;
    datagram->destination_port = destination_port;

    int ret = -1;
    pthread_mutex_lock(&socket->mutex);
    if (!socket->closing && rte_ring_enqueue(socket->receive_queue, datagram) == 0) {
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
        /* A retry returns to send_queue; never drain it repeatedly in the same pass. */
        unsigned budget = rte_ring_count(socket->send_queue);
        if (budget > BURST_SIZE) {
            budget = BURST_SIZE;
        }
        for (unsigned j = 0; j < budget; j++) {
            struct udp_datagram *datagram = NULL;
            pthread_mutex_lock(&socket->mutex);
            int ret =
                socket->closing ? -1 : rte_ring_dequeue(socket->send_queue, (void **)&datagram);
            pthread_mutex_unlock(&socket->mutex);
            if (ret != 0) {
                break;
            }
            if (udp_output(mbuf_pool, datagram) == 1) {
                pthread_mutex_lock(&socket->mutex);
                ret = socket->closing ? -1 : rte_ring_enqueue(socket->send_queue, datagram);
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
