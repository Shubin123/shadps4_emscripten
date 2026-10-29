// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <string>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/interpreter/cpu.h"
#include "core/paleblood/paleblood.h"

// Paleblood's runtime/guest.h: where the executable's p_vaddr 0 is mapped.
extern "C" {
unsigned char* rt_image = nullptr;
}

namespace Core::Paleblood {

namespace {

constexpr u32 kEbootBase = 0x400000;
// jmp qword ptr [rip+0] followed by the 8-byte target: reaches any address and leaves every
// argument register alone.
constexpr u32 kJumpSize = 14;

bool Skipped(const Hook& hook) {
    const char* env = std::getenv("BB_HOOK_SKIP");
    if (env == nullptr || *env == '\0') {
        return false;
    }
    const std::string addr = fmt::format("{:#010x}", hook.address);
    std::string_view list{env};
    while (!list.empty()) {
        const size_t comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        if (item == addr || item == hook.name) {
            return true;
        }
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return false;
}

} // namespace

u32 InstallHooks(u8* image, u64 size, std::span<const Hook> hooks) {
    rt_image = image;
    u32 installed = 0;
    for (const Hook& hook : hooks) {
        if (Skipped(hook)) {
            LOG_INFO(Loader, "Paleblood: {} ({:#010x}) skipped (BB_HOOK_SKIP)", hook.name,
                     hook.address);
            continue;
        }
        const u64 offset = static_cast<u64>(hook.address) - kEbootBase;
        if (hook.address < kEbootBase || offset + hook.size > size || hook.size < kJumpSize) {
            LOG_ERROR(Loader, "Paleblood: can't hook {} at {:#010x} ({} bytes)", hook.name,
                      hook.address, hook.size);
            continue;
        }
        u8 jump[kJumpSize] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        const u64 target = hook.thunk();
        std::memcpy(jump + 6, &target, sizeof(target));
        // The interpreter's decode cache checks an instruction's bytes before reusing it, so
        // nothing stale of the original survives the patch.
        std::memcpy(image + offset, jump, sizeof(jump));
        installed++;
    }
    return installed;
}

void OnGameLoaded(VAddr eboot_base, u64 eboot_size, std::string_view serial,
                  std::string_view app_ver) {
    const auto hooks = GameHooks();
    if (hooks.empty() || serial != kTargetSerial) {
        return;
    }
    if (const char* env = std::getenv("SHADPS4_PALEBLOOD"); env && std::string_view{env} == "0") {
        LOG_INFO(Loader, "Paleblood: replacements turned off (SHADPS4_PALEBLOOD=0)");
        return;
    }
    if (app_ver != kTargetAppVer) {
        // The replacements are bound to 1.09's addresses; any other version's code is elsewhere.
        LOG_WARNING(Loader, "Paleblood: {} version {} isn't {}, the version its replacements are "
                            "written for; not installing them",
                    serial, app_ver, kTargetAppVer);
        return;
    }
    const u32 installed = InstallHooks(reinterpret_cast<u8*>(eboot_base), eboot_size, hooks);
    LOG_INFO(Loader, "Paleblood: {} of {} replacement(s) installed", installed, hooks.size());
}

} // namespace Core::Paleblood

unsigned char* paleblood_guest_thread_pointer() {
    const u64 fs_base = Core::Interpreter::Cpu::Current().State().fs_base;
    unsigned char* tp;
    std::memcpy(&tp, reinterpret_cast<const void*>(fs_base), sizeof(tp));
    return tp;
}
