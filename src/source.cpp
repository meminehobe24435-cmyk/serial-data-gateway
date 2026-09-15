// source.cpp —— 串口采集线程（含无硬件时的模拟源）
//
// 帧格式（与固件侧约定）：AA 55 | LEN(1) | CH(1) | PAYLOAD(LEN) | CRC16(2, 小端)
// CRC 覆盖 LEN..PAYLOAD 全部字节。解析采用状态机 + 失步重同步：
// 任一字节校验失败都回到找帧头状态，避免一次丢字节导致后续帧永久错位。
#include "gateway.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

static constexpr uint8_t kHead0 = 0xAA;
static constexpr uint8_t kHead1 = 0x55;

static speed_t toSpeed(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 460800: return B460800;
        default:     return B115200;
    }
}

bool Source::openSerial() {
    fd_ = ::open(cfg_.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) { ::close(fd_); fd_ = -1; return false; }
    cfmakeraw(&tty);                                   // 关闭回显/行缓冲/信号
    cfsetispeed(&tty, toSpeed(cfg_.baud));
    cfsetospeed(&tty, toSpeed(cfg_.baud));
    tty.c_cflag |= (CLOCAL | CREAD);                   // 忽略调制解调器控制线
    tty.c_cflag &= ~CRTSCTS;                           // 无硬件流控
    tty.c_cc[VMIN] = 0;                                // 配合 poll 做非阻塞读
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSANOW, &tty) != 0) { ::close(fd_); fd_ = -1; return false; }
    tcflush(fd_, TCIOFLUSH);
    return true;
}

ssize_t Source::readBytes(uint8_t* buf, size_t n) {
    if (fd_ >= 0) return ::read(fd_, buf, n);

    // 模拟源：每 20 ms 构造一帧**完整**的线上字节流
    // （AA 55 | LEN | CH | PAYLOAD | CRC16 小端），
    // 让模拟路径与真实串口走同一条解析状态机，而不是绕开校验。
    usleep(20000);
    static uint32_t phase = 0;

    uint8_t payload[kRecordPayload];
    const uint8_t len = static_cast<uint8_t>(8 + (phase % 8));
    for (uint8_t i = 0; i < len; ++i)
        payload[i] = static_cast<uint8_t>(128 + 100 * ((phase + i) % 32) / 32.0);

    uint8_t crcIn[kRecordPayload + 2] = {len, static_cast<uint8_t>(phase % 2)};
    std::memcpy(crcIn + 2, payload, len);
    const uint16_t crc = crc16_modbus(crcIn, len + 2);

    const size_t total = 3 + 1 + len + 2;      // 头2 + LEN + CH + 载荷 + CRC2
    if (total > n) return 0;

    size_t k = 0;
    buf[k++] = kHead0;
    buf[k++] = kHead1;
    buf[k++] = len;
    buf[k++] = crcIn[1];                       // CH
    std::memcpy(buf + k, payload, len);
    k += len;
    buf[k++] = static_cast<uint8_t>(crc & 0xFF);
    buf[k++] = static_cast<uint8_t>(crc >> 8);
    ++phase;
    return static_cast<ssize_t>(k);
}

void Source::run(BoundedQueue<Record>& out, std::atomic<bool>& stop) {
    simulated_ = cfg_.simulate || !openSerial();
    if (simulated_)
        std::fprintf(stderr, "[source] %s 模式（未使用真实串口）\n",
                     "simulated");
    else
        std::fprintf(stderr, "[source] 已打开 %s @ %d 8N1\n",
                     cfg_.port.c_str(), cfg_.baud);

    // 单字节模式的状态机：等长度 -> 等通道 -> 收载荷 -> 验 CRC
    enum { H0, H1, LEN, CH, BODY, CRC0, CRC1 } st = H0;
    uint8_t  body[kRecordPayload]{};
    uint8_t  bodyLen = 0, bodyGot = 0, ch = 0, crcLo = 0;
    uint64_t frames = 0, bad = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        uint8_t buf[256];
        ssize_t n = readBytes(buf, sizeof(buf));
        if (n <= 0) continue;

        for (ssize_t i = 0; i < n; ++i) {
            const uint8_t b = buf[i];
            switch (st) {
                case H0: if (b == kHead0) st = H1; break;
                case H1: st = (b == kHead1) ? LEN : H0; break;
                case LEN:
                    if (b == 0 || b > kRecordPayload) { st = H0; break; }
                    bodyLen = b; bodyGot = 0; st = CH; break;
                case CH: ch = b; st = BODY; break;
                case BODY:
                    body[bodyGot++] = b;
                    if (bodyGot == bodyLen) st = CRC0;
                    break;
                case CRC0: crcLo = b; st = CRC1; break;
                case CRC1: {
                    // 校验范围 = LEN, CH, PAYLOAD
                    uint8_t tmp[kRecordPayload + 2] = {bodyLen, ch};
                    std::memcpy(tmp + 2, body, bodyLen);
                    const uint16_t want =
                        static_cast<uint16_t>(crcLo | (b << 8));
                    if (crc16_modbus(tmp, bodyLen + 2) == want) {
                        Record r{};
                        r.seq    = ++seq_;
                        r.ts_ms  = now_ms();
                        r.ch     = ch;
                        r.len    = bodyLen;
                        std::memcpy(r.payload, body, bodyLen);
                        out.push(r);
                        ++frames;
                    } else {
                        ++bad;
                    }
                    st = H0;
                    break;
                }
            }
        }
    }
    if (fd_ >= 0) ::close(fd_);
    std::fprintf(stderr, "[source] 退出：解析 %llu 帧，CRC 失败 %llu 次\n",
                 static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(bad));
}
