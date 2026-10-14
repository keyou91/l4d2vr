#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_scope_optics.h"
#include "../L4D2VR/vr_hands/vr_hand_math.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Scope renderer failed at %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)
using DWORD = std::uint32_t;
using D3DCOLOR = DWORD;
using HRESULT = std::int32_t;
constexpr HRESULT S_OK = 0, E_FAIL = -1;
constexpr DWORD TRUE = 1, FALSE = 0;
#define SUCCEEDED(value) ((value) >= 0)
#define FAILED(value) ((value) < 0)

enum {
    D3DSBT_ALL, D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION,
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CLIPPING,
    D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE,
    D3DRS_VERTEXBLEND, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_CULLMODE,
    D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
    D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_COLORWRITEENABLE,
    D3DRS_WRAP0, D3DRS_TEXTUREFACTOR, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_COLOROP,
    D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV
};
constexpr DWORD D3DFVF_XYZ = 2, D3DFVF_DIFFUSE = 0x40, D3DFVF_TEX1 = 0x100;
constexpr DWORD D3DCMP_LESSEQUAL = 4, D3DFILL_SOLID = 3, D3DFILL_WIREFRAME = 2;
constexpr DWORD D3DVBF_DISABLE = 0, D3DCULL_NONE = 1, D3DTTFF_DISABLE = 0;
constexpr DWORD D3DTOP_SELECTARG1 = 2, D3DTOP_MODULATE = 4, D3DTOP_DISABLE = 1;
constexpr DWORD D3DTA_TFACTOR = 3, D3DTA_DIFFUSE = 0, D3DTA_TEXTURE = 2;
constexpr DWORD D3DTEXF_LINEAR = 2, D3DTADDRESS_CLAMP = 3;
constexpr DWORD D3DBLENDOP_ADD = 1, D3DBLEND_SRCALPHA = 5, D3DBLEND_INVSRCALPHA = 6;
constexpr int D3DPT_TRIANGLEFAN = 6, D3DPT_LINELIST = 2;
static std::uint64_t GetTickCount64() { return 1000u; }
static D3DCOLOR D3DCOLOR_ARGB(DWORD alpha, DWORD red, DWORD green, DWORD blue)
{ return (alpha << 24) | (red << 16) | (green << 8) | blue; }

