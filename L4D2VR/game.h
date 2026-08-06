#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdarg>
#include <Windows.h>

#include "vector.h"

// === Forward Declarations for Engine Interfaces ===
class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IMaterialSystem;
class IBaseClientDLL;
class IModelInfo;
class IModelRender;
class IMaterial;
class IInput;
class ISurface;
class IGameEventManager2;
class C_BaseEntity;
class C_BasePlayer;
class Server_BaseEntity;
struct model_t;
class IVDebugOverlay;
struct edict_t;

// === Forward Declarations for Internal Systems ===
class Game;
class Offsets;
class VR;
class Hooks;

// === Global Game Instance ===
inline Game* g_Game = nullptr;

// === Per-Player VR State ===
struct ManualThrowPoseSample
{
    bool valid = false;
    bool hasPlayerRelativePosition = false;
    int tick = 0;
    Vector position = { 0.f, 0.f, 0.f };
    Vector playerRelativePosition = { 0.f, 0.f, 0.f };
    QAngle angles = { 0.f, 0.f, 0.f };
};

struct ManualThrowPending
{
    bool valid = false;
    bool inventoryDrop = false;
    bool inventoryDropExecuted = false;
    int weaponId = 0;
    int releaseTick = 0;
    void* owner = nullptr;
    void* sourceWeapon = nullptr;
    void* sourceWeaponVtable = nullptr;
    void* spawnedPhysicsProp = nullptr;
    bool velocityMismatchLogged = false;
    Vector origin = { 0.f, 0.f, 0.f };
    QAngle angles = { 0.f, 0.f, 0.f };
    Vector velocity = { 0.f, 0.f, 0.f };
    Vector angularVelocity = { 0.f, 0.f, 0.f };
};

struct ObjectPullServerState
{
    bool active = false;
    bool launched = false;
    bool catchRequested = false;
    bool holdAsPhysicsProp = false;
    bool held = false;
    bool nativePickupPending = false;
    bool nativePickupTargetSelected = false;
    bool nativePickupIssued = false;
    int launchTick = 0;
    int lastCommandTick = 0;
    // The client keeps transmitting the highlighted map entity index even
    // after a repeatable weapon spawn has produced a separate world weapon.
    int targetEntityIndex = 0;
    void* entity = nullptr;
    void* entityVtable = nullptr;
    int entityIndex = 0;
    void* sourceEntity = nullptr;
    void* sourceEntityVtable = nullptr;
    int sourceEntityIndex = 0;
    bool sourceIsWeaponSpawn = false;
    uint8_t sourceMapPropHint = 0;
    char sourceClassName[128]{};
};

struct VRTrackedPoseLocal
{
    Vector position = { 0.f, 0.f, 0.f };
    QAngle angles = { 0.f, 0.f, 0.f };
};

struct VRPoseFrame
{
    bool valid = false;
    bool bodyYawValid = false;
    std::uint8_t validMask = 0u;
    std::uint8_t featureMask = 0u;
    std::uint8_t handStateFlags = 0u;
    std::uint16_t sequence = 0u;
    std::uint64_t receivedTickMs = 0u;
    float bodyYaw = 0.0f;
    VRTrackedPoseLocal hmd{};
    VRTrackedPoseLocal leftHand{};
    VRTrackedPoseLocal rightHand{};
    std::array<float, 5> leftFingerCurls{};
    std::array<float, 5> rightFingerCurls{};
};

struct Player
{
    C_BasePlayer* pPlayer = nullptr;
    bool isUsingVR = false;

    Vector controllerPos = { 0.f, 0.f, 0.f };
    QAngle controllerAngle = { 0.f, 0.f, 0.f };
    QAngle prevControllerAngle = { 0.f, 0.f, 0.f };

    bool isMeleeing = false;
    bool isNewSwing = false;

