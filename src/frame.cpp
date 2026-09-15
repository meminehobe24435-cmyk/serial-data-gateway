// frame.cpp —— CRC 校验与时间戳工具
#include "gateway.h"

#include <chrono>

// CRC-16/MODBUS 查表实现：反射多项式 0xA001，初值 0xFFFF。
// 查表比逐位快约 8 倍，且表在编译期生成，不占运行时开销。
static const uint16_t* crc_table() {
    static uint16_t table[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; ++i) {
            uint16_t crc = i;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                                : static_cast<uint16_t>(crc >> 1);
            table[i] = crc;
        }
        built = true;
    }
    return table;
}

uint16_t crc16_modbus(const uint8_t* data, size_t n) {
    const uint16_t* t = crc_table();
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i)
        crc = static_cast<uint16_t>((crc >> 8) ^ t[(crc ^ data[i]) & 0xFF]);
    return crc;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
