// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")
#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <cwctype>
#include <cctype>
#include <cstdlib>
#include <system_error>
#include <shellapi.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>
#include "game.h"
#include "hooks.h"
#include "sdk.h"

extern "C" void __cdecl L4D2VR_ShutdownSystemMouseInputSuppression();
extern "C" void __cdecl L4D2VR_ShutdownReShadeVRBridge();

namespace
{
    constexpr wchar_t kDesiredVrWindowTitle[] = L"Left 4 Dead 2 VR - Vulkan";
    constexpr wchar_t kUiFontFixFileName[] = L"UI Font Fix.vpk";
    constexpr wchar_t kWorkshopUpdateItemId[] = L"3724995607";
    constexpr wchar_t kWorkshopUpdateVpkName[] = L"3724995607.vpk";
    constexpr wchar_t kConfigMigrationsStateFile[] = L"config_migrations_applied.txt";
    constexpr char kForcedDataCacheHeapSizeKiB[] = "2097151";
    constexpr uint32_t kForcedDataCacheSizeBytes = 2097152u * 1024u;
    constexpr uint32_t kOriginalIndexBufferSize = 32768u;
    constexpr uint32_t kForcedIndexBufferSize = 65535u;

    static_assert(kForcedDataCacheSizeBytes == 0x80000000u,
        "The forced datacache budget must be exactly 2 GiB.");

    // L4D2's tier0 command-line interface. Keep this ABI-only declaration in the
    // same order as ICommandLine; adding a virtual destructor would shift every slot.
    class SourceCommandLine
    {
    public:
        virtual void CreateCmdLine(const char* commandline) = 0;
        virtual void CreateCmdLine(int argc, char** argv) = 0;
        virtual const char* GetCmdLine() const = 0;
        virtual const char* CheckParm(const char* parameter, const char** value = nullptr) const = 0;
        virtual void RemoveParm(const char* parameter) = 0;
        virtual void AppendParm(const char* parameter, const char* values) = 0;
    };

    struct SourceDataCacheStatus
    {
        uint32_t bytes = 0;
        uint32_t items = 0;
        uint32_t bytesLocked = 0;
        uint32_t itemsLocked = 0;
        uint32_t findRequests = 0;
        uint32_t findHits = 0;
    };

    struct SourceDataCacheLimits
    {
        uint32_t maxBytes = UINT32_MAX;
        uint32_t maxItems = UINT32_MAX;
        uint32_t minBytes = 0;
        uint32_t minItems = 0;
    };

    // VDataCache003 in L4D2 has the five legacy IAppSystem methods followed by
    // IDataCache. Only SetSize/GetStatus are used here, but the preceding slots
    // must be declared to preserve their vtable indices (5 and 8 respectively).
    class SourceDataCache
    {
    public:
        virtual bool Connect(void* factory) = 0;
        virtual void Disconnect() = 0;
        virtual void* QueryInterface(const char* interfaceName) = 0;
        virtual int Init() = 0;
        virtual void Shutdown() = 0;
        virtual void SetSize(int maxBytes) = 0;
        virtual void SetOptions(uint32_t options) = 0;
        virtual void SetSectionLimits(const char* sectionName, const SourceDataCacheLimits& limits) = 0;
        virtual void GetStatus(SourceDataCacheStatus* status, SourceDataCacheLimits* limits = nullptr) = 0;
    };

    struct DataCacheForceState
    {
        bool enabled = false;
        bool commandLineOverridden = false;
        bool interfaceFound = false;
        bool alreadyAtTarget = false;
        bool safeLimitApplied = false;
        bool exactLimitApplied = false;
        uint32_t observedLimitBytes = 0;
        size_t exactLimitCandidateCount = 0;
    };

    DataCacheForceState g_DataCacheForceState;

    // Left4Neko raises the three hard-coded dynamic index-buffer limits in the
    // active shaderapi module. Keep the same signatures here so VRMod can provide
    // the safety margin when Left4Neko is not actually active.
    constexpr std::array<uint8_t, 16> kIndexBufferPatternCreate{
        0x6A, 0x01, 0xFF, 0xD2, 0x0F, 0xB6, 0xC0, 0x50,
        0x68, 0x00, 0x80, 0x00, 0x00, 0x53, 0x8B, 0xCF };
    constexpr std::array<uint8_t, 11> kIndexBufferPatternMember{
        0xC7, 0x86, 0xC8, 0x03, 0x00, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x89 };
    constexpr std::array<uint8_t, 6> kIndexBufferPatternGetter{
        0xB8, 0x00, 0x80, 0x00, 0x00, 0xC3 };

    struct IndexBufferPatchPattern
    {
        const uint8_t* bytes = nullptr;
        size_t length = 0;
        size_t immediateOffset = 0;
        const char* name = nullptr;
    };

    constexpr std::array<IndexBufferPatchPattern, 3> kIndexBufferPatchPatterns{
        IndexBufferPatchPattern{ kIndexBufferPatternCreate.data(), kIndexBufferPatternCreate.size(), 9, "create" },
        IndexBufferPatchPattern{ kIndexBufferPatternMember.data(), kIndexBufferPatternMember.size(), 6, "member" },
        IndexBufferPatchPattern{ kIndexBufferPatternGetter.data(), kIndexBufferPatternGetter.size(), 1, "getter" } };

    struct ExecutableImageRange
    {
        uint8_t* begin = nullptr;
        size_t size = 0;
    };

    struct IndexBufferPatchStats
    {
        size_t patched = 0;
        size_t alreadyAtTarget = 0;
        size_t externallyChanged = 0;
        size_t missing = 0;
        size_t ambiguous = 0;
        size_t writeFailed = 0;
    };

    struct WindowSearchContext
    {
        DWORD processId = 0;
        HWND hwnd = nullptr;
    };

    BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
        if (!ctx || !IsWindow(hwnd))
            return TRUE;

        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (windowProcessId != ctx->processId)
            return TRUE;

        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if ((style & WS_VISIBLE) == 0)
            return TRUE;

