#pragma once

#include <cstdint>

#include "core_constants.h"
#include "slot.h"

namespace construct {

// Sum of all slots. The model is computed from this and never stored.
// 64-bit because adding several uint32 slots can pass 32 bits. About 10 KB: keep it static.
struct Totals {
    uint64_t count[NUM_CLASSES];
    uint64_t sum[NUM_CLASSES][NUM_FEATURES];
    uint64_t sumsq[NUM_CLASSES][NUM_FEATURES];

    void clear();
    void add(const Slot& slot);
    uint64_t total_count() const;
};

// One hub. Holds a copy of every node's slot; only slots[self_id] is its own.
// About 41 KB: keep it in global or static memory.
struct Node {
    uint8_t self_id;
    Slot slots[MAX_NODES];

    // Clears every slot. Returns false if id is not 0 to MAX_NODES-1.
    bool init(int id);

    TeachResult teach(const uint8_t pixels[NUM_FEATURES], int label);

    // Merge one incoming copy of owner_id's slot. Returns true if our copy changed.
    bool receive(int owner_id, const Slot& incoming);

    void merge_from(const Node& other);

    void compute_totals(Totals& out) const;
};

}  // namespace construct
