/*
 * mqtt.h — MQTT 3.1.1 客户端（报文编解码 + 连接/发布/订阅/保活状态机）
 *
 * 为什么自己写而不是引 paho：
 *   网关要跑在 ARM 板子上，交叉编译第三方库是个麻烦事；
 *   而 MQTT 3.1.1 的报文格式并不复杂，自己写一份零依赖的实现，
 *   反而更容易裁剪（只留 QoS0/1）与排查（报文字节可控）。
 *
 * 几个必须对的细节（错一个就连不上）：
 *   · **剩余长度是变长编码**：每字节用低 7 位，最高位表示"后面还有"
 *     —— 这是 MQTT 最经典的坑，128 字节以下看不出来，一大就出事
 *   · 主题名是 **UTF-8 前缀长度**（2 字节大端长度 + 内容），不是 C 字符串
 *   · CONNECT 的**连接标志位**：用户名/密码/遗嘱各占一位，顺序不能错
 *   · PUBLISH 的 QoS 在**固定头低 4 位**里（bit1-2），QoS>0 才带 PacketId
 *
 * C++17，零第三方依赖。
 */
#ifndef GATEWAY_MQTT_H
#define GATEWAY_MQTT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mqtt {

/* ---------------------------------------------------------------- 报文类型 */
enum class PacketType : uint8_t {
    CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4,
    PUBREC = 5, PUBREL = 6, PUBCOMP = 7, SUBSCRIBE = 8,
    SUBACK = 9, UNSUBSCRIBE = 10, UNSUBACK = 11,
    PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14,
};

const char *packet_type_name(PacketType t);

enum class QoS : uint8_t { AT_MOST_ONCE = 0, AT_LEAST_ONCE = 1, EXACTLY_ONCE = 2 };

/* ---------------------------------------------------------------- 变长编码 */
/* 这是 MQTT 最容易写错的地方，单独暴露出来便于单测。 */
std::vector<uint8_t> encode_remaining_length(uint32_t len);
/* 解析：从 buf[offset] 开始读；成功返回 true 并填 len 与 consumed */
bool decode_remaining_length(const uint8_t *buf, size_t n, size_t offset,
                             uint32_t &len, size_t &consumed);

/* ---------------------------------------------------------------- 报文结构 */
struct PublishMessage {
    std::string topic;
    std::string payload;
    QoS qos = QoS::AT_MOST_ONCE;
    bool retain = false;
    bool dup = false;
    uint16_t packet_id = 0;      // 仅 QoS > 0 有效
};

struct ConnackResult {
    bool session_present = false;
    uint8_t return_code = 0;
    bool ok() const { return return_code == 0; }
    const char *error_text() const;
};

/* ---------------------------------------------------------------- 报文编码 */
std::vector<uint8_t> build_connect(const std::string &client_id,
                                   const std::string &username = "",
                                   const std::string &password = "",
                                   uint16_t keep_alive_s = 60,
                                   bool clean_session = true,
                                   const std::string &will_topic = "",
                                   const std::string &will_payload = "",
                                   QoS will_qos = QoS::AT_MOST_ONCE,
                                   bool will_retain = false);

std::vector<uint8_t> build_publish(const PublishMessage &msg);
std::vector<uint8_t> build_subscribe(uint16_t packet_id,
                                     const std::vector<std::string> &topics,
                                     QoS qos = QoS::AT_MOST_ONCE);
std::vector<uint8_t> build_unsubscribe(uint16_t packet_id,
                                       const std::vector<std::string> &topics);
std::vector<uint8_t> build_puback(uint16_t packet_id);
std::vector<uint8_t> build_pingreq();
std::vector<uint8_t> build_disconnect();

/* ---------------------------------------------------------------- 报文解析 */
struct ParsedPacket {
    PacketType type = PacketType::CONNACK;
    uint8_t flags = 0;
    uint32_t remaining_len = 0;
    ConnackResult connack;
    PublishMessage publish;      // type == PUBLISH 时有效
    std::vector<uint8_t> suback_codes;   // type == SUBACK 时有效
    uint16_t packet_id = 0;      // PUBACK/SUBACK 等带的 packet id
    size_t consumed = 0;         // 本包在缓冲里占了多少字节
};

/* 从字节流解析一个完整报文；数据不足返回 false（consumed 无意义） */
bool parse_packet(const uint8_t *buf, size_t n, ParsedPacket &out,
                  std::string *error = nullptr);

/* ---------------------------------------------------------------- 传输抽象
 * 与 mqtt 协议解耦，便于单测与换实现（本机 socket / TLS / 串口透传模块）
 */
