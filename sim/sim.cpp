// Laptop simulator for the sync protocol: many virtual hubs in one process, connected by a
// fake radio that drops, delays, reorders, duplicates, and corrupts packets, with switchable
// partitions. All randomness comes from one seed, so any run (and any failure) can be replayed.
//
//   .pio/build/native/program                 one verbose run (seed 1, 6 hubs)
//   .pio/build/native/program 42 8            one verbose run with seed 42 and 8 hubs
//   .pio/build/native/program --seeds 500     automated test: convergence + no resurrection
//   .pio/build/native/program --capacity 50   more people than MAX_SLOTS: refusals, no corruption
//   .pio/build/native/program --hops 50       relays in a line: time per hop, broadcast savings
//
// Exit code 1 if any check fails.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "models.h"
#include "replica.h"
#include "sync.h"

using namespace meshstate;

namespace sim {

// Small deterministic random generator (xorshift64*). Same numbers on every machine.
class Random {
public:
    void seed(uint64_t value) { state_ = value * 2654435761u + 1; }
    uint32_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return uint32_t((state_ * 2685821657736338717ull) >> 32);
    }
    uint32_t below(uint32_t limit) { return limit == 0 ? 0 : next() % limit; }
    bool chance(double probability) { return next() < probability * 4294967296.0; }

private:
    uint64_t state_ = 1;
};

struct FaultConfig {
    double drop_rate;       // packet never arrives
    double duplicate_rate;  // packet arrives twice
    double corrupt_rate;    // one random byte is flipped
    uint32_t min_delay_ms;  // random delay per packet; different delays reorder packets
    uint32_t max_delay_ms;
};

struct RadioStats {
    uint32_t sent;  // one per receiver, since a broadcast reaches each hub separately
    uint32_t delivered;
    uint32_t dropped;
    uint32_t blocked;  // no link between the two hubs right now
    uint32_t duplicated;
    uint32_t corrupted;
    uint32_t overflowed;  // in-flight queue full (should stay 0)
};

constexpr uint8_t SIM_APP = 2;
constexpr uint32_t FLUSH_INTERVAL_MS = 2000;  // same as the firmware: save open sessions every 2 s
constexpr int MAX_TRACKED_USERS = 256;

class World {
public:
    static constexpr uint32_t STEP_MS = 5;

    bool reset(uint64_t seed, int num_hubs, const FaultConfig& faults);

    // Who can hear whom. set_groups: hubs hear their own group. set_line: only neighbors.
    void set_groups(const int group_of_hub[]);
    void set_line();
    void heal();
    void set_link(int a, int b, bool on);
    bool link(int a, int b) const { return link_[a][b]; }
    void set_faults(const FaultConfig& faults) { faults_ = faults; }
    void reboot(int hub);  // power cut: unsaved samples are lost, saved state reloads
    void advance(uint32_t duration_ms);

    // People calibrating: one random sample for whoever sits at this hub (a new user if nobody).
    void teach_random(int hub);
    // If a person's data is deleted while they sit at a hub, seat a new person right away
    // (keeps the automated runs busy). The playground turns this off.
    bool reseat_after_delete = true;
    void finish_user(int hub);  // that person leaves; their sessions are closed (saved)
    void finish_everyone();
    uint16_t user_at(int hub) const { return calibration_[hub].active ? calibration_[hub].user : 0; }
    uint16_t last_user_at(int hub) const { return calibration_[hub].user; }

    // Deletions, recorded in the world's own ground truth (not trusted from the hubs).
    bool delete_random_slot(int hub);
    void delete_user(int hub, uint16_t user);

    bool converged();
    // Number of slots, on any hub, that the ground truth says must be gone.
    int resurrected_slots();
    // Number of (hub, slot) pairs missing although nobody deleted the slot: wrongly lost data.
    int wrongly_lost_slots();
    bool totals_consistent();  // every hub's running totals equal a fresh recount

    uint32_t now_ms() const { return now_ms_; }
    Random& random() { return random_; }
    const RadioStats& radio_stats() const { return radio_; }
    Replica& replica(int hub) { return replicas_[hub]; }
    const SyncEngine& engine(int hub) const { return engines_[hub]; }
    int num_hubs() const { return num_hubs_; }
    uint16_t users_created() const { return uint16_t(next_user_ - 1); }

