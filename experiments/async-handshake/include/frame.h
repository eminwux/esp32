#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ook {
constexpr uint8_t kLength = 12;  // Eight application bytes plus a 32-bit session.
constexpr size_t kBodySize = 1 + kLength + 2;
constexpr size_t kWireSize = 10 + kBodySize;
constexpr uint8_t kPing = 1, kPongPing = 2;
struct Frame {
    uint8_t type, source, destination;
    uint16_t seq, ack;
    uint32_t session;
};
inline uint16_t crc16(const uint8_t* data, size_t n) {
    uint16_t crc = 0xffff;
    while (n--) {
        crc ^= uint16_t(*data++) << 8;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}
inline void put16(uint8_t* p, uint16_t x) { p[0] = x >> 8; p[1] = x; }
inline uint16_t get16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline void encode(const Frame& f, uint8_t* wire) {
    for (unsigned i = 0; i < 8; ++i) wire[i] = 0xaa;
    wire[8] = 0xd3; wire[9] = 0x91;
    uint8_t* b = wire + 10;
    b[0] = kLength; b[1] = 1; b[2] = f.type; b[3] = f.source; b[4] = f.destination;
    put16(b + 5, f.seq); put16(b + 7, f.ack);
    for (unsigned i = 0; i < 4; ++i) b[9 + i] = f.session >> (24 - 8 * i);
    put16(b + 13, crc16(b, 13));
}
enum class Result { None, Good, Length, Crc, Invalid, Timeout };
inline Result decode(const uint8_t* b, size_t n, uint8_t local, Frame& f) {
    if (n != kBodySize || b[0] != kLength) return Result::Length;
    if (get16(b + 13) != crc16(b, 13)) return Result::Crc;
    if (b[1] != 1 || (b[2] != kPing && b[2] != kPongPing) ||
        (local != 1 && local != 2) || b[3] != 3 - local || b[4] != local)
        return Result::Invalid;
    f = {b[2], b[3], b[4], get16(b + 5), get16(b + 7), 0};
    for (unsigned i = 0; i < 4; ++i) f.session = (f.session << 8) | b[9 + i];
    if (!f.session || (f.type == kPing &&
        (f.source != 1 || f.seq != 0 || f.ack != 0xffff))) return Result::Invalid;
    return Result::Good;
}

}  // namespace ook
