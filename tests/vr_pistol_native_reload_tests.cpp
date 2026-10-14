#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "../L4D2VR/vr_pistol_ammo.h"
#include "../L4D2VR/vr_pistol_reload.h"
#include "../L4D2VR/vr_pistol_ammo_sync.h"
#include "../L4D2VR/vr_server_pistol_commands.h"
#include "../L4D2VR/vr_magazine_policy.h"
#include "../L4D2VR/vr_native_scope.h"
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

static int checks = 0, nativeReloadCalls = 0, ammoWrites = 0, requestExecutions = 0;
static uint32_t originalProgress = 0u;
static bool nativeOutcome = false;
static uint64_t nowMs = 1000u;
static uint64_t FixtureTickCount() { return nowMs; }
static DWORD FixtureProcessId() { return 77u; }
#define GetTickCount64 FixtureTickCount
#define GetCurrentProcessId FixtureProcessId
static void Require(bool value, const char* message)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(EXIT_FAILURE); }
}

struct Entity;
struct edict_t
{
    Entity* entity = nullptr;
    int index = -1;
    unsigned serial = 0u, m_fStateFlags = 4u;
    bool live = true;
};
struct Entity
{
    std::array<unsigned char, 0x1a00> bytes{};
    edict_t edict;
    Entity* owner = nullptr;
    Entity* active = nullptr;
    int id = 1;
    Entity(int index, unsigned serial) : edict{this, index, serial}
    {
        *reinterpret_cast<edict_t**>(bytes.data() + 0x28) = &edict;
        *reinterpret_cast<int*>(bytes.data() + 0x238) = 2;
        SetClip(15);
    }
    void* Object() { return bytes.data(); }
    int Clip() const { return *reinterpret_cast<const int*>(bytes.data() + 0x1414); }
    void SetClip(int value) { *reinterpret_cast<int*>(bytes.data() + 0x1414) = value; }
    void SetHandle(unsigned value) { *reinterpret_cast<unsigned*>(bytes.data() + 0x19d4) = value; }
    void SetDual(bool dual) { bytes[0x17DD] = dual ? 1u : 0u; }
};
static std::vector<Entity*> entities;
static Entity* FindEntity(const void* object)
{
    for (auto* entity : entities) if (entity->bytes.data() == object) return entity;
    return nullptr;
}
static void* __fastcall NativeOwner(void* object, void*)
{ auto* entity = FindEntity(object); return entity && entity->owner ? entity->owner->Object() : nullptr; }
static void* __fastcall NativeActive(void* object, void*)
{ auto* entity = FindEntity(object); return entity && entity->active ? entity->active->Object() : nullptr; }
static int __fastcall NativeIndex(void* object, void*)
{ auto* entity = FindEntity(object); return entity ? entity->edict.index : -1; }
static int __fastcall NativeCapacity(void*, void*) { return 15; }
struct Server_WeaponCSBase
{
    int GetWeaponID() const { auto* entity = FindEntity(this); return entity ? entity->id : 0; }
};
struct C_WeaponCSBase { enum WeaponID { PISTOL = 1 }; };
struct CUserCmd { int command_number = 0, buttons = 0; };
namespace l4d2vr_calibration
{
    static bool IsFirearm(int id) { return id == 1 || id == 2; }
}
struct Binding { bool valid = true; uintptr_t address = 1u; };
struct Offsets
{
    Binding PhysicalShellActiveHandle, GetActiveWeapon, CBaseEntity_entindex, PhysicalGunOwner, PhysicalShellMaxClip;
    Offsets()
    {
        GetActiveWeapon.address = reinterpret_cast<uintptr_t>(&NativeActive);
        CBaseEntity_entindex.address = reinterpret_cast<uintptr_t>(&NativeIndex);
        PhysicalGunOwner.address = reinterpret_cast<uintptr_t>(&NativeOwner);
        PhysicalShellMaxClip.address = reinterpret_cast<uintptr_t>(&NativeCapacity);
    }
};
struct VR { bool m_DualPistolsIndependentHandsEnabled = true, m_MagazineInteractionEnabled = true; };
struct SourceCCommand
{
    std::vector<std::string> arguments;
    int ArgC() const { return static_cast<int>(arguments.size()); }
    const char* Arg(int index) const { return arguments[static_cast<size_t>(index)].c_str(); }
};
struct VRPoseRelayCommandView { const char* name = nullptr; };
static std::vector<std::string> sentCommands;
static bool VRPoseRelayReadCommand(const void* source, VRPoseRelayCommandView& view)
{
    if (!source) return false;
    const auto& command = *static_cast<const SourceCCommand*>(source);
    if (!command.ArgC()) return false;
    view.name = command.Arg(0); return true;
}
static bool VRPoseRelayReadEdictIdentity(edict_t* edict, int& index, unsigned& serial)
{
    if (!edict || !edict->live || !edict->entity) return false;
    index = edict->index; serial = edict->serial; return index > 0 && index < 65;
}
static void VRPoseRelaySendClientCommand(void*, edict_t*, const char* command) { sentCommands.emplace_back(command); }
struct Player { bool isUsingVR = true; l4d2vr_magazine::NativeReloadLedger physicalReloadLedger; };

