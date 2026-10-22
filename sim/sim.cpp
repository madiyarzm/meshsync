// Laptop simulator for the sync protocol: many virtual hubs in one process, connected by a
// fake radio that drops, delays, reorders, duplicates, and corrupts packets, with switchable
// partitions. All randomness comes from one seed, so any run (and any failure) can be replayed.
//
//   .pio/build/native/program                 one verbose run (seed 1, 6 hubs)
//   .pio/build/native/program 42 8            one verbose run with seed 42 and 8 hubs
//   .pio/build/native/program --seeds 100     100 quiet runs; exit code 1 if any fails to converge
//
// The --seeds mode is the automated sync test: every run must end with all hubs identical.

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
    uint32_t blocked;  // by a partition
    uint32_t duplicated;
    uint32_t corrupted;
    uint32_t overflowed;  // in-flight queue full (should stay 0)
};

constexpr uint8_t SIM_APP = 2;
constexpr uint32_t FLUSH_INTERVAL_MS = 2000;  // same as the firmware: save open sessions every 2 s

class World {
public:
    static constexpr uint32_t STEP_MS = 5;

    bool reset(uint64_t seed, int num_hubs, const FaultConfig& faults);
    void set_groups(const int group_of_hub[]);
    void heal();
    void advance(uint32_t duration_ms);

    // A hub records one random sample for whoever is calibrating there now.
    void teach_random(int hub);
    // The current person at this hub finishes; the next sample starts a new user's sessions.
    void finish_user(int hub);
    void finish_everyone();

    bool converged();

    uint32_t now_ms() const { return now_ms_; }
    Random& random() { return random_; }
    const RadioStats& radio_stats() const { return radio_; }
    Replica& replica(int hub) { return replicas_[hub]; }
    const SyncEngine& engine(int hub) const { return engines_[hub]; }

private:
    struct InFlight {
        bool used;
        uint8_t to;
        uint8_t from;
        uint8_t length;
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
    void broadcast(int from, const uint8_t* data, size_t length);
    void queue_packet(int from, int to, const uint8_t* data, size_t length);
    void deliver_due_packets();

    int num_hubs_ = 0;
    uint32_t now_ms_ = 0;
    uint32_t last_flush_ms_ = 0;
    uint16_t next_user_ = 1;
    FaultConfig faults_ = {};
    Random random_;
    RadioStats radio_ = {};
    int group_[MAX_DEVICES] = {};
    Calibration calibration_[MAX_DEVICES] = {};
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
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        in_flight_[i].used = false;
    }
    for (int hub = 0; hub < num_hubs_; hub++) {
        group_[hub] = 0;
        calibration_[hub] = {};
        stores_[hub].wipe();
        replicas_[hub].begin(uint8_t(hub), &stores_[hub]);
        contexts_[hub] = {this, hub};
        engines_[hub].begin(&replicas_[hub], SIM_APP, &World::send_from_hub, nullptr, &contexts_[hub], now_ms_);
    }
    return true;
}

void World::set_groups(const int group_of_hub[]) {
    for (int hub = 0; hub < num_hubs_; hub++) {
        group_[hub] = group_of_hub[hub];
    }
}

