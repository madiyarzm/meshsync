#include "core.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace construct {

// --------------------------------------------------------------------------------------
// Slot
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Node and Totals
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Models
// --------------------------------------------------------------------------------------

namespace {
constexpr double PI = 3.141592653589793;  // same double as Python's math.pi
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();
}  // namespace

int nearest_centroid_predict(const Totals& totals, const uint8_t pixels[NUM_FEATURES]) {
    if (!pixels_valid(pixels)) {
        return -1;
    }
    int best_label = -1;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int label = 0; label < NUM_CLASSES; label++) {
        uint64_t n = totals.count[label];
        if (n == 0) {
            continue;  // never taught this digit: it has no centroid
        }
        // n*(x - mean) = x*n - sum is an exact integer, so the only rounding is the final division.
        uint64_t squared_numerator = 0;
        for (int j = 0; j < NUM_FEATURES; j++) {
            int64_t diff = int64_t(pixels[j]) * int64_t(n) - int64_t(totals.sum[label][j]);
            squared_numerator += uint64_t(diff * diff);
        }
        double distance = double(squared_numerator) / double(n * n);
        if (distance < best_distance) {
            best_distance = distance;
            best_label = label;
        }
    }
    return best_label;
}

bool GaussianNB::fit(const Totals& totals, double var_floor) {
    for (int label = 0; label < NUM_CLASSES; label++) {
        has_class[label] = false;
    }
    if (!(var_floor > 0.0)) {
        return false;  // also rejects NaN
    }

    uint64_t total = totals.total_count();
    for (int label = 0; label < NUM_CLASSES; label++) {
        int64_t n = int64_t(totals.count[label]);
        if (n == 0) {
            continue;
        }
        double norm = 0.0;
        for (int j = 0; j < NUM_FEATURES; j++) {
            int64_t s = int64_t(totals.sum[label][j]);
            int64_t sq = int64_t(totals.sumsq[label][j]);
            mean[label][j] = double(s) / double(n);
            // n*sumsq - sum^2 is n^2 times the variance, as an exact integer.
            int64_t spread = n * sq - s * s;
            if (spread < 0) {
                spread = 0;  // impossible for real data; guards against a corrupted slot
            }
            var[label][j] = double(spread) / double(n * n) + var_floor;
            norm += -0.5 * std::log(2.0 * PI * var[label][j]);
        }
        log_norm[label] = norm;
        log_prior[label] = std::log(double(n) / double(total));
        has_class[label] = true;
    }
    return true;
}

void GaussianNB::log_scores(const uint8_t pixels[NUM_FEATURES], double scores[NUM_CLASSES]) const {
    bool valid = pixels_valid(pixels);
    for (int label = 0; label < NUM_CLASSES; label++) {
        if (!valid || !has_class[label]) {
            scores[label] = NEGATIVE_INFINITY;
            continue;
        }
        double score = log_prior[label] + log_norm[label];
        for (int j = 0; j < NUM_FEATURES; j++) {
            double diff = pixels[j] - mean[label][j];
            score -= diff * diff / (2.0 * var[label][j]);
        }
        scores[label] = score;
    }
}

Prediction GaussianNB::predict(const uint8_t pixels[NUM_FEATURES]) const {
    Prediction result = {-1, 0.0};
    if (!pixels_valid(pixels)) {
        return result;
    }
    double scores[NUM_CLASSES];
    log_scores(pixels, scores);

    int best_label = -1;
    for (int label = 0; label < NUM_CLASSES; label++) {
        if (!has_class[label]) {
            continue;
        }
        if (best_label == -1 || scores[label] > scores[best_label]) {
            best_label = label;
        }
    }
    if (best_label == -1) {
        return result;  // nothing taught yet
    }

    // Softmax relative to the best score so exp() never overflows.
    double denominator = 0.0;
    for (int label = 0; label < NUM_CLASSES; label++) {
        if (has_class[label]) {
            denominator += std::exp(scores[label] - scores[best_label]);
        }
    }
    result.label = best_label;
    result.confidence = 1.0 / denominator;
    return result;
}

// --------------------------------------------------------------------------------------
// Serialization
// --------------------------------------------------------------------------------------

size_t RawCodec::encode(const Slot& slot, uint8_t* out, size_t capacity) const {
    if (out == nullptr || capacity < RAW_SLOT_BYTES) {
        return 0;
    }
    write_u32(out, slot.version);
    for (int label = 0; label < NUM_CLASSES; label++) {
        write_u32(out, slot.count[label]);
    }
    for (int label = 0; label < NUM_CLASSES; label++) {
        for (int j = 0; j < NUM_FEATURES; j++) {
            write_u32(out, slot.sum[label][j]);
        }
    }
    for (int label = 0; label < NUM_CLASSES; label++) {
        for (int j = 0; j < NUM_FEATURES; j++) {
            write_u32(out, slot.sumsq[label][j]);
        }
    }
    return RAW_SLOT_BYTES;
}

bool RawCodec::decode(const uint8_t* data, size_t length, Slot& out) const {
    if (data == nullptr || length != RAW_SLOT_BYTES) {
        return false;  // truncated or padded data
    }
    out.version = read_u32(data);
    for (int label = 0; label < NUM_CLASSES; label++) {
        out.count[label] = read_u32(data);
    }
    for (int label = 0; label < NUM_CLASSES; label++) {
        for (int j = 0; j < NUM_FEATURES; j++) {
            out.sum[label][j] = read_u32(data);
        }
    }
    for (int label = 0; label < NUM_CLASSES; label++) {
        for (int j = 0; j < NUM_FEATURES; j++) {
            out.sumsq[label][j] = read_u32(data);
        }
    }
    return out.is_valid();
}

// --------------------------------------------------------------------------------------
// CRC32
// --------------------------------------------------------------------------------------

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length) {
    // Bit by bit instead of a 1 KB lookup table: slower, but simpler and needs no table in memory.
    crc = ~crc;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;  // reversed form of the CRC-32 polynomial
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

}  // namespace construct