    // For the visual playground: packets in the air, and recent sync events.
    struct PacketView {
        uint8_t from;
        uint8_t to;
        uint8_t type;
        uint32_t sent_ms;
        uint32_t deliver_ms;
    };
    int packets_in_flight(PacketView* out, int capacity) const;
    struct EventView {
        uint32_t time_ms;
        uint8_t hub;
        uint8_t kind;  // SyncEvent
        uint32_t id;
    };
    static constexpr int EVENT_LOG = 64;
    int event_count() const { return event_count_; }
    const EventView& event(int i) const { return events_[(event_count_ - 1 - i) % EVENT_LOG]; }  // 0 = newest

private:
    struct InFlight {
        bool used;
        uint8_t to;
        uint8_t from;
        uint8_t length;
        uint32_t sent_at_ms;
        uint32_t deliver_at_ms;
        uint8_t data[MAX_PACKET_BYTES];
    };
    struct HubContext {
        World* world;
        int hub;
    };
    struct Calibration {
        bool active;
        uint16_t user;
        SlotKey gesture;
        SlotKey gaze;
    };
    static constexpr int MAX_IN_FLIGHT = 8192;

    static void send_from_hub(void* context, const uint8_t* data, size_t length);
    static void on_event(void* context, SyncEvent event, uint32_t id);
    void broadcast(int from, const uint8_t* data, size_t length);
    void queue_packet(int from, int to, const uint8_t* data, size_t length);
    void deliver_due_packets();
    bool truly_dead(const SlotHeader& header) const;

    int num_hubs_ = 0;
    uint32_t now_ms_ = 0;
    uint32_t last_flush_ms_ = 0;
    uint16_t next_user_ = 1;
    FaultConfig faults_ = {};
    Random random_;
    RadioStats radio_ = {};
    bool link_[MAX_DEVICES][MAX_DEVICES] = {};
    Calibration calibration_[MAX_DEVICES] = {};

    // Ground truth of every deletion anyone ever made.
    uint32_t deleted_keys_[MAX_TOMBSTONES] = {};
    int deleted_key_count_ = 0;
    uint32_t deleted_epoch_[MAX_TRACKED_USERS] = {};
    // Ground truth of every slot that ever got a sample, with its user and creation counter.
    static constexpr int MAX_CREATED = 512;
    SlotHeader created_[MAX_CREATED] = {};
    int created_count_ = 0;
    void record_created(const Replica& replica, SlotKey key);

    EventView events_[EVENT_LOG] = {};
    int event_count_ = 0;

    MemoryStore<MAX_SLOTS> stores_[MAX_DEVICES];
    Replica replicas_[MAX_DEVICES];
    SyncEngine engines_[MAX_DEVICES];
    HubContext contexts_[MAX_DEVICES];
    InFlight in_flight_[MAX_IN_FLIGHT];
};

bool World::reset(uint64_t seed, int num_hubs, const FaultConfig& faults) {
    if (num_hubs < 1 || num_hubs > MAX_DEVICES) {
        return false;
    }
    num_hubs_ = num_hubs;
    now_ms_ = 0;
    last_flush_ms_ = 0;
    next_user_ = 1;
    faults_ = faults;
    random_.seed(seed);
    radio_ = {};
    deleted_key_count_ = 0;
    created_count_ = 0;
    event_count_ = 0;
    for (int u = 0; u < MAX_TRACKED_USERS; u++) {
        deleted_epoch_[u] = 0;
    }
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        in_flight_[i].used = false;
    }
    for (int hub = 0; hub < num_hubs_; hub++) {
        calibration_[hub] = {};
        stores_[hub].wipe();
        replicas_[hub].begin(uint8_t(hub), &stores_[hub]);
        contexts_[hub] = {this, hub};
        engines_[hub].begin(&replicas_[hub], SIM_APP, &World::send_from_hub, &World::on_event, &contexts_[hub],
                            now_ms_);
    }
    heal();
    return true;
}

void World::set_link(int a, int b, bool on) {
    if (a >= 0 && b >= 0 && a < num_hubs_ && b < num_hubs_ && a != b) {
        link_[a][b] = on;
        link_[b][a] = on;
    }
}

void World::reboot(int hub) {
    if (hub < 0 || hub >= num_hubs_) {
        return;
    }
    calibration_[hub].active = false;  // the open sessions were in RAM
    replicas_[hub].begin(uint8_t(hub), &stores_[hub]);
    engines_[hub].begin(&replicas_[hub], SIM_APP, &World::send_from_hub, &World::on_event, &contexts_[hub], now_ms_);
}

void World::on_event(void* context, SyncEvent event, uint32_t id) {
    HubContext* hub = static_cast<HubContext*>(context);
    World* world = hub->world;
    world->events_[world->event_count_ % EVENT_LOG] = {world->now_ms_, uint8_t(hub->hub), uint8_t(event), id};
    world->event_count_++;
}

