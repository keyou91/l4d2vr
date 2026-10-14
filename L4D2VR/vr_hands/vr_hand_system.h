#pragma once

#include "openvr.h"
#include "vr_hand_asset_loader.h"
#include "vr_hand_renderer_d3d9.h"
#include "vr_hand_skeleton_runtime.h"
#include "vector.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class CViewSetup;
struct IDirect3DDevice9;
namespace VrHandVmPose { struct Snapshot; }

struct VrHandControllerContext
{
    vr::IVRSystem* system = nullptr;
    std::array<vr::TrackedDeviceIndex_t, 2> physicalIndices{
        vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid};
};

class VrHandSystem
{
public:
    VrHandSystem();
    ~VrHandSystem();

    VrHandSystem(const VrHandSystem&) = delete;
    VrHandSystem& operator=(const VrHandSystem&) = delete;

    bool DrawForEye(
        IDirect3DDevice9* device,
        vr::IVRInput* input,
        const l4d2vr_input::ActionFrame& actionFrame,
        const VrHandControllerContext& controllers,
        const CViewSetup& view,
        int eyeIndex,
        float vrScale,
        float modelScale,
        bool motionRangeWithoutController,
        bool rightUseViewmodelPose,
        bool twoHandedViewmodelPose,
        bool leftHanded,
        bool allowControllerlessTestPose,
        bool debugLog,
        float sceneLightScale,
        const Vector& leftControllerPosition,
        const QAngle& leftControllerAngles,
        const Vector& rightControllerPosition,
        const QAngle& rightControllerAngles,
        const Vector& currentViewmodelPosition,
        const QAngle& currentViewmodelAngles,
        const Vector& leftHandPoseOffsetMeters,
        const Vector& leftHandPoseRotationOffsetDeg,
        const Vector& rightHandPoseOffsetMeters,
        const Vector& rightHandPoseRotationOffsetDeg,
        const VrHandMatrix4* standaloneMagazineBoxWorld,
        const Vector& standaloneMagazineBoxMins,
        const Vector& standaloneMagazineBoxMaxs,
        bool standaloneMagazineBoxUseViewmodelLayer,
        const VrHandMatrix4* magazineSocketCaptureBoxWorld,
        const Vector& magazineSocketCaptureBoxMins,
        const Vector& magazineSocketCaptureBoxMaxs,
        bool magazineSocketCaptureBoxUseViewmodelLayer,
        const VrHandMatrix4* currentMagazineBoxWorld,
        const Vector& currentMagazineBoxMins,
        const Vector& currentMagazineBoxMaxs,
        bool currentMagazineBoxUseViewmodelLayer,
        const VrHandMatrix4* currentBoltBoxWorld,
        const Vector& currentBoltBoxMins,
        const Vector& currentBoltBoxMaxs,
        bool currentBoltBoxUseViewmodelLayer,
        const VrHandMatrix4* leftHandTargetBoxWorld,
        const Vector& leftHandTargetBoxMins,
        const Vector& leftHandTargetBoxMaxs,
        bool leftHandTargetBoxUseViewmodelLayer,
        unsigned magazineGripPhysicalMask,
        const std::array<float, 5>& gloveFingerMaxCurl,
        VrHandDrawPass drawPass);

    bool DrawMagazineDebugBoxesForEye(
        IDirect3DDevice9* device,
        const CViewSetup& view,
        float sceneLightScale,
        const VrHandMatrix4* standaloneMagazineBoxWorld,
        const Vector& standaloneMagazineBoxMins,
        const Vector& standaloneMagazineBoxMaxs,
        bool standaloneMagazineBoxUseViewmodelLayer,
        const VrHandMatrix4* magazineSocketCaptureBoxWorld,
        const Vector& magazineSocketCaptureBoxMins,
        const Vector& magazineSocketCaptureBoxMaxs,
        bool magazineSocketCaptureBoxUseViewmodelLayer,
        const VrHandMatrix4* currentMagazineBoxWorld,
        const Vector& currentMagazineBoxMins,
        const Vector& currentMagazineBoxMaxs,
        bool currentMagazineBoxUseViewmodelLayer,
        const VrHandMatrix4* currentBoltBoxWorld,
        const Vector& currentBoltBoxMins,
        const Vector& currentBoltBoxMaxs,
        bool currentBoltBoxUseViewmodelLayer,
        const VrHandMatrix4* leftHandTargetBoxWorld,
        const Vector& leftHandTargetBoxMins,
        const Vector& leftHandTargetBoxMaxs,
        bool leftHandTargetBoxUseViewmodelLayer,
        VrHandDrawPass drawPass);

    bool ClearViewmodelOcclusionStencil(IDirect3DDevice9* device);
    void OnDeviceLost();
    bool EnsureAssetsAvailable(bool debugLog);
    bool IsDependencyUnavailable() const;
    const std::string& DependencyFailureReason() const;
    // Render-thread only: wrist position in the currently skinned glove model.
    bool ReadFreeWristModelPosition(bool physicalLeft, const l4d2vr_input::ActionFrame& actionFrame,
        Vector& position) const;

private:
    std::uint32_t m_LastPoseUpdateTick = 0u;
    struct FreeWristSnapshot
    {
        Vector position{0.0f, 0.0f, 0.0f};
        std::uint64_t frameToken = 0u;
        std::uint32_t updatedAt = 0u;
        bool valid = false;
    };
    mutable std::mutex m_FreeWristMutex;
    std::array<FreeWristSnapshot, 2> m_FreeWrists{};
    struct HandState
    {
        const char* actionPath = nullptr;
        const char* assetFileName = nullptr;
        vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
        vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle;
        VrHandMeshAsset asset;
        bool assetLoadAttempted = false;
        std::uint32_t assetLoadAttemptTick = 0u;
        std::string assetFailureReason;
        VrHandSkeletonRuntime skeleton;
        std::vector<VrHandMatrixRows3x4> palette;
        bool paletteValid = false;
        std::uint64_t paletteFrameToken = 0u;
        bool skeletonInitialized = false;
    };

