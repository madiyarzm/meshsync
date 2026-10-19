#pragma once

// Portable core: standard C++ only. No Arduino or ESP-IDF includes anywhere in lib/core,
// so the same code builds for the laptop (native) and the ESP32.

#include <cstdint>

namespace construct {

constexpr int NUM_CLASSES = 10;   // digits 0-9
constexpr int NUM_FEATURES = 64;  // 8x8 grid, same as the scikit-learn digits dataset
constexpr int MAX_PIXEL = 16;     // each grid cell holds 0-16

}  // namespace construct
