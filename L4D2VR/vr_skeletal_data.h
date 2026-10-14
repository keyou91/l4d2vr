#pragma once

#include "openvr.h"
#include "vr_hands/vr_hand_types.h"

#include <cmath>
#include <limits>

namespace l4d2vr_skeletal
{
    inline bool ValidCurl(float value)
    { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; }

    inline bool ValidSummaryCurls(const vr::VRSkeletalSummaryData_t& summary)
    {
        for (float curl : summary.flFingerCurl)
            if (!ValidCurl(curl)) return false;
        return true;
    }

    inline bool ValidBone(const vr::VRBoneTransform_t& bone)
    {
        for (float component : bone.position.v)
            if (!std::isfinite(component)) return false;
        const float components[] = {bone.orientation.x, bone.orientation.y,
            bone.orientation.z, bone.orientation.w};
        double norm = 0.0;
        for (float component : components)
        {
            if (!std::isfinite(component)) return false;
            norm += static_cast<double>(component) * component;
        }
        return norm > 0.000001 && norm <= (std::numeric_limits<float>::max)();
    }

    inline bool ValidMatrix(const VrHandMatrix4& matrix)
    {
        for (float component : matrix.m)
            if (!std::isfinite(component)) return false;
        return true;
    }

    inline bool ValidPalette(const std::vector<VrHandMatrixRows3x4>& palette)
    {
        if (palette.empty()) return false;
        for (const auto& matrix : palette)
            for (float component : matrix.v)
                if (!std::isfinite(component)) return false;
        return true;
    }
}
