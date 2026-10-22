// construct hub firmware for the ESP32.
// Digit demo (APP_DIGITS): access point + drawing web page, ESP-NOW sync, flash persistence.
// Set BOARD_LETTER in config.h before flashing each board.
// Serial (115200): send "BENCH" to measure merge and predict times on this board.
//
// Arduino IDE: open this file, board "ESP32 Dev Module" (esp32 core 3.x), Upload.
// PlatformIO:  pio run -e esp32dev -t upload

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "config.h"
#include "flash_store.h"
#include "src/meshstate/models.h"
#include "src/meshstate/replica.h"
#include "src/meshstate/sync.h"
#include "web_page.h"

using namespace meshstate;

// ======================================================================================
// Hub state
// ======================================================================================

// Replica and SyncEngine (~75 KB together) are allocated once in setup() and never freed:
// static memory can't fit them next to Wi-Fi. Everything else is a plain global.
FlashStore store;
Replica* replica = nullptr;
SyncEngine* sync_engine = nullptr;
DigitModel model;
bool model_dirty = true;
uint32_t led_off_at_ms = 0;

const SlotKey MY_DIGITS = {uint8_t(DEVICE_ID), uint8_t(PayloadType::Digits), 0};

// Scratch for reading slots back (status page, benchmark): too big for the stack.
uint8_t slot_buffer[MAX_ENCODED_SLOT_BYTES];
AnyPayload slot_payload;

static void refresh_model() {
    if (!model_dirty) {
        return;
    }
    model.fit(replica->digits_totals(), DIGIT_VAR_FLOOR);
    model_dirty = false;
}

// ======================================================================================
// Radio (ESP-NOW). Everything is broadcast; the sync protocol handles addressing and retries.
// ======================================================================================