int World::packets_in_flight(PacketView* out, int capacity) const {
    int count = 0;
    for (int i = 0; i < MAX_IN_FLIGHT && count < capacity; i++) {
        const InFlight& packet = in_flight_[i];
        if (packet.used) {
            out[count++] = {packet.from, packet.to, packet.data[1], packet.sent_at_ms, packet.deliver_at_ms};
        }
    }
    return count;
}

void World::set_groups(const int group_of_hub[]) {
    for (int a = 0; a < num_hubs_; a++) {
        for (int b = 0; b < num_hubs_; b++) {
            link_[a][b] = group_of_hub[a] == group_of_hub[b];
        }
    }
}

void World::set_line() {
    for (int a = 0; a < num_hubs_; a++) {
        for (int b = 0; b < num_hubs_; b++) {
            link_[a][b] = a - b == 1 || b - a == 1;
        }
    }
}

void World::heal() {
    for (int a = 0; a < num_hubs_; a++) {
        for (int b = 0; b < num_hubs_; b++) {
            link_[a][b] = true;
        }
    }
}

void World::send_from_hub(void* context, const uint8_t* data, size_t length) {
    HubContext* hub = static_cast<HubContext*>(context);
    hub->world->broadcast(hub->hub, data, length);
}

void World::broadcast(int from, const uint8_t* data, size_t length) {
    for (int to = 0; to < num_hubs_; to++) {
        if (to == from) {
            continue;
        }
        radio_.sent++;
        if (random_.chance(faults_.drop_rate)) {
            radio_.dropped++;
            continue;
        }
        queue_packet(from, to, data, length);
        if (random_.chance(faults_.duplicate_rate)) {
            radio_.duplicated++;
            queue_packet(from, to, data, length);
        }
    }
}

void World::queue_packet(int from, int to, const uint8_t* data, size_t length) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        InFlight& packet = in_flight_[i];
        if (packet.used) {
            continue;
        }
        packet.used = true;
        packet.from = uint8_t(from);
        packet.to = uint8_t(to);
        packet.length = uint8_t(length);
        uint32_t spread = faults_.max_delay_ms - faults_.min_delay_ms + 1;
        packet.sent_at_ms = now_ms_;
        packet.deliver_at_ms = now_ms_ + faults_.min_delay_ms + random_.below(spread);
        std::memcpy(packet.data, data, length);
        if (random_.chance(faults_.corrupt_rate)) {
            radio_.corrupted++;
            packet.data[random_.below(uint32_t(length))] ^= uint8_t(1 + random_.below(255));
        }
        return;
    }
    radio_.overflowed++;
}

void World::deliver_due_packets() {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        InFlight& packet = in_flight_[i];
        if (!packet.used || packet.deliver_at_ms > now_ms_) {
            continue;
        }
        packet.used = false;
        // Checked on arrival, so packets in the air when a link drops are lost too.
        if (!link_[packet.from][packet.to]) {
            radio_.blocked++;
            continue;
        }
        radio_.delivered++;
        engines_[packet.to].on_packet(packet.data, packet.length, now_ms_);
    }
}

void World::advance(uint32_t duration_ms) {
    uint32_t end_ms = now_ms_ + duration_ms;
    while (now_ms_ < end_ms) {
        now_ms_ += STEP_MS;
        deliver_due_packets();
        if (now_ms_ - last_flush_ms_ >= FLUSH_INTERVAL_MS) {
            for (int hub = 0; hub < num_hubs_; hub++) {
                replicas_[hub].flush();
            }
            last_flush_ms_ = now_ms_;
        }
        for (int hub = 0; hub < num_hubs_; hub++) {
            engines_[hub].tick(now_ms_);
        }
    }
}

void World::teach_random(int hub) {
    Calibration& calibration = calibration_[hub];
    Replica& replica = replicas_[hub];
    if (!calibration.active) {
        if (next_user_ >= MAX_TRACKED_USERS) {
            return;
        }
        calibration.user = next_user_;
        if (replica.open_session(PayloadType::GestureBlend, calibration.user, calibration.gesture) != Result::Ok) {
            return;  // store full: this hub can't take a new person
        }
        if (replica.open_session(PayloadType::Gaze, calibration.user, calibration.gaze) != Result::Ok) {
            replica.close_session(calibration.gesture);
            return;
        }
        next_user_++;  // only people who actually got sessions get an id
        calibration.active = true;
    }
    int32_t features[FACE_FEATURES];
    for (int f = 0; f < FACE_FEATURES; f++) {
        features[f] = int32_t(random_.below(2001)) - 1000;
    }
    Result gesture = replica.add_gesture(calibration.gesture, int(random_.below(GESTURE_CLASSES)), features);
    Result gaze = replica.add_gaze(calibration.gaze, features, int32_t(random_.below(1001)), int32_t(random_.below(1001)));
    if (gesture == Result::Ok) {
        record_created(replica, calibration.gesture);
    }
    if (gaze == Result::Ok) {
        record_created(replica, calibration.gaze);
    }
    if (gesture == Result::UnknownSession && gaze == Result::UnknownSession) {
        calibration.active = false;  // this person's data was deleted while they sat here
        if (reseat_after_delete) {
            teach_random(hub);
        }
    }
}

