// Golden test: the C++ core must give exactly the same answers as the Python prototype
// on the Phase 1 data. Data comes from experiments/export_golden.py. Run: pio test -e native

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "core.h"
#include "golden_data.h"

using namespace construct;

static Node room_a;
static Node room_b;
static Totals scenario_totals[golden::NUM_SCENARIOS];
static GaussianNB model;
static uint8_t buffer[RAW_SLOT_BYTES];

void setUp() {}
void tearDown() {}

// Compares the actual bits, so even a last-digit rounding difference counts as a mismatch.
static bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

void test_teach_accepts_all_training_data() {
    room_a.init(0);
    room_b.init(1);
    for (int i = 0; i < golden::NUM_TRAIN; i++) {
        Node& room = golden::train_room[i] == 0 ? room_a : room_b;
        TEST_ASSERT_TRUE(room.teach(golden::train_pixels[i], golden::train_label[i]) == TeachResult::Ok);
    }
    room_a.compute_totals(scenario_totals[0]);
    room_b.compute_totals(scenario_totals[1]);
}

void test_room_slots_match_python_bytes() {
    RawCodec codec;
    TEST_ASSERT_EQUAL_UINT32(RAW_SLOT_BYTES, codec.encode(room_a.slots[0], buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_HEX32(golden::room_slot_crc[0], crc32(buffer, RAW_SLOT_BYTES));
    TEST_ASSERT_EQUAL_UINT32(RAW_SLOT_BYTES, codec.encode(room_b.slots[1], buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_HEX32(golden::room_slot_crc[1], crc32(buffer, RAW_SLOT_BYTES));
}

void test_merge_makes_rooms_identical() {
    room_a.merge_from(room_b);
    room_b.merge_from(room_a);
    for (int owner = 0; owner < MAX_NODES; owner++) {
        TEST_ASSERT_TRUE(room_a.slots[owner] == room_b.slots[owner]);
    }
    room_a.compute_totals(scenario_totals[2]);
}

static void check_scenario(int scenario) {
    const Totals& totals = scenario_totals[scenario];
    TEST_ASSERT_TRUE(model.fit(totals, VAR_FLOOR));

    int mismatches = 0;
    int nc_correct = 0;
    int gnb_correct = 0;
    char first_mismatch[160] = "";
    for (int i = 0; i < golden::NUM_TEST; i++) {
        const uint8_t* pixels = golden::test_pixels[i];
        int nc_label = nearest_centroid_predict(totals, pixels);
        Prediction prediction = model.predict(pixels);
        double scores[NUM_CLASSES];
        model.log_scores(pixels, scores);

        bool same = nc_label == golden::expected_nearest_centroid[scenario][i] &&
                    prediction.label == golden::expected_gnb_label[scenario][i] &&
                    same_bits(prediction.confidence, golden::expected_gnb_confidence[scenario][i]);
        for (int label = 0; label < NUM_CLASSES; label++) {
            if (!same_bits(scores[label], golden::expected_gnb_scores[scenario][i][label])) {
                same = false;
            }
        }
        if (!same) {
            if (mismatches == 0) {
                std::snprintf(first_mismatch, sizeof(first_mismatch),
                              "first mismatch at test sample %d (nc %d vs %d, gnb %d vs %d)", i, nc_label,
                              golden::expected_nearest_centroid[scenario][i], prediction.label,
                              golden::expected_gnb_label[scenario][i]);
            }
            mismatches++;
        }
        nc_correct += nc_label == golden::test_label[i];
        gnb_correct += prediction.label == golden::test_label[i];
    }

    char summary[160];
    std::snprintf(summary, sizeof(summary),
                  "%d/%d samples bit-identical to Python; accuracy: nearest centroid %d/%d, GNB %d/%d",
                  golden::NUM_TEST - mismatches, golden::NUM_TEST, nc_correct, golden::NUM_TEST, gnb_correct,
                  golden::NUM_TEST);
    TEST_MESSAGE(summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches, first_mismatch);
}

void test_room_a_before_merge_matches_python() { check_scenario(0); }
void test_room_b_before_merge_matches_python() { check_scenario(1); }
void test_after_merge_matches_python() { check_scenario(2); }

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_teach_accepts_all_training_data);
    RUN_TEST(test_room_slots_match_python_bytes);
    RUN_TEST(test_merge_makes_rooms_identical);
    RUN_TEST(test_room_a_before_merge_matches_python);
    RUN_TEST(test_room_b_before_merge_matches_python);
    RUN_TEST(test_after_merge_matches_python);
    return UNITY_END();
}
