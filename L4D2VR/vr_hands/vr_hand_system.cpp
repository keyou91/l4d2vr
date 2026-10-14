#include "vr_hand_system.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_math.h"
#include "vr_hand_vm_pose.h"
#include "../vr_skeletal_data.h"
#include "../vr_hand_input_validation.h"
#include "../vr_hand_pose_cache.h"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
    bool IsNonFatalViewmodelOcclusionError(const std::string& error)
    {
        return error.find("VR hand stencil clear received") != std::string::npos ||
            error.find("VR hand viewmodel occlusion requires") != std::string::npos ||
            error.find("D3D9 reserved stencil-bit clear failed") != std::string::npos;
    }

    struct VrHandsVec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct VrHandsVmFingerSource
    {
        const char* bone0 = nullptr;
        const char* bone1 = nullptr;
        const char* bone2 = nullptr;
        float proximalScale = 0.85f;
        float middleScale = 1.05f;
        float distalScale = 0.70f;
        float maxProximal = 1.15f;
        float maxMiddle = 1.25f;
        float maxDistal = 0.90f;
    };

    struct VrHandsGloveFingerChain
    {
        const char* joint0 = nullptr;
        const char* joint1 = nullptr;
        const char* joint2 = nullptr;
    };

    constexpr std::uint32_t kVrHandsVmPoseMaxAgeMs = 500u;
    constexpr std::uint32_t kMagazineDebugFreshBoxColorArgb = 0xFFFFC247u;
    constexpr std::uint32_t kMagazineDebugSocketCaptureBoxColorArgb = 0xFF2EF5FFu;
    constexpr std::uint32_t kMagazineDebugCurrentMagazineBoxColorArgb = 0xFFFF4818u;
    constexpr std::uint32_t kMagazineDebugBoltBoxColorArgb = 0xFF4A8CFFu;
    constexpr std::uint32_t kLeftHandTargetBoxColorArgb = 0xFF47FF72u;
    constexpr float kVrHandsVmCurlDeadZoneRadians = 0.08f;

    float ResolveVrHandsFingerMaxCurl(const std::array<float, 5>& fingerMaxCurl, size_t finger)
    {
        if (finger >= fingerMaxCurl.size())
            return 1.0f;
        return std::clamp(fingerMaxCurl[finger], 0.0f, 1.0f);
    }

    float ResolveVrHandsSceneAspect(const CViewSetup& view)
    {
        float aspect = view.m_flAspectRatio;
        if (!(aspect > 0.001f) && view.height > 0)
            aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
        return (aspect > 0.001f) ? aspect : 1.0f;
    }

    float ResolveVrHandsViewmodelAspect(const CViewSetup& view)
    {
        // Source's viewmodel layer is projected against the actual eye viewport.
        // On runtimes that recommend a non-square eye RT while reporting a square
        // lens projection, view.m_flAspectRatio intentionally remains the lens
        // aspect for the world layer. Reusing it for a VM-bound hand makes the
        // glove drift away from the weapon as the hand moves vertically or rotates.
        if (view.width > 0 && view.height > 0)
            return static_cast<float>(view.width) / static_cast<float>(view.height);
        return ResolveVrHandsSceneAspect(view);
    }

    VrHandMatrix4 BuildVrHandsProjection(const CViewSetup& view, bool useViewmodelLayer)
    {
        const float fov =
            (useViewmodelLayer && view.fovViewmodel > 0.001f) ? view.fovViewmodel : view.fov;
        const float aspect =
            useViewmodelLayer ? ResolveVrHandsViewmodelAspect(view) : ResolveVrHandsSceneAspect(view);
        const float zNear =
            (useViewmodelLayer && view.zNearViewmodel > 0.001f) ? view.zNearViewmodel : view.zNear;
        const float zFar =
            (useViewmodelLayer && view.zFarViewmodel > zNear) ? view.zFarViewmodel : view.zFar;
        return VrHandMath::BuildPerspective(fov, aspect, zNear, zFar);
    }

    bool ReprojectScenePointToViewmodelLayer(
        const CViewSetup& view,
        const Vector& scenePoint,
        Vector& outViewmodelPoint)
    {
        const float sceneFov = (view.fov > 0.001f) ? view.fov : 90.0f;
        const float viewmodelFov =
            (view.fovViewmodel > 0.001f) ? view.fovViewmodel : sceneFov;
        const float sceneAspect = ResolveVrHandsSceneAspect(view);
        const float viewmodelAspect = ResolveVrHandsViewmodelAspect(view);

        constexpr float kPi = 3.14159265358979323846f;
        const float sceneXScale =
            1.0f / std::tan(std::clamp(sceneFov, 1.0f, 179.0f) * kPi / 360.0f);
        const float viewmodelXScale =
            1.0f / std::tan(std::clamp(viewmodelFov, 1.0f, 179.0f) * kPi / 360.0f);
        const float sceneYScale = sceneXScale * sceneAspect;
        const float viewmodelYScale = viewmodelXScale * viewmodelAspect;
        if (!(viewmodelXScale > 0.0001f) || !(viewmodelYScale > 0.0001f))
            return false;

        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle qangles(view.angles.x, view.angles.y, view.angles.z);
        QAngle::AngleVectors(qangles, &forward, &right, &up);
        forward = VrHandMath::Normalize(forward);
        right = VrHandMath::Normalize(right);
        up = VrHandMath::Normalize(up);
        if (forward.Length() <= 0.0001f || right.Length() <= 0.0001f || up.Length() <= 0.0001f)
            return false;

        const Vector delta = scenePoint - view.origin;
        const float viewX = VrHandMath::Dot(delta, right);
        const float viewY = VrHandMath::Dot(delta, up);
        const float viewZ = VrHandMath::Dot(delta, forward);
        if (!std::isfinite(viewX) || !std::isfinite(viewY) || !std::isfinite(viewZ))
            return false;

        outViewmodelPoint =
            view.origin +
            right * (viewX * (sceneXScale / viewmodelXScale)) +
            up * (viewY * (sceneYScale / viewmodelYScale)) +
            forward * viewZ;
        return true;
    }

    int FindVrHandsNameIndex(const std::vector<std::string>& names, const char* name)
    {
        if (!name || !*name)
            return -1;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    VrHandMatrix4 BuildVrHandsBindLocalMatrix(const VrHandMeshAsset& asset, int joint)
    {
        const int parent = asset.jointParents[static_cast<size_t>(joint)];
        if (parent >= 0 && parent < static_cast<int>(asset.inverseBindMatrices.size()))
        {
            return VrHandMath::Multiply(
                asset.inverseBindMatrices[static_cast<size_t>(parent)],
                asset.bindMatrices[static_cast<size_t>(joint)]);
        }
        return asset.bindMatrices[static_cast<size_t>(joint)];
    }

    VrHandsVec3 VrHandsMatrixTranslation(const VrHandMatrix4& matrix)
    {
        return
        {
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3)
        };
    }

    VrHandsVec3 VrHandsSubtract(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        return { left.x - right.x, left.y - right.y, left.z - right.z };
    }

    float VrHandsDot(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    float VrHandsAngleBetween(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        const float leftLenSq = VrHandsDot(left, left);
        const float rightLenSq = VrHandsDot(right, right);
        if (!(leftLenSq > 0.000001f) || !(rightLenSq > 0.000001f))
            return 0.0f;
        const float invLen = 1.0f / std::sqrt(leftLenSq * rightLenSq);
        return std::acos(std::clamp(VrHandsDot(left, right) * invLen, -1.0f, 1.0f));
    }

    VrHandMatrix4 MakeVrHandsLocalZRotation(float radians)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        VrHandMath::Set(out, 0, 0, c);
        VrHandMath::Set(out, 0, 1, -s);
        VrHandMath::Set(out, 1, 0, s);
        VrHandMath::Set(out, 1, 1, c);
        return out;
    }

    VrHandMatrix4 BuildVrHandsSourceEntityWorld(const Vector& origin, const QAngle& angles)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);

        forward = VrHandMath::Normalize(forward);
        right = VrHandMath::Normalize(right);
        up = VrHandMath::Normalize(up);

        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, forward.x);
        VrHandMath::Set(out, 1, 0, forward.y);
        VrHandMath::Set(out, 2, 0, forward.z);
        VrHandMath::Set(out, 0, 1, right.x);
        VrHandMath::Set(out, 1, 1, right.y);
        VrHandMath::Set(out, 2, 1, right.z);
        VrHandMath::Set(out, 0, 2, up.x);
        VrHandMath::Set(out, 1, 2, up.y);
        VrHandMath::Set(out, 2, 2, up.z);
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    VrHandMatrix4 BuildVrHandsLocalCorrection(
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg,
        float sourceUnitsPerMeter)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        // Match the standalone OpenVR hand controls: R = Rz * Ry * Rx and
        // translation is converted from meters before model scaling.
        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, cz * cy);
        VrHandMath::Set(out, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(out, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(out, 1, 0, sz * cy);
        VrHandMath::Set(out, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(out, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(out, 2, 0, -sy);
        VrHandMath::Set(out, 2, 1, cy * sx);
        VrHandMath::Set(out, 2, 2, cy * cx);
        VrHandMath::Set(out, 0, 3, localPositionOffsetMeters.x * sourceUnitsPerMeter);
        VrHandMath::Set(out, 1, 3, localPositionOffsetMeters.y * sourceUnitsPerMeter);
        VrHandMath::Set(out, 2, 3, localPositionOffsetMeters.z * sourceUnitsPerMeter);
        return out;
    }

    bool GetVrHandsVmBonePositionInPalmSpace(
        const VrHandVmPose::Snapshot& snapshot,
        const VrHandMatrix4& palmInverse,
        const char* boneName,
        VrHandsVec3& outPosition)
    {
        const int bone = FindVrHandsNameIndex(snapshot.boneNames, boneName);
        if (bone < 0 || bone >= static_cast<int>(snapshot.boneWorldMatrices.size()))
            return false;
        outPosition = VrHandsMatrixTranslation(VrHandMath::Multiply(
            palmInverse,
            snapshot.boneWorldMatrices[static_cast<size_t>(bone)]));
        return true;
    }

    bool ComputeVrHandsVmFingerCurl(
        const VrHandVmPose::Snapshot& snapshot,
        const VrHandMatrix4& palmInverse,
        const VrHandsVmFingerSource& source,
        std::array<float, 3>& outCurl)
    {
        VrHandsVec3 palm{};
        VrHandsVec3 bone0{};
        VrHandsVec3 bone1{};
        VrHandsVec3 bone2{};
        if (!GetVrHandsVmBonePositionInPalmSpace(snapshot, palmInverse, source.bone0, bone0) ||
            !GetVrHandsVmBonePositionInPalmSpace(snapshot, palmInverse, source.bone1, bone1) ||
            !GetVrHandsVmBonePositionInPalmSpace(snapshot, palmInverse, source.bone2, bone2))
        {
            return false;
        }

        const VrHandsVec3 segment0 = VrHandsSubtract(bone0, palm);
        const VrHandsVec3 segment1 = VrHandsSubtract(bone1, bone0);
        const VrHandsVec3 segment2 = VrHandsSubtract(bone2, bone1);
        const float proximal = std::max(0.0f, VrHandsAngleBetween(segment0, segment1) - kVrHandsVmCurlDeadZoneRadians);
        const float middle = std::max(0.0f, VrHandsAngleBetween(segment1, segment2) - kVrHandsVmCurlDeadZoneRadians);
        outCurl[0] = std::clamp(proximal * source.proximalScale, 0.0f, source.maxProximal);
        outCurl[1] = std::clamp(middle * source.middleScale, 0.0f, source.maxMiddle);
        outCurl[2] = std::clamp(middle * source.distalScale, 0.0f, source.maxDistal);
        return true;
    }

    VrHandsVec3 VrHandsAdd(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        return { left.x + right.x, left.y + right.y, left.z + right.z };
    }

    VrHandsVec3 VrHandsScale(const VrHandsVec3& value, float scale)
    {
        return { value.x * scale, value.y * scale, value.z * scale };
    }

    VrHandsVec3 VrHandsCross(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        return
        {
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x
        };
    }

    bool VrHandsNormalize(VrHandsVec3& value)
    {
        const float lengthSq = VrHandsDot(value, value);
        if (!(lengthSq > 0.000001f))
            return false;
        value = VrHandsScale(value, 1.0f / std::sqrt(lengthSq));
        return true;
    }

    bool BuildVrHandsPalmBasis(
        const std::array<VrHandsVec3, 4>& fingerRoots,
        VrHandMatrix4& outBasis)
    {
        VrHandsVec3 forward{};
        for (const VrHandsVec3& root : fingerRoots)
            forward = VrHandsAdd(forward, root);
        forward = VrHandsScale(forward, 0.25f);

        VrHandsVec3 side = VrHandsSubtract(fingerRoots[0], fingerRoots[3]);
        if (!VrHandsNormalize(forward) || !VrHandsNormalize(side))
            return false;

        VrHandsVec3 normal = VrHandsCross(forward, side);
        if (!VrHandsNormalize(normal))
            return false;

        side = VrHandsCross(normal, forward);
        if (!VrHandsNormalize(side))
            return false;

        outBasis = VrHandMath::Identity();
        VrHandMath::Set(outBasis, 0, 0, forward.x);
        VrHandMath::Set(outBasis, 1, 0, forward.y);
        VrHandMath::Set(outBasis, 2, 0, forward.z);
        VrHandMath::Set(outBasis, 0, 1, side.x);
        VrHandMath::Set(outBasis, 1, 1, side.y);
        VrHandMath::Set(outBasis, 2, 1, side.z);
        VrHandMath::Set(outBasis, 0, 2, normal.x);
        VrHandMath::Set(outBasis, 1, 2, normal.y);
        VrHandMath::Set(outBasis, 2, 2, normal.z);
        return true;
    }

    VrHandMatrix4 TransposeVrHandsRotation(const VrHandMatrix4& matrix)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
                VrHandMath::Set(out, row, column, VrHandMath::Get(matrix, column, row));
        }
        return out;
    }

    bool GetVrHandsAssetBindPositionInWristSpace(
        const VrHandMeshAsset& asset,
        const VrHandMatrix4& wristInverse,
        const char* jointName,
        VrHandsVec3& outPosition)
    {
        const int joint = FindVrHandsNameIndex(asset.jointNames, jointName);
        if (joint < 0 || joint >= static_cast<int>(asset.bindMatrices.size()))
            return false;

        outPosition = VrHandsMatrixTranslation(VrHandMath::Multiply(
            wristInverse,
            asset.bindMatrices[static_cast<size_t>(joint)]));
        return true;
    }
}

