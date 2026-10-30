// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <xxhash.h>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/wasm_high_memory.h"
#include "video_core/renderer_web/web_surfaces.h"

namespace Web {

namespace {

using AmdGpu::DataFormat;
using AmdGpu::NumberFormat;
using Conversion = HostFormat::Conversion;

constexpr u64 LinearMemoryEnd = 16_GB;

// Guest memory for [address, address + size): linear memory directly, or gathered from the
// translated high region (core/wasm_high_memory.h) into `scratch`.
const u8* GuestBytes(VAddr address, u64 size, std::vector<u8>& scratch) {
    if (!Core::HighMemory::InRange(address)) {
        return address + size <= LinearMemoryEnd ? reinterpret_cast<const u8*>(address) : nullptr;
    }
    using Core::HighMemory::kPageSize;
    scratch.assign(size, 0);
    for (u64 offset = 0; offset < size;) {
        const VAddr va = address + offset;
        const u64 n = std::min(size - offset, kPageSize - (va & (kPageSize - 1)));
        if (const u8* page = Core::HighMemory::Translate(va)) {
            std::memcpy(&scratch[offset], page, n);
        }
        offset += n;
    }
    return scratch.data();
}

HostFormat Make(const char* format, u32 guest_bytes, char sample_type,
                Conversion conversion = Conversion::None, u32 host_bytes = 0, u32 components = 0) {
    return HostFormat{format,     guest_bytes, host_bytes ? host_bytes : guest_bytes,
                      components, conversion,  false,
                      sample_type};
}

u16 FloatToHalf(float f) {
    const u32 x = std::bit_cast<u32>(f);
    const u32 sign = (x >> 16) & 0x8000;
    const s32 exp = static_cast<s32>((x >> 23) & 0xFF) - 127 + 15;
    u32 mant = x & 0x7FFFFF;
    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<u16>(sign);
        }
        mant |= 0x800000;
        return static_cast<u16>(sign | (mant >> (14 - exp)));
    }
    if (exp >= 31) {
        return static_cast<u16>(sign | 0x7C00);
    }
    return static_cast<u16>(sign | (exp << 10) | (mant >> 13));
}

