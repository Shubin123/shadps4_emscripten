// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stands in for Vulkan::Rasterizer (video_core/renderer_vulkan/vk_rasterizer.h) in the wasm build,
// with the same name and the same public interface Liverpool and Core::MemoryManager call, so
// neither needs to change. Draws and compute dispatches run on WebGPU when the page has a device
// (web_graphics.h, web_compute.h), recorded into a batch that goes to the page at the end of each
// submit. The CP DMA operations Liverpool forwards here (FillBuffer/CopyBuffer/ReadDataFromGds)
// are carried out on guest memory on the CPU - games use them for ordinary copies, fence writes
// and fast clears - and tell the renderer which memory changed.

#pragma once

#include <array>
#include <atomic>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <boost/container/small_vector.hpp>
#include <fmt/format.h>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_web/web_compute.h"
#include "video_core/renderer_web/web_context.h"
#include "video_core/renderer_web/web_graphics.h"

// The real vk_rasterizer.h pulls in a large include tree, and some of its includers (memory.cpp,
// liverpool.cpp) rely on <shared_mutex>/boost small_vector arriving through it transitively -
// hence the two otherwise-unneeded includes above.

namespace AmdGpu {
struct Liverpool;
struct ComputeProgram;
}

namespace Vulkan {

struct RasterizerStats {
    std::atomic<u64> draws{};
    std::atomic<u64> draws_run{}; // on WebGPU; the rest were dropped
    std::atomic<u64> dispatches{};
    std::atomic<u64> dispatches_run{}; // on WebGPU; the rest were dropped
    std::atomic<u64> dma_copies{};
    std::atomic<u64> dma_fills{};
    std::atomic<u64> submits{};
};

class Rasterizer {
public:
    explicit Rasterizer(AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    void Draw(bool is_indexed, u32 index_offset = 0);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view, fmt::format_args, auto&& func) {
        func();
    }
    void ScopeMarkerBegin(const std::string_view&, bool = false) {}
    void ScopeMarkerEnd(bool = false) {}
    void ScopedMarkerInsert(const std::string_view&, bool = false) {}
    void ScopedMarkerInsertColor(const std::string_view&, const u32, bool = false) {}

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gds_offset);
    bool InvalidateMemory(VAddr, u64, bool = false) {
        return false;
    }
    bool ReadMemory(VAddr, u64, bool = false) {
        return false;
    }
    bool IsMapped(VAddr, u64) {
        return true;
    }
    void MapMemory(VAddr, u64) {}
    void RegisterMemory(VAddr, u64) {}
    void UnmapMemory(VAddr, u64) {}

    u64 Flush() {
        return 0;
    }
    void Finish() {}
    void OnSubmit();
    void OnFence() {}

    std::thread::id GetGpuCommandProcessorThread();

    const RasterizerStats& Stats() const {
        return stats;
    }

    // For the presenter: the GPU's copy of the display buffer at `address`, `width`x`height`
    // texels of `bytes_per_texel`, read back into `out`. False if guest memory holds the current
    // picture (nothing rendered there, or the CPU wrote it since).
    bool ReadScanout(VAddr address, u32 width, u32 height, u32 bytes_per_texel,
                     std::vector<u8>& out);
    // A frame went out: surfaces are checked against guest memory again from here.
    void OnFlip();

private:
    void Dispatch(const AmdGpu::ComputeProgram& cs, std::array<u32, 3> groups);
    void DrawOne(const Web::DrawParams& params);

    AmdGpu::Liverpool* liverpool;
    RasterizerStats stats;
    Web::Context context;
    Web::ComputePipelines compute{context};
    Web::GraphicsPipelines graphics{context};
    // Global Data Share: 64KB of on-chip GPU memory addressed by dword index.
    std::array<u32, 0x10000 / sizeof(u32)> gds{};
};

} // namespace Vulkan
