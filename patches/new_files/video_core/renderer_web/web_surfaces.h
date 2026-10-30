// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Textures and render targets on the page's GPU, by guest address - the part of upstream's
// texture cache the WebGPU renderer needs.
//
// A surface is a GPU texture holding an image that lives at some guest address. Textures (T#s)
// are uploaded from guest memory: detiled and, where WebGPU has no equivalent format, converted
// on the CPU. Render targets are created the same way and from then on are newer on the GPU than
// in guest memory, because nothing is written back: a draw that samples memory a draw rendered
// to gets the render target itself, and the presenter reads display buffers back from the GPU.
//
// Without page protection there's no telling when the CPU writes to guest memory, so content is
// compared instead: each surface remembers a hash of the guest memory it was last synced with,
// re-checked once per frame. A texture whose memory changed is uploaded again; a render target
// whose memory changed was written by the CPU (or a DMA) since the GPU last used it - the GPU
// never touches guest memory - and the CPU's version wins.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include "common/types.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_web/web_gpu.h"
#include "video_core/renderer_web/web_tiling.h"

namespace Web {

// How a guest format is stored on the page. `conversion` != None means texels are rewritten on
// the CPU on the way up.
struct HostFormat {
    enum class Conversion : u8 {
        None,
        Unsupported, // no usable host format: shown as magenta
        R5G6B5,      // -> rgba8unorm
        A1R5G5B5,
        B4G4R4A4,
        Norm16ToHalf, // unorm/snorm 16 -> f16, per component
        Snorm8ToHalf,
        Rgb32ToRgba32, // 96-bit texels -> 128
        Srgb8ToRgba8,  // r8/rg8 sRGB -> rgba8unorm-srgb
    };
    std::string format;   // WebGPU texture format
    u32 guest_bytes;      // per guest element (texel, or 4x4 block)
    u32 host_bytes;       // per host element
    u32 components;       // guest components (for the per-component conversions)
    Conversion conversion;
    bool is_block;
    char sample_type;     // 'f' float, 'u' uint, 's' sint, 'd' depth
    bool signed_source{}; // the norm conversions: snorm rather than unorm
};

HostFormat ToHostFormat(AmdGpu::DataFormat dfmt, AmdGpu::NumberFormat nfmt, bool render_target);

struct Surface {
    u32 id;          // the page's texture id
    VAddr address;
    SurfaceLayout layout;
    HostFormat host;
    AmdGpu::DataFormat dfmt;
    AmdGpu::NumberFormat nfmt;
    u32 width;
    u32 height;
    u32 depth_or_layers;
    bool is_3d{};
    bool is_depth{};
    bool has_stencil{};
    bool render_target{}; // created to render to (usage, view formats)
    bool storage{};       // created for shaders to write (a storage texture)
    bool gpu_modified{};  // the page's copy is newer than guest memory
    u64 written{};        // when the GPU last wrote it (Surfaces' counter), to pick the newest
    bool uploaded{};
    bool stale{};         // guest memory changed since the last sync (Surfaces::CheckGuest)
    u64 guest_hash{};     // of guest memory when last checked
    u64 checked_frame{~0ull};
    u64 used_frame{};     // last frame it was bound (for evicting unused textures)
};

class Surfaces {
public:
    Surfaces();
    ~Surfaces();

    // The render target for color buffer `cb`, synced with guest memory if the CPU changed it.
    Surface* ColorTarget(Gpu::Batch& batch, const AmdGpu::ColorBuffer& cb,
                         AmdGpu::CbDbExtent hint);
    // The depth (and stencil) target. Its content starts cleared rather than uploaded: WebGPU
    // can't copy into depth formats, and games clear depth before using it.
    Surface* DepthTarget(Gpu::Batch& batch, const AmdGpu::DepthBuffer& db, AmdGpu::CbDbExtent hint);

    // The binding for a sampled T#, as a "tex" entry for webgpu.js ({"tex": id, "view": ...}).
    // `wgsl_type` is the texture type the shader declared.
    Gpu::Json Texture(Gpu::Batch& batch, const AmdGpu::Image& sharp, bool is_array,
                      const std::string& wgsl_type);
    // The binding for an image a shader writes (a storage texture of `format`, the backend's
    // StorageTexelFormat). From then on the GPU's copy is the current one, like a render target's.
    Gpu::Json StorageImage(Gpu::Batch& batch, const AmdGpu::Image& sharp, bool is_array,
                           const std::string& wgsl_type, const std::string& format);

    // Guest memory the CPU side wrote (DMA fills and copies): surfaces over it are checked again
    // the next time they're used.
    void Invalidate(VAddr address, u64 size);

    // A frame was presented: content checks happen again from here, and textures nothing has
    // used for a while are dropped (they can be uploaded again from guest memory; render
    // targets and written images, whose current content exists only on the GPU, are kept).
    void NextFrame(Gpu::Batch& batch);

    // The render target a display buffer at `address` was last drawn into, if the GPU's version
    // is current; nullptr when guest memory should be shown instead.
    Surface* FindScanout(VAddr address, u32 width, u32 height);

private:
    Surface* Create(Gpu::Batch& batch, std::unique_ptr<Surface> surface);
    void Upload(Gpu::Batch& batch, Surface& surface);
    // Re-hashes guest memory (once per frame unless forced) and sets `stale` if it changed:
    // whoever uses the surface next decides what that means for it.
    void CheckGuest(Surface& surface, bool force = false);
    void Destroy(Gpu::Batch& batch, Surface& surface);

    // The T#'s view of its image, for a WGSL texture of `wgsl_type`.
    Gpu::Json View(const AmdGpu::Image& sharp, bool is_array, const std::string& wgsl_type);
    void MarkWritten(Surface& surface) {
        surface.gpu_modified = true;
        surface.written = ++write_counter;
    }

    std::multimap<VAddr, std::unique_ptr<Surface>> surfaces;
    u32 next_id = 1;
    u64 write_counter = 0;
    u64 frame = 0;
};

} // namespace Web
