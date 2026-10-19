#pragma once

// Sync protocol: packet formats and the engine that exchanges slots between nodes.

#include <cstddef>
#include <cstdint>

#include "core.h"

namespace construct {

// --------------------------------------------------------------------------------------
// Packets
// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------
// Sync engine
// --------------------------------------------------------------------------------------

// How sync works, in short:
//  1. Every node broadcasts a heartbeat with its version vector once a second.
//  2. A node that sees a newer version of some slot asks that peer for it (Request).
//  3. The peer broadcasts the encoded slot in chunks. Anyone missing that version may use them.
//  4. The receiver applies the slot only when every chunk arrived, the CRC matches,
//     the contents are valid, and the version is newer. Missing chunks are re-requested.
// The engine never touches hardware: it sends through a callback and is driven by tick(),
// so the same code runs in the laptop simulator and on the ESP32.

constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
constexpr uint32_t REQUEST_RETRY_MS = 300;  // no new chunk for this long: ask again
constexpr int MAX_REQUEST_RETRIES = 6;      // then give up; a later heartbeat restarts it
constexpr uint32_t CHUNK_INTERVAL_MS = 10;  // pacing, so the radio's send queue never overflows
constexpr int CHUNKS_PER_INTERVAL = 4;
constexpr uint32_t PEER_ONLINE_MS = 5000;   // heard from within this time = shown as online

struct SyncStats {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_rejected;  // malformed, or claimed to come from ourselves
    uint32_t chunks_sent;
    uint32_t slots_applied;
    uint32_t crc_failures;
    uint32_t transfers_abandoned;
};

struct PeerStatus {
    bool seen;
    uint32_t last_seen_ms;
    uint32_t versions[MAX_NODES];  // from the peer's last heartbeat
};

class SyncEngine {
public:
    using SendFunction = void (*)(void* context, const uint8_t* data, size_t length);
    using SlotChangedFunction = void (*)(void* context, int owner);

    // slot_changed may be null. It runs after a received slot is applied (e.g. to save it to flash).
    void begin(Node* node, const SlotCodec* codec, SendFunction send, SlotChangedFunction slot_changed,
               void* context, uint32_t now_ms);

    // Call often (every loop / simulator step). Sends heartbeats, retries, and queued chunks.
    void tick(uint32_t now_ms);

    // Call for every packet received from the radio.
    void on_packet(const uint8_t* data, size_t length, uint32_t now_ms);

    const SyncStats& stats() const { return stats_; }
    const PeerStatus& peer(int id) const { return peers_[id]; }
    bool peer_online(int id, uint32_t now_ms) const;
    bool transfer_active() const { return transfer_.active; }

private:
    struct IncomingTransfer {
        bool active;
        uint8_t slot;
        uint8_t source;  // who we ask for missing chunks
        uint32_t version;
        uint8_t total;  // 0 until the first chunk arrives
        uint32_t slot_crc;
        uint32_t received_mask;
        uint8_t last_chunk_length;
        uint32_t last_progress_ms;
        int retries;
    };

    void send_packet(size_t length);
    void send_heartbeat();
    void send_request();
    void start_transfer(int slot, int source, uint32_t version, uint32_t now_ms);
    void check_transfer(uint32_t now_ms);
    void finish_transfer();
    bool prepare_outgoing(int slot);
    void send_pending_chunks();

    void handle_heartbeat(const HeartbeatPacket& packet, uint32_t now_ms);
    void handle_request(const RequestPacket& packet);
    void handle_chunk(const ChunkPacket& packet, uint32_t now_ms);

    Node* node_ = nullptr;
    const SlotCodec* codec_ = nullptr;
    SendFunction send_ = nullptr;
    SlotChangedFunction slot_changed_ = nullptr;
    void* context_ = nullptr;

    uint32_t last_heartbeat_ms_ = 0;
    uint32_t last_chunk_ms_ = 0;
    SyncStats stats_ = {};
    PeerStatus peers_[MAX_NODES] = {};

    // Receiving: one slot at a time keeps memory fixed; the others wait for the next heartbeat.
    IncomingTransfer transfer_ = {};
    uint8_t assembly_[MAX_ENCODED_SLOT_BYTES];
    Slot scratch_;  // decode target, so a bad slot never touches the live one

    // Sending: chunks other nodes asked for, per slot, and a cached encoding of one slot.
    uint32_t send_mask_[MAX_NODES] = {};
    int next_send_slot_ = 0;
    int tx_slot_ = -1;
    uint32_t tx_version_ = 0;
    size_t tx_length_ = 0;
    uint32_t tx_crc_ = 0;
    uint8_t tx_buffer_[MAX_ENCODED_SLOT_BYTES];

    uint8_t packet_[MAX_PACKET_BYTES];
};

}  // namespace construct
