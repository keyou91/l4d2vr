#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_weapon_calibration.h"
#include "../L4D2VR/vr_weapon_models.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Check(bool result, const char* message)
{
    if (!result)
    {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static bool Near(const Vector& value, const Vector& expected)
{
    return (value - expected).LengthSqr() < 0.000001f;
}

int main()
{
    Check(l4d2vr_weapon_models::StockWeaponId("models/v_models/v_rifle_ak47.mdl") == 26,
        "AK contacts must not be accepted for a different stock gun");
    Check(l4d2vr_weapon_models::StockWeaponId("models/v_models/v_desert_eagle.mdl") == 32,
        "Magnum contacts must retain their inventory identity");
    Check(l4d2vr_weapon_models::StockWeaponId("models/v_models/v_dual_pistola.mdl") == 1,
        "A dual viewmodel must retain the pistol inventory type");
    Check(l4d2vr_weapon_models::StockWeaponId("models/v_models/custom.mdl") == 0,
        "Unknown models must not borrow a stock gun identity");
    using namespace l4d2vr_calibration;
    const Vector zero(0.0f, 0.0f, 0.0f);
    const Vector up(0.0f, 0.0f, 1.0f);
    Vector origin(10.0f, 20.0f, 30.0f), direction(2.0f, 0.0f, 0.0f);
    Check(ApplyRayOffset(origin, direction, up, zero, zero) &&
        Near(origin, Vector(10.0f, 20.0f, 30.0f)) && Near(direction, Vector(2.0f, 0.0f, 0.0f)),
        "Zero offsets must preserve the original ray, including its origin and scale");

    origin = zero; direction = Vector(1.0f, 0.0f, 0.0f);
    Check(ApplyRayOffset(origin, direction, up, Vector(2.0f, 3.0f, 4.0f), zero) &&
        Near(origin, Vector(2.0f, -3.0f, 4.0f)), "Source-local axes must map forward, right and up correctly");
    origin = zero; direction = Vector(1.0f, 0.0f, 0.0f);
    Check(ApplyRayOffset(origin, direction, up, Vector(2.0f, 0.0f, 0.0f), Vector(0.0f, 90.0f, 0.0f)) &&
        Near(direction, Vector(0.0f, 1.0f, 0.0f)) && Near(origin, Vector(0.0f, 2.0f, 0.0f)),
        "Yaw must rotate the shot ray and its local positional offset together");
    origin = zero; direction = Vector(1.0f, 0.0f, 0.0f);
    Check(ApplyRayOffset(origin, direction, up, zero, Vector(90.0f, 0.0f, 0.0f)) &&
        Near(direction, up), "Pitch must use the Source right axis");
    origin = zero; direction = Vector(1.0f, 0.0f, 0.0f);
    Check(ApplyRayOffset(origin, direction, up, Vector(0.0f, 1.0f, 0.0f), Vector(0.0f, 0.0f, 90.0f)) &&
        Near(origin, Vector(0.0f, 0.0f, -1.0f)) && Near(direction, Vector(1.0f, 0.0f, 0.0f)),
        "Roll must change the offset basis without bending the forward ray");
    origin = zero; direction = up;
    Check(ApplyRayOffset(origin, direction, up, Vector(1.0f, 0.0f, 0.0f), zero) && Near(origin, up),
        "A vertical ray must retain a valid basis when the reference up is parallel");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    origin = Vector(10.0f, 20.0f, 30.0f); direction = Vector(1.0f, 0.0f, 0.0f);
    Check(!ApplyRayOffset(origin, direction, up, Vector(nan, 0.0f, 0.0f), zero) &&
        Near(origin, Vector(10.0f, 20.0f, 30.0f)) && Near(direction, Vector(1.0f, 0.0f, 0.0f)),
        "Invalid calibration must fail without corrupting the original ray");
    Check(!ApplyRayOffset(origin, direction, up, zero, Vector(0.0f, inf, 0.0f)), "Infinite rotations must be rejected");
    Check(!ApplyRayOffset(origin, direction, Vector(nan, 0.0f, 0.0f), zero, zero), "Invalid tracked axes must be rejected");
    direction = zero;
    Check(!ApplyRayOffset(origin, direction, up, zero, zero), "A zero direction must not produce a shot ray");
    Check(ClampFinite(nan, -10.0f, 10.0f) == 0.0f && ClampFinite(inf, -10.0f, 10.0f) == 0.0f &&
        ClampFinite(100.0f, -10.0f, 10.0f) == 10.0f, "Config parsing must discard nonfinite values and bound valid values");

    Check(IsFirearm(1) && IsFirearm(21) && IsFirearm(32), "Pistol, grenade launcher and Magnum must remain firearms");
    for (const int item : {0, 12, 13, 14, 19, 20, 25, 38, 52})
        Check(!IsFirearm(item), "Items, throwables, melee and infected attacks must keep their native pose paths");
    Check(ShouldEncodeRay(true, true, false, false, false, false, false), "A calibrated firearm attack must reach the server");
    Check(!ShouldEncodeRay(false, true, false, false, false, false, false), "Idle commands must preserve hand tracking");
    Check(!ShouldEncodeRay(true, false, false, false, false, false, false), "A pipe bomb or melee attack must not use firearm calibration");
    Check(!ShouldEncodeRay(true, true, true, false, false, false, false), "Mounted weapons must retain native aim");
    Check(!ShouldEncodeRay(true, true, false, true, false, false, false), "Mouse aim must retain its eye-convergence path");
    Check(!ShouldEncodeRay(true, true, false, false, true, false, false), "Scope shots must retain their scope-camera ray");
    Check(!ShouldEncodeRay(true, true, false, false, false, true, false), "Object-pull commands must retain their interaction pose");
    Check(!ShouldEncodeRay(true, true, false, false, false, false, true), "A selected dual-pistol shot must not be replaced by right-hand aim");
    for (const auto key : kConfigKeys)
    {
        Check(!ShouldApplySampleValue(key, true), "Saved calibration must survive a forced sample default");
        Check(ShouldApplySampleValue(key, false), "New installs must receive calibration defaults");
    }
    Check(ShouldApplySampleValue("ControllerSmoothing", true), "Existing sample behavior must remain available for other settings");
}
