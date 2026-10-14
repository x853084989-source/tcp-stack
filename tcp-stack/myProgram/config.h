#ifndef MYPROGRAM_CONFIG_H
#define MYPROGRAM_CONFIG_H

/* Existing protocol switches; UDP_APP selects the 07 application layer. */
#define ENABLE_SEND 1
#define ENABLE_ARP_REQUEST 1
#define ENABLE_ARP_REPLY 1
#define ENABLE_ICMP 1
#ifndef ENABLE_UDP_APP
#define ENABLE_UDP_APP 1
#endif

#define RING_SIZE 1024
#define MAX_UDP_SOCKETS 64
#define UDP_APP_PORT 8889
#define UDP_MAX_PAYLOAD_SIZE 1472
#define ARP_RETRY_SECONDS 1
#define UDP_SEND_TIMEOUT_SECONDS 5

#define DPDK_PORT_ID 0
#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 32
#define MAKE_IPV4_HOST_ADDR(a, b, c, d) ((a << 24) + (b << 16) + (c << 8) + (d))

/* Existing TSC threshold, now managed by the protocol lcore. */
#define TIMER_RESOLUTION_CYCLES 120000000000ULL

#endif /* MYPROGRAM_CONFIG_H */
