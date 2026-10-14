#pragma once
#include "vr_pistol_ammo_sync.h"
#include "vr_dual_pistols.h"
#include <array>

namespace l4d2vr_pistol_prediction
{
    using Hand = l4d2vr_dual::Hand;
    inline std::uint64_t Pack(int right, int left, unsigned physical, unsigned chambered)
    {
        return right >= 0 && right <= 15 && left >= 0 && left <= 15 && physical <= 3u && chambered <= 3u
            ? static_cast<std::uint64_t>(right) | (static_cast<std::uint64_t>(left) << 16) |
                (static_cast<std::uint64_t>(physical) << 32) | (static_cast<std::uint64_t>(chambered) << 34)
            : 0xffffffffffffffffull;
    }
    inline unsigned FiringMask(std::uint64_t packed)
    {
        if (packed == 0xffffffffffffffffull) return 3u;
        unsigned mask = ((packed & 0xffffu) > 0u ? 1u : 0u) | (((packed >> 16) & 0xffffu) > 0u ? 2u : 0u);
        const auto physical = static_cast<unsigned>((packed >> 32) & 3u);
        const auto chambered = static_cast<unsigned>((packed >> 34) & 3u);
        return mask & (~physical | chambered);
    }
    inline bool BlocksEmptyHand(bool dual, bool consumesAmmo, Hand hand, int right, int left)
    {
        return dual && consumesAmmo && right >= 0 && left >= 0 &&
            ((hand == Hand::Right && right == 0) || (hand == Hand::Left && left == 0));
    }
    struct Fire
    {
        std::uintptr_t owner = 0u, weapon = 0u;
        std::uint32_t token = 0u, handle = 0u, command = 0u;
        unsigned ordinal = 0u;
        Hand hand = Hand::None;
        int clip = -1;
        bool dual = false;
        int handRemainingAfter = -1;
    };

    // Only actual native consumption enters this journal. Projection is scoped
    // to the simulation point, so replaying an old command cannot spend a
    // newer command's round twice. The native shared clip validates every read.
    class Journal
    {
    public:
        void Reset() { *this = {}; }
        bool Refresh(std::uintptr_t owner, const l4d2vr_pistol_sync::State& state)
        {
            if (!owner || !state.token || !state.sequence || !state.known || !state.ValidContents())
            { Reset(); return false; }
            if (m_Owner != owner || m_Base.token != state.token || m_Base.handle != state.handle ||
                m_Base.dual != state.dual || m_Base.capacity != state.capacity)
                Reset();
            if (m_Owner && (state.sequence < m_Base.sequence || state.command < m_Base.command ||
                (state.sequence == m_Base.sequence &&
                    (state.command != m_Base.command || !state.SameContents(m_Base))) ||
                (!m_Known && state.sequence == m_BrokenSequence))) return false;
            m_Owner = owner; m_Base = state; m_Known = true;
            for (auto& event : m_Events)
                if (event.command && event.command <= state.command) event = {};
            return true;
        }
        bool Counts(int nativeClip, std::uint32_t command, unsigned ordinal, int& right, int& left) const
        {
            right = left = -1;
            if (!m_Known || nativeClip < 0 || command > 0x7fffffffu ||
                (command && (command <= m_Base.command || ordinal == 0u || ordinal > 64u))) return false;
            int r = m_Base.right, l = m_Base.left;
            for (const auto& event : m_Events)
            {
                if (!event.command || event.command <= m_Base.command ||
                    (command && (event.command > command || (event.command == command && event.ordinal >= ordinal))))
                    continue;
                (event.hand == Hand::Right ? r : l) -= event.rounds;
            }
            if (r < 0 || l < 0 || r + l != nativeClip) return false;
            right = r; left = l; return true;
        }
        bool Record(std::uint32_t command, unsigned ordinal, Hand hand, int before, int after, unsigned bullets)
        {
            if (!m_Known || !command || command > 0x7fffffffu || !ordinal || ordinal > 64u ||
                (hand != Hand::Right && hand != Hand::Left)) return false;
            if (command <= m_Base.command) return true; // already settled by the host
            const int capacity = m_Base.capacity * (m_Base.dual ? 2 : 1);
            if (before < 0 || after < 0 || before > capacity || after > capacity)
            { Invalidate(); return false; }
            if (before == after && before >= 0)
            { Cancel(command, ordinal); return true; } // cooldown, rejection or native non-consumption
            const int rounds = before - after;
            int right = -1, left = -1;
            if (after < 0 || rounds <= 0 || rounds > m_Base.capacity || bullets != static_cast<unsigned>(rounds) ||
                !Counts(before, command, ordinal, right, left) || (hand == Hand::Right ? right : left) < rounds)
            { Invalidate(); return false; }
            Event* vacant = nullptr;
            for (auto& event : m_Events)
            {
                if (event.command == command && event.ordinal == ordinal)
                {
                    if (event.hand != hand || event.rounds != rounds) { Invalidate(); return false; }
                    return true;
                }
                if (!event.command) vacant = &event;
            }
            if (!vacant) { Invalidate(); return false; }
            *vacant = {command, ordinal, hand, rounds};
            return true;
        }
        void Cancel(std::uint32_t command, unsigned ordinal)
        {
            for (auto& event : m_Events)
                if (event.command == command && event.ordinal == ordinal) event = {};
        }
    private:
        void Invalidate()
        {
            m_Known = false; m_BrokenSequence = m_Base.sequence;
            m_Events.fill(Event{});
        }
        struct Event
        {
            std::uint32_t command = 0u;
            unsigned ordinal = 0u;
            Hand hand = Hand::None;
            int rounds = 0;
        };
        std::uintptr_t m_Owner = 0u;
        l4d2vr_pistol_sync::State m_Base{};
        std::uint32_t m_BrokenSequence = 0u;
        bool m_Known = false;
        std::array<Event, 64> m_Events{};
    };
}
