#include "face_app.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace meshstate;

namespace {

constexpr int MAX_ARGS = 20;  // "<id> ADD <session> <tx> <ty>" + 12 features = 17 tokens

const char* const TYPE_NAMES[3] = {"gest_blend", "gest_lmk", "gaze"};
const PayloadType TYPES[3] = {PayloadType::GestureBlend, PayloadType::GestureLandmark, PayloadType::Gaze};

int model_index(PayloadType type) {
    return type == PayloadType::GestureBlend ? 0 : (type == PayloadType::GestureLandmark ? 1 : 2);
}

bool parse_type(const char* text, PayloadType& type) {
    for (int i = 0; i < 3; i++) {
        if (std::strcmp(text, TYPE_NAMES[i]) == 0) {
            type = TYPES[i];
            return true;
        }
    }
    return false;
}

const char* type_name(uint8_t type) {
    switch (PayloadType(type)) {
        case PayloadType::Digits: return "digits";
        case PayloadType::GestureBlend: return "gest_blend";
        case PayloadType::GestureLandmark: return "gest_lmk";
        case PayloadType::Gaze: return "gaze";
    }
    return "unknown";
}

// Whole token must be an integer in [low, high]. No signs other than a leading '-'.
bool parse_int(const char* text, long low, long high, long& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    const char* p = text;
    bool negative = false;
    if (*p == '-') {
        negative = true;
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    long value = 0;
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (*p - '0');
        if (value > 1000000000L) {
            return false;  // far outside every range we accept
        }
    }
    if (negative) {
        value = -value;
    }
    if (value < low || value > high) {
        return false;
    }
    out = value;
    return true;
}

bool parse_features(char** args, int32_t features[FACE_FEATURES]) {
    for (int f = 0; f < FACE_FEATURES; f++) {
        long value = 0;
        if (!parse_int(args[f], -MAX_FEATURE_VALUE, MAX_FEATURE_VALUE, value)) {
            return false;
        }
        features[f] = int32_t(value);
    }
    return true;
}

// Splits on spaces, in place. Returns the number of tokens, or -1 if there are too many.
int split(char* line, char** tokens, int capacity) {
    int count = 0;
    char* p = line;
    while (*p != '\0') {
        while (*p == ' ') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        if (count == capacity) {
            return -1;  // tokens[0] is still set, so the caller can answer with the id
        }
        tokens[count++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
    }
    return count;
}

}  // namespace

void FaceApp::begin(char letter, uint8_t app, Replica* replica, const SyncEngine* sync, WriteLine write,
                    Micros micros, void* context) {
    letter_ = letter;
    app_ = app;
    replica_ = replica;
    sync_ = sync;
    write_ = write;
    micros_ = micros;
    context_ = context;
    for (int i = 0; i < 3; i++) {  // whatever was fitted before belongs to another replica
        fitted_local_[i] = 0;
        fitted_mesh_[i] = 0;
    }
    opened_count_ = 0;
    lambda_ = 1.0;
    last_label_ = -1;
    timing_idle_ = {};
    timing_sync_ = {};
    for (int i = 0; i < MAX_DEVICES; i++) {
        peers_[i] = {};
    }
}

void FaceApp::set_lock(Lock lock, Unlock unlock) {
    lock_ = lock;
    unlock_ = unlock;
}

void FaceApp::boot() {
    event("0 BOOT %c %d %u", letter_, FACE_PROTOCOL, unsigned(app_));
}

bool FaceApp::take_bench_request() {
    bool requested = bench_requested_;
    bench_requested_ = false;
    return requested;
}

bool FaceApp::led_on(uint32_t now_ms) const {
    return int32_t(led_until_ms_ - now_ms) > 0;
}

// --------------------------------------------------------------------------------------
// Output
// --------------------------------------------------------------------------------------

void FaceApp::write_line(char* buffer, size_t capacity, const char* format, va_list args) {
    int written = std::vsnprintf(buffer, capacity, format, args);
    if (written < 0) {
        return;
    }
    if (write_ != nullptr) {
        write_(context_, buffer);  // truncated at capacity if ever too long; never at current sizes
    }
}

void FaceApp::reply(uint16_t id, const char* format, ...) {
    char body[FACE_MAX_OUTPUT - 10];  // room for "65535 OK "
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    std::snprintf(reply_buffer_, sizeof(reply_buffer_), "%u OK %s", unsigned(id), body);
    if (write_ != nullptr) {
        write_(context_, reply_buffer_);
    }
}

void FaceApp::error(uint16_t id, const char* code) {
    std::snprintf(reply_buffer_, sizeof(reply_buffer_), "%u ERR %s", unsigned(id), code);
    if (write_ != nullptr) {
        write_(context_, reply_buffer_);
    }
}

void FaceApp::event(const char* format, ...) {
    va_list args;
    va_start(args, format);
    write_line(event_buffer_, sizeof(event_buffer_), format, args);
    va_end(args);
}

bool FaceApp::lock_state(bool wait) {
    return lock_ == nullptr || lock_(context_, wait);
}

void FaceApp::unlock_state() {
    if (unlock_ != nullptr) {
        unlock_(context_);
    }
}

// --------------------------------------------------------------------------------------
// Requests
// --------------------------------------------------------------------------------------

void FaceApp::handle_line(char* line, uint32_t now_ms) {
    uint32_t start_us = micros_ != nullptr ? micros_() : 0;
    if (line[0] == '#' || line[0] == '\0') {
        return;
    }
    if (std::strlen(line) > FACE_MAX_LINE) {
        error(0, "BAD_LINE");
        return;
    }
    char* tokens[MAX_ARGS];
    int count = split(line, tokens, MAX_ARGS);
    long id_value = 0;
    if (count == 0 || !parse_int(tokens[0], 1, 65535, id_value)) {
        error(0, "BAD_LINE");  // no usable id to answer to
        return;
    }
    uint16_t id = uint16_t(id_value);
    if (count < 2) {
        error(id, "BAD_LINE");  // too many tokens (count -1 keeps tokens[0]) or no command
        return;
    }
    const char* command = tokens[1];
    char** args = tokens + 2;
    int arg_count = count - 2;

    if (std::strcmp(command, "PREDICT") == 0) {
        // The live path: never waits for the mesh side (see refresh_model).
        cmd_predict(id, args, arg_count, now_ms);
        if (micros_ != nullptr) {
            uint32_t elapsed = micros_() - start_us;
            Timing& timing = sync_ != nullptr && sync_->syncing() ? timing_sync_ : timing_idle_;
            timing.count++;
            timing.total_us += elapsed;
            if (elapsed > timing.max_us) {
                timing.max_us = elapsed;
            }
        }
        return;
    }
    if (std::strcmp(command, "TIMING") == 0) {
        cmd_timing(id, args, arg_count);
        return;
    }
    if (std::strcmp(command, "BENCH") == 0) {
        bench_requested_ = true;
        reply(id, "BENCH");
        return;
    }

    // Everything else reads or changes the hub state.
    lock_state(true);
    if (std::strcmp(command, "HELLO") == 0) {
        close_opened_sessions();  // a new page connection
        reply(id, "HELLO %c %d %d", letter_, FACE_PROTOCOL, MAX_SLOTS);
    } else if (std::strcmp(command, "USER") == 0) {
        cmd_user(id, args, arg_count);
    } else if (std::strcmp(command, "NEW_USER") == 0) {
        cmd_new_user(id);
    } else if (std::strcmp(command, "COUNTS") == 0) {
        cmd_counts(id, args, arg_count);
    } else if (std::strcmp(command, "SESSION") == 0) {
        cmd_session(id, args, arg_count);
    } else if (std::strcmp(command, "ADD") == 0) {
        cmd_add(id, args, arg_count);
    } else if (std::strcmp(command, "CLOSE") == 0) {
        cmd_close(id, args, arg_count);
    } else if (std::strcmp(command, "LAMBDA") == 0) {
        cmd_lambda(id, args, arg_count);
    } else if (std::strcmp(command, "DELETE_USER") == 0) {
        cmd_delete_user(id, args, arg_count);
    } else if (std::strcmp(command, "STATUS") == 0) {
        cmd_status(id);
    } else if (std::strcmp(command, "SLOTS") == 0) {
        cmd_slots(id);
    } else {
        error(id, "UNKNOWN_COMMAND");
    }
    unlock_state();
}

bool FaceApp::find_session(uint16_t session, SlotKey& key) const {
    for (int i = 0; i < 3; i++) {
        SlotKey candidate = {replica_->device_id(), uint8_t(TYPES[i]), session};
        if (replica_->session_open(candidate)) {
            key = candidate;
            return true;
        }
    }
    return false;
}

void FaceApp::close_opened_sessions() {
    for (int i = 0; i < opened_count_; i++) {
        replica_->close_session(opened_[i]);  // UnknownSession if it was deleted: nothing to save
    }
    opened_count_ = 0;
}

// A number is taken if any saved slot belongs to it, or it was ever deleted (has a counter).
bool FaceApp::user_known(uint16_t user) const {
    for (int i = 0; i < replica_->slot_count(); i++) {
        if (replica_->slot_header(i).user == user) {
            return true;
        }
    }
    return replica_->user_epoch(user) > 0;
}

// Person numbers are (device + 1) * 1000 + n, so only this hub ever hands out its range.
// A number given to someone who never calibrated can be given again: there's no data behind it.
void FaceApp::cmd_new_user(uint16_t id) {
    close_opened_sessions();  // the previous person's samples are saved first, so they count as taken
    uint16_t base = uint16_t((replica_->device_id() + 1) * 1000);
    for (uint16_t n = 1; n < 1000; n++) {
        uint16_t user = uint16_t(base + n);
        if (!user_known(user)) {
            replica_->set_active_user(user);
            local_changes_++;
            last_label_ = -1;
            reply(id, "NEW_USER %u", unsigned(user));
            return;
        }
    }
    error(id, "USERS_FULL");
}

// Samples per gesture: the active person's own, and everyone else's.
void FaceApp::cmd_counts(uint16_t id, char** args, int count) {
    PayloadType type;
    if (count != 1 || !parse_type(args[0], type) || type == PayloadType::Gaze) {
        error(id, "BAD_LINE");
        return;
    }
    const GestureTotals& all = replica_->gesture_totals(type, false);
    const GestureTotals& own = replica_->gesture_totals(type, true);
    long mine[GESTURE_CLASSES];
    long others[GESTURE_CLASSES];
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        mine[c] = long(own.count(c));
        others[c] = long(all.count(c) - own.count(c));
    }
    reply(id, "COUNTS %s %ld %ld %ld %ld %ld %ld", TYPE_NAMES[model_index(type)], mine[0], mine[1], mine[2], others[0],
          others[1], others[2]);
}