VrHandSystem::VrHandSystem()
{
    m_Hands[0].actionPath = "/actions/base/in/skeleton_lefthand";
    m_Hands[0].assetFileName = "vr_glove_left_model.glb";
    m_Hands[1].actionPath = "/actions/base/in/skeleton_righthand";
    m_Hands[1].assetFileName = "vr_glove_right_model.glb";
    m_StandaloneMagazineBoxPalette = { VrHandMath::ToRows3x4(VrHandMath::Identity()) };
}

VrHandSystem::~VrHandSystem() = default;

int VrHandSystem::ViewmodelPoseStateIndex(int sourceSide)
{
    return (sourceSide < 0) ? 0 : 1;
}

void VrHandSystem::ResetViewmodelPoseState(ViewmodelPoseState& state, bool enabled, int targetHandIndex)
{
    state = ViewmodelPoseState{};
    state.enabled = enabled;
    state.targetHandIndex = targetHandIndex;
}

void VrHandSystem::ReportErrorOnce(const std::string& error)
{
    if (error.empty() || !m_ReportedErrors.emplace(error).second)
        return;
    Game::errorMsg(("[VR][Hands] " + error).c_str());
}

void VrHandSystem::ReportWarningOnce(const std::string& warning)
{
    if (warning.empty() || !m_ReportedWarnings.emplace(warning).second)
        return;
    Game::logMsg("[VR][Hands] %s", warning.c_str());
}

bool VrHandSystem::ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const
{
    outPath.clear();

    std::vector<char> buffer(vr::k_unMaxPropertyStringSize, '\0');
    uint32_t required = 0;
    if (!vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required))
        return false;
    if (required > buffer.size())
    {
        buffer.assign(static_cast<size_t>(required) + 1u, '\0');
        if (!vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required))
            return false;
    }
    if (buffer.empty() || buffer.front() == '\0')
        return false;

    if (!fileName || !*fileName)
        return false;
    try
    {
        const std::filesystem::path path = std::filesystem::u8path(buffer.data()) /
            "resources" / "rendermodels" / "vr_glove" / fileName;
        outPath = path.u8string();
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return false;
    }
}

