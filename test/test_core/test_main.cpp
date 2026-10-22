// Unit tests for the meshstate core. Run: pio test -e native
// The sync protocol end to end is tested by the simulator: .pio/build/native/program --seeds 100

#include <unity.h>

#include <cmath>
#include <cstring>

#include "models.h"
#include "replica.h"
#include "sync.h"

using namespace meshstate;

// Large objects live here, not on the stack, the same way the firmware holds them.
static MemoryStore<MAX_SLOTS> stores[4];
static Replica replicas[4];
static uint8_t buffer[MAX_ENCODED_SLOT_BYTES];
static DigitsTotals recount_digits;
static GestureTotals recount_gestures[2];
static GazeTotals recount_gaze;

void setUp() {
    for (int i = 0; i < 4; i++) {
        stores[i].wipe();
        replicas[i].begin(uint8_t(i), &stores[i]);
    }
}
void tearDown() {}

// Small deterministic random generator (xorshift32) so failures can be replayed.
static uint32_t rng_state = 1;
static uint32_t next_random() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void random_features(int32_t features[FACE_FEATURES]) {
    for (int f = 0; f < FACE_FEATURES; f++) {
        features[f] = int32_t(next_random() % 2001) - 1000;
    }
}

static bool totals_match_recount(Replica& replica) {
    if (replica.recount(recount_digits, recount_gestures, recount_gaze) != Result::Ok) {
        return false;
    }
    return std::memcmp(&recount_digits, &replica.digits_totals(), sizeof(DigitsTotals)) == 0 &&
           std::memcmp(&recount_gestures[0], &replica.gesture_totals(PayloadType::GestureBlend, false),
                       sizeof(GestureTotals)) == 0 &&
           std::memcmp(&recount_gestures[1], &replica.gesture_totals(PayloadType::GestureLandmark, false),
                       sizeof(GestureTotals)) == 0 &&
           std::memcmp(&recount_gaze, &replica.gaze_totals(false), sizeof(GazeTotals)) == 0;
}

// Copy a replica's saved slot to another replica, as the radio would.
static Result send_slot(Replica& from, Replica& to, SlotKey key) {
    size_t length = from.read_slot(key.packed(), buffer, sizeof(buffer));
    return length == 0 ? Result::Invalid : to.merge_slot(buffer, length);
}

// --- packets and CRC -----------------------------------------------------------

void test_crc_and_packets() {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(check, sizeof(check)));  // standard check value
    TEST_ASSERT_EQUAL_HEX32(crc32(check, 9), crc32_update(crc32(check, 4), check + 4, 5));

    uint8_t packet[MAX_PACKET_BYTES];
    HeartbeatPacket heartbeat = {2, 1, 300, 0xABCD1234};
    size_t length = write_heartbeat(heartbeat, packet, sizeof(packet));
    HeartbeatPacket heartbeat_back;
    TEST_ASSERT_TRUE(read_heartbeat(packet, length, heartbeat_back));
    TEST_ASSERT_EQUAL_HEX32(0xABCD1234, heartbeat_back.digest);
    TEST_ASSERT_FALSE(read_heartbeat(packet, length - 1, heartbeat_back));

    static InventoryPacket page;
    page = {1, 0, 2, uint8_t(ENTRIES_PER_PAGE), {}};
    for (int i = 0; i < ENTRIES_PER_PAGE; i++) {
        page.entries[i] = {ENTRY_SLOT, uint32_t(i), uint32_t(i * 7)};
    }
    length = write_inventory(page, packet, sizeof(packet));
    TEST_ASSERT_TRUE(length <= MAX_PACKET_BYTES);
    static InventoryPacket page_back;
    TEST_ASSERT_TRUE(read_inventory(packet, length, page_back));
    TEST_ASSERT_EQUAL_UINT32(26 * 7, page_back.entries[26].value);
    TEST_ASSERT_FALSE(read_inventory(packet, length - 1, page_back));  // truncated

    uint8_t data[MAX_CHUNK_DATA];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = uint8_t(i);
    }
    ChunkPacket chunk = {1, 0x01020003, 9, 0, 23, 0xDEADBEEF, uint8_t(MAX_CHUNK_DATA), data};
    length = write_chunk(chunk, packet, sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(MAX_PACKET_BYTES, length);  // a full chunk fills one ESP-NOW frame
    ChunkPacket chunk_back;
    TEST_ASSERT_TRUE(read_chunk(packet, length, chunk_back));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, chunk_back.data, MAX_CHUNK_DATA);
    TEST_ASSERT_FALSE(read_chunk(packet, length - 1, chunk_back));
    packet[0] ^= 0xFF;
    TEST_ASSERT_TRUE(packet_type(packet, length) == PacketType::Invalid);  // bad magic
}

