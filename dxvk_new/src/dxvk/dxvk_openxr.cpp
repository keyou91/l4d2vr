#define XR_USE_GRAPHICS_API_VULKAN 1

#include "dxvk_instance.h"
#include "dxvk_openxr.h"

#include "L4D2VR/vr_runtime_backend.h"
#include "../../../thirdparty/openxr/include/openxr/openxr_platform.h"

#include <cstdio>
#include <cstring>

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

using PFN___wineopenxr_GetVulkanInstanceExtensions = int (WINAPI *)(uint32_t, uint32_t *, char *);
using PFN___wineopenxr_GetVulkanDeviceExtensions = int (WINAPI *)(uint32_t, uint32_t *, char *);

namespace {

  std::string formatXrResult(const char* action, XrResult result) {
    return std::string(action ? action : "OpenXR call") + " failed: " + std::to_string(static_cast<int>(result));
  }

  template <typename T>
  bool loadNativeXrFunction(
          PFN_xrGetInstanceProcAddr getInstanceProcAddr,
          XrInstance                instance,
    const char*                     name,
          T&                        out,
          std::string&              detail) {
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = getInstanceProcAddr(instance, name, &function);
    if (XR_FAILED(result) || !function) {
      detail = formatXrResult(name, result);
      return false;
    }

    out = reinterpret_cast<T>(function);
    return true;
  }

  bool nativeOpenXrHasInstanceExtension(
          PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions,
    const char*                                       extensionName,
          std::string&                                detail) {
    uint32_t extensionCount = 0;
    XrResult result = enumerateExtensions(nullptr, 0, &extensionCount, nullptr);
    if (XR_FAILED(result)) {
      detail = formatXrResult("xrEnumerateInstanceExtensionProperties(count)", result);
      return false;
    }

    std::vector<XrExtensionProperties> extensions(extensionCount);
    for (auto& extension : extensions)
      extension.type = XR_TYPE_EXTENSION_PROPERTIES;

    result = enumerateExtensions(nullptr, extensionCount, &extensionCount, extensions.data());
    if (XR_FAILED(result)) {
      detail = formatXrResult("xrEnumerateInstanceExtensionProperties(data)", result);
      return false;
    }

    for (const XrExtensionProperties& extension : extensions) {
      if (std::strcmp(extension.extensionName, extensionName) == 0)
        return true;
    }

    detail = std::string(extensionName) + " is not supported by the active OpenXR runtime";
    return false;
  }

  template <typename T>
  bool queryNativeOpenXrVulkanExtensionString(
          T                 query,
          XrInstance        instance,
          XrSystemId        systemId,
    const char*             label,
          std::string&      out,
          std::string&      detail) {
    uint32_t length = 0;
    XrResult result = query(instance, systemId, 0, &length, nullptr);
    if (XR_FAILED(result)) {
      detail = formatXrResult(label, result);
      return false;
    }

    if (length == 0) {
      out.clear();
      return true;
    }

    std::vector<char> buffer(length);
    result = query(instance, systemId, length, &length, buffer.data());
    if (XR_FAILED(result)) {
      detail = formatXrResult(label, result);
      return false;
    }

    out.assign(buffer.data(), buffer.data() + length);
    const size_t zero = out.find('\0');
    if (zero != std::string::npos)
      out.resize(zero);
    return true;
  }
}

namespace dxvk {
  
  struct WineXrFunctions {
    PFN___wineopenxr_GetVulkanInstanceExtensions __wineopenxr_GetVulkanInstanceExtensions = nullptr;
    PFN___wineopenxr_GetVulkanDeviceExtensions __wineopenxr_GetVulkanDeviceExtensions = nullptr;
  };
  
  WineXrFunctions g_winexrFunctions;
  DxvkXrProvider DxvkXrProvider::s_instance;

  DxvkXrProvider:: DxvkXrProvider() { }

  DxvkXrProvider::~DxvkXrProvider() { }


