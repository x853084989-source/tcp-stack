#ifndef __NG_ARP_H__
#define __NG_ARP_H__

#include "main.h"

#define ARP_ENTRY_STATUS_DYNAMIC	0
#define ARP_ENTRY_STATUS_STATIC		1


#define LL_ADD(item, list) do {		\
	item->prev = NULL;				\
	item->next = list;				\
	if (list != NULL) list->prev = item; \
	list = item;					\
} while(0)


#define LL_REMOVE(item, list) do {		\
	if (item->prev != NULL) item->prev->next = item->next;	\
	if (item->next != NULL) item->next->prev = item->prev;	\
	if (list == item) list = item->next;	\
	item->prev = item->next = NULL;			\
} while(0)

#define TIMER_RESOLUTION_CYCLES 120000000000ULL // 10ms * 1000 = 10s * 6 

struct arp_table_item {

	uint32_t ip;
	uint8_t mac_addr[RTE_ETHER_ADDR_LEN];

	uint8_t type;
	// 

	struct arp_table_item *next;
	struct arp_table_item *prev;
	
};

struct arp_table {

	struct arp_table_item * top_table;
	int count;

};



struct arp_table *arp_table_instance(void);

uint8_t *get_dst_macaddr(uint32_t dip);



struct rte_mbuf *send_arp(struct rte_mempool *mbuf_pool, uint16_t opcode, uint8_t *dst_mac, uint32_t sip, uint32_t dip);

static void arp_request_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg);
#endif
