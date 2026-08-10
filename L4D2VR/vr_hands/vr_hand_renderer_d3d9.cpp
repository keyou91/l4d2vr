#include "vr_hand_renderer_d3d9.h"

#include "vr_hand_math.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace
{
    constexpr int kMaxShaderBones = 64;
    constexpr int kBoneConstantStart = 8;
    constexpr float kWorldDepthRangeMin = 0.0f;
    constexpr float kWorldDepthRangeMax = 1.0f;
    // VR hands and detached viewmodel components are placed in world space and
    // share the eye scene projection. Keep their depth comparable with Source's
    // world depth so nearby geometry can occlude them.
    constexpr float kViewmodelDepthRangeMax = 1.0f;
    constexpr DWORD kVrHandOcclusionStencilBit = 0x80u;

    const char* kVertexShaderSource = R"HLSL(
float4 gWorldViewProjectionRows[4] : register(c0);
float4 gLightDirectionAmbient : register(c4);
float4 gWorldNormalRows[3] : register(c5);
float4 gBoneRows[192] : register(c8);

struct VS_INPUT
{
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float4 weights : BLENDWEIGHT0;
    float4 joints : BLENDINDICES0;
};

struct VS_OUTPUT
{
    float4 position : POSITION0;
    float2 uv : TEXCOORD0;
    float light : TEXCOORD1;
};

float3 TransformBonePosition(float3 position, int joint)
{
    int row = joint * 3;
    float4 v = float4(position, 1.0);
    return float3(dot(v, gBoneRows[row + 0]), dot(v, gBoneRows[row + 1]), dot(v, gBoneRows[row + 2]));
}

float3 TransformBoneNormal(float3 normal, int joint)
{
    int row = joint * 3;
    return float3(dot(normal, gBoneRows[row + 0].xyz), dot(normal, gBoneRows[row + 1].xyz), dot(normal, gBoneRows[row + 2].xyz));
}

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    int4 joints = int4(input.joints);
    float3 skinnedPosition =
        TransformBonePosition(input.position, joints.x) * input.weights.x +
        TransformBonePosition(input.position, joints.y) * input.weights.y +
        TransformBonePosition(input.position, joints.z) * input.weights.z +
        TransformBonePosition(input.position, joints.w) * input.weights.w;
    float3 skinnedNormal = normalize(
        TransformBoneNormal(input.normal, joints.x) * input.weights.x +
        TransformBoneNormal(input.normal, joints.y) * input.weights.y +
        TransformBoneNormal(input.normal, joints.z) * input.weights.z +
        TransformBoneNormal(input.normal, joints.w) * input.weights.w);
    float3 worldNormal = normalize(float3(
        dot(skinnedNormal, gWorldNormalRows[0].xyz),
        dot(skinnedNormal, gWorldNormalRows[1].xyz),
        dot(skinnedNormal, gWorldNormalRows[2].xyz)));
    float4 p = float4(skinnedPosition, 1.0);
    output.position = float4(
        dot(p, gWorldViewProjectionRows[0]),
        dot(p, gWorldViewProjectionRows[1]),
        dot(p, gWorldViewProjectionRows[2]),
        dot(p, gWorldViewProjectionRows[3]));
    output.uv = input.uv;
    float rawLight = (gLightDirectionAmbient.w +
        max(dot(worldNormal, -gLightDirectionAmbient.xyz), 0.0) * 0.62) * gWorldNormalRows[2].w;
    float visibleLight = saturate(rawLight);
    float sceneScale = saturate(gWorldNormalRows[2].w);
    float rescue = 0.16 * sqrt(sceneScale) * saturate((0.18 - visibleLight) / 0.18);
    output.light = saturate(visibleLight + rescue);
    return output;
}
)HLSL";

    const char* kPixelShaderSource = R"HLSL(
sampler2D gTexture : register(s0);

