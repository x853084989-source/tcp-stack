#include "tcp_app.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "socket/socket_api.h"

static void close_connection(int epoll_fd, int connection_fd, int *connections) {
    nepoll_ctl(epoll_fd, EPOLL_CTL_DEL, connection_fd, NULL);
    if (nclose(connection_fd) < 0) {
        perror("TCP nclose");
    }
    connections[connection_fd - TCP_FIRST_FD] = 0;
}

int tcp_server_entry(__attribute__((unused)) void *arg) {
    int listen_fd = nsocket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("TCP nsocket");
        return -1;
    }
    struct sockaddr_in local_address = {0};
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(TCP_APP_PORT);
    if (nbind(listen_fd, (struct sockaddr *)&local_address, sizeof(local_address)) < 0 ||
        nlisten(listen_fd, 10) < 0) {
        perror("TCP bind/listen");
        nclose(listen_fd);
        return -1;
    }
    int epoll_fd = nepoll_create(1);
    if (epoll_fd < 0) {
        perror("nepoll_create");
        nclose(listen_fd);
        return -1;
    }
    struct epoll_event event = {.events = EPOLLIN, .data.fd = listen_fd};
    if (nepoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) < 0) {
        perror("nepoll_ctl listener");
        nclose(epoll_fd);
        nclose(listen_fd);
        return -1;
    }
    printf("TCP application listening on port %u with readable events\n", TCP_APP_PORT);
    int connections[MAX_TCP_SOCKETS] = {0};
    struct epoll_event events[TCP_APP_MAX_EVENTS];
    uint8_t receive_buffer[TCP_APP_BUFFER_SIZE];
    while (1) {
        int ready_count = nepoll_wait(epoll_fd, events, TCP_APP_MAX_EVENTS, -1);
        if (ready_count < 0) {
            break;
        }
        for (int i = 0; i < ready_count; i++) {
            int socket_fd = events[i].data.fd;
            if (socket_fd == listen_fd) {
                struct sockaddr_in client_address = {0};
                socklen_t address_length = sizeof(client_address);
                int connection_fd =
                    naccept(listen_fd, (struct sockaddr *)&client_address, &address_length);
                if (connection_fd < 0) {
                    perror("naccept");
                    continue;
                }
                event.events = EPOLLIN;
                event.data.fd = connection_fd;
                if (nepoll_ctl(epoll_fd, EPOLL_CTL_ADD, connection_fd, &event) < 0) {
                    perror("nepoll_ctl connection");
                    nclose(connection_fd);
                    continue;
                }
                connections[connection_fd - TCP_FIRST_FD] = 1;
                printf("TCP application accepted %s:%u\n", inet_ntoa(client_address.sin_addr),
                       ntohs(client_address.sin_port));
                continue;
            }
            /* One reader owns these descriptors; readiness cannot be consumed elsewhere. */
            ssize_t received_length =
                nrecv(socket_fd, receive_buffer, sizeof(receive_buffer), 0);
            if (received_length <= 0) {
                if (received_length < 0) {
                    perror("nrecv");
                }
                close_connection(epoll_fd, socket_fd, connections);
                continue;
            }
            size_t sent_length = 0;
            while (sent_length < (size_t)received_length) {
                ssize_t count = nsend(socket_fd, receive_buffer + sent_length,
                                      (size_t)received_length - sent_length, 0);
                if (count <= 0) {
                    perror("nsend");
                    break;
                }
                sent_length += (size_t)count;
            }
            if (sent_length != (size_t)received_length) {
                close_connection(epoll_fd, socket_fd, connections);
            }
        }
    }
    for (unsigned i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (connections[i]) {
            nclose(TCP_FIRST_FD + (int)i);
        }
    }
    nclose(listen_fd);
    nclose(epoll_fd);
    return -1;
}
