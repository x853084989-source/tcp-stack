#include "socket_api.h"

#include <errno.h>
#include <netinet/in.h>

#include "config.h"
#include "socket/tcp_socket.h"
#include "socket/udp_socket.h"

int nsocket(int domain, int type, int protocol) {
    if (domain == AF_INET && type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) {
        return udp_socket_create(domain, type, protocol);
    }
    if (domain == AF_INET && type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) {
        return tcp_socket_create();
    }
    errno = EPROTONOSUPPORT;
    return -1;
}

int nbind(int socket_fd, const struct sockaddr *address, socklen_t address_length) {
    if (socket_fd >= TCP_FIRST_FD) {
        return tcp_socket_bind(socket_fd, address, address_length);
    }
    return udp_socket_bind(socket_fd, address, address_length);
}

int nlisten(int socket_fd, int backlog) {
    return tcp_socket_listen(socket_fd, backlog);
}

int naccept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length) {
    return tcp_socket_accept(socket_fd, client_address, address_length);
}

ssize_t nrecv(int socket_fd, void *buffer, size_t buffer_length, int flags) {
    return tcp_socket_receive(socket_fd, buffer, buffer_length, flags);
}

ssize_t nsend(int socket_fd, const void *payload, size_t payload_length, int flags) {
    return tcp_socket_send(socket_fd, payload, payload_length, flags);
}

ssize_t nrecvfrom(int socket_fd, void *buffer, size_t buffer_length, int flags,
                  struct sockaddr *source_address, socklen_t *address_length) {
    return udp_socket_receive(socket_fd, buffer, buffer_length, flags, source_address,
                              address_length);
}

ssize_t nsendto(int socket_fd, const void *payload, size_t payload_length, int flags,
                const struct sockaddr *destination_address, socklen_t address_length) {
    return udp_socket_send(socket_fd, payload, payload_length, flags, destination_address,
                           address_length);
}

int nclose(int socket_fd) {
    if (socket_fd >= TCP_FIRST_FD) {
        return tcp_socket_close(socket_fd);
    }
    return udp_socket_close(socket_fd);
}
