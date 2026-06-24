#ifndef NOMINMAX
#define NOMINMAX
#endif
#define XR_USE_GRAPHICS_API_VULKAN 1

#include "openxr_backend.h"

#include <Windows.h>
#include <d3d9_vr.h>

#include "../thirdparty/openxr/include/openxr/openxr_platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
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
        char buffer[160] = {};
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

    bool HasInstanceExtension(
        PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions,
        const char* extensionName,
        std::string& detail)
    {
        uint32_t extensionCount = 0;
        XrResult result = enumerateExtensions(nullptr, 0, &extensionCount, nullptr);
        if (XR_FAILED(result))
        {
            detail = FormatXrResult("xrEnumerateInstanceExtensionProperties(count)", result);
            return false;
        }

        std::vector<XrExtensionProperties> extensions(extensionCount);
        for (auto& extension : extensions)
            extension.type = XR_TYPE_EXTENSION_PROPERTIES;

        result = enumerateExtensions(nullptr, extensionCount, &extensionCount, extensions.data());
        if (XR_FAILED(result))
        {
            detail = FormatXrResult("xrEnumerateInstanceExtensionProperties(data)", result);
            return false;
        }

        const auto it = std::find_if(extensions.begin(), extensions.end(),
            [extensionName](const XrExtensionProperties& extension)
            {
                return std::strcmp(extension.extensionName, extensionName) == 0;
            });
        if (it == extensions.end())
        {
            detail = std::string(extensionName) + " is not supported by the active OpenXR runtime";
            return false;
        }

        return true;
    }

    bool IsValidGraphicsBinding(const D3D9_OPENXR_GRAPHICS_BINDING_DESC& graphics)
    {
        return graphics.Instance != VK_NULL_HANDLE &&
            graphics.PhysicalDevice != VK_NULL_HANDLE &&
            graphics.Device != VK_NULL_HANDLE &&
            graphics.Queue != VK_NULL_HANDLE;
    }
}

OpenXrBackend::~OpenXrBackend()
{
    Shutdown();
}

