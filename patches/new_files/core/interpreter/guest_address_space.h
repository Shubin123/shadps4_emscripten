// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include "core/interpreter/guest_memory.h"

namespace Core::Interpreter {

// Stands in for Core::AddressSpace (src/core/address_space.h) when targeting Emscripten.
//
// The real AddressSpace reserves a real 1TB (40-bit) region of the HOST's virtual address space
// via mmap and maps guest pages into it, so a guest VAddr and a host pointer are the same
// number (Traits::Entry = u8*, ADDRESS_SPACE_BITS = 40). That identity mapping is cheap on a
// real OS because an unmapped mmap reservation doesn't commit physical memory; WebAssembly
// linear memory has no equivalent (no sparse reservation at an arbitrary 64-bit address, and no
// realistic way to back a 1TB declared memory even under the memory64 proposal).
//
// GuestAddressSpace instead tracks an explicit list of mapped regions, each backed by its own
// ordinary heap allocation, and Translate() finds which region a VAddr falls in and adds that
// region's host-base offset.
//
// Known limitation, by design: Translate() hands back one contiguous host pointer, which only
// works for an access that stays within a single Map()'d region. A page-table-per-16KB-page
// scheme (mirroring AddressSpace::BackingPageTable exactly) was considered instead, but runs
// into the same problem one level down: pages are independently allocated and not contiguous
// with each other in host memory, so an access straddling a page boundary would need the same
// fallback this class doesn't have either. shadPS4's real HLE code almost always maps a whole
// segment/allocation in one Map() call and then accesses within it, so the common case is
// covered; an access that straddles the boundary between two separately-mapped regions returns
// nullptr (the interpreter treats that as a fault) rather than silently reading adjacent,
// unrelated memory. See PROJECT.md, "Guest virtual memory".
class GuestAddressSpace final : public GuestMemory {
public:
    GuestAddressSpace() = default;

    // Maps `size` zero-initialized bytes at `virtual_addr`. Mirrors the shape of
    // Core::AddressSpace::Map, minus the phys_addr/exec parameters: there is no physical-memory
    // or executable-protection distinction to model here, every mapping is just a heap
    // allocation the interpreter is free to read and write.
    void Map(std::uint64_t virtual_addr, std::uint64_t size);

    // Picks an address this GuestAddressSpace hasn't handed out yet and maps `size` bytes there.
    // Stands in for AddressSpace::Map's "virtual_addr == 0 picks an address" behavior.
    std::uint64_t MapAnywhere(std::uint64_t size);

    // Unmaps the region that starts at `virtual_addr` exactly (not a sub-range of it - this is
    // simpler than AddressSpace::Unmap's partial-unmap support because nothing exercises partial
    // unmapping yet; extend this if/when something needs it).
    void Unmap(std::uint64_t virtual_addr);

    std::uint8_t* Translate(VAddr vaddr, std::size_t size, bool for_write) override;

private:
    struct Region {
        std::uint64_t base;
        std::uint64_t size;
        std::unique_ptr<std::uint8_t[]> storage;
    };

    // Keyed by base address so Translate() can find the containing region with one
    // upper_bound() lookup. Region counts are expected to stay in the tens (one per ELF
    // segment, one per large allocation, ...), not large enough to need anything fancier.
    std::map<std::uint64_t, Region> regions;
    std::uint64_t next_auto_base = 0x10'0000'0000ull; // arbitrary; just out of low addresses a
                                                       // game's own fixed-address mappings
                                                       // might want.
};

} // namespace Core::Interpreter
