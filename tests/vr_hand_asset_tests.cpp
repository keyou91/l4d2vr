#define NOMINMAX
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../L4D2VR/vr_input_recovery.h"
#include "../L4D2VR/vr_hand_pose_cache.h"
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif

// Compile the real GLB parser and IO boundaries, with only call counting added.
#define VrHandAssetLoader ProductionAssetLoader
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_hands/vr_hand_asset_loader.cpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef VrHandAssetLoader

namespace fs = std::filesystem;
static unsigned checks = 0u;
static void Require(bool value, const char* message)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static std::uint32_t fixtureTick = 0u;
static std::uint32_t ScriptedTick() { return fixtureTick; }
#define GetTickCount ScriptedTick
static std::string runtimeRoot;
static unsigned resolves = 0u;
static std::array<unsigned, 2> loads{};
namespace vr
{
    using VRActionHandle_t = std::uint64_t;
    using VRInputValueHandle_t = std::uint64_t;
    constexpr VRActionHandle_t k_ulInvalidActionHandle = 0u;
    constexpr VRInputValueHandle_t k_ulInvalidInputValueHandle = 0u;
    constexpr unsigned k_unMaxPropertyStringSize = 32768u;
    bool VR_GetRuntimePath(char* output, std::uint32_t capacity, std::uint32_t* required)
    {
        ++resolves;
        *required = static_cast<std::uint32_t>(runtimeRoot.size() + 1u);
        if (capacity < *required) return false;
        std::memcpy(output, runtimeRoot.c_str(), *required);
        return true;
    }
}
struct VrHandAssetLoader
{
    static bool LoadGlb(const std::string& path, VrHandMeshAsset& asset, std::string& error)
    {
        ++loads[path.find("right") != std::string::npos ? 1u : 0u];
        return ProductionAssetLoader::LoadGlb(path, asset, error);
    }
};
struct Game { template<class... Args> static void logMsg(const char*, Args...) {} };
class VrHandSystem
{
public:
    struct HandState
    {
#include "vr_hand_asset_state.inl"
    };
    std::array<HandState, 2> m_Hands{};
    struct FreeWristSnapshot
    {
        Vector position{0.0f, 0.0f, 0.0f};
        std::uint64_t frameToken = 0u;
        std::uint32_t updatedAt = 0u;
        bool valid = false;
    };
    mutable std::mutex m_FreeWristMutex;
    std::array<FreeWristSnapshot, 2> m_FreeWrists{};
    std::unordered_set<std::string> warnings;
#include "vr_hand_asset_status.inl"
    VrHandSystem()
    {
        m_Hands[0].assetFileName = "vr_glove_left_model.glb";
        m_Hands[1].assetFileName = "vr_glove_right_model.glb";
    }
    void ReportWarningOnce(const std::string& warning) { warnings.emplace(warning); }
    bool ResolveSteamVrAssetPath(const char*, std::string&) const;
    bool EnsureAssetsLoaded(bool);
    bool EnsureAssetsAvailable(bool);
    bool IsDependencyUnavailable() const;
    const std::string& DependencyFailureReason() const;
    bool ReadFreeWristModelPosition(bool, const l4d2vr_input::ActionFrame&, Vector&) const;
};
#include "vr_hand_asset_functions.inl"