struct D3DMATRIX { float m[4][4]{}; };
struct D3DVIEWPORT9 {
    DWORD X = 0, Y = 0, Width = 0, Height = 0;
    float MinZ = 0, MaxZ = 1;
    bool operator==(const D3DVIEWPORT9& other) const
    { return X == other.X && Y == other.Y && Width == other.Width && Height == other.Height && MinZ == other.MinZ && MaxZ == other.MaxZ; }
};
struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const Rect& other) const
    { return left == other.left && top == other.top && right == other.right && bottom == other.bottom; }
};
struct _GUID { unsigned long a; unsigned short b, c; unsigned char d[8]; };
#ifdef _MSC_VER
struct __declspec(uuid("d4949f93-20e9-4c23-89bd-c44212bb0d95")) IDirect3DTexture9 {
#else
#define __uuidof(type) _GUID{}
struct IDirect3DTexture9 {
#endif
    int refs = 1;
    void Release() { --refs; }
};
struct IDirect3DDevice9;
struct IDirect3DSurface9 {
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* texture = nullptr;
    int refs = 1;
    int width = 800, height = 600;
    HRESULT GetDevice(IDirect3DDevice9** out);
    HRESULT GetContainer(const _GUID&, void** out)
    {
        *out = texture;
        if (!texture) return E_FAIL;
        ++texture->refs;
        return S_OK;
    }
    void Release() { --refs; }
};
struct Pipeline {
    std::map<int, DWORD> render;
    std::map<std::pair<int, int>, DWORD> stage, sampler;
    std::map<int, std::array<float, 16>> transforms;
    DWORD fvf = 0;
    std::uintptr_t vertexShader = 0, pixelShader = 0, stream = 0;
    IDirect3DTexture9* texture = nullptr;
    D3DVIEWPORT9 viewport{};
    Rect scissor{};
    bool operator==(const Pipeline& other) const
    {
        return render == other.render && stage == other.stage && sampler == other.sampler &&
            transforms == other.transforms && fvf == other.fvf && vertexShader == other.vertexShader &&
            pixelShader == other.pixelShader && stream == other.stream && texture == other.texture &&
            viewport == other.viewport && scissor == other.scissor;
    }
};
struct DrawRecord { Pipeline pipeline; int type; unsigned primitives, stride; std::vector<unsigned char> vertices; };
struct IDirect3DStateBlock9 {
    IDirect3DDevice9* device;
    Pipeline saved;
    explicit IDirect3DStateBlock9(IDirect3DDevice9* owner) : device(owner) {}
    HRESULT Capture();
    HRESULT Apply();
    void Release();
};
struct IDirect3DDevice9 {
    Pipeline pipeline;
    IDirect3DTexture9 raw{}, processed{}, originalTexture{};
    IDirect3DSurface9 original{}, eye{}, depth{}, rawSurface{};
    IDirect3DSurface9* target = &original;
    std::vector<DrawRecord> draws;
    std::vector<std::string> setters;
    int refs = 1, stateBlocks = 0, failSetter = 0, failDraw = 0, drawsAtFailure = -1;
    bool setterFailed = false;
    std::string failure;
    IDirect3DDevice9()
    {
        original.device = eye.device = depth.device = rawSurface.device = this;
        eye.width = eye.height = 256;
        rawSurface.texture = &raw;
        pipeline.render = {{D3DRS_SCISSORTESTENABLE, TRUE}, {D3DRS_CLIPPLANEENABLE, 1u},
            {D3DRS_FILLMODE, D3DFILL_WIREFRAME}, {D3DRS_VERTEXBLEND, 3u},
            {D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE}, {D3DRS_WRAP0, 3u},
            {D3DRS_BLENDOP, 3u}, {D3DRS_SEPARATEALPHABLENDENABLE, TRUE},
            {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_LIGHTING, TRUE},
            {D3DRS_ALPHATESTENABLE, TRUE}, {D3DRS_STENCILENABLE, TRUE}};
        pipeline.stage = {{{0, D3DTSS_TEXCOORDINDEX}, 0x30000u},
            {{0, D3DTSS_TEXTURETRANSFORMFLAGS}, 3u | 256u}, {{0, D3DTSS_ALPHAOP}, 7u},
            {{1, D3DTSS_COLOROP}, D3DTOP_MODULATE}};
        pipeline.sampler[std::make_pair(0, D3DSAMP_ADDRESSU)] = 1u;
        pipeline.viewport = {7, 11, 401, 307, 0.2f, 0.8f};
        pipeline.scissor = {13, 17, 31, 43};
        pipeline.vertexShader = 0x101u; pipeline.pixelShader = 0x202u;
        pipeline.stream = 0x303u; pipeline.fvf = 0x777u; pipeline.texture = &originalTexture;
        pipeline.transforms[D3DTS_WORLD] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 5, 6, 7, 1};
    }
    bool Setter(const std::string& name)
    {
        setters.push_back(name);
        if (static_cast<int>(setters.size()) != failSetter) return true;
        setterFailed = true; drawsAtFailure = static_cast<int>(draws.size()); return false;
    }
    HRESULT CreateStateBlock(int, IDirect3DStateBlock9** out)
    {
        if (failure == "CreateStateBlock") { *out = nullptr; return E_FAIL; }
        *out = new IDirect3DStateBlock9(this); ++stateBlocks; return S_OK;
    }
    HRESULT GetRenderTarget(int, IDirect3DSurface9** out)
    { if (failure == "GetRenderTarget") { *out = nullptr; return E_FAIL; } *out = target; ++target->refs; return S_OK; }
    HRESULT GetDepthStencilSurface(IDirect3DSurface9** out)
    { if (failure == "GetDepth") { *out = nullptr; return E_FAIL; } *out = &depth; ++depth.refs; return S_OK; }
    HRESULT GetViewport(D3DVIEWPORT9* out)
    { if (failure == "GetViewport") return E_FAIL; *out = pipeline.viewport; return S_OK; }
    HRESULT SetRenderTarget(int, IDirect3DSurface9* value)
    {
        if (!Setter(value == &original ? "RestoreTarget" : "EyeTarget")) return E_FAIL;
        target = value;
        // D3D9 resets both states when the target changes, even to the original target.
        pipeline.viewport = {0, 0, static_cast<DWORD>(value->width), static_cast<DWORD>(value->height), 0, 1};
        pipeline.scissor = {0, 0, value->width, value->height};
        return S_OK;
    }
    HRESULT SetViewport(const D3DVIEWPORT9* value)
    { if (!Setter("SetViewport")) return E_FAIL; pipeline.viewport = *value; return S_OK; }
    HRESULT SetVertexShader(void* value)
    { if (!Setter("SetVertexShader")) return E_FAIL; pipeline.vertexShader = reinterpret_cast<std::uintptr_t>(value); return S_OK; }
    HRESULT SetPixelShader(void* value)
    { if (!Setter("SetPixelShader")) return E_FAIL; pipeline.pixelShader = reinterpret_cast<std::uintptr_t>(value); return S_OK; }
    HRESULT SetFVF(DWORD value)
    { if (!Setter("SetFVF")) return E_FAIL; pipeline.fvf = value; return S_OK; }
    HRESULT SetTransform(int key, const D3DMATRIX* matrix)
    {
        if (!Setter("SetTransform")) return E_FAIL;
        std::array<float, 16> value{};
        for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
            value[static_cast<size_t>(row * 4 + col)] = matrix->m[row][col];
        pipeline.transforms[key] = value; return S_OK;
    }
    HRESULT SetRenderState(int key, DWORD value)
    { if (!Setter("SetRenderState:" + std::to_string(key))) return E_FAIL; pipeline.render[key] = value; return S_OK; }
    HRESULT SetTexture(int, IDirect3DTexture9* value)
    { if (!Setter("SetTexture")) return E_FAIL; pipeline.texture = value; return S_OK; }
    HRESULT SetTextureStageState(int stage, int key, DWORD value)
    { if (!Setter("SetTextureStageState:" + std::to_string(stage) + ":" + std::to_string(key))) return E_FAIL; pipeline.stage[{stage, key}] = value; return S_OK; }
    HRESULT SetSamplerState(int stage, int key, DWORD value)
    { if (!Setter("SetSamplerState:" + std::to_string(key))) return E_FAIL; pipeline.sampler[{stage, key}] = value; return S_OK; }
    HRESULT DrawPrimitiveUP(int type, unsigned primitives, const void* vertices, unsigned stride)
    {
        CHECK(!setterFailed);
        const auto count = type == D3DPT_TRIANGLEFAN ? primitives + 2u : primitives * 2u;
        const auto* bytes = static_cast<const unsigned char*>(vertices);
        draws.push_back({pipeline, type, primitives, stride, {bytes, bytes + count * stride}});
        pipeline.stream = 0u; // DrawPrimitiveUP clears stream zero in D3D9.
        return static_cast<int>(draws.size()) == failDraw ? E_FAIL : S_OK;
    }
    void Release() { --refs; }
};
HRESULT IDirect3DSurface9::GetDevice(IDirect3DDevice9** out)
{ if (device->failure == "GetDevice") { *out = nullptr; return E_FAIL; } *out = device; ++device->refs; return S_OK; }
HRESULT IDirect3DStateBlock9::Capture()
{ if (device->failure == "Capture") return E_FAIL; saved = device->pipeline; return S_OK; }
HRESULT IDirect3DStateBlock9::Apply()
{ if (device->failure == "Apply") return E_FAIL; device->pipeline = saved; return S_OK; }
void IDirect3DStateBlock9::Release() { --device->stateBlocks; delete this; }

