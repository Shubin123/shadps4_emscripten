// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "video_core/renderer_web/web_gpu.h"

namespace Web::Gpu {

namespace {

// Read field by field by Module.shadps4GpuSubmit in emscripten/webgpu.js. The block it points
// into (this header, the JSON, then the upload area) is freed by the page once it's done.
struct JsBatch {
    u64 json;
    u64 json_size;
    u64 upload;
    u64 upload_size;
    u64 done; // u32 status word, or 0 when nobody waits
};
static_assert(sizeof(JsBatch) == 40);

// Blocks until the page's batch handler stores a nonzero status into `done`. There's no giving
// up: the handler writes into this thread's stack when it finishes.
u32 WaitForBatch(std::atomic<u32>& done) {
    const double start = emscripten_get_now();
    double next_warning = 10000.0;
    u32 status;
    while ((status = done.load(std::memory_order_acquire)) == 0) {
        emscripten_futex_wait(&done, 0, 1000.0);
        if (const double elapsed = emscripten_get_now() - start; elapsed >= next_warning) {
            LOG_WARNING(Render, "WebGPU batch still running after {:.0f}s", elapsed / 1000);
            next_warning += 10000.0;
        }
    }
    return status;
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void shadps4_gpu_release(void* block) {
    std::free(block);
}

void LogOnce(bool error, const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::scoped_lock lk{mutex};
    if (!seen.insert(message).second) {
        return;
    }
    if (error) {
        LOG_ERROR(Render, "{}", message);
    } else {
        LOG_WARNING(Render, "{}", message);
    }
}

bool Available() {
    static const bool available = MAIN_THREAD_EM_ASM_INT({
        var gpu = Module["shadps4Gpu"];
        return gpu && gpu.device && Module["shadps4GpuSubmit"] ? 1 : 0;
    }) != 0;
    return available;
}

const Features& GetFeatures() {
    static const Features features = [] {
        Features f{};
        if (!Available()) {
            return f;
        }
        u32 words[8]{};
        MAIN_THREAD_EM_ASM(
            {
                var d = Module["shadps4Gpu"].device;
                var w = Number($0) / 4;
                HEAPU32[w + 0] = d.features.has("texture-compression-bc") ? 1 : 0;
                HEAPU32[w + 1] = d.features.has("float32-filterable") ? 1 : 0;
                HEAPU32[w + 2] = d.features.has("depth-clip-control") ? 1 : 0;
                HEAPU32[w + 3] = d.features.has("depth32float-stencil8") ? 1 : 0;
                HEAPU32[w + 4] = d.features.has("dual-source-blending") ? 1 : 0;
                HEAPU32[w + 5] = d.limits.maxTextureDimension2D;
                HEAPU32[w + 6] = Math.min(d.limits.maxStorageBufferBindingSize, 4294967295);
                HEAPU32[w + 7] = d.features.has("rg11b10ufloat-renderable") ? 1 : 0;
            },
            words);
        f.bc = words[0] != 0;
        f.float32_filterable = words[1] != 0;
        f.depth_clip_control = words[2] != 0;
        f.depth32_stencil8 = words[3] != 0;
        f.dual_source = words[4] != 0;
        f.max_texture_size = words[5];
        f.max_binding_size = words[6];
        f.rg11b10_renderable = words[7] != 0;
        LOG_INFO(Render, "WebGPU features: bc={} float32-filterable={} depth-clip-control={} "
                         "depth32float-stencil8={} dual-source={} max texture {} max binding {}",
                 f.bc, f.float32_filterable, f.depth_clip_control, f.depth32_stencil8,
                 f.dual_source, f.max_texture_size, f.max_binding_size);
        return f;
    }();
    return features;
}

Batch::Batch() = default;
Batch::~Batch() = default;

u64 Batch::Allocate(u64 size, u8** out) {
    const u64 offset = Common::AlignUp(arena.size(), UploadAlignment);
    arena.resize(offset + Common::AlignUp(std::max<u64>(size, 4), 4));
    *out = arena.data() + offset;
    return offset;
}

u64 Batch::Upload(const void* data, u64 size) {
    u8* dst;
    const u64 offset = Allocate(size, &dst);
    std::memcpy(dst, data, size);
    return offset;
}

bool Batch::Submit(bool wait) {
    if (commands.empty()) {
        arena.clear();
        return true;
    }
    const std::string json = Json(std::move(commands)).dump(-1, ' ', false,
                                                            Json::error_handler_t::replace);
    commands.clear();
    const u64 json_offset = sizeof(JsBatch);
    const u64 upload_offset = Common::AlignUp(json_offset + json.size(), 16);
    auto* block = static_cast<u8*>(std::malloc(upload_offset + arena.size()));
    alignas(8) std::atomic<u32> done{0};
    JsBatch header{
        .json = reinterpret_cast<u64>(block + json_offset),
        .json_size = json.size(),
        .upload = reinterpret_cast<u64>(block + upload_offset),
        .upload_size = arena.size(),
        .done = wait ? reinterpret_cast<u64>(&done) : 0,
    };
    std::memcpy(block, &header, sizeof(header));
    std::memcpy(block + json_offset, json.data(), json.size());
    if (!arena.empty()) {
        std::memcpy(block + upload_offset, arena.data(), arena.size());
    }
    arena.clear();
    MAIN_THREAD_ASYNC_EM_ASM({ Module["shadps4GpuSubmit"](Number($0)); }, block);
    return !wait || WaitForBatch(done) == 1;
}

} // namespace Web::Gpu