bool VrHandSystem::EnsureStandaloneMagazineBoxLoaded(
    const Vector& mins,
    const Vector& maxs,
    std::uint32_t fallbackColorArgb,
    const char* debugName,
    bool debugLog)
{
    const char* boxName = (debugName && debugName[0] != '\0') ? debugName : "magazine_box";
    Vector boxMins(
        std::min(mins.x, maxs.x),
        std::min(mins.y, maxs.y),
        std::min(mins.z, maxs.z));
    Vector boxMaxs(
        std::max(mins.x, maxs.x),
        std::max(mins.y, maxs.y),
        std::max(mins.z, maxs.z));

    const float minExtent = 0.01f;
    if ((boxMaxs.x - boxMins.x) < minExtent)
    {
        const float center = (boxMaxs.x + boxMins.x) * 0.5f;
        boxMins.x = center - minExtent * 0.5f;
        boxMaxs.x = center + minExtent * 0.5f;
    }
    if ((boxMaxs.y - boxMins.y) < minExtent)
    {
        const float center = (boxMaxs.y + boxMins.y) * 0.5f;
        boxMins.y = center - minExtent * 0.5f;
        boxMaxs.y = center + minExtent * 0.5f;
    }
    if ((boxMaxs.z - boxMins.z) < minExtent)
    {
        const float center = (boxMaxs.z + boxMins.z) * 0.5f;
        boxMins.z = center - minExtent * 0.5f;
        boxMaxs.z = center + minExtent * 0.5f;
    }

    char key[192] = {};
    std::snprintf(
        key,
        sizeof(key),
        "generated:magazine_box:%08X:%.3f,%.3f,%.3f:%.3f,%.3f,%.3f",
        fallbackColorArgb,
        boxMins.x,
        boxMins.y,
        boxMins.z,
        boxMaxs.x,
        boxMaxs.y,
        boxMaxs.z);
    if (m_StandaloneMagazineBoxAsset.IsValid() && m_StandaloneMagazineBoxKey == key)
        return true;

    VrHandMeshAsset asset{};
    asset.jointNames = { "root" };
    asset.jointParents = { -1 };
    asset.bindMatrices = { VrHandMath::Identity() };
    asset.inverseBindMatrices = { VrHandMath::Identity() };
    asset.fallbackColorArgb = fallbackColorArgb;
    asset.sourcePath = key;

    auto addFace = [&](const Vector& a, const Vector& b, const Vector& c, const Vector& d, const Vector& normal)
    {
        const std::uint16_t base = static_cast<std::uint16_t>(asset.vertices.size());
        const Vector positions[4] = { a, b, c, d };
        const float uvs[4][2] =
        {
            { 0.0f, 0.0f },
            { 1.0f, 0.0f },
            { 1.0f, 1.0f },
            { 0.0f, 1.0f }
        };
        for (int i = 0; i < 4; ++i)
        {
            VrHandVertex vertex{};
            vertex.position[0] = positions[i].x;
            vertex.position[1] = positions[i].y;
            vertex.position[2] = positions[i].z;
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
            vertex.uv[0] = uvs[i][0];
            vertex.uv[1] = uvs[i][1];
            vertex.weights[0] = 1.0f;
            vertex.joints[0] = 0;
            asset.vertices.push_back(vertex);
        }
        asset.indices.push_back(base + 0);
        asset.indices.push_back(base + 1);
        asset.indices.push_back(base + 2);
        asset.indices.push_back(base + 0);
        asset.indices.push_back(base + 2);
        asset.indices.push_back(base + 3);
    };

    const float x0 = boxMins.x;
    const float y0 = boxMins.y;
    const float z0 = boxMins.z;
    const float x1 = boxMaxs.x;
    const float y1 = boxMaxs.y;
    const float z1 = boxMaxs.z;
    addFace(Vector(x0, y0, z0), Vector(x0, y1, z0), Vector(x0, y1, z1), Vector(x0, y0, z1), Vector(-1.0f, 0.0f, 0.0f));
    addFace(Vector(x1, y0, z0), Vector(x1, y0, z1), Vector(x1, y1, z1), Vector(x1, y1, z0), Vector(1.0f, 0.0f, 0.0f));
    addFace(Vector(x0, y0, z0), Vector(x0, y0, z1), Vector(x1, y0, z1), Vector(x1, y0, z0), Vector(0.0f, -1.0f, 0.0f));
    addFace(Vector(x0, y1, z0), Vector(x1, y1, z0), Vector(x1, y1, z1), Vector(x0, y1, z1), Vector(0.0f, 1.0f, 0.0f));
    addFace(Vector(x0, y0, z0), Vector(x1, y0, z0), Vector(x1, y1, z0), Vector(x0, y1, z0), Vector(0.0f, 0.0f, -1.0f));
    addFace(Vector(x0, y0, z1), Vector(x0, y1, z1), Vector(x1, y1, z1), Vector(x1, y0, z1), Vector(0.0f, 0.0f, 1.0f));

    m_StandaloneMagazineBoxAsset = std::move(asset);
    m_StandaloneMagazineBoxKey = key;
    if (debugLog)
    {
        Game::logMsg(
            "[VR][MagazineInteraction] generated %s debug box color=%08X mins=(%.2f %.2f %.2f) maxs=(%.2f %.2f %.2f)",
            boxName,
            fallbackColorArgb,
            boxMins.x,
            boxMins.y,
            boxMins.z,
            boxMaxs.x,
            boxMaxs.y,
            boxMaxs.z);
    }
    return true;
}

bool VrHandSystem::DrawStandaloneMagazineBox(
    IDirect3DDevice9* device,
    const CViewSetup& view,
    float sceneLightScale,
    const VrHandMatrix4* world,
    const Vector& mins,
    const Vector& maxs,
    std::uint32_t fallbackColorArgb,
    const char* debugName,
    bool useViewmodelLayer,
    VrHandDrawPass drawPass)
{
    const char* boxName = (debugName && debugName[0] != '\0') ? debugName : "magazine_box";
    if (!world)
        return false;
    if (useViewmodelLayer && drawPass == VrHandDrawPass::WorldVisibilityMask)
        return false;
    if (!EnsureStandaloneMagazineBoxLoaded(mins, maxs, fallbackColorArgb, boxName, false))
        return false;

    const float projectionAspect =
        useViewmodelLayer ? ResolveVrHandsViewmodelAspect(view) : ResolveVrHandsSceneAspect(view);
    const float projectionFov =
        (useViewmodelLayer && view.fovViewmodel > 0.001f) ? view.fovViewmodel : view.fov;
    const float projectionNear = useViewmodelLayer ? 0.10f : view.zNear;
    const float projectionFar =
        (useViewmodelLayer && view.zFarViewmodel > projectionNear) ? view.zFarViewmodel : view.zFar;

    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(
        projectionFov,
        projectionAspect,
        projectionNear,
        projectionFar);
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const VrHandMatrix4 cameraWorld = VrHandMath::Multiply(camera, *world);
    const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, cameraWorld);
    const VrHandDrawPass rendererPass =
        useViewmodelLayer ? VrHandDrawPass::ViewmodelStandalone : drawPass;

    std::string error;
    if (!m_Renderer.Draw(
            device,
            2,
            m_StandaloneMagazineBoxAsset,
            m_StandaloneMagazineBoxPalette,
            *world,
            wvp,
            rendererPass,
            sceneLightScale,
            error))
    {
        ReportErrorOnce(error);
        return false;
    }

    if (rendererPass != VrHandDrawPass::WorldVisibilityMask &&
        m_StandaloneMagazineBoxDrawLoggedKeys.insert(std::string(boxName) + ":" + m_StandaloneMagazineBoxKey).second)
    {
        const char* passName = "world-depth";
        if (rendererPass == VrHandDrawPass::ViewmodelComposite)
            passName = "viewmodel-composite";
        else if (rendererPass == VrHandDrawPass::ViewmodelStandalone)
            passName = "viewmodel-standalone";

        Game::logMsg(
            "[VR][MagazineInteraction] drew %s debug box world=(%.2f %.2f %.2f) camera=(%.2f %.2f %.2f) pass=%s fov=%.2f near=%.2f",
            boxName,
            VrHandMath::Get(*world, 0, 3),
            VrHandMath::Get(*world, 1, 3),
            VrHandMath::Get(*world, 2, 3),
            VrHandMath::Get(cameraWorld, 0, 3),
            VrHandMath::Get(cameraWorld, 1, 3),
            VrHandMath::Get(cameraWorld, 2, 3),
            passName,
            projectionFov,
            projectionNear);
    }
    return true;
}

bool VrHandSystem::DrawMagazineDebugBoxesForEye(
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
    VrHandDrawPass drawPass)
{
    bool drewAny = false;
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            standaloneMagazineBoxWorld,
            standaloneMagazineBoxMins,
            standaloneMagazineBoxMaxs,
            kMagazineDebugFreshBoxColorArgb,
            "fresh_magazine",
            standaloneMagazineBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            magazineSocketCaptureBoxWorld,
            magazineSocketCaptureBoxMins,
            magazineSocketCaptureBoxMaxs,
            kMagazineDebugSocketCaptureBoxColorArgb,
            "socket_capture",
            magazineSocketCaptureBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            currentMagazineBoxWorld,
            currentMagazineBoxMins,
            currentMagazineBoxMaxs,
            kMagazineDebugCurrentMagazineBoxColorArgb,
            "current_magazine",
            currentMagazineBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            currentBoltBoxWorld,
            currentBoltBoxMins,
            currentBoltBoxMaxs,
            kMagazineDebugBoltBoxColorArgb,
            "current_bolt",
            currentBoltBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            leftHandTargetBoxWorld,
            leftHandTargetBoxMins,
            leftHandTargetBoxMaxs,
            kLeftHandTargetBoxColorArgb,
            "left_hand_target",
            leftHandTargetBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    return drewAny;
}

