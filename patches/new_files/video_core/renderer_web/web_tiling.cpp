// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include "video_core/renderer_web/web_tiling.h"

namespace Web {

namespace {

using AmdGpu::ArrayMode;
using AmdGpu::MicroTileMode;

constexpr u32 MicroTileWidth = 8;
constexpr u32 MicroTileHeight = 8;
constexpr u32 MicroTilePixels = MicroTileWidth * MicroTileHeight;
constexpr u32 NumPipeInterleaveBits = 8;

constexpr u32 Bit(u32 v, u32 b) {
    return (v >> b) & 1u;
}

// The constants tiling.comp is specialized with (TileManager::GetTilingPipeline).
struct Params {
    u32 bpp;
    u32 num_samples;
    MicroTileMode micro_mode;
    u32 thickness;
    bool macro;
    ArrayMode array_mode;
    AmdGpu::PipeConfig pipe_config;
    u32 bank_width;
    u32 bank_height;
    u32 num_banks;
    u32 num_bank_bits;
    u32 tile_split_bytes;
    u32 macro_tile_aspect;
    u32 num_pipes;
    u32 num_pipe_bits;
    u32 bank_swizzle;
};

u32 PixelIndexWithinMicroTile(const Params& p, u32 x, u32 y, u32 z, u32 thickness) {
    const u32 x0 = Bit(x, 0), x1 = Bit(x, 1), x2 = Bit(x, 2);
    const u32 y0 = Bit(y, 0), y1 = Bit(y, 1), y2 = Bit(y, 2);
    const u32 z0 = Bit(z, 0), z1 = Bit(z, 1), z2 = Bit(z, 2);
    u32 b[9]{};
    if (p.micro_mode == MicroTileMode::Display) {
        switch (p.bpp) {
        case 8:
            b[0] = x0, b[1] = x1, b[2] = x2, b[3] = y1, b[4] = y0, b[5] = y2;
            break;
        case 16:
            b[0] = x0, b[1] = x1, b[2] = x2, b[3] = y0, b[4] = y1, b[5] = y2;
            break;
        case 32:
            b[0] = x0, b[1] = x1, b[2] = y0, b[3] = x2, b[4] = y1, b[5] = y2;
            break;
        case 64:
            b[0] = x0, b[1] = y0, b[2] = x1, b[3] = x2, b[4] = y1, b[5] = y2;
            break;
        case 128:
            b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1, b[4] = x2, b[5] = y2;
            break;
        }
    } else if (p.micro_mode == MicroTileMode::Thin || p.micro_mode == MicroTileMode::Depth) {
        b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1, b[4] = x2, b[5] = y2;
    } else {
        if (p.bpp == 8 || p.bpp == 16) {
            b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1, b[4] = z0, b[5] = z1;
        } else if (p.bpp == 32) {
            b[0] = x0, b[1] = y0, b[2] = x1, b[3] = z0, b[4] = y1, b[5] = z1;
        } else {
            b[0] = x0, b[1] = y0, b[2] = z0, b[3] = x1, b[4] = y1, b[5] = z1;
        }
        b[6] = x2;
        b[7] = y2;
        if (thickness == 8) {
            b[8] = z2;
        }
    }
    u32 index = 0;
    for (u32 i = 0; i < 9; i++) {
        index |= b[i] << i;
    }
    return index;
}

u32 AddrMicroTiled(const Params& p, u32 x, u32 y, u32 slice, u32 pitch, u32 height,
                   u32 thickness) {
    const u32 micro_tile_bytes = MicroTilePixels * thickness * p.bpp * p.num_samples / 8;
    const u32 slice_bytes = (pitch * height * thickness * p.bpp * p.num_samples + 7) / 8;
    const u32 micro_tiles_per_row = pitch / MicroTileWidth;
    const u32 slice_offset = (slice / thickness) * slice_bytes;
    const u32 micro_tile_offset =
        ((y / MicroTileHeight) * micro_tiles_per_row + x / MicroTileWidth) * micro_tile_bytes;
    const u32 pixel_index = PixelIndexWithinMicroTile(p, x, y, slice, thickness);
    const u32 pixel_offset = p.micro_mode == MicroTileMode::Depth
                                 ? pixel_index * p.bpp * p.num_samples
                                 : pixel_index * p.bpp;
    return slice_offset + micro_tile_offset + pixel_offset / 8;
}

u32 PipeFromCoord(const Params& p, u32 x, u32 y, u32 slice) {
    const u32 tx = x / MicroTileWidth;
    const u32 ty = y / MicroTileHeight;
    const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2);
    const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2);
    u32 p0 = 0, p1 = 0, p2 = 0;
    switch (p.pipe_config) {
    case AmdGpu::PipeConfig::P2:
        p0 = x3 ^ y3;
        break;
    case AmdGpu::PipeConfig::P8_32x32_8x16:
        p0 = x4 ^ y3 ^ x5;
        p1 = x3 ^ y4;
        p2 = x5 ^ y5;
        break;
    case AmdGpu::PipeConfig::P8_32x32_16x16:
        p0 = x3 ^ y3 ^ x4;
        p1 = x4 ^ y4;
        p2 = x5 ^ y5;
        break;
    default:
        break;
    }
    u32 pipe = p0 | (p1 << 1) | (p2 << 2);
    u32 pipe_swizzle = 0;
    if (p.array_mode == ArrayMode::Array3DTiledThin1 ||
        p.array_mode == ArrayMode::Array3DTiledThick ||
        p.array_mode == ArrayMode::Array3DTiledXThick) {
        pipe_swizzle += std::max(1u, p.num_pipes / 2 - 1) * (slice / p.thickness);
    }
    pipe_swizzle &= p.num_pipes - 1;
    return pipe ^ pipe_swizzle;
}

