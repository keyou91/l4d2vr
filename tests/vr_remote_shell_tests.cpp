#include "../L4D2VR/vr_remote_shells.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void CheckImpl(bool okay, int line)
{
    if (!okay) { std::fprintf(stderr, "Remote shell check failed at line %d\n", line); std::abort(); }
}
#define Check(value) CheckImpl((value), __LINE__)

static void CheckUnavailableShellRecovery(bool rejected, bool releasedOnRecovery)
{
    using namespace l4d2vr_shell;
    constexpr uint32_t handle = 0x1001u;
    ClientRequest client; ServerLedger host;
    client.Offer(kVersion, 801u); host.Reset(801u);
    Request request{};
    Check(client.Begin(handle,3,3,20,200u,100u,1u,1000u,request));
    Snapshot native{true,handle,200u,3,rejected ? 2 : 3,20,8};
    int clip=native.clip,reserve=native.reserve;
    const auto reply=host.Apply(request,native,1050u,[&](int c,int r){clip=c;reserve=r;return true;});
    Check(reply.status==(rejected ? Status::Stale : Status::Applied));
    bool held=ShellRemainsHeld(true,false,false,false);
    Check(held); // unavailable input is unknown, not a grip release
    Reply settled{};
    Check(client.Update(handle,3,100u,1u,1060u,settled,clip,reserve)==ClientRequest::Poll::Waiting);
    client.Receive(reply);
    Check(client.Update(handle,3,100u,1u,1070u,settled,clip,reserve)==ClientRequest::Poll::Result);
    Check(!client.Pending()); // reply/replication settlement does not wait for action recovery
    if (!rejected) Check(clip==4 && reserve==19);
    else
    {
        Check(held && RetainRejectedShell(settled.status));
        held=ShellRemainsHeld(held,!releasedOnRecovery,false,true);
        Check(held==!releasedOnRecovery);
        if (releasedOnRecovery) Check(!ShellRemainsHeld(held,true,false,true));
    }
}