bool VrHandSystem::EnsureAssetsLoaded(bool debugLog)
{
    if (m_AssetsLoaded)
        return true;

    const std::uint32_t now = GetTickCount();
    bool loadedNewAsset = false;

    for (HandState& hand : m_Hands)
    {
        if (hand.asset.IsValid())
            continue;
        if (hand.assetLoadAttempted && now - hand.assetLoadAttemptTick < 1000u)
            continue;
        hand.assetLoadAttempted = true;
        hand.assetLoadAttemptTick = now;

        std::string assetPath;
        if (!ResolveSteamVrAssetPath(hand.assetFileName, assetPath))
        {
            hand.assetFailureReason = std::string("missing SteamVR glove asset: ") +
                (hand.assetFileName ? hand.assetFileName : "<unknown>");
            continue;
        }

        VrHandMeshAsset asset;
        std::string error;
        if (!VrHandAssetLoader::LoadGlb(assetPath, asset, error) || !asset.IsValid())
        {
            hand.assetFailureReason = "failed to load SteamVR glove asset: " + assetPath;
            if (!error.empty())
                hand.assetFailureReason += " (" + error + ")";
            continue;
        }
        hand.asset = std::move(asset);
        hand.assetFailureReason.clear();
        loadedNewAsset = true;
    }

    bool anyAssetLoaded = false;
    m_AssetsLoaded = true;
    m_DependencyFailureReason.clear();
    for (const HandState& hand : m_Hands)
    {
        anyAssetLoaded |= hand.asset.IsValid();
        m_AssetsLoaded &= hand.asset.IsValid();
        if (!hand.asset.IsValid() && m_DependencyFailureReason.empty())
            m_DependencyFailureReason = hand.assetFailureReason;
        if (debugLog && !hand.assetFailureReason.empty())
            ReportWarningOnce(hand.assetFailureReason);
    }
    m_DependencyUnavailable = !anyAssetLoaded;
    if (debugLog && loadedNewAsset)
    {
        Game::logMsg(
            "[VR][Hands] assets loaded left=%u vertices right=%u vertices",
            static_cast<unsigned int>(m_Hands[0].asset.vertices.size()),
            static_cast<unsigned int>(m_Hands[1].asset.vertices.size()));
    }
    return anyAssetLoaded;
}

bool VrHandSystem::EnsureInitialized(
    vr::IVRInput* input,
    const l4d2vr_input::ActionFrame& actionFrame,
    const VrHandControllerContext& controllers,
    bool rightUseViewmodelPose,
    bool twoHandedViewmodelPose,
    bool leftHanded,
    bool debugLog)
{
    if (!input || !EnsureAssetsLoaded(debugLog))
        return false;
    const auto frameToken = actionFrame.Read();

    const int rightViewmodelPoseHandIndex = rightUseViewmodelPose ? (leftHanded ? 0 : 1) : -1;
    const int leftViewmodelPoseHandIndex = twoHandedViewmodelPose ? (leftHanded ? 1 : 0) : -1;
    auto usesViewmodelPose = [&](size_t handIndex) -> bool
        {
            const int index = static_cast<int>(handIndex);
            return index == rightViewmodelPoseHandIndex || index == leftViewmodelPoseHandIndex;
        };
    for (size_t handIndex = 0; handIndex < m_Hands.size(); ++handIndex)
    {
        HandState& hand = m_Hands[handIndex];
        if (hand.skeletonInitialized)
            continue;
        if (!l4d2vr_input::ActionFrame::Ready(frameToken))
            continue;
        if (hand.source == vr::k_ulInvalidInputValueHandle &&
            (input->GetInputSourceHandle(handIndex == 0u ? "/user/hand/left" : "/user/hand/right", &hand.source) !=
                vr::VRInputError_None || hand.source == vr::k_ulInvalidInputValueHandle))
            continue;

        if (hand.action == vr::k_ulInvalidActionHandle &&
            (input->GetActionHandle(hand.actionPath, &hand.action) != vr::VRInputError_None ||
                hand.action == vr::k_ulInvalidActionHandle))
        {
            m_DependencyFailureReason = std::string("missing SteamVR skeletal action: ") +
                (hand.actionPath ? hand.actionPath : "<unknown>");
            continue;
        }

        vr::InputSkeletalActionData_t actionData{};
        const vr::EVRInputError actionDataResult = input->GetSkeletalActionData(hand.action, &actionData, sizeof(actionData));
        if (actionDataResult != vr::VRInputError_None || !actionData.bActive ||
            !l4d2vr_hand_input::ValidateOrigin(input, controllers.system, actionData.activeOrigin,
                hand.source, controllers.physicalIndices[handIndex]))
        {
            if (debugLog)
            {
                static DWORD lastLogTick = 0;
                const DWORD now = GetTickCount();
                if (now - lastLogTick >= 1000u)
                {
                    lastLogTick = now;
                    Game::logMsg("[VR][Hands] waiting for active skeletal action: %s", hand.actionPath);
                }
            }
            continue;
        }

        std::string error;
        if (!hand.skeleton.Initialize(input, hand.action, error))
        {
            m_DependencyFailureReason = "failed to initialize SteamVR hand skeleton";
            if (!error.empty())
                m_DependencyFailureReason += ": " + error;
            continue;
        }
        if (actionFrame.Matches(frameToken))
            hand.skeletonInitialized = true;
    }

    m_Initialized = false;
    for (size_t handIndex = 0; handIndex < m_Hands.size(); ++handIndex)
    {
        if (usesViewmodelPose(handIndex) || m_Hands[handIndex].skeletonInitialized)
            m_Initialized = true;
    }
    if (m_Initialized && debugLog && !m_DebugInitializationLogged)
    {
        m_DebugInitializationLogged = true;
        const char* leftMode = usesViewmodelPose(0u) ? "viewmodel" : m_Hands[0].skeletonInitialized ? "openvr" : "pending";
        const char* rightMode = usesViewmodelPose(1u) ? "viewmodel" : m_Hands[1].skeletonInitialized ? "openvr" : "pending";
        Game::logMsg(
            "[VR][Hands] initialized left=%s/%u vertices right=%s/%u vertices",
            leftMode,
            static_cast<unsigned int>(m_Hands[0].asset.vertices.size()),
            rightMode,
            static_cast<unsigned int>(m_Hands[1].asset.vertices.size()));
    }
    return m_Initialized;
}

