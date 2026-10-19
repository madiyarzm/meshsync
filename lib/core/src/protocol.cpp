#include "protocol.h"

#include <cstring>

#include "bytes.h"

namespace construct {

namespace {

bool valid_id(uint8_t id) {
    return id < MAX_NODES;
}

void write_header(uint8_t*& out, PacketType type, uint8_t sender) {
    out[0] = PACKET_MAGIC;
    out[1] = uint8_t(type);
    out[2] = sender;
    out += 3;
}

}  // namespace

PacketType packet_type(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 3 || data[0] != PACKET_MAGIC || !valid_id(data[2])) {
        return PacketType::Invalid;
    }
    switch (data[1]) {
        case uint8_t(PacketType::Heartbeat):
            return PacketType::Heartbeat;
        case uint8_t(PacketType::Request):
            return PacketType::Request;
        case uint8_t(PacketType::Chunk):
            return PacketType::Chunk;
        default:
            return PacketType::Invalid;
    }
}

size_t write_heartbeat(const HeartbeatPacket& packet, uint8_t* out, size_t capacity) {
    if (out == nullptr || capacity < HEARTBEAT_BYTES || !valid_id(packet.sender)) {
        return 0;
    }
    write_header(out, PacketType::Heartbeat, packet.sender);
    *out++ = MAX_NODES;
    for (int slot = 0; slot < MAX_NODES; slot++) {
        write_u32(out, packet.versions[slot]);
    }
    return HEARTBEAT_BYTES;
}

bool read_heartbeat(const uint8_t* data, size_t length, HeartbeatPacket& out) {
    // A different slot count means the peer was built with another MAX_NODES.
    if (length != HEARTBEAT_BYTES || packet_type(data, length) != PacketType::Heartbeat ||
        data[3] != MAX_NODES) {
        return false;
    }
    out.sender = data[2];
    const uint8_t* in = data + 4;
    for (int slot = 0; slot < MAX_NODES; slot++) {
        out.versions[slot] = read_u32(in);
    }
    return true;
}

size_t write_request(const RequestPacket& packet, uint8_t* out, size_t capacity) {
    if (out == nullptr || capacity < REQUEST_BYTES || !valid_id(packet.sender) || !valid_id(packet.target) ||
        !valid_id(packet.slot)) {
        return 0;
    }
    write_header(out, PacketType::Request, packet.sender);
    *out++ = packet.target;
    *out++ = packet.slot;
    write_u32(out, packet.version);
    write_u32(out, packet.missing_mask);
    return REQUEST_BYTES;
}

bool read_request(const uint8_t* data, size_t length, RequestPacket& out) {
    if (length != REQUEST_BYTES || packet_type(data, length) != PacketType::Request) {
        return false;
    }
    out.sender = data[2];
    out.target = data[3];
    out.slot = data[4];
    const uint8_t* in = data + 5;
    out.version = read_u32(in);
    out.missing_mask = read_u32(in);
    return valid_id(out.target) && valid_id(out.slot);
}

size_t write_chunk(const ChunkPacket& packet, uint8_t* out, size_t capacity) {
    size_t size = CHUNK_HEADER_BYTES + packet.length;
    if (out == nullptr || packet.data == nullptr || capacity < size || packet.length == 0 ||
        packet.length > MAX_CHUNK_DATA || !valid_id(packet.sender) || !valid_id(packet.slot)) {
        return 0;
    }
    write_header(out, PacketType::Chunk, packet.sender);
    *out++ = packet.slot;
    write_u32(out, packet.version);
    *out++ = packet.index;
    *out++ = packet.total;
    write_u32(out, packet.slot_crc);
    *out++ = packet.length;
    std::memcpy(out, packet.data, packet.length);
    return size;
}

bool read_chunk(const uint8_t* data, size_t length, ChunkPacket& out) {
    if (length <= CHUNK_HEADER_BYTES || packet_type(data, length) != PacketType::Chunk) {
        return false;
    }
    out.sender = data[2];
    out.slot = data[3];
    const uint8_t* in = data + 4;
    out.version = read_u32(in);
    out.index = in[0];
    out.total = in[1];
    in += 2;
    out.slot_crc = read_u32(in);
    out.length = in[0];
    out.data = in + 1;

    if (!valid_id(out.slot) || out.total == 0 || out.total > MAX_CHUNKS || out.index >= out.total) {
        return false;
    }
    if (out.length == 0 || CHUNK_HEADER_BYTES + out.length != length) {
        return false;  // truncated, or the length byte is wrong
    }
    // Only the last chunk may be short, so every chunk's offset is index * MAX_CHUNK_DATA.
    bool is_last = out.index == out.total - 1;
    return is_last || out.length == MAX_CHUNK_DATA;
}

}  // namespace construct
