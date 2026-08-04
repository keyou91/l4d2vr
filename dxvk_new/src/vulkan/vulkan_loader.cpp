#ifdef _WIN32
#include <mutex>
#include <unordered_map>
#endif
#include <tuple>

#include "vulkan_loader.h"

#include "../util/log/log.h"

#include "../util/util_string.h"
#include "../util/util_win32_compat.h"

namespace dxvk::vk {

#if defined(_WIN32) && defined(VK_KHR_swapchain)
  namespace {

    struct BandicamDeviceFunctions {
      PFN_vkDeviceWaitIdle          waitIdle         = nullptr;
      PFN_vkDestroySwapchainKHR     destroySwapchain = nullptr;
    };

    std::mutex g_bandicamMutex;
    std::unordered_map<VkDevice, BandicamDeviceFunctions> g_bandicamDevices;

    bool isBandicamVulkanHookLoaded() {
      return ::GetModuleHandleW(L"bdcamvk32.dll") != nullptr
          || ::GetModuleHandleW(L"bdcamvk64.dll") != nullptr;
    }

    VKAPI_ATTR void VKAPI_CALL destroySwapchainBandicamCompat(
            VkDevice                    device,
            VkSwapchainKHR              swapchain,
      const VkAllocationCallbacks*      allocator) {
      BandicamDeviceFunctions functions;

      {
        std::lock_guard lock(g_bandicamMutex);
        auto entry = g_bandicamDevices.find(device);

        if (entry != g_bandicamDevices.end())
          functions = entry->second;
      }

      if (functions.waitIdle)
        functions.waitIdle(device);

      if (functions.destroySwapchain)
        functions.destroySwapchain(device, swapchain, allocator);
    }

  }
#endif

  static std::pair<HMODULE, PFN_vkGetInstanceProcAddr> loadVulkanLibrary() {
    static const std::array<const char*, 2> dllNames = {{
#ifdef _WIN32
      "winevulkan.dll",
      "vulkan-1.dll",
#else
      "libvulkan.so",
      "libvulkan.so.1",
#endif
    }};

    for (auto dllName : dllNames) {
      HMODULE library = LoadLibraryA(dllName);

      if (!library)
        continue;

      auto proc = GetProcAddress(library, "vkGetInstanceProcAddr");

      if (!proc) {
        FreeLibrary(library);
        continue;
      }

      Logger::info(str::format("Vulkan: Found vkGetInstanceProcAddr in ", dllName, " @ 0x", std::hex, reinterpret_cast<uintptr_t>(proc)));
      return std::make_pair(library, reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc));
    }

    Logger::err("Vulkan: vkGetInstanceProcAddr not found");
    return { };
  }

  LibraryLoader::LibraryLoader() {
    std::tie(m_library, m_getInstanceProcAddr) = loadVulkanLibrary();
  }

  LibraryLoader::LibraryLoader(PFN_vkGetInstanceProcAddr loaderProc) {
    m_getInstanceProcAddr = loaderProc;
  }

  LibraryLoader::~LibraryLoader() {
    if (m_library)
      FreeLibrary(m_library);
  }

  PFN_vkVoidFunction LibraryLoader::sym(VkInstance instance, const char* name) const {
    return m_getInstanceProcAddr(instance, name);
  }

  PFN_vkVoidFunction LibraryLoader::sym(const char* name) const {
    return sym(nullptr, name);
  }

  bool LibraryLoader::valid() const {
    return m_getInstanceProcAddr != nullptr;
  }
  
  
  InstanceLoader::InstanceLoader(const Rc<LibraryLoader>& library, bool owned, VkInstance instance)
  : m_library(library), m_instance(instance), m_owned(owned) { }
  
  
  PFN_vkVoidFunction InstanceLoader::sym(const char* name) const {
    return m_library->sym(m_instance, name);
  }
  
  
  DeviceLoader::DeviceLoader(const Rc<InstanceLoader>& library, bool owned, VkDevice device)
  : m_library(library)
  , m_getDeviceProcAddr(reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      m_library->sym("vkGetDeviceProcAddr"))),
    m_device(device), m_owned(owned) { }
  
  
  PFN_vkVoidFunction DeviceLoader::sym(const char* name) const {
    return m_getDeviceProcAddr(m_device, name);
  }
  
  
  LibraryFn::LibraryFn() { }
  LibraryFn::LibraryFn(PFN_vkGetInstanceProcAddr loaderProc)
  : LibraryLoader(loaderProc) { }
  LibraryFn::~LibraryFn() { }
  
  
  InstanceFn::InstanceFn(const Rc<LibraryLoader>& library, bool owned, VkInstance instance)
  : InstanceLoader(library, owned, instance) { }
  InstanceFn::~InstanceFn() {
    if (m_owned)
      this->vkDestroyInstance(m_instance, nullptr);
  }
  
  
  DeviceFn::DeviceFn(const Rc<InstanceLoader>& library, bool owned, VkDevice device)
  : DeviceLoader(library, owned, device) {
#if defined(_WIN32) && defined(VK_KHR_swapchain)
    if (isBandicamVulkanHookLoaded()
     && this->vkDeviceWaitIdle
     && this->vkDestroySwapchainKHR) {
      {
        std::lock_guard lock(g_bandicamMutex);
        g_bandicamDevices[device] = {
          this->vkDeviceWaitIdle,
          this->vkDestroySwapchainKHR,
        };
      }

      this->vkDestroySwapchainKHR = &destroySwapchainBandicamCompat;
      Logger::warn("Vulkan: Bandicam hook detected; synchronizing the device before swapchain destruction.");
    }
#endif
  }

  DeviceFn::~DeviceFn() {
    if (m_owned)
      this->vkDestroyDevice(m_device, nullptr);

#if defined(_WIN32) && defined(VK_KHR_swapchain)
    std::lock_guard lock(g_bandicamMutex);
    g_bandicamDevices.erase(m_device);
#endif
  }
  
}
