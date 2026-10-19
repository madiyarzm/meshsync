#include "node.h"

#include <cstring>

namespace construct {

void Totals::clear() {
    std::memset(count, 0, sizeof(count));
    std::memset(sum, 0, sizeof(sum));
    std::memset(sumsq, 0, sizeof(sumsq));
}

void Totals::add(const Slot& slot) {
    for (int label = 0; label < NUM_CLASSES; label++) {
        count[label] += slot.count[label];
        for (int j = 0; j < NUM_FEATURES; j++) {
            sum[label][j] += slot.sum[label][j];
            sumsq[label][j] += slot.sumsq[label][j];
        }
    }
}

uint64_t Totals::total_count() const {
    uint64_t total = 0;
    for (int label = 0; label < NUM_CLASSES; label++) {
        total += count[label];
    }
    return total;
}

bool Node::init(int id) {
    if (id < 0 || id >= MAX_NODES) {
        return false;
    }
    self_id = uint8_t(id);
    for (int owner = 0; owner < MAX_NODES; owner++) {
        slots[owner].clear();
    }
    return true;
}

TeachResult Node::teach(const uint8_t pixels[NUM_FEATURES], int label) {
    return slots[self_id].teach(pixels, label);
}

bool Node::receive(int owner_id, const Slot& incoming) {
    if (owner_id < 0 || owner_id >= MAX_NODES) {
        return false;
    }
    return slots[owner_id].merge_from(incoming);
}

void Node::merge_from(const Node& other) {
    for (int owner = 0; owner < MAX_NODES; owner++) {
        receive(owner, other.slots[owner]);
    }
}

void Node::compute_totals(Totals& out) const {
    out.clear();
    for (int owner = 0; owner < MAX_NODES; owner++) {
        out.add(slots[owner]);
    }
}

}  // namespace construct
