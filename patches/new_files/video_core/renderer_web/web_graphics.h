// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest draws on WebGPU: vk_rasterizer's Draw and vk_pipeline_cache's graphics half for the
// WebGPU renderer. A draw's vertex and pixel shaders go through the recompiler and the WGSL
// backend (vertex fetch inline, the vertex entry point linked to the pixel shader's inputs), its
// render targets come from Surfaces, and the fixed-function state (topology, culling, depth and
// stencil, blending, write masks) becomes a WebGPU render pipeline description. What WebGPU
// lacks is emulated: the guest's viewport transform runs in the vertex shader (WebGPU viewports
// must lie inside the target), rectangle lists are expanded there, and fans, quads, polygons and
// line loops are rewritten as lists of indices.
//
// Draws are recorded into the context's batch; nothing waits for the GPU unless a pixel shader
// writes guest memory.

#pragma once

#include <array>
#include <optional>
#include "common/types.h"
#include "video_core/renderer_web/web_context.h"

namespace AmdGpu {
union Regs;
struct Liverpool;
} // namespace AmdGpu

namespace Web {

struct DrawParams {
    bool indexed;
    u32 index_offset;   // first index (DRAW_INDEX_OFFSET)
    // Indirect draws, with their arguments already read from guest memory.
    std::optional<u32> count;
    std::optional<u32> instances;
};

class GraphicsPipelines {
public:
    explicit GraphicsPipelines(Context& context);
    ~GraphicsPipelines();

    // False if the draw was dropped (the reason is logged once per kind).
    bool Draw(AmdGpu::Liverpool& liverpool, const DrawParams& params);
    // The CB_COLOR_CONTROL modes that aren't draws: fast-clear elimination and MSAA resolve.
    void EliminateFastClear(AmdGpu::Liverpool& liverpool);
    void Resolve(AmdGpu::Liverpool& liverpool);

private:
    Context& ctx;
};

} // namespace Web
