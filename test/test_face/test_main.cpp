// Tests for the face-mode text protocol (firmware/face_app.*). Run: pio test -e native

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "face_app.h"

using namespace meshstate;

static MemoryStore<MAX_SLOTS> stores[2];
static Replica replicas[2];
static FaceApp apps[2];
static uint8_t buffer[MAX_ENCODED_SLOT_BYTES];

// Everything the app writes, one line per entry.
static char output[256][FACE_MAX_OUTPUT];
static int output_count = 0;
static void capture(void* context, const char* line) {
    (void)context;
    if (output_count < 256) {
        std::snprintf(output[output_count++], FACE_MAX_OUTPUT, "%s", line);
    }
}
static uint32_t fake_micros() {
    return 0;
}

// Sends one request and returns the last line written.
static const char* send(int hub, const char* text) {
    static char line[FACE_MAX_LINE + 64];
    std::snprintf(line, sizeof(line), "%s", text);
    output_count = 0;
    apps[hub].handle_line(line, 1000);
    return output_count > 0 ? output[output_count - 1] : "";
}

void setUp() {
    for (int i = 0; i < 2; i++) {
        stores[i].wipe();
        replicas[i].begin(uint8_t(i), &stores[i]);
        apps[i].begin(char('A' + i), 2, &replicas[i], nullptr, capture, fake_micros, nullptr);
    }
    output_count = 0;
}
void tearDown() {}

// Three well-separated gestures: feature 0 is high for class 1, feature 5 for class 2.
static void gesture_line(char* out, size_t size, const char* prefix, int label, int jitter) {
    int written = std::snprintf(out, size, "%s", prefix);
    for (int f = 0; f < FACE_FEATURES; f++) {
        int value = 100 + jitter;
        if (label == 1 && f == 0) {
            value = 800 + jitter;
        }
        if (label == 2 && f == 5) {
            value = 700 - jitter;
        }
        written += std::snprintf(out + written, size - size_t(written), " %d", value);
    }
}

// Calibrates `per_class` samples of each gesture for the active user; returns the session id.
static int calibrate(int hub, int per_class) {
    const char* reply = send(hub, "1 SESSION gest_blend");
    int session = 0;
    TEST_ASSERT_EQUAL(1, std::sscanf(reply, "1 OK SESSION %d", &session));
    char line[FACE_MAX_LINE + 1];
    char prefix[32];
    for (int label = 0; label < GESTURE_CLASSES; label++) {
        for (int i = 0; i < per_class; i++) {
            std::snprintf(prefix, sizeof(prefix), "2 ADD %d %d", session, label);
            gesture_line(line, sizeof(line), prefix, label, (i * 7) % 40);
            reply = send(hub, line);
            TEST_ASSERT_EQUAL(0, std::strncmp(reply, "2 OK ADD", 8));
        }
    }
    return session;
}

static int predicted_label(int hub, int label) {
    char line[FACE_MAX_LINE + 1];
    gesture_line(line, sizeof(line), "3 PREDICT gest_blend", label, 13);
    int predicted = -1;
    std::sscanf(send(hub, line), "3 OK PREDICT gest_blend %d", &predicted);
    return predicted;
}

static void copy_all_slots(int from, int to) {
    replicas[from].flush();
    for (int i = 0; i < replicas[from].slot_count(); i++) {
        uint32_t key = replicas[from].slot_header(i).key.packed();
        size_t length = replicas[from].read_slot(key, buffer, sizeof(buffer));
        TEST_ASSERT_TRUE(length > 0);
        replicas[to].merge_slot(buffer, length);
    }
}

