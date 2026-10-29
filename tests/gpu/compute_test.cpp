// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WebGPU compute path, end to end: hand-assembled GCN compute programs are set up the way a game
// leaves them in the COMPUTE_* registers (program address, user data holding buffer V#s,
// workgroup size, LDS size) and dispatched through Web::ComputePipelines - the code
// Rasterizer::DispatchDirect calls - in a browser with WebGPU (run_gpu_tests.mjs). That covers the
// recompiler, the WGSL backend, the bindings and the page's executor, and the results are checked
// in guest memory afterwards.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>
#include "common/logging/log.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_web/web_compute.h"
#include "video_core/renderer_web/web_gpu.h"

namespace {

constexpr u32 SMovVccHi = 0xBEEB03FF; // s_mov_b32 vcc_hi, literal: shaders start with it
constexpr u32 SEndpgm = 0xBF810000;
constexpr u32 SBarrier = 0xBF8A0000;
constexpr u32 SWaitVm = 0xBF8C0F70;   // s_waitcnt vmcnt(0)
constexpr u32 SWaitLgkm = 0xBF8C007F; // s_waitcnt lgkmcnt(0)

// Copies `body` into 256-byte aligned memory (the register holds address >> 8) behind the
// s_mov_b32 vcc_hi header, followed by the OrbShdr footer the header points at.
const u32* Assemble(const std::vector<u32>& body, u64 hash) {
    const size_t length = 2 + body.size();
    const size_t footer = (length + 1) & ~size_t{1};
    auto* code = static_cast<u32*>(std::aligned_alloc(256, 1024 + footer * 4));
    std::memset(code, 0, 1024 + footer * 4);
    code[0] = SMovVccHi;
    code[1] = static_cast<u32>(footer / 2 - 1);
    std::memcpy(code + 2, body.data(), body.size() * 4);
    AmdGpu::BinaryInfo info{};
    std::memcpy(info.signature.data(), AmdGpu::BinaryInfo::signature_ref.data(),
                info.signature.size());
    info.length = static_cast<u32>(length * 4);
    info.shader_hash = hash;
    std::memcpy(code + footer, &info, sizeof(info));
    return code;
}

// A raw (untyped) buffer V# over `words` dwords at `data`.
std::array<u32, 4> BufferSharp(const u32* data, u32 words) {
    AmdGpu::Buffer sharp{};
    sharp.base_address = reinterpret_cast<u64>(data);
    sharp.stride = 4;
    sharp.num_records = words;
    sharp.dst_sel_x = 4;
    sharp.dst_sel_y = 5;
    sharp.dst_sel_z = 6;
    sharp.dst_sel_w = 7;
    sharp.num_format = 7;  // float
    sharp.data_format = 4; // 32
    std::array<u32, 4> dwords;
    std::memcpy(dwords.data(), &sharp, sizeof(dwords));
    return dwords;
}

AmdGpu::ComputeProgram Program(const u32* code, u32 user_regs, u32 threads, u32 lds_bytes) {
    AmdGpu::ComputeProgram cs{};
    cs.address = reinterpret_cast<u64>(code) >> 8;
    cs.settings.num_vgprs = 2;
    cs.settings.num_user_regs = user_regs;
    cs.settings.tgid_enable = 1;
    cs.settings.lds_dwords = (lds_bytes / 4 + 127) / 128;
    cs.num_thread_x.full = static_cast<u16>(threads);
    cs.num_thread_y.full = 1;
    cs.num_thread_z.full = 1;
    return cs;
}

int failures = 0;

void Check(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += ok ? 0 : 1;
}

// out[gid] = in[gid] + in[gid ^ 1] + 1, the neighbour's value passed through LDS across an
// s_barrier. Four workgroups of 64: the workgroup id (tgid_x, s8) feeds the global index.
void TestLdsExchange(std::span<u32> gds) {
    const u32* code = Assemble(
        {
            0x8F098608, // s_lshl_b32 s9, s8, 6              ; workgroup base
            0x4A020009, // v_add_i32 v1, vcc, s9, v0         ; gid
            0x34040282, // v_lshlrev_b32 v2, 2, v1           ; byte offset
            0xE0301000, 0x80000302, // buffer_load_dword v3, v2, s[0:3], 0 offen
            SWaitVm,
            0x34080082, // v_lshlrev_b32 v4, 2, v0           ; LDS address
            0xD8340000, 0x00000304, // ds_write_b32 v4, v3
            SWaitLgkm,
            SBarrier,
            0x3A0A0884, // v_xor_b32 v5, 4, v4               ; neighbour's LDS address
            0xD8D80000, 0x06000005, // ds_read_b32 v6, v5
            SWaitLgkm,
            0x4A0E0D03, // v_add_i32 v7, vcc, v3, v6
            0x4A0E0E81, // v_add_i32 v7, vcc, 1, v7
            0xE0701000, 0x80010702, // buffer_store_dword v7, v2, s[4:7], 0 offen
            SEndpgm,
        },
        0x7e57000000000001ULL);
    constexpr u32 N = 256;
    std::vector<u32> in(N), out(N, 0xDEADBEEF);
    for (u32 i = 0; i < N; i++) {
        in[i] = i * 7 + 3;
    }
    auto cs = Program(code, 8, 64, 64 * 4);
    const auto in_sharp = BufferSharp(in.data(), N);
    const auto out_sharp = BufferSharp(out.data(), N);
    std::memcpy(&cs.user_data[0], in_sharp.data(), 16);
    std::memcpy(&cs.user_data[4], out_sharp.data(), 16);

    Web::ComputePipelines pipelines;
    const bool ran = pipelines.Dispatch(cs, {N / 64, 1, 1}, gds);
    Check("lds-exchange: dispatch ran", ran);
    u32 wrong = 0;
    for (u32 i = 0; i < N; i++) {
        if (out[i] != in[i] + in[i ^ 1] + 1) {
            if (wrong++ < 4) {
                std::printf("  out[%u] = %u, expected %u\n", i, out[i], in[i] + in[i ^ 1] + 1);
            }
        }
    }
    Check("lds-exchange: results", ran && wrong == 0);

    // Same program, new buffers: the cached translation is reused with the new V#s.
    std::vector<u32> in2(N, 5), out2(N, 0);
    const auto in2_sharp = BufferSharp(in2.data(), N);
    const auto out2_sharp = BufferSharp(out2.data(), N);
    std::memcpy(&cs.user_data[0], in2_sharp.data(), 16);
    std::memcpy(&cs.user_data[4], out2_sharp.data(), 16);
    const bool ran2 = pipelines.Dispatch(cs, {N / 64, 1, 1}, gds);
    Check("lds-exchange: second dispatch, rebound buffers",
          ran2 && std::all_of(out2.begin(), out2.end(), [](u32 v) { return v == 11; }));
}

// A loop with a per-lane trip count ((tid & 3) + 1, the exec mask shrinking as lanes finish)
// and an s_barrier in its body - the shape the WGSL backend has to turn into a loop with a
// workgroup-uniform exit. Every lane must count exactly its own trips.
void TestDivergentLoopWithBarrier(std::span<u32> gds) {
    const u32* code = Assemble(
        {
            0x36020083, // v_and_b32 v1, 3, v0
            0x4A020281, // v_add_i32 v1, vcc, 1, v1          ; trips
            0x7E040280, // v_mov_b32 v2, 0                    ; counter
            0xBE8A047E, // s_mov_b64 s[10:11], exec
            // loop:
            0x4A040481, // v_add_i32 v2, vcc, 1, v2
            SBarrier,
            0x7D820302, // v_cmp_lt_u32 vcc, v2, v1
            0x87FE6A7E, // s_and_b64 exec, exec, vcc
            0xBF89FFFB, // s_cbranch_execnz loop
            0xBEFE040A, // s_mov_b64 exec, s[10:11]
            0x34060082, // v_lshlrev_b32 v3, 2, v0
            0xE0701000, 0x80000203, // buffer_store_dword v2, v3, s[0:3], 0 offen
            SEndpgm,
        },
        0x7e57000000000002ULL);
    constexpr u32 N = 64;
    std::vector<u32> out(N, 0xDEADBEEF);
    auto cs = Program(code, 4, N, 0);
    const auto sharp = BufferSharp(out.data(), N);
    std::memcpy(&cs.user_data[0], sharp.data(), 16);
    Web::ComputePipelines pipelines;
    const bool ran = pipelines.Dispatch(cs, {1, 1, 1}, gds);
    Check("divergent-loop-barrier: dispatch ran", ran);
    u32 wrong = 0;
    for (u32 i = 0; i < N; i++) {
        if (out[i] != (i & 3) + 1 && wrong++ < 4) {
            std::printf("  out[%u] = %u, expected %u\n", i, out[i], (i & 3) + 1);
        }
    }
    Check("divergent-loop-barrier: results", ran && wrong == 0);
}

} // namespace

int main() {
    Common::Log::Setup("gpu_tests.log"); // the page's console gets the emulator's log too
    // The page hands environment variables in as Module.shadps4Env (emscripten/pre.js), the way
    // shadps4_web's ?env= does; main runs on a pthread here, as in the emulator.
    const char* env = std::getenv("SHADPS4_GPU_TEST_ENV");
    Check("environment from the page reaches getenv", env && std::strcmp(env, "page") == 0);
    if (!Web::Gpu::Available()) {
        std::printf("GPU TESTS: no WebGPU device\n");
        return 2;
    }
    static std::array<u32, 0x10000 / 4> gds{};
    TestLdsExchange(gds);
    TestDivergentLoopWithBarrier(gds);
    std::printf("GPU TESTS: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
