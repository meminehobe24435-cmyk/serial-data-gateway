/* mqtt.cpp — MQTT 3.1.1 客户端实现 */
#include "mqtt.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mqtt {

/* =============================================================== 工具 */
const char *packet_type_name(PacketType t) {
    switch (t) {
    case PacketType::CONNECT: return "CONNECT";
    case PacketType::CONNACK: return "CONNACK";
    case PacketType::PUBLISH: return "PUBLISH";
    case PacketType::PUBACK: return "PUBACK";
    case PacketType::PUBREC: return "PUBREC";
    case PacketType::PUBREL: return "PUBREL";
    case PacketType::PUBCOMP: return "PUBCOMP";
    case PacketType::SUBSCRIBE: return "SUBSCRIBE";
    case PacketType::SUBACK: return "SUBACK";
    case PacketType::UNSUBSCRIBE: return "UNSUBSCRIBE";
    case PacketType::UNSUBACK: return "UNSUBACK";
    case PacketType::PINGREQ: return "PINGREQ";
    case PacketType::PINGRESP: return "PINGRESP";
    case PacketType::DISCONNECT: return "DISCONNECT";
    }
    return "?";
}

const char *client_state_name(ClientState s) {
    switch (s) {
    case ClientState::DISCONNECTED: return "disconnected";
    case ClientState::CONNECTING: return "connecting";
    case ClientState::CONNECTED: return "connected";
    case ClientState::DISCONNECTING: return "disconnecting";
    case ClientState::ERROR: return "error";
    }
    return "?";
}

const char *ConnackResult::error_text() const {
    switch (return_code) {
    case 0: return "连接已接受";
    case 1: return "协议版本不支持";
    case 2: return "客户端标识符被拒绝";
    case 3: return "服务端不可用";
    case 4: return "用户名或密码错误";
    case 5: return "未授权";
    default: return "未知返回码";
    }
}

/* =============================================================== 变长编码
 * 规则：每字节低 7 位是数据，最高位（0x80）表示"后面还有字节"。
 * 最多 4 字节，因此最大可表示 268435455。
 */
std::vector<uint8_t> encode_remaining_length(uint32_t len) {
    std::vector<uint8_t> out;
    do {
        uint8_t b = static_cast<uint8_t>(len % 128);
        len /= 128;
        if (len > 0) b |= 0x80;
        out.push_back(b);
    } while (len > 0 && out.size() < 4);
    return out;
}

bool decode_remaining_length(const uint8_t *buf, size_t n, size_t offset,
                             uint32_t &len, size_t &consumed) {
    len = 0;
    consumed = 0;
    uint32_t mult = 1;
    for (size_t i = 0; i < 4; ++i) {
        if (offset + i >= n) return false;          /* 数据不够，等更多字节 */
        const uint8_t b = buf[offset + i];
        len += static_cast<uint32_t>(b & 0x7F) * mult;
        consumed++;
        if ((b & 0x80) == 0) return true;
        mult *= 128;
    }
    return false;                                    /* 超过 4 字节：非法 */
}

/* =============================================================== 编码辅助 */
static void put_u16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