void FaceApp::cmd_user(uint16_t id, char** args, int count) {
    long user = 0;
    if (count != 1 || !parse_int(args[0], 1, 65535, user)) {
        error(id, "BAD_LINE");
        return;
    }
    close_opened_sessions();
    replica_->set_active_user(uint16_t(user));
    local_changes_++;
    last_label_ = -1;
    reply(id, "USER %ld %lu", user, (unsigned long)replica_->user_epoch(uint16_t(user)));
}

void FaceApp::cmd_session(uint16_t id, char** args, int count) {
    PayloadType type;
    if (count != 1 || !parse_type(args[0], type)) {
        error(id, "BAD_LINE");
        return;
    }
    if (replica_->active_user() == 0) {
        error(id, "NO_USER");
        return;
    }
    SlotKey key;
    // Forget sessions that ended elsewhere (deleted by a user deletion from the mesh).
    int kept = 0;
    for (int i = 0; i < opened_count_; i++) {
        if (replica_->session_open(opened_[i])) {
            opened_[kept++] = opened_[i];
        }
    }
    opened_count_ = kept;
    Result result = replica_->open_session(type, replica_->active_user(), key);
    if (result != Result::Ok) {
        error(id, result_name(result));
        return;
    }
    if (opened_count_ < MAX_OPEN_SESSIONS) {
        opened_[opened_count_++] = key;
    }
    reply(id, "SESSION %u", unsigned(key.session));
}

