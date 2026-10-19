// construct hub firmware for the ESP32.
// Access point + drawing web page for teaching, ESP-NOW sync, flash persistence.
// Set BOARD_LETTER in config.h before flashing each board.
//
// Arduino IDE: open this file, board "ESP32 Dev Module" (esp32 core 3.x), Upload.
// PlatformIO:  pio run -e esp32dev -t upload

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "config.h"
#include "src/core/core.h"
#include "src/core/sync.h"
#include "web_page.h"

// ======================================================================================
// Hub state
// ======================================================================================

// The 41 KB Node is allocated once in setup() and never freed: static memory can't fit
// it next to Wi-Fi. Everything else is a plain global (too big for the 8 KB loop stack).
construct::Node* node = nullptr;
construct::Totals totals;
construct::GaussianNB model;
construct::SyncEngine sync_engine;
construct::RawCodec codec;

bool model_dirty = true;        // slots changed since the model was last computed
uint32_t unsaved_mask = 0;      // bit i: slot i changed but is not in flash yet
uint32_t unsaved_since_ms = 0;
uint32_t led_off_at_ms = 0;

static void refresh_model() {
    if (!model_dirty) {
        return;
    }
    node->compute_totals(totals);
    model.fit(totals, construct::VAR_FLOOR);
    model_dirty = false;
}

// ======================================================================================
// Radio (ESP-NOW). Everything is broadcast; the sync protocol handles addressing and retries.
// ======================================================================================

const uint8_t BROADCAST_ADDRESS[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const int RADIO_QUEUE_LENGTH = 16;

struct RadioPacket {
    uint8_t length;
    uint8_t data[construct::MAX_PACKET_BYTES];
};

// Fixed memory for the queue between the receive callback and loop().
StaticQueue_t radio_queue_control;
uint8_t radio_queue_storage[RADIO_QUEUE_LENGTH * sizeof(RadioPacket)];
QueueHandle_t radio_queue = nullptr;
volatile uint32_t radio_dropped = 0;  // queue full: loop() fell behind; sync will retry
uint32_t radio_send_failures = 0;

// Runs in the Wi-Fi task, not in loop(): only copy the packet into the queue and return.
static void on_radio_receive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
    (void)info;
    if (data == nullptr || length <= 0 || length > int(construct::MAX_PACKET_BYTES)) {
        radio_dropped = radio_dropped + 1;
        return;
    }
    RadioPacket packet;
    packet.length = uint8_t(length);
    std::memcpy(packet.data, data, size_t(length));
    if (xQueueSend(radio_queue, &packet, 0) != pdTRUE) {
        radio_dropped = radio_dropped + 1;
    }
}

// Call after the access point is up: ESP-NOW uses its interface and channel.
static bool radio_begin() {
    radio_queue = xQueueCreateStatic(RADIO_QUEUE_LENGTH, sizeof(RadioPacket), radio_queue_storage,
                                     &radio_queue_control);
    if (radio_queue == nullptr || esp_now_init() != ESP_OK) {
        return false;
    }
    esp_now_register_recv_cb(on_radio_receive);

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, BROADCAST_ADDRESS, sizeof(BROADCAST_ADDRESS));
    peer.channel = WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_AP;  // we run as an access point, so send on that interface
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

// Matches SyncEngine::SendFunction.
static void radio_send(void* context, const uint8_t* data, size_t length) {
    (void)context;
    if (esp_now_send(BROADCAST_ADDRESS, data, length) != ESP_OK) {
        radio_send_failures++;
    }
}

// ======================================================================================
// Flash storage (LittleFS). One file per slot: /slot_N.bin = [magic][length][crc32][slot]
// ======================================================================================

const uint32_t FILE_MAGIC = 0x544F4C53;  // "SLOT"
const size_t FILE_HEADER_BYTES = 12;
uint8_t file_buffer[FILE_HEADER_BYTES + construct::MAX_ENCODED_SLOT_BYTES];

