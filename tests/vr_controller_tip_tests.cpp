#include "../L4D2VR/vr_controller_tip.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#define CHECK(value) do { if (!(value)) std::abort(); } while (false)

namespace
{
    struct System
    {
        vr::TrackedDeviceIndex_t device = 3;
        bool connected = true;
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        std::string name = "custom_controller";
        uint32_t reportedLength = 0;
        bool terminate = true;
        int propertyCalls = 0;
        vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole)
        { return device; }
        bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t) { return connected; }
        uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t,
            vr::ETrackedDeviceProperty, char* buffer, uint32_t capacity,
            vr::ETrackedPropertyError* outError)
        {
            ++propertyCalls;
            *outError = error;
            // Property failures can leave the caller's buffer untouched.
            if (error != vr::TrackedProp_Success)
                return reportedLength;
            if (!terminate)
                std::memset(buffer, 'x', capacity);
            else if (name.size() < capacity)
                std::memcpy(buffer, name.c_str(), name.size() + 1);
            return reportedLength ? reportedLength : static_cast<uint32_t>(name.size() + 1);
        }
    };
    struct Input
    {
        vr::EVRInputError error = vr::VRInputError_None;
        vr::VRInputValueHandle_t handle = 7;
        std::string path;
        vr::EVRInputError GetInputSourceHandle(const char* value, vr::VRInputValueHandle_t* output)
        { path = value; *output = handle; return error; }
    };
    struct Models
    {
        bool success = true;
        int calls = 0;
        vr::HmdMatrix34_t matrix = l4d2vr_controller_tip::Identity();
        bool GetComponentStateForDevicePath(const char* name, const char* component,
            vr::VRInputValueHandle_t handle, const vr::RenderModel_ControllerMode_State_t*,
            vr::RenderModel_ComponentState_t* output)
        {
            CHECK(name && name[0]);
            CHECK(std::strcmp(component, vr::k_pch_Controller_Component_Tip) == 0);
            CHECK(handle == 7);
            ++calls;
            output->mTrackingToComponentLocal = matrix;
            return success;
        }
    };
    struct Fixture
    {
        System system;
        Input input;
        Models models;
        l4d2vr_controller_tip::Result Lookup(vr::ETrackedControllerRole role = vr::TrackedControllerRole_RightHand)
        { return l4d2vr_controller_tip::Lookup(&system, &input, &models, role); }
        void Fallback(l4d2vr_controller_tip::Status status)
        {
            const auto result = Lookup();
            CHECK(result.status == status);
            const auto identity = l4d2vr_controller_tip::Identity();
            CHECK(std::memcmp(&result.matrix, &identity, sizeof(identity)) == 0);
        }
    };
}

int main()
{
    using namespace l4d2vr_controller_tip;
    Fixture f;
    f.models.matrix.m[2][3] = -0.12f;
    auto result = f.Lookup();
    CHECK(result.status == Status::Ready && result.matrix.m[2][3] == -0.12f);
    CHECK(f.input.path == "/user/hand/right");
    CHECK(f.Lookup(vr::TrackedControllerRole_LeftHand).status == Status::Ready);
    CHECK(f.input.path == "/user/hand/left");
    CHECK(f.Lookup(vr::TrackedControllerRole_Invalid).status == Status::UnsupportedRole);
    CHECK(Lookup(static_cast<System*>(nullptr), &f.input, &f.models,
        vr::TrackedControllerRole_RightHand).status == Status::NoInterface);
    CHECK(Lookup(&f.system, static_cast<Input*>(nullptr), &f.models,
        vr::TrackedControllerRole_RightHand).status == Status::NoInterface);
    CHECK(Lookup(&f.system, &f.input, static_cast<Models*>(nullptr),
        vr::TrackedControllerRole_RightHand).status == Status::NoInterface);

    for (const auto device : { vr::k_unTrackedDeviceIndexInvalid,
        vr::k_unTrackedDeviceIndexOther, vr::k_unMaxTrackedDeviceCount,
        vr::k_unTrackedDeviceIndex_Hmd })
    {
        Fixture invalid;
        invalid.system.device = device;
        invalid.Fallback(Status::NoDevice);
        CHECK(invalid.system.propertyCalls == 0 && invalid.models.calls == 0);
    }
    f = Fixture{};
    f.system.connected = false;
    f.Fallback(Status::NoDevice);
    CHECK(f.system.propertyCalls == 0 && f.models.calls == 0);
    f = Fixture{};
    f.input.error = vr::VRInputError_InvalidHandle;
    f.Fallback(Status::NoInput);
    CHECK(f.system.propertyCalls == 0 && f.models.calls == 0);
    f.input.error = vr::VRInputError_None;
    f.input.handle = vr::k_ulInvalidInputValueHandle;
    f.Fallback(Status::NoInput);
    CHECK(f.models.calls == 0);

    for (const auto error : { vr::TrackedProp_UnknownProperty, vr::TrackedProp_BufferTooSmall,
        vr::TrackedProp_InvalidDevice })
    {
        Fixture missing;
        missing.system.error = error;
        missing.Fallback(Status::NoModelName);
        CHECK(missing.models.calls == 0);
        CHECK(missing.Lookup().propertyError == error);
        missing.system.error = vr::TrackedProp_Success;
        CHECK(missing.Lookup().status == Status::Ready);
    }
    f = Fixture{};
    f.system.name.clear();
    f.Fallback(Status::NoModelName);
    CHECK(f.models.calls == 0);
    f = Fixture{};
    f.system.reportedLength = vr::k_unMaxPropertyStringSize + 1;
    f.Fallback(Status::NoModelName);
    CHECK(f.models.calls == 0);
    f.system.reportedLength = 3; // A claimed length must agree with the terminator.
    f.Fallback(Status::NoModelName);
    CHECK(f.models.calls == 0);
    f.system.reportedLength = vr::k_unMaxPropertyStringSize;
    f.system.terminate = false;
    f.Fallback(Status::NoModelName);
    CHECK(f.models.calls == 0);
    f = Fixture{};
    f.system.name.assign(vr::k_unMaxPropertyStringSize - 1, 'x');
    CHECK(f.Lookup().status == Status::Ready);

    f = Fixture{};
    f.models.success = false;
    f.models.matrix.m[0][3] = 99.0f;
    f.Fallback(Status::NoComponent);
    f.models.success = true;
    f.models.matrix.m[1][2] = std::numeric_limits<float>::quiet_NaN();
    f.Fallback(Status::InvalidTransform);
    f.models.matrix = Identity();
    f.models.matrix.m[2][3] = std::numeric_limits<float>::infinity();
    f.Fallback(Status::InvalidTransform);
    f.models.matrix = Identity();
    CHECK(f.Lookup().status == Status::Ready);
    std::puts("Controller driver boundary and fallback checks passed");
}
