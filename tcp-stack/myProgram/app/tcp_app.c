#include "tcp_app.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "socket/socket_api.h"

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
    printf("TCP application listening on port %u\n", TCP_APP_PORT);

    while (1) {
        struct sockaddr_in client_address = {0};
        socklen_t address_length = sizeof(client_address);
        int connection_fd = naccept(listen_fd, (struct sockaddr *)&client_address, &address_length);
        if (connection_fd < 0) {
            perror("naccept");
            break;
        }
        printf("TCP application accepted %s:%u\n", inet_ntoa(client_address.sin_addr),
               ntohs(client_address.sin_port));

        uint8_t receive_buffer[TCP_APP_BUFFER_SIZE];
        while (1) {
            ssize_t received_length =
                nrecv(connection_fd, receive_buffer, sizeof(receive_buffer), 0);
            if (received_length <= 0) {
                if (received_length < 0) {
                    perror("nrecv");
                }
                break;
            }
            size_t sent_length = 0;
            while (sent_length < (size_t)received_length) {
                ssize_t count = nsend(connection_fd, receive_buffer + sent_length,
                                      (size_t)received_length - sent_length, 0);
                if (count <= 0) {
                    perror("nsend");
                    break;
                }
                sent_length += (size_t)count;
            }
            if (sent_length != (size_t)received_length) {
                break;
            }
        }
        if (nclose(connection_fd) < 0) {
            perror("TCP nclose");
        }
    }
    nclose(listen_fd);
    return -1;
}
