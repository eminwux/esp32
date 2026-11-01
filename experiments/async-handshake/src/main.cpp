#include <Arduino.h>
#include <esp_system.h>
#include "config.h"
#include "decoder.h"
#include "handshake.h"
#include "bit_io.h"
#include "radio.h"
#include "timing.h"


Radio radio;
ook::Handshake handshake(config::kLocal);
ook::SampleDecoder decoder;
uint32_t bootMs, txStartMs = 0, lastEndMs = 0, offTimeMs = 0;
uint32_t replyStartMs = 0, queuedMs = 0, retryStartMs = 0, retryWaitMs = 0;
uint32_t good = 0, crcErrors = 0, rejects = 0, decoderTimeouts = 0, losses = 0;
uint32_t rawRuns = 0, rawRun = 0, rawMin = UINT32_MAX, rawMax = 0, lastStats = 0;
bool rawPrevious = false, haveTx = false, healthy = false, stopped = false, recovering = true;

const char* messageName(uint8_t type) { return type == ook::kPing ? "PING" : "PONG_PING"; }
uint32_t airtimeWait(uint32_t now) {
    return haveTx ? ook::remainingMs(now, lastEndMs, offTimeMs) : 0;
}
uint32_t responseWait(uint32_t now) {
    return ook::sendWaitMs(ook::remainingMs(now, queuedMs, TURNAROUND_DELAY_MS), airtimeWait(now));
}
void logFrame(const char* direction, const ook::Frame& f) {
    if (f.type == ook::kPing)
        logMessage(direction, "PING %c -> %c: sequence=%u, acknowledgment=none, session=%08lx",
                   'A' + f.source - 1, 'A' + f.destination - 1, f.seq, (unsigned long)f.session);
    else
        logMessage(direction, "PONG_PING %c -> %c: sequence=%u, acknowledges=%u, session=%08lx",
                   'A' + f.source - 1, 'A' + f.destination - 1, f.seq, f.ack, (unsigned long)f.session);
}
void logPending(uint32_t now) {
    logMessage("WAIT", "%s sequence=%u queued; send in %lu ms (airtime cooldown=%lu ms, turnaround=%lu ms); listening to %c",
               messageName(handshake.next().type), handshake.next().seq,
               (unsigned long)responseWait(now), (unsigned long)airtimeWait(now),
               (unsigned long)ook::remainingMs(now, queuedMs, TURNAROUND_DELAY_MS), config::kPeerLabel);
}

