#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <string_view>
#include <initializer_list>

namespace l4d2vr_dual
{
    enum class Hand : unsigned { None, Right, Left };
    struct Matrix3x4 { float m[3][4]{}; };
    inline Hand PrimaryHand(bool independent, bool dual, unsigned ownership)
    { return independent && !dual && ownership == 2u ? Hand::Left : Hand::Right; }
    inline bool ContactHandMatches(Hand published, Hand current)
    { return published != Hand::None && published == current; }
    inline bool ControllerFrameAvailable(bool hmd, bool hand, std::uint64_t captured, std::uint64_t now)
    { return hmd && hand && captured != 0u && now >= captured && now - captured <= 350u; }
    inline bool ReloadSupportIsLeft(Hand primary) { return primary != Hand::Left; }
    inline unsigned ReloadGripPhysicalMask(bool active, Hand primary, bool leftHanded)
    {
        const bool physicalLeft = ReloadSupportIsLeft(primary) != leftHanded;
        return active ? (physicalLeft ? 1u : 2u) : 0u;
    }
    inline bool RetainedWeaponPart(std::string_view name)
    {
        // Some models keep mechanical parts outside the weapon-root subtree.
        for (const auto prefix : {"weapon_", "valvebiped.weapon_", "clip", "valvebiped.clip",
            "slide", "valvebiped.slide", "bolt", "valvebiped.bolt", "bullet", "valvebiped.bullet",
            "shell", "valvebiped.shell"})
            if (name.substr(0, std::string_view(prefix).size()) == prefix) return true;
        return false;
    }
    constexpr unsigned kRightShotMarker = 62u;
    constexpr unsigned kLeftShotMarker = 63u;
    inline Hand DecodeShotHand(unsigned word, bool attack)
    {
        if (!attack) return Hand::None;
        return word == kLeftShotMarker ? Hand::Left : word == kRightShotMarker ? Hand::Right : Hand::None;
    }
    inline bool IsShotMarker(unsigned word, bool attack, bool pistol)
    {
        return attack && pistol && (word == kRightShotMarker || word == kLeftShotMarker);
    }

    class ReloadPulse
    {
    public:
        bool Update(bool eligible, bool pressed, std::uint32_t now)
        {
            if (!eligible) { m_Active = false; return false; }
            if (pressed) { m_Started = now; m_Active = true; }
            if (m_Active && now - m_Started >= 350u) m_Active = false;
            return m_Active;
        }
    private:
        bool m_Active = false;
        std::uint32_t m_Started = 0u;
    };

    class TriggerRouter
    {
    public:
        Hand Update(bool eligible, bool right, bool left, int clip)
        {
            if (!eligible || clip < 0) { Reset(); return Hand::None; }
            const bool spent = m_Clip >= 0 && clip < m_Clip;
            Hand selected = Hand::None;
            if (right && !left) selected = Hand::Right;
            else if (left && !right) selected = Hand::Left;
            else if (right && left)
            {
                selected = m_Hand == Hand::None ? Hand::Right : m_Hand;
                if (spent)
                    selected = selected == Hand::Right ? Hand::Left : Hand::Right;
                else if (left && !m_Left && !(right && !m_Right))
                    selected = Hand::Left;
                else if (right && !m_Right && !(left && !m_Left))
                    selected = Hand::Right;
            }
            const bool separator = selected != Hand::None && m_Hand != Hand::None && selected != m_Hand;
            m_Clip = clip;
            m_Right = right;
            m_Left = left;
            m_Hand = selected;
            // Native pistols share one attack command. A release tick between
            // hands keeps a new trigger press from being swallowed as a hold.
            return separator ? Hand::None : selected;
        }
        void Reset() { m_Clip = -1; m_Right = m_Left = false; m_Hand = Hand::None; }
    private:
        int m_Clip = -1;
        bool m_Right = false, m_Left = false;
        Hand m_Hand = Hand::None;
    };

    struct Shot
    {
        int command = 0;
        Hand hand = Hand::None;
        std::array<float, 3> position{};
        std::array<float, 3> angles{};
        std::uintptr_t owner = 0u, weapon = 0u;
        std::uint64_t capturedAtMs = 0u;
        std::uint32_t weaponHandle = 0u;
    };

