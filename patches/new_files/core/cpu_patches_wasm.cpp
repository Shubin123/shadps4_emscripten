// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wasm version of core/cpu_patches.cpp. Natively, shadPS4 rewrites guest instructions the host
// CPU can't run as-is (fs-relative TLS accesses on macOS/Windows, some Jaguar-only extensions)
// into jumps to Xbyak-generated replacements, and protects the Windows red zone. The interpreter
// implements those instructions directly (fs_base is just a register to it), so there is nothing
// to patch.

#include "core/cpu_patches.h"

namespace Core::WindowsGuestRedZoneProtection {

void SetActiveMode(WindowsGuestRedZoneProtectionMode) noexcept {}

WindowsGuestRedZoneProtectionMode GetActiveMode() noexcept {
    return WindowsGuestRedZoneProtectionMode::Disabled;
}

bool IsStaticPatchingEnabled() noexcept {
    return false;
}

} // namespace Core::WindowsGuestRedZoneProtection

namespace Core {

void RegisterPatchModule(void*, u64, void*, u64) {}

void PrePatchInstructions(u64, u64) {}

RedZonePatchResult PatchRedZoneMemoryInstructions(u64, u64, std::span<const uintptr_t>) {
    return {};
}

RedZonePatchResult PatchCpuInstructionsStatically(u64, u64, std::span<const uintptr_t>) {
    return {};
}

} // namespace Core
