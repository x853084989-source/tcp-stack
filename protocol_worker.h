/* 协议处理入口由main通过rte_eal_remote_launch安排到已有工作lcore执行。 */

#ifndef MYPROGRAM_PROTOCOL_WORKER_H
#define MYPROGRAM_PROTOCOL_WORKER_H

/* Runs on the dedicated protocol lcore. */
/* arg由main传入mbuf池；每轮即使没有RX包，也要推进发送及TCP计时，避免空闲连接无法重传。 */
int protocol_worker(void *arg);

#endif /* MYPROGRAM_PROTOCOL_WORKER_H */
