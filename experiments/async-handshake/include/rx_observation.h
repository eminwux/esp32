#pragma once
#include "decoder.h"

namespace ook {
// Foreground-only observations of samples actually drained from the RX ring.
// These distinguish a quiet input from activity that never reaches the CRC.
struct RxObservation {
    uint32_t samples = 0, high = 0, edges = 0, preambles = 0, syncs = 0;
    bool previousLevel = false, havePrevious = false;

    void observe(bool level, SampleDecoder::Stage before, SampleDecoder::Stage after) {
        ++samples;
        if (level) ++high;
        if (havePrevious && level != previousLevel) ++edges;
        previousLevel = level;
        havePrevious = true;
        if (before == SampleDecoder::Stage::Preamble && after == SampleDecoder::Stage::Sync)
            ++preambles;
        if (before == SampleDecoder::Stage::Sync && after == SampleDecoder::Stage::Body)
            ++syncs;
    }
    // A pause discards samples. Do not count a transition across that gap.
    void discontinuity() { havePrevious = false; }
    // Keep the last level so an edge across reporting windows is counted once.
    void clearWindow() { samples = high = edges = preambles = syncs = 0; }
};
}  // namespace ook
