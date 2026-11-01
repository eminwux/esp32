#include <Arduino.h>
#include <RadioLib.h>
#include <esp_system.h>

#include "protocol.h"

#ifndef NODE_ROLE_A
#define NODE_ROLE_A 1
#endif
#ifndef RF_CORRECTION_PPM
#define RF_CORRECTION_PPM 0
#endif
#ifndef RF_FREQ_HZ
#define RF_FREQ_HZ 433920000UL
#endif
#ifndef REPLY_GUARD_MS
#define REPLY_GUARD_MS 50
#endif
#ifndef RX_TIMEOUT_MS
#define RX_TIMEOUT_MS 500
#endif

constexpr uint32_t EXCHANGE_INTERVAL_MS = 1000;
constexpr uint32_t LISTEN_WINDOW_MS = 2000;
constexpr uint32_t RECOVERY_MS = 1000;
// 4 preamble + 2 sync + 16 payload + 2 CRC bytes at 4.8 kbps = 40 ms.
constexpr uint32_t REPLY_AIRTIME_MS = 40;
static_assert(RX_TIMEOUT_MS > REPLY_GUARD_MS + REPLY_AIRTIME_MS + 20,
              "Receive timeout must cover guard, reply airtime and settling");

CC1101 radio = new Module(5, 26, RADIOLIB_NC, 27);
volatile bool packetReceived = false;

enum class Mode { Idle, WaitingEcho, Listening, Turnaround, Recover };
Mode mode = Mode::Idle;
uint32_t modeStarted = 0;
uint32_t txStarted = 0;
uint32_t sequence = 0;
uint32_t session = 0;
uint8_t outgoing[am::FRAME_SIZE];
uint8_t received[am::FRAME_SIZE];

struct Counters {
    uint32_t attempts = 0;
    uint32_t successes = 0;
    uint32_t timeouts = 0;
    uint32_t txErrors = 0;
    uint32_t rxErrors = 0;
    uint32_t rejected = 0;
} counters;

void IRAM_ATTR onPacketReceived() {
    packetReceived = true;
}

bool checkRadio(int16_t state, const char* operation) {
    if (state == RADIOLIB_ERR_NONE) {
        return true;
    }
    Serial.printf("%s failed: %d\n", operation, state);
    return false;
}

void stopReception() {
    radio.clearPacketReceivedAction();
    packetReceived = false;
}

void recover() {
    stopReception();
    checkRadio(radio.standby(), "Recovery standby");
    mode = Mode::Recover;
    modeStarted = millis();
}

bool initializeRadio() {
    stopReception();
    const int64_t correctedHz = static_cast<int64_t>(RF_FREQ_HZ) *
                               (1000000LL + RF_CORRECTION_PPM) / 1000000LL;
    const float frequencyMHz = correctedHz / 1000000.0f;
    if (!checkRadio(radio.begin(frequencyMHz, 4.8, 5.0, 135.0, -30, 32), "Begin") ||
        !checkRadio(radio.setOOK(true), "Enable OOK") ||
        !checkRadio(radio.setSyncWord(0xA5, 0x5A, 0, false), "Sync word") ||
        !checkRadio(radio.disableAddressFiltering(), "Disable address filter") ||
        !checkRadio(radio.fixedPacketLengthMode(am::FRAME_SIZE), "Fixed length") ||
        !checkRadio(radio.setCrcFiltering(true), "CRC")) {
        return false;
    }
    Serial.printf("OOK ready: %.6f MHz, correction %d ppm, 4.8 kbps, -30 dBm\n",
                  frequencyMHz, RF_CORRECTION_PPM);
    return true;
}

// Restart RX without changing the exchange's original deadline.
bool startReception() {
    stopReception();
    if (!checkRadio(radio.startReceive(), "Start RX")) {
        recover();
        return false;
    }
    // Attach after mapping GDO0 so its configuration cannot cause a false IRQ.
    radio.setPacketReceivedAction(onPacketReceived);
    return true;
}

void printFrame(const char* label, const uint8_t* frame) {
    Serial.printf("%s session=%08lX seq=%lu bytes=", label,
                  static_cast<unsigned long>(am::readU32(frame + 4)),
                  static_cast<unsigned long>(am::readU32(frame + 8)));
    for (size_t i = 0; i < am::FRAME_SIZE; ++i) {
        Serial.printf("%02X", frame[i]);
    }
    Serial.println();
}

