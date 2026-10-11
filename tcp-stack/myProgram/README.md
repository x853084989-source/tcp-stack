# myProgram 模块说明

本次整理保留现有报文处理、发送、字节序、全局状态和定时器参数。

| 文件 | 职责 |
|---|---|
| `main.c` | 初始化、批量收包、调用处理入口、驱动定时器管理 |
| `packet.c/.h` | 检查 Ethernet/IPv4 基本长度、按协议分发、释放收到的 mbuf |
| `arp.c/.h` | ARP 收包处理、表管理、报文构造、定时探测回调 |
| `udp.c/.h` | UDP 收包检查及回显、UDP 报文构造 |
| `icmp.c/.h` | ICMP 收包处理及应答、ICMP 报文构造 |
| `port_init.c/.h` | 网卡和收发队列初始化 |
| `app_state.c/.h` | 共享端点变量的一份定义及对应 extern 声明 |
| `config.h` | 功能开关、端口号、缓冲区和定时器配置 |
| `.clang-format` | 四空格缩进、统一括号与换行的排版约定 |

协议处理函数借用收到的 mbuf，返回后由 `process_packets()` 释放；
应答发送成功后由 DPDK 接管，应答发送失败仍按原逻辑释放。
ARP 表中的 MAC 地址由表管理，`get_dst_macaddr()` 返回借用指针。

构建：

```sh
make RTE_SDK=/home/user/code/2.network/2.4/dpdk/dpdk-stable-19.08.2 \
     RTE_TARGET=x86_64-native-linux-gcc
```
