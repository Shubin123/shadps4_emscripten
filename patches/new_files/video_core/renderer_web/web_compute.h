// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest compute dispatches on WebGPU. The GCN program goes through the shader recompiler and the
// WGSL backend (web_shaders.h), its resources are bound like a draw's (web_resources.h), and the
// dispatch is recorded into the context's batch. A dispatch that writes guest buffers is waited
// for, with the written buffers copied back, so the guest sees the results as soon as the
// command processor moves on; one that only reads (or writes nothing the CPU can see) isn't.

#pragma once

#include <array>
#include <span>
#include "common/types.h"
#include "video_core/renderer_web/web_context.h"

namespace AmdGpu {
struct ComputeProgram;
}

namespace Web {

class ComputePipelines {
public:
    explicit ComputePipelines(Context& context);
    ~ComputePipelines();

    // Runs `cs` over `groups` workgroups. False if the dispatch was dropped: no device, or the
    // shader uses something the WGSL backend or the binding code doesn't handle yet (logged
    // once per shader). `gds` backs the Global Data Share buffer type.
    bool Dispatch(const AmdGpu::ComputeProgram& cs, std::array<u32, 3> groups, std::span<u32> gds);

private:
    Context& ctx;
};

} // namespace Web