bool OpenXrBackend::InitializeSession(
    const D3D9_OPENXR_GRAPHICS_BINDING_DESC& graphics,
    std::string* detail)
{
    Shutdown();

    auto fail = [&](const std::string& message)
        {
            if (detail)
                *detail = message;
            Shutdown();
            return false;
        };

    if (!IsValidGraphicsBinding(graphics))
        return fail("DXVK Vulkan graphics binding is incomplete");

    m_Loader = LoadLibraryA("openxr_loader.dll");
    if (!m_Loader)
        return fail("openxr_loader.dll was not found");

    m_xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(reinterpret_cast<HMODULE>(m_Loader), "xrGetInstanceProcAddr"));
    if (!m_xrGetInstanceProcAddr)
        return fail("openxr_loader.dll does not export xrGetInstanceProcAddr");

    std::string loadDetail;
    PFN_xrCreateInstance xrCreateInstanceFn = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionPropertiesFn = nullptr;
    if (!LoadXrFunction(m_xrGetInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstanceFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", xrEnumerateInstanceExtensionPropertiesFn, loadDetail))
    {
        return fail(loadDetail);
    }

    if (!HasInstanceExtension(xrEnumerateInstanceExtensionPropertiesFn, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, loadDetail))
        return fail(loadDetail);

    const char* extensions[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "L4D2VR");
    createInfo.applicationInfo.applicationVersion = 1;
    std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Source/DXVK");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
    createInfo.enabledExtensionNames = extensions;

    XrResult result = xrCreateInstanceFn(&createInfo, &m_Instance);
    if (XR_FAILED(result) || m_Instance == XR_NULL_HANDLE)
        return fail(FormatXrResult("xrCreateInstance", result));

    PFN_xrGetInstanceProperties xrGetInstancePropertiesFn = nullptr;
    PFN_xrGetSystem xrGetSystemFn = nullptr;
    PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViewsFn = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHRFn = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHRFn = nullptr;
    PFN_xrCreateSession xrCreateSessionFn = nullptr;
    PFN_xrCreateReferenceSpace xrCreateReferenceSpaceFn = nullptr;
    PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormatsFn = nullptr;

    if (!LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrDestroyInstance", m_xrDestroyInstance, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrDestroySession", m_xrDestroySession, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrDestroySpace", m_xrDestroySpace, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrGetInstanceProperties", xrGetInstancePropertiesFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrGetSystem", xrGetSystemFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViewsFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrGetVulkanGraphicsDeviceKHR", xrGetVulkanGraphicsDeviceKHRFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrGetVulkanGraphicsRequirementsKHR", xrGetVulkanGraphicsRequirementsKHRFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrCreateSession", xrCreateSessionFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrCreateReferenceSpace", xrCreateReferenceSpaceFn, loadDetail) ||
        !LoadXrFunction(m_xrGetInstanceProcAddr, m_Instance, "xrEnumerateSwapchainFormats", xrEnumerateSwapchainFormatsFn, loadDetail))
    {
        return fail(loadDetail);
    }

    XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
    result = xrGetInstancePropertiesFn(m_Instance, &instanceProperties);
    if (XR_SUCCEEDED(result))
    {
        std::snprintf(m_RuntimeName, sizeof(m_RuntimeName), "%s", instanceProperties.runtimeName);
        m_RuntimeVersionMajor = XR_VERSION_MAJOR(instanceProperties.runtimeVersion);
        m_RuntimeVersionMinor = XR_VERSION_MINOR(instanceProperties.runtimeVersion);
        m_RuntimeVersionPatch = XR_VERSION_PATCH(instanceProperties.runtimeVersion);
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystemFn(m_Instance, &systemInfo, &m_SystemId);
    if (XR_FAILED(result) || m_SystemId == XR_NULL_SYSTEM_ID)
        return fail(FormatXrResult("xrGetSystem(HMD)", result));

    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViewsFn(
        m_Instance,
        m_SystemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &viewCount,
        nullptr);
    if (XR_FAILED(result) || viewCount == 0)
    {
        return fail(XR_FAILED(result)
            ? FormatXrResult("xrEnumerateViewConfigurationViews(count)", result)
            : "xrEnumerateViewConfigurationViews returned zero primary stereo views");
    }

    std::vector<XrViewConfigurationView> views(viewCount);
    for (auto& view : views)
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

    result = xrEnumerateViewConfigurationViewsFn(
        m_Instance,
        m_SystemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount,
        &viewCount,
        views.data());
    if (XR_FAILED(result) || viewCount == 0)
    {
        return fail(XR_FAILED(result)
            ? FormatXrResult("xrEnumerateViewConfigurationViews(data)", result)
            : "xrEnumerateViewConfigurationViews returned no view data");
    }

    m_RecommendedEyeWidth = views[0].recommendedImageRectWidth;
    m_RecommendedEyeHeight = views[0].recommendedImageRectHeight;

    XrGraphicsRequirementsVulkanKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    result = xrGetVulkanGraphicsRequirementsKHRFn(m_Instance, m_SystemId, &graphicsRequirements);
    if (XR_FAILED(result))
        return fail(FormatXrResult("xrGetVulkanGraphicsRequirementsKHR", result));

    VkPhysicalDevice runtimePhysicalDevice = VK_NULL_HANDLE;
    result = xrGetVulkanGraphicsDeviceKHRFn(m_Instance, m_SystemId, graphics.Instance, &runtimePhysicalDevice);
    if (XR_FAILED(result) || runtimePhysicalDevice == VK_NULL_HANDLE)
        return fail(FormatXrResult("xrGetVulkanGraphicsDeviceKHR", result));

    if (runtimePhysicalDevice != graphics.PhysicalDevice)
    {
        char message[192] = {};
        std::snprintf(message, sizeof(message),
            "OpenXR runtime selected VkPhysicalDevice=%p but DXVK uses %p",
            reinterpret_cast<void*>(runtimePhysicalDevice),
            reinterpret_cast<void*>(graphics.PhysicalDevice));
        return fail(message);
    }

    XrGraphicsBindingVulkanKHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    graphicsBinding.instance = graphics.Instance;
    graphicsBinding.physicalDevice = graphics.PhysicalDevice;
    graphicsBinding.device = graphics.Device;
    graphicsBinding.queueFamilyIndex = graphics.QueueFamilyIndex;
    graphicsBinding.queueIndex = graphics.QueueIndex;

    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = m_SystemId;

    result = xrCreateSessionFn(m_Instance, &sessionInfo, &m_Session);
    if (XR_FAILED(result) || m_Session == XR_NULL_HANDLE)
        return fail(FormatXrResult("xrCreateSession(Vulkan)", result));

    uint32_t swapchainFormatCount = 0;
    result = xrEnumerateSwapchainFormatsFn(m_Session, 0, &swapchainFormatCount, nullptr);
    if (XR_FAILED(result))
        return fail(FormatXrResult("xrEnumerateSwapchainFormats(count)", result));

    m_SwapchainFormats.clear();
    if (swapchainFormatCount > 0)
    {
        m_SwapchainFormats.resize(swapchainFormatCount);
        result = xrEnumerateSwapchainFormatsFn(
            m_Session,
            swapchainFormatCount,
            &swapchainFormatCount,
            m_SwapchainFormats.data());
        if (XR_FAILED(result))
            return fail(FormatXrResult("xrEnumerateSwapchainFormats(data)", result));
        m_SwapchainFormats.resize(swapchainFormatCount);
    }

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

    const XrResult stageSpaceResult = xrCreateReferenceSpaceFn(m_Session, &spaceInfo, &m_AppSpace);
    const char* spaceName = "stage";
    if (XR_FAILED(stageSpaceResult) || m_AppSpace == XR_NULL_HANDLE)
    {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = xrCreateReferenceSpaceFn(m_Session, &spaceInfo, &m_AppSpace);
        spaceName = "local";
        if (XR_FAILED(result) || m_AppSpace == XR_NULL_HANDLE)
            return fail(FormatXrResult("xrCreateReferenceSpace(local)", result));
    }

    m_SessionReady = true;

    if (detail)
    {
        char message[320] = {};
        std::snprintf(message, sizeof(message),
            "runtime=%s version=%u.%u.%u recommendedEye=%ux%u views=%u swapchainFormats=%u space=%s queueFamily=%u queueIndex=%u",
            m_RuntimeName[0] ? m_RuntimeName : "unknown",
            m_RuntimeVersionMajor,
            m_RuntimeVersionMinor,
            m_RuntimeVersionPatch,
            m_RecommendedEyeWidth,
            m_RecommendedEyeHeight,
            viewCount,
            static_cast<unsigned int>(m_SwapchainFormats.size()),
            spaceName,
            graphics.QueueFamilyIndex,
            graphics.QueueIndex);
        *detail = message;
    }

    return true;
}

void OpenXrBackend::Shutdown()
{
    if (m_xrDestroySpace && m_AppSpace != XR_NULL_HANDLE)
        m_xrDestroySpace(m_AppSpace);
    m_AppSpace = XR_NULL_HANDLE;

    if (m_xrDestroySession && m_Session != XR_NULL_HANDLE)
        m_xrDestroySession(m_Session);
    m_Session = XR_NULL_HANDLE;

    if (m_xrDestroyInstance && m_Instance != XR_NULL_HANDLE)
        m_xrDestroyInstance(m_Instance);
    m_Instance = XR_NULL_HANDLE;
    m_SystemId = XR_NULL_SYSTEM_ID;

    if (m_Loader)
        FreeLibrary(reinterpret_cast<HMODULE>(m_Loader));
    m_Loader = nullptr;

    m_xrGetInstanceProcAddr = nullptr;
    m_xrDestroyInstance = nullptr;
    m_xrDestroySession = nullptr;
    m_xrDestroySpace = nullptr;
    m_SessionReady = false;

    m_RuntimeName[0] = '\0';
    m_RuntimeVersionMajor = 0;
    m_RuntimeVersionMinor = 0;
    m_RuntimeVersionPatch = 0;
    m_RecommendedEyeWidth = 0;
    m_RecommendedEyeHeight = 0;
    m_SwapchainFormats.clear();
}

bool OpenXrBackend::SupportsSwapchainFormat(int64_t format) const
{
    return std::find(m_SwapchainFormats.begin(), m_SwapchainFormats.end(), format) != m_SwapchainFormats.end();
}
