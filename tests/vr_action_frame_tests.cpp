#include "../L4D2VR/vr_input_recovery.h"
#include "../L4D2VR/vr_hand_input_validation.h"
#include "../L4D2VR/vr_skeletal_data.h"
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_interaction_geometry.h"
#include "../L4D2VR/vr_magazine_policy.h"
#include "../L4D2VR/vr_pistol_detachment.h"
#include "../L4D2VR/vr_remote_shells.h"
#include "../L4D2VR/vr_remote_magazines.h"
#include "../L4D2VR/vr_pistol_reload.h"
#include "openvr.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "vr_action_frame_types.inl"

static int checks = 0;
static void Require(bool value, const char* message)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(EXIT_FAILURE); }
}
struct ScriptedSystem
{
    bool leftConnected = true, rightConnected = true;
    vr::ETrackedDeviceClass type = vr::TrackedDeviceClass_Controller;
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t device) const
    { return device == 1u ? leftConnected : device == 2u && rightConnected; }
    vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t) const { return type; }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole) const
    { return vr::k_unTrackedDeviceIndexInvalid; }
};
struct ScriptedInput
{
    vr::EVRInputError originError = vr::VRInputError_None;
    vr::VRInputValueHandle_t originSource = 100u;
    vr::TrackedDeviceIndex_t originDevice = 2u;
    bool zeroSource = false;
    vr::EVRInputError updateError = vr::VRInputError_None, readError = vr::VRInputError_None;
    std::unordered_map<vr::VRActionHandle_t, vr::InputDigitalActionData_t> digital;
    vr::InputAnalogActionData_t analog{};
    vr::InputSkeletalActionData_t skeleton{};
    vr::VRSkeletalSummaryData_t summary{};
    int reads = 0, updates = 0;
    std::function<void()> onRead;
    vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t*, uint32_t size, uint32_t count)
    {
        Require(size == sizeof(vr::VRActiveActionSet_t) && count == 2u, "action sets retain OpenVR ABI");
        ++updates; return updateError;
    }
    vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t action, vr::InputDigitalActionData_t* out,
        uint32_t, vr::VRInputValueHandle_t)
    { ++reads; *out = digital[action]; if (onRead) onRead(); return readError; }
    vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t, vr::InputAnalogActionData_t* out,
        uint32_t, vr::VRInputValueHandle_t)
    { ++reads; *out = analog; if (onRead) onRead(); return readError; }
    vr::EVRInputError GetInputSourceHandle(const char* path, vr::VRInputValueHandle_t* out)
    { *out = zeroSource ? 0u : std::strstr(path,"left") ? 101u : 100u; return vr::VRInputError_None; }
    vr::EVRInputError GetOriginTrackedDeviceInfo(vr::VRInputValueHandle_t, vr::InputOriginInfo_t* out, uint32_t)
    { out->devicePath=originSource; out->trackedDeviceIndex=originDevice; return originError; }
    vr::EVRInputError GetActionHandle(const char*, vr::VRActionHandle_t* out)
    { *out = 9u; return vr::VRInputError_None; }
    vr::EVRInputError GetSkeletalActionData(vr::VRActionHandle_t, vr::InputSkeletalActionData_t* out, uint32_t)
    { ++reads; *out = skeleton; return readError; }
};
static bool TryReadOpenVRSkeletalSummary(ScriptedInput* input, vr::VRActionHandle_t,
    vr::EVRSummaryType, vr::VRSkeletalSummaryData_t& out, const char*)
{ out = input->summary; if (input->onRead) input->onRead(); return true; }
#include "vr_action_frame_curls.inl"

struct C_WeaponCSBase
{
    enum class WeaponID { NONE, PISTOL };
    WeaponID GetWeaponID() const { return WeaponID::PISTOL; }
};
struct C_BasePlayer
{
    int team = 2;
    unsigned char life = 0;
    int observer = 0;
    C_WeaponCSBase weapon;
    void* GetActiveWeapon() { return &weapon; }
};
static bool SafeReadInt(const unsigned char* base, int offset, int& out)
{ std::memcpy(&out, base+offset, sizeof(out)); return true; }
static bool SafeReadU8(const unsigned char* base, int offset, unsigned char& out)
{ out=base[offset]; return true; }
struct Engine
{
    bool inGame = true;
    int playerIndex = 1;
    int GetLocalPlayer() const { return playerIndex; }
    bool IsInGame() const { return inGame; }
};
struct Game
{
    Engine engine;
    Engine* m_EngineClient = &engine;
    C_BasePlayer player;
    int entityLookups = 0;
    bool dualPistol = false;
    std::vector<std::string> commands;
    void* GetClientEntity(int index) { Require(index>0,"entity zero is never cast to player"); ++entityLookups; return &player; }
    bool IsDualPistolWeapon(C_WeaponCSBase*) const { return dualPistol; }
    void ClientCmd_Unrestricted(const char* command) { commands.emplace_back(command); }
    static void logMsg(const char*, ...) {}
};
enum class ObjectPullClientPhase { Idle, Active };
enum class MagazineInteractionManualState { WaitingForServerMagazine, HoldingFreshMagazine, WaitingForFreshMagazine };
struct VRWorldPoseTrackingSnapshot
{
    bool hmdValid = true, leftHandValid = true, rightHandValid = true;
    uint64_t capturedAtMs = 100u;
    Vector hmdPosition{0,0,0}, leftHandPosition{0,0,0}, rightHandPosition{0,0,0};
    QAngle hmdAngles{0,0,0}, leftHandAngles{0,0,0}, rightHandAngles{0,0,0};
};
static uint64_t GetTickCount64() { return 100u; }

