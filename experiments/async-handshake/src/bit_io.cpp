#include <Arduino.h>
#include <soc/gpio_struct.h>
#include <string.h>
#include "bit_io.h"
#include "config.h"

namespace bit_io {
namespace {
// Only the ISR touches GPIO at sample/bit cadence. Shared indices, event flags
// and statistics are protected by bufferLock on both sides; volatile alone
// would not make the multi-field snapshots atomic. One ring slot stays empty
// to distinguish full from empty (8191 usable samples, about 852 ms at 104 us).
hw_timer_t* timer = nullptr;
portMUX_TYPE bufferLock = portMUX_INITIALIZER_UNLOCKED;
constexpr unsigned kRing = kSampleBufferSize;
volatile uint8_t samples[kRing];
volatile unsigned writeIndex = 0, readIndex = 0;
uint8_t txWire[ook::kWireSize];
volatile unsigned transmitTick = 0, transmitBitCount = 0;
volatile bool sampling = false, transmitting = false, transmitCompleted = false, timingFault = false;
volatile uint32_t previousTickUs = 0, maxIntervalUs = 0, overruns = 0;

// Keep this callback IRAM-safe and bounded: no SPI, Serial, allocation, frame
// parsing or protocol decisions. Foreground code handles the latched events.
void IRAM_ATTR tick() {
    portENTER_CRITICAL_ISR(&bufferLock);
    uint32_t now = micros(), delta = now - previousTickUs;
    previousTickUs = now;
    if (delta > maxIntervalUs) maxIntervalUs = delta;
    if ((sampling || transmitting) && delta > SAMPLE_PERIOD_US * 3 / 2) {
        timingFault = true;
        sampling = transmitting = false;
        GPIO.out_w1tc = 1UL << config::kDataPin;
    }
    if (transmitting) {
        if (transmitTick % config::kSamplesPerBit == 0) {
            unsigned bit = transmitTick / config::kSamplesPerBit;
            if (bit == transmitBitCount) {
                GPIO.out_w1tc = 1UL << config::kDataPin;
                transmitting = false;
                transmitCompleted = true;
            } else if (txWire[bit / 8] & (0x80 >> (bit % 8))) {
                GPIO.out_w1ts = 1UL << config::kDataPin;
            } else GPIO.out_w1tc = 1UL << config::kDataPin;
        }
        ++transmitTick;
    } else if (sampling) {
        unsigned next = (writeIndex + 1) % kRing;
        if (next == readIndex) {
            ++overruns;
            timingFault = true;
            sampling = false;
        } else {
            samples[writeIndex] = (GPIO.in >> config::kDataPin) & 1;
            writeIndex = next;
        }
    }
    portEXIT_CRITICAL_ISR(&bufferLock);
}

}  // namespace

bool begin() {
    timer = timerBegin(0, 80, true); // 80 MHz APB / 80 = 1 us.
    if (!timer) return false;
    timerAttachInterrupt(timer, &tick, true);
    timerAlarmWrite(timer, SAMPLE_PERIOD_US, true);
    previousTickUs = micros();
    timerAlarmEnable(timer);
    return true;
}
void pause() {
    portENTER_CRITICAL(&bufferLock);
    sampling = transmitting = false;
    writeIndex = readIndex = 0;
    portEXIT_CRITICAL(&bufferLock);
}
void startReceiving() {
    portENTER_CRITICAL(&bufferLock);
    previousTickUs = micros();
    sampling = true;
    portEXIT_CRITICAL(&bufferLock);
}
void startTransmission(const uint8_t (&wire)[ook::kWireSize], unsigned bitCount) {
    // The ISR cannot read this buffer while paused; copy before publishing TX.
    memcpy(txWire, wire, sizeof(txWire));
    portENTER_CRITICAL(&bufferLock);
    transmitBitCount = bitCount;
    transmitTick = 0;
    transmitCompleted = false;
    previousTickUs = micros();
    transmitting = true;
    portEXIT_CRITICAL(&bufferLock);
}
// Fault/completion are consumed once. transmitBitCount remains nonzero until
// foreground accounting acknowledges even an aborted transmission.
Events takeEvents() {
    portENTER_CRITICAL(&bufferLock);
    Events events{timingFault, transmitCompleted, transmitting, transmitBitCount != 0};
    timingFault = false;
    transmitCompleted = false;
    portEXIT_CRITICAL(&bufferLock);
    return events;
}
void acknowledgeTransmission() {
    portENTER_CRITICAL(&bufferLock);
    transmitBitCount = 0;
    portEXIT_CRITICAL(&bufferLock);
}
bool popSample(bool& level) {
    portENTER_CRITICAL(&bufferLock);
    if (readIndex == writeIndex) {
        portEXIT_CRITICAL(&bufferLock);
        return false;
    }
    level = samples[readIndex];
    readIndex = (readIndex + 1) % kRing;
    portEXIT_CRITICAL(&bufferLock);
    return true;
}
TimingStats takeTimingStats() {
    portENTER_CRITICAL(&bufferLock);
    TimingStats stats{maxIntervalUs, overruns};
    maxIntervalUs = 0;
    portEXIT_CRITICAL(&bufferLock);
    return stats;
}
}  // namespace bit_io