// --- writing -------------------------------------------------------------------

void test_bad_samples_change_nothing() {
    Replica& a = replicas[0];
    SlotKey gaze;
    TEST_ASSERT_TRUE(a.open_session(PayloadType::Gaze, 5, gaze) == Result::Ok);
    int32_t features[FACE_FEATURES] = {};
    features[3] = MAX_FEATURE_VALUE + 1;
    TEST_ASSERT_TRUE(a.add_gaze(gaze, features, 500, 500) == Result::BadInput);
    features[3] = 0;
    TEST_ASSERT_TRUE(a.add_gaze(gaze, features, 1001, 500) == Result::BadInput);
    TEST_ASSERT_TRUE(a.add_gaze(gaze, nullptr, 500, 500) == Result::BadInput);
    TEST_ASSERT_TRUE(a.add_gesture(gaze, 0, features) == Result::BadInput);  // wrong payload type
    TEST_ASSERT_EQUAL_UINT32(0, a.session_version(gaze));
    TEST_ASSERT_EQUAL_INT64(0, a.gaze_totals(false).n());

    SlotKey not_mine = {1, uint8_t(PayloadType::Digits), 0};
    TEST_ASSERT_TRUE(a.resume_session(not_mine, 0) == Result::NotOwner);
}

void test_merge_is_idempotent() {
    Replica& a = replicas[0];
    Replica& b = replicas[1];
    SlotKey key;
    a.open_session(PayloadType::GestureBlend, 3, key);
    int32_t features[FACE_FEATURES];
    for (int i = 0; i < 20; i++) {
        random_features(features);
        TEST_ASSERT_TRUE(a.add_gesture(key, i % 3, features) == Result::Ok);
    }
    a.flush();
    TEST_ASSERT_TRUE(send_slot(a, b, key) == Result::Ok);
    uint32_t digest = b.digest();
    GestureTotals before = b.gesture_totals(PayloadType::GestureBlend, false);
    TEST_ASSERT_TRUE(send_slot(a, b, key) == Result::Unchanged);  // same update twice
    TEST_ASSERT_EQUAL_HEX32(digest, b.digest());
    TEST_ASSERT_EQUAL_MEMORY(&before, &b.gesture_totals(PayloadType::GestureBlend, false), sizeof(GestureTotals));
    TEST_ASSERT_EQUAL_HEX32(a.digest(), b.digest());
}

// --- order independence and exactness ----------------------------------------------

constexpr int MAX_SNAPSHOTS = 60;
struct Snapshot {
    size_t length;
    uint8_t data[MAX_ENCODED_SLOT_BYTES];
};
static Snapshot snapshots[MAX_SNAPSHOTS];
static GestureTotals central_gesture;
static GazeTotals central_gaze;
static GesturePayload one_gesture;
static GazePayload one_gaze;