struct VR
{
    static constexpr int kTeamNumOffset = offsetof(C_BasePlayer,team);
    static constexpr int kLifeStateOffset = offsetof(C_BasePlayer,life);
    static constexpr int kObserverModeOffset = offsetof(C_BasePlayer,observer);
    bool m_IsVREnabled = true, m_DualPistolsIndependentHandsEnabled = true, m_MouseModeEnabled = false;
    bool m_MagazineInteractionEnabled = true, m_VrHandsEnabled = true, m_NativeViewmodelHandsOnly = false;
    bool m_DualPistolsNativeReloadFallbackEnabled = false, m_PhysicalReloadObservedEmpty = false;
    bool m_MagazineInteractionSuppressEmptyClipAutoReload = false, m_ManualThrowEnabled = true;
    bool m_VrHandsTwoHandedGripActive = false;
    int m_VrHandsTwoHandedGripWeaponId = 0;
    uintptr_t m_VrHandsTwoHandedGripWeaponTag = 0;
    std::atomic<bool> m_LeftHandPistolActive{false}, m_DualPistolsActive{false};
    std::atomic<unsigned> m_PistolOwnershipMask{2u}, m_ServerPhysicalInteractionVersion{1u};
    l4d2vr_magazine::SessionTracker m_MagazineInteractionSession;
    std::atomic<unsigned> m_MagazineInteractionSessionGeneration{0u};
    bool m_MagazineInteractionLeftHandHolding = true, m_MagazineInteractionQuickReloadMode = false;
    bool m_MagazineInteractionRetainedMagazineHeld = false, m_MagazineInteractionTransactionGripHeld = false;
    MagazineInteractionManualState m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
    Vector m_RetainedMagazineCatchHandLocal{1,2,3};
    struct InsertGate { int resets = 0; void Reset() { ++resets; } } m_RetainedMagazineInsertGate;
    bool magazineProtocolSupported = true;
    int magazineAnchorReads = 0;
    std::atomic<unsigned> m_MagazineInteractionLeftHandPoseActive{0u};
    int shellWorldUpdates = 0;
    ScriptedInput input;
    ScriptedInput* m_Input = &input;
    ScriptedSystem system;
    ScriptedSystem* m_System = &system;
    std::atomic<uint32_t> m_MagazineGripInputState{3u};
    bool IsMagazineGripInputAvailable() const { return (m_MagazineGripInputState.load() & 1u) != 0u; }
    vr::TrackedDeviceIndex_t GetPhysicalControllerIndexForHand(bool left) const { return left ? 1u : 2u; }
    Game game;
    Game* m_Game = &game;
    l4d2vr_input::ActionFrame m_InputActionFrame;
    vr::EVRInputError m_LastInputActionStateError = vr::VRInputError_None;
    std::array<vr::VRActiveActionSet_t,2> m_ActiveActionSets{};
    bool m_InputActionHandlesReady = true;
    std::string m_InputActionManifestPath;
    std::chrono::steady_clock::time_point m_InputAcquisitionNextRetry{};
    int SetActionManifest(const char*) { return 0; }
    uint64_t m_InputActionStateFrameSerial = 0u;
    bool m_LeftHanded = false, m_LeftHandedSwapInputActions = false, m_VrHandsDebugLog = false;
    vr::VRActionHandle_t m_ActionWalk = 10u, m_ActionTurn = 11u, m_ActionTeleport = 1u, m_ActionWeaponGrip = 1u;
    std::unordered_map<vr::VRActionHandle_t,std::vector<LeftHandedMirroredActionSource>> m_LeftHandedDigitalActionSwapMap, m_LeftHandedAnalogActionSwapMap;
    std::unordered_set<vr::VRActionHandle_t> m_LeftHandedDigitalHandBoundActions, m_LeftHandedAnalogHandBoundActions;
    std::unordered_map<vr::VRActionHandle_t,LeftHandedDigitalActionFrameCache> m_LeftHandedDigitalActionFrameCache;
    std::unordered_map<vr::VRActionHandle_t,LeftHandedAnalogActionFrameCache> m_LeftHandedAnalogActionFrameCache;
    bool m_SuppressPlayerInput = false, m_PrimaryAttackCmdOwned = false, m_SecondaryAttackCmdOwned = false;
    bool m_JumpCmdOwned = false, m_UseCmdOwned = false, m_ReloadCmdOwned = false, m_DuckCmdOwned = false;
    bool m_ServerUseControllerAimActive = false, m_SpeechToTextCaptureActive = false;
    bool m_VoiceRecordCmdOwned = false, m_AutoVoiceRecordRequested = false, m_SpeechVoiceBroadcastActive = false;
    bool m_PrimaryAttackDown = false, m_MagazineInteractionThumbIndexCurlDownPrev = false, m_MagazineInteractionThreeFingerCurlDownPrev = false;
    bool m_GripReleaseDropEnabled = true, m_BodyGripInventoryEnabled = false, leftPistolPhysicalReload = false;
    std::chrono::steady_clock::time_point m_ServerUseControllerAimUntil{}, m_JumpGestureHoldUntil{}, m_ReloadGestureHoldUntil{}, m_SecondaryAttackGestureHoldUntil{};
    ObjectPullClientPhase m_ObjectPullPhase = ObjectPullClientPhase::Idle;
    int cancellations = 0, teleportCommits = 0, customReleases = 0, settlementPolls = 0, settlements = 0, freshGestures = 0;
    bool targeting = false, pendingMagazine = false, pendingShell = false, acknowledgement = false, chamberRequired = true;
    bool gripDown = false, gripPressed = false, curlPressed = false;
    bool IsInputActionFrameAvailable() const { return l4d2vr_input::ActionFrame::Ready(m_InputActionFrame.Read()); }
    bool ReadWorldPoseTrackingSnapshot(VRWorldPoseTrackingSnapshot&) const { return true; }
    void RefreshLeftHandedInputActionSwapMaps(bool) {}
    void UpdateInputActionState(const l4d2vr_input::ActionFrame::Attempt&);
    bool PressedDigitalAction(vr::VRActionHandle_t&, bool = false);
    bool GetLeftHandedMirroredDigitalActionData(vr::VRActionHandle_t, vr::InputDigitalActionData_t&);
    bool GetLeftHandedMirroredAnalogActionData(vr::VRActionHandle_t, vr::InputAnalogActionData_t&);
    bool GetDigitalActionData(vr::VRActionHandle_t&, vr::InputDigitalActionData_t&);
    bool GetAnalogActionData(vr::VRActionHandle_t&, vr::InputAnalogActionData_t&);
    bool GetWalkAxis(float&, float&);
    bool HasFreshMagazineInteractionTracking() const;
    bool Origin(vr::VRInputValueHandle_t origin, vr::ETrackedControllerRole role)
    {
#include "vr_action_frame_origin.inl"
        return originMatchesRole(origin,role);
    }
    l4d2vr_dual::Hand GetPrimaryPistolHand() const;
    void Frame(vr::EVRInputError error = vr::VRInputError_None)
    { input.updateError = error; UpdateInputActionState(m_InputActionFrame.Begin()); }
    void ReleaseCustomActionInputs() { ++customReleases; }
    void EndSpeechToTextCapture(bool) {}
    void StopVoiceRecordCommandNow() {}
    void ResetObjectPullInput(bool) {}
    void CancelMagazineInteractionManual() { ++cancellations; pendingMagazine = pendingShell = false; chamberRequired = false; }
    void ResetMagazineInteractionSession()
    {
#include "vr_action_frame_session_reset.inl"
    }
    void BeginTeleportTargeting() { targeting = true; }
    void EndTeleportTargeting(bool commit) { if (commit) ++teleportCommits; targeting = false; }
    void CancelTeleportTargeting() { targeting = false; }
    bool IsGameplayHandLeftPhysical(bool left) const { return left; }
    bool UpdateMagazineInteraction(C_BasePlayer* player, bool a, bool b, bool c, bool d, bool e)
    {
        Require(!a && !b && !c && !d && !e, "unavailable frame supplies neutral controls");
        C_BasePlayer* localPlayer = player;
        C_WeaponCSBase* activeWeapon = player ? &player->weapon : nullptr;
        const bool vrHandsInteractionAvailable = player != nullptr;
        const int activeWeaponIdInt = 1;
        const uint32_t activeWeaponHandle = 0x1001u;
        const bool authoritativeMagazine = false, authoritativeShell = false, authoritativePistol = true;
#include "vr_action_frame_session.inl"
        if (player)
        {
            Require(player == &game.player, "settlement retains the real owner");
            ++settlementPolls;
            if (acknowledgement)
            {
                settlements += int(pendingMagazine) + int(pendingShell);
                pendingMagazine = pendingShell = false;
            }
            if (HasFreshMagazineInteractionTracking()) ++freshGestures;
        }
        return false;
    }
    void Cleanup()
    {
#include "vr_action_frame_ownership.inl"
#include "vr_action_frame_cleanup.inl"
    }
    void updateFreshDetachedMagazineFromLeftHand() { ++shellWorldUpdates; }
    void ShellPending(bool leftGripDown)
    {
#include "vr_action_frame_shell_hold.inl"
    }
    bool RemoteMagazineProtocolSupported() const { return magazineProtocolSupported; }
    Vector leftControllerPosRelativeToWeapon() { ++magazineAnchorReads; return {4,5,6}; }
    void CaptureMagazineTransaction()
    {
#include "vr_action_frame_magazine_snapshot.inl"
    }
    void MagazinePending(bool leftGripDown)
    {
#include "vr_action_frame_magazine_hold.inl"
    }
    bool MagazineResult(const l4d2vr_remote_mag::Reply& reply,
        l4d2vr_remote_mag::ClientRequest::Poll poll, bool authoritativePistol, bool leftGripDown)
    {
#include "vr_action_frame_magazine_hold.inl"
#include "vr_action_frame_magazine_reject.inl"
        return true;
    }
    void Teleport()
    {
#include "vr_action_frame_teleport.inl"
    }
    void Grip()
    {
        uint32_t weaponGripInputState = 0u;
#include "vr_action_frame_grip.inl"
        gripDown = rightGripDown; gripPressed = rightGripJustPressed;
        (void)rightGripValid; (void)weaponGripInputState;
    }
    void CurlEdges(bool held, bool available = true)
    {
        const bool magazineButtonGripInput = false, magazineButtonGripDown = false, magazineButtonGripJustPressed = false;
        const bool magazineGripInputAvailable = available;
        const auto previous=m_MagazineGripInputState.load();
        const bool magazineGripRecovered=(previous & 1u)==0u && l4d2vr_input::GripSample::Generation(previous)!=0u;
        uint32_t magazineGripInputState = 0u;
        const bool thumbIndexGripDown = held, threeFingerGripDown = held;
        const bool thumbIndexWasDown = m_MagazineInteractionThumbIndexCurlDownPrev;
        const bool threeFingerWasDown = m_MagazineInteractionThreeFingerCurlDownPrev;
#include "vr_action_frame_curl_edges.inl"
        curlPressed = magazineGripJustPressed || supportHandJustPressed;
        (void)magazineGripDown; (void)supportHandDown;
    }
};
struct Hooks { static void RetryCreateMoveHook() {} };
#include "vr_action_frame_functions.inl"
#include "vr_action_frame_tracking.inl"
#include "vr_action_frame_primary.inl"

