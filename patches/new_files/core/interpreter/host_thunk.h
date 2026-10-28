// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace Core::Interpreter {

class X86Interpreter;

// Lets interpreted guest code call back out to real, natively-compiled host functions - the PS4
// HLE library implementations under core/libraries/**, which shadPS4's linker currently resolves
// guest imports directly to (Linker::RelocateAllImports / aerolib). Under direct execution that
// "point the import at a host function pointer" just works, because guest and host code run on
// the same real CPU. Under interpretation it can't: landing on one of those addresses has to
// invoke the real host function instead of trying to decode its compiled machine code as if it
// were guest bytes, then splice the real return value back into the virtual register file. This
// is also how RunMainEntry's ProgramExitFunc (linker.cpp) and any other "hand a guest-visible
// address that is actually a host function pointer" site has to work once wired up for real -
// see PROJECT.md, "Guest<->host call boundary".
using HostThunk = std::function<void(X86Interpreter&)>;

class HostThunkTable {
public:
    // Registers `fn` and returns a synthetic address for it. shadPS4's guest user address space
    // is 40 bits wide (Core::AddressSpace::Traits::ADDRESS_SPACE_BITS), so addresses at or above
    // 2^48 can never collide with a real guest VA.
    std::uint64_t Register(HostThunk fn);

    // If `address` was returned by Register(), invokes the thunk and returns true.
    bool TryInvoke(std::uint64_t address, X86Interpreter& cpu) const;

private:
    static constexpr std::uint64_t kBase = 0xFFFF'0000'0000'0000ull;
    std::unordered_map<std::uint64_t, HostThunk> thunks;
    std::uint64_t next = kBase;
};

} // namespace Core::Interpreter
