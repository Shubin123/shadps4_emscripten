// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "common/logging/log.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_web/web_presenter.h"
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

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& group,
                               VAddr cpu_address) {
    const auto& attrib = group.attrib;
    Frame* frame = GetFreeFrame();
    frame->width = attrib.width;
    frame->height = attrib.height;
    frame->rgba.resize(static_cast<size_t>(attrib.width) * attrib.height);

    if (attrib.pixel_format == PixelFormat::A16R16G16B16Float) {
        std::fill(frame->rgba.begin(), frame->rgba.end(), 0xFF000000u);
        return frame;
    }
    if (attrib.tiling_mode == TilingMode::Tile && !warned_tiled) {
        // Display buffers the GPU renders into are GCN-tiled; detiling them on the CPU only
        // matters once there's a GPU backend producing their contents, so for now they're read
        // as if linear (which shows the right thing for CPU-written linear buffers and garbage -
        // rather than nothing - for tiled ones).
        LOG_WARNING(Render, "tiled VideoOut buffer presented as linear (no CPU detiler yet)");
        warned_tiled = true;
    }
    const u8* src = reinterpret_cast<const u8*>(cpu_address);
    const size_t pitch_bytes = static_cast<size_t>(attrib.pitch_in_pixel) * 4;
    for (u32 y = 0; y < attrib.height; y++) {
        const u8* row = src + y * pitch_bytes;
        u32* out = frame->rgba.data() + static_cast<size_t>(y) * attrib.width;
        for (u32 x = 0; x < attrib.width; x++) {
            out[x] = ToRgba8(attrib.pixel_format, row + x * 4);
        }
    }
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
