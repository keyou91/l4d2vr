#define NOMINMAX
#include <Windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <filesystem>
#include <mutex>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include "openvr.h"
#include "../L4D2VR/vr_input_recovery.h"

static unsigned checks = 0u;
static void Require(bool value, const char* message)
{
    ++checks;
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(EXIT_FAILURE); }
}
struct Offset { uintptr_t address = 0u; };
struct Offsets { Offset g_pClientMode; };
struct Game
{
    bool m_Initialized = true;
    Offsets offsets;
    Offsets* m_Offsets = &offsets;
    static inline std::vector<std::string> logs;
    static void errorMsg(const char* message) { logs.emplace_back(message); }
    static void logMsg(const char* format, ...)
    {
        char buffer[1024]{}; va_list args;
        va_start(args,format); vsnprintf_s(buffer,sizeof(buffer),_TRUNCATE,format,args); va_end(args);
        logs.emplace_back(buffer);
    }
};
static constexpr int MH_OK = 0;
static unsigned creates = 0u, enables = 0u, disables = 0u;
static int MH_CreateHook(LPVOID target, LPVOID, LPVOID* original)
{ ++creates; *original=target; return 0; }
static int MH_EnableHook(LPVOID target) { ++enables; Require(target!=nullptr,"native enable never receives ALL_HOOKS"); return 0; }
static int MH_DisableHook(LPVOID target) { ++disables; Require(target!=nullptr,"native disable never receives ALL_HOOKS"); return 0; }
#include "vr_input_startup_hook.inl"
struct Hooks
{
    static inline Game* m_Game = nullptr;
    static inline Hook<void*> hkCreateMove{};
    static inline void* dCreateMove = reinterpret_cast<void*>(1u);
    static void RetryCreateMoveHook();
    static bool InstallFragment();
};
#include "vr_input_startup_discovery.inl"
bool Hooks::InstallFragment()
{
#include "vr_input_startup_install.inl"
    return true; // Remaining independent hook initialization is reachable.
}

static std::string BuildVrHandsActionManifestPath(const char* path) { return path; }
struct Input
{
    bool failManifest = false, zeroWalk = false;
    vr::EVRInputError updateError = vr::VRInputError_None;
    std::string failedAction, failedSet;
    std::string manifestPath;
    unsigned registrations = 0u, acquisitions = 0u, updates = 0u;
    std::unordered_map<std::string,unsigned> reads;
    vr::EVRInputError SetActionManifestPath(const char* path)
    { ++registrations; manifestPath=path; return failManifest ? vr::VRInputError_InvalidParam : vr::VRInputError_None; }
    vr::EVRInputError GetActionHandle(const char* name, vr::VRActionHandle_t* output)
    {
        ++acquisitions; ++reads[name]; *output=0u;
        if (failedAction==name) return vr::VRInputError_InvalidHandle;
        if (zeroWalk && std::strcmp(name,"/actions/main/in/Walk")==0) return vr::VRInputError_None;
        if (std::strcmp(name,"/actions/main/in/boolean_turnleft")==0 ||
            std::strcmp(name,"/actions/main/in/InventoryGripLeft")==0) return vr::VRInputError_NameNotFound;
        *output=100u+acquisitions; return vr::VRInputError_None;
    }
    vr::EVRInputError GetActionSetHandle(const char* name, vr::VRActionSetHandle_t* output)
    {
        ++acquisitions; ++reads[name]; *output=0u;
        if (failedSet==name) return vr::VRInputError_InvalidHandle;
        *output=100u+acquisitions; return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t* sets, uint32_t size, uint32_t count)
    {
        ++updates;
        Require(size==sizeof(vr::VRActiveActionSet_t) && count==2u && sets[0].ulActionSet && sets[1].ulActionSet,
            "only acquired action sets reach the real update ABI");
        return updateError;
    }
};
struct VR
{
    static constexpr int MAX_STR_LEN=512;
    Input input;
    Input* m_Input=&input;
#include "vr_input_startup_handles.inl"
    std::array<vr::VRActiveActionSet_t,2> m_ActiveActionSets{};
    bool m_InputManifestRegistered=false, m_InputActionHandlesReady=false;
    std::string m_InputActionManifestPath, m_InputAcquisitionFailure;
    std::chrono::steady_clock::time_point m_InputAcquisitionNextRetry{};
    l4d2vr_input::ActionFrame m_InputActionFrame;
    uint64_t m_InputActionStateFrameSerial=0u;
    vr::EVRInputError m_LastInputActionStateError=vr::VRInputError_None;
    bool m_IsVREnabled=true, m_EncodeVRUsercmd=true, m_ForceNonVRServerMovement=false;
    void RefreshLeftHandedInputActionSwapMaps(bool) {}
    int SetActionManifest(const char*);
    void UpdateInputActionState(const l4d2vr_input::ActionFrame::Attempt&);
    void Frame(bool retryNow=false)
    {
        if(retryNow) m_InputAcquisitionNextRetry={};
        UpdateInputActionState(m_InputActionFrame.Begin());
    }
};
#include "vr_input_startup_manifest.inl"
#include "vr_input_startup_update.inl"

