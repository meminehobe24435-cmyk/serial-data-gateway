// gateway.h —— 共享类型与各组件接口
//
// 数据流（单生产者、单消费者，各阶段解耦）：
//
//   [串口/模拟源] --Source--> raw_q --Dispatcher--> Hub  --> TcpServer (LAST/STATS/SUB)
//                                                \--> store_q --> Storage (落盘+fsync)
//
// 为什么不让 Storage 和 TcpServer 直接竞争同一个队列：
// BoundedQueue 是单消费者语义，两个消费者会互相"偷"记录。
// 因此引入 Dispatcher 作为唯一消费者，再把记录分发到落盘队列与内存 Hub。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- 数据记录
// 定长载荷（64B）便于按固定步长落盘与回读，避免变长记录的解析开销。
struct Record {
    uint64_t seq;           // 全局递增序号，用于查漏
    uint64_t ts_ms;         // 接收时刻（monotonic，毫秒）
    uint8_t  ch;            // 通道号
    uint8_t  len;           // 有效载荷长度（<= 64）
    uint8_t  payload[64];
};

static constexpr size_t kRecordPayload = 64;
static constexpr size_t kHubCapacity   = 512;   // Hub 环形缓存条数

// CRC-16/MODBUS：poly=0xA001（反射 0x8005）、init=0xFFFF、无 xorout
uint16_t crc16_modbus(const uint8_t* data, size_t n);
uint64_t now_ms();

// ------------------------------------------------------- 有界队列（背压策略）
// 采集侧绝不能被阻塞，因此入队满时丢弃最旧记录并计数（drop-oldest），
// 而不是阻塞生产者。丢弃数是可观测指标，由 STATS 命令读出。
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : cap_(capacity) {}

    void push(T value) {
        std::unique_lock<std::mutex> lk(m_);
        if (q_.size() >= cap_) { q_.pop_front(); ++dropped_; }
        q_.push_back(std::move(value));
        cv_.notify_one();
    }

    // 返回 false 表示队列已关闭且已排空，消费者应退出
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return !q_.empty() || closed_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    // 带超时的取出：超时返回 false，但队列并未关闭。
    // 供"既要攒批落盘、又要有周期性 fsync"的消费者使用，
    // 避免用 size()+sleep 轮询（那样会在空闲时白占 CPU，且丢精度）。
    bool popFor(T& out, int timeout_ms) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [this] { return !q_.empty() || closed_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        cv_.notify_all();
    }

    bool isClosed() const { std::lock_guard<std::mutex> lk(m_); return closed_; }
    size_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    size_t size()    const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }

private:
    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::deque<T>           q_;
    size_t                  cap_;
    size_t                  dropped_ = 0;
    bool                    closed_  = false;
};

// ------------------------------------------------------------------ 内存 Hub
// 保存最近 N 条记录与累计计数，供 TCP 侧查询；由 Dispatcher 单线程写入。
class Hub {
public:
    void add(const Record& r) {
        std::lock_guard<std::mutex> lk(m_);
        if (recent_.size() >= kHubCapacity) recent_.pop_front();
        recent_.push_back(r);
        ++total_;
        by_ch_[r.ch & 0x07] += 1;
    }
    void setDropped(size_t d) { std::lock_guard<std::mutex> lk(m_); dropped_ = d; }

    uint64_t total()   const { std::lock_guard<std::mutex> lk(m_); return total_; }
    size_t   dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    std::vector<Record> last(size_t n) const {
        std::lock_guard<std::mutex> lk(m_);
        const size_t k = n < recent_.size() ? n : recent_.size();
        return std::vector<Record>(recent_.end() - k, recent_.end());
    }
    uint64_t channelCount(size_t ch) const {
        std::lock_guard<std::mutex> lk(m_);
        return ch < 8 ? by_ch_[ch] : 0;
    }

private:
    mutable std::mutex   m_;
    std::deque<Record>   recent_;
    uint64_t             total_ = 0;
    size_t               dropped_ = 0;
    uint64_t             by_ch_[8] = {0};
};

// ---------------------------------------------------------------- 运行时配置
struct Config {
    std::string port   = "";              // 串口设备；空则启用模拟源
    int         baud   = 115200;
    int         listen = 9000;            // TCP 监听端口
    size_t      queue_capacity = 4096;    // 队列深度
    std::string data_file = "records.bin";// 定长记录文件
    int         fsync_ms  = 1000;         // 落盘 fsync 周期
    bool        simulate  = false;
};

// ------------------------------------------------------------------ 采集源
// 打开串口并配置为 raw 模式；若未指定串口或打开失败，则用内置波形源，
// 保证项目在没有任何硬件的机器上也能完整跑通。
class Source {
public:
    explicit Source(const Config& cfg) : cfg_(cfg) {}
    // 阻塞运行，直到 stop 被置位；每解析出一帧即推入队列
    void run(BoundedQueue<Record>& out, std::atomic<bool>& stop);
    const char* mode() const { return simulated_ ? "simulated" : "serial"; }
private:
    bool openSerial();
    ssize_t readBytes(uint8_t* buf, size_t n);
    Config cfg_;
    int    fd_ = -1;
    bool   simulated_ = false;
    std::atomic<uint64_t> seq_{0};
};

// -------------------------------------------------------------------- 存储
class Storage {
public:
    explicit Storage(const Config& cfg) : cfg_(cfg) {}
    bool open();                                          // 打开/追加定长记录文件
    void run(BoundedQueue<Record>& in, std::atomic<bool>& stop);
    uint64_t written() const { return written_; }
private:
    Config   cfg_;
    int      fd_ = -1;
    uint64_t written_ = 0;
};

// ---------------------------------------------------------------- 分发线程
void dispatch(BoundedQueue<Record>& raw, BoundedQueue<Record>& store,
              Hub& hub, std::atomic<bool>& stop);

// ---------------------------------------------------------------- TCP 服务
// epoll + 非阻塞 socket。命令为 ASCII 行，便于用 nc / telnet 手工验证：
//   STATS            运行统计
//   LAST <n>         最近 n 条记录的十六进制摘要
//   SUB ON|OFF       开启/关闭实时推送
//   QUIT             断开
class TcpServer {
public:
    TcpServer(const Config& cfg, Hub& hub, const Source& src)
        : cfg_(cfg), hub_(hub), src_(src) {}
    // 在调用者线程中运行 epoll 循环，直到 stop 被置位
    bool listenAndServe(std::atomic<bool>& stop);
private:
    struct Conn {
        int         fd;
        bool        subscribe   = false;
        bool        read_closed = false;  // 对端已 shutdown(WR)，仍需把 out 写完
        std::string in;    // 半包缓冲
        std::string out;   // 待写缓冲（慢消费者保护）
    };
    std::string handle(const std::string& cmd, Conn& c);
    Config       cfg_;
    Hub&         hub_;
    const Source& src_;
};
