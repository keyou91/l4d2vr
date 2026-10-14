#pragma once

#include <array>
#include <cstdint>
#include <cmath>

namespace l4d2vr_physical
{
    struct MeleeCommand
    {
        int command = 0;
        bool valid = false;
        bool swinging = false;
        std::array<float, 3> position{};
        std::array<float, 3> angles{};

        bool Finite() const
        {
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(position[axis]) || !std::isfinite(angles[axis])) return false;
            return true;
        }
    };

    inline void SelectNewestMeleeCommand(MeleeCommand& latest, const MeleeCommand& sample)
    {
        if (sample.command > latest.command) latest = sample;
    }

    inline bool MeleeTrackingFresh(std::uint32_t now, std::uint32_t sampled)
    {
        return now - sampled <= 150u;
    }

    class MeleeCommands
    {
    public:
        void Store(const MeleeCommand& sample)
        {
            if (sample.command > 0)
                m_Samples[static_cast<unsigned>(sample.command) % m_Samples.size()] = sample;
        }
        bool Get(int command, MeleeCommand& result) const
        {
            if (command <= 0) return false;
            const auto& sample = m_Samples[static_cast<unsigned>(command) % m_Samples.size()];
            if (sample.command != command || !sample.valid || !sample.Finite()) return false;
            result = sample;
            return true;
        }
        void Reset() { m_Samples = {}; }
    private:
        std::array<MeleeCommand, 150> m_Samples{};
    };

    // Wrist rotation moves the weapon tip even when the controller translates
    // very little. Walking velocity is removed before it reaches this gate.
    class MeleeMotion
    {
    public:
        bool Update(bool eligible, std::uintptr_t owner, std::uintptr_t weapon,
            float relativeSpeed, float angularSpeedDegrees)
        {
            if (!eligible || !owner || !weapon || !std::isfinite(relativeSpeed) ||
                !std::isfinite(angularSpeedDegrees) || relativeSpeed < 0.0f || angularSpeedDegrees < 0.0f)
            { Reset(); return false; }
            if (owner != m_Owner || weapon != m_Weapon)
            { Reset(); m_Owner = owner; m_Weapon = weapon; }
            constexpr float kTipRadiusMeters = 0.35f;
            const float speed = relativeSpeed + angularSpeedDegrees * (3.14159265f / 180.0f) * kTipRadiusMeters;
            if (!std::isfinite(speed)) { Reset(); return false; }
            if (speed <= 0.45f) { m_Armed = true; m_Swinging = false; }
            else if (m_Armed && speed >= 1.1f) { m_Armed = false; m_Swinging = true; }
            return m_Swinging;
        }
        void Reset() { m_Owner = m_Weapon = 0u; m_Armed = m_Swinging = false; }
    private:
        std::uintptr_t m_Owner = 0u, m_Weapon = 0u;
        bool m_Armed = false, m_Swinging = false;
    };

    // The packet reader may see backup commands in either order. Damage uses
    // only a fresh command and never connects poses across a weapon/session gap.
    class MeleeSweepHistory
    {
    public:
        enum Result { Ignore, Rebase, Sweep };
        Result Accept(std::uintptr_t owner, std::uintptr_t weapon, int command, bool eligible)
        {
            if (!owner || command <= 0) { BreakContinuity(); return Ignore; }
            if (owner != m_Owner) { m_Owner = owner; m_LastCommand = 0; BreakContinuity(); }
            if (command <= m_LastCommand) return Ignore;
            const auto gap = static_cast<std::int64_t>(command) - m_LastCommand;
            m_LastCommand = command;
            if (!eligible || !weapon) { BreakContinuity(); return Rebase; }
            const bool continuous = m_Weapon == weapon && gap <= 8;
            m_Weapon = weapon;
            return continuous ? Sweep : Rebase;
        }
        void BreakContinuity() { m_Weapon = 0u; }
    private:
        std::uintptr_t m_Owner = 0u, m_Weapon = 0u;
        int m_LastCommand = 0;
    };

    inline int MeleeSweepSamples(float translation, float angleDegrees, float unitsPerMeter)
    {
        if (!std::isfinite(translation) || !std::isfinite(angleDegrees) ||
            !std::isfinite(unitsPerMeter) || unitsPerMeter <= 0.001f ||
            translation < 0.0f || translation > 0.5f * unitsPerMeter)
            return 0;
        if (translation <= 0.001f && std::fabs(angleDegrees) <= 0.01f)
            return 0;
        const float byDistance = translation / (0.02f * unitsPerMeter);
        const float byAngle = std::fabs(angleDegrees) / 10.0f;
        const float wanted = std::fmax(byDistance, byAngle);
        // Avoid an extra sample from roundoff at an exact 2 cm boundary.
        return wanted >= 12.0f ? 12 : static_cast<int>(std::fmax(1.0f, std::ceil(wanted - 0.00001f)));
    }

    // A contact can consume a held grip once. Leaving and entering a different
    // body slot while still holding the same item must not switch it repeatedly.
    class ContactLatch
    {
    public:
        bool Update(bool active, bool down, bool contact)
        {
            if (!active)
            {
                m_RequireRelease = true;
                return false;
            }
            if (m_RequireRelease)
            {
                if (!down) { m_RequireRelease = false; m_Consumed = false; }
                return false;
            }
            if (!down)
            {
                m_Consumed = false;
                return false;
            }
            if (!contact || m_Consumed)
                return false;
            m_Consumed = true;
            return true;
        }
    private:
        bool m_Consumed = false;
        bool m_RequireRelease = false;
    };

    class ReservedGrip
    {
    public:
        bool AllowPickup(bool eligible, bool active, bool down, bool reserved, std::uint32_t discontinuity = 0u)
        {
            if (discontinuity != m_Discontinuity)
            {
                m_Discontinuity = discontinuity;
                m_RequireRelease = true;
                return false;
            }
            if (!eligible || !active || reserved)
            { m_RequireRelease = true; return false; }
            if (!down) { m_RequireRelease = false; return false; }
            return !m_RequireRelease;
        }
        void Reset() { m_RequireRelease = true; }
    private:
        bool m_RequireRelease = false;
        std::uint32_t m_Discontinuity = 0u;
    };

    class PickupIntent
    {
    public:
        enum : unsigned { RequestUse = 1u, AdoptWeapon = 2u };

        void Begin(std::uint32_t now, std::uintptr_t weapon)
        {
            m_Pending = true;
            m_Started = now;
            m_PreviousWeapon = weapon;
        }

        unsigned Update(bool eligible, bool active, bool down, bool contact,
            std::uint32_t now, std::uintptr_t weapon)
        {
            if (!eligible || !active || !down)
            {
                Reset();
                return 0u;
            }
            if (m_Pending)
            {
                if (now - m_Started <= 650u && weapon && weapon != m_PreviousWeapon)
                {
                    m_Pending = false;
                    m_Consumed = true;
                    return AdoptWeapon;
                }
                if (now - m_Started <= 650u)
                    return 0u;
                m_Pending = false;
                m_Consumed = true;
            }
            if (!contact || m_Consumed)
                return 0u;
            Begin(now, weapon);
            return RequestUse;
        }

        void Reset()
        {
            m_Pending = false;
            m_Consumed = false;
            m_Started = 0u;
            m_PreviousWeapon = 0u;
        }
    private:
        bool m_Pending = false;
        bool m_Consumed = false;
        std::uint32_t m_Started = 0u;
        std::uintptr_t m_PreviousWeapon = 0u;
    };

    // Keep cycling state per weapon, including while the player draws another
    // item. Clip increases are loading, not shots. No ammo is created here.
    class PumpCycles
    {
    public:
        void ObserveOwner(bool active, std::uintptr_t owner)
        {
            if (!active || !owner) { Reset(); return; }
            if (owner != m_Owner) { Reset(); m_Owner = owner; }
        }
        bool Observe(std::uintptr_t weapon, int weaponId, int clip, std::uint32_t handle = 0u)
        {
            if (!weapon || clip < 0)
                return false;
            Entry* entry = nullptr;
            for (auto& candidate : m_Entries)
            {
                // A recycled entity address must not inherit a spent gun's cycle.
                if (candidate.weapon == weapon &&
                    (candidate.weaponId != weaponId || candidate.handle != handle)) candidate = {};
                if (candidate.weapon == weapon && candidate.weaponId == weaponId)
                {
                    entry = &candidate;
                    break;
                }
            }
            if (!entry)
            {
                entry = &m_Entries[m_Next++ % m_Entries.size()];
                *entry = { weapon, weaponId, clip, false, handle };
            }
            entry->clip = clip;
            return entry->needsCycle;
        }

        void NotifyShot(std::uintptr_t weapon, int weaponId, int clip, std::uint32_t handle = 0u)
        {
            if (!weapon || (weaponId != 3 && weaponId != 8) || clip < 0)
                return;
            Observe(weapon, weaponId, clip, handle);
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.weaponId == weaponId && entry.handle == handle)
                    entry.needsCycle = true;
        }

        bool Complete(std::uintptr_t weapon, std::uint32_t handle = 0u)
        {
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle)
                {
                    // Closing an empty action does not put a shell in the chamber.
                    if (entry.clip <= 0) return false;
                    entry.needsCycle = false;
                    return true;
                }
            return false;
        }

        void Reset() { m_Entries = {}; m_Next = 0u; m_Owner = 0u; }

        static bool StrokeComplete(bool reachedRear, float distance, float returnDistance)
        {
            return reachedRear && std::isfinite(distance) &&
                std::isfinite(returnDistance) && distance >= 0.0f &&
                distance <= returnDistance;
        }
    private:
        struct Entry
        {
            std::uintptr_t weapon = 0u;
            int weaponId = 0;
            int clip = -1;
            bool needsCycle = false;
            std::uint32_t handle = 0u;
        };
        std::array<Entry, 16> m_Entries{};
        std::uintptr_t m_Owner = 0u;
        std::size_t m_Next = 0u;
    };
}
