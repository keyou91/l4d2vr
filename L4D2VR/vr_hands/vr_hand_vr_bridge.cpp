#include "vr.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_system.h"
#include "vr_hand_math.h"
#include "vr_hand_vm_pose.h"

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
        vr::VRActionHandle_t& action,
        const char* actionPath,
        std::array<float, 5>& outCurls,
        const char* label)
    {
        outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        if (!input || !actionPath || actionPath[0] == '\0')
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
        if (input->GetSkeletalActionData(action, &actionData, sizeof(actionData)) != vr::VRInputError_None ||
            !actionData.bActive ||
            actionData.activeOrigin == vr::k_ulInvalidInputValueHandle)
        {
            return false;
        }

        vr::VRSkeletalSummaryData_t summary{};
        if (!TryReadOpenVRSkeletalSummary(
                input,
                action,
                vr::VRSummaryType_FromAnimation,
                summary,
                label))
        {
            return false;
        }

        for (int finger = 0; finger < 5; ++finger)
            outCurls[static_cast<size_t>(finger)] = std::clamp(summary.flFingerCurl[finger], 0.0f, 1.0f);

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
        const Vector delta = worldPoint - box.origin;
        const Vector local(
            VrHandMath::Dot(delta, box.axisX),
            VrHandMath::Dot(delta, box.axisY),
            VrHandMath::Dot(delta, box.axisZ));
        const Vector closest(
            std::clamp(local.x, box.mins.x, box.maxs.x),
            std::clamp(local.y, box.mins.y, box.maxs.y),
            std::clamp(local.z, box.mins.z, box.maxs.z));
        return (local - closest).Length();
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
        if (vr && vr->m_LeftHanded)
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

        const Vector bodyOrigin = vr->m_HmdPosAbs
            + bodyForward * (vr->m_InventoryBodyOriginOffset.x * vr->m_VRScale)
            + bodyRight * (vr->m_InventoryBodyOriginOffset.y * vr->m_VRScale)
            + worldUp * (vr->m_InventoryBodyOriginOffset.z * vr->m_VRScale);
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
        outBox.axisY = bodyRight;
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

        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
            vr,
            vr->m_LeftControllerPosAbs,
            vr->m_LeftControllerForward,
            vr->m_LeftControllerRight,
            vr->m_LeftControllerUp);
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

        const VrHandMatrix4 handLocal = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            vr->m_NativeViewmodelLeftHandPoseOffsetMeters,
            vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg);
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
        if (vr)
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
        if (vr->GetMagazineInteractionNativeLeftWristAnchor(wristWorld))
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
        Vector fallback = VrHandMath::Normalize(fallbackAxis);
        if (!vr || !MagazineInteractionHasBoltPullAxisProfileOverride(vr))
            return fallback;

        Vector viewmodelBack = vr->m_ViewmodelForward * -1.0f;
        viewmodelBack.z = 0.0f;
        viewmodelBack = VrHandMath::Normalize(viewmodelBack);
        if (viewmodelBack.Length() > 0.0001f)
            return viewmodelBack;

        Vector hmdBack = vr->m_HmdForward * -1.0f;
        hmdBack.z = 0.0f;
        hmdBack = VrHandMath::Normalize(hmdBack);
        if (hmdBack.Length() > 0.0001f)
            return hmdBack;

        return fallback;
    }

    Vector MagazineInteractionAlignBoltVisualAxisToInputAxis(
        const VR* vr,
        const Vector& visualAxis,
        const Vector& inputAxis)
    {
        Vector visual = VrHandMath::Normalize(visualAxis);
        if (!vr || !MagazineInteractionHasBoltPullAxisProfileOverride(vr))
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
        return std::clamp(value, 0.0f, 0.25f);
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
        VrHandMatrix4& outWorld)
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
        if (!MagazineInteractionBuildCurrentViewmodelModelBasisWorld(vr, currentAnchorWorld))
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
        const Vector leftControllerPos = mutableVr->GetLeftControllerAbsPos();
        const QAngle leftControllerAng = mutableVr->GetLeftControllerAbsAngle();
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
        const Vector bodyOrigin = hmdPosAbs
            + bodyForward * (vr->m_InventoryBodyOriginOffset.x * vr->m_VRScale)
            + bodyRight * (vr->m_InventoryBodyOriginOffset.y * vr->m_VRScale)
            + worldUp * (vr->m_InventoryBodyOriginOffset.z * vr->m_VRScale);
        const Vector pickupOffsetMeters = MagazineInteractionFreshMagazinePickupOffsetMeters(vr);
        const Vector pickup = bodyOrigin
            + bodyForward * (pickupOffsetMeters.x * vr->m_VRScale)
            + bodyRight * (pickupOffsetMeters.y * vr->m_VRScale)
            + worldUp * (pickupOffsetMeters.z * vr->m_VRScale);

        Vector viewmodelPickup = pickup;
        Vector reprojectedPickup{};
        if (MagazineInteractionReprojectScenePointToViewmodelLayer(vr, pickup, reprojectedPickup))
            viewmodelPickup = reprojectedPickup;

        outWorld = MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(vr, viewmodelPickup);
        return MagazineInteractionMatrixLooksRenderable(outWorld);
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
        case C_WeaponCSBase::WeaponID::M60:
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

    bool MagazineInteractionClipValueLooksPlausible(int value)
    {
        return value >= 0 && value <= 250;
    }

    int MagazineInteractionScoreServerClipOffset(
        void* serverWeapon,
        int offset,
        int expectedPriorClip,
        int ammoType)
    {
        int clipValue = -1;
        if (!MagazineInteractionTryReadValue(serverWeapon, offset, clipValue) ||
            !MagazineInteractionClipValueLooksPlausible(clipValue))
        {
            return -1;
        }

        int score = 0;
        if (expectedPriorClip >= 0)
        {
            if (clipValue != expectedPriorClip)
                return -1;
            score += 100;
        }
        else
        {
            score += 10;
        }

        if (ammoType >= 0 && ammoType < 32)
        {
            int serverAmmoType = -1;
            if (MagazineInteractionTryReadValue(serverWeapon, offset - static_cast<int>(sizeof(int)) * 2, serverAmmoType) &&
                serverAmmoType == ammoType)
            {
                score += 40;
            }
        }

        if (offset == 0x1414)
            score += 20;
        return score;
    }

    int MagazineInteractionFindServerClipOffset(
        void* serverWeapon,
        int knownOffset,
        int expectedPriorClip,
        int ammoType)
    {
        if (!serverWeapon)
            return 0;

        if (knownOffset > 0)
        {
            const int knownScore = MagazineInteractionScoreServerClipOffset(
                serverWeapon,
                knownOffset,
                expectedPriorClip,
                ammoType);
            if (knownScore >= 0)
                return knownOffset;
        }

        static constexpr int kCandidateOffsets[] = {
            0x1414, // observed server CBaseCombatWeapon::m_iClip1 on the existing shotgun path
            VR::kClip1Offset
        };
        int bestOffset = 0;
        int bestScore = -1;
        for (int offset : kCandidateOffsets)
        {
            const int score = MagazineInteractionScoreServerClipOffset(
                serverWeapon,
                offset,
                expectedPriorClip,
                ammoType);
            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = offset;
            }
        }

        if (bestOffset != 0 || expectedPriorClip < 0)
            return bestOffset;

        for (int offset = 0x800; offset <= 0x1800; offset += static_cast<int>(sizeof(int)))
        {
            const int score = MagazineInteractionScoreServerClipOffset(
                serverWeapon,
                offset,
                expectedPriorClip,
                ammoType);
            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = offset;
            }
        }
        return bestOffset;
    }

    bool MagazineInteractionReserveValueLooksPlausible(int value)
    {
        return value >= 0 && value <= 5000;
    }

    int MagazineInteractionScoreServerReserveOffset(
        void* serverPlayer,
        int elementOffset,
        int ammoType,
        int targetReserve,
        int expectedPriorReserve)
    {
        if (!serverPlayer || elementOffset <= 0 || ammoType < 0 || ammoType >= 32)
            return -1;

        int reserveValue = -1;
        if (!MagazineInteractionTryReadValue(serverPlayer, elementOffset, reserveValue) ||
            !MagazineInteractionReserveValueLooksPlausible(reserveValue))
        {
            return -1;
        }

        int score = -1;
        if (expectedPriorReserve >= 0)
        {
            if (reserveValue == expectedPriorReserve)
                score = 120;
            else if (targetReserve >= 0 && reserveValue == targetReserve)
                score = 105;
            else if (targetReserve >= 0 &&
                reserveValue >= std::min(targetReserve, expectedPriorReserve) &&
                reserveValue <= std::max(targetReserve, expectedPriorReserve))
            {
                score = 70;
            }
        }
        else if (targetReserve >= 0 && reserveValue == targetReserve)
        {
            score = 80;
        }
        if (score < 0)
            return -1;

        const int baseOffset = elementOffset - ammoType * static_cast<int>(sizeof(int));
        int plausibleNeighbors = 0;
        for (int index = 0; index < 32; ++index)
        {
            int neighbor = -1;
            if (MagazineInteractionTryReadValue(
                serverPlayer,
                baseOffset + index * static_cast<int>(sizeof(int)),
                neighbor) &&
                MagazineInteractionReserveValueLooksPlausible(neighbor))
            {
                ++plausibleNeighbors;
            }
        }
        if (plausibleNeighbors >= 8)
            score += std::min(plausibleNeighbors, 16);

        if (baseOffset == VR::kAmmoArrayOffset ||
            baseOffset == VR::kAmmoArrayOffset + 0xA90 ||
            baseOffset == VR::kAmmoArrayOffset + 0xAF0)
        {
            score += 25;
        }
        return score;
    }

    int MagazineInteractionFindServerReserveOffset(
        void* serverPlayer,
        int knownOffset,
        int ammoType,
        int targetReserve,
        int expectedPriorReserve)
    {
        if (!serverPlayer || ammoType < 0 || ammoType >= 32 || targetReserve < 0)
            return 0;

        if (knownOffset > 0)
        {
            const int knownScore = MagazineInteractionScoreServerReserveOffset(
                serverPlayer,
                knownOffset,
                ammoType,
                targetReserve,
                expectedPriorReserve);
            if (knownScore >= 0)
                return knownOffset;
        }

        static constexpr int kCandidateBaseOffsets[] = {
            VR::kAmmoArrayOffset,
            VR::kAmmoArrayOffset + 0xA90,
            VR::kAmmoArrayOffset + 0xAF0
        };

        int bestOffset = 0;
        int bestScore = -1;
        for (int baseOffset : kCandidateBaseOffsets)
        {
            const int elementOffset = baseOffset + ammoType * static_cast<int>(sizeof(int));
            const int score = MagazineInteractionScoreServerReserveOffset(
                serverPlayer,
                elementOffset,
                ammoType,
                targetReserve,
                expectedPriorReserve);
            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = elementOffset;
            }
        }

        if (bestOffset != 0 || expectedPriorReserve <= 0)
            return bestOffset;

        for (int elementOffset = 0xC00; elementOffset <= 0x2400; elementOffset += static_cast<int>(sizeof(int)))
        {
            const int score = MagazineInteractionScoreServerReserveOffset(
                serverPlayer,
                elementOffset,
                ammoType,
                targetReserve,
                expectedPriorReserve);
            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = elementOffset;
            }
        }

        return bestScore >= 100 ? bestOffset : 0;
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
    if (!m_MagazineInteractionEnabled &&
        !m_MagazineBoxDebugEnabled &&
        !m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed) &&
        !(m_IsVREnabled && (m_VrHandsEnabled || m_NativeViewmodelHandsOnly)))
    {
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
    VrHandMatrix4 viewmodelAnchorWorld{};
    if (MagazineInteractionBuildCurrentViewmodelModelBasisWorld(this, viewmodelAnchorWorld))
    {
        snapshot.viewmodelAnchorBasisValid = true;
        snapshot.viewmodelAnchorOrigin = MagazineInteractionMatrixOrigin(viewmodelAnchorWorld);
        snapshot.viewmodelAnchorAxisX = MagazineInteractionMatrixAxis(viewmodelAnchorWorld, 0);
        snapshot.viewmodelAnchorAxisY = MagazineInteractionMatrixAxis(viewmodelAnchorWorld, 1);
        snapshot.viewmodelAnchorAxisZ = MagazineInteractionMatrixAxis(viewmodelAnchorWorld, 2);
    }
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
    if (!m_MagazineInteractionEnabled &&
        !m_MagazineBoxDebugEnabled &&
        !m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed))
    {
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
    if (!m_MagazineInteractionBoxValid)
        return false;
    outSnapshot = m_MagazineInteractionBox;
    return true;
}

