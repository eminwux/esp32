#include <Arduino.h>
#include <esp_system.h>
#include "config.h"
#include "decoder.h"
#include "handshake.h"
#include "bit_io.h"
#include "radio.h"
#include "timing.h"
#include "rx_observation.h"

Cc1101Radio radio;
ook::Handshake handshake(config::kLocal);
ook::SampleDecoder decoder;
ook::RxObservation rxObservation;
// Foreground-owned state. Interrupt-owned data lives entirely in bit_io.cpp.
enum class AppState { Initializing, Ready, Stopped };
AppState appState = AppState::Initializing;
struct TxBudget {
    bool hasTransmission = false;
    uint32_t startedMs = 0, endedMs = 0, silenceMs = 0;
} txBudget;
struct SessionTiming {
    uint32_t replyStartedMs = 0, queuedMs = 0, retryStartedMs = 0, retryDelayMs = 0;
    bool recoveryPending = true;
} sessionTiming;
struct Counters {
    uint32_t accepted = 0, badCrc = 0, rejected = 0, decoderTimeouts = 0, replyTimeouts = 0;
} counters;
struct RawObservation {
    uint32_t runs = 0, currentRun = 0, shortest = UINT32_MAX, longest = 0;
    bool previousLevel = false;
} raw;
uint32_t bootMs = 0, lastStatsMs = 0;

const char* messageName(uint8_t type) { return type == ook::kPing ? "PING" : "PONG_PING"; }
uint32_t airtimeWait(uint32_t now) {
    return txBudget.hasTransmission ? ook::remainingMs(now, txBudget.endedMs, txBudget.silenceMs) : 0;
}
uint32_t responseWait(uint32_t now) {
    return ook::sendWaitMs(ook::remainingMs(now, sessionTiming.queuedMs, TURNAROUND_DELAY_MS), airtimeWait(now));
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
               messageName(handshake.queuedFrame().type), handshake.queuedFrame().seq,
               (unsigned long)responseWait(now), (unsigned long)airtimeWait(now),
               (unsigned long)ook::remainingMs(now, sessionTiming.queuedMs, TURNAROUND_DELAY_MS), config::kPeerLabel);
}

void pauseSamples() {
    bit_io::pause();
    decoder.reset();
    rxObservation.discontinuity();
}
bool enterRx() {
    pauseSamples();
    if (!radio.enterReceive(config::kRxHz)) return false;
    bit_io::startReceiving();
    return true;
}
void recovery() {
    handshake.retireSession();
    sessionTiming.recoveryPending = true;
    sessionTiming.retryStartedMs = millis();
    sessionTiming.retryDelayMs = RETRY_INTERVAL_MS + esp_random() % 1000;
}
void accountTx() {
    // Count the entire radio TX state, not just logical one-bits. Preserve
    // debt over errors/recovery so retries cannot bypass the duty budget.
    txBudget.endedMs = millis();
    txBudget.hasTransmission = true;
    uint32_t duration = txBudget.endedMs - txBudget.startedMs + 1;
    txBudget.silenceMs = ook::silenceMs(duration, DUTY_PERMILLE, MIN_INTERFRAME_MS);
}
void radioFault() {
    pauseSamples();
    digitalWrite(Cc1101Radio::kData, LOW);
    radio.enterIdle();
    pinMode(Cc1101Radio::kData, INPUT);
    appState = AppState::Initializing;
    recovery();
    logMessage("FAULT", "Radio/timing failure; discarded RX data. Reinitializing after %lu ms backoff",
               (unsigned long)sessionTiming.retryDelayMs);
}
bool beginTx() {
    pauseSamples();
    uint8_t txWire[ook::kWireSize];
    if (RAW_OOK_TEST) for (auto& b : txWire) b = 0xaa;
    else ook::encode(handshake.queuedFrame(), txWire);
    txBudget.startedMs = millis();
    if (!radio.enterTransmit(config::kTxHz)) {
        radio.enterIdle();
        accountTx();
        return false;
    }
    bit_io::startTransmission(txWire, RAW_OOK_TEST ? 64 : ook::kWireSize * 8);
    return true;
}
// SPI state changes and timing charges finish before a frame is considered sent.
bool finishTransmission() {
    if (!radio.enterIdle()) {
        accountTx();
        bit_io::acknowledgeTransmission();
        radioFault();
        return false;
    }
    accountTx();
    bit_io::acknowledgeTransmission();
    // Resume RX before logging so serial output cannot delay peer reception.
    if (!enterRx()) {
        radioFault();
        return false;
    }
    if (!RAW_OOK_TEST) {
        const auto f = handshake.queuedFrame();
        handshake.onTransmitComplete();
        sessionTiming.replyStartedMs = txBudget.endedMs;
        logFrame("SENT", f);
        logMessage("LISTEN", "Waiting for %c's reply on %lu Hz (timeout in %lu ms); local TX cooldown=%lu ms",
                   config::kPeerLabel, (unsigned long)config::kRxHz,
                   (unsigned long)ook::remainingMs(millis(), sessionTiming.replyStartedMs, REPLY_TIMEOUT_MS),
                   (unsigned long)airtimeWait(millis()));
    } else {
        logMessage("SENT", "64 alternating raw OOK bits; next burst in %lu ms; receiver ready",
                   (unsigned long)airtimeWait(millis()));
    }
    return true;
}

