#include "tcp_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_ring.h>

#include "common/app_state.h"
#include "config.h"
#include "protocol/tcp.h"

enum tcp_state {
    TCP_STATE_CLOSED,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_RCVD,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK
};

struct tcp_socket {
    int fd;
    int listener_fd;
    int bound;
    /* Visible through application fd lookup; application-created endpoints start at 1. */
    int accepted;
    int application_closed;
    int retired;
    unsigned references;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t send_next;
    uint32_t receive_next;
    enum tcp_state state;

    struct rte_ring *receive_queue;
    struct rte_ring *send_queue;
    struct tcp_segment *receive_pending;
    struct tcp_segment *send_pending;
    uint16_t receive_offset;
    pthread_cond_t cond;
    struct tcp_socket *next;
};

/* One mutex protects registry, TCP state, queue predicates and object lifetime. */
static struct tcp_socket *socket_list;
static pthread_mutex_t socket_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void free_segment(struct tcp_segment *segment) {
    if (segment != NULL) {
        rte_free(segment->payload);
        rte_free(segment);
    }
}

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
    while (rte_ring_dequeue(socket->send_queue, (void **)&segment) == 0) {
        free_segment(segment);
    }
    free_segment(socket->receive_pending);
    free_segment(socket->send_pending);
    rte_ring_free(socket->receive_queue);
    rte_ring_free(socket->send_queue);
    pthread_cond_destroy(&socket->cond);
    rte_free(socket);
}

static void retire_socket(struct tcp_socket *socket) {
    socket->retired = 1;
    socket->application_closed = 1;
    pthread_cond_broadcast(&socket->cond);
    if (socket->references == 0) {
        destroy_socket(socket);
    }
}

static void release_socket(struct tcp_socket *socket) {
    socket->references--;
    if (socket->retired && socket->references == 0) {
        destroy_socket(socket);
    }
}

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
    return socket;
}

static int enqueue_tx(struct tcp_socket *socket, uint8_t flags, const void *payload,
                      uint16_t payload_length) {
    struct tcp_segment *segment = create_segment(payload, payload_length);
    if (segment == NULL) {
        return -1;
    }
    segment->source_ip = socket->local_ip;
    segment->destination_ip = socket->remote_ip;
    segment->source_port = socket->local_port;
    segment->destination_port = socket->remote_port;
    segment->sequence_number = socket->send_next;
    segment->acknowledgement_number = socket->receive_next;
    segment->tcp_flags = flags;
    if (rte_ring_enqueue(socket->send_queue, segment) != 0) {
        free_segment(segment);
        errno = ENOBUFS;
        return -1;
    }
    /* Reserve sequence space when queued so consecutive sends never reuse it. */
    socket->send_next += payload_length;
    if ((flags & (RTE_TCP_SYN_FLAG | RTE_TCP_FIN_FLAG)) != 0) {
        socket->send_next++;
    }
    return 0;
}

int tcp_socket_create(void) {
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *socket = create_socket();
    int fd = socket == NULL ? -1 : socket->fd;
    pthread_mutex_unlock(&socket_list_mutex);
    return fd;
}

