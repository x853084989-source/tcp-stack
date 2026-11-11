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
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK
};

TAILQ_HEAD(tcp_unack_list, tcp_segment);

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
    /* Queued sequence end, submitted sequence end and oldest unacknowledged byte. */
    uint32_t send_next;
    uint32_t send_sent;
    uint32_t send_unacked;
    unsigned send_buffered;
    uint32_t receive_next;
    uint64_t rto_cycles;
    uint64_t retransmit_deadline_tsc;
    uint64_t retransmit_retry_tsc;
    uint64_t send_retry_tsc;
    unsigned retransmit_count;
    unsigned output_failures;
    int retransmit_pending;
    int error_code;
    enum tcp_state state;

    struct rte_ring *receive_queue;
    struct rte_ring *send_queue;
    struct tcp_segment *receive_pending;
    struct tcp_segment *send_pending;
    uint16_t receive_offset;
    pthread_cond_t cond;
    struct tcp_socket *next;

    struct tcp_unack_list unack_list;
};

/* One mutex protects registry, TCP state, queue predicates and object lifetime. */
static struct tcp_socket *socket_list;
static pthread_mutex_t socket_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t socket_ready_events(struct tcp_socket *socket);

static void free_segment(struct tcp_segment *segment) {
    if (segment != NULL) {
        rte_free(segment->payload);
        rte_free(segment);
    }
}

/* Outstanding sequence space is bounded below half the uint32_t range. */
static int sequence_before(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) < 0;
}

