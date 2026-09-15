// tcp_server.cpp —— epoll 非阻塞 TCP 服务
//
// 关键设计：
//   1) 监听与连接套接字都设为非阻塞，accept 循环直到 EAGAIN；
//   2) 输入用"半包缓冲"累积，遇 '\n' 才切出一条命令，避免 TCP 粘包/拆包；
//   3) 输出用待写缓冲 + 动态 EPOLLOUT：慢消费者不会阻塞整个事件循环，
//      待写缓冲超过上限时直接断开该连接（而不是拖垮服务）；
//   4) epoll_wait 带 100 ms 超时，使 stop 标志能在 100 ms 内被感知。
#include "gateway.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <sstream>

static constexpr size_t kMaxOutBuffer = 256 * 1024;
static constexpr int    kMaxEvents   = 64;

std::string TcpServer::handle(const std::string& cmd, Conn& c) {
    std::istringstream is(cmd);
    std::string verb;
    is >> verb;

    if (verb == "STATS") {
        std::ostringstream os;
        os << "records=" << hub_.total()
           << " dropped=" << hub_.dropped()
           << " source=" << src_.mode()
           << " ch0=" << hub_.channelCount(0)
           << " ch1=" << hub_.channelCount(1)
           << " connections=1\n";
        return os.str();
    }
    if (verb == "LAST") {
        size_t n = 5;
        is >> n;
        if (n > 100) n = 100;
        const auto rows = hub_.last(n);
        std::ostringstream os;
        os << "count=" << rows.size() << "\n";
        char hex[3 * kRecordPayload + 1];
        for (const auto& r : rows) {
            for (size_t i = 0; i < r.len; ++i) std::snprintf(hex + i * 3, 4, "%02X ", r.payload[i]);
            hex[r.len * 3] = '\0';
            os << "seq=" << r.seq << " ch=" << static_cast<int>(r.ch)
               << " len=" << static_cast<int>(r.len) << " data=" << hex << "\n";
        }
        return os.str();
    }
    if (verb == "SUB") {
        std::string arg; is >> arg;
        c.subscribe = (arg == "ON");
        return std::string("sub=") + (c.subscribe ? "on" : "off") + "\n";
    }
    if (verb == "QUIT") return "";      // 空串表示关闭连接
    return "ERR unknown command; try STATS | LAST <n> | SUB ON|OFF | QUIT\n";
}

