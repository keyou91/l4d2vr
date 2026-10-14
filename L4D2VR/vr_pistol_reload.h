#pragma once
#include "vr_pistol_ammo.h"
#include "vr_remote_shells.h"
#include "vr_pistol_ammo_sync.h"

namespace l4d2vr_pistol_reload
{
    using Hand = l4d2vr_pistol::Hand;
    using Action = l4d2vr_pistol::MagazineAction;
    using Status = l4d2vr_shell::Status;
    inline bool BlocksNativeReload(bool eligible, unsigned physical, bool manualSingle = false)
    { return eligible && physical <= 3u && (manualSingle || physical != 0u); }
    inline bool MatchesSingleHand(const l4d2vr_pistol_sync::State& state, Hand hand)
    {
        if (!state.known || state.dual || !state.ValidContents()) return false;
        return hand == Hand::Right ? state.left == 0 && (state.physical & 2u) == 0u :
            hand == Hand::Left ? state.right == 0 && (state.physical & 1u) == 0u : false;
    }
    struct Request
    {
        std::uint32_t token = 0u, sequence = 0u, handle = 0u, command = 0u;
        int clip = 0, reserve = 0;
        Hand hand = Hand::None;
        Action action = Action::Eject;
        bool operator==(const Request& other) const
        {
            return token == other.token && sequence == other.sequence && handle == other.handle &&
                command == other.command && clip == other.clip && reserve == other.reserve &&
                hand == other.hand && action == other.action;
        }
    };
    struct Reply
    {
        Request request{};
        Status status = Status::Backend;
        l4d2vr_pistol::MagazineResult result{};
    };
    enum class GestureOutcome
    { Cancel, Ejected, MagazineReady, NeedsCycle, Cycled, RetryMagazine, RetryCycle };
    // Only a settled host transaction may advance the physical gesture.
    inline GestureOutcome SettleGesture(const Reply& reply, Hand hand)
    {
        if ((hand != Hand::Right && hand != Hand::Left) || reply.request.hand != hand ||
            static_cast<unsigned>(reply.request.action) > 3u)
            return GestureOutcome::Cancel;
        if (reply.status != Status::Applied)
        {
            if (reply.status != Status::Stale && reply.status != Status::RateLimited &&
                reply.status != Status::NoSpaceOrAmmo) return GestureOutcome::Cancel;
            return reply.request.action == Action::Cycle ? GestureOutcome::RetryCycle :
                reply.request.action == Action::Eject ? GestureOutcome::Cancel : GestureOutcome::RetryMagazine;
        }
        const unsigned bit = l4d2vr_pistol::Bit(hand);
        if ((reply.result.state.physical & bit) == 0u) return GestureOutcome::Cancel;
        const bool attached = (reply.result.state.attached & bit) != 0u;
        const bool chambered = (reply.result.state.chambered & bit) != 0u;
        if (reply.request.action == Action::Eject)
            return !attached ? GestureOutcome::Ejected : GestureOutcome::Cancel;
        if (!attached) return GestureOutcome::Cancel;
        if (reply.request.action == Action::Cycle)
            return chambered ? GestureOutcome::Cycled : GestureOutcome::Cancel;
        return chambered ? GestureOutcome::MagazineReady : GestureOutcome::NeedsCycle;
    }
    struct Snapshot
    {
        bool eligible = false;
        std::uint32_t handle = 0u, command = 0u;
        l4d2vr_pistol::AmmoSnapshot ammo{};
        int reserve = 0;
        bool infiniteReserve = false;
    };
    class Server
    {
    public:
        void Reset(std::uint32_t token) { *this = {}; m_Token = token; }
        template<class Writer> Reply Apply(const Request& request, const Snapshot& snapshot,
            std::uint64_t now, l4d2vr_pistol::AmmoLedger& ammo, Writer writer)
        {
            Reply reply{request, Status::Session, {snapshot.ammo.clip, snapshot.reserve}};
            if (!m_Token || request.token != m_Token || !request.sequence) return reply;
            if (m_HaveMemo && request == m_Memo.request) return m_Memo;
            if (request.sequence <= m_HighSequence) { reply.status = Status::Stale; return reply; }
            m_HighSequence = request.sequence;
            const unsigned hand = request.hand == Hand::Right ? 0u : 1u;
            bool nativeFailure = false;
            if (!snapshot.eligible || !snapshot.ammo.Valid() || !l4d2vr_shell::ValidHandle(request.handle) ||
                request.handle != snapshot.handle || (request.hand != Hand::Right && request.hand != Hand::Left) ||
                static_cast<unsigned>(request.action) > 3u) reply.status = Status::Weapon;
            else if (!request.command || !snapshot.command || request.command > 0x7fffffffu ||
                static_cast<std::int64_t>(snapshot.command) - request.command > 48 ||
                request.command > snapshot.command ||
                request.clip != snapshot.ammo.clip || request.reserve != snapshot.reserve)
                reply.status = Status::Stale;
            else if (m_HaveApplied[hand] && (now < m_AppliedAt[hand] || now - m_AppliedAt[hand] < 80u))
                reply.status = Status::RateLimited;
            else if (!ammo.Magazine(snapshot.ammo, request.hand, request.action, snapshot.reserve,
                snapshot.infiniteReserve, [&](int clip, int reserve)
                { const bool ok = writer(clip, reserve); nativeFailure = !ok; return ok; }, reply.result))
                reply.status = nativeFailure ? Status::Backend : Status::NoSpaceOrAmmo;
            else
            {
                reply.status = Status::Applied;
                m_HaveApplied[hand] = true; m_AppliedAt[hand] = now;
            }
            m_Memo = reply; m_HaveMemo = true; return reply;
        }
    private:
        std::uint32_t m_Token = 0u, m_HighSequence = 0u;
        bool m_HaveMemo = false;
        Reply m_Memo{};
        std::array<bool, 2> m_HaveApplied{};
        std::array<std::uint64_t, 2> m_AppliedAt{};
    };
    class Client
    {
    public:
        enum class Poll { None, Waiting, Result, Timeout };
        void Offer(std::uint32_t token)
        { if (token != m_BlockedToken && token != m_Token) { *this = {}; m_Token = token; } }
        void Disconnect() { *this = {}; }
        bool Pending() const { return m_Pending; }
        bool Blocked(std::uint32_t token) const { return token != 0u && token == m_BlockedToken; }
        void Cancel() { m_Pending = m_Ready = false; m_Retries = 0u; }
        bool Begin(std::uint32_t handle, int clip, int reserve, Hand hand, Action action,
            std::uint32_t command, std::uintptr_t owner, bool dual, std::uint64_t now, Request& request)
        {
            if (!m_Token || m_Pending || !owner || !command || command > 0x7fffffffu ||
                !l4d2vr_shell::ValidHandle(handle) || clip < 0 || clip > 30 || reserve < 0 || reserve > 5000 ||
                (hand != Hand::Right && hand != Hand::Left) || static_cast<unsigned>(action) > 3u || m_Sequence == 0xffffffffu)
                return false;
            m_Request = {m_Token, ++m_Sequence, handle, command, clip, reserve, hand, action};
            m_Owner = owner; m_Dual = dual; m_Started = m_LastRetry = now;
            m_Pending = true; m_Ready = false; m_Retries = 0u;
            request = m_Request; return true;
        }
        bool NextRetry(std::uint64_t now, Request& request)
        {
            if (!m_Pending || m_Ready || m_Retries >= 3u || now < m_Started ||
                now - m_Started >= 2500u || now < m_LastRetry || now - m_LastRetry < 500u)
                return false;
            ++m_Retries; m_LastRetry = now; request = m_Request;
            return true;
        }
        void Receive(const Reply& reply)
        {
            if (!m_Pending || !(reply.request == m_Request) || static_cast<unsigned>(reply.status) > 7u) return;
            const auto& result = reply.result;
            if (reply.status == Status::Applied)
            {
                if (result.clip < 0 || result.clip > 30 || result.reserve < 0 || result.reserve > 5000 ||
                    result.removed < 0 || result.removed > 15 || result.added < 0 || result.added > 15 ||
                    result.released < 0 || result.released > 15 || result.state.physical > 3u ||
                    result.state.attached > 3u || result.state.chambered > 3u) return;
                const unsigned bit = l4d2vr_pistol::Bit(m_Request.hand);
                if ((result.state.physical & bit) == 0u) return;
                if (m_Request.action == Action::Eject)
                {
                    if (result.clip != m_Request.clip - result.removed || result.reserve != m_Request.reserve ||
                        result.added != 0 || result.released != 0 || (result.state.attached & bit) != 0u) return;
                }
                else if (m_Request.action == Action::Cycle)
                {
                    if (result.clip != m_Request.clip || result.reserve != m_Request.reserve || result.removed || result.added || result.released ||
                        (result.state.chambered & bit) == 0u) return;
                }
                else if (result.clip != m_Request.clip + result.added || result.removed ||
                    (m_Request.action == Action::Reinsert && result.released != 0) ||
                    (m_Request.action == Action::Insert && result.added <= 0) ||
                    (result.state.attached & bit) == 0u ||
                    (m_Request.action == Action::Reinsert ? result.reserve != m_Request.reserve :
                        !(result.reserve == m_Request.reserve || result.reserve == m_Request.reserve - result.added))) return;
            }
            m_Reply = reply; m_Ready = true;
        }
        Poll Update(std::uintptr_t owner, std::uint32_t handle, bool dual, std::uint64_t now,
            const l4d2vr_pistol_sync::State& state, bool nativeMatches, Reply& reply)
        {
            if (!m_Pending) return Poll::None;
            if (!owner || owner != m_Owner || handle != m_Request.handle || dual != m_Dual)
            { Cancel(); return Poll::None; }
            if (now < m_Started || now - m_Started >= 2500u)
            { reply = {m_Request, Status::Backend, {}}; m_BlockedToken = m_Token; m_Token = 0u; Cancel(); return Poll::Timeout; }
            if (!m_Ready || (m_Reply.status == Status::Applied && (!nativeMatches || !state.known ||
                state.token != m_Request.token || state.handle != handle || state.reloadSequence < m_Request.sequence)))
                return Poll::Waiting;
            reply = m_Reply; Cancel(); return Poll::Result;
        }
    private:
        std::uint32_t m_Token = 0u, m_Sequence = 0u, m_BlockedToken = 0u;
        std::uintptr_t m_Owner = 0u;
        std::uint64_t m_Started = 0u, m_LastRetry = 0u;
        unsigned m_Retries = 0u;
        bool m_Pending = false, m_Ready = false, m_Dual = false;
        Request m_Request{};
        Reply m_Reply{};
    };
}
