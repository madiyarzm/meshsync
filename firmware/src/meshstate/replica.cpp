#include "replica.h"

namespace meshstate {

namespace {

constexpr uint32_t META_MAGIC = 0x314D534D;  // "MSM1"

int gesture_index(PayloadType type) {
    return type == PayloadType::GestureBlend ? 0 : 1;
}

bool entry_less(const InventoryEntry& a, const InventoryEntry& b) {
    return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
}

}  // namespace

// ======================================================================================
// Startup and persistence
// ======================================================================================

Result Replica::begin(uint8_t device_id, SlotStore* store) {
    if (device_id >= MAX_DEVICES || store == nullptr) {
        return Result::BadInput;
    }
    device_id_ = device_id;
    store_ = store;
    next_session_ = 1;
    active_user_ = 0;
    changes_ = 1;
    stats_ = {};
    index_count_ = 0;
    tombstone_count_ = 0;
    user_count_ = 0;
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        open_[i].used = false;
    }
    digits_all_.clear();
    for (int i = 0; i < 2; i++) {
        gesture_all_[i].clear();
        gesture_own_[i].clear();
    }
    gaze_all_.clear();
    gaze_own_.clear();
    digest_at_changes_ = 0;

    Result meta_result = load_meta();

    // Rebuild the index and totals from the saved slots, dropping anything dead or damaged.
    uint32_t keys[MAX_SLOTS * 2];
    int count = store_->list(keys, MAX_SLOTS * 2);
    for (int i = 0; i < count; i++) {
        size_t length = store_->load(keys[i], encoded_, sizeof(encoded_));
        SlotHeader header;
        if (length == 0 || decode_slot(encoded_, length, header, incoming_) != Result::Ok ||
            header.key.packed() != keys[i] || is_dead(header) || index_count_ >= MAX_SLOTS) {
            store_->erase(keys[i]);
            continue;
        }
        if (change_totals(header, incoming_, +1) != Result::Ok) {
            store_->erase(keys[i]);
            continue;
        }
        index_[index_count_++] = header;
        if (header.key.device == device_id_ && header.key.session >= next_session_) {
            next_session_ = uint16_t(header.key.session + 1);  // never reuse a session id
        }
    }
    return meta_result;
}

Result Replica::save_meta() {
    uint8_t* out = meta_;
    write_u32(out, META_MAGIC);
    write_u16(out, next_session_);
    write_u16(out, uint16_t(tombstone_count_));
    for (int i = 0; i < tombstone_count_; i++) {
        write_u32(out, tombstones_[i]);
    }
    write_u16(out, uint16_t(user_count_));
    for (int i = 0; i < user_count_; i++) {
        write_u16(out, users_[i]);
        write_u32(out, user_epochs_[i]);
    }
    size_t length = size_t(out - meta_);
    write_u32(out, crc32(meta_, length));
    return store_->save_meta(meta_, length + 4) ? Result::Ok : Result::StorageError;
}

Result Replica::load_meta() {
    size_t length = store_->load_meta(meta_, sizeof(meta_));
    if (length == 0) {
        return Result::Ok;  // a fresh device
    }
    if (length < 14 || crc32(meta_, length - 4) != [&] {
            const uint8_t* tail = meta_ + length - 4;
            return read_u32(tail);
        }()) {
        return Result::StorageError;  // damaged: start empty rather than trust it
    }
    const uint8_t* in = meta_;
    const uint8_t* end = meta_ + length - 4;
    if (read_u32(in) != META_MAGIC) {
        return Result::StorageError;
    }
    uint16_t next_session = read_u16(in);
    int tombstones = read_u16(in);
    if (tombstones > MAX_TOMBSTONES || in + 4 * tombstones + 2 > end) {
        return Result::StorageError;
    }
    for (int i = 0; i < tombstones; i++) {
        tombstones_[i] = read_u32(in);
    }
    int users = read_u16(in);
    if (users > MAX_USERS || in + 6 * users != end) {
        return Result::StorageError;
    }
    for (int i = 0; i < users; i++) {
        users_[i] = read_u16(in);
        user_epochs_[i] = read_u32(in);
    }
    next_session_ = next_session;
    tombstone_count_ = tombstones;
    user_count_ = users;
    return Result::Ok;
}

