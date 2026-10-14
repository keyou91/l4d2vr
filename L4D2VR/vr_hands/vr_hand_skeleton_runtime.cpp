#include "vr_hand_skeleton_runtime.h"

#include "vr_hand_math.h"
#include "../vr_skeletal_data.h"
#include "../vr_hand_input_validation.h"

#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/base/span.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <excpt.h>
#include <functional>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    ozz::math::Transform ToOzzTransform(const vr::VRBoneTransform_t& transform, bool forceIdentity)
    {
        if (forceIdentity)
            return ozz::math::Transform::identity();

        ozz::math::Transform out = ozz::math::Transform::identity();
        out.translation = { transform.position.v[0], transform.position.v[1], transform.position.v[2] };
        const auto& q = transform.orientation;
        const double norm = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
            static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w;
        const float inverseNorm = static_cast<float>(1.0 / std::sqrt(norm));
        out.rotation = { q.x * inverseNorm, q.y * inverseNorm, q.z * inverseNorm, q.w * inverseNorm };
        return out;
    }

    std::string ToLowerAscii(std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    bool TryReadOpenVRSkeletalSummary(
        vr::IVRInput* input,
        vr::VRActionHandle_t action,
        vr::EVRSummaryType summaryType,
        vr::VRSkeletalSummaryData_t& outSummary)
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

        return !faulted && result == vr::VRInputError_None &&
            l4d2vr_skeletal::ValidSummaryCurls(outSummary);
    }

    VrHandMatrix4 ToVrHandMatrix(const ozz::math::Float4x4& matrix)
    {
        VrHandMatrix4 out{};
        for (int column = 0; column < 4; ++column)
            ozz::math::StorePtrU(matrix.cols[column], &out.m[static_cast<size_t>(column) * 4u]);
        return out;
    }

    VrHandMatrix4 ToVrHandMatrix(const vr::VRBoneTransform_t& transform)
    {
        float x = transform.orientation.x;
        float y = transform.orientation.y;
        float z = transform.orientation.z;
        float w = transform.orientation.w;
        const double lengthSq = static_cast<double>(x) * x + static_cast<double>(y) * y +
            static_cast<double>(z) * z + static_cast<double>(w) * w;
        if (lengthSq > 0.000001f)
        {
            const float invLength = static_cast<float>(1.0 / std::sqrt(lengthSq));
            x *= invLength;
            y *= invLength;
            z *= invLength;
            w *= invLength;
        }
        else
        {
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
            w = 1.0f;
        }

        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, 1.0f - 2.0f * (y * y + z * z));
        VrHandMath::Set(out, 0, 1, 2.0f * (x * y - z * w));
        VrHandMath::Set(out, 0, 2, 2.0f * (x * z + y * w));
        VrHandMath::Set(out, 1, 0, 2.0f * (x * y + z * w));
        VrHandMath::Set(out, 1, 1, 1.0f - 2.0f * (x * x + z * z));
        VrHandMath::Set(out, 1, 2, 2.0f * (y * z - x * w));
        VrHandMath::Set(out, 2, 0, 2.0f * (x * z - y * w));
        VrHandMath::Set(out, 2, 1, 2.0f * (y * z + x * w));
        VrHandMath::Set(out, 2, 2, 1.0f - 2.0f * (x * x + y * y));
        VrHandMath::Set(out, 0, 3, transform.position.v[0]);
        VrHandMath::Set(out, 1, 3, transform.position.v[1]);
        VrHandMath::Set(out, 2, 3, transform.position.v[2]);
        return out;
    }

    int FindNameIndex(const std::vector<std::string>& names, const std::string& name)
    {
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    VrHandMatrix4 BuildBindLocalMatrix(const VrHandMeshAsset& asset, int joint)
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

    VrHandMatrix4 MakeLocalZRotation(float radians)
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

    float ResolveFingerMaxCurl(const std::array<float, 5>* fingerMaxCurl, int finger)
    {
        if (!fingerMaxCurl ||
            finger < 0 ||
            finger >= static_cast<int>(fingerMaxCurl->size()))
        {
            return 1.0f;
        }
        return std::clamp((*fingerMaxCurl)[static_cast<size_t>(finger)], 0.0f, 1.0f);
    }

    bool BuildSummaryCurlPalette(
        const VrHandMeshAsset& asset,
        const vr::VRSkeletalSummaryData_t& summary,
        std::vector<VrHandMatrixRows3x4>& outPalette,
        const VrHandFingerCurlOverride* fingerCurlOverride,
        const std::array<float, 5>* fingerMaxCurl)
    {
        const bool rightHand = FindNameIndex(asset.jointNames, "wrist_r") >= 0;
        const bool leftHand = FindNameIndex(asset.jointNames, "wrist_l") >= 0;
        if (!rightHand && !leftHand)
            return false;

        const char suffix = rightHand ? 'r' : 'l';
        static const char* kFingerNames[vr::VRFinger_Count] =
        {
            "thumb",
            "index",
            "middle",
            "ring",
            "pinky"
        };
        static const float kMaxCurlRadians[vr::VRFinger_Count][3] =
        {
            { 0.75f, 0.90f, 0.65f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
        };

        std::vector<VrHandMatrix4> localMatrices(asset.jointNames.size(), VrHandMath::Identity());
        for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
            localMatrices[joint] = BuildBindLocalMatrix(asset, static_cast<int>(joint));

        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            float curl = std::clamp(summary.flFingerCurl[finger], 0.0f, 1.0f);
            const float globalMaxCurl = ResolveFingerMaxCurl(fingerMaxCurl, finger);
            if (fingerCurlOverride && fingerCurlOverride->enabled &&
                finger < static_cast<int>(fingerCurlOverride->minCurl.size()) &&
                finger < static_cast<int>(fingerCurlOverride->maxCurl.size()))
            {
                const float maxCurl = std::min(
                    std::clamp(fingerCurlOverride->maxCurl[static_cast<size_t>(finger)], 0.0f, 1.0f),
                    globalMaxCurl);
                const float minCurl = std::min(
                    std::clamp(fingerCurlOverride->minCurl[static_cast<size_t>(finger)], 0.0f, 1.0f),
                    maxCurl);
                curl = std::clamp(curl, minCurl, maxCurl);
            }
            else
            {
                curl = std::min(curl, globalMaxCurl);
            }
            for (int segment = 0; segment < 3; ++segment)
            {
                const std::string jointName = std::string("finger_") +
                    kFingerNames[finger] + "_" + static_cast<char>('0' + segment) + "_" + suffix;
                const int joint = FindNameIndex(asset.jointNames, jointName);
                if (joint < 0)
                    return false;

                localMatrices[static_cast<size_t>(joint)] = VrHandMath::Multiply(
                    localMatrices[static_cast<size_t>(joint)],
                    MakeLocalZRotation(curl * kMaxCurlRadians[finger][segment]));
            }
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
                return false;
        }

        outPalette.resize(asset.jointNames.size());
        for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
        {
            outPalette[joint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(
                modelMatrices[joint],
                asset.inverseBindMatrices[joint]));
        }
        return true;
    }
}

