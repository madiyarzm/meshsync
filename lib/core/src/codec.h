#pragma once

#include <cstddef>
#include <cstdint>

#include "core_constants.h"
#include "slot.h"

namespace construct {

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

}  // namespace construct
