#pragma once

#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>

namespace l4d2vr_magazine
{
    constexpr unsigned kAuthoritativeMagazineInputMode = 64u;
    constexpr unsigned kAuthoritativeShellInputMode = 128u;
    constexpr unsigned kAuthoritativePistolInputMode = 256u;
    constexpr float kMinimumBoltPullSourceUnits = 0.001f;
    inline float ResolveBoltPullDistanceMeters(float configured, bool pump, float sourceUnitsPerMeter)
    {
        const float fallback = pump ? 0.09f : 0.055f;
        if (!std::isfinite(configured) || configured <= 0.0f) return fallback;
        const float bounded = std::min(configured, 0.25f);
        const float stroke = bounded * sourceUnitsPerMeter;
        return std::isfinite(stroke) && stroke > kMinimumBoltPullSourceUnits ? bounded : fallback;
    }
    inline bool TrackingAllowsGesture(bool hmd, bool left, bool right,
        std::uint64_t captured, std::uint64_t now)
    {
        return hmd && left && right && captured != 0u && now >= captured && now - captured <= 150u;
    }
    inline bool UseNativeReloadFallback(bool enabled, int weaponId, bool physicalReload, bool negotiated)
    {
        // The launcher reloads one round through its stock native mechanism.
        return enabled && (physicalReload || weaponId == 21) && !negotiated;
    }
    inline bool UseAuthoritativeMagazine(bool supported, bool detachable, bool nativeDual, bool leftHandPistol)
    {
        return supported && detachable && !nativeDual && !leftHandPistol;
    }
    inline bool WantsNativeReload(bool buttonOrGesture, bool manualBackend, bool explicitBackendReload)
    {
        return explicitBackendReload || (buttonOrGesture && !manualBackend);
    }
    class NativeFallbackPulse
    {
    public:
        void Reset() { m_Active = false; m_HaveScope = false; }
        bool Update(bool eligible, bool pressed, std::uint32_t generation, std::uint32_t now)
        {
            if (!m_HaveScope || generation != m_Generation)
            { m_Active = false; m_Generation = generation; m_HaveScope = true; }
            if (!eligible) { m_Active = false; return false; }
            if (pressed) { m_Started = now; m_Active = true; }
            if (m_Active && now - m_Started >= 350u) m_Active = false;
            return m_Active;
        }
    private:
        bool m_Active = false, m_HaveScope = false;
        std::uint32_t m_Generation = 0u, m_Started = 0u;
    };
    // Prediction can refill a clip before a physical slide has been cycled.
    // Retain the empty chamber across inventory switches, using native handle
    // serials to distinguish a replacement entity at the same address.
    class ChamberHistory
    {
    public:
        void ObserveOwner(bool active, std::uintptr_t owner)
        {
            if (!active || !owner || owner != m_Owner)
            {
                m_Entries = {}; m_Next = 0u; m_ActiveWeapon = 0u; m_ActiveHandle = 0u;
                m_Owner = active ? owner : 0u;
            }
        }
        bool Observe(std::uintptr_t weapon, std::uint32_t handle, int weaponId, int clip)
        {
            if (!m_Owner) return false;
            if (weapon != m_ActiveWeapon || handle != m_ActiveHandle)
                for (auto& entry : m_Entries)
                    if (entry.handle == 0u) entry = {}; // no serial: retain only the current draw
            m_ActiveWeapon = weapon; m_ActiveHandle = handle;
            if (!weapon || weaponId <= 0 || clip < 0) return false;
            Entry* selected = nullptr;
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle && entry.weaponId == weaponId)
                { selected = &entry; break; }
            if (!selected)
            {
                selected = &m_Entries[m_Next++ % m_Entries.size()];
                *selected = {weapon, handle, weaponId, false};
            }
            if (clip == 0) selected->empty = true;
            return selected->empty;
        }
        bool HasEmptyChamber(std::uintptr_t owner, std::uintptr_t weapon, std::uint32_t handle, int weaponId) const
        {
            if (!owner || owner != m_Owner || !weapon || !handle || weaponId <= 0) return false;
            for (const auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle && entry.weaponId == weaponId)
                    return entry.empty;
            return false;
        }
        void Complete(std::uintptr_t weapon)
        {
            if (weapon != m_ActiveWeapon) return;
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == m_ActiveHandle) entry.empty = false;
        }
        void Forget(std::uintptr_t weapon)
        {
            for (auto& entry : m_Entries) if (entry.weapon == weapon) entry = {};
            if (m_ActiveWeapon == weapon) { m_ActiveWeapon = 0u; m_ActiveHandle = 0u; }
        }
    private:
        struct Entry
        {
            std::uintptr_t weapon = 0u;
            std::uint32_t handle = 0u;
            int weaponId = 0;
            bool empty = false;
        };
        std::array<Entry, 16> m_Entries{};
        unsigned m_Next = 0u;
        std::uintptr_t m_Owner = 0u, m_ActiveWeapon = 0u;
        std::uint32_t m_ActiveHandle = 0u;
    };

    inline bool ShellSettlementPending(std::uint64_t now, std::uint64_t expires)
    {
        return expires != 0u && now < expires;
    }

    inline bool ShotgunBlocksFire(bool chamberEmpty, bool shellHeld,
        bool cycling, bool backendReload, bool ammoSettlement)
    {
        return chamberEmpty || shellHeld || cycling || backendReload || ammoSettlement;
    }
    inline bool ShouldResumeMagazineCycle(bool supported, bool activeSession, bool retainedEmpty, int clip)
    { return supported && !activeSession && retainedEmpty && clip > 0; }
    inline bool MayFinishShotgunLoading(bool shotgun, bool chamberEmpty, int clip, int shellsLoaded, bool gripDown)
    { return shotgun && chamberEmpty && gripDown && (clip > 0 || shellsLoaded > 0); }
    inline bool PumpShouldStartAfterShot(bool needsCycle, int clip, int shellsLoaded)
    {
        // Empty tubes must accept shells first. A shell-loading session lets
        // the user choose when to finish loading and grab the pump.
        return needsCycle && clip > 0 && shellsLoaded == 0;
    }

    class NativeReloadLedger
    {
    public:
        void Observe(int command, std::uintptr_t weapon, bool block, bool preserve = false,
            std::uintptr_t owner = 0u, unsigned serial = 0u)
        {
            if (owner != m_Owner || serial != m_Serial)
            {
                m_Owner = owner; m_Serial = serial; m_Command = 0; m_Weapon = 0u;
            }
            if (command <= m_Command) return;
            m_Command = command;
            if (preserve)
            {
                if (m_Weapon != weapon) m_Weapon = 0u;
                return;
            }
            m_Weapon = block ? weapon : 0u;
        }
        bool Blocks(std::uintptr_t weapon) const { return weapon != 0u && m_Weapon == weapon; }
    private:
        int m_Command = 0;
        std::uintptr_t m_Weapon = 0u;
        std::uintptr_t m_Owner = 0u;
        unsigned m_Serial = 0u;
    };
    inline bool IsLocalPlayerCommand(int playerIndex, int localPlayerIndex)
    {
        return playerIndex > 0 && localPlayerIndex > 0 && playerIndex == localPlayerIndex;
    }

    inline bool MayCommitAmmo(int expectedWeaponId, int serverWeaponId, int clientWeaponId,
        std::uint32_t queuedGeneration, std::uint32_t currentGeneration)
    {
        return expectedWeaponId > 0 && expectedWeaponId == serverWeaponId &&
            expectedWeaponId == clientWeaponId && queuedGeneration == currentGeneration;
    }

    class SessionTracker
    {
    public:
        std::uintptr_t OwnerTag() const { return m_Ready ? m_Player : 0u; }
        // Compare opaque identities without dereferencing objects that may
        // already have been deleted. Completion of a reload is not a boundary.
        bool Observe(bool ready, std::uintptr_t player, std::uintptr_t weapon,
            int weaponId, unsigned int inputMode, std::uint32_t handle = 0u)
        {
            ready = ready && player != 0u && weapon != 0u && weaponId > 0;
            if (!ready)
            {
                const bool changed = m_Ready;
                m_Ready = false;
                m_Player = m_Weapon = 0u;
                m_WeaponId = 0;
                m_InputMode = 0u;
                m_Handle = 0u;
                return changed;
            }
            const bool changed = !m_Ready || player != m_Player || weapon != m_Weapon ||
                weaponId != m_WeaponId || inputMode != m_InputMode || handle != m_Handle;
            m_Ready = true;
            m_Player = player;
            m_Weapon = weapon;
            m_WeaponId = weaponId;
            m_InputMode = inputMode;
            m_Handle = handle;
            return changed;
        }

    private:
        bool m_Ready = false;
        std::uintptr_t m_Player = 0u;
        std::uintptr_t m_Weapon = 0u;
        int m_WeaponId = 0;
        unsigned int m_InputMode = 0u;
        std::uint32_t m_Handle = 0u;
    };

    inline bool ShouldEject(bool buttonRequired, bool buttonPressed,
        bool suppressEmptyAutoReload, int clip, bool detachable)
    {
        if (!detachable)
            return false;
        return buttonRequired ? buttonPressed : (suppressEmptyAutoReload && clip == 0);
    }

    inline int ChamberRoundsAfterEject(int clip)
    {
        // Reserve ammunition cannot add a round to an empty chamber or
        // remove the round already loaded in a nonempty weapon.
        return clip > 0 ? 1 : 0;
    }
    inline bool ChamberReadyAfterEject(bool observedEmpty, int nativeRetainedClip)
    {
        // Source can refill its clip before the physical slide is cycled.
        // A native count confirms ammunition, not completion of that cycle.
        return !observedEmpty && nativeRetainedClip > 0;
    }
    inline bool MayCatchEjectedMagazine(bool gripDown, bool tracked, float distance, float padding)
    {
        return gripDown && tracked && std::isfinite(distance) && std::isfinite(padding) &&
            distance >= 0.0f && padding >= 0.0f && distance <= padding;
    }
    class RetainedInsertGate
    {
    public:
        void Reset() { m_Withdrawn = false; }
        bool Update(bool fitsSocket, float travel, float required)
        {
            if (!std::isfinite(travel) || !std::isfinite(required) || travel < 0.0f || required <= 0.0f) return false;
            if (!fitsSocket && travel >= required) m_Withdrawn = true;
            return m_Withdrawn && fitsSocket;
        }
    private:
        bool m_Withdrawn = false;
    };
}
