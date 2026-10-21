#ifndef MYPROGRAM_TCP_SOCKET_H
#define MYPROGRAM_TCP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <rte_ip.h>
#include <rte_mempool.h>
#include <rte_tcp.h>

/* Addresses/ports are network order; sequence numbers and lengths are host order. */
struct tcp_segment {
    uint32_t source_ip;
    uint32_t destination_ip;
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence_number;
    uint32_t acknowledgement_number;
    uint8_t tcp_flags;
    uint16_t payload_length;
    uint8_t *payload;
    uint64_t next_arp_retry_tsc;
};

int tcp_socket_create(void);
int tcp_socket_bind(int socket_fd, const struct sockaddr *address, socklen_t address_length);
int tcp_socket_listen(int socket_fd, int backlog);
int tcp_socket_accept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length);
ssize_t tcp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags);
ssize_t tcp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags);
int tcp_socket_close(int socket_fd);

/* Borrows checked input; updates connection state and copies accepted RX payload. */
void tcp_socket_input(const struct rte_ipv4_hdr *ipv4_header, const struct rte_tcp_hdr *tcp_header,
                      const uint8_t *payload, uint16_t payload_length);
/* Protocol lcore advances pending sends; ARP misses retain the head segment. */
void tcp_socket_output(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_TCP_SOCKET_H */
