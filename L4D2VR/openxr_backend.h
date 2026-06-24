#pragma once

#include "../thirdparty/openxr/include/openxr/openxr.h"

#include <cstdint>
#include <string>
#include <vector>

struct D3D9_OPENXR_GRAPHICS_BINDING_DESC;

class OpenXrBackend
{
public:
    OpenXrBackend() = default;
    ~OpenXrBackend();

    OpenXrBackend(const OpenXrBackend&) = delete;
    OpenXrBackend& operator=(const OpenXrBackend&) = delete;

    bool InitializeSession(
        const D3D9_OPENXR_GRAPHICS_BINDING_DESC& graphics,
        std::string* detail = nullptr);
    void Shutdown();

    bool IsSessionReady() const { return m_SessionReady; }
    const char* RuntimeName() const { return m_RuntimeName; }
    uint32_t RuntimeVersionMajor() const { return m_RuntimeVersionMajor; }
    uint32_t RuntimeVersionMinor() const { return m_RuntimeVersionMinor; }
    uint32_t RuntimeVersionPatch() const { return m_RuntimeVersionPatch; }
    uint32_t RecommendedEyeWidth() const { return m_RecommendedEyeWidth; }
    uint32_t RecommendedEyeHeight() const { return m_RecommendedEyeHeight; }
    uint32_t SwapchainFormatCount() const { return static_cast<uint32_t>(m_SwapchainFormats.size()); }
    bool SupportsSwapchainFormat(int64_t format) const;

private:
    void* m_Loader = nullptr;
    PFN_xrGetInstanceProcAddr m_xrGetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance m_xrDestroyInstance = nullptr;
    PFN_xrDestroySession m_xrDestroySession = nullptr;
    PFN_xrDestroySpace m_xrDestroySpace = nullptr;

    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
    XrSession m_Session = XR_NULL_HANDLE;
    XrSpace m_AppSpace = XR_NULL_HANDLE;
    bool m_SessionReady = false;

    char m_RuntimeName[XR_MAX_RUNTIME_NAME_SIZE] = {};
    uint32_t m_RuntimeVersionMajor = 0;
    uint32_t m_RuntimeVersionMinor = 0;
    uint32_t m_RuntimeVersionPatch = 0;
    uint32_t m_RecommendedEyeWidth = 0;
    uint32_t m_RecommendedEyeHeight = 0;
    std::vector<int64_t> m_SwapchainFormats;
};