void FaceApp::cmd_add(uint16_t id, char** args, int count) {
    long session = 0;
    if (count < 1 || !parse_int(args[0], 1, 65535, session)) {
        error(id, "BAD_LINE");
        return;
    }
    SlotKey key;
    if (!find_session(uint16_t(session), key)) {
        error(id, "UNKNOWN_SESSION");  // closed, deleted, or lost in a reboot
        return;
    }
    int32_t features[FACE_FEATURES];
    Result result;
    if (PayloadType(key.type) == PayloadType::Gaze) {
        long target_x = 0;
        long target_y = 0;
        if (count != 3 + FACE_FEATURES || !parse_int(args[1], 0, MAX_TARGET_VALUE, target_x) ||
            !parse_int(args[2], 0, MAX_TARGET_VALUE, target_y)) {
            error(id, "BAD_LINE");
            return;
        }
        if (!parse_features(args + 3, features)) {
            error(id, "BAD_FEATURES");
            return;
        }
        result = replica_->add_gaze(key, features, int32_t(target_x), int32_t(target_y));
    } else {
        long label = 0;
        if (count != 2 + FACE_FEATURES || !parse_int(args[1], 0, GESTURE_CLASSES - 1, label)) {
            error(id, "BAD_LINE");
            return;
        }
        if (!parse_features(args + 2, features)) {
            error(id, "BAD_FEATURES");
            return;
        }
        result = replica_->add_gesture(key, int(label), features);
    }
    if (result != Result::Ok) {
        error(id, result_name(result));
        return;
    }
    local_changes_++;
    reply(id, "ADD %lu", (unsigned long)replica_->session_version(key));
}

