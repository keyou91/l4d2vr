#include "../L4D2VR/vr_remote_magazines.h"
#include <cstdio>
#include <cstdlib>

static void CheckImpl(bool okay, int line)
{
    if (!okay) { std::fprintf(stderr, "Remote magazine check failed at line %d\n", line); std::abort(); }
}
#define Check(value) CheckImpl((value), __LINE__)

static void CheckLostMagazineReplies(int weaponId, int initialClip, int capacity)
{
    using namespace l4d2vr_remote_mag;
    const std::uint32_t handle = 0x3005u;
    ServerLedger server; server.Reset(91u);
    ClientRequest client; client.Offer(kVersion,91u);
    Snapshot native{true,handle,200u,weaponId,initialClip,100,capacity,false};
    std::uint64_t now = 1000u;
    auto act = [&](Action action)
    {
        now += 1000u; ++native.latestCommand;
        Request request{},retry{}; Reply selected{};
        Check(client.Begin(handle,weaponId,native.clip,native.reserve,action,native.latestCommand,10u,1u,now,request));
        int writes = 0, nextClip = native.clip, nextReserve = native.reserve;
        auto writer = [&](int c,int r) { ++writes; nextClip=c; nextReserve=r; return true; };
        const auto applied = server.Apply(request,native,now,writer);
        Check(applied.status == Status::Applied && writes == 1);
        native.clip=nextClip; native.reserve=nextReserve; native.latestCommand+=100u;
        Check(client.Update(handle,weaponId,10u,1u,now+499u,selected,-1,-1) == ClientRequest::Poll::Waiting);
        Check(!client.NextRetry(now+499u,retry));
        Check(client.NextRetry(now+500u,retry) && retry == request);
        const auto memo = server.Apply(retry,native,now+500u,writer);
        Check(memo.status == Status::Applied && writes == 1 && memo.clip == applied.clip && memo.reserve == applied.reserve);
        client.Receive(memo);
        Check(!client.NextRetry(now+1000u,retry));
        Check(client.Update(handle,weaponId,10u,1u,now+501u,selected,-1,-1) == ClientRequest::Poll::Waiting);
        Check(client.Update(handle,weaponId,10u,1u,now+502u,selected,native.clip,native.reserve) == ClientRequest::Poll::Result);
        Check(!client.NextRetry(now+1000u,retry));
    };
    act(Action::Eject); act(Action::Reinsert); act(Action::Eject); act(Action::Insert);
}

