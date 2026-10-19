#include "web.h"

#include <WebServer.h>

#include <cstdarg>
#include <cstdio>

#include "config.h"
#include "hub.h"
#include "radio.h"
#include "storage.h"
#include "web_page.h"

namespace {

WebServer server(80);
char json[2048];  // responses are built here; /status is the largest at well under 2 KB
size_t json_length = 0;

void json_reset() {
    json_length = 0;
    json[0] = '\0';
}

void json_add(const char* format, ...) {
    if (json_length >= sizeof(json)) {
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

void send_error(const char* message) {
    json_reset();
    json_add("{\"ok\":false,\"error\":\"%s\"}", message);
    server.send(400, "application/json", json);
}

// "0,3,16,..." -> exactly 64 values 0-16. Rejects anything else.
bool parse_pixels(const String& text, uint8_t pixels[construct::NUM_FEATURES]) {
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

void handle_page() {
    server.send(200, "text/html", WEB_PAGE);
}

void handle_teach() {
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
    construct::TeachResult result = hub.node->teach(pixels, label_text[0] - '0');
    if (result != construct::TeachResult::Ok) {
        send_error("this board cannot learn more of that digit");
        return;
    }
    // Save before answering: if our own slot's version went backwards after a reboot,
    // we could reuse a version number peers already hold with different data.
    bool saved = storage_save_slot(*hub.node, NODE_ID, hub.codec);
    hub.model_dirty = true;
    hub_blink();

    json_reset();
    json_add("{\"ok\":true,\"version\":%u,\"saved\":%s}", unsigned(hub.node->slots[NODE_ID].version),
             saved ? "true" : "false");
    server.send(200, "application/json", json);
}

void handle_guess() {
    uint8_t pixels[construct::NUM_FEATURES];
    if (!parse_pixels(server.arg("pixels"), pixels)) {
        send_error("pixels must be 64 values 0-16");
        return;
    }
    hub_refresh_model();
    construct::Prediction prediction = hub.model.predict(pixels);
    int centroid_label = construct::nearest_centroid_predict(hub.totals, pixels);

    json_reset();
    json_add("{\"ok\":true,\"label\":%d,\"confidence\":%.4f,\"centroid\":%d}", prediction.label,
             prediction.confidence, centroid_label);
    server.send(200, "application/json", json);
}

void handle_status() {
    uint32_t now = millis();
    json_reset();
    json_add("{\"board\":\"%c\",\"uptime_s\":%u,\"versions\":[", BOARD_LETTER, unsigned(now / 1000));
    for (int owner = 0; owner < construct::MAX_NODES; owner++) {
        json_add("%s%u", owner ? "," : "", unsigned(hub.node->slots[owner].version));
    }

    json_add("],\"peers\":[");
    bool first = true;
    for (int id = 0; id < construct::MAX_NODES; id++) {
        const construct::PeerStatus& peer = hub.sync.peer(id);
        if (id == NODE_ID || !peer.seen) {
            continue;
        }
        json_add("%s{\"board\":\"%c\",\"online\":%s,\"seen_s_ago\":%u}", first ? "" : ",", 'A' + id,
                 hub.sync.peer_online(id, now) ? "true" : "false", unsigned((now - peer.last_seen_ms) / 1000));
        first = false;
    }

    // knowledge[digit][board] = drawings of that digit that board's room taught
    json_add("],\"knowledge\":[");
    for (int label = 0; label < construct::NUM_CLASSES; label++) {
        json_add("%s[", label ? "," : "");
        for (int owner = 0; owner < construct::MAX_NODES; owner++) {
            json_add("%s%u", owner ? "," : "", unsigned(hub.node->slots[owner].count[label]));
        }
        json_add("]");
    }

    const construct::SyncStats& stats = hub.sync.stats();
    json_add("],\"sync\":{\"sent\":%u,\"received\":%u,\"applied\":%u,\"crc_failures\":%u,"
             "\"radio_dropped\":%u,\"send_failures\":%u,\"syncing\":%s}}",
             unsigned(stats.packets_sent), unsigned(stats.packets_received), unsigned(stats.slots_applied),
             unsigned(stats.crc_failures), unsigned(radio_dropped()), unsigned(radio_send_failures()),
             hub.sync.transfer_active() ? "true" : "false");
    server.send(200, "application/json", json);
}

}  // namespace

void web_begin() {
    server.on("/", HTTP_GET, handle_page);
    server.on("/teach", HTTP_POST, handle_teach);
    server.on("/guess", HTTP_POST, handle_guess);
    server.on("/status", HTTP_GET, handle_status);
    server.onNotFound(handle_page);  // any other address shows the page
    server.begin();
}

void web_handle() {
    server.handleClient();
}
