#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "logging.h"
#include "config.h"

// Datasheet names keep register dumps searchable in TI's register tables.
namespace cc1101 {
enum Register : uint8_t {
    IOCFG2 = 0x00,
    IOCFG1 = 0x01,
    IOCFG0 = 0x02,
    SYNC1 = 0x04,
    SYNC0 = 0x05,
    PKTCTRL1 = 0x07,
    PKTCTRL0 = 0x08,
    CHANNR = 0x0a,
    FSCTRL1 = 0x0b,
    FSCTRL0 = 0x0c,
    FREQ2 = 0x0d,
    FREQ1 = 0x0e,
    FREQ0 = 0x0f,
    MDMCFG4 = 0x10,
    MDMCFG3 = 0x11,
    MDMCFG2 = 0x12,
    MDMCFG1 = 0x13,
    MDMCFG0 = 0x14,
    DEVIATN = 0x15,
    MCSM2 = 0x16,
    MCSM1 = 0x17,
    MCSM0 = 0x18,
    FOCCFG = 0x19,
    BSCFG = 0x1a,
    AGCCTRL2 = 0x1b,
    AGCCTRL1 = 0x1c,
    AGCCTRL0 = 0x1d,
    FREND1 = 0x21,
    FREND0 = 0x22,
    FSCAL3 = 0x23,
    FSCAL2 = 0x24,
    FSCAL1 = 0x25,
    FSCAL0 = 0x26,
    TEST2 = 0x2c,
    TEST1 = 0x2d,
    TEST0 = 0x2e,
    PARTNUM = 0x30,
    VERSION = 0x31,
    MARCSTATE = 0x35,
    PATABLE = 0x3e,
};
enum Strobe : uint8_t { SRES = 0x30, SCAL = 0x33, SRX = 0x34, STX = 0x35, SIDLE = 0x36 };
enum State : uint8_t { IDLE = 0x01, RX = 0x0d, TX = 0x13 };
constexpr uint8_t kRead = 0x80, kBurst = 0x40, kStateMask = 0x1f;
constexpr uint8_t kHighImpedance = 0x2e, kAsyncRxData = 0x0d;
}  // namespace cc1101

// CC1101 SWRS061I: sections 10, 12-14, 24-27 and register tables.
// GDO0 is baseband data, never a software-generated RF carrier.
class Cc1101Radio {
    static constexpr int kCs = config::kCsPin, kMiso = config::kMisoPin;
    SPISettings settings_{1000000, MSBFIRST, SPI_MODE0};
    bool waitForSpiReady() {
        uint32_t start = micros();
        while (digitalRead(kMiso)) if (micros() - start > 5000) return false;
        return true;
    }
    bool selectChip() {
        SPI.beginTransaction(settings_);
        digitalWrite(kCs, LOW);
        if (waitForSpiReady()) return true;
        releaseChip();
        return false;
    }
    void releaseChip() { digitalWrite(kCs, HIGH); SPI.endTransaction(); }
    bool writeRegisters(uint8_t address, const uint8_t* data, size_t n) {
        if (!selectChip()) return false;
        SPI.transfer(address | (n > 1 ? cc1101::kBurst : 0));
        for (size_t i = 0; i < n; ++i) SPI.transfer(data[i]);
        releaseChip();
        return true;
    }
    bool writeVerified(uint8_t address, uint8_t value) {
        uint8_t actual;
        return writeRegisters(address, &value, 1) && readRegister(address, actual) && actual == value;
    }
    bool waitForState(uint8_t wanted) {
        uint32_t start = millis();
        do {
            uint8_t a, b;
            // Status registers need READ|BURST even for one byte. Double-read
            // avoids trusting an incoherent status during a radio transition.
            if (!readRegister(cc1101::MARCSTATE, a, true) || !readRegister(cc1101::MARCSTATE, b, true)) return false;
            if ((a & cc1101::kStateMask) == wanted && a == b) return true;
            delay(1);
        } while (millis() - start < 25);
        return false;
    }
    bool retuneAndCalibrate(uint32_t hz) {
        if (!enterIdle()) return false;
        // RF_FREQ_OFFSET is signed synthesizer-word steps, not Hz or ppm.
        int64_t word = (uint64_t(hz) * 65536 + 13000000) / 26000000;
        word += RF_FREQ_OFFSET;
        if (word < 0 || word > 0xffffff) return false;
        uint8_t bytes[] = {uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)};
        if (!writeRegisters(cc1101::FREQ2, bytes, 3)) return false;
        uint8_t actual[3];
        if (!readRegisters(cc1101::FREQ2, actual, sizeof(actual))) return false;
        for (unsigned i = 0; i < 3; ++i) {
            if (actual[i] != bytes[i]) return false;
        }
        if (!sendStrobe(cc1101::SCAL)) return false;  // SCAL, manual every retune, including same-frequency.
        delay(2);                      // Do not mistake pre-calibration IDLE for completion.
        return waitForState(cc1101::IDLE);
    }
