#pragma once

#include "openvr.h"
#include "vr_input_recovery.h"
#include <cmath>

namespace l4d2vr_hand_input
{
    template<class Input, class System>
    bool ValidateOrigin(Input* input, System* system, vr::VRInputValueHandle_t origin,
        vr::VRInputValueHandle_t expectedSource, vr::TrackedDeviceIndex_t expectedDevice)
    {
        if (!input || !system || origin == vr::k_ulInvalidInputValueHandle ||
            expectedSource == vr::k_ulInvalidInputValueHandle ||
            expectedDevice == vr::k_unTrackedDeviceIndex_Hmd ||
            expectedDevice >= vr::k_unMaxTrackedDeviceCount)
            return false;
        vr::InputOriginInfo_t info{};
        info.trackedDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
        return input->GetOriginTrackedDeviceInfo(origin, &info, sizeof(info)) == vr::VRInputError_None &&
            info.devicePath == expectedSource && info.trackedDeviceIndex == expectedDevice &&
            system->IsTrackedDeviceConnected(expectedDevice) &&
            system->GetTrackedDeviceClass(expectedDevice) == vr::TrackedDeviceClass_Controller;
    }

    template<class Input, class System>
    bool ReadDigitalForHand(Input* input, System* system, const l4d2vr_input::ActionFrame& actionFrame,
        vr::VRActionHandle_t action, const char* sourcePath, vr::TrackedDeviceIndex_t expectedDevice,
        vr::InputDigitalActionData_t& out)
    {
        out = {};
        const auto frame = actionFrame.Read();
        if (!input || !system || !sourcePath || !l4d2vr_input::ActionFrame::Ready(frame) ||
            action == vr::k_ulInvalidActionHandle)
            return false;
        vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle;
        vr::InputDigitalActionData_t data{};
        if (input->GetInputSourceHandle(sourcePath, &source) != vr::VRInputError_None ||
            source == vr::k_ulInvalidInputValueHandle ||
            input->GetDigitalActionData(action, &data, sizeof(data), source) != vr::VRInputError_None ||
            !data.bActive || !std::isfinite(data.fUpdateTime) ||
            !ValidateOrigin(input, system, data.activeOrigin, source, expectedDevice) ||
            !actionFrame.Matches(frame))
            return false;
        out = data;
        if (l4d2vr_input::ActionFrame::SuppressEdges(frame))
            out.bChanged = false;
        return true;
    }

    inline bool ValidStickAxes(const vr::InputAnalogActionData_t& data)
    {
        return std::isfinite(data.x) && std::isfinite(data.y) && std::isfinite(data.z) &&
            std::fabs(data.x) <= 1.0f && std::fabs(data.y) <= 1.0f && std::fabs(data.z) <= 1.0f;
    }
}
