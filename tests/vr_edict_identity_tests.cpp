#include "../L4D2VR/vr_edict_identity.h"
#include "../L4D2VR/vr_pistol_ammo.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool value)
{
    if (!value) { std::puts("Native edict identity check failed"); std::abort(); }
}
int main()
{
    // Actual survivor fields from the failing negotiation: native index 1,
    // serial 396 at byte 4. Reading a cached index there rejected this player.
    const l4d2vr_edict::Layout32 captured{132u, 396u, 0x409e002cu, 0x409e0010u};
    int index = -1; unsigned serial = 0u;
    Check(l4d2vr_edict::Identity(1, static_cast<int>(captured.serial), captured.flags, index, serial));
    Check(index == 1 && serial == 396u);
    Check(l4d2vr_edict::IndexFromArray(0x2c3b0010u, 2048, 0x2c3b0020u) == 1);
    Check(l4d2vr_edict::IndexFromArray(0x2c3b0010u, 2048, 0x2c3b0024u) == -1);
    Check(l4d2vr_edict::IndexFromArray(0x2c3b0010u, 2048, 0x2c3b0000u) == -1);
    Check(l4d2vr_edict::IndexFromArray(100u, 4, 164u) == -1);
    Check(l4d2vr_edict::IndexFromArray(0u, 4, 16u) == -1);
    Check(l4d2vr_edict::IndexFromArray(100u, 4097, 116u) == -1);
    Check(l4d2vr_edict::IndexFromArray(std::numeric_limits<std::uintptr_t>::max() - 8u, 4,
        std::numeric_limits<std::uintptr_t>::max() - 4u) == -1);
    Check(l4d2vr_edict::Identity(2048, 65536 + 396, 4u, index, serial));
    Check(index == 2048 && serial == 65932u);
    using namespace l4d2vr_pistol;
    AmmoSnapshot first{10u, 20u, 396u, 5u, 15, false, 15};
    AmmoSnapshot reused = first; reused.ownerSerial += 65536u;
    Check(!first.SameGun(reused));
    Check(!l4d2vr_edict::Identity(1, 396, 6u, index, serial));
    Check(index == -1 && serial == 0u);
    Check(!l4d2vr_edict::Identity(1, 396, 0u, index, serial));
    Check(!l4d2vr_edict::Identity(0, 396, 4u, index, serial));
    Check(!l4d2vr_edict::Identity(4096, 396, 4u, index, serial));
    Check(!l4d2vr_edict::Identity(1, -1, 4u, index, serial));
    Check(!l4d2vr_edict::Identity(1, 0x100000, 4u, index, serial));
    std::puts("Native edict identity checks passed");
}
