# serial-data-gateway · 嵌入式设备数据采集网关

面向嵌入式设备的 **Linux/C++17 数据采集与分发网关**：从串口接收带 CRC 校验的设备数据帧，
多线程解耦后落盘（定长记录 + 周期 `fsync`），同时通过 **epoll 非阻塞 TCP 服务**对外提供
查询与实时订阅。

**零第三方依赖** —— 只需要 `g++` 与 `pthread`，`make` 一条命令即可构建。

```
采集线程 ──► 有界队列 ──► 分发线程 ──┬──► 落盘队列 ──► 落盘线程（攒批 + fsync）
   (串口/模拟)  (drop-oldest)         └──► 内存 Hub ──► epoll TCP 服务（STATS/LAST/SUB）
```

---

## 1. 为什么做这个

嵌入式现场常见的问题是：设备只有串口输出，人要长时间盯着串口助手，数据没法留存、
没法远程查。这个网关把三件事拆成三级流水线：

| 阶段 | 职责 | 关键点 |
|---|---|---|
| 采集 | 解析 `AA 55` 帧头 + CRC16 校验，失步后能重新同步 | 单字节状态机，一次丢字节不会导致后续帧永久错位 |
| 缓存 | 有界队列解耦采集与落盘 | 队列满时**丢弃最旧**并计数，绝不阻塞采集 |
| 落盘 | 定长记录顺序追加 + 周期 `fsync` | 掉电丢失窗口有界（默认 1 秒） |
| 分发 | epoll + 非阻塞套接字对外服务 | 慢消费者不会拖垮事件循环 |

---

## 2. 构建与运行

```bash
make                      # 零依赖构建
./serial-data-gateway --simulate --listen 9000    # 无硬件也能完整跑通
```

有一个真实串口设备时：

```bash
./serial-data-gateway --port /dev/ttyUSB0 --baud 115200 --listen 9000
```

也可以用 CMake：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

### 命令行参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `--port <dev>` | 串口设备；不指定则自动使用模拟源 | 空 |
| `--baud <n>` | 波特率（9600 / 57600 / 115200 / 460800） | 115200 |
| `--listen <n>` | TCP 监听端口 | 9000 |
| `--queue <n>` | 队列深度（条） | 4096 |
| `--file <path>` | 定长记录文件 | records.bin |
| `--fsync <ms>` | fsync 周期 | 1000 |
| `--simulate` | 强制使用模拟源 | 关 |

### 对外命令（`nc 127.0.0.1 9000` 即可手工验证）

```text
STATS             运行统计（总条数 / 丢弃数 / 分通道计数 / 数据源模式）
LAST <n>          最近 n 条记录的十六进制摘要
SUB ON | OFF      开启/关闭实时推送
QUIT              断开
```

实际输出示例：

```text
$ printf 'STATS\n' | nc 127.0.0.1 9100
OK serial-data-gateway ready; try STATS
records=73 dropped=0 source=simulated ch0=37 ch1=36 connections=1

$ printf 'LAST 2\n' | nc 127.0.0.1 9100
count=2
seq=218 ch=1 len=9 data=CE D1 D4 D7 DA DD E0 80 83
seq=219 ch=0 len=15 data=...
```

---

## 3. 线上帧格式

与固件侧约定，所有多字节字段小端：

```text
+------+------+-----+----+--------------+---------+---------+
| 0xAA | 0x55 | LEN | CH |   PAYLOAD    |  CRC_L  |  CRC_H  |
+------+------+-----+----+--------------+---------+---------+
  帧头    帧头   1B    1B     LEN 字节      2B（CRC-16/MODBUS）
```

- `CRC-16/MODBUS`：`poly=0xA001`（反射 0x8005）、`init=0xFFFF`、无 `xorout`
- 校验覆盖范围：`LEN + CH + PAYLOAD`
- 采用**查表实现**（表在首次调用时生成），比逐位计算快约 8 倍

---

## 4. 关键设计决策

### 4.1 队列满时丢弃最旧，而不是阻塞采集

采集侧是实时入口，一旦被阻塞就会丢输入且不可恢复。因此 `BoundedQueue::push` 在队列满时
弹出最旧记录并累加 `dropped` 计数（drop-oldest），把"背压"变成**可观测指标**而不是隐性丢帧。
`STATS` 命令可以直接读出丢弃数。

### 4.2 信号处理用 self-pipe trick

