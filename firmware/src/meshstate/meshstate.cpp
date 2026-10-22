#include "meshstate.h"

namespace meshstate {

const char* result_name(Result result) {
    switch (result) {
        case Result::Ok: return "OK";
        case Result::Unchanged: return "UNCHANGED";
        case Result::BadInput: return "BAD_INPUT";
        case Result::Overflow: return "OVERFLOW";
        case Result::NotOwner: return "NOT_OWNER";
        case Result::UnknownSession: return "UNKNOWN_SESSION";
        case Result::TooManyOpen: return "TOO_MANY_OPEN";
        case Result::StoreFull: return "STORE_FULL";
        case Result::TombstonesFull: return "TOMBSTONES_FULL";
        case Result::UsersFull: return "USERS_FULL";
        case Result::SchemaMismatch: return "SCHEMA";
        case Result::Deleted: return "DELETED";
        case Result::Invalid: return "INVALID";
        case Result::StorageError: return "STORAGE";
    }
    return "UNKNOWN";
}

bool payload_type_known(uint8_t type) {
    return type >= 1 && type <= NUM_PAYLOAD_TYPES;
}

uint8_t schema_for(PayloadType type) {
    switch (type) {
        case PayloadType::Digits: return 1;
        case PayloadType::GestureBlend: return 1;
        case PayloadType::GestureLandmark: return 1;
        case PayloadType::Gaze: return 1;
    }
    return 0;
}

size_t payload_bytes(PayloadType type) {
    switch (type) {
        case PayloadType::Digits: return sizeof(uint32_t) * DigitsPayload::VALUES;
        case PayloadType::GestureBlend:
        case PayloadType::GestureLandmark: return sizeof(int64_t) * GesturePayload::VALUES;
        case PayloadType::Gaze: return sizeof(int64_t) * GazePayload::VALUES;
    }
    return 0;
}

void clear_payload(PayloadType type, AnyPayload& payload) {
    if (type == PayloadType::Digits) {
        payload.digits.clear();
    } else if (type == PayloadType::Gaze) {
        payload.gaze.clear();
    } else {
        payload.gesture.clear();
    }
}

void write_header(uint8_t*& out, const SlotHeader& header) {
    write_u8(out, header.key.device);
    write_u8(out, header.key.type);
    write_u16(out, header.key.session);
    write_u8(out, header.schema);
    write_u16(out, header.user);
    write_u32(out, header.user_epoch);
    write_u32(out, header.version);
}

SlotHeader read_header(const uint8_t*& in) {
    SlotHeader header;
    header.key.device = read_u8(in);
    header.key.type = read_u8(in);
    header.key.session = read_u16(in);
    header.schema = read_u8(in);
    header.user = read_u16(in);
    header.user_epoch = read_u32(in);
    header.version = read_u32(in);
    return header;
}

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

// --------------------------------------------------------------------------------------
// Encoding
// --------------------------------------------------------------------------------------

size_t encode_slot(const SlotHeader& header, const AnyPayload& payload, uint8_t* out, size_t capacity) {
    PayloadType type = PayloadType(header.key.type);
    if (out == nullptr || !payload_type_known(header.key.type)) {
        return 0;
    }
    size_t total = HEADER_BYTES + payload_bytes(type);
    if (capacity < total) {
        return 0;
    }
    write_header(out, header);
    if (type == PayloadType::Digits) {
        for (int i = 0; i < DigitsPayload::VALUES; i++) {
            write_u32(out, payload.digits.values[i]);
        }
    } else if (type == PayloadType::Gaze) {
        for (int i = 0; i < GazePayload::VALUES; i++) {
            write_u64(out, uint64_t(payload.gaze.values[i]));
        }
    } else {
        for (int i = 0; i < GesturePayload::VALUES; i++) {
            write_u64(out, uint64_t(payload.gesture.values[i]));
        }
    }
    return total;
}

Result decode_slot(const uint8_t* data, size_t length, SlotHeader& header, AnyPayload& payload) {
    if (data == nullptr || length < HEADER_BYTES) {
        return Result::Invalid;
    }
    const uint8_t* in = data;
    header = read_header(in);
    if (!payload_type_known(header.key.type) || header.key.device >= MAX_DEVICES) {
        return Result::Invalid;
    }
    PayloadType type = PayloadType(header.key.type);
    if (header.schema != schema_for(type)) {
        return Result::SchemaMismatch;  // a different layout: merging it would mix up fields
    }
    if (length != HEADER_BYTES + payload_bytes(type)) {
        return Result::Invalid;  // truncated or padded
    }
    if (type == PayloadType::Digits) {
        for (int i = 0; i < DigitsPayload::VALUES; i++) {
            payload.digits.values[i] = read_u32(in);
        }
    } else if (type == PayloadType::Gaze) {
        for (int i = 0; i < GazePayload::VALUES; i++) {
            payload.gaze.values[i] = int64_t(read_u64(in));
        }
    } else {
        for (int i = 0; i < GesturePayload::VALUES; i++) {
            payload.gesture.values[i] = int64_t(read_u64(in));
        }
    }
    return payload_valid(header, payload) ? Result::Ok : Result::Invalid;
}

// --------------------------------------------------------------------------------------
// Validity: reject contents no real sequence of samples could produce
// --------------------------------------------------------------------------------------

namespace {

bool digits_valid(const SlotHeader& header, const DigitsPayload& p) {
    if (header.user != 0 || header.key.session != 0) {
        return false;  // one digit slot per device keeps the digit math within 64 bits
    }
    uint64_t total = 0;
    for (int c = 0; c < DIGIT_CLASSES; c++) {
        uint64_t n = p.count(c);
        if (n > DIGIT_MAX_PER_CLASS) {
            return false;
        }
        total += n;
        for (int f = 0; f < DIGIT_FEATURES; f++) {
            uint64_t s = p.sum(c, f);
            uint64_t sq = p.sumsq(c, f);
            // Every value v is 0-16, so v <= v*v <= 16*v. Summed over samples:
            if (s > uint64_t(DIGIT_MAX_PIXEL) * n || sq < s || sq > uint64_t(DIGIT_MAX_PIXEL) * s) {
                return false;
            }
        }
    }
    return header.version >= total;  // every sample bumps the version once
}

bool gesture_valid(const SlotHeader& header, const GesturePayload& p) {
    if (header.user == 0) {
        return false;
    }
    const int64_t max_value = MAX_FEATURE_VALUE;
    uint64_t total = 0;
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        int64_t n = p.count(c);
        if (n < 0 || n > int64_t(MAX_SESSION_SAMPLES)) {
            return false;
        }
        total += uint64_t(n);
        for (int f = 0; f < FACE_FEATURES; f++) {
            int64_t s = p.sum(c, f);
            int64_t sq = p.sumsq(c, f);
            if (s > max_value * n || s < -max_value * n || sq < 0 || sq > max_value * max_value * n) {
                return false;
            }
            if (s * s > n * sq) {
                return false;  // (sum x)^2 <= n * sum x^2 always holds for real data
            }
        }
    }
    return header.version >= total;
}

bool gaze_valid(const SlotHeader& header, const GazePayload& p) {
    if (header.user == 0) {
        return false;
    }
    int64_t n = p.n();
    if (n < 0 || n > int64_t(MAX_SESSION_SAMPLES)) {
        return false;
    }
    const int64_t max_x = MAX_FEATURE_VALUE;
    const int64_t max_y = MAX_TARGET_VALUE;
    if (p.xtx(0, 0) != int64_t(BIAS_VALUE) * BIAS_VALUE * n) {
        return false;  // feature 0 is always the bias
    }
    for (int i = 0; i < GAZE_DIM; i++) {
        for (int j = 0; j < GAZE_DIM; j++) {
            int64_t value = p.xtx(i, j);
            if (value != p.xtx(j, i) || value > max_x * max_x * n || value < -max_x * max_x * n) {
                return false;
            }
        }
        if (p.xtx(i, i) < 0) {
            return false;
        }
        for (int k = 0; k < GAZE_TARGETS; k++) {
            int64_t value = p.xty(i, k);
            if (value > max_x * max_y * n || value < -max_x * max_y * n) {
                return false;
            }
        }
    }
    for (int k = 0; k < GAZE_TARGETS; k++) {
        if (p.yty(k) < 0 || p.yty(k) > max_y * max_y * n) {
            return false;
        }
    }
    return header.version >= uint64_t(n);
}

}  // namespace

