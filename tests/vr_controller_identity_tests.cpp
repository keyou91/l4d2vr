#include "../L4D2VR/vr_controller_identity.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

namespace
{
    int checks = 0;
    void Check(bool value, const char* expression, int line)
    {
        ++checks;
        if (!value)
        {
            std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
            std::exit(EXIT_FAILURE);
        }
    }
#define CHECK(value) Check(bool(value), #value, __LINE__)
    constexpr auto invalid = vr::k_unTrackedDeviceIndexInvalid;
    namespace identity = l4d2vr_controller_identity;

    struct Device
    {
        bool connected = false;
        vr::ETrackedDeviceClass type = vr::TrackedDeviceClass_Invalid;
        int32_t hint = vr::TrackedControllerRole_Invalid;
        vr::ETrackedPropertyError hintError = vr::TrackedProp_UnknownProperty;
    };
    struct System
    {
        std::array<Device, vr::k_unMaxTrackedDeviceCount> devices{};
        vr::TrackedDeviceIndex_t legacyLeft = invalid, legacyRight = invalid;
        int hintReads = 0;
        vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole role)
        { return role == vr::TrackedControllerRole_LeftHand ? legacyLeft : legacyRight; }
        bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t index)
        { CHECK(index < devices.size()); return devices[index].connected; }
        vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t index)
        { CHECK(index < devices.size()); return devices[index].type; }
        int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t index, vr::ETrackedDeviceProperty property,
            vr::ETrackedPropertyError* error)
        {
            CHECK(index < devices.size());
            CHECK(property == vr::Prop_ControllerRoleHint_Int32);
            ++hintReads;
            *error = devices[index].hintError;
            return devices[index].hint;
        }
        void Add(vr::TrackedDeviceIndex_t index, vr::ETrackedControllerRole hint)
        {
            devices[index] = { true, vr::TrackedDeviceClass_Controller, hint, vr::TrackedProp_Success };
        }
    };

    vr::TrackedDevicePose_t Pose(float x)
    {
        vr::TrackedDevicePose_t pose{};
        pose.bDeviceIsConnected = true;
        pose.bPoseIsValid = true;
        pose.mDeviceToAbsoluteTracking.m[0][0] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[1][1] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[2][2] = 1.0f;
        pose.mDeviceToAbsoluteTracking.m[0][3] = x;
        return pose;
    }
}

struct VR
{
    System system;
    System* m_System = &system;
    vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount]{};
    vr::TrackedDevicePose_t m_HmdPose{}, m_LeftControllerPose{}, m_RightControllerPose{};
    bool m_LeftHanded = false;
#include "vr_controller_identity_cache.inl"
    void GetPoses();
    vr::TrackedDeviceIndex_t GetPhysicalControllerIndexForHand(bool leftHand) const;
    void GetPoseData(vr::TrackedDevicePose_t& source, vr::TrackedDevicePose_t& destination)
    { destination = source; }
};

#include "vr_controller_identity_functions.inl"

static void ExplicitHintAndLegacyPrecedenceTests()
{
    CHECK(identity::Resolve(static_cast<System*>(nullptr)).left == invalid);
    System system;
    system.Add(5, vr::TrackedControllerRole_LeftHand);
    system.Add(2, vr::TrackedControllerRole_RightHand);
    auto pair = identity::Resolve(&system);
    CHECK(pair.left == 5 && pair.right == 2);
    CHECK(identity::Unpack(identity::Pack(pair)).left == 5);
    CHECK(identity::Unpack(identity::Pack(pair)).right == 2);

    system.legacyLeft = 2;
    system.legacyRight = 5;
    const int reads = system.hintReads;
    pair = identity::Resolve(&system);
    CHECK(pair.left == 2 && pair.right == 5);
    CHECK(system.hintReads == reads);
    system.legacyRight = invalid;
    pair = identity::Resolve(&system);
    CHECK(pair.left == 2 && pair.right == invalid);

    system.legacyLeft = 5;
    pair = identity::Resolve(&system);
    CHECK(pair.left == 5 && pair.right == 2);
    system.legacyRight = 5;
    pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == invalid);
}

