#pragma once

// Change this for each board: 'A', 'B', 'C', ... up to 'H'.
constexpr char BOARD_LETTER = 'A';

#include <cstdint>

#include "src/meshstate/meshstate.h"

constexpr int DEVICE_ID = BOARD_LETTER - 'A';  // this board's id in slot keys
static_assert(DEVICE_ID >= 0 && DEVICE_ID < meshstate::MAX_DEVICES, "BOARD_LETTER must be A-H");

// Which app this board runs. Boards only sync with boards running the same app.
constexpr uint8_t APP_DIGITS = 1;  // Wi-Fi drawing page (the digits-v1 demo)
constexpr uint8_t APP_FACE = 2;    // webcam gestures + gaze over USB serial (Phase D)
constexpr uint8_t APP_MODE = APP_DIGITS;

constexpr uint8_t WIFI_CHANNEL = 1;           // ESP-NOW and the access point must share one channel
constexpr const char* WIFI_NAME_PREFIX = "Madiyar-Hub";  // board A shows up as "Madiyar-Hub-A"
constexpr const char* AP_PASSWORD = nullptr;  // open network for visitors; or 8+ characters to lock it
constexpr int AP_MAX_CLIENTS = 8;
constexpr int LED_PIN = 2;

// Open sessions are saved this often. Only saved versions are ever sent to other boards.
constexpr uint32_t SAVE_INTERVAL_MS = 2000;