bool VrHandSystem::BuildViewmodelPalette(
    const VrHandVmPose::Snapshot& snapshot,
    int sourceSide,
    int targetHandIndex,
    const std::array<float, 5>& gloveFingerMaxCurl,
    ViewmodelPoseState& state)
{
    state.palette.clear();
    state.paletteValid = false;
    state.palmWorldValid = false;

    if (targetHandIndex < 0 || targetHandIndex >= static_cast<int>(m_Hands.size()))
        return false;

    const bool sourceLeftHand = sourceSide < 0;
    const bool targetLeftHand = targetHandIndex == 0;
    HandState& hand = m_Hands[static_cast<size_t>(targetHandIndex)];
    const VrHandMeshAsset& asset = hand.asset;
    if (!asset.IsValid() || snapshot.boneWorldMatrices.size() < snapshot.boneNames.size())
        return false;

    const int vmPalm = FindVrHandsNameIndex(
        snapshot.boneNames,
        sourceLeftHand ? "ValveBiped.Bip01_L_Hand" : "ValveBiped.Bip01_R_Hand");
    if (vmPalm < 0 || vmPalm >= static_cast<int>(snapshot.boneWorldMatrices.size()))
        return false;

    if (state.poseModel != snapshot.modelName ||
        state.targetHandIndex != targetHandIndex)
    {
        state.poseModel = snapshot.modelName;
        state.targetHandIndex = targetHandIndex;
        state.anchorModel.clear();
        state.anchorValid = false;
    }
    state.palmWorld = snapshot.boneWorldMatrices[static_cast<size_t>(vmPalm)];
    state.palmWorldValid = true;
    state.autoGripAligned = snapshot.autoGripAligned;

    VrHandMatrix4 modelWorldInverse{};
    if (!VrHandMath::Invert4x4(snapshot.modelWorldMatrix, modelWorldInverse))
    {
        state.palmWorldValid = false;
        state.palmFromModelRootValid = false;
        return false;
    }
    state.palmFromModelRoot = VrHandMath::Multiply(
        modelWorldInverse,
        state.palmWorld);
    state.palmFromModelRootValid = true;

    VrHandMatrix4 palmInverse{};
    if (!VrHandMath::Invert4x4(state.palmWorld, palmInverse))
    {
        state.palmWorldValid = false;
        return false;
    }

    if (!state.anchorValid || state.anchorModel != snapshot.modelName)
    {
        const int gloveWrist = FindVrHandsNameIndex(asset.jointNames, targetLeftHand ? "wrist_l" : "wrist_r");
        if (gloveWrist < 0 || gloveWrist >= static_cast<int>(asset.bindMatrices.size()) ||
            !VrHandMath::Invert4x4(
                asset.bindMatrices[static_cast<size_t>(gloveWrist)],
                state.gloveWristBindInverse))
        {
            state.palmWorldValid = false;
            return false;
        }

        static const char* kVmFingerRootsRight[] =
        {
            "ValveBiped.Bip01_R_Finger1",
            "ValveBiped.Bip01_R_Finger2",
            "ValveBiped.Bip01_R_Finger3",
            "ValveBiped.Bip01_R_Finger4"
        };
        static const char* kVmFingerRootsLeft[] =
        {
            "ValveBiped.Bip01_L_Finger1",
            "ValveBiped.Bip01_L_Finger2",
            "ValveBiped.Bip01_L_Finger3",
            "ValveBiped.Bip01_L_Finger4"
        };
        static const char* kGloveFingerRootsRight[] =
        {
            "finger_index_meta_r",
            "finger_middle_meta_r",
            "finger_ring_meta_r",
            "finger_pinky_meta_r"
        };
        static const char* kGloveFingerRootsLeft[] =
        {
            "finger_index_meta_l",
            "finger_middle_meta_l",
            "finger_ring_meta_l",
            "finger_pinky_meta_l"
        };
        const char* const* gloveFingerRootNames = targetLeftHand ? kGloveFingerRootsLeft : kGloveFingerRootsRight;
        const char* const* vmFingerRootNames = sourceLeftHand ? kVmFingerRootsLeft : kVmFingerRootsRight;

        std::array<VrHandsVec3, 4> vmFingerRoots{};
        std::array<VrHandsVec3, 4> gloveFingerRootPositions{};
        for (size_t finger = 0; finger < vmFingerRoots.size(); ++finger)
        {
            if (!GetVrHandsVmBonePositionInPalmSpace(
                    snapshot,
                    palmInverse,
                    vmFingerRootNames[finger],
                    vmFingerRoots[finger]) ||
                !GetVrHandsAssetBindPositionInWristSpace(
                    asset,
                    state.gloveWristBindInverse,
                    gloveFingerRootNames[finger],
                    gloveFingerRootPositions[finger]))
            {
                state.palmWorldValid = false;
                return false;
            }
        }

        VrHandMatrix4 vmPalmBasis{};
        VrHandMatrix4 glovePalmBasis{};
        if (!BuildVrHandsPalmBasis(vmFingerRoots, vmPalmBasis) ||
            !BuildVrHandsPalmBasis(gloveFingerRootPositions, glovePalmBasis))
        {
            state.palmWorldValid = false;
            return false;
        }

        state.palmFromGloveWrist = VrHandMath::Multiply(
            vmPalmBasis,
            TransposeVrHandsRotation(glovePalmBasis));
        state.anchorModel = snapshot.modelName;
        state.anchorValid = true;
    }

    std::vector<VrHandMatrix4> localMatrices(asset.jointNames.size(), VrHandMath::Identity());
    for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
        localMatrices[joint] = BuildVrHandsBindLocalMatrix(asset, static_cast<int>(joint));

    static const VrHandsVmFingerSource kSourcesRight[] =
    {
        { "ValveBiped.Bip01_R_Finger0", "ValveBiped.Bip01_R_Finger01", "ValveBiped.Bip01_R_Finger02", 0.45f, 0.75f, 0.50f, 0.75f, 0.90f, 0.65f },
        { "ValveBiped.Bip01_R_Finger1", "ValveBiped.Bip01_R_Finger11", "ValveBiped.Bip01_R_Finger12" },
        { "ValveBiped.Bip01_R_Finger2", "ValveBiped.Bip01_R_Finger21", "ValveBiped.Bip01_R_Finger22" },
        { "ValveBiped.Bip01_R_Finger3", "ValveBiped.Bip01_R_Finger31", "ValveBiped.Bip01_R_Finger32" },
        { "ValveBiped.Bip01_R_Finger4", "ValveBiped.Bip01_R_Finger41", "ValveBiped.Bip01_R_Finger42" },
    };
    static const VrHandsVmFingerSource kSourcesLeft[] =
    {
        { "ValveBiped.Bip01_L_Finger0", "ValveBiped.Bip01_L_Finger01", "ValveBiped.Bip01_L_Finger02", 0.45f, 0.75f, 0.50f, 0.75f, 0.90f, 0.65f },
        { "ValveBiped.Bip01_L_Finger1", "ValveBiped.Bip01_L_Finger11", "ValveBiped.Bip01_L_Finger12" },
        { "ValveBiped.Bip01_L_Finger2", "ValveBiped.Bip01_L_Finger21", "ValveBiped.Bip01_L_Finger22" },
        { "ValveBiped.Bip01_L_Finger3", "ValveBiped.Bip01_L_Finger31", "ValveBiped.Bip01_L_Finger32" },
        { "ValveBiped.Bip01_L_Finger4", "ValveBiped.Bip01_L_Finger41", "ValveBiped.Bip01_L_Finger42" },
    };
    static const VrHandsGloveFingerChain kChainsRight[] =
    {
        { "finger_thumb_0_r", "finger_thumb_1_r", "finger_thumb_2_r" },
        { "finger_index_0_r", "finger_index_1_r", "finger_index_2_r" },
        { "finger_middle_0_r", "finger_middle_1_r", "finger_middle_2_r" },
        { "finger_ring_0_r", "finger_ring_1_r", "finger_ring_2_r" },
        { "finger_pinky_0_r", "finger_pinky_1_r", "finger_pinky_2_r" },
    };
    static const VrHandsGloveFingerChain kChainsLeft[] =
    {
        { "finger_thumb_0_l", "finger_thumb_1_l", "finger_thumb_2_l" },
        { "finger_index_0_l", "finger_index_1_l", "finger_index_2_l" },
        { "finger_middle_0_l", "finger_middle_1_l", "finger_middle_2_l" },
        { "finger_ring_0_l", "finger_ring_1_l", "finger_ring_2_l" },
        { "finger_pinky_0_l", "finger_pinky_1_l", "finger_pinky_2_l" },
    };
    const VrHandsGloveFingerChain* chains = targetLeftHand ? kChainsLeft : kChainsRight;
    const VrHandsVmFingerSource* sources = sourceLeftHand ? kSourcesLeft : kSourcesRight;

    int appliedFingerCount = 0;
    for (size_t finger = 0; finger < 5u; ++finger)
    {
        std::array<float, 3> curl{};
        if (!ComputeVrHandsVmFingerCurl(snapshot, palmInverse, sources[finger], curl))
            continue;
        const float maxCurl = ResolveVrHandsFingerMaxCurl(gloveFingerMaxCurl, finger);
        curl[0] = std::min(curl[0], sources[finger].maxProximal * maxCurl);
        curl[1] = std::min(curl[1], sources[finger].maxMiddle * maxCurl);
        curl[2] = std::min(curl[2], sources[finger].maxDistal * maxCurl);

        const char* joints[] = { chains[finger].joint0, chains[finger].joint1, chains[finger].joint2 };
        bool applied = true;
        for (size_t segment = 0; segment < 3u; ++segment)
        {
            const int joint = FindVrHandsNameIndex(asset.jointNames, joints[segment]);
            if (joint < 0)
            {
                applied = false;
                break;
            }

            // SteamVR glove rigs use positive local-Z flexion for into-palm curl.
            // Keep the selected viewmodel hand as the curl source, but write it
            // into the selected left/right glove chain.
            localMatrices[static_cast<size_t>(joint)] = VrHandMath::Multiply(
                localMatrices[static_cast<size_t>(joint)],
                MakeVrHandsLocalZRotation(curl[segment]));
        }
        if (applied)
            ++appliedFingerCount;
    }
    if (appliedFingerCount < 5)
    {
        state.palmWorldValid = false;
        return false;
    }

    std::vector<VrHandMatrix4> modelMatrices(asset.jointNames.size(), VrHandMath::Identity());
    std::vector<bool> resolved(asset.jointNames.size(), false);
    size_t unresolved = asset.jointNames.size();
    for (size_t pass = 0; pass < asset.jointNames.size() && unresolved > 0; ++pass)
    {
        bool progressed = false;
        for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
        {
            if (resolved[joint])
                continue;
            const int parent = asset.jointParents[joint];
            if (parent >= 0 && (parent >= static_cast<int>(resolved.size()) || !resolved[static_cast<size_t>(parent)]))
                continue;
            modelMatrices[joint] = (parent >= 0)
                ? VrHandMath::Multiply(modelMatrices[static_cast<size_t>(parent)], localMatrices[joint])
                : localMatrices[joint];
            resolved[joint] = true;
            --unresolved;
            progressed = true;
        }
        if (!progressed)
        {
            state.palmWorldValid = false;
            return false;
        }
    }

    state.palette.resize(asset.jointNames.size());
    for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
    {
        state.palette[joint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(
            modelMatrices[joint],
            asset.inverseBindMatrices[joint]));
    }
    state.paletteValid = true;
    return true;
}

