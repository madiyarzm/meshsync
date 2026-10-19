// Unit tests for the core (firmware/src/core). Run: pio test -e native
// The sync protocol end to end is tested by the simulator: .pio/build/native/program --seeds 100

#include <unity.h>

#include <cmath>
#include <cstring>

#include "core.h"
#include "sync.h"

using namespace construct;

// Large objects live here, not on the stack, the same way the firmware will hold them.
static Slot slot_a;
static Slot slot_b;
static Node node_a;
static Node node_b;
static Node node_c;
static Node result_left;
static Node result_right;
static Totals totals;
static GaussianNB model;
static uint8_t buffer[RAW_SLOT_BYTES + 8];

void setUp() {}
void tearDown() {}

static void fill(uint8_t pixels[NUM_FEATURES], uint8_t value) {
    for (int j = 0; j < NUM_FEATURES; j++) {
        pixels[j] = value;
    }
}

// Small deterministic random generator (xorshift32) so failures can be replayed.
static uint32_t rng_state = 1;
static uint32_t next_random() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// --- teach ---------------------------------------------------------------------

void test_dimensions_match_prototype() {
    TEST_ASSERT_EQUAL_INT(10, NUM_CLASSES);
    TEST_ASSERT_EQUAL_INT(64, NUM_FEATURES);
    TEST_ASSERT_EQUAL_INT(16, MAX_PIXEL);
    TEST_ASSERT_EQUAL_UINT32(5164, RAW_SLOT_BYTES);
}

void test_teach_updates_sums_and_version() {
    uint8_t pixels[NUM_FEATURES];
    for (int j = 0; j < NUM_FEATURES; j++) {
        pixels[j] = uint8_t(j % 17);
    }
    slot_a.clear();
    TEST_ASSERT_TRUE(slot_a.teach(pixels, 3) == TeachResult::Ok);
    TEST_ASSERT_EQUAL_UINT32(1, slot_a.version);
    TEST_ASSERT_EQUAL_UINT32(1, slot_a.count[3]);
    for (int j = 0; j < NUM_FEATURES; j++) {
        TEST_ASSERT_EQUAL_UINT32(pixels[j], slot_a.sum[3][j]);
        TEST_ASSERT_EQUAL_UINT32(pixels[j] * pixels[j], slot_a.sumsq[3][j]);
    }
    TEST_ASSERT_TRUE(slot_a.is_valid());
}

void test_bad_teach_is_rejected_and_changes_nothing() {
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 0);
    slot_a.clear();
    slot_b.clear();
    TEST_ASSERT_TRUE(slot_a.teach(pixels, -1) == TeachResult::BadLabel);
    TEST_ASSERT_TRUE(slot_a.teach(pixels, 10) == TeachResult::BadLabel);
    TEST_ASSERT_TRUE(slot_a.teach(nullptr, 1) == TeachResult::BadPixel);
    pixels[63] = 17;
    TEST_ASSERT_TRUE(slot_a.teach(pixels, 1) == TeachResult::BadPixel);
    TEST_ASSERT_TRUE(slot_a == slot_b);
}

void test_teach_refuses_when_full() {
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 16);
    slot_a.clear();
    slot_a.count[2] = MAX_SAMPLES_PER_CLASS;
    TEST_ASSERT_TRUE(slot_a.teach(pixels, 2) == TeachResult::SlotFull);
    TEST_ASSERT_TRUE(slot_a.teach(pixels, 3) == TeachResult::Ok);  // other digits still fine
}

// --- merge ---------------------------------------------------------------------

void test_merge_keeps_higher_version_and_ignores_stale() {
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 5);
    slot_a.clear();
    slot_b.clear();
    slot_b.teach(pixels, 7);

    TEST_ASSERT_TRUE(slot_a.merge_from(slot_b));    // newer: taken
    TEST_ASSERT_TRUE(slot_a == slot_b);
    TEST_ASSERT_FALSE(slot_a.merge_from(slot_b));   // duplicate: ignored
    slot_b.clear();
    TEST_ASSERT_FALSE(slot_a.merge_from(slot_b));   // older: ignored
    TEST_ASSERT_EQUAL_UINT32(1, slot_a.version);
}

void test_node_init_and_receive_reject_bad_ids() {
    TEST_ASSERT_FALSE(node_a.init(-1));
    TEST_ASSERT_FALSE(node_a.init(MAX_NODES));
    TEST_ASSERT_TRUE(node_a.init(0));
    slot_a.clear();
    slot_a.version = 1;
    TEST_ASSERT_FALSE(node_a.receive(-1, slot_a));
    TEST_ASSERT_FALSE(node_a.receive(MAX_NODES, slot_a));
}

