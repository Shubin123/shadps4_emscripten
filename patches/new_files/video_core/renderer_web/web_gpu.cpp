// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstddef>
#include <vector>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "common/logging/log.h"
#include "video_core/renderer_web/web_gpu.h"

namespace Web::Gpu {

namespace {

// Read field by field by Module.shadps4GpuCompute in emscripten/webgpu.js.
struct JsBinding {
    u64 ptr;
    u64 size;
    u32 binding;
    u32 writable;
};
static_assert(sizeof(JsBinding) == 24);

struct JsComputeJob {
    u64 wgsl_ptr;
    u64 wgsl_len;
    u64 key;
    u32 groups[3];
    u32 num_bindings;
    u64 bindings;
    u64 done;
};
static_assert(offsetof(JsComputeJob, bindings) == 40 && offsetof(JsComputeJob, done) == 48);

// Blocks until the page's job handler stores a nonzero status into `done`. There's no giving
// up: the handler writes into this thread's stack (the job, the done word) when it finishes.
u32 WaitForJob(std::atomic<u32>& done) {
    const double start = emscripten_get_now();
    double next_warning = 10000.0;
    u32 status;
    while ((status = done.load(std::memory_order_acquire)) == 0) {
        emscripten_futex_wait(&done, 0, 1000.0);
        if (const double elapsed = emscripten_get_now() - start; elapsed >= next_warning) {
            LOG_WARNING(Render, "WebGPU job still running after {:.0f}s", elapsed / 1000);
            next_warning += 10000.0;
        }
    }
    return status;
}

} // namespace

bool Available() {
    static const bool available = MAIN_THREAD_EM_ASM_INT({
        var gpu = Module["shadps4Gpu"];
        return gpu && gpu.device && Module["shadps4GpuCompute"] ? 1 : 0;
    }) != 0;
    return available;
}

bool RunCompute(const std::string& wgsl, u64 key, std::array<u32, 3> groups,
                std::span<const BufferBinding> buffers) {
    std::vector<JsBinding> list;
    list.reserve(buffers.size());
    for (const auto& b : buffers) {
        list.push_back({reinterpret_cast<u64>(b.data), b.size, b.binding, b.writable ? 1u : 0u});
    }
    alignas(8) std::atomic<u32> done{0};
    const JsComputeJob job{
        .wgsl_ptr = reinterpret_cast<u64>(wgsl.data()),
        .wgsl_len = wgsl.size(),
        .key = key,
        .groups = {groups[0], groups[1], groups[2]},
        .num_bindings = static_cast<u32>(list.size()),
        .bindings = reinterpret_cast<u64>(list.data()),
        .done = reinterpret_cast<u64>(&done),
    };
    MAIN_THREAD_ASYNC_EM_ASM({ Module["shadps4GpuCompute"](Number($0)); }, &job);
    return WaitForJob(done) == 1;
}

} // namespace Web::Gpu
