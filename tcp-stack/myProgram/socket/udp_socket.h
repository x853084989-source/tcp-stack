#ifndef MYPROGRAM_UDP_SOCKET_H
#define MYPROGRAM_UDP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <rte_mempool.h>

struct udp_datagram {
    /* Addresses and ports stay in network byte order; payload_length is payload bytes. */
    uint32_t source_ip;
    uint32_t destination_ip;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t payload_length;
    uint8_t *payload;

    uint64_t next_arp_retry_tsc;
    uint64_t send_deadline_tsc;
};

/* Internal UDP branches used by the shared application interface. */
int udp_socket_create(int domain, int type, int protocol);
int udp_socket_bind(int socket_fd, const struct sockaddr *addr, socklen_t address_length);
/* Receives into the caller buffer; a short buffer discards the datagram tail. */
ssize_t udp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags,
                           struct sockaddr *source_address, socklen_t *address_length);
/* Copies payload into send_queue; success does not imply delivery to the peer. */
ssize_t udp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags,
                        const struct sockaddr *destination_address, socklen_t address_length);
int udp_socket_close(int socket_fd);

/* Borrows payload; makes a private copy for the selected application's receive_queue. */
int udp_socket_input(uint32_t source_ip, uint16_t source_port, uint32_t destination_ip,
                     uint16_t destination_port, const uint8_t *payload, uint16_t payload_length);
/* Protocol lcore advances a bounded send budget; ARP misses remain queued. */
void udp_socket_output(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_UDP_SOCKET_H */