struct ThrowFixture
{
    VR vr;
    l4d2vr_input::ReleaseInputGate s_manualThrowInputGate;
    uintptr_t s_manualCarryThrowWeapon = 0;
    bool s_manualCarryThrowPhysicalAttackDown = false, s_manualInventoryThrowPhysicalUseDown = false;
    bool s_manualInventoryThrowArmed = false;
    struct Command { int buttons=0,command_number=1; unsigned char impulse=0; } command;
    void Step(bool carriedProp, bool attack, bool use)
    {
        vr.m_PrimaryAttackDown=attack; vr.m_UseCmdOwned=use;
        VR* m_VR=&vr;
        Command* cmd=&command;
        const bool s_ServerUnderstandsVR=true, throwBackendReady=true, emptyHandsPlaceholderActive=false;
        const bool localUseButtonDownForAutoActions=use, gripDropRequested=false;
        constexpr uint32_t kManualCarryThrowIN_ATTACK=1u, kManualInventoryThrowIN_USE=1u<<5;
        constexpr uint32_t kManualCarryThrowWeaponShift=26u, kManualCarryThrowWeaponMask=0x3fu<<26;
        constexpr int carryWeaponId=3, activeWeaponId=4;
        constexpr uintptr_t activeWeaponTag=100u;
        const char* activeWeaponName="fixture"; const char* activeWeaponNetClass="fixture";
        const auto pistolDropHand=l4d2vr_dual::Hand::None;
        auto pistolInteractionHand=l4d2vr_dual::Hand::None;
#include "vr_action_frame_throw_gate.inl"
#include "vr_action_frame_throw_release.inl"
        (void)pistolInteractionHand;
    }
    bool Emitted() const { return (static_cast<uint32_t>(command.buttons) & (0x3fu<<26)) != 0u || command.impulse != 0u; }
};

