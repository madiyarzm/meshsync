#include "sync.h"

#include <cstring>

namespace meshstate {

// ======================================================================================
// Packets
// ======================================================================================

namespace {

bool valid_id(uint8_t id) {
    return id < MAX_DEVICES;
}

void write_packet_header(uint8_t*& out, PacketType type, uint8_t sender) {
    write_u8(out, PACKET_MAGIC);
    write_u8(out, uint8_t(type));
    write_u8(out, sender);
}

}  // namespace

PacketType packet_type(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 3 || data[0] != PACKET_MAGIC || !valid_id(data[2])) {
        return PacketType::Invalid;
    }
    if (data[1] >= uint8_t(PacketType::Heartbeat) && data[1] <= uint8_t(PacketType::Chunk)) {
        return PacketType(data[1]);
    }
    return PacketType::Invalid;
}

size_t write_heartbeat(const HeartbeatPacket& packet, uint8_t* out, size_t capacity) {
    if (out == nullptr || capacity < HEARTBEAT_BYTES || !valid_id(packet.sender)) {
        return 0;
    }
    write_packet_header(out, PacketType::Heartbeat, packet.sender);
    write_u8(out, packet.app);
    write_u16(out, packet.entries);
    write_u32(out, packet.digest);
    return HEARTBEAT_BYTES;
}

bool read_heartbeat(const uint8_t* data, size_t length, HeartbeatPacket& out) {
    if (length != HEARTBEAT_BYTES || packet_type(data, length) != PacketType::Heartbeat) {
        return false;
    }
    const uint8_t* in = data + 2;
    out.sender = read_u8(in);
    out.app = read_u8(in);
    out.entries = read_u16(in);
    out.digest = read_u32(in);
    return true;
}

size_t write_inventory_request(const InventoryRequestPacket& packet, uint8_t* out, size_t capacity) {
    if (out == nullptr || capacity < INVENTORY_REQUEST_BYTES || !valid_id(packet.sender) || !valid_id(packet.target)) {
        return 0;
    }
    write_packet_header(out, PacketType::InventoryRequest, packet.sender);
    write_u8(out, packet.target);
    write_u8(out, packet.page);
    return INVENTORY_REQUEST_BYTES;
}

bool read_inventory_request(const uint8_t* data, size_t length, InventoryRequestPacket& out) {
    if (length != INVENTORY_REQUEST_BYTES || packet_type(data, length) != PacketType::InventoryRequest) {
        return false;
    }
    out.sender = data[2];
    out.target = data[3];
    out.page = data[4];
    return valid_id(out.target);
}

size_t write_inventory(const InventoryPacket& packet, uint8_t* out, size_t capacity) {
    size_t size = INVENTORY_HEADER_BYTES + size_t(packet.count) * INVENTORY_ENTRY_BYTES + INVENTORY_CRC_BYTES;
    if (out == nullptr || capacity < size || packet.count > ENTRIES_PER_PAGE || !valid_id(packet.sender)) {
        return 0;
    }
    const uint8_t* packet_start = out;
    write_packet_header(out, PacketType::Inventory, packet.sender);
    write_u8(out, packet.page);
    write_u8(out, packet.pages);
    write_u8(out, packet.count);
    for (int i = 0; i < packet.count; i++) {
        write_u8(out, packet.entries[i].kind);
        write_u32(out, packet.entries[i].id);
        write_u32(out, packet.entries[i].value);
    }
    write_u32(out, crc32(packet_start, size - INVENTORY_CRC_BYTES));
    return size;
}

bool read_inventory(const uint8_t* data, size_t length, InventoryPacket& out) {
    if (length < INVENTORY_HEADER_BYTES || packet_type(data, length) != PacketType::Inventory) {
        return false;
    }
    out.sender = data[2];
    out.page = data[3];
    out.pages = data[4];
    out.count = data[5];
    if (out.count > ENTRIES_PER_PAGE || out.pages == 0 || out.page >= out.pages ||
        length != INVENTORY_HEADER_BYTES + size_t(out.count) * INVENTORY_ENTRY_BYTES + INVENTORY_CRC_BYTES) {
        return false;
    }
    const uint8_t* crc_field = data + length - INVENTORY_CRC_BYTES;
    if (read_u32(crc_field) != crc32(data, length - INVENTORY_CRC_BYTES)) {
        return false;  // damaged on the way: drop the whole page
    }
    const uint8_t* in = data + INVENTORY_HEADER_BYTES;
    for (int i = 0; i < out.count; i++) {
        out.entries[i].kind = read_u8(in);
        out.entries[i].id = read_u32(in);
        out.entries[i].value = read_u32(in);
        if (out.entries[i].kind < ENTRY_SLOT || out.entries[i].kind > ENTRY_USER_EPOCH) {
            return false;
        }
    }
    return true;
}

