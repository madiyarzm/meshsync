// Visual playground: the simulator and the real meshstate library, compiled to WebAssembly.
// The page (index.html) calls these plain C functions. Build: web/playground/build.sh

#define SIM_NO_MAIN
#include "../../sim/sim.cpp"  // the simulator is one file; the playground drives its World

#include <emscripten/emscripten.h>

namespace {

sim::World world;
bool calibrating[MAX_DEVICES] = {};
uint32_t next_sample_ms = 0;
sim::World::PacketView packets[4096];
sim::FaultConfig faults = {0.20, 0.10, 0.01, 20, 120};  // slower than the tests, so packets are visible

constexpr uint32_t SAMPLE_EVERY_MS = 200;  // a calibrating person adds ~5 samples per second

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void pg_reset(int seed, int hubs) {
    world.reset(uint64_t(seed), hubs, faults);
    world.reseat_after_delete = false;  // a deleted person stops calibrating; nobody new appears
    for (int hub = 0; hub < MAX_DEVICES; hub++) {
        calibrating[hub] = false;
    }
    next_sample_ms = 0;
}

EMSCRIPTEN_KEEPALIVE void pg_step(int ms) {
    for (int done = 0; done < ms; done += int(sim::World::STEP_MS)) {
        if (world.now_ms() >= next_sample_ms) {
            for (int hub = 0; hub < world.num_hubs(); hub++) {
                if (calibrating[hub]) {
                    world.teach_random(hub);
                    calibrating[hub] = world.user_at(hub) != 0;  // stops if their data was deleted
                }
            }
            next_sample_ms = world.now_ms() + SAMPLE_EVERY_MS;
        }
        world.advance(sim::World::STEP_MS);
    }
}

EMSCRIPTEN_KEEPALIVE void pg_set_faults(int drop_percent, int duplicate_percent, int corrupt_percent) {
    faults.drop_rate = drop_percent / 100.0;
    faults.duplicate_rate = duplicate_percent / 100.0;
    faults.corrupt_rate = corrupt_percent / 100.0;
    world.set_faults(faults);
}

// --- network ---
EMSCRIPTEN_KEEPALIVE void pg_set_link(int a, int b, int on) { world.set_link(a, b, on != 0); }
EMSCRIPTEN_KEEPALIVE int pg_link(int a, int b) { return world.link(a, b) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void pg_heal() { world.heal(); }
EMSCRIPTEN_KEEPALIVE void pg_line() { world.set_line(); }
EMSCRIPTEN_KEEPALIVE void pg_isolate(int hub) {
    for (int other = 0; other < world.num_hubs(); other++) {
        world.set_link(hub, other, false);
    }
}
EMSCRIPTEN_KEEPALIVE void pg_split() {
    int groups[MAX_DEVICES];
    for (int hub = 0; hub < world.num_hubs(); hub++) {
        groups[hub] = hub < world.num_hubs() / 2 ? 0 : 1;
    }
    world.set_groups(groups);
}

// --- people and deletion ---
EMSCRIPTEN_KEEPALIVE void pg_calibrate(int hub, int on) {
    if (on) {
        calibrating[hub] = true;
        world.teach_random(hub);  // sits down now
        calibrating[hub] = world.user_at(hub) != 0;
    } else {
        calibrating[hub] = false;
        world.finish_user(hub);
    }
}
// Delete the person currently (or last) at `person_hub`, pressed at `pressed_at`. Returns their id.
EMSCRIPTEN_KEEPALIVE int pg_delete_person(int person_hub, int pressed_at) {
    uint16_t user = world.last_user_at(person_hub);
    if (user != 0) {
        world.delete_user(pressed_at, user);
    }
    return user;
}
EMSCRIPTEN_KEEPALIVE int pg_delete_slot(int hub) { return world.delete_random_slot(hub) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void pg_reboot(int hub) {
    calibrating[hub] = false;
    world.reboot(hub);
}

// --- reading state ---
EMSCRIPTEN_KEEPALIVE int pg_now() { return int(world.now_ms()); }
EMSCRIPTEN_KEEPALIVE int pg_hubs() { return world.num_hubs(); }
EMSCRIPTEN_KEEPALIVE unsigned pg_digest(int hub) { return world.replica(hub).digest(); }
EMSCRIPTEN_KEEPALIVE int pg_slots(int hub) { return world.replica(hub).slot_count(); }
EMSCRIPTEN_KEEPALIVE int pg_tombstones(int hub) { return world.replica(hub).tombstone_count(); }
EMSCRIPTEN_KEEPALIVE int pg_deleted_users(int hub) { return world.replica(hub).user_count(); }
EMSCRIPTEN_KEEPALIVE int pg_user(int hub) { return calibrating[hub] ? world.user_at(hub) : 0; }
EMSCRIPTEN_KEEPALIVE int pg_samples(int hub) { return int(world.replica(hub).gaze_totals(false).n()); }
EMSCRIPTEN_KEEPALIVE int pg_syncing(int hub) { return world.engine(hub).syncing() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int pg_converged() { return world.converged() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int pg_resurrected() { return world.resurrected_slots(); }
EMSCRIPTEN_KEEPALIVE int pg_lost() { return world.wrongly_lost_slots(); }

// Writes [sent, delivered, dropped, blocked, duplicated, corrupted].
EMSCRIPTEN_KEEPALIVE void pg_radio(int* out) {
    const sim::RadioStats& radio = world.radio_stats();
    out[0] = int(radio.sent);
    out[1] = int(radio.delivered);
    out[2] = int(radio.dropped);
    out[3] = int(radio.blocked);
    out[4] = int(radio.duplicated);
    out[5] = int(radio.corrupted);
}

// Writes 5 ints per packet: from, to, type, sent_ms, deliver_ms. Returns the count.
EMSCRIPTEN_KEEPALIVE int pg_packets(int* out, int capacity) {
    int count = world.packets_in_flight(packets, capacity < 4096 ? capacity : 4096);
    for (int i = 0; i < count; i++) {
        out[i * 5 + 0] = packets[i].from;
        out[i * 5 + 1] = packets[i].to;
        out[i * 5 + 2] = packets[i].type;
        out[i * 5 + 3] = int(packets[i].sent_ms);
        out[i * 5 + 4] = int(packets[i].deliver_ms);
    }
    return count;
}

// Event i (0 = newest): writes time_ms, hub, kind (0 slot applied, 1 tombstone, 2 user deleted), id.
EMSCRIPTEN_KEEPALIVE int pg_event_count() { return world.event_count(); }
EMSCRIPTEN_KEEPALIVE void pg_event(int i, int* out) {
    const sim::World::EventView& event = world.event(i);
    out[0] = int(event.time_ms);
    out[1] = event.hub;
    out[2] = event.kind;
    out[3] = int(event.id);
}

}  // extern "C"
