// Laptop simulator for the sync protocol.
//
//   .pio/build/native/program                 one verbose run (seed 1, 6 hubs)
//   .pio/build/native/program 42 8            one verbose run with seed 42 and 8 hubs
//   .pio/build/native/program --seeds 100     100 quiet runs, prints a summary
//
// The same seed always replays the exact same run.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "world.h"

// Not built into the unit tests, which have their own main().
#ifndef PIO_UNIT_TESTING

static sim::World world;  // several MB: too big for the stack

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
    std::printf("hub  %-32s%13s  %12s  %9s\n", "version vector", "slots applied", "crc failures", "abandoned");
    for (int hub = 0; hub < num_hubs; hub++) {
        const construct::SyncStats& stats = world.engine(hub).stats();
        std::printf("%c    ", 'A' + hub);
        for (int owner = 0; owner < num_hubs; owner++) {
            std::printf("%3u ", unsigned(world.node(hub).slots[owner].version));
        }
        std::printf("%*s%13u  %12u  %9u\n", (8 - num_hubs) * 4, "", unsigned(stats.slots_applied),
                    unsigned(stats.crc_failures), unsigned(stats.transfers_abandoned));
    }
    if (result.converged) {
        std::printf("\nCONVERGED %.1f s after healing: all %d hubs hold identical slots\n",
                    result.converge_ms / 1000.0, num_hubs);
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
    if (num_hubs < 1 || num_hubs > construct::MAX_NODES) {
        std::printf("hubs must be 1-%d\n", construct::MAX_NODES);
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
