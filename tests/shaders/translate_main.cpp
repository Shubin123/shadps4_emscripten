// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs real PS4 GCN shaders through shadPS4's shader recompiler and the WGSL backend, to check the
// backend against what games actually contain rather than hand-written cases. Shaders are found in
// a game executable by their OrbShdr footer (the same footer shadPS4 locates at draw time).
//
//   node translate_shaders.js <eboot.bin> <out dir>            list shaders
//   node translate_shaders.js <eboot.bin> <out dir> <index>    translate one; writes <out>/<n>.wgsl
//
// One shader per process: a recompiler assertion aborts the whole wasm instance, and the driver
// (run_shader_tests.sh) should record that as one failure, not lose the rest.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "common/logging/log.h"
#include "shader_recompiler/backend/wgsl/emit_wgsl.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_shader.h"

namespace {

struct Found {
    size_t offset; // code start in the file
    u32 length;    // code bytes
    u64 hash;
    Shader::SwStage stage;
    const char* stage_name;
};

std::vector<u8> ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

// VS export position, PS export colour/depth, CS export nothing.
bool GuessStage(const u32* code, size_t dwords, Shader::SwStage& stage, const char*& name) {
    Shader::Gcn::GcnCodeSlice slice(code, code + dwords);
    Shader::Gcn::GcnDecodeContext decoder;
    bool pos = false, mrt = false;
    while (!slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(slice);
        if (inst.category == Shader::Gcn::InstCategory::Export) {
            const u32 target = inst.control.exp.target;
            if (target >= 12 && target <= 15) {
                pos = true;
            } else if (target <= 8) {
                mrt = true;
            }
        }
        if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
    if (pos) {
        stage = Shader::SwStage::Vertex;
        name = "vs";
    } else if (mrt) {
        stage = Shader::SwStage::Fragment;
        name = "ps";
    } else {
        stage = Shader::SwStage::Compute;
        name = "cs";
    }
    return true;
}

std::vector<Found> FindShaders(const std::vector<u8>& data) {
    std::vector<Found> found;
    for (size_t i = 0; i + sizeof(AmdGpu::BinaryInfo) <= data.size(); i++) {
        if (std::memcmp(&data[i], AmdGpu::BinaryInfo::signature_ref.data(), 7) != 0) {
            continue;
        }
        AmdGpu::BinaryInfo bi;
        std::memcpy(&bi, &data[i], sizeof(bi));
        if (bi.length == 0 || bi.length % 4 != 0) {
            continue;
        }
        // Code starts with `s_mov_b32 vcc_hi, N` and the footer sits (N + 1) * 8 bytes after it
        // (AmdGpu::SearchBinaryInfo); `length` counts only the instructions before padding.
        size_t start = ~size_t{0};
        for (size_t back = 8; back <= 0x40000 && back <= i; back += 4) {
            u32 w[2];
            std::memcpy(w, &data[i - back], 8);
            if (w[0] == 0xBEEB03FF && (size_t(w[1]) + 1) * 8 == back) {
                start = i - back;
                break;
            }
        }
        if (start == ~size_t{0} || start + bi.length > i) {
            continue;
        }
        Found f{start, bi.length, bi.shader_hash};
        GuessStage(reinterpret_cast<const u32*>(&data[start]), bi.length / 4, f.stage,
                   f.stage_name);
        found.push_back(f);
    }
    return found;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: translate_shaders <eboot.bin> <out dir> [index]\n");
        return 2;
    }
    const bool trace = std::getenv("TRACE") != nullptr;
    if (trace) {
        Common::Log::Setup("translate_shaders.log");
    }
    const auto data = ReadFile(argv[1]);
    const auto shaders = FindShaders(data);
    if (argc == 3) {
        for (size_t i = 0; i < shaders.size(); i++) {
            std::printf("%zu %s offset=%#zx length=%u hash=%#018llx\n", i, shaders[i].stage_name,
                        shaders[i].offset, shaders[i].length,
                        static_cast<unsigned long long>(shaders[i].hash));
        }
        return 0;
    }
    const size_t index = std::strtoul(argv[3], nullptr, 10);
    if (index >= shaders.size()) {
        return 2;
    }
    const auto& sh = shaders[index];

    // Keep the code in an aligned buffer that outlives translation (Info refers into it).
    static std::vector<u32> code(sh.length / 4);
    std::memcpy(code.data(), &data[sh.offset], sh.length);
    static std::array<u32, 16> user_data{};
    const Shader::ShaderParams params{std::span<const u32, 16>(user_data), std::span{code},
                                      sh.hash};

    const auto hw = sh.stage == Shader::SwStage::Vertex     ? Shader::HwStage::Vertex
                    : sh.stage == Shader::SwStage::Fragment ? Shader::HwStage::Fragment
                                                          : Shader::HwStage::Compute;
    Shader::Info info(hw, sh.stage, params);
    // RuntimeInfo has no default constructor (unions of stage infos); Initialize() memsets it.
    alignas(Shader::RuntimeInfo) static u8 rt_storage[sizeof(Shader::RuntimeInfo)];
    auto& rt = *reinterpret_cast<Shader::RuntimeInfo*>(rt_storage);
    rt.Initialize(hw, sh.stage);
    // What a draw/dispatch normally supplies from registers (PGM_RSRC*, SPI_PS_INPUT_*),
    // set to typical values: all 16 user SGPRs, all thread-group ids, perspective-center
    // barycentrics plus fragment position for pixel shaders.
    rt.props.num_user_data = 16;
    rt.props.num_input_vgprs = 3;
    if (hw == Shader::HwStage::Compute) {
        rt.hw.cs.workgroup_size = {64, 1, 1};
        rt.hw.cs.shared_memory_size = 16384;
        rt.hw.cs.tgid_enable = {true, true, true};
    }
    if (hw == Shader::HwStage::Fragment) {
        auto& f = rt.hw.fs;
        f.en_flags.persp_center_ena = 1;
        f.en_flags.pos_x_float_ena = f.en_flags.pos_y_float_ena = 1;
        f.addr_flags = f.en_flags;
        f.num_inputs = 8;
        for (u32 i = 0; i < f.num_inputs; i++) {
            f.inputs[i].param_index = i;
        }
    }
    const auto profile = Shader::Backend::WGSL::MakeProfile();
    Shader::Pools pools;
    if (trace) std::fprintf(stderr, "translating %zu\n", index);
    const auto program = Shader::TranslateProgram(code, pools, info, rt, profile);
    if (trace) std::fprintf(stderr, "translated; emitting\n");
    const auto out = Shader::Backend::WGSL::EmitWGSL(profile, rt, program);

    const std::string path = std::string(argv[2]) + "/" + std::to_string(index) + "_" +
                             sh.stage_name + ".wgsl";
    std::ofstream(path) << out.code;
    std::printf("%zu %s hash=%#018llx dwords=%u bindings=%zu wgsl_lines=%zu errors=%zu\n", index,
                sh.stage_name, static_cast<unsigned long long>(sh.hash), sh.length / 4,
                out.bindings.size(), std::count(out.code.begin(), out.code.end(), '\n'),
                out.errors.size());
    for (const auto& e : out.errors) {
        std::printf("  unsupported: %s\n", e.c_str());
    }
    return out.errors.empty() ? 0 : 1;
}