// ======================================================================================
// Lookups
// ======================================================================================

Replica::OpenSession* Replica::find_open(uint32_t key) {
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        if (open_[i].used && open_[i].header.key.packed() == key) {
            return &open_[i];
        }
    }
    return nullptr;
}

const Replica::OpenSession* Replica::find_open(uint32_t key) const {
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        if (open_[i].used && open_[i].header.key.packed() == key) {
            return &open_[i];
        }
    }
    return nullptr;
}

int Replica::find_index(uint32_t key) const {
    for (int i = 0; i < index_count_; i++) {
        if (index_[i].key.packed() == key) {
            return i;
        }
    }
    return -1;
}

bool Replica::find_slot(uint32_t key, SlotHeader& out) const {
    int i = find_index(key);
    if (i < 0) {
        return false;
    }
    out = index_[i];
    return true;
}

// Saved slots plus open sessions that haven't been saved yet (they hold a place).
int Replica::live_count() const {
    int count = index_count_;
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        if (open_[i].used && find_index(open_[i].header.key.packed()) < 0) {
            count++;
        }
    }
    return count;
}

bool Replica::is_tombstoned(uint32_t key) const {
    for (int i = 0; i < tombstone_count_; i++) {
        if (tombstones_[i] == key) {
            return true;
        }
    }
    return false;
}

int Replica::user_slot(uint16_t user) const {
    for (int i = 0; i < user_count_; i++) {
        if (users_[i] == user) {
            return i;
        }
    }
    return -1;
}

uint32_t Replica::user_epoch(uint16_t user) const {
    int i = user_slot(user);
    return i < 0 ? 0 : user_epochs_[i];
}

bool Replica::is_dead(const SlotHeader& header) const {
    if (is_tombstoned(header.key.packed())) {
        return true;
    }
    return header.user != 0 && header.user_epoch < user_epoch(header.user);
}

bool Replica::session_open(SlotKey key) const {
    return find_open(key.packed()) != nullptr;
}

uint32_t Replica::session_version(SlotKey key) const {
    const OpenSession* session = find_open(key.packed());
    return session == nullptr ? 0 : session->header.version;
}

// ======================================================================================
// Running totals
// ======================================================================================

Result Replica::change_totals(const SlotHeader& header, const AnyPayload& payload, int sign) {
    PayloadType type = PayloadType(header.key.type);
    bool own = active_user_ != 0 && header.user == active_user_;
    if (type == PayloadType::Digits) {
        return accumulate(digits_all_.values, payload.digits.values, DigitsPayload::VALUES, sign);
    }
    if (type == PayloadType::Gaze) {
        Result result = accumulate(gaze_all_.values, payload.gaze.values, GazePayload::VALUES, sign);
        if (result != Result::Ok || !own) {
            return result;
        }
        result = accumulate(gaze_own_.values, payload.gaze.values, GazePayload::VALUES, sign);
        if (result != Result::Ok) {
            accumulate(gaze_all_.values, payload.gaze.values, GazePayload::VALUES, -sign);
        }
        return result;
    }
    int g = gesture_index(type);
    Result result = accumulate(gesture_all_[g].values, payload.gesture.values, GesturePayload::VALUES, sign);
    if (result != Result::Ok || !own) {
        return result;
    }
    result = accumulate(gesture_own_[g].values, payload.gesture.values, GesturePayload::VALUES, sign);
    if (result != Result::Ok) {
        accumulate(gesture_all_[g].values, payload.gesture.values, GesturePayload::VALUES, -sign);
    }
    return result;
}

