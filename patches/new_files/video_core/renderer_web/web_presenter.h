// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stands in for Vulkan::Presenter (video_core/renderer_vulkan/vk_presenter.h) in the wasm build,
// keeping the name and the subset of the interface VideoOut/GnmDriver use. At each flip it hands
// the page (web/web_display.h) the VideoOut buffer as RGBA8: read back from the GPU when the
// guest rendered it there (Rasterizer::ReadScanout), otherwise read from guest memory on the CPU,
// detiled if the buffer is tiled.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include "common/types.h"
#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_web/web_rasterizer.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

namespace HostPasses {
struct PostProcessingPass {
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
    };
};
struct FsrPass {
    struct Settings {
        bool enable{false};
        bool use_rcas{false};
        float rcas_attenuation{0.25f};
    };
};
} // namespace HostPasses

struct Frame {
    u32 width{};
    u32 height{};
    std::vector<u32> rgba;
    bool is_hdr{false};
    u8 id{};
};

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }
    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }
    Frontend::WindowSDL& GetWindow() const {
        return window;
    }
    Rasterizer& GetRasterizer() const {
        return *rasterizer;
    }

    bool IsHDRSupported() const {
        return false;
    }
    void SetHDR(bool) {}

    void RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                                 VAddr cpu_address);

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);
    Frame* PrepareBlankFrame(bool present_thread);
    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetFreeFrame();

    Frontend::WindowSDL& window;
    AmdGpu::Liverpool* liverpool;
    std::unique_ptr<Rasterizer> rasterizer;
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::FsrPass::Settings fsr_settings{};

    std::mutex frames_mutex;
    std::array<Frame, 4> frames{};
    u32 next_frame{};
    Frame* last_frame{};
    bool warned_tiled{};
    u64 gpu_frames{};
};

} // namespace Vulkan
