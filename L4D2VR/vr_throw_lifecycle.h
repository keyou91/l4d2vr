#pragma once
#include <cstdint>

namespace l4d2vr_throw
{
    struct EntityIdentity
    {
        std::uintptr_t object = 0u, edict = 0u;
        int index = -1;
        unsigned serial = 0u;
        bool Valid() const { return object && edict && index > 0 && index <= 4095 && serial <= 0xfffffu; }
    };
    inline bool SameEntity(const EntityIdentity& expected, const EntityIdentity& current)
    {
        return expected.Valid() && current.Valid() && expected.object == current.object &&
            expected.edict == current.edict && expected.index == current.index && expected.serial == current.serial;
    }
    inline bool ProjectilePending(bool inventoryDrop, bool projectileWeapon)
    { return !inventoryDrop && projectileWeapon; }
    inline bool DetachedPending(bool inventoryDrop, bool dropExecuted, bool carryWeapon)
    { return inventoryDrop ? dropExecuted : carryWeapon; }
}