public:
    static constexpr int kData = config::kDataPin;
    bool readRegisters(uint8_t address, uint8_t* values, size_t n) {
        if (!selectChip()) return false;
        SPI.transfer(address | cc1101::kRead | cc1101::kBurst);
        for (size_t i = 0; i < n; ++i) values[i] = SPI.transfer(0);
        releaseChip();
        return true;
    }
    bool readRegister(uint8_t address, uint8_t& value, bool status = false) {
        if (!selectChip()) return false;
        SPI.transfer(address | (cc1101::kRead | (status ? cc1101::kBurst : 0)));
        value = SPI.transfer(0);
        releaseChip();
        return true;
    }
    bool sendStrobe(uint8_t command) {
        if (!selectChip()) return false;
        SPI.transfer(command);
        bool ok = command != cc1101::SRES || waitForSpiReady();  // SRES requires a second SO-ready wait.
        releaseChip();
        return ok;
    }
    bool enterIdle() { return sendStrobe(cc1101::SIDLE) && waitForState(cc1101::IDLE); }
    bool initialize() {
        pinMode(kData, INPUT);
        pinMode(kCs, OUTPUT);
        digitalWrite(kCs, HIGH);
        SPI.begin(config::kClockPin, kMiso, config::kMosiPin, kCs);
        // Manual reset sequence, before any pin is driven against reset GDO0.
        digitalWrite(kCs, LOW);
        delayMicroseconds(10);
        digitalWrite(kCs, HIGH);
        delayMicroseconds(50);
        if (!sendStrobe(cc1101::SRES) || !waitForState(cc1101::IDLE)) return false;
        uint8_t part, version;
        if (!readRegister(cc1101::PARTNUM, part, true) || !readRegister(cc1101::VERSION, version, true) ||
            part != 0 || version == 0 || version == 0xff) return false;
        logMessage("RADIO", "CC1101 detected (part=0x%02x, silicon version=0x%02x)", part, version);
        const uint8_t registerProfile[][2] = {
            {cc1101::IOCFG2, 0x2e}, {cc1101::IOCFG1, 0x2e}, {cc1101::IOCFG0, 0x2e}, // All GDOs initially high-Z.
            {cc1101::SYNC1, 0x00}, {cc1101::SYNC0, 0x00}, // Hardware sync unused.
            {cc1101::PKTCTRL1, 0x00}, {cc1101::PKTCTRL0, 0x30}, // No address/status/CRC/whitening; async format 3.
            {cc1101::CHANNR, 0x00}, {cc1101::FSCTRL1, 0x06}, {cc1101::FSCTRL0, 0x00}, // Channel 0, IF 152 kHz.
            {cc1101::MDMCFG4, 0xc4}, {cc1101::MDMCFG3, 0x84}, // BW=101.56 kHz; rate=601.29 baud.
            {cc1101::MDMCFG2, 0x30}, {cc1101::MDMCFG1, 0x00}, {cc1101::MDMCFG0, 0x00}, // OOK, no sync/Manchester/FEC.
            {cc1101::DEVIATN, 0x00}, {cc1101::MCSM2, 0x07}, // Deviation irrelevant; RX stays on.
            {cc1101::MCSM1, 0x00}, {cc1101::MCSM0, 0x08}, // No CCA/automatic transition or calibration.
            {cc1101::FOCCFG, 0x00}, {cc1101::BSCFG, 0x00}, // FOC_LIMIT=0 is required for OOK.
            // TI DN022 section 3.1 recommends AGCCTRL1=0 for OOK: reduce LNA2
            // gain before LNA gain. Keep magnitude target and slicer unchanged
            // for a controlled comparison with rx-diag-1's 03/40/91 profile.
            {cc1101::AGCCTRL2, 0x03}, {cc1101::AGCCTRL1, 0x00}, {cc1101::AGCCTRL0, 0x91},
            {cc1101::FREND1, 0x56}, {cc1101::FREND0, 0x11}, // RX frontend; PA index 1.
            {cc1101::FSCAL3, 0xe9}, {cc1101::FSCAL2, 0x2a}, {cc1101::FSCAL1, 0x00}, {cc1101::FSCAL0, 0x1f},
            {cc1101::TEST2, 0x81}, {cc1101::TEST1, 0x35}, {cc1101::TEST0, 0x09}
        };
        for (const auto& pair : registerProfile) if (!writeVerified(pair[0], pair[1])) return false;
        // Table 39, 433 MHz: index 0 OFF; index 1 nominal -30 dBm.
        const uint8_t pa[8] = {0, 0x12, 0, 0, 0, 0, 0, 0};
        if (!writeRegisters(cc1101::PATABLE, pa, sizeof(pa))) return false;
        uint8_t actual[8];
        if (!readRegisters(cc1101::PATABLE, actual, sizeof(actual))) return false;
        for (unsigned i = 0; i < 8; ++i) if (actual[i] != pa[i]) return false;
        if (LOG_REGISTERS) {
            for (const auto& pair : registerProfile)
                logMessage("REGISTER", "verified address=0x%02x value=0x%02x", pair[0], pair[1]);
            logMessage("REGISTER", "PATABLE=00,12,00,00,00,00,00,00");
        }
        logMessage("RADIO", "Configuration verified: async serial OOK; software framing and CRC; nominal TX power -30 dBm");
        logMessage("RADIO", "RX gain profile verified: AGCCTRL2/1/0=03/00/91 (rx-agc-2 trial)");
        return true;
    }
    // GPIO direction order is electrical safety: release the ESP32 output
    // before mapping the radio's RX output onto the same GDO0 wire.
    bool enterReceive(uint32_t hz) {
        pinMode(kData, INPUT);
        if (!retuneAndCalibrate(hz) || !writeVerified(cc1101::IOCFG0, cc1101::kAsyncRxData) || !sendStrobe(cc1101::SRX) || !waitForState(cc1101::RX)) return false;
        delay(20); // Discard transients; caller enables sampling only after settling.
        return true;
    }
    // First make the radio's GDO0 output high-Z, then allow the ESP32 to drive
    // baseband data. TX starts at logical zero until the timer emits the frame.
    bool enterTransmit(uint32_t hz) {
        pinMode(kData, INPUT);
        if (!retuneAndCalibrate(hz) || !writeVerified(cc1101::IOCFG0, cc1101::kHighImpedance)) return false;
        digitalWrite(kData, LOW);
        pinMode(kData, OUTPUT);
        return sendStrobe(cc1101::STX) && waitForState(cc1101::TX);
    }
};
