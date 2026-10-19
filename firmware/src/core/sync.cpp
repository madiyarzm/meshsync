#include "sync.h"

#include <cstring>

namespace construct {

// --------------------------------------------------------------------------------------
// Packets
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Sync engine
// --------------------------------------------------------------------------------------

void SyncEngine::begin(Node* node, const SlotCodec* codec, SendFunction send, SlotChangedFunction slot_changed,
                       void* context, uint32_t now_ms) {
    node_ = node;
    codec_ = codec;
    send_ = send;
    slot_changed_ = slot_changed;
    context_ = context;
    last_heartbeat_ms_ = now_ms - HEARTBEAT_INTERVAL_MS;  // so the first tick announces us right away
    last_chunk_ms_ = now_ms;
    stats_ = {};
    transfer_ = {};
    tx_slot_ = -1;
    next_send_slot_ = 0;
    for (int id = 0; id < MAX_NODES; id++) {
        peers_[id] = {};
        send_mask_[id] = 0;
    }
}

bool SyncEngine::peer_online(int id, uint32_t now_ms) const {
    if (id < 0 || id >= MAX_NODES || !peers_[id].seen) {
        return false;
    }
    return now_ms - peers_[id].last_seen_ms < PEER_ONLINE_MS;
}

void SyncEngine::tick(uint32_t now_ms) {
    // Unsigned subtraction stays correct when millis() wraps around after 49 days.
    if (now_ms - last_heartbeat_ms_ >= HEARTBEAT_INTERVAL_MS) {
        send_heartbeat();
        last_heartbeat_ms_ = now_ms;
    }
    check_transfer(now_ms);
    if (now_ms - last_chunk_ms_ >= CHUNK_INTERVAL_MS) {
        send_pending_chunks();
        last_chunk_ms_ = now_ms;
    }
}

void SyncEngine::on_packet(const uint8_t* data, size_t length, uint32_t now_ms) {
    stats_.packets_received++;
    PacketType type = packet_type(data, length);
    if (type == PacketType::Invalid || data[2] == node_->self_id) {
        stats_.packets_rejected++;  // garbage, or another board configured with our letter
        return;
    }

    if (type == PacketType::Heartbeat) {
        HeartbeatPacket packet;
        if (read_heartbeat(data, length, packet)) {
            handle_heartbeat(packet, now_ms);
            return;
        }
    } else if (type == PacketType::Request) {
        RequestPacket packet;
        if (read_request(data, length, packet)) {
            handle_request(packet);
            return;
        }
    } else if (type == PacketType::Chunk) {
        ChunkPacket packet;
        if (read_chunk(data, length, packet)) {
            handle_chunk(packet, now_ms);
            return;
        }
    }
    stats_.packets_rejected++;
}

// --- sending ----------------------------------------------------------------------

void SyncEngine::send_packet(size_t length) {
    if (length == 0) {
        return;
    }
    send_(context_, packet_, length);
    stats_.packets_sent++;
}

void SyncEngine::send_heartbeat() {
    HeartbeatPacket packet;
    packet.sender = node_->self_id;
    for (int slot = 0; slot < MAX_NODES; slot++) {
        packet.versions[slot] = node_->slots[slot].version;
    }
    send_packet(write_heartbeat(packet, packet_, sizeof(packet_)));
}

void SyncEngine::send_request() {
    RequestPacket packet;
    packet.sender = node_->self_id;
    packet.target = transfer_.source;
    packet.slot = transfer_.slot;
    packet.version = transfer_.version;
    // Before the first chunk we don't know how many there are, so ask for all of them.
    uint32_t wanted = transfer_.total == 0 ? 0xFFFFFFFFu : all_chunks_mask(transfer_.total);
    packet.missing_mask = wanted & ~transfer_.received_mask;
    send_packet(write_request(packet, packet_, sizeof(packet_)));
}

// Encode a slot into tx_buffer_ unless that exact version is already there.
bool SyncEngine::prepare_outgoing(int slot) {
    const Slot& current = node_->slots[slot];
    if (tx_slot_ == slot && tx_version_ == current.version) {
        return true;
    }
    tx_length_ = codec_->encode(current, tx_buffer_, sizeof(tx_buffer_));
    if (tx_length_ == 0 || chunks_for(tx_length_) > MAX_CHUNKS) {
        tx_slot_ = -1;
        return false;
    }
    tx_slot_ = slot;
    tx_version_ = current.version;
    tx_crc_ = crc32(tx_buffer_, tx_length_);
    return true;
}

void SyncEngine::send_pending_chunks() {
    int budget = CHUNKS_PER_INTERVAL;
    for (int step = 0; step < MAX_NODES && budget > 0; step++) {
        int slot = (next_send_slot_ + step) % MAX_NODES;
        if (send_mask_[slot] == 0) {
            continue;
        }
        if (!prepare_outgoing(slot)) {
            send_mask_[slot] = 0;
            continue;
        }
        int total = chunks_for(tx_length_);
        send_mask_[slot] &= all_chunks_mask(total);  // drop bits past the last chunk

        for (int index = 0; index < total && budget > 0; index++) {
            uint32_t bit = 1u << index;
            if ((send_mask_[slot] & bit) == 0) {
                continue;
            }
            size_t offset = size_t(index) * MAX_CHUNK_DATA;
            size_t remaining = tx_length_ - offset;
            ChunkPacket chunk;
            chunk.sender = node_->self_id;
            chunk.slot = uint8_t(slot);
            chunk.version = tx_version_;
            chunk.index = uint8_t(index);
            chunk.total = uint8_t(total);
            chunk.slot_crc = tx_crc_;
            chunk.length = uint8_t(remaining < MAX_CHUNK_DATA ? remaining : MAX_CHUNK_DATA);
            chunk.data = tx_buffer_ + offset;
            send_packet(write_chunk(chunk, packet_, sizeof(packet_)));
            stats_.chunks_sent++;
            send_mask_[slot] &= ~bit;
            budget--;
        }
        if (send_mask_[slot] == 0) {
            next_send_slot_ = (slot + 1) % MAX_NODES;  // take turns between slots
        }
    }
}

// --- receiving --------------------------------------------------------------------

void SyncEngine::handle_heartbeat(const HeartbeatPacket& packet, uint32_t now_ms) {
    PeerStatus& peer = peers_[packet.sender];
    peer.seen = true;
    peer.last_seen_ms = now_ms;
    for (int slot = 0; slot < MAX_NODES; slot++) {
        peer.versions[slot] = packet.versions[slot];
    }

    if (transfer_.active) {
        // Already fetching this slot, but the peer has an even newer version: switch to that.
        if (packet.versions[transfer_.slot] > transfer_.version) {
            start_transfer(transfer_.slot, packet.sender, packet.versions[transfer_.slot], now_ms);
            send_request();
        }
        return;
    }
    for (int slot = 0; slot < MAX_NODES; slot++) {
        if (packet.versions[slot] > node_->slots[slot].version) {
            start_transfer(slot, packet.sender, packet.versions[slot], now_ms);
            send_request();
            return;  // one at a time; the next heartbeat brings the next slot
        }
    }
}

void SyncEngine::handle_request(const RequestPacket& packet) {
    if (packet.target != node_->self_id) {
        return;  // asked someone else
    }
    const Slot& slot = node_->slots[packet.slot];
    if (slot.version == 0 || slot.version < packet.version) {
        return;  // we don't have that version (yet)
    }
    // If we have something newer, we send that instead; the requester switches to it.
    send_mask_[packet.slot] |= packet.missing_mask;
}

void SyncEngine::start_transfer(int slot, int source, uint32_t version, uint32_t now_ms) {
    transfer_ = {};
    transfer_.active = true;
    transfer_.slot = uint8_t(slot);
    transfer_.source = uint8_t(source);
    transfer_.version = version;
    transfer_.last_progress_ms = now_ms;
}

void SyncEngine::handle_chunk(const ChunkPacket& chunk, uint32_t now_ms) {
    if (chunk.version <= node_->slots[chunk.slot].version) {
        return;  // we already have this or newer
    }
    if (transfer_.active && transfer_.slot != chunk.slot) {
        return;  // busy with another slot
    }
    if (transfer_.active && chunk.version < transfer_.version) {
        return;  // older than what we're assembling
    }
    if (!transfer_.active || chunk.version > transfer_.version) {
        // Overheard chunks of a slot we need: collect them too. Missing ones get requested on retry.
        start_transfer(chunk.slot, chunk.sender, chunk.version, now_ms);
    }

    if (transfer_.total == 0) {
        transfer_.total = chunk.total;
        transfer_.slot_crc = chunk.slot_crc;
    } else if (chunk.total != transfer_.total || chunk.slot_crc != transfer_.slot_crc) {
        stats_.packets_rejected++;  // disagrees with earlier chunks of the same version: damaged
        return;
    }

    uint32_t bit = 1u << chunk.index;
    if (transfer_.received_mask & bit) {
        return;  // duplicate
    }
    std::memcpy(assembly_ + size_t(chunk.index) * MAX_CHUNK_DATA, chunk.data, chunk.length);
    transfer_.received_mask |= bit;
    if (chunk.index == transfer_.total - 1) {
        transfer_.last_chunk_length = chunk.length;
    }
    transfer_.last_progress_ms = now_ms;
    transfer_.retries = 0;

    if (transfer_.received_mask == all_chunks_mask(transfer_.total)) {
        finish_transfer();
    }
}

void SyncEngine::finish_transfer() {
    transfer_.active = false;
    size_t length = size_t(transfer_.total - 1) * MAX_CHUNK_DATA + transfer_.last_chunk_length;
    if (crc32(assembly_, length) != transfer_.slot_crc) {
        stats_.crc_failures++;  // a damaged chunk got through; the next heartbeat starts over
        return;
    }
    if (!codec_->decode(assembly_, length, scratch_) || scratch_.version != transfer_.version) {
        stats_.packets_rejected++;
        return;
    }
    if (node_->receive(transfer_.slot, scratch_)) {
        stats_.slots_applied++;
        if (slot_changed_ != nullptr) {
            slot_changed_(context_, transfer_.slot);
        }
    }
}

void SyncEngine::check_transfer(uint32_t now_ms) {
    if (!transfer_.active) {
        return;
    }
    if (node_->slots[transfer_.slot].version >= transfer_.version) {
        transfer_.active = false;  // got it another way meanwhile
        return;
    }
    if (now_ms - transfer_.last_progress_ms < REQUEST_RETRY_MS) {
        return;
    }
    if (transfer_.retries >= MAX_REQUEST_RETRIES) {
        transfer_.active = false;  // source unreachable; a later heartbeat picks a new one
        stats_.transfers_abandoned++;
        return;
    }
    transfer_.retries++;
    transfer_.last_progress_ms = now_ms;
    send_request();
}

}  // namespace construct
