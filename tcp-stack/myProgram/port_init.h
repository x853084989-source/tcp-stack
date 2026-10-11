#ifndef MYPROGRAM_PORT_INIT_H
#define MYPROGRAM_PORT_INIT_H

#include <rte_mempool.h>

int port_init(struct rte_mempool *mbuf_pool);

#endif /* MYPROGRAM_PORT_INIT_H */
