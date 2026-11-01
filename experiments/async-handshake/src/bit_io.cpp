#include <Arduino.h>
#include <soc/gpio_struct.h>
#include <string.h>
#include "bit_io.h"
#include "config.h"

namespace bit_io {
namespace {
hw_timer_t* timer = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
constexpr unsigned kRing = kSampleBufferSize;
volatile uint8_t samples[kRing];
volatile unsigned head = 0, tail = 0;
uint8_t txWire[ook::kWireSize];
volatile unsigned txTicks = 0, txBits = 0;
volatile bool sampling = false, transmitting = false, txDone = false, timingFault = false;
volatile uint32_t previousTick = 0, maxTickUs = 0, overruns = 0;

void IRAM_ATTR tick() {
    portENTER_CRITICAL_ISR(&mux);
    uint32_t now = micros(), delta = now - previousTick;
    previousTick = now;
    if (delta > maxTickUs) maxTickUs = delta;
    if ((sampling || transmitting) && delta > SAMPLE_PERIOD_US * 3 / 2) {
        timingFault = true;
        sampling = transmitting = false;
        GPIO.out_w1tc = 1UL << config::kDataPin;
    }
    if (transmitting) {
        if (txTicks % config::kSamplesPerBit == 0) {
            unsigned bit = txTicks / config::kSamplesPerBit;
            if (bit == txBits) {
                GPIO.out_w1tc = 1UL << config::kDataPin;
                transmitting = false; txDone = true;
            } else if (txWire[bit / 8] & (0x80 >> (bit % 8))) {
                GPIO.out_w1ts = 1UL << config::kDataPin;
            } else GPIO.out_w1tc = 1UL << config::kDataPin;
        }
        ++txTicks;
    } else if (sampling) {
        unsigned next = (head + 1) % kRing;
        if (next == tail) { ++overruns; timingFault = true; sampling = false; }
        else { samples[head] = (GPIO.in >> config::kDataPin) & 1; head = next; }
    }
    portEXIT_CRITICAL_ISR(&mux);
}

}  // namespace

bool begin() {
    timer = timerBegin(0, 80, true); // 80 MHz APB / 80 = 1 us.
    if (!timer) return false;
    timerAttachInterrupt(timer, &tick, true);
    timerAlarmWrite(timer, SAMPLE_PERIOD_US, true);
    previousTick = micros();
    timerAlarmEnable(timer);
    return true;
}
void pause() {
    portENTER_CRITICAL(&mux);
    sampling = transmitting = false;
    head = tail = 0;
    portEXIT_CRITICAL(&mux);
}
void startReceiving() {
    portENTER_CRITICAL(&mux);
    previousTick = micros();
    sampling = true;
    portEXIT_CRITICAL(&mux);
}
void startTransmission(const uint8_t (&wire)[ook::kWireSize], unsigned bitCount) {
    // The ISR cannot read this buffer while paused; copy before publishing TX.
    memcpy(txWire, wire, sizeof(txWire));
    portENTER_CRITICAL(&mux);
    txBits = bitCount;
    txTicks = 0;
    txDone = false;
    previousTick = micros();
    transmitting = true;
    portEXIT_CRITICAL(&mux);
}
Events takeEvents() {
    portENTER_CRITICAL(&mux);
    Events events{timingFault, txDone, transmitting, txBits != 0};
    timingFault = false;
    txDone = false;
    portEXIT_CRITICAL(&mux);
    return events;
}
void acknowledgeTransmission() {
    portENTER_CRITICAL(&mux);
    txBits = 0;
    portEXIT_CRITICAL(&mux);
}
bool popSample(bool& level) {
    portENTER_CRITICAL(&mux);
    if (tail == head) { portEXIT_CRITICAL(&mux); return false; }
    level = samples[tail];
    tail = (tail + 1) % kRing;
    portEXIT_CRITICAL(&mux);
    return true;
}
TimingStats takeTimingStats() {
    portENTER_CRITICAL(&mux);
    TimingStats stats{maxTickUs, overruns};
    maxTickUs = 0;
    portEXIT_CRITICAL(&mux);
    return stats;
}
}  // namespace bit_io
