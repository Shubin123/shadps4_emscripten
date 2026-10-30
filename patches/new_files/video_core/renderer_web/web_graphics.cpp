// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/number_utils.h"
#include "core/wasm_high_memory.h"
#include "shader_recompiler/backend/wgsl/emit_wgsl.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_web/web_graphics.h"

namespace Web {

namespace {

namespace WGSL = Shader::Backend::WGSL;
using AmdGpu::PrimitiveType;
using BlendFactor = AmdGpu::BlendControl::BlendFactor;
using BlendFunc = AmdGpu::BlendControl::BlendFunc;

// Logs why a draw was dropped, once per reason.
bool Drop(const std::string& reason) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::scoped_lock lk{mutex};
    if (seen.insert(reason).second) {
        LOG_WARNING(Render, "WebGPU: draw dropped: {}", reason);
    }
    return false;
}

template <typename Program>
void BuildCommon(Shader::RuntimeInfo& info, const Program& program) {
    info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = program.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    info.props.num_allocated_vgprs = program.NumVgprs();
    info.props.num_user_data = program.settings.num_user_regs;
    info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
    info.props.dx10_clamp = program.settings.dx10_clamp;
}

// As vk_pipeline_cache.cpp's MapOutputs.
u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    using Shader::Output;
    u32 num_outputs = 0;
    if (ctl.vs_out_misc_enable) {
        auto& misc = outputs[num_outputs++];
        misc[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc[1] = ctl.use_vtx_edge_flag
                      ? Output::EdgeFlag
                      : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc[2] = ctl.use_vtx_kill_flag
                      ? Output::KillFlag
                      : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }
    for (u32 set = 0; set < 2; set++) {
        if (set == 0 ? !ctl.vs_out_ccdist0_enable : !ctl.vs_out_ccdist1_enable) {
            continue;
        }
        auto& dist = outputs[num_outputs++];
        for (u32 i = 0; i < 4; i++) {
            const u32 n = set * 4 + i;
            dist[i] = ctl.IsClipDistEnabled(n)
                          ? Output(u32(Output::ClipDist0) + n)
                          : (ctl.IsCullDistEnabled(n) ? Output(u32(Output::CullDist0) + n)
                                                      : Output::None);
        }
    }
    return num_outputs;
}

const char* Factor(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero:
        return "zero";
    case BlendFactor::One:
        return "one";
    case BlendFactor::SrcColor:
        return "src";
    case BlendFactor::OneMinusSrcColor:
        return "one-minus-src";
    case BlendFactor::SrcAlpha:
        return "src-alpha";
    case BlendFactor::OneMinusSrcAlpha:
        return "one-minus-src-alpha";
    case BlendFactor::DstAlpha:
        return "dst-alpha";
    case BlendFactor::OneMinusDstAlpha:
        return "one-minus-dst-alpha";
    case BlendFactor::DstColor:
        return "dst";
    case BlendFactor::OneMinusDstColor:
        return "one-minus-dst";
    case BlendFactor::SrcAlphaSaturate:
        return "src-alpha-saturated";
    case BlendFactor::ConstantColor:
    case BlendFactor::ConstantAlpha: // WebGPU has one blend constant for both
        return "constant";
    case BlendFactor::OneMinusConstantColor:
    case BlendFactor::OneMinusConstantAlpha:
        return "one-minus-constant";
    // Dual-source factors: the second output isn't produced, so they fall back to the first.
    case BlendFactor::Src1Color:
        return "src";
    case BlendFactor::InvSrc1Color:
        return "one-minus-src";
    case BlendFactor::Src1Alpha:
        return "src-alpha";
    case BlendFactor::InvSrc1Alpha:
        return "one-minus-src-alpha";
    }
    return "one";
}

Gpu::Json BlendComponent(BlendFunc func, BlendFactor src, BlendFactor dst) {
    const char* op = "add";
    switch (func) {
    case BlendFunc::Add:
        break;
    case BlendFunc::Subtract:
        op = "subtract";
        break;
    case BlendFunc::ReverseSubtract:
        op = "reverse-subtract";
        break;
    case BlendFunc::Min:
    case BlendFunc::Max:
        // GCN ignores the factors for min/max; WebGPU requires them to be one.
        return {{"operation", func == BlendFunc::Min ? "min" : "max"},
                {"srcFactor", "one"},
                {"dstFactor", "one"}};
    }
    return {{"operation", op}, {"srcFactor", Factor(src)}, {"dstFactor", Factor(dst)}};
}

