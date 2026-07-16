
#include "vr.h"
#include <Windows.h>
#include <winhttp.h>
#include <mmsystem.h>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "offsets.h"
#include "usercmd.h"
#include "trace.h"
#include "vr_hands/vr_hand_manifest.h"
#include "vr_hands/vr_hand_system.h"
#include "sdk/ivdebugoverlay.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <system_error>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <regex>
#include <vector>
#include <d3d9_vr.h>

#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Winhttp.lib")

namespace
{
    constexpr size_t kMaxActiveKillIndicators = 16;
    constexpr float kKillIndicatorTrimIntervalSeconds = 1.0f / 90.0f;
    constexpr float kHitIndicatorMergeWindowSeconds = 0.12f;
    constexpr float kHitIndicatorMergeDistance = 128.0f;
    constexpr size_t kFeedbackSoundWorkerMaxQueuedJobs = 64;
    constexpr uint32_t kSourceVoiceInputSampleRate = 11025;
    // NOTE: “被控放行”要宁可保守：只在「确实是控制者本人」且「目标非常贴近队友」时才放行。
    // Used by VR::UpdateFriendlyFireAimHit().
    constexpr float kAllowThroughControlledTeammateMaxDist = 64.0f; // units (conservative)
    // Returns true if the call should be skipped because we ran it too recently.
    inline bool ShouldThrottle(std::chrono::steady_clock::time_point& last, float maxHz)
    {
        if (maxHz <= 0.0f)
            return false;

        const float minInterval = 1.0f / std::max(1.0f, maxHz);
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0)
        {
            const float elapsed = std::chrono::duration<float>(now - last).count();
            if (elapsed < minInterval)
                return true;
        }
        last = now;
        return false;
    }

    inline float MinIntervalSeconds(float maxHz)
    {
        if (maxHz <= 0.0f)
            return 0.0f;
        return 1.0f / std::max(1.0f, maxHz);
    }

    inline int FindClientEntityIndexByPointer(IClientEntityList* entityList, const void* ptr)
    {
        if (!entityList || !ptr)
            return -1;

        int highestIndex = entityList->GetHighestEntityIndex();
        if (highestIndex < 1)
            return -1;

        highestIndex = (std::min)(highestIndex, 4096);
        for (int i = 1; i <= highestIndex; ++i)
        {
            if (entityList->GetClientEntity(i) == ptr)
                return i;
        }

        return -1;
    }

    struct VASStats
    {
        size_t freeTotal = 0;
        size_t freeLargest = 0;
        size_t reserved = 0;
        size_t committed = 0;
    };

    inline double BytesToMiB(size_t bytes)
    {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    // Best-effort Virtual Address Space snapshot for 32-bit processes.
    // This is what typical "av" tools visualize: free vs reserved/committed address ranges.
    inline VASStats QueryVASStats()
    {
        VASStats out{};

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        MEMORY_BASIC_INFORMATION mbi{};
        while (addr < maxAddr)
        {
            SIZE_T queried = VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
            if (queried == 0)
                break;

            const size_t regionSize = static_cast<size_t>(mbi.RegionSize);
            switch (mbi.State)
            {
            case MEM_FREE:
                out.freeTotal += regionSize;
                out.freeLargest = std::max(out.freeLargest, regionSize);
                break;
            case MEM_RESERVE:
                out.reserved += regionSize;
                break;
            case MEM_COMMIT:
                out.committed += regionSize;
                break;
            default:
                break;
            }

            // Advance to the next region.
            addr += regionSize ? regionSize : 4096;
        }

        return out;
    }

    // Normalize Source-style view angles:
    // - Bring pitch/yaw into [-180, 180] first (avoid -30 becoming 330 then clamped to 89).
    // - Then clamp pitch to [-89, 89].
    inline void NormalizeAndClampViewAngles(QAngle& a)
    {
        while (a.x > 180.f) a.x -= 360.f;
        while (a.x < -180.f) a.x += 360.f;
        while (a.y > 180.f) a.y -= 360.f;
        while (a.y < -180.f) a.y += 360.f;
        a.z = 0.f;
        if (a.x > 89.f) a.x = 89.f;
        if (a.x < -89.f) a.x = -89.f;
    }
    inline bool IsFirearmWeaponId(C_WeaponCSBase::WeaponID id)
    {
        switch (id)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::MP5:

        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
        case C_WeaponCSBase::WeaponID::SPAS:

        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SG552:

        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:

        case C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER:
        case C_WeaponCSBase::WeaponID::M60:
        case C_WeaponCSBase::WeaponID::MACHINEGUN:
            return true;

        default:
            return false;
        }
    }
}

// ----------------------------
// One Euro filter helpers (for scope stabilization)
// ----------------------------
constexpr float kPi = 3.14159265358979323846f;

inline float OneEuroAlpha(float cutoffHz, float dt)
{
    cutoffHz = std::max(0.0001f, cutoffHz);
    dt = std::max(0.000001f, dt);
    const float tau = 1.0f / (2.0f * kPi * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
}

inline float AngleDeltaDeg(float a, float b)
{
    float d = a - b;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}

inline Vector OneEuroFilterVec3(
    const Vector& x,
    Vector& xHat,
    Vector& dxHat,
    bool& initialized,
    float dt,
    float minCutoff,
    float beta,
    float dCutoff)
{
    if (!initialized)
    {
        initialized = true;
        xHat = x;
        dxHat = { 0.0f, 0.0f, 0.0f };
        return xHat;
    }

    const Vector dx = (x - xHat) * (1.0f / std::max(0.000001f, dt));
    const float aD = OneEuroAlpha(dCutoff, dt);
    dxHat = dxHat + (dx - dxHat) * aD;

    const float speed = VectorLength(dxHat);
    const float cutoff = std::max(0.0001f, minCutoff + beta * speed);
    const float a = OneEuroAlpha(cutoff, dt);
    xHat = xHat + (x - xHat) * a;
    return xHat;
}

inline QAngle OneEuroFilterAngles(
    const QAngle& x,
    QAngle& xHat,
    QAngle& dxHat,
    bool& initialized,
    float dt,
    float minCutoff,
    float beta,
    float dCutoff)
{
    if (!initialized)
    {
        initialized = true;
        xHat = x;
        dxHat = { 0.0f, 0.0f, 0.0f };
        return xHat;
    }

    const float invDt = 1.0f / std::max(0.000001f, dt);
    const QAngle dx = {
        AngleDeltaDeg(x.x, xHat.x) * invDt,
        AngleDeltaDeg(x.y, xHat.y) * invDt,
        AngleDeltaDeg(x.z, xHat.z) * invDt
    };

    const float aD = OneEuroAlpha(dCutoff, dt);
    dxHat.x = dxHat.x + (dx.x - dxHat.x) * aD;
    dxHat.y = dxHat.y + (dx.y - dxHat.y) * aD;
    dxHat.z = dxHat.z + (dx.z - dxHat.z) * aD;

    const float speed = std::sqrt(dxHat.x * dxHat.x + dxHat.y * dxHat.y + dxHat.z * dxHat.z);
    const float cutoff = std::max(0.0001f, minCutoff + beta * speed);
    const float a = OneEuroAlpha(cutoff, dt);

    xHat.x = xHat.x + AngleDeltaDeg(x.x, xHat.x) * a;
    xHat.y = xHat.y + AngleDeltaDeg(x.y, xHat.y) * a;
    xHat.z = xHat.z + AngleDeltaDeg(x.z, xHat.z) * a;

    // Keep angles in a sane range.
    while (xHat.x > 180.f) xHat.x -= 360.f;
    while (xHat.x < -180.f) xHat.x += 360.f;
    while (xHat.y > 180.f) xHat.y -= 360.f;
    while (xHat.y < -180.f) xHat.y += 360.f;
    while (xHat.z > 180.f) xHat.z -= 360.f;
    while (xHat.z < -180.f) xHat.z += 360.f;
    return xHat;
}

// ----------------------------
// Player name extraction (robust across engine struct variants)
// ----------------------------

static inline size_t BoundedStrLen(const char* s, size_t maxLen)
{
    if (!s) return 0;
    size_t n = 0;
    for (; n < maxLen; ++n)
    {
        if (s[n] == '\0')
            break;
    }
    return n;
}

static inline bool LooksLikeSteamGuidOrJunk(const char* s)
{
    if (!s || !*s) return true;
    // Reject obvious non-name payloads that could be misread if the struct layout is off.
    // (Examples: STEAM_*, BOT, GUID-like tokens)
    if (std::strncmp(s, "STEAM_", 6) == 0) return true;
    if (std::strncmp(s, "BOT", 3) == 0) return true;
    if (std::strncmp(s, "STEAM_ID_", 9) == 0) return true;

    // Reject GUID-like or machine-token payloads (mostly hex + separators).
    const size_t n = std::strlen(s);
    if (n >= 16)
    {
        size_t hexish = 0;
        size_t separators = 0;
        size_t other = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char c = (unsigned char)s[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                ++hexish;
            else if (c == '-' || c == '_' || c == ':' || c == '{' || c == '}')
                ++separators;
            else
                ++other;
        }
        if (other == 0 && hexish >= 12)
            return true;
    }

    return false;
}

static inline bool CopyNormalizedNameCandidate(const char* src, size_t srcMax, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0) return false;
    dst[0] = 0;
    if (!src) return false;

    // Require a terminating NUL within srcMax.
    const size_t rawLen = BoundedStrLen(src, srcMax);
    if (rawLen == 0 || rawLen >= srcMax)
        return false;

    // Trim leading ASCII whitespace/control chars.
    size_t b = 0;
    while (b < rawLen)
    {
        const unsigned char c = (unsigned char)src[b];
        if (c > 0x20) break;
        ++b;
    }
    if (b >= rawLen)
        return false;

    // Trim trailing ASCII whitespace.
    size_t e = rawLen;
    while (e > b)
    {
        const unsigned char c = (unsigned char)src[e - 1];
        if (c > 0x20) break;
        --e;
    }
    if (e <= b)
        return false;

    // Heuristic: reject payloads that are clearly not player names.
    if (LooksLikeSteamGuidOrJunk(src + b))
        return false;

    // Heuristic: reject strings with control chars in the trimmed region.
    int printable = 0;
    int controls = 0;
    for (size_t i = b; i < e; ++i)
    {
        const unsigned char c = (unsigned char)src[i];
        if (c == 0) break;
        if (c < 0x20) ++controls;
        else ++printable;
    }
    if (printable <= 0 || controls > 0)
        return false;

    const size_t want = e - b;
    const size_t ncopy = (want < (dstSize - 1)) ? want : (dstSize - 1);
    std::memcpy(dst, src + b, ncopy);
    dst[ncopy] = 0;
    return dst[0] != 0;
}

static inline int ScoreNameCandidate(const char* s)
{
    if (!s || !*s)
        return (std::numeric_limits<int>::min)();

    size_t len = 0;
    int alnum = 0;
    int printable = 0;
    int punctuation = 0;
    int highBytes = 0;

    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
    {
        const unsigned char c = *p;
        ++len;
        if (c < 0x20 || c == 0x7F)
            return (std::numeric_limits<int>::min)();
        if (c < 0x80)
        {
            ++printable;
            if (std::isalnum(c))
                ++alnum;
            else if (std::ispunct(c))
                ++punctuation;
        }
        else
        {
            ++printable;
            ++highBytes;
        }
    }

    if (len == 0 || printable == 0)
        return (std::numeric_limits<int>::min)();

    int score = 0;
    score += (int)len * 6;          // Prefer fuller names over accidental 1-2 byte junk.
    score += alnum * 2;
    score += highBytes;             // Keep non-ASCII names competitive.
    score -= punctuation;

    // Penalize very short payloads to avoid false positives from misaligned struct probes.
    if (len <= 2) score -= 40;
    else if (len <= 4) score -= 12;

    return score;
}

// Best-effort UTF-8 player name. Works even if our player_info_t struct is slightly mismatched
// by probing multiple plausible locations for the name string.
static inline bool GetPlayerNameUtf8Safe(IEngineClient* engine, int entIndex, char* outName, size_t outNameSize)
{
    if (!outName || outNameSize == 0)
        return false;
    outName[0] = 0;
    if (!engine || entIndex <= 0)
        return false;

    player_info_t info{};
    if (!engine->GetPlayerInfo(entIndex, &info))
        return false;

    char bestName[128] = { 0 };
    int bestScore = (std::numeric_limits<int>::min)();

    auto consider = [&](const char* src, size_t srcMax)
        {
            char cand[128] = { 0 };
            if (!CopyNormalizedNameCandidate(src, srcMax, cand, sizeof(cand)))
                return;

            const int score = ScoreNameCandidate(cand);
            if (score > bestScore)
            {
                bestScore = score;
                const size_t n = (std::min)(std::strlen(cand), sizeof(bestName) - 1);
                if (n > 0)
                    std::memcpy(bestName, cand, n);
                bestName[n] = 0;
            }
        };

    // 1) Preferred fields.
    consider(info.name, sizeof(info.name));
    consider(info.friendsName, sizeof(info.friendsName));

    // 2) Some builds place name at the beginning or after an 8-byte XUID.
    // Probe a few common offsets and pick the most plausible candidate.
    const char* blob = reinterpret_cast<const char*>(&info);
    consider(blob + 16, 128);
    consider(blob + 8, 128);
    consider(blob + 0, 128);

    if (bestScore == (std::numeric_limits<int>::min)() || !bestName[0])
        return false;

    const size_t n = (std::min)(std::strlen(bestName), outNameSize - 1);
    if (n > 0)
        std::memcpy(outName, bestName, n);
    outName[n] = 0;
    return outName[0] != 0;
}


void VR::UpdateHudLiftGestureState(bool inGame)
{
    const bool wasActive = m_HudLiftGestureActive.load(std::memory_order_acquire) != 0;
    auto stopLiftHudCapture = [&](bool clearCapturedHud)
        {
            m_HudLiftGestureActive.store(0, std::memory_order_release);
            m_HudLiftGestureVisibleUntil = {};
            if (clearCapturedHud)
            {
                ClearQueuedHudFresh();
                m_RenderedHud.store(false, std::memory_order_release);
                m_HudPaintedThisFrame.store(false, std::memory_order_release);
            }
        };

    if (!inGame || !m_System)
    {
        stopLiftHudCapture(true);
        m_HudShowActionHeld.store(0, std::memory_order_release);
        m_HudToggleState = false;
        return;
    }

    const bool showHudHeld = PressedDigitalAction(m_ToggleHUD, false);
    m_HudShowActionHeld.store(showHudHeld ? 1u : 0u, std::memory_order_release);

    if (m_HudAlwaysVisible)
    {
        stopLiftHudCapture(false);
        m_HudToggleState = true;
        return;
    }

    vr::TrackedDeviceIndex_t leftControllerIndex =
        m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t rightControllerIndex =
        m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    if (m_LeftHanded)
        std::swap(leftControllerIndex, rightControllerIndex);

    const bool leftControllerValid =
        leftControllerIndex != vr::k_unTrackedDeviceIndexInvalid &&
        leftControllerIndex < vr::k_unMaxTrackedDeviceCount &&
        m_Poses[leftControllerIndex].bPoseIsValid;

    // When the off-hand is actively gripping a two-handed weapon, its normal aiming
    // movement must never be interpreted as a request to reveal the HUD. ShowHUD
    // remains available as the explicit, hold-to-show action.
    const bool offhandGripActive = m_VrHandsTwoHandedGripActive;
    bool raised = false;
    if (leftControllerValid && !offhandGripActive)
    {
        const float scale = (std::max)(1.0f, m_VRScale);

        // Source units: m_VRScale is units per meter. Keep the exit threshold tight:
        // lowering the off-hand must stop normal gameplay HUD capture, not just hide an overlay.
        const float enterBelowHead = 0.24f * scale;
        const float exitBelowHead = 0.31f * scale;
        const float zThreshold = m_HmdPosAbs.z - (wasActive ? exitBelowHead : enterBelowHead);

        const Vector delta = m_LeftControllerPosAbs - m_HmdPosAbs;
        const float horizontalDistSq = delta.x * delta.x + delta.y * delta.y;
        const float maxHorizontalDist = 0.95f * scale;

        raised =
            m_LeftControllerPosAbs.z >= zThreshold &&
            horizontalDistSq <= maxHorizontalDist * maxHorizontalDist;
    }

    const auto now = std::chrono::steady_clock::now();
    if (raised)
        m_HudLiftGestureVisibleUntil = now + std::chrono::milliseconds(120);

    const bool active = raised || (m_HudLiftGestureVisibleUntil.time_since_epoch().count() != 0 &&
        now < m_HudLiftGestureVisibleUntil);

    if (!active)
    {
        stopLiftHudCapture(true);
        m_HudToggleState = false;
        return;
    }

    m_HudLiftGestureActive.store(1u, std::memory_order_release);
    m_HudToggleState = true;
}

bool VR::IsGameplayHudRequested() const
{
    return m_HudAlwaysVisible ||
        m_HudShowActionHeld.load(std::memory_order_acquire) != 0 ||
        m_HudLiftGestureActive.load(std::memory_order_acquire) != 0;
}

void VR::MarkQueuedHudFresh(uint64_t holdMs)
{
    const uint64_t nowMs = static_cast<uint64_t>(GetTickCount64());
    m_QueuedHudFreshUntilMs.store(nowMs + holdMs, std::memory_order_release);
}

void VR::ClearQueuedHudFresh()
{
    m_QueuedHudFreshUntilMs.store(0, std::memory_order_release);
    m_NativeDesktopHudPainted.store(false, std::memory_order_release);
}

bool VR::IsQueuedHudFresh() const
{
    const uint64_t freshUntilMs = m_QueuedHudFreshUntilMs.load(std::memory_order_acquire);
    if (freshUntilMs == 0)
        return false;

    const uint64_t nowMs = static_cast<uint64_t>(GetTickCount64());
    return nowMs < freshUntilMs;
}

#include "vr/vr_lifecycle_init.inl"
#include "vr/vr_lifecycle_update.inl"
#include "vr/vr_lifecycle_pose_hud.inl"
#include "vr/vr_process_input.inl"
#include "vr/vr_roomscale_prediction.inl"
#include "vr/vr_tracking.inl"
#include "vr/vr_aiming.inl"
#include "vr/vr_viewmodel_config.inl"

namespace
{
    constexpr float kKillSoundTraceDistance = 8192.0f;
    constexpr int kFeedbackSoundVoiceCount = 8;
    constexpr int kGameEventDebugIdInit = 42;

    struct FeedbackSoundVoiceState
    {
        std::string alias;
        std::string loadedPath;
        bool isOpen = false;
        bool usesWaveOut = false;
        uint32_t waveSampleRate = 0;
        uint16_t waveSourceChannels = 0;
        std::vector<int16_t> waveSourceSamples;
        std::vector<int16_t> waveStereoSamples;
        HWAVEOUT waveOut = nullptr;
        WAVEHDR waveHeader{};
        bool wavePrepared = false;
        std::chrono::steady_clock::time_point lastStarted{};
    };

    class VRKillSoundEventListener final : public IGameEventListener2
    {
    public:
        explicit VRKillSoundEventListener(VR* vr)
            : m_VR(vr)
        {
        }

        void FireGameEvent(IGameEvent* event) override
        {
            if (m_VR)
                m_VR->HandleKillSoundGameEvent(event);
        }

        int GetEventDebugID(void) override
        {
            return kGameEventDebugIdInit;
        }

    private:
        VR* m_VR = nullptr;
    };

    class VRDamageFeedbackEventListener final : public IGameEventListener2
    {
    public:
        explicit VRDamageFeedbackEventListener(VR* vr)
            : m_VR(vr)
        {
        }

        void FireGameEvent(IGameEvent* event) override
        {
            if (m_VR)
                m_VR->HandleDamageFeedbackGameEvent(event);
        }

        int GetEventDebugID(void) override
        {
            return kGameEventDebugIdInit;
        }

    private:
        VR* m_VR = nullptr;
    };

    class VRMeleeHitHapticsEventListener final : public IGameEventListener2
    {
    public:
        explicit VRMeleeHitHapticsEventListener(VR* vr)
            : m_VR(vr)
        {
        }

        void FireGameEvent(IGameEvent* event) override
        {
            if (m_VR)
                m_VR->HandleMeleeHitHapticsGameEvent(event);
        }

        int GetEventDebugID(void) override
        {
            return kGameEventDebugIdInit;
        }

    private:
        VR* m_VR = nullptr;
    };

    static std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    static bool IsMeleeHitWeaponName(const char* weaponName)
    {
        if (!weaponName || !*weaponName)
            return false;

        const std::string weapon = ToLowerCopy(weaponName);
        if (weapon == "melee" || weapon.find("weapon_melee") != std::string::npos || weapon.find("melee") != std::string::npos)
            return true;

        static constexpr const char* kMeleeWeaponNames[] = {
            "fireaxe",
            "katana",
            "electric_guitar",
            "baseball_bat",
            "cricket_bat",
            "knife",
            "golfclub",
            "crowbar",
            "machete",
            "tonfa",
            "frying_pan",
            "shovel",
            "pitchfork",
            "riotshield"
        };

        for (const char* name : kMeleeWeaponNames)
        {
            if (weapon.find(name) != std::string::npos)
                return true;
        }

        return false;
    }

    static bool IsActiveWeaponMeleeSafe(C_BasePlayer* player)
    {
        if (!player)
            return false;

#ifdef _MSC_VER
        __try
        {
            C_WeaponCSBase* activeWeapon = reinterpret_cast<C_WeaponCSBase*>(player->GetActiveWeapon());
            return activeWeapon && activeWeapon->GetWeaponID() == C_WeaponCSBase::WeaponID::MELEE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        C_WeaponCSBase* activeWeapon = reinterpret_cast<C_WeaponCSBase*>(player->GetActiveWeapon());
        return activeWeapon && activeWeapon->GetWeaponID() == C_WeaponCSBase::WeaponID::MELEE;
#endif
    }

    static std::string TrimCopy(std::string value)
    {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !isSpace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), value.end());
        return value;
    }

    static bool StartsWithInsensitive(const std::string& value, const char* prefix)
    {
        if (!prefix)
            return false;

        const size_t prefixLen = std::strlen(prefix);
        if (value.size() < prefixLen)
            return false;

        for (size_t i = 0; i < prefixLen; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }

        return true;
    }

    static bool EndsWithInsensitive(const std::string& value, const char* suffix)
    {
        if (!suffix)
            return false;

        const size_t suffixLen = std::strlen(suffix);
        if (value.size() < suffixLen)
            return false;

        const size_t start = value.size() - suffixLen;
        for (size_t i = 0; i < suffixLen; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(value[start + i])) != std::tolower(static_cast<unsigned char>(suffix[i])))
                return false;
        }

        return true;
    }

    static bool FileExistsPath(const std::string& path)
    {
        if (path.empty())
            return false;

        DWORD attrs = ::GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static bool DirectoryExistsPath(const std::string& path)
    {
        if (path.empty())
            return false;

        const DWORD attrs = ::GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    static bool TryGetFileLastWriteTime(const std::string& path, FILETIME& outLastWriteTime)
    {
        if (path.empty())
            return false;

        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs))
            return false;
        if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;

        outLastWriteTime = attrs.ftLastWriteTime;
        return true;
    }

    static bool IsAbsoluteWindowsPath(const std::string& path)
    {
        if (!path.empty() && (path[0] == '\\' || path[0] == '/'))
            return true;

        if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
            return true;

        return path.size() >= 2
            && ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'));
    }

    static std::string JoinWindowsPath(const std::string& base, const std::string& child)
    {
        if (base.empty())
            return child;
        if (child.empty())
            return base;

        const char tail = base.back();
        if (tail == '\\' || tail == '/')
            return base + child;

        return base + "\\" + child;
    }

    static std::string GetModuleDirectoryA()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD len = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return {};

        std::string path(modulePath, len);
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return {};

        return path.substr(0, slash);
    }

    static std::string ResolveVrPath(const std::string& rawPath)
    {
        const std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        if (IsAbsoluteWindowsPath(path))
            return FileExistsPath(path) ? path : std::string{};

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        if (StartsWithInsensitive(path, "VR\\") || StartsWithInsensitive(path, "VR/"))
        {
            const std::string fromModule = JoinWindowsPath(moduleDir, path);
            return FileExistsPath(fromModule) ? fromModule : std::string{};
        }

        const std::string fromVrDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), path);
        if (FileExistsPath(fromVrDir))
            return fromVrDir;

        const std::string fromModule = JoinWindowsPath(moduleDir, path);
        if (FileExistsPath(fromModule))
            return fromModule;

        return {};
    }

    static std::string ResolveVrDirectoryPath(const std::string& rawPath)
    {
        const std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        if (IsAbsoluteWindowsPath(path))
            return DirectoryExistsPath(path) ? path : std::string{};

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        if (StartsWithInsensitive(path, "VR\\") || StartsWithInsensitive(path, "VR/"))
        {
            const std::string fromModule = JoinWindowsPath(moduleDir, path);
            return DirectoryExistsPath(fromModule) ? fromModule : std::string{};
        }

        const std::string fromVrDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), path);
        if (DirectoryExistsPath(fromVrDir))
            return fromVrDir;

        const std::string fromModule = JoinWindowsPath(moduleDir, path);
        if (DirectoryExistsPath(fromModule))
            return fromModule;

        return {};
    }

    static bool EnsureDirectoryExistsRecursive(const std::string& rawPath)
    {
        std::string path = TrimCopy(rawPath);
        if (path.empty())
            return false;

        std::replace(path.begin(), path.end(), '/', '\\');
        if (!path.empty() && path.back() == '\\')
            path.pop_back();
        if (path.empty())
            return false;

        size_t start = 0;
        if (path.size() >= 2 && path[1] == ':')
            start = 3;
        else if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\')
        {
            start = path.find('\\', 2);
            if (start == std::string::npos)
                return true;
            start = path.find('\\', start + 1);
            if (start == std::string::npos)
                return true;
            ++start;
        }

        for (size_t pos = start; pos <= path.size(); ++pos)
        {
            if (pos < path.size() && path[pos] != '\\')
                continue;

            const std::string partial = path.substr(0, pos);
            if (partial.empty())
                continue;

            const DWORD attrs = ::GetFileAttributesA(partial.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES)
            {
                if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    return false;
                continue;
            }

            if (!::CreateDirectoryA(partial.c_str(), nullptr))
            {
                const DWORD error = ::GetLastError();
                if (error != ERROR_ALREADY_EXISTS)
                    return false;
            }
        }

        return true;
    }

    static std::string QuoteProcessArg(const std::string& value)
    {
        if (value.empty())
            return "\"\"";

        const bool needsQuotes = value.find_first_of(" \t\"") != std::string::npos;
        if (!needsQuotes)
            return value;

        std::string quoted;
        quoted.push_back('"');
        size_t backslashCount = 0;
        for (const char ch : value)
        {
            if (ch == '\\')
            {
                ++backslashCount;
                continue;
            }

            if (ch == '"')
            {
                quoted.append(backslashCount * 2 + 1, '\\');
                quoted.push_back('"');
                backslashCount = 0;
                continue;
            }

            if (backslashCount != 0)
            {
                quoted.append(backslashCount, '\\');
                backslashCount = 0;
            }

            quoted.push_back(ch);
        }

        if (backslashCount != 0)
            quoted.append(backslashCount * 2, '\\');
        quoted.push_back('"');
        return quoted;
    }

    static bool RunProcessHidden(const std::string& commandLine, DWORD& outExitCode)
    {
        outExitCode = static_cast<DWORD>(-1);
        if (commandLine.empty())
            return false;

        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo{};
        std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back('\0');

        const std::string workingDirectory = GetModuleDirectoryA();
        if (!::CreateProcessA(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startupInfo,
            &processInfo))
        {
            return false;
        }

        ::WaitForSingleObject(processInfo.hProcess, INFINITE);
        ::GetExitCodeProcess(processInfo.hProcess, &outExitCode);
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);
        return true;
    }

    static bool StartProcessHidden(const std::string& commandLine, const std::string& workingDirectory, HANDLE& outProcessHandle, DWORD& outProcessId)
    {
        outProcessHandle = nullptr;
        outProcessId = 0;
        if (commandLine.empty())
            return false;

        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo{};
        std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back('\0');

        const char* workingDirValue = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
        if (!::CreateProcessA(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDirValue,
            &startupInfo,
            &processInfo))
        {
            return false;
        }

        ::CloseHandle(processInfo.hThread);
        outProcessHandle = processInfo.hProcess;
        outProcessId = processInfo.dwProcessId;
        return true;
    }

    static bool IsProcessHandleRunning(HANDLE processHandle)
    {
        if (!processHandle)
            return false;

        const DWORD waitResult = ::WaitForSingleObject(processHandle, 0);
        return waitResult == WAIT_TIMEOUT;
    }

    static void CloseOwnedProcessHandle(HANDLE& processHandle, DWORD& processId)
    {
        if (processHandle)
        {
            ::CloseHandle(processHandle);
            processHandle = nullptr;
        }
        processId = 0;
    }

    static void CloseOwnedHandle(HANDLE& handle)
    {
        if (!handle)
            return;

        ::CloseHandle(handle);
        handle = nullptr;
    }

    static std::string StripUtf8Bom(std::string value)
    {
        if (value.size() >= 3
            && static_cast<unsigned char>(value[0]) == 0xEF
            && static_cast<unsigned char>(value[1]) == 0xBB
            && static_cast<unsigned char>(value[2]) == 0xBF)
        {
            value.erase(0, 3);
        }
        return value;
    }

    static std::string CollapseWhitespace(std::string value)
    {
        value = StripUtf8Bom(value);

        std::string collapsed;
        collapsed.reserve(value.size());
        bool prevSpace = false;
        for (const unsigned char ch : value)
        {
            if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ')
            {
                if (!collapsed.empty() && !prevSpace)
                {
                    collapsed.push_back(' ');
                    prevSpace = true;
                }
                continue;
            }

            if (ch < 0x20)
                continue;

            collapsed.push_back(static_cast<char>(ch));
            prevSpace = false;
        }

        return TrimCopy(collapsed);
    }

    static std::vector<std::string> SplitLiteralDelimitedList(const std::string& rawValue, const std::string& rawDelimiter)
    {
        std::vector<std::string> parts;
        const std::string value = TrimCopy(rawValue);
        if (value.empty())
            return parts;

        const std::string delimiter = rawDelimiter.empty() ? std::string("__VR_REGEX_SPLIT__") : rawDelimiter;
        if (delimiter.empty())
        {
            parts.push_back(value);
            return parts;
        }

        size_t start = 0;
        for (;;)
        {
            const size_t pos = value.find(delimiter, start);
            const std::string part = TrimCopy(value.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
            if (!part.empty())
                parts.push_back(part);

            if (pos == std::string::npos)
                break;
            start = pos + delimiter.size();
        }

        return parts;
    }

    static bool StripLeadingClosedTag(std::string& value, const std::string& openToken, const std::string& closeToken, size_t maxBytes)
    {
        if (openToken.empty() || closeToken.empty() || value.rfind(openToken, 0) != 0)
            return false;

        const size_t closePos = value.find(closeToken, openToken.size());
        if (closePos == std::string::npos || closePos > maxBytes)
            return false;

        std::string rest = TrimCopy(value.substr(closePos + closeToken.size()));
        if (rest.empty())
            return false;

        value = rest;
        return true;
    }

    static std::string NormalizeHudChatSpeakerForPlayerLookup(std::string value)
    {
        value = CollapseWhitespace(value);
        for (int pass = 0; pass < 8 && !value.empty(); ++pass)
        {
            const std::string before = value;

            if (StripLeadingClosedTag(value, "(", ")", 64)
                || StripLeadingClosedTag(value, "\xEF\xBC\x88", "\xEF\xBC\x89", 96)
                || StripLeadingClosedTag(value, "\xE3\x80\x90", "\xE3\x80\x91", 96))
            {
                value = CollapseWhitespace(value);
                continue;
            }

            if (value.size() > 2 && value[0] == '*')
            {
                const size_t closePos = value.find('*', 1);
                if (closePos != std::string::npos && closePos <= 32)
                {
                    std::string rest = TrimCopy(value.substr(closePos + 1));
                    if (!rest.empty())
                    {
                        value = CollapseWhitespace(rest);
                        continue;
                    }
                }
            }

            if (value.size() > 4 && value[0] == '[' && StartsWithInsensitive(value.substr(1), "lv"))
            {
                const size_t closePos = value.find(']', 1);
                if (closePos != std::string::npos && closePos <= 16)
                {
                    std::string rest = TrimCopy(value.substr(closePos + 1));
                    if (!rest.empty())
                    {
                        value = CollapseWhitespace(rest);
                        continue;
                    }
                }
            }

            if (value == before)
                break;
        }

        return value;
    }

    static bool TryGetPlayerTeamByNormalizedName(Game* game, const std::string& normalizedName, int& outTeam)
    {
        outTeam = 0;
        const std::string lookupName = NormalizeHudChatSpeakerForPlayerLookup(normalizedName);
        if (!game || !game->m_EngineClient || lookupName.empty())
            return false;

        for (int playerIndex = 1; playerIndex <= 64; ++playerIndex)
        {
            char playerName[128] = {};
            if (!GetPlayerNameUtf8Safe(game->m_EngineClient, playerIndex, playerName, sizeof(playerName)))
                continue;

            const std::string candidateName = CollapseWhitespace(playerName);
            const std::string candidateLookupName = NormalizeHudChatSpeakerForPlayerLookup(candidateName);
            if (candidateName.empty()
                || (_stricmp(candidateName.c_str(), lookupName.c_str()) != 0
                    && (candidateLookupName.empty() || _stricmp(candidateLookupName.c_str(), lookupName.c_str()) != 0)))
                continue;

            C_BasePlayer* player = reinterpret_cast<C_BasePlayer*>(game->GetClientEntity(playerIndex));
            if (!player)
                continue;

            int team = 0;
            if (!VR_TryReadI32(reinterpret_cast<const unsigned char*>(player), 0xE4, team))
                continue;

            outTeam = team;
            return true;
        }

        return false;
    }

    static bool TryResolveTextToSpeechWhitelistMatch(
        const std::string& rawText,
        const std::string& rawRegexList,
        const std::string& rawSeparator,
        std::string& outMatchedText)
    {
        outMatchedText.clear();
        if (rawText.empty())
            return false;

        const std::vector<std::string> regexList = SplitLiteralDelimitedList(rawRegexList, rawSeparator);
        if (regexList.empty())
            return false;

        static std::unordered_map<std::string, bool> s_loggedInvalidRegexes;

        auto normalizePatternForStdRegex = [](const std::string& rawPattern)
            {
                std::string normalized;
                normalized.reserve(rawPattern.size());

                bool escaped = false;
                bool inCharClass = false;
                for (size_t index = 0; index < rawPattern.size(); ++index)
                {
                    const char ch = rawPattern[index];
                    if (escaped)
                    {
                        normalized.push_back(ch);
                        escaped = false;
                        continue;
                    }

                    if (ch == '\\')
                    {
                        normalized.push_back(ch);
                        escaped = true;
                        continue;
                    }

                    if (ch == '[' && !inCharClass)
                    {
                        inCharClass = true;
                        normalized.push_back(ch);
                        continue;
                    }

                    if (ch == ']' && inCharClass)
                    {
                        inCharClass = false;
                        normalized.push_back(ch);
                        continue;
                    }

                    if (!inCharClass
                        && ch == '('
                        && index + 2 < rawPattern.size()
                        && rawPattern[index + 1] == '?'
                        && rawPattern[index + 2] == ':')
                    {
                        normalized.push_back('(');
                        index += 2;
                        continue;
                    }

                    normalized.push_back(ch);
                }

                return normalized;
            };

        for (ptrdiff_t i = static_cast<ptrdiff_t>(regexList.size()) - 1; i >= 0; --i)
        {
            const std::string& pattern = regexList[static_cast<size_t>(i)];
            try
            {
                const std::string normalizedPattern = normalizePatternForStdRegex(pattern);
                const std::regex regex(normalizedPattern, std::regex_constants::ECMAScript);
                std::smatch match;
                if (!std::regex_search(rawText, match, regex) || match.empty())
                    continue;

                std::string matchedText;
                for (ptrdiff_t groupIndex = static_cast<ptrdiff_t>(match.size()) - 1; groupIndex >= 1; --groupIndex)
                {
                    if (!match[static_cast<size_t>(groupIndex)].matched)
                        continue;

                    matchedText = CollapseWhitespace(match.str(static_cast<size_t>(groupIndex)));
                    if (!matchedText.empty())
                        break;
                }

                if (matchedText.empty())
                    matchedText = CollapseWhitespace(match.str(0));
                if (matchedText.empty())
                    continue;

                outMatchedText = matchedText;
                return true;
            }
            catch (const std::regex_error&)
            {
                if (!s_loggedInvalidRegexes[pattern])
                {
                    s_loggedInvalidRegexes[pattern] = true;
                    Game::logMsg("[Speech][TTS] invalid whitelist regex: %s", pattern.c_str());
                }
            }
        }

        return false;
    }

    static std::string SanitizeTextForSourceSayCommand(const std::string& rawText)
    {
        std::string text = CollapseWhitespace(rawText);
        std::string sanitized;
        sanitized.reserve(text.size());
        for (const unsigned char ch : text)
        {
            if (ch == '"' || ch == ';' || ch == '\r' || ch == '\n')
                continue;
            sanitized.push_back(static_cast<char>(ch));
        }

        sanitized = TrimCopy(sanitized);
        constexpr size_t kMaxChatChars = 220;
        if (sanitized.size() > kMaxChatChars)
            sanitized.resize(kMaxChatChars);
        return sanitized;
    }

    static bool WriteMonoPcm16WaveFile(const std::string& path, const std::vector<int16_t>& samples, uint32_t sampleRate)
    {
        if (path.empty() || samples.empty() || sampleRate == 0)
            return false;

        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
        {
            const std::string directory = path.substr(0, slash);
            if (!directory.empty() && !EnsureDirectoryExistsRecursive(directory))
                return false;
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;

        const uint16_t channels = 1;
        const uint16_t bitsPerSample = 16;
        const uint32_t bytesPerSecond = sampleRate * channels * (bitsPerSample / 8);
        const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
        const uint32_t dataSize = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
        const uint32_t riffSize = 36u + dataSize;

        file.write("RIFF", 4);
        file.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
        file.write("WAVE", 4);
        file.write("fmt ", 4);

        const uint32_t fmtSize = 16;
        const uint16_t formatTag = 1;
        file.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
        file.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
        file.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
        file.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
        file.write(reinterpret_cast<const char*>(&bytesPerSecond), sizeof(bytesPerSecond));
        file.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
        file.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
        file.write("data", 4);
        file.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
        file.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataSize));
        return file.good();
    }

    static std::string BuildSpeechCachePath(const char* stem, const char* extension)
    {
        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        const std::string cacheDir = JoinWindowsPath(JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), "speech"), "cache");
        if (!EnsureDirectoryExistsRecursive(cacheDir))
            return {};

        static std::atomic<uint64_t> counter{ 0 };
        const uint64_t serial = ++counter;
        const auto now = std::chrono::system_clock::now().time_since_epoch().count();

        std::ostringstream fileName;
        fileName << (stem ? stem : "speech")
            << "_"
            << now
            << "_"
            << ::GetCurrentProcessId()
            << "_"
            << ::GetCurrentThreadId()
            << "_"
            << serial
            << (extension ? extension : "");
        return JoinWindowsPath(cacheDir, fileName.str());
    }

    static std::string BuildSpeechCacheFixedPath(const char* fileName)
    {
        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty() || !fileName || !*fileName)
            return {};

        const std::string cacheDir = JoinWindowsPath(JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), "speech"), "cache");
        if (!EnsureDirectoryExistsRecursive(cacheDir))
            return {};

        return JoinWindowsPath(cacheDir, fileName);
    }

    static std::string BuildProcessPrefix(const std::string& rawPrefix)
    {
        const std::string prefix = TrimCopy(rawPrefix);
        if (prefix.empty())
            return {};

        const std::string resolved = ResolveVrPath(prefix);
        if (!resolved.empty())
            return QuoteProcessArg(resolved);

        return prefix;
    }

    static std::string GetDefaultTextToSpeechCommandPrefix()
    {
        return "python api_v2.py";
    }

    static std::string GetEffectiveTextToSpeechCommandPrefixSpec(const std::string& rawPrefix)
    {
        const std::string trimmed = TrimCopy(rawPrefix);
        if (trimmed.empty())
            return GetDefaultTextToSpeechCommandPrefix();
        return trimmed;
    }

    static std::string NormalizeSlashes(std::string value, char slash);
    static std::string EscapeJsonString(const std::string& value);
    static std::string TrimHttpResponseForLog(const std::vector<char>& responseBody);
    static bool HttpRequest(
        const std::string& host,
        INTERNET_PORT port,
        const wchar_t* method,
        const wchar_t* path,
        const std::string* requestBody,
        const wchar_t* extraHeaders,
        DWORD timeoutMs,
        DWORD& outStatusCode,
        std::vector<char>& outResponseBody);

    static std::string BuildTextToSpeechServerLaunchSignature(const VR::TextToSpeechRuntimeConfig& config)
    {
        std::ostringstream signature;
        signature
            << config.resolvedPrefix
            << "|"
            << config.resolvedWorkingDir
            << "|"
            << config.serverPort;

        if (config.hotSwitchProfileValid)
        {
            signature
                << "|"
                << ToLowerCopy(config.resolvedDevice)
                << "|"
                << (config.resolvedIsHalf ? "1" : "0")
                << "|"
                << config.resolvedBertBasePath
                << "|"
                << config.resolvedCnHubertBasePath;
        }
        else
        {
            signature
                << "|"
                << config.resolvedConfigPath;
        }

        return signature.str();
    }

    static std::string BuildTextToSpeechServerModelSignature(const VR::TextToSpeechRuntimeConfig& config)
    {
        if (!config.hotSwitchProfileValid)
            return config.resolvedConfigPath;

        std::ostringstream signature;
        signature
            << ToLowerCopy(config.resolvedModelVersion)
            << "|"
            << config.resolvedT2SWeightsPath
            << "|"
            << config.resolvedVitsWeightsPath;
        return signature.str();
    }

    static bool IsWindowsAbsolutePath(const std::string& path)
    {
        return path.size() >= 2
            && ((std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
                || (path.size() >= 2 && path[0] == '\\' && path[1] == '\\')
                || path[0] == '/');
    }

    static int HexDigitValue(char ch)
    {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F')
            return 10 + (ch - 'A');
        return -1;
    }

    static void AppendUtf8Codepoint(std::string& out, unsigned int codepoint)
    {
        if (codepoint <= 0x7F)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    static std::string DecodeBasicYamlEscapes(const std::string& value)
    {
        std::string decoded;
        decoded.reserve(value.size());

        for (size_t i = 0; i < value.size(); ++i)
        {
            const char ch = value[i];
            if (ch != '\\' || i + 1 >= value.size())
            {
                decoded.push_back(ch);
                continue;
            }

            const char next = value[++i];
            switch (next)
            {
            case '\\': decoded.push_back('\\'); break;
            case '"': decoded.push_back('"'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case 'u':
            {
                if (i + 4 >= value.size())
                {
                    decoded.append("\\u");
                    break;
                }

                unsigned int codepoint = 0;
                bool valid = true;
                for (size_t digit = 0; digit < 4; ++digit)
                {
                    const int hexValue = HexDigitValue(value[i + 1 + digit]);
                    if (hexValue < 0)
                    {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | static_cast<unsigned int>(hexValue);
                }

                if (!valid)
                {
                    decoded.append("\\u");
                    break;
                }

                AppendUtf8Codepoint(decoded, codepoint);
                i += 4;
                break;
            }
            default:
                decoded.push_back(next);
                break;
            }
        }

        return decoded;
    }

    static std::string ParseYamlScalarValue(std::string value)
    {
        value = TrimCopy(value);
        if (value.empty())
            return {};

        const bool isDoubleQuoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
        const bool isSingleQuoted = value.size() >= 2 && value.front() == '\'' && value.back() == '\'';
        if (isDoubleQuoted || isSingleQuoted)
        {
            value = value.substr(1, value.size() - 2);
            if (isDoubleQuoted)
                value = DecodeBasicYamlEscapes(value);
            return value;
        }

        const size_t commentPos = value.find(" #");
        if (commentPos != std::string::npos)
            value = TrimCopy(value.substr(0, commentPos));
        return value;
    }

    static bool TryParseYamlStringValue(const std::string& line, const char* key, std::string& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#')
            return false;
        if (!StartsWithInsensitive(trimmed, key))
            return false;

        const size_t keyLen = std::strlen(key);
        if (trimmed.size() <= keyLen || trimmed[keyLen] != ':')
            return false;

        outValue = ParseYamlScalarValue(trimmed.substr(keyLen + 1));
        return !outValue.empty();
    }

    static bool TryParseYamlBoolValue(const std::string& line, const char* key, bool& outValue)
    {
        std::string parsed;
        if (!TryParseYamlStringValue(line, key, parsed))
            return false;

        const std::string lowered = ToLowerCopy(parsed);
        if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on")
        {
            outValue = true;
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off")
        {
            outValue = false;
            return true;
        }
        return false;
    }

    static std::string ResolveTextToSpeechModelProfilePath(const std::string& rawPath, const std::string& workingDir)
    {
        const std::string trimmed = TrimCopy(rawPath);
        if (trimmed.empty())
            return {};

        if (IsWindowsAbsolutePath(trimmed))
            return NormalizeSlashes(trimmed, '\\');

        if (workingDir.empty())
            return NormalizeSlashes(trimmed, '\\');

        return JoinWindowsPath(workingDir, NormalizeSlashes(trimmed, '\\'));
    }

    static bool TryLoadTextToSpeechHotSwitchProfile(VR::TextToSpeechRuntimeConfig& config)
    {
        config.hotSwitchProfileValid = false;
        config.resolvedDevice.clear();
        config.resolvedBertBasePath.clear();
        config.resolvedCnHubertBasePath.clear();
        config.resolvedT2SWeightsPath.clear();
        config.resolvedVitsWeightsPath.clear();
        config.resolvedModelVersion.clear();
        config.resolvedIsHalf = false;

        if (config.resolvedConfigPath.empty() || config.resolvedWorkingDir.empty())
            return false;

        std::ifstream file(config.resolvedConfigPath);
        if (!file.is_open())
            return false;

        bool inCustomSection = false;
        std::string device;
        bool isHalf = false;
        bool hasIsHalf = false;
        std::string bertBasePath;
        std::string cnhuhbertBasePath;
        std::string t2sWeightsPath;
        std::string vitsWeightsPath;
        std::string version;

        std::string line;
        while (std::getline(file, line))
        {
            const std::string trimmed = TrimCopy(line);
            if (trimmed.empty() || trimmed[0] == '#')
                continue;

            const bool topLevel = !line.empty() && !std::isspace(static_cast<unsigned char>(line.front()));
            if (topLevel)
            {
                if (StartsWithInsensitive(trimmed, "custom:"))
                {
                    inCustomSection = true;
                    continue;
                }

                if (inCustomSection)
                    break;

                continue;
            }

            if (!inCustomSection)
                continue;

            std::string parsed;
            if (TryParseYamlStringValue(trimmed, "device", parsed))
                device = parsed;
            else if (TryParseYamlBoolValue(trimmed, "is_half", isHalf))
                hasIsHalf = true;
            else if (TryParseYamlStringValue(trimmed, "bert_base_path", parsed))
                bertBasePath = parsed;
            else if (TryParseYamlStringValue(trimmed, "cnhuhbert_base_path", parsed))
                cnhuhbertBasePath = parsed;
            else if (TryParseYamlStringValue(trimmed, "t2s_weights_path", parsed))
                t2sWeightsPath = parsed;
            else if (TryParseYamlStringValue(trimmed, "vits_weights_path", parsed))
                vitsWeightsPath = parsed;
            else if (TryParseYamlStringValue(trimmed, "version", parsed))
                version = parsed;
        }

        if (device.empty() || !hasIsHalf || bertBasePath.empty() || cnhuhbertBasePath.empty()
            || t2sWeightsPath.empty() || vitsWeightsPath.empty() || version.empty())
        {
            return false;
        }

        config.resolvedDevice = device;
        config.resolvedIsHalf = isHalf;
        config.resolvedBertBasePath = ResolveTextToSpeechModelProfilePath(bertBasePath, config.resolvedWorkingDir);
        config.resolvedCnHubertBasePath = ResolveTextToSpeechModelProfilePath(cnhuhbertBasePath, config.resolvedWorkingDir);
        config.resolvedT2SWeightsPath = ResolveTextToSpeechModelProfilePath(t2sWeightsPath, config.resolvedWorkingDir);
        config.resolvedVitsWeightsPath = ResolveTextToSpeechModelProfilePath(vitsWeightsPath, config.resolvedWorkingDir);
        config.resolvedModelVersion = version;

        config.hotSwitchProfileValid = !config.resolvedBertBasePath.empty()
            && !config.resolvedCnHubertBasePath.empty()
            && !config.resolvedT2SWeightsPath.empty()
            && !config.resolvedVitsWeightsPath.empty()
            && !config.resolvedModelVersion.empty();
        return config.hotSwitchProfileValid;
    }

    static bool TryParseConfigFloatValue(const std::string& line, const char* key, float& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        if (trimmed.empty())
            return false;
        if (trimmed[0] == '/' || trimmed[0] == '#' || StartsWithInsensitive(trimmed, "//"))
            return false;
        if (!StartsWithInsensitive(trimmed, key))
            return false;

        const size_t keyLen = std::strlen(key);
        if (trimmed.size() > keyLen)
        {
            const char next = trimmed[keyLen];
            if (!std::isspace(static_cast<unsigned char>(next)) && next != '=')
                return false;
        }

        std::string value = TrimCopy(trimmed.substr(keyLen));
        if (!value.empty() && value.front() == '=')
            value = TrimCopy(value.substr(1));
        if (value.empty())
            return false;

        if (value.front() == '"')
        {
            const size_t closingQuote = value.find('"', 1);
            if (closingQuote != std::string::npos)
                value = value.substr(1, closingQuote - 1);
            else
                value.erase(value.begin());
        }

        char* endPtr = nullptr;
        const float parsed = std::strtof(value.c_str(), &endPtr);
        if (!endPtr || endPtr == value.c_str())
            return false;

        outValue = parsed;
        return true;
    }

    static int GetLocalPlayerUserId(Game* game)
    {
        if (!game || !game->m_EngineClient)
            return 0;

        const int localPlayerIndex = game->m_EngineClient->GetLocalPlayer();
        if (localPlayerIndex <= 0)
            return 0;

        player_info_t playerInfo{};
        if (!game->m_EngineClient->GetPlayerInfo(localPlayerIndex, &playerInfo))
            return 0;

        auto isValidUserId = [&](int candidateUserId)
            {
                return candidateUserId > 0
                    && candidateUserId <= 0x7FFF
                    && game->m_EngineClient->GetPlayerForUserID(candidateUserId) == localPlayerIndex;
            };

        auto readUserIdAtOffset = [&](size_t offset)
            {
                if (offset + sizeof(int) > sizeof(player_info_t))
                    return 0;

                int candidateUserId = 0;
                std::memcpy(&candidateUserId, reinterpret_cast<const unsigned char*>(&playerInfo) + offset, sizeof(candidateUserId));
                return isValidUserId(candidateUserId) ? candidateUserId : 0;
            };

        static size_t s_cachedUserIdOffset = SIZE_MAX;

        if (s_cachedUserIdOffset != SIZE_MAX)
        {
            const int cachedUserId = readUserIdAtOffset(s_cachedUserIdOffset);
            if (cachedUserId > 0)
                return cachedUserId;

            s_cachedUserIdOffset = SIZE_MAX;
        }

        const size_t declaredOffset = offsetof(player_info_t, userID);
        const int declaredUserId = readUserIdAtOffset(declaredOffset);
        if (declaredUserId > 0)
        {
            s_cachedUserIdOffset = declaredOffset;
            return declaredUserId;
        }

        const size_t scanLimit = (std::min)(sizeof(player_info_t), static_cast<size_t>(256));
        for (size_t offset = 0; offset + sizeof(int) <= scanLimit; offset += sizeof(int))
        {
            if (offset == declaredOffset)
                continue;

            const int candidateUserId = readUserIdAtOffset(offset);
            if (candidateUserId <= 0)
                continue;

            s_cachedUserIdOffset = offset;
            return candidateUserId;
        }

        return 0;
    }

    static std::uintptr_t ResolveKillEventEntityTag(Game* game, IGameEvent* event, const std::string& eventName)
    {
        if (!game || !event)
            return 0;

        int entityIndex = 0;
        if (eventName == "infected_death")
        {
            entityIndex = event->GetInt("infected_id", 0);
        }
        else if (eventName == "infected_hurt")
        {
            entityIndex = event->GetInt("entityid", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entindex", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("infected_id", 0);
        }
        else if (eventName == "witch_killed")
        {
            entityIndex = event->GetInt("witchid", 0);
        }
        else if (eventName == "witch_hurt")
        {
            entityIndex = event->GetInt("witchid", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entityid", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entindex", 0);
        }
        else if (eventName == "player_death")
        {
            const int victimUserId = event->GetInt("userid", 0);
            if (victimUserId > 0 && game->m_EngineClient)
                entityIndex = game->m_EngineClient->GetPlayerForUserID(victimUserId);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entityid", 0);
        }
        else if (eventName == "player_hurt")
        {
            const int victimUserId = event->GetInt("userid", 0);
            if (victimUserId > 0 && game->m_EngineClient)
                entityIndex = game->m_EngineClient->GetPlayerForUserID(victimUserId);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entityid", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entindex", 0);
        }

        if (entityIndex <= 0)
            return 0;

        return reinterpret_cast<std::uintptr_t>(game->GetClientEntity(entityIndex));
    }
    static float ReadGameMasterVolumeFromConfig(IEngineClient* engine)
    {
        struct CachedGameVolumeState
        {
            bool wasInGame = false;
            float cachedVolume = 1.0f;
            bool initialized = false;
            std::string cachedConfigPath;
            FILETIME cachedWriteTime{};
            std::chrono::steady_clock::time_point lastRefreshAt{};
        };

        static CachedGameVolumeState state{};
        constexpr float kVolumeConfigRefreshSeconds = 10.0f;

        const bool inGame = engine && engine->IsInGame();
        if (!inGame)
        {
            state = {};
            return 1.0f;
        }

        const auto now = std::chrono::steady_clock::now();
        if (state.initialized && state.wasInGame && state.lastRefreshAt.time_since_epoch().count() != 0)
        {
            const float elapsed = std::chrono::duration<float>(now - state.lastRefreshAt).count();
            if (elapsed < kVolumeConfigRefreshSeconds)
                return state.cachedVolume;
        }

        auto updateCachedVolume = [&](float volume, const std::string& configPath, const FILETIME* writeTime)
            {
                state.wasInGame = true;
                state.cachedVolume = std::clamp(volume, 0.0f, 1.0f);
                state.initialized = true;
                state.cachedConfigPath = configPath;
                state.cachedWriteTime = writeTime ? *writeTime : FILETIME{};
                state.lastRefreshAt = now;
                return state.cachedVolume;
            };

        if (state.initialized && state.wasInGame && !state.cachedConfigPath.empty())
        {
            FILETIME writeTime{};
            if (TryGetFileLastWriteTime(state.cachedConfigPath, writeTime)
                && ::CompareFileTime(&writeTime, &state.cachedWriteTime) == 0)
            {
                state.lastRefreshAt = now;
                return state.cachedVolume;
            }
        }

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return updateCachedVolume(1.0f, std::string{}, nullptr);

        const std::array<std::string, 2> candidates =
        {
            JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "cfg\\config.cfg"),
            JoinWindowsPath(moduleDir, "config.cfg")
        };

        for (const auto& candidate : candidates)
        {
            FILETIME writeTime{};
            if (!TryGetFileLastWriteTime(candidate, writeTime))
                continue;

            std::ifstream file(candidate);
            if (!file.is_open())
                continue;

            float parsedVolume = 1.0f;
            std::string line;
            while (std::getline(file, line))
            {
                float value = 0.0f;
                if (TryParseConfigFloatValue(line, "volume", value))
                    parsedVolume = value;
            }

            return updateCachedVolume(parsedVolume, candidate, &writeTime);
        }

        return updateCachedVolume(1.0f, std::string{}, nullptr);
    }

    static std::string ResolveKillSoundFilePath(const std::string& rawPath)
    {
        const std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        if (IsAbsoluteWindowsPath(path))
            return FileExistsPath(path) ? path : std::string{};

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        if (StartsWithInsensitive(path, "VR\\") || StartsWithInsensitive(path, "VR/"))
        {
            const std::string fromModule = JoinWindowsPath(moduleDir, path);
            return FileExistsPath(fromModule) ? fromModule : std::string{};
        }

        const std::string fromVrDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), path);
        if (FileExistsPath(fromVrDir))
            return fromVrDir;

        return {};
    }

    static std::string ResolveGameSoundFilePath(const std::string& rawPath)
    {
        std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        while (!path.empty() && (path.front() == '\\' || path.front() == '/'))
            path.erase(path.begin());

        if (StartsWithInsensitive(path, "sound\\") || StartsWithInsensitive(path, "sound/"))
            path = TrimCopy(path.substr(6));

        if (path.empty())
            return {};

        if (IsAbsoluteWindowsPath(path))
            return FileExistsPath(path) ? path : std::string{};

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        const std::string candidate = JoinWindowsPath(JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "sound"), path);
        return FileExistsPath(candidate) ? candidate : std::string{};
    }

    static std::string ResolveBuiltinFeedbackGameSoundPath(const std::string& rawSoundName)
    {
        const std::string soundName = TrimCopy(rawSoundName);
        if (soundName.empty())
            return {};

        if (_stricmp(soundName.c_str(), "VR_HitMarker") == 0)
            return ResolveGameSoundFilePath("vrmod/hit.mp3");
        if (_stricmp(soundName.c_str(), "VR_KillMarker") == 0)
            return ResolveGameSoundFilePath("vrmod/kill.mp3");
        if (_stricmp(soundName.c_str(), "VR_HeadshotMarker") == 0)
            return ResolveGameSoundFilePath("vrmod/headshot.mp3");

        return {};
    }

    static bool LooksLikeAudioFilePath(const std::string& value)
    {
        return value.find('\\') != std::string::npos
            || value.find('/') != std::string::npos
            || value.find(':') != std::string::npos
            || EndsWithInsensitive(value, ".wav")
            || EndsWithInsensitive(value, ".mp3")
            || EndsWithInsensitive(value, ".ogg")
            || EndsWithInsensitive(value, ".m4a")
            || EndsWithInsensitive(value, ".aac")
            || EndsWithInsensitive(value, ".wma")
            || EndsWithInsensitive(value, ".flac");
    }

    static std::string FormatFeedbackSoundVolume(float value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2) << std::clamp(value, 0.0f, 2.0f);
        return stream.str();
    }

    static std::string ReadWholeTextFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};

        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    static bool WriteWholeTextFileIfChanged(const std::string& path, const std::string& contents)
    {
        if (path.empty())
            return false;

        if (ReadWholeTextFile(path) == contents)
            return true;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;

        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return file.good();
    }

    static bool WriteBinaryFile(const std::string& path, const std::vector<char>& contents)
    {
        if (path.empty())
            return false;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;

        if (!contents.empty())
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return file.good();
    }

    static std::wstring WideFromUtf8OrAnsi(const std::string& value)
    {
        if (value.empty())
            return {};

        auto decode = [&](UINT codePage, DWORD flags) -> std::wstring
            {
                const int len = ::MultiByteToWideChar(codePage, flags, value.c_str(), -1, nullptr, 0);
                if (len <= 1)
                    return {};

                std::wstring out(static_cast<size_t>(len), L'\0');
                ::MultiByteToWideChar(codePage, flags, value.c_str(), -1, out.data(), len);
                out.pop_back();
                return out;
            };

        std::wstring wide = decode(CP_UTF8, MB_ERR_INVALID_CHARS);
        if (!wide.empty())
            return wide;

        wide = decode(CP_UTF8, 0);
        if (!wide.empty())
            return wide;

        return decode(CP_ACP, 0);
    }

    static std::string EscapeJsonString(const std::string& value)
    {
        std::ostringstream stream;
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
            case '\\': stream << "\\\\"; break;
            case '"': stream << "\\\""; break;
            case '\b': stream << "\\b"; break;
            case '\f': stream << "\\f"; break;
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            default:
                if (ch < 0x20)
                {
                    stream << "\\u"
                        << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                }
                else
                {
                    stream << static_cast<char>(ch);
                }
                break;
            }
        }
        return stream.str();
    }

    static std::string TrimHttpResponseForLog(const std::vector<char>& responseBody)
    {
        if (responseBody.empty())
            return {};

        const size_t maxBytes = (std::min)(responseBody.size(), static_cast<size_t>(400));
        return CollapseWhitespace(std::string(responseBody.data(), maxBytes));
    }

    static bool HttpRequest(
        const std::string& host,
        INTERNET_PORT port,
        const wchar_t* method,
        const wchar_t* path,
        const std::string* requestBody,
        const wchar_t* extraHeaders,
        DWORD timeoutMs,
        DWORD& outStatusCode,
        std::vector<char>& outResponseBody)
    {
        outStatusCode = 0;
        outResponseBody.clear();

        const std::wstring wideHost = WideFromUtf8OrAnsi(host);
        if (wideHost.empty() || !method || !path)
            return false;

        HINTERNET session = ::WinHttpOpen(L"L4D2VR/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return false;

        bool success = false;
        HINTERNET connect = nullptr;
        HINTERNET request = nullptr;

        do
        {
            ::WinHttpSetTimeouts(session, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));

            connect = ::WinHttpConnect(session, wideHost.c_str(), port, 0);
            if (!connect)
                break;

            request = ::WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (!request)
                break;

            const void* bodyData = requestBody && !requestBody->empty() ? requestBody->data() : WINHTTP_NO_REQUEST_DATA;
            const DWORD bodySize = requestBody ? static_cast<DWORD>(requestBody->size()) : 0;
            const DWORD totalSize = requestBody ? bodySize : 0;

            if (!::WinHttpSendRequest(request, extraHeaders, extraHeaders ? static_cast<DWORD>(-1L) : 0, const_cast<void*>(bodyData), bodySize, totalSize, 0))
                break;
            if (!::WinHttpReceiveResponse(request, nullptr))
                break;

            DWORD statusCodeSize = sizeof(outStatusCode);
            if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &outStatusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
                break;

            for (;;)
            {
                DWORD available = 0;
                if (!::WinHttpQueryDataAvailable(request, &available))
                    break;
                if (available == 0)
                {
                    success = true;
                    break;
                }

                const size_t oldSize = outResponseBody.size();
                outResponseBody.resize(oldSize + available);
                DWORD downloaded = 0;
                if (!::WinHttpReadData(request, outResponseBody.data() + oldSize, available, &downloaded))
                    break;
                outResponseBody.resize(oldSize + downloaded);
            }
        } while (false);

        if (request)
            ::WinHttpCloseHandle(request);
        if (connect)
            ::WinHttpCloseHandle(connect);
        if (session)
            ::WinHttpCloseHandle(session);
        return success;
    }

    static bool IsLocalHttpDocsReady(INTERNET_PORT port, DWORD timeoutMs)
    {
        DWORD statusCode = 0;
        std::vector<char> responseBody;
        return HttpRequest("127.0.0.1", port, L"GET", L"/docs", nullptr, nullptr, timeoutMs, statusCode, responseBody)
            && statusCode == 200;
    }

    static void TryStopExistingTextToSpeechHttpServer(INTERNET_PORT port)
    {
        DWORD statusCode = 0;
        std::vector<char> responseBody;
        HttpRequest("127.0.0.1", port, L"GET", L"/control?command=exit", nullptr, nullptr, 1500, statusCode, responseBody);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!IsLocalHttpDocsReady(port, 500))
                return;

            ::Sleep(150);
        }
    }

    static std::string UrlEncodeHttpQueryValue(const std::string& value)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";

        std::string encoded;
        encoded.reserve(value.size() * 3);
        for (unsigned char ch : value)
        {
            const bool isUnreserved =
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~';

            if (isUnreserved)
            {
                encoded.push_back(static_cast<char>(ch));
            }
            else
            {
                encoded.push_back('%');
                encoded.push_back(kHex[(ch >> 4) & 0x0F]);
                encoded.push_back(kHex[ch & 0x0F]);
            }
        }
        return encoded;
    }

    static bool HttpGetUtf8Path(
        const std::string& host,
        INTERNET_PORT port,
        const std::string& utf8Path,
        DWORD timeoutMs,
        DWORD& outStatusCode,
        std::vector<char>& outResponseBody)
    {
        const std::wstring widePath = WideFromUtf8OrAnsi(utf8Path);
        if (widePath.empty())
            return false;

        return HttpRequest(host, port, L"GET", widePath.c_str(), nullptr, nullptr, timeoutMs, outStatusCode, outResponseBody);
    }

    static bool TryHotSwitchTextToSpeechServerModel(INTERNET_PORT port, const VR::TextToSpeechRuntimeConfig& config)
    {
        if (!config.hotSwitchProfileValid)
            return false;

        auto callSwitchEndpoint = [&](const char* endpointName, const std::string& requestPath) -> bool
            {
                DWORD statusCode = 0;
                std::vector<char> responseBody;
                if (!HttpGetUtf8Path("127.0.0.1", port, requestPath, 30000, statusCode, responseBody) || statusCode != 200)
                {
                    Game::logMsg("[Speech][TTS] %s failed: status=%lu response=%s",
                        endpointName,
                        static_cast<unsigned long>(statusCode),
                        TrimHttpResponseForLog(responseBody).c_str());
                    return false;
                }
                return true;
            };

        const std::string setSovitsPath = "/set_sovits_weights?weights_path=" + UrlEncodeHttpQueryValue(config.resolvedVitsWeightsPath);
        if (!callSwitchEndpoint("set_sovits_weights", setSovitsPath))
            return false;

        const std::string setGptPath = "/set_gpt_weights?weights_path=" + UrlEncodeHttpQueryValue(config.resolvedT2SWeightsPath);
        if (!callSwitchEndpoint("set_gpt_weights", setGptPath))
            return false;

        return true;
    }

    static std::string EscapeMciString(std::string value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value)
        {
            if (ch != '"')
                escaped.push_back(ch);
        }
        return escaped;
    }

    static uint16_t ReadLe16(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return static_cast<uint16_t>(bytes[offset])
            | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
    }

    static uint32_t ReadLe32(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return static_cast<uint32_t>(bytes[offset])
            | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    }

    static int16_t ClampFeedbackSoundSample(int32_t sample)
    {
        return static_cast<int16_t>(std::clamp(sample, -32768, 32767));
    }

    static void CleanupFeedbackSoundWavePlayback(FeedbackSoundVoiceState& voice)
    {
        if (voice.waveOut)
        {
            ::waveOutReset(voice.waveOut);
            if (voice.wavePrepared)
            {
                ::waveOutUnprepareHeader(voice.waveOut, &voice.waveHeader, sizeof(voice.waveHeader));
                voice.wavePrepared = false;
            }
            ::waveOutClose(voice.waveOut);
            voice.waveOut = nullptr;
        }

        voice.waveHeader = {};
        voice.waveStereoSamples.clear();
    }

    static bool TryLoadFeedbackSoundWavePcm(const std::string& path, std::vector<int16_t>& outSamples, uint32_t& outSampleRate, uint16_t& outChannels)
    {
        outSamples.clear();
        outSampleRate = 0;
        outChannels = 0;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 44)
            return false;

        if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
            return false;

        bool haveFmt = false;
        size_t dataOffset = 0;
        size_t dataSize = 0;
        uint16_t formatTag = 0;
        uint16_t channels = 0;
        uint16_t bitsPerSample = 0;
        uint32_t sampleRate = 0;

        for (size_t pos = 12; pos + 8 <= bytes.size();)
        {
            const uint32_t chunkSize = ReadLe32(bytes, pos + 4);
            const size_t chunkDataOffset = pos + 8;
            if (chunkDataOffset + chunkSize > bytes.size())
                break;

            if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0)
            {
                if (chunkSize < 16)
                    return false;

                formatTag = ReadLe16(bytes, chunkDataOffset + 0);
                channels = ReadLe16(bytes, chunkDataOffset + 2);
                sampleRate = ReadLe32(bytes, chunkDataOffset + 4);
                bitsPerSample = ReadLe16(bytes, chunkDataOffset + 14);
                haveFmt = true;
            }
            else if (std::memcmp(bytes.data() + pos, "data", 4) == 0)
            {
                dataOffset = chunkDataOffset;
                dataSize = static_cast<size_t>(chunkSize);
            }

            pos = chunkDataOffset + chunkSize;
            if ((chunkSize & 1u) != 0u)
                ++pos;
        }

        if (!haveFmt || dataOffset == 0 || dataSize == 0)
            return false;
        if (formatTag != 1 || (channels != 1 && channels != 2) || bitsPerSample != 16 || sampleRate == 0)
            return false;

        const size_t bytesPerSample = static_cast<size_t>(bitsPerSample / 8);
        const size_t frameSize = bytesPerSample * static_cast<size_t>(channels);
        if (frameSize == 0)
            return false;

        const size_t frameCount = dataSize / frameSize;
        if (frameCount == 0)
            return false;

        outSamples.resize(frameCount * static_cast<size_t>(channels));
        for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            const size_t sampleOffset = dataOffset + frameIndex * frameSize;
            for (uint16_t channelIndex = 0; channelIndex < channels; ++channelIndex)
            {
                const size_t channelOffset = sampleOffset + static_cast<size_t>(channelIndex) * bytesPerSample;
                outSamples[frameIndex * static_cast<size_t>(channels) + static_cast<size_t>(channelIndex)] =
                    static_cast<int16_t>(ReadLe16(bytes, channelOffset));
            }
        }

        outSampleRate = sampleRate;
        outChannels = channels;
        return true;
    }

    static std::vector<std::string> BuildVoiceInputWavePaths()
    {
        std::vector<std::string> paths;
        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return paths;

        paths.push_back(JoinWindowsPath(moduleDir, "voice_input.wav"));

        const std::string gameDirPath = JoinWindowsPath(moduleDir, "left4dead2");
        const std::string gameVoiceInputPath = JoinWindowsPath(gameDirPath, "voice_input.wav");
        if (_stricmp(paths.front().c_str(), gameVoiceInputPath.c_str()) != 0)
            paths.push_back(gameVoiceInputPath);

        return paths;
    }

    static std::vector<int16_t> ResampleMonoPcm16Linear(const std::vector<int16_t>& inputSamples, uint32_t inputSampleRate, uint32_t outputSampleRate)
    {
        if (inputSamples.empty() || inputSampleRate == 0 || outputSampleRate == 0)
            return {};
        if (inputSampleRate == outputSampleRate)
            return inputSamples;

        const double ratio = static_cast<double>(outputSampleRate) / static_cast<double>(inputSampleRate);
        const size_t outputFrameCount = std::max<size_t>(
            1,
            static_cast<size_t>(std::llround(static_cast<double>(inputSamples.size()) * ratio)));
        std::vector<int16_t> outputSamples(outputFrameCount);
        if (inputSamples.size() == 1)
        {
            std::fill(outputSamples.begin(), outputSamples.end(), inputSamples.front());
            return outputSamples;
        }

        for (size_t outputIndex = 0; outputIndex < outputFrameCount; ++outputIndex)
        {
            const double inputPosition = static_cast<double>(outputIndex) * static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
            const size_t leftIndex = static_cast<size_t>(std::floor(inputPosition));
            const size_t rightIndex = std::min(leftIndex + 1, inputSamples.size() - 1);
            const double frac = inputPosition - static_cast<double>(leftIndex);
            const double interpolated =
                static_cast<double>(inputSamples[leftIndex]) * (1.0 - frac)
                + static_cast<double>(inputSamples[rightIndex]) * frac;
            outputSamples[outputIndex] = ClampFeedbackSoundSample(static_cast<int32_t>(std::lround(interpolated)));
        }

        return outputSamples;
    }

    static bool PrepareVoiceInputWaveFile(const std::string& sourcePath, std::string& outVoiceInputPath, float& outDurationSeconds)
    {
        outVoiceInputPath.clear();
        outDurationSeconds = 0.0f;

        std::vector<int16_t> sourceSamples;
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        if (!TryLoadFeedbackSoundWavePcm(sourcePath, sourceSamples, sampleRate, channels))
            return false;
        if (sourceSamples.empty() || sampleRate == 0 || (channels != 1 && channels != 2))
            return false;

        std::vector<int16_t> monoSamples;
        if (channels == 1)
        {
            monoSamples = sourceSamples;
        }
        else
        {
            const size_t frameCount = sourceSamples.size() / 2u;
            monoSamples.resize(frameCount);
            for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
            {
                const int32_t mixed = static_cast<int32_t>(sourceSamples[frameIndex * 2u + 0u])
                    + static_cast<int32_t>(sourceSamples[frameIndex * 2u + 1u]);
                monoSamples[frameIndex] = ClampFeedbackSoundSample(mixed / 2);
            }
        }

        if (monoSamples.empty())
            return false;

        const std::vector<int16_t> voiceInputSamples = ResampleMonoPcm16Linear(monoSamples, sampleRate, kSourceVoiceInputSampleRate);
        if (voiceInputSamples.empty())
            return false;

        const std::vector<std::string> candidatePaths = BuildVoiceInputWavePaths();
        bool wroteAny = false;
        for (const std::string& candidatePath : candidatePaths)
        {
            if (!WriteMonoPcm16WaveFile(candidatePath, voiceInputSamples, kSourceVoiceInputSampleRate))
                continue;

            if (outVoiceInputPath.empty())
                outVoiceInputPath = candidatePath;
            wroteAny = true;
        }

        if (!wroteAny)
            return false;

        outDurationSeconds = static_cast<float>(voiceInputSamples.size()) / static_cast<float>(kSourceVoiceInputSampleRate);
        return outDurationSeconds > 0.0f;
    }

    static bool TryPlayFeedbackSoundWaveVoice(FeedbackSoundVoiceState& voice, int leftVolume, int rightVolume)
    {
        if (voice.waveSourceSamples.empty() || voice.waveSampleRate == 0 || (voice.waveSourceChannels != 1 && voice.waveSourceChannels != 2))
            return false;

        CleanupFeedbackSoundWavePlayback(voice);

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = voice.waveSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        if (::waveOutOpen(&voice.waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
            return false;

        const size_t frameCount = voice.waveSourceSamples.size() / static_cast<size_t>(voice.waveSourceChannels);
        voice.waveStereoSamples.resize(frameCount * 2u);
        for (size_t i = 0; i < frameCount; ++i)
        {
            int32_t sourceLeft = 0;
            int32_t sourceRight = 0;
            if (voice.waveSourceChannels == 1)
            {
                sourceLeft = static_cast<int32_t>(voice.waveSourceSamples[i]);
                sourceRight = sourceLeft;
            }
            else
            {
                sourceLeft = static_cast<int32_t>(voice.waveSourceSamples[i * 2u]);
                sourceRight = static_cast<int32_t>(voice.waveSourceSamples[i * 2u + 1u]);
            }

            const int32_t left = (sourceLeft * leftVolume) / 1000;
            const int32_t right = (sourceRight * rightVolume) / 1000;
            voice.waveStereoSamples[i * 2u] = ClampFeedbackSoundSample(left);
            voice.waveStereoSamples[i * 2u + 1u] = ClampFeedbackSoundSample(right);
        }

        voice.waveHeader = {};
        voice.waveHeader.lpData = reinterpret_cast<LPSTR>(voice.waveStereoSamples.data());
        voice.waveHeader.dwBufferLength = static_cast<DWORD>(voice.waveStereoSamples.size() * sizeof(int16_t));

        if (::waveOutPrepareHeader(voice.waveOut, &voice.waveHeader, sizeof(voice.waveHeader)) != MMSYSERR_NOERROR)
        {
            CleanupFeedbackSoundWavePlayback(voice);
            return false;
        }

        voice.wavePrepared = true;
        if (::waveOutWrite(voice.waveOut, &voice.waveHeader, sizeof(voice.waveHeader)) != MMSYSERR_NOERROR)
        {
            CleanupFeedbackSoundWavePlayback(voice);
            return false;
        }

        return true;
    }

    static std::array<FeedbackSoundVoiceState, kFeedbackSoundVoiceCount>& GetFeedbackSoundVoices()
    {
        static std::array<FeedbackSoundVoiceState, kFeedbackSoundVoiceCount> voices{};
        static bool initialized = false;
        if (!initialized)
        {
            for (int i = 0; i < kFeedbackSoundVoiceCount; ++i)
                voices[static_cast<size_t>(i)].alias = "l4d2vr_feedback_" + std::to_string(i);

            initialized = true;
        }

        return voices;
    }

    static bool IsSameFeedbackSoundPath(const std::string& lhs, const std::string& rhs)
    {
        return _stricmp(lhs.c_str(), rhs.c_str()) == 0;
    }

    static void CloseFeedbackSoundVoice(FeedbackSoundVoiceState& voice)
    {
        CleanupFeedbackSoundWavePlayback(voice);

        if (!voice.alias.empty())
        {
            const std::string closeCmd = std::string("close ") + voice.alias;
            ::mciSendStringA(closeCmd.c_str(), nullptr, 0, nullptr);
        }

        voice.loadedPath.clear();
        voice.isOpen = false;
        voice.usesWaveOut = false;
        voice.waveSampleRate = 0;
        voice.waveSourceChannels = 0;
        voice.waveSourceSamples.clear();
    }

    static void CloseAllFeedbackSoundVoices()
    {
        auto& voices = GetFeedbackSoundVoices();
        for (FeedbackSoundVoiceState& voice : voices)
            CloseFeedbackSoundVoice(voice);
    }

    static bool EnsureFeedbackSoundVoiceOpen(FeedbackSoundVoiceState& voice, const std::string& resolvedPath)
    {
        if (resolvedPath.empty())
            return false;

        const bool wantsWaveOut = EndsWithInsensitive(resolvedPath, ".wav");
        if (voice.isOpen && !voice.loadedPath.empty() && IsSameFeedbackSoundPath(voice.loadedPath, resolvedPath))
            return true;

        CloseFeedbackSoundVoice(voice);
        if (wantsWaveOut)
        {
            if (!TryLoadFeedbackSoundWavePcm(resolvedPath, voice.waveSourceSamples, voice.waveSampleRate, voice.waveSourceChannels))
                return false;

            voice.loadedPath = resolvedPath;
            voice.isOpen = true;
            voice.usesWaveOut = true;
            return true;
        }

        const std::string escapedPath = EscapeMciString(resolvedPath);
        const std::array<std::string, 2> openCommands =
        {
            std::string("open \"") + escapedPath + "\" type mpegvideo alias " + voice.alias,
            std::string("open \"") + escapedPath + "\" alias " + voice.alias
        };

        bool opened = false;
        for (const std::string& openCmd : openCommands)
        {
            if (::mciSendStringA(openCmd.c_str(), nullptr, 0, nullptr) == 0)
            {
                opened = true;
                break;
            }
        }

        if (!opened)
            return false;

        voice.loadedPath = resolvedPath;
        voice.isOpen = true;
        voice.usesWaveOut = false;
        return true;
    }

    static FeedbackSoundVoiceState& AcquireFeedbackSoundVoice(const std::string* preferredResolvedPath = nullptr)
    {
        auto& voices = GetFeedbackSoundVoices();
        if (preferredResolvedPath && !preferredResolvedPath->empty())
        {
            auto it = std::min_element(
                voices.begin(),
                voices.end(),
                [&](const FeedbackSoundVoiceState& lhs, const FeedbackSoundVoiceState& rhs)
                {
                    const bool lhsMatches = lhs.isOpen && !lhs.loadedPath.empty() && IsSameFeedbackSoundPath(lhs.loadedPath, *preferredResolvedPath);
                    const bool rhsMatches = rhs.isOpen && !rhs.loadedPath.empty() && IsSameFeedbackSoundPath(rhs.loadedPath, *preferredResolvedPath);
                    if (lhsMatches != rhsMatches)
                        return lhsMatches;
                    if (lhsMatches && rhsMatches)
                        return lhs.lastStarted < rhs.lastStarted;
                    if (lhs.isOpen != rhs.isOpen)
                        return !lhs.isOpen;
                    return lhs.lastStarted < rhs.lastStarted;
                });
            return *it;
        }

        auto it = std::min_element(
            voices.begin(),
            voices.end(),
            [](const FeedbackSoundVoiceState& lhs, const FeedbackSoundVoiceState& rhs)
            {
                if (lhs.isOpen != rhs.isOpen)
                    return !lhs.isOpen;
                return lhs.lastStarted < rhs.lastStarted;
            });
        return *it;
    }

    static bool TryResolveFeedbackSoundFileSpec(const std::string& rawSpec, std::string& outResolvedPath)
    {
        const std::string spec = TrimCopy(rawSpec);
        if (spec.empty())
            return false;

        auto getPayload = [&](size_t prefixLen)
            {
                return TrimCopy(spec.substr(prefixLen));
            };

        if (StartsWithInsensitive(spec, "alias:") || StartsWithInsensitive(spec, "cmd:"))
            return false;

        if (StartsWithInsensitive(spec, "file:"))
            outResolvedPath = ResolveKillSoundFilePath(getPayload(5));
        else if (StartsWithInsensitive(spec, "game:"))
            outResolvedPath = ResolveGameSoundFilePath(getPayload(5));
        else if (StartsWithInsensitive(spec, "gamesound:"))
            outResolvedPath = ResolveBuiltinFeedbackGameSoundPath(getPayload(10));
        else
            outResolvedPath = ResolveKillSoundFilePath(spec);

        return !outResolvedPath.empty();
    }

    static void ApplyFeedbackSoundStereoVolumes(const std::string& alias, int leftVolume, int rightVolume)
    {
        if (alias.empty())
            return;

        leftVolume = std::clamp(leftVolume, 0, 1000);
        rightVolume = std::clamp(rightVolume, 0, 1000);

        const int overallVolume = std::clamp((leftVolume + rightVolume) / 2, 0, 1000);
        const std::string volumeCmd = "setaudio " + alias + " volume to " + std::to_string(overallVolume);
        ::mciSendStringA(volumeCmd.c_str(), nullptr, 0, nullptr);

        const std::string leftCmd = "setaudio " + alias + " left volume to " + std::to_string(leftVolume);
        ::mciSendStringA(leftCmd.c_str(), nullptr, 0, nullptr);

        const std::string rightCmd = "setaudio " + alias + " right volume to " + std::to_string(rightVolume);
        ::mciSendStringA(rightCmd.c_str(), nullptr, 0, nullptr);
    }

    static bool TryPlayFeedbackSoundFilePath(const std::string& rawPath, int leftVolume, int rightVolume, bool preferLoadedPathReuse = true)
    {
        const std::string resolvedPath = ResolveKillSoundFilePath(rawPath);
        if (resolvedPath.empty())
            return false;

        leftVolume = std::clamp(leftVolume, 0, 1000);
        rightVolume = std::clamp(rightVolume, 0, 1000);
        if (leftVolume == 0 && rightVolume == 0)
            return true;

        FeedbackSoundVoiceState& voice = preferLoadedPathReuse
            ? AcquireFeedbackSoundVoice(&resolvedPath)
            : AcquireFeedbackSoundVoice();
        if (!EnsureFeedbackSoundVoiceOpen(voice, resolvedPath))
            return false;

        if (voice.usesWaveOut)
        {
            if (!TryPlayFeedbackSoundWaveVoice(voice, leftVolume, rightVolume))
            {
                CloseFeedbackSoundVoice(voice);
                return false;
            }

            voice.lastStarted = std::chrono::steady_clock::now();
            return true;
        }

        const std::string stopCmd = std::string("stop ") + voice.alias;
        ::mciSendStringA(stopCmd.c_str(), nullptr, 0, nullptr);
        const std::string seekCmd = std::string("seek ") + voice.alias + " to start";
        ::mciSendStringA(seekCmd.c_str(), nullptr, 0, nullptr);

        ApplyFeedbackSoundStereoVolumes(voice.alias, leftVolume, rightVolume);

        const std::string playCmd = std::string("play ") + voice.alias + " from 0";
        if (::mciSendStringA(playCmd.c_str(), nullptr, 0, nullptr) != 0)
        {
            CloseFeedbackSoundVoice(voice);
            return false;
        }

        voice.lastStarted = std::chrono::steady_clock::now();
        return true;
    }

    static float Clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    static float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    static float EaseOutCubic(float t)
    {
        const float clamped = Clamp01(t);
        const float inv = 1.0f - clamped;
        return 1.0f - inv * inv * inv;
    }

    static const char* DescribeFeedbackSoundDebugForceChannel(int forceChannel)
    {
        if (forceChannel < 0)
            return "force_left";
        if (forceChannel > 0)
            return "force_right";
        return "normal";
    }

    static std::string NormalizeMaterialPathSpec(std::string rawSpec)
    {
        std::string spec = TrimCopy(rawSpec);
        if (spec.empty())
            return {};

        std::replace(spec.begin(), spec.end(), '\\', '/');

        std::string lowered = spec;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        const size_t materialMarker = lowered.find("/materials/");
        if (materialMarker != std::string::npos)
            spec = spec.substr(materialMarker + 11);
        else if (lowered.rfind("materials/", 0) == 0)
            spec = spec.substr(10);

        while (!spec.empty() && spec.front() == '/')
            spec.erase(spec.begin());

        if (EndsWithInsensitive(spec, ".vmt"))
            spec.erase(spec.size() - 4);

        while (!spec.empty() && spec.back() == '/')
            spec.pop_back();

        return TrimCopy(spec);
    }

    static std::string BuildKillIndicatorMaterialName(const std::string& rawBaseSpec, const char* desiredLeaf)
    {
        std::string spec = NormalizeMaterialPathSpec(rawBaseSpec);
        if (spec.empty() || !desiredLeaf || !*desiredLeaf)
            return {};

        const size_t slash = spec.find_last_of('/');
        const std::string leaf = (slash == std::string::npos) ? spec : spec.substr(slash + 1);

        if (_stricmp(leaf.c_str(), "kill") == 0 || _stricmp(leaf.c_str(), "headshot") == 0 || _stricmp(leaf.c_str(), "hit") == 0)
        {
            if (slash == std::string::npos)
                return desiredLeaf;

            return spec.substr(0, slash + 1) + desiredLeaf;
        }

        return spec + "/" + desiredLeaf;
    }

    struct KillIndicatorDecodedFrames
    {
        bool attempted = false;
        bool loaded = false;
        bool additive = false;
        uint32_t width = 0;
        uint32_t height = 0;
        float frameRate = 0.0f;
        std::vector<std::vector<uint8_t>> frames;
    };

    static std::string NormalizeSlashes(std::string value, char slash)
    {
        std::replace(value.begin(), value.end(), '\\', slash);
        std::replace(value.begin(), value.end(), '/', slash);
        return value;
    }

    static std::string TrimQuotesCopy(std::string value)
    {
        value = TrimCopy(value);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }

    static size_t BytesPerBlockForImageFormat(ImageFormat format)
    {
        switch (format)
        {
        case IMAGE_FORMAT_DXT1:
        case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            return 8;
        case IMAGE_FORMAT_DXT3:
        case IMAGE_FORMAT_DXT5:
            return 16;
        default:
            return 0;
        }
    }

    static size_t ComputeImageByteSize(ImageFormat format, uint32_t width, uint32_t height)
    {
        switch (format)
        {
        case IMAGE_FORMAT_RGBA8888:
        case IMAGE_FORMAT_ABGR8888:
        case IMAGE_FORMAT_ARGB8888:
        case IMAGE_FORMAT_BGRA8888:
        case IMAGE_FORMAT_BGRX8888:
            return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        case IMAGE_FORMAT_RGB888:
        case IMAGE_FORMAT_BGR888:
            return static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
        case IMAGE_FORMAT_DXT1:
        case IMAGE_FORMAT_DXT1_ONEBITALPHA:
        case IMAGE_FORMAT_DXT3:
        case IMAGE_FORMAT_DXT5:
        {
            const size_t blockWidth = (static_cast<size_t>(width) + 3u) / 4u;
            const size_t blockHeight = (static_cast<size_t>(height) + 3u) / 4u;
            return blockWidth * blockHeight * BytesPerBlockForImageFormat(format);
        }
        default:
            return 0;
        }
    }

    static void DecodeRgb565(uint16_t packed, uint8_t& outR, uint8_t& outG, uint8_t& outB)
    {
        outR = static_cast<uint8_t>(((packed >> 11) & 0x1Fu) * 255u / 31u);
        outG = static_cast<uint8_t>(((packed >> 5) & 0x3Fu) * 255u / 63u);
        outB = static_cast<uint8_t>((packed & 0x1Fu) * 255u / 31u);
    }

    static void DecodeDxt1Block(const uint8_t* block, uint8_t* rgba, uint32_t stride, uint32_t maxX, uint32_t maxY, bool oneBitAlpha)
    {
        const uint16_t color0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
        const uint16_t color1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
        uint8_t palette[4][4] = {};
        DecodeRgb565(color0, palette[0][0], palette[0][1], palette[0][2]);
        DecodeRgb565(color1, palette[1][0], palette[1][1], palette[1][2]);
        palette[0][3] = 255;
        palette[1][3] = 255;

        if (color0 > color1 || !oneBitAlpha)
        {
            for (int c = 0; c < 3; ++c)
            {
                palette[2][c] = static_cast<uint8_t>((2u * palette[0][c] + palette[1][c]) / 3u);
                palette[3][c] = static_cast<uint8_t>((palette[0][c] + 2u * palette[1][c]) / 3u);
            }
            palette[2][3] = 255;
            palette[3][3] = 255;
        }
        else
        {
            for (int c = 0; c < 3; ++c)
                palette[2][c] = static_cast<uint8_t>((palette[0][c] + palette[1][c]) / 2u);
            palette[2][3] = 255;
            palette[3][0] = palette[3][1] = palette[3][2] = 0;
            palette[3][3] = 0;
        }

        uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
        for (uint32_t y = 0; y < maxY; ++y)
        {
            for (uint32_t x = 0; x < maxX; ++x)
            {
                const uint32_t idx = (indices >> (2u * (4u * y + x))) & 0x3u;
                uint8_t* dst = rgba + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
                dst[0] = palette[idx][0];
                dst[1] = palette[idx][1];
                dst[2] = palette[idx][2];
                dst[3] = palette[idx][3];
            }
        }
    }

    static void DecodeDxt5Block(const uint8_t* block, uint8_t* rgba, uint32_t stride, uint32_t maxX, uint32_t maxY)
    {
        uint8_t alphaPalette[8] = {};
        alphaPalette[0] = block[0];
        alphaPalette[1] = block[1];
        if (alphaPalette[0] > alphaPalette[1])
        {
            alphaPalette[2] = static_cast<uint8_t>((6u * alphaPalette[0] + 1u * alphaPalette[1]) / 7u);
            alphaPalette[3] = static_cast<uint8_t>((5u * alphaPalette[0] + 2u * alphaPalette[1]) / 7u);
            alphaPalette[4] = static_cast<uint8_t>((4u * alphaPalette[0] + 3u * alphaPalette[1]) / 7u);
            alphaPalette[5] = static_cast<uint8_t>((3u * alphaPalette[0] + 4u * alphaPalette[1]) / 7u);
            alphaPalette[6] = static_cast<uint8_t>((2u * alphaPalette[0] + 5u * alphaPalette[1]) / 7u);
            alphaPalette[7] = static_cast<uint8_t>((1u * alphaPalette[0] + 6u * alphaPalette[1]) / 7u);
        }
        else
        {
            alphaPalette[2] = static_cast<uint8_t>((4u * alphaPalette[0] + 1u * alphaPalette[1]) / 5u);
            alphaPalette[3] = static_cast<uint8_t>((3u * alphaPalette[0] + 2u * alphaPalette[1]) / 5u);
            alphaPalette[4] = static_cast<uint8_t>((2u * alphaPalette[0] + 3u * alphaPalette[1]) / 5u);
            alphaPalette[5] = static_cast<uint8_t>((1u * alphaPalette[0] + 4u * alphaPalette[1]) / 5u);
            alphaPalette[6] = 0;
            alphaPalette[7] = 255;
        }

        uint64_t alphaIndices = 0;
        for (int i = 0; i < 6; ++i)
            alphaIndices |= static_cast<uint64_t>(block[2 + i]) << (8u * i);

        DecodeDxt1Block(block + 8, rgba, stride, maxX, maxY, false);

        for (uint32_t y = 0; y < maxY; ++y)
        {
            for (uint32_t x = 0; x < maxX; ++x)
            {
                const uint32_t alphaIndex = static_cast<uint32_t>((alphaIndices >> (3u * (4u * y + x))) & 0x7u);
                uint8_t* dst = rgba + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
                dst[3] = alphaPalette[alphaIndex];
            }
        }
    }

    static bool ResizeRgbaNearest(const std::vector<uint8_t>& src, uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight, std::vector<uint8_t>& dst)
    {
        if (src.empty() || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
            return false;

        dst.resize(static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight) * 4u);
        for (uint32_t y = 0; y < dstHeight; ++y)
        {
            const uint32_t srcY = static_cast<uint32_t>((static_cast<uint64_t>(y) * srcHeight) / dstHeight);
            for (uint32_t x = 0; x < dstWidth; ++x)
            {
                const uint32_t srcX = static_cast<uint32_t>((static_cast<uint64_t>(x) * srcWidth) / dstWidth);
                const uint8_t* srcPx = src.data() + (static_cast<size_t>(srcY) * srcWidth + srcX) * 4u;
                uint8_t* dstPx = dst.data() + (static_cast<size_t>(y) * dstWidth + x) * 4u;
                dstPx[0] = srcPx[0];
                dstPx[1] = srcPx[1];
                dstPx[2] = srcPx[2];
                dstPx[3] = srcPx[3];
            }
        }
        return true;
    }

    static bool CropRgba(const std::vector<uint8_t>& src, uint32_t srcWidth, uint32_t srcHeight, uint32_t left, uint32_t top, uint32_t cropWidth, uint32_t cropHeight, std::vector<uint8_t>& dst)
    {
        if (src.empty() || srcWidth == 0 || srcHeight == 0 || cropWidth == 0 || cropHeight == 0)
            return false;
        if (left >= srcWidth || top >= srcHeight)
            return false;
        if (left + cropWidth > srcWidth || top + cropHeight > srcHeight)
            return false;

        dst.resize(static_cast<size_t>(cropWidth) * static_cast<size_t>(cropHeight) * 4u);
        for (uint32_t y = 0; y < cropHeight; ++y)
        {
            const uint8_t* srcRow = src.data() + ((static_cast<size_t>(top + y) * srcWidth) + left) * 4u;
            uint8_t* dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(cropWidth) * 4u;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(cropWidth) * 4u);
        }
        return true;
    }

    static void ConvertAdditiveRgbaToOverlay(std::vector<uint8_t>& rgba)
    {
        constexpr uint8_t kVisibilityThreshold = 12;

        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            const uint8_t srcR = rgba[i + 0];
            const uint8_t srcG = rgba[i + 1];
            const uint8_t srcB = rgba[i + 2];
            const uint8_t srcA = rgba[i + 3];
            const uint8_t maxRgb = (std::max)({ srcR, srcG, srcB });

            if (maxRgb <= kVisibilityThreshold || srcA == 0)
            {
                rgba[i + 0] = 0;
                rgba[i + 1] = 0;
                rgba[i + 2] = 0;
                rgba[i + 3] = 0;
                continue;
            }

            const float intensity = static_cast<float>(maxRgb) / 255.0f;
            const uint8_t overlayAlpha = static_cast<uint8_t>(std::clamp(std::lround(std::pow(intensity, 0.75f) * 255.0f), 0l, 255l));
            const float hueScale = 255.0f / static_cast<float>(maxRgb);

            rgba[i + 0] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(srcR) * hueScale), 0l, 255l));
            rgba[i + 1] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(srcG) * hueScale), 0l, 255l));
            rgba[i + 2] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(srcB) * hueScale), 0l, 255l));
            rgba[i + 3] = overlayAlpha;
        }
    }

    static bool FindVisibleRgbaBounds(const std::vector<std::vector<uint8_t>>& frames, uint32_t width, uint32_t height, uint32_t& outLeft, uint32_t& outTop, uint32_t& outWidth, uint32_t& outHeight)
    {
        if (frames.empty() || width == 0 || height == 0)
            return false;

        constexpr uint8_t kVisibleAlphaThreshold = 8;

        bool foundAny = false;
        uint32_t minX = width;
        uint32_t minY = height;
        uint32_t maxX = 0;
        uint32_t maxY = 0;

        for (const std::vector<uint8_t>& frame : frames)
        {
            if (frame.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
                continue;

            for (uint32_t y = 0; y < height; ++y)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint8_t* px = frame.data() + ((static_cast<size_t>(y) * width) + x) * 4u;
                    if (px[3] <= kVisibleAlphaThreshold)
                        continue;

                    foundAny = true;
                    minX = (std::min)(minX, x);
                    minY = (std::min)(minY, y);
                    maxX = (std::max)(maxX, x);
                    maxY = (std::max)(maxY, y);
                }
            }
        }

        if (!foundAny)
            return false;

        const uint32_t padding = (std::max)(4u, (std::max)(width, height) / 64u);
        outLeft = (minX > padding) ? (minX - padding) : 0u;
        outTop = (minY > padding) ? (minY - padding) : 0u;
        const uint32_t paddedRight = (std::min)(width - 1u, maxX + padding);
        const uint32_t paddedBottom = (std::min)(height - 1u, maxY + padding);
        outWidth = paddedRight - outLeft + 1u;
        outHeight = paddedBottom - outTop + 1u;
        return outWidth > 0 && outHeight > 0;
    }

    static bool ParseTrailingBoolValue(const std::string& line, const char* key, bool& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        const std::string lowered = ToLowerCopy(trimmed);
        const std::string loweredKey = ToLowerCopy(key);
        const size_t keyPos = lowered.find(loweredKey);
        if (keyPos == std::string::npos)
            return false;

        std::string tail = ToLowerCopy(TrimQuotesCopy(trimmed.substr(keyPos + loweredKey.size())));
        if (tail.empty())
            return false;

        if (tail == "1" || tail == "true" || tail == "yes")
        {
            outValue = true;
            return true;
        }
        if (tail == "0" || tail == "false" || tail == "no")
        {
            outValue = false;
            return true;
        }

        return false;
    }

    static bool ParseQuotedMaterialValue(const std::string& line, const char* key, std::string& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        const std::string lowered = ToLowerCopy(trimmed);
        const std::string loweredKey = ToLowerCopy(key);
        const size_t keyPos = lowered.find(loweredKey);
        if (keyPos == std::string::npos)
            return false;

        size_t searchPos = keyPos + loweredKey.size();
        while (searchPos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[searchPos])))
            ++searchPos;

        if (searchPos >= trimmed.size() || trimmed[searchPos] != '"')
            return false;

        size_t firstQuote = searchPos;
        size_t secondQuote = trimmed.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos)
            return false;

        std::string candidate = TrimCopy(trimmed.substr(firstQuote + 1, secondQuote - firstQuote - 1));
        if (!candidate.empty())
        {
            outValue = candidate;
            return true;
        }

        firstQuote = secondQuote;
        secondQuote = trimmed.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos)
            return false;

        outValue = TrimCopy(trimmed.substr(firstQuote + 1, secondQuote - firstQuote - 1));
        return !outValue.empty();
    }

    static bool ParseTrailingFloatValue(const std::string& line, const char* key, float& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        const std::string lowered = ToLowerCopy(trimmed);
        const std::string loweredKey = ToLowerCopy(key);
        const size_t keyPos = lowered.find(loweredKey);
        if (keyPos == std::string::npos)
            return false;

        std::string tail = TrimQuotesCopy(trimmed.substr(keyPos + loweredKey.size()));
        if (tail.empty())
            return false;

        char* endPtr = nullptr;
        const float value = std::strtof(tail.c_str(), &endPtr);
        if (endPtr == tail.c_str())
            return false;
        outValue = value;
        return true;
    }

    static bool LoadKillIndicatorDecodedFramesFromDisk(const std::string& materialName, KillIndicatorDecodedFrames& outFrames)
    {
        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return false;

        const std::string normalizedMaterial = NormalizeMaterialPathSpec(materialName);
        if (normalizedMaterial.empty())
            return false;

        const std::string materialsDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "materials");
        const std::string vmtPath = JoinWindowsPath(materialsDir, NormalizeSlashes(normalizedMaterial, '\\') + ".vmt");

        std::ifstream vmtFile(vmtPath);
        if (!vmtFile.is_open())
            return false;

        std::string baseTexture = normalizedMaterial;
        float frameRate = 0.0f;
        bool additive = false;
        std::string line;
        while (std::getline(vmtFile, line))
        {
            std::string parsedValue;
            if (ParseQuotedMaterialValue(line, "$basetexture", parsedValue))
                baseTexture = NormalizeMaterialPathSpec(parsedValue);

            float parsedRate = 0.0f;
            if (ParseTrailingFloatValue(line, "animatedTextureFrameRate", parsedRate))
                frameRate = (std::max)(0.0f, parsedRate);

            std::string parsedBoolValue;
            if (ParseQuotedMaterialValue(line, "$additive", parsedBoolValue))
            {
                const std::string loweredBool = ToLowerCopy(TrimQuotesCopy(parsedBoolValue));
                if (loweredBool == "1" || loweredBool == "true" || loweredBool == "yes")
                    additive = true;
                else if (loweredBool == "0" || loweredBool == "false" || loweredBool == "no")
                    additive = false;
            }
            else
            {
                bool parsedAdditive = false;
                if (ParseTrailingBoolValue(line, "$additive", parsedAdditive))
                    additive = parsedAdditive;
            }
        }

        const std::string vtfPath = JoinWindowsPath(materialsDir, NormalizeSlashes(baseTexture, '\\') + ".vtf");
        std::ifstream vtfFile(vtfPath, std::ios::binary);
        if (!vtfFile.is_open())
            return false;

        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(vtfFile)), std::istreambuf_iterator<char>());
        if (bytes.size() < 80 || bytes[0] != 'V' || bytes[1] != 'T' || bytes[2] != 'F' || bytes[3] != '\0')
            return false;

        auto readU16 = [&](size_t offset) -> uint16_t
            {
                return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
            };
        auto readU32 = [&](size_t offset) -> uint32_t
            {
                return static_cast<uint32_t>(bytes[offset]
                    | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
                    | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
                    | (static_cast<uint32_t>(bytes[offset + 3]) << 24));
            };

        const uint32_t headerSize = readU32(12);
        const uint32_t width = readU16(16);
        const uint32_t height = readU16(18);
        const uint32_t frameCount = (std::max<uint32_t>)(1u, readU16(24));
        const ImageFormat highResFormat = static_cast<ImageFormat>(readU32(52));
        const uint8_t mipCount = bytes[56];
        const ImageFormat lowResFormat = static_cast<ImageFormat>(readU32(57));
        const uint8_t lowResWidth = bytes[61];
        const uint8_t lowResHeight = bytes[62];
        const uint16_t depth = readU16(63);
        if (headerSize >= bytes.size() || width == 0 || height == 0 || mipCount == 0 || depth == 0)
            return false;

        const size_t lowResBytes = ComputeImageByteSize(lowResFormat, lowResWidth, lowResHeight);
        size_t highResOffset = static_cast<size_t>(headerSize) + lowResBytes;
        for (int mip = static_cast<int>(mipCount) - 1; mip >= 1; --mip)
        {
            const uint32_t mipWidth = (std::max)(1u, width >> mip);
            const uint32_t mipHeight = (std::max)(1u, height >> mip);
            const uint32_t mipDepth = (std::max)(1u, static_cast<uint32_t>(depth) >> mip);
            const size_t mipBytes = ComputeImageByteSize(highResFormat, mipWidth, mipHeight) * static_cast<size_t>(mipDepth) * static_cast<size_t>(frameCount);
            highResOffset += mipBytes;
        }

        const size_t frameBytes = ComputeImageByteSize(highResFormat, width, height);
        if (frameBytes == 0 || highResOffset + frameBytes * static_cast<size_t>(frameCount) > bytes.size())
            return false;

        outFrames.frames.clear();
        outFrames.frames.reserve(frameCount);
        outFrames.additive = additive;
        outFrames.width = width;
        outFrames.height = height;
        outFrames.frameRate = frameRate;

        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            const uint8_t* frameData = bytes.data() + highResOffset + frameIndex * frameBytes;
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);

            switch (highResFormat)
            {
            case IMAGE_FORMAT_DXT1:
            case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            {
                const size_t blocksWide = (static_cast<size_t>(width) + 3u) / 4u;
                const size_t blocksHigh = (static_cast<size_t>(height) + 3u) / 4u;
                for (size_t by = 0; by < blocksHigh; ++by)
                {
                    for (size_t bx = 0; bx < blocksWide; ++bx)
                    {
                        const uint8_t* block = frameData + (by * blocksWide + bx) * 8u;
                        uint8_t* dst = rgba.data() + (by * 4u * static_cast<size_t>(width) + bx * 4u) * 4u;
                        const uint32_t maxX = static_cast<uint32_t>((std::min)(4u, width - static_cast<uint32_t>(bx * 4u)));
                        const uint32_t maxY = static_cast<uint32_t>((std::min)(4u, height - static_cast<uint32_t>(by * 4u)));
                        DecodeDxt1Block(block, dst, width * 4u, maxX, maxY, highResFormat == IMAGE_FORMAT_DXT1_ONEBITALPHA);
                    }
                }
                break;
            }
            case IMAGE_FORMAT_DXT5:
            {
                const size_t blocksWide = (static_cast<size_t>(width) + 3u) / 4u;
                const size_t blocksHigh = (static_cast<size_t>(height) + 3u) / 4u;
                for (size_t by = 0; by < blocksHigh; ++by)
                {
                    for (size_t bx = 0; bx < blocksWide; ++bx)
                    {
                        const uint8_t* block = frameData + (by * blocksWide + bx) * 16u;
                        uint8_t* dst = rgba.data() + (by * 4u * static_cast<size_t>(width) + bx * 4u) * 4u;
                        const uint32_t maxX = static_cast<uint32_t>((std::min)(4u, width - static_cast<uint32_t>(bx * 4u)));
                        const uint32_t maxY = static_cast<uint32_t>((std::min)(4u, height - static_cast<uint32_t>(by * 4u)));
                        DecodeDxt5Block(block, dst, width * 4u, maxX, maxY);
                    }
                }
                break;
            }
            case IMAGE_FORMAT_BGRA8888:
            {
                for (uint32_t i = 0; i < width * height; ++i)
                {
                    rgba[i * 4 + 0] = frameData[i * 4 + 2];
                    rgba[i * 4 + 1] = frameData[i * 4 + 1];
                    rgba[i * 4 + 2] = frameData[i * 4 + 0];
                    rgba[i * 4 + 3] = frameData[i * 4 + 3];
                }
                break;
            }
            case IMAGE_FORMAT_RGBA8888:
            {
                std::memcpy(rgba.data(), frameData, rgba.size());
                break;
            }
            default:
                return false;
            }

            outFrames.frames.push_back(std::move(rgba));
        }

        if (outFrames.additive)
        {
            for (std::vector<uint8_t>& frame : outFrames.frames)
                ConvertAdditiveRgbaToOverlay(frame);
        }

        uint32_t visibleLeft = 0;
        uint32_t visibleTop = 0;
        uint32_t visibleWidth = outFrames.width;
        uint32_t visibleHeight = outFrames.height;
        if (FindVisibleRgbaBounds(outFrames.frames, outFrames.width, outFrames.height, visibleLeft, visibleTop, visibleWidth, visibleHeight)
            && (visibleWidth < outFrames.width || visibleHeight < outFrames.height))
        {
            for (std::vector<uint8_t>& frame : outFrames.frames)
            {
                std::vector<uint8_t> cropped;
                if (CropRgba(frame, outFrames.width, outFrames.height, visibleLeft, visibleTop, visibleWidth, visibleHeight, cropped))
                    frame = std::move(cropped);
            }

            outFrames.width = visibleWidth;
            outFrames.height = visibleHeight;
        }

        if (outFrames.width > 256 || outFrames.height > 256)
        {
            const uint32_t dstWidth = (std::min)(256u, outFrames.width);
            const uint32_t dstHeight = (std::min)(256u, outFrames.height);
            for (std::vector<uint8_t>& frame : outFrames.frames)
            {
                std::vector<uint8_t> resized;
                if (ResizeRgbaNearest(frame, outFrames.width, outFrames.height, dstWidth, dstHeight, resized))
                    frame = std::move(resized);
            }
            outFrames.width = dstWidth;
            outFrames.height = dstHeight;
        }

        outFrames.loaded = !outFrames.frames.empty();
        return outFrames.loaded;
    }

    static KillIndicatorDecodedFrames& GetKillIndicatorDecodedFrameCache(const std::string& materialName)
    {
        static std::unordered_map<std::string, KillIndicatorDecodedFrames> cache;
        const std::string key = ToLowerCopy(materialName);
        KillIndicatorDecodedFrames& entry = cache[key];
        if (!entry.attempted)
        {
            entry.attempted = true;
            entry.loaded = LoadKillIndicatorDecodedFramesFromDisk(materialName, entry);
        }
        return entry;
    }

    static float GetActiveIndicatorLifetimeSeconds(const VR::ActiveKillIndicator& indicator, float killLifetimeSeconds)
    {
        if (indicator.killConfirmed)
            return (std::max)(0.10f, killLifetimeSeconds);

        const float scaled = killLifetimeSeconds * 0.42f;
        return std::clamp(scaled, 0.10f, 0.45f);
    }

    enum class KillIndicatorMaterialKind
    {
        Hit = 0,
        Kill = 1,
        Headshot = 2,
    };

    static int GetPreboundKillIndicatorMaterialForSlot(int slotIndex)
    {
        // Most simultaneous markers are ordinary hits. Keep small dedicated pools
        // for kill/headshot so gameplay never has to rebind a Vulkan texture when
        // an event fires.
        if (slotIndex < 12)
            return static_cast<int>(KillIndicatorMaterialKind::Hit);
        if (slotIndex < 14)
            return static_cast<int>(KillIndicatorMaterialKind::Kill);
        return static_cast<int>(KillIndicatorMaterialKind::Headshot);
    }

    static IDirect3DDevice9* GetKillIndicatorD3DDevice(VR* vr)
    {
        if (!vr)
            return nullptr;

        IDirect3DDevice9* device = nullptr;
        if (vr->m_D9HUDSurface)
            vr->m_D9HUDSurface->GetDevice(&device);
        else if (vr->m_D9LeftEyeSurface)
            vr->m_D9LeftEyeSurface->GetDevice(&device);
        else if (vr->m_D9RightEyeSurface)
            vr->m_D9RightEyeSurface->GetDevice(&device);
        return device;
    }

    static bool ProjectKillIndicatorToHud(const VR* vr, const Vector& worldPos, int screenWidth, int screenHeight, float maxDistance, int& outX, int& outY)
    {
        if (!vr || screenWidth <= 0 || screenHeight <= 0)
            return false;

        Vector camForward{};
        Vector camRight{};
        Vector camUp{};
        QAngle::AngleVectors(vr->m_SetupAngles, &camForward, &camRight, &camUp);
        if (camForward.IsZero() || camRight.IsZero() || camUp.IsZero())
        {
            camForward = vr->m_HmdForward;
            camRight = vr->m_HmdRight;
            camUp = vr->m_HmdUp;
        }

        if (camForward.IsZero() || camRight.IsZero() || camUp.IsZero())
            return false;

        VectorNormalize(camForward);
        VectorNormalize(camRight);
        VectorNormalize(camUp);

        const Vector delta = worldPos - vr->m_SetupOrigin;
        if (maxDistance > 0.0f && delta.LengthSqr() > (maxDistance * maxDistance))
            return false;

        const float depth = DotProduct(delta, camForward);
        if (depth <= 4.0f)
            return false;

        constexpr float kPi = 3.14159265358979323846f;
        const float fovXDeg = std::clamp(vr->m_Fov, 10.0f, 170.0f);
        const float tanHalfFovX = std::tan((fovXDeg * 0.5f) * (kPi / 180.0f));
        const float aspect = std::max(0.25f, static_cast<float>(screenWidth) / static_cast<float>(screenHeight));
        const float tanHalfFovY = tanHalfFovX / aspect;
        if (tanHalfFovX <= 0.0f || tanHalfFovY <= 0.0f)
            return false;

        const float ndcX = (DotProduct(delta, camRight) / depth) / tanHalfFovX;
        const float ndcY = (DotProduct(delta, camUp) / depth) / tanHalfFovY;
        if (std::fabs(ndcX) > 1.2f || std::fabs(ndcY) > 1.2f)
            return false;

        const float clampedX = std::clamp(ndcX, -0.96f, 0.96f);
        const float clampedY = std::clamp(ndcY, -0.96f, 0.96f);

        outX = static_cast<int>(std::lround((clampedX * 0.5f + 0.5f) * static_cast<float>(screenWidth)));
        outY = static_cast<int>(std::lround((0.5f - clampedY * 0.5f) * static_cast<float>(screenHeight)));
        return true;
    }

    struct ProjectedItemLabelVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };

    static constexpr DWORD kProjectedItemLabelFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static float GetProjectedItemLabelHoldSeconds(const VR* vr)
{
    if (!vr)
        return 0.90f;

    const float hz = (std::max)(1.0f, vr->m_ItemModelLabelMaxHz);
    return std::clamp(20.0f / hz, 0.75f, 1.50f);
}

static bool ShouldSuppressProjectedItemLabelForMotion(const VR::ProjectedItemLabel& label, const std::chrono::steady_clock::time_point& now)
{
    constexpr float kItemLabelStillSeconds = 3.0f;

    if (label.stableSince.time_since_epoch().count() == 0)
        return true;

    const float stableSeconds = std::chrono::duration<float>(now - label.stableSince).count();
    return stableSeconds >= 0.0f && stableSeconds < kItemLabelStillSeconds;
}

    static int SafeGetProjectedItemLabelHighestEntityIndex(VR* vr)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_ClientEntityList)
            return 0;
#ifdef _MSC_VER
        __try
        {
            return vr->m_Game->m_ClientEntityList->GetHighestEntityIndex();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
#else
        return vr->m_Game->m_ClientEntityList->GetHighestEntityIndex();
#endif
    }

    static C_BaseEntity* SafeGetProjectedItemLabelClientEntity(VR* vr, int entityIndex)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_ClientEntityList || entityIndex <= 0)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return static_cast<C_BaseEntity*>(vr->m_Game->m_ClientEntityList->GetClientEntity(entityIndex));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return static_cast<C_BaseEntity*>(vr->m_Game->m_ClientEntityList->GetClientEntity(entityIndex));
#endif
    }

    static const char* SafeGetProjectedItemLabelNetworkClassName(const VR* vr, const C_BaseEntity* entity)
    {
        if (!vr || !vr->m_Game || !entity)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return vr->m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return vr->m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
#endif
    }

    static bool SafeGetProjectedItemLabelAbsOrigin(const C_BaseEntity* entity, Vector& out)
    {
        out = Vector();
        if (!entity)
            return false;
#ifdef _MSC_VER
        __try
        {
            out = const_cast<C_BaseEntity*>(entity)->GetAbsOrigin();
            return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = Vector();
            return false;
        }
#else
        out = const_cast<C_BaseEntity*>(entity)->GetAbsOrigin();
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
#endif
    }

    static bool SafeGetProjectedItemLabelInt32(const C_BaseEntity* entity, int offset, int& out)
    {
        out = 0;
        if (!entity || offset < 0)
            return false;
        const auto* base = reinterpret_cast<const unsigned char*>(entity);
        return SafeReadInt(base, offset, out);
    }

    class IClientRenderableProjectedItemLabelProbe
    {
    public:
        virtual void* GetIClientUnknown() = 0;
        virtual const Vector& GetRenderOrigin() = 0;
        virtual const QAngle& GetRenderAngles() = 0;
        virtual bool ShouldDraw() = 0;
        virtual int GetRenderFlags() = 0;
        virtual void Unused() const = 0;
        virtual void* GetShadowHandle() const = 0;
        virtual void* RenderHandle() = 0;
        virtual void* GetModel() const = 0;
    };

    static bool SafeGetProjectedItemLabelRenderableOriginAndShouldDraw(
        VR* vr,
        const C_BaseEntity* entity,
        Vector& outOrigin,
        bool& outShouldDraw)
    {
        outOrigin = Vector();
        outShouldDraw = false;
        if (!vr || !vr->m_Game || !entity)
            return false;

        // DT_BaseEntity::m_fEffects. EF_NODRAW means the renderable should not be used as a label anchor.
        static constexpr int kProjectedItemLabelBaseEntityEffectsOffset = 0x0E0;
        static constexpr int kProjectedItemLabelEffectNoDraw = 0x020;
        int effects = 0;
        if (SafeGetProjectedItemLabelInt32(entity, kProjectedItemLabelBaseEntityEffectsOffset, effects) &&
            (effects & kProjectedItemLabelEffectNoDraw) != 0)
        {
            return false;
        }

#ifdef _MSC_VER
        __try
        {
            void* renderableVoid = const_cast<C_BaseEntity*>(entity)->GetClientRenderable();
            if (!renderableVoid)
                return false;

            auto* renderable = reinterpret_cast<IClientRenderableProjectedItemLabelProbe*>(renderableVoid);
            outShouldDraw = renderable->ShouldDraw();
            const Vector& renderOrigin = renderable->GetRenderOrigin();
            outOrigin = renderOrigin;
            return std::isfinite(outOrigin.x) && std::isfinite(outOrigin.y) && std::isfinite(outOrigin.z);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outOrigin = Vector();
            outShouldDraw = false;
            return false;
        }
#else
        void* renderableVoid = const_cast<C_BaseEntity*>(entity)->GetClientRenderable();
        if (!renderableVoid)
            return false;

        auto* renderable = reinterpret_cast<IClientRenderableProjectedItemLabelProbe*>(renderableVoid);
        outShouldDraw = renderable->ShouldDraw();
        const Vector& renderOrigin = renderable->GetRenderOrigin();
        outOrigin = renderOrigin;
        return std::isfinite(outOrigin.x) && std::isfinite(outOrigin.y) && std::isfinite(outOrigin.z);
#endif
    }

    static bool IsProjectedItemLabelAlivePlayer(VR* vr, const C_BaseEntity* entity)
    {
        if (!vr || !vr->m_Game || !entity)
            return false;

        const char* playerClass = SafeGetProjectedItemLabelNetworkClassName(vr, entity);
        if (!playerClass)
            return false;

        if (std::strcmp(playerClass, "CTerrorPlayer") != 0 &&
            std::strcmp(playerClass, "C_TerrorPlayer") != 0)
        {
            return false;
        }

        return vr->IsEntityAlive(entity);
    }

    static bool ShouldSuppressProjectedItemLabelNearPlayer(VR* vr, const Vector& worldPos)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_ClientEntityList)
            return false;

        const float suppressRadius = std::max(0.0f, vr->m_ItemModelLabelPlayerSuppressRadius);
        if (suppressRadius <= 0.0f)
            return false;

        const float suppressMinZ = (std::min)(vr->m_ItemModelLabelPlayerSuppressMinZ, vr->m_ItemModelLabelPlayerSuppressMaxZ);
        const float suppressMaxZ = (std::max)(vr->m_ItemModelLabelPlayerSuppressMinZ, vr->m_ItemModelLabelPlayerSuppressMaxZ);
        const float suppressRadiusSq = suppressRadius * suppressRadius;
        const auto isWithinSuppressVolume = [&](const Vector& anchor, float radiusSq, float minZ, float maxZ) -> bool
            {
                const Vector delta = worldPos - anchor;
                const float horizontalDistSq = delta.x * delta.x + delta.y * delta.y;
                return horizontalDistSq <= radiusSq && delta.z >= minZ && delta.z <= maxZ;
            };

        const int localPlayerIndex =
            (vr->m_Game->m_EngineClient != nullptr) ? vr->m_Game->m_EngineClient->GetLocalPlayer() : -1;
        const int maxEntityIndex = (std::min)(32, SafeGetProjectedItemLabelHighestEntityIndex(vr));
        for (int entityIndex = 1; entityIndex <= maxEntityIndex; ++entityIndex)
        {
            C_BaseEntity* player = SafeGetProjectedItemLabelClientEntity(vr, entityIndex);
            if (!player || !IsProjectedItemLabelAlivePlayer(vr, player))
                continue;

            Vector playerOrigin{};
            bool playerShouldDraw = false;
            const bool hasRenderableOrigin =
                SafeGetProjectedItemLabelRenderableOriginAndShouldDraw(vr, player, playerOrigin, playerShouldDraw);
            if (!hasRenderableOrigin && !SafeGetProjectedItemLabelAbsOrigin(player, playerOrigin))
                continue;

            if (isWithinSuppressVolume(playerOrigin, suppressRadiusSq, suppressMinZ, suppressMaxZ))
            {
                return true;
            }

            if (entityIndex == localPlayerIndex)
            {
                const float localRadius = (std::min)(48.0f, suppressRadius);
                if (localRadius > 0.0f)
                {
                    const float localRadiusSq = localRadius * localRadius;
                    if (isWithinSuppressVolume(vr->m_HmdPosAbs, localRadiusSq, -48.0f, 48.0f))
                        return true;
                }
            }
        }

        return false;
    }

    static int EstimateProjectedItemLabelWidth(const std::string& text, int fontPx)
    {
        const int units = (std::max)(1, Utf8HudUnits(text.c_str()));
        const int approx = static_cast<int>(std::lround(static_cast<float>(units) * static_cast<float>(fontPx) * 0.58f)) + 14;
        return (std::max)(48, approx);
    }

    static int EstimateProjectedItemLabelHeight(int fontPx, bool hasNonAscii)
    {
        return fontPx + (hasNonAscii ? 12 : 10);
    }

    static int QuantizeProjectedItemLabelFontPx(int fontPx)
    {
        if (fontPx <= 12)
            return fontPx;
        return ((fontPx + 1) / 2) * 2;
    }

    static std::string BuildProjectedItemLabelCacheKey(const std::string& text, int fontPx, const Rgba& color)
    {
        std::string key;
        key.reserve(text.size() + 32);
        key.append(std::to_string(fontPx));
        key.push_back('|');
        key.append(std::to_string(static_cast<int>(color.r)));
        key.push_back(',');
        key.append(std::to_string(static_cast<int>(color.g)));
        key.push_back(',');
        key.append(std::to_string(static_cast<int>(color.b)));
        key.push_back(',');
        key.append(std::to_string(static_cast<int>(color.a)));
        key.push_back('|');
        key.append(text);
        return key;
    }

    static bool GetProjectedItemLabelColor(VR::ItemModelLabelCategory category, Rgba& outColor)
    {
        switch (category)
        {
        case VR::ItemModelLabelCategory::Firearm:
            outColor = { 255, 220, 96, 255 };
            return true;
        case VR::ItemModelLabelCategory::Melee:
            outColor = { 255, 150, 72, 255 };
            return true;
        case VR::ItemModelLabelCategory::Throwable:
            outColor = { 96, 255, 180, 255 };
            return true;
        case VR::ItemModelLabelCategory::MedicalPack:
            outColor = { 255, 96, 96, 255 };
            return true;
        case VR::ItemModelLabelCategory::Medicine:
            outColor = { 96, 180, 255, 255 };
            return true;
        default:
            break;
        }

        return false;
    }


}

bool VR::ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial)
{
    return ReadLocalKillCounters(localPlayer, outCommon, outSpecial, nullptr);
}

bool VR::ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial, char* outSource)
{
    outCommon = 0;
    outSpecial = 0;
    if (outSource)
        *outSource = 'N';

    if (!localPlayer)
        return false;

    const auto* base = reinterpret_cast<const unsigned char*>(localPlayer);
    int localTeam = 0;
    if (!VR_TryReadI32(base, kTeamNumOffset, localTeam) || localTeam != 2)
        return false;

    auto readKillsArray = [&](int baseOff, int& common, int& special) -> bool
        {
            int value = 0;
            bool okAny = false;

            common = 0;
            special = 0;

            if (VR_TryReadI32(base, baseOff + 0 * 4, value))
            {
                common = (std::max)(0, value);
                okAny = true;
            }

            int specialSum = 0;
            for (int i = 1; i <= kZombieKillsMaxIndex; ++i)
            {
                if (VR_TryReadI32(base, baseOff + i * 4, value))
                {
                    specialSum += (std::max)(0, value);
                    okAny = true;
                }
            }

            special = specialSum;
            return okAny;
        };

    int missionCommon = 0;
    int missionSpecial = 0;
    int checkpointCommon = 0;
    int checkpointSpecial = 0;
    const bool missionOk = readKillsArray(kMissionZombieKillsOffset, missionCommon, missionSpecial);
    const bool checkpointOk = readKillsArray(kCheckpointZombieKillsOffset, checkpointCommon, checkpointSpecial);

    if (!missionOk && !checkpointOk)
        return false;

    const int missionSum = missionCommon + missionSpecial;
    const int checkpointSum = checkpointCommon + checkpointSpecial;

    const int prevMissionSum = m_LastKillCounterMissionSum;
    const int prevCheckpointSum = m_LastKillCounterCheckpointSum;
    const char prevSource = m_KillCounterPreferredSource;
    const bool missionReset = missionOk && prevMissionSum >= 0 && missionSum < prevMissionSum;
    const bool checkpointReset = checkpointOk && prevCheckpointSum >= 0 && checkpointSum < prevCheckpointSum;
    const bool missionAdvanced = missionOk && prevMissionSum >= 0 && missionSum > prevMissionSum;
    const bool checkpointAdvanced = checkpointOk && prevCheckpointSum >= 0 && checkpointSum > prevCheckpointSum;

    bool useCheckpoint = checkpointOk && !missionOk;
    if (missionOk && checkpointOk)
    {
        // Team-wipe retry can reset m_checkpointZombieKills while m_missionZombieKills
        // keeps the abandoned attempt's total. Stay on checkpoint after that reset so
        // kill feedback and the wrist HUD don't stay pinned until the next map load.
        if (checkpointReset && !missionReset)
        {
            useCheckpoint = true;
        }
        else if (missionReset && !checkpointReset)
        {
            useCheckpoint = false;
        }
        else if (prevSource == 'C')
        {
            // Some servers only move one source. If checkpoint stops moving but mission
            // advances, switch back; otherwise keep checkpoint once it was chosen.
            useCheckpoint = !(missionAdvanced && !checkpointAdvanced);
        }
        else
        {
            // Preserve the existing map-transition fallback: stale mission values are
            // common, and checkpoint is the fresher source once it overtakes mission.
            useCheckpoint = checkpointSum > missionSum;
        }
    }

    const char selectedSource = useCheckpoint ? 'C' : 'M';
    if (m_FeedbackSoundDebugLog && prevSource != 'N' && selectedSource != prevSource
        && !ShouldThrottle(m_LastFeedbackSoundDebugLogTime, m_FeedbackSoundDebugLogHz))
    {
        Game::logMsg(
            "[VR][KillSound][counter-source] %c->%c mission=%d(prev=%d reset=%d adv=%d) checkpoint=%d(prev=%d reset=%d adv=%d)",
            prevSource,
            selectedSource,
            missionSum,
            prevMissionSum,
            missionReset ? 1 : 0,
            missionAdvanced ? 1 : 0,
            checkpointSum,
            prevCheckpointSum,
            checkpointReset ? 1 : 0,
            checkpointAdvanced ? 1 : 0);
    }

    m_LastKillCounterMissionSum = missionOk ? missionSum : -1;
    m_LastKillCounterCheckpointSum = checkpointOk ? checkpointSum : -1;
    m_KillCounterPreferredSource = selectedSource;

    if (useCheckpoint)
    {
        outCommon = checkpointCommon;
        outSpecial = checkpointSpecial;
        if (outSource)
            *outSource = 'C';
        return true;
    }

    if (missionOk)
    {
        outCommon = missionCommon;
        outSpecial = missionSpecial;
        if (outSource)
            *outSource = 'M';
        return true;
    }

    outCommon = checkpointCommon;
    outSpecial = checkpointSpecial;
    if (outSource)
        *outSource = 'C';
    return true;
}

bool VR::ReadLocalHeadshotCounter(C_BasePlayer* localPlayer, int& outHeadshots) const
{
    outHeadshots = 0;

    if (!localPlayer)
        return false;

    const auto* base = reinterpret_cast<const unsigned char*>(localPlayer);
    int localTeam = 0;
    if (!VR_TryReadI32(base, kTeamNumOffset, localTeam) || localTeam != 2)
        return false;

    int value = 0;
    if (!VR_TryReadI32(base, kCheckpointHeadshotsOffset, value))
        return false;

    outHeadshots = (std::max)(0, value);
    return true;
}

void VR::TriggerImpactHapticsBothHands(float amplitude, float frequency, float durationSeconds, int priority)
{
    const float amp = std::clamp(amplitude, 0.0f, 1.0f);
    if (amp <= 0.0f)
        return;

    TriggerPhysicalHandHapticPulse(true, durationSeconds, frequency, amp, priority);
    TriggerPhysicalHandHapticPulse(false, durationSeconds, frequency, amp, priority);
}

void VR::TriggerDirectionalDamageHaptics(float amplitude, float frequency, float durationSeconds, float rightBias, int priority)
{
    const float amp = std::clamp(amplitude * std::clamp(m_DamageFeedbackOverallScale, 0.0f, 1.0f), 0.0f, 1.0f);
    if (amp <= 0.0f)
        return;

    float bias = std::clamp(rightBias, -1.0f, 1.0f);
    if (bias != 0.0f)
    {
        // Sharpen mid-strength directional samples so they do not collapse into a near-even
        // two-hand buzz. This keeps the dominant hand clearly stronger without requiring
        // a perfectly side-on attacker position.
        bias = std::copysign(std::pow(std::abs(bias), 0.65f), bias);
    }

    constexpr float kOppositeHandFloor = 0.05f;
    const float rightBlend = (bias + 1.0f) * 0.5f;
    const float leftWeight = std::clamp(1.0f - (1.0f - kOppositeHandFloor) * rightBlend, kOppositeHandFloor, 1.0f);
    const float rightWeight = std::clamp(kOppositeHandFloor + (1.0f - kOppositeHandFloor) * rightBlend, kOppositeHandFloor, 1.0f);
    TriggerPhysicalHandHapticPulse(true, durationSeconds, frequency, amp * leftWeight, priority);
    TriggerPhysicalHandHapticPulse(false, durationSeconds, frequency, amp * rightWeight, priority);
}

void VR::UpdateMeleeHitHaptics()
{
    if (!m_IsVREnabled || !m_WeaponHapticsEnabled || !m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
    {
        m_LastMeleeHitHapticsTriggerTime = {};
        m_LastMeleeHitHapticsEntityTag = 0;
        return;
    }

    EnsureMeleeHitHapticsEventListener();
}

void VR::EnsureMeleeHitHapticsEventListener()
{
    if (m_MeleeHitHapticsEventListenerRegistered || !m_Game)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_LastMeleeHitHapticsEventRegisterAttempt.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastMeleeHitHapticsEventRegisterAttempt).count();
        if (elapsed < 2.0f)
            return;
    }
    m_LastMeleeHitHapticsEventRegisterAttempt = now;

    if (!m_MeleeHitHapticsEventManager)
        m_MeleeHitHapticsEventManager = m_Game->m_GameEventManager;

    if (!m_MeleeHitHapticsEventManager)
    {
        HMODULE engineModule = GetModuleHandleA("engine.dll");
        if (engineModule)
        {
            using tCreateInterface = void* (__cdecl*)(const char* name, int* returnCode);
            const auto createInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(engineModule, "CreateInterface"));
            if (createInterface)
            {
                int returnCode = 0;
                void* iface = createInterface("GAMEEVENTSMANAGER002", &returnCode);
                if (!iface)
                    iface = createInterface("GAMEEVENTSMANAGER001", &returnCode);
                m_MeleeHitHapticsEventManager = static_cast<IGameEventManager2*>(iface);
                if (m_MeleeHitHapticsEventManager && !m_Game->m_GameEventManager)
                    m_Game->m_GameEventManager = m_MeleeHitHapticsEventManager;
            }
        }
    }

    if (!m_MeleeHitHapticsEventManager)
        return;

    if (!m_MeleeHitHapticsEventListener)
        m_MeleeHitHapticsEventListener = new VRMeleeHitHapticsEventListener(this);

    static constexpr const char* kMeleeHitEvents[] = {
        "infected_hurt",
        "player_hurt",
        "witch_hurt",
        "infected_death",
        "player_death",
        "witch_killed"
    };

    bool registeredAny = false;
    for (const char* eventName : kMeleeHitEvents)
    {
        const bool alreadyRegistered = m_MeleeHitHapticsEventManager->FindListener(m_MeleeHitHapticsEventListener, eventName);
        const bool registered = alreadyRegistered || m_MeleeHitHapticsEventManager->AddListener(m_MeleeHitHapticsEventListener, eventName, false);
        registeredAny = registeredAny || registered;
    }

    m_MeleeHitHapticsEventListenerRegistered = registeredAny;
}

void VR::HandleMeleeHitHapticsGameEvent(IGameEvent* event)
{
    if (!event || !m_WeaponHapticsEnabled || !m_IsVREnabled || !m_Game || !m_Game->m_EngineClient)
        return;

    const char* rawEventName = event->GetName();
    if (!rawEventName || !*rawEventName)
        return;

    const std::string eventName = ToLowerCopy(rawEventName);
    const bool isCommonInfectedEvent = (eventName == "infected_hurt" || eventName == "infected_death");
    const bool isWitchEvent = (eventName == "witch_hurt" || eventName == "witch_killed");
    const bool isPlayerInfectedEvent = (eventName == "player_hurt" || eventName == "player_death");
    if (!isCommonInfectedEvent && !isWitchEvent && !isPlayerInfectedEvent)
        return;

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    if (localPlayerIndex <= 0)
        return;

    const int localUserId = GetLocalPlayerUserId(m_Game);
    int attackerUserId = event->GetInt("attacker", 0);
    if (attackerUserId <= 0)
        attackerUserId = event->GetInt("userid", 0);
    if (attackerUserId <= 0)
        attackerUserId = event->GetInt("attackeruserid", 0);

    int attackerIndex = 0;
    if (attackerUserId > 0)
        attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
    if (attackerIndex <= 0)
        attackerIndex = event->GetInt("attackerentid", 0);
    if (attackerIndex <= 0)
        attackerIndex = event->GetInt("attackerentityid", 0);
    if (attackerIndex <= 0)
        attackerIndex = event->GetInt("attackerid", 0);

    const bool attackerMatchesLocalUser = localUserId > 0 && attackerUserId == localUserId;
    const bool attackerMatchesLocalIndex = attackerIndex > 0 && attackerIndex == localPlayerIndex;
    if (!attackerMatchesLocalUser && !attackerMatchesLocalIndex)
        return;

    bool activeWeaponIsMelee = false;
    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(SafeGetProjectedItemLabelClientEntity(this, localPlayerIndex));
    activeWeaponIsMelee = IsActiveWeaponMeleeSafe(localPlayer);

    const bool eventWeaponIsMelee = IsMeleeHitWeaponName(event->GetString("weapon", ""));
    if (!eventWeaponIsMelee && !activeWeaponIsMelee)
        return;

    const std::uintptr_t entityTag = ResolveKillEventEntityTag(m_Game, event, eventName);
    bool monsterHit = isCommonInfectedEvent || isWitchEvent;
    if (isPlayerInfectedEvent)
    {
        C_BaseEntity* victim = reinterpret_cast<C_BaseEntity*>(entityTag);
        if (!victim)
            return;

        const unsigned char* base = reinterpret_cast<const unsigned char*>(victim);
        int team = 0;
        const bool hasTeam = VR_TryReadI32(base, kTeamNumOffset, team);
        if (hasTeam && team == 2)
            return;

        const SpecialInfectedType specialType = GetSpecialInfectedType(victim);
        monsterHit = (hasTeam && team == 3) || specialType != SpecialInfectedType::None;
    }

    if (!monsterHit)
        return;

    NotifyMeleeHitConfirmed(entityTag);
}

VR::DamageFeedbackType VR::ClassifyDamageFeedbackType(const char* weaponName, int damage) const
{
    const std::string weapon = weaponName ? ToLowerCopy(weaponName) : std::string();

    if (weapon.find("spit") != std::string::npos || weapon.find("acid") != std::string::npos || weapon.find("insect_swarm") != std::string::npos)
        return DamageFeedbackType::Acid;
    if (weapon.find("fire") != std::string::npos || weapon.find("burn") != std::string::npos || weapon.find("inferno") != std::string::npos || weapon.find("molotov") != std::string::npos)
        return DamageFeedbackType::Fire;
    if (weapon.find("explosion") != std::string::npos || weapon.find("grenade") != std::string::npos || weapon.find("blast") != std::string::npos || weapon.find("pipe_bomb") != std::string::npos || weapon.find("propane") != std::string::npos)
        return DamageFeedbackType::Explosion;
    if (weapon.find("tank") != std::string::npos || weapon.find("charger") != std::string::npos || weapon.find("rock") != std::string::npos)
        return DamageFeedbackType::HeavyHit;
    if (weapon.find("claw") != std::string::npos || weapon.find("hunter") != std::string::npos || weapon.find("jockey") != std::string::npos || weapon.find("smoker") != std::string::npos || weapon.find("boomer") != std::string::npos || weapon.find("spitter") != std::string::npos)
        return DamageFeedbackType::SpecialHit;

    if (damage >= 22)
        return DamageFeedbackType::HeavyHit;
    if (damage >= 8)
        return DamageFeedbackType::SpecialHit;
    return DamageFeedbackType::CommonHit;
}

WeaponHapticsProfile VR::GetDamageHapticsProfile(DamageFeedbackType type) const
{
    switch (type)
    {
    case DamageFeedbackType::CommonHit: return m_DamageCommonHapticsProfile;
    case DamageFeedbackType::SpecialHit: return m_DamageSpecialHapticsProfile;
    case DamageFeedbackType::HeavyHit: return m_DamageHeavyHapticsProfile;
    case DamageFeedbackType::Explosion: return m_DamageExplosionHapticsProfile;
    case DamageFeedbackType::Fire: return m_DamageFireHapticsProfile;
    case DamageFeedbackType::Acid: return m_DamageAcidHapticsProfile;
    default: return m_DamageCommonHapticsProfile;
    }
}

VR::ControlledDamageState VR::ResolveLocalSpecialControlState(C_BasePlayer* localPlayer, C_BaseEntity** outControllerEntity) const
{
    if (outControllerEntity)
        *outControllerEntity = nullptr;

    if (!localPlayer || !m_Game || !m_Game->m_ClientEntityList)
        return ControlledDamageState::None;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    auto resolveHandle = [&](int offset) -> C_BaseEntity*
        {
            uint32_t handle = 0u;
            if (!VR_TryReadU32(base, offset, handle) || handle == 0u || handle == 0xFFFFFFFFu)
                return nullptr;
            return reinterpret_cast<C_BaseEntity*>(m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle));
        };

    if (C_BaseEntity* attacker = resolveHandle(kPummelAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::ChargerPummel;
    }

    if (C_BaseEntity* attacker = resolveHandle(kCarryAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::ChargerCarry;
    }

    if (C_BaseEntity* attacker = resolveHandle(kPounceAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::HunterPounce;
    }

    if (C_BaseEntity* attacker = resolveHandle(kJockeyAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::JockeyRide;
    }

    if (C_BaseEntity* attacker = resolveHandle(kTongueOwnerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::SmokerTongue;
    }

    return ControlledDamageState::None;
}

WeaponHapticsProfile VR::GetControlledDamageHapticsProfile(ControlledDamageState state) const
{
    switch (state)
    {
    case ControlledDamageState::SmokerTongue:  return m_DamageSmokerControlHapticsProfile;
    case ControlledDamageState::HunterPounce:  return m_DamageHunterControlHapticsProfile;
    case ControlledDamageState::JockeyRide:    return m_DamageJockeyControlHapticsProfile;
    case ControlledDamageState::ChargerCarry:  return m_DamageChargerCarryHapticsProfile;
    case ControlledDamageState::ChargerPummel: return m_DamageChargerPummelHapticsProfile;
    default:                                   return m_DamageSpecialHapticsProfile;
    }
}

float VR::GetControlledDamageDirectionalBiasScale(ControlledDamageState state) const
{
    switch (state)
    {
    case ControlledDamageState::SmokerTongue:  return m_DamageSmokerControlDirectionalBiasScale;
    case ControlledDamageState::HunterPounce:  return m_DamageHunterControlDirectionalBiasScale;
    case ControlledDamageState::JockeyRide:    return m_DamageJockeyControlDirectionalBiasScale;
    case ControlledDamageState::ChargerCarry:  return m_DamageChargerCarryDirectionalBiasScale;
    case ControlledDamageState::ChargerPummel: return m_DamageChargerPummelDirectionalBiasScale;
    default:                                   return 1.00f;
    }
}

void VR::ParseHapticsConfigFile()
{
    std::ifstream configStream("VR\\haptics_config.txt");
    if (!configStream)
        return;

    auto trim = [](std::string& s)
        {
            auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char ch) { return !isSpace(ch); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), s.end());
        };

    std::unordered_map<std::string, std::string> userConfig;
    std::string line;
    while (std::getline(configStream, line))
    {
        size_t cut = std::string::npos;
        const size_t p1 = line.find("//");
        const size_t p2 = line.find('#');
        const size_t p3 = line.find(';');
        if (p1 != std::string::npos) cut = p1;
        if (p2 != std::string::npos) cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
        if (p3 != std::string::npos) cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
        if (cut != std::string::npos)
            line.erase(cut);

        trim(line);
        if (line.empty())
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);
        if (key.empty())
            continue;

        userConfig[ToLowerCopy(key)] = value;
    }

    auto getBool = [&](const char* key, bool defVal) -> bool
        {
            auto it = userConfig.find(ToLowerCopy(key));
            if (it == userConfig.end())
                return defVal;
            std::string value = ToLowerCopy(TrimCopy(it->second));
            if (value == "1" || value == "true" || value == "on" || value == "yes")
                return true;
            if (value == "0" || value == "false" || value == "off" || value == "no")
                return false;
            return defVal;
        };

    auto getFloat = [&](const char* key, float defVal) -> float
        {
            auto it = userConfig.find(ToLowerCopy(key));
            if (it == userConfig.end())
                return defVal;
            try
            {
                return std::stof(TrimCopy(it->second));
            }
            catch (...)
            {
                return defVal;
            }
        };

    auto parseProfile = [&](const std::string& key, const WeaponHapticsProfile& defaults) -> WeaponHapticsProfile
        {
            auto it = userConfig.find(ToLowerCopy(key));
            if (it == userConfig.end())
                return defaults;

            WeaponHapticsProfile profile = defaults;
            std::stringstream ss(it->second);
            std::string token;
            float* values[3] = { &profile.durationSeconds, &profile.frequency, &profile.amplitude };
            int index = 0;
            while (std::getline(ss, token, ',') && index < 3)
            {
                token = TrimCopy(token);
                if (!token.empty())
                {
                    try
                    {
                        *values[index] = std::stof(token);
                    }
                    catch (...)
                    {
                    }
                }
                ++index;
            }
            profile.durationSeconds = std::clamp(profile.durationSeconds, 0.0f, 0.5f);
            profile.frequency = std::clamp(profile.frequency, 0.0f, 320.0f);
            profile.amplitude = std::clamp(profile.amplitude, 0.0f, 1.0f);
            return profile;
        };

    auto setWeaponOverride = [&](const char* weaponKey, const WeaponHapticsProfile& defaults)
        {
            const std::string key = std::string("weapon.") + weaponKey;
            m_WeaponHapticsOverrides[weaponKey] = parseProfile(key, defaults);
        };

    m_WeaponHapticsEnabled = getBool("weapon.enabled", m_WeaponHapticsEnabled);
    m_HapticMixMinIntervalSeconds = std::max(0.0f, getFloat("mix.min_interval", m_HapticMixMinIntervalSeconds));
    m_DefaultWeaponHapticsProfile = parseProfile("weapon.default", m_DefaultWeaponHapticsProfile);
    m_MeleeSwingHapticsProfile = parseProfile("melee.swing", m_MeleeSwingHapticsProfile);
    m_ShoveHapticsProfile = parseProfile("melee.shove", m_ShoveHapticsProfile);

    m_DamageFeedbackEnabled = getBool("damage.enabled", true);
    m_DamageDirectionalEnabled = getBool("damage.directional", true);
    m_DamageFeedbackMergeWindowSeconds = std::max(0.0f, getFloat("damage.merge_window", m_DamageFeedbackMergeWindowSeconds));
    m_DamageFeedbackMinTriggerIntervalSeconds = std::max(0.0f, getFloat("damage.min_trigger_interval", m_DamageFeedbackMinTriggerIntervalSeconds));
    m_DamageFeedbackMergedHitBonus = std::max(0.0f, getFloat("damage.merged_hit_bonus", m_DamageFeedbackMergedHitBonus));
    m_DamageScaleStart = std::max(0.0f, getFloat("damage.scale_start", m_DamageScaleStart));
    m_DamageScalePerPoint = std::max(0.0f, getFloat("damage.scale_per_point", m_DamageScalePerPoint));
    m_DamageScaleMaxBonus = std::max(0.0f, getFloat("damage.scale_max_bonus", m_DamageScaleMaxBonus));
    m_DamageAmplitudeMin = std::clamp(getFloat("damage.amplitude_min", m_DamageAmplitudeMin), 0.0f, 1.0f);
    m_DamageAmplitudeMax = std::clamp(getFloat("damage.amplitude_max", m_DamageAmplitudeMax), 0.0f, 1.0f);
    if (m_DamageAmplitudeMax < m_DamageAmplitudeMin)
        std::swap(m_DamageAmplitudeMin, m_DamageAmplitudeMax);

    m_DamageCommonHapticsProfile = parseProfile("damage.common", m_DamageCommonHapticsProfile);
    m_DamageSpecialHapticsProfile = parseProfile("damage.special", m_DamageSpecialHapticsProfile);
    m_DamageHeavyHapticsProfile = parseProfile("damage.heavy", m_DamageHeavyHapticsProfile);
    m_DamageExplosionHapticsProfile = parseProfile("damage.explosion", m_DamageExplosionHapticsProfile);
    m_DamageFireHapticsProfile = parseProfile("damage.fire", m_DamageFireHapticsProfile);
    m_DamageAcidHapticsProfile = parseProfile("damage.acid", m_DamageAcidHapticsProfile);
    m_DamageSmokerControlHapticsProfile = parseProfile("damage.control.smoker", m_DamageSmokerControlHapticsProfile);
    m_DamageHunterControlHapticsProfile = parseProfile("damage.control.hunter", m_DamageHunterControlHapticsProfile);
    m_DamageJockeyControlHapticsProfile = parseProfile("damage.control.jockey", m_DamageJockeyControlHapticsProfile);
    m_DamageChargerCarryHapticsProfile = parseProfile("damage.control.charger_carry", m_DamageChargerCarryHapticsProfile);
    m_DamageChargerPummelHapticsProfile = parseProfile("damage.control.charger_pummel", m_DamageChargerPummelHapticsProfile);

    m_DamageSmokerControlDirectionalBiasScale = std::clamp(getFloat("damage.control.smoker.directional_bias", m_DamageSmokerControlDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageHunterControlDirectionalBiasScale = std::clamp(getFloat("damage.control.hunter.directional_bias", m_DamageHunterControlDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageJockeyControlDirectionalBiasScale = std::clamp(getFloat("damage.control.jockey.directional_bias", m_DamageJockeyControlDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageChargerCarryDirectionalBiasScale = std::clamp(getFloat("damage.control.charger_carry.directional_bias", m_DamageChargerCarryDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageChargerPummelDirectionalBiasScale = std::clamp(getFloat("damage.control.charger_pummel.directional_bias", m_DamageChargerPummelDirectionalBiasScale), 0.0f, 1.0f);

    m_LandingHapticsEnabled = getBool("landing.enabled", m_LandingHapticsEnabled);
    m_LandingMinAirTime = std::max(0.0f, getFloat("landing.min_air_time", m_LandingMinAirTime));
    m_LandingMinDownwardSpeed = std::max(0.0f, getFloat("landing.min_down_speed", m_LandingMinDownwardSpeed));
    m_LandingMediumHapticsProfile = parseProfile("landing.medium", m_LandingMediumHapticsProfile);
    m_LandingDamageHapticsProfile = parseProfile("landing.damage", m_LandingDamageHapticsProfile);

    setWeaponOverride("pistol", { 0.018f, 165.0f, 0.33f });
    setWeaponOverride("magnum", { 0.032f, 85.0f, 0.66f });
    setWeaponOverride("uzi", { 0.012f, 185.0f, 0.23f });
    setWeaponOverride("mac10", { 0.011f, 195.0f, 0.24f });
    setWeaponOverride("mp5", { 0.012f, 190.0f, 0.26f });
    setWeaponOverride("m16a1", { 0.015f, 145.0f, 0.34f });
    setWeaponOverride("ak47", { 0.020f, 120.0f, 0.44f });
    setWeaponOverride("scar", { 0.017f, 135.0f, 0.39f });
    setWeaponOverride("sg552", { 0.018f, 130.0f, 0.40f });
    setWeaponOverride("pumpshotgun", { 0.040f, 72.0f, 0.78f });
    setWeaponOverride("shotgun_chrome", { 0.042f, 70.0f, 0.80f });
    setWeaponOverride("autoshotgun", { 0.030f, 78.0f, 0.65f });
    setWeaponOverride("spas", { 0.029f, 82.0f, 0.62f });
    setWeaponOverride("hunting_rifle", { 0.038f, 88.0f, 0.72f });
    setWeaponOverride("sniper_military", { 0.033f, 92.0f, 0.61f });
    setWeaponOverride("scout", { 0.036f, 96.0f, 0.69f });
    setWeaponOverride("awp", { 0.052f, 62.0f, 0.94f });
    setWeaponOverride("m60", { 0.019f, 115.0f, 0.50f });
    setWeaponOverride("grenade_launcher", { 0.060f, 55.0f, 1.00f });
    setWeaponOverride("melee", { 0.028f, 105.0f, 0.54f });
    setWeaponOverride("chainsaw", { 0.014f, 175.0f, 0.34f });

    // Direct damage-event haptics remain configurable here.
    // Sustained acid/fire and camera-shake haptics still stay off until their old branches are rebuilt,
    // but landing haptics are now active again and use raw netvar offsets to detect the airborne->landed transition.
    m_DamageSustainEnabled = false;
    m_CameraShakeHapticsEnabled = false;
    m_AcidSustainUntil = {};
    m_FireSustainUntil = {};
    m_NextAcidSustainPulse = {};
    m_NextFireSustainPulse = {};
    m_LastDamageFeedbackEventRegisterAttempt = {};
    m_LastDamageFeedbackEventSeenTime = {};
    m_LastObservedDamageHealth = -1;
    m_LastCameraShakeHapticsPulse = {};
    m_LandingAirborneSince = {};
    m_WasOnGroundForHaptics = true;
    m_LastVerticalSpeedForHaptics = 0.0f;
    m_LandingPeakDownwardSpeedForHaptics = 0.0f;
    m_LastAirborneHealthForHaptics = -1;
    m_CameraShakeStateInitialized = false;
    m_LastCameraShakeOrigin = { 0, 0, 0 };
    m_LastCameraShakeAngles = { 0, 0, 0 };
}

void VR::EnsureDamageFeedbackEventListener()
{
    if (m_DamageFeedbackEventListenerRegistered || !m_Game || !m_DamageFeedbackEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_LastDamageFeedbackEventRegisterAttempt.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastDamageFeedbackEventRegisterAttempt).count();
        if (elapsed < 2.0f)
            return;
    }
    m_LastDamageFeedbackEventRegisterAttempt = now;

    if (!m_DamageFeedbackEventManager)
        m_DamageFeedbackEventManager = m_Game->m_GameEventManager;

    if (!m_DamageFeedbackEventManager)
    {
        HMODULE engineModule = GetModuleHandleA("engine.dll");
        if (engineModule)
        {
            using tCreateInterface = void* (__cdecl*)(const char* name, int* returnCode);
            const auto createInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(engineModule, "CreateInterface"));
            if (createInterface)
            {
                int returnCode = 0;
                void* iface = createInterface("GAMEEVENTSMANAGER002", &returnCode);
                if (!iface)
                    iface = createInterface("GAMEEVENTSMANAGER001", &returnCode);
                m_DamageFeedbackEventManager = static_cast<IGameEventManager2*>(iface);
                if (m_DamageFeedbackEventManager && !m_Game->m_GameEventManager)
                    m_Game->m_GameEventManager = m_DamageFeedbackEventManager;
            }
        }
    }

    if (!m_DamageFeedbackEventManager)
    {
        return;
    }

    if (!m_DamageFeedbackEventListener)
        m_DamageFeedbackEventListener = new VRDamageFeedbackEventListener(this);

    static constexpr const char* kDamageEvents[] = { "player_hurt" };
    bool registeredAll = true;
    for (const char* eventName : kDamageEvents)
    {
        const bool alreadyRegistered = m_DamageFeedbackEventManager->FindListener(m_DamageFeedbackEventListener, eventName);
        const bool registered = alreadyRegistered || m_DamageFeedbackEventManager->AddListener(m_DamageFeedbackEventListener, eventName, false);
        registeredAll = registeredAll && registered;
    }

    m_DamageFeedbackEventListenerRegistered = registeredAll;
}

void VR::HandleDamageFeedbackGameEvent(IGameEvent* event)
{
    if (!event || !m_DamageFeedbackEnabled || !m_Game || !m_Game->m_EngineClient || !m_IsVREnabled)
        return;

    const char* eventNameRaw = event->GetName();
    if (!eventNameRaw || !*eventNameRaw)
        return;

    const std::string eventName = ToLowerCopy(eventNameRaw);
    if (eventName != "player_hurt")
        return;

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    if (localPlayerIndex <= 0)
        return;

    const int victimUserId = event->GetInt("userid", 0);
    int victimIndex = 0;
    if (victimUserId > 0)
        victimIndex = m_Game->m_EngineClient->GetPlayerForUserID(victimUserId);
    if (victimIndex <= 0)
        victimIndex = event->GetInt("victimentid", 0);
    if (victimIndex <= 0)
        victimIndex = event->GetInt("entityid", 0);
    if (victimIndex <= 0)
        victimIndex = event->GetInt("entindex", 0);

    bool eventIsForLocalPlayer = victimIndex > 0 && victimIndex == localPlayerIndex;
    if (!eventIsForLocalPlayer)
    {
        const int localUserId = GetLocalPlayerUserId(m_Game);
        eventIsForLocalPlayer = localUserId > 0 && victimUserId > 0 && victimUserId == localUserId;
    }
    if (!eventIsForLocalPlayer)
        return;

    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(SafeGetProjectedItemLabelClientEntity(this, localPlayerIndex));
    if (!localPlayer)
        return;

    m_LastDamageFeedbackEventSeenTime = std::chrono::steady_clock::now();

    const int damage = (std::max)(1, event->GetInt("dmg_health", event->GetInt("amount", 1)));
    const char* weapon = event->GetString("weapon", "");
    const DamageFeedbackType baseType = ClassifyDamageFeedbackType(weapon, damage);

    C_BaseEntity* controllerEntity = nullptr;
    const ControlledDamageState controlState = ResolveLocalSpecialControlState(localPlayer, &controllerEntity);
    const bool useControlledProfile = controlState != ControlledDamageState::None
        && baseType != DamageFeedbackType::Fire
        && baseType != DamageFeedbackType::Acid
        && baseType != DamageFeedbackType::Explosion;

    float directionalBias = 0.0f;
    bool hasDirectionalBias = false;

    if (m_DamageDirectionalEnabled)
    {
        C_BaseEntity* directionSource = nullptr;
        if (useControlledProfile)
        {
            directionSource = controllerEntity;
        }
        else
        {
            int attackerIndex = 0;
            const int attackerUserId = event->GetInt("attacker", 0);
            if (attackerUserId > 0)
                attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
            if (attackerIndex <= 0)
                attackerIndex = event->GetInt("attackerentid", 0);
            if (attackerIndex > 0)
                directionSource = SafeGetProjectedItemLabelClientEntity(this, attackerIndex);
        }

        if (directionSource)
        {
            Vector localPos{};
            Vector attackerPos{};
            if (SafeGetProjectedItemLabelAbsOrigin(localPlayer, localPos) &&
                SafeGetProjectedItemLabelAbsOrigin(directionSource, attackerPos))
            {
                Vector toAttacker = attackerPos - localPos;
                toAttacker.z = 0.0f;

                Vector right = m_HmdRight;
                right.z = 0.0f;

                if (!toAttacker.IsZero() && !right.IsZero())
                {
                    VectorNormalize(toAttacker);
                    VectorNormalize(right);
                    directionalBias = std::clamp(DotProduct(toAttacker, right), -1.0f, 1.0f);
                    hasDirectionalBias = true;
                }
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    PendingDamageFeedback& pending = m_PendingDamageFeedback;
    const ControlledDamageState pendingControlState = useControlledProfile ? controlState : ControlledDamageState::None;

    const bool sameBurst = pending.active
        && pending.queuedAt.time_since_epoch().count() != 0
        && pending.controlState == pendingControlState
        && std::chrono::duration<float>(now - pending.queuedAt).count() <= (std::max)(0.0f, m_DamageFeedbackMergeWindowSeconds);

    auto damageSeverity = [](DamageFeedbackType feedbackType)
        {
            switch (feedbackType)
            {
            case DamageFeedbackType::CommonHit: return 0;
            case DamageFeedbackType::SpecialHit: return 1;
            case DamageFeedbackType::Fire:      return 2;
            case DamageFeedbackType::Acid:      return 3;
            case DamageFeedbackType::HeavyHit:  return 4;
            case DamageFeedbackType::Explosion: return 5;
            default:                            return 0;
            }
        };

    if (!sameBurst)
    {
        pending = {};
        pending.active = true;
        pending.type = baseType;
        pending.controlState = pendingControlState;
        pending.maxDamage = damage;
        pending.mergedCount = 1;
        pending.queuedAt = now;
    }
    else
    {
        pending.maxDamage = (std::max)(pending.maxDamage, damage);
        pending.mergedCount = (std::min)(pending.mergedCount + 1, 8);
        if (pending.controlState == ControlledDamageState::None && damageSeverity(baseType) > damageSeverity(pending.type))
            pending.type = baseType;
    }

    if (hasDirectionalBias)
    {
        pending.directionalBiasSum += directionalBias;
        pending.directionalSampleCount += 1;
        if (std::abs(directionalBias) > std::abs(pending.directionalBiasPeak))
            pending.directionalBiasPeak = directionalBias;
    }
}

void VR::UpdateDamageFeedback()
{
    if (!m_DamageFeedbackEnabled || !m_IsVREnabled || !m_Game || !m_Game->m_EngineClient)
    {
        m_LastObservedDamageHealth = -1;
        return;
    }

    EnsureDamageFeedbackEventListener();

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(SafeGetProjectedItemLabelClientEntity(this, localPlayerIndex));
    if (!localPlayer)
    {
        m_LastObservedDamageHealth = -1;
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    int currentHealthRaw = 0;
    if (!SafeReadInt(reinterpret_cast<const unsigned char*>(localPlayer), kHealthOffset, currentHealthRaw))
    {
        m_LastObservedDamageHealth = -1;
        return;
    }

    const int currentHealth = (std::max)(0, currentHealthRaw);
    if (m_LastObservedDamageHealth < 0)
    {
        m_LastObservedDamageHealth = currentHealth;
    }
    else
    {
        if (currentHealth < m_LastObservedDamageHealth)
        {
            const int healthDelta = m_LastObservedDamageHealth - currentHealth;
            const bool recentDamageEvent = m_LastDamageFeedbackEventSeenTime.time_since_epoch().count() != 0
                && std::chrono::duration<float>(now - m_LastDamageFeedbackEventSeenTime).count() <= 0.20f;

            if (!recentDamageEvent && healthDelta > 0)
            {
                PendingDamageFeedback& pending = m_PendingDamageFeedback;
                pending = {};
                pending.active = true;
                pending.type = ClassifyDamageFeedbackType("", healthDelta);
                pending.controlState = ResolveLocalSpecialControlState(localPlayer, nullptr);
                pending.maxDamage = healthDelta;
                pending.mergedCount = 1;
                pending.queuedAt = now;
            }
        }

        m_LastObservedDamageHealth = currentHealth;
    }

    if (m_PendingDamageFeedback.active)
    {
        const bool mergeWindowElapsed = m_PendingDamageFeedback.queuedAt.time_since_epoch().count() == 0
            || std::chrono::duration<float>(now - m_PendingDamageFeedback.queuedAt).count() >= (std::max)(0.0f, m_DamageFeedbackMergeWindowSeconds);

        const bool triggerIntervalElapsed = m_LastDamageFeedbackTriggerTime.time_since_epoch().count() == 0
            || std::chrono::duration<float>(now - m_LastDamageFeedbackTriggerTime).count() >= (std::max)(0.0f, m_DamageFeedbackMinTriggerIntervalSeconds);

        if (mergeWindowElapsed && triggerIntervalElapsed)
        {
            const bool controlled = m_PendingDamageFeedback.controlState != ControlledDamageState::None;
            const WeaponHapticsProfile damageProfile = controlled
                ? GetControlledDamageHapticsProfile(m_PendingDamageFeedback.controlState)
                : GetDamageHapticsProfile(m_PendingDamageFeedback.type);
            float amplitude = damageProfile.amplitude;
            const float frequency = damageProfile.frequency;
            const float duration = damageProfile.durationSeconds;
            const float damageBonus = std::clamp((m_PendingDamageFeedback.maxDamage - m_DamageScaleStart) * m_DamageScalePerPoint, 0.0f, m_DamageScaleMaxBonus);
            const float mergedBonus = (std::max)(0, m_PendingDamageFeedback.mergedCount - 1) * (std::max)(0.0f, m_DamageFeedbackMergedHitBonus);
            amplitude = std::clamp(amplitude + damageBonus + mergedBonus, m_DamageAmplitudeMin, m_DamageAmplitudeMax);

            int damagePriority = 2;
            if (controlled)
            {
                switch (m_PendingDamageFeedback.controlState)
                {
                case ControlledDamageState::SmokerTongue:
                case ControlledDamageState::HunterPounce:
                case ControlledDamageState::JockeyRide:
                    damagePriority = 3;
                    break;
                case ControlledDamageState::ChargerCarry:
                    damagePriority = 4;
                    break;
                case ControlledDamageState::ChargerPummel:
                    damagePriority = 5;
                    break;
                default:
                    break;
                }
            }
            else
            {
                if (m_PendingDamageFeedback.type == DamageFeedbackType::SpecialHit)
                    damagePriority = 3;
                else if (m_PendingDamageFeedback.type == DamageFeedbackType::HeavyHit || m_PendingDamageFeedback.type == DamageFeedbackType::Explosion)
                    damagePriority = 4;
            }

            float rightBias = 0.0f;
            if (m_PendingDamageFeedback.directionalSampleCount > 0)
            {
                const float averageBias = std::clamp(
                    m_PendingDamageFeedback.directionalBiasSum / (float)m_PendingDamageFeedback.directionalSampleCount,
                    -1.0f,
                    1.0f);
                const float dominantBias = std::clamp(m_PendingDamageFeedback.directionalBiasPeak, -1.0f, 1.0f);
                // Keep the strongest sample in the burst dominant so rapid multi-hit merges
                // do not wash the direction back toward center.
                rightBias = std::clamp(averageBias * 0.35f + dominantBias * 0.65f, -1.0f, 1.0f);
            }
            if (controlled)
                rightBias *= std::clamp(GetControlledDamageDirectionalBiasScale(m_PendingDamageFeedback.controlState), 0.0f, 1.0f);

            TriggerDirectionalDamageHaptics(amplitude, frequency, duration, rightBias, damagePriority);
            m_LastDamageFeedbackTriggerTime = now;
            m_PendingDamageFeedback = {};
        }
    }

    // Sustained damage envelopes (acid / fire).
    if (m_DamageSustainEnabled)
    {
        if (now < m_AcidSustainUntil && now >= m_NextAcidSustainPulse)
        {
            TriggerImpactHapticsBothHands(
                m_DamageAcidSustainPulse.amplitude,
                m_DamageAcidSustainPulse.frequency,
                m_DamageAcidSustainPulse.durationSeconds,
                1);
            m_NextAcidSustainPulse = now + std::chrono::milliseconds((int)std::round(m_DamageAcidSustainIntervalSeconds * 1000.0f));
        }
        if (now < m_FireSustainUntil && now >= m_NextFireSustainPulse)
        {
            TriggerImpactHapticsBothHands(
                m_DamageFireSustainPulse.amplitude,
                m_DamageFireSustainPulse.frequency,
                m_DamageFireSustainPulse.durationSeconds,
                1);
            m_NextFireSustainPulse = now + std::chrono::milliseconds((int)std::round(m_DamageFireSustainIntervalSeconds * 1000.0f));
        }
    }

    // Landing impact. Detect the airborne->landed transition from raw netvar offsets instead of
    // relying on C_BasePlayer field layouts directly, then split the response into:
    //   - landing.medium: landed without losing health
    //   - landing.damage: landed and health dropped on the landing frame
    if (m_LandingHapticsEnabled)
    {
        auto resetLandingState = [&]()
            {
                m_LandingAirborneSince = {};
                m_WasOnGroundForHaptics = true;
                m_LastVerticalSpeedForHaptics = 0.0f;
                m_LandingPeakDownwardSpeedForHaptics = 0.0f;
                m_LastAirborneHealthForHaptics = -1;
            };

        const unsigned char* playerBase = reinterpret_cast<const unsigned char*>(localPlayer);
        unsigned char lifeState = 0;
        int groundEntity = -1;
        int flags = 0;
        float verticalSpeed = 0.0f;
        int health = 0;

        const bool validLandingRead =
            SafeReadU8(playerBase, kLifeStateOffset, lifeState) &&
            SafeReadInt(playerBase, kGroundEntityOffset, groundEntity) &&
            SafeReadInt(playerBase, kFlagsOffset, flags) &&
            SafeReadFloat(playerBase, kVelocityZOffset, verticalSpeed) &&
            SafeReadInt(playerBase, kHealthOffset, health);

        if (!validLandingRead || lifeState != 0 || health <= 0)
        {
            resetLandingState();
        }
        else
        {
            const bool onGround = (groundEntity != -1) || ((flags & kOnGroundFlag) != 0);
            if (onGround)
            {
                if (!m_WasOnGroundForHaptics)
                {
                    const float airTime = (m_LandingAirborneSince.time_since_epoch().count() == 0)
                        ? 0.0f
                        : std::chrono::duration<float>(now - m_LandingAirborneSince).count();
                    const bool hardEnoughLanding = airTime >= m_LandingMinAirTime
                        && m_LandingPeakDownwardSpeedForHaptics >= m_LandingMinDownwardSpeed;

                    if (hardEnoughLanding)
                    {
                        const bool landedWithDamage = m_LastAirborneHealthForHaptics > 0 && health < m_LastAirborneHealthForHaptics;
                        const WeaponHapticsProfile& landingProfile = landedWithDamage
                            ? m_LandingDamageHapticsProfile
                            : m_LandingMediumHapticsProfile;
                        TriggerImpactHapticsBothHands(
                            landingProfile.amplitude,
                            landingProfile.frequency,
                            landingProfile.durationSeconds,
                            landedWithDamage ? 4 : 3);
                    }
                }

                m_LandingAirborneSince = {};
                m_LandingPeakDownwardSpeedForHaptics = 0.0f;
                m_LastAirborneHealthForHaptics = health;
            }
            else
            {
                const float downwardSpeed = std::max(0.0f, -verticalSpeed);
                if (m_WasOnGroundForHaptics)
                {
                    m_LandingAirborneSince = now;
                    m_LandingPeakDownwardSpeedForHaptics = downwardSpeed;
                }
                else
                {
                    m_LandingPeakDownwardSpeedForHaptics = std::max(m_LandingPeakDownwardSpeedForHaptics, downwardSpeed);
                }

                // Keep the last airborne HP sample so the first grounded frame can tell whether
                // the landing itself caused damage. Mid-air damage that already replicated earlier
                // will naturally update this baseline and won't be misclassified as fall damage.
                m_LastAirborneHealthForHaptics = health;
            }

            m_WasOnGroundForHaptics = onGround;
            m_LastVerticalSpeedForHaptics = verticalSpeed;
        }
    }

    // Camera shake -> haptics (explosions, tank stomps, etc.).
    if (m_CameraShakeHapticsEnabled)
    {
        const auto normalizeAngleDelta = [](float current, float previous)
            {
                return std::fabs(std::remainderf(current - previous, 360.0f));
            };

        if (!m_CameraShakeStateInitialized)
        {
            m_LastCameraShakeOrigin = m_SetupOrigin;
            m_LastCameraShakeAngles = m_SetupAngles;
            m_CameraShakeStateInitialized = true;
        }
        else
        {
            vr::InputAnalogActionData_t turnActionData{};
            const bool turnActionActive = GetAnalogActionData(m_ActionTurn, turnActionData)
                && std::fabs(turnActionData.x) > 0.2f;
            const float angDelta =
                normalizeAngleDelta(m_SetupAngles.x, m_LastCameraShakeAngles.x) +
                normalizeAngleDelta(m_SetupAngles.z, m_LastCameraShakeAngles.z);
            const float posDelta = (m_SetupOrigin - m_LastCameraShakeOrigin).Length();
            const float hmdAngVel = m_HmdPose.TrackedDeviceAngVel.Length();

            m_LastCameraShakeAngles = m_SetupAngles;
            m_LastCameraShakeOrigin = m_SetupOrigin;

            if (!turnActionActive && hmdAngVel < m_CameraShakeHmdAngVelMax)
            {
                const float shakeScore = (std::max)(
                    std::clamp((angDelta - m_CameraShakeAngleThreshold) / m_CameraShakeAngleRange, 0.0f, 1.0f),
                    std::clamp((posDelta - m_CameraShakePosThreshold) / m_CameraShakePosRange, 0.0f, 1.0f));

                if (shakeScore > 0.0f
                    && (m_LastCameraShakeHapticsPulse.time_since_epoch().count() == 0
                        || std::chrono::duration<float>(now - m_LastCameraShakeHapticsPulse).count() >= m_CameraShakePulseIntervalSeconds))
                {
                    const float amp = m_CameraShakePulseAmpMin + (m_CameraShakePulseAmpMax - m_CameraShakePulseAmpMin) * shakeScore;
                    const float freq = m_CameraShakePulseFreqMax + (m_CameraShakePulseFreqMin - m_CameraShakePulseFreqMax) * shakeScore;
                    const float dur = m_CameraShakePulseDurMin + (m_CameraShakePulseDurMax - m_CameraShakePulseDurMin) * shakeScore;
                    TriggerImpactHapticsBothHands(amp, freq, dur, 2);
                    m_LastCameraShakeHapticsPulse = now;
                }
            }
        }
    }
}

void VR::EnsureKillSoundEventListener()
{
    if (m_KillSoundEventListenerRegistered || !m_Game)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_LastKillSoundEventRegisterAttempt.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastKillSoundEventRegisterAttempt).count();
        if (elapsed < 2.0f)
            return;
    }
    m_LastKillSoundEventRegisterAttempt = now;

    if (!m_KillSoundEventManager)
        m_KillSoundEventManager = m_Game->m_GameEventManager;
    if (!m_KillSoundEventManager)
    {
        return;
    }

    if (!m_KillSoundEventListener)
        m_KillSoundEventListener = new VRKillSoundEventListener(this);

    static constexpr const char* kKillSoundEvents[] = { "player_hurt", "infected_hurt", "player_death", "infected_death", "witch_killed" };
    bool registeredAll = true;
    for (const char* eventName : kKillSoundEvents)
    {
        const bool alreadyRegistered = m_KillSoundEventManager->FindListener(m_KillSoundEventListener, eventName);
        const bool registered = alreadyRegistered || m_KillSoundEventManager->AddListener(m_KillSoundEventListener, eventName, false);
        registeredAll = registeredAll && registered;
    }

    m_KillSoundEventListenerRegistered = registeredAll;
}

void VR::HandleKillSoundGameEvent(IGameEvent* event)
{
    if (!event || !m_Game || !m_Game->m_EngineClient)
        return;

    const char* rawEventName = event->GetName();
    if (!rawEventName || !*rawEventName)
        return;

    const std::string eventName(rawEventName);
    const int localUserId = GetLocalPlayerUserId(m_Game);
    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    if (localPlayerIndex <= 0)
        return;

    if (eventName == "player_hurt" || eventName == "infected_hurt")
    {
        const int attackerUserId = event->GetInt("attacker", 0);
        int attackerIndex = 0;
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        if (attackerIndex <= 0)
            attackerIndex = event->GetInt("attackerentid", 0);

        const bool attackerMatchesLocalUser = localUserId > 0 && attackerUserId == localUserId;
        const bool attackerMatchesLocalIndex = attackerIndex > 0 && attackerIndex == localPlayerIndex;

        const auto now = std::chrono::steady_clock::now();
        const std::uintptr_t entityTag = ResolveKillEventEntityTag(m_Game, event, eventName);
        Vector impactPos{};
        const bool attackerMatchesLocal = attackerMatchesLocalUser || attackerMatchesLocalIndex;
        const uint32_t shotSerial = m_PredictedHitFeedbackShotSerial;
        const float shotAgeSeconds =
            m_LastPredictedHitFeedbackShotTime.time_since_epoch().count() != 0
            ? std::chrono::duration<float>(now - m_LastPredictedHitFeedbackShotTime).count()
            : -1.0f;
        const bool recentShotWindowActive =
            m_LastPredictedHitFeedbackShotTime.time_since_epoch().count() != 0
            && shotAgeSeconds >= 0.0f
            && shotAgeSeconds <= 0.35f;

        bool matchedImpactByEntityTag = false;
        bool matchedImpactByShotFallback = false;
        if (recentShotWindowActive && shotSerial != 0)
        {
            if (entityTag != 0)
                matchedImpactByEntityTag = FindPendingKillSoundHit(entityTag, now, &impactPos, shotSerial);
            if (!matchedImpactByEntityTag)
                matchedImpactByShotFallback = FindPendingKillSoundHit(0, now, &impactPos, shotSerial);
        }
        const bool matchedImpact = matchedImpactByEntityTag || matchedImpactByShotFallback;

        if (m_FeedbackSoundDebugLog &&
            !ShouldThrottle(m_LastFeedbackSoundDebugLogTime, m_FeedbackSoundDebugLogHz) &&
            (attackerMatchesLocal || recentShotWindowActive || matchedImpact))
        {
            Game::logMsg(
                "[VR][KillSound][hurt] event=%s attackerUid=%d attackerIdx=%d local=%d entity=%p shot=%u age=%.3f matchedTag=%d matchedShot=%d accept=%d pending=%zu",
                eventName.c_str(),
                attackerUserId,
                attackerIndex,
                attackerMatchesLocal ? 1 : 0,
                reinterpret_cast<void*>(entityTag),
                shotSerial,
                shotAgeSeconds,
                matchedImpactByEntityTag ? 1 : 0,
                matchedImpactByShotFallback ? 1 : 0,
                (attackerMatchesLocal && matchedImpact && recentShotWindowActive) ? 1 : 0,
                m_PendingKillSoundHits.size());
        }

        // Hurt events alone are not a reliable hit-confirm source here:
        // they can arrive without a usable impact match, or be unrelated to the
        // current shot (DOT / lingering damage / other local-side quirks). Only
        // emit hit feedback when the event can still be tied back to a recent
        // predicted shot impact.
        if (!attackerMatchesLocal || !matchedImpact || !recentShotWindowActive)
            return;

        if (m_HitIndicatorEnabled)
            SpawnHitIndicator(impactPos);
        return;
    }

    int attackerUserId = 0;
    int attackerIndex = 0;
    bool headshot = false;
    if (eventName == "player_death")
    {
        attackerUserId = event->GetInt("attacker", 0);
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        if (attackerIndex <= 0)
            attackerIndex = event->GetInt("attackerentid", 0);
        headshot = event->GetBool("headshot", false);
    }
    else if (eventName == "infected_death")
    {
        attackerUserId = event->GetInt("attacker", 0);
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        if (attackerIndex <= 0)
            attackerIndex = event->GetInt("attackerentid", 0);
        headshot = event->GetBool("headshot", false);
    }
    else if (eventName == "witch_killed")
    {
        attackerUserId = event->GetInt("userid", 0);
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        headshot = false;
    }
    else
    {
        return;
    }

    const bool attackerMatchesLocalUser = localUserId > 0 && attackerUserId == localUserId;
    const bool attackerMatchesLocalIndex = attackerIndex > 0 && attackerIndex == localPlayerIndex;
    const std::uintptr_t entityTag = ResolveKillEventEntityTag(m_Game, event, eventName);

    if (!attackerMatchesLocalUser && !attackerMatchesLocalIndex)
    {
        const auto now = std::chrono::steady_clock::now();
        bool matchedImpact = false;
        if (entityTag != 0)
            matchedImpact = FindPendingKillSoundHit(entityTag, now, nullptr);
        if (!matchedImpact)
            matchedImpact = FindPendingKillSoundHit(0, now, nullptr);
        if (!matchedImpact)
            return;
    }

    QueuePendingKillSoundEvent(entityTag, headshot);
}

void VR::QueuePendingKillSoundEvent(std::uintptr_t entityTag, bool headshot)
{
    const auto now = std::chrono::steady_clock::now();
    const float eventLifetimeSeconds = (std::max)(0.5f, m_KillSoundDetectionWindowSeconds + 0.35f);
    m_PendingKillSoundEvents.erase(
        std::remove_if(
            m_PendingKillSoundEvents.begin(),
            m_PendingKillSoundEvents.end(),
            [&](const PendingKillSoundEvent& pendingEvent)
            {
                return std::chrono::duration<float>(now - pendingEvent.receivedAt).count() >= eventLifetimeSeconds;
            }),
        m_PendingKillSoundEvents.end());

    PendingKillSoundEvent pendingEvent{};
    pendingEvent.entityTag = entityTag;
    pendingEvent.headshot = headshot;
    pendingEvent.receivedAt = now;
    m_PendingKillSoundEvents.push_back(std::move(pendingEvent));
    if (m_PendingKillSoundEvents.size() > 32)
        m_PendingKillSoundEvents.erase(m_PendingKillSoundEvents.begin(), m_PendingKillSoundEvents.begin() + (m_PendingKillSoundEvents.size() - 32));
}

bool VR::ConsumePendingKillSoundEvent(std::chrono::steady_clock::time_point now, bool& outHeadshot, std::uintptr_t& outEntityTag)
{
    const float eventLifetimeSeconds = (std::max)(0.5f, m_KillSoundDetectionWindowSeconds + 0.35f);
    m_PendingKillSoundEvents.erase(
        std::remove_if(
            m_PendingKillSoundEvents.begin(),
            m_PendingKillSoundEvents.end(),
            [&](const PendingKillSoundEvent& pendingEvent)
            {
                return std::chrono::duration<float>(now - pendingEvent.receivedAt).count() >= eventLifetimeSeconds;
            }),
        m_PendingKillSoundEvents.end());

    if (m_PendingKillSoundEvents.empty())
        return false;

    const PendingKillSoundEvent pendingEvent = m_PendingKillSoundEvents.front();
    m_PendingKillSoundEvents.erase(m_PendingKillSoundEvents.begin());

    outHeadshot = pendingEvent.headshot;
    outEntityTag = pendingEvent.entityTag;
    return true;
}

bool VR::IsKillSoundTargetEntity(const C_BaseEntity* entity) const
{
    if (!entity || !m_Game)
        return false;

    const auto* base = reinterpret_cast<const unsigned char*>(entity);
    unsigned char lifeState = 1;
    int team = 0;
    const bool hasLifeState = VR_TryReadU8(base, kLifeStateOffset, lifeState);
    const bool hasTeam = VR_TryReadI32(base, kTeamNumOffset, team);
    const bool isAlive = hasLifeState && lifeState == 0;

    const SpecialInfectedType specialType = GetSpecialInfectedType(entity);

    const char* className = SafeGetProjectedItemLabelNetworkClassName(this, entity);
    if (className && *className)
    {
        const std::string lowered = ToLowerCopy(className);
        if ((lowered.find("infected") != std::string::npos || lowered.find("witch") != std::string::npos) && isAlive)
            return true;

        const bool isPlayerClass = className && (std::strcmp(className, "CTerrorPlayer") == 0 || std::strcmp(className, "C_TerrorPlayer") == 0);
        if (isPlayerClass && hasTeam && team == 3 && isAlive && specialType != SpecialInfectedType::None)
            return true;
    }

    if (hasTeam && team == 3 && isAlive && specialType != SpecialInfectedType::None)
        return true;

    if (hasTeam && team == 3 && isAlive)
        return true;

    return false;
}

void VR::BeginPredictedHitFeedbackShot(int commandNumber)
{
    const auto now = std::chrono::steady_clock::now();

    // Client prediction can replay the same CUserCmd on remote servers. Use the
    // command number as the shot id when available, so a replay of the same shot
    // cannot emit another local hit sound / hit indicator.
    if (commandNumber > 0)
    {
        m_PredictedHitFeedbackShotSerial = static_cast<uint32_t>(commandNumber);
    }
    else
    {
        ++m_PredictedHitFeedbackShotSerial;
        if (m_PredictedHitFeedbackShotSerial == 0)
            m_PredictedHitFeedbackShotSerial = 1;
    }

    m_LastPredictedHitFeedbackShotTime = now;
}

void VR::RegisterPotentialKillSoundHit(const Vector& start, const QAngle& angles)
{
    const bool wantsHitFeedback = m_HitSoundEnabled || m_HitIndicatorEnabled;
    const bool wantsKillFeedback = m_KillSoundEnabled || m_KillIndicatorEnabled;
    if ((!wantsHitFeedback && !wantsKillFeedback) || !m_Game || !m_Game->m_EngineTrace || !m_Game->m_EngineClient)
        return;

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(SafeGetProjectedItemLabelClientEntity(this, localPlayerIndex));
    if (!localPlayer)
        return;

    Vector forward{};
    Vector right{};
    Vector up{};
    QAngle::AngleVectors(angles, &forward, &right, &up);
    if (forward.IsZero())
        return;
    VectorNormalize(forward);

    Ray_t ray;
    ray.Init(start, start + forward * kKillSoundTraceDistance);

    CTraceFilterSkipSelf traceFilter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);
    CGameTrace trace{};
    if (!VR_SafeTraceRay(m_Game->m_EngineTrace, ray, MASK_SHOT, &traceFilter, trace))
        return;

    C_BaseEntity* entity = reinterpret_cast<C_BaseEntity*>(trace.m_pEnt);
    const int entityIndex = FindClientEntityIndexByPointer(m_Game->m_ClientEntityList, entity);
    const auto* base = reinterpret_cast<const unsigned char*>(entity);
    unsigned char lifeState = 1;
    int team = 0;
    int zombieClass = 0;
    const bool hasLifeState = entity && VR_TryReadU8(base, kLifeStateOffset, lifeState);
    const bool hasTeam = entity && VR_TryReadI32(base, kTeamNumOffset, team);
    const bool hasZombieClass = entity && VR_TryReadI32(base, kZombieClassOffset, zombieClass);
    const char* className = entity ? SafeGetProjectedItemLabelNetworkClassName(this, entity) : nullptr;
    if (!entity || !IsKillSoundTargetEntity(entity))
    {
        if (entity &&
            m_FeedbackSoundDebugLog &&
            !ShouldThrottle(m_LastFeedbackSoundDebugLogTime, m_FeedbackSoundDebugLogHz))
        {
            Game::logMsg(
                "[VR][KillSound][predicted-reject] idx=%d entity=%p class=%s frac=%.3f team=%d life=%d z=%d contents=%d hitgroup=%d hitbox=%d",
                entityIndex,
                reinterpret_cast<void*>(entity),
                className ? className : "<unknown>",
                trace.fraction,
                hasTeam ? team : -1,
                hasLifeState ? static_cast<int>(lifeState) : -1,
                hasZombieClass ? zombieClass : -1,
                trace.contents,
                trace.hitgroup,
                trace.hitbox);
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint32_t shotSerial = m_PredictedHitFeedbackShotSerial;
    const bool canUseShotGate = shotSerial != 0
        && m_LastPredictedHitFeedbackShotTime.time_since_epoch().count() != 0
        && std::chrono::duration<float>(now - m_LastPredictedHitFeedbackShotTime).count() <= 0.35f;

    std::uintptr_t weaponTag = 0;
    int clip1 = -2147483647;
#ifdef _MSC_VER
    __try
    {
        C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
        weaponTag = reinterpret_cast<std::uintptr_t>(activeWeapon);
        if (activeWeapon)
            VR_TryReadI32(reinterpret_cast<const unsigned char*>(activeWeapon), kClip1Offset, clip1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        weaponTag = 0;
        clip1 = -2147483647;
    }
#else
    C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
    weaponTag = reinterpret_cast<std::uintptr_t>(activeWeapon);
    if (activeWeapon)
        VR_TryReadI32(reinterpret_cast<const unsigned char*>(activeWeapon), kClip1Offset, clip1);
#endif

    const std::uintptr_t entityTag = reinterpret_cast<std::uintptr_t>(entity);

    const bool shotAlreadyEmitted = canUseShotGate
        && std::find(
            m_RecentPredictedHitFeedbackShotSerials.begin(),
            m_RecentPredictedHitFeedbackShotSerials.end(),
            shotSerial) != m_RecentPredictedHitFeedbackShotSerials.end();

    bool replaySignatureAlreadyEmitted = false;
    if (!shotAlreadyEmitted)
    {
        static constexpr float kPredictedHitReplayDedupeSeconds = 0.22f;
        static constexpr float kPredictedHitReplayStartDistSqr = 64.0f;
        static constexpr float kPredictedHitReplayAimDot = 0.9985f;
        static constexpr int kUnknownClip1 = -2147483647;

        for (const PredictedHitFeedbackEmission& emitted : m_RecentPredictedHitFeedbackEmissions)
        {
            if (emitted.emittedAt.time_since_epoch().count() == 0)
                continue;

            const float age = std::chrono::duration<float>(now - emitted.emittedAt).count();
            if (age < 0.0f || age > kPredictedHitReplayDedupeSeconds)
                continue;

            const bool sameShotSerial = canUseShotGate && emitted.shotSerial != 0 && emitted.shotSerial == shotSerial;
            if (sameShotSerial)
            {
                replaySignatureAlreadyEmitted = true;
                break;
            }

            const bool sameWeapon = weaponTag != 0 && emitted.weaponTag == weaponTag;
            const bool sameClip = clip1 != kUnknownClip1 && emitted.clip1 != kUnknownClip1 && emitted.clip1 == clip1;
            if (!sameWeapon || !sameClip)
                continue;

            const Vector deltaStart = start - emitted.start;
            const float startDistSq = deltaStart.LengthSqr();
            const float aimDot = DotProduct(forward, emitted.dir);
            const bool sameTrajectory = startDistSq <= kPredictedHitReplayStartDistSqr && aimDot >= kPredictedHitReplayAimDot;
            const bool sameEntity = entityTag != 0 && emitted.entityTag == entityTag;
            if (sameTrajectory || sameEntity)
            {
                replaySignatureAlreadyEmitted = true;
                break;
            }
        }
    }

    bool triggerDirectHitFeedback = !shotAlreadyEmitted && !replaySignatureAlreadyEmitted;
    if (triggerDirectHitFeedback && m_PredictedHitFeedbackDedupWindowSeconds > 0.0f)
    {
        const auto elapsed = std::chrono::duration<float>(now - m_LastPredictedHitFeedbackTime).count();
        if (m_LastPredictedHitFeedbackTime.time_since_epoch().count() != 0
            && elapsed >= 0.0f
            && elapsed <= m_PredictedHitFeedbackDedupWindowSeconds)
        {
            const Vector deltaStart = start - m_LastPredictedHitFeedbackStart;
            const float startDistSq = deltaStart.LengthSqr();
            const float aimDot = DotProduct(forward, m_LastPredictedHitFeedbackDir);
            if (startDistSq <= 16.0f && aimDot >= 0.9995f)
                triggerDirectHitFeedback = false;
        }
    }

    if (triggerDirectHitFeedback)
    {
        if (m_HitSoundEnabled)
            QueueHitSoundPlayback(&trace.endpos, canUseShotGate ? shotSerial : 0, entityTag);
        if (m_HitIndicatorEnabled)
            SpawnHitIndicator(trace.endpos);

        if (canUseShotGate)
        {
            m_LastPredictedHitSoundShotSerial = shotSerial;
            m_RecentPredictedHitFeedbackShotSerials[
                m_RecentPredictedHitFeedbackShotSerialCursor % m_RecentPredictedHitFeedbackShotSerials.size()] = shotSerial;
            ++m_RecentPredictedHitFeedbackShotSerialCursor;
        }

        PredictedHitFeedbackEmission& emitted =
            m_RecentPredictedHitFeedbackEmissions[
                m_RecentPredictedHitFeedbackEmissionCursor % m_RecentPredictedHitFeedbackEmissions.size()];
        emitted.entityTag = entityTag;
        emitted.weaponTag = weaponTag;
        emitted.clip1 = clip1;
        emitted.shotSerial = shotSerial;
        emitted.start = start;
        emitted.dir = forward;
        emitted.emittedAt = now;
        ++m_RecentPredictedHitFeedbackEmissionCursor;

        m_LastPredictedHitFeedbackStart = start;
        m_LastPredictedHitFeedbackDir = forward;
        m_LastPredictedHitFeedbackEntityTag = reinterpret_cast<std::uintptr_t>(entity);
        m_LastPredictedHitFeedbackTime = now;
    }

    const auto expiresAt = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(m_KillSoundDetectionWindowSeconds));
    const SpecialInfectedType specialType = GetSpecialInfectedType(entity);

    if (m_FeedbackSoundDebugLog &&
        !ShouldThrottle(m_LastFeedbackSoundDebugLogTime, m_FeedbackSoundDebugLogHz))
    {
        Game::logMsg(
            "[VR][KillSound][predicted-hit] shot=%u idx=%d entity=%p class=%s team=%d life=%d z=%d si=%d direct=%d shotDupe=%d replayDupe=%d clip=%d weapon=%p pos=(%.1f %.1f %.1f)",
            shotSerial,
            entityIndex,
            reinterpret_cast<void*>(entityTag),
            className ? className : "<unknown>",
            hasTeam ? team : -1,
            hasLifeState ? static_cast<int>(lifeState) : -1,
            hasZombieClass ? zombieClass : -1,
            static_cast<int>(specialType),
            triggerDirectHitFeedback ? 1 : 0,
            shotAlreadyEmitted ? 1 : 0,
            replaySignatureAlreadyEmitted ? 1 : 0,
            clip1,
            reinterpret_cast<void*>(weaponTag),
            trace.endpos.x,
            trace.endpos.y,
            trace.endpos.z);
    }

    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    for (auto& hit : m_PendingKillSoundHits)
    {
        if (hit.entityTag == entityTag)
        {
            hit.expiresAt = expiresAt;
            hit.shotSerial = shotSerial;
            hit.impactPos = trace.endpos;
            return;
        }
    }

    PendingKillSoundHit hit{};
    hit.entityTag = entityTag;
    hit.expiresAt = expiresAt;
    hit.shotSerial = shotSerial;
    hit.impactPos = trace.endpos;
    m_PendingKillSoundHits.push_back(hit);
    if (m_PendingKillSoundHits.size() > 24)
        m_PendingKillSoundHits.erase(m_PendingKillSoundHits.begin(), m_PendingKillSoundHits.begin() + (m_PendingKillSoundHits.size() - 24));
}

bool VR::ConsumePendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos)
{
    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    if (preferredEntityTag != 0)
    {
        for (auto it = m_PendingKillSoundHits.rbegin(); it != m_PendingKillSoundHits.rend(); ++it)
        {
            if (it->entityTag != preferredEntityTag)
                continue;

            if (outImpactPos)
                *outImpactPos = it->impactPos;
            m_PendingKillSoundHits.erase(std::next(it).base());
            return true;
        }
    }

    for (auto it = m_PendingKillSoundHits.rbegin(); it != m_PendingKillSoundHits.rend(); ++it)
    {
        const auto* entity = reinterpret_cast<const C_BaseEntity*>(it->entityTag);
        if (entity && IsEntityAlive(entity))
            continue;
        if (outImpactPos)
            *outImpactPos = it->impactPos;
        m_PendingKillSoundHits.erase(std::next(it).base());
        return true;
    }

    if (!m_PendingKillSoundHits.empty())
    {
        if (outImpactPos)
            *outImpactPos = m_PendingKillSoundHits.back().impactPos;
        m_PendingKillSoundHits.pop_back();
        return true;
    }

    return false;
}

bool VR::FindPendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos, uint32_t preferredShotSerial)
{
    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    auto matchesTag = [&](const PendingKillSoundHit& hit)
        {
            return preferredEntityTag == 0 || hit.entityTag == preferredEntityTag;
        };
    auto matchesShotSerial = [&](const PendingKillSoundHit& hit)
        {
            return preferredShotSerial == 0 || hit.shotSerial == preferredShotSerial;
        };

    for (auto it = m_PendingKillSoundHits.rbegin(); it != m_PendingKillSoundHits.rend(); ++it)
    {
        if (!matchesTag(*it) || !matchesShotSerial(*it))
            continue;

        if (outImpactPos)
            *outImpactPos = it->impactPos;
        return true;
    }

    return false;
}

bool VR::TryPlayKillSoundSpec(const std::string& rawSpec, float baseVolume, const Vector* worldPos, bool preferLoadedPathReuse)
{
    const std::string spec = TrimCopy(rawSpec);
    if (spec.empty())
        return false;

    auto computeStereoVolumes = [&](int& leftVolume, int& rightVolume)
        {
            ComputeFeedbackSoundStereoVolumes(worldPos, baseVolume, leftVolume, rightVolume);
            if (!m_FeedbackSoundDebugLog)
                return;
            if (ShouldThrottle(m_LastFeedbackSoundDebugLogTime, m_FeedbackSoundDebugLogHz))
                return;

            if (worldPos)
            {
                Game::logMsg(
                    "[VR][FeedbackSound] mode=%s spec=%s base=%.3f blend=%.3f L=%d R=%d src=(%.1f %.1f %.1f) hmd=(%.1f %.1f %.1f) right=(%.3f %.3f %.3f)",
                    DescribeFeedbackSoundDebugForceChannel(m_FeedbackSoundDebugForceChannel),
                    spec.c_str(),
                    baseVolume,
                    m_FeedbackSoundSpatialBlend,
                    leftVolume,
                    rightVolume,
                    worldPos->x,
                    worldPos->y,
                    worldPos->z,
                    m_HmdPosAbs.x,
                    m_HmdPosAbs.y,
                    m_HmdPosAbs.z,
                    m_HmdRight.x,
                    m_HmdRight.y,
                    m_HmdRight.z);
                return;
            }

            Game::logMsg(
                "[VR][FeedbackSound] mode=%s spec=%s base=%.3f blend=%.3f L=%d R=%d src=(none) hmd=(%.1f %.1f %.1f) right=(%.3f %.3f %.3f)",
                DescribeFeedbackSoundDebugForceChannel(m_FeedbackSoundDebugForceChannel),
                spec.c_str(),
                baseVolume,
                m_FeedbackSoundSpatialBlend,
                leftVolume,
                rightVolume,
                m_HmdPosAbs.x,
                m_HmdPosAbs.y,
                m_HmdPosAbs.z,
                m_HmdRight.x,
                m_HmdRight.y,
                m_HmdRight.z);
        };

    auto getPayload = [&](size_t prefixLen)
        {
            return TrimCopy(spec.substr(prefixLen));
        };

    if (StartsWithInsensitive(spec, "alias:"))
    {
        const std::string alias = getPayload(6);
        return !alias.empty()
            && ::PlaySoundA(alias.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }

    if (StartsWithInsensitive(spec, "file:"))
    {
        std::string resolvedPath;
        if (!TryResolveFeedbackSoundFileSpec(spec, resolvedPath))
            return false;

        int leftVolume = 1000;
        int rightVolume = 1000;
        computeStereoVolumes(leftVolume, rightVolume);
        return EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, preferLoadedPathReuse);
    }

    if (StartsWithInsensitive(spec, "game:"))
    {
        const std::string soundPath = getPayload(5);
        if (soundPath.empty())
            return false;

        const std::string resolvedPath = ResolveGameSoundFilePath(soundPath);
        if (!resolvedPath.empty())
        {
            int leftVolume = 1000;
            int rightVolume = 1000;
            computeStereoVolumes(leftVolume, rightVolume);
            if (EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, false))
                return true;
        }

        if (!m_Game)
            return false;

        const std::string cmd = "play " + soundPath;
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (StartsWithInsensitive(spec, "gamesound:"))
    {
        const std::string soundName = getPayload(10);
        if (soundName.empty())
            return false;

        // Route the built-in VR marker sounds through the local voice pool so repeated hits can overlap.
        const std::string resolvedPath = ResolveBuiltinFeedbackGameSoundPath(soundName);
        if (!resolvedPath.empty())
        {
            int leftVolume = 1000;
            int rightVolume = 1000;
            computeStereoVolumes(leftVolume, rightVolume);
            if (EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, false))
                return true;
        }

        if (!m_Game)
            return false;

        const std::string cmd = "playgamesound " + soundName;
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (StartsWithInsensitive(spec, "cmd:"))
    {
        const std::string cmd = getPayload(4);
        if (cmd.empty() || !m_Game)
            return false;

        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (LooksLikeAudioFilePath(spec))
    {
        std::string resolvedPath;
        if (!TryResolveFeedbackSoundFileSpec(spec, resolvedPath))
            return false;

        int leftVolume = 1000;
        int rightVolume = 1000;
        computeStereoVolumes(leftVolume, rightVolume);
        return EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, preferLoadedPathReuse);
    }

    return ::PlaySoundA(spec.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

bool VR::TryPlayGameSoundFileLocal(const std::string& soundPath, float baseVolume, const Vector* worldPos)
{
    const std::string resolvedPath = ResolveGameSoundFilePath(soundPath);
    if (resolvedPath.empty())
        return false;

    int leftVolume = 1000;
    int rightVolume = 1000;
    ComputeFeedbackSoundStereoVolumes(worldPos, baseVolume, leftVolume, rightVolume);
    return EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, false);
}

void VR::EnsureFeedbackSoundWorkerThread()
{
    bool expected = false;
    if (!m_FeedbackSoundWorkerStarted.compare_exchange_strong(expected, true))
        return;

    try
    {
        std::thread worker(&VR::FeedbackSoundWorkerMain, this);
        worker.detach();
    }
    catch (const std::system_error&)
    {
        m_FeedbackSoundWorkerStarted.store(false);
    }
}

bool VR::EnqueueFeedbackSoundPlayback(const std::string& resolvedPath, int leftVolume, int rightVolume, bool preferLoadedPathReuse)
{
    if (resolvedPath.empty())
        return false;
    if (leftVolume <= 0 && rightVolume <= 0)
        return true;

    EnsureFeedbackSoundWorkerThread();
    if (!m_FeedbackSoundWorkerStarted.load())
        return false;

    FeedbackSoundWorkerJob job{};
    job.type = FeedbackSoundWorkerJob::Type::PlayFile;
    job.resolvedPath = resolvedPath;
    job.leftVolume = std::clamp(leftVolume, 0, 1000);
    job.rightVolume = std::clamp(rightVolume, 0, 1000);
    job.preferLoadedPathReuse = preferLoadedPathReuse;

    {
        std::lock_guard<std::mutex> lock(m_FeedbackSoundWorkerMutex);
        if (m_FeedbackSoundWorkerJobs.size() >= kFeedbackSoundWorkerMaxQueuedJobs)
        {
            auto warmupIt = std::find_if(
                m_FeedbackSoundWorkerJobs.begin(),
                m_FeedbackSoundWorkerJobs.end(),
                [](const FeedbackSoundWorkerJob& queuedJob)
                {
                    return queuedJob.type == FeedbackSoundWorkerJob::Type::WarmupFile;
                });
            if (warmupIt != m_FeedbackSoundWorkerJobs.end())
                m_FeedbackSoundWorkerJobs.erase(warmupIt);
            else
                m_FeedbackSoundWorkerJobs.pop_front();
        }

        m_FeedbackSoundWorkerJobs.push_back(std::move(job));
    }

    m_FeedbackSoundWorkerCv.notify_one();
    return true;
}

void VR::EnqueueFeedbackSoundWarmupPath(const std::string& resolvedPath)
{
    if (resolvedPath.empty())
        return;

    EnsureFeedbackSoundWorkerThread();
    if (!m_FeedbackSoundWorkerStarted.load())
        return;

    FeedbackSoundWorkerJob job{};
    job.type = FeedbackSoundWorkerJob::Type::WarmupFile;
    job.resolvedPath = resolvedPath;

    {
        std::lock_guard<std::mutex> lock(m_FeedbackSoundWorkerMutex);
        const auto duplicateIt = std::find_if(
            m_FeedbackSoundWorkerJobs.begin(),
            m_FeedbackSoundWorkerJobs.end(),
            [&](const FeedbackSoundWorkerJob& queuedJob)
            {
                return queuedJob.type == FeedbackSoundWorkerJob::Type::WarmupFile
                    && IsSameFeedbackSoundPath(queuedJob.resolvedPath, resolvedPath);
            });
        if (duplicateIt != m_FeedbackSoundWorkerJobs.end())
            return;

        if (m_FeedbackSoundWorkerJobs.size() >= kFeedbackSoundWorkerMaxQueuedJobs)
        {
            auto warmupIt = std::find_if(
                m_FeedbackSoundWorkerJobs.begin(),
                m_FeedbackSoundWorkerJobs.end(),
                [](const FeedbackSoundWorkerJob& queuedJob)
                {
                    return queuedJob.type == FeedbackSoundWorkerJob::Type::WarmupFile;
                });
            if (warmupIt != m_FeedbackSoundWorkerJobs.end())
                m_FeedbackSoundWorkerJobs.erase(warmupIt);
            else
                m_FeedbackSoundWorkerJobs.pop_front();
        }

        m_FeedbackSoundWorkerJobs.push_back(std::move(job));
    }

    m_FeedbackSoundWorkerCv.notify_one();
}

void VR::ResetFeedbackSoundWorkerState()
{
    if (!m_FeedbackSoundWorkerStarted.load())
        return;

    FeedbackSoundWorkerJob job{};
    job.type = FeedbackSoundWorkerJob::Type::ResetState;

    {
        std::lock_guard<std::mutex> lock(m_FeedbackSoundWorkerMutex);
        m_FeedbackSoundWorkerJobs.clear();
        m_FeedbackSoundWorkerJobs.push_back(std::move(job));
    }

    m_FeedbackSoundWorkerCv.notify_one();
}

void VR::FeedbackSoundWorkerMain()
{
    for (;;)
    {
        FeedbackSoundWorkerJob job{};
        {
            std::unique_lock<std::mutex> lock(m_FeedbackSoundWorkerMutex);
            m_FeedbackSoundWorkerCv.wait(lock, [&]()
                {
                    return !m_FeedbackSoundWorkerJobs.empty();
                });

            job = std::move(m_FeedbackSoundWorkerJobs.front());
            m_FeedbackSoundWorkerJobs.pop_front();
        }

        switch (job.type)
        {
        case FeedbackSoundWorkerJob::Type::PlayFile:
            if (!job.resolvedPath.empty())
                TryPlayFeedbackSoundFilePath(job.resolvedPath, job.leftVolume, job.rightVolume, job.preferLoadedPathReuse);
            break;
        case FeedbackSoundWorkerJob::Type::WarmupFile:
            if (!job.resolvedPath.empty())
            {
                FeedbackSoundVoiceState& voice = AcquireFeedbackSoundVoice(&job.resolvedPath);
                EnsureFeedbackSoundVoiceOpen(voice, job.resolvedPath);
            }
            break;
        case FeedbackSoundWorkerJob::Type::ResetState:
            CloseAllFeedbackSoundVoices();
            break;
        }
    }
}

void VR::EnsureSpeechWorkerThread()
{
    bool expected = false;
    if (!m_SpeechWorkerStarted.compare_exchange_strong(expected, true))
        return;

    try
    {
        std::thread worker(&VR::SpeechWorkerMain, this);
        worker.detach();
    }
    catch (const std::system_error&)
    {
        m_SpeechWorkerStarted.store(false);
    }
}

void VR::PumpSpeechToTextCapture()
{
    std::lock_guard<std::mutex> lock(m_SpeechCaptureMutex);
    if (!m_SpeechCaptureWaveIn || !m_SpeechToTextCaptureActive || m_SpeechCaptureStopping)
        return;

    for (auto& buffer : m_SpeechCaptureBuffers)
    {
        if (!buffer.prepared || (buffer.header.dwFlags & WHDR_DONE) == 0)
            continue;

        if (buffer.header.dwBytesRecorded >= sizeof(int16_t) && buffer.header.lpData)
        {
            const size_t sampleCount = static_cast<size_t>(buffer.header.dwBytesRecorded / sizeof(int16_t));
            const size_t oldSize = m_SpeechCapturePcm.size();
            m_SpeechCapturePcm.resize(oldSize + sampleCount);
            std::memcpy(m_SpeechCapturePcm.data() + oldSize, buffer.header.lpData, sampleCount * sizeof(int16_t));
        }

        buffer.header.dwBytesRecorded = 0;
        const MMRESULT addResult = ::waveInAddBuffer(m_SpeechCaptureWaveIn, &buffer.header, sizeof(WAVEHDR));
        if (addResult != MMSYSERR_NOERROR)
            Game::logMsg("[Speech][STT] waveInAddBuffer(pump) failed: %u", static_cast<unsigned>(addResult));
    }
}

bool VR::BeginSpeechToTextCapture()
{
    if (!m_SpeechToTextEnabled)
        return false;

    std::lock_guard<std::mutex> lock(m_SpeechCaptureMutex);
    if (m_SpeechCaptureWaveIn)
        return true;

    m_SpeechCapturePcm.clear();
    m_SpeechCaptureStopping = false;
    m_SpeechCaptureStartedAt = std::chrono::steady_clock::now();

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    auto cleanupCapture = [&]()
        {
            if (m_SpeechCaptureWaveIn)
            {
                ::waveInStop(m_SpeechCaptureWaveIn);
                ::waveInReset(m_SpeechCaptureWaveIn);
            }

            for (auto& buffer : m_SpeechCaptureBuffers)
            {
                if (m_SpeechCaptureWaveIn && buffer.prepared)
                    ::waveInUnprepareHeader(m_SpeechCaptureWaveIn, &buffer.header, sizeof(WAVEHDR));
                buffer.header = {};
                buffer.prepared = false;
            }

            if (m_SpeechCaptureWaveIn)
            {
                ::waveInClose(m_SpeechCaptureWaveIn);
                m_SpeechCaptureWaveIn = nullptr;
            }

            m_SpeechToTextCaptureActive = false;
            m_SpeechCaptureStopping = false;
            m_SpeechCapturePcm.clear();
            m_SpeechCaptureStartedAt = {};
        };

    const MMRESULT openResult = ::waveInOpen(
        &m_SpeechCaptureWaveIn,
        WAVE_MAPPER,
        &format,
        0,
        0,
        CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR)
    {
        Game::logMsg("[Speech][STT] waveInOpen failed: %u", static_cast<unsigned>(openResult));
        cleanupCapture();
        return false;
    }

    for (auto& buffer : m_SpeechCaptureBuffers)
    {
        buffer.header = {};
        buffer.header.lpData = buffer.bytes.data();
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.bytes.size());

        const MMRESULT prepareResult = ::waveInPrepareHeader(m_SpeechCaptureWaveIn, &buffer.header, sizeof(WAVEHDR));
        if (prepareResult != MMSYSERR_NOERROR)
        {
            Game::logMsg("[Speech][STT] waveInPrepareHeader failed: %u", static_cast<unsigned>(prepareResult));
            cleanupCapture();
            return false;
        }

        buffer.prepared = true;
        const MMRESULT addResult = ::waveInAddBuffer(m_SpeechCaptureWaveIn, &buffer.header, sizeof(WAVEHDR));
        if (addResult != MMSYSERR_NOERROR)
        {
            Game::logMsg("[Speech][STT] waveInAddBuffer failed: %u", static_cast<unsigned>(addResult));
            cleanupCapture();
            return false;
        }
    }

    const MMRESULT startResult = ::waveInStart(m_SpeechCaptureWaveIn);
    if (startResult != MMSYSERR_NOERROR)
    {
        Game::logMsg("[Speech][STT] waveInStart failed: %u", static_cast<unsigned>(startResult));
        cleanupCapture();
        return false;
    }

    m_SpeechToTextCaptureActive = true;
    Game::logMsg("[Speech][STT] capture started");
    return true;
}

void VR::EndSpeechToTextCapture(bool queueTranscription)
{
    std::vector<int16_t> capturedSamples;
    {
        std::lock_guard<std::mutex> lock(m_SpeechCaptureMutex);
        if (!m_SpeechCaptureWaveIn && !m_SpeechToTextCaptureActive)
            return;

        m_SpeechCaptureStopping = true;
        if (m_SpeechCaptureWaveIn)
        {
            ::waveInStop(m_SpeechCaptureWaveIn);
            ::waveInReset(m_SpeechCaptureWaveIn);

            for (auto& buffer : m_SpeechCaptureBuffers)
            {
                if (buffer.header.dwBytesRecorded >= sizeof(int16_t) && buffer.header.lpData)
                {
                    const size_t sampleCount = static_cast<size_t>(buffer.header.dwBytesRecorded / sizeof(int16_t));
                    const size_t oldSize = m_SpeechCapturePcm.size();
                    m_SpeechCapturePcm.resize(oldSize + sampleCount);
                    std::memcpy(m_SpeechCapturePcm.data() + oldSize, buffer.header.lpData, sampleCount * sizeof(int16_t));
                    buffer.header.dwBytesRecorded = 0;
                }
            }

            for (auto& buffer : m_SpeechCaptureBuffers)
            {
                if (buffer.prepared)
                {
                    ::waveInUnprepareHeader(m_SpeechCaptureWaveIn, &buffer.header, sizeof(WAVEHDR));
                    buffer.prepared = false;
                }
                buffer.header = {};
            }

            ::waveInClose(m_SpeechCaptureWaveIn);
            m_SpeechCaptureWaveIn = nullptr;
        }

        m_SpeechToTextCaptureActive = false;
        m_SpeechCaptureStopping = false;
        m_SpeechCaptureStartedAt = {};
        capturedSamples.swap(m_SpeechCapturePcm);
    }

    if (!queueTranscription || !m_SpeechToTextEnabled)
        return;

    const float durationSeconds = static_cast<float>(capturedSamples.size()) / 16000.0f;
    if (capturedSamples.empty() || durationSeconds < m_SpeechToTextMinimumRecordSeconds)
    {
        Game::logMsg("[Speech][STT] capture discarded (%.2fs)", durationSeconds);
        return;
    }

    const std::string inputWavePath = BuildSpeechCachePath("stt_input", ".wav");
    const std::string outputBasePath = BuildSpeechCachePath("stt_output", "");
    if (inputWavePath.empty() || outputBasePath.empty())
    {
        Game::logMsg("[Speech][STT] failed to allocate temp paths");
        return;
    }

    if (!WriteMonoPcm16WaveFile(inputWavePath, capturedSamples, 16000))
    {
        Game::logMsg("[Speech][STT] failed to write %s", inputWavePath.c_str());
        return;
    }

    EnsureSpeechWorkerThread();
    if (!m_SpeechWorkerStarted.load())
    {
        Game::logMsg("[Speech][STT] worker thread unavailable");
        return;
    }

    SpeechWorkerJob job{};
    job.type = SpeechWorkerJob::Type::TranscribeWave;
    job.inputPath = inputWavePath;
    job.outputPath = outputBasePath;

    {
        std::lock_guard<std::mutex> lock(m_SpeechWorkerMutex);
        if (m_SpeechWorkerJobs.size() >= 16)
            m_SpeechWorkerJobs.pop_front();
        m_SpeechWorkerJobs.push_back(std::move(job));
    }

    m_SpeechWorkerCv.notify_one();
    Game::logMsg("[Speech][STT] queued %.2fs capture", durationSeconds);
}

VR::TextToSpeechRuntimeConfig VR::BuildTextToSpeechRuntimeConfig(bool useSpeechToTextSendVoiceProfile) const
{
    TextToSpeechRuntimeConfig config{};

    auto pickSpec = [&](const std::string& overrideValue, const std::string& fallbackValue) -> std::string
        {
            return TrimCopy(overrideValue).empty() ? fallbackValue : overrideValue;
        };
    config.commandPrefixSpec = GetEffectiveTextToSpeechCommandPrefixSpec(
        useSpeechToTextSendVoiceProfile
        ? pickSpec(m_SpeechToTextSendVoiceCommandPrefix, m_TextToSpeechCommandPrefix)
        : m_TextToSpeechCommandPrefix);
    config.modelSpec = useSpeechToTextSendVoiceProfile ? pickSpec(m_SpeechToTextSendVoiceModel, m_TextToSpeechModel) : m_TextToSpeechModel;
    config.workingDirSpec = useSpeechToTextSendVoiceProfile ? pickSpec(m_SpeechToTextSendVoiceWorkingDir, m_TextToSpeechWorkingDir) : m_TextToSpeechWorkingDir;
    config.referenceAudioSpec = useSpeechToTextSendVoiceProfile ? pickSpec(m_SpeechToTextSendVoiceReferenceAudio, m_TextToSpeechReferenceAudio) : m_TextToSpeechReferenceAudio;
    config.promptText = useSpeechToTextSendVoiceProfile && !TrimCopy(m_SpeechToTextSendVoicePromptText).empty()
        ? m_SpeechToTextSendVoicePromptText
        : m_TextToSpeechPromptText;
    config.promptLanguage = useSpeechToTextSendVoiceProfile ? pickSpec(m_SpeechToTextSendVoicePromptLanguage, m_TextToSpeechPromptLanguage) : m_TextToSpeechPromptLanguage;
    config.textLanguage = useSpeechToTextSendVoiceProfile ? pickSpec(m_SpeechToTextSendVoiceLanguage, m_TextToSpeechLanguage) : m_TextToSpeechLanguage;
    config.textSplitMethod = useSpeechToTextSendVoiceProfile ? pickSpec(m_SpeechToTextSendVoiceTextSplitMethod, m_TextToSpeechTextSplitMethod) : m_TextToSpeechTextSplitMethod;
    config.serverPort = std::clamp(m_TextToSpeechServerPort, 1, 65535);
    config.volume = std::clamp(m_TextToSpeechVolume, 0.0f, 2.0f);
    config.includeSpeakerName = m_TextToSpeechIncludeSpeakerName;

    config.resolvedPrefix = BuildProcessPrefix(config.commandPrefixSpec);
    config.resolvedWorkingDir = ResolveVrDirectoryPath(config.workingDirSpec);
    config.resolvedReferenceAudioPath = ResolveVrPath(config.referenceAudioSpec);
    config.resolvedConfigPath = ResolveVrPath(config.modelSpec);
    TryLoadTextToSpeechHotSwitchProfile(config);
    config.resolvedLaunchConfigPath = config.hotSwitchProfileValid
        ? BuildSpeechCacheFixedPath("gpt_sovits_runtime.yaml")
        : config.resolvedConfigPath;
    if (config.resolvedLaunchConfigPath.empty())
        config.resolvedLaunchConfigPath = config.resolvedConfigPath;

    return config;
}

bool VR::EnsureTextToSpeechServerReady(const TextToSpeechRuntimeConfig& config)
{
    if (config.resolvedPrefix.empty() || config.resolvedWorkingDir.empty() || config.resolvedConfigPath.empty() || config.resolvedLaunchConfigPath.empty())
    {
        Game::logMsg("[Speech][TTS] missing GPT-SoVITS command, working dir, or config");
        Game::logMsg("[Speech][TTS] raw prefix='%s' raw workingDir='%s' raw config='%s' raw launchConfig='%s'",
            config.commandPrefixSpec.c_str(),
            config.workingDirSpec.c_str(),
            config.modelSpec.c_str(),
            config.resolvedLaunchConfigPath.c_str());
        return false;
    }

    const std::string launchSignature = BuildTextToSpeechServerLaunchSignature(config);
    const std::string modelSignature = BuildTextToSpeechServerModelSignature(config);

    HANDLE staleProcess = nullptr;
    HANDLE staleJob = nullptr;
    DWORD staleProcessId = 0;
    bool shouldHotSwitchModel = false;

    {
        std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
        if (m_TextToSpeechServerProcess && !IsProcessHandleRunning(m_TextToSpeechServerProcess))
        {
            CloseOwnedHandle(m_TextToSpeechServerJob);
            CloseOwnedProcessHandle(m_TextToSpeechServerProcess, m_TextToSpeechServerProcessId);
            m_TextToSpeechServerLaunchSignature.clear();
            m_TextToSpeechServerModelSignature.clear();
        }

        if (m_TextToSpeechServerProcess && m_TextToSpeechServerLaunchSignature != launchSignature)
        {
            staleProcess = m_TextToSpeechServerProcess;
            staleJob = m_TextToSpeechServerJob;
            staleProcessId = m_TextToSpeechServerProcessId;
            m_TextToSpeechServerProcess = nullptr;
            m_TextToSpeechServerJob = nullptr;
            m_TextToSpeechServerProcessId = 0;
            m_TextToSpeechServerLaunchSignature.clear();
            m_TextToSpeechServerModelSignature.clear();
        }
    }

    auto stopDetachedServer = [&](HANDLE processHandle, HANDLE jobHandle, DWORD processId)
        {
            if (!processHandle)
                return;

            CloseOwnedHandle(jobHandle);
            if (IsProcessHandleRunning(processHandle))
            {
                ::TerminateProcess(processHandle, 0);
                ::WaitForSingleObject(processHandle, 5000);
            }
            CloseOwnedProcessHandle(processHandle, processId);
        };

    if (staleProcess)
        stopDetachedServer(staleProcess, staleJob, staleProcessId);

    auto waitForServerReady = [&]() -> bool
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (std::chrono::steady_clock::now() < deadline)
            {
                DWORD statusCode = 0;
                std::vector<char> responseBody;
                if (HttpRequest("127.0.0.1", static_cast<INTERNET_PORT>(config.serverPort), L"GET", L"/docs", nullptr, nullptr, 1500, statusCode, responseBody)
                    && statusCode == 200)
                {
                    return true;
                }

                {
                    std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
                    if (m_TextToSpeechServerProcess && !IsProcessHandleRunning(m_TextToSpeechServerProcess))
                    {
                        CloseOwnedHandle(m_TextToSpeechServerJob);
                        CloseOwnedProcessHandle(m_TextToSpeechServerProcess, m_TextToSpeechServerProcessId);
                        m_TextToSpeechServerLaunchSignature.clear();
                        m_TextToSpeechServerModelSignature.clear();
                        break;
                    }
                }

                ::Sleep(250);
            }

            Game::logMsg("[Speech][TTS] GPT-SoVITS server did not become ready");
            return false;
        };

    auto startServer = [&]() -> bool
        {
            if (config.resolvedLaunchConfigPath != config.resolvedConfigPath)
            {
                const std::string configContents = ReadWholeTextFile(config.resolvedConfigPath);
                if (configContents.empty() || !WriteWholeTextFileIfChanged(config.resolvedLaunchConfigPath, configContents))
                {
                    Game::logMsg("[Speech][TTS] failed to prepare runtime config copy: %s -> %s",
                        config.resolvedConfigPath.c_str(),
                        config.resolvedLaunchConfigPath.c_str());
                    return false;
                }
            }

            TryStopExistingTextToSpeechHttpServer(static_cast<INTERNET_PORT>(config.serverPort));

            std::string commandLine = config.resolvedPrefix
                + " -a 127.0.0.1 -p " + std::to_string(config.serverPort)
                + " -c " + QuoteProcessArg(config.resolvedLaunchConfigPath);

            HANDLE processHandle = nullptr;
            DWORD processId = 0;
            if (!StartProcessHidden(commandLine, config.resolvedWorkingDir, processHandle, processId))
            {
                Game::logMsg("[Speech][TTS] failed to launch GPT-SoVITS server: %s", commandLine.c_str());
                return false;
            }

            HANDLE jobHandle = ::CreateJobObjectA(nullptr, nullptr);
            if (jobHandle)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
                jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!::SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo))
                    || !::AssignProcessToJobObject(jobHandle, processHandle))
                {
                    CloseOwnedHandle(jobHandle);
                    Game::logMsg("[Speech][TTS] warning: GPT-SoVITS process is running without job-object auto-cleanup");
                }
            }
            else
            {
                Game::logMsg("[Speech][TTS] warning: failed to create GPT-SoVITS job object");
            }

            {
                std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
                if (!m_TextToSpeechServerProcess)
                {
                    m_TextToSpeechServerProcess = processHandle;
                    m_TextToSpeechServerJob = jobHandle;
                    m_TextToSpeechServerProcessId = processId;
                    m_TextToSpeechServerLaunchSignature = launchSignature;
                    m_TextToSpeechServerModelSignature = modelSignature;
                    processHandle = nullptr;
                    jobHandle = nullptr;
                    processId = 0;
                }
            }

            if (jobHandle)
                CloseOwnedHandle(jobHandle);
            if (processHandle)
                CloseOwnedProcessHandle(processHandle, processId);

            Game::logMsg("[Speech][TTS] started GPT-SoVITS server on 127.0.0.1:%d", config.serverPort);
            return true;
        };

    auto detachRunningServer = [&](HANDLE& outProcess, HANDLE& outJob, DWORD& outProcessId)
        {
            outProcess = nullptr;
            outJob = nullptr;
            outProcessId = 0;

            std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
            if (!m_TextToSpeechServerProcess)
                return;

            outProcess = m_TextToSpeechServerProcess;
            outJob = m_TextToSpeechServerJob;
            outProcessId = m_TextToSpeechServerProcessId;
            m_TextToSpeechServerProcess = nullptr;
            m_TextToSpeechServerJob = nullptr;
            m_TextToSpeechServerProcessId = 0;
            m_TextToSpeechServerLaunchSignature.clear();
            m_TextToSpeechServerModelSignature.clear();
        };

    bool shouldStartServer = false;
    {
        std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
        shouldStartServer = m_TextToSpeechServerProcess == nullptr;
        shouldHotSwitchModel = !shouldStartServer && m_TextToSpeechServerModelSignature != modelSignature;
    }

    if (shouldStartServer)
    {
        if (!startServer())
            return false;
    }

    if (!waitForServerReady())
        return false;

    if (shouldHotSwitchModel)
    {
        Game::logMsg("[Speech][TTS] hot-switching GPT-SoVITS model: version=%s",
            config.resolvedModelVersion.empty() ? "<unknown>" : config.resolvedModelVersion.c_str());

        if (!TryHotSwitchTextToSpeechServerModel(static_cast<INTERNET_PORT>(config.serverPort), config))
        {
            Game::logMsg("[Speech][TTS] hot-switch failed, restarting GPT-SoVITS server");

            HANDLE runningProcess = nullptr;
            HANDLE runningJob = nullptr;
            DWORD runningProcessId = 0;
            detachRunningServer(runningProcess, runningJob, runningProcessId);
            stopDetachedServer(runningProcess, runningJob, runningProcessId);

            if (!startServer())
                return false;

            if (!waitForServerReady())
                return false;
        }
        else
        {
            std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
            m_TextToSpeechServerModelSignature = modelSignature;
            if (!m_TextToSpeechServerLaunchSignature.empty())
            {
                Game::logMsg("[Speech][TTS] hot-switched GPT-SoVITS model to %s",
                    config.resolvedModelVersion.empty() ? config.resolvedVitsWeightsPath.c_str() : config.resolvedModelVersion.c_str());
            }
        }
    }

    return true;
}

void VR::ShutdownTextToSpeechServer()
{
    HANDLE processHandle = nullptr;
    HANDLE jobHandle = nullptr;
    DWORD processId = 0;
    {
        std::lock_guard<std::mutex> lock(m_TextToSpeechServerMutex);
        if (!m_TextToSpeechServerProcess)
            return;

        processHandle = m_TextToSpeechServerProcess;
        jobHandle = m_TextToSpeechServerJob;
        processId = m_TextToSpeechServerProcessId;
        m_TextToSpeechServerProcess = nullptr;
        m_TextToSpeechServerJob = nullptr;
        m_TextToSpeechServerProcessId = 0;
        m_TextToSpeechServerLaunchSignature.clear();
        m_TextToSpeechServerModelSignature.clear();
    }

    CloseOwnedHandle(jobHandle);
    if (processHandle && IsProcessHandleRunning(processHandle))
    {
        const DWORD waitResult = ::WaitForSingleObject(processHandle, 5000);
        if (waitResult == WAIT_TIMEOUT)
        {
            ::TerminateProcess(processHandle, 0);
            ::WaitForSingleObject(processHandle, 5000);
        }
    }

    CloseOwnedProcessHandle(processHandle, processId);
    Game::logMsg("[Speech][TTS] GPT-SoVITS server stopped");
}

void VR::UpdateVoiceRecordCommandState()
{
    if (!m_Game)
        return;

    const bool wantVoiceRecord = m_VoiceRecordActive || m_AutoVoiceRecordRequested;
    if (wantVoiceRecord && !m_VoiceRecordCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("+voicerecord");
        m_VoiceRecordCmdOwned = true;
    }
    else if (!wantVoiceRecord && m_VoiceRecordCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-voicerecord");
        m_VoiceRecordCmdOwned = false;
    }
}

void VR::StopVoiceRecordCommandNow(bool disableVoiceInputFromFile)
{
    m_VoiceRecordActive = false;
    m_AutoVoiceRecordRequested = false;
    m_SpeechVoiceBroadcastActive = false;
    m_SpeechVoiceBroadcastStopAt = {};

    if (m_Game)
    {
        if (disableVoiceInputFromFile)
            m_Game->ClientCmd_Unrestricted("voice_inputfromfile 0");
        if (m_SpeechVoiceLoopbackCmdOwned)
            m_Game->ClientCmd_Unrestricted("voice_loopback 0");
    }
    m_SpeechVoiceLoopbackCmdOwned = false;

    if (m_Game && m_VoiceRecordCmdOwned)
        m_Game->ClientCmd_Unrestricted("-voicerecord");
    m_VoiceRecordCmdOwned = false;
}

void VR::SpeechWorkerMain()
{
    for (;;)
    {
        SpeechWorkerJob job{};
        {
            std::unique_lock<std::mutex> lock(m_SpeechWorkerMutex);
            m_SpeechWorkerCv.wait(lock, [&]()
                {
                    return !m_SpeechWorkerJobs.empty();
                });

            job = std::move(m_SpeechWorkerJobs.front());
            m_SpeechWorkerJobs.pop_front();
        }

        if (job.type == SpeechWorkerJob::Type::TranscribeWave)
        {
            const std::string prefix = BuildProcessPrefix(m_SpeechToTextCommandPrefix);
            const std::string modelPath = ResolveVrPath(m_SpeechToTextModel);
            if (prefix.empty() || modelPath.empty())
            {
                Game::logMsg("[Speech][STT] missing command prefix or model path");
                if (!job.inputPath.empty())
                    ::DeleteFileA(job.inputPath.c_str());
                continue;
            }

            const std::string language = TrimCopy(m_SpeechToTextLanguage);
            std::string commandLine = prefix
                + " -m " + QuoteProcessArg(modelPath)
                + " -f " + QuoteProcessArg(job.inputPath)
                + " -otxt -of " + QuoteProcessArg(job.outputPath)
                + " -np -nt";
            if (!language.empty())
                commandLine += " -l " + QuoteProcessArg(language);

            DWORD exitCode = 0;
            if (!RunProcessHidden(commandLine, exitCode))
            {
                Game::logMsg("[Speech][STT] failed to launch: %s", commandLine.c_str());
                if (!job.inputPath.empty())
                    ::DeleteFileA(job.inputPath.c_str());
                continue;
            }

            const std::string transcriptPath = job.outputPath + ".txt";
            const std::string transcript = CollapseWhitespace(ReadWholeTextFile(transcriptPath));
            if (!job.inputPath.empty())
                ::DeleteFileA(job.inputPath.c_str());
            if (!transcriptPath.empty())
                ::DeleteFileA(transcriptPath.c_str());

            if (exitCode != 0)
            {
                Game::logMsg("[Speech][STT] process exit code %lu", static_cast<unsigned long>(exitCode));
                continue;
            }

            if (transcript.empty())
            {
                Game::logMsg("[Speech][STT] empty transcript");
                continue;
            }

            Game::logMsg("[Speech][STT] %s", transcript.c_str());
            if (m_SpeechToTextSendChatEnabled)
            {
                std::lock_guard<std::mutex> lock(m_SpeechResultMutex);
                if (m_PendingSpeechToTextChatMessages.size() >= 8)
                    m_PendingSpeechToTextChatMessages.pop_front();
                m_PendingSpeechToTextChatMessages.push_back(transcript);
            }

            if (m_SpeechToTextSendVoiceEnabled)
                QueueSpeechToTextVoicePlayback(transcript);
            continue;
        }

        if (job.type == SpeechWorkerJob::Type::SpeakText)
        {
            if (!m_TextToSpeechEnabled && !job.allowWhenTextToSpeechDisabled)
                continue;

            const TextToSpeechRuntimeConfig ttsConfig = BuildTextToSpeechRuntimeConfig(job.useSpeechToTextSendVoiceProfile);
            std::string text = CollapseWhitespace(job.text);
            std::string speaker = CollapseWhitespace(job.speaker);
            if (text.empty())
                continue;

            if (job.includeSpeakerName && ttsConfig.includeSpeakerName && !speaker.empty())
                text = speaker + ": " + text;

            const std::string refAudioPath = ttsConfig.resolvedReferenceAudioPath;
            if (refAudioPath.empty())
            {
                Game::logMsg("[Speech][TTS] missing GPT-SoVITS reference audio");
                continue;
            }

            if (job.outputPath.empty())
            {
                Game::logMsg("[Speech][TTS] failed to allocate temp output path");
                continue;
            }

            if (!EnsureTextToSpeechServerReady(ttsConfig))
                continue;

            const std::string textLanguage = TrimCopy(ttsConfig.textLanguage);
            const std::string promptLanguage = TrimCopy(ttsConfig.promptLanguage);
            const std::string promptText = TrimCopy(ttsConfig.promptText);
            const std::string splitMethod = TrimCopy(ttsConfig.textSplitMethod);
            if (textLanguage.empty() || promptLanguage.empty())
            {
                Game::logMsg("[Speech][TTS] missing GPT-SoVITS text language or prompt language");
                continue;
            }

            std::ostringstream requestBody;
            requestBody
                << "{"
                << "\"text\":\"" << EscapeJsonString(text) << "\","
                << "\"text_lang\":\"" << EscapeJsonString(textLanguage) << "\","
                << "\"ref_audio_path\":\"" << EscapeJsonString(refAudioPath) << "\","
                << "\"prompt_text\":\"" << EscapeJsonString(promptText) << "\","
                << "\"prompt_lang\":\"" << EscapeJsonString(promptLanguage) << "\","
                << "\"text_split_method\":\"" << EscapeJsonString(splitMethod.empty() ? std::string("cut5") : splitMethod) << "\","
                << "\"media_type\":\"wav\","
                << "\"streaming_mode\":false"
                << "}";

            const std::string requestJson = requestBody.str();
            DWORD statusCode = 0;
            std::vector<char> responseBody;
            const wchar_t* headers = L"Content-Type: application/json; charset=utf-8\r\nAccept: audio/wav\r\n";
            if (!HttpRequest("127.0.0.1", static_cast<INTERNET_PORT>(std::clamp(ttsConfig.serverPort, 1, 65535)), L"POST", L"/tts", &requestJson, headers, 120000, statusCode, responseBody))
            {
                Game::logMsg("[Speech][TTS] HTTP request to GPT-SoVITS failed");
                continue;
            }

            if (statusCode != 200)
            {
                const std::string responsePreview = TrimHttpResponseForLog(responseBody);
                Game::logMsg("[Speech][TTS] GPT-SoVITS returned HTTP %lu%s%s",
                    static_cast<unsigned long>(statusCode),
                    responsePreview.empty() ? "" : ": ",
                    responsePreview.empty() ? "" : responsePreview.c_str());
                continue;
            }

            if (!WriteBinaryFile(job.outputPath, responseBody))
            {
                Game::logMsg("[Speech][TTS] failed to write %s", job.outputPath.c_str());
                continue;
            }

            if (job.sendToVoiceChat)
                QueueGeneratedSpeechVoiceBroadcast(job.outputPath);

            if (job.playLocally)
            {
                const int playbackVolume = std::clamp(
                    static_cast<int>(std::lround(std::clamp(ttsConfig.volume, 0.0f, 2.0f) * 1000.0f)),
                    0,
                    1000);
                if (playbackVolume > 0)
                    EnqueueFeedbackSoundPlayback(job.outputPath, playbackVolume, playbackVolume, false);
            }
        }
    }
}

void VR::PumpSpeechToTextResults()
{
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(m_SpeechResultMutex);
        pending.swap(m_PendingSpeechToTextChatMessages);
    }

    for (const std::string& rawText : pending)
    {
        const std::string chatText = SanitizeTextForSourceSayCommand(rawText);
        if (chatText.empty() || !m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
            continue;

        const std::string command = "say \"" + chatText + "\"";
        m_Game->ClientCmd_Unrestricted(command.c_str());
        Game::logMsg("[Speech][STT] sent chat: %s", chatText.c_str());
    }
}

void VR::QueueSpeechToTextVoicePlayback(const std::string& text)
{
    std::string collapsedText = CollapseWhitespace(text);
    if (collapsedText.empty())
        return;

    EnsureSpeechWorkerThread();
    if (!m_SpeechWorkerStarted.load())
        return;

    SpeechWorkerJob job{};
    job.type = SpeechWorkerJob::Type::SpeakText;
    job.text = collapsedText;
    job.outputPath = BuildSpeechCachePath("stt_tts_output", ".wav");
    job.allowWhenTextToSpeechDisabled = true;
    job.includeSpeakerName = false;
    job.playLocally = true;
    job.sendToVoiceChat = true;
    job.useSpeechToTextSendVoiceProfile = true;
    if (job.outputPath.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(m_SpeechWorkerMutex);
        if (m_SpeechWorkerJobs.size() >= 16)
        {
            auto it = std::find_if(
                m_SpeechWorkerJobs.begin(),
                m_SpeechWorkerJobs.end(),
                [](const SpeechWorkerJob& queuedJob)
                {
                    return queuedJob.type == SpeechWorkerJob::Type::SpeakText;
                });
            if (it != m_SpeechWorkerJobs.end())
                m_SpeechWorkerJobs.erase(it);
            else
                m_SpeechWorkerJobs.pop_front();
        }

        m_SpeechWorkerJobs.push_back(std::move(job));
    }

    m_SpeechWorkerCv.notify_one();
}

void VR::QueueGeneratedSpeechVoiceBroadcast(const std::string& wavPath)
{
    if (wavPath.empty())
        return;

    std::lock_guard<std::mutex> lock(m_SpeechVoiceBroadcastMutex);
    if (m_PendingSpeechVoiceBroadcasts.size() >= 4)
        m_PendingSpeechVoiceBroadcasts.pop_front();
    m_PendingSpeechVoiceBroadcasts.push_back({ wavPath });
}

void VR::PumpSpeechToTextVoiceBroadcast()
{
    if (!m_SpeechToTextSendVoiceEnabled)
    {
        {
            std::lock_guard<std::mutex> lock(m_SpeechVoiceBroadcastMutex);
            m_PendingSpeechVoiceBroadcasts.clear();
        }

        if (m_SpeechVoiceBroadcastActive || m_AutoVoiceRecordRequested)
        {
            m_AutoVoiceRecordRequested = false;
            m_SpeechVoiceBroadcastActive = false;
            m_SpeechVoiceBroadcastStopAt = {};
            if (m_Game)
            {
                m_Game->ClientCmd_Unrestricted("voice_inputfromfile 0");
                if (m_SpeechVoiceLoopbackCmdOwned)
                    m_Game->ClientCmd_Unrestricted("voice_loopback 0");
            }
            m_SpeechVoiceLoopbackCmdOwned = false;
            UpdateVoiceRecordCommandState();
        }
        return;
    }

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
    {
        if (m_SpeechVoiceBroadcastActive || m_AutoVoiceRecordRequested)
            StopVoiceRecordCommandNow(true);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_SpeechVoiceBroadcastActive && now >= m_SpeechVoiceBroadcastStopAt)
    {
        m_SpeechVoiceBroadcastActive = false;
        m_AutoVoiceRecordRequested = false;
        m_SpeechVoiceBroadcastStopAt = {};
        m_Game->ClientCmd_Unrestricted("voice_inputfromfile 0");
        if (m_SpeechVoiceLoopbackCmdOwned)
            m_Game->ClientCmd_Unrestricted("voice_loopback 0");
        m_SpeechVoiceLoopbackCmdOwned = false;
        UpdateVoiceRecordCommandState();
        Game::logMsg("[Speech][VoiceSend] playback finished");
    }

    if (m_SpeechVoiceBroadcastActive || m_SpeechToTextCaptureActive || m_VoiceRecordActive || m_SuppressPlayerInput)
        return;

    std::string queuedWavePath;
    {
        std::lock_guard<std::mutex> lock(m_SpeechVoiceBroadcastMutex);
        if (m_PendingSpeechVoiceBroadcasts.empty())
            return;

        queuedWavePath = std::move(m_PendingSpeechVoiceBroadcasts.front().wavPath);
        m_PendingSpeechVoiceBroadcasts.pop_front();
    }

    if (queuedWavePath.empty())
        return;

    std::string voiceInputPath;
    float durationSeconds = 0.0f;
    if (!PrepareVoiceInputWaveFile(queuedWavePath, voiceInputPath, durationSeconds))
    {
        Game::logMsg("[Speech][VoiceSend] failed to prepare voice_input.wav from %s", queuedWavePath.c_str());
        return;
    }

    constexpr float kVoiceSendPaddingSeconds = 0.20f;
    m_Game->ClientCmd_Unrestricted("voice_inputfromfile 1");
    if (m_SpeechToTextSendVoiceLoopbackEnabled && !m_SpeechVoiceLoopbackCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("voice_loopback 1");
        m_SpeechVoiceLoopbackCmdOwned = true;
    }
    m_AutoVoiceRecordRequested = true;
    m_SpeechVoiceBroadcastActive = true;
    m_SpeechVoiceBroadcastStopAt = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(std::max(0.10f, durationSeconds + kVoiceSendPaddingSeconds)));
    UpdateVoiceRecordCommandState();
    Game::logMsg("[Speech][VoiceSend] broadcasting %.2fs at %u Hz from %s%s",
        durationSeconds,
        static_cast<unsigned>(kSourceVoiceInputSampleRate),
        voiceInputPath.c_str(),
        m_SpeechVoiceLoopbackCmdOwned ? " (voice_loopback=1)" : "");
}

void VR::QueueChatTextToSpeech(const std::string& speaker, const std::string& text)
{
    if (!m_TextToSpeechEnabled)
        return;

    std::string collapsedText = CollapseWhitespace(text);
    if (collapsedText.empty())
        return;

    std::string collapsedSpeaker = CollapseWhitespace(speaker);
    if (m_TextToSpeechSkipOwnMessages && !collapsedSpeaker.empty() && m_Game && m_Game->m_EngineClient)
    {
        const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        char localPlayerName[128] = {};
        if (localPlayerIndex > 0 && GetPlayerNameUtf8Safe(m_Game->m_EngineClient, localPlayerIndex, localPlayerName, sizeof(localPlayerName)))
        {
            const std::string localName = NormalizeHudChatSpeakerForPlayerLookup(localPlayerName);
            const std::string speakerName = NormalizeHudChatSpeakerForPlayerLookup(collapsedSpeaker);
            if (!localName.empty() && !speakerName.empty() && _stricmp(localName.c_str(), speakerName.c_str()) == 0)
                return;
        }
    }

    bool whitelistMatched = false;
    std::string whitelistMatchedText;
    if (!m_TextToSpeechWhitelistRegexes.empty())
    {
        whitelistMatched = TryResolveTextToSpeechWhitelistMatch(
            collapsedText,
            m_TextToSpeechWhitelistRegexes,
            m_TextToSpeechWhitelistSeparator,
            whitelistMatchedText);
    }

    if (whitelistMatched)
    {
        collapsedText = whitelistMatchedText;
        collapsedSpeaker.clear();
    }
    else if (m_TextToSpeechSurvivorOnly)
    {
        int speakerTeam = 0;
        if (collapsedSpeaker.empty() || !TryGetPlayerTeamByNormalizedName(m_Game, collapsedSpeaker, speakerTeam) || speakerTeam != 2)
            return;
    }

    EnsureSpeechWorkerThread();
    if (!m_SpeechWorkerStarted.load())
        return;

    SpeechWorkerJob job{};
    job.type = SpeechWorkerJob::Type::SpeakText;
    job.speaker = collapsedSpeaker;
    job.text = collapsedText;
    job.includeSpeakerName = !whitelistMatched;
    job.outputPath = BuildSpeechCachePath("tts_output", ".wav");
    if (job.outputPath.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(m_SpeechWorkerMutex);
        if (m_SpeechWorkerJobs.size() >= 16)
        {
            auto it = std::find_if(
                m_SpeechWorkerJobs.begin(),
                m_SpeechWorkerJobs.end(),
                [](const SpeechWorkerJob& queuedJob)
                {
                    return queuedJob.type == SpeechWorkerJob::Type::SpeakText;
                });
            if (it != m_SpeechWorkerJobs.end())
                m_SpeechWorkerJobs.erase(it);
            else
                m_SpeechWorkerJobs.pop_front();
        }

        m_SpeechWorkerJobs.push_back(std::move(job));
    }

    m_SpeechWorkerCv.notify_one();
}

void VR::HandleHudChatLine(const std::string& speaker, const std::string& text)
{
    QueueChatTextToSpeech(speaker, text);
}

void VR::ComputeFeedbackSoundStereoVolumes(const Vector* worldPos, float baseVolume, int& outLeftVolume, int& outRightVolume) const
{
    const float gameMasterVolume = ReadGameMasterVolumeFromConfig(m_Game ? m_Game->m_EngineClient : nullptr);
    const float clampedBaseVolume = std::clamp(baseVolume * std::clamp(gameMasterVolume, 0.0f, 1.0f), 0.0f, 2.0f);
    float leftGain = clampedBaseVolume;
    float rightGain = clampedBaseVolume;

    if (worldPos && m_FeedbackSoundSpatialBlend > 0.0f)
    {
        Vector listenerForward{};
        Vector listenerRight{};
        Vector listenerUp{};
        Vector listenerOrigin = m_SetupOrigin;
        if (!m_HmdForward.IsZero() && !m_HmdRight.IsZero() && !m_HmdUp.IsZero())
        {
            listenerForward = m_HmdForward;
            listenerRight = m_HmdRight;
            listenerUp = m_HmdUp;
        }
        else
        {
            QAngle::AngleVectors(m_SetupAngles, &listenerForward, &listenerRight, &listenerUp);
        }

        if (!listenerForward.IsZero() && !listenerRight.IsZero())
        {
            VectorNormalize(listenerForward);
            VectorNormalize(listenerRight);
            if (!m_HmdForward.IsZero() && !m_HmdRight.IsZero() && !m_HmdUp.IsZero())
                listenerOrigin = m_HmdPosAbs;

            // Feedback sounds should follow the player's actual head pose, not the render camera basis.
            const Vector delta = *worldPos - listenerOrigin;
            const float distance = delta.Length();
            if (distance > 0.001f)
            {
                Vector dir = delta;
                VectorNormalize(dir);

                const float spatialBlend = Clamp01(m_FeedbackSoundSpatialBlend);
                const float pan = std::clamp(-DotProduct(dir, listenerRight), -1.0f, 1.0f) * spatialBlend;
                const float normalizedPan = pan * 0.5f + 0.5f;
                const float panAngle = normalizedPan * (kPi * 0.5f);
                const float leftPan = std::cos(panAngle) * 1.41421356f;
                const float rightPan = std::sin(panAngle) * 1.41421356f;

                const float nearDistance = 96.0f;
                const float farDistance = (std::max)(nearDistance + 1.0f, m_FeedbackSoundSpatialRange);
                const float distanceT = Clamp01((distance - nearDistance) / (farDistance - nearDistance));
                const float distanceGain = 1.0f - 0.65f * spatialBlend * std::sqrt(distanceT);

                leftGain *= distanceGain * Lerp(1.0f, leftPan, spatialBlend);
                rightGain *= distanceGain * Lerp(1.0f, rightPan, spatialBlend);
            }
        }
    }

    outLeftVolume = std::clamp(static_cast<int>(std::lround(leftGain * 1000.0f)), 0, 1000);
    outRightVolume = std::clamp(static_cast<int>(std::lround(rightGain * 1000.0f)), 0, 1000);

    if (m_FeedbackSoundDebugForceChannel != 0)
    {
        const int dominantVolume = (std::max)(outLeftVolume, outRightVolume);
        if (m_FeedbackSoundDebugForceChannel < 0)
        {
            outLeftVolume = dominantVolume;
            outRightVolume = 0;
        }
        else
        {
            outLeftVolume = 0;
            outRightVolume = dominantVolume;
        }
    }
}

void VR::PlayHitSound(const Vector* worldPos)
{
    if (!m_HitSoundEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_HitSoundPlaybackCooldownSeconds > 0.0f && m_LastHitSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastHitSoundPlaybackTime).count();
        if (elapsed < m_HitSoundPlaybackCooldownSeconds)
            return;
    }

    bool played = TryPlayKillSoundSpec(m_HitSoundSpec, m_HitSoundVolume, worldPos, false);
    if (!played)
    {
        EnsureFeedbackSoundWarmup();
        played = TryPlayKillSoundSpec(m_HitSoundSpec, m_HitSoundVolume, worldPos, false);
    }
    if (!played)
        MessageBeep(MB_ICONQUESTION);

    m_LastHitSoundPlaybackTime = now;
}

void VR::QueueHitSoundPlayback(const Vector* worldPos, uint32_t shotSerial, std::uintptr_t entityTag)
{
    const auto now = std::chrono::steady_clock::now();
    const Vector queuedPos = worldPos ? *worldPos : Vector{};

    auto isSameImpactAs = [&](const Vector& rhs) -> bool
        {
            if (!worldPos || rhs.IsZero())
                return false;
            // Keep this tight: it is only meant to catch prediction replays of the same bullet,
            // not hide real follow-up shots on the same infected.
            return (queuedPos - rhs).LengthSqr() <= 144.0f;
        };

    auto recordSuppressed = [&]()
        {
            ++m_HitSoundStatsMerged;
            ++m_HitSoundPendingMergedCount;
        };

    // If a sound has just been emitted, do not keep another pending sound alive until
    // the cooldown expires. Remote-server prediction replays were using that delayed
    // pending path and turned one hit into several staggered hit sounds.
    if (m_HitSoundPlaybackCooldownSeconds > 0.0f && m_LastHitSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastHitSoundPlaybackTime).count();
        if (elapsed >= 0.0f && elapsed < m_HitSoundPlaybackCooldownSeconds)
        {
            recordSuppressed();
            return;
        }
    }

    if (shotSerial != 0)
    {
        const bool shotAlreadyPlayed = std::find(
            m_RecentHitSoundPlaybackShotSerials.begin(),
            m_RecentHitSoundPlaybackShotSerials.end(),
            shotSerial) != m_RecentHitSoundPlaybackShotSerials.end();
        if (shotAlreadyPlayed)
        {
            recordSuppressed();
            return;
        }
    }

    if (m_HitSoundPending)
    {
        const bool samePendingShot = shotSerial != 0 && m_HitSoundPendingShotSerial == shotSerial;
        const bool samePendingEntity = entityTag != 0 && m_HitSoundPendingEntityTag == entityTag;
        if (samePendingShot || (samePendingEntity && isSameImpactAs(m_HitSoundPendingWorldPos)))
        {
            recordSuppressed();
            if (worldPos)
                m_HitSoundPendingWorldPos = queuedPos;
            return;
        }

        ++m_HitSoundStatsMerged;
        ++m_HitSoundPendingMergedCount;
        if (worldPos)
            m_HitSoundPendingWorldPos = queuedPos;
        if (shotSerial != 0)
            m_HitSoundPendingShotSerial = shotSerial;
        if (entityTag != 0)
            m_HitSoundPendingEntityTag = entityTag;
        return;
    }

    // Some remote servers cause ClientFireTerrorBullets to be replayed with a new local
    // shot serial but the same impact a few frames later. Suppress only very-near duplicate
    // impact signatures; normal automatic fire still passes because the window is short.
    if (m_LastHitSoundQueueAcceptedTime.time_since_epoch().count() != 0)
    {
        constexpr float kQueueReplayDedupeSeconds = 0.090f;
        const float elapsed = std::chrono::duration<float>(now - m_LastHitSoundQueueAcceptedTime).count();
        const bool inReplayWindow = elapsed >= 0.0f && elapsed <= kQueueReplayDedupeSeconds;
        const bool sameRecentEntity = entityTag != 0 && m_LastHitSoundQueueAcceptedEntityTag == entityTag;
        if (inReplayWindow && sameRecentEntity && isSameImpactAs(m_LastHitSoundQueueAcceptedWorldPos))
        {
            recordSuppressed();
            return;
        }
    }

    m_HitSoundPending = true;
    ++m_HitSoundStatsQueued;
    m_HitSoundPendingMergedCount = 1;
    m_HitSoundPendingShotSerial = shotSerial;
    m_HitSoundPendingEntityTag = entityTag;
    m_HitSoundPendingQueuedAt = now;
    m_HitSoundPendingWorldPos = queuedPos;
    m_LastHitSoundQueueAcceptedTime = now;
    m_LastHitSoundQueueAcceptedWorldPos = queuedPos;
    m_LastHitSoundQueueAcceptedEntityTag = entityTag;
}

void VR::FlushPendingHitSound(std::chrono::steady_clock::time_point now)
{
    if (!m_HitSoundPending || !m_HitSoundEnabled)
        return;

    if (m_HitSoundPlaybackCooldownSeconds > 0.0f && m_LastHitSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastHitSoundPlaybackTime).count();
        if (elapsed >= 0.0f && elapsed < m_HitSoundPlaybackCooldownSeconds)
        {
            // Do not delay a replayed hit until the cooldown ends. Drop it here; otherwise a
            // single server-prediction replay can become an audible echo after the cooldown.
            ++m_HitSoundStatsMerged;
            m_HitSoundPending = false;
            m_HitSoundPendingMergedCount = 0;
            m_HitSoundPendingShotSerial = 0;
            m_HitSoundPendingEntityTag = 0;
            m_HitSoundPendingWorldPos = Vector{};
            m_HitSoundPendingQueuedAt = {};
            return;
        }
    }

    const uint32_t playedShotSerial = m_HitSoundPendingShotSerial;
    const Vector* worldPos = m_HitSoundPendingWorldPos.IsZero() ? nullptr : &m_HitSoundPendingWorldPos;
    PlayHitSound(worldPos);
    ++m_HitSoundStatsFlushed;

    if (playedShotSerial != 0)
    {
        m_RecentHitSoundPlaybackShotSerials[
            m_RecentHitSoundPlaybackShotSerialCursor % m_RecentHitSoundPlaybackShotSerials.size()] = playedShotSerial;
        ++m_RecentHitSoundPlaybackShotSerialCursor;
    }

    m_HitSoundPending = false;
    m_HitSoundPendingMergedCount = 0;
    m_HitSoundPendingShotSerial = 0;
    m_HitSoundPendingEntityTag = 0;
    m_HitSoundPendingWorldPos = Vector{};
    m_HitSoundPendingQueuedAt = {};
}

void VR::PlayKillSound(bool headshot, const Vector* worldPos)
{
    if (!m_KillSoundEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_KillSoundPlaybackCooldownSeconds > 0.0f && m_LastKillSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastKillSoundPlaybackTime).count();
        if (elapsed < m_KillSoundPlaybackCooldownSeconds)
            return;
    }

    const std::string& preferredSpec = headshot && !m_KillSoundHeadshotSpec.empty()
        ? m_KillSoundHeadshotSpec
        : m_KillSoundNormalSpec;
    const float preferredVolume = headshot ? m_HeadshotSoundVolume : m_KillSoundVolume;

    auto tryPlayConfiguredKillSounds = [&]() -> bool
        {
            bool result = TryPlayKillSoundSpec(preferredSpec, preferredVolume, worldPos);
            if (!result && headshot && !m_KillSoundNormalSpec.empty())
                result = TryPlayKillSoundSpec(m_KillSoundNormalSpec, m_KillSoundVolume, worldPos);
            return result;
        };

    bool played = tryPlayConfiguredKillSounds();
    if (!played)
    {
        EnsureFeedbackSoundWarmup();
        played = tryPlayConfiguredKillSounds();
    }

    if (!played)
        MessageBeep(headshot ? MB_ICONEXCLAMATION : MB_ICONASTERISK);

    m_LastKillSoundPlaybackTime = now;
}

void VR::EnsureFeedbackSoundWarmup()
{
    const std::string signature = m_HitSoundSpec + "\n" + m_KillSoundNormalSpec + "\n" + m_KillSoundHeadshotSpec;
    if (signature == m_FeedbackSoundWarmupSignature)
        return;

    m_FeedbackSoundWarmupSignature = signature;

    const std::array<std::string, 3> specs =
    {
        m_HitSoundEnabled ? m_HitSoundSpec : std::string{},
        m_KillSoundEnabled ? m_KillSoundNormalSpec : std::string{},
        m_KillSoundEnabled ? m_KillSoundHeadshotSpec : std::string{}
    };

    std::vector<std::string> warmedPaths;
    warmedPaths.reserve(specs.size());

    for (const std::string& spec : specs)
    {
        std::string resolvedPath;
        if (!TryResolveFeedbackSoundFileSpec(spec, resolvedPath))
            continue;

        bool alreadyWarmed = false;
        for (const std::string& existingPath : warmedPaths)
        {
            if (IsSameFeedbackSoundPath(existingPath, resolvedPath))
            {
                alreadyWarmed = true;
                break;
            }
        }
        if (alreadyWarmed)
            continue;

        EnqueueFeedbackSoundWarmupPath(resolvedPath);
        warmedPaths.push_back(resolvedPath);
    }
}

void VR::SyncVrmodFeedbackGameSounds() const
{
    const std::string moduleDir = GetModuleDirectoryA();
    if (moduleDir.empty())
        return;

    const std::string scriptsDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "scripts");
    ::CreateDirectoryA(scriptsDir.c_str(), nullptr);

    const std::string scriptPath = JoinWindowsPath(scriptsDir, "game_sounds_vrmod.txt");
    std::ostringstream script;
    script
        << "\"VR_HitMarker\"\r\n"
        << "{\r\n"
        << "\t\"channel\"\t\t\"CHAN_AUTO\"\r\n"
        << "\t\"volume\"\t\t\"" << FormatFeedbackSoundVolume(m_HitSoundVolume) << "\"\r\n"
        << "\t\"soundlevel\"\t\t\"SNDLVL_NONE\"\r\n"
        << "\t\"pitch\"\t\t\t\"100\"\r\n"
        << "\t\"wave\"\t\t\t\"vrmod/hit.mp3\"\r\n"
        << "}\r\n\r\n"
        << "\"VR_KillMarker\"\r\n"
        << "{\r\n"
        << "\t\"channel\"\t\t\"CHAN_AUTO\"\r\n"
        << "\t\"volume\"\t\t\"" << FormatFeedbackSoundVolume(m_KillSoundVolume) << "\"\r\n"
        << "\t\"soundlevel\"\t\t\"SNDLVL_NONE\"\r\n"
        << "\t\"pitch\"\t\t\t\"100\"\r\n"
        << "\t\"wave\"\t\t\t\"vrmod/kill.mp3\"\r\n"
        << "}\r\n\r\n"
        << "\"VR_HeadshotMarker\"\r\n"
        << "{\r\n"
        << "\t\"channel\"\t\t\"CHAN_AUTO\"\r\n"
        << "\t\"volume\"\t\t\"" << FormatFeedbackSoundVolume(m_HeadshotSoundVolume) << "\"\r\n"
        << "\t\"soundlevel\"\t\t\"SNDLVL_NONE\"\r\n"
        << "\t\"pitch\"\t\t\t\"100\"\r\n"
        << "\t\"wave\"\t\t\t\"vrmod/headshot.mp3\"\r\n"
        << "}\r\n";

}

IMaterial* VR::ResolveHitIndicatorMaterial()
{
    if (m_KillIndicatorHitMaterial && !m_KillIndicatorHitMaterial->IsErrorMaterial())
        return m_KillIndicatorHitMaterial;

    if (!m_Game || !m_Game->m_MaterialSystem)
        return nullptr;

    const std::string materialName = BuildKillIndicatorMaterialName(m_KillIndicatorMaterialBaseSpec, "hit");
    if (materialName.empty())
        return nullptr;

    m_KillIndicatorHitMaterial = m_Game->m_MaterialSystem->FindMaterial(materialName.c_str(), "Other textures", false, nullptr);
    if (!m_KillIndicatorHitMaterial || m_KillIndicatorHitMaterial->IsErrorMaterial())
    {
        m_KillIndicatorHitMaterial = nullptr;
        return nullptr;
    }

    return m_KillIndicatorHitMaterial;
}

IMaterial* VR::ResolveKillIndicatorMaterial(bool headshot)
{
    IMaterial*& cachedMaterial = headshot ? m_KillIndicatorHeadshotMaterial : m_KillIndicatorNormalMaterial;
    if (cachedMaterial && !cachedMaterial->IsErrorMaterial())
        return cachedMaterial;

    if (!m_Game || !m_Game->m_MaterialSystem)
        return nullptr;

    const std::string materialName = BuildKillIndicatorMaterialName(m_KillIndicatorMaterialBaseSpec, headshot ? "headshot" : "kill");
    if (materialName.empty())
        return nullptr;

    cachedMaterial = m_Game->m_MaterialSystem->FindMaterial(materialName.c_str(), "Other textures", false, nullptr);
    if (!cachedMaterial || cachedMaterial->IsErrorMaterial())
    {
        cachedMaterial = nullptr;
        return nullptr;
    }

    return cachedMaterial;
}

void VR::DestroyHandHudWorldQuadTextures()
{
    auto SafeReleaseD3D = [](auto*& ptr)
        {
            if (!ptr)
                return;
            ptr->Release();
            ptr = nullptr;
        };

    SafeReleaseD3D(m_D9LeftWristHudDynSurface);
    SafeReleaseD3D(m_D9LeftWristHudDynTex);
    SafeReleaseD3D(m_D9RightAmmoHudDynSurface);
    SafeReleaseD3D(m_D9RightAmmoHudDynTex);
    m_D9LeftWristHudDynW = m_D9LeftWristHudDynH = 0;
    m_D9RightAmmoHudDynW = m_D9RightAmmoHudDynH = 0;
    std::memset(&m_VKLeftWristHudDyn, 0, sizeof(m_VKLeftWristHudDyn));
    std::memset(&m_VKRightAmmoHudDyn, 0, sizeof(m_VKRightAmmoHudDyn));
}

void VR::DestroyKillIndicatorOverlayTextures()
{
    for (int materialIndex = 0; materialIndex < static_cast<int>(m_KillIndicatorOverlayTextures.size()); ++materialIndex)
        DestroyKillIndicatorOverlayTexture(materialIndex);
}

void VR::DestroyKillIndicatorOverlayTexture(int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(m_KillIndicatorOverlayTextures.size()))
        return;

    auto SafeReleaseD3D = [](auto*& ptr)
        {
            if (!ptr)
                return;
            ptr->Release();
            ptr = nullptr;
        };

    KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
    SafeReleaseD3D(texture.d3dSurface);
    SafeReleaseD3D(texture.d3dTexture);
    texture.width = 0;
    texture.height = 0;
    std::memset(&texture.sharedTexture, 0, sizeof(texture.sharedTexture));
    texture.uploadedFrameIndex = UINT32_MAX;
    texture.uploadedFromDecodedFrames = false;

    for (KillIndicatorOverlaySlot& slot : m_KillIndicatorOverlaySlots)
    {
        if (slot.materialIndex == materialIndex)
        {
            slot.materialIndex = -1;
            slot.visible = false;
        }
    }
}

bool VR::EnsureKillIndicatorOverlayTexture(int materialIndex, int width, int height)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(m_KillIndicatorOverlayTextures.size()) || width <= 0 || height <= 0)
        return false;
    if (!g_D3DVR9)
        return false;

    KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
    if (texture.d3dTexture && (texture.width != width || texture.height != height))
        DestroyKillIndicatorOverlayTexture(materialIndex);
    if (texture.d3dTexture && texture.d3dSurface)
        return true;

    IDirect3DDevice9* device = GetKillIndicatorD3DDevice(this);
    if (!device)
        return false;

    g_D3DVR9->LockDevice();

    HRESULT hr = device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &texture.d3dTexture,
        nullptr);

    if (SUCCEEDED(hr) && texture.d3dTexture)
    {
        texture.d3dTexture->GetSurfaceLevel(0, &texture.d3dSurface);
        if (texture.d3dSurface)
        {
            D3D9_TEXTURE_VR_DESC desc{};
            if (SUCCEEDED(g_D3DVR9->GetVRDesc(texture.d3dSurface, &desc)))
            {
                std::memcpy(&texture.sharedTexture.m_VulkanData, &desc, sizeof(vr::VRVulkanTextureData_t));
                texture.sharedTexture.m_VRTexture.handle = &texture.sharedTexture.m_VulkanData;
                texture.sharedTexture.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
                texture.sharedTexture.m_VRTexture.eType = vr::TextureType_Vulkan;
                texture.width = width;
                texture.height = height;
            }
            else
            {
                DestroyKillIndicatorOverlayTexture(materialIndex);
            }
        }
        else
        {
            DestroyKillIndicatorOverlayTexture(materialIndex);
        }
    }

    g_D3DVR9->UnlockDevice();
    device->Release();

    return texture.d3dTexture != nullptr && texture.d3dSurface != nullptr;
}

bool VR::UploadKillIndicatorOverlayTexture(int materialIndex, const uint8_t* rgba, int width, int height, uint32_t frameIndex, bool fromDecodedFrames)
{
    if (!rgba || width <= 0 || height <= 0)
        return false;
    if (!EnsureKillIndicatorOverlayTexture(materialIndex, width, height))
        return false;
    if (!g_D3DVR9)
        return false;

    KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
    if (!texture.d3dTexture || !texture.d3dSurface)
        return false;

    g_D3DVR9->LockDevice();

    D3DLOCKED_RECT lockedRect{};
    const HRESULT hr = texture.d3dTexture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr) || !lockedRect.pBits)
    {
        g_D3DVR9->UnlockDevice();
        return false;
    }

    uint8_t* dst0 = reinterpret_cast<uint8_t*>(lockedRect.pBits);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* srcRow = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        uint8_t* dstRow = dst0 + static_cast<size_t>(y) * static_cast<size_t>(lockedRect.Pitch);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t r = srcRow[x * 4 + 0];
            const uint8_t g = srcRow[x * 4 + 1];
            const uint8_t b = srcRow[x * 4 + 2];
            const uint8_t a = srcRow[x * 4 + 3];
            dstRow[x * 4 + 0] = b;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = r;
            dstRow[x * 4 + 3] = a;
        }
    }

    texture.d3dTexture->UnlockRect(0);
    const HRESULT transferHr = g_D3DVR9->TransferSurface(texture.d3dSurface, FALSE);
    g_D3DVR9->UnlockDevice();
    if (FAILED(transferHr))
    {
        DestroyKillIndicatorOverlayTexture(materialIndex);
        return false;
    }
    texture.uploadedFrameIndex = frameIndex;
    texture.uploadedFromDecodedFrames = fromDecodedFrames;
    return true;
}

void VR::DestroyKillIndicatorOverlay(ActiveKillIndicator& indicator)
{
    if (indicator.overlaySlot < 0 || indicator.overlaySlot >= static_cast<int>(m_KillIndicatorOverlaySlots.size()))
    {
        indicator.overlaySlot = -1;
        return;
    }

    KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[indicator.overlaySlot];
    if (slot.overlayHandle != vr::k_ulOverlayHandleInvalid)
    {
        vr::IVROverlay* overlay = m_Overlay ? m_Overlay : vr::VROverlay();
        if (overlay)
        {
            std::lock_guard<std::mutex> lock(m_VROverlayMutex);
            overlay->HideOverlay(slot.overlayHandle);
        }
    }

    slot.visible = false;
    indicator.overlaySlot = -1;
}

bool VR::EnsureKillIndicatorOverlaySlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(m_KillIndicatorOverlaySlots.size()))
        return false;

    KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[slotIndex];
    if (slot.overlayHandle != vr::k_ulOverlayHandleInvalid)
        return true;

    vr::IVROverlay* overlay = m_Overlay ? m_Overlay : vr::VROverlay();
    if (!overlay)
        return false;

    const std::string key = "KillIndicatorOverlayKey_" + std::to_string(m_NextKillIndicatorOverlaySerial++);
    const std::string name = "KillIndicatorOverlay_" + std::to_string(slotIndex);

    std::lock_guard<std::mutex> lock(m_VROverlayMutex);
    const vr::EVROverlayError createError = overlay->CreateOverlay(key.c_str(), name.c_str(), &slot.overlayHandle);
    if (createError != vr::VROverlayError_None)
    {
        slot.overlayHandle = vr::k_ulOverlayHandleInvalid;
        return false;
    }

    static const vr::VRTextureBounds_t fullTextureBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    overlay->SetOverlayTexelAspect(slot.overlayHandle, 1.0f);
    overlay->SetOverlayFlag(slot.overlayHandle, vr::VROverlayFlags_IgnoreTextureAlpha, false);
    overlay->SetOverlayTextureBounds(slot.overlayHandle, &fullTextureBounds);
    slot.materialIndex = -1;
    slot.visible = false;
    return true;
}

int VR::AcquireKillIndicatorOverlaySlot(int materialIndex) const
{
    std::array<bool, 16> used{};
    for (const ActiveKillIndicator& active : m_ActiveKillIndicators)
    {
        if (active.overlaySlot >= 0 && active.overlaySlot < static_cast<int>(used.size()))
            used[active.overlaySlot] = true;
    }

    for (int slotIndex = 0; slotIndex < static_cast<int>(used.size()); ++slotIndex)
    {
        if (!used[slotIndex] &&
            GetPreboundKillIndicatorMaterialForSlot(slotIndex) == materialIndex)
            return slotIndex;
    }

    return -1;
}

void VR::TrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool clearAll)
{
    size_t writeIndex = 0;
    const size_t originalCount = m_ActiveKillIndicators.size();
    for (size_t readIndex = 0; readIndex < originalCount; ++readIndex)
    {
        ActiveKillIndicator& indicator = m_ActiveKillIndicators[readIndex];
        const bool expired = clearAll
            || std::chrono::duration<float>(now - indicator.startedAt).count() >= GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
        if (!expired)
        {
            if (writeIndex != readIndex)
                m_ActiveKillIndicators[writeIndex] = std::move(indicator);
            ++writeIndex;
            continue;
        }

        DestroyKillIndicatorOverlay(indicator);
        ++m_KillIndicatorStatsTrimmed;
    }

    if (writeIndex < originalCount)
        m_ActiveKillIndicators.resize(writeIndex);
}

void VR::MaybeTrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool force)
{
    if (!force && m_LastKillIndicatorTrimTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastKillIndicatorTrimTime).count();
        if (elapsed < kKillIndicatorTrimIntervalSeconds)
            return;
    }

    TrimExpiredKillIndicators(now, false);
    m_LastKillIndicatorTrimTime = now;
}

void VR::MaybeLogKillIndicatorStats(std::chrono::steady_clock::time_point now)
{
    if (!m_KillIndicatorDebugLog)
        return;

    if (ShouldThrottle(m_LastKillIndicatorDebugLogTime, m_KillIndicatorDebugLogHz))
        return;

    Game::logMsg(
        "[VR][KillIndicator][stats] active=%zu peak=%u hit_spawned=%u kill_spawned=%u hit_merged=%u recycled=%u trimmed=%u hit_sound_queued=%u hit_sound_merged=%u hit_sound_flushed=%u",
        m_ActiveKillIndicators.size(),
        m_KillIndicatorStatsPeakActive,
        m_KillIndicatorStatsHitSpawned,
        m_KillIndicatorStatsKillSpawned,
        m_KillIndicatorStatsHitMerged,
        m_KillIndicatorStatsRecycled,
        m_KillIndicatorStatsTrimmed,
        m_HitSoundStatsQueued,
        m_HitSoundStatsMerged,
        m_HitSoundStatsFlushed);

    m_KillIndicatorStatsHitSpawned = 0;
    m_KillIndicatorStatsKillSpawned = 0;
    m_KillIndicatorStatsHitMerged = 0;
    m_KillIndicatorStatsRecycled = 0;
    m_KillIndicatorStatsTrimmed = 0;
    m_KillIndicatorStatsPeakActive = static_cast<uint32_t>(m_ActiveKillIndicators.size());
    m_HitSoundStatsQueued = 0;
    m_HitSoundStatsMerged = 0;
    m_HitSoundStatsFlushed = 0;
}

int VR::FindReusableKillIndicatorIndex(bool preferNonKill) const
{
    if (m_ActiveKillIndicators.empty())
        return -1;

    int fallbackIndex = 0;
    auto fallbackStartedAt = m_ActiveKillIndicators[0].startedAt;
    int preferredIndex = -1;
    auto preferredStartedAt = std::chrono::steady_clock::time_point::max();

    for (int i = 0; i < static_cast<int>(m_ActiveKillIndicators.size()); ++i)
    {
        const ActiveKillIndicator& indicator = m_ActiveKillIndicators[i];
        if (indicator.startedAt < fallbackStartedAt)
        {
            fallbackStartedAt = indicator.startedAt;
            fallbackIndex = i;
        }

        if (preferNonKill && indicator.killConfirmed)
            continue;

        if (preferredIndex < 0 || indicator.startedAt < preferredStartedAt)
        {
            preferredStartedAt = indicator.startedAt;
            preferredIndex = i;
        }
    }

    return preferredIndex >= 0 ? preferredIndex : fallbackIndex;
}

void VR::AddOrRecycleKillIndicator(const Vector& worldPos, bool killConfirmed, bool headshot, std::chrono::steady_clock::time_point now, bool preferNonKill)
{
    if (m_ActiveKillIndicators.capacity() < kMaxActiveKillIndicators)
        m_ActiveKillIndicators.reserve(kMaxActiveKillIndicators);

    if (m_ActiveKillIndicators.size() < kMaxActiveKillIndicators)
    {
        ActiveKillIndicator indicator{};
        indicator.worldPos = worldPos;
        indicator.startedAt = now;
        indicator.killConfirmed = killConfirmed;
        indicator.headshot = headshot;
        m_ActiveKillIndicators.push_back(indicator);
        m_KillIndicatorStatsPeakActive = (std::max)(m_KillIndicatorStatsPeakActive, static_cast<uint32_t>(m_ActiveKillIndicators.size()));
        return;
    }

    const int reuseIndex = FindReusableKillIndicatorIndex(preferNonKill);
    if (reuseIndex < 0)
        return;

    ActiveKillIndicator& indicator = m_ActiveKillIndicators[reuseIndex];
    DestroyKillIndicatorOverlay(indicator);
    ++m_KillIndicatorStatsRecycled;
    indicator.worldPos = worldPos;
    indicator.startedAt = now;
    indicator.killConfirmed = killConfirmed;
    indicator.headshot = headshot;
    indicator.overlaySlot = -1;
}

bool VR::BuildKillIndicatorOverlayPixels(IMaterial* material, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight, uint32_t preferredFrameIndex, bool* outUsedDecodedFrames)
{
    outPixels.clear();
    outWidth = 0;
    outHeight = 0;
    if (outUsedDecodedFrames)
        *outUsedDecodedFrames = false;

    if (!material || material->IsErrorMaterial())
        return false;

    KillIndicatorDecodedFrames& decoded = GetKillIndicatorDecodedFrameCache(material->GetName());
    if (decoded.loaded && !decoded.frames.empty())
    {
        size_t frameIndex = 0;
        if (decoded.frames.size() > 1)
        {
            if (preferredFrameIndex != UINT32_MAX)
            {
                frameIndex = static_cast<size_t>(preferredFrameIndex) % decoded.frames.size();
            }
            else if (decoded.frameRate > 0.01f)
            {
                const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                frameIndex = static_cast<size_t>(std::floor(nowSeconds * decoded.frameRate)) % decoded.frames.size();
            }
        }

        outPixels = decoded.frames[frameIndex];
        outWidth = decoded.width;
        outHeight = decoded.height;
        if (outUsedDecodedFrames)
            *outUsedDecodedFrames = true;
        return !outPixels.empty();
    }

    int previewWidth = 0;
    int previewHeight = 0;
    ImageFormat previewFormat = IMAGE_FORMAT_RGBA8888;
    bool isTranslucent = false;
    material->Refresh();
    material->GetPreviewImageProperties(&previewWidth, &previewHeight, &previewFormat, &isTranslucent);

    if (previewWidth <= 0 || previewHeight <= 0)
    {
        previewWidth = material->GetMappingWidth();
        previewHeight = material->GetMappingHeight();
    }

    previewWidth = std::clamp(previewWidth, 1, 256);
    previewHeight = std::clamp(previewHeight, 1, 256);

    outPixels.resize(static_cast<size_t>(previewWidth) * static_cast<size_t>(previewHeight) * 4u, 0);
    material->GetPreviewImage(outPixels.data(), previewWidth, previewHeight, IMAGE_FORMAT_RGBA8888);

    outWidth = static_cast<uint32_t>(previewWidth);
    outHeight = static_cast<uint32_t>(previewHeight);
    return !outPixels.empty();
}

bool VR::ComputeKillIndicatorOverlayTransform(const Vector& worldPos, vr::HmdMatrix34_t& outTransform) const
{
    Vector srcRight = m_HmdRight;
    Vector srcUp = m_HmdUp;
    Vector srcForward = m_HmdForward;
    if (VectorNormalize(srcRight) == 0.0f || VectorNormalize(srcUp) == 0.0f || VectorNormalize(srcForward) == 0.0f)
        return false;

    const float unitsPerMeter = (std::max)(1.0f, m_VRScale);
    const Vector deltaSource = worldPos - m_HmdPosAbs;
    const float deltaRightMeters = DotProduct(deltaSource, srcRight) / unitsPerMeter;
    const float deltaUpMeters = DotProduct(deltaSource, srcUp) / unitsPerMeter;
    const float deltaForwardMeters = DotProduct(deltaSource, srcForward) / unitsPerMeter;

    outTransform = {
        1.0f, 0.0f, 0.0f, deltaRightMeters,
        0.0f, 1.0f, 0.0f, deltaUpMeters,
        0.0f, 0.0f, 1.0f, -deltaForwardMeters
    };
    return true;
}

void VR::UpdateKillIndicatorOverlays()
{
    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, true);
    MaybeLogKillIndicatorStats(now);

    if (!m_KillIndicatorEnabled)
        return;

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;

    {
        std::lock_guard<std::mutex> lock(m_VROverlayMutex);
        if (vr::IVROverlay* current = vr::VROverlay())
            m_Overlay = current;
    }

    vr::IVROverlay* overlay = m_Overlay ? m_Overlay : vr::VROverlay();
    if (!overlay || !m_Compositor)
        return;

    IMaterial* materials[3] = {
        ResolveHitIndicatorMaterial(),
        ResolveKillIndicatorMaterial(false),
        ResolveKillIndicatorMaterial(true)
    };
    bool textureReady[3] = {};
    const float baseSizePixels = (std::max)(16.0f, m_KillIndicatorSizePixels);

    for (int materialIndex = 0; materialIndex < 3; ++materialIndex)
    {
        IMaterial* material = materials[materialIndex];
        if (!material)
            continue;

        uint32_t desiredFrameIndex = 0;
        bool usesDecodedFrames = false;
        KillIndicatorDecodedFrames& decoded = GetKillIndicatorDecodedFrameCache(material->GetName());
        if (decoded.loaded && !decoded.frames.empty())
        {
            usesDecodedFrames = true;
            if (!m_ActiveKillIndicators.empty() &&
                decoded.frames.size() > 1 && decoded.frameRate > 0.01f)
            {
                const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
                desiredFrameIndex = static_cast<uint32_t>(std::floor(nowSeconds * decoded.frameRate)) % static_cast<uint32_t>(decoded.frames.size());
            }
        }

        KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
        const bool needsUpload = texture.sharedTexture.m_VRTexture.handle == nullptr
            || texture.uploadedFrameIndex != desiredFrameIndex
            || texture.uploadedFromDecodedFrames != usesDecodedFrames;
        if (needsUpload)
        {
            std::vector<uint8_t> pixels;
            uint32_t pixelWidth = 0;
            uint32_t pixelHeight = 0;
            bool builtFromDecodedFrames = false;
            if (!BuildKillIndicatorOverlayPixels(material, pixels, pixelWidth, pixelHeight, desiredFrameIndex, &builtFromDecodedFrames))
            {
                continue;
            }

            if (!UploadKillIndicatorOverlayTexture(materialIndex, pixels.data(), static_cast<int>(pixelWidth), static_cast<int>(pixelHeight), desiredFrameIndex, builtFromDecodedFrames))
            {
                continue;
            }
        }

        textureReady[materialIndex] = m_KillIndicatorOverlayTextures[materialIndex].sharedTexture.m_VRTexture.handle != nullptr;
    }

    // Pre-create and pre-bind fixed material pools before any hit event needs a
    // slot. Texture uploads above are flushed once for the whole pool; subsequent
    // hits only update transform/alpha/visibility and never drain DXVK's CS thread.
    std::array<int, 16> slotsToBind{};
    size_t slotsToBindCount = 0;
    for (int slotIndex = 0; slotIndex < static_cast<int>(m_KillIndicatorOverlaySlots.size()); ++slotIndex)
    {
        const int materialIndex = GetPreboundKillIndicatorMaterialForSlot(slotIndex);
        if (!textureReady[materialIndex])
            continue;
        if (!EnsureKillIndicatorOverlaySlot(slotIndex))
            continue;

        KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[slotIndex];
        if (slot.materialIndex != materialIndex)
            slotsToBind[slotsToBindCount++] = slotIndex;
    }

    if (slotsToBindCount != 0 && g_D3DVR9)
    {
        // Preserve the established overlay -> submission-queue lock order used by
        // the rest of the OpenVR paths.
        std::lock_guard<std::mutex> lock(m_VROverlayMutex);
        const HRESULT queueHr = g_D3DVR9->LockSubmissionQueue();
        if (SUCCEEDED(queueHr))
        {
            for (size_t i = 0; i < slotsToBindCount; ++i)
            {
                KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[slotsToBind[i]];
                const int materialIndex = GetPreboundKillIndicatorMaterialForSlot(slotsToBind[i]);
                const vr::EVROverlayError textureError = overlay->SetOverlayTexture(
                    slot.overlayHandle,
                    &m_KillIndicatorOverlayTextures[materialIndex].sharedTexture.m_VRTexture);
                if (textureError == vr::VROverlayError_None)
                    slot.materialIndex = materialIndex;
                else if (textureError == vr::VROverlayError_InvalidHandle)
                    slot.overlayHandle = vr::k_ulOverlayHandleInvalid;
            }
            g_D3DVR9->UnlockSubmissionQueue();
        }
    }

    if (m_ActiveKillIndicators.empty())
        return;

    for (ActiveKillIndicator& indicator : m_ActiveKillIndicators)
    {
        const KillIndicatorMaterialKind kind = !indicator.killConfirmed
            ? KillIndicatorMaterialKind::Hit
            : (indicator.headshot ? KillIndicatorMaterialKind::Headshot : KillIndicatorMaterialKind::Kill);
        const int materialIndex = static_cast<int>(kind);
        IMaterial* material = materials[materialIndex];
        if (!material || !textureReady[materialIndex])
            continue;

        if (indicator.overlaySlot < 0)
        {
            indicator.overlaySlot = AcquireKillIndicatorOverlaySlot(materialIndex);
            if (indicator.overlaySlot < 0)
                continue;
        }

        if (!EnsureKillIndicatorOverlaySlot(indicator.overlaySlot))
        {
            indicator.overlaySlot = -1;
            continue;
        }

        KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[indicator.overlaySlot];
        if (slot.overlayHandle == vr::k_ulOverlayHandleInvalid)
            continue;

        const float lifetime = GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
        const float ageSeconds = std::chrono::duration<float>(now - indicator.startedAt).count();
        const float progress = Clamp01(ageSeconds / lifetime);
        const float introWindow = indicator.killConfirmed ? 0.22f : 0.18f;
        const float intro = Clamp01(progress / introWindow);
        const float fadeStart = indicator.killConfirmed ? 0.72f : 0.58f;
        const float fadeWidth = indicator.killConfirmed ? 0.28f : 0.42f;
        const float fade = 1.0f - Clamp01((progress - fadeStart) / fadeWidth);
        const float pulse = std::sin(intro * 1.57079632679f);

        Vector drawPos = indicator.worldPos;
        if ((drawPos - m_HmdPosAbs).Length() > m_KillIndicatorMaxDistance)
        {
            if (slot.visible)
            {
                std::lock_guard<std::mutex> lock(m_VROverlayMutex);
                overlay->HideOverlay(slot.overlayHandle);
                slot.visible = false;
            }
            continue;
        }

        vr::HmdMatrix34_t transform{};
        if (!ComputeKillIndicatorOverlayTransform(drawPos, transform))
            continue;

        float scale = indicator.killConfirmed ? (0.78f + 0.34f * pulse) : (0.56f + 0.24f * pulse);
        if (indicator.killConfirmed && indicator.headshot)
            scale *= 1.10f;

        const float alphaBase = indicator.killConfirmed ? 0.72f : 0.60f;
        const float alpha = Clamp01((alphaBase + (1.0f - alphaBase) * intro) * fade);
        const float widthMeters = std::clamp((baseSizePixels / 640.0f) * scale, 0.10f, 0.45f);

        // A gameplay event must never bind a new Vulkan texture. If prewarming is
        // still pending, defer this marker for one frame instead of introducing a
        // hit-correlated queue drain (and a Source render interleave window).
        if (slot.materialIndex != materialIndex)
            continue;

        std::lock_guard<std::mutex> lock(m_VROverlayMutex);
        overlay->SetOverlayTransformTrackedDeviceRelative(slot.overlayHandle, vr::k_unTrackedDeviceIndex_Hmd, &transform);
        overlay->SetOverlayWidthInMeters(slot.overlayHandle, widthMeters);
        overlay->SetOverlayAlpha(slot.overlayHandle, alpha);
        if (!slot.visible)
        {
            const vr::EVROverlayError showError = overlay->ShowOverlay(slot.overlayHandle);
            if (showError != vr::VROverlayError_None)
            {
                slot.visible = false;
                if (showError == vr::VROverlayError_InvalidHandle)
                    slot.overlayHandle = vr::k_ulOverlayHandleInvalid;
                continue;
            }
            slot.visible = true;
        }
    }
}

void VR::SpawnHitIndicator(const Vector& worldPos)
{
    if (!m_HitIndicatorEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, false);
    ++m_KillIndicatorStatsHitSpawned;

    const float mergeDistanceSqr = kHitIndicatorMergeDistance * kHitIndicatorMergeDistance;
    for (ActiveKillIndicator& indicator : m_ActiveKillIndicators)
    {
        if (indicator.killConfirmed)
            continue;

        const float ageSeconds = std::chrono::duration<float>(now - indicator.startedAt).count();
        if (ageSeconds > kHitIndicatorMergeWindowSeconds)
            continue;

        if (indicator.worldPos.DistToSqr(worldPos) > mergeDistanceSqr)
            continue;

        indicator.worldPos = worldPos;
        indicator.startedAt = now;
        ++m_KillIndicatorStatsHitMerged;
        return;
    }

    AddOrRecycleKillIndicator(worldPos, false, false, now, true);
}

void VR::SpawnKillIndicator(bool headshot, const Vector& worldPos)
{
    if (!m_KillIndicatorEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, false);
    ++m_KillIndicatorStatsKillSpawned;

    Vector indicatorPos = worldPos;
    if (indicatorPos.IsZero())
    {
        Vector fallbackForward{};
        Vector fallbackRight{};
        Vector fallbackUp{};
        QAngle::AngleVectors(m_SetupAngles, &fallbackForward, &fallbackRight, &fallbackUp);
        if (fallbackForward.IsZero())
            fallbackForward = m_HmdForward;
        if (fallbackForward.IsZero())
            fallbackForward = { 1.0f, 0.0f, 0.0f };

        VectorNormalize(fallbackForward);
        indicatorPos = m_SetupOrigin + fallbackForward * 196.0f;
    }

    AddOrRecycleKillIndicator(indicatorPos, true, headshot, now, true);
}

void VR::DestroyItemLabelOverlayTexture()
{
    ClearQueuedProjectedItemLabels();

    std::lock_guard<std::recursive_mutex> cacheLock(m_ItemLabelTextureCacheMutex);
    for (auto& pair : m_ItemLabelTextureCache)
    {
        auto& cached = pair.second;
        if (cached.texture)
        {
            cached.texture->Release();
            cached.texture = nullptr;
        }
        cached.width = 0;
        cached.height = 0;
    }

    m_ItemLabelTextureCache.clear();
}

LRESULT CALLBACK VR::DesktopCompanionWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    VR* vr = reinterpret_cast<VR*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        const CREATESTRUCTA* cs = reinterpret_cast<const CREATESTRUCTA*>(lParam);
        vr = reinterpret_cast<VR*>(cs ? cs->lpCreateParams : nullptr);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(vr));
    }

    if (msg == WM_ERASEBKGND)
        return 1;

    if (msg == WM_PAINT && vr)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int clientW = (std::max)(1, static_cast<int>(rc.right - rc.left));
        const int clientH = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

        HDC paintDc = dc;
        HDC memDc = CreateCompatibleDC(dc);
        HBITMAP memBitmap = memDc ? CreateCompatibleBitmap(dc, clientW, clientH) : nullptr;
        HGDIOBJ oldBitmap = nullptr;
        if (memDc && memBitmap)
        {
            oldBitmap = SelectObject(memDc, memBitmap);
            paintDc = memDc;
        }

        RECT fillRc{ 0, 0, clientW, clientH };
        FillRect(paintDc, &fillRc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        std::vector<uint8_t> pixels;
        int w = 0;
        int h = 0;
        {
            std::lock_guard<std::mutex> lock(vr->m_DesktopCompanionHudMutex);
            if (hwnd == vr->m_DesktopRearMirrorWindow)
            {
                pixels = vr->m_DesktopCompanionRearMirrorBgra;
                w = vr->m_DesktopCompanionRearMirrorW;
                h = vr->m_DesktopCompanionRearMirrorH;
            }
            else if (hwnd == vr->m_DesktopIntentSenseHudWindow)
            {
                pixels = vr->m_DesktopCompanionIntentHudBgra;
                w = vr->m_DesktopCompanionIntentHudW;
                h = vr->m_DesktopCompanionIntentHudH;
            }
        }

        if (!pixels.empty() && w > 0 && h > 0)
        {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            const float sx = static_cast<float>(clientW) / static_cast<float>(w);
            const float sy = static_cast<float>(clientH) / static_cast<float>(h);
            const float scale = (std::min)(sx, sy);
            const int drawW = (std::max)(1, static_cast<int>(std::round(w * scale)));
            const int drawH = (std::max)(1, static_cast<int>(std::round(h * scale)));
            const int drawX = (clientW - drawW) / 2;
            const int drawY = (clientH - drawH) / 2;

            SetStretchBltMode(paintDc, HALFTONE);
            StretchDIBits(paintDc, drawX, drawY, drawW, drawH,
                0, 0, w, h, pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }

        if (paintDc != dc)
            BitBlt(dc, 0, 0, clientW, clientH, paintDc, 0, 0, SRCCOPY);
        if (oldBitmap)
            SelectObject(memDc, oldBitmap);
        if (memBitmap)
            DeleteObject(memBitmap);
        if (memDc)
            DeleteDC(memDc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    if (msg == WM_CLOSE)
    {
        ShowWindow(hwnd, SW_HIDE);
        if (vr)
        {
            if (hwnd == vr->m_DesktopRearMirrorWindow)
                vr->m_DesktopRearMirrorWindowShown = false;
            else if (hwnd == vr->m_DesktopIntentSenseHudWindow)
                vr->m_DesktopIntentSenseHudWindowShown = false;
        }
        return 0;
    }

    if (msg == WM_SIZE)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    if (msg == WM_GETMINMAXINFO)
    {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        if (info)
        {
            info->ptMinTrackSize.x = 160;
            info->ptMinTrackSize.y = 90;
        }
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void VR::PumpDesktopCompanionWindows()
{
    MSG msg{};
    while (m_DesktopRearMirrorWindow && PeekMessageA(&msg, m_DesktopRearMirrorWindow, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    while (m_DesktopIntentSenseHudWindow && PeekMessageA(&msg, m_DesktopIntentSenseHudWindow, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static HWND EnsureDesktopCompanionWindow(VR* vr, HWND& hwnd, const char* className, const char* title, int width, int height)
{
    if (hwnd)
        return hwnd;

    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VR::DesktopCompanionWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = className;
    RegisterClassExA(&wc);

    RECT rect{ 0, 0, (std::max)(64, width), (std::max)(64, height) };
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    hwnd = CreateWindowExA(
        exStyle,
        className,
        title,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        vr);
    return hwnd;
}

static void RefreshDesktopCompanionZOrder(HWND rearMirrorWindow, bool rearMirrorShown, HWND intentHudWindow, bool intentHudShown)
{
    if (rearMirrorWindow && rearMirrorShown)
    {
        SetWindowPos(rearMirrorWindow, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (intentHudWindow && intentHudShown)
    {
        SetWindowPos(intentHudWindow, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void VR::UpdateDesktopIntentSenseHudWindow(const uint8_t* rgba, int width, int height, bool visible)
{
    PumpDesktopCompanionWindows();

    if (!m_DesktopIntentSenseHudWindowEnabled)
    {
        if (m_DesktopIntentSenseHudWindow && m_DesktopIntentSenseHudWindowShown)
            ShowWindow(m_DesktopIntentSenseHudWindow, SW_HIDE);
        m_DesktopIntentSenseHudWindowShown = false;
        return;
    }

    int windowW = width;
    int windowH = height;
    if (windowW <= 0 || windowH <= 0)
    {
        std::lock_guard<std::mutex> lock(m_DesktopCompanionHudMutex);
        windowW = m_DesktopCompanionIntentHudW > 0 ? m_DesktopCompanionIntentHudW : (std::max)(128, m_SpecialInfectedIntentSenseHudTexW);
        windowH = m_DesktopCompanionIntentHudH > 0 ? m_DesktopCompanionIntentHudH : (std::max)(64, m_SpecialInfectedIntentSenseHudTexH);
    }

    HWND hwnd = EnsureDesktopCompanionWindow(this, m_DesktopIntentSenseHudWindow,
        "L4D2VRDesktopIntentSenseHudWindow", "L4D2VR Intent Sense HUD", windowW, windowH);
    if (!hwnd)
        return;

    if (!m_DesktopIntentSenseHudWindowShown)
    {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        m_DesktopIntentSenseHudWindowShown = true;
        RefreshDesktopCompanionZOrder(m_DesktopRearMirrorWindow, m_DesktopRearMirrorWindowShown,
            m_DesktopIntentSenseHudWindow, m_DesktopIntentSenseHudWindowShown);
    }

    if (!visible || !rgba || width <= 0 || height <= 0)
    {
        std::lock_guard<std::mutex> lock(m_DesktopCompanionHudMutex);
        const int clearW = windowW > 0 ? windowW : 128;
        const int clearH = windowH > 0 ? windowH : 64;
        m_DesktopCompanionIntentHudW = clearW;
        m_DesktopCompanionIntentHudH = clearH;
        m_DesktopCompanionIntentHudBgra.assign((size_t)clearW * (size_t)clearH * 4, 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_DesktopCompanionHudMutex);
        if (m_DesktopCompanionIntentHudW != width || m_DesktopCompanionIntentHudH != height)
        {
            m_DesktopCompanionIntentHudW = width;
            m_DesktopCompanionIntentHudH = height;
        }

        m_DesktopCompanionIntentHudBgra.resize((size_t)width * (size_t)height * 4);
        for (int y = 0; y < height; ++y)
        {
            const uint8_t* src = rgba + (size_t)y * (size_t)width * 4;
            uint8_t* dst = m_DesktopCompanionIntentHudBgra.data() + (size_t)y * (size_t)width * 4;
            for (int x = 0; x < width; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

void VR::UpdateDesktopRearMirrorWindow(bool visible)
{
    PumpDesktopCompanionWindows();

    if (!m_DesktopRearMirrorWindowEnabled || !visible || !m_D9RearMirrorSurface || !g_D3DVR9)
    {
        if (m_DesktopRearMirrorWindow && m_DesktopRearMirrorWindowShown)
            ShowWindow(m_DesktopRearMirrorWindow, SW_HIDE);
        m_DesktopRearMirrorWindowShown = false;
        return;
    }

    if (ShouldThrottle(m_LastDesktopCompanionRearMirrorCopyTime, (std::max)(1.0f, (std::min)(m_RearMirrorRTTMaxHz, 30.0f))))
        return;

    D3DSURFACE_DESC desc{};
    if (FAILED(m_D9RearMirrorSurface->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
        return;
    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
        return;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(m_D9RearMirrorSurface->GetDevice(&device)) || !device)
        return;

    g_D3DVR9->LockDevice();

    if (m_D9DesktopCompanionRearMirrorReadback)
    {
        D3DSURFACE_DESC oldDesc{};
        if (FAILED(m_D9DesktopCompanionRearMirrorReadback->GetDesc(&oldDesc)) ||
            oldDesc.Width != desc.Width || oldDesc.Height != desc.Height || oldDesc.Format != desc.Format)
        {
            m_D9DesktopCompanionRearMirrorReadback->Release();
            m_D9DesktopCompanionRearMirrorReadback = nullptr;
        }
    }

    if (!m_D9DesktopCompanionRearMirrorReadback)
    {
        const HRESULT createHr = device->CreateOffscreenPlainSurface(
            desc.Width,
            desc.Height,
            desc.Format,
            D3DPOOL_SYSTEMMEM,
            &m_D9DesktopCompanionRearMirrorReadback,
            nullptr);
        if (FAILED(createHr))
        {
            g_D3DVR9->UnlockDevice();
            device->Release();
            return;
        }
    }

    const HRESULT copyHr = device->GetRenderTargetData(m_D9RearMirrorSurface, m_D9DesktopCompanionRearMirrorReadback);
    D3DLOCKED_RECT lr{};
    const HRESULT lockHr = SUCCEEDED(copyHr)
        ? m_D9DesktopCompanionRearMirrorReadback->LockRect(&lr, nullptr, D3DLOCK_READONLY)
        : E_FAIL;

    if (SUCCEEDED(lockHr) && lr.pBits)
    {
        const int width = static_cast<int>(desc.Width);
        const int height = static_cast<int>(desc.Height);
        HWND hwnd = EnsureDesktopCompanionWindow(this, m_DesktopRearMirrorWindow,
            "L4D2VRDesktopRearMirrorWindow", "L4D2VR Rear Mirror", width, height);
        if (hwnd)
        {
            {
                std::lock_guard<std::mutex> lock(m_DesktopCompanionHudMutex);
                if (m_DesktopCompanionRearMirrorW != width || m_DesktopCompanionRearMirrorH != height)
                {
                    m_DesktopCompanionRearMirrorW = width;
                    m_DesktopCompanionRearMirrorH = height;
                }

                m_DesktopCompanionRearMirrorBgra.resize((size_t)width * (size_t)height * 4);
                const uint8_t* src0 = reinterpret_cast<const uint8_t*>(lr.pBits);
                for (int y = 0; y < height; ++y)
                {
                    const uint8_t* src = src0 + (size_t)y * (size_t)lr.Pitch;
                    uint8_t* dst = m_DesktopCompanionRearMirrorBgra.data() + (size_t)y * (size_t)width * 4;
                    std::memcpy(dst, src, (size_t)width * 4);
                }
            }

            if (!m_DesktopRearMirrorWindowShown)
            {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                m_DesktopRearMirrorWindowShown = true;
                RefreshDesktopCompanionZOrder(m_DesktopRearMirrorWindow, m_DesktopRearMirrorWindowShown,
                    m_DesktopIntentSenseHudWindow, m_DesktopIntentSenseHudWindowShown);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        m_D9DesktopCompanionRearMirrorReadback->UnlockRect();
    }

    g_D3DVR9->UnlockDevice();
    device->Release();
}

void VR::DestroyDesktopCompanionWindows()
{
    if (m_DesktopRearMirrorWindow)
    {
        DestroyWindow(m_DesktopRearMirrorWindow);
        m_DesktopRearMirrorWindow = nullptr;
    }
    if (m_DesktopIntentSenseHudWindow)
    {
        DestroyWindow(m_DesktopIntentSenseHudWindow);
        m_DesktopIntentSenseHudWindow = nullptr;
    }
    m_DesktopRearMirrorWindowShown = false;
    m_DesktopIntentSenseHudWindowShown = false;

    if (m_D9DesktopCompanionRearMirrorReadback)
    {
        m_D9DesktopCompanionRearMirrorReadback->Release();
        m_D9DesktopCompanionRearMirrorReadback = nullptr;
    }

    std::lock_guard<std::mutex> lock(m_DesktopCompanionHudMutex);
    m_DesktopCompanionRearMirrorBgra.clear();
    m_DesktopCompanionIntentHudBgra.clear();
    m_DesktopCompanionRearMirrorW = 0;
    m_DesktopCompanionRearMirrorH = 0;
    m_DesktopCompanionIntentHudW = 0;
    m_DesktopCompanionIntentHudH = 0;
}

void VR::PublishQueuedProjectedItemLabels(int eyeIndex, std::vector<QueuedProjectedItemLabelDraw> draws)
{
    if (eyeIndex < 0 || eyeIndex >= static_cast<int>(m_QueuedProjectedItemLabelEyes.size()))
        return;

    std::lock_guard<std::mutex> lock(m_QueuedProjectedItemLabelMutex);
    m_QueuedProjectedItemLabelEyes[eyeIndex] = std::move(draws);
}

void VR::ClearQueuedProjectedItemLabelEye(int eyeIndex)
{
    if (eyeIndex < 0 || eyeIndex >= static_cast<int>(m_QueuedProjectedItemLabelEyes.size()))
        return;

    std::lock_guard<std::mutex> lock(m_QueuedProjectedItemLabelMutex);
    m_QueuedProjectedItemLabelEyes[eyeIndex].clear();
}

void VR::ClearQueuedProjectedItemLabels()
{
    std::lock_guard<std::mutex> lock(m_QueuedProjectedItemLabelMutex);
    for (auto& eye : m_QueuedProjectedItemLabelEyes)
        eye.clear();
}

void VR::DrawQueuedProjectedItemLabelsToSurface(IDirect3DDevice9* device, int eyeIndex, IDirect3DSurface9* target, bool manageScene)
{
    if (!device || !target || eyeIndex < 0 || eyeIndex >= static_cast<int>(m_QueuedProjectedItemLabelEyes.size()))
        return;

    std::vector<QueuedProjectedItemLabelDraw> draws;
    {
        std::lock_guard<std::mutex> lock(m_QueuedProjectedItemLabelMutex);
        draws = m_QueuedProjectedItemLabelEyes[eyeIndex];
    }
    if (draws.empty())
        return;

    D3DSURFACE_DESC desc{};
    if (FAILED(target->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::recursive_mutex> cacheLock(m_ItemLabelTextureCacheMutex);
    for (auto it = m_ItemLabelTextureCache.begin(); it != m_ItemLabelTextureCache.end();)
    {
        const float ageSeconds = std::chrono::duration<float>(now - it->second.lastUsed).count();
        const bool stale = ageSeconds > 8.0f;
        const bool overBudget = (m_ItemLabelTextureCache.size() > 96) && ageSeconds > 2.0f;
        if (!it->second.texture || stale || overBudget)
        {
            if (it->second.texture)
                it->second.texture->Release();
            it = m_ItemLabelTextureCache.erase(it);
            continue;
        }
        ++it;
    }

    if (manageScene && FAILED(device->BeginScene()))
        return;

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
    {
        if (manageScene)
            device->EndScene();
        return;
    }
    stateBlock->Capture();

    IDirect3DSurface9* oldRenderTarget = nullptr;
    device->GetRenderTarget(0, &oldRenderTarget);

    D3DVIEWPORT9 oldViewport{};
    const bool hasOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = desc.Width;
    viewport.Height = desc.Height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    device->SetRenderTarget(0, target);
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(kProjectedItemLabelFvf);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const int screenWidth = static_cast<int>(desc.Width);
    const int screenHeight = static_cast<int>(desc.Height);
    for (const QueuedProjectedItemLabelDraw& draw : draws)
    {
        if (draw.text.empty() || draw.fontPx <= 0)
            continue;

        int texW = 0;
        int texH = 0;
        IDirect3DTexture9* texture = GetOrCreateProjectedItemLabelTexture(
            device,
            draw.text,
            draw.fontPx,
            draw.colorR,
            draw.colorG,
            draw.colorB,
            draw.colorA,
            texW,
            texH);
        if (!texture || texW <= 0 || texH <= 0)
            continue;

        const int drawX = std::clamp(draw.screenX - texW / 2, 2, (std::max)(2, screenWidth - texW - 2));
        const int drawY = std::clamp(draw.screenY - texH / 2, 2, (std::max)(2, screenHeight - texH - 2));
        const float left = static_cast<float>(drawX) - 0.5f;
        const float top = static_cast<float>(drawY) - 0.5f;
        const float right = left + static_cast<float>(texW);
        const float bottom = top + static_cast<float>(texH);

        const ProjectedItemLabelVertex quad[4] =
        {
            { left, top, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
            { right, top, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
            { left, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f }
        };

        device->SetTexture(0, texture);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ProjectedItemLabelVertex));
    }

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }

    if (oldRenderTarget)
    {
        device->SetRenderTarget(0, oldRenderTarget);
        oldRenderTarget->Release();
    }

    if (hasOldViewport)
        device->SetViewport(&oldViewport);

    if (manageScene)
        device->EndScene();
}

IDirect3DTexture9* VR::GetOrCreateProjectedItemLabelTexture(
    IDirect3DDevice9* device,
    const std::string& text,
    int fontPx,
    int colorR,
    int colorG,
    int colorB,
    int colorA,
    int& outWidth,
    int& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    if (!device || text.empty())
        return nullptr;

    std::lock_guard<std::recursive_mutex> cacheLock(m_ItemLabelTextureCacheMutex);
    const bool hasNonAscii = ContainsNonAscii(text.c_str());
    fontPx = QuantizeProjectedItemLabelFontPx(fontPx);
    const Rgba color
    {
        static_cast<uint8_t>(std::clamp(colorR, 0, 255)),
        static_cast<uint8_t>(std::clamp(colorG, 0, 255)),
        static_cast<uint8_t>(std::clamp(colorB, 0, 255)),
        static_cast<uint8_t>(std::clamp(colorA, 0, 255))
    };
    const std::string cacheKey = BuildProjectedItemLabelCacheKey(text, fontPx, color);
    const auto now = std::chrono::steady_clock::now();

    auto found = m_ItemLabelTextureCache.find(cacheKey);
    if (found != m_ItemLabelTextureCache.end() && found->second.texture)
    {
        found->second.lastUsed = now;
        outWidth = found->second.width;
        outHeight = found->second.height;
        return found->second.texture;
    }

    const int width = EstimateProjectedItemLabelWidth(text, fontPx);
    const int height = EstimateProjectedItemLabelHeight(fontPx, hasNonAscii);
    if (width <= 0 || height <= 0)
        return nullptr;

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    HudSurface surface{ pixels.data(), width, height, width * 4 };
    DrawTextUtf8OutlinedGdiClippedEx(
        surface,
        0,
        0,
        width,
        text.c_str(),
        fontPx,
        color,
        false);

    IDirect3DTexture9* texture = nullptr;
    const HRESULT createHr = device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &texture,
        nullptr);
    if (FAILED(createHr) || !texture)
    {
        if (m_ItemModelLabelDebugLog)
            Game::logMsg(
                "[VR][ItemLabel][d3d] CreateTexture failed hr=0x%08X label=%s size=%dx%d",
                static_cast<unsigned int>(createHr),
                text.c_str(),
                width,
                height);
        return nullptr;
    }

    D3DLOCKED_RECT lockedRect{};
    const HRESULT lockHr = texture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
    if (FAILED(lockHr) || !lockedRect.pBits)
    {
        if (m_ItemModelLabelDebugLog)
            Game::logMsg(
                "[VR][ItemLabel][d3d] LockRect failed hr=0x%08X label=%s size=%dx%d",
                static_cast<unsigned int>(lockHr),
                text.c_str(),
                width,
                height);
        texture->Release();
        return nullptr;
    }

    const uint8_t* src = pixels.data();
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(
            static_cast<unsigned char*>(lockedRect.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lockedRect.Pitch),
            src + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
            static_cast<size_t>(width) * 4u);
    }
    texture->UnlockRect(0);

    auto& cached = m_ItemLabelTextureCache[cacheKey];
    if (cached.texture)
        cached.texture->Release();
    cached.texture = texture;
    cached.width = width;
    cached.height = height;
    cached.lastUsed = now;
    outWidth = width;
    outHeight = height;
    return cached.texture;
}

void VR::DrawProjectedItemLabels(IMatRenderContext* renderContext, const CViewSetup& view, int eyeIndex)
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    const bool queued = queueMode != 0;
    const auto clearQueuedEye = [&]()
        {
            if (queued)
                ClearQueuedProjectedItemLabelEye(eyeIndex);
        };

    if (!renderContext)
    {
        clearQueuedEye();
        return;
    }

    if (m_DesktopMirrorCleanRenderingPass && m_DesktopMirrorHidePluginOverlays)
    {
        clearQueuedEye();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<ProjectedViewmodelBoneLabel> viewmodelBoneLabels;
    const bool calibrationOverlayActive =
        m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
    const bool viewmodelBoneLabelsRequested =
        m_ViewmodelBoneLabelsEnabled && !calibrationOverlayActive;
    if (viewmodelBoneLabelsRequested)
    {
        std::lock_guard<std::mutex> lock(m_ViewmodelBoneLabelMutex);
        viewmodelBoneLabels = m_ViewmodelBoneLabels;
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_ViewmodelBoneLabelMutex);
        m_ViewmodelBoneLabels.clear();
    }

    const bool itemLabelsActive = m_ItemModelLabelEnabled;
    const bool viewmodelBoneLabelsActive = viewmodelBoneLabelsRequested && !viewmodelBoneLabels.empty();
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame() ||
        (!itemLabelsActive && !viewmodelBoneLabelsActive))
    {
        if (!itemLabelsActive)
            m_ProjectedItemLabels.clear();
        ClearQueuedProjectedItemLabels();
        return;
    }

    if (!itemLabelsActive)
        m_ProjectedItemLabels.clear();

    if (!queued)
        ClearQueuedProjectedItemLabels();

    if (itemLabelsActive)
    {
        const float holdSeconds = GetProjectedItemLabelHoldSeconds(this);
        for (auto it = m_ProjectedItemLabels.begin(); it != m_ProjectedItemLabels.end();)
        {
            const float ageSeconds = std::chrono::duration<float>(now - it->second.lastSeen).count();
            if (ageSeconds < 0.0f || ageSeconds > holdSeconds)
                it = m_ProjectedItemLabels.erase(it);
            else
                ++it;
        }
    }

    if (m_ProjectedItemLabels.empty() && !viewmodelBoneLabelsActive)
    {
        clearQueuedEye();
        return;
    }

    int screenWidth = view.width;
    int screenHeight = view.height;
    if (screenWidth <= 0 || screenHeight <= 0)
    {
        clearQueuedEye();
        return;
    }

    QAngle viewAngles(view.angles.x, view.angles.y, view.angles.z);
    Vector forward{};
    Vector right{};
    Vector up{};
    QAngle::AngleVectors(viewAngles, &forward, &right, &up);
    if (forward.IsZero() || right.IsZero() || up.IsZero())
    {
        clearQueuedEye();
        return;
    }

    VectorNormalize(forward);
    VectorNormalize(right);
    VectorNormalize(up);

    const float aspect = (view.m_flAspectRatio > 0.01f)
        ? view.m_flAspectRatio
        : (static_cast<float>(screenWidth) / static_cast<float>(std::max(screenHeight, 1)));
    const float tanHalfFovX = std::tan(DEG2RAD(view.fov * 0.5f));
    const float tanHalfFovY = tanHalfFovX / std::max(aspect, 0.01f);
    if (tanHalfFovX <= 0.0001f || tanHalfFovY <= 0.0001f)
    {
        clearQueuedEye();
        return;
    }

    struct VisibleProjectedItemLabel
    {
        int screenX = 0;
        int screenY = 0;
        float depth = 0.0f;
        const ProjectedItemLabel* label = nullptr;
        std::string directText;
        Rgba directColor{ 255, 255, 255, 255 };
        float textScale = 1.0f;
        bool direct = false;
    };

    std::vector<VisibleProjectedItemLabel> visibleLabels;
    visibleLabels.reserve(m_ProjectedItemLabels.size() + viewmodelBoneLabels.size());

    if (itemLabelsActive)
    {
        for (const auto& pair : m_ProjectedItemLabels)
        {
            const ProjectedItemLabel& projected = pair.second;
            if (ShouldSuppressProjectedItemLabelForMotion(projected, now))
                continue;

            if (ShouldSuppressProjectedItemLabelNearPlayer(this, projected.worldPos))
                continue;

            const float maxDistance = std::max(0.0f, m_ItemModelLabelMaxDistance);
            if (maxDistance > 0.0f)
            {
                const Vector deltaFromView = projected.worldPos - view.origin;
                if (deltaFromView.LengthSqr() > (maxDistance * maxDistance))
                    continue;
            }

            float screenX = 0.0f;
            float screenY = 0.0f;
            float depth = 0.0f;
            if (!VR_ProjectAimLinePointToView(view, forward, right, up, projected.worldPos, tanHalfFovX, tanHalfFovY, screenX, screenY, depth))
                continue;
            if (depth <= std::max(view.zNear, 1.0f))
                continue;

            VisibleProjectedItemLabel candidate{
                static_cast<int>(std::lround(screenX)),
                static_cast<int>(std::lround(screenY)),
                depth,
                &projected
            };

            bool suppressedDuplicate = false;
            if (projected.category == ItemModelLabelCategory::Firearm ||
                projected.category == ItemModelLabelCategory::Melee)
            {
                for (VisibleProjectedItemLabel& existing : visibleLabels)
                {
                    if (existing.direct || !existing.label || existing.label->category != projected.category)
                        continue;

                    const Vector delta = existing.label->worldPos - projected.worldPos;
                    const float horizontalDistSq = delta.x * delta.x + delta.y * delta.y;
                    if (horizontalDistSq > (18.0f * 18.0f) || std::fabs(delta.z) > 28.0f)
                        continue;

                    if (projected.worldPos.z > existing.label->worldPos.z + 0.25f ||
                        (std::fabs(projected.worldPos.z - existing.label->worldPos.z) <= 0.25f && candidate.depth < existing.depth))
                    {
                        existing = candidate;
                    }

                    suppressedDuplicate = true;
                    break;
                }
            }

            if (!suppressedDuplicate)
                visibleLabels.push_back(candidate);
        }
    }

    if (viewmodelBoneLabelsActive)
    {
        for (const ProjectedViewmodelBoneLabel& projected : viewmodelBoneLabels)
        {
            if (projected.label.empty())
                continue;

            float screenX = 0.0f;
            float screenY = 0.0f;
            float depth = 0.0f;
            if (!VR_ProjectAimLinePointToView(view, forward, right, up, projected.worldPos, tanHalfFovX, tanHalfFovY, screenX, screenY, depth))
                continue;
            if (depth <= 0.25f)
                continue;

            VisibleProjectedItemLabel candidate{};
            candidate.screenX = static_cast<int>(std::lround(screenX));
            candidate.screenY = static_cast<int>(std::lround(screenY));
            candidate.depth = depth;
            candidate.directText = projected.label;
            candidate.directColor = projected.highlighted
                ? Rgba{ 120, 220, 255, 255 }
                : projected.hasName
                ? Rgba{ 255, 216, 64, 255 }
                : Rgba{ 80, 220, 255, 255 };
            candidate.textScale = projected.highlighted ? 1.05f : 0.82f;
            candidate.direct = true;
            visibleLabels.push_back(std::move(candidate));
        }
    }

    if (visibleLabels.empty())
    {
        clearQueuedEye();
        return;
    }

    std::sort(
        visibleLabels.begin(),
        visibleLabels.end(),
        [](const VisibleProjectedItemLabel& lhs, const VisibleProjectedItemLabel& rhs)
        {
            return lhs.depth < rhs.depth;
        });

    const size_t maxProjectedItemLabelsPerEye = viewmodelBoneLabelsActive
        ? static_cast<size_t>(96)
        : static_cast<size_t>(std::clamp(m_ItemModelLabelMaxVisiblePerEye, 1, 64));
    if (visibleLabels.size() > maxProjectedItemLabelsPerEye)
        visibleLabels.resize(maxProjectedItemLabelsPerEye);

    std::sort(
        visibleLabels.begin(),
        visibleLabels.end(),
        [](const VisibleProjectedItemLabel& lhs, const VisibleProjectedItemLabel& rhs)
        {
            return lhs.depth > rhs.depth;
        });

    if (queued)
    {
        std::vector<QueuedProjectedItemLabelDraw> queuedDraws;
        queuedDraws.reserve(visibleLabels.size());
        for (const VisibleProjectedItemLabel& visible : visibleLabels)
        {
            if (!visible.direct && !visible.label)
                continue;

            const std::string& text = visible.direct ? visible.directText : visible.label->label;
            if (text.empty())
                continue;

            Rgba color = visible.direct ? visible.directColor : Rgba{};
            if (!visible.direct && !GetProjectedItemLabelColor(visible.label->category, color))
                continue;

            const float textScale = visible.direct
                ? std::clamp(visible.textScale, 0.25f, 4.0f)
                : std::clamp(m_ItemModelLabelTextScale, 0.25f, 4.0f);
            const float worldTextHeight = (visible.direct ? 6.0f : 9.0f) * textScale;
            int fontPx = static_cast<int>(std::lround((worldTextHeight / (visible.depth * tanHalfFovY * 2.0f)) * static_cast<float>(screenHeight)));
            const bool hasNonAscii = ContainsNonAscii(text.c_str());
            const int minFontPx = (std::max)(6, static_cast<int>(std::lround((hasNonAscii ? 12.0f : 10.0f) * textScale)));
            const int maxFontPx = (std::max)(minFontPx, static_cast<int>(std::lround((hasNonAscii ? 28.0f : 24.0f) * textScale)));
            fontPx = std::clamp(fontPx, minFontPx, maxFontPx);
            fontPx = QuantizeProjectedItemLabelFontPx(fontPx);

            QueuedProjectedItemLabelDraw draw{};
            draw.text = text;
            draw.screenX = visible.screenX;
            draw.screenY = visible.screenY;
            draw.fontPx = fontPx;
            draw.colorR = static_cast<int>(color.r);
            draw.colorG = static_cast<int>(color.g);
            draw.colorB = static_cast<int>(color.b);
            draw.colorA = static_cast<int>(color.a);
            queuedDraws.push_back(std::move(draw));
        }

        PublishQueuedProjectedItemLabels(eyeIndex, std::move(queuedDraws));
        return;
    }

    IDirect3DDevice9* device = GetKillIndicatorD3DDevice(this);
    if (!device)
    {
        if (m_ItemModelLabelDebugLog)
            Game::logMsg("[VR][ItemLabel][d3d] no D3D device during draw");
        return;
    }

    std::lock_guard<std::recursive_mutex> cacheLock(m_ItemLabelTextureCacheMutex);
    for (auto it = m_ItemLabelTextureCache.begin(); it != m_ItemLabelTextureCache.end();)
    {
        const float ageSeconds = std::chrono::duration<float>(now - it->second.lastUsed).count();
        const bool stale = ageSeconds > 8.0f;
        const bool overBudget = (m_ItemLabelTextureCache.size() > 96) && ageSeconds > 2.0f;
        if (!it->second.texture || stale || overBudget)
        {
            if (it->second.texture)
                it->second.texture->Release();
            it = m_ItemLabelTextureCache.erase(it);
            continue;
        }
        ++it;
    }

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();

    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(kProjectedItemLabelFvf);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    for (const VisibleProjectedItemLabel& visible : visibleLabels)
    {
        if (!visible.direct && !visible.label)
            continue;

        const std::string& text = visible.direct ? visible.directText : visible.label->label;
        if (text.empty())
            continue;

        Rgba color = visible.direct ? visible.directColor : Rgba{};
        if (!visible.direct && !GetProjectedItemLabelColor(visible.label->category, color))
            continue;

        const float textScale = visible.direct
            ? std::clamp(visible.textScale, 0.25f, 4.0f)
            : std::clamp(m_ItemModelLabelTextScale, 0.25f, 4.0f);
        const float worldTextHeight = (visible.direct ? 6.0f : 9.0f) * textScale;
        int fontPx = static_cast<int>(std::lround((worldTextHeight / (visible.depth * tanHalfFovY * 2.0f)) * static_cast<float>(screenHeight)));
        const bool hasNonAscii = ContainsNonAscii(text.c_str());
        const int minFontPx = (std::max)(6, static_cast<int>(std::lround((hasNonAscii ? 12.0f : 10.0f) * textScale)));
        const int maxFontPx = (std::max)(minFontPx, static_cast<int>(std::lround((hasNonAscii ? 28.0f : 24.0f) * textScale)));
        fontPx = std::clamp(fontPx, minFontPx, maxFontPx);
        fontPx = QuantizeProjectedItemLabelFontPx(fontPx);

        int texW = 0;
        int texH = 0;
        IDirect3DTexture9* texture = GetOrCreateProjectedItemLabelTexture(
            device,
            text,
            fontPx,
            static_cast<int>(color.r),
            static_cast<int>(color.g),
            static_cast<int>(color.b),
            static_cast<int>(color.a),
            texW,
            texH);
        if (!texture || texW <= 0 || texH <= 0)
            continue;

        const int drawX = std::clamp(visible.screenX - texW / 2, 2, (std::max)(2, screenWidth - texW - 2));
        const int drawY = std::clamp(visible.screenY - texH / 2, 2, (std::max)(2, screenHeight - texH - 2));
        const float left = static_cast<float>(drawX) - 0.5f;
        const float top = static_cast<float>(drawY) - 0.5f;
        const float right = left + static_cast<float>(texW);
        const float bottom = top + static_cast<float>(texH);

        const ProjectedItemLabelVertex quad[4] =
        {
            { left, top, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
            { right, top, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
            { left, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f }
        };

        device->SetTexture(0, texture);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ProjectedItemLabelVertex));
    }

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }

    device->Release();
}

namespace
{
    struct PostMirrorOverlayVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
    };

    static constexpr DWORD kPostMirrorOverlayFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

    static void DrawPostMirrorLine2D(
        IDirect3DDevice9* device,
        float x0,
        float y0,
        float x1,
        float y1,
        D3DCOLOR color,
        float thicknessPixels)
    {
        if (!device)
            return;

        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1))
            return;

        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.01f)
            return;

        const int passes = std::clamp(static_cast<int>(std::ceil(std::max(1.0f, thicknessPixels))), 1, 9);
        const float nx = -dy / len;
        const float ny = dx / len;
        const float center = (static_cast<float>(passes) - 1.0f) * 0.5f;

        for (int i = 0; i < passes; ++i)
        {
            const float offset = static_cast<float>(i) - center;
            const PostMirrorOverlayVertex verts[2] =
            {
                { x0 + nx * offset - 0.5f, y0 + ny * offset - 0.5f, 0.0f, 1.0f, color },
                { x1 + nx * offset - 0.5f, y1 + ny * offset - 0.5f, 0.0f, 1.0f, color }
            };
            device->DrawPrimitiveUP(D3DPT_LINELIST, 1, verts, sizeof(PostMirrorOverlayVertex));
        }
    }

    static bool ProjectPostMirrorSegment(
        const CViewSetup& view,
        const Vector& forward,
        const Vector& right,
        const Vector& up,
        float tanHalfFovX,
        float tanHalfFovY,
        Vector start,
        Vector end,
        float& x0,
        float& y0,
        float& x1,
        float& y1)
    {
        const float nearDepth = std::max(view.zNear, 1.0f);
        const float startDepth = DotProduct(start - view.origin, forward);
        const float endDepth = DotProduct(end - view.origin, forward);
        if (startDepth <= nearDepth && endDepth <= nearDepth)
            return false;

        if (startDepth <= nearDepth || endDepth <= nearDepth)
        {
            const float denom = endDepth - startDepth;
            if (std::fabs(denom) <= 0.0001f)
                return false;

            const float t = std::clamp((nearDepth - startDepth) / denom, 0.0f, 1.0f);
            const Vector clipped = start + (end - start) * t;
            if (startDepth <= nearDepth)
                start = clipped;
            else
                end = clipped;
        }

        float d0 = 0.0f;
        float d1 = 0.0f;
        if (!VR_ProjectAimLinePointToView(view, forward, right, up, start, tanHalfFovX, tanHalfFovY, x0, y0, d0))
            return false;
        if (!VR_ProjectAimLinePointToView(view, forward, right, up, end, tanHalfFovX, tanHalfFovY, x1, y1, d1))
            return false;

        const float guardX = static_cast<float>(std::max(view.width, 1)) * 0.35f;
        const float guardY = static_cast<float>(std::max(view.height, 1)) * 0.35f;
        if ((x0 < -guardX && x1 < -guardX) || (x0 > view.width + guardX && x1 > view.width + guardX) ||
            (y0 < -guardY && y1 < -guardY) || (y0 > view.height + guardY && y1 > view.height + guardY))
        {
            return false;
        }
        return true;
    }

    static void BeginPostMirrorOverlayD3DState(IDirect3DDevice9* device, IDirect3DStateBlock9*& stateBlock)
    {
        stateBlock = nullptr;
        if (!device)
            return;

        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
            stateBlock->Capture();

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetTexture(0, nullptr);
        device->SetFVF(kPostMirrorOverlayFvf);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    }

    static void EndPostMirrorOverlayD3DState(IDirect3DStateBlock9* stateBlock)
    {
        if (stateBlock)
        {
            stateBlock->Apply();
            stateBlock->Release();
        }
    }
}

void VR::RecordProjectedSpecialInfectedArrow(int entityIndex, const Vector& origin, SpecialInfectedType type)
{
    if (!m_DesktopMirrorHidePluginOverlays || !m_DesktopMirrorEnabled || type == SpecialInfectedType::None)
        return;
    if (!m_SpecialInfectedArrowEnabled || m_SpecialInfectedArrowSize <= 0.0f)
        return;

    int key = entityIndex;
    if (key <= 0)
    {
        const int ix = static_cast<int>(std::lround(origin.x * 4.0f));
        const int iy = static_cast<int>(std::lround(origin.y * 4.0f));
        const int iz = static_cast<int>(std::lround(origin.z * 4.0f));
        const uint32_t hash = 0x40000000u
            ^ (static_cast<uint32_t>(ix) * 73856093u)
            ^ (static_cast<uint32_t>(iy) * 19349663u)
            ^ (static_cast<uint32_t>(iz) * 83492791u)
            ^ (static_cast<uint32_t>(type) * 2654435761u);
        key = static_cast<int>(hash & 0x7FFFFFFFu);
        if (key <= 0)
            key = 0x40000000;
    }

    std::lock_guard<std::mutex> lock(m_ProjectedSpecialInfectedArrowMutex);
    auto& arrow = m_ProjectedSpecialInfectedArrows[key];
    arrow.origin = origin;
    arrow.type = type;
    arrow.sourceEntityIndex = entityIndex;
    arrow.lastSeen = std::chrono::steady_clock::now();
}

void VR::DrawCachedSpecialInfectedArrowsDebugOverlay()
{
    if (!m_DesktopMirrorHidePluginOverlays || !m_DesktopMirrorEnabled)
        return;
    if (!m_SpecialInfectedArrowEnabled || m_SpecialInfectedArrowSize <= 0.0f || !m_Game || !m_Game->m_DebugOverlay)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::vector<ProjectedSpecialInfectedArrow> arrows;
    {
        std::lock_guard<std::mutex> lock(m_ProjectedSpecialInfectedArrowMutex);
        const float holdSeconds = std::clamp(2.0f / std::max(1.0f, m_SpecialInfectedOverlayMaxHz), 0.05f, 0.20f);
        for (auto it = m_ProjectedSpecialInfectedArrows.begin(); it != m_ProjectedSpecialInfectedArrows.end();)
        {
            const float ageSeconds = std::chrono::duration<float>(now - it->second.lastSeen).count();
            if (ageSeconds < 0.0f || ageSeconds > holdSeconds)
                it = m_ProjectedSpecialInfectedArrows.erase(it);
            else
            {
                arrows.push_back(it->second);
                ++it;
            }
        }
    }
    if (arrows.empty())
        return;

    const float frameDuration = (std::isfinite(m_LastFrameDuration) && m_LastFrameDuration > 0.0f)
        ? std::clamp(m_LastFrameDuration, 1.0f / 240.0f, 1.0f / 10.0f)
        : (1.0f / 60.0f);
    const float duration = std::clamp(frameDuration * 0.20f, 0.0015f, 0.0060f);
    const float baseHeight = m_SpecialInfectedArrowHeight;
    const float wingLength = m_SpecialInfectedArrowSize;
    const float offset = m_SpecialInfectedArrowThickness * 0.5f;
    const std::array<Vector, 5> offsets =
    {
        Vector(0.0f, 0.0f, 0.0f),
        Vector(offset, 0.0f, 0.0f),
        Vector(-offset, 0.0f, 0.0f),
        Vector(0.0f, offset, 0.0f),
        Vector(0.0f, -offset, 0.0f)
    };

    for (const ProjectedSpecialInfectedArrow& arrow : arrows)
    {
        if (arrow.type == SpecialInfectedType::None)
            continue;

        const size_t typeIndex = static_cast<size_t>(arrow.type);
        const RgbColor& arrowColor = typeIndex < m_SpecialInfectedArrowColors.size()
            ? m_SpecialInfectedArrowColors[typeIndex]
            : m_SpecialInfectedArrowDefaultColor;

        const Vector base = arrow.origin + Vector(0.0f, 0.0f, baseHeight);
        const Vector tip = base + Vector(0.0f, 0.0f, -m_SpecialInfectedArrowSize);
        auto drawArrowLine = [&](const Vector& start, const Vector& end)
            {
                if (offset <= 0.0f)
                {
                    m_Game->m_DebugOverlay->AddLineOverlay(start, end, arrowColor.r, arrowColor.g, arrowColor.b, true, duration);
                    return;
                }
                for (const Vector& lineOffset : offsets)
                    m_Game->m_DebugOverlay->AddLineOverlay(start + lineOffset, end + lineOffset, arrowColor.r, arrowColor.g, arrowColor.b, true, duration);
            };

        drawArrowLine(base, tip);
        drawArrowLine(base + Vector(wingLength, 0.0f, 0.0f), tip);
        drawArrowLine(base + Vector(-wingLength, 0.0f, 0.0f), tip);
        drawArrowLine(base + Vector(0.0f, wingLength, 0.0f), tip);
        drawArrowLine(base + Vector(0.0f, -wingLength, 0.0f), tip);
    }
}

void VR::DrawProjectedSpecialInfectedArrows(IMatRenderContext* renderContext, const CViewSetup& view)
{
    if (!renderContext)
        return;

    // Avoid direct IDirect3DDevice9 drawing from the queued render path.
    if (m_Game && m_Game->GetMatQueueMode() != 0)
        return;
    if (!m_DesktopMirrorHidePluginOverlays || !m_DesktopMirrorEnabled)
        return;
    if (!m_SpecialInfectedArrowEnabled || m_SpecialInfectedArrowSize <= 0.0f)
    {
        std::lock_guard<std::mutex> lock(m_ProjectedSpecialInfectedArrowMutex);
        m_ProjectedSpecialInfectedArrows.clear();
        return;
    }
    if (view.width <= 0 || view.height <= 0 || view.fov <= 1.0f)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::vector<ProjectedSpecialInfectedArrow> arrows;
    {
        std::lock_guard<std::mutex> lock(m_ProjectedSpecialInfectedArrowMutex);
        const float holdSeconds = std::clamp(6.0f / std::max(1.0f, m_SpecialInfectedOverlayMaxHz), 0.12f, 0.45f);
        for (auto it = m_ProjectedSpecialInfectedArrows.begin(); it != m_ProjectedSpecialInfectedArrows.end();)
        {
            const float ageSeconds = std::chrono::duration<float>(now - it->second.lastSeen).count();
            if (ageSeconds < 0.0f || ageSeconds > holdSeconds)
                it = m_ProjectedSpecialInfectedArrows.erase(it);
            else
            {
                arrows.push_back(it->second);
                ++it;
            }
        }
    }
    if (arrows.empty())
        return;

    QAngle viewAngles(view.angles.x, view.angles.y, view.angles.z);
    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(viewAngles, &forward, &right, &up);
    if (forward.IsZero() || right.IsZero() || up.IsZero())
        return;
    VectorNormalize(forward);
    VectorNormalize(right);
    VectorNormalize(up);

    const float aspect = (view.m_flAspectRatio > 0.01f)
        ? view.m_flAspectRatio
        : (static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)));
    const float tanHalfFovX = std::tan(DEG2RAD(view.fov * 0.5f));
    const float tanHalfFovY = tanHalfFovX / std::max(aspect, 0.01f);
    if (tanHalfFovX <= 0.0001f || tanHalfFovY <= 0.0001f)
        return;

    IDirect3DDevice9* device = GetKillIndicatorD3DDevice(this);
    if (!device)
        return;

    IDirect3DStateBlock9* stateBlock = nullptr;
    BeginPostMirrorOverlayD3DState(device, stateBlock);

    const float height = m_SpecialInfectedArrowHeight;
    const float wing = m_SpecialInfectedArrowSize;
    const float thickness = std::max(1.0f, m_SpecialInfectedArrowThickness <= 0.0f ? 1.0f : m_SpecialInfectedArrowThickness);

    auto drawWorldLine = [&](const Vector& a, const Vector& b, D3DCOLOR color)
        {
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            if (!ProjectPostMirrorSegment(view, forward, right, up, tanHalfFovX, tanHalfFovY, a, b, x0, y0, x1, y1))
                return;
            DrawPostMirrorLine2D(device, x0, y0, x1, y1, color, thickness);
        };

    for (const ProjectedSpecialInfectedArrow& arrow : arrows)
    {
        if (arrow.type == SpecialInfectedType::None)
            continue;

        const size_t typeIndex = static_cast<size_t>(arrow.type);
        const RgbColor& color = typeIndex < m_SpecialInfectedArrowColors.size()
            ? m_SpecialInfectedArrowColors[typeIndex]
            : m_SpecialInfectedArrowDefaultColor;
        const D3DCOLOR d3dColor = D3DCOLOR_ARGB(255,
            std::clamp(color.r, 0, 255),
            std::clamp(color.g, 0, 255),
            std::clamp(color.b, 0, 255));

        const Vector base = arrow.origin + Vector(0.0f, 0.0f, height);
        const Vector tip = base + Vector(0.0f, 0.0f, -wing);
        drawWorldLine(base, tip, d3dColor);
        drawWorldLine(base + Vector(wing, 0.0f, 0.0f), tip, d3dColor);
        drawWorldLine(base + Vector(-wing, 0.0f, 0.0f), tip, d3dColor);
        drawWorldLine(base + Vector(0.0f, wing, 0.0f), tip, d3dColor);
        drawWorldLine(base + Vector(0.0f, -wing, 0.0f), tip, d3dColor);
    }

    EndPostMirrorOverlayD3DState(stateBlock);
    device->Release();
}

void VR::DrawPostMirrorPluginOverlays(IMatRenderContext* renderContext, C_BasePlayer* localPlayer, const CViewSetup& view, int eyeIndex)
{
    if (!renderContext)
        return;

    // Raw D3D draws after Source RenderView are unsafe in queued mode. Item labels still
    // use the same GDI-generated textures as the single-threaded path, but their textured
    // quads are published here and drawn later from DXVK's safe eye-submit point.
    if (m_Game && m_Game->GetMatQueueMode() != 0)
    {
        DrawProjectedItemLabels(renderContext, view, eyeIndex);
        return;
    }

    DrawProjectedItemLabels(renderContext, view, eyeIndex);
    DrawProjectedSpecialInfectedArrows(renderContext, view);

    if (!m_DesktopMirrorHidePluginOverlays || !m_DesktopMirrorEnabled)
        return;
    if (!localPlayer || !m_D3DAimLineOverlayEnabled || !m_IsVREnabled)
        return;
    if (view.width <= 0 || view.height <= 0 || view.fov <= 1.0f)
        return;

    if (m_HasThrowArc)
    {
        m_ThrowArcMaxHz = GetHmdDisplayFrequencyHz();
        if (ShouldThrottle(m_LastPostMirrorThrowArcDrawTime, m_ThrowArcMaxHz))
            return;
    }
    else
    {
        m_AimLineMaxHz = GetHmdDisplayFrequencyHz();
        if (ShouldThrottle(m_LastPostMirrorAimLineDrawTime, m_AimLineMaxHz))
            return;
    }

    QAngle viewAngles(view.angles.x, view.angles.y, view.angles.z);
    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(viewAngles, &forward, &right, &up);
    if (forward.IsZero() || right.IsZero() || up.IsZero())
        return;
    VectorNormalize(forward);
    VectorNormalize(right);
    VectorNormalize(up);

    const float aspect = (view.m_flAspectRatio > 0.01f)
        ? view.m_flAspectRatio
        : (static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)));
    const float tanHalfFovX = std::tan(DEG2RAD(view.fov * 0.5f));
    const float tanHalfFovY = tanHalfFovX / std::max(aspect, 0.01f);
    if (tanHalfFovX <= 0.0001f || tanHalfFovY <= 0.0001f)
        return;

    IDirect3DDevice9* device = GetKillIndicatorD3DDevice(this);
    if (!device)
        return;

    IDirect3DStateBlock9* stateBlock = nullptr;
    BeginPostMirrorOverlayD3DState(device, stateBlock);

    int colorR = 0, colorG = 0, colorB = 0, colorA = 0;
    GetAimLineColor(colorR, colorG, colorB, colorA);
    const D3DCOLOR aimColor = D3DCOLOR_ARGB(std::clamp(colorA, 0, 255), std::clamp(colorR, 0, 255), std::clamp(colorG, 0, 255), std::clamp(colorB, 0, 255));

    if (colorA > 0)
    {
        auto drawWorldLine = [&](const Vector& a, const Vector& b)
            {
                float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
                if (!ProjectPostMirrorSegment(view, forward, right, up, tanHalfFovX, tanHalfFovY, a, b, x0, y0, x1, y1))
                    return;
                DrawPostMirrorLine2D(device, x0, y0, x1, y1, aimColor, std::max(1.0f, m_AimLineThickness));
            };

        if (m_HasThrowArc)
        {
            for (int i = 0; i < THROW_ARC_SEGMENTS; ++i)
                drawWorldLine(m_LastThrowArcPoints[i], m_LastThrowArcPoints[i + 1]);
        }
        else
        {
            Vector start{};
            Vector end{};
            if (BuildRenderAimLineSegment(localPlayer, start, end)
                && ApplyD3DAimLineOcclusionFromAimLine(localPlayer, start, end))
            {
                drawWorldLine(start, end);
            }
        }
    }

    EndPostMirrorOverlayD3DState(stateBlock);
    device->Release();
}

namespace
{
    // Avoid referencing the external IID_IDirect3DTexture9 symbol.
    // This project does not always link dxguid.lib, so using the SDK global IID
    // can produce LNK2001. The value is the D3D9 IID for IDirect3DTexture9.
    static const GUID kDesktopMirrorIID_IDirect3DTexture9 =
    {
        0x85c31227, 0x3de5, 0x4f00,
        { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 }
    };

    struct ScopeLensVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };

    struct ScopeLensColorVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
    };

    static constexpr DWORD kScopeLensTexturedFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    static constexpr DWORD kScopeLensColorFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

    static inline float ScopeClamp01(float v)
    {
        return std::clamp(v, 0.0f, 1.0f);
    }

    static inline D3DCOLOR ScopeColor(int a, int r, int g, int b)
    {
        return D3DCOLOR_ARGB(std::clamp(a, 0, 255), std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
    }

    static inline float ScopeSmoothStep01(float v)
    {
        v = ScopeClamp01(v);
        return v * v * (3.0f - 2.0f * v);
    }

    static void ScopeAddTexturedVertex(std::vector<ScopeLensVertex>& out, float cx, float cy, float radius, float nx, float ny)
    {
        const float r2 = nx * nx + ny * ny;
        const float r = std::sqrt(std::max(0.0f, r2));
        const float rClamped = ScopeClamp01(r);

        // Rifle scopes are corrected optics. The center should stay almost perfectly rectilinear;
        // only the last outer band gets a very small radial compression so the edge reads as glass,
        // not as a fish-eye lens.
        const float edgeWarp = ScopeSmoothStep01((rClamped - 0.78f) / 0.22f);
        const float radialScale = 1.0f - 0.018f * edgeWarp * edgeWarp;
        float sx = nx * radialScale;
        float sy = ny * radialScale;

        sx = std::clamp(sx, -0.998f, 0.998f);
        sy = std::clamp(sy, -0.998f, 0.998f);

        // Mild optical vignetting: clear center, darker rim. This is what makes it look like a scope
        // without bending the whole image.
        const float vignette = ScopeSmoothStep01((rClamped - 0.58f) / 0.42f);
        const float rim = ScopeSmoothStep01((rClamped - 0.90f) / 0.10f);
        const float shade = std::clamp(1.0f - 0.20f * vignette * vignette - 0.32f * rim, 0.46f, 1.0f);
        const int c = static_cast<int>(std::lround(255.0f * shade));

        out.push_back(ScopeLensVertex{
            cx + nx * radius - 0.5f,
            cy + ny * radius - 0.5f,
            0.0f,
            1.0f,
            ScopeColor(255, c, c, c),
            0.5f + sx * 0.5f,
            0.5f + sy * 0.5f
        });
    }

    static void ScopeAddTexturedTri(std::vector<ScopeLensVertex>& out, float cx, float cy, float radius,
        float ax, float ay, float bx, float by, float cxn, float cyn)
    {
        ScopeAddTexturedVertex(out, cx, cy, radius, ax, ay);
        ScopeAddTexturedVertex(out, cx, cy, radius, bx, by);
        ScopeAddTexturedVertex(out, cx, cy, radius, cxn, cyn);
    }

    static void ScopeAddThickLine(std::vector<ScopeLensColorVertex>& out,
        float x0, float y0, float x1, float y1, float width, D3DCOLOR color)
    {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 0.001f) || !(width > 0.0f))
            return;

        const float px = (-dy / len) * (width * 0.5f);
        const float py = ( dx / len) * (width * 0.5f);

        const ScopeLensColorVertex v0{ x0 + px - 0.5f, y0 + py - 0.5f, 0.0f, 1.0f, color };
        const ScopeLensColorVertex v1{ x1 + px - 0.5f, y1 + py - 0.5f, 0.0f, 1.0f, color };
        const ScopeLensColorVertex v2{ x1 - px - 0.5f, y1 - py - 0.5f, 0.0f, 1.0f, color };
        const ScopeLensColorVertex v3{ x0 - px - 0.5f, y0 - py - 0.5f, 0.0f, 1.0f, color };

        out.push_back(v0); out.push_back(v1); out.push_back(v2);
        out.push_back(v0); out.push_back(v2); out.push_back(v3);
    }

    static void ScopeAddRing(std::vector<ScopeLensColorVertex>& out,
        float cx, float cy, float radius, float width, int segments, D3DCOLOR color)
    {
        constexpr float kTwoPi = 6.2831853071795864769f;
        segments = std::max(12, segments);
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * kTwoPi;
            const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * kTwoPi;
            ScopeAddThickLine(out,
                cx + std::cos(a0) * radius,
                cy + std::sin(a0) * radius,
                cx + std::cos(a1) * radius,
                cy + std::sin(a1) * radius,
                width,
                color);
        }
    }

    static void ScopeBuildLensMesh(std::vector<ScopeLensVertex>& out, float cx, float cy, float radius)
    {
        constexpr int kSegments = 96;
        constexpr int kRings = 24;
        constexpr float kTwoPi = 6.2831853071795864769f;
        out.clear();
        out.reserve(static_cast<size_t>(kSegments) * static_cast<size_t>(kRings) * 6u);

        for (int ring = 0; ring < kRings; ++ring)
        {
            const float r0 = static_cast<float>(ring) / static_cast<float>(kRings);
            const float r1 = static_cast<float>(ring + 1) / static_cast<float>(kRings);
            for (int seg = 0; seg < kSegments; ++seg)
            {
                const float a0 = (static_cast<float>(seg) / static_cast<float>(kSegments)) * kTwoPi;
                const float a1 = (static_cast<float>(seg + 1) / static_cast<float>(kSegments)) * kTwoPi;

                const float x00 = std::cos(a0) * r0;
                const float y00 = std::sin(a0) * r0;
                const float x10 = std::cos(a1) * r0;
                const float y10 = std::sin(a1) * r0;
                const float x01 = std::cos(a0) * r1;
                const float y01 = std::sin(a0) * r1;
                const float x11 = std::cos(a1) * r1;
                const float y11 = std::sin(a1) * r1;

                ScopeAddTexturedTri(out, cx, cy, radius, x00, y00, x01, y01, x11, y11);
                ScopeAddTexturedTri(out, cx, cy, radius, x00, y00, x11, y11, x10, y10);
            }
        }
    }

    static void ScopeBuildReticleMesh(std::vector<ScopeLensColorVertex>& out, float cx, float cy, float radius, float reticleAlphaScale)
    {
        out.clear();
        out.reserve(4096);

        reticleAlphaScale = ScopeClamp01(reticleAlphaScale);
        auto scaledAlpha = [&](int alpha)
            {
                return std::clamp(static_cast<int>(std::lround(static_cast<float>(alpha) * reticleAlphaScale)), 0, 255);
            };

        const float px = std::max(1.0f, radius / 240.0f);
        const D3DCOLOR outerRing = ScopeColor(230, 7, 8, 7);
        const D3DCOLOR innerRing = ScopeColor(110, 220, 255, 210);
        const D3DCOLOR reticleShadow = ScopeColor(scaledAlpha(145), 0, 0, 0);
        const D3DCOLOR reticle = ScopeColor(scaledAlpha(235), 80, 255, 145);
        const D3DCOLOR reticleHot = ScopeColor(scaledAlpha(245), 210, 255, 220);

        ScopeAddRing(out, cx, cy, radius * 0.998f, std::max(4.0f, px * 5.5f), 128, outerRing);
        ScopeAddRing(out, cx, cy, radius * 0.965f, std::max(1.0f, px * 1.2f), 128, innerRing);

        const float postNear = radius * 0.075f;
        const float postFar = radius * 0.42f;
        const float postWidth = std::max(1.4f, px * 1.8f);
        const float shadowWidth = postWidth + std::max(1.0f, px * 1.5f);

        ScopeAddThickLine(out, cx - postFar, cy, cx - postNear, cy, shadowWidth, reticleShadow);
        ScopeAddThickLine(out, cx + postNear, cy, cx + postFar, cy, shadowWidth, reticleShadow);
        ScopeAddThickLine(out, cx, cy - postFar, cx, cy - postNear, shadowWidth, reticleShadow);
        ScopeAddThickLine(out, cx, cy + postNear, cx, cy + postFar, shadowWidth, reticleShadow);

        ScopeAddThickLine(out, cx - postFar, cy, cx - postNear, cy, postWidth, reticle);
        ScopeAddThickLine(out, cx + postNear, cy, cx + postFar, cy, postWidth, reticle);
        ScopeAddThickLine(out, cx, cy - postFar, cx, cy - postNear, postWidth, reticle);
        ScopeAddThickLine(out, cx, cy + postNear, cx, cy + postFar, postWidth, reticle);

        ScopeAddRing(out, cx, cy, radius * 0.105f, std::max(1.1f, px * 1.3f), 64, reticle);
        ScopeAddRing(out, cx, cy, radius * 0.018f, std::max(1.0f, px * 1.2f), 32, reticleHot);

        const float tick0 = radius * 0.19f;
        const float tick1 = radius * 0.23f;
        const float tickHalf = radius * 0.018f;
        for (int signIdx = 0; signIdx < 2; ++signIdx)
        {
            const int sign = (signIdx == 0) ? -1 : 1;
            const float x = cx + static_cast<float>(sign) * tick1;
            ScopeAddThickLine(out, x, cy - tickHalf, x, cy + tickHalf, std::max(1.0f, px * 1.2f), reticle);
            const float y = cy + static_cast<float>(sign) * tick1;
            ScopeAddThickLine(out, cx - tickHalf, y, cx + tickHalf, y, std::max(1.0f, px * 1.2f), reticle);
        }

        // A small lower stadia mark makes it read more like an optic instead of a flat crosshair.
        ScopeAddThickLine(out, cx - radius * 0.055f, cy + tick0, cx + radius * 0.055f, cy + tick0, std::max(1.0f, px * 1.1f), reticle);
    }


    static void ScopeReleaseSurface(IDirect3DSurface9*& surface)
    {
        if (surface)
        {
            surface->Release();
            surface = nullptr;
        }
    }

    static void ScopeReleaseTexture(IDirect3DTexture9*& texture)
    {
        if (texture)
        {
            texture->Release();
            texture = nullptr;
        }
    }

}

bool VR::ApplyScopeLensPostProcess()
{
    if (!g_D3DVR9 || !m_CreatedVRTextures.load(std::memory_order_acquire))
        return false;

    IDirect3DSurface9* scopeSurface = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DStateBlock9* stateBlock = nullptr;
    IDirect3DSurface9* oldRenderTarget = nullptr;
    IDirect3DSurface9* oldDepthStencil = nullptr;
    D3DVIEWPORT9 oldViewport{};
    bool haveOldViewport = false;

    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
    if (m_CreatingTextureID != Texture_None || !m_D9ScopeSurface)
        return false;

    scopeSurface = m_D9ScopeSurface;
    scopeSurface->AddRef();

    auto releaseRefs = [&]()
        {
            ScopeReleaseSurface(oldRenderTarget);
            ScopeReleaseSurface(oldDepthStencil);
            if (stateBlock)
            {
                stateBlock->Release();
                stateBlock = nullptr;
            }
            if (device)
            {
                device->Release();
                device = nullptr;
            }
            ScopeReleaseSurface(scopeSurface);
        };

    auto destroyScopeLensOutput = [&]()
        {
            ScopeReleaseSurface(m_D9ScopeLensSurface);
            ScopeReleaseTexture(m_D9ScopeLensTexture);
            std::memset(&m_VKScopeLens, 0, sizeof(m_VKScopeLens));
            m_ScopeLensOverlayReady.store(0u, std::memory_order_release);
        };

    auto destroyScopeLensScratch = [&]()
        {
            ScopeReleaseSurface(m_D9ScopeLensScratchSurface);
            ScopeReleaseTexture(m_D9ScopeLensScratchTexture);
            m_D9ScopeLensScratchW = 0;
            m_D9ScopeLensScratchH = 0;
            m_D9ScopeLensScratchFormat = 0;
        };

    HRESULT hr = scopeSurface->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        releaseRefs();
        return false;
    }

    D3DSURFACE_DESC desc{};
    hr = scopeSurface->GetDesc(&desc);
    if (FAILED(hr) || desc.Width == 0 || desc.Height == 0)
    {
        releaseRefs();
        return false;
    }

    g_D3DVR9->LockDevice();
    bool locked = true;
    auto unlockDevice = [&]()
        {
            if (locked)
            {
                g_D3DVR9->UnlockDevice();
                locked = false;
            }
        };

    const bool resourceMismatch =
        !m_D9ScopeLensScratchTexture ||
        !m_D9ScopeLensScratchSurface ||
        !m_D9ScopeLensTexture ||
        !m_D9ScopeLensSurface ||
        m_D9ScopeLensScratchW != desc.Width ||
        m_D9ScopeLensScratchH != desc.Height ||
        m_D9ScopeLensScratchFormat != static_cast<uint32_t>(desc.Format);

    if (resourceMismatch)
    {
        destroyScopeLensOutput();
        destroyScopeLensScratch();

        hr = device->CreateTexture(
            desc.Width,
            desc.Height,
            1,
            D3DUSAGE_RENDERTARGET,
            desc.Format,
            D3DPOOL_DEFAULT,
            &m_D9ScopeLensScratchTexture,
            nullptr);
        if (SUCCEEDED(hr) && m_D9ScopeLensScratchTexture)
            hr = m_D9ScopeLensScratchTexture->GetSurfaceLevel(0, &m_D9ScopeLensScratchSurface);

        if (SUCCEEDED(hr))
        {
            hr = device->CreateTexture(
                desc.Width,
                desc.Height,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                &m_D9ScopeLensTexture,
                nullptr);
        }
        if (SUCCEEDED(hr) && m_D9ScopeLensTexture)
            hr = m_D9ScopeLensTexture->GetSurfaceLevel(0, &m_D9ScopeLensSurface);

        if (SUCCEEDED(hr) && m_D9ScopeLensSurface)
        {
            D3D9_TEXTURE_VR_DESC vrDesc{};
            hr = g_D3DVR9->GetVRDesc(m_D9ScopeLensSurface, &vrDesc);
            if (SUCCEEDED(hr))
            {
                std::memcpy(&m_VKScopeLens.m_VulkanData, &vrDesc, sizeof(vr::VRVulkanTextureData_t));
                m_VKScopeLens.m_VRTexture.handle = &m_VKScopeLens.m_VulkanData;
                m_VKScopeLens.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
                m_VKScopeLens.m_VRTexture.eType = vr::TextureType_Vulkan;
            }
        }

        if (FAILED(hr) || !m_D9ScopeLensScratchTexture || !m_D9ScopeLensScratchSurface ||
            !m_D9ScopeLensTexture || !m_D9ScopeLensSurface || !m_VKScopeLens.m_VRTexture.handle)
        {
            destroyScopeLensOutput();
            destroyScopeLensScratch();
            unlockDevice();
            releaseRefs();
            return false;
        }

        m_D9ScopeLensScratchW = desc.Width;
        m_D9ScopeLensScratchH = desc.Height;
        m_D9ScopeLensScratchFormat = static_cast<uint32_t>(desc.Format);
    }

    hr = device->StretchRect(scopeSurface, nullptr, m_D9ScopeLensScratchSurface, nullptr, D3DTEXF_LINEAR);
    if (FAILED(hr))
    {
        unlockDevice();
        releaseRefs();
        return false;
    }

    const HRESULT oldRtHr = device->GetRenderTarget(0, &oldRenderTarget);
    const HRESULT oldDepthHr = device->GetDepthStencilSurface(&oldDepthStencil);
    haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = desc.Width;
    viewport.Height = desc.Height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    bool ok = true;
    ok = ok && SUCCEEDED(device->SetRenderTarget(0, m_D9ScopeLensSurface));
    if (ok)
    {
        device->SetDepthStencilSurface(nullptr);
        device->SetViewport(&viewport);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, ScopeColor(0, 0, 0, 0), 1.0f, 0);

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kScopeLensTexturedFvf);
        device->SetTexture(0, m_D9ScopeLensScratchTexture);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        static thread_local std::vector<ScopeLensVertex> lensVerts;
        const float size = static_cast<float>(std::min(desc.Width, desc.Height));
        const float cx = static_cast<float>(desc.Width) * 0.5f;
        const float cy = static_cast<float>(desc.Height) * 0.5f;
        const float radius = size * 0.492f;
        ScopeBuildLensMesh(lensVerts, cx, cy, radius);
        if (!lensVerts.empty())
        {
            hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
                static_cast<UINT>(lensVerts.size() / 3u),
                lensVerts.data(),
                sizeof(ScopeLensVertex));
            ok = ok && SUCCEEDED(hr);
        }

        device->SetTexture(0, nullptr);
        device->SetFVF(kScopeLensColorFvf);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

        static thread_local std::vector<ScopeLensColorVertex> reticleVerts;
        ScopeBuildReticleMesh(reticleVerts, cx, cy, radius, m_ScopeReticleAlpha);
        if (!reticleVerts.empty())
        {
            hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
                static_cast<UINT>(reticleVerts.size() / 3u),
                reticleVerts.data(),
                sizeof(ScopeLensColorVertex));
            ok = ok && SUCCEEDED(hr);
        }
    }

    if (stateBlock)
        stateBlock->Apply();
    else
        device->SetTexture(0, nullptr);

    if (SUCCEEDED(oldRtHr) && oldRenderTarget)
        device->SetRenderTarget(0, oldRenderTarget);
    if (SUCCEEDED(oldDepthHr))
        device->SetDepthStencilSurface(oldDepthStencil);
    if (haveOldViewport)
        device->SetViewport(&oldViewport);

    if (ok)
    {
        hr = g_D3DVR9->TransferSurface(m_D9ScopeLensSurface, FALSE);
        ok = SUCCEEDED(hr);
    }

    m_ScopeLensOverlayReady.store(ok ? 1u : 0u, std::memory_order_release);

    unlockDevice();
    releaseRefs();
    return ok;
}

bool VR::CopyLeftEyeToRightEyeTexture()
{
    if (!m_IsVREnabled || !m_RightEyeCopyFromLeft)
        return false;

    // Single-threaded only. In queued rendering, direct D3D9 copy/draw work can race
    // Source/DXVK command submission. This mode is a deliberate performance option
    // that replaces the real right-eye scene with a copy of the left-eye scene.
    if (m_Game && m_Game->GetMatQueueMode() != 0)
        return false;

    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return false;

    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* dst = nullptr;
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (m_CreatingTextureID != Texture_None)
            return false;

        src = m_D9LeftEyeSurface;
        dst = m_D9RightEyeSurface;
        if (!src || !dst || src == dst)
            return false;

        src->AddRef();
        dst->AddRef();
    }

    auto releaseSurfaceRefs = [&]()
        {
            if (src)
            {
                src->Release();
                src = nullptr;
            }
            if (dst)
            {
                dst->Release();
                dst = nullptr;
            }
        };

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = src->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        hr = dst->GetDevice(&device);
        if (FAILED(hr) || !device)
        {
            releaseSurfaceRefs();
            return false;
        }
    }

    const HRESULT cooperativeHr = device->TestCooperativeLevel();
    if (cooperativeHr == D3DERR_DEVICELOST ||
        cooperativeHr == D3DERR_DEVICENOTRESET ||
        cooperativeHr == D3DERR_DRIVERINTERNALERROR)
    {
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    if (FAILED(src->GetDesc(&srcDesc)) || FAILED(dst->GetDesc(&dstDesc)) ||
        srcDesc.Width == 0 || srcDesc.Height == 0 ||
        dstDesc.Width == 0 || dstDesc.Height == 0)
    {
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    IDirect3DTexture9* srcTexture = nullptr;
    hr = src->GetContainer(kDesktopMirrorIID_IDirect3DTexture9, reinterpret_cast<void**>(&srcTexture));
    if (FAILED(hr) || !srcTexture)
    {
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    IDirect3DSurface9* oldRenderTarget = nullptr;
    const HRESULT oldRtHr = device->GetRenderTarget(0, &oldRenderTarget);
    D3DVIEWPORT9 oldViewport{};
    const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = dstDesc.Width;
    viewport.Height = dstDesc.Height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    struct EyeCopyVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };
    static constexpr DWORD kEyeCopyFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    const float left = -0.5f;
    const float top = -0.5f;
    const float right = static_cast<float>(dstDesc.Width) - 0.5f;
    const float bottom = static_cast<float>(dstDesc.Height) - 0.5f;
    const EyeCopyVertex quad[4] =
    {
        { left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f }
    };

    bool ok = true;
    hr = device->SetRenderTarget(0, dst);
    ok = ok && SUCCEEDED(hr);
    if (ok)
    {
        device->SetViewport(&viewport);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kEyeCopyFvf);
        device->SetTexture(0, srcTexture);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
            D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(EyeCopyVertex));
        ok = SUCCEEDED(hr);
    }

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }
    else
    {
        device->SetTexture(0, nullptr);
    }

    if (SUCCEEDED(oldRtHr) && oldRenderTarget)
        device->SetRenderTarget(0, oldRenderTarget);
    if (haveOldViewport)
        device->SetViewport(&oldViewport);

    if (oldRenderTarget)
        oldRenderTarget->Release();
    srcTexture->Release();
    device->Release();
    releaseSurfaceRefs();
    return ok;
}

bool VR::CompositeHudToDesktopMirrorTexture()
{
    if (!m_IsVREnabled || !m_DesktopMirrorEnabled || !m_DesktopMirrorHidePluginOverlays)
        return false;

    const bool focusedInGameVgui =
        (m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused()) ||
        (m_Game && m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible());
    const bool gameplayHudRequested = IsGameplayHudRequested();
    if (!focusedInGameVgui && !gameplayHudRequested)
        return false;

    const bool queued = m_Game && m_Game->GetMatQueueMode() != 0;
    if (!m_HudPaintedThisFrame.load(std::memory_order_acquire) &&
        !m_RenderedHud.load(std::memory_order_acquire) &&
        !(queued && IsQueuedHudFresh()))
        return false;
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return false;

    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* dst = nullptr;
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (m_CreatingTextureID != Texture_None)
            return false;

        src = m_D9HUDSurface;
        dst = m_D9DesktopMirrorSurface;
        if (!src || !dst || src == dst)
            return false;

        src->AddRef();
        dst->AddRef();
    }

    auto releaseSurfaceRefs = [&]()
        {
            if (src)
            {
                src->Release();
                src = nullptr;
            }
            if (dst)
            {
                dst->Release();
                dst = nullptr;
            }
        };

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = dst->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        hr = src->GetDevice(&device);
        if (FAILED(hr) || !device)
        {
            releaseSurfaceRefs();
            return false;
        }
    }

    const HRESULT cooperativeHr = device->TestCooperativeLevel();
    if (cooperativeHr == D3DERR_DEVICELOST || cooperativeHr == D3DERR_DEVICENOTRESET || cooperativeHr == D3DERR_DRIVERINTERNALERROR)
    {
        if (m_RenderPipelineDebugLog)
            Game::logMsg("[VR][DesktopHUD] skip HUD composite during device transition hr=0x%08X", static_cast<unsigned int>(cooperativeHr));
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    if (FAILED(src->GetDesc(&srcDesc)) || FAILED(dst->GetDesc(&dstDesc)) ||
        srcDesc.Width == 0 || srcDesc.Height == 0 || dstDesc.Width == 0 || dstDesc.Height == 0)
    {
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    IDirect3DTexture9* srcTexture = nullptr;
    hr = src->GetContainer(kDesktopMirrorIID_IDirect3DTexture9, reinterpret_cast<void**>(&srcTexture));
    if (FAILED(hr) || !srcTexture)
    {
        if (m_RenderPipelineDebugLog)
            Game::logMsg("[VR][DesktopHUD] HUD surface has no texture container hr=0x%08X", static_cast<unsigned int>(hr));
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    IDirect3DSurface9* oldRenderTarget = nullptr;
    const HRESULT oldRtHr = device->GetRenderTarget(0, &oldRenderTarget);
    D3DVIEWPORT9 oldViewport{};
    const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = dstDesc.Width;
    viewport.Height = dstDesc.Height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    struct DesktopHudVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };
    static constexpr DWORD kDesktopHudFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    const float left = -0.5f;
    const float top = -0.5f;
    const float right = static_cast<float>(dstDesc.Width) - 0.5f;
    const float bottom = static_cast<float>(dstDesc.Height) - 0.5f;
    const DesktopHudVertex quad[4] =
    {
        { left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f }
    };

    bool ok = true;
    hr = device->SetRenderTarget(0, dst);
    ok = ok && SUCCEEDED(hr);
    if (ok)
    {
        device->SetViewport(&viewport);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kDesktopHudFvf);
        device->SetTexture(0, srcTexture);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        if (focusedInGameVgui)
        {
            // Menu / cursor UI is captured with meaningful alpha. Keep normal alpha blending.
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        }
        else
        {
            // Gameplay VGUI often writes RGB but leaves alpha at the transparent clear value.
            // VR overlays still display that RGB, but desktop alpha blending would make it invisible.
            // Use source-color compositing for gameplay HUD so black clear pixels remain no-op while HUD RGB appears.
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        }
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
            D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(DesktopHudVertex));
        ok = SUCCEEDED(hr);
    }

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }
    else
    {
        device->SetTexture(0, nullptr);
    }

    if (SUCCEEDED(oldRtHr) && oldRenderTarget)
        device->SetRenderTarget(0, oldRenderTarget);
    if (haveOldViewport)
        device->SetViewport(&oldViewport);

    if (!ok && m_RenderPipelineDebugLog)
        Game::logMsg("[VR][DesktopHUD] HUD composite failed hr=0x%08X", static_cast<unsigned int>(hr));

    if (oldRenderTarget)
        oldRenderTarget->Release();
    srcTexture->Release();
    device->Release();
    releaseSurfaceRefs();
    return ok;
}

bool VR::CopyEyeToDesktopMirrorTexture(int eyeIndex)
{
    if (!m_IsVREnabled || !m_DesktopMirrorEnabled || !m_DesktopMirrorHidePluginOverlays)
        return false;
    if (eyeIndex < 0 || eyeIndex > 1)
        return false;

    // Queued/multicore rendering updates desktopMirrorClean0 through a separate
    // clean Source RenderView pass. Direct D3D9 copies/draws here can collide with
    // DXVK's queued command stream, so this low-cost copy path is single-threaded only.
    if (m_Game && m_Game->GetMatQueueMode() != 0)
        return false;

    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return false;

    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* dst = nullptr;

    // The backing D3D surfaces can be released during video mode changes / DXVK reset.
    // Take strong references while holding the texture mutex, then do the actual GPU work
    // outside the lock so we do not block the texture lifecycle on command submission.
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (m_CreatingTextureID != Texture_None)
            return false;

        src = (eyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
        dst = m_D9DesktopMirrorSurface;
        if (!src || !dst || src == dst)
            return false;

        src->AddRef();
        dst->AddRef();
    }

    auto releaseSurfaceRefs = [&]()
        {
            if (src)
            {
                src->Release();
                src = nullptr;
            }
            if (dst)
            {
                dst->Release();
                dst = nullptr;
            }
        };

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = src->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        hr = dst->GetDevice(&device);
        if (FAILED(hr) || !device)
        {
            releaseSurfaceRefs();
            return false;
        }
    }

    const HRESULT cooperativeHr = device->TestCooperativeLevel();
    if (cooperativeHr == D3DERR_DEVICELOST || cooperativeHr == D3DERR_DEVICENOTRESET || cooperativeHr == D3DERR_DRIVERINTERNALERROR)
    {
        if (m_RenderPipelineDebugLog)
            Game::logMsg("[VR][DesktopMirror] skip clean copy during device transition hr=0x%08X", static_cast<unsigned int>(cooperativeHr));
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    if (FAILED(src->GetDesc(&srcDesc)) || FAILED(dst->GetDesc(&dstDesc)) || srcDesc.Width == 0 || srcDesc.Height == 0 || dstDesc.Width == 0 || dstDesc.Height == 0)
    {
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    // Do not use IDirect3DDevice9::StretchRect here. Under DXVK, the crash dump showed
    // StretchRect entering dxvk::DxvkCsChunk::push with a null command chunk. A textured
    // fullscreen quad stays on the same lightweight D3D draw path already used by our
    // post-mirror overlays and avoids the extra full RenderView cost.
    IDirect3DTexture9* srcTexture = nullptr;
    hr = src->GetContainer(kDesktopMirrorIID_IDirect3DTexture9, reinterpret_cast<void**>(&srcTexture));
    if (FAILED(hr) || !srcTexture)
    {
        if (m_RenderPipelineDebugLog)
            Game::logMsg("[VR][DesktopMirror] source eye surface has no texture container eye=%d hr=0x%08X", eyeIndex, static_cast<unsigned int>(hr));
        device->Release();
        releaseSurfaceRefs();
        return false;
    }

    IDirect3DSurface9* oldRenderTarget = nullptr;
    const HRESULT oldRtHr = device->GetRenderTarget(0, &oldRenderTarget);
    D3DVIEWPORT9 oldViewport{};
    const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = dstDesc.Width;
    viewport.Height = dstDesc.Height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    struct DesktopMirrorCopyVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };
    static constexpr DWORD kDesktopMirrorCopyFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    const float left = -0.5f;
    const float top = -0.5f;
    const float right = static_cast<float>(dstDesc.Width) - 0.5f;
    const float bottom = static_cast<float>(dstDesc.Height) - 0.5f;
    const DesktopMirrorCopyVertex quad[4] =
    {
        { left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f }
    };

    bool ok = true;
    hr = device->SetRenderTarget(0, dst);
    ok = ok && SUCCEEDED(hr);
    if (ok)
    {
        device->SetViewport(&viewport);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kDesktopMirrorCopyFvf);
        device->SetTexture(0, srcTexture);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        const DWORD filter = m_DesktopMirrorLinearFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(DesktopMirrorCopyVertex));
        ok = SUCCEEDED(hr);
    }

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }
    else
    {
        device->SetTexture(0, nullptr);
    }
    if (SUCCEEDED(oldRtHr) && oldRenderTarget)
        device->SetRenderTarget(0, oldRenderTarget);
    if (haveOldViewport)
        device->SetViewport(&oldViewport);

    if (!ok && m_RenderPipelineDebugLog)
        Game::logMsg("[VR][DesktopMirror] quad clean copy eye=%d failed hr=0x%08X", eyeIndex, static_cast<unsigned int>(hr));

    if (oldRenderTarget)
        oldRenderTarget->Release();
    srcTexture->Release();
    device->Release();
    releaseSurfaceRefs();

    // Do not composite HUD into desktopMirrorClean0 here. DXVK's Present path is the
    // final owner of desktop mirroring and composites m_HUDTexture after StretchRect.
    // Doing it here as well can double-draw the HUD on single-threaded rendering.

    return ok;
}

void VR::DrawKillIndicators(IMatRenderContext* renderContext, ITexture* hudTexture)
{
    if (!renderContext)
        return;

    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, true);

    if (!m_KillIndicatorEnabled || m_ActiveKillIndicators.empty() || !hudTexture)
        return;

    if (m_IsVREnabled && (m_Overlay || vr::VROverlay()))
        return;

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
    {
        TrimExpiredKillIndicators(now, true);
        return;
    }

    IMaterial* hitMaterial = ResolveHitIndicatorMaterial();
    IMaterial* normalMaterial = ResolveKillIndicatorMaterial(false);
    IMaterial* headshotMaterial = ResolveKillIndicatorMaterial(true);
    if (!hitMaterial && !normalMaterial && !headshotMaterial)
        return;

    int screenWidth = hudTexture->GetMappingWidth();
    int screenHeight = hudTexture->GetMappingHeight();
    if (screenWidth <= 0 || screenHeight <= 0)
        renderContext->GetWindowSize(screenWidth, screenHeight);
    if (screenWidth <= 0 || screenHeight <= 0)
        return;

    const float baseSizePixels = (std::max)(16.0f, m_KillIndicatorSizePixels);
    for (const ActiveKillIndicator& indicator : m_ActiveKillIndicators)
    {
        IMaterial* material = nullptr;
        if (!indicator.killConfirmed)
            material = hitMaterial;
        else
            material = indicator.headshot ? headshotMaterial : normalMaterial;
        if (!material)
            material = normalMaterial ? normalMaterial : (headshotMaterial ? headshotMaterial : hitMaterial);
        if (!material)
            continue;

        const float lifetime = GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
        const float ageSeconds = std::chrono::duration<float>(now - indicator.startedAt).count();
        const float progress = Clamp01(ageSeconds / lifetime);
        const float introWindow = indicator.killConfirmed ? 0.22f : 0.18f;
        const float intro = Clamp01(progress / introWindow);
        const float fadeStart = indicator.killConfirmed ? 0.72f : 0.58f;
        const float fadeWidth = indicator.killConfirmed ? 0.28f : 0.42f;
        const float fade = 1.0f - Clamp01((progress - fadeStart) / fadeWidth);
        const float pulse = std::sin(intro * 1.57079632679f);

        Vector drawPos = indicator.worldPos;

        int screenX = 0;
        int screenY = 0;
        if (!ProjectKillIndicatorToHud(this, drawPos, screenWidth, screenHeight, m_KillIndicatorMaxDistance, screenX, screenY))
            continue;

        float scale = indicator.killConfirmed ? (0.78f + 0.34f * pulse) : (0.56f + 0.24f * pulse);
        if (indicator.killConfirmed && indicator.headshot)
            scale *= 1.10f;

        const float alphaBase = indicator.killConfirmed ? 0.72f : 0.60f;
        const float alpha = Clamp01((alphaBase + (1.0f - alphaBase) * intro) * fade);
        const int texWidth = (std::max)(1, material->GetMappingWidth());
        const int texHeight = (std::max)(1, material->GetMappingHeight());
        const float texAspect = static_cast<float>(texWidth) / static_cast<float>(texHeight);
        const int drawHeight = (std::max)(8, static_cast<int>(std::lround(baseSizePixels * scale)));
        const int drawWidth = (std::max)(8, static_cast<int>(std::lround(static_cast<float>(drawHeight) * texAspect)));

        const float colorG = (!indicator.killConfirmed || indicator.headshot) ? 0.94f : 1.0f;
        const float colorB = (!indicator.killConfirmed || indicator.headshot) ? 0.94f : 1.0f;
        material->ColorModulate(1.0f, colorG, colorB);
        material->AlphaModulate(alpha);
        renderContext->DrawScreenSpaceRectangle(
            material,
            screenX - drawWidth / 2,
            screenY - drawHeight / 2,
            drawWidth,
            drawHeight,
            0.0f,
            0.0f,
            static_cast<float>(texWidth),
            static_cast<float>(texHeight),
            texWidth,
            texHeight);
    }

    if (hitMaterial)
    {
        hitMaterial->ColorModulate(1.0f, 1.0f, 1.0f);
        hitMaterial->AlphaModulate(1.0f);
    }
    if (normalMaterial)
    {
        normalMaterial->ColorModulate(1.0f, 1.0f, 1.0f);
        normalMaterial->AlphaModulate(1.0f);
    }
    if (headshotMaterial && headshotMaterial != normalMaterial)
    {
        headshotMaterial->ColorModulate(1.0f, 1.0f, 1.0f);
        headshotMaterial->AlphaModulate(1.0f);
    }
}

void VR::UpdateKillSoundFeedback()
{
    auto resetState = [&]()
        {
            const bool hadFeedbackState = m_HitSoundPending
                || !m_PendingKillSoundHits.empty()
                || !m_PendingKillSoundEvents.empty()
                || !m_FeedbackSoundWarmupSignature.empty();
            if (hadFeedbackState)
                ResetFeedbackSoundWorkerState();

            TrimExpiredKillIndicators(std::chrono::steady_clock::now(), true);
            m_PendingKillSoundHits.clear();
            m_PendingKillSoundEvents.clear();
            m_HitSoundPending = false;
            m_HitSoundPendingMergedCount = 0;
            m_HitSoundPendingShotSerial = 0;
            m_HitSoundPendingEntityTag = 0;
            m_HitSoundPendingWorldPos = Vector{};
            m_HitSoundPendingQueuedAt = {};
            m_LastHitSoundQueueAcceptedTime = {};
            m_LastHitSoundQueueAcceptedWorldPos = Vector{};
            m_LastHitSoundQueueAcceptedEntityTag = 0;
            m_RecentHitSoundPlaybackShotSerials.fill(0);
            m_RecentHitSoundPlaybackShotSerialCursor = 0;
            m_LastKillSoundCommonKills = -1;
            m_LastKillSoundSpecialKills = -1;
            m_PredictedHitFeedbackShotSerial = 0;
            m_LastPredictedHitSoundShotSerial = 0;
            m_CurrentPredictedHitFeedbackCmdNumber = 0;
            m_RecentPredictedHitFeedbackShotSerials.fill(0);
            m_RecentPredictedHitFeedbackShotSerialCursor = 0;
            m_RecentPredictedHitFeedbackEmissions = {};
            m_RecentPredictedHitFeedbackEmissionCursor = 0;
            m_LastPredictedHitFeedbackShotTime = {};
            m_FeedbackSoundWarmupSignature.clear();
        };

    const bool wantsHitFeedback = m_HitSoundEnabled || m_HitIndicatorEnabled;
    const bool wantsKillFeedback = m_KillSoundEnabled || m_KillIndicatorEnabled;
    const bool wantsAnyFeedback = wantsHitFeedback || wantsKillFeedback;
    if (!wantsAnyFeedback || !m_Game || !m_Game->m_EngineClient)
    {
        resetState();
        return;
    }

    if (!m_Game->m_EngineClient->IsInGame())
    {
        resetState();
        return;
    }

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(SafeGetProjectedItemLabelClientEntity(this, localPlayerIndex));
    if (!localPlayer)
    {
        resetState();
        return;
    }

    EnsureFeedbackSoundWarmup();

    const auto now = std::chrono::steady_clock::now();
    FlushPendingHitSound(now);
    EnsureKillSoundEventListener();
    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    if (!wantsKillFeedback)
        return;

    int commonKills = 0;
    int specialKills = 0;
    if (!ReadLocalKillCounters(localPlayer, commonKills, specialKills))
    {
        m_LastKillSoundCommonKills = -1;
        m_LastKillSoundSpecialKills = -1;
        return;
    }

    if (m_LastKillSoundCommonKills < 0 || m_LastKillSoundSpecialKills < 0)
    {
        m_LastKillSoundCommonKills = commonKills;
        m_LastKillSoundSpecialKills = specialKills;
        return;
    }

    const int deltaCommon = commonKills - m_LastKillSoundCommonKills;
    const int deltaSpecial = specialKills - m_LastKillSoundSpecialKills;
    if (deltaCommon < 0 || deltaSpecial < 0)
    {
        m_LastKillSoundCommonKills = commonKills;
        m_LastKillSoundSpecialKills = specialKills;
        m_PendingKillSoundHits.clear();
        m_PendingKillSoundEvents.clear();
        return;
    }

    const int totalDelta = deltaCommon + deltaSpecial;

    for (int i = 0; i < totalDelta; ++i)
    {
        bool headshot = false;
        std::uintptr_t entityTag = 0;
        const bool hasEvent = ConsumePendingKillSoundEvent(now, headshot, entityTag);
        Vector impactPos{};
        bool matched = false;
        if (entityTag != 0)
            matched = ConsumePendingKillSoundHit(entityTag, now, &impactPos);
        if (!matched && hasEvent)
            matched = ConsumePendingKillSoundHit(0, now, &impactPos);

        if (matched)
        {
            PlayKillSound(headshot, &impactPos);
            SpawnKillIndicator(headshot, impactPos);
        }
        else
        {
            const bool unresolvedHeadshot = hasEvent && headshot;
            PlayKillSound(unresolvedHeadshot, nullptr);
            SpawnKillIndicator(unresolvedHeadshot, Vector{});
        }
    }

    m_LastKillSoundCommonKills = commonKills;
    m_LastKillSoundSpecialKills = specialKills;
}

// -----------------------------------------------------------------------------
// Optional special-infected / item-label feature bridge
// -----------------------------------------------------------------------------
// These VR methods are called from core code and hooks even when the
// optional special_infected_features.cpp translation unit is removed from the
// project. Keep stable wrappers here so stripped builds link cleanly. When the
// optional file is linked, it registers the real implementation callbacks during
// static initialization and the wrappers forward to them.
namespace
{
    using L4D2VROptionalDrawItemModelLabelFn = void(__cdecl*)(VR*, int, const std::string&, const Vector&, const C_BaseEntity*, const char*);
    using L4D2VROptionalScanFn = void(__cdecl*)(VR*);
    using L4D2VROptionalCreateMoveFn = void(__cdecl*)(VR*, CUserCmd*, int, bool, bool, float, float, bool);

    struct L4D2VROptionalSpecialInfectedCallbacks
    {
        L4D2VROptionalDrawItemModelLabelFn drawItemModelLabel = nullptr;
        L4D2VROptionalScanFn scanSpecialInfectedEntities = nullptr;
        L4D2VROptionalScanFn scanItemModelLabelEntities = nullptr;
        L4D2VROptionalCreateMoveFn applyCreateMoveFeatures = nullptr;
    };

    L4D2VROptionalSpecialInfectedCallbacks& GetOptionalSpecialInfectedCallbacks()
    {
        static L4D2VROptionalSpecialInfectedCallbacks callbacks;
        return callbacks;
    }
}

extern "C" void __cdecl L4D2VR_RegisterSpecialInfectedFeatureCallbacks(
    L4D2VROptionalDrawItemModelLabelFn drawItemModelLabel,
    L4D2VROptionalScanFn scanSpecialInfectedEntities,
    L4D2VROptionalScanFn scanItemModelLabelEntities,
    L4D2VROptionalCreateMoveFn applyCreateMoveFeatures)
{
    auto& callbacks = GetOptionalSpecialInfectedCallbacks();
    callbacks.drawItemModelLabel = drawItemModelLabel;
    callbacks.scanSpecialInfectedEntities = scanSpecialInfectedEntities;
    callbacks.scanItemModelLabelEntities = scanItemModelLabelEntities;
    callbacks.applyCreateMoveFeatures = applyCreateMoveFeatures;
}

void VR::DrawItemModelLabel(int entityIndex, const std::string& modelName, const Vector& modelOrigin, const C_BaseEntity* entity, const char* className)
{
    if (!m_ItemModelLabelEnabled)
        return;

    auto* callback = GetOptionalSpecialInfectedCallbacks().drawItemModelLabel;
    if (callback)
        callback(this, entityIndex, modelName, modelOrigin, entity, className);
}

void VR::ScanSpecialInfectedEntitiesFromClientList()
{
    const bool wantsEntityScan =
        m_SpecialInfectedArrowEnabled ||
        m_SpecialInfectedArrowDebugLog ||
        m_SpecialInfectedIntentSenseEnabled ||
        m_DesktopIntentSenseHudWindowEnabled ||
        (m_SpecialInfectedBlindSpotDistance > 0.0f) ||
        (m_SpecialInfectedPreWarningDistance > 0.0f && m_SpecialInfectedPreWarningAutoAimEnabled) ||
        ((m_RearMirrorEnabled || m_DesktopRearMirrorWindowEnabled) && m_RearMirrorShowOnlyOnSpecialWarning
            && m_RearMirrorSpecialShowHoldSeconds > 0.0f && m_RearMirrorSpecialWarningDistance > 0.0f);
    if (!wantsEntityScan)
        return;

    auto* callback = GetOptionalSpecialInfectedCallbacks().scanSpecialInfectedEntities;
    if (callback)
        callback(this);
}

void VR::ApplyOptionalCreateMoveFeatures(CUserCmd* cmd, int stage, bool inputJumpHeld, bool hadWalkAxis, float walkNx, float walkNy, bool suppressScopeWalk)
{
    auto* callback = GetOptionalSpecialInfectedCallbacks().applyCreateMoveFeatures;
    if (callback)
        callback(this, cmd, stage, inputJumpHeld, hadWalkAxis, walkNx, walkNy, suppressScopeWalk);
}

void VR::ScanItemModelLabelEntitiesFromClientList()
{
    if (!m_ItemModelLabelEnabled)
        return;

    auto* callback = GetOptionalSpecialInfectedCallbacks().scanItemModelLabelEntities;
    if (callback)
        callback(this);
}

