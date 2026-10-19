#pragma once

#include <cstdint>

namespace construct {

// Byte by byte, so the format is little-endian on any CPU. Both advance the pointer.

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

}  // namespace construct