信号处理器里只能调用异步信号安全函数，`printf`/`malloc`/`join` 都不允许。
这里让处理器只做一件事 —— 往管道写一个字节；主线程在 `epoll_wait` 的 100 ms 超时后
统一处理收尾逻辑。

### 4.3 TCP 半关闭必须写完响应再关连接

`printf 'STATS\n' | nc host port` 这种用法中，`nc` 发完命令后**立刻关闭写端**。
若在 `recv() == 0` 时直接 `close()`，已生成的响应就永远发不出去 —— 客户端什么也收不到。

正确做法是把"读关闭"与"写完成"分开：收到 `recv()==0` 只置 `read_closed`，
等待写缓冲清空后再关闭连接。**这个 bug 在开发过程中真实出现过，冒烟测试第 3 项就是它的回归用例。**

### 4.4 多级流水线的退出顺序

退出时必须按 **生产者 → 中间层 → 消费者** 逐级收尾：

```
stop = true  →  join(采集)  →  close(raw_q)  →  join(分发)
             →  close(store_q)  →  join(落盘)
```

顺序错了就会丢数据：若先关 `store_q`，分发线程后续 `push` 的记录将无人消费。
另外消费者不能写成 `while (!stop)` 就退出 —— `stop` 只表示"信号已到达"，
此刻队列里可能还有未处理记录，必须排空到队列关闭为止。

**这个 bug 同样真实出现过**：修好前的一致性测试显示 `解析 148 帧 / 落盘 147 条`。
修复后 146/146/146 完全对齐。

### 4.5 慢消费者保护

每个连接维护独立的待写缓冲，积压超过 256 KB 直接断开该连接，
避免一个卡住的客户端把整个事件循环拖住。待写缓冲清空后立刻取消 `EPOLLOUT` 注册，
防止事件循环被空转唤醒。

---

## 5. 测试

```bash
bash tests/verify_all.sh        # 全部：构建 + 冒烟 + 一致性
bash tests/smoke_test.sh        # 8 项功能/协议/退出测试
bash tests/consistency_test.sh  # 数据一致性对账
```

`consistency_test.sh` 是核心回归用例，校验三级流水线**逐级条数完全相等**：

```text
[source]   解析 146 帧，CRC 失败 0 次
[dispatch] 分发 146 条，队列丢弃 0 条
[storage]  落盘 146 条
文件 12848 字节 / sizeof(Record)=88  =  146 条
```

即：**无丢帧、退出前 flush 生效、无半条写入**。

---

## 6. 已知限制

诚实清单 —— 以下都没有做或没有验证：

| 类别 | 说明 |
|---|---|
| 真实串口 | 开发与测试全部使用内置模拟源（它会构造完整带 CRC 的字节流，走同一条解析状态机）。**未在真实 USB-TTL 设备上验证** |
| 吞吐上限 | 未做压力量化。模拟源约 50 帧/秒，真实 460800 波特率下的持续吞吐没有实测数据 |
| 队列溢出行为 | `drop-oldest` 有计数，但"丢弃后上游是否感知"没有反压通知机制 |
| 落盘格式 | 定长二进制记录，**没有索引文件**，大文件按序回读可行但随机查询未实现 |
| 订阅推送 | `SUB ON` 是随命令交互触发的轻量推送，**不是完整的异步广播**；高频订阅会受待写缓冲上限限制 |
| 多进程/多实例 | 未考虑同一记录文件被多个实例打开的情况 |
| 信号 | 只处理 `SIGINT`/`SIGTERM`，`SIGHUP` 重载配置未实现 |

---

## 7. 目录结构

```text
.
├── src/
│   ├── gateway.h        共享类型：Record / BoundedQueue / Hub / Config / 组件声明
│   ├── frame.cpp        CRC-16/MODBUS 查表实现、单调时钟
│   ├── source.cpp       串口 raw 模式配置 + 帧解析状态机 + 模拟源
│   ├── storage.cpp      定长记录落盘：攒批 + 周期 fsync
│   ├── tcp_server.cpp   epoll 非阻塞服务：半包缓冲 / 半关闭 / 慢消费者保护
│   └── main.cpp         参数解析、self-pipe 信号处理、流水线编排与退出顺序
├── tests/
│   ├── smoke_test.sh        功能与协议冒烟测试
│   ├── consistency_test.sh  三级流水线数据一致性对账
│   ├── verify_all.sh        一键全量校验
│   └── count_lines.sh       代码行数统计
├── Makefile
└── CMakeLists.txt
```

代码规模：C++ 合计 795 行（其中有效代码 618 行，其余为设计说明注释）。

---

## 8. License

MIT