struct Command
{
    int command_number=100, tick_count=200, buttons=0, mousedx=0, mousedy=0;
    struct Angles { float x=0,y=0,z=0; } viewangles;
    float forwardmove=123, sidemove=45, upmove=0;
};
struct WireFixture
{
    VR vr;
    VR* m_VR=&vr;
    bool s_ServerUnderstandsVR=false;
    struct Writer
    {
        Command emitted{};
        int fOriginal(void*,Command* to,Command*) { std::memcpy(&emitted,to,sizeof(emitted)); return 123; }
    } hkWriteUsercmd;
    int Write(Command* to)
    {
        void* buf=nullptr; Command* from=nullptr;
#include "vr_input_startup_wire_gate.inl"
        const Command::Angles controllerAngles{}, controllerPos{20,40,70};
        const bool meleePose=to->buttons<0, pistolPose=to->buttons<0;
        struct { bool swinging=false; } meleeCommand;
#include "vr_input_startup_wire_fields.inl"
        to->tick_count*=-1;
        return hkWriteUsercmd.fOriginal(buf,to,from);
    }
};

static void AcquisitionTests()
{
    const auto originalDirectory=std::filesystem::current_path();
    const auto unicodeDirectory=originalDirectory / L"input_\u00e4_\u624b\u67c4";
    std::filesystem::create_directory(unicodeDirectory);
    std::filesystem::current_path(unicodeDirectory);
    VR unicode; unicode.input.failManifest=true;
    Require(unicode.SetActionManifest("action_manifest.json")!=0,"Unicode acquisition failure is retryable");
    const auto expectedManifest=(unicodeDirectory / L"VR" / L"SteamVRActionManifest" / L"action_manifest.json").u8string();
    std::filesystem::current_path(originalDirectory);
    unicode.input.failManifest=false; unicode.Frame(true);
    Require(unicode.m_InputActionHandlesReady && unicode.input.manifestPath==expectedManifest &&
        std::filesystem::u8path(unicode.input.manifestPath).parent_path().parent_path().parent_path()==unicodeDirectory,
        "actual acquisition retains exact UTF8 Unicode manifest path across retry and working-directory change");
    VR manifest; manifest.input.failManifest=true;
    Require(manifest.SetActionManifest("action_manifest.json")!=0 && !manifest.m_InputActionHandlesReady,
        "manifest registration failure is truthful and prevents action readiness");
    manifest.Frame();
    Require(manifest.input.updates==0u && manifest.input.registrations==1u,
        "startup retry is throttled and unavailable sets never update");
    manifest.input.failManifest=false; manifest.Frame(true);
    Require(manifest.m_InputActionHandlesReady && l4d2vr_input::ActionFrame::Ready(manifest.m_InputActionFrame.Read()),
        "late registration/acquisition recovers through actual update function");
    Require(manifest.m_ActionBooleanTurnLeft==0u && manifest.m_ActionInventoryGripLeft==0u,
        "absent optional legacy actions remain nonfatal");
    const auto acquired=manifest.input.acquisitions;
    const auto walk=manifest.m_ActionWalk;
    manifest.input.updateError=vr::VRInputError_IPCError; manifest.Frame(true);
    Require(!l4d2vr_input::ActionFrame::Ready(manifest.m_InputActionFrame.Read()) &&
        manifest.input.acquisitions==acquired && manifest.m_ActionWalk==walk,
        "healthy handles stay immutable through later frame errors");
    manifest.input.updateError=vr::VRInputError_None; manifest.Frame();
    Require(l4d2vr_input::ActionFrame::Ready(manifest.m_InputActionFrame.Read()),"ordinary action-frame error recovery remains intact");
    VR handles; handles.input.failedAction="/actions/main/in/Walk"; handles.input.failedSet="/actions/base";
    Require(handles.SetActionManifest("action_manifest.json")!=0 && !handles.m_InputActionHandlesReady,
        "core action/set acquisition failure is truthful");
    const auto primary=handles.m_ActionPrimaryAttack;
    handles.input.failedAction.clear(); handles.input.failedSet.clear(); handles.Frame(true);
    Require(handles.m_InputActionHandlesReady && handles.m_ActionWalk && handles.m_BaseActionSet &&
        handles.m_ActionPrimaryAttack==primary && handles.input.reads["/actions/main/in/PrimaryAttack"]==1u,
        "retry acquires failed controls/sets while preserving already acquired handles");
    VR zero; zero.input.zeroWalk=true;
    Require(zero.SetActionManifest("action_manifest.json")!=0,"successful API with zero mandatory handle remains unavailable");
    zero.input.zeroWalk=false; zero.Frame(true);
    Require(zero.m_InputActionHandlesReady,"zero-handle startup recovers on bounded retry");
    VR absent; absent.m_Input=nullptr;
    Require(absent.SetActionManifest("action_manifest.json")!=0,"missing input interface is diagnosed without dereference");
    absent.m_Input=&absent.input; absent.Frame(true);
    Require(absent.m_InputActionHandlesReady,"late input interface can establish startup controls");
    bool sawFailure=false,sawReady=false;
    for(const auto& log:Game::logs) {sawFailure|=log.find("stage=/actions/main/in/Walk")!=std::string::npos;
        sawReady|=log.find("acquisition ready manifest=")!=std::string::npos;}
    Require(sawFailure && sawReady,"startup diagnostics name failed core action and successful manifest");
}
static void HookTests()
{
    Hook<void*> absent{};
    const auto beforeEnable=enables,beforeDisable=disables;
    Require(absent.enableHook()!=0 && absent.disableHook()!=0 && !absent.isEnabled &&
        enables==beforeEnable && disables==beforeDisable,"null targets cannot implicitly activate/deactivate every hook");
    Game game; uintptr_t outer=0u,inner=0u;
    void* vtable[28]{}; vtable[27]=reinterpret_cast<void*>(2u); void** mode=vtable;
    game.offsets.g_pClientMode.address=reinterpret_cast<uintptr_t>(&outer);
    Hooks::m_Game=&game; Hooks::hkCreateMove={};
    Require(Hooks::InstallFragment() && g_CreateMoveDiscoveryPending && !Hooks::hkCreateMove.pTarget,
        "delayed CreateMove discovery leaves trailing hook initialization reachable");
    Require(g_CreateMoveDiscoveryDeadline.time_since_epoch().count()==0,
        "graphics/runtime startup before first initialized callback does not consume discovery budget");
    game.m_Initialized=false; Hooks::RetryCreateMoveHook();
    Require(g_CreateMoveDiscoveryDeadline.time_since_epoch().count()==0,
        "uninitialized updates also leave discovery budget unstarted");
    inner=reinterpret_cast<uintptr_t>(&mode); outer=reinterpret_cast<uintptr_t>(&inner);
    // Model a first callback arriving well after the old startup deadline.
    g_CreateMoveDiscoveryNext=std::chrono::steady_clock::now()-std::chrono::seconds(90);
    game.m_Initialized=true; Hooks::RetryCreateMoveHook();
    Require(Hooks::hkCreateMove.pTarget==vtable[27] && Hooks::hkCreateMove.isEnabled && !g_CreateMoveDiscoveryPending,
        "bounded deferred discovery installs/enables exactly the late CreateMove target");
    const auto created=creates,enabled=enables;
    Hooks::RetryCreateMoveHook();
    Require(creates==created && enables==enabled,"successful live hook is never recreated on subsequent updates");
    Hooks::hkCreateMove={}; outer=0u; Hooks::InstallFragment();
    Hooks::RetryCreateMoveHook();
    Require(g_CreateMoveDiscoveryPending && g_CreateMoveDiscoveryDeadline>std::chrono::steady_clock::now(),
        "first initialized missing-target probe starts a full discovery budget");
    const auto next=g_CreateMoveDiscoveryNext;
    Hooks::RetryCreateMoveHook();
    Require(g_CreateMoveDiscoveryNext==next,"missing-target updates remain throttled to one second");
    g_CreateMoveDiscoveryDeadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);
    g_CreateMoveDiscoveryNext={};
    Hooks::RetryCreateMoveHook();
    Require(!g_CreateMoveDiscoveryPending && !Hooks::hkCreateMove.pTarget,"deferred discovery stops at its deadline");
    Hooks::InstallFragment(); outer=reinterpret_cast<uintptr_t>(&inner);
    g_CreateMoveDiscoveryDeadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);
    Hooks::RetryCreateMoveHook();
    Require(Hooks::hkCreateMove.isEnabled && !g_CreateMoveDiscoveryPending,
        "available target is probed and accepted before final expiry decision");
    outer=0u; Hooks::InstallFragment(); CancelCreateMoveDiscovery();
    Require(!g_CreateMoveDiscoveryPending,"discovery cancellation helper clears pending retry state");
    Hooks::m_Game=nullptr;
}
static void NativeWireTests()
{
    WireFixture f; Command native; const Command original=native;
    Require(f.Write(&native)==123 && std::memcmp(&original,&native,sizeof(native))==0 &&
        std::memcmp(&original,&f.hkWriteUsercmd.emitted,sizeof(native))==0,
        "unconfirmed server retains every original usercmd byte and native serializer result");
    f.s_ServerUnderstandsVR=true; Command confirmed;
    Require(f.Write(&confirmed)==123 && f.hkWriteUsercmd.emitted.tick_count==-200 &&
        f.hkWriteUsercmd.emitted.viewangles.x>89 && f.hkWriteUsercmd.emitted.viewangles.z==20,
        "confirmed capability retains established private VR encoding");
    f.vr.m_ForceNonVRServerMovement=true; Command forced; const Command forcedOriginal=forced;
    Require(f.Write(&forced)==123 && std::memcmp(&forcedOriginal,&f.hkWriteUsercmd.emitted,sizeof(forced))==0,
        "explicit native-server override remains byte preserving after confirmation");
}
int main()
{
    AcquisitionTests(); HookTests(); NativeWireTests();
    std::printf("PASS: input startup integration (%u checks)\n",checks);
    return EXIT_SUCCESS;
}
