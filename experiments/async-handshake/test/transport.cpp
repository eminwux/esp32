#include <Arduino.h>
#include <soc/gpio_struct.h>
#include "bit_io.h"
#include "config.h"
#include "decoder.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

// Compile the real bit_io.cpp as a separate translation unit. Only timer/GPIO
// primitives are fake: the production ISR, copying, ring and events run intact.
static uint32_t clockUs = 0, output = 0;
static unsigned alarmPeriod = 0;
static void (*timerCallback)() = nullptr;
static hw_timer_t timer;
struct Write { uint32_t time, level; };
static std::vector<Write> writes;
FakeGpio GPIO;
void FakeWriteRegister::operator=(uint32_t mask) {
    assert(mask == (1UL << config::kDataPin));
    output = set ? output | mask : output & ~mask;
    writes.push_back({clockUs, (output >> config::kDataPin) & 1U});
}
uint32_t micros() { return clockUs; }
hw_timer_t* timerBegin(uint8_t number, uint16_t divider, bool countUp) {
    assert(number == 0 && divider == 80 && countUp);
    return &timer;
}
void timerAttachInterrupt(hw_timer_t* t, void (*callback)(), bool edge) {
    assert(t == &timer && edge);
    timerCallback = callback;
}
void timerAlarmWrite(hw_timer_t* t, uint64_t period, bool reload) {
    assert(t == &timer && period == SAMPLE_PERIOD_US && reload);
    alarmPeriod = unsigned(period);
}
void timerAlarmEnable(hw_timer_t* t) { assert(t == &timer && timerCallback); }
static void tick(bool input = false, unsigned elapsed = SAMPLE_PERIOD_US) {
    GPIO.in = uint32_t(input) << config::kDataPin;
    clockUs += elapsed;
    timerCallback();
}
static bool wireBit(const uint8_t* wire, unsigned bit) {
    return wire[bit / 8] & (0x80 >> (bit % 8));
}
static void transmit() {
    bit_io::pause();
    uint8_t wire[ook::kWireSize], expected[ook::kWireSize];
    ook::Frame ping{ook::kPing, 1, 2, 0, 0xffff, 0xa7c3df3d};
    ook::encode(ping, wire);
    memcpy(expected, wire, sizeof wire);
    writes.clear();
    bit_io::startTransmission(wire, ook::kWireSize * 8);
    // main.cpp's buffer is local: prove the transport owns a copy throughout TX.
    memset(wire, 0, sizeof wire);
    for (unsigned bit = 0; bit < ook::kWireSize * 8; ++bit) {
        for (unsigned sample = 0; sample < ook::kSamplesPerBit; ++sample) {
            tick();
            assert(bool((output >> config::kDataPin) & 1U) == wireBit(expected, bit));
            const auto events = bit_io::takeEvents();
            assert(events.transmitting && events.txPending && !events.completed && !events.fault);
        }
    }
    tick();
    auto done = bit_io::takeEvents();
    assert(done.completed && done.txPending && !done.transmitting && !done.fault);
    assert(output == 0 && writes.size() == ook::kWireSize * 8 + 1);
    for (unsigned i = 1; i < writes.size(); ++i)
        assert(writes[i].time - writes[i - 1].time == config::kBitUs);
    assert(!bit_io::takeEvents().completed); // Completion is consumed once.
    bit_io::acknowledgeTransmission();
    assert(!bit_io::takeEvents().txPending);
}
static void receive() {
    bit_io::pause();
    bit_io::startReceiving();
    uint8_t wire[ook::kWireSize];
    // Both roles receive an addressed valid frame; B's case reproduces A's
    // initial PING from the failing trace all the way through the production ring.
    const ook::Frame sent = config::kLocal == 2 ?
        ook::Frame{ook::kPing, 1, 2, 0, 0xffff, 0xa7c3df3d} :
        ook::Frame{ook::kPongPing, 2, 1, 1, 0, 0xa7c3df3d};
    ook::encode(sent, wire);
    ook::SampleDecoder decoder;
    unsigned accepted = 0, sampleCount = 0;
    // Buffer a whole frame, then drain it. This tests the actual ring, not a
    // parallel reimplementation of its indexing or bit-generation logic.
    for (unsigned i = 0; i < 32; ++i) tick();
    for (unsigned bit = 0; bit < ook::kWireSize * 8; ++bit)
        for (unsigned sample = 0; sample < ook::kSamplesPerBit; ++sample)
            tick(wireBit(wire, bit));
    for (unsigned i = 0; i < 32; ++i) tick();
    bool level;
    while (bit_io::popSample(level)) {
        ++sampleCount;
        ook::Frame received;
        if (decoder.sample(level, config::kLocal, received) == ook::Result::Good) {
            assert(received.session == sent.session && received.seq == sent.seq && received.ack == sent.ack);
            ++accepted;
        }
    }
    assert(sampleCount == 3264 && accepted == 1);
    assert(!bit_io::takeEvents().fault);
    // More than one ring revolution, drained as in the foreground loop.
    for (unsigned i = 0; i < bit_io::kSampleBufferSize * 2; ++i) {
        tick(i % 3 == 0);
        assert(bit_io::popSample(level) && level == (i % 3 == 0));
    }
    tick(true);
    bit_io::pause();
    assert(!bit_io::popSample(level)); // Pausing discards stale samples.
}
static void faults() {
    bit_io::pause();
    bit_io::startReceiving();
    for (unsigned i = 0; i < bit_io::kSampleBufferSize; ++i) tick();
    auto events = bit_io::takeEvents();
    assert(events.fault && !events.txPending);
    assert(!bit_io::takeEvents().fault);
    assert(bit_io::takeTimingStats().overruns == 1);
    bit_io::pause();
    bit_io::startReceiving();
    tick(false, SAMPLE_PERIOD_US * 3 / 2); // Inclusive boundary is allowed.
    assert(!bit_io::takeEvents().fault);
    tick(false, SAMPLE_PERIOD_US * 3 / 2 + 1);
    assert(bit_io::takeEvents().fault);
    bit_io::pause();
    uint8_t wire[ook::kWireSize]; memset(wire, 0xff, sizeof wire);
    bit_io::startTransmission(wire, 64);
    tick(); assert(output != 0);
    tick(false, SAMPLE_PERIOD_US * 3 / 2 + 1);
    events = bit_io::takeEvents();
    assert(events.fault && events.txPending && !events.transmitting && !events.completed);
    assert(output == 0); // Aborted TX must release logical one immediately.
    bit_io::acknowledgeTransmission();
    assert(!bit_io::takeEvents().txPending);
    bit_io::pause();
    // Full raw burst still holds 64 bits, then reports completion.
    bit_io::startTransmission(wire, 64);
    for (unsigned i = 0; i < 64 * ook::kSamplesPerBit; ++i) tick();
    assert(!bit_io::takeEvents().completed);
    tick(); assert(bit_io::takeEvents().completed);
    bit_io::acknowledgeTransmission();
}
int main() {
    assert(bit_io::begin() && alarmPeriod == SAMPLE_PERIOD_US);
    transmit(); receive(); faults();
    printf("PASS: node %c transport buffer, emitted bits, RX ring, decoder and fault events (fake timer/GPIO)\n", config::kNodeLabel);
}
