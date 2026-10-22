#pragma once

// meshstate: mergeable decoder state for small offline meshes.
//
// State is a set of slots. Each slot is owned by one device and holds additive integer
// statistics (class stats for classifiers, X'X / X'Y tables for ridge regression).
// Merge keeps the highest version per slot; tombstones and per-user deletion counters
// always win. Every model is computed from integer sums, so merging in any order gives
// exactly the totals you'd get from all the data in one place.
//
// Standard C++ only: no Arduino or ESP-IDF includes, no dynamic allocation.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace meshstate {

// ======================================================================================
// Limits
// ======================================================================================

constexpr int MAX_DEVICES = 8;         // device ids 0-7 (board letters A-H)
constexpr int MAX_SLOTS = 64;          // live slots per replica; a mesh-wide budget
constexpr int MAX_TOMBSTONES = 256;    // deleted slot keys, kept forever
constexpr int MAX_USERS = 64;          // users with a deletion counter
constexpr int MAX_OPEN_SESSIONS = 2;   // sessions being written at once on this device
constexpr uint32_t MAX_SESSION_SAMPLES = 100000;

// ======================================================================================
// Results
// ======================================================================================

enum class Result : uint8_t {
    Ok,
    Unchanged,       // valid, but nothing new (duplicate or stale)
    BadInput,        // wrong label, feature count, or value out of range
    Overflow,        // an int64 sum would overflow; nothing was changed
    NotOwner,        // only the device in the key may write a slot
    UnknownSession,  // no open session with that key
    TooManyOpen,     // MAX_OPEN_SESSIONS already open
    StoreFull,       // MAX_SLOTS reached
    TombstonesFull,  // MAX_TOMBSTONES reached
    UsersFull,       // MAX_USERS reached
    SchemaMismatch,  // payload layout version differs from ours
    Deleted,         // slot is tombstoned or its user's data was deleted
    Invalid,         // malformed or impossible contents
    StorageError,    // the SlotStore failed to read or write
};

const char* result_name(Result result);

// ======================================================================================
// Payload types and slot keys
// ======================================================================================

enum class PayloadType : uint8_t {
    Digits = 1,           // 10 classes x 64 pixels, uint32 (the digit demo)
    GestureBlend = 2,     // 3 classes x 12 blendshape features, int64
    GestureLandmark = 3,  // 3 classes x 12 landmark features, int64
    Gaze = 4,             // ridge regression, 12 features + bias -> (x, y), int64
};
constexpr int NUM_PAYLOAD_TYPES = 4;

bool payload_type_known(uint8_t type);
uint8_t schema_for(PayloadType type);  // bump when a payload layout changes

// (device, payload type, session): unique across the mesh, never reused.
struct SlotKey {
    uint8_t device;
    uint8_t type;
    uint16_t session;

    uint32_t packed() const { return (uint32_t(device) << 24) | (uint32_t(type) << 16) | session; }
    static SlotKey unpack(uint32_t value) {
        return {uint8_t(value >> 24), uint8_t(value >> 16), uint16_t(value)};
    }
    bool operator==(const SlotKey& other) const { return packed() == other.packed(); }
    bool operator!=(const SlotKey& other) const { return packed() != other.packed(); }
};

// Fixed when the slot is created, except version, which only the owner bumps.
struct SlotHeader {
    SlotKey key;
    uint8_t schema;
    uint16_t user;        // 0 = no user (digit demo)
    uint32_t user_epoch;  // the user's deletion counter when this slot was created
    uint32_t version;
};
constexpr size_t HEADER_BYTES = 15;

// ======================================================================================
// Little-endian byte helpers (advance the pointer)
// ======================================================================================

