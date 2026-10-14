#pragma once

#include <cstdint>

namespace l4d2vr_grip
{
    constexpr unsigned char kPickupImpulse = 234u;
    constexpr unsigned char kReleaseImpulse = 235u;
    constexpr unsigned char kBlockNativeReloadImpulse = 236u;
    inline bool MayCommitDrop(bool liveOwner, bool sameOwner, bool sameEntity, bool activeWeapon, std::int64_t ageTicks)
    {
        return liveOwner && sameOwner && sameEntity && activeWeapon && ageTicks >= 0 && ageTicks <= 48;
    }
    inline bool CanReleaseInventoryItem(bool firearmOrMelee, bool primaryDown,
        bool primaryWasDown, bool useDown, bool manualThrowArmed)
    {
        // Guns can cancel their current interaction. A primed throwable must
        // finish its native attack path before an inventory drop is allowed.
        return firearmOrMelee ||
            (!primaryDown && !primaryWasDown && !useDown && !manualThrowArmed);
    }

    // Require a fresh press for each weapon/input session. A lost action,
    // menu transition, or weapon switch must not synthesize a release.
    class ReleaseLatch
    {
    public:
        // Source's extra mouse samples call CreateMove with command number
        // zero. Keep a live session's held grip across those camera-only calls.
        // Actual lifecycle loss or a different inventory item still cancels it.
        void ObserveSession(bool gameplayActive, std::uintptr_t weapon)
        {
            if (!gameplayActive || !weapon || (m_Weapon && m_Weapon != weapon))
                Reset();
        }
        bool IsArmed() const { return m_Armed; }
        bool WaitingForRelease() const { return m_WaitForRelease; }
        bool Update(bool eligible, bool actionActive, bool down, std::uintptr_t weapon,
            std::uint32_t discontinuity = 0u)
        {
            if (discontinuity != m_Discontinuity)
            {
                m_Discontinuity = discontinuity;
                m_Weapon = weapon;
                m_Armed = false;
                m_WaitForRelease = true;
                return false;
            }
            if (!eligible || !actionActive || weapon == 0u)
            {
                Reset();
                return false;
            }
            if (weapon != m_Weapon)
            {
                m_Weapon = weapon;
                m_Armed = false;
                m_WaitForRelease = down;
            }
            if (m_WaitForRelease)
            {
                m_WaitForRelease = down;
                return false;
            }
            if (down)
            {
                m_Armed = true;
                return false;
            }
            const bool released = m_Armed;
            m_Armed = false;
            return released;
        }

        // Only a confirmed, deliberate grip pickup may inherit a held grip.
        // Ordinary keyboard/joystick weapon switches still require a new press.
        void AdoptWeapon(std::uintptr_t weapon)
        {
            m_Weapon = weapon;
            m_Armed = weapon != 0u;
            m_WaitForRelease = false;
        }

        void Reset()
        {
            m_Weapon = 0u;
            m_Armed = false;
            m_WaitForRelease = false;
        }

    private:
        std::uintptr_t m_Weapon = 0u;
        bool m_Armed = false;
        bool m_WaitForRelease = false;
        std::uint32_t m_Discontinuity = 0u;
    };
}
