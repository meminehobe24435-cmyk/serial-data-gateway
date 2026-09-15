// storage.cpp —— 落盘线程
//
// 设计要点：
//   1) 定长记录（sizeof(Record)）顺序追加，回读时可直接按索引定位；
//   2) 攒批写入（攒够 64 条或距上次写入超过 200 ms 才 write），降低写放大；
//   3) 按 fsync_ms 周期做一次 fsync，把"掉电丢失窗口"限制在可控范围内；
//   4) 接收侧与落盘侧是两个线程，磁盘抖动不会阻塞采集。
#include "gateway.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

bool Storage::open() {
    fd_ = ::open(cfg_.data_file.c_str(),
                 O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) { std::perror("[storage] open"); return false; }
    std::fprintf(stderr, "[storage] 记录文件 %s（定长 %zu 字节/条）\n",
                 cfg_.data_file.c_str(), sizeof(Record));
    return true;
}

void Storage::run(BoundedQueue<Record>& in, std::atomic<bool>& stop) {
    (void)stop;   // 退出由队列关闭驱动，而不是由 stop 标志提前打断
    std::vector<Record> batch;
    batch.reserve(64);
    uint64_t last_write = now_ms();
    uint64_t last_sync  = last_write;

    auto flush = [&](bool do_sync) {
        if (batch.empty()) return;
        const size_t bytes = batch.size() * sizeof(Record);
        ssize_t w = ::write(fd_, batch.data(), bytes);
        if (w < 0) std::perror("[storage] write");
        written_ += batch.size();
        batch.clear();
        if (do_sync) {
            if (::fsync(fd_) != 0) std::perror("[storage] fsync");
            last_sync = now_ms();
        }
        last_write = now_ms();
    };

    for (;;) {
        Record r{};
        // 带超时取出：队列为空时最多等 100 ms 就返回，
        // 这样即便长时间没有新数据，fsync/攒批的周期判断也不会被饿死。
        const bool got = in.popFor(r, 100);
        if (got) {
            batch.push_back(r);
        } else if (in.isClosed()) {
            break;                 // 队列已关闭且排空 -> 收工
        }

        const uint64_t t = now_ms();
        const bool by_count = batch.size() >= 64;
        const bool by_time  = !batch.empty() && (t - last_write) >= 200;
        if (by_count || by_time)
            flush((t - last_sync) >= static_cast<uint64_t>(cfg_.fsync_ms));
    }
    flush(true);   // 退出前必须把尾部缓冲写掉，否则会丢最后一批数据
    if (fd_ >= 0) ::close(fd_);
    std::fprintf(stderr, "[storage] 退出：累计写入 %llu 条\n",
                 static_cast<unsigned long long>(written_));
}
