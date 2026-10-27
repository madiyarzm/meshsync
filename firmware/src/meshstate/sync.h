#pragma once

#include <cstddef>
#include <cstdint>

#include "meshstate.h"
#include "replica.h"

namespace meshstate {

// ======================================================================================
// Packets. Every packet starts with [magic][type][sender] and fits in one ESP-NOW frame.
// ======================================================================================

constexpr size_t MAX_PACKET_BYTES = 250;  // ESP-NOW payload limit
constexpr uint8_t PACKET_MAGIC = 0xC8;    // digits-v1 used 0xC7, so old and new boards ignore each other

enum class PacketType : uint8_t {
    Invalid = 0,
    Heartbeat = 1,         // "I have N entries, and their digest is D"
    InventoryRequest = 2,  // "target: send me page P of your inventory"
    Inventory = 3,         // one page of entries (slots, tombstones, user deletion counters)
    Request = 4,           // "target: send me these chunks of slot K, version V"
    Chunk = 5,             // one piece of an encoded slot
};

struct HeartbeatPacket {
    uint8_t sender;
    uint8_t app;  // peers running a different app ignore each other
    uint16_t entries;
    uint32_t digest;
    uint8_t heard;  // bit i = device i was heard in the last PEER_ONLINE_MS (draws the mesh map)
};
constexpr size_t HEARTBEAT_BYTES = 11;

struct InventoryRequestPacket {
    uint8_t sender;
    uint8_t target;
    uint8_t page;
};
constexpr size_t INVENTORY_REQUEST_BYTES = 5;

// [magic][type][sender][page][pages][count] + entries + CRC32 of everything before it.
// The CRC matters: entries are applied directly (a tombstone deletes data), so one corrupted
// byte must never turn into a deletion. Other packets are covered by the slot CRC or are harmless.
constexpr size_t INVENTORY_HEADER_BYTES = 6;
constexpr size_t INVENTORY_CRC_BYTES = 4;
constexpr int ENTRIES_PER_PAGE =
    int((MAX_PACKET_BYTES - INVENTORY_HEADER_BYTES - INVENTORY_CRC_BYTES) / INVENTORY_ENTRY_BYTES);  // 26

struct InventoryPacket {
    uint8_t sender;
    uint8_t page;
    uint8_t pages;
    uint8_t count;
    InventoryEntry entries[ENTRIES_PER_PAGE];
};

struct RequestPacket {
    uint8_t sender;
    uint8_t target;
    uint32_t key;
    uint32_t version;
    uint32_t missing_mask;  // bit i set = chunk i still needed
};
constexpr size_t REQUEST_BYTES = 16;

constexpr size_t CHUNK_HEADER_BYTES = 18;
constexpr size_t MAX_CHUNK_DATA = MAX_PACKET_BYTES - CHUNK_HEADER_BYTES;  // 232
constexpr int MAX_CHUNKS = 32;  // the request's missing-chunk mask has 32 bits
static_assert(MAX_ENCODED_SLOT_BYTES <= MAX_CHUNKS * MAX_CHUNK_DATA, "largest slot must fit in 32 chunks");

struct ChunkPacket {
    uint8_t sender;
    uint32_t key;
    uint32_t version;
    uint8_t index;
    uint8_t total;
    uint32_t slot_crc;    // CRC32 of the whole encoded slot, checked after assembly
    uint8_t length;
    const uint8_t* data;  // points into the packet buffer
};

PacketType packet_type(const uint8_t* data, size_t length);
size_t write_heartbeat(const HeartbeatPacket& packet, uint8_t* out, size_t capacity);
bool read_heartbeat(const uint8_t* data, size_t length, HeartbeatPacket& out);
size_t write_inventory_request(const InventoryRequestPacket& packet, uint8_t* out, size_t capacity);
bool read_inventory_request(const uint8_t* data, size_t length, InventoryRequestPacket& out);
size_t write_inventory(const InventoryPacket& packet, uint8_t* out, size_t capacity);
bool read_inventory(const uint8_t* data, size_t length, InventoryPacket& out);
size_t write_request(const RequestPacket& packet, uint8_t* out, size_t capacity);
bool read_request(const uint8_t* data, size_t length, RequestPacket& out);
size_t write_chunk(const ChunkPacket& packet, uint8_t* out, size_t capacity);
bool read_chunk(const uint8_t* data, size_t length, ChunkPacket& out);

inline int chunks_for(size_t encoded_bytes) {
    return int((encoded_bytes + MAX_CHUNK_DATA - 1) / MAX_CHUNK_DATA);
}
inline uint32_t all_chunks_mask(int total) {
    return total >= 32 ? 0xFFFFFFFFu : ((1u << total) - 1u);
}

// ======================================================================================
// Sync engine
//
//  1. Every second each node broadcasts a heartbeat: entry count + digest of its inventory.
//  2. A node whose digest differs from a peer's pages through that peer's inventory.
//  3. Tombstones and user deletion counters in the inventory are applied right away.
//     Slots that are newer than ours are queued and fetched in chunks, one at a time.
//  4. A slot is applied only when every chunk arrived, the CRC matches, the contents
//     are valid, and it is newer. Missing chunks and pages are re-requested.
// No hardware access: sends through a callback and is driven by tick(), so the same
// code runs in the simulator and on the ESP32.
// ======================================================================================

constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
constexpr uint32_t RETRY_MS = 300;             // no progress for this long: ask again
constexpr int MAX_RETRIES = 6;                 // then give up; a later heartbeat restarts it
constexpr uint32_t INVENTORY_COOLDOWN_MS = 1500;  // don't re-read the same peer's inventory faster
constexpr uint32_t CHUNK_INTERVAL_MS = 10;     // pacing, so the radio's send queue never overflows
constexpr int CHUNKS_PER_INTERVAL = 4;
constexpr uint32_t PEER_ONLINE_MS = 5000;
constexpr int MAX_WANTED = 16;                 // slots queued for download
constexpr int MAX_PENDING_SENDS = 8;           // slots other nodes asked us for

enum class SyncEvent : uint8_t {
    SlotApplied,   // id = slot key
    Tombstone,     // id = slot key
    UserDeleted,   // id = user
};

struct SyncStats {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_rejected;
    uint32_t chunks_sent;
    uint32_t slots_applied;
    uint32_t crc_failures;
    uint32_t transfers_abandoned;
    uint32_t inventory_pages;
};

struct PeerStatus {
    bool seen;
    uint32_t last_seen_ms;
    uint32_t digest;
    uint16_t entries;
    uint32_t last_inventory_ms;
    uint8_t heard;  // which devices this peer hears (from its heartbeat)
};

class SyncEngine {
public:
    using SendFunction = void (*)(void* context, const uint8_t* data, size_t length);
    using EventFunction = void (*)(void* context, SyncEvent event, uint32_t id);

