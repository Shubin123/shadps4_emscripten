// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WGSL backend for the shader recompiler, for the WebGPU renderer in the wasm build.
//
// The SPIR-V backend (backend/spirv) is what native shadPS4 feeds Vulkan; browsers' WebGPU only
// accepts WGSL. This emits WGSL text from the same IR program (after the same frontend and
// optimization passes), mirroring the SPIR-V backend's resource model: one storage buffer per
// Info::buffers entry, one texture per Info::images entry, one sampler per Info::samplers entry,
// all in @group(0) with bindings assigned in that order.
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

namespace Shader::Backend::WGSL {

struct Binding {
    enum class Kind : u8 {
        Buffer,         // var<storage>, array<u32>
        Texture,        // texture_*<T> / texture_depth_*
        StorageTexture, // texture_storage_*<format, write>
        Sampler,        // sampler / sampler_comparison
    };
    Kind kind;
    u32 binding;  // @binding index in @group(0)
    u32 resource; // index into Info::buffers / images / samplers
    bool writable{};
    bool comparison{};      // Sampler: used for depth comparison
    std::string wgsl_type;  // the declared type, for building the bind group layout
};

struct VertexInput {
    u32 location;      // @location in the vertex entry point (= fetch shader semantic)
    u32 dest_vgpr;     // first VGPR the fetch shader loads it into (identifies the attribute)
    u32 num_components;
    bool is_integer;
    bool is_signed;
};

struct Output {
    std::string code;
    std::vector<Binding> bindings;
    std::vector<VertexInput> vertex_inputs;
    u32 color_output_mask{}; // fragment: which RenderTargetN are written
    bool writes_depth{};
    bool uses_discard{};
    std::vector<std::string> errors; // non-empty: the program could not be fully translated
};

Output EmitWGSL(const Profile& profile, const RuntimeInfo& runtime_info, const IR::Program& program);

// The recompiler profile for this backend: what the frontend may assume WebGPU/WGSL provides.
Profile MakeProfile();

} // namespace Shader::Backend::WGSL
