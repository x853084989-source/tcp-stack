#include "packet.h"

#include <arpa/inet.h>
#include <stdio.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_timer.h>
#include <rte_icmp.h>

#include "protocol/arp.h"
#include "protocol/icmp.h"
#include "config.h"
#include "common/net_arch.h"
#include "protocol/udp.h"
#include "socket/udp_socket.h"
#include "protocol/tcp.h"
#include "socket/tcp_socket.h"

void process_packets(struct rte_mempool *mbuf_pool, struct rte_mbuf **mbufs, int num_recvd) {
    for (int i = 0; i < num_recvd; i++) {
        if (!rte_pktmbuf_is_contiguous(mbufs[i]) ||
            rte_pktmbuf_data_len(mbufs[i]) < sizeof(struct rte_ether_hdr)) {
            rte_pktmbuf_free(mbufs[i]);
            continue;
        }
        struct rte_ether_hdr *ethernet_header = rte_pktmbuf_mtod(mbufs[i], struct rte_ether_hdr *);

        if (ethernet_header->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP) &&
            rte_pktmbuf_data_len(mbufs[i]) >=
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr)) {
            struct rte_arp_hdr *arp_header = rte_pktmbuf_mtod_offset(mbufs[i], struct rte_arp_hdr *,
                                                                     sizeof(struct rte_ether_hdr));
            process_arp_packet(mbuf_pool, arp_header);
        }

        if (ethernet_header->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            if (rte_pktmbuf_data_len(mbufs[i]) <
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            struct rte_ipv4_hdr *ipv4_header = rte_pktmbuf_mtod_offset(
                mbufs[i], struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
            struct in_addr addr;
            addr.s_addr = ipv4_header->src_addr;
            printf("[Received IPv4 message from source IP: %s\n", inet_ntoa(addr));

            if (ipv4_header->next_proto_id == IPPROTO_UDP) {
                udp_input(mbufs[i], ipv4_header);
            }

            if (ipv4_header->next_proto_id == IPPROTO_TCP) {
                tcp_input(mbufs[i], ipv4_header);
            }

            if (ipv4_header->next_proto_id == IPPROTO_ICMP && ipv4_header->version_ihl == 0x45 &&
                rte_pktmbuf_data_len(mbufs[i]) >= sizeof(struct rte_ether_hdr) +
                                                      sizeof(struct rte_ipv4_hdr) +
                                                      sizeof(struct rte_icmp_hdr)) {
                process_icmp_packet(mbuf_pool, mbufs[i], ethernet_header, ipv4_header);
            }
        }

        rte_pktmbuf_free(mbufs[i]);
    }
}

int packet_worker(void *arg) {
    struct rte_mempool *mbuf_pool = arg;
    struct inout_ring *rings = packet_rings_instance();
    uint64_t last_timer_manage_tsc = 0;

    while (1) {
        struct rte_mbuf *mbufs[BURST_SIZE];
        unsigned count = rte_ring_dequeue_burst(rings->in, (void **)mbufs, BURST_SIZE, NULL);
        process_packets(mbuf_pool, mbufs, (int)count);

        udp_socket_output(mbuf_pool);
        tcp_socket_output(mbuf_pool);

        uint64_t current_tsc = rte_rdtsc();
        if (current_tsc - last_timer_manage_tsc > TIMER_RESOLUTION_CYCLES) {
            rte_timer_manage();
            last_timer_manage_tsc = current_tsc;
        }
    }
    return 0;
}
