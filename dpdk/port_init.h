/* 网卡端口初始化入口；mbuf_pool供网卡接收队列分配报文缓冲区。 */

#ifndef MYPROGRAM_PORT_INIT_H
#define MYPROGRAM_PORT_INIT_H

#include <rte_mempool.h>

/* mbuf_pool供RX队列使用；先配置端口、建RX/TX描述符队列，再启动设备。 */
int port_init(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_PORT_INIT_H */
