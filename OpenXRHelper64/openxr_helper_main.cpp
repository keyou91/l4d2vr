#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_VULKAN

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "../dxvk_new/include/vulkan/include/vulkan/vulkan.h"
#include "../thirdparty/openxr/include/openxr/openxr.h"
#include "../thirdparty/openxr/include/openxr/openxr_platform.h"
#include "../L4D2VR/openxr_bridge_protocol.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <share.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr uint32_t kDefaultTargetFrames = 180;
    constexpr uint32_t kDefaultWaitReadySeconds = 45;
    constexpr uint32_t kBlitDescriptorSetCount = L4D2VR_OPENXR_EYE_COUNT + L4D2VR_OPENXR_OVERLAY_COUNT;
    constexpr uint32_t kMaxOpenXrHudCurveSegments = 12;
    constexpr uint32_t kMaxOpenXrOverlayLayers = L4D2VR_OPENXR_OVERLAY_COUNT + kMaxOpenXrHudCurveSegments - 1;
    constexpr float kPi = 3.141592654f;

    const char* EyeName(uint32_t eyeIndex)
    {
        switch (eyeIndex)
        {
        case L4D2VR_OPENXR_EYE_LEFT: return "left";
        case L4D2VR_OPENXR_EYE_RIGHT: return "right";
        default: return "unknown";
        }
    }

    const char* OverlayName(uint32_t overlayIndex)
    {
        switch (overlayIndex)
        {
        case L4D2VR_OPENXR_OVERLAY_MAIN_MENU: return "main_menu";
        case L4D2VR_OPENXR_OVERLAY_HUD: return "hud";
        default: return "unknown";
        }
    }

    struct Options
    {
        std::wstring logPath;
        std::wstring mappingName;
        uint32_t targetFrames = kDefaultTargetFrames;
        uint32_t waitReadySeconds = kDefaultWaitReadySeconds;
        bool swapProjectionEyes = false;
        bool swapProjectionViewOrder = false;
        bool mirrorProjectionHorizontal = false;
        bool disableQuadOverlays = false;
        bool disableProjectionLayer = false;
        bool useSymmetricProjectionFov = false;
        bool useGameRenderPoseForProjection = false;
        int forceMonoProjectionEye = -1;
        int forceMonoProjectionView = -1;
        DWORD parentPid = 0;
    };

    class Logger
    {
    public:
        bool Open(const std::wstring& path)
        {
            if (path.empty())
                return true;

            m_File = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
            return m_File != nullptr;
        }

        ~Logger()
        {
            if (m_File)
                std::fclose(m_File);
        }

        void Print(const char* fmt, ...)
        {
            SYSTEMTIME st{};
            GetLocalTime(&st);

            char message[2048] = {};
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(message, sizeof(message), fmt, args);
            va_end(args);

            char line[2300] = {};
            std::snprintf(
                line,
                sizeof(line),
                "[%04u-%02u-%02u %02u:%02u:%02u] %s\n",
                st.wYear,
                st.wMonth,
                st.wDay,
                st.wHour,
                st.wMinute,
                st.wSecond,
                message);

            OutputDebugStringA(line);
            std::fputs(line, stdout);
            std::fflush(stdout);

            if (m_File)
            {
                std::fputs(line, m_File);
                std::fflush(m_File);
            }
        }

    private:
        FILE* m_File = nullptr;
    };

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (len == 0 || len >= ARRAYSIZE(path))
            return L".";

        wchar_t* slash = std::wcsrchr(path, L'\\');
        if (!slash)
            return L".";

        *slash = L'\0';
        return path;
    }

    std::wstring DefaultLogPath()
    {
        return ExeDirectory() + L"\\openxr_helper64.log";
    }

    bool ParseUintArg(const wchar_t* value, uint32_t& out)
    {
        if (!value || !*value)
            return false;

        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(value, &end, 10);
        if (!end || *end != L'\0')
            return false;

        out = static_cast<uint32_t>(parsed);
        return true;
    }

    bool ParseProjectionEyeArg(const wchar_t* value, int& out)
    {
        if (!value || !*value)
            return false;

        if (_wcsicmp(value, L"none") == 0 ||
            _wcsicmp(value, L"off") == 0 ||
            _wcsicmp(value, L"disabled") == 0 ||
            std::wcscmp(value, L"-1") == 0)
        {
            out = -1;
            return true;
        }

        if (_wcsicmp(value, L"left") == 0 || _wcsicmp(value, L"l") == 0)
        {
            out = static_cast<int>(L4D2VR_OPENXR_EYE_LEFT);
            return true;
        }

        if (_wcsicmp(value, L"right") == 0 || _wcsicmp(value, L"r") == 0)
        {
            out = static_cast<int>(L4D2VR_OPENXR_EYE_RIGHT);
            return true;
        }

        uint32_t parsed = 0;
        if (ParseUintArg(value, parsed) && parsed <= L4D2VR_OPENXR_EYE_RIGHT)
        {
            out = static_cast<int>(parsed);
            return true;
        }

        return false;
    }

    const char* ForceMonoProjectionEyeName(int eye)
    {
        if (eye == static_cast<int>(L4D2VR_OPENXR_EYE_LEFT))
            return "left";
        if (eye == static_cast<int>(L4D2VR_OPENXR_EYE_RIGHT))
            return "right";
        return "none";
    }

    uint32_t SelectProjectionImageEye(const Options& options, uint32_t viewEye)
    {
        if (options.forceMonoProjectionEye == static_cast<int>(L4D2VR_OPENXR_EYE_LEFT) ||
            options.forceMonoProjectionEye == static_cast<int>(L4D2VR_OPENXR_EYE_RIGHT))
        {
            return static_cast<uint32_t>(options.forceMonoProjectionEye);
        }

        return options.swapProjectionEyes ? (viewEye ^ 1u) : viewEye;
    }

    uint32_t SelectProjectionViewEye(const Options& options, uint32_t viewEye)
    {
        if (options.forceMonoProjectionView == static_cast<int>(L4D2VR_OPENXR_EYE_LEFT) ||
            options.forceMonoProjectionView == static_cast<int>(L4D2VR_OPENXR_EYE_RIGHT))
        {
            return static_cast<uint32_t>(options.forceMonoProjectionView);
        }

        return viewEye;
    }

    Options ParseOptions(int argc, wchar_t** argv)
    {
        Options options{};
        options.logPath = DefaultLogPath();

        for (int i = 1; i < argc; ++i)
        {
            const wchar_t* arg = argv[i];
            if (!arg)
                continue;

            auto needsValue = [&](const wchar_t* name) -> const wchar_t*
                {
                    if (_wcsicmp(arg, name) != 0 || i + 1 >= argc)
                        return nullptr;
                    return argv[++i];
                };

            if (const wchar_t* value = needsValue(L"--log"))
            {
                options.logPath = value;
            }
            else if (const wchar_t* value = needsValue(L"--mapping"))
            {
                options.mappingName = value;
            }
            else if (const wchar_t* value = needsValue(L"--frames"))
            {
                ParseUintArg(value, options.targetFrames);
            }
            else if (const wchar_t* value = needsValue(L"--wait-ready-sec"))
            {
                ParseUintArg(value, options.waitReadySeconds);
            }
            else if (const wchar_t* value = needsValue(L"--swap-projection-eyes"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.swapProjectionEyes = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--swap-projection-view-order"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.swapProjectionViewOrder = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--mirror-projection-horizontal"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.mirrorProjectionHorizontal = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--disable-quad-overlays"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.disableQuadOverlays = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--disable-projection-layer"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.disableProjectionLayer = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--use-symmetric-projection-fov"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.useSymmetricProjectionFov = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--use-game-render-pose-for-projection"))
            {
                uint32_t enabled = 0;
                if (ParseUintArg(value, enabled))
                    options.useGameRenderPoseForProjection = enabled != 0;
            }
            else if (const wchar_t* value = needsValue(L"--force-mono-projection-eye"))
            {
                ParseProjectionEyeArg(value, options.forceMonoProjectionEye);
            }
            else if (const wchar_t* value = needsValue(L"--force-mono-projection-view"))
            {
                ParseProjectionEyeArg(value, options.forceMonoProjectionView);
            }
            else if (const wchar_t* value = needsValue(L"--parent"))
            {
                uint32_t pid = 0;
                if (ParseUintArg(value, pid))
                    options.parentPid = static_cast<DWORD>(pid);
            }
        }

        return options;
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

    bool Succeeded(Logger& log, const char* action, XrResult result)
    {
        if (XR_SUCCEEDED(result))
            return true;

        log.Print("%s failed: %s (%d)", action, XrResultName(result), static_cast<int>(result));
        return false;
    }

    bool IsValidRuntimeFov(const XrFovf& fov)
    {
        return std::isfinite(fov.angleLeft) &&
            std::isfinite(fov.angleRight) &&
            std::isfinite(fov.angleUp) &&
            std::isfinite(fov.angleDown) &&
            fov.angleLeft < -0.001f &&
            fov.angleRight > 0.001f &&
            fov.angleUp > 0.001f &&
            fov.angleDown < -0.001f;
    }

    L4D2VROpenXrRuntimeViewConfigDesc BuildRuntimeViewConfig(
        const std::vector<XrViewConfigurationView>& viewConfigs,
        const std::vector<XrView>& locatedViews,
        uint32_t locatedCount)
    {
        L4D2VROpenXrRuntimeViewConfigDesc config{};
        if (viewConfigs.size() < L4D2VR_OPENXR_EYE_COUNT ||
            locatedViews.size() < L4D2VR_OPENXR_EYE_COUNT ||
            locatedCount < L4D2VR_OPENXR_EYE_COUNT)
        {
            return config;
        }

        config.viewCount = L4D2VR_OPENXR_EYE_COUNT;
        for (uint32_t eye = 0; eye < L4D2VR_OPENXR_EYE_COUNT; ++eye)
        {
            const XrViewConfigurationView& viewConfig = viewConfigs[eye];
            const XrView& locatedView = locatedViews[eye];
            L4D2VROpenXrRuntimeViewDesc& out = config.views[eye];
            out.width = viewConfig.recommendedImageRectWidth;
            out.height = viewConfig.recommendedImageRectHeight;
            out.recommendedSampleCount = viewConfig.recommendedSwapchainSampleCount;
            out.angleLeft = locatedView.fov.angleLeft;
            out.angleRight = locatedView.fov.angleRight;
            out.angleUp = locatedView.fov.angleUp;
            out.angleDown = locatedView.fov.angleDown;
            out.valid =
                out.width > 0 &&
                out.height > 0 &&
                IsValidRuntimeFov(locatedView.fov)
                    ? 1u
                    : 0u;
        }

        config.valid =
            config.views[L4D2VR_OPENXR_EYE_LEFT].valid &&
            config.views[L4D2VR_OPENXR_EYE_RIGHT].valid
                ? 1u
                : 0u;
        return config;
    }

    bool SameLuid(const LUID& a, const LUID& b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }

    std::string Narrow(const std::wstring& value)
    {
        if (value.empty())
            return std::string();

        const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1)
            return std::string();

        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), len, nullptr, nullptr);
        return out;
    }

    template <typename T>
    bool LoadXrFunction(PFN_xrGetInstanceProcAddr getInstanceProcAddr, XrInstance instance, const char* name, T& out, Logger& log)
    {
        PFN_xrVoidFunction function = nullptr;
        const XrResult result = getInstanceProcAddr(instance, name, &function);
        if (XR_FAILED(result) || !function)
        {
            log.Print("xrGetInstanceProcAddr(%s) failed: %s (%d)", name, XrResultName(result), static_cast<int>(result));
            return false;
        }

        out = reinterpret_cast<T>(function);
        return true;
    }

    struct XrDispatch
    {
        PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
        PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
        PFN_xrCreateInstance xrCreateInstance = nullptr;
        PFN_xrDestroyInstance xrDestroyInstance = nullptr;
        PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
        PFN_xrGetSystem xrGetSystem = nullptr;
        PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = nullptr;
        PFN_xrEnumerateEnvironmentBlendModes xrEnumerateEnvironmentBlendModes = nullptr;
        PFN_xrCreateSession xrCreateSession = nullptr;
        PFN_xrDestroySession xrDestroySession = nullptr;
        PFN_xrStringToPath xrStringToPath = nullptr;
        PFN_xrPathToString xrPathToString = nullptr;
        PFN_xrCreateActionSet xrCreateActionSet = nullptr;
        PFN_xrDestroyActionSet xrDestroyActionSet = nullptr;
        PFN_xrCreateAction xrCreateAction = nullptr;
        PFN_xrDestroyAction xrDestroyAction = nullptr;
        PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = nullptr;
        PFN_xrGetCurrentInteractionProfile xrGetCurrentInteractionProfile = nullptr;
        PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
        PFN_xrGetActionStateBoolean xrGetActionStateBoolean = nullptr;
        PFN_xrGetActionStateFloat xrGetActionStateFloat = nullptr;
        PFN_xrGetActionStateVector2f xrGetActionStateVector2f = nullptr;
        PFN_xrGetActionStatePose xrGetActionStatePose = nullptr;
        PFN_xrSyncActions xrSyncActions = nullptr;
        PFN_xrApplyHapticFeedback xrApplyHapticFeedback = nullptr;
        PFN_xrStopHapticFeedback xrStopHapticFeedback = nullptr;
        PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
        PFN_xrDestroySpace xrDestroySpace = nullptr;
        PFN_xrCreateActionSpace xrCreateActionSpace = nullptr;
        PFN_xrLocateSpace xrLocateSpace = nullptr;
        PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
        PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
        PFN_xrDestroySwapchain xrDestroySwapchain = nullptr;
        PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
        PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
        PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
        PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
        PFN_xrPollEvent xrPollEvent = nullptr;
        PFN_xrBeginSession xrBeginSession = nullptr;
        PFN_xrEndSession xrEndSession = nullptr;
        PFN_xrWaitFrame xrWaitFrame = nullptr;
        PFN_xrBeginFrame xrBeginFrame = nullptr;
        PFN_xrEndFrame xrEndFrame = nullptr;
        PFN_xrLocateViews xrLocateViews = nullptr;
        PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = nullptr;
        PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR = nullptr;
        PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR = nullptr;
        PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR = nullptr;
        PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR = nullptr;
        PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT = nullptr;
        PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT = nullptr;
        PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT = nullptr;
    };

    struct EyeSwapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t format = DXGI_FORMAT_UNKNOWN;
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<ComPtr<ID3D11RenderTargetView>> renderTargetViews;
    };

    struct GameEyeTexture
    {
        uint32_t generation = 0;
        uint64_t kmtHandle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> shaderResourceView;
    };

    const char* VkResultName(VkResult result)
    {
        switch (result)
        {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VK_UNKNOWN_RESULT";
        }
    }

    std::vector<std::string> SplitExtensionString(const std::string& extensions)
    {
        std::vector<std::string> result;
        std::istringstream stream(extensions);
        std::string item;
        while (stream >> item)
            result.push_back(item);
        return result;
    }

    bool HasName(const std::vector<std::string>& values, const char* name)
    {
        return std::find(values.begin(), values.end(), name) != values.end();
    }

    void AddUniqueName(std::vector<std::string>& values, const char* name)
    {
        if (name && *name && !HasName(values, name))
            values.emplace_back(name);
    }

    std::vector<const char*> MakeNamePointers(const std::vector<std::string>& values)
    {
        std::vector<const char*> pointers;
        pointers.reserve(values.size());
        for (const std::string& value : values)
            pointers.push_back(value.c_str());
        return pointers;
    }

    int Base64Value(char ch)
    {
        if (ch >= 'A' && ch <= 'Z')
            return ch - 'A';
        if (ch >= 'a' && ch <= 'z')
            return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9')
            return ch - '0' + 52;
        if (ch == '+')
            return 62;
        if (ch == '/')
            return 63;
        return -1;
    }

    std::vector<uint32_t> DecodeBase64Spirv(const char* text)
    {
        std::vector<uint8_t> bytes;
        int value = 0;
        int valueBits = -8;
        for (const char* p = text; p && *p; ++p)
        {
            if (*p == '=')
                break;
            const int digit = Base64Value(*p);
            if (digit < 0)
                continue;
            value = (value << 6) | digit;
            valueBits += 6;
            if (valueBits >= 0)
            {
                bytes.push_back(static_cast<uint8_t>((value >> valueBits) & 0xFF));
                valueBits -= 8;
            }
        }

        if (bytes.empty() || (bytes.size() % sizeof(uint32_t)) != 0)
            return {};

        std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        return words;
    }

    bool IsSrgbVkFormat(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return true;
        default:
            return false;
        }
    }

    constexpr char kOpenXrBlitVertSpvBase64[] =
        "AwIjBwAAAQALAAgAMwAAAAAAAAARAAIAAQAAAAsABgABAAAAR0xTTC5zdGQuNDUwAAAAAA4AAwAAAAAAAQAAAA8ACAAAAAAABAAAAG1haW4AAAAAHwAAACMAAAAvAAAAAwADAAIAAADCAQAABQAEAAQAAABtYWluAAAAAAUAAwAMAAAAcG9zAAUAAwATAAAAdXYAAAUABgAdAAAAZ2xfUGVyVmVydGV4AAAAAAYABgAdAAAAAAAAAGdsX1Bvc2l0aW9uAAYABwAdAAAAAQAAAGdsX1BvaW50U2l6ZQAAAAAGAAcAHQAAAAIAAABnbF9DbGlwRGlzdGFuY2UABgAHAB0AAAADAAAAZ2xfQ3VsbERpc3RhbmNlAAUAAwAfAAAAAAAAAAUABgAjAAAAZ2xfVmVydGV4SW5kZXgAAAUAAwAvAAAAdlV2AEcAAwAdAAAAAgAAAEgABQAdAAAAAAAAAAsAAAAAAAAASAAFAB0AAAABAAAACwAAAAEAAABIAAUAHQAAAAIAAAALAAAAAwAAAEgABQAdAAAAAwAAAAsAAAAEAAAARwAEACMAAAALAAAAKgAAAEcABAAvAAAAHgAAAAAAAAATAAIAAgAAACEAAwADAAAAAgAAABYAAwAGAAAAIAAAABcABAAHAAAABgAAAAIAAAAVAAQACAAAACAAAAAAAAAAKwAEAAgAAAAJAAAAAwAAABwABAAKAAAABwAAAAkAAAAgAAQACwAAAAcAAAAKAAAAKwAEAAYAAAANAAAAAACAvywABQAHAAAADgAAAA0AAAANAAAAKwAEAAYAAAAPAAAAAABAQCwABQAHAAAAEAAAAA8AAAANAAAALAAFAAcAAAARAAAADQAAAA8AAAAsAAYACgAAABIAAAAOAAAAEAAAABEAAAArAAQABgAAABQAAAAAAAAALAAFAAcAAAAVAAAAFAAAABQAAAArAAQABgAAABYAAAAAAABALAAFAAcAAAAXAAAAFgAAABQAAAAsAAUABwAAABgAAAAUAAAAFgAAACwABgAKAAAAGQAAABUAAAAXAAAAGAAAABcABAAaAAAABgAAAAQAAAArAAQACAAAABsAAAABAAAAHAAEABwAAAAGAAAAGwAAAB4ABgAdAAAAGgAAAAYAAAAcAAAAHAAAACAABAAeAAAAAwAAAB0AAAA7AAQAHgAAAB8AAAADAAAAFQAEACAAAAAgAAAAAQAAACsABAAgAAAAIQAAAAAAAAAgAAQAIgAAAAEAAAAgAAAAOwAEACIAAAAjAAAAAQAAACAABAAlAAAABwAAAAcAAAArAAQABgAAACgAAAAAAIA/IAAEACwAAAADAAAAGgAAACAABAAuAAAAAwAAAAcAAAA7AAQALgAAAC8AAAADAAAANgAFAAIAAAAEAAAAAAAAAAMAAAD4AAIABQAAADsABAALAAAADAAAAAcAAAA7AAQACwAAABMAAAAHAAAAPgADAAwAAAASAAAAPgADABMAAAAZAAAAPQAEACAAAAAkAAAAIwAAAEEABQAlAAAAJgAAAAwAAAAkAAAAPQAEAAcAAAAnAAAAJgAAAFEABQAGAAAAKQAAACcAAAAAAAAAUQAFAAYAAAAqAAAAJwAAAAEAAABQAAcAGgAAACsAAAApAAAAKgAAABQAAAAoAAAAQQAFACwAAAAtAAAAHwAAACEAAAA+AAMALQAAACsAAAA9AAQAIAAAADAAAAAjAAAAQQAFACUAAAAxAAAAEwAAADAAAAA9AAQABwAAADIAAAAxAAAAPgADAC8AAAAyAAAA/QABADgAAQA=";

    constexpr char kOpenXrBlitFragSpvBase64[] =
        "AwIjBwAAAQALAAgAXwAAAAAAAAARAAIAAQAAAAsABgABAAAAR0xTTC5zdGQuNDUwAAAAAA4AAwAAAAAAAQAAAA8ABwAEAAAABAAAAG1haW4AAAAAPAAAAE4AAAAQAAMABAAAAAcAAAADAAMAAgAAAMIBAAAFAAQABAAAAG1haW4AAAAABQAHAAsAAABzcmdiVG9MaW5lYXIodmYzOwAAAAUAAwAKAAAAYwAAAAUABAAQAAAAY3V0b2ZmAAAFAAMAFQAAAGxvdwAFAAQAGgAAAGhpZ2gAAAAABQADAC0AAAB1dgAABQAGAC8AAABQdXNoQ29uc3RhbnRzAAAABgAFAC8AAAAAAAAAYm91bmRzAAAFAAMAMQAAAHBjAAAFAAMAPAAAAHZVdgAFAAMARQAAAGMAAAAFAAQASQAAAHNyY1RleAAABQAFAE4AAABvdXRDb2xvcgAAAAAFAAQAVAAAAHBhcmFtAAAARwADAC8AAAACAAAASAAFAC8AAAAAAAAAIwAAAAAAAABHAAQAPAAAAB4AAAAAAAAARwAEAEkAAAAhAAAAAAAAAEcABABJAAAAIgAAAAAAAABHAAQATgAAAB4AAAAAAAAAEwACAAIAAAAhAAMAAwAAAAIAAAAWAAMABgAAACAAAAAXAAQABwAAAAYAAAADAAAAIAAEAAgAAAAHAAAABwAAACEABAAJAAAABwAAAAgAAAAUAAIADQAAABcABAAOAAAADQAAAAMAAAAgAAQADwAAAAcAAAAOAAAAKwAEAAYAAAASAAAA5q4lPSwABgAHAAAAEwAAABIAAAASAAAAEgAAACsABAAGAAAAFwAAAFK4TkErAAQABgAAABwAAACuR2E9KwAEAAYAAAAfAAAAPQqHPysABAAGAAAAIgAAAJqZGUAsAAYABwAAACMAAAAiAAAAIgAAACIAAAAXAAQAKwAAAAYAAAACAAAAIAAEACwAAAAHAAAAKwAAABcABAAuAAAABgAAAAQAAAAeAAMALwAAAC4AAAAgAAQAMAAAAAkAAAAvAAAAOwAEADAAAAAxAAAACQAAABUABAAyAAAAIAAAAAEAAAArAAQAMgAAADMAAAAAAAAAIAAEADQAAAAJAAAALgAAACAABAA7AAAAAQAAACsAAAA7AAQAOwAAADwAAAABAAAAKwAEAAYAAAA+AAAAAAAAACwABQArAAAAPwAAAD4AAAA+AAAAKwAEAAYAAABAAAAAAACAPywABQArAAAAQQAAAEAAAABAAAAAIAAEAEQAAAAHAAAALgAAABkACQBGAAAABgAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAABsAAwBHAAAARgAAACAABABIAAAAAAAAAEcAAAA7AAQASAAAAEkAAAAAAAAAIAAEAE0AAAADAAAALgAAADsABABNAAAATgAAAAMAAAAsAAYABwAAAFEAAAA+AAAAPgAAAD4AAAAsAAYABwAAAFIAAABAAAAAQAAAAEAAAAAVAAQAVgAAACAAAAAAAAAAKwAEAFYAAABXAAAAAwAAACAABABYAAAABwAAAAYAAAA2AAUAAgAAAAQAAAAAAAAAAwAAAPgAAgAFAAAAOwAEACwAAAAtAAAABwAAADsABABEAAAARQAAAAcAAAA7AAQACAAAAFQAAAAHAAAAQQAFADQAAAA1AAAAMQAAADMAAAA9AAQALgAAADYAAAA1AAAATwAHACsAAAA3AAAANgAAADYAAAAAAAAAAQAAAEEABQA0AAAAOAAAADEAAAAzAAAAPQAEAC4AAAA5AAAAOAAAAE8ABwArAAAAOgAAADkAAAA5AAAAAgAAAAMAAAA9AAQAKwAAAD0AAAA8AAAADAAIACsAAABCAAAAAQAAACsAAAA9AAAAPwAAAEEAAAAMAAgAKwAAAEMAAAABAAAALgAAADcAAAA6AAAAQgAAAD4AAwAtAAAAQwAAAD0ABABHAAAASgAAAEkAAAA9AAQAKwAAAEsAAAAtAAAAVwAFAC4AAABMAAAASgAAAEsAAAA+AAMARQAAAEwAAAA9AAQALgAAAE8AAABFAAAATwAIAAcAAABQAAAATwAAAE8AAAAAAAAAAQAAAAIAAAAMAAgABwAAAFMAAAABAAAAKwAAAFAAAABRAAAAUgAAAD4AAwBUAAAAUwAAADkABQAHAAAAVQAAAAsAAABUAAAAQQAFAFgAAABZAAAARQAAAFcAAAA9AAQABgAAAFoAAABZAAAAUQAFAAYAAABbAAAAVQAAAAAAAABRAAUABgAAAFwAAABVAAAAAQAAAFEABQAGAAAAXQAAAFUAAAACAAAAUAAHAC4AAABeAAAAWwAAAFwAAABdAAAAWgAAAD4AAwBOAAAAXgAAAP0AAQA4AAEANgAFAAcAAAALAAAAAAAAAAkAAAA3AAMACAAAAAoAAAD4AAIADAAAADsABAAPAAAAEAAAAAcAAAA7AAQACAAAABUAAAAHAAAAOwAEAAgAAAAaAAAABwAAAD0ABAAHAAAAEQAAAAoAAAC8AAUADgAAABQAAAARAAAAEwAAAD4AAwAQAAAAFAAAAD0ABAAHAAAAFgAAAAoAAABQAAYABwAAABgAAAAXAAAAFwAAABcAAACIAAUABwAAABkAAAAWAAAAGAAAAD4AAwAVAAAAGQAAAD0ABAAHAAAAGwAAAAoAAABQAAYABwAAAB0AAAAcAAAAHAAAABwAAACBAAUABwAAAB4AAAAbAAAAHQAAAFAABgAHAAAAIAAAAB8AAAAfAAAAHwAAAIgABQAHAAAAIQAAAB4AAAAgAAAADAAHAAcAAAAkAAAAAQAAABoAAAAhAAAAIwAAAD4AAwAaAAAAJAAAAD0ABAAHAAAAJQAAABoAAAA9AAQABwAAACYAAAAVAAAAPQAEAA4AAAAnAAAAEAAAAKkABgAHAAAAKAAAACcAAAAmAAAAJQAAAP4AAgAoAAAAOAABAA==";

    struct VulkanDispatch
    {
        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
        PFN_vkCreateInstance vkCreateInstance = nullptr;
        PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
        PFN_vkDestroyInstance vkDestroyInstance = nullptr;
        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddrInstance = nullptr;
        PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
        PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
        PFN_vkCreateDevice vkCreateDevice = nullptr;
        PFN_vkDestroyDevice vkDestroyDevice = nullptr;
        PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
        PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
        PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
        PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
        PFN_vkQueueSubmit vkQueueSubmit = nullptr;
        PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
        PFN_vkCreateImage vkCreateImage = nullptr;
        PFN_vkDestroyImage vkDestroyImage = nullptr;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
        PFN_vkCreateBuffer vkCreateBuffer = nullptr;
        PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
        PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
        PFN_vkAllocateMemory vkAllocateMemory = nullptr;
        PFN_vkFreeMemory vkFreeMemory = nullptr;
        PFN_vkBindImageMemory vkBindImageMemory = nullptr;
        PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
        PFN_vkMapMemory vkMapMemory = nullptr;
        PFN_vkUnmapMemory vkUnmapMemory = nullptr;
        PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
        PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
        PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
        PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
        PFN_vkCreateImageView vkCreateImageView = nullptr;
        PFN_vkDestroyImageView vkDestroyImageView = nullptr;
        PFN_vkCreateSampler vkCreateSampler = nullptr;
        PFN_vkDestroySampler vkDestroySampler = nullptr;
        PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
        PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
        PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
        PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
        PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
        PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
        PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
        PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
        PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
        PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
        PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
        PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
        PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
        PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
        PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
        PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
        PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
        PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
        PFN_vkCmdDraw vkCmdDraw = nullptr;
    };

    struct VulkanEyeSwapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool supportsTransferSrc = false;
        std::vector<XrSwapchainImageVulkanKHR> images;
        std::vector<VkImageLayout> layouts;
        std::vector<VkImageView> imageViews;
        std::vector<VkFramebuffer> framebuffers;
    };

    struct VulkanGameEyeTexture
    {
        uint32_t generation = 0;
        uint64_t kmtHandle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
        float uMin = 0.0f;
        float vMin = 0.0f;
        float uMax = 1.0f;
        float vMax = 1.0f;
        float renderFovXDeg = 90.0f;
        float renderAspect = 1.0f;
    };

    class BridgeWriter
    {
    public:
        bool Open(const std::wstring& mappingName, Logger& log)
        {
            if (mappingName.empty())
                return true;

            m_Mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
            if (!m_Mapping)
            {
                log.Print("OpenFileMapping failed for bridge mapping (GetLastError=%lu)", GetLastError());
                return false;
            }

            m_State = static_cast<L4D2VROpenXrBridgeState*>(
                MapViewOfFile(m_Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(L4D2VROpenXrBridgeState)));
            if (!m_State)
            {
                log.Print("MapViewOfFile failed for bridge mapping (GetLastError=%lu)", GetLastError());
                CloseHandle(m_Mapping);
                m_Mapping = nullptr;
                return false;
            }

            if (m_State->magic != L4D2VR_OPENXR_BRIDGE_MAGIC ||
                m_State->version != L4D2VR_OPENXR_BRIDGE_VERSION ||
                m_State->size != sizeof(L4D2VROpenXrBridgeState))
            {
                log.Print("Bridge mapping has incompatible header");
                return false;
            }

            m_State->helperPid = GetCurrentProcessId();
            Update(L4D2VROpenXrBridgeStatus::Starting, 0, 0, "helper started");
            return true;
        }

        ~BridgeWriter()
        {
            if (m_State)
            {
                UnmapViewOfFile(m_State);
                m_State = nullptr;
            }
            if (m_Mapping)
            {
                CloseHandle(m_Mapping);
                m_Mapping = nullptr;
            }
        }

        void Update(L4D2VROpenXrBridgeStatus status, int32_t exitCode, uint32_t submittedFrames, const char* detail)
        {
            if (!m_State)
                return;

            m_State->status = static_cast<uint32_t>(status);
            m_State->exitCode = exitCode;
            m_State->submittedFrames = submittedFrames;
            m_State->heartbeatTickMs = GetTickCount64();
            if (detail)
            {
                std::snprintf(m_State->detail, sizeof(m_State->detail), "%s", detail);
            }
        }

        bool HasState() const
        {
            return m_State != nullptr;
        }

        bool SharedTexturesReady() const
        {
            return m_State &&
                (m_State->sharedTexturesReadyMask & L4D2VR_OPENXR_EYES_READY_MASK) == L4D2VR_OPENXR_EYES_READY_MASK;
        }

        uint32_t SharedTextureGeneration() const
        {
            return m_State ? m_State->sharedTextureGeneration : 0;
        }

        L4D2VROpenXrSharedTextureDesc SharedTexture(uint32_t eyeIndex) const
        {
            if (!m_State || eyeIndex >= L4D2VR_OPENXR_EYE_COUNT)
                return {};
            return m_State->eyeTextures[eyeIndex];
        }

        uint32_t OverlayGeneration() const
        {
            return m_State ? m_State->overlayGeneration : 0;
        }

        L4D2VROpenXrOverlayDesc Overlay(uint32_t overlayIndex) const
        {
            if (!m_State || overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT)
                return {};
            return m_State->overlays[overlayIndex];
        }

        bool ReadOverlayFrame(uint32_t& frameId, uint32_t* generation = nullptr) const
        {
            if (!m_State)
                return false;

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint32_t gen0 = m_State->overlayFrameGeneration;
                if (gen0 == 0 || (gen0 & 1u))
                    continue;

                const uint32_t snapshotFrameId = m_State->overlayFrameId;
                const uint32_t gen1 = m_State->overlayFrameGeneration;
                if (gen0 == gen1 && !(gen1 & 1u) && snapshotFrameId != 0)
                {
                    frameId = snapshotFrameId;
                    if (generation)
                        *generation = gen1;
                    return true;
                }
            }

            return false;
        }

        bool ReadSharedTextureFrame(uint32_t& frameId, uint32_t* generation = nullptr) const
        {
            if (!m_State)
                return false;

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint32_t gen0 = m_State->sharedTextureFrameGeneration;
                if (gen0 == 0 || (gen0 & 1u))
                    continue;

                const uint32_t snapshotFrameId = m_State->sharedTextureFrameId;
                const uint32_t gen1 = m_State->sharedTextureFrameGeneration;
                if (gen0 == gen1 && !(gen1 & 1u) && snapshotFrameId != 0)
                {
                    frameId = snapshotFrameId;
                    if (generation)
                        *generation = gen1;
                    return true;
                }
            }

            return false;
        }

        bool ReadGameRenderPose(L4D2VROpenXrPoseDesc& pose, uint32_t* generation = nullptr) const
        {
            if (!m_State)
                return false;

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint32_t gen0 = m_State->gameRenderPoseGeneration;
                if (gen0 == 0 || (gen0 & 1u))
                    continue;

                L4D2VROpenXrPoseDesc snapshot = m_State->gameRenderPose;
                const uint32_t gen1 = m_State->gameRenderPoseGeneration;
                if (gen0 == gen1 && !(gen1 & 1u) && snapshot.valid)
                {
                    pose = snapshot;
                    if (generation)
                        *generation = gen1;
                    return true;
                }
            }

            return false;
        }

        void PublishHmdPose(const L4D2VROpenXrPoseDesc& pose)
        {
            if (!m_State || !pose.valid)
                return;

            ++m_State->trackingPoseGeneration;
            m_State->hmdPose = pose;
            ++m_State->trackingPoseGeneration;
            m_State->heartbeatTickMs = GetTickCount64();
        }

        void PublishRuntimeViewConfig(const L4D2VROpenXrRuntimeViewConfigDesc& config)
        {
            if (!m_State || !config.valid)
                return;

            ++m_State->runtimeViewConfigGeneration;
            m_State->runtimeViewConfig = config;
            ++m_State->runtimeViewConfigGeneration;
            m_State->heartbeatTickMs = GetTickCount64();
        }

        void PublishInputState(const L4D2VROpenXrInputStateDesc& inputState)
        {
            if (!m_State || !inputState.valid)
                return;

            ++m_State->inputStateGeneration;
            m_State->inputState = inputState;
            ++m_State->inputStateGeneration;
            m_State->heartbeatTickMs = GetTickCount64();
        }

        bool ReadHapticRequest(uint32_t handIndex, L4D2VROpenXrHapticRequestDesc& request) const
        {
            if (!m_State || handIndex >= L4D2VR_OPENXR_HAND_COUNT)
                return false;

            const L4D2VROpenXrHapticRequestDesc& sharedRequest = m_State->hapticRequests[handIndex];
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint32_t seq0 = sharedRequest.sequence;
                if (seq0 == 0 || (seq0 & 1u))
                    continue;

                L4D2VROpenXrHapticRequestDesc snapshot = sharedRequest;
                const uint32_t seq1 = sharedRequest.sequence;
                if (seq0 == seq1 && !(seq1 & 1u) && snapshot.valid)
                {
                    request = snapshot;
                    return true;
                }
            }

            return false;
        }

    private:
        HANDLE m_Mapping = nullptr;
        L4D2VROpenXrBridgeState* m_State = nullptr;
    };

    L4D2VROpenXrPoseDesc BuildHmdPoseFromLocatedViews(
        const std::vector<XrView>& views,
        uint32_t locatedCount,
        XrViewStateFlags viewStateFlags,
        XrTime displayTime)
    {
        L4D2VROpenXrPoseDesc pose{};
        if (locatedCount == 0 || views.empty())
            return pose;

        const bool orientationValid = (viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        const bool positionValid = (viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        if (!orientationValid)
            return pose;

        const uint32_t count = std::min<uint32_t>(locatedCount, static_cast<uint32_t>(views.size()));
        pose.valid = 1;
        pose.viewStateFlags = static_cast<uint32_t>(viewStateFlags);
        pose.displayTime = static_cast<int64_t>(displayTime);
        pose.orientation[0] = views[0].pose.orientation.x;
        pose.orientation[1] = views[0].pose.orientation.y;
        pose.orientation[2] = views[0].pose.orientation.z;
        pose.orientation[3] = views[0].pose.orientation.w;

        if (positionValid)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                pose.position[0] += views[i].pose.position.x;
                pose.position[1] += views[i].pose.position.y;
                pose.position[2] += views[i].pose.position.z;
            }
            const float invCount = 1.0f / static_cast<float>(count);
            pose.position[0] *= invCount;
            pose.position[1] *= invCount;
            pose.position[2] *= invCount;
        }
        return pose;
    }

    float ExtractOpenXrYaw(const XrQuaternionf& orientation)
    {
        float x = orientation.x;
        float y = orientation.y;
        float z = orientation.z;
        float w = orientation.w;
        const float lenSq = x * x + y * y + z * z + w * w;
        if (lenSq > 0.000001f)
        {
            const float invLen = 1.0f / std::sqrt(lenSq);
            x *= invLen;
            y *= invLen;
            z *= invLen;
            w *= invLen;
        }
        else
        {
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
            w = 1.0f;
        }

        return std::atan2(
            2.0f * (w * y + x * z),
            1.0f - 2.0f * (y * y + z * z));
    }

    XrQuaternionf MakeOpenXrYawQuaternion(float yaw)
    {
        const float half = yaw * 0.5f;
        return XrQuaternionf{ 0.0f, std::sin(half), 0.0f, std::cos(half) };
    }

    XrQuaternionf NormalizeOpenXrQuaternion(const float orientation[4])
    {
        XrQuaternionf q{
            orientation[0],
            orientation[1],
            orientation[2],
            orientation[3]
        };
        const float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (lenSq > 0.000001f)
        {
            const float invLen = 1.0f / std::sqrt(lenSq);
            q.x *= invLen;
            q.y *= invLen;
            q.z *= invLen;
            q.w *= invLen;
        }
        else
        {
            q = XrQuaternionf{ 0.0f, 0.0f, 0.0f, 1.0f };
        }
        return q;
    }

    XrVector3f RotateOpenXrVector(const XrQuaternionf& q, const XrVector3f& v)
    {
        const float tx = 2.0f * (q.y * v.z - q.z * v.y);
        const float ty = 2.0f * (q.z * v.x - q.x * v.z);
        const float tz = 2.0f * (q.x * v.y - q.y * v.x);
        return XrVector3f{
            v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)
        };
    }

    float GetLocatedViewIpd(const std::vector<XrView>& views, uint32_t locatedCount)
    {
        if (locatedCount < 2 || views.size() < 2)
            return 0.063f;

        const XrVector3f& left = views[L4D2VR_OPENXR_EYE_LEFT].pose.position;
        const XrVector3f& right = views[L4D2VR_OPENXR_EYE_RIGHT].pose.position;
        const float dx = right.x - left.x;
        const float dy = right.y - left.y;
        const float dz = right.z - left.z;
        const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
        return (std::isfinite(ipd) && ipd > 0.01f && ipd < 0.20f) ? ipd : 0.063f;
    }

    bool TryBuildSymmetricProjectionFov(float renderFovXDeg, float renderAspect, XrFovf& outFov)
    {
        if (!std::isfinite(renderFovXDeg) || renderFovXDeg <= 1.0f || renderFovXDeg >= 179.0f ||
            !std::isfinite(renderAspect) || renderAspect <= 0.1f || renderAspect >= 10.0f)
            return false;

        const float halfFovX = 0.5f * renderFovXDeg * (kPi / 180.0f);
        const float tanHalfFovX = std::tan(halfFovX);
        const float tanHalfFovY = tanHalfFovX / renderAspect;
        if (!std::isfinite(tanHalfFovX) || tanHalfFovX <= 0.0f ||
            !std::isfinite(tanHalfFovY) || tanHalfFovY <= 0.0f)
            return false;

        const float halfFovY = std::atan(tanHalfFovY);
        outFov.angleLeft = -halfFovX;
        outFov.angleRight = halfFovX;
        outFov.angleUp = halfFovY;
        outFov.angleDown = -halfFovY;
        return true;
    }

    XrPosef BuildProjectionPoseFromGameRenderPose(
        const L4D2VROpenXrPoseDesc& renderPose,
        const std::vector<XrView>& locatedViews,
        uint32_t locatedCount,
        uint32_t eyeIndex,
        float* outYaw = nullptr,
        float* outIpd = nullptr)
    {
        XrPosef pose{};
        pose.orientation = NormalizeOpenXrQuaternion(renderPose.orientation);
        pose.position = XrVector3f{
            renderPose.position[0],
            renderPose.position[1],
            renderPose.position[2]
        };

        const float ipd = GetLocatedViewIpd(locatedViews, locatedCount);
        XrVector3f right = RotateOpenXrVector(pose.orientation, XrVector3f{ 1.0f, 0.0f, 0.0f });
        const float rightLen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
        if (rightLen > 0.0001f)
        {
            const float invLen = 1.0f / rightLen;
            right.x *= invLen;
            right.y *= invLen;
            right.z *= invLen;
        }
        else
        {
            right = XrVector3f{ 1.0f, 0.0f, 0.0f };
        }

        const float side = (eyeIndex == L4D2VR_OPENXR_EYE_LEFT) ? -0.5f : 0.5f;
        pose.position.x += right.x * ipd * side;
        pose.position.y += right.y * ipd * side;
        pose.position.z += right.z * ipd * side;

        if (outYaw)
            *outYaw = ExtractOpenXrYaw(pose.orientation);
        if (outIpd)
            *outIpd = ipd;
        return pose;
    }

    XrPosef BuildProjectionPose(
        const std::vector<XrView>& views,
        uint32_t locatedCount,
        uint32_t eyeIndex,
        float* outYaw = nullptr,
        float* outIpd = nullptr)
    {
        if (views.empty() || eyeIndex >= views.size())
            return XrPosef{ XrQuaternionf{ 0.0f, 0.0f, 0.0f, 1.0f }, XrVector3f{ 0.0f, 0.0f, 0.0f } };

        XrPosef pose = views[eyeIndex].pose;
        const float yaw = ExtractOpenXrYaw(views[0].pose.orientation);

        if (locatedCount >= 2 && views.size() >= 2)
        {
            const XrVector3f& left = views[L4D2VR_OPENXR_EYE_LEFT].pose.position;
            const XrVector3f& right = views[L4D2VR_OPENXR_EYE_RIGHT].pose.position;
            const float dx = right.x - left.x;
            const float dy = right.y - left.y;
            const float dz = right.z - left.z;
            const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (outIpd)
                *outIpd = ipd;
        }

        if (outYaw)
            *outYaw = yaw;
        return pose;
    }

    bool RuntimeViewConfigIsReady(XrSessionState sessionState)
    {
        return sessionState == XR_SESSION_STATE_FOCUSED;
    }

    void LogRuntimeViewConfig(Logger& log, const char* renderer, const L4D2VROpenXrRuntimeViewConfigDesc& config)
    {
        if (!config.valid)
            return;

        const L4D2VROpenXrRuntimeViewDesc& left = config.views[L4D2VR_OPENXR_EYE_LEFT];
        const L4D2VROpenXrRuntimeViewDesc& right = config.views[L4D2VR_OPENXR_EYE_RIGHT];
        log.Print(
            "%s publishing focused runtime view config L(%ux%u fov=%.4f %.4f %.4f %.4f) R(%ux%u fov=%.4f %.4f %.4f %.4f)",
            renderer,
            left.width,
            left.height,
            left.angleLeft,
            left.angleRight,
            left.angleUp,
            left.angleDown,
            right.width,
            right.height,
            right.angleLeft,
            right.angleRight,
            right.angleUp,
            right.angleDown);
    }

    template <typename T>
    bool TryLoadXrFunction(PFN_xrGetInstanceProcAddr getInstanceProcAddr, XrInstance instance, const char* name, T& out)
    {
        PFN_xrVoidFunction function = nullptr;
        const XrResult result = getInstanceProcAddr(instance, name, &function);
        if (XR_FAILED(result) || !function)
            return false;

        out = reinterpret_cast<T>(function);
        return true;
    }

    bool LoadOpenXrInputFunctions(XrDispatch& xr, XrInstance instance, Logger& log)
    {
        return
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrStringToPath", xr.xrStringToPath, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrPathToString", xr.xrPathToString, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrCreateActionSet", xr.xrCreateActionSet, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrDestroyActionSet", xr.xrDestroyActionSet, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrCreateAction", xr.xrCreateAction, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrDestroyAction", xr.xrDestroyAction, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrSuggestInteractionProfileBindings", xr.xrSuggestInteractionProfileBindings, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrGetCurrentInteractionProfile", xr.xrGetCurrentInteractionProfile, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrAttachSessionActionSets", xr.xrAttachSessionActionSets, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrGetActionStateBoolean", xr.xrGetActionStateBoolean, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrGetActionStateFloat", xr.xrGetActionStateFloat, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrGetActionStateVector2f", xr.xrGetActionStateVector2f, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrGetActionStatePose", xr.xrGetActionStatePose, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrSyncActions", xr.xrSyncActions, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrCreateActionSpace", xr.xrCreateActionSpace, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrLocateSpace", xr.xrLocateSpace, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrApplyHapticFeedback", xr.xrApplyHapticFeedback, log) &&
            LoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrStopHapticFeedback", xr.xrStopHapticFeedback, log);
    }

    void LoadOpenXrOptionalHandTrackingFunctions(XrDispatch& xr, XrInstance instance, Logger& log, bool handTrackingEnabled)
    {
        if (!handTrackingEnabled)
            return;

        const bool loaded =
            TryLoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrCreateHandTrackerEXT", xr.xrCreateHandTrackerEXT) &&
            TryLoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrDestroyHandTrackerEXT", xr.xrDestroyHandTrackerEXT) &&
            TryLoadXrFunction(xr.xrGetInstanceProcAddr, instance, "xrLocateHandJointsEXT", xr.xrLocateHandJointsEXT);
        if (!loaded)
        {
            xr.xrCreateHandTrackerEXT = nullptr;
            xr.xrDestroyHandTrackerEXT = nullptr;
            xr.xrLocateHandJointsEXT = nullptr;
            log.Print("XR_EXT_hand_tracking enabled but extension entry points were unavailable");
        }
    }

    class OpenXrInputBridge
    {
    public:
        bool InitializeInstance(XrDispatch& xr, XrInstance instance, Logger& log);
        bool InitializeSession(XrSession session, XrSpace appSpace, bool handTrackingEnabled, Logger& log);
        void Shutdown();
        void UpdateFrame(XrTime displayTime, BridgeWriter& bridge, Logger& log);

    private:
        struct BooleanActionDef
        {
            L4D2VROpenXrActionId id;
            const char* name;
            const char* localizedName;
        };

        struct FloatDigitalActionDef
        {
            L4D2VROpenXrActionId id;
            const char* name;
            const char* localizedName;
            float threshold;
        };

        struct AnalogActionDef
        {
            L4D2VROpenXrActionId id;
            const char* name;
            const char* localizedName;
        };

        struct Vec3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        static constexpr size_t Index(L4D2VROpenXrActionId id)
        {
            return static_cast<size_t>(id);
        }

        static const std::array<BooleanActionDef, 31>& BooleanDefs();
        static const std::array<BooleanActionDef, 3>& ExtraBooleanDefs();
        static const std::array<FloatDigitalActionDef, 4>& FloatDigitalDefs();
        static const std::array<AnalogActionDef, 2>& AnalogDefs();

        bool Path(const char* text, XrPath& out, Logger& log);
        bool TryPath(const char* text, XrPath& out);
        bool CreateAction(XrActionSet set, XrActionType type, const char* name, const char* localizedName, XrAction& out, Logger& log);
        bool CreateSubactionAction(XrActionSet set, XrActionType type, const char* name, const char* localizedName, XrAction& out, Logger& log);
        bool CreatePoseAction(Logger& log);
        bool CreateHapticAction(Logger& log);
        bool CreateBooleanActions(Logger& log);
        bool CreateFloatDigitalActions(Logger& log);
        bool CreateAnalogActions(Logger& log);
        void AddBinding(std::vector<XrActionSuggestedBinding>& bindings, XrAction action, const char* bindingPath);
        void SuggestProfile(Logger& log, const char* profilePath, const std::vector<XrActionSuggestedBinding>& bindings);
        void AddPoseAndHapticBindings(std::vector<XrActionSuggestedBinding>& bindings);
        void AddStickAndTriggerBindings(std::vector<XrActionSuggestedBinding>& bindings, const char* stickName);
        void AddGripValueBindings(std::vector<XrActionSuggestedBinding>& bindings, const char* gripValueName);
        void AddTouchFaceButtonBindings(std::vector<XrActionSuggestedBinding>& bindings);
        void AddIndexFaceButtonBindings(std::vector<XrActionSuggestedBinding>& bindings);
        void AddMenuButtonBindings(std::vector<XrActionSuggestedBinding>& bindings);
        void SuggestBindings(Logger& log);
        std::string PathToString(XrPath path) const;
        void LogCurrentInteractionProfiles(Logger& log);
        void ReadBooleanActions(L4D2VROpenXrInputStateDesc& outState);
        void ReadFloatDigitalActions(L4D2VROpenXrInputStateDesc& outState);
        void ReadAnalogActions(L4D2VROpenXrInputStateDesc& outState);
        void SetDerivedDigital(L4D2VROpenXrInputStateDesc& outState, L4D2VROpenXrActionId id, bool down);
        void PublishDerivedDpadActions(L4D2VROpenXrInputStateDesc& outState);
        void LocateControllerPoses(XrTime displayTime, L4D2VROpenXrInputStateDesc& outState, Logger& log);
        static Vec3 JointPos(const XrHandJointLocationEXT& joint);
        static Vec3 Sub(Vec3 a, Vec3 b);
        static float Dot(Vec3 a, Vec3 b);
        static float Length(Vec3 value);
        static float Angle(Vec3 a, Vec3 b);
        static bool JointPositionValid(const XrHandJointLocationEXT& joint);
        static float ComputeCurl(
            const std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT>& joints,
            XrHandJointEXT j0,
            XrHandJointEXT j1,
            XrHandJointEXT j2,
            XrHandJointEXT j3,
            XrHandJointEXT j4);
        void LocateHandTracking(XrTime displayTime, L4D2VROpenXrInputStateDesc& outState);
        void PumpHaptics(BridgeWriter& bridge, Logger& log);
        void DestroyAction(XrAction& action);

        XrDispatch* m_Xr = nullptr;
        XrInstance m_Instance = XR_NULL_HANDLE;
        XrSession m_Session = XR_NULL_HANDLE;
        XrSpace m_AppSpace = XR_NULL_HANDLE;
        XrActionSet m_MainActionSet = XR_NULL_HANDLE;
        XrActionSet m_BaseActionSet = XR_NULL_HANDLE;
        XrAction m_HandPoseAction = XR_NULL_HANDLE;
        XrAction m_HapticAction = XR_NULL_HANDLE;
        std::array<XrPath, L4D2VR_OPENXR_HAND_COUNT> m_HandPaths{};
        std::array<XrSpace, L4D2VR_OPENXR_HAND_COUNT> m_HandSpaces{};
        std::array<XrHandTrackerEXT, L4D2VR_OPENXR_HAND_COUNT> m_HandTrackers{};
        std::array<XrAction, L4D2VR_OPENXR_ACTION_COUNT> m_BooleanActions{};
        std::array<XrAction, L4D2VR_OPENXR_ACTION_COUNT> m_FloatDigitalActions{};
        std::array<XrAction, L4D2VR_OPENXR_ACTION_COUNT> m_AnalogActions{};
        std::array<bool, L4D2VR_OPENXR_ACTION_COUNT> m_FloatDigitalDown{};
        std::array<bool, L4D2VR_OPENXR_ACTION_COUNT> m_FloatDigitalInitialized{};
        std::array<bool, L4D2VR_OPENXR_ACTION_COUNT> m_DerivedDigitalDown{};
        std::array<bool, L4D2VR_OPENXR_ACTION_COUNT> m_DerivedDigitalInitialized{};
        std::array<XrPath, L4D2VR_OPENXR_HAND_COUNT> m_LastInteractionProfiles{};
        std::array<bool, L4D2VR_OPENXR_HAND_COUNT> m_PoseInactiveLogged{};
        std::array<uint32_t, L4D2VR_OPENXR_HAND_COUNT> m_LastHapticSequences{};
        uint32_t m_FeatureFlags = 0;
        bool m_SessionInitialized = false;
        bool m_SyncFailureLogged = false;
        bool m_HapticFailureLogged = false;
    };

