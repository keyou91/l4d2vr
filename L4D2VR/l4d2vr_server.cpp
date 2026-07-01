#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "sigscanner.h"
#include "sdk/vector.h"

class Server_BaseEntity;

class IHandleEntity
{
public:
    virtual ~IHandleEntity() {}
    virtual void SetRefEHandle(int handle) = 0;
    virtual void GetRefEHandle() const = 0;
};

class IServerUnknown : public IHandleEntity
{
public:
    virtual void* GetCollideable() = 0;
    virtual void* GetNetworkable() = 0;
    virtual Server_BaseEntity* GetBaseEntity() = 0;
};

struct edict_t
{
    int m_fStateFlags;
    short m_NetworkSerialNumber;
    short m_EdictIndex;
    void* m_pNetworkable;
    IServerUnknown* m_pUnk;
    float freetime;
};

#include "sdk/sdk_server.h"

class matrix3x4_t;

struct cplane_t
{
    Vector normal;
    float dist;
    unsigned char type;
    unsigned char signbits;
    unsigned char pad[2];
};

struct csurface_t
{
    const char* name;
    short surfaceProps;
    unsigned short flags;
};

constexpr unsigned int CONTENTS_SOLID = 0x1;
constexpr unsigned int CONTENTS_WINDOW = 0x2;
constexpr unsigned int CONTENTS_GRATE = 0x8;
constexpr unsigned int CONTENTS_MOVEABLE = 0x4000;
constexpr unsigned int CONTENTS_PLAYERCLIP = 0x10000;
constexpr unsigned int CONTENTS_MONSTER = 0x2000000;

class CBaseTrace
{
public:
    Vector startpos;
    Vector endpos;
    cplane_t plane;
    float fraction = 1.0f;
    int contents = 0;
    unsigned short dispFlags = 0;
    bool allsolid = false;
    bool startsolid = false;
};

class CGameTrace : public CBaseTrace
{
public:
    float fractionleftsolid = 0.0f;
    csurface_t surface{};
    int hitgroup = 0;
    short physicsbone = 0;
    void* m_pEnt = nullptr;
    int hitbox = 0;
};

enum class TraceType
{
    TRACE_EVERYTHING = 0,
    TRACE_WORLD_ONLY,
    TRACE_ENTITIES_ONLY,
    TRACE_EVERYTHING_FILTER_PROPS,
};

class ITraceFilter
{
public:
    virtual bool ShouldHitEntity(IHandleEntity* entity, int contentsMask) = 0;
    virtual TraceType GetTraceType() const = 0;
};

class CTraceFilter : public ITraceFilter
{
public:
    CTraceFilter(IHandleEntity* passEntity, int collisionGroup)
        : m_pPassEnt(passEntity)
        , m_collisionGroup(collisionGroup)
    {
    }

    TraceType GetTraceType() const override
    {
        return TraceType::TRACE_EVERYTHING;
    }

    IHandleEntity* m_pPassEnt = nullptr;
    int m_collisionGroup = 0;
    void* m_pExtraShouldHitCheckFunction = nullptr;
};

class CTraceFilterSkipSelf : public CTraceFilter
{
public:
    CTraceFilterSkipSelf(IHandleEntity* passEntity, int collisionGroup)
        : CTraceFilter(passEntity, collisionGroup)
    {
    }

    bool ShouldHitEntity(IHandleEntity* entity, int) override
    {
        return entity && entity != m_pPassEnt;
    }
};

struct Ray_t
{
    VectorAligned m_Start;
    VectorAligned m_Delta;
    VectorAligned m_StartOffset;
    VectorAligned m_Extents;
    const matrix3x4_t* m_pWorldAxisTransform = nullptr;
    bool m_IsRay = true;
    bool m_IsSwept = false;

    void Init(const Vector& start, const Vector& end)
    {
        VectorSubtract(end, start, m_Delta);
        m_IsSwept = (m_Delta.LengthSqr() != 0.0f);
        VectorClear(m_Extents);
        m_IsRay = true;
        m_pWorldAxisTransform = nullptr;
        VectorClear(m_StartOffset);
        VectorCopy(start, m_Start);
    }

    void Init(const Vector& start, const Vector& end, const Vector& mins, const Vector& maxs)
    {
        VectorSubtract(end, start, m_Delta);
        m_IsSwept = (m_Delta.LengthSqr() != 0.0f);
        VectorSubtract(maxs, mins, m_Extents);
        m_Extents *= 0.5f;
        m_IsRay = (m_Extents.LengthSqr() < 1e-6f);
        m_pWorldAxisTransform = nullptr;
        VectorAdd(mins, maxs, m_StartOffset);
        m_StartOffset *= 0.5f;
        VectorAdd(start, m_StartOffset, m_Start);
        m_StartOffset *= -1.0f;
    }
};

using trace_t = CGameTrace;

class IEngineTrace
{
public:
    virtual void fn0() = 0;
    virtual void fn1() = 0;
    virtual void fn2() = 0;
    virtual void fn3() = 0;
    virtual void fn4() = 0;
    virtual void TraceRay(const Ray_t& ray, unsigned int mask, CTraceFilter* filter, trace_t* trace) = 0;
};

class IVEngineServer
{
public:
    virtual void ChangeLevel(const char*, const char*) = 0;
    virtual int IsMapValid(const char*) = 0;
    virtual bool IsDedicatedServer() = 0;
    virtual int IsInEditMode() = 0;
    virtual void* GetLaunchOptions() = 0;
    virtual int PrecacheModel(const char*, bool = false) = 0;
    virtual int PrecacheSentenceFile(const char*, bool = false) = 0;
    virtual int PrecacheDecal(const char*, bool = false) = 0;
    virtual int PrecacheGeneric(const char*, bool = false) = 0;
    virtual bool IsModelPrecached(const char*) const = 0;
    virtual bool IsDecalPrecached(const char*) const = 0;
    virtual bool IsGenericPrecached(const char*) const = 0;
    virtual int GetClusterForOrigin(const Vector&) = 0;
    virtual int GetPVSForCluster(int, int, unsigned char*) = 0;
    virtual bool CheckOriginInPVS(const Vector&, const unsigned char*, int) = 0;
    virtual bool CheckBoxInPVS(const Vector&, const Vector&, const unsigned char*, int) = 0;
    virtual int GetPlayerUserId(const edict_t*) = 0;
    virtual const char* GetPlayerNetworkIDString(const edict_t*) = 0;
    virtual bool IsUserIDInUse(int) = 0;
    virtual int GetLoadingProgressForUserID(int) = 0;
    virtual int GetEntityCount() = 0;
    virtual void* GetPlayerNetInfo(int) = 0;
    virtual edict_t* CreateEdict(int = -1) = 0;
    virtual void RemoveEdict(edict_t*) = 0;
    virtual void* PvAllocEntPrivateData(long) = 0;
    virtual void FreeEntPrivateData(void*) = 0;
    virtual void* SaveAllocMemory(size_t, size_t) = 0;
    virtual void SaveFreeMemory(void*) = 0;
    virtual void EmitAmbientSound(int, const Vector&, const char*, float, int, int, int, float = 0.0f) = 0;
    virtual void FadeClientVolume(const edict_t*, float, float, float, float) = 0;
    virtual int SentenceGroupPick(int, char*, int) = 0;
    virtual int SentenceGroupPickSequential(int, char*, int, int, int) = 0;
    virtual int SentenceIndexFromName(const char*) = 0;
    virtual const char* SentenceNameFromIndex(int) = 0;
    virtual int SentenceGroupIndexFromName(const char*) = 0;
    virtual const char* SentenceGroupNameFromIndex(int) = 0;
    virtual float SentenceLength(int) = 0;
    virtual void ServerCommand(const char*) = 0;
    virtual void ServerExecute() = 0;
    virtual void ClientCommand(edict_t*, const char*, ...) = 0;
};

namespace
{
    static_assert(sizeof(void*) == 4, "L4D2VR dedicated server plugin must be built as 32-bit/x86.");

    constexpr int kMaxPlayers = 65;
    constexpr int kWeaponMelee = 19;
    constexpr int kWeaponMolotov = 13;
    constexpr int kWeaponPipeBomb = 14;
    constexpr int kWeaponVomitJar = 25;
    constexpr int kUsingMountedGunOffset = 0x1EBA;
    constexpr int kUsingMountedWeaponOffset = 0x1EBB;
    constexpr int kVrExtraCommandMagic = 0x56000000;
    constexpr int kVrExtraCommandMagicMask = 0xFF000000;
    constexpr int kVrExtraTypeShift = 20;
    constexpr int kVrExtraFlagsShift = 16;
    constexpr int kVrExtraLowMask = 0xFFFF;
    constexpr int kVrExtraTypeTeleport = 1;
    constexpr int kVrExtraTypeRoomscale = 2;
    constexpr int kVrExtraFlagCrouched = 1 << 0;
    constexpr int kServerAckMaxAttempts = 8;
    constexpr int kServerAckRetryMs = 1000;
    constexpr int kMeleeSwingHistoryCapacity = 16;
    constexpr float kDefaultTeleportMaxDistanceUnits = 2500.0f;
    constexpr char kServerAckCommand[] = "l4d2vr_server_ack 1\n";
    constexpr char kServerLogFileName[] = "l4d2vr_server_log.txt";
    constexpr char kServerConfigFileName[] = "l4d2vr_server_config.txt";

