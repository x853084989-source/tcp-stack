/*
 * 项目容量与时间策略。长度、记录数、毫秒、秒和计数器ticks不是同一单位。
 * 当前TCP窗口是固定通告值；下面的重试次数属于实验策略，不代表完整拥塞控制。
 */

#ifndef MYPROGRAM_CONFIG_H
#define MYPROGRAM_CONFIG_H

/* Retained ENABLE definitions; features are compiled unconditionally. */
#define ENABLE_SEND 1
#define ENABLE_ARP_REQUEST 1
#define ENABLE_ARP_REPLY 1
#define ENABLE_ICMP 1
#define ENABLE_UDP_APP 1
#define ENABLE_TCP_APP 1
#define ENABLE_KNI_APP 1
#define ENABLE_DDOS_DETECT 1
#define ENABLE_SINGLE_EPOLL 1

/* ring配置项数；默认非EXACT_SZ模式实际可容纳RING_SIZE-1个对象，不是正文总字节数。 */
#define RING_SIZE 1024
#define MAX_UDP_SOCKETS 64
#define UDP_APP_PORT 8889
#define UDP_MAX_PAYLOAD_SIZE 1472

/* UDP、TCP、poll使用不同fd区间，socket_api据此分流；这些编号不属于Linux fd表。 */
#define TCP_FIRST_FD (3 + MAX_UDP_SOCKETS)
#define MAX_TCP_SOCKETS 64
#define TCP_APP_PORT 9999
#define TCP_APP_BUFFER_SIZE 1024
#define TCP_APP_MAX_EVENTS 16
#define EVENT_POLL_FD (TCP_FIRST_FD + MAX_TCP_SOCKETS)
#define TCP_MAX_PAYLOAD_SIZE 1460

/* 当前写入TCP首部的固定接收窗口；尚未随真实接收缓冲或拥塞状态动态调整。 */
#define TCP_INITIAL_WINDOW 14600

/* RTO到期比较约每10毫秒开放一轮；不是每10毫秒重传，也不使协议线程sleep。 */
#define TCP_TIMER_CHECK_MS 10

/* 新连接初始ACK等待时长为1秒；首次成功提交需要确认的记录时才启动截止时间。 */
#define TCP_RTO_INITIAL_SECONDS 1

/* 成功重传后的RTO倍增上限；当前没有RTT估计器把退避值自动收缩。 */
#define TCP_RTO_MAX_SECONDS 60

/* 不含首发，允许4次成功提交重传；最后一次仍等待一轮ACK，无进展才终止连接。 */
#define TCP_RETRANSMIT_LIMIT 4

/* 本地组包/入队硬失败后的等待，独立于等待对端ACK的RTO。 */
#define TCP_OUTPUT_RETRY_SECONDS 1

/* 连接连续2次本地硬失败则终止；成功输出清零，等待ARP不计入硬失败。 */
#define TCP_OUTPUT_FAILURE_LIMIT 2

/* 单条待发记录主动ARP请求的最小间隔，避免每轮协议轮询都广播请求。 */
#define ARP_RETRY_SECONDS 1

/* UDP记录从接纳起最多等待5秒本地输出；UDP本身没有ACK，这不是可靠重传时限。 */
#define UDP_SEND_TIMEOUT_SECONDS 5

#define DPDK_PORT_ID 0
#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 32
#define MAKE_IPV4_HOST_ADDR(a, b, c, d) ((a << 24) + (b << 16) + (c << 8) + (d))

/* Existing TSC threshold, now managed by the protocol lcore. */
/*
 * 旧的ARP定时器manage调用门槛，单位为TSC计数，不是秒或毫秒。
 * 门槛很大时ARP到期回调可能明显滞后；TCP RTO检查独立，不受这个门槛控制。
 */
#define TIMER_RESOLUTION_CYCLES 120000000000ULL

#endif /* MYPROGRAM_CONFIG_H */