static void ThrowFailureTests(bool carried)
{
    ThrowFixture f; f.vr.Frame();
    f.Step(carried,false,!carried);
    f.Step(carried,true,!carried);
    Require(carried || f.s_manualInventoryThrowArmed,"inventory arms on genuine use-then-trigger press");
    f.vr.Frame(vr::VRInputError_IPCError);
    // CreateMove misses the failed and edge-suppressed recovery Presents.
    f.vr.Frame(); f.vr.Frame();
    f.Step(carried,false,false);
    Require(!f.Emitted() && !f.s_manualInventoryThrowArmed,"missed failure cancels the old release state");
    f.Step(carried,true,!carried);
    Require(!f.Emitted() && !f.s_manualInventoryThrowArmed,"held recovery cannot rearm throw");
    f.Step(carried,false,false);
    Require(!f.Emitted(),"neutral recovery is only a baseline");
    f.Step(carried,false,!carried); f.Step(carried,true,!carried); f.Step(carried,false,!carried);
    Require(f.Emitted(),"fresh deliberate press and release still emits");

    ThrowFixture direct; direct.vr.Frame(); direct.Step(carried,false,!carried); direct.Step(carried,true,!carried);
    direct.vr.Frame(vr::VRInputError_IPCError);
    direct.command.buttons=static_cast<int>(0x3fu<<26); direct.command.impulse=l4d2vr_grip::kReleaseImpulse;
    direct.Step(carried,false,false);
    Require(!direct.Emitted(),"unavailable frame suppresses encoded throw/drop output");
    direct.vr.Frame(); direct.Step(carried,true,!carried);
    Require(!direct.Emitted() && !direct.s_manualInventoryThrowArmed,"first recovered held sample stays disarmed");
}

static vr::InputDigitalActionData_t Digital(bool active, bool held, bool changed)
{ vr::InputDigitalActionData_t data{}; data.bActive=active; data.bState=held; data.bChanged=changed; data.activeOrigin=100u; return data; }

static void GripContributorTests()
{
    l4d2vr_input::GripContributors support, independent;
    std::atomic<uint32_t> published{0u};
    auto sample = [&](uint32_t known, uint32_t down)
    {
        const bool available = support.Update(known, down, 31u);
        l4d2vr_input::GripSample::Publish(published,
            available ? (1u | (support.Down() ? 2u : 0u)) : 0u);
        return available;
    };
    Require(sample(1u,1u) && support.Down(),"off-hand grip becomes the held contributor");
    Require(!sample(2u,0u) && support.Down() && (published.load() & 3u)==0u,
        "unrelated neutral reload cannot certify unavailable held grip release");
    const auto lossGeneration=l4d2vr_input::GripSample::Generation(published.load());
    Require(lossGeneration==1u,"aggregate held-source loss publishes persistent discontinuity");
    Require(independent.Update(32u,32u,32u) && independent.Down(),
        "unavailable support source leaves independent right grip usable");
    Require(!sample(2u,2u) && support.Down(),"alternate held source cannot erase unknown original contributor");
    Require(sample(3u,2u) && support.Down(),"valid original release transfers hold to known alternate source");
    Require((published.load() & 3u)==3u && l4d2vr_input::GripSample::Generation(published.load())==lossGeneration,
        "source transfer restores held availability without synthetic aggregate release");
    Require(sample(2u,0u) && !support.Down(),"known remaining contributor release produces trusted neutral");
    Require(sample(2u,0u),"optional sources that never held do not block neutral input");
    Require(sample(1u,1u) && !sample(2u,0u) && sample(3u,1u) && support.Down(),
        "healthy held-source recovery preserves hold");
    Require(sample(1u,0u) && !support.Down(),"healthy source release clears its remembered ownership");
    Require(independent.Update(32u,0u,32u) && !independent.Down(),"independent hand releases normally");
}