float4 main(float2 uv : TEXCOORD0, float light : TEXCOORD1) : COLOR0
{
    float4 color = tex2D(gTexture, uv);
    return float4(color.rgb * light, color.a);
}
)HLSL";

    template <typename T>
    void SafeRelease(T*& pointer)
    {
        if (!pointer)
            return;
        pointer->Release();
        pointer = nullptr;
    }

    bool CompileVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9** outShader, std::string& outError)
    {
        ID3DXBuffer* bytecode = nullptr;
        ID3DXBuffer* errors = nullptr;
        const HRESULT compileResult = D3DXCompileShader(
            kVertexShaderSource,
            static_cast<UINT>(std::strlen(kVertexShaderSource)),
            nullptr,
            nullptr,
            "main",
            "vs_3_0",
            0,
            &bytecode,
            &errors,
            nullptr);
        if (FAILED(compileResult) || !bytecode)
        {
            outError = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DXCompileShader failed for VR hand vertex shader";
            SafeRelease(errors);
            SafeRelease(bytecode);
            return false;
        }
        SafeRelease(errors);
        const HRESULT createResult = device->CreateVertexShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), outShader);
        SafeRelease(bytecode);
        if (FAILED(createResult) || !*outShader)
        {
            outError = "CreateVertexShader failed for VR hands";
            return false;
        }
        return true;
    }

    bool CompilePixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9** outShader, std::string& outError)
    {
        ID3DXBuffer* bytecode = nullptr;
        ID3DXBuffer* errors = nullptr;
        const HRESULT compileResult = D3DXCompileShader(
            kPixelShaderSource,
            static_cast<UINT>(std::strlen(kPixelShaderSource)),
            nullptr,
            nullptr,
            "main",
            "ps_3_0",
            0,
            &bytecode,
            &errors,
            nullptr);
        if (FAILED(compileResult) || !bytecode)
        {
            outError = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DXCompileShader failed for VR hand pixel shader";
            SafeRelease(errors);
            SafeRelease(bytecode);
            return false;
        }
        SafeRelease(errors);
        const HRESULT createResult = device->CreatePixelShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), outShader);
        SafeRelease(bytecode);
        if (FAILED(createResult) || !*outShader)
        {
            outError = "CreatePixelShader failed for VR hands";
            return false;
        }
        return true;
    }
}

VrHandRendererD3D9::VrHandRendererD3D9() = default;

VrHandRendererD3D9::~VrHandRendererD3D9()
{
    OnDeviceLost();
}

bool VrHandRendererD3D9::EnsureSharedResources(IDirect3DDevice9* device, std::string& outError)
{
    if (!device)
    {
        outError = "VR hand renderer received no D3D9 device";
        return false;
    }
    if (m_Device != device)
    {
        OnDeviceLost();
        m_Device = device;
        m_Device->AddRef();
    }
    if (m_VertexDeclaration && m_VertexShader && m_PixelShader)
        return true;

    const D3DVERTEXELEMENT9 elements[] =
    {
        { 0, static_cast<WORD>(offsetof(VrHandVertex, position)), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, normal)), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, uv)), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, weights)), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, joints)), D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
        D3DDECL_END()
    };

    if (FAILED(device->CreateVertexDeclaration(elements, &m_VertexDeclaration)) || !m_VertexDeclaration)
    {
        outError = "CreateVertexDeclaration failed for VR hands";
        return false;
    }
    if (!CompileVertexShader(device, &m_VertexShader, outError))
        return false;
    if (!CompilePixelShader(device, &m_PixelShader, outError))
        return false;
    return true;
}

