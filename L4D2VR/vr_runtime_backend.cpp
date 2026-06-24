#include "vr_runtime_backend.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "../thirdparty/openxr/include/openxr/openxr.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr bool kNativeOpenXrBackendImplemented = false;

    std::string NormalizeBackendName(std::string value)
    {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '_'), value.end());
        value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
        return value;
    }

    const char* XrResultName(XrResult result)
    {
        switch (result)
        {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
        case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
        case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
        case XR_SPACE_BOUNDS_UNAVAILABLE: return "XR_SPACE_BOUNDS_UNAVAILABLE";
        case XR_SESSION_NOT_FOCUSED: return "XR_SESSION_NOT_FOCUSED";
        case XR_FRAME_DISCARDED: return "XR_FRAME_DISCARDED";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
        case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
        case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
        case XR_ERROR_PATH_INVALID: return "XR_ERROR_PATH_INVALID";
        case XR_ERROR_PATH_COUNT_EXCEEDED: return "XR_ERROR_PATH_COUNT_EXCEEDED";
        case XR_ERROR_PATH_FORMAT_INVALID: return "XR_ERROR_PATH_FORMAT_INVALID";
        case XR_ERROR_PATH_UNSUPPORTED: return "XR_ERROR_PATH_UNSUPPORTED";
        case XR_ERROR_LAYER_INVALID: return "XR_ERROR_LAYER_INVALID";
        case XR_ERROR_LAYER_LIMIT_EXCEEDED: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
        case XR_ERROR_SWAPCHAIN_RECT_INVALID: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_ERROR_ACTION_TYPE_MISMATCH: return "XR_ERROR_ACTION_TYPE_MISMATCH";
        case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
        case XR_ERROR_SESSION_NOT_STOPPING: return "XR_ERROR_SESSION_NOT_STOPPING";
        case XR_ERROR_TIME_INVALID: return "XR_ERROR_TIME_INVALID";
        case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
        case XR_ERROR_FILE_ACCESS_ERROR: return "XR_ERROR_FILE_ACCESS_ERROR";
        case XR_ERROR_FILE_CONTENTS_INVALID: return "XR_ERROR_FILE_CONTENTS_INVALID";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
        case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_POSE_INVALID: return "XR_ERROR_POSE_INVALID";
        case XR_ERROR_INDEX_OUT_OF_RANGE: return "XR_ERROR_INDEX_OUT_OF_RANGE";
        case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
        case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
        case XR_ERROR_NAME_DUPLICATED: return "XR_ERROR_NAME_DUPLICATED";
        case XR_ERROR_NAME_INVALID: return "XR_ERROR_NAME_INVALID";
        case XR_ERROR_ACTIONSET_NOT_ATTACHED: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
        case XR_ERROR_ACTIONSETS_ALREADY_ATTACHED: return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
        case XR_ERROR_LOCALIZED_NAME_DUPLICATED: return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
        case XR_ERROR_LOCALIZED_NAME_INVALID: return "XR_ERROR_LOCALIZED_NAME_INVALID";
        case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        default: return "XR_UNKNOWN_RESULT";
        }
    }

    std::string FormatXrResult(const char* action, XrResult result)
    {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "%s failed: %s (%d)",
            action ? action : "OpenXR call",
            XrResultName(result),
            static_cast<int>(result));
        return buffer;
    }

    template <typename T>
    bool LoadXrFunction(
        PFN_xrGetInstanceProcAddr getInstanceProcAddr,
        XrInstance instance,
        const char* name,
        T& out,
        std::string& detail)
    {
        PFN_xrVoidFunction function = nullptr;
        const XrResult result = getInstanceProcAddr(instance, name, &function);
        if (XR_FAILED(result) || !function)
        {
            detail = FormatXrResult(name, result);
            return false;
        }

        out = reinterpret_cast<T>(function);
        return true;
    }
}

VrRuntimeBackend L4D2VR_ParseRuntimeBackend(const std::string& value, VrRuntimeBackend fallback)
{
    const std::string normalized = NormalizeBackendName(value);
    if (normalized == "openxr" || normalized == "xr")
        return VrRuntimeBackend::OpenXR;
    if (normalized == "openvr" || normalized == "steamvr" || normalized == "vr")
        return VrRuntimeBackend::OpenVR;
    return fallback;
}

const char* L4D2VR_RuntimeBackendName(VrRuntimeBackend backend)
{
    switch (backend)
    {
    case VrRuntimeBackend::OpenXR:
        return "openxr";
    case VrRuntimeBackend::OpenVR:
    default:
        return "openvr";
    }
}

bool L4D2VR_IsOpenXrLoaderAvailable(std::string* detail)
{
    HMODULE loader = LoadLibraryA("openxr_loader.dll");
    if (!loader)
    {
        if (detail)
            *detail = "openxr_loader.dll was not found";
        return false;
    }

    const bool hasEntryPoint = GetProcAddress(loader, "xrGetInstanceProcAddr") != nullptr;
    FreeLibrary(loader);

    if (!hasEntryPoint)
    {
        if (detail)
            *detail = "openxr_loader.dll does not export xrGetInstanceProcAddr";
        return false;
    }

    if (detail)
        *detail = "openxr_loader.dll is present";
    return true;
}