static void slot_path(char* out, size_t size, int owner, bool temporary) {
    std::snprintf(out, size, "/slot_%d.%s", owner, temporary ? "tmp" : "bin");
}

static bool load_slot_file(const char* path, construct::Slot& out) {
    if (!LittleFS.exists(path)) {
        return false;
    }
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }
    size_t length = file.read(file_buffer, sizeof(file_buffer));
    file.close();
    if (length < FILE_HEADER_BYTES) {
        return false;
    }
    const uint8_t* in = file_buffer;
    uint32_t magic = construct::read_u32(in);
    uint32_t encoded_length = construct::read_u32(in);
    uint32_t crc = construct::read_u32(in);
    if (magic != FILE_MAGIC || encoded_length != length - FILE_HEADER_BYTES) {
        return false;  // truncated, e.g. power was cut while writing
    }
    if (construct::crc32(in, encoded_length) != crc) {
        return false;
    }
    return codec.decode(in, encoded_length, out);
}

// Call once at boot on a freshly initialized node. Returns how many slots were loaded.
static int load_all_slots() {
    int loaded = 0;
    char path[24];
    for (int owner = 0; owner < construct::MAX_NODES; owner++) {
        // Decoding straight into the slot is safe here: at boot it is empty,
        // and a failed decode is cleared again below.
        construct::Slot& slot = node->slots[owner];
        slot_path(path, sizeof(path), owner, false);
        bool ok = load_slot_file(path, slot);
        if (!ok) {
            // A crash between "write temp" and "rename" leaves only the temp file.
            slot_path(path, sizeof(path), owner, true);
            ok = load_slot_file(path, slot);
        }
        if (ok && slot.version > 0) {
            loaded++;
        } else {
            slot.clear();
        }
    }
    return loaded;
}

static bool save_slot(int owner) {
    uint8_t* encoded = file_buffer + FILE_HEADER_BYTES;
    size_t encoded_length = codec.encode(node->slots[owner], encoded, construct::MAX_ENCODED_SLOT_BYTES);
    if (encoded_length == 0) {
        return false;
    }
    uint8_t* out = file_buffer;
    construct::write_u32(out, FILE_MAGIC);
    construct::write_u32(out, uint32_t(encoded_length));
    construct::write_u32(out, construct::crc32(encoded, encoded_length));

    // Write a temp file, then swap it in, so the old copy survives a power cut mid-write.
    char temp_path[24];
    char final_path[24];
    slot_path(temp_path, sizeof(temp_path), owner, true);
    slot_path(final_path, sizeof(final_path), owner, false);
    File file = LittleFS.open(temp_path, "w");
    if (!file) {
        return false;
    }
    size_t total = FILE_HEADER_BYTES + encoded_length;
    size_t written = file.write(file_buffer, total);
    file.close();
    if (written != total) {
        return false;
    }
    LittleFS.remove(final_path);
    return LittleFS.rename(temp_path, final_path);
}

// A peer's slot was applied: mark the model stale and queue the slot for saving.
static void on_slot_changed(void* context, int owner) {
    (void)context;
    model_dirty = true;
    if (unsaved_mask == 0) {
        unsaved_since_ms = millis();
    }
    unsaved_mask |= 1u << owner;
    Serial.printf("received slot %c, version %u\n", 'A' + owner, unsigned(node->slots[owner].version));
}

// Peers' slots are saved in batches; losing one is harmless since it can be fetched again.
static void save_pending_slots(uint32_t now_ms) {
    if (unsaved_mask == 0 || now_ms - unsaved_since_ms < SAVE_DELAY_MS) {
        return;
    }
    for (int owner = 0; owner < construct::MAX_NODES; owner++) {
        if (unsaved_mask & (1u << owner)) {
            save_slot(owner);
        }
    }
    unsaved_mask = 0;
}

