#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_flashlight_pose.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static void Check(bool condition, const char* message)
{ if (!condition) { std::fprintf(stderr,"%s\n",message); std::exit(1); } }
static bool Near(const Vector& a,const Vector& b) { return (a-b).LengthSqr()<0.000001f; }
int main()
{
    using namespace l4d2vr_flashlight;
    const Vector head(10.0f,20.0f,100.0f), forward(1.0f,0.0f,0.0f), right(0.0f,-1.0f,0.0f), up(0.0f,0.0f,1.0f);
    Pose pose{},resolved{};
    Check(Capture(1u,100u,head,forward,right,up,40.0f,0.18f,1.0f,false,false,pose) &&
        Near(pose.origin,Vector(17.2f,20.0f,100.0f)), "An unobstructed mount lies in front of the tracked head");
    Check(Resolve(pose,1u,250u,40.0f,0.18f,resolved), "Current owner can consume a fresh main-thread pose");
    Check(!Resolve(pose,2u,150u,40.0f,0.18f,resolved) && !Resolve(pose,1u,251u,40.0f,0.18f,resolved) &&
        !Resolve(pose,1u,99u,40.0f,0.18f,resolved), "Another player, expired sample or future pose cannot move the light");
    Check(!Resolve(pose,1u,150u,45.0f,0.18f,resolved) && !Resolve(pose,1u,150u,40.0f,0.0f,resolved),
        "Live scale and offset changes invalidate old collision results");
    Check(Capture(1u,100u,head,forward,right,up,40.0f,0.18f,0.5f,false,false,pose) &&
        Near(pose.origin,Vector(13.2f,20.0f,100.0f)), "The mount stops before a nearby wall with a safety margin");
    Check(Capture(1u,100u,head,forward,right,up,40.0f,0.18f,0.02f,false,false,pose) && Near(pose.origin,head),
        "A very close wall must not push the mount through or behind the viewpoint");
    Check(Capture(1u,100u,head,forward,right,up,40.0f,0.18f,1.0f,true,false,pose) && Near(pose.origin,head),
        "Starting inside geometry disables the outward displacement");
    Check(Capture(1u,100u,head,forward,right,up,40.0f,0.0f,1.0f,false,false,pose) && Near(pose.origin,head),
        "Zero offset retains the former viewpoint mount");
    Check(Capture(1u,100u,head,Vector(0.0f,0.0f,-1.0f),right,forward,40.0f,0.18f,1.0f,false,false,pose) &&
        Near(pose.origin,Vector(10.0f,20.0f,92.8f)), "Looking down must move the mount along the head direction");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Check(!Capture(1u,100u,head,forward,right,up,40.0f,0.18f,nan,false,false,pose), "Invalid collision data cannot move a light");
    Check(!Capture(1u,100u,head,forward,forward,up,40.0f,0.18f,1.0f,false,false,pose), "Non-orthogonal tracking is rejected");
    Check(OffsetMeters(nan)==0.0f && OffsetMeters(-1.0f)==0.0f && OffsetMeters(5.0f)==0.30f,
        "Configuration remains finite and bounded");
}
