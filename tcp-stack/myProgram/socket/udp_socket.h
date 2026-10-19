#ifndef MYPROGRAM_UDP_SOCKET_H
#define MYPROGRAM_UDP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <rte_mempool.h>

#include "socket/socket_api.h"

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
int udp_socket_close(int socket_fd);

/* Borrows payload; makes a private copy for the selected application's receive_queue. */
int udp_socket_deliver(uint32_t source_ip, uint16_t source_port, uint32_t destination_ip,
                       uint16_t destination_port, const uint8_t *payload, uint16_t payload_length);
/* Called by the protocol lcore to drain endpoint send_queue. */
void udp_socket_flush(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_UDP_SOCKET_H */