// ======================================================================================
// Web server (http://192.168.4.1)
//   GET  /        drawing page
//   POST /teach   label=N&pixels=64 comma-separated values 0-16
//   POST /guess   pixels=...
//   GET  /status  versions, peers, per-digit knowledge by room, sync counters
// ======================================================================================

WebServer server(80);
char json[2048];  // responses are built here; /status is the largest at well under 2 KB
size_t json_length = 0;

static void json_reset() {
    json_length = 0;
    json[0] = '\0';
}

static void json_add(const char* format, ...) {
    if (json_length >= sizeof(json) - 1) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = std::vsnprintf(json + json_length, sizeof(json) - json_length, format, args);
    va_end(args);
    if (written > 0) {
        json_length += size_t(written);
        if (json_length >= sizeof(json)) {
            json_length = sizeof(json) - 1;  // truncated; never happens at current sizes
        }
    }
}

static void send_error(const char* message) {
    json_reset();
    json_add("{\"ok\":false,\"error\":\"%s\"}", message);
    server.send(400, "application/json", json);
}

// "0,3,16,..." -> exactly 64 values 0-16. Rejects anything else.
static bool parse_pixels(const String& text, uint8_t* pixels) {
    int count = 0;
    int value = -1;  // -1: no digit seen yet for the current number
    for (size_t i = 0; i <= text.length(); i++) {
        char c = i < text.length() ? text[i] : ',';
        if (c >= '0' && c <= '9') {
            value = (value < 0 ? 0 : value * 10) + (c - '0');
            if (value > construct::MAX_PIXEL) {
                return false;
            }
        } else if (c == ',') {
            if (value < 0 || count >= construct::NUM_FEATURES) {
                return false;
            }
            pixels[count++] = uint8_t(value);
            value = -1;
        } else {
            return false;
        }
    }
    return count == construct::NUM_FEATURES;
}

static void handle_page() {
    server.send(200, "text/html", WEB_PAGE);
}

static void handle_teach() {
    String label_text = server.arg("label");
    if (label_text.length() != 1 || label_text[0] < '0' || label_text[0] > '9') {
        send_error("label must be one digit 0-9");
        return;
    }
    uint8_t pixels[construct::NUM_FEATURES];
    if (!parse_pixels(server.arg("pixels"), pixels)) {
        send_error("pixels must be 64 values 0-16");
        return;
    }
    if (node->teach(pixels, label_text[0] - '0') != construct::TeachResult::Ok) {
        send_error("this board cannot learn more of that digit");
        return;
    }
    // Save before answering: if our own slot's version went backwards after a reboot,
    // we could reuse a version number peers already hold with different data.
    bool saved = save_slot(NODE_ID);
    model_dirty = true;
    led_off_at_ms = millis() + 150;  // short flash

    json_reset();
    json_add("{\"ok\":true,\"version\":%u,\"saved\":%s}", unsigned(node->slots[NODE_ID].version),
             saved ? "true" : "false");
    server.send(200, "application/json", json);
}

static void handle_guess() {
    uint8_t pixels[construct::NUM_FEATURES];
    if (!parse_pixels(server.arg("pixels"), pixels)) {
        send_error("pixels must be 64 values 0-16");
        return;
    }
    refresh_model();
    construct::Prediction prediction = model.predict(pixels);
    int centroid_label = construct::nearest_centroid_predict(totals, pixels);

    json_reset();
    json_add("{\"ok\":true,\"label\":%d,\"confidence\":%.4f,\"centroid\":%d}", prediction.label,
             prediction.confidence, centroid_label);
    server.send(200, "application/json", json);
}

