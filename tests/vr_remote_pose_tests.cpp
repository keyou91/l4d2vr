#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include "../L4D2VR/vr_pose_protocol.h"
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "vr_remote_pose_types.inl"

static unsigned checks = 0u, nativeReads = 0u;
static void Check(bool value, const char* message)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(EXIT_FAILURE); }
}
static void NativeRead();
struct C_BasePlayer
{
    Vector origin{0,0,0};
    QAngle angles{0,0,0};
    const Vector& GetAbsOrigin() const { NativeRead(); return origin; }
    const QAngle& GetAbsAngles() const { NativeRead(); return angles; }
};
struct player_info_t { int userID = 0; };
struct EntityList
{
    C_BasePlayer* current = nullptr;
    void* GetClientEntity(int) { NativeRead(); return current; }
};
struct Engine
{
    bool inGame = true, haveInfo = true;
    int id = 10;
    std::function<void()> onInfo;
    bool IsInGame() const { NativeRead(); return inGame; }
    bool GetPlayerInfo(int, player_info_t* info)
    {
        NativeRead();
        const bool available = haveInfo;
        info->userID = id;
        auto callback = std::move(onInfo);
        onInfo = {};
        if (callback) callback();
        return available;
    }
};
struct Player
{
    C_BasePlayer* pPlayer = nullptr;
#include "vr_remote_pose_owner.inl"
};
struct VR { bool m_WorldModelVRPoseDebugLog = false; };
struct Game
{
    static constexpr int kMaxPlayers = 65;
    std::array<Player, kMaxPlayers> m_PlayersVRInfo{};
    mutable std::mutex m_VRPoseMutex;
    std::atomic<bool> m_VRPoseServerCapable{false};
    EntityList* m_ClientEntityList = nullptr;
    Engine* m_EngineClient = nullptr;
    VR* m_VR = nullptr;
    bool IsValidPlayerIndex(int index) const { return index >= 0 && index < kMaxPlayers; }
    static void logMsg(const char*, ...) {}
    bool ReceiveVRPosePayload(int, std::uint16_t, const char*, bool, float);
    void RefreshVRPosePlayerIdentitiesMainThread();
    bool GetInterpolatedVRPose(int, float, float, VRPoseFrame&, float&) const;
};
static Game* activeGame = nullptr;
static std::uint64_t scriptedTick = 1000u;
static void NativeRead()
{
    ++nativeReads;
    Check(activeGame && activeGame->m_VRPoseMutex.try_lock(), "native identity/transform reads happen outside pose mutex");
    activeGame->m_VRPoseMutex.unlock();
}
static std::uint64_t VRPoseTickMs() { return scriptedTick; }
#include "vr_remote_pose_functions.inl"

struct Fixture
{
    Game game;
    Engine engine;
    EntityList entities;
    C_BasePlayer first, replacement, unrelatedLifecycleOwner;
    Fixture()
    {
        activeGame = &game;
        scriptedTick = 1000u;
        game.m_EngineClient = &engine;
        game.m_ClientEntityList = &entities;
        entities.current = &first;
        game.m_PlayersVRInfo[2].pPlayer = &unrelatedLifecycleOwner;
    }
    bool Receive(l4d2vr_pose::WirePacket wire, std::uint16_t sequence)
    {
        l4d2vr_pose::FinalizePacket(wire);
        std::string payload;
        Check(l4d2vr_pose::EncodePayload(wire, payload), "fixture encodes actual protocol");
        return game.ReceiveVRPosePayload(2, sequence, payload.c_str(), true, 0.0f);
    }
    bool Pose(VRPoseFrame& pose, float delay = 50.0f)
    {
        const auto reads = nativeReads;
        float freshness = 0.0f;
        const bool available = game.GetInterpolatedVRPose(2, delay, 250.0f, pose, freshness);
        Check(nativeReads == reads, "queued interpolation never invokes native APIs");
        return available;
    }
};
static l4d2vr_pose::WirePacket Packet(std::uint8_t mask)
{
    l4d2vr_pose::WirePacket wire{};
    wire.versionAndValidMask = static_cast<std::uint8_t>((l4d2vr_pose::kVersion << 4) | mask);
    wire.featureMask = l4d2vr_pose::kFeatureBodyYaw;
    return wire;
}
static void SetPoint(l4d2vr_pose::PackedTrackedPose& packed, float x, float y, float z)
{
    VRPosePackTrackedPose({Vector(x,y,z), QAngle(0,0,0)}, packed);
}
static bool Near(float left, float right) { return std::fabs(left-right) < 0.1f; }

