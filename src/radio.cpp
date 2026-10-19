#include "radio.h"

#include <WiFi.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstring>

#include "config.h"
#include "protocol.h"

namespace {

const uint8_t BROADCAST_ADDRESS[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr int QUEUE_LENGTH = 16;

struct RadioPacket {
    uint8_t length;
    uint8_t data[construct::MAX_PACKET_BYTES];
};

// Fixed memory for the queue, so nothing is allocated at runtime.
StaticQueue_t queue_control;
uint8_t queue_storage[QUEUE_LENGTH * sizeof(RadioPacket)];
QueueHandle_t receive_queue = nullptr;

volatile uint32_t dropped_count = 0;
uint32_t send_failure_count = 0;

// Runs in the Wi-Fi task, not in loop(): only copy the packet into the queue and return.
void on_receive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
    (void)info;
    if (data == nullptr || length <= 0 || length > int(construct::MAX_PACKET_BYTES)) {
        dropped_count = dropped_count + 1;
        return;
    }
    RadioPacket packet;
    packet.length = uint8_t(length);
    std::memcpy(packet.data, data, size_t(length));
    if (xQueueSend(receive_queue, &packet, 0) != pdTRUE) {
        dropped_count = dropped_count + 1;  // loop() is behind; the sync protocol will retry
    }
}

}  // namespace

bool radio_begin() {
    receive_queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(RadioPacket), queue_storage, &queue_control);
    if (receive_queue == nullptr || esp_now_init() != ESP_OK) {
        return false;
    }
    esp_now_register_recv_cb(on_receive);

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, BROADCAST_ADDRESS, sizeof(BROADCAST_ADDRESS));
    peer.channel = WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_AP;  // we run as an access point, so send on that interface
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

void radio_send(const uint8_t* data, size_t length) {
    if (esp_now_send(BROADCAST_ADDRESS, data, length) != ESP_OK) {
        send_failure_count++;
    }
}

size_t radio_receive(uint8_t* out, size_t capacity) {
    RadioPacket packet;
    if (receive_queue == nullptr || xQueueReceive(receive_queue, &packet, 0) != pdTRUE) {
        return 0;
    }
    if (packet.length > capacity) {
        return 0;
    }
    std::memcpy(out, packet.data, packet.length);
    return packet.length;
}

uint32_t radio_dropped() {
    return dropped_count;
}

uint32_t radio_send_failures() {
    return send_failure_count;
}