bool VrHandSystem::BuildViewmodelWorld(
    const ViewmodelPoseState& state,
    float sourceUnitsPerMeter,
    float modelScale,
    const Vector& localPositionOffsetMeters,
    const Vector& localRotationOffsetDeg,
    bool anchorToCurrentViewmodelRoot,
    const Vector& currentViewmodelPosition,
    const QAngle& currentViewmodelAngles,
    VrHandMatrix4& outWorld) const
{
    if (!state.palmWorldValid || !state.anchorValid ||
        !(sourceUnitsPerMeter > 0.0001f) || !(modelScale > 0.0001f))
    {
        return false;
    }

    const float uniformScale = sourceUnitsPerMeter * modelScale;
    VrHandMatrix4 scale = VrHandMath::Identity();
    VrHandMath::Set(scale, 0, 0, uniformScale);
    VrHandMath::Set(scale, 1, 1, uniformScale);
    VrHandMath::Set(scale, 2, 2, uniformScale);

    const VrHandMatrix4 localCorrection = BuildVrHandsLocalCorrection(
        localPositionOffsetMeters,
        localRotationOffsetDeg,
        sourceUnitsPerMeter);

    VrHandMatrix4 palmWorld = state.palmWorld;
    if (anchorToCurrentViewmodelRoot)
    {
        if (!state.palmFromModelRootValid)
            return false;
        palmWorld = VrHandMath::Multiply(
            BuildVrHandsSourceEntityWorld(currentViewmodelPosition, currentViewmodelAngles),
            state.palmFromModelRoot);
    }

    outWorld = VrHandMath::Multiply(
        palmWorld,
        VrHandMath::Multiply(
            state.palmFromGloveWrist,
            VrHandMath::Multiply(
                localCorrection,
                VrHandMath::Multiply(scale, state.gloveWristBindInverse))));
    return true;
}

void VrHandSystem::UpdatePoses(
    vr::IVRInput* input,
    const l4d2vr_input::ActionFrame& actionFrame,
    const VrHandControllerContext& controllers,
    bool motionRangeWithoutController,
    bool rightUseViewmodelPose,
    bool twoHandedViewmodelPose,
    bool leftHanded,
    unsigned magazineGripPhysicalMask,
    const std::array<float, 5>& gloveFingerMaxCurl,
    bool debugLog)
{
    m_LastPoseUpdateTick = GetTickCount();
    {
        std::lock_guard<std::mutex> lock(m_FreeWristMutex);
        m_FreeWrists = {};
    }
    auto physicalHandIndexForGameplay = [&](int gameplayHandIndex) -> int
        {
            return leftHanded ? (1 - gameplayHandIndex) : gameplayHandIndex;
        };
    auto prepareState = [&](int sourceSide, bool enabled, int targetHandIndex) -> ViewmodelPoseState&
        {
            ViewmodelPoseState& state =
                m_ViewmodelPoseStates[static_cast<size_t>(ViewmodelPoseStateIndex(sourceSide))];
            if (state.enabled != enabled || state.targetHandIndex != targetHandIndex)
                ResetViewmodelPoseState(state, enabled, targetHandIndex);
            state.paletteValid = false;
            state.palmWorldValid = false;
            return state;
        };

    ViewmodelPoseState& leftViewmodelState =
        prepareState(-1, twoHandedViewmodelPose, physicalHandIndexForGameplay(0));
    ViewmodelPoseState& rightViewmodelState =
        prepareState(1, rightUseViewmodelPose, physicalHandIndexForGameplay(1));

    const vr::EVRSkeletalMotionRange motionRange = motionRangeWithoutController
        ? vr::VRSkeletalMotionRange_WithoutController
        : vr::VRSkeletalMotionRange_WithController;

    VrHandFingerCurlOverride magazineGripOverride{};
    if (magazineGripPhysicalMask)
    {
        magazineGripOverride.enabled = true;
        magazineGripOverride.minCurl = { 0.34f, 0.60f, 0.66f, 0.68f, 0.68f };
        magazineGripOverride.maxCurl = { 0.58f, 0.82f, 0.88f, 0.90f, 0.90f };
    }

    for (size_t handIndex = 0; handIndex < m_Hands.size(); ++handIndex)
    {
        HandState& hand = m_Hands[handIndex];
        hand.paletteValid = false;
        hand.paletteFrameToken = 0u;
        if (!hand.skeletonInitialized)
            continue;

        std::string error;
        if (!hand.skeleton.Update(input, actionFrame, controllers.system, hand.source,
                controllers.physicalIndices[handIndex], motionRange, error))
        {
            continue;
        }
        const VrHandFingerCurlOverride* fingerCurlOverride =
            ((magazineGripPhysicalMask & (1u << handIndex)) != 0u) ? &magazineGripOverride : nullptr;
        if (!hand.skeleton.BuildSkinningPalette(hand.asset, hand.palette, error, fingerCurlOverride, &gloveFingerMaxCurl))
        {
            ReportErrorOnce(std::string(handIndex == 0u ? "left" : "right") +
                " hand skinning palette unavailable" + (error.empty() ? "" : ": " + error));
            continue;
        }
        hand.paletteValid = true;
        hand.paletteFrameToken = hand.skeleton.PoseFrameToken();
        PublishFreeWristModelPosition(handIndex);
    }

    bool hasLeftViewmodelPose = false;
    bool hasRightViewmodelPose = false;
    if (rightUseViewmodelPose || twoHandedViewmodelPose)
    {
        VrHandVmPose::Snapshot snapshot{};
        if (VrHandVmPose::GetLatest(snapshot, kVrHandsVmPoseMaxAgeMs))
        {
            if (twoHandedViewmodelPose &&
                BuildViewmodelPalette(snapshot, -1, leftViewmodelState.targetHandIndex, gloveFingerMaxCurl, leftViewmodelState))
            {
                hasLeftViewmodelPose = true;
                if (debugLog && !leftViewmodelState.debugLogged)
                {
                    leftViewmodelState.debugLogged = true;
                    Game::logMsg(
                        "[VR][Hands] left hand uses viewmodel pose model=\"%s\" seq=%u",
                        snapshot.modelName.c_str(),
                        snapshot.sequence);
                }
            }

            if (rightUseViewmodelPose &&
                BuildViewmodelPalette(snapshot, 1, rightViewmodelState.targetHandIndex, gloveFingerMaxCurl, rightViewmodelState))
            {
                hasRightViewmodelPose = true;
                if (debugLog && !rightViewmodelState.debugLogged)
                {
                    rightViewmodelState.debugLogged = true;
                    Game::logMsg(
                        "[VR][Hands] right hand uses viewmodel pose model=\"%s\" seq=%u",
                        snapshot.modelName.c_str(),
                        snapshot.sequence);
                }
            }
        }

        if (debugLog && twoHandedViewmodelPose && !hasLeftViewmodelPose && !leftViewmodelState.debugMissingLogged)
        {
            leftViewmodelState.debugMissingLogged = true;
            Game::logMsg("[VR][Hands] left viewmodel pose enabled; waiting for a valid VM arm snapshot");
        }
        if (debugLog && rightUseViewmodelPose && !hasRightViewmodelPose && !rightViewmodelState.debugMissingLogged)
        {
            rightViewmodelState.debugMissingLogged = true;
            Game::logMsg("[VR][Hands] right viewmodel pose enabled; waiting for a valid VM arm snapshot");
        }
    }

    if (debugLog)
    {
        static DWORD lastLogTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastLogTick >= 1000u)
        {
            lastLogTick = now;
            Game::logMsg(
                "[VR][Hands] pose left=%d right=%d leftVm=%d rightVm=%d leftSource=%s rightSource=%s motionRange=%s",
                m_Hands[0].paletteValid ? 1 : 0,
                m_Hands[1].paletteValid ? 1 : 0,
                leftViewmodelState.paletteValid ? 1 : 0,
                rightViewmodelState.paletteValid ? 1 : 0,
                twoHandedViewmodelPose ? (hasLeftViewmodelPose ? "viewmodel" : "viewmodel-waiting") : "openvr",
                rightUseViewmodelPose ? (hasRightViewmodelPose ? "viewmodel" : "viewmodel-waiting") : "openvr",
                motionRangeWithoutController ? "without-controller" : "with-controller");
        }
    }
}

void VrHandSystem::PublishFreeWristModelPosition(size_t handIndex)
{
    if (handIndex >= m_Hands.size()) return;
    FreeWristSnapshot snapshot{};
    const auto& hand = m_Hands[handIndex];
    const int wrist = FindVrHandsNameIndex(hand.asset.jointNames, handIndex == 0u ? "wrist_l" : "wrist_r");
    if (hand.paletteValid && hand.asset.IsValid() && wrist >= 0 &&
        wrist < static_cast<int>(hand.palette.size()) && wrist < static_cast<int>(hand.asset.bindMatrices.size()))
    {
        VrHandMatrix4 skin = VrHandMath::Identity();
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                VrHandMath::Set(skin, row, col, hand.palette[static_cast<size_t>(wrist)].v[static_cast<size_t>(row) * 4u + col]);
        const auto model = VrHandMath::Multiply(skin, hand.asset.bindMatrices[static_cast<size_t>(wrist)]);
        snapshot.position = Vector(VrHandMath::Get(model, 0, 3), VrHandMath::Get(model, 1, 3), VrHandMath::Get(model, 2, 3));
        snapshot.valid = std::isfinite(snapshot.position.x) && std::isfinite(snapshot.position.y) && std::isfinite(snapshot.position.z);
        snapshot.frameToken = hand.paletteFrameToken;
        snapshot.updatedAt = m_LastPoseUpdateTick;
    }
    std::lock_guard<std::mutex> lock(m_FreeWristMutex);
    m_FreeWrists[handIndex] = snapshot;
}