struct CViewSetup {};
class VR
{
public:
    const CViewSetup* m_VrHandsActiveEyeView = nullptr;
    int m_VrHandsActiveEyeIndex = -1;
    bool m_VrHandsWorldMaskDrawn = false;
    std::atomic<bool> m_MagazineInteractionCalibrationOverlayActive{false};
    std::atomic<bool> m_ManualInventoryEmptyHandsActive{false};
    bool m_MagazineBoxDebugEnabled = false;
    bool m_IsVREnabled = true;
    Game game;
    Game* m_Game = &game;
    bool m_VrHandsEnabled = true;
    bool m_GripReleaseDropEnabled = false;
    bool m_VrHandsGlovesRuntimeFallback = false;
    void* m_Input = this;
    bool m_VrHandsGlovesEnabled = true;
    l4d2vr_input::ActionFrame m_InputActionFrame;
    std::unique_ptr<VrHandSystem> m_VrHands;
    bool m_VrHandsDebugLog = false;
    bool m_NativeViewmodelHandsOnly = false;
    bool m_HideArms = true;
    bool m_VrHandsGlovesFallbackLogged = false;
    bool m_VrHandsGlovesHideArmsBeforeFallback = false;
    bool m_ScopeEnabled = false;
    std::mutex m_PhysicalScopeLensMutex;
    std::array<int, 2> m_PhysicalScopeLenses{};
    bool HasFreshMagazineInteractionDebugBoxWork() const { return false; }
    bool IsPhysicalScopeMode() const { return false; }
    bool ScopeWeaponEligible() const { return false; }
    void FallbackVrHandsGlovesToNative(const char*);
    void BeginVrHandsEyeRender(const CViewSetup&, int);
    void Ready() { m_InputActionFrame.Complete(m_InputActionFrame.Begin(), true); }
};
#include "vr_hand_asset_bridge.inl"