/** MQTT 里的字符串 = 2 字节大端长度 + UTF-8 内容 */
static void put_string(std::vector<uint8_t> &v, const std::string &s) {
    put_u16(v, static_cast<uint16_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

static bool get_u16(const uint8_t *buf, size_t n, size_t &off, uint16_t &out) {
    if (off + 2 > n) return false;
    out = static_cast<uint16_t>((buf[off] << 8) | buf[off + 1]);
    off += 2;
    return true;
}

static bool get_string(const uint8_t *buf, size_t n, size_t &off, std::string &out) {
    uint16_t len = 0;
    if (!get_u16(buf, n, off, len)) return false;
    if (off + len > n) return false;
    out.assign(reinterpret_cast<const char *>(buf + off), len);
    off += len;
    return true;
}

/** 把"固定头 + 剩余长度 + 变长部分"拼成一个完整报文 */
static std::vector<uint8_t> assemble(PacketType type, uint8_t flags,
                                     const std::vector<uint8_t> &body) {
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | (flags & 0x0F)));
    const auto rl = encode_remaining_length(static_cast<uint32_t>(body.size()));
    pkt.insert(pkt.end(), rl.begin(), rl.end());
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

/* =============================================================== 报文构造 */
std::vector<uint8_t> build_connect(const std::string &client_id,
                                   const std::string &username,
                                   const std::string &password,
                                   uint16_t keep_alive_s, bool clean_session,
                                   const std::string &will_topic,
                                   const std::string &will_payload,
                                   QoS will_qos, bool will_retain) {
    std::vector<uint8_t> body;
    put_string(body, "MQTT");                       /* 协议名（固定） */
    body.push_back(4);                              /* 协议级别：3.1.1 = 4 */

    /* 连接标志：bit0 保留=0；bit1 clean session；bit2 will flag；
     *           bit3-4 will qos；bit5 will retain；bit6 password；bit7 username */
    uint8_t flags = 0;
    if (clean_session) flags |= 0x02;
    const bool has_will = !will_topic.empty();
    if (has_will) {
        flags |= 0x04;
        flags |= static_cast<uint8_t>((static_cast<uint8_t>(will_qos) & 0x03) << 3);
        if (will_retain) flags |= 0x20;
    }
    if (!password.empty()) flags |= 0x40;
    if (!username.empty()) flags |= 0x80;
    body.push_back(flags);

    put_u16(body, keep_alive_s);

    put_string(body, client_id);
    if (has_will) {
        put_string(body, will_topic);
        put_string(body, will_payload);
    }
    if (!username.empty()) put_string(body, username);
    if (!password.empty()) put_string(body, password);

    return assemble(PacketType::CONNECT, 0, body);
}

std::vector<uint8_t> build_publish(const PublishMessage &msg) {
    std::vector<uint8_t> body;
    put_string(body, msg.topic);
    const uint8_t q = static_cast<uint8_t>(msg.qos);
    if (q > 0) put_u16(body, msg.packet_id);
    body.insert(body.end(), msg.payload.begin(), msg.payload.end());

    uint8_t flags = static_cast<uint8_t>((q & 0x03) << 1);
    if (msg.retain) flags |= 0x01;
    if (msg.dup) flags |= 0x08;
    return assemble(PacketType::PUBLISH, flags, body);
}

std::vector<uint8_t> build_subscribe(uint16_t packet_id,
                                     const std::vector<std::string> &topics,
                                     QoS qos) {
    std::vector<uint8_t> body;
    put_u16(body, packet_id);
    for (const auto &t : topics) {
        put_string(body, t);
        body.push_back(static_cast<uint8_t>(qos));
    }
    /* SUBSCRIBE 必须把 flags 设成 0b0010（协议规定），否则服务端直接断连 */
    return assemble(PacketType::SUBSCRIBE, 0x02, body);
}

std::vector<uint8_t> build_unsubscribe(uint16_t packet_id,
                                       const std::vector<std::string> &topics) {
    std::vector<uint8_t> body;
    put_u16(body, packet_id);
    for (const auto &t : topics) put_string(body, t);
    return assemble(PacketType::UNSUBSCRIBE, 0x02, body);
}

std::vector<uint8_t> build_puback(uint16_t packet_id) {
    std::vector<uint8_t> body;
    put_u16(body, packet_id);
    return assemble(PacketType::PUBACK, 0, body);
}

std::vector<uint8_t> build_pingreq() {
    return assemble(PacketType::PINGREQ, 0, {});
}

std::vector<uint8_t> build_disconnect() {
    return assemble(PacketType::DISCONNECT, 0, {});
}

/* =============================================================== 解析 */
bool parse_packet(const uint8_t *buf, size_t n, ParsedPacket &out,
                  std::string *error) {
    auto fail = [&](const char *m) {
        if (error) *error = m;
        return false;
    };
    if (n < 2) return fail("数据不足一个报文头");

    const uint8_t b0 = buf[0];
    out.type = static_cast<PacketType>(b0 >> 4);
    out.flags = b0 & 0x0F;

    uint32_t rl = 0;
    size_t rl_bytes = 0;
    if (!decode_remaining_length(buf, n, 1, rl, rl_bytes)) {
        if (n < 5) return fail("剩余长度还没读全");
        return fail("剩余长度超过 4 字节（非法）");
    }
    const size_t hdr = 1 + rl_bytes;
    if (n < hdr + rl) return fail("报文体还没收全");
    out.remaining_len = rl;
    out.consumed = hdr + rl;

    const uint8_t *body = buf + hdr;
    size_t off = 0;

    switch (out.type) {
    case PacketType::CONNACK: {
        if (rl < 2) return fail("CONNACK 长度不足");
        out.connack.session_present = (body[0] & 0x01) != 0;
        out.connack.return_code = body[1];
        break;
    }
    case PacketType::PUBLISH: {
        if (!get_string(body, rl, off, out.publish.topic)) return fail("PUBLISH 主题解析失败");
        const uint8_t q = (out.flags >> 1) & 0x03;
        if (q > 2) return fail("PUBLISH QoS 非法");
        out.publish.qos = static_cast<QoS>(q);
        out.publish.retain = (out.flags & 0x01) != 0;
        out.publish.dup = (out.flags & 0x08) != 0;
        if (q > 0) {
            if (!get_u16(body, rl, off, out.publish.packet_id))
                return fail("PUBLISH PacketId 缺失");
        }
        out.publish.payload.assign(reinterpret_cast<const char *>(body + off), rl - off);
        break;
    }
    case PacketType::PUBACK:
    case PacketType::PUBREC:
    case PacketType::PUBREL:
    case PacketType::PUBCOMP:
    case PacketType::UNSUBACK: {
        if (!get_u16(body, rl, off, out.packet_id)) return fail("PacketId 缺失");
        break;
    }
    case PacketType::SUBACK: {
        if (!get_u16(body, rl, off, out.packet_id)) return fail("SUBACK PacketId 缺失");
        out.suback_codes.assign(body + off, body + rl);
        break;
    }
    case PacketType::PINGRESP:
    case PacketType::PINGREQ:
    case PacketType::DISCONNECT:
        break;                                       /* 无报文体 */
    default:
        break;                                       /* SUBSCRIBE 等由服务端发出时才解析 */
    }
    return true;
}

/* =============================================================== MemoryTransport */
MemoryTransport::MemoryTransport(bool auto_connack, bool auto_pingresp)
    : auto_connack_(auto_connack), auto_pingresp_(auto_pingresp) {}

bool MemoryTransport::connect(const std::string &, uint16_t, int) {
    connected_ = true;
    return true;
}

void MemoryTransport::close() { connected_ = false; }

void MemoryTransport::queue_incoming(const std::vector<uint8_t> &bytes) {
    rx_.insert(rx_.end(), bytes.begin(), bytes.end());
}

bool MemoryTransport::write(const uint8_t *data, size_t n) {
    if (!connected_) return false;
    written_.emplace_back(data, data + n);

    /* 自动应答：CONNECT -> CONNACK，PINGREQ -> PINGRESP */
    if (n >= 2) {
        const PacketType t = static_cast<PacketType>(data[0] >> 4);
        if (t == PacketType::CONNECT && auto_connack_) {
            /* CONNACK: 0x20 0x02 <session> <code> */
            queue_incoming({0x20, 0x02, 0x00, connack_code_});
        } else if (t == PacketType::PINGREQ && auto_pingresp_) {
            queue_incoming({0xD0, 0x00});
        }
    }
    return true;
}

size_t MemoryTransport::read(uint8_t *data, size_t n, int) {
    if (!connected_ || rx_.empty()) return 0;
    const size_t take = std::min(n, rx_.size());
    std::memcpy(data, rx_.data(), take);
    rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(take));
    return take;
}

