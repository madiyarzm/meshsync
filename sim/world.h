#pragma once

// Simulated mesh: many virtual hubs in one process, connected by a fake radio.
// All randomness comes from one seed, so any run (and any failure) can be replayed exactly.

#include <cstddef>
#include <cstdint>

#include "core.h"

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
    uint32_t dropped;     // by the fault model
    uint32_t blocked;     // by a partition
    uint32_t duplicated;
    uint32_t corrupted;
    uint32_t overflowed;  // in-flight queue full (should stay 0)
};

class World {
public:
    static constexpr uint32_t STEP_MS = 5;

    // Returns false if num_hubs is not 1 to MAX_NODES.
    bool reset(uint64_t seed, int num_hubs, const FaultConfig& faults);

    // group_of_hub[i] = group number of hub i. Hubs only hear hubs in the same group.
    void set_groups(const int group_of_hub[]);
    void heal();

    void advance(uint32_t duration_ms);
    void teach_random(int hub);

    // True when every hub holds exactly the same slots as their owners.
    bool converged() const;

    uint32_t now_ms() const { return now_ms_; }
    int num_hubs() const { return num_hubs_; }
    Random& random() { return random_; }
    const RadioStats& radio_stats() const { return radio_; }
    const construct::Node& node(int hub) const { return nodes_[hub]; }
    const construct::SyncEngine& engine(int hub) const { return engines_[hub]; }

private:
    struct InFlight {
        bool used;
        uint8_t to;
        uint8_t from;
        uint8_t length;
        uint32_t deliver_at_ms;
        uint8_t data[construct::MAX_PACKET_BYTES];
    };
    struct HubContext {
        World* world;
        int hub;
    };
    static constexpr int MAX_IN_FLIGHT = 8192;

    static void send_from_hub(void* context, const uint8_t* data, size_t length);
    void broadcast(int from, const uint8_t* data, size_t length);
    void queue_packet(int from, int to, const uint8_t* data, size_t length);
    void deliver_due_packets();
    bool connected(int a, int b) const { return group_[a] == group_[b]; }

    int num_hubs_ = 0;
    uint32_t now_ms_ = 0;
    FaultConfig faults_ = {};
    Random random_;
    RadioStats radio_ = {};
    int group_[construct::MAX_NODES] = {};
    construct::RawCodec codec_;
    construct::Node nodes_[construct::MAX_NODES];
    construct::SyncEngine engines_[construct::MAX_NODES];
    HubContext contexts_[construct::MAX_NODES];
    InFlight in_flight_[MAX_IN_FLIGHT];
};

// One full run: connected -> several random partitions (teaching continues) -> heal -> wait.
struct ScenarioResult {
    bool converged;
    uint32_t heal_ms;      // time when the last partition was healed
    uint32_t converge_ms;  // time from heal until all hubs were identical
    uint32_t teaches;
};

ScenarioResult run_scenario(World& world, uint64_t seed, int num_hubs, const FaultConfig& faults, bool verbose);

}  // namespace sim
