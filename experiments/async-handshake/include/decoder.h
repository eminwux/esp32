#pragma once
#include "frame.h"

namespace ook {
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

}  // namespace ook
