#include "../L4D2VR/vr_pistol_detachment.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed at line %d\n", __LINE__); std::abort(); } } while(false)

int main()
{
    using namespace l4d2vr_pistol;
    Ownership discontinuous;
    discontinuous.Observe(true,88u,true,100u);
    discontinuous.Release(true,false,true,false,101u,0u,0u);
    discontinuous.Release(true,true,true,true,102u,0u,0u);
    CHECK(discontinuous.Release(true,false,true,true,103u,1u,0u)==Hand::None);
    CHECK(discontinuous.Release(true,true,true,false,104u,1u,0u)==Hand::Left);
    CHECK(discontinuous.Mask()==3u); // the sent left drop remains pending while right is baselined
    Ownership cancelledPickup;
    cancelledPickup.Observe(true, 99u, false, 100u);
    cancelledPickup.PickupRequested(Hand::Left);
    cancelledPickup.CancelPickup(Hand::Left);
    cancelledPickup.Observe(true, 99u, true, 110u);
    CHECK(cancelledPickup.Mask() == 3u);
    CHECK(cancelledPickup.Release(true, false, true, true, 120u) == Hand::None);
    CHECK(cancelledPickup.Release(true, false, true, false, 130u) == Hand::None);
    CHECK(cancelledPickup.Release(true, false, true, true, 140u) == Hand::None);
    CHECK(cancelledPickup.Release(true, false, true, false, 150u) == Hand::Left);
    CHECK(DecodeDrop(60u, false) == Hand::Right);
    CHECK(DecodeDrop(61u, false) == Hand::Left);
    CHECK(DecodeDrop(61u, true) == Hand::None);
    CHECK(DecodeDrop(62u, false) == Hand::None);
    CHECK(DecodePickup(62u, l4d2vr_grip::kPickupImpulse, true, false) == Hand::Right);
    CHECK(DecodePickup(63u, l4d2vr_grip::kPickupImpulse, true, false) == Hand::Left);
    CHECK(DecodePickup(63u, l4d2vr_grip::kReleaseImpulse, true, false) == Hand::None);
    CHECK(DecodePickup(63u, l4d2vr_grip::kPickupImpulse, false, false) == Hand::None);
    CHECK(DecodePickup(63u, l4d2vr_grip::kPickupImpulse, true, true) == Hand::None);
    CHECK(DecodePickup(60u, l4d2vr_grip::kPickupImpulse, true, false) == Hand::None);
    AmmoSplit ammo{};
    for (int clip = 0; clip <= 30; ++clip)
    {
        CHECK(SplitAmmo(clip, ammo));
        CHECK(ammo.retained + ammo.dropped == clip);
        CHECK(ammo.retained <= 15 && ammo.dropped <= 15);
        int restored = -1;
        CHECK(JoinAmmo(ammo.retained, ammo.dropped, restored) && restored == clip);
    }
    CHECK(!SplitAmmo(-1, ammo) && !SplitAmmo(31, ammo));
    int invalid = -1;
    CHECK(!JoinAmmo(16, 1, invalid) && !JoinAmmo(1, -1, invalid));
    Ownership hands;
    hands.Observe(true, 100u, true, 0u);
    CHECK(hands.Mask() == 3u);
    CHECK(hands.Release(true, false, true, false, 0u) == Hand::None);
    CHECK(hands.Release(true, true, true, true, 10u) == Hand::None);
    CHECK(hands.Release(true, false, true, true, 20u) == Hand::Right);
    CHECK(hands.Mask() == 3u && hands.Awaiting()); // no visual detachment before confirmation
    CHECK(hands.Release(true, false, true, true, 30u) == Hand::None);
    hands.Observe(true, 100u, false, 40u);
    CHECK(hands.Mask() == 2u && !hands.Awaiting());
    CHECK(hands.Release(true, false, true, false, 50u) == Hand::Left);

    // Simultaneous releases are serialized through the native pair-to-single confirmation.
    hands.Reset(); hands.Observe(true, 100u, true, 0u);
    hands.Release(true, false, true, false, 0u);
    hands.Release(true, true, true, true, 10u);
    CHECK(hands.Release(true, false, true, false, 20u) == Hand::Right);
    CHECK(hands.Release(true, false, true, false, 30u) == Hand::None);
    hands.Observe(true, 100u, false, 40u);
    CHECK(hands.Release(true, false, true, false, 40u) == Hand::Left);

    // Tracking loss, menus, and holding a grip across a weapon switch do not drop.
    hands.Reset(); hands.Observe(true, 100u, true, 0u);
    CHECK(hands.Release(true, true, true, true, 1u) == Hand::None);
    CHECK(hands.Release(false, false, false, false, 2u) == Hand::None);
    CHECK(hands.Release(true, false, true, false, 3u) == Hand::None);
    hands.Observe(false, 100u, true, 4u);
    CHECK(hands.Mask() == 0u);
    hands.Observe(true, 200u, false, 5u);
    CHECK(hands.Release(true, true, true, true, 6u) == Hand::None);
    CHECK(hands.Release(true, false, true, false, 7u) == Hand::None);

    // A held grip on a confirmed pickup owns the newly acquired pistol.
    hands.AdoptPickup(Hand::Left);
    CHECK(hands.Mask() == 2u);
    CHECK(hands.Release(true, false, true, true, 8u) == Hand::None);
    hands.PickupRequested(Hand::Right);
    hands.Observe(true, 200u, true, 9u);
    CHECK(hands.Mask() == 3u);
    CHECK(hands.Release(true, true, true, true, 10u) == Hand::None);
    CHECK(hands.Release(true, false, true, true, 11u) == Hand::Right);
    hands.Observe(true, 200u, true, 2012u); // failure timeout never retries its release
    CHECK(!hands.Awaiting() && hands.Mask() == 3u);
    CHECK(hands.Release(true, false, true, true, 2013u) == Hand::None);
    hands.Observe(true, 200u, false, 2014u); // delayed authoritative split still retains left ownership
    CHECK(hands.Mask() == 2u);
    hands.CancelInteractions();
    CHECK(hands.Mask() == 2u && !hands.Awaiting());
    CHECK(hands.Release(true, false, true, false, 2015u) == Hand::None);
}