OpenXrRuntimeProbe L4D2VR_ProbeOpenXrRuntime()
{
    OpenXrRuntimeProbe probe{};

    HMODULE loader = LoadLibraryA("openxr_loader.dll");
    if (!loader)
    {
        probe.detail = "openxr_loader.dll was not found";
        return probe;
    }
    probe.loaderAvailable = true;

    auto getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!getInstanceProcAddr)
    {
        probe.detail = "openxr_loader.dll does not export xrGetInstanceProcAddr";
        FreeLibrary(loader);
        return probe;
    }

    std::string detail;
    PFN_xrCreateInstance xrCreateInstanceFn = nullptr;
    PFN_xrDestroyInstance xrDestroyInstanceFn = nullptr;
    PFN_xrGetSystem xrGetSystemFn = nullptr;
    PFN_xrGetInstanceProperties xrGetInstancePropertiesFn = nullptr;
    PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViewsFn = nullptr;

    if (!LoadXrFunction(getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstanceFn, detail))
    {
        probe.detail = detail;
        FreeLibrary(loader);
        return probe;
    }

    XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "L4D2VR");
    createInfo.applicationInfo.applicationVersion = 1;
    std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Source/DXVK");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstanceFn(&createInfo, &instance);
    if (XR_FAILED(result) || instance == XR_NULL_HANDLE)
    {
        probe.detail = FormatXrResult("xrCreateInstance", result);
        FreeLibrary(loader);
        return probe;
    }
    probe.instanceCreated = true;

    auto cleanup = [&]()
        {
            if (xrDestroyInstanceFn && instance != XR_NULL_HANDLE)
                xrDestroyInstanceFn(instance);
            FreeLibrary(loader);
        };

    if (!LoadXrFunction(getInstanceProcAddr, instance, "xrDestroyInstance", xrDestroyInstanceFn, detail) ||
        !LoadXrFunction(getInstanceProcAddr, instance, "xrGetSystem", xrGetSystemFn, detail) ||
        !LoadXrFunction(getInstanceProcAddr, instance, "xrGetInstanceProperties", xrGetInstancePropertiesFn, detail) ||
        !LoadXrFunction(getInstanceProcAddr, instance, "xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViewsFn, detail))
    {
        probe.detail = detail;
        cleanup();
        return probe;
    }

    XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
    result = xrGetInstancePropertiesFn(instance, &instanceProperties);
    if (XR_SUCCEEDED(result))
    {
        std::snprintf(probe.runtimeName, sizeof(probe.runtimeName), "%s", instanceProperties.runtimeName);
        probe.runtimeVersionMajor = XR_VERSION_MAJOR(instanceProperties.runtimeVersion);
        probe.runtimeVersionMinor = XR_VERSION_MINOR(instanceProperties.runtimeVersion);
        probe.runtimeVersionPatch = XR_VERSION_PATCH(instanceProperties.runtimeVersion);
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    result = xrGetSystemFn(instance, &systemInfo, &systemId);
    if (XR_FAILED(result) || systemId == XR_NULL_SYSTEM_ID)
    {
        probe.detail = FormatXrResult("xrGetSystem(HMD)", result);
        cleanup();
        return probe;
    }
    probe.systemAvailable = true;

    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViewsFn(
        instance,
        systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &viewCount,
        nullptr);
    if (XR_FAILED(result) || viewCount == 0)
    {
        probe.detail = XR_FAILED(result)
            ? FormatXrResult("xrEnumerateViewConfigurationViews(count)", result)
            : "xrEnumerateViewConfigurationViews returned zero primary stereo views";
        cleanup();
        return probe;
    }

    std::vector<XrViewConfigurationView> views(viewCount);
    for (auto& view : views)
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    result = xrEnumerateViewConfigurationViewsFn(
        instance,
        systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount,
        &viewCount,
        views.data());
    if (XR_FAILED(result) || viewCount == 0)
    {
        probe.detail = XR_FAILED(result)
            ? FormatXrResult("xrEnumerateViewConfigurationViews(data)", result)
            : "xrEnumerateViewConfigurationViews returned no view data";
        cleanup();
        return probe;
    }

    probe.stereoViewsAvailable = true;
    probe.stereoViewCount = viewCount;
    probe.recommendedWidth = views[0].recommendedImageRectWidth;
    probe.recommendedHeight = views[0].recommendedImageRectHeight;
    probe.detail = "OpenXR runtime probe succeeded";

    cleanup();
    return probe;
}

VrRuntimeBackendConfig L4D2VR_ReadRuntimeBackendConfig()
{
    VrRuntimeBackendConfig config{};

    auto trim = [](std::string& s)
        {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char ch) { return !std::isspace(ch); }));
            s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };

    std::unordered_map<std::string, std::string> values;
    auto parsePath = [&](const char* path)
        {
            std::ifstream configStream(path);
            if (!configStream)
                return;

            std::string line;
            while (std::getline(configStream, line))
            {
                size_t cut = std::string::npos;
                const size_t p1 = line.find("//");
                const size_t p2 = line.find('#');
                const size_t p3 = line.find(';');
                if (p1 != std::string::npos) cut = p1;
                if (p2 != std::string::npos) cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
                if (p3 != std::string::npos) cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
                if (cut != std::string::npos)
                    line.erase(cut);

                trim(line);
                if (line.empty())
                    continue;

                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                trim(key);
                trim(value);
                if (key.size() >= 3 &&
                    static_cast<unsigned char>(key[0]) == 0xEF &&
                    static_cast<unsigned char>(key[1]) == 0xBB &&
                    static_cast<unsigned char>(key[2]) == 0xBF)
                {
                    key.erase(0, 3);
                    trim(key);
                }

                if (!key.empty())
                    values[key] = value;
            }
        };

    parsePath("VR\\config.txt");
    parsePath("VR\\config2.txt");

    auto backendIt = values.find("VRRuntimeBackend");
    if (backendIt != values.end() && !backendIt->second.empty())
        config.requestedBackend = backendIt->second;

    auto fallbackIt = values.find("VRRuntimeFallbackToOpenVR");
    if (fallbackIt != values.end())
    {
        std::string value = fallbackIt->second;
        trim(value);
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (value == "1" || value == "true" || value == "on" || value == "yes")
            config.fallbackToOpenVR = true;
        else if (value == "0" || value == "false" || value == "off" || value == "no")
            config.fallbackToOpenVR = false;
    }

    return config;
}

