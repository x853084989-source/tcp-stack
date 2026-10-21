# myProgram：06/07 网络架构、UDP 与 08～10 TCP 应用接口

沿用原有四空格、snake_case、.c/.h 按职责拆分的风格，在已有 06/07 版本上整合
10_tcp_api/tcp.c 的 TCP 服务端主线。原有未提交的可读性重构作为本轮基线保留。

## 目录结构

~~~text
myProgram/
├── main.c               # 启动和执行角色安排
├── config.h             # 配置与保留的 ENABLE 定义
├── app/                 # UDP / TCP 回显应用
├── common/              # 共享本地地址、网卡、收发队列和协议循环
├── protocol/            # ARP / ICMP / UDP / TCP 报文处理
├── socket/              # 共用 n* 接口、UDP 端点、TCP 连接与状态
├── Makefile
└── README.md
~~~

每个模块的 .c/.h 相邻。跨模块引用写明目录，例如 protocol/udp.h；
Makefile 为两套原有构建分支提供项目根目录搜索路径。

此前重构已删除未使用的 send_udp 包装及其旧全局地址缓存，
ARP/ICMP 构造报文的函数已改名为 build_arp_packet/build_icmp_packet。
本轮将 runtime 原样迁移为 common，更新引用与两套构建分支；新增 TCP 协议、连接和应用模块。
Makefile 固定默认目标为 all，避免 legacy 构建载入旧依赖后只检查首个对象文件。
应用中的 client_address 是本次收到数据报的客户端地址；
socket 接口中的 source_address/destination_address 分别表示接收来源与发送目标。
udp_datagram 的 payload/payload_length 明确只保存 UDP 正文，IP/端口仍是网络字节序。
UDP 行为、已有队列容量、UDP ring 名称及 mbuf 所有权规则沿用原有实现。
所有公开 n* 入口集中在 socket/socket_api.c，再转到 UDP 或 TCP 内部实现；
UDP 描述符仍使用 3～66，TCP 使用后续独立区间，避免两类端点冲突。

## 2026-10-21 命名与接口归属整理

保留 app / socket / protocol / common 分类、公开 n* 接口和现有协议行为。
本次只统一命名、集中应用入口、去掉 UDP 输入的未用参数，并清理内部头文件依赖。

| 层次 | UDP | TCP |
|---|---|---|
| 应用读 / 写的内部实现 | udp_socket_receive / udp_socket_send | tcp_socket_receive / tcp_socket_send |
| 端点输入 / 输出推进 | udp_socket_input / udp_socket_output | tcp_socket_input / tcp_socket_output |
| 报文输入 / 输出 | udp_input / udp_output | tcp_input / tcp_output |
| 端点 / 排队数据 | udp_socket / udp_datagram | tcp_socket / tcp_segment |

input/output 表示该层的处理方向；入队操作使用 enqueue，队列对象使用 queue。
udp_socket_input 匹配端点、复制正文、入接收队列并通知；tcp_socket_input 还负责连接状态推进。
两侧 socket_output 都只是尝试推进发送，ARP 等待可能保留消息，不表示已经提交网卡或得到确认。
accepted 保留原语义：应用创建的端点可被 fd 接口查找；子连接到 accept 时才向应用开放。

修改前后均通过原有进程内检查：UDP 11542 项、TCP 3307 项；14 个头文件独立编译通过，
DPDK 19.08.2 legacy 构建无警告。执行代码经词法对照，仅有约定的命名、转发和未用参数整理。
main.c 原有 UDP 线程变量改动及公开 socket_api.h 保持原样；未执行真实网卡端到端验证。
本次工作树基线、前后验收日志及修改映射保存在：
/home/user/.codex/code-backups/myProgram-naming-20261021-4m1sltnv。

## 模块与执行角色

