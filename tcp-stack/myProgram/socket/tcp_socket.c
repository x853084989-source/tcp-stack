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

enum tcp_status {
    TCP_STATUS_CLOSED,
    TCP_STATUS_LISTEN,
    TCP_STATUS_SYN_RCVD,
    TCP_STATUS_ESTABLISHED,
    TCP_STATUS_CLOSE_WAIT,
    TCP_STATUS_LAST_ACK
};

struct tcp_stream {
    int fd;
    int listener_fd;
    int bound;
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
    enum tcp_status status;

    struct rte_ring *receive_queue;
    struct rte_ring *send_queue;
    struct tcp_fragment *receive_pending;
    struct tcp_fragment *send_pending;
    uint16_t receive_offset;
    pthread_cond_t cond;
    struct tcp_stream *next;
};

/* One mutex protects registry, TCP state, queue predicates and object lifetime. */
static struct tcp_stream *stream_list;
static pthread_mutex_t stream_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void free_fragment(struct tcp_fragment *fragment) {
    if (fragment != NULL) {
        rte_free(fragment->payload);
        rte_free(fragment);
    }
}

static struct tcp_fragment *create_fragment(const void *payload, uint16_t payload_length) {
    struct tcp_fragment *fragment = rte_zmalloc("tcp_fragment", sizeof(*fragment), 0);
    if (fragment == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (payload_length != 0) {
        fragment->payload = rte_malloc("tcp_payload", payload_length, 0);
        if (fragment->payload == NULL) {
            rte_free(fragment);
            errno = ENOMEM;
            return NULL;
        }
        rte_memcpy(fragment->payload, payload, payload_length);
    }
    fragment->payload_length = payload_length;
    return fragment;
}

/* All helpers below require stream_list_mutex. */
static struct tcp_stream *find_stream_fd(int fd) {
    for (struct tcp_stream *stream = stream_list; stream != NULL; stream = stream->next) {
        if (stream->fd == fd && stream->accepted && !stream->application_closed &&
            !stream->retired) {
            return stream;
        }
    }
    errno = EBADF;
    return NULL;
}

static void destroy_stream(struct tcp_stream *stream) {
    struct tcp_stream **link = &stream_list;
    while (*link != stream) {
        link = &(*link)->next;
    }
    *link = stream->next;
    struct tcp_fragment *fragment;
    while (rte_ring_dequeue(stream->receive_queue, (void **)&fragment) == 0) {
        free_fragment(fragment);
    }
    while (rte_ring_dequeue(stream->send_queue, (void **)&fragment) == 0) {
        free_fragment(fragment);
    }
    free_fragment(stream->receive_pending);
    free_fragment(stream->send_pending);
    rte_ring_free(stream->receive_queue);
    rte_ring_free(stream->send_queue);
    pthread_cond_destroy(&stream->cond);
    rte_free(stream);
}

static void retire_stream(struct tcp_stream *stream) {
    stream->retired = 1;
    stream->application_closed = 1;
    pthread_cond_broadcast(&stream->cond);
    if (stream->references == 0) {
        destroy_stream(stream);
    }
}

static void release_stream(struct tcp_stream *stream) {
    stream->references--;
    if (stream->retired && stream->references == 0) {
        destroy_stream(stream);
    }
}

static struct tcp_stream *create_stream(void) {
    int fd;
    for (fd = TCP_FIRST_FD; fd < TCP_FIRST_FD + MAX_TCP_STREAMS; fd++) {
        struct tcp_stream *iter;
        for (iter = stream_list; iter != NULL && iter->fd != fd; iter = iter->next) {
        }
        if (iter == NULL) {
            break;
        }
    }
    if (fd == TCP_FIRST_FD + MAX_TCP_STREAMS) {
        errno = EMFILE;
        return NULL;
    }
    struct tcp_stream *stream = rte_zmalloc("tcp_stream", sizeof(*stream), 0);
    if (stream == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    int error = pthread_cond_init(&stream->cond, NULL);
    if (error != 0) {
        rte_free(stream);
        errno = error;
        return NULL;
    }
    stream->fd = fd;
    stream->listener_fd = -1;
    stream->accepted = 1;
    char name[32];
    snprintf(name, sizeof(name), "myProgram_tcp_rx_%d", fd);
    stream->receive_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
    snprintf(name, sizeof(name), "myProgram_tcp_tx_%d", fd);
    stream->send_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(), 0);
    if (stream->receive_queue == NULL || stream->send_queue == NULL) {
        if (stream->receive_queue != NULL) {
            rte_ring_free(stream->receive_queue);
        }
        if (stream->send_queue != NULL) {
            rte_ring_free(stream->send_queue);
        }
        pthread_cond_destroy(&stream->cond);
        rte_free(stream);
        errno = ENOMEM;
        return NULL;
    }
    stream->next = stream_list;
    stream_list = stream;
    return stream;
}

static int enqueue_send(struct tcp_stream *stream, uint8_t flags, const void *payload,
                        uint16_t payload_length) {
    struct tcp_fragment *fragment = create_fragment(payload, payload_length);
    if (fragment == NULL) {
        return -1;
    }
    fragment->source_ip = stream->local_ip;
    fragment->destination_ip = stream->remote_ip;
    fragment->source_port = stream->local_port;
    fragment->destination_port = stream->remote_port;
    fragment->sequence_number = stream->send_next;
    fragment->acknowledgement_number = stream->receive_next;
    fragment->tcp_flags = flags;
    if (rte_ring_enqueue(stream->send_queue, fragment) != 0) {
        free_fragment(fragment);
        errno = ENOBUFS;
        return -1;
    }
    /* Reserve sequence space when queued so consecutive sends never reuse it. */
    stream->send_next += payload_length;
    if ((flags & (RTE_TCP_SYN_FLAG | RTE_TCP_FIN_FLAG)) != 0) {
        stream->send_next++;
    }
    return 0;
}

int tcp_socket_create(void) {
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = create_stream();
    int fd = stream == NULL ? -1 : stream->fd;
    pthread_mutex_unlock(&stream_list_mutex);
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
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = find_stream_fd(socket_fd);
    if (stream == NULL) {
        error = EBADF;
    } else if (stream->bound || stream->status != TCP_STATUS_CLOSED) {
        error = EINVAL;
    } else {
        for (struct tcp_stream *iter = stream_list; iter != NULL; iter = iter->next) {
            if (iter != stream && !iter->retired && iter->bound && iter->listener_fd == -1 &&
                iter->local_port == local_address->sin_port) {
                error = EADDRINUSE;
                break;
            }
        }
        if (error == 0) {
            stream->local_ip = local_address->sin_addr.s_addr;
            stream->local_port = local_address->sin_port;
            stream->bound = 1;
        }
    }
    pthread_mutex_unlock(&stream_list_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int tcp_socket_listen(int socket_fd, __attribute__((unused)) int backlog) {
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = find_stream_fd(socket_fd);
    int error = stream == NULL ? EBADF : 0;
    if (stream != NULL) {
        if (!stream->bound ||
            (stream->status != TCP_STATUS_CLOSED && stream->status != TCP_STATUS_LISTEN)) {
            error = EINVAL;
        } else {
            stream->status = TCP_STATUS_LISTEN;
        }
    }
    pthread_mutex_unlock(&stream_list_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static struct tcp_stream *find_accepted_stream(int listener_fd) {
    for (struct tcp_stream *stream = stream_list; stream != NULL; stream = stream->next) {
        if (stream->listener_fd == listener_fd && !stream->accepted && !stream->retired &&
            (stream->status == TCP_STATUS_ESTABLISHED || stream->status == TCP_STATUS_CLOSE_WAIT)) {
            return stream;
        }
    }
    return NULL;
}

int tcp_socket_accept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length) {
    if (client_address != NULL && address_length == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *listener = find_stream_fd(socket_fd);
    if (listener == NULL || listener->status != TCP_STATUS_LISTEN) {
        errno = listener == NULL ? EBADF : EINVAL;
        pthread_mutex_unlock(&stream_list_mutex);
        return -1;
    }
    listener->references++;
    struct tcp_stream *stream;
    while (!listener->application_closed && (stream = find_accepted_stream(socket_fd)) == NULL) {
        pthread_cond_wait(&listener->cond, &stream_list_mutex);
    }
    int fd = -1;
    if (listener->application_closed) {
        errno = EBADF;
    } else {
        stream->accepted = 1;
        fd = stream->fd;
        if (client_address != NULL) {
            struct sockaddr_in remote_address = {0};
            remote_address.sin_family = AF_INET;
            remote_address.sin_addr.s_addr = stream->remote_ip;
            remote_address.sin_port = stream->remote_port;
            size_t count =
                *address_length < sizeof(remote_address) ? *address_length : sizeof(remote_address);
            rte_memcpy(client_address, &remote_address, count);
            *address_length = sizeof(remote_address);
        }
    }
    release_stream(listener);
    pthread_mutex_unlock(&stream_list_mutex);
    return fd;
}

ssize_t tcp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags) {
    if ((buffer == NULL && buffer_length != 0) || flags != 0) {
        errno = flags != 0 ? EOPNOTSUPP : EINVAL;
        return -1;
    }
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = find_stream_fd(socket_fd);
    if (stream == NULL ||
        (stream->status != TCP_STATUS_ESTABLISHED && stream->status != TCP_STATUS_CLOSE_WAIT)) {
        errno = stream == NULL ? EBADF : ENOTCONN;
        pthread_mutex_unlock(&stream_list_mutex);
        return -1;
    }
    if (buffer_length == 0) {
        pthread_mutex_unlock(&stream_list_mutex);
        return 0;
    }
    stream->references++;
    while (!stream->application_closed && stream->receive_pending == NULL &&
           rte_ring_dequeue(stream->receive_queue, (void **)&stream->receive_pending) != 0 &&
           stream->status == TCP_STATUS_ESTABLISHED) {
        pthread_cond_wait(&stream->cond, &stream_list_mutex);
    }
    ssize_t copied = 0;
    if (stream->application_closed) {
        errno = EBADF;
        copied = -1;
    } else if (stream->receive_pending != NULL) {
        struct tcp_fragment *fragment = stream->receive_pending;
        size_t remaining = fragment->payload_length - stream->receive_offset;
        size_t count = buffer_length < remaining ? buffer_length : remaining;
        rte_memcpy(buffer, fragment->payload + stream->receive_offset, count);
        stream->receive_offset += (uint16_t)count;
        copied = (ssize_t)count;
        if (stream->receive_offset == fragment->payload_length) {
            free_fragment(fragment);
            stream->receive_pending = NULL;
            stream->receive_offset = 0;
        }
    }
    /* CLOSE_WAIT returns EOF only after all bytes, including a partial head, are read. */
    release_stream(stream);
    pthread_mutex_unlock(&stream_list_mutex);
    return copied;
}

ssize_t tcp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags) {
    if ((payload == NULL && payload_length != 0) || flags != 0) {
        errno = flags != 0 ? EOPNOTSUPP : EINVAL;
        return -1;
    }
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = find_stream_fd(socket_fd);
    if (stream == NULL ||
        (stream->status != TCP_STATUS_ESTABLISHED && stream->status != TCP_STATUS_CLOSE_WAIT)) {
        errno = stream == NULL ? EBADF : ENOTCONN;
        pthread_mutex_unlock(&stream_list_mutex);
        return -1;
    }
    uint16_t count =
        payload_length > TCP_MAX_PAYLOAD_SIZE ? TCP_MAX_PAYLOAD_SIZE : (uint16_t)payload_length;
    int result =
        count == 0 ? 0 : enqueue_send(stream, RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG, payload, count);
    pthread_mutex_unlock(&stream_list_mutex);
    /* Only a private copy in send_queue is guaranteed, not delivery or acknowledgement. */
    return result == 0 ? (ssize_t)count : -1;
}

int tcp_socket_close(int socket_fd) {
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = find_stream_fd(socket_fd);
    if (stream == NULL) {
        pthread_mutex_unlock(&stream_list_mutex);
        return -1;
    }
    if (stream->status == TCP_STATUS_LISTEN) {
        struct tcp_stream *iter = stream_list;
        while (iter != NULL) {
            struct tcp_stream *next = iter->next;
            if (iter->listener_fd == socket_fd) {
                if (!iter->accepted) {
                    retire_stream(iter);
                } else {
                    iter->listener_fd = -1;
                }
            }
            iter = next;
        }
        retire_stream(stream);
    } else if (stream->status == TCP_STATUS_CLOSED) {
        retire_stream(stream);
    } else {
        if (enqueue_send(stream, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
            pthread_mutex_unlock(&stream_list_mutex);
            return -1;
        }
        /* Same teaching scope as 10: the supported close path is peer FIN -> LAST_ACK. */
        stream->status = TCP_STATUS_LAST_ACK;
        stream->application_closed = 1;
        pthread_cond_broadcast(&stream->cond);
    }
    pthread_mutex_unlock(&stream_list_mutex);
    return 0;
}

static struct tcp_stream *find_packet_stream(const struct rte_ipv4_hdr *ipv4_header,
                                             const struct rte_tcp_hdr *tcp_header) {
    for (struct tcp_stream *stream = stream_list; stream != NULL; stream = stream->next) {
        if (!stream->retired && stream->status != TCP_STATUS_LISTEN &&
            stream->local_ip == ipv4_header->dst_addr &&
            stream->remote_ip == ipv4_header->src_addr &&
            stream->local_port == tcp_header->dst_port &&
            stream->remote_port == tcp_header->src_port) {
            return stream;
        }
    }
    for (struct tcp_stream *stream = stream_list; stream != NULL; stream = stream->next) {
        if (!stream->application_closed && stream->status == TCP_STATUS_LISTEN &&
            stream->local_port == tcp_header->dst_port &&
            (stream->local_ip == ipv4_header->dst_addr || stream->local_ip == htonl(INADDR_ANY))) {
            return stream;
        }
    }
    return NULL;
}

static void handle_listen(struct tcp_stream *listener, const struct rte_ipv4_hdr *ipv4_header,
                          const struct rte_tcp_hdr *tcp_header) {
    if ((tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 ||
        (tcp_header->tcp_flags & (RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG)) != 0) {
        return;
    }
    struct tcp_stream *stream = create_stream();
    if (stream == NULL) {
        return;
    }
    stream->accepted = 0;
    stream->bound = 1;
    stream->listener_fd = listener->fd;
    stream->local_ip = ipv4_header->dst_addr;
    stream->remote_ip = ipv4_header->src_addr;
    stream->local_port = tcp_header->dst_port;
    stream->remote_port = tcp_header->src_port;
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)stream->fd;
    stream->send_next = rand_r(&seed);
    stream->receive_next = ntohl(tcp_header->sent_seq) + 1;
    stream->status = TCP_STATUS_SYN_RCVD;
    if (enqueue_send(stream, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
        retire_stream(stream);
    }
}

static void handle_established(struct tcp_stream *stream, const struct rte_tcp_hdr *tcp_header,
                               const uint8_t *payload, uint16_t payload_length) {
    const int has_fin = (tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0;
    if (payload_length == 0 && !has_fin) {
        return;
    }
    /* This example accepts only the next in-order segment; it never buffers gaps. */
    if (ntohl(tcp_header->sent_seq) != stream->receive_next ||
        (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) != 0) {
        enqueue_send(stream, RTE_TCP_ACK_FLAG, NULL, 0);
        return;
    }
    if (payload_length != 0) {
        struct tcp_fragment *fragment = create_fragment(payload, payload_length);
        if (fragment == NULL) {
            return;
        }
        if (rte_ring_enqueue(stream->receive_queue, fragment) != 0) {
            free_fragment(fragment);
            return;
        }
        stream->receive_next += payload_length;
    }
    if (has_fin) {
        stream->receive_next++;
        stream->status = TCP_STATUS_CLOSE_WAIT;
    }
    enqueue_send(stream, RTE_TCP_ACK_FLAG, NULL, 0);
    pthread_cond_broadcast(&stream->cond);
}

void tcp_socket_deliver(const struct rte_ipv4_hdr *ipv4_header,
                        const struct rte_tcp_hdr *tcp_header, const uint8_t *payload,
                        uint16_t payload_length) {
    pthread_mutex_lock(&stream_list_mutex);
    struct tcp_stream *stream = find_packet_stream(ipv4_header, tcp_header);
    if (stream == NULL || (tcp_header->tcp_flags & RTE_TCP_RST_FLAG) != 0) {
        pthread_mutex_unlock(&stream_list_mutex);
        return;
    }
    switch (stream->status) {
    case TCP_STATUS_LISTEN:
        handle_listen(stream, ipv4_header, tcp_header);
        break;
    case TCP_STATUS_SYN_RCVD:
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) == 0 &&
            ntohl(tcp_header->recv_ack) == stream->send_next &&
            ntohl(tcp_header->sent_seq) == stream->receive_next) {
            stream->status = TCP_STATUS_ESTABLISHED;
            struct tcp_stream *listener = find_stream_fd(stream->listener_fd);
            if (listener != NULL) {
                pthread_cond_broadcast(&listener->cond);
            }
            handle_established(stream, tcp_header, payload, payload_length);
        }
        break;
    case TCP_STATUS_ESTABLISHED:
        handle_established(stream, tcp_header, payload, payload_length);
        break;
    case TCP_STATUS_CLOSE_WAIT:
        if ((tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0) {
            enqueue_send(stream, RTE_TCP_ACK_FLAG, NULL, 0);
        }
        break;
    case TCP_STATUS_LAST_ACK:
        if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) != 0 &&
            ntohl(tcp_header->recv_ack) == stream->send_next &&
            ntohl(tcp_header->sent_seq) == stream->receive_next) {
            retire_stream(stream);
        }
        break;
    case TCP_STATUS_CLOSED:
        break;
    }
    pthread_mutex_unlock(&stream_list_mutex);
}

void tcp_socket_flush(struct rte_mempool *mbuf_pool) {
    pthread_mutex_lock(&stream_list_mutex);
    for (struct tcp_stream *stream = stream_list; stream != NULL; stream = stream->next) {
        if (stream->retired || stream->status == TCP_STATUS_LISTEN) {
            continue;
        }
        if (stream->send_pending == NULL &&
            rte_ring_dequeue(stream->send_queue, (void **)&stream->send_pending) != 0) {
            continue;
        }
        /* A pending ARP lookup retains the head, preserving per-connection send order. */
        if (tcp_output(mbuf_pool, stream->send_pending) != 1) {
            free_fragment(stream->send_pending);
            stream->send_pending = NULL;
        }
    }
    pthread_mutex_unlock(&stream_list_mutex);
}