void test_merge_order_does_not_matter_and_equals_centralized() {
    rng_state = 777;
    central_gesture.clear();
    central_gaze.clear();
    int snapshot_count = 0;

    // Three devices each record two sessions, saving stale snapshots along the way.
    for (int device = 0; device < 3; device++) {
        Replica& owner = replicas[device];
        SlotKey gesture_key;
        SlotKey gaze_key;
        owner.open_session(PayloadType::GestureBlend, uint16_t(10 + device), gesture_key);
        owner.open_session(PayloadType::Gaze, uint16_t(10 + device), gaze_key);
        for (int step = 0; step < 40; step++) {
            int32_t features[FACE_FEATURES];
            random_features(features);
            int label = int(next_random() % GESTURE_CLASSES);
            int32_t tx = int32_t(next_random() % 1001);
            int32_t ty = int32_t(next_random() % 1001);
            TEST_ASSERT_TRUE(owner.add_gesture(gesture_key, label, features) == Result::Ok);
            TEST_ASSERT_TRUE(owner.add_gaze(gaze_key, features, tx, ty) == Result::Ok);
            // Centralized: every sample added straight into one total, no slots involved.
            gesture_sample(one_gesture, label, features);
            gaze_sample(one_gaze, features, tx, ty);
            accumulate(central_gesture.values, one_gesture.values, GesturePayload::VALUES, +1);
            accumulate(central_gaze.values, one_gaze.values, GazePayload::VALUES, +1);
            if (step % 10 == 9) {
                owner.flush();
                SlotKey keys[2] = {gesture_key, gaze_key};
                for (int k = 0; k < 2; k++) {
                    Snapshot& snapshot = snapshots[snapshot_count++];
                    snapshot.length = owner.read_slot(keys[k].packed(), snapshot.data, sizeof(snapshot.data));
                }
            }
        }
    }

    uint32_t first_digest = 0;
    for (int trial = 0; trial < 30; trial++) {
        Replica& receiver = replicas[3];
        stores[3].wipe();
        receiver.begin(3, &stores[3]);
        // Random order, with every snapshot delivered 1-3 times.
        int order[MAX_SNAPSHOTS * 3];
        int deliveries = 0;
        for (int i = 0; i < snapshot_count; i++) {
            int copies = 1 + int(next_random() % 3);
            for (int c = 0; c < copies; c++) {
                order[deliveries++] = i;
            }
        }
        for (int i = deliveries - 1; i > 0; i--) {
            int j = int(next_random() % uint32_t(i + 1));
            int swap = order[i];
            order[i] = order[j];
            order[j] = swap;
        }
        for (int i = 0; i < deliveries; i++) {
            Result result = receiver.merge_slot(snapshots[order[i]].data, snapshots[order[i]].length);
            TEST_ASSERT_TRUE(result == Result::Ok || result == Result::Unchanged);
        }
        TEST_ASSERT_EQUAL_MEMORY(&central_gesture, &receiver.gesture_totals(PayloadType::GestureBlend, false),
                                 sizeof(GestureTotals));
        TEST_ASSERT_EQUAL_MEMORY(&central_gaze, &receiver.gaze_totals(false), sizeof(GazeTotals));
        if (trial == 0) {
            first_digest = receiver.digest();
        }
        TEST_ASSERT_EQUAL_HEX32(first_digest, receiver.digest());
    }
}

// --- deletion --------------------------------------------------------------------

void test_tombstone_beats_any_version() {
    Replica& owner = replicas[0];
    Replica& early = replicas[1];  // gets the tombstone first
    Replica& late = replicas[2];   // gets the data first
    SlotKey key;
    owner.open_session(PayloadType::GestureLandmark, 4, key);
    int32_t features[FACE_FEATURES];
    random_features(features);
    owner.add_gesture(key, 1, features);
    owner.flush();
    static Snapshot v1;
    v1.length = owner.read_slot(key.packed(), v1.data, sizeof(v1.data));
    owner.add_gesture(key, 2, features);
    owner.flush();
    static Snapshot v2;
    v2.length = owner.read_slot(key.packed(), v2.data, sizeof(v2.data));

    TEST_ASSERT_TRUE(early.merge_tombstone(key.packed()) == Result::Ok);
    TEST_ASSERT_TRUE(early.merge_slot(v2.data, v2.length) == Result::Deleted);
    TEST_ASSERT_TRUE(early.merge_slot(v1.data, v1.length) == Result::Deleted);

    TEST_ASSERT_TRUE(late.merge_slot(v1.data, v1.length) == Result::Ok);
    TEST_ASSERT_TRUE(late.merge_slot(v2.data, v2.length) == Result::Ok);
    TEST_ASSERT_TRUE(late.merge_tombstone(key.packed()) == Result::Ok);
    TEST_ASSERT_TRUE(late.merge_slot(v2.data, v2.length) == Result::Deleted);  // stale copy arrives later
    TEST_ASSERT_EQUAL_INT64(0, late.gesture_totals(PayloadType::GestureLandmark, false).count(2));
    TEST_ASSERT_EQUAL_HEX32(early.digest(), late.digest());

    TEST_ASSERT_TRUE(owner.delete_slot(key.packed()) == Result::Ok);  // the owner too
    TEST_ASSERT_EQUAL_INT(0, owner.slot_count());
    TEST_ASSERT_EQUAL_HEX32(early.digest(), owner.digest());
}

