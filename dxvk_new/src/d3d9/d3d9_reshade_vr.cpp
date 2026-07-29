#include "d3d9_reshade_vr.h"
#include "d3d9_reshade_api.h"

#include "../util/log/log.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" BOOL WINAPI K32EnumProcessModules(
        HANDLE    process,
        HMODULE*  modules,
        DWORD     size,
        LPDWORD   needed);

namespace dxvk {

  namespace {

    using ReShadeRegisterAddonProc = bool (__cdecl *)(void*, uint32_t);
    using ReShadeUnregisterAddonProc = void (__cdecl *)(void*);
    using ReShadeRegisterEventForAddonProc = void (__cdecl *)(void*, reshade_api::addon_event, void*);
    using ReShadeUnregisterEventForAddonProc = void (__cdecl *)(void*, reshade_api::addon_event, void*);

    constexpr char kReShadeVrCompatConfigKey[] = "ReShadeVRCompat";
    constexpr char kGenericDepthAddonName[] = "Generic Depth";
    constexpr char kManagedGenericDepthKey[] = "ManagedGenericDepthDisable";

    std::once_flag g_prepareConfigurationOnce;

    std::string TrimAscii(std::string value) {
      const auto isSpace = [] (unsigned char c) { return std::isspace(c) != 0; };
      value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [&] (char c) { return !isSpace(static_cast<unsigned char>(c)); }));
      value.erase(std::find_if(value.rbegin(), value.rend(),
        [&] (char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), value.end());
      return value;
    }

    bool EqualsAsciiI(std::string_view a, std::string_view b) {
      if (a.size() != b.size())
        return false;

      for (size_t i = 0; i < a.size(); i++) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
          return false;
      }