class MqttTransport {
public:
    virtual ~MqttTransport() = default;
    virtual bool connect(const std::string &host, uint16_t port,
                         int timeout_ms) = 0;
    virtual void close() = 0;
    virtual bool connected() const = 0;
    /** 写全部字节；返回是否成功 */
    virtual bool write(const uint8_t *data, size_t n) = 0;
    /** 读最多 n 字节；返回实际读到的字节数（0 表示暂无数据/超时） */
    virtual size_t read(uint8_t *data, size_t n, int timeout_ms) = 0;
    virtual std::string name() const = 0;
};

/** 内存传输：把"服务端"响应预先排好，用于单测（毫秒级跑完整个握手） */
class MemoryTransport : public MqttTransport {
public:
    explicit MemoryTransport(bool auto_connack = true, bool auto_pingresp = true);

    bool connect(const std::string &host, uint16_t port, int timeout_ms) override;
    void close() override;
    bool connected() const override { return connected_; }
    bool write(const uint8_t *data, size_t n) override;
    size_t read(uint8_t *data, size_t n, int timeout_ms) override;
    std::string name() const override { return "memory"; }

    /** 写出去的字节（测试断言用） */
    const std::vector<std::vector<uint8_t>> &written() const { return written_; }
    /** 手工排入一段"服务端发来的"数据 */
    void queue_incoming(const std::vector<uint8_t> &bytes);
    /** 模拟服务端断开 */
    void force_close() { connected_ = false; }
    /** 是否自动回 CONNACK / PINGRESP */
    void set_auto_reply(bool connack, bool pingresp) {
        auto_connack_ = connack;
        auto_pingresp_ = pingresp;
    }
    void set_connack_code(uint8_t code) { connack_code_ = code; }

private:
    bool connected_ = false;
    bool auto_connack_;
    bool auto_pingresp_;
    uint8_t connack_code_ = 0;
    std::vector<std::vector<uint8_t>> written_;
    std::vector<uint8_t> rx_;
};

/* ---------------------------------------------------------------- 客户端 */
enum class ClientState : uint8_t {
    DISCONNECTED = 0, CONNECTING, CONNECTED, DISCONNECTING, ERROR,
};

const char *client_state_name(ClientState s);

struct ClientConfig {
    std::string client_id;
    std::string host;
    uint16_t port = 1883;
    std::string username;
    std::string password;
    std::string will_topic;
    std::string will_payload;
    uint16_t keep_alive_s = 60;
    /** 保活：超过 keep_alive * 1.5 未收到任何报文就重连 */
    uint32_t ping_interval_ms = 30000;
    uint32_t connect_timeout_ms = 5000;
    bool clean_session = true;
};

/** 客户端统计（对应"运行状态监控"） */
struct ClientStats {
    uint64_t connect_attempts = 0;
    uint64_t connects_ok = 0;
    uint64_t publishes = 0;
    uint64_t publish_bytes = 0;
    uint64_t packets_in = 0;
    uint64_t packets_out = 0;
    uint64_t pings = 0;
    uint64_t reconnects = 0;
    uint64_t errors = 0;
};

/**
 * MQTT 客户端。
 *
 * 只做**同步、单线程**的实现，并且不自己起线程 ——
 * 这样它能被放进已有的 epoll 事件循环里（网关就是这种结构），
 * 也便于单元测试精确控制时序。
 */
class Client {
public:
    using MessageHandler = std::function<void(const PublishMessage &)>;

    Client(MqttTransport &transport, const ClientConfig &cfg = {});

    /** 连接并完成 CONNECT/CONNACK 握手（阻塞，带超时）。 */
    bool connect(std::string *error = nullptr);
    void disconnect();

    /** 发布一条消息；QoS1 会带 PacketId。 */
    bool publish(const std::string &topic, const std::string &payload,
                 QoS qos = QoS::AT_MOST_ONCE, bool retain = false);
    bool subscribe(const std::string &topic, QoS qos = QoS::AT_MOST_ONCE);
    bool unsubscribe(const std::string &topic);

    /** 处理一次可读事件：读入并解析所有完整报文。返回处理的消息数。 */
    int poll(int timeout_ms = 0);

    /** 周期性调用：到点发 PINGREQ（保活）。 */
    void tick(uint32_t elapsed_ms);

    void set_message_handler(MessageHandler h) { on_message_ = std::move(h); }

    ClientState state() const { return state_; }
    const ClientStats &stats() const { return stats_; }
    uint16_t next_packet_id();

    /** 上一次错误（便于日志与现场排查） */
    const std::string &last_error() const { return last_error_; }

private:
    bool send_packet(const std::vector<uint8_t> &pkt);
    void set_error(const std::string &e);

    MqttTransport &tp_;
    ClientConfig cfg_;
    ClientState state_ = ClientState::DISCONNECTED;
    ClientStats stats_;
    MessageHandler on_message_;
    std::vector<uint8_t> rx_;
    uint16_t packet_id_ = 1;
    uint32_t since_ping_ms_ = 0;
    std::string last_error_;
    bool subscribed_ = false;
};

}  // namespace mqtt

#endif  // GATEWAY_MQTT_H
