// ESP32 hub firmware. Placeholder until Phase 5: builds the core for the ESP32,
// runs a tiny teach/merge/predict check at boot, and blinks the LED.

#include <Arduino.h>

#include "core.h"

constexpr int LED_PIN = 2;  // onboard LED

// Global, not on the stack: the Arduino loop task only has an 8 KB stack.
static construct::Node node;
static construct::Totals totals;
static construct::GaussianNB model;

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    node.init(0);
    uint8_t dark[construct::NUM_FEATURES] = {};
    uint8_t bright[construct::NUM_FEATURES];
    for (int j = 0; j < construct::NUM_FEATURES; j++) {
        bright[j] = construct::MAX_PIXEL;
    }
    node.teach(dark, 0);
    node.teach(bright, 1);

    uint32_t start = micros();
    node.compute_totals(totals);
    model.fit(totals, construct::VAR_FLOOR);
    construct::Prediction prediction = model.predict(bright);
    uint32_t elapsed = micros() - start;

    Serial.printf("construct hub: Node %u bytes, Totals %u bytes, model %u bytes\n",
                  unsigned(sizeof(node)), unsigned(sizeof(totals)), unsigned(sizeof(model)));
    Serial.printf("self-check: predicted %d (expected 1), confidence %.3f, took %u us\n",
                  prediction.label, prediction.confidence, unsigned(elapsed));
}

void loop() {
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
}