void World::finish_user(int hub) {
    Calibration& calibration = calibration_[hub];
    if (calibration.active) {
        replicas_[hub].close_session(calibration.gesture);
        replicas_[hub].close_session(calibration.gaze);
        calibration.active = false;
    }
}

void World::finish_everyone() {
    for (int hub = 0; hub < num_hubs_; hub++) {
        finish_user(hub);
    }
}

void World::record_created(const Replica& replica, SlotKey key) {
    for (int i = 0; i < created_count_; i++) {
        if (created_[i].key == key) {
            return;
        }
    }
    if (created_count_ < MAX_CREATED) {
        SlotHeader header = {};
        header.key = key;
        header.user = calibration_[key.device].user;
        header.user_epoch = replica.user_epoch(header.user);  // what the slot was created with
        created_[created_count_++] = header;
    }
}

int World::wrongly_lost_slots() {
    int lost = 0;
    for (int i = 0; i < created_count_; i++) {
        if (truly_dead(created_[i])) {
            continue;  // deleted on purpose
        }
        for (int hub = 0; hub < num_hubs_; hub++) {
            SlotHeader header;
            if (!replicas_[hub].find_slot(created_[i].key.packed(), header)) {
                lost++;
            }
        }
    }
    return lost;
}

bool World::delete_random_slot(int hub) {
    Replica& replica = replicas_[hub];
    if (replica.slot_count() == 0 || deleted_key_count_ >= MAX_TOMBSTONES) {
        return false;
    }
    uint32_t key = replica.slot_header(int(random_.below(uint32_t(replica.slot_count())))).key.packed();
    if (replica.delete_slot(key) != Result::Ok) {
        return false;
    }
    deleted_keys_[deleted_key_count_++] = key;
    return true;
}

void World::delete_user(int hub, uint16_t user) {
    int removed = 0;
    if (user == 0 || user >= MAX_TRACKED_USERS || replicas_[hub].delete_user(user, removed) != Result::Ok) {
        return;
    }
    uint32_t epoch = replicas_[hub].user_epoch(user);
    if (epoch > deleted_epoch_[user]) {
        deleted_epoch_[user] = epoch;
    }
}

bool World::truly_dead(const SlotHeader& header) const {
    uint32_t key = header.key.packed();
    for (int i = 0; i < deleted_key_count_; i++) {
        if (deleted_keys_[i] == key) {
            return true;
        }
    }
    return header.user != 0 && header.user < MAX_TRACKED_USERS && header.user_epoch < deleted_epoch_[header.user];
}

int World::resurrected_slots() {
    int bad = 0;
    for (int hub = 0; hub < num_hubs_; hub++) {
        for (int i = 0; i < replicas_[hub].slot_count(); i++) {
            if (truly_dead(replicas_[hub].slot_header(i))) {
                bad++;
            }
        }
    }
    return bad;
}

bool World::totals_consistent() {
    static DigitsTotals digits;
    static GestureTotals gestures[2];
    static GazeTotals gaze;
    for (int hub = 0; hub < num_hubs_; hub++) {
        Replica& replica = replicas_[hub];
        if (replica.recount(digits, gestures, gaze) != Result::Ok ||
            std::memcmp(&gaze, &replica.gaze_totals(false), sizeof(gaze)) != 0 ||
            std::memcmp(&gestures[0], &replica.gesture_totals(PayloadType::GestureBlend, false), sizeof(gestures[0])) != 0) {
            return false;
        }
    }
    return true;
}

// Identical state everywhere, and every hub's running totals equal a fresh recount.
bool World::converged() {
    uint32_t digest = replicas_[0].digest();
    for (int hub = 1; hub < num_hubs_; hub++) {
        if (replicas_[hub].digest() != digest ||
            std::memcmp(&replicas_[hub].gaze_totals(false), &replicas_[0].gaze_totals(false), sizeof(GazeTotals)) != 0) {
            return false;
        }
    }
    return totals_consistent();
}

// ======================================================================================
// Scenario 1: partitions + deletions, including the hard case
// ======================================================================================