void processReceivedSamples() {
    // Drain captured samples outside the ISR, with a bounded loop. Serial and
    // SPI never run in the timer callback; an overrun invalidates the candidate.
    for (unsigned i = 0; i < bit_io::kSampleBufferSize; ++i) {
        bool level;
        if (!bit_io::popSample(level)) break;
        if (RAW_OOK_TEST) {
            if (level == raw.previousLevel) ++raw.currentRun;
            else {
                if (raw.currentRun < raw.shortest) raw.shortest = raw.currentRun;
                if (raw.currentRun > raw.longest) raw.longest = raw.currentRun;
                ++raw.runs;
                raw.currentRun = 1;
                raw.previousLevel = level;
            }
            continue;
        }
        ook::Frame f;
        const auto before = decoder.stage();
        ook::Result result = decoder.sample(level, config::kLocal, f);
        rxObservation.observe(level, before, decoder.stage());
        if (result == ook::Result::Good) {
            bool awaitedReply = handshake.awaitingReply() && f.type == ook::kPongPing;
            if (handshake.onFrameReceived(f)) {
                ++counters.accepted;
                sessionTiming.queuedMs = millis();
                sessionTiming.recoveryPending = false;
                logFrame("RECEIVED", f);
                if (awaitedReply) logMessage("REPLY", "Accepted %c's reply %lu ms after our TX completed",
                                             config::kPeerLabel, (unsigned long)(sessionTiming.queuedMs - sessionTiming.replyStartedMs));
                else logMessage("SESSION", "Accepted A's new session; preparing first response");
                logPending(millis());
            } else ++counters.rejected;
        } else if (result == ook::Result::Crc) ++counters.badCrc;
        else if (result == ook::Result::Timeout) ++counters.decoderTimeouts;
        else if (result != ook::Result::None) ++counters.rejected;
    }
}

void processHandshakeDeadlines(uint32_t now) {
    if (!RAW_OOK_TEST && handshake.awaitingReply() && now - sessionTiming.replyStartedMs >= REPLY_TIMEOUT_MS) {
        ++counters.replyTimeouts;
        recovery();
        decoder.reset();
        if (config::kLocal == 1)
            logMessage("TIMEOUT", "No valid reply from B within %u ms; retired session. New PING after %lu ms backoff and airtime cooldown",
                       unsigned(REPLY_TIMEOUT_MS), (unsigned long)sessionTiming.retryDelayMs);
        else logMessage("TIMEOUT", "No valid reply from A within %u ms; retired session. Listening for A's new PING",
                        unsigned(REPLY_TIMEOUT_MS));
    }
    if (!RAW_OOK_TEST && config::kLocal == 1 && sessionTiming.recoveryPending && now - sessionTiming.retryStartedMs >= sessionTiming.retryDelayMs) {
        handshake.startSession(esp_random());
        sessionTiming.queuedMs = now;
        sessionTiming.recoveryPending = false;
        logMessage("SESSION", "Starting exchange with B; new session=%08lx", (unsigned long)handshake.queuedFrame().session);
        logPending(millis());
    }
}

bool sendIfReady(uint32_t now) {
    bool wantTx = RAW_OOK_TEST ? config::kLocal == 1 :
        handshake.hasQueuedReply() && responseWait(now) == 0;
    if (wantTx && airtimeWait(now) == 0) {
        if (!beginTx()) radioFault();
        return true;
    }
    return false;
}