struct Game
{
    static constexpr size_t kMaxPlayers = 65u;
    VR* m_VR = nullptr;
    Offsets* m_Offsets = nullptr;
    void* m_ServerPluginHelpers = reinterpret_cast<void*>(1u);
    std::array<l4d2vr_pistol::AmmoLedger, kMaxPlayers> m_PistolAmmo{};
    std::mutex m_PistolAmmoMutex, m_PistolAmmoServerMutex, m_ServerPistolCommandsMutex;
#include "pistol_native_server_client.inl"
    std::array<PistolAmmoServerClient, kMaxPlayers> m_PistolAmmoServerClients{};
    std::array<l4d2vr_server_pistol::Commands, kMaxPlayers> m_ServerPistolCommands{};
    std::array<l4d2vr_server_pistol::Reloads, kMaxPlayers> m_ServerReloadCommands{};
    std::array<Player, kMaxPlayers> m_PlayersVRInfo{};
    int reserve = 40;
    bool IsValidPlayerIndex(int index) const { return index > 0 && index < static_cast<int>(kMaxPlayers); }
    bool ReadServerEdictIdentity(edict_t* edict, int& index, unsigned& serial, const void* object) const
    {
        if (!edict || !edict->live || !edict->entity || edict->entity->bytes.data() != object) return false;
        index = edict->index; serial = edict->serial; return index > 0 && index <= 4095;
    }
    int GetConVarIntDirect(const char*, int) const { return 0; }
    static void logMsg(const char*, ...) {}
    void OfferPistolAmmoProtocol(edict_t* entity);
    bool PistolAmmoClientReady(int index, unsigned ownerSerial);
    bool HandlePistolAmmoCommand(edict_t* entity, const void* command);
    bool HandlePistolMagazineCommand(edict_t* entity, const void* command);
    bool RelinquishPistolPhysicalReload(int index, const l4d2vr_pistol::AmmoSnapshot& snapshot, int command);
    void ProcessPistolMagazineRequests(int index, int command, unsigned serial);
    void PublishPistolAmmoState(int, const l4d2vr_pistol::AmmoSnapshot&, int) {}
};
static bool NativeReloadOriginal(void*) { ++nativeReloadCalls; return true; }
static void NativePlayerOriginal(void* owner, CUserCmd*, void*);
struct Hooks
{
    static inline Game* m_Game = nullptr;
    static inline VR* m_VR = nullptr;
    struct Hook { bool isEnabled = true; bool (*fOriginal)(void*) = &NativeReloadOriginal; };
    struct PlayerHook { bool isEnabled = true; void (*fOriginal)(void*, CUserCmd*, void*) = &NativePlayerOriginal; };
    static PlayerHook hkPistolPlayerRunCommand;
    static Hook hkPistolGunFire, hkPhysicalGunReload;
    static bool __fastcall dPhysicalGunReload(void* ecx, void* edx);
    static void __fastcall dPistolPlayerRunCommand(void* owner, void*, CUserCmd* command, void* moveHelper);
};
Hooks::PlayerHook Hooks::hkPistolPlayerRunCommand{};
Hooks::Hook Hooks::hkPistolGunFire{};
Hooks::Hook Hooks::hkPhysicalGunReload{};
static bool ManualPistolLayoutIsVerified() { return true; }
static bool ManualEmptyHandsPlaceholderOwnerIsLiveSurvivor(void* object)
{
    auto* entity = FindEntity(object);
    return entity && entity->edict.live && entity->bytes[0xf0] == 0u &&
        *reinterpret_cast<const int*>(entity->bytes.data() + 0x238) == 2;
}
#include "pistol_native_prototypes.inl"
// The native hook's unused register placeholder is part of the existing ABI.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#endif
#include "pistol_native_reload_hook.inl"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "pistol_native_context.inl"
#include "pistol_native_functions.inl"