void test_teach_and_merge_between_nodes() {
    uint8_t pixels[NUM_FEATURES];
    node_a.init(0);
    node_b.init(1);
    fill(pixels, 1);
    node_a.teach(pixels, 0);
    fill(pixels, 2);
    node_b.teach(pixels, 1);
    fill(pixels, 3);
    node_b.teach(pixels, 1);

    node_a.merge_from(node_b);
    TEST_ASSERT_EQUAL_UINT32(1, node_a.slots[0].version);
    TEST_ASSERT_EQUAL_UINT32(2, node_a.slots[1].version);
    node_a.compute_totals(totals);
    TEST_ASSERT_EQUAL_UINT64(1, totals.count[0]);
    TEST_ASSERT_EQUAL_UINT64(2, totals.count[1]);
    TEST_ASSERT_EQUAL_UINT64(5, totals.sum[1][0]);
    TEST_ASSERT_EQUAL_UINT64(13, totals.sumsq[1][0]);
    TEST_ASSERT_EQUAL_UINT64(3, totals.total_count());
}

// Every version each owner's slot went through, so tests can pick random (stale) copies.
constexpr int LAW_OWNERS = 4;
constexpr int LAW_VERSIONS = 10;
static Slot history[LAW_OWNERS][LAW_VERSIONS];

static void build_random_history() {
    uint8_t pixels[NUM_FEATURES];
    for (int owner = 0; owner < LAW_OWNERS; owner++) {
        history[owner][0].clear();
        for (int v = 1; v < LAW_VERSIONS; v++) {
            history[owner][v] = history[owner][v - 1];
            for (int j = 0; j < NUM_FEATURES; j++) {
                pixels[j] = uint8_t(next_random() % (MAX_PIXEL + 1));
            }
            history[owner][v].teach(pixels, int(next_random() % NUM_CLASSES));
        }
    }
}

static void random_state(Node& node) {
    node.init(0);
    for (int owner = 0; owner < LAW_OWNERS; owner++) {
        node.receive(owner, history[owner][next_random() % LAW_VERSIONS]);
    }
}

static bool same_slots(const Node& left, const Node& right) {
    for (int owner = 0; owner < MAX_NODES; owner++) {
        if (left.slots[owner] != right.slots[owner]) {
            return false;
        }
    }
    return true;
}

void test_merge_is_commutative_associative_idempotent() {
    rng_state = 12345;
    build_random_history();
    for (int trial = 0; trial < 50; trial++) {
        random_state(node_a);
        random_state(node_b);
        random_state(node_c);

        // commutative: a+b == b+a
        result_left = node_a;
        result_left.merge_from(node_b);
        result_right = node_b;
        result_right.merge_from(node_a);
        TEST_ASSERT_TRUE(same_slots(result_left, result_right));

        // associative: (a+b)+c == a+(b+c)
        result_left.merge_from(node_c);
        result_right = node_b;
        result_right.merge_from(node_c);
        result_right.merge_from(node_a);
        TEST_ASSERT_TRUE(same_slots(result_left, result_right));

        // idempotent: a+a == a
        result_left = node_a;
        result_left.merge_from(node_a);
        TEST_ASSERT_TRUE(same_slots(result_left, node_a));
    }
}

// --- models --------------------------------------------------------------------

void test_models_with_nothing_taught() {
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 0);
    totals.clear();
    TEST_ASSERT_EQUAL_INT(-1, nearest_centroid_predict(totals, pixels));
    TEST_ASSERT_TRUE(model.fit(totals, VAR_FLOOR));
    Prediction prediction = model.predict(pixels);
    TEST_ASSERT_EQUAL_INT(-1, prediction.label);
    TEST_ASSERT_TRUE(prediction.confidence == 0.0);
}

void test_models_skip_empty_classes() {
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 16);
    slot_a.clear();
    slot_a.teach(pixels, 9);  // one sample: variance 0, only the floor keeps it finite
    totals.clear();
    totals.add(slot_a);

    fill(pixels, 0);
    TEST_ASSERT_EQUAL_INT(9, nearest_centroid_predict(totals, pixels));
    TEST_ASSERT_TRUE(model.fit(totals, VAR_FLOOR));
    Prediction prediction = model.predict(pixels);
    TEST_ASSERT_EQUAL_INT(9, prediction.label);
    TEST_ASSERT_TRUE(prediction.confidence == 1.0);

    double scores[NUM_CLASSES];
    model.log_scores(pixels, scores);
    TEST_ASSERT_TRUE(std::isinf(scores[0]) && scores[0] < 0);  // never taught
    TEST_ASSERT_TRUE(std::isfinite(scores[9]));
}