size_t write_request(const RequestPacket& packet, uint8_t* out, size_t capacity) {
    if (out == nullptr || capacity < REQUEST_BYTES || !valid_id(packet.sender) || !valid_id(packet.target)) {
        return 0;
    }
    write_packet_header(out, PacketType::Request, packet.sender);
    write_u8(out, packet.target);
    write_u32(out, packet.key);
    write_u32(out, packet.version);
    write_u32(out, packet.missing_mask);
    return REQUEST_BYTES;
}

bool read_request(const uint8_t* data, size_t length, RequestPacket& out) {
    if (length != REQUEST_BYTES || packet_type(data, length) != PacketType::Request) {
        return false;
    }
    const uint8_t* in = data + 2;
    out.sender = read_u8(in);
    out.target = read_u8(in);
    out.key = read_u32(in);
    out.version = read_u32(in);
    out.missing_mask = read_u32(in);
    return valid_id(out.target);
}

size_t write_chunk(const ChunkPacket& packet, uint8_t* out, size_t capacity) {
    size_t size = CHUNK_HEADER_BYTES + packet.length;
    if (out == nullptr || packet.data == nullptr || capacity < size || packet.length == 0 ||
        packet.length > MAX_CHUNK_DATA || !valid_id(packet.sender)) {
        return 0;
    }
    write_packet_header(out, PacketType::Chunk, packet.sender);
    write_u32(out, packet.key);
    write_u32(out, packet.version);
    write_u8(out, packet.index);
    write_u8(out, packet.total);
    write_u32(out, packet.slot_crc);
    write_u8(out, packet.length);
    std::memcpy(out, packet.data, packet.length);
    return size;
}

bool read_chunk(const uint8_t* data, size_t length, ChunkPacket& out) {
    if (length <= CHUNK_HEADER_BYTES || packet_type(data, length) != PacketType::Chunk) {
        return false;
    }
    const uint8_t* in = data + 2;
    out.sender = read_u8(in);
    out.key = read_u32(in);
    out.version = read_u32(in);
    out.index = read_u8(in);
    out.total = read_u8(in);
    out.slot_crc = read_u32(in);
    out.length = read_u8(in);
    out.data = in;
    if (out.total == 0 || out.total > MAX_CHUNKS || out.index >= out.total) {
        return false;
    }
    if (out.length == 0 || CHUNK_HEADER_BYTES + out.length != length) {
        return false;  // truncated, or the length byte is wrong
    }
    // Only the last chunk may be short, so every chunk's offset is index * MAX_CHUNK_DATA.
    return out.index == out.total - 1 || out.length == MAX_CHUNK_DATA;
}

// ======================================================================================
// Engine: setup and housekeeping
// ======================================================================================

void SyncEngine::begin(Replica* replica, uint8_t app, SendFunction send, EventFunction on_event, void* context,
                       uint32_t now_ms) {
    replica_ = replica;
    app_ = app;
    send_ = send;
    on_event_ = on_event;
    context_ = context;
    last_heartbeat_ms_ = now_ms - HEARTBEAT_INTERVAL_MS;  // so the first tick announces us right away
    last_chunk_ms_ = now_ms;
    stats_ = {};
    fetch_ = {};
    transfer_ = {};
    wanted_count_ = 0;
    tx_version_ = 0;
    next_send_ = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        peers_[i] = {};
    }
    for (int i = 0; i < MAX_PENDING_SENDS; i++) {
        sends_[i] = {};
    }
}

bool SyncEngine::peer_online(int id, uint32_t now_ms) const {
    if (id < 0 || id >= MAX_DEVICES || !peers_[id].seen) {
        return false;
    }
    return now_ms - peers_[id].last_seen_ms < PEER_ONLINE_MS;
}

void SyncEngine::send_packet(size_t length) {
    if (length == 0) {
        return;
    }
    send_(context_, packet_, length);
    stats_.packets_sent++;
}

void SyncEngine::notify(SyncEvent event, uint32_t id) {
    if (on_event_ != nullptr) {
        on_event_(context_, event, id);
    }
}