void FaceApp::cmd_close(uint16_t id, char** args, int count) {
    long session = 0;
    SlotKey key;
    if (count != 1 || !parse_int(args[0], 1, 65535, session)) {
        error(id, "BAD_LINE");
        return;
    }
    if (!find_session(uint16_t(session), key)) {
        error(id, "UNKNOWN_SESSION");
        return;
    }
    uint32_t version = replica_->session_version(key);
    Result result = replica_->close_session(key);
    for (int i = 0; i < opened_count_; i++) {
        if (opened_[i] == key) {
            opened_[i] = opened_[--opened_count_];
            break;
        }
    }
    if (result != Result::Ok) {
        error(id, result_name(result));
        return;
    }
    reply(id, "CLOSE %lu", (unsigned long)version);
}

// Refit only if the totals changed since the last fit. If the mesh side holds the state
// right now (merging, or saving to flash), keep predicting with the current model; only the
// very first fit waits for it.
void FaceApp::refresh_model(PayloadType type) {
    int index = model_index(type);
    uint32_t mesh = mesh_changes_;
    if (fitted_local_[index] == local_changes_ && fitted_mesh_[index] == mesh) {
        return;
    }
    bool have_model = fitted_local_[index] != 0;
    if (!lock_state(!have_model)) {
        return;  // busy: use the older model
    }
    if (type == PayloadType::Gaze) {
        gaze_model_.fit(replica_->gaze_totals(false), replica_->gaze_totals(true), lambda_, GAZE_RIDGE_ALPHA);
    } else {
        gesture_models_[index].fit(replica_->gesture_totals(type, false), replica_->gesture_totals(type, true),
                                   lambda_, GESTURE_VAR_FLOOR);
    }
    unlock_state();
    fitted_local_[index] = local_changes_;
    fitted_mesh_[index] = mesh;
}

void FaceApp::cmd_predict(uint16_t id, char** args, int count, uint32_t now_ms) {
    PayloadType type;
    int32_t features[FACE_FEATURES];
    if (count != 1 + FACE_FEATURES || !parse_type(args[0], type)) {
        error(id, "BAD_LINE");
        return;
    }
    if (!parse_features(args + 1, features)) {
        error(id, "BAD_FEATURES");
        return;
    }
    refresh_model(type);

    if (type == PayloadType::Gaze) {
        double x = 0;
        double y = 0;
        if (!gaze_model_.predict(features, x, y)) {
            error(id, "NOT_READY");
            return;
        }
        reply(id, "PREDICT gaze %ld %ld %d %.0f %.0f", std::lround(x * 1000), std::lround(y * 1000), gaze_cell(x, y),
              gaze_model_.own_samples, gaze_model_.other_samples);
        return;
    }

    const GestureModel& model = gesture_models_[model_index(type)];
    double scores[GESTURE_CLASSES];
    model.log_scores(features, scores);
    // Softmax over the classes that have data; classes without data get probability 0.
    int best = -1;
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        if (model.has_class[c] && (best < 0 || scores[c] > scores[best])) {
            best = c;
        }
    }
    if (best < 0) {
        error(id, "NOT_READY");
        return;
    }
    double probability[GESTURE_CLASSES];
    double total = 0;
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        probability[c] = model.has_class[c] ? std::exp(scores[c] - scores[best]) : 0.0;
        total += probability[c];
    }
    long p[GESTURE_CLASSES];
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        p[c] = std::lround(1000.0 * probability[c] / total);
    }
    // LED: flash once when a (confident) non-neutral gesture starts.
    if (best != last_label_ && best != 0 && p[best] >= 800) {
        led_until_ms_ = now_ms + 150;
    }
    last_label_ = best;
    reply(id, "PREDICT %s %d %ld %ld %ld %.0f %.0f", TYPE_NAMES[model_index(type)], best, p[0], p[1], p[2],
          model.own_samples, model.other_samples);
}

