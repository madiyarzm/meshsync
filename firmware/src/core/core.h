#pragma once

// The portable core: slots, merge, models, serialization, CRC32.
// Standard C++ only, no Arduino or ESP-IDF includes, so the same code builds for the
// laptop (tests, simulator) and the ESP32. No dynamic allocation: fixed-size arrays only.

#include <cstddef>
#include <cstdint>

namespace construct {

// --------------------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Little-endian byte helpers (both advance the pointer)
// --------------------------------------------------------------------------------------

inline void write_u32(uint8_t*& out, uint32_t value) {
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> 8);
    out[2] = uint8_t(value >> 16);
    out[3] = uint8_t(value >> 24);
    out += 4;
}

inline uint32_t read_u32(const uint8_t*& in) {
    uint32_t value = uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) |
                     (uint32_t(in[3]) << 24);
    in += 4;
    return value;
}

// --------------------------------------------------------------------------------------
// Slot: what one node has learned
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Node and Totals
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Models
// --------------------------------------------------------------------------------------

// Same math, in the same order, as experiments/prototype.py, so the results are
// bit-identical on the same machine (checked by test/test_golden).

// Label of the digit whose mean drawing is closest, or -1 if nothing was taught
// or the drawing is invalid. Ties go to the lower label.
int nearest_centroid_predict(const Totals& totals, const uint8_t pixels[NUM_FEATURES]);

struct Prediction {
    int label;          // -1 if no prediction
    double confidence;  // 0-1, softmax of the log scores
};

// Gaussian Naive Bayes built from integer totals. About 10 KB: keep it static.
struct GaussianNB {
    bool has_class[NUM_CLASSES];  // false for digits never taught
    double log_prior[NUM_CLASSES];
    double log_norm[NUM_CLASSES];  // sum of -0.5*log(2*pi*var) over all pixels
    double mean[NUM_CLASSES][NUM_FEATURES];
    double var[NUM_CLASSES][NUM_FEATURES];

    // var_floor is added to every variance so pixels that never change (corners) don't
    // divide by zero. Returns false (and an empty model) if var_floor is not positive.
    bool fit(const Totals& totals, double var_floor);

    // Log score per digit; -infinity for digits never taught or if the drawing is invalid.
    void log_scores(const uint8_t pixels[NUM_FEATURES], double scores[NUM_CLASSES]) const;

    Prediction predict(const uint8_t pixels[NUM_FEATURES]) const;
};

// --------------------------------------------------------------------------------------
// Serialization
// --------------------------------------------------------------------------------------

// Turns a slot into bytes and back. Sync and flash code only use this interface,
// so a CBOR codec can later replace RawCodec without touching them.
class SlotCodec {
public:
    virtual ~SlotCodec() = default;

    // Largest number of bytes encode() can produce. Use it to size buffers.
    virtual size_t max_encoded_size() const = 0;

    // Returns the number of bytes written, or 0 if capacity is too small.
    virtual size_t encode(const Slot& slot, uint8_t* out, size_t capacity) const = 0;

    // Returns false for the wrong length or contents that fail Slot::is_valid().
    // On false, `out` holds garbage, so decode into a scratch slot, not a live one.
    virtual bool decode(const uint8_t* data, size_t length, Slot& out) const = 0;
};

constexpr size_t RAW_SLOT_BYTES = 4 * SLOT_UINT32_FIELDS;  // 5164

// Every field as a little-endian uint32, in order: version, count, sum, sumsq.
// Same layout as serialize_raw() in the Python prototype.
class RawCodec : public SlotCodec {
public:
    size_t max_encoded_size() const override { return RAW_SLOT_BYTES; }
    size_t encode(const Slot& slot, uint8_t* out, size_t capacity) const override;
    bool decode(const uint8_t* data, size_t length, Slot& out) const override;
};

// --------------------------------------------------------------------------------------
// CRC32
// --------------------------------------------------------------------------------------

// Standard CRC-32 (IEEE 802.3), the same as Python's zlib.crc32.
// Data that arrives in pieces can be checked by chaining:
//   crc32_update(crc32_update(0, a, len_a), b, len_b) == crc32 of a followed by b
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length);

inline uint32_t crc32(const uint8_t* data, size_t length) {
    return crc32_update(0, data, length);
}

}  // namespace construct
