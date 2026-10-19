#pragma once

#include "core.h"

// Keeps slots in flash (LittleFS) so a rebooted hub remembers everything.
// One file per slot: /slot_N.bin = [magic][length][crc32][encoded slot].

bool storage_begin();

// Loads every saved slot into a freshly initialized node (call once at boot).
// Damaged files are skipped and leave that slot empty. Returns how many loaded.
int storage_load_all(construct::Node& node, const construct::SlotCodec& codec);

bool storage_save_slot(const construct::Node& node, int owner, const construct::SlotCodec& codec);
