#include <Arduino.h>
#include <SPI.h>

#define CS_PIN 5

SPISettings settings(1000000, MSBFIRST, SPI_MODE0);

// Alternating and grouped bits make the packet easy to recognize in SDR++'s
// time graph. CC1101 transmits these bytes MSB first.
constexpr uint8_t testSequence[] = {
    0xAA, 0xCC, 0xF0, 0x00, 0xFF, 0x81, 0x7E, 0xAA,
};

// Write CC1101 register
void writeReg(uint8_t addr, uint8_t value) {
    SPI.beginTransaction(settings);

    digitalWrite(CS_PIN, LOW);
    while (digitalRead(19) == HIGH) {}

    SPI.transfer(addr);
    SPI.transfer(value);

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
}

// Send CC1101 command
void strobe(uint8_t command) {
    SPI.beginTransaction(settings);

    digitalWrite(CS_PIN, LOW);
    while (digitalRead(19) == HIGH) {}

    SPI.transfer(command);

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
}

void writeTxFifo(const uint8_t *data, size_t length) {
    SPI.beginTransaction(settings);

    digitalWrite(CS_PIN, LOW);
    while (digitalRead(19) == HIGH) {}

    SPI.transfer(0x7F);  // CC1101 TX FIFO, burst write
    for (size_t n = 0; n < length; ++n) {
        SPI.transfer(data[n]);
    }

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
}

void setup() {
    Serial.begin(115200);

    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);

    SPI.begin(18, 19, 23, CS_PIN);

    delay(100);

    // Reset CC1101
    strobe(0x30);
    delay(10);

    // Configure frequency: 433.92 MHz
    writeReg(0x0D, 0x10);
    writeReg(0x0E, 0xB0);
    writeReg(0x0F, 0x71);

    // Configure OOK modulation
    writeReg(0x12, 0x30);

    // Fixed-length packet with CRC disabled; the packet contains testSequence.
    writeReg(0x08, 0x00);  // PKTCTRL0: fixed packet length
    writeReg(0x06, sizeof(testSequence));  // PKTLEN

    // Set transmission power
    writeReg(0x0A, 0xC0);

    // Enter IDLE
    strobe(0x36);

    Serial.println("CC1101 OOK byte sequence transmitter ready");
}

void loop() {
    // Refill the FIFO for each packet. The short gap makes packet boundaries
    // visible in the SDR++ time graph.
    strobe(0x36);  // SIDLE
    strobe(0x3B);  // SFTX: flush TX FIFO
    writeTxFifo(testSequence, sizeof(testSequence));
    strobe(0x34);  // STX: transmit the fixed-length packet

    delay(250);
}