struct ScenarioResult {
    bool converged;
    int resurrected;  // must be 0
    int wrongly_lost;  // must be 0
    bool offline_case_tested;
    uint32_t heal_ms;
    uint32_t converge_ms;  // time from heal until all hubs were identical
    uint32_t samples;
    int slot_deletions;
    int user_deletions;
    int slots;
    int tombstones;
};

// Round 1 is fully connected. Round 2 is the hard case: one hub where someone is calibrating is
// cut off, that person's data is deleted elsewhere, and the cut-off hub keeps recording them.
// Rounds 3-4 split hubs randomly. Deletions happen at random throughout.
ScenarioResult run_scenario(World& world, uint64_t seed, int num_hubs, const FaultConfig& faults, bool verbose) {
    ScenarioResult result = {};
    world.reset(seed, num_hubs, faults);
    Random& random = world.random();

    const int rounds = 4;
    for (int round = 0; round < rounds; round++) {
        int groups[MAX_DEVICES];
        int isolated = -1;
        if (round == 0) {
            for (int hub = 0; hub < num_hubs; hub++) {
                groups[hub] = 0;
            }
        } else if (round == 1 && num_hubs >= 2) {
            isolated = int(random.below(uint32_t(num_hubs)));
            for (int hub = 0; hub < num_hubs; hub++) {
                groups[hub] = hub == isolated ? 1 : 0;
            }
        } else {
            int group_count = 2 + int(random.below(2));
            for (int hub = 0; hub < num_hubs; hub++) {
                groups[hub] = int(random.below(uint32_t(group_count)));
            }
        }
        world.set_groups(groups);
        if (verbose) {
            std::printf("t=%6.1fs  round %d: groups", world.now_ms() / 1000.0, round + 1);
            for (int hub = 0; hub < num_hubs; hub++) {
                std::printf(" %c:%d", 'A' + hub, groups[hub]);
            }
            std::printf("\n");
        }

        if (isolated >= 0) {
            world.teach_random(isolated);  // make sure someone is calibrating there
            uint16_t user = world.user_at(isolated);
            int elsewhere = (isolated + 1 + int(random.below(uint32_t(num_hubs - 1)))) % num_hubs;
            if (user != 0) {
                world.delete_user(elsewhere, user);  // pressed on a hub that can't reach the isolated one
                result.user_deletions++;
                result.offline_case_tested = true;
                if (verbose) {
                    std::printf("           user %u deleted at %c while still recording at cut-off hub %c\n", user,
                                'A' + elsewhere, 'A' + isolated);
                }
            }
        }

        uint32_t round_ms = 5000 + random.below(10001);
        for (uint32_t elapsed = 0; elapsed < round_ms; elapsed += 100) {
            int hub = int(random.below(uint32_t(num_hubs)));
            if (random.chance(0.5)) {
                world.teach_random(hub);
                result.samples++;
            }
            if (isolated >= 0 && random.chance(0.5)) {
                world.teach_random(isolated);  // the cut-off hub keeps recording its user
                result.samples++;
            }
            if (random.chance(0.01)) {
                world.finish_user(hub);
            }
            if (random.chance(0.012) && world.delete_random_slot(int(random.below(uint32_t(num_hubs))))) {
                result.slot_deletions++;
            }
            if (random.chance(0.002) && world.users_created() > 0) {
                world.delete_user(int(random.below(uint32_t(num_hubs))),
                                  uint16_t(1 + random.below(world.users_created())));
                result.user_deletions++;
            }
            world.advance(100);
        }
    }

    world.finish_everyone();
    world.heal();
    result.heal_ms = world.now_ms();
    if (verbose) {
        std::printf("t=%6.1fs  healed, calibration stopped (%u samples, %d slot deletions, %d user deletions)\n",
                    result.heal_ms / 1000.0, unsigned(result.samples), result.slot_deletions, result.user_deletions);
    }
    const uint32_t timeout_ms = 120000;
    while (world.now_ms() - result.heal_ms < timeout_ms) {
        world.advance(100);
        if (world.converged()) {
            result.converged = true;
            result.converge_ms = world.now_ms() - result.heal_ms;
            break;
        }
    }
    result.resurrected = world.resurrected_slots();
    result.wrongly_lost = world.wrongly_lost_slots();
    result.slots = world.replica(0).slot_count();
    result.tombstones = world.replica(0).tombstone_count();
    return result;
}

// ======================================================================================
// Scenario 2: more people than the store can hold
// ======================================================================================

struct CapacityResult {
    bool hit_cap;
    bool consistent;  // no corruption: every hub's totals equal a recount
    bool converged;
    uint32_t refused;
    int users;
};

