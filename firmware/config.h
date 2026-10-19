#pragma once

// Change this for each board: 'A', 'B', 'C', ... up to 'H'.
constexpr char BOARD_LETTER = 'A';

#include <cstdint>

#include "src/core/core.h"

constexpr int NODE_ID = BOARD_LETTER - 'A';  // the slot this board owns
static_assert(NODE_ID >= 0 && NODE_ID < construct::MAX_NODES, "BOARD_LETTER must be A-H");

constexpr uint8_t WIFI_CHANNEL = 1;           // ESP-NOW and the access point must share one channel
constexpr const char* WIFI_NAME_PREFIX = "Madiyar-Hub";  // board A shows up as "Madiyar-Hub-A"
constexpr const char* AP_PASSWORD = nullptr;  // open network for visitors; or 8+ characters to lock it
constexpr int AP_MAX_CLIENTS = 8;
constexpr int LED_PIN = 2;

// Slots received from peers are saved in batches. Our own slot is saved on every teach.
constexpr uint32_t SAVE_DELAY_MS = 2000;
