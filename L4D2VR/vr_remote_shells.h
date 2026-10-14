#pragma once
#include <cstdint>
#include <limits>

namespace l4d2vr_shell
{
    constexpr unsigned kVersion = 1u;
    constexpr std::uint64_t kReplyTimeoutMs = 2500u;
    inline bool IsShotgun(int id) { return id == 3 || id == 4 || id == 8 || id == 11; }
    inline bool ValidHandle(std::uint32_t handle)
    {
        // L4D2: 12 local entry bits, 10 networked serial bits. World is not a weapon.
        const auto entry = handle & 0xfffu;
        return handle < (1u << 22) && entry > 0u && entry < 2048u;
    }
    inline bool HandleMatches(std::uint32_t handle, int entry, unsigned serial)
    {
        return ValidHandle(handle) && (handle & 0xfffu) == static_cast<unsigned>(entry) &&
            (handle >> 12) == (serial & 0x3ffu);
    }
    inline bool ParseNumber(const char* text, std::uint32_t maximum, std::uint32_t& result)
    {
        if (!text || !*text) return false;
        std::uint32_t value = 0u;
        for (unsigned length = 0; *text; ++text, ++length)
        {
            if (length >= 10u || *text < '0' || *text > '9') return false;
            const auto digit = static_cast<std::uint32_t>(*text - '0');
            if (digit > maximum || value > (maximum - digit) / 10u) return false;
            value = value * 10u + digit;
        }
        result = value; return true;
    }
    struct Request
    {
        std::uint32_t token = 0u, sequence = 0u, handle = 0u, command = 0u;
        int weaponId = 0, clip = 0, reserve = 0;
        bool operator==(const Request& other) const
        {
            return token == other.token && sequence == other.sequence && handle == other.handle &&
                command == other.command && weaponId == other.weaponId && clip == other.clip && reserve == other.reserve;
        }
    };
    enum class Status : unsigned
    {
        Applied = 0, Session = 1, Stale = 2, Weapon = 3, NoSpaceOrAmmo = 4,
        RateLimited = 5, Backend = 6, Unsupported = 7
    };
    inline bool RetainRejectedShell(Status status)
    { return status == Status::Stale || status == Status::NoSpaceOrAmmo || status == Status::RateLimited; }
    inline bool ShellRemainsHeld(bool held, bool gripDown, bool quickReload, bool inputAvailable = true)
    { return held && (!inputAvailable || gripDown || quickReload); }

    class RetryContact
    {
    public:
        void Reset() { m_RequiresExit = false; }
        void Reject(Status status) { m_RequiresExit = RetainRejectedShell(status); }
        bool Allows(bool touching)
        {
            if (!touching) m_RequiresExit = false;
            return touching && !m_RequiresExit;
        }
    private:
        bool m_RequiresExit = false;
    };
    struct Reply
    {
        Request request{};
        Status status = Status::Backend;
        int clip = 0, reserve = 0;
    };
    struct Snapshot
    {
        bool eligible = false;
        std::uint32_t handle = 0u, latestCommand = 0u;
        int weaponId = 0, clip = 0, reserve = 0, capacity = 0;
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
        template<class Writer> Reply Apply(const Request& request, const Snapshot& state,
            std::uint64_t now, Writer writer)
        {
            Reply reply{request, Status::Session, state.clip, state.reserve};
            if (!m_Token || request.token != m_Token || !request.sequence) return reply;
            if (m_HaveMemo && request == m_Memo.request) return m_Memo;
            if (request.sequence <= m_HighSequence) { reply.status = Status::Stale; return reply; }
            m_HighSequence = request.sequence;
            if (!state.eligible || !ValidHandle(request.handle) || request.handle != state.handle ||
                request.weaponId != state.weaponId || !IsShotgun(state.weaponId)) reply.status = Status::Weapon;
            else if (!request.command || !state.latestCommand ||
                static_cast<std::int64_t>(state.latestCommand) - request.command > 48 ||
                static_cast<std::int64_t>(request.command) - state.latestCommand > 8 ||
                state.clip != request.clip || state.reserve != request.reserve) reply.status = Status::Stale;
            else if (state.capacity <= 0 || state.capacity > 128 || state.clip < 0 ||
                state.clip >= state.capacity || state.reserve < 1 || state.reserve > 5000) reply.status = Status::NoSpaceOrAmmo;
            else if (m_HaveApplied && (now < m_LastApplied || now - m_LastApplied < 100u)) reply.status = Status::RateLimited;
            else if (!writer(state.clip + 1, state.reserve - 1)) reply.status = Status::Backend;
            else
            {
                reply.status = Status::Applied; reply.clip = state.clip + 1; reply.reserve = state.reserve - 1;
                m_LastApplied = now; m_HaveApplied = true;
            }
            m_Memo = reply; m_HaveMemo = true;
            return reply;
        }
    private:
        std::uint32_t m_Token = 0u, m_HighSequence = 0u;
        bool m_HaveMemo = false, m_HaveApplied = false;
        std::uint64_t m_LastApplied = 0u;
        Reply m_Memo{};
    };

