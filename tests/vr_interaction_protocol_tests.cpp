#include "../L4D2VR/vr_interaction_protocol.h"
#include <cstdio>
#include <cstdlib>
static void Check(bool x) { if (!x) { std::fprintf(stderr,"Interaction packet arbitration failed\n"); std::abort(); } }
int main()
{
    using namespace l4d2vr_wire;
    Check(!SupportsPhysicalVersion(0u));
    Check(SupportsPhysicalVersion(1u));
    Check(!SupportsPhysicalVersion(2u));
    Check(!SupportsPhysicalVersion(255u));
    for (unsigned value=0;value<256;++value)
    {
        const auto impulse=static_cast<std::uint8_t>(value);
        const bool grip=value==234u || value==235u;
        const bool pull=value>=240u && value<=243u;
        Check(IsGripAction(impulse)==grip);
        Check(IsObjectPull(impulse)==pull);
        Check(!UseObjectPullPayload(false,impulse));
        Check(UseObjectPullPayload(true,impulse)==!grip);
        Check(KeepNativeReloadGate(impulse)==(grip||pull));
    }
    Check(!UseObjectPullPayload(true,l4d2vr_grip::kReleaseImpulse));
    Check(!UseObjectPullPayload(true,l4d2vr_grip::kPickupImpulse));
    Check(UseObjectPullPayload(true,l4d2vr_grip::kBlockNativeReloadImpulse));
}
