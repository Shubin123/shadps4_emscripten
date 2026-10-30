// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/wasm_high_memory.h"
#include "video_core/renderer_web/web_resources.h"

namespace Web {

namespace {

namespace WGSL = Shader::Backend::WGSL;

// Larger guest buffers are bound truncated (WebGPU's own limit is lower on many adapters).
constexpr u64 MaxBindingSize = 256_MB;
// Linear memory ends here; guest addresses below it are host addresses.
constexpr u64 LinearMemoryEnd = 16_GB;

const char* AddressMode(AmdGpu::ClampMode mode) {
    switch (mode) {
    case AmdGpu::ClampMode::Wrap:
        return "repeat";
    case AmdGpu::ClampMode::Mirror:
        return "mirror-repeat";
    default:
        // WebGPU has no border colors or mirror-once.
        return "clamp-to-edge";
    }
}

const char* CompareFunction(u32 func) {
    static constexpr const char* names[] = {"never",   "less",      "equal",         "less-equal",
                                            "greater", "not-equal", "greater-equal", "always"};
    return names[func & 7];
}

Gpu::Json SamplerDesc(const AmdGpu::Sampler& s, bool comparison) {
    const auto filter = [](AmdGpu::Filter f) {
        return f == AmdGpu::Filter::Bilinear || f == AmdGpu::Filter::AnisoLinear ? "linear"
                                                                                  : "nearest";
    };
    const char* mag = filter(s.xy_mag_filter);
    const char* min = filter(s.xy_min_filter);
    const char* mip = s.mip_filter == AmdGpu::MipFilter::Linear ? "linear" : "nearest";
    float lod_min = std::min(s.MinLod(), 32.0f);
    float lod_max = std::max(lod_min, s.MaxLod());
    if (s.mip_filter == AmdGpu::MipFilter::None) {
        lod_max = lod_min; // base level only
    }
    Gpu::Json d{{"addressModeU", AddressMode(s.clamp_x)},
                {"addressModeV", AddressMode(s.clamp_y)},
                {"addressModeW", AddressMode(s.clamp_z)},
                {"magFilter", mag},
                {"minFilter", min},
                {"mipmapFilter", mip},
                {"lodMinClamp", lod_min},
                {"lodMaxClamp", lod_max}};
    const bool aniso = AmdGpu::IsAnisoFilter(s.xy_mag_filter) ||
                       AmdGpu::IsAnisoFilter(s.xy_min_filter);
    if (aniso && s.max_aniso.Value() != AmdGpu::AnisoRatio::One &&
        std::string_view(mag) == "linear" && std::string_view(min) == "linear" &&
        std::string_view(mip) == "linear") {
        d["maxAnisotropy"] = static_cast<u32>(s.MaxAniso());
    }
    if (comparison) {
        d["compare"] = CompareFunction(static_cast<u32>(s.depth_compare_func.Value()));
    }
    return d;
}

} // namespace

std::optional<u64> Uploads::Guest(Gpu::Batch& batch, VAddr address, u64 size) {
    const auto key = std::make_pair(address, size);
    if (const auto it = ranges.find(key); it != ranges.end()) {
        return it->second;
    }
    u64 offset;
    if (!Core::HighMemory::InRange(address)) {
        if (address + size > LinearMemoryEnd) {
            return std::nullopt;
        }
        offset = batch.Upload(reinterpret_cast<const void*>(address), size);
    } else {
        u8* dst;
        offset = batch.Allocate(size, &dst);
        using Core::HighMemory::kPageSize;
        for (u64 done = 0; done < size;) {
            const VAddr va = address + done;
            const u64 n = std::min(size - done, kPageSize - (va & (kPageSize - 1)));
            if (const u8* page = Core::HighMemory::Translate(va)) {
                std::memcpy(dst + done, page, n);
            } else {
                std::memset(dst + done, 0, n);
            }
            done += n;
        }
    }
    ranges.emplace(key, offset);
    return offset;
}

u64 Uploads::Host(Gpu::Batch& batch, const void* data, u64 size) {
    return batch.Upload(data, size);
}

void Uploads::Forget(VAddr address, u64 size) {
    std::erase_if(ranges, [&](const auto& entry) {
        const auto& [range, offset] = entry;
        return range.first < address + size && address < range.first + range.second;
    });
}

bool BindStage(Gpu::Batch& batch, Uploads& uploads, Surfaces& surfaces, const Shader::Info& info,
               const WGSL::Output& out, std::span<u32> gds, StageBindings& bindings,
               std::span<const float> clip_planes) {
    static const u32 null_buffer[4]{};
    const u64 max_size = std::min<u64>(MaxBindingSize, std::max<u64>(Gpu::GetFeatures().max_binding_size, 16));
    for (const auto& b : out.bindings) {
        Gpu::Json layout{{"b", b.binding}, {"type", b.wgsl_type}};
        switch (b.kind) {
        case WGSL::Binding::Kind::Buffer: {
            layout["kind"] = "buffer";
            layout["writable"] = b.writable;
            const auto& desc = info.buffers[b.resource];
            u64 offset = 0;
            u64 size = 0;
            switch (desc.buffer_type) {
            case Shader::BufferType::Guest: {
                const auto sharp = desc.GetSharp(info);
                size = sharp.GetSize();
                if (sharp.base_address == 0 || size == 0) {
                    size = 0;
                    break;
                }
                if (sharp.base_address & 3) {
                    // The WGSL backend indexes buffers in dwords from the bound base.
                    Gpu::LogOnce(true, fmt::format("WebGPU: shader {:#x}: buffer at {:#x} isn't "
                                                 "dword aligned",
                                                 info.pgm_hash, u64(sharp.base_address)));
                    return false;
                }
                if (size > max_size) {
                    Gpu::LogOnce(false, fmt::format("WebGPU: buffer at {:#x} truncated from {} to {} "
                                                  "bytes",
                                                  u64(sharp.base_address), size, max_size));
                    size = max_size;
                }
                size = Common::AlignUp(size, 4);
                const auto at = uploads.Guest(batch, sharp.base_address, size);
                if (!at) {
                    Gpu::LogOnce(true, fmt::format("WebGPU: buffer at {:#x} is outside guest memory",
                                                 u64(sharp.base_address)));
                    return false;
                }
                offset = *at;
                if (b.writable) {
                    bindings.writebacks.push_back({offset, size, sharp.base_address});
                }
                break;
            }
            case Shader::BufferType::Flatbuf:
                size = std::max<u64>(4, info.flattened_ud_buf.size() * sizeof(u32));
                offset = uploads.Host(batch, info.flattened_ud_buf.data(),
                                      info.flattened_ud_buf.size() * sizeof(u32));
                break;
            case Shader::BufferType::ClipPlanes: {
                // Read only when the vertex stage lowers user clip planes (their distances
                // aren't a WebGPU output yet); otherwise any 6 x vec4 will do.
                static const float none[24]{};
                const auto planes = clip_planes.empty() ? std::span<const float>{none} : clip_planes;
                size = planes.size_bytes();
                offset = uploads.Host(batch, planes.data(), size);
                break;
            }
            case Shader::BufferType::GdsBuffer:
                size = gds.size_bytes();
                offset = uploads.Host(batch, gds.data(), size);
                if (b.writable) {
                    bindings.writebacks.push_back({offset, size, reinterpret_cast<VAddr>(gds.data())});
                }
                break;
            default:
                Gpu::LogOnce(true, fmt::format("WebGPU: shader {:#x} uses buffer type {}",
                                             info.pgm_hash, u32(desc.buffer_type)));
                return false;
            }
            if (size == 0) {
                size = sizeof(null_buffer);
                offset = uploads.Host(batch, null_buffer, size);
            }
            bindings.entries.push_back({{"b", b.binding}, {"buf", {offset, size}}});
            break;
        }
        case WGSL::Binding::Kind::Texture: {
            layout["kind"] = "texture";
            const auto& desc = info.images[b.resource];
            Gpu::Json entry = surfaces.Texture(batch, desc.GetSharp(info), desc.is_array,
                                               b.wgsl_type);
            entry["b"] = b.binding;
            bindings.entries.push_back(std::move(entry));
            break;
        }
        case WGSL::Binding::Kind::StorageTexture: {
            layout["kind"] = "storageTexture";
            const auto& desc = info.images[b.resource];
            // texture_storage_2d<FORMAT, access>
            const auto& type = b.wgsl_type;
            const size_t open = type.find('<');
            const std::string format = type.substr(open + 1, type.find(',') - open - 1);
            Gpu::Json entry = surfaces.StorageImage(batch, desc.GetSharp(info), desc.is_array,
                                                    type, format);
            if (entry.empty()) {
                return false;
            }
            entry["b"] = b.binding;
            bindings.entries.push_back(std::move(entry));
            break;
        }
        case WGSL::Binding::Kind::Sampler: {
            layout["kind"] = "sampler";
            layout["comparison"] = b.comparison;
            const auto& desc = info.samplers[b.resource];
            bindings.entries.push_back(
                {{"b", b.binding}, {"samp", SamplerDesc(desc.GetSharp(info), b.comparison)}});
            break;
        }
        }
        bindings.layout.push_back(std::move(layout));
    }
    return true;
}

} // namespace Web