    // on_event may be null.
    void begin(Replica* replica, uint8_t app, SendFunction send, EventFunction on_event, void* context,
               uint32_t now_ms);
    void tick(uint32_t now_ms);
    void on_packet(const uint8_t* data, size_t length, uint32_t now_ms);

    const SyncStats& stats() const { return stats_; }
    const PeerStatus& peer(int id) const { return peers_[id]; }
    bool peer_online(int id, uint32_t now_ms) const;
    uint8_t heard_mask(uint32_t now_ms) const;  // bit i = peer i online
    bool syncing() const { return transfer_.active || fetch_.active || wanted_count_ > 0; }

private:
    struct InventoryFetch {
        bool active;
        uint8_t peer;
        uint8_t next_page;
        uint32_t last_progress_ms;
        int retries;
    };
    struct Wanted {
        uint32_t key;
        uint32_t version;
        uint8_t source;
    };
    struct IncomingTransfer {
        bool active;
        uint32_t key;
        uint8_t source;
        uint32_t version;
        uint8_t total;  // 0 until the first chunk arrives
        uint32_t slot_crc;
        uint32_t received_mask;
        uint8_t last_chunk_length;
        uint32_t last_progress_ms;
        int retries;
    };
    struct PendingSend {
        uint32_t key;
        uint32_t mask;
    };

    void send_packet(size_t length);
    void notify(SyncEvent event, uint32_t id);
    uint32_t local_version(uint32_t key) const;

    void handle_heartbeat(const HeartbeatPacket& packet, uint32_t now_ms);
    void handle_inventory_request(const InventoryRequestPacket& packet);
    void handle_inventory(const InventoryPacket& packet, uint32_t now_ms);
    void handle_request(const RequestPacket& packet);
    void handle_chunk(const ChunkPacket& packet, uint32_t now_ms);

    void request_page();
    void want(uint32_t key, uint32_t version, uint8_t source);
    void start_next_transfer(uint32_t now_ms);
    void start_transfer(uint32_t key, uint8_t source, uint32_t version, uint32_t now_ms);
    void send_request();
    void finish_transfer();
    void check_timeouts(uint32_t now_ms);
    bool prepare_outgoing(uint32_t key);
    void send_pending_chunks();

    Replica* replica_ = nullptr;
    uint8_t app_ = 0;
    SendFunction send_ = nullptr;
    EventFunction on_event_ = nullptr;
    void* context_ = nullptr;

    uint32_t last_heartbeat_ms_ = 0;
    uint32_t last_chunk_ms_ = 0;
    SyncStats stats_ = {};
    PeerStatus peers_[MAX_DEVICES] = {};

    InventoryFetch fetch_ = {};
    Wanted wanted_[MAX_WANTED] = {};
    int wanted_count_ = 0;
    IncomingTransfer transfer_ = {};
    uint8_t assembly_[MAX_CHUNKS * MAX_CHUNK_DATA];

    PendingSend sends_[MAX_PENDING_SENDS] = {};
    int next_send_ = 0;
    uint32_t tx_key_ = 0;
    uint32_t tx_version_ = 0;  // 0 = nothing cached
    size_t tx_length_ = 0;
    uint32_t tx_crc_ = 0;
    uint8_t tx_buffer_[MAX_ENCODED_SLOT_BYTES];

    InventoryEntry inventory_[MAX_INVENTORY];  // snapshot used to answer inventory requests
    InventoryPacket rx_inventory_;  // 250+ bytes each: members, not stack variables
    InventoryPacket tx_inventory_;
    uint8_t packet_[MAX_PACKET_BYTES];
};

}  // namespace meshstate