struct VrHandSkeletonRuntime::Impl
{
    vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
    std::vector<vr::BoneIndex_t> parents;
    std::vector<std::string> openVrJointNames;
    std::vector<vr::VRBoneTransform_t> transforms;
    vr::VRSkeletalSummaryData_t summary{};
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    ozz::vector<ozz::math::SoaTransform> locals;
    ozz::vector<ozz::math::Float4x4> models;
    std::unordered_map<std::string, int> jointIndexByName;
    std::vector<int> ozzJointToOpenVrBone;
    bool hasPose = false;
    bool hasSummary = false;
    bool hasBoneData = false;
    const l4d2vr_input::ActionFrame* actionFrame = nullptr;
    std::uint64_t poseFrameToken = 0u;
};

VrHandSkeletonRuntime::VrHandSkeletonRuntime()
    : m_Impl(std::make_unique<Impl>())
{
}

VrHandSkeletonRuntime::~VrHandSkeletonRuntime() = default;

bool VrHandSkeletonRuntime::Initialize(vr::IVRInput* input, vr::VRActionHandle_t action, std::string& outError)
{
    outError.clear();
    m_Impl->hasPose = false;
    m_Impl->hasSummary = false;
    m_Impl->hasBoneData = false;
    m_Impl->poseFrameToken = 0u;
    if (!input || action == vr::k_ulInvalidActionHandle)
    {
        outError = "invalid OpenVR skeletal action handle";
        return false;
    }

    uint32_t boneCount = 0;
    if (input->GetBoneCount(action, &boneCount) != vr::VRInputError_None || boneCount == 0 || boneCount > 64)
    {
        outError = "OpenVR returned an invalid hand bone count";
        return false;
    }

    m_Impl->action = action;
    m_Impl->parents.assign(boneCount, vr::k_unInvalidBoneIndex);
    m_Impl->openVrJointNames.assign(boneCount, std::string{});
    m_Impl->transforms.assign(boneCount, vr::VRBoneTransform_t{});

    if (input->GetBoneHierarchy(action, m_Impl->parents.data(), boneCount) != vr::VRInputError_None)
    {
        outError = "OpenVR GetBoneHierarchy failed";
        return false;
    }

    for (uint32_t i = 0; i < boneCount; ++i)
    {
        char name[vr::k_unMaxBoneNameLength]{};
        if (input->GetBoneName(action, static_cast<vr::BoneIndex_t>(i), name, static_cast<uint32_t>(sizeof(name))) != vr::VRInputError_None)
        {
            outError = "OpenVR GetBoneName failed";
            return false;
        }
        m_Impl->openVrJointNames[i] = name;
    }

    if (input->GetSkeletalReferenceTransforms(
        action,
        vr::VRSkeletalTransformSpace_Parent,
        vr::VRSkeletalReferencePose_BindPose,
        m_Impl->transforms.data(),
        boneCount) != vr::VRInputError_None)
    {
        outError = "OpenVR GetSkeletalReferenceTransforms failed";
        return false;
    }
    if (!std::all_of(m_Impl->transforms.begin(), m_Impl->transforms.end(), l4d2vr_skeletal::ValidBone))
    {
        outError = "OpenVR hand reference transforms are invalid";
        return false;
    }

    std::vector<std::vector<uint32_t>> children(boneCount);
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        const vr::BoneIndex_t parent = m_Impl->parents[i];
        if (parent == vr::k_unInvalidBoneIndex)
        {
            roots.push_back(i);
            continue;
        }
        if (parent < 0 || static_cast<uint32_t>(parent) >= boneCount)
        {
            outError = "OpenVR returned an invalid parent bone index";
            return false;
        }
        children[static_cast<size_t>(parent)].push_back(i);
    }
    if (roots.empty())
    {
        outError = "OpenVR hand skeleton has no root bone";
        return false;
    }

    ozz::animation::offline::RawSkeleton rawSkeleton;
    std::function<void(uint32_t, ozz::animation::offline::RawSkeleton::Joint::Children&)> appendJoint;
    appendJoint = [&](uint32_t index, ozz::animation::offline::RawSkeleton::Joint::Children& destination)
        {
            destination.resize(destination.size() + 1u);
            auto& joint = destination.back();
            joint.name = m_Impl->openVrJointNames[index].c_str();
            joint.transform = ToOzzTransform(m_Impl->transforms[index], m_Impl->parents[index] == vr::k_unInvalidBoneIndex);
            for (uint32_t child : children[index])
                appendJoint(child, joint.children);
        };
    for (uint32_t root : roots)
        appendJoint(root, rawSkeleton.roots);

    ozz::animation::offline::SkeletonBuilder builder;
    m_Impl->skeleton = builder(rawSkeleton);
    if (!m_Impl->skeleton || m_Impl->skeleton->num_joints() != static_cast<int>(boneCount))
    {
        outError = "ozz SkeletonBuilder failed for the OpenVR hand hierarchy";
        return false;
    }

    m_Impl->locals.resize(static_cast<size_t>(m_Impl->skeleton->num_soa_joints()));
    m_Impl->models.resize(static_cast<size_t>(m_Impl->skeleton->num_joints()));
    std::unordered_map<std::string, int> openVrBoneIndexByName;
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        const std::string& name = m_Impl->openVrJointNames[i];
        openVrBoneIndexByName.emplace(name, static_cast<int>(i));
        openVrBoneIndexByName.emplace(ToLowerAscii(name), static_cast<int>(i));
    }

    m_Impl->jointIndexByName.clear();
    m_Impl->ozzJointToOpenVrBone.assign(static_cast<size_t>(m_Impl->skeleton->num_joints()), -1);
    const auto jointNames = m_Impl->skeleton->joint_names();
    for (int i = 0; i < m_Impl->skeleton->num_joints(); ++i)
    {
        const std::string name = jointNames[static_cast<size_t>(i)] ? jointNames[static_cast<size_t>(i)] : "";
        m_Impl->jointIndexByName.emplace(name, i);
        m_Impl->jointIndexByName.emplace(ToLowerAscii(name), i);

        auto openVrBone = openVrBoneIndexByName.find(name);
        if (openVrBone == openVrBoneIndexByName.end())
            openVrBone = openVrBoneIndexByName.find(ToLowerAscii(name));
        if (openVrBone == openVrBoneIndexByName.end())
        {
            outError = "ozz skeleton joint does not map back to an OpenVR bone: " + name;
            return false;
        }
        m_Impl->ozzJointToOpenVrBone[static_cast<size_t>(i)] = openVrBone->second;
    }

    m_Impl->hasPose = false;
    return true;
}

