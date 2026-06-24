#pragma once

#include <cstdint>
#include <string>

enum class VrRuntimeBackend
{
    OpenVR,
    OpenXR
};

struct VrRuntimeBackendSelection
{
    VrRuntimeBackend requested = VrRuntimeBackend::OpenVR;
    VrRuntimeBackend active = VrRuntimeBackend::OpenVR;
    bool fallbackToOpenVR = false;
    bool usedFallback = false;
    bool canStart = true;
    bool openXrLoaderAvailable = false;
    bool openXrBackendImplemented = false;
    std::string message;
};

struct VrRuntimeBackendConfig
{
    std::string requestedBackend = "openvr";
    bool fallbackToOpenVR = false;
};

struct OpenXrRuntimeProbe
{
    bool loaderAvailable = false;
    bool instanceCreated = false;
    bool systemAvailable = false;
    bool stereoViewsAvailable = false;
    char runtimeName[128] = {};
    uint32_t runtimeVersionMajor = 0;
    uint32_t runtimeVersionMinor = 0;
    uint32_t runtimeVersionPatch = 0;
    uint32_t stereoViewCount = 0;
    uint32_t recommendedWidth = 0;
    uint32_t recommendedHeight = 0;
    std::string loaderPath;
    std::string detail;
};

VrRuntimeBackend L4D2VR_ParseRuntimeBackend(const std::string& value, VrRuntimeBackend fallback);
const char* L4D2VR_RuntimeBackendName(VrRuntimeBackend backend);
bool L4D2VR_IsOpenXrLoaderAvailable(std::string* detail = nullptr);
VrRuntimeBackendConfig L4D2VR_ReadRuntimeBackendConfig();
OpenXrRuntimeProbe L4D2VR_ProbeOpenXrRuntime();
VrRuntimeBackendSelection L4D2VR_SelectRuntimeBackend(
    const std::string& requestedBackend,
    bool fallbackToOpenVR);
