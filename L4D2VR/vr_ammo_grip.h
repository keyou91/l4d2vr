#pragma once
#include "vr_hands/vr_hand_math.h"

namespace l4d2vr_ammo_grip
{
    struct Pose
    {
        VrHandMatrix4 orientationLocal{};
        Vector centerOffsetLocal{0.0f, 0.0f, 0.0f};
    };
    inline Vector Origin(const VrHandMatrix4& matrix)
    {
        return Vector(VrHandMath::Get(matrix, 0, 3), VrHandMath::Get(matrix, 1, 3), VrHandMath::Get(matrix, 2, 3));
    }
    inline Vector Axis(const VrHandMatrix4& matrix, int column)
    {
        return Vector(VrHandMath::Get(matrix, 0, column), VrHandMath::Get(matrix, 1, column), VrHandMath::Get(matrix, 2, column));
    }
    inline bool Finite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    inline bool Rigid(const VrHandMatrix4& matrix)
    {
        for (float value : matrix.m) if (!std::isfinite(value)) return false;
        const Vector x = Axis(matrix, 0), y = Axis(matrix, 1), z = Axis(matrix, 2);
        Vector cross; CrossProduct(x, y, cross);
        return std::fabs(x.LengthSqr() - 1.0f) < 0.002f && std::fabs(y.LengthSqr() - 1.0f) < 0.002f &&
            std::fabs(z.LengthSqr() - 1.0f) < 0.002f && std::fabs(VrHandMath::Dot(x, y)) < 0.002f &&
            std::fabs(VrHandMath::Dot(x, z)) < 0.002f && std::fabs(VrHandMath::Dot(y, z)) < 0.002f &&
            VrHandMath::Dot(cross, z) > 0.998f &&
            std::fabs(VrHandMath::Get(matrix, 3, 0)) < 0.0001f &&
            std::fabs(VrHandMath::Get(matrix, 3, 1)) < 0.0001f &&
            std::fabs(VrHandMath::Get(matrix, 3, 2)) < 0.0001f &&
            std::fabs(VrHandMath::Get(matrix, 3, 3) - 1.0f) < 0.0001f;
    }
    inline Vector Rotate(const VrHandMatrix4& matrix, const Vector& value)
    {
        return Axis(matrix, 0) * value.x + Axis(matrix, 1) * value.y + Axis(matrix, 2) * value.z;
    }
    inline VrHandMatrix4 Calibration(const Vector& degrees)
    {
        constexpr float radians = 3.14159265358979323846f / 180.0f;
        const float sx = std::sin(degrees.x * radians), cx = std::cos(degrees.x * radians);
        const float sy = std::sin(degrees.y * radians), cy = std::cos(degrees.y * radians);
        const float sz = std::sin(degrees.z * radians), cz = std::cos(degrees.z * radians);
        VrHandMatrix4 matrix = VrHandMath::Identity();
        VrHandMath::Set(matrix, 0, 0, cz * cy);
        VrHandMath::Set(matrix, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(matrix, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(matrix, 1, 0, sz * cy);
        VrHandMath::Set(matrix, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(matrix, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(matrix, 2, 0, -sy);
        VrHandMath::Set(matrix, 2, 1, cy * sx);
        VrHandMath::Set(matrix, 2, 2, cy * cx);
        return matrix;
    }
    inline bool Capture(const VrHandMatrix4& controller, const Vector& rotationDegrees,
        const Vector& palmWorld, const Vector& boxCenterLocal, const Vector& gripPointLocal, Pose& result)
    {
        if (!Rigid(controller) || !Finite(rotationDegrees) || !Finite(palmWorld) ||
            !Finite(boxCenterLocal) || !Finite(gripPointLocal)) return false;
        Pose pose{};
        pose.orientationLocal = Calibration(rotationDegrees);
        const auto orientation = VrHandMath::Multiply(controller, pose.orientationLocal);
        const Vector center = palmWorld + Rotate(orientation, boxCenterLocal - gripPointLocal);
        const Vector delta = center - Origin(controller);
        pose.centerOffsetLocal = Vector(VrHandMath::Dot(delta, Axis(controller, 0)),
            VrHandMath::Dot(delta, Axis(controller, 1)), VrHandMath::Dot(delta, Axis(controller, 2)));
        if (!Rigid(pose.orientationLocal) || !Finite(pose.centerOffsetLocal)) return false;
        result = pose; return true;
    }
    inline bool Follow(const VrHandMatrix4& controller, const Pose& pose,
        const Vector& boxCenterLocal, VrHandMatrix4& result)
    {
        if (!Rigid(controller) || !Rigid(pose.orientationLocal) ||
            !Finite(pose.centerOffsetLocal) || !Finite(boxCenterLocal)) return false;
        auto world = VrHandMath::Multiply(controller, pose.orientationLocal);
        const Vector center = Origin(controller) + Rotate(controller, pose.centerOffsetLocal);
        const Vector origin = center - Rotate(world, boxCenterLocal);
        if (!Finite(origin)) return false;
        VrHandMath::Set(world, 0, 3, origin.x);
        VrHandMath::Set(world, 1, 3, origin.y);
        VrHandMath::Set(world, 2, 3, origin.z);
        result = world; return true;
    }
    inline bool BodyWorld(const Vector& forward, const Vector& right,
        const Vector& centerWorld, const Vector& boxCenterLocal, VrHandMatrix4& result)
    {
        if (!Finite(forward) || !Finite(right) || !Finite(centerWorld) || !Finite(boxCenterLocal)) return false;
        auto world = VrHandMath::Identity();
        // Source's forward/right/up frame has negative handedness. Model
        // vertices need forward/left/up so the mesh and its normals are intact.
        for (int row = 0; row < 3; ++row)
        {
            VrHandMath::Set(world, row, 0, forward[row]);
            VrHandMath::Set(world, row, 1, -right[row]);
        }
        if (!Rigid(world)) return false;
        const Vector origin = centerWorld - Rotate(world, boxCenterLocal);
        if (!Finite(origin)) return false;
        VrHandMath::Set(world, 0, 3, origin.x);
        VrHandMath::Set(world, 1, 3, origin.y);
        VrHandMath::Set(world, 2, 3, origin.z);
        result = world; return true;
    }
}
