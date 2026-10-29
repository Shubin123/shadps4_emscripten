// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The hand-off from the emulator's presenter (any thread) to whatever is showing frames: in a
// browser, a <canvas> owned by the page's main thread; under Node (tests), optional PPM dumps.

#pragma once

#include "common/types.h"

namespace Web::Display {

// Copies `width*height` RGBA8 pixels and schedules them to be shown. Never blocks on the page.
void Submit(const u32* rgba, u32 width, u32 height, bool is_game_frame);

u64 FramesSubmitted();

} // namespace Web::Display