bool VrHandRendererD3D9::CreateTexture(IDirect3DDevice9* device, const VrHandMeshAsset& asset, IDirect3DTexture9** outTexture, std::string& outError)
{
    *outTexture = nullptr;
    if (!asset.baseColorTextureBytes.empty())
    {
        const HRESULT result = D3DXCreateTextureFromFileInMemory(
            device,
            asset.baseColorTextureBytes.data(),
            static_cast<UINT>(asset.baseColorTextureBytes.size()),
            outTexture);
        if (SUCCEEDED(result) && *outTexture)
            return true;
    }

    if (FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, outTexture, nullptr)) || !*outTexture)
    {
        outError = "cannot create VR hand fallback texture";
        return false;
    }

    D3DLOCKED_RECT lock{};
    if (FAILED((*outTexture)->LockRect(0, &lock, nullptr, 0)))
    {
        outError = "cannot initialize VR hand fallback texture";
        SafeRelease(*outTexture);
        return false;
    }
    *static_cast<DWORD*>(lock.pBits) = asset.fallbackColorArgb;
    (*outTexture)->UnlockRect(0);
    return true;
}

bool VrHandRendererD3D9::EnsureMeshResources(IDirect3DDevice9* device, int handIndex, const VrHandMeshAsset& asset, std::string& outError)
{
    if (handIndex < 0 || handIndex >= static_cast<int>(m_Meshes.size()))
    {
        outError = "invalid VR hand index";
        return false;
    }
    if (!asset.IsValid())
    {
        outError = "VR hand mesh asset is invalid";
        return false;
    }

    MeshResources& mesh = m_Meshes[static_cast<size_t>(handIndex)];
    if (mesh.vertexBuffer && mesh.indexBuffer && mesh.texture && mesh.sourcePath == asset.sourcePath)
        return true;
    ReleaseMesh(mesh);

    const UINT vertexBytes = static_cast<UINT>(asset.vertices.size() * sizeof(VrHandVertex));
    const UINT indexBytes = static_cast<UINT>(asset.indices.size() * sizeof(std::uint16_t));
    if (FAILED(device->CreateVertexBuffer(vertexBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &mesh.vertexBuffer, nullptr)) || !mesh.vertexBuffer)
    {
        outError = "CreateVertexBuffer failed for VR hands";
        return false;
    }
    if (FAILED(device->CreateIndexBuffer(indexBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &mesh.indexBuffer, nullptr)) || !mesh.indexBuffer)
    {
        outError = "CreateIndexBuffer failed for VR hands";
        ReleaseMesh(mesh);
        return false;
    }

    void* vertexData = nullptr;
    if (FAILED(mesh.vertexBuffer->Lock(0, vertexBytes, &vertexData, 0)) || !vertexData)
    {
        outError = "cannot lock VR hand vertex buffer";
        ReleaseMesh(mesh);
        return false;
    }
    std::memcpy(vertexData, asset.vertices.data(), vertexBytes);
    mesh.vertexBuffer->Unlock();

    void* indexData = nullptr;
    if (FAILED(mesh.indexBuffer->Lock(0, indexBytes, &indexData, 0)) || !indexData)
    {
        outError = "cannot lock VR hand index buffer";
        ReleaseMesh(mesh);
        return false;
    }
    std::memcpy(indexData, asset.indices.data(), indexBytes);
    mesh.indexBuffer->Unlock();

    if (!CreateTexture(device, asset, &mesh.texture, outError))
    {
        ReleaseMesh(mesh);
        return false;
    }

    mesh.sourcePath = asset.sourcePath;
    mesh.vertexCount = static_cast<unsigned int>(asset.vertices.size());
    mesh.indexCount = static_cast<unsigned int>(asset.indices.size());
    return true;
}

