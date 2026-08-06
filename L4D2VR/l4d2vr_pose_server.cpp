#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "vr_pose_protocol.h"

// L4D2 does not ship linkable Source SDK libraries.  These are the minimal
// ABI declarations needed by an ISERVERPLUGINCALLBACKS003 plugin.  Keep their
// virtual method order and data layout in sync with the 32-bit Source engine.
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

struct Vector
{
    float x;
    float y;
    float z;
};

struct edict_t
{
    int m_fStateFlags;
    std::int16_t m_NetworkSerialNumber;
    std::int16_t m_EdictIndex;
    void* m_pNetworkable;
    void* m_pUnknown;
    float freetime;
};

class CCommand
{
public:
    int ArgC() const
    {
        return m_nArgc;
    }

    const char* Arg(int index) const
    {
        if (index < 0 || index >= m_nArgc)
            return "";
        return m_ppArgv[index] ? m_ppArgv[index] : "";
    }

private:
    enum
    {
        COMMAND_MAX_ARGC = 64,
        COMMAND_MAX_LENGTH = 512,
    };

    int m_nArgc;
    int m_nArgv0Size;
    char m_pArgSBuffer[COMMAND_MAX_LENGTH];
    char m_pArgvBuffer[COMMAND_MAX_LENGTH];
    const char* m_ppArgv[COMMAND_MAX_ARGC];
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
    virtual void* SaveAllocMemory(std::size_t, std::size_t) = 0;
    virtual void SaveFreeMemory(void*) = 0;
    virtual void EmitAmbientSound(
        int,
        const Vector&,
        const char*,
        float,
        int,
        int,
        int,
        float = 0.0f) = 0;
    virtual void FadeClientVolume(
        const edict_t*,
        float,
        float,
        float,
        float) = 0;
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

class IServerPluginCallbacks
{
public:
    virtual bool Load(
        CreateInterfaceFn interfaceFactory,
        CreateInterfaceFn gameServerFactory) = 0;
    virtual void Unload() = 0;
    virtual void Pause() = 0;
    virtual void UnPause() = 0;
    virtual const char* GetPluginDescription() = 0;
    virtual void LevelInit(const char* mapName) = 0;
    virtual void ServerActivate(
        edict_t* edictList,
        int edictCount,
        int clientMax) = 0;
    virtual void GameFrame(bool simulating) = 0;
    virtual void LevelShutdown() = 0;
    virtual void ClientActive(edict_t* entity) = 0;
    virtual void ClientDisconnect(edict_t* entity) = 0;
    virtual void ClientPutInServer(
        edict_t* entity,
        const char* playerName) = 0;
    virtual void SetCommandClient(int index) = 0;
    virtual void ClientSettingsChanged(edict_t* edict) = 0;
    virtual PLUGIN_RESULT ClientConnect(
        bool* allowConnect,
        edict_t* entity,
        const char* name,
        const char* address,
        char* reject,
        int maxRejectLength) = 0;
    virtual PLUGIN_RESULT ClientCommand(
        edict_t* entity,
        const CCommand& args) = 0;
    virtual PLUGIN_RESULT NetworkIDValidated(
        const char* userName,
        const char* networkId) = 0;
    virtual void OnQueryCvarValueFinished(
        QueryCvarCookie_t cookie,
        edict_t* playerEntity,
        EQueryCvarValueStatus status,
        const char* cvarName,
        const char* cvarValue) = 0;
    virtual void OnEdictAllocated(edict_t* edict) = 0;
    virtual void OnEdictFreed(const edict_t* edict) = 0;
};

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr int kMaxPlayerSlots = 64;
    constexpr int kAckMaxAttempts = 3;
    constexpr auto kAckRetryInterval = std::chrono::seconds(1);
    constexpr auto kMinimumUploadInterval = std::chrono::microseconds(16667);
    constexpr char kPluginInterfaceName[] = "ISERVERPLUGINCALLBACKS003";
    constexpr char kPoseHelloCommand[] = "l4d2vr_pose_hello";
    constexpr char kPoseUploadCommand[] = "l4d2vr_pose_upload";
    constexpr char kPoseAckCommand[] = "l4d2vr_pose_ack 2\n";

