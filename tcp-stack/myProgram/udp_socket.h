#ifndef MYPROGRAM_UDP_SOCKET_H
#define MYPROGRAM_UDP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <rte_mempool.h>

struct udp_datagram {
    /* Addresses and ports stay in network byte order; length is payload bytes. */
    uint32_t sip;
    uint32_t dip;
    uint16_t sport;
    uint16_t dport;
    uint16_t length;
    uint8_t *data;

    uint64_t next_arp_tsc;
    uint64_t expire_tsc;
};

/* User-space IPv4 UDP interfaces, not Linux socket system calls. */
int nsocket(int domain, int type, int protocol);
int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr,
                  socklen_t *addrlen);
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
int nclose(int sockfd);

/* Borrows payload; makes a private copy for the selected application's rcvbuf. */
int udp_socket_deliver(uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport,
                       const uint8_t *payload, uint16_t length);
/* Called by the protocol lcore to drain endpoint sndbuf queues. */
void udp_socket_flush(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_UDP_SOCKET_H */