bool VrHandRendererD3D9::ClearViewmodelOcclusionStencil(IDirect3DDevice9* device, std::string& outError)
{
    outError.clear();
    if (!device)
    {
        outError = "VR hand stencil clear received a null D3D9 device";
        return false;
    }

    IDirect3DSurface9* depthStencil = nullptr;
    D3DSURFACE_DESC depthStencilDesc{};
    if (FAILED(device->GetDepthStencilSurface(&depthStencil)) || !depthStencil || FAILED(depthStencil->GetDesc(&depthStencilDesc)))
    {
        SafeRelease(depthStencil);
        outError = "VR hand viewmodel occlusion requires an active depth-stencil surface";
        return false;
    }
    const bool hasStencil =
        depthStencilDesc.Format == D3DFMT_D15S1 ||
        depthStencilDesc.Format == D3DFMT_D24S8 ||
        depthStencilDesc.Format == D3DFMT_D24X4S4 ||
        depthStencilDesc.Format == D3DFMT_D24FS8;
    SafeRelease(depthStencil);
    if (!hasStencil)
    {
        outError = "VR hand viewmodel occlusion requires a stencil-capable depth surface";
        return false;
    }

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
    {
        outError = "CreateStateBlock failed before VR hand stencil-bit clear";
        return false;
    }
    stateBlock->Capture();

    D3DVIEWPORT9 viewport{};
    const bool haveViewport = SUCCEEDED(device->GetViewport(&viewport));
    HRESULT drawResult = E_FAIL;
    if (haveViewport)
    {
        struct ClearStencilVertex
        {
            float x, y, z, rhw;
        };
        const float left = static_cast<float>(viewport.X) - 0.5f;
        const float top = static_cast<float>(viewport.Y) - 0.5f;
        const float right = static_cast<float>(viewport.X + viewport.Width) - 0.5f;
        const float bottom = static_cast<float>(viewport.Y + viewport.Height) - 0.5f;
        const ClearStencilVertex vertices[4] =
        {
            { left,  top,    0.0f, 1.0f },
            { right, top,    0.0f, 1.0f },
            { left,  bottom, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f }
        };

        // Clear only the reserved VR-hand stencil bit. D3D9 Clear(D3DCLEAR_STENCIL)
        // would erase every stencil bit and could disturb Source's own later passes.
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0u);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRS_STENCILREF, 0u);
        device->SetRenderState(D3DRS_STENCILMASK, kVrHandOcclusionStencilBit);
        device->SetRenderState(D3DRS_STENCILWRITEMASK, kVrHandOcclusionStencilBit);
        device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        device->SetVertexDeclaration(nullptr);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetTexture(0, nullptr);
        device->SetFVF(D3DFVF_XYZRHW);
        drawResult = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ClearStencilVertex));
    }

    stateBlock->Apply();
    SafeRelease(stateBlock);
    if (!haveViewport || FAILED(drawResult))
    {
        outError = "D3D9 reserved stencil-bit clear failed before VR hand world-depth mask";
        return false;
    }
    return true;
}