| 文件 | 职责 |
|---|---|
| main.c | 初始化；安排协议 worker，创建 UDP 和 TCP 两条应用 pthread |
| common/net_arch.c/.h | 全局 in/out ring、待发报文入队、主 lcore 独占网卡 RX/TX |
| common/packet.c/.h | 协议循环、报文分类、RX mbuf 释放、UDP/TCP 发送调度、定时器管理 |
| protocol/arp.c/.h | ARP 收包处理、表管理、报文构造、定时探测；所有输出进入 out |
| protocol/icmp.c/.h | 原有简化 ICMP 应答；所有输出进入 out |
| protocol/udp.c/.h | UDP 报文检查、应用分发、ARP 查询、封包和输出 |
| protocol/tcp.c/.h | TCP 长度与校验和检查、应用分发、ARP 查询、TCP 封包和输出 |
| socket/socket_api.c/.h | 全部公开 n* 应用接口入口 |
| socket/udp_socket.c/.h | 原有 UDP 端点、receive_queue/send_queue、阻塞收发与生命周期 |
| socket/tcp_socket.c/.h | 监听与连接对象、四元组、TCP 状态、收发队列与阻塞应用接口 |
| app/udp_app.c/.h | 使用 nsocket/nbind/nrecvfrom/nsendto 的 UDP 回显应用 |
| app/tcp_app.c/.h | 使用 nsocket/nbind/nlisten/naccept/nrecv/nsend/nclose 的 TCP 回显应用 |
| common/port_init.c/.h | 网卡及收发队列配置 |
| common/app_state.c/.h | 本地 MAC/IP |
| config.h | 保留的 ENABLE 定义、队列容量、应用端口、UDP/TCP 大小与 ARP 参数 |

默认有四个业务执行角色（不是进程全部 OS 线程的实测总数）：

- 主 DPDK lcore：网卡收发，把收到的 mbuf 指针送入 in，把 out 中的报文提交给网卡。
- 协议 DPDK lcore：packet_worker 从 in 取包，处理 ARP/ICMP/UDP/TCP，调度发送和定时器。
- UDP pthread：pthread_create → run_udp_application → udp_server_entry，阻塞接收后回显。
- TCP pthread：pthread_create → run_tcp_application → tcp_server_entry，接受连接后阻塞收发。

当前虚拟机有两个 CPU，因此应用使用普通 pthread，共享 CPU；只需两个 DPDK lcore。
协议 worker 由 rte_eal_remote_launch 安排到 EAL 已创建的 worker lcore；
ARP 定时器回调在协议 worker 执行，不增加业务线程。

## 数据路径

~~~text
接收：
网卡 -> io_worker -> in -> packet_worker -> process_packets
     -> udp_input -> udp_socket_input -> 端点 receive_queue
     -> udp_socket_receive -> nrecvfrom 返回 -> udp_server_entry

发送：
udp_server_entry -> nsendto -> udp_socket_send -> 端点 send_queue -> udp_socket_output
                 -> udp_output -> 查 ARP / 构造 UDP 报文
                 -> out -> io_worker -> 网卡
~~~

in/out 传递 mbuf 指针；receive_queue/send_queue 传递拥有独立正文副本的 udp_datagram。
端点按绑定的本地 IP/端口匹配。n* 是本项目的用户态接口，不是 Linux socket 系统调用。

TCP 接收复用 in：tcp_input → tcp_socket_input → 四元组对应的 tcp_socket
→ receive_queue → tcp_socket_receive → nrecv 返回 → tcp_server_entry。
TCP 发送复用 out：nsend → tcp_socket_send → send_queue → tcp_socket_output → tcp_output → out → io_worker。
TCP 队列传递 tcp_segment；短读剩余数据保留在 receive_pending，ARP 等待的发送头保留在 send_pending。

监听对象收到 SYN 后建立独立连接对象并排入 SYN+ACK；第三次 ACK 合法后才唤醒 naccept。
连接路径为 SYN_RCVD → ESTABLISHED → 对端 FIN → CLOSE_WAIT → nclose → LAST_ACK → 最后 ACK 释放。
TCP 应用与 10 一样处理完当前连接才再次 accept，不包含 13 的 nepoll 多连接应用循环。

## 生命周期与线程边界

- 协议处理函数借用 RX mbuf，返回后由 process_packets() 释放一次。
- enqueue_tx_packet() 接管参数：入队成功交给 out，失败立即释放。
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

TCP 注册表与状态使用独立互斥锁；阻塞等待使用同一把锁检查谓词并等待条件变量。
TCP 关闭应用接口后仍保留连接和描述符，收到最后 ACK 且无等待者后才释放队列与对象。
每个连接的 ring 名称包含描述符，避免原示例固定名称导致后续连接创建失败。
nsend 返回复制入发送队列的字节数；一次最多接受 1460 字节，应用按返回值继续发送。
nrecv 短读只返回实际复制数，尾部留在队头；数据全部取完后，才将对端 FIN 表示为 EOF。

原 ARP 定时器周期和管理阈值保留，只将执行位置移动到协议 lcore。

## 当前学习版本

