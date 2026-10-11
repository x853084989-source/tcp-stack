#include "app_state.h"

#include <rte_byteorder.h>
#include <rte_ip.h>

/* Keep the local IP in network byte order, like ARP packet fields. */
uint32_t local_host_ip = RTE_BE32(RTE_IPV4(192U, 168U, 27U, 8U));
uint32_t src_ip;
uint32_t dst_ip;

uint8_t src_mac[RTE_ETHER_ADDR_LEN];
uint8_t dst_mac[RTE_ETHER_ADDR_LEN];

uint16_t src_port;
uint16_t dst_port;
