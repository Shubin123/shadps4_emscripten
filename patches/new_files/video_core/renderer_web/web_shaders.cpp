// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <magic_enum/magic_enum.hpp>
#include "common/logging/log.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/renderer_web/web_shaders.h"

namespace Web {

namespace WGSL = Shader::Backend::WGSL;

struct ShaderCache::Program {
    Program(Shader::HwStage hw, Shader::SwStage sw, const Shader::ShaderParams& params)
        : info{hw, sw, params} {}

    Shader::Info info;
    std::vector<std::unique_ptr<Permutation>> permutations;
    bool reported{};
};

ShaderCache::ShaderCache() = default;
ShaderCache::~ShaderCache() = default;

namespace {

// The image state the WGSL text depends on beyond StageSpecialization: sampled images are
// swizzled in the shader (WebGPU views can't), and a storage image's texel format is declared.
std::vector<u64> ImageKeys(const Shader::Info& info) {
    std::vector<u64> keys;
    keys.reserve(info.images.size());
    for (const auto& desc : info.images) {
        const AmdGpu::Image sharp = desc.GetSharp(info);
        keys.push_back(u64(sharp.data_format) | u64(sharp.num_format) << 6 |
                       u64(sharp.dst_sel_x) << 10 | u64(sharp.dst_sel_y) << 13 |
                       u64(sharp.dst_sel_z) << 16 | u64(sharp.dst_sel_w) << 19 |
                       u64(sharp.type) << 22);
    }
    return keys;
}

} // namespace

ShaderCache::Result ShaderCache::Get(Shader::HwStage hw, Shader::SwStage sw,
                                     const Shader::ShaderParams& params,
                                     const Shader::RuntimeInfo& runtime_info) {
    static const Shader::Profile profile = WGSL::MakeProfile();
    const auto compile = [&](Shader::Info& info) {
        auto perm = std::make_unique<Permutation>();
        Shader::Pools pools;
        Shader::RuntimeInfo rt = runtime_info;
        const auto program = Shader::TranslateProgram(params.code, pools, info, rt, profile);
        perm->out = WGSL::EmitWGSL(profile, rt, program);
        perm->usable = perm->out.errors.empty();
        if (static const char* dir = std::getenv("SHADPS4_WGSL_DUMP"); dir != nullptr) {
            // One file per translation, for looking at what a game's shaders became.
            static std::atomic<u32> serial{0};
            const std::string path = fmt::format("{}/{}_{:016x}_{}.wgsl", dir,
                                                 magic_enum::enum_name(sw), params.hash,
                                                 serial++);
            if (FILE* f = std::fopen(path.c_str(), "w")) {
                std::fwrite(perm->out.code.data(), 1, perm->out.code.size(), f);
                std::fclose(f);
            }
        }
        LOG_INFO(Render, "WebGPU: {} shader {:#x} -> WGSL ({} bindings){}",
                 magic_enum::enum_name(sw), params.hash,
                 perm->out.bindings.size(), perm->usable ? "" : ", not usable");
        return perm;
    };

    auto [it, inserted] = programs.try_emplace(params.hash);
    Permutation* perm = nullptr;
    if (inserted) {
        it->second = std::make_unique<Program>(hw, sw, params);
        auto& program = *it->second;
        auto first = compile(program.info);
        first->spec = Shader::StageSpecialization(program.info, runtime_info, profile, {});
        first->image_keys = ImageKeys(program.info);
        perm = first.get();
        program.permutations.push_back(std::move(first));
    } else {
        auto& info = it->second->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        Shader::StageSpecialization spec(info, runtime_info, profile, {});
        auto keys = ImageKeys(info);
        auto& perms = it->second->permutations;
        const auto found = std::ranges::find_if(
            perms, [&](const auto& p) { return p->spec == spec && p->image_keys == keys; });
        if (found != perms.end()) {
            perm = found->get();
        } else {
            Shader::Info new_info(hw, sw, params);
            auto next = compile(new_info);
            next->spec = std::move(spec);
            next->image_keys = std::move(keys);
            perm = next.get();
            perms.push_back(std::move(next));
        }
    }
    Program& program = *it->second;
    if (!perm->usable) {
        if (!std::exchange(program.reported, true)) {
            for (const auto& error : perm->out.errors) {
                LOG_ERROR(Render, "WebGPU: {} shader {:#x} not usable: {}",
                          magic_enum::enum_name(sw), params.hash, error);
            }
        }
        return {nullptr, &program.info};
    }
    return {perm, &program.info};
}

u32 PageObjects::Module(Gpu::Batch& batch, const std::string& code) {
    const u64 key = std::hash<std::string>{}(code);
    const auto [it, inserted] = modules.try_emplace(key, next_module);
    if (inserted) {
        next_module++;
        batch.Add({{"op", "shader"}, {"id", it->second}, {"code", code}});
    }
    return it->second;
}

u32 PageObjects::Pipeline(Gpu::Batch& batch, Gpu::Json desc) {
    const auto [it, inserted] = pipelines.try_emplace(desc.dump(), next_pipeline);
    if (inserted) {
        next_pipeline++;
        desc["op"] = "pipeline";
        desc["id"] = it->second;
        batch.Add(std::move(desc));
    }
    return it->second;
}

PageObjects& GetPageObjects() {
    static PageObjects objects;
    return objects;
}

} // namespace Web
