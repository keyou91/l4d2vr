#pragma once
#include "vr_remote_shells.h"
#include <algorithm>
#include <array>

namespace l4d2vr_remote_mag
{
    constexpr unsigned kVersion = 2u;
    constexpr int kMaxClip = 512;
    using Status = l4d2vr_shell::Status;
    enum class Action : unsigned { Eject = 0, Insert = 1, Reinsert = 2 };
    inline bool RetainsRejectedHeldMagazine(Status status, Action action, bool held,
        bool inputKnown, bool gripDown, bool retained, bool pistol)
    {
        if (!held || action == Action::Eject || status == Status::Applied)
            return false;
        return l4d2vr_shell::RetainRejectedShell(status) ||
            (inputKnown && gripDown && (pistol || (action == Action::Reinsert && retained)));
    }
    inline bool ValidAction(Action action)
    {
        return action == Action::Eject || action == Action::Insert || action == Action::Reinsert;
    }
    inline bool IsDetachable(int id)
    {
        switch (id)
        {
        case 1: case 2: case 5: case 6: case 7: case 9: case 10:
        case 26: case 32: case 33: case 34: case 35: case 36: return true;
        default: return false;
        }
    }
    struct Request
    {
        std::uint32_t token = 0, sequence = 0, handle = 0, command = 0;
        int weaponId = 0, clip = 0, reserve = 0;
        Action action = Action::Eject;
        bool operator==(const Request& other) const
        {
            return token == other.token && sequence == other.sequence && handle == other.handle &&
                command == other.command && weaponId == other.weaponId && clip == other.clip &&
                reserve == other.reserve && action == other.action;
        }
    };
    struct Reply { Request request{}; Status status = Status::Backend; int clip = 0, reserve = 0; };
    struct Snapshot
    {
        bool eligible = false;
        std::uint32_t handle = 0, latestCommand = 0;
        int weaponId = 0, clip = 0, reserve = 0, capacity = 0;
        bool infinite = false;
    };
    class ServerLedger
    {
    public:
        void Reset(std::uint32_t token) { *this = {}; m_Token = token; }
        std::uint32_t Token() const { return m_Token; }
        Reply Unsupported(const Request& request)
        {
            Reply reply{request, Status::Session, 0, 0};
            if (!m_Token || request.token != m_Token || !request.sequence) return reply;
            if (m_HaveMemo && request == m_Memo.request) return m_Memo;
            if (request.sequence <= m_HighSequence) { reply.status = Status::Stale; return reply; }
            m_HighSequence = request.sequence;
            reply.status = Status::Unsupported; m_Memo = reply; m_HaveMemo = true;
            return reply;
        }
        template<class Writer> Reply Apply(const Request& request, const Snapshot state,
            std::uint64_t now, Writer writer)
        {
            Reply reply{request, Status::Session, state.clip, state.reserve};
            if (!m_Token || request.token != m_Token || !request.sequence) return reply;
            if (m_HaveMemo && request == m_Memo.request) return m_Memo;
            if (request.sequence <= m_HighSequence) { reply.status = Status::Stale; return reply; }
            m_HighSequence = request.sequence;
            auto* detached = Find(state.handle);
            if (!state.eligible || !l4d2vr_shell::ValidHandle(request.handle) || request.handle != state.handle ||
                request.weaponId != state.weaponId || !IsDetachable(state.weaponId) ||
                !ValidAction(request.action)) reply.status = Status::Weapon;
            else if (!request.command || !state.latestCommand ||
                static_cast<std::int64_t>(state.latestCommand) - request.command > 48 ||
                static_cast<std::int64_t>(request.command) - state.latestCommand > 8 ||
                state.clip != request.clip || state.reserve != request.reserve) reply.status = Status::Stale;
            else if (state.capacity <= 0 || state.capacity > kMaxClip || state.clip < 0 ||
                state.clip > state.capacity || state.reserve < 0 || state.reserve > 5000)
                reply.status = Status::NoSpaceOrAmmo;
            else if (request.action != Action::Eject && (!detached || state.clip > 1)) reply.status = Status::Weapon;
            else if (request.action == Action::Reinsert && detached->rounds > state.capacity - state.clip)
                reply.status = Status::NoSpaceOrAmmo;
            else if (m_HaveApplied && (now < m_LastApplied || now - m_LastApplied < 100u))
                reply.status = Status::RateLimited;
            else
            {
                const bool eject = request.action == Action::Eject;
                const bool reinsert = request.action == Action::Reinsert;
                if (eject && !detached) detached = EmptySlot();
                const int added = eject ? 0 : std::min(state.capacity - state.clip,
                    reinsert ? detached->rounds : state.infinite ? state.capacity : state.reserve);
                const int clip = eject ? std::min(1, state.clip) : state.clip + added;
                const int reserve = state.reserve - ((!eject && !reinsert && !state.infinite) ? added : 0);
                if ((eject && !detached) || (!eject && !reinsert && added <= 0)) reply.status = Status::NoSpaceOrAmmo;
                else if (!writer(clip, reserve)) reply.status = Status::Backend;
                else
                {
                    if (eject)
                    {
                        // A new gesture replaces the old catch opportunity.
                        // Exact network repeats reuse the memo before this write.
                        *detached = {state.handle, state.clip - clip};
                    }
                    else *detached = {};
                    reply.status = Status::Applied; reply.clip = clip; reply.reserve = reserve;
                    m_LastApplied = now; m_HaveApplied = true;
                }
            }
            m_Memo = reply; m_HaveMemo = true; return reply;
        }
    private:
        struct Detached { std::uint32_t handle = 0; int rounds = 0; };
        Detached* Find(std::uint32_t handle)
        {
            if (!handle) return nullptr;
            for (auto& entry : m_Detached) if (entry.handle == handle) return &entry;
            return nullptr;
        }
        Detached* EmptySlot()
        {
            for (auto& entry : m_Detached) if (!entry.handle) return &entry;
            // Keep the lease bounded during long sessions with many dropped
            // guns. An evicted gun must eject again before it can insert.
            auto* entry = &m_Detached[m_NextEviction];
            m_NextEviction = (m_NextEviction + 1u) % m_Detached.size();
            return entry;
        }
        std::array<Detached, 16> m_Detached{};
        std::size_t m_NextEviction = 0;
        std::uint32_t m_Token = 0, m_HighSequence = 0;
        bool m_HaveMemo = false, m_HaveApplied = false;
        std::uint64_t m_LastApplied = 0;
        Reply m_Memo{};
    };
    class ClientRequest
    {
    public:
        using Poll = l4d2vr_shell::ClientRequest::Poll;
        void Offer(unsigned version, std::uint32_t token)
        {
            if (version != kVersion || !token || token == m_BlockedToken || token == m_Token) return;
            *this = {}; m_Token = token;
        }
        bool Supported() const { return m_Token != 0; }
        bool Pending() const { return m_Pending; }
        void Disconnect() { *this = {}; }
        void Cancel() { m_Pending = m_Ready = false; m_Retries = 0u; }
        bool Begin(std::uint32_t handle, int id, int clip, int reserve, Action action, std::uint32_t command,
            std::uintptr_t owner, std::uint32_t generation, std::uint64_t now, Request& out)
        {
            if (!Supported() || m_Pending || !l4d2vr_shell::ValidHandle(handle) || !IsDetachable(id) || !owner ||
                clip < 0 || clip > kMaxClip || reserve < 0 || reserve > 5000 || !command ||
                !ValidAction(action) ||
                m_Next == (std::numeric_limits<std::uint32_t>::max)()) return false;
            m_Request = {m_Token, ++m_Next, handle, command, id, clip, reserve, action};
            m_Owner = owner; m_Generation = generation; m_Started = now;
            m_Pending = true; m_Ready = false; m_Retries = 0u; m_LastRetry = now; out = m_Request; return true;
        }
        bool NextRetry(std::uint64_t now, Request& request)
        {
            if (!m_Pending || m_Ready || m_Retries >= 3u || now < m_Started ||
                now - m_Started >= l4d2vr_shell::kReplyTimeoutMs || now < m_LastRetry ||
                now - m_LastRetry < 500u) return false;
            ++m_Retries; m_LastRetry = now; request = m_Request; return true;
        }
        void Receive(const Reply& reply)
        {
            if (!m_Pending || !(reply.request == m_Request)) return;
            if (reply.status == Status::Applied)
            {
                if (reply.clip < 0 || reply.clip > kMaxClip || reply.reserve < 0 || reply.reserve > 5000) return;
                if (m_Request.action == Action::Eject)
                {
                    if (reply.clip != std::min(1, m_Request.clip) || reply.reserve != m_Request.reserve) return;
                }
                else if (m_Request.action == Action::Reinsert)
                {
                    if (reply.clip < m_Request.clip || reply.reserve != m_Request.reserve) return;
                }
                else
                {
                    const int added = reply.clip - m_Request.clip;
                    const int used = m_Request.reserve - reply.reserve;
                    if (added <= 0 || (used != 0 && used != added)) return;
                }
            }
            m_Reply = reply; m_Ready = true;
        }
        Poll Update(std::uint32_t handle, int id, std::uintptr_t owner, std::uint32_t generation,
            std::uint64_t now, Reply& result, int replicatedClip, int replicatedReserve)
        {
            if (!m_Pending) return Poll::None;
            if (handle != m_Request.handle || id != m_Request.weaponId || owner != m_Owner || generation != m_Generation)
            { Cancel(); return Poll::None; }
            if (now < m_Started || now - m_Started >= l4d2vr_shell::kReplyTimeoutMs)
            { m_BlockedToken = m_Token; m_Token = 0; Cancel(); return Poll::Timeout; }
            if (!m_Ready || (m_Reply.status == Status::Applied &&
                (replicatedClip != m_Reply.clip || replicatedReserve != m_Reply.reserve))) return Poll::Waiting;
            result = m_Reply;
            if (result.status == Status::Unsupported || result.status == Status::Backend)
            { m_BlockedToken = m_Token; m_Token = 0; }
            Cancel(); return Poll::Result;
        }
    private:
        std::uint32_t m_Token = 0, m_BlockedToken = 0, m_Next = 0, m_Generation = 0;
        std::uintptr_t m_Owner = 0;
        std::uint64_t m_Started = 0, m_LastRetry = 0;
        unsigned m_Retries = 0u;
        bool m_Pending = false, m_Ready = false;
        Request m_Request{};
        Reply m_Reply{};
    };
}