static void AmbiguousAndUnsupportedEvidenceTests()
{
    System system;
    system.Add(1, vr::TrackedControllerRole_LeftHand);
    system.Add(2, vr::TrackedControllerRole_RightHand);
    system.Add(3, vr::TrackedControllerRole_LeftHand);
    auto pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == 2);

    system.devices[3].connected = false;
    system.devices[1].hintError = vr::TrackedProp_InvalidDevice;
    pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == 2);
    system.devices[1].hintError = vr::TrackedProp_Success;
    system.devices[1].type = vr::TrackedDeviceClass_TrackingReference;
    pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == 2);
    system.devices[1].type = vr::TrackedDeviceClass_Controller;
    system.devices[1].connected = false;
    pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == 2);
    system.devices[1].connected = true;
    system.devices[1].hint = vr::TrackedControllerRole_OptOut;
    pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == 2);

    system.Add(0, vr::TrackedControllerRole_LeftHand);
    system.legacyLeft = 0;
    system.legacyRight = vr::k_unMaxTrackedDeviceCount;
    pair = identity::Resolve(&system);
    CHECK(pair.left == invalid && pair.right == 2);
    CHECK(identity::CachedIndex(&system, identity::Pack({ 2, 2 }), true) == invalid);
    CHECK(identity::CachedIndex(static_cast<System*>(nullptr), identity::Pack({ 1, 2 }), true) == invalid);
}

static void PoseValidationTests()
{
    const auto good = Pose(1.0f);
    CHECK(identity::UsablePose(good));
    auto noisy = good;
    noisy.mDeviceToAbsoluteTracking.m[0][0] = 1.001f;
    noisy.mDeviceToAbsoluteTracking.m[0][1] = 0.0001f;
    CHECK(identity::UsablePose(noisy));
    for (int x = -180; x <= 180; x += 30)
        for (int y = -180; y <= 180; y += 30)
            for (int z = -180; z <= 180; z += 30)
            {
                auto rotated = good;
                constexpr double radians = 3.14159265358979323846 / 180.0;
                const double cx = std::cos(x*radians), sx = std::sin(x*radians);
                const double cy = std::cos(y*radians), sy = std::sin(y*radians);
                const double cz = std::cos(z*radians), sz = std::sin(z*radians);
                const double matrix[3][3] = {
                    {cz*cy, cz*sy*sx-sz*cx, cz*sy*cx+sz*sx},
                    {sz*cy, sz*sy*sx+cz*cx, sz*sy*cx-cz*sx},
                    {-sy, cy*sx, cy*cx}};
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        rotated.mDeviceToAbsoluteTracking.m[row][column] = static_cast<float>(matrix[row][column]);
                CHECK(identity::UsablePose(rotated));
            }
    auto bad = good;
    bad.bDeviceIsConnected = false;
    CHECK(!identity::UsablePose(bad));
    bad = good;
    bad.bPoseIsValid = false;
    CHECK(!identity::UsablePose(bad));
    bad = good;
    bad.mDeviceToAbsoluteTracking.m[0][3] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!identity::UsablePose(bad));
    bad = good;
    bad.vVelocity.v[2] = std::numeric_limits<float>::infinity();
    CHECK(!identity::UsablePose(bad));
    bad = good;
    bad.vAngularVelocity.v[1] = -std::numeric_limits<float>::infinity();
    CHECK(!identity::UsablePose(bad));
    bad = good;
    bad.mDeviceToAbsoluteTracking.m[1][2] = 1.01f;
    CHECK(!identity::UsablePose(bad));
    CHECK(identity::WithUsablePoses({ 1, 2 }, nullptr, 0).left == invalid);
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    poses[1] = good;
    auto pair = identity::WithUsablePoses({ 1, 2 }, poses.data(), static_cast<uint32_t>(poses.size()));
    CHECK(pair.left == 1 && pair.right == invalid);
}