void printStats() {
    Serial.printf("STATS attempts=%lu success=%lu timeout=%lu tx_error=%lu "
                  "rx_error=%lu rejected=%lu success_rate=%.1f%%\n",
                  static_cast<unsigned long>(counters.attempts),
                  static_cast<unsigned long>(counters.successes),
                  static_cast<unsigned long>(counters.timeouts),
                  static_cast<unsigned long>(counters.txErrors),
                  static_cast<unsigned long>(counters.rxErrors),
                  static_cast<unsigned long>(counters.rejected),
                  counters.attempts ? 100.0f * counters.successes / counters.attempts : 0.0f);
}

void finishExchange() {
    stopReception();
    if (!checkRadio(radio.standby(), "End RX")) {
        recover();
    } else {
        mode = Mode::Idle;
        modeStarted = millis();
    }
    printStats();
}

void beginExchange() {
    am::makeFrame(outgoing, session, sequence++);
    ++counters.attempts;
    printFrame("TX", outgoing);
    stopReception();
    txStarted = millis();
    if (!checkRadio(radio.transmit(outgoing, sizeof(outgoing)), "TX")) {
        ++counters.txErrors;
        printStats();
        recover();
        return;
    }
    mode = Mode::WaitingEcho;
    modeStarted = millis();
    // Enter RX before printing any further serial output.
    if (!startReception()) {
        ++counters.rxErrors;
        printStats();
    }
}

void handlePacket() {
    stopReception();
    const int16_t state = radio.readData(received, sizeof(received));
    if (!checkRadio(state, "Read RX")) {
        ++counters.rxErrors;
        startReception();
        return;
    }
    if (!am::validFrame(received, sizeof(received))) {
        ++counters.rejected;
        Serial.println("Rejected: wrong experiment or test pattern");
        startReception();
        return;
    }

#if NODE_ROLE_A
    if (memcmp(received, outgoing, sizeof(outgoing)) != 0) {
        ++counters.rejected;
        Serial.println("Rejected: stale or mismatched echo");
        startReception();
        return;
    }
    ++counters.successes;
    const uint32_t roundTrip = millis() - txStarted;
    printFrame("ECHO RX", received);
    Serial.printf("ROUNDTRIP SUCCESS | RTT: %lu ms | RSSI: %.1f dBm\n",
                  static_cast<unsigned long>(roundTrip), radio.getRSSI());
    finishExchange();
#else
    // readData() leaves the radio idle. Never regenerate or edit the payload.
    mode = Mode::Turnaround;
    modeStarted = millis();
    printFrame("RX", received);
    Serial.printf("RSSI: %.1f dBm\n", radio.getRSSI());
#endif
}

void setup() {
    Serial.begin(115200);
    delay(500);
    session = esp_random();
    Serial.printf("AM/OOK roundtrip | NODE %c | guard=%u ms timeout=%u ms\n",
                  NODE_ROLE_A ? 'A' : 'B', REPLY_GUARD_MS, RX_TIMEOUT_MS);
    // Do not access the radio before begin() initializes its SPI interface.
    mode = Mode::Recover;
    modeStarted = millis() - RECOVERY_MS;
}

void loop() {
    const uint32_t now = millis();
    if (mode == Mode::Recover) {
        if (am::elapsed(now, modeStarted, RECOVERY_MS)) {
            if (initializeRadio()) {
#if NODE_ROLE_A
                mode = Mode::Idle;
                modeStarted = millis();
#else
                mode = Mode::Listening;
                modeStarted = millis();
                startReception();
#endif
            } else {
                recover();
            }
        }
    } else if (mode == Mode::Idle) {
        if (am::elapsed(now, modeStarted, EXCHANGE_INTERVAL_MS)) {
            beginExchange();
        }
    } else if (mode == Mode::WaitingEcho || mode == Mode::Listening) {
        // Check the deadline first: unrelated packets cannot extend it.
        const uint32_t window = mode == Mode::WaitingEcho ? RX_TIMEOUT_MS : LISTEN_WINDOW_MS;
        if (am::elapsed(now, modeStarted, window)) {
            if (mode == Mode::WaitingEcho) {
                ++counters.timeouts;
                Serial.println("ECHO timeout");
                finishExchange();
            } else {
                // Clear any incomplete frame/overflow and resume listening.
                modeStarted = now;
                startReception();
            }
        } else if (packetReceived) {
            handlePacket();
        }
    } else if (mode == Mode::Turnaround && am::elapsed(now, modeStarted, REPLY_GUARD_MS)) {
        const int16_t state = radio.transmit(received, sizeof(received));
        if (!checkRadio(state, "Echo TX")) {
            ++counters.txErrors;
            recover();
        } else {
            mode = Mode::Listening;
            modeStarted = millis();
            startReception();
            printFrame("ECHO TX", received);
        }
    }
    delay(1);
}