u32 BankFromCoord(const Params& p, u32 x, u32 y, u32 slice, u32 tile_split_slice) {
    const u32 tx = x / MicroTileWidth / (p.bank_width * p.num_pipes);
    const u32 ty = y / MicroTileHeight / p.bank_height;
    const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
    u32 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (p.num_banks == 16) {
        b0 = x3 ^ y6;
        b1 = x4 ^ y5 ^ y6;
        b2 = x5 ^ y4;
        b3 = x6 ^ y3;
    } else if (p.num_banks == 8) {
        b0 = x3 ^ y5;
        b1 = x4 ^ y4 ^ y5;
        b2 = x5 ^ y3;
    } else if (p.num_banks == 4) {
        b0 = x3 ^ y4;
        b1 = x4 ^ y3;
    } else if (p.num_banks == 2) {
        b0 = x3 ^ y3;
    }
    u32 bank = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);
    u32 slice_rotation = 0;
    if (p.array_mode == ArrayMode::Array2DTiledThin1 ||
        p.array_mode == ArrayMode::Array2DTiledThick ||
        p.array_mode == ArrayMode::Array2DTiledXThick) {
        slice_rotation = (p.num_banks / 2 - 1) * (slice / p.thickness);
    } else if (p.array_mode == ArrayMode::Array3DTiledThin1 ||
               p.array_mode == ArrayMode::Array3DTiledThick ||
               p.array_mode == ArrayMode::Array3DTiledXThick) {
        slice_rotation = std::max(1u, p.num_pipes / 2 - 1) * (slice / p.thickness) / p.num_pipes;
    }
    u32 tile_split_rotation = 0;
    if (p.array_mode == ArrayMode::Array2DTiledThin1 ||
        p.array_mode == ArrayMode::Array3DTiledThin1 ||
        p.array_mode == ArrayMode::ArrayPrt2DTiledThin1 ||
        p.array_mode == ArrayMode::ArrayPrt3DTiledThin1) {
        tile_split_rotation = (p.num_banks / 2 + 1) * tile_split_slice;
    }
    bank ^= p.bank_swizzle + slice_rotation;
    bank ^= tile_split_rotation;
    return bank & (p.num_banks - 1);
}

