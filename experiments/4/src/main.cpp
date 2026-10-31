#include <Arduino.h>
#include <SPI.h>

#define CS_PIN   5
#define SCK_PIN  18
#define MISO_PIN 19
#define MOSI_PIN 23

SPISettings settings(100000, MSBFIRST, SPI_MODE0);

uint8_t readVersion() {
    SPI.beginTransaction(settings);

    digitalWrite(CS_PIN, LOW);
    delayMicroseconds(10);

    // Wait until the CC1101 is ready
    unsigned long start = micros();

    while (digitalRead(MISO_PIN) == HIGH) {
        if (micros() - start > 100000) {
            digitalWrite(CS_PIN, HIGH);
            SPI.endTransaction();
            return 0xFF;
        }
    }

    SPI.transfer(0xF1); // VERSION register 0x31 + READ bit
    uint8_t result = SPI.transfer(0x00);

    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();

    return result;
}

void setup() {
    Serial.begin(115200);

    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);

    delay(500);

    Serial.println("CC1101 VERSION register test");

    for (int i = 0; i < 5; i++) {
        Serial.printf("Attempt %d: 0x%02X\n",
                      i + 1, readVersion());
        delay(100);
    }
}

void loop() {}