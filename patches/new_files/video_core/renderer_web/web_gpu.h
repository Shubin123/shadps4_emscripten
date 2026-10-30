// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The browser's WebGPU device, as seen from the emulator's threads. The device itself lives on
// the page's main thread (emscripten/webgpu.js creates it before the runtime starts): work is
// recorded here as a Batch - a list of commands (JSON) plus an upload area of data copied out of
// guest memory - and handed to the page, which runs batches strictly in the order they were
// submitted. Submitting never blocks unless the caller asks to wait for results (readbacks), and
// waiting must happen on a pthread - the GPU command processor or any other emulator thread,
// never the browser main thread.
//
// The commands and what they reference (shader, pipeline and texture ids, all assigned on this
// side) are described in emscripten/webgpu.js.

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/types.h"

namespace Web::Gpu {

using Json = nlohmann::json;

// Optional device features the renderer adapts to (all false without a device).
struct Features {
    bool bc;                 // texture-compression-bc
    bool float32_filterable; // r32float & co. can be sampled with filtering
    bool depth_clip_control; // unclippedDepth
    bool depth32_stencil8;   // depth32float-stencil8
    bool dual_source;        // dual-source-blending
    bool rg11b10_renderable; // rg11b10ufloat-renderable
    u32 max_texture_size;    // maxTextureDimension2D
    u64 max_binding_size;    // maxStorageBufferBindingSize
};

// Logs `message` (as an error or a warning) the first time it comes up: binding problems and
// unsupported state recur on every draw.
void LogOnce(bool error, const std::string& message);

// True when there is a device (a browser with WebGPU, and the page didn't turn it off).
bool Available();
const Features& GetFeatures();

// Upload offsets are aligned for binding storage and uniform buffers at them.
constexpr u64 UploadAlignment = 256;

class Batch {
public:
    Batch();
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    // Copies `size` bytes into the upload area; returns where they are (UploadAlignment-aligned).
    u64 Upload(const void* data, u64 size);
    // Reserves `size` bytes in the upload area for the caller to fill; valid until the next
    // Upload/Allocate. Returns the offset; `*out` points at the space.
    u64 Allocate(u64 size, u8** out);
    u64 UploadSize() const {
        return arena.size();
    }

    void Add(Json command) {
        commands.push_back(std::move(command));
    }
    size_t NumCommands() const {
        return commands.size();
    }
    bool Empty() const {
        return commands.empty();
    }

    // Hands the batch to the page and starts a new one. With `wait`, blocks until the page has
    // run it and copied every readback into linear memory; false if the page reported an error.
    bool Submit(bool wait);

private:
    std::vector<Json> commands;
    std::vector<u8> arena;
};

} // namespace Web::Gpu
