#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_skeletal_data.h"
#include "../L4D2VR/vr_hand_pose_cache.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Skeletal data failed at %d\n", __LINE__); std::exit(1); } } while (false)

int main()
{
    using namespace l4d2vr_skeletal;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    CHECK(ValidCurl(0.0f) && ValidCurl(1.0f) && ValidCurl(0.5f));
    CHECK(!ValidCurl(nan) && !ValidCurl(infinity) && !ValidCurl(-infinity));
    CHECK(!ValidCurl(-0.01f) && !ValidCurl(1.01f));
    vr::VRSkeletalSummaryData_t summary{};
    CHECK(ValidSummaryCurls(summary));
    for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
    {
        summary = {};
        summary.flFingerCurl[finger] = infinity;
        CHECK(!ValidSummaryCurls(summary));
        summary.flFingerCurl[finger] = nan;
        CHECK(!ValidSummaryCurls(summary));
    }
    vr::VRBoneTransform_t bone{};
    CHECK(!ValidBone(bone));
    bone.orientation.w = 1.0f;
    CHECK(ValidBone(bone));
    bone.orientation.w = 2.0f;
    CHECK(ValidBone(bone));
    bone.position.v[1] = nan;
    CHECK(!ValidBone(bone));
    bone.position.v[1] = 0.0f;
    bone.orientation.x = infinity;
    CHECK(!ValidBone(bone));
    bone.orientation.x = 0.0f;
    bone.orientation.w = 0.00000001f;
    CHECK(!ValidBone(bone));
    bone.orientation.w = (std::numeric_limits<float>::max)();
    CHECK(!ValidBone(bone));
    VrHandMatrix4 matrix{};
    CHECK(ValidMatrix(matrix));
    matrix.m[7] = nan;
    CHECK(!ValidMatrix(matrix));
    std::vector<VrHandMatrixRows3x4> palette(2);
    CHECK(ValidPalette(palette));
    palette[1].v[11] = infinity;
    CHECK(!ValidPalette(palette));
    palette.clear();
    CHECK(!ValidPalette(palette));
    l4d2vr_input::ActionFrame frame;
    auto attempt = frame.Begin(); frame.Complete(attempt, true);
    const auto sampled = frame.Read();
    attempt = frame.Begin(); frame.Complete(attempt, true);
    CHECK(frame.Read() != sampled && l4d2vr_hand_pose::Available(frame.Read(), sampled, 100u, 150u));
    CHECK(!l4d2vr_hand_pose::Available(frame.Read(), sampled, 100u, 251u));
    attempt = frame.Begin(); frame.Complete(attempt, false);
    CHECK(!l4d2vr_hand_pose::Available(frame.Read(), sampled, 100u, 150u));
    attempt = frame.Begin(); frame.Complete(attempt, true);
    CHECK(!l4d2vr_hand_pose::Available(frame.Read(), sampled, 100u, 150u));
    CHECK(l4d2vr_hand_pose::Available(frame.Read(), frame.Read(), 0xfffffff0u, 20u));
    std::puts("Skeletal curls, bone quaternions and palette validation passed");
}
