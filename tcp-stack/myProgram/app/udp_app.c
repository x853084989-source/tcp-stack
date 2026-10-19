#include "udp_app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>

#include "common/app_state.h"
#include "config.h"
#include "socket/socket_api.h"

int udp_server_entry(__attribute__((unused)) void *arg) {
    int socket_fd = nsocket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("nsocket");
        return -1;
    }
    struct sockaddr_in local_address = {0};
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = local_host_ip;
    local_address.sin_port = htons(UDP_APP_PORT);
    if (nbind(socket_fd, (struct sockaddr *)&local_address, sizeof(local_address)) < 0) {
        perror("nbind");
        nclose(socket_fd);
        return -1;
    }
    printf("UDP application listening on %s:%u\n", inet_ntoa(local_address.sin_addr), UDP_APP_PORT);

    uint8_t receive_buffer[UDP_MAX_PAYLOAD_SIZE];
    while (1) {
        struct sockaddr_in client_address = {0};
        socklen_t address_length = sizeof(client_address);
        ssize_t received_length = nrecvfrom(socket_fd, receive_buffer, sizeof(receive_buffer), 0,
                                            (struct sockaddr *)&client_address, &address_length);
        if (received_length < 0) {
            perror("nrecvfrom");
            break;
        }
        /* Echo the returned byte count, including binary and empty datagrams. */
        if (nsendto(socket_fd, receive_buffer, (size_t)received_length, 0,
                    (struct sockaddr *)&client_address, sizeof(client_address)) < 0) {
            perror("nsendto");
        }
    }
    nclose(socket_fd);
    return -1;
}
