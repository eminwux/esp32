#include <Arduino.h>
#include <esp_system.h>
#include <soc/gpio_struct.h>
#include "protocol.h"
#include "radio.h"
#include "timing.h"

#if defined(NODE_ROLE_A) == defined(NODE_ROLE_B)
#error Define exactly one node role
#endif
#ifdef NODE_ROLE_A
constexpr uint8_t kLocal = 1;
constexpr uint32_t kTxHz = RF_FREQ_A_HZ, kRxHz = RF_FREQ_B_HZ;
#else
constexpr uint8_t kLocal = 2;
constexpr uint32_t kTxHz = RF_FREQ_B_HZ, kRxHz = RF_FREQ_A_HZ;
#endif
constexpr uint32_t kBitUs = SAMPLE_PERIOD_US * 16;
constexpr uint32_t kFrameMs = (ook::kWireSize * 8 * kBitUs + 999) / 1000;
static_assert(SAMPLE_PERIOD_US == 104, "This RF profile and decoder are validated only for 104 us sampling");
static_assert(DUTY_PERMILLE > 0 && DUTY_PERMILLE <= 50, "Choose 1..50 per-mille TX budget");
static_assert(TURNAROUND_DELAY_MS >= 100, "Allow peer retune/calibration/settling");
static_assert(REPLY_TIMEOUT_MS > MIN_INTERFRAME_MS + kFrameMs + TURNAROUND_DELAY_MS + 100,
              "Receive window must cover peer spacing and airtime");
static_assert(REPLY_TIMEOUT_MS > (kFrameMs + 30) * 1000 / DUTY_PERMILLE + TURNAROUND_DELAY_MS + 100,
              "Receive window must cover peer duty-cycle wait");
static_assert(RAW_OOK_TEST == 0 || RAW_OOK_TEST == 1, "RAW_OOK_TEST must be 0 or 1");
static_assert(RF_FREQ_A_HZ >= 433050000UL && RF_FREQ_A_HZ <= 434790000UL &&
              RF_FREQ_B_HZ >= 433050000UL && RF_FREQ_B_HZ <= 434790000UL,
              "This PA/filter profile is restricted to the 433 MHz experiment");

Radio radio;
ook::Handshake handshake(kLocal);
ook::SampleDecoder decoder;
hw_timer_t* timer = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
constexpr unsigned kRing = 8192;
volatile uint8_t samples[kRing];
volatile unsigned head = 0, tail = 0;
uint8_t txWire[ook::kWireSize];
volatile unsigned txTicks = 0, txBits = 0;
volatile bool sampling = false, transmitting = false, txDone = false, timingFault = false;
volatile uint32_t previousTick = 0, maxTickUs = 0, overruns = 0;
uint32_t bootMs, txStartMs = 0, lastEndMs = 0, offTimeMs = 0;
uint32_t replyStartMs = 0, queuedMs = 0, retryStartMs = 0, retryWaitMs = 0;
uint32_t good = 0, crcErrors = 0, rejects = 0, decoderTimeouts = 0, losses = 0;
uint32_t rawRuns = 0, rawRun = 0, rawMin = UINT32_MAX, rawMax = 0, lastStats = 0;
bool rawPrevious = false, haveTx = false, healthy = false, stopped = false, recovering = true;

const char* messageName(uint8_t type) { return type == ook::kPing ? "PING" : "PONG_PING"; }
constexpr char kPeer = kLocal == 1 ? 'B' : 'A';
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
               (unsigned long)ook::remainingMs(now, queuedMs, TURNAROUND_DELAY_MS), kPeer);
}

void IRAM_ATTR tick() {
    portENTER_CRITICAL_ISR(&mux);
    uint32_t now = micros(), delta = now - previousTick;
    previousTick = now;
    if (delta > maxTickUs) maxTickUs = delta;
    if ((sampling || transmitting) && delta > SAMPLE_PERIOD_US * 3 / 2) {
        timingFault = true;
        sampling = transmitting = false;
        GPIO.out_w1tc = 1UL << Radio::kData;
    }
    if (transmitting) {
        if (txTicks % 16 == 0) {
            unsigned bit = txTicks / 16;
            if (bit == txBits) {
                GPIO.out_w1tc = 1UL << Radio::kData;
                transmitting = false; txDone = true;
            } else if (txWire[bit / 8] & (0x80 >> (bit % 8))) {
                GPIO.out_w1ts = 1UL << Radio::kData;
            } else GPIO.out_w1tc = 1UL << Radio::kData;
        }
        ++txTicks;
    } else if (sampling) {
        unsigned next = (head + 1) % kRing;
        if (next == tail) { ++overruns; timingFault = true; sampling = false; }
        else { samples[head] = (GPIO.in >> Radio::kData) & 1; head = next; }
    }
    portEXIT_CRITICAL_ISR(&mux);
}

