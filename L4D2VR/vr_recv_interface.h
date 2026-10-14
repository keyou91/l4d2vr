#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace l4d2vr_recv
{
    // Verified VClient016 slot in L4D2's Windows x86 client. The older
    // declaration with Connect/Disconnect selected a HUD input method instead.
    constexpr std::size_t ClassListSlot = 7u;

    inline bool Contains(std::uintptr_t base, std::size_t size,
        std::uintptr_t address, std::size_t length)
    {
        return base && size && size <= std::numeric_limits<std::uintptr_t>::max() - base &&
            address >= base && address - base <= size && length <= size - (address - base);
    }

    // This native getter is MOV EAX,[absolute address]; RET. Read its data
    // directly rather than invoking an unchecked virtual method during loading.
    inline bool DecodeClassListAddress(const unsigned char* code, std::size_t length,
        std::uintptr_t moduleBase, std::size_t moduleSize, std::uintptr_t& address)
    {
        address = 0u;
        if (!code || length < 6u || code[0] != 0xa1u || code[5] != 0xc3u) return false;
        const auto candidate = static_cast<std::uintptr_t>(code[1]) |
            (static_cast<std::uintptr_t>(code[2]) << 8u) |
            (static_cast<std::uintptr_t>(code[3]) << 16u) |
            (static_cast<std::uintptr_t>(code[4]) << 24u);
        if (!Contains(moduleBase, moduleSize, candidate, sizeof(std::uint32_t))) return false;
        address = candidate;
        return true;
    }
}
