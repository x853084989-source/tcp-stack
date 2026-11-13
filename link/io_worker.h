/* I/O循环同时处理KNI控制请求，网卡队列只由这个执行角色访问。 */

#ifndef MYPROGRAM_IO_WORKER_H
#define MYPROGRAM_IO_WORKER_H

/* Main lcore: sole owner of NIC RX/TX and KNI device-control requests. */
/* 主lcore的持续收发循环；RX入队后由协议线程消费，TX提交成功的mbuf交由驱动持有。 */
int io_worker(void *arg);

#endif /* MYPROGRAM_IO_WORKER_H */
