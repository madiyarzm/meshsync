#include "slot.h"

#include <cstring>

namespace construct {

bool pixels_valid(const uint8_t pixels[NUM_FEATURES]) {
    if (pixels == nullptr) {
        return false;
    }
    for (int j = 0; j < NUM_FEATURES; j++) {
        if (pixels[j] > MAX_PIXEL) {
            return false;
        }
    }
    return true;
}

void Slot::clear() {
    version = 0;
    std::memset(count, 0, sizeof(count));
    std::memset(sum, 0, sizeof(sum));
    std::memset(sumsq, 0, sizeof(sumsq));
}

TeachResult Slot::teach(const uint8_t pixels[NUM_FEATURES], int label) {
    if (label < 0 || label >= NUM_CLASSES) {
        return TeachResult::BadLabel;
    }
    if (!pixels_valid(pixels)) {
        return TeachResult::BadPixel;
    }
    if (count[label] >= MAX_SAMPLES_PER_CLASS || version == UINT32_MAX) {
        return TeachResult::SlotFull;
    }

    count[label]++;
    for (int j = 0; j < NUM_FEATURES; j++) {
        uint32_t value = pixels[j];
        sum[label][j] += value;
        sumsq[label][j] += value * value;
    }
    version++;
    return TeachResult::Ok;
}

bool Slot::merge_from(const Slot& incoming) {
    if (incoming.version <= version) {
        return false;  // stale or duplicate: ignoring it is what makes merge idempotent
    }
    *this = incoming;
    return true;
}

bool Slot::is_valid() const {
    uint64_t total_samples = 0;
    for (int label = 0; label < NUM_CLASSES; label++) {
        uint64_t n = count[label];
        if (n > MAX_SAMPLES_PER_CLASS) {
            return false;
        }
        total_samples += n;
        for (int j = 0; j < NUM_FEATURES; j++) {
            uint64_t s = sum[label][j];
            uint64_t sq = sumsq[label][j];
            // Every value v is 0-16, so v <= v*v <= 16*v. Summed over samples:
            if (s > uint64_t(MAX_PIXEL) * n || sq < s || sq > uint64_t(MAX_PIXEL) * s) {
                return false;
            }
        }
    }
    // Every teach bumps the version once, so fewer versions than samples is impossible.
    return version >= total_samples;
}

bool Slot::operator==(const Slot& other) const {
    return std::memcmp(this, &other, sizeof(Slot)) == 0;
}

}  // namespace construct
