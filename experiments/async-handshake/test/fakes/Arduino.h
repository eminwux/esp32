#pragma once
#include <stdint.h>

// Minimal single-threaded timer boundary for transport.cpp. It does not model
// ESP32 interrupt latency, core concurrency or critical-section correctness.
#define IRAM_ATTR
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
inline void portENTER_CRITICAL_ISR(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL_ISR(portMUX_TYPE*) {}
struct hw_timer_t {};
uint32_t micros();
hw_timer_t* timerBegin(uint8_t number, uint16_t divider, bool countUp);
void timerAttachInterrupt(hw_timer_t*, void (*callback)(), bool edge);
void timerAlarmWrite(hw_timer_t*, uint64_t period, bool reload);
void timerAlarmEnable(hw_timer_t*);