static uint32_t segment_sequence_length(const struct tcp_segment *segment) {
    uint32_t length = segment->payload_length;
    if ((segment->tcp_flags & RTE_TCP_SYN_FLAG) != 0) {
        length++;
    }
    if ((segment->tcp_flags & RTE_TCP_FIN_FLAG) != 0) {
        length++;
    }
    return length;
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

static void clear_send_records(struct tcp_socket *socket) {
    struct tcp_segment *segment;
    while (rte_ring_dequeue(socket->send_queue, (void **)&segment) == 0) {
        free_segment(segment);
    }
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

static void retire_socket(struct tcp_socket *socket) {
    event_poll_remove(socket->fd);
    socket->retired = 1;
    socket->application_closed = 1;
    pthread_cond_broadcast(&socket->cond);
    if (socket->references == 0) {
        destroy_socket(socket);
    }
}

/* Keep an accepted application fd alive until close so it can report ETIMEDOUT. */
static void fail_socket(struct tcp_socket *socket) {
    socket->error_code = ETIMEDOUT;
    socket->state = TCP_STATE_CLOSED;
    clear_send_records(socket);
    if (!socket->accepted || socket->application_closed) {
        retire_socket(socket);
        return;
    }
    pthread_cond_broadcast(&socket->cond);
    event_poll_update(socket->fd, socket_ready_events(socket));
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
    TAILQ_INIT(&socket->unack_list);
    return socket;
}

static int enqueue_tx_segment(struct tcp_socket *socket, uint8_t flags, const void *payload,
                              uint16_t payload_length) {
    /* Bound queued plus unacknowledged records; reserve one slot for FIN. */
    unsigned limit = RING_SIZE - 1;
    if (payload_length != 0) {
        limit--;
    }
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

static uint32_t socket_ready_events(struct tcp_socket *socket) {
    if (socket->application_closed || socket->retired) {
        return 0;
    }
    if (socket->error_code != 0) {
        return EPOLLIN;
    }
    if (socket->state == TCP_STATE_LISTEN) {
        if (find_accept_ready(socket->fd) != NULL) {
            return EPOLLIN;
        }
    } else if (socket->receive_pending != NULL || !rte_ring_empty(socket->receive_queue) ||
               socket->state == TCP_STATE_CLOSE_WAIT) {
        return EPOLLIN;
    }
    return 0;
}

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
    connection->send_sent = connection->send_next;
    connection->send_unacked = connection->send_next;
    connection->receive_next = ntohl(tcp_header->sent_seq) + 1;
    connection->state = TCP_STATE_SYN_RCVD;
    if (enqueue_tx_segment(connection, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, NULL, 0) != 0) {
        retire_socket(connection);
    }
}

/* Caller has checked the ACK flag and acceptable receive sequence. */
static int handle_ack(struct tcp_socket *connection, const struct rte_tcp_hdr *tcp_header) {
    uint32_t ack_num = ntohl(tcp_header->recv_ack);
    if (sequence_before(connection->send_sent, ack_num)) {
        enqueue_tx_segment(connection, RTE_TCP_ACK_FLAG, NULL, 0);
        return 0;
    }
    if (!sequence_before(connection->send_unacked, ack_num)) {
        return 1;
    }

    connection->send_unacked = ack_num;
    struct tcp_segment *segment;
    while ((segment = TAILQ_FIRST(&connection->unack_list)) != NULL) {
        uint32_t sequence_end = segment->seq_num + segment_sequence_length(segment);
        if (sequence_before(ack_num, sequence_end)) {
            break;
        }
        TAILQ_REMOVE(&connection->unack_list, segment, unack_link);
        connection->send_buffered--;
        free_segment(segment);
    }
    /* ACK progress cancels any pending retry of the former oldest range. */
    connection->retransmit_count = 0;
    connection->retransmit_pending = 0;
    connection->retransmit_retry_tsc = 0;
    if (TAILQ_EMPTY(&connection->unack_list)) {
        connection->retransmit_deadline_tsc = 0;
    } else {
        connection->retransmit_deadline_tsc = rte_get_timer_cycles() + connection->rto_cycles;
    }
    /* A partially acknowledged head stays intact; send_unacked tracks its prefix. */
    return 1;
}

static void handle_established(struct tcp_socket *connection, const struct rte_tcp_hdr *tcp_header,
                               const uint8_t *payload, uint16_t payload_length) {
    /* This example accepts only the next in-order segment; it never buffers gaps. */
    if (ntohl(tcp_header->sent_seq) != connection->receive_next ||
        (tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) != 0) {
        enqueue_tx_segment(connection, RTE_TCP_ACK_FLAG, NULL, 0);
        return;
    }
    if ((tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) == 0 ||
        !handle_ack(connection, tcp_header)) {
        return;
    }
    const int has_fin = (tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) != 0;
    if (payload_length == 0 && !has_fin) {
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
    enqueue_tx_segment(connection, RTE_TCP_ACK_FLAG, NULL, 0);
    pthread_cond_broadcast(&connection->cond);
    event_poll_update(connection->fd, socket_ready_events(connection));
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
static void prepare_retransmit(struct tcp_socket *socket, struct tcp_segment *view) {
    const struct tcp_segment *segment = TAILQ_FIRST(&socket->unack_list);
    *view = *segment;
    uint32_t skip = socket->send_unacked - segment->seq_num;
    view->seq_num = socket->send_unacked;
    view->ack_num = socket->receive_next;
    if ((view->tcp_flags & RTE_TCP_SYN_FLAG) != 0 && skip != 0) {
        view->tcp_flags &= ~RTE_TCP_SYN_FLAG;
        skip--;
    }
    if (skip != 0) {
        view->payload += skip;
        view->payload_length -= (uint16_t)skip;
    }
}

/* Returns zero if the connection has failed; caller must not access it again. */
static int output_failed(struct tcp_socket *socket, int result, uint64_t now,
                         uint64_t *retry_tsc) {
    /* ARP requests are already paced inside tcp_output; retry promptly once resolved. */
    if (result == 1) {
        *retry_tsc = 0;
        return 1;
    }
    *retry_tsc = now + rte_get_timer_hz() * TCP_OUTPUT_RETRY_SECONDS;
    if (++socket->output_failures >= TCP_OUTPUT_FAILURE_LIMIT) {
        fail_socket(socket);
        return 0;
    }
    return 1;
}

/* Protocol-lcore only; explicit time also permits deterministic in-process checks. */
static void socket_output_at(struct rte_mempool *mbuf_pool, uint64_t now, int check_timers) {
    pthread_mutex_lock(&socket_list_mutex);
    struct tcp_socket *socket = socket_list;
    while (socket != NULL) {
        struct tcp_socket *current = socket;
        socket = socket->next;
        if (current->retired || current->state == TCP_STATE_LISTEN || current->error_code != 0) {
            continue;
        }
        if (check_timers && current->retransmit_deadline_tsc != 0 &&
            now >= current->retransmit_deadline_tsc) {
            if (current->retransmit_count >= TCP_RETRANSMIT_LIMIT) {
                fail_socket(current);
                continue;
            }
            current->retransmit_pending = 1;
        }
        if (current->retransmit_pending) {
            if (now < current->retransmit_retry_tsc) {
                continue;
            }
            struct tcp_segment view;
            prepare_retransmit(current, &view);
            int result = tcp_output(mbuf_pool, &view);
            TAILQ_FIRST(&current->unack_list)->next_arp_retry_tsc = view.next_arp_retry_tsc;
            if (result != 0) {
                output_failed(current, result, now, &current->retransmit_retry_tsc);
                continue;
            }
            current->output_failures = 0;
            current->retransmit_pending = 0;
            current->retransmit_retry_tsc = 0;
            current->retransmit_count++;
            uint64_t max_rto = rte_get_timer_hz() * TCP_RTO_MAX_SECONDS;
            if (current->rto_cycles < max_rto / 2) {
                current->rto_cycles *= 2;
            } else {
                current->rto_cycles = max_rto;
            }
            current->retransmit_deadline_tsc = now + current->rto_cycles;
            continue;
        }
        if (now < current->send_retry_tsc) {
            continue;
        }
        if (current->send_pending == NULL &&
            rte_ring_dequeue(current->send_queue, (void **)&current->send_pending) != 0) {
            continue;
        }
        int result = tcp_output(mbuf_pool, current->send_pending);
        if (result != 0) {
            output_failed(current, result, now, &current->send_retry_tsc);
            continue;
        }
        current->output_failures = 0;
        current->send_retry_tsc = 0;
        uint32_t sequence_length = segment_sequence_length(current->send_pending);
        if (sequence_length != 0) {
            current->send_sent = current->send_pending->seq_num + sequence_length;
            TAILQ_INSERT_TAIL(&current->unack_list, current->send_pending, unack_link);
            if (current->retransmit_deadline_tsc == 0) {
                current->retransmit_deadline_tsc = now + current->rto_cycles;
            }
        } else {
            free_segment(current->send_pending);
        }
        current->send_pending = NULL;
    }
    pthread_mutex_unlock(&socket_list_mutex);
}

void tcp_socket_output(struct rte_mempool *mbuf_pool) {
    static uint64_t last_timer_check_tsc;
    uint64_t now = rte_get_timer_cycles();
    uint64_t interval = rte_get_timer_hz() * TCP_TIMER_CHECK_MS / 1000;
    int check_timers = now - last_timer_check_tsc >= interval;
    if (check_timers) {
        last_timer_check_tsc = now;
    }
    socket_output_at(mbuf_pool, now, check_timers);
}