bool payload_valid(const SlotHeader& header, const AnyPayload& payload) {
    switch (PayloadType(header.key.type)) {
        case PayloadType::Digits: return digits_valid(header, payload.digits);
        case PayloadType::GestureBlend:
        case PayloadType::GestureLandmark: return gesture_valid(header, payload.gesture);
        case PayloadType::Gaze: return gaze_valid(header, payload.gaze);
    }
    return false;
}

// --------------------------------------------------------------------------------------
// Adding samples
// --------------------------------------------------------------------------------------

Result digit_sample(DigitsPayload& out, int label, const uint8_t pixels[DIGIT_FEATURES]) {
    if (label < 0 || label >= DIGIT_CLASSES || pixels == nullptr) {
        return Result::BadInput;
    }
    for (int f = 0; f < DIGIT_FEATURES; f++) {
        if (pixels[f] > DIGIT_MAX_PIXEL) {
            return Result::BadInput;
        }
    }
    out.clear();
    out.count(label) = 1;
    for (int f = 0; f < DIGIT_FEATURES; f++) {
        uint32_t value = pixels[f];
        out.sum(label, f) = value;
        out.sumsq(label, f) = value * value;
    }
    return Result::Ok;
}

static bool features_valid(const int32_t features[FACE_FEATURES]) {
    if (features == nullptr) {
        return false;
    }
    for (int f = 0; f < FACE_FEATURES; f++) {
        if (features[f] > MAX_FEATURE_VALUE || features[f] < -MAX_FEATURE_VALUE) {
            return false;
        }
    }
    return true;
}

