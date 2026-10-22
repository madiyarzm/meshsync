#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "meshstate.h"

namespace meshstate {

// ======================================================================================
// Storage: where encoded slots and the small "meta" record live.
// RAM in tests and the simulator (MemoryStore), flash on the ESP32.
// ======================================================================================

class SlotStore {
public:
    virtual ~SlotStore() = default;
    virtual int list(uint32_t* keys, int capacity) = 0;                    // packed keys of saved slots
    virtual size_t load(uint32_t key, uint8_t* out, size_t capacity) = 0;  // encoded slot; 0 if missing
    virtual bool save(uint32_t key, const uint8_t* data, size_t length) = 0;
    virtual bool erase(uint32_t key) = 0;
    virtual size_t load_meta(uint8_t* out, size_t capacity) = 0;
    virtual bool save_meta(const uint8_t* data, size_t length) = 0;
};

// Fixed-size RAM store. Large (CAPACITY x 5 KB): use it as a global or static.
template <int CAPACITY>
class MemoryStore : public SlotStore {
public:
    int list(uint32_t* keys, int capacity) override {
        int count = 0;
        for (int i = 0; i < CAPACITY && count < capacity; i++) {
            if (entries_[i].used) {
                keys[count++] = entries_[i].key;
            }
        }
        return count;
    }
    size_t load(uint32_t key, uint8_t* out, size_t capacity) override {
        Entry* entry = find(key);
        if (entry == nullptr || entry->length > capacity) {
            return 0;
        }
        std::memcpy(out, entry->data, entry->length);
        return entry->length;
    }
    bool save(uint32_t key, const uint8_t* data, size_t length) override {
        if (length > MAX_ENCODED_SLOT_BYTES) {
            return false;
        }
        Entry* entry = find(key);
        for (int i = 0; entry == nullptr && i < CAPACITY; i++) {
            if (!entries_[i].used) {
                entry = &entries_[i];
            }
        }
        if (entry == nullptr) {
            return false;  // full
        }
        entry->used = true;
        entry->key = key;
        entry->length = length;
        std::memcpy(entry->data, data, length);
        return true;
    }
    bool erase(uint32_t key) override {
        Entry* entry = find(key);
        if (entry != nullptr) {
            entry->used = false;
        }
        return true;
    }
    size_t load_meta(uint8_t* out, size_t capacity) override {
        if (meta_length_ > capacity) {
            return 0;
        }
        std::memcpy(out, meta_, meta_length_);
        return meta_length_;
    }
    bool save_meta(const uint8_t* data, size_t length) override {
        if (length > sizeof(meta_)) {
            return false;
        }
        std::memcpy(meta_, data, length);
        meta_length_ = length;
        return true;
    }
    void wipe() {  // simulate a blank board
        for (int i = 0; i < CAPACITY; i++) {
            entries_[i].used = false;
        }
        meta_length_ = 0;
    }

private:
    struct Entry {
        bool used;
        uint32_t key;
        size_t length;
        uint8_t data[MAX_ENCODED_SLOT_BYTES];
    };
    Entry* find(uint32_t key) {
        for (int i = 0; i < CAPACITY; i++) {
            if (entries_[i].used && entries_[i].key == key) {
                return &entries_[i];
            }
        }
        return nullptr;
    }
    Entry entries_[CAPACITY] = {};
    uint8_t meta_[2048] = {};
    size_t meta_length_ = 0;
};

// ======================================================================================
// Inventory: the replica's whole state as a sorted list. Peers compare a CRC of it
// (the digest) to know whether they differ, and page through it to see what.
// ======================================================================================

enum EntryKind : uint8_t {
    ENTRY_SLOT = 1,        // id = slot key, value = version
    ENTRY_TOMBSTONE = 2,   // id = slot key, value = 0
    ENTRY_USER_EPOCH = 3,  // id = user, value = deletion counter
};

struct InventoryEntry {
    uint8_t kind;
    uint32_t id;
    uint32_t value;
};
constexpr int MAX_INVENTORY = MAX_SLOTS + MAX_TOMBSTONES + MAX_USERS;
constexpr size_t INVENTORY_ENTRY_BYTES = 9;

struct ReplicaStats {
    uint32_t slots_merged;
    uint32_t refused_deleted;
    uint32_t refused_schema;
    uint32_t refused_invalid;
    uint32_t refused_store_full;
    uint32_t refused_overflow;
};

// ======================================================================================
// Replica: one device's copy of the mesh state.
//
// State = slots (highest version wins) + tombstones (grow-only set of deleted keys)
//       + user deletion counters (highest wins). All three only ever move "up", and a slot
//       that is dead stays dead, so merging is order-independent and idempotent.
//
// Rule that makes reboots safe: only versions saved to the store are ever shared
// (inventory and read_slot use the saved copy). After a reboot a device resumes from its
// saved copy, and every newer version it had was never seen by anyone.
// ======================================================================================

class Replica {
public:
    // Loads saved state and rebuilds the running totals. Large: keep it global or static.
    Result begin(uint8_t device_id, SlotStore* store);
    uint8_t device_id() const { return device_id_; }

