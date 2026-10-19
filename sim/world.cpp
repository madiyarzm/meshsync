#include "world.h"

#include <cstdio>
#include <cstring>

namespace sim {

using construct::MAX_NODES;
using construct::NUM_CLASSES;
using construct::NUM_FEATURES;

bool World::reset(uint64_t seed, int num_hubs, const FaultConfig& faults) {
    if (num_hubs < 1 || num_hubs > MAX_NODES) {
        return false;
    }
    num_hubs_ = num_hubs;
    now_ms_ = 0;
    faults_ = faults;
    random_.seed(seed);
    radio_ = {};
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        in_flight_[i].used = false;
    }
    for (int hub = 0; hub < num_hubs_; hub++) {
        group_[hub] = 0;
        nodes_[hub].init(hub);
        contexts_[hub] = {this, hub};
        engines_[hub].begin(&nodes_[hub], &codec_, &World::send_from_hub, nullptr, &contexts_[hub], now_ms_);
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
        if (!connected(packet.from, packet.to)) {
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
        for (int hub = 0; hub < num_hubs_; hub++) {
            engines_[hub].tick(now_ms_);
        }
    }
}

void World::teach_random(int hub) {
    uint8_t pixels[NUM_FEATURES];
    for (int j = 0; j < NUM_FEATURES; j++) {
        pixels[j] = uint8_t(random_.below(construct::MAX_PIXEL + 1));
    }
    nodes_[hub].teach(pixels, int(random_.below(NUM_CLASSES)));
}

bool World::converged() const {
    for (int hub = 0; hub < num_hubs_; hub++) {
        for (int owner = 0; owner < MAX_NODES; owner++) {
            bool owner_exists = owner < num_hubs_;
            const construct::Slot& copy = nodes_[hub].slots[owner];
            if (owner_exists && copy != nodes_[owner].slots[owner]) {
                return false;
            }
            if (!owner_exists && copy.version != 0) {
                return false;  // a slot nobody owns appeared: corrupted data got applied
            }
        }
    }
    return true;
}

ScenarioResult run_scenario(World& world, uint64_t seed, int num_hubs, const FaultConfig& faults, bool verbose) {
    ScenarioResult result = {};
    world.reset(seed, num_hubs, faults);
    Random& random = world.random();

    // Round 1 is fully connected. Later rounds split hubs randomly into 2-3 groups.
    // Each round lasts 5-15 s and hubs keep teaching throughout.
    const int rounds = 4;
    for (int round = 0; round < rounds; round++) {
        int groups[MAX_NODES];
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
            if (random.chance(0.3)) {
                world.teach_random(int(random.below(uint32_t(num_hubs))));
                result.teaches++;
            }
            world.advance(100);
        }
    }

    world.heal();
    result.heal_ms = world.now_ms();
    if (verbose) {
        std::printf("t=%6.1fs  healed, teaching stopped (%u drawings taught in total)\n",
                    result.heal_ms / 1000.0, unsigned(result.teaches));
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
    return result;
}

}  // namespace sim
