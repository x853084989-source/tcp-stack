#ifndef MYPROGRAM_APP_STATE_H
#define MYPROGRAM_APP_STATE_H

#include <stdint.h>
#include <rte_ether.h>

/* Local interface identity shared by initialization and packet builders. */
extern uint32_t local_host_ip;
extern uint8_t local_mac[RTE_ETHER_ADDR_LEN];

#endif /* MYPROGRAM_APP_STATE_H */
