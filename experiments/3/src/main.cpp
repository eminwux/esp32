#include <Arduino.h>

#define LED_PIN 2

void setup() {
    pinMode(LED_PIN, OUTPUT);

    Serial.begin(115200);
    Serial.println("ESP32 initialized!");
}

void loop() {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("ON");
    delay(1000);

    digitalWrite(LED_PIN, LOW);
    Serial.println("OFF");
    delay(1000);
}
