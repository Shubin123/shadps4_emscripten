// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest compute dispatches on WebGPU. The GCN program goes through the shader recompiler and the
// WGSL backend (cached per program and specialization, the way vk_pipeline_cache keeps SPIR-V
// permutations), then runs over the guest buffers it uses: each is uploaded from guest memory
// before the dispatch and the written ones are copied back after it, so the guest sees the
// results as soon as the dispatch returns. That costs a round trip per dispatch - a GPU-resident
// buffer cache using the address space's dirty tracking is the next step.

#pragma once

#include <array>
#include <memory>
#include <span>
#include <unordered_map>
#include "common/types.h"

namespace AmdGpu {
struct ComputeProgram;
}

namespace Web {

class ComputePipelines {
public:
    ComputePipelines();
    ~ComputePipelines();

    // Runs `cs` over `groups` workgroups. False if the dispatch was dropped: no device, or the
    // shader uses something the WGSL backend or this binding code doesn't handle yet (logged
    // once per shader). `gds` backs the Global Data Share buffer type.
    bool Dispatch(const AmdGpu::ComputeProgram& cs, std::array<u32, 3> groups, std::span<u32> gds);

private:
    struct Program;
    struct Permutation;
    std::unordered_map<u64, std::unique_ptr<Program>> programs;
};

} // namespace Web