    constexpr int kIsIncapacitatedOffset = 0x1EA9;
    constexpr int kTongueOwnerOffset = 0x1F6C;
    constexpr int kIsHangingFromTongueOffset = 0x1F84;
    constexpr int kCarryAttackerOffset = 0x2714;
    constexpr int kPummelAttackerOffset = 0x2720;
    constexpr int kPounceAttackerOffset = 0x272C;
    constexpr int kJockeyAttackerOffset = 0x274C;

    struct ServerConfig
    {
        bool teleportEnabled = true;
        bool teleportBlockWhileControlled = true;
        bool teleportBlockWhileIncapacitated = true;
        float teleportMaxDistanceUnits = kDefaultTeleportMaxDistanceUnits;
        float teleportCooldownSeconds = 1.0f;

        bool meleeSwingEnabled = true;
        bool meleeSwingBlockWhileControlled = true;
        bool meleeSwingBlockWhileIncapacitated = true;
        float meleeSwingCooldownSeconds = 0.35f;
        float meleeSwingBurstWindowSeconds = 2.0f;
        int meleeSwingBurstMax = 4;
    };

    struct PlayerVrState
    {
        bool isUsingVR = false;
        bool isMeleeing = false;
        bool physicalCrouch = false;
        bool isNewSwing = false;
        bool currentMeleeSwingAllowed = false;
        bool serverAckPending = false;
        int serverAckAttempts = 0;
        edict_t* serverAckEdict = nullptr;
        std::chrono::steady_clock::time_point nextServerAckTime{};
        std::chrono::steady_clock::time_point lastTeleportTime{};
        std::chrono::steady_clock::time_point lastMeleeSwingTime{};
        std::array<std::chrono::steady_clock::time_point, kMeleeSwingHistoryCapacity> meleeSwingHistory{};
        Vector controllerPos = Vector(0.0f, 0.0f, 0.0f);
        QAngle controllerAngle = QAngle(0.0f, 0.0f, 0.0f);
        QAngle prevControllerAngle = QAngle(0.0f, 0.0f, 0.0f);
    };

    enum class VrExtraCommandType
    {
        None,
        Teleport,
        Roomscale,
    };

    struct VrExtraCommand
    {
        VrExtraCommandType type = VrExtraCommandType::None;
        bool crouched = false;
        Vector value = Vector(0.0f, 0.0f, 0.0f);
    };

    class CUserCmd
    {
    public:
        virtual ~CUserCmd() {}

        int command_number;
        int tick_count;
        QAngle viewangles;
        float forwardmove;
        float sidemove;
        float upmove;
        int buttons;
        unsigned char impulse;
        int weaponselect;
        int weaponsubtype;
        int random_seed;
        short mousedx;
        short mousedy;
        bool hasbeenpredicted;
        char pad[25];
    };

    static_assert(sizeof(CUserCmd) == 0x58, "Unexpected CUserCmd layout.");

    using tServerFireTerrorBullets = int(__cdecl*)(int playerId, const Vector& origin, const QAngle& angles, int a4, int a5, int a6, float a7);
    using tProcessUsercmds = float(__thiscall*)(void* thisptr, edict_t* player, void* buf, int numcmds, int totalcmds, int droppedPackets, bool ignore, bool paused);
    using tReadUsercmd = int(__cdecl*)(void* buf, CUserCmd* move, CUserCmd* from);
    using tTestMeleeSwingCollision = int(__thiscall*)(void* thisptr, Vector const& direction);
    using tGetPrimaryAttackActivity = int(__thiscall*)(void* thisptr, void* meleeInfo);
    using tEyePosition = Vector* (__thiscall*)(void* thisptr, Vector* eyePos);
    using tEyeAngles = const QAngle* (__thiscall*)(void* thisptr);
    using tFindUseEntity = Server_BaseEntity* (__thiscall*)(void* thisptr, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra);
    using tPlayerUse = void(__thiscall*)(void* thisptr, void* useEntity);
    using tGetActiveWeapon = Server_WeaponCSBase* (__thiscall*)(void* thisptr);
    using tGetMeleeWeaponInfo = void* (__thiscall*)(void* thisptr);
    using tGetAbsOriginServer = Vector* (__thiscall*)(void* thisptr);
    using tSetOriginServer = void(__thiscall*)(void* thisptr, const Vector& origin);

    std::mutex g_logMutex;
    std::array<PlayerVrState, kMaxPlayers> g_players{};
    std::atomic_bool g_hooksInstalled{ false };
    std::atomic_bool g_hookWorkerStarted{ false };
    std::atomic_bool g_unloading{ false };
    HMODULE g_module = nullptr;
    IEngineTrace* g_engineTraceServer = nullptr;
    IVEngineServer* g_engineServer = nullptr;
    ServerConfig g_config{};

    thread_local int g_currentUsercmdPlayerIndex = -1;
    thread_local Server_BaseEntity* g_currentUsercmdPlayer = nullptr;
    thread_local IServerUnknown* g_currentUsercmdUnknown = nullptr;
    thread_local bool g_performingMeleeTrace = false;
    thread_local bool g_commandControllerAimOverride = false;
    thread_local void* g_commandControllerAimPlayer = nullptr;
    thread_local Vector g_commandControllerAimOrigin = Vector(0.0f, 0.0f, 0.0f);
    thread_local QAngle g_commandControllerAimAngles = QAngle(0.0f, 0.0f, 0.0f);
    thread_local int g_commandControllerAimReason = 0;
    thread_local bool g_useControllerAimOverride = false;
    thread_local void* g_useControllerAimPlayer = nullptr;
    thread_local Vector g_useControllerAimOrigin = Vector(0.0f, 0.0f, 0.0f);
    thread_local QAngle g_useControllerAimAngles = QAngle(0.0f, 0.0f, 0.0f);
    thread_local QAngle g_returnEyeAngles = QAngle(0.0f, 0.0f, 0.0f);
    thread_local VrExtraCommand g_pendingExtraCommand{};