bool VrHandSkeletonRuntime::Update(vr::IVRInput* input, const l4d2vr_input::ActionFrame& actionFrame,
    vr::IVRSystem* system, vr::VRInputValueHandle_t expectedSource,
    vr::TrackedDeviceIndex_t expectedDevice,
    vr::EVRSkeletalMotionRange motionRange, std::string& outError)
{
    outError.clear();
    m_Impl->hasPose = false;
    m_Impl->hasSummary = false;
    m_Impl->hasBoneData = false;
    m_Impl->poseFrameToken = 0u;
    m_Impl->actionFrame = &actionFrame;
    const auto frameToken = actionFrame.Read();
    if (!input || !m_Impl->skeleton || m_Impl->action == vr::k_ulInvalidActionHandle ||
        !l4d2vr_input::ActionFrame::Ready(frameToken))
    {
        outError = "hand skeleton runtime is not initialized";
        return false;
    }

    vr::InputSkeletalActionData_t actionData{};
    if (input->GetSkeletalActionData(m_Impl->action, &actionData, sizeof(actionData)) != vr::VRInputError_None ||
        !actionData.bActive || !l4d2vr_hand_input::ValidateOrigin(input, system, actionData.activeOrigin,
            expectedSource, expectedDevice))
    {
        m_Impl->hasPose = false;
        m_Impl->hasSummary = false;
        return false;
    }

    vr::VRSkeletalSummaryData_t summary{};
    const bool hasSummary = TryReadOpenVRSkeletalSummary(
        input,
        m_Impl->action,
        vr::VRSummaryType_FromAnimation,
        summary);

    std::vector<vr::VRBoneTransform_t> transforms(m_Impl->transforms.size());
    const bool hasBoneData = input->GetSkeletalBoneData(
        m_Impl->action,
        vr::VRSkeletalTransformSpace_Model,
        motionRange,
        transforms.data(),
        static_cast<uint32_t>(transforms.size())) == vr::VRInputError_None &&
        std::all_of(transforms.begin(), transforms.end(), l4d2vr_skeletal::ValidBone);
    if (!actionFrame.Matches(frameToken))
    {
        outError = "SteamVR action frame changed during hand skeleton read";
        return false;
    }
    if (!hasSummary && !hasBoneData)
    {
        outError = "OpenVR hand summary and bone data are unavailable or invalid";
        m_Impl->hasPose = false;
        return false;
    }

    // OpenVR can return each bone already concatenated in model space.
    // Use that representation directly instead of rebuilding the hierarchy through
    // a second animation runtime; the SteamVR glove GLB inverse-bind matrices are
    // authored for this model-space skeleton.

    m_Impl->summary = hasSummary ? summary : vr::VRSkeletalSummaryData_t{};
    if (hasBoneData)
        m_Impl->transforms = std::move(transforms);
    m_Impl->hasSummary = hasSummary;
    m_Impl->hasBoneData = hasBoneData;
    m_Impl->poseFrameToken = frameToken;
    m_Impl->hasPose = true;
    return true;
}