static void NativePlayerOriginal(void* owner, CUserCmd*, void*)
{
    auto* player = FindEntity(owner);
    originalProgress = Hooks::m_Game->m_PistolAmmoServerClients[player->edict.index].executedCommand;
    nativeOutcome = Hooks::dPhysicalGunReload(player->active->Object(), nullptr);
}

static ServerPistolExecution CaptureExecution(void* owner, CUserCmd* command)
{
    auto* m_Game = Hooks::m_Game;
    ServerPistolExecution context{};
    context.owner = owner;
    if (ReadServerPistolExecution(owner, command, context))
    {
#include "pistol_native_producer.inl"
    return context;
}

void Game::ProcessPistolMagazineRequests(int index, int command, unsigned ownerSerial)
{
    ++requestExecutions;
#include "pistol_native_process_prefix.inl"
    auto& client = m_PistolAmmoServerClients[index];
    if (expired || !client.sender.Ready()) return;
    auto* owner = entity->entity;
    auto* weapon = owner->active;
    l4d2vr_pistol::AmmoSnapshot ammo{reinterpret_cast<uintptr_t>(owner->Object()), reinterpret_cast<uintptr_t>(weapon->Object()),
        owner->edict.serial, weapon->edict.serial, weapon->Clip(), weapon->bytes[0x17DD] != 0u, 15};
    l4d2vr_pistol_reload::Snapshot snapshot{true, *reinterpret_cast<unsigned*>(owner->bytes.data() + 0x19d4),
        static_cast<uint32_t>(command), ammo, reserve, true};
    int nextClip = weapon->Clip(), nextReserve = reserve;
    const auto reply = client.reload.Apply(request, snapshot, nowMs, m_PistolAmmo[index], [&](int clip, int ammoReserve)
        { ++ammoWrites; nextClip = clip; nextReserve = ammoReserve; return true; });
    if (reply.status == l4d2vr_shell::Status::Applied) { weapon->SetClip(nextClip); reserve = nextReserve; }
}

struct Scene
{
    Entity owner{1, 2u}, weapon{5, 3u}, otherWeapon{6, 4u}, otherOwner{2, 5u};
    VR vr;
    Offsets offsets;
    Game game;
    unsigned sequence = 0u;
    l4d2vr_pistol_reload::Request lastRequest{};
    l4d2vr_pistol_reload::Reply lastReply{};
    Scene()
    {
        entities = {&owner, &weapon, &otherWeapon, &otherOwner};
        nowMs = 1000u; nativeReloadCalls = ammoWrites = requestExecutions = 0; sentCommands.clear();
        owner.active = &weapon; weapon.owner = otherWeapon.owner = &owner;
        owner.SetHandle(0x3005u);
        game.m_VR = &vr; game.m_Offsets = &offsets;
        Hooks::m_Game = &game; Hooks::m_VR = &vr; g_ServerPistolExecution = nullptr;
        auto& client = game.m_PistolAmmoServerClients[1];
        client.entity = &owner.edict; client.serial = 2u; client.sender.Reset(42u); client.reload.Reset(42u);
        Require(client.sender.Acknowledge(l4d2vr_pistol_sync::kVersion, 42u), "Establish the original physical session");
        Require(game.m_PistolAmmo[1].Single(Snapshot(), l4d2vr_pistol::Hand::Right), "Establish known single pistol hand");
    }
    l4d2vr_pistol::AmmoSnapshot Snapshot() const
    { return {reinterpret_cast<uintptr_t>(owner.bytes.data()), reinterpret_cast<uintptr_t>(weapon.bytes.data()),
        owner.edict.serial, weapon.edict.serial, weapon.Clip(), weapon.bytes[0x17DD] != 0u, 15}; }
    l4d2vr_pistol_reload::Reply Act(l4d2vr_pistol::MagazineAction action, l4d2vr_pistol::Hand hand = l4d2vr_pistol::Hand::Right)
    {
        nowMs += 100u;
        lastRequest = {42u, ++sequence, 0x3005u, 100u, weapon.Clip(), game.reserve, hand, action};
        l4d2vr_pistol_reload::Snapshot snapshot{true, 0x3005u, 100u, Snapshot(), game.reserve, true};
        int nextClip = weapon.Clip(), nextReserve = game.reserve;
        lastReply = game.m_PistolAmmoServerClients[1].reload.Apply(lastRequest, snapshot, nowMs, game.m_PistolAmmo[1],
            [&](int clip, int reserve) { ++ammoWrites; nextClip = clip; nextReserve = reserve; return true; });
        if (lastReply.status == l4d2vr_shell::Status::Applied) { weapon.SetClip(nextClip); game.reserve = nextReserve; }
        Require(lastReply.status == l4d2vr_shell::Status::Applied, "Physical action establishes ownership");
        return lastReply;
    }
    ServerPistolExecution Capture(l4d2vr_server_pistol::ReloadPolicy policy, bool reload, bool decoded = true, int commandNumber = 101)
    {
        l4d2vr_dual::Shot shot{}; shot.command = commandNumber; shot.capturedAtMs = nowMs;
        if (decoded) Require(game.m_ServerPistolCommands[1].Store(reinterpret_cast<uintptr_t>(owner.Object()), 2u, shot, policy),
            "Store exact executing command policy");
        CUserCmd command{commandNumber, reload ? 1 << 13 : 0};
        return CaptureExecution(owner.Object(), &command);
    }
    bool Reload(const ServerPistolExecution* context)
    {
        g_ServerPistolExecution = context;
        const bool result = Hooks::dPhysicalGunReload(weapon.Object(), nullptr);
        g_ServerPistolExecution = nullptr;
        return result;
    }
    l4d2vr_pistol::MagazineState Magazines()
    {
        l4d2vr_pistol::MagazineState result{};
        Require(game.m_PistolAmmo[1].MagazineInfo(Snapshot(), result), "Read current mechanical ownership");
        return result;
    }
};

static SourceCCommand MagazineCommand(const l4d2vr_pistol_reload::Request& request)
{
    return { {"l4d2vr_pistol_mag_v1", std::to_string(request.token), std::to_string(request.sequence),
        std::to_string(request.handle), std::to_string(request.command), std::to_string(request.clip), std::to_string(request.reserve),
        std::to_string(static_cast<unsigned>(request.hand)), std::to_string(static_cast<unsigned>(request.action))} };
}

static void TestNativeEscapes()
{
    for (const auto hand : {l4d2vr_pistol::Hand::Right, l4d2vr_pistol::Hand::Left})
    {
        for (const bool full : {false, true})
        {
            auto sceneStorage = std::make_unique<Scene>();
            auto& scene = *sceneStorage;
            scene.weapon.SetClip(0);
            Require(scene.game.m_PistolAmmo[1].Single(scene.Snapshot(), hand), "Select the held hand without adding rounds");
            scene.Act(l4d2vr_pistol::MagazineAction::Eject, hand);
            if (full) scene.Act(l4d2vr_pistol::MagazineAction::Insert, hand);
            auto& client = scene.game.m_PistolAmmoServerClients[1];
            client.pending = {42u, ++scene.sequence, 0x3005u, 102u, scene.weapon.Clip(), 40, hand, l4d2vr_pistol::MagazineAction::Cycle};
            client.havePending = true; client.queuedAt = nowMs; client.reloadSequence = scene.sequence - 1u;
            scene.vr.m_MagazineInteractionEnabled = false; // Manual Reload or VR Hands was disabled.
            const int clip = scene.weapon.Clip(), reserve = scene.game.reserve, writes = ammoWrites;
            auto context = scene.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true);
            Require(context.reload && context.reloadPolicyValid && context.reloadValid && !context.reloadBlocked,
                "Exact producer resolves explicit Native IN_RELOAD");
            Require(scene.Reload(&context) && nativeReloadCalls == 1, "Explicit native reload reaches the original server hook");
            Require(scene.weapon.Clip() == clip && scene.game.reserve == reserve && ammoWrites == writes,
                "Relinquishing mechanical ownership does not write native ammo");
            auto state = scene.Magazines();
            Require(state.physical == 0u && state.attached == 0u && state.chambered == 0u, "Current physical masks are cleared");
            int right = -1, left = -1;
            Require(scene.game.m_PistolAmmo[1].Counts(scene.Snapshot(), right, left) &&
                right == (hand == l4d2vr_pistol::Hand::Right ? clip : 0) && left == (hand == l4d2vr_pistol::Hand::Left ? clip : 0),
                "Known left or right hand partition survives native takeover");
            Require(!client.havePending && client.reloadSequence == 0u && client.sender.Token() != 42u && client.sender.Token() != 0u,
                "Old pending request and replay session are revoked before a new offer");
            Require(sentCommands.size() == 1u && sentCommands.back().find("l4d2vr_pistol_ammo_cap ") == 0u,
                "Existing protocol offers the replacement token");
            const unsigned token = client.sender.Token();
            Require(client.sender.Acknowledge(l4d2vr_pistol_sync::kVersion, token), "Acknowledge replacement session");
            const auto oldAck = SourceCCommand{{"l4d2vr_pistol_ammo_ack", std::to_string(l4d2vr_pistol_sync::kVersion), "42"}};
            Require(scene.game.HandlePistolAmmoCommand(&scene.owner.edict, &oldAck) && client.sender.Token() == token && client.sender.Ready(),
                "Late old acknowledgement cannot restore the old session");
            const auto oldDisconnect = SourceCCommand{{"l4d2vr_pistol_ammo_ack", "0", "42"}};
            Require(scene.game.HandlePistolAmmoCommand(&scene.owner.edict, &oldDisconnect) && client.sender.Token() == token && client.sender.Ready(),
                "Late old disconnect cannot revoke the rotated session");
            for (const auto action : {l4d2vr_pistol::MagazineAction::Eject, l4d2vr_pistol::MagazineAction::Insert,
                l4d2vr_pistol::MagazineAction::Reinsert, l4d2vr_pistol::MagazineAction::Cycle})
            {
                const l4d2vr_pistol_reload::Request late{42u, 99u, 0x3005u, 101u, clip, reserve, hand, action};
                const auto command = MagazineCommand(late);
                const int executions = requestExecutions;
                Require(scene.game.HandlePistolMagazineCommand(&scene.owner.edict, &command) && !client.havePending &&
                    requestExecutions == executions && ammoWrites == writes,
                    "Late same-clip physical request is rejected before transaction execution");
            }
            l4d2vr_pistol_reload::Snapshot snapshot{true, 0x3005u, 101u, scene.Snapshot(), reserve, true};
            const auto memo = client.reload.Apply(scene.lastRequest, snapshot, nowMs, scene.game.m_PistolAmmo[1],
                [&](int, int) { ++ammoWrites; return true; });
            Require(memo.status == l4d2vr_shell::Status::Session && ammoWrites == writes,
                "A duplicate old applied result cannot replay after session rotation");
            l4d2vr_pistol_reload::Client oldClient;
            oldClient.Offer(42u);
            l4d2vr_pistol_reload::Request clientRequest{};
            Require(oldClient.Begin(0x3005u, scene.lastRequest.clip, reserve, hand, scene.lastRequest.action, 100u,
                scene.Snapshot().owner, false, nowMs, clientRequest), "Create old pending client request");
            oldClient.Offer(token);
            Require(oldClient.Begin(0x3005u, clip, reserve, hand, l4d2vr_pistol::MagazineAction::Eject, 101u,
                scene.Snapshot().owner, false, nowMs, clientRequest), "Create replacement-session request");
            oldClient.Receive(scene.lastReply);
            l4d2vr_pistol_sync::State newState{token, 1u, 0x3005u, 101u, 15, clip,
                hand == l4d2vr_pistol::Hand::Right ? clip : 0, hand == l4d2vr_pistol::Hand::Left ? clip : 0, true, false};
            l4d2vr_pistol_reload::Reply ignored{};
            Require(oldClient.Update(scene.Snapshot().owner, 0x3005u, false, nowMs, newState, true, ignored) ==
                l4d2vr_pistol_reload::Client::Poll::Waiting,
                "Late old applied reply cannot settle the replacement-session request");
        }
    }
    auto dualStorage = std::make_unique<Scene>();
    auto& dual = *dualStorage;
    dual.weapon.SetDual(true); dual.weapon.SetClip(11);
    Require(dual.game.m_PistolAmmo[1].Joined(dual.Snapshot(), 4, 7, l4d2vr_pistol::Hand::Left), "Preserve an exact uneven dual partition");
    dual.Act(l4d2vr_pistol::MagazineAction::Eject);
    const int clip = dual.weapon.Clip(), writes = ammoWrites;
    auto context = dual.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true);
    Require(dual.Reload(&context), "Dual pistol native button transition relinquishes earlier physical ownership");
    int right = -1, left = -1;
    Require(dual.game.m_PistolAmmo[1].Counts(dual.Snapshot(), right, left) && right == 1 && left == 7 &&
        dual.weapon.Clip() == clip && dual.game.reserve == 40 && ammoWrites == writes,
        "Dual native takeover preserves rounds, reserve and the known uneven partition");
}