bool VrHandRendererD3D9::Draw(
    IDirect3DDevice9* device,
    int handIndex,
    const VrHandMeshAsset& asset,
    const std::vector<VrHandMatrixRows3x4>& palette,
    const VrHandMatrix4& world,
    const VrHandMatrix4& worldViewProjection,
    VrHandDrawPass drawPass,
    float sceneLightScale,
    std::string& outError)
{
    outError.clear();
    static_assert(sizeof(VrHandMatrixRows3x4) == sizeof(float) * 12u, "VR hand palette rows must be tightly packed");
    if (palette.empty() || palette.size() > kMaxShaderBones)
    {
        outError = "VR hand skinning palette count is invalid";
        return false;
    }
    if (!EnsureSharedResources(device, outError) || !EnsureMeshResources(device, handIndex, asset, outError))
        return false;

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
    {
        outError = "CreateStateBlock failed before VR hand draw";
        return false;
    }
    stateBlock->Capture();

    const MeshResources& mesh = m_Meshes[static_cast<size_t>(handIndex)];
    const std::array<float, 16> wvpRows = VrHandMath::ToRows4x4(worldViewProjection);
    VrHandMatrixRows3x4 worldNormalRows = VrHandMath::ToRows3x4(world);

    const bool maskPass = drawPass == VrHandDrawPass::WorldVisibilityMask;
    const bool compositePass = drawPass == VrHandDrawPass::ViewmodelComposite;
    const bool standaloneViewmodelPass = drawPass == VrHandDrawPass::ViewmodelStandalone;
    const bool viewmodelDepthPass = compositePass || standaloneViewmodelPass;
    const bool standaloneGeneratedBox =
        handIndex == 2 &&
        asset.sourcePath.rfind("generated:magazine_box:", 0) == 0;
    const bool opaqueStandaloneMagazine = handIndex == 2 && !standaloneGeneratedBox;
    const bool opaqueStandaloneDebugBox = handIndex == 2 && standaloneGeneratedBox;
    const bool opaqueStandaloneMesh = opaqueStandaloneMagazine || opaqueStandaloneDebugBox;

    // VR gloves use a lightweight directional-light approximation. Reusing that
    // approximation for a replacement magazine made the same exported texture
    // visibly darker than Blender and the Source viewmodel material. Keep the
    // standalone magazine opaque and sample its base-color texture at full
    // intensity. Exact Source-material parity still depends on exporting the same
    // skin texture used by the active weapon replacement.
    worldNormalRows.v[11] = opaqueStandaloneMesh
        ? 1.0f
        : std::clamp(sceneLightScale, 0.06f, 1.25f);
    const float gloveLight[4] = { 0.35f, -0.45f, -0.82f, 0.14f };
    const float magazineUnlit[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float* light = opaqueStandaloneMesh ? magazineUnlit : gloveLight;
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    // The final color pass must write depth as well. Otherwise every triangle of the
    // same glove blends through the others, so folded fingers remain visible through
    // the palm. The standalone magazine skips the VR-hand stencil test because it is
    // a viewmodel component, but it still uses the full world-comparable depth range.
    const bool writeDepth = drawPass == VrHandDrawPass::WorldDepth || viewmodelDepthPass;
    device->SetRenderState(D3DRS_ZWRITEENABLE, writeDepth ? TRUE : FALSE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, standaloneGeneratedBox ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    // Detached magazine meshes need opaque rendering because exported materials may
    // carry an unused zero alpha channel. Generated debug boxes render as wireframe
    // so they do not hide the weapon while calibrating.
    // Some exported materials keep an unused zero alpha channel, which made a successfully
    // loaded GLB invisible. Hands retain normal alpha blending; standalone helpers render opaque.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, opaqueStandaloneMesh ? FALSE : TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        maskPass ? 0u : (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA));
    device->SetRenderState(D3DRS_STENCILENABLE, (maskPass || compositePass) ? TRUE : FALSE);
    device->SetRenderState(D3DRS_STENCILFUNC, maskPass ? D3DCMP_ALWAYS : D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILREF, kVrHandOcclusionStencilBit);
    device->SetRenderState(D3DRS_STENCILMASK, kVrHandOcclusionStencilBit);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, maskPass ? kVrHandOcclusionStencilBit : 0u);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILPASS, maskPass ? D3DSTENCILOP_REPLACE : D3DSTENCILOP_KEEP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetFVF(0);
    device->SetVertexDeclaration(m_VertexDeclaration);
    device->SetVertexShader(m_VertexShader);
    device->SetPixelShader(m_PixelShader);
    device->SetTexture(0, mesh.texture);
    device->SetStreamSource(0, mesh.vertexBuffer, 0, sizeof(VrHandVertex));
    device->SetIndices(mesh.indexBuffer);
    device->SetVertexShaderConstantF(0, wvpRows.data(), 4);
    device->SetVertexShaderConstantF(4, light, 1);
    device->SetVertexShaderConstantF(5, worldNormalRows.v.data(), 3);
    device->SetVertexShaderConstantF(kBoneConstantStart, palette.front().v.data(), static_cast<UINT>(palette.size() * 3u));

    D3DVIEWPORT9 oldViewport{};
    const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));
    if (haveOldViewport)
    {
        D3DVIEWPORT9 handViewport = oldViewport;
        handViewport.MinZ = kWorldDepthRangeMin;
        handViewport.MaxZ = viewmodelDepthPass ? kViewmodelDepthRangeMax : kWorldDepthRangeMax;
        device->SetViewport(&handViewport);
    }

    HRESULT drawResult = device->DrawIndexedPrimitive(
        D3DPT_TRIANGLELIST,
        0,
        0,
        mesh.vertexCount,
        0,
        mesh.indexCount / 3u);

    if (SUCCEEDED(drawResult) && standaloneGeneratedBox && !asset.vertices.empty())
    {
        Vector mins(
            asset.vertices.front().position[0],
            asset.vertices.front().position[1],
            asset.vertices.front().position[2]);
        Vector maxs = mins;
        for (const VrHandVertex& vertex : asset.vertices)
        {
            mins.x = std::min(mins.x, vertex.position[0]);
            mins.y = std::min(mins.y, vertex.position[1]);
            mins.z = std::min(mins.z, vertex.position[2]);
            maxs.x = std::max(maxs.x, vertex.position[0]);
            maxs.y = std::max(maxs.y, vertex.position[1]);
            maxs.z = std::max(maxs.z, vertex.position[2]);
        }

        const Vector center = (mins + maxs) * 0.5f;
        const Vector span = maxs - mins;
        const float markerX = std::max(0.025f, span.x * 0.12f);
        const float markerY = std::max(0.025f, span.y * 0.12f);
        const float markerZ = std::max(0.025f, span.z * 0.12f);
        VrHandVertex marker[12]{};
        int markerVertexCount = 0;
        auto addMarkerVertex = [&](const Vector& position)
        {
            VrHandVertex& vertex = marker[markerVertexCount++];
            vertex.position[0] = position.x;
            vertex.position[1] = position.y;
            vertex.position[2] = position.z;
            vertex.normal[2] = 1.0f;
            vertex.weights[0] = 1.0f;
            vertex.joints[0] = 0;
        };
        auto addMarkerLine = [&](const Vector& a, const Vector& b)
        {
            addMarkerVertex(a);
            addMarkerVertex(b);
        };
        addMarkerLine(
            center + Vector(-markerX, -markerY, 0.0f),
            center + Vector(markerX, markerY, 0.0f));
        addMarkerLine(
            center + Vector(-markerX, markerY, 0.0f),
            center + Vector(markerX, -markerY, 0.0f));
        addMarkerLine(
            center + Vector(-markerX, 0.0f, -markerZ),
            center + Vector(markerX, 0.0f, markerZ));
        addMarkerLine(
            center + Vector(-markerX, 0.0f, markerZ),
            center + Vector(markerX, 0.0f, -markerZ));
        addMarkerLine(
            center + Vector(0.0f, -markerY, -markerZ),
            center + Vector(0.0f, markerY, markerZ));
        addMarkerLine(
            center + Vector(0.0f, -markerY, markerZ),
            center + Vector(0.0f, markerY, -markerZ));

        const HRESULT markerResult = device->DrawPrimitiveUP(
            D3DPT_LINELIST,
            static_cast<UINT>(markerVertexCount / 2),
            marker,
            sizeof(VrHandVertex));
        if (FAILED(markerResult))
            drawResult = markerResult;
    }

    if (haveOldViewport)
        device->SetViewport(&oldViewport);

    stateBlock->Apply();
    SafeRelease(stateBlock);
    if (FAILED(drawResult))
    {
        outError = "DrawIndexedPrimitive failed for VR hands";
        return false;
    }
    return true;
}

void VrHandRendererD3D9::ReleaseMesh(MeshResources& mesh)
{
    SafeRelease(mesh.vertexBuffer);
    SafeRelease(mesh.indexBuffer);
    SafeRelease(mesh.texture);
    mesh.sourcePath.clear();
    mesh.vertexCount = 0;
    mesh.indexCount = 0;
}

void VrHandRendererD3D9::ReleaseShared()
{
    SafeRelease(m_VertexDeclaration);
    SafeRelease(m_VertexShader);
    SafeRelease(m_PixelShader);
}

void VrHandRendererD3D9::OnDeviceLost()
{
    for (MeshResources& mesh : m_Meshes)
        ReleaseMesh(mesh);
    ReleaseShared();
    SafeRelease(m_Device);
}
