
#include <Arduino.h>
#include <SPI.h>

// ESP32 pins
#define CS_PIN   5
#define SCK_PIN  18
#define MISO_PIN 19
#define MOSI_PIN 23
#define GDO0_PIN 26

#ifndef RF_FREQ_HZ
#define RF_FREQ_HZ 433920000UL
#endif

#ifndef RF_FREQ_OFFSET
#define RF_FREQ_OFFSET 0
#endif

#define RF_XOSC_HZ 26000000ULL

SPISettings settings(1000000, MSBFIRST, SPI_MODE0);

// Wait until CC1101 SPI interface is ready
bool waitReady() {
    unsigned long start = micros();

    while (digitalRead(MISO_PIN) == HIGH) {
        if (micros() - start > 100000) {
            return false;
        }
    }

    return true;
}

// Write a CC1101 configuration register
bool writeReg(uint8_t addr, uint8_t value) {
    SPI.beginTransaction(settings);
    digitalWrite(CS_PIN, LOW);

    bool ready = waitReady();

    if (ready) {
        SPI.transfer(addr);
        SPI.transfer(value);
    }

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();

    return ready;
}

// Read a CC1101 configuration register
uint8_t readReg(uint8_t addr) {
    SPI.beginTransaction(settings);
    digitalWrite(CS_PIN, LOW);

    if (!waitReady()) {
        digitalWrite(CS_PIN, HIGH);
        SPI.endTransaction();
        return 0xFF;
    }

    SPI.transfer(addr | 0x80);
    uint8_t value = SPI.transfer(0x00);

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();

    return value;
}


void configureFrequency() {
    // CC1101 frequency word:
    // FREQ = RF frequency * 2^16 / crystal frequency

    uint64_t numerator =
        (uint64_t)RF_FREQ_HZ * 65536ULL;

    int64_t word =
        (numerator + RF_XOSC_HZ / 2) / RF_XOSC_HZ;

    // Per-module crystal calibration
    word += RF_FREQ_OFFSET;

    uint32_t freqWord = (uint32_t)word;

    writeReg(0x0D, (freqWord >> 16) & 0xFF);
    writeReg(0x0E, (freqWord >> 8) & 0xFF);
    writeReg(0x0F, freqWord & 0xFF);

    Serial.printf(
        "Configured RF frequency: %lu Hz\n",
        (unsigned long)RF_FREQ_HZ
    );

    Serial.printf(
        "Calibration offset: %d\n",
        RF_FREQ_OFFSET
    );
}


// Send a CC1101 command strobe
bool strobe(uint8_t command) {
    SPI.beginTransaction(settings);
    digitalWrite(CS_PIN, LOW);

    bool ready = waitReady();

    if (ready) {
        SPI.transfer(command);
    }

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();

    return ready;
}

// Configure two OOK power levels
bool configurePower() {
    SPI.beginTransaction(settings);
    digitalWrite(CS_PIN, LOW);

    if (!waitReady()) {
        digitalWrite(CS_PIN, HIGH);
        SPI.endTransaction();
        return false;
    }

    SPI.transfer(0x7E); // PATABLE burst write
    SPI.transfer(0x00); // Entry 0: RF off
    SPI.transfer(0x03); // Entry 1: very low RF power

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();

    // Select PATABLE entry 1
    return writeReg(0x22, 0x11);
}

void setup() {
    Serial.begin(115200);

    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);

    pinMode(GDO0_PIN, OUTPUT);
    digitalWrite(GDO0_PIN, LOW);

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);

    delay(100);

    Serial.println();
    Serial.println("CC1101 RF pulse generator");

    // Reset radio
    if (!strobe(0x30)) {
        Serial.println("ERROR: CC1101 not responding");
        return;
    }

    delay(10);

    // Verify radio
    uint8_t partnum = readReg(0x30);
    uint8_t version = readReg(0x31);

    Serial.printf("PARTNUM: 0x%02X\n", partnum);
    Serial.printf("VERSION: 0x%02X\n", version);

    if (version == 0xFF) {
        Serial.println("ERROR: SPI communication failed");
        return;
    }

    // Frequency: 433.92 MHz (26 MHz crystal)
    // writeReg(0x0D, 0x10); // FREQ2
    // writeReg(0x0E, 0xB0); // FREQ1
    // writeReg(0x0F, 0x71); // FREQ0
    // writeReg(0x0F, 0xB8); // FREQ0 - correct    

    configureFrequency();

    // Modulation: OOK
    writeReg(0x12, 0x30); // MDMCFG2

    // Asynchronous serial mode
    writeReg(0x08, 0x30); // PKTCTRL0

    // Data rate configuration
    writeReg(0x10, 0xF5); // MDMCFG4
    writeReg(0x11, 0x83); // MDMCFG3

    // Configure PA table
    if (!configurePower()) {
        Serial.println("ERROR: PATABLE configuration failed");
        return;
    }

    // Calibrate frequency synthesizer
    strobe(0x33); // SCAL
    delay(5);

    // Start transmitter
    if (!strobe(0x35)) { // STX
        Serial.println("ERROR: Could not start transmitter");
        return;
    }

    Serial.println("Transmitter started");
    Serial.println("433.92 MHz - OOK");
}
void loop() {
    digitalWrite(GDO0_PIN, HIGH);
    delay(100);
    // delayMicroseconds(1);

    digitalWrite(GDO0_PIN, LOW);
    delay(100);
    // delayMicroseconds(1);
}
// void loop() {
//     // RF ON
//     digitalWrite(GDO0_PIN, HIGH);
//     delay(200);

//     // RF OFF
//     digitalWrite(GDO0_PIN, LOW);
//     delay(200);
// }