/* =============================================================== Client */
Client::Client(MqttTransport &transport, const ClientConfig &cfg)
    : tp_(transport), cfg_(cfg) {
    if (cfg_.connect_timeout_ms == 0) cfg_.connect_timeout_ms = 5000;
}

bool Client::connect(std::string *error) {
    auto fail = [&](const std::string &m) {
        set_error(m);
        if (error) *error = m;
        return false;
    };

    state_ = ClientState::CONNECTING;
    stats_.connect_attempts++;

    if (!tp_.connect(cfg_.host, cfg_.port, static_cast<int>(cfg_.connect_timeout_ms)))
        return fail("TCP 连接失败：" + cfg_.host + ":" + std::to_string(cfg_.port));

    const auto pkt = build_connect(cfg_.client_id, cfg_.username, cfg_.password,
                                   cfg_.keep_alive_s, cfg_.clean_session,
                                   cfg_.will_topic, cfg_.will_payload);
    if (!tp_.write(pkt.data(), pkt.size())) return fail("CONNECT 发送失败");
    stats_.packets_out++;

    /* 等 CONNACK（带超时） */
    const uint32_t deadline_ms = cfg_.connect_timeout_ms;
    uint32_t waited = 0;
    const uint32_t step = 50;
    while (waited <= deadline_ms) {
        uint8_t buf[256];
        const size_t got = tp_.read(buf, sizeof(buf), static_cast<int>(step));
        if (got > 0) {
            rx_.insert(rx_.end(), buf, buf + got);
            ParsedPacket pp;
            std::string perr;
            if (parse_packet(rx_.data(), rx_.size(), pp, &perr)) {
                rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(pp.consumed));
                stats_.packets_in++;
                if (pp.type != PacketType::CONNACK)
                    return fail("期待 CONNACK，收到 " + std::string(packet_type_name(pp.type)));
                if (!pp.connack.ok())
                    return fail("服务端拒绝连接：" + std::string(pp.connack.error_text()));
                state_ = ClientState::CONNECTED;
                stats_.connects_ok++;
                if (stats_.connects_ok > 1) stats_.reconnects++;
                since_ping_ms_ = 0;
                return true;
            }
        }
        waited += step;
    }
    return fail("等待 CONNACK 超时（" + std::to_string(deadline_ms) + " ms）");
}