    class ClientRequest
    {
    public:
        enum class Poll { None, Waiting, Result, Timeout };
        void Offer(unsigned version, std::uint32_t token)
        {
            if (version != kVersion || !token || token == m_BlockedToken || token == m_Token) return;
            *this = {}; m_Token = token;
        }
        bool Supported() const { return m_Token != 0u; }
        bool Pending() const { return m_Pending; }
        void Disconnect() { *this = {}; }
        void Cancel() { m_Pending = m_Ready = false; m_Retries = 0u; }
        bool Begin(std::uint32_t handle, int id, int clip, int reserve, std::uint32_t command,
            std::uintptr_t owner, std::uint32_t generation, std::uint64_t now, Request& out)
        {
            if (!Supported() || m_Pending || !ValidHandle(handle) || !IsShotgun(id) || !owner ||
                clip < 0 || clip > 128 || reserve < 1 || reserve > 5000 || !command ||
                m_Next == (std::numeric_limits<std::uint32_t>::max)()) return false;
            m_Request = {m_Token, ++m_Next, handle, command, id, clip, reserve};
            m_Owner = owner; m_Generation = generation; m_Started = now;
            m_Pending = true; m_Ready = false; m_Retries = 0u; m_LastRetry = now; out = m_Request; return true;
        }
        bool NextRetry(std::uint64_t now, Request& request)
        {
            if (!m_Pending || m_Ready || m_Retries >= 3u || now < m_Started || now - m_Started >= kReplyTimeoutMs ||
                now < m_LastRetry || now - m_LastRetry < 500u) return false;
            ++m_Retries; m_LastRetry = now; request = m_Request;
            return true;
        }
        void Receive(const Reply& reply)
        {
            if (!m_Pending || !(reply.request == m_Request)) return;
            if (reply.status == Status::Applied &&
                (reply.clip != m_Request.clip + 1 || reply.reserve != m_Request.reserve - 1)) return;
            m_Reply = reply; m_Ready = true;
        }
        Poll Update(std::uint32_t handle, int id, std::uintptr_t owner, std::uint32_t generation,
            std::uint64_t now, Reply& result, int replicatedClip = -1, int replicatedReserve = -1)
        {
            if (!m_Pending) return Poll::None;
            if (handle != m_Request.handle || id != m_Request.weaponId || owner != m_Owner || generation != m_Generation)
            { Cancel(); return Poll::None; }
            if (now < m_Started || now - m_Started >= kReplyTimeoutMs)
            {
                m_BlockedToken = m_Token; m_Token = 0u; Cancel(); return Poll::Timeout;
            }
            if (!m_Ready) return Poll::Waiting;
            if (m_Reply.status == Status::Applied &&
                (replicatedClip != m_Reply.clip || replicatedReserve != m_Reply.reserve)) return Poll::Waiting;
            result = m_Reply;
            if (result.status == Status::Unsupported || result.status == Status::Backend)
            { m_BlockedToken = m_Token; m_Token = 0u; }
            Cancel(); return Poll::Result;
        }
    private:
        std::uint32_t m_Token = 0u, m_BlockedToken = 0u, m_Next = 0u, m_Generation = 0u;
        std::uintptr_t m_Owner = 0u;
        std::uint64_t m_Started = 0u, m_LastRetry = 0u;
        unsigned m_Retries = 0u;
        bool m_Pending = false, m_Ready = false;
        Request m_Request{};
        Reply m_Reply{};
    };
}