static void CheckHeldShellSettlement(l4d2vr_shell::Status expectedStatus, bool quickReload, bool gripDown)
{
    using namespace l4d2vr_shell;
    constexpr uint32_t handle = 0x1001u;
    ClientRequest client;
    ServerLedger host;
    client.Offer(kVersion, 801u);
    host.Reset(expectedStatus == Status::Session ? 802u : 801u);
    Request request{};
    Check(client.Begin(handle,3,3,20,200u,100u,1u,1000u,request));
    Snapshot native{true,handle,200u,3,3,20,8};
    int commits = 0, nativeClip = native.clip, nativeReserve = native.reserve;
    const auto writer = [&](int clip, int reserve)
    {
        if (expectedStatus == Status::Backend) return false;
        ++commits; nativeClip = clip; nativeReserve = reserve; return true;
    };
    if (expectedStatus == Status::RateLimited)
    {
        const auto first = host.Apply(request,native,1000u,writer);
        Check(first.status == Status::Applied);
        client.Receive(first);
        Reply settled{};
        Check(client.Update(handle,3,100u,1u,1020u,settled,nativeClip,nativeReserve) == ClientRequest::Poll::Result);
        native.clip = nativeClip; native.reserve = nativeReserve;
        native.latestCommand = 201u;
        Check(client.Begin(handle,3,native.clip,native.reserve,201u,100u,1u,1050u,request));
    }
    if (expectedStatus == Status::Stale) native.clip = 2; // a shot reached the host before insertion
    if (expectedStatus == Status::NoSpaceOrAmmo) native.capacity = native.clip;
    if (expectedStatus == Status::Weapon) native.eligible = false;
    nativeClip = native.clip; nativeReserve = native.reserve;
    const int beforeClip = nativeClip, beforeReserve = nativeReserve, beforeCommits = commits;
    const auto reply = expectedStatus == Status::Unsupported ? host.Unsupported(request) :
        host.Apply(request,native,1050u,writer);
    Check(reply.status == expectedStatus);
    const bool held = ShellRemainsHeld(true,gripDown,quickReload);
    Reply settled{};
    Check(client.Update(handle,3,100u,1u,1060u,settled,nativeClip,nativeReserve) == ClientRequest::Poll::Waiting);
    Check(held == (gripDown || quickReload)); // same policy drives the pending shell's draw flag
    client.Receive(reply);
    if (expectedStatus == Status::Applied)
    {
        Check(commits == beforeCommits + 1 && nativeClip == beforeClip + 1 && nativeReserve == beforeReserve - 1);
        Check(client.Update(handle,3,100u,1u,1070u,settled,beforeClip,beforeReserve) == ClientRequest::Poll::Waiting);
    }
    else Check(commits == beforeCommits && nativeClip == beforeClip && nativeReserve == beforeReserve);
    const auto poll = client.Update(handle,3,100u,1u,1080u,settled,nativeClip,nativeReserve);
    Check(poll == ClientRequest::Poll::Result && settled.status == expectedStatus);
    Check(!client.Pending());
    const bool retained = held && RetainRejectedShell(settled.status);
    Check(retained == ((quickReload || gripDown) && (expectedStatus == Status::Stale ||
        expectedStatus == Status::RateLimited || expectedStatus == Status::NoSpaceOrAmmo)));
    if (retained)
    {
        RetryContact contact;
        contact.Reject(settled.status);
        Check(!contact.Allows(true) && !contact.Allows(true)); // rejection cannot submit another request at the port
        Check(!contact.Allows(false) && contact.Allows(true)); // withdrawal permits one new gesture
        Request next{};
        Check(client.Begin(handle,3,nativeClip,nativeReserve,202u,100u,1u,1700u,next));
        Check(next.sequence == request.sequence + 1u);
        client.Receive(reply); // the rejected insertion cannot settle its replacement
        Check(client.Update(handle,3,100u,1u,1710u,settled,nativeClip,nativeReserve) == ClientRequest::Poll::Waiting);
    }
    Check(client.Supported() == (expectedStatus != Status::Backend && expectedStatus != Status::Unsupported));
}

