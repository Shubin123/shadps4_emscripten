// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/logging/log.h"
#include "core/wasm_high_memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_web/web_gpu.h"
#include "video_core/renderer_web/web_rasterizer.h"

namespace Vulkan {

Rasterizer::Rasterizer(AmdGpu::Liverpool* liverpool_) : liverpool{liverpool_} {
    liverpool->BindRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::Draw(bool is_indexed, u32) {
    if (stats.draws++ == 0) {
        LOG_WARNING(Render, "first guest draw (indexed={}) - no GPU backend in the wasm build yet, "
                            "draws are dropped",
                    is_indexed);
    }
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr, u32, u32, u32, VAddr, u16, u16) {
    Draw(is_indexed);
}

void Rasterizer::DispatchDirect() {
    const auto& cs = liverpool->GetCsRegs();
    Dispatch(cs, {cs.dim_x, cs.dim_y, cs.dim_z});
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    // The arguments are three dwords of guest memory: the workgroup counts.
    std::array<u32, 3> groups{};
    const VAddr args = address + offset;
    const u8* host = Core::HighMemory::InRange(args) ? Core::HighMemory::Translate(args)
                                                     : reinterpret_cast<const u8*>(args);
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
    auto* dst = reinterpret_cast<u32*>(address);
    for (u32 i = 0; i < num_bytes / sizeof(u32); i++) {
        dst[i] = value;
    }
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    stats.dma_copies++;
    u8* dst_ptr = dst_gds ? reinterpret_cast<u8*>(gds.data()) + dst : reinterpret_cast<u8*>(dst);
    const u8* src_ptr =
        src_gds ? reinterpret_cast<const u8*>(gds.data()) + src : reinterpret_cast<const u8*>(src);
    std::memmove(dst_ptr, src_ptr, num_bytes);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    return gds[(gds_offset / sizeof(u32)) % gds.size()];
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

} // namespace Vulkan