static void MagazineRejectionTests()
{
    using Action = l4d2vr_remote_mag::Action;
    using Status = l4d2vr_remote_mag::Status;
    using Poll = l4d2vr_remote_mag::ClientRequest::Poll;
    for (const bool pistol : {false,true})
    for (const Action action : {Action::Insert,Action::Reinsert})
    for (const Status status : {Status::Stale,Status::RateLimited,Status::NoSpaceOrAmmo})
    for (unsigned releaseCase=0u; releaseCase<3u; ++releaseCase)
    {
        VR f; f.Frame(); f.Cleanup();
        f.m_MagazineInteractionRetainedMagazineHeld=action==Action::Reinsert;
        f.m_MagazineInteractionLeftHandHolding=true;
        const auto owner=reinterpret_cast<uintptr_t>(&f.game.player);
        l4d2vr_remote_mag::ClientRequest magazineClient;
        l4d2vr_pistol_reload::Client pistolClient;
        l4d2vr_remote_mag::Request magazineRequest{};
        l4d2vr_pistol_reload::Request pistolRequest{};
        if (pistol)
        {
            Require(f.GetPrimaryPistolHand()==l4d2vr_dual::Hand::Left,"single-pistol rejection keeps input-independent left ownership");
            pistolClient.Offer(9u);
            Require(pistolClient.Begin(0x1001u,1,7,l4d2vr_pistol_reload::Hand::Left,
                static_cast<l4d2vr_pistol_reload::Action>(action),200u,owner,false,1000u,pistolRequest),
                "left-held single pistol submits real Insert/Reinsert request");
        }
        else
        {
            magazineClient.Offer(l4d2vr_remote_mag::kVersion,9u);
            Require(magazineClient.Begin(0x1001u,2,1,7,action,200u,owner,1u,1000u,magazineRequest),
                "detachable magazine submits real Insert/Reinsert request");
        }
        f.CaptureMagazineTransaction();
        Require(f.m_MagazineInteractionTransactionGripHeld && !f.m_MagazineInteractionLeftHandHolding &&
            f.m_MagazineInteractionState==MagazineInteractionManualState::WaitingForServerMagazine,
            "actual transaction snapshot preserves held ownership before visual hold clears");
        if (releaseCase!=0u)
        {
            f.MagazinePending(false);
            Require(!f.m_MagazineInteractionTransactionGripHeld,"known pending grip release clears transaction ownership");
            if (releaseCase==2u) f.MagazinePending(true);
            Require(!f.m_MagazineInteractionTransactionGripHeld,"regrip cannot resurrect released pending magazine");
        }
        l4d2vr_input::GripSample::Publish(f.m_MagazineGripInputState,0u);
        f.MagazinePending(false);
        l4d2vr_remote_mag::Reply selected{};
        Poll poll=Poll::None;
        if (pistol)
        {
            pistolClient.Receive({pistolRequest,status,{}});
            l4d2vr_pistol_reload::Reply reply{};
            const l4d2vr_pistol_sync::State noReplica{};
            Require(pistolClient.Update(owner,0x1001u,false,1001u,noReplica,false,reply)==
                l4d2vr_pistol_reload::Client::Poll::Result,"left-pistol rejected reply settles while grip and replica are unavailable");
            Require(l4d2vr_pistol_reload::SettleGesture(reply,l4d2vr_pistol_reload::Hand::Left)==
                l4d2vr_pistol_reload::GestureOutcome::RetryMagazine,"retryable left-pistol result reaches magazine rejection path");
            poll=Poll::Result;
            selected.status=reply.status;
            selected.request.action=static_cast<Action>(reply.request.action);
            selected.clip=reply.result.clip; selected.reserve=reply.result.reserve;
        }
        else
        {
            magazineClient.Receive({magazineRequest,status,1,7});
            poll=magazineClient.Update(0x1001u,2,owner,1u,1001u,selected,-1,-1);
            Require(poll==Poll::Result,"rejected magazine reply settles while grip and replica are unavailable");
        }
        Require(!f.MagazineResult(selected,poll,pistol,false),"actual rejected-result branch completes without fresh gesture");
        const bool retained=releaseCase==0u;
        Require(f.m_MagazineInteractionLeftHandHolding==retained &&
            f.m_MagazineInteractionState==(retained ? MagazineInteractionManualState::HoldingFreshMagazine :
                MagazineInteractionManualState::WaitingForFreshMagazine),
            "retryable rejection retains unknown held ownership but respects prior known release");
        Require(f.magazineAnchorReads==0 && f.m_RetainedMagazineCatchHandLocal.x==1.0f,
            "unknown rejected grip never updates hand-world anchor");
        Require(f.m_RetainedMagazineInsertGate.resets==int(retained),"retry grip gate resets only for retained hold");
    }

    for (const Action action : {Action::Insert,Action::Reinsert})
    {
        VR f; f.Frame(); f.m_MagazineInteractionRetainedMagazineHeld=action==Action::Reinsert;
        f.CaptureMagazineTransaction();
        const l4d2vr_remote_mag::Reply reply{{9u,1u,0x1001u,200u,2,1,7,action},Status::Stale,1,7};
        Require(!f.MagazineResult(reply,Poll::Result,false,true) && f.m_MagazineInteractionLeftHandHolding &&
            f.magazineAnchorReads==1,"known held rejection resumes normal hand anchor updates");
    }
    for (const Status status : {Status::Weapon,Status::Backend,Status::Unsupported,Status::Session})
    {
        VR f; f.Frame(); f.m_MagazineInteractionRetainedMagazineHeld=true; f.CaptureMagazineTransaction();
        l4d2vr_input::GripSample::Publish(f.m_MagazineGripInputState,0u);
        const l4d2vr_remote_mag::Reply reply{{9u,1u,0x1001u,200u,2,1,7,Action::Reinsert},status,1,7};
        f.MagazineResult(reply,Poll::Result,false,false);
        Require(!f.m_MagazineInteractionLeftHandHolding && !f.m_MagazineInteractionRetainedMagazineHeld,
            "unknown grip does not retain magazine on hard rejection");
    }
    VR eject; eject.Frame(); eject.CaptureMagazineTransaction();
    const l4d2vr_remote_mag::Reply ejectReply{{9u,1u,0x1001u,200u,2,1,7,Action::Eject},Status::Stale,1,7};
    eject.MagazineResult(ejectReply,Poll::Result,false,true);
    Require(eject.cancellations==1,"rejected Eject retains explicit cancellation semantics");
    VR timeout; timeout.Frame(); timeout.CaptureMagazineTransaction();
    timeout.MagazineResult(ejectReply,Poll::Timeout,false,false);
    Require(timeout.cancellations==1,"timed out pending transaction still cancels");
    VR backend; backend.Frame(); backend.CaptureMagazineTransaction(); backend.magazineProtocolSupported=false;
    backend.MagazineResult(ejectReply,Poll::Result,false,false);
    Require(backend.cancellations==1,"unsupported magazine backend still cancels");
    VR applied; applied.Frame(); applied.CaptureMagazineTransaction();
    l4d2vr_input::GripSample::Publish(applied.m_MagazineGripInputState,0u);
    const l4d2vr_remote_mag::Reply appliedReply{{9u,1u,0x1001u,200u,2,1,7,Action::Insert},Status::Applied,2,6};
    Require(applied.MagazineResult(appliedReply,Poll::Result,false,false) &&
        applied.m_MagazineInteractionState==MagazineInteractionManualState::WaitingForServerMagazine &&
        !applied.m_MagazineInteractionLeftHandHolding,"Applied reply bypasses rejected-result retention path");
}

