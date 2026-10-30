// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "common/logging/log.h"
#include "core/wasm_high_memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_web/web_gpu.h"
#include "video_core/renderer_web/web_rasterizer.h"

namespace Vulkan {

namespace {

// A batch is sent early once it holds this much, rather than at the end of the submit.
constexpr u64 MaxBatchUpload = 64_MB;
constexpr size_t MaxBatchCommands = 4096;

// Indirect arguments and counts are read straight from guest memory.
const u8* GuestArgs(VAddr address, u64 size) {
    if (Core::HighMemory::InRange(address)) {
        return Core::HighMemory::Translate(address);
    }
    return address != 0 && address + size <= 16_GB ? reinterpret_cast<const u8*>(address)
                                                    : nullptr;
}

} // namespace

Rasterizer::Rasterizer(AmdGpu::Liverpool* liverpool_) : liverpool{liverpool_} {
    liverpool->BindRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::DrawOne(const Web::DrawParams& params) {
    stats.draws++;
    const auto& regs = liverpool->regs;
    using Mode = AmdGpu::ColorControl::OperationMode;
    switch (regs.color_control.mode) {
    case Mode::EliminateFastClear:
    case Mode::FmaskDecompress:
        graphics.EliminateFastClear(*liverpool);
        return;
    case Mode::Resolve:
        graphics.Resolve(*liverpool);
        return;
    default:
        break;
    }
    if (graphics.Draw(*liverpool, params)) {
        stats.draws_run++;
    } else if (stats.draws - stats.draws_run == 1) {
        LOG_WARNING(Render, "first dropped guest draw ({})",
                    Web::Gpu::Available() ? "see above" : "no WebGPU device");
    }
    if (context.batch.UploadSize() > MaxBatchUpload ||
        context.batch.NumCommands() > MaxBatchCommands) {
        context.Submit(false);
    }
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    DrawOne({.indexed = is_indexed, .index_offset = index_offset});
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    u32 count = max_count;
    if (count_address != 0) {
        if (const u8* p = GuestArgs(count_address, 4)) {
            u32 n;
            std::memcpy(&n, p, 4);
            count = std::min(count, n);
        }
    }
    // The CP writes the base vertex and instance into the vertex shader's user data, where the
    // fetch shader adds them (UdRegFromShOffset in the recompiler: VS registers start at 0x4c).
    auto& user_data = liverpool->regs.vs_program.user_data;
    const auto set_sgpr = [&](u16 sh_offset, u32 value) {
        if (sh_offset >= 0x4c && sh_offset - 0x4cu < user_data.size()) {
            user_data[sh_offset - 0x4c] = value;
        }
    };
    for (u32 i = 0; i < count; i++) {
        const VAddr args = arg_address + offset + u64(i) * stride;
        if (is_indexed) {
            AmdGpu::DrawIndexedIndirectArgs a{};
            const u8* p = GuestArgs(args, sizeof(a));
            if (p == nullptr) {
                return;
            }
            std::memcpy(&a, p, sizeof(a));
            set_sgpr(vertex_sgpr_offset, a.base_vertex_location);
            set_sgpr(instance_sgpr_offset, a.start_instance_location);
            DrawOne({.indexed = true,
                     .index_offset = a.start_index_location,
                     .count = a.index_count_per_instance,
                     .instances = a.instance_count});
        } else {
            AmdGpu::DrawIndirectArgs a{};
            const u8* p = GuestArgs(args, sizeof(a));
            if (p == nullptr) {
                return;
            }
            std::memcpy(&a, p, sizeof(a));
            set_sgpr(vertex_sgpr_offset, a.start_vertex_location);
            set_sgpr(instance_sgpr_offset, a.start_instance_location);
            DrawOne({.indexed = false,
                     .index_offset = 0,
                     .count = a.vertex_count_per_instance,
                     .instances = a.instance_count});
        }
    }
}

void Rasterizer::DispatchDirect() {
    const auto& cs = liverpool->GetCsRegs();
    Dispatch(cs, {cs.dim_x, cs.dim_y, cs.dim_z});
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    // The arguments are three dwords of guest memory: the workgroup counts.
    std::array<u32, 3> groups{};
    const u8* host = GuestArgs(address + offset, sizeof(groups));
    if (host != nullptr && size >= sizeof(groups)) {
        std::memcpy(groups.data(), host, sizeof(groups));
    }
    Dispatch(liverpool->GetCsRegs(), groups);
}

void Rasterizer::Dispatch(const AmdGpu::ComputeProgram& cs, std::array<u32, 3> groups) {
    if (compute.Dispatch(cs, groups, gds)) {
        stats.dispatches_run++;
    } else if (stats.dispatches - stats.dispatches_run == 0) {
        LOG_WARNING(Render, "first dropped guest compute dispatch ({})",
                    Web::Gpu::Available() ? "see the shader's errors above" : "no WebGPU device");
    }
    stats.dispatches++;
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    stats.dma_fills++;
    if (is_gds) {
        const u32 first = static_cast<u32>(address / sizeof(u32));
        for (u32 i = 0; i < num_bytes / sizeof(u32) && first + i < gds.size(); i++) {
            gds[first + i] = value;
        }
        return;
    }
    // Fills of render target metadata are fast clears (MetaClears).
    context.meta.NoteFill(address);
    auto* dst = reinterpret_cast<u32*>(address);
    for (u32 i = 0; i < num_bytes / sizeof(u32); i++) {
        dst[i] = value;
    }
    context.uploads.Forget(address, num_bytes);
    context.surfaces.Invalidate(address, num_bytes);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    stats.dma_copies++;
    u8* dst_ptr = dst_gds ? reinterpret_cast<u8*>(gds.data()) + dst : reinterpret_cast<u8*>(dst);
    const u8* src_ptr =
        src_gds ? reinterpret_cast<const u8*>(gds.data()) + src : reinterpret_cast<const u8*>(src);
    std::memmove(dst_ptr, src_ptr, num_bytes);
    if (!dst_gds) {
        context.uploads.Forget(dst, num_bytes);
        context.surfaces.Invalidate(dst, num_bytes);
    }
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    return gds[(gds_offset / sizeof(u32)) % gds.size()];
}

void Rasterizer::OnSubmit() {
    const u64 n = ++stats.submits;
    context.Submit(false);
    if ((n & (n - 1)) == 0) {
        LOG_INFO(Render, "WebGPU after {} submits: {} of {} draws and {} of {} dispatches ran",
                 n, stats.draws_run.load(), stats.draws.load(), stats.dispatches_run.load(),
                 stats.dispatches.load());
    }
}

bool Rasterizer::ReadScanout(VAddr address, u32 width, u32 height, u32 bytes_per_texel,
                             std::vector<u8>& out) {
    if (!Web::Gpu::Available()) {
        return false;
    }
    const Web::Surface* surface = context.surfaces.FindScanout(address, width, height);
    if (surface == nullptr || surface->host.host_bytes != bytes_per_texel) {
        return false;
    }
    out.resize(u64(width) * height * bytes_per_texel);
    context.batch.Add({{"op", "readTexture"},
                       {"texture", surface->id},
                       {"width", width},
                       {"height", height},
                       {"bytesPerRow", width * bytes_per_texel},
                       {"ptr", reinterpret_cast<u64>(out.data())}});
    return context.Submit(true);
}

void Rasterizer::OnFlip() {
    context.surfaces.NextFrame(context.batch);
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

} // namespace Vulkan