struct Bridge {
    int locks = 0, unlocks = 0;
    bool fail = false;
    HRESULT LockDevice() { if (fail) return E_FAIL; ++locks; return S_OK; }
    void UnlockDevice() { ++unlocks; }
};
static Bridge bridge;
static Bridge* g_D3DVR9 = &bridge;
using TextureStateMutex = std::recursive_mutex;
enum TextureId { Texture_None, Texture_Busy };
struct Game { int queue = 1; int GetMatQueueMode() const { return queue; } };
struct CViewSetup {
    int width = 256, height = 256;
    float fovViewmodel = 90, zNearViewmodel = 6, zFarViewmodel = 1000;
    Vector origin{0, 0, 0}, angles{0, 0, 0};
};
struct PhysicalScopeLensSnapshot {
    Vector center{2.16f, 0, 0}, forward{1, 0, 0}, right{0, -1, 0}, up{0, 0, 1};
    Vector sceneCenter{2.16f, 0, 0}, sceneForward{1, 0, 0};
    float radius = 0.66f, sceneRadius = 0.66f, magnification = 2;
    int weaponId = 6;
    l4d2vr_optics::WeaponIdentity weapon{};
    std::uint64_t publishedAt = 1000u;
};
struct VR {
    Game game; Game* m_Game = &game;
    bool m_ScopeEnabled = true, m_VRNearClipSelfBody = true;
    std::atomic<int> m_PhysicalScopeWeaponId{6};
    std::mutex m_PhysicalScopeLensMutex;
    TextureStateMutex m_TextureMutex;
    l4d2vr_optics::WeaponTracker m_PhysicalScopeWeapons;
    l4d2vr_optics::ImageStamp m_PhysicalScopeImage{}, m_PhysicalScopeProcessedImage{};
    std::atomic<DWORD> m_SourceRenderQueueMarkerEpoch{7}, m_ScopeLensOverlayReady{0};
    TextureId m_CreatingTextureID = Texture_None;
    IDirect3DSurface9* m_D9LeftEyeSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeSurface = nullptr;
    IDirect3DSurface9* m_D9ScopeSurface = nullptr;
    IDirect3DTexture9* m_D9ScopeLensTexture = nullptr;
    float m_VRScale = 43.2f, m_ScopeReticleAlpha = 0.5f;
    VR() { m_PhysicalScopeWeapons.Observe(1u, 0x3005u, 6); }
    bool IsPhysicalScopeMode() const { return true; }
    bool HasFreshPhysicalScopeTracking() const { return true; }
    bool DrawPhysicalScopeLensForEye(const CViewSetup&, int, const PhysicalScopeLensSnapshot&);
};
#include "vr_physical_scope_matrix.inl"
#include "vr_physical_scope_draw.inl"