inline void write_u8(uint8_t*& out, uint8_t value) {
    *out++ = value;
}
inline void write_u16(uint8_t*& out, uint16_t value) {
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> 8);
    out += 2;
}
inline void write_u32(uint8_t*& out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out[i] = uint8_t(value >> (8 * i));
    }
    out += 4;
}
inline void write_u64(uint8_t*& out, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        out[i] = uint8_t(value >> (8 * i));
    }
    out += 8;
}
inline uint8_t read_u8(const uint8_t*& in) {
    return *in++;
}
inline uint16_t read_u16(const uint8_t*& in) {
    uint16_t value = uint16_t(in[0] | (in[1] << 8));
    in += 2;
    return value;
}
inline uint32_t read_u32(const uint8_t*& in) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= uint32_t(in[i]) << (8 * i);
    }
    in += 4;
    return value;
}
inline uint64_t read_u64(const uint8_t*& in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= uint64_t(in[i]) << (8 * i);
    }
    in += 8;
    return value;
}

void write_header(uint8_t*& out, const SlotHeader& header);
SlotHeader read_header(const uint8_t*& in);

// Standard CRC-32 (same as Python's zlib.crc32). Chain calls for data that arrives in pieces.
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length);
inline uint32_t crc32(const uint8_t* data, size_t length) {
    return crc32_update(0, data, length);
}

// ======================================================================================
// Checked integer math: report overflow instead of wrapping around
// ======================================================================================

