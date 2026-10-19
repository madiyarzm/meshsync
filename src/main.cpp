// ESP32 hub firmware. Placeholder until Phase 5: proves the core builds for the
// ESP32 and gives a visible sign (blinking LED) that the board is running.

#include <Arduino.h>

#include "core_constants.h"

constexpr int LED_PIN = 2;  // onboard LED

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    Serial.printf("construct hub: %d classes, %d features per drawing\n",
                  construct::NUM_CLASSES, construct::NUM_FEATURES);
}

void loop() {
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
}