const char* Compare(AmdGpu::CompareFunc func) {
    static constexpr const char* names[] = {"never",   "less",      "equal",         "less-equal",
                                            "greater", "not-equal", "greater-equal", "always"};
    return names[u32(func) & 7];
}

const char* StencilOperation(AmdGpu::StencilFunc func) {
    using F = AmdGpu::StencilFunc;
    switch (func) {
    case F::Keep:
        return "keep";
    case F::Zero:
        return "zero";
    case F::Ones:
        return "invert"; // closest: no "set all bits" op
    case F::ReplaceTest:
    case F::ReplaceOp:
        return "replace";
    case F::AddClamp:
        return "increment-clamp";
    case F::SubClamp:
        return "decrement-clamp";
    case F::Invert:
        return "invert";
    case F::AddWrap:
        return "increment-wrap";
    case F::SubWrap:
        return "decrement-wrap";
    default:
        return "keep"; // the logic ops have no WebGPU equivalent
    }
}

// The color a fast-cleared target starts with, as upstream's ColorBufferClearValue decodes it.
std::array<double, 4> ClearColor(const AmdGpu::ColorBuffer& cb) {
    using AmdGpu::DataFormat;
    using AmdGpu::NumberFormat;
    using namespace NumberUtils;
    const auto format = DataFormat(cb.info.format);
    const auto nfmt = cb.GetFixedNumberFormat();
    const bool integer = nfmt == NumberFormat::Uint || nfmt == NumberFormat::Sint;
    const bool snorm = nfmt == NumberFormat::Snorm || nfmt == NumberFormat::SnormNz;
    const u32 c0 = cb.clear_word0;
    const u32 c1 = cb.clear_word1;
    std::array<double, 4> v{0.0, 0.0, 0.0, 1.0};
    const auto bits = [](u32 word, u32 shift, u32 n) { return (word >> shift) & ((1u << n) - 1); };
    const auto unorm8 = [&](u32 word, u32 shift) -> double {
        const u32 b = bits(word, shift, 8);
        return integer ? double(b) : snorm ? S8ToSnorm(s8(b)) : U8ToUnorm(u8(b));
    };
    const auto norm16 = [&](u32 word, u32 shift) -> double {
        const u32 h = bits(word, shift, 16);
        if (integer) {
            return double(h);
        }
        if (nfmt == NumberFormat::Float) {
            return Uf16ToF32(u16(h));
        }
        return snorm ? S16ToSnorm(s16(h)) : U16ToUnorm(u16(h));
    };
    switch (format) {
    case DataFormat::Format8:
        v[0] = unorm8(c0, 0);
        break;
    case DataFormat::Format8_8:
        v[0] = unorm8(c0, 0), v[1] = unorm8(c0, 8);
        break;
    case DataFormat::Format8_8_8_8:
        v[0] = unorm8(c0, 0), v[1] = unorm8(c0, 8), v[2] = unorm8(c0, 16), v[3] = unorm8(c0, 24);
        break;
    case DataFormat::Format16:
        v[0] = norm16(c0, 0);
        break;
    case DataFormat::Format16_16:
        v[0] = norm16(c0, 0), v[1] = norm16(c0, 16);
        break;
    case DataFormat::Format16_16_16_16:
        v[0] = norm16(c0, 0), v[1] = norm16(c0, 16), v[2] = norm16(c1, 0), v[3] = norm16(c1, 16);
        break;
    case DataFormat::Format32:
        v[0] = integer ? double(c0) : double(std::bit_cast<float>(c0));
        break;
    case DataFormat::Format32_32:
        v[0] = integer ? double(c0) : double(std::bit_cast<float>(c0));
        v[1] = integer ? double(c1) : double(std::bit_cast<float>(c1));
        break;
    case DataFormat::Format2_10_10_10:
    case DataFormat::Format10_10_10_2:
        v[0] = U10ToUnorm(u16(bits(c0, 0, 10))), v[1] = U10ToUnorm(u16(bits(c0, 10, 10)));
        v[2] = U10ToUnorm(u16(bits(c0, 20, 10))), v[3] = U2ToUnorm(u8(bits(c0, 30, 2)));
        break;
    case DataFormat::Format10_11_11:
    case DataFormat::Format11_11_10:
        v[0] = Uf11ToF32(u16(bits(c0, 0, 11))), v[1] = Uf11ToF32(u16(bits(c0, 11, 11)));
        v[2] = Uf10ToF32(u16(bits(c0, 22, 10)));
        break;
    default:
        break;
    }
    return cb.Swizzle().Apply(v);
}