inline bool add_overflows(int64_t a, int64_t b) {
    return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

// totals[i] += sign * values[i] for all i, or nothing at all if any step would overflow
// (or, for unsigned totals, go below zero).
template <typename Total, typename Value>
Result accumulate(Total* totals, const Value* values, int count, int sign) {
    for (int i = 0; i < count; i++) {
        int64_t delta = sign > 0 ? int64_t(values[i]) : -int64_t(values[i]);
        bool bad;
        if constexpr (std::is_unsigned<Total>::value) {
            const uint64_t max_total = std::numeric_limits<Total>::max();
            bad = delta < 0 ? uint64_t(-delta) > uint64_t(totals[i])
                            : uint64_t(totals[i]) > max_total - uint64_t(delta);
        } else {
            bad = add_overflows(int64_t(totals[i]), delta);
        }
        if (bad) {
            for (int undo = 0; undo < i; undo++) {  // roll back what was already added
                int64_t back = sign > 0 ? -int64_t(values[undo]) : int64_t(values[undo]);
                totals[undo] = Total(int64_t(totals[undo]) + back);
            }
            return Result::Overflow;
        }
        totals[i] = Total(int64_t(totals[i]) + delta);
    }
    return Result::Ok;
}

// ======================================================================================
// Payloads: flat arrays of integers with named accessors
// ======================================================================================

// Per-class sufficient statistics: count[c], sum[c][f], sumsq[c][f], stored in that order.
template <typename T, int C, int F>
struct ClassStats {
    static constexpr int CLASSES = C;
    static constexpr int FEATURES = F;
    static constexpr int VALUES = C + 2 * C * F;
    using Value = T;

    T values[VALUES];

    T& count(int c) { return values[c]; }
    T& sum(int c, int f) { return values[C + c * F + f]; }
    T& sumsq(int c, int f) { return values[C + C * F + c * F + f]; }
    const T& count(int c) const { return values[c]; }
    const T& sum(int c, int f) const { return values[C + c * F + f]; }
    const T& sumsq(int c, int f) const { return values[C + C * F + c * F + f]; }

    void clear() {
        for (int i = 0; i < VALUES; i++) {
            values[i] = 0;
        }
    }
    uint64_t total_count() const {
        uint64_t total = 0;
        for (int c = 0; c < C; c++) {
            total += uint64_t(count(c));
        }
        return total;
    }
};

// Ridge regression sufficient statistics: n, X'X[D][D], X'Y[D][K], Y'Y[K] (feature 0 is the bias).
template <int D, int K>
struct RidgeStats {
    static constexpr int DIM = D;
    static constexpr int TARGETS = K;
    static constexpr int VALUES = 1 + D * D + D * K + K;
    using Value = int64_t;

    int64_t values[VALUES];

    int64_t& n() { return values[0]; }
    int64_t& xtx(int i, int j) { return values[1 + i * D + j]; }
    int64_t& xty(int i, int k) { return values[1 + D * D + i * K + k]; }
    int64_t& yty(int k) { return values[1 + D * D + D * K + k]; }
    const int64_t& n() const { return values[0]; }
    const int64_t& xtx(int i, int j) const { return values[1 + i * D + j]; }
    const int64_t& xty(int i, int k) const { return values[1 + D * D + i * K + k]; }
    const int64_t& yty(int k) const { return values[1 + D * D + D * K + k]; }

    void clear() {
        for (int i = 0; i < VALUES; i++) {
            values[i] = 0;
        }
    }
};

// ======================================================================================
// This project's payload shapes
// ======================================================================================

constexpr int DIGIT_CLASSES = 10;
constexpr int DIGIT_FEATURES = 64;  // 8x8 grid, values 0-16
constexpr int DIGIT_MAX_PIXEL = 16;
constexpr uint32_t DIGIT_MAX_PER_CLASS = 1000000;
// Digit slots always use session 0, so at most MAX_DEVICES of them exist. That keeps the
// mesh-wide count per digit under 2^25, so the exact 64-bit math in the digit model fits.
static_assert(uint64_t(MAX_DEVICES) * DIGIT_MAX_PER_CLASS < (uint64_t(1) << 25), "digit totals too large");

constexpr int GESTURE_CLASSES = 3;     // 0 neutral, 1 eyebrow raise, 2 smile
constexpr int FACE_FEATURES = 12;      // features per frame, scaled x1000 by the browser
constexpr int GAZE_DIM = FACE_FEATURES + 1;  // + bias term
constexpr int GAZE_TARGETS = 2;        // screen x, y
constexpr int32_t MAX_FEATURE_VALUE = 2000;  // |feature| <= 2.0 after x1000 scaling
constexpr int32_t MAX_TARGET_VALUE = 1000;   // targets 0-1 after x1000 scaling
constexpr int32_t BIAS_VALUE = 1000;         // the bias feature, 1.0 scaled

using DigitsPayload = ClassStats<uint32_t, DIGIT_CLASSES, DIGIT_FEATURES>;
using GesturePayload = ClassStats<int64_t, GESTURE_CLASSES, FACE_FEATURES>;
using GazePayload = RidgeStats<GAZE_DIM, GAZE_TARGETS>;

// Running totals: digit totals are wider than digit slots; int64 payloads total in int64.
using DigitsTotals = ClassStats<uint64_t, DIGIT_CLASSES, DIGIT_FEATURES>;
using GestureTotals = GesturePayload;
using GazeTotals = GazePayload;

size_t payload_bytes(PayloadType type);
constexpr size_t MAX_PAYLOAD_BYTES = sizeof(uint32_t) * DigitsPayload::VALUES;  // the largest
constexpr size_t MAX_ENCODED_SLOT_BYTES = HEADER_BYTES + MAX_PAYLOAD_BYTES;

// Any one payload, tagged by the slot's type.
union AnyPayload {
    DigitsPayload digits;
    GesturePayload gesture;
    GazePayload gaze;
};

void clear_payload(PayloadType type, AnyPayload& payload);

// Encoded slot = header + payload values, little-endian. Returns bytes written, or 0.
size_t encode_slot(const SlotHeader& header, const AnyPayload& payload, uint8_t* out, size_t capacity);

// Parses and checks an encoded slot: known type, our schema, exact length, possible contents.
Result decode_slot(const uint8_t* data, size_t length, SlotHeader& header, AnyPayload& payload);

// True if no real sequence of samples could produce these contents.
bool payload_valid(const SlotHeader& header, const AnyPayload& payload);

// Build a payload holding exactly one sample. Teaching = adding this to the slot with
// accumulate(), the same checked add that merging uses. Returns BadInput for bad values.
Result digit_sample(DigitsPayload& out, int label, const uint8_t pixels[DIGIT_FEATURES]);
Result gesture_sample(GesturePayload& out, int label, const int32_t features[FACE_FEATURES]);
Result gaze_sample(GazePayload& out, const int32_t features[FACE_FEATURES], int32_t target_x, int32_t target_y);

// target += sign * source for any payload type, checked and all-or-nothing.
Result accumulate_payload(PayloadType type, AnyPayload& target, const AnyPayload& source, int sign);

// Samples in a payload (sum of class counts, or n for ridge).
uint64_t payload_samples(PayloadType type, const AnyPayload& payload);

}  // namespace meshstate
