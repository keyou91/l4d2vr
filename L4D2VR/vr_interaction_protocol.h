#pragma once
#include "vr_grip_release.h"
#include <cstdint>

namespace l4d2vr_wire
{
    constexpr unsigned kPhysicalVersion = 1u;
    inline bool SupportsPhysicalVersion(unsigned version) { return version == kPhysicalVersion; }
    inline bool IsGripAction(std::uint8_t impulse)
    {
        return impulse == l4d2vr_grip::kPickupImpulse || impulse == l4d2vr_grip::kReleaseImpulse;
    }
    inline bool IsObjectPull(std::uint8_t impulse) { return impulse >= 240u && impulse <= 243u; }
    inline bool UseObjectPullPayload(bool available, std::uint8_t impulse)
    {
        // One command has one impulse and one controller pose. A grip contact
        // or release must never borrow the unrelated remote-pull pose.
        return available && !IsGripAction(impulse);
    }
    inline bool KeepNativeReloadGate(std::uint8_t impulse)
    {
        // A temporary action occupying the impulse does not grant a reload.
        // The next ordinary command carries the current reload policy again.
        return IsGripAction(impulse) || IsObjectPull(impulse);
    }
}