void test_hello_and_bad_lines() {
    TEST_ASSERT_EQUAL_STRING("5 OK HELLO A 1 64", send(0, "5 HELLO 1"));
    TEST_ASSERT_EQUAL_STRING("0 ERR BAD_LINE", send(0, "HELLO"));             // no id
    TEST_ASSERT_EQUAL_STRING("0 ERR BAD_LINE", send(0, "70000 HELLO"));       // id out of range
    TEST_ASSERT_EQUAL_STRING("7 ERR UNKNOWN_COMMAND", send(0, "7 FLY"));
    TEST_ASSERT_EQUAL_STRING("8 ERR NO_USER", send(0, "8 SESSION gest_blend"));
    TEST_ASSERT_EQUAL_STRING("9 OK USER 42 0", send(0, "9 USER 42"));
    TEST_ASSERT_EQUAL_STRING("10 ERR BAD_LINE", send(0, "10 SESSION juggling"));
    TEST_ASSERT_EQUAL_STRING("11 ERR UNKNOWN_SESSION", send(0, "11 ADD 99 0 1 2 3 4 5 6 7 8 9 10 11 12"));
    TEST_ASSERT_EQUAL_STRING("12 OK SESSION 1", send(0, "12 SESSION gest_blend"));
    TEST_ASSERT_EQUAL_STRING("13 ERR BAD_LINE", send(0, "13 ADD 1 0 1 2 3"));              // too few features
    TEST_ASSERT_EQUAL_STRING("14 ERR BAD_LINE", send(0, "14 ADD 1 3 1 2 3 4 5 6 7 8 9 10 11 12"));  // label 3
    TEST_ASSERT_EQUAL_STRING("15 ERR BAD_FEATURES", send(0, "15 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 2001"));
    TEST_ASSERT_EQUAL_STRING("16 ERR BAD_FEATURES", send(0, "16 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 1x"));
    TEST_ASSERT_EQUAL_STRING("17 ERR BAD_LINE", send(0, "17 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18"));
    TEST_ASSERT_EQUAL_STRING("18 ERR NOT_READY", send(0, "18 PREDICT gest_blend 1 2 3 4 5 6 7 8 9 10 11 12"));
    TEST_ASSERT_EQUAL_STRING("19 OK ADD 1", send(0, "19 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 12"));
    TEST_ASSERT_EQUAL_STRING("20 OK CLOSE 1", send(0, "20 CLOSE 1"));
    TEST_ASSERT_EQUAL_STRING("21 ERR UNKNOWN_SESSION", send(0, "21 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 12"));
    TEST_ASSERT_EQUAL_STRING("22 ERR BAD_LINE", send(0, "22 LAMBDA -1"));
    TEST_ASSERT_EQUAL_STRING("23 OK LAMBDA 500", send(0, "23 LAMBDA 500"));
    TEST_ASSERT_EQUAL(0, output_count - 1);  // exactly one line per reply
}

void test_calibrate_and_predict() {
    send(0, "1 USER 7");
    calibrate(0, 45);
    for (int label = 0; label < GESTURE_CLASSES; label++) {
        TEST_ASSERT_EQUAL(label, predicted_label(0, label));
    }
    char line[FACE_MAX_LINE + 1];
    gesture_line(line, sizeof(line), "4 PREDICT gest_blend", 1, 13);
    int label = 0, p0 = 0, p1 = 0, p2 = 0, own = 0, others = 0;
    TEST_ASSERT_EQUAL(6, std::sscanf(send(0, line), "4 OK PREDICT gest_blend %d %d %d %d %d %d", &label, &p0, &p1,
                                     &p2, &own, &others));
    TEST_ASSERT_EQUAL(1, label);
    TEST_ASSERT_TRUE(p1 > 900);
    TEST_ASSERT_INT_WITHIN(1, 1000, p0 + p1 + p2);  // rounding
    TEST_ASSERT_EQUAL(135, own);
    TEST_ASSERT_EQUAL(0, others);
    TEST_ASSERT_TRUE(apps[0].led_on(1000));  // a confident gesture started: LED flash
}

// The headline: calibrate at hub A, sit down at hub B, and B already knows you.
void test_calibration_follows_you() {
    send(0, "1 USER 7");
    calibrate(0, 45);
    copy_all_slots(0, 1);  // what the mesh does
    TEST_ASSERT_EQUAL_STRING("1 OK USER 7 0", send(1, "1 USER 7"));
    for (int label = 0; label < GESTURE_CLASSES; label++) {
        TEST_ASSERT_EQUAL(label, predicted_label(1, label));
    }
    // A different person at B gets the shared model at lambda 1, and nothing at lambda 0.
    send(1, "2 USER 8");
    TEST_ASSERT_EQUAL(1, predicted_label(1, 1));
    send(1, "3 LAMBDA 0");
    TEST_ASSERT_EQUAL(-1, predicted_label(1, 1));
}