// Topologies WebGPU draws natively; the others are rewritten as lists.
const char* NativeTopology(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::PointList:
        return "point-list";
    case PrimitiveType::LineList:
        return "line-list";
    case PrimitiveType::LineStrip:
        return "line-strip";
    case PrimitiveType::TriangleList:
        return "triangle-list";
    case PrimitiveType::TriangleStrip:
        return "triangle-strip";
    default:
        return nullptr;
    }
}

// The index list `type` is drawn as, for `count` source vertices with indices `src(i)`.
template <typename Src>
std::vector<u32> ExpandIndices(PrimitiveType type, u32 count, Src&& src, const char** topology) {
    std::vector<u32> out;
    switch (type) {
    case PrimitiveType::QuadList:
        *topology = "triangle-list";
        for (u32 q = 0; q + 3 < count; q += 4) {
            for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) {
                out.push_back(src(q + i));
            }
        }
        break;
    case PrimitiveType::QuadStrip:
        *topology = "triangle-list";
        for (u32 k = 0; k + 3 < count; k += 2) {
            for (u32 i : {0u, 1u, 2u, 1u, 3u, 2u}) {
                out.push_back(src(k + i));
            }
        }
        break;
    case PrimitiveType::TriangleFan:
    case PrimitiveType::Polygon:
        *topology = "triangle-list";
        for (u32 i = 1; i + 1 < count; i++) {
            out.push_back(src(0));
            out.push_back(src(i));
            out.push_back(src(i + 1));
        }
        break;
    case PrimitiveType::LineLoop:
        *topology = "line-list";
        for (u32 i = 0; i + 1 < count; i++) {
            out.push_back(src(i));
            out.push_back(src(i + 1));
        }
        if (count > 2) {
            out.push_back(src(count - 1));
            out.push_back(src(0));
        }
        break;
    default:
        *topology = nullptr;
        break;
    }
    return out;
}

const u8* GuestPointer(VAddr address, u64 size) {
    if (Core::HighMemory::InRange(address)) {
        // Index data in the translated high region would need gathering; not seen so far.
        return nullptr;
    }
    return address + size <= 16_GB ? reinterpret_cast<const u8*>(address) : nullptr;
}

} // namespace

GraphicsPipelines::GraphicsPipelines(Context& context) : ctx{context} {}
GraphicsPipelines::~GraphicsPipelines() = default;

void GraphicsPipelines::EliminateFastClear(AmdGpu::Liverpool& liverpool) {
    const auto& cb = liverpool.regs.color_buffers[0];
    if (!cb || !cb.info.fast_clear || !ctx.meta.Consume(cb.CmaskAddress())) {
        return;
    }
    Surface* rt = ctx.surfaces.ColorTarget(ctx.batch, cb, liverpool.last_cb_extent[0]);
    if (rt == nullptr) {
        return;
    }
    const auto color = ClearColor(cb);
    ctx.batch.Add({{"op", "clearTexture"},
                   {"texture", rt->id},
                   {"color", {color[0], color[1], color[2], color[3]}}});
}

void GraphicsPipelines::Resolve(AmdGpu::Liverpool& liverpool) {
    const auto& regs = liverpool.regs;
    Surface* src = ctx.surfaces.ColorTarget(ctx.batch, regs.color_buffers[0],
                                            liverpool.last_cb_extent[0]);
    Surface* dst = ctx.surfaces.ColorTarget(ctx.batch, regs.color_buffers[1],
                                            liverpool.last_cb_extent[1]);
    if (src == nullptr || dst == nullptr || src == dst) {
        return;
    }
    // Targets are rendered single-sampled here, so resolving is a copy.
    ctx.batch.Add({{"op", "copyTexture"},
                   {"src", src->id},
                   {"dst", dst->id},
                   {"size", {std::min(src->width, dst->width), std::min(src->height, dst->height)}}});
}