static void TrackerRecovery()
{
    Fixture f;
    auto lost = Packet(l4d2vr_pose::kValidLeftHand);
    SetPoint(lost.hmd,4000,0,0);
    SetPoint(lost.rightHand,4000,0,0);
    SetPoint(lost.leftHand,10,0,0);
    Check(f.Receive(lost,1u), "unchecked invalid HMD/hand bytes do not reject healthy independent hand");
    const auto& stored = f.game.m_PlayersVRInfo[2].latestWorldPose;
    Check(stored.hmd.position.LengthSqr()==0.0f && stored.rightHand.position.LengthSqr()==0.0f,
        "invalid received tracker payloads are neutralized before history");
    scriptedTick = 1040u;
    auto recovered = Packet(l4d2vr_pose::kValidMask);
    recovered.bodyYaw = VRPosePackAngle(90.0f);
    SetPoint(recovered.hmd,0,0,70);
    SetPoint(recovered.rightHand,25,12,60);
    SetPoint(recovered.leftHand,0,30,0);
    recovered.handStateFlags = 0u;
    recovered.featureMask |= l4d2vr_pose::kFeatureRightFingerCurls;
    recovered.rightFingerCurls[0] = 200u;
    Check(f.Receive(recovered,2u), "tracker recovery with body-yaw turn accepts latest sample");
    scriptedTick = 1070u;
    VRPoseFrame pose{};
    Check(f.Pose(pose), "recovered packet is available at halfway interpolation time");
    Check(pose.validMask==l4d2vr_pose::kValidMask && Near(pose.hmd.position.z,70),
        "recovered HMD uses latest directly and preserves latest validity");
    Check(Near(pose.rightHand.position.x,25) && Near(pose.rightHand.position.y,12) &&
        Near(pose.rightHand.position.z,60), "recovered hand is neither blended from invalid bytes nor rebased through old yaw");
    Check(Near(pose.leftHand.position.x,0) && Near(pose.leftHand.position.y,20),
        "unaffected hand still smooths after rebasing its own valid older yaw sample");
    Check(Near(pose.rightFingerCurls[0],VRPoseUnpackFingerCurl(200u)), "newly ready curl feature takes latest directly");
    scriptedTick = 1080u;
    recovered.versionAndValidMask = static_cast<std::uint8_t>((l4d2vr_pose::kVersion<<4)|l4d2vr_pose::kValidHmd);
    recovered.featureMask = l4d2vr_pose::kFeatureBodyYaw;
    Check(f.Receive(recovered,3u) && f.Pose(pose), "subsequent hand loss keeps independent HMD pose");
    Check(pose.validMask==l4d2vr_pose::kValidHmd && pose.rightHand.position.LengthSqr()==0.0f,
        "latest invalid hand never borrows older validity or pose");
}

static void OwnerLifecycle()
{
    Fixture f;
    auto wire = Packet(l4d2vr_pose::kValidHmd);
    SetPoint(wire.hmd,0,0,70);
    Check(f.Receive(wire,90u), "first known owner packet is accepted");
    VRPoseFrame pose{};
    Check(f.Pose(pose), "first known owner pose can render");
    f.entities.current = &f.replacement;
    f.engine.id = 11;
    f.game.RefreshVRPosePlayerIdentitiesMainThread();
    Check(!f.Pose(pose), "main-thread replacement invalidates removed occupant before queued draw");
    Check(f.game.m_PlayersVRInfo[2].pPlayer==&f.unrelatedLifecycleOwner,
        "pose ownership lifecycle leaves unrelated pPlayer state intact");
    scriptedTick = 1010u;
    Check(f.Receive(wire,1u) && f.Pose(pose), "new occupant packet accepts restarted sequence without old history");
    Check(f.game.m_PlayersVRInfo[2].worldPoseEntity==&f.replacement &&
        f.game.m_PlayersVRInfo[2].worldPoseUserID==11 && pose.sequence==1u, "new pose has dedicated current owner identity");
    // Same entity address can be reused while Source's userID still changes.
    f.engine.id = 12;
    f.game.RefreshVRPosePlayerIdentitiesMainThread();
    Check(!f.Pose(pose), "userID replacement also fences reused entity address");
    Check(f.Receive(wire,1u), "same-address new user can establish fresh pose ownership");
    f.entities.current = nullptr;
    f.game.RefreshVRPosePlayerIdentitiesMainThread();
    Check(!f.Pose(pose) && !f.Receive(wire,2u), "absent entity invalidates history and rejects pre-entity packets");
    f.entities.current = &f.first;
    f.engine.haveInfo = false;
    Check(!f.Receive(wire,2u), "unknown native userID packet is conservatively rejected");
    f.engine.haveInfo = true;
    f.engine.id = -1;
    Check(!f.Receive(wire,2u), "successful but unknown userID is also rejected");
    f.engine.id = 13;
    Check(f.Receive(wire,1u), "known identity recovers on next actual packet");
    f.engine.inGame = false;
    f.game.RefreshVRPosePlayerIdentitiesMainThread();
    Check(!f.Pose(pose) && !f.Receive(wire,2u), "session disconnect invalidates cache and rejects packets");
    f.engine.inGame = true;
    Check(f.Receive(wire,1u), "later live session can establish fresh owner");
    scriptedTick = 1510u;
    Check(!f.Pose(pose), "ordinary stale fade still expires current owner pose");
}

static void ConcurrentOwnerRefresh()
{
    Fixture f;
    auto wire = Packet(l4d2vr_pose::kValidHmd);
    SetPoint(wire.hmd,0,0,70);
    Check(f.Receive(wire,20u), "concurrency fixture starts with known pose");
    f.engine.haveInfo = false;
    f.engine.onInfo = [&]
    {
        f.entities.current = &f.replacement;
        f.engine.id = 11;
        f.engine.haveInfo = true;
        Check(f.Receive(wire,1u), "new owner publishes while older refresh query is in flight");
    };
    f.game.RefreshVRPosePlayerIdentitiesMainThread();
    VRPoseFrame pose{};
    Check(f.Pose(pose) && pose.sequence==1u && f.game.m_PlayersVRInfo[2].worldPoseUserID==11,
        "stale missing-identity observation cannot erase newer packet revision");
    f.engine.onInfo = [&]
    {
        f.engine.id = 12;
        Check(f.Receive(wire,1u), "newer owner packet supersedes in-flight older receive identity query");
    };
    Check(!f.Receive(wire,2u), "older receive with obsolete native identity read is rejected");
    Check(f.Pose(pose) && pose.sequence==1u && f.game.m_PlayersVRInfo[2].worldPoseUserID==12,
        "newer accepted owner survives obsolete receive completion");
}

int main()
{
    TrackerRecovery();
    OwnerLifecycle();
    ConcurrentOwnerRefresh();
    std::printf("PASS: remote pose lifecycle (%u checks)\n", checks);
    return EXIT_SUCCESS;
}