VrRuntimeBackendSelection L4D2VR_SelectRuntimeBackend(
    const std::string& requestedBackend,
    bool fallbackToOpenVR)
{
    VrRuntimeBackendSelection result{};
    result.requested = L4D2VR_ParseRuntimeBackend(requestedBackend, VrRuntimeBackend::OpenVR);
    result.fallbackToOpenVR = fallbackToOpenVR;
    result.openXrBackendImplemented = kNativeOpenXrBackendImplemented;

    if (result.requested == VrRuntimeBackend::OpenVR)
    {
        result.active = VrRuntimeBackend::OpenVR;
        result.canStart = true;
        result.message = "OpenVR runtime selected";
        return result;
    }

    const OpenXrRuntimeProbe openXrProbe = L4D2VR_ProbeOpenXrRuntime();
    result.openXrLoaderAvailable = openXrProbe.loaderAvailable;

    if (!result.openXrLoaderAvailable)
    {
        if (fallbackToOpenVR)
        {
            result.active = VrRuntimeBackend::OpenVR;
            result.usedFallback = true;
            result.canStart = true;
            result.message = "OpenXR requested, but " + openXrProbe.detail + "; falling back to OpenVR";
        }
        else
        {
            result.active = VrRuntimeBackend::OpenXR;
            result.canStart = false;
            result.message = "OpenXR requested, but " + openXrProbe.detail;
        }
        return result;
    }

    if (!openXrProbe.instanceCreated || !openXrProbe.systemAvailable || !openXrProbe.stereoViewsAvailable)
    {
        if (fallbackToOpenVR)
        {
            result.active = VrRuntimeBackend::OpenVR;
            result.usedFallback = true;
            result.canStart = true;
            result.message = "OpenXR requested, but runtime probe failed: " + openXrProbe.detail + "; falling back to OpenVR";
        }
        else
        {
            result.active = VrRuntimeBackend::OpenXR;
            result.canStart = false;
            result.message = "OpenXR requested, but runtime probe failed: " + openXrProbe.detail;
        }
        return result;
    }

    if (!kNativeOpenXrBackendImplemented)
    {
        if (fallbackToOpenVR)
        {
            result.active = VrRuntimeBackend::OpenVR;
            result.usedFallback = true;
            result.canStart = true;
            result.message = "OpenXR runtime probe succeeded";
            if (openXrProbe.runtimeName[0])
            {
                result.message += " (runtime=";
                result.message += openXrProbe.runtimeName;
                result.message += ", version=" + std::to_string(openXrProbe.runtimeVersionMajor)
                    + "." + std::to_string(openXrProbe.runtimeVersionMinor)
                    + "." + std::to_string(openXrProbe.runtimeVersionPatch);
                result.message += ", stereoViews=" + std::to_string(openXrProbe.stereoViewCount);
                result.message += ", recommendedEye="
                    + std::to_string(openXrProbe.recommendedWidth)
                    + "x"
                    + std::to_string(openXrProbe.recommendedHeight)
                    + ")";
            }
            result.message += ", but native OpenXR rendering/input is not implemented yet; falling back to OpenVR";
        }
        else
        {
            result.active = VrRuntimeBackend::OpenXR;
            result.canStart = false;
            result.message = "OpenXR requested and loader is available, but native OpenXR rendering/input is not implemented yet";
        }
        return result;
    }

    result.active = VrRuntimeBackend::OpenXR;
    result.canStart = true;
    result.message = "OpenXR runtime selected";
    return result;
}