u32 AddrMacroTiled(const Params& p, u32 x, u32 y, u32 slice, u32 pitch, u32 height) {
    const u32 micro_tile_bits = MicroTilePixels * p.thickness * p.bpp * p.num_samples;
    const u32 pixel_index = PixelIndexWithinMicroTile(p, x, y, slice, p.thickness);
    const u32 pixel_offset = p.micro_mode == MicroTileMode::Depth
                                 ? pixel_index * p.bpp * p.num_samples
                                 : pixel_index * p.bpp;
    u32 element_offset = pixel_offset / 8;

    u32 micro_tile_bytes = micro_tile_bits / 8;
    u32 slices_per_tile = 1;
    u32 tile_split_slice = 0;
    if (micro_tile_bytes > p.tile_split_bytes && p.thickness == 1) {
        slices_per_tile = micro_tile_bytes / p.tile_split_bytes;
        tile_split_slice = element_offset / p.tile_split_bytes;
        element_offset %= p.tile_split_bytes;
        micro_tile_bytes = p.tile_split_bytes;
    }

    const u32 macro_tile_pitch = (MicroTileWidth * p.bank_width * p.num_pipes) * p.macro_tile_aspect;
    const u32 macro_tile_height =
        (MicroTileHeight * p.bank_height * p.num_banks) / p.macro_tile_aspect;
    const u32 macro_tile_bytes = micro_tile_bytes * (macro_tile_pitch / MicroTileWidth) *
                                 (macro_tile_height / MicroTileHeight) /
                                 (p.num_pipes * p.num_banks);
    const u32 macro_tiles_per_row = pitch / macro_tile_pitch;
    const u32 macro_tile_offset =
        ((y / macro_tile_height) * macro_tiles_per_row + x / macro_tile_pitch) * macro_tile_bytes;
    const u32 macro_tiles_per_slice = macro_tiles_per_row * (height / macro_tile_height);
    const u32 slice_bytes = macro_tiles_per_slice * macro_tile_bytes;
    const u32 slice_offset =
        slice_bytes * (tile_split_slice + slices_per_tile * (slice / p.thickness));

    const u32 tile_row_index = (y / MicroTileHeight) % p.bank_height;
    const u32 tile_column_index = ((x / MicroTileWidth) / p.num_pipes) % p.bank_width;
    const u32 tile_offset = (tile_row_index * p.bank_width + tile_column_index) * micro_tile_bytes;

    const u32 total_offset = slice_offset + macro_tile_offset + element_offset + tile_offset;

    if (AmdGpu::IsPrt(p.array_mode)) {
        x %= macro_tile_pitch;
        y %= macro_tile_height;
    }
    const u32 pipe = PipeFromCoord(p, x, y, slice);
    const u32 bank = BankFromCoord(p, x, y, slice, tile_split_slice);

    const u32 pipe_interleave_offset = total_offset & ((1u << NumPipeInterleaveBits) - 1);
    const u32 offset = total_offset >> NumPipeInterleaveBits;
    return pipe_interleave_offset | (pipe << NumPipeInterleaveBits) |
           (bank << (NumPipeInterleaveBits + p.num_pipe_bits)) |
           (offset << (NumPipeInterleaveBits + p.num_pipe_bits + p.num_bank_bits));
}