void Replica::set_active_user(uint16_t user) {
    active_user_ = user;
    for (int i = 0; i < 2; i++) {
        gesture_own_[i].clear();
    }
    gaze_own_.clear();
    if (user == 0) {
        return;
    }
    // Rebuild "own" totals from this user's slots (open sessions use their unsaved copy).
    for (int i = 0; i < index_count_; i++) {
        const SlotHeader& header = index_[i];
        if (header.user != user || find_open(header.key.packed()) != nullptr) {
            continue;
        }
        size_t length = store_->load(header.key.packed(), encoded_, sizeof(encoded_));
        SlotHeader loaded;
        if (length == 0 || decode_slot(encoded_, length, loaded, previous_) != Result::Ok) {
            continue;
        }
        PayloadType type = PayloadType(header.key.type);
        if (type == PayloadType::Gaze) {
            accumulate(gaze_own_.values, previous_.gaze.values, GazePayload::VALUES, +1);
        } else if (type != PayloadType::Digits) {
            accumulate(gesture_own_[gesture_index(type)].values, previous_.gesture.values, GesturePayload::VALUES, +1);
        }
    }
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        const OpenSession& session = open_[i];
        if (!session.used || session.header.user != user) {
            continue;
        }
        PayloadType type = PayloadType(session.header.key.type);
        if (type == PayloadType::Gaze) {
            accumulate(gaze_own_.values, session.payload.gaze.values, GazePayload::VALUES, +1);
        } else if (type != PayloadType::Digits) {
            accumulate(gesture_own_[gesture_index(type)].values, session.payload.gesture.values,
                       GesturePayload::VALUES, +1);
        }
    }
}

const GestureTotals& Replica::gesture_totals(PayloadType type, bool own_only) const {
    int g = gesture_index(type);
    return own_only ? gesture_own_[g] : gesture_all_[g];
}

const GazeTotals& Replica::gaze_totals(bool own_only) const {
    return own_only ? gaze_own_ : gaze_all_;
}

Result Replica::recount(DigitsTotals& digits, GestureTotals gestures[2], GazeTotals& gaze) {
    digits.clear();
    gestures[0].clear();
    gestures[1].clear();
    gaze.clear();
    for (int i = 0; i < index_count_; i++) {
        uint32_t key = index_[i].key.packed();
        const OpenSession* session = find_open(key);
        SlotHeader header;
        const AnyPayload* payload = nullptr;
        if (session != nullptr) {
            payload = &session->payload;
            header = session->header;
        } else {
            size_t length = store_->load(key, encoded_, sizeof(encoded_));
            if (length == 0 || decode_slot(encoded_, length, header, previous_) != Result::Ok) {
                return Result::StorageError;
            }
            payload = &previous_;
        }
        PayloadType type = PayloadType(header.key.type);
        if (type == PayloadType::Digits) {
            accumulate(digits.values, payload->digits.values, DigitsPayload::VALUES, +1);
        } else if (type == PayloadType::Gaze) {
            accumulate(gaze.values, payload->gaze.values, GazePayload::VALUES, +1);
        } else {
            accumulate(gestures[gesture_index(type)].values, payload->gesture.values, GesturePayload::VALUES, +1);
        }
    }
    // Open sessions that were never saved still count.
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        const OpenSession& session = open_[i];
        if (!session.used || find_index(session.header.key.packed()) >= 0) {
            continue;
        }
        PayloadType type = PayloadType(session.header.key.type);
        if (type == PayloadType::Digits) {
            accumulate(digits.values, session.payload.digits.values, DigitsPayload::VALUES, +1);
        } else if (type == PayloadType::Gaze) {
            accumulate(gaze.values, session.payload.gaze.values, GazePayload::VALUES, +1);
        } else {
            accumulate(gestures[gesture_index(type)].values, session.payload.gesture.values,
                       GesturePayload::VALUES, +1);
        }
    }
    return Result::Ok;
}

// ======================================================================================
// Writing our own slots
// ======================================================================================

Result Replica::open_session(PayloadType type, uint16_t user, SlotKey& key_out) {
    if (!payload_type_known(uint8_t(type)) || type == PayloadType::Digits || user == 0) {
        return Result::BadInput;  // digit slots use resume_session with session 0
    }
    OpenSession* session = nullptr;
    for (int i = 0; i < MAX_OPEN_SESSIONS && session == nullptr; i++) {
        if (!open_[i].used) {
            session = &open_[i];
        }
    }
    if (session == nullptr) {
        return Result::TooManyOpen;
    }
    if (live_count() >= MAX_SLOTS) {
        return Result::StoreFull;
    }
    if (next_session_ == 0xFFFF) {
        return Result::Overflow;
    }
    // Save the counter before using the id, so a reboot can never hand it out again.
    uint16_t session_id = next_session_++;
    if (save_meta() != Result::Ok) {
        next_session_--;
        return Result::StorageError;
    }
    session->used = true;
    session->dirty = false;
    session->header = {{device_id_, uint8_t(type), session_id}, schema_for(type), user, user_epoch(user), 0};
    clear_payload(type, session->payload);
    key_out = session->header.key;
    return Result::Ok;
}

