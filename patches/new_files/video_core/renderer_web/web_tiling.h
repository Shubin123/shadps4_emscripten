// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GCN surface layouts on the CPU: how big a texture or render target is in guest memory and
// where each of its texels lives, so the renderer can copy it out of guest memory into the
// linear rows WebGPU uploads. The addressing is upstream's GPU detiler
// (video_core/host_shaders/tiling.comp) ported to C++; sizes and mip offsets come from
// texture_cache/tile.h unchanged.

#pragma once

#include <array>
#include <span>
#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/texture_cache/tile.h"

namespace Web {

struct SurfaceLayout {
    AmdGpu::TileMode tile_mode{AmdGpu::TileMode::DisplayLinearAligned};
    AmdGpu::ArrayMode array_mode{AmdGpu::ArrayMode::ArrayLinearAligned};
    u32 num_bits{32}; // per element (a 4x4 block for block-compressed formats)
    u32 num_samples{1};
    u32 pitch{};      // texels
    u32 height{};
    u32 depth{1};     // 3D slices
    u32 layers{1};
    u32 levels{1};
    bool is_block{};
    bool is_pow2{};
    u8 bank_swizzle{};

    // Filled by Compute().
    u32 guest_size{};
    u32 micro_tiled_mips{};
    std::array<VideoCore::MipInfo, VideoCore::MAX_MIPS> mips{};

    bool IsTiled() const {
        return array_mode != AmdGpu::ArrayMode::ArrayLinearAligned &&
               array_mode != AmdGpu::ArrayMode::ArrayLinearGeneral;
    }
    u32 BytesPerElement() const {
        return num_bits / 8;
    }
    // Elements per row / rows of mip `level` in the linear copy (blocks for BCn).
    u32 MipPitchElements(u32 level) const {
        return is_block ? std::max(1u, (mips[level].pitch + 3u) / 4u) : mips[level].pitch;
    }
    u32 MipHeightElements(u32 level) const {
        return is_block ? std::max(1u, (mips[level].height + 3u) / 4u) : mips[level].height;
    }

    // Sizes every mip the way the guest's GPU lays them out. False if the layout is one this
    // code can't compute (an unsupported element size or tile mode).
    bool Compute();
};

// Copies the surface at `src` (guest_size bytes) into `dst` (also guest_size bytes) with every
// mip in linear rows of MipPitchElements, slice after slice, at the same offsets it has in guest
// memory. Linear surfaces are copied as they are.
void Detile(const SurfaceLayout& layout, const u8* src, u8* dst);

} // namespace Web
