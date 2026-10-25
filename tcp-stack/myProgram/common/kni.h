#ifndef MYPROGRAM_KNI_H
#define MYPROGRAM_KNI_H

#include <rte_mbuf.h>
#include <rte_mempool.h>

int kni_init(struct rte_mempool *mbuf_pool);
/* Consumes one packet: transfers it to the kernel, or frees it on failure. */
int kni_send(struct rte_mbuf *mbuf);
/* Protocol lcore: moves kernel packets to out; does not access NIC queues. */
void kni_receive(void);
/* I/O lcore only: control callbacks may stop/restart its NIC. */
void kni_handle_requests(void);
int kni_port_is_running(void);

#endif /* MYPROGRAM_KNI_H */