void pauseSamples() {
    portENTER_CRITICAL(&mux);
    sampling = transmitting = false;
    head = tail = 0;
    portEXIT_CRITICAL(&mux);
    decoder.reset();
}
bool enterRx() {
    pauseSamples();
    if (!radio.receive(kRxHz)) return false;
    portENTER_CRITICAL(&mux);
    previousTick = micros(); sampling = true;
    portEXIT_CRITICAL(&mux);
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
    if (RAW_OOK_TEST) for (auto& b : txWire) b = 0xaa;
    else ook::encode(handshake.next(), txWire);
    txStartMs = millis();
    if (!radio.transmit(kTxHz)) { radio.idle(); accountTx(); return false; }
    portENTER_CRITICAL(&mux);
    txBits = RAW_OOK_TEST ? 64 : ook::kWireSize * 8;
    txTicks = 0; txDone = false; previousTick = micros(); transmitting = true;
    portEXIT_CRITICAL(&mux);
    return true;
}
void setup() {
    Serial.begin(115200);
    bootMs = millis();
    logMessage("START", "Node %c (%s), peer %c; TX=%lu Hz, RX=%lu Hz, frequency correction=%d steps",
               kLogNode, kLocal == 1 ? "initiator" : "responder", kPeer,
               (unsigned long)kTxHz, (unsigned long)kRxHz, RF_FREQ_OFFSET);
    logMessage("TIMING", "Bit period=%lu us; turnaround=%u ms; reply timeout=%u ms; TX budget=%u.%u%%",
               (unsigned long)kBitUs, unsigned(TURNAROUND_DELAY_MS), unsigned(REPLY_TIMEOUT_MS),
               unsigned(DUTY_PERMILLE / 10), unsigned(DUTY_PERMILLE % 10));
    if (RF_SESSION_MS) logMessage("LIMIT", "RF run ends %lu ms after boot; reset both boards for another run",
                                 (unsigned long)RF_SESSION_MS);
    else logMessage("LIMIT", "Indefinite RF run enabled; airtime cooldown remains enforced");
    timer = timerBegin(0, 80, true); // 80 MHz APB / 80 = 1 us.
    if (!timer) { stopped = true; logMessage("FAULT", "Could not allocate sampling timer; stopped"); return; }
    timerAttachInterrupt(timer, &tick, true);
    timerAlarmWrite(timer, SAMPLE_PERIOD_US, true);
    previousTick = micros(); timerAlarmEnable(timer);
    healthy = radio.begin() && enterRx();
    if (!healthy) radioFault();
    if (healthy) { retryStartMs = millis(); retryWaitMs = 0; }
    if (healthy) logMessage("LISTEN", "Receiver settled on %lu Hz; %s", (unsigned long)kRxHz,
                            RAW_OOK_TEST ? "raw OOK test mode" :
                            kLocal == 1 ? "preparing initial PING" : "waiting for A's initial PING");
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
    bool fault, done, sending;
    portENTER_CRITICAL(&mux);
    fault = timingFault; timingFault = false;
    done = txDone; txDone = false; sending = transmitting;
    portEXIT_CRITICAL(&mux);
    if (fault) {
        bool wasTx = txBits != 0;
        radioFault();
        if (wasTx) { accountTx(); txBits = 0; }
        return;
    }
    if (!healthy) {
        if (now - retryStartMs >= retryWaitMs) {
            healthy = radio.begin() && enterRx();
            recovery();
            if (healthy) logMessage("LISTEN", "Radio recovered; receiver ready on %lu Hz", (unsigned long)kRxHz);
            else logMessage("FAULT", "Radio initialization failed; retrying in %lu ms", (unsigned long)retryWaitMs);
        }
        delay(1); return;
    }
    if (sending) { delay(1); return; } // Never log or use SPI during bit generation.
    if (done) {
        if (!radio.idle()) { accountTx(); txBits = 0; radioFault(); return; }
        accountTx(); txBits = 0;
        // Resume RX before logging so serial output cannot delay peer reception.
        if (!enterRx()) { radioFault(); return; }
        if (!RAW_OOK_TEST) {
            const auto f = handshake.next();
            handshake.sent(); replyStartMs = lastEndMs;
            logFrame("SENT", f);
            logMessage("LISTEN", "Waiting for %c's reply on %lu Hz (timeout in %lu ms); local TX cooldown=%lu ms",
                       kPeer, (unsigned long)kRxHz,
                       (unsigned long)ook::remainingMs(millis(), replyStartMs, REPLY_TIMEOUT_MS),
                       (unsigned long)airtimeWait(millis()));
        } else {
            logMessage("SENT", "64 alternating raw OOK bits; next burst in %lu ms; receiver ready",
                       (unsigned long)airtimeWait(millis()));
        }
    }
    // Drain captured samples outside the ISR, with a bounded loop. Serial and
    // SPI never run in the timer callback; an overrun invalidates the candidate.
    for (unsigned i = 0; i < kRing; ++i) {
        bool level;
        portENTER_CRITICAL(&mux);
        if (tail == head) { portEXIT_CRITICAL(&mux); break; }
        level = samples[tail]; tail = (tail + 1) % kRing;
        portEXIT_CRITICAL(&mux);
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
        ook::Result result = decoder.sample(level, kLocal, f);
        if (result == ook::Result::Good) {
            bool awaitedReply = handshake.waiting() && f.type == ook::kPongPing;
            if (handshake.receive(f)) {
                ++good; queuedMs = millis(); recovering = false;
                logFrame("RECEIVED", f);
                if (awaitedReply) logMessage("REPLY", "Accepted %c's reply %lu ms after our TX completed",
                                             kPeer, (unsigned long)(queuedMs - replyStartMs));
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
        if (kLocal == 1)
            logMessage("TIMEOUT", "No valid reply from B within %u ms; retired session. New PING after %lu ms backoff and airtime cooldown",
                       unsigned(REPLY_TIMEOUT_MS), (unsigned long)retryWaitMs);
        else logMessage("TIMEOUT", "No valid reply from A within %u ms; retired session. Listening for A's new PING",
                        unsigned(REPLY_TIMEOUT_MS));
    }
    if (!RAW_OOK_TEST && kLocal == 1 && recovering && now - retryStartMs >= retryWaitMs) {
        handshake.recover(esp_random()); queuedMs = now; recovering = false;
        logMessage("SESSION", "Starting exchange with B; new session=%08lx", (unsigned long)handshake.next().session);
        logPending(millis());
    }
    bool wantTx = RAW_OOK_TEST ? kLocal == 1 :
        handshake.pending() && responseWait(now) == 0;
    if (wantTx && airtimeWait(now) == 0) {
        if (!beginTx()) radioFault();
        return;
    }
    if (now - lastStats >= 1000) {
        lastStats = now;
        uint32_t jitter, overflow;
        portENTER_CRITICAL(&mux);
        jitter = maxTickUs; maxTickUs = 0; overflow = overruns;
        portEXIT_CRITICAL(&mux);
        if (RAW_OOK_TEST) {
            logMessage("RAW", "Observed %lu level runs; shortest=%lu, longest=%lu samples (104 us/sample)",
                          (unsigned long)rawRuns, (unsigned long)(rawRuns ? rawMin : 0),
                          (unsigned long)rawMax);
            rawRuns = rawMax = 0; rawMin = UINT32_MAX;
        } else {
            if (handshake.pending()) logPending(now);
            else if (handshake.waiting())
                logMessage("WAIT", "Listening for %c's reply; timeout in %lu ms", kPeer,
                           (unsigned long)ook::remainingMs(now, replyStartMs, REPLY_TIMEOUT_MS));
            else if (kLocal == 1)
                logMessage("WAIT", "Recovery backoff=%lu ms; airtime cooldown=%lu ms",
                           (unsigned long)ook::remainingMs(now, retryStartMs, retryWaitMs),
                           (unsigned long)airtimeWait(now));
            else logMessage("WAIT", "Listening for A's initial PING");
            logMessage("HEALTH", "Accepted=%lu, bad CRC=%lu, rejected=%lu, incomplete frames=%lu, reply timeouts=%lu",
                       (unsigned long)good, (unsigned long)crcErrors, (unsigned long)rejects,
                       (unsigned long)decoderTimeouts, (unsigned long)losses);
        }
        logMessage("TIMING", "Longest sample interval=%lu us (target 104 us); buffer overruns=%lu",
                   (unsigned long)jitter, (unsigned long)overflow);
    }
    delay(1);
}
