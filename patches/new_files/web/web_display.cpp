// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "common/logging/log.h"
#include "web/web_display.h"

namespace Web::Display {

namespace {

// Two slots: the presenter fills whichever one the page isn't reading. `pending` is the slot index
// waiting to be drawn (+1, so 0 means none); the page clears it once it has copied the pixels.
struct Slot {
    std::vector<u32> pixels;
    u32 width{};
    u32 height{};
};
Slot slots[2];
std::mutex slot_mutex;
std::atomic<u32> pending{0};
std::atomic<u64> submitted{0};
u32 write_slot = 0;

void DumpPpm(const Slot& slot, u64 index) {
    static const char* dir = std::getenv("SHADPS4_FRAME_DUMP");
    if (dir == nullptr) {
        return;
    }
    // Every frame up to 8, then powers of two: enough to see whether output changes over time
    // without writing thousands of files.
    if (index > 8 && (index & (index - 1)) != 0) {
        return;
    }
    const std::string path = std::string(dir) + "/frame_" + std::to_string(index) + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", slot.width, slot.height);
    std::vector<u8> row(slot.width * 3);
    for (u32 y = 0; y < slot.height; y++) {
        for (u32 x = 0; x < slot.width; x++) {
            const u32 p = slot.pixels[y * slot.width + x];
            row[x * 3 + 0] = p & 0xFF;
            row[x * 3 + 1] = (p >> 8) & 0xFF;
            row[x * 3 + 2] = (p >> 16) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

} // namespace

extern "C" {

// Called on the page's main thread once it has drawn a slot, so the next frame may reuse it.
EMSCRIPTEN_KEEPALIVE void shadps4_display_release() {
    pending.store(0, std::memory_order_release);
}

} // extern "C"

void Submit(const u32* rgba, u32 width, u32 height, bool) {
    const u64 index = ++submitted;
    if (pending.load(std::memory_order_acquire) != 0) {
        // The page hasn't drawn the previous frame yet; drop this one rather than stall the
        // emulator on the browser's frame pacing.
        return;
    }
    Slot* slot;
    {
        std::scoped_lock lk{slot_mutex};
        slot = &slots[write_slot];
        slot->width = width;
        slot->height = height;
        slot->pixels.assign(rgba, rgba + static_cast<size_t>(width) * height);
        write_slot ^= 1;
    }
    DumpPpm(*slot, index);
    pending.store(1, std::memory_order_release);
    // Copied out of linear memory here, in Emscripten's own JS scope, where its heap views are
    // kept valid across memory growth; the page gets a plain RGBA array it can keep.
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            var ptr = Number($0);
            var len = $1 * $2 * 4;
            var cb = Module["shadps4OnFrame"];
            if (cb) {
                var rgba = new Uint8ClampedArray(len);
                rgba.set(HEAPU8.subarray(ptr, ptr + len));
                cb(rgba, $1, $2, Number($3));
            }
            _shadps4_display_release();
        },
        slot->pixels.data(), width, height, index);
}

u64 FramesSubmitted() {
    return submitted.load();
}

} // namespace Web::Display
