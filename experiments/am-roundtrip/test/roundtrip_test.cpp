#include <assert.h>
#include <stdio.h>
#include "../src/main.cpp"

uint32_t fakeMillis = 0;
FakeSerial Serial;

void testProtocol() {
    uint8_t frame[am::FRAME_SIZE];
    am::makeFrame(frame, 0x12345678, 0xFEDCBA98);
    assert(am::validFrame(frame, sizeof(frame)));
    assert(frame[4] == 0x12 && frame[7] == 0x78);
    assert(am::readU32(frame + 8) == 0xFEDCBA98);
    assert(!am::validFrame(frame, sizeof(frame) - 1));
    frame[0] ^= 1;
    assert(!am::validFrame(frame, sizeof(frame)));
    frame[0] ^= 1;
    frame[15] ^= 1;
    assert(!am::validFrame(frame, sizeof(frame)));
    assert(!am::elapsed(0x10, 0xFFFFFFF0, 33));
    assert(am::elapsed(0x10, 0xFFFFFFF0, 32));
}

void reinitialize() {
    fakeMillis = modeStarted + RECOVERY_MS;
    loop();
}

#if NODE_ROLE_A
void sendNext() {
    assert(mode == Mode::Idle);
    fakeMillis = modeStarted + EXCHANGE_INTERVAL_MS;
    loop();
    assert(mode == Mode::WaitingEcho && radio.listening);
}

void testRole() {
    assert(mode == Mode::Idle);
    sendNext();
    radio.inject(outgoing, sizeof(outgoing));
    loop();
    assert(counters.successes == 1 && mode == Mode::Idle);
    assert(!radio.listening);

    sendNext();
    const uint32_t deadlineStart = modeStarted;
    uint8_t stale[am::FRAME_SIZE];
    am::makeFrame(stale, session, am::readU32(outgoing + 8) - 1);
    radio.inject(stale, sizeof(stale));
    loop();
    assert(counters.rejected == 1 && modeStarted == deadlineStart);
    // Same sequence from a prior boot cannot satisfy this exchange.
    am::makeFrame(stale, session ^ 1, am::readU32(outgoing + 8));
    radio.inject(stale, sizeof(stale));
    loop();
    assert(counters.rejected == 2 && modeStarted == deadlineStart);
    radio.inject(outgoing, sizeof(outgoing), RADIOLIB_ERR_CRC_MISMATCH);
    loop();
    assert(counters.rxErrors == 1 && radio.listening);
    assert(modeStarted == deadlineStart);
    stale[0] = 0;
    radio.inject(stale, sizeof(stale));
    loop();
    assert(counters.rejected == 3 && modeStarted == deadlineStart);
    // Even a queued valid echo cannot pass after the fixed deadline.
    radio.inject(outgoing, sizeof(outgoing));
    fakeMillis = deadlineStart + RX_TIMEOUT_MS;
    loop();
    assert(counters.timeouts == 1 && counters.successes == 1);
    assert(mode == Mode::Idle && !radio.listening);

    sendNext();
    radio.inject(outgoing, sizeof(outgoing));
    loop();
    assert(counters.successes == 2);

    radio.txResult = -5;
    fakeMillis = modeStarted + EXCHANGE_INTERVAL_MS;
    loop();
    assert(mode == Mode::Recover && counters.txErrors == 1);
    radio.txResult = 0;
    reinitialize();

    radio.rxResult = -1;
    fakeMillis = modeStarted + EXCHANGE_INTERVAL_MS;
    loop();
    assert(mode == Mode::Recover && counters.rxErrors == 2);
    radio.rxResult = 0;
    reinitialize();

    // A silent responder must time out even across millis() rollover.
    fakeMillis = 0xFFFFFF00;
    beginExchange();
    fakeMillis = modeStarted + RX_TIMEOUT_MS;
    loop();
    assert(counters.timeouts == 2 && mode == Mode::Idle);
}
#else
void testRole() {
    assert(mode == Mode::Listening && radio.listening);
    uint8_t frame[am::FRAME_SIZE];
    am::makeFrame(frame, 0x89ABCDEF, 42);
    radio.inject(frame, sizeof(frame));
    loop();
    assert(mode == Mode::Turnaround && radio.txCount == 0);
    fakeMillis = modeStarted + REPLY_GUARD_MS - 1;
    loop();
    assert(radio.txCount == 0);
    fakeMillis = modeStarted + REPLY_GUARD_MS;
    loop();
    assert(mode == Mode::Listening && radio.listening);
    assert(radio.txCount == 1);
    assert(memcmp(radio.output.data(), frame, sizeof(frame)) == 0);
    // B never initiates on its own, including after the RX watchdog.
    fakeMillis = modeStarted + LISTEN_WINDOW_MS;
    loop();
    assert(radio.txCount == 1 && radio.listening);

    radio.inject(frame, sizeof(frame), RADIOLIB_ERR_CRC_MISMATCH);
    loop();
    assert(counters.rxErrors == 1 && radio.txCount == 1 && radio.listening);
    frame[0] = 0;
    radio.inject(frame, sizeof(frame));
    loop();
    assert(counters.rejected == 1 && radio.txCount == 1 && radio.listening);

    am::makeFrame(frame, 0x89ABCDEF, 43);
    radio.inject(frame, sizeof(frame));
    loop();
    radio.txResult = -5;
    fakeMillis = modeStarted + REPLY_GUARD_MS;
    loop();
    assert(mode == Mode::Recover && counters.txErrors == 1);
    radio.txResult = 0;
    reinitialize();
    assert(mode == Mode::Listening && radio.listening);

    radio.rxResult = -1;
    fakeMillis = modeStarted + LISTEN_WINDOW_MS;
    loop();
    assert(mode == Mode::Recover);
    radio.rxResult = 0;
    reinitialize();
    assert(mode == Mode::Listening && radio.listening);
}
#endif

int main() {
    testProtocol();
    setup();
    assert(!radio.initialized);  // setup must not touch SPI before begin().
    radio.beginResult = -2;
    reinitialize();
    assert(mode == Mode::Recover);
    radio.beginResult = 0;
    reinitialize();
    assert(radio.initCount == 2);
    assert(radio.ook && radio.crc && !radio.addressFiltering);
    assert(radio.packetLength == am::FRAME_SIZE);
    testRole();
    printf("Node %c: protocol and runtime tests passed\n", NODE_ROLE_A ? 'A' : 'B');
}
