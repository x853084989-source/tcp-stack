#include "packet.h"

#include <arpa/inet.h>
#include <stdio.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_timer.h>

#include "protocol/arp.h"
#include "config.h"
#include "common/kni.h"
#include "common/net_arch.h"
#include "protocol/udp.h"
#include "socket/udp_socket.h"
#include "protocol/tcp.h"
#include "socket/tcp_socket.h"

void process_packets(struct rte_mempool *mbuf_pool, struct rte_mbuf **mbufs, int num_recvd) {
    (void)mbuf_pool;
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
            struct rte_arp_hdr *arp_header = rte_pktmbuf_mtod_offset(
                mbufs[i], struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));
            if (arp_header->arp_hardware == rte_cpu_to_be_16(1) &&
                arp_header->arp_protocol == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) &&
                arp_header->arp_hlen == RTE_ETHER_ADDR_LEN &&
                arp_header->arp_plen == sizeof(uint32_t) &&
                (arp_header->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) ||
                 arp_header->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY))) {
                arp_learn(arp_header->arp_data.arp_sip, arp_header->arp_data.arp_sha.addr_bytes);
            }
        }
        if (ethernet_header->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            if (rte_pktmbuf_data_len(mbufs[i]) <
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            struct rte_ipv4_hdr *ipv4_header = rte_pktmbuf_mtod_offset(
                mbufs[i], struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
            unsigned header_length = (ipv4_header->version_ihl & 0x0f) * 4;
            unsigned packet_length = rte_be_to_cpu_16(ipv4_header->total_length);
            if ((ipv4_header->version_ihl >> 4) != 4 ||
                header_length < sizeof(*ipv4_header) || packet_length < header_length ||
                packet_length > rte_pktmbuf_data_len(mbufs[i]) - sizeof(struct rte_ether_hdr)) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            arp_learn(ipv4_header->src_addr, ethernet_header->s_addr.addr_bytes);
            struct in_addr addr;
            addr.s_addr = ipv4_header->src_addr;
            printf("[Received IPv4 message from source IP: %s\n", inet_ntoa(addr));
            if (ipv4_header->next_proto_id == IPPROTO_UDP) {
                udp_input(mbufs[i], ipv4_header);
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (ipv4_header->next_proto_id == IPPROTO_TCP) {
                tcp_input(mbufs[i], ipv4_header);
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
        }
        /* kni_send consumes this packet on both success and failure. */
        kni_send(mbufs[i]);
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
        kni_receive();
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
