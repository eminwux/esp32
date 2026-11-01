#pragma once
#include "frame.h"

namespace ook {
// Bit-level search requires the last two preamble bytes and exact sync.
// Candidate size is fixed; no on-air length can allocate memory or overrun it.
class BitDecoder {
    static constexpr uint32_t kPreambleAndSync = 0xaaaad391UL;
    uint32_t preambleAndSync_ = 0;
    uint8_t body_[kBodySize] = {};
    unsigned collectedBits_ = 0;
    bool collecting_ = false;
public:
    bool collectingFrame() const { return collecting_; }
    void reset() {
        preambleAndSync_ = 0;
        collectedBits_ = 0;
        collecting_ = false;
    }
    Result pushBit(bool level, uint8_t local, Frame& f) {
        if (!collecting_) {
            preambleAndSync_ = (preambleAndSync_ << 1) | level;
            if (preambleAndSync_ == kPreambleAndSync) {
                collecting_ = true;
                collectedBits_ = 0;
            }
            return Result::None;
        }
        unsigned byte = collectedBits_ / 8;
        if (collectedBits_ % 8 == 0) body_[byte] = 0;
        body_[byte] = uint8_t((body_[byte] << 1) | level);
        ++collectedBits_;
        if (collectedBits_ == 8 && body_[0] != kLength) {
            reset();
            return Result::Length;
        }
        if (collectedBits_ != kBodySize * 8) return Result::None;
        Result r = decode(body_, kBodySize, local, f);
        reset();
        return r;
    }
};

// Samples arrive at 16 times the bit rate. Reject isolated single-sample
// glitches, acquire 16 approximately bit-spaced edges, then sample centers
// on a fixed clock. No tracking claim is made for arbitrary NRZ run lengths.
class SampleDecoder {
    static constexpr unsigned kMinEdgeSamples = 12, kMaxEdgeSamples = 20;
    static constexpr unsigned kAcquisitionEdges = 16, kSearchTimeoutBits = 256;
    BitDecoder frameDecoder_;
    uint32_t sampleTick_ = 0, lastEdgeTick_ = 0, nextCenterTick_ = 0, acquiredAtTick_ = 0;
    uint8_t recentSamples_ = 0, qualifyingEdges_ = 0;
    bool previousFilteredLevel_ = false, phaseAcquired_ = false;
public:
    enum class Stage { Preamble, Sync, Body };
    Stage stage() const {
        if (!phaseAcquired_) return Stage::Preamble;
        return frameDecoder_.collectingFrame() ? Stage::Body : Stage::Sync;
    }
    void reset() { *this = SampleDecoder(); }
    Result sample(bool level, uint8_t local, Frame& f) {
        ++sampleTick_;
        recentSamples_ = uint8_t(((recentSamples_ << 1) | level) & 7);
        // Three-sample majority removes isolated one-sample glitches. It is
        // not a substitute for the analog receiver or arbitrary noise filtering.
        bool filtered = recentSamples_ == 3 || recentSamples_ == 5 || recentSamples_ == 6 || recentSamples_ == 7;
        // Acquire phase using the alternating preamble. Once acquired, keep
        // a fixed clock even through long NRZ runs; there is no frequency PLL.
        if (!phaseAcquired_ && filtered != previousFilteredLevel_) {
            uint32_t spacing = sampleTick_ - lastEdgeTick_;
            lastEdgeTick_ = sampleTick_;
            qualifyingEdges_ = spacing >= kMinEdgeSamples && spacing <= kMaxEdgeSamples ? uint8_t(qualifyingEdges_ + 1) : 0;
            if (qualifyingEdges_ >= kAcquisitionEdges) {
                phaseAcquired_ = true;
                acquiredAtTick_ = sampleTick_;
                nextCenterTick_ = sampleTick_ + kSamplesPerBit / 2;
                frameDecoder_.reset();
            }
        }
        previousFilteredLevel_ = filtered;
        if (phaseAcquired_ && sampleTick_ == nextCenterTick_) {
            nextCenterTick_ += kSamplesPerBit;
            Result r = frameDecoder_.pushBit(filtered, local, f);
            if (r != Result::None) {
                reset();
                return r;
            }
        }
        if (phaseAcquired_ && sampleTick_ - acquiredAtTick_ >= kSearchTimeoutBits * kSamplesPerBit) {
            reset();
            return Result::Timeout;
        }
        return Result::None;
    }
};

}  // namespace ook
