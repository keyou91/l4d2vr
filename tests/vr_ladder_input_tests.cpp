#include "../L4D2VR/vr_ladder_input.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(v) do { if (!(v)) std::abort(); } while (false)
int main()
{
    using l4d2vr_ladder::DirectionButtons;
    constexpr int other = (1 << 0) | (1 << 1) | (1 << 5) | (1 << 13);
    CHECK(DirectionButtons(other, 200.0f, 0.0f) == (other | (1 << 3)));
    CHECK(DirectionButtons(other | (1 << 3), -200.0f, 0.0f) == (other | (1 << 4)));
    CHECK(DirectionButtons(other, 0.0f, -80.0f) == (other | (1 << 9)));
    CHECK(DirectionButtons(other, 0.0f, 80.0f) == (other | (1 << 10)));
    CHECK(DirectionButtons(other | (1 << 3) | (1 << 10), 0.0f, 0.0f) == other);
    CHECK(DirectionButtons(other, 200.0f, -80.0f) == (other | (1 << 3) | (1 << 9)));
    CHECK(DirectionButtons(other, std::numeric_limits<float>::quiet_NaN(), 0.0f) == other);
    CHECK(DirectionButtons(other, 0.0f, std::numeric_limits<float>::infinity()) == other);
    std::puts("Ladder analog direction intent checks passed");
}