static PhysicalScopeLensSnapshot Setup(VR& vr, IDirect3DDevice9& device, int mode = 0)
{
    PhysicalScopeLensSnapshot lens;
    lens.weapon = vr.m_PhysicalScopeWeapons.Current();
    vr.m_D9LeftEyeSurface = vr.m_D9RightEyeSurface = &device.eye;
    vr.m_PhysicalScopeImage = {lens.weapon, 2, 1000u, 7u};
    if (mode == 0) vr.m_D9ScopeSurface = &device.rawSurface;
    if (mode == 1) {
        vr.m_D9ScopeLensTexture = &device.processed; vr.m_ScopeLensOverlayReady = 1;
        vr.m_PhysicalScopeProcessedImage = vr.m_PhysicalScopeImage;
    }
    return lens;
}
static void Balanced(const IDirect3DDevice9& device)
{
    CHECK(device.refs == 1 && device.stateBlocks == 0);
    CHECK(device.original.refs == 1 && device.depth.refs == 1 && device.eye.refs == 1);
    CHECK(device.raw.refs == 1 && device.processed.refs == 1);
    CHECK(bridge.locks == bridge.unlocks);
}
static void Deterministic(const DrawRecord& draw)
{
    const auto& pipeline = draw.pipeline;
    CHECK(pipeline.vertexShader == 0u && pipeline.pixelShader == 0u);
    CHECK(pipeline.fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1));
    CHECK(pipeline.render.at(D3DRS_ZENABLE) == TRUE && pipeline.render.at(D3DRS_ZWRITEENABLE) == FALSE);
    CHECK(pipeline.render.at(D3DRS_ZFUNC) == D3DCMP_LESSEQUAL);
    CHECK(pipeline.render.at(D3DRS_SCISSORTESTENABLE) == FALSE && pipeline.render.at(D3DRS_CLIPPLANEENABLE) == 0u);
    CHECK(pipeline.render.at(D3DRS_FILLMODE) == D3DFILL_SOLID && pipeline.render.at(D3DRS_WRAP0) == 0u);
    CHECK(pipeline.render.at(D3DRS_VERTEXBLEND) == D3DVBF_DISABLE && pipeline.render.at(D3DRS_INDEXEDVERTEXBLENDENABLE) == FALSE);
    CHECK(pipeline.stage.at({0, D3DTSS_TEXCOORDINDEX}) == 0u && pipeline.stage.at({0, D3DTSS_TEXTURETRANSFORMFLAGS}) == D3DTTFF_DISABLE);
    CHECK(pipeline.stage.at({1, D3DTSS_COLOROP}) == D3DTOP_DISABLE && pipeline.stage.at({1, D3DTSS_ALPHAOP}) == D3DTOP_DISABLE);
    CHECK(pipeline.stage.at({0, D3DTSS_ALPHAOP}) == D3DTOP_SELECTARG1 && pipeline.stage.at({0, D3DTSS_ALPHAARG1}) == D3DTA_DIFFUSE);
    if (draw.type == D3DPT_LINELIST) {
        CHECK(pipeline.render.at(D3DRS_BLENDOP) == D3DBLENDOP_ADD && pipeline.render.at(D3DRS_SEPARATEALPHABLENDENABLE) == FALSE);
        CHECK(pipeline.render.at(D3DRS_SRCBLEND) == D3DBLEND_SRCALPHA && pipeline.render.at(D3DRS_DESTBLEND) == D3DBLEND_INVSRCALPHA);
    }
}
int main()
{
    size_t successfulSetters = 0;
    for (int mode = 0; mode < 3; ++mode) {
        IDirect3DDevice9 device; VR vr; const auto lens = Setup(vr, device, mode);
        const auto before = device.pipeline;
        CHECK(vr.DrawPhysicalScopeLensForEye(CViewSetup{}, 0, lens));
        CHECK(device.draws.size() == static_cast<size_t>(mode == 0 ? 3 : mode == 1 ? 2 : 1));
        for (const auto& draw : device.draws) Deterministic(draw);
        CHECK(device.pipeline == before && device.target == &device.original);
        CHECK(device.draws[0].stride == sizeof(float) * 6u);
        float center[3]{}; std::memcpy(center, device.draws[0].vertices.data(), sizeof(center));
        CHECK(center[0] == lens.center.x && center[1] == lens.center.y && center[2] == lens.center.z);
        Balanced(device);
        if (mode == 0) successfulSetters = device.setters.size();
    }
    CHECK(successfulSetters >= 50u);
    // Last two setters restore target and viewport; all earlier setters prepare a draw.
    for (size_t fail = 1u; fail + 2u <= successfulSetters; ++fail) {
        IDirect3DDevice9 device; VR vr; const auto lens = Setup(vr, device);
        const auto before = device.pipeline; device.failSetter = static_cast<int>(fail);
        CHECK(!vr.DrawPhysicalScopeLensForEye(CViewSetup{}, 0, lens));
        CHECK(device.setterFailed && static_cast<int>(device.draws.size()) == device.drawsAtFailure);
        CHECK(device.pipeline == before && device.target == &device.original); Balanced(device);
    }
    for (int fail = 1; fail <= 3; ++fail) {
        IDirect3DDevice9 device; VR vr; const auto lens = Setup(vr, device);
        const auto before = device.pipeline; device.failDraw = fail;
        CHECK(!vr.DrawPhysicalScopeLensForEye(CViewSetup{}, 0, lens));
        CHECK(device.draws.size() == static_cast<size_t>(fail));
        CHECK(device.pipeline == before && device.target == &device.original); Balanced(device);
    }
    for (const char* failure : {"GetDevice", "CreateStateBlock", "Capture", "GetRenderTarget", "GetDepth", "GetViewport"}) {
        IDirect3DDevice9 device; VR vr; const auto lens = Setup(vr, device);
        const auto before = device.pipeline; device.failure = failure;
        CHECK(!vr.DrawPhysicalScopeLensForEye(CViewSetup{}, 0, lens));
        CHECK(device.draws.empty() && device.pipeline == before && device.target == &device.original); Balanced(device);
    }
    for (size_t fail : {successfulSetters - 1u, successfulSetters}) {
        IDirect3DDevice9 device; VR vr; const auto lens = Setup(vr, device);
        device.failSetter = static_cast<int>(fail);
        CHECK(!vr.DrawPhysicalScopeLensForEye(CViewSetup{}, 0, lens)); CHECK(device.draws.size() == 3u); Balanced(device);
    }
    {
        IDirect3DDevice9 device; VR vr; const auto lens = Setup(vr, device); device.failure = "Apply";
        CHECK(!vr.DrawPhysicalScopeLensForEye(CViewSetup{}, 0, lens)); CHECK(device.draws.size() == 3u); Balanced(device);
    }
    std::printf("Actual mounted scope: poisoned state, %zu setter failures, draw failures and restoration checks passed\n", successfulSetters - 2u);
}
