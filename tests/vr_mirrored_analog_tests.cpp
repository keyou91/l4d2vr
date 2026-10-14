#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openvr.h"
#include "../L4D2VR/vr_input_recovery.h"
#include "../L4D2VR/vr_hand_input_validation.h"
#include "vr_mirrored_analog_types.inl"

namespace
{
    constexpr vr::VRActionHandle_t kWalkAction = 1;
    constexpr vr::VRInputValueHandle_t kLeftSource = 101;
    constexpr vr::VRInputValueHandle_t kRightSource = 102;
    int checks = 0;

    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(EXIT_FAILURE);
        }
    }

    vr::InputAnalogActionData_t Sample(bool active, float x = 0.0f, float y = 0.0f, float z = 0.0f)
    {
        vr::InputAnalogActionData_t data{};
        data.bActive = active;
        data.x = x;
        data.y = y;
        data.z = z;
        return data;
    }

    void RequireNeutral(const vr::InputAnalogActionData_t& data, const char* message)
    {
        Require(!data.bActive && data.x == 0.0f && data.y == 0.0f && data.z == 0.0f &&
            data.deltaX == 0.0f && data.deltaY == 0.0f && data.deltaZ == 0.0f &&
            data.activeOrigin == vr::k_ulInvalidInputValueHandle, message);
    }
}

struct ScriptedInput
{
    struct Response
    {
        vr::EVRInputError error = vr::VRInputError_None;
        vr::InputAnalogActionData_t data{};
        vr::VRInputValueHandle_t device = kRightSource;
    };
    std::unordered_map<vr::VRActionHandle_t, Response> responses;
    int reads = 0;

    vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t action,
        vr::InputAnalogActionData_t* output, uint32_t outputSize, vr::VRInputValueHandle_t device)
    {
        ++reads;
        Require(outputSize == sizeof(*output), "read uses the OpenVR analog structure size");
        const auto response = responses.find(action);
        if (response == responses.end())
            return vr::VRInputError_NoData;
        Require(device == response->second.device, "read remains restricted to the mirrored controller");
        *output = response->second.data;
        return response->second.error;
    }
};

struct Game
{
    static void logMsg(const char*, ...) {}
};

struct VR
{
    l4d2vr_input::ActionFrame m_InputActionFrame;
    VR() { m_InputActionFrame.Complete(m_InputActionFrame.Begin(), true); }
    ScriptedInput input;
    ScriptedInput* m_Input = &input;
    vr::IVRSystem* m_System = nullptr;
    bool m_LeftHanded = true;
    bool m_LeftHandedSwapInputActions = true;
    bool m_VrHandsDebugLog = false;
    vr::VRActionHandle_t m_ActionWalk = kWalkAction;
    vr::VRActionHandle_t m_ActionTurn = 12u;
    uint64_t m_InputActionStateFrameSerial = 1;
    std::unordered_map<vr::VRActionHandle_t, std::vector<LeftHandedMirroredActionSource>> m_LeftHandedAnalogActionSwapMap;
    std::unordered_set<vr::VRActionHandle_t> m_LeftHandedAnalogHandBoundActions{ kWalkAction };
    std::unordered_map<vr::VRActionHandle_t, LeftHandedAnalogActionFrameCache> m_LeftHandedAnalogActionFrameCache;

    void RefreshLeftHandedInputActionSwapMaps(bool) {}
    bool GetLeftHandedMirroredAnalogActionData(vr::VRActionHandle_t action, vr::InputAnalogActionData_t& output);
    bool GetAnalogActionData(vr::VRActionHandle_t& action, vr::InputAnalogActionData_t& output);
    bool GetWalkAxis(float& x, float& y);

    void Bind(vr::VRActionHandle_t action, vr::InputAnalogActionData_t data,
        vr::EVRInputError error = vr::VRInputError_None, vr::VRInputValueHandle_t device = kRightSource)
    {
        m_LeftHandedAnalogActionSwapMap[kWalkAction].push_back({ action, device });
        input.responses[action] = { error, data, device };
    }
};

#include "vr_mirrored_analog_functions.inl"

static void EmptySourceAndFailedReadTests()
{
    VR empty;
    auto output = Sample(true, 1.0f, 1.0f, 1.0f);
    Require(!empty.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "empty mirrored sources fail");
    RequireNeutral(output, "empty mirrored sources clear output");
    Require(!empty.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "empty cached sources still fail");
    Require(empty.input.reads == 0, "empty mirrored sources do not read original-hand input");

    VR failed;
    failed.Bind(2, Sample(true, 0.9f), vr::VRInputError_InvalidDevice);
    failed.Bind(3, Sample(true, 0.8f), vr::VRInputError_IPCError, kLeftSource);
    Require(!failed.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "all failed queries fail");
    RequireNeutral(output, "failed queries cannot activate stale payloads");
    const int reads = failed.input.reads;
    Require(!failed.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "cached failure does not become success");
    Require(failed.input.reads == reads, "failure is cached within the action frame");
    float x = 9.0f, y = 9.0f;
    Require(!failed.GetWalkAxis(x, y) && x == 0.0f && y == 0.0f, "failed mirrored Walk remains unavailable");
    Require(failed.input.reads == reads, "Walk uses the cached failed status");
}

