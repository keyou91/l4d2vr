#pragma once
#include <cmath>

namespace l4d2vr_ladder
{
    // Source ladder movement consumes direction buttons as well as analog axes.
    inline int DirectionButtons(int buttons, float forward, float side)
    {
        if (!std::isfinite(forward) || !std::isfinite(side)) return buttons;
        constexpr int forwardBit = 1 << 3, backBit = 1 << 4;
        constexpr int leftBit = 1 << 9, rightBit = 1 << 10;
        buttons &= ~(forwardBit | backBit | leftBit | rightBit);
        if (forward > 0.01f) buttons |= forwardBit;
        else if (forward < -0.01f) buttons |= backBit;
        if (side > 0.01f) buttons |= rightBit;
        else if (side < -0.01f) buttons |= leftBit;
        return buttons;
    }
}