static void handle_status() {
    uint32_t now = millis();
    json_reset();
    json_add("{\"board\":\"%c\",\"uptime_s\":%u,\"versions\":[", BOARD_LETTER, unsigned(now / 1000));
    for (int owner = 0; owner < construct::MAX_NODES; owner++) {
        json_add("%s%u", owner ? "," : "", unsigned(node->slots[owner].version));
    }

    json_add("],\"peers\":[");
    bool first = true;
    for (int id = 0; id < construct::MAX_NODES; id++) {
        const construct::PeerStatus& peer = sync_engine.peer(id);
        if (id == NODE_ID || !peer.seen) {
            continue;
        }
        json_add("%s{\"board\":\"%c\",\"online\":%s,\"seen_s_ago\":%u}", first ? "" : ",", 'A' + id,
                 sync_engine.peer_online(id, now) ? "true" : "false", unsigned((now - peer.last_seen_ms) / 1000));
        first = false;
    }

    // knowledge[digit][board] = drawings of that digit that board's room taught
    json_add("],\"knowledge\":[");
    for (int label = 0; label < construct::NUM_CLASSES; label++) {
        json_add("%s[", label ? "," : "");
        for (int owner = 0; owner < construct::MAX_NODES; owner++) {
            json_add("%s%u", owner ? "," : "", unsigned(node->slots[owner].count[label]));
        }
        json_add("]");
    }

    const construct::SyncStats& stats = sync_engine.stats();
    json_add("],\"sync\":{\"sent\":%u,\"received\":%u,\"applied\":%u,\"crc_failures\":%u,"
             "\"radio_dropped\":%u,\"send_failures\":%u,\"syncing\":%s}}",
             unsigned(stats.packets_sent), unsigned(stats.packets_received), unsigned(stats.slots_applied),
             unsigned(stats.crc_failures), unsigned(radio_dropped), unsigned(radio_send_failures),
             sync_engine.transfer_active() ? "true" : "false");
    server.send(200, "application/json", json);
}

// ======================================================================================
// setup / loop
// ======================================================================================

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    node = new (std::nothrow) construct::Node();
    if (node == nullptr) {
        Serial.println("not enough memory for the hub state");
        while (true) {
            delay(1000);
        }
    }
    node->init(NODE_ID);
    if (LittleFS.begin(true)) {  // true: format on first use
        int loaded = load_all_slots();
        Serial.printf("loaded %d slots from flash; own version %u\n", loaded,
                      unsigned(node->slots[NODE_ID].version));
    } else {
        Serial.println("flash storage unavailable: this hub will forget everything on reboot");
    }

    char ssid[32];
    std::snprintf(ssid, sizeof(ssid), "construct-%c", BOARD_LETTER);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, AP_PASSWORD, WIFI_CHANNEL, 0, AP_MAX_CLIENTS);
    if (!radio_begin()) {
        Serial.println("ESP-NOW failed to start: this hub will not sync");
    }
    sync_engine.begin(node, &codec, radio_send, on_slot_changed, nullptr, millis());

    server.on("/", HTTP_GET, handle_page);
    server.on("/teach", HTTP_POST, handle_teach);
    server.on("/guess", HTTP_POST, handle_guess);
    server.on("/status", HTTP_GET, handle_status);
    server.onNotFound(handle_page);  // any other address shows the page
    server.begin();

    Serial.printf("room %c ready: join Wi-Fi \"%s\" and open http://%s\n", BOARD_LETTER, ssid,
                  WiFi.softAPIP().toString().c_str());
}

void loop() {
    static RadioPacket packet;
    uint32_t now = millis();

    // Bounded, so a flood of packets can't starve the web server.
    for (int handled = 0; handled < 16; handled++) {
        if (radio_queue == nullptr || xQueueReceive(radio_queue, &packet, 0) != pdTRUE) {
            break;
        }
        sync_engine.on_packet(packet.data, packet.length, now);
    }
    sync_engine.tick(now);
    server.handleClient();
    save_pending_slots(now);

    // LED: on while receiving a slot, plus a short flash after each teach.
    bool flash = int32_t(led_off_at_ms - now) > 0;
    digitalWrite(LED_PIN, (sync_engine.transfer_active() || flash) ? HIGH : LOW);
    delay(1);  // let the Wi-Fi tasks run
}
