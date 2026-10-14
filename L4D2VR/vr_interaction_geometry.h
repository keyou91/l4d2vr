#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace l4d2vr_interaction
{
    inline bool BoneInBranch(const std::vector<int>& parents, int bone, int root)
    {
        if (parents.size() > 512u) return false;
        const int count = static_cast<int>(parents.size());
        if (root < 0 || root >= count) return false;
        for (int guard = 0; guard < count && bone >= 0 && bone < count; ++guard)
        {
            if (bone == root) return true;
            bone = parents[static_cast<std::size_t>(bone)];
        }
        return false;
    }

    inline bool PumpMovesBone(const std::vector<int>& parents, int bone, int pump, int heldWrist)
    {
        // The support wrist is usually a sibling of the weapon, not its child.
        // The union also avoids translating a nested wrist twice.
        return BoneInBranch(parents, bone, pump) || BoneInBranch(parents, bone, heldWrist);
    }

    inline bool Finite(const Vector& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    inline float WrapYaw(float value)
    {
        return value - 360.0f * std::floor((value + 180.0f) / 360.0f);
    }

    inline float BodyYaw(const Vector& worldHeadForward, float previous, float turnDelta, bool valid)
    {
        const float turned = WrapYaw(previous + turnDelta);
        // Looking straight down must not invalidate the waist reference.
        if (worldHeadForward.x * worldHeadForward.x + worldHeadForward.y * worldHeadForward.y < 0.000001f)
            return turned;
        const float head = std::atan2(worldHeadForward.y, worldHeadForward.x) * 180.0f / 3.14159265358979323846f;
        return !valid || std::fabs(WrapYaw(head - turned)) > 65.0f ? head : turned;
    }

    inline Vector BodyOrigin(const Vector& head, const Vector& forward, const Vector& right,
        const Vector& offsetMeters, float scale)
    {
        return head + forward * (offsetMeters.x * scale) + right * (offsetMeters.y * scale) +
            Vector(0.0f, 0.0f, offsetMeters.z * scale);
    }

    inline bool LocalHandPosition(const Vector& hand, const Vector& weapon,
        const Vector& weaponForward, const Vector& referenceUp, Vector& result)
    {
        if (!Finite(hand) || !Finite(weapon) || !Finite(weaponForward) || !Finite(referenceUp))
            return false;
        Vector forward = weaponForward;
        const float length = forward.Length();
        if (!std::isfinite(length) || length < 0.0001f)
            return false;
        forward *= 1.0f / length;
        Vector right{};
        CrossProduct(forward, referenceUp, right);
        const float rightLength = right.Length();
        if (!std::isfinite(rightLength) || rightLength < 0.0001f)
            return false;
        right *= 1.0f / rightLength;
        Vector up{};
        CrossProduct(right, forward, up);
        const Vector delta = hand - weapon;
        result = Vector(DotProduct(delta, forward), DotProduct(delta, right), DotProduct(delta, up));
        return Finite(result);
    }

    inline bool ShellTouchesPort(const Vector& shell, const Vector& port, float radius)
    {
        return Finite(shell) && Finite(port) && std::isfinite(radius) && radius > 0.0f &&
            (shell - port).LengthSqr() <= radius * radius;
    }
    inline bool ValidBoxBounds(const Vector& mins, const Vector& maxs)
    {
        return Finite(mins) && Finite(maxs) && mins.x <= maxs.x && mins.y <= maxs.y && mins.z <= maxs.z;
    }
    inline bool ValidBox(const Vector& origin, const Vector& axisX, const Vector& axisY,
        const Vector& axisZ, const Vector& mins, const Vector& maxs)
    {
        if (!Finite(origin) || !Finite(axisX) || !Finite(axisY) || !Finite(axisZ) || !ValidBoxBounds(mins, maxs))
            return false;
        const float xLength = axisX.Length(), yLength = axisY.Length(), zLength = axisZ.Length();
        if (!std::isfinite(xLength) || !std::isfinite(yLength) || !std::isfinite(zLength) ||
            xLength < 0.0001f || yLength < 0.0001f || zLength < 0.0001f)
            return false;
        Vector cross{};
        CrossProduct(axisX * (1.0f / xLength), axisY * (1.0f / yLength), cross);
        return std::fabs(DotProduct(cross, axisZ * (1.0f / zLength))) > 0.001f;
    }
    inline float PointBoxDistance(const Vector& point, const Vector& mins, const Vector& maxs)
    {
        if (!Finite(point) || !ValidBoxBounds(mins, maxs))
            return INFINITY;
        const Vector closest(std::clamp(point.x, mins.x, maxs.x), std::clamp(point.y, mins.y, maxs.y),
            std::clamp(point.z, mins.z, maxs.z));
        return (point - closest).Length();
    }
    inline float OrientedBoxDistance(const Vector& point, const Vector& origin, const Vector& axisX,
        const Vector& axisY, const Vector& axisZ, const Vector& mins, const Vector& maxs)
    {
        if (!Finite(point) || !ValidBox(origin, axisX, axisY, axisZ, mins, maxs)) return INFINITY;
        const Vector delta = point - origin;
        return PointBoxDistance(Vector(DotProduct(delta, axisX), DotProduct(delta, axisY), DotProduct(delta, axisZ)),
            mins, maxs);
    }
}
