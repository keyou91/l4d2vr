#include "../L4D2VR/vr_pistol_effects.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Check(bool value, const char* message)
{ if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
static l4d2vr_dual::Matrix3x4 Pose(float x, float y)
{
    l4d2vr_dual::Matrix3x4 m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    m.m[0][3] = x; m.m[1][3] = y;
    return m;
}
int main()
{
    using namespace l4d2vr_pistol_fx;
    using l4d2vr_dual::Hand;
    Check(FireActivity(true, Hand::Left, 191, 14) == 192,
        "A left shot must select the authored left animation even when shared clip parity selects right");
    Check(FireActivity(true, Hand::Right, 192, 14) == 191,
        "A right shot must select the authored right animation");
    const int leftAfter = RemainingAfterShot(true,Hand::Left,15,1);
    Check(leftAfter == 0 && FireActivity(true,Hand::Left,192,leftAfter) == 481,
        "The last left round locks its own slide even when the right pistol is full");
    Check(FireActivity(true,Hand::Right,191,RemainingAfterShot(true,Hand::Right,1,15)) == 194,
        "The last right round locks only the right slide");
    Check(FireActivity(true,Hand::Left,194,RemainingAfterShot(false,Hand::Left,15,1)) == 192,
        "Infinite ammo must not use a last-round animation");
    Check(FireActivity(true,Hand::Left,194,-1) == 481 && FireActivity(true,Hand::Right,481,-1) == 194,
        "Unknown per-hand ammo retains native last-round information while choosing the command's hand");
    Check(FireActivity(false,Hand::Left,191,0) == 191 && FireActivity(true,Hand::None,192,0) == 192 &&
        FireActivity(true,Hand::Left,180,0) == 180, "Unrelated, unmarked and single-pistol activity must retain its native behavior");
    Check(RemainingAfterShot(true,Hand::None,15,15) == -1 &&
        RemainingAfterShot(true,Hand::Left,15,-1) == -1 && RemainingAfterShot(true,Hand::Left,15,16) == -1,
        "Unavailable or invalid ammo counts cannot invent a last-round state");
    auto sourceController = Pose(10.0f,0.0f), targetController = Pose(-10.0f,0.0f);
    auto sourceGun = Pose(12.0f,1.0f), oldOtherGun = Pose(13.0f,-1.0f), sourceWrist = Pose(11.0f,0.0f);
    l4d2vr_dual::Matrix3x4 gunDelta{};
    Check(l4d2vr_dual::Retarget(sourceController,targetController,sourceGun,oldOtherGun,gunDelta), "Build calibrated left gun transfer");
    const auto rotationBefore = gunDelta;
    Check(AlignGripPosition(sourceController,targetController,sourceWrist,{-8.0f,2.0f,0.0f},40.0f,gunDelta),
        "The gun grip can follow the displayed glove wrist");
    const auto placed = l4d2vr_dual::Multiply(gunDelta,oldOtherGun);
    Check(std::fabs(placed.m[0][3]+7.0f)<0.0001f && std::fabs(placed.m[1][3]-3.0f)<0.0001f,
        "The authored gun-to-grip offset survives wrist alignment");
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            Check(gunDelta.m[row][col] == rotationBefore.m[row][col], "Glove rotations must not reverse the gun aim");
    const auto validDelta = gunDelta;
    Check(!AlignGripPosition(sourceController,targetController,sourceWrist,{100000.0f,0.0f,0.0f},40.0f,gunDelta) &&
        gunDelta.m[0][3] == validDelta.m[0][3], "An invalid distant glove cannot move or partially alter the gun");
    auto movedSource = Pose(40.0f,20.0f);
    movedSource.m[0][0]=0.0f; movedSource.m[0][1]=-1.0f;
    movedSource.m[1][0]=1.0f; movedSource.m[1][1]=0.0f;
    const auto movedGun = l4d2vr_dual::Multiply(movedSource,Pose(2.0f,1.0f));
    const auto movedWrist = l4d2vr_dual::Multiply(movedSource,Pose(1.0f,0.0f));
    Check(l4d2vr_dual::Retarget(movedSource,targetController,movedGun,oldOtherGun,gunDelta) &&
        AlignGripPosition(movedSource,targetController,movedWrist,{-8.0f,2.0f,0.0f},40.0f,gunDelta),
        "Moving the source gun and hand together preserves independent target alignment");
    const auto stationaryLeft = l4d2vr_dual::Multiply(gunDelta,oldOtherGun);
    Check(std::fabs(stationaryLeft.m[0][3]-placed.m[0][3])<0.0001f &&
        std::fabs(stationaryLeft.m[1][3]-placed.m[1][3])<0.0001f,
        "The left pistol cannot drift when only the right hand translates or rotates");
    Muzzles cache;
    const Key dual{100u, 200u, 0x1001u, 3u};
    cache.Observe(dual);
    const auto right = Pose(10.0f, 0.0f), left = Pose(-10.0f, 0.0f);
    Check(cache.Capture(dual, Hand::Right, right, Pose(14.0f, 1.0f), 40.0f, 100u), "Capture right muzzle");
    Check(cache.Capture(dual, Hand::Left, left, Pose(-6.0f, -1.0f), 40.0f, 100u), "Capture left muzzle independently");
    auto executing = Pose(-20.0f, 5.0f), result = Pose(0.0f, 0.0f);
    executing.m[0][0] = 0.0f; executing.m[0][1] = -1.0f;
    executing.m[1][0] = 1.0f; executing.m[1][1] = 0.0f;
    Check(cache.Resolve(dual, Hand::Left, executing, 150u, result), "Resolve the left muzzle in the executing command frame");
    Check(std::fabs(result.m[0][3] + 19.0f) < 0.0001f && std::fabs(result.m[1][3] - 9.0f) < 0.0001f,
        "Room-scale translation and rotation must preserve the left gun's own muzzle offset");
    Check(cache.Resolve(dual, Hand::Right, executing, 150u, result) && std::fabs(result.m[0][3] + 21.0f) < 0.0001f,
        "A right shot must not borrow the left attachment");
    Check(!cache.Resolve({101u,200u,0x1001u,3u}, Hand::Left, executing, 150u, result), "Reject another player");
    Check(!cache.Resolve({100u,201u,0x1001u,3u}, Hand::Left, executing, 150u, result), "Reject another weapon");
    Check(!cache.Resolve({100u,200u,0x2001u,3u}, Hand::Left, executing, 150u, result), "Reject a reused entity address with a new serial");
    Check(!cache.Resolve(dual, Hand::Left, executing, 251u, result), "Reject a stale attachment");
    Check(!cache.Resolve(dual, Hand::Left, executing, 99u, result), "Reject a future sample");
    Check(!cache.Resolve(dual, Hand::None, executing, 150u, result), "Never choose a hand without a shot marker");
    Check(!cache.Capture(dual, Hand::Left, left, Pose(-10.0f,100000.0f),40.0f,150u), "Hidden gun branches cannot publish a muzzle");
    auto bad = left; bad.m[0][0] = std::numeric_limits<float>::quiet_NaN();
    Check(!cache.Capture(dual, Hand::Left, bad, left,40.0f,150u), "Reject non-finite tracking");
    const Key retained{100u,200u,0x1001u,2u};
    cache.Observe(retained);
    Check(!cache.Resolve(retained, Hand::Left, executing, 150u, result), "Changing handed inventory invalidates the old model sample");
    Check(!cache.Capture(dual, Hand::Left, left,left,40.0f,150u), "Reject a queued draw from the previous inventory mode");
    Check(cache.Capture(retained, Hand::Left, left,Pose(-6.0f,-1.0f),40.0f,150u), "Retained left pistol can publish its transformed primary attachment");
    Check(!cache.Resolve(retained, Hand::Right, right,150u,result), "A dropped pistol cannot emit effects");
    cache.Observe({}); cache.Observe(retained);
    Check(!cache.Resolve(retained,Hand::Left,left,150u,result), "Gameplay loss clears previous attachments");
}
