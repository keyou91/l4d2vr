#include "../L4D2VR/vr_pistol_ammo.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Pistol ammo failed at line %d\n", __LINE__); std::abort(); } } while(false)
int main()
{
    using namespace l4d2vr_pistol;
    AmmoLedger ledger;
    AmmoSnapshot pair{10u, 50u, 2u, 3u, 22, true};
    CHECK(ledger.Joined(pair, 7, 15, Hand::Left));
    auto before = pair;
    for (int command = 1; command <= 6; ++command)
    {
        auto after = before; --after.clip;
        CHECK(ledger.Shot(before, after, command, Hand::Right));
        before = after;
    }
    AmmoSplit split{}; bool exact = false;
    CHECK(ledger.Split(before, Hand::Right, split, exact) && exact);
    CHECK(split.retained == 15 && split.dropped == 1 && split.retained + split.dropped == before.clip);
    CHECK(ledger.Split(before, Hand::Left, split, exact) && exact);
    CHECK(split.retained == 1 && split.dropped == 15);
    auto single = before; single.dual = false; single.clip = 1;
    CHECK(ledger.Single(single, Hand::Right));
    auto rejoined = single; rejoined.dual = true; rejoined.clip = 16;
    CHECK(ledger.Joined(rejoined, 1, 15, Hand::Left));
    CHECK(ledger.Split(rejoined, Hand::Right, split, exact) && exact && split.dropped == 1);

    // Retaining the left gun, topping it up, and picking up a right pistol
    // preserves the original held hand even on the legacy pickup path.
    ledger.Reset(); single.clip = 4;
    CHECK(ledger.Single(single, Hand::Left));
    single.clip = 9; CHECK(ledger.Observe(single));
    pair.clip = 12; CHECK(ledger.Joined(pair, 9, 3, Hand::None));
    CHECK(ledger.Split(pair, Hand::Left, split, exact) && exact && split.dropped == 9 && split.retained == 3);
    CHECK(ledger.Joined(pair, 9, 3, Hand::Right));
    auto after = pair; --after.clip;
    CHECK(ledger.Shot(pair, after, 100, Hand::Left));
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 8 && split.retained == 3);
    CHECK(!ledger.Shot(pair, after, 100, Hand::Left)); // unchanged native replay cannot spend a second round
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 8);

    // Unknown/native changes cannot retain a stale per-hand partition.
    after.clip = 20;
    CHECK(ledger.Split(after, Hand::Left, split, exact) && !exact && split.retained + split.dropped == 20);
    after.clip = 30;
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.retained == 15 && split.dropped == 15);
    pair = after; after.clip = 29;
    CHECK(ledger.Shot(pair, after, 101, Hand::Left));
    auto recycled = after; ++recycled.weaponSerial;
    CHECK(ledger.Split(recycled, Hand::Left, split, exact) && !exact && split.retained + split.dropped == 29);
    recycled = after; ++recycled.ownerSerial;
    CHECK(ledger.Split(recycled, Hand::Left, split, exact) && !exact);
    recycled = after; ++recycled.owner;
    CHECK(ledger.Split(recycled, Hand::Left, split, exact) && !exact);

    // A native shot requested from an empty logical hand invalidates attribution;
    // the shared native clip remains authoritative until firing is separated.
    ledger.Reset(); pair.clip = 4;
    CHECK(ledger.Joined(pair, 0, 4, Hand::Left));
    after = pair; after.clip = 3;
    CHECK(!ledger.Shot(pair, after, 1, Hand::Right));
    CHECK(ledger.Split(after, Hand::Right, split, exact) && !exact && split.retained + split.dropped == 3);
    CHECK(!ledger.Joined(pair, 16, 0, Hand::Left));
    CHECK(!ledger.Joined(pair, 0, 3, Hand::Left));
    auto invalid = pair; invalid.clip = 31;
    CHECK(!ledger.Observe(invalid));
    invalid = pair; invalid.owner = 0u; CHECK(!ledger.Observe(invalid));
    invalid = pair; invalid.weapon = 0u; CHECK(!ledger.Observe(invalid));
    CHECK(!ledger.Split(pair, Hand::None, split, exact));
    CHECK(!ledger.Shot(pair, after, 0, Hand::Left));
    invalid = after; ++invalid.weaponSerial;
    CHECK(!ledger.Shot(pair, invalid, 2, Hand::Left));
    CHECK(!ledger.Shot(pair, pair, 2, Hand::Left));
    ledger.Reset(); pair.clip = 22;
    CHECK(ledger.Joined(pair, 7, 15, Hand::Left));
    for (unsigned serial = 4u; serial <= 20u; ++serial)
    {
        auto other = pair; other.weaponSerial = serial;
        CHECK(ledger.Observe(other));
    }
    CHECK(ledger.Split(pair, Hand::Right, split, exact) && !exact);
    CHECK(split.retained + split.dropped == 22);
    ledger.Reset();
    pair.capacity = 8; pair.clip = 16;
    CHECK(ledger.Split(pair, Hand::Left, split, exact) && exact && split.dropped == 8);
    after = pair; --after.clip;
    CHECK(ledger.Shot(pair, after, 1, Hand::Left));
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 7 && split.retained == 8);
    auto smaller = after; smaller.capacity = 10;
    CHECK(ledger.Split(smaller, Hand::Left, split, exact) && !exact);
    CHECK(!ledger.Joined(pair, 9, 7, Hand::Left));
    invalid = pair; invalid.capacity = 16; CHECK(!ledger.Observe(invalid));
    invalid = pair; invalid.capacity = 0; CHECK(!ledger.Observe(invalid));
    pair.capacity = 15;
    ledger.Reset();
    for (int clip = 0; clip <= 30; ++clip)
    {
        pair.clip = clip;
        CHECK(ledger.Split(pair, Hand::Left, split, exact));
        CHECK(split.retained >= 0 && split.dropped >= 0 && split.retained <= 15 && split.dropped <= 15);
        CHECK(split.retained + split.dropped == clip);
    }
    ledger.Reset(); pair.clip = 30;
    after = pair; after.clip = 27;
    CHECK(ledger.ObserveFire(pair, after, 200, Hand::Left, 0u, 3u));
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 12 && split.retained == 15);
    before = after; after.clip = 25;
    CHECK(ledger.ObserveFire(before, after, 200, Hand::Left, 3u, 5u));
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 10);
    CHECK(!ledger.ObserveFire(before, after, 200, Hand::Left, 3u, 5u));
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 10);
    auto wrongGun = after; ++wrongGun.weaponSerial;
    CHECK(!ledger.ObserveFire(before, wrongGun, 201, Hand::Right, 5u, 7u));
    CHECK(ledger.Split(after, Hand::Left, split, exact) && exact && split.dropped == 10);
    before = after; after.clip = 24;
    CHECK(!ledger.ObserveFire(before, after, 201, Hand::Right, 5u, 7u)); // two events, one native round
    CHECK(ledger.Split(after, Hand::Left, split, exact) && !exact && split.retained + split.dropped == 24);
    CHECK(!ledger.ObserveFire(before, after, 202, Hand::Right, 7u, 5u)); // reversed counter
    CHECK(!ledger.Shot(before, after, 202, Hand::Right, 0));
    CHECK(!ledger.Shot(before, after, 202, Hand::Right, 16));
    std::puts("Pistol per-hand ammo conservation checks passed");
}
