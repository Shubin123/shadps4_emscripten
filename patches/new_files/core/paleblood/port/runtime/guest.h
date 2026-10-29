// SPDX-License-Identifier: GPL-2.0-or-later
//
// Paleblood's runtime/guest.h (third_party/paleblood/runtime/include/runtime/guest.h) for the
// wasm build, where the vendored game replacements are compiled against it. Same interface:
// rt_image is where the executable's ELF p_vaddr 0 is mapped, and Paleblood's addresses are PS4
// virtual addresses with the executable at 0x400000. Guest addresses are linear-memory addresses
// here (core/wasm_layout.h), so a guest pointer can be dereferenced directly.
//
// Paleblood's rt::fn<Signature>(address) - a native function pointer to guest code - is left
// out: guest code is interpreted, so such a call can't work, and leaving it undefined turns any
// use into a compile error instead of a trap at run time.

#ifndef RUNTIME_GUEST_H
#define RUNTIME_GUEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_EBOOT_BASE 0x400000u

extern unsigned char* rt_image;

static inline void* rt_guest(uint32_t ps4_address) {
    return rt_image + (ps4_address - RT_EBOOT_BASE);
}

#ifdef __cplusplus
}

namespace rt {
template <typename T>
inline T* ptr(uint32_t ps4_address) {
    return static_cast<T*>(rt_guest(ps4_address));
}
} // namespace rt
#endif

#endif
