#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

class Server_BaseEntity;

struct Vector
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector() = default;
    Vector(float xValue, float yValue, float zValue)
        : x(xValue), y(yValue), z(zValue)
    {
    }

    Vector operator+(const Vector& other) const { return Vector(x + other.x, y + other.y, z + other.z); }
    Vector operator-(const Vector& other) const { return Vector(x - other.x, y - other.y, z - other.z); }
    Vector operator*(float value) const { return Vector(x * value, y * value, z * value); }
    Vector& operator+=(const Vector& other)
    {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }
    Vector& operator*=(float value)
    {
        x *= value;
        y *= value;
        z *= value;
        return *this;
    }
    float LengthSqr() const { return x * x + y * y + z * z; }
    float Length() const { return std::sqrt(LengthSqr()); }
};

struct QAngle
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    QAngle() = default;
    QAngle(float pitch, float yaw, float roll)
        : x(pitch), y(yaw), z(roll)
    {
    }

    static void AngleVectors(const QAngle& angles, Vector* forward, Vector* right, Vector* up)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float sp = std::sin(angles.x * kDegToRad);
        const float cp = std::cos(angles.x * kDegToRad);
        const float sy = std::sin(angles.y * kDegToRad);
        const float cy = std::cos(angles.y * kDegToRad);
        const float sr = std::sin(angles.z * kDegToRad);
        const float cr = std::cos(angles.z * kDegToRad);

        if (forward)
            *forward = Vector(cp * cy, cp * sy, -sp);
        if (right)
            *right = Vector((-sr * sp * cy) + (-cr * -sy), (-sr * sp * sy) + (-cr * cy), -sr * cp);
        if (up)
            *up = Vector((cr * sp * cy) + (-sr * -sy), (cr * sp * sy) + (-sr * cy), cr * cp);
    }
};

struct alignas(16) VectorAligned
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    VectorAligned() = default;
    explicit VectorAligned(const Vector& value)
        : x(value.x), y(value.y), z(value.z), w(0.0f)
    {
    }
};

static_assert(sizeof(Vector) == 12, "Unexpected Vector layout.");
static_assert(sizeof(QAngle) == 12, "Unexpected QAngle layout.");
static_assert(sizeof(VectorAligned) == 16, "Unexpected VectorAligned layout.");

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

class matrix3x4_t;

struct cplane_t
{
    Vector normal;
    float dist = 0.0f;
    unsigned char type = 0;
    unsigned char signbits = 0;
    unsigned char pad[2] = {};
};

