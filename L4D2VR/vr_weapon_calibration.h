#pragma once

#include "sdk/vector.h"
#include <array>
#include <cmath>
#include <string_view>

namespace l4d2vr_calibration
{
    inline constexpr std::array<std::string_view, 8> kConfigKeys{
        "WeaponAimYawOffsetDeg", "WeaponAimRollOffsetDeg",
        "ViewmodelPoseOffset", "ViewmodelPoseRotationOffsetDeg",
        "BulletAimOffset", "BulletAimRotationPitchDeg",
        "BulletAimRotationYawDeg", "BulletAimRotationRollDeg"
    };

    inline bool IsCalibrationKey(std::string_view key)
    {
        for (const auto candidate : kConfigKeys)
            if (candidate == key)
                return true;
        return false;
    }

    inline bool ShouldApplySampleValue(std::string_view key, bool alreadySaved)
    {
        return !alreadySaved || !IsCalibrationKey(key);
    }

    inline float ClampFinite(float value, float minimum, float maximum)
    {
        if (!std::isfinite(value))
            return 0.0f;
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    inline bool Finite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    inline bool HasOffset(const Vector& position, const Vector& rotation)
    {
        return position.x != 0.0f || position.y != 0.0f || position.z != 0.0f ||
            rotation.x != 0.0f || rotation.y != 0.0f || rotation.z != 0.0f;
    }

    // Values are the native L4D2 WeaponID values. Items, melee and infected
    // attacks must not receive firearm calibration. Mounted guns use their
    // own aim path even though their weapon ID is classified as a firearm.
    inline constexpr bool IsFirearm(int weaponId)
    {
        return (weaponId >= 1 && weaponId <= 11) || weaponId == 21 ||
            weaponId == 26 || (weaponId >= 32 && weaponId <= 37) || weaponId == 45;
    }

    inline constexpr bool ShouldCalibrateRay(bool firearm, bool mouseMode, bool scopeActive)
    {
        return firearm && !mouseMode && !scopeActive;
    }

    inline constexpr bool ShouldEncodeRay(bool attack, bool firearm, bool mounted,
        bool mouseMode, bool scopeActive, bool objectPull, bool preservedShotPose)
    {
        return attack && ShouldCalibrateRay(firearm, mouseMode, scopeActive) &&
            !mounted && !objectPull && !preservedShotPose;
    }

    inline bool Normalize(Vector& value)
    {
        const float lengthSquared = DotProduct(value, value);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.00000001f)
            return false;
        value *= 1.0f / std::sqrt(lengthSquared);
        return Finite(value);
    }

    inline bool ApplyRayOffset(Vector& origin, Vector& direction, const Vector& referenceUp,
        const Vector& positionOffset, const Vector& rotationOffset)
    {
        if (!Finite(origin) || !Finite(direction) || !Finite(referenceUp) ||
            !Finite(positionOffset) || !Finite(rotationOffset))
            return false;

        Vector forward = direction;
        if (!Normalize(forward))
            return false;

        // Zero calibration preserves the original ray exactly.
        if (!HasOffset(positionOffset, rotationOffset))
            return true;

        Vector up = referenceUp;
        Vector right{};
        CrossProduct(forward, up, right);
        if (!Normalize(right))
        {
            up = std::fabs(forward.z) < 0.99f
                ? Vector(0.0f, 0.0f, 1.0f) : Vector(0.0f, 1.0f, 0.0f);
            CrossProduct(forward, up, right);
            if (!Normalize(right))
                return false;
        }
        CrossProduct(right, forward, up);
        if (!Normalize(up))
            return false;

        if (rotationOffset.y != 0.0f)
        {
            forward = VectorRotate(forward, up, rotationOffset.y);
            right = VectorRotate(right, up, rotationOffset.y);
        }
        if (rotationOffset.x != 0.0f)
        {
            forward = VectorRotate(forward, right, rotationOffset.x);
            up = VectorRotate(up, right, rotationOffset.x);
        }
        if (rotationOffset.z != 0.0f)
        {
            right = VectorRotate(right, forward, rotationOffset.z);
            up = VectorRotate(up, forward, rotationOffset.z);
        }

        const Vector calibratedOrigin = origin + forward * positionOffset.x +
            right * positionOffset.y + up * positionOffset.z;
        if (!Finite(calibratedOrigin) || !Normalize(forward))
            return false;

        // Invalid input leaves the caller's ray untouched.
        origin = calibratedOrigin;
        direction = forward;
        return true;
    }
}
