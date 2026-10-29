// SPDX-License-Identifier: GPL-2.0-or-later
//
// Paleblood's runtime/original.h (third_party/paleblood/runtime/include/runtime/original.h) for
// the wasm build. The replacements reach the original executable through it by name:
//
//   RT_ORIGINAL(0x00123450, thing_update, void(Thing *, int32_t));
//   RT_GLOBAL(0x00456780, thing_manager_instance, ThingManager *);
//
// Paleblood's own version calls an original function by casting its address to a function
// pointer - the original is x86 code in the same process. Here guest code is interpreted, so a
// call goes through the interpreter instead (Core::Interpreter::CallGuest), nested on the calling
// guest thread's stack. Globals are plain memory either way.
//
// rt::guest_fn<Signature> is this port's addition: a function pointer stored in guest memory
// (a vtable slot), 8 bytes like the pointer it stands for, whose call also runs through the
// interpreter. tools/paleblood_port.py rewrites the replacements' vtable members to it at build
// time; natively they're ordinary function pointers.

#ifndef RUNTIME_ORIGINAL_H
#define RUNTIME_ORIGINAL_H

#include "runtime/guest.h"

#ifdef __cplusplus
#include <cstdint>
#include <type_traits>
#include "core/interpreter/guest.h"

namespace rt {

namespace detail {

template <typename R, typename... A>
R call_guest(uint64_t address, A... args) {
    return ::Core::Interpreter::CallGuest<R>(address, args...);
}

inline uint64_t guest_address(uint32_t ps4_address) {
    return reinterpret_cast<uint64_t>(rt_guest(ps4_address));
}

} // namespace detail

template <uint32_t Address, typename Signature>
struct original;

template <uint32_t Address, typename R, typename... P>
struct original<Address, R(P...)> {
    R operator()(P... p) const {
        return detail::call_guest<R>(detail::guest_address(Address), p...);
    }
    // The function's own address, for code that passes it on (a callback).
    void* address() const {
        return rt_guest(Address);
    }
};

template <uint32_t Address, typename R, typename... P>
struct original<Address, R(P..., ...)> {
    template <typename... V>
    R operator()(P... p, V... v) const {
        return detail::call_guest<R>(detail::guest_address(Address), p..., v...);
    }
    void* address() const {
        return rt_guest(Address);
    }
};

template <uint32_t Address, typename T>
struct global {
    T& get() const {
        return *static_cast<T*>(rt_guest(Address));
    }
    T* address() const {
        return static_cast<T*>(rt_guest(Address));
    }
};

template <typename Signature>
struct guest_fn;

template <typename R, typename... P>
struct guest_fn<R(P...)> {
    uint64_t address;

    R operator()(P... p) const {
        return detail::call_guest<R>(address, p...);
    }
};
static_assert(sizeof(guest_fn<void()>) == 8 && std::is_standard_layout_v<guest_fn<void()>>);

} // namespace rt

#define RT_ORIGINAL(address, name, signature)                                                     \
    inline constexpr ::rt::original<(address), signature> name {}
#define RT_GLOBAL(address, name, type) inline constexpr ::rt::global<(address), type> name {}

#endif

#endif