void test_nearest_centroid_tie_goes_to_lower_label() {
    uint8_t pixels[NUM_FEATURES];
    slot_a.clear();
    fill(pixels, 0);
    slot_a.teach(pixels, 4);
    fill(pixels, 2);
    slot_a.teach(pixels, 6);
    totals.clear();
    totals.add(slot_a);
    fill(pixels, 1);
    TEST_ASSERT_EQUAL_INT(4, nearest_centroid_predict(totals, pixels));
}

void test_gaussian_nb_picks_closer_class_with_confidence() {
    uint8_t dark[NUM_FEATURES];
    uint8_t bright[NUM_FEATURES];
    fill(dark, 0);
    fill(bright, 16);
    slot_a.clear();
    for (int i = 0; i < 5; i++) {
        slot_a.teach(dark, 0);
        slot_a.teach(bright, 1);
    }
    totals.clear();
    totals.add(slot_a);
    model.fit(totals, VAR_FLOOR);

    uint8_t query[NUM_FEATURES];
    fill(query, 15);
    Prediction prediction = model.predict(query);
    TEST_ASSERT_EQUAL_INT(1, prediction.label);
    TEST_ASSERT_TRUE(prediction.confidence > 0.5 && prediction.confidence <= 1.0);
}

void test_gaussian_nb_rejects_bad_floor() {
    totals.clear();
    TEST_ASSERT_FALSE(model.fit(totals, 0.0));
    TEST_ASSERT_FALSE(model.fit(totals, -1.0));
    TEST_ASSERT_FALSE(model.fit(totals, std::nan("")));
}

void test_predict_rejects_bad_drawing() {
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 1);
    slot_a.clear();
    slot_a.teach(pixels, 1);
    totals.clear();
    totals.add(slot_a);
    model.fit(totals, VAR_FLOOR);

    pixels[10] = 17;
    TEST_ASSERT_EQUAL_INT(-1, nearest_centroid_predict(totals, pixels));
    TEST_ASSERT_EQUAL_INT(-1, model.predict(pixels).label);
    TEST_ASSERT_EQUAL_INT(-1, model.predict(nullptr).label);
}

// --- serialization -------------------------------------------------------------

void test_raw_round_trip() {
    RawCodec codec;
    rng_state = 99;
    build_random_history();
    const Slot& original = history[0][LAW_VERSIONS - 1];
    TEST_ASSERT_EQUAL_UINT32(RAW_SLOT_BYTES, codec.encode(original, buffer, sizeof(buffer)));
    TEST_ASSERT_TRUE(codec.decode(buffer, RAW_SLOT_BYTES, slot_a));
    TEST_ASSERT_TRUE(slot_a == original);
}

void test_raw_is_little_endian_in_field_order() {
    RawCodec codec;
    slot_a.clear();
    slot_a.version = 0x04030201;
    slot_a.count[0] = 0x04030201;  // keep it valid: version >= total samples
    codec.encode(slot_a, buffer, sizeof(buffer));
    const uint8_t expected[8] = {1, 2, 3, 4, 1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buffer, 8);
}

void test_raw_rejects_wrong_sizes() {
    RawCodec codec;
    slot_a.clear();
    TEST_ASSERT_EQUAL_UINT32(0, codec.encode(slot_a, buffer, RAW_SLOT_BYTES - 1));
    TEST_ASSERT_EQUAL_UINT32(0, codec.encode(slot_a, nullptr, RAW_SLOT_BYTES));
    codec.encode(slot_a, buffer, sizeof(buffer));
    TEST_ASSERT_FALSE(codec.decode(buffer, RAW_SLOT_BYTES - 1, slot_b));  // truncated
    TEST_ASSERT_FALSE(codec.decode(buffer, RAW_SLOT_BYTES + 1, slot_b));  // extra bytes
    TEST_ASSERT_FALSE(codec.decode(nullptr, RAW_SLOT_BYTES, slot_b));
    TEST_ASSERT_TRUE(codec.decode(buffer, RAW_SLOT_BYTES, slot_b));
}