int main()
{
    using namespace l4d2vr_remote_mag;
    CheckLostMagazineReplies(32,8,8);
    CheckLostMagazineReplies(2,23,50);
    CheckLostMagazineReplies(7,0,50);
    // Bounded retries retain exact identity and stop at lifecycle boundaries.
    {
        ClientRequest retryClient; Request original{}, resend{}; Reply result{};
        retryClient.Offer(kVersion,92u);
        Check(retryClient.Begin(0x3005u,2,0,100,Action::Eject,200u,10u,1u,1000u,original));
        Check(!retryClient.NextRetry(999u,resend) && !retryClient.NextRetry(1499u,resend));
        Check(retryClient.NextRetry(1500u,resend) && resend == original);
        Check(!retryClient.NextRetry(1500u,resend) && !retryClient.NextRetry(1499u,resend));
        Check(retryClient.NextRetry(2000u,resend) && retryClient.NextRetry(2500u,resend));
        Check(!retryClient.NextRetry(3000u,resend));
        Check(retryClient.Update(0x3005u,2,10u,1u,3500u,result,0,100) == ClientRequest::Poll::Timeout);
        Check(!retryClient.NextRetry(3500u,resend) && !retryClient.Supported());
        retryClient.Offer(kVersion,93u);
        Check(retryClient.Begin(0x3005u,2,0,100,Action::Eject,201u,10u,1u,4000u,original));
        Check(retryClient.Update(0x3005u,2,10u,2u,4100u,result,0,100) == ClientRequest::Poll::None);
        Check(!retryClient.NextRetry(4500u,resend));
        Check(retryClient.Begin(0x3005u,2,0,100,Action::Eject,202u,10u,2u,5000u,original));
        Check(!retryClient.NextRetry(5499u,resend) && retryClient.NextRetry(5500u,resend));
        retryClient.Cancel(); Check(!retryClient.NextRetry(6000u,resend));
        Check(retryClient.Begin(0x3005u,2,0,100,Action::Eject,203u,10u,2u,7000u,original));
        Check(retryClient.Update(0x3006u,2,10u,2u,7100u,result,0,100) == ClientRequest::Poll::None);
        Check(!retryClient.NextRetry(7500u,resend));
    }
    const std::uint32_t handle = (3u << 12) | 27u;
    {
        ServerLedger nonReloadable; nonReloadable.Reset(10u);
        const Snapshot m60{true,handle,200u,37,150,150,150,false};
        const Request request{10u,1u,handle,200u,37,150,150,Action::Eject};
        int attemptedWrites = 0;
        const auto result = nonReloadable.Apply(request,m60,1000u,
            [&](int,int) { ++attemptedWrites; return true; });
        Check(result.status == Status::Weapon && attemptedWrites == 0);
    }
    ServerLedger server; server.Reset(10u);
    Snapshot state{true, handle, 200u, 2, 23, 7, 50, false};
    Request eject{10u, 1u, handle, 200u, 2, 23, 7, Action::Eject};
    int writes = 0;
    auto writer = [&](int clip, int reserve) { ++writes; state.clip = clip; state.reserve = reserve; return true; };
    auto out = server.Apply(eject, state, 1000u, writer);
    Check(out.status == Status::Applied && state.clip == 1 && state.reserve == 7 && writes == 1);
    Check(server.Apply(eject, state, 1200u, writer).clip == 1 && writes == 1);
    Request altered = eject; altered.action = Action::Insert;
    Check(server.Apply(altered, state, 1300u, writer).status == Status::Stale && writes == 1);
    // A fresh magazine with only seven reserve rounds is partially loaded.
    Request insert{10u, 2u, handle, 200u, 2, 1, 7, Action::Insert};
    out = server.Apply(insert, state, 1400u, writer);
    Check(out.status == Status::Applied && state.clip == 8 && state.reserve == 0 && writes == 2);
    Check(server.Apply(insert, state, 1600u, writer).status == Status::Applied && writes == 2);
    insert.sequence = 3; insert.clip = state.clip; insert.reserve = state.reserve;
    Check(server.Apply(insert, state, 1700u, writer).status == Status::Weapon && writes == 2);

    // The listen-host uses this same path even while its local weapon hooks
    // are active. A shot spending the retained chamber is real native ammo.
    server.Reset(40u); state = {true, handle, 300u, 2, 20, 2, 50, false};
    eject = {40u, 1u, handle, 300u, 2, 20, 2, Action::Eject};
    Check(server.Apply(eject, state, 1800u, writer).status == Status::Applied && state.clip == 1);
    state.clip = 0; // native shot between ejection and insertion
    insert = {40u, 2u, handle, 300u, 2, 1, 2, Action::Insert};
    const int beforeShot = writes;
    Check(server.Apply(insert, state, 2000u, writer).status == Status::Stale && writes == beforeShot);
    insert.sequence = 3; insert.clip = 0;
    Check(server.Apply(insert, state, 2200u, writer).status == Status::Applied && state.clip == 2 && state.reserve == 0);

    // An empty gun keeps no chambered round. Infinite ammo comes from the host,
    // not from the weapon ID or a client-controlled request flag.
    server.Reset(11u); state = {true, handle, 200u, 1, 0, 0, 15, false};
    eject = {11u, 1u, handle, 200u, 1, 0, 0, Action::Eject};
    Check(server.Apply(eject, state, 2000u, writer).status == Status::Applied && state.clip == 0);
    insert = {11u, 2u, handle, 200u, 1, 0, 0, Action::Insert};
    Check(server.Apply(insert, state, 2200u, writer).status == Status::NoSpaceOrAmmo && state.clip == 0);
    state.infinite = true; insert.sequence = 3;
    out = server.Apply(insert, state, 2400u, writer);
    Check(out.status == Status::Applied && state.clip == 15 && state.reserve == 0);

    // A reloadable rifle with a custom script capacity uses its native limit.
    server.Reset(12u); state = {true, handle, 200u, 5, 120, 149, 150, false};
    eject = {12u, 1u, handle, 200u, 5, 120, 149, Action::Eject};
    Check(server.Apply(eject, state, 3000u, writer).status == Status::Applied);
    insert = {12u, 2u, handle, 200u, 5, 1, 149, Action::Insert};
    Check(server.Apply(insert, state, 3200u, writer).status == Status::Applied && state.clip == 150 && state.reserve == 0);

    // Native changes, stale commands, recycled handles and invalid owners
    // cannot be overwritten by an insertion from an earlier physical gesture.
    server.Reset(13u); state = {true, handle, 200u, 32, 0, 16, 8, false};
    eject = {13u, 1u, handle, 200u, 32, 0, 16, Action::Eject};
    Check(server.Apply(eject, state, 4000u, writer).status == Status::Applied);
    insert = {13u, 2u, handle, 200u, 32, 0, 16, Action::Insert};
    state.reserve = 15;
    Check(server.Apply(insert, state, 4200u, writer).status == Status::Stale);
    insert.sequence = 3; insert.reserve = 15; insert.command = 151u;
    Check(server.Apply(insert, state, 4300u, writer).status == Status::Stale);
    insert.sequence = 4; insert.command = 209u;
    Check(server.Apply(insert, state, 4400u, writer).status == Status::Stale);
    insert.sequence = 5; insert.command = 200u; insert.handle += 4096u;
    Check(server.Apply(insert, state, 4500u, writer).status == Status::Weapon);
    insert.sequence = 6; insert.handle = handle; state.eligible = false;
    Check(server.Apply(insert, state, 4600u, writer).status == Status::Weapon);
    state.eligible = true; insert.sequence = 7;
    const int beforeFailed = writes;
    Check(server.Apply(insert, state, 4700u, [](int, int) { return false; }).status == Status::Backend);
    Check(server.Apply(insert, state, 4800u, writer).status == Status::Backend && writes == beforeFailed);
    insert.sequence = 8;
    Check(server.Apply(insert, state, 4900u, writer).status == Status::Applied && state.clip == 8 && state.reserve == 7);

    // Leaving and redrawing an ejected weapon can repeat ejection safely.
    eject.sequence = 9; eject.clip = 8; eject.reserve = 7;
    Check(server.Apply(eject, state, 5100u, writer).status == Status::Applied && state.clip == 1);
    eject.sequence = 10; eject.clip = 1;
    Check(server.Apply(eject, state, 5300u, writer).status == Status::Applied && state.clip == 1 && state.reserve == 7);
    insert.sequence = 11; insert.clip = 1; insert.reserve = 7;
    Check(server.Apply(insert, state, 5350u, writer).status == Status::RateLimited);
    insert.sequence = 12;
    Check(server.Apply(insert, state, 5500u, writer).status == Status::Applied && state.clip == 8 && state.reserve == 0);

    // ACK delivery and Source replication can arrive in either order. Neither
    // one alone advances the physical reload to its next phase.
    ClientRequest client; Reply reply{}; Request request{};
    client.Offer(kVersion, 20u);
    Check(client.Begin(handle, 2, 23, 7, Action::Eject, 200u, 100u, 4u, 6000u, request));
    Check(!client.Begin(handle, 2, 23, 7, Action::Eject, 200u, 100u, 4u, 6001u, request));
    Check(client.Update(handle, 2, 100u, 4u, 6100u, reply, 1, 7) == ClientRequest::Poll::Waiting);
    Reply ack{request, Status::Applied, 1, 7};
    auto wrong = ack; ++wrong.request.sequence; client.Receive(wrong);
    Check(client.Update(handle, 2, 100u, 4u, 6200u, reply, 1, 7) == ClientRequest::Poll::Waiting);
    client.Receive(ack);
    Check(client.Update(handle, 2, 100u, 4u, 6300u, reply, 23, 7) == ClientRequest::Poll::Waiting);
    Check(client.Update(handle, 2, 100u, 4u, 6400u, reply, 1, 7) == ClientRequest::Poll::Result);
    Check(client.Begin(handle, 2, 1, 7, Action::Insert, 210u, 100u, 4u, 6500u, request));
    ack = {request, Status::Applied, 8, 0}; client.Receive(ack);
    Check(client.Update(handle, 2, 100u, 4u, 6600u, reply, 8, 7) == ClientRequest::Poll::Waiting);
    Check(client.Update(handle, 2, 100u, 4u, 6700u, reply, 8, 0) == ClientRequest::Poll::Result);
    // Invalid transfers cannot advance the client. Infinite reserve remains
    // unchanged only when the host's applied result confirms it.
    Check(client.Begin(handle, 1, 0, 0, Action::Insert, 211u, 100u, 4u, 6800u, request));
    ack = {request, Status::Applied, 15, 1}; client.Receive(ack);
    Check(client.Update(handle, 1, 100u, 4u, 6900u, reply, 15, 1) == ClientRequest::Poll::Waiting);
    ack.reserve = 0; client.Receive(ack);
    Check(client.Update(handle, 1, 100u, 4u, 7000u, reply, 15, 0) == ClientRequest::Poll::Result);
    Check(client.Begin(handle, 2, 0, 10, Action::Insert, 212u, 100u, 4u, 7100u, request));
    Check(client.Update(handle, 2, 101u, 4u, 7200u, reply, 0, 10) == ClientRequest::Poll::None && !client.Pending());
    Check(client.Begin(handle, 2, 0, 10, Action::Insert, 213u, 100u, 4u, 7300u, request));
    Check(client.Update(handle, 2, 100u, 5u, 7400u, reply, 0, 10) == ClientRequest::Poll::None);
    Check(client.Begin(handle, 2, 0, 10, Action::Insert, 214u, 100u, 5u, 7500u, request));
    Check(client.Update(handle, 2, 100u, 5u, 10000u, reply, 0, 10) == ClientRequest::Poll::Timeout && !client.Supported());
    client.Offer(kVersion, 20u); Check(!client.Supported());
    client.Offer(kVersion, 21u); Check(client.Supported());
    Check(client.Begin(handle, 2, 0, 10, Action::Insert, 215u, 100u, 5u, 10100u, request));
    ack = {request, Status::Unsupported, 0, 0}; client.Receive(ack);
    Check(client.Update(handle, 2, 100u, 5u, 10200u, reply, 0, 10) == ClientRequest::Poll::Result && !client.Supported());
    client.Disconnect(); client.Offer(kVersion + 1u, 22u); Check(!client.Supported());
    // Bound the server's remembered dropped guns without allowing insertion
    // into an evicted entry. Redrawing and ejecting it restores that phase.
    server.Reset(30u);
    for (unsigned i = 0; i < 17u; ++i)
    {
        state = {true, handle + i, 300u, 2, 0, 10, 50, false};
        eject = {30u, i + 1u, state.handle, 300u, 2, 0, 10, Action::Eject};
        Check(server.Apply(eject, state, 11000u + i * 200u, writer).status == Status::Applied);
    }
    state = {true, handle, 300u, 2, 0, 10, 50, false};
    insert = {30u, 18u, handle, 300u, 2, 0, 10, Action::Insert};
    Check(server.Apply(insert, state, 15000u, writer).status == Status::Weapon);
    eject = {30u, 19u, handle, 300u, 2, 0, 10, Action::Eject};
    Check(server.Apply(eject, state, 15200u, writer).status == Status::Applied);
    insert.sequence = 20u;
    Check(server.Apply(insert, state, 15400u, writer).status == Status::Applied && state.clip == 10 && state.reserve == 0);
    // Catch and reinsert uses the actual removed rounds, never reserve ammo.
    server.Reset(50u); state = {true, handle, 400u, 2, 18, 7, 50, false};
    eject = {50u, 1u, handle, 400u, 2, 18, 7, Action::Eject};
    Check(server.Apply(eject, state, 16000u, writer).status == Status::Applied && state.clip == 1);
    insert = {50u, 2u, handle, 400u, 2, 1, 7, Action::Reinsert};
    const int beforeReuse = writes;
    Check(server.Apply(insert, state, 16200u, writer).status == Status::Applied && state.clip == 18 && state.reserve == 7);
    Check(server.Apply(insert, state, 16400u, writer).status == Status::Applied && writes == beforeReuse + 1);
    insert.sequence = 3u; insert.clip = 18;
    Check(server.Apply(insert, state, 16600u, writer).status == Status::Weapon);
    // Spending the retained chamber does not refill it from the old magazine.
    eject.sequence = 4u; eject.clip = 18;
    Check(server.Apply(eject, state, 16800u, writer).status == Status::Applied);
    state.clip = 0;
    insert = {50u, 5u, handle, 400u, 2, 0, 7, Action::Reinsert};
    Check(server.Apply(insert, state, 17000u, writer).status == Status::Applied && state.clip == 17 && state.reserve == 7);
    // A changed script cannot truncate a retained magazine or mint rounds.
    eject.sequence = 6u; eject.clip = 17;
    Check(server.Apply(eject, state, 17200u, writer).status == Status::Applied);
    state.capacity = 8; insert.sequence = 7u; insert.clip = 1;
    Check(server.Apply(insert, state, 17400u, writer).status == Status::NoSpaceOrAmmo && state.clip == 1);
    state.capacity = 50; insert.sequence = 8u;
    Check(server.Apply(insert, state, 17600u, writer).status == Status::Applied && state.clip == 17);
    // Infinite ammo does not replace a partially used retained magazine.
    server.Reset(51u); state = {true, handle, 400u, 1, 4, 0, 15, true};
    eject = {51u, 1u, handle, 400u, 1, 4, 0, Action::Eject};
    Check(server.Apply(eject, state, 18000u, writer).status == Status::Applied);
    insert = {51u, 2u, handle, 400u, 1, 1, 0, Action::Reinsert};
    Check(server.Apply(insert, state, 18200u, writer).status == Status::Applied && state.clip == 4 && state.reserve == 0);
    // Reinserting an empty magazine is valid and transfers no ammunition.
    server.Reset(52u); state = {true, handle, 400u, 1, 0, 0, 15, true};
    eject = {52u, 1u, handle, 400u, 1, 0, 0, Action::Eject};
    Check(server.Apply(eject, state, 18400u, writer).status == Status::Applied);
    insert = {52u, 2u, handle, 400u, 1, 0, 0, Action::Reinsert};
    Check(server.Apply(insert, state, 18600u, writer).status == Status::Applied && state.clip == 0 && state.reserve == 0);
    // The result still waits for Source replication and exact request identity.
    client.Disconnect(); client.Offer(1u, 60u); Check(!client.Supported());
    client.Offer(kVersion, 60u);
    Check(client.Begin(handle, 2, 1, 7, Action::Reinsert, 400u, 100u, 10u, 19000u, request));
    ack = {request, Status::Applied, 18, 6}; client.Receive(ack);
    Check(client.Update(handle, 2, 100u, 10u, 19100u, reply, 18, 6) == ClientRequest::Poll::Waiting);
    ack.reserve = 7; client.Receive(ack);
    Check(client.Update(handle, 2, 100u, 10u, 19200u, reply, 1, 7) == ClientRequest::Poll::Waiting);
    Check(client.Update(handle, 2, 100u, 10u, 19300u, reply, 18, 7) == ClientRequest::Poll::Result);
    Check(client.Begin(handle, 1, 0, 0, Action::Reinsert, 401u, 100u, 10u, 19400u, request));
    ack = {request, Status::Applied, 0, 0}; client.Receive(ack);
    Check(client.Update(handle, 1, 100u, 10u, 19500u, reply, 0, 0) == ClientRequest::Poll::Result);
    // Fresh replacement discards the old catch; it cannot be reused afterward.
    server.Reset(53u); state = {true, handle, 400u, 2, 18, 7, 50, false};
    eject = {53u, 1u, handle, 400u, 2, 18, 7, Action::Eject};
    Check(server.Apply(eject, state, 20000u, writer).status == Status::Applied);
    insert = {53u, 2u, handle, 400u, 2, 1, 7, Action::Insert};
    Check(server.Apply(insert, state, 20200u, writer).status == Status::Applied && state.clip == 8);
    insert = {53u, 3u, handle, 400u, 2, 8, 0, Action::Reinsert};
    Check(server.Apply(insert, state, 20400u, writer).status == Status::Weapon);
    // A new ejection gesture after cancellation cannot recreate the old item.
    server.Reset(54u); state = {true, handle, 400u, 2, 18, 7, 50, false};
    eject = {54u, 1u, handle, 400u, 2, 18, 7, Action::Eject};
    Check(server.Apply(eject, state, 20600u, writer).status == Status::Applied);
    eject.sequence = 2u; eject.clip = 1;
    Check(server.Apply(eject, state, 20800u, writer).status == Status::Applied);
    insert = {54u, 3u, handle, 400u, 2, 1, 7, Action::Reinsert};
    Check(server.Apply(insert, state, 21000u, writer).status == Status::Applied && state.clip == 1 && state.reserve == 7);
    // Failed native insertion keeps the retained item available for a retry.
    server.Reset(55u); state = {true, handle, 400u, 2, 12, 3, 50, false};
    eject = {55u, 1u, handle, 400u, 2, 12, 3, Action::Eject};
    Check(server.Apply(eject, state, 21200u, writer).status == Status::Applied);
    insert = {55u, 2u, handle, 400u, 2, 1, 3, Action::Reinsert};
    Check(server.Apply(insert, state, 21400u, [](int, int) { return false; }).status == Status::Backend);
    insert.sequence = 3u;
    Check(server.Apply(insert, state, 21600u, writer).status == Status::Applied && state.clip == 12 && state.reserve == 3);
    std::puts("Remote magazine regression checks passed");
}
