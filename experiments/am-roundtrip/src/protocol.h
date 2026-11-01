#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace am {

constexpr size_t FRAME_SIZE = 16;
constexpr uint8_t MAGIC[] = {'A', 'M', 'E', '1'};
constexpr uint8_t PATTERN[] = {0xAA, 0x55, 0x00, 0xFF};

inline void writeU32(uint8_t* bytes, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (24 - 8 * i));
    }
}

inline uint32_t readU32(const uint8_t* bytes) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

inline void makeFrame(uint8_t* frame, uint32_t session, uint32_t sequence) {
    memcpy(frame, MAGIC, sizeof(MAGIC));
    writeU32(frame + 4, session);
    writeU32(frame + 8, sequence);
    memcpy(frame + 12, PATTERN, sizeof(PATTERN));
}

inline bool validFrame(const uint8_t* frame, size_t length) {
    return length == FRAME_SIZE &&
           memcmp(frame, MAGIC, sizeof(MAGIC)) == 0 &&
           memcmp(frame + 12, PATTERN, sizeof(PATTERN)) == 0;
}

// Unsigned subtraction also works across the millis() rollover.
inline bool elapsed(uint32_t now, uint32_t start, uint32_t duration) {
    return static_cast<uint32_t>(now - start) >= duration;
}

}  // namespace am
