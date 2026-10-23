#pragma once

// Face mode (APP_FACE): the text protocol between the browser page and its hub.
// Portable, like the library: the firmware feeds it lines from USB serial, and the browser's
// simulated hub (web/playground/playground.cpp) feeds it lines from JavaScript.
//
// Lines end in '\n', fields are separated by spaces, numbers are plain integers (features x1000).
// Every request starts with an id chosen by the browser, and the reply repeats it:
//
//   <id> HELLO 1                           -> <id> OK HELLO <letter> <protocol> <max_slots>
//   <id> USER <user>                       -> <id> OK USER <user> <deletion counter>
//     (HELLO and USER close and save the sessions opened before, so a page that went away
//      without closing can't use up the hub's open sessions)
//   <id> SESSION gest_blend|gest_lmk|gaze  -> <id> OK SESSION <session>
//   <id> ADD <session> <label> f1..f12     -> <id> OK ADD <version>          (gesture, label 0-2)
//   <id> ADD <session> <tx> <ty> f1..f12   -> <id> OK ADD <version>          (gaze, targets 0-1000)
//   <id> CLOSE <session>                   -> <id> OK CLOSE <version>
//   <id> PREDICT gest_blend|gest_lmk f1..f12 -> <id> OK PREDICT <type> <label> <p0> <p1> <p2> <own> <others>
//   <id> PREDICT gaze f1..f12              -> <id> OK PREDICT gaze <x> <y> <cell> <own> <others>
//   <id> LAMBDA <lambda x1000>             -> <id> OK LAMBDA <lambda x1000>
//   <id> DELETE_USER <user>                -> <id> OK DELETE_USER <slots removed> <deletion counter>
//   <id> STATUS                            -> <id> OK STATUS key=value ...
//   <id> SLOTS                             -> <id> SLOT <key> <owner> <type> <session> <user> <samples>, then <id> OK SLOTS <count>
//   <id> TIMING [RESET]                    -> <id> OK TIMING idle=<n>,<avg_us>,<max_us> sync=<n>,<avg_us>,<max_us>
//   <id> BENCH                             -> <id> OK BENCH, then the board restarts and prints #BENCH lines
//   errors                                 -> <id> ERR <CODE>
//
// Unprompted lines use id 0:
//   0 BOOT <letter> <protocol> <app>       after every reset
//   0 EVENT SLOT <key> / TOMBSTONE <key> / USER_DELETED <user> <counter>   from the mesh
//   0 EVENT MESH <digest> <entries> <slots> <tombstones> <syncing> <tx_packets> <tx_bytes>   every second
//   0 EVENT PEER <letter> <app> <online> <age_ms> <rssi> <packets> <bytes> <hb> <inv> <req> <chunk> <digest> <entries>
//     every second for each board heard in the last minute; counts cover the last second, rssi 0 = unknown
// Lines starting with '#' are logs; the page ignores them.

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "src/meshstate/models.h"
#include "src/meshstate/replica.h"
#include "src/meshstate/sync.h"

constexpr int FACE_PROTOCOL = 1;
constexpr size_t FACE_MAX_LINE = 255;    // longest request line
constexpr size_t FACE_MAX_OUTPUT = 320;  // longest line the hub writes
constexpr uint32_t FACE_REPORT_MS = 1000;
constexpr uint32_t FACE_PEER_FORGET_MS = 60000;

class FaceApp {
public:
    using WriteLine = void (*)(void* context, const char* line);  // line without '\n'
    using Micros = uint32_t (*)();
    // lock(context, true) waits for the hub state; lock(context, false) gives up if it's busy.
    using Lock = bool (*)(void* context, bool wait);
    using Unlock = void (*)(void* context);

    void begin(char letter, uint8_t app, meshstate::Replica* replica, const meshstate::SyncEngine* sync,
               WriteLine write, Micros micros, void* context);
    // Optional: only needed when the sync side runs on another core (the ESP32).
    void set_lock(Lock lock, Unlock unlock);
    void boot();  // writes "0 BOOT ..."

    // --- browser side: one request line (changed in place while parsing) --------------
    void handle_line(char* line, uint32_t now_ms);
    bool take_bench_request();               // true once after "<id> BENCH"
    bool led_on(uint32_t now_ms) const;      // short flash after a gesture is detected

    // --- mesh side: call with the hub state locked ----------------------------------------
    void on_sync_event(meshstate::SyncEvent event, uint32_t id);
    void on_radio_packet(const uint8_t* data, size_t length, int rssi, uint32_t now_ms);
    void on_radio_send(const uint8_t* data, size_t length);
    void tick(uint32_t now_ms);  // EVENT MESH / PEER lines once a second

private:
    struct PeerTraffic {
        bool heard;
        uint8_t app;
        uint32_t last_ms;
        int rssi_sum;
        int rssi_samples;
        int last_rssi;
        uint32_t packets;
        uint32_t bytes;
        uint32_t by_type[6];  // index = PacketType
    };
    struct Timing {
        uint32_t count;
        uint64_t total_us;
        uint32_t max_us;
    };

    void reply(uint16_t id, const char* format, ...);
    void error(uint16_t id, const char* code);
    void event(const char* format, ...);
    void write_line(char* buffer, size_t capacity, const char* format, va_list args);
    bool lock_state(bool wait);
    void unlock_state();
    bool find_session(uint16_t session, meshstate::SlotKey& key) const;
    void close_opened_sessions();
    void refresh_model(meshstate::PayloadType type);

    void cmd_user(uint16_t id, char** args, int count);
    void cmd_session(uint16_t id, char** args, int count);
    void cmd_add(uint16_t id, char** args, int count);
    void cmd_close(uint16_t id, char** args, int count);
    void cmd_predict(uint16_t id, char** args, int count, uint32_t now_ms);
    void cmd_lambda(uint16_t id, char** args, int count);
    void cmd_delete_user(uint16_t id, char** args, int count);
    void cmd_status(uint16_t id);
    void cmd_slots(uint16_t id);
    void cmd_timing(uint16_t id, char** args, int count);

    char letter_ = 'A';
    uint8_t app_ = 0;
    meshstate::Replica* replica_ = nullptr;
    const meshstate::SyncEngine* sync_ = nullptr;
    WriteLine write_ = nullptr;
    Micros micros_ = nullptr;
    Lock lock_ = nullptr;
    Unlock unlock_ = nullptr;
    void* context_ = nullptr;

    // Models are rebuilt from the totals when something changed since the last fit. The two
    // counters are written by different sides (browser side / mesh side), so neither races.
    uint32_t local_changes_ = 1;
    volatile uint32_t mesh_changes_ = 0;
    uint32_t fitted_local_[3] = {};  // per model: gest_blend, gest_lmk, gaze
    uint32_t fitted_mesh_[3] = {};
    meshstate::GestureModel gesture_models_[2];
    meshstate::GazeModel gaze_model_;
    double lambda_ = 1.0;

    meshstate::SlotKey opened_[meshstate::MAX_OPEN_SESSIONS] = {};
    int opened_count_ = 0;

    int last_label_ = -1;
    uint32_t led_until_ms_ = 0;
    bool bench_requested_ = false;
    Timing timing_idle_ = {};
    Timing timing_sync_ = {};

    PeerTraffic peers_[meshstate::MAX_DEVICES] = {};
    uint32_t tx_packets_ = 0;
    uint32_t tx_bytes_ = 0;
    uint32_t last_report_ms_ = 0;

    char reply_buffer_[FACE_MAX_OUTPUT];  // browser side
    char event_buffer_[FACE_MAX_OUTPUT];  // mesh side (may run on the other core)
};
