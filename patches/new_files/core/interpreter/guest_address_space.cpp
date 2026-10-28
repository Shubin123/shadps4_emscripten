// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "core/interpreter/guest_address_space.h"

namespace Core::Interpreter {

void GuestAddressSpace::Map(std::uint64_t virtual_addr, std::uint64_t size) {
    auto storage = std::make_unique<std::uint8_t[]>(size);
    std::memset(storage.get(), 0, size);
    regions.insert_or_assign(virtual_addr, Region{virtual_addr, size, std::move(storage)});
}

std::uint64_t GuestAddressSpace::MapAnywhere(std::uint64_t size) {
    const std::uint64_t base = next_auto_base;
    // Round the next base up to a 64KB boundary and leave it strictly past this region, mirroring
    // a real allocator leaving some slack rather than packing regions edge-to-edge.
    next_auto_base = (base + size + 0xFFFFull) & ~0xFFFFull;
    Map(base, size);
    return base;
}

void GuestAddressSpace::Unmap(std::uint64_t virtual_addr) {
    regions.erase(virtual_addr);
}

std::uint8_t* GuestAddressSpace::Translate(VAddr vaddr, std::size_t size, bool /*for_write*/) {
    if (regions.empty()) {
        return nullptr;
    }
    auto it = regions.upper_bound(vaddr);
    if (it == regions.begin()) {
        return nullptr; // no region starts at or before vaddr
    }
    --it;
    const Region& region = it->second;
    if (vaddr < region.base || vaddr - region.base > region.size ||
        size > region.size - (vaddr - region.base)) {
        return nullptr; // before this region, or [vaddr, vaddr+size) runs past its end
    }
    return region.storage.get() + (vaddr - region.base);
}

} // namespace Core::Interpreter
