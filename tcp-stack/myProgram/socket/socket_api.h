#ifndef MYPROGRAM_SOCKET_API_H
#define MYPROGRAM_SOCKET_API_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

/* User-space IPv4 interfaces; descriptors belong to this program, not Linux. */
int nsocket(int domain, int type, int protocol);
int nbind(int socket_fd, const struct sockaddr *address, socklen_t address_length);
int nlisten(int socket_fd, int backlog);
int naccept(int socket_fd, struct sockaddr *client_address, socklen_t *address_length);
ssize_t nrecv(int socket_fd, void *buffer, size_t buffer_length, int flags);
ssize_t nsend(int socket_fd, const void *payload, size_t payload_length, int flags);
ssize_t nrecvfrom(int socket_fd, void *buffer, size_t buffer_length, int flags,
                  struct sockaddr *source_address, socklen_t *address_length);
ssize_t nsendto(int socket_fd, const void *payload, size_t payload_length, int flags,
                const struct sockaddr *destination_address, socklen_t address_length);
int nclose(int socket_fd);

#endif /* MYPROGRAM_SOCKET_API_H */
