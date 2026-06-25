#pragma once

#include <cstdint>

#include "openxr_bridge_protocol.h"

struct OpenXrHelperLaunchConfig
{
    bool enabled = false;
    uint32_t submitTestFrames = 180;
    uint32_t waitReadySeconds = 45;
};

OpenXrHelperLaunchConfig L4D2VR_ReadOpenXrHelperLaunchConfig();
bool L4D2VR_StartOpenXrHelper(const OpenXrHelperLaunchConfig& config);
bool L4D2VR_OpenXrHelperBridgeIsStarted();
bool L4D2VR_OpenXrHelperHasSubmittedFrame();
bool L4D2VR_ReadOpenXrHmdPose(L4D2VROpenXrPoseDesc& pose, uint32_t* generation = nullptr);
bool L4D2VR_ReadOpenXrRuntimeViewConfig(L4D2VROpenXrRuntimeViewConfigDesc& config, uint32_t* generation = nullptr);
void L4D2VR_PublishOpenXrGameRenderPose(const L4D2VROpenXrPoseDesc& pose);
void L4D2VR_PublishOpenXrSharedTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& texture);
void L4D2VR_PublishOpenXrSharedTextureFrame(uint32_t frameId);
