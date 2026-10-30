// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A shader stage's resources, bound for one dispatch or draw: what vk_rasterizer's
// BindBuffers/BindTextures do, producing webgpu.js bind group entries. Guest buffers are copied
// into the batch's upload area (once per batch for the same range), textures come from Surfaces,
// samplers are described for the page to create.

#pragma once

#include <map>
#include <optional>
#include <span>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/backend/wgsl/emit_wgsl.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_web/web_gpu.h"
#include "video_core/renderer_web/web_surfaces.h"

namespace Web {

// Guest memory a dispatch or draw writes, copied back after the batch runs.
struct Writeback {
    u64 offset; // in the batch's upload area
    u64 size;
    VAddr address;
};

struct StageBindings {
    Gpu::Json layout = Gpu::Json::array();  // binding descriptions, for the pipeline layout
    Gpu::Json entries = Gpu::Json::array(); // what is bound, for this draw or dispatch
    std::vector<Writeback> writebacks;
};

// Upload-area copies of guest buffers, shared by every binding of the same range in a batch.
class Uploads {
public:
    // Where [address, address + size) is in the batch's upload area; nullopt if it isn't guest
    // memory.
    std::optional<u64> Guest(Gpu::Batch& batch, VAddr address, u64 size);
    u64 Host(Gpu::Batch& batch, const void* data, u64 size);
    // Guest memory changed (DMA, a dispatch's results): later bindings copy it again.
    void Forget(VAddr address, u64 size);
    // A new batch: nothing is uploaded yet.
    void Reset() {
        ranges.clear();
    }

private:
    std::map<std::pair<VAddr, u64>, u64> ranges;
};

// Binds every resource `out` declares, read from `info`. False if one can't be bound (logged).
// `clip_planes` (vertex shaders: the user clip planes, 6 x vec4) backs the ClipPlanes buffer.
bool BindStage(Gpu::Batch& batch, Uploads& uploads, Surfaces& surfaces, const Shader::Info& info,
               const Shader::Backend::WGSL::Output& out, std::span<u32> gds,
               StageBindings& bindings, std::span<const float> clip_planes = {});

} // namespace Web