void World::heal() {
    for (int hub = 0; hub < num_hubs_; hub++) {
        group_[hub] = 0;
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
        // Checked on arrival, so packets in the air when a partition starts are lost too.
        if (group_[packet.from] != group_[packet.to]) {
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
        calibration.user = next_user_++;
        if (replica.open_session(PayloadType::GestureBlend, calibration.user, calibration.gesture) != Result::Ok ||
            replica.open_session(PayloadType::Gaze, calibration.user, calibration.gaze) != Result::Ok) {
            replica.close_session(calibration.gesture);
            return;  // store full: this hub can't take a new person
        }
        calibration.active = true;
    }
    int32_t features[FACE_FEATURES];
    for (int f = 0; f < FACE_FEATURES; f++) {
        features[f] = int32_t(random_.below(2001)) - 1000;
    }
    replica.add_gesture(calibration.gesture, int(random_.below(GESTURE_CLASSES)), features);
    replica.add_gaze(calibration.gaze, features, int32_t(random_.below(1001)), int32_t(random_.below(1001)));
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

// Identical state everywhere, and every hub's running totals equal a fresh recount.
bool World::converged() {
    static DigitsTotals digits;
    static GestureTotals gestures[2];
    static GazeTotals gaze;
    uint32_t digest = replicas_[0].digest();
    for (int hub = 0; hub < num_hubs_; hub++) {
        Replica& replica = replicas_[hub];
        if (replica.digest() != digest || replica.recount(digits, gestures, gaze) != Result::Ok) {
            return false;
        }
        if (std::memcmp(&gaze, &replica.gaze_totals(false), sizeof(gaze)) != 0 ||
            std::memcmp(&gestures[0], &replica.gesture_totals(PayloadType::GestureBlend, false),
                        sizeof(gestures[0])) != 0) {
            return false;
        }
        if (std::memcmp(&replica.gaze_totals(false), &replicas_[0].gaze_totals(false), sizeof(gaze)) != 0) {
            return false;
        }
    }
    return true;
}

// One full run: connected -> several random partitions (people keep calibrating) -> heal -> wait.
struct ScenarioResult {
    bool converged;
    uint32_t heal_ms;
    uint32_t converge_ms;  // time from heal until all hubs were identical
    uint32_t samples;
    int slots;
};

ScenarioResult run_scenario(World& world, uint64_t seed, int num_hubs, const FaultConfig& faults, bool verbose) {
    ScenarioResult result = {};
    world.reset(seed, num_hubs, faults);
    Random& random = world.random();

    // Round 1 is fully connected. Later rounds split hubs randomly into 2-3 groups.
    const int rounds = 4;
    for (int round = 0; round < rounds; round++) {
        int groups[MAX_DEVICES];
        int group_count = round == 0 ? 1 : 2 + int(random.below(2));
        for (int hub = 0; hub < num_hubs; hub++) {
            groups[hub] = int(random.below(uint32_t(group_count)));
        }
        world.set_groups(groups);
        if (verbose) {
            std::printf("t=%6.1fs  round %d: groups", world.now_ms() / 1000.0, round + 1);
            for (int hub = 0; hub < num_hubs; hub++) {
                std::printf(" %c:%d", 'A' + hub, groups[hub]);
            }
            std::printf("\n");
        }
        uint32_t round_ms = 5000 + random.below(10001);
        for (uint32_t elapsed = 0; elapsed < round_ms; elapsed += 100) {
            int hub = int(random.below(uint32_t(num_hubs)));
            if (random.chance(0.5)) {
                world.teach_random(hub);
                result.samples++;
            }
            if (random.chance(0.01)) {
                world.finish_user(hub);  // the person leaves; someone new sits down later
            }
            world.advance(100);
        }
    }

    world.finish_everyone();
    world.heal();
    result.heal_ms = world.now_ms();
    if (verbose) {
        std::printf("t=%6.1fs  healed, calibration stopped (%u samples recorded)\n", result.heal_ms / 1000.0,
                    unsigned(result.samples));
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
    result.slots = world.replica(0).slot_count();
    return result;
}

}  // namespace sim

// Not built into the unit tests, which have their own main().
#ifndef PIO_UNIT_TESTING

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
    std::printf("hub  slots  digest      slots applied  crc failures  abandoned\n");
    for (int hub = 0; hub < num_hubs; hub++) {
        const SyncStats& stats = world.engine(hub).stats();
        std::printf("%c    %5d  %08X  %13u  %12u  %9u\n", 'A' + hub, world.replica(hub).slot_count(),
                    unsigned(world.replica(hub).digest()), unsigned(stats.slots_applied),
                    unsigned(stats.crc_failures), unsigned(stats.transfers_abandoned));
    }
    if (result.converged) {
        std::printf("\nCONVERGED %.1f s after healing: all %d hubs hold identical slots (%d) and totals\n",
                    result.converge_ms / 1000.0, num_hubs, result.slots);
    } else {
        std::printf("\nNOT CONVERGED within the timeout\n");
    }
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--seeds") == 0) {
        int seeds = std::atoi(argv[2]);
        int passed = 0;
        uint32_t slowest_ms = 0;
        double total_ms = 0;
        for (int seed = 1; seed <= seeds; seed++) {
            sim::ScenarioResult result = sim::run_scenario(world, uint64_t(seed), 6, FAULTS, false);
            if (result.converged) {
                passed++;
                total_ms += result.converge_ms;
                if (result.converge_ms > slowest_ms) {
                    slowest_ms = result.converge_ms;
                }
            } else {
                std::printf("seed %d did NOT converge (replay: program %d 6)\n", seed, seed);
            }
        }
        std::printf("%d/%d seeds converged; after healing: average %.1f s, slowest %.1f s\n", passed, seeds,
                    passed ? total_ms / passed / 1000.0 : 0.0, slowest_ms / 1000.0);
        return passed == seeds ? 0 : 1;
    }

    uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    int num_hubs = argc > 2 ? std::atoi(argv[2]) : 6;
    if (num_hubs < 1 || num_hubs > MAX_DEVICES) {
        std::printf("hubs must be 1-%d\n", MAX_DEVICES);
        return 1;
    }
    std::printf("seed %llu, %d hubs, faults: drop %.0f%%, duplicate %.0f%%, corrupt %.0f%%, delay %u-%u ms\n",
                (unsigned long long)seed, num_hubs, FAULTS.drop_rate * 100, FAULTS.duplicate_rate * 100,
                FAULTS.corrupt_rate * 100, unsigned(FAULTS.min_delay_ms), unsigned(FAULTS.max_delay_ms));
    sim::ScenarioResult result = sim::run_scenario(world, seed, num_hubs, FAULTS, true);
    print_run(result, num_hubs);
    return result.converged ? 0 : 1;
}

#endif
