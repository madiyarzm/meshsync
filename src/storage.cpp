#include "storage.h"

#include <LittleFS.h>

#include <cstdio>

#include "bytes.h"

namespace {

constexpr uint32_t FILE_MAGIC = 0x544F4C53;  // "SLOT"
constexpr size_t FILE_HEADER_BYTES = 12;

uint8_t file_buffer[FILE_HEADER_BYTES + construct::MAX_ENCODED_SLOT_BYTES];

void slot_path(char* out, size_t size, int owner, bool temporary) {
    std::snprintf(out, size, "/slot_%d.%s", owner, temporary ? "tmp" : "bin");
}

bool load_file(const char* path, const construct::SlotCodec& codec, construct::Slot& out) {
    if (!LittleFS.exists(path)) {
        return false;
    }
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }
    size_t length = file.read(file_buffer, sizeof(file_buffer));
    file.close();
    if (length < FILE_HEADER_BYTES) {
        return false;
    }
    const uint8_t* in = file_buffer;
    uint32_t magic = construct::read_u32(in);
    uint32_t encoded_length = construct::read_u32(in);
    uint32_t crc = construct::read_u32(in);
    if (magic != FILE_MAGIC || encoded_length != length - FILE_HEADER_BYTES) {
        return false;  // truncated, e.g. power was cut while writing
    }
    if (construct::crc32(in, encoded_length) != crc) {
        return false;
    }
    return codec.decode(in, encoded_length, out);
}

}  // namespace

bool storage_begin() {
    return LittleFS.begin(true);  // true: format on first use
}

int storage_load_all(construct::Node& node, const construct::SlotCodec& codec) {
    int loaded = 0;
    char path[24];
    for (int owner = 0; owner < construct::MAX_NODES; owner++) {
        // Decoding straight into the slot is safe here: at boot it is empty, and a failed
        // decode is cleared again below.
        construct::Slot& slot = node.slots[owner];
        slot_path(path, sizeof(path), owner, false);
        bool ok = load_file(path, codec, slot);
        if (!ok) {
            // A crash between "write temp" and "rename" leaves only the temp file.
            slot_path(path, sizeof(path), owner, true);
            ok = load_file(path, codec, slot);
        }
        if (ok && slot.version > 0) {
            loaded++;
        } else {
            slot.clear();
        }
    }
    return loaded;
}

bool storage_save_slot(const construct::Node& node, int owner, const construct::SlotCodec& codec) {
    if (owner < 0 || owner >= construct::MAX_NODES) {
        return false;
    }
    uint8_t* encoded = file_buffer + FILE_HEADER_BYTES;
    size_t encoded_length = codec.encode(node.slots[owner], encoded, construct::MAX_ENCODED_SLOT_BYTES);
    if (encoded_length == 0) {
        return false;
    }
    uint8_t* out = file_buffer;
    construct::write_u32(out, FILE_MAGIC);
    construct::write_u32(out, uint32_t(encoded_length));
    construct::write_u32(out, construct::crc32(encoded, encoded_length));

    // Write a temp file, then swap it in, so the old copy survives a power cut mid-write.
    char temp_path[24];
    char final_path[24];
    slot_path(temp_path, sizeof(temp_path), owner, true);
    slot_path(final_path, sizeof(final_path), owner, false);
    File file = LittleFS.open(temp_path, "w");
    if (!file) {
        return false;
    }
    size_t total = FILE_HEADER_BYTES + encoded_length;
    size_t written = file.write(file_buffer, total);
    file.close();
    if (written != total) {
        return false;
    }
    LittleFS.remove(final_path);
    return LittleFS.rename(temp_path, final_path);
}
