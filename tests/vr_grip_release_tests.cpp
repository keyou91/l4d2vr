#include "../L4D2VR/vr_grip_release.h"
#include "../L4D2VR/vr_magazine_policy.h"
#include "../L4D2VR/vr_input_recovery.h"
#include <cstdlib>
#include <iostream>

static void Check(bool passed, const char* contract)
{
    if (!passed)
    {
        std::cerr << contract << '\n';
        std::exit(1);
    }
}

int main()
{
    using l4d2vr_grip::ReleaseLatch;
    using l4d2vr_grip::CanReleaseInventoryItem;
    using l4d2vr_grip::MayCommitDrop;
    std::atomic<uint32_t> samples{0u};
    ReleaseLatch missed;
    l4d2vr_input::GripSample::Publish(samples,1u);
    missed.Update(true,true,false,7u,l4d2vr_input::GripSample::Generation(samples.load()));
    l4d2vr_input::GripSample::Publish(samples,3u);
    missed.Update(true,true,true,7u,l4d2vr_input::GripSample::Generation(samples.load()));
    l4d2vr_input::GripSample::Publish(samples,0u); // native command generation misses this sample
    l4d2vr_input::GripSample::Publish(samples,1u);
    const auto discontinuity=l4d2vr_input::GripSample::Generation(samples.load());
    Check(!missed.Update(true,true,false,7u,discontinuity),"Skipped unavailable grip cannot become drop");
    Check(!missed.Update(true,true,true,7u,discontinuity),"Recovered held grip cannot rearm drop");
    Check(!missed.Update(true,true,false,7u,discontinuity),"Healthy neutral grip is only a new baseline");
    Check(!missed.Update(true,true,true,7u,discontinuity),"Fresh grip press arms normally");
    Check(missed.Update(true,true,false,7u,discontinuity),"Fresh grip release still drops normally");
    Check(MayCommitDrop(true, true, true, true, 0), "A current live owned weapon may be released");
    Check(MayCommitDrop(true, true, true, true, 48), "A bounded delayed command may still commit");
    Check(!MayCommitDrop(true, true, true, true, 49), "An expired release must not mutate inventory");
    Check(!MayCommitDrop(true, true, true, true, -1), "A future or reordered release must not commit");
    Check(!MayCommitDrop(true, true, true, true, 4294967295LL), "An overflowing tick difference must not appear current");
    Check(!MayCommitDrop(false, true, true, true, 0), "Dead players must not replay a queued drop");
    Check(!MayCommitDrop(true, false, true, true, 0), "A different player must not consume the release");
    Check(!MayCommitDrop(true, true, false, true, 0), "An expired entity must not be used");
    Check(!MayCommitDrop(true, true, true, false, 0), "A weapon swap must cancel the old release");
    Check(CanReleaseInventoryItem(true, true, true, true, true), "A firearm release must cancel firing or reload interaction");
    Check(!CanReleaseInventoryItem(false, true, true, false, false), "A primed grenade must not execute an inventory drop");
    Check(!CanReleaseInventoryItem(false, false, true, false, false), "Grenade trigger release must not also drop its inventory weapon");
    Check(CanReleaseInventoryItem(false, false, false, false, false), "An idle inventory item can be dropped");
    ReleaseLatch grip;
    Check(!grip.Update(true, true, false, 1), "Idle input must not drop a weapon");
    Check(!grip.Update(true, true, true, 1), "Pressing grip must not drop a weapon");
    Check(!grip.Update(true, true, true, 1), "Holding grip must not repeat a drop");
    Check(grip.Update(true, true, false, 1), "A deliberate release must drop once");
    Check(!grip.Update(true, true, false, 1), "Repeated release samples must be ignored");

    grip.Update(true, true, true, 1);
    Check(!grip.Update(true, false, false, 1), "Losing tracking must not synthesize a release");
    Check(!grip.Update(true, true, false, 1), "Recovering tracking must not replay a release");

    grip.Update(true, true, true, 1);
    Check(!grip.Update(false, true, false, 1), "A menu or incapacitation must cancel the grip");
    Check(!grip.Update(true, true, false, 1), "Returning to gameplay must not drop the weapon");

    grip.Update(true, true, true, 1);
    Check(!grip.Update(true, true, true, 2), "Switching weapons while gripping must not arm the new weapon");
    Check(!grip.Update(true, true, false, 2), "Releasing an inherited grip must be ignored");
    grip.Update(true, true, true, 2);
    Check(grip.Update(true, true, false, 2), "The new weapon must accept a fresh press and release");

    grip.Reset();
    Check(!grip.Update(true, true, true, 3), "An initially held grip must wait for release");
    Check(!grip.Update(true, true, false, 3), "The initial release must not drop the weapon");
    grip.Update(true, true, true, 3);
    Check(!grip.Update(true, true, false, 0), "An absent weapon must cancel a pending drop");
    Check(!grip.Update(true, true, false, 3), "Reappearing weapons must not replay the cancelled drop");

    grip.Reset();
    grip.Update(true, true, false, 4);
    grip.Update(true, true, true, 4);
    // Reproduce extra camera input samples between two actual user commands.
    for (int sample = 0; sample < 12; ++sample) grip.ObserveSession(true, 4);
    Check(grip.IsArmed(), "Camera-only samples must preserve the held grip");
    Check(grip.Update(true, true, false, 4), "Release after extra samples must drop exactly once");
    Check(!grip.Update(true, true, false, 4), "Camera samples must not produce duplicate releases");
    grip.Update(true, true, true, 4);
    grip.ObserveSession(false, 4); // pause, death, or disconnect
    Check(!grip.IsArmed(), "Actual lifecycle loss must cancel the held grip");
    Check(!grip.Update(true, true, false, 4), "Menu-time release must not replay after return");
    grip.Update(true, true, true, 4);
    grip.ObserveSession(true, 5);
    Check(!grip.IsArmed(), "A changed inventory item must cancel old grip ownership");
    Check(!grip.Update(true, true, true, 5), "A weapon switch with grip held must wait for release");
    Check(grip.WaitingForRelease(), "A switched weapon cannot inherit an unrelated held grip");
    Check(!grip.Update(true, true, false, 5), "Inherited release after a switch must be ignored");

    using l4d2vr_magazine::ShouldEject;
    using l4d2vr_magazine::ChamberRoundsAfterEject;
    Check(!ShouldEject(true, false, true, 0, true), "Empty ammunition must not automatically eject a magazine in button mode");
    Check(ShouldEject(true, true, true, 15, true), "The button must permit ejecting a full magazine");
    Check(ShouldEject(true, true, true, 0, true), "The button must eject an empty magazine");
    Check(!ShouldEject(true, true, true, 8, false), "A shotgun must not use detachable-magazine ejection");
    Check(ShouldEject(false, false, true, 0, true), "Legacy empty-magazine handling must remain available");
    Check(ChamberRoundsAfterEject(0) == 0, "An empty pistol must not gain a chambered round");
    Check(ChamberRoundsAfterEject(-1) == 0, "Unknown ammunition must not create a chambered round");
    Check(ChamberRoundsAfterEject(1) == 1, "The last loaded round must remain in the chamber");
    Check(ChamberRoundsAfterEject(15) == 1, "Removing a loaded magazine must retain exactly one chambered round");
    std::cout << "Grip release and magazine contracts passed\n";
}