CapacityResult run_capacity(World& world, uint64_t seed, const FaultConfig& faults) {
    CapacityResult result = {};
    world.reset(seed, 6, faults);
    Random& random = world.random();
    // 6 hubs, each seeing a new person every few seconds: well over MAX_SLOTS (each person = 2 slots).
    for (int step = 0; step < 600; step++) {
        int hub = int(random.below(6));
        world.teach_random(hub);
        if (random.chance(0.15)) {
            world.finish_user(hub);
        }
        world.advance(100);
    }
    world.finish_everyone();
    world.advance(30000);
    for (int hub = 0; hub < 6; hub++) {
        result.refused += world.replica(hub).stats().refused_store_full;
        if (world.replica(hub).slot_count() >= MAX_SLOTS) {
            result.hit_cap = true;
        }
    }
    result.consistent = world.totals_consistent();
    result.converged = world.converged();
    result.users = world.users_created();
    return result;
}

// ======================================================================================
// Scenario 3: relays in a line, and what broadcast saves
// ======================================================================================

constexpr int LINE_HUBS = 6;

struct HopsResult {
    bool ok;
    uint32_t slot_arrival_ms[LINE_HUBS];    // when hub i first had the slot created at hub 0
    uint32_t delete_arrival_ms[LINE_HUBS];  // when hub i first had the user's deletion
};

// Only neighbors can hear each other: A-B-C-D-E-F. Middle hubs have nobody calibrating;
// they just store and forward. A person calibrates gaze at A (9 dots x 15 samples).
HopsResult run_hops(World& world, uint64_t seed, const FaultConfig& faults) {
    HopsResult result = {};
    world.reset(seed, LINE_HUBS, faults);
    world.set_line();
    world.advance(3000);  // let heartbeats settle
    Replica& origin = world.replica(0);
    SlotKey key;
    const uint16_t user = 1;
    origin.open_session(PayloadType::Gaze, user, key);
    int32_t features[FACE_FEATURES] = {};
    for (int i = 0; i < 135; i++) {
        for (int f = 0; f < FACE_FEATURES; f++) {
            features[f] = int32_t(world.random().below(2001)) - 1000;
        }
        origin.add_gaze(key, features, int32_t(world.random().below(1001)), int32_t(world.random().below(1001)));
    }
    origin.close_session(key);
    uint32_t start = world.now_ms();
    bool all = false;
    for (int hub = 0; hub < LINE_HUBS; hub++) {
        result.slot_arrival_ms[hub] = hub == 0 ? 0 : UINT32_MAX;
    }
    while (!all && world.now_ms() - start < 120000) {
        world.advance(20);
        all = true;
        for (int hub = 1; hub < LINE_HUBS; hub++) {
            SlotHeader header;
            if (result.slot_arrival_ms[hub] == UINT32_MAX && world.replica(hub).find_slot(key.packed(), header)) {
                result.slot_arrival_ms[hub] = world.now_ms() - start;
            }
            all = all && result.slot_arrival_ms[hub] != UINT32_MAX;
        }
    }
    if (!all) {
        return result;
    }

    int removed = 0;
    origin.delete_user(user, removed);
    start = world.now_ms();
    all = false;
    for (int hub = 0; hub < LINE_HUBS; hub++) {
        result.delete_arrival_ms[hub] = hub == 0 ? 0 : UINT32_MAX;
    }
    while (!all && world.now_ms() - start < 120000) {
        world.advance(20);
        all = true;
        for (int hub = 1; hub < LINE_HUBS; hub++) {
            if (result.delete_arrival_ms[hub] == UINT32_MAX && world.replica(hub).user_epoch(user) > 0) {
                result.delete_arrival_ms[hub] = world.now_ms() - start;
            }
            all = all && result.delete_arrival_ms[hub] != UINT32_MAX;
        }
    }
    result.ok = all;
    return result;
}

// Fully connected, no faults: one slot created at A. How many chunks did A send, compared
// with sending a separate copy to every hub?
struct BroadcastResult {
    uint32_t chunks_sent_total;
    int chunks_per_slot;
    int receivers;
};

BroadcastResult run_broadcast(World& world, int num_hubs) {
    FaultConfig perfect = {0, 0, 0, 1, 20};
    world.reset(7, num_hubs, perfect);
    world.advance(3000);
    Replica& origin = world.replica(0);
    SlotKey key;
    origin.open_session(PayloadType::Gaze, 1, key);
    int32_t features[FACE_FEATURES] = {};
    origin.add_gaze(key, features, 500, 500);
    origin.close_session(key);
    uint32_t before = 0;
    for (int hub = 0; hub < num_hubs; hub++) {
        before += world.engine(hub).stats().chunks_sent;
    }
    world.advance(20000);
    BroadcastResult result = {};
    for (int hub = 0; hub < num_hubs; hub++) {
        result.chunks_sent_total += world.engine(hub).stats().chunks_sent;
    }
    result.chunks_sent_total -= before;
    uint8_t buffer[MAX_ENCODED_SLOT_BYTES];
    result.chunks_per_slot = chunks_for(origin.read_slot(key.packed(), buffer, sizeof(buffer)));
    result.receivers = num_hubs - 1;
    return result;
}

}  // namespace sim