bool TcpServer::listenAndServe(std::atomic<bool>& stop) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd < 0) { std::perror("[tcp] socket"); return false; }

    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(cfg_.listen));
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("[tcp] bind"); ::close(lfd); return false;
    }
    if (::listen(lfd, 16) != 0) { std::perror("[tcp] listen"); ::close(lfd); return false; }
    std::fprintf(stderr, "[tcp] 监听 0.0.0.0:%d（nc 127.0.0.1 %d 试一下）\n",
                 cfg_.listen, cfg_.listen);

    const int ep = ::epoll_create1(0);
    if (ep < 0) { std::perror("[tcp] epoll_create1"); ::close(lfd); return false; }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = lfd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    std::vector<Conn> conns;

    auto closeConn = [&](size_t i) {
        ::epoll_ctl(ep, EPOLL_CTL_DEL, conns[i].fd, nullptr);
        ::close(conns[i].fd);
        conns.erase(conns.begin() + static_cast<long>(i));
    };

    while (!stop.load(std::memory_order_relaxed)) {
        epoll_event events[kMaxEvents];
        const int n = ::epoll_wait(ep, events, kMaxEvents, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("[tcp] epoll_wait");
            break;
        }

        for (int e = 0; e < n; ++e) {
            // ---- 新连接 ----
            if (events[e].data.fd == lfd) {
                for (;;) {
                    const int cfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (cfd < 0) break;                       // EAGAIN：没有更多待处理连接
                    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    Conn c{cfd, false, false, std::string(), std::string()};
                    c.out = "OK serial-data-gateway ready; try STATS\n";
                    conns.push_back(std::move(c));
                    epoll_event ce{};
                    ce.events  = EPOLLIN | EPOLLOUT;          // 有初始欢迎语要写
                    ce.data.fd = cfd;
                    ::epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ce);
                    std::fprintf(stderr, "[tcp] +conn fd=%d (共 %zu)\n", cfd, conns.size());
                }
                continue;
            }

            // ---- 已有连接 ----
            size_t i = 0;
            while (i < conns.size() && conns[i].fd != events[e].data.fd) ++i;
            if (i == conns.size()) continue;

            if (events[e].events & (EPOLLHUP | EPOLLERR)) { closeConn(i); continue; }

            if ((events[e].events & EPOLLIN) && !conns[i].read_closed) {
                char buf[1024];
                const ssize_t r = ::recv(conns[i].fd, buf, sizeof(buf), 0);
                if (r == 0) {
                    // 对端半关闭（如 `printf 'STATS\n' | nc`）：停止读，
                    // 但已生成的响应必须写完再关，否则客户端什么都收不到。
                    conns[i].read_closed = true;
                } else if (r < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) { closeConn(i); continue; }
                } else {
                    conns[i].in.append(buf, static_cast<size_t>(r));
                    size_t pos;
                    bool kill = false;
                    while ((pos = conns[i].in.find('\n')) != std::string::npos) {
                        std::string line = conns[i].in.substr(0, pos);
                        conns[i].in.erase(0, pos + 1);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        std::string resp = handle(line, conns[i]);
                        if (resp.empty() && line.rfind("QUIT", 0) == 0) { kill = true; break; }
                        conns[i].out += resp;
                    }
                    if (kill) { closeConn(i); continue; }
                    // 实时推送：订阅者每次收到命令处理后，附带最近 1 条
                    if (conns[i].subscribe) {
                        const auto rows = hub_.last(1);
                        if (!rows.empty()) {
                            char hex[3 * kRecordPayload + 1];
                            const auto& r0 = rows[0];
                            for (size_t k = 0; k < r0.len; ++k)
                                std::snprintf(hex + k * 3, 4, "%02X ", r0.payload[k]);
                            hex[r0.len * 3] = '\0';
                            char line[256];
                            std::snprintf(line, sizeof(line), "PUSH seq=%llu ch=%d data=%s\n",
                                          static_cast<unsigned long long>(r0.seq),
                                          static_cast<int>(r0.ch), hex);
                            conns[i].out += line;
                        }
                    }
                }
            }

            if (events[e].events & EPOLLOUT) {
                if (!conns[i].out.empty()) {
                    const ssize_t w = ::send(conns[i].fd, conns[i].out.data(),
                                             conns[i].out.size(), MSG_NOSIGNAL);
                    if (w > 0) conns[i].out.erase(0, static_cast<size_t>(w));
                    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        closeConn(i); continue;
                    }
                }
            }

            // 统一重算关注的事件：
            //   读完（半关闭）后不再关心 EPOLLIN；待写缓冲空则取消 EPOLLOUT，
            //   避免事件循环被空转唤醒。两者都没有时说明该连接可以关闭了。
            const uint32_t want =
                (conns[i].read_closed ? 0u : static_cast<uint32_t>(EPOLLIN)) |
                (conns[i].out.empty()   ? 0u : static_cast<uint32_t>(EPOLLOUT));
            if (want == 0) { closeConn(i); continue; }

            epoll_event ce{};
            ce.events  = want;
            ce.data.fd = conns[i].fd;
            ::epoll_ctl(ep, EPOLL_CTL_MOD, conns[i].fd, &ce);

            // 慢消费者保护：积压超过上限直接断开，不让它拖垮事件循环
            if (conns[i].out.size() > kMaxOutBuffer) {
                std::fprintf(stderr, "[tcp] 断开慢消费者 fd=%d\n", conns[i].fd);
                closeConn(i);
            }
        }
    }

    for (auto& c : conns) ::close(c.fd);
    ::close(ep);
    ::close(lfd);
    std::fprintf(stderr, "[tcp] 退出\n");
    return true;
}
