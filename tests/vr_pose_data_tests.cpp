#include "../L4D2VR/vr_controller_identity.h"
#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "vr_pose_data_type.inl"

struct VR
{
    void GetPoseData(vr::TrackedDevicePose_t&, TrackedDevicePoseData&);
};

#include "vr_pose_data_function.inl"

static bool ShouldConvertQueuedHmd(
    const std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount>& renderPoses)
{
#include "vr_pose_data_render_condition.inl"
}

namespace
{
    int checks = 0;
    void Require(bool result, const char* message)
    {
        ++checks;
        if (!result)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }
    template<class V>
    bool Zero(const V& value) { return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }
    template<class V>
    bool Near(const V& a, const V& b)
    {
        return std::fabs(a.x-b.x) < 0.0001f && std::fabs(a.y-b.y) < 0.0001f && std::fabs(a.z-b.z) < 0.0001f;
    }
    vr::TrackedDevicePose_t Sample()
    {
        vr::TrackedDevicePose_t pose{};
        pose.bDeviceIsConnected = true;
        pose.bPoseIsValid = true;
        pose.mDeviceToAbsoluteTracking.m[0][0] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[1][1] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[2][2] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[0][3] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[1][3] = 2.0f;
        pose.mDeviceToAbsoluteTracking.m[2][3] = 3.0f;
        pose.vVelocity = {{4.0f,5.0f,6.0f}};
        pose.vAngularVelocity = {{7.0f,8.0f,9.0f}};
        return pose;
    }
    void CheckRejected(vr::TrackedDevicePose_t bad, const char* message)
    {
        VR vr;
        auto good = Sample();
        TrackedDevicePoseData output;
        vr.GetPoseData(good, output);
        const Vector originalPosition = output.TrackedDevicePos;
        const QAngle originalAngles = output.TrackedDeviceAng;
        std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses{};
        renderPoses[vr::k_unTrackedDeviceIndex_Hmd] = bad;
        Require(!ShouldConvertQueuedHmd(renderPoses), "Rejected queued samples keep the existing camera fallback");
        vr.GetPoseData(bad, output);
        Require(Near(output.TrackedDevicePos, originalPosition) && Near(output.TrackedDeviceAng, originalAngles), message);
        Require(Zero(output.TrackedDeviceVel) && Zero(output.TrackedDeviceAngVel),
            "Rejected tracking cannot retain stale throw or swing velocity");
        vr.GetPoseData(good, output);
        Require(Near(output.TrackedDeviceVel, Vector(-6.0f,-4.0f,5.0f)), "Usable tracking recovers immediately");
    }
}

int main()
{
    VR vr;
    TrackedDevicePoseData output;
    Require(Zero(output.TrackedDevicePos) && Zero(output.TrackedDeviceVel) &&
        Zero(output.TrackedDeviceAng) && Zero(output.TrackedDeviceAngVel),
        "Initial tracking fields are finite zero even with VECTOR_PARANOIA");
    vr::TrackedDevicePose_t missing{};
    vr.GetPoseData(missing, output);
    Require(Zero(output.TrackedDevicePos) && Zero(output.TrackedDeviceAng), "Absent startup tracking stays neutral");
    auto pose = Sample();
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses{};
    renderPoses[vr::k_unTrackedDeviceIndex_Hmd] = pose;
    Require(ShouldConvertQueuedHmd(renderPoses), "Usable queued tracking still converts the HMD pose");
    vr.GetPoseData(pose, output);
    Require(Near(output.TrackedDevicePos, Vector(-3.0f,-1.0f,2.0f)), "Usable pose keeps the native coordinate conversion");
    Require(Near(output.TrackedDeviceVel, Vector(-6.0f,-4.0f,5.0f)), "Usable velocity keeps the native coordinate conversion");
    Require(Zero(output.TrackedDeviceAng), "Identity orientation remains neutral");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
        {
            auto malformed = pose;
            malformed.mDeviceToAbsoluteTracking.m[row][column] = nan;
            CheckRejected(malformed, "A NaN tracking matrix cannot poison a retained pose");
            malformed.mDeviceToAbsoluteTracking.m[row][column] = inf;
            CheckRejected(malformed, "An infinite tracking matrix cannot poison a retained pose");
        }
    for (int axis = 0; axis < 3; ++axis)
    {
        auto malformed = pose;
        malformed.vVelocity.v[axis] = nan;
        CheckRejected(malformed, "A NaN linear velocity rejects the tracking sample");
        malformed = pose;
        malformed.vAngularVelocity.v[axis] = inf;
        CheckRejected(malformed, "An infinite angular velocity rejects the tracking sample");
        malformed = pose;
        malformed.vAngularVelocity.v[axis] = std::numeric_limits<float>::max();
        CheckRejected(malformed, "Degree conversion cannot overflow a finite angular velocity");
    }
    auto malformed = pose;
    malformed.mDeviceToAbsoluteTracking.m[1][2] = 1.001f;
    CheckRejected(malformed, "Out-of-domain orientation cannot produce NaN angles");
    malformed = pose; malformed.bDeviceIsConnected = false;
    CheckRejected(malformed, "A disconnected device cannot update pose data");
    malformed = pose; malformed.bPoseIsValid = false;
    CheckRejected(malformed, "An invalid sample cannot update pose data");
    malformed = pose;
    for (auto& row : malformed.mDeviceToAbsoluteTracking.m)
        for (int column = 0; column < 3; ++column) row[column] = 0.0f;
    CheckRejected(malformed, "A collapsed rotation cannot move the camera to a fabricated origin");
    malformed = pose; malformed.mDeviceToAbsoluteTracking.m[0][0] = -1.0f;
    CheckRejected(malformed, "A mirrored rotation cannot enter native pose conversion");
    malformed = pose; malformed.mDeviceToAbsoluteTracking.m[0][1] = 0.25f;
    CheckRejected(malformed, "A sheared tracked pose cannot distort the retained camera");
    malformed = pose; malformed.mDeviceToAbsoluteTracking.m[0][0] = 2.0f;
    CheckRejected(malformed, "A scaled tracked rotation cannot distort pose conversion");
    std::printf("Pose data recovery passed %d checks\n", checks);
}