// Rewrites `count` guest elements at `src` into host elements at `dst`.
void Convert(const HostFormat& host, const u8* src, u8* dst, u64 count) {
    const auto expand = [](u32 v, u32 bits) {
        return static_cast<u8>((v * 255u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
    };
    switch (host.conversion) {
    case Conversion::None:
        std::memcpy(dst, src, count * host.guest_bytes);
        return;
    case Conversion::Unsupported:
        for (u64 i = 0; i < count; i++) {
            const u32 magenta = 0xFFFF00FFu;
            std::memcpy(dst + i * 4, &magenta, 4);
        }
        return;
    case Conversion::R5G6B5:
    case Conversion::A1R5G5B5:
    case Conversion::B4G4R4A4:
        for (u64 i = 0; i < count; i++) {
            u16 v;
            std::memcpy(&v, src + i * 2, 2);
            u8* o = dst + i * 4;
            if (host.conversion == Conversion::R5G6B5) {
                o[0] = expand(v >> 11, 5), o[1] = expand((v >> 5) & 63, 6), o[2] = expand(v & 31, 5);
                o[3] = 255;
            } else if (host.conversion == Conversion::A1R5G5B5) {
                o[0] = expand((v >> 10) & 31, 5), o[1] = expand((v >> 5) & 31, 5);
                o[2] = expand(v & 31, 5), o[3] = (v >> 15) ? 255 : 0;
            } else {
                o[0] = expand((v >> 4) & 15, 4), o[1] = expand((v >> 8) & 15, 4);
                o[2] = expand(v >> 12, 4), o[3] = expand(v & 15, 4);
            }
        }
        return;
    case Conversion::Norm16ToHalf:
    case Conversion::Snorm8ToHalf: {
        const bool is16 = host.conversion == Conversion::Norm16ToHalf;
        const bool is_signed = host.signed_source;
        const u32 comps = host.components;
        const u32 host_comps = host.host_bytes / 2;
        for (u64 i = 0; i < count; i++) {
            for (u32 c = 0; c < host_comps; c++) {
                float f = c == 3 ? 1.0f : 0.0f;
                if (c < comps) {
                    if (is16) {
                        u16 v;
                        std::memcpy(&v, src + (i * comps + c) * 2, 2);
                        f = is_signed ? std::max(static_cast<s16>(v) / 32767.0f, -1.0f)
                                      : v / 65535.0f;
                    } else {
                        const s8 v = static_cast<s8>(src[i * comps + c]);
                        f = std::max(v / 127.0f, -1.0f);
                    }
                }
                const u16 h = FloatToHalf(f);
                std::memcpy(dst + (i * host_comps + c) * 2, &h, 2);
            }
        }
        return;
    }
    case Conversion::Rgb32ToRgba32: {
        const u32 one = host.sample_type == 'f' ? std::bit_cast<u32>(1.0f) : 1u;
        for (u64 i = 0; i < count; i++) {
            std::memcpy(dst + i * 16, src + i * 12, 12);
            std::memcpy(dst + i * 16 + 12, &one, 4);
        }
        return;
    }
    case Conversion::Srgb8ToRgba8:
        for (u64 i = 0; i < count; i++) {
            u8* o = dst + i * 4;
            o[0] = src[i * host.components];
            o[1] = host.components > 1 ? src[i * host.components + 1] : 0;
            o[2] = 0;
            o[3] = 255;
        }
        return;
    }
}

std::string ViewDimension(const std::string& wgsl_type) {
    if (wgsl_type.find("cube_array") != std::string::npos) {
        return "cube-array";
    }
    if (wgsl_type.find("cube") != std::string::npos) {
        return "cube";
    }
    if (wgsl_type.find("2d_array") != std::string::npos) {
        return "2d-array";
    }
    if (wgsl_type.find("3d") != std::string::npos) {
        return "3d";
    }
    return "2d";
}

char DeclaredSampleType(const std::string& wgsl_type) {
    if (wgsl_type.starts_with("texture_depth")) {
        return 'd';
    }
    if (wgsl_type.find("<u32") != std::string::npos) {
        return 'u';
    }
    if (wgsl_type.find("<i32") != std::string::npos) {
        return 's';
    }
    return 'f';
}

Gpu::Json Dummy(const std::string& wgsl_type) {
    return {{"dummy", std::string(1, DeclaredSampleType(wgsl_type))},
            {"dimension", ViewDimension(wgsl_type)},
            {"multisampled", wgsl_type.find("multisampled") != std::string::npos}};
}

} // namespace

HostFormat ToHostFormat(DataFormat dfmt, NumberFormat nfmt, bool rt) {
    const bool sint = nfmt == NumberFormat::Sint;
    const bool uint = nfmt == NumberFormat::Uint;
    const bool snorm = nfmt == NumberFormat::Snorm;
    const char st = sint ? 's' : uint ? 'u' : 'f';
    const auto norm16 = [&](const char* f16, u32 comps) {
        HostFormat h = Make(f16, comps * 2, 'f', Conversion::Norm16ToHalf, comps * 2, comps);
        h.signed_source = snorm;
        return h;
    };
    HostFormat h;
    switch (dfmt) {
    case DataFormat::Format8:
        if (nfmt == NumberFormat::Srgb) {
            h = Make("rgba8unorm-srgb", 1, 'f', Conversion::Srgb8ToRgba8, 4, 1);
        } else if (snorm && rt) {
            h = Make("r16float", 1, 'f', Conversion::Snorm8ToHalf, 2, 1);
        } else {
            h = Make(sint ? "r8sint" : uint ? "r8uint" : snorm ? "r8snorm" : "r8unorm", 1, st);
        }
        break;
    case DataFormat::Format8_8:
        if (nfmt == NumberFormat::Srgb) {
            h = Make("rgba8unorm-srgb", 2, 'f', Conversion::Srgb8ToRgba8, 4, 2);
        } else if (snorm && rt) {
            h = Make("rg16float", 2, 'f', Conversion::Snorm8ToHalf, 4, 2);
        } else {
            h = Make(sint ? "rg8sint" : uint ? "rg8uint" : snorm ? "rg8snorm" : "rg8unorm", 2, st);
        }
        break;
    case DataFormat::Format8_8_8_8:
        if (snorm && rt) {
            h = Make("rgba16float", 4, 'f', Conversion::Snorm8ToHalf, 8, 4);
        } else {
            h = Make(sint                          ? "rgba8sint"
                     : uint                        ? "rgba8uint"
                     : snorm                       ? "rgba8snorm"
                     : nfmt == NumberFormat::Srgb ? "rgba8unorm-srgb"
                                                   : "rgba8unorm",
                     4, st);
        }
        break;
    case DataFormat::Format16:
        h = sint || uint ? Make(sint ? "r16sint" : "r16uint", 2, st)
            : nfmt == NumberFormat::Float ? Make("r16float", 2, 'f')
                                          : norm16("r16float", 1);
        break;
    case DataFormat::Format16_16:
        h = sint || uint ? Make(sint ? "rg16sint" : "rg16uint", 4, st)
            : nfmt == NumberFormat::Float ? Make("rg16float", 4, 'f')
                                          : norm16("rg16float", 2);
        break;
    case DataFormat::Format16_16_16_16:
        h = sint || uint ? Make(sint ? "rgba16sint" : "rgba16uint", 8, st)
            : nfmt == NumberFormat::Float ? Make("rgba16float", 8, 'f')
                                          : norm16("rgba16float", 4);
        break;
    case DataFormat::Format32:
        h = Make(sint ? "r32sint" : uint ? "r32uint" : "r32float", 4, st);
        break;
    case DataFormat::Format32_32:
        h = Make(sint ? "rg32sint" : uint ? "rg32uint" : "rg32float", 8, st);
        break;
    case DataFormat::Format32_32_32:
        h = Make(sint ? "rgba32sint" : uint ? "rgba32uint" : "rgba32float", 12, st,
                 Conversion::Rgb32ToRgba32, 16);
        break;
    case DataFormat::Format32_32_32_32:
        h = Make(sint ? "rgba32sint" : uint ? "rgba32uint" : "rgba32float", 16, st);
        break;
    case DataFormat::Format10_11_11:
        if (rt && !Gpu::GetFeatures().rg11b10_renderable) {
            // Rendered as f16 instead; the guest's initial content can't be carried over.
            h = Make("rgba16float", 4, 'f', Conversion::Unsupported, 8);
        } else {
            h = Make("rg11b10ufloat", 4, 'f');
        }
        break;
    case DataFormat::Format2_10_10_10:
        h = Make(uint ? "rgb10a2uint" : "rgb10a2unorm", 4, uint ? 'u' : 'f');
        break;
    case DataFormat::Format5_9_9_9:
        h = Make("rgb9e5ufloat", 4, 'f');
        break;
    case DataFormat::Format5_6_5:
        h = Make("rgba8unorm", 2, 'f', Conversion::R5G6B5, 4);
        break;
    case DataFormat::Format1_5_5_5:
        h = Make("rgba8unorm", 2, 'f', Conversion::A1R5G5B5, 4);
        break;
    case DataFormat::Format4_4_4_4:
        h = Make("rgba8unorm", 2, 'f', Conversion::B4G4R4A4, 4);
        break;
    case DataFormat::FormatBc1:
    case DataFormat::FormatBc2:
    case DataFormat::FormatBc3:
    case DataFormat::FormatBc4:
    case DataFormat::FormatBc5:
    case DataFormat::FormatBc6:
    case DataFormat::FormatBc7: {
        const bool srgb = nfmt == NumberFormat::Srgb;
        const char* name = nullptr;
        u32 bytes = 16;
        switch (dfmt) {
        case DataFormat::FormatBc1:
            name = srgb ? "bc1-rgba-unorm-srgb" : "bc1-rgba-unorm";
            bytes = 8;
            break;
        case DataFormat::FormatBc2:
            name = srgb ? "bc2-rgba-unorm-srgb" : "bc2-rgba-unorm";
            break;
        case DataFormat::FormatBc3:
            name = srgb ? "bc3-rgba-unorm-srgb" : "bc3-rgba-unorm";
            break;
        case DataFormat::FormatBc4:
            name = snorm ? "bc4-r-snorm" : "bc4-r-unorm";
            bytes = 8;
            break;
        case DataFormat::FormatBc5:
            name = snorm ? "bc5-rg-snorm" : "bc5-rg-unorm";
            break;
        case DataFormat::FormatBc6:
            name = snorm ? "bc6h-rgb-float" : "bc6h-rgb-ufloat";
            break;
        default:
            name = srgb ? "bc7-rgba-unorm-srgb" : "bc7-rgba-unorm";
            break;
        }
        if (!Gpu::GetFeatures().bc) {
            h = Make("rgba8unorm", bytes, 'f', Conversion::Unsupported, 4);
        } else {
            h = Make(name, bytes, 'f');
            h.is_block = true;
        }
        break;
    }
    default:
        h = Make("rgba8unorm", std::max(1u, AmdGpu::NumBitsPerBlock(dfmt) / 8), 'f',
                 Conversion::Unsupported, 4);
        break;
    }
    return h;
}

Surfaces::Surfaces() = default;
Surfaces::~Surfaces() = default;

void Surfaces::CheckGuest(Surface& surface, bool force) {
    if (!force && surface.checked_frame == frame) {
        return;
    }
    surface.checked_frame = frame;
    std::vector<u8> scratch;
    const u8* src = GuestBytes(surface.address, surface.layout.guest_size, scratch);
    if (src == nullptr) {
        return;
    }
    const u64 hash = XXH3_64bits(src, surface.layout.guest_size);
    if (!surface.uploaded || hash != surface.guest_hash) {
        surface.stale = true;
    }
    surface.guest_hash = hash;
}

Surface* Surfaces::Create(Gpu::Batch& batch, std::unique_ptr<Surface> s) {
    s->id = next_id++;
    Gpu::Json view_formats = Gpu::Json::array();
    if ((s->render_target || s->storage) && s->host.format.starts_with("rgba8unorm")) {
        view_formats = {"rgba8unorm", "rgba8unorm-srgb"};
    }
    // Block-compressed textures must be whole blocks in WebGPU, even when the guest's isn't.
    const u32 align = s->host.is_block ? 4 : 1;
    batch.Add({{"op", "texture"},
               {"id", s->id},
               {"format", s->host.format},
               {"size",
                {Common::AlignUp(s->width, align), Common::AlignUp(s->height, align),
                 s->depth_or_layers}},
               {"dimension", s->is_3d ? "3d" : "2d"},
               {"levels", s->layout.levels},
               {"renderTarget", s->render_target},
               {"storage", s->storage},
               {"viewFormats", view_formats}});
    Surface* ptr = s.get();
    surfaces.emplace(s->address, std::move(s));
    return ptr;
}

void Surfaces::Destroy(Gpu::Batch& batch, Surface& surface) {
    batch.Add({{"op", "destroyTexture"}, {"texture", surface.id}});
    for (auto it = surfaces.begin(); it != surfaces.end(); ++it) {
        if (it->second.get() == &surface) {
            surfaces.erase(it);
            return;
        }
    }
}

void Surfaces::Upload(Gpu::Batch& batch, Surface& s) {
    const auto& layout = s.layout;
    s.uploaded = true;
    s.stale = false;
    if (s.render_target && s.host.conversion == Conversion::Unsupported) {
        return; // starts out blank
    }
    std::vector<u8> scratch;
    const u8* guest = GuestBytes(s.address, layout.guest_size, scratch);
    if (guest == nullptr) {
        LOG_ERROR(Render, "WebGPU: surface at {:#x} is outside guest memory", s.address);
        return;
    }
    std::vector<u8> linear(layout.guest_size);
    Detile(layout, guest, linear.data());
    if (static const bool trace = std::getenv("SHADPS4_WEBGPU_TRACE") != nullptr; trace) {
        u32 first = 0;
        std::memcpy(&first, linear.data(), std::min<size_t>(4, linear.size()));
        LOG_INFO(Render, "WebGPU upload: texture {} at {:#x} {}x{}x{} {} levels {} first {:#010x}",
                 s.id, s.address, s.width, s.height, s.depth_or_layers, s.host.format,
                 layout.levels, first);
    }
    const u32 align = s.host.is_block ? 4 : 1;
    for (u32 level = 0; level < layout.levels; level++) {
        const auto& mip = layout.mips[level];
        const u32 pitch = layout.MipPitchElements(level);
        const u32 rows = layout.MipHeightElements(level);
        const u32 width = std::max(1u, s.width >> level);
        const u32 height = std::max(1u, s.height >> level);
        const u32 slices = s.is_3d ? std::max(1u, s.depth_or_layers >> level) : s.depth_or_layers;
        const u64 guest_slice = u64(pitch) * rows * s.host.guest_bytes;
        if (mip.offset + guest_slice * slices > linear.size()) {
            break;
        }
        const u64 count = u64(pitch) * rows * slices;
        u8* dst;
        const u64 offset = batch.Allocate(count * s.host.host_bytes, &dst);
        Convert(s.host, linear.data() + mip.offset, dst, count);
        batch.Add({{"op", "write"},
                   {"texture", s.id},
                   {"level", level},
                   {"offset", offset},
                   {"bytesPerRow", pitch * s.host.host_bytes},
                   {"rowsPerImage", rows},
                   // In texels: whole blocks for BCn (the texture was created block-aligned).
                   {"size",
                    {Common::AlignUp(width, align), Common::AlignUp(height, align), slices}}});
    }
}

Surface* Surfaces::ColorTarget(Gpu::Batch& batch, const AmdGpu::ColorBuffer& cb,
                               AmdGpu::CbDbExtent hint) {
    const VAddr address = cb.Address();
    const auto dfmt = cb.GetDataFmt();
    const auto nfmt = cb.GetNumberFmt();
    const HostFormat host = ToHostFormat(dfmt, nfmt, true);
    if (host.conversion == Conversion::Unsupported || host.is_block) {
        return nullptr;
    }
    const u32 width = hint.Valid() ? hint.width : cb.Pitch();
    const u32 height = hint.Valid() ? hint.height : cb.Height();
    const u32 layers = cb.NumSlices();
    Surface* found = nullptr;
    for (auto [it, end] = surfaces.equal_range(address); it != end;) {
        Surface& s = *it->second;
        ++it;
        if (!s.render_target || s.is_depth) {
            continue;
        }
        if (s.host.format == host.format && s.width == width && s.height == height &&
            s.depth_or_layers == layers) {
            found = &s;
        } else {
            // The memory holds a different render target now.
            Destroy(batch, s);
        }
    }
    if (found == nullptr) {
        auto s = std::make_unique<Surface>();
        s->address = address;
        s->host = host;
        s->dfmt = dfmt;
        s->nfmt = nfmt;
        s->width = width;
        s->height = height;
        s->depth_or_layers = layers;
        s->render_target = true;
        auto& layout = s->layout;
        layout.tile_mode = cb.GetTileMode();
        layout.array_mode = AmdGpu::GetArrayMode(layout.tile_mode);
        layout.num_bits = AmdGpu::NumBitsPerBlock(dfmt);
        layout.num_samples = cb.NumSamples();
        layout.pitch = cb.Pitch();
        layout.height = cb.Height();
        layout.layers = layers;
        if (!layout.Compute()) {
            return nullptr;
        }
        found = Create(batch, std::move(s));
        LOG_INFO(Render, "WebGPU: render target {:#x} {}x{} {} ({})", address, width, height,
                 found->host.format, AmdGpu::NameOf(layout.tile_mode));
    }
    CheckGuest(*found);
    if (found->stale) {
        // New, or written by the CPU since the GPU last drew into it.
        Upload(batch, *found);
    }
    MarkWritten(*found);
    found->used_frame = frame;
    return found;
}

Surface* Surfaces::DepthTarget(Gpu::Batch& batch, const AmdGpu::DepthBuffer& db,
                               AmdGpu::CbDbExtent hint) {
    const VAddr address = db.DepthValid() ? db.DepthAddress() : db.StencilAddress();
    const bool has_stencil = db.StencilValid();
    const bool has_depth = db.DepthValid();
    const char* format = has_stencil
                             ? (Gpu::GetFeatures().depth32_stencil8 ? "depth32float-stencil8"
                                                                    : "depth24plus-stencil8")
                             : "depth32float";
    if (!has_depth && has_stencil) {
        format = "stencil8";
    }
    const u32 width = hint.Valid() ? hint.width : db.Pitch();
    const u32 height = hint.Valid() ? hint.height : db.Height();
    for (auto [it, end] = surfaces.equal_range(address); it != end;) {
        Surface& s = *it->second;
        ++it;
        if (!s.is_depth) {
            continue;
        }
        if (s.host.format == format && s.width == width && s.height == height) {
            MarkWritten(s);
            s.used_frame = frame;
            return &s;
        }
        Destroy(batch, s);
    }
    auto s = std::make_unique<Surface>();
    s->address = address;
    s->host = Make(format, db.NumBits() / 8, 'd');
    s->dfmt = db.z_info.format == AmdGpu::DepthBuffer::ZFormat::Z16 ? DataFormat::Format16
                                                                    : DataFormat::Format32;
    s->nfmt = NumberFormat::Float;
    s->width = width;
    s->height = height;
    s->depth_or_layers = 1;
    s->is_depth = true;
    s->has_stencil = has_stencil;
    s->render_target = true;
    s->uploaded = true;
    auto& layout = s->layout;
    layout.tile_mode = db.GetTileMode();
    layout.array_mode = AmdGpu::GetArrayMode(layout.tile_mode);
    layout.num_bits = std::max(8u, db.NumBits());
    layout.num_samples = db.NumSamples();
    layout.pitch = db.Pitch();
    layout.height = db.Height();
    layout.Compute();
    Surface* created = Create(batch, std::move(s));
    MarkWritten(*created);
    created->used_frame = frame;
    batch.Add({{"op", "clearTexture"}, {"texture", created->id}, {"depth", 0.0}, {"stencil", 0}});
    LOG_INFO(Render, "WebGPU: depth target {:#x} {}x{} {}", address, width, height, format);
    return created;
}

Gpu::Json Surfaces::Texture(Gpu::Batch& batch, const AmdGpu::Image& sharp, bool is_array,
                            const std::string& wgsl_type) {
    if (!sharp || !sharp.Valid()) {
        return Dummy(wgsl_type);
    }
    const VAddr address = sharp.Address();
    const auto dfmt = sharp.GetDataFmt();
    const auto nfmt = sharp.GetNumberFmt();
    const auto type = sharp.GetType();
    const char declared = DeclaredSampleType(wgsl_type);
    const u32 width = sharp.width + 1;
    const u32 height = sharp.height + 1;
    const bool is_3d = type == AmdGpu::ImageType::Color3D;
    const u32 layers = is_3d ? sharp.depth + 1 : sharp.NumLayers();
    Gpu::Json view = View(sharp, is_array, wgsl_type);
    const u32 base_level = view["baseMipLevel"].get<u32>();

    // Something the GPU wrote there (the newest, if several) takes precedence over guest memory.
    Surface* newest = nullptr;
    for (auto [it, end] = surfaces.equal_range(address); it != end; ++it) {
        Surface& s = *it->second;
        if (!s.gpu_modified) {
            continue;
        }
        CheckGuest(s);
        if (s.stale) {
            // The CPU wrote over it since; guest memory is current again (and the surface gets
            // it the next time the GPU writes it).
            s.gpu_modified = false;
            continue;
        }
        if (newest == nullptr || s.written > newest->written) {
            newest = &s;
        }
    }
    if (newest != nullptr) {
        Surface& s = *newest;
        s.used_frame = frame;
        Gpu::Json binding{{"tex", s.id}};
        if (s.is_depth) {
            if (declared != 'd') {
                binding["depthAsColor"] = true;
            }
            view["aspect"] = "depth-only";
        } else if ((declared == 'f') != (s.host.sample_type == 'f') ||
                   (declared != 'f' && declared != s.host.sample_type)) {
            Gpu::LogOnce(false, fmt::format("WebGPU: texture {:#x} reads a {} surface as "
                                            "another kind of value; bound as a blank texture",
                                            address, s.host.format));
            return Dummy(wgsl_type);
        }
        if (s.host.format.starts_with("rgba8unorm")) {
            view["format"] = nfmt == NumberFormat::Srgb ? "rgba8unorm-srgb" : "rgba8unorm";
        }
        if (s.layout.levels <= base_level) {
            view["baseMipLevel"] = 0;
        }
        view["mipLevelCount"] = std::min<u32>(view["mipLevelCount"].get<u32>(),
                                              s.layout.levels - view["baseMipLevel"].get<u32>());
        view["arrayLayerCount"] = std::min<u32>(view["arrayLayerCount"].get<u32>(),
                                                s.depth_or_layers);
        view["baseArrayLayer"] = 0;
        if (width != s.width || height != s.height) {
            // Normalized coordinates span the T#'s size: sample a copy of that region.
            binding["copy"] = {std::min(width, s.width), std::min(height, s.height)};
        }
        binding["view"] = std::move(view);
        return binding;
    }

    const HostFormat host = ToHostFormat(dfmt, nfmt, false);
    if (declared == 'd' || (declared == 'f') != (host.sample_type == 'f') ||
        (declared != 'f' && declared != host.sample_type)) {
        // Depth comparisons on a texture that wasn't rendered (WebGPU can't upload into depth
        // formats), or a T# whose type doesn't match how the shader reads it.
        return Dummy(wgsl_type);
    }
    Surface* found = nullptr;
    for (auto [it, end] = surfaces.equal_range(address); it != end; ++it) {
        Surface& s = *it->second;
        if (!s.render_target && !s.storage && s.dfmt == dfmt && s.host.format == host.format &&
            s.width == width && s.height == height && s.depth_or_layers == layers &&
            s.is_3d == is_3d && s.layout.levels == sharp.NumLevels() &&
            s.layout.tile_mode == sharp.GetTileMode() && s.layout.pitch == sharp.Pitch()) {
            found = &s;
            break;
        }
    }
    if (found == nullptr) {
        auto s = std::make_unique<Surface>();
        s->address = address;
        s->host = host;
        s->dfmt = dfmt;
        s->nfmt = nfmt;
        s->width = width;
        s->height = height;
        s->depth_or_layers = layers;
        s->is_3d = is_3d;
        auto& layout = s->layout;
        layout.tile_mode = sharp.GetTileMode();
        layout.array_mode = AmdGpu::GetArrayMode(layout.tile_mode);
        layout.num_bits = AmdGpu::NumBitsPerBlock(dfmt);
        layout.num_samples = sharp.NumSamples();
        layout.pitch = sharp.Pitch();
        layout.height = height;
        layout.depth = is_3d ? layers : 1;
        layout.layers = is_3d ? 1 : layers;
        layout.levels = sharp.NumLevels();
        layout.is_block = AmdGpu::IsBlockCoded(dfmt);
        layout.is_pow2 = sharp.pow2pad;
        const u32 max_size = Gpu::GetFeatures().max_texture_size;
        if (!layout.Compute() || width > max_size || height > max_size) {
            Gpu::LogOnce(false, fmt::format("WebGPU: texture {:#x} ({}x{} {} {}) has a layout "
                                            "this renderer can't read; bound as a blank texture",
                                            address, width, height, AmdGpu::NameOf(dfmt),
                                            AmdGpu::NameOf(layout.tile_mode)));
            return Dummy(wgsl_type);
        }
        found = Create(batch, std::move(s));
    }
    CheckGuest(*found);
    if (found->stale) {
        Upload(batch, *found);
    }
    found->used_frame = frame;
    view["mipLevelCount"] = std::min<u32>(view["mipLevelCount"].get<u32>(),
                                          found->layout.levels - base_level);
    return {{"tex", found->id}, {"view", std::move(view)}};
}

Gpu::Json Surfaces::View(const AmdGpu::Image& sharp, bool is_array, const std::string& wgsl_type) {
    const bool is_3d = sharp.GetType() == AmdGpu::ImageType::Color3D;
    const u32 layers = is_3d ? sharp.depth + 1 : sharp.NumLayers();
    const std::string dimension = ViewDimension(wgsl_type);
    Gpu::Json view{{"dimension", dimension}};
    view["baseMipLevel"] = std::min<u32>(sharp.base_level, sharp.NumLevels() - 1);
    view["mipLevelCount"] = std::max(1u, sharp.NumViewLevels(is_array));
    if (dimension == "2d" || dimension == "3d") {
        view["baseArrayLayer"] = is_3d ? 0 : std::min<u32>(sharp.base_array, layers - 1);
        view["arrayLayerCount"] = 1;
    } else if (dimension == "cube") {
        view["baseArrayLayer"] = 0;
        view["arrayLayerCount"] = 6;
    } else {
        view["baseArrayLayer"] = std::min<u32>(sharp.base_array, layers - 1);
        u32 count = sharp.NumViewLayers(is_array);
        if (dimension == "cube-array") {
            count = std::max(6u, count - count % 6);
        } else if (sharp.GetType() == AmdGpu::ImageType::Cube) {
            count = layers - view["baseArrayLayer"].get<u32>(); // a cube written as its faces
        }
        view["arrayLayerCount"] = count;
    }
    return view;
}

Gpu::Json Surfaces::StorageImage(Gpu::Batch& batch, const AmdGpu::Image& sharp, bool is_array,
                                 const std::string& wgsl_type, const std::string& format) {
    const VAddr address = sharp.Address();
    const auto dfmt = sharp.GetDataFmt();
    const auto nfmt = sharp.GetNumberFmt();
    const u32 width = sharp.width + 1;
    const u32 height = sharp.height + 1;
    const bool is_3d = sharp.GetType() == AmdGpu::ImageType::Color3D;
    const u32 layers = is_3d ? sharp.depth + 1 : sharp.NumLayers();
    Surface* found = nullptr;
    for (auto [it, end] = surfaces.equal_range(address); it != end; ++it) {
        Surface& s = *it->second;
        if ((s.storage || s.render_target) && !s.is_depth && s.host.format == format &&
            s.width == width && s.height == height && s.depth_or_layers == layers &&
            s.is_3d == is_3d && s.layout.levels == sharp.NumLevels() &&
            (found == nullptr || s.written > found->written)) {
            found = &s;
        }
    }
    if (found == nullptr || !found->storage) {
        auto s = std::make_unique<Surface>();
        s->address = address;
        const char st = nfmt == NumberFormat::Uint ? 'u' : nfmt == NumberFormat::Sint ? 's' : 'f';
        s->host = Make(format.c_str(), AmdGpu::NumBitsPerBlock(dfmt) / 8, st);
        s->dfmt = dfmt;
        s->nfmt = nfmt;
        s->width = width;
        s->height = height;
        s->depth_or_layers = layers;
        s->is_3d = is_3d;
        s->storage = true;
        auto& layout = s->layout;
        layout.tile_mode = sharp.GetTileMode();
        layout.array_mode = AmdGpu::GetArrayMode(layout.tile_mode);
        layout.num_bits = AmdGpu::NumBitsPerBlock(dfmt);
        layout.num_samples = 1;
        layout.pitch = sharp.Pitch();
        layout.height = height;
        layout.depth = is_3d ? layers : 1;
        layout.layers = is_3d ? 1 : layers;
        layout.levels = sharp.NumLevels();
        layout.is_pow2 = sharp.pow2pad;
        if (!layout.Compute()) {
            Gpu::LogOnce(false, fmt::format("WebGPU: written image {:#x} ({}x{} {}) has a "
                                            "layout this renderer can't read",
                                            address, width, height, format));
            return {};
        }
        Surface* previous = found;
        found = Create(batch, std::move(s));
        if (previous != nullptr) {
            // A render target with this format: carry its content over.
            batch.Add({{"op", "copyTexture"},
                       {"src", previous->id},
                       {"dst", found->id},
                       {"size", {width, height}}});
            found->uploaded = true;
            found->guest_hash = previous->guest_hash;
            found->checked_frame = previous->checked_frame;
        }
        LOG_INFO(Render, "WebGPU: written image {:#x} {}x{} {} ({})", address, width, height,
                 format, AmdGpu::NameOf(layout.tile_mode));
    }
    CheckGuest(*found);
    if (found->stale) {
        Upload(batch, *found);
    }
    MarkWritten(*found);
    found->used_frame = frame;
    Gpu::Json view = View(sharp, is_array, wgsl_type);
    view["mipLevelCount"] = 1; // storage views are one level
    view["baseMipLevel"] = std::min<u32>(view["baseMipLevel"].get<u32>(), found->layout.levels - 1);
    return {{"tex", found->id}, {"view", std::move(view)}, {"storage", true}};
}

void Surfaces::NextFrame(Gpu::Batch& batch) {
    constexpr u64 EvictAfterFrames = 600;
    if (++frame % 64 != 0) {
        return;
    }
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        Surface& s = *it->second;
        if (!s.gpu_modified && frame - s.used_frame > EvictAfterFrames) {
            batch.Add({{"op", "destroyTexture"}, {"texture", s.id}});
            it = surfaces.erase(it);
        } else {
            ++it;
        }
    }
}

void Surfaces::Invalidate(VAddr address, u64 size) {
    for (auto& [base, s] : surfaces) {
        if (base < address + size && address < base + s->layout.guest_size) {
            s->checked_frame = ~0ull;
        }
    }
}

Surface* Surfaces::FindScanout(VAddr address, u32 width, u32 height) {
    // What the GPU wrote there last: a render target, or an image a shader wrote.
    Surface* newest = nullptr;
    for (auto [it, end] = surfaces.equal_range(address); it != end; ++it) {
        Surface& s = *it->second;
        if (s.is_depth || !s.gpu_modified || s.width < width || s.height < height ||
            (newest != nullptr && s.written < newest->written)) {
            continue;
        }
        newest = &s;
    }
    if (newest == nullptr) {
        return nullptr;
    }
    CheckGuest(*newest, true);
    if (newest->stale) {
        newest->gpu_modified = false;
        return nullptr;
    }
    return newest;
}

} // namespace Web
