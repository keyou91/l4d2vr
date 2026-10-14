#pragma once

#include "vr_hand_types.h"

#include <array>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DVertexDeclaration9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct IDirect3DTexture9;
namespace l4d2vr_input { class ActionFrame; }

enum class VrHandDrawPass
{
    WorldDepth,
    WorldVisibilityMask,
    ViewmodelComposite,
    // Standalone magazine anchored to Source viewmodel bones. It uses the
    // viewmodel depth range but must not depend on the VR-hand stencil mask.
    ViewmodelStandalone
};

class VrHandRendererD3D9
{
public:
    VrHandRendererD3D9();
    ~VrHandRendererD3D9();

    VrHandRendererD3D9(const VrHandRendererD3D9&) = delete;
    VrHandRendererD3D9& operator=(const VrHandRendererD3D9&) = delete;

    bool Draw(
        IDirect3DDevice9* device,
        int handIndex,
        const VrHandMeshAsset& asset,
        const std::vector<VrHandMatrixRows3x4>& palette,
        const VrHandMatrix4& world,
        const VrHandMatrix4& worldViewProjection,
        VrHandDrawPass drawPass,
        float sceneLightScale,
        std::string& outError,
        const l4d2vr_input::ActionFrame* actionFrame = nullptr,
        std::uint64_t paletteFrameToken = 0u,
        std::uint32_t paletteUpdatedAt = 0u);

    bool ClearViewmodelOcclusionStencil(IDirect3DDevice9* device, std::string& outError);
    void OnDeviceLost();

private:
    struct MeshResources
    {
        IDirect3DVertexBuffer9* vertexBuffer = nullptr;
        IDirect3DIndexBuffer9* indexBuffer = nullptr;
        IDirect3DTexture9* texture = nullptr;
        std::string sourcePath;
        unsigned int vertexCount = 0;
        unsigned int indexCount = 0;
    };

    bool EnsureSharedResources(IDirect3DDevice9* device, std::string& outError);
    bool EnsureMeshResources(IDirect3DDevice9* device, int handIndex, const VrHandMeshAsset& asset, std::string& outError);
    bool CreateTexture(IDirect3DDevice9* device, const VrHandMeshAsset& asset, IDirect3DTexture9** outTexture, std::string& outError);
    void ReleaseMesh(MeshResources& mesh);
    void ReleaseShared();

    IDirect3DDevice9* m_Device = nullptr;
    IDirect3DVertexDeclaration9* m_VertexDeclaration = nullptr;
    IDirect3DVertexShader9* m_VertexShader = nullptr;
    IDirect3DPixelShader9* m_PixelShader = nullptr;
    std::array<MeshResources, 3> m_Meshes{};
};
