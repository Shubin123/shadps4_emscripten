// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/interpreter/host_thunk.h"

namespace Core::Interpreter {

std::uint64_t HostThunkTable::Register(HostThunk fn) {
    const std::uint64_t address = next;
    next += 16;
    thunks.emplace(address, std::move(fn));
    return address;
}

bool HostThunkTable::TryInvoke(std::uint64_t address, X86Interpreter& cpu) const {
    const auto it = thunks.find(address);
    if (it == thunks.end()) {
        return false;
    }
    it->second(cpu);
    return true;
}

} // namespace Core::Interpreter
