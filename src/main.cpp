// ESP32 hub firmware: access point + web page for teaching, ESP-NOW sync, flash persistence.
// Set the board letter in config.h before flashing each board.

#include <Arduino.h>
#include <WiFi.h>

#include <new>

#include "config.h"
#include "hub.h"
#include "radio.h"
#include "storage.h"
#include "web.h"

HubState hub;

void hub_refresh_model() {
    if (!hub.model_dirty) {
        return;
    }
    hub.node->compute_totals(hub.totals);
    hub.model.fit(hub.totals, construct::VAR_FLOOR);
    hub.model_dirty = false;
}

void hub_blink() {
    hub.led_off_at_ms = millis() + 150;
}

static void send_packet(void* context, const uint8_t* data, size_t length) {
    (void)context;
    radio_send(data, length);
}

// A peer's slot was applied: mark the model stale and queue the slot for saving.
static void on_slot_changed(void* context, int owner) {
    (void)context;
    hub.model_dirty = true;
    if (hub.unsaved_mask == 0) {
        hub.unsaved_since_ms = millis();
    }
    hub.unsaved_mask |= 1u << owner;
    Serial.printf("received slot %c, version %u\n", 'A' + owner, unsigned(hub.node->slots[owner].version));
}

static void save_pending_slots(uint32_t now_ms) {
    if (hub.unsaved_mask == 0 || now_ms - hub.unsaved_since_ms < SAVE_DELAY_MS) {
        return;
    }
    for (int owner = 0; owner < construct::MAX_NODES; owner++) {
        if (hub.unsaved_mask & (1u << owner)) {
            storage_save_slot(*hub.node, owner, hub.codec);
        }
    }
    hub.unsaved_mask = 0;
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    hub.node = new (std::nothrow) construct::Node();
    if (hub.node == nullptr) {
        Serial.println("not enough memory for the hub state");
        while (true) {
            delay(1000);
        }
    }
    hub.node->init(NODE_ID);
    if (storage_begin()) {
        int loaded = storage_load_all(*hub.node, hub.codec);
        Serial.printf("loaded %d slots from flash; own version %u\n", loaded,
                      unsigned(hub.node->slots[NODE_ID].version));
    } else {
        Serial.println("flash storage unavailable: this hub will forget everything on reboot");
    }
    hub.model_dirty = true;

    char ssid[32];
    std::snprintf(ssid, sizeof(ssid), "construct-%c", BOARD_LETTER);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, AP_PASSWORD, WIFI_CHANNEL, 0, AP_MAX_CLIENTS);
    if (!radio_begin()) {
        Serial.println("ESP-NOW failed to start: this hub will not sync");
    }

    hub.sync.begin(hub.node, &hub.codec, send_packet, on_slot_changed, nullptr, millis());
    web_begin();
    Serial.printf("room %c ready: join Wi-Fi \"%s\" and open http://%s\n", BOARD_LETTER, ssid,
                  WiFi.softAPIP().toString().c_str());
}

void loop() {
    static uint8_t packet[construct::MAX_PACKET_BYTES];
    uint32_t now = millis();

    // Bounded, so a flood of packets can't starve the web server.
    for (int handled = 0; handled < 16; handled++) {
        size_t length = radio_receive(packet, sizeof(packet));
        if (length == 0) {
            break;
        }
        hub.sync.on_packet(packet, length, now);
    }
    hub.sync.tick(now);
    web_handle();
    save_pending_slots(now);

    // LED: on while receiving a slot, plus a short flash after each teach.
    bool flash = int32_t(hub.led_off_at_ms - now) > 0;
    digitalWrite(LED_PIN, (hub.sync.transfer_active() || flash) ? HIGH : LOW);
    delay(1);  // let the Wi-Fi tasks run
}
