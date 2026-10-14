# myProgram：06 网络架构与 07 UDP 应用层

沿用原有四空格、snake_case、.c/.h 按职责拆分的风格，在已有 ARP 表、UDP、ICMP 基础上加入 06/07 的功能。

## 模块与执行角色

| 文件 | 职责 |
|---|---|
| main.c | EAL、内存池、网卡、ring 和定时器初始化；启动协议线程和 UDP 应用线程 |
| net_arch.c/.h | 全局 in/out ring、待发报文入队、主 lcore 独占网卡 RX/TX |
| packet.c/.h | 协议 lcore 的循环、报文分类、RX mbuf 释放、UDP 发送调度、定时器管理 |
| arp.c/.h | ARP 收包处理、表管理、报文构造、定时探测；所有输出进入 out |
| icmp.c/.h | 原有简化 ICMP 应答；所有输出进入 out |
| udp.c/.h | UDP 报文检查、应用分发、ARP 查询、封包和输出 |
| udp_socket.c/.h | 用户态 UDP 端点、每端点 rcvbuf/sndbuf、条件变量及 n* 接口 |
| udp_app.c/.h | 使用 nsocket/nbind/nrecvfrom/nsendto 的 UDP 回显应用 |
| port_init.c/.h | 网卡及收发队列配置 |
| app_state.c/.h | 原有共享端点变量与本地 MAC/IP |
| config.h | 功能开关、队列容量、应用端口、UDP 大小与 ARP 重试参数 |

默认有三个执行角色：

- 主 DPDK lcore：网卡收发，把收到的 mbuf 指针送入 in，把 out 中的报文提交给网卡。
- 协议 DPDK lcore：从 in 取包，处理 ARP/ICMP/UDP，调度 UDP 发送和 ARP 定时器。
- 普通 pthread：UDP 应用在空接收队列上等待，被唤醒后回显。

当前虚拟机有两个 CPU，因此应用使用普通 pthread，共享 CPU；只需两个 DPDK lcore。
原 07 用独立 DPDK lcore 执行应用，这里保留其线程分工与队列机制，适配现有虚拟机。

## 数据路径

~~~text
接收：
网卡 -> io_worker -> in -> packet_worker -> process_packets
     -> process_udp_packet -> udp_socket_deliver -> 端点 rcvbuf
     -> 唤醒 nrecvfrom -> udp_server_entry

发送：
udp_server_entry -> nsendto -> 端点 sndbuf -> udp_socket_flush
                 -> udp_output -> 查 ARP / 构造 UDP 报文
                 -> out -> io_worker -> 网卡
~~~

in/out 传递 mbuf 指针；rcvbuf/sndbuf 传递拥有独立正文副本的 udp_datagram。
端点按绑定的本地 IP/端口匹配。n* 是本项目的用户态接口，不是 Linux socket 系统调用。

## 生命周期与线程边界

- 协议处理函数借用 RX mbuf，返回后由 process_packets() 释放一次。
- queue_tx_packet() 接管参数：入队成功交给 out，失败立即释放。
- io_worker() 只释放网卡未接受的 TX mbuf；已接受的报文归 DPDK 管理。
- in 满时，仅释放未成功入队的 RX mbuf。
- 接收正文先复制到消息，再释放 RX mbuf；应用取得一条消息后释放消息。
- nsendto() 复制应用数据并入队，返回表示入队成功，不表示对端收到。
- 消息长度只表示 UDP 正文字节数；回显用 nrecvfrom() 返回值，支持二进制与空数据报。
- 接收缓冲区过短时丢弃同一数据报剩余正文，不把 UDP 拆成字节流。
- ARP 未命中时消息留在发送队列重试；每条消息最多每秒探测一次，五秒未输出则丢弃。
- ARP 表查询、更新和定时回调都在协议 lcore，out 只有一个生产角色。
- 端点注册表使用互斥锁；端点队列、等待谓词和通知使用端点锁。
- nclose() 标记关闭并唤醒阻塞接收；活跃调用的引用结束后再释放队列和端点。

原 ARP 定时器周期和管理阈值保留，只将执行位置移动到协议 lcore。

## 两种学习配置

config.h 默认 ENABLE_UDP_APP=1：

- 06 的网络架构和 07 的 UDP 应用层均启用。
- 当前逻辑 IP 为 192.168.27.8，UDP 回显应用监听 8889。
- 只有匹配本地 IP 和绑定端口的数据报进入应用。

将 ENABLE_UDP_APP 设为 0，或构建时传 EXTRA_CFLAGS=-DENABLE_UDP_APP=0：

- 保留 06 的线程与全局 ring 架构。
- UDP 使用原有协议层直接回显，回复仍经 out 交主 lcore 发送。

学习入口建议先看 main.c 的角色安排，然后看 udp_app.c 的业务流程，
再沿收发路径追踪队列和协议处理，不必逐文件读全部实现。

## 构建与验证

~~~sh
cd /home/user/code/program/tcp-stack/myProgram
make RTE_SDK=/home/user/code/2.network/2.4/dpdk/dpdk-stable-19.08.2 \
     RTE_TARGET=x86_64-native-linux-gcc
~~~

本次核对到数据网卡 0000:0b:00.0 绑定 igb_uio、CPU 为 0-1。
在已有 DPDK 运行条件下，默认配置可使用：

~~~sh
sudo ./build/myProgram -l 0-1 -n 4 -w 0000:0b:00.0
~~~

网络助手向 192.168.27.8:8889 发送数据，观察：

1. 启动日志中的 I/O、协议 lcore 与 UDP 监听地址。
2. 客户端收到相同字节数、相同内容的回复。
3. 先测无换行的 nihao，应收到五字节 6E 69 68 61 6F；再测包含 00 的二进制数据。
4. ARP 表没有对端时，先解析邻居，再尝试 UDP 输出；超时会丢弃未发送消息。

当前已通过两种配置的无警告构建，以及不接触物理网卡的进程内行为检查，覆盖应用回显、消息边界、
阻塞/关闭、ARP 重试、队列容量及部分 TX 的 mbuf 生命周期。
这些检查不代替真实网卡启动和网络助手端到端回包确认。

## 当前范围

只实现 IPv4 UDP 应用接口；不提供 TCP、自动临时端口、完整 POSIX flags 或分片重组。
单条正文最多 1472 字节；支持绑定本地 IP 或 INADDR_ANY，接收只处理发往本地逻辑 IP 的数据报。
保留原有简化 ICMP 应答和 ARP 表策略；不把本轮迭代扩展为完整协议栈修复。
