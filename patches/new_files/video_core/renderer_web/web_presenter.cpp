// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "common/logging/log.h"
#include "video_core/amdgpu/liverpool.h"
#include "common/number_utils.h"
#include "video_core/renderer_web/web_presenter.h"
#include "video_core/renderer_web/web_tiling.h"
#include "web/web_display.h"

namespace Vulkan {

using Libraries::VideoOut::PixelFormat;
using Libraries::VideoOut::TilingMode;

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      rasterizer{std::make_unique<Rasterizer>(liverpool_)} {}

Presenter::~Presenter() = default;

void Presenter::RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& group,
                                        VAddr cpu_address) {
    LOG_INFO(Render, "VideoOut surface {:#x}: {}x{} pitch {} format {:#x} tiling {}", cpu_address,
             group.attrib.width, group.attrib.height, group.attrib.pitch_in_pixel,
             static_cast<u32>(group.attrib.pixel_format),
             static_cast<s32>(group.attrib.tiling_mode));
}

Frame* Presenter::GetFreeFrame() {
    std::scoped_lock lk{frames_mutex};
    Frame* frame = &frames[next_frame];
    frame->id = static_cast<u8>(next_frame);
    next_frame = (next_frame + 1) % frames.size();
    return frame;
}

// Converts one guest scan-out pixel to the page's RGBA8 (little-endian u32 = R | G<<8 | B<<16 |
// A<<24, which is what a canvas ImageData expects byte-wise).
static u32 ToRgba8(PixelFormat format, const u8* src) {
    u32 v;
    std::memcpy(&v, src, sizeof(v));
    switch (format) {
    case PixelFormat::A8B8G8R8Srgb:
        return v | 0xFF000000u;
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq: {
        const u32 b = (v >> 2) & 0xFF, g = (v >> 12) & 0xFF, r = (v >> 22) & 0xFF;
        return r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    case PixelFormat::A8R8G8B8Srgb:
    default: {
        const u32 b = v & 0xFF, g = (v >> 8) & 0xFF, r = (v >> 16) & 0xFF;
        return r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    }
}

// A16R16G16B16Float: half floats, B G R A in memory order like the 8-bit formats.
static u32 HalfToRgba8(const u8* src) {
    u16 h[4];
    std::memcpy(h, src, sizeof(h));
    const auto to8 = [](u16 v) {
        const float f = std::clamp(NumberUtils::Uf16ToF32(v), 0.0f, 1.0f);
        return static_cast<u32>(f * 255.0f + 0.5f);
    };
    return to8(h[2]) | (to8(h[1]) << 8) | (to8(h[0]) << 16) | 0xFF000000u;
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& group,
                               VAddr cpu_address) {
    const auto& attrib = group.attrib;
    Frame* frame = GetFreeFrame();
    frame->width = attrib.width;
    frame->height = attrib.height;
    frame->rgba.resize(static_cast<size_t>(attrib.width) * attrib.height);
    const bool half = attrib.pixel_format == PixelFormat::A16R16G16B16Float;
    const u32 bpp = half ? 8 : 4;
    const auto convert = [&](const u8* src, size_t pitch_pixels) {
        for (u32 y = 0; y < attrib.height; y++) {
            const u8* row = src + y * pitch_pixels * bpp;
            u32* out = frame->rgba.data() + static_cast<size_t>(y) * attrib.width;
            for (u32 x = 0; x < attrib.width; x++) {
                out[x] = half ? HalfToRgba8(row + x * bpp) : ToRgba8(attrib.pixel_format,
                                                                      row + x * bpp);
            }
        }
    };

    // What the GPU rendered there, unless the CPU has written the buffer since.
    std::vector<u8> pixels;
    if (rasterizer->ReadScanout(cpu_address, attrib.width, attrib.height, bpp, pixels)) {
        convert(pixels.data(), attrib.width);
        rasterizer->OnFlip();
        if (const u64 n = ++gpu_frames; (n & (n - 1)) == 0) {
            LOG_INFO(Render, "{} frame(s) presented from the GPU (buffer {:#x})", n, cpu_address);
        }
        return frame;
    }
    rasterizer->OnFlip();

    const u8* src = reinterpret_cast<const u8*>(cpu_address);
    if (attrib.tiling_mode == TilingMode::Tile) {
        // Display buffers are tiled as the GPU's color buffers are (ImageInfo's VideoOut case).
        Web::SurfaceLayout layout;
        layout.tile_mode = AmdGpu::TileMode::Display2DThin;
        layout.array_mode = AmdGpu::GetArrayMode(layout.tile_mode);
        layout.num_bits = bpp * 8;
        layout.pitch = (attrib.width + 127) & ~127u;
        layout.height = attrib.height;
        if (layout.Compute()) {
            std::vector<u8> linear(layout.guest_size);
            Web::Detile(layout, src, linear.data());
            convert(linear.data(), layout.MipPitchElements(0));
            return frame;
        }
        if (!warned_tiled) {
            LOG_WARNING(Render, "tiled VideoOut buffer {}x{} presented as linear",
                        attrib.width, attrib.height);
            warned_tiled = true;
        }
    }
    convert(src, attrib.pitch_in_pixel);
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool) {
    Frame* frame = GetFreeFrame();
    frame->width = 1280;
    frame->height = 720;
    frame->rgba.assign(static_cast<size_t>(frame->width) * frame->height, 0xFF000000u);
    return frame;
}

Frame* Presenter::PrepareLastFrame() {
    return last_frame;
}

void Presenter::Present(Frame* frame, bool, bool is_game_frame) {
    if (frame == nullptr) {
        return;
    }
    last_frame = frame;
    Web::Display::Submit(frame->rgba.data(), frame->width, frame->height, is_game_frame);
}

} // namespace Vulkan
