// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WGSL backend for the shader recompiler, for the WebGPU renderer in the wasm build.
//
// The SPIR-V backend (backend/spirv) is what native shadPS4 feeds Vulkan; browsers' WebGPU only
// accepts WGSL. This emits WGSL text from the same IR program (after the same frontend and
// optimization passes), mirroring the SPIR-V backend's resource model: one storage buffer per
// Info::buffers entry, one texture per Info::images entry, one sampler per Info::samplers entry,
// with bindings assigned in that order. Compute and vertex shaders put them in @group(0), fragment
// shaders in @group(1), so a draw's two stages share one pipeline layout; @group(2) is the draw's
// own data (DrawInfo below).
//
// Vertex shaders fetch their attributes themselves (the fetch shader is translated inline, as
// buffer loads), so a draw binds vertex buffers like any other buffer. Their entry point is made
// per draw by LinkVertexShader: it has to match the fragment shader's inputs, applies the guest
// viewport transform, and can expand rectangle lists, none of which the vertex program decides.
//
// Anything it can't express (yet, or at all in WGSL - geometry/tessellation stages, 64-bit floats,
// some wave operations) is reported in Output::errors rather than emitted wrong; a program with
// errors must not be used for rendering.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/pixel_format.h"

namespace Shader::Backend::WGSL {

struct Binding {
    enum class Kind : u8 {
        Buffer,         // var<storage>, array<u32>
        Texture,        // texture_*<T> / texture_depth_*
        StorageTexture, // texture_storage_*<format, write>
        Sampler,        // sampler / sampler_comparison
    };
    Kind kind;
    u32 binding;  // @binding index in the stage's @group
    u32 resource; // index into Info::buffers / images / samplers
    bool writable{};
    bool comparison{};      // Sampler: used for depth comparison
    std::string wgsl_type;  // the declared type, for building the bind group layout
};

// A fragment shader input, as the vertex stage has to produce it. Locations are dense (the
// order the inputs are read in), since WebGPU's inter-stage limit is lower than GCN's 32 params.
struct FragmentInput {
    u32 location;
    u32 param;                 // the vertex shader's ParamN it reads
    std::string interpolation; // "" (perspective, center) or the @interpolate(...) arguments
};

struct Output {
    std::string code;
    std::vector<Binding> bindings;
    u32 group{}; // the @group of `bindings`
    // Vertex: which ParamN the program writes, and the code without an entry point, for
    // LinkVertexShader. `code` itself has a standalone entry point (for validation).
    u32 param_mask{};
    std::string code_without_entry;
    // Fragment: the inputs it reads (by location) and which RenderTargetN it writes.
    std::vector<FragmentInput> inputs;
    u32 color_output_mask{};
    bool writes_depth{};
    bool uses_discard{};
    std::vector<std::string> errors; // non-empty: the program could not be fully translated
};

// @group(2) @binding(0) of a linked vertex shader: the viewport transform, applied to the guest's
// position (clip space, or window space when clipping is disabled) to reach WebGPU clip space for
// a viewport covering the whole render target. For each axis, out = p * k + p.w * w + c with
// (k, w, c) the first three components; vz.w is unused.
struct DrawInfo {
    float vx[4];
    float vy[4];
    float vz[4];
    u32 pad[4];
};

struct VertexLink {
    std::vector<FragmentInput> inputs; // the fragment shader's
    // Rectangle lists: each invocation is one of 6 vertices (two triangles) of a rectangle whose
    // 3 guest vertex indices are in @group(2) @binding(1); the 4th corner is computed from them.
    bool rect_list{};
};

// The WebGPU storage texture format a written image of this guest format is declared with (and
// the renderer must create its texture with), or "" if WebGPU has none. Formats beyond core
// WebGPU's need the texture-formats-tier1 feature, and read-write access tier2.
std::string StorageTexelFormat(AmdGpu::DataFormat dfmt, AmdGpu::NumberFormat nfmt);

// The vertex shader `vs` with an entry point for a draw: outputs matching `link.inputs` (ones the
// program doesn't write are zero), the position through DrawInfo.
std::string LinkVertexShader(const Output& vs, const VertexLink& link);

Output EmitWGSL(const Profile& profile, const RuntimeInfo& runtime_info, const IR::Program& program);

// The recompiler profile for this backend: what the frontend may assume WebGPU/WGSL provides.
Profile MakeProfile();

} // namespace Shader::Backend::WGSL
