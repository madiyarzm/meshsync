#pragma once

#include <cstddef>
#include <cstdint>

#include "codec.h"
#include "core_constants.h"

namespace construct {

// Wire format of the three sync packets. Every packet starts with
// [magic][type][sender] and fits in one ESP-NOW frame. All integers are little-endian.

constexpr size_t MAX_PACKET_BYTES = 250;  // ESP-NOW payload limit
constexpr uint8_t PACKET_MAGIC = 0xC7;    // quick filter for stray traffic

// [magic][type][sender][slot count][version x MAX_NODES]
constexpr size_t HEARTBEAT_BYTES = 4 + 4 * MAX_NODES;
// [magic][type][sender][target][slot][version u32][missing chunks mask u32]
constexpr size_t REQUEST_BYTES = 13;
// [magic][type][sender][slot][version u32][index][total][slot crc u32][length] + data
constexpr size_t CHUNK_HEADER_BYTES = 15;
constexpr size_t MAX_CHUNK_DATA = MAX_PACKET_BYTES - CHUNK_HEADER_BYTES;  // 235

constexpr int MAX_CHUNKS = 32;  // the request's missing-chunk mask has 32 bits
constexpr size_t MAX_ENCODED_SLOT_BYTES = MAX_CHUNKS * MAX_CHUNK_DATA;
static_assert(RAW_SLOT_BYTES <= MAX_ENCODED_SLOT_BYTES, "a raw slot must fit in 32 chunks");

enum class PacketType : uint8_t {
    Invalid = 0,
    Heartbeat = 1,  // "here are the slot versions I have"
    Request = 2,    // "target: please send these chunks of this slot"
    Chunk = 3,      // one piece of an encoded slot
};

struct HeartbeatPacket {
    uint8_t sender;
    uint32_t versions[MAX_NODES];  // the version vector
};

struct RequestPacket {
    uint8_t sender;
    uint8_t target;  // only this node answers
    uint8_t slot;
    uint32_t version;       // the version the sender is assembling
    uint32_t missing_mask;  // bit i set = chunk i still needed
};

struct ChunkPacket {
    uint8_t sender;
    uint8_t slot;
    uint32_t version;
    uint8_t index;
    uint8_t total;
    uint32_t slot_crc;    // CRC32 of the whole encoded slot, checked after assembly
    uint8_t length;       // bytes of data in this chunk
    const uint8_t* data;  // points into the packet buffer
};

// Type of a packet that passes the basic checks, or Invalid.
PacketType packet_type(const uint8_t* data, size_t length);

// write_* return the number of bytes written, or 0 if the packet is invalid or does not fit.
// read_* return false for truncated or malformed packets.
size_t write_heartbeat(const HeartbeatPacket& packet, uint8_t* out, size_t capacity);
bool read_heartbeat(const uint8_t* data, size_t length, HeartbeatPacket& out);

size_t write_request(const RequestPacket& packet, uint8_t* out, size_t capacity);
bool read_request(const uint8_t* data, size_t length, RequestPacket& out);

size_t write_chunk(const ChunkPacket& packet, uint8_t* out, size_t capacity);
bool read_chunk(const uint8_t* data, size_t length, ChunkPacket& out);

// Number of chunks needed for an encoded slot of this many bytes.
inline int chunks_for(size_t encoded_bytes) {
    return int((encoded_bytes + MAX_CHUNK_DATA - 1) / MAX_CHUNK_DATA);
}

// Mask with the lowest `total` bits set.
inline uint32_t all_chunks_mask(int total) {
    return total >= 32 ? 0xFFFFFFFFu : ((1u << total) - 1u);
}

}  // namespace construct
