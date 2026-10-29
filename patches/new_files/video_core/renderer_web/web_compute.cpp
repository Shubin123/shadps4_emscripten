// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>
#include "common/logging/log.h"
#include "core/wasm_high_memory.h"
#include "shader_recompiler/backend/wgsl/emit_wgsl.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/renderer_web/web_compute.h"
#include "video_core/renderer_web/web_gpu.h"

namespace Web {

namespace {

namespace WGSL = Shader::Backend::WGSL;

// Larger guest buffers are bound truncated (WebGPU's own limit is lower on many adapters).
constexpr u64 MaxBindingSize = 256_MB;
// Linear memory ends here; guest addresses below it are host addresses.
constexpr u64 LinearMemoryEnd = 16_GB;

// RuntimeInfo has no default constructor (its stage infos are in unions); Initialize() memsets it.
struct RuntimeInfoStorage {
    alignas(Shader::RuntimeInfo) u8 bytes[sizeof(Shader::RuntimeInfo)];
    Shader::RuntimeInfo& Get() {
        return *reinterpret_cast<Shader::RuntimeInfo*>(bytes);
    }
};

// As PipelineCache::BuildRuntimeInfo does for HwStage::Compute.
void BuildRuntimeInfo(Shader::RuntimeInfo& info, const AmdGpu::ComputeProgram& cs) {
    info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    info.props.num_user_data = cs.settings.num_user_regs;
    info.props.num_allocated_vgprs = cs.settings.num_vgprs * 4;
    info.props.fp_denorm_mode32 = cs.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs.settings.fp_round_mode64;
    info.props.dx10_clamp = cs.settings.dx10_clamp;
    info.hw.cs.workgroup_size = {cs.num_thread_x.full, cs.num_thread_y.full, cs.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs.IsTgidEnabled(0), cs.IsTgidEnabled(1), cs.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs.SharedMemSize();
}

// Guest memory for a binding: linear memory directly, or a gathered copy for the translated
// high region (core/wasm_high_memory.h), scattered back after the dispatch if written.
struct GuestRange {
    VAddr address;
    u64 size;
    u8* host{};
    std::vector<u8> copy;

    bool Acquire() {
        if (!Core::HighMemory::InRange(address)) {
            if (address + size > LinearMemoryEnd) {
                return false;
            }
            host = reinterpret_cast<u8*>(address);
            return true;
        }
        copy.assign(size, 0);
        ForEachPage([this](u8* page, u64 offset, u64 n) { std::memcpy(&copy[offset], page, n); });
        host = copy.data();
        return true;
    }

    void Release(bool written) {
        if (written && !copy.empty()) {
            ForEachPage(
                [this](u8* page, u64 offset, u64 n) { std::memcpy(page, &copy[offset], n); });
        }
    }

    void ForEachPage(const std::function<void(u8*, u64, u64)>& func) {
        using Core::HighMemory::kPageSize;
        for (u64 offset = 0; offset < size;) {
            const VAddr va = address + offset;
            const u64 n = std::min(size - offset, kPageSize - (va & (kPageSize - 1)));
            if (u8* page = Core::HighMemory::Translate(va)) {
                func(page, offset, n);
            }
            offset += n;
        }
    }
};

} // namespace

struct ComputePipelines::Permutation {
    Shader::StageSpecialization spec;
    WGSL::Output out;
    u64 key{};
    bool usable{};
};

struct ComputePipelines::Program {
    explicit Program(const Shader::ShaderParams& params)
        : info{Shader::HwStage::Compute, Shader::SwStage::Compute, params} {}