// Not built into the unit tests or the playground, which have their own entry points.
#if !defined(PIO_UNIT_TESTING) && !defined(SIM_NO_MAIN)

static sim::World world;  // tens of MB: too big for the stack

static const sim::FaultConfig FAULTS = {
    0.20,  // drop 20% of packets
    0.10,  // duplicate 10%
    0.01,  // corrupt 1%
    1,     // delay 1-80 ms, which also reorders packets
    80,
};

static void print_run(const sim::ScenarioResult& result, int num_hubs) {
    const sim::RadioStats& radio = world.radio_stats();
    std::printf("\nradio: %u sent, %u delivered, %u dropped, %u blocked by partition, %u duplicated, %u corrupted\n",
                unsigned(radio.sent), unsigned(radio.delivered), unsigned(radio.dropped), unsigned(radio.blocked),
                unsigned(radio.duplicated), unsigned(radio.corrupted));
    std::printf("hub  slots  tombstones  digest    slots applied  crc failures  abandoned\n");
    for (int hub = 0; hub < num_hubs; hub++) {
        const SyncStats& stats = world.engine(hub).stats();
        std::printf("%c    %5d  %10d  %08X  %13u  %12u  %9u\n", 'A' + hub, world.replica(hub).slot_count(),
                    world.replica(hub).tombstone_count(), unsigned(world.replica(hub).digest()),
                    unsigned(stats.slots_applied), unsigned(stats.crc_failures), unsigned(stats.transfers_abandoned));
    }
    std::printf("\nslots that should be deleted but exist somewhere: %d\n", result.resurrected);
    std::printf("slots missing although nobody deleted them: %d\n", result.wrongly_lost);
    if (result.converged) {
        std::printf("CONVERGED %.1f s after healing: all %d hubs identical (%d slots, %d tombstones)\n",
                    result.converge_ms / 1000.0, num_hubs, result.slots, result.tombstones);
    } else {
        std::printf("NOT CONVERGED within the timeout\n");
    }
}

static int run_seeds(int seeds) {
    int converged = 0;
    int clean = 0;
    int nothing_lost = 0;
    int offline_cases = 0;
    int slot_deletions = 0;
    int user_deletions = 0;
    uint32_t slowest_ms = 0;
    double total_ms = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        sim::ScenarioResult result = sim::run_scenario(world, uint64_t(seed), 6, FAULTS, false);
        slot_deletions += result.slot_deletions;
        user_deletions += result.user_deletions;
        offline_cases += result.offline_case_tested ? 1 : 0;
        if (!result.offline_case_tested && std::getenv("SIM_DEBUG")) {
            std::printf("seed %d: hard case not tested\n", seed);
        }
        if (result.resurrected == 0) {
            clean++;
        }
        if (result.wrongly_lost == 0) {
            nothing_lost++;
        }
        if (result.converged) {
            converged++;
            total_ms += result.converge_ms;
            slowest_ms = result.converge_ms > slowest_ms ? result.converge_ms : slowest_ms;
        }
        if (!result.converged || result.resurrected != 0 || result.wrongly_lost != 0) {
            std::printf("seed %d FAILED: converged=%d resurrected=%d wrongly_lost=%d (replay: program %d 6)\n", seed,
                        result.converged ? 1 : 0, result.resurrected, result.wrongly_lost, seed);
        }
    }
    std::printf("%d seeds, 6 hubs, %d slot deletions and %d user deletions in total\n", seeds, slot_deletions,
                user_deletions);
    std::printf("  converged:                      %d/%d (after healing: average %.1f s, slowest %.1f s)\n",
                converged, seeds, converged ? total_ms / converged / 1000.0 : 0.0, slowest_ms / 1000.0);
    std::printf("  no deleted slot came back:      %d/%d\n", clean, seeds);
    std::printf("  nothing deleted by mistake:     %d/%d\n", nothing_lost, seeds);
    std::printf("  offline-during-deletion tested: %d/%d\n", offline_cases, seeds);
    return converged == seeds && clean == seeds && nothing_lost == seeds ? 0 : 1;
}