// The newest version we hold of a slot, saved or still being written.
uint32_t SyncEngine::local_version(uint32_t key) const {
    SlotKey slot = SlotKey::unpack(key);
    uint32_t version = replica_->session_version(slot);
    SlotHeader header;
    if (replica_->find_slot(key, header) && header.version > version) {
        version = header.version;
    }
    return version;
}

void SyncEngine::tick(uint32_t now_ms) {
    // Unsigned subtraction stays correct when millis() wraps around after 49 days.
    if (now_ms - last_heartbeat_ms_ >= HEARTBEAT_INTERVAL_MS) {
        HeartbeatPacket heartbeat;
        heartbeat.sender = replica_->device_id();
        heartbeat.app = app_;
        heartbeat.entries = uint16_t(replica_->slot_count() + replica_->tombstone_count() + replica_->user_count());
        heartbeat.digest = replica_->digest();
        send_packet(write_heartbeat(heartbeat, packet_, sizeof(packet_)));
        last_heartbeat_ms_ = now_ms;
    }
    check_timeouts(now_ms);
    if (!transfer_.active) {
        start_next_transfer(now_ms);
    }
    if (now_ms - last_chunk_ms_ >= CHUNK_INTERVAL_MS) {
        send_pending_chunks();
        last_chunk_ms_ = now_ms;
    }
}

void SyncEngine::on_packet(const uint8_t* data, size_t length, uint32_t now_ms) {
    stats_.packets_received++;
    PacketType type = packet_type(data, length);
    if (type == PacketType::Invalid || data[2] == replica_->device_id()) {
        stats_.packets_rejected++;  // garbage, or another board configured with our letter
        return;
    }
    bool ok = false;
    if (type == PacketType::Heartbeat) {
        HeartbeatPacket packet;
        ok = read_heartbeat(data, length, packet);
        if (ok) {
            handle_heartbeat(packet, now_ms);
        }
    } else if (type == PacketType::InventoryRequest) {
        InventoryRequestPacket packet;
        ok = read_inventory_request(data, length, packet);
        if (ok) {
            handle_inventory_request(packet);
        }
    } else if (type == PacketType::Inventory) {
        ok = read_inventory(data, length, rx_inventory_);
        if (ok) {
            handle_inventory(rx_inventory_, now_ms);
        }
    } else if (type == PacketType::Request) {
        RequestPacket packet;
        ok = read_request(data, length, packet);
        if (ok) {
            handle_request(packet);
        }
    } else if (type == PacketType::Chunk) {
        ChunkPacket packet;
        ok = read_chunk(data, length, packet);
        if (ok) {
            handle_chunk(packet, now_ms);
        }
    }
    if (!ok) {
        stats_.packets_rejected++;
    }
}

// ======================================================================================
// Inventory: find out what differs
// ======================================================================================

void SyncEngine::handle_heartbeat(const HeartbeatPacket& packet, uint32_t now_ms) {
    if (packet.app != app_) {
        return;  // a board running the other app mode
    }
    PeerStatus& peer = peers_[packet.sender];
    peer.seen = true;
    peer.last_seen_ms = now_ms;
    peer.digest = packet.digest;
    peer.entries = packet.entries;

    if (packet.digest == replica_->digest() || fetch_.active) {
        return;  // same state, or already reading someone's inventory
    }
    if (peer.last_inventory_ms != 0 && now_ms - peer.last_inventory_ms < INVENTORY_COOLDOWN_MS) {
        return;
    }
    fetch_ = {true, packet.sender, 0, now_ms, 0};
    request_page();
}

void SyncEngine::request_page() {
    InventoryRequestPacket request = {replica_->device_id(), fetch_.peer, fetch_.next_page};
    send_packet(write_inventory_request(request, packet_, sizeof(packet_)));
}

void SyncEngine::handle_inventory_request(const InventoryRequestPacket& packet) {
    if (packet.target != replica_->device_id()) {
        return;
    }
    int count = replica_->inventory(inventory_, MAX_INVENTORY);
    int pages = count == 0 ? 1 : (count + ENTRIES_PER_PAGE - 1) / ENTRIES_PER_PAGE;
    if (packet.page >= pages) {
        return;
    }
    InventoryPacket& page = tx_inventory_;
    page.sender = replica_->device_id();
    page.page = packet.page;
    page.pages = uint8_t(pages);
    page.count = 0;
    for (int i = packet.page * ENTRIES_PER_PAGE; i < count && page.count < ENTRIES_PER_PAGE; i++) {
        page.entries[page.count++] = inventory_[i];
    }
    send_packet(write_inventory(page, packet_, sizeof(packet_)));
}