  std::string_view DxvkXrProvider::getName() {
    return "OpenXR";
  }
  
  
  DxvkNameSet DxvkXrProvider::getInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return m_insExtensions;
  }


  DxvkNameSet DxvkXrProvider::getDeviceExtensions(uint32_t adapterId) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return m_devExtensions;
  }


  void DxvkXrProvider::initInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_initializedInsExt)
      return;

    if (!this->shouldUseConfiguredOpenXrRuntime())
      return;

    DxvkNameSet nativeInstanceExtensions;
    DxvkNameSet nativeDeviceExtensions;
    if (this->queryNativeOpenXrExtensions(nativeInstanceExtensions, nativeDeviceExtensions)) {
      m_insExtensions = nativeInstanceExtensions;
      m_devExtensions = nativeDeviceExtensions;
      m_initializedInsExt = true;
      m_initializedDevExt = true;
      Logger::info("OpenXR: Native loader Vulkan extensions enabled");
      return;
    }

    if (!m_wineOxr)
      m_wineOxr = this->loadLibrary();

    if (!m_wineOxr || m_initializedInsExt)
      return;

    if (!this->loadFunctions()) {
      this->shutdown();
      return;
    }

    m_insExtensions = this->queryInstanceExtensions();
    m_initializedInsExt = true;
  }


  bool DxvkXrProvider::loadFunctions() {
    g_winexrFunctions.__wineopenxr_GetVulkanInstanceExtensions =
        reinterpret_cast<PFN___wineopenxr_GetVulkanInstanceExtensions>(this->getSym("__wineopenxr_GetVulkanInstanceExtensions"));
    g_winexrFunctions.__wineopenxr_GetVulkanDeviceExtensions =
        reinterpret_cast<PFN___wineopenxr_GetVulkanDeviceExtensions>(this->getSym("__wineopenxr_GetVulkanDeviceExtensions"));
    return g_winexrFunctions.__wineopenxr_GetVulkanInstanceExtensions != nullptr
      && g_winexrFunctions.__wineopenxr_GetVulkanDeviceExtensions != nullptr;
  }


  void DxvkXrProvider::initDeviceExtensions(const DxvkInstance* instance) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!m_wineOxr || m_initializedDevExt)
      return;
    
    m_devExtensions = this->queryDeviceExtensions();
    m_initializedDevExt = true;

    this->shutdown();
  }


  DxvkNameSet DxvkXrProvider::queryInstanceExtensions() const {
    int res;
    uint32_t len;

    res = g_winexrFunctions.__wineopenxr_GetVulkanInstanceExtensions(0, &len, nullptr);
    if (res != 0) {
      Logger::warn("OpenXR: Unable to get required Vulkan instance extensions size");
      return DxvkNameSet();
    }

    std::vector<char> extensionList(len);
    res = g_winexrFunctions.__wineopenxr_GetVulkanInstanceExtensions(len, &len, &extensionList[0]);
    if (res != 0) {
      Logger::warn("OpenXR: Unable to get required Vulkan instance extensions");
      return DxvkNameSet();
    }

    return parseExtensionList(std::string(extensionList.data(), len));
  }
  
  
  DxvkNameSet DxvkXrProvider::queryDeviceExtensions() const {
    int res;

    uint32_t len;
    res = g_winexrFunctions.__wineopenxr_GetVulkanDeviceExtensions(0, &len, nullptr);
    if (res != 0) {
      Logger::warn("OpenXR: Unable to get required Vulkan Device extensions size");
      return DxvkNameSet();
    }

    std::vector<char> extensionList(len);
    res = g_winexrFunctions.__wineopenxr_GetVulkanDeviceExtensions(len, &len, &extensionList[0]);
    if (res != 0) {
      Logger::warn("OpenXR: Unable to get required Vulkan Device extensions");
      return DxvkNameSet();
    }

    return parseExtensionList(std::string(extensionList.data(), len));
  }
  
  
  DxvkNameSet DxvkXrProvider::parseExtensionList(const std::string& str) const {
    DxvkNameSet result;
    
    std::stringstream strstream(str);
    std::string       section;
    
    while (std::getline(strstream, section, ' '))
      result.add(section.c_str());
    
    return result;
  }


  bool DxvkXrProvider::shouldUseConfiguredOpenXrRuntime() const {
    const VrRuntimeBackendConfig runtimeConfig = L4D2VR_ReadRuntimeBackendConfig();
    return L4D2VR_ParseRuntimeBackend(runtimeConfig.requestedBackend, VrRuntimeBackend::OpenVR) == VrRuntimeBackend::OpenXR;
  }


  bool DxvkXrProvider::queryNativeOpenXrExtensions(
          DxvkNameSet& instanceExtensions,
          DxvkNameSet& deviceExtensions) const {
    HMODULE loader = ::LoadLibrary("openxr_loader.dll");
    if (!loader) {
      Logger::warn("OpenXR: Native openxr_loader.dll not found");
      return false;
    }

    auto cleanupLoader = [&]() {
      ::FreeLibrary(loader);
    };

    auto getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
      ::GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!getInstanceProcAddr) {
      Logger::warn("OpenXR: openxr_loader.dll does not export xrGetInstanceProcAddr");
      cleanupLoader();
      return false;
    }

    std::string detail;
    PFN_xrCreateInstance xrCreateInstanceFn = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionPropertiesFn = nullptr;

    if (!loadNativeXrFunction(getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstanceFn, detail) ||
        !loadNativeXrFunction(getInstanceProcAddr, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", xrEnumerateInstanceExtensionPropertiesFn, detail)) {
      Logger::warn(str::format("OpenXR: ", detail));
      cleanupLoader();
      return false;
    }

    if (!nativeOpenXrHasInstanceExtension(xrEnumerateInstanceExtensionPropertiesFn, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, detail)) {
      Logger::warn(str::format("OpenXR: ", detail));
      cleanupLoader();
      return false;
    }

    const char* extensions[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "L4D2VR");
    createInfo.applicationInfo.applicationVersion = 1;
    std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "DXVK");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
    createInfo.enabledExtensionNames = extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstanceFn(&createInfo, &instance);
    if (XR_FAILED(result) || instance == XR_NULL_HANDLE) {
      Logger::warn(str::format("OpenXR: ", formatXrResult("xrCreateInstance", result)));
      cleanupLoader();
      return false;
    }

    PFN_xrDestroyInstance xrDestroyInstanceFn = nullptr;
    if (!loadNativeXrFunction(getInstanceProcAddr, instance, "xrDestroyInstance", xrDestroyInstanceFn, detail)) {
      Logger::warn(str::format("OpenXR: ", detail));
      cleanupLoader();
      return false;
    }

    auto cleanupInstance = [&]() {
      if (instance != XR_NULL_HANDLE)
        xrDestroyInstanceFn(instance);
      cleanupLoader();
    };

    PFN_xrGetSystem xrGetSystemFn = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHRFn = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHRFn = nullptr;

    if (!loadNativeXrFunction(getInstanceProcAddr, instance, "xrGetSystem", xrGetSystemFn, detail) ||
        !loadNativeXrFunction(getInstanceProcAddr, instance, "xrGetVulkanInstanceExtensionsKHR", xrGetVulkanInstanceExtensionsKHRFn, detail) ||
        !loadNativeXrFunction(getInstanceProcAddr, instance, "xrGetVulkanDeviceExtensionsKHR", xrGetVulkanDeviceExtensionsKHRFn, detail)) {
      Logger::warn(str::format("OpenXR: ", detail));
      cleanupInstance();
      return false;
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    result = xrGetSystemFn(instance, &systemInfo, &systemId);
    if (XR_FAILED(result) || systemId == XR_NULL_SYSTEM_ID) {
      Logger::warn(str::format("OpenXR: ", formatXrResult("xrGetSystem(HMD)", result)));
      cleanupInstance();
      return false;
    }

    std::string instanceExtensionString;
    std::string deviceExtensionString;
    if (!queryNativeOpenXrVulkanExtensionString(
            xrGetVulkanInstanceExtensionsKHRFn,
            instance,
            systemId,
            "xrGetVulkanInstanceExtensionsKHR",
            instanceExtensionString,
            detail) ||
        !queryNativeOpenXrVulkanExtensionString(
            xrGetVulkanDeviceExtensionsKHRFn,
            instance,
            systemId,
            "xrGetVulkanDeviceExtensionsKHR",
            deviceExtensionString,
            detail)) {
      Logger::warn(str::format("OpenXR: ", detail));
      cleanupInstance();
      return false;
    }

    instanceExtensions = this->parseExtensionList(instanceExtensionString);
    deviceExtensions = this->parseExtensionList(deviceExtensionString);
    cleanupInstance();
    return true;
  }
  
  
  void DxvkXrProvider::shutdown() {
    if (m_loadedOxrApi)
      this->freeLibrary();
    
    m_loadedOxrApi      = false;
    m_wineOxr = nullptr;
  }


  HMODULE DxvkXrProvider::loadLibrary() {
    HMODULE handle = ::LoadLibrary("wineopenxr.dll");

    m_loadedOxrApi = handle != nullptr;
    return handle;
  }


  void DxvkXrProvider::freeLibrary() {
    ::FreeLibrary(m_wineOxr);
  }

  
  void* DxvkXrProvider::getSym(const char* sym) {
    return reinterpret_cast<void*>(
      ::GetProcAddress(m_wineOxr, sym));
  }
}