static int run_capacity_seeds(int seeds) {
    int hit = 0;
    int consistent = 0;
    int converged = 0;
    uint32_t refused = 0;
    int users = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        sim::CapacityResult result = sim::run_capacity(world, uint64_t(seed), FAULTS);
        hit += result.hit_cap ? 1 : 0;
        consistent += result.consistent ? 1 : 0;
        converged += result.converged ? 1 : 0;
        refused += result.refused;
        users += result.users;
    }
    std::printf("%d seeds, 6 hubs, %.0f people per run on average (2 slots each, MAX_SLOTS = %d)\n", seeds,
                double(users) / seeds, MAX_SLOTS);
    std::printf("  runs that hit the store limit:  %d/%d (%u incoming slots refused in total)\n", hit, seeds,
                unsigned(refused));
    std::printf("  totals still exact (no corruption): %d/%d\n", consistent, seeds);
    std::printf("  converged anyway:               %d/%d (not expected above the limit)\n", converged, seeds);
    return consistent == seeds ? 0 : 1;
}

static int run_hops_seeds(int seeds) {
    double slot_sum[sim::LINE_HUBS] = {};
    double delete_sum[sim::LINE_HUBS] = {};
    uint32_t slot_max[sim::LINE_HUBS] = {};
    uint32_t delete_max[sim::LINE_HUBS] = {};
    int ok = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        sim::HopsResult result = sim::run_hops(world, uint64_t(seed), FAULTS);
        if (!result.ok) {
            std::printf("seed %d: did not reach every hub\n", seed);
            continue;
        }
        ok++;
        for (int hub = 1; hub < sim::LINE_HUBS; hub++) {
            slot_sum[hub] += result.slot_arrival_ms[hub];
            delete_sum[hub] += result.delete_arrival_ms[hub];
            slot_max[hub] = result.slot_arrival_ms[hub] > slot_max[hub] ? result.slot_arrival_ms[hub] : slot_max[hub];
            delete_max[hub] = result.delete_arrival_ms[hub] > delete_max[hub] ? result.delete_arrival_ms[hub] : delete_max[hub];
        }
    }
    std::printf("Line A-B-C-D-E-F (neighbors only), same faults, %d/%d seeds reached every hub\n", ok, seeds);
    std::printf("hops  gaze slot arrives (avg / max)   deletion arrives (avg / max)\n");
    for (int hub = 1; hub < sim::LINE_HUBS; hub++) {
        std::printf("%4d  %8.2f s / %6.2f s          %8.2f s / %6.2f s\n", hub,
                    ok ? slot_sum[hub] / ok / 1000.0 : 0.0, slot_max[hub] / 1000.0,
                    ok ? delete_sum[hub] / ok / 1000.0 : 0.0, delete_max[hub] / 1000.0);
    }
    std::printf("\nBroadcast: one gaze slot created at A, no packet loss\n");
    std::printf("hubs  receivers  chunks per copy  chunks actually sent  separate copies would need\n");
    for (int hubs = 2; hubs <= MAX_DEVICES; hubs += 2) {
        sim::BroadcastResult result = sim::run_broadcast(world, hubs);
        std::printf("%4d  %9d  %15d  %20u  %26d\n", hubs, result.receivers, result.chunks_per_slot,
                    unsigned(result.chunks_sent_total), result.chunks_per_slot * result.receivers);
    }
    return ok == seeds ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--seeds") == 0) {
        return run_seeds(std::atoi(argv[2]));
    }
    if (argc == 3 && std::strcmp(argv[1], "--capacity") == 0) {
        return run_capacity_seeds(std::atoi(argv[2]));
    }
    if (argc == 3 && std::strcmp(argv[1], "--hops") == 0) {
        return run_hops_seeds(std::atoi(argv[2]));
    }
    uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    int num_hubs = argc > 2 ? std::atoi(argv[2]) : 6;
    if (num_hubs < 2 || num_hubs > MAX_DEVICES) {
        std::printf("hubs must be 2-%d\n", MAX_DEVICES);
        return 1;
    }
    std::printf("seed %llu, %d hubs, faults: drop %.0f%%, duplicate %.0f%%, corrupt %.0f%%, delay %u-%u ms\n",
                (unsigned long long)seed, num_hubs, FAULTS.drop_rate * 100, FAULTS.duplicate_rate * 100,
                FAULTS.corrupt_rate * 100, unsigned(FAULTS.min_delay_ms), unsigned(FAULTS.max_delay_ms));
    sim::ScenarioResult result = sim::run_scenario(world, seed, num_hubs, FAULTS, true);
    print_run(result, num_hubs);
    return result.converged && result.resurrected == 0 && result.wrongly_lost == 0 ? 0 : 1;
}

#endif