void FaceApp::cmd_lambda(uint16_t id, char** args, int count) {
    long value = 0;
    if (count != 1 || !parse_int(args[0], 0, 10000, value)) {
        error(id, "BAD_LINE");
        return;
    }
    lambda_ = value / 1000.0;
    local_changes_++;
    reply(id, "LAMBDA %ld", value);
}

void FaceApp::cmd_delete_user(uint16_t id, char** args, int count) {
    long user = 0;
    if (count != 1 || !parse_int(args[0], 1, 65535, user)) {
        error(id, "BAD_LINE");
        return;
    }
    int removed = 0;
    Result result = replica_->delete_user(uint16_t(user), removed);
    if (result != Result::Ok) {
        error(id, result_name(result));
        return;
    }
    local_changes_++;
    reply(id, "DELETE_USER %d %lu", removed, (unsigned long)replica_->user_epoch(uint16_t(user)));
}

void FaceApp::cmd_status(uint16_t id) {
    const ReplicaStats& stats = replica_->stats();
    uint32_t refused = stats.refused_deleted + stats.refused_schema + stats.refused_invalid +
                       stats.refused_store_full + stats.refused_overflow;
    uint16_t user = replica_->active_user();
    const SyncStats* sync = sync_ != nullptr ? &sync_->stats() : nullptr;
    reply(id,
          "STATUS letter=%c user=%u epoch=%lu lambda=%ld slots=%d max_slots=%d tombstones=%d users=%d "
          "digest=%08lx syncing=%d merged=%lu refused=%lu store_full=%lu crc_failures=%lu sent=%lu received=%lu",
          letter_, unsigned(user), (unsigned long)replica_->user_epoch(user), std::lround(lambda_ * 1000),
          replica_->slot_count(), MAX_SLOTS, replica_->tombstone_count(), replica_->user_count(),
          (unsigned long)replica_->digest(), sync != nullptr && sync_->syncing() ? 1 : 0,
          (unsigned long)stats.slots_merged, (unsigned long)refused, (unsigned long)stats.refused_store_full,
          (unsigned long)(sync != nullptr ? sync->crc_failures : 0), (unsigned long)(sync != nullptr ? sync->packets_sent : 0),
          (unsigned long)(sync != nullptr ? sync->packets_received : 0));
}

// Saved slots only: these are what the mesh sees. A face slot's version equals its sample count.
void FaceApp::cmd_slots(uint16_t id) {
    int count = 0;
    for (int i = 0; i < replica_->slot_count(); i++) {
        const SlotHeader& header = replica_->slot_header(i);
        std::snprintf(reply_buffer_, sizeof(reply_buffer_), "%u SLOT %08lx %c %s %u %u %lu", unsigned(id),
                      (unsigned long)header.key.packed(), 'A' + header.key.device, type_name(header.key.type),
                      unsigned(header.key.session), unsigned(header.user), (unsigned long)header.version);
        if (write_ != nullptr) {
            write_(context_, reply_buffer_);
        }
        count++;
    }
    reply(id, "SLOTS %d", count);
}

void FaceApp::cmd_timing(uint16_t id, char** args, int count) {
    if (count == 1 && std::strcmp(args[0], "RESET") == 0) {
        timing_idle_ = {};
        timing_sync_ = {};
    } else if (count != 0) {
        error(id, "BAD_LINE");
        return;
    }
    const Timing& idle = timing_idle_;
    const Timing& busy = timing_sync_;
    reply(id, "TIMING idle=%lu,%lu,%lu sync=%lu,%lu,%lu", (unsigned long)idle.count,
          (unsigned long)(idle.count ? idle.total_us / idle.count : 0), (unsigned long)idle.max_us,
          (unsigned long)busy.count, (unsigned long)(busy.count ? busy.total_us / busy.count : 0),
          (unsigned long)busy.max_us);
}

