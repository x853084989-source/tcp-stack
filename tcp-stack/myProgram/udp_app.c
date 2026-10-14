#include "udp_app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>

#include "app_state.h"
#include "config.h"
#include "udp_socket.h"

int udp_server_entry(__attribute__((unused)) void *arg) {
    int sockfd = nsocket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("nsocket");
        return -1;
    }
    struct sockaddr_in localaddr = {0};
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = local_host_ip;
    localaddr.sin_port = htons(UDP_APP_PORT);
    if (nbind(sockfd, (struct sockaddr *)&localaddr, sizeof(localaddr)) < 0) {
        perror("nbind");
        nclose(sockfd);
        return -1;
    }
    printf("UDP application listening on %s:%u\n", inet_ntoa(localaddr.sin_addr), UDP_APP_PORT);

    uint8_t buffer[UDP_MAX_PAYLOAD_SIZE];
    while (1) {
        struct sockaddr_in peer = {0};
        socklen_t addrlen = sizeof(peer);
        ssize_t received =
            nrecvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&peer, &addrlen);
        if (received < 0) {
            perror("nrecvfrom");
            break;
        }
        /* Echo the returned byte count, including binary and empty datagrams. */
        if (nsendto(sockfd, buffer, (size_t)received, 0, (struct sockaddr *)&peer, sizeof(peer)) <
            0) {
            perror("nsendto");
        }
    }
    nclose(sockfd);
    return -1;
}
