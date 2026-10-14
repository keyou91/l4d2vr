#include "vr.h"

#include "game.h"
#include "hooks.h"
#include "sdk.h"
#include "trace.h"
#include "vr_hand_system.h"
#include "../vr_skeletal_data.h"
#include "vr_hand_math.h"
#include "vr_hand_vm_pose.h"
#include "vr_magazine_policy.h"
#include "vr_interaction_protocol.h"
#include "vr_ammo_grip.h"

#include <d3d9.h>
#include <d3d9_vr.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

VR::VR() = default;

VR::~VR() = default;

namespace
{
    std::string MagazineInteractionPrepareConsoleSoundSample(const std::string& rawSample);
    std::string MagazineInteractionPrepareSoundSamplePath(const std::string& rawSample);
    bool MagazineInteractionReprojectScenePointToViewmodelLayer(
        const VR* vr,
        const Vector& scenePoint,
        Vector& outViewmodelPoint);
    bool MagazineInteractionPlaySyntheticSound(
        VR* vr,
        const std::string& rawSample,
        std::string& pendingSample,
        std::chrono::steady_clock::time_point& pendingStarted,
        const char* label);
    int MagazineInteractionFindClientEntityIndex(Game* game, const void* entity);

    bool TryReadOpenVRSkeletalSummary(
        vr::IVRInput* input,
        vr::VRActionHandle_t action,
        vr::EVRSummaryType summaryType,
        vr::VRSkeletalSummaryData_t& outSummary,
        const char* label)
    {
        outSummary = {};
        if (!input || action == vr::k_ulInvalidActionHandle)
            return false;

        vr::EVRInputError result = vr::VRInputError_None;
        bool faulted = false;
        __try
        {
            result = input->GetSkeletalSummaryData(action, summaryType, &outSummary);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }

        if (faulted)
        {
            static DWORD s_lastFaultLogTick = 0;
            const DWORD now = GetTickCount();
            if (now - s_lastFaultLogTick >= 1000u)
            {
                s_lastFaultLogTick = now;
                Game::logMsg(
                    "[VR][OpenVR] GetSkeletalSummaryData faulted for %s; ignoring skeletal curl this frame",
                    (label && label[0] != '\0') ? label : "unknown action");
            }
            return false;
        }

        return result == vr::VRInputError_None;
    }

    bool ReadOpenVRSkeletalFingerCurls(
        vr::IVRInput* input,
        vr::IVRSystem* system,
        const l4d2vr_input::ActionFrame& actionFrame,
        vr::TrackedDeviceIndex_t expectedDevice,
        const char* sourcePath,
        vr::VRActionHandle_t& action,
        const char* actionPath,
        std::array<float, 5>& outCurls,
        const char* label)
    {
        outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        const auto frame = actionFrame.Read();
        if (!input || !system || !sourcePath || !l4d2vr_input::ActionFrame::Ready(frame) || !actionPath || actionPath[0] == '\0')
            return false;

        if (action == vr::k_ulInvalidActionHandle)
        {
            if (input->GetActionHandle(actionPath, &action) != vr::VRInputError_None ||
                action == vr::k_ulInvalidActionHandle)
            {
                return false;
            }
        }

        vr::InputSkeletalActionData_t actionData{};
        vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle;
        if (input->GetSkeletalActionData(action, &actionData, sizeof(actionData)) != vr::VRInputError_None ||
            !actionData.bActive ||
            input->GetInputSourceHandle(sourcePath, &source) != vr::VRInputError_None ||
            !l4d2vr_hand_input::ValidateOrigin(input, system, actionData.activeOrigin, source, expectedDevice))
        {
            return false;
        }

        vr::VRSkeletalSummaryData_t summary{};
        if (!TryReadOpenVRSkeletalSummary(
                input,
                action,
                vr::VRSummaryType_FromAnimation,
                summary,
                label) || !l4d2vr_skeletal::ValidSummaryCurls(summary))
        {
            return false;
        }

        for (int finger = 0; finger < 5; ++finger)
            outCurls[static_cast<size_t>(finger)] = summary.flFingerCurl[finger];

        if (!actionFrame.Matches(frame))
        {
            outCurls = {};
            return false;
        }
        return true;
    }

    constexpr std::uint32_t kVrHandsLeftTargetDebugBoxMaxAgeMs = 500u;
    constexpr float kVrHandsLeftTargetDebugBoxHalfExtentMeters = 0.05f;

    int FindVrHandsSnapshotBoneIndex(const VrHandVmPose::Snapshot& snapshot, const char* boneName)
    {
        if (!boneName || boneName[0] == '\0')
            return -1;

        const size_t maxBones = std::min(snapshot.boneNames.size(), snapshot.boneWorldMatrices.size());
        for (size_t i = 0; i < maxBones; ++i)
        {
            if (snapshot.boneNames[i] == boneName)
                return static_cast<int>(i);
        }
        return -1;
    }

    bool TryBuildLeftHandTargetDebugBox(
        float sourceUnitsPerMeter,
        float targetBoxScale,
        VrHandMatrix4& outWorld,
        Vector& outMins,
        Vector& outMaxs)
    {
        VrHandVmPose::Snapshot snapshot{};
        if (!VrHandVmPose::GetLatest(snapshot, kVrHandsLeftTargetDebugBoxMaxAgeMs))
            return false;

        static const char* kLeftMiddleFingerBones[] =
        {
            "ValveBiped.Bip01_L_Finger2",
            "ValveBiped.Bip01_L_Finger21",
            "ValveBiped.Bip01_L_Finger22",
        };

        int orientationBone = -1;
        Vector center(0.0f, 0.0f, 0.0f);
        int centerBoneCount = 0;
        for (const char* boneName : kLeftMiddleFingerBones)
        {
            const int bone = FindVrHandsSnapshotBoneIndex(snapshot, boneName);
            if (bone < 0 || bone >= static_cast<int>(snapshot.boneWorldMatrices.size()))
                continue;

            if (orientationBone < 0)
                orientationBone = bone;

            const VrHandMatrix4& boneWorld = snapshot.boneWorldMatrices[static_cast<size_t>(bone)];
            center += Vector(
                VrHandMath::Get(boneWorld, 0, 3),
                VrHandMath::Get(boneWorld, 1, 3),
                VrHandMath::Get(boneWorld, 2, 3));
            ++centerBoneCount;
        }

        if (orientationBone >= 0 && centerBoneCount > 0)
        {
            center *= 1.0f / static_cast<float>(centerBoneCount);
            outWorld = snapshot.boneWorldMatrices[static_cast<size_t>(orientationBone)];
            VrHandMath::Set(outWorld, 0, 3, center.x);
            VrHandMath::Set(outWorld, 1, 3, center.y);
            VrHandMath::Set(outWorld, 2, 3, center.z);
        }
        else
        {
            const int handBone = FindVrHandsSnapshotBoneIndex(snapshot, "ValveBiped.Bip01_L_Hand");
            if (handBone < 0 || handBone >= static_cast<int>(snapshot.boneWorldMatrices.size()))
                return false;
            outWorld = snapshot.boneWorldMatrices[static_cast<size_t>(handBone)];
        }

        const float halfExtent = std::max(
            0.1f,
            kVrHandsLeftTargetDebugBoxHalfExtentMeters *
                std::clamp(targetBoxScale, 0.25f, 8.0f) *
                sourceUnitsPerMeter);
        outMins = Vector(-halfExtent, -halfExtent, -halfExtent);
        outMaxs = Vector(halfExtent, halfExtent, halfExtent);
        return true;
    }

    bool TryBuildMountFriendlyTwoHandedGripBox(
        const VR* vr,
        float sourceUnitsPerMeter,
        const std::chrono::steady_clock::time_point& now,
        MagazineInteractionBoxSnapshot& outBox)
    {
        if (!vr ||
            !(sourceUnitsPerMeter > 0.001f) ||
            !std::isfinite(sourceUnitsPerMeter))
        {
            return false;
        }

        Vector forward = VrHandMath::Normalize(vr->m_RightControllerForward);
        Vector right = VrHandMath::Normalize(vr->m_RightControllerRight);
        Vector up = VrHandMath::Normalize(vr->m_RightControllerUp);
        if (forward.IsZero() || right.IsZero() || up.IsZero())
        {
            QAngle::AngleVectors(vr->m_RightControllerAngAbs, &forward, &right, &up);
            forward = VrHandMath::Normalize(forward);
            right = VrHandMath::Normalize(right);
            up = VrHandMath::Normalize(up);
            if (forward.IsZero() || right.IsZero() || up.IsZero())
                return false;
        }

        Vector viewmodelOrigin{};
        if (!MagazineInteractionReprojectScenePointToViewmodelLayer(
                vr,
                vr->m_RightControllerPosAbs,
                viewmodelOrigin))
        {
            return false;
        }

        constexpr float kLengthMeters = 0.50f;
        constexpr float kWidthMeters = 0.20f;
        constexpr float kHeightMeters = 0.20f;
        const float halfLength = 0.5f * kLengthMeters * sourceUnitsPerMeter;
        const float halfWidth = 0.5f * kWidthMeters * sourceUnitsPerMeter;
        const float halfHeight = 0.5f * kHeightMeters * sourceUnitsPerMeter;

        outBox = MagazineInteractionBoxSnapshot{};
        outBox.origin = viewmodelOrigin + forward * halfLength;
        outBox.axisX = right;
        outBox.axisY = up;
        outBox.axisZ = forward;
        outBox.mins = Vector(-halfWidth, -halfHeight, -halfLength);
        outBox.maxs = Vector(halfWidth, halfHeight, halfLength);
        outBox.modelName = "two_hand_mount_friendly_target";
        outBox.publishedAt = now;
        return true;
    }

    class ScopedVrHandsQueuedD3DLock
    {
    public:
        explicit ScopedVrHandsQueuedD3DLock(bool enabled)
        {
            if (enabled && g_D3DVR9 && SUCCEEDED(g_D3DVR9->LockDevice()))
                m_Locked = true;
        }

        ~ScopedVrHandsQueuedD3DLock()
        {
            if (m_Locked && g_D3DVR9)
                g_D3DVR9->UnlockDevice();
        }

        ScopedVrHandsQueuedD3DLock(const ScopedVrHandsQueuedD3DLock&) = delete;
        ScopedVrHandsQueuedD3DLock& operator=(const ScopedVrHandsQueuedD3DLock&) = delete;

    private:
        bool m_Locked = false;
    };

    class ScopedVrHandsD3DTarget
    {
    public:
        ScopedVrHandsD3DTarget(
            IDirect3DDevice9* device,
            IDirect3DSurface9* target,
            uint32_t width,
            uint32_t height,
            bool forceBindTarget)
            : m_Device(device),
            m_ForceBindTarget(forceBindTarget)
        {
            if (!m_Device || !target || !m_ForceBindTarget)
                return;

            if (SUCCEEDED(m_Device->GetRenderTarget(0, &m_OldRenderTarget)))
                m_HaveOldRenderTarget = true;
            m_HaveOldViewport = SUCCEEDED(m_Device->GetViewport(&m_OldViewport));

            if (FAILED(m_Device->SetRenderTarget(0, target)))
                return;

            if (width > 0 && height > 0)
            {
                D3DVIEWPORT9 viewport{};
                viewport.X = 0;
                viewport.Y = 0;
                viewport.Width = width;
                viewport.Height = height;
                viewport.MinZ = 0.0f;
                viewport.MaxZ = 1.0f;
                m_Device->SetViewport(&viewport);
            }

            m_Bound = true;
        }

        ~ScopedVrHandsD3DTarget()
        {
            if (!m_Device || !m_ForceBindTarget)
                return;

            if (m_Bound)
            {
                if (m_HaveOldRenderTarget)
                    m_Device->SetRenderTarget(0, m_OldRenderTarget);
                if (m_HaveOldViewport)
                    m_Device->SetViewport(&m_OldViewport);
            }
            if (m_OldRenderTarget)
                m_OldRenderTarget->Release();
        }

        bool IsBound() const { return !m_ForceBindTarget || m_Bound; }

        ScopedVrHandsD3DTarget(const ScopedVrHandsD3DTarget&) = delete;
        ScopedVrHandsD3DTarget& operator=(const ScopedVrHandsD3DTarget&) = delete;

    private:
        IDirect3DDevice9* m_Device = nullptr;
        IDirect3DSurface9* m_OldRenderTarget = nullptr;
        D3DVIEWPORT9 m_OldViewport{};
        bool m_ForceBindTarget = false;
        bool m_HaveOldRenderTarget = false;
        bool m_HaveOldViewport = false;
        bool m_Bound = false;
    };

    class ScopedVrHandsRenderSnapshot
    {
    public:
        explicit ScopedVrHandsRenderSnapshot(bool enabled)
            : m_Enabled(enabled),
            m_Previous(VR::t_UseRenderFrameSnapshot)
        {
            if (m_Enabled)
                VR::t_UseRenderFrameSnapshot = true;
        }

        ~ScopedVrHandsRenderSnapshot()
        {
            if (m_Enabled)
                VR::t_UseRenderFrameSnapshot = m_Previous;
        }

        ScopedVrHandsRenderSnapshot(const ScopedVrHandsRenderSnapshot&) = delete;
        ScopedVrHandsRenderSnapshot& operator=(const ScopedVrHandsRenderSnapshot&) = delete;

    private:
        bool m_Enabled = false;
        bool m_Previous = false;
    };

    Vector MagazineInteractionNormalizeAxis(const Vector& axis, const Vector& fallback)
    {
        const Vector normalized = VrHandMath::Normalize(axis);
        return normalized.Length() > 0.0001f ? normalized : fallback;
    }

    int MagazineInteractionDominantAxis(const Vector& value)
    {
        const float ax = std::fabs(value.x);
        const float ay = std::fabs(value.y);
        const float az = std::fabs(value.z);
        if (ay >= ax && ay >= az)
            return 1;
        if (az >= ax && az >= ay)
            return 2;
        return 0;
    }

    float MagazineInteractionDistanceToBoxSourceUnits(
        const MagazineInteractionBoxSnapshot& box,
        const Vector& worldPoint)
    {
        return l4d2vr_interaction::OrientedBoxDistance(worldPoint, box.origin,
            box.axisX, box.axisY, box.axisZ, box.mins, box.maxs);
    }

    float MagazineInteractionResolveAspect(float preferred, uint32_t width, uint32_t height)
    {
        if (preferred > 0.001f)
            return preferred;
        if (height > 0)
            return static_cast<float>(width) / static_cast<float>(height);
        return 1.0f;
    }

    float MagazineInteractionProjectionXScale(float horizontalFovDeg)
    {
        constexpr float kPi = 3.14159265358979323846f;
        const float fov = std::clamp(horizontalFovDeg, 1.0f, 179.0f) * kPi / 180.0f;
        return 1.0f / std::tan(fov * 0.5f);
    }

    bool MagazineInteractionResolveViewFrame(
        const VR* vr,
        Vector& outViewOrigin,
        Vector& outForward,
        Vector& outRight,
        Vector& outUp)
    {
        if (!vr)
            return false;

        VR* mutableVr = const_cast<VR*>(vr);
        const Vector viewAngles = mutableVr->GetViewAngle();
        const Vector viewLeft = mutableVr->GetViewOriginLeft();
        const Vector viewRight = mutableVr->GetViewOriginRight();

        QAngle eyeAngles(viewAngles.x, viewAngles.y, viewAngles.z);
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(eyeAngles, &forward, &right, &up);
        forward = VrHandMath::Normalize(forward);
        right = VrHandMath::Normalize(right);
        up = VrHandMath::Normalize(up);
        if (forward.Length() <= 0.0001f || right.Length() <= 0.0001f || up.Length() <= 0.0001f)
            return false;

        const Vector viewOrigin = (viewLeft + viewRight) * 0.5f;
        if (!std::isfinite(viewOrigin.x) || !std::isfinite(viewOrigin.y) || !std::isfinite(viewOrigin.z))
            return false;

        outViewOrigin = viewOrigin;
        outForward = forward;
        outRight = right;
        outUp = up;
        return true;
    }

    bool MagazineInteractionReprojectScenePointToViewmodelLayer(
        const VR* vr,
        const Vector& scenePoint,
        Vector& outViewmodelPoint)
    {
        if (!vr)
            return false;

        const float sceneFov = (vr->m_Fov > 0.001f) ? vr->m_Fov : 90.0f;
        const float viewmodelFov = sceneFov;
        const float sceneAspect = MagazineInteractionResolveAspect(
            vr->m_Aspect,
            vr->m_RenderWidth,
            vr->m_RenderHeight);
        const float viewmodelAspect = (vr->m_RenderHeight > 0)
            ? static_cast<float>(vr->m_RenderWidth) / static_cast<float>(vr->m_RenderHeight)
            : sceneAspect;

        const float sceneXScale = MagazineInteractionProjectionXScale(sceneFov);
        const float viewmodelXScale = MagazineInteractionProjectionXScale(viewmodelFov);
        const float sceneYScale = sceneXScale * sceneAspect;
        const float viewmodelYScale = viewmodelXScale * viewmodelAspect;
        if (!(viewmodelXScale > 0.0001f) || !(viewmodelYScale > 0.0001f))
            return false;

        Vector viewOrigin{};
        Vector forward{};
        Vector right{};
        Vector up{};
        if (!MagazineInteractionResolveViewFrame(vr, viewOrigin, forward, right, up))
            return false;

        const Vector delta = scenePoint - viewOrigin;
        const float viewX = VrHandMath::Dot(delta, right);
        const float viewY = VrHandMath::Dot(delta, up);
        const float viewZ = VrHandMath::Dot(delta, forward);
        if (!std::isfinite(viewX) || !std::isfinite(viewY) || !std::isfinite(viewZ))
            return false;

        outViewmodelPoint =
            viewOrigin +
            right * (viewX * (sceneXScale / viewmodelXScale)) +
            up * (viewY * (sceneYScale / viewmodelYScale)) +
            forward * viewZ;
        return true;
    }

    float MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
        const MagazineInteractionBoxSnapshot& box,
        const Vector& controllerOrigin,
        const QAngle& controllerAngles,
        float sourceUnitsPerMeter,
        const VR* viewmodelReprojectVr = nullptr)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(controllerAngles, &forward, &right, &up);
        forward = VrHandMath::Normalize(forward);
        right = VrHandMath::Normalize(right);
        up = VrHandMath::Normalize(up);

        const float nearOffset = 0.06f * sourceUnitsPerMeter;
        const float farOffset = 0.12f * sourceUnitsPerMeter;
        const Vector probes[] =
        {
            controllerOrigin,
            controllerOrigin + forward * nearOffset,
            controllerOrigin - forward * nearOffset,
            controllerOrigin + right * nearOffset,
            controllerOrigin - right * nearOffset,
            controllerOrigin + up * nearOffset,
            controllerOrigin - up * nearOffset,
            controllerOrigin + forward * farOffset,
            controllerOrigin - forward * farOffset,
            controllerOrigin + up * farOffset,
            controllerOrigin - up * farOffset
        };

        float nearest = FLT_MAX;
        for (const Vector& probe : probes)
        {
            Vector testPoint = probe;
            if (viewmodelReprojectVr)
            {
                Vector reprojected{};
                if (MagazineInteractionReprojectScenePointToViewmodelLayer(viewmodelReprojectVr, probe, reprojected))
                    testPoint = reprojected;
            }
            nearest = std::min(nearest, MagazineInteractionDistanceToBoxSourceUnits(box, testPoint));
        }
        return nearest;
    }

    VrHandMatrix4 MagazineInteractionBuildBoxWorld(const MagazineInteractionBoxSnapshot& box)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, box.axisX.x);
        VrHandMath::Set(out, 1, 0, box.axisX.y);
        VrHandMath::Set(out, 2, 0, box.axisX.z);
        VrHandMath::Set(out, 0, 1, box.axisY.x);
        VrHandMath::Set(out, 1, 1, box.axisY.y);
        VrHandMath::Set(out, 2, 1, box.axisY.z);
        VrHandMath::Set(out, 0, 2, box.axisZ.x);
        VrHandMath::Set(out, 1, 2, box.axisZ.y);
        VrHandMath::Set(out, 2, 2, box.axisZ.z);
        VrHandMath::Set(out, 0, 3, box.origin.x);
        VrHandMath::Set(out, 1, 3, box.origin.y);
        VrHandMath::Set(out, 2, 3, box.origin.z);
        return out;
    }

    VrHandMatrix4 MagazineInteractionBuildControllerWorldFromAxes(
        const Vector& origin,
        const Vector& forwardIn,
        const Vector& rightIn,
        const Vector& upIn)
    {
        Vector forward = VrHandMath::Normalize(forwardIn);
        Vector right = VrHandMath::Normalize(rightIn);
        Vector up = VrHandMath::Normalize(upIn);
        if (forward.Length() <= 0.0001f)
            forward = Vector(1.0f, 0.0f, 0.0f);
        if (right.Length() <= 0.0001f)
            right = Vector(0.0f, -1.0f, 0.0f);
        if (up.Length() <= 0.0001f)
            up = Vector(0.0f, 0.0f, 1.0f);

        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, right.x);
        VrHandMath::Set(out, 1, 0, right.y);
        VrHandMath::Set(out, 2, 0, right.z);
        VrHandMath::Set(out, 0, 1, up.x);
        VrHandMath::Set(out, 1, 1, up.y);
        VrHandMath::Set(out, 2, 1, up.z);
        VrHandMath::Set(out, 0, 2, -forward.x);
        VrHandMath::Set(out, 1, 2, -forward.y);
        VrHandMath::Set(out, 2, 2, -forward.z);
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    VrHandMatrix4 MagazineInteractionBuildViewmodelReprojectedControllerWorld(
        const VR* vr,
        const Vector& origin,
        const Vector& forward,
        const Vector& right,
        const Vector& up)
    {
        Vector reprojectedOrigin = origin;
        Vector candidate{};
        if (MagazineInteractionReprojectScenePointToViewmodelLayer(vr, origin, candidate))
            reprojectedOrigin = candidate;

        return MagazineInteractionBuildControllerWorldFromAxes(
            reprojectedOrigin,
            forward,
            right,
            up);
    }

    float MagazineInteractionWrapDegrees(float angle)
    {
        if (!std::isfinite(angle))
            return 0.0f;
        angle -= 360.0f * std::floor((angle + 180.0f) / 360.0f);
        return angle;
    }

    bool MagazineInteractionBuildHorizontalBasisFromYaw(
        float yawDegrees,
        Vector& outForward,
        Vector& outRight)
    {
        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector bodyForward{};
        Vector unusedRight{};
        Vector unusedUp{};
        QAngle::AngleVectors(QAngle(0.0f, yawDegrees, 0.0f), &bodyForward, &unusedRight, &unusedUp);
        bodyForward.z = 0.0f;
        bodyForward = VrHandMath::Normalize(bodyForward);
        if (bodyForward.Length() <= 0.0001f)
            bodyForward = Vector(1.0f, 0.0f, 0.0f);

        Vector bodyRight(
            bodyForward.y * worldUp.z - bodyForward.z * worldUp.y,
            bodyForward.z * worldUp.x - bodyForward.x * worldUp.z,
            bodyForward.x * worldUp.y - bodyForward.y * worldUp.x);
        bodyRight = VrHandMath::Normalize(bodyRight);
        if (bodyRight.Length() <= 0.0001f)
            bodyRight = Vector(0.0f, -1.0f, 0.0f);

        outForward = bodyForward;
        outRight = bodyRight;
        return true;
    }

    bool MagazineInteractionBuildFreshPickupBodyBasis(
        const VR* vr,
        float hmdYawOffsetDeg,
        Vector& outForward,
        Vector& outRight)
    {
        if (!vr)
            return false;

        return MagazineInteractionBuildHorizontalBasisFromYaw(
            vr->m_RotationOffset + hmdYawOffsetDeg,
            outForward,
            outRight);
    }

    bool MagazineInteractionComputeFreshPickupHmdYawOffset(
        const VR* vr,
        float& outOffsetDeg)
    {
        if (!vr)
            return false;

        Vector bodyForward{};
        Vector bodyRight{};
        if (!MagazineInteractionBuildHorizontalBasisFromYaw(vr->m_RotationOffset, bodyForward, bodyRight))
            return false;

        Vector hmdForward = vr->m_HmdForward;
        hmdForward.z = 0.0f;
        hmdForward = VrHandMath::Normalize(hmdForward);
        if (hmdForward.Length() <= 0.0001f)
            return false;

        const float dot = std::clamp(DotProduct(bodyForward, hmdForward), -1.0f, 1.0f);
        const float crossZ = bodyForward.x * hmdForward.y - bodyForward.y * hmdForward.x;
        constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
        outOffsetDeg = MagazineInteractionWrapDegrees(std::atan2(crossZ, dot) * kRadToDeg);
        return true;
    }

    bool MagazineInteractionEnsureFreshPickupBodyBasis(VR* vr)
    {
        if (!vr)
            return false;

        constexpr float kHmdRefreshThresholdDeg = 60.0f;
        float currentHmdYawOffsetDeg = 0.0f;
        const bool hasHmdYawOffset =
            MagazineInteractionComputeFreshPickupHmdYawOffset(vr, currentHmdYawOffsetDeg);
        const bool logFirstCapture = !vr->m_MagazineInteractionFreshPickupBasisValid;
        bool hmdYawRefreshed = false;
        float hmdYawDeltaDeg = 0.0f;
        if (!vr->m_MagazineInteractionFreshPickupBasisValid)
        {
            vr->m_MagazineInteractionFreshPickupHmdYawOffsetDeg =
                hasHmdYawOffset ? currentHmdYawOffsetDeg : 0.0f;
        }
        else if (hasHmdYawOffset)
        {
            hmdYawDeltaDeg = MagazineInteractionWrapDegrees(
                currentHmdYawOffsetDeg - vr->m_MagazineInteractionFreshPickupHmdYawOffsetDeg);
            if (std::fabs(hmdYawDeltaDeg) >= kHmdRefreshThresholdDeg)
            {
                vr->m_MagazineInteractionFreshPickupHmdYawOffsetDeg = currentHmdYawOffsetDeg;
                hmdYawRefreshed = true;
            }
        }

        Vector bodyForward{};
        Vector bodyRight{};
        if (!MagazineInteractionBuildFreshPickupBodyBasis(
            vr,
            vr->m_MagazineInteractionFreshPickupHmdYawOffsetDeg,
            bodyForward,
            bodyRight))
            return false;

        vr->m_MagazineInteractionFreshPickupForward = bodyForward;
        vr->m_MagazineInteractionFreshPickupRight = bodyRight;
        vr->m_MagazineInteractionFreshPickupRotationOffset = vr->m_RotationOffset;
        vr->m_MagazineInteractionFreshPickupBasisValid = true;
        if (logFirstCapture)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] fresh magazine waist pickup basis initialized hmdLatchOffset=%.1f threshold=%.1f forward=(%.2f %.2f %.2f) right=(%.2f %.2f %.2f)",
                vr->m_MagazineInteractionFreshPickupHmdYawOffsetDeg,
                kHmdRefreshThresholdDeg,
                bodyForward.x,
                bodyForward.y,
                bodyForward.z,
                bodyRight.x,
                bodyRight.y,
                bodyRight.z);
        }
        else if (hmdYawRefreshed)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] fresh magazine waist pickup basis refreshed by HMD yaw delta=%.1f newLatchOffset=%.1f threshold=%.1f forward=(%.2f %.2f %.2f) right=(%.2f %.2f %.2f)",
                hmdYawDeltaDeg,
                vr->m_MagazineInteractionFreshPickupHmdYawOffsetDeg,
                kHmdRefreshThresholdDeg,
                bodyForward.x,
                bodyForward.y,
                bodyForward.z,
                bodyRight.x,
                bodyRight.y,
                bodyRight.z);
        }
        return true;
    }

    bool MagazineInteractionResolveFreshPickupBodyAxes(
        const VR* vr,
        Vector& outForward,
        Vector& outRight)
    {
        if (!vr)
            return false;

        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector bodyForward{};
        Vector bodyRight{};
        if (vr->m_MagazineInteractionFreshPickupBasisValid)
        {
            bodyForward = vr->m_MagazineInteractionFreshPickupForward;
            bodyRight = vr->m_MagazineInteractionFreshPickupRight;
        }
        else
        {
            float hmdYawOffsetDeg = 0.0f;
            MagazineInteractionComputeFreshPickupHmdYawOffset(vr, hmdYawOffsetDeg);
            if (!MagazineInteractionBuildFreshPickupBodyBasis(vr, hmdYawOffsetDeg, bodyForward, bodyRight))
                return false;
        }

        bodyForward = VrHandMath::Normalize(bodyForward);
        if (bodyForward.Length() <= 0.0001f)
            bodyForward = Vector(1.0f, 0.0f, 0.0f);

        if (!vr->m_MagazineInteractionFreshPickupBasisValid)
        {
            bodyRight = Vector(
                bodyForward.y * worldUp.z - bodyForward.z * worldUp.y,
                bodyForward.z * worldUp.x - bodyForward.x * worldUp.z,
                bodyForward.x * worldUp.y - bodyForward.y * worldUp.x);
        }
        bodyRight = VrHandMath::Normalize(bodyRight);
        if (bodyRight.Length() <= 0.0001f)
            bodyRight = Vector(0.0f, -1.0f, 0.0f);

        outForward = bodyForward;
        outRight = bodyRight;
        return true;
    }

    Vector MagazineInteractionFreshMagazinePickupOffsetMeters(const VR* vr)
    {
        Vector offset = vr ? vr->m_MagazineInteractionFreshMagazinePickupOffsetMeters : Vector(0.0f, 0.0f, 0.0f);
        if (vr && !vr->IsGameplayHandLeftPhysical(vr->MagazineReloadSupportIsLeft()))
            offset.y = -offset.y;
        return offset;
    }

    bool MagazineInteractionBuildFreshMagazinePickupBox(
        VR* vr,
        MagazineInteractionBoxSnapshot& outBox,
        QAngle& outAngles)
    {
        if (!vr)
            return false;

        MagazineInteractionEnsureFreshPickupBodyBasis(vr);
        Vector bodyForward{};
        Vector bodyRight{};
        if (!MagazineInteractionResolveFreshPickupBodyAxes(vr, bodyForward, bodyRight))
            return false;
        const Vector worldUp(0.0f, 0.0f, 1.0f);

        Vector bodyOrigin = vr->m_HmdPosAbs
            + bodyForward * (vr->m_InventoryBodyOriginOffset.x * vr->m_VRScale)
            + bodyRight * (vr->m_InventoryBodyOriginOffset.y * vr->m_VRScale)
            + worldUp * (vr->m_InventoryBodyOriginOffset.z * vr->m_VRScale);
        if (vr->m_BodyGripInventoryEnabled)
            vr->GetBodyInventoryPose(bodyOrigin, bodyForward, bodyRight);
        const Vector pickupOffsetMeters = MagazineInteractionFreshMagazinePickupOffsetMeters(vr);
        const Vector pickup = bodyOrigin
            + bodyForward * (pickupOffsetMeters.x * vr->m_VRScale)
            + bodyRight * (pickupOffsetMeters.y * vr->m_VRScale)
            + worldUp * (pickupOffsetMeters.z * vr->m_VRScale);
        Vector viewmodelPickup = pickup;
        Vector reprojectedPickup{};
        if (MagazineInteractionReprojectScenePointToViewmodelLayer(vr, pickup, reprojectedPickup))
            viewmodelPickup = reprojectedPickup;

        const Vector half(
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.x) * vr->m_VRScale,
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.y) * vr->m_VRScale,
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.z) * vr->m_VRScale);

        outBox = {};
        outBox.origin = viewmodelPickup;
        outBox.axisX = bodyForward;
        outBox.axisY = bodyRight * -1.0f;
        outBox.axisZ = worldUp;
        outBox.mins = Vector(-half.x, -half.y, -half.z);
        outBox.maxs = Vector(half.x, half.y, half.z);
        outBox.publishedAt = std::chrono::steady_clock::now();

        QAngle::VectorAngles(bodyForward, outAngles);
        outAngles.x = 0.0f;
        outAngles.z = 0.0f;
        return true;
    }

    VrHandMatrix4 MagazineInteractionBuildLocalTransform(
        float sourceUnitsPerMeter,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        VrHandMatrix4 local = VrHandMath::Identity();
        VrHandMath::Set(local, 0, 0, cz * cy);
        VrHandMath::Set(local, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(local, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(local, 1, 0, sz * cy);
        VrHandMath::Set(local, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(local, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(local, 2, 0, -sy);
        VrHandMath::Set(local, 2, 1, cy * sx);
        VrHandMath::Set(local, 2, 2, cy * cx);
        VrHandMath::Set(local, 0, 3, localPositionOffsetMeters.x * sourceUnitsPerMeter);
        VrHandMath::Set(local, 1, 3, localPositionOffsetMeters.y * sourceUnitsPerMeter);
        VrHandMath::Set(local, 2, 3, localPositionOffsetMeters.z * sourceUnitsPerMeter);
        return local;
    }

    Vector MagazineInteractionMatrixOrigin(const VrHandMatrix4& matrix);
    Vector MagazineInteractionMatrixLocalVectorToWorld(
        const VrHandMatrix4& matrix,
        const Vector& localVector);
    bool MagazineInteractionMatrixLooksRenderable(const VrHandMatrix4& matrix);

    VrHandMatrix4 MagazineInteractionBuildFreshHandMagazineWorld(const VR* vr)
    {
        if (!vr)
            return VrHandMath::Identity();

        const bool supportLeft = vr->MagazineReloadSupportIsLeft();
        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
            vr,
            supportLeft ? vr->m_LeftControllerPosAbs : vr->m_RightControllerPosAbs,
            supportLeft ? vr->m_LeftControllerForward : vr->m_RightControllerForward,
            supportLeft ? vr->m_LeftControllerRight : vr->m_RightControllerRight,
            supportLeft ? vr->m_LeftControllerUp : vr->m_RightControllerUp);
        const VrHandMatrix4 local = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            vr->m_MagazineInteractionMagazineHandOffsetMeters,
            vr->m_MagazineInteractionMagazineHandRotationOffsetDeg);
        return VrHandMath::Multiply(controllerWorld, local);
    }

    VrHandMatrix4 MagazineInteractionBuildNativeLeftHandWorldFromController(
        const VR* vr,
        const VrHandMatrix4& controllerWorld)
    {
        if (!vr)
            return controllerWorld;

        const bool left = vr->MagazineReloadSupportIsLeft();

        if (vr->m_VrHandsEnabled)
        {
            // Use the glove renderer's GLB Euler convention, not Source's
            // pitch/yaw/roll convention. Remove model scale from the anchor.
            return VrHandMath::BuildControllerWorld(MagazineInteractionMatrixOrigin(controllerWorld),
                left ? const_cast<VR*>(vr)->GetLeftControllerAbsAngle() : const_cast<VR*>(vr)->GetRightControllerAbsAngle(), vr->m_VRScale,
                1.0f / std::max(0.001f, vr->m_VRScale), left ? vr->m_VrHandsLeftPoseOffsetMeters : vr->m_VrHandsRightPoseOffsetMeters,
                left ? vr->m_VrHandsLeftPoseRotationOffsetDeg : vr->m_VrHandsRightPoseRotationOffsetDeg);
        }

        const VrHandMatrix4 handLocal = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            left ? vr->m_NativeViewmodelLeftHandPoseOffsetMeters : vr->m_NativeViewmodelRightHandPoseOffsetMeters,
            left ? vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg : vr->m_NativeViewmodelRightHandPoseRotationOffsetDeg);
        VrHandMatrix4 handWorld = VrHandMath::Multiply(controllerWorld, handLocal);

        const VrHandMatrix4 magazineOffsetLocal = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            vr->m_MagazineInteractionMagazineHandOffsetMeters,
            Vector(0.0f, 0.0f, 0.0f));
        handWorld = VrHandMath::Multiply(handWorld, magazineOffsetLocal);
        return handWorld;
    }

    Vector MagazineInteractionNativeLeftHandPalmAnchorWorld(
        const VR* vr,
        const VrHandMatrix4& nativeLeftHandWorld)
    {
        const float scale = vr ? vr->m_VRScale : 1.0f;
        Vector palmLocalMeters(0.0f, 0.0f, 0.07f);
        if (vr && !vr->m_VrHandsEnabled)
            palmLocalMeters += vr->m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters;
        return MagazineInteractionMatrixOrigin(nativeLeftHandWorld) +
            MagazineInteractionMatrixLocalVectorToWorld(
                nativeLeftHandWorld,
                palmLocalMeters * scale);
    }

    Vector MagazineInteractionFreshMagazineWristAnchorOffsetWorld(
        const VR* vr,
        const VrHandMatrix4& wristWorld)
    {
        if (!vr)
            return MagazineInteractionMatrixOrigin(wristWorld);

        return MagazineInteractionMatrixOrigin(wristWorld) +
            MagazineInteractionMatrixLocalVectorToWorld(
                wristWorld,
                vr->m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters * vr->m_VRScale);
    }

    bool MagazineInteractionResolveFreshMagazineHandAnchorWorld(
        const VR* vr,
        const VrHandMatrix4& controllerWorld,
        Vector& outAnchorWorld,
        bool& outUsedWristAnchor)
    {
        outUsedWristAnchor = false;
        if (!vr)
        {
            outAnchorWorld = MagazineInteractionMatrixOrigin(controllerWorld);
            return false;
        }

        const VrHandMatrix4 nativeLeftHandWorld =
            MagazineInteractionBuildNativeLeftHandWorldFromController(vr, controllerWorld);
        if (MagazineInteractionMatrixLooksRenderable(nativeLeftHandWorld))
        {
            outAnchorWorld =
                MagazineInteractionNativeLeftHandPalmAnchorWorld(vr, nativeLeftHandWorld);
            return true;
        }

        VrHandMatrix4 wristWorld{};
        if (vr->MagazineReloadSupportIsLeft() && vr->GetMagazineInteractionNativeLeftWristAnchor(wristWorld))
        {
            outAnchorWorld =
                MagazineInteractionFreshMagazineWristAnchorOffsetWorld(vr, wristWorld);
            outUsedWristAnchor = true;
            return true;
        }

        outAnchorWorld = MagazineInteractionMatrixOrigin(controllerWorld);
        return false;
    }

    bool MagazineInteractionMatrixBasisLooksValid(const VrHandMatrix4& matrix)
    {
        for (int column = 0; column < 3; ++column)
        {
            const Vector axis(
                VrHandMath::Get(matrix, 0, column),
                VrHandMath::Get(matrix, 1, column),
                VrHandMath::Get(matrix, 2, column));
            const float length = axis.Length();
            if (!std::isfinite(length) || length < 0.001f || length > 100.0f)
                return false;
        }
        return true;
    }

    bool MagazineInteractionMatrixLooksRenderable(const VrHandMatrix4& matrix)
    {
        for (float value : matrix.m)
        {
            if (!std::isfinite(value))
                return false;
        }

        const Vector origin(
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3));
        if (std::fabs(origin.x) > 1000000.0f ||
            std::fabs(origin.y) > 1000000.0f ||
            std::fabs(origin.z) > 1000000.0f)
        {
            return false;
        }

        for (int column = 0; column < 3; ++column)
        {
            const Vector axis(
                VrHandMath::Get(matrix, 0, column),
                VrHandMath::Get(matrix, 1, column),
                VrHandMath::Get(matrix, 2, column));
            const float length = axis.Length();
            if (!std::isfinite(length) || length < 0.001f || length > 100.0f)
                return false;
        }
        return true;
    }

    VrHandMatrix4 MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(const VR* vr, const Vector& desiredCenter)
    {
        if (!vr)
            return VrHandMath::Identity();

        VrHandMatrix4 out =
            vr->m_MagazineInteractionSocketValid &&
            MagazineInteractionMatrixLooksRenderable(vr->m_MagazineInteractionSocketCaptureWorld)
            ? vr->m_MagazineInteractionSocketCaptureWorld
            : vr->m_MagazineInteractionSocketValid
            ? vr->m_MagazineInteractionSocketWorld
            : VrHandMath::Identity();
        const Vector centerLocal = (vr->m_MagazineInteractionSocketBox.mins +
            vr->m_MagazineInteractionSocketBox.maxs) * 0.5f;
        const Vector axisX = MagazineInteractionNormalizeAxis(
            Vector(VrHandMath::Get(out, 0, 0), VrHandMath::Get(out, 1, 0), VrHandMath::Get(out, 2, 0)),
            Vector(1.0f, 0.0f, 0.0f));
        const Vector axisY = MagazineInteractionNormalizeAxis(
            Vector(VrHandMath::Get(out, 0, 1), VrHandMath::Get(out, 1, 1), VrHandMath::Get(out, 2, 1)),
            Vector(0.0f, 1.0f, 0.0f));
        const Vector axisZ = MagazineInteractionNormalizeAxis(
            Vector(VrHandMath::Get(out, 0, 2), VrHandMath::Get(out, 1, 2), VrHandMath::Get(out, 2, 2)),
            Vector(0.0f, 0.0f, 1.0f));
        const Vector targetClipOrigin =
            desiredCenter -
            axisX * centerLocal.x -
            axisY * centerLocal.y -
            axisZ * centerLocal.z;
        VrHandMath::Set(out, 0, 3, targetClipOrigin.x);
        VrHandMath::Set(out, 1, 3, targetClipOrigin.y);
        VrHandMath::Set(out, 2, 3, targetClipOrigin.z);
        return out;
    }

    VrHandMatrix4 MagazineInteractionBuildFreshSocketOrientedMagazineWorld(const VR* vr)
    {
        if (!vr)
            return VrHandMath::Identity();

        const VrHandMatrix4 handPlaced = MagazineInteractionBuildFreshHandMagazineWorld(vr);
        return MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(
            vr,
            Vector(
                VrHandMath::Get(handPlaced, 0, 3),
                VrHandMath::Get(handPlaced, 1, 3),
                VrHandMath::Get(handPlaced, 2, 3)));
    }

    Vector MagazineInteractionMatrixOrigin(const VrHandMatrix4& matrix)
    {
        return Vector(
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3));
    }

    Vector MagazineInteractionMatrixAxis(const VrHandMatrix4& matrix, int column)
    {
        return VrHandMath::Normalize(Vector(
            VrHandMath::Get(matrix, 0, column),
            VrHandMath::Get(matrix, 1, column),
            VrHandMath::Get(matrix, 2, column)));
    }

    Vector MagazineInteractionWorldVectorToMatrixLocal(
        const VrHandMatrix4& matrix,
        const Vector& worldVector)
    {
        return Vector(
            VrHandMath::Dot(worldVector, MagazineInteractionMatrixAxis(matrix, 0)),
            VrHandMath::Dot(worldVector, MagazineInteractionMatrixAxis(matrix, 1)),
            VrHandMath::Dot(worldVector, MagazineInteractionMatrixAxis(matrix, 2)));
    }

    Vector MagazineInteractionMatrixLocalVectorToWorld(
        const VrHandMatrix4& matrix,
        const Vector& localVector)
    {
        return
            MagazineInteractionMatrixAxis(matrix, 0) * localVector.x +
            MagazineInteractionMatrixAxis(matrix, 1) * localVector.y +
            MagazineInteractionMatrixAxis(matrix, 2) * localVector.z;
    }

    VrHandMatrix4 MagazineInteractionMatrixWithOrigin(const VrHandMatrix4& matrix, const Vector& origin)
    {
        VrHandMatrix4 out = matrix;
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    Vector MagazineInteractionBoxCenterLocal(const MagazineInteractionBoxSnapshot& box);
    Vector MagazineInteractionMatrixPointWorld(const VrHandMatrix4& matrix, const Vector& localPoint);

    int MagazineInteractionResolveWeaponIdForConfig(const VR* vr)
    {
        if (!vr)
            return 0;

        if (vr->m_MagazineInteractionWeaponId > 0)
            return vr->m_MagazineInteractionWeaponId;

        return vr->m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
    }

    int MagazineInteractionInferWeaponIdFromModelName(const std::string& modelName)
    {
        std::string lowerModel = modelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const int stockId = l4d2vr_weapon_models::StockWeaponId(lowerModel);
        if (stockId > 0) return stockId;
        if (lowerModel.find("shotgun_chrome") != std::string::npos ||
            lowerModel.find("chrome_shotgun") != std::string::npos)
        {
            return static_cast<int>(C_WeaponCSBase::WeaponID::SHOTGUN_CHROME);
        }
        if (lowerModel.find("pumpshotgun") != std::string::npos ||
            lowerModel.find("pump_shotgun") != std::string::npos)
        {
            return static_cast<int>(C_WeaponCSBase::WeaponID::PUMPSHOTGUN);
        }
        if (lowerModel.find("autoshotgun") != std::string::npos ||
            lowerModel.find("auto_shotgun") != std::string::npos)
        {
            return static_cast<int>(C_WeaponCSBase::WeaponID::AUTOSHOTGUN);
        }
        if (lowerModel.find("shotgun_spas") != std::string::npos ||
            lowerModel.find("spas") != std::string::npos)
        {
            return static_cast<int>(C_WeaponCSBase::WeaponID::SPAS);
        }
        return 0;
    }

    template <typename T>
    bool MagazineInteractionFindWeaponOverride(
        const VR* vr,
        const std::unordered_map<int, T>& overrides,
        T& outValue,
        int& outMatchedWeaponId)
    {
        outMatchedWeaponId = 0;
        if (!vr || overrides.empty())
            return false;

        const int weaponId = MagazineInteractionResolveWeaponIdForConfig(vr);
        const int inferredWeaponId = MagazineInteractionInferWeaponIdFromModelName(vr->m_MagazineInteractionMagazineModelName);
        const int candidates[] = { weaponId, inferredWeaponId };
        for (int candidate : candidates)
        {
            if (candidate <= 0)
                continue;
            if (candidate != weaponId && candidate == candidates[0])
                continue;

            const auto it = overrides.find(candidate);
            if (it == overrides.end())
                continue;

            outValue = it->second;
            outMatchedWeaponId = candidate;
            return true;
        }
        return false;
    }

    std::string MagazineInteractionCurrentProfileKey(const VR* vr)
    {
        if (!vr)
            return std::string();

        const uint32_t modelFingerprint =
            vr->m_MagazineInteractionCurrentModelFingerprint.load(std::memory_order_relaxed);
        const uint32_t boneSignature =
            vr->m_MagazineInteractionCurrentBoneSignature.load(std::memory_order_relaxed);
        (void)modelFingerprint;
        if (boneSignature == 0)
            return std::string();

        char text[16] = {};
        std::snprintf(text, sizeof(text), "bs%08x", boneSignature);
        return std::string(text);
    }

    template <typename T>
    bool MagazineInteractionFindProfileOverride(
        const VR* vr,
        const std::unordered_map<std::string, T>& profileOverrides,
        T& outValue)
    {
        if (!vr || profileOverrides.empty())
            return false;

        const std::string profileKey = MagazineInteractionCurrentProfileKey(vr);
        if (profileKey.empty())
            return false;

        const auto it = profileOverrides.find(profileKey);
        if (it == profileOverrides.end())
            return false;

        outValue = it->second;
        return true;
    }

    template <typename T>
    bool MagazineInteractionFindProfileOrWeaponOverride(
        const VR* vr,
        const std::unordered_map<std::string, T>& profileOverrides,
        const std::unordered_map<int, T>& weaponOverrides,
        T& outValue)
    {
        if (MagazineInteractionFindProfileOverride(vr, profileOverrides, outValue))
            return true;

        int matchedWeaponId = 0;
        return MagazineInteractionFindWeaponOverride(vr, weaponOverrides, outValue, matchedWeaponId);
    }

    Vector MagazineInteractionResolveSocketCaptureBoxHalfExtentsMeters(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides,
            value);

        value.x = std::clamp(value.x, 0.0f, 0.50f);
        value.y = std::clamp(value.y, 0.0f, 0.50f);
        value.z = std::clamp(value.z, 0.0f, 0.50f);
        return value;
    }

    Vector MagazineInteractionResolveSocketCaptureBoxLocalOffsetMeters(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides,
            value);

        value.x = std::clamp(value.x, -0.50f, 0.50f);
        value.y = std::clamp(value.y, -0.50f, 0.50f);
        value.z = std::clamp(value.z, -0.50f, 0.50f);
        return value;
    }

    Vector MagazineInteractionResolveSocketCaptureBoxLocalRotationOffsetDeg(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides,
            value);

        value.x = std::clamp(value.x, -180.0f, 180.0f);
        value.y = std::clamp(value.y, -180.0f, 180.0f);
        value.z = std::clamp(value.z, -180.0f, 180.0f);
        return value;
    }

    float MagazineInteractionResolveSocketCaptureAngleDeg(const VR* vr)
    {
        if (!vr)
            return 35.0f;

        float value = vr->m_MagazineInteractionSocketCaptureAngleDeg;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionSocketCaptureAngleDegProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureAngleDegOverrides,
            value);
        return std::clamp(value, 0.0f, 89.0f);
    }

    float MagazineInteractionResolveSocketRequiredDepthMeters(const VR* vr)
    {
        if (!vr)
            return 0.04f;

        float value = vr->m_MagazineInteractionSocketRequiredDepthMeters;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionSocketRequiredDepthMetersProfileOverrides,
            vr->m_MagazineInteractionSocketRequiredDepthMetersOverrides,
            value);
        return std::clamp(value, 0.0f, 0.25f);
    }

    float MagazineInteractionResolveSocketRequiredOverlapFraction(const VR* vr)
    {
        if (!vr)
            return 0.45f;

        float value = vr->m_MagazineInteractionSocketRequiredOverlapFraction;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionSocketRequiredOverlapFractionProfileOverrides,
            vr->m_MagazineInteractionSocketRequiredOverlapFractionOverrides,
            value);
        return std::clamp(value, 0.0f, 1.0f);
    }

    bool MagazineInteractionBuildSocketCaptureBox(
        const VR* vr,
        const MagazineInteractionBoxSnapshot& socketBox,
        MagazineInteractionBoxSnapshot& outBox,
        VrHandMatrix4& outWorld)
    {
        if (!vr)
            return false;

        const VrHandMatrix4 socketWorld = MagazineInteractionBuildBoxWorld(socketBox);
        if (!MagazineInteractionMatrixLooksRenderable(socketWorld))
            return false;

        const Vector centerLocal = MagazineInteractionBoxCenterLocal(socketBox);
        const Vector centerWorld = MagazineInteractionMatrixPointWorld(socketWorld, centerLocal);
        VrHandMatrix4 centerSocketWorld = socketWorld;
        VrHandMath::Set(centerSocketWorld, 0, 3, centerWorld.x);
        VrHandMath::Set(centerSocketWorld, 1, 3, centerWorld.y);
        VrHandMath::Set(centerSocketWorld, 2, 3, centerWorld.z);

        const VrHandMatrix4 local = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            MagazineInteractionResolveSocketCaptureBoxLocalOffsetMeters(vr),
            MagazineInteractionResolveSocketCaptureBoxLocalRotationOffsetDeg(vr));
        outWorld = VrHandMath::Multiply(centerSocketWorld, local);
        if (!MagazineInteractionMatrixLooksRenderable(outWorld))
            return false;

        const Vector sourceHalf = (socketBox.maxs - socketBox.mins) * 0.5f;
        const Vector configuredHalfMeters = MagazineInteractionResolveSocketCaptureBoxHalfExtentsMeters(vr);
        const Vector half(
            (configuredHalfMeters.x > 0.0001f) ? configuredHalfMeters.x * vr->m_VRScale : std::max(0.001f, sourceHalf.x),
            (configuredHalfMeters.y > 0.0001f) ? configuredHalfMeters.y * vr->m_VRScale : std::max(0.001f, sourceHalf.y),
            (configuredHalfMeters.z > 0.0001f) ? configuredHalfMeters.z * vr->m_VRScale : std::max(0.001f, sourceHalf.z));

        outBox = socketBox;
        outBox.origin = MagazineInteractionMatrixOrigin(outWorld);
        outBox.axisX = MagazineInteractionMatrixAxis(outWorld, 0);
        outBox.axisY = MagazineInteractionMatrixAxis(outWorld, 1);
        outBox.axisZ = MagazineInteractionMatrixAxis(outWorld, 2);
        outBox.mins = Vector(-half.x, -half.y, -half.z);
        outBox.maxs = Vector(half.x, half.y, half.z);
        return true;
    }

    Vector MagazineInteractionResolveBoltPullAxisLocal(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 1.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionBoltPullAxisLocal;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionBoltPullAxisLocalProfileOverrides,
            vr->m_MagazineInteractionBoltPullAxisLocalOverrides,
            value);

        return value;
    }

    bool MagazineInteractionHasBoltPullAxisProfileOverride(const VR* vr)
    {
        if (!vr)
            return false;

        Vector unused{};
        return MagazineInteractionFindProfileOverride(
            vr,
            vr->m_MagazineInteractionBoltPullAxisLocalProfileOverrides,
            unused);
    }

    Vector MagazineInteractionBuildBoltInputAxisWorld(const VR* vr, const Vector& fallbackAxis)
    {
        if (!vr)
            return VrHandMath::Normalize(fallbackAxis);
        const bool gunLeft = vr->GetPrimaryPistolHand() == l4d2vr_dual::Hand::Left;
        const Vector forward = gunLeft ? vr->m_LeftControllerForward :
            (vr->m_RightControllerForwardUnforced.IsZero() ? vr->m_RightControllerForward : vr->m_RightControllerForwardUnforced);
        const Vector rear = VrHandMath::Normalize(forward * -1.0f);
        return rear.IsZero() ? VrHandMath::Normalize(fallbackAxis) : rear;
    }

    Vector MagazineInteractionAlignBoltVisualAxisToInputAxis(
        const VR* vr,
        const Vector& visualAxis,
        const Vector& inputAxis)
    {
        Vector visual = VrHandMath::Normalize(visualAxis);
        if (!vr)
            return visual;

        const Vector input = VrHandMath::Normalize(inputAxis);
        if (visual.Length() <= 0.0001f || input.Length() <= 0.0001f)
            return visual;

        return (VrHandMath::Dot(visual, input) < -0.15f) ? (visual * -1.0f) : visual;
    }

    float MagazineInteractionResolveBoltGrabPaddingMeters(const VR* vr)
    {
        if (!vr)
            return 0.10f;

        float value = vr->m_MagazineInteractionBoltGrabPaddingMeters;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionBoltGrabPaddingMetersProfileOverrides,
            vr->m_MagazineInteractionBoltGrabPaddingMetersOverrides,
            value);
        return std::clamp(value, 0.0f, 0.25f);
    }

    float MagazineInteractionResolveBoltPullDistanceMeters(const VR* vr)
    {
        if (!vr)
            return 0.055f;

        float value = vr->m_MagazineInteractionBoltPullDistanceMeters;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionBoltPullDistanceMetersProfileOverrides,
            vr->m_MagazineInteractionBoltPullDistanceMetersOverrides,
            value);
        const auto weaponId = static_cast<C_WeaponCSBase::WeaponID>(MagazineInteractionResolveWeaponIdForConfig(vr));
        const bool pump = weaponId == C_WeaponCSBase::WeaponID::PUMPSHOTGUN ||
            weaponId == C_WeaponCSBase::WeaponID::SHOTGUN_CHROME;
        return l4d2vr_magazine::ResolveBoltPullDistanceMeters(value, pump, vr->m_VRScale);
    }

    float MagazineInteractionResolveBoltReturnDistanceMeters(const VR* vr)
    {
        if (!vr)
            return 0.018f;

        float value = vr->m_MagazineInteractionBoltReturnDistanceMeters;
        MagazineInteractionFindProfileOrWeaponOverride(
            vr,
            vr->m_MagazineInteractionBoltReturnDistanceMetersProfileOverrides,
            vr->m_MagazineInteractionBoltReturnDistanceMetersOverrides,
            value);
        return std::clamp(value, 0.0f, 0.10f);
    }

    Vector MagazineInteractionBuildBoltPullAxisWorld(
        const VR* vr,
        const MagazineInteractionBoxSnapshot& boltBox,
        const VrHandMatrix4& boltRestWorld)
    {
        if (!vr)
            return Vector(-1.0f, 0.0f, 0.0f);

        Vector publishedAxis = VrHandMath::Normalize(boltBox.pullAxisWorld);
        if (publishedAxis.Length() > 0.0001f)
            return publishedAxis;

        Vector localAxis = VrHandMath::Normalize(MagazineInteractionResolveBoltPullAxisLocal(vr));
        if (localAxis.Length() > 0.0001f && MagazineInteractionMatrixBasisLooksValid(boltRestWorld))
        {
            Vector worldAxis = MagazineInteractionMatrixLocalVectorToWorld(boltRestWorld, localAxis);
            worldAxis = VrHandMath::Normalize(worldAxis);
            if (worldAxis.Length() > 0.0001f)
                return worldAxis;
        }

        return Vector(-1.0f, 0.0f, 0.0f);
    }

    VrHandMatrix4 MagazineInteractionBuildControllerRelation(
        const VrHandMatrix4& controllerWorld,
        const VrHandMatrix4& magazineWorld)
    {
        VrHandMatrix4 relation = VrHandMath::Identity();

        const Vector controllerOrigin = MagazineInteractionMatrixOrigin(controllerWorld);
        const Vector controllerAxes[3] =
        {
            MagazineInteractionMatrixAxis(controllerWorld, 0),
            MagazineInteractionMatrixAxis(controllerWorld, 1),
            MagazineInteractionMatrixAxis(controllerWorld, 2)
        };
        const Vector magazineOrigin = MagazineInteractionMatrixOrigin(magazineWorld);
        const Vector delta = magazineOrigin - controllerOrigin;
        const Vector localOrigin(
            VrHandMath::Dot(delta, controllerAxes[0]),
            VrHandMath::Dot(delta, controllerAxes[1]),
            VrHandMath::Dot(delta, controllerAxes[2]));

        for (int column = 0; column < 3; ++column)
        {
            const Vector magazineAxis = MagazineInteractionMatrixAxis(magazineWorld, column);
            VrHandMath::Set(relation, 0, column, VrHandMath::Dot(magazineAxis, controllerAxes[0]));
            VrHandMath::Set(relation, 1, column, VrHandMath::Dot(magazineAxis, controllerAxes[1]));
            VrHandMath::Set(relation, 2, column, VrHandMath::Dot(magazineAxis, controllerAxes[2]));
        }
        VrHandMath::Set(relation, 0, 3, localOrigin.x);
        VrHandMath::Set(relation, 1, 3, localOrigin.y);
        VrHandMath::Set(relation, 2, 3, localOrigin.z);
        return relation;
    }

    VrHandMatrix4 MagazineInteractionBuildWorldFromControllerRelation(
        const VrHandMatrix4& controllerWorld,
        const VrHandMatrix4& relation)
    {
        VrHandMatrix4 out = VrHandMath::Identity();

        const Vector controllerOrigin = MagazineInteractionMatrixOrigin(controllerWorld);
        const Vector controllerAxes[3] =
        {
            MagazineInteractionMatrixAxis(controllerWorld, 0),
            MagazineInteractionMatrixAxis(controllerWorld, 1),
            MagazineInteractionMatrixAxis(controllerWorld, 2)
        };

        for (int column = 0; column < 3; ++column)
        {
            const Vector localAxis(
                VrHandMath::Get(relation, 0, column),
                VrHandMath::Get(relation, 1, column),
                VrHandMath::Get(relation, 2, column));
            const Vector worldAxis =
                controllerAxes[0] * localAxis.x +
                controllerAxes[1] * localAxis.y +
                controllerAxes[2] * localAxis.z;
            VrHandMath::Set(out, 0, column, worldAxis.x);
            VrHandMath::Set(out, 1, column, worldAxis.y);
            VrHandMath::Set(out, 2, column, worldAxis.z);
        }

        const Vector localOrigin(
            VrHandMath::Get(relation, 0, 3),
            VrHandMath::Get(relation, 1, 3),
            VrHandMath::Get(relation, 2, 3));
        const Vector worldOrigin =
            controllerOrigin +
            controllerAxes[0] * localOrigin.x +
            controllerAxes[1] * localOrigin.y +
            controllerAxes[2] * localOrigin.z;
        VrHandMath::Set(out, 0, 3, worldOrigin.x);
        VrHandMath::Set(out, 1, 3, worldOrigin.y);
        VrHandMath::Set(out, 2, 3, worldOrigin.z);
        return out;
    }

    Vector MagazineInteractionBoxCenterLocal(const MagazineInteractionBoxSnapshot& box)
    {
        return (box.mins + box.maxs) * 0.5f;
    }

    Vector MagazineInteractionMatrixPointWorld(const VrHandMatrix4& matrix, const Vector& localPoint)
    {
        return MagazineInteractionMatrixOrigin(matrix) +
            MagazineInteractionMatrixAxis(matrix, 0) * localPoint.x +
            MagazineInteractionMatrixAxis(matrix, 1) * localPoint.y +
            MagazineInteractionMatrixAxis(matrix, 2) * localPoint.z;
    }

    MagazineInteractionBoxSnapshot MagazineInteractionBuildWorldBoxSnapshot(
        const MagazineInteractionBoxSnapshot& localBox,
        const VrHandMatrix4& world)
    {
        MagazineInteractionBoxSnapshot out = localBox;
        out.origin = MagazineInteractionMatrixOrigin(world);
        out.axisX = MagazineInteractionMatrixAxis(world, 0);
        out.axisY = MagazineInteractionMatrixAxis(world, 1);
        out.axisZ = MagazineInteractionMatrixAxis(world, 2);
        out.publishedAt = std::chrono::steady_clock::now();
        return out;
    }

    MagazineInteractionBoxSnapshot MagazineInteractionBuildWorldBoxSnapshotAtCenter(
        const MagazineInteractionBoxSnapshot& localBox,
        const VrHandMatrix4& world,
        const Vector& desiredCenter)
    {
        MagazineInteractionBoxSnapshot out = MagazineInteractionBuildWorldBoxSnapshot(localBox, world);
        const Vector centerLocal = MagazineInteractionBoxCenterLocal(out);
        out.origin =
            desiredCenter -
            out.axisX * centerLocal.x -
            out.axisY * centerLocal.y -
            out.axisZ * centerLocal.z;
        return out;
    }

    void MagazineInteractionInflateLocalBox(Vector& mins, Vector& maxs, float padding)
    {
        if (!(padding > 0.0f))
            return;

        mins.x -= padding;
        mins.y -= padding;
        mins.z -= padding;
        maxs.x += padding;
        maxs.y += padding;
        maxs.z += padding;
    }

    Vector MagazineInteractionFreshMagazineHalfExtentsSourceUnits(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        return Vector(
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.x) * vr->m_VRScale,
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.y) * vr->m_VRScale,
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.z) * vr->m_VRScale);
    }

    Vector MagazineInteractionFreshMagazineHandAnchorLocal(
        const VR* vr,
        const MagazineInteractionBoxSnapshot& magazineBox)
    {
        Vector anchorLocal = MagazineInteractionBoxCenterLocal(magazineBox);
        if (vr && vr->m_VrHandsEnabled) return anchorLocal;
        const Vector freshHalf = MagazineInteractionFreshMagazineHalfExtentsSourceUnits(vr);
        const float configuredHeight = freshHalf.z * 2.0f;
        const float fallbackHeight = std::max(0.0f, magazineBox.maxs.z - magazineBox.mins.z);
        const float boxHeight = configuredHeight > 0.0f ? configuredHeight : fallbackHeight;
        anchorLocal.z -= boxHeight * 0.5f;
        return anchorLocal;
    }

    bool MagazineInteractionBuildModelBasisWorld(
        const MagazineInteractionBoxSnapshot& box,
        VrHandMatrix4& outWorld)
    {
        if (!box.modelBasisValid)
            return false;

        outWorld = VrHandMath::Identity();
        const Vector axisX = MagazineInteractionNormalizeAxis(box.modelAxisX, Vector(1.0f, 0.0f, 0.0f));
        const Vector axisY = MagazineInteractionNormalizeAxis(box.modelAxisY, Vector(0.0f, 1.0f, 0.0f));
        const Vector axisZ = MagazineInteractionNormalizeAxis(box.modelAxisZ, Vector(0.0f, 0.0f, 1.0f));
        VrHandMath::Set(outWorld, 0, 0, axisX.x);
        VrHandMath::Set(outWorld, 1, 0, axisX.y);
        VrHandMath::Set(outWorld, 2, 0, axisX.z);
        VrHandMath::Set(outWorld, 0, 1, axisY.x);
        VrHandMath::Set(outWorld, 1, 1, axisY.y);
        VrHandMath::Set(outWorld, 2, 1, axisY.z);
        VrHandMath::Set(outWorld, 0, 2, axisZ.x);
        VrHandMath::Set(outWorld, 1, 2, axisZ.y);
        VrHandMath::Set(outWorld, 2, 2, axisZ.z);
        VrHandMath::Set(outWorld, 0, 3, box.modelOrigin.x);
        VrHandMath::Set(outWorld, 1, 3, box.modelOrigin.y);
        VrHandMath::Set(outWorld, 2, 3, box.modelOrigin.z);
        return MagazineInteractionMatrixLooksRenderable(outWorld);
    }

    bool MagazineInteractionBuildCurrentViewmodelModelBasisWorld(
        const VR* vr,
        VrHandMatrix4& outWorld,
        l4d2vr_dual::Hand hand = l4d2vr_dual::Hand::Right)
    {
        if (!vr)
            return false;

        VR* mutableVr = const_cast<VR*>(vr);
        const Vector origin = mutableVr->GetRecommendedViewmodelAbsPos();
        const QAngle angles = mutableVr->GetRecommendedViewmodelAbsAngle();

        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);
        forward = MagazineInteractionNormalizeAxis(forward, Vector(1.0f, 0.0f, 0.0f));
        right = MagazineInteractionNormalizeAxis(right, Vector(0.0f, 1.0f, 0.0f));
        up = MagazineInteractionNormalizeAxis(up, Vector(0.0f, 0.0f, 1.0f));

        outWorld = VrHandMath::Identity();
        VrHandMath::Set(outWorld, 0, 0, forward.x);
        VrHandMath::Set(outWorld, 1, 0, forward.y);
        VrHandMath::Set(outWorld, 2, 0, forward.z);
        VrHandMath::Set(outWorld, 0, 1, right.x);
        VrHandMath::Set(outWorld, 1, 1, right.y);
        VrHandMath::Set(outWorld, 2, 1, right.z);
        VrHandMath::Set(outWorld, 0, 2, up.x);
        VrHandMath::Set(outWorld, 1, 2, up.y);
        VrHandMath::Set(outWorld, 2, 2, up.z);
        VrHandMath::Set(outWorld, 0, 3, origin.x);
        VrHandMath::Set(outWorld, 1, 3, origin.y);
        VrHandMath::Set(outWorld, 2, 3, origin.z);
        if (hand == l4d2vr_dual::Hand::Left)
        {
            VrHandMatrix4 rightController{}, leftController{};
            if (!vr->BuildPistolControllerWorld(false, rightController) ||
                !vr->BuildPistolControllerWorld(true, leftController)) return false;
            l4d2vr_dual::Matrix3x4 rightFrame{}, leftFrame{}, identity{}, delta{};
            for (int row = 0; row < 3; ++row)
            {
                identity.m[row][row] = 1.0f;
                for (int col = 0; col < 4; ++col)
                {
                    rightFrame.m[row][col] = VrHandMath::Get(rightController, row, col);
                    leftFrame.m[row][col] = VrHandMath::Get(leftController, row, col);
                }
            }
            if (!l4d2vr_dual::Retarget(rightFrame, leftFrame, identity, identity, delta)) return false;
            VrHandMatrix4 transfer = VrHandMath::Identity();
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    VrHandMath::Set(transfer, row, col, delta.m[row][col]);
            outWorld = VrHandMath::Multiply(transfer, outWorld);
        }
        else if (hand != l4d2vr_dual::Hand::Right) return false;
        return MagazineInteractionMatrixLooksRenderable(outWorld);
    }

    bool MagazineInteractionBuildPublishedViewmodelAnchorWorld(
        const MagazineInteractionBoxSnapshot& box,
        VrHandMatrix4& outWorld)
    {
        if (!box.viewmodelAnchorBasisValid)
            return false;

        outWorld = VrHandMath::Identity();
        const Vector axisX = MagazineInteractionNormalizeAxis(box.viewmodelAnchorAxisX, Vector(1.0f, 0.0f, 0.0f));
        const Vector axisY = MagazineInteractionNormalizeAxis(box.viewmodelAnchorAxisY, Vector(0.0f, 1.0f, 0.0f));
        const Vector axisZ = MagazineInteractionNormalizeAxis(box.viewmodelAnchorAxisZ, Vector(0.0f, 0.0f, 1.0f));
        VrHandMath::Set(outWorld, 0, 0, axisX.x);
        VrHandMath::Set(outWorld, 1, 0, axisX.y);
        VrHandMath::Set(outWorld, 2, 0, axisX.z);
        VrHandMath::Set(outWorld, 0, 1, axisY.x);
        VrHandMath::Set(outWorld, 1, 1, axisY.y);
        VrHandMath::Set(outWorld, 2, 1, axisY.z);
        VrHandMath::Set(outWorld, 0, 2, axisZ.x);
        VrHandMath::Set(outWorld, 1, 2, axisZ.y);
        VrHandMath::Set(outWorld, 2, 2, axisZ.z);
        VrHandMath::Set(outWorld, 0, 3, box.viewmodelAnchorOrigin.x);
        VrHandMath::Set(outWorld, 1, 3, box.viewmodelAnchorOrigin.y);
        VrHandMath::Set(outWorld, 2, 3, box.viewmodelAnchorOrigin.z);
        return MagazineInteractionMatrixLooksRenderable(outWorld);
    }

    bool MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(
        const VR* vr,
        const MagazineInteractionBoxSnapshot& sourceBox,
        MagazineInteractionBoxSnapshot& outBox,
        VrHandMatrix4& outWorld)
    {
        VrHandMatrix4 oldAnchorWorld{};
        if (!MagazineInteractionBuildPublishedViewmodelAnchorWorld(sourceBox, oldAnchorWorld))
            return false;

        VrHandMatrix4 currentAnchorWorld{};
        if (!vr || !l4d2vr_dual::ContactHandMatches(sourceBox.anchorHand, vr->GetPrimaryPistolHand()) ||
            !MagazineInteractionBuildCurrentViewmodelModelBasisWorld(vr, currentAnchorWorld, sourceBox.anchorHand))
            return false;

        const VrHandMatrix4 sourceWorld = MagazineInteractionBuildBoxWorld(sourceBox);
        if (!MagazineInteractionMatrixLooksRenderable(sourceWorld))
            return false;

        const VrHandMatrix4 anchorToBox = MagazineInteractionBuildControllerRelation(
            oldAnchorWorld,
            sourceWorld);
        if (!MagazineInteractionMatrixBasisLooksValid(anchorToBox))
            return false;

        outWorld = MagazineInteractionBuildWorldFromControllerRelation(
            currentAnchorWorld,
            anchorToBox);
        if (!MagazineInteractionMatrixLooksRenderable(outWorld))
            return false;

        outBox = sourceBox;
        outBox.origin = MagazineInteractionMatrixOrigin(outWorld);
        outBox.axisX = MagazineInteractionMatrixAxis(outWorld, 0);
        outBox.axisY = MagazineInteractionMatrixAxis(outWorld, 1);
        outBox.axisZ = MagazineInteractionMatrixAxis(outWorld, 2);
        outBox.viewmodelAnchorBasisValid = true;
        outBox.viewmodelAnchorOrigin = MagazineInteractionMatrixOrigin(currentAnchorWorld);
        outBox.viewmodelAnchorAxisX = MagazineInteractionMatrixAxis(currentAnchorWorld, 0);
        outBox.viewmodelAnchorAxisY = MagazineInteractionMatrixAxis(currentAnchorWorld, 1);
        outBox.viewmodelAnchorAxisZ = MagazineInteractionMatrixAxis(currentAnchorWorld, 2);
        const Vector sourcePullAxis = VrHandMath::Normalize(sourceBox.pullAxisWorld);
        if (sourcePullAxis.Length() > 0.0001f)
        {
            Vector rebasedPullAxis = MagazineInteractionMatrixLocalVectorToWorld(
                currentAnchorWorld,
                MagazineInteractionWorldVectorToMatrixLocal(oldAnchorWorld, sourcePullAxis));
            rebasedPullAxis = VrHandMath::Normalize(rebasedPullAxis);
            if (rebasedPullAxis.Length() > 0.0001f)
                outBox.pullAxisWorld = rebasedPullAxis;
        }
        outBox.publishedAt = std::chrono::steady_clock::now();
        return true;
    }

    void MagazineInteractionProjectBoxOntoAxis(
        const VrHandMatrix4& world,
        const MagazineInteractionBoxSnapshot& box,
        const Vector& axis,
        float& outMin,
        float& outMax)
    {
        outMin = FLT_MAX;
        outMax = -FLT_MAX;
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = 0; y <= 1; ++y)
            {
                for (int x = 0; x <= 1; ++x)
                {
                    const Vector local(
                        x ? box.maxs.x : box.mins.x,
                        y ? box.maxs.y : box.mins.y,
                        z ? box.maxs.z : box.mins.z);
                    const float projected = VrHandMath::Dot(
                        MagazineInteractionMatrixPointWorld(world, local),
                        axis);
                    outMin = std::min(outMin, projected);
                    outMax = std::max(outMax, projected);
                }
            }
        }
    }

    float MagazineInteractionIntervalOverlap(float aMin, float aMax, float bMin, float bMax)
    {
        return std::max(0.0f, std::min(aMax, bMax) - std::max(aMin, bMin));
    }

    VrHandMatrix4 MagazineInteractionBuildWorldAtBoxCenter(
        const VrHandMatrix4& orientationWorld,
        const MagazineInteractionBoxSnapshot& box,
        const Vector& desiredCenter)
    {
        VrHandMatrix4 out = orientationWorld;
        const Vector axisX = MagazineInteractionMatrixAxis(orientationWorld, 0);
        const Vector axisY = MagazineInteractionMatrixAxis(orientationWorld, 1);
        const Vector axisZ = MagazineInteractionMatrixAxis(orientationWorld, 2);
        const Vector centerLocal = MagazineInteractionBoxCenterLocal(box);
        const Vector origin =
            desiredCenter -
            axisX * centerLocal.x -
            axisY * centerLocal.y -
            axisZ * centerLocal.z;

        VrHandMath::Set(out, 0, 0, axisX.x);
        VrHandMath::Set(out, 1, 0, axisX.y);
        VrHandMath::Set(out, 2, 0, axisX.z);
        VrHandMath::Set(out, 0, 1, axisY.x);
        VrHandMath::Set(out, 1, 1, axisY.y);
        VrHandMath::Set(out, 2, 1, axisY.z);
        VrHandMath::Set(out, 0, 2, axisZ.x);
        VrHandMath::Set(out, 1, 2, axisZ.y);
        VrHandMath::Set(out, 2, 2, axisZ.z);
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    VrHandMatrix4 MagazineInteractionBuildFreshHandMagazineWorldFromController(
        const VR* vr,
        const VrHandMatrix4& controllerWorld)
    {
        if (!vr)
            return VrHandMath::Identity();

        const VrHandMatrix4 local = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            vr->m_MagazineInteractionMagazineHandOffsetMeters,
            vr->m_MagazineInteractionMagazineHandRotationOffsetDeg);
        return VrHandMath::Multiply(controllerWorld, local);
    }

    VrHandMatrix4 MagazineInteractionBuildRenderSnapshotLeftControllerWorld(const VR* vr)
    {
        if (!vr)
            return VrHandMath::Identity();

        VR* mutableVr = const_cast<VR*>(vr);
        const bool supportLeft = vr->MagazineReloadSupportIsLeft();
        const Vector leftControllerPos = supportLeft ? mutableVr->GetLeftControllerAbsPos() : mutableVr->GetRightControllerAbsPos();
        const QAngle leftControllerAng = supportLeft ? mutableVr->GetLeftControllerAbsAngle() : mutableVr->GetRightControllerAbsAngle();
        Vector leftForward{};
        Vector leftRight{};
        Vector leftUp{};
        QAngle::AngleVectors(leftControllerAng, &leftForward, &leftRight, &leftUp);
        return MagazineInteractionBuildViewmodelReprojectedControllerWorld(
            vr,
            leftControllerPos,
            leftForward,
            leftRight,
            leftUp);
    }

    bool MagazineInteractionTryBuildHeldMagazineWorldFromRenderSnapshot(
        const VR* vr,
        bool freshMagazine,
        VrHandMatrix4& outWorld)
    {
        if (!vr || !VR::t_UseRenderFrameSnapshot || !vr->m_MagazineInteractionSocketValid)
            return false;

        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildRenderSnapshotLeftControllerWorld(vr);
        if (!MagazineInteractionMatrixLooksRenderable(controllerWorld))
            return false;

        if (freshMagazine)
        {
            const l4d2vr_ammo_grip::Pose pose{vr->m_MagazineInteractionControllerToMagazine,
                vr->m_MagazineInteractionHeldMagazineCenterOffsetLocal};
            return l4d2vr_ammo_grip::Follow(controllerWorld, pose,
                MagazineInteractionBoxCenterLocal(vr->m_MagazineInteractionSocketBox), outWorld);
        }

        VrHandMatrix4 orientationWorld = MagazineInteractionBuildWorldFromControllerRelation(
            controllerWorld,
            vr->m_MagazineInteractionControllerToMagazine);
        if (!MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            orientationWorld = MagazineInteractionBuildFreshHandMagazineWorldFromController(vr, controllerWorld);
        if (freshMagazine && !MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            orientationWorld = controllerWorld;
        if (!MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            return false;

        const Vector desiredCenter =
            MagazineInteractionMatrixOrigin(controllerWorld) +
            MagazineInteractionMatrixLocalVectorToWorld(
                controllerWorld,
                vr->m_MagazineInteractionHeldMagazineCenterOffsetLocal);
        outWorld = MagazineInteractionBuildWorldAtBoxCenter(
            orientationWorld,
            vr->m_MagazineInteractionSocketBox,
            desiredCenter);
        return MagazineInteractionMatrixLooksRenderable(outWorld);
    }

    bool MagazineInteractionTryGetRenderSnapshotHmdPosAbs(const VR* vr, Vector& outHmdPosAbs)
    {
        if (!vr || !VR::t_UseRenderFrameSnapshot)
            return false;

        Vector viewOrigin{};
        Vector forward{};
        Vector right{};
        Vector up{};
        if (!MagazineInteractionResolveViewFrame(vr, viewOrigin, forward, right, up))
            return false;

        const float eyeZ = vr->m_EyeZ * vr->m_VRScale;
        if (!std::isfinite(eyeZ))
            return false;

        outHmdPosAbs = viewOrigin + forward * eyeZ;
        return std::isfinite(outHmdPosAbs.x) &&
            std::isfinite(outHmdPosAbs.y) &&
            std::isfinite(outHmdPosAbs.z);
    }

    bool MagazineInteractionTryBuildWaitingFreshMagazineWorldFromRenderSnapshot(
        const VR* vr,
        VrHandMatrix4& outWorld)
    {
        if (!vr || !VR::t_UseRenderFrameSnapshot || !vr->m_MagazineInteractionSocketValid)
            return false;

        Vector hmdPosAbs{};
        if (!MagazineInteractionTryGetRenderSnapshotHmdPosAbs(vr, hmdPosAbs))
            return false;

        Vector bodyForward{};
        Vector bodyRight{};
        if (!MagazineInteractionResolveFreshPickupBodyAxes(vr, bodyForward, bodyRight))
            return false;

        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector bodyOrigin = hmdPosAbs
            + bodyForward * (vr->m_InventoryBodyOriginOffset.x * vr->m_VRScale)
            + bodyRight * (vr->m_InventoryBodyOriginOffset.y * vr->m_VRScale)
            + worldUp * (vr->m_InventoryBodyOriginOffset.z * vr->m_VRScale);
        if (vr->m_BodyGripInventoryEnabled)
            vr->GetBodyInventoryPose(bodyOrigin, bodyForward, bodyRight);
        const Vector pickupOffsetMeters = MagazineInteractionFreshMagazinePickupOffsetMeters(vr);
        const Vector pickup = bodyOrigin
            + bodyForward * (pickupOffsetMeters.x * vr->m_VRScale)
            + bodyRight * (pickupOffsetMeters.y * vr->m_VRScale)
            + worldUp * (pickupOffsetMeters.z * vr->m_VRScale);

        Vector viewmodelPickup = pickup;
        Vector reprojectedPickup{};
        if (MagazineInteractionReprojectScenePointToViewmodelLayer(vr, pickup, reprojectedPickup))
            viewmodelPickup = reprojectedPickup;

        return l4d2vr_ammo_grip::BodyWorld(bodyForward, bodyRight, viewmodelPickup,
            MagazineInteractionBoxCenterLocal(vr->m_MagazineInteractionSocketBox), outWorld) &&
            MagazineInteractionMatrixLooksRenderable(outWorld);
    }

    bool MagazineInteractionTryReadInt(const void* entity, int offset, int& out)
    {
        if (!entity || offset < 0)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            out = *reinterpret_cast<const int*>(reinterpret_cast<const unsigned char*>(entity) + offset);
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
#endif
    }

    template <typename T>
    bool MagazineInteractionTryReadValue(const void* entity, int offset, T& out)
    {
        if (!entity || offset < 0)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            out = *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(entity) + offset);
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = {};
            return false;
        }
#endif
    }

    template <typename T>
    bool MagazineInteractionTryWriteValue(void* entity, int offset, const T& value)
    {
        if (!entity || offset < 0)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(entity) + offset) = value;
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    bool MagazineInteractionWeaponUsesDetachableMagazine(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::MP5:
        case C_WeaponCSBase::WeaponID::SG552:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:
            return true;
        default:
            return false;
        }
    }

    bool MagazineInteractionWeaponUsesShotgunShells(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
        case C_WeaponCSBase::WeaponID::SPAS:
            return true;
        default:
            return false;
        }
    }

    bool MagazineInteractionPublishedBoxMatchesWeapon(
        const MagazineInteractionBoxSnapshot& box,
        C_WeaponCSBase::WeaponID activeWeaponId)
    {
        const int modelWeaponId = MagazineInteractionInferWeaponIdFromModelName(box.modelName);
        return modelWeaponId <= 0 || modelWeaponId == static_cast<int>(activeWeaponId);
    }

    bool MagazineInteractionWeaponUsesSinglePumpSound(C_WeaponCSBase::WeaponID weaponId)
    {
        return weaponId == C_WeaponCSBase::WeaponID::PUMPSHOTGUN ||
            weaponId == C_WeaponCSBase::WeaponID::SHOTGUN_CHROME;
    }

    bool MagazineInteractionWeaponUsesPhysicalReload(C_WeaponCSBase::WeaponID weaponId)
    {
        return MagazineInteractionWeaponUsesDetachableMagazine(weaponId) ||
            MagazineInteractionWeaponUsesShotgunShells(weaponId);
    }

    bool VrHandsLongWeapon(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::SPAS:
        case C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::MP5:
        case C_WeaponCSBase::WeaponID::SG552:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:
        case C_WeaponCSBase::WeaponID::M60:
        case C_WeaponCSBase::WeaponID::MACHINEGUN:
            return true;
        default:
            return false;
        }
    }

    float GetMagazineInteractionEmptyClipAutoEjectFreezeDelaySeconds(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
            return 0.12f;
        case C_WeaponCSBase::WeaponID::MAGNUM:
            return 0.12f;
        case C_WeaponCSBase::WeaponID::UZI:
            return 0.15f;
        case C_WeaponCSBase::WeaponID::MAC10:
            return 0.15f;
        case C_WeaponCSBase::WeaponID::MP5:
            return 0.15f;
        case C_WeaponCSBase::WeaponID::M16A1:
            return 0.08f;
        case C_WeaponCSBase::WeaponID::AK47:
            return 0.08f;
        case C_WeaponCSBase::WeaponID::SCAR:
            return 0.08f;
        case C_WeaponCSBase::WeaponID::SG552:
            return 0.005f;
            return 0.0f;
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
            return 0.0f;
        case C_WeaponCSBase::WeaponID::AWP:
            return 0.0f;
        case C_WeaponCSBase::WeaponID::SCOUT:
            return 0.0f;
        case C_WeaponCSBase::WeaponID::M60:
            return 0.0f;
        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
            return 0.25f;
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
            return 0.25f;
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
            return 0.25f;
        case C_WeaponCSBase::WeaponID::SPAS:
            return 0.25f;
        default:
            return 0.18f;
        }
    }

    bool MagazineInteractionClearDetachableClientReloadState(C_WeaponCSBase* clientWeapon)
    {
        if (!clientWeapon)
            return false;

        constexpr int kClientNextPrimaryAttackOffset = 0x960;       // DT_BaseCombatWeapon::m_flNextPrimaryAttack
        constexpr int kClientNextSecondaryAttackOffset = 0x964;     // DT_BaseCombatWeapon::m_flNextSecondaryAttack
        constexpr int kClientTimeWeaponIdleOffset = 0x990;          // DT_BaseCombatWeapon::m_flTimeWeaponIdle
        constexpr int kClientBaseWeaponInReloadOffset = 0x9BD;      // DT_BaseCombatWeapon::m_bInReload
        constexpr int kClientPartialReloadStageOffset = 0xCE8;      // DT_WeaponCSBase::m_partialReloadStage
        constexpr int kClientReloadFromEmptyOffset = 0xCEF;         // DT_WeaponCSBase::m_reloadFromEmpty

        const bool wroteNextPrimaryAttack =
            MagazineInteractionTryWriteValue<float>(clientWeapon, kClientNextPrimaryAttackOffset, 0.0f);
        const bool wroteNextSecondaryAttack =
            MagazineInteractionTryWriteValue<float>(clientWeapon, kClientNextSecondaryAttackOffset, 0.0f);
        const bool wroteTimeWeaponIdle =
            MagazineInteractionTryWriteValue<float>(clientWeapon, kClientTimeWeaponIdleOffset, 0.0f);
        const bool wroteInReload =
            MagazineInteractionTryWriteValue<unsigned char>(clientWeapon, kClientBaseWeaponInReloadOffset, 0);
        const bool wrotePartialReloadStage =
            MagazineInteractionTryWriteValue<unsigned char>(clientWeapon, kClientPartialReloadStageOffset, 0);
        const bool wroteReloadFromEmpty =
            MagazineInteractionTryWriteValue<unsigned char>(clientWeapon, kClientReloadFromEmptyOffset, 0);
        return wroteNextPrimaryAttack || wroteNextSecondaryAttack || wroteTimeWeaponIdle ||
            wroteInReload || wrotePartialReloadStage || wroteReloadFromEmpty;
    }

    bool MagazineInteractionReserveValueLooksPlausible(int value)
    {
        return value >= 0 && value <= 5000;
    }

    bool MagazineInteractionWeaponRequiresManualBolt(C_WeaponCSBase::WeaponID weaponId)
    {
        return MagazineInteractionWeaponUsesPhysicalReload(weaponId);
    }

    int MagazineInteractionDefaultMaxClip(C_WeaponCSBase::WeaponID weaponId, const std::string& modelName)
    {
        std::string lowerModel = modelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
            return (lowerModel.find("dual_pistol") != std::string::npos || lowerModel.find("dual_pistola") != std::string::npos) ? 30 : 15;
        case C_WeaponCSBase::WeaponID::MAGNUM: return 8;
        case C_WeaponCSBase::WeaponID::UZI: return 50;
        case C_WeaponCSBase::WeaponID::MAC10: return 50;
        case C_WeaponCSBase::WeaponID::MP5: return 50;
        case C_WeaponCSBase::WeaponID::M16A1: return 50;
        case C_WeaponCSBase::WeaponID::AK47: return 40;
        case C_WeaponCSBase::WeaponID::SCAR: return 60;
        case C_WeaponCSBase::WeaponID::SG552: return 50;
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE: return 15;
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY: return 30;
        case C_WeaponCSBase::WeaponID::AWP: return 20;
        case C_WeaponCSBase::WeaponID::SCOUT: return 15;
        case C_WeaponCSBase::WeaponID::M60: return 150;
        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN: return 8;
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME: return 8;
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN: return 10;
        case C_WeaponCSBase::WeaponID::SPAS: return 10;
        default: return 0;
        }
    }

    std::string MagazineInteractionClipOutSoundSample(const VR* vr)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerModel.find("dual_pistol") != std::string::npos ||
            lowerModel.find("dual_pistola") != std::string::npos)
        {
            return "weapons/dual_pistol/gunother/dualpistol_clip_out_1.wav";
        }
        if (lowerModel.find("desert_eagle") != std::string::npos ||
            lowerModel.find("magnum") != std::string::npos)
        {
            return "weapons/magnum/gunother/pistol_clip_out_1.wav";
        }
        if (lowerModel.find("pistol") != std::string::npos)
            return "weapons/pistol/gunother/pistol_clip_out_1.wav";
        if (lowerModel.find("rifle_ak47") != std::string::npos ||
            lowerModel.find("ak47") != std::string::npos)
        {
            return "weapons/rifle_ak47/gunother/rifle_clip_out_1.wav";
        }
        if (lowerModel.find("desert_rifle") != std::string::npos)
            return "weapons/rifle_desert/gunother/rifle_clip_out_1.wav";
        if (lowerModel.find("sg552") != std::string::npos)
            return "weapons/sg552/gunother/sg552_clipout.wav";
        if (lowerModel.find("silenced_smg") != std::string::npos ||
            lowerModel.find("smg_silenced") != std::string::npos)
        {
            return "weapons/smg_silenced/gunother/smg_clip_out_1.wav";
        }
        if (lowerModel.find("mp5") != std::string::npos)
            return "weapons/mp5navy/gunother/mp5_clipout.wav";
        if (lowerModel.find("smg") != std::string::npos ||
            lowerModel.find("uzi") != std::string::npos)
        {
            return "weapons/smg/gunother/smg_clip_out_1.wav";
        }
        if (lowerModel.find("huntingrifle") != std::string::npos ||
            lowerModel.find("hunting_rifle") != std::string::npos)
        {
            return "weapons/hunting_rifle/gunother/hunting_rifle_clipout.wav";
        }
        if (lowerModel.find("sniper_military") != std::string::npos)
            return "weapons/sniper_military/gunother/sniper_military_clip_out_1.wav";
        if (lowerModel.find("awp") != std::string::npos)
            return "weapons/awp/gunother/awp_clipout.wav";
        if (lowerModel.find("scout") != std::string::npos)
            return "weapons/scout/gunother/scout_clipout.wav";
        if (lowerModel.find("m60") != std::string::npos ||
            lowerModel.find("machinegun_m60") != std::string::npos)
        {
            return "weapons/machinegun_m60/gunother/rifle_clip_out_1.wav";
        }
        if (lowerModel.find("rifle") != std::string::npos ||
            lowerModel.find("sg552") != std::string::npos)
        {
            return "weapons/rifle/gunother/rifle_clip_out_1.wav";
        }
        return std::string();
    }

    std::string MagazineInteractionClipInSoundSample(const VR* vr)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerModel.find("dual_pistol") != std::string::npos ||
            lowerModel.find("dual_pistola") != std::string::npos)
        {
            return "weapons/dual_pistol/gunother/dualpistol_clip_in_1.wav";
        }
        if (lowerModel.find("desert_eagle") != std::string::npos ||
            lowerModel.find("magnum") != std::string::npos)
        {
            return "weapons/magnum/gunother/pistol_clip_in_1.wav";
        }
        if (lowerModel.find("pistol") != std::string::npos)
            return "weapons/pistol/gunother/pistol_clip_in_1.wav";
        if (lowerModel.find("rifle_ak47") != std::string::npos ||
            lowerModel.find("ak47") != std::string::npos)
        {
            return "weapons/rifle_ak47/gunother/rifle_clip_in_1.wav";
        }
        if (lowerModel.find("desert_rifle") != std::string::npos)
            return "weapons/rifle_desert/gunother/rifle_clip_in_1.wav";
        if (lowerModel.find("sg552") != std::string::npos)
            return "weapons/sg552/gunother/sg552_clipin.wav";
        if (lowerModel.find("silenced_smg") != std::string::npos ||
            lowerModel.find("smg_silenced") != std::string::npos)
        {
            return "weapons/smg_silenced/gunother/smg_clip_in_1.wav";
        }
        if (lowerModel.find("mp5") != std::string::npos)
            return "weapons/mp5navy/gunother/mp5_clipin.wav";
        if (lowerModel.find("smg") != std::string::npos ||
            lowerModel.find("uzi") != std::string::npos)
        {
            return "weapons/smg/gunother/smg_clip_in_1.wav";
        }
        if (lowerModel.find("huntingrifle") != std::string::npos ||
            lowerModel.find("hunting_rifle") != std::string::npos)
        {
            return "weapons/hunting_rifle/gunother/hunting_rifle_clipin.wav";
        }
        if (lowerModel.find("sniper_military") != std::string::npos)
            return "weapons/sniper_military/gunother/sniper_military_clip_in_1.wav";
        if (lowerModel.find("awp") != std::string::npos)
            return "weapons/awp/gunother/awp_clipin.wav";
        if (lowerModel.find("scout") != std::string::npos)
            return "weapons/scout/gunother/scout_clipin.wav";
        if (lowerModel.find("m60") != std::string::npos ||
            lowerModel.find("machinegun_m60") != std::string::npos)
        {
            return "weapons/machinegun_m60/gunother/rifle_clip_in_1.wav";
        }
        if (lowerModel.find("rifle") != std::string::npos)
            return "weapons/rifle/gunother/rifle_clip_in_1.wav";
        return std::string();
    }

    std::string MagazineInteractionShotgunShellInSoundSample(const VR* vr)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerModel.find("shotgun_chrome") != std::string::npos ||
            lowerModel.find("chrome") != std::string::npos)
        {
            return "weapons/shotgun/gunother/shotgun_load_shell_2.wav";
        }
        if (lowerModel.find("autoshotgun") != std::string::npos ||
            lowerModel.find("auto_shotgun") != std::string::npos)
        {
            return "weapons/auto_shotgun/gunother/auto_shotgun_load_shell_2.wav";
        }
        if (lowerModel.find("shotgun_spas") != std::string::npos ||
            lowerModel.find("spas") != std::string::npos)
        {
            return "weapons/auto_shotgun/gunother/auto_shotgun_load_shell_2.wav";
        }
        if (lowerModel.find("pumpshotgun") != std::string::npos ||
            lowerModel.find("pump_shotgun") != std::string::npos ||
            lowerModel.find("shotgun") != std::string::npos)
        {
            return "weapons/shotgun/gunother/shotgun_load_shell_2.wav";
        }
        return std::string();
    }

    std::string MagazineInteractionBoltSoundSample(const VR* vr, bool forward)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const char* suffix = forward ? "forward" : "back";
        auto slide = [&](const char* directory, const char* stem, bool numbered = true) -> std::string
        {
            std::string sample = std::string("weapons/") + directory + "/gunother/" + stem + "_slide" + suffix;
            if (numbered)
                sample += "_1";
            sample += ".wav";
            return sample;
        };
        auto bolt = [&](const char* directory, const char* stem) -> std::string
        {
            return std::string("weapons/") + directory + "/gunother/" + stem + "_bolt" + suffix + ".wav";
        };

        if (lowerModel.find("dual_pistol") != std::string::npos ||
            lowerModel.find("dual_pistola") != std::string::npos)
        {
            return slide("dual_pistol", "dualpistol");
        }
        if (lowerModel.find("desert_eagle") != std::string::npos ||
            lowerModel.find("magnum") != std::string::npos)
        {
            return slide("magnum", "pistol");
        }
        if (lowerModel.find("pistol") != std::string::npos)
            return slide("pistol", "pistol");
        if (lowerModel.find("rifle_ak47") != std::string::npos ||
            lowerModel.find("ak47") != std::string::npos)
        {
            return slide("rifle_ak47", "rifle", false);
        }
        if (lowerModel.find("desert_rifle") != std::string::npos)
            return slide("rifle_desert", "rifle");
        if (lowerModel.find("sg552") != std::string::npos)
        {
            return forward
                ? "weapons/sg552/gunother/sg552_boltpullforward.wav"
                : "weapons/sg552/gunother/sg552_boltpull.wav";
        }
        if (lowerModel.find("silenced_smg") != std::string::npos ||
            lowerModel.find("smg_silenced") != std::string::npos)
        {
            return slide("smg_silenced", "smg");
        }
        if (lowerModel.find("mp5") != std::string::npos)
            return slide("mp5navy", "mp5", false);
        if (lowerModel.find("smg") != std::string::npos ||
            lowerModel.find("uzi") != std::string::npos)
        {
            return slide("smg", "smg");
        }
        if (lowerModel.find("huntingrifle") != std::string::npos ||
            lowerModel.find("hunting_rifle") != std::string::npos)
        {
            return bolt("hunting_rifle", "hunting_rifle");
        }
        if (lowerModel.find("sniper_military") != std::string::npos)
            return slide("sniper_military", "sniper_military");
        if (lowerModel.find("awp") != std::string::npos)
            return bolt("awp", "awp");
        if (lowerModel.find("scout") != std::string::npos)
            return bolt("scout", "scout");
        if (lowerModel.find("m60") != std::string::npos ||
            lowerModel.find("machinegun_m60") != std::string::npos)
        {
            return slide("machinegun_m60", "rifle");
        }
        if (lowerModel.find("shotgun_chrome") != std::string::npos ||
            lowerModel.find("pumpshotgun") != std::string::npos ||
            lowerModel.find("pump_shotgun") != std::string::npos)
        {
            return forward ? std::string() : "weapons/shotgun/gunother/shotgun_pump_1.wav";
        }
        if (lowerModel.find("autoshotgun") != std::string::npos ||
            lowerModel.find("auto_shotgun") != std::string::npos)
        {
            return forward
                ? "weapons/auto_shotgun/gunother/autoshotgun_boltforward.wav"
                : "weapons/auto_shotgun/gunother/autoshotgun_boltback.wav";
        }
        if (lowerModel.find("shotgun_spas") != std::string::npos ||
            lowerModel.find("spas") != std::string::npos)
        {
            return forward
                ? "weapons/auto_shotgun/gunother/autoshotgun_boltforward.wav"
                : "weapons/auto_shotgun/gunother/autoshotgun_boltback.wav";
        }
        if (lowerModel.find("rifle") != std::string::npos ||
            lowerModel.find("sg552") != std::string::npos)
        {
            return slide("rifle", "rifle");
        }
        return std::string();
    }

    std::string MagazineInteractionLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool MagazineInteractionSoundMatchesCurrentWeapon(const VR* vr, const char* sample)
    {
        if (!vr || !sample || !*sample)
            return false;

        const std::string lower = MagazineInteractionLowerAscii(sample);
        const std::string model = MagazineInteractionLowerAscii(vr->m_MagazineInteractionMagazineModelName);
        auto has = [&](const char* token) -> bool
            {
                return token && *token && lower.find(token) != std::string::npos;
            };
        auto modelHas = [&](const char* token) -> bool
            {
                return token && *token && model.find(token) != std::string::npos;
            };

        const bool knownSpecificWeapon =
            has("hunting_rifle") ||
            has("sniper_military") ||
            has("rifle_ak47") ||
            has("ak47") ||
            has("rifle_desert") ||
            has("desert_rifle") ||
            has("smg_silenced") ||
            has("silenced_smg") ||
            has("smg_mp5") ||
            has("mp5") ||
            has("desert_eagle") ||
            has("magnum") ||
            has("dual_pistol") ||
            has("dualpistol") ||
            has("snip_awp") ||
            has("awp") ||
            has("snip_scout") ||
            has("scout") ||
            has("machinegun_m60") ||
            has("m60") ||
            has("pumpshotgun") ||
            has("pump_shotgun") ||
            has("shotgun_chrome") ||
            has("autoshotgun") ||
            has("auto_shotgun") ||
            has("shotgun_spas") ||
            has("spas") ||
            has("shotgun");

        switch (static_cast<C_WeaponCSBase::WeaponID>(vr->m_MagazineInteractionWeaponId))
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
            return has("pistol") && !has("magnum") && !has("desert_eagle");
        case C_WeaponCSBase::WeaponID::MAGNUM:
            return has("magnum") || has("desert_eagle") ||
                (has("pistol") && modelHas("desert_eagle"));
        case C_WeaponCSBase::WeaponID::UZI:
            return has("weapons/smg/gunother/") || has("weapons\\smg\\gunother\\") ||
                (has("smg") && !has("silenced") && !has("mp5"));
        case C_WeaponCSBase::WeaponID::MAC10:
            return has("smg_silenced") || has("silenced_smg") ||
                (has("smg") && modelHas("silenced_smg"));
        case C_WeaponCSBase::WeaponID::MP5:
            return has("mp5") || has("smg_mp5");
        case C_WeaponCSBase::WeaponID::M16A1:
            return has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\") ||
                (has("rifle") && modelHas("v_rifle.mdl"));
        case C_WeaponCSBase::WeaponID::AK47:
            return has("rifle_ak47") || has("ak47") ||
                ((has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\")) && modelHas("ak47"));
        case C_WeaponCSBase::WeaponID::SCAR:
            return has("rifle_desert") || has("desert_rifle") || has("scar");
        case C_WeaponCSBase::WeaponID::SG552:
            return has("sg552") ||
                ((has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\")) && modelHas("sg552"));
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
            return has("hunting_rifle") || has("huntingrifle");
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
            if (has("hunting_rifle") || has("huntingrifle"))
                return false;
            return has("sniper_military") || has("military_sniper");
        case C_WeaponCSBase::WeaponID::AWP:
            return has("awp") || has("snip_awp");
        case C_WeaponCSBase::WeaponID::SCOUT:
            return has("scout") || has("snip_scout");
        case C_WeaponCSBase::WeaponID::M60:
            return has("m60") || has("machinegun_m60") ||
                ((has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\")) && modelHas("m60"));
        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
            return has("pumpshotgun") || has("pump_shotgun") ||
                ((has("weapons/shotgun/") || has("weapons\\shotgun\\")) && !has("chrome") && !has("spas"));
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
            return has("shotgun_chrome") || has("chrome") ||
                ((has("weapons/shotgun/") || has("weapons\\shotgun\\")) && modelHas("shotgun_chrome"));
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
            return has("autoshotgun") || has("auto_shotgun");
        case C_WeaponCSBase::WeaponID::SPAS:
            return has("shotgun_spas") || has("spas");
        default:
            break;
        }

        return !knownSpecificWeapon;
    }

    void MagazineInteractionPlayClipOutSound(VR* vr)
    {
        if (!vr || !vr->m_Game)
            return;

        const std::string sample = MagazineInteractionClipOutSoundSample(vr);
        if (sample.empty())
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] no synthetic clip-out sample for model=%s weaponId=%d",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        if (!MagazineInteractionPlaySyntheticSound(
            vr,
            sample,
            vr->m_MagazineInteractionSyntheticClipOutSample,
            vr->m_MagazineInteractionSyntheticClipOutStarted,
            "clip-out"))
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] failed to play synthetic clip-out sample=%s",
                sample.c_str());
        }
    }

    void MagazineInteractionPlayClipInSound(VR* vr)
    {
        if (!vr || !vr->m_Game)
            return;

        const std::string sample =
            vr->m_MagazineInteractionShotgunShellMode
            ? MagazineInteractionShotgunShellInSoundSample(vr)
            : MagazineInteractionClipInSoundSample(vr);
        if (sample.empty())
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] no synthetic clip-in sample for model=%s weaponId=%d",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        if (!MagazineInteractionPlaySyntheticSound(
            vr,
            sample,
            vr->m_MagazineInteractionSyntheticClipInSample,
            vr->m_MagazineInteractionSyntheticClipInStarted,
            "clip-in"))
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] failed to play synthetic clip-in sample=%s",
                sample.c_str());
        }
    }

    void MagazineInteractionPlayBoltSound(VR* vr, bool forward)
    {
        if (!vr || !vr->m_Game)
            return;

        const auto weaponId = static_cast<C_WeaponCSBase::WeaponID>(vr->m_MagazineInteractionWeaponId);
        if (forward && MagazineInteractionWeaponUsesSinglePumpSound(weaponId))
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] skipped bolt-forward sound for single-sample pump shotgun model=%s weaponId=%d",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        const std::string sample = MagazineInteractionBoltSoundSample(vr, forward);
        if (MagazineInteractionPrepareSoundSamplePath(sample).empty())
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] no configured bolt-%s sample for model=%s weaponId=%d",
                forward ? "forward" : "back",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        if (forward)
        {
            MagazineInteractionPlaySyntheticSound(
                vr,
                sample,
                vr->m_MagazineInteractionSyntheticBoltForwardSample,
                vr->m_MagazineInteractionSyntheticBoltForwardStarted,
                "bolt-forward");
        }
        else
        {
            MagazineInteractionPlaySyntheticSound(
                vr,
                sample,
                vr->m_MagazineInteractionSyntheticBoltBackSample,
                vr->m_MagazineInteractionSyntheticBoltBackStarted,
                "bolt-back");
        }
    }

    const char* MagazineInteractionBlockedFireEmptySound(int weaponId)
    {
        const auto id = static_cast<C_WeaponCSBase::WeaponID>(weaponId);
        return (id == C_WeaponCSBase::WeaponID::PISTOL ||
            id == C_WeaponCSBase::WeaponID::MAGNUM)
            ? "ClipEmpty_Pistol"
            : "ClipEmpty_Rifle";
    }

    bool MagazineInteractionSoundLooksShotgunFire(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("shotgun_fire") != std::string::npos;
    }

    bool MagazineInteractionSoundLooksShotgunPump(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("shotgun_pump") != std::string::npos;
    }

    bool MagazineInteractionSoundLooksClipEmpty(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clipempty") != std::string::npos ||
            lower.find("clip_empty") != std::string::npos ||
            lower.find("clip.empty") != std::string::npos ||
            lower.find("clip-empty") != std::string::npos;
    }

    uint32_t MagazineInteractionActiveWeaponHandle(Game* game, C_BasePlayer* player, C_WeaponCSBase* weapon)
    {
        if (!game || !game->m_ClientEntityList || !player || !weapon) return 0u;
        const int offset = game->FindRecvPropOffset("DT_TerrorPlayer", "m_hActiveWeapon");
        uint32_t handle = 0u;
        if (offset <= 0 || !MagazineInteractionTryReadValue(player, offset, handle) ||
            handle == 0u || handle == 0xffffffffu) return 0u;
#ifdef _MSC_VER
        __try
        {
#endif
            return game->m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(handle)) == weapon
                ? handle : 0u;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
#endif
    }

    bool MagazineInteractionReadActiveWeapon(
        C_BasePlayer* localPlayer,
        C_WeaponCSBase*& outWeapon,
        C_WeaponCSBase::WeaponID& outWeaponId,
        int& outClip)
    {
        outWeapon = nullptr;
        outWeaponId = C_WeaponCSBase::WeaponID::NONE;
        outClip = -1;
        if (!localPlayer)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            outWeapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
            if (!outWeapon)
                return false;
            outWeaponId = outWeapon->GetWeaponID();
            if (!MagazineInteractionTryReadInt(outWeapon, VR::kClip1Offset, outClip))
                return false;
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outWeapon = nullptr;
            outWeaponId = C_WeaponCSBase::WeaponID::NONE;
            outClip = -1;
            return false;
        }
#endif
    }
}

l4d2vr_dual::Hand VR::GetPrimaryPistolHand() const
{
    return l4d2vr_dual::PrimaryHand(m_IsVREnabled && m_DualPistolsIndependentHandsEnabled && !m_MouseModeEnabled &&
        m_LeftHandPistolActive.load(std::memory_order_acquire),
        m_DualPistolsActive.load(std::memory_order_acquire), m_PistolOwnershipMask.load(std::memory_order_acquire));
}

bool VR::MagazineReloadSupportIsLeft() const
{
    return l4d2vr_dual::ReloadSupportIsLeft(GetPrimaryPistolHand());
}

unsigned VR::MagazineGripPhysicalMask() const
{
    return l4d2vr_dual::ReloadGripPhysicalMask(
        m_MagazineInteractionLeftHandPoseActive.load(std::memory_order_acquire) != 0u,
        GetPrimaryPistolHand(), m_LeftHanded);
}

bool VR::BuildPistolControllerWorld(bool left, VrHandMatrix4& outWorld) const
{
    outWorld = VrHandMath::Identity();
    {
        std::lock_guard<std::mutex> lock(m_WorldPoseTrackingSnapshotMutex);
        const auto now = GetTickCount64();
        const bool physicalLeft = IsGameplayHandLeftPhysical(left);
        const bool handValid = physicalLeft ? m_WorldPoseTrackingSnapshot.leftHandValid : m_WorldPoseTrackingSnapshot.rightHandValid;
        if (!m_WorldPoseTrackingSnapshot.initialized ||
            !l4d2vr_dual::ControllerFrameAvailable(m_WorldPoseTrackingSnapshot.hmdValid, handValid,
                m_WorldPoseTrackingSnapshot.capturedAtMs, now)) return false;
    }
    VR* mutableVr = const_cast<VR*>(this);
    const Vector origin = left ? mutableVr->GetLeftControllerAbsPos() : mutableVr->GetRightControllerAbsPos();
    const QAngle angles = left ? mutableVr->GetLeftControllerAbsAngle() : mutableVr->GetRightControllerAbsAngle();
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(angles.x) || !std::isfinite(angles.y) || !std::isfinite(angles.z)) return false;
    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(angles, &forward, &right, &up);
    outWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(this, origin, forward, right, up);
    return MagazineInteractionMatrixLooksRenderable(outWorld);
}

bool VR::ReadLeftPistolGloveWrist(Vector& wrist) const
{
    if (!m_VrHandsEnabled || !m_VrHandsGlovesEnabled || !m_VrHands || m_VrHandsTwoHandedGripActive) return false;
    Vector local{};
    if (!m_VrHands->ReadFreeWristModelPosition(IsGameplayHandLeftPhysical(true),m_InputActionFrame,local)) return false;
    VrHandMatrix4 tracked{};
    if (!BuildPistolControllerWorld(true,tracked)) return false;
    VR* mutableVr = const_cast<VR*>(this);
    const auto glove = VrHandMath::BuildControllerWorld(mutableVr->GetLeftControllerAbsPos(),
        mutableVr->GetLeftControllerAbsAngle(),m_VRScale,std::clamp(m_VrHandsModelScale,0.25f,4.0f),
        m_VrHandsLeftPoseOffsetMeters,m_VrHandsLeftPoseRotationOffsetDeg);
    wrist = MagazineInteractionMatrixPointWorld(glove,local);
    Vector projected{};
    if (!l4d2vr_interaction::Finite(wrist) ||
        !MagazineInteractionReprojectScenePointToViewmodelLayer(this,wrist,projected)) return false;
    wrist = projected;
    return l4d2vr_interaction::Finite(wrist);
}

void VR::PublishMagazineInteractionBox(
    const Vector& origin,
    const Vector& axisX,
    const Vector& axisY,
    const Vector& axisZ,
    const Vector& mins,
    const Vector& maxs,
    uint32_t frameSeq,
    int entityIndex,
    int boneIndex,
    const char* modelName,
    bool modelBasisValid,
    const Vector& modelOrigin,
    const Vector& modelAxisX,
    const Vector& modelAxisY,
    const Vector& modelAxisZ)
{
    if (t_MagazineContactHand == l4d2vr_dual::Hand::None) return;
    if (!m_MagazineInteractionEnabled &&
        !m_MagazineBoxDebugEnabled &&
        !m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed) &&
        !(m_IsVREnabled && (m_VrHandsEnabled || m_NativeViewmodelHandsOnly)))
    {
        return;
    }

    if (!l4d2vr_interaction::ValidBox(origin, axisX, axisY, axisZ, mins, maxs))
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionBoxMutex);
        m_MagazineInteractionBoxValid = false;
        m_MagazineInteractionBox = {};
        return;
    }

    MagazineInteractionBoxSnapshot snapshot{};
    snapshot.origin = origin;
    snapshot.axisX = MagazineInteractionNormalizeAxis(axisX, Vector(1.0f, 0.0f, 0.0f));
    snapshot.axisY = MagazineInteractionNormalizeAxis(axisY, Vector(0.0f, 1.0f, 0.0f));
    snapshot.axisZ = MagazineInteractionNormalizeAxis(axisZ, Vector(0.0f, 0.0f, 1.0f));
    snapshot.modelBasisValid = modelBasisValid;
    if (modelBasisValid)
    {
        snapshot.modelOrigin = modelOrigin;
        snapshot.modelAxisX = MagazineInteractionNormalizeAxis(modelAxisX, Vector(1.0f, 0.0f, 0.0f));
        snapshot.modelAxisY = MagazineInteractionNormalizeAxis(modelAxisY, Vector(0.0f, 1.0f, 0.0f));
        snapshot.modelAxisZ = MagazineInteractionNormalizeAxis(modelAxisZ, Vector(0.0f, 0.0f, 1.0f));
    }
    snapshot.anchorHand = t_MagazineContactHand;
    VrHandMatrix4 viewmodelAnchorWorld{};
    if (MagazineInteractionBuildCurrentViewmodelModelBasisWorld(this, viewmodelAnchorWorld, snapshot.anchorHand))
    {
        snapshot.viewmodelAnchorBasisValid = true;
        snapshot.viewmodelAnchorOrigin = MagazineInteractionMatrixOrigin(viewmodelAnchorWorld);
        snapshot.viewmodelAnchorAxisX = MagazineInteractionMatrixAxis(viewmodelAnchorWorld, 0);
        snapshot.viewmodelAnchorAxisY = MagazineInteractionMatrixAxis(viewmodelAnchorWorld, 1);
        snapshot.viewmodelAnchorAxisZ = MagazineInteractionMatrixAxis(viewmodelAnchorWorld, 2);
        if (snapshot.anchorHand == l4d2vr_dual::Hand::Left && snapshot.modelBasisValid)
        {
            VrHandMatrix4 nativeAnchor{}, inverse{}, modelWorld{};
            if (!MagazineInteractionBuildCurrentViewmodelModelBasisWorld(this, nativeAnchor) ||
                !VrHandMath::Invert4x4(nativeAnchor, inverse) ||
                !MagazineInteractionBuildModelBasisWorld(snapshot, modelWorld)) return;
            const auto transfer = VrHandMath::Multiply(viewmodelAnchorWorld, inverse);
            modelWorld = VrHandMath::Multiply(transfer, modelWorld);
            if (!MagazineInteractionMatrixLooksRenderable(modelWorld)) return;
            snapshot.modelOrigin = MagazineInteractionMatrixOrigin(modelWorld);
            snapshot.modelAxisX = MagazineInteractionMatrixAxis(modelWorld, 0);
            snapshot.modelAxisY = MagazineInteractionMatrixAxis(modelWorld, 1);
            snapshot.modelAxisZ = MagazineInteractionMatrixAxis(modelWorld, 2);
        }
    }
    else if (snapshot.anchorHand == l4d2vr_dual::Hand::Left) return;
    snapshot.mins = mins;
    snapshot.maxs = maxs;
    snapshot.frameSeq = frameSeq;
    snapshot.entityIndex = entityIndex;
    snapshot.boneIndex = boneIndex;
    snapshot.modelName = modelName ? modelName : "";
    snapshot.publishedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoxMutex);
    snapshot.publishSeq = ++m_MagazineInteractionPublishSeq;
    m_MagazineInteractionBox = snapshot;
    m_MagazineInteractionBoxValid = true;
}

void VR::PublishMagazineInteractionBoltBox(
    const Vector& origin,
    const Vector& axisX,
    const Vector& axisY,
    const Vector& axisZ,
    const Vector& mins,
    const Vector& maxs,
    const Vector& pullAxisWorld,
    uint32_t frameSeq,
    int entityIndex,
    int boneIndex,
    const char* modelName)
{
    if (t_MagazineContactHand == l4d2vr_dual::Hand::None) return;
    if (!m_MagazineInteractionEnabled &&
        !m_MagazineBoxDebugEnabled &&
        !m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed))
    {
        return;
    }

    if (!l4d2vr_interaction::ValidBox(origin, axisX, axisY, axisZ, mins, maxs))
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
        m_MagazineInteractionBoltBoxValid = false;
        m_MagazineInteractionBoltBox = {};
        return;
    }

    MagazineInteractionBoxSnapshot snapshot{};
    snapshot.origin = origin;
    snapshot.axisX = MagazineInteractionNormalizeAxis(axisX, Vector(1.0f, 0.0f, 0.0f));
    snapshot.axisY = MagazineInteractionNormalizeAxis(axisY, Vector(0.0f, 1.0f, 0.0f));
    snapshot.axisZ = MagazineInteractionNormalizeAxis(axisZ, Vector(0.0f, 0.0f, 1.0f));
    snapshot.pullAxisWorld = MagazineInteractionNormalizeAxis(pullAxisWorld, Vector(0.0f, 0.0f, 0.0f));
    snapshot.mins = mins;
    snapshot.maxs = maxs;
    snapshot.frameSeq = frameSeq;
    snapshot.entityIndex = entityIndex;
    snapshot.boneIndex = boneIndex;
    snapshot.modelName = modelName ? modelName : "";
    snapshot.publishedAt = std::chrono::steady_clock::now();

    snapshot.anchorHand = t_MagazineContactHand;
    VrHandMatrix4 anchor{};
    if (MagazineInteractionBuildCurrentViewmodelModelBasisWorld(this, anchor, snapshot.anchorHand))
    {
        snapshot.viewmodelAnchorBasisValid = true;
        snapshot.viewmodelAnchorOrigin = MagazineInteractionMatrixOrigin(anchor);
        snapshot.viewmodelAnchorAxisX = MagazineInteractionMatrixAxis(anchor, 0);
        snapshot.viewmodelAnchorAxisY = MagazineInteractionMatrixAxis(anchor, 1);
        snapshot.viewmodelAnchorAxisZ = MagazineInteractionMatrixAxis(anchor, 2);
    }
    else if (snapshot.anchorHand == l4d2vr_dual::Hand::Left) return;

    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
    snapshot.publishSeq = ++m_MagazineInteractionBoltPublishSeq;
    m_MagazineInteractionBoltBox = snapshot;
    m_MagazineInteractionBoltBoxValid = true;
}

void VR::PublishVrHandsTwoHandedGripWeaponBox(
    const Vector& origin,
    const Vector& axisX,
    const Vector& axisY,
    const Vector& axisZ,
    const Vector& mins,
    const Vector& maxs,
    uint32_t frameSeq,
    int entityIndex,
    const char* modelName)
{
    if (!m_IsVREnabled || (!m_VrHandsEnabled && !m_NativeViewmodelHandsOnly))
        return;

    if (!l4d2vr_interaction::ValidBox(origin, axisX, axisY, axisZ, mins, maxs))
    {
        std::lock_guard<std::mutex> lock(m_VrHandsTwoHandedGripWeaponBoxMutex);
        m_VrHandsTwoHandedGripWeaponBoxValid = false;
        m_VrHandsTwoHandedGripWeaponBox = {};
        return;
    }

    MagazineInteractionBoxSnapshot snapshot{};
    snapshot.origin = origin;
    snapshot.axisX = MagazineInteractionNormalizeAxis(axisX, Vector(1.0f, 0.0f, 0.0f));
    snapshot.axisY = MagazineInteractionNormalizeAxis(axisY, Vector(0.0f, 1.0f, 0.0f));
    snapshot.axisZ = MagazineInteractionNormalizeAxis(axisZ, Vector(0.0f, 0.0f, 1.0f));
    snapshot.mins = mins;
    snapshot.maxs = maxs;
    snapshot.frameSeq = frameSeq;
    snapshot.entityIndex = entityIndex;
    snapshot.boneIndex = -1;
    snapshot.modelName = modelName ? modelName : "";
    snapshot.publishedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_VrHandsTwoHandedGripWeaponBoxMutex);
    snapshot.publishSeq = ++m_VrHandsTwoHandedGripWeaponBoxPublishSeq;
    m_VrHandsTwoHandedGripWeaponBox = snapshot;
    m_VrHandsTwoHandedGripWeaponBoxValid = true;
}

void VR::PublishMagazineInteractionNativeLeftWristAnchor(
    const Vector& origin,
    const Vector& axisX,
    const Vector& axisY,
    const Vector& axisZ)
{
    if (!m_MagazineInteractionEnabled &&
        !m_MagazineBoxDebugEnabled &&
        !m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed))
    {
        return;
    }

    const Vector normalizedX = MagazineInteractionNormalizeAxis(axisX, Vector(1.0f, 0.0f, 0.0f));
    const Vector normalizedY = MagazineInteractionNormalizeAxis(axisY, Vector(0.0f, 1.0f, 0.0f));
    const Vector normalizedZ = MagazineInteractionNormalizeAxis(axisZ, Vector(0.0f, 0.0f, 1.0f));

    VrHandMatrix4 world = VrHandMath::Identity();
    VrHandMath::Set(world, 0, 0, normalizedX.x);
    VrHandMath::Set(world, 1, 0, normalizedX.y);
    VrHandMath::Set(world, 2, 0, normalizedX.z);
    VrHandMath::Set(world, 0, 1, normalizedY.x);
    VrHandMath::Set(world, 1, 1, normalizedY.y);
    VrHandMath::Set(world, 2, 1, normalizedY.z);
    VrHandMath::Set(world, 0, 2, normalizedZ.x);
    VrHandMath::Set(world, 1, 2, normalizedZ.y);
    VrHandMath::Set(world, 2, 2, normalizedZ.z);
    VrHandMath::Set(world, 0, 3, origin.x);
    VrHandMath::Set(world, 1, 3, origin.y);
    VrHandMath::Set(world, 2, 3, origin.z);

    if (!MagazineInteractionMatrixLooksRenderable(world))
        return;

    std::lock_guard<std::mutex> lock(m_MagazineInteractionHandAnchorMutex);
    m_MagazineInteractionNativeLeftWristWorld = world;
    m_MagazineInteractionNativeLeftWristPublishedAt = std::chrono::steady_clock::now();
    m_MagazineInteractionNativeLeftWristValid = true;
}

void VR::PublishMagazineInteractionCalibrationSnapshot(
    const char* modelName,
    const char* sourceClassName,
    uint32_t modelFingerprint,
    uint32_t boneSignature,
    uint32_t renderFrameSeq,
    int entityIndex,
    int weaponId,
    int inferredWeaponId,
    int sourceScore,
    int numBones,
    int recommendedMagazineBone,
    int recommendedBoltBone,
    bool sourceIsViewmodelClass,
    const std::vector<MagazineInteractionCalibrationBone>& bones)
{
    if (!modelName || !*modelName || numBones <= 0)
        return;

    MagazineInteractionCalibrationSnapshot snapshot{};
    snapshot.valid = true;
    snapshot.modelName = modelName;
    snapshot.sourceClassName = (sourceClassName && *sourceClassName) ? sourceClassName : "";
    snapshot.modelFingerprint = modelFingerprint;
    snapshot.boneSignature = boneSignature;
    snapshot.renderFrameSeq = renderFrameSeq;
    snapshot.entityIndex = entityIndex;
    snapshot.weaponId = weaponId;
    snapshot.inferredWeaponId = inferredWeaponId;
    snapshot.sourceScore = sourceScore;
    snapshot.numBones = numBones;
    snapshot.recommendedMagazineBone = recommendedMagazineBone;
    snapshot.recommendedBoltBone = recommendedBoltBone;
    snapshot.sourceIsViewmodelClass = sourceIsViewmodelClass;
    snapshot.bones = bones;
    const auto now = std::chrono::steady_clock::now();
    snapshot.publishedAt = now;

    std::lock_guard<std::mutex> lock(m_MagazineInteractionCalibrationMutex);
    bool sameSelectionWindow = false;
    float existingAgeSeconds = 9999.0f;
    if (m_MagazineInteractionCalibrationSnapshot.valid)
    {
        existingAgeSeconds = std::chrono::duration<float>(
            now - m_MagazineInteractionCalibrationSnapshot.publishedAt).count();
        if (renderFrameSeq != 0 &&
            m_MagazineInteractionCalibrationSnapshot.renderFrameSeq == renderFrameSeq)
        {
            sameSelectionWindow = true;
        }
        else if (renderFrameSeq == 0 &&
            m_MagazineInteractionCalibrationSnapshot.renderFrameSeq == 0 &&
            existingAgeSeconds >= 0.0f &&
            existingAgeSeconds < 0.10f)
        {
            sameSelectionWindow = true;
        }
    }

    const bool currentSnapshotIsStrongViewmodel =
        m_MagazineInteractionCalibrationSnapshot.valid &&
        existingAgeSeconds >= 0.0f &&
        existingAgeSeconds < 1.5f &&
        m_MagazineInteractionCalibrationSnapshot.sourceScore >= 900;
    const bool candidateIsWeakNonViewmodel = sourceScore < 900;
    const bool shouldKeepCurrent =
        m_MagazineInteractionCalibrationSnapshot.valid &&
        m_MagazineInteractionCalibrationSnapshot.sourceScore > sourceScore &&
        (sameSelectionWindow || (currentSnapshotIsStrongViewmodel && candidateIsWeakNonViewmodel));
    if (shouldKeepCurrent)
        return;

    snapshot.publishSeq = ++m_MagazineInteractionCalibrationPublishSeq;
    m_MagazineInteractionCalibrationSnapshot = std::move(snapshot);
}

bool VR::GetMagazineInteractionBox(MagazineInteractionBoxSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoxMutex);
    if (!m_MagazineInteractionBoxValid ||
        !l4d2vr_dual::ContactHandMatches(m_MagazineInteractionBox.anchorHand, GetPrimaryPistolHand()))
        return false;
    outSnapshot = m_MagazineInteractionBox;
    return true;
}

bool VR::GetMagazineInteractionBoltBox(MagazineInteractionBoxSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
    if (!m_MagazineInteractionBoltBoxValid ||
        !l4d2vr_dual::ContactHandMatches(m_MagazineInteractionBoltBox.anchorHand, GetPrimaryPistolHand()))
        return false;
    outSnapshot = m_MagazineInteractionBoltBox;
    return true;
}

bool VR::GetVrHandsTwoHandedGripWeaponBox(MagazineInteractionBoxSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_VrHandsTwoHandedGripWeaponBoxMutex);
    if (!m_VrHandsTwoHandedGripWeaponBoxValid)
        return false;
    outSnapshot = m_VrHandsTwoHandedGripWeaponBox;
    return true;
}

bool VR::GetMagazineInteractionNativeLeftWristAnchor(VrHandMatrix4& outWorld) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionHandAnchorMutex);
    if (!m_MagazineInteractionNativeLeftWristValid ||
        m_MagazineInteractionNativeLeftWristPublishedAt.time_since_epoch().count() == 0)
    {
        return false;
    }

    const float staleSeconds = std::max(0.02f, m_MagazineInteractionStaleSeconds);
    const float ageSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() -
        m_MagazineInteractionNativeLeftWristPublishedAt).count();
    if (ageSeconds > staleSeconds)
        return false;

    outWorld = m_MagazineInteractionNativeLeftWristWorld;
    return MagazineInteractionMatrixLooksRenderable(outWorld);
}

void VR::RecordDualPistolCommand(int command, l4d2vr_dual::Hand hand, bool firing, uintptr_t owner, uintptr_t weapon)
{
    l4d2vr_dual::Shot shot{};
    shot.command = command;
    shot.hand = hand;
    shot.owner = owner; shot.weapon = weapon;
    shot.capturedAtMs = GetTickCount64();
    shot.weaponHandle = MagazineInteractionActiveWeaponHandle(m_Game,
        reinterpret_cast<C_BasePlayer*>(owner), reinterpret_cast<C_WeaponCSBase*>(weapon));
    {
        const unsigned hands = m_IsVREnabled && m_DualPistolsIndependentHandsEnabled && !m_MouseModeEnabled
            ? (m_DualPistolsActive.load(std::memory_order_acquire) ? 3u :
                m_LeftHandPistolActive.load(std::memory_order_acquire) ? 2u : 0u) : 0u;
        std::lock_guard<std::mutex> lock(m_PistolMuzzleMutex);
        m_PistolMuzzles.Observe({owner, weapon, shot.weaponHandle, hands});
    }
    if (hand != l4d2vr_dual::Hand::None)
    {
        const bool left = hand == l4d2vr_dual::Hand::Left;
        const Vector position = left ? GetLeftControllerAbsPos() : GetRightControllerAbsPos();
        const QAngle angles = left ? GetLeftControllerAbsAngle() : GetRightControllerAbsAngle();
        shot.position = { position.x, position.y, position.z };
        shot.angles = { angles.x, angles.y, angles.z };
    }
    std::lock_guard<std::mutex> lock(m_DualPistolShotMutex);
    m_DualPistolCommandShots.Store(shot);
    if (firing && hand != l4d2vr_dual::Hand::None)
        m_LatestDualPistolShot = shot;
    else if (!m_DualPistolsActive.load(std::memory_order_acquire) && !m_LeftHandPistolActive.load(std::memory_order_acquire))
        m_LatestDualPistolShot = {};
}

bool VR::GetDualPistolCommandPose(int command, Vector& position, QAngle& angles, l4d2vr_dual::Hand& hand) const
{
    std::lock_guard<std::mutex> lock(m_DualPistolShotMutex);
    l4d2vr_dual::Shot shot{};
    if (!m_DualPistolCommandShots.Get(command, shot)) return false;
    position = Vector(shot.position[0], shot.position[1], shot.position[2]);
    angles = QAngle(shot.angles[0], shot.angles[1], shot.angles[2]);
    hand = shot.hand;
    return true;
}

bool VR::GetLatestDualPistolShotPose(uintptr_t owner, uintptr_t weapon, Vector& position,
    QAngle& angles, l4d2vr_dual::Hand& hand) const
{
    std::lock_guard<std::mutex> lock(m_DualPistolShotMutex);
    const auto& shot = m_LatestDualPistolShot;
    if (!l4d2vr_dual::LiveShotMatches(shot, owner, weapon, GetTickCount64())) return false;
    position = Vector(shot.position[0], shot.position[1], shot.position[2]);
    angles = QAngle(shot.angles[0], shot.angles[1], shot.angles[2]);
    hand = shot.hand;
    return true;
}

bool VR::GetExecutingDualPistolShotPose(int command, uintptr_t owner, uintptr_t weapon,
    Vector& position, QAngle& angles, l4d2vr_dual::Hand& hand) const
{
    const auto handle = MagazineInteractionActiveWeaponHandle(m_Game,
        reinterpret_cast<C_BasePlayer*>(owner), reinterpret_cast<C_WeaponCSBase*>(weapon));
    std::lock_guard<std::mutex> lock(m_DualPistolShotMutex);
    l4d2vr_dual::Shot shot{};
    if (!m_DualPistolCommandShots.GetForExecution(command, owner, weapon, handle, GetTickCount64(), shot)) return false;
    position = Vector(shot.position[0], shot.position[1], shot.position[2]);
    angles = QAngle(shot.angles[0], shot.angles[1], shot.angles[2]);
    hand = shot.hand;
    return true;
}

l4d2vr_pistol_fx::Key VR::ReadPistolMuzzleKey() const
{
    std::lock_guard<std::mutex> lock(m_PistolMuzzleMutex);
    return m_PistolMuzzles.Current();
}

void VR::PublishPistolMuzzle(const l4d2vr_pistol_fx::Key& key, l4d2vr_dual::Hand hand,
    const l4d2vr_dual::Matrix3x4& controller, const l4d2vr_dual::Matrix3x4& muzzle)
{
    std::lock_guard<std::mutex> lock(m_PistolMuzzleMutex);
    m_PistolMuzzles.Capture(key, hand, controller, muzzle, m_VRScale, GetTickCount64());
}

bool VR::GetExecutingPistolMuzzle(int command, uintptr_t owner, uintptr_t weapon, Vector& origin) const
{
    Vector position{}; QAngle angles{}; l4d2vr_dual::Hand hand = l4d2vr_dual::Hand::None;
    if (!GetExecutingDualPistolShotPose(command, owner, weapon, position, angles, hand)) return false;
    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(angles, &forward, &right, &up);
    const auto controller = l4d2vr_dual::ControllerFrame<l4d2vr_dual::Matrix3x4>(
        {position.x,position.y,position.z}, {forward.x,forward.y,forward.z},
        {right.x,right.y,right.z}, {up.x,up.y,up.z});
    const auto handle = MagazineInteractionActiveWeaponHandle(m_Game,
        reinterpret_cast<C_BasePlayer*>(owner), reinterpret_cast<C_WeaponCSBase*>(weapon));
    std::lock_guard<std::mutex> lock(m_PistolMuzzleMutex);
    const auto key = m_PistolMuzzles.Current();
    if (key.owner != owner || key.weapon != weapon || key.handle != handle) return false;
    l4d2vr_dual::Matrix3x4 muzzle{};
    if (!m_PistolMuzzles.Resolve(key, hand, controller, GetTickCount64(), muzzle)) return false;
    origin = Vector(muzzle.m[0][3],muzzle.m[1][3],muzzle.m[2][3]);
    return true;
}

void VR::OfferRemoteShellProtocol(unsigned version, uint32_t token)
{
    std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
    m_RemoteShellRequest.Offer(version, token);
    if (m_RemoteShellRequest.Supported())
        Game::logMsg("[VR][RemoteShell][client] protocol=%u offered", version);
}
void VR::ReceiveRemoteShellReply(const l4d2vr_shell::Reply& reply)
{
    std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
    m_RemoteShellRequest.Receive(reply);
}
void VR::DisconnectRemoteShellProtocol()
{
    std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
    m_RemoteShellRequest.Disconnect();
}
void VR::CancelRemoteShellRequest()
{
    std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
    m_RemoteShellRequest.Cancel();
}
bool VR::RemoteShellProtocolSupported() const
{
    if (!m_IsVREnabled || !m_EncodeVRUsercmd || m_ForceNonVRServerMovement ||
        !l4d2vr_wire::SupportsPhysicalVersion(m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)))
        return false;
    std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
    return m_RemoteShellRequest.Supported();
}
bool VR::RemoteShellRequestPending() const
{
    std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
    return m_RemoteShellRequest.Pending();
}
void VR::UpdatePhysicalScopeWeapon(C_BasePlayer* owner, C_WeaponCSBase* weapon)
{
    const int id = weapon ? static_cast<int>(weapon->GetWeaponID()) : 0;
    const uint32_t handle = l4d2vr_optics::HasOptic(id)
        ? MagazineInteractionActiveWeaponHandle(m_Game, owner, weapon) : 0u;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    if (m_PhysicalScopeWeapons.Observe(reinterpret_cast<uintptr_t>(owner), handle, id))
    {
        m_PhysicalScopeLenses[0] = {}; m_PhysicalScopeLenses[1] = {};
        m_PhysicalScopeImage = {}; m_PhysicalScopeProcessedImage = {}; m_PhysicalScopeRender = {};
    }
    m_PhysicalScopeWeaponId.store(m_PhysicalScopeWeapons.Current().Valid() ? id : 0, std::memory_order_release);
}

bool VR::HasFreshMagazineInteractionTracking() const
{
    if (!IsInputActionFrameAvailable() || !IsMagazineGripInputAvailable())
        return false;
    VRWorldPoseTrackingSnapshot pose{};
    if (!ReadWorldPoseTrackingSnapshot(pose) ||
        !l4d2vr_magazine::TrackingAllowsGesture(pose.hmdValid, pose.leftHandValid, pose.rightHandValid,
            pose.capturedAtMs, GetTickCount64())) return false;
    const auto finiteAngles = [](const QAngle& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    return l4d2vr_interaction::Finite(pose.hmdPosition) && finiteAngles(pose.hmdAngles) &&
        l4d2vr_interaction::Finite(pose.leftHandPosition) && finiteAngles(pose.leftHandAngles) &&
        l4d2vr_interaction::Finite(pose.rightHandPosition) && finiteAngles(pose.rightHandAngles);
}

bool VR::BeginRemoteShellRequest(uint32_t handle, int id, int clip, int reserve,
    uintptr_t owner, uint32_t generation)
{
    if (!RemoteShellProtocolSupported() || !m_Game || !m_Game->m_EngineClient ||
        m_Game->m_EngineClient->IsPaused() || m_MouseModeEnabled || m_SuppressPlayerInput ||
        !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) ||
        m_RenderPlayerControlledBySI.load(std::memory_order_acquire) ||
        (m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible())) return false;
    if (!HasFreshMagazineInteractionTracking()) return false;
    l4d2vr_shell::Request request{};
    {
        std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
        if (m_CurrentPredictedHitFeedbackCmdNumber <= 0 || !m_RemoteShellRequest.Begin(handle, id, clip, reserve,
            static_cast<uint32_t>(m_CurrentPredictedHitFeedbackCmdNumber), owner, generation, GetTickCount64(), request))
            return false;
    }
    SendRemoteShellRequest(request,false);
    return true;
}

void VR::SendRemoteShellRequest(const l4d2vr_shell::Request& request, bool retry)
{
    if (!m_Game) return;
    char command[192]{};
    std::snprintf(command, sizeof(command), "l4d2vr_shell_insert_v1 %u %u %u %u %d %d %d\n",
        request.token, request.sequence, request.handle, request.command, request.weaponId, request.clip, request.reserve);
    m_Game->ServerCmd(command, true);
    Game::logMsg("[VR][RemoteShell][client] %s seq=%u weaponId=%d clip=%d reserve=%d",
        retry ? "retry" : "requested", request.sequence, request.weaponId, request.clip, request.reserve);
}
l4d2vr_shell::ClientRequest::Poll VR::PollRemoteShellReply(uint32_t handle, int id,
    uintptr_t owner, uint32_t generation, int clip, int reserve, l4d2vr_shell::Reply& reply)
{
    l4d2vr_shell::Request retry{};
    bool sendRetry = false;
    l4d2vr_shell::ClientRequest::Poll result{};
    {
        std::lock_guard<std::mutex> lock(m_RemoteShellMutex);
        const auto now = GetTickCount64();
        result = m_RemoteShellRequest.Update(handle,id,owner,generation,now,reply,clip,reserve);
        if (result == l4d2vr_shell::ClientRequest::Poll::Waiting && m_Game && m_Game->m_EngineClient &&
            !m_Game->m_EngineClient->IsPaused() && !m_MouseModeEnabled && !m_SuppressPlayerInput &&
            m_FirstPersonControlReady.load(std::memory_order_acquire) &&
            !m_RenderPlayerIncap.load(std::memory_order_acquire) && !m_RenderPlayerControlledBySI.load(std::memory_order_acquire))
            sendRetry = m_RemoteShellRequest.NextRetry(now,retry);
    }
    // Replies may be dispatched synchronously. Never send while owning the request mutex.
    if (sendRetry) SendRemoteShellRequest(retry,true);
    return result;
}

void VR::OfferRemoteMagazineProtocol(unsigned version, uint32_t token)
{
    std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
    m_RemoteMagazineRequest.Offer(version, token);
}
void VR::OfferPistolAmmoProtocol(unsigned version, uint32_t token)
{
    bool accepted = false;
    const bool ammoModeAvailable = m_Game && m_Game->GetConVarIntDirect("sv_infinite_ammo", -1) >= 0;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
        const auto previousToken = m_PistolAmmoClient.Token();
        if (!m_PistolMagazineRequest.Blocked(token) && ammoModeAvailable && m_IsVREnabled && m_EncodeVRUsercmd && !m_ForceNonVRServerMovement &&
            l4d2vr_wire::SupportsPhysicalVersion(m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) &&
            m_DualPistolsIndependentHandsEnabled && Hooks::hkClientPistolRunCommand.isEnabled &&
            Hooks::hkClientPistolGunFire.isEnabled)
            accepted = m_PistolAmmoClient.Offer(version, token);
        else m_PistolAmmoClient.Disconnect();
        if (accepted) m_PistolMagazineRequest.Offer(token);
        else if (!m_PistolMagazineRequest.Blocked(token)) m_PistolMagazineRequest.Disconnect();
        if (!accepted || m_PistolAmmoClient.Token() != previousToken)
        {
            m_PistolAmmoPrediction.Reset();
            m_PistolMagazineReplyReady = false;
            m_PistolAmmoCounts.store(0xffffffffffffffffull, std::memory_order_release);
        }
    }
    if (m_Game && version == l4d2vr_pistol_sync::kVersion && token)
    {
        char command[96]{};
        std::snprintf(command, sizeof(command), "l4d2vr_pistol_ammo_ack %u %u\n",
            accepted ? l4d2vr_pistol_sync::kVersion : 0u, token);
        m_Game->ServerCmd(command, true);
    }
}
void VR::ReceivePistolAmmoState(const l4d2vr_pistol_sync::State& state)
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    m_PistolAmmoClient.Receive(state, GetTickCount64());
}
void VR::DisconnectPistolAmmoProtocol()
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    m_PistolAmmoClient.Disconnect();
    m_PistolAmmoPrediction.Reset();
    m_PistolMagazineRequest.Disconnect(); m_PistolMagazineReplyReady = false;
    m_PistolAmmoCounts.store(0xffffffffffffffffull, std::memory_order_release);
}
namespace
{
    bool ReadClientPistolAmmoSnapshot(Game* game, C_BasePlayer* player, C_WeaponCSBase* weapon,
        uint32_t& handle, int& clip, bool& dual)
    {
        if (!game || !player || !weapon) return false;
#ifdef _MSC_VER
        __try
        {
#endif
            if (weapon->GetWeaponID() != C_WeaponCSBase::WeaponID::PISTOL ||
                !MagazineInteractionTryReadInt(weapon, VR::kClip1Offset, clip)) return false;
            handle = MagazineInteractionActiveWeaponHandle(game, player, weapon);
            dual = game->IsDualPistolWeapon(weapon);
            return l4d2vr_shell::ValidHandle(handle) && clip >= 0 && clip <= 30;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
    }
    bool ReadClientPistolMagazineContext(Game* game, C_BasePlayer*& player, C_WeaponCSBase*& weapon,
        uint32_t& handle, int& clip, int& reserve, bool& dual)
    {
        if (!game || !game->m_EngineClient) return false;
#ifdef _MSC_VER
        __try
        {
#endif
            player = reinterpret_cast<C_BasePlayer*>(game->GetClientEntity(game->m_EngineClient->GetLocalPlayer()));
            weapon = player ? reinterpret_cast<C_WeaponCSBase*>(player->GetActiveWeapon()) : nullptr;
            int ammoType = -1;
            return ReadClientPistolAmmoSnapshot(game, player, weapon, handle, clip, dual) &&
                MagazineInteractionTryReadValue(weapon, VR::kPrimaryAmmoTypeOffset, ammoType) && ammoType >= 0 && ammoType < 32 &&
                MagazineInteractionTryReadValue(player, VR::kAmmoArrayOffset + ammoType * static_cast<int>(sizeof(int)), reserve);
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
    }
}
namespace
{
    void SendPistolMagazineRequest(Game* game, const l4d2vr_pistol_reload::Request& request)
    {
        if (!game) return;
        char command[192]{};
        std::snprintf(command, sizeof(command), "l4d2vr_pistol_mag_v1 %u %u %u %u %d %d %u %u\n",
            request.token, request.sequence, request.handle, request.command, request.clip, request.reserve,
            static_cast<unsigned>(request.hand), static_cast<unsigned>(request.action));
        game->ServerCmd(command, true);
    }
}
void VR::UpdatePistolAmmoSnapshot(C_BasePlayer* player, C_WeaponCSBase* weapon)
{
    int right = -1, left = -1, clip = -1;
    uint32_t handle = 0u; bool dual = false;
    const bool nativeReady = m_Game && player && weapon && m_IsVREnabled && m_EncodeVRUsercmd && !m_ForceNonVRServerMovement &&
        m_DualPistolsIndependentHandsEnabled && m_FirstPersonControlReady.load(std::memory_order_acquire) &&
        Hooks::hkClientPistolRunCommand.isEnabled && Hooks::hkClientPistolGunFire.isEnabled &&
        !m_RenderPlayerIncap.load(std::memory_order_relaxed) && !m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) &&
        l4d2vr_wire::SupportsPhysicalVersion(m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) &&
        ReadClientPistolAmmoSnapshot(m_Game, player, weapon, handle, clip, dual);
    if (!nativeReady) handle = 0u;
    l4d2vr_pistol_sync::State state{};
    // Serialize publication with disconnect and renegotiation, so an older input
    // sample cannot restore counts after those paths cleared the session.
    uint32_t revokeToken = 0u;
    l4d2vr_pistol_reload::Request retry{};
    bool resend = false;
    {
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    const uint64_t now = GetTickCount64();
    const auto owner = handle ? reinterpret_cast<uintptr_t>(player) : 0u;
    if (!m_PistolAmmoClient.ReadBaseline(owner, handle, dual, now, state))
        m_PistolAmmoPrediction.Reset();
    else if (m_PistolAmmoPrediction.Refresh(owner, state))
        m_PistolAmmoPrediction.Counts(clip, 0u, 0u, right, left);
    const uint64_t packed = l4d2vr_pistol_prediction::Pack(right, left, state.physical, state.chambered);
    l4d2vr_pistol_reload::Reply reloadReply{};
    const bool reloadAvailable = m_MagazineInteractionEnabled &&
        Hooks::hkClientPistolReload.isEnabled && Hooks::hkClientPistolFinishReload.isEnabled;
    const auto reloadPoll = m_PistolMagazineRequest.Update(reloadAvailable ? owner : 0u, handle, dual, now, state,
        packed != 0xffffffffffffffffull, reloadReply);
    if (reloadPoll == l4d2vr_pistol_reload::Client::Poll::Result || reloadPoll == l4d2vr_pistol_reload::Client::Poll::Timeout)
    { m_PistolMagazineReply = reloadReply; m_PistolMagazineReplyReady = true; }
    if (reloadPoll == l4d2vr_pistol_reload::Client::Poll::Timeout)
    {
        revokeToken = m_PistolAmmoClient.Token();
        m_PistolAmmoClient.Disconnect(); m_PistolAmmoPrediction.Reset();
        m_PistolAmmoCounts.store(0xffffffffffffffffull, std::memory_order_release);
    }
    else m_PistolAmmoCounts.store(packed, std::memory_order_release);
    if (reloadPoll == l4d2vr_pistol_reload::Client::Poll::Waiting &&
        !m_MouseModeEnabled && !m_SuppressPlayerInput &&
        m_Game && m_Game->m_EngineClient &&
        !m_Game->m_EngineClient->IsPaused() &&
        !(m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible()))
        resend = m_PistolMagazineRequest.NextRetry(now, retry);
    }
    // A native command can synchronously deliver its reply through this mutex.
    if (resend)
    {
        SendPistolMagazineRequest(m_Game, retry);
        Game::logMsg("[VR][PistolMagazine][client] retry seq=%u hand=%u action=%u",
            retry.sequence, static_cast<unsigned>(retry.hand), static_cast<unsigned>(retry.action));
    }
    if (revokeToken && m_Game)
    {
        char command[96]{};
        std::snprintf(command, sizeof(command), "l4d2vr_pistol_ammo_ack 0 %u\n", revokeToken);
        m_Game->ServerCmd(command, true);
    }
}

bool VR::PreparePistolPredictionFire(C_BasePlayer* player, C_WeaponCSBase* weapon, int command,
    unsigned ordinal, l4d2vr_dual::Hand hand, l4d2vr_pistol_prediction::Fire& fire, bool& blocked)
{
    blocked = false;
    int clip = -1; uint32_t handle = 0u; bool dual = false;
    if (!m_IsVREnabled || !m_EncodeVRUsercmd || m_ForceNonVRServerMovement ||
        !l4d2vr_wire::SupportsPhysicalVersion(m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) ||
        !m_DualPistolsIndependentHandsEnabled || !Hooks::hkClientPistolRunCommand.isEnabled ||
        !Hooks::hkClientPistolGunFire.isEnabled || command <= 0 || !ordinal || ordinal > 64u ||
        (hand != l4d2vr_dual::Hand::Right && hand != l4d2vr_dual::Hand::Left) ||
        !ReadClientPistolAmmoSnapshot(m_Game, player, weapon, handle, clip, dual)) return false;
    const auto owner = reinterpret_cast<uintptr_t>(player);
    const bool consumesAmmo = m_Game->GetConVarIntDirect("sv_infinite_ammo", -1) == 0;
    l4d2vr_pistol_sync::State state{};
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    if (!m_PistolAmmoClient.ReadBaseline(owner, handle, dual, GetTickCount64(), state) ||
        !m_PistolAmmoPrediction.Refresh(owner, state)) return false;
    int right = -1, left = -1;
    m_PistolAmmoPrediction.Counts(clip, static_cast<uint32_t>(command), ordinal, right, left);
    blocked = l4d2vr_pistol_prediction::BlocksEmptyHand(dual, consumesAmmo, hand, right, left);
    const unsigned bit = l4d2vr_pistol::Bit(hand);
    blocked = blocked || (consumesAmmo && right >= 0 && left >= 0 &&
        (state.physical & bit) != 0u && (state.chambered & bit) == 0u);
    fire = {owner, reinterpret_cast<uintptr_t>(weapon), state.token, handle,
        static_cast<uint32_t>(command), ordinal, hand, clip, dual};
    fire.handRemainingAfter = l4d2vr_pistol_fx::RemainingAfterShot(consumesAmmo, hand, right, left);
    return true;
}

void VR::RecordPistolPredictionFire(const l4d2vr_pistol_prediction::Fire& fire, unsigned bullets)
{
    int clip = -1; uint32_t handle = 0u; bool dual = false;
    const bool nativeReady = ReadClientPistolAmmoSnapshot(m_Game,
        reinterpret_cast<C_BasePlayer*>(fire.owner), reinterpret_cast<C_WeaponCSBase*>(fire.weapon), handle, clip, dual);
    l4d2vr_pistol_sync::State state{};
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    if (fire.token != m_PistolAmmoClient.Token()) return;
    if (!nativeReady || handle != fire.handle || dual != fire.dual ||
        !m_PistolAmmoClient.ReadBaseline(fire.owner, handle, dual, GetTickCount64(), state))
        m_PistolAmmoPrediction.Reset();
    else if (m_PistolAmmoPrediction.Refresh(fire.owner, state))
        m_PistolAmmoPrediction.Record(fire.command, fire.ordinal, fire.hand, fire.clip, clip, bullets);
    int right = -1, left = -1;
    m_PistolAmmoPrediction.Counts(clip, 0u, 0u, right, left);
    m_PistolAmmoCounts.store(l4d2vr_pistol_prediction::Pack(right, left, state.physical, state.chambered), std::memory_order_release);
}

void VR::CancelPistolPredictionFire(const l4d2vr_pistol_prediction::Fire& fire)
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    if (fire.token == m_PistolAmmoClient.Token()) m_PistolAmmoPrediction.Cancel(fire.command, fire.ordinal);
}

namespace
{
    bool MagazineInteractionClearPistolReloadFlags(C_WeaponCSBase* weapon)
    {
        unsigned char inReload = 0u, fromEmpty = 0u; int stage = 0;
        bool changed = false;
        if (MagazineInteractionTryReadValue(weapon, 0x9bd, inReload) && inReload <= 1u && inReload)
            changed |= MagazineInteractionTryWriteValue<unsigned char>(weapon, 0x9bd, 0u);
        if (MagazineInteractionTryReadValue(weapon, 0xce8, stage) && stage >= 0 && stage <= 4 && stage)
            changed |= MagazineInteractionTryWriteValue<int>(weapon, 0xce8, 0);
        if (MagazineInteractionTryReadValue(weapon, 0xcef, fromEmpty) && fromEmpty <= 1u && fromEmpty)
            changed |= MagazineInteractionTryWriteValue<unsigned char>(weapon, 0xcef, 0u);
        return changed;
    }
}

bool VR::PistolMagazineProtocolSupported() const
{
    if (!m_Game || !m_IsVREnabled || !m_EncodeVRUsercmd || m_ForceNonVRServerMovement ||
        !m_MagazineInteractionEnabled || !m_DualPistolsIndependentHandsEnabled ||
        !Hooks::hkClientPistolRunCommand.isEnabled || !Hooks::hkClientPistolGunFire.isEnabled ||
        !Hooks::hkClientPistolReload.isEnabled || !Hooks::hkClientPistolFinishReload.isEnabled ||
        !l4d2vr_wire::SupportsPhysicalVersion(m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) ||
        !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_relaxed) || m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) ||
        m_Game->GetConVarIntDirect("sv_infinite_ammo", -1) != 0) return false;
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    return m_PistolAmmoClient.Token() != 0u;
}

bool VR::ReadPistolMagazineState(C_BasePlayer* player, C_WeaponCSBase* weapon, l4d2vr_pistol_sync::State& state)
{
    state = {};
    if (!PistolMagazineProtocolSupported()) return false;
    C_BasePlayer* currentPlayer = nullptr; C_WeaponCSBase* currentWeapon = nullptr;
    uint32_t handle = 0u; int clip = -1, reserve = -1; bool dual = false;
    if (!ReadClientPistolMagazineContext(m_Game, currentPlayer, currentWeapon, handle, clip, reserve, dual) ||
        player != currentPlayer || weapon != currentWeapon) return false;
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    return m_PistolAmmoClient.ReadBaseline(reinterpret_cast<uintptr_t>(player), handle, dual, GetTickCount64(), state);
}

bool VR::PistolMagazineRequestPending() const
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    return m_PistolMagazineRequest.Pending();
}

bool VR::BeginPistolMagazineRequest(int commandNumber, l4d2vr_pistol::Hand hand, l4d2vr_pistol::MagazineAction action)
{
    if (commandNumber <= 0 || !m_Game || !m_Game->m_EngineClient || !m_IsVREnabled || !m_MagazineInteractionEnabled || !m_DualPistolsIndependentHandsEnabled ||
        !Hooks::hkClientPistolReload.isEnabled || !Hooks::hkClientPistolFinishReload.isEnabled ||
        !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_Game->GetConVarIntDirect("sv_infinite_ammo", -1) != 0 || m_MouseModeEnabled || m_SuppressPlayerInput ||
        m_RenderPlayerIncap.load(std::memory_order_relaxed) || m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) ||
        m_Game->m_EngineClient->IsPaused() ||
        (m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible())) return false;
    C_BasePlayer* player = nullptr; C_WeaponCSBase* weapon = nullptr;
    uint32_t handle = 0u; int clip = -1, reserve = -1; bool dual = false;
    if (!ReadClientPistolMagazineContext(m_Game, player, weapon, handle, clip, reserve, dual)) return false;
    l4d2vr_pistol_sync::State available{};
    if (!ReadPistolMagazineState(player, weapon, available) || !HasFreshMagazineInteractionTracking()) return false;
    l4d2vr_pistol_reload::Request request{};
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
        l4d2vr_pistol_sync::State state{};
        if (!m_PistolAmmoClient.ReadBaseline(reinterpret_cast<uintptr_t>(player), handle, dual, GetTickCount64(), state) ||
            !m_PistolMagazineRequest.Begin(handle, clip, reserve, hand, action,
                static_cast<uint32_t>(commandNumber), reinterpret_cast<uintptr_t>(player), dual, GetTickCount64(), request))
            return false;
        m_PistolMagazineReplyReady = false;
    }
    SendPistolMagazineRequest(m_Game, request);
    Game::logMsg("[VR][PistolMagazine][client] requested seq=%u hand=%u action=%u clip=%d reserve=%d",
        request.sequence, static_cast<unsigned>(hand), static_cast<unsigned>(action), clip, reserve);
    return true;
}

void VR::ReceivePistolMagazineReply(const l4d2vr_pistol_reload::Reply& reply)
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    m_PistolMagazineRequest.Receive(reply);
}

bool VR::ConsumePistolMagazineReply(l4d2vr_pistol_reload::Reply& reply)
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
    if (!m_PistolMagazineReplyReady) return false;
    reply = m_PistolMagazineReply; m_PistolMagazineReplyReady = false; return true;
}

bool VR::BlockPistolNativeReload(C_WeaponCSBase* candidate)
{
    if (!m_Game || !candidate || !m_IsVREnabled || !m_EncodeVRUsercmd || m_ForceNonVRServerMovement ||
        !m_DualPistolsIndependentHandsEnabled || !m_MagazineInteractionEnabled ||
        !Hooks::hkClientPistolReload.isEnabled || !Hooks::hkClientPistolFinishReload.isEnabled ||
        !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_relaxed) || m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) ||
        m_Game->GetConVarIntDirect("sv_infinite_ammo", -1) != 0) return false;
    C_BasePlayer* player = nullptr; C_WeaponCSBase* weapon = nullptr;
    uint32_t handle = 0u; int clip = -1, reserve = -1; bool dual = false;
    if (!ReadClientPistolMagazineContext(m_Game, player, weapon, handle, clip, reserve, dual) || weapon != candidate) return false;
    bool blocked = false;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoClientMutex);
        l4d2vr_pistol_sync::State state{};
        blocked = m_PistolAmmoClient.ReadBaseline(reinterpret_cast<uintptr_t>(player), handle, dual, GetTickCount64(), state) &&
            l4d2vr_pistol_reload::BlocksNativeReload(true, state.physical,
                !dual && m_PhysicalReloadOwnsStockInput.load(std::memory_order_acquire) &&
                l4d2vr_pistol_reload::MatchesSingleHand(state,GetPrimaryPistolHand()));
    }
    if (!blocked) return false;
    // Clear only reload ownership. Shared firing cadence and native ammo stay
    // untouched, including a loaded pistol in the other hand.
    MagazineInteractionClearPistolReloadFlags(weapon);
    return true;
}
void VR::ReceiveRemoteMagazineReply(const l4d2vr_remote_mag::Reply& reply)
{
    std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
    m_RemoteMagazineRequest.Receive(reply);
}
void VR::DisconnectRemoteMagazineProtocol()
{
    std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
    m_RemoteMagazineRequest.Disconnect();
}
void VR::CancelRemoteMagazineRequest()
{
    std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
    m_RemoteMagazineRequest.Cancel();
}
bool VR::RemoteMagazineProtocolSupported() const
{
    if (!m_IsVREnabled || !m_EncodeVRUsercmd || m_ForceNonVRServerMovement ||
        !l4d2vr_wire::SupportsPhysicalVersion(m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)))
        return false;
    std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
    return m_RemoteMagazineRequest.Supported();
}
bool VR::RemoteMagazineRequestPending() const
{
    std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
    return m_RemoteMagazineRequest.Pending();
}
bool VR::BeginRemoteMagazineRequest(uint32_t handle, int id, int clip, int reserve,
    l4d2vr_remote_mag::Action action, uintptr_t owner, uint32_t generation)
{
    if (!RemoteMagazineProtocolSupported() || !m_Game || !m_Game->m_EngineClient ||
        m_Game->m_EngineClient->IsPaused() || m_MouseModeEnabled || m_SuppressPlayerInput ||
        !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) ||
        m_RenderPlayerControlledBySI.load(std::memory_order_acquire) ||
        (m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible())) return false;
    if (!HasFreshMagazineInteractionTracking()) return false;
    l4d2vr_remote_mag::Request request{};
    {
        std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
        if (m_CurrentPredictedHitFeedbackCmdNumber <= 0 || !m_RemoteMagazineRequest.Begin(handle, id, clip, reserve,
            action, static_cast<uint32_t>(m_CurrentPredictedHitFeedbackCmdNumber), owner, generation, GetTickCount64(), request))
            return false;
    }
    SendRemoteMagazineRequest(request,false);
    return true;
}
void VR::SendRemoteMagazineRequest(const l4d2vr_remote_mag::Request& request, bool retry)
{
    if (!m_Game) return;
    char command[192]{};
    std::snprintf(command, sizeof(command), "l4d2vr_mag_action_v2 %u %u %u %u %d %d %d %u\n",
        request.token, request.sequence, request.handle, request.command, request.weaponId,
        request.clip, request.reserve, static_cast<unsigned>(request.action));
    m_Game->ServerCmd(command,true);
    Game::logMsg("[VR][RemoteMagazine][client] %s seq=%u action=%u weaponId=%d clip=%d reserve=%d",
        retry ? "retry" : "requested",request.sequence,static_cast<unsigned>(request.action),
        request.weaponId,request.clip,request.reserve);
}
l4d2vr_remote_mag::ClientRequest::Poll VR::PollRemoteMagazineReply(uint32_t handle, int id,
    uintptr_t owner, uint32_t generation, int clip, int reserve, l4d2vr_remote_mag::Reply& reply)
{
    l4d2vr_remote_mag::Request retry{};
    bool resend = false;
    l4d2vr_remote_mag::ClientRequest::Poll result{};
    {
        std::lock_guard<std::mutex> lock(m_RemoteMagazineMutex);
        const uint64_t now = GetTickCount64();
        result = m_RemoteMagazineRequest.Update(handle,id,owner,generation,now,reply,clip,reserve);
        if (result == l4d2vr_remote_mag::ClientRequest::Poll::Waiting && m_Game && m_Game->m_EngineClient &&
            !m_Game->m_EngineClient->IsPaused() && !m_MouseModeEnabled && !m_SuppressPlayerInput &&
            m_FirstPersonControlReady.load(std::memory_order_acquire) &&
            !m_RenderPlayerIncap.load(std::memory_order_acquire) && !m_RenderPlayerControlledBySI.load(std::memory_order_acquire) &&
            !(m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible()))
            resend = m_RemoteMagazineRequest.NextRetry(now,retry);
    }
    if (resend) SendRemoteMagazineRequest(retry,true);
    return result;
}

void VR::RecordMeleeCommand(int command, bool eligible, uintptr_t owner, uintptr_t weapon)
{
    if (command <= 0) return;
    VRMeleeTrackingSnapshot tracking{};
    {
        std::lock_guard<std::mutex> lock(m_WorldPoseTrackingSnapshotMutex);
        tracking = m_MeleeTrackingSnapshot;
    }
    l4d2vr_physical::MeleeCommand sample{};
    sample.command = command;
    sample.position = { tracking.position.x, tracking.position.y, tracking.position.z };
    sample.angles = { tracking.angles.x, tracking.angles.y, tracking.angles.z };
    sample.valid = eligible && tracking.valid && sample.Finite() &&
        l4d2vr_physical::MeleeTrackingFresh(static_cast<uint32_t>(GetTickCount64()), tracking.sampledAtMs);
    std::lock_guard<std::mutex> lock(m_MeleeCommandMutex);
    if (command < m_LastMeleeRecordedCommand)
    {
        m_MeleeCommands.Reset(); m_MeleeMotion.Reset(); m_LastMeleeRecordedCommand = 0;
    }
    if (command == m_LastMeleeRecordedCommand) return;
    m_LastMeleeRecordedCommand = command;
    sample.swinging = m_MeleeMotion.Update(sample.valid, owner, weapon,
        tracking.relativeVelocity.Length(), tracking.angularVelocity.Length());
    m_MeleeCommands.Store(sample);
    if (m_VrHandsDebugLog && eligible)
    {
        static uint32_t lastLogAt = 0u;
        static bool lastValid = false, lastSwinging = false;
        const uint32_t logAt = static_cast<uint32_t>(GetTickCount64());
        if (sample.valid != lastValid || sample.swinging != lastSwinging || logAt - lastLogAt >= 1000u)
        {
            lastLogAt = logAt; lastValid = sample.valid; lastSwinging = sample.swinging;
            Game::logMsg("[VR][PhysicalMelee][client] command=%d valid=%d swinging=%d ageMs=%u linear=%.3f angularDeg=%.1f tracked=%d",
                command, sample.valid ? 1 : 0, sample.swinging ? 1 : 0,
                logAt - tracking.sampledAtMs, tracking.relativeVelocity.Length(),
                tracking.angularVelocity.Length(), tracking.valid ? 1 : 0);
        }
    }
}

bool VR::GetMeleeCommand(int command, l4d2vr_physical::MeleeCommand& sample) const
{
    std::lock_guard<std::mutex> lock(m_MeleeCommandMutex);
    return m_MeleeCommands.Get(command, sample);
}

void VR::CancelMeleeMotion()
{
    std::lock_guard<std::mutex> lock(m_MeleeCommandMutex);
    m_MeleeMotion.Reset();
    m_MeleeCommands.Reset();
    m_LastMeleeRecordedCommand = 0;
}

void VR::RecordManualPumpShot()
{
    if (!m_ManualPumpEnabled || !m_IsVREnabled || !m_Game || !m_Game->m_EngineClient)
        return;
    const int index = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* player = index > 0 ? reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(index)) : nullptr;
    C_WeaponCSBase* weapon = nullptr;
    C_WeaponCSBase::WeaponID id = C_WeaponCSBase::WeaponID::NONE;
    int clip = -1;
    if (!MagazineInteractionReadActiveWeapon(player, weapon, id, clip) || !MagazineInteractionWeaponUsesSinglePumpSound(id))
        return;
    if (!IsMagazineInteractionShotgunServerHookActive(static_cast<int>(id))) return;
    const uint32_t handle = MagazineInteractionActiveWeaponHandle(m_Game, player, weapon);
    std::lock_guard<std::mutex> lock(m_ManualPumpMutex);
    m_ManualPumpCycles.ObserveOwner(true, reinterpret_cast<uintptr_t>(player));
    m_ManualPumpCycles.NotifyShot(reinterpret_cast<uintptr_t>(weapon), static_cast<int>(id), clip, handle);
    Game::logMsg("[VR][ManualPump] shot observed weaponId=%d clip=%d", static_cast<int>(id), clip);
}

namespace
{
    bool TraceHmdFlashlightMount(IEngineTrace* engine, const Ray_t& ray, CTraceFilter* filter, CGameTrace& trace)
    {
        if (!engine) return false;
#ifdef _MSC_VER
        __try
#endif
        {
            engine->TraceRay(ray, MASK_STATICWORLD | CONTENTS_MOVEABLE, filter, &trace);
            return true;
        }
#ifdef _MSC_VER
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
    }
}

void VR::UpdateHmdFlashlightPose(C_BasePlayer* player)
{
    l4d2vr_flashlight::Pose pose{};
    VRWorldPoseTrackingSnapshot tracking{};
    const auto now = GetTickCount64();
    const bool eligible = player && m_IsVREnabled && m_FlashlightFollowHmd && !m_MouseModeEnabled &&
        !IsThirdPersonCameraActive() && !m_RenderTpObserver.load(std::memory_order_acquire) &&
        m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame() &&
        ReadWorldPoseTrackingSnapshot(tracking) && tracking.hmdValid && tracking.capturedAtMs != 0u &&
        now >= tracking.capturedAtMs && now - tracking.capturedAtMs <= 150u &&
        std::isfinite(tracking.hmdAngles.x) && std::isfinite(tracking.hmdAngles.y) && std::isfinite(tracking.hmdAngles.z);
    if (eligible)
    {
        Vector forward{},right{},up{};
        QAngle::AngleVectors(tracking.hmdAngles,&forward,&right,&up);
        const float offset = l4d2vr_flashlight::OffsetMeters(m_FlashlightHmdForwardOffsetMeters);
        float fraction = 0.0f;
        bool startSolid = false, allSolid = false;
        if (offset == 0.0f) fraction = 1.0f;
        else if (l4d2vr_interaction::Finite(tracking.hmdPosition) && std::isfinite(m_VRScale) && m_VRScale > 0.001f)
        {
            // CreateMove owns collision queries. Render callbacks consume only
            // this published sample, including its matching head basis.
            const float radius = 0.015f * m_VRScale;
            const Vector target = tracking.hmdPosition + forward * (offset * m_VRScale);
            if (std::isfinite(radius) && l4d2vr_interaction::Finite(target) && l4d2vr_interaction::Finite(forward))
            {
                Ray_t ray;
                ray.Init(tracking.hmdPosition,target,Vector(-radius,-radius,-radius),Vector(radius,radius,radius));
                CTraceFilterSkipThreeEntities filter(reinterpret_cast<IHandleEntity*>(player), nullptr,
                    reinterpret_cast<IHandleEntity*>(player->GetActiveWeapon()),0);
                CGameTrace trace{};
                if (TraceHmdFlashlightMount(m_Game->m_EngineTrace,ray,&filter,trace))
                {
                    fraction = trace.fraction; startSolid = trace.startsolid; allSolid = trace.allsolid;
                }
            }
        }
        l4d2vr_flashlight::Capture(reinterpret_cast<uintptr_t>(player),tracking.capturedAtMs,
            tracking.hmdPosition,forward,right,up,m_VRScale,offset,fraction,startSolid,allSolid,pose);
    }
    std::lock_guard<std::mutex> lock(m_HmdFlashlightMutex);
    m_HmdFlashlightPose = pose;
}

bool VR::ReadHmdFlashlightPose(uintptr_t owner, l4d2vr_flashlight::Pose& pose) const
{
    std::lock_guard<std::mutex> lock(m_HmdFlashlightMutex);
    return l4d2vr_flashlight::Resolve(m_HmdFlashlightPose,owner,GetTickCount64(),
        m_VRScale,m_FlashlightHmdForwardOffsetMeters,pose);
}

void VR::PublishBodyInventoryModelPose(uintptr_t owner, uintptr_t model, uint64_t generation,
    const Vector& referenceHead, const Vector& pelvis, float bodyYaw)
{
    l4d2vr_body_inventory::ModelPose pose{};
    if (!l4d2vr_body_inventory::Capture(owner, model, generation, referenceHead, pelvis,
        bodyYaw, m_RotationOffset, m_VRScale, GetTickCount64(), pose))
    {
        ClearBodyInventoryModelPose();
        return;
    }
    std::lock_guard<std::mutex> lock(m_BodyInventoryModelMutex);
    m_BodyInventoryModelPose = pose;
}

void VR::ClearBodyInventoryModelPose()
{
    std::lock_guard<std::mutex> lock(m_BodyInventoryModelMutex);
    m_BodyInventoryModelPose = {};
}

bool VR::ResolveBodyInventoryModelPose(const Vector& head, Vector& origin, Vector& forward, Vector& right) const
{
    if (!m_BodyGripInventoryUseModelPelvis || !m_FirstPersonBodyEnabled ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) || m_RenderTpObserver.load(std::memory_order_acquire))
        return false;
    l4d2vr_body_inventory::ModelPose pose{};
    {
        std::lock_guard<std::mutex> lock(m_BodyInventoryModelMutex);
        pose = m_BodyInventoryModelPose;
    }
    return l4d2vr_body_inventory::Resolve(pose, m_BodyInventoryOwnerTag.load(std::memory_order_acquire),
        GetTickCount64(), head, m_InventoryBodyOriginOffset, m_VRScale, m_RotationOffset, origin, forward, right);
}

void VR::UpdateBodyInventoryPose()
{
    const int index = m_Game && m_Game->m_EngineClient ? m_Game->m_EngineClient->GetLocalPlayer() : -1;
    m_BodyInventoryOwnerTag.store(index > 0 ? reinterpret_cast<uintptr_t>(m_Game->GetClientEntity(index)) : 0u,
        std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_BodyInventoryPoseMutex);
    if (!m_IsVREnabled || !m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame() ||
        !l4d2vr_interaction::Finite(m_HmdPosAbs) || !l4d2vr_interaction::Finite(m_HmdForward) ||
        !std::isfinite(m_RotationOffset) || !std::isfinite(m_VRScale) || m_VRScale <= 0.001f)
    {
        m_BodyInventoryPoseValid = false;
        return;
    }
    // Tracking already rotated the head's forward vector into game space.
    const float turnDelta = MagazineInteractionWrapDegrees(m_RotationOffset - m_BodyInventoryRotationOffset);
    m_BodyInventoryYaw = l4d2vr_interaction::BodyYaw(m_HmdForward,
        m_BodyInventoryPoseValid ? m_BodyInventoryYaw : m_RotationOffset,
        m_BodyInventoryPoseValid ? turnDelta : 0.0f, m_BodyInventoryPoseValid);
    m_BodyInventoryRotationOffset = m_RotationOffset;
    const float radians = m_BodyInventoryYaw * 3.14159265358979323846f / 180.0f;
    m_BodyInventoryForward = Vector(std::cos(radians), std::sin(radians), 0.0f);
    m_BodyInventoryRight = Vector(m_BodyInventoryForward.y, -m_BodyInventoryForward.x, 0.0f);
    m_BodyInventoryHeadPosAbs = m_HmdPosAbs;
    m_BodyInventoryOrigin = l4d2vr_interaction::BodyOrigin(m_BodyInventoryHeadPosAbs,
        m_BodyInventoryForward, m_BodyInventoryRight, m_InventoryBodyOriginOffset, m_VRScale);
    // Keep the cached head frame as the fallback. A model sample may expire or
    // be invalidated on the render worker between input updates.
    Vector fittedOrigin = m_BodyInventoryOrigin;
    Vector fittedForward = m_BodyInventoryForward, fittedRight = m_BodyInventoryRight;
    const bool modelFitted = ResolveBodyInventoryModelPose(m_BodyInventoryHeadPosAbs,
        fittedOrigin, fittedForward, fittedRight);
    m_BodyInventoryPoseValid = true;
    if (m_BodyGripInventoryEnabled && m_VrHandsDebugLog)
    {
        static std::chrono::steady_clock::time_point lastLog{};
        const auto now = std::chrono::steady_clock::now();
        if (lastLog.time_since_epoch().count() == 0 || now-lastLog >= std::chrono::seconds(1))
        {
            lastLog = now;
            Game::logMsg("[VR][BodyInventory] head=(%.2f %.2f %.2f) body=(%.2f %.2f %.2f) yaw=%.1f turn=%.1f modelFitted=%d",
                m_BodyInventoryHeadPosAbs.x, m_BodyInventoryHeadPosAbs.y, m_BodyInventoryHeadPosAbs.z,
                fittedOrigin.x, fittedOrigin.y, fittedOrigin.z,
                m_BodyInventoryYaw, m_RotationOffset, modelFitted ? 1 : 0);
        }
    }
}

bool VR::GetBodyInventoryPose(Vector& origin, Vector& forward, Vector& right) const
{
    std::lock_guard<std::mutex> lock(m_BodyInventoryPoseMutex);
    if (!m_BodyInventoryPoseValid)
        return false;
    origin = m_BodyInventoryOrigin;
    Vector head = m_BodyInventoryHeadPosAbs;
    Vector renderHead{};
    if (VR::t_UseRenderFrameSnapshot && MagazineInteractionTryGetRenderSnapshotHmdPosAbs(this, renderHead))
    {
        origin += renderHead - m_BodyInventoryHeadPosAbs;
        head = renderHead;
    }
    forward = m_BodyInventoryForward;
    right = m_BodyInventoryRight;
    ResolveBodyInventoryModelPose(head, origin, forward, right);
    return true;
}

bool VR::GetBodyAmmoPreviewWorld(const MagazineInteractionBoxSnapshot& box, VrHandMatrix4& outWorld) const
{
    if (!m_BodyGripInventoryEnabled || !m_BodyAmmoAvailable.load(std::memory_order_acquire))
        return false;
    Vector origin{}, forward{}, right{};
    if (!GetBodyInventoryPose(origin, forward, right))
        return false;
    const Vector offset = MagazineInteractionFreshMagazinePickupOffsetMeters(this);
    Vector pickup = origin + forward * (offset.x * m_VRScale) +
        right * (offset.y * m_VRScale) + Vector(0.0f, 0.0f, offset.z * m_VRScale);
    Vector reprojected{};
    if (MagazineInteractionReprojectScenePointToViewmodelLayer(this, pickup, reprojected))
        pickup = reprojected;
    return l4d2vr_ammo_grip::BodyWorld(forward, right, pickup,
        MagazineInteractionBoxCenterLocal(box), outWorld) && MagazineInteractionMatrixLooksRenderable(outWorld);
}

bool VR::HasFreshMagazineInteractionDebugBoxWork() const
{
    if (!m_MagazineBoxDebugEnabled &&
        !m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed))
    {
        return false;
    }

    if (m_MagazineInteractionSocketValid &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine))
    {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const float staleSeconds = std::max(0.02f, m_MagazineInteractionStaleSeconds);
    auto fresh = [&](const MagazineInteractionBoxSnapshot& snapshot) -> bool
    {
        return snapshot.publishedAt.time_since_epoch().count() != 0 &&
            std::chrono::duration<float>(now - snapshot.publishedAt).count() <= staleSeconds;
    };

    MagazineInteractionBoxSnapshot box{};
    if (GetMagazineInteractionBox(box) && fresh(box))
        return true;

    MagazineInteractionBoxSnapshot boltBox{};
    return GetMagazineInteractionBoltBox(boltBox) && fresh(boltBox);
}

bool VR::GetMagazineInteractionCalibrationSnapshot(MagazineInteractionCalibrationSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionCalibrationMutex);
    if (!m_MagazineInteractionCalibrationSnapshot.valid)
        return false;
    outSnapshot = m_MagazineInteractionCalibrationSnapshot;
    return true;
}

void VR::PublishPlayerModelMaterialsSnapshot(
    uint32_t session,
    int entityIndex,
    const std::string& characterName,
    const std::string& modelName,
    const std::vector<std::string>& materialNames,
    bool localPlayer)
{
    if (characterName.empty() || modelName.empty() || materialNames.empty())
        return;

    std::lock_guard<std::mutex> lock(m_PlayerModelMaterialsSnapshotMutex);
    auto existing = m_LoadedPlayerModelMaterialsSnapshots.find(characterName);
    const bool cacheMatches =
        existing != m_LoadedPlayerModelMaterialsSnapshots.end() &&
        existing->second.session == session &&
        existing->second.entityIndex == entityIndex &&
        existing->second.modelName == modelName &&
        existing->second.materialNames == materialNames;
    if (!cacheMatches)
    {
        PlayerModelMaterialsSnapshot snapshot{};
        snapshot.valid = true;
        snapshot.session = session;
        snapshot.publishSeq = ++m_PlayerModelMaterialsSnapshotPublishSeq;
        snapshot.entityIndex = entityIndex;
        snapshot.characterName = characterName;
        snapshot.modelName = modelName;
        snapshot.materialNames = materialNames;
        existing = m_LoadedPlayerModelMaterialsSnapshots.insert_or_assign(
            characterName,
            std::move(snapshot)).first;
    }

    if (localPlayer && existing != m_LoadedPlayerModelMaterialsSnapshots.end())
        m_PlayerModelMaterialsSnapshot = existing->second;
}

void VR::ClearPlayerModelMaterialsSnapshot()
{
    std::lock_guard<std::mutex> lock(m_PlayerModelMaterialsSnapshotMutex);
    if (!m_PlayerModelMaterialsSnapshot.valid &&
        m_LoadedPlayerModelMaterialsSnapshots.empty())
        return;

    PlayerModelMaterialsSnapshot snapshot{};
    snapshot.publishSeq = ++m_PlayerModelMaterialsSnapshotPublishSeq;
    m_PlayerModelMaterialsSnapshot = std::move(snapshot);
    m_LoadedPlayerModelMaterialsSnapshots.clear();
}

bool VR::GetPlayerModelMaterialsSnapshot(
    PlayerModelMaterialsSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_PlayerModelMaterialsSnapshotMutex);
    outSnapshot = m_PlayerModelMaterialsSnapshot;
    return outSnapshot.valid;
}

void VR::GetLoadedPlayerModelMaterialsSnapshots(
    std::vector<PlayerModelMaterialsSnapshot>& outSnapshots) const
{
    std::lock_guard<std::mutex> lock(m_PlayerModelMaterialsSnapshotMutex);
    outSnapshots.clear();
    outSnapshots.reserve(m_LoadedPlayerModelMaterialsSnapshots.size());
    for (const auto& entry : m_LoadedPlayerModelMaterialsSnapshots)
        outSnapshots.push_back(entry.second);
}

bool VR::IsMagazineInteractionLeftHandActive() const
{
    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerShell ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerPistolCycle) return true;
    if (m_MagazineInteractionShotgunShellMode &&
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine &&
        m_MagazineInteractionShotgunShellsLoadedThisSession > 0)
    {
        return true;
    }

    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
        m_MagazineInteractionSuppressLeftInputUntilRelease ||
        m_MagazineInteractionLeftHandHolding;
}

bool VR::IsMagazineInteractionManualActive() const
{
    return m_MagazineInteractionState != MagazineInteractionManualState::Idle;
}

bool VR::MagazineInteractionOwnsGrip(bool gameplayLeft) const
{
    return gameplayLeft == MagazineReloadSupportIsLeft() && IsMagazineInteractionLeftHandActive();
}

bool VR::IsMagazineInteractionNativeReloadSuppressActive() const
{
    return m_MagazineInteractionNativeReloadSuppressUntil.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() <= m_MagazineInteractionNativeReloadSuppressUntil;
}

bool VR::IsMagazineInteractionViewmodelOverrideActive() const
{
    if (IsMagazineInteractionManualActive())
        return true;

    return IsMagazineInteractionNativeReloadSuppressActive();
}

void VR::MarkMagazineInteractionReloadCommandIssued()
{
    if (m_MagazineInteractionReloadCommandPending && !m_MagazineInteractionReloadCommandIssued)
    {
        Game::logMsg(
            "[VR][MagazineInteraction] backend reload command issued after physical old magazine pull");
    }
    m_MagazineInteractionReloadCommandPending = false;
    m_MagazineInteractionReloadCommandIssued = true;
}

bool VR::IsMagazineInteractionReloadCommandActive() const
{
    if (m_MagazineInteractionShotgunShellMode &&
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload &&
        m_MagazineInteractionReloadTriggered &&
        m_MagazineInteractionReloadCommandIssued)
    {
        return true;
    }

    return m_MagazineInteractionReloadCommandPending ||
        (m_MagazineInteractionReloadTriggered && m_MagazineInteractionReloadCommandIssued &&
            m_MagazineInteractionReloadCommandHoldUntil.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() < m_MagazineInteractionReloadCommandHoldUntil);
}

bool VR::ShouldSuppressMagazineInteractionEmptyClipAutoReload(C_BasePlayer* localPlayer) const
{
    if ((m_DualPistolNativeReloadState.load(std::memory_order_acquire) & 1u) != 0u)
        return false;
    // m_MagazineInteractionOneInChamber is only maintained by the detachable-magazine
    // settlement path. Shotguns never set it, so it cannot gate generic empty-clip suppression.
    if (!m_MagazineInteractionEnabled ||
        !m_IsVREnabled ||
        (!m_VrHandsEnabled && !m_NativeViewmodelHandsOnly))
    {
        return false;
    }
    if (IsMagazineInteractionReloadCommandActive())
        return false;

    C_BasePlayer* player = localPlayer;
    if (!player && m_Game && m_Game->m_EngineClient)
    {
        const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        if (localPlayerIndex > 0)
            player = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localPlayerIndex));
    }

    C_WeaponCSBase* activeWeapon = nullptr;
    C_WeaponCSBase::WeaponID activeWeaponId = C_WeaponCSBase::WeaponID::NONE;
    int activeClip = -1;
    if (!MagazineInteractionReadActiveWeapon(player, activeWeapon, activeWeaponId, activeClip) ||
        !activeWeapon ||
        !MagazineInteractionWeaponUsesPhysicalReload(activeWeaponId) ||
        activeClip != 0)
    {
        return false;
    }

    const bool usesShotgunShells =
        MagazineInteractionWeaponUsesShotgunShells(activeWeaponId);
    if (usesShotgunShells && !IsMagazineInteractionShotgunServerHookActive(static_cast<int>(activeWeaponId)))
        return false; // conventional empty reload on a server without a physical shell backend
    if (!usesShotgunShells && !IsMagazineInteractionDetachableBackendActive(static_cast<int>(activeWeaponId)))
        return false;
    const bool forceShotgunMagazineInteraction =
        usesShotgunShells && m_MagazineInteractionShotgunShellMode;
    if (!m_MagazineInteractionSuppressEmptyClipAutoReload &&
        !forceShotgunMagazineInteraction)
    {
        return false;
    }

    static std::chrono::steady_clock::time_point s_lastLog{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastLog.time_since_epoch().count() == 0 ||
        std::chrono::duration<float>(now - s_lastLog).count() >= 1.0f)
    {
        s_lastLog = now;
        Game::logMsg(
            "[VR][MagazineInteraction] suppressing empty-clip automatic reload weaponId=%d clip=%d; physical %s reload required",
            static_cast<int>(activeWeaponId),
            activeClip,
            usesShotgunShells ? "shell" : "magazine");
    }
    return true;
}

bool VR::IsMagazineInteractionBlockingFire() const
{
    const uint32_t nativeDualReload = m_DualPistolNativeReloadState.load(std::memory_order_acquire);
    if ((nativeDualReload & 1u) != 0u) return (nativeDualReload & 2u) != 0u;
    if (m_ManualPumpBlockingFire.load(std::memory_order_acquire))
        return true;
    if (RemoteShellRequestPending()) return true;
    if (RemoteMagazineRequestPending()) return true;
    if (m_IsVREnabled && m_MagazineInteractionEnabled && m_Game && m_Game->m_EngineClient &&
        m_Game->m_EngineClient->IsInGame())
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        auto* player = playerIndex > 0 ? static_cast<C_BasePlayer*>(m_Game->GetClientEntity(playerIndex)) : nullptr;
        C_WeaponCSBase* weapon = nullptr;
        C_WeaponCSBase::WeaponID weaponId = C_WeaponCSBase::WeaponID::NONE;
        int clip = -1;
        if (MagazineInteractionReadActiveWeapon(player, weapon, weaponId, clip) && clip > 0)
        {
            const int id = static_cast<int>(weaponId);
            const bool supported = MagazineInteractionWeaponUsesShotgunShells(weaponId)
                ? IsMagazineInteractionShotgunServerHookActive(id) : IsMagazineInteractionDetachableBackendActive(id);
            const auto handle = MagazineInteractionActiveWeaponHandle(m_Game, player, weapon);
            if (supported && l4d2vr_shell::ValidHandle(handle) &&
                m_PhysicalReloadChambers.HasEmptyChamber(reinterpret_cast<uintptr_t>(player),
                    reinterpret_cast<uintptr_t>(weapon), handle, id))
                return true;
        }
    }
    if (m_MagazineInteractionShotgunShellMode)
    {
        const bool cycling =
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
            m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting;
        return l4d2vr_magazine::ShotgunBlocksFire(m_MagazineInteractionChamberEmpty,
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine,
            cycling, m_MagazineInteractionReloadTriggered ||
                m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
                m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerShell,
            RemoteShellRequestPending());
    }

    return IsMagazineInteractionManualActive() && !m_MagazineInteractionOneInChamber;
}

void VR::PlayMagazineInteractionBlockedFireEmptySound()
{
    if (!m_Game)
        return;

    const bool magazineInteractionBlocksFire = IsMagazineInteractionBlockingFire();
    int weaponId = magazineInteractionBlocksFire ? m_MagazineInteractionWeaponId : 0;

    if (weaponId == 0 || !magazineInteractionBlocksFire)
    {
        C_BasePlayer* player = nullptr;
        if (m_Game->m_EngineClient)
        {
            const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
            if (localPlayerIndex > 0)
                player = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localPlayerIndex));
        }

        C_WeaponCSBase* activeWeapon = nullptr;
        C_WeaponCSBase::WeaponID activeWeaponId = C_WeaponCSBase::WeaponID::NONE;
        int activeClip = -1;
        if (!MagazineInteractionReadActiveWeapon(player, activeWeapon, activeWeaponId, activeClip) ||
            activeClip != 0)
        {
            if (!magazineInteractionBlocksFire)
                return;
        }
        else
        {
            weaponId = static_cast<int>(activeWeaponId);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_MagazineInteractionEmptyFireSoundLastPlayed.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(now - m_MagazineInteractionEmptyFireSoundLastPlayed).count() < 0.12f)
    {
        return;
    }

    const char* soundName = MagazineInteractionBlockedFireEmptySound(weaponId);
    const char* soundPath = (std::strcmp(soundName, "ClipEmpty_Pistol") == 0)
        ? "weapons/clipempty_pistol.wav"
        : "weapons/clipempty_rifle.wav";
    const std::string soundSpec = std::string("game:") + soundPath;
    const bool feedbackPlayed = TryPlayKillSoundSpec(soundSpec, 1.0f, nullptr, false);
    if (!feedbackPlayed)
    {
        const std::string command = std::string("play ") + soundPath;
        m_Game->ClientCmd_Unrestricted(command.c_str());
    }
    m_MagazineInteractionEmptyFireSoundLastPlayed = now;
    Game::logMsg(
        "[VR][MagazineInteraction][Audio] played blocked-fire empty sound name=%s spec=%s feedback=%d weaponId=%d state=%d blocking=%d",
        soundName,
        soundSpec.c_str(),
        feedbackPlayed ? 1 : 0,
        weaponId,
        static_cast<int>(m_MagazineInteractionState),
        magazineInteractionBlocksFire ? 1 : 0);
}

bool VR::ShouldFreezeMagazineInteractionViewmodel() const
{
    if ((m_DualPistolNativeReloadState.load(std::memory_order_acquire) & 1u) != 0u)
        return false;
    if (m_MagazineInteractionViewmodelFreezeDeferredUntil.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() < m_MagazineInteractionViewmodelFreezeDeferredUntil)
    {
        return false;
    }

    if (IsMagazineInteractionNativeReloadSuppressActive())
        return true;

    if (m_MagazineInteractionShotgunShellMode)
    {
        return m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
            m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
            ((m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
                m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine) &&
                (m_MagazineInteractionShotgunShellsLoadedThisSession > 0 ||
                    (m_MagazineInteractionSuppressEmptyClipAutoReload &&
                        m_MagazineInteractionChamberEmpty)));
    }

    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerPistolCycle ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
        m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting;
}

void VR::MarkMagazineInteractionServerHookSeen(int serverWeaponId)
{
    const auto now = std::chrono::steady_clock::now();
    m_MagazineInteractionAnyServerHookWeaponId = serverWeaponId;
    m_MagazineInteractionAnyServerHookLastSeen = now;

    if (!MagazineInteractionWeaponUsesPhysicalReload(static_cast<C_WeaponCSBase::WeaponID>(serverWeaponId)))
        return;

    m_MagazineInteractionServerHookWeaponId = serverWeaponId;
    m_MagazineInteractionServerHookLastSeen = now;
}

bool VR::IsMagazineInteractionAnyServerHookActive() const
{
    if (m_MagazineInteractionAnyServerHookLastSeen.time_since_epoch().count() == 0)
        return false;

    constexpr float kServerHookFreshSeconds = 1.50f;
    return std::chrono::duration<float>(
        std::chrono::steady_clock::now() -
        m_MagazineInteractionAnyServerHookLastSeen).count() <= kServerHookFreshSeconds;
}

bool VR::IsMagazineInteractionServerHookActive(int weaponId) const
{
    if (weaponId <= 0 ||
        m_MagazineInteractionServerHookWeaponId != weaponId ||
        m_MagazineInteractionServerHookLastSeen.time_since_epoch().count() == 0)
    {
        return false;
    }

    constexpr float kServerHookFreshSeconds = 1.50f;
    return std::chrono::duration<float>(
        std::chrono::steady_clock::now() -
        m_MagazineInteractionServerHookLastSeen).count() <= kServerHookFreshSeconds;
}

void VR::MarkMagazineInteractionShotgunServerHookSeen(int serverWeaponId)
{
    if (!MagazineInteractionWeaponUsesShotgunShells(static_cast<C_WeaponCSBase::WeaponID>(serverWeaponId)))
        return;

    MarkMagazineInteractionServerHookSeen(serverWeaponId);
    m_MagazineInteractionShotgunServerHookWeaponId = serverWeaponId;
    m_MagazineInteractionShotgunServerHookLastSeen = std::chrono::steady_clock::now();
}

bool VR::IsMagazineInteractionShotgunServerHookActive(int weaponId) const
{
    return l4d2vr_shell::IsShotgun(weaponId) && RemoteShellProtocolSupported();
}

bool VR::IsMagazineInteractionDetachableBackendActive(int weaponId) const
{
    if (weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL) && PistolMagazineProtocolSupported())
        return true;
    return l4d2vr_remote_mag::IsDetachable(weaponId) &&
        RemoteMagazineProtocolSupported();
}

bool VR::ApplyMagazineInteractionShotgunClientReloadAbort(
    C_WeaponCSBase* clientWeapon,
    int clientWeaponId,
    const char*)
{
    if (!clientWeapon ||
        !MagazineInteractionWeaponUsesShotgunShells(static_cast<C_WeaponCSBase::WeaponID>(clientWeaponId)))
        return false;

    constexpr int kClientNextPrimaryAttackOffset = 0x960;       // DT_BaseCombatWeapon::m_flNextPrimaryAttack
    constexpr int kClientNextSecondaryAttackOffset = 0x964;     // DT_BaseCombatWeapon::m_flNextSecondaryAttack
    constexpr int kClientTimeWeaponIdleOffset = 0x990;          // DT_BaseCombatWeapon::m_flTimeWeaponIdle
    constexpr int kClientBaseWeaponInReloadOffset = 0x9BD;      // DT_BaseCombatWeapon::m_bInReload
    constexpr int kClientPartialReloadStageOffset = 0xCE8;      // DT_WeaponCSBase::m_partialReloadStage
    constexpr int kClientReloadFromEmptyOffset = 0xCEF;         // DT_WeaponCSBase::m_reloadFromEmpty
    constexpr int kClientShotgunReloadStateOffset = 0xD40;      // DT_BaseShotgun::m_reloadState
    constexpr int kClientShotgunReloadNumShellsOffset = 0xD44;  // DT_BaseShotgun::m_reloadNumShells
    constexpr int kClientShotgunReloadStartTimeOffset = 0xD48;  // DT_BaseShotgun::m_reloadStartTime
    constexpr int kClientShotgunReloadStartDurationOffset = 0xD4C;
    constexpr int kClientShotgunReloadInsertDurationOffset = 0xD50;
    constexpr int kClientShotgunReloadEndDurationOffset = 0xD54;
    constexpr int kClientShotgunReloadAnimStateOffset = 0xD58;  // DT_BaseShotgun::m_reloadAnimState
    constexpr int kClientShotgunShellsInsertedOffset = 0xD5C;   // DT_BaseShotgun::m_shellsInserted

    const bool wroteReloadState =
        MagazineInteractionTryWriteValue<int>(clientWeapon, kClientShotgunReloadStateOffset, 0);
    const bool wroteReloadNumShells =
        MagazineInteractionTryWriteValue<int>(clientWeapon, kClientShotgunReloadNumShellsOffset, 0);
    const bool wroteReloadAnimState =
        MagazineInteractionTryWriteValue<int>(clientWeapon, kClientShotgunReloadAnimStateOffset, 0);
    const bool wroteShellsInserted =
        MagazineInteractionTryWriteValue<int>(clientWeapon, kClientShotgunShellsInsertedOffset, 0);
    const bool wrotePartialReloadStage =
        MagazineInteractionTryWriteValue<unsigned char>(clientWeapon, kClientPartialReloadStageOffset, 0);
    const bool wroteReloadFromEmpty =
        MagazineInteractionTryWriteValue<unsigned char>(clientWeapon, kClientReloadFromEmptyOffset, 0);
    const bool wroteInReload =
        MagazineInteractionTryWriteValue<unsigned char>(clientWeapon, kClientBaseWeaponInReloadOffset, 0);
    const bool wroteReloadStartTime =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientShotgunReloadStartTimeOffset, 0.0f);
    const bool wroteReloadStartDuration =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientShotgunReloadStartDurationOffset, 0.0f);
    const bool wroteReloadInsertDuration =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientShotgunReloadInsertDurationOffset, 0.0f);
    const bool wroteReloadEndDuration =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientShotgunReloadEndDurationOffset, 0.0f);
    const bool wroteNextPrimaryAttack =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientNextPrimaryAttackOffset, 0.0f);
    const bool wroteNextSecondaryAttack =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientNextSecondaryAttackOffset, 0.0f);
    const bool wroteTimeWeaponIdle =
        MagazineInteractionTryWriteValue<float>(clientWeapon, kClientTimeWeaponIdleOffset, 0.0f);

    const bool wroteAny =
        wroteReloadState || wroteReloadNumShells || wroteReloadAnimState || wroteShellsInserted ||
        wrotePartialReloadStage || wroteReloadFromEmpty || wroteInReload ||
        wroteReloadStartTime || wroteReloadStartDuration || wroteReloadInsertDuration ||
        wroteReloadEndDuration || wroteNextPrimaryAttack || wroteNextSecondaryAttack ||
        wroteTimeWeaponIdle;
    return wroteAny;
}

bool VR::ShouldHideMagazineInteractionNativeClip() const
{
    if (m_MagazineInteractionShotgunShellMode)
        return false;

    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
}

bool VR::ShouldDrawMagazineInteractionDetachedMagazine() const
{
    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine ||
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerShell && m_MagazineInteractionLeftHandHolding);
}

bool VR::GetMagazineInteractionDetachedMagazineWorld(VrHandMatrix4& outWorld) const
{
    if (!ShouldDrawMagazineInteractionDetachedMagazine())
        return false;

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine)
    {
        if (MagazineInteractionTryBuildWaitingFreshMagazineWorldFromRenderSnapshot(this, outWorld))
            return true;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
    {
        if (MagazineInteractionTryBuildHeldMagazineWorldFromRenderSnapshot(
            this,
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine,
            outWorld))
        {
            return true;
        }
    }

    std::lock_guard<std::mutex> lock(m_MagazineInteractionPoseMutex);
    outWorld = m_MagazineInteractionDetachedMagazineWorld;
    return MagazineInteractionMatrixLooksRenderable(outWorld);
}

bool VR::ShouldMoveMagazineInteractionBolt() const
{
    return (m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
        m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting ||
        (m_ManualPumpEnabled &&
            MagazineInteractionWeaponUsesSinglePumpSound(static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionWeaponId)) &&
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab)) &&
        m_MagazineInteractionBoltRestValid &&
        MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltWorld);
}

bool VR::GetMagazineInteractionBoltWorld(VrHandMatrix4& outWorld) const
{
    if (!ShouldMoveMagazineInteractionBolt())
        return false;
    outWorld = m_MagazineInteractionBoltWorld;
    return MagazineInteractionMatrixLooksRenderable(outWorld);
}

void VR::ResetMagazineInteractionSession()
{
    m_NativeAmmoReloadFallbackPulse.Reset();
    CancelRemoteShellRequest();
    CancelRemoteMagazineRequest();
    // Fence old updates before waiting for a server-side settlement in progress.
    m_MagazineInteractionSessionGeneration.fetch_add(1, std::memory_order_acq_rel);
    CancelMagazineInteractionManual();
    {
        std::lock_guard<std::mutex> lock(m_ManualPumpMutex);
        const uintptr_t owner = m_MagazineInteractionSession.OwnerTag();
        m_ManualPumpCycles.ObserveOwner(m_ManualPumpEnabled && owner != 0u, owner);
    }
    m_ManualPumpClosedBoltValid = false;
    m_ManualPumpClosedBoltWeapon = 0;
    m_BodyAmmoAvailable.store(false, std::memory_order_release);
    if (m_ReloadCmdOwned)
    {
        if (m_Game && m_Game->m_EngineClient)
            m_Game->ClientCmd_Unrestricted("-reload");
        m_ReloadCmdOwned = false;
    }
    m_MagazineInteractionSuppressLeftInputUntilRelease = false;
    m_MagazineInteractionNativeReloadSuppressUntil = {};
    m_MagazineInteractionNativeReloadSuppressWeaponId = 0;
    m_VrHandsTwoHandedGripActive = false;
    m_VrHandsTwoHandedGripWeaponId = 0;
    m_VrHandsTwoHandedGripWeaponTag = 0;
    m_VrHandsTwoHandedMountFriendlyGripEnteredAt = {};
    m_VrHandsTwoHandedMountFriendlyGripContact = false;
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionCommitMutex);
    }
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionBoxMutex);
        m_MagazineInteractionBoxValid = false;
        m_MagazineInteractionBox = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
        m_MagazineInteractionBoltBoxValid = false;
        m_MagazineInteractionBoltBox = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_VrHandsTwoHandedGripWeaponBoxMutex);
        m_VrHandsTwoHandedGripWeaponBoxValid = false;
        m_VrHandsTwoHandedGripWeaponBox = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionHandAnchorMutex);
        m_MagazineInteractionNativeLeftWristValid = false;
        m_MagazineInteractionNativeLeftWristPublishedAt = {};
    }
}

void VR::CancelMagazineInteractionManual()
{
    m_MagazineButtonGripSources.Reset();
    m_MagazineInteractionTransactionGripHeld = false;
    m_ShellRetryContact.Reset();
    m_ManualPumpBlockingFire.store(false, std::memory_order_release);
    const bool wasActive = IsMagazineInteractionManualActive() || m_MagazineInteractionLeftHandHolding;
    m_MagazineInteractionState = MagazineInteractionManualState::Idle;
    m_MagazineInteractionLeftHandHolding = false;
    m_MagazineInteractionReloadTriggered = false;
    m_MagazineInteractionReloadCommandPending = false;
    m_MagazineInteractionReloadCommandIssued = false;
    m_MagazineInteractionReloadCommandHoldUntil = {};
    m_MagazineInteractionOldMagazinePulled = false;
    m_MagazineInteractionRetainedMagazineHeld = false;
    m_PistolMagazineCycleSuppressInput = false;
    m_MagazineInteractionPistolHandle = 0u;
    m_RetainedMagazineInsertGate.Reset();
    m_MagazineInteractionChamberEmpty = false;
    m_MagazineInteractionOneInChamber = false;
    m_MagazineInteractionOldMagazineContactActive = false;
    m_MagazineInteractionFreshMagazineContactActive = false;
    m_MagazineInteractionBoltContactActive = false;
    m_MagazineInteractionShotgunShellMode = false;
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionCommitMutex);
    }
    m_MagazineInteractionShotgunShellsLoadedThisSession = 0;
    m_MagazineInteractionShotgunLastInterruptedClip = -1;
    m_MagazineInteractionWeapon = nullptr;
    m_MagazineInteractionWeaponId = 0;
    m_MagazineInteractionWeaponEntityIndex = -1;
    m_MagazineInteractionStartClip = -1;
    m_MagazineInteractionMagazineBoneIndex = -1;
    m_MagazineInteractionViewmodelEntityIndex = -1;
    m_MagazineInteractionMagazineModelName.clear();
    m_MagazineInteractionCurrentModelFingerprint.store(0, std::memory_order_relaxed);
    m_MagazineInteractionCurrentBoneSignature.store(0, std::memory_order_relaxed);
    m_MagazineInteractionSocketValid = false;
    m_MagazineInteractionBoltRestValid = false;
    m_MagazineInteractionSocketWorld = {};
    m_MagazineInteractionSocketCaptureWorld = {};
    m_MagazineInteractionShotgunStableCaptureModelLocal = {};
    m_MagazineInteractionBoltRestWorld = {};
    m_MagazineInteractionBoltWorld = {};
    m_MagazineInteractionControllerToMagazine = {};
    m_MagazineInteractionFreshPickupBasisValid = false;
    m_MagazineInteractionFreshPickupForward = {};
    m_MagazineInteractionFreshPickupRight = {};
    m_MagazineInteractionFreshPickupHmdYawOffsetDeg = 0.0f;
    m_MagazineInteractionFreshPickupRotationOffset = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionPoseMutex);
        m_MagazineInteractionDetachedMagazineWorld = {};
    }
    m_MagazineInteractionBoltRestBox = {};
    m_MagazineInteractionSocketCaptureBox = {};
    m_MagazineInteractionShotgunStableCaptureBox = {};
    m_MagazineInteractionBoltPullAxisWorld = {};
    m_MagazineInteractionBoltInputAxisWorld = {};
    m_MagazineInteractionGrabStartLeftControllerPosAbs = {};
    m_MagazineInteractionHeldMagazineCenterOffsetLocal = {};
    m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = {};
    m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
    m_MagazineInteractionBoltPullDistance = 0.0f;
    m_MagazineInteractionBoltMaxPullDistance = 0.0f;
    m_MagazineInteractionBoltGrabRequiresGripRelease = false;
    m_MagazineInteractionBoltReachedRear = false;
    m_MagazineInteractionBoltPullAxisSignLocked = false;
    m_MagazineInteractionBoltStageBeforeBackendReloadComplete = false;
    m_MagazineInteractionBoltCompletedBeforeBackendReload = false;
    m_MagazineInteractionShotgunStableCaptureValid = false;
    m_MagazineInteractionStarted = {};
    m_MagazineInteractionFreshGrabbedAt = {};
    m_MagazineInteractionPostInsertStarted = {};
    m_MagazineInteractionBoltStageStarted = {};
    m_MagazineInteractionBoltGrabbedAt = {};
    m_MagazineInteractionViewmodelFreezeDeferredUntil = {};
    m_MagazineInteractionSyntheticClipOutSample.clear();
    m_MagazineInteractionSyntheticClipOutStarted = {};
    m_MagazineInteractionSyntheticClipInSample.clear();
    m_MagazineInteractionSyntheticClipInStarted = {};
    m_MagazineInteractionSyntheticBoltBackSample.clear();
    m_MagazineInteractionSyntheticBoltBackStarted = {};
    m_MagazineInteractionSyntheticBoltForwardSample.clear();
    m_MagazineInteractionSyntheticBoltForwardStarted = {};
    m_MagazineInteractionCapturedBoltBackSample.clear();
    m_MagazineInteractionCapturedBoltForwardSample.clear();
    m_MagazineInteractionCapturedBoltBackSoundScore = -1;
    m_MagazineInteractionCapturedBoltForwardSoundScore = -1;
    m_MagazineInteractionEmptyFireSoundLastPlayed = {};
    m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
    if (wasActive)
        Game::logMsg("[VR][MagazineInteraction] reset manual magazine interaction state");
}

bool VR::ReadMagazineInteractionFingerCurls(std::array<float, 5>& outCurls)
{
    outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (!m_Input)
        return false;

    const bool physicalLeftHand = IsGameplayHandLeftPhysical(MagazineReloadSupportIsLeft());
    vr::VRActionHandle_t& action = physicalLeftHand
        ? m_NativeViewmodelLeftHandOpenVRAction
        : m_NativeViewmodelRightHandOpenVRAction;
    const char* actionPath = physicalLeftHand
        ? "/actions/base/in/skeleton_lefthand"
        : "/actions/base/in/skeleton_righthand";

    return ReadOpenVRSkeletalFingerCurls(
        m_Input,
        m_System,
        m_InputActionFrame,
        GetPhysicalControllerIndexForHand(physicalLeftHand),
        physicalLeftHand ? "/user/hand/left" : "/user/hand/right",
        action,
        actionPath,
        outCurls,
        physicalLeftHand ? "magazine left hand" : "magazine right hand");
}

void VR::UpdateNativeViewmodelLeftHandOpenVRFingerCurls()
{
    std::array<float, 5> curls{};
    const bool valid =
        m_NativeViewmodelLeftHandOpenVRSkeleton &&
        m_Input &&
        ReadOpenVRSkeletalFingerCurls(
            m_Input,
            m_System,
            m_InputActionFrame,
            GetPhysicalControllerIndexForHand(true),
            "/user/hand/left",
            m_NativeViewmodelLeftHandOpenVRAction,
            "/actions/base/in/skeleton_lefthand",
            curls,
            "native viewmodel left hand");

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_NativeViewmodelLeftHandOpenVRFingerCurlMutex);
    if (valid)
    {
        m_NativeViewmodelLeftHandOpenVRFingerCurls = curls;
        m_NativeViewmodelLeftHandOpenVRFingerCurlsAt = now;
        m_NativeViewmodelLeftHandOpenVRFingerCurlsValid = true;
        return;
    }

    m_NativeViewmodelLeftHandOpenVRFingerCurlsValid = false;
}

bool VR::GetNativeViewmodelLeftHandOpenVRFingerCurls(std::array<float, 5>& outCurls) const
{
    outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (!m_NativeViewmodelLeftHandOpenVRSkeleton || !IsInputActionFrameAvailable())
        return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_NativeViewmodelLeftHandOpenVRFingerCurlMutex);
    if (!m_NativeViewmodelLeftHandOpenVRFingerCurlsValid ||
        std::chrono::duration<float>(now - m_NativeViewmodelLeftHandOpenVRFingerCurlsAt).count() > 0.35f)
    {
        return false;
    }

    outCurls = m_NativeViewmodelLeftHandOpenVRFingerCurls;
    return true;
}

void VR::UpdateNativeViewmodelRightHandOpenVRFingerCurls()
{
    std::array<float, 5> curls{};
    const bool physicalLeftHand = IsGameplayHandLeftPhysical(false);
    vr::VRActionHandle_t& action = physicalLeftHand
        ? m_NativeViewmodelLeftHandOpenVRAction
        : m_NativeViewmodelRightHandOpenVRAction;
    const char* actionPath = physicalLeftHand
        ? "/actions/base/in/skeleton_lefthand"
        : "/actions/base/in/skeleton_righthand";
    const bool valid =
        m_NativeViewmodelLeftHandOpenVRSkeleton &&
        m_Input &&
        ReadOpenVRSkeletalFingerCurls(
            m_Input,
            m_System,
            m_InputActionFrame,
            GetPhysicalControllerIndexForHand(physicalLeftHand),
            physicalLeftHand ? "/user/hand/left" : "/user/hand/right",
            action,
            actionPath,
            curls,
            physicalLeftHand
                ? "native viewmodel gameplay right hand (physical left)"
                : "native viewmodel right hand");

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_NativeViewmodelRightHandOpenVRFingerCurlMutex);
    if (valid)
    {
        m_NativeViewmodelRightHandOpenVRFingerCurls = curls;
        m_NativeViewmodelRightHandOpenVRFingerCurlsAt = now;
        m_NativeViewmodelRightHandOpenVRFingerCurlsValid = true;
        return;
    }

    m_NativeViewmodelRightHandOpenVRFingerCurlsValid = false;
}

bool VR::GetNativeViewmodelRightHandOpenVRFingerCurls(std::array<float, 5>& outCurls) const
{
    outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (!m_NativeViewmodelLeftHandOpenVRSkeleton || !IsInputActionFrameAvailable())
        return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_NativeViewmodelRightHandOpenVRFingerCurlMutex);
    if (!m_NativeViewmodelRightHandOpenVRFingerCurlsValid ||
        std::chrono::duration<float>(now - m_NativeViewmodelRightHandOpenVRFingerCurlsAt).count() > 0.35f)
    {
        return false;
    }

    outCurls = m_NativeViewmodelRightHandOpenVRFingerCurls;
    return true;
}

bool VR::GetWorldPoseAnatomicalRightHandOpenVRFingerCurls(
    std::array<float, 5>& outCurls) const
{
    outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // In the normal right-handed layout the existing gameplay-right cache is
    // already the anatomical physical-right hand. Left-handed mode maps the
    // gameplay-right cache to the physical-left controller, so read the
    // physical-right skeleton action directly for network world-pose data.
    if (!m_LeftHanded)
        return GetNativeViewmodelRightHandOpenVRFingerCurls(outCurls);

    if (!m_NativeViewmodelLeftHandOpenVRSkeleton || !m_Input)
        return false;

    return ReadOpenVRSkeletalFingerCurls(
        m_Input,
        m_System,
        m_InputActionFrame,
        GetPhysicalControllerIndexForHand(false),
        "/user/hand/right",
        m_NativeViewmodelRightHandOpenVRAction,
        "/actions/base/in/skeleton_righthand",
        outCurls,
        "world pose anatomical right hand");
}

bool VR::UpdateNativeAmmoReloadButtonContext(C_BasePlayer* localPlayer)
{
    C_WeaponCSBase* weapon = nullptr;
    C_WeaponCSBase::WeaponID weaponId = C_WeaponCSBase::WeaponID::NONE;
    int clip = -1;
    const bool liveWeapon = m_IsVREnabled && m_Game && m_Game->m_EngineClient &&
        m_Game->m_EngineClient->IsInGame() &&
        MagazineInteractionReadActiveWeapon(localPlayer, weapon, weaponId, clip);
    const int id = static_cast<int>(weaponId);
    const uint32_t handle = liveWeapon ? MagazineInteractionActiveWeaponHandle(m_Game, localPlayer, weapon) : 0u;
    const bool reloadable = l4d2vr_remote_mag::IsDetachable(id) || l4d2vr_shell::IsShotgun(id) ||
        weaponId == C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER;
    const bool enabled = liveWeapon && l4d2vr_shell::ValidHandle(handle) && reloadable &&
        (!m_MagazineInteractionEnabled || (!m_VrHandsEnabled && !m_NativeViewmodelHandsOnly) ||
            m_NativeAmmoReloadFallbackActive.load(std::memory_order_acquire));
    const unsigned mode = (m_MagazineInteractionEnabled ? 1u : 0u) |
        (m_VrHandsEnabled ? 2u : 0u) | (m_NativeViewmodelHandsOnly ? 4u : 0u);
    if (m_NativeAmmoReloadButtonSession.Observe(enabled, reinterpret_cast<uintptr_t>(localPlayer),
            reinterpret_cast<uintptr_t>(weapon), id, mode, handle))
        m_NativeAmmoReloadFallbackPulse.Reset();
    return enabled;
}

bool VR::UpdateMagazineInteraction(
    C_BasePlayer* localPlayer,
    bool leftGripDown,
    bool leftGripJustPressed,
    bool leftSupportHandDown,
    bool leftSupportHandJustPressed,
    bool allowGameplayInputOnTwoHandedGripRelease)
{
    m_NativeAmmoReloadFallbackActive.store(false, std::memory_order_release);
    m_PhysicalReloadOwnsStockInput.store(false, std::memory_order_release);
    C_WeaponCSBase* activeWeapon = nullptr;
    C_WeaponCSBase::WeaponID activeWeaponId = C_WeaponCSBase::WeaponID::NONE;
    int activeClip = -1;
    const bool hasActiveWeapon = MagazineInteractionReadActiveWeapon(
        localPlayer,
        activeWeapon,
        activeWeaponId,
        activeClip);
    m_MagazineInteractionCurrentWeaponId.store(
        hasActiveWeapon ? static_cast<int>(activeWeaponId) : 0,
        std::memory_order_relaxed);

    unsigned char lifeState = 0xFF;
    const bool vrHandsInteractionAvailable = m_IsVREnabled &&
        (m_VrHandsEnabled || m_NativeViewmodelHandsOnly) &&
        m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame() &&
        !m_RenderPlayerIncap.load(std::memory_order_acquire) &&
        !m_RenderTpObserver.load(std::memory_order_acquire) && hasActiveWeapon &&
        MagazineInteractionTryReadValue(localPlayer, kLifeStateOffset, lifeState) && lifeState == 0;
    const int activeWeaponIdInt = static_cast<int>(activeWeaponId);
    const uint32_t activeWeaponHandle = vrHandsInteractionAvailable
        ? MagazineInteractionActiveWeaponHandle(m_Game, localPlayer, activeWeapon) : 0u;
    m_PhysicalReloadChambers.ObserveOwner(vrHandsInteractionAvailable && m_MagazineInteractionEnabled &&
        m_MagazineInteractionSuppressEmptyClipAutoReload,
        reinterpret_cast<uintptr_t>(localPlayer));
    m_PhysicalReloadObservedEmpty = m_PhysicalReloadChambers.Observe(
        reinterpret_cast<uintptr_t>(activeWeapon),
        activeWeaponHandle, activeWeaponIdInt,
        MagazineInteractionWeaponUsesPhysicalReload(activeWeaponId) ? activeClip : -1);
    // Use one native transaction path for the listen-host and joining clients.
    // Latch its availability into the session identity so a late capability or
    // timeout cannot mix a predicted local ejection with a server insertion.
    const auto pistolHand = GetPrimaryPistolHand();
    const unsigned pistolBit = l4d2vr_pistol::Bit(pistolHand);
    l4d2vr_pistol_sync::State pistolMagazineState{};
    const bool pistolGestureEligible = vrHandsInteractionAvailable &&
        activeWeaponId == C_WeaponCSBase::WeaponID::PISTOL &&
        l4d2vr_shell::ValidHandle(activeWeaponHandle) &&
        !m_Game->IsDualPistolWeapon(activeWeapon) &&
        PistolMagazineProtocolSupported();
    const bool havePistolMagazineState = pistolGestureEligible &&
        ReadPistolMagazineState(localPlayer, activeWeapon, pistolMagazineState);
    const bool authoritativePistol = pistolGestureEligible &&
        ((havePistolMagazineState && l4d2vr_pistol_reload::MatchesSingleHand(pistolMagazineState, pistolHand)) ||
            (IsMagazineInteractionManualActive() && m_MagazineInteractionPistolHandle == activeWeaponHandle &&
                m_MagazineInteractionWeapon == activeWeapon &&
                m_MagazineInteractionSession.OwnerTag() == reinterpret_cast<uintptr_t>(localPlayer)));
    if (havePistolMagazineState && (pistolMagazineState.physical & pistolBit) != 0u)
        m_PhysicalReloadObservedEmpty = (pistolMagazineState.chambered & pistolBit) == 0u || activeClip <= 0;
    const bool authoritativeMagazine = !authoritativePistol && vrHandsInteractionAvailable &&
        l4d2vr_magazine::UseAuthoritativeMagazine(RemoteMagazineProtocolSupported(),
            MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId),
            m_Game->IsDualPistolWeapon(activeWeapon),
            m_LeftHandPistolActive.load(std::memory_order_acquire));
    const bool authoritativeShell = vrHandsInteractionAvailable &&
        MagazineInteractionWeaponUsesShotgunShells(activeWeaponId) && RemoteShellProtocolSupported();
    m_PhysicalReloadOwnsStockInput.store(m_MagazineInteractionEnabled &&
        m_FirstPersonControlReady.load(std::memory_order_acquire) && !m_MouseModeEnabled &&
        !m_SuppressPlayerInput && (authoritativePistol || authoritativeMagazine || authoritativeShell),
        std::memory_order_release);
    const unsigned int interactionInputMode = (m_MagazineInteractionEnabled ? 1u : 0u) |
        (m_VrHandsEnabled ? 2u : 0u) | (m_NativeViewmodelHandsOnly ? 4u : 0u) |
        (m_DualPistolsNativeReloadFallbackEnabled ? 8u : 0u) |
        (m_DualPistolsActive.load(std::memory_order_acquire) ? 16u : 0u) |
        (m_LeftHandPistolActive.load(std::memory_order_acquire) ? 32u : 0u) |
        (authoritativeMagazine ? l4d2vr_magazine::kAuthoritativeMagazineInputMode : 0u) |
        (authoritativeShell ? l4d2vr_magazine::kAuthoritativeShellInputMode : 0u) |
        (authoritativePistol ? l4d2vr_magazine::kAuthoritativePistolInputMode : 0u);
    if (m_MagazineInteractionSession.Observe(vrHandsInteractionAvailable,
            reinterpret_cast<uintptr_t>(localPlayer), reinterpret_cast<uintptr_t>(activeWeapon),
            activeWeaponIdInt, interactionInputMode, activeWeaponHandle))
    {
        if (m_VrHandsDebugLog && vrHandsInteractionAvailable)
            Game::logMsg("[VR][ReloadChamber] weapon=%p handle=0x%X weaponId=%d empty=%d retainedAcrossDraws=%d",
                activeWeapon, activeWeaponHandle, activeWeaponIdInt, m_PhysicalReloadObservedEmpty ? 1 : 0,
                activeWeaponHandle != 0u && m_MagazineInteractionEnabled &&
                    m_MagazineInteractionSuppressEmptyClipAutoReload ? 1 : 0);
        ResetMagazineInteractionSession();
    }
    if (!vrHandsInteractionAvailable)
        return false;

    if (!authoritativePistol && m_DualPistolsNativeReloadFallbackEnabled && m_Game && localPlayer &&
        (m_LeftHandPistolActive.load(std::memory_order_acquire) ||
            m_Game->IsDualPistolWeapon(reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon()))))
    {
        m_PhysicalReloadChambers.Forget(reinterpret_cast<uintptr_t>(activeWeapon));
        m_PhysicalReloadObservedEmpty = false;
        if (IsMagazineInteractionManualActive() || m_VrHandsTwoHandedGripActive)
            ResetMagazineInteractionSession();
        CancelMagazineInteractionManual();
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        m_BodyAmmoAvailable.store(false, std::memory_order_release);
        return false;
    }
    bool bodyAmmoAvailable = false;
    const bool nativeAmmoFallback = l4d2vr_magazine::UseNativeReloadFallback(
        m_MagazineInteractionEnabled, activeWeaponIdInt,
        MagazineInteractionWeaponUsesPhysicalReload(activeWeaponId),
        authoritativeMagazine || authoritativePistol || authoritativeShell);
    if (m_VrHandsDebugLog && MagazineInteractionWeaponUsesPhysicalReload(activeWeaponId))
    {
        static uint32_t lastHandle = 0u;
        static unsigned lastMode = ~0u;
        static uint64_t lastLogAt = 0u;
        const unsigned mode = (authoritativeMagazine ? 1u : 0u) | (authoritativePistol ? 2u : 0u) |
            (authoritativeShell ? 4u : 0u) | (nativeAmmoFallback ? 8u : 0u);
        const uint64_t logAt = GetTickCount64();
        if ((activeWeaponHandle != lastHandle || mode != lastMode) && logAt - lastLogAt >= 1000u)
        {
            lastHandle = activeWeaponHandle; lastMode = mode; lastLogAt = logAt;
            Game::logMsg("[VR][ReloadBackend] weaponId=%d physicalMagazine=%d physicalPistol=%d physicalShell=%d nativeFallback=%d protocol=%u encode=%d",
                activeWeaponIdInt, authoritativeMagazine ? 1 : 0, authoritativePistol ? 1 : 0,
                authoritativeShell ? 1 : 0, nativeAmmoFallback ? 1 : 0,
                m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire), m_EncodeVRUsercmd ? 1 : 0);
        }
    }
    if (nativeAmmoFallback)
    {
        // Native reload owns the counters when this server cannot negotiate
        // physical magazine transactions. Do not synthesize a magazine or
        // retain an empty-chamber latch through a stock reload.
        m_PhysicalReloadChambers.Forget(reinterpret_cast<uintptr_t>(activeWeapon));
        m_PhysicalReloadObservedEmpty = false;
        if (IsMagazineInteractionManualActive()) CancelMagazineInteractionManual();
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        m_BodyAmmoAvailable.store(false, std::memory_order_release);
        m_NativeAmmoReloadFallbackActive.store(true, std::memory_order_release);
        m_MagazineInteractionNativeReloadSuppressUntil = {};
        m_MagazineInteractionNativeReloadSuppressWeaponId = 0;
        m_ManualPumpBlockingFire.store(false, std::memory_order_release);
        if (MagazineInteractionWeaponUsesShotgunShells(activeWeaponId))
        {
            std::lock_guard<std::mutex> lock(m_ManualPumpMutex);
            m_ManualPumpCycles.ObserveOwner(false, 0u);
        }
    }
    struct BodyAmmoPublication
    {
        std::atomic<bool>& destination;
        bool& value;
        ~BodyAmmoPublication() { destination.store(value, std::memory_order_release); }
    } bodyAmmoPublication{ m_BodyAmmoAvailable, bodyAmmoAvailable };
    const bool supportLeft = MagazineReloadSupportIsLeft();
    const Vector supportPosition = supportLeft ? m_LeftControllerPosAbs : m_RightControllerPosAbs;
    const QAngle supportAngles = supportLeft ? m_LeftControllerAngAbs : m_RightControllerAngAbs;
    const Vector supportForward = supportLeft ? m_LeftControllerForward : m_RightControllerForward;
    const Vector supportRight = supportLeft ? m_LeftControllerRight : m_RightControllerRight;
    const Vector supportUp = supportLeft ? m_LeftControllerUp : m_RightControllerUp;
    const Vector gunPosition = supportLeft ? m_RightControllerPosAbs : m_LeftControllerPosAbs;
    const Vector gunForward = supportLeft ? m_RightControllerForward : m_LeftControllerForward;
    const Vector gunForwardUnforced = supportLeft ? m_RightControllerForwardUnforced : m_LeftControllerForward;
    const Vector gunRight = supportLeft ? m_RightControllerRight : m_LeftControllerRight;
    const Vector gunUp = supportLeft ? m_RightControllerUp : m_LeftControllerUp;
    const auto now = std::chrono::steady_clock::now();
    const bool magazineInteractionPhysicalLeftHand = IsGameplayHandLeftPhysical(MagazineReloadSupportIsLeft());
    if (m_MagazineInteractionSuppressLeftInputUntilRelease && !leftGripDown)
    {
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        Game::logMsg("[VR][MagazineInteraction] interaction gesture released after physical reload; normal left-hand input restored");
    }
    if (m_MagazineInteractionSuppressLeftInputUntilRelease &&
        !(m_ManualPumpEnabled && MagazineInteractionWeaponUsesSinglePumpSound(
            static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed)))))
        return false;
    auto reloadCommandPending = [&]() -> bool
    {
        if (m_MagazineInteractionReloadCommandPending && !m_MagazineInteractionReloadCommandIssued)
            return true;
        if (m_MagazineInteractionShotgunShellMode &&
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload &&
            m_MagazineInteractionReloadTriggered &&
            m_MagazineInteractionReloadCommandIssued)
        {
            return true;
        }
        return m_MagazineInteractionReloadTriggered &&
            m_MagazineInteractionReloadCommandIssued &&
            m_MagazineInteractionReloadCommandHoldUntil.time_since_epoch().count() != 0 &&
            now < m_MagazineInteractionReloadCommandHoldUntil;
    };

    auto stopNativeShellReloadPrediction = [&](const char*)
    {
        if (!m_Game)
            return;

        m_Game->ClientCmd_Unrestricted("-reload");
        m_ReloadCmdOwned = false;
        m_MagazineInteractionReloadTriggered = false;
        m_MagazineInteractionReloadCommandPending = false;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil = {};
        m_Game->ClientCmd_Unrestricted("-attack");
    };

    auto triggerMagazineInteractionHapticForHand = [&](
        bool physicalLeftHand,
        float durationSeconds,
        float frequency,
        float amplitude,
        int priority = 2)
    {
        TriggerPhysicalHandHapticPulse(
            physicalLeftHand,
            durationSeconds,
            frequency,
            amplitude,
            priority);
    };

    auto triggerMagazineInteractionHaptic = [&](float durationSeconds, float frequency, float amplitude, int priority = 2)
    {
        triggerMagazineInteractionHapticForHand(
            magazineInteractionPhysicalLeftHand,
            durationSeconds,
            frequency,
            amplitude,
            priority);
    };

    auto triggerMagazineInteractionWeaponHandHaptic = [&](float durationSeconds, float frequency, float amplitude, int priority = 2)
    {
        triggerMagazineInteractionHapticForHand(
            IsGameplayHandLeftPhysical(!MagazineReloadSupportIsLeft()),
            durationSeconds,
            frequency,
            amplitude,
            priority);
    };

    auto triggerMagazineInteractionBothHandsHaptic = [&](float durationSeconds, float frequency, float amplitude, int priority = 2)
    {
        triggerMagazineInteractionHapticForHand(true, durationSeconds, frequency, amplitude, priority);
        triggerMagazineInteractionHapticForHand(false, durationSeconds, frequency, amplitude, priority);
    };

    auto leftControllerPosRelativeToHmd = [&]() -> Vector
    {
        return supportPosition - m_HmdPosAbs;
    };

    auto leftControllerPosRelativeToWeapon = [&]() -> Vector
    {
        Vector local{};
        const Vector forward = gunForwardUnforced.IsZero()
            ? gunForward : gunForwardUnforced;
        if (!l4d2vr_interaction::LocalHandPosition(supportPosition, gunPosition,
                forward, gunUp, local))
            return Vector(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
        return local;
    };

    auto updateMagazineInteractionContactHaptic = [&](
        bool& contactActive,
        bool touching,
        float durationSeconds,
        float frequency,
        float amplitude)
    {
        if (touching && !contactActive)
            triggerMagazineInteractionHaptic(durationSeconds, frequency, amplitude, 2);
        contactActive = touching;
    };

    MagazineInteractionBoxSnapshot inputSocketBox{};
    MagazineInteractionBoxSnapshot inputSocketCaptureBox{};
    VrHandMatrix4 inputSocketWorld{};
    VrHandMatrix4 inputSocketCaptureWorld{};
    bool inputSocketValid = false;
    bool isSeparateInput = m_MagazineInteractionSeparateButtonInput;

    auto rebuildInputSocketFromSessionBox = [&]() -> bool
    {
        inputSocketValid = false;
        if (!m_MagazineInteractionSocketValid)
            return false;

        inputSocketBox = m_MagazineInteractionSocketBox;
        inputSocketWorld = m_MagazineInteractionSocketWorld;
        {
            MagazineInteractionBoxSnapshot rebasedBox{};
            VrHandMatrix4 rebasedWorld{};
            if (MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(
                    this,
                    m_MagazineInteractionSocketBox,
                    rebasedBox,
                    rebasedWorld))
            {
                inputSocketBox = rebasedBox;
                inputSocketWorld = rebasedWorld;
            }
        }

        if (!MagazineInteractionMatrixLooksRenderable(inputSocketWorld))
            return false;

        inputSocketCaptureBox = m_MagazineInteractionSocketCaptureBox;
        if (MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketWorld) &&
            MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketCaptureWorld))
        {
            const VrHandMatrix4 socketToCapture = MagazineInteractionBuildControllerRelation(
                m_MagazineInteractionSocketWorld,
                m_MagazineInteractionSocketCaptureWorld);
            inputSocketCaptureWorld = MagazineInteractionBuildWorldFromControllerRelation(
                inputSocketWorld,
                socketToCapture);
            if (MagazineInteractionMatrixLooksRenderable(inputSocketCaptureWorld))
            {
                inputSocketCaptureBox.origin = MagazineInteractionMatrixOrigin(inputSocketCaptureWorld);
                inputSocketCaptureBox.axisX = MagazineInteractionMatrixAxis(inputSocketCaptureWorld, 0);
                inputSocketCaptureBox.axisY = MagazineInteractionMatrixAxis(inputSocketCaptureWorld, 1);
                inputSocketCaptureBox.axisZ = MagazineInteractionMatrixAxis(inputSocketCaptureWorld, 2);
            }
            else
            {
                inputSocketCaptureBox = inputSocketBox;
                inputSocketCaptureWorld = inputSocketWorld;
            }
        }
        else
        {
            inputSocketCaptureBox = inputSocketBox;
            inputSocketCaptureWorld = inputSocketWorld;
        }

        inputSocketValid = true;
        return true;
    };

    auto setDetachedMagazineWorld = [&](const VrHandMatrix4& world)
    {
        if (!MagazineInteractionMatrixLooksRenderable(world))
        {
            static std::chrono::steady_clock::time_point s_lastBadWorldLog{};
            if (s_lastBadWorldLog.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - s_lastBadWorldLog).count() >= 0.50f)
            {
                s_lastBadWorldLog = now;
                Game::logMsg(
                    "[VR][MagazineInteraction] ignored invalid detached magazine world origin=(%.2f %.2f %.2f)",
                    VrHandMath::Get(world, 0, 3),
                    VrHandMath::Get(world, 1, 3),
                    VrHandMath::Get(world, 2, 3));
            }
            return;
        }
        std::lock_guard<std::mutex> lock(m_MagazineInteractionPoseMutex);
        m_MagazineInteractionDetachedMagazineWorld = world;
    };

    auto buildHeldMagazineWorldFromLeftHand = [&]() -> VrHandMatrix4
    {
        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
            this,
            supportPosition,
            supportForward,
            supportRight,
            supportUp);
        VrHandMatrix4 orientationWorld = MagazineInteractionBuildWorldFromControllerRelation(
            controllerWorld,
            m_MagazineInteractionControllerToMagazine);
        if (!MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            orientationWorld = MagazineInteractionBuildFreshHandMagazineWorld(this);

        const Vector desiredCenter =
            MagazineInteractionMatrixOrigin(controllerWorld) +
            MagazineInteractionMatrixLocalVectorToWorld(
                controllerWorld,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal);
        return MagazineInteractionBuildWorldAtBoxCenter(
            orientationWorld,
            m_MagazineInteractionSocketBox,
            desiredCenter);
    };

    auto buildFreshHeldMagazineWorldFromLeftHand = [&]() -> VrHandMatrix4
    {
        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
            this,
            supportPosition,
            supportForward,
            supportRight,
            supportUp);
        const l4d2vr_ammo_grip::Pose pose{m_MagazineInteractionControllerToMagazine,
            m_MagazineInteractionHeldMagazineCenterOffsetLocal};
        VrHandMatrix4 world{};
        l4d2vr_ammo_grip::Follow(controllerWorld, pose,
            MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox), world);
        return world;
    };

    auto snapFreshMagazineCenterOffsetToLeftHandAnchor =
        [&](const VrHandMatrix4& controllerWorld,
            bool& outUsedWristAnchor,
            Vector& outTargetHandAnchorWorld,
            Vector& outFreshHandAnchorLocal,
            Vector& outFreshCenterLocal) -> bool
    {
        outFreshCenterLocal = MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox);
        outFreshHandAnchorLocal =
            MagazineInteractionFreshMagazineHandAnchorLocal(this, m_MagazineInteractionSocketBox);
        if (!MagazineInteractionResolveFreshMagazineHandAnchorWorld(
            this,
            controllerWorld,
            outTargetHandAnchorWorld,
            outUsedWristAnchor))
        {
            return false;
        }

        l4d2vr_ammo_grip::Pose pose{};
        if (!l4d2vr_ammo_grip::Capture(controllerWorld, m_MagazineInteractionMagazineHandRotationOffsetDeg,
            outTargetHandAnchorWorld, outFreshCenterLocal, outFreshHandAnchorLocal, pose)) return false;
        m_MagazineInteractionControllerToMagazine = pose.orientationLocal;
        m_MagazineInteractionHeldMagazineCenterOffsetLocal = pose.centerOffsetLocal;
        return true;
    };

    auto updateDetachedMagazineFromLeftHand = [&]()
    {
        setDetachedMagazineWorld(buildHeldMagazineWorldFromLeftHand());
    };

    auto updateFreshDetachedMagazineFromLeftHand = [&]()
    {
        setDetachedMagazineWorld(buildFreshHeldMagazineWorldFromLeftHand());
    };

    auto applyShotgunStableSocketCapture = [&]() -> bool
    {
        if (!m_MagazineInteractionShotgunStableCaptureValid)
            return false;

        VrHandMatrix4 modelWorld{};
        if (!MagazineInteractionBuildModelBasisWorld(m_MagazineInteractionSocketBox, modelWorld))
            return false;

        const VrHandMatrix4 captureWorld = MagazineInteractionBuildWorldFromControllerRelation(
            modelWorld,
            m_MagazineInteractionShotgunStableCaptureModelLocal);
        if (!MagazineInteractionMatrixLooksRenderable(captureWorld))
            return false;

        MagazineInteractionBoxSnapshot captureBox = m_MagazineInteractionShotgunStableCaptureBox;
        captureBox.origin = MagazineInteractionMatrixOrigin(captureWorld);
        captureBox.axisX = MagazineInteractionMatrixAxis(captureWorld, 0);
        captureBox.axisY = MagazineInteractionMatrixAxis(captureWorld, 1);
        captureBox.axisZ = MagazineInteractionMatrixAxis(captureWorld, 2);
        captureBox.entityIndex = m_MagazineInteractionSocketBox.entityIndex;
        captureBox.boneIndex = m_MagazineInteractionSocketBox.boneIndex;
        captureBox.modelName = m_MagazineInteractionSocketBox.modelName;
        captureBox.frameSeq = m_MagazineInteractionSocketBox.frameSeq;
        captureBox.publishSeq = m_MagazineInteractionSocketBox.publishSeq;
        captureBox.publishedAt = now;
        captureBox.modelBasisValid = m_MagazineInteractionSocketBox.modelBasisValid;
        captureBox.modelOrigin = m_MagazineInteractionSocketBox.modelOrigin;
        captureBox.modelAxisX = m_MagazineInteractionSocketBox.modelAxisX;
        captureBox.modelAxisY = m_MagazineInteractionSocketBox.modelAxisY;
        captureBox.modelAxisZ = m_MagazineInteractionSocketBox.modelAxisZ;
        m_MagazineInteractionSocketCaptureBox = captureBox;
        m_MagazineInteractionSocketCaptureWorld = captureWorld;
        return true;
    };

    auto captureShotgunStableSocketCapture = [&](const char*) -> bool
    {
        if (!m_MagazineInteractionShotgunShellMode ||
            !MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketCaptureWorld))
        {
            return false;
        }

        VrHandMatrix4 modelWorld{};
        if (!MagazineInteractionBuildModelBasisWorld(m_MagazineInteractionSocketBox, modelWorld))
            return false;

        const VrHandMatrix4 stableLocal = MagazineInteractionBuildControllerRelation(
            modelWorld,
            m_MagazineInteractionSocketCaptureWorld);
        if (!MagazineInteractionMatrixBasisLooksValid(stableLocal))
            return false;

        m_MagazineInteractionShotgunStableCaptureModelLocal = stableLocal;
        m_MagazineInteractionShotgunStableCaptureBox = m_MagazineInteractionSocketCaptureBox;
        m_MagazineInteractionShotgunStableCaptureValid = true;
        return true;
    };

    auto rebuildSocketCaptureFromSocket = [&]()
    {
        if (m_MagazineInteractionShotgunShellMode &&
            (m_MagazineInteractionShotgunShellsLoadedThisSession > 0 ||
                (m_MagazineInteractionSuppressEmptyClipAutoReload &&
                    m_MagazineInteractionChamberEmpty)) &&
            applyShotgunStableSocketCapture())
        {
            return;
        }

        MagazineInteractionBoxSnapshot captureBox{};
        VrHandMatrix4 captureWorld{};
        if (MagazineInteractionBuildSocketCaptureBox(
            this,
            m_MagazineInteractionSocketBox,
            captureBox,
            captureWorld))
        {
            m_MagazineInteractionSocketCaptureBox = captureBox;
            m_MagazineInteractionSocketCaptureWorld = captureWorld;
            return;
        }

        m_MagazineInteractionSocketCaptureBox = m_MagazineInteractionSocketBox;
        m_MagazineInteractionSocketCaptureWorld = m_MagazineInteractionSocketWorld;
    };

    auto refreshSocketFromPublishedViewmodelBox = [&]()
    {
        if (!m_MagazineInteractionSocketValid)
            return;

        MagazineInteractionBoxSnapshot box{};
        if (!GetMagazineInteractionBox(box))
            return;

        const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
            return;
        if (m_MagazineInteractionViewmodelEntityIndex >= 0 &&
            box.entityIndex != m_MagazineInteractionViewmodelEntityIndex)
        {
            return;
        }
        if (m_MagazineInteractionMagazineBoneIndex >= 0 &&
            box.boneIndex != m_MagazineInteractionMagazineBoneIndex)
        {
            return;
        }
        if (!m_MagazineInteractionMagazineModelName.empty() &&
            !box.modelName.empty() &&
            box.modelName != m_MagazineInteractionMagazineModelName)
        {
            return;
        }

        const VrHandMatrix4 boxWorld = MagazineInteractionBuildBoxWorld(box);
        if (!MagazineInteractionMatrixLooksRenderable(boxWorld))
            return;

        m_MagazineInteractionSocketBox = box;
        m_MagazineInteractionSocketWorld = boxWorld;
        rebuildSocketCaptureFromSocket();
    };

    auto setBoltPullDistance = [&](float pullDistance)
    {
        const float maxPull = std::max(
            0.0f,
            std::max(m_MagazineInteractionBoltMaxPullDistance,
                MagazineInteractionResolveBoltPullDistanceMeters(this) * m_VRScale));
        m_MagazineInteractionBoltPullDistance = std::clamp(pullDistance, 0.0f, maxPull);
        if (!m_MagazineInteractionBoltRestValid ||
            !MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltRestWorld))
        {
            m_MagazineInteractionBoltWorld = {};
            return;
        }

        Vector axis = VrHandMath::Normalize(m_MagazineInteractionBoltPullAxisWorld);
        if (axis.Length() <= 0.0001f)
            axis = MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        axis = MagazineInteractionAlignBoltVisualAxisToInputAxis(
            this,
            axis,
            m_MagazineInteractionBoltInputAxisWorld);
        m_MagazineInteractionBoltPullAxisWorld = axis;

        const Vector restOrigin = MagazineInteractionMatrixOrigin(m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltWorld = MagazineInteractionMatrixWithOrigin(
            m_MagazineInteractionBoltRestWorld,
            restOrigin + axis * m_MagazineInteractionBoltPullDistance);
    };

    auto getFreshBoltBoxForActiveViewmodel = [&](MagazineInteractionBoxSnapshot& box) -> bool
    {
        if (!GetMagazineInteractionBoltBox(box))
            return false;

        const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
            return false;
        if (m_MagazineInteractionViewmodelEntityIndex >= 0 &&
            box.entityIndex != m_MagazineInteractionViewmodelEntityIndex)
        {
            return false;
        }
        if (!m_MagazineInteractionMagazineModelName.empty() &&
            !box.modelName.empty() &&
            box.modelName != m_MagazineInteractionMagazineModelName)
        {
            return false;
        }
        {
            MagazineInteractionBoxSnapshot rebasedBox{};
            VrHandMatrix4 rebasedWorld{};
            if (MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(
                    this,
                    box,
                    rebasedBox,
                    rebasedWorld))
            {
                box = rebasedBox;
            }
        }
        return MagazineInteractionMatrixLooksRenderable(MagazineInteractionBuildBoxWorld(box));
    };

    auto refreshBoltFromPublishedViewmodelBox = [&]()
    {
        if (!m_MagazineInteractionBoltRestValid)
            return;

        MagazineInteractionBoxSnapshot box{};
        const bool manualPump = m_ManualPumpEnabled &&
            MagazineInteractionWeaponUsesSinglePumpSound(activeWeaponId) &&
            m_ManualPumpClosedBoltValid && m_ManualPumpClosedBoltWeapon == reinterpret_cast<uintptr_t>(activeWeapon);
        if (manualPump)
        {
            VrHandMatrix4 rebasedWorld{};
            if (!MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(this, m_ManualPumpClosedBoltBox, box, rebasedWorld)) return;
        }
        else if (!getFreshBoltBoxForActiveViewmodel(box)) return;

        const VrHandMatrix4 boxWorld = MagazineInteractionBuildBoxWorld(box);
        if (!MagazineInteractionMatrixLooksRenderable(boxWorld))
            return;

        m_MagazineInteractionBoltRestBox = box;
        m_MagazineInteractionBoltRestWorld = boxWorld;
        m_MagazineInteractionBoltPullAxisWorld =
            MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltInputAxisWorld =
            MagazineInteractionBuildBoltInputAxisWorld(this, m_MagazineInteractionBoltPullAxisWorld);
        setBoltPullDistance(m_MagazineInteractionBoltPullDistance);
    };

    auto beginBoltStage = [&](C_WeaponCSBase::WeaponID weaponId, const char* reason, bool backendReloadStillPending = false) -> bool
    {
        // Whether a bolt cycle is required depends on the chamber state at the
        // start of this physical reload, not on the option that suppresses the
        // game's empty-clip auto reload. A tactical magazine change keeps the
        // chambered round and must not leave the viewmodel frozen waiting for a
        // bolt grab when that suppression option is disabled.
        if (!m_MagazineInteractionChamberEmpty)
            return false;

        if (!MagazineInteractionWeaponRequiresManualBolt(weaponId))
            return false;

        MagazineInteractionBoxSnapshot boltBox{};
        bool haveBoltBox = false;
        if (m_ManualPumpEnabled && MagazineInteractionWeaponUsesSinglePumpSound(weaponId) &&
            m_ManualPumpClosedBoltValid && m_ManualPumpClosedBoltWeapon == reinterpret_cast<uintptr_t>(activeWeapon))
        {
            VrHandMatrix4 currentWorld{};
            haveBoltBox = MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(this,
                m_ManualPumpClosedBoltBox, boltBox, currentWorld);
        }
        if (!haveBoltBox)
            haveBoltBox = getFreshBoltBoxForActiveViewmodel(boltBox);
        if (!haveBoltBox)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] cannot start bolt stage: no fresh bolt/slide box weaponId=%d model=%s reason=%s",
                static_cast<int>(weaponId),
                m_MagazineInteractionMagazineModelName.c_str(),
                reason ? reason : "unknown");
            return false;
        }

        const float requiredPull = std::max(0.0f, MagazineInteractionResolveBoltPullDistanceMeters(this)) * m_VRScale;
        if (!std::isfinite(requiredPull) || requiredPull <= l4d2vr_magazine::kMinimumBoltPullSourceUnits)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] cannot start bolt stage: invalid scaled pull distance weaponId=%d reason=%s",
                static_cast<int>(weaponId),
                reason ? reason : "unknown");
            return false;
        }

        m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionBoltRestBox = boltBox;
        m_MagazineInteractionBoltRestValid = true;
        m_MagazineInteractionBoltRestWorld = MagazineInteractionBuildBoxWorld(boltBox);
        m_MagazineInteractionBoltPullAxisWorld =
            MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltInputAxisWorld =
            MagazineInteractionBuildBoltInputAxisWorld(this, m_MagazineInteractionBoltPullAxisWorld);
        m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = {};
        m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
        m_MagazineInteractionBoltPullDistance = 0.0f;
        m_MagazineInteractionBoltMaxPullDistance = requiredPull;
        m_MagazineInteractionBoltGrabRequiresGripRelease =
            leftGripDown && !m_MagazineInteractionShotgunShellMode;
        m_MagazineInteractionBoltReachedRear = false;
        m_MagazineInteractionBoltPullAxisSignLocked = false;
        m_MagazineInteractionBoltContactActive = false;
        m_MagazineInteractionBoltStageBeforeBackendReloadComplete = backendReloadStillPending;
        m_MagazineInteractionBoltCompletedBeforeBackendReload = false;
        m_MagazineInteractionBoltStageStarted = now;
        m_MagazineInteractionBoltGrabbedAt = {};
        setBoltPullDistance(0.0f);
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        const Vector boltBoxOffsetMeters =
            (m_VRScale > 0.001f)
            ? ((boltBox.mins + boltBox.maxs) * (0.5f / m_VRScale))
            : Vector(0.0f, 0.0f, 0.0f);
        const Vector boltBoxHalfExtentsMeters =
            (m_VRScale > 0.001f)
            ? ((boltBox.maxs - boltBox.mins) * (0.5f / m_VRScale))
            : Vector(0.0f, 0.0f, 0.0f);
        Game::logMsg(
            "[VR][MagazineInteraction] %s; bolt stage armed weaponId=%d ent=%d bone=%d pull=%.2f return=%.2f visualAxis=(%.2f %.2f %.2f) inputAxis=(%.2f %.2f %.2f) boxOffsetMeters=(%.3f %.3f %.3f) boxHalfExtentsMeters=(%.3f %.3f %.3f) model=%s reason=%s",
            backendReloadStillPending ? "backend reload still pending" : "backend reload complete",
            static_cast<int>(weaponId),
            boltBox.entityIndex,
            boltBox.boneIndex,
            requiredPull,
            std::max(0.0f, MagazineInteractionResolveBoltReturnDistanceMeters(this)) * m_VRScale,
            m_MagazineInteractionBoltPullAxisWorld.x,
            m_MagazineInteractionBoltPullAxisWorld.y,
            m_MagazineInteractionBoltPullAxisWorld.z,
            m_MagazineInteractionBoltInputAxisWorld.x,
            m_MagazineInteractionBoltInputAxisWorld.y,
            m_MagazineInteractionBoltInputAxisWorld.z,
            boltBoxOffsetMeters.x,
            boltBoxOffsetMeters.y,
            boltBoxOffsetMeters.z,
            boltBoxHalfExtentsMeters.x,
            boltBoxHalfExtentsMeters.y,
            boltBoxHalfExtentsMeters.z,
            boltBox.modelName.c_str(),
            reason ? reason : "unknown");
        if (m_MagazineInteractionBoltGrabRequiresGripRelease)
            Game::logMsg("[VR][MagazineInteraction] bolt stage armed while interaction gesture is still held; bolt grab requires release first");
        else if (leftGripDown && m_MagazineInteractionShotgunShellMode)
            Game::logMsg("[VR][MagazineInteraction] shotgun bolt stage armed while interaction gesture is still held; allowing continuous grip transition");
        return true;
    };

    auto beginBoltHoldFromCurrentLeftHand = [&](float grabDistance, float grabRange)
    {
        const Vector handLocal = leftControllerPosRelativeToWeapon();
        if (!l4d2vr_interaction::Finite(handLocal))
            return;
        m_MagazineInteractionState = MagazineInteractionManualState::HoldingBolt;
        m_MagazineInteractionLeftHandHolding = true;
        m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = handLocal;
        m_MagazineInteractionBoltGrabStartPullDistance = m_MagazineInteractionBoltPullDistance;
        m_MagazineInteractionBoltGrabbedAt = now;
        m_MagazineInteractionBoltPullAxisSignLocked = false;
        m_MagazineInteractionBoltInputAxisWorld =
            MagazineInteractionBuildBoltInputAxisWorld(this, m_MagazineInteractionBoltPullAxisWorld);
        m_MagazineInteractionBoltPullAxisWorld =
            MagazineInteractionAlignBoltVisualAxisToInputAxis(
                this,
                m_MagazineInteractionBoltPullAxisWorld,
                m_MagazineInteractionBoltInputAxisWorld);
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
        Game::logMsg(
            "[VR][MagazineInteraction] bolt grabbed distance=%.2f range=%.2f pull=%.2f visualAxis=(%.2f %.2f %.2f) inputAxis=(%.2f %.2f %.2f) model=%s",
            grabDistance,
            grabRange,
            m_MagazineInteractionBoltMaxPullDistance,
            m_MagazineInteractionBoltPullAxisWorld.x,
            m_MagazineInteractionBoltPullAxisWorld.y,
            m_MagazineInteractionBoltPullAxisWorld.z,
            m_MagazineInteractionBoltInputAxisWorld.x,
            m_MagazineInteractionBoltInputAxisWorld.y,
            m_MagazineInteractionBoltInputAxisWorld.z,
            m_MagazineInteractionBoltRestBox.modelName.c_str());
    };

    auto completeBoltStage = [&](const char* reason, bool suppressLeftInputUntilRelease, bool pistolCycleConfirmed = false)
    {
        if (authoritativePistol && !pistolCycleConfirmed)
        {
            if (BeginPistolMagazineRequest(m_CurrentPredictedHitFeedbackCmdNumber,
                pistolHand, l4d2vr_pistol::MagazineAction::Cycle))
            {
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForServerPistolCycle;
                m_PistolMagazineCycleSuppressInput = suppressLeftInputUntilRelease;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                setBoltPullDistance(0.0f);
            }
            return;
        }
        const bool manualPump = m_ManualPumpEnabled &&
            MagazineInteractionWeaponUsesSinglePumpSound(static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionWeaponId));
        if (manualPump)
        {
            bool chamberReady = false;
            {
                std::lock_guard<std::mutex> lock(m_ManualPumpMutex);
                chamberReady = m_ManualPumpCycles.Complete(reinterpret_cast<uintptr_t>(m_MagazineInteractionWeapon), activeWeaponHandle);
            }
            if (!chamberReady)
            {
                setBoltPullDistance(0.0f);
                m_MagazineInteractionBoltReachedRear = false;
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                m_MagazineInteractionChamberEmpty = true;
                m_MagazineInteractionOneInChamber = false;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionBoltContactActive = false;
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                MagazineInteractionPlayBoltSound(this, true);
                Game::logMsg("[VR][ManualPump] empty stroke completed; load shells before chambering weaponId=%d clip=%d",
                    activeWeaponIdInt, activeClip);
                return;
            }
            m_ManualPumpBlockingFire.store(false, std::memory_order_release);
            suppressLeftInputUntilRelease = false;
            m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        }
        m_PhysicalReloadChambers.Complete(reinterpret_cast<uintptr_t>(m_MagazineInteractionWeapon));
        m_PhysicalReloadObservedEmpty = false;
        m_MagazineInteractionChamberEmpty = false;
        setBoltPullDistance(0.0f);
        MagazineInteractionPlayBoltSound(this, true);
        triggerMagazineInteractionWeaponHandHaptic(0.030f, 95.0f, 0.45f, 2);
        if (m_MagazineInteractionBoltStageBeforeBackendReloadComplete)
        {
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBackendReload;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionBoltRestValid = false;
            m_MagazineInteractionBoltRestWorld = {};
            m_MagazineInteractionBoltWorld = {};
            m_MagazineInteractionBoltInputAxisWorld = {};
            m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = {};
            m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
            m_MagazineInteractionBoltPullDistance = 0.0f;
            m_MagazineInteractionBoltMaxPullDistance = 0.0f;
            m_MagazineInteractionBoltGrabRequiresGripRelease = false;
            m_MagazineInteractionBoltReachedRear = false;
            m_MagazineInteractionBoltPullAxisSignLocked = false;
            m_MagazineInteractionBoltStageBeforeBackendReloadComplete = false;
            m_MagazineInteractionBoltCompletedBeforeBackendReload = true;
            m_MagazineInteractionBoltStageStarted = {};
            m_MagazineInteractionBoltGrabbedAt = {};
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            Game::logMsg(
                "[VR][MagazineInteraction] bolt returned to battery before backend reload completed; fire remains blocked reason=%s",
                reason ? reason : "unknown");
            return;
        }
        Game::logMsg(
            "[VR][MagazineInteraction] bolt returned to battery; physical reload complete reason=%s",
            reason ? reason : "unknown");
        const auto sessionWeaponId =
            static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionWeaponId);
        const bool shouldSuppressDetachedNativeReload =
            !m_MagazineInteractionShotgunShellMode &&
            m_MagazineInteractionQuickReloadMode &&
            (MagazineInteractionWeaponUsesDetachableMagazine(sessionWeaponId) ||
                m_MagazineInteractionWeaponId != 0);
        if (shouldSuppressDetachedNativeReload)
        {
            const auto until = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(m_MagazineInteractionQuickReloadMode ? 1.15f : 0.85f));
            if (m_MagazineInteractionNativeReloadSuppressUntil.time_since_epoch().count() == 0 ||
                until > m_MagazineInteractionNativeReloadSuppressUntil)
            {
                m_MagazineInteractionNativeReloadSuppressUntil = until;
            }
            m_MagazineInteractionNativeReloadSuppressWeaponId =
                m_MagazineInteractionWeaponId != 0
                    ? m_MagazineInteractionWeaponId
                    : 0;
            if (authoritativePistol) MagazineInteractionClearPistolReloadFlags(m_MagazineInteractionWeapon);
            else MagazineInteractionClearDetachableClientReloadState(m_MagazineInteractionWeapon);
            if (m_Game)
            {
                m_Game->ClientCmd_Unrestricted("-reload");
                m_Game->ClientCmd_Unrestricted("-attack");
            }
            m_ReloadCmdOwned = false;
        }
        CancelMagazineInteractionManual();
        if (suppressLeftInputUntilRelease)
        {
            m_MagazineInteractionSuppressLeftInputUntilRelease = true;
            Game::logMsg("[VR][MagazineInteraction] bolt completed while interaction gesture is still held; suppressing normal reload until release");
        }
    };

    auto detachedMagazineFitsSocket = [&]() -> bool
    {
        if (!m_MagazineInteractionSocketValid)
            return false;

        const bool useInputSocket = inputSocketValid;
        VrHandMatrix4 socketWorld = m_MagazineInteractionSocketWorld;
        if (useInputSocket)
        {
            socketWorld = MagazineInteractionMatrixLooksRenderable(inputSocketCaptureWorld)
                ? inputSocketCaptureWorld
                : inputSocketWorld;
        }
        else if (MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketCaptureWorld))
        {
            socketWorld = m_MagazineInteractionSocketCaptureWorld;
        }
        const VrHandMatrix4 detachedWorld = m_MagazineInteractionDetachedMagazineWorld;
        if (!MagazineInteractionMatrixLooksRenderable(socketWorld) ||
            !MagazineInteractionMatrixLooksRenderable(detachedWorld))
        {
            return false;
        }

        const MagazineInteractionBoxSnapshot& socketCaptureBox = useInputSocket
            ? (MagazineInteractionMatrixLooksRenderable(inputSocketCaptureWorld)
                ? inputSocketCaptureBox
                : inputSocketBox)
            : (MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketCaptureWorld)
                ? m_MagazineInteractionSocketCaptureBox
                : m_MagazineInteractionSocketBox);
        if (m_MagazineInteractionShotgunShellMode)
        {
            // A loose cylindrical shell is loaded by contact with the port.
            // Its roll is not constrained like a rectangular detachable magazine.
            const Vector shellCenter = MagazineInteractionMatrixPointWorld(detachedWorld,
                MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox));
            const Vector portCenter = MagazineInteractionMatrixPointWorld(socketWorld,
                MagazineInteractionBoxCenterLocal(socketCaptureBox));
            const float radius = std::max(0.02f, m_MagazineInteractionSocketCaptureRadiusMeters) * m_VRScale;
            const bool touching = l4d2vr_interaction::ShellTouchesPort(shellCenter, portCenter, radius);
            if (m_VrHandsDebugLog)
            {
                static uint64_t lastLogAt = 0u;
                const uint64_t logAt = GetTickCount64();
                if (logAt - lastLogAt >= 1000u)
                {
                    lastLogAt = logAt;
                    Game::logMsg("[VR][ShellInsert][client] weaponId=%d touching=%d distanceUnits=%.2f radiusUnits=%.2f inputSocket=%d clip=%d",
                        activeWeaponIdInt, touching ? 1 : 0, (shellCenter - portCenter).Length(), radius,
                        useInputSocket ? 1 : 0, activeClip);
                }
            }
            return touching;
        }
        const Vector detachedAxes[3] =
        {
            MagazineInteractionMatrixAxis(detachedWorld, 0),
            MagazineInteractionMatrixAxis(detachedWorld, 1),
            MagazineInteractionMatrixAxis(detachedWorld, 2)
        };
        const Vector socketAxes[3] =
        {
            MagazineInteractionMatrixAxis(socketWorld, 0),
            MagazineInteractionMatrixAxis(socketWorld, 1),
            MagazineInteractionMatrixAxis(socketWorld, 2)
        };

        auto detachedInsertProbeTouchesSocket = [&]() -> bool
        {
            const int insertionAxis = std::clamp(
                MagazineInteractionDominantAxis(m_MagazineInteractionMagazineInsertionAxisLocal),
                0,
                2);
            const int sideAxisA = (insertionAxis + 1) % 3;
            const int sideAxisB = (insertionAxis + 2) % 3;
            const Vector detachedMins = m_MagazineInteractionSocketBox.mins;
            const Vector detachedMaxs = m_MagazineInteractionSocketBox.maxs;
            const Vector detachedCenter = (detachedMins + detachedMaxs) * 0.5f;
            auto axisValue = [](const Vector& value, int axis) -> float
            {
                return (axis == 0) ? value.x : (axis == 1) ? value.y : value.z;
            };
            auto setAxisValue = [](Vector value, int axis, float component) -> Vector
            {
                if (axis == 0)
                    value.x = component;
                else if (axis == 1)
                    value.y = component;
                else
                    value.z = component;
                return value;
            };

            Vector probes[15]{};
            int probeCount = 0;
            const float insertionValues[] =
            {
                axisValue(detachedMins, insertionAxis),
                axisValue(detachedCenter, insertionAxis),
                axisValue(detachedMaxs, insertionAxis)
            };
            for (float insertionValue : insertionValues)
            {
                Vector probe = setAxisValue(detachedCenter, insertionAxis, insertionValue);
                probes[probeCount++] = probe;
                probes[probeCount++] = setAxisValue(probe, sideAxisA, axisValue(detachedMins, sideAxisA));
                probes[probeCount++] = setAxisValue(probe, sideAxisA, axisValue(detachedMaxs, sideAxisA));
                probes[probeCount++] = setAxisValue(probe, sideAxisB, axisValue(detachedMins, sideAxisB));
                probes[probeCount++] = setAxisValue(probe, sideAxisB, axisValue(detachedMaxs, sideAxisB));
            }

            const Vector socketOrigin = MagazineInteractionMatrixOrigin(socketWorld);
            const float inflate = std::max(0.001f, 0.012f * m_VRScale);
            const Vector socketMins = socketCaptureBox.mins - Vector(inflate, inflate, inflate);
            const Vector socketMaxs = socketCaptureBox.maxs + Vector(inflate, inflate, inflate);
            for (int i = 0; i < probeCount; ++i)
            {
                const Vector probeWorld = MagazineInteractionMatrixPointWorld(detachedWorld, probes[i]);
                const Vector delta = probeWorld - socketOrigin;
                const Vector local(
                    VrHandMath::Dot(delta, socketAxes[0]),
                    VrHandMath::Dot(delta, socketAxes[1]),
                    VrHandMath::Dot(delta, socketAxes[2]));
                if (local.x >= socketMins.x && local.x <= socketMaxs.x &&
                    local.y >= socketMins.y && local.y <= socketMaxs.y &&
                    local.z >= socketMins.z && local.z <= socketMaxs.z)
                {
                    return true;
                }
            }
            return false;
        };

        if (!m_MagazineInteractionShotgunShellMode)
        {
            const float minDot = std::cos(MagazineInteractionResolveSocketCaptureAngleDeg(this) *
                3.14159265358979323846f / 180.0f);
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::fabs(VrHandMath::Dot(detachedAxes[axis], socketAxes[axis])) < minDot)
                    return false;
            }
        }

        const int insertionAxis = std::clamp(
            MagazineInteractionDominantAxis(m_MagazineInteractionMagazineInsertionAxisLocal),
            0,
            2);
        const float overlapFraction = MagazineInteractionResolveSocketRequiredOverlapFraction(this);
        const float requiredDepth = MagazineInteractionResolveSocketRequiredDepthMeters(this) * m_VRScale;
        bool strictOverlapSatisfied = true;
        for (int axis = 0; axis < 3; ++axis)
        {
            float socketMin = 0.0f;
            float socketMax = 0.0f;
            float detachedMin = 0.0f;
            float detachedMax = 0.0f;
            MagazineInteractionProjectBoxOntoAxis(
                socketWorld,
                socketCaptureBox,
                socketAxes[axis],
                socketMin,
                socketMax);
            MagazineInteractionProjectBoxOntoAxis(
                detachedWorld,
                m_MagazineInteractionSocketBox,
                socketAxes[axis],
                detachedMin,
                detachedMax);

            const float socketSpan = std::max(0.001f, socketMax - socketMin);
            const float detachedSpan = std::max(0.001f, detachedMax - detachedMin);
            const float overlap = MagazineInteractionIntervalOverlap(
                socketMin,
                socketMax,
                detachedMin,
                detachedMax);
            const float requiredOverlap = (axis == insertionAxis)
                ? std::max(requiredDepth, std::min(socketSpan, detachedSpan) * overlapFraction)
                : std::min(socketSpan, detachedSpan) * overlapFraction;
            if (overlap < requiredOverlap)
            {
                strictOverlapSatisfied = false;
                break;
            }
        }
        if (strictOverlapSatisfied)
            return true;

        const bool probeTouchesSocket = detachedInsertProbeTouchesSocket();
        if (probeTouchesSocket)
            Game::logMsg("[VR][MagazineInteraction] detached magazine insert accepted by socket probe fallback");
        return probeTouchesSocket;
    };

    bool activePumpNeedsCycle = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualPumpMutex);
        if (!m_ManualPumpEnabled)
            m_ManualPumpCycles.Reset();
        else if (hasActiveWeapon && MagazineInteractionWeaponUsesSinglePumpSound(activeWeaponId))
            activePumpNeedsCycle = m_ManualPumpCycles.Observe(reinterpret_cast<uintptr_t>(activeWeapon), static_cast<int>(activeWeaponId), activeClip, activeWeaponHandle);
    }
    if (m_ManualPumpEnabled && MagazineInteractionWeaponUsesSinglePumpSound(activeWeaponId) &&
        !activePumpNeedsCycle && activeClip > 0 &&
        (m_MagazineInteractionState == MagazineInteractionManualState::Idle ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine))
    {
        MagazineInteractionBoxSnapshot closedBox{};
        if (getFreshBoltBoxForActiveViewmodel(closedBox))
        {
            m_ManualPumpClosedBoltBox = closedBox;
            m_ManualPumpClosedBoltWeapon = reinterpret_cast<uintptr_t>(activeWeapon);
            m_ManualPumpClosedBoltValid = true;
        }
    }
    if (!activePumpNeedsCycle)
        m_ManualPumpBlockingFire.store(false, std::memory_order_release);

    if (m_DualPistolsActive.load(std::memory_order_acquire) || m_LeftHandPistolActive.load(std::memory_order_acquire))
    {
        m_VrHandsTwoHandedGripActive = false;
        m_VrHandsTwoHandedGripWeaponId = 0;
        m_VrHandsTwoHandedGripWeaponTag = 0;
    }

    const bool activeWeaponUsesShotgunShells =
        MagazineInteractionWeaponUsesShotgunShells(activeWeaponId);
    const bool activeWeaponUsesPhysicalReload =
        MagazineInteractionWeaponUsesPhysicalReload(activeWeaponId);
    if (!nativeAmmoFallback && m_BodyGripInventoryEnabled && m_MagazineInteractionEnabled && activeWeaponUsesPhysicalReload &&
        !m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire))
    {
        int ammoType = -1;
        int reserve = 0;
        const bool infinitePistol = activeWeaponId == C_WeaponCSBase::WeaponID::PISTOL ||
            activeWeaponId == C_WeaponCSBase::WeaponID::MAGNUM;
        const bool reserveRead = MagazineInteractionTryReadValue(activeWeapon, kPrimaryAmmoTypeOffset, ammoType) &&
            ammoType >= 0 && ammoType < 32 &&
            MagazineInteractionTryReadValue(localPlayer, kAmmoArrayOffset + ammoType * static_cast<int>(sizeof(int)), reserve) &&
            reserve > 0 && reserve <= 5000;
        bodyAmmoAvailable = infinitePistol || reserveRead;
    }
    if (m_MagazineReleaseButtonRequired && m_MagazineReleaseJustPressed &&
        MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId))
    {
        m_VrHandsTwoHandedGripActive = false;
        m_VrHandsTwoHandedGripWeaponId = 0;
        m_VrHandsTwoHandedGripWeaponTag = 0;
    }
    const bool activeShotgunManualReloadAvailable =
        activeWeaponUsesShotgunShells &&
        IsMagazineInteractionShotgunServerHookActive(static_cast<int>(activeWeaponId));
    const bool activeAuthoritativeMagazineBackend = authoritativeMagazine || authoritativePistol;

    if (m_VrHandsTwoHandedGripActive &&
        (m_VrHandsTwoHandedGripWeaponId != activeWeaponIdInt ||
         m_VrHandsTwoHandedGripWeaponTag != reinterpret_cast<uintptr_t>(activeWeapon)))
    {
        if (m_VrHandsDebugLog)
        {
            Game::logMsg(
                "[VR][Hands] two-hand grip released weaponChanged=%d oldWeapon=%d newWeapon=%d",
                (m_VrHandsTwoHandedGripWeaponId != activeWeaponIdInt) ? 1 : 0,
                m_VrHandsTwoHandedGripWeaponId,
                activeWeaponIdInt);
        }
        m_VrHandsTwoHandedGripActive = false;
        m_VrHandsTwoHandedGripWeaponId = 0;
        m_VrHandsTwoHandedMountFriendlyGripEnteredAt = {};
        m_VrHandsTwoHandedMountFriendlyGripContact = false;
    }

    auto boxFreshForActiveWeapon = [&](const MagazineInteractionBoxSnapshot& box) -> bool
    {
        if (!MagazineInteractionPublishedBoxMatchesWeapon(box, activeWeaponId))
            return false;
        const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        return ageSeconds >= -0.01f && ageSeconds <= std::max(0.02f, m_MagazineInteractionStaleSeconds);
    };

    auto buildTwoHandedGripTargetBox = [&](MagazineInteractionBoxSnapshot& outBox) -> bool
    {
        VrHandMatrix4 targetWorld{};
        Vector targetMins(0.0f, 0.0f, 0.0f);
        Vector targetMaxs(0.0f, 0.0f, 0.0f);
        if (!TryBuildLeftHandTargetDebugBox(
                m_VRScale,
                m_VrHandsTwoHandedGripTargetBoxScale,
                targetWorld,
                targetMins,
                targetMaxs) ||
            !MagazineInteractionMatrixLooksRenderable(targetWorld))
        {
            return false;
        }

        outBox = MagazineInteractionBoxSnapshot{};
        outBox.origin = MagazineInteractionMatrixOrigin(targetWorld);
        outBox.axisX = MagazineInteractionMatrixAxis(targetWorld, 0);
        outBox.axisY = MagazineInteractionMatrixAxis(targetWorld, 1);
        outBox.axisZ = MagazineInteractionMatrixAxis(targetWorld, 2);
        outBox.mins = targetMins;
        outBox.maxs = targetMaxs;
        outBox.modelName = "two_hand_left_target";
        outBox.publishedAt = now;
        return true;
    };

    auto leftHandTouchesTwoHandedGripTarget = [&](float& outDistance) -> bool
    {
        outDistance = FLT_MAX;
        MagazineInteractionBoxSnapshot targetBox{};
        if (!buildTwoHandedGripTargetBox(targetBox))
            return false;

        outDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
            targetBox,
            supportPosition,
            supportAngles,
            m_VRScale,
            this);
        return outDistance <= 0.0f;
    };

    auto leftHandTouchesMagazineForGripExclusion = [&]() -> bool
    {
        MagazineInteractionBoxSnapshot box{};
        if (!GetMagazineInteractionBox(box) || !boxFreshForActiveWeapon(box))
            return false;

        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, box.modelName);
        if (maxClip > 0 && activeClip >= maxClip)
            return false;

        MagazineInteractionBoxSnapshot magazineTouchBox = box;
        if (!activeWeaponUsesShotgunShells)
        {
            MagazineInteractionBoxSnapshot rebasedBox{};
            VrHandMatrix4 rebasedWorld{};
            if (MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(
                    this,
                    box,
                    rebasedBox,
                    rebasedWorld))
            {
                magazineTouchBox = rebasedBox;
            }
        }

        const float exclusionPadding = std::max(0.0f, 0.006f * m_VRScale);
        const float distance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
            magazineTouchBox,
            supportPosition,
            supportAngles,
            m_VRScale,
            this);
        return distance <= exclusionPadding;
    };

    auto clearMountFriendlyGripContact = [&]()
    {
        m_VrHandsTwoHandedMountFriendlyGripEnteredAt = {};
        m_VrHandsTwoHandedMountFriendlyGripContact = false;
    };

    auto leftHandTouchesMountFriendlyGripBox = [&](float& outDistance) -> bool
    {
        outDistance = FLT_MAX;
        MagazineInteractionBoxSnapshot stockBox{};
        if (!TryBuildMountFriendlyTwoHandedGripBox(this, m_VRScale, now, stockBox))
            return false;

        outDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
            stockBox,
            supportPosition,
            supportAngles,
            m_VRScale,
            this);
        return outDistance <= 0.0f;
    };

    auto nativeReloadSuppressWindowActiveForWeapon = [&]() -> bool
    {
        if (m_MagazineInteractionNativeReloadSuppressUntil.time_since_epoch().count() == 0 ||
            now > m_MagazineInteractionNativeReloadSuppressUntil)
        {
            return false;
        }
        return m_MagazineInteractionNativeReloadSuppressWeaponId == 0 ||
            m_MagazineInteractionNativeReloadSuppressWeaponId == static_cast<int>(activeWeaponId);
    };

    const bool continuousPumpSupport = m_ManualPumpEnabled && activeWeaponUsesShotgunShells &&
        m_VrHandsTwoHandedGripHeldMode && leftGripDown &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt);
    const bool manualReloadOwnsOffhandInput = !continuousPumpSupport &&
        (IsMagazineInteractionManualActive() || nativeReloadSuppressWindowActiveForWeapon());

    auto releaseMountFriendlyGrip = [&](const char* reason, bool feedback)
    {
        clearMountFriendlyGripContact();
        m_VrHandsTwoHandedGripActive = false;
        m_VrHandsTwoHandedGripWeaponId = 0;
        if (feedback)
            triggerMagazineInteractionHaptic(0.018f, 85.0f, 0.28f, 2);
        if (m_VrHandsDebugLog)
        {
            Game::logMsg(
                "[VR][Hands] mount-friendly two-hand grip released reason=%s weaponId=%d",
                (reason && reason[0] != '\0') ? reason : "unknown",
                activeWeaponIdInt);
        }
    };

    if (manualReloadOwnsOffhandInput)
        clearMountFriendlyGripContact();

    if (m_VrHandsTwoHandedAimMountFriendly && manualReloadOwnsOffhandInput)
    {
        if (m_VrHandsTwoHandedGripActive)
            releaseMountFriendlyGrip("manual-reload", false);
    }
    else if (m_VrHandsTwoHandedAimMountFriendly)
    {
        if (m_VrHandsTwoHandedGripActive)
        {
            if (leftGripJustPressed)
            {
                if (leftGripDown && !allowGameplayInputOnTwoHandedGripRelease)
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                releaseMountFriendlyGrip("grip", true);
                return false;
            }

            m_VrHandsTwoHandedMountFriendlyGripContact = true;
            return false;
        }

        const bool magazineExclusion = leftHandTouchesMagazineForGripExclusion();
        if (magazineExclusion)
        {
            clearMountFriendlyGripContact();
        }
        else
        {
            float stockDistance = FLT_MAX;
            const bool touchesStockRegion = leftHandTouchesMountFriendlyGripBox(stockDistance);
            if (touchesStockRegion)
            {
                if (!m_VrHandsTwoHandedMountFriendlyGripContact)
                {
                    m_VrHandsTwoHandedMountFriendlyGripContact = true;
                    m_VrHandsTwoHandedMountFriendlyGripEnteredAt = now;
                    triggerMagazineInteractionHaptic(0.008f, 55.0f, 0.10f, 1);
                }

                if (!m_VrHandsTwoHandedGripActive &&
                    m_VrHandsTwoHandedMountFriendlyGripEnteredAt.time_since_epoch().count() != 0)
                {
                    const float dwellSeconds = std::chrono::duration<float>(
                        now - m_VrHandsTwoHandedMountFriendlyGripEnteredAt).count();
                    if (dwellSeconds >= 0.5f)
                    {
                        m_VrHandsTwoHandedGripPistol = false;
                        m_VrHandsTwoHandedGripActive = true;
                        m_VrHandsTwoHandedGripWeaponId = activeWeaponIdInt;
                        m_VrHandsTwoHandedGripWeaponTag = reinterpret_cast<uintptr_t>(activeWeapon);
                        triggerMagazineInteractionHaptic(0.020f, 95.0f, 0.32f, 2);
                        if (m_VrHandsDebugLog)
                        {
                            Game::logMsg(
                                "[VR][Hands] mount-friendly two-hand grip auto enabled weaponId=%d stockDistance=%.2f dwell=%.2f",
                                activeWeaponIdInt,
                                stockDistance,
                                dwellSeconds);
                        }
                        return false;
                    }
                }
            }
            else
            {
                clearMountFriendlyGripContact();
            }
        }
    }
    else
    {
        clearMountFriendlyGripContact();

        if ((
                (!isSeparateInput && (m_VrHandsTwoHandedGripHeldMode ? !leftGripDown : leftGripJustPressed)) ||
                (isSeparateInput && (m_VrHandsTwoHandedGripHeldMode ? !leftSupportHandDown : leftSupportHandJustPressed))
            ) &&
            m_VrHandsTwoHandedGripActive)
        {
            m_VrHandsTwoHandedGripActive = false;
            m_VrHandsTwoHandedGripWeaponId = 0;
            if (leftGripDown && !allowGameplayInputOnTwoHandedGripRelease)
                m_MagazineInteractionSuppressLeftInputUntilRelease = true;
            triggerMagazineInteractionHaptic(0.018f, 85.0f, 0.28f, 2);
            if (m_VrHandsDebugLog)
            {
                Game::logMsg(
                    "[VR][Hands] two-hand grip toggled off weaponId=%d",
                    activeWeaponIdInt);
            }
            return false;
        }

        if ((
                (!isSeparateInput && leftGripJustPressed) ||
                (isSeparateInput && leftSupportHandJustPressed)
            ) &&
            (!IsMagazineInteractionManualActive() || activeWeaponUsesShotgunShells) &&
            !m_MagazineInteractionLeftHandHolding &&
            (!leftHandTouchesMagazineForGripExclusion() || isSeparateInput ||
             m_MagazineReleaseButtonRequired))
        {
            float twoHandTargetDistance = FLT_MAX;
            if (leftHandTouchesTwoHandedGripTarget(twoHandTargetDistance))
            {
                m_VrHandsTwoHandedGripPistol = !VrHandsLongWeapon(activeWeaponId);
                m_VrHandsTwoHandedGripActive = true;
                m_VrHandsTwoHandedGripWeaponId = activeWeaponIdInt;
                m_VrHandsTwoHandedGripWeaponTag = reinterpret_cast<uintptr_t>(activeWeapon);
                if (leftGripDown && !m_VrHandsTwoHandedGripHeldMode)
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                triggerMagazineInteractionHaptic(0.020f, 95.0f, 0.32f, 2);
                if (m_VrHandsDebugLog)
                {
                    Game::logMsg(
                        "[VR][Hands] two-hand grip toggled on weaponId=%d targetDistance=%.2f",
                        activeWeaponIdInt,
                        twoHandTargetDistance);
                }
                return false;
            }
        }
    }

    if (nativeAmmoFallback) return false; // support grip above remains available
    if (!m_MagazineInteractionEnabled || !activeWeaponUsesPhysicalReload)
    {
        CancelMagazineInteractionManual();
        return false;
    }

    auto readClientPrimaryAmmo = [&](int& ammoType, int& reserve, int& reserveOffset) -> bool
    {
        ammoType = -1;
        reserve = -1;
        reserveOffset = -1;
        if (!activeWeapon || !localPlayer)
            return false;

        auto readAmmoSlot = [&](int candidateAmmoType, int& outReserve, int& outReserveOffset) -> bool
        {
            if (candidateAmmoType < 0 || candidateAmmoType >= 32)
                return false;

            const int candidateReserveOffset =
                VR::kAmmoArrayOffset + candidateAmmoType * static_cast<int>(sizeof(int));
            int candidateReserve = -1;
            if (!MagazineInteractionTryReadValue(localPlayer, candidateReserveOffset, candidateReserve) ||
                !MagazineInteractionReserveValueLooksPlausible(candidateReserve))
            {
                return false;
            }

            outReserve = candidateReserve;
            outReserveOffset = candidateReserveOffset;
            return true;
        };

        int primaryAmmoType = -1;
        if (MagazineInteractionTryReadValue(activeWeapon, VR::kPrimaryAmmoTypeOffset, primaryAmmoType) &&
            primaryAmmoType >= 0 &&
            primaryAmmoType < 32 &&
            readAmmoSlot(primaryAmmoType, reserve, reserveOffset))
        {
            ammoType = primaryAmmoType;
            return true;
        }

        return false;
    };

    auto clearNativeClientReloadState = [&](const char* reason) -> bool
    {
        if (!activeWeapon)
            return false;
        if (authoritativePistol)
            return MagazineInteractionClearPistolReloadFlags(activeWeapon);
        if (m_MagazineInteractionShotgunShellMode ||
            MagazineInteractionWeaponUsesShotgunShells(activeWeaponId))
        {
            return ApplyMagazineInteractionShotgunClientReloadAbort(
                activeWeapon,
                static_cast<int>(activeWeaponId),
                reason ? reason : "native-reload-suppress");
        }
        return MagazineInteractionClearDetachableClientReloadState(activeWeapon);
    };

    auto suppressNativeReloadPlayback = [&](float seconds, const char* reason)
    {
        const auto until = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(std::max(0.05f, seconds)));
        if (m_MagazineInteractionNativeReloadSuppressUntil.time_since_epoch().count() == 0 ||
            until > m_MagazineInteractionNativeReloadSuppressUntil)
        {
            m_MagazineInteractionNativeReloadSuppressUntil = until;
        }
        m_MagazineInteractionNativeReloadSuppressWeaponId =
            m_MagazineInteractionWeaponId != 0
                ? m_MagazineInteractionWeaponId
                : static_cast<int>(activeWeaponId);
        clearNativeClientReloadState(reason);
        if (m_Game)
        {
            m_Game->ClientCmd_Unrestricted("-reload");
            m_Game->ClientCmd_Unrestricted("-attack");
        }
        m_ReloadCmdOwned = false;
    };

    if (nativeReloadSuppressWindowActiveForWeapon())
    {
        clearNativeClientReloadState("native-reload-suppress-window");
        if (m_Game)
        {
            m_Game->ClientCmd_Unrestricted("-reload");
        }
        m_ReloadCmdOwned = false;
    }

    auto quickReloadShouldAutoBolt = [&]() -> bool
    {
        return !authoritativePistol && m_MagazineInteractionQuickReloadMode &&
            !m_MagazineInteractionShotgunShellMode &&
            m_MagazineInteractionStartClip > 0 &&
            MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId);
    };

    auto suppressQuickReloadNativeTail = [&](const char* reason)
    {
        const auto sessionWeaponId =
            static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionWeaponId);
        const bool sessionWeaponMatchesActive =
            m_MagazineInteractionWeaponId != 0 &&
            m_MagazineInteractionWeaponId == static_cast<int>(activeWeaponId);
        const bool quickReloadWeaponMatches =
            MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId) ||
            MagazineInteractionWeaponUsesDetachableMagazine(sessionWeaponId) ||
            sessionWeaponMatchesActive;
        if (m_MagazineInteractionQuickReloadMode &&
            !m_MagazineInteractionShotgunShellMode &&
            quickReloadWeaponMatches)
        {
            suppressNativeReloadPlayback(1.15f, reason);
        }
    };

    auto suppressRegularPhysicalReloadNativeTail = [&](const char* reason)
    {
        const auto sessionWeaponId =
            static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionWeaponId);
        const bool sessionWeaponMatchesActive =
            m_MagazineInteractionWeaponId != 0 &&
            m_MagazineInteractionWeaponId == static_cast<int>(activeWeaponId);
        const bool detachableWeaponMatches =
            MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId) ||
            MagazineInteractionWeaponUsesDetachableMagazine(sessionWeaponId) ||
            sessionWeaponMatchesActive;
        if (!m_MagazineInteractionQuickReloadMode &&
            !m_MagazineInteractionShotgunShellMode &&
            detachableWeaponMatches)
        {
            suppressNativeReloadPlayback(0.85f, reason);
        }
    };

    auto completeQuickReloadAutoBoltFallback = [&](const char* reason, bool suppressLeftInputUntilRelease)
    {
        m_PhysicalReloadChambers.Complete(reinterpret_cast<uintptr_t>(activeWeapon));
        m_PhysicalReloadObservedEmpty = false;
        MagazineInteractionPlayBoltSound(this, false);
        MagazineInteractionPlayBoltSound(this, true);
        triggerMagazineInteractionWeaponHandHaptic(0.030f, 100.0f, 0.45f, 2);
        Game::logMsg(
            "[VR][MagazineInteraction] quick reload auto-bolt complete reason=%s clip=%d startClip=%d",
            reason ? reason : "unknown",
            activeClip,
            m_MagazineInteractionStartClip);
        suppressQuickReloadNativeTail(reason);
        CancelMagazineInteractionManual();
        if (suppressLeftInputUntilRelease)
            m_MagazineInteractionSuppressLeftInputUntilRelease = true;
    };

    auto beginQuickReloadAutoBoltAnimation = [&](const char* reason) -> bool
    {
        if (!quickReloadShouldAutoBolt())
            return false;

        MagazineInteractionBoxSnapshot boltBox{};
        if (!getFreshBoltBoxForActiveViewmodel(boltBox))
        {
            Game::logMsg(
                "[VR][MagazineInteraction] quick reload auto-bolt animation fallback: no fresh bolt/slide box weaponId=%d model=%s reason=%s",
                static_cast<int>(activeWeaponId),
                m_MagazineInteractionMagazineModelName.c_str(),
                reason ? reason : "unknown");
            return false;
        }

        const float requiredPull = std::max(0.0f, MagazineInteractionResolveBoltPullDistanceMeters(this)) * m_VRScale;
        if (!std::isfinite(requiredPull) || requiredPull <= l4d2vr_magazine::kMinimumBoltPullSourceUnits)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] quick reload auto-bolt animation fallback: invalid scaled pull distance weaponId=%d reason=%s",
                static_cast<int>(activeWeaponId),
                reason ? reason : "unknown");
            return false;
        }

        m_MagazineInteractionState = MagazineInteractionManualState::AutoBolting;
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionBoltRestBox = boltBox;
        m_MagazineInteractionBoltRestValid = true;
        m_MagazineInteractionBoltRestWorld = MagazineInteractionBuildBoxWorld(boltBox);
        m_MagazineInteractionBoltPullAxisWorld =
            MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltInputAxisWorld =
            MagazineInteractionBuildBoltInputAxisWorld(this, m_MagazineInteractionBoltPullAxisWorld);
        m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = {};
        m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
        m_MagazineInteractionBoltPullDistance = 0.0f;
        m_MagazineInteractionBoltMaxPullDistance = requiredPull;
        m_MagazineInteractionBoltGrabRequiresGripRelease = false;
        m_MagazineInteractionBoltReachedRear = false;
        m_MagazineInteractionBoltPullAxisSignLocked = true;
        m_MagazineInteractionBoltContactActive = false;
        m_MagazineInteractionBoltStageBeforeBackendReloadComplete = false;
        m_MagazineInteractionBoltCompletedBeforeBackendReload = false;
        m_MagazineInteractionBoltStageStarted = now;
        m_MagazineInteractionBoltGrabbedAt = {};
        setBoltPullDistance(0.0f);
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        Game::logMsg(
            "[VR][MagazineInteraction] quick reload auto-bolt animation started weaponId=%d pull=%.2f visualAxis=(%.2f %.2f %.2f) inputAxis=(%.2f %.2f %.2f) model=%s reason=%s",
            static_cast<int>(activeWeaponId),
            requiredPull,
            m_MagazineInteractionBoltPullAxisWorld.x,
            m_MagazineInteractionBoltPullAxisWorld.y,
            m_MagazineInteractionBoltPullAxisWorld.z,
            m_MagazineInteractionBoltInputAxisWorld.x,
            m_MagazineInteractionBoltInputAxisWorld.y,
            m_MagazineInteractionBoltInputAxisWorld.z,
            boltBox.modelName.c_str(),
            reason ? reason : "unknown");
        return true;
    };

    auto beginMagazineInteractionSession = [&](const MagazineInteractionBoxSnapshot& box)
    {
        m_MagazineInteractionPistolHandle = authoritativePistol ? activeWeaponHandle : 0u;
        m_MagazineInteractionReloadTriggered = false;
        m_MagazineInteractionReloadCommandPending = false;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil = {};
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        m_MagazineInteractionOldMagazinePulled = false;
        m_MagazineInteractionRetainedMagazineHeld = false;
        m_RetainedMagazineInsertGate.Reset();
        m_MagazineInteractionOldMagazineContactActive = false;
        m_MagazineInteractionFreshMagazineContactActive = false;
        m_MagazineInteractionBoltContactActive = false;
        m_MagazineInteractionShotgunShellMode = MagazineInteractionWeaponUsesShotgunShells(activeWeaponId);
        // Capture this before the interaction mutates the client/server clip.
        // The chamber state remains authoritative for the eventual bolt stage,
        // even when empty-clip auto-reload suppression is disabled.
        m_MagazineInteractionChamberEmpty = m_PhysicalReloadObservedEmpty || activeClip <= 0;
        m_MagazineInteractionShotgunShellsLoadedThisSession = 0;
        m_MagazineInteractionShotgunLastInterruptedClip = -1;
        m_MagazineInteractionShotgunStableCaptureValid = false;
        m_MagazineInteractionBoltStageBeforeBackendReloadComplete = false;
        m_MagazineInteractionBoltCompletedBeforeBackendReload = false;
        m_MagazineInteractionShotgunStableCaptureBox = {};
        m_MagazineInteractionShotgunStableCaptureModelLocal = {};
        m_MagazineInteractionWeapon = activeWeapon;
        m_MagazineInteractionWeaponId = static_cast<int>(activeWeaponId);
        m_MagazineInteractionWeaponEntityIndex =
            MagazineInteractionFindClientEntityIndex(m_Game, activeWeapon);
        m_MagazineInteractionStartClip = m_PhysicalReloadObservedEmpty &&
            !MagazineInteractionWeaponUsesShotgunShells(activeWeaponId) ? 0 : activeClip;
        m_MagazineInteractionMagazineBoneIndex = box.boneIndex;
        m_MagazineInteractionViewmodelEntityIndex = box.entityIndex;
        m_MagazineInteractionMagazineModelName = box.modelName;
        m_MagazineInteractionSocketBox = box;
        m_MagazineInteractionSocketValid = true;
        m_MagazineInteractionSocketWorld = MagazineInteractionBuildBoxWorld(box);
        rebuildSocketCaptureFromSocket();
        if (m_MagazineInteractionShotgunShellMode)
            captureShotgunStableSocketCapture("session-begin");
        setDetachedMagazineWorld(m_MagazineInteractionSocketWorld);
        m_MagazineInteractionFreshPickupBasisValid = false;
        m_MagazineInteractionFreshPickupForward = {};
        m_MagazineInteractionFreshPickupRight = {};
        m_MagazineInteractionGrabStartLeftControllerPosAbs = leftControllerPosRelativeToHmd();
        m_MagazineInteractionStarted = now;
        m_MagazineInteractionFreshGrabbedAt = {};
        m_MagazineInteractionPostInsertStarted = {};
        m_MagazineInteractionViewmodelFreezeDeferredUntil = {};
        m_MagazineInteractionSyntheticClipOutSample.clear();
        m_MagazineInteractionSyntheticClipOutStarted = {};
        m_MagazineInteractionSyntheticClipInSample.clear();
        m_MagazineInteractionSyntheticClipInStarted = {};
        m_MagazineInteractionEmptyFireSoundLastPlayed = {};
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);

        const Vector captureOffsetMeters = MagazineInteractionResolveSocketCaptureBoxLocalOffsetMeters(this);
        const Vector captureRotationDeg = MagazineInteractionResolveSocketCaptureBoxLocalRotationOffsetDeg(this);
        const Vector captureHalfExtentsMeters =
            (m_VRScale > 0.001f)
            ? ((m_MagazineInteractionSocketCaptureBox.maxs - m_MagazineInteractionSocketCaptureBox.mins) * (0.5f / m_VRScale))
            : Vector(0.0f, 0.0f, 0.0f);
        Vector captureHalfExtentsOverride{};
        Vector captureOffsetOverride{};
        Vector captureRotationOverride{};
        float captureAngleOverride = 0.0f;
        int captureHalfExtentsOverrideWeaponId = 0;
        int captureOffsetOverrideWeaponId = 0;
        int captureRotationOverrideWeaponId = 0;
        int captureAngleOverrideWeaponId = 0;
        MagazineInteractionFindWeaponOverride(
            this,
            m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides,
            captureHalfExtentsOverride,
            captureHalfExtentsOverrideWeaponId);
        MagazineInteractionFindWeaponOverride(
            this,
            m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides,
            captureOffsetOverride,
            captureOffsetOverrideWeaponId);
        MagazineInteractionFindWeaponOverride(
            this,
            m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides,
            captureRotationOverride,
            captureRotationOverrideWeaponId);
        MagazineInteractionFindWeaponOverride(
            this,
            m_MagazineInteractionSocketCaptureAngleDegOverrides,
            captureAngleOverride,
            captureAngleOverrideWeaponId);
        Game::logMsg(
            "[VR][MagazineInteraction] socket capture tuning weaponId=%d modelWeaponId=%d overrideWeaponIds=(half:%d offset:%d rot:%d angle:%d) offsetMeters=(%.3f %.3f %.3f) rotationDeg=(%.1f %.1f %.1f) halfExtentsMeters=(%.3f %.3f %.3f) angleDeg=%.1f model=%s",
            static_cast<int>(activeWeaponId),
            MagazineInteractionInferWeaponIdFromModelName(box.modelName),
            captureHalfExtentsOverrideWeaponId,
            captureOffsetOverrideWeaponId,
            captureRotationOverrideWeaponId,
            captureAngleOverrideWeaponId,
            captureOffsetMeters.x,
            captureOffsetMeters.y,
            captureOffsetMeters.z,
            captureRotationDeg.x,
            captureRotationDeg.y,
            captureRotationDeg.z,
            captureHalfExtentsMeters.x,
            captureHalfExtentsMeters.y,
            captureHalfExtentsMeters.z,
            MagazineInteractionResolveSocketCaptureAngleDeg(this),
            box.modelName.c_str());
    };

    auto getMagazineBoxForAutoEject = [&](MagazineInteractionBoxSnapshot& box, float& ageSeconds, bool& usedCachedBox) -> bool
    {
        ageSeconds = -1.0f;
        usedCachedBox = false;
        const float freshMaxAgeSeconds = std::max(0.02f, m_MagazineInteractionStaleSeconds);
        static std::chrono::steady_clock::time_point s_lastAutoEjectBoxLog{};
        auto logLimited = [&](const char* reason, int entityIndex, int boneIndex)
        {
            if (s_lastAutoEjectBoxLog.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(now - s_lastAutoEjectBoxLog).count() < 1.0f)
            {
                return;
            }
            s_lastAutoEjectBoxLog = now;
            Game::logMsg(
                "[VR][MagazineInteraction] empty clip auto-eject cannot start: %s age=%.3fs freshMax=%.3fs ent=%d bone=%d",
                reason ? reason : "unknown",
                ageSeconds,
                freshMaxAgeSeconds,
                entityIndex,
                boneIndex);
        };

        if (!GetMagazineInteractionBox(box))
        {
            logLimited("no published magazine box", -1, -1);
            return false;
        }

        ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        if (ageSeconds <= freshMaxAgeSeconds)
            return true;

        if (MagazineInteractionMatrixLooksRenderable(MagazineInteractionBuildBoxWorld(box)))
        {
            usedCachedBox = true;
            if (s_lastAutoEjectBoxLog.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - s_lastAutoEjectBoxLog).count() >= 1.0f)
            {
                s_lastAutoEjectBoxLog = now;
                Game::logMsg(
                    "[VR][MagazineInteraction] empty clip auto-eject using last known magazine box age=%.3fs freshMax=%.3fs ent=%d bone=%d model=%s",
                    ageSeconds,
                    freshMaxAgeSeconds,
                    box.entityIndex,
                    box.boneIndex,
                    box.modelName.c_str());
            }
            return true;
        }

        logLimited("last known magazine box invalid", box.entityIndex, box.boneIndex);
        return false;
    };

    if (IsMagazineInteractionManualActive() && activeWeapon != m_MagazineInteractionWeapon)
    {
        Game::logMsg("[VR][MagazineInteraction] active weapon changed; canceling manual magazine interaction");
        CancelMagazineInteractionManual();
        return false;
    }

    auto beginRemoteMagazineAction = [&](l4d2vr_remote_mag::Action action) -> bool
    {
        int ammoType = -1, reserve = -1, reserveOffset = -1;
        if (authoritativePistol)
        {
            if (!BeginPistolMagazineRequest(m_CurrentPredictedHitFeedbackCmdNumber, pistolHand,
                static_cast<l4d2vr_pistol::MagazineAction>(action))) return false;
        }
        else if (!readClientPrimaryAmmo(ammoType, reserve, reserveOffset) ||
            !BeginRemoteMagazineRequest(activeWeaponHandle, activeWeaponIdInt, activeClip, reserve, action,
                reinterpret_cast<uintptr_t>(localPlayer),
                m_MagazineInteractionSessionGeneration.load(std::memory_order_acquire))) return false;
        m_MagazineInteractionTransactionGripHeld =
            m_MagazineInteractionLeftHandHolding || m_MagazineInteractionRetainedMagazineHeld;
        m_MagazineInteractionState = MagazineInteractionManualState::WaitingForServerMagazine;
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        m_MagazineInteractionFreshMagazineContactActive = false;
        m_MagazineInteractionReloadTriggered = false;
        m_MagazineInteractionReloadCommandPending = false;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil = {};
        suppressNativeReloadPlayback(0.35f, "remote-magazine-pending");
        return true;
    };

    auto catchRetainedMagazine = [&]() -> bool
    {
        if (!leftGripDown || m_MagazineInteractionShotgunShellMode || m_MouseModeEnabled || m_SuppressPlayerInput ||
            !m_FirstPersonControlReady.load(std::memory_order_acquire) || m_Game->m_EngineClient->IsPaused() ||
            (m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible())) return false;
        const bool tracked = HasFreshMagazineInteractionTracking();
        refreshSocketFromPublishedViewmodelBox();
        rebuildInputSocketFromSessionBox();
        const auto controller = MagazineInteractionBuildViewmodelReprojectedControllerWorld(this,
            supportPosition, supportForward, supportRight, supportUp);
        Vector palm(0.0f, 0.0f, 0.0f);
        bool wrist = false;
        if (!MagazineInteractionResolveFreshMagazineHandAnchorWorld(this, controller, palm, wrist)) return false;
        const auto& socket = inputSocketValid ? inputSocketBox : m_MagazineInteractionSocketBox;
        const float distance = MagazineInteractionDistanceToBoxSourceUnits(socket, palm);
        const float padding = std::clamp(m_MagazineInteractionGrabPaddingMeters, 0.0f, 0.05f) * m_VRScale;
        if (!l4d2vr_magazine::MayCatchEjectedMagazine(leftGripDown, tracked, distance, padding)) return false;
        Vector gripPoint(0.0f, 0.0f, 0.0f), center(0.0f, 0.0f, 0.0f);
        if (!snapFreshMagazineCenterOffsetToLeftHandAnchor(controller, wrist, palm, gripPoint, center)) return false;
        m_MagazineInteractionRetainedMagazineHeld = true;
        m_RetainedMagazineCatchHandLocal = leftControllerPosRelativeToWeapon();
        m_RetainedMagazineInsertGate.Reset();
        m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
        m_MagazineInteractionLeftHandHolding = true;
        m_MagazineInteractionFreshGrabbedAt = now;
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
        setDetachedMagazineWorld(buildFreshHeldMagazineWorldFromLeftHand());
        Game::logMsg("[VR][RetainedMagazine] caught weaponId=%d distance=%.2f", activeWeaponIdInt, distance);
        return true;
    };

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerPistolCycle)
    {
        l4d2vr_pistol_reload::Reply reply{};
        if (!authoritativePistol) { CancelMagazineInteractionManual(); return false; }
        if (!ConsumePistolMagazineReply(reply))
        {
            if (!PistolMagazineRequestPending()) CancelMagazineInteractionManual();
            return false;
        }
        const auto outcome = l4d2vr_pistol_reload::SettleGesture(reply, pistolHand);
        if (outcome == l4d2vr_pistol_reload::GestureOutcome::Cycled)
            completeBoltStage("pistol-cycle-confirmed", m_PistolMagazineCycleSuppressInput, true);
        else if (outcome == l4d2vr_pistol_reload::GestureOutcome::RetryCycle)
        {
            if (activeClip <= 0 || (havePistolMagazineState && (pistolMagazineState.chambered & pistolBit) != 0u))
                CancelMagazineInteractionManual();
            else
            {
                m_MagazineInteractionChamberEmpty = true;
                if (!beginBoltStage(activeWeaponId, "pistol-cycle-rejected"))
                    m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
            }
        }
        else CancelMagazineInteractionManual();
        return false;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerMagazine)
    {
        const bool magazineInputAvailable = IsInputActionFrameAvailable() && IsMagazineGripInputAvailable();
        m_MagazineInteractionTransactionGripHeld = l4d2vr_shell::ShellRemainsHeld(
            m_MagazineInteractionTransactionGripHeld, leftGripDown,
            m_MagazineInteractionQuickReloadMode && !m_MagazineInteractionRetainedMagazineHeld,
            magazineInputAvailable);
        clearNativeClientReloadState("remote-magazine-pending");
        l4d2vr_remote_mag::Reply reply{};
        int ammoType = -1, replicatedReserve = -1, reserveOffset = -1;
        auto poll = l4d2vr_remote_mag::ClientRequest::Poll::None;
        if (authoritativePistol)
        {
            l4d2vr_pistol_reload::Reply pistolReply{};
            if (!ConsumePistolMagazineReply(pistolReply))
            {
                if (!PistolMagazineRequestPending()) CancelMagazineInteractionManual();
                return false;
            }
            const auto outcome = l4d2vr_pistol_reload::SettleGesture(pistolReply, pistolHand);
            if (outcome == l4d2vr_pistol_reload::GestureOutcome::Cancel ||
                outcome == l4d2vr_pistol_reload::GestureOutcome::Cycled ||
                outcome == l4d2vr_pistol_reload::GestureOutcome::RetryCycle)
            { CancelMagazineInteractionManual(); return false; }
            poll = l4d2vr_remote_mag::ClientRequest::Poll::Result;
            reply.status = pistolReply.status;
            reply.request.action = static_cast<l4d2vr_remote_mag::Action>(pistolReply.request.action);
            reply.clip = pistolReply.result.clip;
            reply.reserve = pistolReply.result.reserve;
            if (pistolReply.status == l4d2vr_pistol_reload::Status::Applied)
            {
                m_MagazineInteractionOneInChamber =
                    (pistolReply.result.state.chambered & pistolBit) != 0u && activeClip > 0;
                m_MagazineInteractionChamberEmpty = !m_MagazineInteractionOneInChamber;
            }
        }
        else
        {
            readClientPrimaryAmmo(ammoType, replicatedReserve, reserveOffset);
            poll = PollRemoteMagazineReply(activeWeaponHandle, activeWeaponIdInt,
                reinterpret_cast<uintptr_t>(localPlayer),
                m_MagazineInteractionSessionGeneration.load(std::memory_order_acquire), activeClip, replicatedReserve, reply);
        }
        if (poll == l4d2vr_remote_mag::ClientRequest::Poll::Waiting) return false;
        m_MagazineInteractionFreshPickupBasisValid = false;
        if (poll != l4d2vr_remote_mag::ClientRequest::Poll::Result || reply.status != l4d2vr_remote_mag::Status::Applied)
        {
            Game::logMsg("[VR][RemoteMagazine][client] action ended poll=%u status=%u",
                static_cast<unsigned>(poll), static_cast<unsigned>(reply.status));
            // Restore native reload after a timeout/backend failure. A rejected
            // insertion leaves the magazine out and lets the player try again.
            if ((!authoritativePistol && !RemoteMagazineProtocolSupported()) || poll != l4d2vr_remote_mag::ClientRequest::Poll::Result ||
                reply.request.action == l4d2vr_remote_mag::Action::Eject) CancelMagazineInteractionManual();
            else if (l4d2vr_remote_mag::RetainsRejectedHeldMagazine(reply.status, reply.request.action,
                m_MagazineInteractionTransactionGripHeld, magazineInputAvailable, leftGripDown,
                m_MagazineInteractionRetainedMagazineHeld, authoritativePistol))
            {
                m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
                m_MagazineInteractionLeftHandHolding = true;
                m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
                if (magazineInputAvailable)
                    m_RetainedMagazineCatchHandLocal = leftControllerPosRelativeToWeapon();
                m_RetainedMagazineInsertGate.Reset();
            }
            else
            {
                m_MagazineInteractionRetainedMagazineHeld = false;
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            }
            return false;
        }
        m_MagazineInteractionStartClip = reply.clip;
        m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
        if (reply.request.action == l4d2vr_remote_mag::Action::Eject)
        {
            m_MagazineInteractionOldMagazinePulled = true;
            if (!authoritativePistol)
            {
                m_MagazineInteractionOneInChamber = l4d2vr_magazine::ChamberReadyAfterEject(
                    m_PhysicalReloadObservedEmpty || m_MagazineInteractionChamberEmpty, reply.clip);
                m_MagazineInteractionChamberEmpty = !m_MagazineInteractionOneInChamber;
            }
            MagazineInteractionPlayClipOutSound(this);
            triggerMagazineInteractionBothHandsHaptic(0.026f, 95.0f, 0.42f, 2);
            catchRetainedMagazine();
        }
        else
        {
            m_MagazineInteractionRetainedMagazineHeld = false;
            MagazineInteractionPlayClipInSound(this);
            triggerMagazineInteractionBothHandsHaptic(0.026f, 105.0f, 0.42f, 2);
            if (quickReloadShouldAutoBolt())
            {
                if (!beginQuickReloadAutoBoltAnimation("remote-magazine-confirmed"))
                    completeQuickReloadAutoBoltFallback("remote-magazine-confirmed", leftGripDown);
            }
            else if (!beginBoltStage(activeWeaponId, "remote-magazine-confirmed"))
            {
                // A loaded chamber needs no slide stroke. An empty chamber
                // must remain blocked if its slide region is unavailable.
                if (!m_MagazineInteractionChamberEmpty) CancelMagazineInteractionManual();
                else m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
            }
        }
        return false;
    }

    if (authoritativePistol && havePistolMagazineState && !IsMagazineInteractionManualActive() &&
        (pistolMagazineState.physical & pistolBit) != 0u &&
        ((pistolMagazineState.attached & pistolBit) == 0u || (m_PhysicalReloadObservedEmpty && activeClip > 0)))
    {
        // Restore the host's magazine/chamber state after a weapon or player
        // lifecycle boundary. Do not eject a magazine that is already out.
        MagazineInteractionBoxSnapshot box{};
        float age = -1.0f; bool cached = false;
        if (!getMagazineBoxForAutoEject(box, age, cached)) return false;
        beginMagazineInteractionSession(box);
        m_MagazineInteractionOneInChamber = !m_PhysicalReloadObservedEmpty;
        m_MagazineInteractionChamberEmpty = m_PhysicalReloadObservedEmpty;
        m_MagazineInteractionOldMagazinePulled = (pistolMagazineState.attached & pistolBit) == 0u;
        m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
        if (!m_MagazineInteractionOldMagazinePulled && activeClip > 0)
        {
            if (!beginBoltStage(activeWeaponId, "pistol-resume-empty-chamber"))
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
        }
    }

    const bool retainedMagazineChamberEmpty = l4d2vr_shell::ValidHandle(activeWeaponHandle) &&
        m_PhysicalReloadChambers.HasEmptyChamber(reinterpret_cast<uintptr_t>(localPlayer),
            reinterpret_cast<uintptr_t>(activeWeapon), activeWeaponHandle, activeWeaponIdInt);
    if (l4d2vr_magazine::ShouldResumeMagazineCycle(authoritativeMagazine,
            IsMagazineInteractionManualActive(), retainedMagazineChamberEmpty, activeClip))
    {
        MagazineInteractionBoxSnapshot box{};
        if (!GetMagazineInteractionBox(box) || !boxFreshForActiveWeapon(box)) return false;
        beginMagazineInteractionSession(box);
        m_MagazineInteractionOneInChamber = false;
        m_MagazineInteractionChamberEmpty = true;
        if (!beginBoltStage(activeWeaponId, "magazine-resume-empty-chamber"))
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
    }

    if (activeAuthoritativeMagazineBackend && m_MagazineInteractionOldMagazinePulled && activeClip == 0)
    {
        m_MagazineInteractionOneInChamber = false;
        m_MagazineInteractionChamberEmpty = true;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForServerShell)
    {
        ApplyMagazineInteractionShotgunClientReloadAbort(activeWeapon, activeWeaponIdInt, "remote-shell-pending");
        const bool shellInputAvailable = IsInputActionFrameAvailable() && IsMagazineGripInputAvailable();
        const bool shellHeld = l4d2vr_shell::ShellRemainsHeld(m_MagazineInteractionLeftHandHolding,
            leftGripDown, m_MagazineInteractionQuickReloadMode, shellInputAvailable);
        m_MagazineInteractionLeftHandHolding = shellHeld;
        m_MagazineInteractionLeftHandPoseActive.store(shellHeld ? 1u : 0u,std::memory_order_relaxed);
        if (shellHeld && shellInputAvailable) updateFreshDetachedMagazineFromLeftHand();
        l4d2vr_shell::Reply reply{};
        int ammoType = -1, replicatedReserve = -1;
        if (MagazineInteractionTryReadValue(activeWeapon, kPrimaryAmmoTypeOffset, ammoType) && ammoType >= 0 && ammoType < 32)
            MagazineInteractionTryReadValue(localPlayer, kAmmoArrayOffset + ammoType * static_cast<int>(sizeof(int)), replicatedReserve);
        const auto poll = PollRemoteShellReply(activeWeaponHandle, activeWeaponIdInt,
            reinterpret_cast<uintptr_t>(localPlayer),
            m_MagazineInteractionSessionGeneration.load(std::memory_order_acquire), activeClip, replicatedReserve, reply);
        if (poll == l4d2vr_shell::ClientRequest::Poll::Waiting) return false;
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
        if (poll != l4d2vr_shell::ClientRequest::Poll::Result || reply.status != l4d2vr_shell::Status::Applied)
        {
            if (shellHeld && poll == l4d2vr_shell::ClientRequest::Poll::Result && l4d2vr_shell::RetainRejectedShell(reply.status))
            {
                m_ShellRetryContact.Reject(reply.status);
                m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
                m_MagazineInteractionLeftHandHolding = true;
                m_MagazineInteractionLeftHandPoseActive.store(1u,std::memory_order_relaxed);
                if (shellInputAvailable) updateFreshDetachedMagazineFromLeftHand();
            }
            else
            {
                m_ShellRetryContact.Reset();
                m_MagazineInteractionFreshPickupBasisValid = false;
            }
            Game::logMsg("[VR][RemoteShell][client] insertion ended poll=%u status=%u",
                static_cast<unsigned>(poll), static_cast<unsigned>(reply.status));
            return false;
        }
        m_ShellRetryContact.Reset();
        m_MagazineInteractionFreshPickupBasisValid = false;
        // Both the acknowledgement and Source's replicated ammo must arrive.
        // Remote shell loading never writes predicted client ammunition.
        ++m_MagazineInteractionShotgunShellsLoadedThisSession;
        m_MagazineInteractionShotgunLastInterruptedClip = activeClip;
        m_MagazineInteractionStartClip = activeClip;
        m_MagazineInteractionReloadTriggered = false;
        ApplyMagazineInteractionShotgunClientReloadAbort(activeWeapon, activeWeaponIdInt, "remote-shell-confirmed");
        triggerMagazineInteractionBothHandsHaptic(0.026f, 105.0f, 0.42f, 2);
        MagazineInteractionPlayClipInSound(this);
        Game::logMsg("[VR][RemoteShell][client] confirmed seq=%u clip=%d reserve=%d", reply.request.sequence, reply.clip, reply.reserve);
        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, m_MagazineInteractionMagazineModelName);
        if (maxClip > 0 && activeClip >= maxClip && !beginBoltStage(activeWeaponId, "remote-shell-full"))
            CancelMagazineInteractionManual();
        return false;
    }

    // Poll submitted transactions above even during tracking loss. New physical
    // movement below must never use retained controller poses as live input.
    if (!nativeAmmoFallback && m_MagazineInteractionEnabled && activeWeaponUsesPhysicalReload &&
        !HasFreshMagazineInteractionTracking())
    {
        if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt)
        {
            // Reacquisition can jump the controller pose. Require a new grip
            // origin instead of counting that jump as a mechanical stroke.
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionBoltGrabRequiresGripRelease = true;
            m_MagazineInteractionBoltContactActive = false;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        }
        return false;
    }

    if (IsMagazineInteractionManualActive() &&
        m_MagazineInteractionShotgunShellMode &&
        !activeShotgunManualReloadAvailable)
    {
        CancelMagazineInteractionManual();
        return false;
    }

    if (!IsMagazineInteractionManualActive() &&
        activeShotgunManualReloadAvailable)
    {
        MagazineInteractionBoxSnapshot box{};
        if (GetMagazineInteractionBox(box))
        {
            const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
            if (ageSeconds <= std::max(0.02f, m_MagazineInteractionStaleSeconds) &&
                MagazineInteractionPublishedBoxMatchesWeapon(box, activeWeaponId))
            {
                beginMagazineInteractionSession(box);
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionFreshPickupBasisValid = false;
            }
        }
    }

    if (activePumpNeedsCycle &&
        m_MagazineInteractionWeapon == activeWeapon &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt))
    {
        if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine)
        {
            m_MagazineInteractionChamberEmpty = true;
            m_MagazineInteractionOneInChamber = false;
            if (l4d2vr_magazine::PumpShouldStartAfterShot(activePumpNeedsCycle, activeClip,
                    m_MagazineInteractionShotgunShellsLoadedThisSession) &&
                beginBoltStage(activeWeaponId, "spent-shell-needs-manual-pump"))
            {
                m_MagazineInteractionBoltGrabRequiresGripRelease = false;
                if (m_VrHandsTwoHandedGripActive && leftGripDown)
                    beginBoltHoldFromCurrentLeftHand(0.0f,
                        MagazineInteractionResolveBoltGrabPaddingMeters(this) * m_VRScale);
            }
        }
        // Only block once a valid interactive pump pose is available. A model
        // lacking the required bone must not leave the player permanently stuck.
        m_ManualPumpBlockingFire.store(m_MagazineInteractionBoltRestValid, std::memory_order_release);
    }

    const bool shotgunWaitingForPhysicalShell =
        m_MagazineInteractionShotgunShellMode &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine);
    const bool suppressEmptyShotgunAutoReload =
        shotgunWaitingForPhysicalShell &&
        m_MagazineInteractionSuppressEmptyClipAutoReload &&
        activeClip == 0;
    if (suppressEmptyShotgunAutoReload)
        m_MagazineInteractionChamberEmpty = true;

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
    {
        refreshSocketFromPublishedViewmodelBox();
    }
    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
    {
        rebuildInputSocketFromSessionBox();
    }

    if (shotgunWaitingForPhysicalShell &&
        (m_MagazineInteractionShotgunShellsLoadedThisSession > 0 ||
            suppressEmptyShotgunAutoReload))
    {
        ApplyMagazineInteractionShotgunClientReloadAbort(
            activeWeapon,
            static_cast<int>(activeWeaponId),
            suppressEmptyShotgunAutoReload
                ? "shotgun-empty-clip-auto-reload"
                : "shotgun-between-shells");
        if (suppressEmptyShotgunAutoReload)
            stopNativeShellReloadPrediction("shotgun-empty-clip-auto-reload");
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload)
    {
        const float elapsed = std::chrono::duration<float>(now - m_MagazineInteractionPostInsertStarted).count();
        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, m_MagazineInteractionMagazineModelName);
        const bool clipUpdated =
            activeClip > m_MagazineInteractionStartClip ||
            (maxClip > 0 && activeClip >= maxClip && activeClip != m_MagazineInteractionStartClip);
        if ((elapsed >= 0.20f && clipUpdated) || elapsed >= 3.0f)
        {
            // An early physical cycle can finish while Source still reports an
            // empty predicted clip. Clear that temporary observation once the
            // backend supplies ammo, including the full-tube completion path.
            if (m_MagazineInteractionBoltCompletedBeforeBackendReload && activeClip > 0)
            {
                m_PhysicalReloadChambers.Complete(reinterpret_cast<uintptr_t>(activeWeapon));
                m_PhysicalReloadObservedEmpty = false;
            }
            if (m_MagazineInteractionShotgunShellMode)
            {
                m_MagazineInteractionReloadTriggered = false;
                m_MagazineInteractionReloadCommandPending = false;
                m_MagazineInteractionReloadCommandIssued = false;
                m_MagazineInteractionReloadCommandHoldUntil = {};

                if (clipUpdated)
                {
                    ++m_MagazineInteractionShotgunShellsLoadedThisSession;
                    m_MagazineInteractionShotgunLastInterruptedClip = activeClip;
                    ApplyMagazineInteractionShotgunClientReloadAbort(
                        activeWeapon,
                        static_cast<int>(activeWeaponId),
                        "shotgun-shell-loaded");
                    if (!(maxClip > 0 && activeClip >= maxClip))
                        stopNativeShellReloadPrediction("shotgun-shell-loaded");
                }

                if (maxClip > 0 && activeClip >= maxClip)
                {
                    if (!beginBoltStage(activeWeaponId, clipUpdated ? "shotgun-full" : "shotgun-timeout-full"))
                    {
                        const bool suppressLeftInputUntilRelease = leftGripDown;
                        CancelMagazineInteractionManual();
                        if (suppressLeftInputUntilRelease)
                            m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    }
                    return false;
                }

                if (m_MagazineInteractionBoltCompletedBeforeBackendReload)
                {
                    Game::logMsg(
                        "[VR][MagazineInteraction] shotgun backend reload complete after early physical bolt elapsed=%.3fs clip=%d startClip=%d max=%d updated=%d",
                        elapsed,
                        activeClip,
                        m_MagazineInteractionStartClip,
                        maxClip,
                        clipUpdated ? 1 : 0);
                    const bool suppressLeftInputUntilRelease = leftGripDown;
                    CancelMagazineInteractionManual();
                    if (suppressLeftInputUntilRelease)
                        m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    return false;
                }

                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionFreshPickupBasisValid = false;
                m_MagazineInteractionPostInsertStarted = {};
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                if (leftGripDown)
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                return false;
            }

            if (m_MagazineInteractionBoltCompletedBeforeBackendReload)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] backend reload complete after early physical bolt elapsed=%.3fs clip=%d startClip=%d max=%d updated=%d",
                    elapsed,
                    activeClip,
                    m_MagazineInteractionStartClip,
                    maxClip,
                    clipUpdated ? 1 : 0);
                const bool suppressLeftInputUntilRelease = leftGripDown;
                suppressQuickReloadNativeTail("quick-reload-early-bolt-backend-complete");
                suppressRegularPhysicalReloadNativeTail("regular-reload-early-bolt-backend-complete");
                CancelMagazineInteractionManual();
                if (suppressLeftInputUntilRelease)
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                return false;
            }

            if (quickReloadShouldAutoBolt())
            {
                const char* autoBoltReason = clipUpdated ? "quick-reload-clip-updated" : "quick-reload-backend-timeout";
                if (!beginQuickReloadAutoBoltAnimation(autoBoltReason))
                    completeQuickReloadAutoBoltFallback(autoBoltReason, leftGripDown);
                return false;
            }

            Game::logMsg(
                "[VR][MagazineInteraction] native reload animation skipped; backend reload wait complete elapsed=%.3fs clip=%d startClip=%d max=%d updated=%d; entering bolt stage",
                elapsed,
                activeClip,
                m_MagazineInteractionStartClip,
                maxClip,
                clipUpdated ? 1 : 0);

            suppressRegularPhysicalReloadNativeTail(
                clipUpdated ? "regular-reload-clip-updated" : "regular-reload-backend-timeout");

            if (!beginBoltStage(activeWeaponId, clipUpdated ? "clip-updated" : "backend-timeout"))
            {
                const bool suppressLeftInputUntilRelease = leftGripDown;
                suppressQuickReloadNativeTail(clipUpdated ? "quick-reload-no-bolt-clip-updated" : "quick-reload-no-bolt-backend-timeout");
                CancelMagazineInteractionManual();
                if (suppressLeftInputUntilRelease)
                {
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    Game::logMsg("[VR][MagazineInteraction] backend reload complete while interaction gesture is still held; suppressing normal reload until release");
                }
            }
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting)
    {
        refreshBoltFromPublishedViewmodelBox();

        if (!m_MagazineInteractionBoltRestValid ||
            !MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltRestWorld))
        {
            Game::logMsg("[VR][MagazineInteraction] quick reload auto-bolt lost rest pose; completing without visible animation");
            completeQuickReloadAutoBoltFallback("quick-reload-auto-bolt-lost-rest-pose", leftGripDown);
            return false;
        }

        constexpr float kQuickReloadAutoBoltDurationSeconds = 0.38f;
        constexpr float kQuickReloadAutoBoltRearFraction = 0.45f;
        const float elapsed = (m_MagazineInteractionBoltStageStarted.time_since_epoch().count() != 0)
            ? std::chrono::duration<float>(now - m_MagazineInteractionBoltStageStarted).count()
            : 0.0f;
        const float normalized = std::clamp(
            elapsed / std::max(0.05f, kQuickReloadAutoBoltDurationSeconds),
            0.0f,
            1.0f);
        const float requiredPull = std::max(0.001f, m_MagazineInteractionBoltMaxPullDistance);
        float pullDistance = 0.0f;
        if (normalized < kQuickReloadAutoBoltRearFraction)
        {
            pullDistance = requiredPull * (normalized / kQuickReloadAutoBoltRearFraction);
        }
        else
        {
            if (!m_MagazineInteractionBoltReachedRear)
            {
                m_MagazineInteractionBoltReachedRear = true;
                MagazineInteractionPlayBoltSound(this, false);
                triggerMagazineInteractionWeaponHandHaptic(0.024f, 110.0f, 0.38f, 2);
                Game::logMsg(
                    "[VR][MagazineInteraction] quick reload auto-bolt rear threshold reached pull=%.2f elapsed=%.3fs",
                    requiredPull,
                    elapsed);
            }

            const float returnProgress = (normalized - kQuickReloadAutoBoltRearFraction) /
                std::max(0.001f, 1.0f - kQuickReloadAutoBoltRearFraction);
            pullDistance = requiredPull * (1.0f - std::clamp(returnProgress, 0.0f, 1.0f));
        }

        setBoltPullDistance(pullDistance);
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);

        if (normalized >= 1.0f)
        {
            m_PhysicalReloadChambers.Complete(reinterpret_cast<uintptr_t>(activeWeapon));
            m_PhysicalReloadObservedEmpty = false;
            setBoltPullDistance(0.0f);
            MagazineInteractionPlayBoltSound(this, true);
            triggerMagazineInteractionWeaponHandHaptic(0.030f, 100.0f, 0.45f, 2);
            Game::logMsg(
                "[VR][MagazineInteraction] quick reload auto-bolt animation complete elapsed=%.3fs clip=%d startClip=%d",
                elapsed,
                activeClip,
                m_MagazineInteractionStartClip);
            const bool suppressLeftInputUntilRelease = leftGripDown;
            suppressQuickReloadNativeTail("quick-reload-auto-bolt-complete");
            CancelMagazineInteractionManual();
            if (suppressLeftInputUntilRelease)
                m_MagazineInteractionSuppressLeftInputUntilRelease = true;
        }
        return false;
    }

    if (m_MagazineInteractionShotgunShellMode &&
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine &&
        m_MagazineInteractionShotgunShellsLoadedThisSession > 0 &&
        m_MagazineInteractionShotgunLastInterruptedClip >= 0 &&
        activeClip > m_MagazineInteractionShotgunLastInterruptedClip)
    {
        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, m_MagazineInteractionMagazineModelName);
        m_MagazineInteractionShotgunLastInterruptedClip = activeClip;
        ApplyMagazineInteractionShotgunClientReloadAbort(
            activeWeapon,
            static_cast<int>(activeWeaponId),
            "shotgun-stray-client-reload");
        stopNativeShellReloadPrediction("regular-release-did-not-stop-shell-reload");
        if (maxClip > 0 && activeClip >= maxClip)
        {
            if (!beginBoltStage(activeWeaponId, "shotgun-full-after-interrupt-fallback"))
                CancelMagazineInteractionManual();
            return false;
        }
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab)
    {
        if (activeAuthoritativeMagazineBackend && m_MagazineInteractionChamberEmpty &&
            !m_MagazineInteractionBoltRestValid)
        {
            // A late render snapshot must not auto-complete an empty reload.
            // Retry acquisition while leaving firing blocked until a real cycle.
            if (m_MagazineInteractionBoltStageStarted.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - m_MagazineInteractionBoltStageStarted).count() >= 1.0f)
            {
                beginBoltStage(activeWeaponId, "remote-magazine-slide-region-retry");
                m_MagazineInteractionBoltStageStarted = now;
            }
            if (!m_MagazineInteractionBoltRestValid) return false;
        }
        refreshBoltFromPublishedViewmodelBox();
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        const bool manualPump = m_ManualPumpEnabled && MagazineInteractionWeaponUsesSinglePumpSound(activeWeaponId);
        if (!manualPump)
            setBoltPullDistance(0.0f);

        if (m_MagazineInteractionBoltGrabRequiresGripRelease)
        {
            if (leftGripDown)
            {
                return m_MagazineInteractionBoltStageBeforeBackendReloadComplete
                    ? reloadCommandPending()
                    : false;
            }

            m_MagazineInteractionBoltGrabRequiresGripRelease = false;
            Game::logMsg("[VR][MagazineInteraction] interaction gesture released after magazine insertion; bolt grab enabled");
        }

        if (!m_MagazineInteractionBoltRestValid)
        {
            m_MagazineInteractionBoltContactActive = false;
            const float elapsed = std::chrono::duration<float>(now - m_MagazineInteractionBoltStageStarted).count();
            if (elapsed >= 2.0f)
            {
                Game::logMsg("[VR][MagazineInteraction] bolt stage lost rest pose; completing reload without physical bolt after %.3fs", elapsed);
                const bool suppressLeftInputUntilRelease = leftGripDown;
                CancelMagazineInteractionManual();
                if (suppressLeftInputUntilRelease)
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
            }
            return false;
        }

        MagazineInteractionBoxSnapshot grabBox = m_MagazineInteractionBoltRestBox;
        if (manualPump && MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltWorld))
        {
            // A released fore-end stays where it was pulled. Re-grab its visible
            // position, rather than the closed rest position nine centimetres away.
            grabBox.origin = MagazineInteractionMatrixOrigin(m_MagazineInteractionBoltWorld);
            grabBox.axisX = MagazineInteractionMatrixAxis(m_MagazineInteractionBoltWorld, 0);
            grabBox.axisY = MagazineInteractionMatrixAxis(m_MagazineInteractionBoltWorld, 1);
            grabBox.axisZ = MagazineInteractionMatrixAxis(m_MagazineInteractionBoltWorld, 2);
        }
        const float grabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
            grabBox,
            supportPosition,
            supportAngles,
            m_VRScale,
            this);
        const float grabRange = std::max(0.0f, MagazineInteractionResolveBoltGrabPaddingMeters(this)) * m_VRScale;
        const bool touchingBolt = grabDistance <= grabRange;
        updateMagazineInteractionContactHaptic(
            m_MagazineInteractionBoltContactActive,
            touchingBolt,
            0.012f,
            150.0f,
            0.22f);

        if (leftGripDown)
        {
            if (touchingBolt)
            {
                beginBoltHoldFromCurrentLeftHand(grabDistance, grabRange);
            }
            else if (leftGripJustPressed)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] bolt grab ignored; left hand outside bolt box distance=%.2f range=%.2f ent=%d bone=%d model=%s",
                    grabDistance,
                    grabRange,
                    m_MagazineInteractionBoltRestBox.entityIndex,
                    m_MagazineInteractionBoltRestBox.boneIndex,
                    m_MagazineInteractionBoltRestBox.modelName.c_str());
            }
        }
        return m_MagazineInteractionBoltStageBeforeBackendReloadComplete
            ? reloadCommandPending()
            : false;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt)
    {
        if (!m_MagazineInteractionBoltRestValid ||
            !MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltRestWorld))
        {
            Game::logMsg("[VR][MagazineInteraction] bolt hold lost rest pose; canceling bolt hold");
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionBoltGrabRequiresGripRelease =
                leftGripDown && !m_MagazineInteractionShotgunShellMode;
            m_MagazineInteractionBoltContactActive = false;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            return m_MagazineInteractionBoltStageBeforeBackendReloadComplete
                ? reloadCommandPending()
                : false;
        }

        const float requiredPull = std::max(0.001f, m_MagazineInteractionBoltMaxPullDistance);
        const float returnDistance = std::clamp(
            MagazineInteractionResolveBoltReturnDistanceMeters(this) * m_VRScale,
            0.0f,
            requiredPull);

        if (!leftGripDown)
        {
            if (m_ManualPumpEnabled && MagazineInteractionWeaponUsesSinglePumpSound(activeWeaponId))
            {
                // A pump has no return spring. Keep its position and rear latch
                // so the next grip can finish the forward stroke.
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionBoltGrabRequiresGripRelease = false;
                m_MagazineInteractionBoltContactActive = false;
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                return false;
            }
            if (m_MagazineInteractionBoltReachedRear)
            {
                completeBoltStage("released-after-rear", false);
            }
            else
            {
                setBoltPullDistance(0.0f);
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionBoltGrabRequiresGripRelease = false;
                m_MagazineInteractionBoltGrabbedAt = {};
                m_MagazineInteractionBoltContactActive = false;
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                Game::logMsg("[VR][MagazineInteraction] bolt released before rear threshold; bolt returned to rest");
            }
            return m_MagazineInteractionBoltStageBeforeBackendReloadComplete
                ? reloadCommandPending()
                : false;
        }

        Vector visualAxis = VrHandMath::Normalize(m_MagazineInteractionBoltPullAxisWorld);
        if (visualAxis.Length() <= 0.0001f)
            visualAxis = MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        if (visualAxis.Length() > 0.0001f)
            m_MagazineInteractionBoltPullAxisWorld = visualAxis;

        Vector inputAxis = VrHandMath::Normalize(m_MagazineInteractionBoltInputAxisWorld);
        if (inputAxis.Length() <= 0.0001f)
            inputAxis = MagazineInteractionBuildBoltInputAxisWorld(this, visualAxis);
        if (inputAxis.Length() > 0.0001f)
            m_MagazineInteractionBoltInputAxisWorld = inputAxis;
        visualAxis = MagazineInteractionAlignBoltVisualAxisToInputAxis(this, visualAxis, inputAxis);
        if (visualAxis.Length() > 0.0001f)
            m_MagazineInteractionBoltPullAxisWorld = visualAxis;

        const Vector handLocal = leftControllerPosRelativeToWeapon();
        if (!l4d2vr_interaction::Finite(handLocal))
            return false;
        const float handPullDistance = m_MagazineInteractionBoltGrabStartLeftControllerPosAbs.x - handLocal.x;
        if (!m_MagazineInteractionBoltPullAxisSignLocked)
        {
            const float signLockDistance = std::max(0.10f, 0.006f * m_VRScale);
            if (std::fabs(handPullDistance) >= signLockDistance)
            {
                m_MagazineInteractionBoltPullAxisSignLocked = true;
                Game::logMsg(
                    "[VR][MagazineInteraction] bolt pull input locked visualAxis=(%.2f %.2f %.2f) inputAxis=(%.2f %.2f %.2f) firstProjected=%.2f",
                    visualAxis.x,
                    visualAxis.y,
                    visualAxis.z,
                    inputAxis.x,
                    inputAxis.y,
                    inputAxis.z,
                    handPullDistance);
            }
        }
        const float desiredPull = std::max(
            0.0f,
            m_MagazineInteractionBoltGrabStartPullDistance + handPullDistance);
        setBoltPullDistance(desiredPull);
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);

        if (!m_MagazineInteractionBoltReachedRear &&
            m_MagazineInteractionBoltPullDistance >= requiredPull)
        {
            m_MagazineInteractionBoltReachedRear = true;
            MagazineInteractionPlayBoltSound(this, false);
            triggerMagazineInteractionHaptic(0.024f, 110.0f, 0.38f, 2);
            Game::logMsg(
                "[VR][MagazineInteraction] bolt rear threshold reached pull=%.2f required=%.2f handProjected=%.2f",
                m_MagazineInteractionBoltPullDistance,
                requiredPull,
                handPullDistance);
        }

        if (l4d2vr_physical::PumpCycles::StrokeComplete(
            m_MagazineInteractionBoltReachedRear, m_MagazineInteractionBoltPullDistance, returnDistance))
        {
            completeBoltStage("returned-to-battery", leftGripDown);
        }
        return m_MagazineInteractionBoltStageBeforeBackendReloadComplete
            ? reloadCommandPending()
            : false;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine)
    {
        m_MagazineInteractionOldMagazineContactActive = false;
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, m_MagazineInteractionMagazineModelName);
        if (l4d2vr_magazine::MayFinishShotgunLoading(m_MagazineInteractionShotgunShellMode,
                m_MagazineInteractionChamberEmpty, activeClip, m_MagazineInteractionShotgunShellsLoadedThisSession,
                leftGripDown))
        {
            MagazineInteractionBoxSnapshot boltBox{};
            if (getFreshBoltBoxForActiveViewmodel(boltBox))
            {
                const float grabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
                    boltBox,
                    supportPosition,
                    supportAngles,
                    m_VRScale,
                    this);
                const float grabRange = std::max(0.0f, MagazineInteractionResolveBoltGrabPaddingMeters(this)) * m_VRScale;
                if (grabDistance <= grabRange)
                {
                    if (beginBoltStage(activeWeaponId, "shotgun-player-finished-loading"))
                        beginBoltHoldFromCurrentLeftHand(grabDistance, grabRange);
                    return false;
                }
            }
        }

        MagazineInteractionBoxSnapshot pickupBox{};
        VrHandMatrix4 pickupMagazineWorld{};
        MagazineInteractionBoxSnapshot freshGrabBox{};
        bool hasPickupBox = false;
        QAngle pickupAngles{};
        if (m_MagazineInteractionSocketValid &&
            MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketWorld) &&
            MagazineInteractionBuildFreshMagazinePickupBox(this, pickupBox, pickupAngles))
        {
            pickupMagazineWorld = MagazineInteractionBuildWorldAtBoxCenter(
                MagazineInteractionBuildBoxWorld(pickupBox),
                m_MagazineInteractionSocketBox,
                pickupBox.origin);
            if (MagazineInteractionMatrixLooksRenderable(pickupMagazineWorld))
            {
                freshGrabBox = MagazineInteractionBuildWorldBoxSnapshotAtCenter(
                    pickupBox,
                    pickupMagazineWorld,
                    pickupBox.origin);
                pickupBox = freshGrabBox;
                hasPickupBox = true;
            }
        }
        if (hasPickupBox)
        {
            setDetachedMagazineWorld(pickupMagazineWorld);
        }
        else
        {
            m_MagazineInteractionFreshMagazineContactActive = false;
        }

        float freshGrabDistance = 999999.0f;
        const float freshGrabRange =
            std::max(0.0f, m_MagazineInteractionFreshMagazineGrabRangeMeters) * m_VRScale;
        if (hasPickupBox)
        {
            freshGrabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
                freshGrabBox,
                supportPosition,
                supportAngles,
                m_VRScale,
                this);
            updateMagazineInteractionContactHaptic(
                m_MagazineInteractionFreshMagazineContactActive,
                freshGrabDistance <= freshGrabRange,
                0.012f,
                150.0f,
                0.22f);
        }

        const bool quickFreshAutoGrab =
            m_MagazineInteractionQuickReloadMode &&
            hasPickupBox &&
            freshGrabDistance <= freshGrabRange;
        const bool freshGrabRequested =
            quickFreshAutoGrab ||
            (leftGripDown && freshGrabDistance <= freshGrabRange);
        if (freshGrabRequested)
        {
            if (!HasFreshMagazineInteractionTracking()) return false;
            if (!hasPickupBox)
            {
                return reloadCommandPending();
            }

            if (freshGrabDistance > freshGrabRange)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] fresh magazine grab ignored; left hand outside fresh magazine box distance=%.2f range=%.2f",
                    freshGrabDistance,
                    freshGrabRange);
                return reloadCommandPending();
            }

            if (m_MagazineInteractionShotgunShellMode && maxClip > 0 && activeClip >= maxClip)
            {
                return false;
            }

            const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
                this,
                supportPosition,
                supportForward,
                supportRight,
                supportUp);
            bool usedWristAnchor = false;
            Vector targetHandAnchorWorld{};
            Vector freshHandAnchorLocal{};
            Vector freshCenterLocal{};
            if (!snapFreshMagazineCenterOffsetToLeftHandAnchor(
                controllerWorld,
                usedWristAnchor,
                targetHandAnchorWorld,
                freshHandAnchorLocal,
                freshCenterLocal)) return false;
            m_VrHandsTwoHandedGripActive = false;
            m_VrHandsTwoHandedGripWeaponId = 0;
            m_VrHandsTwoHandedGripWeaponTag = 0;
            m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
            m_MagazineInteractionRetainedMagazineHeld = false;
            m_MagazineInteractionLeftHandHolding = true;
            m_MagazineInteractionFreshGrabbedAt = now;
            if (m_MagazineInteractionShotgunShellMode) m_ShellRetryContact.Reset();
            m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
            const VrHandMatrix4 snappedFreshMagazineWorld = buildFreshHeldMagazineWorldFromLeftHand();
            setDetachedMagazineWorld(snappedFreshMagazineWorld);
            const Vector freshClipOrigin = MagazineInteractionMatrixOrigin(snappedFreshMagazineWorld);
            const Vector freshCenterWorld = MagazineInteractionMatrixPointWorld(
                snappedFreshMagazineWorld,
                freshCenterLocal);
            const Vector freshHandAnchorWorld = MagazineInteractionMatrixPointWorld(
                snappedFreshMagazineWorld,
                freshHandAnchorLocal);
            Game::logMsg(
                "[VR][MagazineInteraction] fresh ammo snapped to calibrated hand grip distance=%.2f range=%.2f calibratedGrip=%d quickAutoGrab=%d wristAnchor=%d clipOrigin=(%.2f %.2f %.2f) visibleCenter=(%.2f %.2f %.2f) handAnchor=(%.2f %.2f %.2f) targetHandAnchor=(%.2f %.2f %.2f) centerLocalOffset=(%.2f %.2f %.2f) centerLocal=(%.2f %.2f %.2f) anchorLocal=(%.2f %.2f %.2f) model=%s; move it into MagazineSocket",
                freshGrabDistance,
                freshGrabRange,
                1,
                quickFreshAutoGrab ? 1 : 0,
                usedWristAnchor ? 1 : 0,
                freshClipOrigin.x,
                freshClipOrigin.y,
                freshClipOrigin.z,
                freshCenterWorld.x,
                freshCenterWorld.y,
                freshCenterWorld.z,
                freshHandAnchorWorld.x,
                freshHandAnchorWorld.y,
                freshHandAnchorWorld.z,
                targetHandAnchorWorld.x,
                targetHandAnchorWorld.y,
                targetHandAnchorWorld.z,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal.x,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal.y,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal.z,
                freshCenterLocal.x,
                freshCenterLocal.y,
                freshCenterLocal.z,
                freshHandAnchorLocal.x,
                freshHandAnchorLocal.y,
                freshHandAnchorLocal.z,
                m_MagazineInteractionMagazineModelName.c_str());
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
    {
        const float freshGrabAgeSeconds =
            (m_MagazineInteractionFreshGrabbedAt.time_since_epoch().count() != 0)
            ? std::chrono::duration<float>(now - m_MagazineInteractionFreshGrabbedAt).count()
            : 999.0f;
        const bool quickReloadKeepsFreshMagazineAttached =
            m_MagazineInteractionQuickReloadMode && !m_MagazineInteractionRetainedMagazineHeld;
        if (!quickReloadKeepsFreshMagazineAttached &&
            !leftGripDown &&
            (m_MagazineInteractionRetainedMagazineHeld || freshGrabAgeSeconds >= 0.18f))
        {
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionRetainedMagazineHeld = false;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionFreshPickupBasisValid = false;
            if (m_MagazineInteractionShotgunShellMode) m_ShellRetryContact.Reset();
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            Game::logMsg(
                "[VR][MagazineInteraction] fresh magazine dropped before socket insertion age=%.3fs",
                freshGrabAgeSeconds);
            return reloadCommandPending();
        }

        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, m_MagazineInteractionMagazineModelName);
        if (m_MagazineInteractionShotgunShellMode && maxClip > 0 && activeClip >= maxClip)
        {
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionFreshPickupBasisValid = false;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            if (leftGripDown)
                m_MagazineInteractionSuppressLeftInputUntilRelease = true;
            return false;
        }

        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
        updateFreshDetachedMagazineFromLeftHand();
        const bool fitsSocket = detachedMagazineFitsSocket();
        if (m_MagazineInteractionShotgunShellMode && !m_ShellRetryContact.Allows(fitsSocket)) return false;
        if (m_MagazineInteractionRetainedMagazineHeld)
        {
            // Catching while the hand is still under the well must not insert
            // the old magazine immediately. Withdraw it, then return it.
            const float travel = (leftControllerPosRelativeToWeapon() - m_RetainedMagazineCatchHandLocal).Length();
            const float required = std::max(0.03f, m_MagazineInteractionPullTriggerMeters) * m_VRScale;
            if (!m_RetainedMagazineInsertGate.Update(fitsSocket, travel, required)) return false;
        }
        if (fitsSocket)
        {
            if (RemoteShellRequestPending() || RemoteMagazineRequestPending()) return false;
            if (m_MagazineInteractionShotgunShellMode)
            {
                captureShotgunStableSocketCapture("shell-insert-request");
                int ammoType = -1, reserve = -1, reserveOffset = -1;
                const bool ammoRead = readClientPrimaryAmmo(ammoType, reserve, reserveOffset);
                if (ammoRead && BeginRemoteShellRequest(activeWeaponHandle, activeWeaponIdInt, activeClip, reserve,
                    reinterpret_cast<uintptr_t>(localPlayer),
                    m_MagazineInteractionSessionGeneration.load(std::memory_order_acquire)))
                {
                    m_MagazineInteractionState = MagazineInteractionManualState::WaitingForServerShell;
                    m_MagazineInteractionLeftHandHolding = l4d2vr_shell::ShellRemainsHeld(
                        m_MagazineInteractionLeftHandHolding, leftGripDown, m_MagazineInteractionQuickReloadMode);
                    m_MagazineInteractionLeftHandPoseActive.store(m_MagazineInteractionLeftHandHolding ? 1u : 0u,
                        std::memory_order_relaxed);
                }
                else if (m_VrHandsDebugLog)
                {
                    static uint64_t lastLogAt = 0u;
                    const uint64_t logAt = GetTickCount64();
                    if (logAt - lastLogAt >= 1000u)
                    {
                        lastLogAt = logAt;
                        Game::logMsg("[VR][ShellInsert][client] request deferred ammoRead=%d ammoType=%d reserve=%d handleValid=%d command=%d controlReady=%d supported=%d",
                            ammoRead ? 1 : 0, ammoType, reserve, l4d2vr_shell::ValidHandle(activeWeaponHandle) ? 1 : 0,
                            m_CurrentPredictedHitFeedbackCmdNumber, m_FirstPersonControlReady.load(std::memory_order_acquire) ? 1 : 0,
                            RemoteShellProtocolSupported() ? 1 : 0);
                    }
                }
                return false;
            }
            if (activeAuthoritativeMagazineBackend && !m_MagazineInteractionShotgunShellMode)
            {
                beginRemoteMagazineAction(m_MagazineInteractionRetainedMagazineHeld
                    ? l4d2vr_remote_mag::Action::Reinsert : l4d2vr_remote_mag::Action::Insert);
                return false;
            }

            CancelMagazineInteractionManual();
            return false;
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine)
    {
        if (!leftGripDown)
        {
            if (m_MagazineInteractionOldMagazinePulled ||
                m_MagazineInteractionReloadTriggered)
            {
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionFreshPickupBasisValid = false;
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                Game::logMsg("[VR][MagazineInteraction] pulled old magazine released; old magazine stays hidden, waiting for fresh magazine");
                return reloadCommandPending();
            }
            else
            {
                Game::logMsg("[VR][MagazineInteraction] old magazine released before pull threshold; restoring native magazine");
                CancelMagazineInteractionManual();
            }
            return false;
        }

        const VrHandMatrix4 heldMagazineWorld = buildHeldMagazineWorldFromLeftHand();
        setDetachedMagazineWorld(heldMagazineWorld);
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
        const float handPullDistance =
            (leftControllerPosRelativeToWeapon() -
                m_MagazineInteractionGrabStartLeftControllerPosAbs).Length();
        const float handTriggerDistance = std::max(0.0f, m_MagazineInteractionPullTriggerMeters) * m_VRScale;
        float magazinePullDistance = 0.0f;
        const bool useInputSocket = inputSocketValid;
        const MagazineInteractionBoxSnapshot& socketBoxForPull =
            useInputSocket ? inputSocketBox : m_MagazineInteractionSocketBox;
        const VrHandMatrix4 socketWorldForPull =
            useInputSocket ? inputSocketWorld : m_MagazineInteractionSocketWorld;
        if ((useInputSocket || m_MagazineInteractionSocketValid) &&
            MagazineInteractionMatrixLooksRenderable(socketWorldForPull) &&
            MagazineInteractionMatrixLooksRenderable(heldMagazineWorld))
        {
            const Vector centerLocal = MagazineInteractionBoxCenterLocal(socketBoxForPull);
            const Vector socketCenter = MagazineInteractionMatrixPointWorld(
                socketWorldForPull,
                centerLocal);
            const Vector heldCenter = MagazineInteractionMatrixPointWorld(
                heldMagazineWorld,
                centerLocal);
            magazinePullDistance = (heldCenter - socketCenter).Length();
        }
        const float magazineTriggerDistance =
            std::max(0.0f, m_MagazineInteractionPullTriggerByMagazineMeters) * m_VRScale;
        const bool handPulled = handTriggerDistance <= 0.0f || handPullDistance >= handTriggerDistance;
        const bool magazinePulled = magazineTriggerDistance <= 0.0f || magazinePullDistance >= magazineTriggerDistance;
        if (!m_MagazineInteractionOldMagazinePulled && (handPulled || magazinePulled))
        {
            m_MagazineInteractionOldMagazinePulled = true;
            m_MagazineInteractionOldMagazineContactActive = false;
            if (activeAuthoritativeMagazineBackend)
            {
                if (!beginRemoteMagazineAction(l4d2vr_remote_mag::Action::Eject)) CancelMagazineInteractionManual();
                return false;
            }

            CancelMagazineInteractionManual();
            return false;

            MagazineInteractionPlayClipOutSound(this);
            triggerMagazineInteractionBothHandsHaptic(0.026f, 95.0f, 0.42f, 2);
            Game::logMsg(
                "[VR][MagazineInteraction] old magazine pull threshold reached after clip-out handDistance=%.2f handThreshold=%.2f magazineDistance=%.2f magazineThreshold=%.2f",
                handPullDistance,
                handTriggerDistance,
                magazinePullDistance,
                magazineTriggerDistance);
            return reloadCommandPending();
        }
        return reloadCommandPending();
    }

    if (l4d2vr_magazine::ShouldEject(m_MagazineReleaseButtonRequired,
        m_MagazineReleaseJustPressed, m_MagazineInteractionSuppressEmptyClipAutoReload,
        activeClip, MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId)))
    {
        MagazineInteractionBoxSnapshot box{};
        float boxAgeSeconds = -1.0f;
        bool usedCachedBox = false;
        if (getMagazineBoxForAutoEject(box, boxAgeSeconds, usedCachedBox))
        {
            beginMagazineInteractionSession(box);
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionChamberEmpty = m_PhysicalReloadObservedEmpty || activeClip <= 0;
            m_MagazineInteractionOneInChamber =
                    l4d2vr_magazine::ChamberRoundsAfterEject(m_PhysicalReloadObservedEmpty ? 0 : activeClip) != 0;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionOldMagazinePulled = true;
            m_MagazineInteractionFreshPickupBasisValid = false;
            m_MagazineInteractionViewmodelFreezeDeferredUntil =
                now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(GetMagazineInteractionEmptyClipAutoEjectFreezeDelaySeconds(activeWeaponId)));
            if (activeAuthoritativeMagazineBackend)
            {
                if (!beginRemoteMagazineAction(l4d2vr_remote_mag::Action::Eject)) CancelMagazineInteractionManual();
                return false;
            }

                m_MagazineInteractionReloadTriggered = false;
                m_MagazineInteractionReloadCommandPending = false;
                m_MagazineInteractionReloadCommandIssued = false;
                m_MagazineInteractionReloadCommandHoldUntil = {};
                clearNativeClientReloadState("empty-clip-auto-eject-defer-reload");
                if (m_Game)
                {
                    m_Game->ClientCmd_Unrestricted("-reload");
                    m_Game->ClientCmd_Unrestricted("-attack");
                }
                m_ReloadCmdOwned = false;

            MagazineInteractionPlayClipOutSound(this);
            Game::logMsg(
                "[VR][MagazineInteraction] magazine released; waiting for fresh magazine weaponId=%d clip=%d ent=%d bone=%d age=%.3fs cached=%d freezeDelay=%.2fs model=%s",
                static_cast<int>(activeWeaponId),
                activeClip,
                box.entityIndex,
                box.boneIndex,
                boxAgeSeconds,
                usedCachedBox ? 1 : 0,
                GetMagazineInteractionEmptyClipAutoEjectFreezeDelaySeconds(activeWeaponId),
                box.modelName.c_str());
            return reloadCommandPending();
        }
    }

    m_MagazineInteractionFreshMagazineContactActive = false;
    m_MagazineInteractionBoltContactActive = false;

    // Attached magazines require the release button in physical-controls mode.
    // An off-hand grip near the magazine well must not start an ejection.
    if (m_MagazineReleaseButtonRequired)
        return reloadCommandPending();

    if (!MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId))
    {
        m_MagazineInteractionOldMagazineContactActive = false;
        return false;
    }

    MagazineInteractionBoxSnapshot box{};
    if (!GetMagazineInteractionBox(box))
    {
        m_MagazineInteractionOldMagazineContactActive = false;
        return false;
    }

    const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, box.modelName);
    if (maxClip > 0 && activeClip >= maxClip)
    {
        m_MagazineInteractionOldMagazineContactActive = false;
        if (leftGripJustPressed)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] ignored full magazine weaponId=%d clip=%d max=%d",
                static_cast<int>(activeWeaponId),
                activeClip,
                maxClip);
        }
        return false;
    }

    const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
    if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
    {
        m_MagazineInteractionOldMagazineContactActive = false;
        return false;
    }

    const float grabPadding = std::max(0.0f, m_MagazineInteractionGrabPaddingMeters) * m_VRScale;
    MagazineInteractionBoxSnapshot oldMagazineTouchBox = box;
    VrHandMatrix4 oldMagazineTouchWorld = MagazineInteractionBuildBoxWorld(box);
    if (!activeWeaponUsesShotgunShells)
    {
        MagazineInteractionBoxSnapshot rebasedBox{};
        VrHandMatrix4 rebasedWorld{};
        if (MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(
                this,
                box,
                rebasedBox,
                rebasedWorld))
        {
            oldMagazineTouchBox = rebasedBox;
            oldMagazineTouchWorld = rebasedWorld;
        }
    }
    const float distance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
        oldMagazineTouchBox,
        supportPosition,
        supportAngles,
        m_VRScale,
        this);
    const bool touchingOldMagazine = distance <= grabPadding;
    updateMagazineInteractionContactHaptic(
        m_MagazineInteractionOldMagazineContactActive,
        touchingOldMagazine,
        0.012f,
        150.0f,
        0.22f);

    if (!leftGripJustPressed)
        return false;

    if (!touchingOldMagazine)
    {
        static std::chrono::steady_clock::time_point s_lastMissLog{};
        if (s_lastMissLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastMissLog).count() >= 0.50f)
        {
            s_lastMissLog = now;
            Game::logMsg(
                "[VR][MagazineInteraction] interaction gesture pressed but magazine box is out of reach nearest=%.2f padding=%.2f ent=%d bone=%d age=%.3fs",
                distance,
                grabPadding,
                box.entityIndex,
                box.boneIndex,
                ageSeconds);
        }
        return false;
    }

    beginMagazineInteractionSession(box);
    m_MagazineInteractionState = MagazineInteractionManualState::HoldingOldMagazine;
    m_MagazineInteractionLeftHandHolding = true;
    if (!MagazineInteractionMatrixLooksRenderable(oldMagazineTouchWorld))
        oldMagazineTouchWorld = m_MagazineInteractionSocketWorld;
    const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
        this,
        supportPosition,
        supportForward,
        supportRight,
        supportUp);
    {
        m_MagazineInteractionControllerToMagazine =
            MagazineInteractionBuildControllerRelation(controllerWorld, oldMagazineTouchWorld);
    }
    const Vector socketCenterLocal = MagazineInteractionBoxCenterLocal(oldMagazineTouchBox);
    const Vector socketCenterWorld = MagazineInteractionMatrixPointWorld(
        oldMagazineTouchWorld,
        socketCenterLocal);
    {
        m_MagazineInteractionHeldMagazineCenterOffsetLocal =
            MagazineInteractionWorldVectorToMatrixLocal(
                controllerWorld,
                socketCenterWorld - MagazineInteractionMatrixOrigin(controllerWorld));
    }
    setDetachedMagazineWorld(buildHeldMagazineWorldFromLeftHand());
    m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
    Game::logMsg(
        "[VR][MagazineInteraction] old magazine grabbed; froze viewmodel and hid native clip weaponId=%d weaponEnt=%d clip=%d ent=%d bone=%d distance=%.2f padding=%.2f centerLocalOffset=(%.2f %.2f %.2f) model=%s",
        static_cast<int>(activeWeaponId),
        m_MagazineInteractionWeaponEntityIndex,
        activeClip,
        box.entityIndex,
        box.boneIndex,
        distance,
        grabPadding,
        m_MagazineInteractionHeldMagazineCenterOffsetLocal.x,
        m_MagazineInteractionHeldMagazineCenterOffsetLocal.y,
        m_MagazineInteractionHeldMagazineCenterOffsetLocal.z,
        box.modelName.c_str());
    return false;
}

bool VR::DrawVrHandsForEye(const CViewSetup& view, int eyeIndex, VrHandDrawPass drawPass)
{
    return DrawVrHandsForEyeImmediate(view, eyeIndex, drawPass, false);
}

void VR::FallbackVrHandsGlovesToNative(const char* reason)
{
    if (!m_VrHandsEnabled && m_NativeViewmodelHandsOnly)
        return;

    if (!m_VrHandsGlovesRuntimeFallback)
        m_VrHandsGlovesHideArmsBeforeFallback = m_HideArms;
    m_VrHandsEnabled = false;
    m_NativeViewmodelHandsOnly = true;
    m_HideArms = false;
    m_VrHandsGlovesRuntimeFallback = true;

    if (!m_VrHandsGlovesFallbackLogged)
    {
        m_VrHandsGlovesFallbackLogged = true;
        Game::logMsg(
            "[VR][Hands] VR glove assets unavailable (%s); using native hands while retrying",
            (reason && reason[0] != '\0') ? reason : "unknown dependency failure");
    }
}

bool VR::DrawVrHandsForEyeImmediate(
    const CViewSetup& view,
    int eyeIndex,
    VrHandDrawPass drawPass,
    bool allowQueuedMode)
{
    const bool emptyHandsPlaceholderActive =
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
    const bool drawGloves =
        m_VrHandsEnabled && (!emptyHandsPlaceholderActive || m_GripReleaseDropEnabled) && m_Input;
    const bool calibrationOverlayActive =
        m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
    const bool drawMagazineDebugBoxes =
        (m_MagazineBoxDebugEnabled || calibrationOverlayActive) &&
        drawPass != VrHandDrawPass::WorldVisibilityMask &&
        HasFreshMagazineInteractionDebugBoxWork();
    const bool drawLeftHandTargetDebugBox =
        (m_MagazineBoxDebugEnabled || calibrationOverlayActive) &&
        drawPass != VrHandDrawPass::WorldVisibilityMask &&
        (m_VrHandsEnabled || m_NativeViewmodelHandsOnly);
    if ((!drawGloves && !drawMagazineDebugBoxes && !drawLeftHandTargetDebugBox) ||
        !m_IsVREnabled ||
        !m_Game)
    {
        return false;
    }

    const int queueMode = m_Game->GetMatQueueMode();
    if (queueMode != 0 && !allowQueuedMode)
        return false;

    if (queueMode != 0 && !g_D3DVR9)
        return false;

    IDirect3DSurface9* surface = nullptr;
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (m_CreatingTextureID != Texture_None)
            return false;

        surface = (eyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
        if (surface)
            surface->AddRef();
    }
    if (!surface)
        return false;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device)
    {
        surface->Release();
        return false;
    }

    bool drewAny = false;
    {
        ScopedVrHandsQueuedD3DLock queuedLock(queueMode != 0);
        ScopedVrHandsD3DTarget targetScope(
            device,
            surface,
            m_RenderWidth,
            m_RenderHeight,
            queueMode != 0);
        if (!targetScope.IsBound())
        {
            device->Release();
            surface->Release();
            return false;
        }

        if (!m_VrHands)
            m_VrHands = std::make_unique<VrHandSystem>();

        ScopedVrHandsRenderSnapshot renderSnapshot(queueMode != 0 && allowQueuedMode);

        float sceneLightScale = 1.0f;
        if (m_AutoFlashlightHasScreenLuma)
        {
            const float localLuma = std::max(
                m_AutoFlashlightCenterMedianLuma,
                m_AutoFlashlightPeripheralMedianLuma * 0.35f);
            sceneLightScale = std::clamp(localLuma / 110.0f, 0.08f, 1.0f);
        }

        // Detached magazines are primarily rendered by repeating the native Source
        // viewmodel draw with every non-clip bone moved out of view. While a fresh
        // magazine is waiting/held, keep a lightweight standalone wire box visible as a
        // fallback because the matching weapon viewmodel draw is not guaranteed to
        // occur every frame.
        VrHandMatrix4 standaloneMagazineBoxWorld{};
        const VrHandMatrix4* standaloneMagazineBoxWorldPtr = nullptr;
        Vector standaloneMagazineBoxMins(0.0f, 0.0f, 0.0f);
        Vector standaloneMagazineBoxMaxs(0.0f, 0.0f, 0.0f);
        const bool standaloneMagazineBoxUseViewmodelLayer = true;
        VrHandMatrix4 magazineSocketCaptureBoxWorld{};
        const VrHandMatrix4* magazineSocketCaptureBoxWorldPtr = nullptr;
        Vector magazineSocketCaptureBoxMins(0.0f, 0.0f, 0.0f);
        Vector magazineSocketCaptureBoxMaxs(0.0f, 0.0f, 0.0f);
        const bool magazineSocketCaptureBoxUseViewmodelLayer = true;
        VrHandMatrix4 currentMagazineBoxWorld{};
        const VrHandMatrix4* currentMagazineBoxWorldPtr = nullptr;
        Vector currentMagazineBoxMins(0.0f, 0.0f, 0.0f);
        Vector currentMagazineBoxMaxs(0.0f, 0.0f, 0.0f);
        const bool currentMagazineBoxUseViewmodelLayer = true;
        VrHandMatrix4 currentBoltBoxWorld{};
        const VrHandMatrix4* currentBoltBoxWorldPtr = nullptr;
        Vector currentBoltBoxMins(0.0f, 0.0f, 0.0f);
        Vector currentBoltBoxMaxs(0.0f, 0.0f, 0.0f);
        const bool currentBoltBoxUseViewmodelLayer = true;
        VrHandMatrix4 leftHandTargetBoxWorld{};
        const VrHandMatrix4* leftHandTargetBoxWorldPtr = nullptr;
        Vector leftHandTargetBoxMins(0.0f, 0.0f, 0.0f);
        Vector leftHandTargetBoxMaxs(0.0f, 0.0f, 0.0f);
        bool leftHandTargetBoxUseViewmodelLayer = true;
    if (drawMagazineDebugBoxes &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine) &&
        m_MagazineInteractionSocketValid &&
        GetMagazineInteractionDetachedMagazineWorld(standaloneMagazineBoxWorld))
    {
        standaloneMagazineBoxWorldPtr = &standaloneMagazineBoxWorld;
        const Vector centerLocal = MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox);
        const Vector freshHalf = MagazineInteractionFreshMagazineHalfExtentsSourceUnits(this);
        standaloneMagazineBoxMins = centerLocal - freshHalf;
        standaloneMagazineBoxMaxs = centerLocal + freshHalf;
        MagazineInteractionInflateLocalBox(
            standaloneMagazineBoxMins,
            standaloneMagazineBoxMaxs,
            std::max(0.0f, m_MagazineInteractionFreshMagazineGrabRangeMeters) * m_VRScale);
    }
    if (drawMagazineDebugBoxes &&
        m_MagazineInteractionSocketValid &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine))
    {
        MagazineInteractionBoxSnapshot socketBox = m_MagazineInteractionSocketBox;
        VrHandMatrix4 socketWorld = m_MagazineInteractionSocketWorld;
        MagazineInteractionBoxSnapshot rebasedSocketBox{};
        VrHandMatrix4 rebasedSocketWorld{};
        if (MagazineInteractionRebaseBoxToCurrentViewmodelModelBasis(
                this,
                m_MagazineInteractionSocketBox,
                rebasedSocketBox,
                rebasedSocketWorld))
        {
            socketBox = rebasedSocketBox;
            socketWorld = rebasedSocketWorld;
        }

        MagazineInteractionBoxSnapshot captureBox = socketBox;
        magazineSocketCaptureBoxWorld = socketWorld;
        if (MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketWorld) &&
            MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketCaptureWorld))
        {
            const VrHandMatrix4 socketToCapture = MagazineInteractionBuildControllerRelation(
                m_MagazineInteractionSocketWorld,
                m_MagazineInteractionSocketCaptureWorld);
            const VrHandMatrix4 captureWorld = MagazineInteractionBuildWorldFromControllerRelation(
                socketWorld,
                socketToCapture);
            if (MagazineInteractionMatrixLooksRenderable(captureWorld))
            {
                captureBox = m_MagazineInteractionSocketCaptureBox;
                captureBox.origin = MagazineInteractionMatrixOrigin(captureWorld);
                captureBox.axisX = MagazineInteractionMatrixAxis(captureWorld, 0);
                captureBox.axisY = MagazineInteractionMatrixAxis(captureWorld, 1);
                captureBox.axisZ = MagazineInteractionMatrixAxis(captureWorld, 2);
                magazineSocketCaptureBoxWorld = captureWorld;
            }
        }

        magazineSocketCaptureBoxWorldPtr = &magazineSocketCaptureBoxWorld;
        magazineSocketCaptureBoxMins = captureBox.mins;
        magazineSocketCaptureBoxMaxs = captureBox.maxs;
    }
    if (drawMagazineDebugBoxes)
    {
        auto tryUseDebugBox = [&](
            const MagazineInteractionBoxSnapshot& debugBox,
            VrHandMatrix4& outWorld,
            const VrHandMatrix4*& outWorldPtr,
            Vector& outMins,
            Vector& outMaxs)
            {
                const float ageSeconds = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - debugBox.publishedAt).count();
                if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
                    return false;

                outWorld = MagazineInteractionBuildBoxWorld(debugBox);
                if (!MagazineInteractionMatrixLooksRenderable(outWorld))
                    return false;

                outWorldPtr = &outWorld;
                outMins = debugBox.mins;
                outMaxs = debugBox.maxs;
                return true;
            };

        MagazineInteractionBoxSnapshot debugBox{};
        if (GetMagazineInteractionBox(debugBox) &&
            tryUseDebugBox(debugBox, currentMagazineBoxWorld, currentMagazineBoxWorldPtr, currentMagazineBoxMins, currentMagazineBoxMaxs))
        {
            MagazineInteractionInflateLocalBox(
                currentMagazineBoxMins,
                currentMagazineBoxMaxs,
                std::max(0.0f, m_MagazineInteractionGrabPaddingMeters) * m_VRScale);
        }

        MagazineInteractionBoxSnapshot boltDebugBox{};
        if (GetMagazineInteractionBoltBox(boltDebugBox) &&
            tryUseDebugBox(boltDebugBox, currentBoltBoxWorld, currentBoltBoxWorldPtr, currentBoltBoxMins, currentBoltBoxMaxs))
        {
            MagazineInteractionInflateLocalBox(
                currentBoltBoxMins,
                currentBoltBoxMaxs,
                std::max(0.0f, MagazineInteractionResolveBoltGrabPaddingMeters(this)) * m_VRScale);
        }
    }
    if (drawLeftHandTargetDebugBox)
    {
        if (m_VrHandsTwoHandedAimMountFriendly)
        {
            MagazineInteractionBoxSnapshot stockBox{};
            if (TryBuildMountFriendlyTwoHandedGripBox(
                    this,
                    m_VRScale,
                    std::chrono::steady_clock::now(),
                    stockBox))
            {
                leftHandTargetBoxWorld = MagazineInteractionBuildBoxWorld(stockBox);
                if (MagazineInteractionMatrixLooksRenderable(leftHandTargetBoxWorld))
                {
                    leftHandTargetBoxWorldPtr = &leftHandTargetBoxWorld;
                    leftHandTargetBoxMins = stockBox.mins;
                    leftHandTargetBoxMaxs = stockBox.maxs;
                    leftHandTargetBoxUseViewmodelLayer = true;
                }
            }
        }
        else if (TryBuildLeftHandTargetDebugBox(
                    m_VRScale,
                    m_VrHandsTwoHandedGripTargetBoxScale,
                    leftHandTargetBoxWorld,
                    leftHandTargetBoxMins,
                    leftHandTargetBoxMaxs) &&
                MagazineInteractionMatrixLooksRenderable(leftHandTargetBoxWorld))
        {
            leftHandTargetBoxWorldPtr = &leftHandTargetBoxWorld;
            leftHandTargetBoxUseViewmodelLayer = true;
        }
    }
    const Vector leftControllerPosition = GetLeftControllerAbsPos();
    const QAngle leftControllerAngles = GetLeftControllerAbsAngle();
    const Vector rightControllerPosition = GetRightControllerAbsPos();
    const QAngle rightControllerAngles = GetRightControllerAbsAngle();
    const Vector currentViewmodelPosition = GetRecommendedViewmodelAbsPos();
    const QAngle currentViewmodelAngles = GetRecommendedViewmodelAbsAngle();
    Vector rightHandPoseOffsetMeters = m_VrHandsRightPoseOffsetMeters;
    Vector rightHandPoseRotationOffsetDeg = m_VrHandsRightPoseRotationOffsetDeg;
    // A spent shell blocks firing, not the support hand's attachment to the pump.
    const bool twoHandedGripPoseActive = IsVrHandsTwoHandedGripPoseActive();
    Vector leftHandPoseOffsetMeters = twoHandedGripPoseActive
        ? Vector(0.0f, 0.0f, 0.0f)
        : m_VrHandsLeftPoseOffsetMeters;
    const Vector leftHandPoseRotationOffsetDeg = twoHandedGripPoseActive
        ? Vector(0.0f, 0.0f, 0.0f)
        : m_VrHandsLeftPoseRotationOffsetDeg;
    const bool vrHandsRightUseViewmodelPose =
        m_VrHandsRightUseViewmodelPose && !(emptyHandsPlaceholderActive && m_GripReleaseDropEnabled) &&
        // Native throwable animations can retain their release pose. Keep this
        // hand on the controller through preparation and throwing instead.
        (m_ManualThrowViewmodelInputState.load(std::memory_order_acquire) & 1u) == 0u &&
        m_PistolOwnershipMask.load(std::memory_order_acquire) != 2u;
    if (!vrHandsRightUseViewmodelPose)
    {
        rightHandPoseOffsetMeters = m_VrHandsRightFreePoseOffsetMeters;
        rightHandPoseRotationOffsetDeg = m_VrHandsRightFreePoseRotationOffsetDeg;
    }
    if (m_LeftHanded)
    {
        // Gameplay-right is the physical left/gun hand after the left-handed pose swap.
        rightHandPoseOffsetMeters += m_VrHandsLeftHandedLeftPoseOffsetMeters;

        // Gameplay-left is the physical right/off hand. Apply its dedicated correction
        // only while the off hand is attached to the weapon in a two-hand grip.
        if (twoHandedGripPoseActive)
            leftHandPoseOffsetMeters += m_VrHandsLeftHandedTwoHandedRightPoseOffsetMeters;

        if (vrHandsRightUseViewmodelPose)
        {
            rightHandPoseOffsetMeters += m_VrHandsLeftHandedViewmodelPoseOffsetMeters;
            rightHandPoseRotationOffsetDeg += m_VrHandsLeftHandedViewmodelPoseRotationOffsetDeg;
        }
    }

    if (drawGloves)
    {
        const auto physical = l4d2vr_controller_identity::Unpack(
            m_PhysicalControllerIndexPair.load(std::memory_order_acquire));
        VrHandControllerContext controllers{};
        controllers.system = m_System;
        controllers.physicalIndices = { physical.left, physical.right };
        drewAny = m_VrHands->DrawForEye(
            device,
            m_Input,
            m_InputActionFrame,
            controllers,
            view,
            eyeIndex,
            m_VRScale,
            m_VrHandsModelScale,
            m_VrHandsMotionRangeWithoutController,
            vrHandsRightUseViewmodelPose,
            twoHandedGripPoseActive,
            m_LeftHanded,
            m_MouseModeEnabled,
            m_VrHandsDebugLog,
            sceneLightScale,
            leftControllerPosition,
            leftControllerAngles,
            rightControllerPosition,
            rightControllerAngles,
            currentViewmodelPosition,
            currentViewmodelAngles,
            leftHandPoseOffsetMeters,
            leftHandPoseRotationOffsetDeg,
            rightHandPoseOffsetMeters,
            rightHandPoseRotationOffsetDeg,
            standaloneMagazineBoxWorldPtr,
            standaloneMagazineBoxMins,
            standaloneMagazineBoxMaxs,
            standaloneMagazineBoxUseViewmodelLayer,
            magazineSocketCaptureBoxWorldPtr,
            magazineSocketCaptureBoxMins,
            magazineSocketCaptureBoxMaxs,
            magazineSocketCaptureBoxUseViewmodelLayer,
            currentMagazineBoxWorldPtr,
            currentMagazineBoxMins,
            currentMagazineBoxMaxs,
            currentMagazineBoxUseViewmodelLayer,
            currentBoltBoxWorldPtr,
            currentBoltBoxMins,
            currentBoltBoxMaxs,
            currentBoltBoxUseViewmodelLayer,
            leftHandTargetBoxWorldPtr,
            leftHandTargetBoxMins,
            leftHandTargetBoxMaxs,
            leftHandTargetBoxUseViewmodelLayer,
            MagazineGripPhysicalMask(),
            m_VrHandsGloveFingerMaxCurl,
            drawPass);
    }
    else
    {
        drewAny = m_VrHands->DrawMagazineDebugBoxesForEye(
            device,
            view,
            sceneLightScale,
            standaloneMagazineBoxWorldPtr,
            standaloneMagazineBoxMins,
            standaloneMagazineBoxMaxs,
            standaloneMagazineBoxUseViewmodelLayer,
            magazineSocketCaptureBoxWorldPtr,
            magazineSocketCaptureBoxMins,
            magazineSocketCaptureBoxMaxs,
            magazineSocketCaptureBoxUseViewmodelLayer,
            currentMagazineBoxWorldPtr,
            currentMagazineBoxMins,
            currentMagazineBoxMaxs,
            currentMagazineBoxUseViewmodelLayer,
            currentBoltBoxWorldPtr,
            currentBoltBoxMins,
            currentBoltBoxMaxs,
            currentBoltBoxUseViewmodelLayer,
            leftHandTargetBoxWorldPtr,
            leftHandTargetBoxMins,
            leftHandTargetBoxMaxs,
            leftHandTargetBoxUseViewmodelLayer,
            drawPass);
    }

    if (drawGloves && !drewAny && m_VrHands && m_VrHands->IsDependencyUnavailable())
        FallbackVrHandsGlovesToNative(m_VrHands->DependencyFailureReason().c_str());
    }
    device->Release();
    surface->Release();
    return drewAny;
}

bool VR::DrawVrHandsWorldDepthMaskForEyeImmediate(const CViewSetup& view, int eyeIndex, bool allowQueuedMode)
{
    const bool emptyHandsPlaceholderActive =
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
    if (!m_VrHandsEnabled || (emptyHandsPlaceholderActive && !m_GripReleaseDropEnabled) ||
        !m_IsVREnabled || !m_Input || !m_Game)
        return false;

    const int queueMode = m_Game->GetMatQueueMode();
    if (queueMode != 0 && !allowQueuedMode)
        return false;

    if (queueMode != 0 && !g_D3DVR9)
        return false;

    IDirect3DSurface9* surface = nullptr;
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (m_CreatingTextureID != Texture_None)
            return false;

        surface = (eyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
        if (surface)
            surface->AddRef();
    }
    if (!surface)
        return false;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device)
    {
        surface->Release();
        return false;
    }

    bool stencilReady = false;
    {
        ScopedVrHandsQueuedD3DLock queuedLock(queueMode != 0);
        ScopedVrHandsD3DTarget targetScope(
            device,
            surface,
            m_RenderWidth,
            m_RenderHeight,
            queueMode != 0);
        if (targetScope.IsBound())
        {
            if (!m_VrHands)
                m_VrHands = std::make_unique<VrHandSystem>();

            stencilReady = m_VrHands->ClearViewmodelOcclusionStencil(device);
        }
    }
    device->Release();
    surface->Release();
    if (!stencilReady)
        return false;

    return DrawVrHandsForEyeImmediate(
        view,
        eyeIndex,
        VrHandDrawPass::WorldVisibilityMask,
        allowQueuedMode);
}

void VR::BeginVrHandsEyeRender(const CViewSetup& view, int eyeIndex)
{
    m_VrHandsActiveEyeView = nullptr;
    m_VrHandsActiveEyeIndex = -1;
    m_VrHandsWorldMaskDrawn = false;
    const bool calibrationOverlayActive =
        m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
    const bool drawMagazineDebugBoxes =
        (m_MagazineBoxDebugEnabled || calibrationOverlayActive) &&
        HasFreshMagazineInteractionDebugBoxWork();
    if (!m_IsVREnabled || !m_Game)
        return;

    const bool emptyHandsPlaceholderActive =
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
    const bool gloveModeRequested = m_VrHandsEnabled || m_VrHandsGlovesRuntimeFallback;
    if (gloveModeRequested && (!emptyHandsPlaceholderActive || m_GripReleaseDropEnabled) &&
        m_Input && m_VrHandsGlovesEnabled &&
        l4d2vr_input::ActionFrame::Ready(m_InputActionFrame.Read()))
    {
        if (!m_VrHands)
            m_VrHands = std::make_unique<VrHandSystem>();

        if (m_VrHands->EnsureAssetsAvailable(m_VrHandsDebugLog))
        {
            if (m_VrHandsGlovesRuntimeFallback)
            {
                m_VrHandsEnabled = true;
                m_NativeViewmodelHandsOnly = false;
                m_HideArms = m_VrHandsGlovesHideArmsBeforeFallback;
                m_VrHandsGlovesRuntimeFallback = false;
                m_VrHandsGlovesFallbackLogged = false;
            }
        }
        else if (m_VrHands->IsDependencyUnavailable())
        {
            FallbackVrHandsGlovesToNative(m_VrHands->DependencyFailureReason().c_str());
        }
    }

    const bool drawVrGloves =
        m_VrHandsEnabled && (!emptyHandsPlaceholderActive || m_GripReleaseDropEnabled);
    const bool drawScopeLens = IsPhysicalScopeMode() && m_ScopeEnabled && ScopeWeaponEligible();
    if (!drawVrGloves && !m_NativeViewmodelHandsOnly && !drawMagazineDebugBoxes && !drawScopeLens)
        return;

    if (eyeIndex >= 0 && eyeIndex < 2)
    {
        std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
        m_PhysicalScopeLenses[eyeIndex] = {};
    }

    m_VrHandsActiveEyeView = &view;
    m_VrHandsActiveEyeIndex = eyeIndex;
}

void VR::DrawVrHandsWorldDepthMaskBeforeViewmodel()
{
    if (!m_VrHandsActiveEyeView || m_VrHandsActiveEyeIndex < 0 || m_VrHandsWorldMaskDrawn)
        return;

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode != 0)
    {
        CRefPtr<IMatRenderContext> renderContextRef;
        if (m_Game && m_Game->m_MaterialSystem)
            renderContextRef = m_Game->m_MaterialSystem->GetRenderContext();
        IMatRenderContext* const renderContext = renderContextRef;
        m_VrHandsWorldMaskDrawn = QueueVrHandsDrawForEye(
            renderContext,
            *m_VrHandsActiveEyeView,
            m_VrHandsActiveEyeIndex,
            VrHandDrawPass::WorldVisibilityMask);
        return;
    }

    m_VrHandsWorldMaskDrawn = DrawVrHandsWorldDepthMaskForEyeImmediate(
        *m_VrHandsActiveEyeView,
        m_VrHandsActiveEyeIndex,
        false);
}

void VR::FinishVrHandsEyeRender()
{
    const CViewSetup* view = m_VrHandsActiveEyeView;
    const int eyeIndex = m_VrHandsActiveEyeIndex;
    const bool worldMaskDrawn = m_VrHandsWorldMaskDrawn;
    m_VrHandsActiveEyeView = nullptr;
    m_VrHandsActiveEyeIndex = -1;
    m_VrHandsWorldMaskDrawn = false;

    if (!view || eyeIndex < 0)
        return;

    const VrHandDrawPass drawPass =
        worldMaskDrawn ? VrHandDrawPass::ViewmodelComposite : VrHandDrawPass::WorldDepth;
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode != 0)
    {
        CRefPtr<IMatRenderContext> renderContextRef;
        if (m_Game && m_Game->m_MaterialSystem)
            renderContextRef = m_Game->m_MaterialSystem->GetRenderContext();
        IMatRenderContext* const renderContext = renderContextRef;
        QueueVrHandsDrawForEye(renderContext, *view, eyeIndex, drawPass);
        return;
    }

    DrawVrHandsForEye(*view, eyeIndex, drawPass);
    PhysicalScopeLensSnapshot lens{};
    if (ReadPhysicalScopeLens(eyeIndex, lens)) DrawPhysicalScopeLensForEye(*view, eyeIndex, lens);
}

void VR::ReleaseVrHandsD3DResources()
{
    if (m_VrHands)
        m_VrHands->OnDeviceLost();
}

namespace
{
    int MagazineInteractionFindClientEntityIndex(Game* game, const void* entity)
    {
        if (!game || !game->m_ClientEntityList || !entity)
            return -1;

        int highestIndex = 0;
#ifdef _MSC_VER
        __try
        {
            highestIndex = game->m_ClientEntityList->GetHighestEntityIndex();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
#else
        highestIndex = game->m_ClientEntityList->GetHighestEntityIndex();
#endif
        if (highestIndex < 1)
            return -1;

        highestIndex = (std::min)(highestIndex, 4096);
        for (int i = 1; i <= highestIndex; ++i)
        {
#ifdef _MSC_VER
            __try
            {
                if (game->m_ClientEntityList->GetClientEntity(i) == entity)
                    return i;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
#else
            if (game->m_ClientEntityList->GetClientEntity(i) == entity)
                return i;
#endif
        }

        return -1;
    }

    bool MagazineInteractionSoundLooksWeaponRelated(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("weapon") != std::string::npos ||
            lower.find("reload") != std::string::npos ||
            lower.find("clip") != std::string::npos ||
            lower.find("mag") != std::string::npos ||
            lower.find("bolt") != std::string::npos ||
            lower.find("slide") != std::string::npos ||
            lower.find("shotgun") != std::string::npos ||
            lower.find("shell") != std::string::npos ||
            lower.find("pump") != std::string::npos ||
            lower.find("rifle") != std::string::npos ||
            lower.find("smg") != std::string::npos ||
            lower.find("pistol") != std::string::npos ||
            lower.find("sniper") != std::string::npos;
    }

    bool MagazineInteractionSoundStartsInsertTail(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clip_in") != std::string::npos ||
            lower.find("clip-in") != std::string::npos ||
            lower.find("clipin") != std::string::npos ||
            lower.find("clip.insert") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("magazine_in") != std::string::npos ||
            lower.find("magazine-in") != std::string::npos ||
            lower.find("magazinein") != std::string::npos ||
            lower.find("mag.insert") != std::string::npos ||
            lower.find("insert") != std::string::npos ||
            lower.find("load_shell") != std::string::npos ||
            lower.find("shell_load") != std::string::npos ||
            lower.find("loadshell") != std::string::npos ||
            lower.find("shellin") != std::string::npos ||
            lower.find("clip_locked") != std::string::npos ||
            lower.find("mag_locked") != std::string::npos ||
            lower.find("magazine_locked") != std::string::npos;
    }

    bool MagazineInteractionSoundLooksClipOut(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clip_out") != std::string::npos ||
            lower.find("clip-out") != std::string::npos ||
            lower.find("clipout") != std::string::npos ||
            lower.find("mag_out") != std::string::npos ||
            lower.find("mag-out") != std::string::npos ||
            lower.find("magout") != std::string::npos ||
            lower.find("magazine_out") != std::string::npos ||
            lower.find("magazine-out") != std::string::npos ||
            lower.find("magazineout") != std::string::npos ||
            lower.find("clip.remove") != std::string::npos ||
            lower.find("mag.remove") != std::string::npos ||
            lower.find("magazine.remove") != std::string::npos;
    }

    enum class MagazineInteractionDelayedSoundStage
    {
        Other,
        Insert,
        Lock,
        Ready,
        SlideBack,
        SlideForward
    };

    std::string MagazineInteractionLowerSoundSample(const std::string& sample)
    {
        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower;
    }

    MagazineInteractionDelayedSoundStage MagazineInteractionClassifyDelayedSound(const std::string& sample)
    {
        const std::string lower = MagazineInteractionLowerSoundSample(sample);
        if (lower.find("clip_in") != std::string::npos ||
            lower.find("clip-in") != std::string::npos ||
            lower.find("clipin") != std::string::npos ||
            lower.find("clip.insert") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("magazine_in") != std::string::npos ||
            lower.find("magazine-in") != std::string::npos ||
            lower.find("magazinein") != std::string::npos ||
            lower.find("mag.insert") != std::string::npos ||
            lower.find("insert") != std::string::npos ||
            lower.find("load_shell") != std::string::npos ||
            lower.find("shell_load") != std::string::npos ||
            lower.find("loadshell") != std::string::npos ||
            lower.find("shellin") != std::string::npos)
        {
            return MagazineInteractionDelayedSoundStage::Insert;
        }
        if (lower.find("clip_locked") != std::string::npos ||
            lower.find("clip-locked") != std::string::npos ||
            lower.find("cliplocked") != std::string::npos ||
            lower.find("mag_locked") != std::string::npos ||
            lower.find("mag-locked") != std::string::npos ||
            lower.find("maglocked") != std::string::npos ||
            lower.find("magazine_locked") != std::string::npos ||
            lower.find("magazine-locked") != std::string::npos ||
            lower.find("magazinelocked") != std::string::npos)
        {
            return MagazineInteractionDelayedSoundStage::Lock;
        }
        if (lower.find("slideback") != std::string::npos ||
            lower.find("slide_back") != std::string::npos ||
            lower.find("slide-back") != std::string::npos ||
            lower.find("boltback") != std::string::npos ||
            lower.find("bolt_back") != std::string::npos ||
            lower.find("bolt-back") != std::string::npos)
        {
            return MagazineInteractionDelayedSoundStage::SlideBack;
        }
        if (lower.find("slideforward") != std::string::npos ||
            lower.find("slide_forward") != std::string::npos ||
            lower.find("slide-forward") != std::string::npos ||
            lower.find("boltforward") != std::string::npos ||
            lower.find("bolt_forward") != std::string::npos ||
            lower.find("bolt-forward") != std::string::npos)
        {
            return MagazineInteractionDelayedSoundStage::SlideForward;
        }
        if (lower.find("ready") != std::string::npos)
            return MagazineInteractionDelayedSoundStage::Ready;
        return MagazineInteractionDelayedSoundStage::Other;
    }

    bool MagazineInteractionSoundLooksNativeReloadTail(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        const MagazineInteractionDelayedSoundStage delayedStage = MagazineInteractionClassifyDelayedSound(sample);
        if (delayedStage != MagazineInteractionDelayedSoundStage::Other ||
            MagazineInteractionSoundStartsInsertTail(sample) ||
            MagazineInteractionSoundLooksClipOut(sample))
        {
            return true;
        }

        const std::string lower = MagazineInteractionLowerSoundSample(sample);
        return lower.find("reload") != std::string::npos ||
            lower.find("clip") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("mag_out") != std::string::npos ||
            lower.find("mag-out") != std::string::npos ||
            lower.find("magout") != std::string::npos ||
            lower.find("magazine") != std::string::npos ||
            lower.find("bolt") != std::string::npos ||
            lower.find("slide") != std::string::npos;
    }

    int MagazineInteractionSoundSpecificityScore(const std::string& sample)
    {
        const std::string lower = MagazineInteractionLowerSoundSample(sample);
        if (lower.find("weapons/rifle/gunother/") != std::string::npos ||
            lower.find("weapons\\rifle\\gunother\\") != std::string::npos ||
            lower.find("weapons/smg/gunother/") != std::string::npos ||
            lower.find("weapons\\smg\\gunother\\") != std::string::npos ||
            lower.find("weapons/pistol/gunother/") != std::string::npos ||
            lower.find("weapons\\pistol\\gunother\\") != std::string::npos ||
            lower.find("weapons/sniper/gunother/") != std::string::npos ||
            lower.find("weapons\\sniper\\gunother\\") != std::string::npos)
        {
            return 0;
        }
        return lower.find("weapon") != std::string::npos ? 1 : 0;
    }

    std::string MagazineInteractionPrepareConsoleSoundSample(const std::string& rawSample)
    {
        size_t start = 0;
        while (start < rawSample.size())
        {
            const char c = rawSample[start];
            if (c == '*' || c == '#' || c == '@' || c == '>' || c == '<' ||
                c == '^' || c == ')' || c == '}' || c == '$' || c == '?' || c == '!')
            {
                ++start;
                continue;
            }
            break;
        }

        std::string sample = rawSample.substr(start);
        if (sample.rfind("sound/", 0) == 0 || sample.rfind("sound\\", 0) == 0)
            sample.erase(0, 6);

        std::string escaped;
        escaped.reserve(sample.size());
        for (char c : sample)
        {
            if (c == '"' || c == '\\')
                escaped.push_back('\\');
            if (c == '\r' || c == '\n' || c == ';')
                continue;
            escaped.push_back(c);
        }
        return escaped;
    }

    std::string MagazineInteractionPrepareSoundSamplePath(const std::string& rawSample)
    {
        size_t start = 0;
        while (start < rawSample.size())
        {
            const char c = rawSample[start];
            if (c == '*' || c == '#' || c == '@' || c == '>' || c == '<' ||
                c == '^' || c == ')' || c == '}' || c == '$' || c == '?' || c == '!')
            {
                ++start;
                continue;
            }
            break;
        }

        std::string sample = rawSample.substr(start);
        if (sample.rfind("sound/", 0) == 0 || sample.rfind("sound\\", 0) == 0)
            sample.erase(0, 6);

        std::string cleaned;
        cleaned.reserve(sample.size());
        for (char c : sample)
        {
            if (c == '\r' || c == '\n' || c == ';' || c == '"')
                continue;
            cleaned.push_back(c);
        }
        return cleaned;
    }

    bool MagazineInteractionPlaySyntheticSound(
        VR* vr,
        const std::string& rawSample,
        std::string& pendingSample,
        std::chrono::steady_clock::time_point& pendingStarted,
        const char* label)
    {
        if (!vr || !vr->m_Game)
            return false;

        const std::string pathSample = MagazineInteractionPrepareSoundSamplePath(rawSample);
        if (pathSample.empty())
            return false;

        const std::string prepared = MagazineInteractionPrepareConsoleSoundSample(rawSample);
        if (prepared.empty())
            return false;

        pendingSample = pathSample;
        pendingStarted = std::chrono::steady_clock::now();
        const std::string command = "playvol \"" + prepared + "\" 1.000";
        vr->m_Game->ClientCmd_Unrestricted(command.c_str());
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] played console synthetic %s sample=%s",
            label ? label : "sound",
            prepared.c_str());
        return true;
    }

    std::string MagazineInteractionNormalizeSoundForCompare(const std::string& rawSample)
    {
        return MagazineInteractionLowerAscii(MagazineInteractionPrepareConsoleSoundSample(rawSample));
    }

    bool MagazineInteractionShouldLetSyntheticSoundPlay(
        std::string& pendingSample,
        std::chrono::steady_clock::time_point& pendingStarted,
        const char* sample,
        std::chrono::steady_clock::time_point now)
    {
        if (!sample || !*sample || pendingSample.empty() ||
            pendingStarted.time_since_epoch().count() == 0)
        {
            return false;
        }

        const float ageSeconds = std::chrono::duration<float>(
            now - pendingStarted).count();
        if (ageSeconds < 0.0f || ageSeconds > 0.12f)
        {
            pendingSample.clear();
            pendingStarted = {};
            return false;
        }

        const std::string pending = MagazineInteractionNormalizeSoundForCompare(
            pendingSample);
        const std::string current = MagazineInteractionNormalizeSoundForCompare(sample);
        if (pending.empty() || current.empty() || pending != current)
            return false;

        pendingSample.clear();
        pendingStarted = {};
        return true;
    }

}

bool VR::CaptureMagazineInteractionSound(int entityIndex, const char* sample, float volume, int flags, int pitch)
{
    if ((m_DualPistolNativeReloadState.load(std::memory_order_acquire) & 1u) != 0u)
        return false;
    constexpr int kSoundChangeVolume = (1 << 0);
    constexpr int kSoundChangePitch = (1 << 1);
    constexpr int kSoundStop = (1 << 2);
    constexpr int kSoundStopLooping = (1 << 5);
    constexpr int kNonStartFlags = kSoundChangeVolume | kSoundChangePitch | kSoundStop | kSoundStopLooping;
    if ((flags & kNonStartFlags) != 0)
        return false;
    if (MagazineInteractionSoundLooksClipEmpty(sample))
        return false;

    if(m_MagazineInteractionEnabled)
    {
        if (MagazineInteractionSoundLooksShotgunFire(sample))
            return false;
        if (MagazineInteractionSoundLooksShotgunPump(sample) && !m_ManualPumpEnabled)
            return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool nativeReloadSuppressed =
        m_MagazineInteractionNativeReloadSuppressUntil.time_since_epoch().count() != 0 &&
        now <= m_MagazineInteractionNativeReloadSuppressUntil;
    const bool nativeReloadSound = MagazineInteractionSoundLooksNativeReloadTail(sample);
    const bool waitingForInsertTail =
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
    const bool removingOldMagazine =
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine;
    const bool waitingForBoltAction =
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
        m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting;
    const bool suppressingNativeReload =
        removingOldMagazine ||
        waitingForInsertTail ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
        waitingForBoltAction ||
        nativeReloadSuppressed;
    const bool shotgunPhysicalSoundState =
        m_MagazineInteractionShotgunShellMode && (waitingForInsertTail || waitingForBoltAction);
    const bool manualPumpSound = m_ManualPumpEnabled &&
        IsMagazineInteractionShotgunServerHookActive(m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed)) &&
        MagazineInteractionSoundLooksShotgunPump(sample);
    if (!sample || !*sample || !m_Game ||
        (!m_MagazineInteractionReloadTriggered && !shotgunPhysicalSoundState && !nativeReloadSuppressed && !manualPumpSound) ||
        (!suppressingNativeReload && !manualPumpSound))
    {
        return false;
    }

    const int localPlayerIndex = (m_Game->m_EngineClient != nullptr)
        ? m_Game->m_EngineClient->GetLocalPlayer()
        : -1;
    const bool fromConsoleOrWorld = entityIndex <= 0;
    const bool fromViewmodel = m_MagazineInteractionViewmodelEntityIndex > 0 &&
        entityIndex == m_MagazineInteractionViewmodelEntityIndex;
    if (m_MagazineInteractionWeaponEntityIndex <= 0 && m_MagazineInteractionWeapon)
        m_MagazineInteractionWeaponEntityIndex =
            MagazineInteractionFindClientEntityIndex(m_Game, m_MagazineInteractionWeapon);
    const bool fromActiveWeapon = m_MagazineInteractionWeaponEntityIndex > 0 &&
        entityIndex == m_MagazineInteractionWeaponEntityIndex;
    const bool fromLocalWeaponPath =
        (fromConsoleOrWorld || entityIndex == localPlayerIndex || fromActiveWeapon) &&
        MagazineInteractionSoundLooksWeaponRelated(sample);
    const bool fromShotgunReloadPath =
        m_MagazineInteractionShotgunShellMode && fromLocalWeaponPath;
    if (!fromViewmodel &&
        !fromLocalWeaponPath &&
        !fromShotgunReloadPath &&
        !(nativeReloadSuppressed && nativeReloadSound))
    {
        return false;
    }

    if (fromConsoleOrWorld && MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticClipOutSample,
        m_MagazineInteractionSyntheticClipOutStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic clip-out play sample=%s",
            sample);
        return false;
    }

    if (fromConsoleOrWorld && MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticClipInSample,
        m_MagazineInteractionSyntheticClipInStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic clip-in play sample=%s",
            sample);
        return false;
    }
    if (fromConsoleOrWorld && MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticBoltBackSample,
        m_MagazineInteractionSyntheticBoltBackStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic bolt-back play sample=%s",
            sample);
        return false;
    }
    if (fromConsoleOrWorld && MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticBoltForwardSample,
        m_MagazineInteractionSyntheticBoltForwardStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic bolt-forward play sample=%s",
            sample);
        return false;
    }

    if (manualPumpSound &&
        (fromViewmodel || fromLocalWeaponPath))
    {
        Game::logMsg("[VR][ManualPump] suppressed native automatic pump sound sample=%s", sample);
        return true;
    }

    const bool allowVisibleShotgunNativeReloadSound =
        m_MagazineInteractionShotgunShellMode &&
        !ShouldFreezeMagazineInteractionViewmodel();
    if (!nativeReloadSuppressed && allowVisibleShotgunNativeReloadSound)
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let visible native shotgun sound play sample=%s state=%d",
            sample,
            static_cast<int>(m_MagazineInteractionState));
        return false;
    }

    if (nativeReloadSuppressed)
    {
        if (!nativeReloadSound)
            return false;
    }
    else if (!MagazineInteractionSoundLooksWeaponRelated(sample) &&
        !MagazineInteractionSoundStartsInsertTail(sample) &&
        !MagazineInteractionSoundLooksClipOut(sample))
    {
        return false;
    }

    if (nativeReloadSuppressed && nativeReloadSound)
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] swallowed native reload sound during suppress window sample=%s state=%d",
            sample,
            static_cast<int>(m_MagazineInteractionState));
        return true;
    }

    if (!MagazineInteractionSoundMatchesCurrentWeapon(this, sample))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] swallowed mismatched native reload sound weaponId=%d model=%s sample=%s state=%d",
            m_MagazineInteractionWeaponId,
            m_MagazineInteractionMagazineModelName.c_str(),
            sample,
            static_cast<int>(m_MagazineInteractionState));
        return true;
    }

    const MagazineInteractionDelayedSoundStage delayedStage = MagazineInteractionClassifyDelayedSound(sample);
    if (delayedStage == MagazineInteractionDelayedSoundStage::SlideBack)
    {
        const int score = MagazineInteractionSoundSpecificityScore(sample);
        if (score >= m_MagazineInteractionCapturedBoltBackSoundScore)
        {
            m_MagazineInteractionCapturedBoltBackSample = sample;
            m_MagazineInteractionCapturedBoltBackSoundScore = score;
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] captured native bolt-back sample score=%d sample=%s",
                score,
                sample);
        }
    }
    else if (delayedStage == MagazineInteractionDelayedSoundStage::SlideForward)
    {
        const int score = MagazineInteractionSoundSpecificityScore(sample);
        if (score >= m_MagazineInteractionCapturedBoltForwardSoundScore)
        {
            m_MagazineInteractionCapturedBoltForwardSample = sample;
            m_MagazineInteractionCapturedBoltForwardSoundScore = score;
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] captured native bolt-forward sample score=%d sample=%s",
                score,
                sample);
        }
    }

    Game::logMsg(
        "[VR][MagazineInteraction][Audio] swallowed native reload sound sample=%s state=%d",
        sample,
        static_cast<int>(m_MagazineInteractionState));
    return true;
}