    struct ViewmodelPoseState
    {
        bool enabled = false;
        int targetHandIndex = -1;
        std::vector<VrHandMatrixRows3x4> palette;
        bool paletteValid = false;
        bool palmWorldValid = false;
        bool autoGripAligned = false;
        bool anchorValid = false;
        bool debugLogged = false;
        bool debugMissingLogged = false;
        std::string poseModel;
        std::string anchorModel;
        VrHandMatrix4 palmWorld{};
        VrHandMatrix4 palmFromModelRoot{};
        bool palmFromModelRootValid = false;
        VrHandMatrix4 palmFromGloveWrist{};
        VrHandMatrix4 gloveWristBindInverse{};
    };

    bool EnsureAssetsLoaded(bool debugLog);
    void PublishFreeWristModelPosition(size_t handIndex);
    bool EnsureInitialized(
        vr::IVRInput* input,
        const l4d2vr_input::ActionFrame& actionFrame,
        const VrHandControllerContext& controllers,
        bool rightUseViewmodelPose,
        bool twoHandedViewmodelPose,
        bool leftHanded,
        bool debugLog);
    bool ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const;
    bool EnsureStandaloneMagazineBoxLoaded(
        const Vector& mins,
        const Vector& maxs,
        std::uint32_t fallbackColorArgb,
        const char* debugName,
        bool debugLog);
    bool DrawStandaloneMagazineBox(
        IDirect3DDevice9* device,
        const CViewSetup& view,
        float sceneLightScale,
        const VrHandMatrix4* world,
        const Vector& mins,
        const Vector& maxs,
        std::uint32_t fallbackColorArgb,
        const char* debugName,
        bool useViewmodelLayer,
        VrHandDrawPass drawPass);
    void UpdatePoses(
        vr::IVRInput* input,
        const l4d2vr_input::ActionFrame& actionFrame,
        const VrHandControllerContext& controllers,
        bool motionRangeWithoutController,
        bool rightUseViewmodelPose,
        bool twoHandedViewmodelPose,
        bool leftHanded,
        unsigned magazineGripPhysicalMask,
        const std::array<float, 5>& gloveFingerMaxCurl,
        bool debugLog);
    bool DrawControllerlessTestPose(
        IDirect3DDevice9* device,
        const CViewSetup& view,
        float vrScale,
        float modelScale,
        bool rightUseViewmodelPose,
        bool twoHandedViewmodelPose,
        bool leftHanded,
        float sceneLightScale,
        bool debugLog,
        const Vector& leftHandPoseOffsetMeters,
        const Vector& leftHandPoseRotationOffsetDeg,
        const Vector& rightHandPoseOffsetMeters,
        const Vector& rightHandPoseRotationOffsetDeg,
        VrHandDrawPass drawPass);
    bool BuildViewmodelPalette(
        const VrHandVmPose::Snapshot& snapshot,
        int sourceSide,
        int targetHandIndex,
        const std::array<float, 5>& gloveFingerMaxCurl,
        ViewmodelPoseState& state);
    bool BuildViewmodelWorld(
        const ViewmodelPoseState& state,
        float sourceUnitsPerMeter,
        float modelScale,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg,
        bool anchorToCurrentViewmodelRoot,
        const Vector& currentViewmodelPosition,
        const QAngle& currentViewmodelAngles,
        VrHandMatrix4& outWorld) const;
    void ReportErrorOnce(const std::string& error);
    void ReportWarningOnce(const std::string& warning);
    static int ViewmodelPoseStateIndex(int sourceSide);
    void ResetViewmodelPoseState(ViewmodelPoseState& state, bool enabled, int targetHandIndex);

    std::array<HandState, 2> m_Hands;
    std::array<ViewmodelPoseState, 2> m_ViewmodelPoseStates;
    VrHandRendererD3D9 m_Renderer;
    std::unordered_set<std::string> m_ReportedErrors;
    std::unordered_set<std::string> m_ReportedWarnings;
    bool m_AssetsLoaded = false;
    bool m_DependencyUnavailable = false;
    std::string m_DependencyFailureReason;
    bool m_InitializationAttempted = false;
    bool m_Initialized = false;
    bool m_DebugInitializationLogged = false;
    bool m_DebugTestPoseLogged = false;
    unsigned m_MagazineGripPhysicalMaskWasEnabled = 0u;
    VrHandMeshAsset m_StandaloneMagazineBoxAsset;
    std::vector<VrHandMatrixRows3x4> m_StandaloneMagazineBoxPalette;
    std::string m_StandaloneMagazineBoxKey;
    std::unordered_set<std::string> m_StandaloneMagazineBoxDrawLoggedKeys;
};