static void TestRetainedGates()
{
    for (const auto policy : {l4d2vr_server_pistol::ReloadPolicy::Block, l4d2vr_server_pistol::ReloadPolicy::Preserve})
    {
        auto sceneStorage = std::make_unique<Scene>();
        auto& scene = *sceneStorage;
        scene.Act(l4d2vr_pistol::MagazineAction::Eject);
        auto context = scene.Capture(policy, true);
        Require(!scene.Reload(&context) && nativeReloadCalls == 0, "Block and Preserve cannot relinquish physical ownership");
        Require(scene.Magazines().physical != 0u && scene.game.m_PistolAmmoServerClients[1].sender.Token() == 42u,
            "Blocked policies preserve masks and the original transaction token");
    }
    auto automaticStorage = std::make_unique<Scene>();
    auto& automatic = *automaticStorage;
    automatic.weapon.SetClip(0); automatic.game.m_PistolAmmo[1].Observe(automatic.Snapshot());
    automatic.Act(l4d2vr_pistol::MagazineAction::Eject);
    auto noButton = automatic.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, false);
    Require(!automatic.Reload(&noButton), "Automatic empty reload without IN_RELOAD remains blocked");
    Require(!automatic.Reload(nullptr), "Out-of-command native calls cannot relinquish ownership");
    auto unknown = automatic.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true, false, 102);
    Require(!unknown.reloadPolicyValid && !automatic.Reload(&unknown), "Missing decoded policy cannot become a verified Native escape");
    auto invalid = automatic.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true, true, 103);
    automatic.owner.SetHandle(0x4005u);
    Require(!automatic.Reload(&invalid), "Mismatched native handle serial cannot relinquish ownership");
    automatic.owner.SetHandle(0x3005u);
    const auto retained = automatic.Snapshot();
    ++automatic.weapon.edict.serial;
    automatic.Reload(&invalid);
    l4d2vr_pistol::MagazineState state{};
    Require(automatic.game.m_PistolAmmo[1].MagazineInfo(retained, state) && state.physical == 1u &&
        automatic.game.m_PistolAmmoServerClients[1].sender.Token() == 42u,
        "A recycled weapon cannot revoke the previous gun's physical session through a stale context");
}

