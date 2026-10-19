#pragma once

#include <cstddef>
#include <cstdint>

// ESP-NOW transport. Everything is broadcast; the sync protocol handles addressing and retries.
// Call after the Wi-Fi access point is up, since ESP-NOW uses its interface and channel.
bool radio_begin();

void radio_send(const uint8_t* data, size_t length);

// Copies the next received packet into out. Returns its length, or 0 if none is waiting.
size_t radio_receive(uint8_t* out, size_t capacity);

uint32_t radio_dropped();        // lost because the receive queue was full
uint32_t radio_send_failures();  // esp_now_send refused the packet
