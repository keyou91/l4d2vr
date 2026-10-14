#pragma once
#include "vr_dual_pistols.h"

namespace l4d2vr_server_pistol
{
    enum class ReloadPolicy : unsigned { Native, Block, Preserve };
    inline bool ValidReloadPolicy(ReloadPolicy policy)
    {
        return policy == ReloadPolicy::Native || policy == ReloadPolicy::Block || policy == ReloadPolicy::Preserve;
    }
    struct Input
    {
        l4d2vr_dual::Shot shot{};
        ReloadPolicy reload = ReloadPolicy::Native;
        l4d2vr_dual::Hand pickup = l4d2vr_dual::Hand::None;
    };
    struct Weapon
    {
        std::uintptr_t pointer = 0u;
        unsigned serial = 0u;
        bool operator==(const Weapon& other) const { return pointer == other.pointer && serial == other.serial; }
        bool operator!=(const Weapon& other) const { return !(*this == other); }
    };
    class Commands
    {
    public:
        void Reset() { *this = {}; }
        bool Store(std::uintptr_t owner, unsigned serial, const l4d2vr_dual::Shot& shot,
            ReloadPolicy reload = ReloadPolicy::Native, l4d2vr_dual::Hand pickup = l4d2vr_dual::Hand::None)
        {
            if (!owner || shot.command <= 0 || !ValidReloadPolicy(reload) ||
                (pickup != l4d2vr_dual::Hand::None && pickup != l4d2vr_dual::Hand::Right &&
                    pickup != l4d2vr_dual::Hand::Left)) return false;
            if (owner != m_Owner || serial != m_Serial)
            { Reset(); m_Owner = owner; m_Serial = serial; }
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(shot.position[axis]) || !std::isfinite(shot.angles[axis])) return false;
            if (static_cast<std::int64_t>(m_Latest) - shot.command >= static_cast<std::int64_t>(m_Shots.size()))
                return false;
            auto& entry = m_Shots[static_cast<unsigned>(shot.command) % m_Shots.size()];
            if (entry.shot.command == shot.command) return false; // first decoded backup owns its exact input
            entry = {shot, reload, pickup};
            if (shot.command > m_Latest) m_Latest = shot.command;
            return true;
        }
        bool Get(std::uintptr_t owner, unsigned serial, int command, std::uint64_t now,
            l4d2vr_dual::Shot& result) const
        {
            if (!owner || owner != m_Owner || serial != m_Serial || command <= 0) return false;
            Input input{};
            if (!GetInput(owner, serial, command, now, input) || input.shot.hand == l4d2vr_dual::Hand::None)
                return false;
            result = input.shot; return true;
        }
        bool GetInput(std::uintptr_t owner, unsigned serial, int command, std::uint64_t now, Input& result) const
        {
            if (!owner || owner != m_Owner || serial != m_Serial || command <= 0) return false;
            const auto& input = m_Shots[static_cast<unsigned>(command) % m_Shots.size()];
            if (input.shot.command != command || now < input.shot.capturedAtMs ||
                now - input.shot.capturedAtMs > 2500u) return false;
            result = input; return true;
        }
    private:
        std::uintptr_t m_Owner = 0;
        unsigned m_Serial = 0;
        int m_Latest = 0;
        std::array<Input, 150> m_Shots{};
    };

    // Resolve transient grip/pull input from the previously simulated command,
    // never from a newer decoded packet. Replayed commands keep their decision.
    class Reloads
    {
    public:
        void Reset() { *this = {}; }
        bool Execute(std::uintptr_t owner, unsigned serial, int command, Weapon weapon,
            ReloadPolicy policy, bool& blocked)
        {
            blocked = false;
            if (!owner || command <= 0 || !ValidReloadPolicy(policy)) return false;
            if (owner != m_Owner || serial != m_Serial)
            { Reset(); m_Owner = owner; m_Serial = serial; }
            auto& result = m_Results[static_cast<unsigned>(command) % m_Results.size()];
            if (result.command == command)
            {
                if (result.weapon != weapon || result.policy != policy) return false;
                blocked = result.blocked; return true;
            }
            if (command <= m_Latest) return false;
            m_Latest = command;
            if (policy == ReloadPolicy::Native || (policy == ReloadPolicy::Preserve && m_BlockedWeapon != weapon))
                m_BlockedWeapon = {};
            else if (policy == ReloadPolicy::Block)
                m_BlockedWeapon = weapon;
            blocked = weapon.pointer != 0u && m_BlockedWeapon == weapon;
            result = {command, weapon, policy, blocked};
            return true;
        }
    private:
        struct Result
        {
            int command = 0;
            Weapon weapon{};
            ReloadPolicy policy = ReloadPolicy::Native;
            bool blocked = false;
        };
        std::uintptr_t m_Owner = 0u;
        Weapon m_BlockedWeapon{};
        unsigned m_Serial = 0u;
        int m_Latest = 0;
        std::array<Result, 150> m_Results{};
    };
}
