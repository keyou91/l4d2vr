#include "../L4D2VR/vr_recv_interface.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Check(bool value)
{
    if (!value) { std::puts("Client receive interface check failed"); std::abort(); }
}
int main()
{
    using namespace l4d2vr_recv;
    // Getter bytes from the installed game's client.dll RVA 0xa8a40.
    const unsigned char getter[] = { 0xa1, 0xd4, 0xa4, 0x73, 0x10, 0xc3 };
    std::uintptr_t address = 0u;
    Check(DecodeClassListAddress(getter, sizeof(getter), 0x10000000u, 0x963000u, address));
    Check(address == 0x1073a4d4u);
    const unsigned char relocated[] = { 0xa1, 0xd4, 0xa4, 0xeb, 0x5b, 0xc3 };
    Check(DecodeClassListAddress(relocated, sizeof(relocated), 0x5b780000u, 0x963000u, address));
    Check(address == 0x5beba4d4u);
    // The previously selected HUD method starts with a frame setup, not a getter.
    const unsigned char hud[] = { 0x55, 0x8b, 0xec, 0xe8, 0xf8, 0xf7 };
    Check(!DecodeClassListAddress(hud, sizeof(hud), 0x10000000u, 0x963000u, address));
    Check(address == 0u);
    Check(!DecodeClassListAddress(nullptr, 6u, 0x10000000u, 0x963000u, address));
    Check(!DecodeClassListAddress(getter, 5u, 0x10000000u, 0x963000u, address));
    Check(!DecodeClassListAddress(getter, 6u, 0x10000000u, 0x73a4d7u, address));
    Check(!DecodeClassListAddress(getter, 6u, 0x20000000u, 0x963000u, address));
    Check(!Contains(0u, 16u, 4u, 4u));
    Check(!Contains(100u, 16u, 99u, 4u));
    Check(Contains(100u, 16u, 112u, 4u));
    Check(!Contains(100u, 16u, 113u, 4u));
    Check(!Contains(100u, 16u, 117u, 0u));
    Check(!Contains(std::numeric_limits<std::uintptr_t>::max() - 8u, 16u,
        std::numeric_limits<std::uintptr_t>::max() - 4u, 4u));
    std::puts("Client receive interface checks passed");
}
