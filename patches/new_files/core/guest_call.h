// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Core::ExecuteGuest(fn, args...) calls a guest function pointer from host code. Natively that's
// an ordinary call (guest code is x86 running on the host CPU); in the wasm build guest code is
// interpreted, so the call goes through the interpreter instead (core/interpreter/guest.h). Every
// place shadPS4 calls into guest code - module entry points, thread start routines, destructors,
// callbacks - goes through here in the wasm build.

#pragma once

#include <cstring>
#include <type_traits>
#include "common/arch.h"
#include "common/types.h"
#ifdef ARCH_WASM
#include "core/interpreter/guest.h"
#endif

namespace Core {

template <typename R, typename... P, typename... A>
R ExecuteGuest(R (*func)(P...), A&&... args) {
#ifdef ARCH_WASM
    u64 addr;
    std::memcpy(&addr, &func, sizeof(addr));
    if (Interpreter::IsHostFunction(addr)) {
        return func(static_cast<P>(args)...);
    }
    return Interpreter::CallGuest<R>(addr, static_cast<P>(args)...);
#else
    return func(static_cast<P>(args)...);
#endif
}

} // namespace Core
