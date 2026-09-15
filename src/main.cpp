// main.cpp —— 进程编排、优雅退出与分发线程
//
// 信号处理采用经典 "self-pipe trick"：
//   信号处理器只做一件事 —— 往管道写一个字节（异步信号安全），
//   主线程在 epoll/等待侧感知到可读后，再执行复杂逻辑。
// 这样避免了在信号处理器里调用非异步信号安全函数（如 printf/malloc）。
#include "gateway.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_sig_pipe[2] = {-1, -1};

static void onSignal(int sig) {
    const uint8_t b = static_cast<uint8_t>(sig);
    ssize_t n = ::write(g_sig_pipe[1], &b, 1);
    (void)n;   // 唯一允许在信号处理器中做的事
}

// 唯一消费者：把原始队列里的记录同时送到落盘队列与内存 Hub。
//
// 注意退出条件：这里**不能**写成 `while (!stop)`。stop 只表示"信号已到达"，
// 此刻队列里可能还压着未处理的记录，提前跳出就会丢数据。
// 正确做法是让循环一直排空到队列被 close() 且为空（pop 返回 false）。
void dispatch(BoundedQueue<Record>& raw, BoundedQueue<Record>& store,
              Hub& hub, std::atomic<bool>& stop) {
    (void)stop;
    uint64_t count = 0;
    Record r{};
    while (raw.pop(r)) {          // 排空后才返回 false
        hub.add(r);
        store.push(r);
        ++count;
    }
    hub.setDropped(raw.dropped());
    std::fprintf(stderr, "[dispatch] 退出：分发 %llu 条，队列丢弃 %zu 条\n",
                 static_cast<unsigned long long>(count), raw.dropped());
}

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "用法: %s [选项]\n"
        "  --port <dev>     串口设备，如 /dev/ttyUSB0（缺省用模拟源）\n"
        "  --baud <n>       波特率，默认 115200\n"
        "  --listen <n>     TCP 监听端口，默认 9000\n"
        "  --queue <n>      队列深度，默认 4096\n"
        "  --file <path>    落盘文件，默认 records.bin\n"
        "  --fsync <ms>     fsync 周期，默认 1000\n"
        "  --simulate       强制使用模拟源\n"
        "示例: %s --simulate --listen 9000\n", argv0, argv0);
}

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s 缺少参数\n", name); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--port")     cfg.port = next("--port");
        else if (a == "--baud")     cfg.baud = std::atoi(next("--baud").c_str());
        else if (a == "--listen")   cfg.listen = std::atoi(next("--listen").c_str());
        else if (a == "--queue")    cfg.queue_capacity = std::strtoul(next("--queue").c_str(), nullptr, 10);
        else if (a == "--file")     cfg.data_file = next("--file");
        else if (a == "--fsync")    cfg.fsync_ms = std::atoi(next("--fsync").c_str());
        else if (a == "--simulate") cfg.simulate = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    // self-pipe：先于信号注册创建，保证处理器里写入的 fd 一定有效
    if (::pipe(g_sig_pipe) != 0) { std::perror("pipe"); return 1; }
    ::fcntl(g_sig_pipe[1], F_SETFL, O_NONBLOCK);   // 管道满时不阻塞处理器

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);                    // 写已关闭 socket 返回 EPIPE 而非杀进程

    std::atomic<bool> stop{false};

    BoundedQueue<Record> raw_q(cfg.queue_capacity);
    BoundedQueue<Record> store_q(cfg.queue_capacity);
    Hub          hub;
    Source       source(cfg);
    Storage      storage(cfg);
    TcpServer    server(cfg, hub, source);

    if (!storage.open()) return 1;

    std::fprintf(stderr,
        "[main] serial-data-gateway 启动：队列 %zu 条，fsync %d ms\n",
        cfg.queue_capacity, cfg.fsync_ms);

    std::thread t_source([&] { source.run(raw_q, stop); });
    std::thread t_store ([&] { storage.run(store_q, stop); });
    std::thread t_disp  ([&] { dispatch(raw_q, store_q, hub, stop); });

    // 信号线程只负责置位；真正的收尾顺序交给主线程，
    // 避免在信号路径里做 join/close 这类可能阻塞的操作。
    std::thread t_signal([&] {
        uint8_t b = 0;
        while (::read(g_sig_pipe[0], &b, 1) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            usleep(100000);
        }
        std::fprintf(stderr, "\n[main] 收到信号 %d，开始优雅退出…\n", static_cast<int>(b));
        stop = true;
    });

    server.listenAndServe(stop);

    // ---- 优雅退出：必须按"生产者 -> 中间层 -> 消费者"的顺序逐级收尾 ----
    // 顺序错了就会丢数据：若先关 store_q，dispatcher 后续 push 的记录将无人消费。
    stop = true;                                   // 1. 通知采集线程停止产生
    if (t_source.joinable()) t_source.join();      // 2. 生产者已停，raw_q 不再增长
    raw_q.close();                                 // 3. 通知 dispatcher：排空后退出
    if (t_disp.joinable())   t_disp.join();        // 4. 分发完成，store_q 不再增长
    store_q.close();                               // 5. 通知 storage：排空后退出
    if (t_store.joinable())  t_store.join();       // 6. 落盘线程把尾部缓冲 flush 掉
    if (t_signal.joinable()) t_signal.detach();

    std::fprintf(stderr, "[main] 已退出，落盘 %llu 条\n",
                 static_cast<unsigned long long>(storage.written()));
    return 0;
}
