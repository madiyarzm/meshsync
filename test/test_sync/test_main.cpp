// Sync protocol tests: packet format, plus full simulator runs with faults and partitions.
// Run: pio test -e native

#include <unity.h>

#include <cstdio>

#include "core.h"
#include "world.h"

using namespace construct;

static sim::World world;  // several MB: keep it off the stack

void setUp() {}
void tearDown() {}

void test_packets_round_trip_and_reject_truncation() {
    uint8_t buffer[MAX_PACKET_BYTES];

    HeartbeatPacket heartbeat = {};
    heartbeat.sender = 2;
    heartbeat.versions[2] = 77;
    size_t length = write_heartbeat(heartbeat, buffer, sizeof(buffer));
    HeartbeatPacket heartbeat_back;
    TEST_ASSERT_TRUE(read_heartbeat(buffer, length, heartbeat_back));
    TEST_ASSERT_EQUAL_UINT32(77, heartbeat_back.versions[2]);
    TEST_ASSERT_FALSE(read_heartbeat(buffer, length - 1, heartbeat_back));

    uint8_t data[MAX_CHUNK_DATA];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = uint8_t(i);
    }
    ChunkPacket chunk = {1, 3, 9, 0, 22, 0xDEADBEEF, uint8_t(MAX_CHUNK_DATA), data};
    length = write_chunk(chunk, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(MAX_PACKET_BYTES, length);
    ChunkPacket chunk_back;
    TEST_ASSERT_TRUE(read_chunk(buffer, length, chunk_back));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, chunk_back.slot_crc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, chunk_back.data, MAX_CHUNK_DATA);
    TEST_ASSERT_FALSE(read_chunk(buffer, length - 1, chunk_back));  // truncated
    buffer[0] ^= 0xFF;
    TEST_ASSERT_TRUE(packet_type(buffer, length) == PacketType::Invalid);  // bad magic
}

void test_perfect_radio_two_hubs_sync() {
    sim::FaultConfig perfect = {0, 0, 0, 1, 1};
    world.reset(1, 2, perfect);
    for (int i = 0; i < 5; i++) {
        world.teach_random(0);
        world.teach_random(1);
    }
    world.advance(3000);
    TEST_ASSERT_TRUE(world.converged());
    TEST_ASSERT_EQUAL_UINT32(5, world.node(1).slots[0].version);
}

static void run_seeds(const sim::FaultConfig& faults, int first_seed, int count) {
    for (int seed = first_seed; seed < first_seed + count; seed++) {
        sim::ScenarioResult result = sim::run_scenario(world, uint64_t(seed), 6, faults, false);
        char message[64];
        std::snprintf(message, sizeof(message), "seed %d did not converge", seed);
        TEST_ASSERT_TRUE_MESSAGE(result.converged, message);
        TEST_ASSERT_EQUAL_UINT32(0, world.radio_stats().overflowed);
    }
}

void test_faults_and_partitions_converge() {
    sim::FaultConfig faults = {0.20, 0.10, 0.01, 1, 80};
    run_seeds(faults, 1, 20);
}

void test_heavy_corruption_never_applies_bad_data() {
    // converged() also fails if any hub applied a slot that differs from its owner's.
    sim::FaultConfig faults = {0.05, 0.05, 0.10, 1, 40};
    run_seeds(faults, 100, 5);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_packets_round_trip_and_reject_truncation);
    RUN_TEST(test_perfect_radio_two_hubs_sync);
    RUN_TEST(test_faults_and_partitions_converge);
    RUN_TEST(test_heavy_corruption_never_applies_bad_data);
    return UNITY_END();
}
