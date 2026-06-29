#pragma once

#include <cstdint>

#include "openxr_bridge_protocol.h"

struct OpenXrHelperLaunchConfig
{
    bool enabled = false;
    bool swapProjectionEyes = false;
    bool swapProjectionViewOrder = false;
    bool mirrorProjectionHorizontal = false;
    bool swapGameEyeOrigins = false;
    bool disableQuadOverlays = false;
    bool disableProjectionLayer = false;
    bool useSymmetricProjectionFov = false;
    bool useGameRenderPoseForProjection = false;
    int forceMonoProjectionEye = -1;
    int forceMonoProjectionView = -1;
    uint32_t submitTestFrames = 180;
    uint32_t waitReadySeconds = 45;
};

OpenXrHelperLaunchConfig L4D2VR_ReadOpenXrHelperLaunchConfig();
bool L4D2VR_StartOpenXrHelper(const OpenXrHelperLaunchConfig& config);
bool L4D2VR_OpenXrHelperBridgeIsStarted();
bool L4D2VR_OpenXrHelperHasSubmittedFrame();
bool L4D2VR_ReadOpenXrHmdPose(L4D2VROpenXrPoseDesc& pose, uint32_t* generation = nullptr);
bool L4D2VR_ReadOpenXrRuntimeViewConfig(L4D2VROpenXrRuntimeViewConfigDesc& config, uint32_t* generation = nullptr);
bool L4D2VR_ReadOpenXrInputState(L4D2VROpenXrInputStateDesc& inputState, uint32_t* generation = nullptr);
void L4D2VR_PublishOpenXrGameRenderPose(const L4D2VROpenXrPoseDesc& pose);
void L4D2VR_PublishOpenXrHapticRequest(uint32_t handIndex, float durationSeconds, float frequency, float amplitude);
void L4D2VR_PublishOpenXrSharedTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& texture);
void L4D2VR_PublishOpenXrSharedTextureFrame(uint32_t frameId);
void L4D2VR_PublishOpenXrOverlay(uint32_t overlayIndex, const L4D2VROpenXrOverlayDesc& overlay);
void L4D2VR_PublishOpenXrOverlayFrame(uint32_t frameId);