void test_user_deletion_covers_slots_nobody_knew_about() {
    Replica& laptop_a = replicas[0];  // keeps adding the user's data, offline
    Replica& laptop_b = replicas[1];  // where the user presses "delete my data"
    const uint16_t user = 42;
    SlotKey old_key;
    SlotKey offline_key;
    int32_t features[FACE_FEATURES];
    random_features(features);
    laptop_a.open_session(PayloadType::GestureBlend, user, old_key);
    laptop_a.add_gesture(old_key, 0, features);
    laptop_a.close_session(old_key);
    TEST_ASSERT_TRUE(send_slot(laptop_a, laptop_b, old_key) == Result::Ok);

    // Partition. B deletes; A, not knowing, records a new session for the same user.
    int removed = 0;
    TEST_ASSERT_TRUE(laptop_b.delete_user(user, removed) == Result::Ok);
    TEST_ASSERT_EQUAL_INT(1, removed);
    laptop_a.open_session(PayloadType::GestureBlend, user, offline_key);
    laptop_a.add_gesture(offline_key, 1, features);
    laptop_a.close_session(offline_key);

    // Heal: B's deletion counter reaches A and kills both, including the one B never saw.
    TEST_ASSERT_TRUE(laptop_a.merge_user_epoch(user, laptop_b.user_epoch(user)) == Result::Ok);
    TEST_ASSERT_EQUAL_INT(0, laptop_a.slot_count());
    TEST_ASSERT_EQUAL_INT64(0, laptop_a.gesture_totals(PayloadType::GestureBlend, false).total_count());
    TEST_ASSERT_EQUAL_HEX32(laptop_a.digest(), laptop_b.digest());

    // The user can calibrate again afterwards: new sessions carry the new counter and live.
    SlotKey fresh_key;
    laptop_b.open_session(PayloadType::GestureBlend, user, fresh_key);
    laptop_b.add_gesture(fresh_key, 2, features);
    laptop_b.close_session(fresh_key);
    TEST_ASSERT_TRUE(send_slot(laptop_b, laptop_a, fresh_key) == Result::Ok);
    TEST_ASSERT_EQUAL_INT(1, laptop_a.slot_count());
}

// --- running totals ---------------------------------------------------------------

void test_running_totals_match_recount() {
    rng_state = 99;
    Replica& a = replicas[0];
    Replica& b = replicas[1];
    a.set_active_user(1);
    SlotKey keys[6];
    int key_count = 0;
    for (int round = 0; round < 200; round++) {
        uint32_t action = next_random() % 10;
        int32_t features[FACE_FEATURES];
        random_features(features);
        if (key_count < 6 && (key_count == 0 || action == 0)) {
            Replica& owner = next_random() % 2 ? a : b;
            owner.flush();
            for (int s = 0; s < 6; s++) {
                if (s < key_count) {
                    owner.close_session(keys[s]);
                }
            }
            PayloadType type = next_random() % 2 ? PayloadType::Gaze : PayloadType::GestureBlend;
            if (owner.open_session(type, uint16_t(1 + next_random() % 2), keys[key_count]) == Result::Ok) {
                key_count++;
            }
        } else if (action < 7 && key_count > 0) {
            SlotKey key = keys[next_random() % uint32_t(key_count)];
            Replica& owner = key.device == 0 ? a : b;
            if (key.type == uint8_t(PayloadType::Gaze)) {
                owner.add_gaze(key, features, int32_t(next_random() % 1001), int32_t(next_random() % 1001));
            } else {
                owner.add_gesture(key, int(next_random() % 3), features);
            }
        } else if (action == 7 && key_count > 0) {
            a.flush();
            b.flush();
            SlotKey key = keys[next_random() % uint32_t(key_count)];
            send_slot(key.device == 0 ? a : b, key.device == 0 ? b : a, key);
        } else if (action == 8 && key_count > 0) {
            a.delete_slot(keys[next_random() % uint32_t(key_count)].packed());
        } else if (action == 9 && round % 50 == 49) {
            int removed;
            b.delete_user(2, removed);
            a.merge_user_epoch(2, b.user_epoch(2));
        }
        TEST_ASSERT_TRUE(totals_match_recount(a));
        TEST_ASSERT_TRUE(totals_match_recount(b));
    }
    // "Own" totals kept incrementally equal a fresh rebuild for the same user.
    GazeTotals own_running = a.gaze_totals(true);
    a.set_active_user(1);
    TEST_ASSERT_EQUAL_MEMORY(&own_running, &a.gaze_totals(true), sizeof(GazeTotals));
}

// --- limits and refusals ---------------------------------------------------------

