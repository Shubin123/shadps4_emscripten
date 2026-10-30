// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the WebGPU renderer's draws and dispatches share: the batch being recorded (one, on the
// GPU command processor's thread), the guest buffer copies already in it, the surfaces and the
// translated shaders.

#pragma once

#include <map>
#include <utility>
#include <vector>
#include "video_core/renderer_web/web_gpu.h"
#include "video_core/renderer_web/web_resources.h"
#include "video_core/renderer_web/web_shaders.h"
#include "video_core/renderer_web/web_surfaces.h"

namespace Web {

// Render target metadata (CMASK/HTILE) known to have been cleared, as upstream's texture cache
// tracks it: a fast clear only writes the metadata, and the next draw into the target starts
// from the clear value.
class MetaClears {
public:
    // A target with metadata at `meta` was bound. A fill of it before that (the usual order: the
    // clear comes before the first draw) still counts.
    void Register(VAddr meta) {
        if (meta != 0) {
            const bool filled = std::erase(recent_fills, meta) != 0;
            cleared.try_emplace(meta, filled);
        }
    }
    // A shader wrote `address`; true if it's metadata (now cleared).
    bool Clear(VAddr address) {
        const auto it = cleared.find(address);
        if (it == cleared.end()) {
            return false;
        }
        it->second = true;
        return true;
    }
    // A DMA fill wrote `address`: metadata now, or maybe once a target using it is bound.
    void NoteFill(VAddr address) {
        if (!Clear(address)) {
            recent_fills.push_back(address);
            if (recent_fills.size() > 64) {
                recent_fills.erase(recent_fills.begin());
            }
        }
    }
    bool IsMeta(VAddr address) const {
        return cleared.contains(address);
    }
    // Whether the target behind `meta` starts cleared, and it doesn't any more after this.
    bool Consume(VAddr meta) {
        const auto it = cleared.find(meta);
        return it != cleared.end() && std::exchange(it->second, false);
    }

private:
    std::map<VAddr, bool> cleared;
    std::vector<VAddr> recent_fills;
};

struct Context {
    Gpu::Batch batch;
    Uploads uploads;
    Surfaces surfaces;
    ShaderCache shaders;
    MetaClears meta;

    // Hands the batch to the page. With `wait`, returns once it has run and its readbacks are
    // in memory.
    bool Submit(bool wait) {
        uploads.Reset();
        return batch.Submit(wait);
    }
};

} // namespace Web