Result gesture_sample(GesturePayload& out, int label, const int32_t features[FACE_FEATURES]) {
    if (label < 0 || label >= GESTURE_CLASSES || !features_valid(features)) {
        return Result::BadInput;
    }
    out.clear();
    out.count(label) = 1;
    for (int f = 0; f < FACE_FEATURES; f++) {
        int64_t x = features[f];
        out.sum(label, f) = x;
        out.sumsq(label, f) = x * x;
    }
    return Result::Ok;
}

Result gaze_sample(GazePayload& out, const int32_t features[FACE_FEATURES], int32_t target_x, int32_t target_y) {
    if (!features_valid(features) || target_x < 0 || target_x > MAX_TARGET_VALUE || target_y < 0 ||
        target_y > MAX_TARGET_VALUE) {
        return Result::BadInput;
    }
    int64_t x[GAZE_DIM];
    x[0] = BIAS_VALUE;
    for (int f = 0; f < FACE_FEATURES; f++) {
        x[f + 1] = features[f];
    }
    int64_t y[GAZE_TARGETS] = {target_x, target_y};

    out.clear();
    out.n() = 1;
    for (int i = 0; i < GAZE_DIM; i++) {
        for (int j = 0; j < GAZE_DIM; j++) {
            out.xtx(i, j) = x[i] * x[j];
        }
        for (int k = 0; k < GAZE_TARGETS; k++) {
            out.xty(i, k) = x[i] * y[k];
        }
    }
    for (int k = 0; k < GAZE_TARGETS; k++) {
        out.yty(k) = y[k] * y[k];
    }
    return Result::Ok;
}

Result accumulate_payload(PayloadType type, AnyPayload& target, const AnyPayload& source, int sign) {
    switch (type) {
        case PayloadType::Digits:
            return accumulate(target.digits.values, source.digits.values, DigitsPayload::VALUES, sign);
        case PayloadType::GestureBlend:
        case PayloadType::GestureLandmark:
            return accumulate(target.gesture.values, source.gesture.values, GesturePayload::VALUES, sign);
        case PayloadType::Gaze:
            return accumulate(target.gaze.values, source.gaze.values, GazePayload::VALUES, sign);
    }
    return Result::Invalid;
}

uint64_t payload_samples(PayloadType type, const AnyPayload& payload) {
    switch (type) {
        case PayloadType::Digits: return payload.digits.total_count();
        case PayloadType::GestureBlend:
        case PayloadType::GestureLandmark: return payload.gesture.total_count();
        case PayloadType::Gaze: return uint64_t(payload.gaze.n());
    }
    return 0;
}

}  // namespace meshstate