static void TestLedgerScope()
{
    l4d2vr_pistol::AmmoLedger ledger;
    l4d2vr_pistol::AmmoSnapshot first{10u, 50u, 2u, 3u, 0, false}, second{10u, 60u, 2u, 4u, 0, false};
    l4d2vr_pistol::MagazineResult result{};
    for (auto snapshot : {first, second})
        Require(ledger.Magazine(snapshot, l4d2vr_pistol::Hand::Right, l4d2vr_pistol::MagazineAction::Eject, 40, true,
            [](int, int) { return true; }, result), "Establish independent physical pistol entries");
    auto wrong = first; ++wrong.ownerSerial;
    Require(!ledger.RelinquishPhysical(wrong), "Unknown owner serial cannot reset the ledger");
    wrong = first; ++wrong.weaponSerial;
    Require(!ledger.RelinquishPhysical(wrong), "Unknown weapon serial cannot create or reset an entry");
    wrong = first; wrong.clip = 1;
    Require(!ledger.RelinquishPhysical(wrong), "Unreplicated native ammo does not permit mechanical ownership takeover");
    Require(ledger.RelinquishPhysical(first), "Matching current pistol can relinquish mechanical ownership");
    l4d2vr_pistol::MagazineState state{};
    Require(ledger.MagazineInfo(second, state) && state.physical == 1u, "Other gun's physical masks remain intact");
    Require(ledger.MagazineInfo(first, state) && state.physical == 0u, "Only the selected gun changes ownership");
    Require(!ledger.RelinquishPhysical(first), "Repeated takeover does not mutate a new session");

    auto sceneStorage = std::make_unique<Scene>();
    auto& scene = *sceneStorage;
    scene.Act(l4d2vr_pistol::MagazineAction::Eject);
    scene.otherWeapon.SetClip(0);
    l4d2vr_pistol::AmmoSnapshot retainedGun{scene.Snapshot().owner, reinterpret_cast<uintptr_t>(scene.otherWeapon.Object()),
        2u, 4u, 0, false};
    Require(scene.game.m_PistolAmmo[1].Magazine(retainedGun, l4d2vr_pistol::Hand::Right, l4d2vr_pistol::MagazineAction::Eject,
        40, true, [](int, int) { return true; }, result), "Establish another retained gun in the same player's ledger");
    Entity peerWeapon{7, 6u}; peerWeapon.owner = &scene.otherOwner; peerWeapon.SetClip(0);
    scene.otherOwner.active = &peerWeapon; entities.push_back(&peerWeapon);
    auto& peer = scene.game.m_PistolAmmoServerClients[2];
    peer.entity = &scene.otherOwner.edict; peer.serial = 5u; peer.sender.Reset(99u); peer.reload.Reset(99u);
    Require(peer.sender.Acknowledge(l4d2vr_pistol_sync::kVersion, 99u), "Establish another player's session");
    peer.havePending = true; peer.pending.token = 99u; peer.reloadSequence = 8u;
    l4d2vr_pistol::AmmoSnapshot peerGun{reinterpret_cast<uintptr_t>(scene.otherOwner.Object()),
        reinterpret_cast<uintptr_t>(peerWeapon.Object()), 5u, 6u, 0, false};
    Require(scene.game.m_PistolAmmo[2].Magazine(peerGun, l4d2vr_pistol::Hand::Right, l4d2vr_pistol::MagazineAction::Eject,
        40, true, [](int, int) { return true; }, result), "Establish the peer's physical masks");
    auto context = scene.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true);
    Require(scene.Reload(&context), "Relinquish the current gun through the actual server hook");
    Require(scene.game.m_PistolAmmo[1].MagazineInfo(retainedGun, state) && state.physical == 1u && scene.otherWeapon.Clip() == 0,
        "Server takeover leaves another gun's mechanical state and ammo intact");
    Require(scene.game.m_PistolAmmo[2].MagazineInfo(peerGun, state) && state.physical == 1u && peerWeapon.Clip() == 0 &&
        peer.sender.Token() == 99u && peer.sender.Ready() && peer.havePending && peer.reloadSequence == 8u,
        "Server takeover leaves another player's ammo and transaction session intact");
}