    inline bool LiveShotMatches(const Shot& shot, std::uintptr_t owner, std::uintptr_t weapon, std::uint64_t now)
    {
        if (!owner || !weapon || shot.owner != owner || shot.weapon != weapon || shot.command <= 0 ||
            shot.hand == Hand::None || now < shot.capturedAtMs || now - shot.capturedAtMs > 350u) return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!std::isfinite(shot.position[axis]) || !std::isfinite(shot.angles[axis])) return false;
        return true;
    }

    class CommandShots
    {
    public:
        void Store(const Shot& shot)
        {
            if (shot.command <= 0) return;
            m_Shots[static_cast<unsigned>(shot.command) % m_Shots.size()] = shot;
        }
        bool Get(int command, Shot& result) const
        {
            if (command <= 0) return false;
            const auto& shot = m_Shots[static_cast<unsigned>(command) % m_Shots.size()];
            if (shot.command != command || shot.hand == Hand::None) return false;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(shot.position[axis]) || !std::isfinite(shot.angles[axis])) return false;
            result = shot;
            return true;
        }
        bool GetForExecution(int command, std::uintptr_t owner, std::uintptr_t weapon,
            std::uint32_t handle, std::uint64_t now, Shot& result) const
        {
            Shot shot{};
            if (!handle || !Get(command, shot) || shot.weaponHandle != handle ||
                !LiveShotMatches(shot, owner, weapon, now)) return false;
            result = shot;
            return true;
        }
    private:
        std::array<Shot, 150> m_Shots{};
    };

    template<class Matrix> Matrix ControllerFrame(const std::array<float,3>& origin,
        const std::array<float,3>& forward, const std::array<float,3>& right, const std::array<float,3>& up)
    {
        // Same basis used by the existing OpenVR hand anchors: right, up, back.
        Matrix out{};
        for (unsigned row=0; row<3; ++row)
        {
            out.m[row][0]=right[row]; out.m[row][1]=up[row];
            out.m[row][2]=-forward[row]; out.m[row][3]=origin[row];
        }
        return out;
    }

    template<class Matrix> Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix out{};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                for (int k = 0; k < 3; ++k) out.m[row][col] += a.m[row][k] * b.m[k][col];
            out.m[row][3] = a.m[row][3];
            for (int k = 0; k < 3; ++k) out.m[row][3] += a.m[row][k] * b.m[k][3];
        }
        return out;
    }

    template<class Matrix> bool Finite(const Matrix& matrix)
    {
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                if (!std::isfinite(matrix.m[row][col])) return false;
        return true;
    }

    // Build the second gun's target from the first gun's calibrated relation
    // to its controller. Each gun then follows its own controller transform.
    template<class Matrix> bool Retarget(const Matrix& sourceController, const Matrix& targetController,
        const Matrix& sourceGun, const Matrix& destinationGun, Matrix& delta)
    {
        if (!Finite(sourceController) || !Finite(targetController) || !Finite(sourceGun) || !Finite(destinationGun))
            return false;
        auto inverse = [](const Matrix& a, Matrix& out)
        {
            const float det = a.m[0][0] * (a.m[1][1]*a.m[2][2]-a.m[1][2]*a.m[2][1]) -
                a.m[0][1] * (a.m[1][0]*a.m[2][2]-a.m[1][2]*a.m[2][0]) +
                a.m[0][2] * (a.m[1][0]*a.m[2][1]-a.m[1][1]*a.m[2][0]);
            if (!std::isfinite(det) || std::fabs(det) < 0.00001f) return false;
            const float d = 1.0f / det;
            out.m[0][0]=(a.m[1][1]*a.m[2][2]-a.m[1][2]*a.m[2][1])*d;
            out.m[0][1]=(a.m[0][2]*a.m[2][1]-a.m[0][1]*a.m[2][2])*d;
            out.m[0][2]=(a.m[0][1]*a.m[1][2]-a.m[0][2]*a.m[1][1])*d;
            out.m[1][0]=(a.m[1][2]*a.m[2][0]-a.m[1][0]*a.m[2][2])*d;
            out.m[1][1]=(a.m[0][0]*a.m[2][2]-a.m[0][2]*a.m[2][0])*d;
            out.m[1][2]=(a.m[0][2]*a.m[1][0]-a.m[0][0]*a.m[1][2])*d;
            out.m[2][0]=(a.m[1][0]*a.m[2][1]-a.m[1][1]*a.m[2][0])*d;
            out.m[2][1]=(a.m[0][1]*a.m[2][0]-a.m[0][0]*a.m[2][1])*d;
            out.m[2][2]=(a.m[0][0]*a.m[1][1]-a.m[0][1]*a.m[1][0])*d;
            for (int r=0;r<3;++r)
            {
                out.m[r][3]=0.0f;
                for (int k=0;k<3;++k) out.m[r][3]-=out.m[r][k]*a.m[k][3];
            }
            return Finite(out);
        };
        Matrix controllerInverse{}, gunInverse{};
        if (!inverse(sourceController, controllerInverse) || !inverse(destinationGun, gunInverse)) return false;
        const Matrix targetGun = Multiply(targetController, Multiply(controllerInverse, sourceGun));
        delta = Multiply(targetGun, gunInverse);
        return Finite(delta);
    }
}
