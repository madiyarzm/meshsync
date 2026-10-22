// Golden test: the C++ core must give exactly the same answers as the Python prototype
// on the Phase 1 data. Data comes from: python experiments/run_experiments.py --export-golden

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "golden_data.h"
#include "models.h"
#include "replica.h"

using namespace meshstate;

// Each room is one device with its own store. Large objects live here, not on the stack.
static MemoryStore<4> store_a;
static MemoryStore<4> store_b;
static Replica room_a;
static Replica room_b;
static DigitsTotals scenario_totals[golden::NUM_SCENARIOS];
static DigitModel model;
static uint8_t buffer[MAX_ENCODED_SLOT_BYTES];

static const SlotKey KEY_A = {0, uint8_t(PayloadType::Digits), 0};
static const SlotKey KEY_B = {1, uint8_t(PayloadType::Digits), 0};

void setUp() {}
void tearDown() {}

// Compares the actual bits, so even a last-digit rounding difference counts as a mismatch.
static bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

void test_teach_accepts_all_training_data() {
    TEST_ASSERT_TRUE(room_a.begin(0, &store_a) == Result::Ok);
    TEST_ASSERT_TRUE(room_b.begin(1, &store_b) == Result::Ok);
    TEST_ASSERT_TRUE(room_a.resume_session(KEY_A, 0) == Result::Ok);
    TEST_ASSERT_TRUE(room_b.resume_session(KEY_B, 0) == Result::Ok);
    for (int i = 0; i < golden::NUM_TRAIN; i++) {
        Replica& room = golden::train_room[i] == 0 ? room_a : room_b;
        SlotKey key = golden::train_room[i] == 0 ? KEY_A : KEY_B;
        TEST_ASSERT_TRUE(room.add_digit(key, golden::train_label[i], golden::train_pixels[i]) == Result::Ok);
    }
    TEST_ASSERT_TRUE(room_a.flush() == Result::Ok);
    TEST_ASSERT_TRUE(room_b.flush() == Result::Ok);
    scenario_totals[0] = room_a.digits_totals();
    scenario_totals[1] = room_b.digits_totals();
}

// The payload (everything after the 15-byte header) is the Phase 1 raw layout, byte for byte.
void test_room_payloads_match_python_bytes() {
    size_t length = room_a.read_slot(KEY_A.packed(), buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(MAX_ENCODED_SLOT_BYTES, length);
    TEST_ASSERT_EQUAL_HEX32(golden::room_payload_crc[0], crc32(buffer + HEADER_BYTES, length - HEADER_BYTES));
    length = room_b.read_slot(KEY_B.packed(), buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_HEX32(golden::room_payload_crc[1], crc32(buffer + HEADER_BYTES, length - HEADER_BYTES));
}

void test_merge_makes_rooms_identical() {
    size_t length = room_b.read_slot(KEY_B.packed(), buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(room_a.merge_slot(buffer, length) == Result::Ok);
    length = room_a.read_slot(KEY_A.packed(), buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(room_b.merge_slot(buffer, length) == Result::Ok);
    TEST_ASSERT_EQUAL_HEX32(room_a.digest(), room_b.digest());
    TEST_ASSERT_EQUAL_MEMORY(&room_a.digits_totals(), &room_b.digits_totals(), sizeof(DigitsTotals));
    scenario_totals[2] = room_a.digits_totals();
}

static void check_scenario(int scenario) {
    const DigitsTotals& totals = scenario_totals[scenario];
    TEST_ASSERT_TRUE(model.fit(totals, DIGIT_VAR_FLOOR));

    int mismatches = 0;
    int nc_correct = 0;
    int gnb_correct = 0;
    char first_mismatch[160] = "";
    for (int i = 0; i < golden::NUM_TEST; i++) {
        const uint8_t* pixels = golden::test_pixels[i];
        int nc_label = digit_nearest_centroid(totals, pixels);
        Prediction prediction = model.predict(pixels);
        double scores[DIGIT_CLASSES];
        model.log_scores(pixels, scores);

        bool same = nc_label == golden::expected_nearest_centroid[scenario][i] &&
                    prediction.label == golden::expected_gnb_label[scenario][i] &&
                    same_bits(prediction.confidence, golden::expected_gnb_confidence[scenario][i]);
        for (int label = 0; label < DIGIT_CLASSES; label++) {
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
    RUN_TEST(test_room_payloads_match_python_bytes);
    RUN_TEST(test_merge_makes_rooms_identical);
    RUN_TEST(test_room_a_before_merge_matches_python);
    RUN_TEST(test_room_b_before_merge_matches_python);
    RUN_TEST(test_after_merge_matches_python);
    return UNITY_END();
}
