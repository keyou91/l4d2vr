#pragma once
#include "openvr.h"
#include <cmath>
#include <cstring>

namespace l4d2vr_controller_tip
{
    enum class Status
    {
        Ready, NoInterface, UnsupportedRole, NoDevice, NoInput,
        NoModelName, NoComponent, InvalidTransform
    };

    inline const char* StatusName(Status status)
    {
        switch (status)
        {
        case Status::Ready: return "ready";
        case Status::NoInterface: return "no-interface";
        case Status::UnsupportedRole: return "unsupported-role";
        case Status::NoDevice: return "no-device";
        case Status::NoInput: return "no-input";
        case Status::NoModelName: return "no-model-name";
        case Status::NoComponent: return "no-component";
        case Status::InvalidTransform: return "invalid-transform";
        }
        return "unknown";
    }

    inline vr::HmdMatrix34_t Identity()
    {
        return { { { 1.0f, 0.0f, 0.0f, 0.0f },
                   { 0.0f, 1.0f, 0.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f, 0.0f } } };
    }

    struct Result
    {
        vr::HmdMatrix34_t matrix = Identity();
        Status status = Status::NoInterface;
        vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid;
        vr::EVRInputError inputError = vr::VRInputError_None;
        vr::ETrackedPropertyError propertyError = vr::TrackedProp_Success;
    };

    inline bool Finite(const vr::HmdMatrix34_t& matrix)
    {
        for (const auto& row : matrix.m)
            for (float value : row)
                if (!std::isfinite(value))
                    return false;
        return true;
    }

    // Templates allow the same driver boundary to be tested without a VR runtime.
    template<class System, class Input, class Models>
    Result Lookup(System* system, Input* input, Models* models,
        vr::ETrackedControllerRole role)
    {
        Result result;
        if (!system || !input || !models)
            return result;
        const char* path = nullptr;
        if (role == vr::TrackedControllerRole_LeftHand)
            path = "/user/hand/left";
        else if (role == vr::TrackedControllerRole_RightHand)
            path = "/user/hand/right";
        else
        {
            result.status = Status::UnsupportedRole;
            return result;
        }
        result.device = system->GetTrackedDeviceIndexForControllerRole(role);
        if (result.device == vr::k_unTrackedDeviceIndex_Hmd ||
            result.device >= vr::k_unMaxTrackedDeviceCount ||
            !system->IsTrackedDeviceConnected(result.device))
        {
            result.status = Status::NoDevice;
            return result;
        }
        vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;
        result.inputError = input->GetInputSourceHandle(path, &inputValue);
        if (result.inputError != vr::VRInputError_None ||
            inputValue == vr::k_ulInvalidInputValueHandle)
        {
            result.status = Status::NoInput;
            return result;
        }
        char model[vr::k_unMaxPropertyStringSize]{};
        const uint32_t length = system->GetStringTrackedDeviceProperty(
            result.device, vr::Prop_RenderModelName_String, model,
            static_cast<uint32_t>(sizeof(model)), &result.propertyError);
        if (result.propertyError != vr::TrackedProp_Success ||
            length < 2 || length > sizeof(model) ||
            std::memchr(model, '\0', length) != model + length - 1)
        {
            result.status = Status::NoModelName;
            return result;
        }
        vr::RenderModel_ControllerMode_State_t controllerState{};
        vr::RenderModel_ComponentState_t component{};
        if (!models->GetComponentStateForDevicePath(model,
            vr::k_pch_Controller_Component_Tip, inputValue,
            &controllerState, &component))
        {
            result.status = Status::NoComponent;
            return result;
        }
        if (!Finite(component.mTrackingToComponentLocal))
        {
            result.status = Status::InvalidTransform;
            return result;
        }
        result.matrix = component.mTrackingToComponentLocal;
        result.status = Status::Ready;
        return result;
    }
}
