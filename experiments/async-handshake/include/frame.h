#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ook {
constexpr uint8_t kPing = 1, kPongPing = 2;
constexpr uint8_t kLength = 12;  // Eight application bytes plus a 32-bit session.
constexpr size_t kPreambleBytes = 8, kSyncBytes = 2;
constexpr size_t kBodyOffset = kPreambleBytes + kSyncBytes;
constexpr size_t kBodySize = 1 + kLength + 2;
constexpr size_t kWireSize = kBodyOffset + kBodySize;
constexpr unsigned kSamplesPerBit = 16;

// Offsets are relative to the body (the length byte), not the preamble.
namespace field {
constexpr size_t Length = 0, Version = 1, Type = 2, Source = 3, Destination = 4;
constexpr size_t Sequence = 5, Acknowledgment = 7, Session = 9, Crc = 13;
}
struct Frame {
    uint8_t type, source, destination;
    uint16_t seq, ack;
    uint32_t session;
};
enum class Result { None, Good, Length, Crc, Invalid, Timeout };

// CRC-16/CCITT-FALSE: process each byte MSB first, without reflection/xorout.
inline uint16_t crc16(const uint8_t* data, size_t size) {
    uint16_t crc = 0xffff;
    while (size--) {
        crc ^= uint16_t(*data++) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}
inline void put16(uint8_t* bytes, uint16_t value) {
    bytes[0] = value >> 8;
    bytes[1] = value;
}
inline uint16_t get16(const uint8_t* bytes) {
    return (uint16_t(bytes[0]) << 8) | bytes[1];
}
inline void encode(const Frame& frame, uint8_t* wire) {
    for (unsigned i = 0; i < kPreambleBytes; ++i) wire[i] = 0xaa;
    wire[kPreambleBytes] = 0xd3;
    wire[kPreambleBytes + 1] = 0x91;
    uint8_t* body = wire + kBodyOffset;
    body[field::Length] = kLength;
    body[field::Version] = 1;
    body[field::Type] = frame.type;
    body[field::Source] = frame.source;
    body[field::Destination] = frame.destination;
    put16(body + field::Sequence, frame.seq);
    put16(body + field::Acknowledgment, frame.ack);
    for (unsigned i = 0; i < 4; ++i)
        body[field::Session + i] = frame.session >> (24 - 8 * i);
    // Include length and application bytes, excluding preamble, sync and CRC.
    put16(body + field::Crc, crc16(body, field::Crc));
}
inline Result decode(const uint8_t* body, size_t size, uint8_t localNode, Frame& frame) {
    if (size != kBodySize || body[field::Length] != kLength) return Result::Length;
    if (get16(body + field::Crc) != crc16(body, field::Crc)) return Result::Crc;
    if (body[field::Version] != 1 ||
        (body[field::Type] != kPing && body[field::Type] != kPongPing) ||
        (localNode != 1 && localNode != 2) ||
        body[field::Source] != 3 - localNode || body[field::Destination] != localNode)
        return Result::Invalid;
    frame = {body[field::Type], body[field::Source], body[field::Destination],
             get16(body + field::Sequence), get16(body + field::Acknowledgment), 0};
    for (unsigned i = 0; i < 4; ++i)
        frame.session = (frame.session << 8) | body[field::Session + i];
    // FFFF is the no-ACK sentinel only for the initial PING; it remains a valid
    // wrapped acknowledgment in a PONG_PING continuation.
    if (!frame.session || (frame.type == kPing &&
        (frame.source != 1 || frame.seq != 0 || frame.ack != 0xffff))) return Result::Invalid;
    return Result::Good;
}
}  // namespace ook