    static_assert(sizeof(void*) == 4, "Build the pose relay as Win32/x86.");
    static_assert(sizeof(edict_t) == 20, "Unexpected 32-bit Source edict layout.");
    static_assert(
        offsetof(edict_t, m_EdictIndex) == 6,
        "Unexpected Source edict player-index offset.");
    static_assert(
        sizeof(CCommand) == 1288,
        "Unexpected 32-bit Source CCommand layout.");
    static_assert(
        l4d2vr_pose::kEncodedPayloadChars == 72,
        "The protocol-v2 server command requires a 72-character payload.");

    struct ClientPoseState
    {
        edict_t* entity = nullptr;
        bool protocolSupported = false;
        bool haveSequence = false;
        std::uint16_t lastSequence = 0;
        bool haveDecodeTime = false;
        Clock::time_point lastDecodeTime{};
        bool ackPending = false;
        int ackAttempts = 0;
        Clock::time_point nextAckTime{};
    };

    IVEngineServer* g_engineServer = nullptr;
    std::array<ClientPoseState, kMaxPlayerSlots + 1> g_clients{};
    int g_clientMax = kMaxPlayerSlots;

    void ResetAllClients()
    {
        for (ClientPoseState& client : g_clients)
            client = ClientPoseState{};
    }

    bool IsValidPlayerIndex(int playerIndex)
    {
        return playerIndex >= 1 &&
            playerIndex <= kMaxPlayerSlots &&
            playerIndex <= g_clientMax;
    }

    int PlayerIndexFromEdict(const edict_t* entity)
    {
        if (!entity)
            return -1;

        const int playerIndex = static_cast<int>(entity->m_EdictIndex);
        return IsValidPlayerIndex(playerIndex) ? playerIndex : -1;
    }

    void ResetClient(int playerIndex, edict_t* entity)
    {
        if (!IsValidPlayerIndex(playerIndex))
            return;

        g_clients[playerIndex] = ClientPoseState{};
        g_clients[playerIndex].entity = entity;
    }

    bool SendClientCommand(edict_t* entity, const char* command)
    {
        if (!g_engineServer || !entity || !command)
            return false;

        bool sent = false;
        __try
        {
            // Never pass network-derived text as the printf-style format.
            g_engineServer->ClientCommand(entity, "%s", command);
            sent = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            sent = false;
        }
        return sent;
    }

    void SendAckAttempt(ClientPoseState& client)
    {
        if (!client.entity || client.ackAttempts >= kAckMaxAttempts)
        {
            client.ackPending = false;
            return;
        }

        SendClientCommand(client.entity, kPoseAckCommand);
        ++client.ackAttempts;
        client.ackPending =
            !client.protocolSupported &&
            client.ackAttempts < kAckMaxAttempts;
        client.nextAckTime = Clock::now() + kAckRetryInterval;
    }

    void QueueInitialAck(int playerIndex, edict_t* entity)
    {
        if (!IsValidPlayerIndex(playerIndex) || !entity)
            return;

        ClientPoseState& client = g_clients[playerIndex];
        client.entity = entity;

        if (client.ackPending || client.ackAttempts >= kAckMaxAttempts)
            return;

        SendAckAttempt(client);
    }

    void PumpPendingAcks()
    {
        const Clock::time_point now = Clock::now();
        for (int playerIndex = 1; playerIndex <= g_clientMax; ++playerIndex)
        {
            ClientPoseState& client = g_clients[playerIndex];
            if (!client.ackPending || now < client.nextAckTime)
                continue;

            SendAckAttempt(client);
        }
    }

