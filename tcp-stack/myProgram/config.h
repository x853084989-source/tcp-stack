#ifndef MYPROGRAM_CONFIG_H
#define MYPROGRAM_CONFIG_H

/* Retained ENABLE definitions; features are compiled unconditionally. */
#define ENABLE_SEND 1
#define ENABLE_ARP_REQUEST 1
#define ENABLE_ARP_REPLY 1
#define ENABLE_ICMP 1
#define ENABLE_UDP_APP 1
#define ENABLE_TCP_APP 1

#define RING_SIZE 1024
#define MAX_UDP_SOCKETS 64
#define UDP_APP_PORT 8889
#define UDP_MAX_PAYLOAD_SIZE 1472
#define TCP_FIRST_FD (3 + MAX_UDP_SOCKETS)
#define MAX_TCP_SOCKETS 64
#define TCP_APP_PORT 9999
#define TCP_APP_BUFFER_SIZE 1024
#define TCP_MAX_PAYLOAD_SIZE 1460
#define TCP_INITIAL_WINDOW 14600
#define ARP_RETRY_SECONDS 1
#define UDP_SEND_TIMEOUT_SECONDS 5

#define DPDK_PORT_ID 0
#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 32
#define MAKE_IPV4_HOST_ADDR(a, b, c, d) ((a << 24) + (b << 16) + (c << 8) + (d))

/* Existing TSC threshold, now managed by the protocol lcore. */
#define TIMER_RESOLUTION_CYCLES 120000000000ULL

#endif /* MYPROGRAM_CONFIG_H */