int tcp_socket_bind(int socket_fd, const struct sockaddr *address, socklen_t address_length) {
    if (address == NULL || address_length < sizeof(struct sockaddr_in) ||
        address->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *local_address = (const struct sockaddr_in *)address;
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

int tcp_socket_listen(int socket_fd, __attribute__((unused)) int backlog) {
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    int error = socket == NULL ? EBADF : 0;
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

static struct tcp_socket *find_accept_ready(int listener_fd) {
    for (struct tcp_socket *connection = socket_list; connection != NULL;
         connection = connection->next) {
        if (connection->listener_fd == listener_fd && !connection->accepted &&
            !connection->retired &&
            (connection->state == TCP_STATE_ESTABLISHED ||
             connection->state == TCP_STATE_CLOSE_WAIT)) {
            return connection;
        }
    }
    return NULL;
}

int tcp_socket_accept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length) {
    if (client_address != NULL && address_length == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *listener = find_socket_by_fd(socket_fd);
    if (listener == NULL || listener->state != TCP_STATE_LISTEN) {
        errno = listener == NULL ? EBADF : EINVAL;
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }
    listener->references++;
    struct tcp_socket *connection;
    while (!listener->application_closed && (connection = find_accept_ready(socket_fd)) == NULL) {
        pthread_cond_wait(&listener->cond, &socket_list_mutex);
    }
    int fd = -1;
    if (listener->application_closed) {
        errno = EBADF;
    } else {
        connection->accepted = 1;
        fd = connection->fd;
        if (client_address != NULL) {
            struct sockaddr_in remote_address = {0};
            remote_address.sin_family = AF_INET;
            remote_address.sin_addr.s_addr = connection->remote_ip;
            remote_address.sin_port = connection->remote_port;
            size_t count =
                *address_length < sizeof(remote_address) ? *address_length : sizeof(remote_address);
            rte_memcpy(client_address, &remote_address, count);
            *address_length = sizeof(remote_address);
        }
    }
    release_socket(listener);
    pthread_mutex_unlock(&socket_list_mutex);
    return fd;
}

ssize_t tcp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags) {
    if ((buffer == NULL && buffer_length != 0) || flags != 0) {
        errno = flags != 0 ? EOPNOTSUPP : EINVAL;
        return -1;
    }
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket == NULL ||
        (socket->state != TCP_STATE_ESTABLISHED && socket->state != TCP_STATE_CLOSE_WAIT)) {
        errno = socket == NULL ? EBADF : ENOTCONN;
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }
    if (buffer_length == 0) {
        pthread_mutex_unlock(&socket_list_mutex);
        return 0;
    }
    socket->references++;
    while (!socket->application_closed && socket->receive_pending == NULL &&
           rte_ring_dequeue(socket->receive_queue, (void **)&socket->receive_pending) != 0 &&
           socket->state == TCP_STATE_ESTABLISHED) {
        pthread_cond_wait(&socket->cond, &socket_list_mutex);
    }
    ssize_t copied = 0;
    if (socket->application_closed) {
        errno = EBADF;
        copied = -1;
    } else if (socket->receive_pending != NULL) {
        struct tcp_segment *segment = socket->receive_pending;
        size_t remaining = segment->payload_length - socket->receive_offset;
        size_t count = buffer_length < remaining ? buffer_length : remaining;
        rte_memcpy(buffer, segment->payload + socket->receive_offset, count);
        socket->receive_offset += (uint16_t)count;
        copied = (ssize_t)count;
        if (socket->receive_offset == segment->payload_length) {
            free_segment(segment);
            socket->receive_pending = NULL;
            socket->receive_offset = 0;
        }
    }
    /* CLOSE_WAIT returns EOF only after all bytes, including a partial head, are read. */
    release_socket(socket);
    pthread_mutex_unlock(&socket_list_mutex);
    return copied;
}

ssize_t tcp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags) {
    if ((payload == NULL && payload_length != 0) || flags != 0) {
        errno = flags != 0 ? EOPNOTSUPP : EINVAL;
        return -1;
    }
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *socket = find_socket_by_fd(socket_fd);
    if (socket == NULL ||
        (socket->state != TCP_STATE_ESTABLISHED && socket->state != TCP_STATE_CLOSE_WAIT)) {
        errno = socket == NULL ? EBADF : ENOTCONN;
        pthread_mutex_unlock(&socket_list_mutex);
        return -1;
    }
    uint16_t count =
        payload_length > TCP_MAX_PAYLOAD_SIZE ? TCP_MAX_PAYLOAD_SIZE : (uint16_t)payload_length;
    int result =
        count == 0 ? 0 : enqueue_tx(socket, RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG, payload, count);
    pthread_mutex_unlock(&socket_list_mutex);
    /* Only a private copy in send_queue is guaranteed, not delivery or acknowledgement. */
    return result == 0 ? (ssize_t)count : -1;
}

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
        if (enqueue_tx(socket, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
            pthread_mutex_unlock(&socket_list_mutex);
            return -1;
        }
        /* Same teaching scope as 10: the supported close path is peer FIN -> LAST_ACK. */
        socket->state = TCP_STATE_LAST_ACK;
        socket->application_closed = 1;
        pthread_cond_broadcast(&socket->cond);
    }
    pthread_mutex_unlock(&socket_list_mutex);
    return 0;
}