    // --- writing our own slots -----------------------------------------------------
    // A new session gets a fresh session id (never reused, even after a reboot).
    Result open_session(PayloadType type, uint16_t user, SlotKey& key_out);
    // Reopen one of our own saved slots, or start it empty (the digit demo uses session 0).
    Result resume_session(SlotKey key, uint16_t user);
    Result add_digit(SlotKey key, int label, const uint8_t pixels[DIGIT_FEATURES]);
    Result add_gesture(SlotKey key, int label, const int32_t features[FACE_FEATURES]);
    Result add_gaze(SlotKey key, const int32_t features[FACE_FEATURES], int32_t target_x, int32_t target_y);
    Result close_session(SlotKey key);  // save, then stop writing to it
    Result flush();                     // save every open session with unsaved samples
    bool session_open(SlotKey key) const;
    uint32_t session_version(SlotKey key) const;  // includes unsaved samples

    // --- deleting --------------------------------------------------------------------
    Result delete_slot(uint32_t key);                // tombstone one slot
    Result delete_user(uint16_t user, int& removed);  // raise the user's deletion counter

    // --- merging what peers send ------------------------------------------------------
    Result merge_slot(const uint8_t* data, size_t length);
    Result merge_tombstone(uint32_t key);
    Result merge_user_epoch(uint16_t user, uint32_t epoch);

    // --- reading state ---------------------------------------------------------------
    int slot_count() const { return index_count_; }
    const SlotHeader& slot_header(int i) const { return index_[i]; }  // saved copies
    bool find_slot(uint32_t key, SlotHeader& out) const;
    bool is_tombstoned(uint32_t key) const;
    uint32_t user_epoch(uint16_t user) const;
    int tombstone_count() const { return tombstone_count_; }
    int user_count() const { return user_count_; }
    bool is_dead(const SlotHeader& header) const;
    size_t read_slot(uint32_t key, uint8_t* out, size_t capacity);  // saved encoded copy
    int inventory(InventoryEntry* out, int capacity) const;           // sorted by (kind, id)
    uint32_t digest() const;
    uint32_t changes() const { return changes_; }  // bumps on every change peers can see
    const ReplicaStats& stats() const { return stats_; }

    // --- running totals: exact sums over live slots, including unsaved samples ---------
    void set_active_user(uint16_t user);  // whose totals are kept separately ("own")
    uint16_t active_user() const { return active_user_; }
    const DigitsTotals& digits_totals() const { return digits_all_; }
    const GestureTotals& gesture_totals(PayloadType type, bool own_only) const;
    const GazeTotals& gaze_totals(bool own_only) const;

    // Recompute totals from scratch into the given buffers (tests compare with the running ones).
    Result recount(DigitsTotals& digits, GestureTotals gestures[2], GazeTotals& gaze);

private:
    struct OpenSession {
        bool used;
        bool dirty;  // has samples not yet saved
        SlotHeader header;
        AnyPayload payload;
    };

    Result add_sample(SlotKey key, const AnyPayload& sample);
    Result change_totals(const SlotHeader& header, const AnyPayload& payload, int sign);
    Result remove_slot(uint32_t key);
    Result save_session(OpenSession& session);
    Result save_meta();
    Result load_meta();
    OpenSession* find_open(uint32_t key);
    const OpenSession* find_open(uint32_t key) const;
    int find_index(uint32_t key) const;
    int live_count() const;
    int user_slot(uint16_t user) const;

    uint8_t device_id_ = 0;
    SlotStore* store_ = nullptr;
    uint16_t next_session_ = 1;  // 0 is reserved for the digit demo
    uint16_t active_user_ = 0;
    uint32_t changes_ = 0;
    ReplicaStats stats_ = {};

    SlotHeader index_[MAX_SLOTS];  // saved slots
    int index_count_ = 0;
    uint32_t tombstones_[MAX_TOMBSTONES];
    int tombstone_count_ = 0;
    uint16_t users_[MAX_USERS];
    uint32_t user_epochs_[MAX_USERS];
    int user_count_ = 0;
    OpenSession open_[MAX_OPEN_SESSIONS] = {};

    DigitsTotals digits_all_ = {};
    GestureTotals gesture_all_[2] = {};  // [0] blendshape, [1] landmark
    GestureTotals gesture_own_[2] = {};
    GazeTotals gaze_all_ = {};
    GazeTotals gaze_own_ = {};

    // Scratch space, so nothing large ever sits on the (small, on ESP32) stack.
    AnyPayload incoming_;
    AnyPayload previous_;
    AnyPayload sample_;
    uint8_t encoded_[MAX_ENCODED_SLOT_BYTES];
    uint8_t meta_[2048];

    // The digest is recomputed only when something changed.
    mutable InventoryEntry digest_entries_[MAX_INVENTORY];
    mutable uint32_t digest_ = 0;
    mutable uint32_t digest_at_changes_ = 0;
};

}  // namespace meshstate
