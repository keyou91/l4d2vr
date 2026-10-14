#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <limits>

namespace l4d2vr_optics
{
    inline bool Finite(const Vector& v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    inline bool Normalize(Vector& v)
    {
        if (!Finite(v)) return false;
        const float length = DotProduct(v, v);
        if (!std::isfinite(length) || !(length > 0.000001f)) return false;
        v *= 1.0f / std::sqrt(length); return true;
    }
    inline bool Fresh(std::uint64_t now, std::uint64_t at)
    { return at != 0u && now >= at && now - at <= 100u; }
    struct Pose { Vector origin{}, forward{}, right{}, up{}; };
    inline bool Valid(const Pose& p)
    { return Finite(p.origin) && Finite(p.forward) && Finite(p.right) && Finite(p.up); }
    inline bool RebaseVector(const Vector& v, const Pose& previous, const Pose& current, Vector& result)
    {
        if (!Finite(v) || !Valid(previous) || !Valid(current)) return false;
        const Vector next = current.forward * DotProduct(v,previous.forward) +
            current.right * DotProduct(v,previous.right) + current.up * DotProduct(v,previous.up);
        if (!Finite(next)) return false;
        result = next; return true;
    }
    inline bool RebasePoint(const Vector& p, const Pose& previous, const Pose& current, Vector& result)
    {
        if (!Finite(p) || !Valid(previous) || !Valid(current)) return false;
        Vector offset(0,0,0);
        if (!RebaseVector(p - previous.origin,previous,current,offset)) return false;
        const Vector next = current.origin + offset;
        if (!Finite(next)) return false;
        result = next; return true;
    }
    inline constexpr bool HasOptic(int id)
    { return id == 6 || id == 10 || id == 34 || id == 35 || id == 36; }
    struct WeaponIdentity
    {
        std::uintptr_t owner = 0u;
        std::uint32_t handle = 0u;
        int id = 0;
        std::uint64_t generation = 0u;
        bool Valid() const { return owner && handle && handle != 0xffffffffu && HasOptic(id) && generation; }
        bool operator==(const WeaponIdentity& other) const
        { return owner == other.owner && handle == other.handle && id == other.id && generation == other.generation; }
    };
    class WeaponTracker
    {
    public:
        const WeaponIdentity& Current() const { return m_Current; }
        bool Observe(std::uintptr_t owner, std::uint32_t handle, int id)
        {
            if (!owner || !handle || handle == 0xffffffffu || !HasOptic(id))
            {
                const bool changed = m_Current.Valid(); m_Current = {}; return changed;
            }
            if (m_Current.Valid() && m_Current.owner == owner && m_Current.handle == handle && m_Current.id == id)
                return false;
            if (m_Generation == (std::numeric_limits<std::uint64_t>::max)())
            { m_Current = {}; return true; }
            m_Current = {owner, handle, id, ++m_Generation}; return true;
        }
    private:
        WeaponIdentity m_Current{};
        std::uint64_t m_Generation = 0u;
    };
    struct ImageStamp
    {
        WeaponIdentity weapon{};
        float magnification = 0.0f;
        std::uint64_t completedAt = 0u;
        std::uint32_t renderEpoch = 0u;
    };
    inline bool ImageMatches(const ImageStamp& image, const WeaponIdentity& weapon,
        float magnification, std::uint32_t renderEpoch, std::uint64_t now)
    {
        return weapon.Valid() && image.weapon == weapon && image.renderEpoch == renderEpoch &&
            Fresh(now, image.completedAt) && std::isfinite(magnification) && magnification >= 1.0f &&
            std::isfinite(image.magnification) && std::fabs(image.magnification - magnification) <= 0.001f;
    }
    inline float Magnification(float controlFov, float maxControlFov, float base)
    {
        if (!std::isfinite(controlFov) || !std::isfinite(maxControlFov) || !std::isfinite(base) ||
            controlFov < 1.0f || maxControlFov < controlFov || maxControlFov >= 179.0f)
            return 1.0f;
        constexpr float radians = 3.14159265358979323846f / 360.0f;
        return std::clamp(base * std::tan(maxControlFov * radians) /
            std::tan(controlFov * radians), 1.0f, 16.0f);
    }
    inline bool ValidClipPlanes(float nearPlane, float farPlane)
    {
        return std::isfinite(nearPlane) && std::isfinite(farPlane) &&
            nearPlane > 0.0f && farPlane > nearPlane;
    }
    inline float ResolveNearPlane(float value, float previous)
    {
        const float fallback = std::isfinite(previous) && previous > 0.0f ? previous : 2.0f;
        return std::clamp(std::isfinite(value) && value > 0.0f ? value : fallback, 0.1f, 64.0f);
    }
    inline bool ValidRenderFov(float fov)
    { return std::isfinite(fov) && fov >= 0.1f && fov <= 120.0f; }
    inline float RenderFov(float radius, float eyeRelief, float magnification)
    {
        if (!std::isfinite(radius) || !std::isfinite(eyeRelief) || !std::isfinite(magnification) ||
            radius <= 0.0f || eyeRelief <= 0.0f || magnification < 1.0f) return 0.0f;
        const float fov = 2.0f * std::atan(radius / (eyeRelief * magnification)) *
            (180.0f / 3.14159265358979323846f);
        return ValidRenderFov(fov) ? fov : 0.0f;
    }
    struct EyeSample { float alpha = 0.0f, relief = 0.0f; };
    inline EyeSample EvaluateEye(const Vector& eye, const Vector& lens, Vector axis,
        float radius, float unitsPerMeter, float magnification)
    {
        if (!Finite(eye) || !Finite(lens) || !Normalize(axis) || !std::isfinite(radius) ||
            !std::isfinite(unitsPerMeter) || !std::isfinite(magnification) ||
            radius <= 0.0f || unitsPerMeter <= 0.0f || magnification < 1.0f)
            return {};
        const Vector delta = lens - eye;
        const float relief = DotProduct(delta, axis);
        if (relief < 0.035f * unitsPerMeter || relief > 0.25f * unitsPerMeter) return {};
        const Vector lateral = delta - axis * relief;
        const float pupilRadius = (std::max)(radius / magnification, 0.001f * unitsPerMeter);
        const float radial = std::sqrt((std::max)(0.0f, DotProduct(lateral, lateral))) / pupilRadius;
        return {std::clamp((1.0f - radial) / 0.25f, 0.0f, 1.0f), relief};
    }
    struct ModelLens
    {
        std::string_view path;
        std::uint32_t checksum;
        int weaponId;
        Vector bindCenter;
        float radius;
    };
    // Rear optic apertures measured from the installed stock LOD0 geometry.
    // Checksum matching excludes replacement models with different geometry.
    inline const ModelLens* FindModel(std::string_view path, std::uint32_t checksum)
    {
        static const ModelLens models[] = {
            {"models/v_models/v_huntingrifle.mdl", 4068759009u, 6, {14.039f, 0.504f, 48.94f}, 0.66f},
            {"models/v_models/v_sniper_military.mdl", 1046396963u, 10, {16.340f, 0.517f, 42.72f}, 0.37f},
            {"models/v_models/v_rif_sg552.mdl", 1582432951u, 34, {16.869f, 0.413f, 44.77f}, 0.46f},
            {"models/v_models/v_snip_awp.mdl", 1241414224u, 35, {15.222f, 0.583f, 43.78f}, 1.20f},
            {"models/v_models/v_snip_scout.mdl", 2116180069u, 36, {14.525f, 0.818f, 53.15f}, 0.80f},
        };
        for (const auto& model : models)
            if (path == model.path && checksum == model.checksum) return &model;
        return nullptr;
    }
}