static struct tcp_socket *find_socket_by_packet(const struct rte_ipv4_hdr *ipv4_header,
                                                const struct rte_tcp_hdr *tcp_header) {
    for (struct tcp_socket *socket = socket_list; socket != NULL; socket = socket->next) {
        if (!socket->retired && socket->state != TCP_STATE_LISTEN &&
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
    connection->receive_next = ntohl(tcp_header->sent_seq) + 1;
    connection->state = TCP_STATE_SYN_RCVD;
    if (enqueue_tx(connection, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
        retire_socket(connection);
    }
}

static void handle_established(struct tcp_socket *connection, const struct rte_tcp_hdr *tcp_header,
                               const uint8_t *payload, uint16_t payload_length) {
    const int has_fin = (tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0;
    if (payload_length == 0 && !has_fin) {
        return;
    }
    /* This example accepts only the next in-order segment; it never buffers gaps. */
    if (ntohl(tcp_header->sent_seq) != connection->receive_next ||
        (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) != 0) {
        enqueue_tx(connection, RTE_TCP_ACK_FLAG, NULL, 0);
        return;
    }
    if (payload_length != 0) {
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
    enqueue_tx(connection, RTE_TCP_ACK_FLAG, NULL, 0);
    pthread_cond_broadcast(&connection->cond);
}

void tcp_socket_input(const struct rte_ipv4_hdr *ipv4_header, const struct rte_tcp_hdr *tcp_header,
                      const uint8_t *payload, uint16_t payload_length) {
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *socket = find_socket_by_packet(ipv4_header, tcp_header);
    if (socket == NULL || (tcp_header->tcp_flags & RTE_TCP_RST_FLAG) != 0) {
        pthread_mutex_unlock(&socket_list_mutex);
        return;
    }
    switch (socket->state) {
    case TCP_STATE_LISTEN:
        handle_listen(socket, ipv4_header, tcp_header);
        break;
    case TCP_STATE_SYN_RCVD:
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 &&
            ntohl(tcp_header->recv_ack) == socket->send_next &&
            ntohl(tcp_header->sent_seq) == socket->receive_next) {
            socket->state = TCP_STATE_ESTABLISHED;
            struct tcp_socket *listener = find_socket_by_fd(socket->listener_fd);
            if (listener != NULL) {
                pthread_cond_broadcast(&listener->cond);
            }
            handle_established(socket, tcp_header, payload, payload_length);
        }
        break;
    case TCP_STATE_ESTABLISHED:
        handle_established(socket, tcp_header, payload, payload_length);
        break;
    case TCP_STATE_CLOSE_WAIT:
        if ((tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0) {
            enqueue_tx(socket, RTE_TCP_ACK_FLAG, NULL, 0);
        }
        break;
    case TCP_STATE_LAST_ACK:
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            ntohl(tcp_header->recv_ack) == socket->send_next &&
            ntohl(tcp_header->sent_seq) == socket->receive_next) {
            retire_socket(socket);
        }
        break;
    case TCP_STATE_CLOSED:
        break;
    }
    pthread_mutex_unlock(&socket_list_mutex);
}

void tcp_socket_output(struct rte_mempool *mbuf_pool) {
    pthread_mutex_lock(&socket_list_mutex);
    for (struct tcp_socket *socket = socket_list; socket != NULL; socket = socket->next) {
        if (socket->retired || socket->state == TCP_STATE_LISTEN) {
            continue;
        }
        if (socket->send_pending == NULL &&
            rte_ring_dequeue(socket->send_queue, (void **)&socket->send_pending) != 0) {
            continue;
        }
        /* A pending ARP lookup retains the head, preserving per-connection send order. */
        if (tcp_output(mbuf_pool, socket->send_pending) != 1) {
            free_segment(socket->send_pending);
            socket->send_pending = NULL;
        }
    }
    pthread_mutex_unlock(&socket_list_mutex);
}
