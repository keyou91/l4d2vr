#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_hands/vr_hand_math.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <cstdio>
#include <cstdlib>
#include "../L4D2VR/vr_ammo_grip.h"
static void CheckImpl(bool x, int line) { if (!x) { std::fprintf(stderr,"Hand anchor regression failed at line %d\n",line); std::exit(1); } }
#define Check(value) CheckImpl((value), __LINE__)
static bool Near(const Vector& a, const Vector& b) { return (a-b).LengthSqr() < 0.00001f; }
static Vector Point(const VrHandMatrix4& matrix, const Vector& point)
{
    return l4d2vr_ammo_grip::Origin(matrix) + l4d2vr_ammo_grip::Rotate(matrix, point);
}
int main()
{
    const Vector p(30.0f,50.0f,80.0f), offset(0.0f,-0.08f,0.1f), rotation(0.0f,180.0f,0.0f);
    const QAngle angle(20.0f,75.0f,40.0f);
    const float units=43.2f;
    const Vector zero(0.0f,0.0f,0.0f);
    const auto rendered=VrHandMath::BuildControllerWorld(p,angle,units,1.2f,offset,rotation);
    const auto anchor=VrHandMath::BuildControllerWorld(p,angle,units,1.0f/units,offset,rotation);
    for (int row=0;row<3;++row)
    {
        Check(std::fabs(VrHandMath::Get(rendered,row,3)-VrHandMath::Get(anchor,row,3))<0.0001f);
        for (int col=0;col<3;++col)
            Check(std::fabs(VrHandMath::Get(rendered,row,col)/(units*1.2f)-VrHandMath::Get(anchor,row,col))<0.0001f);
    }
    // The same local palm point must land at the same world position in the
    // scaled glove mesh and in the unscaled interaction anchor.
    for (int row=0;row<3;++row)
        Check(std::fabs(VrHandMath::Get(rendered,row,3)+VrHandMath::Get(rendered,row,2)*0.07f/1.2f-
            VrHandMath::Get(anchor,row,3)-VrHandMath::Get(anchor,row,2)*0.07f*units)<0.0001f);

    // Acquire ammunition at different wrist angles and body heights. Its
    // model grip point must stay at the palm, with the same local orientation.
    const Vector boxCenter(2.0f, -1.0f, 4.0f), gripPoint(2.0f, -1.0f, 1.0f);
    const Vector palmLocal(0.4f, -0.2f, 3.0f), correction(15.0f, 90.0f, -20.0f);
    l4d2vr_ammo_grip::Pose firstGrip{};
    for (int i=0;i<12;++i)
    {
        const float step=static_cast<float>(i);
        const auto controller=VrHandMath::BuildControllerWorld(Vector(step*6.0f, 20.0f, 90.0f-step*2.0f),
            QAngle(step*7.0f, step*30.0f, step*-11.0f), units, 1.0f/units, zero, zero);
        const Vector palm=Point(controller,palmLocal);
        l4d2vr_ammo_grip::Pose grip{};
        Check(l4d2vr_ammo_grip::Capture(controller,correction,palm,boxCenter,gripPoint,grip));
        if (i==0) firstGrip=grip;
        Check(Near(firstGrip.centerOffsetLocal,grip.centerOffsetLocal));
        for (int col=0;col<3;++col)
            Check(Near(l4d2vr_ammo_grip::Axis(firstGrip.orientationLocal,col),
                l4d2vr_ammo_grip::Axis(grip.orientationLocal,col)));
        VrHandMatrix4 held{};
        Check(l4d2vr_ammo_grip::Follow(controller,grip,boxCenter,held));
        Check(Near(Point(held,gripPoint),palm) && l4d2vr_ammo_grip::Rigid(held));

        // A later render pose uses the same stored grip. Translate and rotate
        // the controller while retaining the physical point inside the palm.
        const auto next=VrHandMath::BuildControllerWorld(Vector(-30.0f+step, 50.0f, 70.0f),
            QAngle(-20.0f, step*23.0f, 37.0f), units, 1.0f/units, zero, zero);
        Check(l4d2vr_ammo_grip::Follow(next,grip,boxCenter,held));
        Check(Near(Point(held,gripPoint),Point(next,palmLocal)));
        // Glove mode grips at the center rather than the bottom of the magazine.
        Check(l4d2vr_ammo_grip::Capture(controller,correction,palm,boxCenter,boxCenter,grip));
        Check(l4d2vr_ammo_grip::Follow(next,grip,boxCenter,held));
        Check(Near(Point(held,boxCenter),Point(next,palmLocal)));
    }
    // Shell-sized and replacement-model bounds use their own local grip point,
    // rather than inheriting the center offset of the previously held magazine.
    const auto controller=VrHandMath::BuildControllerWorld(p,angle,units,1.0f/units,zero,zero);
    l4d2vr_ammo_grip::Pose shell{};
    Check(l4d2vr_ammo_grip::Capture(controller,zero,Point(controller,palmLocal),zero,zero,shell));
    VrHandMatrix4 held{};
    Check(l4d2vr_ammo_grip::Follow(controller,shell,zero,held));
    Check(Near(l4d2vr_ammo_grip::Origin(held),Point(controller,palmLocal)));
    const auto saved=shell;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Check(!l4d2vr_ammo_grip::Capture(controller,Vector(nan,0.0f,0.0f),p,boxCenter,gripPoint,shell));
    Check(Near(shell.centerOffsetLocal,saved.centerOffsetLocal));
    auto invalid=controller;
    VrHandMath::Set(invalid,0,0,VrHandMath::Get(invalid,0,0)*2.0f);
    Check(!l4d2vr_ammo_grip::Capture(invalid,correction,p,boxCenter,gripPoint,shell));
    invalid=controller;
    for (int row=0;row<3;++row) VrHandMath::Set(invalid,row,0,-VrHandMath::Get(invalid,row,0));
    Check(!l4d2vr_ammo_grip::Follow(invalid,saved,boxCenter,held)); // do not mirror the ammo mesh
    Check(!l4d2vr_ammo_grip::Follow(controller,saved,Vector(0.0f,nan,0.0f),held));
    const auto defaultHand=VrHandMath::BuildControllerWorld(p,angle,units);
    const auto explicitHand=VrHandMath::BuildControllerWorld(p,angle,units,1.0f,zero,zero);
    for (int i=0;i<16;++i) Check(defaultHand.m[static_cast<size_t>(i)]==explicitHand.m[static_cast<size_t>(i)]);
    // Waist previews and waiting ammo use the same body frame, with outward
    // winding preserved. Gun orientation is not an input to this transform.
    for (int i=0;i<8;++i)
    {
        Vector forward, right, up;
        QAngle::AngleVectors(QAngle(0.0f,static_cast<float>(i)*45.0f,0.0f),&forward,&right,&up);
        const Vector waist=p+forward*4.0f+right*-9.0f-Vector(0.0f,0.0f,25.0f);
        Check(l4d2vr_ammo_grip::BodyWorld(forward,right,waist,boxCenter,held));
        Check(Near(Point(held,boxCenter),waist) && l4d2vr_ammo_grip::Rigid(held));
        Check(Near(l4d2vr_ammo_grip::Axis(held,0),forward));
        Check(Near(l4d2vr_ammo_grip::Axis(held,1),right*-1.0f));
        VrHandMatrix4 translated{};
        const Vector motion(10.0f,-30.0f,5.0f);
        Check(l4d2vr_ammo_grip::BodyWorld(forward,right,waist+motion,boxCenter,translated));
        Check(Near(l4d2vr_ammo_grip::Origin(translated)-l4d2vr_ammo_grip::Origin(held),motion));
    }
    Check(!l4d2vr_ammo_grip::BodyWorld(Vector(0.0f,0.0f,1.0f),Vector(0.0f,-1.0f,0.0f),p,boxCenter,held));
}