bool GraphicsPipelines::Draw(AmdGpu::Liverpool& liverpool, const DrawParams& params) {
    if (!Gpu::Available()) {
        return false;
    }
    auto& regs = liverpool.regs;
    if (regs.primitive_type == PrimitiveType::None) {
        return false;
    }
    if (regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        return Drop("geometry or tessellation stages (WebGPU has neither)");
    }
    const PrimitiveType prim = regs.primitive_type;
    const bool rect_list = prim == PrimitiveType::RectList;
    const bool triangles = rect_list || prim == PrimitiveType::TriangleList ||
                           prim == PrimitiveType::TriangleStrip ||
                           prim == PrimitiveType::TriangleFan || prim == PrimitiveType::QuadList ||
                           prim == PrimitiveType::QuadStrip || prim == PrimitiveType::Polygon;
    if (prim == PrimitiveType::PatchPrimitive || prim == PrimitiveType::AdjLineList ||
        prim == PrimitiveType::AdjLineStrip || prim == PrimitiveType::AdjTriangleList ||
        prim == PrimitiveType::AdjTriangleStrip) {
        return Drop("adjacency or patch primitives");
    }
    if (regs.vs_program.Address<u32*>() == nullptr) {
        return Drop("no vertex shader");
    }

    // ---- shaders ----
    const bool skip_cb = regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    static RuntimeInfoStorage ps_storage;
    static RuntimeInfoStorage vs_storage;
    auto& ps_rt = ps_storage.Get();
    auto& vs_rt = vs_storage.Get();

    ShaderCache::Result ps{};
    const bool has_ps = regs.ps_program.Address<u32*>() != nullptr;
    if (has_ps) {
        ps_rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        BuildCommon(ps_rt, regs.ps_program);
        auto& fs = ps_rt.hw.fs;
        fs.en_flags = regs.ps_input_ena;
        fs.addr_flags = regs.ps_input_addr;
        fs.num_inputs = regs.num_interp;
        fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        fs.depth_before_shader = regs.depth_shader_control.depth_before_shader;
        fs.num_samples = 1;
        fs.z_export_format = regs.z_export_format;
        const u8 stencil_export = regs.depth_shader_control.stencil_op_val_export_enable |
                                  regs.depth_shader_control.stencil_test_val_export_enable;
        fs.mrtz_mask = regs.depth_shader_control.z_export_enable | (stencil_export << 1) |
                       (regs.depth_shader_control.mask_export_enable << 2) |
                       (regs.depth_shader_control.coverage_to_mask_enable << 3);
        fs.dual_source_blending = false;
        for (u32 i = 0; i < regs.num_interp; i++) {
            const auto& in = regs.ps_inputs[i];
            fs.inputs[i] = {.param_index = u16(in.input_offset),
                            .is_default = u16(in.use_default),
                            .is_flat = u16(in.flat_shade),
                            .default_value = u16(in.default_value)};
        }
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb; cb++) {
            const auto& col_buf = regs.color_buffers[cb];
            if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
                continue;
            }
            auto& out = fs.color_buffers[cb];
            out.data_format = col_buf.GetDataFmt();
            out.num_format = col_buf.GetNumberFmt();
            out.num_conversion = col_buf.GetNumberConversion();
            out.export_format = regs.color_export_format.GetFormat(cb);
            out.swizzle = col_buf.Swizzle();
            const auto& bc = regs.blend_control[cb];
            out.blend_self_scale = bc.enable && !col_buf.info.blend_bypass &&
                                   (bc.color_func == BlendFunc::Min ||
                                    bc.color_func == BlendFunc::Max) &&
                                   bc.color_src_factor == BlendFactor::SrcColor &&
                                   bc.color_dst_factor == BlendFactor::DstColor;
        }
        ps = ctx.shaders.Get(Shader::HwStage::Fragment, Shader::SwStage::Fragment,
                             AmdGpu::GetParams(regs.ps_program), ps_rt);
        if (ps.perm == nullptr) {
            return Drop("a pixel shader the WGSL backend can't translate (see above)");
        }
    }

    vs_rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    BuildCommon(vs_rt, regs.vs_program);
    vs_rt.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
    vs_rt.hw.vs.num_outputs = MapOutputs(vs_rt.hw.vs.outputs, regs.vs_output_control);
    vs_rt.hw.vs.clip_disable = regs.IsClipDisabled();
    vs_rt.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
    vs_rt.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
    const auto vs = ctx.shaders.Get(Shader::HwStage::Vertex, Shader::SwStage::Vertex,
                                    AmdGpu::GetParams(regs.vs_program), vs_rt);
    if (vs.perm == nullptr) {
        return Drop("a vertex shader the WGSL backend can't translate (see above)");
    }

    // ---- render targets ----
    const u32 mrt_mask = has_ps ? ps.perm->out.color_output_mask : 0;
    Gpu::Json targets = Gpu::Json::array();
    Gpu::Json color_attachments = Gpu::Json::array();
    u32 width = ~0u;
    u32 height = ~0u;
    for (u32 cb = 0; cb < static_cast<u32>(std::bit_width(mrt_mask)); cb++) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb || !(mrt_mask & (1u << cb)) || !col_buf || !target_mask) {
            targets.push_back(nullptr);
            color_attachments.push_back(nullptr);
            continue;
        }
        Surface* rt = ctx.surfaces.ColorTarget(ctx.batch, col_buf, liverpool.last_cb_extent[cb]);
        if (rt == nullptr) {
            return Drop(fmt::format("render target format {} {}",
                                    AmdGpu::NameOf(col_buf.GetDataFmt()),
                                    AmdGpu::NameOf(col_buf.GetNumberFmt())));
        }
        width = std::min(width, rt->width);
        height = std::min(height, rt->height);
        Gpu::Json target{{"format", rt->host.format},
                         {"writeMask", col_buf.Swizzle().ApplyMask(target_mask)}};
        const auto& bc = regs.blend_control[cb];
        if (bc.enable && !col_buf.info.blend_bypass && rt->host.sample_type == 'f') {
            const auto color = BlendComponent(bc.color_func, bc.color_src_factor,
                                              bc.color_dst_factor);
            target["blend"] = {{"color", color},
                               {"alpha", bc.separate_alpha_blend
                                             ? BlendComponent(bc.alpha_func, bc.alpha_src_factor,
                                                              bc.alpha_dst_factor)
                                             : color}};
        }
        targets.push_back(std::move(target));

        Gpu::Json attachment{{"tex", rt->id}, {"load", "load"}};
        if (col_buf.info.fast_clear) {
            ctx.meta.Register(col_buf.CmaskAddress());
            if (ctx.meta.Consume(col_buf.CmaskAddress())) {
                const auto clear = ClearColor(col_buf);
                attachment["load"] = "clear";
                attachment["clear"] = {clear[0], clear[1], clear[2], clear[3]};
            }
        }
        color_attachments.push_back(std::move(attachment));
    }

    Gpu::Json depth_state = nullptr;
    Gpu::Json depth_attachment = nullptr;
    const auto& db = regs.depth_buffer;
    const auto& dc = regs.depth_control;
    if ((dc.depth_enable && db.DepthValid()) || (dc.stencil_enable && db.StencilValid())) {
        Surface* ds = ctx.surfaces.DepthTarget(ctx.batch, db, liverpool.last_db_extent);
        width = std::min(width, ds->width);
        height = std::min(height, ds->height);
        const bool depth = dc.depth_enable && db.DepthValid();
        const bool stencil = dc.stencil_enable && db.StencilValid();
        const auto stencil_face = [&](bool back) -> Gpu::Json {
            const auto& sc = regs.stencil_control;
            if (!stencil) {
                return {{"compare", "always"}};
            }
            const bool b = back && dc.backface_enable;
            return {{"compare", Compare(b ? dc.stencil_bf_func : dc.stencil_ref_func)},
                    {"failOp", StencilOperation(b ? sc.stencil_fail_back : sc.stencil_fail_front)},
                    {"depthFailOp",
                     StencilOperation(b ? sc.stencil_zfail_back : sc.stencil_zfail_front)},
                    {"passOp",
                     StencilOperation(b ? sc.stencil_zpass_back : sc.stencil_zpass_front)}};
        };
        const auto& front = regs.stencil_ref_front;
        depth_state = {
            {"format", ds->host.format},
            {"depthWriteEnabled",
             depth && dc.depth_write_enable && !regs.depth_render_control.depth_clear_enable},
            {"depthCompare", depth ? Compare(dc.depth_func) : "always"},
            {"stencilFront", stencil_face(false)},
            {"stencilBack", stencil_face(true)},
            {"stencilReadMask", stencil ? front.stencil_mask : 0xFF},
            {"stencilWriteMask",
             stencil && !regs.depth_render_control.stencil_clear_enable ? front.stencil_write_mask
                                                                        : 0}};
        if (regs.polygon_control.NeedsBias() && triangles) {
            const bool f = regs.polygon_control.enable_polygon_offset_front;
            const auto& po = regs.poly_offset;
            depth_state["depthBias"] =
                static_cast<s32>(std::lround(f ? po.front_offset : po.back_offset));
            depth_state["depthBiasSlopeScale"] = (f ? po.front_scale : po.back_scale) / 16.0f;
            depth_state["depthBiasClamp"] = po.depth_bias;
        }
        const VAddr htile = db.z_info.tile_surface_enable ? regs.depth_htile_data_base.GetAddress()
                                                          : 0;
        ctx.meta.Register(htile);
        const bool depth_clear = (regs.depth_render_control.depth_clear_enable &&
                                  dc.depth_enable && dc.depth_write_enable) ||
                                 (htile != 0 && ctx.meta.Consume(htile));
        depth_attachment = {{"tex", ds->id},
                            {"depthLoad", depth_clear ? "clear" : "load"},
                            {"depthClear", std::clamp(regs.depth_clear, 0.0f, 1.0f)},
                            {"stencilLoad",
                             regs.depth_render_control.stencil_clear_enable ? "clear" : "load"},
                            {"stencilClear", regs.stencil_clear & 0xFF}};
    }
    if (width == ~0u) {
        // Neither color nor depth is written: nothing the draw does can be seen.
        return true;
    }

    // ---- viewport and scissor ----
    const auto& vp = regs.viewports[0];
    const auto& vte = regs.viewport_control;
    const float xscale = vte.xscale_enable ? vp.xscale : 1.0f;
    const float xoffset = vte.xoffset_enable ? vp.xoffset : 0.0f;
    const float yscale = vte.yscale_enable ? vp.yscale : 1.0f;
    const float yoffset = vte.yoffset_enable ? vp.yoffset : 0.0f;
    const float zscale = vte.zscale_enable ? vp.zscale : 1.0f;
    const float zoffset = vte.zoffset_enable ? vp.zoffset : 0.0f;
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    WGSL::DrawInfo draw_info{};
    // Window x = x * xscale + xoffset (divided by w first unless clipping is off, where the
    // position is already in window space); WebGPU NDC for a full-target viewport is
    // x_ndc = 2 * x_win / w - 1 and y_ndc = 1 - 2 * y_win / h.
    const bool clip_disabled = regs.IsClipDisabled();
    draw_info.vx[0] = 2.0f * xscale / w;
    draw_info.vy[0] = -2.0f * yscale / h;
    if (clip_disabled) {
        draw_info.vx[2] = 2.0f * xoffset / w - 1.0f;
        draw_info.vy[2] = 1.0f - 2.0f * yoffset / h;
    } else {
        draw_info.vx[1] = 2.0f * xoffset / w - 1.0f;
        draw_info.vy[1] = 1.0f - 2.0f * yoffset / h;
    }
    draw_info.vz[0] = zscale;
    draw_info.vz[1] = zoffset;

    AmdGpu::Scissor scissor = regs.GetCombinedScissor();
    if (regs.mode_control.vport_scissor_enable) {
        const auto& vs_scissor = regs.viewport_scissors[0];
        scissor.top_left_x = std::max<s16>(scissor.top_left_x, s16(vs_scissor.top_left_x));
        scissor.top_left_y = std::max<s16>(scissor.top_left_y, s16(vs_scissor.top_left_y));
        scissor.bottom_right_x =
            std::min<s16>(scissor.bottom_right_x, s16(vs_scissor.bottom_right_x));
        scissor.bottom_right_y =
            std::min<s16>(scissor.bottom_right_y, s16(vs_scissor.bottom_right_y));
    }
    const u32 sx = std::min<u32>(AmdGpu::Scissor::Clamp(scissor.top_left_x), width);
    const u32 sy = std::min<u32>(AmdGpu::Scissor::Clamp(scissor.top_left_y), height);
    const u32 ex = std::min<u32>(AmdGpu::Scissor::Clamp(scissor.bottom_right_x), width);
    const u32 ey = std::min<u32>(AmdGpu::Scissor::Clamp(scissor.bottom_right_y), height);
    if (ex <= sx || ey <= sy) {
        return true; // nothing to draw
    }

    // ---- vertices and indices ----
    const u32 count = params.count.value_or(regs.num_indices);
    const u32 instances = params.instances.value_or(regs.num_instances.NumInstances());
    if (count == 0 || instances == 0) {
        return true;
    }
    const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const u32 index_size = index16 ? 2 : 4;
    const u32* indices32 = nullptr;
    const u16* indices16 = nullptr;
    u32 num_indices = count;
    if (params.indexed) {
        num_indices = std::min(count, regs.max_index_size);
        const VAddr address = regs.index_base_address.Address<VAddr>() +
                              u64(params.index_offset) * index_size;
        const u8* data = GuestPointer(address, u64(num_indices) * index_size);
        if (data == nullptr) {
            return Drop("an index buffer outside linear guest memory");
        }
        if (index16) {
            indices16 = reinterpret_cast<const u16*>(data);
        } else {
            indices32 = reinterpret_cast<const u32*>(data);
        }
    }
    const auto source_index = [&](u32 i) -> u32 {
        return indices16 ? indices16[i] : indices32 ? indices32[i] : i;
    };
    const u32 base_vertex = regs.index_offset;

    Gpu::Json draw{{"op", "draw"}, {"instances", instances}};
    const char* topology = NativeTopology(prim);
    Gpu::Json group2_layout = Gpu::Json::array(
        {{{"b", 0}, {"kind", "uniform"}, {"type", "DrawInfo"}}});
    Gpu::Json group2 = Gpu::Json::array();
    if (rect_list) {
        // The vertex shader reads each rectangle's three vertex indices itself.
        const u32 rects = num_indices / 3;
        std::vector<u32> list(rects * 3);
        for (u32 i = 0; i < rects * 3; i++) {
            list[i] = source_index(i) + base_vertex;
        }
        if (rects == 0) {
            return true;
        }
        const u64 at = ctx.uploads.Host(ctx.batch, list.data(), list.size() * 4);
        group2_layout.push_back({{"b", 1}, {"kind", "buffer"}, {"writable", false},
                                 {"type", "array<u32>"}});
        group2.push_back({{"b", 1}, {"buf", {at, list.size() * 4}}});
        topology = "triangle-list";
        draw["vertexCount"] = rects * 6;
        draw["firstVertex"] = 0;
    } else if (topology == nullptr) {
        auto list = ExpandIndices(prim, num_indices, source_index, &topology);
        if (topology == nullptr) {
            return Drop(fmt::format("primitive type {}", u32(prim)));
        }
        if (list.empty()) {
            return true;
        }
        const u64 at = ctx.uploads.Host(ctx.batch, list.data(), list.size() * 4);
        draw["index"] = {{"offset", at}, {"size", list.size() * 4}, {"format", "uint32"}};
        draw["indexCount"] = list.size();
        draw["baseVertex"] = base_vertex;
    } else if (params.indexed) {
        const u64 at = ctx.uploads.Host(ctx.batch, indices16 ? static_cast<const void*>(indices16)
                                                             : indices32,
                                        u64(num_indices) * index_size);
        draw["index"] = {{"offset", at},
                         {"size", Common::AlignUp(u64(num_indices) * index_size, 4)},
                         {"format", index16 ? "uint16" : "uint32"}};
        draw["indexCount"] = num_indices;
        draw["baseVertex"] = base_vertex;
    } else {
        draw["vertexCount"] = count;
        draw["firstVertex"] = base_vertex;
    }
    group2.insert(group2.begin(),
                  Gpu::Json{{"b", 0},
                            {"buf", {ctx.uploads.Host(ctx.batch, &draw_info, sizeof(draw_info)),
                                     sizeof(draw_info)}}});

    // ---- resources ----
    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> clip_planes{};
    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; i++) {
        const auto& plane = regs.clip_user_data[i];
        clip_planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
        clip_planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
        clip_planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
        clip_planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
    }
    StageBindings vs_bindings;
    if (!BindStage(ctx.batch, ctx.uploads, ctx.surfaces, *vs.info, vs.perm->out, {},
                   vs_bindings, clip_planes)) {
        return Drop("a vertex shader resource that can't be bound (see above)");
    }
    StageBindings ps_bindings;
    if (has_ps && !BindStage(ctx.batch, ctx.uploads, ctx.surfaces, *ps.info, ps.perm->out, {},
                             ps_bindings)) {
        return Drop("a pixel shader resource that can't be bound (see above)");
    }

    // ---- pipeline ----
    auto& objects = GetPageObjects();
    WGSL::VertexLink link;
    if (has_ps) {
        link.inputs = ps.perm->out.inputs;
    }
    link.rect_list = rect_list;
    const u32 vs_module = objects.Module(ctx.batch, WGSL::LinkVertexShader(vs.perm->out, link));
    const u32 ps_module = has_ps ? objects.Module(ctx.batch, ps.perm->out.code) : 0;
    const auto cull = triangles && !rect_list ? regs.polygon_control.CullingMode()
                                              : AmdGpu::CullMode::None;
    if (cull == AmdGpu::CullMode::FrontAndBack) {
        return true; // every triangle is culled
    }
    Gpu::Json primitive{
        {"topology", topology},
        {"frontFace",
         regs.polygon_control.front_face == AmdGpu::FrontFace::Clockwise ? "cw" : "ccw"},
        {"cullMode", cull == AmdGpu::CullMode::Front  ? "front"
                     : cull == AmdGpu::CullMode::Back ? "back"
                                                      : "none"}};
    if (std::string_view(topology).ends_with("strip") && draw.contains("index")) {
        primitive["stripIndexFormat"] = draw["index"]["format"];
    }
    if (!regs.clipper_control.ZclipEnable() && Gpu::GetFeatures().depth_clip_control) {
        primitive["unclippedDepth"] = true;
    }
    Gpu::Json desc{{"kind", "render"},
                   {"vertex", vs_module},
                   {"fragment", has_ps ? Gpu::Json(ps_module) : Gpu::Json(nullptr)},
                   {"layouts", {vs_bindings.layout, ps_bindings.layout, group2_layout}},
                   {"targets", targets},
                   {"depthStencil", depth_state},
                   {"primitive", primitive}};
    draw["pipeline"] = objects.Pipeline(ctx.batch, std::move(desc));
    draw["pass"] = {{"color", color_attachments}, {"depth", depth_attachment}};
    draw["groups"] = {vs_bindings.entries, ps_bindings.entries, group2};
    draw["scissor"] = {sx, sy, ex - sx, ey - sy};
    const auto& bc = regs.blend_constants;
    draw["blendConstant"] = {bc[0], bc[1], bc[2], bc[3]};
    // REPLACE writes STENCILOPVAL; WebGPU has one reference for testing and replacing.
    const auto& sc = regs.stencil_control;
    const bool replace_op = sc.stencil_fail_front == AmdGpu::StencilFunc::ReplaceOp ||
                            sc.stencil_zpass_front == AmdGpu::StencilFunc::ReplaceOp ||
                            sc.stencil_zfail_front == AmdGpu::StencilFunc::ReplaceOp;
    draw["stencilReference"] = replace_op ? regs.stencil_ref_front.stencil_op_val
                                          : regs.stencil_ref_front.stencil_test_val;
    if (static const bool trace = std::getenv("SHADPS4_WEBGPU_TRACE") != nullptr; trace) {
        LOG_INFO(Render, "WebGPU draw: vs {:#x} ps {:#x} prim {} count {} x{} pass {} groups {}",
                 vs.info->pgm_hash, has_ps ? ps.info->pgm_hash : 0, u32(prim), count, instances,
                 draw["pass"].dump(), draw["groups"].dump());
    }
    ctx.batch.Add(std::move(draw));

    if (!ps_bindings.writebacks.empty()) {
        // The pixel shader wrote guest buffers: the CPU may look at them as soon as the draw is
        // done, so this one waits for its results.
        for (const auto& wb : ps_bindings.writebacks) {
            if (Core::HighMemory::InRange(wb.address)) {
                continue; // not written back (no draw has needed it so far)
            }
            ctx.batch.Add({{"op", "readBuffer"},
                           {"offset", wb.offset},
                           {"size", wb.size},
                           {"ptr", wb.address}});
        }
        ctx.Submit(true);
    }
    return true;
}

} // namespace Web
