#ifndef MYPROGRAM_CONFIG_H
#define MYPROGRAM_CONFIG_H

/* Feature flags keep their existing values and uses. */
#define ENABLE_SEND 1
#define ENABLE_ARP_REQUEST 1
#define ENABLE_ARP_REPLY 1
#define ENABLE_ICMP 1

#define DPDK_PORT_ID 0
#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 32
#define MAKE_IPV4_HOST_ADDR(a, b, c, d) ((a << 24) + (b << 16) + (c << 8) + (d))

/* Existing TSC threshold for calling rte_timer_manage(). */
#define TIMER_RESOLUTION_CYCLES 120000000000ULL

#endif /* MYPROGRAM_CONFIG_H */
