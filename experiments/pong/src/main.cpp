
#include <Arduino.h>
#include <RadioLib.h>

// --------------------------------------------------
// Build configuration
// --------------------------------------------------

#ifndef NODE_ROLE_A
#define NODE_ROLE_A 1
#endif

#ifndef RF_CORRECTION_PPM
#define RF_CORRECTION_PPM 0
#endif

// ESP32 / CC1101 wiring
#define CS_PIN   5
#define GDO0_PIN 26
#define GDO2_PIN 27

// Communication frequencies
#define A_TO_B_HZ 433920000UL
#define B_TO_A_HZ 433500000UL

// Radio configuration
#define BIT_RATE_KBPS 4.8
#define FREQ_DEV_KHZ  5.0
#define RX_BW_KHZ     135.0
#define TX_POWER_DBM  -30

#define RX_TIMEOUT_MS 300

// CC1101: CS, GDO0, reset, GDO2
CC1101 radio = new Module(
    CS_PIN,
    GDO0_PIN,
    RADIOLIB_NC,
    GDO2_PIN
);

// --------------------------------------------------
// Frequency calibration
// --------------------------------------------------

uint32_t correctedFrequency(uint32_t targetHz) {
    int64_t corrected =
        (int64_t)targetHz *
        (1000000LL + RF_CORRECTION_PPM) /
        1000000LL;

    return (uint32_t)corrected;
}

bool tune(uint32_t targetHz) {
    uint32_t frequency = correctedFrequency(targetHz);

    // RadioLib's float overload expects MHz, not Hz.
    float frequencyMHz = frequency / 1000000.0f;

    Serial.printf("Tuning to %.6f MHz... ", frequencyMHz);

    int state = radio.setFrequency(frequencyMHz);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("failed, code %d\n", state);
        return false;
    }

    Serial.println("OK");
    return true;
}

// --------------------------------------------------
// Initialization
// --------------------------------------------------

void initializeRadio() {
    Serial.println("Initializing CC1101...");

    uint32_t initialFrequency =
        correctedFrequency(A_TO_B_HZ);

    int state = radio.begin(
        initialFrequency / 1000000.0,
        BIT_RATE_KBPS,
        FREQ_DEV_KHZ,
        RX_BW_KHZ,
        TX_POWER_DBM,
        32
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("Radio initialization failed: %d\n", state);
        while (true) delay(1000);
    }

    // Our own packet sync word
    state = radio.setSyncWord(0xA5, 0x5A);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("Sync word configuration failed: %d\n", state);
        while (true) delay(1000);
    }

    // Hardware CRC
    state = radio.setCrcFiltering(true);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("CRC configuration failed: %d\n", state);
        while (true) delay(1000);
    }

    Serial.println("CC1101 ready");
    Serial.printf("Crystal correction: %d ppm\n",
                  RF_CORRECTION_PPM);
}

// --------------------------------------------------
// Node A: Initiator
// --------------------------------------------------

void runNodeA() {
    static uint16_t sequence = 0;

    Serial.println("ROLE: NODE A");
    Serial.println("PING -> 433.920 MHz");
    Serial.println("PONG <- 433.500 MHz");

    while (true) {
        String ping = "PING:" + String(sequence);

        // Send PING to B
        if (!tune(A_TO_B_HZ)) {
            delay(1000);
            continue;
        }

        Serial.printf("TX: %s\n", ping.c_str());

        int txState = radio.transmit(ping);

        if (txState != RADIOLIB_ERR_NONE) {
            Serial.printf("TX failed: %d\n", txState);
            delay(500);
            continue;
        }

        // Listen for PONG from B
        if (!tune(B_TO_A_HZ)) {
            delay(1000);
            continue;
        }

        String response;

        int rxState = radio.receive(
            response,
            RX_TIMEOUT_MS
        );

        if (rxState == RADIOLIB_ERR_NONE) {
            String expected = "PONG:" + String(sequence);

            if (response == expected) {
                Serial.printf("RX: %s\n", response.c_str());
                Serial.printf(
                    "PING/PONG SUCCESS | RSSI: %.1f dBm\n",
                    radio.getRSSI()
                );
            } else {
                Serial.printf(
                    "Unexpected packet: %s\n",
                    response.c_str()
                );
            }

        } else if (rxState == RADIOLIB_ERR_RX_TIMEOUT) {
            Serial.println("PONG timeout");

        } else if (rxState == RADIOLIB_ERR_CRC_MISMATCH) {
            Serial.println("PONG CRC error");

        } else {
            Serial.printf("RX error: %d\n", rxState);
        }

        sequence++;

        delay(20);
    }
}

// --------------------------------------------------
// Node B: Responder
// --------------------------------------------------
void runNodeB() {
    while (true) {
        if (!tune(A_TO_B_HZ)) {
            delay(100);
            continue;
        }

        Serial.println("Waiting for PING...");

        String message;
        int rxState = radio.receive(message, RX_TIMEOUT_MS);

        if (rxState == RADIOLIB_ERR_NONE) {
            Serial.print("RX: ");
            Serial.println(message);

            Serial.print("RSSI: ");
            Serial.println(radio.getRSSI());

            if (message.startsWith("PING:")) {
                String sequence = message.substring(5);

                if (!tune(B_TO_A_HZ)) {
                    continue;
                }

                delay(500);

                String response = "PONG:" + sequence;

                Serial.print("TX: ");
                Serial.println(response);

                int txState = radio.transmit(response);            

                Serial.print("TX result: ");
                Serial.println(txState);
            }
        }
        else if (rxState == RADIOLIB_ERR_RX_TIMEOUT) {
            Serial.println("RX timeout");
        }
        else {
            Serial.print("RX error: ");
            Serial.println(rxState);
        }
    }
}

// --------------------------------------------------
// Arduino entry points
// --------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("==========================");
    Serial.println("CC1101 PING/PONG");
    Serial.println("==========================");

    initializeRadio();

#if NODE_ROLE_A
    runNodeA();
#else
    runNodeB();
#endif
}

void loop() {
    // Communication runs in setup().
}