void Client::disconnect() {
    if (state_ == ClientState::CONNECTED) {
        const auto pkt = build_disconnect();
        tp_.write(pkt.data(), pkt.size());
        stats_.packets_out++;
    }
    state_ = ClientState::DISCONNECTING;
    tp_.close();
    state_ = ClientState::DISCONNECTED;
    subscribed_ = false;
}

bool Client::send_packet(const std::vector<uint8_t> &pkt) {
    if (state_ != ClientState::CONNECTED) {
        set_error("未连接，无法发送");
        return false;
    }
    if (!tp_.connected()) {
        state_ = ClientState::ERROR;
        set_error("链路已断开");
        return false;
    }
    if (!tp_.write(pkt.data(), pkt.size())) {
        stats_.errors++;
        set_error("发送失败");
        return false;
    }
    stats_.packets_out++;
    return true;
}

uint16_t Client::next_packet_id() {
    packet_id_++;
    if (packet_id_ == 0) packet_id_ = 1;         /* 0 是保留值 */
    return packet_id_;
}

bool Client::publish(const std::string &topic, const std::string &payload,
                     QoS qos, bool retain) {
    PublishMessage m;
    m.topic = topic;
    m.payload = payload;
    m.qos = qos;
    m.retain = retain;
    if (qos != QoS::AT_MOST_ONCE) m.packet_id = next_packet_id();

    const auto pkt = build_publish(m);
    if (!send_packet(pkt)) return false;
    stats_.publishes++;
    stats_.publish_bytes += payload.size();
    return true;
}

bool Client::subscribe(const std::string &topic, QoS qos) {
    const auto pkt = build_subscribe(next_packet_id(), {topic}, qos);
    if (!send_packet(pkt)) return false;
    subscribed_ = true;
    return true;
}

bool Client::unsubscribe(const std::string &topic) {
    const auto pkt = build_unsubscribe(next_packet_id(), {topic});
    return send_packet(pkt);
}

int Client::poll(int timeout_ms) {
    uint8_t buf[1024];
    const size_t got = tp_.read(buf, sizeof(buf), timeout_ms);
    if (got == 0) {
        if (!tp_.connected() && state_ == ClientState::CONNECTED) {
            state_ = ClientState::ERROR;
            set_error("对端关闭连接");
        }
        return 0;
    }
    rx_.insert(rx_.end(), buf, buf + got);

    int handled = 0;
    while (true) {
        ParsedPacket pp;
        std::string perr;
        if (!parse_packet(rx_.data(), rx_.size(), pp, &perr)) break;
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(pp.consumed));
        stats_.packets_in++;
        handled++;

        switch (pp.type) {
        case PacketType::PUBLISH:
            if (on_message_) on_message_(pp.publish);
            if (pp.publish.qos == QoS::AT_LEAST_ONCE) {
                const auto ack = build_puback(pp.publish.packet_id);
                if (tp_.write(ack.data(), ack.size())) stats_.packets_out++;
            }
            break;
        case PacketType::PINGRESP:
            break;
        default:
            break;
        }
    }
    return handled;
}

void Client::tick(uint32_t elapsed_ms) {
    if (state_ != ClientState::CONNECTED) return;
    since_ping_ms_ += elapsed_ms;
    if (cfg_.ping_interval_ms > 0 && since_ping_ms_ >= cfg_.ping_interval_ms) {
        const auto pkt = build_pingreq();
        if (send_packet(pkt)) {
            stats_.pings++;
            since_ping_ms_ = 0;
        }
    }
}

void Client::set_error(const std::string &e) {
    last_error_ = e;
    stats_.errors++;
}

}  // namespace mqtt
