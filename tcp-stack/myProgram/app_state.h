#ifndef MYPROGRAM_APP_STATE_H
#define MYPROGRAM_APP_STATE_H

#include <stdint.h>
#include <rte_ether.h>

/* Shared endpoint values used by the existing echo path. */
extern uint32_t local_host_ip;
extern uint32_t src_ip;
extern uint32_t dst_ip;
extern uint8_t src_mac[RTE_ETHER_ADDR_LEN];
extern uint8_t dst_mac[RTE_ETHER_ADDR_LEN];
extern uint16_t src_port;
extern uint16_t dst_port;

#endif /* MYPROGRAM_APP_STATE_H */
