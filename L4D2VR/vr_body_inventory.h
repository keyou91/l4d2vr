#pragma once
#include "vr_interaction_geometry.h"
#include <cstdint>

namespace l4d2vr_body_inventory
{
    struct ModelPose
    {
        std::uintptr_t owner = 0, model = 0;
        std::uint64_t generation = 0, sampledAtMs = 0;
        Vector pelvisFromHeadLocalMeters{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f, rotationOffset = 0.0f;
    };
    inline bool Valid(const ModelPose& pose)
    {
        const auto& delta = pose.pelvisFromHeadLocalMeters;
        return pose.owner && pose.model && pose.generation && l4d2vr_interaction::Finite(delta) &&
            std::isfinite(pose.yaw) && std::isfinite(pose.rotationOffset) &&
            delta.z >= -1.3f && delta.z <= -0.3f && delta.x * delta.x + delta.y * delta.y <= 0.49f;
    }
    inline bool Capture(std::uintptr_t owner, std::uintptr_t model, std::uint64_t generation,
        const Vector& head, const Vector& pelvis, float yaw, float rotationOffset, float scale,
        std::uint64_t now, ModelPose& result)
    {
        if (!l4d2vr_interaction::Finite(head) || !l4d2vr_interaction::Finite(pelvis) ||
            !std::isfinite(yaw) || !std::isfinite(rotationOffset) || !std::isfinite(scale) || scale <= 0.001f)
            return false;
        const float radians = yaw * 3.14159265358979323846f / 180.0f;
        const Vector forward(std::cos(radians), std::sin(radians), 0.0f);
        const Vector right(forward.y, -forward.x, 0.0f);
        const Vector delta = (pelvis - head) * (1.0f / scale);
        ModelPose pose{owner, model, generation, now,
            Vector(DotProduct(delta, forward), DotProduct(delta, right), delta.z), yaw, rotationOffset};
        if (!Valid(pose)) return false;
        result = pose; return true;
    }
    inline bool Resolve(const ModelPose& pose, std::uintptr_t owner, std::uint64_t now,
        const Vector& head, const Vector& configuredOriginOffset, float scale, float rotationOffset,
        Vector& origin, Vector& forward, Vector& right)
    {
        if (!Valid(pose) || owner != pose.owner || now < pose.sampledAtMs || now - pose.sampledAtMs > 300u ||
            !l4d2vr_interaction::Finite(head) || !l4d2vr_interaction::Finite(configuredOriginOffset) ||
            !std::isfinite(scale) || scale <= 0.001f || !std::isfinite(rotationOffset)) return false;
        const float yaw = l4d2vr_interaction::WrapYaw(pose.yaw +
            l4d2vr_interaction::WrapYaw(rotationOffset - pose.rotationOffset));
        const float radians = yaw * 3.14159265358979323846f / 180.0f;
        const Vector modelForward(std::cos(radians), std::sin(radians), 0.0f);
        const Vector modelRight(modelForward.y, -modelForward.x, 0.0f);
        // The existing default waist is 0.68 m below the head: origin -0.28
        // plus slot -0.40. Rebase that nominal waist onto the rendered pelvis,
        // preserving the user's configured offsets and slot spacing.
        const Vector local = pose.pelvisFromHeadLocalMeters + configuredOriginOffset + Vector(0.0f, 0.0f, 0.68f);
        const Vector fitted = head + modelForward * (local.x * scale) + modelRight * (local.y * scale) +
            Vector(0.0f, 0.0f, local.z * scale);
        if (!l4d2vr_interaction::Finite(fitted)) return false;
        origin = fitted; forward = modelForward; right = modelRight; return true;
    }
}