// --------------------------------------------------------------------------------------
// Mesh side
// --------------------------------------------------------------------------------------

void FaceApp::on_sync_event(SyncEvent sync_event, uint32_t id) {
    mesh_changes_ = mesh_changes_ + 1;
    if (sync_event == SyncEvent::SlotApplied) {
        event("0 EVENT SLOT %08lx", (unsigned long)id);
    } else if (sync_event == SyncEvent::Tombstone) {
        event("0 EVENT TOMBSTONE %08lx", (unsigned long)id);
    } else {
        event("0 EVENT USER_DELETED %lu %lu", (unsigned long)id,
              (unsigned long)replica_->user_epoch(uint16_t(id)));
    }
}

// Every packet in radio range, including ones addressed to other boards (ESP-NOW is broadcast).
void FaceApp::on_radio_packet(const uint8_t* data, size_t length, int rssi, uint32_t now_ms) {
    if (data == nullptr || length < 3 || data[0] != PACKET_MAGIC || data[2] >= MAX_DEVICES ||
        data[2] == replica_->device_id()) {
        return;  // not ours to count (another protocol, garbage, or an echo)
    }
    PeerTraffic& peer = peers_[data[2]];
    peer.heard = true;
    peer.last_ms = now_ms;
    peer.packets++;
    peer.bytes += uint32_t(length);
    if (data[1] < 6) {
        peer.by_type[data[1]]++;
    }
    if (rssi != 0) {
        peer.rssi_sum += rssi;
        peer.rssi_samples++;
        peer.last_rssi = rssi;
    }
    HeartbeatPacket heartbeat;
    if (packet_type(data, length) == PacketType::Heartbeat && read_heartbeat(data, length, heartbeat)) {
        peer.app = heartbeat.app;
    }
}

void FaceApp::on_radio_send(const uint8_t* data, size_t length) {
    (void)data;
    tx_packets_++;
    tx_bytes_ += uint32_t(length);
}

void FaceApp::tick(uint32_t now_ms) {
    if (now_ms - last_report_ms_ < FACE_REPORT_MS) {
        return;
    }
    last_report_ms_ = now_ms;
    event("0 EVENT MESH %08lx %d %d %d %d %lu %lu %02x", (unsigned long)replica_->digest(),
          replica_->slot_count() + replica_->tombstone_count() + replica_->user_count(), replica_->slot_count(),
          replica_->tombstone_count(), sync_ != nullptr && sync_->syncing() ? 1 : 0, (unsigned long)tx_packets_,
          (unsigned long)tx_bytes_, unsigned(sync_ != nullptr ? sync_->heard_mask(now_ms) : 0));
    tx_packets_ = 0;
    tx_bytes_ = 0;
    for (int id = 0; id < MAX_DEVICES; id++) {
        PeerTraffic& peer = peers_[id];
        if (!peer.heard || now_ms - peer.last_ms > FACE_PEER_FORGET_MS) {
            continue;
        }
        int rssi = peer.rssi_samples > 0 ? peer.rssi_sum / peer.rssi_samples : peer.last_rssi;
        bool online = sync_ != nullptr && sync_->peer_online(id, now_ms);
        uint32_t digest = sync_ != nullptr ? sync_->peer(id).digest : 0;
        unsigned entries = sync_ != nullptr ? sync_->peer(id).entries : 0;
        unsigned heard = sync_ != nullptr ? sync_->peer(id).heard : 0;
        event("0 EVENT PEER %c %u %d %lu %d %lu %lu %lu %lu %lu %lu %08lx %u %02x", 'A' + id, unsigned(peer.app),
              online ? 1 : 0, (unsigned long)(now_ms - peer.last_ms), rssi, (unsigned long)peer.packets,
              (unsigned long)peer.bytes, (unsigned long)peer.by_type[1],
              (unsigned long)(peer.by_type[2] + peer.by_type[3]), (unsigned long)peer.by_type[4],
              (unsigned long)peer.by_type[5], (unsigned long)digest, entries, heard);
        peer.packets = 0;
        peer.bytes = 0;
        peer.rssi_sum = 0;
        peer.rssi_samples = 0;
        for (int t = 0; t < 6; t++) {
            peer.by_type[t] = 0;
        }
    }
}
