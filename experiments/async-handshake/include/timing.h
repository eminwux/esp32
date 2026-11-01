#pragma once
#include <stdint.h>

namespace ook {
// Use the same wrap-safe countdown for scheduling and the user-facing status.
inline uint32_t remainingMs(uint32_t now, uint32_t since, uint32_t duration) {
    uint32_t elapsed = now - since;
    return elapsed >= duration ? 0 : duration - elapsed;
}
inline uint32_t silenceMs(uint32_t txDurationMs, uint32_t dutyPermille, uint32_t minimumMs) {
    uint32_t dutySilence = uint64_t(txDurationMs) * (1000 - dutyPermille) / dutyPermille;
    return dutySilence > minimumMs ? dutySilence : minimumMs;
}
inline uint32_t sendWaitMs(uint32_t turnaroundRemaining, uint32_t silenceRemaining) {
    return turnaroundRemaining > silenceRemaining ? turnaroundRemaining : silenceRemaining;
}
}  // namespace ook
