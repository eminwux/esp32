#include <Arduino.h>
#include <esp_system.h>
#include <soc/gpio_struct.h>
#include "protocol.h"
#include "radio.h"

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
    offTimeMs = uint64_t(duration) * (1000 - DUTY_PERMILLE) / DUTY_PERMILLE;
    if (offTimeMs < MIN_INTERFRAME_MS) offTimeMs = MIN_INTERFRAME_MS;
}
void radioFault() {
    pauseSamples(); digitalWrite(Radio::kData, LOW);
    radio.idle(); pinMode(Radio::kData, INPUT);
    healthy = false; recovery();
    Serial.println("FAULT: radio/timing; RX discarded, retrying initialization");
}
bool airtimeReady(uint32_t now) { return !haveTx || now - lastEndMs >= offTimeMs; }
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
    Serial.printf("Async OOK node=%u TX=%lu RX=%lu offset=%d steps bit_us=%lu\n",
                  kLocal, (unsigned long)kTxHz, (unsigned long)kRxHz, RF_FREQ_OFFSET,
                  (unsigned long)kBitUs);
    timer = timerBegin(0, 80, true); // 80 MHz APB / 80 = 1 us.
    if (!timer) { stopped = true; Serial.println("FAULT: timer allocation"); return; }
    timerAttachInterrupt(timer, &tick, true);
    timerAlarmWrite(timer, SAMPLE_PERIOD_US, true);
    previousTick = micros(); timerAlarmEnable(timer);
    healthy = radio.begin() && enterRx();
    if (!healthy) radioFault();
    retryStartMs = millis(); retryWaitMs = 0;
    Serial.println("RX listening; timing and RF behavior require bench verification");
}
void loop() {
    if (stopped) { delay(10); return; }
    uint32_t now = millis();
    if (RF_SESSION_MS && now - bootMs >= RF_SESSION_MS) {
        pauseSamples(); digitalWrite(Radio::kData, LOW); radio.idle();
        pinMode(Radio::kData, INPUT); stopped = true;
        Serial.println("STOP: RF session limit; reset boards to run again"); return;
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
        }
        delay(1); return;
    }
    if (sending) { delay(1); return; } // Never log or use SPI during bit generation.
    if (done) {
        if (!radio.idle()) { accountTx(); txBits = 0; radioFault(); return; }
        accountTx(); txBits = 0;
        if (!RAW_OOK_TEST) {
            const auto f = handshake.next();
            handshake.sent(); replyStartMs = millis();
            Serial.printf("TX type=%u seq=%u ack=%u session=%08lx\n", f.type, f.seq, f.ack,
                          (unsigned long)f.session);
        }
        if (!enterRx()) { radioFault(); return; }
        Serial.println("RX settled");
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
            if (handshake.receive(f)) {
                ++good; queuedMs = millis(); recovering = false;
                Serial.printf("RX seq=%u ack=%u session=%08lx latency_ms=%lu; reply queued\n",
                              f.seq, f.ack, (unsigned long)f.session,
                              (unsigned long)(millis() - replyStartMs));
            } else ++rejects;
        } else if (result == ook::Result::Crc) ++crcErrors;
        else if (result == ook::Result::Timeout) ++decoderTimeouts;
        else if (result != ook::Result::None) ++rejects;
    }
    now = millis();
    if (!RAW_OOK_TEST && handshake.waiting() && now - replyStartMs >= REPLY_TIMEOUT_MS) {
        ++losses; recovery(); decoder.reset();
        Serial.println("TIMEOUT: retire session; A will retry, B listens for a new PING");
    }
    if (!RAW_OOK_TEST && kLocal == 1 && recovering && now - retryStartMs >= retryWaitMs) {
        handshake.recover(esp_random()); queuedMs = now; recovering = false;
    }
    bool wantTx = RAW_OOK_TEST ? kLocal == 1 :
        handshake.pending() && now - queuedMs >= TURNAROUND_DELAY_MS;
    if (wantTx && airtimeReady(now)) {
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
            Serial.printf("raw runs=%lu min=%lu max=%lu samples (104us/sample)\n",
                          (unsigned long)rawRuns, (unsigned long)(rawRuns ? rawMin : 0),
                          (unsigned long)rawMax);
            rawRuns = rawMax = 0; rawMin = UINT32_MAX;
        } else Serial.printf("stats good=%lu crc=%lu rejected=%lu decoder_timeout=%lu lost=%lu\n",
                              (unsigned long)good, (unsigned long)crcErrors, (unsigned long)rejects,
                              (unsigned long)decoderTimeouts, (unsigned long)losses);
        Serial.printf("timer max_interval_us=%lu overruns=%lu\n", (unsigned long)jitter,
                      (unsigned long)overflow);
    }
    delay(1);
}
