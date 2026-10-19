#include "codec.h"

#include "bytes.h"

namespace construct {

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