    bool ParseSequence(const char* text, std::uint16_t& sequence)
    {
        if (!text || !text[0])
            return false;

        std::uint32_t value = 0;
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(text);
             *cursor;
             ++cursor)
        {
            if (*cursor < '0' || *cursor > '9')
                return false;

            value = value * 10u + static_cast<std::uint32_t>(*cursor - '0');
            if (value > 0xFFFFu)
                return false;
        }

        sequence = static_cast<std::uint16_t>(value);
        return true;
    }

    void RelayPose(
        int senderIndex,
        std::uint16_t sequence,
        const char* payload)
    {
        char command[192] = {};
        const int commandLength = std::snprintf(
            command,
            sizeof(command),
            "l4d2vr_pose_receive %d %u %s\n",
            senderIndex,
            static_cast<unsigned int>(sequence),
            payload);
        if (commandLength <= 0 ||
            commandLength >= static_cast<int>(sizeof(command)))
        {
            return;
        }

        for (int recipientIndex = 1;
             recipientIndex <= g_clientMax;
             ++recipientIndex)
        {
            if (recipientIndex == senderIndex)
                continue;

            ClientPoseState& recipient = g_clients[recipientIndex];
            if (!recipient.protocolSupported || !recipient.entity)
                continue;

            SendClientCommand(recipient.entity, command);
        }
    }

    bool IsCommand(const CCommand& args, const char* expected)
    {
        return args.ArgC() > 0 &&
            std::strcmp(args.Arg(0), expected) == 0;
    }

    class L4D2VRPoseServerPlugin final : public IServerPluginCallbacks
    {
    public:
        bool Load(
            CreateInterfaceFn interfaceFactory,
            CreateInterfaceFn) override
        {
            ResetAllClients();
            g_clientMax = kMaxPlayerSlots;
            g_engineServer = nullptr;

            if (interfaceFactory)
            {
                int returnCode = IFACE_FAILED;
                g_engineServer = static_cast<IVEngineServer*>(
                    interfaceFactory("VEngineServer022", &returnCode));
            }

            if (!g_engineServer)
            {
                OutputDebugStringA(
                    "L4D2VR pose server: VEngineServer interface unavailable\n");
                return false;
            }

            OutputDebugStringA("L4D2VR pose server loaded\n");
            return true;
        }

        void Unload() override
        {
            ResetAllClients();
            g_engineServer = nullptr;
            OutputDebugStringA("L4D2VR pose server unloaded\n");
        }

        void Pause() override {}
        void UnPause() override {}

        const char* GetPluginDescription() override
        {
            return "L4D2VR dedicated-server protocol-v2 body, hand, and finger pose relay";
        }

        void LevelInit(const char*) override
        {
            ResetAllClients();
        }

        void ServerActivate(edict_t*, int, int clientMax) override
        {
            if (clientMax >= 1 && clientMax <= kMaxPlayerSlots)
                g_clientMax = clientMax;
            else
                g_clientMax = kMaxPlayerSlots;
        }

        void GameFrame(bool) override
        {
            PumpPendingAcks();
        }

        void LevelShutdown() override
        {
            ResetAllClients();
            g_clientMax = kMaxPlayerSlots;
        }

        void ClientActive(edict_t* entity) override
        {
            const int playerIndex = PlayerIndexFromEdict(entity);
            if (!IsValidPlayerIndex(playerIndex))
                return;

            ClientPoseState& client = g_clients[playerIndex];
            if (client.entity != entity)
                ResetClient(playerIndex, entity);

            QueueInitialAck(playerIndex, entity);
        }

        void ClientDisconnect(edict_t* entity) override
        {
            const int playerIndex = PlayerIndexFromEdict(entity);
            if (!IsValidPlayerIndex(playerIndex))
                return;

            ClientPoseState& client = g_clients[playerIndex];
            if (!client.entity || client.entity == entity)
                ResetClient(playerIndex, nullptr);
        }

        void ClientPutInServer(edict_t* entity, const char*) override
        {
            const int playerIndex = PlayerIndexFromEdict(entity);
            if (!IsValidPlayerIndex(playerIndex))
                return;

            ResetClient(playerIndex, entity);
            QueueInitialAck(playerIndex, entity);
        }

        void SetCommandClient(int) override {}
        void ClientSettingsChanged(edict_t*) override {}

        PLUGIN_RESULT ClientConnect(
            bool*,
            edict_t*,
            const char*,
            const char*,
            char*,
            int) override
        {
            return PLUGIN_CONTINUE;
        }

        PLUGIN_RESULT ClientCommand(
            edict_t* entity,
            const CCommand& args) override
        {
            const bool isHello = IsCommand(args, kPoseHelloCommand);
            const bool isUpload = IsCommand(args, kPoseUploadCommand);
            if (!isHello && !isUpload)
                return PLUGIN_CONTINUE;

            const int playerIndex = PlayerIndexFromEdict(entity);
            if (!IsValidPlayerIndex(playerIndex))
                return PLUGIN_STOP;

            ClientPoseState& client = g_clients[playerIndex];
            if (client.entity != entity)
                ResetClient(playerIndex, entity);

            if (isHello)
            {
                std::uint16_t protocolVersion = 0u;
                if (args.ArgC() == 2 &&
                    ParseSequence(args.Arg(1), protocolVersion) &&
                    protocolVersion == l4d2vr_pose::kVersion)
                {
                    client.protocolSupported = true;
                    client.ackPending = false;
                }
                return PLUGIN_STOP;
            }

            if (!client.protocolSupported || args.ArgC() != 3)
                return PLUGIN_STOP;

            std::uint16_t sequence = 0;
            if (!ParseSequence(args.Arg(1), sequence))
                return PLUGIN_STOP;

            if (client.haveSequence &&
                !l4d2vr_pose::IsSequenceNewer(
                    sequence,
                    client.lastSequence))
            {
                return PLUGIN_STOP;
            }

            const Clock::time_point now = Clock::now();
            if (client.haveDecodeTime &&
                now - client.lastDecodeTime < kMinimumUploadInterval)
            {
                return PLUGIN_STOP;
            }
            // Invalid traffic also consumes this validation slot, bounding the
            // CRC/decode work from one client to at most 60 attempts per second.
            client.haveDecodeTime = true;
            client.lastDecodeTime = now;

            l4d2vr_pose::WirePacket packet{};
            const char* payload = args.Arg(2);
            if (!l4d2vr_pose::DecodePayload(payload, packet))
                return PLUGIN_STOP;

            client.haveSequence = true;
            client.lastSequence = sequence;
            RelayPose(playerIndex, sequence, payload);
            return PLUGIN_STOP;
        }

        PLUGIN_RESULT NetworkIDValidated(
            const char*,
            const char*) override
        {
            return PLUGIN_CONTINUE;
        }

        void OnQueryCvarValueFinished(
            QueryCvarCookie_t,
            edict_t*,
            EQueryCvarValueStatus,
            const char*,
            const char*) override
        {
        }

        void OnEdictAllocated(edict_t*) override {}

        void OnEdictFreed(const edict_t* entity) override
        {
            const int playerIndex = PlayerIndexFromEdict(entity);
            if (!IsValidPlayerIndex(playerIndex))
                return;

            ClientPoseState& client = g_clients[playerIndex];
            if (client.entity == entity)
                ResetClient(playerIndex, nullptr);
        }
    };

    L4D2VRPoseServerPlugin g_plugin;
}

extern "C" __declspec(dllexport) void* __cdecl CreateInterface(
    const char* name,
    int* returnCode)
{
    if (name && std::strcmp(name, kPluginInterfaceName) == 0)
    {
        if (returnCode)
            *returnCode = IFACE_OK;
        return &g_plugin;
    }

    if (returnCode)
        *returnCode = IFACE_FAILED;
    return nullptr;
}