void SyncEngine::handle_inventory(const InventoryPacket& packet, uint32_t now_ms) {
    stats_.inventory_pages++;
    // Every page is useful, even one meant for someone else: it's all valid state.
    for (int i = 0; i < packet.count; i++) {
        const InventoryEntry& entry = packet.entries[i];
        if (entry.kind == ENTRY_TOMBSTONE) {
            if (replica_->merge_tombstone(entry.id) == Result::Ok) {
                notify(SyncEvent::Tombstone, entry.id);
            }
        } else if (entry.kind == ENTRY_USER_EPOCH) {
            if (replica_->merge_user_epoch(uint16_t(entry.id), entry.value) == Result::Ok) {
                notify(SyncEvent::UserDeleted, entry.id);
            }
        } else if (entry.kind == ENTRY_SLOT) {
            if (!replica_->is_tombstoned(entry.id) && entry.value > local_version(entry.id)) {
                want(entry.id, entry.value, packet.sender);
            }
        }
    }
    if (fetch_.active && packet.sender == fetch_.peer && packet.page == fetch_.next_page) {
        fetch_.next_page++;
        fetch_.last_progress_ms = now_ms;
        fetch_.retries = 0;
        if (fetch_.next_page >= packet.pages) {
            fetch_.active = false;
            peers_[packet.sender].last_inventory_ms = now_ms;
        } else {
            request_page();
        }
    }
}

void SyncEngine::want(uint32_t key, uint32_t version, uint8_t source) {
    for (int i = 0; i < wanted_count_; i++) {
        if (wanted_[i].key == key) {
            if (version > wanted_[i].version) {
                wanted_[i] = {key, version, source};
            }
            return;
        }
    }
    if (wanted_count_ < MAX_WANTED) {
        wanted_[wanted_count_++] = {key, version, source};
    }
    // Queue full: dropped for now. The digest still differs, so a later round finds it again.
}

// ======================================================================================
// Receiving slots
// ======================================================================================

void SyncEngine::start_next_transfer(uint32_t now_ms) {
    while (wanted_count_ > 0) {
        Wanted next = wanted_[0];
        for (int i = 1; i < wanted_count_; i++) {
            wanted_[i - 1] = wanted_[i];
        }
        wanted_count_--;
        if (!replica_->is_tombstoned(next.key) && next.version > local_version(next.key)) {
            start_transfer(next.key, next.source, next.version, now_ms);
            send_request();
            return;
        }
    }
}

void SyncEngine::start_transfer(uint32_t key, uint8_t source, uint32_t version, uint32_t now_ms) {
    transfer_ = {};
    transfer_.active = true;
    transfer_.key = key;
    transfer_.source = source;
    transfer_.version = version;
    transfer_.last_progress_ms = now_ms;
}

void SyncEngine::send_request() {
    RequestPacket request;
    request.sender = replica_->device_id();
    request.target = transfer_.source;
    request.key = transfer_.key;
    request.version = transfer_.version;
    // Before the first chunk we don't know how many there are, so ask for all of them.
    uint32_t wanted = transfer_.total == 0 ? 0xFFFFFFFFu : all_chunks_mask(transfer_.total);
    request.missing_mask = wanted & ~transfer_.received_mask;
    send_packet(write_request(request, packet_, sizeof(packet_)));
}

void SyncEngine::handle_chunk(const ChunkPacket& chunk, uint32_t now_ms) {
    if (replica_->is_tombstoned(chunk.key) || chunk.version <= local_version(chunk.key)) {
        return;  // deleted, or we already have this or newer
    }
    if (transfer_.active && transfer_.key != chunk.key) {
        want(chunk.key, chunk.version, chunk.sender);  // busy: fetch it afterwards
        return;
    }
    if (transfer_.active && chunk.version < transfer_.version) {
        return;  // older than what we're assembling
    }
    if (!transfer_.active || chunk.version > transfer_.version) {
        // Overheard chunks of a slot we need: collect them too. Missing ones get requested on retry.
        start_transfer(chunk.key, chunk.sender, chunk.version, now_ms);
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
        stats_.crc_failures++;  // a damaged chunk got through; the next round starts over
        return;
    }
    // The header inside must match what the chunks claimed to carry.
    const uint8_t* in = assembly_;
    SlotHeader header = read_header(in);
    if (length < HEADER_BYTES || header.key.packed() != transfer_.key || header.version != transfer_.version) {
        stats_.packets_rejected++;
        return;
    }
    if (replica_->merge_slot(assembly_, length) == Result::Ok) {
        stats_.slots_applied++;
        notify(SyncEvent::SlotApplied, transfer_.key);
    }
}