void pauseSamples() {
    bit_io::pause();
    decoder.reset();
}
bool enterRx() {
    pauseSamples();
    if (!radio.receive(config::kRxHz)) return false;
    bit_io::startReceiving();
    return true;
}
void recovery() {
    handshake.timeout(); recovering = true;
    retryStartMs = millis();
    retryWaitMs = RETRY_INTERVAL_MS + esp_random() % 1000;
}
void accountTx() {
    // Count the entire radio TX state, not just logical one-bits. Preserve
    // debt over errors/recovery so retries cannot bypass the duty budget.
    lastEndMs = millis(); haveTx = true;
    uint32_t duration = lastEndMs - txStartMs + 1;
    offTimeMs = ook::silenceMs(duration, DUTY_PERMILLE, MIN_INTERFRAME_MS);
}
void radioFault() {
    pauseSamples(); digitalWrite(Radio::kData, LOW);
    radio.idle(); pinMode(Radio::kData, INPUT);
    healthy = false; recovery();
    logMessage("FAULT", "Radio/timing failure; discarded RX data. Reinitializing after %lu ms backoff",
               (unsigned long)retryWaitMs);
}
bool beginTx() {
    pauseSamples();
    uint8_t txWire[ook::kWireSize];
    if (RAW_OOK_TEST) for (auto& b : txWire) b = 0xaa;
    else ook::encode(handshake.next(), txWire);
    txStartMs = millis();
    if (!radio.transmit(config::kTxHz)) { radio.idle(); accountTx(); return false; }
    bit_io::startTransmission(txWire, RAW_OOK_TEST ? 64 : ook::kWireSize * 8);
    return true;
}
void setup() {
    Serial.begin(115200);
    bootMs = millis();
    logMessage("START", "Node %c (%s), peer %c; TX=%lu Hz, RX=%lu Hz, frequency correction=%d steps",
               kLogNode, config::kLocal == 1 ? "initiator" : "responder", config::kPeerLabel,
               (unsigned long)config::kTxHz, (unsigned long)config::kRxHz, RF_FREQ_OFFSET);
    logMessage("TIMING", "Bit period=%lu us; turnaround=%u ms; reply timeout=%u ms; TX budget=%u.%u%%",
               (unsigned long)config::kBitUs, unsigned(TURNAROUND_DELAY_MS), unsigned(REPLY_TIMEOUT_MS),
               unsigned(DUTY_PERMILLE / 10), unsigned(DUTY_PERMILLE % 10));
    if (RF_SESSION_MS) logMessage("LIMIT", "RF run ends %lu ms after boot; reset both boards for another run",
                                 (unsigned long)RF_SESSION_MS);
    else logMessage("LIMIT", "Indefinite RF run enabled; airtime cooldown remains enforced");
    if (!bit_io::begin()) {
        stopped = true;
        logMessage("FAULT", "Could not allocate sampling timer; stopped");
        return;
    }
    healthy = radio.begin() && enterRx();
    if (!healthy) radioFault();
    if (healthy) { retryStartMs = millis(); retryWaitMs = 0; }
    if (healthy) logMessage("LISTEN", "Receiver settled on %lu Hz; %s", (unsigned long)config::kRxHz,
                            RAW_OOK_TEST ? "raw OOK test mode" :
                            config::kLocal == 1 ? "preparing initial PING" : "waiting for A's initial PING");
}
void loop() {
    if (stopped) { delay(10); return; }
    uint32_t now = millis();
    if (RF_SESSION_MS && now - bootMs >= RF_SESSION_MS) {
        pauseSamples(); digitalWrite(Radio::kData, LOW); radio.idle();
        pinMode(Radio::kData, INPUT); stopped = true;
        logMessage("STOP", "RF run reached %lu ms; requested radio IDLE. Reset both boards to run again",
                   (unsigned long)RF_SESSION_MS); return;
    }
    const auto events = bit_io::takeEvents();
    if (events.fault) {
        bool wasTx = events.txPending;
        radioFault();
        if (wasTx) { accountTx(); bit_io::acknowledgeTransmission(); }
        return;
    }
    if (!healthy) {
        if (now - retryStartMs >= retryWaitMs) {
            healthy = radio.begin() && enterRx();
            recovery();
            if (healthy) logMessage("LISTEN", "Radio recovered; receiver ready on %lu Hz", (unsigned long)config::kRxHz);
            else logMessage("FAULT", "Radio initialization failed; retrying in %lu ms", (unsigned long)retryWaitMs);
        }
        delay(1); return;
    }
    if (events.transmitting) { delay(1); return; } // Never log or use SPI during bit generation.
    if (events.completed) {
        if (!radio.idle()) { accountTx(); bit_io::acknowledgeTransmission(); radioFault(); return; }
        accountTx(); bit_io::acknowledgeTransmission();
        // Resume RX before logging so serial output cannot delay peer reception.
        if (!enterRx()) { radioFault(); return; }
        if (!RAW_OOK_TEST) {
            const auto f = handshake.next();
            handshake.sent(); replyStartMs = lastEndMs;
            logFrame("SENT", f);
            logMessage("LISTEN", "Waiting for %c's reply on %lu Hz (timeout in %lu ms); local TX cooldown=%lu ms",
                       config::kPeerLabel, (unsigned long)config::kRxHz,
                       (unsigned long)ook::remainingMs(millis(), replyStartMs, REPLY_TIMEOUT_MS),
                       (unsigned long)airtimeWait(millis()));
        } else {
            logMessage("SENT", "64 alternating raw OOK bits; next burst in %lu ms; receiver ready",
                       (unsigned long)airtimeWait(millis()));
        }
    }
    // Drain captured samples outside the ISR, with a bounded loop. Serial and
    // SPI never run in the timer callback; an overrun invalidates the candidate.
    for (unsigned i = 0; i < bit_io::kSampleBufferSize; ++i) {
        bool level;
        if (!bit_io::popSample(level)) break;
        if (RAW_OOK_TEST) {
            if (level == rawPrevious) ++rawRun;
            else {
                if (rawRun < rawMin) rawMin = rawRun;
                if (rawRun > rawMax) rawMax = rawRun;
                ++rawRuns; rawRun = 1; rawPrevious = level;
            }
            continue;
        }
        ook::Frame f;
        ook::Result result = decoder.sample(level, config::kLocal, f);
        if (result == ook::Result::Good) {
            bool awaitedReply = handshake.waiting() && f.type == ook::kPongPing;
            if (handshake.receive(f)) {
                ++good; queuedMs = millis(); recovering = false;
                logFrame("RECEIVED", f);
                if (awaitedReply) logMessage("REPLY", "Accepted %c's reply %lu ms after our TX completed",
                                             config::kPeerLabel, (unsigned long)(queuedMs - replyStartMs));
                else logMessage("SESSION", "Accepted A's new session; preparing first response");
                logPending(millis());
            } else ++rejects;
        } else if (result == ook::Result::Crc) ++crcErrors;
        else if (result == ook::Result::Timeout) ++decoderTimeouts;
        else if (result != ook::Result::None) ++rejects;
    }
    now = millis();
    if (!RAW_OOK_TEST && handshake.waiting() && now - replyStartMs >= REPLY_TIMEOUT_MS) {
        ++losses; recovery(); decoder.reset();
        if (config::kLocal == 1)
            logMessage("TIMEOUT", "No valid reply from B within %u ms; retired session. New PING after %lu ms backoff and airtime cooldown",
                       unsigned(REPLY_TIMEOUT_MS), (unsigned long)retryWaitMs);
        else logMessage("TIMEOUT", "No valid reply from A within %u ms; retired session. Listening for A's new PING",
                        unsigned(REPLY_TIMEOUT_MS));
    }
    if (!RAW_OOK_TEST && config::kLocal == 1 && recovering && now - retryStartMs >= retryWaitMs) {
        handshake.recover(esp_random()); queuedMs = now; recovering = false;
        logMessage("SESSION", "Starting exchange with B; new session=%08lx", (unsigned long)handshake.next().session);
        logPending(millis());
    }
    bool wantTx = RAW_OOK_TEST ? config::kLocal == 1 :
        handshake.pending() && responseWait(now) == 0;
    if (wantTx && airtimeWait(now) == 0) {
        if (!beginTx()) radioFault();
        return;
    }
    if (now - lastStats >= 1000) {
        lastStats = now;
        const auto timing = bit_io::takeTimingStats();
        if (RAW_OOK_TEST) {
            logMessage("RAW", "Observed %lu level runs; shortest=%lu, longest=%lu samples (104 us/sample)",
                          (unsigned long)rawRuns, (unsigned long)(rawRuns ? rawMin : 0),
                          (unsigned long)rawMax);
            rawRuns = rawMax = 0; rawMin = UINT32_MAX;
        } else {
            if (handshake.pending()) logPending(now);
            else if (handshake.waiting())
                logMessage("WAIT", "Listening for %c's reply; timeout in %lu ms", config::kPeerLabel,
                           (unsigned long)ook::remainingMs(now, replyStartMs, REPLY_TIMEOUT_MS));
            else if (config::kLocal == 1)
                logMessage("WAIT", "Recovery backoff=%lu ms; airtime cooldown=%lu ms",
                           (unsigned long)ook::remainingMs(now, retryStartMs, retryWaitMs),
                           (unsigned long)airtimeWait(now));
            else logMessage("WAIT", "Listening for A's initial PING");
            logMessage("HEALTH", "Accepted=%lu, bad CRC=%lu, rejected=%lu, incomplete frames=%lu, reply timeouts=%lu",
                       (unsigned long)good, (unsigned long)crcErrors, (unsigned long)rejects,
                       (unsigned long)decoderTimeouts, (unsigned long)losses);
        }
        logMessage("TIMING", "Longest sample interval=%lu us (target 104 us); buffer overruns=%lu",
                   (unsigned long)timing.maxIntervalUs, (unsigned long)timing.overruns);
    }
    delay(1);
}
