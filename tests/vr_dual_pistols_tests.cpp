#include "../L4D2VR/vr_dual_pistols.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr,"Failed at line %d\n",__LINE__);std::abort(); } } while(false)
struct Matrix { float m[3][4]{}; };
static Matrix Pose(float x, float y) { Matrix p{}; p.m[0][0]=p.m[1][1]=p.m[2][2]=1; p.m[0][3]=x; p.m[1][3]=y; return p; }

int main()
{
    using namespace l4d2vr_dual;
    ReloadPulse reload;
    CHECK(!reload.Update(true,false,100u));
    CHECK(reload.Update(true,true,200u));
    CHECK(reload.Update(true,false,549u));
    CHECK(!reload.Update(true,false,550u));
    CHECK(reload.Update(true,true,0xfffffff0u));
    CHECK(reload.Update(true,false,20u));
    CHECK(!reload.Update(false,false,30u));
    CHECK(IsShotMarker(kLeftShotMarker,true,true));
    CHECK(IsShotMarker(kRightShotMarker,true,true));
    CHECK(!IsShotMarker(kLeftShotMarker,false,true));
    CHECK(!IsShotMarker(kLeftShotMarker,true,false));
    CHECK(!IsShotMarker(2,true,true)); // ordinary pistol inventory-drop marker
    // Preserve the input hint before weapon selection is simulated. Its use
    // still depends on validating the actual firing weapon as a pistol.
    CHECK(DecodeShotHand(kLeftShotMarker, true) == Hand::Left);
    CHECK(DecodeShotHand(kRightShotMarker, true) == Hand::Right);
    CHECK(DecodeShotHand(kLeftShotMarker, false) == Hand::None);
    CHECK(DecodeShotHand(61u, true) == Hand::None);
    TriggerRouter router;
    CHECK(router.Update(true,false,true,30)==Hand::Left);
    CHECK(router.Update(true,false,true,29)==Hand::Left);
    CHECK(router.Update(true,true,true,29)==Hand::None);
    CHECK(router.Update(true,true,true,29)==Hand::Right);
    CHECK(router.Update(true,true,true,28)==Hand::None);
    CHECK(router.Update(true,true,true,28)==Hand::Left);
    CHECK(router.Update(false,true,true,28)==Hand::None);
    CHECK(router.Update(true,false,false,28)==Hand::None);
    CHECK(router.Update(true,true,false,28)==Hand::Right);

    CommandShots cache;
    Shot left{101,Hand::Left,{-5,2,3},{0,90,0}};
    Shot right{102,Hand::Right,{5,2,3},{0,-90,0}};
    cache.Store(left); cache.Store(right);
    Shot copy{};
    CHECK(cache.Get(101,copy) && copy.hand==Hand::Left && copy.angles[1]==90);
    CHECK(cache.Get(102,copy) && copy.hand==Hand::Right && copy.angles[1]==-90);
    CHECK(cache.Get(101,copy) && copy.position[0]==-5); // backup retransmission
    CHECK(!cache.Get(103,copy));

    Shot feedback{104,Hand::Left,{-5,2,3},{0,90,0},100u,10u,1000u};
    CHECK(LiveShotMatches(feedback,100u,10u,1000u));
    CHECK(LiveShotMatches(feedback,100u,10u,1350u));
    CHECK(!LiveShotMatches(feedback,100u,10u,1351u));
    CHECK(!LiveShotMatches(feedback,100u,10u,999u));
    CHECK(!LiveShotMatches(feedback,200u,10u,1001u)); // player replacement cannot reuse the old ray
    CHECK(!LiveShotMatches(feedback,100u,20u,1001u)); // switching away cannot aim the new weapon from this pistol
    CHECK(!LiveShotMatches(feedback,0u,10u,1001u));
    feedback.weaponHandle=0x3005u;
    cache.Store(feedback);
    cache.Store({105,Hand::Right,{5,2,3},{0,-90,0},100u,10u,1001u,0x3005u});
    CHECK(cache.GetForExecution(104,100u,10u,0x3005u,1002u,copy) && copy.hand==Hand::Left);
    CHECK(cache.GetForExecution(105,100u,10u,0x3005u,1002u,copy) && copy.hand==Hand::Right);
    CHECK(cache.GetForExecution(104,100u,10u,0x3005u,1003u,copy) && copy.position[0]==-5); // older command replay
    CHECK(!cache.GetForExecution(104,200u,10u,0x3005u,1002u,copy));
    CHECK(!cache.GetForExecution(104,100u,20u,0x3005u,1002u,copy));
    CHECK(!cache.GetForExecution(104,100u,10u,0x4005u,1002u,copy)); // same address, new native handle serial
    CHECK(!cache.GetForExecution(104,100u,10u,0u,1002u,copy));
    CHECK(!cache.GetForExecution(104,100u,10u,0x3005u,1351u,copy));
    CHECK(!cache.GetForExecution(106,100u,10u,0x3005u,1002u,copy));
    feedback.hand=Hand::None;
    CHECK(!LiveShotMatches(feedback,100u,10u,1001u));
    feedback.hand=Hand::Right;
    feedback.position[0]=std::numeric_limits<float>::quiet_NaN();
    CHECK(!LiveShotMatches(feedback,100u,10u,1001u));
    cache.Store({251,Hand::None,{},{}});
    CHECK(!cache.Get(101,copy));
    cache.Store({103,Hand::Left,{std::numeric_limits<float>::infinity(),0,0},{}});
    CHECK(!cache.Get(103,copy));
    cache.Store({254,Hand::Right,{5,2,3},{0,-90,0},100u,10u,1100u});
    CHECK(!cache.GetForExecution(104,100u,10u,0x3005u,1101u,copy)); // ring eviction cannot reuse the new command

    Matrix rightHand=Pose(10,0), leftHand=Pose(-10,0), rightGun=Pose(11,0), oldLeft=Pose(12,5), delta{};
    const Matrix frame=ControllerFrame<Matrix>({0,0,0},{1,0,0},{0,-1,0},{0,0,1});
    CHECK(frame.m[1][0]==-1 && frame.m[2][1]==1 && frame.m[0][2]==-1);
    CHECK(Retarget(rightHand,leftHand,rightGun,oldLeft,delta));
    Matrix target=Multiply(delta,oldLeft);
    CHECK(std::fabs(target.m[0][3]+9)<0.0001f && std::fabs(target.m[1][3])<0.0001f);
    leftHand.m[0][0]=0; leftHand.m[0][1]=-1; leftHand.m[1][0]=1; leftHand.m[1][1]=0;
    CHECK(Retarget(rightHand,leftHand,rightGun,oldLeft,delta));
    target=Multiply(delta,oldLeft);
    CHECK(std::fabs(target.m[0][3]+10)<0.0001f && std::fabs(target.m[1][3]-1)<0.0001f);
    Matrix singular{};
    CHECK(!Retarget(singular,leftHand,rightGun,oldLeft,delta));

    CHECK(PrimaryHand(true, false, 2u) == Hand::Left);
    CHECK(PrimaryHand(true, true, 2u) == Hand::Right);
    CHECK(PrimaryHand(false, false, 2u) == Hand::Right);
    CHECK(PrimaryHand(true, false, 3u) == Hand::Right);
    CHECK(ContactHandMatches(Hand::Left, Hand::Left));
    CHECK(!ContactHandMatches(Hand::Right, Hand::Left));
    CHECK(!ContactHandMatches(Hand::Left, Hand::Right));
    CHECK(!ContactHandMatches(Hand::None, Hand::None));
    CHECK(ReloadSupportIsLeft(Hand::Right));
    CHECK(!ReloadSupportIsLeft(Hand::Left));
    CHECK(ReloadGripPhysicalMask(false, Hand::Left, false) == 0u);
    CHECK(ReloadGripPhysicalMask(true, Hand::Right, false) == 1u);
    CHECK(ReloadGripPhysicalMask(true, Hand::Right, true) == 2u);
    CHECK(ReloadGripPhysicalMask(true, Hand::Left, false) == 2u);
    CHECK(ReloadGripPhysicalMask(true, Hand::Left, true) == 1u);
    CHECK(ControllerFrameAvailable(true, true, 1000u, 1350u));
    CHECK(!ControllerFrameAvailable(true, true, 1000u, 1351u));
    CHECK(!ControllerFrameAvailable(true, true, 1000u, 999u));
    CHECK(!ControllerFrameAvailable(false, true, 1000u, 1001u));
    CHECK(!ControllerFrameAvailable(true, false, 1000u, 1001u));
    CHECK(!ControllerFrameAvailable(true, true, 0u, 1001u));
    CHECK(RetainedWeaponPart("valvebiped.clip"));
    CHECK(RetainedWeaponPart("valvebiped.bullet_l"));
    CHECK(RetainedWeaponPart("slide"));
    CHECK(!RetainedWeaponPart("valvebiped.bip01_r_hand"));
    CHECK(!RetainedWeaponPart("valvebiped.bip01_l_forearm"));

    // A moved primary gun's contacts must use its moved anchor. Free-hand
    // movement must cancel out of both the visible gun and input rebasing.
    Matrix r0 = Pose(10, 0), l0 = Pose(-10, 0), calibration = Pose(1, 2);
    Matrix native0 = Multiply(r0, calibration), moved0{};
    CHECK(Retarget(r0, l0, native0, native0, delta));
    moved0 = Multiply(delta, native0);
    Matrix magazineLocal = Pose(2, -3), contact0 = Multiply(moved0, magazineLocal);
    Matrix r1 = Pose(35, 17);
    r1.m[0][0] = 0; r1.m[0][1] = -1; r1.m[1][0] = 1; r1.m[1][1] = 0;
    Matrix native1 = Multiply(r1, calibration);
    CHECK(Retarget(r1, l0, native1, native1, delta));
    Matrix moved1 = Multiply(delta, native1);
    CHECK(Retarget(moved0, moved1, contact0, contact0, delta));
    Matrix inputContact = Multiply(delta, contact0);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            CHECK(std::fabs(inputContact.m[row][col] - contact0.m[row][col]) < 0.0001f);
    Matrix l1 = Pose(-20, 6);
    l1.m[0][0] = 0; l1.m[0][1] = -1; l1.m[1][0] = 1; l1.m[1][1] = 0;
    CHECK(Retarget(r1, l1, native1, native1, delta));
    Matrix moved2 = Multiply(delta, native1);
    CHECK(Retarget(moved0, moved2, contact0, contact0, delta));
    inputContact = Multiply(delta, contact0);
    Matrix expectedContact = Multiply(moved2, magazineLocal);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            CHECK(std::fabs(inputContact.m[row][col] - expectedContact.m[row][col]) < 0.0001f);
}