void SyncEngine::check_timeouts(uint32_t now_ms) {
    if (fetch_.active && now_ms - fetch_.last_progress_ms >= RETRY_MS) {
        if (fetch_.retries >= MAX_RETRIES) {
            fetch_.active = false;  // peer unreachable; a later heartbeat restarts it
        } else {
            fetch_.retries++;
            fetch_.last_progress_ms = now_ms;
            request_page();
        }
    }
    if (!transfer_.active) {
        return;
    }
    if (replica_->is_tombstoned(transfer_.key) || local_version(transfer_.key) >= transfer_.version) {
        transfer_.active = false;  // deleted or received another way meanwhile
        return;
    }
    if (now_ms - transfer_.last_progress_ms < RETRY_MS) {
        return;
    }
    if (transfer_.retries >= MAX_RETRIES) {
        transfer_.active = false;
        stats_.transfers_abandoned++;
        return;
    }
    transfer_.retries++;
    transfer_.last_progress_ms = now_ms;
    send_request();
}

// ======================================================================================
// Sending slots
// ======================================================================================

void SyncEngine::handle_request(const RequestPacket& packet) {
    if (packet.target != replica_->device_id()) {
        return;
    }
    SlotHeader header;
    if (!replica_->find_slot(packet.key, header) || header.version < packet.version) {
        return;  // we don't have that version (saved) yet
    }
    // If we have something newer, we send that instead; the requester switches to it.
    for (int i = 0; i < MAX_PENDING_SENDS; i++) {
        if (sends_[i].mask != 0 && sends_[i].key == packet.key) {
            sends_[i].mask |= packet.missing_mask;
            return;
        }
    }
    for (int i = 0; i < MAX_PENDING_SENDS; i++) {
        if (sends_[i].mask == 0) {
            sends_[i] = {packet.key, packet.missing_mask};
            return;
        }
    }
    // All busy: the requester will ask again.
}

// Read a saved slot into tx_buffer_ unless that exact version is already there.
bool SyncEngine::prepare_outgoing(uint32_t key) {
    SlotHeader header;
    if (!replica_->find_slot(key, header)) {
        return false;
    }
    if (tx_version_ != 0 && tx_key_ == key && tx_version_ == header.version) {
        return true;
    }
    tx_length_ = replica_->read_slot(key, tx_buffer_, sizeof(tx_buffer_));
    if (tx_length_ == 0 || chunks_for(tx_length_) > MAX_CHUNKS) {
        tx_version_ = 0;
        return false;
    }
    tx_key_ = key;
    tx_version_ = header.version;
    tx_crc_ = crc32(tx_buffer_, tx_length_);
    return true;
}

void SyncEngine::send_pending_chunks() {
    int budget = CHUNKS_PER_INTERVAL;
    for (int step = 0; step < MAX_PENDING_SENDS && budget > 0; step++) {
        PendingSend& pending = sends_[(next_send_ + step) % MAX_PENDING_SENDS];
        if (pending.mask == 0) {
            continue;
        }
        if (!prepare_outgoing(pending.key)) {
            pending.mask = 0;  // deleted meanwhile
            continue;
        }
        int total = chunks_for(tx_length_);
        pending.mask &= all_chunks_mask(total);
        for (int index = 0; index < total && budget > 0; index++) {
            uint32_t bit = 1u << index;
            if ((pending.mask & bit) == 0) {
                continue;
            }
            size_t offset = size_t(index) * MAX_CHUNK_DATA;
            size_t remaining = tx_length_ - offset;
            ChunkPacket chunk;
            chunk.sender = replica_->device_id();
            chunk.key = tx_key_;
            chunk.version = tx_version_;
            chunk.index = uint8_t(index);
            chunk.total = uint8_t(total);
            chunk.slot_crc = tx_crc_;
            chunk.length = uint8_t(remaining < MAX_CHUNK_DATA ? remaining : MAX_CHUNK_DATA);
            chunk.data = tx_buffer_ + offset;
            send_packet(write_chunk(chunk, packet_, sizeof(packet_)));
            stats_.chunks_sent++;
            pending.mask &= ~bit;
            budget--;
        }
        if (pending.mask == 0) {
            next_send_ = (next_send_ + step + 1) % MAX_PENDING_SENDS;  // take turns between slots
        }
    }
}

}  // namespace meshstate
