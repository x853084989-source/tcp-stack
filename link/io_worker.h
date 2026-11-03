#ifndef MYPROGRAM_IO_WORKER_H
#define MYPROGRAM_IO_WORKER_H

/* Main lcore: sole owner of NIC RX/TX and KNI device-control requests. */
int io_worker(void *arg);

#endif /* MYPROGRAM_IO_WORKER_H */