bool VR::GetMagazineInteractionBoltBox(MagazineInteractionBoxSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
    if (!m_MagazineInteractionBoltBoxValid)
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

bool VR::IsMagazineInteractionLeftHandActive() const
{
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
    if (!m_MagazineInteractionEnabled ||
        !m_IsVREnabled ||
        !m_MagazineInteractionOneInChamber ||
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

    const bool forceShotgunMagazineInteraction =
        MagazineInteractionWeaponUsesShotgunShells(activeWeaponId) &&
        m_MagazineInteractionShotgunShellMode;
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
            "[VR][MagazineInteraction] suppressing empty-clip automatic reload weaponId=%d clip=%d; physical magazine reload required",
            static_cast<int>(activeWeaponId),
            activeClip);
    }
    return true;
}

bool VR::IsMagazineInteractionBlockingFire() const
{
    if (m_MagazineInteractionShotgunShellMode)
    {
        return m_MagazineInteractionReloadTriggered ||
            m_MagazineInteractionShotgunShellsLoadedThisSession > 0 ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
            m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting;
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
                m_MagazineInteractionShotgunShellsLoadedThisSession > 0);
    }

    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
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
    if (weaponId <= 0 ||
        m_MagazineInteractionShotgunServerHookWeaponId != weaponId ||
        m_MagazineInteractionShotgunServerHookLastSeen.time_since_epoch().count() == 0)
    {
        return false;
    }

    constexpr float kServerHookFreshSeconds = 1.50f;
    return std::chrono::duration<float>(
        std::chrono::steady_clock::now() -
        m_MagazineInteractionShotgunServerHookLastSeen).count() <= kServerHookFreshSeconds;
}

void VR::QueueMagazineInteractionServerClipCommit(
    int targetClip,
    int ammoType,
    int targetReserve,
    int expectedPriorClip,
    int expectedPriorReserve,
    const char*,
    float holdSeconds)
{
    if (targetClip < 0 ||
        !MagazineInteractionWeaponUsesDetachableMagazine(
            static_cast<C_WeaponCSBase::WeaponID>(m_MagazineInteractionWeaponId)))
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    m_MagazineInteractionServerClipCommitPending = true;
    m_MagazineInteractionServerClipCommitted = false;
    m_MagazineInteractionServerReserveCommitted =
        targetReserve < 0 || ammoType < 0 || ammoType >= 32;
    m_MagazineInteractionServerClipTarget = targetClip;
    m_MagazineInteractionServerClipAmmoType = ammoType;
    m_MagazineInteractionServerClipTargetReserve = targetReserve;
    m_MagazineInteractionServerClipWeaponId = m_MagazineInteractionWeaponId;
    m_MagazineInteractionServerClipExpectedPrior = expectedPriorClip;
    m_MagazineInteractionServerReserveExpectedPrior = expectedPriorReserve;
    m_MagazineInteractionServerClipCommitUntil =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(std::max(0.05f, holdSeconds)));
}

bool VR::TryApplyMagazineInteractionServerClipCommit(
    void* serverWeapon,
    int serverWeaponId,
    void* serverPlayer)
{
    if (!serverWeapon)
        return false;

    MarkMagazineInteractionServerHookSeen(serverWeaponId);

    if (!m_MagazineInteractionServerClipCommitPending)
        return false;

    auto clearPendingCommit = [&]()
    {
        m_MagazineInteractionServerClipCommitPending = false;
        m_MagazineInteractionServerClipCommitted = false;
        m_MagazineInteractionServerReserveCommitted = false;
        m_MagazineInteractionServerClipTarget = -1;
        m_MagazineInteractionServerClipAmmoType = -1;
        m_MagazineInteractionServerClipTargetReserve = -1;
        m_MagazineInteractionServerClipWeaponId = 0;
        m_MagazineInteractionServerClipExpectedPrior = -1;
        m_MagazineInteractionServerReserveExpectedPrior = -1;
        m_MagazineInteractionServerClipCommitUntil = {};
    };

    const auto now = std::chrono::steady_clock::now();
    if (m_MagazineInteractionServerClipCommitUntil.time_since_epoch().count() != 0 &&
        now > m_MagazineInteractionServerClipCommitUntil)
    {
        clearPendingCommit();
        return false;
    }

    const int currentClientWeaponId =
        m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
    if (m_MagazineInteractionServerClipWeaponId != 0 &&
        serverWeaponId != m_MagazineInteractionServerClipWeaponId &&
        currentClientWeaponId != m_MagazineInteractionServerClipWeaponId)
    {
        return false;
    }

    const int targetClip = m_MagazineInteractionServerClipTarget;
    const int ammoType = m_MagazineInteractionServerClipAmmoType;
    const int targetReserve = m_MagazineInteractionServerClipTargetReserve;
    const int expectedPriorClip = m_MagazineInteractionServerClipExpectedPrior;
    const int expectedPriorReserve = m_MagazineInteractionServerReserveExpectedPrior;
    bool wroteAny = false;

    const int serverClipOffset = MagazineInteractionFindServerClipOffset(
        serverWeapon,
        m_MagazineInteractionServerClipOffset,
        expectedPriorClip,
        ammoType);
    if (serverClipOffset > 0 && serverClipOffset != m_MagazineInteractionServerClipOffset)
    {
        Game::logMsg(
            "[VR][MagazineInteraction] resolved server clip offset=0x%X weaponId=%d expectedPrior=%d ammoType=%d",
            serverClipOffset,
            serverWeaponId,
            expectedPriorClip,
            ammoType);
        m_MagazineInteractionServerClipOffset = serverClipOffset;
    }

    int serverClip = -1;
    if (targetClip >= 0 &&
        serverClipOffset > 0 &&
        MagazineInteractionTryReadValue(serverWeapon, serverClipOffset, serverClip))
    {
        if (serverClip == targetClip)
        {
            m_MagazineInteractionServerClipCommitted = true;
        }
        else if (MagazineInteractionTryWriteValue<int>(serverWeapon, serverClipOffset, targetClip))
        {
            m_MagazineInteractionServerClipCommitted = true;
            wroteAny = true;
            Game::logMsg(
                "[VR][MagazineInteraction] server clip committed weaponId=%d offset=0x%X old=%d target=%d expectedPrior=%d",
                serverWeaponId,
                serverClipOffset,
                serverClip,
                targetClip,
                expectedPriorClip);
        }
    }
    else if (targetClip >= 0)
    {
        static std::chrono::steady_clock::time_point s_lastServerClipOffsetFailLog{};
        if (s_lastServerClipOffsetFailLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastServerClipOffsetFailLog).count() >= 0.50f)
        {
            s_lastServerClipOffsetFailLog = now;
            Game::logMsg(
                "[VR][MagazineInteraction] server clip commit pending but clip offset unresolved weaponId=%d target=%d expectedPrior=%d ammoType=%d",
                serverWeaponId,
                targetClip,
                expectedPriorClip,
                ammoType);
        }
    }

    if (!m_MagazineInteractionServerReserveCommitted &&
        serverPlayer &&
        ammoType >= 0 &&
        ammoType < 32 &&
        targetReserve >= 0)
    {
        const int reserveOffset = MagazineInteractionFindServerReserveOffset(
            serverPlayer,
            m_MagazineInteractionServerReserveOffset,
            ammoType,
            targetReserve,
            expectedPriorReserve);
        if (reserveOffset > 0 && reserveOffset != m_MagazineInteractionServerReserveOffset)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] resolved server reserve offset=0x%X weaponId=%d ammoType=%d expectedPrior=%d target=%d",
                reserveOffset,
                serverWeaponId,
                ammoType,
                expectedPriorReserve,
                targetReserve);
            m_MagazineInteractionServerReserveOffset = reserveOffset;
        }

        int serverReserve = -1;
        if (reserveOffset > 0 &&
            MagazineInteractionTryReadValue(serverPlayer, reserveOffset, serverReserve))
        {
            if (serverReserve == targetReserve)
            {
                m_MagazineInteractionServerReserveCommitted = true;
            }
            else if (MagazineInteractionTryWriteValue<int>(serverPlayer, reserveOffset, targetReserve))
            {
                m_MagazineInteractionServerReserveCommitted = true;
                wroteAny = true;
                Game::logMsg(
                    "[VR][MagazineInteraction] server reserve committed weaponId=%d offset=0x%X old=%d target=%d expectedPrior=%d",
                    serverWeaponId,
                    reserveOffset,
                    serverReserve,
                    targetReserve,
                    expectedPriorReserve);
            }
        }
        else
        {
            static std::chrono::steady_clock::time_point s_lastServerReserveOffsetFailLog{};
            if (s_lastServerReserveOffsetFailLog.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - s_lastServerReserveOffsetFailLog).count() >= 0.50f)
            {
                s_lastServerReserveOffsetFailLog = now;
                Game::logMsg(
                    "[VR][MagazineInteraction] server reserve commit pending but reserve offset unresolved weaponId=%d ammoType=%d target=%d expectedPrior=%d",
                    serverWeaponId,
                    ammoType,
                    targetReserve,
                    expectedPriorReserve);
            }
        }
    }

    constexpr int kServerBaseWeaponInReloadOffset = 0x144D;          // DT_BaseCombatWeapon::m_bInReload
    constexpr int kServerL4DWeaponPartialReloadStageOffset = 0x17D8; // DT_WeaponCSBase::m_partialReloadStage
    constexpr int kServerL4DWeaponReloadFromEmptyOffset = 0x17DF;    // DT_WeaponCSBase::m_reloadFromEmpty
    const bool wroteServerInReload =
        MagazineInteractionTryWriteValue<unsigned char>(serverWeapon, kServerBaseWeaponInReloadOffset, 0);
    const bool wroteServerPartialReloadStage =
        MagazineInteractionTryWriteValue<unsigned char>(serverWeapon, kServerL4DWeaponPartialReloadStageOffset, 0);
    const bool wroteServerReloadFromEmpty =
        MagazineInteractionTryWriteValue<unsigned char>(serverWeapon, kServerL4DWeaponReloadFromEmptyOffset, 0);
    wroteAny = wroteAny || wroteServerInReload || wroteServerPartialReloadStage || wroteServerReloadFromEmpty;

    if (m_MagazineInteractionServerClipCommitted &&
        m_MagazineInteractionServerReserveCommitted)
    {
        clearPendingCommit();
        return true;
    }

    return wroteAny;
}