Params MakeParams(const SurfaceLayout& layout) {
    Params p{};
    p.bpp = layout.num_bits;
    p.num_samples = layout.num_samples;
    p.micro_mode = AmdGpu::GetMicroTileMode(layout.tile_mode);
    p.thickness = AmdGpu::GetMicroTileThickness(layout.array_mode);
    p.array_mode = layout.array_mode;
    p.macro = AmdGpu::IsMacroTiled(layout.array_mode);
    p.bank_swizzle = layout.bank_swizzle;
    if (p.macro) {
        const auto macro_mode =
            AmdGpu::CalculateMacrotileMode(layout.tile_mode, layout.num_bits, layout.num_samples);
        p.pipe_config = AmdGpu::GetPipeConfig(layout.tile_mode);
        p.bank_width = AmdGpu::GetBankWidth(macro_mode);
        p.bank_height = AmdGpu::GetBankHeight(macro_mode);
        p.num_banks = AmdGpu::GetNumBanks(macro_mode);
        p.num_bank_bits = std::bit_width(p.num_banks) - 1;
        p.tile_split_bytes = AmdGpu::CalculateTileSplit(layout.tile_mode, layout.array_mode,
                                                        p.micro_mode, layout.num_bits);
        p.macro_tile_aspect = AmdGpu::GetMacrotileAspect(macro_mode);
        p.num_pipes = p.pipe_config == AmdGpu::PipeConfig::P2 ? 2 : 8;
        p.num_pipe_bits = p.pipe_config == AmdGpu::PipeConfig::P2 ? 1 : 3;
    }
    return p;
}

template <u32 Bytes>
void DetileElements(const SurfaceLayout& layout, const Params& p, const u8* src, u8* dst) {
    for (u32 mip = 0; mip < layout.levels; mip++) {
        const auto& info = layout.mips[mip];
        const u32 pitch = layout.MipPitchElements(mip);
        const u32 height = layout.MipHeightElements(mip);
        if (pitch == 0 || height == 0) {
            continue;
        }
        const u32 slices = info.size / (pitch * height * Bytes);
        const bool micro = !p.macro || (layout.micro_tiled_mips >> mip) & 1;
        const u32 micro_thickness = p.macro ? std::min(p.thickness, 4u) : p.thickness;
        u8* out = dst + info.offset;
        for (u32 slice = 0; slice < slices; slice++) {
            for (u32 y = 0; y < height; y++) {
                for (u32 x = 0; x < pitch; x++, out += Bytes) {
                    const u32 offset =
                        info.offset + (micro ? AddrMicroTiled(p, x, y, slice, pitch, height,
                                                              micro_thickness)
                                             : AddrMacroTiled(p, x, y, slice, pitch, height));
                    if (offset + Bytes <= layout.guest_size) {
                        std::memcpy(out, src + offset, Bytes);
                    }
                }
            }
        }
    }
}

} // namespace

bool SurfaceLayout::Compute() {
    if (num_bits != 8 && num_bits != 16 && num_bits != 32 && num_bits != 64 && num_bits != 96 &&
        num_bits != 128) {
        return false;
    }
    if (num_bits == 96 && IsTiled()) {
        // tiling.comp handles it as three dwords; the address math has no 96-bit case.
        return false;
    }
    levels = std::clamp(levels, 1u, static_cast<u32>(VideoCore::MAX_MIPS));
    const AmdGpu::ArrayMode mode =
        array_mode == AmdGpu::ArrayMode::ArrayLinearGeneral ? AmdGpu::ArrayMode::ArrayLinearAligned
                                                            : array_mode;
    VideoCore::ComputeImageSize(pitch, height, depth, levels, layers, num_bits, num_samples,
                                is_block, is_pow2, false, tile_mode, mode, &guest_size,
                                &micro_tiled_mips, mips.data());
    return guest_size != 0;
}

void Detile(const SurfaceLayout& layout, const u8* src, u8* dst) {
    if (!layout.IsTiled()) {
        std::memcpy(dst, src, layout.guest_size);
        return;
    }
    const Params p = MakeParams(layout);
    switch (layout.num_bits) {
    case 8:
        return DetileElements<1>(layout, p, src, dst);
    case 16:
        return DetileElements<2>(layout, p, src, dst);
    case 32:
        return DetileElements<4>(layout, p, src, dst);
    case 64:
        return DetileElements<8>(layout, p, src, dst);
    case 128:
        return DetileElements<16>(layout, p, src, dst);
    default:
        std::memcpy(dst, src, layout.guest_size);
        return;
    }
}

} // namespace Web
