#include "ethernet.h"

#include <rte_arp.h>
#include <rte_byteorder.h>
#include <rte_memcpy.h>

#include "network_interface.h"
#include "link/arp.h"
#include "link/frame_queue.h"
#include "network/ipv4.h"
#include "dpdk/kni.h"

void ethernet_input(struct rte_mbuf *frame) {
    if (!rte_pktmbuf_is_contiguous(frame) ||
        rte_pktmbuf_data_len(frame) < sizeof(struct rte_ether_hdr)) {
        rte_pktmbuf_free(frame);
        return;
    }
    struct rte_ether_hdr *ethernet_header = rte_pktmbuf_mtod(frame, struct rte_ether_hdr *);
    if (ethernet_header->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP) &&
        rte_pktmbuf_data_len(frame) >= sizeof(*ethernet_header) + sizeof(struct rte_arp_hdr)) {
        struct rte_arp_hdr *arp_header = rte_pktmbuf_mtod_offset(
            frame, struct rte_arp_hdr *, sizeof(*ethernet_header));
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
        ipv4_input(frame, ethernet_header);
        return;
    }
    kni_send(frame);
}

unsigned ethernet_frame_length(unsigned payload_length) {
    return sizeof(struct rte_ether_hdr) + payload_length;
}

uint8_t *ethernet_payload_data(struct rte_mbuf *frame) {
    return rte_pktmbuf_mtod_offset(frame, uint8_t *, sizeof(struct rte_ether_hdr));
}

int ethernet_output(struct rte_mbuf *frame, const uint8_t *destination_mac,
                    uint16_t ether_type) {
    struct rte_ether_hdr *ethernet_header = rte_pktmbuf_mtod(frame, struct rte_ether_hdr *);
    rte_memcpy(ethernet_header->s_addr.addr_bytes, local_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(ethernet_header->d_addr.addr_bytes, destination_mac, RTE_ETHER_ADDR_LEN);
    ethernet_header->ether_type = rte_cpu_to_be_16(ether_type);
    return enqueue_tx_frame(frame);
}