      return true;
    }

    bool ParseBoolValue(std::string value, bool& result) {
      value = TrimAscii(std::move(value));
      std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });

      if (value == "1" || value == "true" || value == "on" || value == "yes") {
        result = true;
        return true;
      }

      if (value == "0" || value == "false" || value == "off" || value == "no") {
        result = false;
        return true;
      }

      return false;
    }

    std::filesystem::path GetProcessRootPath() {
      wchar_t executablePath[MAX_PATH] = { };
      const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
      if (length == 0 || length >= MAX_PATH)
        return { };

      return std::filesystem::path(executablePath).parent_path();
    }

    void ReadCompatConfigFile(const std::filesystem::path& path, bool& value) {
      std::ifstream stream(path, std::ios::binary);
      if (!stream)
        return;

      std::string line;
      bool firstLine = true;
      while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (firstLine && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
          line.erase(0, 3);
        firstLine = false;

        size_t comment = std::string::npos;
        const size_t slashComment = line.find("//");
        const size_t hashComment = line.find('#');
        const size_t semicolonComment = line.find(';');
        if (slashComment != std::string::npos)
          comment = slashComment;
        if (hashComment != std::string::npos)
          comment = (std::min)(comment, hashComment);
        if (semicolonComment != std::string::npos)
          comment = (std::min)(comment, semicolonComment);
        if (comment != std::string::npos)
          line.erase(comment);

        const size_t equals = line.find('=');
        if (equals == std::string::npos)
          continue;

        const std::string key = TrimAscii(line.substr(0, equals));
        if (!EqualsAsciiI(key, kReShadeVrCompatConfigKey))
          continue;

        bool parsedValue = value;
        if (ParseBoolValue(line.substr(equals + 1), parsedValue))
          value = parsedValue;
      }
    }

    bool ReadReShadeVrCompatEnabled() {
      const std::filesystem::path root = GetProcessRootPath();
      if (root.empty())
        return false;

      bool enabled = false;
      ReadCompatConfigFile(root / L"VR" / L"config.txt", enabled);
      ReadCompatConfigFile(root / L"VR" / L"config2.txt", enabled);
      return enabled;
    }

    struct IniDocument {
      bool utf8Bom = false;
      std::vector<std::string> lines;
    };

    bool ReadIniDocument(const std::filesystem::path& path, IniDocument& document) {
      document = { };

      std::error_code ec;
      if (!std::filesystem::exists(path, ec))
        return !ec;

      std::ifstream stream(path, std::ios::binary);
      if (!stream)
        return false;

      std::ostringstream buffer;
      buffer << stream.rdbuf();
      std::string text = buffer.str();
      if (text.size() >= 3 &&
          static_cast<unsigned char>(text[0]) == 0xEF &&
          static_cast<unsigned char>(text[1]) == 0xBB &&
          static_cast<unsigned char>(text[2]) == 0xBF) {
        document.utf8Bom = true;
        text.erase(0, 3);
      }

      std::istringstream lineStream(text);
      std::string line;
      while (std::getline(lineStream, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        document.lines.push_back(std::move(line));
      }

      return true;
    }

    bool WriteIniDocument(const std::filesystem::path& path, const IniDocument& document) {
      std::ofstream stream(path, std::ios::binary | std::ios::trunc);
      if (!stream)
        return false;

      if (document.utf8Bom)
        stream.write("\xEF\xBB\xBF", 3);

      for (const std::string& line : document.lines)
        stream << line << "\r\n";

      stream.flush();
      return stream.good();
    }

    bool ParseIniSection(const std::string& line, std::string& section) {
      const std::string trimmed = TrimAscii(line);
      if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
        return false;

      section = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
      return !section.empty();
    }

    bool ParseIniAssignment(const std::string& line, std::string& key, std::string& value) {
      const std::string trimmed = TrimAscii(line);
      if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#')
        return false;

      const size_t equals = trimmed.find('=');
      if (equals == std::string::npos)
        return false;

      key = TrimAscii(trimmed.substr(0, equals));
      value = TrimAscii(trimmed.substr(equals + 1));
      return !key.empty();
    }

    bool FindIniSection(
            const IniDocument& document,
            std::string_view   sectionName,
            size_t&            sectionHeader,
            size_t&            sectionEnd) {
      for (size_t i = 0; i < document.lines.size(); i++) {
        std::string section;
        if (!ParseIniSection(document.lines[i], section) || !EqualsAsciiI(section, sectionName))
          continue;

        sectionHeader = i;
        sectionEnd = document.lines.size();
        for (size_t j = i + 1; j < document.lines.size(); j++) {
          std::string nextSection;
          if (ParseIniSection(document.lines[j], nextSection)) {
            sectionEnd = j;
            break;
          }
        }
        return true;
      }

      return false;
    }

    bool FindIniKey(
            const IniDocument& document,
            std::string_view   sectionName,
            std::string_view   keyName,
            size_t&            keyIndex,
            std::string*       value = nullptr) {
      size_t sectionHeader = 0;
      size_t sectionEnd = 0;
      if (!FindIniSection(document, sectionName, sectionHeader, sectionEnd))
        return false;

      for (size_t i = sectionHeader + 1; i < sectionEnd; i++) {
        std::string key;
        std::string parsedValue;
        if (!ParseIniAssignment(document.lines[i], key, parsedValue) || !EqualsAsciiI(key, keyName))
          continue;

        keyIndex = i;
        if (value != nullptr)
          *value = std::move(parsedValue);
        return true;
      }

      return false;
    }

    void SetIniValue(
            IniDocument&     document,
            std::string_view sectionName,
            std::string_view keyName,
            const std::string& value) {
      size_t keyIndex = 0;
      if (FindIniKey(document, sectionName, keyName, keyIndex)) {
        document.lines[keyIndex] = std::string(keyName) + "=" + value;
        return;
      }

      size_t sectionHeader = 0;
      size_t sectionEnd = 0;
      if (FindIniSection(document, sectionName, sectionHeader, sectionEnd)) {
        document.lines.insert(document.lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd),
          std::string(keyName) + "=" + value);
        return;
      }

      if (!document.lines.empty() && !document.lines.back().empty())
        document.lines.emplace_back();
      document.lines.push_back("[" + std::string(sectionName) + "]");
      document.lines.push_back(std::string(keyName) + "=" + value);
    }

    void EraseIniKey(
            IniDocument&     document,
            std::string_view sectionName,
            std::string_view keyName) {
      size_t keyIndex = 0;
      if (FindIniKey(document, sectionName, keyName, keyIndex))
        document.lines.erase(document.lines.begin() + static_cast<std::ptrdiff_t>(keyIndex));
    }

    std::vector<std::string> ParseIniList(const std::string& value) {
      std::vector<std::string> result;
      size_t begin = 0;
      while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        std::string element = TrimAscii(value.substr(begin,
          comma == std::string::npos ? std::string::npos : comma - begin));
        if (!element.empty())
          result.push_back(std::move(element));
        if (comma == std::string::npos)
          break;
        begin = comma + 1;
      }
      return result;
    }

    std::string JoinIniList(const std::vector<std::string>& values) {
      std::string result;
      for (const std::string& value : values) {
        if (!result.empty())
          result += ',';
        result += value;
      }
      return result;
    }

    enum class GenericDepthConfigResult {
      unchanged,
      disabled,
      restored,
      failed,
    };

    GenericDepthConfigResult ConfigureGenericDepthAddon(bool bridgeEnabled) {
      const std::filesystem::path root = GetProcessRootPath();
      if (root.empty())
        return GenericDepthConfigResult::failed;

      const std::filesystem::path reshadeConfigPath = root / L"ReShade.ini";
      IniDocument document;
      if (!ReadIniDocument(reshadeConfigPath, document))
        return GenericDepthConfigResult::failed;

      std::string disabledValue;
      size_t disabledIndex = 0;
      const bool hasDisabledValue = FindIniKey(
        document, "ADDON", "DisabledAddons", disabledIndex, &disabledValue);
      std::vector<std::string> disabledAddons = hasDisabledValue
        ? ParseIniList(disabledValue)
        : std::vector<std::string>();

      const auto genericDepth = std::find_if(disabledAddons.begin(), disabledAddons.end(), [] (const std::string& value) {
        return EqualsAsciiI(value, kGenericDepthAddonName);
      });

      std::string managedValue;
      size_t managedIndex = 0;
      bool managedDisable = false;
      if (FindIniKey(document, "L4D2VR", kManagedGenericDepthKey, managedIndex, &managedValue))
        ParseBoolValue(managedValue, managedDisable);

      if (bridgeEnabled) {
        if (genericDepth != disabledAddons.end())
          return GenericDepthConfigResult::unchanged;

        disabledAddons.push_back(kGenericDepthAddonName);
        SetIniValue(document, "ADDON", "DisabledAddons", JoinIniList(disabledAddons));
        SetIniValue(document, "L4D2VR", kManagedGenericDepthKey, "1");
        if (!WriteIniDocument(reshadeConfigPath, document))
          return GenericDepthConfigResult::failed;
        return GenericDepthConfigResult::disabled;
      }

      if (!managedDisable)
        return GenericDepthConfigResult::unchanged;

      disabledAddons.erase(std::remove_if(disabledAddons.begin(), disabledAddons.end(), [] (const std::string& value) {
        return EqualsAsciiI(value, kGenericDepthAddonName);
      }), disabledAddons.end());

      if (disabledAddons.empty())
        EraseIniKey(document, "ADDON", "DisabledAddons");
      else
        SetIniValue(document, "ADDON", "DisabledAddons", JoinIniList(disabledAddons));
      EraseIniKey(document, "L4D2VR", kManagedGenericDepthKey);

      if (!WriteIniDocument(reshadeConfigPath, document))
        return GenericDepthConfigResult::failed;
      return GenericDepthConfigResult::restored;
    }

    std::atomic<uintptr_t> g_depthDevice { 0 };
    std::atomic<uint32_t>  g_depthWidth { 0 };
    std::atomic<uint32_t>  g_depthHeight { 0 };
    std::atomic<uint64_t>  g_depthView { 0 };

    // 0 = not attempted, 1 = registered, -1 = unavailable/failed.
    std::atomic<int> g_registrationState { 0 };
    std::atomic<bool> g_runtimeMismatchLogged { false };
    std::atomic<bool> g_bindingSuccessLogged { false };

    // Updating a DEPTH semantic can make ReShade wait for the Vulkan queue while
    // rewriting descriptors. Keep that operation off the normal frame path.
    // Generic Depth is disabled before ReShade initializes, so ordinary frames
    // only need atomic identity checks and never query or rewrite descriptors.
    std::atomic<uintptr_t> g_boundRuntime { 0 };
    std::atomic<uint64_t>  g_boundDepthView { 0 };
    std::atomic<bool>      g_bindingDirty { true };

    // The exact bridge is only allowed to register after the built-in Generic
    // Depth tracker has either been disabled successfully or was already disabled.
    std::atomic<bool> g_genericDepthConfigurationReady { false };

    // Vulkan normally creates a desktop runtime before the OpenVR runtime. Cache
    // the desktop runtime so its regular callback becomes one atomic comparison.
    std::atomic<uintptr_t> g_desktopRuntime { 0 };

    std::mutex g_registrationMutex;
    std::mutex g_bindingMutex;

    HMODULE g_reshadeModule = nullptr;
    HMODULE g_addonModule = nullptr;
    ReShadeUnregisterAddonProc g_unregisterAddon = nullptr;
    ReShadeUnregisterEventForAddonProc g_unregisterEvent = nullptr;

    template<typename T>
    uint64_t VulkanHandleToUint64(T handle) {
      if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<uint64_t>(handle);
      else
        return static_cast<uint64_t>(handle);
    }

    uint32_t EnumerateProcessModules(HMODULE* modules, uint32_t capacity) {
      if (modules == nullptr || capacity == 0)
        return 0;

      DWORD needed = 0;
      const DWORD bytes = capacity * static_cast<DWORD>(sizeof(HMODULE));
      if (!K32EnumProcessModules(GetCurrentProcess(), modules, bytes, &needed))
        return 0;

      return (std::min)(
        static_cast<uint32_t>(needed / static_cast<DWORD>(sizeof(HMODULE))),
        capacity);
    }

    HMODULE FindReShadeModule() {
      HMODULE modules[1024] = { };
      const uint32_t count = EnumerateProcessModules(modules, static_cast<uint32_t>(std::size(modules)));
      for (uint32_t i = 0; i < count; i++) {
        HMODULE module = modules[i];
        if (GetProcAddress(module, "ReShadeRegisterAddon") != nullptr &&
            GetProcAddress(module, "ReShadeUnregisterAddon") != nullptr &&
            GetProcAddress(module, "ReShadeRegisterEventForAddon") != nullptr &&
            GetProcAddress(module, "ReShadeUnregisterEventForAddon") != nullptr)
          return module;
      }

      return nullptr;
    }

    bool IsModuleLoaded(HMODULE target) {
      if (target == nullptr)
        return false;

      HMODULE modules[1024] = { };
      const uint32_t count = EnumerateProcessModules(modules, static_cast<uint32_t>(std::size(modules)));
      for (uint32_t i = 0; i < count; i++) {
        if (modules[i] == target)
          return true;
      }

      return false;
    }

    HMODULE GetCurrentAddonModule() {
      HMODULE module = nullptr;
      GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&D3D9ReShadeVrPublishDepth),
        &module);
      return module;
    }

    void ResetRuntimeBindingStateLocked(reshade_api::effect_runtime* runtime = nullptr) {
      const uintptr_t runtimeAddress = reinterpret_cast<uintptr_t>(runtime);
      if (runtime == nullptr || g_boundRuntime.load(std::memory_order_relaxed) == runtimeAddress) {
        g_boundRuntime.store(0, std::memory_order_relaxed);
        g_boundDepthView.store(0, std::memory_order_relaxed);
        g_bindingDirty.store(true, std::memory_order_release);
      }
    }

    bool IsDepthBindingCurrent(
            uintptr_t runtimeAddress,
            uint64_t  depthView) {
      return !g_bindingDirty.load(std::memory_order_acquire) &&
             g_boundRuntime.load(std::memory_order_relaxed) == runtimeAddress &&
             g_boundDepthView.load(std::memory_order_relaxed) == depthView;
    }

    void __cdecl SetDepthReadyUniform(
            reshade_api::effect_runtime* runtime,
            reshade_api::effect_uniform_variable variable,
            void*) {
      char source[32] = { };
      size_t sourceSize = sizeof(source);
      if (runtime->get_annotation_string_from_uniform_variable(
            variable, "source", source, &sourceSize) &&
          std::strcmp(source, "bufready_depth") == 0) {
        const bool ready = true;
        runtime->set_uniform_value_bool(variable, &ready, 1);
      }
    }

    void __cdecl OnReShadeDestroyEffectRuntime(reshade_api::effect_runtime* runtime) {
      std::lock_guard<std::mutex> lock(g_bindingMutex);

      const uintptr_t runtimeAddress = reinterpret_cast<uintptr_t>(runtime);
      if (g_desktopRuntime.load(std::memory_order_relaxed) == runtimeAddress)
        g_desktopRuntime.store(0, std::memory_order_relaxed);

      ResetRuntimeBindingStateLocked(runtime);
    }

    void __cdecl OnReShadeReloadedEffects(reshade_api::effect_runtime* runtime) {
      std::lock_guard<std::mutex> lock(g_bindingMutex);

      const uintptr_t runtimeAddress = reinterpret_cast<uintptr_t>(runtime);
      const uintptr_t boundRuntime = g_boundRuntime.load(std::memory_order_relaxed);
      if (boundRuntime == 0 || boundRuntime == runtimeAddress)
        g_bindingDirty.store(true, std::memory_order_release);
    }

    void __cdecl OnReShadeBeginEffects(
            reshade_api::effect_runtime* runtime,
            reshade_api::command_list*,
            reshade_api::resource_view,
            reshade_api::resource_view) {
      if (runtime == nullptr)
        return;

      uint64_t depthView = g_depthView.load(std::memory_order_acquire);
      if (depthView == 0)
        return;

      const uintptr_t runtimeAddress = reinterpret_cast<uintptr_t>(runtime);
      if (g_desktopRuntime.load(std::memory_order_relaxed) == runtimeAddress)
        return;

      // Normal-frame path: atomic comparisons only. No descriptor query,
      // descriptor rewrite, Vulkan queue wait or bridge mutex.
      if (IsDepthBindingCurrent(runtimeAddress, depthView))
        return;

      std::lock_guard<std::mutex> lock(g_bindingMutex);

      // Re-read after taking the slow-path lock, since D3D reset/destruction may
      // have invalidated or replaced the atlas while this callback was waiting.
      depthView = g_depthView.load(std::memory_order_acquire);
      if (depthView == 0 || IsDepthBindingCurrent(runtimeAddress, depthView))
        return;

      if (runtime->get_hwnd() != nullptr) {
        g_desktopRuntime.store(runtimeAddress, std::memory_order_relaxed);
        return;
      }

      reshade_api::device* device = runtime->get_device();
      if (device == nullptr || device->get_api() != reshade_api::device_api::vulkan)
        return;

      const uintptr_t publishedDevice = g_depthDevice.load(std::memory_order_relaxed);
      if (publishedDevice == 0 || static_cast<uintptr_t>(device->get_native()) != publishedDevice)
        return;

      uint32_t runtimeWidth = 0;
      uint32_t runtimeHeight = 0;
      runtime->get_screenshot_width_and_height(&runtimeWidth, &runtimeHeight);
      if (runtimeWidth != g_depthWidth.load(std::memory_order_relaxed) ||
          runtimeHeight != g_depthHeight.load(std::memory_order_relaxed)) {
        if (!g_runtimeMismatchLogged.exchange(true, std::memory_order_relaxed))
          Logger::warn("L4D2VR ReShade VR: OpenVR effect runtime size does not match the stereo depth atlas");
        return;
      }

      const reshade_api::resource_view view = { depthView };
      runtime->update_texture_bindings("DEPTH", view, view);
      runtime->enumerate_uniform_variables(nullptr, &SetDepthReadyUniform, nullptr);

      g_boundRuntime.store(runtimeAddress, std::memory_order_relaxed);
      g_boundDepthView.store(depthView, std::memory_order_relaxed);
      g_bindingDirty.store(false, std::memory_order_release);

      if (!g_bindingSuccessLogged.exchange(true, std::memory_order_relaxed))
        Logger::info("L4D2VR ReShade VR: bound the GPU stereo depth atlas to the OpenVR DEPTH semantic");
    }

    bool RegisterBridge() {
      D3D9ReShadeVrPrepareConfiguration();
      if (!g_genericDepthConfigurationReady.load(std::memory_order_acquire)) {
        Logger::warn("L4D2VR ReShade VR: bridge initialization stopped because ReShade Generic Depth could not be disabled safely");
        g_registrationState.store(-1, std::memory_order_release);
        return false;
      }

      const int currentState = g_registrationState.load(std::memory_order_acquire);
      if (currentState != 0)
        return currentState > 0;

      std::lock_guard<std::mutex> lock(g_registrationMutex);
      const int lockedState = g_registrationState.load(std::memory_order_relaxed);
      if (lockedState != 0)
        return lockedState > 0;

      HMODULE reshadeModule = FindReShadeModule();
      HMODULE addonModule = GetCurrentAddonModule();
      if (reshadeModule == nullptr || addonModule == nullptr) {
        Logger::warn("L4D2VR ReShade VR: ReShade full add-on runtime was not found; install the Vulkan full add-on build and restart");
        g_registrationState.store(-1, std::memory_order_release);
        return false;
      }

      const auto registerAddon = reinterpret_cast<ReShadeRegisterAddonProc>(
        GetProcAddress(reshadeModule, "ReShadeRegisterAddon"));
      const auto unregisterAddon = reinterpret_cast<ReShadeUnregisterAddonProc>(
        GetProcAddress(reshadeModule, "ReShadeUnregisterAddon"));
      const auto registerEvent = reinterpret_cast<ReShadeRegisterEventForAddonProc>(
        GetProcAddress(reshadeModule, "ReShadeRegisterEventForAddon"));
      const auto unregisterEvent = reinterpret_cast<ReShadeUnregisterEventForAddonProc>(
        GetProcAddress(reshadeModule, "ReShadeUnregisterEventForAddon"));

      if (registerAddon == nullptr || unregisterAddon == nullptr ||
          registerEvent == nullptr || unregisterEvent == nullptr ||
          !registerAddon(addonModule, reshade_api::ApiVersion)) {
        Logger::warn("L4D2VR ReShade VR: add-on registration failed; a current ReShade full add-on build with API 18 support is required");
        g_registrationState.store(-1, std::memory_order_release);
        return false;
      }

      registerEvent(addonModule, reshade_api::addon_event::destroy_effect_runtime,
        reinterpret_cast<void*>(&OnReShadeDestroyEffectRuntime));
      registerEvent(addonModule, reshade_api::addon_event::reshade_begin_effects,
        reinterpret_cast<void*>(&OnReShadeBeginEffects));
      registerEvent(addonModule, reshade_api::addon_event::reshade_reloaded_effects,
        reinterpret_cast<void*>(&OnReShadeReloadedEffects));

      g_reshadeModule = reshadeModule;
      g_addonModule = addonModule;
      g_unregisterAddon = unregisterAddon;
      g_unregisterEvent = unregisterEvent;
      g_registrationState.store(1, std::memory_order_release);

      Logger::info("L4D2VR ReShade VR: registered exact stereo DEPTH binding for the OpenVR Vulkan runtime (add-on API 18)");
      return true;
    }

  }

  void D3D9ReShadeVrPrepareConfiguration() {
    std::call_once(g_prepareConfigurationOnce, [] {
      const bool bridgeEnabled = ReadReShadeVrCompatEnabled();
      const GenericDepthConfigResult result = ConfigureGenericDepthAddon(bridgeEnabled);
      g_genericDepthConfigurationReady.store(
        !bridgeEnabled || result != GenericDepthConfigResult::failed,
        std::memory_order_release);

      switch (result) {
      case GenericDepthConfigResult::disabled:
        Logger::info("L4D2VR ReShade VR: disabled ReShade Generic Depth before Vulkan initialization to avoid per-draw depth tracking");
        break;
      case GenericDepthConfigResult::restored:
        Logger::info("L4D2VR ReShade VR: restored the ReShade Generic Depth add-on after ReShadeVRCompat was disabled");
        break;
      case GenericDepthConfigResult::failed:
        if (bridgeEnabled)
          Logger::warn("L4D2VR ReShade VR: failed to update ReShade.ini; the exact depth bridge will remain disabled");
        break;
      case GenericDepthConfigResult::unchanged:
        break;
      }
    });
  }

  bool D3D9ReShadeVrInitialize() {
    return RegisterBridge();
  }

  void D3D9ReShadeVrShutdown() {
    std::lock_guard<std::mutex> registrationLock(g_registrationMutex);

    if (g_registrationState.load(std::memory_order_relaxed) > 0 &&
        IsModuleLoaded(g_reshadeModule) &&
        g_addonModule != nullptr &&
        g_unregisterEvent != nullptr &&
        g_unregisterAddon != nullptr) {
      g_unregisterEvent(g_addonModule, reshade_api::addon_event::reshade_reloaded_effects,
        reinterpret_cast<void*>(&OnReShadeReloadedEffects));
      g_unregisterEvent(g_addonModule, reshade_api::addon_event::reshade_begin_effects,
        reinterpret_cast<void*>(&OnReShadeBeginEffects));
      g_unregisterEvent(g_addonModule, reshade_api::addon_event::destroy_effect_runtime,
        reinterpret_cast<void*>(&OnReShadeDestroyEffectRuntime));
      g_unregisterAddon(g_addonModule);
    }

    g_registrationState.store(0, std::memory_order_release);
    g_reshadeModule = nullptr;
    g_addonModule = nullptr;
    g_unregisterAddon = nullptr;
    g_unregisterEvent = nullptr;

    std::lock_guard<std::mutex> bindingLock(g_bindingMutex);
    g_depthView.store(0, std::memory_order_release);
    g_depthWidth.store(0, std::memory_order_relaxed);
    g_depthHeight.store(0, std::memory_order_relaxed);
    g_depthDevice.store(0, std::memory_order_relaxed);
    g_desktopRuntime.store(0, std::memory_order_relaxed);
    ResetRuntimeBindingStateLocked();
  }

  bool D3D9ReShadeVrIsReady() {
    return g_registrationState.load(std::memory_order_acquire) > 0;
  }

  void D3D9ReShadeVrInvalidateBinding() {
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    g_bindingDirty.store(true, std::memory_order_release);
  }

  void D3D9ReShadeVrPublishDepth(
          VkDevice        device,
          VkImageView     depthView,
          uint32_t        width,
          uint32_t        height) {
    if (device == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE || width == 0 || height == 0)
      return;

    const uintptr_t deviceHandle = reinterpret_cast<uintptr_t>(device);
    const uint64_t viewHandle = VulkanHandleToUint64(depthView);
    const bool changed =
      g_depthDevice.load(std::memory_order_relaxed) != deviceHandle ||
      g_depthView.load(std::memory_order_relaxed) != viewHandle ||
      g_depthWidth.load(std::memory_order_relaxed) != width ||
      g_depthHeight.load(std::memory_order_relaxed) != height;

    if (!changed)
      return;

    std::lock_guard<std::mutex> lock(g_bindingMutex);
    ResetRuntimeBindingStateLocked();

    g_depthDevice.store(deviceHandle, std::memory_order_relaxed);
    g_depthWidth.store(width, std::memory_order_relaxed);
    g_depthHeight.store(height, std::memory_order_relaxed);
    g_runtimeMismatchLogged.store(false, std::memory_order_relaxed);
    g_depthView.store(viewHandle, std::memory_order_release);
  }

  void D3D9ReShadeVrClearDepth(VkDevice device) {
    if (device == VK_NULL_HANDLE)
      return;

    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (g_depthDevice.load(std::memory_order_relaxed) != reinterpret_cast<uintptr_t>(device))
      return;

    g_depthView.store(0, std::memory_order_release);
    g_depthWidth.store(0, std::memory_order_relaxed);
    g_depthHeight.store(0, std::memory_order_relaxed);
    g_depthDevice.store(0, std::memory_order_relaxed);
    ResetRuntimeBindingStateLocked();
  }

}

extern "C" bool __cdecl L4D2VR_InitializeReShadeVRBridge() {
  return dxvk::D3D9ReShadeVrInitialize();
}

extern "C" void __cdecl L4D2VR_ShutdownReShadeVRBridge() {
  dxvk::D3D9ReShadeVrShutdown();
}