static void InactiveAndActiveZeroTests()
{
    VR inactive;
    auto stale = Sample(false, 1.0f, -1.0f, 1.0f);
    stale.deltaX = 1.0f;
    stale.activeOrigin = kRightSource;
    inactive.Bind(2, stale);
    vr::InputAnalogActionData_t output{};
    Require(inactive.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "inactive successful read remains successful");
    RequireNeutral(output, "inactive payload is neutralized");
    const int inactiveReads = inactive.input.reads;
    Require(inactive.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "cached inactive success stays successful");
    RequireNeutral(output, "cached inactive output stays neutral");
    Require(inactive.input.reads == inactiveReads, "inactive success is cached");
    float x = 9.0f, y = 9.0f;
    Require(!inactive.GetWalkAxis(x, y) && x == 0.0f && y == 0.0f, "inactive mirrored Walk remains unavailable");

    VR activeZero;
    activeZero.Bind(2, Sample(true));
    Require(activeZero.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "active zero read succeeds");
    Require(output.bActive && output.x == 0.0f && output.y == 0.0f && output.z == 0.0f, "active zero remains active");
    Require(activeZero.GetWalkAxis(x, y) && x == 0.0f && y == 0.0f, "active zero Walk stays available");
}

static void UsableSourceSelectionTests()
{
    VR mixed;
    mixed.Bind(2, Sample(false, 8.0f));
    mixed.Bind(3, Sample(true, std::numeric_limits<float>::infinity()));
    mixed.Bind(4, Sample(true, 0.1f, 0.2f, 0.3f), vr::VRInputError_None, kLeftSource);
    auto strongest = Sample(true, 0.4f, 0.5f, 0.6f);
    strongest.activeOrigin = kRightSource;
    strongest.deltaX = 0.02f;
    mixed.Bind(5, strongest);
    vr::InputAnalogActionData_t output{};
    Require(mixed.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "mixed successful sources succeed");
    Require(output.bActive && output.x == strongest.x && output.y == strongest.y && output.z == strongest.z &&
        output.deltaX == strongest.deltaX && output.activeOrigin == strongest.activeOrigin,
        "largest active finite source preserves its payload");
    float x = 0.0f, y = 0.0f;
    Require(mixed.GetWalkAxis(x, y) && x == strongest.x && y == strongest.y, "Walk consumes the selected active source");

    VR invalid;
    invalid.Bind(2, Sample(true, std::numeric_limits<float>::infinity()));
    invalid.Bind(3, Sample(true, 0.0f, std::numeric_limits<float>::quiet_NaN()));
    invalid.Bind(4, Sample(true, 0.0f, 0.0f, -std::numeric_limits<float>::infinity()));
    Require(invalid.GetLeftHandedMirroredAnalogActionData(kWalkAction, output), "nonfinite payloads retain raw query success");
    RequireNeutral(output, "nonfinite candidates cannot provide an active source");
    Require(!invalid.GetWalkAxis(x, y), "nonfinite-only Walk remains unavailable");

    VR large;
    const float maximum = std::numeric_limits<float>::max();
    large.Bind(2, Sample(true, maximum * 0.5f));
    large.Bind(3, Sample(true, 0.75f));
    Require(large.GetLeftHandedMirroredAnalogActionData(kWalkAction, output) && output.x == 0.75f,
        "out-of-range finite source cannot displace a usable stick");
}

static void ControllerLossAndRecoveryTests()
{
    VR session;
    session.Bind(2, Sample(true, 0.75f, 0.5f));
    float x = 0.0f, y = 0.0f;
    Require(session.GetWalkAxis(x, y) && x == 0.75f && y == 0.5f, "initial active movement is available");

    ++session.m_InputActionStateFrameSerial;
    session.input.responses[2].error = vr::VRInputError_InvalidDevice;
    Require(!session.GetWalkAxis(x, y) && x == 0.0f && y == 0.0f, "controller read failure drops previous movement");
    const int failedReads = session.input.reads;
    session.input.responses[2] = { vr::VRInputError_None, Sample(true), kRightSource };
    Require(!session.GetWalkAxis(x, y), "failed frame does not change status halfway through the frame");
    Require(session.input.reads == failedReads, "failed frame is not queried repeatedly");

    ++session.m_InputActionStateFrameSerial;
    Require(session.GetWalkAxis(x, y) && x == 0.0f && y == 0.0f, "controller recovers on the next frame at rest");

    ++session.m_InputActionStateFrameSerial;
    session.input.responses[2].data = Sample(false, 1.0f, 1.0f);
    Require(!session.GetWalkAxis(x, y) && x == 0.0f && y == 0.0f, "inactive recovery state does not reuse stale axes");

    ++session.m_InputActionStateFrameSerial;
    session.input.responses[2].data = Sample(true, -0.25f, 0.9f);
    Require(session.GetWalkAxis(x, y) && x == -0.25f && y == 0.9f, "movement resumes after reactivation");
}

static void UnboundActionPassthroughTest()
{
    VR direct;
    direct.m_LeftHandedAnalogHandBoundActions.clear();
    direct.input.responses[kWalkAction] = {
        vr::VRInputError_None, Sample(true, 0.25f), vr::k_ulInvalidInputValueHandle };
    vr::InputAnalogActionData_t output{};
    Require(direct.GetLeftHandedMirroredAnalogActionData(kWalkAction, output) && output.x == 0.25f,
        "actions without hand bindings retain the unrestricted read");
}

int main()
{
    EmptySourceAndFailedReadTests();
    InactiveAndActiveZeroTests();
    UsableSourceSelectionTests();
    ControllerLossAndRecoveryTests();
    UnboundActionPassthroughTest();
    std::printf("PASS: mirrored analog input (%d checks)\n", checks);
    return EXIT_SUCCESS;
}