void test_delete_user_everywhere() {
    send(0, "1 USER 7");
    int session = calibrate(0, 10);
    copy_all_slots(0, 1);
    send(1, "1 USER 7");
    TEST_ASSERT_EQUAL_STRING("5 OK DELETE_USER 1 1", send(1, "5 DELETE_USER 7"));
    char line[FACE_MAX_LINE + 1];
    gesture_line(line, sizeof(line), "6 PREDICT gest_blend", 1, 0);
    TEST_ASSERT_EQUAL_STRING("6 ERR NOT_READY", send(1, line));

    // A hasn't heard yet and keeps recording; when the counter arrives, all of it dies.
    TEST_ASSERT_EQUAL(0, std::strncmp(send(0, "7 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 12"), "7 OK ADD", 8));
    output_count = 0;
    replicas[0].merge_user_epoch(7, replicas[1].user_epoch(7));
    apps[0].on_sync_event(SyncEvent::UserDeleted, 7);
    TEST_ASSERT_EQUAL_STRING("0 EVENT USER_DELETED 7 1", output[0]);
    TEST_ASSERT_EQUAL(0, replicas[0].slot_count());
    std::snprintf(line, sizeof(line), "8 ADD %d 0 1 2 3 4 5 6 7 8 9 10 11 12", session);
    TEST_ASSERT_EQUAL_STRING("8 ERR UNKNOWN_SESSION", send(0, line));
    gesture_line(line, sizeof(line), "9 PREDICT gest_blend", 1, 0);
    TEST_ASSERT_EQUAL_STRING("9 ERR NOT_READY", send(0, line));
}

void test_gaze_session() {
    send(0, "1 USER 3");
    int session = 0;
    TEST_ASSERT_EQUAL(1, std::sscanf(send(0, "2 SESSION gaze"), "2 OK SESSION %d", &session));
    // Target = the first two features (a linear map the ridge can learn).
    char line[FACE_MAX_LINE + 1];
    for (int i = 0; i < 90; i++) {
        int x = 100 + (i % 3) * 400;
        int y = 100 + ((i / 3) % 3) * 400;
        std::snprintf(line, sizeof(line), "3 ADD %d %d %d %d %d 5 %d 0 0 0 0 0 0 0 0", session, x, y, x, y, i % 7);
        TEST_ASSERT_EQUAL(0, std::strncmp(send(0, line), "3 OK ADD", 8));
    }
    int x = 0, y = 0, cell = -1;
    TEST_ASSERT_EQUAL(3, std::sscanf(send(0, "4 PREDICT gaze 900 900 5 3 0 0 0 0 0 0 0 0"), "4 OK PREDICT gaze %d %d %d",
                                     &x, &y, &cell));
    TEST_ASSERT_INT_WITHIN(30, 900, x);
    TEST_ASSERT_INT_WITHIN(30, 900, y);
    TEST_ASSERT_EQUAL(8, cell);  // bottom-right
    TEST_ASSERT_EQUAL_STRING("5 ERR BAD_LINE", send(0, "5 ADD 1 1001 0 1 2 3 4 5 6 7 8 9 10 11 12"));
}

void test_slots_and_status() {
    send(0, "1 USER 7");
    calibrate(0, 5);
    replicas[0].flush();
    send(0, "2 SLOTS");
    TEST_ASSERT_EQUAL(2, output_count);
    TEST_ASSERT_EQUAL_STRING("2 SLOT 00020001 A gest_blend 1 7 15", output[0]);
    TEST_ASSERT_EQUAL_STRING("2 OK SLOTS 1", output[1]);
    TEST_ASSERT_EQUAL(0, std::strncmp(send(0, "3 STATUS"), "3 OK STATUS letter=A user=7 epoch=0 lambda=1000 slots=1", 55));
}

