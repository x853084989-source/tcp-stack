/* 本地接口共享配置；组包时直接使用网络序IP，避免重复转换字节序。 */

#ifndef MYPROGRAM_NETWORK_INTERFACE_H
#define MYPROGRAM_NETWORK_INTERFACE_H

#include <stdint.h>
#include <rte_ether.h>

/* Local interface identity shared by initialization and packet builders. */
extern uint32_t local_host_ip;
extern uint8_t local_mac[RTE_ETHER_ADDR_LEN];

#endif /* MYPROGRAM_NETWORK_INTERFACE_H */
