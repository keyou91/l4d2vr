#include "../L4D2VR/vr_magazine_policy.h"
#include "../L4D2VR/vr_physical_controls.h"
#include "../L4D2VR/vr_remote_magazines.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool result, const char* message)
{
    if (!result)
    {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static void CheckEmptyPumpReload(int weaponId)
{
    using namespace l4d2vr_magazine;
    using namespace l4d2vr_shell;
    const std::uintptr_t owner = 501u, gun = 601u;
    const std::uint32_t handle = 0x3005u;
    l4d2vr_physical::PumpCycles pump;
    pump.ObserveOwner(true, owner);
    pump.NotifyShot(gun, weaponId, 0, handle);
    ChamberHistory chamber; chamber.ObserveOwner(true, owner);
    Check(chamber.Observe(gun, handle, weaponId, 0), "The last shot leaves an empty pump chamber");
    Check(pump.Observe(gun, weaponId, 0, handle), "The spent action still needs a cycle");
    Check(!pump.Complete(gun, handle), "A dry stroke cannot complete chambering");
    Check(!PumpShouldStartAfterShot(true, 0, 0), "An empty tube must accept shells before entering the pump stage");

    ServerLedger host; host.Reset(9u);
    ClientRequest client; client.Offer(kVersion, 9u);
    int clip = 0, reserve = 3, writes = 0;
    for (int loaded = 1; loaded <= 2; ++loaded)
    {
        const std::uint64_t now = 1000u + loaded * 300u;
        const std::uint32_t command = 200u + loaded;
        Request request{}; Reply reply{};
        Check(client.Begin(handle, weaponId, clip, reserve, command, owner, 1u, now, request), "A shell gesture submits its real counters");
        const Snapshot state{true, handle, command, weaponId, clip, reserve, 8};
        const auto applied = host.Apply(request, state, now, [&](int nextClip, int nextReserve) {
            clip = nextClip; reserve = nextReserve; ++writes; return true;
        });
        Check(applied.status == Status::Applied, "The host inserts one shell");
        client.Receive(applied);
        Check(client.Update(handle, weaponId, owner, 1u, now + 1u, reply, clip, reserve) == ClientRequest::Poll::Result,
            "The shell settles after acknowledgement and native replication");
        const bool needsCycle = pump.Observe(gun, weaponId, clip, handle);
        Check(needsCycle && !PumpShouldStartAfterShot(needsCycle, clip, loaded), "Loading shells must not force an early pump stage");
        Check(chamber.Observe(gun, handle, weaponId, clip), "Loading does not chamber a shell");
        Check(ShotgunBlocksFire(true, false, false, false, false), "The empty chamber blocks firing during tube loading");
        Check(clip + reserve == 3 && writes == loaded, "Each gesture transfers exactly one existing shell");
    }
    Check(l4d2vr_physical::PumpCycles::StrokeComplete(true, 0.01f, 0.02f), "A full forward return completes the mechanical stroke");
    Check(pump.Complete(gun, handle), "The loaded tube can now feed its chamber");
    chamber.Complete(gun);
    Check(!pump.Observe(gun, weaponId, clip, handle) && !chamber.Observe(gun, handle, weaponId, clip), "The completed reload permits firing");
    Check(PumpShouldStartAfterShot(true, clip - 1, 0), "A normal shot with remaining shells still enters the pump stage");
}

static void CheckUnfinishedCycleReturn(int weaponId, int capacity, bool shotgun)
{
    using namespace l4d2vr_magazine;
    constexpr std::uintptr_t owner = 501u, gun = 601u;
    constexpr std::uint32_t handle = 0x3005u;
    ChamberHistory chamber;
    SessionTracker session;
    chamber.ObserveOwner(true, owner);
    chamber.Observe(gun, handle, weaponId, 0);
    const unsigned mode = 3u | (shotgun ? kAuthoritativeShellInputMode : kAuthoritativeMagazineInputMode);
    session.Observe(true, owner, gun, weaponId, mode, handle);
    const bool infinite = weaponId == 32;
    int clip = 0, reserve = shotgun ? 1 : infinite ? 0 : capacity;
    const int originalAmmo = clip + reserve;
    int writes = 0;
    const auto writer = [&](int nextClip, int nextReserve) { clip = nextClip; reserve = nextReserve; ++writes; return true; };
    if (shotgun)
    {
        l4d2vr_shell::ServerLedger host; host.Reset(9u);
        l4d2vr_shell::ClientRequest client; client.Offer(l4d2vr_shell::kVersion,9u);
        l4d2vr_shell::Request request{}; l4d2vr_shell::Reply reply{};
        Check(client.Begin(handle,weaponId,clip,reserve,201u,owner,1u,1000u,request), "The last reserve shell can start insertion");
        const l4d2vr_shell::Snapshot native{true,handle,201u,weaponId,clip,reserve,capacity};
        const auto applied = host.Apply(request,native,1000u,writer);
        client.Receive(applied);
        Check(client.Update(handle,weaponId,owner,1u,1001u,reply,clip,reserve) == l4d2vr_shell::ClientRequest::Poll::Result,
            "The host shell result settles through actual replicated counters");
    }
    else
    {
        l4d2vr_remote_mag::ServerLedger host; host.Reset(9u);
        l4d2vr_remote_mag::ClientRequest client; client.Offer(l4d2vr_remote_mag::kVersion,9u);
        for (const auto action : {l4d2vr_remote_mag::Action::Eject,l4d2vr_remote_mag::Action::Insert})
        {
            const bool eject = action == l4d2vr_remote_mag::Action::Eject;
            const unsigned command = eject ? 201u : 202u;
            const std::uint64_t now = eject ? 1000u : 1300u;
            l4d2vr_remote_mag::Request request{}; l4d2vr_remote_mag::Reply reply{};
            Check(client.Begin(handle,weaponId,clip,reserve,action,command,owner,1u,now,request), "An empty magazine reload submits native counters");
            const l4d2vr_remote_mag::Snapshot native{true,handle,command,weaponId,clip,reserve,capacity,infinite};
            const auto applied = host.Apply(request,native,now,writer);
            Check(applied.status == l4d2vr_shell::Status::Applied, "The host applies each empty magazine action");
            client.Receive(applied);
            Check(client.Update(handle,weaponId,owner,1u,now+1u,reply,clip,reserve) == l4d2vr_shell::ClientRequest::Poll::Result,
                "The magazine result settles through actual replicated counters");
        }
    }
    Check(clip > 0 && reserve == 0 && (infinite ? clip == capacity : clip + reserve == originalAmmo),
        "The physical cycle must use the last existing ammo or native infinite capacity");
    chamber.Observe(gun,handle,weaponId,clip);
    Check(session.Observe(true,owner,602u,12,3u,0x3006u), "Switching to an item fences only the active hand session");
    chamber.ObserveOwner(true,owner); chamber.Observe(602u,0x3006u,12,-1);
    Check(session.Observe(true,owner,gun,weaponId,mode,handle), "Returning starts a fresh hand session");
    chamber.ObserveOwner(true,owner); chamber.Observe(gun,handle,weaponId,clip);
    const bool retained = chamber.HasEmptyChamber(owner,gun,handle,weaponId);
    Check(retained, "A same-owner same-serial return preserves its unfinished chamber cycle");
    Check(shotgun ? MayFinishShotgunLoading(true,retained,clip,0,true) : ShouldResumeMagazineCycle(true,false,retained,clip),
        "A returned loaded gun can cycle with no spare reserve and no insertions in the new session");
    Check(chamber.HasEmptyChamber(owner,gun,handle,weaponId), "Missing or stale render regions do not ready the chamber");
    Check(!chamber.HasEmptyChamber(owner+1u,gun,handle,weaponId) &&
        !chamber.HasEmptyChamber(owner,gun,0x4005u,weaponId) &&
        !chamber.HasEmptyChamber(owner,gun,handle,weaponId+1), "A reused owner, serial or weapon type cannot inherit the requirement");
    Check(!ShouldResumeMagazineCycle(false,false,retained,clip), "Lost capability must keep native fallback ownership");
    Check(!l4d2vr_physical::PumpCycles::StrokeComplete(false,0.0f,0.02f), "A fresh action region alone cannot complete the stroke");
    Check(l4d2vr_physical::PumpCycles::StrokeComplete(true,0.01f,0.02f), "The existing physical forward stroke can finish the returned gun");
    chamber.Complete(gun);
    Check(!chamber.HasEmptyChamber(owner,gun,handle,weaponId), "Only cycle completion releases the retained fire requirement");
    Check(writes == (shotgun ? 1 : 2), "Switching, region recovery and cycling never write ammunition");
}

int main()
{
    using namespace l4d2vr_magazine;
    CheckEmptyPumpReload(3);
    CheckEmptyPumpReload(8);
    struct StockMagazine { int id, capacity; };
    for (const auto gun : {StockMagazine{2,50},StockMagazine{5,50},StockMagazine{6,15},StockMagazine{7,50},
        StockMagazine{9,60},StockMagazine{10,30},StockMagazine{26,40},StockMagazine{32,8},StockMagazine{33,50},
        StockMagazine{34,50},StockMagazine{35,20},StockMagazine{36,15}})
        CheckUnfinishedCycleReturn(gun.id,gun.capacity,false);
    for (const auto gun : {StockMagazine{3,8},StockMagazine{8,8},StockMagazine{4,10},StockMagazine{11,10}})
        CheckUnfinishedCycleReturn(gun.id,gun.capacity,true);
    SessionTracker serialSession;
    Check(serialSession.Observe(true, 100u, 200u, 3, 3u, 0x3005u), "A native gun starts its own reload session");
    Check(!serialSession.Observe(true, 100u, 200u, 3, 3u, 0x3005u), "The same serial keeps the current gesture");
    Check(serialSession.Observe(true, 100u, 200u, 3, 3u, 0x4005u), "A recycled gun address must fence the previous gesture and replies");
    Check(!serialSession.Observe(true, 100u, 200u, 3, 3u, 0x4005u), "The replacement has stable independent ownership");
    Check(serialSession.Observe(true, 100u, 200u, 3, 3u, 0u), "Losing serial identity invalidates retained contact state");

    // Controller loss pauses gestures, while a submitted ammo operation can
    // still settle against its host reply and native replication.
    l4d2vr_remote_mag::ClientRequest pending;
    pending.Offer(l4d2vr_remote_mag::kVersion, 9u);
    l4d2vr_remote_mag::Request request{};
    Check(TrackingAllowsGesture(true, true, true, 1000u, 1000u), "Fresh hands can start a reload");
    Check(pending.Begin(0x3005u, 2, 10, 50, l4d2vr_remote_mag::Action::Eject, 100u, 100u, 1u, 1000u, request),
        "A fresh gesture submits its authoritative ejection");
    Check(!TrackingAllowsGesture(true, true, true, 1000u, 1151u), "Old valid flags cannot advance a slide or magazine");
    Check(!TrackingAllowsGesture(true, false, true, 1200u, 1200u), "An untracked support hand cannot complete a stroke");
    pending.Receive({request, l4d2vr_remote_mag::Status::Applied, 1, 50});
    l4d2vr_remote_mag::Reply reply{};
    Check(pending.Update(0x3005u, 2, 100u, 1u, 1200u, reply, 1, 50) == l4d2vr_remote_mag::ClientRequest::Poll::Result,
        "Tracking loss must not discard an already applied ammo transaction");
    Check(TrackingAllowsGesture(true, true, true, 1300u, 1300u), "Fresh tracking permits the next physical stage");
    Check(!TrackingAllowsGesture(true, true, true, 0u, 1300u), "An unstamped pose is not live input");
    Check(!TrackingAllowsGesture(true, true, true, 1400u, 1300u), "A future pose cannot bypass freshness");
    SessionTracker pumpSession;
    l4d2vr_physical::PumpCycles pump;
    pumpSession.Observe(true, 501u, 601u, 3, 3u);
    pump.ObserveOwner(true, pumpSession.OwnerTag());
    pump.NotifyShot(601u, 3, 7);
    Check(pumpSession.Observe(true, 501u, 602u, 2, 3u), "Drawing an SMG resets only current hand interaction");
    pump.ObserveOwner(true, pumpSession.OwnerTag());
    pumpSession.Observe(true, 501u, 601u, 3, 3u);
    pump.ObserveOwner(true, pumpSession.OwnerTag());
    Check(pump.Observe(601u, 3, 7), "Interaction recovery must preserve the shotgun's outstanding pump cycle");
    pumpSession.Observe(false, 501u, 601u, 3, 3u);
    pump.ObserveOwner(false, pumpSession.OwnerTag());
    Check(!pump.Observe(601u, 3, 7), "Death or incapacitation must clear expired pump ownership");
    SessionTracker session;
    Check(!session.Observe(false, 0u, 0u, 0, 0u), "A disconnected session must remain idle");
    Check(session.Observe(true, 100u, 200u, 1, 3u), "Entering gameplay must discard state from the previous session");
    const unsigned int queuedGeneration = 1u;
    for (int reload = 0; reload < 10; ++reload)
    {
        Check(!session.Observe(true, 100u, 200u, 1, 3u), "Normal reload completion must not invalidate its pending ammunition update");
        Check(MayCommitAmmo(1, 1, 1, queuedGeneration, queuedGeneration), "A current reload may settle its actual ammunition");
    }
    Check(session.Observe(true, 100u, 201u, 1, 3u), "A same-type weapon swap must reset held magazine and support-hand ownership");
    Check(!MayCommitAmmo(1, 1, 1, queuedGeneration, 2u), "An old reload must not reach a different pistol of the same type");
    Check(session.Observe(false, 100u, 201u, 1, 3u), "Incapacitation must end the interaction session even while grip stays held");
    Check(!session.Observe(false, 100u, 201u, 1, 3u), "Remaining incapacitated must not repeatedly create new sessions");
    Check(session.Observe(true, 100u, 201u, 1, 3u), "Revival must begin with new interaction ownership");
    Check(session.Observe(true, 101u, 201u, 1, 3u), "Respawn or player replacement must invalidate old interactions");
    Check(session.Observe(true, 101u, 201u, 2, 3u), "A changed weapon type at the same address must reset ownership");
    Check(session.Observe(true, 101u, 201u, 2, 2u), "Disabling manual reload or changing hand mode must clear prior suppression");
    Check(session.Observe(true, 101u, 201u, 2, 3u), "Re-enabling manual reload must start a new session");
    Check(session.Observe(true, 101u, 0u, 2, 3u), "A missing active weapon must end the session");
    Check(!session.Observe(true, 101u, 0u, 2, 3u), "An invalid weapon must never become ready");
    Check(!session.Observe(true, 0u, 201u, 2, 3u), "An invalid player must never become ready");
    Check(!session.Observe(true, 101u, 201u, 0, 3u), "An unknown weapon must never become ready");

    Check(!MayCommitAmmo(1, 5, 1, 1u, 1u), "A matching client pistol must not authorize writing pistol ammo to a server rifle");
    Check(!MayCommitAmmo(1, 1, 5, 1u, 1u), "A matching server pistol must not authorize an update after the client switched weapons");
    Check(!MayCommitAmmo(0, 1, 1, 1u, 1u), "An update with no known weapon owner must fail");
    Check(!MayCommitAmmo(3, 0, 3, 1u, 1u), "An unresolved server weapon must never receive a shell update");
    Check(MayCommitAmmo(3, 3, 3, 4u, 4u), "A current shotgun shell update must remain valid");
    Check(!MayCommitAmmo(3, 8, 3, 4u, 4u), "A pump-shotgun update must not affect a Chrome shotgun");

    SessionTracker ammoSession;
    Check(ammoSession.Observe(true, 1u, 2u, 2, 3u), "A legacy local reload starts with its own session");
    const unsigned authoritativeMode = 3u | kAuthoritativeMagazineInputMode;
    Check(UseAuthoritativeMagazine(true, true, false, false), "A supported single gun uses native transactions on host and guest");
    Check(ammoSession.Observe(true, 1u, 2u, 2, authoritativeMode), "A late ammo capability must fence the old predicted reload");
    Check(!ammoSession.Observe(true, 1u, 2u, 2, authoritativeMode), "Local hook heartbeats must not move an authoritative reload back to prediction");
    Check(!MayCommitAmmo(2, 2, 2, 7u, 8u), "A previously queued local clip write cannot cross the backend boundary");
    const unsigned pistolMode = 3u | kAuthoritativePistolInputMode;
    Check(ammoSession.Observe(true, 1u, 2u, 2, pistolMode), "Pistol transactions fence the older magazine protocol");
    Check(!ammoSession.Observe(true, 1u, 2u, 2, pistolMode), "A pending pistol gesture retains its backend session");
    Check((kAuthoritativePistolInputMode & (kAuthoritativeMagazineInputMode | kAuthoritativeShellInputMode)) == 0u,
        "Pistol, magazine and shell session capabilities remain distinct");
    Check(ammoSession.Observe(true, 1u, 2u, 2, 3u), "Losing the transaction lease must cancel its pending hand state");
    Check(!UseAuthoritativeMagazine(false, true, false, false), "An absent capability cannot invent transaction support");
    Check(!UseAuthoritativeMagazine(true, true, true, false), "Native dual pistols keep their explicit shared reload fallback");
    Check(!UseAuthoritativeMagazine(true, true, false, true), "A pistol retained in the left hand keeps its native fallback");
    Check(!UseAuthoritativeMagazine(true, false, false, false), "A shell-fed gun cannot enter the detachable-magazine path");
    SessionTracker shellSession;
    Check(shellSession.Observe(true, 1u, 3u, 3, 3u), "An unsupported shotgun begins with native controls");
    const unsigned shellMode = 3u | kAuthoritativeShellInputMode;
    Check(shellSession.Observe(true, 1u, 3u, 3, shellMode), "Shell capability starts fresh physical reload ownership");
    Check(!shellSession.Observe(true, 1u, 3u, 3, shellMode), "Local server hooks do not change the native shell route");
    Check(shellSession.Observe(true, 1u, 3u, 3, 3u), "A shell timeout clears held-ammo and pump interaction state");
    Check((kAuthoritativeShellInputMode & kAuthoritativeMagazineInputMode) == 0u, "Shell and magazine session capabilities remain distinct");

    NativeFallbackPulse nativePulse;
    Check(!nativePulse.Update(true, false, 10u, 100u), "Unsupported servers must not reload until a button request");
    Check(nativePulse.Update(true, true, 10u, 200u), "Magazine Release requests stock reload without writing ammo");
    Check(nativePulse.Update(true, false, 10u, 549u), "The request must survive an input frame until Source samples it");
    Check(!nativePulse.Update(true, false, 10u, 550u), "A stock reload request has a bounded button hold");
    Check(nativePulse.Update(true, true, 10u, 600u), "Another deliberate press starts a new reload request");
    Check(!nativePulse.Update(true, false, 11u, 610u), "A gun/player/backend change cannot reload the newly selected gun");
    Check(nativePulse.Update(true, true, 11u, 620u), "The new session can accept its own button request");
    Check(!nativePulse.Update(false, false, 11u, 630u), "Menus and unavailable control cancel native fallback input");
    Check(!nativePulse.Update(true, false, 11u, 640u), "Returning to gameplay cannot resume a stale native reload pulse");
    Check(nativePulse.Update(true, true, 11u, 0xfffffff0u), "The pulse can start before a tick-count wrap");
    Check(nativePulse.Update(true, false, 11u, 20u), "Tick-count wrap does not cancel a current short request");
    nativePulse.Reset();
    Check(!nativePulse.Update(true, false, 11u, 30u), "Session reset requires a fresh request even with the same generation");

    Check(IsLocalPlayerCommand(1, 1), "The host's command may settle the host's reload");
    for (int player = 2; player <= 32; ++player)
        Check(!IsLocalPlayerCommand(player, 1), "Other players must never consume the host's queued ammunition update");
    Check(!IsLocalPlayerCommand(0, 0) && !IsLocalPlayerCommand(-1, -1), "Invalid player slots must not be treated as a local host");
}
