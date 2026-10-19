#pragma once

#include <cstddef>
#include <cstdint>

namespace construct {

// Standard CRC-32 (IEEE 802.3), the same as Python's zlib.crc32.
// Data that arrives in pieces can be checked by chaining:
//   crc32_update(crc32_update(0, a, len_a), b, len_b) == crc32 of a followed by b
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length);

inline uint32_t crc32(const uint8_t* data, size_t length) {
    return crc32_update(0, data, length);
}

}  // namespace construct