static void TestReplayAndExecutionOrder()
{
    auto sceneStorage = std::make_unique<Scene>();
    auto& scene = *sceneStorage;
    scene.Act(l4d2vr_pistol::MagazineAction::Eject);
    auto& client = scene.game.m_PistolAmmoServerClients[1];
    client.executedCommand = 100u;
    scene.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true);
    CUserCmd command{101, 1 << 13};
    const int writes = ammoWrites;
    Hooks::dPistolPlayerRunCommand(scene.owner.Object(), nullptr, &command, nullptr);
    Require(nativeOutcome && originalProgress == 100u && client.executedCommand == 101u && ammoWrites == writes,
        "Actual PlayerRunCommand invokes the first live native hook before advancing request execution progress");
    unsigned token = client.sender.Token();
    Require(token != 42u && client.sender.Acknowledge(l4d2vr_pistol_sync::kVersion, token), "Acknowledge the rotated session");
    Require(client.executedCommand == 101u, "Token rotation retains simulation progress for this owner identity");
    nowMs += 100u;
    const l4d2vr_pistol_reload::Request reacquire{token, 1u, 0x3005u, 101u, scene.weapon.Clip(), scene.game.reserve,
        l4d2vr_pistol::Hand::Right, l4d2vr_pistol::MagazineAction::Eject};
    const auto acquireCommand = MagazineCommand(reacquire);
    Require(scene.game.HandlePistolMagazineCommand(&scene.owner.edict, &acquireCommand) && scene.Magazines().physical == 1u,
        "A new-token same-clip physical action can reacquire ownership through the real command handler");
    const int acquiredWrites = ammoWrites, reloads = nativeReloadCalls;
    Hooks::dPistolPlayerRunCommand(scene.owner.Object(), nullptr, &command, nullptr);
    Require(!nativeOutcome && nativeReloadCalls == reloads && ammoWrites == acquiredWrites &&
        client.sender.Token() == token && scene.Magazines().physical == 1u,
        "Replayed old Native IN_RELOAD cannot clear reacquired ownership or rotate its new session");
    scene.Capture(l4d2vr_server_pistol::ReloadPolicy::Native, true, true, 102);
    command.command_number = 102;
    Hooks::dPistolPlayerRunCommand(scene.owner.Object(), nullptr, &command, nullptr);
    Require(nativeOutcome && originalProgress == 101u && client.executedCommand == 102u &&
        scene.Magazines().physical == 0u && ammoWrites == acquiredWrites,
        "A fresh explicit command still relinquishes ownership in the same native call order");

    // Ownership replacement resets the old owner's progress. A new gun serial
    // receives a fresh identity check rather than inheriting a gun-specific latch.
    ++scene.owner.edict.serial;
    scene.game.OfferPistolAmmoProtocol(&scene.owner.edict);
    token = client.sender.Token();
    Require(client.executedCommand == 0u && client.sender.Acknowledge(l4d2vr_pistol_sync::kVersion, token),
        "Owner replacement resets the previous execution fence");
    auto snapshot = scene.Snapshot();
    l4d2vr_pistol::MagazineResult result{};
    Require(scene.game.m_PistolAmmo[1].Magazine(snapshot, l4d2vr_pistol::Hand::Right, l4d2vr_pistol::MagazineAction::Eject,
        40, true, [](int, int) { return true; }, result), "Replacement owner acquires physical ownership");
    l4d2vr_dual::Shot shot{}; shot.command = 1; shot.capturedAtMs = nowMs;
    Require(scene.game.m_ServerPistolCommands[1].Store(snapshot.owner, snapshot.ownerSerial, shot,
        l4d2vr_server_pistol::ReloadPolicy::Native), "Replacement owner supplies a fresh known command");
    command.command_number = 1;
    Hooks::dPistolPlayerRunCommand(scene.owner.Object(), nullptr, &command, nullptr);
    Require(nativeOutcome && originalProgress == 0u && client.executedCommand == 1u,
        "First live command for a replacement owner is accepted");
    token = client.sender.Token();
    Require(client.sender.Acknowledge(l4d2vr_pistol_sync::kVersion, token), "Acknowledge the replacement owner's rotated session");
    ++scene.weapon.edict.serial; scene.owner.SetHandle(0x4005u);
    snapshot = scene.Snapshot();
    Require(scene.game.m_PistolAmmo[1].Magazine(snapshot, l4d2vr_pistol::Hand::Right, l4d2vr_pistol::MagazineAction::Eject,
        40, true, [](int, int) { return true; }, result), "Replacement gun acquires its own physical entry");
    shot.command = 2;
    Require(scene.game.m_ServerPistolCommands[1].Store(snapshot.owner, snapshot.ownerSerial, shot,
        l4d2vr_server_pistol::ReloadPolicy::Native), "Replacement gun supplies a fresh known command");
    command.command_number = 2;
    Hooks::dPistolPlayerRunCommand(scene.owner.Object(), nullptr, &command, nullptr);
    Require(nativeOutcome && client.executedCommand == 2u, "Fresh replacement-gun command does not inherit a stale gun latch");
}

int main()
{
    TestNativeEscapes();
    TestRetainedGates();
    TestLedgerScope();
    TestReplayAndExecutionOrder();
    std::printf("vr_pistol_native_reload: %d checks passed\n", checks);
}
