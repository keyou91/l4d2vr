#include "../L4D2VR/vr_physical_controls.h"
#include "../L4D2VR/vr_grip_release.h"
#include <cstdlib>
#include <cstdio>
#include <limits>

static void CheckImpl(bool result, int line)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d\n", line); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__)

int main()
{
    using namespace l4d2vr_physical;
    Check(MeleeSweepSamples(0.0f, 0.0f, 40.0f) == 0);
    Check(MeleeSweepSamples(4.0f, 0.0f, 40.0f) == 5);
    Check(MeleeSweepSamples(0.0f, 80.0f, 40.0f) == 8);
    Check(MeleeSweepSamples(19.0f, 180.0f, 40.0f) == 12);
    Check(MeleeSweepSamples(21.0f, 30.0f, 40.0f) == 0);
    Check(MeleeSweepSamples(std::numeric_limits<float>::infinity(), 1.0f, 40.0f) == 0);
    MeleeMotion melee;
    Check(!melee.Update(true, 100u, 10u, 2.0f, 0.0f)); // wait for rest on weapon adoption
    Check(!melee.Update(true, 100u, 10u, 0.0f, 0.0f));
    Check(melee.Update(true, 100u, 10u, 1.2f, 0.0f)); // no attack button involved
    Check(melee.Update(true, 100u, 10u, 0.7f, 0.0f)); // retain the swing across threshold noise
    Check(!melee.Update(true, 100u, 10u, 0.4f, 0.0f));
    Check(melee.Update(true, 100u, 10u, 0.0f, 200.0f)); // a stationary wrist rotates the blade
    Check(!melee.Update(false, 100u, 10u, 2.0f, 0.0f)); // tracking, menu or incap cancels
    Check(!melee.Update(true, 100u, 10u, 2.0f, 0.0f));
    Check(!melee.Update(true, 100u, 10u, 0.0f, 0.0f));
    Check(melee.Update(true, 100u, 10u, 1.2f, 0.0f));
    Check(!melee.Update(true, 100u, 20u, 1.2f, 0.0f)); // weapon swap cannot carry a swing
    Check(!melee.Update(true, 100u, 20u, 0.0f, 0.0f));
    Check(melee.Update(true, 100u, 20u, 1.2f, 0.0f));
    Check(!melee.Update(true, 200u, 20u, 1.2f, 0.0f)); // player replacement
    Check(!melee.Update(true, 200u, 20u, std::numeric_limits<float>::quiet_NaN(), 0.0f));
    Check(!melee.Update(true, 200u, 20u, 0.0f, std::numeric_limits<float>::infinity()));
    Check(MeleeTrackingFresh(100u, 100u));
    Check(MeleeTrackingFresh(250u, 100u));
    Check(!MeleeTrackingFresh(251u, 100u));
    Check(!MeleeTrackingFresh(99u, 100u));
    Check(MeleeTrackingFresh(20u, 0xfffffff0u)); // monotonic timer wraps

    MeleeCommands meleeCommands;
    MeleeCommand moving{100, true, true, {1.0f, 2.0f, 3.0f}, {20.0f, 30.0f, 40.0f}};
    MeleeCommand resting{101, true, false, {4.0f, 5.0f, 6.0f}, {50.0f, 60.0f, 70.0f}};
    meleeCommands.Store(moving);
    meleeCommands.Store(resting);
    MeleeCommand captured{};
    Check(meleeCommands.Get(100, captured) && captured.swinging && captured.position[0] == 1.0f);
    Check(meleeCommands.Get(101, captured) && !captured.swinging && captured.position[0] == 4.0f);
    MeleeCommand latest{};
    SelectNewestMeleeCommand(latest, resting);
    SelectNewestMeleeCommand(latest, moving); // reversed decode order cannot restore the older swing
    Check(latest.command == 101 && !latest.swinging);
    latest = {};
    SelectNewestMeleeCommand(latest, moving);
    SelectNewestMeleeCommand(latest, resting);
    Check(latest.command == 101 && !latest.swinging);
    moving.command = 250; meleeCommands.Store(moving);
    Check(!meleeCommands.Get(100, captured)); // ring replacement cannot impersonate a backup command
    moving.command = 251; moving.position[0] = std::numeric_limits<float>::infinity();
    meleeCommands.Store(moving);
    Check(!meleeCommands.Get(251, captured));
    resting.command = 252; resting.valid = false; meleeCommands.Store(resting);
    Check(!meleeCommands.Get(252, captured));
    meleeCommands.Reset();
    Check(!meleeCommands.Get(250, captured));

    MeleeSweepHistory history;
    Check(history.Accept(100u, 10u, 100, true) == MeleeSweepHistory::Rebase);
    Check(history.Accept(100u, 10u, 101, true) == MeleeSweepHistory::Sweep);
    Check(history.Accept(100u, 10u, 101, true) == MeleeSweepHistory::Ignore);
    Check(history.Accept(100u, 10u, 100, true) == MeleeSweepHistory::Ignore);
    Check(history.Accept(100u, 20u, 102, true) == MeleeSweepHistory::Rebase);
    Check(history.Accept(100u, 20u, 103, true) == MeleeSweepHistory::Sweep);
    Check(history.Accept(100u, 20u, 112, true) == MeleeSweepHistory::Rebase); // command discontinuity
    Check(history.Accept(100u, 20u, 113, false) == MeleeSweepHistory::Rebase);
    Check(history.Accept(100u, 20u, 114, true) == MeleeSweepHistory::Rebase);
    Check(history.Accept(100u, 20u, 115, true) == MeleeSweepHistory::Sweep);
    Check(history.Accept(200u, 20u, 1, true) == MeleeSweepHistory::Rebase);
    Check(history.Accept(200u, 20u, 2, true) == MeleeSweepHistory::Sweep);
    Check(history.Accept(200u, 20u, 0, true) == MeleeSweepHistory::Ignore);
    Check(history.Accept(200u, 20u, 3, true) == MeleeSweepHistory::Rebase);
    ContactLatch contact;
    Check(!contact.Update(true, true, false));
    Check(contact.Update(true, true, true));
    Check(!contact.Update(true, true, true));
    Check(!contact.Update(false, true, true));
    Check(!contact.Update(true, true, true));
    Check(!contact.Update(true, false, true));
    Check(contact.Update(true, true, true));

    PickupIntent pickup;
    ReservedGrip reservedGrip;
    PickupIntent reservedPickup;
    Check(reservedGrip.AllowPickup(true, true, true, false));
    Check(!reservedGrip.AllowPickup(true, true, true, true));
    Check(reservedPickup.Update(reservedGrip.AllowPickup(true, true, true, true),
        true, true, true, 100u, 10u) == 0u);
    Check(reservedPickup.Update(reservedGrip.AllowPickup(true, true, true, false),
        true, true, true, 110u, 10u) == 0u); // finishing reload does not reuse a held grip
    Check(!reservedGrip.AllowPickup(true, true, false, false));
    Check(reservedPickup.Update(reservedGrip.AllowPickup(true, true, true, false),
        true, true, true, 120u, 10u) == PickupIntent::RequestUse);
    Check(reservedPickup.Update(reservedGrip.AllowPickup(true, true, true, true),
        true, true, false, 130u, 20u) == 0u); // cancel before late native adoption
    Check(reservedPickup.Update(reservedGrip.AllowPickup(true, true, true, false),
        true, true, false, 140u, 20u) == 0u);
    Check(!reservedGrip.AllowPickup(false, true, true, false));
    Check(!reservedGrip.AllowPickup(true, true, true, false));
    Check(!reservedGrip.AllowPickup(true, true, false, false));
    Check(reservedGrip.AllowPickup(true, true, true, false));
    Check(!reservedGrip.AllowPickup(true, false, true, false));
    Check(!reservedGrip.AllowPickup(true, true, true, false));
    reservedGrip.Reset();
    Check(!reservedGrip.AllowPickup(true, true, true, false));
    Check(!reservedGrip.AllowPickup(true, true, false, false));
    Check(reservedGrip.AllowPickup(true, true, true, false));
    Check(pickup.Update(true, true, true, true, 100u, 10u) == PickupIntent::RequestUse);
    Check(pickup.Update(true, true, true, true, 110u, 10u) == 0u);
    Check(pickup.Update(true, true, true, false, 150u, 20u) == PickupIntent::AdoptWeapon);
    Check(pickup.Update(true, true, true, true, 160u, 20u) == 0u);
    Check(pickup.Update(true, true, false, true, 170u, 20u) == 0u);
    Check(pickup.Update(true, true, true, true, 180u, 20u) == PickupIntent::RequestUse);
    Check(pickup.Update(true, true, true, true, 900u, 30u) == 0u);
    Check(pickup.Update(false, true, true, true, 901u, 30u) == 0u);
    pickup.Begin(0xfffffff0u, 10u);
    Check(pickup.Update(true, true, true, false, 20u, 20u) == PickupIntent::AdoptWeapon);

    l4d2vr_grip::ReleaseLatch release;
    release.AdoptWeapon(20u);
    Check(!release.Update(true, true, true, 20u));
    Check(release.Update(true, true, false, 20u));
    Check(!release.Update(true, true, false, 20u));
    Check(!release.Update(true, true, true, 30u));
    Check(!release.Update(true, true, false, 30u));

    PumpCycles pumps;
    Check(!pumps.Observe(10u, 3, 8));
    Check(!pumps.Observe(10u, 3, 7)); // ammo corrections are not shot events
    pumps.NotifyShot(10u, 3, 7);
    Check(pumps.Observe(10u, 3, 7));
    Check(!pumps.Observe(20u, 8, 8));
    Check(pumps.Observe(10u, 3, 8)); // loading and switching cannot bypass cycling
    Check(!PumpCycles::StrokeComplete(false, 0.0f, 0.02f));
    Check(!PumpCycles::StrokeComplete(true, 0.08f, 0.02f));
    Check(PumpCycles::StrokeComplete(true, 0.01f, 0.02f));
    Check(!PumpCycles::StrokeComplete(true, std::numeric_limits<float>::quiet_NaN(), 0.02f));
    pumps.Complete(10u);
    Check(!pumps.Observe(10u, 3, 8));
    pumps.NotifyShot(10u, 3, 0);
    Check(pumps.Observe(10u, 3, 0));
    pumps.Reset();
    Check(!pumps.Observe(10u, 3, 0));

    pumps.ObserveOwner(true, 100u);
    pumps.NotifyShot(10u, 3, 7);
    pumps.ObserveOwner(true, 100u); // normal interaction reset on a weapon swap
    Check(!pumps.Observe(20u, 2, 50));
    pumps.ObserveOwner(true, 100u);
    Check(pumps.Observe(10u, 3, 7)); // drawing the shotgun again still requires a stroke
    pumps.ObserveOwner(false, 100u); // downed/dead/VR interaction unavailable
    Check(!pumps.Observe(10u, 3, 7));
    pumps.ObserveOwner(true, 100u);
    pumps.NotifyShot(10u, 3, 6);
    pumps.ObserveOwner(true, 200u); // a replacement player owns a different inventory
    Check(!pumps.Observe(10u, 3, 6));
    pumps.ObserveOwner(true, 200u);
    pumps.NotifyShot(10u, 8, 5);
    pumps.Complete(10u);
    Check(!pumps.Observe(10u, 8, 5));
    // Native handles distinguish a replacement gun at a recycled address.
    pumps.Reset(); pumps.ObserveOwner(true, 100u);
    pumps.NotifyShot(10u, 3, 7, 0x3005u);
    Check(pumps.Observe(10u, 3, 8, 0x3005u));
    Check(!pumps.Observe(20u, 8, 8, 0x3006u));
    Check(pumps.Observe(10u, 3, 8, 0x3005u));
    Check(!pumps.Observe(10u, 3, 8, 0x4005u));
    pumps.NotifyShot(10u, 3, 7, 0x4005u);
    pumps.Complete(10u, 0x3005u);
    Check(pumps.Observe(10u, 3, 7, 0x4005u));
    pumps.Complete(10u, 0x4005u);
    Check(!pumps.Observe(10u, 3, 7, 0x4005u));
}