Result Replica::resume_session(SlotKey key, uint16_t user) {
    if (key.device != device_id_) {
        return Result::NotOwner;
    }
    if (!payload_type_known(key.type)) {
        return Result::BadInput;
    }
    if (find_open(key.packed()) != nullptr) {
        return Result::Ok;
    }
    OpenSession* session = nullptr;
    for (int i = 0; i < MAX_OPEN_SESSIONS && session == nullptr; i++) {
        if (!open_[i].used) {
            session = &open_[i];
        }
    }
    if (session == nullptr) {
        return Result::TooManyOpen;
    }
    if (find_index(key.packed()) >= 0) {
        // Its saved copy is already in the totals; just load it to keep writing.
        size_t length = store_->load(key.packed(), encoded_, sizeof(encoded_));
        if (length == 0 || decode_slot(encoded_, length, session->header, session->payload) != Result::Ok) {
            return Result::StorageError;
        }
    } else {
        if (is_tombstoned(key.packed())) {
            return Result::Deleted;
        }
        if (live_count() >= MAX_SLOTS) {
            return Result::StoreFull;
        }
        PayloadType type = PayloadType(key.type);
        session->header = {key, schema_for(type), user, user_epoch(user), 0};
        clear_payload(type, session->payload);
    }
    session->used = true;
    session->dirty = false;
    return Result::Ok;
}

Result Replica::add_sample(SlotKey key, const AnyPayload& sample) {
    OpenSession* session = find_open(key.packed());
    if (session == nullptr) {
        return Result::UnknownSession;
    }
    PayloadType type = PayloadType(key.type);
    if (payload_samples(type, session->payload) >= MAX_SESSION_SAMPLES || session->header.version == UINT32_MAX) {
        return Result::Overflow;
    }
    // Teaching is merging a one-sample payload: same checked add, slot first, then totals.
    Result result = accumulate_payload(type, session->payload, sample, +1);
    if (result != Result::Ok) {
        return result;
    }
    result = change_totals(session->header, sample, +1);
    if (result != Result::Ok) {
        accumulate_payload(type, session->payload, sample, -1);
        return result;
    }
    session->header.version++;
    session->dirty = true;
    return Result::Ok;
}

Result Replica::add_digit(SlotKey key, int label, const uint8_t pixels[DIGIT_FEATURES]) {
    if (PayloadType(key.type) != PayloadType::Digits) {
        return Result::BadInput;
    }
    Result result = digit_sample(sample_.digits, label, pixels);
    return result == Result::Ok ? add_sample(key, sample_) : result;
}

Result Replica::add_gesture(SlotKey key, int label, const int32_t features[FACE_FEATURES]) {
    PayloadType type = PayloadType(key.type);
    if (type != PayloadType::GestureBlend && type != PayloadType::GestureLandmark) {
        return Result::BadInput;
    }
    Result result = gesture_sample(sample_.gesture, label, features);
    return result == Result::Ok ? add_sample(key, sample_) : result;
}

Result Replica::add_gaze(SlotKey key, const int32_t features[FACE_FEATURES], int32_t target_x, int32_t target_y) {
    if (PayloadType(key.type) != PayloadType::Gaze) {
        return Result::BadInput;
    }
    Result result = gaze_sample(sample_.gaze, features, target_x, target_y);
    return result == Result::Ok ? add_sample(key, sample_) : result;
}

Result Replica::save_session(OpenSession& session) {
    if (!session.dirty || session.header.version == 0) {
        return Result::Ok;
    }
    size_t length = encode_slot(session.header, session.payload, encoded_, sizeof(encoded_));
    uint32_t key = session.header.key.packed();
    if (length == 0 || !store_->save(key, encoded_, length)) {
        return Result::StorageError;
    }
    int i = find_index(key);
    if (i < 0) {
        if (index_count_ >= MAX_SLOTS) {
            return Result::StoreFull;  // can't happen: open sessions reserve their place
        }
        i = index_count_++;
    }
    index_[i] = session.header;
    session.dirty = false;
    changes_++;
    return Result::Ok;
}