void test_overflow_is_refused_and_rolled_back() {
    int64_t totals[3] = {10, INT64_MAX - 5, 20};
    int64_t values[3] = {1, 6, 1};
    TEST_ASSERT_TRUE(accumulate(totals, values, 3, +1) == Result::Overflow);
    TEST_ASSERT_EQUAL_INT64(10, totals[0]);  // the first add was rolled back
    TEST_ASSERT_EQUAL_INT64(INT64_MAX - 5, totals[1]);
    uint64_t unsigned_totals[2] = {5, 3};
    uint32_t unsigned_values[2] = {1, 4};
    TEST_ASSERT_TRUE(accumulate(unsigned_totals, unsigned_values, 2, -1) == Result::Overflow);  // would go below 0
    TEST_ASSERT_EQUAL_UINT64(5, unsigned_totals[0]);

    // A slot with sums no real samples could produce is refused on merge.
    Replica& a = replicas[0];
    SlotKey key;
    a.open_session(PayloadType::GestureBlend, 1, key);
    int32_t features[FACE_FEATURES] = {};
    a.add_gesture(key, 0, features);
    a.flush();
    size_t length = a.read_slot(key.packed(), buffer, sizeof(buffer));
    uint8_t* sum_field = buffer + HEADER_BYTES + 8 * GESTURE_CLASSES;  // sum[0][0]
    write_u64(sum_field, uint64_t(INT64_MAX));
    TEST_ASSERT_TRUE(replicas[1].merge_slot(buffer, length) == Result::Invalid);
}

void test_schema_mismatch_is_refused() {
    Replica& a = replicas[0];
    SlotKey key;
    a.open_session(PayloadType::Gaze, 1, key);
    int32_t features[FACE_FEATURES] = {};
    a.add_gaze(key, features, 1, 2);
    a.flush();
    size_t length = a.read_slot(key.packed(), buffer, sizeof(buffer));
    buffer[4] = uint8_t(buffer[4] + 1);  // the schema byte
    TEST_ASSERT_TRUE(replicas[1].merge_slot(buffer, length) == Result::SchemaMismatch);
    TEST_ASSERT_EQUAL_INT(0, replicas[1].slot_count());
}

void test_store_and_tombstone_limits() {
    Replica& a = replicas[0];
    Replica& b = replicas[1];
    int32_t features[FACE_FEATURES] = {};
    for (int i = 0; i < MAX_SLOTS; i++) {
        SlotKey key;
        TEST_ASSERT_TRUE(a.open_session(PayloadType::GestureBlend, 1, key) == Result::Ok);
        a.add_gesture(key, 0, features);
        TEST_ASSERT_TRUE(a.close_session(key) == Result::Ok);
    }
    SlotKey extra;
    TEST_ASSERT_TRUE(a.open_session(PayloadType::GestureBlend, 1, extra) == Result::StoreFull);
    SlotKey theirs;
    b.open_session(PayloadType::GestureBlend, 2, theirs);
    b.add_gesture(theirs, 0, features);
    b.flush();
    TEST_ASSERT_TRUE(send_slot(b, a, theirs) == Result::StoreFull);
    TEST_ASSERT_EQUAL_UINT32(1, a.stats().refused_store_full);

    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        TEST_ASSERT_TRUE(b.merge_tombstone(0x07000000u + uint32_t(i)) == Result::Ok);
    }
    TEST_ASSERT_TRUE(b.merge_tombstone(0x07FFFFFFu) == Result::TombstonesFull);
}

void test_reboot_keeps_saved_state_and_never_reuses_sessions() {
    Replica& a = replicas[0];
    SlotKey key;
    a.open_session(PayloadType::Gaze, 9, key);
    int32_t features[FACE_FEATURES] = {};
    a.add_gaze(key, features, 100, 200);
    a.add_gaze(key, features, 300, 400);
    a.flush();
    uint32_t saved_digest = a.digest();
    a.add_gaze(key, features, 500, 600);  // not saved: nobody has seen this version
    TEST_ASSERT_EQUAL_HEX32(saved_digest, a.digest());  // unsaved samples are invisible to peers

    TEST_ASSERT_TRUE(a.begin(0, &stores[0]) == Result::Ok);  // power cut
    TEST_ASSERT_EQUAL_HEX32(saved_digest, a.digest());
    TEST_ASSERT_EQUAL_INT64(2, a.gaze_totals(false).n());
    SlotKey next;
    a.open_session(PayloadType::Gaze, 9, next);
    TEST_ASSERT_TRUE(next.session > key.session);
    TEST_ASSERT_TRUE(a.resume_session(key, 9) == Result::Ok);  // resuming from the saved copy is safe
    TEST_ASSERT_EQUAL_UINT32(2, a.session_version(key));
}

// --- models -----------------------------------------------------------------------