    static constexpr size_t kManualThrowPoseSampleCount = 8;
    std::array<ManualThrowPoseSample, kManualThrowPoseSampleCount> manualThrowPoseSamples{};
    int manualThrowPoseCount = 0;
    int manualThrowLastTick = 0;
    uintptr_t throwableAimWeaponTag = 0;
    int throwableAimWeaponId = 0;
    int throwableAimTicks = 0;
    bool throwableAimPrevAttackDown = false;
    bool throwableAimPrevWeaponThrowable = false;
    int manualCarryThrowLastDecodedReleaseTick = 0;
    bool manualEmptyHandsPlaceholderArmed = false;
    bool manualEmptyHandsPlaceholderUseDown = false;
    void* manualEmptyHandsDummyPistol = nullptr;
    void* manualEmptyHandsDummyPistolVtable = nullptr;
    ManualThrowPending manualThrowPending{};
    ObjectPullServerState objectPull{};
    // Source re-decodes backup CUserCmds on later packets. Keep this wire
    // sequence watermark outside ObjectPullServerState so Cancel cannot erase
    // it and let an older Catch bootstrap the same map spawn again.
    int objectPullLastWireCommandNumber = 0;
    int objectPullLastWireTick = 0;
    // This survives active-state reset so a delayed repeated Catch packet
    // cannot relaunch the entity immediately after native pickup.
    int objectPullLastPickedUpEntityIndex = 0;
    int objectPullLastPickupTick = 0;

    // Observer-side VR world-model pose history. Positions and rotations remain
    // in sender body-yaw-local space; DrawModelExecute resolves them against the
    // current rendered player origin/yaw before applying IK.
    VRPoseFrame previousWorldPose{};
    VRPoseFrame latestWorldPose{};
    float worldPoseBlendWeight = 0.0f;
    std::uint64_t worldPoseBlendTickMs = 0u;
    int worldPoseUserID = -1;
};

struct VRPoseRelayServerClient
{
    edict_t* entity = nullptr;
    std::int16_t edictSerial = 0;
    bool protocolSupported = false;
    bool haveSequence = false;
    std::uint16_t lastSequence = 0;
    std::uint64_t lastDecodeTickUs = 0u;
    std::uint64_t lastUploadTickMs = 0u;
    std::uint64_t lastAckTickMs = 0u;
    int ackAttempts = 0;
};

// === Main Game System ===
class Game
{
public:
    // === Engine Interfaces ===
    IClientEntityList* m_ClientEntityList = nullptr;
    IEngineTrace* m_EngineTrace = nullptr;
    IEngineTrace* m_EngineTraceServer = nullptr;
    IEngineClient* m_EngineClient = nullptr;
    void* m_EngineSound = nullptr;
    IMaterialSystem* m_MaterialSystem = nullptr;
    IBaseClientDLL* m_BaseClientDll = nullptr;
    IModelInfo* m_ModelInfo = nullptr;
    IModelRender* m_ModelRender = nullptr;
    IInput* m_VguiInput = nullptr;
    ISurface* m_VguiSurface = nullptr;
    IVDebugOverlay* m_DebugOverlay = nullptr;
    IGameEventManager2* m_GameEventManager = nullptr;
    void* m_Cvar = nullptr;
    // Present in a listen-server process and used to issue commands to a
    // specific connected client without loading a second plugin module.
    void* m_ServerPluginHelpers = nullptr;
    void* m_ServerGameClients = nullptr;

    // === Module Base Addresses ===
    uintptr_t m_BaseEngine = 0;
    uintptr_t m_BaseClient = 0;
    uintptr_t m_BaseServer = 0;
    uintptr_t m_BaseMaterialSystem = 0;
    uintptr_t m_BaseVgui2 = 0;

    // === Internal Systems ===
    Offsets* m_Offsets = nullptr;
    VR* m_VR = nullptr;
    Hooks* m_Hooks = nullptr;

    // === State Flags ===
    bool m_Initialized = false;
    bool m_PerformingMelee = false;
    int m_CurrentUsercmdID = -1;
    Server_BaseEntity* m_CurrentUsercmdPlayer = nullptr;
    edict_t* m_CurrentUsercmdEdict = nullptr;

    // === Player VR State (Multiplayer) ===
    // Matches Source's MAX_PLAYERS (65) to cover the full player index range.
    static constexpr size_t kMaxPlayers = 65;
    std::array<Player, kMaxPlayers> m_PlayersVRInfo;
    mutable std::mutex m_VRPoseMutex;
    std::atomic<bool> m_VRPoseServerCapable{ false };
    std::atomic<bool> m_VRPoseHelloSent{ false };
    std::uint64_t m_VRPoseLastLocalPublishTickMs = 0u;
    std::uint16_t m_VRPoseLocalSequence = 0u;
    std::recursive_mutex m_BuiltinVRPoseRelayMutex;
    std::array<VRPoseRelayServerClient, kMaxPlayers>
        m_BuiltinVRPoseRelayClients{};