Result Replica::flush() {
    Result first_error = Result::Ok;
    for (int i = 0; i < MAX_OPEN_SESSIONS; i++) {
        if (open_[i].used) {
            Result result = save_session(open_[i]);
            if (result != Result::Ok && first_error == Result::Ok) {
                first_error = result;
            }
        }
    }
    return first_error;
}

Result Replica::close_session(SlotKey key) {
    OpenSession* session = find_open(key.packed());
    if (session == nullptr) {
        return Result::UnknownSession;
    }
    Result result = save_session(*session);
    if (result == Result::Ok) {
        session->used = false;  // an empty session (no samples) just gives its place back
    }
    return result;
}

// ======================================================================================
// Removing slots (tombstones and user deletion)
// ======================================================================================

// Take a slot out of the totals, the store, the index, and any open session.
Result Replica::remove_slot(uint32_t key) {
    Result result = Result::Ok;
    OpenSession* session = find_open(key);
    int i = find_index(key);
    if (session != nullptr) {
        change_totals(session->header, session->payload, -1);  // the open copy is what's counted
        session->used = false;
    } else if (i >= 0) {
        size_t length = store_->load(key, encoded_, sizeof(encoded_));
        SlotHeader header;
        if (length == 0 || decode_slot(encoded_, length, header, previous_) != Result::Ok) {
            result = Result::StorageError;
        } else {
            change_totals(header, previous_, -1);
        }
    }
    if (i >= 0) {
        store_->erase(key);
        index_[i] = index_[index_count_ - 1];
        index_count_--;
    }
    return result;
}

Result Replica::merge_tombstone(uint32_t key) {
    if (is_tombstoned(key)) {
        return Result::Unchanged;
    }
    if (tombstone_count_ >= MAX_TOMBSTONES) {
        return Result::TombstonesFull;
    }
    tombstones_[tombstone_count_++] = key;
    if (save_meta() != Result::Ok) {
        tombstone_count_--;
        return Result::StorageError;
    }
    // Saved first: if we crash now, begin() drops the slot anyway because it is tombstoned.
    Result result = remove_slot(key);
    changes_++;
    return result;
}

Result Replica::delete_slot(uint32_t key) {
    return merge_tombstone(key);
}

Result Replica::merge_user_epoch(uint16_t user, uint32_t epoch) {
    if (user == 0) {
        return Result::BadInput;
    }
    int i = user_slot(user);
    if (i >= 0 && epoch <= user_epochs_[i]) {
        return Result::Unchanged;
    }
    bool new_user = i < 0;
    uint32_t old_epoch = 0;
    if (new_user) {
        if (user_count_ >= MAX_USERS) {
            return Result::UsersFull;
        }
        i = user_count_++;
        users_[i] = user;
    } else {
        old_epoch = user_epochs_[i];
    }
    user_epochs_[i] = epoch;
    if (save_meta() != Result::Ok) {
        if (new_user) {
            user_count_--;
        } else {
            user_epochs_[i] = old_epoch;
        }
        return Result::StorageError;
    }

    // Everything this user had before the new counter value is now dead, wherever it came from.
    Result result = Result::Ok;
    for (int s = 0; s < MAX_OPEN_SESSIONS; s++) {
        if (open_[s].used && open_[s].header.user == user && open_[s].header.user_epoch < epoch) {
            Result removed = remove_slot(open_[s].header.key.packed());
            if (removed != Result::Ok) {
                result = removed;
            }
        }
    }
    int k = 0;
    while (k < index_count_) {
        if (index_[k].user == user && index_[k].user_epoch < epoch) {
            Result removed = remove_slot(index_[k].key.packed());  // moves the last entry into k
            if (removed != Result::Ok) {
                result = removed;
            }
        } else {
            k++;
        }
    }
    changes_++;
    return result;
}

Result Replica::delete_user(uint16_t user, int& removed) {
    int before = live_count();
    Result result = merge_user_epoch(user, user_epoch(user) + 1);
    removed = before - live_count();
    return result;
}

// ======================================================================================
// Merging slots from peers
// ======================================================================================