void reportStatusIfDue(uint32_t now) {
    if (now - lastStatsMs >= 1000) {
        lastStatsMs = now;
        const auto timing = bit_io::takeTimingStats();
        if (RAW_OOK_TEST) {
            logMessage("RAW", "Observed %lu level runs; shortest=%lu, longest=%lu samples (104 us/sample)",
                          (unsigned long)raw.runs, (unsigned long)(raw.runs ? raw.shortest : 0),
                          (unsigned long)raw.longest);
            raw.runs = raw.longest = 0;
            raw.shortest = UINT32_MAX;
        } else {
            if (handshake.hasQueuedReply()) logPending(now);
            else if (handshake.awaitingReply())
                logMessage("WAIT", "Listening for %c's reply; timeout in %lu ms", config::kPeerLabel,
                           (unsigned long)ook::remainingMs(now, sessionTiming.replyStartedMs, REPLY_TIMEOUT_MS));
            else if (config::kLocal == 1)
                logMessage("WAIT", "Recovery backoff=%lu ms; airtime cooldown=%lu ms",
                           (unsigned long)ook::remainingMs(now, sessionTiming.retryStartedMs, sessionTiming.retryDelayMs),
                           (unsigned long)airtimeWait(now));
            else logMessage("WAIT", "Listening for A's initial PING");
            logMessage("HEALTH", "Accepted=%lu, bad CRC=%lu, rejected=%lu, incomplete frames=%lu, reply timeouts=%lu",
                       (unsigned long)counters.accepted, (unsigned long)counters.badCrc, (unsigned long)counters.rejected,
                       (unsigned long)counters.decoderTimeouts, (unsigned long)counters.replyTimeouts);
            const auto stage = decoder.stage();
            logMessage("INPUT", "Since last report: samples=%lu, high=%lu, edges=%lu, preambles=%lu, syncs=%lu; seeking=%s",
                       (unsigned long)rxObservation.samples, (unsigned long)rxObservation.high,
                       (unsigned long)rxObservation.edges, (unsigned long)rxObservation.preambles,
                       (unsigned long)rxObservation.syncs,
                       stage == ook::SampleDecoder::Stage::Preamble ? "preamble" :
                       stage == ook::SampleDecoder::Stage::Sync ? "sync" : "body");
            rxObservation.clearWindow();
        }
        logMessage("TIMING", "Longest sample interval=%lu us (target 104 us); buffer overruns=%lu",
                   (unsigned long)timing.maxIntervalUs, (unsigned long)timing.overruns);
    }
}

void setup() {
    Serial.begin(115200);
    bootMs = millis();
    logMessage("BUILD", "async-ook rx-diag-1; compiled %s %s", __DATE__, __TIME__);
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
        appState = AppState::Stopped;
        logMessage("FAULT", "Could not allocate sampling timer; stopped");
        return;
    }
    appState = radio.initialize() && enterRx() ? AppState::Ready : AppState::Initializing;
    if (appState != AppState::Ready) radioFault();
    if (appState == AppState::Ready) {
        sessionTiming.retryStartedMs = millis();
        sessionTiming.retryDelayMs = 0;
    }
    if (appState == AppState::Ready) logMessage("LISTEN", "Receiver settled on %lu Hz; %s", (unsigned long)config::kRxHz,
                            RAW_OOK_TEST ? "raw OOK test mode" :
                            config::kLocal == 1 ? "preparing initial PING" : "waiting for A's initial PING");
}
// Cooperative foreground loop: faults first, then RX, deadlines, TX and logs.
// During ordinary TX, only the timer ISR emits bits. Stop/fault handling
// takes precedence; otherwise foreground SPI/logging waits for completion.
void loop() {
    if (appState == AppState::Stopped) {
        delay(10);
        return;
    }
    uint32_t now = millis();
    if (RF_SESSION_MS && now - bootMs >= RF_SESSION_MS) {
        pauseSamples();
        digitalWrite(Cc1101Radio::kData, LOW);
        radio.enterIdle();
        pinMode(Cc1101Radio::kData, INPUT);
        appState = AppState::Stopped;
        logMessage("STOP", "RF run reached %lu ms; requested radio IDLE. Reset both boards to run again",
                   (unsigned long)RF_SESSION_MS);
        return;
    }
    const auto events = bit_io::takeEvents();
    if (events.fault) {
        bool wasTx = events.txPending;
        radioFault();
        if (wasTx) {
            accountTx();
            bit_io::acknowledgeTransmission();
        }
        return;
    }
    if (appState != AppState::Ready) {
        if (now - sessionTiming.retryStartedMs >= sessionTiming.retryDelayMs) {
            appState = radio.initialize() && enterRx() ? AppState::Ready : AppState::Initializing;
            recovery();
            if (appState == AppState::Ready) logMessage("LISTEN", "Radio recovered; receiver ready on %lu Hz", (unsigned long)config::kRxHz);
            else logMessage("FAULT", "Radio initialization failed; retrying in %lu ms", (unsigned long)sessionTiming.retryDelayMs);
        }
        delay(1);
        return;
    }
    if (events.transmitting) {
        delay(1);
        return;
    }
    if (events.completed && !finishTransmission()) return;
    processReceivedSamples();
    now = millis();
    processHandshakeDeadlines(now);
    if (sendIfReady(now)) return;
    reportStatusIfDue(now);
    delay(1);
}
