#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_weapon_calibration.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_magazine_policy.h"
#include "../L4D2VR/vr_remote_magazines.h"
#include "vr_native_reload_config.inl"
#include "vr_native_reload_embedded_sample.inl"

static int checks = 0;
static void Require(bool result, const char* message)
{
    ++checks;
    if (!result) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(EXIT_FAILURE); }
}
static uint64_t nowMs = 1000u;
static uint64_t GetTickCount64() { return nowMs; }

struct C_WeaponCSBase
{
    enum WeaponID : int { NONE = 0, GRENADE_LAUNCHER = 21 };
    int id = 1, clip = 5;
};
struct C_BasePlayer
{
    C_WeaponCSBase* weapon = nullptr;
    uint32_t handle = 0x1005u;
    bool readable = true, handleMatches = true;
};
struct Engine
{
    bool inGame = true, paused = false;
    bool IsInGame() const { return inGame; }
    bool IsPaused() const { return paused; }
};
struct Surface
{
    bool visible = false;
    bool IsCursorVisible() const { return visible; }
};
struct Game { Engine* m_EngineClient = nullptr; Surface* m_VguiSurface = nullptr; };

// Script the existing safe native readers at their boundary. The production
// context getter and its input caller below are extracted unchanged.
static bool MagazineInteractionReadActiveWeapon(C_BasePlayer* player, C_WeaponCSBase*& weapon,
    C_WeaponCSBase::WeaponID& id, int& clip)
{
    weapon = nullptr; id = C_WeaponCSBase::NONE; clip = -1;
    if (!player || !player->readable || !player->weapon) return false;
    weapon = player->weapon; id = static_cast<C_WeaponCSBase::WeaponID>(weapon->id); clip = weapon->clip;
    return true;
}
static uint32_t MagazineInteractionActiveWeaponHandle(Game*, C_BasePlayer* player, C_WeaponCSBase*)
{ return player && player->handleMatches ? player->handle : 0u; }

struct VR
{
    bool m_IsVREnabled = true, m_MagazineInteractionEnabled = true, m_VrHandsEnabled = true;
    bool m_NativeViewmodelHandsOnly = false, m_MouseModeEnabled = false, m_SuppressPlayerInput = false;
    std::atomic<bool> m_NativeAmmoReloadFallbackActive{false}, m_FirstPersonControlReady{true};
    std::atomic<uint32_t> m_MagazineInteractionSessionGeneration{7u};
    l4d2vr_magazine::SessionTracker m_NativeAmmoReloadButtonSession;
    l4d2vr_magazine::NativeFallbackPulse m_NativeAmmoReloadFallbackPulse;
    Game* m_Game = nullptr;
    bool UpdateNativeAmmoReloadButtonContext(C_BasePlayer* localPlayer);
    bool Frame(C_BasePlayer* localPlayer, bool magazineReleaseJustPressed, bool isObserverOrIdle = false)
    {
#include "vr_native_reload_pulse.inl"
        return l4d2vr_magazine::WantsNativeReload(false, false, nativeAmmoReloadPulse);
    }
};
#include "vr_native_reload_context.inl"

struct Fixture
{
    Engine engine;
    Surface surface;
    Game game{&engine, &surface};
    C_WeaponCSBase weapon;
    C_BasePlayer owner{&weapon};
    VR vr;
    Fixture() { vr.m_Game = &game; nowMs = 1000u; }
};

static void TestNativeModes()
{
    for (const int id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 26, 32, 33, 34, 35, 36})
    {
        Fixture disabled;
        disabled.weapon.id = id; disabled.vr.m_MagazineInteractionEnabled = false;
        Require(disabled.vr.Frame(&disabled.owner, true), "Disabled manual reload retains the release button reload");
        nowMs = 1200u;
        Require(disabled.vr.Frame(&disabled.owner, false), "Native reload survives ordinary button release");
        nowMs = 1350u;
        Require(!disabled.vr.Frame(&disabled.owner, false), "Native reload stops at the existing 350 ms deadline");
        Fixture handsDisabled;
        handsDisabled.weapon.id = id; handsDisabled.vr.m_VrHandsEnabled = false;
        Require(handsDisabled.vr.Frame(&handsDisabled.owner, true), "Disabled VR hands retain a native release reload");
        Fixture unsupported;
        unsupported.weapon.id = id; unsupported.vr.m_NativeAmmoReloadFallbackActive = true;
        Require(unsupported.vr.Frame(&unsupported.owner, true), "Enabled unsupported hosts retain their native fallback");
        Fixture physical;
        physical.weapon.id = id;
        Require(!physical.vr.Frame(&physical.owner, true), "Supported physical mode does not start a stock reload");
    }
    for (const int id : {0, 12, 13, 14, 19, 20, 37, 64, -1})
    {
        Fixture unrelated;
        unrelated.weapon.id = id; unrelated.vr.m_MagazineInteractionEnabled = false;
        unrelated.vr.m_NativeAmmoReloadFallbackActive = true;
        Require(!unrelated.vr.Frame(&unrelated.owner, true), "Unrelated item, melee and M60 do not get reload pulses");
    }
}