void test_raw_rejects_impossible_contents() {
    RawCodec codec;
    uint8_t pixels[NUM_FEATURES];
    fill(pixels, 4);

    slot_a.clear();
    slot_a.teach(pixels, 0);
    slot_a.sum[0][0] = 17;  // one sample can't sum above 16
    codec.encode(slot_a, buffer, sizeof(buffer));
    TEST_ASSERT_FALSE(codec.decode(buffer, RAW_SLOT_BYTES, slot_b));

    slot_a.clear();
    slot_a.teach(pixels, 0);
    slot_a.version = 0;  // fewer versions than samples
    codec.encode(slot_a, buffer, sizeof(buffer));
    TEST_ASSERT_FALSE(codec.decode(buffer, RAW_SLOT_BYTES, slot_b));

    slot_a.clear();
    slot_a.teach(pixels, 0);
    slot_a.sumsq[0][5] = 3;  // 4*4 = 16, can't be 3
    codec.encode(slot_a, buffer, sizeof(buffer));
    TEST_ASSERT_FALSE(codec.decode(buffer, RAW_SLOT_BYTES, slot_b));
}

// --- CRC32 ---------------------------------------------------------------------

void test_crc32_known_values() {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(check, sizeof(check)));  // standard check value
    TEST_ASSERT_EQUAL_HEX32(0x00000000, crc32(nullptr, 0));
}

void test_crc32_chains_across_pieces() {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint32_t crc = crc32_update(0, data, 4);
    crc = crc32_update(crc, data + 4, 5);
    TEST_ASSERT_EQUAL_HEX32(crc32(data, sizeof(data)), crc);
}

// --- sync packets --------------------------------------------------------------

void test_packets_round_trip_and_reject_damage() {
    uint8_t packet[MAX_PACKET_BYTES];

    HeartbeatPacket heartbeat = {};
    heartbeat.sender = 2;
    heartbeat.versions[2] = 77;
    size_t length = write_heartbeat(heartbeat, packet, sizeof(packet));
    HeartbeatPacket heartbeat_back;
    TEST_ASSERT_TRUE(read_heartbeat(packet, length, heartbeat_back));
    TEST_ASSERT_EQUAL_UINT32(77, heartbeat_back.versions[2]);
    TEST_ASSERT_FALSE(read_heartbeat(packet, length - 1, heartbeat_back));

    uint8_t data[MAX_CHUNK_DATA];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = uint8_t(i);
    }
    ChunkPacket chunk = {1, 3, 9, 0, 22, 0xDEADBEEF, uint8_t(MAX_CHUNK_DATA), data};
    length = write_chunk(chunk, packet, sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(MAX_PACKET_BYTES, length);  // a full chunk fills one ESP-NOW frame
    ChunkPacket chunk_back;
    TEST_ASSERT_TRUE(read_chunk(packet, length, chunk_back));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, chunk_back.slot_crc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, chunk_back.data, MAX_CHUNK_DATA);
    TEST_ASSERT_FALSE(read_chunk(packet, length - 1, chunk_back));  // truncated
    packet[0] ^= 0xFF;
    TEST_ASSERT_TRUE(packet_type(packet, length) == PacketType::Invalid);  // bad magic
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_dimensions_match_prototype);
    RUN_TEST(test_teach_updates_sums_and_version);
    RUN_TEST(test_bad_teach_is_rejected_and_changes_nothing);
    RUN_TEST(test_teach_refuses_when_full);
    RUN_TEST(test_merge_keeps_higher_version_and_ignores_stale);
    RUN_TEST(test_node_init_and_receive_reject_bad_ids);
    RUN_TEST(test_teach_and_merge_between_nodes);
    RUN_TEST(test_merge_is_commutative_associative_idempotent);
    RUN_TEST(test_models_with_nothing_taught);
    RUN_TEST(test_models_skip_empty_classes);
    RUN_TEST(test_nearest_centroid_tie_goes_to_lower_label);
    RUN_TEST(test_gaussian_nb_picks_closer_class_with_confidence);
    RUN_TEST(test_gaussian_nb_rejects_bad_floor);
    RUN_TEST(test_predict_rejects_bad_drawing);
    RUN_TEST(test_raw_round_trip);
    RUN_TEST(test_raw_is_little_endian_in_field_order);
    RUN_TEST(test_raw_rejects_wrong_sizes);
    RUN_TEST(test_raw_rejects_impossible_contents);
    RUN_TEST(test_crc32_known_values);
    RUN_TEST(test_crc32_chains_across_pieces);
    RUN_TEST(test_packets_round_trip_and_reject_damage);
    return UNITY_END();
}