        ctx->hwnd = hwnd;
        return FALSE;
    }

    HWND FindCurrentProcessMainWindow()
    {
        WindowSearchContext ctx;
        ctx.processId = GetCurrentProcessId();
        EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hwnd;
    }

    bool EnsureDesiredWindowTitle(HWND hwnd)
    {
        if (!IsWindow(hwnd))
            return false;

        wchar_t currentTitle[256] = {};
        if (GetWindowTextW(hwnd, currentTitle, _countof(currentTitle)) > 0 &&
            lstrcmpW(currentTitle, kDesiredVrWindowTitle) == 0)
        {
            return true;
        }

        return SetWindowTextW(hwnd, kDesiredVrWindowTitle) != FALSE;
    }

    bool ForceWindowForeground(HWND hwnd)
    {
        if (!IsWindow(hwnd))
            return false;

        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        else
            ShowWindow(hwnd, SW_SHOW);

        HWND fgWindow = GetForegroundWindow();
        DWORD fgThreadId = fgWindow ? GetWindowThreadProcessId(fgWindow, nullptr) : 0;
        DWORD curThreadId = GetCurrentThreadId();
        const bool needAttach = (fgThreadId != 0 && fgThreadId != curThreadId);

        if (needAttach)
            AttachThreadInput(fgThreadId, curThreadId, TRUE);

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);

        if (needAttach)
            AttachThreadInput(fgThreadId, curThreadId, FALSE);

        return GetForegroundWindow() == hwnd;
    }

    DWORD WINAPI FocusGameWindowWorker(LPVOID)
    {
        constexpr int kMaxTries = 30;
        constexpr DWORD kRetryDelayMs = 500;
        constexpr int kSuccessStabilityTries = 3;

        int stableForegroundCount = 0;
        for (int i = 0; i < kMaxTries; ++i)
        {
            HWND window = FindCurrentProcessMainWindow();
            if (window && ForceWindowForeground(window))
            {
                ++stableForegroundCount;
                if (stableForegroundCount >= kSuccessStabilityTries)
                    return 0;
            }
            else
            {
                stableForegroundCount = 0;
            }

            Sleep(kRetryDelayMs);
        }

        return 0;
    }

    DWORD WINAPI MaintainWindowTitleWorker(LPVOID)
    {
        constexpr int kMaxRetries = 60;
        constexpr DWORD kRetryDelayMs = 1000;

        for (int i = 0; i < kMaxRetries; ++i)
        {
            HWND window = FindCurrentProcessMainWindow();
            if (window)
                EnsureDesiredWindowTitle(window);

            Sleep(kRetryDelayMs);
        }

        return 0;
    }

    bool IsNoHmdLaunchArgPresent()
    {
        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return false;

        bool noHmdEnabled = false;
        for (int i = 0; i < nArgs; ++i)
        {
            if (_wcsicmp(szArglist[i], L"-nohmd") == 0)
            {
                noHmdEnabled = true;
                break;
            }
        }

        LocalFree(szArglist);
        return noHmdEnabled;
    }

    struct LaunchArgumentState
    {
        bool hasWidth = false;
        bool hasHeight = false;
    };

    bool IsExactLaunchArg(const wchar_t* value, const wchar_t* expected)
    {
        return value && expected && _wcsicmp(value, expected) == 0;
    }

    bool IsLaunchArgName(const wchar_t* value, const wchar_t* expected)
    {
        if (!value || !expected)
            return false;

        if (_wcsicmp(value, expected) == 0)
            return true;

        const size_t expectedLen = wcslen(expected);
        return _wcsnicmp(value, expected, expectedLen) == 0 && value[expectedLen] == L'=';
    }

    bool IsBigFronLaunchArgPresent()
    {
        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return false;

        bool bigFronEnabled = false;
        for (int i = 0; i < nArgs; ++i)
        {
            if (IsLaunchArgName(szArglist[i], L"-bigfonts"))
            {
                bigFronEnabled = true;
                break;
            }
        }

        LocalFree(szArglist);
        return bigFronEnabled;
    }

    LaunchArgumentState ReadLaunchArgumentState()
    {
        LaunchArgumentState state;

        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return state;

        for (int i = 0; i < nArgs; ++i)
        {
            if (IsExactLaunchArg(szArglist[i], L"-w"))
                state.hasWidth = true;
            else if (IsExactLaunchArg(szArglist[i], L"-h"))
                state.hasHeight = true;
        }

        LocalFree(szArglist);
        return state;
    }

    std::filesystem::path GetGameRootPath()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return {};

        return std::filesystem::path(exePath).parent_path();
    }

    std::filesystem::path GetLaunchArgumentNoticePath()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return {};

        return gameRootPath / L"left4dead2" / L"cfg" / L"l4d2vr_launch_argument_notice.txt";
    }

    bool FileExistsNoThrow(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;

        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }

    bool DeleteFileNoThrow(const std::filesystem::path& path)
    {
        if (!FileExistsNoThrow(path))
            return false;

        // A readonly VPK can make std::filesystem::remove silently fail through error_code.
        // Clear attributes first so the cleanup path is deterministic.
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

        std::error_code ec;
        const bool removed = std::filesystem::remove(path, ec);
        return removed && !FileExistsNoThrow(path);
    }

    bool IsWritableMemoryRange(const void* address, size_t size, size_t& bytesAvailable)
    {
        bytesAvailable = 0;
        if (!address || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION memoryInfo{};
        if (VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo) ||
            memoryInfo.State != MEM_COMMIT ||
            (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const DWORD writableProtection =
            PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((memoryInfo.Protect & writableProtection) == 0)
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t regionStart = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
        const uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
        if (start < regionStart || start >= regionEnd)
            return false;

        bytesAvailable = static_cast<size_t>(regionEnd - start);
        return bytesAvailable >= size;
    }

    bool GetExecutableImageRanges(HMODULE module, std::vector<ExecutableImageRange>& ranges)
    {
        ranges.clear();
        if (!module)
            return false;

        auto* base = reinterpret_cast<uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0)
            return false;

        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;

            const size_t sectionOffset = section->VirtualAddress;
            if (sectionOffset >= imageSize)
                continue;

            size_t sectionSize = section->Misc.VirtualSize;
            if (sectionSize == 0)
                sectionSize = section->SizeOfRawData;
            sectionSize = (std::min)(sectionSize, imageSize - sectionOffset);
            if (sectionSize == 0)
                continue;

            ranges.push_back({ base + sectionOffset, sectionSize });
        }

        return !ranges.empty();
    }

    bool MatchesIndexBufferPattern(
        const uint8_t* candidate,
        const IndexBufferPatchPattern& pattern)
    {
        for (size_t i = 0; i < pattern.length; ++i)
        {
            if (i >= pattern.immediateOffset &&
                i < pattern.immediateOffset + sizeof(uint32_t))
            {
                continue;
            }
            if (candidate[i] != pattern.bytes[i])
                return false;
        }
        return true;
    }

    std::vector<uint8_t*> FindIndexBufferPatternMatches(
        HMODULE module,
        const IndexBufferPatchPattern& pattern)
    {
        std::vector<ExecutableImageRange> ranges;
        if (!GetExecutableImageRanges(module, ranges) ||
            !pattern.bytes ||
            pattern.length < pattern.immediateOffset + sizeof(uint32_t))
        {
            return {};
        }

        std::vector<uint8_t*> matches;
        for (const ExecutableImageRange& range : ranges)
        {
            if (!range.begin || range.size < pattern.length)
                continue;

            const size_t lastOffset = range.size - pattern.length;
            for (size_t offset = 0; offset <= lastOffset; ++offset)
            {
                uint8_t* candidate = range.begin + offset;
                if (!MatchesIndexBufferPattern(candidate, pattern))
                    continue;

                uint32_t currentValue = 0;
                std::memcpy(
                    &currentValue,
                    candidate + pattern.immediateOffset,
                    sizeof(currentValue));
                if (currentValue == kOriginalIndexBufferSize ||
                    currentValue == kForcedIndexBufferSize)
                {
                    matches.push_back(candidate);
                }
            }
        }
        return matches;
    }

    bool WriteIndexBufferImmediate(uint8_t* address)
    {
        if (!address)
            return false;

        DWORD oldProtection = 0;
        if (!VirtualProtect(address, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        uint32_t currentValue = 0;
        std::memcpy(&currentValue, address, sizeof(currentValue));
        if (currentValue == kOriginalIndexBufferSize)
            std::memcpy(address, &kForcedIndexBufferSize, sizeof(kForcedIndexBufferSize));

        FlushInstructionCache(GetCurrentProcess(), address, sizeof(uint32_t));
        DWORD ignoredProtection = 0;
        VirtualProtect(address, sizeof(uint32_t), oldProtection, &ignoredProtection);

        uint32_t verifiedValue = 0;
        std::memcpy(&verifiedValue, address, sizeof(verifiedValue));
        return verifiedValue == kForcedIndexBufferSize;
    }

    IndexBufferPatchStats ApplyForcedIndexBufferSizeToModule(
        HMODULE module,
        const wchar_t* moduleName)
    {
        IndexBufferPatchStats stats;
        for (const IndexBufferPatchPattern& pattern : kIndexBufferPatchPatterns)
        {
            const std::vector<uint8_t*> matches = FindIndexBufferPatternMatches(module, pattern);
            if (matches.empty())
            {
                ++stats.missing;
                Game::logMsg(
                    "[VR][IndexBuffer] module=%ls pattern=%s missing",
                    moduleName,
                    pattern.name);
                continue;
            }
            if (matches.size() != 1)
            {
                ++stats.ambiguous;
                Game::logMsg(
                    "[VR][IndexBuffer] module=%ls pattern=%s ambiguous matches=%zu",
                    moduleName,
                    pattern.name,
                    matches.size());
                continue;
            }

            uint8_t* immediate = matches.front() + pattern.immediateOffset;
            uint32_t currentValue = 0;
            std::memcpy(&currentValue, immediate, sizeof(currentValue));
            if (currentValue == kForcedIndexBufferSize)
            {
                ++stats.alreadyAtTarget;
                continue;
            }
            if (currentValue != kOriginalIndexBufferSize)
            {
                // Another active patch owns this value. Do not identify it by a
                // DLL filename and do not overwrite its explicit configuration.
                ++stats.externallyChanged;
                Game::logMsg(
                    "[VR][IndexBuffer] module=%ls pattern=%s external value=%u preserved",
                    moduleName,
                    pattern.name,
                    currentValue);
                continue;
            }

            if (WriteIndexBufferImmediate(immediate))
                ++stats.patched;
            else
                ++stats.writeFailed;
        }

        Game::logMsg(
            "[VR][IndexBuffer] module=%ls target=%u patched=%zu already=%zu external=%zu missing=%zu ambiguous=%zu failed=%zu",
            moduleName,
            kForcedIndexBufferSize,
            stats.patched,
            stats.alreadyAtTarget,
            stats.externallyChanged,
            stats.missing,
            stats.ambiguous,
            stats.writeFailed);
        return stats;
    }

    bool ApplyForcedIndexBufferSizeToLoadedShaderApi()
    {
        constexpr std::array<const wchar_t*, 2> kShaderApiModules{
            L"shaderapidx9.dll",
            L"shaderapivk.dll" };

        bool foundLoadedModule = false;
        for (const wchar_t* moduleName : kShaderApiModules)
        {
            HMODULE module = GetModuleHandleW(moduleName);
            if (!module)
                continue;

            foundLoadedModule = true;
            ApplyForcedIndexBufferSizeToModule(module, moduleName);
        }
        return foundLoadedModule;
    }

    DWORD WINAPI WaitForShaderApiAndApplyIndexBufferSize(LPVOID)
    {
        constexpr DWORD kRetryDelayMs = 100;
        constexpr int kMaxAttempts = 600;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            if (ApplyForcedIndexBufferSizeToLoadedShaderApi())
                return 0;
            Sleep(kRetryDelayMs);
        }

        Game::logMsg("[VR][IndexBuffer] shaderapi module was not loaded within 60 seconds");
        return 0;
    }

    void StartForcedIndexBufferPolicy()
    {
        if (ApplyForcedIndexBufferSizeToLoadedShaderApi())
            return;

        HANDLE worker = CreateThread(
            nullptr,
            0,
            WaitForShaderApiAndApplyIndexBufferSize,
            nullptr,
            0,
            nullptr);
        if (worker)
            CloseHandle(worker);
        else
            Game::logMsg("[VR][IndexBuffer] failed to start shaderapi wait worker");
    }

    bool OverrideSourceHeapSizeCommandLine()
    {
        using CommandLineFactory = SourceCommandLine* (__cdecl*)();

        HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
        if (!tier0)
            tier0 = GetModuleHandleW(L"tier0_s.dll");
        if (!tier0)
            return false;

        auto commandLineFactory = reinterpret_cast<CommandLineFactory>(
            GetProcAddress(tier0, "CommandLine_Tier0"));
        if (!commandLineFactory)
            return false;

        SourceCommandLine* commandLine = commandLineFactory();
        if (!commandLine)
            return false;

        // 2097152 KiB cannot pass through the engine's signed KB-to-byte path.
        // Lock the parsed value to the largest non-overflowing KiB value; the
        // datacache field is corrected to the exact unsigned 2 GiB value below.
        commandLine->RemoveParm("-heapsize");
        commandLine->AppendParm("-heapsize", kForcedDataCacheHeapSizeKiB);
        return true;
    }

    SourceDataCache* FindSourceDataCache()
    {
        using CreateInterfaceFn = void* (__cdecl*)(const char* name, int* returnCode);

        HMODULE dataCacheModule = GetModuleHandleW(L"datacache.dll");
        if (!dataCacheModule)
            return nullptr;

        auto createInterface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(dataCacheModule, "CreateInterface"));
        if (!createInterface)
            return nullptr;

        int returnCode = 0;
        return static_cast<SourceDataCache*>(createInterface("VDataCache003", &returnCode));
    }

    bool ApplyExactTwoGiBDataCacheLimit()
    {
        SourceDataCache* dataCache = FindSourceDataCache();
        if (!dataCache)
            return false;

        g_DataCacheForceState.interfaceFound = true;

        SourceDataCacheStatus status{};
        SourceDataCacheLimits limits{};
        dataCache->GetStatus(&status, &limits);
        g_DataCacheForceState.observedLimitBytes = limits.maxBytes;
        if (limits.maxBytes == kForcedDataCacheSizeBytes)
        {
            g_DataCacheForceState.alreadyAtTarget = true;
            g_DataCacheForceState.exactLimitApplied = true;
            return true;
        }

        constexpr uint32_t kSafeLimitBytes = kForcedDataCacheSizeBytes - 1024u;
        static_assert(kSafeLimitBytes <= static_cast<uint32_t>(std::numeric_limits<int>::max()),
            "Safe datacache limit must fit the signed SetSize ABI.");

        dataCache->SetSize(static_cast<int>(kSafeLimitBytes));
        status = {};
        limits = {};
        dataCache->GetStatus(&status, &limits);
        g_DataCacheForceState.safeLimitApplied = (limits.maxBytes == kSafeLimitBytes);
        g_DataCacheForceState.observedLimitBytes = limits.maxBytes;
        if (limits.maxBytes == 0)
            return false;

        // SetSize takes a signed int and therefore cannot carry 0x80000000. Some
        // engine states also reject a late size increase and leave the old limit
        // in place. In either case, find the single writable backing field whose
        // value GetStatus just returned instead of hard-coding a member offset.
        // An unknown build with zero or multiple candidates remains unchanged.
        size_t bytesAvailable = 0;
        constexpr size_t kDataCacheObjectScanBytes = 128;
        if (!IsWritableMemoryRange(dataCache, sizeof(uint32_t), bytesAvailable))
            return false;

        const size_t scanBytes = (std::min)(bytesAvailable, kDataCacheObjectScanBytes);
        auto* objectBytes = reinterpret_cast<uint8_t*>(dataCache);
        volatile LONG* exactLimitField = nullptr;
        size_t candidateCount = 0;
        for (size_t offset = 0; offset + sizeof(uint32_t) <= scanBytes; offset += alignof(uint32_t))
        {
            uint32_t value = 0;
            std::memcpy(&value, objectBytes + offset, sizeof(value));
            if (value == limits.maxBytes)
            {
                ++candidateCount;
                exactLimitField = reinterpret_cast<volatile LONG*>(objectBytes + offset);
            }
        }

        g_DataCacheForceState.exactLimitCandidateCount = candidateCount;
        if (candidateCount != 1 || !exactLimitField)
            return false;

        InterlockedExchange(exactLimitField, static_cast<LONG>(kForcedDataCacheSizeBytes));

        status = {};
        limits = {};
        dataCache->GetStatus(&status, &limits);
        g_DataCacheForceState.observedLimitBytes = limits.maxBytes;
        g_DataCacheForceState.exactLimitApplied = (limits.maxBytes == kForcedDataCacheSizeBytes);
        return g_DataCacheForceState.exactLimitApplied;
    }

    void FinalizeForcedDataCachePolicy()
    {
        // Judge the effective cache capacity rather than the presence of any
        // Left4Neko artifact. A working memory plugin has already set 2 GiB and
        // becomes a no-op here; stale files/modules/commands cannot suppress it.
        g_DataCacheForceState.enabled = true;
        g_DataCacheForceState.commandLineOverridden = OverrideSourceHeapSizeCommandLine();
        const bool exactLimitApplied = ApplyExactTwoGiBDataCacheLimit();
        Game::logMsg(
            "[VR][DataCache] forced=%d commandLine=%d interface=%d already=%d safe=%d exact=%d limit=%u bytes candidates=%zu",
            g_DataCacheForceState.enabled ? 1 : 0,
            g_DataCacheForceState.commandLineOverridden ? 1 : 0,
            g_DataCacheForceState.interfaceFound ? 1 : 0,
            g_DataCacheForceState.alreadyAtTarget ? 1 : 0,
            g_DataCacheForceState.safeLimitApplied ? 1 : 0,
            exactLimitApplied ? 1 : 0,
            g_DataCacheForceState.observedLimitBytes,
            g_DataCacheForceState.exactLimitCandidateCount);
    }

    bool ShouldUseChineseLaunchArgumentPrompt()
    {
        const LANGID uiLang = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(uiLang) == LANG_CHINESE)
            return true;

        wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(localeName, _countof(localeName)) > 0)
            return _wcsnicmp(localeName, L"zh", 2) == 0;

        return false;
    }

    bool LaunchArgumentNoticeAlreadyShown(const std::filesystem::path& noticePath)
    {
        if (!FileExistsNoThrow(noticePath))
            return false;

        std::wifstream input(noticePath);
        if (!input.is_open())
            return true;

        std::wstring line;
        while (std::getline(input, line))
        {
            if (line.find(L"LaunchArgumentNoticeVersion=2") != std::wstring::npos)
                return true;
        }

        return false;
    }

    void MarkLaunchArgumentNoticeShown(const std::filesystem::path& noticePath)
    {
        if (noticePath.empty())
            return;

        std::error_code ec;
        std::filesystem::create_directories(noticePath.parent_path(), ec);

        std::wofstream output(noticePath, std::ios::trunc);
        if (!output.is_open())
            return;

        output << L"LaunchArgumentNoticeShown=1\n";
        output << L"LaunchArgumentNoticeVersion=2\n";
        output << L"CheckedArgs=-w,-h\n";
    }

    void ShowLaunchArgumentNoticeIfNeeded()
    {
        const std::filesystem::path noticePath = GetLaunchArgumentNoticePath();
        if (LaunchArgumentNoticeAlreadyShown(noticePath))
            return;

        const LaunchArgumentState state = ReadLaunchArgumentState();
        const bool hasResolutionLaunchArgs = state.hasWidth || state.hasHeight;
        if (!hasResolutionLaunchArgs)
            return;

        const bool useChinese = ShouldUseChineseLaunchArgumentPrompt();
        const wchar_t* title = useChinese ? L"L4D2VR \u542f\u52a8\u53c2\u6570\u63d0\u793a" : L"L4D2VR Launch Options";

        std::wstring message;
        if (useChinese)
        {
            message = L"\u68c0\u6d4b\u5230\u5f53\u524d\u542f\u52a8\u53c2\u6570\u53ef\u80fd\u4e0d\u9002\u5408 L4D2VR\uff1a\n\n";
            if (hasResolutionLaunchArgs)
                message += L"- \u68c0\u6d4b\u5230 -w \u6216 -h\u3002\u5efa\u8bae\u4ece Steam \u542f\u52a8\u53c2\u6570\u4e2d\u5220\u9664\u5b83\u4eec\uff0c\u907f\u514d\u8986\u76d6\u63d2\u4ef6\u7ba1\u7406\u7684\u7a97\u53e3\u5206\u8fa8\u7387\u3002\n";
            message += L"\n\u6b64\u63d0\u793a\u53ea\u663e\u793a\u4e00\u6b21\u3002";
        }
        else
        {
            message = L"The current launch options may not be suitable for L4D2VR:\n\n";
            if (hasResolutionLaunchArgs)
                message += L"- -w or -h was detected. Remove them from the Steam launch options to avoid overriding the window resolution managed by the plugin.\n";
            message += L"\nThis notice will only be shown once.";
        }

        HWND owner = FindCurrentProcessMainWindow();
        MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        MarkLaunchArgumentNoticeShown(noticePath);
    }

    bool ReplaceConfigValueInLine(std::wstring& line, const wchar_t* key, const wchar_t* expectedValue)
    {
        if (line.find(key) == std::wstring::npos)
            return false;

        const size_t firstQuote = line.find(L'"');
        if (firstQuote == std::wstring::npos)
            return false;

        const size_t secondQuote = line.find(L'"', firstQuote + 1);
        if (secondQuote == std::wstring::npos)
            return false;

        const size_t valueStartQuote = line.find(L'"', secondQuote + 1);
        if (valueStartQuote == std::wstring::npos)
            return false;

        const size_t valueEndQuote = line.find(L'"', valueStartQuote + 1);
        if (valueEndQuote == std::wstring::npos)
            return false;

        const std::wstring currentValue = line.substr(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1);
        if (currentValue == expectedValue)
            return false;

        line.replace(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1, expectedValue);
        return true;
    }

    bool ExtractConfigValueFromLine(const std::wstring& line, const wchar_t* key, std::wstring& outValue)
    {
        if (line.find(key) == std::wstring::npos)
            return false;

        const size_t firstQuote = line.find(L'"');
        if (firstQuote == std::wstring::npos)
            return false;

        const size_t secondQuote = line.find(L'"', firstQuote + 1);
        if (secondQuote == std::wstring::npos)
            return false;

        const size_t valueStartQuote = line.find(L'"', secondQuote + 1);
        if (valueStartQuote == std::wstring::npos)
            return false;

        const size_t valueEndQuote = line.find(L'"', valueStartQuote + 1);
        if (valueEndQuote == std::wstring::npos)
            return false;

        outValue = line.substr(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1);
        return true;
    }

    std::wstring TrimWhitespace(const std::wstring& value)
    {
        size_t start = 0;
        while (start < value.size() && std::iswspace(value[start]))
            ++start;

        size_t end = value.size();
        while (end > start && std::iswspace(value[end - 1]))
            --end;

        return value.substr(start, end - start);
    }

    std::string TrimAsciiWhitespace(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            ++start;

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
            --end;

        return value.substr(start, end - start);
    }

    struct VideoCfgDesiredSetting
    {
        const wchar_t* key;
        const wchar_t* value;
    };

    constexpr char kVrRecommendedVideoSettingsConfigKey[] = "VrRecommendedVideoSettingsEnabled";

    constexpr VideoCfgDesiredSetting kDesiredVideoCfgSettings[] =
    {
        { L"\"setting.cpu_level\"", L"1" },
        { L"\"setting.gpu_level\"", L"2" },
        { L"\"setting.mat_antialias\"", L"1" },
        { L"\"setting.mat_aaquality\"", L"0" },
        { L"\"setting.mat_vsync\"", L"0" },
        { L"\"setting.mat_triplebuffered\"", L"0" },
        { L"\"setting.mat_grain_scale_override\"", L"0" },
        { L"\"setting.mat_monitorgamma\"", L"2.200000" },
        { L"\"setting.gpu_mem_level\"", L"1" },
        { L"\"setting.mem_level\"", L"0" },
        { L"\"setting.mat_queue_mode\"", L"0" },
        { L"\"setting.aspectratiomode\"", L"1" },
        { L"\"setting.fullscreen\"", L"0" },
        { L"\"setting.nowindowborder\"", L"1" },
    };

    bool ConfigValueIsEnabled(std::string value)
    {
        value = TrimAsciiWhitespace(value);
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    bool IsVrRecommendedVideoSettingsEnabledInConfig()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return false;

        const std::filesystem::path configPath = gameRootPath / L"VR" / L"config.txt";
        std::ifstream input(configPath);
        if (!input.is_open())
            return false;

        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            line = TrimAsciiWhitespace(line);
            if (line.empty() || line.rfind("//", 0) == 0 || line[0] == '#' || line[0] == ';')
                continue;

            size_t cut = line.find_first_of("#;");
            const size_t slashComment = line.find("//");
            if (slashComment != std::string::npos)
                cut = (cut == std::string::npos) ? slashComment : (std::min)(cut, slashComment);
            if (cut != std::string::npos)
                line.erase(cut);

            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            const std::string key = TrimAsciiWhitespace(line.substr(0, equals));
            if (_stricmp(key.c_str(), kVrRecommendedVideoSettingsConfigKey) != 0)
                continue;

            return ConfigValueIsEnabled(line.substr(equals + 1));
        }

        return false;
    }

    void InsertVideoCfgLineBeforeClosingBrace(std::vector<std::wstring>& lines, const wchar_t* key, const wchar_t* value)
    {
        std::wstring text = L"\t";
        text += key;
        text += L"\t\t\"";
        text += value;
        text += L"\"";

        for (auto it = lines.begin(); it != lines.end(); ++it)
        {
            if (TrimWhitespace(*it) == L"}")
            {
                lines.insert(it, text);
                return;
            }
        }

        lines.push_back(text);
    }

    bool EnsureVideoCfgSettings()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return false;

        const std::filesystem::path videoCfgPath = gameRootPath / L"left4dead2" / L"cfg" / L"video.txt";
        if (!std::filesystem::exists(videoCfgPath))
            return false;

        std::wifstream input(videoCfgPath);
        if (!input.is_open())
            return false;

        std::vector<std::wstring> lines;
        lines.reserve(64);

        bool found[_countof(kDesiredVideoCfgSettings)] = {};
        std::wstring line;
        bool changed = false;
        while (std::getline(input, line))
        {
            for (size_t i = 0; i < _countof(kDesiredVideoCfgSettings); ++i)
            {
                const VideoCfgDesiredSetting& desired = kDesiredVideoCfgSettings[i];
                if (line.find(desired.key) != std::wstring::npos)
                {
                    found[i] = true;
                    changed |= ReplaceConfigValueInLine(line, desired.key, desired.value);
                    break;
                }
            }

            lines.push_back(line);
        }
        input.close();

        for (size_t i = 0; i < _countof(kDesiredVideoCfgSettings); ++i)
        {
            if (!found[i])
            {
                InsertVideoCfgLineBeforeClosingBrace(
                    lines,
                    kDesiredVideoCfgSettings[i].key,
                    kDesiredVideoCfgSettings[i].value);
                changed = true;
            }
        }

        if (!changed)
            return true;

        std::wofstream output(videoCfgPath, std::ios::trunc);
        if (!output.is_open())
            return false;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << L'\n';
        }

        return true;
    }

    void ApplyRuntimeVideoSettings()
    {
        if (!g_Game || !g_Game->m_Initialized)
            return;

        // The engine can rewrite video.txt after our early startup pass, especially after
        // display-mode changes. Apply the runtime ConVars as well so the current session
        // stays clamped while the worker keeps the persisted file corrected for next launch.
        g_Game->SetConVarInt("mat_antialias", 1);
        g_Game->SetConVarInt("mat_aaquality", 0);
        g_Game->SetConVarInt("mat_vsync", 0);
        g_Game->SetConVarInt("mat_triplebuffered", 0);
        g_Game->SetConVarFloat("mat_grain_scale_override", 0.0f);
        g_Game->SetConVarFloat("mat_monitorgamma", 2.2f);
    }

    std::string StripQuotes(std::string value)
    {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            return value.substr(1, value.size() - 2);
        return value;
    }

    bool AutoexecHasCrosshairOne(const std::filesystem::path& autoexecPath)
    {
        std::ifstream input(autoexecPath);
        if (!input.is_open())
            return false;

        std::string line;
        while (std::getline(input, line))
        {
            const size_t commentPos = line.find("//");
            if (commentPos != std::string::npos)
                line = line.substr(0, commentPos);

            std::istringstream iss(line);
            std::string cmd;
            std::string value;
            if (!(iss >> cmd >> value))
                continue;

            cmd = StripQuotes(cmd);
            value = StripQuotes(value);
            if (_stricmp(cmd.c_str(), "crosshair") == 0 && value == "1")
                return true;
        }

        return false;
    }

    void EnsureNoHmdAutoexecCrosshair()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const std::filesystem::path autoexecPath = gameRootPath / L"left4dead2" / L"cfg" / L"autoexec.cfg";

        if (!std::filesystem::exists(autoexecPath))
        {
            std::error_code ec;
            std::filesystem::create_directories(autoexecPath.parent_path(), ec);

            std::ofstream createFile(autoexecPath, std::ios::trunc);
            if (createFile.is_open())
                createFile << "crosshair 1\n";
            return;
        }

        if (AutoexecHasCrosshairOne(autoexecPath))
            return;

        std::ofstream appendFile(autoexecPath, std::ios::app);
        if (!appendFile.is_open())
            return;

        appendFile << "\ncrosshair 1\n";
    }

    bool TryGetCurrentDisplayResolution(int& width, int& height)
    {
        width = 0;
        height = 0;

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        {
            width = static_cast<int>(dm.dmPelsWidth);
            height = static_cast<int>(dm.dmPelsHeight);
        }
        else
        {
            width = GetSystemMetrics(SM_CXSCREEN);
            height = GetSystemMetrics(SM_CYSCREEN);
        }

        return width > 0 && height > 0;
    }

    bool TryExtractFirstQuotedString(const std::wstring& line, std::wstring& outValue)
    {
        const size_t start = line.find(L'"');
        if (start == std::wstring::npos)
            return false;

        const size_t end = line.find(L'"', start + 1);
        if (end == std::wstring::npos)
            return false;

        outValue = line.substr(start + 1, end - start - 1);
        return true;
    }

    bool TryExtractSecondQuotedString(const std::wstring& line, std::wstring& outValue)
    {
        size_t pos = line.find(L'"');
        if (pos == std::wstring::npos)
            return false;
        pos = line.find(L'"', pos + 1);
        if (pos == std::wstring::npos)
            return false;
        pos = line.find(L'"', pos + 1);
        if (pos == std::wstring::npos)
            return false;

        const size_t end = line.find(L'"', pos + 1);
        if (end == std::wstring::npos)
            return false;

        outValue = line.substr(pos + 1, end - pos - 1);
        return true;
    }

    bool IsUiFontFixAddonEntryLine(const std::wstring& line)
    {
        std::wstring firstQuoted;
        if (!TryExtractFirstQuotedString(line, firstQuoted))
            return false;

        for (wchar_t& ch : firstQuoted)
        {
            if (ch == L'/')
                ch = L'\\';
        }

        const wchar_t* suffix = kUiFontFixFileName;
        const size_t suffixLen = wcslen(suffix);
        if (firstQuoted.size() < suffixLen)
            return false;

        return _wcsicmp(firstQuoted.c_str() + firstQuoted.size() - suffixLen, suffix) == 0;
    }

    bool IsAddonListEntryLine(const std::wstring& line)
    {
        std::wstring firstQuoted;
        std::wstring secondQuoted;
        return TryExtractFirstQuotedString(line, firstQuoted) && TryExtractSecondQuotedString(line, secondQuoted);
    }

    void WriteAddonListLines(const std::filesystem::path& addonListPath, const std::vector<std::wstring>& lines)
    {
        std::error_code ec;
        std::filesystem::create_directories(addonListPath.parent_path(), ec);

        std::wofstream output(addonListPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << L'\n';
        }
    }

    void EnsureUiFontFixAddonListFirst(const std::filesystem::path& addonListPath)
    {
        constexpr wchar_t kUiFontFixEntry[] = L"\t\"UI Font Fix.vpk\"\t\t\"1\"";

        if (!FileExistsNoThrow(addonListPath))
        {
            WriteAddonListLines(addonListPath, {
                L"\"AddonList\"",
                L"{",
                kUiFontFixEntry,
                L"}"
                });
            return;
        }

        std::wifstream input(addonListPath);
        if (!input.is_open())
            return;

        std::vector<std::wstring> lines;
        lines.reserve(128);

        std::wstring line;
        while (std::getline(input, line))
            lines.push_back(line);
        input.close();

        int openBraceIndex = -1;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (TrimWhitespace(lines[i]) == L"{")
            {
                openBraceIndex = static_cast<int>(i);
                break;
            }
        }

        if (openBraceIndex < 0)
        {
            WriteAddonListLines(addonListPath, {
                L"\"AddonList\"",
                L"{",
                kUiFontFixEntry,
                L"}"
                });
            return;
        }

        int firstEntryIndex = -1;
        int existingUiFontFixIndex = -1;
        std::vector<std::wstring> filteredLines;
        filteredLines.reserve(lines.size() + 1);

        for (size_t i = 0; i < lines.size(); ++i)
        {
            const bool isAddonEntry = static_cast<int>(i) > openBraceIndex && IsAddonListEntryLine(lines[i]);
            if (isAddonEntry && firstEntryIndex < 0)
                firstEntryIndex = static_cast<int>(i);

            if (IsUiFontFixAddonEntryLine(lines[i]))
            {
                if (existingUiFontFixIndex < 0)
                    existingUiFontFixIndex = static_cast<int>(i);
                continue;
            }

            filteredLines.push_back(lines[i]);
        }

        const bool alreadyFirst = (existingUiFontFixIndex >= 0) &&
            (firstEntryIndex < 0 || existingUiFontFixIndex == firstEntryIndex);
        if (alreadyFirst)
        {
            std::wstring firstValue;
            const bool enabled = TryExtractSecondQuotedString(lines[existingUiFontFixIndex], firstValue) && firstValue == L"1";
            if (enabled)
                return;
        }

        int filteredOpenBraceIndex = -1;
        for (size_t i = 0; i < filteredLines.size(); ++i)
        {
            if (TrimWhitespace(filteredLines[i]) == L"{")
            {
                filteredOpenBraceIndex = static_cast<int>(i);
                break;
            }
        }

        if (filteredOpenBraceIndex < 0)
        {
            WriteAddonListLines(addonListPath, {
                L"\"AddonList\"",
                L"{",
                kUiFontFixEntry,
                L"}"
                });
            return;
        }

        filteredLines.insert(filteredLines.begin() + filteredOpenBraceIndex + 1, kUiFontFixEntry);
        WriteAddonListLines(addonListPath, filteredLines);
    }

    void RemoveUiFontFixAddonListEntry(const std::filesystem::path& addonListPath)
    {
        if (!FileExistsNoThrow(addonListPath))
            return;

        std::wifstream input(addonListPath);
        if (!input.is_open())
            return;

        std::vector<std::wstring> lines;
        lines.reserve(128);

        std::wstring line;
        bool changed = false;
        while (std::getline(input, line))
        {
            if (IsUiFontFixAddonEntryLine(line))
            {
                changed = true;
                continue;
            }

            lines.push_back(line);
        }
        input.close();

        if (!changed)
            return;

        WriteAddonListLines(addonListPath, lines);
    }

    bool IsMatSetVideoModeLine(const std::string& line)
    {
        std::string activePart = line;
        const size_t commentPos = activePart.find("//");
        if (commentPos != std::string::npos)
            activePart = activePart.substr(0, commentPos);

        activePart = TrimAsciiWhitespace(activePart);
        if (activePart.empty())
            return false;

        std::istringstream iss(activePart);
        std::string cmd;
        if (!(iss >> cmd))
            return false;

        cmd = StripQuotes(cmd);
        return _stricmp(cmd.c_str(), "mat_setvideomode") == 0;
    }

    void EnsureAutoexecMatSetVideoMode(const std::filesystem::path& autoexecPath, int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;

        const std::string line0 = "mat_setvideomode " + std::to_string(width) + " " + std::to_string(height) + " 0";
        const std::string line1 = "mat_setvideomode " + std::to_string(width) + " " + std::to_string(height) + " 1";

        std::vector<std::string> lines;
        bool foundLine0 = false;
        bool foundLine1 = false;
        int matSetVideoModeLineCount = 0;

        if (FileExistsNoThrow(autoexecPath))
        {
            std::ifstream input(autoexecPath);
            if (input.is_open())
            {
                std::string line;
                while (std::getline(input, line))
                {
                    const std::string trimmed = TrimAsciiWhitespace(line);
                    if (IsMatSetVideoModeLine(line))
                    {
                        ++matSetVideoModeLineCount;
                        if (_stricmp(trimmed.c_str(), line0.c_str()) == 0)
                            foundLine0 = true;
                        else if (_stricmp(trimmed.c_str(), line1.c_str()) == 0)
                            foundLine1 = true;
                        continue;
                    }

                    lines.push_back(line);
                }
            }
        }

        if (foundLine0 && foundLine1 && matSetVideoModeLineCount == 2)
            return;

        if (!lines.empty() && !TrimAsciiWhitespace(lines.back()).empty())
            lines.push_back(std::string());

        lines.push_back(line0);
        lines.push_back(line1);

        std::error_code ec;
        std::filesystem::create_directories(autoexecPath.parent_path(), ec);

        std::ofstream output(autoexecPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << '\n';
        }
    }

    void RemoveAutoexecMatSetVideoMode(const std::filesystem::path& autoexecPath)
    {
        if (!FileExistsNoThrow(autoexecPath))
            return;

        std::ifstream input(autoexecPath);
        if (!input.is_open())
            return;

        std::vector<std::string> lines;
        lines.reserve(128);

        std::string line;
        bool changed = false;
        while (std::getline(input, line))
        {
            if (IsMatSetVideoModeLine(line))
            {
                changed = true;
                continue;
            }

            lines.push_back(line);
        }
        input.close();

        if (!changed)
            return;

        std::ofstream output(autoexecPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << '\n';
        }
    }

    void RemoveUiFontFixVpkAndConfig(const std::filesystem::path& gameRootPath)
    {
        if (gameRootPath.empty())
            return;

        const std::filesystem::path left4dead2Path = gameRootPath / L"left4dead2";
        const std::filesystem::path targetVpkPath = left4dead2Path / L"addons" / kUiFontFixFileName;

        DeleteFileNoThrow(targetVpkPath);
        RemoveUiFontFixAddonListEntry(left4dead2Path / L"addonlist.txt");
        RemoveAutoexecMatSetVideoMode(left4dead2Path / L"cfg" / L"autoexec.cfg");
    }


    struct Sha256State
    {
        std::array<uint8_t, 64> data{};
        uint32_t dataLen = 0;
        uint64_t bitLen = 0;
        std::array<uint32_t, 8> state{};
    };

    constexpr std::array<uint32_t, 64> kSha256RoundConstants = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    constexpr uint32_t Sha256RotateRight(uint32_t value, uint32_t bits)
    {
        return (value >> bits) | (value << (32u - bits));
    }

    void Sha256Transform(Sha256State& ctx, const uint8_t data[64])
    {
        uint32_t m[64] = {};
        for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4)
        {
            m[i] = (static_cast<uint32_t>(data[j]) << 24) |
                (static_cast<uint32_t>(data[j + 1]) << 16) |
                (static_cast<uint32_t>(data[j + 2]) << 8) |
                (static_cast<uint32_t>(data[j + 3]));
        }

        for (uint32_t i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Sha256RotateRight(m[i - 15], 7) ^ Sha256RotateRight(m[i - 15], 18) ^ (m[i - 15] >> 3);
            const uint32_t s1 = Sha256RotateRight(m[i - 2], 17) ^ Sha256RotateRight(m[i - 2], 19) ^ (m[i - 2] >> 10);
            m[i] = m[i - 16] + s0 + m[i - 7] + s1;
        }

        uint32_t a = ctx.state[0];
        uint32_t b = ctx.state[1];
        uint32_t c = ctx.state[2];
        uint32_t d = ctx.state[3];
        uint32_t e = ctx.state[4];
        uint32_t f = ctx.state[5];
        uint32_t g = ctx.state[6];
        uint32_t h = ctx.state[7];

        for (uint32_t i = 0; i < 64; ++i)
        {
            const uint32_t s1 = Sha256RotateRight(e, 6) ^ Sha256RotateRight(e, 11) ^ Sha256RotateRight(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[i] + m[i];
            const uint32_t s0 = Sha256RotateRight(a, 2) ^ Sha256RotateRight(a, 13) ^ Sha256RotateRight(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        ctx.state[0] += a;
        ctx.state[1] += b;
        ctx.state[2] += c;
        ctx.state[3] += d;
        ctx.state[4] += e;
        ctx.state[5] += f;
        ctx.state[6] += g;
        ctx.state[7] += h;
    }

    void Sha256Init(Sha256State& ctx)
    {
        ctx.data.fill(0);
        ctx.dataLen = 0;
        ctx.bitLen = 0;
        ctx.state = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    }

    void Sha256Update(Sha256State& ctx, const uint8_t* data, size_t len)
    {
        for (size_t i = 0; i < len; ++i)
        {
            ctx.data[ctx.dataLen++] = data[i];
            if (ctx.dataLen == 64)
            {
                Sha256Transform(ctx, ctx.data.data());
                ctx.bitLen += 512;
                ctx.dataLen = 0;
            }
        }
    }

    std::string Sha256FinalHex(Sha256State& ctx)
    {
        uint32_t i = ctx.dataLen;

        if (ctx.dataLen < 56)
        {
            ctx.data[i++] = 0x80;
            while (i < 56)
                ctx.data[i++] = 0x00;
        }
        else
        {
            ctx.data[i++] = 0x80;
            while (i < 64)
                ctx.data[i++] = 0x00;
            Sha256Transform(ctx, ctx.data.data());
            ctx.data.fill(0);
        }

        ctx.bitLen += static_cast<uint64_t>(ctx.dataLen) * 8ull;
        ctx.data[63] = static_cast<uint8_t>(ctx.bitLen);
        ctx.data[62] = static_cast<uint8_t>(ctx.bitLen >> 8);
        ctx.data[61] = static_cast<uint8_t>(ctx.bitLen >> 16);
        ctx.data[60] = static_cast<uint8_t>(ctx.bitLen >> 24);
        ctx.data[59] = static_cast<uint8_t>(ctx.bitLen >> 32);
        ctx.data[58] = static_cast<uint8_t>(ctx.bitLen >> 40);
        ctx.data[57] = static_cast<uint8_t>(ctx.bitLen >> 48);
        ctx.data[56] = static_cast<uint8_t>(ctx.bitLen >> 56);
        Sha256Transform(ctx, ctx.data.data());

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t value : ctx.state)
        {
            for (int shift = 28; shift >= 0; shift -= 4)
                out.push_back(kHex[(value >> shift) & 0x0f]);
        }
        return out;
    }

    bool Sha256FileHex(const std::filesystem::path& path, std::string& outHex)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
            return false;

        Sha256State ctx;
        Sha256Init(ctx);

        std::array<char, 64 * 1024> buffer{};
        while (input.good())
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = input.gcount();
            if (got > 0)
                Sha256Update(ctx, reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(got));
        }

        if (input.bad())
            return false;

        outHex = Sha256FinalHex(ctx);
        return true;
    }

    std::wstring QuoteWinArg(const std::wstring& arg)
    {
        std::wstring result;
        result.push_back(L'"');
        size_t backslashes = 0;
        for (wchar_t ch : arg)
        {
            if (ch == L'\\')
            {
                ++backslashes;
                continue;
            }

            if (ch == L'"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(ch);
                backslashes = 0;
                continue;
            }

            if (backslashes > 0)
            {
                result.append(backslashes, L'\\');
                backslashes = 0;
            }
            result.push_back(ch);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    bool RunProcessWait(std::wstring commandLine, DWORD& exitCode, DWORD timeoutMs = INFINITE)
    {
        exitCode = static_cast<DWORD>(-1);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return false;

        const DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateProcess(pi.hProcess, 2);
            WaitForSingleObject(pi.hProcess, 5000);
        }

        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return waitResult != WAIT_FAILED && waitResult != WAIT_TIMEOUT;
    }

    bool LaunchProcessNewConsole(std::wstring commandLine)
    {
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi))
            return false;

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    bool WriteAsciiFile(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return false;

        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return output.good();
    }

    bool ReadAsciiFile(const std::filesystem::path& path, std::string& outText)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
            return false;

        std::ostringstream oss;
        oss << input.rdbuf();
        outText = oss.str();
        return !input.bad();
    }

    std::string TrimAsciiLeftWhitespace(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            ++start;
        return value.substr(start);
    }

    bool IsValidConfigParameterKey(const std::string& key)
    {
        if (key.empty())
            return false;

        for (const char ch : key)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!std::isalnum(c) && ch != '_' && ch != '.' && ch != '-')
                return false;
        }

        return true;
    }

    bool ConfigParameterValueAllowsSemicolon(std::string key)
    {
        key = TrimAsciiWhitespace(key);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        static constexpr char overrideSuffix[] = "overrides";
        const size_t suffixLength = sizeof(overrideSuffix) - 1;
        return key.size() >= suffixLength &&
            key.compare(key.size() - suffixLength, suffixLength, overrideSuffix) == 0;
    }

    bool ConfigParameterValueAllowsHash(std::string key)
    {
        key = TrimAsciiWhitespace(key);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        return key == "manualreloadmagazineboneoverrides" ||
            key == "magazineinteractionboltboneoverrides";
    }

    size_t FindConfigCommentStart(const std::string& line)
    {
        size_t cut = std::string::npos;
        const size_t eq = line.find('=');
        const std::string key = eq == std::string::npos ? std::string() : line.substr(0, eq);
        const size_t p1 = line.find("//");
        const size_t p2 = line.find('#');
        const size_t p3 = line.find(';');
        if (p1 != std::string::npos)
            cut = p1;
        if (p2 != std::string::npos &&
            (eq == std::string::npos || p2 < eq || !ConfigParameterValueAllowsHash(key)))
        {
            cut = (cut == std::string::npos) ? p2 : (std::min)(cut, p2);
        }
        if (p3 != std::string::npos &&
            (eq == std::string::npos || p3 < eq || !ConfigParameterValueAllowsSemicolon(key)))
        {
            cut = (cut == std::string::npos) ? p3 : (std::min)(cut, p3);
        }
        return cut;
    }

    bool TryExtractConfigParameterAssignment(const std::string& rawLine, std::string& outKey, std::string& outValue, bool allowLeadingHash)
    {
        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        line = TrimAsciiLeftWhitespace(line);
        if (line.empty())
            return false;

        if (line.rfind("//", 0) == 0 || line[0] == ';')
            return false;

        if (line[0] == '#')
        {
            if (!allowLeadingHash)
                return false;

            line = TrimAsciiLeftWhitespace(line.substr(1));
            if (line.empty() || line.rfind("//", 0) == 0 || line[0] == '#' || line[0] == ';')
                return false;
        }

        const size_t cut = FindConfigCommentStart(line);
        if (cut != std::string::npos)
            line.erase(cut);

        line = TrimAsciiWhitespace(line);
        if (line.empty())
            return false;

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return false;

        outKey = TrimAsciiWhitespace(line.substr(0, eq));
        outValue = TrimAsciiWhitespace(line.substr(eq + 1));
        return IsValidConfigParameterKey(outKey);
    }

    bool TryExtractConfigParameterKey(const std::string& rawLine, std::string& outKey)
    {
        std::string value;
        return TryExtractConfigParameterAssignment(rawLine, outKey, value, false);
    }

    bool TryUncommentConfigSampleParameterLine(const std::string& rawLine, std::string& outLine)
    {
        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t firstNonWhitespace = 0;
        while (firstNonWhitespace < line.size() && std::isspace(static_cast<unsigned char>(line[firstNonWhitespace])))
            ++firstNonWhitespace;

        if (firstNonWhitespace >= line.size() || line[firstNonWhitespace] != '#')
            return false;

        const std::string afterHash = TrimAsciiLeftWhitespace(line.substr(firstNonWhitespace + 1));
        std::string key;
        std::string value;
        if (!TryExtractConfigParameterAssignment(afterHash, key, value, false))
            return false;

        outLine = line.substr(0, firstNonWhitespace) + afterHash;
        return true;
    }

    std::string BuildConfigTextFromSampleForNewInstall(const std::string& sampleText)
    {
        std::istringstream sampleStream(sampleText);
        std::ostringstream output;
        std::string line;
        while (std::getline(sampleStream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::string uncommentedLine;
            if (TryUncommentConfigSampleParameterLine(line, uncommentedLine))
                line = uncommentedLine;

            output << line << '\n';
        }

        return output.str();
    }

    std::unordered_set<std::string> ExtractConfigParameterKeys(const std::string& text, bool allowLeadingHash)
    {
        std::unordered_set<std::string> keys;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            std::string key;
            std::string value;
            if (TryExtractConfigParameterAssignment(line, key, value, allowLeadingHash))
                keys.insert(key);
        }
        return keys;
    }

    bool ConfigParameterKeyExists(const std::string& text, const std::string& targetKey)
    {
        if (targetKey.empty())
            return false;

        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line))
        {
            std::string key;
            if (TryExtractConfigParameterKey(line, key) && key == targetKey)
                return true;
        }

        return false;
    }

    bool RemoveConfigParametersNotInSample(std::string& text, const std::unordered_set<std::string>& sampleKeys)
    {
        std::istringstream input(text);
        std::ostringstream output;
        std::string line;
        bool changed = false;
        bool wroteAnyLine = false;

        while (std::getline(input, line))
        {
            std::string key;
            if (TryExtractConfigParameterKey(line, key) && sampleKeys.find(key) == sampleKeys.end())
            {
                changed = true;
                continue;
            }

            output << line << '\n';
            wroteAnyLine = true;
        }

        if (changed)
        {
            text = wroteAnyLine ? output.str() : std::string();
        }

        return changed;
    }

    bool SetConfigParameterValue(std::string& text, const std::string& targetKey, const std::string& targetValue)
    {
        if (targetKey.empty())
            return false;

        std::istringstream input(text);
        std::ostringstream output;
        std::string line;
        bool found = false;
        bool changed = false;

        while (std::getline(input, line))
        {
            std::string key;
            if (TryExtractConfigParameterKey(line, key) && key == targetKey)
            {
                const std::string replacement = targetKey + "=" + targetValue;
                if (line != replacement)
                    changed = true;
                line = replacement;
                found = true;
            }

            output << line << '\n';
        }

        if (!found)
        {
            if (!text.empty() && text.back() != '\n')
                output << '\n';
            output << targetKey << '=' << targetValue << '\n';
            changed = true;
        }

        if (changed)
            text = output.str();

        return changed;
    }

    void AppendUpdateLog(const std::filesystem::path& updateDir, const std::string& line)
    {
        std::error_code ec;
        std::filesystem::create_directories(updateDir, ec);
        std::ofstream output(updateDir / L"update_check.log", std::ios::app);
        if (output.is_open())
            output << line << "\n";
    }

    bool StartsWithAsciiNoCase(const std::string& value, const char* prefix)
    {
        if (!prefix)
            return false;

        const size_t prefixLen = std::strlen(prefix);
        if (value.size() < prefixLen)
            return false;

        for (size_t i = 0; i < prefixLen; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    }

    bool IsHexSha256(const std::string& value)
    {
        if (value.size() != 64)
            return false;
        for (char ch : value)
        {
            if (!std::isxdigit(static_cast<unsigned char>(ch)))
                return false;
        }
        return true;
    }

    bool Utf8ToWide(const std::string& value, std::wstring& out)
    {
        out.clear();
        if (value.empty())
            return true;

        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0)
        {
            count = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (count <= 0)
                return false;
            out.resize(static_cast<size_t>(count));
            return MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), out.data(), count) > 0;
        }

        out.resize(static_cast<size_t>(count));
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), count) > 0;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};

        int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
            return std::string(value.begin(), value.end());

        std::string out(static_cast<size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count, nullptr, nullptr) <= 0)
            return std::string(value.begin(), value.end());
        return out;
    }

    std::string NormalizeManifestPath(std::string value)
    {
        value = TrimAsciiWhitespace(value);
        for (char& ch : value)
        {
            if (ch == '\\')
                ch = '/';
        }

        while (!value.empty() && value.front() == '/')
            value.erase(value.begin());

        while (StartsWithAsciiNoCase(value, "./"))
            value = value.substr(2);

        if (StartsWithAsciiNoCase(value, "l4d2vr_update/"))
            value = value.substr(std::strlen("l4d2vr_update/"));

        return value;
    }

    bool TryMakeSafeRelativePath(const std::string& manifestPath, std::filesystem::path& outPath)
    {
        outPath.clear();

        const std::string normalized = NormalizeManifestPath(manifestPath);
        if (normalized.empty())
            return false;
        if (normalized.find(':') != std::string::npos)
            return false;
        if (normalized.find("//") != std::string::npos)
            return false;

        std::wstring wide;
        if (!Utf8ToWide(normalized, wide))
            return false;

        std::filesystem::path rel(wide);
        if (rel.is_absolute())
            return false;

        for (const auto& part : rel)
        {
            if (part == L".." || part == L".")
                return false;
        }

        outPath = rel;
        return true;
    }

    bool JsonReadStringAt(const std::string& json, size_t quotePos, std::string& out, size_t* outNext = nullptr)
    {
        if (quotePos >= json.size() || json[quotePos] != '"')
            return false;

        out.clear();
        for (size_t i = quotePos + 1; i < json.size(); ++i)
        {
            const char ch = json[i];
            if (ch == '"')
            {
                if (outNext)
                    *outNext = i + 1;
                return true;
            }

            if (ch == '\\')
            {
                if (++i >= json.size())
                    return false;

                const char esc = json[i];
                switch (esc)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(esc); break;
                }
                continue;
            }

            out.push_back(ch);
        }
        return false;
    }

    bool JsonFindStringValue(const std::string& json, const char* key, std::string& outValue)
    {
        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = 0;
        while ((pos = json.find(needle, pos)) != std::string::npos)
        {
            pos += needle.size();
            while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
                ++pos;
            if (pos >= json.size() || json[pos] != ':')
                continue;
            ++pos;
            while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
                ++pos;
            if (pos >= json.size() || json[pos] != '"')
                continue;
            return JsonReadStringAt(json, pos, outValue, nullptr);
        }
        return false;
    }

    bool JsonFindStringArrayValue(const std::string& json, const char* key, std::vector<std::string>& outValues)
    {
        outValues.clear();

        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = 0;
        while ((pos = json.find(needle, pos)) != std::string::npos)
        {
            pos += needle.size();
            while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
                ++pos;
            if (pos >= json.size() || json[pos] != ':')
                continue;
            ++pos;
            while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
                ++pos;
            if (pos >= json.size() || json[pos] != '[')
                continue;

            ++pos;
            while (pos < json.size())
            {
                while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
                    ++pos;
                if (pos >= json.size())
                    break;
                if (json[pos] == ']')
                    return true;
                if (json[pos] != '"')
                {
                    ++pos;
                    continue;
                }

                std::string value;
                size_t next = pos;
                if (!JsonReadStringAt(json, pos, value, &next))
                    return !outValues.empty();
                value = TrimAsciiWhitespace(value);
                if (!value.empty())
                    outValues.push_back(value);

                pos = next;
                while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
                    ++pos;
                if (pos < json.size() && json[pos] == ',')
                    ++pos;
            }

            return !outValues.empty();
        }
        return false;
    }

    bool JsonFindStringValueFromKeys(const std::string& json, const char* const* keys, size_t keyCount, std::string& outValue)
    {
        for (size_t i = 0; i < keyCount; ++i)
        {
            std::string value;
            if (JsonFindStringValue(json, keys[i], value))
            {
                value = TrimAsciiWhitespace(value);
                if (!value.empty())
                {
                    outValue = value;
                    return true;
                }
            }
        }

        return false;
    }

    bool JsonFindStringArrayValueFromKeys(const std::string& json, const char* const* keys, size_t keyCount, std::vector<std::string>& outValues)
    {
        for (size_t i = 0; i < keyCount; ++i)
        {
            std::vector<std::string> values;
            if (JsonFindStringArrayValue(json, keys[i], values) && !values.empty())
            {
                outValues = values;
                return true;
            }
        }

        outValues.clear();
        return false;
    }

    std::string BuildManifestNotesText(const std::string& json, bool useChineseText)
    {
        std::string notes;

        const char* const chineseStringKeys[] = {
            "changesh",
            "changesZh",
            "changesZH",
            "changesCn",
            "changesCN"
        };
        const char* const englishStringKeys[] = {
            "changes",
            "changesEn",
            "changesEN",
            "changesEnglish"
        };
        const char* const genericStringKeys[] = {
            "notes",
            "changelog",
            "changeLog",
            "releaseNotes",
            "updateNotes"
        };

        const char* const* localizedStringKeys = useChineseText ? chineseStringKeys : englishStringKeys;
        const size_t localizedStringKeyCount = useChineseText ? _countof(chineseStringKeys) : _countof(englishStringKeys);
        const char* const* fallbackStringKeys = useChineseText ? englishStringKeys : chineseStringKeys;
        const size_t fallbackStringKeyCount = useChineseText ? _countof(englishStringKeys) : _countof(chineseStringKeys);

        if (!JsonFindStringValueFromKeys(json, localizedStringKeys, localizedStringKeyCount, notes) &&
            !JsonFindStringValueFromKeys(json, genericStringKeys, _countof(genericStringKeys), notes))
        {
            JsonFindStringValueFromKeys(json, fallbackStringKeys, fallbackStringKeyCount, notes);
        }

        std::vector<std::string> changes;
        const char* const chineseArrayKeys[] = {
            "changesh",
            "changesZh",
            "changesZH",
            "changesCn",
            "changesCN"
        };
        const char* const englishArrayKeys[] = {
            "changes",
            "changesEn",
            "changesEN",
            "changesEnglish"
        };
        const char* const genericArrayKeys[] = {
            "updateItems"
        };

        const char* const* localizedArrayKeys = useChineseText ? chineseArrayKeys : englishArrayKeys;
        const size_t localizedArrayKeyCount = useChineseText ? _countof(chineseArrayKeys) : _countof(englishArrayKeys);
        const char* const* fallbackArrayKeys = useChineseText ? englishArrayKeys : chineseArrayKeys;
        const size_t fallbackArrayKeyCount = useChineseText ? _countof(englishArrayKeys) : _countof(chineseArrayKeys);

        if (!JsonFindStringArrayValueFromKeys(json, localizedArrayKeys, localizedArrayKeyCount, changes) &&
            !JsonFindStringArrayValueFromKeys(json, genericArrayKeys, _countof(genericArrayKeys), changes))
        {
            JsonFindStringArrayValueFromKeys(json, fallbackArrayKeys, fallbackArrayKeyCount, changes);
        }

        if (!changes.empty())
        {
            if (!notes.empty())
                notes += "\n";
            for (const std::string& change : changes)
            {
                notes += "\n- ";
                notes += change;
            }
        }

        return TrimAsciiWhitespace(notes);
    }

    std::wstring MakeMessageBoxFriendlyText(const std::string& utf8, size_t maxChars)
    {
        std::wstring wide;
        if (!Utf8ToWide(utf8, wide))
            wide.assign(utf8.begin(), utf8.end());

        for (wchar_t& ch : wide)
        {
            if (ch == L'\r')
                ch = L'\n';
        }

        std::wstring normalized;
        normalized.reserve(wide.size());
        bool prevNewline = false;
        for (wchar_t ch : wide)
        {
            if (ch == L'\n')
            {
                if (!prevNewline)
                    normalized.push_back(ch);
                prevNewline = true;
            }
            else
            {
                normalized.push_back(ch);
                prevNewline = false;
            }
        }

        if (normalized.size() > maxChars)
        {
            normalized.resize(maxChars);
            normalized += L"\n...";
        }
        return normalized;
    }

    struct UpdateManifest
    {
        std::string version;
        std::string notes;
    };

    bool ParseUpdateManifest(const std::string& json, UpdateManifest& manifest, std::string& error)
    {
        manifest = {};
        if (!JsonFindStringValue(json, "version", manifest.version))
        {
            error = "manifest.json is missing string field: version";
            return false;
        }

        manifest.version = TrimAsciiWhitespace(manifest.version);
        if (manifest.version.empty())
        {
            error = "manifest.json contains an empty version";
            return false;
        }

        manifest.notes = BuildManifestNotesText(json, ShouldUseChineseLaunchArgumentPrompt());
        return true;
    }

    std::vector<uint64_t> ExtractVersionNumbers(const std::string& version)
    {
        std::vector<uint64_t> out;
        uint64_t current = 0;
        bool inNumber = false;
        for (char ch : version)
        {
            if (std::isdigit(static_cast<unsigned char>(ch)))
            {
                inNumber = true;
                const uint64_t digit = static_cast<uint64_t>(ch - '0');
                if (current <= (UINT64_MAX - digit) / 10ull)
                    current = current * 10ull + digit;
            }
            else if (inNumber)
            {
                out.push_back(current);
                current = 0;
                inNumber = false;
            }
        }
        if (inNumber)
            out.push_back(current);
        return out;
    }

    int CompareVersions(const std::string& a, const std::string& b)
    {
        const std::vector<uint64_t> av = ExtractVersionNumbers(a);
        const std::vector<uint64_t> bv = ExtractVersionNumbers(b);
        const size_t count = (std::max)(av.size(), bv.size());
        for (size_t i = 0; i < count; ++i)
        {
            const uint64_t ai = (i < av.size()) ? av[i] : 0;
            const uint64_t bi = (i < bv.size()) ? bv[i] : 0;
            if (ai > bi)
                return 1;
            if (ai < bi)
                return -1;
        }
        return 0;
    }

    std::filesystem::path FindWorkshopUpdateVpk(const std::filesystem::path& gameRootPath)
    {
        const std::vector<std::filesystem::path> candidates = {
            gameRootPath / L"left4dead2" / L"addons" / L"workshop" / kWorkshopUpdateVpkName,
            gameRootPath / L"left4dead2" / L"addons" / kWorkshopUpdateVpkName,
            gameRootPath / L".." / L".." / L"workshop" / L"content" / L"550" / kWorkshopUpdateItemId / kWorkshopUpdateVpkName,
            gameRootPath / L".." / L"workshop" / L"content" / L"550" / kWorkshopUpdateItemId / kWorkshopUpdateVpkName
        };

        for (const auto& candidate : candidates)
        {
            std::error_code ec;
            const std::filesystem::path normalized = std::filesystem::weakly_canonical(candidate, ec);
            const std::filesystem::path& checkPath = ec ? candidate : normalized;
            if (FileExistsNoThrow(checkPath))
                return checkPath;
        }

        const std::vector<std::filesystem::path> searchRoots = {
            gameRootPath / L"left4dead2" / L"addons" / L"workshop",
            gameRootPath / L".." / L".." / L"workshop" / L"content" / L"550" / kWorkshopUpdateItemId,
            gameRootPath / L".." / L"workshop" / L"content" / L"550" / kWorkshopUpdateItemId
        };

        for (const auto& root : searchRoots)
        {
            if (!FileExistsNoThrow(root))
                continue;

            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
                !ec && it != end;
                it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                if (_wcsicmp(it->path().filename().c_str(), kWorkshopUpdateVpkName) == 0)
                    return it->path();
            }
        }

        return {};
    }

    bool WritePowerShellExtractManifestScript(const std::filesystem::path& scriptPath)
    {
        static const char script[] =
            "param([string]$ZipPath, [string]$OutputPath)\r\n"
            "$ErrorActionPreference = 'Stop'\r\n"
            "Add-Type -AssemblyName System.IO.Compression.FileSystem\r\n"
            "$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)\r\n"
            "try {\r\n"
            "  $entry = $null\r\n"
            "  foreach ($e in $zip.Entries) {\r\n"
            "    $name = $e.FullName.Replace('\\', '/')\r\n"
            "    if ([string]::Equals($name, 'manifest.json', [System.StringComparison]::OrdinalIgnoreCase)) { $entry = $e; break }\r\n"
            "  }\r\n"
            "  if ($null -eq $entry) { exit 2 }\r\n"
            "  $dir = [System.IO.Path]::GetDirectoryName($OutputPath)\r\n"
            "  [System.IO.Directory]::CreateDirectory($dir) | Out-Null\r\n"
            "  if ([System.IO.File]::Exists($OutputPath)) { [System.IO.File]::Delete($OutputPath) }\r\n"
            "  [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $OutputPath)\r\n"
            "  exit 0\r\n"
            "}\r\n"
            "finally { $zip.Dispose() }\r\n";
        return WriteAsciiFile(scriptPath, script);
    }

    bool WritePowerShellExtractUpdateScript(const std::filesystem::path& scriptPath)
    {
        static const char script[] =
            "param([string]$ZipPath, [string]$StageRoot)\r\n"
            "$ErrorActionPreference = 'Stop'\r\n"
            "Add-Type -AssemblyName System.IO.Compression.FileSystem\r\n"
            "if ([System.IO.Directory]::Exists($StageRoot)) { Remove-Item -LiteralPath $StageRoot -Recurse -Force }\r\n"
            "[System.IO.Directory]::CreateDirectory($StageRoot) | Out-Null\r\n"
            "$stageFull = [System.IO.Path]::GetFullPath($StageRoot)\r\n"
            "$stagePrefix = $stageFull.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar\r\n"
            "$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)\r\n"
            "try {\r\n"
            "  foreach ($entry in $zip.Entries) {\r\n"
            "    $name = $entry.FullName.Replace('\\', '/')\r\n"
            "    if ([string]::IsNullOrWhiteSpace($name)) { continue }\r\n"
            "    if ($name.StartsWith('/', [System.StringComparison]::OrdinalIgnoreCase)) { exit 3 }\r\n"
            "    if ($name.Contains('..')) {\r\n"
            "      $parts = $name.Split('/')\r\n"
            "      foreach ($part in $parts) { if ($part -eq '..') { exit 3 } }\r\n"
            "    }\r\n"
            "    $dest = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($StageRoot, $name))\r\n"
            "    if (($dest -ne $stageFull) -and (-not $dest.StartsWith($stagePrefix, [System.StringComparison]::OrdinalIgnoreCase))) { exit 3 }\r\n"
            "    if ($name.EndsWith('/')) { [System.IO.Directory]::CreateDirectory($dest) | Out-Null; continue }\r\n"
            "    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($dest)) | Out-Null\r\n"
            "    if ([System.IO.File]::Exists($dest)) { [System.IO.File]::Delete($dest) }\r\n"
            "    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest)\r\n"
            "  }\r\n"
            "  exit 0\r\n"
            "}\r\n"
            "finally { $zip.Dispose() }\r\n";
        return WriteAsciiFile(scriptPath, script);
    }

    bool RunPowerShellScript(const std::filesystem::path& scriptPath, const std::vector<std::filesystem::path>& args, DWORD& exitCode)
    {
        std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File ";
        cmd += QuoteWinArg(scriptPath.wstring());
        for (const auto& arg : args)
        {
            cmd.push_back(L' ');
            cmd += QuoteWinArg(arg.wstring());
        }
        return RunProcessWait(cmd, exitCode, 10 * 60 * 1000);
    }

    bool WriteRunUpdateCmd(const std::filesystem::path& cmdPath, const std::filesystem::path& gameRootPath, const std::filesystem::path& stagingPath, const std::string& newVersion, bool useChineseText)
    {
        const DWORD pid = GetCurrentProcessId();
        const std::wstring root = gameRootPath.wstring();
        const std::wstring staging = stagingPath.wstring();
        const std::wstring logPath = (gameRootPath / L"VR" / L"update" / L"run_update.log").wstring();
        const std::wstring donePath = (gameRootPath / L"VR" / L"update" / L"update_complete.txt").wstring();
        const std::wstring dllPath = (gameRootPath / L"d3d9.dll").wstring();

        const std::string titleText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u52A9\u624B") : "L4D2VR Update Helper";
        const std::string startedText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u5DF2\u542F\u52A8\u3002\u76EE\u6807\u7248\u672C\uFF1A") : "L4D2VR update started. Target version: ";
        const std::string waitProcessText = useChineseText ? WideToUtf8(L"\u6B63\u5728\u7B49\u5F85\u6E38\u620F\u8FDB\u7A0B %GAME_PID% \u9000\u51FA...") : "Waiting for game process %GAME_PID% to exit...";
        const std::string processExitedText = useChineseText ? WideToUtf8(L"\u6E38\u620F\u8FDB\u7A0B\u5DF2\u9000\u51FA\u3002\u6B63\u5728\u7B49\u5F85 d3d9.dll \u89E3\u9501...") : "Game process has exited. Waiting for d3d9.dll unlock...";
        const std::string copyingText = useChineseText ? WideToUtf8(L"\u6B63\u5728\u590D\u5236\u66F4\u65B0\u6587\u4EF6...") : "Copying staged files...";
        const std::string completeLogText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u5B8C\u6210\u3002Robocopy=") : "L4D2VR update complete. Robocopy=";
        const std::string completeDoneText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u5B8C\u6210\u3002\u7248\u672C ") : "L4D2VR update complete. Version ";
        const std::string completeWindowText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u5B8C\u6210\u3002\u73B0\u5728\u53EF\u4EE5\u91CD\u65B0\u542F\u52A8\u6E38\u620F\u3002") : "L4D2VR update complete. You can restart the game now.";
        const std::string failedLogText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u5931\u8D25\u3002Robocopy=") : "L4D2VR update failed. Robocopy=";
        const std::string failedWindowText = useChineseText ? WideToUtf8(L"L4D2VR \u66F4\u65B0\u5931\u8D25\u3002\u8BF7\u68C0\u67E5 VR\\update\\run_update.log\u3002") : "L4D2VR update failed. Check VR\\update\\run_update.log.";
        const std::string pressKeyText = useChineseText ? WideToUtf8(L"\u8BF7\u6309\u4EFB\u610F\u952E\u5173\u95ED\u6B64\u7A97\u53E3...") : "Press any key to close this window...";

        std::ostringstream cmd;
        cmd << "@echo off\r\n";
        cmd << "setlocal EnableExtensions\r\n";
        cmd << "chcp 65001 >nul\r\n";
        cmd << "title " << titleText << "\r\n";
        cmd << "set \"GAME_PID=" << pid << "\"\r\n";
        cmd << "set \"ROOT=" << WideToUtf8(root) << "\"\r\n";
        cmd << "set \"STAGING=" << WideToUtf8(staging) << "\"\r\n";
        cmd << "set \"LOG=" << WideToUtf8(logPath) << "\"\r\n";
        cmd << "set \"DONE=" << WideToUtf8(donePath) << "\"\r\n";
        cmd << "set \"DLL_PATH=" << WideToUtf8(dllPath) << "\"\r\n";
        cmd << "echo [%date% %time%] " << startedText << newVersion << " > \"%LOG%\"\r\n";
        cmd << "echo " << waitProcessText << " >> \"%LOG%\"\r\n";
        cmd << ":wait_process\r\n";
        cmd << "tasklist /FI \"PID eq %GAME_PID%\" 2>nul | findstr /C:\"%GAME_PID%\" >nul\r\n";
        cmd << "if not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait_process)\r\n";
        cmd << "echo " << processExitedText << " >> \"%LOG%\"\r\n";
        cmd << ":wait_unlock\r\n";
        cmd << "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"try { $fs=[System.IO.File]::Open($env:DLL_PATH,'Open','ReadWrite','None'); $fs.Close(); exit 0 } catch { exit 1 }\" >nul 2>nul\r\n";
        cmd << "if errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait_unlock)\r\n";
        cmd << "echo " << copyingText << " >> \"%LOG%\"\r\n";
        cmd << "robocopy \"%STAGING%\" \"%ROOT%\" /E /COPY:DAT /R:20 /W:1 /NFL /NDL /NP /NJH /NJS >> \"%LOG%\"\r\n";
        cmd << "set \"RC=%ERRORLEVEL%\"\r\n";
        cmd << "if %RC% GEQ 8 goto fail\r\n";
        cmd << "echo [%date% %time%] " << completeLogText << "%RC% >> \"%LOG%\"\r\n";
        cmd << "echo " << completeDoneText << newVersion << " > \"%DONE%\"\r\n";
        cmd << "start \"\" cmd /c \"chcp 65001 >nul & echo " << completeWindowText << "&echo.&echo " << pressKeyText << "&pause >nul\"\r\n";
        cmd << "exit /b 0\r\n";
        cmd << ":fail\r\n";
        cmd << "echo [%date% %time%] " << failedLogText << "%RC% >> \"%LOG%\"\r\n";
        cmd << "start \"\" cmd /c \"chcp 65001 >nul & echo " << failedWindowText << "&echo.&echo " << pressKeyText << "&pause >nul\"\r\n";
        cmd << "exit /b %RC%\r\n";

        return WriteAsciiFile(cmdPath, cmd.str());
    }

    bool LaunchRunUpdateCmd(const std::filesystem::path& cmdPath)
    {
        std::wstring commandLine = L"cmd.exe /c ";
        commandLine += QuoteWinArg(cmdPath.wstring());
        return LaunchProcessNewConsole(commandLine);
    }

    const wchar_t* GetUpdateMessageTitle()
    {
        return ShouldUseChineseLaunchArgumentPrompt() ? L"L4D2VR \u66F4\u65B0" : L"L4D2VR Update";
    }

    std::wstring SelectLocalizedUpdateText(const std::wstring& english, const std::wstring& chinese)
    {
        if (ShouldUseChineseLaunchArgumentPrompt() && !chinese.empty())
            return chinese;
        return english;
    }

    void ShowUpdateError(const std::wstring& englishMessage, const std::wstring& chineseMessage = L"")
    {
        HWND owner = FindCurrentProcessMainWindow();
        const std::wstring message = SelectLocalizedUpdateText(englishMessage, chineseMessage);
        MessageBoxW(owner, message.c_str(), GetUpdateMessageTitle(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }

    void ShowUpdateInfo(const std::wstring& englishMessage, const std::wstring& chineseMessage = L"")
    {
        HWND owner = FindCurrentProcessMainWindow();
        const std::wstring message = SelectLocalizedUpdateText(englishMessage, chineseMessage);
        MessageBoxW(owner, message.c_str(), GetUpdateMessageTitle(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }

    bool CheckWorkshopUpdateOnce()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return false;

        const std::filesystem::path updateDir = gameRootPath / L"VR" / L"update";
        const std::filesystem::path vpkPath = FindWorkshopUpdateVpk(gameRootPath);
        if (vpkPath.empty())
        {
            AppendUpdateLog(updateDir, "workshop vpk not found: 3724995607.vpk");
            return false;
        }

        const std::filesystem::path manifestPath = updateDir / L"workshop_manifest.json";
        const std::filesystem::path extractManifestScript = updateDir / L"extract_manifest.ps1";
        if (!WritePowerShellExtractManifestScript(extractManifestScript))
        {
            AppendUpdateLog(updateDir, "failed to write extract_manifest.ps1");
            return false;
        }

        DWORD extractExit = 0;
        if (!RunPowerShellScript(extractManifestScript, { vpkPath, manifestPath }, extractExit) || extractExit != 0)
        {
            AppendUpdateLog(updateDir, "failed to read root manifest.json from vpk, exit=" + std::to_string(extractExit));
            return false;
        }

        std::string manifestJson;
        if (!ReadAsciiFile(manifestPath, manifestJson))
        {
            AppendUpdateLog(updateDir, "failed to read extracted manifest.json");
            return false;
        }

        UpdateManifest manifest;
        std::string parseError;
        if (!ParseUpdateManifest(manifestJson, manifest, parseError))
        {
            AppendUpdateLog(updateDir, parseError);
            return false;
        }

        std::string localVersion;
        if (!ReadAsciiFile(gameRootPath / L"VR" / L"version.txt", localVersion))
            localVersion = "0";
        localVersion = TrimAsciiWhitespace(localVersion);
        manifest.version = TrimAsciiWhitespace(manifest.version);

        if (CompareVersions(manifest.version, localVersion) <= 0)
        {
            AppendUpdateLog(updateDir, "no update needed. local=" + localVersion + ", workshop=" + manifest.version);
            return false;
        }

        const bool useChineseUpdatePrompt = ShouldUseChineseLaunchArgumentPrompt();
        std::wstring message;
        if (useChineseUpdatePrompt)
        {
            message = L"\u68C0\u6D4B\u5230\u65B0\u7684 L4D2VR \u5DE5\u574A\u66F4\u65B0\u3002\n\n";
            message += L"\u5F53\u524D\u7248\u672C\uFF1A";
            message += std::wstring(localVersion.begin(), localVersion.end());
            message += L"\n\u5DE5\u574A\u7248\u672C\uFF1A";
            message += std::wstring(manifest.version.begin(), manifest.version.end());
            if (!manifest.notes.empty())
            {
                message += L"\n\n\u66F4\u65B0\u5185\u5BB9\uFF1A\n";
                message += MakeMessageBoxFriendlyText(manifest.notes, 1800);
            }
            message += L"\n\n\u662F\u5426\u73B0\u5728\u5B89\u88C5\uFF1F\u6E38\u620F\u4F1A\u9000\u51FA\uFF0C\u968F\u540E\u66F4\u65B0\u811A\u672C\u4F1A\u5728 d3d9.dll \u89E3\u9501\u540E\u590D\u5236\u65B0\u6587\u4EF6\u3002";
        }
        else
        {
            message = L"Detected a newer L4D2VR workshop update.\n\n";
            message += L"Current version: ";
            message += std::wstring(localVersion.begin(), localVersion.end());
            message += L"\nWorkshop version: ";
            message += std::wstring(manifest.version.begin(), manifest.version.end());
            if (!manifest.notes.empty())
            {
                message += L"\n\nUpdate notes:\n";
                message += MakeMessageBoxFriendlyText(manifest.notes, 1800);
            }
            message += L"\n\nInstall it now? The game will quit, then the updater cmd will copy the new files after d3d9.dll is unlocked.";
        }

        HWND owner = FindCurrentProcessMainWindow();
        const int answer = MessageBoxW(owner, message.c_str(), GetUpdateMessageTitle(), MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
        if (answer != IDYES)
        {
            AppendUpdateLog(updateDir, "user declined update. local=" + localVersion + ", workshop=" + manifest.version);
            return false;
        }

        const std::filesystem::path stagingPath = updateDir / L"staging";
        const std::filesystem::path extractUpdateScript = updateDir / L"extract_update.ps1";
        if (!WritePowerShellExtractUpdateScript(extractUpdateScript))
        {
            ShowUpdateError(L"Failed to write VR\\update\\extract_update.ps1.", L"\u65E0\u6CD5\u5199\u5165 VR\\update\\extract_update.ps1\u3002");
            return false;
        }

        DWORD updateExtractExit = 0;
        if (!RunPowerShellScript(extractUpdateScript, { vpkPath, stagingPath }, updateExtractExit) || updateExtractExit != 0)
        {
            ShowUpdateError(L"Failed to extract root files from 3724995607.vpk. Check VR\\update\\update_check.log.", L"\u65E0\u6CD5\u4ECE 3724995607.vpk \u89E3\u538B\u66F4\u65B0\u6587\u4EF6\u3002\u8BF7\u68C0\u67E5 VR\\update\\update_check.log\u3002");
            AppendUpdateLog(updateDir, "failed to extract update files, exit=" + std::to_string(updateExtractExit));
            return false;
        }

        const std::filesystem::path runCmdPath = updateDir / L"run_update.cmd";
        if (!WriteRunUpdateCmd(runCmdPath, gameRootPath, stagingPath, manifest.version, useChineseUpdatePrompt))
        {
            ShowUpdateError(L"Failed to generate VR\\update\\run_update.cmd.", L"\u65E0\u6CD5\u751F\u6210 VR\\update\\run_update.cmd\u3002");
            return false;
        }

        if (!LaunchRunUpdateCmd(runCmdPath))
        {
            ShowUpdateError(L"Failed to start cmd.exe for VR\\update\\run_update.cmd.", L"\u65E0\u6CD5\u542F\u52A8\u7528\u4E8E\u6267\u884C VR\\update\\run_update.cmd \u7684 cmd.exe\u3002");
            return false;
        }

        AppendUpdateLog(updateDir, "update accepted and updater cmd launched. local=" + localVersion + ", workshop=" + manifest.version);
        ShowUpdateInfo(L"The update helper has started. The game will quit now. Do not restart until the cmd window reports completion.", L"\u66F4\u65B0\u52A9\u624B\u5DF2\u542F\u52A8\u3002\u6E38\u620F\u5373\u5C06\u9000\u51FA\u3002\u5728 cmd \u7A97\u53E3\u63D0\u793A\u5B8C\u6210\u4E4B\u524D\uFF0C\u8BF7\u4E0D\u8981\u91CD\u65B0\u542F\u52A8\u6E38\u620F\u3002");

        if (g_Game && g_Game->m_Initialized)
        {
            g_Game->ClientCmd_Unrestricted("quit\n");
            g_Game->ClientCmd("quit\n");
        }
        else
        {
            HWND hwnd = FindCurrentProcessMainWindow();
            if (hwnd)
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }

        return true;
    }

    struct OneShotConfigMigration
    {
        const char* id = nullptr;
        const char* key = nullptr;
        const char* value = nullptr;
    };

    constexpr OneShotConfigMigration kOneShotConfigMigrations[] =
    {
        { "2026-05-23_auto_mat_queue_mode_default_false", "AutoMatQueueMode", "false" },
        { "2026-06-12_vr_recommended_video_settings_default_false", "VrRecommendedVideoSettingsEnabled", "false" },
        { "2026-08-12_vr_hands_gloves_disable_once", "VrHandsGlovesEnabled", "false" },
    };

    std::unordered_set<std::string> ReadAppliedConfigMigrationIds(const std::filesystem::path& statePath)
    {
        std::unordered_set<std::string> ids;

        std::string text;
        if (!ReadAsciiFile(statePath, text))
            return ids;

        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            line = TrimAsciiWhitespace(line);
            if (line.empty() || line[0] == '#')
                continue;

            const size_t comment = line.find('#');
            if (comment != std::string::npos)
                line = TrimAsciiWhitespace(line.substr(0, comment));
            if (!line.empty())
                ids.insert(line);
        }

        return ids;
    }

    bool WriteAppliedConfigMigrationIds(const std::filesystem::path& statePath, const std::unordered_set<std::string>& ids)
    {
        std::vector<std::string> sortedIds(ids.begin(), ids.end());
        std::sort(sortedIds.begin(), sortedIds.end());

        std::ostringstream output;
        output << "# L4D2VR one-shot config migrations. Do not edit unless you want a migration to run again.\n";
        for (const std::string& id : sortedIds)
            output << id << '\n';

        return WriteAsciiFile(statePath, output.str());
    }

    void MigrateExistingConfigOnce()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const std::filesystem::path vrPath = gameRootPath / L"vr";
        const std::filesystem::path configPath = vrPath / L"config.txt";
        const std::filesystem::path statePath = vrPath / kConfigMigrationsStateFile;

        // Only migrate existing user configs. New installs should get current defaults from config.sample.
        if (!FileExistsNoThrow(configPath))
            return;

        std::string configText;
        if (!ReadAsciiFile(configPath, configText))
            return;

        std::unordered_set<std::string> appliedIds = ReadAppliedConfigMigrationIds(statePath);

        bool anyPendingMigration = false;
        bool configChanged = false;
        for (const OneShotConfigMigration& migration : kOneShotConfigMigrations)
        {
            if (!migration.id || !*migration.id || !migration.key || !*migration.key || !migration.value)
                continue;
            if (appliedIds.find(migration.id) != appliedIds.end())
                continue;

            anyPendingMigration = true;
            if (SetConfigParameterValue(configText, migration.key, migration.value))
                configChanged = true;
            appliedIds.insert(migration.id);
        }

        if (!anyPendingMigration)
            return;

        if (configChanged && !WriteAsciiFile(configPath, configText))
            return;

        WriteAppliedConfigMigrationIds(statePath, appliedIds);
    }

    void EnsureVrConfigTxtFromSample()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const std::filesystem::path vrPath = gameRootPath / L"vr";
        const std::filesystem::path configPath = vrPath / L"config.txt";
        const std::filesystem::path samplePath = vrPath / L"config.sample";
        if (!FileExistsNoThrow(samplePath))
            return;

        std::string sampleText;
        if (!ReadAsciiFile(samplePath, sampleText))
            return;

        if (!FileExistsNoThrow(configPath))
        {
            WriteAsciiFile(configPath, BuildConfigTextFromSampleForNewInstall(sampleText));
            return;
        }

        std::string configText;
        if (!ReadAsciiFile(configPath, configText))
            return;

        bool configChanged = false;
        const std::unordered_set<std::string> sampleKeys = ExtractConfigParameterKeys(sampleText, true);
        configChanged |= RemoveConfigParametersNotInSample(configText, sampleKeys);

        std::istringstream sampleStream(sampleText);
        std::string line;
        while (std::getline(sampleStream, line))
        {
            std::string key;
            std::string value;
            if (TryExtractConfigParameterAssignment(line, key, value, false))
            {
                if (SetConfigParameterValue(configText, key, value))
                    configChanged = true;
                continue;
            }

            if (!TryExtractConfigParameterAssignment(line, key, value, true))
                continue;

            if (!ConfigParameterKeyExists(configText, key) && SetConfigParameterValue(configText, key, value))
                configChanged = true;
        }

        if (configChanged)
            WriteAsciiFile(configPath, configText);
    }

    void EnsureUiFontFixVpkAndConfig()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const bool bigFronEnabled = IsBigFronLaunchArgPresent();
        if (!bigFronEnabled)
        {
            RemoveUiFontFixVpkAndConfig(gameRootPath);
            return;
        }

        const std::filesystem::path sourceVpkPath = gameRootPath / L"vr" / kUiFontFixFileName;
        if (!FileExistsNoThrow(sourceVpkPath))
            return;

        const std::filesystem::path left4dead2Path = gameRootPath / L"left4dead2";
        const std::filesystem::path addonsPath = left4dead2Path / L"addons";
        const std::filesystem::path targetVpkPath = addonsPath / kUiFontFixFileName;

        if (!FileExistsNoThrow(targetVpkPath))
        {
            std::error_code ec;
            std::filesystem::create_directories(addonsPath, ec);
            std::filesystem::copy_file(sourceVpkPath, targetVpkPath, std::filesystem::copy_options::none, ec);
            if (!FileExistsNoThrow(targetVpkPath))
                return;
        }

        EnsureUiFontFixAddonListFirst(left4dead2Path / L"addonlist.txt");

        int width = 0;
        int height = 0;
        if (!TryGetCurrentDisplayResolution(width, height))
            return;

        EnsureAutoexecMatSetVideoMode(left4dead2Path / L"cfg" / L"autoexec.cfg", width, height);
    }
}

