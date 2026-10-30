// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WebGPU draw path, end to end: hand-assembled GCN vertex and pixel shaders, with the Liverpool
// registers set the way a game's command buffer leaves them, drawn through Vulkan::Rasterizer
// (the class Liverpool calls) into a linear RGBA8 render target in guest memory. The target is
// read back the way the presenter reads a display buffer, and checked pixel by pixel.
//
// The vertex shader fetches its attributes through a fetch shader called with s_swappc, as
// compiled PS4 vertex shaders do, so the inline fetch translation, the SRT walk to the vertex
// buffer table and the WGSL vertex linking are all on the path.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_web/web_rasterizer.h"
#include "video_core/texture_cache/tile.h"
#include "gcn_test.h"

using namespace GcnTest;

namespace {

constexpr u32 W = 64;
constexpr u32 H = 64;

u32 Exp(u32 target, bool done, bool vm) {
    return 0xF8000000u | (vm ? 1u << 12 : 0) | (done ? 1u << 11 : 0) | (target << 4) | 0xFu;
}
constexpr u32 ExpSrc(u32 v) { // v, v+1, v+2, v+3
    return v | (v + 1) << 8 | (v + 2) << 16 | (v + 3) << 24;
}
constexpr u32 Interp(u32 op, u32 vdst, u32 attr, u32 chan, u32 vsrc) {
    return 0xC8000000u | vdst << 18 | op << 16 | attr << 10 | chan << 8 | vsrc;
}
constexpr u32 ExpPos0 = 12;
constexpr u32 ExpParam0 = 32;
constexpr u32 ExpMrt0 = 0;

// Fetch shader: two attributes (V#s 0 and 1 of the table at s[2:3]) into v[4:7] and v[8:11].
const u32* FetchShader() {
    static const u32* code = [] {
        auto* f = static_cast<u32*>(std::aligned_alloc(256, 256));
        const u32 body[] = {
            0xC0840300, // s_load_dwordx4 s[8:11], s[2:3], 0x0
            0xC0860304, // s_load_dwordx4 s[12:15], s[2:3], 0x4
            SWaitLgkm,
            0xE00C2000, 0x80020400, // buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
            0xE00C2000, 0x80030800, // buffer_load_format_xyzw v[8:11], v0, s[12:15], 0 idxen
            SWaitVm,
            0xBE802000, // s_setpc_b64 s[0:1]
        };
        std::memcpy(f, body, sizeof(body));
        return f;
    }();
    return code;
}

// Vertex shader: calls the fetch shader, exports v[4:7] as the position and v[8:11] as param0.
const u32* VertexShader() {
    static const u32* code = Assemble(
        {
            0xBE802100, // s_swappc_b64 s[0:1], s[0:1]
            Exp(ExpParam0, false, false), ExpSrc(8),
            Exp(ExpPos0, true, false), ExpSrc(4),
            SEndpgm,
        },
        0x7e57000000000010ULL);
    return code;
}

// Pixel shader: param0, interpolated, is the color.
const u32* ColorPixelShader() {
    static const u32* code = Assemble(
        {
            Interp(0, 2, 0, 0, 0), Interp(1, 2, 0, 0, 1), // v2 = param0.x
            Interp(0, 3, 0, 1, 0), Interp(1, 3, 0, 1, 1), // v3 = param0.y
            Interp(0, 4, 0, 2, 0), Interp(1, 4, 0, 2, 1), // v4 = param0.z
            Interp(0, 5, 0, 3, 0), Interp(1, 5, 0, 3, 1), // v5 = param0.w
            Exp(ExpMrt0, true, true), ExpSrc(2),
            SEndpgm,
        },
        0x7e57000000000011ULL);
    return code;
}

// Pixel shader: samples the T# in s[0:7] with the S# in s[8:11] at param0.xy.
const u32* TexturePixelShader() {
    static const u32* code = Assemble(
        {
            Interp(0, 2, 0, 0, 0), Interp(1, 2, 0, 0, 1), // v2 = u
            Interp(0, 3, 0, 1, 0), Interp(1, 3, 0, 1, 1), // v3 = v
            0xF0800F00, 0x00400402, // image_sample v[4:7], v[2:3], s[0:7], s[8:11] dmask:0xf
            SWaitVm,
            Exp(ExpMrt0, true, true), ExpSrc(4),
            SEndpgm,
        },
        0x7e57000000000012ULL);
    return code;
}

struct Vertex {
    float pos[4];
    float attr[4];
};

// Guest memory a draw reads, kept alive for the test's duration.
struct Scene {
    std::vector<Vertex> vertices;
    alignas(16) u32 table[8]; // the vertex buffer V#s
};

u8* NewTarget() {
    auto* rt = static_cast<u8*>(std::aligned_alloc(256, W * H * 4)); // a multiple of 256
    std::memset(rt, 0, W * H * 4);
    return rt;
}

// The registers every test starts from: one linear RGBA8 target, a D3D-style viewport over it,
// no depth, no blending, triangle lists.
void BaseState(AmdGpu::Liverpool& lp, u8* target) {
    auto& r = lp.regs;
    r = {};
    auto& cb = r.color_buffers[0];
    cb.base_address = static_cast<u32>(reinterpret_cast<u64>(target) >> 8);
    cb.pitch.tile_max = W / 8 - 1;
    cb.slice.tile_max = W * H / 64 - 1;
    cb.info.format = static_cast<u32>(AmdGpu::DataFormat::Format8_8_8_8);
    cb.info.number_type = static_cast<u32>(AmdGpu::NumberFormat::Unorm);
    cb.attrib.tile_mode_index = AmdGpu::TileMode::DisplayLinearAligned;
    r.color_target_mask.raw = 0xF;
    r.color_shader_mask.raw = 0xF;
    r.color_control.mode = AmdGpu::ColorControl::OperationMode::Normal;
    r.color_export_format.raw = static_cast<u32>(AmdGpu::ShaderExportFormat::ABGR_32);

    r.viewport_control.xscale_enable = 1;
    r.viewport_control.xoffset_enable = 1;
    r.viewport_control.yscale_enable = 1;
    r.viewport_control.yoffset_enable = 1;
    r.viewport_control.zscale_enable = 1;
    r.viewport_control.zoffset_enable = 1;
    r.viewports[0] = {W / 2.0f, W / 2.0f, -(H / 2.0f), H / 2.0f, 1.0f, 0.0f};
    r.clipper_control.clip_space = AmdGpu::ClipSpace::ZeroToW;
    for (auto* s : {&r.window_scissor, &r.generic_scissor}) {
        s->top_left_x = 0;
        s->top_left_y = 0;
        s->bottom_right_x = W;
        s->bottom_right_y = H;
        s->window_offset_disable = 1;
    }
    r.screen_scissor.top_left_x = 0;
    r.screen_scissor.top_left_y = 0;
    r.screen_scissor.bottom_right_x = W;
    r.screen_scissor.bottom_right_y = H;
    r.primitive_type = AmdGpu::PrimitiveType::TriangleList;
    r.num_instances.num_instances = 1;
    r.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::Vs;
    r.max_index_size = 0xFFFFFFFF;
}

void SetShaders(AmdGpu::Liverpool& lp, Scene& scene, const u32* ps, u32 ps_user_regs) {
    auto& r = lp.regs;
    const auto& v = scene.vertices;
    const auto pos = BufferSharp(v.data()->pos, static_cast<u32>(v.size()), sizeof(Vertex), 14);
    const auto attr = BufferSharp(v.data()->attr, static_cast<u32>(v.size()), sizeof(Vertex), 14);
    std::memcpy(scene.table, pos.data(), 16);
    std::memcpy(scene.table + 4, attr.data(), 16);
    r.vs_program.address = reinterpret_cast<u64>(VertexShader()) >> 8;
    r.vs_program.settings.num_vgprs = 2; // 12 VGPRs
    r.vs_program.settings.num_user_regs = 4;
    const u64 fetch = reinterpret_cast<u64>(FetchShader());
    const u64 table = reinterpret_cast<u64>(scene.table);
    r.vs_program.user_data[0] = static_cast<u32>(fetch);
    r.vs_program.user_data[1] = static_cast<u32>(fetch >> 32);
    r.vs_program.user_data[2] = static_cast<u32>(table);
    r.vs_program.user_data[3] = static_cast<u32>(table >> 32);
    r.vs_output_config.export_count_min_one = 0;
    r.ps_program.address = reinterpret_cast<u64>(ps) >> 8;
    r.ps_program.settings.num_vgprs = 1; // 8 VGPRs
    r.ps_program.settings.num_user_regs = ps_user_regs;
    r.ps_input_ena.persp_center_ena = 1;
    r.ps_input_addr.persp_center_ena = 1;
    r.num_interp = 1;
    r.ps_inputs[0].input_offset = 0;
}

std::vector<u8> ReadBack(Vulkan::Rasterizer& rast, u8* target) {
    rast.OnSubmit();
    std::vector<u8> pixels;
    if (!rast.ReadScanout(reinterpret_cast<VAddr>(target), W, H, 4, pixels)) {
        pixels.clear();
    }
    return pixels;
}

bool Near(const u8* px, std::array<int, 4> want, int tolerance = 1) {
    for (int c = 0; c < 4; c++) {
        if (std::abs(int(px[c]) - want[c]) > tolerance) {
            return false;
        }
    }
    return true;
}

// Counts pixels where `expected(x, y)` doesn't match, printing the first few.
template <typename F>
u32 CountWrong(const std::vector<u8>& pixels, F&& expected, int tolerance = 1) {
    if (pixels.size() != W * H * 4) {
        std::printf("  no pixels read back\n");
        return W * H;
    }
    u32 wrong = 0;
    for (u32 y = 0; y < H; y++) {
        for (u32 x = 0; x < W; x++) {
            const u8* px = &pixels[(y * W + x) * 4];
            const auto want = expected(x, y);
            if (!Near(px, want, tolerance) && wrong++ < 4) {
                std::printf("  (%u,%u) = %d %d %d %d, expected %d %d %d %d\n", x, y, px[0], px[1],
                            px[2], px[3], want[0], want[1], want[2], want[3]);
            }
        }
    }
    return wrong;
}

// A triangle over the whole target, one color: the vertex fetch, the viewport transform and the
// export path.
void TestFlatTriangle(AmdGpu::Liverpool& lp, Vulkan::Rasterizer& rast) {
    u8* target = NewTarget();
    static Scene scene;
    scene.vertices = {{{-1, -1, 0, 1}, {0.25f, 0.5f, 0.75f, 1}},
                      {{3, -1, 0, 1}, {0.25f, 0.5f, 0.75f, 1}},
                      {{-1, 3, 0, 1}, {0.25f, 0.5f, 0.75f, 1}}};
    BaseState(lp, target);
    SetShaders(lp, scene, ColorPixelShader(), 0);
    lp.regs.num_indices = 3;
    rast.Draw(false);
    const auto pixels = ReadBack(rast, target);
    Check("draw: flat triangle reaches every pixel with its color",
          CountWrong(pixels, [](u32, u32) { return std::array{64, 128, 191, 255}; }) == 0);
}

// A quad list (WebGPU has none: rewritten as triangles), with red going 0 -> 1 left to right,
// first auto-indexed, then through a 16-bit index buffer.
void TestQuadGradient(AmdGpu::Liverpool& lp, Vulkan::Rasterizer& rast) {
    static Scene scene;
    scene.vertices = {{{-1, -1, 0, 1}, {0, 0, 0, 1}},
                      {{1, -1, 0, 1}, {1, 0, 0, 1}},
                      {{1, 1, 0, 1}, {1, 0, 0, 1}},
                      {{-1, 1, 0, 1}, {0, 0, 0, 1}}};
    const auto expected = [](u32 x, u32) {
        return std::array{int(std::lround((x + 0.5) / W * 255.0)), 0, 0, 255};
    };
    for (const bool indexed : {false, true}) {
        u8* target = NewTarget();
        BaseState(lp, target);
        SetShaders(lp, scene, ColorPixelShader(), 0);
        lp.regs.primitive_type = AmdGpu::PrimitiveType::QuadList;
        lp.regs.num_indices = 4;
        static const u16 indices[4] = {0, 1, 2, 3};
        if (indexed) {
            const u64 address = reinterpret_cast<u64>(indices);
            lp.regs.index_base_address.base_addr_lo = static_cast<u32>(address);
            lp.regs.index_base_address.base_addr_hi = static_cast<u32>(address >> 32);
            lp.regs.index_buffer_type.index_type = AmdGpu::IndexType::Index16;
        }
        rast.Draw(indexed);
        const auto pixels = ReadBack(rast, target);
        Check(indexed ? "draw: indexed quad list, interpolated across the target"
                      : "draw: quad list, interpolated across the target",
              CountWrong(pixels, expected, 2) == 0);
    }
}

// A rectangle list in window coordinates (clipping and the viewport transform off, as games
// draw fullscreen passes), over a target fast-cleared through its CMASK: the 4th corner comes
// from the vertex shader, and the rest of the target has the clear color.
void TestRectOverFastClear(AmdGpu::Liverpool& lp, Vulkan::Rasterizer& rast) {
    u8* target = NewTarget();
    static Scene scene;
    scene.vertices = {{{16, 16, 0, 1}, {1, 0, 0, 1}},
                      {{48, 16, 0, 1}, {1, 0, 0, 1}},
                      {{16, 48, 0, 1}, {1, 0, 0, 1}}};
    BaseState(lp, target);
    SetShaders(lp, scene, ColorPixelShader(), 0);
    auto& r = lp.regs;
    r.primitive_type = AmdGpu::PrimitiveType::RectList;
    r.viewport_control = {};
    r.num_indices = 3;
    alignas(256) static u32 cmask[64]{}; // CB_COLOR0_CMASK holds address >> 8
    auto& cb = r.color_buffers[0];
    cb.info.fast_clear = 1;
    cb.cmask_base_address = static_cast<u32>(reinterpret_cast<u64>(cmask) >> 8);
    cb.clear_word0 = 0xFF204000; // r=0x00 g=0x40 b=0x20 a=0xFF
    // The game clears the CMASK (a DMA fill) before drawing into the target.
    rast.FillBuffer(reinterpret_cast<VAddr>(cmask), sizeof(cmask), 0, false);
    rast.Draw(false);
    const auto pixels = ReadBack(rast, target);
    Check("draw: rectangle list in window space over a fast-cleared target",
          CountWrong(pixels, [](u32 x, u32 y) {
              const bool inside = x >= 16 && x < 48 && y >= 16 && y < 48;
              return inside ? std::array{255, 0, 0, 255} : std::array{0, 0x40, 0x20, 255};
          }) == 0);
}

// Alpha blending onto what the target already holds.
void TestBlend(AmdGpu::Liverpool& lp, Vulkan::Rasterizer& rast) {
    u8* target = NewTarget();
    for (u32 i = 0; i < W * H; i++) {
        target[i * 4 + 2] = 255; // blue, written by the CPU before the GPU draws
        target[i * 4 + 3] = 255;
    }
    static Scene scene;
    scene.vertices = {{{-1, -1, 0, 1}, {1, 0, 0, 0.5f}},
                      {{3, -1, 0, 1}, {1, 0, 0, 0.5f}},
                      {{-1, 3, 0, 1}, {1, 0, 0, 0.5f}}};
    BaseState(lp, target);
    SetShaders(lp, scene, ColorPixelShader(), 0);
    using F = AmdGpu::BlendControl::BlendFactor;
    auto& bc = lp.regs.blend_control[0];
    bc.enable = 1;
    bc.color_src_factor = F::SrcAlpha;
    bc.color_dst_factor = F::OneMinusSrcAlpha;
    bc.color_func = AmdGpu::BlendControl::BlendFunc::Add;
    lp.regs.num_indices = 3;
    rast.Draw(false);
    const auto pixels = ReadBack(rast, target);
    Check("draw: alpha blending over the target's previous contents",
          CountWrong(pixels, [](u32, u32) { return std::array{128, 0, 128, 191}; }) == 0);
}

// A pixel shader sampling a 2x2 texture from guest memory (T#/S# in user data), point
// filtered, over a quad: each quadrant of the target gets one texel.
void TestTexture(AmdGpu::Liverpool& lp, Vulkan::Rasterizer& rast) {
    u8* target = NewTarget();
    // The guest layout of a linear 2x2 RGBA8 texture: rows padded the way GCN pads them.
    const auto [pitch, rows, size] = VideoCore::ImageSizeLinearAligned(2, 2, 32, 1);
    auto* texels = static_cast<u32*>(std::aligned_alloc(256, std::max<size_t>(size, 256)));
    std::memset(texels, 0, size);
    texels[0] = 0xFF0000FF;         // (0,0) red
    texels[1] = 0xFF00FF00;         // (1,0) green
    texels[pitch] = 0xFFFF0000;     // (0,1) blue
    texels[pitch + 1] = 0xFFFFFFFF; // (1,1) white
    AmdGpu::Image t{};
    t.base_address = reinterpret_cast<u64>(texels) >> 8;
    t.data_format = static_cast<u64>(AmdGpu::DataFormat::Format8_8_8_8);
    t.num_format = static_cast<u64>(AmdGpu::NumberFormat::Unorm);
    t.width = 1;
    t.height = 1;
    t.pitch = 1;
    t.dst_sel_x = 4;
    t.dst_sel_y = 5;
    t.dst_sel_z = 6;
    t.dst_sel_w = 7;
    t.tiling_index = static_cast<u64>(AmdGpu::TileMode::DisplayLinearAligned);
    t.type = static_cast<u64>(AmdGpu::ImageType::Color2D);
    AmdGpu::Sampler s{};
    s.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
    s.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
    s.clamp_z.Assign(AmdGpu::ClampMode::ClampLastTexel);
    s.max_lod.Assign(0xFFF);

    static Scene scene;
    // uv (0,0) at the target's top left: clip y = +1 is window row 0 with this viewport.
    scene.vertices = {{{-1, -1, 0, 1}, {0, 1, 0, 1}},
                      {{1, -1, 0, 1}, {1, 1, 0, 1}},
                      {{1, 1, 0, 1}, {1, 0, 0, 1}},
                      {{-1, 1, 0, 1}, {0, 0, 0, 1}}};
    BaseState(lp, target);
    SetShaders(lp, scene, TexturePixelShader(), 12);
    std::memcpy(&lp.regs.ps_program.user_data[0], &t, sizeof(t));
    std::memcpy(&lp.regs.ps_program.user_data[8], &s, sizeof(s));
    lp.regs.primitive_type = AmdGpu::PrimitiveType::QuadList;
    lp.regs.num_indices = 4;
    rast.Draw(false);
    const auto pixels = ReadBack(rast, target);
    Check("draw: texture sampled from guest memory", CountWrong(pixels, [](u32 x, u32 y) {
              const bool right = x >= W / 2;
              const bool bottom = y >= H / 2;
              if (!bottom) {
                  return right ? std::array{0, 255, 0, 255} : std::array{255, 0, 0, 255};
              }
              return right ? std::array{255, 255, 255, 255} : std::array{0, 0, 255, 255};
          }) == 0);
}

// A compute shader writing an image (image_store to a T#), submitted as PM4 through the command
// processor: the image stays on the GPU, and the presenter's readback of that memory sees what
// the shader wrote. Workgroup y writes row y.
void TestComputeImageWrite(AmdGpu::Liverpool& lp, Vulkan::Rasterizer& rast) {
    u8* target = NewTarget();
    const u32* code = Assemble(
        {
            0x7E040D00, // v_cvt_f32_u32 v2, v0            ; x
            0x100404FF, 0x3C800000, // v_mul_f32 v2, 1/64, v2
            0x7E060C08, // v_cvt_f32_u32 v3, s8            ; y (the workgroup id)
            0x100606FF, 0x3C800000, // v_mul_f32 v3, 1/64, v3
            0x7E0802F0, // v_mov_b32 v4, 0.5
            0x7E0A02F2, // v_mov_b32 v5, 1.0
            0x7E020208, // v_mov_b32 v1, s8
            0xF0200F00, 0x00000200, // image_store v[2:5], v[0:1], s[0:7] dmask:0xf
            SEndpgm,
        },
        0x7e57000000000013ULL);
    AmdGpu::Image t{};
    t.base_address = reinterpret_cast<u64>(target) >> 8;
    t.data_format = static_cast<u64>(AmdGpu::DataFormat::Format8_8_8_8);
    t.num_format = static_cast<u64>(AmdGpu::NumberFormat::Unorm);
    t.width = W - 1;
    t.height = H - 1;
    t.pitch = W - 1;
    t.dst_sel_x = 4;
    t.dst_sel_y = 5;
    t.dst_sel_z = 6;
    t.dst_sel_w = 7;
    t.tiling_index = static_cast<u64>(AmdGpu::TileMode::DisplayLinearAligned);
    t.type = static_cast<u64>(AmdGpu::ImageType::Color2D);
    AmdGpu::ComputeProgram cs{};
    cs.address = reinterpret_cast<u64>(code) >> 8;
    cs.settings.num_vgprs = 1;
    cs.settings.num_user_regs = 8;
    cs.settings.tgid_enable = 1;
    cs.num_thread_x.full = W;
    cs.num_thread_y.full = 1;
    cs.num_thread_z.full = 1;
    std::memcpy(&cs.user_data[0], &t, sizeof(t));
    // As a game's command buffer has it: the COMPUTE_* registers, then DISPATCH_DIRECT, through
    // the command processor.
    using AmdGpu::PM4ItOpcode;
    using AmdGpu::PM4Type3Header;
    constexpr u32 words = sizeof(cs) / sizeof(u32);
    static std::vector<u32> dcb;
    dcb.clear();
    dcb.push_back(PM4Type3Header{PM4ItOpcode::SetShReg, words}.raw);
    dcb.push_back(0x200); // COMPUTE_DISPATCH_INITIATOR, the start of ComputeProgram
    const auto* regs = reinterpret_cast<const u32*>(&cs);
    dcb.insert(dcb.end(), regs, regs + words);
    dcb.push_back(PM4Type3Header{PM4ItOpcode::DispatchDirect, 3}.raw);
    dcb.insert(dcb.end(), {H, 1, 1, 1}); // groups, initiator: compute shader enable
    lp.SubmitGfx(dcb, {});
    lp.SubmitDone();
    lp.WaitGpuIdle();
    // Read back on the command processor's thread, after the submit - where the presenter's
    // flips run too (the renderer's batch belongs to that thread).
    std::vector<u8> pixels;
    lp.SendCommand<true>([&] { pixels = ReadBack(rast, target); });
    Check("dispatch: image written by a compute shader, read back from the GPU",
          CountWrong(pixels, [](u32 x, u32 y) {
              return std::array{int(std::lround(x / 64.0 * 255)), int(std::lround(y / 64.0 * 255)),
                                128, 255};
          }) == 0);
}

} // namespace

void RunDrawTests() {
    AmdGpu::Liverpool lp;
    Vulkan::Rasterizer rast{&lp};
    TestFlatTriangle(lp, rast);
    TestQuadGradient(lp, rast);
    TestRectOverFastClear(lp, rast);
    TestBlend(lp, rast);
    TestTexture(lp, rast);
    TestComputeImageWrite(lp, rast);
    const auto& stats = rast.Stats();
    Check("draw: every draw ran on WebGPU", stats.draws_run == stats.draws && stats.draws == 6);
}
