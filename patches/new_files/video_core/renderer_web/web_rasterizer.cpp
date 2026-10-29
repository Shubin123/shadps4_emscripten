// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/logging/log.h"
#include "video_core/amdgpu/liverpool.h"
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
    if (stats.dispatches++ == 0) {
        LOG_WARNING(Render, "first guest compute dispatch - dropped (no GPU backend yet)");
    }
}

void Rasterizer::DispatchIndirect(VAddr, u32, u32) {
    DispatchDirect();
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
