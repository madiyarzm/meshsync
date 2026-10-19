#include "codec.h"

namespace construct {

namespace {

// Byte by byte, so the format is little-endian on any CPU.
void write_u32(uint8_t*& out, uint32_t value) {
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> 8);
    out[2] = uint8_t(value >> 16);
    out[3] = uint8_t(value >> 24);
    out += 4;
}

uint32_t read_u32(const uint8_t*& in) {
    uint32_t value = uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) |
                     (uint32_t(in[3]) << 24);
    in += 4;
    return value;
}

}  // namespace

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

}  // namespace construct
