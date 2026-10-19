#pragma once

#include <cstddef>
#include <cstdint>

#include "codec.h"
#include "node.h"
#include "protocol.h"

namespace construct {

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