const uint8_t BROADCAST_ADDRESS[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const int RADIO_QUEUE_LENGTH = 16;

struct RadioPacket {
    uint8_t length;
    uint8_t data[MAX_PACKET_BYTES];
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
    if (data == nullptr || length <= 0 || length > int(MAX_PACKET_BYTES)) {
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

// Matches SyncEngine::EventFunction.
static void on_sync_event(void* context, SyncEvent event, uint32_t id) {
    (void)context;
    model_dirty = true;
    SlotKey key = SlotKey::unpack(id);
    if (event == SyncEvent::SlotApplied) {
        Serial.printf("# received slot from %c (type %u, session %u)\n", 'A' + key.device, key.type, key.session);
    } else if (event == SyncEvent::Tombstone) {
        Serial.printf("# slot deleted (from %c, session %u)\n", 'A' + key.device, key.session);
    } else {
        Serial.printf("# user %lu's data deleted\n", (unsigned long)id);
    }
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
            if (value > DIGIT_MAX_PIXEL) {
                return false;
            }
        } else if (c == ',') {
            if (value < 0 || count >= DIGIT_FEATURES) {
                return false;
            }
            pixels[count++] = uint8_t(value);
            value = -1;
        } else {
            return false;
        }
    }
    return count == DIGIT_FEATURES;
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
    uint8_t pixels[DIGIT_FEATURES];
    if (!parse_pixels(server.arg("pixels"), pixels)) {
        send_error("pixels must be 64 values 0-16");
        return;
    }
    Result result = replica->add_digit(MY_DIGITS, label_text[0] - '0', pixels);
    if (result != Result::Ok) {
        send_error(result_name(result));
        return;
    }
    // Save right away so the new drawing can sync (only saved versions are ever sent).
    bool saved = replica->flush() == Result::Ok;
    model_dirty = true;
    led_off_at_ms = millis() + 150;  // short flash

    json_reset();
    json_add("{\"ok\":true,\"version\":%u,\"saved\":%s}", unsigned(replica->session_version(MY_DIGITS)),
             saved ? "true" : "false");
    server.send(200, "application/json", json);
}

static void handle_guess() {
    uint8_t pixels[DIGIT_FEATURES];
    if (!parse_pixels(server.arg("pixels"), pixels)) {
        send_error("pixels must be 64 values 0-16");
        return;
    }
    refresh_model();
    Prediction prediction = model.predict(pixels);
    int centroid_label = digit_nearest_centroid(replica->digits_totals(), pixels);

    json_reset();
    json_add("{\"ok\":true,\"label\":%d,\"confidence\":%.4f,\"centroid\":%d}", prediction.label,
             prediction.confidence, centroid_label);
    server.send(200, "application/json", json);
}

static void handle_status() {
    uint32_t now = millis();
    // Per board: its digit slot's version and per-digit counts (read back from flash).
    uint32_t versions[MAX_DEVICES] = {};
    uint32_t counts[DIGIT_CLASSES][MAX_DEVICES] = {};
    for (int i = 0; i < replica->slot_count(); i++) {
        const SlotHeader& header = replica->slot_header(i);
        if (header.key.type != uint8_t(PayloadType::Digits)) {
            continue;
        }
        size_t length = replica->read_slot(header.key.packed(), slot_buffer, sizeof(slot_buffer));
        SlotHeader loaded;
        if (length == 0 || decode_slot(slot_buffer, length, loaded, slot_payload) != Result::Ok) {
            continue;
        }
        versions[header.key.device] = header.version;
        for (int label = 0; label < DIGIT_CLASSES; label++) {
            counts[label][header.key.device] = slot_payload.digits.count(label);
        }
    }

    json_reset();
    json_add("{\"board\":\"%c\",\"uptime_s\":%u,\"versions\":[", BOARD_LETTER, unsigned(now / 1000));
    for (int device = 0; device < MAX_DEVICES; device++) {
        json_add("%s%u", device ? "," : "", unsigned(versions[device]));
    }
    json_add("],\"peers\":[");
    bool first = true;
    for (int id = 0; id < MAX_DEVICES; id++) {
        const PeerStatus& peer = sync_engine->peer(id);
        if (id == DEVICE_ID || !peer.seen) {
            continue;
        }
        json_add("%s{\"board\":\"%c\",\"online\":%s,\"seen_s_ago\":%u}", first ? "" : ",", 'A' + id,
                 sync_engine->peer_online(id, now) ? "true" : "false", unsigned((now - peer.last_seen_ms) / 1000));
        first = false;
    }
    // knowledge[digit][board] = drawings of that digit that board's room taught
    json_add("],\"knowledge\":[");
    for (int label = 0; label < DIGIT_CLASSES; label++) {
        json_add("%s[", label ? "," : "");
        for (int device = 0; device < MAX_DEVICES; device++) {
            json_add("%s%u", device ? "," : "", unsigned(counts[label][device]));
        }
        json_add("]");
    }
    const SyncStats& stats = sync_engine->stats();
    json_add("],\"sync\":{\"sent\":%u,\"received\":%u,\"applied\":%u,\"crc_failures\":%u,"
             "\"radio_dropped\":%u,\"send_failures\":%u,\"syncing\":%s}}",
             unsigned(stats.packets_sent), unsigned(stats.packets_received), unsigned(stats.slots_applied),
             unsigned(stats.crc_failures), unsigned(radio_dropped), unsigned(radio_send_failures),
             sync_engine->syncing() ? "true" : "false");
    server.send(200, "application/json", json);
}

// ======================================================================================
// Benchmark: send "BENCH" over serial. Uses a separate replica in RAM, so it never touches
// this board's real state.
// ======================================================================================

static uint32_t bench_value = 12345;
static int32_t bench_random(int32_t low, int32_t high) {
    bench_value = bench_value * 1103515245u + 12345u;
    return low + int32_t((bench_value >> 8) % uint32_t(high - low + 1));
}

// Fill slot_payload with `samples` random samples of one payload type.
static void bench_fill(PayloadType type, int samples) {
    static AnyPayload one;
    clear_payload(type, slot_payload);
    int32_t features[FACE_FEATURES];
    uint8_t pixels[DIGIT_FEATURES];
    for (int s = 0; s < samples; s++) {
        for (int f = 0; f < FACE_FEATURES; f++) {
            features[f] = bench_random(-1000, 1000);
        }
        for (int f = 0; f < DIGIT_FEATURES; f++) {
            pixels[f] = uint8_t(bench_random(0, 16));
        }
        if (type == PayloadType::Digits) {
            digit_sample(one.digits, s % DIGIT_CLASSES, pixels);
        } else if (type == PayloadType::Gaze) {
            gaze_sample(one.gaze, features, bench_random(0, 1000), bench_random(0, 1000));
        } else {
            gesture_sample(one.gesture, s % GESTURE_CLASSES, features);
        }
        accumulate_payload(type, slot_payload, one, +1);
    }
}

static void run_benchmark() {
    Serial.printf("#BENCH heap free=%u largest_block=%u\n", unsigned(ESP.getFreeHeap()),
                  unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    Serial.printf("#BENCH sizes replica=%u sync_engine=%u digit_model=%u gesture_model=%u gaze_model=%u\n",
                  unsigned(sizeof(Replica)), unsigned(sizeof(SyncEngine)), unsigned(sizeof(DigitModel)),
                  unsigned(sizeof(GestureModel)), unsigned(sizeof(GazeModel)));
    MemoryStore<3>* bench_store = new (std::nothrow) MemoryStore<3>();
    Replica* bench = new (std::nothrow) Replica();
    if (bench_store == nullptr || bench == nullptr) {
        Serial.println("#BENCH not enough free memory for a second replica");
        delete bench_store;
        delete bench;
        return;
    }
    bench->begin(7, bench_store);
    bench->set_active_user(1);

    const PayloadType types[3] = {PayloadType::GestureBlend, PayloadType::Gaze, PayloadType::Digits};
    const char* names[3] = {"gesture", "gaze", "digits"};
    const int samples[3] = {135, 135, 100};  // ~45 per gesture class / 9 gaze dots x 15
    for (int t = 0; t < 3; t++) {
        bench_fill(types[t], samples[t]);
        bool digits = types[t] == PayloadType::Digits;
        SlotHeader header = {{6, uint8_t(types[t]), uint16_t(digits ? 0 : 1)}, schema_for(types[t]),
                             uint16_t(digits ? 0 : 1), 0, uint32_t(samples[t])};
        // First merge inserts; later ones replace (subtract old, add new) like a real update.
        uint32_t first_us = 0;
        uint32_t total_us = 0;
        const int repeats = 20;
        size_t length = 0;
        for (int r = 0; r <= repeats; r++) {
            header.version = uint32_t(samples[t] + r);
            length = encode_slot(header, slot_payload, slot_buffer, sizeof(slot_buffer));
            uint32_t start = micros();
            Result result = bench->merge_slot(slot_buffer, length);
            uint32_t elapsed = micros() - start;
            if (result != Result::Ok) {
                Serial.printf("#BENCH merge %s failed: %s\n", names[t], result_name(result));
                break;
            }
            if (r == 0) {
                first_us = elapsed;
            } else {
                total_us += elapsed;
            }
        }
        // Flash cost of saving that slot, which a real merge on this board also pays.
        uint32_t start = micros();
        bool saved = store.save(0xFFFFFF00u + uint32_t(t), slot_buffer, length);
        uint32_t flash_us = micros() - start;
        store.erase(0xFFFFFF00u + uint32_t(t));
        Serial.printf("#BENCH merge %s bytes=%u first_us=%u replace_avg_us=%u flash_save_us=%u%s\n", names[t],
                      unsigned(length), unsigned(first_us), unsigned(total_us / repeats), unsigned(flash_us),
                      saved ? "" : " (flash save failed)");
    }

    static GestureModel gesture_model;
    static GazeModel gaze_model;
    static DigitModel digit_model;
    int32_t features[FACE_FEATURES];
    uint8_t pixels[DIGIT_FEATURES];
    for (int f = 0; f < FACE_FEATURES; f++) {
        features[f] = bench_random(-1000, 1000);
    }
    for (int f = 0; f < DIGIT_FEATURES; f++) {
        pixels[f] = uint8_t(bench_random(0, 16));
    }
    const int predictions = 100;
    volatile double sink = 0;  // keeps the compiler from removing the timed calls

    uint32_t start = micros();
    gesture_model.fit(bench->gesture_totals(PayloadType::GestureBlend, false),
                      bench->gesture_totals(PayloadType::GestureBlend, true), 0.5, GESTURE_VAR_FLOOR);
    uint32_t fit_us = micros() - start;
    start = micros();
    for (int i = 0; i < predictions; i++) {
        sink = sink + gesture_model.predict(features).confidence;
    }
    Serial.printf("#BENCH gesture fit_us=%u predict_us=%u\n", unsigned(fit_us), unsigned((micros() - start) / predictions));

    start = micros();
    bool ready = gaze_model.fit(bench->gaze_totals(false), bench->gaze_totals(true), 0.5, GAZE_RIDGE_ALPHA);
    fit_us = micros() - start;
    start = micros();
    double x = 0;
    double y = 0;
    for (int i = 0; i < predictions; i++) {
        gaze_model.predict(features, x, y);
        sink = sink + x;
    }
    Serial.printf("#BENCH gaze fit_us=%u predict_us=%u ready=%d\n", unsigned(fit_us),
                  unsigned((micros() - start) / predictions), ready ? 1 : 0);

    start = micros();
    digit_model.fit(bench->digits_totals(), DIGIT_VAR_FLOOR);
    fit_us = micros() - start;
    start = micros();
    for (int i = 0; i < predictions; i++) {
        sink = sink + digit_model.predict(pixels).confidence;
    }
    Serial.printf("#BENCH digits fit_us=%u predict_us=%u\n", unsigned(fit_us), unsigned((micros() - start) / predictions));

    delete bench;
    delete bench_store;
    Serial.println("#BENCH done");
}

// Serial commands, one per line.
static void handle_serial() {
    static char line[64];
    static size_t length = 0;
    while (Serial.available() > 0) {
        char c = char(Serial.read());
        if (c == '\n' || c == '\r') {
            line[length] = '\0';
            if (std::strcmp(line, "BENCH") == 0) {
                run_benchmark();
            }
            length = 0;
        } else if (length < sizeof(line) - 1) {
            line[length++] = c;
        }
    }
}

// ======================================================================================
// setup / loop
// ======================================================================================

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    replica = new (std::nothrow) Replica();
    sync_engine = new (std::nothrow) SyncEngine();
    if (replica == nullptr || sync_engine == nullptr) {
        Serial.println("# not enough memory for the hub state");
        while (true) {
            delay(1000);
        }
    }
    if (store.begin()) {
        // Files from the digits-v1 firmware use another format; remove them once.
        char legacy[24];
        for (int i = 0; i < MAX_DEVICES; i++) {
            std::snprintf(legacy, sizeof(legacy), "/slot_%d.bin", i);
            if (LittleFS.exists(legacy)) {
                LittleFS.remove(legacy);
            }
        }
    } else {
        Serial.println("# flash storage unavailable: this hub will forget everything on reboot");
    }
    Result result = replica->begin(uint8_t(DEVICE_ID), &store);
    if (result != Result::Ok) {
        Serial.printf("# saved state problem: %s (starting with what could be read)\n", result_name(result));
    }
    replica->resume_session(MY_DIGITS, 0);
    Serial.printf("# loaded %d slots, %d tombstones; own version %u\n", replica->slot_count(),
                  replica->tombstone_count(), unsigned(replica->session_version(MY_DIGITS)));

    char wifi_name[32];
    std::snprintf(wifi_name, sizeof(wifi_name), "%s-%c", WIFI_NAME_PREFIX, BOARD_LETTER);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(wifi_name, AP_PASSWORD, WIFI_CHANNEL, 0, AP_MAX_CLIENTS);
    if (!radio_begin()) {
        Serial.println("# ESP-NOW failed to start: this hub will not sync");
    }
    sync_engine->begin(replica, APP_MODE, radio_send, on_sync_event, nullptr, millis());

    server.on("/", HTTP_GET, handle_page);
    server.on("/teach", HTTP_POST, handle_teach);
    server.on("/guess", HTTP_POST, handle_guess);
    server.on("/status", HTTP_GET, handle_status);
    server.onNotFound(handle_page);  // any other address shows the page
    server.begin();

    Serial.printf("# room %c ready: join Wi-Fi \"%s\" and open http://%s\n", BOARD_LETTER, wifi_name,
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
        sync_engine->on_packet(packet.data, packet.length, now);
    }
    sync_engine->tick(now);
    server.handleClient();
    handle_serial();

    // LED: on while syncing, plus a short flash after each teach.
    bool flash = int32_t(led_off_at_ms - now) > 0;
    digitalWrite(LED_PIN, (sync_engine->syncing() || flash) ? HIGH : LOW);
    delay(1);  // let the Wi-Fi tasks run
}
