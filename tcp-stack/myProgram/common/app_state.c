#include "app_state.h"

#include <rte_byteorder.h>
#include <rte_ip.h>

/* Keep the local IP in network byte order, like ARP packet fields. */
uint32_t local_host_ip = RTE_BE32(RTE_IPV4(192U, 168U, 27U, 8U));

uint8_t local_mac[RTE_ETHER_ADDR_LEN];
