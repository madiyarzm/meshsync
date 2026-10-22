#pragma once

// SlotStore on the ESP32's flash (LittleFS). One file per slot, /s/<key in hex>.bin, plus
// /meta.bin for tombstones, user deletion counters, and the session counter.
// Every write goes to a temp file first and is then renamed, so a power cut mid-write
// leaves the old copy intact.

#include <LittleFS.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "src/meshstate/replica.h"

class FlashStore : public meshstate::SlotStore {
public:
    bool begin() {
        if (!LittleFS.begin(true)) {  // true: format on first use
            return false;
        }
        if (!LittleFS.exists("/s")) {
            LittleFS.mkdir("/s");
        }
        return true;
    }

    int list(uint32_t* keys, int capacity) override {
        int count = 0;
        File dir = LittleFS.open("/s");
        if (!dir) {
            return 0;
        }
        File file = dir.openNextFile();
        while (file && count < capacity) {
            const char* name = file.name();  // "0102000a.bin"
            const char* slash = std::strrchr(name, '/');
            if (slash != nullptr) {
                name = slash + 1;
            }
            // ".tmp" too: a crash between writing and renaming leaves only the temp file.
            if (std::strlen(name) == 12 && (std::strcmp(name + 8, ".bin") == 0 || std::strcmp(name + 8, ".tmp") == 0)) {
                uint32_t key = uint32_t(std::strtoul(name, nullptr, 16));
                bool seen = false;
                for (int i = 0; i < count; i++) {
                    seen = seen || keys[i] == key;
                }
                if (!seen) {
                    keys[count++] = key;
                }
            }
            file.close();
            file = dir.openNextFile();
        }
        return count;
    }

    size_t load(uint32_t key, uint8_t* out, size_t capacity) override {
        char path[24];
        slot_path(path, sizeof(path), key, false);
        size_t length = read_file(path, out, capacity);
        if (length == 0) {
            slot_path(path, sizeof(path), key, true);
            length = read_file(path, out, capacity);
        }
        return length;
    }

    bool save(uint32_t key, const uint8_t* data, size_t length) override {
        char path[24];
        char temp[24];
        slot_path(path, sizeof(path), key, false);
        slot_path(temp, sizeof(temp), key, true);
        return write_file(temp, path, data, length);
    }

    bool erase(uint32_t key) override {
        char path[24];
        slot_path(path, sizeof(path), key, false);
        if (LittleFS.exists(path)) {
            LittleFS.remove(path);
        }
        slot_path(path, sizeof(path), key, true);
        if (LittleFS.exists(path)) {
            LittleFS.remove(path);
        }
        return true;
    }

    size_t load_meta(uint8_t* out, size_t capacity) override {
        size_t length = read_file("/meta.bin", out, capacity);
        if (length == 0) {
            length = read_file("/meta.tmp", out, capacity);  // crash between write and rename
        }
        return length;
    }

    bool save_meta(const uint8_t* data, size_t length) override {
        return write_file("/meta.tmp", "/meta.bin", data, length);
    }

private:
    static void slot_path(char* out, size_t size, uint32_t key, bool temporary) {
        std::snprintf(out, size, "/s/%08lx.%s", (unsigned long)key, temporary ? "tmp" : "bin");
    }

    static size_t read_file(const char* path, uint8_t* out, size_t capacity) {
        if (!LittleFS.exists(path)) {
            return 0;
        }
        File file = LittleFS.open(path, "r");
        if (!file) {
            return 0;
        }
        size_t size = file.size();
        size_t length = size <= capacity ? file.read(out, size) : 0;
        file.close();
        return length == size ? length : 0;
    }

    static bool write_file(const char* temp, const char* path, const uint8_t* data, size_t length) {
        File file = LittleFS.open(temp, "w");
        if (!file) {
            return false;
        }
        size_t written = file.write(data, length);
        file.close();
        if (written != length) {
            return false;
        }
        LittleFS.remove(path);
        return LittleFS.rename(temp, path);
    }
};