bool VrHandSystem::ReadFreeWristModelPosition(bool physicalLeft,
    const l4d2vr_input::ActionFrame& actionFrame, Vector& position) const
{
    FreeWristSnapshot snapshot{};
    {
        std::lock_guard<std::mutex> lock(m_FreeWristMutex);
        snapshot = m_FreeWrists[physicalLeft ? 0u : 1u];
    }
    if (!snapshot.valid || !l4d2vr_hand_pose::Available(actionFrame.Read(), snapshot.frameToken,
            snapshot.updatedAt, GetTickCount())) return false;
    position = snapshot.position;
    return true;
}

bool VrHandSystem::DrawForEye(
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
    VrHandDrawPass drawPass)
{
    if (!device || !input)
        return false;
    if (!EnsureInitialized(input, actionFrame, controllers, rightUseViewmodelPose, twoHandedViewmodelPose, leftHanded, debugLog))
    {
        if (m_DependencyUnavailable)
            return false;

        bool drewAny = false;
        if (allowControllerlessTestPose)
        {
            drewAny = DrawControllerlessTestPose(
                device,
                view,
                vrScale,
                modelScale,
                rightUseViewmodelPose,
                twoHandedViewmodelPose,
                leftHanded,
                sceneLightScale,
                debugLog,
                leftHandPoseOffsetMeters,
                leftHandPoseRotationOffsetDeg,
                rightHandPoseOffsetMeters,
                rightHandPoseRotationOffsetDeg,
                drawPass);
        }

        if (DrawStandaloneMagazineBox(
                device,
                view,
                sceneLightScale,
                standaloneMagazineBoxWorld,
                standaloneMagazineBoxMins,
                standaloneMagazineBoxMaxs,
                kMagazineDebugFreshBoxColorArgb,
                "fresh_magazine",
                standaloneMagazineBoxUseViewmodelLayer,
                drawPass))
        {
            drewAny = true;
        }
        if (DrawStandaloneMagazineBox(
                device,
                view,
                sceneLightScale,
                magazineSocketCaptureBoxWorld,
                magazineSocketCaptureBoxMins,
                magazineSocketCaptureBoxMaxs,
                kMagazineDebugSocketCaptureBoxColorArgb,
                "socket_capture",
                magazineSocketCaptureBoxUseViewmodelLayer,
                drawPass))
        {
            drewAny = true;
        }
        if (DrawStandaloneMagazineBox(
                device,
                view,
                sceneLightScale,
                currentMagazineBoxWorld,
                currentMagazineBoxMins,
                currentMagazineBoxMaxs,
                kMagazineDebugCurrentMagazineBoxColorArgb,
                "current_magazine",
                currentMagazineBoxUseViewmodelLayer,
                drawPass))
        {
            drewAny = true;
        }
        if (DrawStandaloneMagazineBox(
                device,
                view,
                sceneLightScale,
                currentBoltBoxWorld,
                currentBoltBoxMins,
                currentBoltBoxMaxs,
                kMagazineDebugBoltBoxColorArgb,
                "current_bolt",
                currentBoltBoxUseViewmodelLayer,
                drawPass))
        {
            drewAny = true;
        }
        if (DrawStandaloneMagazineBox(
                device,
                view,
                sceneLightScale,
                leftHandTargetBoxWorld,
                leftHandTargetBoxMins,
                leftHandTargetBoxMaxs,
                kLeftHandTargetBoxColorArgb,
                "left_hand_target",
                leftHandTargetBoxUseViewmodelLayer,
                drawPass))
        {
            drewAny = true;
        }
        return drewAny;
    }

    if (m_MagazineGripPhysicalMaskWasEnabled != magazineGripPhysicalMask)
    {
        m_MagazineGripPhysicalMaskWasEnabled = magazineGripPhysicalMask;
        m_Hands[0].paletteValid = false;
        m_Hands[1].paletteValid = false;
    }

    auto physicalHandIndexForGameplay = [&](int gameplayHandIndex) -> int
        {
            return leftHanded ? (1 - gameplayHandIndex) : gameplayHandIndex;
        };
    const ViewmodelPoseState& leftViewmodelState =
        m_ViewmodelPoseStates[static_cast<size_t>(ViewmodelPoseStateIndex(-1))];
    const ViewmodelPoseState& rightViewmodelState =
        m_ViewmodelPoseStates[static_cast<size_t>(ViewmodelPoseStateIndex(1))];
    auto needsPoseUpdate = [&]() -> bool
        {
            if (rightUseViewmodelPose || twoHandedViewmodelPose || eyeIndex == 0)
                return true;

            for (int gameplayHandIndex = 0; gameplayHandIndex < static_cast<int>(m_Hands.size()); ++gameplayHandIndex)
            {
                const bool useLeftViewmodelPose = gameplayHandIndex == 0 && twoHandedViewmodelPose;
                const bool useRightViewmodelPose = gameplayHandIndex == 1 && rightUseViewmodelPose;
                if (useLeftViewmodelPose || useRightViewmodelPose)
                {
                    const ViewmodelPoseState& state =
                        useLeftViewmodelPose ? leftViewmodelState : rightViewmodelState;
                    if (!state.paletteValid)
                        return true;
                    continue;
                }

                const int physicalHandIndex = physicalHandIndexForGameplay(gameplayHandIndex);
                if (physicalHandIndex < 0 || physicalHandIndex >= static_cast<int>(m_Hands.size()))
                    return true;
                const HandState& hand = m_Hands[static_cast<size_t>(physicalHandIndex)];
                if (!hand.paletteValid || !l4d2vr_hand_pose::Available(actionFrame.Read(), hand.paletteFrameToken,
                        m_LastPoseUpdateTick, GetTickCount()))
                    return true;
            }
            return false;
        };

    if (needsPoseUpdate())
        UpdatePoses(
            input,
            actionFrame,
            controllers,
            motionRangeWithoutController,
            rightUseViewmodelPose,
            twoHandedViewmodelPose,
            leftHanded,
            magazineGripPhysicalMask,
            gloveFingerMaxCurl,
            debugLog);

    const VrHandMatrix4 sceneProjection = BuildVrHandsProjection(view, false);
    const bool magazineUsesViewmodelLayer =
        magazineGripPhysicalMask && standaloneMagazineBoxUseViewmodelLayer;
    const VrHandMatrix4 viewmodelProjection =
        (rightUseViewmodelPose || twoHandedViewmodelPose || magazineUsesViewmodelLayer)
        ? BuildVrHandsProjection(view, true)
        : sceneProjection;
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const float sourceUnitsPerMeter = vrScale;
    const float clampedModelScale = std::clamp(modelScale, 0.25f, 4.0f);

    bool drewAny = false;
    Vector positions[2] = { leftControllerPosition, rightControllerPosition };
    if (magazineUsesViewmodelLayer)
    {
        for (int gameplayHand = 0; gameplayHand < 2; ++gameplayHand)
        {
            const int physicalHand = physicalHandIndexForGameplay(gameplayHand);
            if (physicalHand < 0 || physicalHand > 1 ||
                (magazineGripPhysicalMask & (1u << physicalHand)) == 0u) continue;
            Vector reprojected{};
            if (ReprojectScenePointToViewmodelLayer(view, positions[gameplayHand], reprojected))
                positions[gameplayHand] = reprojected;
        }
    }
    const QAngle angles[2] = { leftControllerAngles, rightControllerAngles };
    const Vector positionOffsets[2] = { leftHandPoseOffsetMeters, rightHandPoseOffsetMeters };
    const Vector rotationOffsets[2] = { leftHandPoseRotationOffsetDeg, rightHandPoseRotationOffsetDeg };
    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const bool useRightViewmodelPose = handIndex == 1 && rightUseViewmodelPose;
        const bool useLeftViewmodelPose = handIndex == 0 && twoHandedViewmodelPose;
        const bool useViewmodelPose = useLeftViewmodelPose || useRightViewmodelPose;
        const int physicalHandIndex = physicalHandIndexForGameplay(handIndex);
        if (physicalHandIndex < 0 || physicalHandIndex >= static_cast<int>(m_Hands.size()))
            continue;

        const HandState& hand = m_Hands[static_cast<size_t>(physicalHandIndex)];
        const ViewmodelPoseState* viewmodelState =
            useViewmodelPose
            ? (useLeftViewmodelPose ? &leftViewmodelState : &rightViewmodelState)
            : nullptr;
        if (viewmodelState && !viewmodelState->paletteValid)
            viewmodelState = nullptr;
        const std::vector<VrHandMatrixRows3x4>& palette =
            viewmodelState ? viewmodelState->palette : hand.palette;
        const bool paletteValid =
            viewmodelState ? viewmodelState->paletteValid : hand.paletteValid;
        if (!paletteValid)
            continue;
        if (!viewmodelState && !l4d2vr_hand_pose::Available(actionFrame.Read(), hand.paletteFrameToken,
                m_LastPoseUpdateTick, GetTickCount()))
            continue;

        VrHandMatrix4 world{};
        if (viewmodelState)
        {
            if (!BuildViewmodelWorld(
                    *viewmodelState,
                    sourceUnitsPerMeter,
                    clampedModelScale,
                    positionOffsets[handIndex],
                    rotationOffsets[handIndex],
                    !allowControllerlessTestPose && !viewmodelState->autoGripAligned,
                    currentViewmodelPosition,
                    currentViewmodelAngles,
                    world))
                continue;
        }
        else
        {
            world = VrHandMath::BuildControllerWorld(
                positions[handIndex],
                angles[handIndex],
                sourceUnitsPerMeter,
                clampedModelScale,
                positionOffsets[handIndex],
                rotationOffsets[handIndex]);
        }

        const bool magazineHandUsesViewmodelLayer = magazineUsesViewmodelLayer &&
            (magazineGripPhysicalMask & (1u << physicalHandIndex)) != 0u;
        const bool useViewmodelLayer = viewmodelState != nullptr || magazineHandUsesViewmodelLayer;
        if (magazineHandUsesViewmodelLayer &&
            drawPass == VrHandDrawPass::WorldVisibilityMask)
        {
            continue;
        }

        const VrHandMatrix4& projection = useViewmodelLayer ? viewmodelProjection : sceneProjection;
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        const VrHandDrawPass handDrawPass =
            (magazineHandUsesViewmodelLayer)
            ? VrHandDrawPass::ViewmodelStandalone
            : drawPass;
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, palette, world, wvp, handDrawPass, sceneLightScale, error,
                viewmodelState ? nullptr : &actionFrame, hand.paletteFrameToken, m_LastPoseUpdateTick))
            ReportErrorOnce(error);
        else
            drewAny = true;
    }

    if (!drewAny && allowControllerlessTestPose)
    {
        drewAny = DrawControllerlessTestPose(
            device,
            view,
            vrScale,
            modelScale,
            rightUseViewmodelPose,
            twoHandedViewmodelPose,
            leftHanded,
            sceneLightScale,
            debugLog,
            leftHandPoseOffsetMeters,
            leftHandPoseRotationOffsetDeg,
            rightHandPoseOffsetMeters,
            rightHandPoseRotationOffsetDeg,
            drawPass);
    }

    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            standaloneMagazineBoxWorld,
            standaloneMagazineBoxMins,
            standaloneMagazineBoxMaxs,
            kMagazineDebugFreshBoxColorArgb,
            "fresh_magazine",
            standaloneMagazineBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            magazineSocketCaptureBoxWorld,
            magazineSocketCaptureBoxMins,
            magazineSocketCaptureBoxMaxs,
            kMagazineDebugSocketCaptureBoxColorArgb,
            "socket_capture",
            magazineSocketCaptureBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            currentMagazineBoxWorld,
            currentMagazineBoxMins,
            currentMagazineBoxMaxs,
            kMagazineDebugCurrentMagazineBoxColorArgb,
            "current_magazine",
            currentMagazineBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            currentBoltBoxWorld,
            currentBoltBoxMins,
            currentBoltBoxMaxs,
            kMagazineDebugBoltBoxColorArgb,
            "current_bolt",
            currentBoltBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    if (DrawStandaloneMagazineBox(
            device,
            view,
            sceneLightScale,
            leftHandTargetBoxWorld,
            leftHandTargetBoxMins,
            leftHandTargetBoxMaxs,
            kLeftHandTargetBoxColorArgb,
            "left_hand_target",
            leftHandTargetBoxUseViewmodelLayer,
            drawPass))
    {
        drewAny = true;
    }
    return drewAny;
}

