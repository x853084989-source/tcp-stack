#include "packet.h"

#include <arpa/inet.h>
#include <stdio.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_timer.h>
#include <rte_icmp.h>

#include "arp.h"
#include "icmp.h"
#include "config.h"
#include "net_arch.h"
#include "udp.h"
#include "udp_socket.h"

void process_packets(struct rte_mempool *mbuf_pool, struct rte_mbuf **mbufs, int num_recvd) {
    for (int i = 0; i < num_recvd; i++) {
        if (!rte_pktmbuf_is_contiguous(mbufs[i]) ||
            rte_pktmbuf_data_len(mbufs[i]) < sizeof(struct rte_ether_hdr)) {
            rte_pktmbuf_free(mbufs[i]);
            continue;
        }
        struct rte_ether_hdr *ehdr = rte_pktmbuf_mtod(mbufs[i], struct rte_ether_hdr *);

        if (ehdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP) &&
            rte_pktmbuf_data_len(mbufs[i]) >=
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr)) {
            struct rte_arp_hdr *ahdr = rte_pktmbuf_mtod_offset(mbufs[i], struct rte_arp_hdr *,
                                                               sizeof(struct rte_ether_hdr));
            process_arp_packet(mbuf_pool, ahdr);
        }

        if (ehdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            if (rte_pktmbuf_data_len(mbufs[i]) <
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            struct rte_ipv4_hdr *iphdr = rte_pktmbuf_mtod_offset(mbufs[i], struct rte_ipv4_hdr *,
                                                                 sizeof(struct rte_ether_hdr));
            struct in_addr addr;
            addr.s_addr = iphdr->src_addr;
            printf("[Received IPv4 message from source IP: %s\n", inet_ntoa(addr));

            if (iphdr->next_proto_id == IPPROTO_UDP) {
                process_udp_packet(mbuf_pool, mbufs[i], ehdr, iphdr);
            }

            if (iphdr->next_proto_id == IPPROTO_ICMP && iphdr->version_ihl == 0x45 &&
                rte_pktmbuf_data_len(mbufs[i]) >= sizeof(struct rte_ether_hdr) +
                                                      sizeof(struct rte_ipv4_hdr) +
                                                      sizeof(struct rte_icmp_hdr)) {
                process_icmp_packet(mbuf_pool, mbufs[i], ehdr, iphdr);
            }
        }

        rte_pktmbuf_free(mbufs[i]);
    }
}

int packet_worker(void *arg) {
    struct rte_mempool *mbuf_pool = arg;
    struct inout_ring *rings = ring_instance();
    uint64_t prev_tsc = 0;

    while (1) {
        struct rte_mbuf *mbufs[BURST_SIZE];
        unsigned count = rte_ring_dequeue_burst(rings->in, (void **)mbufs, BURST_SIZE, NULL);
        process_packets(mbuf_pool, mbufs, (int)count);

#if ENABLE_UDP_APP
        udp_socket_flush(mbuf_pool);
#endif

        uint64_t cur_tsc = rte_rdtsc();
        if (cur_tsc - prev_tsc > TIMER_RESOLUTION_CYCLES) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }
    }
    return 0;
}