    void ResetLogFile()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);

        FILE* file = nullptr;
        if (fopen_s(&file, kServerLogFileName, "w") == 0 && file)
        {
            fputs("L4D2VR dedicated server log\n", file);
            fclose(file);
        }
    }

    void Log(const char* fmt, ...)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);

        SYSTEMTIME st{};
        GetLocalTime(&st);

        char message[2048] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        va_end(args);

        char line[2300] = {};
        snprintf(line, sizeof(line),
            "[%04u-%02u-%02u %02u:%02u:%02u] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, message);

        OutputDebugStringA(line);

        FILE* file = nullptr;
        if (fopen_s(&file, kServerLogFileName, "a") == 0 && file)
        {
            fputs(line, file);
            fclose(file);
        }
    }

    bool IsValidPlayerIndex(int index)
    {
        return index >= 0 && index < static_cast<int>(g_players.size());
    }

    std::string Trim(std::string value)
    {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    std::string ToLowerAscii(std::string value)
    {
        for (char& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    }

    bool ParseBoolValue(const std::string& value, bool& out)
    {
        const std::string lower = ToLowerAscii(Trim(value));
        if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
        {
            out = true;
            return true;
        }

        if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
        {
            out = false;
            return true;
        }

        return false;
    }

    bool ParseFloatValue(const std::string& value, float& out)
    {
        char* end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        if (end == value.c_str() || !std::isfinite(parsed))
            return false;

        out = parsed;
        return true;
    }

    bool ParseIntValue(const std::string& value, int& out)
    {
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str())
            return false;

        out = static_cast<int>(std::clamp<long>(parsed, -1000000L, 1000000L));
        return true;
    }

    void WriteDefaultServerConfigIfMissing()
    {
        FILE* existing = nullptr;
        if (fopen_s(&existing, kServerConfigFileName, "r") == 0 && existing)
        {
            fclose(existing);
            return;
        }

        FILE* file = nullptr;
        if (fopen_s(&file, kServerConfigFileName, "w") != 0 || !file)
            return;

        fputs(
            "# L4D2VR dedicated server config. Edit values, then restart the server.\n"
            "# Distances are Source units. Rough reference: 43.2 units is about 1 meter.\n"
            "TeleportEnabled=true\n"
            "TeleportBlockWhileControlled=true\n"
            "TeleportBlockWhileIncapacitated=true\n"
            "TeleportMaxDistanceUnits=2500\n"
            "# Optional alternative; if set after TeleportMaxDistanceUnits it overrides it.\n"
            "# TeleportMaxDistanceMeters=20\n"
            "TeleportCooldownSeconds=1.0\n"
            "\n"
            "MeleeSwingEnabled=true\n"
            "MeleeSwingBlockWhileControlled=true\n"
            "MeleeSwingBlockWhileIncapacitated=true\n"
            "MeleeSwingCooldownSeconds=0.35\n"
            "MeleeSwingBurstWindowSeconds=2.0\n"
            "MeleeSwingBurstMax=4\n",
            file);
        fclose(file);
    }

    void ApplyServerConfigValue(const std::string& rawKey, const std::string& rawValue)
    {
        const std::string key = ToLowerAscii(Trim(rawKey));
        const std::string value = Trim(rawValue);

        bool boolValue = false;
        float floatValue = 0.0f;
        int intValue = 0;

        if (key == "teleportenabled" && ParseBoolValue(value, boolValue))
            g_config.teleportEnabled = boolValue;
        else if (key == "teleportblockwhilecontrolled" && ParseBoolValue(value, boolValue))
            g_config.teleportBlockWhileControlled = boolValue;
        else if (key == "teleportblockwhileincapacitated" && ParseBoolValue(value, boolValue))
            g_config.teleportBlockWhileIncapacitated = boolValue;
        else if (key == "teleportmaxdistanceunits" && ParseFloatValue(value, floatValue))
            g_config.teleportMaxDistanceUnits = std::clamp(floatValue, 1.0f, 100000.0f);
        else if (key == "teleportmaxdistancemeters" && ParseFloatValue(value, floatValue))
            g_config.teleportMaxDistanceUnits = std::clamp(floatValue * 43.2f, 1.0f, 100000.0f);
        else if (key == "teleportcooldownseconds" && ParseFloatValue(value, floatValue))
            g_config.teleportCooldownSeconds = std::clamp(floatValue, 0.0f, 600.0f);
        else if (key == "meleeswingenabled" && ParseBoolValue(value, boolValue))
            g_config.meleeSwingEnabled = boolValue;
        else if (key == "meleeswingblockwhilecontrolled" && ParseBoolValue(value, boolValue))
            g_config.meleeSwingBlockWhileControlled = boolValue;
        else if (key == "meleeswingblockwhileincapacitated" && ParseBoolValue(value, boolValue))
            g_config.meleeSwingBlockWhileIncapacitated = boolValue;
        else if (key == "meleeswingcooldownseconds" && ParseFloatValue(value, floatValue))
            g_config.meleeSwingCooldownSeconds = std::clamp(floatValue, 0.0f, 60.0f);
        else if (key == "meleeswingburstwindowseconds" && ParseFloatValue(value, floatValue))
            g_config.meleeSwingBurstWindowSeconds = std::clamp(floatValue, 0.0f, 60.0f);
        else if (key == "meleeswingburstmax" && ParseIntValue(value, intValue))
            g_config.meleeSwingBurstMax = std::clamp(intValue, 0, kMeleeSwingHistoryCapacity);
    }

    void LoadServerConfig()
    {
        g_config = ServerConfig{};
        WriteDefaultServerConfigIfMissing();

        FILE* file = nullptr;
        if (fopen_s(&file, kServerConfigFileName, "r") != 0 || !file)
        {
            Log("server config not found; using defaults");
            return;
        }

        char lineBuffer[512] = {};
        while (fgets(lineBuffer, sizeof(lineBuffer), file))
        {
            std::string line(lineBuffer);
            const size_t comment = line.find('#');
            if (comment != std::string::npos)
                line.erase(comment);

            line = Trim(line);
            if (line.empty())
                continue;

            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            ApplyServerConfigValue(line.substr(0, equals), line.substr(equals + 1));
        }
        fclose(file);

        Log("server config loaded: teleport enabled=%d controlledBlock=%d incapBlock=%d maxUnits=%.1f cooldown=%.2f melee enabled=%d controlledBlock=%d incapBlock=%d cooldown=%.2f burstWindow=%.2f burstMax=%d",
            g_config.teleportEnabled ? 1 : 0,
            g_config.teleportBlockWhileControlled ? 1 : 0,
            g_config.teleportBlockWhileIncapacitated ? 1 : 0,
            g_config.teleportMaxDistanceUnits,
            g_config.teleportCooldownSeconds,
            g_config.meleeSwingEnabled ? 1 : 0,
            g_config.meleeSwingBlockWhileControlled ? 1 : 0,
            g_config.meleeSwingBlockWhileIncapacitated ? 1 : 0,
            g_config.meleeSwingCooldownSeconds,
            g_config.meleeSwingBurstWindowSeconds,
            g_config.meleeSwingBurstMax);
    }

    int GetPlayerIndexFromEdict(const edict_t* entity)
    {
        if (!entity)
            return -1;

        return static_cast<int>(entity->m_EdictIndex);
    }

    bool SendServerAckToClient(edict_t* entity, int playerIndex, const char* reason)
    {
        if (!g_engineServer || !entity || !IsValidPlayerIndex(playerIndex))
            return false;

#ifdef _MSC_VER
        __try
        {
            g_engineServer->ClientCommand(entity, kServerAckCommand);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("failed to send L4D2VR server ack to player %d (%s)", playerIndex, reason ? reason : "unknown");
            return false;
        }
#else
        g_engineServer->ClientCommand(entity, kServerAckCommand);
#endif

        Log("sent L4D2VR server ack to player %d (%s)", playerIndex, reason ? reason : "unknown");
        return true;
    }

    void QueueServerAckForClient(edict_t* entity, const char* reason)
    {
        const int playerIndex = GetPlayerIndexFromEdict(entity);
        if (!IsValidPlayerIndex(playerIndex))
            return;

        PlayerVrState& player = g_players[playerIndex];
        player.serverAckEdict = entity;
        player.serverAckPending = true;
        player.serverAckAttempts = 0;
        player.nextServerAckTime = {};
        SendServerAckToClient(entity, playerIndex, reason);
        player.serverAckAttempts = 1;
        player.nextServerAckTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(kServerAckRetryMs);
    }

    void PumpPendingServerAcks()
    {
        if (!g_engineServer)
            return;

        const auto now = std::chrono::steady_clock::now();
        for (int playerIndex = 1; playerIndex < static_cast<int>(g_players.size()); ++playerIndex)
        {
            PlayerVrState& player = g_players[playerIndex];
            if (!player.serverAckPending || !player.serverAckEdict)
                continue;

            if (player.serverAckAttempts >= kServerAckMaxAttempts)
            {
                player.serverAckPending = false;
                continue;
            }

            if (player.nextServerAckTime.time_since_epoch().count() != 0 && now < player.nextServerAckTime)
                continue;

            SendServerAckToClient(player.serverAckEdict, playerIndex, "retry");
            ++player.serverAckAttempts;
            player.nextServerAckTime = now + std::chrono::milliseconds(kServerAckRetryMs);
        }
    }

    bool IsFiniteVector(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFiniteAngles(const QAngle& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    template <typename T>
    bool TryReadEntityValue(const Server_BaseEntity* entity, int offset, T& out)
    {
        if (!entity || offset < 0)
            return false;

#ifdef _MSC_VER
        __try
        {
            out = *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(entity) + offset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        out = *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(entity) + offset);
        return true;
#endif
    }

    bool HandleValid(int handle)
    {
        return handle != 0 && handle != -1;
    }

    bool IsServerPlayerIncapacitated(const Server_BaseEntity* player)
    {
        unsigned char incapacitated = 0;
        return TryReadEntityValue(player, kIsIncapacitatedOffset, incapacitated) && incapacitated != 0;
    }

    bool IsServerPlayerControlledBySI(const Server_BaseEntity* player)
    {
        if (!player)
            return false;

        int tongueOwner = 0;
        unsigned char hangingFromTongue = 0;
        int carryAttacker = 0;
        int pummelAttacker = 0;
        int pounceAttacker = 0;
        int jockeyAttacker = 0;

        TryReadEntityValue(player, kTongueOwnerOffset, tongueOwner);
        TryReadEntityValue(player, kIsHangingFromTongueOffset, hangingFromTongue);
        TryReadEntityValue(player, kCarryAttackerOffset, carryAttacker);
        TryReadEntityValue(player, kPummelAttackerOffset, pummelAttacker);
        TryReadEntityValue(player, kPounceAttackerOffset, pounceAttacker);
        TryReadEntityValue(player, kJockeyAttackerOffset, jockeyAttacker);

        return hangingFromTongue != 0 ||
            HandleValid(tongueOwner) ||
            HandleValid(carryAttacker) ||
            HandleValid(pummelAttacker) ||
            HandleValid(pounceAttacker) ||
            HandleValid(jockeyAttacker);
    }

    int DecodeSigned16(int value)
    {
        return static_cast<int>(static_cast<int16_t>(value & 0xFFFF));
    }

    bool DecodeVrExtraCommand(const CUserCmd* move, VrExtraCommand& out)
    {
        out = {};
        if (!move)
            return false;

        const int header = move->weaponselect;
        if ((header & kVrExtraCommandMagicMask) != kVrExtraCommandMagic)
            return false;

        const int type = (header >> kVrExtraTypeShift) & 0x0F;
        const int flags = (header >> kVrExtraFlagsShift) & 0x0F;
        const int z = DecodeSigned16(header);
        const int packedXY = move->weaponsubtype;
        const int x = DecodeSigned16(packedXY);
        const int y = DecodeSigned16(packedXY >> 16);

        out.crouched = (flags & kVrExtraFlagCrouched) != 0;
        if (type == kVrExtraTypeTeleport)
        {
            out.type = VrExtraCommandType::Teleport;
            out.value = Vector(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        }
        else if (type == kVrExtraTypeRoomscale)
        {
            out.type = VrExtraCommandType::Roomscale;
            out.value = Vector(static_cast<float>(x) / 10.0f, static_cast<float>(y) / 10.0f, 0.0f);
        }
        else
        {
            return false;
        }

        return IsFiniteVector(out.value);
    }

    void ClearVrExtraCommandFields(CUserCmd* move)
    {
        if (!move)
            return;

        if ((move->weaponselect & kVrExtraCommandMagicMask) == kVrExtraCommandMagic)
        {
            move->weaponselect = 0;
            move->weaponsubtype = 0;
        }
    }

    void NormalizeAndClampViewAngles(QAngle& angles)
    {
        while (angles.x > 180.0f) angles.x -= 360.0f;
        while (angles.x < -180.0f) angles.x += 360.0f;
        while (angles.y > 180.0f) angles.y -= 360.0f;
        while (angles.y < -180.0f) angles.y += 360.0f;
        angles.z = 0.0f;
        angles.x = std::clamp(angles.x, -89.0f, 89.0f);
    }

    Server_BaseEntity* GetBaseEntityFromEdict(edict_t* player)
    {
        if (!player || !player->m_pUnk)
            return nullptr;

#ifdef _MSC_VER
        __try
        {
            return player->m_pUnk->GetBaseEntity();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return player->m_pUnk->GetBaseEntity();
#endif
    }

    void TraceServerRay(const Ray_t& ray, unsigned int mask, CTraceFilter* filter, CGameTrace& outTrace)
    {
        outTrace = {};
        outTrace.fraction = 1.0f;
        if (!g_engineTraceServer)
            return;

#ifdef _MSC_VER
        __try
        {
            g_engineTraceServer->TraceRay(ray, mask, filter, &outTrace);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outTrace = {};
            outTrace.fraction = 1.0f;
        }
#else
        g_engineTraceServer->TraceRay(ray, mask, filter, &outTrace);
#endif
    }

    struct DedicatedOffset
    {
        const char* name = nullptr;
        const char* moduleName = nullptr;
        int offset = 0;
        const char* signature = nullptr;
        int sigOffset = 0;
        bool optional = false;
        uintptr_t address = 0;
        bool valid = false;

        bool Resolve()
        {
            address = 0;
            valid = false;

            if (!moduleName || !signature)
                return false;

            const int verifiedOffset = SigScanner::VerifyOffset(moduleName, offset, signature, sigOffset);
            if (verifiedOffset > 0)
                offset = verifiedOffset;
            else if (verifiedOffset == -1)
            {
                if (!optional)
                    Log("required signature not found: %s", name ? name : signature);
                else
                    Log("optional signature not found: %s", name ? name : signature);
                return optional;
            }

            HMODULE module = GetModuleHandleA(moduleName);
            if (!module)
            {
                if (!optional)
                    Log("required module not loaded: %s", moduleName);
                return optional;
            }

            address = reinterpret_cast<uintptr_t>(module) + static_cast<uintptr_t>(offset);
            valid = address != 0;
            if (valid)
                Log("resolved %s at %s+0x%X", name ? name : "<offset>", moduleName, offset);
            return valid || optional;
        }
    };

    struct DedicatedOffsets
    {
        DedicatedOffset ServerFireTerrorBullets{ "ServerFireTerrorBullets", "server.dll", 0x3C3FC0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
        DedicatedOffset ReadUserCmd{ "ReadUserCmd", "server.dll", 0x205100, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
        DedicatedOffset ProcessUsercmds{ "ProcessUsercmds", "server.dll", 0xEF710, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };
        DedicatedOffset TestMeleeSwingServer{ "TestMeleeSwingServer", "server.dll", 0x3E79E0, "24 FF D2 5B 5F 5E C3", 20 };
        DedicatedOffset GetPrimaryAttackActivity{ "GetPrimaryAttackActivity", "server.dll", 0x3E7630, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?" };
        DedicatedOffset GetActiveWeapon{ "GetActiveWeapon", "server.dll", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64 };
        DedicatedOffset GetMeleeWeaponInfo{ "GetMeleeWeaponInfo", "server.dll", 0x3E67D0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
        DedicatedOffset EyePosition{ "EyePosition", "server.dll", 0x6D610, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" };
        DedicatedOffset ServerPlayerEyeAngles{ "ServerPlayerEyeAngles", "server.dll", 0x7B0F0, "55 8B EC 83 EC 64 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 64 80 3D ? ? ? ? 00 75 5B 8B 87 ? ? ? ? 83 F8 FF 74 50", 0, true };
        DedicatedOffset ServerPlayerEyePosition{ "ServerPlayerEyePosition", "server.dll", 0x7B2A0, "55 8B EC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 71 80 3D ? ? ? ? 00 75 68 8B 87 ? ? ? ? 83 F8 FF 74 5D", 0, true };
        DedicatedOffset FindUseEntity{ "FindUseEntity", "server.dll", 0x34E6C0, "55 8B EC B8 80 13 00 00 E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 0F 57 C9 F3 0F 10 45 0C 0F 2F C8 F3 0F 10 55 08", 0, true };
        DedicatedOffset PlayerUse{ "PlayerUse", "server.dll", 0x312AC0, "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B F1 8B 9E B8 1C 00 00 8B 06 8B 90 28 01 00 00", 0, true };
        DedicatedOffset CBaseEntity_GetAbsOrigin_Server{ "CBaseEntity_GetAbsOrigin_Server", "server.dll", 0x28D10, "56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8D 86 ? ? ? ? 5E C3", 0, true };
        DedicatedOffset CBaseEntity_SetOrigin_Server{ "CBaseEntity_SetOrigin_Server", "server.dll", 0x521A0, "55 8B EC 8B 01 8B 55 08 8B 80 ? ? ? ? 6A 00 6A 00 52 FF D0 5D C2 04 00", 0, true };

        bool ResolveAll()
        {
            bool ok = true;
            ok &= ServerFireTerrorBullets.Resolve();
            ok &= ReadUserCmd.Resolve();
            ok &= ProcessUsercmds.Resolve();
            ok &= TestMeleeSwingServer.Resolve();
            ok &= GetPrimaryAttackActivity.Resolve();
            ok &= GetActiveWeapon.Resolve();
            ok &= GetMeleeWeaponInfo.Resolve();
            ok &= EyePosition.Resolve();
            ok &= ServerPlayerEyeAngles.Resolve();
            ok &= ServerPlayerEyePosition.Resolve();
            ok &= FindUseEntity.Resolve();
            ok &= PlayerUse.Resolve();
            ok &= CBaseEntity_GetAbsOrigin_Server.Resolve();
            ok &= CBaseEntity_SetOrigin_Server.Resolve();
            return ok;
        }
    };

    DedicatedOffsets g_offsets;

    template <typename T>
    struct Hook
    {
        T original = nullptr;
        void* target = nullptr;
        const char* name = nullptr;

        bool Create(void* targetFunc, void* detourFunc, const char* hookName, bool required = true)
        {
            name = hookName;
            target = targetFunc;

            if (!targetFunc)
            {
                if (required)
                    Log("hook target missing: %s", hookName);
                return !required;
            }

            const MH_STATUS status = MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<void**>(&original));
            if (status != MH_OK)
            {
                Log("MH_CreateHook failed for %s: %d", hookName, static_cast<int>(status));
                return !required;
            }

            return true;
        }
    };

    Hook<tServerFireTerrorBullets> g_serverFireTerrorBullets;
    Hook<tProcessUsercmds> g_processUsercmds;
    Hook<tReadUsercmd> g_readUsercmd;
    Hook<tTestMeleeSwingCollision> g_testMeleeSwingServer;
    Hook<tGetPrimaryAttackActivity> g_getPrimaryAttackActivity;
    Hook<tEyePosition> g_eyePosition;
    Hook<tEyePosition> g_serverPlayerEyePosition;
    Hook<tEyeAngles> g_serverPlayerEyeAngles;
    Hook<tFindUseEntity> g_findUseEntity;
    Hook<tPlayerUse> g_playerUse;

    Server_WeaponCSBase* SafeGetActiveWeapon(Server_BaseEntity* player)
    {
        if (!player || !g_offsets.GetActiveWeapon.valid)
            return nullptr;

        auto getActiveWeapon = reinterpret_cast<tGetActiveWeapon>(g_offsets.GetActiveWeapon.address);
#ifdef _MSC_VER
        __try
        {
            return getActiveWeapon(player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return getActiveWeapon(player);
#endif
    }

    int SafeGetWeaponId(Server_WeaponCSBase* weapon)
    {
        if (!weapon)
            return 0;

#ifdef _MSC_VER
        __try
        {
            return weapon->GetWeaponID();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
#else
        return weapon->GetWeaponID();
#endif
    }

    void* SafeGetMeleeWeaponInfo(Server_WeaponCSBase* weapon)
    {
        if (!weapon || !g_offsets.GetMeleeWeaponInfo.valid)
            return nullptr;

        auto getInfo = reinterpret_cast<tGetMeleeWeaponInfo>(g_offsets.GetMeleeWeaponInfo.address);
#ifdef _MSC_VER
        __try
        {
            return getInfo(weapon);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return getInfo(weapon);
#endif
    }

    Vector* SafeGetAbsOrigin(Server_BaseEntity* entity)
    {
        if (!entity || !g_offsets.CBaseEntity_GetAbsOrigin_Server.valid)
            return nullptr;

        auto getAbsOrigin = reinterpret_cast<tGetAbsOriginServer>(g_offsets.CBaseEntity_GetAbsOrigin_Server.address);
#ifdef _MSC_VER
        __try
        {
            return getAbsOrigin(entity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return getAbsOrigin(entity);
#endif
    }

    bool SafeSetOrigin(Server_BaseEntity* entity, const Vector& origin)
    {
        if (!entity || !g_offsets.CBaseEntity_SetOrigin_Server.valid || !IsFiniteVector(origin))
            return false;

        auto setOrigin = reinterpret_cast<tSetOriginServer>(g_offsets.CBaseEntity_SetOrigin_Server.address);
#ifdef _MSC_VER
        __try
        {
            setOrigin(entity, origin);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        setOrigin(entity, origin);
        return true;
#endif
    }

    bool IsTeleportServerWalkableFloor(const CGameTrace& trace)
    {
        return !trace.allsolid && !trace.startsolid && trace.fraction < 0.999f;
    }

    bool ValidateServerTeleportLanding(
        Server_BaseEntity* serverPlayer,
        IServerUnknown* serverUnknown,
        const Vector& requestedTarget,
        bool crouched,
        Vector& outStart,
        Vector& outLandingTarget)
    {
        if (!serverPlayer || !serverUnknown || !g_engineTraceServer || !IsFiniteVector(requestedTarget))
            return false;

        Vector* originPtr = SafeGetAbsOrigin(serverPlayer);
        if (!originPtr || !IsFiniteVector(*originPtr))
            return false;

        outStart = *originPtr;
        const Vector requestedDelta = requestedTarget - outStart;
        const float requestedDistance = requestedDelta.Length();
        if (!std::isfinite(requestedDistance) || requestedDistance > g_config.teleportMaxDistanceUnits)
            return false;

        constexpr unsigned int kTeleportLandingStaticTraceMask =
            CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE;
        CTraceFilterSkipSelf filter(serverUnknown, 0);

        Ray_t floorRay;
        floorRay.Init(
            requestedTarget + Vector(0.0f, 0.0f, 4.0f),
            requestedTarget - Vector(0.0f, 0.0f, 256.0f));
        CGameTrace floorTrace{};
        TraceServerRay(floorRay, kTeleportLandingStaticTraceMask, &filter, floorTrace);
        if (!IsTeleportServerWalkableFloor(floorTrace))
            return false;

        const float hullHeight = crouched ? 36.0f : 72.0f;
        const Vector occupancyMins(-14.0f, -14.0f, 1.0f);
        const Vector occupancyMaxs(14.0f, 14.0f, hullHeight - 1.0f);

        Vector landingTarget = floorTrace.endpos + Vector(0.0f, 0.0f, 2.0f);
        if (!IsFiniteVector(landingTarget))
            return false;

        const Vector acceptedDelta = landingTarget - outStart;
        const float acceptedDistance = acceptedDelta.Length();
        if (!std::isfinite(acceptedDistance) || acceptedDistance > g_config.teleportMaxDistanceUnits)
            return false;

        bool occupancyClear = false;
        for (int liftStep = 0; liftStep <= 24; ++liftStep)
        {
            Vector candidate = landingTarget + Vector(0.0f, 0.0f, static_cast<float>(liftStep));
            Ray_t occupancyRay;
            occupancyRay.Init(candidate, candidate, occupancyMins, occupancyMaxs);
            CGameTrace occupancyTrace{};
            TraceServerRay(occupancyRay, kTeleportLandingStaticTraceMask, &filter, occupancyTrace);
            if (!occupancyTrace.startsolid && !occupancyTrace.allsolid && occupancyTrace.fraction >= 0.999f)
            {
                landingTarget = candidate;
                occupancyClear = true;
                break;
            }
        }

        if (!occupancyClear)
            return false;

        outLandingTarget = landingTarget;
        return true;
    }

    bool ApplyServerTeleportMove(Server_BaseEntity* serverPlayer, IServerUnknown* serverUnknown, int playerIndex, const VrExtraCommand& command)
    {
        if (command.type != VrExtraCommandType::Teleport)
            return false;

        if (!g_config.teleportEnabled)
        {
            Log("teleport rejected player=%d reason=disabled", playerIndex);
            return false;
        }

        if (g_config.teleportBlockWhileControlled && IsServerPlayerControlledBySI(serverPlayer))
        {
            Log("teleport rejected player=%d reason=controlled", playerIndex);
            return false;
        }

        if (g_config.teleportBlockWhileIncapacitated && IsServerPlayerIncapacitated(serverPlayer))
        {
            Log("teleport rejected player=%d reason=incapacitated", playerIndex);
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (IsValidPlayerIndex(playerIndex) && g_config.teleportCooldownSeconds > 0.0f)
        {
            PlayerVrState& vrPlayer = g_players[playerIndex];
            if (vrPlayer.lastTeleportTime.time_since_epoch().count() != 0)
            {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - vrPlayer.lastTeleportTime).count();
                const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
                if (elapsedSeconds < g_config.teleportCooldownSeconds)
                {
                    Log("teleport rejected player=%d reason=cooldown remaining=%.2f", playerIndex, g_config.teleportCooldownSeconds - elapsedSeconds);
                    return false;
                }
            }
        }

        Vector start{};
        Vector landingTarget{};
        if (!ValidateServerTeleportLanding(serverPlayer, serverUnknown, command.value, command.crouched, start, landingTarget))
        {
            Log("teleport rejected player=%d target=(%.1f %.1f %.1f)", playerIndex, command.value.x, command.value.y, command.value.z);
            return false;
        }

        if (!SafeSetOrigin(serverPlayer, landingTarget))
            return false;

        if (IsValidPlayerIndex(playerIndex))
            g_players[playerIndex].lastTeleportTime = now;

        Log("teleport applied player=%d start=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f)",
            playerIndex, start.x, start.y, start.z, landingTarget.x, landingTarget.y, landingTarget.z);
        return true;
    }

    bool ApplyServerRoomscaleMove(Server_BaseEntity* serverPlayer, IServerUnknown* serverUnknown, int playerIndex, const VrExtraCommand& command, CUserCmd* move)
    {
        if (command.type != VrExtraCommandType::Roomscale)
            return false;

        if (!serverPlayer || !serverUnknown || !move || !g_engineTraceServer)
            return false;

        Vector worldDelta(command.value.x, command.value.y, 0.0f);
        const float requestedLen = worldDelta.Length();
        if (!std::isfinite(requestedLen) || requestedLen <= 0.01f)
            return false;

        Vector* originPtr = SafeGetAbsOrigin(serverPlayer);
        if (!originPtr || !IsFiniteVector(*originPtr))
            return false;

        const Vector start = *originPtr;
        Vector target = start + worldDelta;
        target.z = start.z;

        const Vector hullMins(-16.0f, -16.0f, 0.0f);
        const Vector hullMaxs(16.0f, 16.0f, command.crouched ? 36.0f : 72.0f);
        constexpr unsigned int kRoomscaleServerMoveMask =
            CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_MONSTER | CONTENTS_GRATE;

        Ray_t ray;
        ray.Init(start, target, hullMins, hullMaxs);
        CTraceFilterSkipSelf filter(serverUnknown, 0);
        CGameTrace trace{};
        TraceServerRay(ray, kRoomscaleServerMoveMask, &filter, trace);

        if (!trace.allsolid && !trace.startsolid && trace.fraction >= 0.999f)
        {
            Vector clippedTarget = trace.endpos;
            clippedTarget.z = start.z;
            if (SafeSetOrigin(serverPlayer, clippedTarget))
                return true;
        }

        constexpr float kFallbackTickSeconds = (1.0f / 30.0f);
        constexpr float kFallbackMaxSpeed = 250.0f;
        Vector fallbackWorldVelocity = worldDelta * (1.0f / kFallbackTickSeconds);
        const float fallbackSpeed = fallbackWorldVelocity.Length();
        if (std::isfinite(fallbackSpeed) && fallbackSpeed > kFallbackMaxSpeed)
            fallbackWorldVelocity *= (kFallbackMaxSpeed / fallbackSpeed);

        QAngle yawOnly(0.0f, move->viewangles.y, 0.0f);
        Vector cmdForward, cmdRight, cmdUp;
        QAngle::AngleVectors(yawOnly, &cmdForward, &cmdRight, &cmdUp);
        Vector worldMove = (cmdForward * move->forwardmove) + (cmdRight * move->sidemove);
        worldMove += fallbackWorldVelocity;
        move->forwardmove = DotProduct(worldMove, cmdForward);
        move->sidemove = DotProduct(worldMove, cmdRight);

        constexpr int kInForward = (1 << 3);
        constexpr int kInBack = (1 << 4);
        constexpr int kInMoveLeft = (1 << 9);
        constexpr int kInMoveRight = (1 << 10);
        if (move->forwardmove > 0.5f) move->buttons |= kInForward;
        else if (move->forwardmove < -0.5f) move->buttons |= kInBack;
        if (move->sidemove > 0.5f) move->buttons |= kInMoveRight;
        else if (move->sidemove < -0.5f) move->buttons |= kInMoveLeft;

        Log("roomscale fallback injected player=%d delta=(%.1f %.1f) move=(%.1f %.1f)",
            playerIndex, worldDelta.x, worldDelta.y, move->forwardmove, move->sidemove);
        return true;
    }

    bool IsThrowableWeapon(int weaponId)
    {
        return weaponId == kWeaponMolotov || weaponId == kWeaponPipeBomb || weaponId == kWeaponVomitJar;
    }

    bool IsCurrentServerPlayerUsingMountedWeapon()
    {
        if (!g_currentUsercmdPlayer)
            return false;

#ifdef _MSC_VER
        __try
        {
            const auto* base = reinterpret_cast<const unsigned char*>(g_currentUsercmdPlayer);
            return base[kUsingMountedGunOffset] != 0 || base[kUsingMountedWeaponOffset] != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        const auto* base = reinterpret_cast<const unsigned char*>(g_currentUsercmdPlayer);
        return base[kUsingMountedGunOffset] != 0 || base[kUsingMountedWeaponOffset] != 0;
#endif
    }

    bool TryGetCurrentServerWeapon(Server_WeaponCSBase*& weapon, int& weaponId)
    {
        weapon = SafeGetActiveWeapon(g_currentUsercmdPlayer);
        weaponId = SafeGetWeaponId(weapon);
        return weapon != nullptr;
    }

    bool TryBuildServerUseControllerPose(void* player, Vector& origin, QAngle& angles)
    {
        if (!player || !g_currentUsercmdPlayer || player != g_currentUsercmdPlayer)
            return false;

        if (!IsValidPlayerIndex(g_currentUsercmdPlayerIndex))
            return false;

        const PlayerVrState& vrPlayer = g_players[g_currentUsercmdPlayerIndex];
        if (!vrPlayer.isUsingVR)
            return false;

        origin = vrPlayer.controllerPos;
        angles = vrPlayer.controllerAngle;
        NormalizeAndClampViewAngles(angles);
        return IsFiniteVector(origin) && IsFiniteAngles(angles);
    }

    bool TryGetServerControllerAimOverride(void* player, Vector& origin, QAngle& angles, int& reason)
    {
        if (!player)
            return false;

        if (g_useControllerAimOverride && player == g_useControllerAimPlayer)
        {
            origin = g_useControllerAimOrigin;
            angles = g_useControllerAimAngles;
            reason = 1;
            return IsFiniteVector(origin) && IsFiniteAngles(angles);
        }

        if (g_commandControllerAimOverride && player == g_commandControllerAimPlayer)
        {
            origin = g_commandControllerAimOrigin;
            angles = g_commandControllerAimAngles;
            reason = g_commandControllerAimReason;
            return IsFiniteVector(origin) && IsFiniteAngles(angles);
        }

        return false;
    }

    class ScopedServerUseControllerAimOverride
    {
    public:
        ScopedServerUseControllerAimOverride(void* player, const Vector& origin, const QAngle& angles)
            : m_prevActive(g_useControllerAimOverride)
            , m_prevPlayer(g_useControllerAimPlayer)
            , m_prevOrigin(g_useControllerAimOrigin)
            , m_prevAngles(g_useControllerAimAngles)
        {
            g_useControllerAimOverride = true;
            g_useControllerAimPlayer = player;
            g_useControllerAimOrigin = origin;
            g_useControllerAimAngles = angles;
        }

        ~ScopedServerUseControllerAimOverride()
        {
            g_useControllerAimOverride = m_prevActive;
            g_useControllerAimPlayer = m_prevPlayer;
            g_useControllerAimOrigin = m_prevOrigin;
            g_useControllerAimAngles = m_prevAngles;
        }

    private:
        bool m_prevActive;
        void* m_prevPlayer;
        Vector m_prevOrigin;
        QAngle m_prevAngles;
    };

    int __cdecl DetourServerFireTerrorBullets(int playerId, const Vector& origin, const QAngle& angles, int a4, int a5, int a6, float a7)
    {
        Vector correctedOrigin = origin;
        QAngle correctedAngles = angles;

        if (IsValidPlayerIndex(playerId))
        {
            const PlayerVrState& player = g_players[playerId];
            if (player.isUsingVR && IsFiniteVector(player.controllerPos) && IsFiniteAngles(player.controllerAngle))
            {
                correctedOrigin = player.controllerPos;
                correctedAngles = player.controllerAngle;
                NormalizeAndClampViewAngles(correctedAngles);
            }
        }

        return g_serverFireTerrorBullets.original(playerId, correctedOrigin, correctedAngles, a4, a5, a6, a7);
    }

    int __cdecl DetourReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from)
    {
        static thread_local uintptr_t s_throwableAimWeapon = 0;
        static thread_local bool s_prevAttackDown = false;
        static thread_local bool s_prevWeaponThrowable = false;
        static thread_local int s_throwableAimTicks = 0;

        g_commandControllerAimOverride = false;
        g_commandControllerAimPlayer = nullptr;
        g_commandControllerAimReason = 0;

        const int originalResult = g_readUsercmd.original(buf, move, from);
        if (!move)
            return originalResult;

        VrExtraCommand extraCommand{};
        const bool hasExtraCommand = DecodeVrExtraCommand(move, extraCommand);
        ClearVrExtraCommandFields(move);

        const int playerIndex = g_currentUsercmdPlayerIndex;
        const bool hasValidPlayer = IsValidPlayerIndex(playerIndex);
        if (move->tick_count < 0)
        {
            move->tick_count *= -1;

            if (move->command_number < 0)
            {
                move->command_number *= -1;
                if (hasValidPlayer)
                    g_players[playerIndex].isMeleeing = true;
            }
            else if (hasValidPlayer)
            {
                g_players[playerIndex].isMeleeing = false;
                g_players[playerIndex].currentMeleeSwingAllowed = false;
            }

            int rollEncoding = move->command_number / 10000000;
            move->command_number -= rollEncoding * 10000000;

            const int decodedZInt = static_cast<int>(move->viewangles.x / 10000.0f);
            float decodedPitch = std::fabs((move->viewangles.x - static_cast<float>(decodedZInt * 10000)) / 10.0f);
            decodedPitch -= 360.0f;
            const float decodedZ = static_cast<float>(decodedZInt) / 10.0f;

            if (hasValidPlayer)
            {
                PlayerVrState& vrPlayer = g_players[playerIndex];
                vrPlayer.isUsingVR = true;
                vrPlayer.physicalCrouch = hasExtraCommand ? extraCommand.crouched : false;
                vrPlayer.controllerAngle.x = static_cast<float>(move->mousedx) / 10.0f;
                vrPlayer.controllerAngle.y = static_cast<float>(move->mousedy) / 10.0f;
                vrPlayer.controllerAngle.z = static_cast<float>((rollEncoding * 2) - 180);
                vrPlayer.controllerPos.x = move->viewangles.z;
                vrPlayer.controllerPos.y = move->upmove;
                vrPlayer.controllerPos.z = decodedZ;
                NormalizeAndClampViewAngles(vrPlayer.controllerAngle);

                if (!IsFiniteVector(vrPlayer.controllerPos) || !IsFiniteAngles(vrPlayer.controllerAngle))
                {
                    vrPlayer.isUsingVR = false;
                    vrPlayer.isMeleeing = false;
                    vrPlayer.currentMeleeSwingAllowed = false;
                }
            }

            move->viewangles.x = decodedPitch;
            move->viewangles.z = 0.0f;
            move->upmove = 0.0f;

            if (hasExtraCommand && hasValidPlayer)
            {
                if (extraCommand.type == VrExtraCommandType::Teleport)
                    ApplyServerTeleportMove(g_currentUsercmdPlayer, g_currentUsercmdUnknown, playerIndex, extraCommand);
                else if (extraCommand.type == VrExtraCommandType::Roomscale)
                    ApplyServerRoomscaleMove(g_currentUsercmdPlayer, g_currentUsercmdUnknown, playerIndex, extraCommand, move);
            }

            Server_WeaponCSBase* serverWeapon = nullptr;
            int serverWeaponId = 0;
            TryGetCurrentServerWeapon(serverWeapon, serverWeaponId);
            const uintptr_t weaponTag = reinterpret_cast<uintptr_t>(serverWeapon);
            if (weaponTag != s_throwableAimWeapon)
            {
                if (s_prevWeaponThrowable && s_prevAttackDown)
                    s_throwableAimTicks = std::max(s_throwableAimTicks, 48);
                s_throwableAimWeapon = weaponTag;
                s_prevAttackDown = false;
                s_prevWeaponThrowable = false;
            }

            constexpr int kInAttack = (1 << 0);
            const bool attackDown = (move->buttons & kInAttack) != 0;
            const bool activeWeaponIsThrowable = IsThrowableWeapon(serverWeaponId);
            bool commandControllerAim = false;
            int commandControllerAimReason = 0;

            if (activeWeaponIsThrowable)
            {
                if (attackDown || (s_prevAttackDown && !attackDown))
                    s_throwableAimTicks = std::max(s_throwableAimTicks, 48);
                commandControllerAim = attackDown || (s_throwableAimTicks > 0);
                commandControllerAimReason = attackDown ? 2 : 3;
                if (s_throwableAimTicks > 0)
                    --s_throwableAimTicks;
            }
            else
            {
                commandControllerAim = s_throwableAimTicks > 0;
                commandControllerAimReason = 3;
                if (s_throwableAimTicks > 0)
                    --s_throwableAimTicks;
            }

            s_prevAttackDown = activeWeaponIsThrowable && attackDown;
            s_prevWeaponThrowable = activeWeaponIsThrowable;

            if (IsCurrentServerPlayerUsingMountedWeapon())
            {
                commandControllerAim = true;
                commandControllerAimReason = 4;
            }

            if (commandControllerAim && hasValidPlayer && g_players[playerIndex].isUsingVR)
            {
                const PlayerVrState& vrPlayer = g_players[playerIndex];
                g_commandControllerAimOverride = true;
                g_commandControllerAimPlayer = g_currentUsercmdPlayer;
                g_commandControllerAimOrigin = vrPlayer.controllerPos;
                g_commandControllerAimAngles = vrPlayer.controllerAngle;
                NormalizeAndClampViewAngles(g_commandControllerAimAngles);
                g_commandControllerAimReason = commandControllerAimReason;
            }
        }
        else if (hasValidPlayer)
        {
            g_players[playerIndex].isUsingVR = false;
            g_players[playerIndex].isMeleeing = false;
            g_players[playerIndex].currentMeleeSwingAllowed = false;
        }

        return originalResult;
    }

    int CountRecentMeleeSwings(const PlayerVrState& vrPlayer, std::chrono::steady_clock::time_point now)
    {
        if (g_config.meleeSwingBurstWindowSeconds <= 0.0f || g_config.meleeSwingBurstMax <= 0)
            return 0;

        const auto windowMs = std::chrono::milliseconds(
            static_cast<int>(g_config.meleeSwingBurstWindowSeconds * 1000.0f));
        int count = 0;
        for (const auto& swingTime : vrPlayer.meleeSwingHistory)
        {
            if (swingTime.time_since_epoch().count() != 0 && now - swingTime <= windowMs)
                ++count;
        }
        return count;
    }

    void RecordMeleeSwing(PlayerVrState& vrPlayer, std::chrono::steady_clock::time_point now)
    {
        vrPlayer.lastMeleeSwingTime = now;

        size_t writeIndex = 0;
        for (size_t i = 0; i < vrPlayer.meleeSwingHistory.size(); ++i)
        {
            if (vrPlayer.meleeSwingHistory[i].time_since_epoch().count() == 0)
            {
                writeIndex = i;
                break;
            }

            if (vrPlayer.meleeSwingHistory[i] < vrPlayer.meleeSwingHistory[writeIndex])
                writeIndex = i;
        }

        vrPlayer.meleeSwingHistory[writeIndex] = now;
    }

    bool CanStartMeleeSwing(Server_BaseEntity* player, int playerIndex, PlayerVrState& vrPlayer)
    {
        if (!g_config.meleeSwingEnabled)
        {
            Log("melee swing rejected player=%d reason=disabled", playerIndex);
            return false;
        }

        if (g_config.meleeSwingBlockWhileControlled && IsServerPlayerControlledBySI(player))
        {
            Log("melee swing rejected player=%d reason=controlled", playerIndex);
            return false;
        }

        if (g_config.meleeSwingBlockWhileIncapacitated && IsServerPlayerIncapacitated(player))
        {
            Log("melee swing rejected player=%d reason=incapacitated", playerIndex);
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (g_config.meleeSwingCooldownSeconds > 0.0f &&
            vrPlayer.lastMeleeSwingTime.time_since_epoch().count() != 0)
        {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - vrPlayer.lastMeleeSwingTime).count();
            const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
            if (elapsedSeconds < g_config.meleeSwingCooldownSeconds)
            {
                Log("melee swing rejected player=%d reason=cooldown remaining=%.2f", playerIndex, g_config.meleeSwingCooldownSeconds - elapsedSeconds);
                return false;
            }
        }

        if (g_config.meleeSwingBurstWindowSeconds > 0.0f &&
            g_config.meleeSwingBurstMax > 0 &&
            CountRecentMeleeSwings(vrPlayer, now) >= g_config.meleeSwingBurstMax)
        {
            Log("melee swing rejected player=%d reason=burst-limit window=%.2f max=%d",
                playerIndex,
                g_config.meleeSwingBurstWindowSeconds,
                g_config.meleeSwingBurstMax);
            return false;
        }

        RecordMeleeSwing(vrPlayer, now);
        return true;
    }

    void ApplyVrMeleeTrace(Server_BaseEntity* player, int playerIndex)
    {
        if (!player || !IsValidPlayerIndex(playerIndex))
            return;

        PlayerVrState& vrPlayer = g_players[playerIndex];
        if (!vrPlayer.isUsingVR || !vrPlayer.isMeleeing)
        {
            vrPlayer.isNewSwing = true;
            vrPlayer.currentMeleeSwingAllowed = false;
            return;
        }

        Server_WeaponCSBase* weapon = SafeGetActiveWeapon(player);
        if (!weapon || SafeGetWeaponId(weapon) != kWeaponMelee)
        {
            vrPlayer.currentMeleeSwingAllowed = false;
            return;
        }

        if (vrPlayer.isNewSwing)
        {
            vrPlayer.isNewSwing = false;
            vrPlayer.currentMeleeSwingAllowed = CanStartMeleeSwing(player, playerIndex, vrPlayer);
            if (!vrPlayer.currentMeleeSwingAllowed)
                return;

            weapon->entitiesHitThisSwing = 0;
        }

        if (!vrPlayer.currentMeleeSwingAllowed)
            return;

        if ((g_config.meleeSwingBlockWhileControlled && IsServerPlayerControlledBySI(player)) ||
            (g_config.meleeSwingBlockWhileIncapacitated && IsServerPlayerIncapacitated(player)))
        {
            vrPlayer.currentMeleeSwingAllowed = false;
            Log("melee swing stopped player=%d reason=state-changed", playerIndex);
            return;
        }

        void* meleeInfo = SafeGetMeleeWeaponInfo(weapon);
        Vector initialForward, initialRight, initialUp;
        QAngle::AngleVectors(vrPlayer.prevControllerAngle, &initialForward, &initialRight, &initialUp);
        Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0f);
        VectorNormalize(initialMeleeDirection);

        Vector finalForward, finalRight, finalUp;
        QAngle::AngleVectors(vrPlayer.controllerAngle, &finalForward, &finalRight, &finalUp);
        Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0f);
        VectorNormalize(finalMeleeDirection);

        Vector pivot;
        CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
        if (VectorNormalize(pivot) <= 0.0001f)
            return;

        const float dot = std::clamp(DotProduct(initialMeleeDirection, finalMeleeDirection), -1.0f, 1.0f);
        const float swingAngle = acosf(dot) * 180.0f / 3.14159265f;
        if (!std::isfinite(swingAngle) || swingAngle <= 0.01f)
            return;

        g_getPrimaryAttackActivity.original(weapon, meleeInfo);

        g_performingMeleeTrace = true;
        Vector traceDirection = initialMeleeDirection;
        constexpr int kNumTraces = 10;
        const float traceAngle = swingAngle / static_cast<float>(kNumTraces);
        for (int i = 0; i < kNumTraces; ++i)
        {
            traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
            g_testMeleeSwingServer.original(weapon, traceDirection);
        }
        g_performingMeleeTrace = false;
    }

    float __fastcall DetourProcessUsercmds(void* ecx, void* edx, edict_t* player, void* buf, int numcmds, int totalcmds, int droppedPackets, bool ignore, bool paused)
    {
        g_commandControllerAimOverride = false;
        g_commandControllerAimPlayer = nullptr;
        g_commandControllerAimReason = 0;

        const int previousIndex = g_currentUsercmdPlayerIndex;
        Server_BaseEntity* const previousPlayer = g_currentUsercmdPlayer;
        IServerUnknown* const previousUnknown = g_currentUsercmdUnknown;

        g_currentUsercmdPlayerIndex = player ? static_cast<int>(player->m_EdictIndex) : -1;
        g_currentUsercmdUnknown = player ? player->m_pUnk : nullptr;
        g_currentUsercmdPlayer = GetBaseEntityFromEdict(player);

        const float result = g_processUsercmds.original(ecx, player, buf, numcmds, totalcmds, droppedPackets, ignore, paused);

        g_commandControllerAimOverride = false;
        g_commandControllerAimPlayer = nullptr;
        g_commandControllerAimReason = 0;

        ApplyVrMeleeTrace(g_currentUsercmdPlayer, g_currentUsercmdPlayerIndex);

        if (IsValidPlayerIndex(g_currentUsercmdPlayerIndex))
            g_players[g_currentUsercmdPlayerIndex].prevControllerAngle = g_players[g_currentUsercmdPlayerIndex].controllerAngle;

        g_currentUsercmdPlayerIndex = previousIndex;
        g_currentUsercmdPlayer = previousPlayer;
        g_currentUsercmdUnknown = previousUnknown;
        return result;
    }

    int __fastcall DetourTestMeleeSwingCollisionServer(void* ecx, void* edx, Vector const& direction)
    {
        return g_testMeleeSwingServer.original(ecx, direction);
    }

    int __fastcall DetourGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo)
    {
        return g_getPrimaryAttackActivity.original(ecx, meleeInfo);
    }

    Vector* __fastcall DetourEyePosition(void* ecx, void* edx, Vector* eyePos)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (eyePos && TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
        {
            *eyePos = controllerOrigin;
            return eyePos;
        }

        Vector* result = g_eyePosition.original(ecx, eyePos);
        if (g_performingMeleeTrace && result && IsValidPlayerIndex(g_currentUsercmdPlayerIndex))
            *result = g_players[g_currentUsercmdPlayerIndex].controllerPos;

        return result;
    }

    Vector* __fastcall DetourServerPlayerEyePosition(void* ecx, void* edx, Vector* eyePos)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (eyePos && TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
        {
            *eyePos = controllerOrigin;
            return eyePos;
        }

        return g_serverPlayerEyePosition.original(ecx, eyePos);
    }

    const QAngle* __fastcall DetourServerPlayerEyeAngles(void* ecx, void* edx)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
        {
            g_returnEyeAngles = controllerAngles;
            NormalizeAndClampViewAngles(g_returnEyeAngles);
            return &g_returnEyeAngles;
        }

        return g_serverPlayerEyeAngles.original(ecx);
    }

    void __fastcall DetourPlayerUse(void* ecx, void* edx, void* useEntity)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        if (TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
        {
            ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
            g_playerUse.original(ecx, useEntity);
            return;
        }

        g_playerUse.original(ecx, useEntity);
    }

    Server_BaseEntity* __fastcall DetourFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        if (TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
        {
            ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
            return g_findUseEntity.original(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
        }

        return g_findUseEntity.original(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
    }

    bool WaitForModule(const char* moduleName, DWORD timeoutMs)
    {
        const DWORD start = GetTickCount();
        while (!g_unloading.load())
        {
            if (GetModuleHandleA(moduleName))
                return true;

            if (GetTickCount() - start >= timeoutMs)
                return false;

            Sleep(100);
        }

        return false;
    }

    bool InstallHooks()
    {
        bool expected = false;
        if (!g_hooksInstalled.compare_exchange_strong(expected, true))
            return true;

        if (!WaitForModule("server.dll", 60000))
        {
            Log("server.dll did not load before timeout; dedicated hooks not installed");
            g_hooksInstalled.store(false);
            return false;
        }

        if (!g_offsets.ResolveAll())
        {
            Log("dedicated offset resolution failed; hooks not installed");
            g_hooksInstalled.store(false);
            return false;
        }

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            Log("MH_Initialize failed: %d", static_cast<int>(initStatus));
            g_hooksInstalled.store(false);
            return false;
        }

        bool ok = true;
        ok &= g_serverFireTerrorBullets.Create(reinterpret_cast<void*>(g_offsets.ServerFireTerrorBullets.address), reinterpret_cast<void*>(&DetourServerFireTerrorBullets), "ServerFireTerrorBullets");
        ok &= g_processUsercmds.Create(reinterpret_cast<void*>(g_offsets.ProcessUsercmds.address), reinterpret_cast<void*>(&DetourProcessUsercmds), "ProcessUsercmds");
        ok &= g_readUsercmd.Create(reinterpret_cast<void*>(g_offsets.ReadUserCmd.address), reinterpret_cast<void*>(&DetourReadUsercmd), "ReadUsercmd");
        ok &= g_testMeleeSwingServer.Create(reinterpret_cast<void*>(g_offsets.TestMeleeSwingServer.address), reinterpret_cast<void*>(&DetourTestMeleeSwingCollisionServer), "TestMeleeSwingServer");
        ok &= g_getPrimaryAttackActivity.Create(reinterpret_cast<void*>(g_offsets.GetPrimaryAttackActivity.address), reinterpret_cast<void*>(&DetourGetPrimaryAttackActivity), "GetPrimaryAttackActivity");
        ok &= g_eyePosition.Create(reinterpret_cast<void*>(g_offsets.EyePosition.address), reinterpret_cast<void*>(&DetourEyePosition), "EyePosition");
        ok &= g_serverPlayerEyePosition.Create(reinterpret_cast<void*>(g_offsets.ServerPlayerEyePosition.address), reinterpret_cast<void*>(&DetourServerPlayerEyePosition), "ServerPlayerEyePosition", false);
        ok &= g_serverPlayerEyeAngles.Create(reinterpret_cast<void*>(g_offsets.ServerPlayerEyeAngles.address), reinterpret_cast<void*>(&DetourServerPlayerEyeAngles), "ServerPlayerEyeAngles", false);
        ok &= g_findUseEntity.Create(reinterpret_cast<void*>(g_offsets.FindUseEntity.address), reinterpret_cast<void*>(&DetourFindUseEntity), "FindUseEntity", false);
        ok &= g_playerUse.Create(reinterpret_cast<void*>(g_offsets.PlayerUse.address), reinterpret_cast<void*>(&DetourPlayerUse), "PlayerUse", false);

        if (!ok)
        {
            Log("required hook creation failed; rolling back");
            MH_Uninitialize();
            g_hooksInstalled.store(false);
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK)
        {
            Log("MH_EnableHook failed: %d", static_cast<int>(enableStatus));
            MH_Uninitialize();
            g_hooksInstalled.store(false);
            return false;
        }

        Log("L4D2VR dedicated server hooks installed");
        return true;
    }

    void UninstallHooks()
    {
        if (!g_hooksInstalled.exchange(false))
            return;

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        Log("L4D2VR dedicated server hooks uninstalled");
    }

    DWORD WINAPI HookWorker(LPVOID)
    {
        InstallHooks();
        return 0;
    }

    void StartHookWorker()
    {
        bool expected = false;
        if (!g_hookWorkerStarted.compare_exchange_strong(expected, true))
            return;

        HANDLE thread = CreateThread(nullptr, 0, HookWorker, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
            return;
        }

        Log("CreateThread failed for hook worker; installing synchronously");
        InstallHooks();
    }

    using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);
    using QueryCvarCookie_t = int;

    enum
    {
        IFACE_OK = 0,
        IFACE_FAILED = 1,
    };

    enum PLUGIN_RESULT
    {
        PLUGIN_CONTINUE = 0,
        PLUGIN_OVERRIDE = 1,
        PLUGIN_STOP = 2,
    };

    enum EQueryCvarValueStatus
    {
        eQueryCvarValueStatus_ValueIntact = 0,
        eQueryCvarValueStatus_CvarNotFound = 1,
        eQueryCvarValueStatus_NotACvar = 2,
        eQueryCvarValueStatus_CvarProtected = 3,
    };

    class CCommand;

    class IServerPluginCallbacks
    {
    public:
        virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) = 0;
        virtual void Unload() = 0;
        virtual void Pause() = 0;
        virtual void UnPause() = 0;
        virtual const char* GetPluginDescription() = 0;
        virtual void LevelInit(const char* mapName) = 0;
        virtual void ServerActivate(edict_t* edictList, int edictCount, int clientMax) = 0;
        virtual void GameFrame(bool simulating) = 0;
        virtual void LevelShutdown() = 0;
        virtual void ClientActive(edict_t* entity) = 0;
        virtual void ClientDisconnect(edict_t* entity) = 0;
        virtual void ClientPutInServer(edict_t* entity, const char* playerName) = 0;
        virtual void SetCommandClient(int index) = 0;
        virtual void ClientSettingsChanged(edict_t* edict) = 0;
        virtual PLUGIN_RESULT ClientConnect(bool* allowConnect, edict_t* entity, const char* name, const char* address, char* reject, int maxRejectLen) = 0;
        virtual PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& args) = 0;
        virtual PLUGIN_RESULT NetworkIDValidated(const char* userName, const char* networkId) = 0;
        virtual void OnQueryCvarValueFinished(QueryCvarCookie_t cookie, edict_t* playerEntity, EQueryCvarValueStatus status, const char* cvarName, const char* cvarValue) = 0;
        virtual void OnEdictAllocated(edict_t* edict) = 0;
        virtual void OnEdictFreed(const edict_t* edict) = 0;
    };

    class L4D2VRServerPlugin final : public IServerPluginCallbacks
    {
    public:
        bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn) override
        {
            ResetLogFile();
            LoadServerConfig();

            g_unloading.store(false);
            g_engineTraceServer = nullptr;
            g_engineServer = nullptr;
            if (interfaceFactory)
            {
                int returnCode = IFACE_FAILED;
                g_engineServer = static_cast<IVEngineServer*>(interfaceFactory("VEngineServer022", &returnCode));
                if (!g_engineServer)
                    g_engineServer = static_cast<IVEngineServer*>(interfaceFactory("VEngineServer021", &returnCode));
                if (!g_engineServer)
                    g_engineServer = static_cast<IVEngineServer*>(interfaceFactory("VEngineServer023", &returnCode));
                g_engineTraceServer = static_cast<IEngineTrace*>(interfaceFactory("EngineTraceServer003", &returnCode));
                if (!g_engineTraceServer)
                    g_engineTraceServer = static_cast<IEngineTrace*>(interfaceFactory("EngineTraceClient003", &returnCode));
            }

            if (!g_engineServer)
                Log("VEngineServer022 unavailable; L4D2VR client ack handshake will be disabled");
            if (!g_engineTraceServer)
                Log("EngineTraceServer003 unavailable; teleport and roomscale server movement will be disabled until it resolves");
            Log("L4D2VR dedicated server plugin loaded");
            StartHookWorker();
            return true;
        }

        void Unload() override
        {
            g_unloading.store(true);
            UninstallHooks();
            g_engineServer = nullptr;
            Log("L4D2VR dedicated server plugin unloaded");
        }

        void Pause() override {}
        void UnPause() override {}

        const char* GetPluginDescription() override
        {
            return "L4D2VR dedicated server usercmd decoder";
        }

        void LevelInit(const char*) override
        {
            LoadServerConfig();
            for (PlayerVrState& player : g_players)
                player = PlayerVrState{};
        }

        void ServerActivate(edict_t*, int, int) override {}
        void GameFrame(bool) override
        {
            if (!g_hooksInstalled.load() && !g_unloading.load())
                StartHookWorker();
            PumpPendingServerAcks();
        }
        void LevelShutdown() override {}
        void ClientActive(edict_t* entity) override
        {
            QueueServerAckForClient(entity, "ClientActive");
        }
        void ClientDisconnect(edict_t* entity) override
        {
            const int playerIndex = GetPlayerIndexFromEdict(entity);
            if (IsValidPlayerIndex(playerIndex))
                g_players[playerIndex] = PlayerVrState{};
        }
        void ClientPutInServer(edict_t* entity, const char*) override
        {
            QueueServerAckForClient(entity, "ClientPutInServer");
        }
        void SetCommandClient(int) override {}
        void ClientSettingsChanged(edict_t*) override {}
        PLUGIN_RESULT ClientConnect(bool*, edict_t*, const char*, const char*, char*, int) override { return PLUGIN_CONTINUE; }
        PLUGIN_RESULT ClientCommand(edict_t*, const CCommand&) override { return PLUGIN_CONTINUE; }
        PLUGIN_RESULT NetworkIDValidated(const char*, const char*) override { return PLUGIN_CONTINUE; }
        void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t*, EQueryCvarValueStatus, const char*, const char*) override {}
        void OnEdictAllocated(edict_t*) override {}
        void OnEdictFreed(const edict_t*) override {}
    };

    L4D2VRServerPlugin g_plugin;
}

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* returnCode)
{
    if (name && std::strcmp(name, "ISERVERPLUGINCALLBACKS003") == 0)
    {
        if (returnCode)
            *returnCode = IFACE_OK;
        return &g_plugin;
    }

    if (returnCode)
        *returnCode = IFACE_FAILED;
    return nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_unloading.store(true);
    }

    return TRUE;
}
