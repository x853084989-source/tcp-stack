# 用户代码风格与命名实例

这是从用户当前 `myProgram` 中提炼、经用户要求保留并推广到后续项目的偏好。规则用于新增和获准修改的代码，不代表需要重新命名现有代码。下面的协议与目录是实例，不是其他项目必须采用的结构。

## 分层与模块

- 按实际职责组织模块，相应 `.c/.h` 相邻。当前项目的 `app / socket / protocol / common` 分别负责应用、连接与接口、报文、共享基础设施；新项目根据自身职责划分，不强加这些目录。
- 公开函数通过模块或领域前缀表达归属；文件内的 `static` 辅助函数可省略重复前缀，但须清楚表达动作和对象。避免每层使用相同且含义不明的 `process`、`handle`、`data` 名称。
- C++ 项目可以由命名空间或类表达归属，例如 `tcp_socket::receive`；不要机械叠加成 `tcp_socket::tcp_socket_receive`。保留的是层次清楚的原则，不是固定的 C 函数前缀形式。
- 保持层次和抽象稳定：接口转发、连接状态推进、报文编码可以分别存在，不把不相关职责塞进一个大函数，也不为凑对偶拆出无实际意义的包装。

当前项目实例：

| 层次或对象 | UDP | TCP |
| --- | --- | --- |
| 应用读取 / 发送的实现 | `udp_socket_receive / udp_socket_send` | `tcp_socket_receive / tcp_socket_send` |
| socket 层输入 / 输出推进 | `udp_socket_input / udp_socket_output` | `tcp_socket_input / tcp_socket_output` |
| 报文层输入 / 输出 | `udp_input / udp_output` | `tcp_input / tcp_output` |
| 端点 / 排队数据对象 | `udp_socket / udp_datagram` | `tcp_socket / tcp_segment` |

`receive/send` 是应用视角的读写，`input/output` 是相应层的处理方向。它们不是随意互换的同义词；输入进入哪层、输出交给谁，需要由接口职责明确。其他项目先确定自身视角，再使用一致的方向词。

## 函数、参数与对象命名

- 自有函数、变量和字段使用 `snake_case`；类型名表达对象本身，函数名表达动作。宏和枚举常量使用大写及必要的领域前缀。第三方 API 和目标语言已有约定保留原名。
- 函数与参数一起设计：同类接口保持相同的参数顺序、方向词和长度含义；正文、报文、连接、缓冲区和队列分别命名，不把它们都称为 `data` 或 `buffer`。
- `fd / ip / mac / tcp / udp` 等清楚的通用缩写可以保留。优先使用简短且没有歧义的名称，不把所有局部变量机械展开为长句；循环索引 `i` 也无需为了形式改成长名。
- 连接身份采用 `local_ip / remote_ip`、`local_port / remote_port`；单个报文采用 `source_ip / destination_ip`、`source_port / destination_port`。不要因回复方向变化就把连接的 local/remote 含义交换。
- 新增代码区分长度与数量：`payload_length / buffer_length / address_length` 表示相应对象的长度，`packet_count / ready_count` 表示对象个数。可能混淆时表达单位，例如 `timeout_ms`、`total_bits`、`next_arp_retry_tsc`。网络字节序与主机字节序在接口或字段说明中明确，不给每个变量叠加冗长后缀。
- 实际存在双向或配对操作时保持词汇一致：`receive/send`、`input/output`、`enqueue/dequeue`、`local/remote`、`source/destination`。生命周期操作按真实语义选词；释放一段分配内存和销毁含队列、锁的对象可以分别使用 `free`、`destroy`，不用为形式统一掩盖区别。

当前接口实例：

```c
ssize_t tcp_socket_receive(int socket_fd, void *buffer, size_t buffer_length, int flags);
ssize_t tcp_socket_send(int socket_fd, const void *payload, size_t payload_length, int flags);
```

这里 `buffer` 是调用者提供的接收空间，`payload` 是待复制发送的正文，两个 length 分别约束对应参数。方向与所有权的实际差异比强迫两侧使用完全相同的单词更重要。

当前状态对象实例：

```c
struct rte_ring *receive_queue;
struct rte_ring *send_queue;
struct tcp_segment *receive_pending;
struct tcp_segment *send_pending;
```

`queue` 表示队列，`pending` 表示已取出但尚未处理完的当前对象。两者不能因为都与收发有关就使用相同的泛化名称。

## 排版与控制流

- 默认延续四空格缩进、不用 Tab、同行左花括号、指针星号靠变量名；当前项目约 100 列换行。目标项目已有格式配置时使用其配置，不为应用本偏好格式化无关文件。
- 条件选择使用清楚的 `if/else`，不新增三元表达式或为了短而压成一行的条件、循环。简单且清楚的提前返回可以保留，不强行给每个 `if` 补无意义的 `else`。
- 初始化、错误判断、清理与返回保持可追踪；使用有意义的返回值，不混淆成功入队、完成发送和对端收到。跨模块包含注明模块位置，避免靠偶然的头文件传递获得声明。

用户当前代码的实例：

```c
size_t count;
if (buffer_length < remaining) {
    count = buffer_length;
} else {
    count = remaining;
}
```

## 注释与改动边界

- 注释说明输入来源、结果去向、字节序、单位、所有权、线程或锁的约束，以及必要的设计原因；不逐句翻译明显代码。
- 当前接口注释 `Consumes mbuf: queues it on success, frees it on failure.` 表明调用后原持有者不应再次释放；`Borrows ...` 则表明函数不接管对象。其他项目可沿用这种明确责任的写法。
- 日常迭代优先新增符合约定的代码，保持现有名称和风格。只有用户授权重命名、或接口变动确实必要时才调整已有标识符，并给出旧名、新名、职责及影响；不要将“风格规范”当作全项目清理授权。
