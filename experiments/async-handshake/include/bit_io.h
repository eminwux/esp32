#pragma once
#include "frame.h"

// Hardware boundary. All public functions are called by the foreground loop.
namespace bit_io {
constexpr unsigned kSampleBufferSize = 8192;
struct Events {
    bool fault;
    bool completed;
    bool transmitting;
    bool txPending;  // A started TX still needs foreground airtime accounting.
};
struct TimingStats { uint32_t maxIntervalUs, overruns; };
bool begin();
void pause();
void startReceiving();
// Caller has paused I/O, entered radio TX, and supplied <= kWireSize * 8 bits.
void startTransmission(const uint8_t (&wire)[ook::kWireSize], unsigned bitCount);
Events takeEvents();
void acknowledgeTransmission();
bool popSample(bool& level);
TimingStats takeTimingStats();
}  // namespace bit_io
