#pragma once

#include "vr_input_recovery.h"
#include <cstdint>

namespace l4d2vr_hand_pose
{
    inline bool Available(std::uint64_t currentFrame, std::uint64_t sampledFrame,
        std::uint32_t updatedAt, std::uint32_t now)
    {
        return l4d2vr_input::ActionFrame::Ready(currentFrame) &&
            l4d2vr_input::ActionFrame::Ready(sampledFrame) &&
            l4d2vr_input::ActionFrame::FailureGeneration(currentFrame) ==
                l4d2vr_input::ActionFrame::FailureGeneration(sampledFrame) &&
            now - updatedAt <= 150u;
    }
}