bool VrHandSkeletonRuntime::BuildSkinningPalette(
    const VrHandMeshAsset& asset,
    std::vector<VrHandMatrixRows3x4>& outPalette,
    std::string& outError,
    const VrHandFingerCurlOverride* fingerCurlOverride,
    const std::array<float, 5>* fingerMaxCurl) const
{
    outError.clear();
    outPalette.clear();
    if (!m_Impl->skeleton || !HasPose())
        return false;
    if (!asset.IsValid())
    {
        outError = "hand mesh asset is invalid";
        return false;
    }
    if (asset.jointNames.size() > 64)
    {
        outError = "hand mesh uses more than 64 joints";
        return false;
    }
    for (size_t joint = 0u; joint < asset.jointNames.size(); ++joint)
    {
        if (!l4d2vr_skeletal::ValidMatrix(asset.bindMatrices[joint]) ||
            !l4d2vr_skeletal::ValidMatrix(asset.inverseBindMatrices[joint]))
        {
            outError = "hand mesh bind matrices are invalid";
            return false;
        }
    }
    auto finishPalette = [&]() -> bool
        {
            if (!l4d2vr_skeletal::ValidPalette(outPalette) || !HasPose())
            {
                outError = "hand skinning palette or action frame is invalid";
                outPalette.clear();
                return false;
            }
            return true;
        };

    if (m_Impl->hasSummary && BuildSummaryCurlPalette(asset, m_Impl->summary, outPalette, fingerCurlOverride, fingerMaxCurl))
        return finishPalette();
    if (!m_Impl->hasBoneData)
    {
        outError = "hand skinning palette requires current valid bone data";
        outPalette.clear();
        return false;
    }

    outPalette.resize(asset.jointNames.size());
    for (size_t meshJoint = 0; meshJoint < asset.jointNames.size(); ++meshJoint)
    {
        const std::string& name = asset.jointNames[meshJoint];
        auto found = m_Impl->jointIndexByName.find(name);
        if (found == m_Impl->jointIndexByName.end())
            found = m_Impl->jointIndexByName.find(ToLowerAscii(name));
        if (found == m_Impl->jointIndexByName.end() && name.empty() && meshJoint < m_Impl->models.size())
        {
            found = m_Impl->jointIndexByName.end();
            const VrHandMatrix4 model = ToVrHandMatrix(m_Impl->transforms[meshJoint]);
            outPalette[meshJoint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(model, asset.inverseBindMatrices[meshJoint]));
            continue;
        }
        if (found == m_Impl->jointIndexByName.end())
        {
            outError = "hand GLB joint name does not exist in the OpenVR skeleton: " + name;
            outPalette.clear();
            return false;
        }

        int openVrBone = found->second;
        if (openVrBone >= 0 && openVrBone < static_cast<int>(m_Impl->ozzJointToOpenVrBone.size()))
            openVrBone = m_Impl->ozzJointToOpenVrBone[static_cast<size_t>(openVrBone)];
        if (openVrBone < 0 || openVrBone >= static_cast<int>(m_Impl->transforms.size()))
        {
            outError = "hand skeleton joint does not map to a valid OpenVR bone: " + name;
            outPalette.clear();
            return false;
        }

        const VrHandMatrix4 model = ToVrHandMatrix(m_Impl->transforms[static_cast<size_t>(openVrBone)]);
        outPalette[meshJoint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(model, asset.inverseBindMatrices[meshJoint]));
    }
    return finishPalette();
}

bool VrHandSkeletonRuntime::IsInitialized() const
{
    return m_Impl && m_Impl->skeleton != nullptr;
}

bool VrHandSkeletonRuntime::HasPose() const
{
    return m_Impl && m_Impl->hasPose && m_Impl->actionFrame &&
        m_Impl->actionFrame->Matches(m_Impl->poseFrameToken);
}

std::uint64_t VrHandSkeletonRuntime::PoseFrameToken() const
{
    return m_Impl ? m_Impl->poseFrameToken : 0u;
}

int VrHandSkeletonRuntime::JointCount() const
{
    return (m_Impl && m_Impl->skeleton) ? m_Impl->skeleton->num_joints() : 0;
}