bool VrHandSystem::DrawControllerlessTestPose(
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
    VrHandDrawPass drawPass)
{
    if (!EnsureAssetsLoaded(debugLog))
        return false;

    const VrHandMatrix4 sceneProjection = BuildVrHandsProjection(view, false);
    const VrHandMatrix4 viewmodelProjection =
        (rightUseViewmodelPose || twoHandedViewmodelPose) ? BuildVrHandsProjection(view, true) : sceneProjection;
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const float sourceUnitsPerMeter = vrScale;
    const float clampedModelScale = std::clamp(modelScale, 0.25f, 4.0f);

    Vector forward{};
    Vector right{};
    Vector up{};
    QAngle viewAngles(view.angles.x, view.angles.y, view.angles.z);
    QAngle::AngleVectors(viewAngles, &forward, &right, &up);

    const Vector base = view.origin + forward * 42.0f - up * 16.0f;
    const Vector positions[2] =
    {
        base - right * 12.0f,
        base + right * 12.0f
    };

    bool drewAny = false;
    const Vector positionOffsets[2] = { leftHandPoseOffsetMeters, rightHandPoseOffsetMeters };
    const Vector rotationOffsets[2] = { leftHandPoseRotationOffsetDeg, rightHandPoseRotationOffsetDeg };
    const std::array<float, 5> defaultGloveFingerMaxCurl = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    auto physicalHandIndexForGameplay = [&](int gameplayHandIndex) -> int
        {
            return leftHanded ? (1 - gameplayHandIndex) : gameplayHandIndex;
        };

    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const bool useRightViewmodelPose = handIndex == 1 && rightUseViewmodelPose;
        const bool useLeftViewmodelPose = handIndex == 0 && twoHandedViewmodelPose;
        const bool useViewmodelPose = useLeftViewmodelPose || useRightViewmodelPose;
        const int physicalHandIndex = physicalHandIndexForGameplay(handIndex);
        if (physicalHandIndex < 0 || physicalHandIndex >= static_cast<int>(m_Hands.size()))
            continue;

        const HandState& hand = m_Hands[static_cast<size_t>(physicalHandIndex)];
        if (!hand.asset.IsValid() || hand.asset.jointNames.empty())
            continue;

        std::vector<VrHandMatrixRows3x4> bindPalette(hand.asset.jointNames.size(), VrHandMath::ToRows3x4(VrHandMath::Identity()));
        const int sourceSide = useLeftViewmodelPose ? -1 : 1;
        ViewmodelPoseState& viewmodelState =
            m_ViewmodelPoseStates[static_cast<size_t>(ViewmodelPoseStateIndex(sourceSide))];
        if (useViewmodelPose)
        {
            VrHandVmPose::Snapshot snapshot{};
            if (!VrHandVmPose::GetLatest(snapshot, kVrHandsVmPoseMaxAgeMs) ||
                !BuildViewmodelPalette(snapshot, sourceSide, physicalHandIndex, defaultGloveFingerMaxCurl, viewmodelState))
            {
                continue;
            }
            bindPalette = viewmodelState.palette;
        }

        VrHandMatrix4 world{};
        if (useViewmodelPose)
        {
            if (!BuildViewmodelWorld(
                    viewmodelState,
                    sourceUnitsPerMeter,
                    clampedModelScale,
                    positionOffsets[handIndex],
                    rotationOffsets[handIndex],
                    false,
                    Vector{},
                    QAngle{},
                    world))
                continue;
        }
        else
        {
            world = VrHandMath::BuildControllerWorld(
                positions[handIndex],
                viewAngles,
                sourceUnitsPerMeter,
                clampedModelScale,
                positionOffsets[handIndex],
                rotationOffsets[handIndex]);
        }
        const bool useViewmodelLayer = useViewmodelPose;
        const VrHandMatrix4& projection = useViewmodelLayer ? viewmodelProjection : sceneProjection;
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, bindPalette, world, wvp, drawPass, sceneLightScale, error))
            ReportErrorOnce(error);
        else
            drewAny = true;
    }

    if (debugLog && !m_DebugTestPoseLogged)
    {
        m_DebugTestPoseLogged = true;
        Game::logMsg("[VR][Hands] drawing mouse-mode controllerless test pose");
    }
    return drewAny;
}

bool VrHandSystem::ClearViewmodelOcclusionStencil(IDirect3DDevice9* device)
{
    if (m_DependencyUnavailable)
        return false;

    std::string error;
    if (m_Renderer.ClearViewmodelOcclusionStencil(device, error))
        return true;
    if (IsNonFatalViewmodelOcclusionError(error))
    {
        ReportWarningOnce(error + "; disabling VR hand viewmodel occlusion mask for this frame");
        return false;
    }
    ReportErrorOnce(error);
    return false;
}

bool VrHandSystem::EnsureAssetsAvailable(bool debugLog)
{
    return EnsureAssetsLoaded(debugLog);
}

bool VrHandSystem::IsDependencyUnavailable() const
{
    return m_DependencyUnavailable;
}

const std::string& VrHandSystem::DependencyFailureReason() const
{
    return m_DependencyFailureReason;
}

void VrHandSystem::OnDeviceLost()
{
    m_Renderer.OnDeviceLost();
}
