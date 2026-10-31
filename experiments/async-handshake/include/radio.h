#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "logging.h"

// CC1101 SWRS061I: sections 10, 12-14, 24-27 and register tables.
// GDO0 is baseband data, never a software-generated RF carrier.
class Radio {
    static constexpr int kCs = 5, kMiso = 19;
    SPISettings settings_{1000000, MSBFIRST, SPI_MODE0};
    bool ready() {
        uint32_t start = micros();
        while (digitalRead(kMiso)) if (micros() - start > 5000) return false;
        return true;
    }
    bool select() {
        SPI.beginTransaction(settings_); digitalWrite(kCs, LOW);
        if (ready()) return true;
        release(); return false;
    }
    void release() { digitalWrite(kCs, HIGH); SPI.endTransaction(); }
    bool write(uint8_t address, const uint8_t* data, size_t n) {
        if (!select()) return false;
        SPI.transfer(address | (n > 1 ? 0x40 : 0));
        for (size_t i = 0; i < n; ++i) SPI.transfer(data[i]);
        release(); return true;
    }
    bool reg(uint8_t address, uint8_t value) {
        uint8_t actual;
        return write(address, &value, 1) && read(address, actual) && actual == value;
    }
    bool state(uint8_t wanted) {
        uint32_t start = millis();
        do {
            uint8_t a, b;
            // Status registers need READ|BURST even for one byte. Double-read
            // avoids trusting an incoherent status during a radio transition.
            if (!read(0x35, a, true) || !read(0x35, b, true)) return false;
            if ((a & 31) == wanted && a == b) return true;
            delay(1);
        } while (millis() - start < 25);
        return false;
    }
    bool tune(uint32_t hz) {
        if (!idle()) return false;
        // RF_FREQ_OFFSET is signed synthesizer-word steps, not Hz or ppm.
        int64_t word = (uint64_t(hz) * 65536 + 13000000) / 26000000;
        word += RF_FREQ_OFFSET;
        if (word < 0 || word > 0xffffff) return false;
        uint8_t bytes[] = {uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)};
        if (!write(0x0d, bytes, 3)) return false;
        uint8_t actual[3];
        if (!readBurst(0x0d, actual, sizeof(actual))) return false;
        for (unsigned i = 0; i < 3; ++i) {
            if (actual[i] != bytes[i]) return false;
        }
        if (!strobe(0x33)) return false;  // SCAL, manual every retune, including same-frequency.
        delay(2);                      // Do not mistake pre-calibration IDLE for completion.
        return state(1);
    }
public:
    static constexpr int kData = 26;
    bool readBurst(uint8_t address, uint8_t* values, size_t n) {
        if (!select()) return false;
        SPI.transfer(address | 0xc0);
        for (size_t i = 0; i < n; ++i) values[i] = SPI.transfer(0);
        release(); return true;
    }
    bool read(uint8_t address, uint8_t& value, bool status = false) {
        if (!select()) return false;
        SPI.transfer(address | (status ? 0xc0 : 0x80));
        value = SPI.transfer(0); release(); return true;
    }
    bool strobe(uint8_t command) {
        if (!select()) return false;
        SPI.transfer(command);
        bool ok = command != 0x30 || ready();  // SRES requires a second SO-ready wait.
        release(); return ok;
    }
    bool idle() { return strobe(0x36) && state(1); }
    bool begin() {
        pinMode(kData, INPUT); pinMode(kCs, OUTPUT); digitalWrite(kCs, HIGH);
        SPI.begin(18, kMiso, 23, kCs);
        // Manual reset sequence, before any pin is driven against reset GDO0.
        digitalWrite(kCs, LOW); delayMicroseconds(10);
        digitalWrite(kCs, HIGH); delayMicroseconds(50);
        if (!strobe(0x30) || !state(1)) return false;
        uint8_t part, version;
        if (!read(0x30, part, true) || !read(0x31, version, true) ||
            part != 0 || version == 0 || version == 0xff) return false;
        logMessage("RADIO", "CC1101 detected (part=0x%02x, silicon version=0x%02x)", part, version);
        const uint8_t config[][2] = {
            {0x00, 0x2e}, {0x01, 0x2e}, {0x02, 0x2e}, // All GDOs initially high-Z.
            {0x04, 0x00}, {0x05, 0x00}, // Hardware sync unused.
            {0x07, 0x00}, {0x08, 0x30}, // No address/status/CRC/whitening; async format 3.
            {0x0a, 0x00}, {0x0b, 0x06}, {0x0c, 0x00}, // Channel 0, IF 152 kHz.
            {0x10, 0xc4}, {0x11, 0x84}, // BW=101.56 kHz; rate=601.29 baud.
            {0x12, 0x30}, {0x13, 0x00}, {0x14, 0x00}, // OOK, no sync/Manchester/FEC.
            {0x15, 0x00}, {0x16, 0x07}, // Deviation irrelevant; RX stays on.
            {0x17, 0x00}, {0x18, 0x08}, // No CCA/automatic transition or calibration.
            {0x19, 0x00}, {0x1a, 0x00}, // FOC_LIMIT=0 is required for OOK.
            {0x1b, 0x03}, {0x1c, 0x40}, {0x1d, 0x91}, // AGC; 8 dB OOK threshold.
            {0x21, 0x56}, {0x22, 0x11}, // RX frontend; PA index 1.
            {0x23, 0xe9}, {0x24, 0x2a}, {0x25, 0x00}, {0x26, 0x1f},
            {0x2c, 0x81}, {0x2d, 0x35}, {0x2e, 0x09}
        };
        for (const auto& pair : config) if (!reg(pair[0], pair[1])) return false;
        // Table 39, 433 MHz: index 0 OFF; index 1 nominal -30 dBm.
        const uint8_t pa[8] = {0, 0x12, 0, 0, 0, 0, 0, 0};
        if (!write(0x3e, pa, sizeof(pa))) return false;
        uint8_t actual[8];
        if (!readBurst(0x3e, actual, sizeof(actual))) return false;
        for (unsigned i = 0; i < 8; ++i) if (actual[i] != pa[i]) return false;
        if (LOG_REGISTERS) {
            for (const auto& pair : config)
                logMessage("REGISTER", "verified address=0x%02x value=0x%02x", pair[0], pair[1]);
            logMessage("REGISTER", "PATABLE=00,12,00,00,00,00,00,00");
        }
        logMessage("RADIO", "Configuration verified: async serial OOK; software framing and CRC; nominal TX power -30 dBm");
        return true;
    }
    bool receive(uint32_t hz) {
        pinMode(kData, INPUT);
        if (!tune(hz) || !reg(0x02, 0x0d) || !strobe(0x34) || !state(0x0d)) return false;
        delay(20); // Discard transients; caller enables sampling only after settling.
        return true;
    }
    bool transmit(uint32_t hz) {
        pinMode(kData, INPUT);
        if (!tune(hz) || !reg(0x02, 0x2e)) return false;
        digitalWrite(kData, LOW); pinMode(kData, OUTPUT);
        return strobe(0x35) && state(0x13);
    }
};
