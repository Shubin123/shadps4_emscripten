// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Memory abstraction the interpreter reads/writes guest state through.
//
// The interpreter never dereferences a guest virtual address as a host pointer. On real
// hardware (and in shadPS4's existing direct-execution backend) guest VAs and host pointers
// are the same number because Core::AddressSpace reserves a real 1TB region of the host's
// virtual address space and mmaps guest pages into it. WebAssembly linear memory has no
// equivalent to a cheap sparse mmap reservation at an arbitrary 40-bit address, so that
// identity mapping cannot exist in the Emscripten build. Every access goes through Translate()
// instead, which is free to back guest pages with whatever storage is actually available
// (today: a flat buffer for tests; eventually: an on-demand-committed page table that mirrors
// AddressSpace::BackingPageTable). See PROJECT.md, "Guest virtual memory".

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Core::Interpreter {

using VAddr = std::uint64_t;

class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    // Returns a host pointer valid for `size` bytes at `vaddr`, or nullptr if the range is not
    // mapped with the requested permission. The pointer is only valid until the next call that
    // could move the backing storage (e.g. a growth-triggering allocation); the interpreter does
    // not hold it across instructions.
    virtual std::uint8_t* Translate(VAddr vaddr, std::size_t size, bool for_write) = 0;

    std::uint64_t Read(VAddr vaddr, std::size_t size) {
        const std::uint8_t* p = Translate(vaddr, size, /*for_write=*/false);
        FaultIfNull(p, vaddr, size);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; i++) {
            value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        }
        return value;
    }

    void Write(VAddr vaddr, std::uint64_t value, std::size_t size) {
        std::uint8_t* p = Translate(vaddr, size, /*for_write=*/true);
        FaultIfNull(p, vaddr, size);
        for (std::size_t i = 0; i < size; i++) {
            p[i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }

    void ReadBytes(VAddr vaddr, void* out, std::size_t size) {
        const std::uint8_t* p = Translate(vaddr, size, /*for_write=*/false);
        FaultIfNull(p, vaddr, size);
        std::memcpy(out, p, size);
    }

    void WriteBytes(VAddr vaddr, const void* in, std::size_t size) {
        std::uint8_t* p = Translate(vaddr, size, /*for_write=*/true);
        FaultIfNull(p, vaddr, size);
        std::memcpy(p, in, size);
    }

private:
    // A null Translate() result means "not mapped, or this request doesn't fit in one mapped
    // region" (see GuestAddressSpace's documented limitation). Without this check, every one of
    // the methods above would silently dereference a null pointer and segfault the host process
    // instead of failing with a diagnostic - which is exactly what happened to the interpreter's
    // instruction fetch path before Step() added its own explicit check (see x86_interpreter.cpp,
    // Fatal() call in Step()). Centralizing the check here means every other guest memory access
    // (ReadOperand/WriteOperand/Push/Pop/...) gets the same protection without each needing its
    // own copy of this check.
    static void FaultIfNull(const void* p, VAddr vaddr, std::size_t size) {
        if (p == nullptr) {
            std::fprintf(stderr,
                          "Core::Interpreter: guest memory fault: %zu-byte access at %#llx is "
                          "not mapped (or straddles two separately-mapped regions)\n",
                          size, static_cast<unsigned long long>(vaddr));
            std::abort();
        }
    }
};

} // namespace Core::Interpreter