Result Replica::merge_slot(const uint8_t* data, size_t length) {
    SlotHeader header;
    Result result = decode_slot(data, length, header, incoming_);
    if (result != Result::Ok) {
        if (result == Result::SchemaMismatch) {
            stats_.refused_schema++;
        } else {
            stats_.refused_invalid++;
        }
        return result;
    }
    uint32_t key = header.key.packed();
    if (is_dead(header)) {
        stats_.refused_deleted++;
        return Result::Deleted;
    }

    OpenSession* session = find_open(key);
    int i = find_index(key);
    uint32_t current = session != nullptr ? session->header.version : (i >= 0 ? index_[i].version : 0);
    bool exists = session != nullptr || i >= 0;
    if (exists && header.version <= current) {
        return Result::Unchanged;  // duplicate or stale: ignoring it is what makes merge idempotent
    }
    if (!exists && live_count() >= MAX_SLOTS) {
        stats_.refused_store_full++;
        return Result::StoreFull;
    }

    // Swap the old contribution for the new one in the running totals.
    SlotHeader previous_header = {};
    bool had_previous = false;
    if (session != nullptr) {
        // A peer has a newer copy of our own open session: only possible if we lost data.
        previous_header = session->header;
        previous_ = session->payload;
        had_previous = true;
    } else if (i >= 0) {
        size_t old_length = store_->load(key, encoded_, sizeof(encoded_));
        if (old_length == 0 || decode_slot(encoded_, old_length, previous_header, previous_) != Result::Ok) {
            return Result::StorageError;
        }
        had_previous = true;
    }
    if (had_previous) {
        change_totals(previous_header, previous_, -1);
    }
    result = change_totals(header, incoming_, +1);
    if (result != Result::Ok) {
        if (had_previous) {
            change_totals(previous_header, previous_, +1);
        }
        stats_.refused_overflow++;
        return result;
    }
    if (!store_->save(key, data, length)) {
        change_totals(header, incoming_, -1);
        if (had_previous) {
            change_totals(previous_header, previous_, +1);
        }
        return Result::StorageError;
    }
    if (session != nullptr) {
        session->used = false;  // stop writing to it; the newer copy wins
    }
    if (i < 0) {
        i = index_count_++;
    }
    index_[i] = header;
    if (header.key.device == device_id_ && header.key.session >= next_session_) {
        next_session_ = uint16_t(header.key.session + 1);  // a board that lost its flash relearns its counter
        save_meta();
    }
    stats_.slots_merged++;
    changes_++;
    return Result::Ok;
}

size_t Replica::read_slot(uint32_t key, uint8_t* out, size_t capacity) {
    if (find_index(key) < 0) {
        return 0;
    }
    return store_->load(key, out, capacity);
}

// ======================================================================================
// Inventory and digest
// ======================================================================================

int Replica::inventory(InventoryEntry* out, int capacity) const {
    int count = 0;
    for (int i = 0; i < index_count_ && count < capacity; i++) {
        out[count++] = {ENTRY_SLOT, index_[i].key.packed(), index_[i].version};
    }
    for (int i = 0; i < tombstone_count_ && count < capacity; i++) {
        out[count++] = {ENTRY_TOMBSTONE, tombstones_[i], 0};
    }
    for (int i = 0; i < user_count_ && count < capacity; i++) {
        out[count++] = {ENTRY_USER_EPOCH, users_[i], user_epochs_[i]};
    }
    // Insertion sort: at most a few hundred entries, and both sides must list them in the same order.
    for (int i = 1; i < count; i++) {
        InventoryEntry entry = out[i];
        int j = i - 1;
        while (j >= 0 && entry_less(entry, out[j])) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = entry;
    }
    return count;
}

uint32_t Replica::digest() const {
    if (digest_at_changes_ == changes_) {
        return digest_;
    }
    int count = inventory(digest_entries_, MAX_INVENTORY);
    uint32_t crc = 0;
    for (int i = 0; i < count; i++) {
        uint8_t bytes[INVENTORY_ENTRY_BYTES];
        uint8_t* out = bytes;
        write_u8(out, digest_entries_[i].kind);
        write_u32(out, digest_entries_[i].id);
        write_u32(out, digest_entries_[i].value);
        crc = crc32_update(crc, bytes, sizeof(bytes));
    }
    digest_ = crc;
    digest_at_changes_ = changes_;
    return digest_;
}

}  // namespace meshstate
