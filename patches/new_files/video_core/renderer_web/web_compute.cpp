// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <vector>
#include "common/logging/log.h"
#include "core/wasm_high_memory.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/renderer_web/web_compute.h"

namespace Web {

namespace {

// As PipelineCache::BuildRuntimeInfo does for HwStage::Compute.
void BuildRuntimeInfo(Shader::RuntimeInfo& info, const AmdGpu::ComputeProgram& cs) {
    info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    info.props.num_user_data = cs.settings.num_user_regs;
    info.props.num_allocated_vgprs = cs.settings.num_vgprs * 4;
    info.props.fp_denorm_mode32 = cs.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs.settings.fp_round_mode64;
    info.props.dx10_clamp = cs.settings.dx10_clamp;
    info.hw.cs.workgroup_size = {cs.num_thread_x.full, cs.num_thread_y.full, cs.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs.IsTgidEnabled(0), cs.IsTgidEnabled(1), cs.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs.SharedMemSize();
}

// Upstream's IsComputeMetaClear: a shader that writes render target metadata (CMASK/HTILE)
// without computing tiled addresses (no xor) is clearing it, and the dispatch is replaced by
// noting the clear.
bool IsMetaClear(MetaClears& meta, const Shader::Info& info) {
    for (const auto& desc : info.buffers) {
        if (!desc.IsSpecial() && !desc.is_written && meta.IsMeta(desc.GetSharp(info).base_address)) {
            return false;
        }
    }
    if (info.has_bitwise_xor) {
        return false;
    }
    for (const auto& desc : info.buffers) {
        if (!desc.IsSpecial() && desc.is_written && meta.Clear(desc.GetSharp(info).base_address)) {
            return true;
        }
    }
    return false;
}

} // namespace

ComputePipelines::ComputePipelines(Context& context) : ctx{context} {}
ComputePipelines::~ComputePipelines() = default;

bool ComputePipelines::Dispatch(const AmdGpu::ComputeProgram& cs, std::array<u32, 3> groups,
                                std::span<u32> gds) {
    if (!Gpu::Available() || groups[0] == 0 || groups[1] == 0 || groups[2] == 0) {
        return false;
    }
    static RuntimeInfoStorage runtime_storage;
    auto& runtime_info = runtime_storage.Get();
    BuildRuntimeInfo(runtime_info, cs);
    const auto params = AmdGpu::GetParams(cs);
    const auto shader = ctx.shaders.Get(Shader::HwStage::Compute, Shader::SwStage::Compute,
                                        params, runtime_info);
    if (shader.perm == nullptr) {
        return false;
    }
    if (IsMetaClear(ctx.meta, *shader.info)) {
        return true;
    }
    StageBindings bindings;
    if (!BindStage(ctx.batch, ctx.uploads, ctx.surfaces, *shader.info, shader.perm->out, gds,
                   bindings)) {
        return false;
    }
    auto& objects = GetPageObjects();
    const u32 module = objects.Module(ctx.batch, shader.perm->out.code);
    const u32 pipeline =
        objects.Pipeline(ctx.batch, {{"kind", "compute"}, {"module", module},
                                     {"layouts", {bindings.layout}}});
    ctx.batch.Add({{"op", "dispatch"},
                   {"pipeline", pipeline},
                   {"groups", {bindings.entries}},
                   {"count", {groups[0], groups[1], groups[2]}}});
    if (bindings.writebacks.empty()) {
        return true;
    }

    // Results land in linear memory directly, or in a copy scattered into the translated high
    // region (core/wasm_high_memory.h) afterwards.
    std::vector<std::pair<const Writeback*, std::vector<u8>>> high;
    for (const auto& wb : bindings.writebacks) {
        u64 ptr = wb.address;
        if (Core::HighMemory::InRange(wb.address)) {
            auto& copy = high.emplace_back(&wb, std::vector<u8>(wb.size)).second;
            ptr = reinterpret_cast<u64>(copy.data());
        }
        ctx.batch.Add({{"op", "readBuffer"}, {"offset", wb.offset}, {"size", wb.size},
                       {"ptr", ptr}});
        ctx.surfaces.Invalidate(wb.address, wb.size);
    }
    const bool ok = ctx.Submit(true);
    for (const auto& [wb, copy] : high) {
        using Core::HighMemory::kPageSize;
        for (u64 done = 0; ok && done < wb->size;) {
            const VAddr va = wb->address + done;
            const u64 n = std::min(wb->size - done, kPageSize - (va & (kPageSize - 1)));
            if (u8* page = Core::HighMemory::Translate(va)) {
                std::memcpy(page, copy.data() + done, n);
            }
            done += n;
        }
    }
    return ok;
}

} // namespace Web
