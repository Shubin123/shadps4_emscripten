// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest shader programs translated to WGSL, the way vk_pipeline_cache keeps SPIR-V: one entry
// per program (by hash), with a permutation per StageSpecialization of the resources bound to it.
// Also the ids the page knows shader modules and pipelines by: each distinct WGSL text or
// pipeline description is sent once (as a "shader"/"pipeline" command) and referred to by id
// after that.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/backend/wgsl/emit_wgsl.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_web/web_gpu.h"

namespace Web {

// RuntimeInfo has no default constructor (its stage infos are in unions); Initialize() memsets it.
struct RuntimeInfoStorage {
    alignas(Shader::RuntimeInfo) u8 bytes[sizeof(Shader::RuntimeInfo)];
    Shader::RuntimeInfo& Get() {
        return *reinterpret_cast<Shader::RuntimeInfo*>(bytes);
    }
};

class ShaderCache {
public:
    struct Permutation {
        Shader::StageSpecialization spec;
        std::vector<u64> image_keys; // what the WGSL also depends on: T# formats and swizzles
        Shader::Backend::WGSL::Output out;
        bool usable{};
    };
    struct Result {
        const Permutation* perm; // nullptr: the program can't be used (logged once)
        const Shader::Info* info; // where its resources are read from (user data refreshed)
    };

    ShaderCache();
    ~ShaderCache();

    Result Get(Shader::HwStage hw, Shader::SwStage sw, const Shader::ShaderParams& params,
               const Shader::RuntimeInfo& runtime_info);

private:
    struct Program;
    std::unordered_map<u64, std::unique_ptr<Program>> programs;
};

// Ids for shader modules and pipelines on the page.
class PageObjects {
public:
    // The id of the module for `code`, adding a "shader" command the first time.
    u32 Module(Gpu::Batch& batch, const std::string& code);
    // The id of the pipeline `desc` describes, adding a "pipeline" command (desc + id) the first
    // time. `desc` is compared as text.
    u32 Pipeline(Gpu::Batch& batch, Gpu::Json desc);

private:
    std::unordered_map<u64, u32> modules;
    std::unordered_map<std::string, u32> pipelines;
    u32 next_module = 1;
    u32 next_pipeline = 1;
};

// One per process: the page's objects outlive any one renderer object.
PageObjects& GetPageObjects();

} // namespace Web
