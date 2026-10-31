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

// Bit-level search requires the last two preamble bytes and exact sync.
// Candidate size is fixed; no on-air length can allocate memory or overrun it.
class BitDecoder {
    uint32_t search_ = 0;
    uint8_t body_[kBodySize] = {};
    unsigned bits_ = 0;
    bool collecting_ = false;
public:
    void reset() { search_ = 0; bits_ = 0; collecting_ = false; }
    Result pushBit(bool level, uint8_t local, Frame& f) {
        if (!collecting_) {
            search_ = (search_ << 1) | level;
            if (search_ == 0xaaaad391UL) { collecting_ = true; bits_ = 0; }
            return Result::None;
        }
        unsigned byte = bits_ / 8;
        if (bits_ % 8 == 0) body_[byte] = 0;
        body_[byte] = uint8_t((body_[byte] << 1) | level);
        ++bits_;
        if (bits_ == 8 && body_[0] != kLength) { reset(); return Result::Length; }
        if (bits_ != kBodySize * 8) return Result::None;
        Result r = decode(body_, kBodySize, local, f);
        reset();
        return r;
    }
};

// Samples arrive at 16 times the bit rate. Reject isolated single-sample
// glitches, acquire 16 approximately bit-spaced edges, then sample centers
// on a fixed clock. No tracking claim is made for arbitrary NRZ run lengths.
class SampleDecoder {
    BitDecoder bits_;
    uint32_t tick_ = 0, edge_ = 0, next_ = 0, start_ = 0;
    uint8_t history_ = 0, edges_ = 0;
    bool previous_ = false, locked_ = false;
public:
    void reset() { *this = SampleDecoder(); }
    Result sample(bool level, uint8_t local, Frame& f) {
        ++tick_;
        history_ = uint8_t(((history_ << 1) | level) & 7);
        bool filtered = history_ == 3 || history_ == 5 || history_ == 6 || history_ == 7;
        if (!locked_ && filtered != previous_) {
            uint32_t spacing = tick_ - edge_;
            edge_ = tick_;
            edges_ = spacing >= 12 && spacing <= 20 ? uint8_t(edges_ + 1) : 0;
            if (edges_ >= 16) {
                locked_ = true; start_ = tick_; next_ = tick_ + 8; bits_.reset();
            }
        }
        previous_ = filtered;
        if (locked_ && tick_ == next_) {
            next_ += 16;
            Result r = bits_.pushBit(filtered, local, f);
            if (r != Result::None) { reset(); return r; }
        }
        if (locked_ && tick_ - start_ >= 256 * 16) { reset(); return Result::Timeout; }
        return Result::None;
    }
};

// Stop-and-wait semantics shared with the host tests. A queued response is
// not an outstanding packet until sent(); duplicates cannot queue more RF.
class Handshake {
    uint8_t local_;
    bool active_ = false, waiting_ = false, queued_ = false;
    uint32_t session_ = 0, retired_ = 0;
    uint16_t last_ = 0;
    Frame next_ = {};
public:
    explicit Handshake(uint8_t local) : local_(local) {}
    bool pending() const { return queued_; }
    bool waiting() const { return waiting_; }
    const Frame& next() const { return next_; }
    void recover(uint32_t session) {
        timeout();
        if (local_ != 1) return;
        session_ = session ? session : 1;
        if (session_ == retired_) { ++session_; if (!session_) session_ = 1; }
        active_ = queued_ = true;
        next_ = {kPing, 1, 2, 0, 0xffff, session_};
    }
    void timeout() {
        if (active_) retired_ = session_;
        active_ = waiting_ = queued_ = false;
    }
    void sent() { last_ = next_.seq; queued_ = false; waiting_ = true; }
    bool receive(const Frame& f) {
        if (f.source != 3 - local_ || f.destination != local_ || !f.session) return false;
        if (local_ == 2 && f.type == kPing && f.seq == 0 && f.ack == 0xffff) {
            if (f.session == retired_ || (active_ && f.session == session_)) return false;
            if (active_) retired_ = session_;
            session_ = f.session; active_ = true;
        } else if (!active_ || !waiting_ || f.type != kPongPing ||
                   f.session != session_ || f.ack != last_ ||
                   f.seq != uint16_t(last_ + 1)) return false;
        next_ = {kPongPing, local_, uint8_t(3 - local_), uint16_t(f.seq + 1), f.seq, session_};
        queued_ = true; waiting_ = false;
        return true;
    }
};
}  // namespace ook