void test_gesture_model_and_personal_mix() {
    Replica& a = replicas[0];
    a.set_active_user(1);
    SlotKey mine;
    SlotKey theirs;
    a.open_session(PayloadType::GestureBlend, 1, mine);
    a.open_session(PayloadType::GestureBlend, 2, theirs);
    int32_t features[FACE_FEATURES] = {};
    for (int i = 0; i < 30; i++) {
        for (int c = 0; c < GESTURE_CLASSES; c++) {
            for (int f = 0; f < FACE_FEATURES; f++) {
                features[f] = int32_t(c * 600 + int(next_random() % 50));
            }
            a.add_gesture(theirs, c, features);  // someone else calibrated all three gestures
        }
    }
    static GestureModel model;
    TEST_ASSERT_TRUE(model.fit(a.gesture_totals(PayloadType::GestureBlend, false),
                               a.gesture_totals(PayloadType::GestureBlend, true), 1.0, GESTURE_VAR_FLOOR));
    for (int f = 0; f < FACE_FEATURES; f++) {
        features[f] = 1210;
    }
    Prediction prediction = model.predict(features);
    TEST_ASSERT_EQUAL_INT(2, prediction.label);  // works for user 1 with zero own samples
    TEST_ASSERT_TRUE(prediction.confidence > 0.9);
    TEST_ASSERT_EQUAL_INT(2, model.nearest_centroid(features));

    // lambda = 0: only your own data counts, and you have none yet.
    model.fit(a.gesture_totals(PayloadType::GestureBlend, false), a.gesture_totals(PayloadType::GestureBlend, true),
              0.0, GESTURE_VAR_FLOOR);
    TEST_ASSERT_EQUAL_INT(-1, model.predict(features).label);
    TEST_ASSERT_FALSE(model.fit(a.gesture_totals(PayloadType::GestureBlend, false),
                                a.gesture_totals(PayloadType::GestureBlend, true), std::nan(""), GESTURE_VAR_FLOOR));
}

void test_gaze_model_recovers_a_linear_map() {
    Replica& a = replicas[0];
    a.set_active_user(1);
    static GazeModel model;
    TEST_ASSERT_FALSE(model.fit(a.gaze_totals(false), a.gaze_totals(true), 1.0, GAZE_RIDGE_ALPHA));  // no data

    SlotKey key;
    a.open_session(PayloadType::Gaze, 1, key);
    rng_state = 5;
    int32_t features[FACE_FEATURES];
    for (int i = 0; i < 200; i++) {
        random_features(features);
        // Screen position is a fixed linear function of two features, plus a little noise.
        int32_t tx = 500 + features[0] / 4 + int32_t(next_random() % 11) - 5;
        int32_t ty = 500 - features[5] / 4 + int32_t(next_random() % 11) - 5;
        a.add_gaze(key, features, tx, ty);
    }
    TEST_ASSERT_TRUE(model.fit(a.gaze_totals(false), a.gaze_totals(true), 1.0, GAZE_RIDGE_ALPHA));
    for (int f = 0; f < FACE_FEATURES; f++) {
        features[f] = 0;
    }
    features[0] = 800;  // expect x = 0.5 + 0.2, y = 0.5
    double x = 0;
    double y = 0;
    TEST_ASSERT_TRUE(model.predict(features, x, y));
    TEST_ASSERT_TRUE(std::fabs(x - 0.70) < 0.02);
    TEST_ASSERT_TRUE(std::fabs(y - 0.50) < 0.02);
    TEST_ASSERT_EQUAL_INT(5, gaze_cell(x, y));  // middle row, right column
    TEST_ASSERT_EQUAL_INT(0, gaze_cell(-0.3, -1.0));
    TEST_ASSERT_EQUAL_INT(8, gaze_cell(1.4, 1.0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc_and_packets);
    RUN_TEST(test_bad_samples_change_nothing);
    RUN_TEST(test_merge_is_idempotent);
    RUN_TEST(test_merge_order_does_not_matter_and_equals_centralized);
    RUN_TEST(test_tombstone_beats_any_version);
    RUN_TEST(test_user_deletion_covers_slots_nobody_knew_about);
    RUN_TEST(test_running_totals_match_recount);
    RUN_TEST(test_overflow_is_refused_and_rolled_back);
    RUN_TEST(test_schema_mismatch_is_refused);
    RUN_TEST(test_store_and_tombstone_limits);
    RUN_TEST(test_reboot_keeps_saved_state_and_never_reuses_sessions);
    RUN_TEST(test_gesture_model_and_personal_mix);
    RUN_TEST(test_gaze_model_recovers_a_linear_map);
    return UNITY_END();
}
