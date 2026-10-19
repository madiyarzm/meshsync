#pragma once

#include <cstdint>

#include "core_constants.h"

namespace construct {

enum class TeachResult {
    Ok,
    BadLabel,  // not 0-9
    BadPixel,  // missing drawing or a value above MAX_PIXEL
    SlotFull,  // another teach would pass MAX_SAMPLES_PER_CLASS
};

// True if the drawing exists and every value is 0-16.
bool pixels_valid(const uint8_t pixels[NUM_FEATURES]);

// Everything one node has learned. Only the owning node ever calls teach() on it.
// 5164 bytes: on the ESP32 keep slots in global or static memory, not on the stack.
struct Slot {
    uint32_t version;  // 0 means nothing learned yet
    uint32_t count[NUM_CLASSES];
    uint32_t sum[NUM_CLASSES][NUM_FEATURES];
    uint32_t sumsq[NUM_CLASSES][NUM_FEATURES];

    void clear();

    // Validates everything before changing any field, so a rejected teach changes nothing.
    TeachResult teach(const uint8_t pixels[NUM_FEATURES], int label);

    // The whole CRDT: keep whichever copy has the higher version. Returns true if this slot changed.
    bool merge_from(const Slot& incoming);

    // Rejects contents no real sequence of teaches could produce (for data from radio or flash).
    bool is_valid() const;

    bool operator==(const Slot& other) const;
    bool operator!=(const Slot& other) const { return !(*this == other); }
};

static_assert(sizeof(Slot) == 4 * SLOT_UINT32_FIELDS, "Slot must have no padding");

}  // namespace construct
