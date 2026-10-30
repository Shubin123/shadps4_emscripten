// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Helpers the GPU tests share: hand-encoded GCN programs laid out the way the recompiler expects
// to find them, buffer descriptors, and pass/fail reporting.

#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/resource.h"

namespace GcnTest {

constexpr u32 SMovVccHi = 0xBEEB03FF; // s_mov_b32 vcc_hi, literal: shaders start with it
constexpr u32 SEndpgm = 0xBF810000;
constexpr u32 SBarrier = 0xBF8A0000;
constexpr u32 SWaitVm = 0xBF8C0F70;   // s_waitcnt vmcnt(0)
constexpr u32 SWaitLgkm = 0xBF8C007F; // s_waitcnt lgkmcnt(0)

inline int failures = 0;

inline void Check(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += ok ? 0 : 1;
}

// Copies `body` into 256-byte aligned memory (the register holds address >> 8) behind the
// s_mov_b32 vcc_hi header, followed by the OrbShdr footer the header points at.
inline const u32* Assemble(const std::vector<u32>& body, u64 hash) {
    const size_t length = 2 + body.size();
    const size_t footer = (length + 1) & ~size_t{1};
    // aligned_alloc wants a size that's a multiple of the alignment.
    const size_t bytes = (1024 + footer * 4 + 255) & ~size_t{255};
    auto* code = static_cast<u32*>(std::aligned_alloc(256, bytes));
    std::memset(code, 0, bytes);
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

// A buffer V# over `records` elements of `stride` bytes at `data`, as 32-bit floats per
// component (`components` of them; the format decides how a format load converts them).
inline std::array<u32, 4> BufferSharp(const void* data, u32 records, u32 stride = 4,
                                      u32 data_format = 4 /* 32 */) {
    AmdGpu::Buffer sharp{};
    sharp.base_address = reinterpret_cast<u64>(data);
    sharp.stride = stride;
    sharp.num_records = records;
    sharp.dst_sel_x = 4;
    sharp.dst_sel_y = 5;
    sharp.dst_sel_z = 6;
    sharp.dst_sel_w = 7;
    sharp.num_format = 7; // float
    sharp.data_format = data_format;
    std::array<u32, 4> dwords;
    std::memcpy(dwords.data(), &sharp, sizeof(dwords));
    return dwords;
}

} // namespace GcnTest
