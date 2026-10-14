#pragma once
#include "vr_remote_shells.h"
#include <cstdint>

namespace l4d2vr_pistol_sync
{
    constexpr unsigned kVersion = 3u;
    constexpr std::uint64_t kHeartbeatMs = 200u, kExpiryMs = 1000u;
    struct State
    {
        std::uint32_t token = 0u, sequence = 0u, handle = 0u, command = 0u;
        int capacity = 0, clip = 0, right = 0, left = 0;
        bool known = false, dual = false;
        unsigned physical = 0u, attached = 0u, chambered = 0u;
        std::uint32_t reloadSequence = 0u;
        bool ValidContents() const
        {
            if (!l4d2vr_shell::ValidHandle(handle) || !command || command > 0x7fffffffu ||
                capacity <= 0 || capacity > 15 || clip < 0 || clip > capacity * (dual ? 2 : 1) ||
                right < 0 || left < 0 || right > capacity || left > capacity ||
                physical > 3u || attached > 3u || chambered > 3u ||
                ((chambered & 1u) != 0u && right == 0) || ((chambered & 2u) != 0u && left == 0) ||
                (!physical && (attached || chambered)) ||
                (!dual && (physical == 3u || ((attached | chambered) & ~physical) != 0u ||
                    (physical == 1u && left > 0) || (physical == 2u && right > 0)))) return false;
            return known ? right + left == clip && (dual || right == 0 || left == 0) :
                right == 0 && left == 0 && !physical && !attached && !chambered;
        }
        bool SameContents(const State& other) const
        {
            return handle == other.handle && capacity == other.capacity && clip == other.clip &&
                right == other.right && left == other.left && known == other.known && dual == other.dual &&
                physical == other.physical && attached == other.attached && chambered == other.chambered &&
                reloadSequence == other.reloadSequence;
        }
    };
    class Sender
    {
    public:
        void Reset(std::uint32_t token) { *this = {}; m_Token = token; }
        std::uint32_t Token() const { return m_Sequence == 0xffffffffu ? 0u : m_Token; }
        bool Ready() const { return m_Ready && Token() != 0u; }
        bool Acknowledge(unsigned version, std::uint32_t token)
        {
            if (!token || token != Token()) return false;
            if (version == 0u) { m_Ready = false; return true; }
            if (version != kVersion) return false;
            m_Ready = true; return true;
        }
        bool Prepare(State state, std::uint64_t now, State& result)
        {
            if (!Ready() || !state.ValidContents() || m_Sequence == 0xffffffffu ||
                (m_HaveState && (state.command < m_Last.command || now < m_SentAt))) return false;
            if (m_HaveState && state.SameContents(m_Last) && now - m_SentAt < kHeartbeatMs) return false;
            state.token = m_Token; state.sequence = ++m_Sequence;
            m_Last = result = state; m_SentAt = now; m_HaveState = true;
            return true;
        }
    private:
        std::uint32_t m_Token = 0u, m_Sequence = 0u;
        State m_Last{};
        std::uint64_t m_SentAt = 0u;
        bool m_HaveState = false;
        bool m_Ready = false;
    };
    class Receiver
    {
    public:
        void Disconnect() { *this = {}; }
        std::uint32_t Token() const { return m_Token; }
        bool Offer(unsigned version, std::uint32_t token)
        {
            if (version != kVersion || !token) { Disconnect(); return false; }
            if (token != m_Token) { Disconnect(); m_Token = token; }
            return true;
        }
        bool Receive(const State& state, std::uint64_t now)
        {
            if (!m_Token || state.token != m_Token || !state.sequence || !state.ValidContents() ||
                state.sequence <= m_HighSequence || state.command < m_HighCommand ||
                (m_HighSequence && now < m_ReceivedAt)) return false;
            m_HighSequence = state.sequence; m_HighCommand = state.command;
            m_State = state; m_ReceivedAt = now; m_HaveState = true;
            return true;
        }
        bool Read(std::uintptr_t owner, std::uint32_t handle, int clip, bool dual,
            std::uint64_t now, State& result)
        {
            State state{};
            if (!ReadBaseline(owner, handle, dual, now, state) || state.clip != clip) return false;
            result = state; return true;
        }
        bool ReadBaseline(std::uintptr_t owner, std::uint32_t handle, bool dual,
            std::uint64_t now, State& result)
        {
            if (!owner) { m_Owner = 0u; m_HaveState = false; return false; }
            if (m_Owner && m_Owner != owner) m_HaveState = false;
            m_Owner = owner;
            if (!m_Token || !m_HaveState || !m_State.known || m_State.handle != handle ||
                m_State.dual != dual || now < m_ReceivedAt ||
                now - m_ReceivedAt > kExpiryMs) return false;
            result = m_State; return true;
        }
    private:
        std::uint32_t m_Token = 0u, m_HighSequence = 0u, m_HighCommand = 0u;
        std::uintptr_t m_Owner = 0u;
        State m_State{};
        std::uint64_t m_ReceivedAt = 0u;
        bool m_HaveState = false;
    };
}
