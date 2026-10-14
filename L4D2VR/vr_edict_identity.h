#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace l4d2vr_edict
{
    // L4D2's edict stores a full serial at byte 4 and has no cached index.
    struct Layout32
    {
        std::uint32_t flags, serial, networkable, unknown;
    };
    static_assert(sizeof(Layout32) == 16u && offsetof(Layout32, serial) == 4u,
        "Unexpected L4D2 edict layout");

    inline int IndexFromArray(std::uintptr_t base, int count, std::uintptr_t entity)
    {
        if (!base || count <= 0 || count > 4096 || entity < base) return -1;
        const auto span = static_cast<std::uintptr_t>(count) * sizeof(Layout32);
        if (base > std::numeric_limits<std::uintptr_t>::max() - span) return -1;
        const auto delta = entity - base;
        if (delta >= span || delta % sizeof(Layout32) != 0u) return -1;
        return static_cast<int>(delta / sizeof(Layout32));
    }

    inline bool Identity(int nativeIndex, int nativeSerial, unsigned flags,
        int& index, unsigned& serial)
    {
        index = -1; serial = 0u;
        if (nativeIndex <= 0 || nativeIndex > 4095 || nativeSerial < 0 ||
            static_cast<unsigned>(nativeSerial) > 0xfffffu ||
            (flags & 2u) != 0u || (flags & 4u) == 0u) return false;
        index = nativeIndex; serial = static_cast<unsigned>(nativeSerial);
        return true;
    }
}
