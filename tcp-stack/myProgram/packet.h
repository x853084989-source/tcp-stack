#ifndef MYPROGRAM_PACKET_H
#define MYPROGRAM_PACKET_H

#include <rte_mbuf.h>
#include <rte_mempool.h>

/* Processes each received mbuf and releases it after protocol handling. */
void process_packets(struct rte_mempool *mbuf_pool, struct rte_mbuf **mbufs, int num_recvd);

#endif /* MYPROGRAM_PACKET_H */
