#pragma once

#include <stdint.h>

#define IRAM_ATTR
extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
inline void delay(uint32_t ms) { fakeMillis += ms; }

struct FakeSerial {
    void begin(unsigned long) {}
    template <typename... Args>
    void printf(const char*, Args...) {}
    void println(const char* = "") {}
};
extern FakeSerial Serial;