static void ActualPoseSelectionAndCachedGetterTests()
{
    VR session;
    session.system.Add(3, vr::TrackedControllerRole_LeftHand);
    session.system.Add(5, vr::TrackedControllerRole_RightHand);
    session.m_Poses[0] = Pose(7.0f);
    session.m_Poses[3] = Pose(30.0f);
    session.m_Poses[5] = Pose(50.0f);
    CHECK(session.GetPhysicalControllerIndexForHand(true) == invalid);
    session.GetPoses();
    CHECK(session.GetPhysicalControllerIndexForHand(true) == 3);
    CHECK(session.GetPhysicalControllerIndexForHand(false) == 5);
    CHECK(session.m_LeftControllerPose.mDeviceToAbsoluteTracking.m[0][3] == 30.0f);
    CHECK(session.m_RightControllerPose.mDeviceToAbsoluteTracking.m[0][3] == 50.0f);
    CHECK(session.m_HmdPose.mDeviceToAbsoluteTracking.m[0][3] == 7.0f);
    const int reads = session.system.hintReads;
    for (int draw = 0; draw < 20; ++draw)
    {
        CHECK(session.GetPhysicalControllerIndexForHand(true) == 3);
        CHECK(session.GetPhysicalControllerIndexForHand(false) == 5);
    }
    CHECK(session.system.hintReads == reads);

    session.m_LeftHanded = true;
    session.GetPoses();
    CHECK(session.GetPhysicalControllerIndexForHand(true) == 3);
    CHECK(session.GetPhysicalControllerIndexForHand(false) == 5);
    CHECK(session.m_LeftControllerPose.mDeviceToAbsoluteTracking.m[0][3] == 50.0f);
    CHECK(session.m_RightControllerPose.mDeviceToAbsoluteTracking.m[0][3] == 30.0f);

    session.system.devices[3].connected = false;
    CHECK(session.GetPhysicalControllerIndexForHand(true) == invalid);
    session.m_LeftHanded = false;
    session.GetPoses();
    CHECK(!session.m_LeftControllerPose.bPoseIsValid);
    session.system.devices[3].connected = true;
    session.GetPoses();
    CHECK(session.GetPhysicalControllerIndexForHand(true) == 3);

    session.system.legacyLeft = 5;
    session.system.legacyRight = 3;
    session.GetPoses();
    CHECK(session.GetPhysicalControllerIndexForHand(true) == 5);
    CHECK(session.GetPhysicalControllerIndexForHand(false) == 3);
    CHECK(session.m_LeftControllerPose.mDeviceToAbsoluteTracking.m[0][3] == 50.0f);

    session.m_Poses[5].vVelocity.v[0] = std::numeric_limits<float>::quiet_NaN();
    session.GetPoses();
    CHECK(session.GetPhysicalControllerIndexForHand(true) == invalid);
    CHECK(!session.m_LeftControllerPose.bPoseIsValid);
    CHECK(session.GetPhysicalControllerIndexForHand(false) == 3);

    session.system.legacyLeft = session.system.legacyRight = invalid;
    session.system.devices[3].connected = false;
    session.system.Add(7, vr::TrackedControllerRole_LeftHand);
    session.m_Poses[7] = Pose(70.0f);
    session.m_Poses[5] = Pose(50.0f);
    session.GetPoses();
    CHECK(session.GetPhysicalControllerIndexForHand(true) == 7);
    CHECK(session.GetPhysicalControllerIndexForHand(false) == 5);
    CHECK(session.m_LeftControllerPose.mDeviceToAbsoluteTracking.m[0][3] == 70.0f);
}

int main()
{
    ExplicitHintAndLegacyPrecedenceTests();
    AmbiguousAndUnsupportedEvidenceTests();
    PoseValidationTests();
    ActualPoseSelectionAndCachedGetterTests();
    std::printf("PASS: controller identity (%d checks)\n", checks);
    return EXIT_SUCCESS;
}
