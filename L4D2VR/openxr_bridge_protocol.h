#pragma once

#include <cstdint>

constexpr uint32_t L4D2VR_OPENXR_BRIDGE_MAGIC = 0x5258344Cu; // L4XR
constexpr uint32_t L4D2VR_OPENXR_BRIDGE_VERSION = 7;

enum class L4D2VROpenXrBridgeStatus : uint32_t
{
    Idle = 0,
    Starting = 1,
    LoaderLoaded = 2,
    InstanceCreated = 3,
    SessionCreated = 4,
    SessionRunning = 5,
    SubmittedFrame = 6,
    Completed = 7,
    Failed = 8,
    WaitingForSharedTextures = 9,
    SharedTexturesReady = 10
};

enum : uint32_t
{
    L4D2VR_OPENXR_EYE_LEFT = 0,
    L4D2VR_OPENXR_EYE_RIGHT = 1,
    L4D2VR_OPENXR_EYE_COUNT = 2,
    L4D2VR_OPENXR_EYE_LEFT_READY = 1u << L4D2VR_OPENXR_EYE_LEFT,
    L4D2VR_OPENXR_EYE_RIGHT_READY = 1u << L4D2VR_OPENXR_EYE_RIGHT,
    L4D2VR_OPENXR_EYES_READY_MASK = L4D2VR_OPENXR_EYE_LEFT_READY | L4D2VR_OPENXR_EYE_RIGHT_READY
};

struct L4D2VROpenXrSharedTextureDesc
{
    uint32_t valid = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t sampleCount = 0;
    uint32_t handleType = 0;
    uint32_t queueFamilyIndex = 0;
    uint32_t reserved0 = 0;
    uint64_t kmtHandle = 0;
    uint64_t image = 0;
    float uMin = 0.0f;
    float vMin = 0.0f;
    float uMax = 1.0f;
    float vMax = 1.0f;
    float renderFovXDeg = 90.0f;
    float renderAspect = 1.0f;
};

struct L4D2VROpenXrPoseDesc
{
    uint32_t valid = 0;
    uint32_t viewStateFlags = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    int64_t displayTime = 0;
    float position[3] = {};
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct L4D2VROpenXrRuntimeViewDesc
{
    uint32_t valid = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t recommendedSampleCount = 0;
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

struct L4D2VROpenXrRuntimeViewConfigDesc
{
    uint32_t valid = 0;
    uint32_t viewCount = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    L4D2VROpenXrRuntimeViewDesc views[L4D2VR_OPENXR_EYE_COUNT] = {};
};

struct L4D2VROpenXrBridgeState
{
    uint32_t magic = L4D2VR_OPENXR_BRIDGE_MAGIC;
    uint32_t version = L4D2VR_OPENXR_BRIDGE_VERSION;
    uint32_t size = sizeof(L4D2VROpenXrBridgeState);
    uint32_t status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::Idle);
    uint32_t gamePid = 0;
    uint32_t helperPid = 0;
    uint32_t submittedFrames = 0;
    int32_t exitCode = 0;
    uint64_t heartbeatTickMs = 0;
    uint32_t sharedTextureGeneration = 0;
    uint32_t sharedTexturesReadyMask = 0;
    uint32_t sharedTextureFrameGeneration = 0;
    uint32_t sharedTextureFrameId = 0;
    L4D2VROpenXrSharedTextureDesc eyeTextures[L4D2VR_OPENXR_EYE_COUNT] = {};
    uint32_t trackingPoseGeneration = 0;
    L4D2VROpenXrPoseDesc hmdPose = {};
    uint32_t gameRenderPoseGeneration = 0;
    L4D2VROpenXrPoseDesc gameRenderPose = {};
    uint32_t runtimeViewConfigGeneration = 0;
    L4D2VROpenXrRuntimeViewConfigDesc runtimeViewConfig = {};
    char detail[256] = {};
};
