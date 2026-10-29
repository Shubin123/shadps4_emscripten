// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The browser's WebGPU device, as seen from the emulator's threads. The device itself lives on
// the page's main thread (emscripten/webgpu.js creates it before the runtime starts); calls here
// hand it a job and wait for the result, so they must come from a pthread - the GPU command
// processor or any other emulator thread, never the browser main thread.

#pragma once

#include <array>
#include <span>
#include <string>
#include "common/types.h"

namespace Web::Gpu {

struct BufferBinding {
    u32 binding;   // @binding in @group(0)
    void* data;    // host memory backing the buffer
    u64 size;      // bytes
    bool writable; // copied back into data after the dispatch
};

// True when there is a device (a browser with WebGPU, and the page didn't turn it off).
bool Available();

// Runs a WGSL compute shader (entry point "main", all resources in @group(0)) over the given
// buffers and waits for it, including the copy back of every writable buffer. `key` identifies
// the shader text, for the page's pipeline cache. False if the device rejected the shader or
// the dispatch (the page's console has the details).
bool RunCompute(const std::string& wgsl, u64 key, std::array<u32, 3> groups,
                std::span<const BufferBinding> buffers);

} // namespace Web::Gpu