void VR::QueueMagazineInteractionShotgunServerReloadAbort(const char*)
{
    if (!m_MagazineInteractionShotgunShellMode)
        return;

    const auto now = std::chrono::steady_clock::now();
    m_MagazineInteractionShotgunServerReloadAbortPending = true;
    m_MagazineInteractionShotgunServerReloadAbortUntil =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(0.50f));
}

void VR::QueueMagazineInteractionShotgunDirectShellCommit(
    int targetClip,
    int ammoType,
    int targetReserve,
    int expectedPriorReserve,
    const char* reason)
{
    if (!m_MagazineInteractionShotgunShellMode || targetClip < 0)
        return;

    const auto now = std::chrono::steady_clock::now();
    m_MagazineInteractionShotgunDirectShellCommitPending = true;
    m_MagazineInteractionShotgunDirectShellServerClipCommitted = false;
    m_MagazineInteractionShotgunDirectShellServerReserveCommitted =
        targetReserve < 0 || ammoType < 0 || ammoType >= 32;
    m_MagazineInteractionShotgunDirectShellTargetClip = targetClip;
    m_MagazineInteractionShotgunDirectShellAmmoType = ammoType;
    m_MagazineInteractionShotgunDirectShellTargetReserve = targetReserve;
    m_MagazineInteractionShotgunDirectShellExpectedPriorReserve = expectedPriorReserve;
    m_MagazineInteractionShotgunDirectShellWeaponId = m_MagazineInteractionWeaponId;
    m_MagazineInteractionShotgunDirectShellCommitUntil =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(1.00f));
    m_MagazineInteractionShotgunLastInterruptedClip = targetClip;
    QueueMagazineInteractionShotgunServerReloadAbort(reason);
}