06～10 的当前功能直接编译，代码不通过 ENABLE 宏选择分支。
config.h 中保留原有 ENABLE_* 定义，修改其值不会关闭功能。

- 当前源码逻辑 IP 为 192.168.27.8，UDP 回显监听 8889，TCP 回显监听 9999。
- 只有匹配本地 IP 和绑定端口的数据报进入应用。

本阶段先比较 main.c 和新增模块，再从 app/tcp_app.c 的应用流程追踪监听、连接和收发。

## 构建与验证

~~~sh
cd /home/user/code/program/tcp-stack/myProgram
make RTE_SDK=/home/user/code/2.network/2.4/dpdk/dpdk-stable-19.08.2 \
     RTE_TARGET=x86_64-native-linux-gcc
~~~

下面启动参数来自此前环境记录；运行前由用户核对当前数据网卡与可用 CPU：

~~~sh
sudo ./build/myProgram -l 0-1 -n 4 -w 0000:0b:00.0
~~~

网络助手向 192.168.27.8:8889 发送数据，观察：

1. 启动日志中的 I/O、协议 lcore 与 UDP 监听地址。
2. 客户端收到相同字节数、相同内容的回复。
3. 先测无换行的 nihao，应收到五字节 6E 69 68 61 6F；再测包含 00 的二进制数据。
4. ARP 表没有对端时，先解析邻居，再尝试 UDP 输出；超时会丢弃未发送消息。

TCP 验收：客户端连接 192.168.27.8:9999，发送 nihao 或含 00 的二进制数据，确认原字节回显；
先关闭客户端发送方向，继续接收至 EOF，再重连，检查连接资源能重复使用。
UDP 8889 继续使用上述原有输入确认回显。真实网卡和外部客户端验证由用户操作。

## 当前范围

实现 IPv4 UDP 与 TCP 被动服务端接口；支持绑定本地 IP 或 INADDR_ANY。
UDP 单条正文最多 1472 字节；TCP 单次 nsend 最多接受 1460 字节；只收发往本地逻辑 IP 的报文。
TCP 接收支持解析 TCP 头长度与跳过选项，发送固定 20 字节 TCP 头，不协商选项。
没有主动连接、完整主动关闭状态机、重传/超时回收、乱序重组、拥塞控制、窗口协商、RST 恢复或 nepoll。
与原 10 一样，nlisten 的 backlog 参数尚不限制半连接/待接受连接队列；总 TCP 对象上限为 64。
握手不完成或最后 ACK 丢失时，对象会保留；不把这些教学边界描述为可靠完整的 TCP。
只接受无 IPv4 选项、未分片的 TCP 报文；不提供自动临时端口、完整 POSIX flags 或分片重组。
保留原有简化 ICMP 应答和 ARP 表策略；不把本轮迭代扩展为完整协议栈修复。

## 2026-10-19 TCP 整合验收

本轮使用现有 DPDK 19.08.2 legacy 构建，编译与链接通过，无编译警告。
复用旧 UDP 检查并适配目录与接口位置，仅将“SOCK_STREAM 不支持”断言改为 SOCK_RAW 不支持：
11542 项检查通过，覆盖 UDP 二进制/空数据报、截断、阻塞/关闭、ARP 重试、队列满、
应用回显及部分 TX 接纳时的缓冲区所有权。
新增 3307 项 TCP 检查通过，覆盖握手前 accept 阻塞、错误 ACK、短读顺序、连续发送序号、
ARP 等待顺序、异常报文、带数据的 FIN、最后 ACK 校验、实际应用二进制回显，
并连续完成 68 次建连和关闭，确认描述符与 ring 名称可复用。
14 个头文件独立编译、现有格式规则及差异空白检查通过。
以上为关闭 PCI 网卡探测的进程内验收；测试辅助文件保存在备份目录，不加入项目源码。
本轮修改前的源码、Git 状态与差异保存在：
/home/user/.codex/code-backups/myProgram-tcp10-20261019。
本轮没有启动真实数据网卡或执行外部客户端端到端验证；编译和定向检查结果见同目录验收记录。

## 保留的既有实现限制

- ICMP 应答仍只构造固定长度的 Echo Reply 头部，没有复制请求中的 ICMP 正文。
- ARP 定时探测要等协议线程调用 rte_timer_manage；原有较大的管理门限保留，
  不能仅凭定时器的周期配置断言实际每秒探测。
以上是源码可见的既有边界，本轮没有修改其功能。
