// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stands in for Vulkan::Rasterizer (video_core/renderer_vulkan/vk_rasterizer.h) in the wasm build,
// with the same name and the same public interface Liverpool and Core::MemoryManager call, so
// neither needs to change. There is no GPU backend behind it yet (see PROJECT.md, M8): draws and
// dispatches are counted and dropped. The CP DMA operations Liverpool forwards here
// (FillBuffer/CopyBuffer/ReadDataFromGds) are real work on guest memory, though - games use them
// for ordinary copies and fence writes, so they are carried out on the CPU instead of skipped.

#pragma once

#include <array>
#include <atomic>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <boost/container/small_vector.hpp>
#include <fmt/format.h>
#include "common/types.h"

// The real vk_rasterizer.h pulls in a large include tree, and some of its includers (memory.cpp,
// liverpool.cpp) rely on <shared_mutex>/boost small_vector arriving through it transitively -
// hence the two otherwise-unneeded includes above.

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct RasterizerStats {
    std::atomic<u64> draws{};
    std::atomic<u64> dispatches{};
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
    void OnSubmit() {
        stats.submits++;
    }
    void OnFence() {}

    std::thread::id GetGpuCommandProcessorThread();

    const RasterizerStats& Stats() const {
        return stats;
    }

private:
    AmdGpu::Liverpool* liverpool;
    RasterizerStats stats;
    // Global Data Share: 64KB of on-chip GPU memory addressed by dword index.
    std::array<u32, 0x10000 / sizeof(u32)> gds{};
};

} // namespace Vulkan