bool VR::TryApplyMagazineInteractionShotgunServerReloadAbort(
    void* serverWeapon,
    int serverWeaponId,
    void* serverPlayer)
{
    if (!serverWeapon)
        return false;

    const auto now = std::chrono::steady_clock::now();
    const bool serverWeaponIsShotgun =
        MagazineInteractionWeaponUsesShotgunShells(static_cast<C_WeaponCSBase::WeaponID>(serverWeaponId));
    if (serverWeaponIsShotgun)
        MarkMagazineInteractionShotgunServerHookSeen(serverWeaponId);

    bool abortPending = m_MagazineInteractionShotgunServerReloadAbortPending;
    bool directCommitPending = m_MagazineInteractionShotgunDirectShellCommitPending;
    if (!abortPending && !directCommitPending)
        return false;

    if (abortPending &&
        m_MagazineInteractionShotgunServerReloadAbortUntil.time_since_epoch().count() != 0 &&
        now > m_MagazineInteractionShotgunServerReloadAbortUntil)
    {
        m_MagazineInteractionShotgunServerReloadAbortPending = false;
        m_MagazineInteractionShotgunServerReloadAbortUntil = {};
        abortPending = false;
    }
    if (directCommitPending &&
        m_MagazineInteractionShotgunDirectShellCommitUntil.time_since_epoch().count() != 0 &&
        now > m_MagazineInteractionShotgunDirectShellCommitUntil)
    {
        m_MagazineInteractionShotgunDirectShellCommitPending = false;
        m_MagazineInteractionShotgunDirectShellServerClipCommitted = false;
        m_MagazineInteractionShotgunDirectShellServerReserveCommitted = false;
        m_MagazineInteractionShotgunDirectShellTargetClip = -1;
        m_MagazineInteractionShotgunDirectShellAmmoType = -1;
        m_MagazineInteractionShotgunDirectShellTargetReserve = -1;
        m_MagazineInteractionShotgunDirectShellExpectedPriorReserve = -1;
        m_MagazineInteractionShotgunDirectShellWeaponId = 0;
        m_MagazineInteractionShotgunDirectShellCommitUntil = {};
        directCommitPending = false;
    }
    if (!abortPending && !directCommitPending)
        return false;

    if (!serverWeaponIsShotgun)
        return false;

    if (directCommitPending &&
        m_MagazineInteractionShotgunDirectShellWeaponId != 0 &&
        serverWeaponId != m_MagazineInteractionShotgunDirectShellWeaponId)
        return false;

    bool directCommitAppliedOrAlreadyCurrent = false;
    bool directCommitWroteServerClip = false;
    if (directCommitPending)
    {
        constexpr int kServerClip1Offset = 0x1414;              // Server CBaseCombatWeapon::m_iClip1
        const int targetClip = m_MagazineInteractionShotgunDirectShellTargetClip;
        const int ammoType = m_MagazineInteractionShotgunDirectShellAmmoType;
        const int targetReserve = m_MagazineInteractionShotgunDirectShellTargetReserve;
        const int expectedPriorReserve = m_MagazineInteractionShotgunDirectShellExpectedPriorReserve;
        int oldServerClip = -1;
        bool wroteServerClip = false;
        bool readServerClip = MagazineInteractionTryReadValue(serverWeapon, kServerClip1Offset, oldServerClip);

        if (!m_MagazineInteractionShotgunDirectShellServerClipCommitted &&
            targetClip >= 0 &&
            readServerClip)
        {
            if (oldServerClip >= targetClip)
            {
                m_MagazineInteractionShotgunDirectShellServerClipCommitted = true;
            }
            else
            {
                wroteServerClip =
                    MagazineInteractionTryWriteValue<int>(serverWeapon, kServerClip1Offset, targetClip);
                if (wroteServerClip)
                {
                    m_MagazineInteractionShotgunDirectShellServerClipCommitted = true;
                    directCommitWroteServerClip = true;
                }
            }
        }

        if (!m_MagazineInteractionShotgunDirectShellServerReserveCommitted &&
            serverPlayer &&
            ammoType >= 0 &&
            ammoType < 32 &&
            targetReserve >= 0)
        {
            const int reserveOffset = MagazineInteractionFindServerReserveOffset(
                serverPlayer,
                m_MagazineInteractionServerReserveOffset,
                ammoType,
                targetReserve,
                expectedPriorReserve);
            if (reserveOffset > 0 && reserveOffset != m_MagazineInteractionServerReserveOffset)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] resolved shotgun server reserve offset=0x%X weaponId=%d ammoType=%d expectedPrior=%d target=%d",
                    reserveOffset,
                    serverWeaponId,
                    ammoType,
                    expectedPriorReserve,
                    targetReserve);
                m_MagazineInteractionServerReserveOffset = reserveOffset;
            }

            int serverReserve = -1;
            if (reserveOffset > 0 &&
                MagazineInteractionTryReadValue(serverPlayer, reserveOffset, serverReserve))
            {
                if (serverReserve <= targetReserve)
                {
                    m_MagazineInteractionShotgunDirectShellServerReserveCommitted = true;
                }
                else
                {
                    if (MagazineInteractionTryWriteValue<int>(serverPlayer, reserveOffset, targetReserve))
                        m_MagazineInteractionShotgunDirectShellServerReserveCommitted = true;
                }
            }
            else
            {
                static std::chrono::steady_clock::time_point s_lastShotgunServerReserveOffsetFailLog{};
                if (s_lastShotgunServerReserveOffsetFailLog.time_since_epoch().count() == 0 ||
                    std::chrono::duration<float>(now - s_lastShotgunServerReserveOffsetFailLog).count() >= 0.50f)
                {
                    s_lastShotgunServerReserveOffsetFailLog = now;
                    Game::logMsg(
                        "[VR][MagazineInteraction] shotgun direct shell reserve commit pending but reserve offset unresolved weaponId=%d ammoType=%d target=%d expectedPrior=%d",
                        serverWeaponId,
                        ammoType,
                        targetReserve,
                        expectedPriorReserve);
                }
            }
        }

        if (m_MagazineInteractionShotgunDirectShellServerClipCommitted &&
            m_MagazineInteractionShotgunDirectShellServerReserveCommitted)
        {
            m_MagazineInteractionShotgunDirectShellCommitPending = false;
            m_MagazineInteractionShotgunDirectShellCommitUntil = {};
            m_MagazineInteractionShotgunDirectShellTargetClip = -1;
            m_MagazineInteractionShotgunDirectShellAmmoType = -1;
            m_MagazineInteractionShotgunDirectShellTargetReserve = -1;
            m_MagazineInteractionShotgunDirectShellExpectedPriorReserve = -1;
            m_MagazineInteractionShotgunDirectShellWeaponId = 0;
            directCommitAppliedOrAlreadyCurrent = true;
        }
    }

    const bool shouldClearServerReloadState =
        abortPending ||
        directCommitWroteServerClip ||
        directCommitAppliedOrAlreadyCurrent;
    if (!shouldClearServerReloadState)
        return directCommitPending;

    constexpr int kBaseWeaponInReloadOffset = 0x144D;             // DT_BaseCombatWeapon::m_bInReload
    constexpr int kL4DWeaponPartialReloadStageOffset = 0x17D8;    // DT_WeaponCSBase::m_partialReloadStage
    constexpr int kL4DWeaponReloadFromEmptyOffset = 0x17DF;       // DT_WeaponCSBase::m_reloadFromEmpty
    constexpr int kShotgunReloadStateOffset = 0x17F0;             // DT_BaseShotgun::m_reloadState
    constexpr int kShotgunReloadNumShellsOffset = 0x17F4;         // DT_BaseShotgun::m_reloadNumShells
    constexpr int kShotgunReloadAnimStateOffset = 0x1808;         // DT_BaseShotgun::m_reloadAnimState
    constexpr int kShotgunShellsInsertedOffset = 0x180C;          // DT_BaseShotgun::m_shellsInserted
    const bool wroteReloadState =
        MagazineInteractionTryWriteValue<int>(serverWeapon, kShotgunReloadStateOffset, 0);
    const bool wroteReloadNumShells =
        MagazineInteractionTryWriteValue<int>(serverWeapon, kShotgunReloadNumShellsOffset, 0);
    const bool wroteReloadAnimState =
        MagazineInteractionTryWriteValue<int>(serverWeapon, kShotgunReloadAnimStateOffset, 0);
    const bool wroteShellsInserted =
        MagazineInteractionTryWriteValue<int>(serverWeapon, kShotgunShellsInsertedOffset, 0);
    const bool wrotePartialReloadStage =
        MagazineInteractionTryWriteValue<unsigned char>(serverWeapon, kL4DWeaponPartialReloadStageOffset, 0);
    const bool wroteReloadFromEmpty =
        MagazineInteractionTryWriteValue<unsigned char>(serverWeapon, kL4DWeaponReloadFromEmptyOffset, 0);
    const bool wroteInReload =
        MagazineInteractionTryWriteValue<unsigned char>(serverWeapon, kBaseWeaponInReloadOffset, 0);

    if (!wroteReloadState && !wroteReloadNumShells && !wroteReloadAnimState &&
        !wroteShellsInserted && !wrotePartialReloadStage && !wroteReloadFromEmpty && !wroteInReload)
    {
        return directCommitAppliedOrAlreadyCurrent;
    }

    m_MagazineInteractionShotgunServerReloadAbortPending = false;
    m_MagazineInteractionShotgunServerReloadAbortUntil = {};

    return true;
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
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
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
        m_MagazineInteractionState == MagazineInteractionManualState::AutoBolting) &&
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

