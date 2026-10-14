#pragma once

#include "openvr.h"
#include <cmath>
#include <cstdint>
#include <limits>

namespace l4d2vr_controller_identity
{
    struct Pair
    {
        vr::TrackedDeviceIndex_t left = vr::k_unTrackedDeviceIndexInvalid;
        vr::TrackedDeviceIndex_t right = vr::k_unTrackedDeviceIndexInvalid;
    };

    template<class System>
    bool IsConnectedController(System* system, vr::TrackedDeviceIndex_t index)
    {
        return system && index != vr::k_unTrackedDeviceIndex_Hmd &&
            index < vr::k_unMaxTrackedDeviceCount && system->IsTrackedDeviceConnected(index) &&
            system->GetTrackedDeviceClass(index) == vr::TrackedDeviceClass_Controller;
    }

    // Driver hints identify hands explicitly. Device order and pose position
    // cannot supply a missing or ambiguous hand identity.
    template<class System>
    Pair Resolve(System* system)
    {
        Pair result;
        if (!system)
            return result;
        const auto legacyLeft = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        const auto legacyRight = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (IsConnectedController(system, legacyLeft))
            result.left = legacyLeft;
        if (IsConnectedController(system, legacyRight))
            result.right = legacyRight;
        if (result.left != vr::k_unTrackedDeviceIndexInvalid && result.left == result.right)
            return {};
        if (result.left != vr::k_unTrackedDeviceIndexInvalid && result.right != vr::k_unTrackedDeviceIndexInvalid)
            return result;

        Pair hinted;
        uint32_t leftCount = 0, rightCount = 0;
        for (vr::TrackedDeviceIndex_t index = 1; index < vr::k_unMaxTrackedDeviceCount; ++index)
        {
            if (!IsConnectedController(system, index))
                continue;
            vr::ETrackedPropertyError error = vr::TrackedProp_Success;
            const int32_t hint = system->GetInt32TrackedDeviceProperty(index, vr::Prop_ControllerRoleHint_Int32, &error);
            if (error != vr::TrackedProp_Success)
                continue;
            if (hint == vr::TrackedControllerRole_LeftHand)
            {
                hinted.left = index;
                ++leftCount;
            }
            else if (hint == vr::TrackedControllerRole_RightHand)
            {
                hinted.right = index;
                ++rightCount;
            }
        }
        if (result.left == vr::k_unTrackedDeviceIndexInvalid && leftCount == 1 && hinted.left != result.right)
            result.left = hinted.left;
        if (result.right == vr::k_unTrackedDeviceIndexInvalid && rightCount == 1 && hinted.right != result.left)
            result.right = hinted.right;
        return result;
    }

    inline bool RigidRotation(const vr::HmdMatrix34_t& matrix)
    {
        // OpenVR tracking matrices contain a rotation, not scale or shear.
        // Allow small driver rounding errors without accepting collapsed axes.
        for (int column = 0; column < 3; ++column)
        {
            double lengthSquared = 0.0;
            for (int row = 0; row < 3; ++row)
            {
                const double value = matrix.m[row][column];
                if (!std::isfinite(value)) return false;
                lengthSquared += value * value;
            }
            if (std::fabs(lengthSquared - 1.0) > 0.02) return false;
            for (int other = 0; other < column; ++other)
            {
                double dot = 0.0;
                for (int row = 0; row < 3; ++row)
                    dot += static_cast<double>(matrix.m[row][column]) * matrix.m[row][other];
                if (!std::isfinite(dot) || std::fabs(dot) > 0.02) return false;
            }
        }
        const double determinant =
            static_cast<double>(matrix.m[0][0]) *
                (static_cast<double>(matrix.m[1][1]) * matrix.m[2][2] - static_cast<double>(matrix.m[1][2]) * matrix.m[2][1]) -
            static_cast<double>(matrix.m[0][1]) *
                (static_cast<double>(matrix.m[1][0]) * matrix.m[2][2] - static_cast<double>(matrix.m[1][2]) * matrix.m[2][0]) +
            static_cast<double>(matrix.m[0][2]) *
                (static_cast<double>(matrix.m[1][0]) * matrix.m[2][1] - static_cast<double>(matrix.m[1][1]) * matrix.m[2][0]);
        return std::isfinite(determinant) && std::fabs(determinant - 1.0) <= 0.05;
    }

    inline bool UsablePose(const vr::TrackedDevicePose_t& pose)
    {
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid)
            return false;
        for (const auto& row : pose.mDeviceToAbsoluteTracking.m)
            for (float value : row)
                if (!std::isfinite(value))
                    return false;
        if (!RigidRotation(pose.mDeviceToAbsoluteTracking)) return false;
        for (float value : pose.vVelocity.v)
            if (!std::isfinite(value))
                return false;
        for (float value : pose.vAngularVelocity.v)
            if (!std::isfinite(value) || std::fabs(value) >
                std::numeric_limits<float>::max() / (180.0 / 3.141592654))
                return false;
        // GetPoseData converts this element through asin.
        return pose.mDeviceToAbsoluteTracking.m[1][2] >= -1.0f &&
            pose.mDeviceToAbsoluteTracking.m[1][2] <= 1.0f;
    }

    inline Pair WithUsablePoses(Pair pair, const vr::TrackedDevicePose_t* poses, uint32_t count)
    {
        if (!poses || pair.left >= count || !UsablePose(poses[pair.left]))
            pair.left = vr::k_unTrackedDeviceIndexInvalid;
        if (!poses || pair.right >= count || !UsablePose(poses[pair.right]))
            pair.right = vr::k_unTrackedDeviceIndexInvalid;
        return pair;
    }

    inline uint64_t Pack(Pair pair)
    {
        return static_cast<uint64_t>(pair.left) | (static_cast<uint64_t>(pair.right) << 32);
    }

    inline Pair Unpack(uint64_t packed)
    {
        return { static_cast<vr::TrackedDeviceIndex_t>(packed & 0xffffffffu),
            static_cast<vr::TrackedDeviceIndex_t>(packed >> 32) };
    }

    template<class System>
    vr::TrackedDeviceIndex_t CachedIndex(System* system, uint64_t packed, bool leftHand)
    {
        const Pair pair = Unpack(packed);
        const auto index = leftHand ? pair.left : pair.right;
        if (pair.left == pair.right || !IsConnectedController(system, index))
            return vr::k_unTrackedDeviceIndexInvalid;
        return index;
    }
}