// A page that went away without CLOSE must not use up the hub's two open sessions.
void test_new_page_or_user_closes_old_sessions() {
    send(0, "1 USER 7");
    TEST_ASSERT_EQUAL_STRING("2 OK SESSION 1", send(0, "2 SESSION gest_blend"));
    TEST_ASSERT_EQUAL_STRING("3 OK SESSION 2", send(0, "3 SESSION gest_lmk"));
    TEST_ASSERT_EQUAL_STRING("4 ERR TOO_MANY_OPEN", send(0, "4 SESSION gaze"));
    send(0, "5 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 12");
    send(0, "6 HELLO 1");  // a new page connects
    TEST_ASSERT_EQUAL_STRING("7 OK SESSION 3", send(0, "7 SESSION gaze"));
    TEST_ASSERT_EQUAL_STRING("8 ERR UNKNOWN_SESSION", send(0, "8 ADD 1 0 1 2 3 4 5 6 7 8 9 10 11 12"));
    TEST_ASSERT_EQUAL(1, replicas[0].slot_count());  // the closed session with a sample was saved
    send(0, "9 USER 8");  // next person
    TEST_ASSERT_EQUAL_STRING("10 OK SESSION 4", send(0, "10 SESSION gest_blend"));
    TEST_ASSERT_EQUAL_STRING("11 OK SESSION 5", send(0, "11 SESSION gest_lmk"));
}

// Packets from other boards are counted per sender and reported once a second.
void test_mesh_monitor() {
    HeartbeatPacket heartbeat = {1, 2, 3, 0xABCD0123};
    uint8_t packet[MAX_PACKET_BYTES];
    size_t length = write_heartbeat(heartbeat, packet, sizeof(packet));
    apps[0].on_radio_packet(packet, length, -40, 500);
    apps[0].on_radio_packet(packet, length, -50, 600);
    apps[0].on_radio_packet(packet, 2, -50, 600);         // too short: ignored
    heartbeat.sender = 0;                                   // our own id: ignored
    length = write_heartbeat(heartbeat, packet, sizeof(packet));
    apps[0].on_radio_packet(packet, length, -50, 600);
    apps[0].on_radio_send(packet, length);
    output_count = 0;
    apps[0].tick(1000);
    TEST_ASSERT_EQUAL(2, output_count);
    TEST_ASSERT_EQUAL_STRING("0 EVENT MESH 00000000 0 0 0 0 1 11 00", output[0]);
    // No sync engine attached here, so online/digest come out as 0.
    TEST_ASSERT_EQUAL_STRING("0 EVENT PEER B 2 0 400 -45 2 22 2 0 0 0 00000000 0 00", output[1]);
    output_count = 0;
    apps[0].tick(1500);  // not a second yet
    TEST_ASSERT_EQUAL(0, output_count);
}

// When the mesh side holds the state, PREDICT keeps using the last model instead of waiting.
static bool busy = false;
static bool test_lock(void* context, bool wait) {
    (void)context;
    return wait || !busy;
}
static void test_unlock(void* context) {
    (void)context;
}
void test_predict_never_waits_for_the_mesh() {
    apps[0].set_lock(test_lock, test_unlock);
    send(0, "1 USER 7");
    char line[FACE_MAX_LINE + 1];
    gesture_line(line, sizeof(line), "2 PREDICT gest_blend", 1, 0);
    busy = true;
    TEST_ASSERT_EQUAL_STRING("2 ERR NOT_READY", send(0, line));  // no model yet: the first fit waits
    busy = false;
    calibrate(0, 10);
    TEST_ASSERT_EQUAL(1, predicted_label(0, 1));
    busy = true;
    apps[0].on_sync_event(SyncEvent::SlotApplied, 0x01020003);  // the mesh changed something
    TEST_ASSERT_EQUAL(1, predicted_label(0, 1));                // still answers, with the older model
    busy = false;
    apps[0].set_lock(nullptr, nullptr);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hello_and_bad_lines);
    RUN_TEST(test_calibrate_and_predict);
    RUN_TEST(test_calibration_follows_you);
    RUN_TEST(test_delete_user_everywhere);
    RUN_TEST(test_gaze_session);
    RUN_TEST(test_slots_and_status);
    RUN_TEST(test_new_page_or_user_closes_old_sessions);
    RUN_TEST(test_mesh_monitor);
    RUN_TEST(test_predict_never_waits_for_the_mesh);
    return UNITY_END();
}