    Shader::Info info;
    std::vector<std::unique_ptr<Permutation>> permutations;
    bool reported{};
};

ComputePipelines::ComputePipelines() = default;
ComputePipelines::~ComputePipelines() = default;

bool ComputePipelines::Dispatch(const AmdGpu::ComputeProgram& cs, std::array<u32, 3> groups,
                                std::span<u32> gds) {
    if (!Gpu::Available() || groups[0] == 0 || groups[1] == 0 || groups[2] == 0) {
        return false;
    }
    static RuntimeInfoStorage runtime_storage;
    auto& runtime_info = runtime_storage.Get();
    BuildRuntimeInfo(runtime_info, cs);
    static const Shader::Profile profile = WGSL::MakeProfile();

    const auto params = AmdGpu::GetParams(cs);
    const auto compile = [&](Shader::Info& info) {
        auto perm = std::make_unique<Permutation>();
        Shader::Pools pools;
        const auto program = Shader::TranslateProgram(params.code, pools, info, runtime_info,
                                                      profile);
        perm->out = WGSL::EmitWGSL(profile, runtime_info, program);
        perm->key = std::hash<std::string>{}(perm->out.code);
        perm->usable = perm->out.errors.empty();
        for (const auto& b : perm->out.bindings) {
            if (b.kind != WGSL::Binding::Kind::Buffer) {
                perm->out.errors.push_back("textures and samplers aren't bound yet");
                perm->usable = false;
                break;
            }
        }
        LOG_INFO(Render, "WebGPU: compute shader {:#x} -> WGSL ({} bindings){}", params.hash,
                 perm->out.bindings.size(), perm->usable ? "" : ", not usable");
        return perm;
    };

    auto [it, inserted] = programs.try_emplace(params.hash);
    Permutation* perm = nullptr;
    if (inserted) {
        it->second = std::make_unique<Program>(params);
        auto& program = *it->second;
        program.permutations.push_back(compile(program.info));
        perm = program.permutations.back().get();
        perm->spec = Shader::StageSpecialization(program.info, runtime_info, profile, {});
    } else {
        auto& info = it->second->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        Shader::StageSpecialization spec(info, runtime_info, profile, {});
        auto& perms = it->second->permutations;
        const auto found =
            std::ranges::find_if(perms, [&](const auto& p) { return p->spec == spec; });
        if (found != perms.end()) {
            perm = found->get();
        } else {
            Shader::Info new_info(Shader::HwStage::Compute, Shader::SwStage::Compute, params);
            perms.push_back(compile(new_info));
            perm = perms.back().get();
            perm->spec = std::move(spec);
        }
    }
    Program& program = *it->second;
    if (!perm->usable) {
        if (!std::exchange(program.reported, true)) {
            for (const auto& error : perm->out.errors) {
                LOG_ERROR(Render, "WebGPU: compute shader {:#x} dropped: {}", params.hash, error);
            }
        }
        return false;
    }

    // Bind every buffer the shader declares (the WGSL bindings follow Info::buffers' order).
    const Shader::Info& info = program.info;
    static u32 null_buffer[4]{};
    std::vector<GuestRange> ranges;
    ranges.reserve(perm->out.bindings.size());
    std::vector<Gpu::BufferBinding> bindings;
    for (const auto& b : perm->out.bindings) {
        const auto& desc = info.buffers[b.resource];
        Gpu::BufferBinding binding{b.binding, null_buffer, sizeof(null_buffer), b.writable};
        switch (desc.buffer_type) {
        case Shader::BufferType::Guest: {
            const auto sharp = desc.GetSharp(info);
            u64 size = sharp.GetSize();
            if (sharp.base_address == 0 || size == 0) {
                binding.writable = false;
                break;
            }
            if (sharp.base_address & 3) {
                // The WGSL backend indexes buffers in dwords from the bound base.
                LOG_ERROR(Render, "WebGPU: compute shader {:#x}: buffer at {:#x} isn't dword "
                                  "aligned, dispatch dropped",
                          params.hash, u64(sharp.base_address));
                return false;
            }
            if (size > MaxBindingSize) {
                LOG_WARNING(Render, "WebGPU: buffer at {:#x} truncated from {} to {} bytes",
                            u64(sharp.base_address), size, MaxBindingSize);
                size = MaxBindingSize;
            }
            auto& range = ranges.emplace_back(GuestRange{sharp.base_address, size});
            if (!range.Acquire()) {
                LOG_ERROR(Render, "WebGPU: buffer at {:#x} is outside guest memory",
                          u64(sharp.base_address));
                return false;
            }
            binding.data = range.host;
            binding.size = size;
            break;
        }
        case Shader::BufferType::Flatbuf:
            binding.data = const_cast<u32*>(info.flattened_ud_buf.data());
            binding.size = info.flattened_ud_buf.size() * sizeof(u32);
            binding.writable = false;
            break;
        case Shader::BufferType::GdsBuffer:
            binding.data = gds.data();
            binding.size = gds.size_bytes();
            break;
        default:
            LOG_ERROR(Render, "WebGPU: compute shader {:#x} uses buffer type {}, dispatch dropped",
                      params.hash, u32(desc.buffer_type));
            return false;
        }
        bindings.push_back(binding);
    }

    const bool ok = Gpu::RunCompute(perm->out.code, perm->key, groups, bindings);
    size_t r = 0;
    for (size_t i = 0; i < bindings.size(); i++) {
        if (info.buffers[perm->out.bindings[i].resource].buffer_type == Shader::BufferType::Guest &&
            bindings[i].data != null_buffer) {
            ranges[r++].Release(ok && bindings[i].writable);
        }
    }
    return ok;
}

} // namespace Web