    // === Weapon / Viewmodel State ===
    bool m_IsMeleeWeaponActive = false;
    bool m_SwitchedWeapons = false;
    model_t* m_ArmsModel = nullptr;
    IMaterial* m_ArmsMaterial = nullptr;
    bool m_CachedArmsModel = false;

    // === Constructor ===
    Game();

    // === Interface Utilities ===
    void* GetInterface(const char* dllname, const char* interfacename);
    C_BaseEntity* GetClientEntity(int entityIndex);
    char* getNetworkName(uintptr_t* entity);
    const char* GetNetworkClassName(uintptr_t* entity) const;
    int FindRecvPropOffset(const char* networkName, const char* propName) const;

    // === Rendering Thread Mode ===
    // Returns material system thread mode (0 = single-threaded, >0 = queued/multicore).
    int GetMatQueueMode() const;

    // === Command Execution ===
    void ServerCmd(const char* szCmdString, bool reliable = true);
    void ClientCmd(const char* szCmdString);
    void ClientCmd_Unrestricted(const char* szCmdString);
    void* FindConVar(const char* name) const;
    const char* GetConVarNameFromPointer(const void* convar) const;
    const char* GetConVarNameFromIConVarPointer(const void* iconvar) const;
    void* GetConVarPrimaryStringSetValueTarget(const char* name) const;
    void* GetConVarPrimaryFloatSetValueTarget(const char* name) const;
    void* GetConVarPrimaryIntSetValueTarget(const char* name) const;
    void* GetConVarStringSetValueTarget(const char* name) const;
    void* GetConVarFloatSetValueTarget(const char* name) const;
    void* GetConVarIntSetValueTarget(const char* name) const;
    void* GetConVarInternalStringSetValueTarget(const char* name) const;
    void* GetConVarInternalFloatSetValueTarget(const char* name) const;
    void* GetConVarInternalIntSetValueTarget(const char* name) const;
    int GetConVarInt(const char* name, int fallback = 0) const;
    int GetConVarIntDirect(const char* name, int fallback = 0) const;
    float GetConVarFloat(const char* name, float fallback = 0.0f) const;
    float GetConVarFloatDirect(const char* name, float fallback = 0.0f) const;
    std::string GetConVarString(const char* name) const;
    int GetConVarFlags(const char* name) const;
    bool SetConVarFlags(const char* name, int flags) const;
    bool SetConVarString(const char* name, const char* value) const;
    bool SetConVarInt(const char* name, int value) const;
    bool SetConVarFloat(const char* name, float value) const;
    bool SetConVarBool(const char* name, bool value) const;
    static void BeginConVarWritePermit();
    static void EndConVarWritePermit();
    static bool HasConVarWritePermit();
    int GetEntityEffects(const C_BaseEntity* entity, int fallback = 0) const;

    // === Logging ===
    static void logMsg(const char* fmt, ...);
    static void errorMsg(const char* msg);
    static bool InstallVertexFormatWarningFilter();
    static void UninstallVertexFormatWarningFilter();

    // === Player Utilities ===
    bool IsValidPlayerIndex(int index) const;
    void ResetAllPlayerVRInfo();

    // === Multiplayer VR world-model poses ===
    void ResetVRPoseServerSession();
    void HandleVRPoseServerAck(int protocolVersion);
    void ObserveBuiltinVRPoseRelayClient(
        int playerIndex,
        edict_t* entity);
    bool HandleBuiltinVRPoseRelayCommand(
        edict_t* entity,
        const void* sourceCommand);
    void PublishLocalVRPose(VR* vr, C_BasePlayer* localPlayer);
    bool ReceiveVRPosePayload(
        int playerIndex,
        std::uint16_t sequence,
        const char* encodedPayload,
        bool fromServer,
        float encodedBodyYaw);
    bool GetInterpolatedVRPose(
        int playerIndex,
        float interpolationDelayMs,
        float staleAfterMs,
        VRPoseFrame& outPose,
        float& outFreshness) const;
    float AdvanceVRPoseBlendWeight(
        int playerIndex,
        float targetWeight,
        float blendSeconds);
};

// === Logging Macros (Debug Only) ===
#ifdef _DEBUG
#define LOG(fmt, ...) Game::logMsg("[LOG] " fmt, ##__VA_ARGS__)
#define ERR(msg) Game::errorMsg("[ERROR] " msg)
#else
#define LOG(fmt, ...)
#define ERR(msg)
#endif