static void TestIdentityAndAvailability()
{
    Fixture switched;
    switched.vr.m_VrHandsEnabled = false;
    Require(switched.vr.Frame(&switched.owner, true), "Start native reload with hands disabled");
    C_WeaponCSBase replacement = switched.weapon;
    switched.owner.weapon = &replacement; switched.owner.handle = 0x1006u; nowMs = 1100u;
    Require(!switched.vr.Frame(&switched.owner, false), "Same-type replacement cannot inherit the old pulse generation");
    Require(switched.vr.Frame(&switched.owner, true), "A later explicit press reloads the replacement");
    switched.owner.handle = 0x2006u;
    Require(!switched.vr.Frame(&switched.owner, false), "Same-address native serial replacement clears the pulse");
    Require(switched.vr.Frame(&switched.owner, true), "Explicit press can start the new serial");
    C_BasePlayer otherOwner = switched.owner;
    Require(!switched.vr.Frame(&otherOwner, false), "Owner replacement cannot inherit a native reload pulse");
    Require(switched.vr.Frame(&otherOwner, true), "New owner can explicitly reload");
    otherOwner.readable = false;
    Require(!switched.vr.Frame(&otherOwner, false), "Unreadable live weapon cancels the pending pulse");
    otherOwner.readable = true;
    Require(!switched.vr.Frame(&otherOwner, false), "Identity recovery alone does not start a reload");
    otherOwner.handleMatches = false;
    Require(!switched.vr.Frame(&otherOwner, true), "Unverified native handle cannot reload");
    otherOwner.handleMatches = true;
    for (const uint32_t invalid : {0u, 0xffffffffu, 0x1000u, 1u << 22})
    {
        otherOwner.handle = invalid;
        Require(!switched.vr.Frame(&otherOwner, true), "Unknown or malformed native identity rejects button reload");
    }
    otherOwner.handle = 0x3006u;
    Require(!switched.vr.Frame(&otherOwner, true, true), "Observer input cannot issue reload");
    switched.engine.inGame = false;
    Require(!switched.vr.Frame(&otherOwner, true), "Menu cannot issue a weapon reload");
    switched.engine.inGame = true; switched.engine.paused = true;
    Require(!switched.vr.Frame(&otherOwner, true), "Paused input cannot issue reload");
    switched.engine.paused = false; switched.surface.visible = true;
    Require(!switched.vr.Frame(&otherOwner, true), "Visible cursor prevents reload");
    switched.surface.visible = false; switched.vr.m_FirstPersonControlReady = false;
    Require(!switched.vr.Frame(&otherOwner, true), "Unavailable first-person control prevents reload");
    switched.vr.m_FirstPersonControlReady = true; switched.vr.m_MouseModeEnabled = true;
    Require(!switched.vr.Frame(&otherOwner, true), "Mouse mode does not use the VR release button");
    switched.vr.m_MouseModeEnabled = false; switched.vr.m_SuppressPlayerInput = true;
    Require(!switched.vr.Frame(&otherOwner, true), "Suppressed input prevents reload");
    switched.vr.m_SuppressPlayerInput = false;
    Require(!switched.vr.Frame(nullptr, true), "Missing owner cannot reload");

    Fixture modeChanged;
    modeChanged.vr.m_MagazineInteractionEnabled = false;
    Require(modeChanged.vr.Frame(&modeChanged.owner, true), "Disabled manual mode starts native reload");
    modeChanged.vr.m_MagazineInteractionEnabled = true;
    Require(!modeChanged.vr.Frame(&modeChanged.owner, false), "Re-enabled physical mode cancels native pulse");
    modeChanged.vr.m_NativeAmmoReloadFallbackActive = true;
    Require(!modeChanged.vr.Frame(&modeChanged.owner, false), "Unsupported-host recovery requires a fresh press");
    Require(modeChanged.vr.Frame(&modeChanged.owner, true), "Explicit native fallback press remains usable");
    modeChanged.vr.m_NativeAmmoReloadFallbackActive = false;
    Require(!modeChanged.vr.Frame(&modeChanged.owner, false), "Backend acquisition cancels the native pulse");
}

static void TestLegacySample()
{
    const std::string legacySample = "#ScopeEnabled=false\n#MagazineInteractionEnabled=true\n";
    std::string config = "ScopeEnabled=true\nScopePhysicalLensEnabled=false\nScopeBaseMagnification=4\n"
        "MagazineReleaseButtonRequired=true\nUnknownSetting=true\n";
    Require(CleanupNativeReloadConfig(config, legacySample), "Legacy cleanup removes an unrelated unknown setting");
    Require(config.find("ScopePhysicalLensEnabled=false") != std::string::npos, "Saved legacy scope fallback survives restart cleanup");
    Require(config.find("ScopeBaseMagnification=4") != std::string::npos, "Saved optical magnification survives restart cleanup");
    Require(config.find("MagazineReleaseButtonRequired=true") != std::string::npos, "Existing release setting remains protected");
    Require(config.find("UnknownSetting=") == std::string::npos, "Unrelated sample pruning behavior remains intact");
    Require(std::string(kCfgSampleConfigText2).find("ScopePhysicalLensEnabled=true\nScopeBaseMagnification=2") != std::string::npos,
        "Embedded reset defaults include the supported physical scope options");
}

int main()
{
    TestNativeModes();
    TestIdentityAndAvailability();
    TestLegacySample();
    std::printf("vr_native_reload: %d checks passed\n", checks);
}
