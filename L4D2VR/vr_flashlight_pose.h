#pragma once
#include "vr_interaction_geometry.h"
#include <cstdint>

namespace l4d2vr_flashlight
{
    inline float OffsetMeters(float value)
    { return std::isfinite(value) ? std::clamp(value, 0.0f, 0.30f) : 0.0f; }

    inline float ClearTravel(float requested, float fraction, bool startSolid, bool allSolid, float margin)
    {
        if (!std::isfinite(requested) || requested < 0.0f || !std::isfinite(fraction) ||
            fraction < 0.0f || fraction > 1.0f || !std::isfinite(margin) || margin < 0.0f ||
            startSolid || allSolid) return 0.0f;
        return fraction == 1.0f ? requested : std::max(0.0f, requested * fraction - margin);
    }

    struct Pose
    {
        std::uintptr_t owner = 0u;
        std::uint64_t tick = 0u;
        Vector origin{0.0f,0.0f,0.0f}, forward{0.0f,0.0f,0.0f};
        Vector right{0.0f,0.0f,0.0f}, up{0.0f,0.0f,0.0f};
        float scale = 0.0f, offset = 0.0f;
        bool valid = false;
    };

    inline bool Capture(std::uintptr_t owner, std::uint64_t tick, const Vector& head,
        const Vector& forward, const Vector& right, const Vector& up, float scale, float offsetMeters,
        float fraction, bool startSolid, bool allSolid, Pose& result)
    {
        result = {};
        if (!owner || !tick || !l4d2vr_interaction::Finite(head) || !l4d2vr_interaction::Finite(forward) ||
            !l4d2vr_interaction::Finite(right) || !l4d2vr_interaction::Finite(up) ||
            !std::isfinite(scale) || scale <= 0.001f || !std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
            return false;
        // AngleVectors supplies an orthonormal basis; reject broken tracking.
        for (const auto* axis : {&forward,&right,&up})
            if (std::fabs(axis->LengthSqr()-1.0f) > 0.01f) return false;
        if (std::fabs(DotProduct(forward,right)) > 0.01f || std::fabs(DotProduct(forward,up)) > 0.01f ||
            std::fabs(DotProduct(right,up)) > 0.01f) return false;
        const float offset = OffsetMeters(offsetMeters);
        const float travel = ClearTravel(offset * scale, fraction, startSolid, allSolid, 0.01f * scale);
        result = {owner,tick,head + forward * travel,forward,right,up,scale,offset,true};
        if (!l4d2vr_interaction::Finite(result.origin)) { result = {}; return false; }
        return true;
    }

    inline bool Resolve(const Pose& sample, std::uintptr_t owner, std::uint64_t now,
        float scale, float offsetMeters, Pose& result)
    {
        if (!sample.valid || !owner || sample.owner != owner || now < sample.tick || now - sample.tick > 150u ||
            sample.scale != scale || sample.offset != OffsetMeters(offsetMeters)) return false;
        result = sample;
        return true;
    }
}