struct csurface_t
{
    const char* name = nullptr;
    short surfaceProps = 0;
    unsigned short flags = 0;
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
        const Vector delta = end - start;
        m_Delta = VectorAligned(delta);
        m_IsSwept = (delta.LengthSqr() != 0.0f);
        m_Extents = VectorAligned(Vector(0.0f, 0.0f, 0.0f));
        m_IsRay = true;
        m_pWorldAxisTransform = nullptr;
        m_StartOffset = VectorAligned(Vector(0.0f, 0.0f, 0.0f));
        m_Start = VectorAligned(start);
    }

    void Init(const Vector& start, const Vector& end, const Vector& mins, const Vector& maxs)
    {
        const Vector delta = end - start;
        const Vector extents = (maxs - mins) * 0.5f;
        const Vector offset = (mins + maxs) * 0.5f;
        m_Delta = VectorAligned(delta);
        m_IsSwept = (delta.LengthSqr() != 0.0f);
        m_Extents = VectorAligned(extents);
        m_IsRay = (extents.LengthSqr() < 1e-6f);
        m_pWorldAxisTransform = nullptr;
        m_StartOffset = VectorAligned(offset * -1.0f);
        m_Start = VectorAligned(start + offset);
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
    static_assert(sizeof(void*) == 4, "L4D2VR Linux dedicated server plugin must be built as 32-bit/x86.");

    constexpr int kMaxPlayers = 65;
    constexpr int kWeaponMelee = 19;
    constexpr int kWeaponMolotov = 13;
    constexpr int kWeaponPipeBomb = 14;
    constexpr int kWeaponVomitJar = 25;
    constexpr int kUsingMountedGunOffset = 0x1EBA;
    constexpr int kUsingMountedWeaponOffset = 0x1EBB;
    constexpr int kMeleeEntitiesHitThisSwingOffset = 0x17F0;
    constexpr int kWeaponIdVirtualIndex = 398;
    constexpr int kVrExtraCommandMagic = 0x56000000;
    constexpr int kVrExtraCommandMagicMask = 0xFF000000;
    constexpr int kVrExtraTypeShift = 20;
    constexpr int kVrExtraFlagsShift = 16;
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
    constexpr char kServerModuleName[] = "server_srv.so";

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

    using tServerFireTerrorBullets = int (*)(int playerId, const Vector& origin, const QAngle& angles, int weaponId, int a5, int a6, float a7);
    using tProcessUsercmds = float (*)(void* thisptr, edict_t* player, void* buf, int numcmds, int totalcmds, int droppedPackets, bool ignore, bool paused);
    using tReadUsercmd = int (*)(void* buf, CUserCmd* move, CUserCmd* from);
    using tTestMeleeSwingCollision = int (*)(void* thisptr, const Vector& direction);
    using tGetPrimaryAttackActivity = int (*)(void* thisptr, void* meleeInfo);
    using tEyePosition = Vector* (*)(Vector* out, void* thisptr);
    using tEyeAngles = const QAngle* (*)(void* thisptr);
    using tFindUseEntity = Server_BaseEntity* (*)(void* thisptr, float radius, float dotLimit, float defaultDotLimit, void* traceResult, bool unknown1, bool unknown2);
    using tPlayerUse = void (*)(void* thisptr, void* useEntity);
    using tGetActiveWeapon = void* (*)(void* thisptr);
    using tGetMeleeWeaponInfo = void* (*)(void* thisptr);
    using tGetAbsOriginServer = const Vector* (*)(void* thisptr);
    using tSetOriginServer = void (*)(void* entity, const Vector& origin, bool fireTriggers);

    std::mutex g_logMutex;
    std::array<PlayerVrState, kMaxPlayers> g_players{};
    std::atomic_bool g_hooksInstalled{ false };
    std::atomic_bool g_hookWorkerStarted{ false };
    std::atomic_bool g_unloading{ false };
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

    float DotProduct(const Vector& a, const Vector& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    void CrossProduct(const Vector& a, const Vector& b, Vector& out)
    {
        out.x = (a.y * b.z) - (a.z * b.y);
        out.y = (a.z * b.x) - (a.x * b.z);
        out.z = (a.x * b.y) - (a.y * b.x);
    }

    float VectorNormalize(Vector& value)
    {
        const float length = value.Length();
        if (length > 0.000001f)
        {
            const float inv = 1.0f / length;
            value *= inv;
        }
        return length;
    }

    Vector VectorRotate(const Vector& value, const Vector& axis, float degrees)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        Vector normalizedAxis = axis;
        if (VectorNormalize(normalizedAxis) <= 0.000001f)
            return value;

        const float radians = degrees * kDegToRad;
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        Vector cross;
        CrossProduct(normalizedAxis, value, cross);
        return (value * c) + (cross * s) + (normalizedAxis * (DotProduct(normalizedAxis, value) * (1.0f - c)));
    }

    void ResetLogFile()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        FILE* file = std::fopen(kServerLogFileName, "w");
        if (file)
        {
            std::fputs("L4D2VR Linux dedicated server log\n", file);
            std::fclose(file);
        }
    }

    void Log(const char* fmt, ...)
    {
        char message[2048] = {};
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);

        std::time_t now = std::time(nullptr);
        std::tm localTime{};
        localtime_r(&now, &localTime);

        char line[2300] = {};
        std::snprintf(line, sizeof(line),
            "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            localTime.tm_year + 1900,
            localTime.tm_mon + 1,
            localTime.tm_mday,
            localTime.tm_hour,
            localTime.tm_min,
            localTime.tm_sec,
            message);

        std::lock_guard<std::mutex> lock(g_logMutex);
        std::fputs(line, stderr);
        FILE* file = std::fopen(kServerLogFileName, "a");
        if (file)
        {
            std::fputs(line, file);
            std::fclose(file);
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
        FILE* existing = std::fopen(kServerConfigFileName, "r");
        if (existing)
        {
            std::fclose(existing);
            return;
        }

        FILE* file = std::fopen(kServerConfigFileName, "w");
        if (!file)
            return;

        std::fputs(
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
        std::fclose(file);
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

        FILE* file = std::fopen(kServerConfigFileName, "r");
        if (!file)
        {
            Log("server config not found; using defaults");
            return;
        }

        char lineBuffer[512] = {};
        while (std::fgets(lineBuffer, sizeof(lineBuffer), file))
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
        std::fclose(file);

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

        g_engineServer->ClientCommand(entity, kServerAckCommand);
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

        out = *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(entity) + offset);
        return true;
    }

    template <typename T>
    bool TryWriteEntityValue(Server_BaseEntity* entity, int offset, const T& value)
    {
        if (!entity || offset < 0)
            return false;

        *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(entity) + offset) = value;
        return true;
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

        return player->m_pUnk->GetBaseEntity();
    }

    void TraceServerRay(const Ray_t& ray, unsigned int mask, CTraceFilter* filter, CGameTrace& outTrace)
    {
        outTrace = {};
        outTrace.fraction = 1.0f;
        if (!g_engineTraceServer)
            return;

        g_engineTraceServer->TraceRay(ray, mask, filter, &outTrace);
    }

    struct ModuleInfo
    {
        uintptr_t base = 0;
        std::string path;
        bool found = false;
    };

    bool EndsWith(const std::string& value, const char* suffix)
    {
        if (!suffix)
            return false;

        const size_t suffixLength = std::strlen(suffix);
        return value.size() >= suffixLength &&
            value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
    }

    int FindModuleCallback(dl_phdr_info* info, size_t, void* data)
    {
        auto* query = static_cast<std::pair<const char*, ModuleInfo*>*>(data);
        const char* name = query->first;
        ModuleInfo* out = query->second;
        if (!info || !name || !out || !info->dlpi_name)
            return 0;

        const std::string path(info->dlpi_name);
        if (!EndsWith(path, name))
            return 0;

        out->base = static_cast<uintptr_t>(info->dlpi_addr);
        out->path = path;
        out->found = true;
        return 1;
    }

    bool FindModuleByName(const char* name, ModuleInfo& out)
    {
        out = {};
        std::pair<const char*, ModuleInfo*> query(name, &out);
        dl_iterate_phdr(FindModuleCallback, &query);
        return out.found && out.base != 0 && !out.path.empty();
    }

    bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out)
    {
        out.clear();
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file)
            return false;

        if (std::fseek(file, 0, SEEK_END) != 0)
        {
            std::fclose(file);
            return false;
        }
        const long length = std::ftell(file);
        if (length <= 0)
        {
            std::fclose(file);
            return false;
        }
        std::rewind(file);

        out.resize(static_cast<size_t>(length));
        const size_t read = std::fread(out.data(), 1, out.size(), file);
        std::fclose(file);
        return read == out.size();
    }

    template <typename T>
    bool RangeFits(const std::vector<unsigned char>& data, size_t offset, size_t count = 1)
    {
        return offset <= data.size() && count <= ((data.size() - offset) / sizeof(T));
    }

    bool ResolveElfSymbolValue(const std::string& path, const char* symbolName, uint32_t& outValue)
    {
        outValue = 0;

        std::vector<unsigned char> data;
        if (!ReadWholeFile(path, data))
            return false;

        if (!RangeFits<Elf32_Ehdr>(data, 0))
            return false;

        const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(data.data());
        if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
            ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
            ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
            ehdr->e_shentsize != sizeof(Elf32_Shdr))
        {
            return false;
        }

        if (!RangeFits<Elf32_Shdr>(data, ehdr->e_shoff, ehdr->e_shnum))
            return false;

        const auto* sections = reinterpret_cast<const Elf32_Shdr*>(data.data() + ehdr->e_shoff);
        for (int i = 0; i < ehdr->e_shnum; ++i)
        {
            const Elf32_Shdr& symSection = sections[i];
            if (symSection.sh_type != SHT_SYMTAB && symSection.sh_type != SHT_DYNSYM)
                continue;
            if (symSection.sh_entsize != sizeof(Elf32_Sym) || symSection.sh_link >= ehdr->e_shnum)
                continue;
            if (!RangeFits<Elf32_Sym>(data, symSection.sh_offset, symSection.sh_size / sizeof(Elf32_Sym)))
                continue;

            const Elf32_Shdr& strSection = sections[symSection.sh_link];
            if (strSection.sh_offset > data.size() || strSection.sh_size > data.size() - strSection.sh_offset)
                continue;

            const char* stringTable = reinterpret_cast<const char*>(data.data() + strSection.sh_offset);
            const size_t stringTableSize = strSection.sh_size;
            const auto* symbols = reinterpret_cast<const Elf32_Sym*>(data.data() + symSection.sh_offset);
            const size_t symbolCount = symSection.sh_size / sizeof(Elf32_Sym);
            for (size_t s = 0; s < symbolCount; ++s)
            {
                const Elf32_Sym& symbol = symbols[s];
                if (symbol.st_name >= stringTableSize || symbol.st_shndx == SHN_UNDEF || symbol.st_value == 0)
                    continue;

                const char* name = stringTable + symbol.st_name;
                if (std::strcmp(name, symbolName) == 0)
                {
                    outValue = symbol.st_value;
                    return true;
                }
            }
        }

        return false;
    }

    bool ResolveServerSymbol(const char* label, const char* symbolName, uint32_t fallbackOffset, uintptr_t& outAddress, bool required)
    {
        outAddress = 0;

        ModuleInfo serverModule;
        if (!FindModuleByName(kServerModuleName, serverModule))
        {
            if (required)
                Log("required module not loaded: %s", kServerModuleName);
            return !required;
        }

        uint32_t symbolValue = 0;
        if (!ResolveElfSymbolValue(serverModule.path, symbolName, symbolValue))
        {
            if (fallbackOffset != 0)
            {
                outAddress = serverModule.base + static_cast<uintptr_t>(fallbackOffset);
                Log("resolved %s by Linux fallback offset at %s+0x%X", label, kServerModuleName, fallbackOffset);
                return true;
            }

            if (required)
                Log("required Linux symbol not found: %s (%s)", label, symbolName);
            else
                Log("optional Linux symbol not found: %s (%s)", label, symbolName);
            return !required;
        }

        outAddress = serverModule.base + static_cast<uintptr_t>(symbolValue);
        Log("resolved %s at %s+0x%X", label, kServerModuleName, symbolValue);
        return outAddress != 0 || !required;
    }

    bool WriteRelativeJump(void* from, void* to)
    {
        auto* bytes = static_cast<unsigned char*>(from);
        const intptr_t rel = reinterpret_cast<unsigned char*>(to) - (bytes + 5);
        if (rel < std::numeric_limits<int32_t>::min() || rel > std::numeric_limits<int32_t>::max())
            return false;

        bytes[0] = 0xE9;
        *reinterpret_cast<int32_t*>(bytes + 1) = static_cast<int32_t>(rel);
        return true;
    }

    bool SetPageProtection(void* address, size_t length, int protection)
    {
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0)
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(static_cast<uintptr_t>(pageSize) - 1U);
        const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + length + static_cast<uintptr_t>(pageSize) - 1U) &
            ~(static_cast<uintptr_t>(pageSize) - 1U);
        return mprotect(reinterpret_cast<void*>(start), end - start, protection) == 0;
    }

    template <typename T>
    struct Hook
    {
        T original = nullptr;
        void* target = nullptr;
        void* trampoline = nullptr;
        size_t patchLength = 0;
        const char* name = nullptr;
        unsigned char originalBytes[24] = {};
        bool active = false;

        bool Create(void* targetFunc, void* detourFunc, const char* hookName, size_t hookPatchLength, bool required = true)
        {
            name = hookName;
            target = targetFunc;
            patchLength = hookPatchLength;

            if (!targetFunc)
            {
                if (required)
                    Log("hook target missing: %s", hookName);
                return !required;
            }

            if (hookPatchLength < 5 || hookPatchLength > sizeof(originalBytes))
            {
                Log("invalid patch length for %s: %zu", hookName, hookPatchLength);
                return !required;
            }

            trampoline = mmap(nullptr, hookPatchLength + 5, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (trampoline == MAP_FAILED)
            {
                trampoline = nullptr;
                Log("mmap trampoline failed for %s", hookName);
                return !required;
            }

            std::memcpy(originalBytes, targetFunc, hookPatchLength);
            std::memcpy(trampoline, targetFunc, hookPatchLength);
            if (!WriteRelativeJump(static_cast<unsigned char*>(trampoline) + hookPatchLength, static_cast<unsigned char*>(targetFunc) + hookPatchLength))
            {
                Log("trampoline relative jump out of range for %s", hookName);
                munmap(trampoline, hookPatchLength + 5);
                trampoline = nullptr;
                return !required;
            }

            if (!SetPageProtection(targetFunc, hookPatchLength, PROT_READ | PROT_WRITE | PROT_EXEC))
            {
                Log("mprotect RWX failed for %s", hookName);
                munmap(trampoline, hookPatchLength + 5);
                trampoline = nullptr;
                return !required;
            }

            if (!WriteRelativeJump(targetFunc, detourFunc))
            {
                Log("detour relative jump out of range for %s", hookName);
                std::memcpy(targetFunc, originalBytes, hookPatchLength);
                SetPageProtection(targetFunc, hookPatchLength, PROT_READ | PROT_EXEC);
                munmap(trampoline, hookPatchLength + 5);
                trampoline = nullptr;
                return !required;
            }

            std::memset(static_cast<unsigned char*>(targetFunc) + 5, 0x90, hookPatchLength - 5);
            __builtin___clear_cache(static_cast<char*>(targetFunc), static_cast<char*>(targetFunc) + hookPatchLength);
            SetPageProtection(targetFunc, hookPatchLength, PROT_READ | PROT_EXEC);

            original = reinterpret_cast<T>(trampoline);
            active = true;
            Log("hooked %s patchLen=%zu", hookName, hookPatchLength);
            return true;
        }

        void Remove()
        {
            if (!active || !target)
                return;

            SetPageProtection(target, patchLength, PROT_READ | PROT_WRITE | PROT_EXEC);
            std::memcpy(target, originalBytes, patchLength);
            __builtin___clear_cache(static_cast<char*>(target), static_cast<char*>(target) + patchLength);
            SetPageProtection(target, patchLength, PROT_READ | PROT_EXEC);
            active = false;

            if (trampoline)
            {
                munmap(trampoline, patchLength + 5);
                trampoline = nullptr;
            }
            original = nullptr;
        }
    };

    struct DedicatedOffsets
    {
        uintptr_t ServerFireTerrorBullets = 0;
        uintptr_t ReadUserCmd = 0;
        uintptr_t ProcessUsercmds = 0;
        uintptr_t TestMeleeSwingServer = 0;
        uintptr_t GetPrimaryAttackActivity = 0;
        uintptr_t GetActiveWeapon = 0;
        uintptr_t GetMeleeWeaponInfo = 0;
        uintptr_t EyePosition = 0;
        uintptr_t EyeAngles = 0;
        uintptr_t FindUseEntity = 0;
        uintptr_t PlayerUse = 0;
        uintptr_t CBaseEntity_GetAbsOrigin_Server = 0;
        uintptr_t CBaseEntity_SetOrigin_Server = 0;

        bool ResolveAll()
        {
            bool ok = true;
            ok &= ResolveServerSymbol("ServerFireTerrorBullets", "_Z17FireTerrorBulletsiRK6VectorRK6QAngle10CSWeaponIDiif", 0x0051CD80, ServerFireTerrorBullets, true);
            ok &= ResolveServerSymbol("ReadUserCmd", "_Z11ReadUsercmdP7bf_readP8CUserCmdS2_", 0x0056B490, ReadUserCmd, true);
            ok &= ResolveServerSymbol("ProcessUsercmds", "_ZN18CServerGameClients15ProcessUsercmdsEP7edict_tP7bf_readiiibb", 0x006B5600, ProcessUsercmds, true);
            ok &= ResolveServerSymbol("TestMeleeSwingServer", "_ZN18CTerrorMeleeWeapon23TestMeleeSwingCollisionERK6Vector", 0x00559630, TestMeleeSwingServer, true);
            ok &= ResolveServerSymbol("GetPrimaryAttackActivity", "_ZN18CTerrorMeleeWeapon24GetPrimaryAttackActivityEP16CMeleeWeaponInfo", 0x00558D20, GetPrimaryAttackActivity, true);
            ok &= ResolveServerSymbol("GetActiveWeapon", "_ZNK20CBaseCombatCharacter15GetActiveWeaponEv", 0x003EE410, GetActiveWeapon, true);
            ok &= ResolveServerSymbol("GetMeleeWeaponInfo", "_ZNK18CTerrorMeleeWeapon18GetMeleeWeaponInfoEv", 0x00558CF0, GetMeleeWeaponInfo, true);
            ok &= ResolveServerSymbol("EyePosition", "_ZN11CBasePlayer11EyePositionEv", 0x00406260, EyePosition, true);
            ok &= ResolveServerSymbol("EyeAngles", "_ZN11CBasePlayer9EyeAnglesEv", 0x00406090, EyeAngles, true);
            ok &= ResolveServerSymbol("FindUseEntity", "_ZN13CTerrorPlayer13FindUseEntityEfffPbb", 0x00509DB0, FindUseEntity, false);
            ok &= ResolveServerSymbol("PlayerUse", "_ZN13CTerrorPlayer9PlayerUseEP11CBaseEntity", 0x009CD3B0, PlayerUse, false);
            ok &= ResolveServerSymbol("CBaseEntity_GetAbsOrigin_Server", "_ZNK11CBaseEntity12GetAbsOriginEv", 0x006093A0, CBaseEntity_GetAbsOrigin_Server, true);
            ok &= ResolveServerSymbol("CBaseEntity_SetOrigin_Server", "_Z14UTIL_SetOriginP11CBaseEntityRK6Vectorb", 0x00B1CAC0, CBaseEntity_SetOrigin_Server, true);
            return ok;
        }
    };

    DedicatedOffsets g_offsets;
    Hook<tServerFireTerrorBullets> g_serverFireTerrorBullets;
    Hook<tProcessUsercmds> g_processUsercmds;
    Hook<tReadUsercmd> g_readUsercmd;
    Hook<tTestMeleeSwingCollision> g_testMeleeSwingServer;
    Hook<tGetPrimaryAttackActivity> g_getPrimaryAttackActivity;
    Hook<tEyePosition> g_eyePosition;
    Hook<tEyeAngles> g_eyeAngles;
    Hook<tFindUseEntity> g_findUseEntity;
    Hook<tPlayerUse> g_playerUse;

    void* SafeGetActiveWeapon(Server_BaseEntity* player)
    {
        if (!player || !g_offsets.GetActiveWeapon)
            return nullptr;

        auto getActiveWeapon = reinterpret_cast<tGetActiveWeapon>(g_offsets.GetActiveWeapon);
        return getActiveWeapon(player);
    }

    int SafeGetWeaponId(void* weapon)
    {
        if (!weapon)
            return 0;

        auto** vtable = *reinterpret_cast<void***>(weapon);
        if (!vtable)
            return 0;

        using WeaponIdVirtualFn = int (*)(void* thisptr);
        auto getWeaponId = reinterpret_cast<WeaponIdVirtualFn>(vtable[kWeaponIdVirtualIndex]);
        if (!getWeaponId)
            return 0;

        return getWeaponId(weapon);
    }

    void* SafeGetMeleeWeaponInfo(void* weapon)
    {
        if (!weapon || !g_offsets.GetMeleeWeaponInfo)
            return nullptr;

        auto getInfo = reinterpret_cast<tGetMeleeWeaponInfo>(g_offsets.GetMeleeWeaponInfo);
        return getInfo(weapon);
    }

    const Vector* SafeGetAbsOrigin(Server_BaseEntity* entity)
    {
        if (!entity || !g_offsets.CBaseEntity_GetAbsOrigin_Server)
            return nullptr;

        auto getAbsOrigin = reinterpret_cast<tGetAbsOriginServer>(g_offsets.CBaseEntity_GetAbsOrigin_Server);
        return getAbsOrigin(entity);
    }

    bool SafeSetOrigin(Server_BaseEntity* entity, const Vector& origin)
    {
        if (!entity || !g_offsets.CBaseEntity_SetOrigin_Server || !IsFiniteVector(origin))
            return false;

        auto setOrigin = reinterpret_cast<tSetOriginServer>(g_offsets.CBaseEntity_SetOrigin_Server);
        setOrigin(entity, origin, false);
        return true;
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

        const Vector* originPtr = SafeGetAbsOrigin(serverPlayer);
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

        const Vector* originPtr = SafeGetAbsOrigin(serverPlayer);
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

        const auto* base = reinterpret_cast<const unsigned char*>(g_currentUsercmdPlayer);
        return base[kUsingMountedGunOffset] != 0 || base[kUsingMountedWeaponOffset] != 0;
    }

    bool TryGetCurrentServerWeapon(void*& weapon, int& weaponId)
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

    int DetourServerFireTerrorBullets(int playerId, const Vector& origin, const QAngle& angles, int weaponId, int a5, int a6, float a7)
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

        return g_serverFireTerrorBullets.original(playerId, correctedOrigin, correctedAngles, weaponId, a5, a6, a7);
    }

    int DetourReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from)
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

            void* serverWeapon = nullptr;
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

    void ResetMeleeEntitiesHitThisSwing(void* weapon)
    {
        if (!weapon)
            return;

        auto* entity = reinterpret_cast<Server_BaseEntity*>(weapon);
        const int zero = 0;
        TryWriteEntityValue(entity, kMeleeEntitiesHitThisSwingOffset, zero);
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

        void* weapon = SafeGetActiveWeapon(player);
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

            ResetMeleeEntitiesHitThisSwing(weapon);
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
        if (!meleeInfo)
            return;

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
        const float swingAngle = std::acos(dot) * 180.0f / 3.14159265f;
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

    float DetourProcessUsercmds(void* thisptr, edict_t* player, void* buf, int numcmds, int totalcmds, int droppedPackets, bool ignore, bool paused)
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

        const float result = g_processUsercmds.original(thisptr, player, buf, numcmds, totalcmds, droppedPackets, ignore, paused);

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

    int DetourTestMeleeSwingCollisionServer(void* thisptr, const Vector& direction)
    {
        return g_testMeleeSwingServer.original(thisptr, direction);
    }

    int DetourGetPrimaryAttackActivity(void* thisptr, void* meleeInfo)
    {
        return g_getPrimaryAttackActivity.original(thisptr, meleeInfo);
    }

    Vector* DetourEyePosition(Vector* eyePos, void* thisptr)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (eyePos && TryGetServerControllerAimOverride(thisptr, controllerOrigin, controllerAngles, overrideReason))
        {
            *eyePos = controllerOrigin;
            return eyePos;
        }

        Vector* result = g_eyePosition.original(eyePos, thisptr);
        if (g_performingMeleeTrace && result && IsValidPlayerIndex(g_currentUsercmdPlayerIndex))
            *result = g_players[g_currentUsercmdPlayerIndex].controllerPos;

        return result;
    }

    const QAngle* DetourEyeAngles(void* thisptr)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (TryGetServerControllerAimOverride(thisptr, controllerOrigin, controllerAngles, overrideReason))
        {
            g_returnEyeAngles = controllerAngles;
            NormalizeAndClampViewAngles(g_returnEyeAngles);
            return &g_returnEyeAngles;
        }

        return g_eyeAngles.original(thisptr);
    }

    void DetourPlayerUse(void* thisptr, void* useEntity)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        if (TryBuildServerUseControllerPose(thisptr, controllerOrigin, controllerAngles))
        {
            ScopedServerUseControllerAimOverride useAim(thisptr, controllerOrigin, controllerAngles);
            g_playerUse.original(thisptr, useEntity);
            return;
        }

        g_playerUse.original(thisptr, useEntity);
    }

    Server_BaseEntity* DetourFindUseEntity(void* thisptr, float radius, float dotLimit, float defaultDotLimit, void* traceResult, bool unknown1, bool unknown2)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        if (TryBuildServerUseControllerPose(thisptr, controllerOrigin, controllerAngles))
        {
            ScopedServerUseControllerAimOverride useAim(thisptr, controllerOrigin, controllerAngles);
            return g_findUseEntity.original(thisptr, radius, dotLimit, defaultDotLimit, traceResult, unknown1, unknown2);
        }

        return g_findUseEntity.original(thisptr, radius, dotLimit, defaultDotLimit, traceResult, unknown1, unknown2);
    }

    bool WaitForServerModule(int timeoutMs)
    {
        const auto start = std::chrono::steady_clock::now();
        while (!g_unloading.load())
        {
            ModuleInfo module;
            if (FindModuleByName(kServerModuleName, module))
                return true;

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return false;
    }

    bool InstallHooks()
    {
        bool expected = false;
        if (!g_hooksInstalled.compare_exchange_strong(expected, true))
            return true;

        if (!WaitForServerModule(60000))
        {
            Log("server_srv.so did not load before timeout; dedicated Linux hooks not installed");
            g_hooksInstalled.store(false);
            return false;
        }

        if (!g_offsets.ResolveAll())
        {
            Log("Linux dedicated symbol resolution failed; hooks not installed");
            g_hooksInstalled.store(false);
            return false;
        }

        bool ok = true;
        ok &= g_serverFireTerrorBullets.Create(reinterpret_cast<void*>(g_offsets.ServerFireTerrorBullets), reinterpret_cast<void*>(&DetourServerFireTerrorBullets), "ServerFireTerrorBullets", 12);
        ok &= g_processUsercmds.Create(reinterpret_cast<void*>(g_offsets.ProcessUsercmds), reinterpret_cast<void*>(&DetourProcessUsercmds), "ProcessUsercmds", 12);
        ok &= g_readUsercmd.Create(reinterpret_cast<void*>(g_offsets.ReadUserCmd), reinterpret_cast<void*>(&DetourReadUsercmd), "ReadUsercmd", 12);
        ok &= g_testMeleeSwingServer.Create(reinterpret_cast<void*>(g_offsets.TestMeleeSwingServer), reinterpret_cast<void*>(&DetourTestMeleeSwingCollisionServer), "TestMeleeSwingServer", 12);
        ok &= g_getPrimaryAttackActivity.Create(reinterpret_cast<void*>(g_offsets.GetPrimaryAttackActivity), reinterpret_cast<void*>(&DetourGetPrimaryAttackActivity), "GetPrimaryAttackActivity", 14);
        ok &= g_eyePosition.Create(reinterpret_cast<void*>(g_offsets.EyePosition), reinterpret_cast<void*>(&DetourEyePosition), "EyePosition", 14);
        ok &= g_eyeAngles.Create(reinterpret_cast<void*>(g_offsets.EyeAngles), reinterpret_cast<void*>(&DetourEyeAngles), "EyeAngles", 14);
        ok &= g_findUseEntity.Create(reinterpret_cast<void*>(g_offsets.FindUseEntity), reinterpret_cast<void*>(&DetourFindUseEntity), "FindUseEntity", 12, false);
        ok &= g_playerUse.Create(reinterpret_cast<void*>(g_offsets.PlayerUse), reinterpret_cast<void*>(&DetourPlayerUse), "PlayerUse", 12, false);

        if (!ok)
        {
            Log("required Linux hook creation failed; rolling back");
            g_playerUse.Remove();
            g_findUseEntity.Remove();
            g_eyeAngles.Remove();
            g_eyePosition.Remove();
            g_getPrimaryAttackActivity.Remove();
            g_testMeleeSwingServer.Remove();
            g_readUsercmd.Remove();
            g_processUsercmds.Remove();
            g_serverFireTerrorBullets.Remove();
            g_hooksInstalled.store(false);
            return false;
        }

        Log("L4D2VR Linux dedicated server hooks installed");
        return true;
    }

    void UninstallHooks()
    {
        if (!g_hooksInstalled.exchange(false))
            return;

        g_playerUse.Remove();
        g_findUseEntity.Remove();
        g_eyeAngles.Remove();
        g_eyePosition.Remove();
        g_getPrimaryAttackActivity.Remove();
        g_testMeleeSwingServer.Remove();
        g_readUsercmd.Remove();
        g_processUsercmds.Remove();
        g_serverFireTerrorBullets.Remove();
        Log("L4D2VR Linux dedicated server hooks uninstalled");
    }

    void StartHookWorker()
    {
        bool expected = false;
        if (!g_hookWorkerStarted.compare_exchange_strong(expected, true))
            return;

        std::thread([]() { InstallHooks(); }).detach();
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
                Log("VEngineServer unavailable; L4D2VR client ack handshake will be disabled");
            if (!g_engineTraceServer)
                Log("EngineTraceServer003 unavailable; teleport and roomscale server movement will be disabled until it resolves");
            Log("L4D2VR Linux dedicated server plugin loaded");
            StartHookWorker();
            return true;
        }

        void Unload() override
        {
            g_unloading.store(true);
            UninstallHooks();
            g_engineTraceServer = nullptr;
            g_engineServer = nullptr;
            Log("L4D2VR Linux dedicated server plugin unloaded");
        }

        void Pause() override {}
        void UnPause() override {}

        const char* GetPluginDescription() override
        {
            return "L4D2VR Linux dedicated server usercmd decoder";
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

extern "C" __attribute__((visibility("default"))) void* CreateInterface(const char* name, int* returnCode)
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
