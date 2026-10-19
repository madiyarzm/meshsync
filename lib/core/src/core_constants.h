#pragma once

// Portable core: standard C++ only. No Arduino or ESP-IDF includes anywhere in lib/core,
// so the same code builds for the laptop (native) and the ESP32.

#include <cstdint>

namespace construct {

constexpr int NUM_CLASSES = 10;   // digits 0-9
constexpr int NUM_FEATURES = 64;  // 8x8 grid, same as the scikit-learn digits dataset
constexpr int MAX_PIXEL = 16;     // each grid cell holds 0-16

// Each slot is 5164 bytes, so 8 slots take 41 KB of the ESP32's ~320 KB of RAM.
constexpr int MAX_NODES = 8;

// Per slot, per digit. Same limit as the Python prototype.
constexpr uint32_t MAX_SAMPLES_PER_CLASS = 1000000;

// Keeps the mesh-wide count per digit under 2^25, so every 64-bit intermediate in the models fits.
static_assert(uint64_t(MAX_NODES) * MAX_SAMPLES_PER_CLASS < (uint64_t(1) << 25),
              "too many nodes for the model's 64-bit math");

// Added to every Gaussian NB variance. Chosen in Phase 1 on training data only.
constexpr double VAR_FLOOR = 1.0;

// version + count[10] + sum[10][64] + sumsq[10][64]
constexpr int SLOT_UINT32_FIELDS = 1 + NUM_CLASSES + 2 * NUM_CLASSES * NUM_FEATURES;

}  // namespace construct