#if 0
    class OpenXrSubmitProbe
    {
    public:
        explicit OpenXrSubmitProbe(Logger& log)
            : m_Log(log)
        {
        }

        int Run(const Options& options)
        {
            m_Bridge.Open(options.mappingName, m_Log);
            m_ParentProcess = OpenParentProcess(options.parentPid);

            if (!LoadLoader())
                return Fail(10, "OpenXR loader load failed");
            if (!LoadGlobalFunctions())
                return Fail(11, "OpenXR global function load failed");
            if (!CreateInstance())
                return Fail(12, "OpenXR instance creation failed");
            if (!LoadInstanceFunctions())
                return Fail(13, "OpenXR instance function load failed");
            if (!CreateSystemAndDevice())
                return Fail(14, "OpenXR D3D11 device creation failed");
            if (!CreateSessionObjects())
                return Fail(15, "OpenXR session creation failed");
            if (!CreateSwapchains())
                return Fail(16, "OpenXR swapchain creation failed");

            const int exitCode = FrameLoop(options);
            if (exitCode != 0)
                m_Bridge.Update(L4D2VROpenXrBridgeStatus::Failed, exitCode, 0, "OpenXR frame loop failed");
            return exitCode;
        }

        ~OpenXrSubmitProbe()
        {
            Shutdown();
        }

    private:
        int Fail(int code, const char* detail)
        {
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::Failed, code, 0, detail);
            return code;
        }

        bool LoadLoader()
        {
            const std::wstring loaderPath = ExeDirectory() + L"\\openxr_loader.dll";
            m_Loader = LoadLibraryW(loaderPath.c_str());
            if (!m_Loader)
            {
                m_Log.Print("LoadLibrary failed for %s (GetLastError=%lu)", Narrow(loaderPath).c_str(), GetLastError());
                return false;
            }

            m_Xr.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
                GetProcAddress(m_Loader, "xrGetInstanceProcAddr"));
            if (!m_Xr.xrGetInstanceProcAddr)
            {
                m_Log.Print("openxr_loader.dll does not export xrGetInstanceProcAddr");
                return false;
            }

            m_Log.Print("Loaded OpenXR loader: %s", Narrow(loaderPath).c_str());
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::LoaderLoaded, 0, 0, "OpenXR loader loaded");
            return true;
        }

        bool LoadGlobalFunctions()
        {
            return LoadXrFunction(
                m_Xr.xrGetInstanceProcAddr,
                XR_NULL_HANDLE,
                "xrEnumerateInstanceExtensionProperties",
                m_Xr.xrEnumerateInstanceExtensionProperties,
                m_Log) &&
                LoadXrFunction(
                    m_Xr.xrGetInstanceProcAddr,
                    XR_NULL_HANDLE,
                    "xrCreateInstance",
                    m_Xr.xrCreateInstance,
                    m_Log);
        }

        bool CreateInstance()
        {
            uint32_t extensionCount = 0;
            XrResult result = m_Xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
            if (!Succeeded(m_Log, "xrEnumerateInstanceExtensionProperties(count)", result))
                return false;

            std::vector<XrExtensionProperties> extensions(extensionCount);
            for (XrExtensionProperties& extension : extensions)
                extension.type = XR_TYPE_EXTENSION_PROPERTIES;

            result = m_Xr.xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
            if (!Succeeded(m_Log, "xrEnumerateInstanceExtensionProperties(data)", result))
                return false;

            bool hasD3D11 = false;
            for (const XrExtensionProperties& extension : extensions)
            {
                if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
                    hasD3D11 = true;
            }

            if (!hasD3D11)
            {
                m_Log.Print("%s is not supported by the active runtime", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
                return false;
            }

            const char* enabledExtensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

            XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
            std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "L4D2VR OpenXR Helper");
            createInfo.applicationInfo.applicationVersion = 1;
            std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "L4D2VR");
            createInfo.applicationInfo.engineVersion = 1;
            createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
            createInfo.enabledExtensionCount = ARRAYSIZE(enabledExtensions);
            createInfo.enabledExtensionNames = enabledExtensions;

            result = m_Xr.xrCreateInstance(&createInfo, &m_Instance);
            if (!Succeeded(m_Log, "xrCreateInstance", result) || m_Instance == XR_NULL_HANDLE)
                return false;

            m_Log.Print("Created OpenXR instance with %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::InstanceCreated, 0, 0, "OpenXR instance created");
            return true;
        }

        bool LoadInstanceFunctions()
        {
            return
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroyInstance", m_Xr.xrDestroyInstance, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetInstanceProperties", m_Xr.xrGetInstanceProperties, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetSystem", m_Xr.xrGetSystem, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateViewConfigurationViews", m_Xr.xrEnumerateViewConfigurationViews, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateEnvironmentBlendModes", m_Xr.xrEnumerateEnvironmentBlendModes, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrCreateSession", m_Xr.xrCreateSession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroySession", m_Xr.xrDestroySession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrCreateReferenceSpace", m_Xr.xrCreateReferenceSpace, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroySpace", m_Xr.xrDestroySpace, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateSwapchainFormats", m_Xr.xrEnumerateSwapchainFormats, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrCreateSwapchain", m_Xr.xrCreateSwapchain, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroySwapchain", m_Xr.xrDestroySwapchain, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateSwapchainImages", m_Xr.xrEnumerateSwapchainImages, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrAcquireSwapchainImage", m_Xr.xrAcquireSwapchainImage, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrWaitSwapchainImage", m_Xr.xrWaitSwapchainImage, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrReleaseSwapchainImage", m_Xr.xrReleaseSwapchainImage, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrPollEvent", m_Xr.xrPollEvent, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrBeginSession", m_Xr.xrBeginSession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEndSession", m_Xr.xrEndSession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrWaitFrame", m_Xr.xrWaitFrame, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrBeginFrame", m_Xr.xrBeginFrame, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEndFrame", m_Xr.xrEndFrame, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrLocateViews", m_Xr.xrLocateViews, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetD3D11GraphicsRequirementsKHR", m_Xr.xrGetD3D11GraphicsRequirementsKHR, m_Log);
        }

        bool CreateSystemAndDevice()
        {
            XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
            XrResult result = m_Xr.xrGetInstanceProperties(m_Instance, &instanceProperties);
            if (XR_SUCCEEDED(result))
            {
                m_Log.Print(
                    "Runtime: %s %llu.%llu.%llu",
                    instanceProperties.runtimeName,
                    static_cast<unsigned long long>(XR_VERSION_MAJOR(instanceProperties.runtimeVersion)),
                    static_cast<unsigned long long>(XR_VERSION_MINOR(instanceProperties.runtimeVersion)),
                    static_cast<unsigned long long>(XR_VERSION_PATCH(instanceProperties.runtimeVersion)));
            }

            XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
            systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            result = m_Xr.xrGetSystem(m_Instance, &systemInfo, &m_SystemId);
            if (!Succeeded(m_Log, "xrGetSystem(HMD)", result) || m_SystemId == XR_NULL_SYSTEM_ID)
                return false;

            XrGraphicsRequirementsD3D11KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
            result = m_Xr.xrGetD3D11GraphicsRequirementsKHR(m_Instance, m_SystemId, &requirements);
            if (!Succeeded(m_Log, "xrGetD3D11GraphicsRequirementsKHR", result))
                return false;

            m_Log.Print(
                "D3D11 requirements: adapterLuid=%ld:%lu minFeatureLevel=0x%X",
                requirements.adapterLuid.HighPart,
                requirements.adapterLuid.LowPart,
                static_cast<unsigned int>(requirements.minFeatureLevel));

            ComPtr<IDXGIFactory1> factory;
            HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
            if (FAILED(hr))
            {
                m_Log.Print("CreateDXGIFactory1 failed: 0x%08X", static_cast<unsigned int>(hr));
                return false;
            }

            ComPtr<IDXGIAdapter1> selectedAdapter;
            for (UINT index = 0;; ++index)
            {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
                    break;

                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && SameLuid(desc.AdapterLuid, requirements.adapterLuid))
                {
                    selectedAdapter = adapter;
                    m_Log.Print("Selected adapter: %ls", desc.Description);
                    break;
                }
            }

            if (!selectedAdapter)
            {
                m_Log.Print("No DXGI adapter matched the OpenXR LUID");
                return false;
            }

            const D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0
            };
            D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_9_1;

            hr = D3D11CreateDevice(
                selectedAdapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                ARRAYSIZE(levels),
                D3D11_SDK_VERSION,
                m_Device.GetAddressOf(),
                &createdLevel,
                m_Context.GetAddressOf());
            if (FAILED(hr))
            {
                m_Log.Print("D3D11CreateDevice failed: 0x%08X", static_cast<unsigned int>(hr));
                return false;
            }

            if (createdLevel < requirements.minFeatureLevel)
            {
                m_Log.Print(
                    "Created D3D11 feature level 0x%X is below OpenXR minimum 0x%X",
                    static_cast<unsigned int>(createdLevel),
                    static_cast<unsigned int>(requirements.minFeatureLevel));
                return false;
            }

            m_Log.Print("Created D3D11 device featureLevel=0x%X", static_cast<unsigned int>(createdLevel));
            return true;
        }

        bool CreateSessionObjects()
        {
            uint32_t viewCount = 0;
            XrResult result = m_Xr.xrEnumerateViewConfigurationViews(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &viewCount,
                nullptr);
            if (!Succeeded(m_Log, "xrEnumerateViewConfigurationViews(count)", result) || viewCount < 2)
                return false;

            m_ViewConfigs.assign(viewCount, XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
            result = m_Xr.xrEnumerateViewConfigurationViews(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                viewCount,
                &viewCount,
                m_ViewConfigs.data());
            if (!Succeeded(m_Log, "xrEnumerateViewConfigurationViews(data)", result) || viewCount < 2)
                return false;
            m_ViewConfigs.resize(viewCount);

            m_Log.Print(
                "Stereo views=%u recommendedEye=%ux%u samples=%u",
                viewCount,
                m_ViewConfigs[0].recommendedImageRectWidth,
                m_ViewConfigs[0].recommendedImageRectHeight,
                m_ViewConfigs[0].recommendedSwapchainSampleCount);

            uint32_t blendCount = 0;
            result = m_Xr.xrEnumerateEnvironmentBlendModes(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &blendCount,
                nullptr);
            if (!Succeeded(m_Log, "xrEnumerateEnvironmentBlendModes(count)", result) || blendCount == 0)
                return false;

            std::vector<XrEnvironmentBlendMode> blendModes(blendCount);
            result = m_Xr.xrEnumerateEnvironmentBlendModes(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                blendCount,
                &blendCount,
                blendModes.data());
            if (!Succeeded(m_Log, "xrEnumerateEnvironmentBlendModes(data)", result) || blendCount == 0)
                return false;

            m_BlendMode = blendModes[0];
            for (XrEnvironmentBlendMode mode : blendModes)
            {
                if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
                    m_BlendMode = mode;
            }

            XrGraphicsBindingD3D11KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
            graphicsBinding.device = m_Device.Get();

            XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
            sessionInfo.next = &graphicsBinding;
            sessionInfo.systemId = m_SystemId;

            result = m_Xr.xrCreateSession(m_Instance, &sessionInfo, &m_Session);
            if (!Succeeded(m_Log, "xrCreateSession", result) || m_Session == XR_NULL_HANDLE)
                return false;

            XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

            result = m_Xr.xrCreateReferenceSpace(m_Session, &spaceInfo, &m_AppSpace);
            if (!Succeeded(m_Log, "xrCreateReferenceSpace(LOCAL)", result) || m_AppSpace == XR_NULL_HANDLE)
                return false;

            m_Log.Print("Created OpenXR session and LOCAL reference space");
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::SessionCreated, 0, 0, "OpenXR session created");
            return true;
        }

        bool CreateSwapchains()
        {
            uint32_t formatCount = 0;
            XrResult result = m_Xr.xrEnumerateSwapchainFormats(m_Session, 0, &formatCount, nullptr);
            if (!Succeeded(m_Log, "xrEnumerateSwapchainFormats(count)", result) || formatCount == 0)
                return false;

            std::vector<int64_t> formats(formatCount);
            result = m_Xr.xrEnumerateSwapchainFormats(m_Session, formatCount, &formatCount, formats.data());
            if (!Succeeded(m_Log, "xrEnumerateSwapchainFormats(data)", result) || formatCount == 0)
                return false;

            const std::array<int64_t, 4> preferredFormats = {
                DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            };

            int64_t selectedFormat = formats[0];
            for (int64_t preferred : preferredFormats)
            {
                if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
                {
                    selectedFormat = preferred;
                    break;
                }
            }

            m_Log.Print("Selected swapchain DXGI_FORMAT=%lld", static_cast<long long>(selectedFormat));

            m_Eyes.resize(2);
            for (size_t eye = 0; eye < m_Eyes.size(); ++eye)
            {
                EyeSwapchain& swapchain = m_Eyes[eye];
                const XrViewConfigurationView& view = m_ViewConfigs[eye];
                swapchain.width = view.recommendedImageRectWidth;
                swapchain.height = view.recommendedImageRectHeight;
                swapchain.format = selectedFormat;

                XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
                createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                createInfo.format = selectedFormat;
                createInfo.sampleCount = std::max(1u, view.recommendedSwapchainSampleCount);
                createInfo.width = swapchain.width;
                createInfo.height = swapchain.height;
                createInfo.faceCount = 1;
                createInfo.arraySize = 1;
                createInfo.mipCount = 1;

                result = m_Xr.xrCreateSwapchain(m_Session, &createInfo, &swapchain.handle);
                if (!Succeeded(m_Log, "xrCreateSwapchain", result) || swapchain.handle == XR_NULL_HANDLE)
                    return false;

                uint32_t imageCount = 0;
                result = m_Xr.xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
                if (!Succeeded(m_Log, "xrEnumerateSwapchainImages(count)", result) || imageCount == 0)
                    return false;

                swapchain.images.assign(imageCount, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
                result = m_Xr.xrEnumerateSwapchainImages(
                    swapchain.handle,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
                if (!Succeeded(m_Log, "xrEnumerateSwapchainImages(data)", result) || imageCount == 0)
                    return false;

                swapchain.images.resize(imageCount);
                swapchain.renderTargetViews.resize(imageCount);

                for (uint32_t image = 0; image < imageCount; ++image)
                {
                    D3D11_TEXTURE2D_DESC textureDesc{};
                    swapchain.images[image].texture->GetDesc(&textureDesc);

                    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
                    rtvDesc.Format = static_cast<DXGI_FORMAT>(selectedFormat);
                    rtvDesc.ViewDimension = textureDesc.SampleDesc.Count > 1
                        ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                        : D3D11_RTV_DIMENSION_TEXTURE2D;

                    HRESULT hr = m_Device->CreateRenderTargetView(
                        swapchain.images[image].texture,
                        &rtvDesc,
                        swapchain.renderTargetViews[image].GetAddressOf());
                    if (FAILED(hr))
                    {
                        m_Log.Print(
                            "CreateRenderTargetView eye=%zu image=%u failed: 0x%08X textureFormat=%u selectedFormat=%lld bind=0x%X samples=%u array=%u",
                            eye,
                            image,
                            static_cast<unsigned int>(hr),
                            static_cast<unsigned int>(textureDesc.Format),
                            static_cast<long long>(selectedFormat),
                            static_cast<unsigned int>(textureDesc.BindFlags),
                            static_cast<unsigned int>(textureDesc.SampleDesc.Count),
                            static_cast<unsigned int>(textureDesc.ArraySize));
                        return false;
                    }
                }

                m_Log.Print(
                    "Created eye %zu swapchain %ux%u images=%u",
                    eye,
                    swapchain.width,
                    swapchain.height,
                    imageCount);
            }

            return true;
        }

        bool PollEvents(bool& shouldExit)
        {
            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            while (true)
            {
                event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
                const XrResult result = m_Xr.xrPollEvent(m_Instance, &event);
                if (result == XR_EVENT_UNAVAILABLE)
                    return true;
                if (!Succeeded(m_Log, "xrPollEvent", result))
                    return false;

                switch (event.type)
                {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    m_Log.Print("OpenXR instance loss pending");
                    shouldExit = true;
                    return true;

                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                {
                    const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    m_SessionState = changed.state;
                    m_Log.Print("Session state changed: %d", static_cast<int>(m_SessionState));

                    if (m_SessionState == XR_SESSION_STATE_READY && !m_SessionRunning)
                    {
                        XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (!Succeeded(m_Log, "xrBeginSession", m_Xr.xrBeginSession(m_Session, &beginInfo)))
                            return false;
                        m_SessionRunning = true;
                        m_Log.Print("OpenXR session running");
                        m_Bridge.Update(L4D2VROpenXrBridgeStatus::SessionRunning, 0, 0, "OpenXR session running");
                    }
                    else if (m_SessionState == XR_SESSION_STATE_STOPPING && m_SessionRunning)
                    {
                        if (!Succeeded(m_Log, "xrEndSession", m_Xr.xrEndSession(m_Session)))
                            return false;
                        m_SessionRunning = false;
                        m_Log.Print("OpenXR session stopped");
                    }
                    else if (m_SessionState == XR_SESSION_STATE_EXITING ||
                        m_SessionState == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        shouldExit = true;
                    }
                    break;
                }

                default:
                    break;
                }
            }
        }

        bool ParentStillAlive() const
        {
            if (!m_ParentProcess)
                return true;

            const DWORD wait = WaitForSingleObject(m_ParentProcess, 0);
            return wait == WAIT_TIMEOUT;
        }

        void DestroyImportedGameEye(VulkanGameEyeTexture& eye)
        {
            if (eye.view != VK_NULL_HANDLE && m_Vk.vkDestroyImageView)
                m_Vk.vkDestroyImageView(m_VkDevice, eye.view, nullptr);
            if (eye.image != VK_NULL_HANDLE && m_Vk.vkDestroyImage)
                m_Vk.vkDestroyImage(m_VkDevice, eye.image, nullptr);
            if (eye.memory != VK_NULL_HANDLE && m_Vk.vkFreeMemory)
                m_Vk.vkFreeMemory(m_VkDevice, eye.memory, nullptr);
            eye = VulkanGameEyeTexture{};
        }

        bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags preferred, uint32_t& outIndex) const
        {
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) &&
                    (m_MemoryProperties.memoryTypes[i].propertyFlags & preferred) == preferred)
                {
                    outIndex = i;
                    return true;
                }
            }

            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if (typeBits & (1u << i))
                {
                    outIndex = i;
                    return true;
                }
            }

            return false;
        }

        uint32_t BuildMutableViewFormats(VkFormat format, std::array<VkFormat, 2>& formats) const
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
                formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
                formats[1] = VK_FORMAT_B8G8R8A8_SRGB;
                return 2;
            case VK_FORMAT_R8G8B8A8_UNORM:
                formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
                formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
                return 2;
            default:
                formats[0] = format;
                return 1;
            }
        }

        VkSampleCountFlagBits SampleCountFromDesc(uint32_t sampleCount) const
        {
            switch (sampleCount)
            {
            case 1: return VK_SAMPLE_COUNT_1_BIT;
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 4: return VK_SAMPLE_COUNT_4_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            case 16: return VK_SAMPLE_COUNT_16_BIT;
            default: return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        bool ImportGameEyeTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& desc, uint32_t generation)
        {
            VulkanGameEyeTexture& eye = m_GameEyes[eyeIndex];
            if (eye.generation == generation &&
                eye.kmtHandle == desc.kmtHandle &&
                eye.image != VK_NULL_HANDLE)
            {
                return true;
            }

            DestroyImportedGameEye(eye);

            if (!desc.valid || desc.kmtHandle == 0 || desc.width == 0 || desc.height == 0)
            {
                m_Log.Print("Shared texture eye=%u is invalid for Vulkan import", eyeIndex);
                return false;
            }

            const VkExternalMemoryHandleTypeFlagBits handleType = desc.handleType != 0
                ? static_cast<VkExternalMemoryHandleTypeFlagBits>(desc.handleType)
                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
            const VkFormat format = static_cast<VkFormat>(desc.format);
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(desc.kmtHandle));

            std::array<VkFormat, 2> viewFormats = {};
            const uint32_t viewFormatCount = BuildMutableViewFormats(format, viewFormats);

            VkImageFormatListCreateInfo formatList{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
            formatList.viewFormatCount = viewFormatCount;
            formatList.pViewFormats = viewFormats.data();

            VkExternalMemoryImageCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            externalInfo.pNext = viewFormatCount > 1 ? &formatList : nullptr;
            externalInfo.handleTypes = handleType;

            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.pNext = &externalInfo;
            imageInfo.flags = viewFormatCount > 1 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { desc.width, desc.height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = SampleCountFromDesc(desc.sampleCount);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage importedImage = VK_NULL_HANDLE;
            VkResult result = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            if (result != VK_SUCCESS && imageInfo.flags != 0)
            {
                imageInfo.flags = 0;
                externalInfo.pNext = nullptr;
                result = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            }

            if (result != VK_SUCCESS || importedImage == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateImage import eye=%u handle=0x%llX format=%u size=%ux%u failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle), desc.format, desc.width, desc.height,
                    VkResultName(result), static_cast<int>(result));
                return false;
            }

            VkMemoryRequirements memoryRequirements{};
            m_Vk.vkGetImageMemoryRequirements(m_VkDevice, importedImage, &memoryRequirements);

            VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
            result = m_Vk.vkGetMemoryWin32HandlePropertiesKHR(m_VkDevice, handleType, handle, &handleProperties);
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkGetMemoryWin32HandlePropertiesKHR eye=%u handle=0x%llX type=0x%X failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle), desc.handleType,
                    VkResultName(result), static_cast<int>(result));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            uint32_t memoryTypeIndex = 0;
            const uint32_t typeBits = memoryRequirements.memoryTypeBits & handleProperties.memoryTypeBits;
            if (!FindMemoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryTypeIndex))
            {
                m_Log.Print("No compatible memory type for imported eye=%u memBits=0x%X handleBits=0x%X",
                    eyeIndex, memoryRequirements.memoryTypeBits, handleProperties.memoryTypeBits);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            importInfo.handleType = handleType;
            importInfo.handle = handle;

            VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = importedImage;

            VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocInfo.pNext = &dedicatedInfo;
            allocInfo.allocationSize = memoryRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            VkDeviceMemory importedMemory = VK_NULL_HANDLE;
            result = m_Vk.vkAllocateMemory(m_VkDevice, &allocInfo, nullptr, &importedMemory);
            if (result != VK_SUCCESS || importedMemory == VK_NULL_HANDLE)
            {
                m_Log.Print("vkAllocateMemory import eye=%u handle=0x%llX size=%llu type=%u failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle),
                    static_cast<unsigned long long>(memoryRequirements.size), memoryTypeIndex,
                    VkResultName(result), static_cast<int>(result));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            result = m_Vk.vkBindImageMemory(m_VkDevice, importedImage, importedMemory, 0);
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkBindImageMemory import eye=%u handle=0x%llX failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle), VkResultName(result), static_cast<int>(result));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            eye.generation = generation;
            eye.kmtHandle = desc.kmtHandle;
            eye.width = desc.width;
            eye.height = desc.height;
            eye.format = format;
            eye.image = importedImage;
            eye.memory = importedMemory;
            eye.layout = VK_IMAGE_LAYOUT_GENERAL;
            eye.uMin = std::clamp(desc.uMin, 0.0f, 1.0f);
            eye.vMin = std::clamp(desc.vMin, 0.0f, 1.0f);
            eye.uMax = std::clamp(desc.uMax, 0.0f, 1.0f);
            eye.vMax = std::clamp(desc.vMax, 0.0f, 1.0f);
            eye.renderFovXDeg = (std::isfinite(desc.renderFovXDeg) && desc.renderFovXDeg > 1.0f && desc.renderFovXDeg < 179.0f)
                ? desc.renderFovXDeg
                : 90.0f;
            eye.renderAspect = (std::isfinite(desc.renderAspect) && desc.renderAspect > 0.1f && desc.renderAspect < 10.0f)
                ? desc.renderAspect
                : ((desc.height > 0) ? (static_cast<float>(desc.width) / static_cast<float>(desc.height)) : 1.0f);
            if (eye.uMax <= eye.uMin)
            {
                eye.uMin = 0.0f;
                eye.uMax = 1.0f;
            }
            if (eye.vMax <= eye.vMin)
            {
                eye.vMin = 0.0f;
                eye.vMax = 1.0f;
            }

            m_Log.Print("Imported Vulkan shared eye texture eye=%u gen=%u handle=0x%llX image=0x%llX size=%ux%u format=%u bounds=(%.3f %.3f %.3f %.3f) projection=(fovX=%.2f aspect=%.4f) memorySize=%llu",
                eyeIndex, generation, static_cast<unsigned long long>(desc.kmtHandle),
                static_cast<unsigned long long>(desc.image), desc.width, desc.height, desc.format,
                eye.uMin, eye.vMin, eye.uMax, eye.vMax,
                eye.renderFovXDeg, eye.renderAspect,
                static_cast<unsigned long long>(memoryRequirements.size));
            return true;
        }

        bool ImportSharedGameTexturesIfNeeded()
        {
            const uint32_t generation = m_Bridge.SharedTextureGeneration();
            if (generation == 0)
                return false;

            for (uint32_t eyeIndex = 0; eyeIndex < L4D2VR_OPENXR_EYE_COUNT; ++eyeIndex)
            {
                const L4D2VROpenXrSharedTextureDesc desc = m_Bridge.SharedTexture(eyeIndex);
                if (!ImportGameEyeTexture(eyeIndex, desc, generation))
                    return false;
            }

            return true;
        }

        void DestroyImportedGameEye(VulkanGameEyeTexture& eye)
        {
            if (eye.view != VK_NULL_HANDLE && m_Vk.vkDestroyImageView)
                m_Vk.vkDestroyImageView(m_VkDevice, eye.view, nullptr);
            if (eye.image != VK_NULL_HANDLE && m_Vk.vkDestroyImage)
                m_Vk.vkDestroyImage(m_VkDevice, eye.image, nullptr);
            if (eye.memory != VK_NULL_HANDLE && m_Vk.vkFreeMemory)
                m_Vk.vkFreeMemory(m_VkDevice, eye.memory, nullptr);
            eye = VulkanGameEyeTexture{};
        }

        bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags preferred, uint32_t& outIndex) const
        {
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) &&
                    (m_MemoryProperties.memoryTypes[i].propertyFlags & preferred) == preferred)
                {
                    outIndex = i;
                    return true;
                }
            }

            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if (typeBits & (1u << i))
                {
                    outIndex = i;
                    return true;
                }
            }

            return false;
        }

        uint32_t BuildMutableViewFormats(VkFormat format, std::array<VkFormat, 2>& formats) const
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
                formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
                formats[1] = VK_FORMAT_B8G8R8A8_SRGB;
                return 2;
            case VK_FORMAT_R8G8B8A8_UNORM:
                formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
                formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
                return 2;
            default:
                formats[0] = format;
                return 1;
            }
        }

        VkSampleCountFlagBits SampleCountFromDesc(uint32_t sampleCount) const
        {
            switch (sampleCount)
            {
            case 1: return VK_SAMPLE_COUNT_1_BIT;
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 4: return VK_SAMPLE_COUNT_4_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            case 16: return VK_SAMPLE_COUNT_16_BIT;
            default: return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        bool ImportGameEyeTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& desc, uint32_t generation)
        {
            VulkanGameEyeTexture& eye = m_GameEyes[eyeIndex];
            if (eye.generation == generation &&
                eye.kmtHandle == desc.kmtHandle &&
                eye.image != VK_NULL_HANDLE)
            {
                return true;
            }

            DestroyImportedGameEye(eye);

            if (!desc.valid || desc.kmtHandle == 0 || desc.width == 0 || desc.height == 0)
            {
                m_Log.Print("Shared texture eye=%u is invalid for Vulkan import", eyeIndex);
                return false;
            }

            const VkExternalMemoryHandleTypeFlagBits handleType = desc.handleType != 0
                ? static_cast<VkExternalMemoryHandleTypeFlagBits>(desc.handleType)
                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
            const VkFormat format = static_cast<VkFormat>(desc.format);
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(desc.kmtHandle));

            std::array<VkFormat, 2> viewFormats = {};
            const uint32_t viewFormatCount = BuildMutableViewFormats(format, viewFormats);

            VkImageFormatListCreateInfo formatList{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
            formatList.viewFormatCount = viewFormatCount;
            formatList.pViewFormats = viewFormats.data();

            VkExternalMemoryImageCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            externalInfo.pNext = viewFormatCount > 1 ? &formatList : nullptr;
            externalInfo.handleTypes = handleType;

            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.pNext = &externalInfo;
            imageInfo.flags = viewFormatCount > 1 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { desc.width, desc.height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = SampleCountFromDesc(desc.sampleCount);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage importedImage = VK_NULL_HANDLE;
            VkResult result = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            if (result != VK_SUCCESS && imageInfo.flags != 0)
            {
                imageInfo.flags = 0;
                externalInfo.pNext = nullptr;
                result = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            }

            if (result != VK_SUCCESS || importedImage == VK_NULL_HANDLE)
            {
                m_Log.Print(
                    "vkCreateImage import eye=%u handle=0x%llX format=%u size=%ux%u failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    desc.format,
                    desc.width,
                    desc.height,
                    VkResultName(result),
                    static_cast<int>(result));
                return false;
            }

            VkMemoryRequirements memoryRequirements{};
            m_Vk.vkGetImageMemoryRequirements(m_VkDevice, importedImage, &memoryRequirements);

            VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
            result = m_Vk.vkGetMemoryWin32HandlePropertiesKHR(m_VkDevice, handleType, handle, &handleProperties);
            if (result != VK_SUCCESS)
            {
                m_Log.Print(
                    "vkGetMemoryWin32HandlePropertiesKHR eye=%u handle=0x%llX type=0x%X failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    desc.handleType,
                    VkResultName(result),
                    static_cast<int>(result));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            uint32_t memoryTypeIndex = 0;
            const uint32_t typeBits = memoryRequirements.memoryTypeBits & handleProperties.memoryTypeBits;
            if (!FindMemoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryTypeIndex))
            {
                m_Log.Print(
                    "No compatible memory type for imported eye=%u memBits=0x%X handleBits=0x%X",
                    eyeIndex,
                    memoryRequirements.memoryTypeBits,
                    handleProperties.memoryTypeBits);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            importInfo.handleType = handleType;
            importInfo.handle = handle;

            VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = importedImage;

            VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocInfo.pNext = &dedicatedInfo;
            allocInfo.allocationSize = memoryRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            VkDeviceMemory importedMemory = VK_NULL_HANDLE;
            result = m_Vk.vkAllocateMemory(m_VkDevice, &allocInfo, nullptr, &importedMemory);
            if (result != VK_SUCCESS || importedMemory == VK_NULL_HANDLE)
            {
                m_Log.Print(
                    "vkAllocateMemory import eye=%u handle=0x%llX size=%llu type=%u failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    static_cast<unsigned long long>(memoryRequirements.size),
                    memoryTypeIndex,
                    VkResultName(result),
                    static_cast<int>(result));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            result = m_Vk.vkBindImageMemory(m_VkDevice, importedImage, importedMemory, 0);
            if (result != VK_SUCCESS)
            {
                m_Log.Print(
                    "vkBindImageMemory import eye=%u handle=0x%llX failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    VkResultName(result),
                    static_cast<int>(result));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            eye.generation = generation;
            eye.kmtHandle = desc.kmtHandle;
            eye.width = desc.width;
            eye.height = desc.height;
            eye.format = format;
            eye.image = importedImage;
            eye.memory = importedMemory;
            eye.layout = VK_IMAGE_LAYOUT_GENERAL;

            m_Log.Print(
                "Imported Vulkan shared eye texture eye=%u gen=%u handle=0x%llX image=0x%llX size=%ux%u format=%u memorySize=%llu",
                eyeIndex,
                generation,
                static_cast<unsigned long long>(desc.kmtHandle),
                static_cast<unsigned long long>(desc.image),
                desc.width,
                desc.height,
                desc.format,
                static_cast<unsigned long long>(memoryRequirements.size));
            return true;
        }

        bool ImportSharedGameTexturesIfNeeded()
        {
            const uint32_t generation = m_Bridge.SharedTextureGeneration();
            if (generation == 0)
                return false;

            for (uint32_t eyeIndex = 0; eyeIndex < L4D2VR_OPENXR_EYE_COUNT; ++eyeIndex)
            {
                const L4D2VROpenXrSharedTextureDesc desc = m_Bridge.SharedTexture(eyeIndex);
                if (!ImportGameEyeTexture(eyeIndex, desc, generation))
                    return false;
            }

            return true;
        }

        void CmdTransitionImage(
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkAccessFlags srcAccess,
            VkAccessFlags dstAccess,
            VkPipelineStageFlags srcStage,
            VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            m_Vk.vkCmdPipelineBarrier(
                m_CommandBuffer,
                srcStage,
                dstStage,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &barrier);
        }

        bool RenderEye(uint32_t eyeIndex, uint32_t frameIndex)
        {
            VulkanEyeSwapchain& eye = m_Eyes[eyeIndex];

            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t imageIndex = 0;
            XrResult xrResult = m_Xr.xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex);
            if (!Succeeded(m_Log, "xrAcquireSwapchainImage", xrResult))
                return false;

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            xrResult = m_Xr.xrWaitSwapchainImage(eye.handle, &waitInfo);
            if (!Succeeded(m_Log, "xrWaitSwapchainImage", xrResult))
                return false;

            const VkImage dstImage = eye.images[imageIndex].image;
            if (dstImage == VK_NULL_HANDLE)
            {
                m_Log.Print("OpenXR swapchain image eye=%u index=%u is null", eyeIndex, imageIndex);
                return false;
            }

            VkResult vkResult = m_Vk.vkResetCommandBuffer(m_CommandBuffer, 0);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkResetCommandBuffer failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResult = m_Vk.vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkBeginCommandBuffer failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            CmdTransitionImage(
                dstImage,
                eye.layouts[imageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);

            if (m_Bridge.HasState())
            {
                const VulkanGameEyeTexture& source = m_GameEyes[eyeIndex];
                if (source.image == VK_NULL_HANDLE)
                    return false;

                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = 0;
                blit.srcSubresource.baseArrayLayer = 0;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[0] = { 0, 0, 0 };
                blit.srcOffsets[1] = {
                    static_cast<int32_t>(source.width),
                    static_cast<int32_t>(source.height),
                    1
                };
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = 0;
                blit.dstSubresource.baseArrayLayer = 0;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[0] = { 0, 0, 0 };
                blit.dstOffsets[1] = {
                    static_cast<int32_t>(eye.width),
                    static_cast<int32_t>(eye.height),
                    1
                };

                m_Vk.vkCmdBlitImage(
                    m_CommandBuffer,
                    source.image,
                    source.layout,
                    dstImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &blit,
                    VK_FILTER_LINEAR);
            }
            else
            {
                const float pulse = static_cast<float>((frameIndex % 180) / 179.0);
                const VkClearColorValue leftColor = { { 0.02f + 0.20f * pulse, 0.05f, 0.85f, 1.0f } };
                const VkClearColorValue rightColor = { { 0.85f, 0.05f + 0.20f * pulse, 0.02f, 1.0f } };
                const VkClearColorValue color = eyeIndex == 0 ? leftColor : rightColor;

                VkImageSubresourceRange range{};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.baseMipLevel = 0;
                range.levelCount = 1;
                range.baseArrayLayer = 0;
                range.layerCount = 1;
                m_Vk.vkCmdClearColorImage(
                    m_CommandBuffer,
                    dstImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &color,
                    1,
                    &range);
            }

            CmdTransitionImage(
                dstImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            eye.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            vkResult = m_Vk.vkEndCommandBuffer(m_CommandBuffer);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkEndCommandBuffer failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CommandBuffer;
            vkResult = m_Vk.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkQueueSubmit failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            vkResult = m_Vk.vkQueueWaitIdle(m_GraphicsQueue);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkQueueWaitIdle failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrResult = m_Xr.xrReleaseSwapchainImage(eye.handle, &releaseInfo);
            if (!Succeeded(m_Log, "xrReleaseSwapchainImage", xrResult))
                return false;

            return true;
        }

        void DestroyImportedGameEye(VulkanGameEyeTexture& eye)
        {
            if (eye.image != VK_NULL_HANDLE && m_Vk.vkDestroyImage)
            {
                m_Vk.vkDestroyImage(m_VkDevice, eye.image, nullptr);
                eye.image = VK_NULL_HANDLE;
            }
            if (eye.memory != VK_NULL_HANDLE && m_Vk.vkFreeMemory)
            {
                m_Vk.vkFreeMemory(m_VkDevice, eye.memory, nullptr);
                eye.memory = VK_NULL_HANDLE;
            }
            eye = VulkanGameEyeTexture{};
        }

        bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags preferred, uint32_t& outIndex) const
        {
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) &&
                    (m_MemoryProperties.memoryTypes[i].propertyFlags & preferred) == preferred)
                {
                    outIndex = i;
                    return true;
                }
            }

            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if (typeBits & (1u << i))
                {
                    outIndex = i;
                    return true;
                }
            }

            return false;
        }

        uint32_t BuildMutableViewFormats(VkFormat format, std::array<VkFormat, 2>& formats) const
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
                formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
                formats[1] = VK_FORMAT_B8G8R8A8_SRGB;
                return 2;
            case VK_FORMAT_R8G8B8A8_UNORM:
                formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
                formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
                return 2;
            default:
                formats[0] = format;
                return 1;
            }
        }

        VkSampleCountFlagBits SampleCountFromDesc(uint32_t sampleCount) const
        {
            switch (sampleCount)
            {
            case 1: return VK_SAMPLE_COUNT_1_BIT;
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 4: return VK_SAMPLE_COUNT_4_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            case 16: return VK_SAMPLE_COUNT_16_BIT;
            default: return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        bool ImportGameEyeTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& desc, uint32_t generation)
        {
            VulkanGameEyeTexture& eye = m_GameEyes[eyeIndex];
            if (eye.generation == generation &&
                eye.kmtHandle == desc.kmtHandle &&
                eye.image != VK_NULL_HANDLE)
            {
                return true;
            }

            DestroyImportedGameEye(eye);

            if (!desc.valid || desc.kmtHandle == 0 || desc.width == 0 || desc.height == 0)
            {
                m_Log.Print("Shared texture eye=%u is invalid for Vulkan import", eyeIndex);
                return false;
            }

            const VkExternalMemoryHandleTypeFlagBits handleType = desc.handleType != 0
                ? static_cast<VkExternalMemoryHandleTypeFlagBits>(desc.handleType)
                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
            const VkFormat format = static_cast<VkFormat>(desc.format);
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(desc.kmtHandle));

            std::array<VkFormat, 2> viewFormats = {};
            const uint32_t viewFormatCount = BuildMutableViewFormats(format, viewFormats);

            VkImageFormatListCreateInfo formatList{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
            formatList.viewFormatCount = viewFormatCount;
            formatList.pViewFormats = viewFormats.data();

            VkExternalMemoryImageCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            externalInfo.pNext = viewFormatCount > 1 ? &formatList : nullptr;
            externalInfo.handleTypes = handleType;

            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.pNext = &externalInfo;
            imageInfo.flags = viewFormatCount > 1 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { desc.width, desc.height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = SampleCountFromDesc(desc.sampleCount);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage importedImage = VK_NULL_HANDLE;
            VkResult result = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            if (result != VK_SUCCESS && imageInfo.flags != 0)
            {
                imageInfo.flags = 0;
                externalInfo.pNext = nullptr;
                result = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            }

            if (result != VK_SUCCESS || importedImage == VK_NULL_HANDLE)
            {
                m_Log.Print(
                    "vkCreateImage import eye=%u handle=0x%llX format=%u size=%ux%u failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    desc.format,
                    desc.width,
                    desc.height,
                    VkResultName(result),
                    static_cast<int>(result));
                return false;
            }

            VkMemoryRequirements memoryRequirements{};
            m_Vk.vkGetImageMemoryRequirements(m_VkDevice, importedImage, &memoryRequirements);

            VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
            result = m_Vk.vkGetMemoryWin32HandlePropertiesKHR(m_VkDevice, handleType, handle, &handleProperties);
            if (result != VK_SUCCESS)
            {
                m_Log.Print(
                    "vkGetMemoryWin32HandlePropertiesKHR eye=%u handle=0x%llX type=0x%X failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    desc.handleType,
                    VkResultName(result),
                    static_cast<int>(result));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            uint32_t memoryTypeIndex = 0;
            const uint32_t typeBits = memoryRequirements.memoryTypeBits & handleProperties.memoryTypeBits;
            if (!FindMemoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryTypeIndex))
            {
                m_Log.Print(
                    "No compatible memory type for imported eye=%u memBits=0x%X handleBits=0x%X",
                    eyeIndex,
                    memoryRequirements.memoryTypeBits,
                    handleProperties.memoryTypeBits);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            importInfo.handleType = handleType;
            importInfo.handle = handle;

            VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = importedImage;

            VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocInfo.pNext = &dedicatedInfo;
            allocInfo.allocationSize = memoryRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            VkDeviceMemory importedMemory = VK_NULL_HANDLE;
            result = m_Vk.vkAllocateMemory(m_VkDevice, &allocInfo, nullptr, &importedMemory);
            if (result != VK_SUCCESS || importedMemory == VK_NULL_HANDLE)
            {
                m_Log.Print(
                    "vkAllocateMemory import eye=%u handle=0x%llX size=%llu type=%u failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    static_cast<unsigned long long>(memoryRequirements.size),
                    memoryTypeIndex,
                    VkResultName(result),
                    static_cast<int>(result));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            result = m_Vk.vkBindImageMemory(m_VkDevice, importedImage, importedMemory, 0);
            if (result != VK_SUCCESS)
            {
                m_Log.Print(
                    "vkBindImageMemory import eye=%u handle=0x%llX failed: %s (%d)",
                    eyeIndex,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    VkResultName(result),
                    static_cast<int>(result));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            eye.generation = generation;
            eye.kmtHandle = desc.kmtHandle;
            eye.width = desc.width;
            eye.height = desc.height;
            eye.format = format;
            eye.image = importedImage;
            eye.memory = importedMemory;
            eye.layout = VK_IMAGE_LAYOUT_GENERAL;

            m_Log.Print(
                "Imported Vulkan shared eye texture eye=%u gen=%u handle=0x%llX image=0x%llX size=%ux%u format=%u memorySize=%llu",
                eyeIndex,
                generation,
                static_cast<unsigned long long>(desc.kmtHandle),
                static_cast<unsigned long long>(desc.image),
                desc.width,
                desc.height,
                desc.format,
                static_cast<unsigned long long>(memoryRequirements.size));
            return true;
        }

        bool ImportSharedGameTexturesIfNeeded()
        {
            const uint32_t generation = m_Bridge.SharedTextureGeneration();
            if (generation == 0)
                return false;

            for (uint32_t eyeIndex = 0; eyeIndex < L4D2VR_OPENXR_EYE_COUNT; ++eyeIndex)
            {
                const L4D2VROpenXrSharedTextureDesc desc = m_Bridge.SharedTexture(eyeIndex);
                if (!ImportGameEyeTexture(eyeIndex, desc, generation))
                    return false;
            }

            return true;
        }

        void CmdTransitionImage(
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkAccessFlags srcAccess,
            VkAccessFlags dstAccess,
            VkPipelineStageFlags srcStage,
            VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            m_Vk.vkCmdPipelineBarrier(
                m_CommandBuffer,
                srcStage,
                dstStage,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &barrier);
        }

        bool RenderEye(uint32_t eyeIndex, uint32_t frameIndex)
        {
            VulkanEyeSwapchain& eye = m_Eyes[eyeIndex];

            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t imageIndex = 0;
            XrResult xrResult = m_Xr.xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex);
            if (!Succeeded(m_Log, "xrAcquireSwapchainImage", xrResult))
                return false;

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            xrResult = m_Xr.xrWaitSwapchainImage(eye.handle, &waitInfo);
            if (!Succeeded(m_Log, "xrWaitSwapchainImage", xrResult))
                return false;

            const VkImage dstImage = eye.images[imageIndex].image;
            if (dstImage == VK_NULL_HANDLE)
            {
                m_Log.Print("OpenXR swapchain image eye=%u index=%u is null", eyeIndex, imageIndex);
                return false;
            }

            VkResult vkResult = m_Vk.vkResetCommandBuffer(m_CommandBuffer, 0);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkResetCommandBuffer failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResult = m_Vk.vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkBeginCommandBuffer failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            CmdTransitionImage(
                dstImage,
                eye.layouts[imageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);

            if (m_Bridge.HasState())
            {
                if (!ImportSharedGameTexturesIfNeeded())
                    return false;

                const VulkanGameEyeTexture& source = m_GameEyes[eyeIndex];
                if (source.image == VK_NULL_HANDLE)
                    return false;

                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = 0;
                blit.srcSubresource.baseArrayLayer = 0;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[0] = { 0, 0, 0 };
                blit.srcOffsets[1] = {
                    static_cast<int32_t>(source.width),
                    static_cast<int32_t>(source.height),
                    1
                };
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = 0;
                blit.dstSubresource.baseArrayLayer = 0;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[0] = { 0, 0, 0 };
                blit.dstOffsets[1] = {
                    static_cast<int32_t>(eye.width),
                    static_cast<int32_t>(eye.height),
                    1
                };

                m_Vk.vkCmdBlitImage(
                    m_CommandBuffer,
                    source.image,
                    source.layout,
                    dstImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &blit,
                    VK_FILTER_LINEAR);
            }
            else
            {
                const float pulse = static_cast<float>((frameIndex % 180) / 179.0);
                const VkClearColorValue leftColor = { { 0.02f + 0.20f * pulse, 0.05f, 0.85f, 1.0f } };
                const VkClearColorValue rightColor = { { 0.85f, 0.05f + 0.20f * pulse, 0.02f, 1.0f } };
                const VkClearColorValue color = eyeIndex == 0 ? leftColor : rightColor;

                VkImageSubresourceRange range{};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.baseMipLevel = 0;
                range.levelCount = 1;
                range.baseArrayLayer = 0;
                range.layerCount = 1;
                m_Vk.vkCmdClearColorImage(
                    m_CommandBuffer,
                    dstImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &color,
                    1,
                    &range);
            }

            CmdTransitionImage(
                dstImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            eye.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            vkResult = m_Vk.vkEndCommandBuffer(m_CommandBuffer);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkEndCommandBuffer failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CommandBuffer;
            vkResult = m_Vk.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkQueueSubmit failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            vkResult = m_Vk.vkQueueWaitIdle(m_GraphicsQueue);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkQueueWaitIdle failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrResult = m_Xr.xrReleaseSwapchainImage(eye.handle, &releaseInfo);
            if (!Succeeded(m_Log, "xrReleaseSwapchainImage", xrResult))
                return false;

            return true;
        }


        int FrameLoop(const Options& options)
        {
            const ULONGLONG startTicks = GetTickCount64();
            uint32_t submittedFrames = 0;
            bool shouldExit = false;
            const bool requireSharedTextures = m_Bridge.HasState();
            uint32_t lastLoggedSharedTextureGeneration = 0;
            ULONGLONG lastWaitingTextureLog = 0;

            m_Log.Print(
                "Entering frame loop targetFrames=%u waitReadySeconds=%u parentPid=%lu requireSharedTextures=%u",
                options.targetFrames,
                options.waitReadySeconds,
                static_cast<unsigned long>(options.parentPid),
                requireSharedTextures ? 1u : 0u);

            while (!shouldExit)
            {
                if (!ParentStillAlive())
                {
                    m_Log.Print("Parent process exited");
                    shouldExit = true;
                    break;
                }

                if (!PollEvents(shouldExit))
                    return 20;
                if (shouldExit)
                    break;

                if (!m_SessionRunning)
                {
                    const ULONGLONG elapsedMs = GetTickCount64() - startTicks;
                    if (options.waitReadySeconds > 0 && elapsedMs > static_cast<ULONGLONG>(options.waitReadySeconds) * 1000ull)
                    {
                        m_Log.Print("Timed out waiting for XR_SESSION_STATE_READY");
                        return 21;
                    }

                    Sleep(10);
                    continue;
                }

                const bool sharedTexturesReady = !requireSharedTextures || m_Bridge.SharedTexturesReady();
                if (requireSharedTextures && !sharedTexturesReady)
                {
                    const ULONGLONG now = GetTickCount64();
                    if (now - lastWaitingTextureLog > 1000ull)
                    {
                        m_Log.Print("Waiting for shared game eye textures");
                        m_Bridge.Update(L4D2VROpenXrBridgeStatus::WaitingForSharedTextures, 0, submittedFrames, "waiting for shared game eye textures");
                        lastWaitingTextureLog = now;
                    }

                }

                if (sharedTexturesReady)
                {
                    const uint32_t sharedTextureGeneration = m_Bridge.SharedTextureGeneration();
                    if (requireSharedTextures && sharedTextureGeneration != lastLoggedSharedTextureGeneration)
                    {
                        const auto left = m_Bridge.SharedTexture(L4D2VR_OPENXR_EYE_LEFT);
                        const auto right = m_Bridge.SharedTexture(L4D2VR_OPENXR_EYE_RIGHT);
                        m_Log.Print(
                            "Shared game eye textures ready gen=%u L(handle=0x%llX image=0x%llX %ux%u fmt=%u) R(handle=0x%llX image=0x%llX %ux%u fmt=%u)",
                            sharedTextureGeneration,
                            static_cast<unsigned long long>(left.kmtHandle),
                            static_cast<unsigned long long>(left.image),
                            left.width,
                            left.height,
                            left.format,
                            static_cast<unsigned long long>(right.kmtHandle),
                            static_cast<unsigned long long>(right.image),
                            right.width,
                            right.height,
                            right.format);
                        lastLoggedSharedTextureGeneration = sharedTextureGeneration;
                    }

                    if (requireSharedTextures && (!EnsureBlitPipeline() || !OpenSharedGameTexturesIfNeeded()))
                        return 28;
                }

                XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
                XrFrameState frameState{ XR_TYPE_FRAME_STATE };
                XrResult result = m_Xr.xrWaitFrame(m_Session, &waitInfo, &frameState);
                if (!Succeeded(m_Log, "xrWaitFrame", result))
                    return 22;

                XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
                result = m_Xr.xrBeginFrame(m_Session, &beginInfo);
                if (!Succeeded(m_Log, "xrBeginFrame", result))
                    return 23;

                m_InputBridge.UpdateFrame(frameState.predictedDisplayTime, m_Bridge, m_Log);

                bool layerReady = false;
                std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
                std::vector<XrView> locatedViews(m_ViewConfigs.size(), XrView{ XR_TYPE_VIEW });

                if (frameState.shouldRender)
                {
                    XrViewState viewState{ XR_TYPE_VIEW_STATE };
                    uint32_t locatedCount = 0;
                    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
                    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = m_AppSpace;

                    result = m_Xr.xrLocateViews(
                        m_Session,
                        &locateInfo,
                        &viewState,
                        static_cast<uint32_t>(locatedViews.size()),
                        &locatedCount,
                        locatedViews.data());
                    if (!Succeeded(m_Log, "xrLocateViews", result))
                        return 24;

                    static bool s_loggedRuntimeViewConfig = false;
                    if (locatedCount >= 2 && RuntimeViewConfigIsReady(m_SessionState))
                    {
                        const L4D2VROpenXrRuntimeViewConfigDesc runtimeViewConfig =
                            BuildRuntimeViewConfig(m_ViewConfigs, locatedViews, locatedCount);
                        if (!s_loggedRuntimeViewConfig && runtimeViewConfig.valid)
                        {
                            s_loggedRuntimeViewConfig = true;
                            LogRuntimeViewConfig(m_Log, "D3D11", runtimeViewConfig);
                        }
                        m_Bridge.PublishRuntimeViewConfig(runtimeViewConfig);
                    }

                    const bool poseValid =
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                        locatedCount >= 2;

                    if (poseValid)
                    {
                        if (sharedTexturesReady)
                        {
                            for (uint32_t eye = 0; eye < 2; ++eye)
                            {
                                if (!RenderEye(eye, submittedFrames))
                                    return 25;

                                projectionViews[eye] = XrCompositionLayerProjectionView{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                                const uint32_t imageEye = SelectProjectionImageEye(options, eye);
                                const uint32_t projectionViewEye = SelectProjectionViewEye(options, eye);
                                projectionViews[eye].pose = BuildProjectionPose(locatedViews, locatedCount, projectionViewEye);
                                projectionViews[eye].fov = locatedViews[projectionViewEye].fov;
                                if (options.mirrorProjectionHorizontal)
                                    projectionViews[eye].fov = MirrorProjectionFovHorizontal(projectionViews[eye].fov);
                                projectionViews[eye].subImage.swapchain = m_Eyes[imageEye].handle;
                                projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    static_cast<int32_t>(m_Eyes[imageEye].width),
                                    static_cast<int32_t>(m_Eyes[imageEye].height)
                                };
                            }

                            if (options.swapProjectionViewOrder)
                            {
                                std::swap(projectionViews[0], projectionViews[1]);
                                static bool s_loggedProjectionViewOrderSwap = false;
                                if (!s_loggedProjectionViewOrderSwap)
                                {
                                    s_loggedProjectionViewOrderSwap = true;
                                    m_Log.Print(
                                        "OpenXR projection view-order swap active: projectionViews[0] and projectionViews[1] are swapped immediately before xrEndFrame");
                                }
                            }

                            layerReady = true;
                        }
                    }
                }

                XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
                layer.space = m_AppSpace;
                layer.viewCount = 2;
                layer.views = projectionViews.data();

                const XrCompositionLayerBaseHeader* layers[] = {
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
                };
                const bool submitProjectionLayer = layerReady && !options.disableProjectionLayer;
                if (layerReady && options.disableProjectionLayer)
                {
                    static bool s_loggedProjectionLayerDisabled = false;
                    if (!s_loggedProjectionLayerDisabled)
                    {
                        s_loggedProjectionLayerDisabled = true;
                        m_Log.Print("[OpenXR][ProjectionLayer] disabled by option; xrEndFrame will submit no projection layer in test path");
                    }
                }

                XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = m_BlendMode;
                endInfo.layerCount = submitProjectionLayer ? 1u : 0u;
                endInfo.layers = submitProjectionLayer ? layers : nullptr;

                result = m_Xr.xrEndFrame(m_Session, &endInfo);
                if (!Succeeded(m_Log, "xrEndFrame", result))
                    return 26;

                if (submitProjectionLayer)
                {
                    ++submittedFrames;
                    m_Bridge.Update(L4D2VROpenXrBridgeStatus::SubmittedFrame, 0, submittedFrames, "OpenXR projection frame submitted");
                    if (submittedFrames == 1 || (submittedFrames % 60) == 0)
                        m_Log.Print("Submitted OpenXR projection frame %u", submittedFrames);
                }

                if (options.targetFrames > 0 && submittedFrames >= options.targetFrames)
                {
                    m_Log.Print("Completed target OpenXR submitted frames: %u", submittedFrames);
                    m_Bridge.Update(L4D2VROpenXrBridgeStatus::Completed, 0, submittedFrames, "target OpenXR frames completed");
                    break;
                }
            }

            if (submittedFrames > 0)
            {
                m_Bridge.Update(L4D2VROpenXrBridgeStatus::Completed, 0, submittedFrames, "OpenXR submit loop completed");
                return 0;
            }

            m_Bridge.Update(L4D2VROpenXrBridgeStatus::Failed, 27, submittedFrames, "no OpenXR frames submitted");
            return 27;
        }

        bool EnsureBlitPipeline()
        {
            if (m_CopyVertexShader && m_CopyPixelShader && m_CopySampler)
                return true;

            const char* vertexShaderSource = R"(
struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VSOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

            const char* pixelShaderSource = R"(
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return sourceTexture.Sample(sourceSampler, uv);
}
)";

            auto compileShader = [&](const char* source, const char* target, ComPtr<ID3DBlob>& outBlob) -> bool
                {
                    ComPtr<ID3DBlob> errorBlob;
                    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
                    const HRESULT hr = D3DCompile(
                        source,
                        std::strlen(source),
                        nullptr,
                        nullptr,
                        nullptr,
                        "main",
                        target,
                        flags,
                        0,
                        outBlob.GetAddressOf(),
                        errorBlob.GetAddressOf());
                    if (FAILED(hr))
                    {
                        m_Log.Print(
                            "D3DCompile %s failed: 0x%08X %s",
                            target,
                            static_cast<unsigned int>(hr),
                            errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "");
                        return false;
                    }
                    return true;
                };

            ComPtr<ID3DBlob> vertexBlob;
            ComPtr<ID3DBlob> pixelBlob;
            if (!compileShader(vertexShaderSource, "vs_5_0", vertexBlob) ||
                !compileShader(pixelShaderSource, "ps_5_0", pixelBlob))
                return false;

            HRESULT hr = m_Device->CreateVertexShader(
                vertexBlob->GetBufferPointer(),
                vertexBlob->GetBufferSize(),
                nullptr,
                m_CopyVertexShader.GetAddressOf());
            if (FAILED(hr))
            {
                m_Log.Print("CreateVertexShader failed: 0x%08X", static_cast<unsigned int>(hr));
                return false;
            }

            hr = m_Device->CreatePixelShader(
                pixelBlob->GetBufferPointer(),
                pixelBlob->GetBufferSize(),
                nullptr,
                m_CopyPixelShader.GetAddressOf());
            if (FAILED(hr))
            {
                m_Log.Print("CreatePixelShader failed: 0x%08X", static_cast<unsigned int>(hr));
                return false;
            }

            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.MinLOD = 0.0f;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            hr = m_Device->CreateSamplerState(&samplerDesc, m_CopySampler.GetAddressOf());
            if (FAILED(hr))
            {
                m_Log.Print("CreateSamplerState failed: 0x%08X", static_cast<unsigned int>(hr));
                return false;
            }

            m_Log.Print("Created D3D11 shared-texture blit pipeline");
            return true;
        }

        bool OpenSharedGameTexturesIfNeeded()
        {
            const uint32_t generation = m_Bridge.SharedTextureGeneration();
            if (generation == 0)
                return false;

            for (uint32_t eyeIndex = 0; eyeIndex < L4D2VR_OPENXR_EYE_COUNT; ++eyeIndex)
            {
                const L4D2VROpenXrSharedTextureDesc desc = m_Bridge.SharedTexture(eyeIndex);
                if (!desc.valid || desc.kmtHandle == 0)
                {
                    m_Log.Print("Shared texture eye=%u is not valid", eyeIndex);
                    return false;
                }

                GameEyeTexture& eye = m_GameEyes[eyeIndex];
                if (eye.generation == generation &&
                    eye.kmtHandle == desc.kmtHandle &&
                    eye.shaderResourceView)
                {
                    continue;
                }

                eye = GameEyeTexture{};

                ComPtr<ID3D11Texture2D> sharedTexture;
                HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(desc.kmtHandle));
                HRESULT hr = m_Device->OpenSharedResource(
                    handle,
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(sharedTexture.GetAddressOf()));
                if (FAILED(hr))
                {
                    m_Log.Print(
                        "OpenSharedResource eye=%u handle=0x%llX type=0x%X image=0x%llX size=%ux%u format=%u failed: 0x%08X",
                        eyeIndex,
                        static_cast<unsigned long long>(desc.kmtHandle),
                        desc.handleType,
                        static_cast<unsigned long long>(desc.image),
                        desc.width,
                        desc.height,
                        desc.format,
                        static_cast<unsigned int>(hr));
                    return false;
                }

                D3D11_TEXTURE2D_DESC textureDesc{};
                sharedTexture->GetDesc(&textureDesc);

                ComPtr<ID3D11ShaderResourceView> shaderResourceView;
                hr = m_Device->CreateShaderResourceView(sharedTexture.Get(), nullptr, shaderResourceView.GetAddressOf());
                if (FAILED(hr))
                {
                    m_Log.Print(
                        "CreateShaderResourceView eye=%u nativeFormat=%u bind=0x%X misc=0x%X failed: 0x%08X",
                        eyeIndex,
                        static_cast<unsigned int>(textureDesc.Format),
                        static_cast<unsigned int>(textureDesc.BindFlags),
                        static_cast<unsigned int>(textureDesc.MiscFlags),
                        static_cast<unsigned int>(hr));
                    return false;
                }

                eye.generation = generation;
                eye.kmtHandle = desc.kmtHandle;
                eye.width = textureDesc.Width;
                eye.height = textureDesc.Height;
                eye.format = static_cast<uint32_t>(textureDesc.Format);
                eye.texture = sharedTexture;
                eye.shaderResourceView = shaderResourceView;

                m_Log.Print(
                    "Opened shared game eye texture eye=%u gen=%u handle=0x%llX native=%ux%u format=%u bind=0x%X misc=0x%X",
                    eyeIndex,
                    generation,
                    static_cast<unsigned long long>(desc.kmtHandle),
                    textureDesc.Width,
                    textureDesc.Height,
                    static_cast<unsigned int>(textureDesc.Format),
                    static_cast<unsigned int>(textureDesc.BindFlags),
                    static_cast<unsigned int>(textureDesc.MiscFlags));
            }

            return true;
        }

        bool RenderSharedEye(uint32_t eyeIndex, ID3D11RenderTargetView* renderTargetView, uint32_t width, uint32_t height)
        {
            if (!EnsureBlitPipeline() || !OpenSharedGameTexturesIfNeeded())
                return false;

            ID3D11ShaderResourceView* sourceView = m_GameEyes[eyeIndex].shaderResourceView.Get();
            if (!sourceView)
                return false;

            const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            m_Context->ClearRenderTargetView(renderTargetView, clearColor);

            D3D11_VIEWPORT viewport{};
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width = static_cast<float>(width);
            viewport.Height = static_cast<float>(height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;

            ID3D11RenderTargetView* targets[] = { renderTargetView };
            ID3D11ShaderResourceView* sourceViews[] = { sourceView };
            ID3D11SamplerState* samplers[] = { m_CopySampler.Get() };

            m_Context->OMSetRenderTargets(1, targets, nullptr);
            m_Context->RSSetViewports(1, &viewport);
            m_Context->IASetInputLayout(nullptr);
            m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_Context->VSSetShader(m_CopyVertexShader.Get(), nullptr, 0);
            m_Context->PSSetShader(m_CopyPixelShader.Get(), nullptr, 0);
            m_Context->PSSetShaderResources(0, 1, sourceViews);
            m_Context->PSSetSamplers(0, 1, samplers);
            m_Context->Draw(3, 0);

            ID3D11ShaderResourceView* nullSourceViews[] = { nullptr };
            ID3D11RenderTargetView* nullTargets[] = { nullptr };
            m_Context->PSSetShaderResources(0, 1, nullSourceViews);
            m_Context->OMSetRenderTargets(1, nullTargets, nullptr);
            return true;
        }

        bool RenderEye(uint32_t eyeIndex, uint32_t frameIndex)
        {
            EyeSwapchain& eye = m_Eyes[eyeIndex];

            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t imageIndex = 0;
            XrResult result = m_Xr.xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex);
            if (!Succeeded(m_Log, "xrAcquireSwapchainImage", result))
                return false;

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            result = m_Xr.xrWaitSwapchainImage(eye.handle, &waitInfo);
            if (!Succeeded(m_Log, "xrWaitSwapchainImage", result))
                return false;

            bool renderSucceeded = false;
            if (m_Bridge.HasState())
            {
                renderSucceeded = RenderSharedEye(
                    eyeIndex,
                    eye.renderTargetViews[imageIndex].Get(),
                    eye.width,
                    eye.height);
            }
            else
            {
                const float pulse = static_cast<float>((frameIndex % 180) / 179.0);
                const float leftColor[4] = { 0.02f + 0.20f * pulse, 0.05f, 0.85f, 1.0f };
                const float rightColor[4] = { 0.85f, 0.05f + 0.20f * pulse, 0.02f, 1.0f };
                const float* color = (eyeIndex == 0) ? leftColor : rightColor;

                m_Context->ClearRenderTargetView(eye.renderTargetViews[imageIndex].Get(), color);
                renderSucceeded = true;
            }
            m_Context->Flush();

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            result = m_Xr.xrReleaseSwapchainImage(eye.handle, &releaseInfo);
            if (!Succeeded(m_Log, "xrReleaseSwapchainImage", result))
                return false;

            return renderSucceeded;
        }

        static HANDLE OpenParentProcess(DWORD parentPid)
        {
            if (parentPid == 0)
                return nullptr;
            return OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        }

        void Shutdown()
        {
            for (EyeSwapchain& eye : m_Eyes)
            {
                eye.renderTargetViews.clear();
                eye.images.clear();
                if (eye.handle != XR_NULL_HANDLE && m_Xr.xrDestroySwapchain)
                {
                    m_Xr.xrDestroySwapchain(eye.handle);
                    eye.handle = XR_NULL_HANDLE;
                }
            }

            if (m_AppSpace != XR_NULL_HANDLE && m_Xr.xrDestroySpace)
            {
                m_Xr.xrDestroySpace(m_AppSpace);
                m_AppSpace = XR_NULL_HANDLE;
            }

            if (m_Session != XR_NULL_HANDLE && m_Xr.xrDestroySession)
            {
                m_Xr.xrDestroySession(m_Session);
                m_Session = XR_NULL_HANDLE;
            }

            for (GameEyeTexture& eye : m_GameEyes)
                eye = GameEyeTexture{};
            m_CopySampler.Reset();
            m_CopyPixelShader.Reset();
            m_CopyVertexShader.Reset();
            m_Context.Reset();
            m_Device.Reset();

            if (m_Instance != XR_NULL_HANDLE && m_Xr.xrDestroyInstance)
            {
                m_Xr.xrDestroyInstance(m_Instance);
                m_Instance = XR_NULL_HANDLE;
            }

            if (m_Loader)
            {
                FreeLibrary(m_Loader);
                m_Loader = nullptr;
            }

            if (m_ParentProcess)
            {
                CloseHandle(m_ParentProcess);
                m_ParentProcess = nullptr;
            }
        }

        Logger& m_Log;
        BridgeWriter m_Bridge;
        HMODULE m_Loader = nullptr;
        XrDispatch m_Xr{};
        XrInstance m_Instance = XR_NULL_HANDLE;
        XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
        XrSession m_Session = XR_NULL_HANDLE;
        XrSpace m_AppSpace = XR_NULL_HANDLE;
        XrSessionState m_SessionState = XR_SESSION_STATE_UNKNOWN;
        XrEnvironmentBlendMode m_BlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        bool m_SessionRunning = false;
        ComPtr<ID3D11Device> m_Device;
        ComPtr<ID3D11DeviceContext> m_Context;
        ComPtr<ID3D11VertexShader> m_CopyVertexShader;
        ComPtr<ID3D11PixelShader> m_CopyPixelShader;
        ComPtr<ID3D11SamplerState> m_CopySampler;
        HANDLE m_ParentProcess = nullptr;
        std::vector<XrViewConfigurationView> m_ViewConfigs;
        std::vector<EyeSwapchain> m_Eyes;
        std::array<GameEyeTexture, L4D2VR_OPENXR_EYE_COUNT> m_GameEyes;
    };

#endif
    const std::array<OpenXrInputBridge::BooleanActionDef, 31>& OpenXrInputBridge::BooleanDefs()
    {
        static const std::array<BooleanActionDef, 31> defs =
        {{
            { L4D2VROpenXrActionId::ActivateVR, "activate_vr", "Activate VR" },
            { L4D2VROpenXrActionId::Jump, "jump", "Jump" },
            { L4D2VROpenXrActionId::Use, "use", "Use" },
            { L4D2VROpenXrActionId::Teleport, "teleport", "Teleport" },
            { L4D2VROpenXrActionId::NextItem, "next_item", "Next Item" },
            { L4D2VROpenXrActionId::PrevItem, "prev_item", "Previous Item" },
            { L4D2VROpenXrActionId::ResetPosition, "reset_position", "Reset Position" },
            { L4D2VROpenXrActionId::Flashlight, "flashlight", "Flashlight" },
            { L4D2VROpenXrActionId::InventoryGripLeft, "inventory_grip_left", "Inventory Grip Left" },
            { L4D2VROpenXrActionId::InventoryGripRight, "inventory_grip_right", "Inventory Grip Right" },
            { L4D2VROpenXrActionId::InventoryQuickSwitch, "inventory_quick_switch", "Inventory Quick Switch" },
            { L4D2VROpenXrActionId::SpecialInfectedAutoAimToggle, "special_infected_auto_aim", "Special Infected Auto Aim" },
            { L4D2VROpenXrActionId::SpecialInfectedDodgeToggle, "special_infected_dodge", "Special Infected Dodge" },
            { L4D2VROpenXrActionId::LedgeGuardToggle, "ledge_guard", "Ledge Guard" },
            { L4D2VROpenXrActionId::EffectiveAttackRangeAutoFireToggle, "effective_range_auto_fire", "Effective Range Auto Fire" },
            { L4D2VROpenXrActionId::SpeechToText, "speech_to_text", "Speech To Text" },
            { L4D2VROpenXrActionId::MenuSelect, "menu_select", "Menu Select" },
            { L4D2VROpenXrActionId::MenuBack, "menu_back", "Menu Back" },
            { L4D2VROpenXrActionId::MenuUp, "menu_up", "Menu Up" },
            { L4D2VROpenXrActionId::MenuDown, "menu_down", "Menu Down" },
            { L4D2VROpenXrActionId::MenuLeft, "menu_left", "Menu Left" },
            { L4D2VROpenXrActionId::MenuRight, "menu_right", "Menu Right" },
            { L4D2VROpenXrActionId::Spray, "spray", "Spray" },
            { L4D2VROpenXrActionId::Scoreboard, "scoreboard", "Scoreboard" },
            { L4D2VROpenXrActionId::ShowHUD, "show_hud", "Show HUD" },
            { L4D2VROpenXrActionId::Pause, "pause", "Pause" },
            { L4D2VROpenXrActionId::NonVRServerMovementAngleToggle, "nonvr_server_movement_angle", "Non-VR Server Movement Angle" },
            { L4D2VROpenXrActionId::ScopeToggle, "scope_toggle", "Scope Toggle" },
            { L4D2VROpenXrActionId::FriendlyFireBlockToggle, "friendly_fire_block", "Friendly Fire Block" },
            { L4D2VROpenXrActionId::CustomAction1, "custom_action_1", "Custom Action 1" },
            { L4D2VROpenXrActionId::CustomAction2, "custom_action_2", "Custom Action 2" },
        }};
        return defs;
    }

    const std::array<OpenXrInputBridge::BooleanActionDef, 3>& OpenXrInputBridge::ExtraBooleanDefs()
    {
        static const std::array<BooleanActionDef, 3> defs =
        {{
            { L4D2VROpenXrActionId::CustomAction3, "custom_action_3", "Custom Action 3" },
            { L4D2VROpenXrActionId::CustomAction4, "custom_action_4", "Custom Action 4" },
            { L4D2VROpenXrActionId::CustomAction5, "custom_action_5", "Custom Action 5" },
        }};
        return defs;
    }

    const std::array<OpenXrInputBridge::FloatDigitalActionDef, 4>& OpenXrInputBridge::FloatDigitalDefs()
    {
        static const std::array<FloatDigitalActionDef, 4> defs =
        {{
            { L4D2VROpenXrActionId::PrimaryAttack, "primary_attack", "Primary Attack", 0.45f },
            { L4D2VROpenXrActionId::SecondaryAttack, "secondary_attack", "Secondary Attack", 0.45f },
            { L4D2VROpenXrActionId::Reload, "reload", "Reload", 0.45f },
            { L4D2VROpenXrActionId::Crouch, "crouch", "Crouch", 0.45f },
        }};
        return defs;
    }

    const std::array<OpenXrInputBridge::AnalogActionDef, 2>& OpenXrInputBridge::AnalogDefs()
    {
        static const std::array<AnalogActionDef, 2> defs =
        {{
            { L4D2VROpenXrActionId::Walk, "walk", "Walk" },
            { L4D2VROpenXrActionId::Turn, "turn", "Turn" },
        }};
        return defs;
    }

    bool OpenXrInputBridge::Path(const char* text, XrPath& out, Logger& log)
    {
        const XrResult result = m_Xr->xrStringToPath(m_Instance, text, &out);
        if (XR_FAILED(result))
        {
            log.Print("xrStringToPath(%s) failed: %s (%d)", text, XrResultName(result), static_cast<int>(result));
            out = XR_NULL_PATH;
            return false;
        }
        return true;
    }

    bool OpenXrInputBridge::TryPath(const char* text, XrPath& out)
    {
        const XrResult result = m_Xr->xrStringToPath(m_Instance, text, &out);
        if (XR_FAILED(result))
        {
            out = XR_NULL_PATH;
            return false;
        }
        return true;
    }

    std::string OpenXrInputBridge::PathToString(XrPath path) const
    {
        if (path == XR_NULL_PATH)
            return "<none>";
        if (!m_Xr || !m_Xr->xrPathToString)
            return "<path-to-string-unavailable>";

        char buffer[XR_MAX_PATH_LENGTH] = {};
        uint32_t count = 0;
        const XrResult result = m_Xr->xrPathToString(m_Instance, path, ARRAYSIZE(buffer), &count, buffer);
        if (XR_FAILED(result))
        {
            char fallback[64] = {};
            std::snprintf(fallback, sizeof(fallback), "<path 0x%llX>", static_cast<unsigned long long>(path));
            return fallback;
        }
        return buffer;
    }

    bool OpenXrInputBridge::CreateAction(XrActionSet set, XrActionType type, const char* name, const char* localizedName, XrAction& out, Logger& log)
    {
        XrActionCreateInfo createInfo{ XR_TYPE_ACTION_CREATE_INFO };
        std::snprintf(createInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
        createInfo.actionType = type;
        std::snprintf(createInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localizedName);
        const XrResult result = m_Xr->xrCreateAction(set, &createInfo, &out);
        if (XR_FAILED(result) || out == XR_NULL_HANDLE)
        {
            log.Print("xrCreateAction(%s) failed: %s (%d)", name, XrResultName(result), static_cast<int>(result));
            return false;
        }
        return true;
    }

    bool OpenXrInputBridge::CreateSubactionAction(XrActionSet set, XrActionType type, const char* name, const char* localizedName, XrAction& out, Logger& log)
    {
        XrActionCreateInfo createInfo{ XR_TYPE_ACTION_CREATE_INFO };
        std::snprintf(createInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
        createInfo.actionType = type;
        createInfo.countSubactionPaths = static_cast<uint32_t>(m_HandPaths.size());
        createInfo.subactionPaths = m_HandPaths.data();
        std::snprintf(createInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localizedName);
        const XrResult result = m_Xr->xrCreateAction(set, &createInfo, &out);
        if (XR_FAILED(result) || out == XR_NULL_HANDLE)
        {
            log.Print("xrCreateAction(%s) failed: %s (%d)", name, XrResultName(result), static_cast<int>(result));
            return false;
        }
        return true;
    }

    bool OpenXrInputBridge::CreatePoseAction(Logger& log)
    {
        return CreateSubactionAction(m_BaseActionSet, XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand Pose", m_HandPoseAction, log);
    }

    bool OpenXrInputBridge::CreateHapticAction(Logger& log)
    {
        return CreateSubactionAction(m_BaseActionSet, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic", m_HapticAction, log);
    }

    bool OpenXrInputBridge::CreateBooleanActions(Logger& log)
    {
        for (const BooleanActionDef& def : BooleanDefs())
        {
            if (!CreateAction(m_MainActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, def.name, def.localizedName, m_BooleanActions[Index(def.id)], log))
                return false;
        }
        for (const BooleanActionDef& def : ExtraBooleanDefs())
        {
            if (!CreateAction(m_MainActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, def.name, def.localizedName, m_BooleanActions[Index(def.id)], log))
                return false;
        }
        return true;
    }

    bool OpenXrInputBridge::CreateFloatDigitalActions(Logger& log)
    {
        for (const FloatDigitalActionDef& def : FloatDigitalDefs())
        {
            if (!CreateAction(m_MainActionSet, XR_ACTION_TYPE_FLOAT_INPUT, def.name, def.localizedName, m_FloatDigitalActions[Index(def.id)], log))
                return false;
        }
        return true;
    }

    bool OpenXrInputBridge::CreateAnalogActions(Logger& log)
    {
        for (const AnalogActionDef& def : AnalogDefs())
        {
            if (!CreateAction(m_MainActionSet, XR_ACTION_TYPE_VECTOR2F_INPUT, def.name, def.localizedName, m_AnalogActions[Index(def.id)], log))
                return false;
        }
        return true;
    }

    bool OpenXrInputBridge::InitializeInstance(XrDispatch& xr, XrInstance instance, Logger& log)
    {
        m_Xr = &xr;
        m_Instance = instance;

        if (!Path("/user/hand/left", m_HandPaths[L4D2VR_OPENXR_HAND_LEFT], log) ||
            !Path("/user/hand/right", m_HandPaths[L4D2VR_OPENXR_HAND_RIGHT], log))
        {
            return false;
        }

        XrActionSetCreateInfo mainSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
        std::snprintf(mainSetInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "main");
        std::snprintf(mainSetInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Main");
        mainSetInfo.priority = 0;
        if (!Succeeded(log, "xrCreateActionSet(main)", xr.xrCreateActionSet(instance, &mainSetInfo, &m_MainActionSet)))
            return false;

        XrActionSetCreateInfo baseSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
        std::snprintf(baseSetInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "base");
        std::snprintf(baseSetInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Base");
        baseSetInfo.priority = 1;
        if (!Succeeded(log, "xrCreateActionSet(base)", xr.xrCreateActionSet(instance, &baseSetInfo, &m_BaseActionSet)))
            return false;

        if (!CreatePoseAction(log) ||
            !CreateHapticAction(log) ||
            !CreateBooleanActions(log) ||
            !CreateFloatDigitalActions(log) ||
            !CreateAnalogActions(log))
        {
            return false;
        }

        SuggestBindings(log);
        return true;
    }

    void OpenXrInputBridge::AddBinding(std::vector<XrActionSuggestedBinding>& bindings, XrAction action, const char* bindingPath)
    {
        if (action == XR_NULL_HANDLE || !bindingPath || !bindingPath[0])
            return;

        XrPath path = XR_NULL_PATH;
        if (TryPath(bindingPath, path))
            bindings.push_back(XrActionSuggestedBinding{ action, path });
    }

    void OpenXrInputBridge::SuggestProfile(Logger& log, const char* profilePath, const std::vector<XrActionSuggestedBinding>& bindings)
    {
        if (bindings.empty())
            return;

        XrPath profile = XR_NULL_PATH;
        if (!TryPath(profilePath, profile))
            return;

        XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        const XrResult result = m_Xr->xrSuggestInteractionProfileBindings(m_Instance, &suggested);
        if (XR_FAILED(result))
        {
            log.Print(
                "xrSuggestInteractionProfileBindings(%s) failed/nonfatal: %s (%d)",
                profilePath,
                XrResultName(result),
                static_cast<int>(result));
        }
    }

    void OpenXrInputBridge::AddPoseAndHapticBindings(std::vector<XrActionSuggestedBinding>& b)
    {
        AddBinding(b, m_HandPoseAction, "/user/hand/left/input/grip/pose");
        AddBinding(b, m_HandPoseAction, "/user/hand/right/input/grip/pose");
        AddBinding(b, m_HapticAction, "/user/hand/left/output/haptic");
        AddBinding(b, m_HapticAction, "/user/hand/right/output/haptic");
    }

    void OpenXrInputBridge::AddStickAndTriggerBindings(
        std::vector<XrActionSuggestedBinding>& b,
        const char* stickName)
    {
        const std::string leftStick = std::string("/user/hand/left/input/") + stickName;
        const std::string rightStick = std::string("/user/hand/right/input/") + stickName;

        AddBinding(b, m_AnalogActions[Index(L4D2VROpenXrActionId::Walk)], (leftStick + "/value").c_str());
        AddBinding(b, m_AnalogActions[Index(L4D2VROpenXrActionId::Turn)], (rightStick + "/value").c_str());
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::ResetPosition)], (leftStick + "/click").c_str());
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Flashlight)], (rightStick + "/click").c_str());

        AddBinding(b, m_FloatDigitalActions[Index(L4D2VROpenXrActionId::PrimaryAttack)], "/user/hand/right/input/trigger/value");
        AddBinding(b, m_FloatDigitalActions[Index(L4D2VROpenXrActionId::SecondaryAttack)], "/user/hand/left/input/trigger/value");
    }

    void OpenXrInputBridge::AddGripValueBindings(
        std::vector<XrActionSuggestedBinding>& b,
        const char* gripValueName)
    {
        const std::string leftGripValue = std::string("/user/hand/left/input/") + gripValueName;
        const std::string rightGripValue = std::string("/user/hand/right/input/") + gripValueName;
        AddBinding(b, m_FloatDigitalActions[Index(L4D2VROpenXrActionId::Reload)], leftGripValue.c_str());
        AddBinding(b, m_FloatDigitalActions[Index(L4D2VROpenXrActionId::Crouch)], rightGripValue.c_str());
    }

    void OpenXrInputBridge::AddTouchFaceButtonBindings(std::vector<XrActionSuggestedBinding>& b)
    {
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Use)], "/user/hand/right/input/b/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::ActivateVR)], "/user/hand/right/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Jump)], "/user/hand/right/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::MenuSelect)], "/user/hand/right/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::MenuBack)], "/user/hand/right/input/b/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Scoreboard)], "/user/hand/left/input/x/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Pause)], "/user/hand/left/input/y/click");
    }

    void OpenXrInputBridge::AddIndexFaceButtonBindings(std::vector<XrActionSuggestedBinding>& b)
    {
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Use)], "/user/hand/right/input/b/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::ActivateVR)], "/user/hand/right/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Jump)], "/user/hand/right/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::MenuSelect)], "/user/hand/right/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::MenuBack)], "/user/hand/right/input/b/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Scoreboard)], "/user/hand/left/input/a/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Pause)], "/user/hand/left/input/b/click");
    }

    void OpenXrInputBridge::AddMenuButtonBindings(std::vector<XrActionSuggestedBinding>& b)
    {
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::ActivateVR)], "/user/hand/right/input/menu/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::MenuSelect)], "/user/hand/right/input/menu/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::MenuBack)], "/user/hand/left/input/menu/click");
        AddBinding(b, m_BooleanActions[Index(L4D2VROpenXrActionId::Pause)], "/user/hand/left/input/menu/click");
    }

    void OpenXrInputBridge::SuggestBindings(Logger& log)
    {
        const char* poseProfiles[] =
        {
            "/interaction_profiles/oculus/touch_controller",
            "/interaction_profiles/facebook/touch_controller_pro",
            "/interaction_profiles/meta/touch_controller_plus",
            "/interaction_profiles/valve/index_controller",
            "/interaction_profiles/microsoft/motion_controller",
            "/interaction_profiles/htc/vive_cosmos_controller",
            "/interaction_profiles/htc/vive_focus3_controller",
            "/interaction_profiles/htc/vive_controller",
            "/interaction_profiles/bytedance/pico4_controller",
            "/interaction_profiles/khr/simple_controller",
        };

        for (const char* profile : poseProfiles)
        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            SuggestProfile(log, profile, b);
        }

        const char* touchLikeProfiles[] =
        {
            "/interaction_profiles/oculus/touch_controller",
            "/interaction_profiles/facebook/touch_controller_pro",
            "/interaction_profiles/meta/touch_controller_plus",
            "/interaction_profiles/bytedance/pico4_controller",
        };
        for (const char* profile : touchLikeProfiles)
        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            AddStickAndTriggerBindings(b, "thumbstick");
            AddGripValueBindings(b, "squeeze/value");
            AddTouchFaceButtonBindings(b);
            SuggestProfile(log, profile, b);
        }

        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            AddStickAndTriggerBindings(b, "thumbstick");
            AddGripValueBindings(b, "squeeze/value");
            AddIndexFaceButtonBindings(b);
            SuggestProfile(log, "/interaction_profiles/valve/index_controller", b);
        }
        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            AddStickAndTriggerBindings(b, "thumbstick");
            AddMenuButtonBindings(b);
            SuggestProfile(log, "/interaction_profiles/microsoft/motion_controller", b);
        }
        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            AddStickAndTriggerBindings(b, "joystick");
            AddGripValueBindings(b, "grip/value");
            AddMenuButtonBindings(b);
            SuggestProfile(log, "/interaction_profiles/htc/vive_cosmos_controller", b);
            SuggestProfile(log, "/interaction_profiles/htc/vive_focus3_controller", b);
        }
        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            AddStickAndTriggerBindings(b, "trackpad");
            AddMenuButtonBindings(b);
            SuggestProfile(log, "/interaction_profiles/htc/vive_controller", b);
        }
        {
            std::vector<XrActionSuggestedBinding> b;
            AddPoseAndHapticBindings(b);
            AddMenuButtonBindings(b);
            SuggestProfile(log, "/interaction_profiles/khr/simple_controller", b);
        }
    }

    void OpenXrInputBridge::LogCurrentInteractionProfiles(Logger& log)
    {
        if (!m_Xr || !m_Xr->xrGetCurrentInteractionProfile || m_Session == XR_NULL_HANDLE)
            return;

        for (uint32_t hand = 0; hand < L4D2VR_OPENXR_HAND_COUNT; ++hand)
        {
            XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
            const XrResult result = m_Xr->xrGetCurrentInteractionProfile(m_Session, m_HandPaths[hand], &state);
            if (XR_FAILED(result))
                continue;

            if (state.interactionProfile != m_LastInteractionProfiles[hand])
            {
                m_LastInteractionProfiles[hand] = state.interactionProfile;
                log.Print(
                    "OpenXR current interaction profile %s: %s",
                    hand == L4D2VR_OPENXR_HAND_LEFT ? "left" : "right",
                    PathToString(state.interactionProfile).c_str());
            }
        }
    }

    bool OpenXrInputBridge::InitializeSession(XrSession session, XrSpace appSpace, bool handTrackingEnabled, Logger& log)
    {
        if (!m_Xr || session == XR_NULL_HANDLE || appSpace == XR_NULL_HANDLE)
            return false;

        m_Session = session;
        m_AppSpace = appSpace;
        m_LastInteractionProfiles.fill(static_cast<XrPath>(~0ull));
        m_PoseInactiveLogged.fill(false);

        for (uint32_t hand = 0; hand < L4D2VR_OPENXR_HAND_COUNT; ++hand)
        {
            XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            spaceInfo.action = m_HandPoseAction;
            spaceInfo.subactionPath = m_HandPaths[hand];
            spaceInfo.poseInActionSpace.orientation.w = 1.0f;
            if (!Succeeded(log, "xrCreateActionSpace(hand_pose)", m_Xr->xrCreateActionSpace(session, &spaceInfo, &m_HandSpaces[hand])))
                return false;
        }

        const XrActionSet actionSets[] = { m_MainActionSet, m_BaseActionSet };
        XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attachInfo.countActionSets = ARRAYSIZE(actionSets);
        attachInfo.actionSets = actionSets;
        if (!Succeeded(log, "xrAttachSessionActionSets", m_Xr->xrAttachSessionActionSets(session, &attachInfo)))
            return false;

        m_FeatureFlags = L4D2VR_OPENXR_INPUT_FEATURE_CONTROLLERS | L4D2VR_OPENXR_INPUT_FEATURE_HAPTICS;
        if (handTrackingEnabled && m_Xr->xrCreateHandTrackerEXT && m_Xr->xrLocateHandJointsEXT)
        {
            for (uint32_t hand = 0; hand < L4D2VR_OPENXR_HAND_COUNT; ++hand)
            {
                XrHandTrackerCreateInfoEXT createInfo{ XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
                createInfo.hand = (hand == L4D2VR_OPENXR_HAND_LEFT) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
                createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                const XrResult result = m_Xr->xrCreateHandTrackerEXT(session, &createInfo, &m_HandTrackers[hand]);
                if (XR_FAILED(result))
                {
                    m_HandTrackers[hand] = XR_NULL_HANDLE;
                    log.Print("xrCreateHandTrackerEXT(%s) failed: %s (%d)",
                        hand == L4D2VR_OPENXR_HAND_LEFT ? "left" : "right",
                        XrResultName(result),
                        static_cast<int>(result));
                }
            }

            if (m_HandTrackers[L4D2VR_OPENXR_HAND_LEFT] != XR_NULL_HANDLE ||
                m_HandTrackers[L4D2VR_OPENXR_HAND_RIGHT] != XR_NULL_HANDLE)
            {
                m_FeatureFlags |= L4D2VR_OPENXR_INPUT_FEATURE_HAND_TRACKING;
            }
        }

        m_SessionInitialized = true;
        log.Print("OpenXR input initialized features=0x%X", m_FeatureFlags);
        return true;
    }

    void OpenXrInputBridge::Shutdown()
    {
        if (m_Xr)
        {
            for (XrHandTrackerEXT& tracker : m_HandTrackers)
            {
                if (tracker != XR_NULL_HANDLE && m_Xr->xrDestroyHandTrackerEXT)
                {
                    m_Xr->xrDestroyHandTrackerEXT(tracker);
                    tracker = XR_NULL_HANDLE;
                }
            }
            for (XrSpace& space : m_HandSpaces)
            {
                if (space != XR_NULL_HANDLE && m_Xr->xrDestroySpace)
                {
                    m_Xr->xrDestroySpace(space);
                    space = XR_NULL_HANDLE;
                }
            }
            DestroyAction(m_HandPoseAction);
            DestroyAction(m_HapticAction);
            for (XrAction& action : m_BooleanActions)
                DestroyAction(action);
            for (XrAction& action : m_FloatDigitalActions)
                DestroyAction(action);
            for (XrAction& action : m_AnalogActions)
                DestroyAction(action);
            if (m_BaseActionSet != XR_NULL_HANDLE && m_Xr->xrDestroyActionSet)
            {
                m_Xr->xrDestroyActionSet(m_BaseActionSet);
                m_BaseActionSet = XR_NULL_HANDLE;
            }
            if (m_MainActionSet != XR_NULL_HANDLE && m_Xr->xrDestroyActionSet)
            {
                m_Xr->xrDestroyActionSet(m_MainActionSet);
                m_MainActionSet = XR_NULL_HANDLE;
            }
        }

        m_SessionInitialized = false;
        m_Xr = nullptr;
        m_Instance = XR_NULL_HANDLE;
        m_Session = XR_NULL_HANDLE;
        m_AppSpace = XR_NULL_HANDLE;
        m_LastInteractionProfiles.fill(XR_NULL_PATH);
        m_PoseInactiveLogged.fill(false);
    }

    void OpenXrInputBridge::DestroyAction(XrAction& action)
    {
        if (action != XR_NULL_HANDLE && m_Xr && m_Xr->xrDestroyAction)
        {
            m_Xr->xrDestroyAction(action);
            action = XR_NULL_HANDLE;
        }
    }

    void OpenXrInputBridge::UpdateFrame(XrTime displayTime, BridgeWriter& bridge, Logger& log)
    {
        if (!m_SessionInitialized || !m_Xr)
            return;

        XrActiveActionSet activeSets[] =
        {
            { m_MainActionSet, XR_NULL_PATH },
            { m_BaseActionSet, XR_NULL_PATH }
        };
        XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = ARRAYSIZE(activeSets);
        syncInfo.activeActionSets = activeSets;
        const XrResult syncResult = m_Xr->xrSyncActions(m_Session, &syncInfo);
        if (XR_FAILED(syncResult))
        {
            if (!m_SyncFailureLogged)
            {
                m_SyncFailureLogged = true;
                log.Print("xrSyncActions failed: %s (%d)", XrResultName(syncResult), static_cast<int>(syncResult));
            }
            return;
        }

        LogCurrentInteractionProfiles(log);

        L4D2VROpenXrInputStateDesc state{};
        state.valid = 1;
        state.featureFlags = m_FeatureFlags;
        state.actionCount = L4D2VR_OPENXR_ACTION_COUNT;

        ReadBooleanActions(state);
        ReadFloatDigitalActions(state);
        ReadAnalogActions(state);
        PublishDerivedDpadActions(state);
        LocateControllerPoses(displayTime, state, log);
        LocateHandTracking(displayTime, state);
        bridge.PublishInputState(state);
        PumpHaptics(bridge, log);
    }

    void OpenXrInputBridge::ReadBooleanActions(L4D2VROpenXrInputStateDesc& outState)
    {
        for (size_t i = 0; i < m_BooleanActions.size(); ++i)
        {
            XrAction action = m_BooleanActions[i];
            if (action == XR_NULL_HANDLE)
                continue;

            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = action;
            XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(m_Xr->xrGetActionStateBoolean(m_Session, &getInfo, &state)) && state.isActive)
            {
                outState.digitalActions[i].active = 1;
                outState.digitalActions[i].state = state.currentState ? 1u : 0u;
                outState.digitalActions[i].changed = state.changedSinceLastSync ? 1u : 0u;
                outState.digitalActions[i].lastChangeTime = static_cast<int64_t>(state.lastChangeTime);
            }
        }
    }

    void OpenXrInputBridge::ReadFloatDigitalActions(L4D2VROpenXrInputStateDesc& outState)
    {
        for (const FloatDigitalActionDef& def : FloatDigitalDefs())
        {
            const size_t i = Index(def.id);
            XrAction action = m_FloatDigitalActions[i];
            if (action == XR_NULL_HANDLE)
                continue;

            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = action;
            XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(m_Xr->xrGetActionStateFloat(m_Session, &getInfo, &state)) && state.isActive)
            {
                const bool down = state.currentState >= def.threshold;
                outState.digitalActions[i].active = 1;
                outState.digitalActions[i].state = down ? 1u : 0u;
                outState.digitalActions[i].changed =
                    ((m_FloatDigitalInitialized[i] && m_FloatDigitalDown[i] != down) ||
                        state.changedSinceLastSync) ? 1u : 0u;
                outState.digitalActions[i].lastChangeTime = static_cast<int64_t>(state.lastChangeTime);
                m_FloatDigitalDown[i] = down;
                m_FloatDigitalInitialized[i] = true;
            }
        }
    }

    void OpenXrInputBridge::ReadAnalogActions(L4D2VROpenXrInputStateDesc& outState)
    {
        for (const AnalogActionDef& def : AnalogDefs())
        {
            const size_t i = Index(def.id);
            XrAction action = m_AnalogActions[i];
            if (action == XR_NULL_HANDLE)
                continue;

            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = action;
            XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
            if (XR_SUCCEEDED(m_Xr->xrGetActionStateVector2f(m_Session, &getInfo, &state)) && state.isActive)
            {
                outState.analogActions[i].active = 1;
                outState.analogActions[i].changed = state.changedSinceLastSync ? 1u : 0u;
                outState.analogActions[i].lastChangeTime = static_cast<int64_t>(state.lastChangeTime);
                outState.analogActions[i].x = state.currentState.x;
                outState.analogActions[i].y = state.currentState.y;
            }
        }
    }

    void OpenXrInputBridge::SetDerivedDigital(L4D2VROpenXrInputStateDesc& outState, L4D2VROpenXrActionId id, bool down)
    {
        const size_t i = Index(id);
        L4D2VROpenXrDigitalActionDesc& action = outState.digitalActions[i];
        if (action.active)
            return;

        action.active = 1;
        action.state = down ? 1u : 0u;
        action.changed = (m_DerivedDigitalInitialized[i] && m_DerivedDigitalDown[i] != down) ? 1u : 0u;
        m_DerivedDigitalDown[i] = down;
        m_DerivedDigitalInitialized[i] = true;
    }

    void OpenXrInputBridge::PublishDerivedDpadActions(L4D2VROpenXrInputStateDesc& outState)
    {
        constexpr float kPress = 0.70f;
        const L4D2VROpenXrAnalogActionDesc& turn = outState.analogActions[Index(L4D2VROpenXrActionId::Turn)];
        if (turn.active)
        {
            SetDerivedDigital(outState, L4D2VROpenXrActionId::PrevItem, turn.y > kPress);
            SetDerivedDigital(outState, L4D2VROpenXrActionId::NextItem, turn.y < -kPress);
        }

        const L4D2VROpenXrAnalogActionDesc& walk = outState.analogActions[Index(L4D2VROpenXrActionId::Walk)];
        if (walk.active)
        {
            SetDerivedDigital(outState, L4D2VROpenXrActionId::MenuUp, walk.y > kPress);
            SetDerivedDigital(outState, L4D2VROpenXrActionId::MenuDown, walk.y < -kPress);
            SetDerivedDigital(outState, L4D2VROpenXrActionId::MenuRight, walk.x > kPress);
            SetDerivedDigital(outState, L4D2VROpenXrActionId::MenuLeft, walk.x < -kPress);
        }
    }

    void OpenXrInputBridge::LocateControllerPoses(XrTime displayTime, L4D2VROpenXrInputStateDesc& outState, Logger& log)
    {
        for (uint32_t hand = 0; hand < L4D2VR_OPENXR_HAND_COUNT; ++hand)
        {
            if (m_HandSpaces[hand] == XR_NULL_HANDLE)
                continue;

            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = m_HandPoseAction;
            getInfo.subactionPath = m_HandPaths[hand];
            XrActionStatePose poseState{ XR_TYPE_ACTION_STATE_POSE };
            const XrResult poseResult = m_Xr->xrGetActionStatePose(m_Session, &getInfo, &poseState);
            if (XR_FAILED(poseResult) || !poseState.isActive)
            {
                if (!m_PoseInactiveLogged[hand])
                {
                    m_PoseInactiveLogged[hand] = true;
                    log.Print(
                        "OpenXR hand_pose inactive %s: result=%s (%d) active=%u profile=%s",
                        hand == L4D2VR_OPENXR_HAND_LEFT ? "left" : "right",
                        XrResultName(poseResult),
                        static_cast<int>(poseResult),
                        poseState.isActive ? 1u : 0u,
                        PathToString(m_LastInteractionProfiles[hand]).c_str());
                }
                continue;
            }
            m_PoseInactiveLogged[hand] = false;

            XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
            if (XR_FAILED(m_Xr->xrLocateSpace(m_HandSpaces[hand], m_AppSpace, displayTime, &location)))
                continue;

            const bool orientationValid = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            const bool positionValid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            if (!orientationValid && !positionValid)
                continue;

            L4D2VROpenXrControllerPoseDesc& out = outState.controllerPoses[hand];
            out.valid = 1;
            out.active = 1;
            out.locationFlags = static_cast<uint64_t>(location.locationFlags);
            out.displayTime = static_cast<int64_t>(displayTime);
            out.position[0] = location.pose.position.x;
            out.position[1] = location.pose.position.y;
            out.position[2] = location.pose.position.z;
            out.orientation[0] = location.pose.orientation.x;
            out.orientation[1] = location.pose.orientation.y;
            out.orientation[2] = location.pose.orientation.z;
            out.orientation[3] = location.pose.orientation.w;
        }
    }

    OpenXrInputBridge::Vec3 OpenXrInputBridge::JointPos(const XrHandJointLocationEXT& joint)
    {
        return Vec3{ joint.pose.position.x, joint.pose.position.y, joint.pose.position.z };
    }

    OpenXrInputBridge::Vec3 OpenXrInputBridge::Sub(Vec3 a, Vec3 b)
    {
        return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    float OpenXrInputBridge::Dot(Vec3 a, Vec3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    float OpenXrInputBridge::Length(Vec3 value)
    {
        return std::sqrt(Dot(value, value));
    }

    float OpenXrInputBridge::Angle(Vec3 a, Vec3 b)
    {
        const float denom = Length(a) * Length(b);
        if (denom <= 0.000001f)
            return 0.0f;
        return std::acos(std::clamp(Dot(a, b) / denom, -1.0f, 1.0f));
    }

    bool OpenXrInputBridge::JointPositionValid(const XrHandJointLocationEXT& joint)
    {
        return (joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    }

    float OpenXrInputBridge::ComputeCurl(
        const std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT>& joints,
        XrHandJointEXT j0,
        XrHandJointEXT j1,
        XrHandJointEXT j2,
        XrHandJointEXT j3,
        XrHandJointEXT j4)
    {
        const XrHandJointLocationEXT& a = joints[static_cast<size_t>(j0)];
        const XrHandJointLocationEXT& b = joints[static_cast<size_t>(j1)];
        const XrHandJointLocationEXT& c = joints[static_cast<size_t>(j2)];
        const XrHandJointLocationEXT& d = joints[static_cast<size_t>(j3)];
        const XrHandJointLocationEXT& e = joints[static_cast<size_t>(j4)];
        if (!JointPositionValid(a) || !JointPositionValid(b) || !JointPositionValid(c) ||
            !JointPositionValid(d) || !JointPositionValid(e))
        {
            return 0.0f;
        }

        const float curl =
            Angle(Sub(JointPos(c), JointPos(b)), Sub(JointPos(b), JointPos(a))) +
            Angle(Sub(JointPos(d), JointPos(c)), Sub(JointPos(c), JointPos(b))) +
            Angle(Sub(JointPos(e), JointPos(d)), Sub(JointPos(d), JointPos(c)));
        return std::clamp(curl / 3.20f, 0.0f, 1.0f);
    }

    void OpenXrInputBridge::LocateHandTracking(XrTime displayTime, L4D2VROpenXrInputStateDesc& outState)
    {
        if ((m_FeatureFlags & L4D2VR_OPENXR_INPUT_FEATURE_HAND_TRACKING) == 0 || !m_Xr->xrLocateHandJointsEXT)
            return;

        for (uint32_t hand = 0; hand < L4D2VR_OPENXR_HAND_COUNT; ++hand)
        {
            if (m_HandTrackers[hand] == XR_NULL_HANDLE)
                continue;

            std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
            XrHandJointLocationsEXT locations{ XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
            locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locations.jointLocations = joints.data();

            XrHandJointsLocateInfoEXT locateInfo{ XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
            locateInfo.baseSpace = m_AppSpace;
            locateInfo.time = displayTime;
            if (XR_FAILED(m_Xr->xrLocateHandJointsEXT(m_HandTrackers[hand], &locateInfo, &locations)) || !locations.isActive)
                continue;

            L4D2VROpenXrHandTrackingDesc& out = outState.handTracking[hand];
            out.valid = 1;
            out.active = 1;
            out.jointCount = std::min<uint32_t>(locations.jointCount, L4D2VR_OPENXR_HAND_JOINT_COUNT);
            for (uint32_t i = 0; i < out.jointCount; ++i)
            {
                out.joints[i].locationFlags = static_cast<uint64_t>(joints[i].locationFlags);
                out.joints[i].radius = joints[i].radius;
                out.joints[i].position[0] = joints[i].pose.position.x;
                out.joints[i].position[1] = joints[i].pose.position.y;
                out.joints[i].position[2] = joints[i].pose.position.z;
                out.joints[i].orientation[0] = joints[i].pose.orientation.x;
                out.joints[i].orientation[1] = joints[i].pose.orientation.y;
                out.joints[i].orientation[2] = joints[i].pose.orientation.z;
                out.joints[i].orientation[3] = joints[i].pose.orientation.w;
            }

            out.fingerCurls[0] = ComputeCurl(joints,
                XR_HAND_JOINT_THUMB_METACARPAL_EXT,
                XR_HAND_JOINT_THUMB_PROXIMAL_EXT,
                XR_HAND_JOINT_THUMB_DISTAL_EXT,
                XR_HAND_JOINT_THUMB_TIP_EXT,
                XR_HAND_JOINT_THUMB_TIP_EXT);
            out.fingerCurls[1] = ComputeCurl(joints,
                XR_HAND_JOINT_INDEX_METACARPAL_EXT,
                XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
                XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,
                XR_HAND_JOINT_INDEX_DISTAL_EXT,
                XR_HAND_JOINT_INDEX_TIP_EXT);
            out.fingerCurls[2] = ComputeCurl(joints,
                XR_HAND_JOINT_MIDDLE_METACARPAL_EXT,
                XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,
                XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT,
                XR_HAND_JOINT_MIDDLE_DISTAL_EXT,
                XR_HAND_JOINT_MIDDLE_TIP_EXT);
            out.fingerCurls[3] = ComputeCurl(joints,
                XR_HAND_JOINT_RING_METACARPAL_EXT,
                XR_HAND_JOINT_RING_PROXIMAL_EXT,
                XR_HAND_JOINT_RING_INTERMEDIATE_EXT,
                XR_HAND_JOINT_RING_DISTAL_EXT,
                XR_HAND_JOINT_RING_TIP_EXT);
            out.fingerCurls[4] = ComputeCurl(joints,
                XR_HAND_JOINT_LITTLE_METACARPAL_EXT,
                XR_HAND_JOINT_LITTLE_PROXIMAL_EXT,
                XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT,
                XR_HAND_JOINT_LITTLE_DISTAL_EXT,
                XR_HAND_JOINT_LITTLE_TIP_EXT);
        }
    }

    void OpenXrInputBridge::PumpHaptics(BridgeWriter& bridge, Logger& log)
    {
        if (m_HapticAction == XR_NULL_HANDLE || !m_Xr->xrApplyHapticFeedback)
            return;

        for (uint32_t hand = 0; hand < L4D2VR_OPENXR_HAND_COUNT; ++hand)
        {
            L4D2VROpenXrHapticRequestDesc request{};
            if (!bridge.ReadHapticRequest(hand, request) ||
                request.sequence == m_LastHapticSequences[hand] ||
                request.durationSeconds <= 0.0f ||
                request.amplitude <= 0.0f)
            {
                continue;
            }

            m_LastHapticSequences[hand] = request.sequence;
            XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
            vibration.duration = static_cast<XrDuration>(std::max(1.0f, request.durationSeconds * 1000000000.0f));
            vibration.frequency = request.frequency;
            vibration.amplitude = std::clamp(request.amplitude, 0.0f, 1.0f);

            XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
            info.action = m_HapticAction;
            info.subactionPath = m_HandPaths[hand];
            const XrResult result = m_Xr->xrApplyHapticFeedback(
                m_Session,
                &info,
                reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
            if (XR_FAILED(result) && !m_HapticFailureLogged)
            {
                m_HapticFailureLogged = true;
                log.Print("xrApplyHapticFeedback failed: %s (%d)", XrResultName(result), static_cast<int>(result));
            }
        }
    }

    class OpenXrVulkanSubmitProbe
    {
    public:
        explicit OpenXrVulkanSubmitProbe(Logger& log)
            : m_Log(log)
        {
        }

        int Run(const Options& options)
        {
            m_Bridge.Open(options.mappingName, m_Log);
            m_ParentProcess = OpenParentProcess(options.parentPid);

            if (!LoadOpenXrLoader())
                return Fail(10, "OpenXR loader load failed");
            if (!LoadOpenXrGlobalFunctions())
                return Fail(11, "OpenXR global function load failed");
            if (!CreateOpenXrInstance())
                return Fail(12, "OpenXR Vulkan instance creation failed");
            if (!LoadOpenXrInstanceFunctions())
                return Fail(13, "OpenXR Vulkan function load failed");
            if (!m_InputBridge.InitializeInstance(m_Xr, m_Instance, m_Log))
                return Fail(14, "OpenXR input action creation failed");
            if (!CreateOpenXrSystem())
                return Fail(15, "OpenXR system creation failed");
            if (!LoadVulkanLibrary())
                return Fail(16, "Vulkan loader load failed");
            if (!CreateVulkanObjects())
                return Fail(17, "Vulkan graphics binding creation failed");
            if (!CreateSessionObjects())
                return Fail(18, "OpenXR Vulkan session creation failed");
            if (!CreateSwapchains())
                return Fail(19, "OpenXR Vulkan swapchain creation failed");

            const int exitCode = FrameLoop(options);
            if (exitCode != 0)
                m_Bridge.Update(L4D2VROpenXrBridgeStatus::Failed, exitCode, 0, "OpenXR Vulkan frame loop failed");
            return exitCode;
        }

        ~OpenXrVulkanSubmitProbe()
        {
            Shutdown();
        }

    private:
        int Fail(int code, const char* detail)
        {
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::Failed, code, 0, detail);
            return code;
        }

        template <typename T>
        bool LoadVkGlobal(const char* name, T& out)
        {
            out = reinterpret_cast<T>(m_Vk.vkGetInstanceProcAddr(VK_NULL_HANDLE, name));
            if (!out)
            {
                m_Log.Print("vkGetInstanceProcAddr(global, %s) returned null", name);
                return false;
            }
            return true;
        }

        template <typename T>
        bool LoadVkInstance(const char* name, T& out)
        {
            out = reinterpret_cast<T>(m_Vk.vkGetInstanceProcAddr(m_VkInstance, name));
            if (!out)
            {
                m_Log.Print("vkGetInstanceProcAddr(instance, %s) returned null", name);
                return false;
            }
            return true;
        }

        template <typename T>
        bool LoadVkDevice(const char* name, T& out)
        {
            out = reinterpret_cast<T>(m_Vk.vkGetDeviceProcAddr(m_VkDevice, name));
            if (!out)
            {
                m_Log.Print("vkGetDeviceProcAddr(%s) returned null", name);
                return false;
            }
            return true;
        }

        bool LoadOpenXrLoader()
        {
            const std::wstring loaderPath = ExeDirectory() + L"\\openxr_loader.dll";
            m_OpenXrLoader = LoadLibraryW(loaderPath.c_str());
            if (!m_OpenXrLoader)
            {
                m_Log.Print("LoadLibrary failed for %s (GetLastError=%lu)", Narrow(loaderPath).c_str(), GetLastError());
                return false;
            }

            m_Xr.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
                GetProcAddress(m_OpenXrLoader, "xrGetInstanceProcAddr"));
            if (!m_Xr.xrGetInstanceProcAddr)
            {
                m_Log.Print("openxr_loader.dll does not export xrGetInstanceProcAddr");
                return false;
            }

            m_Log.Print("Loaded OpenXR loader: %s", Narrow(loaderPath).c_str());
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::LoaderLoaded, 0, 0, "OpenXR loader loaded");
            return true;
        }

        bool LoadOpenXrGlobalFunctions()
        {
            return LoadXrFunction(
                m_Xr.xrGetInstanceProcAddr,
                XR_NULL_HANDLE,
                "xrEnumerateInstanceExtensionProperties",
                m_Xr.xrEnumerateInstanceExtensionProperties,
                m_Log) &&
                LoadXrFunction(
                    m_Xr.xrGetInstanceProcAddr,
                    XR_NULL_HANDLE,
                    "xrCreateInstance",
                    m_Xr.xrCreateInstance,
                    m_Log);
        }

        bool CreateOpenXrInstance()
        {
            uint32_t extensionCount = 0;
            XrResult result = m_Xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
            if (!Succeeded(m_Log, "xrEnumerateInstanceExtensionProperties(count)", result))
                return false;

            std::vector<XrExtensionProperties> extensions(extensionCount);
            for (XrExtensionProperties& extension : extensions)
                extension.type = XR_TYPE_EXTENSION_PROPERTIES;

            result = m_Xr.xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
            if (!Succeeded(m_Log, "xrEnumerateInstanceExtensionProperties(data)", result))
                return false;

            bool hasVulkan = false;
            bool hasHandTracking = false;
            for (const XrExtensionProperties& extension : extensions)
            {
                if (std::strcmp(extension.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
                    hasVulkan = true;
                if (std::strcmp(extension.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0)
                    hasHandTracking = true;
            }

            if (!hasVulkan)
            {
                m_Log.Print("%s is not supported by the active runtime", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
                return false;
            }

            std::vector<const char*> enabledExtensions;
            enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
            if (hasHandTracking)
            {
                enabledExtensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
                m_HandTrackingExtensionEnabled = true;
            }

            XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
            std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "L4D2VR OpenXR Vulkan Helper");
            createInfo.applicationInfo.applicationVersion = 1;
            std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "L4D2VR");
            createInfo.applicationInfo.engineVersion = 1;
            createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
            createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
            createInfo.enabledExtensionNames = enabledExtensions.data();

            result = m_Xr.xrCreateInstance(&createInfo, &m_Instance);
            if (!Succeeded(m_Log, "xrCreateInstance(Vulkan)", result) || m_Instance == XR_NULL_HANDLE)
                return false;

            m_Log.Print("Created OpenXR instance with %s handTracking=%u",
                XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
                m_HandTrackingExtensionEnabled ? 1u : 0u);
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::InstanceCreated, 0, 0, "OpenXR instance created");
            return true;
        }

        bool LoadOpenXrInstanceFunctions()
        {
            const bool loaded =
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroyInstance", m_Xr.xrDestroyInstance, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetInstanceProperties", m_Xr.xrGetInstanceProperties, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetSystem", m_Xr.xrGetSystem, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateViewConfigurationViews", m_Xr.xrEnumerateViewConfigurationViews, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateEnvironmentBlendModes", m_Xr.xrEnumerateEnvironmentBlendModes, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrCreateSession", m_Xr.xrCreateSession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroySession", m_Xr.xrDestroySession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrCreateReferenceSpace", m_Xr.xrCreateReferenceSpace, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroySpace", m_Xr.xrDestroySpace, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateSwapchainFormats", m_Xr.xrEnumerateSwapchainFormats, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrCreateSwapchain", m_Xr.xrCreateSwapchain, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrDestroySwapchain", m_Xr.xrDestroySwapchain, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEnumerateSwapchainImages", m_Xr.xrEnumerateSwapchainImages, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrAcquireSwapchainImage", m_Xr.xrAcquireSwapchainImage, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrWaitSwapchainImage", m_Xr.xrWaitSwapchainImage, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrReleaseSwapchainImage", m_Xr.xrReleaseSwapchainImage, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrPollEvent", m_Xr.xrPollEvent, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrBeginSession", m_Xr.xrBeginSession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEndSession", m_Xr.xrEndSession, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrWaitFrame", m_Xr.xrWaitFrame, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrBeginFrame", m_Xr.xrBeginFrame, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrEndFrame", m_Xr.xrEndFrame, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrLocateViews", m_Xr.xrLocateViews, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetVulkanInstanceExtensionsKHR", m_Xr.xrGetVulkanInstanceExtensionsKHR, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetVulkanDeviceExtensionsKHR", m_Xr.xrGetVulkanDeviceExtensionsKHR, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetVulkanGraphicsDeviceKHR", m_Xr.xrGetVulkanGraphicsDeviceKHR, m_Log) &&
                LoadXrFunction(m_Xr.xrGetInstanceProcAddr, m_Instance, "xrGetVulkanGraphicsRequirementsKHR", m_Xr.xrGetVulkanGraphicsRequirementsKHR, m_Log) &&
                LoadOpenXrInputFunctions(m_Xr, m_Instance, m_Log);
            if (loaded)
                LoadOpenXrOptionalHandTrackingFunctions(m_Xr, m_Instance, m_Log, m_HandTrackingExtensionEnabled);
            return loaded;
        }

        bool CreateOpenXrSystem()
        {
            XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
            XrResult result = m_Xr.xrGetInstanceProperties(m_Instance, &instanceProperties);
            if (XR_SUCCEEDED(result))
            {
                m_Log.Print(
                    "Runtime: %s %llu.%llu.%llu",
                    instanceProperties.runtimeName,
                    static_cast<unsigned long long>(XR_VERSION_MAJOR(instanceProperties.runtimeVersion)),
                    static_cast<unsigned long long>(XR_VERSION_MINOR(instanceProperties.runtimeVersion)),
                    static_cast<unsigned long long>(XR_VERSION_PATCH(instanceProperties.runtimeVersion)));
            }

            XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
            systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            result = m_Xr.xrGetSystem(m_Instance, &systemInfo, &m_SystemId);
            if (!Succeeded(m_Log, "xrGetSystem(HMD)", result) || m_SystemId == XR_NULL_SYSTEM_ID)
                return false;

            uint32_t viewCount = 0;
            result = m_Xr.xrEnumerateViewConfigurationViews(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &viewCount,
                nullptr);
            if (!Succeeded(m_Log, "xrEnumerateViewConfigurationViews(count)", result) || viewCount < 2)
                return false;

            m_ViewConfigs.assign(viewCount, XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
            result = m_Xr.xrEnumerateViewConfigurationViews(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                viewCount,
                &viewCount,
                m_ViewConfigs.data());
            if (!Succeeded(m_Log, "xrEnumerateViewConfigurationViews(data)", result) || viewCount < 2)
                return false;
            m_ViewConfigs.resize(viewCount);

            m_Log.Print(
                "Stereo views=%u recommendedEye=%ux%u samples=%u",
                viewCount,
                m_ViewConfigs[0].recommendedImageRectWidth,
                m_ViewConfigs[0].recommendedImageRectHeight,
                m_ViewConfigs[0].recommendedSwapchainSampleCount);

            uint32_t blendCount = 0;
            result = m_Xr.xrEnumerateEnvironmentBlendModes(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &blendCount,
                nullptr);
            if (!Succeeded(m_Log, "xrEnumerateEnvironmentBlendModes(count)", result) || blendCount == 0)
                return false;

            std::vector<XrEnvironmentBlendMode> blendModes(blendCount);
            result = m_Xr.xrEnumerateEnvironmentBlendModes(
                m_Instance,
                m_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                blendCount,
                &blendCount,
                blendModes.data());
            if (!Succeeded(m_Log, "xrEnumerateEnvironmentBlendModes(data)", result) || blendCount == 0)
                return false;

            m_BlendMode = blendModes[0];
            for (XrEnvironmentBlendMode mode : blendModes)
            {
                if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
                    m_BlendMode = mode;
            }

            return true;
        }

        bool LoadVulkanLibrary()
        {
            m_VulkanLoader = LoadLibraryW(L"vulkan-1.dll");
            if (!m_VulkanLoader)
            {
                m_Log.Print("LoadLibrary(vulkan-1.dll) failed GetLastError=%lu", GetLastError());
                return false;
            }

            m_Vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(m_VulkanLoader, "vkGetInstanceProcAddr"));
            if (!m_Vk.vkGetInstanceProcAddr)
            {
                m_Log.Print("vulkan-1.dll does not export vkGetInstanceProcAddr");
                return false;
            }

            return LoadVkGlobal("vkCreateInstance", m_Vk.vkCreateInstance) &&
                LoadVkGlobal("vkEnumerateInstanceExtensionProperties", m_Vk.vkEnumerateInstanceExtensionProperties);
        }

        bool QueryXrVulkanExtensionString(
            const char* action,
            XrResult (XRAPI_PTR *queryFn)(XrInstance, XrSystemId, uint32_t, uint32_t*, char*),
            std::string& out)
        {
            uint32_t count = 0;
            XrResult result = queryFn(m_Instance, m_SystemId, 0, &count, nullptr);
            if (!Succeeded(m_Log, action, result))
                return false;

            if (count == 0)
            {
                out.clear();
                return true;
            }

            std::vector<char> buffer(count + 1, '\0');
            result = queryFn(m_Instance, m_SystemId, count, &count, buffer.data());
            if (!Succeeded(m_Log, action, result))
                return false;

            buffer[count] = '\0';
            out.assign(buffer.data());
            return true;
        }

        bool EnumerateInstanceExtensions(std::vector<std::string>& out)
        {
            uint32_t count = 0;
            VkResult result = m_Vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkEnumerateInstanceExtensionProperties(count) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            std::vector<VkExtensionProperties> properties(count);
            result = m_Vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data());
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkEnumerateInstanceExtensionProperties(data) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            out.clear();
            for (const VkExtensionProperties& property : properties)
                out.emplace_back(property.extensionName);
            return true;
        }

        bool EnumerateDeviceExtensions(std::vector<std::string>& out)
        {
            uint32_t count = 0;
            VkResult result = m_Vk.vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkEnumerateDeviceExtensionProperties(count) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            std::vector<VkExtensionProperties> properties(count);
            result = m_Vk.vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &count, properties.data());
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkEnumerateDeviceExtensionProperties(data) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            out.clear();
            for (const VkExtensionProperties& property : properties)
                out.emplace_back(property.extensionName);
            return true;
        }

        bool AddRequiredDeviceExtension(
            const std::vector<std::string>& availableExtensions,
            std::vector<std::string>& enabledExtensions,
            const char* extensionName)
        {
            if (HasName(enabledExtensions, extensionName))
                return true;

            if (!HasName(availableExtensions, extensionName))
            {
                m_Log.Print("Required Vulkan device extension missing: %s", extensionName);
                return false;
            }

            enabledExtensions.emplace_back(extensionName);
            return true;
        }

        bool CreateVulkanObjects()
        {
            XrGraphicsRequirementsVulkanKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
            XrResult xrResult = m_Xr.xrGetVulkanGraphicsRequirementsKHR(m_Instance, m_SystemId, &requirements);
            if (!Succeeded(m_Log, "xrGetVulkanGraphicsRequirementsKHR", xrResult))
                return false;

            m_Log.Print(
                "Vulkan requirements min=%llu.%llu.%llu max=%llu.%llu.%llu",
                static_cast<unsigned long long>(XR_VERSION_MAJOR(requirements.minApiVersionSupported)),
                static_cast<unsigned long long>(XR_VERSION_MINOR(requirements.minApiVersionSupported)),
                static_cast<unsigned long long>(XR_VERSION_PATCH(requirements.minApiVersionSupported)),
                static_cast<unsigned long long>(XR_VERSION_MAJOR(requirements.maxApiVersionSupported)),
                static_cast<unsigned long long>(XR_VERSION_MINOR(requirements.maxApiVersionSupported)),
                static_cast<unsigned long long>(XR_VERSION_PATCH(requirements.maxApiVersionSupported)));

            std::string xrInstanceExtensions;
            if (!QueryXrVulkanExtensionString(
                "xrGetVulkanInstanceExtensionsKHR",
                m_Xr.xrGetVulkanInstanceExtensionsKHR,
                xrInstanceExtensions))
            {
                return false;
            }

            std::vector<std::string> availableInstanceExtensions;
            if (!EnumerateInstanceExtensions(availableInstanceExtensions))
                return false;

            std::vector<std::string> enabledInstanceExtensions = SplitExtensionString(xrInstanceExtensions);
            for (const std::string& extension : enabledInstanceExtensions)
            {
                if (!HasName(availableInstanceExtensions, extension.c_str()))
                {
                    m_Log.Print("OpenXR required Vulkan instance extension is unavailable: %s", extension.c_str());
                    return false;
                }
            }

            if (HasName(availableInstanceExtensions, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))
                AddUniqueName(enabledInstanceExtensions, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
            if (HasName(availableInstanceExtensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                AddUniqueName(enabledInstanceExtensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

            const std::vector<const char*> instanceExtensionNames = MakeNamePointers(enabledInstanceExtensions);

            VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            appInfo.pApplicationName = "L4D2VR OpenXR Vulkan Helper";
            appInfo.applicationVersion = 1;
            appInfo.pEngineName = "L4D2VR";
            appInfo.engineVersion = 1;
            appInfo.apiVersion = VK_API_VERSION_1_1;

            VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            instanceInfo.pApplicationInfo = &appInfo;
            instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensionNames.size());
            instanceInfo.ppEnabledExtensionNames = instanceExtensionNames.empty() ? nullptr : instanceExtensionNames.data();

            VkResult vkResult = m_Vk.vkCreateInstance(&instanceInfo, nullptr, &m_VkInstance);
            if (vkResult != VK_SUCCESS || m_VkInstance == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateInstance failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            if (!LoadVkInstance("vkDestroyInstance", m_Vk.vkDestroyInstance) ||
                !LoadVkInstance("vkGetDeviceProcAddr", m_Vk.vkGetDeviceProcAddr) ||
                !LoadVkInstance("vkGetPhysicalDeviceProperties", m_Vk.vkGetPhysicalDeviceProperties) ||
                !LoadVkInstance("vkGetPhysicalDeviceQueueFamilyProperties", m_Vk.vkGetPhysicalDeviceQueueFamilyProperties) ||
                !LoadVkInstance("vkGetPhysicalDeviceMemoryProperties", m_Vk.vkGetPhysicalDeviceMemoryProperties) ||
                !LoadVkInstance("vkEnumerateDeviceExtensionProperties", m_Vk.vkEnumerateDeviceExtensionProperties) ||
                !LoadVkInstance("vkCreateDevice", m_Vk.vkCreateDevice))
            {
                return false;
            }

            xrResult = m_Xr.xrGetVulkanGraphicsDeviceKHR(m_Instance, m_SystemId, m_VkInstance, &m_PhysicalDevice);
            if (!Succeeded(m_Log, "xrGetVulkanGraphicsDeviceKHR", xrResult) || m_PhysicalDevice == VK_NULL_HANDLE)
                return false;

            VkPhysicalDeviceProperties deviceProperties{};
            m_Vk.vkGetPhysicalDeviceProperties(m_PhysicalDevice, &deviceProperties);
            m_Log.Print(
                "Selected Vulkan physical device: %s api=%u.%u.%u",
                deviceProperties.deviceName,
                VK_VERSION_MAJOR(deviceProperties.apiVersion),
                VK_VERSION_MINOR(deviceProperties.apiVersion),
                VK_VERSION_PATCH(deviceProperties.apiVersion));

            uint32_t queueFamilyCount = 0;
            m_Vk.vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            m_Vk.vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

            m_GraphicsQueueFamily = UINT32_MAX;
            for (uint32_t index = 0; index < queueFamilyCount; ++index)
            {
                if (queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    m_GraphicsQueueFamily = index;
                    break;
                }
            }

            if (m_GraphicsQueueFamily == UINT32_MAX)
            {
                m_Log.Print("No Vulkan graphics queue family found");
                return false;
            }

            std::string xrDeviceExtensions;
            if (!QueryXrVulkanExtensionString(
                "xrGetVulkanDeviceExtensionsKHR",
                m_Xr.xrGetVulkanDeviceExtensionsKHR,
                xrDeviceExtensions))
            {
                return false;
            }

            std::vector<std::string> availableDeviceExtensions;
            if (!EnumerateDeviceExtensions(availableDeviceExtensions))
                return false;

            std::vector<std::string> enabledDeviceExtensions = SplitExtensionString(xrDeviceExtensions);
            for (const std::string& extension : enabledDeviceExtensions)
            {
                if (!HasName(availableDeviceExtensions, extension.c_str()))
                {
                    m_Log.Print("OpenXR required Vulkan device extension is unavailable: %s", extension.c_str());
                    return false;
                }
            }

            if (!AddRequiredDeviceExtension(availableDeviceExtensions, enabledDeviceExtensions, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME))
                return false;
            if (HasName(availableDeviceExtensions, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME))
                AddUniqueName(enabledDeviceExtensions, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
            if (HasName(availableDeviceExtensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
                AddUniqueName(enabledDeviceExtensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
            if (HasName(availableDeviceExtensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
                AddUniqueName(enabledDeviceExtensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
            if (HasName(availableDeviceExtensions, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME))
                AddUniqueName(enabledDeviceExtensions, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);

            const std::vector<const char*> deviceExtensionNames = MakeNamePointers(enabledDeviceExtensions);
            const float queuePriority = 1.0f;

            VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            queueInfo.queueFamilyIndex = m_GraphicsQueueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;

            VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensionNames.size());
            deviceInfo.ppEnabledExtensionNames = deviceExtensionNames.empty() ? nullptr : deviceExtensionNames.data();

            vkResult = m_Vk.vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_VkDevice);
            if (vkResult != VK_SUCCESS || m_VkDevice == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateDevice failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            if (!LoadVkDevice("vkDestroyDevice", m_Vk.vkDestroyDevice) ||
                !LoadVkDevice("vkGetDeviceQueue", m_Vk.vkGetDeviceQueue) ||
                !LoadVkDevice("vkDeviceWaitIdle", m_Vk.vkDeviceWaitIdle) ||
                !LoadVkDevice("vkCreateCommandPool", m_Vk.vkCreateCommandPool) ||
                !LoadVkDevice("vkDestroyCommandPool", m_Vk.vkDestroyCommandPool) ||
                !LoadVkDevice("vkAllocateCommandBuffers", m_Vk.vkAllocateCommandBuffers) ||
                !LoadVkDevice("vkResetCommandBuffer", m_Vk.vkResetCommandBuffer) ||
                !LoadVkDevice("vkBeginCommandBuffer", m_Vk.vkBeginCommandBuffer) ||
                !LoadVkDevice("vkEndCommandBuffer", m_Vk.vkEndCommandBuffer) ||
                !LoadVkDevice("vkQueueSubmit", m_Vk.vkQueueSubmit) ||
                !LoadVkDevice("vkQueueWaitIdle", m_Vk.vkQueueWaitIdle) ||
                !LoadVkDevice("vkCreateImage", m_Vk.vkCreateImage) ||
                !LoadVkDevice("vkDestroyImage", m_Vk.vkDestroyImage) ||
                !LoadVkDevice("vkGetImageMemoryRequirements", m_Vk.vkGetImageMemoryRequirements) ||
                !LoadVkDevice("vkCreateBuffer", m_Vk.vkCreateBuffer) ||
                !LoadVkDevice("vkDestroyBuffer", m_Vk.vkDestroyBuffer) ||
                !LoadVkDevice("vkGetBufferMemoryRequirements", m_Vk.vkGetBufferMemoryRequirements) ||
                !LoadVkDevice("vkAllocateMemory", m_Vk.vkAllocateMemory) ||
                !LoadVkDevice("vkFreeMemory", m_Vk.vkFreeMemory) ||
                !LoadVkDevice("vkBindImageMemory", m_Vk.vkBindImageMemory) ||
                !LoadVkDevice("vkBindBufferMemory", m_Vk.vkBindBufferMemory) ||
                !LoadVkDevice("vkMapMemory", m_Vk.vkMapMemory) ||
                !LoadVkDevice("vkUnmapMemory", m_Vk.vkUnmapMemory) ||
                !LoadVkDevice("vkGetMemoryWin32HandlePropertiesKHR", m_Vk.vkGetMemoryWin32HandlePropertiesKHR) ||
                !LoadVkDevice("vkCmdPipelineBarrier", m_Vk.vkCmdPipelineBarrier) ||
                !LoadVkDevice("vkCmdBlitImage", m_Vk.vkCmdBlitImage) ||
                !LoadVkDevice("vkCmdCopyImageToBuffer", m_Vk.vkCmdCopyImageToBuffer) ||
                !LoadVkDevice("vkCmdClearColorImage", m_Vk.vkCmdClearColorImage) ||
                !LoadVkDevice("vkCreateImageView", m_Vk.vkCreateImageView) ||
                !LoadVkDevice("vkDestroyImageView", m_Vk.vkDestroyImageView) ||
                !LoadVkDevice("vkCreateSampler", m_Vk.vkCreateSampler) ||
                !LoadVkDevice("vkDestroySampler", m_Vk.vkDestroySampler) ||
                !LoadVkDevice("vkCreateShaderModule", m_Vk.vkCreateShaderModule) ||
                !LoadVkDevice("vkDestroyShaderModule", m_Vk.vkDestroyShaderModule) ||
                !LoadVkDevice("vkCreateRenderPass", m_Vk.vkCreateRenderPass) ||
                !LoadVkDevice("vkDestroyRenderPass", m_Vk.vkDestroyRenderPass) ||
                !LoadVkDevice("vkCreateFramebuffer", m_Vk.vkCreateFramebuffer) ||
                !LoadVkDevice("vkDestroyFramebuffer", m_Vk.vkDestroyFramebuffer) ||
                !LoadVkDevice("vkCreateDescriptorSetLayout", m_Vk.vkCreateDescriptorSetLayout) ||
                !LoadVkDevice("vkDestroyDescriptorSetLayout", m_Vk.vkDestroyDescriptorSetLayout) ||
                !LoadVkDevice("vkCreateDescriptorPool", m_Vk.vkCreateDescriptorPool) ||
                !LoadVkDevice("vkDestroyDescriptorPool", m_Vk.vkDestroyDescriptorPool) ||
                !LoadVkDevice("vkAllocateDescriptorSets", m_Vk.vkAllocateDescriptorSets) ||
                !LoadVkDevice("vkUpdateDescriptorSets", m_Vk.vkUpdateDescriptorSets) ||
                !LoadVkDevice("vkCreatePipelineLayout", m_Vk.vkCreatePipelineLayout) ||
                !LoadVkDevice("vkDestroyPipelineLayout", m_Vk.vkDestroyPipelineLayout) ||
                !LoadVkDevice("vkCreateGraphicsPipelines", m_Vk.vkCreateGraphicsPipelines) ||
                !LoadVkDevice("vkDestroyPipeline", m_Vk.vkDestroyPipeline) ||
                !LoadVkDevice("vkCmdBeginRenderPass", m_Vk.vkCmdBeginRenderPass) ||
                !LoadVkDevice("vkCmdEndRenderPass", m_Vk.vkCmdEndRenderPass) ||
                !LoadVkDevice("vkCmdBindPipeline", m_Vk.vkCmdBindPipeline) ||
                !LoadVkDevice("vkCmdBindDescriptorSets", m_Vk.vkCmdBindDescriptorSets) ||
                !LoadVkDevice("vkCmdPushConstants", m_Vk.vkCmdPushConstants) ||
                !LoadVkDevice("vkCmdSetViewport", m_Vk.vkCmdSetViewport) ||
                !LoadVkDevice("vkCmdSetScissor", m_Vk.vkCmdSetScissor) ||
                !LoadVkDevice("vkCmdDraw", m_Vk.vkCmdDraw))
            {
                return false;
            }

            m_Vk.vkGetDeviceQueue(m_VkDevice, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
            if (m_GraphicsQueue == VK_NULL_HANDLE)
            {
                m_Log.Print("vkGetDeviceQueue returned null graphics queue");
                return false;
            }

            m_Vk.vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &m_MemoryProperties);

            VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = m_GraphicsQueueFamily;
            vkResult = m_Vk.vkCreateCommandPool(m_VkDevice, &poolInfo, nullptr, &m_CommandPool);
            if (vkResult != VK_SUCCESS || m_CommandPool == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateCommandPool failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocInfo.commandPool = m_CommandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkResult = m_Vk.vkAllocateCommandBuffers(m_VkDevice, &allocInfo, &m_CommandBuffer);
            if (vkResult != VK_SUCCESS || m_CommandBuffer == VK_NULL_HANDLE)
            {
                m_Log.Print("vkAllocateCommandBuffers failed: %s (%d)", VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            m_Log.Print(
                "Created Vulkan device queueFamily=%u extensions=%zu",
                m_GraphicsQueueFamily,
                enabledDeviceExtensions.size());
            return true;
        }

        bool CreateSessionObjects()
        {
            XrGraphicsBindingVulkanKHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
            graphicsBinding.instance = m_VkInstance;
            graphicsBinding.physicalDevice = m_PhysicalDevice;
            graphicsBinding.device = m_VkDevice;
            graphicsBinding.queueFamilyIndex = m_GraphicsQueueFamily;
            graphicsBinding.queueIndex = 0;

            XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
            sessionInfo.next = &graphicsBinding;
            sessionInfo.systemId = m_SystemId;

            XrResult result = m_Xr.xrCreateSession(m_Instance, &sessionInfo, &m_Session);
            if (!Succeeded(m_Log, "xrCreateSession(Vulkan)", result) || m_Session == XR_NULL_HANDLE)
                return false;

            XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

            result = m_Xr.xrCreateReferenceSpace(m_Session, &spaceInfo, &m_AppSpace);
            if (!Succeeded(m_Log, "xrCreateReferenceSpace(LOCAL)", result) || m_AppSpace == XR_NULL_HANDLE)
                return false;

            if (!m_InputBridge.InitializeSession(m_Session, m_AppSpace, m_HandTrackingExtensionEnabled, m_Log))
                return false;

            m_Log.Print("Created OpenXR Vulkan session and LOCAL reference space");
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::SessionCreated, 0, 0, "OpenXR Vulkan session created");
            return true;
        }

        VkShaderModule CreateShaderModuleFromBase64(const char* base64, const char* name)
        {
            const std::vector<uint32_t> code = DecodeBase64Spirv(base64);
            if (code.empty())
            {
                m_Log.Print("Failed to decode %s SPIR-V", name);
                return VK_NULL_HANDLE;
            }

            VkShaderModuleCreateInfo createInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            createInfo.codeSize = code.size() * sizeof(uint32_t);
            createInfo.pCode = code.data();

            VkShaderModule module = VK_NULL_HANDLE;
            VkResult result = m_Vk.vkCreateShaderModule(m_VkDevice, &createInfo, nullptr, &module);
            if (result != VK_SUCCESS || module == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateShaderModule(%s) failed: %s (%d)", name, VkResultName(result), static_cast<int>(result));
                return VK_NULL_HANDLE;
            }
            return module;
        }

        bool CreateShaderBlitPipeline(VkFormat colorFormat)
        {
            VkDescriptorSetLayoutBinding textureBinding{};
            textureBinding.binding = 0;
            textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            textureBinding.descriptorCount = 1;
            textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            descriptorLayoutInfo.bindingCount = 1;
            descriptorLayoutInfo.pBindings = &textureBinding;

            VkResult result = m_Vk.vkCreateDescriptorSetLayout(m_VkDevice, &descriptorLayoutInfo, nullptr, &m_BlitDescriptorSetLayout);
            if (result != VK_SUCCESS || m_BlitDescriptorSetLayout == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateDescriptorSetLayout(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = 1.0f;

            result = m_Vk.vkCreateSampler(m_VkDevice, &samplerInfo, nullptr, &m_BlitSampler);
            if (result != VK_SUCCESS || m_BlitSampler == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateSampler(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            poolSize.descriptorCount = kBlitDescriptorSetCount;

            VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.maxSets = kBlitDescriptorSetCount;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;

            result = m_Vk.vkCreateDescriptorPool(m_VkDevice, &poolInfo, nullptr, &m_BlitDescriptorPool);
            if (result != VK_SUCCESS || m_BlitDescriptorPool == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateDescriptorPool(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            std::array<VkDescriptorSetLayout, kBlitDescriptorSetCount> descriptorLayouts{};
            descriptorLayouts.fill(m_BlitDescriptorSetLayout);
            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool = m_BlitDescriptorPool;
            allocInfo.descriptorSetCount = static_cast<uint32_t>(descriptorLayouts.size());
            allocInfo.pSetLayouts = descriptorLayouts.data();

            result = m_Vk.vkAllocateDescriptorSets(m_VkDevice, &allocInfo, m_BlitDescriptorSets.data());
            if (result != VK_SUCCESS)
            {
                m_Log.Print("vkAllocateDescriptorSets(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = colorFormat;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorRef{};
            colorRef.attachment = 0;
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;

            VkRenderPassCreateInfo renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &colorAttachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;

            result = m_Vk.vkCreateRenderPass(m_VkDevice, &renderPassInfo, nullptr, &m_BlitRenderPass);
            if (result != VK_SUCCESS || m_BlitRenderPass == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateRenderPass(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            VkPushConstantRange pushRange{};
            pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            pushRange.size = sizeof(float) * 4;

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &m_BlitDescriptorSetLayout;
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &pushRange;

            result = m_Vk.vkCreatePipelineLayout(m_VkDevice, &pipelineLayoutInfo, nullptr, &m_BlitPipelineLayout);
            if (result != VK_SUCCESS || m_BlitPipelineLayout == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreatePipelineLayout(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            VkShaderModule vertexShader = CreateShaderModuleFromBase64(kOpenXrBlitVertSpvBase64, "blit vertex");
            VkShaderModule fragmentShader = CreateShaderModuleFromBase64(kOpenXrBlitFragSpvBase64, "blit fragment");
            if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE)
            {
                if (vertexShader != VK_NULL_HANDLE)
                    m_Vk.vkDestroyShaderModule(m_VkDevice, vertexShader, nullptr);
                if (fragmentShader != VK_NULL_HANDLE)
                    m_Vk.vkDestroyShaderModule(m_VkDevice, fragmentShader, nullptr);
                return false;
            }

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertexShader;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragmentShader;
            stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT |
                VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT |
                VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            colorBlend.attachmentCount = 1;
            colorBlend.pAttachments = &colorBlendAttachment;

            VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamicState.dynamicStateCount = ARRAYSIZE(dynamicStates);
            dynamicState.pDynamicStates = dynamicStates;

            VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pipelineInfo.stageCount = ARRAYSIZE(stages);
            pipelineInfo.pStages = stages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.pColorBlendState = &colorBlend;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = m_BlitPipelineLayout;
            pipelineInfo.renderPass = m_BlitRenderPass;
            pipelineInfo.subpass = 0;

            result = m_Vk.vkCreateGraphicsPipelines(m_VkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_BlitPipeline);

            m_Vk.vkDestroyShaderModule(m_VkDevice, fragmentShader, nullptr);
            m_Vk.vkDestroyShaderModule(m_VkDevice, vertexShader, nullptr);

            if (result != VK_SUCCESS || m_BlitPipeline == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateGraphicsPipelines(blit) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                return false;
            }

            m_Log.Print("Created sRGB-correct Vulkan blit pipeline for swapchain format=%u", static_cast<unsigned int>(colorFormat));
            return true;
        }

        void DestroySwapchainRenderTargets(VulkanEyeSwapchain& swapchain)
        {
            for (VkFramebuffer framebuffer : swapchain.framebuffers)
            {
                if (framebuffer != VK_NULL_HANDLE && m_Vk.vkDestroyFramebuffer)
                    m_Vk.vkDestroyFramebuffer(m_VkDevice, framebuffer, nullptr);
            }
            swapchain.framebuffers.clear();

            for (VkImageView view : swapchain.imageViews)
            {
                if (view != VK_NULL_HANDLE && m_Vk.vkDestroyImageView)
                    m_Vk.vkDestroyImageView(m_VkDevice, view, nullptr);
            }
            swapchain.imageViews.clear();
        }

        bool CreateSwapchainRenderTargets(VulkanEyeSwapchain& swapchain)
        {
            if (!m_UseShaderBlit)
                return true;

            swapchain.imageViews.assign(swapchain.images.size(), VK_NULL_HANDLE);
            swapchain.framebuffers.assign(swapchain.images.size(), VK_NULL_HANDLE);

            for (size_t i = 0; i < swapchain.images.size(); ++i)
            {
                VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                viewInfo.image = swapchain.images[i].image;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = swapchain.format;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.layerCount = 1;

                VkResult result = m_Vk.vkCreateImageView(m_VkDevice, &viewInfo, nullptr, &swapchain.imageViews[i]);
                if (result != VK_SUCCESS || swapchain.imageViews[i] == VK_NULL_HANDLE)
                {
                    m_Log.Print("vkCreateImageView(swapchain) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                    return false;
                }

                VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                framebufferInfo.renderPass = m_BlitRenderPass;
                framebufferInfo.attachmentCount = 1;
                framebufferInfo.pAttachments = &swapchain.imageViews[i];
                framebufferInfo.width = swapchain.width;
                framebufferInfo.height = swapchain.height;
                framebufferInfo.layers = 1;

                result = m_Vk.vkCreateFramebuffer(m_VkDevice, &framebufferInfo, nullptr, &swapchain.framebuffers[i]);
                if (result != VK_SUCCESS || swapchain.framebuffers[i] == VK_NULL_HANDLE)
                {
                    m_Log.Print("vkCreateFramebuffer(swapchain) failed: %s (%d)", VkResultName(result), static_cast<int>(result));
                    return false;
                }
            }

            return true;
        }

        bool CreateSwapchains()
        {
            uint32_t formatCount = 0;
            XrResult result = m_Xr.xrEnumerateSwapchainFormats(m_Session, 0, &formatCount, nullptr);
            if (!Succeeded(m_Log, "xrEnumerateSwapchainFormats(count)", result) || formatCount == 0)
                return false;

            std::vector<int64_t> formats(formatCount);
            result = m_Xr.xrEnumerateSwapchainFormats(m_Session, formatCount, &formatCount, formats.data());
            if (!Succeeded(m_Log, "xrEnumerateSwapchainFormats(data)", result) || formatCount == 0)
                return false;

            std::ostringstream formatList;
            for (size_t i = 0; i < formats.size(); ++i)
            {
                if (i != 0)
                    formatList << ",";
                formatList << formats[i];
            }
            m_Log.Print("Runtime swapchain VkFormats=%s", formatList.str().c_str());

            const std::array<VkFormat, 4> preferredFormats = {
                VK_FORMAT_B8G8R8A8_UNORM,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_R8G8B8A8_SRGB
            };

            VkFormat selectedFormat = static_cast<VkFormat>(formats[0]);
            for (VkFormat preferred : preferredFormats)
            {
                if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(preferred)) != formats.end())
                {
                    selectedFormat = preferred;
                    break;
                }
            }

            m_Log.Print("Selected swapchain VkFormat=%u", static_cast<unsigned int>(selectedFormat));
            m_SelectedSwapchainFormat = selectedFormat;
            m_UseShaderBlit = IsSrgbVkFormat(selectedFormat);
            if (m_UseShaderBlit && !CreateShaderBlitPipeline(selectedFormat))
                return false;

            m_Eyes.resize(2);
            for (size_t eye = 0; eye < m_Eyes.size(); ++eye)
            {
                VulkanEyeSwapchain& swapchain = m_Eyes[eye];
                const XrViewConfigurationView& view = m_ViewConfigs[eye];
                swapchain.width = view.recommendedImageRectWidth;
                swapchain.height = view.recommendedImageRectHeight;
                swapchain.format = selectedFormat;

                XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
                createInfo.usageFlags =
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
                createInfo.format = static_cast<int64_t>(selectedFormat);
                createInfo.sampleCount = std::max(1u, view.recommendedSwapchainSampleCount);
                createInfo.width = swapchain.width;
                createInfo.height = swapchain.height;
                createInfo.faceCount = 1;
                createInfo.arraySize = 1;
                createInfo.mipCount = 1;

                result = m_Xr.xrCreateSwapchain(m_Session, &createInfo, &swapchain.handle);
                swapchain.supportsTransferSrc = XR_SUCCEEDED(result) && swapchain.handle != XR_NULL_HANDLE;
                if (!swapchain.supportsTransferSrc)
                {
                    m_Log.Print("xrCreateSwapchain(Vulkan transfer-src) failed result=%d; retrying without readback usage",
                        static_cast<int>(result));
                    swapchain.handle = XR_NULL_HANDLE;
                    createInfo.usageFlags =
                        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
                    result = m_Xr.xrCreateSwapchain(m_Session, &createInfo, &swapchain.handle);
                }
                if (!Succeeded(m_Log, "xrCreateSwapchain(Vulkan)", result) || swapchain.handle == XR_NULL_HANDLE)
                    return false;

                uint32_t imageCount = 0;
                result = m_Xr.xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
                if (!Succeeded(m_Log, "xrEnumerateSwapchainImages(count)", result) || imageCount == 0)
                    return false;

                swapchain.images.assign(imageCount, XrSwapchainImageVulkanKHR{ XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
                result = m_Xr.xrEnumerateSwapchainImages(
                    swapchain.handle,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
                if (!Succeeded(m_Log, "xrEnumerateSwapchainImages(data)", result) || imageCount == 0)
                    return false;

                swapchain.images.resize(imageCount);
                swapchain.layouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
                if (!CreateSwapchainRenderTargets(swapchain))
                    return false;

                m_Log.Print(
                    "Created Vulkan eye %zu swapchain %ux%u images=%u transferSrc=%u",
                    eye,
                    swapchain.width,
                    swapchain.height,
                    imageCount,
                    swapchain.supportsTransferSrc ? 1u : 0u);
            }

            return true;
        }

        bool PollEvents(bool& shouldExit)
        {
            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            while (true)
            {
                event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
                const XrResult result = m_Xr.xrPollEvent(m_Instance, &event);
                if (result == XR_EVENT_UNAVAILABLE)
                    return true;
                if (!Succeeded(m_Log, "xrPollEvent", result))
                    return false;

                switch (event.type)
                {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    m_Log.Print("OpenXR instance loss pending");
                    shouldExit = true;
                    return true;

                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                {
                    const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    m_SessionState = changed.state;
                    m_Log.Print("Session state changed: %d", static_cast<int>(m_SessionState));

                    if (m_SessionState == XR_SESSION_STATE_READY && !m_SessionRunning)
                    {
                        XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (!Succeeded(m_Log, "xrBeginSession", m_Xr.xrBeginSession(m_Session, &beginInfo)))
                            return false;
                        m_SessionRunning = true;
                        m_Log.Print("OpenXR Vulkan session running");
                        m_Bridge.Update(L4D2VROpenXrBridgeStatus::SessionRunning, 0, 0, "OpenXR Vulkan session running");
                    }
                    else if (m_SessionState == XR_SESSION_STATE_STOPPING && m_SessionRunning)
                    {
                        if (!Succeeded(m_Log, "xrEndSession", m_Xr.xrEndSession(m_Session)))
                            return false;
                        m_SessionRunning = false;
                        m_Log.Print("OpenXR Vulkan session stopped");
                    }
                    else if (m_SessionState == XR_SESSION_STATE_EXITING ||
                        m_SessionState == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        shouldExit = true;
                    }
                    break;
                }

                default:
                    break;
                }
            }
        }

        bool ParentStillAlive() const
        {
            if (!m_ParentProcess)
                return true;

            const DWORD wait = WaitForSingleObject(m_ParentProcess, 0);
            return wait == WAIT_TIMEOUT;
        }

        void DestroyImportedGameEye(VulkanGameEyeTexture& eye)
        {
            if (eye.view != VK_NULL_HANDLE && m_Vk.vkDestroyImageView)
                m_Vk.vkDestroyImageView(m_VkDevice, eye.view, nullptr);
            if (eye.image != VK_NULL_HANDLE && m_Vk.vkDestroyImage)
                m_Vk.vkDestroyImage(m_VkDevice, eye.image, nullptr);
            if (eye.memory != VK_NULL_HANDLE && m_Vk.vkFreeMemory)
                m_Vk.vkFreeMemory(m_VkDevice, eye.memory, nullptr);
            eye = VulkanGameEyeTexture{};
        }

        bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags preferred, uint32_t& outIndex) const
        {
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) &&
                    (m_MemoryProperties.memoryTypes[i].propertyFlags & preferred) == preferred)
                {
                    outIndex = i;
                    return true;
                }
            }
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if (typeBits & (1u << i))
                {
                    outIndex = i;
                    return true;
                }
            }
            return false;
        }

        struct VulkanImageDumpTarget
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            VkFormat format = VK_FORMAT_UNDEFINED;
            std::string path;
            std::string label;
        };

        uint32_t DebugVkFormatBytesPerPixel(VkFormat format) const
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                return 4;
            default:
                return 0;
            }
        }

        bool FindExactMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required, uint32_t& outIndex) const
        {
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) &&
                    (m_MemoryProperties.memoryTypes[i].propertyFlags & required) == required)
                {
                    outIndex = i;
                    return true;
                }
            }
            return false;
        }

        void DestroyImageDumpTarget(VulkanImageDumpTarget& target)
        {
            if (target.buffer != VK_NULL_HANDLE && m_Vk.vkDestroyBuffer)
                m_Vk.vkDestroyBuffer(m_VkDevice, target.buffer, nullptr);
            if (target.memory != VK_NULL_HANDLE && m_Vk.vkFreeMemory)
                m_Vk.vkFreeMemory(m_VkDevice, target.memory, nullptr);
            target = VulkanImageDumpTarget{};
        }

        bool CreateImageDumpTarget(
            const char* label,
            uint32_t eyeIndex,
            uint32_t frameIndex,
            uint32_t width,
            uint32_t height,
            VkFormat format,
            VulkanImageDumpTarget& target)
        {
            if (!label || width == 0 || height == 0 || DebugVkFormatBytesPerPixel(format) == 0)
            {
                m_Log.Print("[OpenXR][ImageDump] skip label=%s unsupported format=%u size=%ux%u",
                    label ? label : "?", static_cast<unsigned int>(format), width, height);
                return false;
            }

            ::CreateDirectoryA("openxr_eye_debug", nullptr);
            char path[MAX_PATH] = {};
            std::snprintf(
                path,
                sizeof(path),
                "openxr_eye_debug\\openxr_%08u_helper_%s_%s.bmp",
                frameIndex,
                EyeName(eyeIndex),
                label);

            target.width = width;
            target.height = height;
            target.format = format;
            target.size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4ull;
            target.path = path;
            target.label = label;

            VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = target.size;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkResult result = m_Vk.vkCreateBuffer(m_VkDevice, &bufferInfo, nullptr, &target.buffer);
            if (result != VK_SUCCESS || target.buffer == VK_NULL_HANDLE)
            {
                m_Log.Print("[OpenXR][ImageDump] vkCreateBuffer label=%s failed: %s (%d)",
                    label, VkResultName(result), static_cast<int>(result));
                target = VulkanImageDumpTarget{};
                return false;
            }

            VkMemoryRequirements memoryRequirements{};
            m_Vk.vkGetBufferMemoryRequirements(m_VkDevice, target.buffer, &memoryRequirements);
            uint32_t memoryTypeIndex = 0;
            if (!FindExactMemoryType(
                memoryRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                memoryTypeIndex))
            {
                m_Log.Print("[OpenXR][ImageDump] no host-coherent staging memory label=%s memBits=0x%X",
                    label, memoryRequirements.memoryTypeBits);
                DestroyImageDumpTarget(target);
                return false;
            }

            VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocInfo.allocationSize = memoryRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;
            result = m_Vk.vkAllocateMemory(m_VkDevice, &allocInfo, nullptr, &target.memory);
            if (result != VK_SUCCESS || target.memory == VK_NULL_HANDLE)
            {
                m_Log.Print("[OpenXR][ImageDump] vkAllocateMemory label=%s failed: %s (%d)",
                    label, VkResultName(result), static_cast<int>(result));
                DestroyImageDumpTarget(target);
                return false;
            }

            result = m_Vk.vkBindBufferMemory(m_VkDevice, target.buffer, target.memory, 0);
            if (result != VK_SUCCESS)
            {
                m_Log.Print("[OpenXR][ImageDump] vkBindBufferMemory label=%s failed: %s (%d)",
                    label, VkResultName(result), static_cast<int>(result));
                DestroyImageDumpTarget(target);
                return false;
            }

            return true;
        }

        void CmdCopyImageToDumpTarget(
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout restoreLayout,
            VulkanImageDumpTarget& target)
        {
            if (image == VK_NULL_HANDLE || target.buffer == VK_NULL_HANDLE)
                return;

            CmdTransitionImage(
                image,
                oldLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkBufferImageCopy copyRegion{};
            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageExtent = { target.width, target.height, 1 };
            m_Vk.vkCmdCopyImageToBuffer(
                m_CommandBuffer,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                target.buffer,
                1,
                &copyRegion);

            CmdTransitionImage(
                image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                restoreLayout,
                VK_ACCESS_TRANSFER_READ_BIT,
                0,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        static bool DebugVkConvertPixelToBgra(
            const uint8_t* src,
            VkFormat format,
            uint8_t& b,
            uint8_t& g,
            uint8_t& r,
            uint8_t& a)
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                b = src[0];
                g = src[1];
                r = src[2];
                a = src[3];
                return true;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                r = src[0];
                g = src[1];
                b = src[2];
                a = src[3];
                return true;
            default:
                return false;
            }
        }

        static bool WriteImageDumpBmp(
            const std::string& path,
            uint32_t width,
            uint32_t height,
            VkFormat format,
            const void* pixels)
        {
            if (!pixels || width == 0 || height == 0)
                return false;

            FILE* file = nullptr;
            if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
                return false;

            auto writeLe16 = [](uint8_t* dst, uint16_t value)
                {
                    dst[0] = static_cast<uint8_t>(value & 0xFFu);
                    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
                };
            auto writeLe32 = [](uint8_t* dst, uint32_t value)
                {
                    dst[0] = static_cast<uint8_t>(value & 0xFFu);
                    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
                    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
                    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
                };

            const uint32_t outPitch = width * 4u;
            const uint32_t pixelBytes = outPitch * height;
            const uint32_t fileBytes = 54u + pixelBytes;
            uint8_t header[54] = {};
            header[0] = 'B';
            header[1] = 'M';
            writeLe32(header + 2, fileBytes);
            writeLe32(header + 10, 54u);
            writeLe32(header + 14, 40u);
            writeLe32(header + 18, width);
            writeLe32(header + 22, static_cast<uint32_t>(-static_cast<int32_t>(height)));
            writeLe16(header + 26, 1u);
            writeLe16(header + 28, 32u);
            writeLe32(header + 34, pixelBytes);
            fwrite(header, 1, sizeof(header), file);

            const uint8_t* srcBase = static_cast<const uint8_t*>(pixels);
            std::vector<uint8_t> row(outPitch);
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
                for (uint32_t x = 0; x < width; ++x)
                {
                    uint8_t b = 0, g = 0, r = 0, a = 0xFF;
                    if (!DebugVkConvertPixelToBgra(srcRow + static_cast<size_t>(x) * 4u, format, b, g, r, a))
                    {
                        fclose(file);
                        return false;
                    }
                    uint8_t* dst = row.data() + static_cast<size_t>(x) * 4u;
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = r;
                    dst[3] = a;
                }
                fwrite(row.data(), 1, row.size(), file);
            }

            fclose(file);
            return true;
        }

        void CompleteImageDumpTarget(VulkanImageDumpTarget& target)
        {
            if (target.buffer == VK_NULL_HANDLE || target.memory == VK_NULL_HANDLE)
                return;

            void* mapped = nullptr;
            VkResult result = m_Vk.vkMapMemory(m_VkDevice, target.memory, 0, target.size, 0, &mapped);
            if (result != VK_SUCCESS || !mapped)
            {
                m_Log.Print("[OpenXR][ImageDump] vkMapMemory label=%s failed: %s (%d)",
                    target.label.c_str(), VkResultName(result), static_cast<int>(result));
                DestroyImageDumpTarget(target);
                return;
            }

            std::vector<uint8_t> pixels;
            try
            {
                pixels.resize(static_cast<size_t>(target.size));
                std::memcpy(pixels.data(), mapped, static_cast<size_t>(target.size));
            }
            catch (...)
            {
                m_Vk.vkUnmapMemory(m_VkDevice, target.memory);
                m_Log.Print("[OpenXR][ImageDump] failed to stage async write label=%s bytes=%llu",
                    target.label.c_str(), static_cast<unsigned long long>(target.size));
                DestroyImageDumpTarget(target);
                return;
            }

            m_Vk.vkUnmapMemory(m_VkDevice, target.memory);

            const std::string path = target.path;
            const std::string label = target.label;
            const uint32_t width = target.width;
            const uint32_t height = target.height;
            const VkFormat format = target.format;
            m_Log.Print("[OpenXR][ImageDump] queued async write %s label=%s size=%ux%u format=%u bytes=%llu",
                path.c_str(), label.c_str(), width, height, static_cast<unsigned int>(format),
                static_cast<unsigned long long>(target.size));
            DestroyImageDumpTarget(target);

            std::thread(
                [path, label, width, height, format, pixels = std::move(pixels)]() mutable
                {
                    const bool wrote = OpenXrVulkanSubmitProbe::WriteImageDumpBmp(
                        path,
                        width,
                        height,
                        format,
                        pixels.data());
                    if (!wrote)
                    {
                        char line[512] = {};
                        std::snprintf(
                            line,
                            sizeof(line),
                            "[OpenXR][ImageDump] async write failed path=%s label=%s\n",
                            path.c_str(),
                            label.c_str());
                        OutputDebugStringA(line);
                    }
                }).detach();
        }

        bool IsSyntheticOpenXrSharedFrame(uint32_t frameId) const
        {
            return frameId == 0 || (frameId & 0x80000000u) != 0;
        }

        bool ShouldDumpOpenXrDebugImages(uint32_t eyeIndex, uint32_t frameIndex, uint32_t sharedFrameId)
        {
            if (m_DebugImageDumpCompleted)
                return false;

            const ULONGLONG now = GetTickCount64();
            if (IsSyntheticOpenXrSharedFrame(sharedFrameId))
            {
                if (m_DebugImageDumpStartMs != 0)
                {
                    m_Log.Print("[OpenXR][ImageDump] reset helper dump timer after synthetic/pre-game shared frame=%u",
                        sharedFrameId);
                    m_DebugImageDumpStartMs = 0;
                    m_DebugImageDumpStartSharedFrameId = 0;
                    m_DebugImageDumpEyeMask = 0;
                }
                if (m_DebugImageDumpWaitingGameFrameLogMs == 0 ||
                    now - m_DebugImageDumpWaitingGameFrameLogMs >= 5000ull)
                {
                    m_DebugImageDumpWaitingGameFrameLogMs = now;
                    m_Log.Print("[OpenXR][ImageDump] waiting for real game shared frame before helper dump currentSharedFrame=%u helperFrame=%u",
                        sharedFrameId, frameIndex);
                }
                return false;
            }

            if (m_DebugImageDumpStartMs == 0)
            {
                m_DebugImageDumpStartMs = now;
                m_DebugImageDumpStartSharedFrameId = sharedFrameId;
                m_Log.Print("[OpenXR][ImageDump] armed; dumping helper source/swapchain images once after 30s of real game shared frames startSharedFrame=%u helperFrame=%u",
                    sharedFrameId, frameIndex);
                return false;
            }

            if (now - m_DebugImageDumpStartMs < 30000ull)
                return false;

            const uint32_t bit = 1u << eyeIndex;
            if ((m_DebugImageDumpEyeMask & bit) != 0)
                return false;

            m_DebugImageDumpEyeMask |= bit;
            if ((m_DebugImageDumpEyeMask & L4D2VR_OPENXR_EYES_READY_MASK) == L4D2VR_OPENXR_EYES_READY_MASK)
                m_DebugImageDumpCompleted = true;

            m_Log.Print("[OpenXR][ImageDump] dumping helper images eye=%s(%u) helperFrame=%u sharedFrame=%u startSharedFrame=%u eyeMask=0x%X elapsedMs=%llu",
                EyeName(eyeIndex),
                eyeIndex,
                frameIndex,
                sharedFrameId,
                m_DebugImageDumpStartSharedFrameId,
                m_DebugImageDumpEyeMask,
                static_cast<unsigned long long>(now - m_DebugImageDumpStartMs));
            return true;
        }

        uint32_t BuildMutableViewFormats(VkFormat format, std::array<VkFormat, 2>& formats) const
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
                formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
                formats[1] = VK_FORMAT_B8G8R8A8_SRGB;
                return 2;
            case VK_FORMAT_R8G8B8A8_UNORM:
                formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
                formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
                return 2;
            default:
                formats[0] = format;
                return 1;
            }
        }

        VkSampleCountFlagBits SampleCountFromDesc(uint32_t sampleCount) const
        {
            switch (sampleCount)
            {
            case 1: return VK_SAMPLE_COUNT_1_BIT;
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 4: return VK_SAMPLE_COUNT_4_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            case 16: return VK_SAMPLE_COUNT_16_BIT;
            default: return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        bool ImportGameEyeTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& desc, uint32_t generation)
        {
            VulkanGameEyeTexture& eye = m_GameEyes[eyeIndex];
            if (eye.generation == generation && eye.kmtHandle == desc.kmtHandle && eye.image != VK_NULL_HANDLE)
                return true;

            DestroyImportedGameEye(eye);
            if (!desc.valid || desc.kmtHandle == 0 || desc.width == 0 || desc.height == 0)
            {
                m_Log.Print("Shared texture eye=%u is invalid for Vulkan import", eyeIndex);
                return false;
            }

            const VkExternalMemoryHandleTypeFlagBits handleType = desc.handleType != 0
                ? static_cast<VkExternalMemoryHandleTypeFlagBits>(desc.handleType)
                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
            const VkFormat format = static_cast<VkFormat>(desc.format);
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(desc.kmtHandle));

            std::array<VkFormat, 2> viewFormats = {};
            const uint32_t viewFormatCount = BuildMutableViewFormats(format, viewFormats);

            VkImageFormatListCreateInfo formatList{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
            formatList.viewFormatCount = viewFormatCount;
            formatList.pViewFormats = viewFormats.data();

            VkExternalMemoryImageCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            externalInfo.pNext = viewFormatCount > 1 ? &formatList : nullptr;
            externalInfo.handleTypes = handleType;

            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.pNext = &externalInfo;
            imageInfo.flags = viewFormatCount > 1 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { desc.width, desc.height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = SampleCountFromDesc(desc.sampleCount);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage importedImage = VK_NULL_HANDLE;
            VkResult vkResult = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            if (vkResult != VK_SUCCESS && imageInfo.flags != 0)
            {
                imageInfo.flags = 0;
                externalInfo.pNext = nullptr;
                vkResult = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            }
            if (vkResult != VK_SUCCESS || importedImage == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateImage import eye=%u handle=0x%llX format=%u size=%ux%u failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle), desc.format, desc.width, desc.height,
                    VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkMemoryRequirements memoryRequirements{};
            m_Vk.vkGetImageMemoryRequirements(m_VkDevice, importedImage, &memoryRequirements);

            uint32_t memoryTypeIndex = 0;
            uint32_t typeBits = memoryRequirements.memoryTypeBits;
            if (handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT)
            {
                VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
                vkResult = m_Vk.vkGetMemoryWin32HandlePropertiesKHR(m_VkDevice, handleType, handle, &handleProperties);
                if (vkResult != VK_SUCCESS)
                {
                    m_Log.Print("vkGetMemoryWin32HandlePropertiesKHR eye=%u handle=0x%llX type=0x%X failed: %s (%d)",
                        eyeIndex, static_cast<unsigned long long>(desc.kmtHandle), desc.handleType,
                        VkResultName(vkResult), static_cast<int>(vkResult));
                    m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                    return false;
                }
                typeBits &= handleProperties.memoryTypeBits;
            }
            else
            {
                m_Log.Print("Skipping memory handle properties query for KMT eye=%u; using image memoryTypeBits=0x%X",
                    eyeIndex, memoryRequirements.memoryTypeBits);
            }
            if (!FindMemoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryTypeIndex))
            {
                m_Log.Print("No compatible memory type for imported eye=%u typeBits=0x%X imageMemBits=0x%X",
                    eyeIndex, typeBits, memoryRequirements.memoryTypeBits);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            importInfo.handleType = handleType;
            importInfo.handle = handle;
            VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = importedImage;
            VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocInfo.pNext = &dedicatedInfo;
            allocInfo.allocationSize = memoryRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            VkDeviceMemory importedMemory = VK_NULL_HANDLE;
            vkResult = m_Vk.vkAllocateMemory(m_VkDevice, &allocInfo, nullptr, &importedMemory);
            if (vkResult != VK_SUCCESS || importedMemory == VK_NULL_HANDLE)
            {
                m_Log.Print("vkAllocateMemory import eye=%u handle=0x%llX size=%llu type=%u failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle),
                    static_cast<unsigned long long>(memoryRequirements.size), memoryTypeIndex,
                    VkResultName(vkResult), static_cast<int>(vkResult));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            vkResult = m_Vk.vkBindImageMemory(m_VkDevice, importedImage, importedMemory, 0);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkBindImageMemory import eye=%u handle=0x%llX failed: %s (%d)",
                    eyeIndex, static_cast<unsigned long long>(desc.kmtHandle), VkResultName(vkResult), static_cast<int>(vkResult));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImageView importedView = VK_NULL_HANDLE;
            VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = importedImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            vkResult = m_Vk.vkCreateImageView(m_VkDevice, &viewInfo, nullptr, &importedView);
            if (vkResult != VK_SUCCESS || importedView == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateImageView import eye=%u failed: %s (%d)",
                    eyeIndex, VkResultName(vkResult), static_cast<int>(vkResult));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            eye.generation = generation;
            eye.kmtHandle = desc.kmtHandle;
            eye.width = desc.width;
            eye.height = desc.height;
            eye.format = format;
            eye.image = importedImage;
            eye.view = importedView;
            eye.memory = importedMemory;
            eye.layout = VK_IMAGE_LAYOUT_GENERAL;
            eye.uMin = std::clamp(desc.uMin, 0.0f, 1.0f);
            eye.vMin = std::clamp(desc.vMin, 0.0f, 1.0f);
            eye.uMax = std::clamp(desc.uMax, 0.0f, 1.0f);
            eye.vMax = std::clamp(desc.vMax, 0.0f, 1.0f);
            eye.renderFovXDeg = (std::isfinite(desc.renderFovXDeg) && desc.renderFovXDeg > 1.0f && desc.renderFovXDeg < 179.0f)
                ? desc.renderFovXDeg
                : 90.0f;
            eye.renderAspect = (std::isfinite(desc.renderAspect) && desc.renderAspect > 0.1f && desc.renderAspect < 10.0f)
                ? desc.renderAspect
                : ((desc.height > 0) ? (static_cast<float>(desc.width) / static_cast<float>(desc.height)) : 1.0f);
            if (eye.uMax <= eye.uMin)
            {
                eye.uMin = 0.0f;
                eye.uMax = 1.0f;
            }
            if (eye.vMax <= eye.vMin)
            {
                eye.vMin = 0.0f;
                eye.vMax = 1.0f;
            }

            if (!UpdateBlitDescriptorSet(eyeIndex))
                return false;

            m_Log.Print("Imported Vulkan shared eye texture eye=%s(%u) gen=%u handle=0x%llX image=0x%llX size=%ux%u format=%u layout=%u bounds=(%.3f %.3f %.3f %.3f) projection=(fovX=%.2f aspect=%.4f) memorySize=%llu",
                EyeName(eyeIndex), eyeIndex, generation, static_cast<unsigned long long>(desc.kmtHandle),
                static_cast<unsigned long long>(desc.image), desc.width, desc.height, desc.format,
                static_cast<unsigned int>(eye.layout),
                eye.uMin, eye.vMin, eye.uMax, eye.vMax,
                eye.renderFovXDeg, eye.renderAspect,
                static_cast<unsigned long long>(memoryRequirements.size));
            return true;
        }

        uint32_t OverlayBlitDescriptorIndex(uint32_t overlayIndex) const
        {
            return L4D2VR_OPENXR_EYE_COUNT + overlayIndex;
        }

        bool UpdateBlitDescriptorSet(VkDescriptorSet descriptorSet, const VulkanGameEyeTexture& texture)
        {
            if (!m_UseShaderBlit)
                return true;
            if (texture.view == VK_NULL_HANDLE || descriptorSet == VK_NULL_HANDLE)
                return false;

            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = m_BlitSampler;
            imageInfo.imageView = texture.view;
            imageInfo.imageLayout = texture.layout;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = descriptorSet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            m_Vk.vkUpdateDescriptorSets(m_VkDevice, 1, &write, 0, nullptr);
            return true;
        }

        bool UpdateBlitDescriptorSet(uint32_t eyeIndex)
        {
            if (!m_UseShaderBlit)
                return true;
            if (eyeIndex >= L4D2VR_OPENXR_EYE_COUNT)
                return false;
            return UpdateBlitDescriptorSet(m_BlitDescriptorSets[eyeIndex], m_GameEyes[eyeIndex]);
        }

        bool UpdateOverlayBlitDescriptorSet(uint32_t overlayIndex)
        {
            if (!m_UseShaderBlit)
                return true;
            const uint32_t descriptorIndex = OverlayBlitDescriptorIndex(overlayIndex);
            if (overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT || descriptorIndex >= m_BlitDescriptorSets.size())
                return false;
            return UpdateBlitDescriptorSet(m_BlitDescriptorSets[descriptorIndex], m_OverlayTextures[overlayIndex]);
        }

        bool ImportSharedGameTexturesIfNeeded()
        {
            const uint32_t generation = m_Bridge.SharedTextureGeneration();
            if (generation == 0)
                return false;
            for (uint32_t eyeIndex = 0; eyeIndex < L4D2VR_OPENXR_EYE_COUNT; ++eyeIndex)
            {
                if (!ImportGameEyeTexture(eyeIndex, m_Bridge.SharedTexture(eyeIndex), generation))
                    return false;
            }
            return true;
        }

        bool ImportOverlayTexture(uint32_t overlayIndex, const L4D2VROpenXrOverlayDesc& overlay, uint32_t generation)
        {
            if (overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT)
                return false;

            const L4D2VROpenXrSharedTextureDesc& desc = overlay.texture;
            VulkanGameEyeTexture& texture = m_OverlayTextures[overlayIndex];
            if (texture.image != VK_NULL_HANDLE &&
                texture.kmtHandle == desc.kmtHandle &&
                texture.width == desc.width &&
                texture.height == desc.height &&
                texture.format == static_cast<VkFormat>(desc.format))
            {
                texture.generation = generation;
                texture.uMin = std::clamp(desc.uMin, 0.0f, 1.0f);
                texture.vMin = std::clamp(desc.vMin, 0.0f, 1.0f);
                texture.uMax = std::clamp(desc.uMax, 0.0f, 1.0f);
                texture.vMax = std::clamp(desc.vMax, 0.0f, 1.0f);
                if (texture.uMax <= texture.uMin)
                {
                    texture.uMin = 0.0f;
                    texture.uMax = 1.0f;
                }
                if (texture.vMax <= texture.vMin)
                {
                    texture.vMin = 0.0f;
                    texture.vMax = 1.0f;
                }
                return true;
            }

            DestroyImportedGameEye(texture);
            if (!overlay.valid || !overlay.visible || !desc.valid || desc.kmtHandle == 0 || desc.width == 0 || desc.height == 0)
                return false;

            const VkExternalMemoryHandleTypeFlagBits handleType = desc.handleType != 0
                ? static_cast<VkExternalMemoryHandleTypeFlagBits>(desc.handleType)
                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
            const VkFormat format = static_cast<VkFormat>(desc.format);
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(desc.kmtHandle));

            std::array<VkFormat, 2> viewFormats = {};
            const uint32_t viewFormatCount = BuildMutableViewFormats(format, viewFormats);

            VkImageFormatListCreateInfo formatList{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
            formatList.viewFormatCount = viewFormatCount;
            formatList.pViewFormats = viewFormats.data();

            VkExternalMemoryImageCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            externalInfo.pNext = viewFormatCount > 1 ? &formatList : nullptr;
            externalInfo.handleTypes = handleType;

            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.pNext = &externalInfo;
            imageInfo.flags = viewFormatCount > 1 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { desc.width, desc.height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = SampleCountFromDesc(desc.sampleCount);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage importedImage = VK_NULL_HANDLE;
            VkResult vkResult = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            if (vkResult != VK_SUCCESS && imageInfo.flags != 0)
            {
                imageInfo.flags = 0;
                externalInfo.pNext = nullptr;
                vkResult = m_Vk.vkCreateImage(m_VkDevice, &imageInfo, nullptr, &importedImage);
            }
            if (vkResult != VK_SUCCESS || importedImage == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateImage import overlay=%u handle=0x%llX format=%u size=%ux%u failed: %s (%d)",
                    overlayIndex, static_cast<unsigned long long>(desc.kmtHandle), desc.format, desc.width, desc.height,
                    VkResultName(vkResult), static_cast<int>(vkResult));
                return false;
            }

            VkMemoryRequirements memoryRequirements{};
            m_Vk.vkGetImageMemoryRequirements(m_VkDevice, importedImage, &memoryRequirements);

            uint32_t memoryTypeIndex = 0;
            uint32_t typeBits = memoryRequirements.memoryTypeBits;
            if (handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT)
            {
                VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
                vkResult = m_Vk.vkGetMemoryWin32HandlePropertiesKHR(m_VkDevice, handleType, handle, &handleProperties);
                if (vkResult != VK_SUCCESS)
                {
                    m_Log.Print("vkGetMemoryWin32HandlePropertiesKHR overlay=%u handle=0x%llX type=0x%X failed: %s (%d)",
                        overlayIndex, static_cast<unsigned long long>(desc.kmtHandle), desc.handleType,
                        VkResultName(vkResult), static_cast<int>(vkResult));
                    m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                    return false;
                }
                typeBits &= handleProperties.memoryTypeBits;
            }

            if (!FindMemoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryTypeIndex))
            {
                m_Log.Print("No compatible memory type for imported overlay=%u typeBits=0x%X imageMemBits=0x%X",
                    overlayIndex, typeBits, memoryRequirements.memoryTypeBits);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            importInfo.handleType = handleType;
            importInfo.handle = handle;
            VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = importedImage;
            VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocInfo.pNext = &dedicatedInfo;
            allocInfo.allocationSize = memoryRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            VkDeviceMemory importedMemory = VK_NULL_HANDLE;
            vkResult = m_Vk.vkAllocateMemory(m_VkDevice, &allocInfo, nullptr, &importedMemory);
            if (vkResult != VK_SUCCESS || importedMemory == VK_NULL_HANDLE)
            {
                m_Log.Print("vkAllocateMemory import overlay=%u handle=0x%llX size=%llu type=%u failed: %s (%d)",
                    overlayIndex, static_cast<unsigned long long>(desc.kmtHandle),
                    static_cast<unsigned long long>(memoryRequirements.size), memoryTypeIndex,
                    VkResultName(vkResult), static_cast<int>(vkResult));
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            vkResult = m_Vk.vkBindImageMemory(m_VkDevice, importedImage, importedMemory, 0);
            if (vkResult != VK_SUCCESS)
            {
                m_Log.Print("vkBindImageMemory import overlay=%u handle=0x%llX failed: %s (%d)",
                    overlayIndex, static_cast<unsigned long long>(desc.kmtHandle), VkResultName(vkResult), static_cast<int>(vkResult));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            VkImageView importedView = VK_NULL_HANDLE;
            VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = importedImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            vkResult = m_Vk.vkCreateImageView(m_VkDevice, &viewInfo, nullptr, &importedView);
            if (vkResult != VK_SUCCESS || importedView == VK_NULL_HANDLE)
            {
                m_Log.Print("vkCreateImageView import overlay=%u failed: %s (%d)",
                    overlayIndex, VkResultName(vkResult), static_cast<int>(vkResult));
                m_Vk.vkFreeMemory(m_VkDevice, importedMemory, nullptr);
                m_Vk.vkDestroyImage(m_VkDevice, importedImage, nullptr);
                return false;
            }

            texture.generation = generation;
            texture.kmtHandle = desc.kmtHandle;
            texture.width = desc.width;
            texture.height = desc.height;
            texture.format = format;
            texture.image = importedImage;
            texture.view = importedView;
            texture.memory = importedMemory;
            texture.layout = VK_IMAGE_LAYOUT_GENERAL;
            texture.uMin = std::clamp(desc.uMin, 0.0f, 1.0f);
            texture.vMin = std::clamp(desc.vMin, 0.0f, 1.0f);
            texture.uMax = std::clamp(desc.uMax, 0.0f, 1.0f);
            texture.vMax = std::clamp(desc.vMax, 0.0f, 1.0f);
            if (texture.uMax <= texture.uMin)
            {
                texture.uMin = 0.0f;
                texture.uMax = 1.0f;
            }
            if (texture.vMax <= texture.vMin)
            {
                texture.vMin = 0.0f;
                texture.vMax = 1.0f;
            }

            return true;
        }

        void DestroyOverlaySwapchain(VulkanEyeSwapchain& swapchain)
        {
            DestroySwapchainRenderTargets(swapchain);
            if (swapchain.handle != XR_NULL_HANDLE && m_Xr.xrDestroySwapchain)
                m_Xr.xrDestroySwapchain(swapchain.handle);
            swapchain = VulkanEyeSwapchain{};
        }

        bool EnsureOverlaySwapchain(uint32_t overlayIndex, const L4D2VROpenXrOverlayDesc& overlay)
        {
            if (overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT)
                return false;
            if (!overlay.valid || !overlay.visible || !overlay.texture.valid)
                return false;

            VulkanEyeSwapchain& swapchain = m_OverlaySwapchains[overlayIndex];
            const uint32_t width = std::clamp(overlay.texture.width, 64u, 4096u);
            const uint32_t height = std::clamp(overlay.texture.height, 64u, 4096u);
            const VkFormat format = m_SelectedSwapchainFormat != VK_FORMAT_UNDEFINED
                ? m_SelectedSwapchainFormat
                : (!m_Eyes.empty() ? m_Eyes[0].format : VK_FORMAT_B8G8R8A8_UNORM);

            if (swapchain.handle != XR_NULL_HANDLE &&
                swapchain.width == width &&
                swapchain.height == height &&
                swapchain.format == format)
            {
                return true;
            }

            DestroyOverlaySwapchain(swapchain);

            swapchain.width = width;
            swapchain.height = height;
            swapchain.format = format;

            XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            createInfo.format = static_cast<int64_t>(format);
            createInfo.sampleCount = 1;
            createInfo.width = swapchain.width;
            createInfo.height = swapchain.height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;

            XrResult xrResult = m_Xr.xrCreateSwapchain(m_Session, &createInfo, &swapchain.handle);
            if (!Succeeded(m_Log, "xrCreateSwapchain(Vulkan overlay)", xrResult) || swapchain.handle == XR_NULL_HANDLE)
                return false;

            uint32_t imageCount = 0;
            xrResult = m_Xr.xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
            if (!Succeeded(m_Log, "xrEnumerateSwapchainImages(overlay count)", xrResult) || imageCount == 0)
                return false;

            swapchain.images.assign(imageCount, XrSwapchainImageVulkanKHR{ XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
            xrResult = m_Xr.xrEnumerateSwapchainImages(
                swapchain.handle,
                imageCount,
                &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
            if (!Succeeded(m_Log, "xrEnumerateSwapchainImages(overlay data)", xrResult) || imageCount == 0)
                return false;

            swapchain.images.resize(imageCount);
            swapchain.layouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
            if (!CreateSwapchainRenderTargets(swapchain))
                return false;

            return true;
        }

        bool RenderOverlaySwapchain(uint32_t overlayIndex, const L4D2VROpenXrOverlayDesc& overlay)
        {
            if (overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT)
                return false;
            if (!ImportOverlayTexture(overlayIndex, overlay, m_Bridge.OverlayGeneration()))
                return false;
            if (!EnsureOverlaySwapchain(overlayIndex, overlay))
                return false;

            VulkanEyeSwapchain& swapchain = m_OverlaySwapchains[overlayIndex];
            VulkanGameEyeTexture& source = m_OverlayTextures[overlayIndex];
            if (m_UseShaderBlit && !UpdateOverlayBlitDescriptorSet(overlayIndex))
                return false;

            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t imageIndex = 0;
            XrResult xrResult = m_Xr.xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex);
            if (!Succeeded(m_Log, "xrAcquireSwapchainImage(overlay)", xrResult))
                return false;

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            xrResult = m_Xr.xrWaitSwapchainImage(swapchain.handle, &waitInfo);
            if (!Succeeded(m_Log, "xrWaitSwapchainImage(overlay)", xrResult))
                return false;

            const VkImage dstImage = swapchain.images[imageIndex].image;
            VkResult vkResult = m_Vk.vkResetCommandBuffer(m_CommandBuffer, 0);
            if (vkResult != VK_SUCCESS)
                return false;

            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResult = m_Vk.vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
            if (vkResult != VK_SUCCESS)
                return false;

            if (m_UseShaderBlit)
            {
                CmdTransitionImage(dstImage, swapchain.layouts[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                if (!CmdRenderOverlayShaderBlit(overlayIndex, swapchain, imageIndex, source))
                    return false;
                swapchain.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            else
            {
                CmdTransitionImage(dstImage, swapchain.layouts[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                const int32_t srcX0 = static_cast<int32_t>(std::floor(source.uMin * static_cast<float>(source.width)));
                const int32_t srcY0 = static_cast<int32_t>(std::floor(source.vMin * static_cast<float>(source.height)));
                const int32_t srcX1 = static_cast<int32_t>(std::ceil(source.uMax * static_cast<float>(source.width)));
                const int32_t srcY1 = static_cast<int32_t>(std::ceil(source.vMax * static_cast<float>(source.height)));
                const int32_t srcMinX = std::clamp(srcX0, 0, static_cast<int32_t>(source.width) - 1);
                const int32_t srcMinY = std::clamp(srcY0, 0, static_cast<int32_t>(source.height) - 1);
                const int32_t srcMaxX = std::clamp(srcX1, srcMinX + 1, static_cast<int32_t>(source.width));
                const int32_t srcMaxY = std::clamp(srcY1, srcMinY + 1, static_cast<int32_t>(source.height));

                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[0] = { srcMinX, srcMinY, 0 };
                blit.srcOffsets[1] = { srcMaxX, srcMaxY, 1 };
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[1] = { static_cast<int32_t>(swapchain.width), static_cast<int32_t>(swapchain.height), 1 };
                m_Vk.vkCmdBlitImage(m_CommandBuffer, source.image, source.layout,
                    dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                CmdTransitionImage(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                swapchain.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            vkResult = m_Vk.vkEndCommandBuffer(m_CommandBuffer);
            if (vkResult != VK_SUCCESS)
                return false;

            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CommandBuffer;
            vkResult = m_Vk.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            if (vkResult != VK_SUCCESS)
                return false;
            vkResult = m_Vk.vkQueueWaitIdle(m_GraphicsQueue);
            if (vkResult != VK_SUCCESS)
                return false;

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrResult = m_Xr.xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
            return Succeeded(m_Log, "xrReleaseSwapchainImage(overlay)", xrResult);
        }

        struct OverlayAnchor
        {
            float yaw = 0.0f;
            XrQuaternionf yawOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };
            XrVector3f center{ 0.0f, 0.0f, -3.0f };
            XrVector3f forward{ 0.0f, 0.0f, -1.0f };
            XrVector3f right{ 1.0f, 0.0f, 0.0f };
            XrVector3f up{ 0.0f, 1.0f, 0.0f };
        };

        OverlayAnchor BuildOverlayAnchor(
            const L4D2VROpenXrOverlayDesc& overlay,
            const std::vector<XrView>& locatedViews,
            uint32_t locatedCount,
            const XrPosef* gameRenderCenterPose = nullptr)
        {
            OverlayAnchor anchor{};
            if (!gameRenderCenterPose && locatedViews.empty())
                return anchor;

            XrVector3f hmdPosition{};
            XrQuaternionf hmdOrientation{};
            if (gameRenderCenterPose)
            {
                hmdPosition = gameRenderCenterPose->position;
                hmdOrientation = gameRenderCenterPose->orientation;
            }
            else
            {
                hmdPosition = locatedViews[0].pose.position;
                hmdOrientation = locatedViews[0].pose.orientation;
                if (locatedCount >= 2 && locatedViews.size() >= 2)
                {
                    hmdPosition.x = (locatedViews[0].pose.position.x + locatedViews[1].pose.position.x) * 0.5f;
                    hmdPosition.y = (locatedViews[0].pose.position.y + locatedViews[1].pose.position.y) * 0.5f;
                    hmdPosition.z = (locatedViews[0].pose.position.z + locatedViews[1].pose.position.z) * 0.5f;
                }
            }

            anchor.yaw = ExtractOpenXrYaw(hmdOrientation);
            anchor.yawOrientation = MakeOpenXrYawQuaternion(anchor.yaw);
            anchor.forward = RotateOpenXrVector(anchor.yawOrientation, XrVector3f{ 0.0f, 0.0f, -1.0f });
            anchor.right = RotateOpenXrVector(anchor.yawOrientation, XrVector3f{ 1.0f, 0.0f, 0.0f });
            anchor.up = XrVector3f{ 0.0f, 1.0f, 0.0f };
            const float distance = (std::isfinite(overlay.distanceMeters) && overlay.distanceMeters > 0.1f && overlay.distanceMeters < 10.0f)
                ? overlay.distanceMeters
                : 3.0f;

            anchor.center.x = hmdPosition.x + anchor.forward.x * distance + anchor.right.x * overlay.offsetMeters[0] + anchor.up.x * overlay.offsetMeters[1] + anchor.forward.x * overlay.offsetMeters[2];
            anchor.center.y = hmdPosition.y + anchor.forward.y * distance + anchor.right.y * overlay.offsetMeters[0] + anchor.up.y * overlay.offsetMeters[1] + anchor.forward.y * overlay.offsetMeters[2];
            anchor.center.z = hmdPosition.z + anchor.forward.z * distance + anchor.right.z * overlay.offsetMeters[0] + anchor.up.z * overlay.offsetMeters[1] + anchor.forward.z * overlay.offsetMeters[2];
            return anchor;
        }

        XrPosef BuildOverlayPose(
            const L4D2VROpenXrOverlayDesc& overlay,
            const std::vector<XrView>& locatedViews,
            uint32_t locatedCount,
            const XrPosef* gameRenderCenterPose = nullptr)
        {
            const OverlayAnchor anchor = BuildOverlayAnchor(overlay, locatedViews, locatedCount, gameRenderCenterPose);
            XrPosef pose{ anchor.yawOrientation, anchor.center };
            return pose;
        }

        XrPosef BuildCurvedOverlaySlicePose(
            const L4D2VROpenXrOverlayDesc& overlay,
            const std::vector<XrView>& locatedViews,
            uint32_t locatedCount,
            float sliceAngle,
            float radiusMeters,
            const XrPosef* gameRenderCenterPose = nullptr)
        {
            const OverlayAnchor anchor = BuildOverlayAnchor(overlay, locatedViews, locatedCount, gameRenderCenterPose);
            const float sideOffset = radiusMeters * std::sin(sliceAngle);
            const float viewerOffset = radiusMeters * (1.0f - std::cos(sliceAngle));

            XrPosef pose{};
            pose.orientation = MakeOpenXrYawQuaternion(anchor.yaw - sliceAngle);
            pose.position.x = anchor.center.x + anchor.right.x * sideOffset - anchor.forward.x * viewerOffset;
            pose.position.y = anchor.center.y + anchor.right.y * sideOffset - anchor.forward.y * viewerOffset;
            pose.position.z = anchor.center.z + anchor.right.z * sideOffset - anchor.forward.z * viewerOffset;
            return pose;
        }

        void CmdTransitionImage(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess,
            VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            m_Vk.vkCmdPipelineBarrier(m_CommandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        bool CmdRenderShaderBlitWithDescriptor(VulkanEyeSwapchain& target, uint32_t imageIndex, const VulkanGameEyeTexture& source, VkDescriptorSet descriptorSet)
        {
            if (!m_UseShaderBlit ||
                imageIndex >= target.framebuffers.size() ||
                source.view == VK_NULL_HANDLE ||
                m_BlitPipeline == VK_NULL_HANDLE ||
                m_BlitPipelineLayout == VK_NULL_HANDLE ||
                descriptorSet == VK_NULL_HANDLE)
            {
                return false;
            }

            VkRenderPassBeginInfo beginInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            beginInfo.renderPass = m_BlitRenderPass;
            beginInfo.framebuffer = target.framebuffers[imageIndex];
            beginInfo.renderArea.extent = { target.width, target.height };

            m_Vk.vkCmdBeginRenderPass(m_CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width = static_cast<float>(target.width);
            viewport.height = static_cast<float>(target.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            m_Vk.vkCmdSetViewport(m_CommandBuffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.extent = { target.width, target.height };
            m_Vk.vkCmdSetScissor(m_CommandBuffer, 0, 1, &scissor);

            m_Vk.vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_BlitPipeline);
            m_Vk.vkCmdBindDescriptorSets(
                m_CommandBuffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_BlitPipelineLayout,
                0,
                1,
                &descriptorSet,
                0,
                nullptr);

            // Match bounded eye-submit semantics: sample the bounded source region
            // and stretch it into the full OpenXR swapchain image.
            const float bounds[4] = { source.uMin, source.vMin, source.uMax, source.vMax };
            m_Vk.vkCmdPushConstants(
                m_CommandBuffer,
                m_BlitPipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(bounds),
                bounds);

            m_Vk.vkCmdDraw(m_CommandBuffer, 3, 1, 0, 0);
            m_Vk.vkCmdEndRenderPass(m_CommandBuffer);
            return true;
        }

        bool CmdRenderShaderBlit(uint32_t eyeIndex, VulkanEyeSwapchain& eye, uint32_t imageIndex, const VulkanGameEyeTexture& source)
        {
            if (eyeIndex >= L4D2VR_OPENXR_EYE_COUNT)
                return false;
            return CmdRenderShaderBlitWithDescriptor(eye, imageIndex, source, m_BlitDescriptorSets[eyeIndex]);
        }

        bool CmdRenderOverlayShaderBlit(uint32_t overlayIndex, VulkanEyeSwapchain& swapchain, uint32_t imageIndex, const VulkanGameEyeTexture& source)
        {
            const uint32_t descriptorIndex = OverlayBlitDescriptorIndex(overlayIndex);
            if (overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT || descriptorIndex >= m_BlitDescriptorSets.size())
                return false;
            return CmdRenderShaderBlitWithDescriptor(swapchain, imageIndex, source, m_BlitDescriptorSets[descriptorIndex]);
        }

        bool RenderEye(uint32_t eyeIndex, uint32_t frameIndex, uint32_t sharedFrameId, bool waitForQueueIdle = true)
        {
            VulkanEyeSwapchain& eye = m_Eyes[eyeIndex];
            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t imageIndex = 0;
            XrResult xrResult = m_Xr.xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex);
            if (!Succeeded(m_Log, "xrAcquireSwapchainImage", xrResult))
                return false;
            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            xrResult = m_Xr.xrWaitSwapchainImage(eye.handle, &waitInfo);
            if (!Succeeded(m_Log, "xrWaitSwapchainImage", xrResult))
                return false;

            const VkImage dstImage = eye.images[imageIndex].image;
            VkResult vkResult = m_Vk.vkResetCommandBuffer(m_CommandBuffer, 0);
            if (vkResult != VK_SUCCESS)
                return false;
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResult = m_Vk.vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
            if (vkResult != VK_SUCCESS)
                return false;

            VulkanImageDumpTarget sourceDump;
            VulkanImageDumpTarget swapchainDump;
            if (m_Bridge.HasState())
            {
                const VulkanGameEyeTexture& source = m_GameEyes[eyeIndex];
                if (source.image == VK_NULL_HANDLE)
                    return false;
                const bool useShaderBlit = m_UseShaderBlit;
                static bool s_loggedBlitPath = false;
                if (!s_loggedBlitPath && eyeIndex == 0)
                {
                    s_loggedBlitPath = true;
                    m_Log.Print("Using %s Vulkan eye blit path for swapchain format=%u",
                        useShaderBlit ? "sRGB shader" : "transfer",
                        static_cast<unsigned int>(eye.format));
                }
                {
                    static uint32_t s_eyeBlitLogBudget = 48;
                    if (s_eyeBlitLogBudget > 0)
                    {
                        --s_eyeBlitLogBudget;
                        m_Log.Print(
                            "[OpenXR][EyeBlit] eye=%s(%u) frame=%u sourceGen=%u sourceHandle=0x%llX sourceImage=0x%llX sourceSize=%ux%u sourceFormat=%u sourceBounds=(%.4f %.4f %.4f %.4f) dstSwapchain=0x%llX dstImage=0x%llX dstImageIndex=%u dstSize=%ux%u path=%s",
                            EyeName(eyeIndex),
                            eyeIndex,
                            frameIndex,
                            source.generation,
                            static_cast<unsigned long long>(source.kmtHandle),
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(source.image)),
                            source.width,
                            source.height,
                            static_cast<unsigned int>(source.format),
                            source.uMin,
                            source.vMin,
                            source.uMax,
                            source.vMax,
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(eye.handle)),
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(dstImage)),
                            imageIndex,
                            eye.width,
                            eye.height,
                            useShaderBlit ? "shader" : "transfer");
                    }
                }
                if (useShaderBlit)
                {
                    CmdTransitionImage(dstImage, eye.layouts[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    if (!CmdRenderShaderBlit(eyeIndex, eye, imageIndex, source))
                        return false;
                    eye.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
                else
                {
                    CmdTransitionImage(dstImage, eye.layouts[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                    const int32_t srcX0 = static_cast<int32_t>(std::floor(source.uMin * static_cast<float>(source.width)));
                    const int32_t srcY0 = static_cast<int32_t>(std::floor(source.vMin * static_cast<float>(source.height)));
                    const int32_t srcX1 = static_cast<int32_t>(std::ceil(source.uMax * static_cast<float>(source.width)));
                    const int32_t srcY1 = static_cast<int32_t>(std::ceil(source.vMax * static_cast<float>(source.height)));
                    const int32_t srcMinX = std::clamp(srcX0, 0, static_cast<int32_t>(source.width) - 1);
                    const int32_t srcMinY = std::clamp(srcY0, 0, static_cast<int32_t>(source.height) - 1);
                    const int32_t srcMaxX = std::clamp(srcX1, srcMinX + 1, static_cast<int32_t>(source.width));
                    const int32_t srcMaxY = std::clamp(srcY1, srcMinY + 1, static_cast<int32_t>(source.height));
                    VkImageBlit blit{};
                    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    blit.srcSubresource.layerCount = 1;
                    blit.srcOffsets[0] = { srcMinX, srcMinY, 0 };
                    blit.srcOffsets[1] = { srcMaxX, srcMaxY, 1 };
                    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    blit.dstSubresource.layerCount = 1;
                    blit.dstOffsets[1] = { static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height), 1 };
                    m_Vk.vkCmdBlitImage(m_CommandBuffer, source.image, source.layout,
                        dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                    CmdTransitionImage(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    eye.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }

                if (ShouldDumpOpenXrDebugImages(eyeIndex, frameIndex, sharedFrameId))
                {
                    const uint32_t dumpFrameId = IsSyntheticOpenXrSharedFrame(sharedFrameId)
                        ? frameIndex
                        : sharedFrameId;
                    if (CreateImageDumpTarget(
                        "source",
                        eyeIndex,
                        dumpFrameId,
                        source.width,
                        source.height,
                        source.format,
                        sourceDump))
                    {
                        CmdCopyImageToDumpTarget(source.image, source.layout, source.layout, sourceDump);
                    }
                    if (eye.supportsTransferSrc && CreateImageDumpTarget(
                        "swapchain",
                        eyeIndex,
                        dumpFrameId,
                        eye.width,
                        eye.height,
                        eye.format,
                        swapchainDump))
                    {
                        CmdCopyImageToDumpTarget(dstImage, eye.layouts[imageIndex], eye.layouts[imageIndex], swapchainDump);
                    }
                    else if (!eye.supportsTransferSrc)
                    {
                        m_Log.Print("[OpenXR][ImageDump] skip swapchain eye=%s(%u); runtime swapchain lacks transfer-src usage",
                            EyeName(eyeIndex), eyeIndex);
                    }
                }
            }
            else
            {
                CmdTransitionImage(dstImage, eye.layouts[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                const float pulse = static_cast<float>((frameIndex % 180) / 179.0);
                const VkClearColorValue color = eyeIndex == 0
                    ? VkClearColorValue{ { 0.02f + 0.20f * pulse, 0.05f, 0.85f, 1.0f } }
                    : VkClearColorValue{ { 0.85f, 0.05f + 0.20f * pulse, 0.02f, 1.0f } };
                VkImageSubresourceRange range{};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.levelCount = 1;
                range.layerCount = 1;
                m_Vk.vkCmdClearColorImage(m_CommandBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

                CmdTransitionImage(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                eye.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            vkResult = m_Vk.vkEndCommandBuffer(m_CommandBuffer);
            if (vkResult != VK_SUCCESS)
            {
                DestroyImageDumpTarget(sourceDump);
                DestroyImageDumpTarget(swapchainDump);
                return false;
            }
            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CommandBuffer;
            vkResult = m_Vk.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            if (vkResult != VK_SUCCESS)
            {
                DestroyImageDumpTarget(sourceDump);
                DestroyImageDumpTarget(swapchainDump);
                return false;
            }
            if (waitForQueueIdle || sourceDump.buffer != VK_NULL_HANDLE || swapchainDump.buffer != VK_NULL_HANDLE)
            {
                vkResult = m_Vk.vkQueueWaitIdle(m_GraphicsQueue);
                if (vkResult != VK_SUCCESS)
                {
                    DestroyImageDumpTarget(sourceDump);
                    DestroyImageDumpTarget(swapchainDump);
                    return false;
                }
            }
            CompleteImageDumpTarget(sourceDump);
            CompleteImageDumpTarget(swapchainDump);

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrResult = m_Xr.xrReleaseSwapchainImage(eye.handle, &releaseInfo);
            return Succeeded(m_Log, "xrReleaseSwapchainImage", xrResult);
        }

        XrRect2Di BuildProjectionImageRect(const VulkanGameEyeTexture& source, const VulkanEyeSwapchain& eye)
        {
            const float u0 = std::clamp(std::min(source.uMin, source.uMax), 0.0f, 1.0f);
            const float u1 = std::clamp(std::max(source.uMin, source.uMax), 0.0f, 1.0f);
            const float v0 = std::clamp(std::min(source.vMin, source.vMax), 0.0f, 1.0f);
            const float v1 = std::clamp(std::max(source.vMin, source.vMax), 0.0f, 1.0f);

            const int32_t width = static_cast<int32_t>(eye.width);
            const int32_t height = static_cast<int32_t>(eye.height);
            if (width <= 0 || height <= 0)
                return XrRect2Di{ { 0, 0 }, { width, height } };

            const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(u0 * static_cast<float>(width) + 0.5f)), 0, width - 1);
            const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(v0 * static_cast<float>(height) + 0.5f)), 0, height - 1);
            const int32_t x1 = std::clamp(static_cast<int32_t>(std::floor(u1 * static_cast<float>(width) + 0.5f)), x0 + 1, width);
            const int32_t y1 = std::clamp(static_cast<int32_t>(std::floor(v1 * static_cast<float>(height) + 0.5f)), y0 + 1, height);
            return XrRect2Di{ { x0, y0 }, { x1 - x0, y1 - y0 } };
        }

        XrFovf MirrorProjectionFovHorizontal(const XrFovf& fov) const
        {
            XrFovf mirrored = fov;
            mirrored.angleLeft = -fov.angleRight;
            mirrored.angleRight = -fov.angleLeft;
            return mirrored;
        }

        bool IsFullSourceBounds(const VulkanGameEyeTexture& source) const
        {
            constexpr float kEpsilon = 0.0005f;
            return std::fabs(source.uMin) <= kEpsilon &&
                std::fabs(source.vMin) <= kEpsilon &&
                std::fabs(source.uMax - 1.0f) <= kEpsilon &&
                std::fabs(source.vMax - 1.0f) <= kEpsilon;
        }

        XrFovf BuildGameProjectionFov(
            const VulkanGameEyeTexture& source,
            const XrFovf& runtimeFov,
            uint32_t eyeIndex,
            bool useSymmetricProjectionFov)
        {
            XrFovf gameFov{};
            const bool haveGameFov =
                m_Bridge.HasState() &&
                TryBuildSymmetricProjectionFov(source.renderFovXDeg, source.renderAspect, gameFov);
            const bool fullSourceBounds = IsFullSourceBounds(source);
            const bool useGameProjectionFov =
                haveGameFov &&
                (useSymmetricProjectionFov || fullSourceBounds);

            static bool s_loggedProjection = false;
            if (!s_loggedProjection && eyeIndex == 0)
            {
                s_loggedProjection = true;
                const char* submitFovMode =
                    useGameProjectionFov
                        ? (useSymmetricProjectionFov ? "symmetric-game" : "auto-full-source-game")
                        : "runtime";
                m_Log.Print(
                    "Using OpenXR projection FOV; sourceBounds=(%.4f %.4f %.4f %.4f) fullSourceBounds=%u useSymmetricProjectionFov=%u autoFullSourceGameFov=%u gameProjectionValid=%u submitFov=%s gameProjection=(fovX=%.2f aspect=%.4f L=%.4f R=%.4f U=%.4f D=%.4f) runtimeLeftEyeFov=(L=%.4f R=%.4f U=%.4f D=%.4f)",
                    source.uMin,
                    source.vMin,
                    source.uMax,
                    source.vMax,
                    fullSourceBounds ? 1u : 0u,
                    useSymmetricProjectionFov ? 1u : 0u,
                    (fullSourceBounds && haveGameFov && !useSymmetricProjectionFov) ? 1u : 0u,
                    haveGameFov ? 1u : 0u,
                    submitFovMode,
                    source.renderFovXDeg,
                    source.renderAspect,
                    gameFov.angleLeft,
                    gameFov.angleRight,
                    gameFov.angleUp,
                    gameFov.angleDown,
                    runtimeFov.angleLeft,
                    runtimeFov.angleRight,
                    runtimeFov.angleUp,
                    runtimeFov.angleDown);
            }

            if (useGameProjectionFov)
                return gameFov;
            return runtimeFov;
        }

        int FrameLoop(const Options& options)
        {
            const ULONGLONG startTicks = GetTickCount64();
            uint32_t submittedFrames = 0;
            bool shouldExit = false;
            const bool requireSharedTextures = m_Bridge.HasState();
            uint32_t lastLoggedSharedTextureGeneration = 0;
            uint32_t lastSubmittedSharedTextureFrameGeneration = 0;
            uint32_t lastSubmittedOverlayFrameGeneration = 0;
            L4D2VROpenXrPoseDesc lastGameRenderPose{};
            uint32_t lastGameRenderPoseGeneration = 0;
            bool eyeSwapchainsHaveContent = false;
            ULONGLONG lastWaitingTextureLog = 0;
            ULONGLONG lastWaitingFrameLog = 0;
            m_Log.Print("Entering Vulkan frame loop targetFrames=%u waitReadySeconds=%u parentPid=%lu requireSharedTextures=%u",
                options.targetFrames, options.waitReadySeconds, static_cast<unsigned long>(options.parentPid), requireSharedTextures ? 1u : 0u);

            while (!shouldExit)
            {
                if (!ParentStillAlive())
                    break;
                if (!PollEvents(shouldExit))
                    return 20;
                if (shouldExit)
                    break;
                if (!m_SessionRunning)
                {
                    const ULONGLONG elapsedMs = GetTickCount64() - startTicks;
                    if (options.waitReadySeconds > 0 && elapsedMs > static_cast<ULONGLONG>(options.waitReadySeconds) * 1000ull)
                        return 21;
                    Sleep(10);
                    continue;
                }
                const bool sharedTexturesReady = !requireSharedTextures || m_Bridge.SharedTexturesReady();
                if (requireSharedTextures && !sharedTexturesReady)
                {
                    const ULONGLONG now = GetTickCount64();
                    if (now - lastWaitingTextureLog > 1000ull)
                    {
                        m_Log.Print("Waiting for shared game eye textures");
                        m_Bridge.Update(L4D2VROpenXrBridgeStatus::WaitingForSharedTextures, 0, submittedFrames, "waiting for shared game eye textures");
                        lastWaitingTextureLog = now;
                    }
                }
                if (sharedTexturesReady)
                {
                    const uint32_t generation = m_Bridge.SharedTextureGeneration();
                    if (requireSharedTextures && generation != lastLoggedSharedTextureGeneration)
                    {
                        const auto left = m_Bridge.SharedTexture(L4D2VR_OPENXR_EYE_LEFT);
                        const auto right = m_Bridge.SharedTexture(L4D2VR_OPENXR_EYE_RIGHT);
                        m_Log.Print("Shared game eye textures ready gen=%u L(handle=0x%llX image=0x%llX %ux%u fmt=%u type=0x%X q=%u) R(handle=0x%llX image=0x%llX %ux%u fmt=%u type=0x%X q=%u)",
                            generation, static_cast<unsigned long long>(left.kmtHandle), static_cast<unsigned long long>(left.image),
                            left.width, left.height, left.format, left.handleType, left.queueFamilyIndex,
                            static_cast<unsigned long long>(right.kmtHandle), static_cast<unsigned long long>(right.image),
                            right.width, right.height, right.format, right.handleType, right.queueFamilyIndex);
                        lastLoggedSharedTextureGeneration = generation;
                    }
                    if (requireSharedTextures && !ImportSharedGameTexturesIfNeeded())
                        return 28;
                }

                uint32_t sharedTextureFrameId = 0;
                uint32_t sharedTextureFrameGeneration = 0;
                const bool sharedTextureFrameReady =
                    !requireSharedTextures ||
                    m_Bridge.ReadSharedTextureFrame(sharedTextureFrameId, &sharedTextureFrameGeneration);
                uint32_t overlayFrameId = 0;
                uint32_t overlayFrameGeneration = 0;
                const bool overlayFrameReady =
                    m_Bridge.ReadOverlayFrame(overlayFrameId, &overlayFrameGeneration);
                if (requireSharedTextures && sharedTexturesReady && !sharedTextureFrameReady)
                {
                    const ULONGLONG now = GetTickCount64();
                    if (now - lastWaitingFrameLog > 1000ull)
                    {
                        m_Log.Print("Waiting for resolved shared eye frame");
                        lastWaitingFrameLog = now;
                    }
                }

                XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
                XrFrameState frameState{ XR_TYPE_FRAME_STATE };
                XrResult result = m_Xr.xrWaitFrame(m_Session, &waitInfo, &frameState);
                if (!Succeeded(m_Log, "xrWaitFrame", result))
                    return 22;
                XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
                result = m_Xr.xrBeginFrame(m_Session, &beginInfo);
                if (!Succeeded(m_Log, "xrBeginFrame", result))
                    return 23;

                m_InputBridge.UpdateFrame(frameState.predictedDisplayTime, m_Bridge, m_Log);

                bool layerReady = false;
                uint32_t overlayLayerCount = 0;
                std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
                std::array<XrCompositionLayerQuad, kMaxOpenXrOverlayLayers> overlayLayers{};
                for (XrCompositionLayerQuad& overlayLayer : overlayLayers)
                    overlayLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                std::vector<XrView> locatedViews(m_ViewConfigs.size(), XrView{ XR_TYPE_VIEW });
                if (frameState.shouldRender)
                {
                    XrViewState viewState{ XR_TYPE_VIEW_STATE };
                    uint32_t locatedCount = 0;
                    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
                    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = m_AppSpace;
                    result = m_Xr.xrLocateViews(m_Session, &locateInfo, &viewState,
                        static_cast<uint32_t>(locatedViews.size()), &locatedCount, locatedViews.data());
                    if (!Succeeded(m_Log, "xrLocateViews", result))
                        return 24;
                    m_Bridge.PublishHmdPose(BuildHmdPoseFromLocatedViews(
                        locatedViews,
                        locatedCount,
                        viewState.viewStateFlags,
                        frameState.predictedDisplayTime));
                    static bool s_loggedRuntimeViewConfig = false;
                    if (locatedCount >= 2 && RuntimeViewConfigIsReady(m_SessionState))
                    {
                        const L4D2VROpenXrRuntimeViewConfigDesc runtimeViewConfig =
                            BuildRuntimeViewConfig(m_ViewConfigs, locatedViews, locatedCount);
                        if (!s_loggedRuntimeViewConfig && runtimeViewConfig.valid)
                        {
                            s_loggedRuntimeViewConfig = true;
                            LogRuntimeViewConfig(m_Log, "Vulkan", runtimeViewConfig);
                        }
                        m_Bridge.PublishRuntimeViewConfig(runtimeViewConfig);
                    }
                    L4D2VROpenXrPoseDesc gameRenderPose{};
                    uint32_t gameRenderPoseGeneration = 0;
                    const bool readGameRenderPose =
                        m_Bridge.ReadGameRenderPose(gameRenderPose, &gameRenderPoseGeneration);
                    if (readGameRenderPose)
                    {
                        lastGameRenderPose = gameRenderPose;
                        lastGameRenderPoseGeneration = gameRenderPoseGeneration;
                    }
                    const bool haveGameRenderPose = lastGameRenderPose.valid != 0;
                    const L4D2VROpenXrPoseDesc& projectionRenderPose =
                        haveGameRenderPose ? lastGameRenderPose : gameRenderPose;
                    const uint32_t projectionRenderPoseGeneration =
                        haveGameRenderPose ? lastGameRenderPoseGeneration : 0;
                    const bool useGameRenderPoseForProjection =
                        options.useGameRenderPoseForProjection && haveGameRenderPose;
                    const char* projectionPoseSource = useGameRenderPoseForProjection
                        ? (readGameRenderPose ? "game render" : "cached game render")
                        : "runtime located";
                    if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 && locatedCount >= 2)
                    {
                        const bool haveNewResolvedSharedFrame =
                            !requireSharedTextures ||
                            (sharedTextureFrameReady &&
                                sharedTextureFrameGeneration != lastSubmittedSharedTextureFrameGeneration);

                        if (sharedTexturesReady && haveNewResolvedSharedFrame)
                        {
                            for (uint32_t eye = 0; eye < 2; ++eye)
                            {
                                if (!RenderEye(eye, submittedFrames, sharedTextureFrameId))
                                    return 25;
                            }

                            eyeSwapchainsHaveContent = true;
                            if (requireSharedTextures)
                                lastSubmittedSharedTextureFrameGeneration = sharedTextureFrameGeneration;
                        }

                        if (sharedTexturesReady && eyeSwapchainsHaveContent)
                        {
                            for (uint32_t eye = 0; eye < 2; ++eye)
                            {
                                projectionViews[eye] = XrCompositionLayerProjectionView{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                                const uint32_t imageEye = SelectProjectionImageEye(options, eye);
                                const uint32_t projectionViewEye = SelectProjectionViewEye(options, eye);
                                float renderYaw = 0.0f;
                                float renderIpd = 0.0f;
                                projectionViews[eye].pose = useGameRenderPoseForProjection
                                    ? BuildProjectionPoseFromGameRenderPose(
                                        projectionRenderPose,
                                        locatedViews,
                                        locatedCount,
                                        projectionViewEye,
                                        &renderYaw,
                                        &renderIpd)
                                    : BuildProjectionPose(
                                        locatedViews,
                                        locatedCount,
                                        projectionViewEye,
                                        &renderYaw,
                                        &renderIpd);
                                projectionViews[eye].fov = BuildGameProjectionFov(
                                    m_GameEyes[imageEye],
                                    locatedViews[projectionViewEye].fov,
                                    projectionViewEye,
                                    options.useSymmetricProjectionFov);
                                if (options.mirrorProjectionHorizontal)
                                    projectionViews[eye].fov = MirrorProjectionFovHorizontal(projectionViews[eye].fov);
                                projectionViews[eye].subImage.swapchain = m_Eyes[imageEye].handle;
                                projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    static_cast<int32_t>(m_Eyes[imageEye].width),
                                    static_cast<int32_t>(m_Eyes[imageEye].height)
                                };
                                static bool s_loggedProjectionPose = false;
                                if (!s_loggedProjectionPose && eye == 0)
                                {
                                    s_loggedProjectionPose = true;
                                    const XrPosef& pose = projectionViews[eye].pose;
                                    m_Log.Print(
                                        "Using %s OpenXR projection pose for submit gameRenderGen=%u yawDeg=%.2f ipd=%.4f eye0Pos=(%.4f %.4f %.4f) eye0Quat=(%.4f %.4f %.4f %.4f)",
                                        projectionPoseSource,
                                        projectionRenderPoseGeneration,
                                        renderYaw * (180.0f / 3.141592654f),
                                        renderIpd,
                                        pose.position.x,
                                        pose.position.y,
                                        pose.position.z,
                                        pose.orientation.x,
                                        pose.orientation.y,
                                        pose.orientation.z,
                                        pose.orientation.w);
                                }
                                static bool s_loggedImageRect = false;
                                if (!s_loggedImageRect && eye == 0)
                                {
                                    s_loggedImageRect = true;
                                    const XrRect2Di& rect = projectionViews[eye].subImage.imageRect;
                                    m_Log.Print(
                                        "Using OpenXR submit source bounds viewEye=%s(%u) projectionViewEye=%s(%u) imageEye=%s(%u) swapProjectionEyes=%u swapProjectionViewOrder=%u mirrorProjectionHorizontal=%u useSymmetricProjectionFov=%u useGameRenderPoseForProjection=%u forceMonoProjectionEye=%s(%d) forceMonoProjectionView=%s(%d) bounds=(%.4f %.4f %.4f %.4f) imageRect=(%d,%d %dx%d) swapchain=%ux%u",
                                        EyeName(eye),
                                        eye,
                                        EyeName(projectionViewEye),
                                        projectionViewEye,
                                        EyeName(imageEye),
                                        imageEye,
                                        options.swapProjectionEyes ? 1u : 0u,
                                        options.swapProjectionViewOrder ? 1u : 0u,
                                        options.mirrorProjectionHorizontal ? 1u : 0u,
                                        options.useSymmetricProjectionFov ? 1u : 0u,
                                        options.useGameRenderPoseForProjection ? 1u : 0u,
                                        ForceMonoProjectionEyeName(options.forceMonoProjectionEye),
                                        options.forceMonoProjectionEye,
                                        ForceMonoProjectionEyeName(options.forceMonoProjectionView),
                                        options.forceMonoProjectionView,
                                        m_GameEyes[imageEye].uMin,
                                        m_GameEyes[imageEye].vMin,
                                        m_GameEyes[imageEye].uMax,
                                        m_GameEyes[imageEye].vMax,
                                        rect.offset.x,
                                        rect.offset.y,
                                        rect.extent.width,
                                        rect.extent.height,
                                        m_Eyes[imageEye].width,
                                        m_Eyes[imageEye].height);
                                }
                                {
                                    static uint32_t s_projectionViewSyntheticLogBudget = 48;
                                    static uint32_t s_projectionViewRealLogBudget = 96;
                                    static ULONGLONG s_lastProjectionViewPeriodicLogMs = 0;
                                    static uint32_t s_projectionViewPeriodicEyesLeft = 0;
                                    const bool realSharedFrame =
                                        sharedTextureFrameId != 0 &&
                                        !IsSyntheticOpenXrSharedFrame(sharedTextureFrameId);
                                    uint32_t& projectionViewLogBudget =
                                        realSharedFrame ? s_projectionViewRealLogBudget : s_projectionViewSyntheticLogBudget;
                                    if (realSharedFrame)
                                    {
                                        const ULONGLONG nowMs = GetTickCount64();
                                        if (eye == 0 &&
                                            (s_lastProjectionViewPeriodicLogMs == 0 ||
                                                nowMs - s_lastProjectionViewPeriodicLogMs >= 30000ull))
                                        {
                                            s_lastProjectionViewPeriodicLogMs = nowMs;
                                            s_projectionViewPeriodicEyesLeft = 2;
                                        }
                                    }
                                    const bool periodicProjectionViewLog =
                                        realSharedFrame && s_projectionViewPeriodicEyesLeft > 0;
                                    if (projectionViewLogBudget > 0 || periodicProjectionViewLog)
                                    {
                                        const bool budgetProjectionViewLog = projectionViewLogBudget > 0;
                                        if (budgetProjectionViewLog)
                                            --projectionViewLogBudget;
                                        if (periodicProjectionViewLog)
                                            --s_projectionViewPeriodicEyesLeft;
                                        const XrPosef& pose = projectionViews[eye].pose;
                                        const XrFovf& fov = projectionViews[eye].fov;
                                        const XrRect2Di& rect = projectionViews[eye].subImage.imageRect;
                                        const VulkanGameEyeTexture& source = m_GameEyes[imageEye];
                                        XrPosef gamePoseCandidate{
                                            XrQuaternionf{ 0.0f, 0.0f, 0.0f, 1.0f },
                                            XrVector3f{ 0.0f, 0.0f, 0.0f }
                                        };
                                        float gamePoseYaw = 0.0f;
                                        float gamePoseIpd = 0.0f;
                                        if (haveGameRenderPose)
                                        {
                                            gamePoseCandidate = BuildProjectionPoseFromGameRenderPose(
                                                projectionRenderPose,
                                                locatedViews,
                                                locatedCount,
                                                projectionViewEye,
                                                &gamePoseYaw,
                                                &gamePoseIpd);
                                        }
                                        const float gamePoseDeltaX = haveGameRenderPose ? (gamePoseCandidate.position.x - pose.position.x) : 0.0f;
                                        const float gamePoseDeltaY = haveGameRenderPose ? (gamePoseCandidate.position.y - pose.position.y) : 0.0f;
                                        const float gamePoseDeltaZ = haveGameRenderPose ? (gamePoseCandidate.position.z - pose.position.z) : 0.0f;
                                        m_Log.Print(
                                            "[OpenXR][ProjectionView] log=%s viewEye=%s(%u) projectionViewEye=%s(%u) imageEye=%s(%u) swapProjectionEyes=%u swapProjectionViewOrder=%u mirrorProjectionHorizontal=%u useSymmetricProjectionFov=%u useGameRenderPoseForProjection=%u forceMonoProjectionEye=%s(%d) forceMonoProjectionView=%s(%d) submittedFrames=%u sharedFrame=%u sharedFrameGen=%u sourceGen=%u sourceHandle=0x%llX sourceImage=0x%llX swapchain=0x%llX imageRect=(%d,%d %dx%d) swapchainSize=%ux%u fov=(L=%.4f R=%.4f U=%.4f D=%.4f) posePos=(%.4f %.4f %.4f) poseQuat=(%.4f %.4f %.4f %.4f) poseSource=%s renderPoseGen=%u gamePoseValid=%u gamePosePos=(%.4f %.4f %.4f) gamePoseDelta=(%.4f %.4f %.4f) gamePoseYawDeg=%.2f gamePoseIpd=%.4f",
                                            periodicProjectionViewLog && !budgetProjectionViewLog ? "periodic" : "budget",
                                            EyeName(eye),
                                            eye,
                                            EyeName(projectionViewEye),
                                            projectionViewEye,
                                            EyeName(imageEye),
                                            imageEye,
                                            options.swapProjectionEyes ? 1u : 0u,
                                            options.swapProjectionViewOrder ? 1u : 0u,
                                            options.mirrorProjectionHorizontal ? 1u : 0u,
                                            options.useSymmetricProjectionFov ? 1u : 0u,
                                            options.useGameRenderPoseForProjection ? 1u : 0u,
                                            ForceMonoProjectionEyeName(options.forceMonoProjectionEye),
                                            options.forceMonoProjectionEye,
                                            ForceMonoProjectionEyeName(options.forceMonoProjectionView),
                                            options.forceMonoProjectionView,
                                            submittedFrames,
                                            sharedTextureFrameId,
                                            sharedTextureFrameGeneration,
                                            source.generation,
                                            static_cast<unsigned long long>(source.kmtHandle),
                                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(source.image)),
                                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(projectionViews[eye].subImage.swapchain)),
                                            rect.offset.x,
                                            rect.offset.y,
                                            rect.extent.width,
                                            rect.extent.height,
                                            m_Eyes[imageEye].width,
                                            m_Eyes[imageEye].height,
                                            fov.angleLeft,
                                            fov.angleRight,
                                            fov.angleUp,
                                            fov.angleDown,
                                            pose.position.x,
                                            pose.position.y,
                                            pose.position.z,
                                            pose.orientation.x,
                                            pose.orientation.y,
                                            pose.orientation.z,
                                            pose.orientation.w,
                                            projectionPoseSource,
                                            projectionRenderPoseGeneration,
                                            haveGameRenderPose ? 1u : 0u,
                                            gamePoseCandidate.position.x,
                                            gamePoseCandidate.position.y,
                                            gamePoseCandidate.position.z,
                                            gamePoseDeltaX,
                                            gamePoseDeltaY,
                                            gamePoseDeltaZ,
                                            gamePoseYaw * (180.0f / 3.141592654f),
                                            gamePoseIpd);
                                    }
                                }
                            }
                            if (options.swapProjectionViewOrder)
                            {
                                std::swap(projectionViews[0], projectionViews[1]);
                                static bool s_loggedProjectionViewOrderSwap = false;
                                if (!s_loggedProjectionViewOrderSwap)
                                {
                                    s_loggedProjectionViewOrderSwap = true;
                                    m_Log.Print(
                                        "OpenXR projection view-order swap active: projectionViews[0] and projectionViews[1] are swapped immediately before xrEndFrame");
                                }
                            }
                            layerReady = true;
                        }

                        bool blittedAnyOverlayFrame = false;
                        if (options.disableQuadOverlays)
                        {
                            static ULONGLONG s_lastOverlayDisabledLogMs = 0;
                            const ULONGLONG nowMs = GetTickCount64();
                            if (s_lastOverlayDisabledLogMs == 0 || nowMs - s_lastOverlayDisabledLogMs >= 30000ull)
                            {
                                s_lastOverlayDisabledLogMs = nowMs;
                                m_Log.Print(
                                    "[OpenXR][Overlay] disableQuadOverlays=1 skipping quad overlays overlayFrameReady=%u overlayFrame=%u overlayFrameGen=%u lastSubmittedOverlayGen=%u",
                                    overlayFrameReady ? 1u : 0u,
                                    overlayFrameId,
                                    overlayFrameGeneration,
                                    lastSubmittedOverlayFrameGeneration);
                            }
                        }
                        else
                        {
                            static uint32_t s_overlayLayerLogBudget = 96;
                            static ULONGLONG s_lastOverlayLayerPeriodicLogMs = 0;
                            uint32_t overlayLayerPeriodicLogBudget = 0;
                            const ULONGLONG overlayLogNowMs = GetTickCount64();
                            if (s_lastOverlayLayerPeriodicLogMs == 0 || overlayLogNowMs - s_lastOverlayLayerPeriodicLogMs >= 30000ull)
                            {
                                s_lastOverlayLayerPeriodicLogMs = overlayLogNowMs;
                                overlayLayerPeriodicLogBudget = static_cast<uint32_t>(overlayLayers.size());
                            }

                            for (uint32_t overlayIndex = 0; overlayIndex < L4D2VR_OPENXR_OVERLAY_COUNT; ++overlayIndex)
                            {
                                const L4D2VROpenXrOverlayDesc overlay = m_Bridge.Overlay(overlayIndex);
                                if (!overlay.valid || !overlay.visible || !overlay.texture.valid)
                                    continue;

                                VulkanEyeSwapchain& overlaySwapchain = m_OverlaySwapchains[overlayIndex];
                                const uint32_t overlayGeneration = m_Bridge.OverlayGeneration();
                                const bool overlayNeedsBlit =
                                    overlayFrameReady &&
                                    (overlayFrameGeneration != lastSubmittedOverlayFrameGeneration ||
                                        m_OverlayTextures[overlayIndex].generation != overlayGeneration ||
                                        overlaySwapchain.handle == XR_NULL_HANDLE);

                                bool overlayReadyForSubmit = overlaySwapchain.handle != XR_NULL_HANDLE;
                                if (overlayNeedsBlit)
                                {
                                    overlayReadyForSubmit = RenderOverlaySwapchain(overlayIndex, overlay);
                                    if (overlayReadyForSubmit)
                                        blittedAnyOverlayFrame = true;
                                }

                                if (overlayReadyForSubmit && overlaySwapchain.handle != XR_NULL_HANDLE)
                                {
                                    const float widthMeters = (std::isfinite(overlay.widthMeters) && overlay.widthMeters > 0.05f)
                                        ? overlay.widthMeters
                                        : 1.5f;
                                    const float heightMeters = (std::isfinite(overlay.heightMeters) && overlay.heightMeters > 0.05f)
                                        ? overlay.heightMeters
                                        : widthMeters * (static_cast<float>((std::max)(1u, overlay.texture.height)) /
                                            static_cast<float>((std::max)(1u, overlay.texture.width)));

                                    const float curvature = std::clamp(overlay.curvature, 0.0f, 1.0f);
                                    const bool curvedHud =
                                        overlayIndex == L4D2VR_OPENXR_OVERLAY_HUD &&
                                        curvature > 0.001f &&
                                        overlaySwapchain.width >= kMaxOpenXrHudCurveSegments;
                                    const uint32_t segmentCount = curvedHud
                                        ? std::clamp(static_cast<uint32_t>(std::ceil(4.0f + curvature * 8.0f)), 4u, kMaxOpenXrHudCurveSegments)
                                        : 1u;
                                    const float totalArc = curvedHud
                                        ? std::clamp(curvature * (0.75f * kPi), 0.01f, 0.85f * kPi)
                                        : 0.0f;
                                    const float radiusMeters = curvedHud ? (widthMeters / totalArc) : 0.0f;
                                    XrPosef hudGameRenderCenterPose{};
                                    const XrPosef* overlayGameRenderCenterPose = nullptr;
                                    constexpr bool kUseGameRenderPoseForHudOverlay = false;
                                    if (overlayIndex == L4D2VR_OPENXR_OVERLAY_HUD &&
                                        kUseGameRenderPoseForHudOverlay &&
                                        haveGameRenderPose)
                                    {
                                        hudGameRenderCenterPose.orientation = NormalizeOpenXrQuaternion(projectionRenderPose.orientation);
                                        hudGameRenderCenterPose.position = XrVector3f{
                                            projectionRenderPose.position[0],
                                            projectionRenderPose.position[1],
                                            projectionRenderPose.position[2]
                                        };
                                        overlayGameRenderCenterPose = &hudGameRenderCenterPose;
                                    }

                                    const auto appendOverlayLayer = [&](uint32_t segmentIndex)
                                    {
                                        if (overlayLayerCount >= overlayLayers.size())
                                            return false;

                                        const float segmentU0 = static_cast<float>(segmentIndex) / static_cast<float>(segmentCount);
                                        const float segmentU1 = static_cast<float>(segmentIndex + 1u) / static_cast<float>(segmentCount);
                                        const uint32_t srcX0 = curvedHud
                                            ? static_cast<uint32_t>(std::floor(segmentU0 * static_cast<float>(overlaySwapchain.width)))
                                            : 0u;
                                        const uint32_t srcX1 = curvedHud
                                            ? static_cast<uint32_t>(std::floor(segmentU1 * static_cast<float>(overlaySwapchain.width)))
                                            : overlaySwapchain.width;
                                        const uint32_t clampedSrcX0 = std::min(srcX0, overlaySwapchain.width - 1u);
                                        const uint32_t clampedSrcX1 = std::clamp(srcX1, clampedSrcX0 + 1u, overlaySwapchain.width);

                                        const uint32_t composedOverlayLayerIndex = overlayLayerCount;
                                        XrCompositionLayerQuad& overlayLayer = overlayLayers[overlayLayerCount++];
                                        overlayLayer.layerFlags =
                                            (overlayIndex == L4D2VR_OPENXR_OVERLAY_HUD)
                                                ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                                                : 0;
                                        overlayLayer.space = m_AppSpace;
                                        overlayLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                                        overlayLayer.subImage.swapchain = overlaySwapchain.handle;
                                        overlayLayer.subImage.imageRect.offset = {
                                            static_cast<int32_t>(clampedSrcX0),
                                            0
                                        };
                                        overlayLayer.subImage.imageRect.extent = {
                                            static_cast<int32_t>(clampedSrcX1 - clampedSrcX0),
                                            static_cast<int32_t>(overlaySwapchain.height)
                                        };

                                        if (curvedHud)
                                        {
                                            const float theta0 = -totalArc * 0.5f + totalArc * segmentU0;
                                            const float theta1 = -totalArc * 0.5f + totalArc * segmentU1;
                                            const float thetaCenter = (theta0 + theta1) * 0.5f;
                                            overlayLayer.pose = BuildCurvedOverlaySlicePose(
                                                overlay,
                                                locatedViews,
                                                locatedCount,
                                                thetaCenter,
                                                radiusMeters,
                                                overlayGameRenderCenterPose);
                                            overlayLayer.size = XrExtent2Df{
                                                2.0f * radiusMeters * std::sin((theta1 - theta0) * 0.5f),
                                                heightMeters
                                            };
                                        }
                                        else
                                        {
                                            overlayLayer.pose = BuildOverlayPose(overlay, locatedViews, locatedCount, overlayGameRenderCenterPose);
                                            overlayLayer.size = XrExtent2Df{ widthMeters, heightMeters };
                                        }

                                        const bool budgetOverlayLog = s_overlayLayerLogBudget > 0;
                                        const bool periodicOverlayLog = overlayLayerPeriodicLogBudget > 0;
                                        if (budgetOverlayLog || periodicOverlayLog)
                                        {
                                            if (budgetOverlayLog)
                                                --s_overlayLayerLogBudget;
                                            if (periodicOverlayLog)
                                                --overlayLayerPeriodicLogBudget;
                                            const XrRect2Di& rect = overlayLayer.subImage.imageRect;
                                            const XrPosef& pose = overlayLayer.pose;
                                            m_Log.Print(
                                                "[OpenXR][OverlayLayer] log=%s submittedFrames=%u overlayFrame=%u overlayFrameGen=%u overlayGen=%u layerIndex=%u overlayIndex=%u overlay=%s segment=%u/%u curved=%u needsBlit=%u blittedThisFrame=%u swapchain=0x%llX imageRect=(%d,%d %dx%d) swapchainSize=%ux%u textureHandle=0x%llX textureImage=0x%llX textureSize=%ux%u textureFmt=%u layerFlags=0x%llX eyeVisibility=%u size=(%.4f %.4f) sourceMeters=(%.4f %.4f dist=%.4f curvature=%.4f offset=%.4f,%.4f,%.4f) posePos=(%.4f %.4f %.4f) poseQuat=(%.4f %.4f %.4f %.4f)",
                                                periodicOverlayLog && !budgetOverlayLog ? "periodic" : "budget",
                                                submittedFrames,
                                                overlayFrameId,
                                                overlayFrameGeneration,
                                                overlayGeneration,
                                                composedOverlayLayerIndex,
                                                overlayIndex,
                                                OverlayName(overlayIndex),
                                                segmentIndex,
                                                segmentCount,
                                                curvedHud ? 1u : 0u,
                                                overlayNeedsBlit ? 1u : 0u,
                                                blittedAnyOverlayFrame ? 1u : 0u,
                                                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(overlayLayer.subImage.swapchain)),
                                                rect.offset.x,
                                                rect.offset.y,
                                                rect.extent.width,
                                                rect.extent.height,
                                                overlaySwapchain.width,
                                                overlaySwapchain.height,
                                                static_cast<unsigned long long>(overlay.texture.kmtHandle),
                                                static_cast<unsigned long long>(overlay.texture.image),
                                                overlay.texture.width,
                                                overlay.texture.height,
                                                overlay.texture.format,
                                                static_cast<unsigned long long>(overlayLayer.layerFlags),
                                                static_cast<unsigned int>(overlayLayer.eyeVisibility),
                                                overlayLayer.size.width,
                                                overlayLayer.size.height,
                                                overlay.widthMeters,
                                                overlay.heightMeters,
                                                overlay.distanceMeters,
                                                overlay.curvature,
                                                overlay.offsetMeters[0],
                                                overlay.offsetMeters[1],
                                                overlay.offsetMeters[2],
                                                pose.position.x,
                                                pose.position.y,
                                                pose.position.z,
                                                pose.orientation.x,
                                                pose.orientation.y,
                                                pose.orientation.z,
                                                pose.orientation.w);
                                        }
                                        return true;
                                    };

                                    for (uint32_t segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex)
                                        appendOverlayLayer(segmentIndex);

                                }
                            }
                        }
                        if (blittedAnyOverlayFrame)
                            lastSubmittedOverlayFrameGeneration = overlayFrameGeneration;
                    }
                }

                XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
                layer.space = m_AppSpace;
                layer.viewCount = 2;
                layer.views = projectionViews.data();
                std::array<const XrCompositionLayerBaseHeader*, 1 + kMaxOpenXrOverlayLayers> layers{};
                uint32_t layerCount = 0;
                const bool submitProjectionLayer = layerReady && !options.disableProjectionLayer;
                if (layerReady && options.disableProjectionLayer)
                {
                    static ULONGLONG s_lastProjectionLayerDisabledLogMs = 0;
                    const ULONGLONG nowMs = GetTickCount64();
                    if (s_lastProjectionLayerDisabledLogMs == 0 || nowMs - s_lastProjectionLayerDisabledLogMs >= 30000ull)
                    {
                        s_lastProjectionLayerDisabledLogMs = nowMs;
                        m_Log.Print("[OpenXR][ProjectionLayer] disabled by option; skipping projection layer, overlays=%u", overlayLayerCount);
                    }
                }
                if (submitProjectionLayer)
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                for (uint32_t overlayLayerIndex = 0; overlayLayerIndex < overlayLayerCount; ++overlayLayerIndex)
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlayLayers[overlayLayerIndex]);
                XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = m_BlendMode;
                endInfo.layerCount = layerCount;
                endInfo.layers = layerCount ? layers.data() : nullptr;
                if (layerReady)
                {
                    static ULONGLONG s_lastEndFrameSubmitLogMs = 0;
                    const ULONGLONG nowMs = GetTickCount64();
                    if (s_lastEndFrameSubmitLogMs == 0 || nowMs - s_lastEndFrameSubmitLogMs >= 30000ull)
                    {
                        s_lastEndFrameSubmitLogMs = nowMs;
                        const XrRect2Di& leftRect = projectionViews[0].subImage.imageRect;
                        const XrRect2Di& rightRect = projectionViews[1].subImage.imageRect;
                        m_Log.Print(
                            "[OpenXR][EndFrameSubmit] layerCount=%u projection=%u projectionReady=%u overlays=%u displayTime=%lld envBlend=%u disableProjectionLayer=%u swapProjectionEyes=%u swapProjectionViewOrder=%u mirrorProjectionHorizontal=%u useSymmetricProjectionFov=%u useGameRenderPoseForProjection=%u forceMonoProjectionEye=%s(%d) forceMonoProjectionView=%s(%d) slot0Swapchain=0x%llX slot0Rect=(%d,%d %dx%d) slot1Swapchain=0x%llX slot1Rect=(%d,%d %dx%d) note=after_this_xrEndFrame_hands_layers_to_runtime_compositor_no_app_readback_image_available",
                            layerCount,
                            submitProjectionLayer ? 1u : 0u,
                            layerReady ? 1u : 0u,
                            overlayLayerCount,
                            static_cast<long long>(endInfo.displayTime),
                            static_cast<unsigned int>(endInfo.environmentBlendMode),
                            options.disableProjectionLayer ? 1u : 0u,
                            options.swapProjectionEyes ? 1u : 0u,
                            options.swapProjectionViewOrder ? 1u : 0u,
                            options.mirrorProjectionHorizontal ? 1u : 0u,
                            options.useSymmetricProjectionFov ? 1u : 0u,
                            options.useGameRenderPoseForProjection ? 1u : 0u,
                            ForceMonoProjectionEyeName(options.forceMonoProjectionEye),
                            options.forceMonoProjectionEye,
                            ForceMonoProjectionEyeName(options.forceMonoProjectionView),
                            options.forceMonoProjectionView,
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(projectionViews[0].subImage.swapchain)),
                            leftRect.offset.x,
                            leftRect.offset.y,
                            leftRect.extent.width,
                            leftRect.extent.height,
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(projectionViews[1].subImage.swapchain)),
                            rightRect.offset.x,
                            rightRect.offset.y,
                            rightRect.extent.width,
                            rightRect.extent.height);
                    }
                }
                result = m_Xr.xrEndFrame(m_Session, &endInfo);
                if (!Succeeded(m_Log, "xrEndFrame", result))
                    return 26;
                if (layerCount != 0)
                {
                    ++submittedFrames;
                    m_Bridge.Update(L4D2VROpenXrBridgeStatus::SubmittedFrame, 0, submittedFrames,
                        submitProjectionLayer
                        ? (overlayLayerCount ? "OpenXR Vulkan projection/quad frame submitted" : "OpenXR Vulkan projection frame submitted")
                        : "OpenXR Vulkan frame submitted without projection layer");
                    if (submittedFrames == 1 || (submittedFrames % 60) == 0)
                        m_Log.Print("Submitted OpenXR Vulkan frame %u layers=%u projection=%u overlays=%u",
                            submittedFrames, layerCount, submitProjectionLayer ? 1u : 0u, overlayLayerCount);
                }
                if (options.targetFrames > 0 && submittedFrames >= options.targetFrames)
                    break;
            }

            if (submittedFrames > 0)
            {
                m_Bridge.Update(L4D2VROpenXrBridgeStatus::Completed, 0, submittedFrames, "OpenXR Vulkan submit loop completed");
                return 0;
            }
            m_Bridge.Update(L4D2VROpenXrBridgeStatus::Failed, 27, submittedFrames, "no OpenXR Vulkan frames submitted");
            return 27;
        }

        static HANDLE OpenParentProcess(DWORD parentPid)
        {
            if (parentPid == 0)
                return nullptr;
            return OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        }

        void Shutdown()
        {
            if (m_VkDevice != VK_NULL_HANDLE && m_Vk.vkDeviceWaitIdle)
                m_Vk.vkDeviceWaitIdle(m_VkDevice);
            m_InputBridge.Shutdown();
            for (VulkanGameEyeTexture& eye : m_GameEyes)
                DestroyImportedGameEye(eye);
            for (VulkanGameEyeTexture& overlay : m_OverlayTextures)
                DestroyImportedGameEye(overlay);
            for (VulkanEyeSwapchain& overlaySwapchain : m_OverlaySwapchains)
                DestroyOverlaySwapchain(overlaySwapchain);
            for (VulkanEyeSwapchain& eye : m_Eyes)
            {
                DestroySwapchainRenderTargets(eye);
                eye.images.clear();
                eye.layouts.clear();
                if (eye.handle != XR_NULL_HANDLE && m_Xr.xrDestroySwapchain)
                    m_Xr.xrDestroySwapchain(eye.handle);
            }
            if (m_BlitPipeline != VK_NULL_HANDLE && m_Vk.vkDestroyPipeline)
                m_Vk.vkDestroyPipeline(m_VkDevice, m_BlitPipeline, nullptr);
            if (m_BlitPipelineLayout != VK_NULL_HANDLE && m_Vk.vkDestroyPipelineLayout)
                m_Vk.vkDestroyPipelineLayout(m_VkDevice, m_BlitPipelineLayout, nullptr);
            if (m_BlitRenderPass != VK_NULL_HANDLE && m_Vk.vkDestroyRenderPass)
                m_Vk.vkDestroyRenderPass(m_VkDevice, m_BlitRenderPass, nullptr);
            if (m_BlitDescriptorPool != VK_NULL_HANDLE && m_Vk.vkDestroyDescriptorPool)
                m_Vk.vkDestroyDescriptorPool(m_VkDevice, m_BlitDescriptorPool, nullptr);
            if (m_BlitSampler != VK_NULL_HANDLE && m_Vk.vkDestroySampler)
                m_Vk.vkDestroySampler(m_VkDevice, m_BlitSampler, nullptr);
            if (m_BlitDescriptorSetLayout != VK_NULL_HANDLE && m_Vk.vkDestroyDescriptorSetLayout)
                m_Vk.vkDestroyDescriptorSetLayout(m_VkDevice, m_BlitDescriptorSetLayout, nullptr);
            if (m_CommandPool != VK_NULL_HANDLE && m_Vk.vkDestroyCommandPool)
                m_Vk.vkDestroyCommandPool(m_VkDevice, m_CommandPool, nullptr);
            if (m_AppSpace != XR_NULL_HANDLE && m_Xr.xrDestroySpace)
                m_Xr.xrDestroySpace(m_AppSpace);
            if (m_Session != XR_NULL_HANDLE && m_Xr.xrDestroySession)
                m_Xr.xrDestroySession(m_Session);
            if (m_VkDevice != VK_NULL_HANDLE && m_Vk.vkDestroyDevice)
                m_Vk.vkDestroyDevice(m_VkDevice, nullptr);
            if (m_VkInstance != VK_NULL_HANDLE && m_Vk.vkDestroyInstance)
                m_Vk.vkDestroyInstance(m_VkInstance, nullptr);
            if (m_Instance != XR_NULL_HANDLE && m_Xr.xrDestroyInstance)
                m_Xr.xrDestroyInstance(m_Instance);
            if (m_VulkanLoader)
                FreeLibrary(m_VulkanLoader);
            if (m_OpenXrLoader)
                FreeLibrary(m_OpenXrLoader);
            if (m_ParentProcess)
                CloseHandle(m_ParentProcess);
        }

        Logger& m_Log;
        BridgeWriter m_Bridge;
        HMODULE m_OpenXrLoader = nullptr;
        HMODULE m_VulkanLoader = nullptr;
        XrDispatch m_Xr{};
        VulkanDispatch m_Vk{};
        XrInstance m_Instance = XR_NULL_HANDLE;
        XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
        XrSession m_Session = XR_NULL_HANDLE;
        XrSpace m_AppSpace = XR_NULL_HANDLE;
        XrSessionState m_SessionState = XR_SESSION_STATE_UNKNOWN;
        XrEnvironmentBlendMode m_BlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        OpenXrInputBridge m_InputBridge;
        bool m_SessionRunning = false;
        bool m_HandTrackingExtensionEnabled = false;
        VkFormat m_SelectedSwapchainFormat = VK_FORMAT_UNDEFINED;
        VkInstance m_VkInstance = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_VkDevice = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        uint32_t m_GraphicsQueueFamily = UINT32_MAX;
        VkPhysicalDeviceMemoryProperties m_MemoryProperties{};
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
        bool m_UseShaderBlit = false;
        VkSampler m_BlitSampler = VK_NULL_HANDLE;
        VkRenderPass m_BlitRenderPass = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_BlitDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_BlitDescriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout m_BlitPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_BlitPipeline = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kBlitDescriptorSetCount> m_BlitDescriptorSets{};
        HANDLE m_ParentProcess = nullptr;
        std::vector<XrViewConfigurationView> m_ViewConfigs;
        std::vector<VulkanEyeSwapchain> m_Eyes;
        std::array<VulkanGameEyeTexture, L4D2VR_OPENXR_EYE_COUNT> m_GameEyes;
        std::array<VulkanGameEyeTexture, L4D2VR_OPENXR_OVERLAY_COUNT> m_OverlayTextures;
        std::array<VulkanEyeSwapchain, L4D2VR_OPENXR_OVERLAY_COUNT> m_OverlaySwapchains;
        ULONGLONG m_DebugImageDumpStartMs = 0;
        ULONGLONG m_DebugImageDumpWaitingGameFrameLogMs = 0;
        uint32_t m_DebugImageDumpStartSharedFrameId = 0;
        uint32_t m_DebugImageDumpEyeMask = 0;
        bool m_DebugImageDumpCompleted = false;
    };
}

int wmain(int argc, wchar_t** argv)
{
    const Options options = ParseOptions(argc, argv);

    Logger log;
    if (!log.Open(options.logPath))
        return 2;

    log.Print("L4D2VR OpenXR Helper64 starting");
    log.Print("Log path: %s", Narrow(options.logPath).c_str());
    log.Print("OpenXR projection eye swap: %s", options.swapProjectionEyes ? "enabled" : "disabled");
    log.Print("OpenXR projection view-order swap: %s", options.swapProjectionViewOrder ? "enabled" : "disabled");
    log.Print("OpenXR projection horizontal mirror: %s", options.mirrorProjectionHorizontal ? "enabled" : "disabled");
    log.Print("OpenXR projection layer: %s", options.disableProjectionLayer ? "disabled" : "enabled");
    log.Print("OpenXR symmetric projection FOV submit: %s", options.useSymmetricProjectionFov ? "enabled" : "disabled");
    log.Print("OpenXR game render pose projection submit: %s", options.useGameRenderPoseForProjection ? "enabled" : "disabled");
    log.Print(
        "OpenXR force mono projection eye: %s(%d)",
        ForceMonoProjectionEyeName(options.forceMonoProjectionEye),
        options.forceMonoProjectionEye);
    log.Print(
        "OpenXR force mono projection view: %s(%d)",
        ForceMonoProjectionEyeName(options.forceMonoProjectionView),
        options.forceMonoProjectionView);
    log.Print("OpenXR quad overlays: %s", options.disableQuadOverlays ? "disabled" : "enabled");

    OpenXrVulkanSubmitProbe probe(log);
    const int exitCode = probe.Run(options);

    log.Print("L4D2VR OpenXR Helper64 exiting code=%d", exitCode);
    return exitCode;
}
