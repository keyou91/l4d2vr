#pragma once

#include "vr_hand_types.h"
#include "vector.h"

#include <algorithm>
#include <cmath>

namespace VrHandMath
{
    inline VrHandMatrix4 Identity()
    {
        VrHandMatrix4 out{};
        out.m[0] = 1.0f;
        out.m[5] = 1.0f;
        out.m[10] = 1.0f;
        out.m[15] = 1.0f;
        return out;
    }

    inline float Get(const VrHandMatrix4& matrix, int row, int column)
    {
        return matrix.m[static_cast<size_t>(column) * 4u + static_cast<size_t>(row)];
    }

    inline void Set(VrHandMatrix4& matrix, int row, int column, float value)
    {
        matrix.m[static_cast<size_t>(column) * 4u + static_cast<size_t>(row)] = value;
    }

    inline VrHandMatrix4 Multiply(const VrHandMatrix4& left, const VrHandMatrix4& right)
    {
        VrHandMatrix4 out{};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                float value = 0.0f;
                for (int k = 0; k < 4; ++k)
                    value += Get(left, row, k) * Get(right, k, column);
                Set(out, row, column, value);
            }
        }
        return out;
    }

    inline bool Invert4x4(const VrHandMatrix4& input, VrHandMatrix4& output)
    {
        float a[4][8]{};
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
                a[row][column] = Get(input, row, column);
            a[row][row + 4] = 1.0f;
        }

        for (int column = 0; column < 4; ++column)
        {
            int pivotRow = column;
            float pivotAbs = std::fabs(a[column][column]);
            for (int row = column + 1; row < 4; ++row)
            {
                const float candidateAbs = std::fabs(a[row][column]);
                if (candidateAbs > pivotAbs)
                {
                    pivotAbs = candidateAbs;
                    pivotRow = row;
                }
            }

            if (!(pivotAbs > 0.0000001f))
                return false;

            if (pivotRow != column)
            {
                for (int i = 0; i < 8; ++i)
                    std::swap(a[column][i], a[pivotRow][i]);
            }

            const float invPivot = 1.0f / a[column][column];
            for (int i = 0; i < 8; ++i)
                a[column][i] *= invPivot;

            for (int row = 0; row < 4; ++row)
            {
                if (row == column)
                    continue;

                const float factor = a[row][column];
                if (factor == 0.0f)
                    continue;

                for (int i = 0; i < 8; ++i)
                    a[row][i] -= factor * a[column][i];
            }
        }

        output = Identity();
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
                Set(output, row, column, a[row][column + 4]);
        }
        return true;
    }

    inline float Dot(const Vector& left, const Vector& right)
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    inline Vector Normalize(const Vector& value)
    {
        const float lenSq = Dot(value, value);
        if (!(lenSq > 0.000001f))
            return Vector(0.0f, 0.0f, 0.0f);
        const float invLen = 1.0f / std::sqrt(lenSq);
        return value * invLen;
    }

    inline VrHandMatrix4 BuildControllerWorld(
        const Vector& origin,
        const QAngle& angles,
        float sourceUnitsPerMeter,
        float modelScale,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);

        forward = Normalize(forward);
        right = Normalize(right);
        up = Normalize(up);

        // This correction exists only inside the standalone GLB hand render matrix.
        // It must not be written back into any tracked-controller pose.
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        VrHandMatrix4 controllerBasis = Identity();
        Set(controllerBasis, 0, 0, right.x);
        Set(controllerBasis, 1, 0, right.y);
        Set(controllerBasis, 2, 0, right.z);
        Set(controllerBasis, 0, 1, up.x);
        Set(controllerBasis, 1, 1, up.y);
        Set(controllerBasis, 2, 1, up.z);
        Set(controllerBasis, 0, 2, -forward.x);
        Set(controllerBasis, 1, 2, -forward.y);
        Set(controllerBasis, 2, 2, -forward.z);
        Set(controllerBasis, 0, 3, origin.x);
        Set(controllerBasis, 1, 3, origin.y);
        Set(controllerBasis, 2, 3, origin.z);

        // Local correction R = Rz * Ry * Rx. Translation is converted from meters
        // before model scaling, so changing hand size never changes the calibration distance.
        VrHandMatrix4 localCorrection = Identity();
        Set(localCorrection, 0, 0, cz * cy);
        Set(localCorrection, 0, 1, cz * sy * sx - sz * cx);
        Set(localCorrection, 0, 2, cz * sy * cx + sz * sx);
        Set(localCorrection, 1, 0, sz * cy);
        Set(localCorrection, 1, 1, sz * sy * sx + cz * cx);
        Set(localCorrection, 1, 2, sz * sy * cx - cz * sx);
        Set(localCorrection, 2, 0, -sy);
        Set(localCorrection, 2, 1, cy * sx);
        Set(localCorrection, 2, 2, cy * cx);
        Set(localCorrection, 0, 3, localPositionOffsetMeters.x * sourceUnitsPerMeter);
        Set(localCorrection, 1, 3, localPositionOffsetMeters.y * sourceUnitsPerMeter);
        Set(localCorrection, 2, 3, localPositionOffsetMeters.z * sourceUnitsPerMeter);

        VrHandMatrix4 modelScaleMatrix = Identity();
        Set(modelScaleMatrix, 0, 0, modelScale * sourceUnitsPerMeter);
        Set(modelScaleMatrix, 1, 1, modelScale * sourceUnitsPerMeter);
        Set(modelScaleMatrix, 2, 2, modelScale * sourceUnitsPerMeter);
        return Multiply(Multiply(controllerBasis, localCorrection), modelScaleMatrix);
    }

    inline VrHandMatrix4 BuildControllerWorld(const Vector& origin, const QAngle& angles, float scale)
    {
        return BuildControllerWorld(origin, angles, scale, 1.0f,
            Vector(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 0.0f));
    }

    inline VrHandMatrix4 BuildSourceView(const Vector& origin, const Vector& angles)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle qangles(angles.x, angles.y, angles.z);
        QAngle::AngleVectors(qangles, &forward, &right, &up);

        forward = Normalize(forward);
        right = Normalize(right);
        up = Normalize(up);

        VrHandMatrix4 out = Identity();
        Set(out, 0, 0, right.x);
        Set(out, 0, 1, right.y);
        Set(out, 0, 2, right.z);
        Set(out, 0, 3, -Dot(right, origin));
        Set(out, 1, 0, up.x);
        Set(out, 1, 1, up.y);
        Set(out, 1, 2, up.z);
        Set(out, 1, 3, -Dot(up, origin));
        Set(out, 2, 0, forward.x);
        Set(out, 2, 1, forward.y);
        Set(out, 2, 2, forward.z);
        Set(out, 2, 3, -Dot(forward, origin));
        return out;
    }

    inline VrHandMatrix4 BuildPerspective(float horizontalFovDegrees, float aspectRatio, float zNear, float zFar)
    {
        const float pi = 3.14159265358979323846f;
        const float fovRadians = std::clamp(horizontalFovDegrees, 1.0f, 179.0f) * pi / 180.0f;
        const float safeAspect = (aspectRatio > 0.001f) ? aspectRatio : 1.0f;
        const float safeNear = std::max(0.01f, zNear);
        const float safeFar = std::max(safeNear + 1.0f, zFar);
        const float xScale = 1.0f / std::tan(fovRadians * 0.5f);
        const float yScale = xScale * safeAspect;

        VrHandMatrix4 out{};
        Set(out, 0, 0, xScale);
        Set(out, 1, 1, yScale);
        Set(out, 2, 2, safeFar / (safeFar - safeNear));
        Set(out, 2, 3, -(safeNear * safeFar) / (safeFar - safeNear));
        Set(out, 3, 2, 1.0f);
        return out;
    }

    inline std::array<float, 16> ToRows4x4(const VrHandMatrix4& matrix)
    {
        std::array<float, 16> rows{};
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
                rows[static_cast<size_t>(row) * 4u + static_cast<size_t>(column)] = Get(matrix, row, column);
        }
        return rows;
    }

    inline VrHandMatrixRows3x4 ToRows3x4(const VrHandMatrix4& matrix)
    {
        VrHandMatrixRows3x4 rows{};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
                rows.v[static_cast<size_t>(row) * 4u + static_cast<size_t>(column)] = Get(matrix, row, column);
        }
        return rows;
    }
}
