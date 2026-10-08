#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <stdio.h>
#include <arpa/inet.h>

#include "enable.h"
#include "main.h"
#include "arp.h"
#define DPDK_PORT_ID 0

#define NUM_MBUFS (4096-1)
#define BURST_SIZE 32
#define MAKE_IPV4_HOST_ADDR(a, b, c, d) ((a<<24) + (b<<16) + (c<<8) + (d))


uint32_t local_host_ip = MAKE_IPV4_HOST_ADDR(192, 168, 27, 8);
uint32_t src_ip;
uint32_t dst_ip;

uint8_t src_mac[RTE_ETHER_ADDR_LEN];
uint8_t dst_mac[RTE_ETHER_ADDR_LEN];

uint16_t src_port;
uint16_t dst_port;


int main(int argc, char* argv[]){

    int ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Error with EAL init\n");

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("mbuf pool", NUM_MBUFS, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if(mbuf_pool == NULL) rte_exit(EXIT_FAILURE, "Could not create mbuf pool\n");

    port_init(mbuf_pool);

    rte_timer_subsystem_init();

	struct rte_timer arp_timer;
	rte_timer_init(&arp_timer);

	uint64_t hz = rte_get_timer_hz();
	unsigned lcore_id = rte_lcore_id();
	rte_timer_reset(&arp_timer, hz, PERIODICAL, lcore_id, arp_request_timer_cb, mbuf_pool);

    rte_eth_macaddr_get(DPDK_PORT_ID, (struct rte_ether_addr *)src_mac);

    while(1){
        struct rte_mbuf* mbufs[BURST_SIZE];
        int num_recvd = rte_eth_rx_burst(DPDK_PORT_ID, 0, mbufs, BURST_SIZE);
        if(num_recvd > BURST_SIZE) rte_exit(EXIT_FAILURE, "Error receiving from eth\n");

        for(int i = 0; i < num_recvd; i++){

            struct rte_ether_hdr* ehdr = rte_pktmbuf_mtod(mbufs[i], struct rte_ether_hdr*);

            if (ehdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)){
                struct rte_arp_hdr* ahdr = rte_pktmbuf_mtod_offset(mbufs[i], struct rte_arp_hdr*, sizeof(struct rte_ether_hdr));

                if(ahdr ->arp_data.arp_tip == local_host_ip){
                struct in_addr addr;
                addr.s_addr = ahdr->arp_data.arp_tip;
                printf("Received ARP message to local IP: %s\n", inet_ntoa(addr));

                if(ahdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)){
                    addr.s_addr = ahdr->arp_data.arp_sip;
                    printf("Received ARP request\n");
                    printf("ARP request from IP: %s\n", inet_ntoa(addr));
                    // ARP reply
                    // check table
                    
                    struct rte_mbuf* arp_reply_buf = 
                    send_arp(mbuf_pool, RTE_ARP_OP_REPLY, ahdr->arp_data.arp_sha.addr_bytes, ahdr->arp_data.arp_tip, ahdr->arp_data.arp_sip);

                    ret = rte_eth_tx_burst(DPDK_PORT_ID, 0, &arp_reply_buf, 1);

                    if(ret != 0){
                        printf("ARP reply sent to IP: %s\n", inet_ntoa(addr));
                        rte_pktmbuf_free(arp_reply_buf);
                    } else {
                        printf("Failed to send ARP reply\n");
                    }

                }else if (ahdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY)){
                        addr.s_addr = ahdr->arp_data.arp_sip;
                        printf("Received ARP reply\n");
                        printf("ARP reply from IP: %s\n", inet_ntoa(addr));
                        // save in ARP table
                        struct arp_table* table = arp_table_instance();
                        uint8_t* mac_addr = get_dst_macaddr(ahdr->arp_data.arp_sip);

                        if(mac_addr == NULL){
                            struct arp_table_item* iter = rte_malloc("arp_table_item", sizeof(struct arp_table_item), 0);

                            if (iter == NULL) {
                                rte_exit(EXIT_FAILURE, "rte_malloc arp_table_item failed\n");
                            }

                            iter->ip = ahdr->arp_data.arp_sip;
                            rte_memcpy(iter->mac_addr, ahdr->arp_data.arp_sha.addr_bytes, RTE_ETHER_ADDR_LEN);
                            LL_ADD(iter, table->top_table);

                            // check if the iter is added to the table
                            for (iter = table->top_table; iter != NULL; iter = iter->next) {
                                struct in_addr addr;
                                addr.s_addr = iter->ip;

                                if(iter->ip == ahdr->arp_data.arp_sip){
                                    printf("ARP table have been added, IP:%s, MAC: %s\n", inet_ntoa(addr), ether_addr_to_str((struct rte_ether_addr *)iter->mac_addr));
                                    break;
                                }
                            }   
                            

                        }
                    }
            }
                


            
            if (ehdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)){
                struct rte_ipv4_hdr* iphdr = rte_pktmbuf_mtod_offset(mbufs[i], struct rte_ipv4_hdr*, sizeof(struct rte_ether_hdr));
                struct in_addr addr;
                addr.s_addr = iphdr->src_addr;
                printf("Received IPv4 message from source IP: %s\n", inet_ntoa(addr));


                if(iphdr->next_proto_id == IPPROTO_UDP){
                    struct rte_udp_hdr* udphdr = 
                    rte_pktmbuf_mtod_offset(mbufs[i], struct rte_udp_hdr*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

                    rte_memcpy(&dst_mac, ehdr->s_addr.addr_bytes, RTE_ETHER_ADDR_LEN);
                    
                    rte_memcpy(&src_ip, &iphdr->dst_addr, sizeof(uint32_t));
                    rte_memcpy(&dst_ip, &iphdr->src_addr, sizeof(uint32_t));

                    rte_memcpy(&src_port, &udphdr->dst_port, sizeof(uint16_t));
                    rte_memcpy(&dst_port, &udphdr->src_port, sizeof(uint16_t));

                    

                    struct rte_mbuf* udp_reply_buf = 
                    send_udp(mbuf_pool, ((uint8_t*)udphdr + 1), ntohs(udphdr->dgram_len) - sizeof(struct rte_udp_hdr));
                    ret = rte_eth_tx_burst(DPDK_PORT_ID, 0, &udp_reply_buf, 1);
                    if(ret != 0){
                        printf("UDP reply sent to IP: %s\n", inet_ntoa(addr));
                        rte_pktmbuf_free(udp_reply_buf);
                    } else {
                        printf("Failed to send UDP reply\n");
                    }
                }

                if(iphdr->next_proto_id == IPPROTO_ICMP){
                    struct rte_icmp_hdr* icmphdr = 
                    rte_pktmbuf_mtod_offset(mbufs[i], struct rte_icmp_hdr*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

                    addr.s_addr = iphdr->src_addr;
                    printf("Received ICMP message from source IP: %s\n", inet_ntoa(addr));
                    
                    if (icmphdr->icmp_type == RTE_IP_ICMP_ECHO_REQUEST) {

					struct rte_mbuf *icmp_reply_buf = send_icmp(mbuf_pool, ehdr->s_addr.addr_bytes,
						iphdr->dst_addr, iphdr->src_addr, icmphdr->icmp_ident, icmphdr->icmp_seq_nb);

					ret = rte_eth_tx_burst(DPDK_PORT_ID, 0, &icmp_reply_buf, 1);
                        if(ret != 0){
                            printf("ICMP reply sent to IP: %s\n", inet_ntoa(addr));
                            rte_pktmbuf_free(icmp_reply_buf);
                        } else {
                            printf("Failed to send ICMP reply\n");
                        }
                    
				    }
                
                }

            }

            rte_pktmbuf_free(mbufs[i]);
        }
    }
}

