// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wasm version of core/aerolib/stubs.cpp. Natively each unresolved import gets a tiny Xbyak
// trampoline that tags the call with its index and jumps to a shared "log and return 0"
// handler. Here each one gets an HLE thunk address instead, and the shared handler finds out
// which stub was called from the address the guest jumped to.

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/interpreter/hle.h"

namespace Core::AeroLib {

namespace {

std::mutex g_mutex;
std::deque<std::string> g_names; // stable storage for thunk names
std::unordered_map<std::string, u64> g_by_nid;
std::unordered_map<u64, u32> g_calls;

void CommonStub(Interpreter::Cpu& cpu) {
    const u64 addr = cpu.s.rip;
    u32 n;
    {
        std::scoped_lock lk{g_mutex};
        n = ++g_calls[addr];
    }
    // Unimplemented functions can be called every frame; log the first few calls of each.
    if (n <= 3) {
        LOG_ERROR(Core, "Stub: {} called{}, returning zero to {:#x}",
                  Interpreter::ThunkName(addr), n == 3 ? " (further calls not logged)" : "",
                  cpu.Load<u64>(cpu.s.gpr[Interpreter::RSP]));
    }
    cpu.s.gpr[Interpreter::RAX] = 0;
}

} // namespace

u64 GetStub(const char* nid) {
    std::scoped_lock lk{g_mutex};
    if (const auto it = g_by_nid.find(nid); it != g_by_nid.end()) {
        return it->second;
    }
    const auto* entry = FindByNid(nid);
    g_names.push_back(entry ? fmt::format("{} (nid: {})", entry->name, entry->nid)
                            : fmt::format("Unknown (nid: {})", nid));
    const u64 addr = Interpreter::RegisterThunk(&CommonStub, g_names.back().c_str());
    g_by_nid.emplace(nid, addr);
    return addr;
}

} // namespace Core::AeroLib
