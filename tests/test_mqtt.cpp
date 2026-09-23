/* test_mqtt.cpp — MQTT 3.1.1 客户端单元测试（纯 C++17，无第三方框架）
 *
 *  T1 varlen   剩余长度变长编码（MQTT 最经典的坑）
 *  T2 build   CONNECT / PUBLISH / SUBSCRIBE / PINGREQ 报文字节
 *  T3 parse   各类型报文解析 + 半包 + 非法输入
 *  T4 client  连接握手、发布、订阅、收消息、QoS1 回 ACK、保活、断线检测
 *  T5 edge    边界：未连接发送、服务端拒绝、超时、PacketId 回绕
 */
#include "mqtt.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mqtt;

static int passed = 0, failed = 0;

#define CHECK(cond, ...) do {                          \
    if (cond) { passed++; std::printf("  [PASS] "); }   \
    else      { failed++; std::printf("  [FAIL] "); }   \
    std::printf(__VA_ARGS__); std::printf("\n");        \
    std::fflush(stdout);                                \
} while (0)

/* ------------------------------------------------------------------ T1 */
static void t1_varlen() {
    std::printf("T1 剩余长度变长编码\n");

    struct { uint32_t v; size_t n; const char *desc; } cases[] = {
        { 0,        1, "0" },
        { 127,      1, "127（单字节上限）" },
        { 128,      2, "128（跨字节，最经典的坑）" },
        { 16383,    2, "16383（两字节上限）" },
        { 16384,    3, "16384（三字节）" },
        { 2097151,  3, "2097151（三字节上限）" },
        { 2097152,  4, "2097152（四字节）" },
        { 268435455,4, "268435455（协议上限）" },
    };
    for (const auto &c : cases) {
        const auto enc = encode_remaining_length(c.v);
        CHECK(enc.size() == c.n, "%s -> 编码 %zu 字节", c.desc, enc.size());
        uint32_t dec = 0;
        size_t used = 0;
        const bool ok = decode_remaining_length(enc.data(), enc.size(), 0, dec, used);
        CHECK(ok && dec == c.v && used == c.n,
              "%s -> 解码回 %u（用了 %zu 字节）", c.desc, dec, used);
    }

    /* 128 的字节形态：0x80 0x01 */
    const auto e128 = encode_remaining_length(128);
    CHECK(e128.size() == 2 && e128[0] == 0x80 && e128[1] == 0x01,
          "128 -> 0x80 0x01（低 7 位 + 续接位）");

    /* 数据不足：应返回 false 而不是猜 */
    uint32_t v = 0;
    size_t used = 0;
    const uint8_t partial[1] = { 0x80 };
    CHECK(!decode_remaining_length(partial, 1, 0, v, used),
          "只有 0x80（续接位为 1）时数据不足 -> 返回 false");

    /* 超过 4 字节：非法 */
    const uint8_t toobig[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
    CHECK(!decode_remaining_length(toobig, 5, 0, v, used),
          "5 字节续接 -> 判为非法");

    /* 带偏移解析 */
    const auto enc = encode_remaining_length(300);
    std::vector<uint8_t> padded = { 0x30, 0xAA };
    padded.insert(padded.end(), enc.begin(), enc.end());
    uint32_t d2 = 0;
    size_t u2 = 0;
    CHECK(decode_remaining_length(padded.data(), padded.size(), 2, d2, u2) && d2 == 300,
          "带偏移解析正确（offset=2 -> %u）", d2);
}

/* ------------------------------------------------------------------ T2 */
static void t2_build() {
    std::printf("T2 报文构造\n");

    /* --- CONNECT（最小） --- */
    auto c = build_connect("gw01");
    CHECK(c[0] == 0x10, "CONNECT 固定头 = 0x%02X", c[0]);
    /* CONNECT 报文体 = 协议名(2+4) + 级别(1) + 连接标志(1) + KeepAlive(2)
     *                + ClientId(2+4) = 16
     * （原来写成 12 是我算错了；这个数必须逐字段对，否则服务端直接断连） */
    CHECK(c[1] == 16, "剩余长度 = %u（6+1+1+2+6 = 16）", c[1]);
    /* 协议名 */
    CHECK(c[4] == 'M' && c[5] == 'Q' && c[6] == 'T' && c[7] == 'T',
          "协议名为 MQTT");
    CHECK(c[8] == 4, "协议级别 = 4（3.1.1）");
    CHECK(c[9] == 0x02, "连接标志 = 0x%02X（clean session）", c[9]);
    CHECK(c[10] == 0x00 && c[11] == 60, "KeepAlive = 60 s");

    /* --- CONNECT（带用户名/密码/遗嘱） --- */
    auto c2 = build_connect("gw01", "user", "pass", 30, true,
                            "gw/status", "offline", QoS::AT_LEAST_ONCE, true);
    CHECK(c2[9] == 0xEE, "连接标志 = 0x%02X（clean+will+qos1+retain+user+pass）", c2[9]);
    CHECK(c2[10] == 0x00 && c2[11] == 30, "KeepAlive = 30 s");

    /* 标志位逐项检查：只加用户名不应带密码位 */
    auto c3 = build_connect("gw01", "user");
    CHECK((c3[9] & 0x80) != 0 && (c3[9] & 0x40) == 0,
          "只有用户名时：username 位置 1、password 位为 0（0x%02X）", c3[9]);

    /* --- PUBLISH QoS0 --- */
    PublishMessage m;
    m.topic = "gw/data";
    m.payload = "hi";
    auto p = build_publish(m);
    CHECK(p[0] == 0x30, "PUBLISH QoS0 固定头 = 0x%02X", p[0]);
    CHECK(p[2] == 0x00 && p[3] == 7, "主题长度前缀 = 7");

    /* --- PUBLISH QoS1 --- */
    m.qos = QoS::AT_LEAST_ONCE;
    m.packet_id = 0x1234;
    m.retain = true;
    auto p1 = build_publish(m);
    CHECK((p1[0] & 0x06) == 0x02, "QoS1 在固定头 bit1-2（0x%02X）", p1[0]);
    CHECK((p1[0] & 0x01) == 0x01, "retain 位为 1");

    /* --- SUBSCRIBE：flags 必须是 0b0010 --- */
    auto s = build_subscribe(1, { "gw/#" });
    CHECK(s[0] == 0x82, "SUBSCRIBE 固定头 = 0x%02X（flags 必须为 0x2）", s[0]);

    /* --- 其它 --- */
    auto pr = build_pingreq();
    CHECK(pr.size() == 2 && pr[0] == 0xC0 && pr[1] == 0x00, "PINGREQ = C0 00");
    auto dc = build_disconnect();
    CHECK(dc.size() == 2 && dc[0] == 0xE0 && dc[1] == 0x00, "DISCONNECT = E0 00");
    auto pa = build_puback(0x0102);
    CHECK(pa.size() == 4 && pa[0] == 0x40 && pa[2] == 0x01 && pa[3] == 0x02,
          "PUBACK 带 PacketId");

    /* 大载荷会触发变长编码 */
    PublishMessage big;
    big.topic = "gw/big";
    big.payload.assign(300, 'x');
    auto pb = build_publish(big);
    uint32_t rl = 0;
    size_t used = 0;
    decode_remaining_length(pb.data(), pb.size(), 1, rl, used);
    CHECK(used == 2 && rl == pb.size() - 1 - used,
          "300 字节载荷 -> 剩余长度用 %zu 字节（%u）", used, rl);
}

/* ------------------------------------------------------------------ T3 */
static void t3_parse() {
    std::printf("T3 报文解析\n");

    /* CONNACK */
    {
        const uint8_t buf[] = { 0x20, 0x02, 0x01, 0x00 };
        ParsedPacket pp;
        std::string err;
        CHECK(parse_packet(buf, sizeof(buf), pp, &err), "CONNACK 解析成功");
        CHECK(pp.type == PacketType::CONNACK, "类型 = %s", packet_type_name(pp.type));
        CHECK(pp.connack.session_present && pp.connack.ok(),
              "session_present=1，返回码 0（%s）", pp.connack.error_text());
        CHECK(pp.consumed == 4, "消耗字节 = %zu", pp.consumed);
    }
    /* CONNACK 失败码 */
    {
        const uint8_t buf[] = { 0x20, 0x02, 0x00, 0x05 };
        ParsedPacket pp;
        parse_packet(buf, sizeof(buf), pp, nullptr);
        CHECK(!pp.connack.ok(), "返回码 5 -> 不通过");
        CHECK(std::string(pp.connack.error_text()).find("未授权") != std::string::npos,
              "错误文案：%s", pp.connack.error_text());
    }
    /* PUBLISH QoS0 */
    {
        const uint8_t buf[] = { 0x30, 0x0A, 0x00, 0x03, 'a', '/', 'b', 'h', 'e', 'l', 'l', 'o' };
        ParsedPacket pp;
        CHECK(parse_packet(buf, sizeof(buf), pp), "PUBLISH 解析成功");
        CHECK(pp.publish.topic == "a/b", "主题 = %s", pp.publish.topic.c_str());
        CHECK(pp.publish.payload == "hello", "载荷 = %s", pp.publish.payload.c_str());
        CHECK(pp.publish.qos == QoS::AT_MOST_ONCE, "QoS = 0");
    }
    /* PUBLISH QoS1（带 PacketId） */
    {
        const uint8_t buf[] = { 0x32, 0x08, 0x00, 0x02, 't', '1', 0x00, 0x2A, 'o', 'k' };
        ParsedPacket pp;
        CHECK(parse_packet(buf, sizeof(buf), pp), "PUBLISH QoS1 解析成功");
        CHECK(pp.publish.packet_id == 42, "PacketId = %u", pp.publish.packet_id);
        CHECK(pp.publish.payload == "ok", "载荷 = %s", pp.publish.payload.c_str());
    }
    /* SUBACK */
    {
        const uint8_t buf[] = { 0x90, 0x03, 0x00, 0x01, 0x00 };
        ParsedPacket pp;
        CHECK(parse_packet(buf, sizeof(buf), pp), "SUBACK 解析成功");
        CHECK(pp.packet_id == 1 && pp.suback_codes.size() == 1 && pp.suback_codes[0] == 0,
              "PacketId=1，授权 QoS=0");
    }
    /* PINGRESP */
    {
        const uint8_t buf[] = { 0xD0, 0x00 };
        ParsedPacket pp;
        CHECK(parse_packet(buf, sizeof(buf), pp), "PINGRESP 解析成功");
        CHECK(pp.type == PacketType::PINGRESP, "类型正确");
    }
    /* 半包：数据不足应返回 false 而不是错解析 */
    {
        const uint8_t full[] = { 0x30, 0x0A, 0x00, 0x03, 'a', '/', 'b', 'h', 'e', 'l', 'l', 'o' };
        ParsedPacket pp;
        std::string err;
        CHECK(!parse_packet(full, 4, pp, &err), "半包 -> 返回 false（%s）", err.c_str());
        CHECK(!parse_packet(full, 1, pp, &err), "只有 1 字节 -> 返回 false");
    }
    /* 非法：QoS 3 */
    {
        const uint8_t bad[] = { 0x36, 0x04, 0x00, 0x01, 'a', 0x00 };
        ParsedPacket pp;
        std::string err;
        CHECK(!parse_packet(bad, sizeof(bad), pp, &err), "QoS=3（非法）-> 拒绝（%s）", err.c_str());
    }
    /* 非法：主题长度超出报文 */
    {
        const uint8_t bad[] = { 0x30, 0x04, 0x00, 0x99, 'a', 'b' };
        ParsedPacket pp;
        CHECK(!parse_packet(bad, sizeof(bad), pp, nullptr), "主题长度越界 -> 拒绝");
    }
    /* 多包连排：应一次只解析一个，consumed 正确 */
    {
        std::vector<uint8_t> two = { 0xD0, 0x00, 0xD0, 0x00 };
        ParsedPacket pp;
        CHECK(parse_packet(two.data(), two.size(), pp) && pp.consumed == 2,
              "连排两个包：第一个消耗 %zu 字节", pp.consumed);
        CHECK(parse_packet(two.data() + 2, 2, pp) && pp.consumed == 2,
              "第二个也能解析");
    }
}

/* ------------------------------------------------------------------ T4 */
static void t4_client() {
    std::printf("T4 客户端\n");

    MemoryTransport tp;
    ClientConfig cfg;
    cfg.client_id = "gw01";
    cfg.host = "broker.test";
    cfg.port = 1883;
    cfg.keep_alive_s = 60;
    Client cl(tp, cfg);

    CHECK(cl.state() == ClientState::DISCONNECTED, "初始状态 %s",
          client_state_name(cl.state()));

    std::string err;
    CHECK(cl.connect(&err), "握手成功（%s）", err.c_str());
    CHECK(cl.state() == ClientState::CONNECTED, "状态 %s",
          client_state_name(cl.state()));
    CHECK(cl.stats().connects_ok == 1 && cl.stats().packets_out == 1,
          "统计：连接 1 次、发出 1 包");
    CHECK(tp.written().size() == 1 && tp.written()[0][0] == 0x10,
          "第一包是 CONNECT");

    /* 发布 QoS0 */
    CHECK(cl.publish("gw/data", "{\"t\":25}"), "发布 QoS0 成功");
    CHECK(cl.stats().publishes == 1, "发布计数 = %llu",
          (unsigned long long)cl.stats().publishes);
    CHECK(cl.stats().publish_bytes == 8, "发布字节 = %llu",
          (unsigned long long)cl.stats().publish_bytes);
    CHECK(tp.written().back()[0] == 0x30, "PUBLISH 固定头正确");

    /* 发布 QoS1 */
    CHECK(cl.publish("gw/data", "x", QoS::AT_LEAST_ONCE), "发布 QoS1 成功");
    CHECK((tp.written().back()[0] & 0x06) == 0x02, "QoS1 标志正确");

    /* 订阅 */
    CHECK(cl.subscribe("gw/cmd", QoS::AT_LEAST_ONCE), "订阅成功");
    CHECK(tp.written().back()[0] == 0x82, "SUBSCRIBE 固定头正确");

    /* 收到 PUBLISH（QoS0） */
    {
        std::vector<PublishMessage> got;
        cl.set_message_handler([&](const PublishMessage &m) { got.push_back(m); });
        tp.queue_incoming({ 0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'X', 'Y' });
        const int n = cl.poll(0);
        CHECK(n == 1 && got.size() == 1, "poll 处理 %d 个报文，回调 %zu 次",
              n, got.size());
        CHECK(!got.empty() && got[0].topic == "a/b" && got[0].payload == "XY",
              "消息内容正确");
    }
    /* 收到 QoS1 PUBLISH -> 应自动回 PUBACK */
    {
        /* ⚠️ 必须重设回调：上一个块里的 got 已经出了作用域，
         * 那个 lambda 持有的是悬垂引用（这是写测试时踩到的真实崩溃）。 */
        cl.set_message_handler([](const PublishMessage &) {});
        const size_t before = tp.written().size();
        tp.queue_incoming({ 0x32, 0x08, 0x00, 0x02, 't', '1', 0x00, 0x2A, 'o', 'k' });
        cl.poll(0);
        CHECK(tp.written().size() == before + 1, "QoS1 应回一包 PUBACK（%zu -> %zu）",
              before, tp.written().size());
        CHECK(tp.written().back()[0] == 0x40, "回的是 PUBACK（0x%02X）",
              tp.written().back()[0]);
        CHECK(tp.written().back()[2] == 0x00 && tp.written().back()[3] == 0x2A,
              "PUBACK 里的 PacketId = 42");
    }
    /* 保活：到点发 PINGREQ */
    {
        const auto before = cl.stats().pings;
        cl.tick(1000);
        CHECK(cl.stats().pings == before, "未到间隔不发 PING");
        cl.tick(cfg.ping_interval_ms);
        CHECK(cl.stats().pings == before + 1, "到间隔发 PINGREQ");
        CHECK(tp.written().back()[0] == 0xC0, "最后一包是 PINGREQ");
    }
    /* 对端断开 -> 状态转 ERROR */
    {
        tp.force_close();
        cl.poll(0);
        CHECK(cl.state() == ClientState::ERROR, "对端断开 -> %s",
              client_state_name(cl.state()));
        CHECK(!cl.last_error().empty(), "记录了错误：%s", cl.last_error().c_str());
    }
    /* 断开后不能再发 */
    CHECK(!cl.publish("x", "y"), "未连接时发布失败");
    cl.disconnect();
    CHECK(cl.state() == ClientState::DISCONNECTED, "disconnect 后回到未连接");
}

/* ------------------------------------------------------------------ T5 */
static void t5_edge() {
    std::printf("T5 边界\n");

    /* 未连接就发 */
    {
        MemoryTransport tp;
        Client cl(tp);
        CHECK(!cl.publish("a", "b"), "未连接发布 -> false");
        CHECK(!cl.subscribe("a"), "未连接订阅 -> false");
    }
    /* 服务端拒绝（返回码 4 用户名密码错） */
    {
        MemoryTransport tp(true, true);
        tp.set_connack_code(4);
        Client cl(tp);
        std::string err;
        CHECK(!cl.connect(&err), "服务端拒绝 -> 连接失败");
        CHECK(err.find("用户名或密码错误") != std::string::npos, "错误信息：%s", err.c_str());
    }
    /* 服务端不回 CONNACK -> 超时 */
    {
        MemoryTransport tp(false, false);          /* 关掉自动应答 */
        ClientConfig cfg;
        cfg.connect_timeout_ms = 200;              /* 缩短等待，测试快 */
        Client cl(tp, cfg);
        std::string err;
        CHECK(!cl.connect(&err), "无 CONNACK -> 失败");
        CHECK(err.find("超时") != std::string::npos, "错误信息：%s", err.c_str());
    }
    /* PacketId 回绕：跳过 0 */
    {
        MemoryTransport tp;
        Client cl(tp);
        cl.connect();
        uint16_t last = cl.next_packet_id();
        bool skipped_zero = true;
        for (int i = 0; i < 100; ++i) {
            const uint16_t v = cl.next_packet_id();
            if (v == 0) skipped_zero = false;
            last = v;
        }
        CHECK(skipped_zero, "PacketId 不会用到保留值 0（最后 %u）", last);
    }
    /* 报文类型名完备 */
    {
        bool all = true;
        for (int i = 1; i <= 14; ++i) {
            const char *n = packet_type_name(static_cast<PacketType>(i));
            if (!n || std::string(n) == "?") all = false;
        }
        CHECK(all, "14 种报文类型都有名字");
    }
    /* 大载荷分两次读入（模拟 TCP 粘包/半包） */
    {
        MemoryTransport tp;
        Client cl(tp);
        cl.connect();
        std::vector<PublishMessage> got;
        cl.set_message_handler([&](const PublishMessage &m) { got.push_back(m); });

        PublishMessage m;
        m.topic = "gw/big";
        m.payload.assign(400, 'z');
        const auto pkt = build_publish(m);
        /* 先喂一半 */
        std::vector<uint8_t> half(pkt.begin(), pkt.begin() + 100);
        tp.queue_incoming(half);
        CHECK(cl.poll(0) == 0, "半包不触发回调");
        std::vector<uint8_t> rest(pkt.begin() + 100, pkt.end());
        tp.queue_incoming(rest);
        CHECK(cl.poll(0) == 1 && got.size() == 1, "补齐后触发回调");
        CHECK(!got.empty() && got[0].payload.size() == 400,
              "大载荷完整收到（%zu 字节）", got.empty() ? 0 : got[0].payload.size());
    }
}

int main() {
    std::printf("==== MQTT 3.1.1 客户端 单元测试 ====\n\n");
    t1_varlen();
    t2_build();
    t3_parse();
    t4_client();
    t5_edge();
    std::printf("\n==== 结果：%d passed, %d failed ====\n", passed, failed);
    return failed ? 1 : 0;
}