void VR::CancelMagazineInteractionManual()
{
    const bool wasActive = IsMagazineInteractionManualActive() || m_MagazineInteractionLeftHandHolding;
    m_MagazineInteractionState = MagazineInteractionManualState::Idle;
    m_MagazineInteractionLeftHandHolding = false;
    m_MagazineInteractionReloadTriggered = false;
    m_MagazineInteractionReloadCommandPending = false;
    m_MagazineInteractionReloadCommandIssued = false;
    m_MagazineInteractionReloadCommandHoldUntil = {};
    m_MagazineInteractionOldMagazinePulled = false;
    m_MagazineInteractionChamberEmpty = false;
    m_MagazineInteractionOneInChamber = false;
    m_MagazineInteractionOldMagazineContactActive = false;
    m_MagazineInteractionFreshMagazineContactActive = false;
    m_MagazineInteractionBoltContactActive = false;
    m_MagazineInteractionShotgunShellMode = false;
    m_MagazineInteractionServerClipSettlementActive = false;
    m_MagazineInteractionServerClipReserveHoldActive = false;
    m_MagazineInteractionServerClipReserveHoldAmmoType = -1;
    m_MagazineInteractionServerClipReserveHoldReserve = -1;
    m_MagazineInteractionServerClipReserveHoldOffset = -1;
    m_MagazineInteractionShotgunServerReloadAbortPending = false;
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
    m_MagazineInteractionShotgunServerReloadAbortUntil = {};
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

    const bool physicalLeftHand = IsGameplayHandLeftPhysical(true);
    vr::VRActionHandle_t& action = physicalLeftHand
        ? m_NativeViewmodelLeftHandOpenVRAction
        : m_NativeViewmodelRightHandOpenVRAction;
    const char* actionPath = physicalLeftHand
        ? "/actions/base/in/skeleton_lefthand"
        : "/actions/base/in/skeleton_righthand";

    return ReadOpenVRSkeletalFingerCurls(
        m_Input,
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

    const bool stale =
        m_NativeViewmodelLeftHandOpenVRFingerCurlsValid &&
        std::chrono::duration<float>(now - m_NativeViewmodelLeftHandOpenVRFingerCurlsAt).count() > 0.35f;
    if (!m_NativeViewmodelLeftHandOpenVRSkeleton || !m_Input || stale)
        m_NativeViewmodelLeftHandOpenVRFingerCurlsValid = false;
}

bool VR::GetNativeViewmodelLeftHandOpenVRFingerCurls(std::array<float, 5>& outCurls) const
{
    outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (!m_NativeViewmodelLeftHandOpenVRSkeleton)
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

    const bool stale =
        m_NativeViewmodelRightHandOpenVRFingerCurlsValid &&
        std::chrono::duration<float>(now - m_NativeViewmodelRightHandOpenVRFingerCurlsAt).count() > 0.35f;
    if (!m_NativeViewmodelLeftHandOpenVRSkeleton || !m_Input || stale)
        m_NativeViewmodelRightHandOpenVRFingerCurlsValid = false;
}

bool VR::GetNativeViewmodelRightHandOpenVRFingerCurls(std::array<float, 5>& outCurls) const
{
    outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (!m_NativeViewmodelLeftHandOpenVRSkeleton)
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

bool VR::UpdateMagazineInteraction(
    C_BasePlayer* localPlayer,
    bool leftGripDown,
    bool leftGripJustPressed,
    bool leftSupportHandDown,
    bool leftSupportHandJustPressed,
    bool allowGameplayInputOnTwoHandedGripRelease)
{
    const auto now = std::chrono::steady_clock::now();
    const bool magazineInteractionPhysicalLeftHand = IsGameplayHandLeftPhysical(true);
    if (m_MagazineInteractionSuppressLeftInputUntilRelease && !leftGripDown)
    {
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        Game::logMsg("[VR][MagazineInteraction] interaction gesture released after physical reload; normal left-hand input restored");
    }
    if (m_MagazineInteractionSuppressLeftInputUntilRelease)
        return false;
    constexpr float kMagazineInteractionReloadCommandHoldSeconds = 0.35f;
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

    auto startImmediateReloadCommand = [&](const char* reason)
    {
        m_MagazineInteractionReloadTriggered = true;
        m_MagazineInteractionReloadCommandPending = true;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(kMagazineInteractionReloadCommandHoldSeconds));
        Game::logMsg(
            "[VR][MagazineInteraction] backend reload command queued reason=%s hold=%.2fs shotgun=%d",
            reason ? reason : "unknown",
            kMagazineInteractionReloadCommandHoldSeconds,
            m_MagazineInteractionShotgunShellMode ? 1 : 0);
    };

    auto queueShotgunServerReloadAbort = [&](const char* reason)
    {
        if (!m_Game)
            return;

        m_Game->ClientCmd_Unrestricted("-reload");
        m_ReloadCmdOwned = false;
        m_MagazineInteractionReloadTriggered = false;
        m_MagazineInteractionReloadCommandPending = false;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil = {};
        QueueMagazineInteractionShotgunServerReloadAbort(reason);
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
            IsGameplayHandLeftPhysical(false),
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
        return m_LeftControllerPosAbs - m_HmdPosAbs;
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
            m_LeftControllerPosAbs,
            m_LeftControllerForward,
            m_LeftControllerRight,
            m_LeftControllerUp);
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
            m_LeftControllerPosAbs,
            m_LeftControllerForward,
            m_LeftControllerRight,
            m_LeftControllerUp);
        VrHandMatrix4 orientationWorld = MagazineInteractionBuildWorldFromControllerRelation(
            controllerWorld,
            m_MagazineInteractionControllerToMagazine);
        if (!MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            orientationWorld = MagazineInteractionBuildFreshHandMagazineWorld(this);
        if (!MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            orientationWorld = controllerWorld;
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

    auto snapFreshMagazineCenterOffsetToLeftHandAnchor =
        [&](const VrHandMatrix4& controllerWorld,
            const VrHandMatrix4& freshMagazineWorld,
            bool& outUsedWristAnchor,
            Vector& outTargetHandAnchorWorld,
            Vector& outFreshHandAnchorLocal,
            Vector& outFreshCenterLocal) -> bool
    {
        outFreshCenterLocal = MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox);
        outFreshHandAnchorLocal =
            MagazineInteractionFreshMagazineHandAnchorLocal(this, m_MagazineInteractionSocketBox);
        const Vector snappedCenterOffsetWorld = MagazineInteractionMatrixLocalVectorToWorld(
            freshMagazineWorld,
            outFreshCenterLocal - outFreshHandAnchorLocal);
        if (!MagazineInteractionResolveFreshMagazineHandAnchorWorld(
            this,
            controllerWorld,
            outTargetHandAnchorWorld,
            outUsedWristAnchor))
        {
            return false;
        }

        m_MagazineInteractionHeldMagazineCenterOffsetLocal =
            MagazineInteractionWorldVectorToMatrixLocal(
                controllerWorld,
                outTargetHandAnchorWorld +
                snappedCenterOffsetWorld -
                MagazineInteractionMatrixOrigin(controllerWorld));
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
            m_MagazineInteractionShotgunShellsLoadedThisSession > 0 &&
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
        if (!getFreshBoltBoxForActiveViewmodel(box))
            return;

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
        if(m_MagazineInteractionSuppressEmptyClipAutoReload && !m_MagazineInteractionChamberEmpty)
            return false;

        if (!MagazineInteractionWeaponRequiresManualBolt(weaponId))
            return false;

        MagazineInteractionBoxSnapshot boltBox{};
        if (!getFreshBoltBoxForActiveViewmodel(boltBox))
        {
            Game::logMsg(
                "[VR][MagazineInteraction] cannot start bolt stage: no fresh bolt/slide box weaponId=%d model=%s reason=%s",
                static_cast<int>(weaponId),
                m_MagazineInteractionMagazineModelName.c_str(),
                reason ? reason : "unknown");
            return false;
        }

        const float requiredPull = std::max(0.0f, MagazineInteractionResolveBoltPullDistanceMeters(this)) * m_VRScale;
        if (requiredPull <= 0.001f)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] skipped bolt stage because pull distance is disabled weaponId=%d reason=%s",
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
        m_MagazineInteractionState = MagazineInteractionManualState::HoldingBolt;
        m_MagazineInteractionLeftHandHolding = true;
        m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = leftControllerPosRelativeToHmd();
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

    auto completeBoltStage = [&](const char* reason, bool suppressLeftInputUntilRelease)
    {
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
            (m_MagazineInteractionServerClipSettlementActive ||
                m_MagazineInteractionQuickReloadMode) &&
            (m_MagazineInteractionServerClipSettlementActive ||
                MagazineInteractionWeaponUsesDetachableMagazine(sessionWeaponId) ||
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
            MagazineInteractionClearDetachableClientReloadState(m_MagazineInteractionWeapon);
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

    const bool vrHandsInteractionAvailable =
        m_IsVREnabled &&
        (m_VrHandsEnabled || m_NativeViewmodelHandsOnly) &&
        hasActiveWeapon;
    const int activeWeaponIdInt = static_cast<int>(activeWeaponId);

    if (!vrHandsInteractionAvailable)
    {
        if (m_VrHandsTwoHandedGripActive)
        {
            m_VrHandsTwoHandedGripActive = false;
            m_VrHandsTwoHandedGripWeaponId = 0;
            m_VrHandsTwoHandedMountFriendlyGripEnteredAt = {};
            m_VrHandsTwoHandedMountFriendlyGripContact = false;
            if (m_VrHandsDebugLog)
                Game::logMsg("[VR][Hands] two-hand grip released because VR hands input became unavailable");
        }
        CancelMagazineInteractionManual();
        return false;
    }

    const bool activeWeaponUsesShotgunShells =
        MagazineInteractionWeaponUsesShotgunShells(activeWeaponId);
    const bool activeWeaponUsesPhysicalReload =
        MagazineInteractionWeaponUsesPhysicalReload(activeWeaponId);
    const bool activeShotgunManualReloadAvailable =
        activeWeaponUsesShotgunShells &&
        IsMagazineInteractionShotgunServerHookActive(static_cast<int>(activeWeaponId));
    const bool activeDetachableServerClipSettlementAvailable =
        MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId) &&
        (IsMagazineInteractionServerHookActive(static_cast<int>(activeWeaponId)) ||
            IsMagazineInteractionAnyServerHookActive());

    m_VrHandsVirtualStockHeldPistol = !VrHandsLongWeapon(activeWeaponId);

    if (m_VrHandsTwoHandedGripActive &&
        (m_VrHandsTwoHandedGripWeaponId != activeWeaponIdInt))
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
            m_LeftControllerPosAbs,
            m_LeftControllerAngAbs,
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
            m_LeftControllerPosAbs,
            m_LeftControllerAngAbs,
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
            m_LeftControllerPosAbs,
            m_LeftControllerAngAbs,
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

    const bool manualReloadOwnsOffhandInput =
        IsMagazineInteractionManualActive() ||
        nativeReloadSuppressWindowActiveForWeapon();

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
                        m_VrHandsTwoHandedGripActive = true;
                        m_VrHandsTwoHandedGripWeaponId = activeWeaponIdInt;
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
            (!leftHandTouchesMagazineForGripExclusion() || isSeparateInput))
        {
            float twoHandTargetDistance = FLT_MAX;
            if (leftHandTouchesTwoHandedGripTarget(twoHandTargetDistance))
            {
                m_VrHandsTwoHandedGripActive = true;
                m_VrHandsTwoHandedGripWeaponId = activeWeaponIdInt;
                if (leftGripDown)
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

    if (!m_MagazineInteractionEnabled || !activeWeaponUsesPhysicalReload)
    {
        CancelMagazineInteractionManual();
        return false;
    }

    auto readClientAmmoForDirectClipSettlement = [&](int& ammoType, int& reserve, int& reserveOffset) -> bool
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

        const int hudReserve = m_LastHudReserve;
        int bestAmmoType = -1;
        int bestReserve = -1;
        int bestReserveOffset = -1;
        int bestScore = -1;
        for (int candidateAmmoType = 0; candidateAmmoType < 32; ++candidateAmmoType)
        {
            int candidateReserve = -1;
            int candidateReserveOffset = -1;
            if (!readAmmoSlot(candidateAmmoType, candidateReserve, candidateReserveOffset))
                continue;

            int score = -1;
            if (hudReserve >= 0)
            {
                if (candidateReserve == hudReserve)
                    score = 1000;
                else if (std::abs(candidateReserve - hudReserve) <= 1)
                    score = 850;
            }
            else if (candidateReserve > 0)
            {
                score = 100 + std::min(candidateReserve, 100);
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestAmmoType = candidateAmmoType;
                bestReserve = candidateReserve;
                bestReserveOffset = candidateReserveOffset;
            }
        }

        if (bestAmmoType >= 0)
        {
            ammoType = bestAmmoType;
            reserve = bestReserve;
            reserveOffset = bestReserveOffset;
            Game::logMsg(
                "[VR][MagazineInteraction] resolved client reserve slot by scan weaponId=%d ammoType=%d reserve=%d hudReserve=%d",
                static_cast<int>(activeWeaponId),
                ammoType,
                reserve,
                hudReserve);
            return true;
        }

        ammoType = -1;
        reserve = -1;
        reserveOffset = -1;
        return false;
    };

    auto captureServerHookReserveHoldIfNeeded = [&](const char* reason) -> bool
    {
        if (m_MagazineInteractionServerClipReserveHoldActive)
            return true;

        int ammoType = -1;
        int reserve = -1;
        int reserveOffset = -1;
        if (!readClientAmmoForDirectClipSettlement(ammoType, reserve, reserveOffset))
            return false;

        m_MagazineInteractionServerClipReserveHoldActive = true;
        m_MagazineInteractionServerClipReserveHoldAmmoType = ammoType;
        m_MagazineInteractionServerClipReserveHoldReserve = reserve;
        m_MagazineInteractionServerClipReserveHoldOffset = reserveOffset;
        Game::logMsg(
            "[VR][MagazineInteraction] captured reserve hold baseline weaponId=%d ammoType=%d reserve=%d reason=%s",
            static_cast<int>(activeWeaponId),
            ammoType,
            reserve,
            reason ? reason : "unknown");
        return true;
    };

    auto clearNativeClientReloadState = [&](const char* reason) -> bool
    {
        if (!activeWeapon)
            return false;
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

    auto applyServerHookClipSettlement = [&](
        int targetClip,
        int ammoType,
        int targetReserve,
        int reserveOffset,
        const char* reason,
        float holdSeconds,
        bool logResult) -> bool
    {
        if (!m_MagazineInteractionServerClipSettlementActive ||
            m_MagazineInteractionShotgunShellMode ||
            !activeWeapon ||
            targetClip < 0)
        {
            return false;
        }

        if (m_MagazineInteractionOldMagazinePulled ||
            targetClip > 0)
        {
            captureServerHookReserveHoldIfNeeded(reason);
        }
        if (m_MagazineInteractionServerClipReserveHoldActive)
        {
            const int heldAmmoType = m_MagazineInteractionServerClipReserveHoldAmmoType;
            const int heldReserve = m_MagazineInteractionServerClipReserveHoldReserve;
            const int heldReserveOffset = m_MagazineInteractionServerClipReserveHoldOffset;
            if (heldAmmoType >= 0 && heldReserve >= 0 && heldReserveOffset >= 0)
            {
                if (targetClip == 0)
                {
                    ammoType = heldAmmoType;
                    targetReserve = heldReserve;
                    reserveOffset = heldReserveOffset;
                }
                else if (targetClip > 0)
                {
                    ammoType = heldAmmoType;
                    const int sourceClip = std::clamp(
                        m_MagazineInteractionStartClip,
                        0,
                        targetClip);
                    const int roundsAdded = std::max(0, targetClip - sourceClip);
                    targetReserve = std::max(0, heldReserve - roundsAdded);
                    reserveOffset = heldReserveOffset;
                }
            }
        }
        suppressNativeReloadPlayback(targetClip == 0 ? 0.35f : 0.85f, reason);

        const int expectedServerClip = activeClip;
        int queuedAmmoType = ammoType;
        int expectedServerReserve = -1;
        if (queuedAmmoType < 0 && activeWeapon)
        {
            int activeAmmoType = -1;
            if (MagazineInteractionTryReadValue(activeWeapon, VR::kPrimaryAmmoTypeOffset, activeAmmoType) &&
                activeAmmoType >= 0 &&
                activeAmmoType < 32)
            {
                queuedAmmoType = activeAmmoType;
            }
        }
        if (targetReserve >= 0)
        {
            if (m_MagazineInteractionServerClipReserveHoldActive &&
                m_MagazineInteractionServerClipReserveHoldReserve >= 0)
            {
                expectedServerReserve = m_MagazineInteractionServerClipReserveHoldReserve;
            }
            else if (localPlayer && reserveOffset >= 0)
            {
                int currentReserve = -1;
                if (MagazineInteractionTryReadValue(localPlayer, reserveOffset, currentReserve))
                    expectedServerReserve = currentReserve;
            }
        }

        const bool wroteClientClip =
            MagazineInteractionTryWriteValue<int>(activeWeapon, VR::kClip1Offset, targetClip);
        bool wroteClientReserve = false;
        if (targetReserve >= 0 && reserveOffset >= 0)
        {
            wroteClientReserve =
                MagazineInteractionTryWriteValue<int>(localPlayer, reserveOffset, targetReserve);
        }
        QueueMagazineInteractionServerClipCommit(
            targetClip,
            queuedAmmoType,
            targetReserve,
            expectedServerClip,
            expectedServerReserve,
            reason,
            holdSeconds);
        activeClip = targetClip;

        if (logResult)
        {
            const int loggedSourceClip = std::clamp(
                m_MagazineInteractionStartClip,
                0,
                std::max(0, targetClip));
            const int loggedRoundsAdded = std::max(0, targetClip - loggedSourceClip);
            Game::logMsg(
                "[VR][MagazineInteraction] server-hook clip settlement targetClip=%d startClip=%d roundsAdded=%d targetReserve=%d weaponId=%d clientClip=%d clientReserve=%d reason=%s",
                targetClip,
                m_MagazineInteractionStartClip,
                loggedRoundsAdded,
                targetReserve,
                static_cast<int>(activeWeaponId),
                wroteClientClip ? 1 : 0,
                wroteClientReserve ? 1 : 0,
                reason ? reason : "unknown");
        }
        return wroteClientClip;
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
        return m_MagazineInteractionQuickReloadMode &&
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
            m_MagazineInteractionServerClipSettlementActive ||
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
        if (requiredPull <= 0.001f)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] quick reload auto-bolt animation fallback: pull distance disabled weaponId=%d reason=%s",
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
        m_MagazineInteractionReloadTriggered = false;
        m_MagazineInteractionReloadCommandPending = false;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil = {};
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        m_MagazineInteractionOldMagazinePulled = false;
        m_MagazineInteractionOldMagazineContactActive = false;
        m_MagazineInteractionFreshMagazineContactActive = false;
        m_MagazineInteractionBoltContactActive = false;
        m_MagazineInteractionShotgunShellMode = MagazineInteractionWeaponUsesShotgunShells(activeWeaponId);
        m_MagazineInteractionServerClipSettlementActive =
            !m_MagazineInteractionShotgunShellMode &&
            activeDetachableServerClipSettlementAvailable;
        m_MagazineInteractionServerClipReserveHoldActive = false;
        m_MagazineInteractionServerClipReserveHoldAmmoType = -1;
        m_MagazineInteractionServerClipReserveHoldReserve = -1;
        m_MagazineInteractionServerClipReserveHoldOffset = -1;
        m_MagazineInteractionShotgunServerReloadAbortPending = false;
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
        m_MagazineInteractionStartClip = activeClip;
        m_MagazineInteractionMagazineBoneIndex = box.boneIndex;
        m_MagazineInteractionViewmodelEntityIndex = box.entityIndex;
        m_MagazineInteractionMagazineModelName = box.modelName;
        m_MagazineInteractionSocketBox = box;
        m_MagazineInteractionSocketValid = true;
        m_MagazineInteractionSocketWorld = MagazineInteractionBuildBoxWorld(box);
        rebuildSocketCaptureFromSocket();
        setDetachedMagazineWorld(m_MagazineInteractionSocketWorld);
        m_MagazineInteractionFreshPickupBasisValid = false;
        m_MagazineInteractionFreshPickupForward = {};
        m_MagazineInteractionFreshPickupRight = {};
        m_MagazineInteractionGrabStartLeftControllerPosAbs = leftControllerPosRelativeToHmd();
        m_MagazineInteractionStarted = now;
        m_MagazineInteractionFreshGrabbedAt = {};
        m_MagazineInteractionPostInsertStarted = {};
        m_MagazineInteractionShotgunServerReloadAbortUntil = {};
        m_MagazineInteractionViewmodelFreezeDeferredUntil = {};
        m_MagazineInteractionSyntheticClipOutSample.clear();
        m_MagazineInteractionSyntheticClipOutStarted = {};
        m_MagazineInteractionSyntheticClipInSample.clear();
        m_MagazineInteractionSyntheticClipInStarted = {};
        m_MagazineInteractionEmptyFireSoundLastPlayed = {};
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        if (!m_MagazineInteractionShotgunShellMode &&
            MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId))
        {
            captureServerHookReserveHoldIfNeeded("session-begin");
        }
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

    auto enableServerHookClipSettlementIfAvailable = [&](const char* reason) -> bool
    {
        if (m_MagazineInteractionServerClipSettlementActive ||
            m_MagazineInteractionShotgunShellMode ||
            !MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId) ||
            !activeDetachableServerClipSettlementAvailable ||
            !IsMagazineInteractionManualActive())
        {
            return false;
        }

        m_MagazineInteractionServerClipSettlementActive = true;
        if (m_MagazineInteractionOldMagazinePulled)
        {
            captureServerHookReserveHoldIfNeeded(reason);
            m_MagazineInteractionReloadTriggered = true;
            m_MagazineInteractionReloadCommandPending = false;
            m_MagazineInteractionReloadCommandIssued = false;
            m_MagazineInteractionReloadCommandHoldUntil = {};
            suppressNativeReloadPlayback(0.85f, reason);
        }
        Game::logMsg(
            "[VR][MagazineInteraction] server-hook clip settlement enabled mid-session weaponId=%d serverWeaponId=%d oldPulled=%d reason=%s",
            static_cast<int>(activeWeaponId),
            m_MagazineInteractionAnyServerHookWeaponId,
            m_MagazineInteractionOldMagazinePulled ? 1 : 0,
            reason ? reason : "unknown");
        return true;
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

    enableServerHookClipSettlementIfAvailable("server-hook-active");

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

    if (m_MagazineInteractionShotgunShellMode &&
        m_MagazineInteractionShotgunShellsLoadedThisSession > 0 &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine))
    {
        ApplyMagazineInteractionShotgunClientReloadAbort(
            activeWeapon,
            static_cast<int>(activeWeaponId),
            "shotgun-between-shells");
    }

    if (m_MagazineInteractionServerClipSettlementActive &&
        m_MagazineInteractionOldMagazinePulled &&
        !m_MagazineInteractionShotgunShellMode &&
        (m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine))
    {
        if (m_MagazineInteractionOneInChamber)
        {
            if (activeClip == 0) // Chamber was left one, but the clip just went empty.
            {
                if (m_MagazineInteractionChamberEmpty && (
                    m_MagazineInteractionViewmodelFreezeDeferredUntil.time_since_epoch().count() == 0 ||
                    now >= m_MagazineInteractionViewmodelFreezeDeferredUntil)) // Wait for animation before set m_MagazineInteractionOneInChamber
                        m_MagazineInteractionOneInChamber = false;
                else if (!m_MagazineInteractionChamberEmpty) // Chamber empty animation
                {
                    m_MagazineInteractionChamberEmpty = true;
                    m_MagazineInteractionViewmodelFreezeDeferredUntil =
                        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<float>(GetMagazineInteractionEmptyClipAutoEjectFreezeDelaySeconds(activeWeaponId)));
                }
            }
        }
        else
            applyServerHookClipSettlement(
                0,
                -1,
                -1,
                -1,
                "magazine-out-maintain-empty",
                0.35f,
                false);
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
                        queueShotgunServerReloadAbort("shotgun-shell-loaded");
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
        queueShotgunServerReloadAbort("regular-release-did-not-stop-shell-reload");
        if (maxClip > 0 && activeClip >= maxClip)
        {
            if (!beginBoltStage(activeWeaponId, "shotgun-full-after-interrupt-fallback"))
                CancelMagazineInteractionManual();
            return false;
        }
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab)
    {
        refreshBoltFromPublishedViewmodelBox();
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
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

        const float grabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
            m_MagazineInteractionBoltRestBox,
            m_LeftControllerPosAbs,
            m_LeftControllerAngAbs,
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

        const Vector handDelta =
            leftControllerPosRelativeToHmd() -
            m_MagazineInteractionBoltGrabStartLeftControllerPosAbs;
        const float handPullDistance = VrHandMath::Dot(handDelta, inputAxis);
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

        if (m_MagazineInteractionBoltReachedRear &&
            m_MagazineInteractionBoltPullDistance <= returnDistance)
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
        if (m_MagazineInteractionShotgunShellMode &&
            m_MagazineInteractionShotgunShellsLoadedThisSession > 0 &&
            leftGripDown)
        {
            MagazineInteractionBoxSnapshot boltBox{};
            if (getFreshBoltBoxForActiveViewmodel(boltBox))
            {
                const float grabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
                    boltBox,
                    m_LeftControllerPosAbs,
                    m_LeftControllerAngAbs,
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
            pickupMagazineWorld = MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(
                this,
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
                m_LeftControllerPosAbs,
                m_LeftControllerAngAbs,
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

            m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
            m_MagazineInteractionLeftHandHolding = true;
            const VrHandMatrix4 controllerWorld = MagazineInteractionBuildViewmodelReprojectedControllerWorld(
                this,
                m_LeftControllerPosAbs,
                m_LeftControllerForward,
                m_LeftControllerRight,
                m_LeftControllerUp);
            const VrHandMatrix4 freshMagazineWorld = pickupMagazineWorld;
            m_MagazineInteractionControllerToMagazine =
                MagazineInteractionBuildControllerRelation(controllerWorld, freshMagazineWorld);
            const bool relationCaptured =
                MagazineInteractionMatrixBasisLooksValid(m_MagazineInteractionControllerToMagazine);
            m_MagazineInteractionFreshGrabbedAt = now;
            m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
            bool usedWristAnchor = false;
            Vector targetHandAnchorWorld{};
            Vector freshHandAnchorLocal{};
            Vector freshCenterLocal{};
            snapFreshMagazineCenterOffsetToLeftHandAnchor(
                controllerWorld,
                freshMagazineWorld,
                usedWristAnchor,
                targetHandAnchorWorld,
                freshHandAnchorLocal,
                freshCenterLocal);
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
                "[VR][MagazineInteraction] fresh magazine snapped to left hand anchor from fresh magazine box distance=%.2f range=%.2f relationCaptured=%d quickAutoGrab=%d wristAnchor=%d clipOrigin=(%.2f %.2f %.2f) visibleCenter=(%.2f %.2f %.2f) handAnchor=(%.2f %.2f %.2f) targetHandAnchor=(%.2f %.2f %.2f) centerLocalOffset=(%.2f %.2f %.2f) centerLocal=(%.2f %.2f %.2f) anchorLocal=(%.2f %.2f %.2f) model=%s; move it into MagazineSocket",
                freshGrabDistance,
                freshGrabRange,
                relationCaptured ? 1 : 0,
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
            m_MagazineInteractionQuickReloadMode;
        if (!quickReloadKeepsFreshMagazineAttached &&
            !leftGripDown &&
            freshGrabAgeSeconds >= 0.18f)
        {
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionFreshPickupBasisValid = false;
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
        if (detachedMagazineFitsSocket())
        {
            bool shotgunInsertedShellWouldFillClip = false;
            bool shotgunInsertedShellCanArmBoltBeforeBackend = false;
            if (m_MagazineInteractionShotgunShellMode)
            {
                captureShotgunStableSocketCapture("shotgun-shell-inserted");
                m_MagazineInteractionStartClip = activeClip;

                const int requestedShells = std::clamp(m_MagazineInteractionShotgunShellsPerInsert, 1, 8);
                const int clipSpace = (maxClip > 0)
                    ? std::max(0, maxClip - activeClip)
                    : requestedShells;
                int ammoType = -1;
                int reserve = -1;
                int reserveOffset = -1;
                const bool reserveKnown =
                    readClientAmmoForDirectClipSettlement(ammoType, reserve, reserveOffset);
                if (reserveKnown && reserve <= 0)
                {
                    m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                    m_MagazineInteractionLeftHandHolding = false;
                    m_MagazineInteractionFreshPickupBasisValid = false;
                    m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                    if (leftGripDown)
                        m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    return false;
                }

                const int shellsToLoad = reserveKnown
                    ? std::min(std::min(requestedShells, clipSpace), reserve)
                    : std::min(requestedShells, clipSpace);
                if (shellsToLoad <= 0)
                {
                    m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                    m_MagazineInteractionLeftHandHolding = false;
                    m_MagazineInteractionFreshPickupBasisValid = false;
                    m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                    if (leftGripDown)
                        m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    return false;
                }

                const int targetClip = activeClip + shellsToLoad;
                shotgunInsertedShellWouldFillClip = maxClip > 0 && targetClip >= maxClip;
                shotgunInsertedShellCanArmBoltBeforeBackend = true;
                const bool wroteClientClip =
                    MagazineInteractionTryWriteValue<int>(activeWeapon, VR::kClip1Offset, targetClip);
                if (wroteClientClip)
                {
                    m_MagazineInteractionFreshMagazineContactActive = false;
                    triggerMagazineInteractionBothHandsHaptic(0.026f, 105.0f, 0.42f, 2);
                    MagazineInteractionPlayClipInSound(this);
                    const int targetReserve = reserveKnown ? std::max(0, reserve - shellsToLoad) : -1;
                    if (reserveKnown)
                    {
                        MagazineInteractionTryWriteValue<int>(localPlayer, reserveOffset, targetReserve);
                    }

                    if (m_Game)
                    {
                        m_Game->ClientCmd_Unrestricted("-reload");
                        m_Game->ClientCmd_Unrestricted("-attack");
                    }
                    activeClip = targetClip;
                    m_ReloadCmdOwned = false;
                    m_MagazineInteractionReloadTriggered = false;
                    m_MagazineInteractionReloadCommandPending = false;
                    m_MagazineInteractionReloadCommandIssued = false;
                    m_MagazineInteractionReloadCommandHoldUntil = {};
                    m_MagazineInteractionShotgunShellsLoadedThisSession += shellsToLoad;
                    m_MagazineInteractionShotgunLastInterruptedClip = targetClip;
                    m_MagazineInteractionStartClip = targetClip;
                    ApplyMagazineInteractionShotgunClientReloadAbort(
                        activeWeapon,
                        static_cast<int>(activeWeaponId),
                        "shotgun-direct-shell-commit");
                    suppressNativeReloadPlayback(0.70f, "shotgun-direct-shell-commit");
                    QueueMagazineInteractionShotgunDirectShellCommit(
                        targetClip,
                        reserveKnown ? ammoType : -1,
                        targetReserve,
                        reserveKnown ? reserve : -1,
                        "shotgun-direct-shell-commit");

                    m_MagazineInteractionLeftHandHolding = false;
                    m_MagazineInteractionPostInsertStarted = {};
                    m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);

                    if (shotgunInsertedShellWouldFillClip)
                    {
                        if (!beginBoltStage(activeWeaponId, "shotgun-full-direct-commit"))
                        {
                            const bool suppressLeftInputUntilRelease = leftGripDown;
                            CancelMagazineInteractionManual();
                            if (suppressLeftInputUntilRelease)
                                m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                        }
                        return false;
                    }

                    m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                    m_MagazineInteractionFreshPickupBasisValid = false;
                    return false;
                }
            }
            if (m_MagazineInteractionServerClipSettlementActive &&
                !m_MagazineInteractionShotgunShellMode)
            {
                const int targetClip = MagazineInteractionDefaultMaxClip(
                    activeWeaponId,
                    m_MagazineInteractionMagazineModelName);
                if (targetClip > 0)
                {
                    int ammoType = -1;
                    int reserve = -1;
                    int reserveOffset = -1;
                    const bool reserveKnown =
                        readClientAmmoForDirectClipSettlement(ammoType, reserve, reserveOffset);
                    const int sourceClip = std::clamp(
                        m_MagazineInteractionStartClip,
                        0,
                        targetClip);
                    const int roundsAdded = std::max(0, targetClip - sourceClip);
                    const int targetReserve = reserveKnown
                        ? std::max(0, reserve - roundsAdded)
                        : -1;

                    m_MagazineInteractionFreshMagazineContactActive = false;
                    triggerMagazineInteractionBothHandsHaptic(0.026f, 105.0f, 0.42f, 2);
                    MagazineInteractionPlayClipInSound(this);
                    applyServerHookClipSettlement(
                        targetClip,
                        reserveKnown ? ammoType : -1,
                        targetReserve,
                        reserveKnown ? reserveOffset : -1,
                        "magazine-inserted-fill-clip",
                        1.00f,
                        true);

                    if (m_Game)
                    {
                        m_Game->ClientCmd_Unrestricted("-reload");
                        m_Game->ClientCmd_Unrestricted("-attack");
                    }
                    m_ReloadCmdOwned = false;
                    m_MagazineInteractionReloadTriggered = false;
                    m_MagazineInteractionReloadCommandPending = false;
                    m_MagazineInteractionReloadCommandIssued = false;
                    m_MagazineInteractionReloadCommandHoldUntil = {};
                    m_MagazineInteractionLeftHandHolding = false;
                    m_MagazineInteractionPostInsertStarted = {};
                    m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);

                    if (quickReloadShouldAutoBolt())
                    {
                        if (!beginQuickReloadAutoBoltAnimation("quick-reload-server-clip-commit"))
                            completeQuickReloadAutoBoltFallback("quick-reload-server-clip-commit", leftGripDown);
                        return false;
                    }

                    if (!beginBoltStage(activeWeaponId, "magazine-inserted-server-clip-commit"))
                    {
                        const bool suppressLeftInputUntilRelease = leftGripDown;
                        suppressQuickReloadNativeTail("quick-reload-server-clip-no-bolt");
                        CancelMagazineInteractionManual();
                        if (suppressLeftInputUntilRelease)
                            m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    }
                    return false;
                }
            }
            m_MagazineInteractionFreshMagazineContactActive = false;
            triggerMagazineInteractionBothHandsHaptic(0.026f, 105.0f, 0.42f, 2);
            if (!m_MagazineInteractionShotgunShellMode)
                MagazineInteractionPlayClipInSound(this);
            if (m_MagazineInteractionShotgunShellMode)
                startImmediateReloadCommand("shotgun-shell-inserted");
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionPostInsertStarted = now;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            const bool quickAutoBoltAfterBackend = quickReloadShouldAutoBolt();
            const bool boltArmedBeforeBackend =
                !quickAutoBoltAfterBackend &&
                (!m_MagazineInteractionShotgunShellMode || shotgunInsertedShellCanArmBoltBeforeBackend) &&
                beginBoltStage(activeWeaponId, "magazine-inserted-backend-pending", true);
            if (!boltArmedBeforeBackend)
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBackendReload;
            Game::logMsg(
                "[VR][MagazineInteraction] fresh %s inserted into MagazineSocket; waiting for backend reload clip=%d startClip=%d boltArmed=%d quickAutoBolt=%d",
                m_MagazineInteractionShotgunShellMode ? "shotgun shell" : "magazine",
                activeClip,
                m_MagazineInteractionStartClip,
                boltArmedBeforeBackend ? 1 : 0,
                quickAutoBoltAfterBackend ? 1 : 0);
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
            (leftControllerPosRelativeToHmd() -
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
            if (m_MagazineInteractionServerClipSettlementActive &&
                !m_MagazineInteractionShotgunShellMode)
            {
                const int heldAmmoType = m_MagazineInteractionServerClipReserveHoldAmmoType;
                const int heldReserve = m_MagazineInteractionServerClipReserveHoldReserve;

                m_MagazineInteractionReloadTriggered = true;
                m_MagazineInteractionReloadCommandPending = false;
                m_MagazineInteractionReloadCommandIssued = false;
                m_MagazineInteractionReloadCommandHoldUntil = {};
                m_MagazineInteractionChamberEmpty = activeClip == 0;

                if ((heldAmmoType <= 2 && heldReserve == 0)) // Pistol
                    m_MagazineInteractionOneInChamber = true;
                else
                    m_MagazineInteractionOneInChamber = !m_MagazineInteractionChamberEmpty && m_MagazineInteractionServerClipReserveHoldReserve > 0;

                applyServerHookClipSettlement(
                    m_MagazineInteractionOneInChamber ? 1 : 0,
                    -1,
                    -1,
                    -1,
                    "magazine-out-clear-clip",
                    0.35f,
                    true);
            }
            else
            {
                startImmediateReloadCommand("clip-out");
            }
            MagazineInteractionPlayClipOutSound(this);
            triggerMagazineInteractionBothHandsHaptic(0.026f, 95.0f, 0.42f, 2);
            Game::logMsg(
                "[VR][MagazineInteraction] old magazine pull threshold reached after clip-out handDistance=%.2f handThreshold=%.2f magazineDistance=%.2f magazineThreshold=%.2f serverClipSettlement=%d",
                handPullDistance,
                handTriggerDistance,
                magazinePullDistance,
                magazineTriggerDistance,
                m_MagazineInteractionServerClipSettlementActive ? 1 : 0);
            return reloadCommandPending();
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionSuppressEmptyClipAutoReload &&
        activeClip == 0 &&
        MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId))
    {
        MagazineInteractionBoxSnapshot box{};
        float boxAgeSeconds = -1.0f;
        bool usedCachedBox = false;
        if (getMagazineBoxForAutoEject(box, boxAgeSeconds, usedCachedBox))
        {
            beginMagazineInteractionSession(box);
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionChamberEmpty = true;
            m_MagazineInteractionOneInChamber = false;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionOldMagazinePulled = true;
            m_MagazineInteractionFreshPickupBasisValid = false;
            m_MagazineInteractionViewmodelFreezeDeferredUntil =
                now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(GetMagazineInteractionEmptyClipAutoEjectFreezeDelaySeconds(activeWeaponId)));
            if (m_MagazineInteractionServerClipSettlementActive &&
                !m_MagazineInteractionShotgunShellMode)
            {
                m_MagazineInteractionReloadTriggered = true;
                m_MagazineInteractionReloadCommandPending = false;
                m_MagazineInteractionReloadCommandIssued = false;
                m_MagazineInteractionReloadCommandHoldUntil = {};
                applyServerHookClipSettlement(
                    0,
                    -1,
                    -1,
                    -1,
                    "empty-clip-auto-eject-clear-clip",
                    0.35f,
                    true);
            }
            else
            {
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
            }
            MagazineInteractionPlayClipOutSound(this);
            Game::logMsg(
                "[VR][MagazineInteraction] empty clip auto-ejected magazine; waiting for fresh magazine weaponId=%d clip=%d ent=%d bone=%d age=%.3fs cached=%d serverClipSettlement=%d freezeDelay=%.2fs model=%s",
                static_cast<int>(activeWeaponId),
                activeClip,
                box.entityIndex,
                box.boneIndex,
                boxAgeSeconds,
                usedCachedBox ? 1 : 0,
                m_MagazineInteractionServerClipSettlementActive ? 1 : 0,
                GetMagazineInteractionEmptyClipAutoEjectFreezeDelaySeconds(activeWeaponId),
                box.modelName.c_str());
            return reloadCommandPending();
        }
    }

    m_MagazineInteractionFreshMagazineContactActive = false;
    m_MagazineInteractionBoltContactActive = false;

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
        m_LeftControllerPosAbs,
        m_LeftControllerAngAbs,
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
        m_LeftControllerPosAbs,
        m_LeftControllerForward,
        m_LeftControllerRight,
        m_LeftControllerUp);
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

    m_VrHandsEnabled = false;
    m_NativeViewmodelHandsOnly = true;
    m_HideArms = false;
    m_VrHandsGlovesRuntimeFallback = true;

    if (!m_VrHandsGlovesFallbackLogged)
    {
        m_VrHandsGlovesFallbackLogged = true;
        Game::logMsg(
            "[VR][Hands] VR glove renderer unavailable (%s); falling back to NativeViewmodelHandsOnly",
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
        m_VrHandsEnabled && !emptyHandsPlaceholderActive && m_Input;
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
    const bool twoHandedGripPoseActive = IsVrHandsTwoHandedGripPoseActive();
    Vector leftHandPoseOffsetMeters = twoHandedGripPoseActive
        ? Vector(0.0f, 0.0f, 0.0f)
        : m_VrHandsLeftPoseOffsetMeters;
    const Vector leftHandPoseRotationOffsetDeg = twoHandedGripPoseActive
        ? Vector(0.0f, 0.0f, 0.0f)
        : m_VrHandsLeftPoseRotationOffsetDeg;
    const bool vrHandsRightUseViewmodelPose =
        m_VrHandsRightUseViewmodelPose;
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
        drewAny = m_VrHands->DrawForEye(
            device,
            m_Input,
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
            m_MagazineInteractionLeftHandPoseActive.load(std::memory_order_relaxed) != 0,
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
    if (!m_VrHandsEnabled || emptyHandsPlaceholderActive ||
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
    const bool drawVrGloves =
        m_VrHandsEnabled && !emptyHandsPlaceholderActive;
    if (drawVrGloves && m_Input && m_VrHandsGlovesEnabled)
    {
        if (!m_VrHands)
            m_VrHands = std::make_unique<VrHandSystem>();

        if (!m_VrHands->EnsureAssetsAvailable(m_VrHandsDebugLog) &&
            m_VrHands->IsDependencyUnavailable())
        {
            FallbackVrHandsGlovesToNative(m_VrHands->DependencyFailureReason().c_str());
        }
    }

    if (!drawVrGloves && !m_NativeViewmodelHandsOnly && !drawMagazineDebugBoxes)
        return;

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
        IMatRenderContext* renderContext =
            m_Game && m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
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
        IMatRenderContext* renderContext =
            m_Game && m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
        QueueVrHandsDrawForEye(renderContext, *view, eyeIndex, drawPass);
        return;
    }

    DrawVrHandsForEye(*view, eyeIndex, drawPass);
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
        if (MagazineInteractionSoundLooksShotgunPump(sample))
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
    if (!sample || !*sample || !m_Game ||
        (!m_MagazineInteractionReloadTriggered && !shotgunPhysicalSoundState && !nativeReloadSuppressed) ||
        !suppressingNativeReload)
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
        m_MagazineInteractionShotgunShellMode &&
        MagazineInteractionSoundLooksWeaponRelated(sample);
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
