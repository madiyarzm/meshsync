#include "crc32.h"

namespace construct {

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