int main()
{
    using namespace l4d2vr_shell;
    uint32_t number = 0u;
    Check(ParseNumber("4294967295", 0xffffffffu, number) && number == 0xffffffffu);
    for (const char* bad : {"", "-1", "+1", "1.0", " 1", "1;quit", "4294967296", "00000000000"})
        Check(!ParseNumber(bad, 0xffffffffu, number));
    Check(!ParseNumber("129", 128u, number));
    const uint32_t handle = (7u << 12) | 25u;
    Check(HandleMatches(handle, 25, 7u));
    Check(HandleMatches(handle, 25, 0x4007u)); // native entity-list serial is truncated by the send proxy
    Check(!HandleMatches(handle, 25, 8u));
    Check(!ValidHandle(0u) && !ValidHandle(0xffffffffu) && !ValidHandle(2048u));
    ServerLedger ledger;
    ledger.Reset(10u);
    int clip = 3, reserve = 20, writes = 0;
    Snapshot state{true, handle, 200u, 3, clip, reserve, 8};
    Request request{10u, 1u, handle, 200u, 3, clip, reserve};
    auto writer = [&](int nextClip, int nextReserve) { clip = nextClip; reserve = nextReserve; ++writes; return true; };
    const auto result = ledger.Apply(request, state, 1000u, writer);
    Check(result.status == Status::Applied && result.clip == 4 && result.reserve == 19);
    state.clip = clip; state.reserve = reserve;
    auto duplicate = ledger.Apply(request, state, 1100u, writer);
    Check(duplicate.status == Status::Applied && writes == 1 && clip + reserve == 23);
    Request altered = request; altered.reserve = 19;
    Check(ledger.Apply(altered, state, 1200u, writer).status == Status::Stale && writes == 1);
    Request next{10u, 2u, handle, 200u, 3, 4, 19};
    Check(ledger.Apply(next, state, 1050u, writer).status == Status::RateLimited && writes == 1);
    next.sequence = 3u;
    Check(ledger.Apply(next, state, 1200u, writer).status == Status::Applied && writes == 2);
    state.clip = clip; state.reserve = reserve;
    next.sequence = 4u; // an old predicted baseline must not overwrite a shot or another reload
    Check(ledger.Apply(next, state, 1400u, writer).status == Status::Stale && writes == 2);
    next.sequence = 5u; next.clip = clip; next.reserve = reserve; next.command = 100u;
    Check(ledger.Apply(next, state, 1500u, writer).status == Status::Stale && writes == 2);
    next.sequence = 6u; next.command = 209u;
    Check(ledger.Apply(next, state, 1600u, writer).status == Status::Stale && writes == 2);
    next.sequence = 7u; next.command = 200u; next.handle += 4096u;
    Check(ledger.Apply(next, state, 1700u, writer).status == Status::Weapon && writes == 2);
    next.sequence = 8u; next.handle = handle; state.eligible = false;
    Check(ledger.Apply(next, state, 1800u, writer).status == Status::Weapon && writes == 2);
    state.eligible = true; state.clip = 8; next.clip = 8; next.sequence = 9u;
    Check(ledger.Apply(next, state, 1900u, writer).status == Status::NoSpaceOrAmmo && writes == 2);
    state.clip = 0; state.reserve = 0; next.clip = next.reserve = 0; next.sequence = 10u;
    Check(ledger.Apply(next, state, 2000u, writer).status == Status::NoSpaceOrAmmo && writes == 2);
    // Custom script capacity comes from the server and still moves exactly one existing round.
    state.clip = 11; state.reserve = 3; state.capacity = 12;
    next.clip = 11; next.reserve = 3; next.sequence = 11u;
    Check(ledger.Apply(next, state, 2100u, writer).status == Status::Applied && clip == 12 && reserve == 2);
    next.sequence = 12u; state.clip = next.clip = 2; state.reserve = next.reserve = 7;
    int failedWrites = 0;
    auto fail = [&](int, int) { ++failedWrites; return false; };
    Check(ledger.Apply(next, state, 2300u, fail).status == Status::Backend);
    Check(ledger.Apply(next, state, 2400u, writer).status == Status::Backend && failedWrites == 1 && writes == 3);
    ledger.Reset(20u);
    Check(ledger.Apply(next, state, 2500u, writer).status == Status::Session && writes == 3);
    next.token = 20u; next.sequence = 1u;
    Check(ledger.Unsupported(next).status == Status::Unsupported);
    Check(ledger.Apply(next, state, 2600u, writer).status == Status::Unsupported && writes == 3);

    // The host and guest both use this ledger. A native shot between the
    // contact gesture and request cannot be overwritten by its old clip count.
    ledger.Reset(40u);
    state = {true, handle, 300u, 3, 4, 2, 8};
    next = {40u, 1u, handle, 300u, 3, 4, 2};
    state.clip = 3;
    const int beforeShot = writes;
    Check(ledger.Apply(next, state, 3000u, writer).status == Status::Stale && writes == beforeShot);
    next.sequence = 2u; next.clip = 3;
    Check(ledger.Apply(next, state, 3200u, writer).status == Status::Applied && clip == 4 && reserve == 1);
    Check(ledger.Apply(next, state, 3400u, writer).status == Status::Applied && writes == beforeShot + 1);

    // A lost reply may be retried without spending the same physical shell twice.
    ServerLedger retryHost; retryHost.Reset(90u);
    ClientRequest retryClient; retryClient.Offer(kVersion,90u);
    Request retryOriginal{},resend{};
    Check(retryClient.Begin(handle,3,3,20,200u,100u,1u,1000u,retryOriginal));
    int retryWrites = 0, retryClip = 3, retryReserve = 20;
    auto retryWriter = [&](int c,int r) { ++retryWrites; retryClip=c; retryReserve=r; return true; };
    Snapshot retryState{true,handle,200u,3,3,20,8};
    const auto lostReply = retryHost.Apply(retryOriginal,retryState,1000u,retryWriter);
    Check(lostReply.status == Status::Applied && retryWrites == 1);
    Reply retryResult{};
    Check(retryClient.Update(handle,3,100u,1u,1400u,retryResult,4,19) == ClientRequest::Poll::Waiting);
    Check(!retryClient.NextRetry(1499u,resend) && retryClient.NextRetry(1500u,resend) && resend == retryOriginal);
    retryState.clip=retryClip; retryState.reserve=retryReserve;
    const auto recovered = retryHost.Apply(resend,retryState,1500u,retryWriter);
    Check(recovered.status == Status::Applied && retryWrites == 1 && retryClip == 4 && retryReserve == 19);
    retryClient.Receive(recovered);
    Check(!retryClient.NextRetry(2000u,resend)); // acknowledgement waits only for replication
    Check(retryClient.Update(handle,3,100u,1u,2000u,retryResult,3,20) == ClientRequest::Poll::Waiting);
    Check(retryClient.Update(handle,3,100u,1u,2100u,retryResult,4,19) == ClientRequest::Poll::Result);
    Check(!retryClient.NextRetry(2200u,resend));
    retryClient.Disconnect(); retryClient.Offer(kVersion,91u);
    Check(retryClient.Begin(handle,3,3,20,200u,100u,1u,1000u,retryOriginal));
    Check(retryClient.NextRetry(1500u,resend) && retryClient.NextRetry(2000u,resend) && retryClient.NextRetry(2500u,resend));
    Check(!retryClient.NextRetry(3000u,resend) && !retryClient.NextRetry(999u,resend));
    Check(retryClient.Update(handle,3,100u,1u,3500u,retryResult) == ClientRequest::Poll::Timeout && !retryClient.Supported());
    retryClient.Disconnect(); retryClient.Offer(kVersion,92u);
    Check(retryClient.Begin(handle,3,3,20,200u,100u,1u,1000u,retryOriginal));
    Check(retryClient.Update(handle,3,100u,2u,1200u,retryResult) == ClientRequest::Poll::None);
    Check(!retryClient.NextRetry(1500u,resend)); // inventory generation changed
    RetryContact contact;
    for (const auto status : {Status::Stale,Status::RateLimited,Status::NoSpaceOrAmmo})
    {
        Check(RetainRejectedShell(status)); contact.Reject(status);
        Check(!contact.Allows(true) && !contact.Allows(true));
        Check(!contact.Allows(false) && contact.Allows(true)); // withdraw and return
    }
    Check(!RetainRejectedShell(Status::Applied) && !RetainRejectedShell(Status::Backend) && !RetainRejectedShell(Status::Unsupported));
    contact.Reject(Status::Stale); contact.Reset(); Check(contact.Allows(true));

    Check(ShellRemainsHeld(true,false,true) && ShellRemainsHeld(true,true,false));
    const bool released = ShellRemainsHeld(true,false,false);
    Check(!released && !ShellRemainsHeld(released,true,false)); // pressing grip cannot resurrect a dropped pending shell
    Check(!ShellRemainsHeld(false,true,true)); // quick mode cannot manufacture a held shell
    Check(ShellRemainsHeld(true,false,false,false) && !ShellRemainsHeld(false,false,false,false));
    for (bool rejected : {false,true})
        for (bool releasedOnRecovery : {false,true})
            CheckUnavailableShellRecovery(rejected,releasedOnRecovery);
    for (const bool quickReload : {false,true})
        for (const bool gripDown : {false,true})
            for (const auto status : {Status::Applied,Status::Stale,Status::RateLimited,Status::NoSpaceOrAmmo,
                Status::Session,Status::Weapon,Status::Backend,Status::Unsupported})
                CheckHeldShellSettlement(status,quickReload,gripDown);

    ClientRequest canceled;
    canceled.Offer(kVersion,900u);
    Request canceledRequest{};
    Check(canceled.Begin(handle,3,3,20,200u,100u,1u,1000u,canceledRequest));
    canceled.Cancel();
    canceled.Receive({canceledRequest,Status::Applied,4,19});
    Reply canceledResult{};
    Check(canceled.Update(handle,3,100u,1u,1100u,canceledResult,4,19) == ClientRequest::Poll::None);
    Check(!canceled.NextRetry(1500u,resend) && canceled.Supported());
    Check(canceled.Begin(handle,3,3,20,201u,100u,1u,1600u,canceledRequest));
    Check(canceled.Update(handle,3,100u,1u,4100u,canceledResult) == ClientRequest::Poll::Timeout);
    Check(!canceled.Pending() && !canceled.Supported());
    contact.Reject(Status::Stale); contact.Reset(); // dropping or replacing the rejected shell ends its contact gate
    Check(contact.Allows(true));

    ClientRequest client;
    client.Offer(2u, 10u); Check(!client.Supported());
    client.Offer(kVersion, 10u); Check(client.Supported());
    Request outgoing{};
    Check(client.Begin(handle, 3, 3, 20, 200u, 100u, 1u, 1000u, outgoing));
    Request extra{};
    Check(!client.Begin(handle, 3, 3, 20, 200u, 100u, 1u, 1001u, extra));
    Reply reply{outgoing, Status::Applied, 5, 18}; // forbid an acknowledgement that mints two shells
    client.Receive(reply);
    Reply received{};
    Check(client.Update(handle, 3, 100u, 1u, 1100u, received) == ClientRequest::Poll::Waiting);
    reply.clip = 4; reply.reserve = 19; client.Receive(reply);
    Check(client.Update(handle, 3, 100u, 2u, 1100u, received) == ClientRequest::Poll::None); // weapon/session reset
    Check(!client.Pending());
    Check(client.Begin(handle, 3, 3, 20, 201u, 100u, 2u, 1200u, outgoing));
    reply = {outgoing, Status::Applied, 4, 19}; client.Receive(reply);
    Check(client.Update(handle, 3, 100u, 2u, 1250u, received, 3, 20) == ClientRequest::Poll::Waiting);
    Check(client.Update(handle, 3, 100u, 2u, 1270u, received, 4, 20) == ClientRequest::Poll::Waiting);
    Check(client.Update(handle, 3, 100u, 2u, 1300u, received, 4, 19) == ClientRequest::Poll::Result);
    Check(received.clip == 4 && !client.Pending());
    client.Receive(reply); // late duplicate must not complete the next insertion
    Check(client.Begin(handle, 3, 4, 19, 202u, 100u, 2u, 1400u, outgoing));
    client.Receive(reply);
    Check(client.Update(handle, 3, 100u, 2u, 1500u, received) == ClientRequest::Poll::Waiting);
    Check(client.Update(handle, 3, 100u, 2u, 1400u + kReplyTimeoutMs, received) == ClientRequest::Poll::Timeout);
    Check(!client.Pending() && !client.Supported());
    client.Offer(kVersion, 10u); Check(!client.Supported()); // repeated offer cannot revive an unresolved lease
    client.Offer(kVersion, 20u); Check(client.Supported());
    Check(!client.Begin(handle, 1, 4, 19, 202u, 100u, 2u, 5000u, outgoing)); // no pistol authority in this protocol
    Check(client.Begin(handle, 3, 4, 19, 202u, 100u, 2u, 5000u, outgoing));
    Check(client.Update(handle, 3, 200u, 2u, 5010u, received) == ClientRequest::Poll::None);
    client.Disconnect(); Check(!client.Supported() && !client.Pending());
    client.Offer(kVersion, 30u);
    Check(client.Begin(handle, 3, 4, 19, 202u, 100u, 2u, 6000u, outgoing));
    reply = {outgoing, Status::Unsupported, 0, 0}; client.Receive(reply);
    Check(client.Update(handle, 3, 100u, 2u, 6010u, received) == ClientRequest::Poll::Result);
    Check(!client.Supported() && !client.Pending());
}