extern "C" bool __cdecl L4D2VR_QueryDataCacheUsage(
    uint32_t* currentBytes,
    uint32_t* totalBytes)
{
    if (!currentBytes || !totalBytes)
        return false;

    *currentBytes = 0;
    *totalBytes = 0;

    SourceDataCache* dataCache = FindSourceDataCache();
    if (!dataCache)
        return false;

    SourceDataCacheStatus status{};
    SourceDataCacheLimits limits{};
#ifdef _MSC_VER
    __try
    {
        dataCache->GetStatus(&status, &limits);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    dataCache->GetStatus(&status, &limits);
#endif

    if (limits.maxBytes == 0)
        return false;

    *currentBytes = status.bytes;
    *totalBytes = limits.maxBytes;
    return true;
}

bool L4D2VR_ApplyRecommendedVideoSettings()
{
    ApplyRuntimeVideoSettings();
    return EnsureVideoCfgSettings();
}

DWORD WINAPI InitL4D2VR(HMODULE hModule)
{
    timeBeginPeriod(1);
    StartForcedIndexBufferPolicy();

#ifdef _DEBUG
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif

    // Make sure -insecure is used
    LPWSTR* szArglist;
    int nArgs;
    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    bool insecureEnabled = false;
    for (int i = 0; i < nArgs; ++i)
    {
        if (wcscmp(szArglist[i], L"-insecure") == 0)
            insecureEnabled = true;
    }
    LocalFree(szArglist);

    MigrateExistingConfigOnce();
    EnsureVrConfigTxtFromSample();
    EnsureUiFontFixVpkAndConfig();
    if (IsNoHmdLaunchArgPresent())
    {
        EnsureNoHmdAutoexecCrosshair();
		// The normal Game constructor immediately initializes OpenVR, so -nohmd
		// intentionally skips it.  Keep a passive, material-only Neko probe alive
		// for desktop/VR output comparison without changing desktop rendering.
		Hooks::InitializeNoHmdNekoPostProbe();
        FinalizeForcedDataCachePolicy();
        return 0;
    }

    ShowLaunchArgumentNoticeIfNeeded();

    CreateThread(nullptr, 0, FocusGameWindowWorker, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, MaintainWindowTitleWorker, nullptr, 0, nullptr);

    const bool recommendedVideoSettingsEnabled = IsVrRecommendedVideoSettingsEnabledInConfig();

    // Persist once before engine interfaces are ready. Runtime ConVars are applied once
    // after Game initializes; later reapplication only happens on config enable/menu return.
    if (recommendedVideoSettingsEnabled)
        EnsureVideoCfgSettings();

    g_Game = new Game();
    FinalizeForcedDataCachePolicy();

    if (CheckWorkshopUpdateOnce())
        return 0;

    if (recommendedVideoSettingsEnabled)
        ApplyRuntimeVideoSettings();

    return 0;
}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitL4D2VR, hModule, 0, NULL);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        L4D2VR_ShutdownReShadeVRBridge();
        L4D2VR_ShutdownSystemMouseInputSuppression();
        Game::UninstallVertexFormatWarningFilter();
        timeEndPeriod(1);
        break;
    }
    return TRUE;
}
