#include "../L4D2VR/vr_pistol_ammo_sync.h"
#include "../L4D2VR/vr_pistol_ammo.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do { if (!(v)) std::abort(); } while(false)
int main()
{
    using namespace l4d2vr_pistol_sync;
    {
        // The feed must never turn the ledger's balanced fallback into a known
        // partition, including after native reloads and recycled entities.
        l4d2vr_pistol::AmmoLedger ledger;
        l4d2vr_pistol::AmmoSnapshot pair{10u, 50u, 2u, 3u, 16, true};
        CHECK(ledger.Joined(pair, 1, 15, l4d2vr_pistol::Hand::Left));
        int right = -1, left = -1;
        CHECK(ledger.Counts(pair, right, left) && right == 1 && left == 15);
        pair.clip = 15;
        CHECK(!ledger.Counts(pair, right, left) && right == 0 && left == 0);
        pair.clip = 30;
        CHECK(ledger.Counts(pair, right, left) && right == 15 && left == 15);
        pair.clip = 16; ++pair.weaponSerial;
        CHECK(!ledger.Counts(pair, right, left) && right == 0 && left == 0);
    }
    Sender server; server.Reset(42u); CHECK(!server.Ready()); CHECK(!server.Acknowledge(kVersion, 43u)); CHECK(!server.Acknowledge(1u, 42u)); CHECK(server.Acknowledge(kVersion, 42u) && server.Ready());
    State state{0u, 0u, 0x3005u, 100u, 15, 16, 1, 15, true, true}, wire{};
    Sender waiting; waiting.Reset(7u);
    CHECK(!waiting.Prepare(state, 1000u, wire));
    CHECK(!waiting.Acknowledge(1u, 7u) && !waiting.Ready());
    CHECK(!waiting.Acknowledge(2u, 7u) && !waiting.Ready());
    CHECK(waiting.Acknowledge(kVersion, 7u) && waiting.Prepare(state, 1000u, wire));
    CHECK(!waiting.Acknowledge(0u, 8u) && waiting.Ready());
    CHECK(waiting.Acknowledge(0u, 7u) && !waiting.Ready());
    CHECK(!waiting.Prepare(state, 1200u, wire));
    CHECK(waiting.Acknowledge(kVersion, 7u) && waiting.Prepare(state, 1200u, wire));
    CHECK(server.Prepare(state, 1000u, wire) && wire.token == 42u && wire.sequence == 1u);
    CHECK(!server.Prepare(state, 1100u, wire));
    Receiver client; CHECK(client.Token() == 0u && client.Offer(kVersion, 42u) && client.Token() == 42u);
    CHECK(client.Receive(wire, 1000u));
    State selected{};
    CHECK(client.Read(10u, 0x3005u, 16, true, 1100u, selected) && selected.right == 1 && selected.left == 15);
    CHECK(!client.Read(10u, 0x4005u, 16, true, 1100u, selected));
    CHECK(!client.Read(10u, 0x3005u, 15, true, 1100u, selected));
    CHECK(client.ReadBaseline(10u, 0x3005u, true, 1100u, selected) && selected.clip == 16);
    CHECK(!client.Read(10u, 0x3005u, 16, false, 1100u, selected));
    CHECK(!client.Receive(wire, 1100u));
    CHECK(!client.Read(10u, 0x3005u, 16, true, 2001u, selected));
    CHECK(!client.Read(10u, 0x3005u, 16, true, 999u, selected));
    CHECK(server.Prepare(state, 1200u, wire) && wire.sequence == 2u);
    CHECK(client.Receive(wire, 1200u));
    CHECK(client.Offer(kVersion, 42u) && client.Read(10u, 0x3005u, 16, true, 1200u, selected));
    CHECK(!client.Read(20u, 0x3005u, 16, true, 1250u, selected));
    CHECK(server.Prepare(state, 1400u, wire) && client.Receive(wire, 1400u));
    CHECK(client.Read(20u, 0x3005u, 16, true, 1400u, selected));
    auto forged = wire; forged.sequence++; forged.token = 43u; CHECK(!client.Receive(forged, 1450u));
    forged = wire; forged.sequence++; forged.right = 16; CHECK(!client.Receive(forged, 1450u));
    forged = wire; forged.sequence++; forged.left = 14; CHECK(!client.Receive(forged, 1450u));
    forged = wire; forged.sequence++; forged.command = 99u; CHECK(!client.Receive(forged, 1450u));
    state.command = 101u; state.known = false; state.right = state.left = 0;
    CHECK(server.Prepare(state, 1450u, wire) && client.Receive(wire, 1450u));
    CHECK(!client.Read(20u, 0x3005u, 16, true, 1450u, selected));
    state.command = 102u; state.known = true; state.right = 1; state.left = 15;
    CHECK(server.Prepare(state, 1500u, wire) && client.Receive(wire, 1500u));
    CHECK(!client.Offer(kVersion + 1u, 42u) && client.Token() == 0u); CHECK(!client.Receive(wire, 1550u));
    CHECK(client.Offer(kVersion, 99u)); CHECK(!client.Receive(wire, 1550u));
    server.Reset(99u); CHECK(!server.Ready() && server.Acknowledge(kVersion, 99u)); CHECK(server.Prepare(state, 1550u, wire) && client.Receive(wire, 1550u));
    CHECK(client.Read(20u, 0x3005u, 16, true, 1550u, selected));
    state.command = 100u; CHECK(!server.Prepare(state, 1560u, wire));
    state.command = 102u; CHECK(!server.Prepare(state, 1549u, wire));
    CHECK(!client.Read(0u, 0u, -1, false, 1600u, selected));
    CHECK(!client.Read(20u, 0x3005u, 16, true, 1600u, selected));
    auto rewind = wire; rewind.sequence++; rewind.command = 101u;
    CHECK(!client.Receive(rewind, 1600u)); // owner/menu clearing cannot reset command ordering
    rewind = wire; rewind.sequence++;
    CHECK(!client.Receive(rewind, 1549u)); // monotonic receive clock
    client.Disconnect(); CHECK(!client.Receive(wire, 1600u));
    state.capacity = 8; state.clip = 16; state.right = state.left = 8;
    CHECK(state.ValidContents()); state.capacity = 16; CHECK(!state.ValidContents());
    state.capacity = 8; state.handle = 0u; CHECK(!state.ValidContents());
    state.handle = 0x3005u; state.dual = false; state.clip = 2; state.right = state.left = 1;
    CHECK(!state.ValidContents());
    State physical{99u, 1u, 0x3005u, 1u, 15, 16, 1, 15, true, true, 1u, 2u, 3u, 1u};
    CHECK(physical.ValidContents());
    auto invalidPhysical = physical; invalidPhysical.right = 0; invalidPhysical.clip = 15;
    CHECK(!invalidPhysical.ValidContents());
    invalidPhysical = physical; invalidPhysical.physical = 0u;
    CHECK(!invalidPhysical.ValidContents());
    invalidPhysical = physical; invalidPhysical.dual = false; invalidPhysical.clip = 1; invalidPhysical.left = 0;
    CHECK(!invalidPhysical.ValidContents());
    Sender alias; alias.Reset(77u); CHECK(alias.Acknowledge(kVersion, 77u));
    State aliased{0u, 0u, 0x3005u, 1u, 15, 16, 1, 15, true, true};
    CHECK(alias.Prepare(aliased, 0u, aliased) && aliased.token == 77u && aliased.sequence == 1u);
    std::puts("Pistol authoritative snapshot checks passed");
}