template<class T, size_t N> static void Append(std::vector<std::uint8_t>& output, const T (&values)[N])
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values);
    output.insert(output.end(), bytes, bytes + sizeof(values));
}
static void AppendWord(std::vector<std::uint8_t>& output, std::uint32_t word)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&word);
    output.insert(output.end(), bytes, bytes + sizeof(word));
}
static void Write(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(stream.good(), "fixture write");
}
static std::vector<std::uint8_t> MeshBytes()
{
    std::vector<std::uint8_t> output;
    const float positions[] = { 0,0,0, 1,0,0, 0,1,0 };
    const float normals[] = { 0,0,1, 0,0,1, 0,0,1 };
    const float uv[] = { 0,0, 1,0, 0,1 };
    const std::uint8_t joints[12]{};
    const float weights[] = { 1,0,0,0, 1,0,0,0, 1,0,0,0 };
    const std::uint16_t indices[] = { 0,1,2,0 };
    const float inverse[] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    Append(output, positions); Append(output, normals); Append(output, uv);
    Append(output, joints); Append(output, weights); Append(output, indices); Append(output, inverse);
    Require(output.size() == 228u, "fixture binary layout");
    return output;
}
static std::string MeshJson(bool external)
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":228";
    if (external) json += ",\"uri\":\"" + std::string(u8"mesh-\u00f5-\u0416.bin") + "\"";
    json += R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":12},{"buffer":0,"byteOffset":108,"byteLength":48},{"buffer":0,"byteOffset":156,"byteLength":6},{"buffer":0,"byteOffset":164,"byteLength":64}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5121,"count":3,"type":"VEC4"},{"bufferView":4,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":5,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":6,"componentType":5126,"count":1,"type":"MAT4"}],"skins":[{"joints":[0],"inverseBindMatrices":6}],"nodes":[{"name":"wrist_l"},{"mesh":0,"skin":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"JOINTS_0":3,"WEIGHTS_0":4},"indices":5,"material":0}]}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],"textures":[{"source":0}],"images":[{"uri":")";
    json += std::string(u8"texture-\u00f5-\u0416.bin") + "\"}]}";
    return json;
}
static void WriteMesh(const fs::path& path, bool external = false)
{
    const auto binary = MeshBytes();
    auto json = MeshJson(external);
    if (external)
    {
        Write(path.parent_path() / fs::u8path(u8"mesh-\u00f5-\u0416.bin"), binary);
        Write(path, std::vector<std::uint8_t>(json.begin(), json.end()));
    }
    else
    {
        while (json.size() % 4u) json.push_back(' ');
        std::vector<std::uint8_t> glb;
        AppendWord(glb, 0x46546C67u); AppendWord(glb, 2u);
        AppendWord(glb, static_cast<std::uint32_t>(28u + json.size() + binary.size()));
        AppendWord(glb, static_cast<std::uint32_t>(json.size())); AppendWord(glb, 0x4E4F534Au);
        glb.insert(glb.end(), json.begin(), json.end());
        AppendWord(glb, static_cast<std::uint32_t>(binary.size())); AppendWord(glb, 0x004E4942u);
        glb.insert(glb.end(), binary.begin(), binary.end());
        Write(path, glb);
    }
    Write(path.parent_path() / fs::u8path(u8"texture-\u00f5-\u0416.bin"), { 2u,3u,5u,7u });
}
static fs::path AssetFolder(const fs::path& root)
{
    const auto folder = root / "resources" / "rendermodels" / "vr_glove";
    fs::create_directories(folder);
    runtimeRoot = root.u8string();
    return folder;
}
static void RetrySide(const fs::path& root, unsigned missingSide, bool corrupt)
{
    const auto folder = AssetFolder(root);
    const auto missing = folder / (missingSide ? "vr_glove_right_model.glb" : "vr_glove_left_model.glb");
    const auto healthy = folder / (missingSide ? "vr_glove_left_model.glb" : "vr_glove_right_model.glb");
    WriteMesh(healthy);
    if (corrupt) Write(missing, { 1u,2u,3u });
    VrHandSystem system;
    fixtureTick = 100u;
    Require(system.EnsureAssetsAvailable(true), "one healthy side remains available");
    Require(!system.IsDependencyUnavailable() && !system.m_AssetsLoaded, "partial availability");
    Require(system.m_Hands[1u - missingSide].asset.IsValid(), "healthy asset retained");
    Require(!system.m_Hands[missingSide].asset.IsValid(), "bad side stays unavailable");
    Require(!system.DependencyFailureReason().empty(), "partial failure diagnostic");
    const auto healthyData = system.m_Hands[1u - missingSide].asset.vertices.data();
    const auto before = loads;
    const auto beforeResolves = resolves;
    for (unsigned i = 0u; i < 200u; ++i) Require(system.EnsureAssetsAvailable(true), "retry throttle preserves healthy side");
    Require(loads == before && resolves == beforeResolves, "no repeated IO within interval");
    WriteMesh(missing);
    fixtureTick = 1099u;
    Require(system.EnsureAssetsAvailable(true) && !system.m_AssetsLoaded, "repair waits for bounded retry");
    fixtureTick = 1100u;
    Require(system.EnsureAssetsAvailable(true) && system.m_AssetsLoaded, "repaired side loads on next interval");
    Require(system.m_Hands[missingSide].asset.IsValid(), "repaired side usable");
    Require(system.m_Hands[1u - missingSide].asset.vertices.data() == healthyData, "healthy asset never replaced");
    Require(loads[1u - missingSide] == before[1u - missingSide], "healthy side never reloaded");
    Require(system.DependencyFailureReason().empty(), "failure cleared after repair");
}
static void BridgeRecovery(const fs::path& root, bool hideArms)
{
    const auto folder = AssetFolder(root);
    VR bridge;
    bridge.m_HideArms = hideArms;
    CViewSetup view;
    fixtureTick = 2000u;
    bridge.BeginVrHandsEyeRender(view, 0);
    Require(!bridge.m_VrHands, "pending input does not attempt assets");
    bridge.Ready();
    bridge.BeginVrHandsEyeRender(view, 0);
    Require(bridge.m_VrHands && bridge.m_VrHands->IsDependencyUnavailable(), "missing pair detected");
    Require(!bridge.m_VrHandsEnabled && bridge.m_NativeViewmodelHandsOnly && !bridge.m_HideArms,
        "native hands render during asset outage");
    Require(bridge.m_VrHandsGlovesEnabled && bridge.m_VrHandsGlovesRuntimeFallback, "requested glove mode retained");
    Require(bridge.m_VrHandsActiveEyeView == &view, "native eye render remains active");
    const auto before = resolves;
    fixtureTick = 2999u;
    bridge.BeginVrHandsEyeRender(view, 1);
    Require(resolves == before, "fallback retries are bounded");
    WriteMesh(folder / "vr_glove_left_model.glb");
    fixtureTick = 3000u;
    bridge.m_InputActionFrame.Complete(bridge.m_InputActionFrame.Begin(), false);
    bridge.BeginVrHandsEyeRender(view, 0);
    Require(resolves == before && bridge.m_VrHandsGlovesRuntimeFallback, "failed input pauses fallback retry");
    bridge.Ready();
    bridge.BeginVrHandsEyeRender(view, 0);
    Require(bridge.m_VrHandsEnabled && !bridge.m_NativeViewmodelHandsOnly && bridge.m_HideArms == hideArms,
        "healthy asset automatically restores requested mode");
    Require(!bridge.m_VrHandsGlovesRuntimeFallback && bridge.m_VrHandsGlovesEnabled, "runtime fallback cleared without disabling choice");
    WriteMesh(folder / "vr_glove_right_model.glb");
    fixtureTick = 4000u;
    bridge.BeginVrHandsEyeRender(view, 1);
    Require(bridge.m_VrHands->m_AssetsLoaded, "remaining side recovers in active glove mode");
    bridge.m_VrHandsEnabled = false;
    bridge.m_VrHandsGlovesRuntimeFallback = false;
    bridge.m_NativeViewmodelHandsOnly = true;
    bridge.BeginVrHandsEyeRender(view, 0);
    Require(!bridge.m_VrHandsEnabled && bridge.m_NativeViewmodelHandsOnly, "explicit native selection respected");
}
int main()
{
    const auto scratch = fs::temp_directory_path() / ("hand-asset-fixture-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()));
    fs::create_directories(scratch);
    for (unsigned side = 0u; side < 2u; ++side)
        for (unsigned corrupt = 0u; corrupt < 2u; ++corrupt)
            RetrySide(scratch / ("ascii-" + std::to_string(side) + std::to_string(corrupt)), side, corrupt != 0u);
    const auto unicode = scratch / fs::u8path(u8"Runtime-\u00f5-\u0416-\u624b");
    const auto folder = AssetFolder(unicode);
    const auto glb = folder / "vr_glove_left_model.glb";
    WriteMesh(glb);
    std::string resolved, error;
    VrHandMeshAsset mesh;
    VrHandSystem system;
    Require(system.ResolveSteamVrAssetPath("vr_glove_left_model.glb", resolved), "Unicode runtime resolver");
    Require(resolved == glb.u8string(), "resolved path remains UTF8");
    Require(ProductionAssetLoader::LoadGlb(resolved, mesh, error) && mesh.IsValid(), "real Unicode GLB decode");
    Require(mesh.baseColorTextureBytes == std::vector<std::uint8_t>({ 2u,3u,5u,7u }), "Unicode texture file decoded");
    Require(ProductionAssetLoader::LoadStaticGlb(resolved, mesh, error) && mesh.IsValid(), "static mesh Unicode reader shares callback");
    const auto external = folder / fs::u8path(u8"hand-\u00f5-\u0416.gltf");
    WriteMesh(external, true);
    Require(ProductionAssetLoader::LoadGlb(external.u8string(), mesh, error) && mesh.IsValid(), "Unicode external buffer decoded");
    Require(mesh.baseColorTextureBytes.size() == 4u, "Unicode external image decoded");
    Require(!system.ResolveSteamVrAssetPath(nullptr, resolved), "null filename rejected");
    RetrySide(scratch / fs::u8path(u8"Partial-\u00f5-\u0416"), 1u, false);
    BridgeRecovery(scratch / fs::u8path(u8"Fallback-\u00f5-\u0416"), true);
    BridgeRecovery(scratch / "fallback-visible-arms", false);
    l4d2vr_input::ActionFrame frame;
    frame.Complete(frame.Begin(), true);
    system.m_FreeWrists[0] = { Vector(1.0f, 2.0f, 3.0f), frame.Read(), fixtureTick, true };
    Vector position(0.0f, 0.0f, 0.0f);
    Require(system.ReadFreeWristModelPosition(true, frame, position) && position.x == 1.0f,
        "real wrist reader compiles and returns validated position");
    std::printf("Hand asset actual-source fixture passed %u checks; ACP=%u\n", checks, GetACP());
    return 0;
}