int main()
{
    GripContributorTests();
    MagazineRejectionTests();
    VR vr;
    vr.input.digital[1] = Digital(true,true,true);
    vr.input.analog.bActive = true; vr.input.analog.x = 0.8f;
    vr.Frame();
    vr.Cleanup();
    vr.UpdateMagazineInteraction(&vr.game.player,false,false,false,false,false);
    vr.cancellations=vr.settlementPolls=vr.freshGestures=0; vr.chamberRequired=true;
    const auto sessionGeneration=vr.m_MagazineInteractionSessionGeneration.load();
    Require(vr.GetPrimaryPistolHand()==l4d2vr_dual::Hand::Left,"single-pistol session starts with physical left ownership");
    float x=0, y=0;
    Require(vr.GetWalkAxis(x,y) && x == 0.8f, "successful action frame provides movement");
    Require(vr.PressedDigitalAction(vr.m_ActionTeleport,true), "healthy fresh press remains available");
    vr.targeting = true;
    vr.Frame(vr::VRInputError_IPCError);
    const int reads = vr.input.reads;
    Require(!vr.GetWalkAxis(x,y) && x == 0.0f, "failed update cannot reuse stale movement");
    Require(!vr.PressedDigitalAction(vr.m_ActionTeleport,true), "failed update cannot replay button edge");
    vr.Grip(); Require(!vr.gripDown, "direct grip read is gated");
    Require(vr.input.reads == reads, "failed frames never query stale action data");
    vr.pendingMagazine = vr.pendingShell = true;
    vr.m_PrimaryAttackCmdOwned = vr.m_SecondaryAttackCmdOwned = vr.m_JumpCmdOwned = true;
    vr.m_UseCmdOwned = vr.m_ReloadCmdOwned = vr.m_DuckCmdOwned = true;
    vr.Cleanup();
    Require(vr.GetPrimaryPistolHand()==l4d2vr_dual::Hand::Left &&
        vr.m_MagazineInteractionSessionGeneration.load()==sessionGeneration,"failed-frame neutral poll keeps real session and generation");
    Require(vr.game.commands.size()==6u && !vr.targeting, "failed frame releases owned controls and cancels targeting");
    Require(vr.pendingMagazine && vr.pendingShell && vr.chamberRequired && vr.cancellations==0, "transient failure preserves submitted ammo and chamber state");
    Require(vr.settlementPolls==1 && vr.freshGestures==0, "acknowledgements poll while gestures pause");
    vr.acknowledgement = true; vr.Cleanup();
    Require(vr.settlements==2 && vr.chamberRequired && vr.game.commands.size()==6u, "magazine and shell settlement remain possible during failure");
    vr.Frame();
    Require(vr.PressedDigitalAction(vr.m_ActionTeleport) && !vr.PressedDigitalAction(vr.m_ActionTeleport,true), "recovery restores held state without one-shot replay");
    vr.Grip(); Require(vr.gripDown && !vr.gripPressed, "grip recovery does not replay pressed edge");
    vr.CurlEdges(true); Require(!vr.curlPressed, "recovered curl baseline does not manufacture gesture edge");
    vr.Teleport(); Require(vr.teleportCommits==0 && !vr.targeting, "recovery does not replay teleport press");
    vr.Frame(); vr.CurlEdges(true); Require(!vr.curlPressed, "held curl remains baselined on following frame");
    vr.input.digital[1] = Digital(false,true,true);
    Require(!vr.PressedDigitalAction(vr.m_ActionTeleport), "inactive direct payload cannot press buttons");
    vr.targeting = true; vr.Teleport();
    Require(!vr.targeting && vr.teleportCommits==0, "inactive teleport cancels without commit");

    VR mirror; mirror.m_LeftHanded = mirror.m_LeftHandedSwapInputActions = true;
    mirror.m_LeftHandedDigitalHandBoundActions.insert(1);
    mirror.m_LeftHandedDigitalActionSwapMap[1] = {{2,100},{3,101}};
    mirror.input.digital[2] = Digital(false,true,true);
    mirror.input.digital[3] = Digital(true,false,false);
    mirror.Frame();
    vr::InputDigitalActionData_t output{};
    Require(mirror.GetDigitalActionData(mirror.m_ActionTeleport,output) && output.bActive && !output.bState && !output.bChanged, "inactive mirrored press cannot contaminate active neutral source");
    mirror.input.readError = vr::VRInputError_InvalidDevice; mirror.Frame();
    Require(!mirror.GetDigitalActionData(mirror.m_ActionTeleport,output) && !output.bActive && !output.bChanged, "all mirrored errors stay failed without release event");
    const int failedReads = mirror.input.reads;
    Require(!mirror.GetDigitalActionData(mirror.m_ActionTeleport,output) && mirror.input.reads == failedReads, "cached mirrored failure retains failure status");
    mirror.input.readError = vr::VRInputError_None; mirror.input.digital[3] = Digital(true,true,true); mirror.Frame();
    Require(mirror.GetDigitalActionData(mirror.m_ActionTeleport,output) && output.bState && !output.bChanged, "source recovery exposes held state without synthetic edge");
    mirror.m_LeftHandedDigitalActionSwapMap[1].clear(); mirror.Frame();
    Require(!mirror.GetDigitalActionData(mirror.m_ActionTeleport,output) && !output.bActive, "empty mirrored source list cannot fabricate success");

    VR concurrent; concurrent.Frame(); concurrent.input.digital[1]=Digital(true,true,true);
    concurrent.input.onRead = [&] { concurrent.m_InputActionFrame.Begin(); };
    Require(!concurrent.GetDigitalActionData(concurrent.m_ActionTeleport,output) && !output.bState, "frame changes during query reject old payload");
    ScriptedInput skeleton; ScriptedSystem skeletonSystem; l4d2vr_input::ActionFrame frames;
    vr::VRActionHandle_t action=9u; std::array<float,5> curls{};
    skeleton.skeleton.bActive=true; skeleton.skeleton.activeOrigin=100u;
    skeleton.originSource=101u; skeleton.originDevice=1u;
    for(float& curl : skeleton.summary.flFingerCurl) curl=0.9f;
    Require(!ReadOpenVRSkeletalFingerCurls(&skeleton,&skeletonSystem,frames,1u,"/user/hand/left",action,"/actions/base/in/skeleton_lefthand",curls,"test") && skeleton.reads==0, "unavailable skeletal frame never reads curls");
    frames.Complete(frames.Begin(),true);
    Require(ReadOpenVRSkeletalFingerCurls(&skeleton,&skeletonSystem,frames,1u,"/user/hand/left",action,"/actions/base/in/skeleton_lefthand",curls,"test") && curls[0]==0.9f, "successful skeletal frame supplies held curls");
    skeleton.summary.flFingerCurl[0]=std::numeric_limits<float>::quiet_NaN();
    Require(!ReadOpenVRSkeletalFingerCurls(&skeleton,&skeletonSystem,frames,1u,"/user/hand/left",action,"/actions/base/in/skeleton_lefthand",curls,"test") && curls[0]==0.0f,"successful NaN curl is unavailable");
    skeleton.summary.flFingerCurl[0]=std::numeric_limits<float>::infinity();
    Require(!ReadOpenVRSkeletalFingerCurls(&skeleton,&skeletonSystem,frames,1u,"/user/hand/left",action,"/actions/base/in/skeleton_lefthand",curls,"test"),"infinite curl cannot fabricate full grip");
    skeleton.summary.flFingerCurl[0]=1.01f;
    Require(!ReadOpenVRSkeletalFingerCurls(&skeleton,&skeletonSystem,frames,1u,"/user/hand/left",action,"/actions/base/in/skeleton_lefthand",curls,"test"),"out-of-range curl is unavailable");
    skeleton.summary.flFingerCurl[0]=0.9f;
    skeleton.onRead = [&] { frames.Begin(); };
    Require(!ReadOpenVRSkeletalFingerCurls(&skeleton,&skeletonSystem,frames,1u,"/user/hand/left",action,"/actions/base/in/skeleton_lefthand",curls,"test") && curls[0]==0.0f, "changed skeletal frame neutralizes curls");
    VR direct; direct.Frame(); vr::InputAnalogActionData_t axes{};
    direct.input.analog.bActive=false; direct.input.analog.x=0.9f; direct.input.analog.y=std::numeric_limits<float>::infinity();
    Require(direct.GetAnalogActionData(direct.m_ActionWalk,axes) && !axes.bActive && axes.x==0.0f && axes.y==0.0f,"inactive direct analog values are neutralized");
    direct.input.analog.bActive=true;
    Require(!direct.GetAnalogActionData(direct.m_ActionWalk,axes) && axes.x==0.0f && !axes.bActive,"nonfinite active direct analog values fail neutral");
    direct.input.analog.y=0.25f;
    Require(direct.GetAnalogActionData(direct.m_ActionWalk,axes) && axes.bActive && axes.x==0.9f,"valid direct analog values remain available");
    direct.input.analog.x=std::numeric_limits<float>::max();
    Require(!direct.GetAnalogActionData(direct.m_ActionWalk,axes) && !axes.bActive,"finite oversized stick cannot reach movement/turn normalization");

    VR origins; origins.Frame(); origins.input.digital[1]=Digital(true,true,true);
    Require(origins.Origin(100u,vr::TrackedControllerRole_RightHand),"resolved right origin works with unassigned legacy roles");
    Require(!origins.Origin(100u,vr::TrackedControllerRole_LeftHand),"opposite-hand origin is rejected");
    origins.Grip(); Require(origins.gripDown,"known right grip is usable");
    origins.input.zeroSource=true; origins.Grip();
    Require(!origins.gripDown,"successful zero source handle never becomes unrestricted grip");
    origins.input.zeroSource=false; origins.input.digital[1].activeOrigin=0u; origins.Grip();
    Require(!origins.gripDown,"missing active origin is unavailable grip");
    origins.input.digital[1].activeOrigin=100u; origins.input.originError=vr::VRInputError_InvalidDevice; origins.Grip();
    Require(!origins.gripDown,"origin query failure is unavailable grip");
    origins.input.originError=vr::VRInputError_None; origins.input.originDevice=0u; origins.Grip();
    Require(!origins.gripDown,"HMD metadata cannot impersonate a hand");
    origins.input.originDevice=vr::k_unMaxTrackedDeviceCount; origins.Grip();
    Require(!origins.gripDown,"out-of-range origin device is rejected");
    origins.input.originDevice=2u; origins.input.originSource=101u; origins.Grip();
    Require(!origins.gripDown,"wrong source path cannot press right grip");
    origins.input.originSource=100u; origins.system.type=vr::TrackedDeviceClass_TrackingReference; origins.Grip();
    Require(!origins.gripDown,"connected non-controller cannot press grip");
    origins.system.type=vr::TrackedDeviceClass_Controller; origins.system.rightConnected=false; origins.Grip();
    Require(!origins.gripDown,"disconnected grip metadata is unavailable");
    origins.input.originDevice=1u; origins.input.originSource=101u;
    vr::InputDigitalActionData_t healthyLeft{};
    Require(l4d2vr_hand_input::ReadDigitalForHand(&origins.input,&origins.system,origins.m_InputActionFrame,
        origins.m_ActionWeaponGrip,"/user/hand/left",1u,healthyLeft) && healthyLeft.bState,
        "bad right hand leaves healthy left input independent");

    VR unknownMagazine; unknownMagazine.Frame(); unknownMagazine.Cleanup();
    unknownMagazine.UpdateMagazineInteraction(&unknownMagazine.game.player,false,false,false,false,false);
    unknownMagazine.pendingMagazine=true; unknownMagazine.chamberRequired=true;
    const auto unknownGeneration=unknownMagazine.m_MagazineInteractionSessionGeneration.load();
    unknownMagazine.CurlEdges(true); unknownMagazine.CurlEdges(false,false);
    Require(unknownMagazine.m_MagazineInteractionThumbIndexCurlDownPrev &&
        !unknownMagazine.IsMagazineGripInputAvailable() && !unknownMagazine.HasFreshMagazineInteractionTracking(),
        "unknown curl grip preserves levels and pauses physical gestures");
    unknownMagazine.acknowledgement=true;
    unknownMagazine.UpdateMagazineInteraction(&unknownMagazine.game.player,false,false,false,false,false);
    Require(!unknownMagazine.pendingMagazine && unknownMagazine.chamberRequired &&
        unknownMagazine.m_MagazineInteractionSessionGeneration.load()==unknownGeneration,
        "specific grip loss still settles ammo without replacing its owner/generation");
    unknownMagazine.CurlEdges(true);
    Require(!unknownMagazine.curlPressed && unknownMagazine.IsMagazineGripInputAvailable(),
        "held curl recovery resumes without replaying pickup edge");

    VR missingPlayer; missingPlayer.Frame(vr::VRInputError_IPCError); missingPlayer.game.engine.inGame=false; missingPlayer.Cleanup();
    Require(missingPlayer.game.entityLookups==0 && !missingPlayer.m_LeftHandPistolActive.load(),"not-in-game settlement never casts world entity");
    missingPlayer.game.engine.inGame=true; missingPlayer.game.engine.playerIndex=0; missingPlayer.Cleanup();
    Require(missingPlayer.game.entityLookups==0,"zero local player index remains unavailable");
    VR changedOwner; changedOwner.Frame(); changedOwner.Cleanup();
    changedOwner.UpdateMagazineInteraction(&changedOwner.game.player,false,false,false,false,false);
    const auto oldGeneration=changedOwner.m_MagazineInteractionSessionGeneration.load();
    changedOwner.pendingMagazine=true; changedOwner.m_PistolOwnershipMask.store(1u);
    changedOwner.Frame(vr::VRInputError_IPCError); changedOwner.Cleanup();
    Require(changedOwner.GetPrimaryPistolHand()==l4d2vr_dual::Hand::Right &&
        changedOwner.m_MagazineInteractionSessionGeneration.load()>oldGeneration && !changedOwner.pendingMagazine,
        "genuine pistol ownership change still fences the real session");

    VR shell; shell.Frame(vr::VRInputError_IPCError); shell.ShellPending(false);
    Require(shell.m_MagazineInteractionLeftHandHolding && shell.shellWorldUpdates==0,"unknown shell input preserves held flag without world update");
    shell.Frame(); shell.ShellPending(true);
    Require(shell.m_MagazineInteractionLeftHandHolding && shell.shellWorldUpdates==1,"still-held recovery resumes shell world update");
    shell.ShellPending(false); shell.ShellPending(true);
    Require(!shell.m_MagazineInteractionLeftHandHolding && shell.shellWorldUpdates==1,"genuine released shell cannot be resurrected");
    ThrowFailureTests(true); ThrowFailureTests(false);
    std::printf("PASS: action frames (%d checks)\n", checks);
    return EXIT_SUCCESS;
}
