#include "game.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"
#include "sigscanner.h"
#include "vr_pose_protocol.h"
#include "sdk/ivdebugoverlay.h"

static std::mutex logMutex;
static std::once_flag logResetOnce;
using tCreateInterface = void* (__cdecl*)(const char* name, int* returnCode);

void L4D2VRConfigOverlay_StartWorker();
void L4D2VRConfigOverlay_Open();

namespace
{
    static_assert(sizeof(void*) == 4, "L4D2VR ConVar bridge assumes 32-bit Source DLL layout.");
    static constexpr size_t kConVarVtableIndexSetValueString = 7;
    static constexpr size_t kConVarVtableIndexSetValueFloat = 8;
    static constexpr size_t kConVarVtableIndexSetValueInt = 9;
    static constexpr size_t kConVarVtableIndexInternalSetValueString = 10;
    static constexpr size_t kConVarVtableIndexInternalSetValueFloat = 11;
    static constexpr size_t kConVarVtableIndexInternalSetValueInt = 12;
    static constexpr size_t kIConVarVtableIndexSetValueString = 0;
    static constexpr size_t kIConVarVtableIndexSetValueFloat = 1;
    static constexpr size_t kIConVarVtableIndexSetValueInt = 2;
    static thread_local int s_ConVarWritePermitDepth = 0;
    static constexpr char kVertexFormatSpamMessage[] = "Too many vertex format changes in frame, whole world not rendered";
    using SpewOutputFunc_t = int(__cdecl*)(int spewType, const char* pMsg);
    using tSetSpewOutputFunc = SpewOutputFunc_t(__cdecl*)(SpewOutputFunc_t func);
    using tGetSpewOutputFunc = SpewOutputFunc_t(__cdecl*)();
    static tSetSpewOutputFunc s_SetSpewOutputFunc = nullptr;
    static tGetSpewOutputFunc s_GetSpewOutputFunc = nullptr;
    static SpewOutputFunc_t s_OriginalSpewOutputFunc = nullptr;
    static bool s_VertexFormatWarningFilterInstalled = false;
    static bool FixCacheSummaryNegativePercent(std::string& text, const char* suffix)
    {
        bool changed = false;
        size_t searchFrom = 0;
        while (suffix && *suffix)
        {
            const size_t suffixPos = text.find(suffix, searchFrom);
            if (suffixPos == std::string::npos)
                return changed;

            size_t numberEnd = suffixPos;
            while (numberEnd > 0 && std::isspace(static_cast<unsigned char>(text[numberEnd - 1])))
                --numberEnd;

            size_t numberStart = numberEnd;
            while (numberStart > 0)
            {
                const unsigned char c = static_cast<unsigned char>(text[numberStart - 1]);
                if (!std::isdigit(c) && c != '.')
                    break;
                --numberStart;
            }

            if (numberStart > 0 && text[numberStart - 1] == '-')
            {
                text.erase(numberStart - 1, 1);
                searchFrom = suffixPos - 1 + std::strlen(suffix);
                changed = true;
            }
            else
            {
                searchFrom = suffixPos + std::strlen(suffix);
            }
        }
        return changed;
    }

    static bool FormatCacheSummaryBytesAsMiB(const char* message, std::string& output)
    {
        if (!message || !*message)
            return false;

        output = message;
        // The engine may emit the command output on a different thread from the
        // ConCommand callback. Identify only cache summary rows by their stable
        // labels instead of relying on thread-local dispatch state.
        if (output.find("Section [") == std::string::npos &&
            output.find("Summary:") == std::string::npos)
        {
            return false;
        }

        bool changed = false;
        size_t searchFrom = 0;
        constexpr char kBytesSuffix[] = " bytes";
        while (true)
        {
            const size_t suffixPos = output.find(kBytesSuffix, searchFrom);
            if (suffixPos == std::string::npos)
                break;

            size_t numberStart = suffixPos;
            while (numberStart > 0)
            {
                const unsigned char c = static_cast<unsigned char>(output[numberStart - 1]);
                if (!std::isdigit(c) && c != ',')
                    break;
                --numberStart;
            }
            if (numberStart > 0 && output[numberStart - 1] == '-')
                --numberStart;

            if (numberStart == suffixPos)
            {
                searchFrom = suffixPos + std::strlen(kBytesSuffix);
                continue;
            }

            std::string numberText = output.substr(numberStart, suffixPos - numberStart);
            numberText.erase(
                std::remove(numberText.begin(), numberText.end(), ','),
                numberText.end());

            char* parseEnd = nullptr;
            const long long signedValue = std::strtoll(numberText.c_str(), &parseEnd, 10);
            if (!parseEnd || parseEnd == numberText.c_str() || *parseEnd != '\0')
            {
                searchFrom = suffixPos + std::strlen(kBytesSuffix);
                continue;
            }

            uint64_t byteCount = 0;
            if (signedValue < 0 && signedValue >= static_cast<long long>(INT32_MIN))
            {
                // cache_print_summary formats the unsigned 0x80000000 limit as
                // a signed int. Reinterpret that bit pattern before converting.
                byteCount = static_cast<uint32_t>(static_cast<int32_t>(signedValue));
            }
            else if (signedValue >= 0)
            {
                byteCount = static_cast<uint64_t>(signedValue);
            }
            else
            {
                searchFrom = suffixPos + std::strlen(kBytesSuffix);
                continue;
            }

            char replacement[64] = {};
            std::snprintf(
                replacement,
                sizeof(replacement),
                "%.2f MB",
                static_cast<double>(byteCount) / (1024.0 * 1024.0));

            const size_t replaceLength =
                suffixPos + std::strlen(kBytesSuffix) - numberStart;
            output.replace(numberStart, replaceLength, replacement);
            searchFrom = numberStart + std::strlen(replacement);
            changed = true;
        }

        changed = FixCacheSummaryNegativePercent(output, "% of limit") || changed;
        changed = FixCacheSummaryNegativePercent(output, "% of capacity") || changed;
        return changed;
    }

    using Tier0MsgFn = void(__cdecl*)(const char* format, ...);
    static Tier0MsgFn s_OriginalDataCacheMsg = nullptr;
    static void** s_DataCacheMsgIatSlot = nullptr;
    static bool s_DataCacheMsgIatHookInstalled = false;

    static void __cdecl HookedDataCacheMsg(const char* format, ...)
    {
        Tier0MsgFn original = s_OriginalDataCacheMsg;
        if (!original || !format)
            return;

        // datacache.dll uses tier0!Msg for cache_print_summary. Resolve its
        // varargs here, transform only the stable summary row formats, and send
        // the final text through the real Msg entry point as a plain string.
        char rendered[8192] = {};
        va_list args;
        va_start(args, format);
        _vsnprintf_s(rendered, _countof(rendered), _TRUNCATE, format, args);
        va_end(args);

        std::string transformed;
        if (FormatCacheSummaryBytesAsMiB(rendered, transformed))
            original("%s", transformed.c_str());
        else
            original("%s", rendered);
    }

    static void** FindImportedFunctionSlot(
        HMODULE module,
        const char* importedModuleName,
        const char* importedFunctionName)
    {
        if (!module || !importedModuleName || !importedFunctionName)
            return nullptr;

        __try
        {
            auto* base = reinterpret_cast<uint8_t*>(module);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return nullptr;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            {
                return nullptr;
            }

            const IMAGE_DATA_DIRECTORY& imports =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (imports.VirtualAddress == 0 || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
                return nullptr;

            auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                base + imports.VirtualAddress);
            for (; descriptor->Name != 0; ++descriptor)
            {
                const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
                if (_stricmp(moduleName, importedModuleName) != 0)
                    continue;

                if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0)
                    return nullptr;

                auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(
                    base + descriptor->OriginalFirstThunk);
                auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(
                    base + descriptor->FirstThunk);
                for (size_t index = 0; names[index].u1.AddressOfData != 0; ++index)
                {
                    if (IMAGE_SNAP_BY_ORDINAL32(names[index].u1.Ordinal))
                        continue;

                    auto* importedName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        base + names[index].u1.AddressOfData);
                    if (std::strcmp(
                        reinterpret_cast<const char*>(importedName->Name),
                        importedFunctionName) == 0)
                    {
                        return reinterpret_cast<void**>(&slots[index].u1.Function);
                    }
                }
                return nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }

        return nullptr;
    }

    static bool InstallDataCacheMsgIatHook()
    {
        if (s_DataCacheMsgIatHookInstalled)
            return true;

        HMODULE dataCache = GetModuleHandleA("datacache.dll");
        void** slot = FindImportedFunctionSlot(dataCache, "tier0.dll", "Msg");
        if (!slot || !*slot)
            return false;

        DWORD oldProtection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
            return false;

        Tier0MsgFn original = reinterpret_cast<Tier0MsgFn>(*slot);
        s_OriginalDataCacheMsg = original;
        InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(slot),
            reinterpret_cast<PVOID>(&HookedDataCacheMsg));

        DWORD ignoredProtection = 0;
        VirtualProtect(slot, sizeof(void*), oldProtection, &ignoredProtection);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

        s_DataCacheMsgIatSlot = slot;
        s_DataCacheMsgIatHookInstalled = (*slot == reinterpret_cast<void*>(&HookedDataCacheMsg));
        if (!s_DataCacheMsgIatHookInstalled)
            s_OriginalDataCacheMsg = nullptr;
        return s_DataCacheMsgIatHookInstalled;
    }

    static int __cdecl FilterVertexFormatWarningSpew(int spewType, const char* pMsg)
    {
        if (pMsg && std::strstr(pMsg, kVertexFormatSpamMessage) != nullptr)
            return 1; // SPEW_CONTINUE

        std::string formattedCacheSummary;
        const char* forwardedMessage = pMsg;
        if (FormatCacheSummaryBytesAsMiB(pMsg, formattedCacheSummary))
        {
            forwardedMessage = formattedCacheSummary.c_str();
        }

        if (s_OriginalSpewOutputFunc && s_OriginalSpewOutputFunc != &FilterVertexFormatWarningSpew)
            return s_OriginalSpewOutputFunc(spewType, forwardedMessage);

        return 1; // SPEW_CONTINUE
    }

    static bool NormalizeSuspiciousFloat(float candidate, float& normalized)
    {
        if (std::isfinite(candidate) && std::fabs(candidate) <= 1000000.0f)
        {
            normalized = candidate;
            return true;
        }

        const double rounded = std::nearbyint(static_cast<double>(candidate));
        if (std::isfinite(candidate) &&
            std::fabs(static_cast<double>(candidate) - rounded) <= 0.5 &&
            rounded >= 0.0 &&
            rounded <= static_cast<double>(UINT32_MAX))
        {
            const uint32_t rebits = static_cast<uint32_t>(rounded);
            float rebound = 0.0f;
            std::memcpy(&rebound, &rebits, sizeof(rebound));
            if (std::isfinite(rebound) && std::fabs(rebound) <= 1000000.0f)
            {
                normalized = rebound;
                return true;
            }
        }

        normalized = candidate;
        return false;
    }

    // Source SDK 2007/2009 style ConVar layout used by L4D2.
    // We only depend on the vtable order for SetValue(...) and the cached numeric fields for reads.
    class SourceConCommandBase
    {
    public:
        virtual ~SourceConCommandBase() = default;
        virtual bool IsCommand(void) const = 0;
        virtual bool IsFlagSet(int flag) const = 0;
        virtual void AddFlags(int flags) = 0;
        virtual const char* GetName(void) const = 0;
        virtual const char* GetHelpText(void) const = 0;
        virtual bool IsRegistered(void) const = 0;
        virtual int GetDLLIdentifier() const = 0;

    protected:
        virtual void CreateBase(const char* pName, const char* pHelpString = nullptr, int flags = 0) = 0;
        virtual void Init() = 0;

    public:
        SourceConCommandBase* m_pNext = nullptr;
        bool m_bRegistered = false;
        char m_PaddingRegistered[3] = {};
        const char* m_pszName = nullptr;
        const char* m_pszHelpString = nullptr;
        int m_nFlags = 0;
    };

    class SourceConVar : public SourceConCommandBase
    {
    public:
        virtual ~SourceConVar() = default;
        virtual bool IsFlagSet(int flag) const = 0;
        virtual const char* GetHelpText(void) const = 0;
        virtual bool IsRegistered(void) const = 0;
        virtual const char* GetName(void) const = 0;
        virtual void AddFlags(int flags) = 0;
        virtual bool IsCommand(void) const = 0;
        virtual void SetValue(const char* value) = 0;
        virtual void SetValue(float value) = 0;
        virtual void SetValue(int value) = 0;

    private:
        virtual void InternalSetValue(const char* value) = 0;
        virtual void InternalSetFloatValue(float value) = 0;
        virtual void InternalSetIntValue(int value) = 0;
        virtual bool ClampValue(float& value) = 0;
        virtual void ChangeStringValue(const char* value, float oldValue) = 0;
        virtual void Create_Vtbl(const char* pName, const char* pDefaultValue, int flags = 0,
            const char* pHelpString = nullptr, bool bMin = false, float fMin = 0.0f,
            bool bMax = false, float fMax = 0.0f, void* callback = nullptr) = 0;
        virtual void Init() = 0;
        virtual void InternalSetFloatValue2(float value, bool force = false) = 0;

    public:
        // ConVar derives from both ConCommandBase and IConVar in Source.
        // FindVar() returns the full ConVar object, so after the ConCommandBase
        // base subobject there is a second vptr for the IConVar base.
        void* m_pIConVarVTable = nullptr;
        SourceConVar* m_pParent = nullptr;
        const char* m_pszDefaultValue = nullptr;
        char* m_pszString = nullptr;
        int m_StringLength = 0;
        float m_fValue = 0.0f;
        int m_nValue = 0;
        bool m_bHasMin = false;
        char m_PaddingHasMin[3] = {};
        float m_fMinVal = 0.0f;
        bool m_bHasMax = false;
        char m_PaddingHasMax[3] = {};
        float m_fMaxVal = 0.0f;
        void* m_fnChangeCallback = nullptr;

        int GetIntValue() const
        {
            const SourceConVar* parent = (m_pParent != nullptr) ? m_pParent : this;
            if (parent->m_pszString && *parent->m_pszString)
            {
                char* end = nullptr;
                const long parsed = std::strtol(parent->m_pszString, &end, 10);
                if (end != parent->m_pszString)
                    return static_cast<int>(parsed);
            }

            const uintptr_t key = reinterpret_cast<uintptr_t>(parent);
            return static_cast<int>(parent->m_nValue ^ static_cast<int>(key));
        }

        int GetIntValueDirect() const
        {
            const SourceConVar* parent = (m_pParent != nullptr) ? m_pParent : this;
            return parent->m_nValue;
        }

        float GetFloatValue() const
        {
            const SourceConVar* parent = (m_pParent != nullptr) ? m_pParent : this;
            if (parent->m_pszString && *parent->m_pszString)
            {
                char* end = nullptr;
                const float parsed = std::strtof(parent->m_pszString, &end);
                if (end != parent->m_pszString)
                {
                    float normalized = parsed;
                    NormalizeSuspiciousFloat(parsed, normalized);
                    return normalized;
                }
            }

            const uintptr_t key = reinterpret_cast<uintptr_t>(parent);
            const uint32_t encodedBits = *reinterpret_cast<const uint32_t*>(&parent->m_fValue);
            const uint32_t decodedBits = encodedBits ^ static_cast<uint32_t>(key);
            float decoded = 0.0f;
            std::memcpy(&decoded, &decodedBits, sizeof(decoded));
            float normalized = decoded;
            NormalizeSuspiciousFloat(decoded, normalized);
            return normalized;
        }

        float GetFloatValueDirect() const
        {
            const SourceConVar* parent = (m_pParent != nullptr) ? m_pParent : this;
            float normalized = parent->m_fValue;
            NormalizeSuspiciousFloat(parent->m_fValue, normalized);
            return normalized;
        }
    };

    class SourceIConVar
    {
    public:
        virtual void SetValue(const char* value) = 0;
        virtual void SetValue(float value) = 0;
        virtual void SetValue(int value) = 0;
        virtual const char* GetName(void) const = 0;
        virtual bool IsFlagSet(int flag) const = 0;
    };

    class SourceICvar
    {
    public:
        virtual bool Connect(void* factory) = 0;
        virtual void Disconnect() = 0;
        virtual void* QueryInterface(const char* pInterfaceName) = 0;
        virtual int Init() = 0;
        virtual void Shutdown() = 0;
        virtual int AllocateDLLIdentifier() = 0;
        virtual void RegisterConCommand(void* pCommandBase) = 0;
        virtual void UnregisterConCommand(void* pCommandBase) = 0;
        virtual void UnregisterConCommands(int id) = 0;
        virtual const char* GetCommandLineValue(const char* pVariableName) = 0;
        virtual SourceConCommandBase* FindCommandBase(const char* name) = 0;
        virtual const SourceConCommandBase* FindCommandBase(const char* name) const = 0;
        virtual SourceConVar* FindVar(const char* varName) = 0;
    };

    // Small, stable interface exposed by engine.dll specifically for sending a
    // command to one client. A listen server can use it directly from d3d9.dll.
    class SourceIServerPluginHelpersPoseRelay
    {
    public:
        virtual void CreateMessage(
            edict_t*,
            int,
            void*,
            void*) = 0;
        virtual void ClientCommand(edict_t*, const char*) = 0;
        virtual int StartQueryCvarValue(edict_t*, const char*) = 0;
    };

    class SourceCCommand
    {
    public:
        static constexpr int kMaxArgCount = 64;
        static constexpr int kMaxCommandLength = 512;

        int ArgC() const
        {
            return (m_nArgc >= 0 && m_nArgc <= kMaxArgCount)
                ? m_nArgc
                : 0;
        }

        const char* Arg(int index) const
        {
            if (index < 0 || index >= ArgC())
                return "";
            const char* value = m_ppArgv[index];
            return value ? value : "";
        }

    private:
        int m_nArgc = 0;
        int m_nArgv0Size = 0;
        char m_pArgSBuffer[kMaxCommandLength]{};
        char m_pArgvBuffer[kMaxCommandLength]{};
        const char* m_ppArgv[kMaxArgCount]{};
    };

    class SourceRegisteredConCommandBase
    {
    public:
        virtual ~SourceRegisteredConCommandBase() = default;
        virtual bool IsCommand() const { return true; }
        virtual bool IsFlagSet(int flag) const { return (m_nFlags & flag) != 0; }
        virtual void AddFlags(int flags) { m_nFlags |= flags; }
        virtual void RemoveFlags(int flags) { m_nFlags &= ~flags; }
        virtual int GetFlags() const { return m_nFlags; }
        virtual const char* GetName() const { return m_pszName; }
        virtual const char* GetHelpText() const { return m_pszHelpString ? m_pszHelpString : ""; }
        virtual bool IsRegistered() const { return m_bRegistered; }
        virtual int GetDLLIdentifier() const { return 0; }

    protected:
        virtual void Create(const char* name, const char* helpString = nullptr, int flags = 0)
        {
            m_pszName = name;
            m_pszHelpString = helpString;
            m_nFlags = flags;
        }

        virtual void Init() {}

    public:
        SourceRegisteredConCommandBase* m_pNext = nullptr;
        bool m_bRegistered = false;
        char m_PaddingRegistered[3] = {};
        const char* m_pszName = nullptr;
        const char* m_pszHelpString = nullptr;
        int m_nFlags = 0;
    };

    class SourceRegisteredConCommand : public SourceRegisteredConCommandBase
    {
    public:
        using Callback = void(__cdecl*)(const SourceCCommand&);

        SourceRegisteredConCommand(const char* name, Callback callback, const char* helpString, int flags)
            : m_Callback(callback)
        {
            Create(name, helpString, flags);
        }

        bool IsCommand() const override { return true; }
        virtual int AutoCompleteSuggest(const char*, void*) { return 0; }
        virtual bool CanAutoComplete() { return false; }
        virtual void Dispatch(const SourceCCommand& command)
        {
            if (m_Callback)
                m_Callback(command);
        }

    private:
        Callback m_Callback = nullptr;
        void* m_CompletionCallback = nullptr;
        bool m_HasCompletionCallback = false;
        bool m_UsingNewCommandCallback = true;
        bool m_UsingCommandCallbackInterface = false;
    };

    struct SourceRecvTable;

    struct SourceRecvProp
    {
        const char* m_pVarName = nullptr;
        int m_RecvType = 0;
        int m_Flags = 0;
        int m_StringBufferSize = 0;
        bool m_bInsideArray = false;
        const void* m_pExtraData = nullptr;
        SourceRecvProp* m_pArrayProp = nullptr;
        void* m_ArrayLengthProxy = nullptr;
        void* m_ProxyFn = nullptr;
        void* m_DataTableProxyFn = nullptr;
        SourceRecvTable* m_pDataTable = nullptr;
        int m_Offset = 0;
        int m_ElementStride = 0;
        int m_nElements = 0;
        const char* m_pParentArrayPropName = nullptr;
    };

    struct SourceRecvTable
    {
        SourceRecvProp* m_pProps = nullptr;
        int m_nProps = 0;
        void* m_pDecoder = nullptr;
        const char* m_pNetTableName = nullptr;
        bool m_bInitialized = false;
        bool m_bInMainList = false;
    };

    struct SourceClientClass
    {
        void* m_pCreateFn = nullptr;
        void* m_pCreateEventFn = nullptr;
        const char* m_pNetworkName = nullptr;
        SourceRecvTable* m_pRecvTable = nullptr;
        SourceClientClass* m_pNext = nullptr;
        int m_ClassID = 0;
    };

    class SourceBaseClientDLL
    {
    public:
        virtual int Connect(void* appSystemFactory, void* pGlobals) = 0;
        virtual int Disconnect(void) = 0;
        virtual int Init(void* appSystemFactory, void* pGlobals) = 0;
        virtual void PostInit() = 0;
        virtual void Shutdown(void) = 0;
        virtual void LevelInitPreEntity(char const* pMapName) = 0;
        virtual void LevelInitPostEntity() = 0;
        virtual void LevelFastReload(void) = 0;
        virtual void LevelShutdown(void) = 0;
        virtual void* GetAllClasses(void) = 0;
    };
}

static_assert(sizeof(SourceConCommandBase) == 24, "Unexpected ConCommandBase size for Source IConVar bridge.");

namespace
{
    struct ScopedConVarWritePermit
    {
        ScopedConVarWritePermit()
        {
            ++s_ConVarWritePermitDepth;
        }

        ~ScopedConVarWritePermit()
        {
            if (s_ConVarWritePermitDepth > 0)
                --s_ConVarWritePermitDepth;
        }
    };
}

static int FindRecvPropOffsetRecursive(const SourceRecvTable* table, const char* propName, int accumulatedOffset);
static int FindRecvPropOffsetSafe(void* baseClientDll, const char* networkName, const char* propName);

// === Utility: Retry module load with logging ===
static HMODULE GetModuleWithRetry(const char* dllname, std::chrono::milliseconds timeout = std::chrono::seconds(30), int delayMs = 50)
{
    const auto start = std::chrono::steady_clock::now();
    int attempt = 0;

    while (true)
    {
        HMODULE handle = GetModuleHandleA(dllname);
        if (handle)
            return handle;

        ++attempt;
        Sleep(delayMs);

        if (timeout.count() >= 0 && std::chrono::steady_clock::now() - start >= timeout)
            break;
    }

    Game::errorMsg(("Failed to load module after retrying: " + std::string(dllname)).c_str());
    return nullptr;
}

// === Utility: Safe interface fetch with static cache ===
static void* GetInterfaceSafe(const char* dllname, const char* interfacename)
{
    static std::unordered_map<std::string, void*> cache;

    std::string key = std::string(dllname) + "::" + interfacename;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
    {
        Game::errorMsg(("CreateInterface not found in " + std::string(dllname)).c_str());
        return nullptr;
    }

    int returnCode = 0;
    void* iface = CreateInterface(interfacename, &returnCode);
    if (!iface)
    {
        Game::errorMsg(("Interface not found: " + std::string(interfacename)).c_str());
        return nullptr;
    }

    cache[key] = iface;
    return iface;
}

// === Utility: Attempt interface fetch without error logging ===
static void* TryInterfaceNoError(const char* dllname, const char* interfacename)
{
    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
        return nullptr;

    int returnCode = 0;
    return CreateInterface(interfacename, &returnCode);
}

namespace
{
    constexpr int kFcvarServerCanExecute = (1 << 28);
    constexpr char kL4D2VRServerAckCommandName[] = "l4d2vr_server_ack";
    constexpr char kL4D2VRPoseAckCommandName[] = "l4d2vr_pose_ack";
    constexpr char kL4D2VRPoseReceiveCommandName[] = "l4d2vr_pose_receive";
    constexpr char kVRConfigCommandName[] = "vrconfig";
    constexpr char kCachePrintSummaryCommandName[] = "cache_print_summary";
    SourceRegisteredConCommand* g_OriginalCachePrintSummaryCommand = nullptr;

    bool ParseUnsignedCommandArgument(
        const SourceCCommand& command,
        int argumentIndex,
        unsigned long maximum,
        unsigned long& outValue)
    {
        outValue = 0;
        const char* text = command.Arg(argumentIndex);
        if (!text || !*text)
            return false;

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 10);
        if (!end || end == text || *end != '\0' || parsed > maximum)
            return false;

        outValue = parsed;
        return true;
    }

    void __cdecl OnL4D2VRServerAckCommand(const SourceCCommand& command)
    {
        const bool wasKnown = Hooks::s_ServerUnderstandsVR;
        Hooks::s_ServerUnderstandsVR = true;

        if (g_Game && g_Game->m_VR)
        {
            VR* vr = g_Game->m_VR;
            vr->m_ServerHookFallbackPending = false;
            vr->m_ServerHookFallbackForcedNonVRServerMovement = false;
            vr->m_ServerHookFallbackCheckTime = {};
            vr->m_ForceNonVRServerMovement = vr->m_ConfigForceNonVRServerMovement;
        }

        if (!wasKnown)
            Game::logMsg("[VR][ServerAck] dedicated server plugin acknowledged VR usercmd support");

        // Combined-server protocol 3+ advertises pose wire protocol 2.
        // Version 2 of that plugin relays the older 40-byte pose packet and
        // must not arm this protocol-v2 uploader.
        unsigned long protocolVersion = 0;
        if (g_Game &&
            ParseUnsignedCommandArgument(command, 1, 255u, protocolVersion) &&
            protocolVersion >= 3u)
        {
            g_Game->HandleVRPoseServerAck(
                static_cast<int>(l4d2vr_pose::kVersion));
        }
    }

    void __cdecl OnL4D2VRPoseAckCommand(const SourceCCommand& command)
    {
        unsigned long protocolVersion = 0;
        if (!g_Game ||
            !ParseUnsignedCommandArgument(command, 1, 255u, protocolVersion))
        {
            return;
        }

        g_Game->HandleVRPoseServerAck(
            static_cast<int>(protocolVersion));
    }

    void __cdecl OnL4D2VRPoseReceiveCommand(const SourceCCommand& command)
    {
        unsigned long playerIndex = 0;
        unsigned long sequence = 0;
        if (!g_Game ||
            command.ArgC() != 4 ||
            !ParseUnsignedCommandArgument(command, 1, 64u, playerIndex) ||
            !ParseUnsignedCommandArgument(command, 2, 65535u, sequence))
        {
            return;
        }

        g_Game->ReceiveVRPosePayload(
            static_cast<int>(playerIndex),
            static_cast<std::uint16_t>(sequence),
            command.Arg(3),
            true,
            std::numeric_limits<float>::quiet_NaN());
    }

    void __cdecl OnVRConfigCommand(const SourceCCommand&)
    {
        L4D2VRConfigOverlay_Open();
    }

    void __cdecl OnCachePrintSummaryCommand(const SourceCCommand& command)
    {
        if (!g_OriginalCachePrintSummaryCommand)
        {
            Game::logMsg("[VR][DataCache] original cache_print_summary command is unavailable");
            return;
        }

        g_OriginalCachePrintSummaryCommand->Dispatch(command);
    }

    SourceRegisteredConCommand g_L4D2VRServerAckCommand(
        kL4D2VRServerAckCommandName,
        &OnL4D2VRServerAckCommand,
        "Accepts L4D2VR dedicated server plugin acknowledgement.",
        kFcvarServerCanExecute);
    SourceRegisteredConCommand g_L4D2VRPoseAckCommand(
        kL4D2VRPoseAckCommandName,
        &OnL4D2VRPoseAckCommand,
        "Accepts L4D2VR world-pose relay acknowledgement.",
        kFcvarServerCanExecute);
    SourceRegisteredConCommand g_L4D2VRPoseReceiveCommand(
        kL4D2VRPoseReceiveCommandName,
        &OnL4D2VRPoseReceiveCommand,
        "Receives a relayed L4D2VR world-model pose.",
        kFcvarServerCanExecute);
    SourceRegisteredConCommand g_VRConfigCommand(
        kVRConfigCommandName,
        &OnVRConfigCommand,
        "Opens the L4D2VR configuration overlay.",
        0);
    SourceRegisteredConCommand g_CachePrintSummaryCommand(
        kCachePrintSummaryCommandName,
        &OnCachePrintSummaryCommand,
        "Prints the engine datacache summary using MB units.",
        0);

    bool g_L4D2VRCommandsRegistered = false;

    void RegisterL4D2VRCommands(void* cvarIface)
    {
        if (!cvarIface || g_L4D2VRCommandsRegistered)
            return;

        SourceICvar* cvar = reinterpret_cast<SourceICvar*>(cvarIface);
        __try
        {
            if (!cvar->FindCommandBase(kL4D2VRServerAckCommandName))
                cvar->RegisterConCommand(&g_L4D2VRServerAckCommand);
            if (!cvar->FindCommandBase(kL4D2VRPoseAckCommandName))
                cvar->RegisterConCommand(&g_L4D2VRPoseAckCommand);
            if (!cvar->FindCommandBase(kL4D2VRPoseReceiveCommandName))
                cvar->RegisterConCommand(&g_L4D2VRPoseReceiveCommand);
            if (!cvar->FindCommandBase(kVRConfigCommandName))
                cvar->RegisterConCommand(&g_VRConfigCommand);

            SourceConCommandBase* cachePrintSummary =
                cvar->FindCommandBase(kCachePrintSummaryCommandName);
            if (cachePrintSummary &&
                cachePrintSummary != reinterpret_cast<SourceConCommandBase*>(&g_CachePrintSummaryCommand))
            {
                g_OriginalCachePrintSummaryCommand =
                    reinterpret_cast<SourceRegisteredConCommand*>(cachePrintSummary);
                g_CachePrintSummaryCommand.m_nFlags = cachePrintSummary->m_nFlags;
                cvar->UnregisterConCommand(cachePrintSummary);
                cvar->RegisterConCommand(&g_CachePrintSummaryCommand);
            }
            g_L4D2VRCommandsRegistered = true;
            Game::logMsg(
                "[VR][Commands] registered commands: %s, %s, %s, %s; cache summary hook=%d",
                kL4D2VRServerAckCommandName,
                kL4D2VRPoseAckCommandName,
                kL4D2VRPoseReceiveCommandName,
                kVRConfigCommandName,
                g_OriginalCachePrintSummaryCommand ? 1 : 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Game::logMsg("[VR][WorldPose] failed to register client commands");
        }
    }
}

// === Game Constructor ===
Game::Game()
{
    m_BaseClient = reinterpret_cast<uintptr_t>(GetModuleWithRetry("client.dll"));
    m_BaseEngine = reinterpret_cast<uintptr_t>(GetModuleWithRetry("engine.dll"));
    m_BaseMaterialSystem = reinterpret_cast<uintptr_t>(GetModuleWithRetry("MaterialSystem.dll"));
    m_BaseServer = reinterpret_cast<uintptr_t>(GetModuleWithRetry("server.dll"));
    m_BaseVgui2 = reinterpret_cast<uintptr_t>(GetModuleWithRetry("vgui2.dll"));

    m_BaseClientDll = static_cast<IBaseClientDLL*>(GetInterfaceSafe("client.dll", "VClient016"));
    m_ClientEntityList = static_cast<IClientEntityList*>(GetInterfaceSafe("client.dll", "VClientEntityList003"));
    m_EngineTrace = static_cast<IEngineTrace*>(GetInterfaceSafe("engine.dll", "EngineTraceClient003"));
    m_EngineTraceServer = static_cast<IEngineTrace*>(TryInterfaceNoError("engine.dll", "EngineTraceServer003"));
    m_EngineClient = static_cast<IEngineClient*>(GetInterfaceSafe("engine.dll", "VEngineClient013"));
    m_EngineSound = TryInterfaceNoError("engine.dll", "IEngineSoundClient003");
    m_ServerPluginHelpers = TryInterfaceNoError(
        "engine.dll",
        "ISERVERPLUGINHELPERS001");
    m_ServerGameClients = TryInterfaceNoError(
        "server.dll",
        "ServerGameClients003");
    m_GameEventManager = static_cast<IGameEventManager2*>(TryInterfaceNoError("engine.dll", "GAMEEVENTSMANAGER002"));
    if (!m_GameEventManager)
        m_GameEventManager = static_cast<IGameEventManager2*>(TryInterfaceNoError("engine.dll", "GAMEEVENTSMANAGER001"));
    m_MaterialSystem = static_cast<IMaterialSystem*>(GetInterfaceSafe("MaterialSystem.dll", "VMaterialSystem080"));
    m_ModelInfo = static_cast<IModelInfo*>(GetInterfaceSafe("engine.dll", "VModelInfoClient004"));
    m_ModelRender = static_cast<IModelRender*>(GetInterfaceSafe("engine.dll", "VEngineModel016"));
    m_VguiInput = static_cast<IInput*>(GetInterfaceSafe("vgui2.dll", "VGUI_InputInternal001"));
    m_VguiSurface = static_cast<ISurface*>(GetInterfaceSafe("vguimatsurface.dll", "VGUI_Surface031"));
    m_DebugOverlay = static_cast<IVDebugOverlay*>(TryInterfaceNoError("engine.dll", "VDebugOverlay003"));
    if (!m_DebugOverlay)
        m_DebugOverlay = static_cast<IVDebugOverlay*>(TryInterfaceNoError("engine.dll", "VDebugOverlay004"));
    m_Cvar = TryInterfaceNoError("vstdlib.dll", "VEngineCvar007");
    if (!m_Cvar)
        m_Cvar = TryInterfaceNoError("vstdlib.dll", "VEngineCvar006");
    if (!m_Cvar)
        m_Cvar = TryInterfaceNoError("vstdlib.dll", "VEngineCvar004");
    RegisterL4D2VRCommands(m_Cvar);
    const bool dataCacheMsgHookInstalled = InstallDataCacheMsgIatHook();
    Game::logMsg(
        "[VR][DataCache] cache_print_summary direct Msg hook=%d slot=%p",
        dataCacheMsgHookInstalled ? 1 : 0,
        s_DataCacheMsgIatSlot);

    m_Offsets = new Offsets();
    m_VR = new VR(this);
    L4D2VRConfigOverlay_StartWorker();

    ResetAllPlayerVRInfo();

    m_Hooks = new Hooks(this);

    InstallVertexFormatWarningFilter();
    m_Initialized = true;

}

// === Fallback Interface ===
void* Game::GetInterface(const char* dllname, const char* interfacename)
{
    return GetInterfaceSafe(dllname, interfacename);
}

// === Thread-safe Log Message with Timestamp ===
void Game::logMsg(const char* fmt, ...)
{
    if (fmt && std::strncmp(fmt, "[VR][DesktopHUD]", 16) == 0)
        return;
    if (fmt &&
        (std::strncmp(fmt, "[VR][UseAim]", 12) == 0 ||
            std::strncmp(fmt, "[VR][MagazineInteraction]", 25) == 0 ||
            std::strncmp(fmt, "[VR][MagazineInteractionFresh]", 30) == 0))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(logMutex);
    std::call_once(logResetOnce, []()
        {
            FILE* file = fopen("vrmod_log.txt", "w");
            if (file)
                fclose(file);
        });

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    char timebuf[20] = {};
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));

    printf("[%s] ", timebuf);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");

    const char* logFiles[] = { "vrmod_log.txt"};
    for (const char* logFile : logFiles)
    {
        FILE* file = fopen(logFile, "a");
        if (!file)
            continue;

        fprintf(file, "[%s] ", timebuf);
        va_list args2;
        va_start(args2, fmt);
        vfprintf(file, fmt, args2);
        va_end(args2);
        fprintf(file, "\n");
        fclose(file);
    }
}

// === Error Message ===
void Game::errorMsg(const char* msg)
{
    logMsg("[ERROR] %s", msg);
    MessageBoxA(nullptr, msg, "L4D2VR Error", MB_ICONERROR | MB_OK);
}

bool Game::InstallVertexFormatWarningFilter()
{
    if (s_VertexFormatWarningFilterInstalled)
        return true;

    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!tier0)
        tier0 = GetModuleHandleA("tier0_s.dll");
    if (!tier0)
        return false;

    if (!s_SetSpewOutputFunc)
    {
        s_SetSpewOutputFunc = reinterpret_cast<tSetSpewOutputFunc>(
            GetProcAddress(tier0, "SpewOutputFunc"));
    }
    if (!s_GetSpewOutputFunc)
    {
        s_GetSpewOutputFunc = reinterpret_cast<tGetSpewOutputFunc>(
            GetProcAddress(tier0, "GetSpewOutputFunc"));
    }

    if (!s_SetSpewOutputFunc)
        return false;

    SpewOutputFunc_t current = s_GetSpewOutputFunc ? s_GetSpewOutputFunc() : nullptr;
    if (current == &FilterVertexFormatWarningSpew)
    {
        s_VertexFormatWarningFilterInstalled = true;
        return true;
    }

    s_OriginalSpewOutputFunc = current;
    SpewOutputFunc_t previous = s_SetSpewOutputFunc(&FilterVertexFormatWarningSpew);
    if (!s_OriginalSpewOutputFunc)
        s_OriginalSpewOutputFunc = previous;

    s_VertexFormatWarningFilterInstalled = true;
    return true;
}

void Game::UninstallVertexFormatWarningFilter()
{
    if (!s_VertexFormatWarningFilterInstalled || !s_SetSpewOutputFunc)
        return;

    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!tier0)
        tier0 = GetModuleHandleA("tier0_s.dll");
    if (!tier0)
        return;

    s_SetSpewOutputFunc(s_OriginalSpewOutputFunc);
    s_VertexFormatWarningFilterInstalled = false;
}

bool Game::IsValidPlayerIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(m_PlayersVRInfo.size());
}

void Game::ResetAllPlayerVRInfo()
{
    {
        std::lock_guard<std::mutex> lock(m_VRPoseMutex);
        m_PlayersVRInfo.fill(Player{});
        m_VRPoseServerCapable.store(false, std::memory_order_release);
        m_VRPoseHelloSent.store(false, std::memory_order_release);
        m_VRPoseLastLocalPublishTickMs = 0u;
        m_VRPoseLocalSequence = 0u;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(
            m_BuiltinVRPoseRelayMutex);
        m_BuiltinVRPoseRelayClients.fill(VRPoseRelayServerClient{});
    }
}

void Game::ResetVRPoseServerSession()
{
    {
        std::lock_guard<std::mutex> lock(m_VRPoseMutex);
        for (Player& player : m_PlayersVRInfo)
        {
            player.previousWorldPose = VRPoseFrame{};
            player.latestWorldPose = VRPoseFrame{};
            player.worldPoseBlendWeight = 0.0f;
            player.worldPoseBlendTickMs = 0u;
            player.worldPoseUserID = -1;
            player.pPlayer = nullptr;
        }
        m_VRPoseServerCapable.store(false, std::memory_order_release);
        m_VRPoseHelloSent.store(false, std::memory_order_release);
        m_VRPoseLastLocalPublishTickMs = 0u;
        m_VRPoseLocalSequence = 0u;
    }
    {
        // On a listen server this also ends the current relay session. Edict
        // slots and packet sequences may be reused on the next map.
        std::lock_guard<std::recursive_mutex> lock(
            m_BuiltinVRPoseRelayMutex);
        m_BuiltinVRPoseRelayClients.fill(VRPoseRelayServerClient{});
    }
}

namespace
{
    constexpr int kVRPoseRelayMaxPlayerIndex = 64;
    constexpr int kVRPoseRelayMaxAckAttempts = 5;
    constexpr std::uint64_t kVRPoseRelayAckIntervalMs = 1000u;
    constexpr std::uint64_t kVRPoseRelayUploadSilenceForAckMs = 1500u;
    constexpr std::uint64_t kVRPoseRelayMinimumDecodeIntervalUs = 16667u;
    constexpr int kSourceEdictFreeFlag = (1 << 1);
    constexpr char kVRPoseRelayHelloCommand[] = "l4d2vr_pose_hello";
    constexpr char kVRPoseRelayUploadCommand[] = "l4d2vr_pose_upload";
    constexpr char kVRPoseRelayAckCommand[] = "l4d2vr_pose_ack 2\n";

    struct VRPoseRelayCommandView
    {
        int argumentCount = 0;
        const char* name = "";
        const char* argument1 = "";
        const char* argument2 = "";
    };

    std::uint64_t VRPoseTickMs()
    {
        return static_cast<std::uint64_t>(GetTickCount64());
    }

    std::uint64_t VRPoseTickUs()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    bool VRPoseRelayReadCommand(
        const void* sourceCommand,
        VRPoseRelayCommandView& outCommand)
    {
        outCommand = {};
        if (!sourceCommand)
            return false;
#ifdef _MSC_VER
        __try
        {
#endif
            const SourceCCommand& command =
                *static_cast<const SourceCCommand*>(sourceCommand);
            outCommand.argumentCount = command.ArgC();
            outCommand.name = command.Arg(0);
            outCommand.argument1 = command.Arg(1);
            outCommand.argument2 = command.Arg(2);
            return true;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outCommand = {};
            return false;
        }
#endif
    }

    bool VRPoseRelayReadEdictIdentity(
        edict_t* entity,
        int& outPlayerIndex,
        std::int16_t& outSerial)
    {
        outPlayerIndex = -1;
        outSerial = 0;
        if (!entity)
            return false;
#ifdef _MSC_VER
        __try
        {
#endif
            if ((entity->m_fStateFlags & kSourceEdictFreeFlag) != 0 ||
                !entity->m_pUnk)
            {
                return false;
            }
            outPlayerIndex = static_cast<int>(entity->m_EdictIndex);
            outSerial = entity->m_NetworkSerialNumber;
            return outPlayerIndex >= 1 &&
                outPlayerIndex <= kVRPoseRelayMaxPlayerIndex;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outPlayerIndex = -1;
            outSerial = 0;
            return false;
        }
#endif
    }

    bool VRPoseRelaySendClientCommand(
        void* serverPluginHelpers,
        edict_t* entity,
        const char* command)
    {
        if (!serverPluginHelpers || !entity || !command || !*command)
            return false;
#ifdef _MSC_VER
        __try
        {
#endif
            static_cast<SourceIServerPluginHelpersPoseRelay*>(
                serverPluginHelpers)->ClientCommand(entity, command);
            return true;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    bool VRPoseRelayParseSequence(
        const char* text,
        std::uint16_t& outSequence)
    {
        outSequence = 0u;
        if (!text || !*text)
            return false;

        std::uint32_t value = 0u;
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(text);
             *cursor;
             ++cursor)
        {
            if (*cursor < '0' || *cursor > '9')
                return false;
            value =
                value * 10u +
                static_cast<std::uint32_t>(*cursor - '0');
            if (value > 0xFFFFu)
                return false;
        }

        outSequence = static_cast<std::uint16_t>(value);
        return true;
    }

    float VRPoseNormalizeAngle(float angle)
    {
        if (!std::isfinite(angle))
            return 0.0f;
        angle -= 360.0f * std::floor((angle + 180.0f) / 360.0f);
        return angle;
    }

    float VRPoseAngleLerp(float from, float to, float fraction)
    {
        const float delta = VRPoseNormalizeAngle(to - from);
        return VRPoseNormalizeAngle(from + delta * fraction);
    }

    bool VRPoseFiniteVector(const Vector& value)
    {
        return std::isfinite(value.x) &&
            std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    bool VRPoseFiniteAngles(const QAngle& value)
    {
        return std::isfinite(value.x) &&
            std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    std::int16_t VRPosePackPosition(float value)
    {
        if (!std::isfinite(value))
            return 0;
        const float scaled = value / l4d2vr_pose::kPositionUnitsPerStep;
        const long rounded = std::lround(std::clamp(
            scaled,
            -32767.0f,
            32767.0f));
        return static_cast<std::int16_t>(rounded);
    }

    float VRPoseUnpackPosition(std::int16_t value)
    {
        return static_cast<float>(value) *
            l4d2vr_pose::kPositionUnitsPerStep;
    }

    std::int16_t VRPosePackAngle(float value)
    {
        const float normalized = VRPoseNormalizeAngle(value);
        const long rounded = std::lround(std::clamp(
            normalized * (32767.0f / 180.0f),
            -32767.0f,
            32767.0f));
        return static_cast<std::int16_t>(rounded);
    }

    float VRPoseUnpackAngle(std::int16_t value)
    {
        return VRPoseNormalizeAngle(
            static_cast<float>(value) * (180.0f / 32767.0f));
    }

    std::uint8_t VRPosePackFingerCurl(float value)
    {
        if (!std::isfinite(value))
            return 0u;
        const float normalized = std::clamp(
            value / l4d2vr_pose::kFingerCurlMaximum,
            0.0f,
            1.0f);
        return static_cast<std::uint8_t>(std::lround(normalized * 255.0f));
    }

    float VRPoseUnpackFingerCurl(std::uint8_t value)
    {
        return static_cast<float>(value) *
            (l4d2vr_pose::kFingerCurlMaximum / 255.0f);
    }

    Vector VRPoseWorldPositionToBodyLocal(
        const Vector& worldPosition,
        const Vector& playerOrigin,
        float bodyYaw)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(
            QAngle(0.0f, bodyYaw, 0.0f),
            &forward,
            &right,
            &up);
        const Vector delta = worldPosition - playerOrigin;
        return Vector(
            DotProduct(delta, forward),
            DotProduct(delta, right),
            DotProduct(delta, up));
    }

    QAngle VRPoseWorldAnglesToBodyLocal(
        const QAngle& worldAngles,
        float bodyYaw)
    {
        return QAngle(
            VRPoseNormalizeAngle(worldAngles.x),
            VRPoseNormalizeAngle(worldAngles.y - bodyYaw),
            VRPoseNormalizeAngle(worldAngles.z));
    }

    void VRPosePackTrackedPose(
        const VRTrackedPoseLocal& source,
        l4d2vr_pose::PackedTrackedPose& destination)
    {
        destination.position[0] = VRPosePackPosition(source.position.x);
        destination.position[1] = VRPosePackPosition(source.position.y);
        destination.position[2] = VRPosePackPosition(source.position.z);
        destination.rotation[0] = VRPosePackAngle(source.angles.x);
        destination.rotation[1] = VRPosePackAngle(source.angles.y);
        destination.rotation[2] = VRPosePackAngle(source.angles.z);
    }

    VRTrackedPoseLocal VRPoseUnpackTrackedPose(
        const l4d2vr_pose::PackedTrackedPose& source)
    {
        VRTrackedPoseLocal result{};
        result.position = Vector(
            VRPoseUnpackPosition(source.position[0]),
            VRPoseUnpackPosition(source.position[1]),
            VRPoseUnpackPosition(source.position[2]));
        result.angles = QAngle(
            VRPoseUnpackAngle(source.rotation[0]),
            VRPoseUnpackAngle(source.rotation[1]),
            VRPoseUnpackAngle(source.rotation[2]));
        return result;
    }

    bool VRPoseTrackedPointSane(const VRTrackedPoseLocal& pose)
    {
        // A tracked point cannot legitimately be more than roughly twelve
        // metres from the player origin. Reject larger values before they can
        // generate extreme observer-side bone matrices.
        return VRPoseFiniteVector(pose.position) &&
            VRPoseFiniteAngles(pose.angles) &&
            pose.position.LengthSqr() <= (512.0f * 512.0f);
    }

    bool VRPoseReadPlayerTransform(
        C_BasePlayer* player,
        Vector& outOrigin,
        QAngle& outAngles)
    {
        if (!player)
            return false;
#ifdef _MSC_VER
        __try
        {
            outOrigin = player->GetAbsOrigin();
            outAngles = player->GetAbsAngles();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        outOrigin = player->GetAbsOrigin();
        outAngles = player->GetAbsAngles();
        return true;
#endif
    }

    void VRPoseReadPlayerIdentity(
        Game* game,
        int playerIndex,
        C_BasePlayer*& outPlayer,
        int& outUserID)
    {
        outPlayer = nullptr;
        outUserID = -1;
        if (!game || playerIndex <= 0)
            return;
#ifdef _MSC_VER
        __try
        {
#endif
            if (game->m_ClientEntityList)
            {
                outPlayer = reinterpret_cast<C_BasePlayer*>(
                    game->m_ClientEntityList->GetClientEntity(
                        playerIndex));
            }
            if (game->m_EngineClient)
            {
                player_info_t info{};
                if (game->m_EngineClient->GetPlayerInfo(
                        playerIndex,
                        &info))
                {
                    outUserID = info.userID;
                }
            }
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outPlayer = nullptr;
            outUserID = -1;
        }
#endif
    }

    VRTrackedPoseLocal VRPoseInterpolateTrackedPoint(
        const VRTrackedPoseLocal& previous,
        const VRTrackedPoseLocal& latest,
        float fraction)
    {
        VRTrackedPoseLocal result{};
        result.position =
            previous.position + (latest.position - previous.position) * fraction;
        result.angles.x =
            VRPoseAngleLerp(previous.angles.x, latest.angles.x, fraction);
        result.angles.y =
            VRPoseAngleLerp(previous.angles.y, latest.angles.y, fraction);
        result.angles.z =
            VRPoseAngleLerp(previous.angles.z, latest.angles.z, fraction);
        return result;
    }

    std::array<float, 5> VRPoseInterpolateFingerCurls(
        const std::array<float, 5>& previous,
        const std::array<float, 5>& latest,
        float fraction)
    {
        std::array<float, 5> result{};
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        for (size_t finger = 0; finger < result.size(); ++finger)
        {
            result[finger] = std::clamp(
                previous[finger] +
                    (latest[finger] - previous[finger]) * fraction,
                0.0f,
                l4d2vr_pose::kFingerCurlMaximum);
        }
        return result;
    }

    VRTrackedPoseLocal VRPoseRebaseTrackedPointYaw(
        const VRTrackedPoseLocal& pose,
        float fromBodyYaw,
        float toBodyYaw)
    {
        VRTrackedPoseLocal result = pose;
        Vector fromForward{};
        Vector fromRight{};
        Vector fromUp{};
        Vector toForward{};
        Vector toRight{};
        Vector toUp{};
        QAngle::AngleVectors(
            QAngle(0.0f, fromBodyYaw, 0.0f),
            &fromForward,
            &fromRight,
            &fromUp);
        QAngle::AngleVectors(
            QAngle(0.0f, toBodyYaw, 0.0f),
            &toForward,
            &toRight,
            &toUp);
        const Vector worldOffset =
            fromForward * pose.position.x +
            fromRight * pose.position.y +
            fromUp * pose.position.z;
        result.position = Vector(
            DotProduct(worldOffset, toForward),
            DotProduct(worldOffset, toRight),
            DotProduct(worldOffset, toUp));
        result.angles.y = VRPoseNormalizeAngle(
            pose.angles.y + fromBodyYaw - toBodyYaw);
        return result;
    }
}

void Game::ObserveBuiltinVRPoseRelayClient(
    int playerIndex,
    edict_t* entity)
{
    if (!m_ServerPluginHelpers ||
        !m_ServerGameClients ||
        playerIndex < 1 ||
        playerIndex > kVRPoseRelayMaxPlayerIndex ||
        !entity)
    {
        return;
    }

    int edictPlayerIndex = -1;
    std::int16_t edictSerial = 0;
    if (!VRPoseRelayReadEdictIdentity(
            entity,
            edictPlayerIndex,
            edictSerial) ||
        edictPlayerIndex != playerIndex)
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(
        m_BuiltinVRPoseRelayMutex);
    VRPoseRelayServerClient& client =
        m_BuiltinVRPoseRelayClients[playerIndex];
    if (client.entity != entity ||
        client.edictSerial != edictSerial)
    {
        client = VRPoseRelayServerClient{};
        client.entity = entity;
        client.edictSerial = edictSerial;
    }

    const std::uint64_t nowMs = VRPoseTickMs();
    const bool uploadIsSilent =
        client.lastUploadTickMs == 0u ||
        nowMs - client.lastUploadTickMs >=
            kVRPoseRelayUploadSilenceForAckMs;
    if (!uploadIsSilent ||
        client.ackAttempts >= kVRPoseRelayMaxAckAttempts ||
        (client.lastAckTickMs != 0u &&
         nowMs - client.lastAckTickMs < kVRPoseRelayAckIntervalMs))
    {
        return;
    }

    client.lastAckTickMs = nowMs;
    ++client.ackAttempts;
    if (VRPoseRelaySendClientCommand(
            m_ServerPluginHelpers,
            entity,
            kVRPoseRelayAckCommand) &&
        client.ackAttempts == 1)
    {
        Game::logMsg(
            "[VR][WorldPose] built-in listen relay offered protocol %u to player %d",
            static_cast<unsigned int>(l4d2vr_pose::kVersion),
            playerIndex);
    }
}

bool Game::HandleBuiltinVRPoseRelayCommand(
    edict_t* entity,
    const void* sourceCommand)
{
    if (!m_ServerPluginHelpers || !m_ServerGameClients)
        return false;

    VRPoseRelayCommandView command{};
    if (!VRPoseRelayReadCommand(sourceCommand, command))
        return false;

    const bool isHello =
        std::strcmp(command.name, kVRPoseRelayHelloCommand) == 0;
    const bool isUpload =
        std::strcmp(command.name, kVRPoseRelayUploadCommand) == 0;
    if (!isHello && !isUpload)
        return false;

    int playerIndex = -1;
    std::int16_t edictSerial = 0;
    if (!VRPoseRelayReadEdictIdentity(
            entity,
            playerIndex,
            edictSerial))
    {
        return true;
    }

    std::lock_guard<std::recursive_mutex> lock(
        m_BuiltinVRPoseRelayMutex);
    VRPoseRelayServerClient& client =
        m_BuiltinVRPoseRelayClients[playerIndex];
    if (client.entity != entity ||
        client.edictSerial != edictSerial)
    {
        client = VRPoseRelayServerClient{};
        client.entity = entity;
        client.edictSerial = edictSerial;
    }

    if (isHello)
    {
        std::uint16_t protocolVersion = 0u;
        if (command.argumentCount == 2 &&
            VRPoseRelayParseSequence(
                command.argument1,
                protocolVersion) &&
            protocolVersion == l4d2vr_pose::kVersion)
        {
            const bool firstHello = !client.protocolSupported;
            client.protocolSupported = true;
            // A hello also marks a new sender session. This permits a client
            // module reload or map transition to restart its uint16 sequence.
            client.haveSequence = false;
            if (firstHello)
            {
                Game::logMsg(
                    "[VR][WorldPose] built-in listen relay accepted player %d",
                    playerIndex);
            }
        }
        return true;
    }

    if (!client.protocolSupported ||
        command.argumentCount != 3)
    {
        return true;
    }

    std::uint16_t sequence = 0u;
    if (!VRPoseRelayParseSequence(command.argument1, sequence))
        return true;
    if (client.haveSequence &&
        !l4d2vr_pose::IsSequenceNewer(
            sequence,
            client.lastSequence))
    {
        return true;
    }

    const std::uint64_t nowUs = VRPoseTickUs();
    if (client.lastDecodeTickUs != 0u &&
        nowUs - client.lastDecodeTickUs <
            kVRPoseRelayMinimumDecodeIntervalUs)
    {
        return true;
    }
    // Invalid payloads consume the rate-limit slot too.
    client.lastDecodeTickUs = nowUs;

    l4d2vr_pose::WirePacket packet{};
    if (!l4d2vr_pose::DecodePayload(
            command.argument2,
            packet))
    {
        return true;
    }

    client.haveSequence = true;
    client.lastSequence = sequence;
    client.lastUploadTickMs = VRPoseTickMs();
    client.ackAttempts = 0;

    char relayCommand[192] = {};
    const int commandLength = std::snprintf(
        relayCommand,
        sizeof(relayCommand),
        "l4d2vr_pose_receive %d %u %s\n",
        playerIndex,
        static_cast<unsigned int>(sequence),
        command.argument2);
    if (commandLength <= 0 ||
        commandLength >= static_cast<int>(sizeof(relayCommand)))
    {
        return true;
    }

    for (int recipientIndex = 1;
         recipientIndex <= kVRPoseRelayMaxPlayerIndex;
         ++recipientIndex)
    {
        if (recipientIndex == playerIndex)
            continue;

        VRPoseRelayServerClient& recipient =
            m_BuiltinVRPoseRelayClients[recipientIndex];
        if (!recipient.protocolSupported || !recipient.entity)
            continue;

        int currentIndex = -1;
        std::int16_t currentSerial = 0;
        if (!VRPoseRelayReadEdictIdentity(
                recipient.entity,
                currentIndex,
                currentSerial) ||
            currentIndex != recipientIndex ||
            currentSerial != recipient.edictSerial)
        {
            recipient = VRPoseRelayServerClient{};
            continue;
        }

        VRPoseRelaySendClientCommand(
            m_ServerPluginHelpers,
            recipient.entity,
            relayCommand);
    }

    return true;
}

void Game::HandleVRPoseServerAck(int protocolVersion)
{
    if (protocolVersion != static_cast<int>(l4d2vr_pose::kVersion))
        return;

    const bool wasCapable =
        m_VRPoseServerCapable.exchange(true, std::memory_order_acq_rel);
    // The relay resets its per-client capability table on every LevelInit.
    // ACK is therefore also the map-transition handshake: always answer it
    // idempotently even when this client already completed a previous map.
    m_VRPoseHelloSent.store(true, std::memory_order_release);
    char helloCommand[32]{};
    const int helloLength = std::snprintf(
        helloCommand,
        sizeof(helloCommand),
        "l4d2vr_pose_hello %u",
        static_cast<unsigned int>(l4d2vr_pose::kVersion));
    if (helloLength > 0 &&
        helloLength < static_cast<int>(sizeof(helloCommand)))
    {
        ServerCmd(helloCommand, true);
    }

    if (!wasCapable)
    {
        Game::logMsg(
            "[VR][WorldPose] server relay protocol %d acknowledged",
            protocolVersion);
    }
}

void Game::PublishLocalVRPose(VR* vr, C_BasePlayer* localPlayer)
{
    if (!vr ||
        !localPlayer ||
        !vr->m_IsVREnabled ||
        !vr->m_WorldModelVRPoseEnabled ||
        !vr->m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        !m_EngineClient ||
        !m_EngineClient->IsInGame())
    {
        return;
    }

    const int localPlayerIndex = m_EngineClient->GetLocalPlayer();
    if (!IsValidPlayerIndex(localPlayerIndex) || localPlayerIndex <= 0)
        return;

    const std::uint64_t nowMs = VRPoseTickMs();
    const float sendHz = std::clamp(
        vr->m_WorldModelVRPoseSendHz,
        10.0f,
        60.0f);
    const std::uint64_t intervalMs = static_cast<std::uint64_t>(
        std::max(1.0f, std::floor(1000.0f / sendHz)));
    if (m_VRPoseLastLocalPublishTickMs != 0u &&
        nowMs - m_VRPoseLastLocalPublishTickMs < intervalMs)
    {
        return;
    }
    m_VRPoseLastLocalPublishTickMs = nowMs;

    Vector playerOrigin{};
    QAngle playerAngles{};
    if (!VRPoseReadPlayerTransform(
            localPlayer,
            playerOrigin,
            playerAngles))
    {
        return;
    }

    if (!VRPoseFiniteVector(playerOrigin) ||
        !VRPoseFiniteAngles(playerAngles))
    {
        return;
    }

    VRWorldPoseTrackingSnapshot tracking{};
    if (!vr->ReadWorldPoseTrackingSnapshot(tracking))
        return;
    if (!VRPoseFiniteVector(tracking.referenceOrigin) ||
        (tracking.twoHandedGripActive && tracking.emptyHandsActive) ||
        (tracking.leftFingerUsesNativeAnimation &&
         tracking.leftFingerCurlsValid) ||
        (tracking.rightFingerUsesNativeAnimation &&
         tracking.rightFingerCurlsValid))
    {
        return;
    }

    // The tracked positions, controller angles and local yaw frame must be one
    // coherent UpdateTracking sample. A later yaw read makes stationary hands
    // orbit the player whenever a turn lands between the two reads.
    const float bodyYaw = VRPoseNormalizeAngle(
        tracking.bodyYawValid ? tracking.bodyYaw : playerAngles.y);
    std::uint8_t validMask = 0u;
    if (tracking.hmdValid)
        validMask |= l4d2vr_pose::kValidHmd;
    if (tracking.leftHandValid)
        validMask |= l4d2vr_pose::kValidLeftHand;
    if (tracking.rightHandValid)
        validMask |= l4d2vr_pose::kValidRightHand;
    if ((validMask & l4d2vr_pose::kValidHmd) == 0u)
        return;

    VRPoseFrame frame{};
    frame.valid = true;
    frame.bodyYawValid = true;
    frame.validMask = validMask;
    frame.featureMask = l4d2vr_pose::kFeatureBodyYaw;
    frame.sequence = ++m_VRPoseLocalSequence;
    frame.receivedTickMs = nowMs;
    frame.bodyYaw = bodyYaw;

    if (tracking.leftFingerUsesNativeAnimation)
    {
        frame.handStateFlags |=
            l4d2vr_pose::kHandStateLeftNativeFingerAnimation;
    }
    if (tracking.rightFingerUsesNativeAnimation)
    {
        frame.handStateFlags |=
            l4d2vr_pose::kHandStateRightNativeFingerAnimation;
    }
    if (tracking.twoHandedGripActive)
        frame.handStateFlags |= l4d2vr_pose::kHandStateTwoHandedGrip;
    if (tracking.emptyHandsActive)
        frame.handStateFlags |= l4d2vr_pose::kHandStateEmptyHands;

    if (tracking.leftFingerCurlsValid &&
        (validMask & l4d2vr_pose::kValidLeftHand) != 0u)
    {
        frame.featureMask |= l4d2vr_pose::kFeatureLeftFingerCurls;
        frame.leftFingerCurls = tracking.leftFingerCurls;
    }
    if (tracking.rightFingerCurlsValid &&
        (validMask & l4d2vr_pose::kValidRightHand) != 0u)
    {
        frame.featureMask |= l4d2vr_pose::kFeatureRightFingerCurls;
        frame.rightFingerCurls = tracking.rightFingerCurls;
    }

    frame.hmd.position = VRPoseWorldPositionToBodyLocal(
        tracking.hmdPosition,
		tracking.referenceOrigin,
        bodyYaw);
    frame.hmd.angles = VRPoseWorldAnglesToBodyLocal(
        tracking.hmdAngles,
        bodyYaw);
    frame.leftHand.position = VRPoseWorldPositionToBodyLocal(
        tracking.leftHandPosition,
		tracking.referenceOrigin,
        bodyYaw);
    frame.leftHand.angles = VRPoseWorldAnglesToBodyLocal(
        tracking.leftHandAngles,
        bodyYaw);
    frame.rightHand.position = VRPoseWorldPositionToBodyLocal(
        tracking.rightHandPosition,
		tracking.referenceOrigin,
        bodyYaw);
    frame.rightHand.angles = VRPoseWorldAnglesToBodyLocal(
        tracking.rightHandAngles,
        bodyYaw);

    if (((frame.validMask & l4d2vr_pose::kValidHmd) != 0u &&
            !VRPoseTrackedPointSane(frame.hmd)) ||
        ((frame.validMask & l4d2vr_pose::kValidLeftHand) != 0u &&
            !VRPoseTrackedPointSane(frame.leftHand)) ||
        ((frame.validMask & l4d2vr_pose::kValidRightHand) != 0u &&
            !VRPoseTrackedPointSane(frame.rightHand)))
    {
        return;
    }

    l4d2vr_pose::WirePacket wire{};
    wire.versionAndValidMask = static_cast<std::uint8_t>(
        (l4d2vr_pose::kVersion << 4) | frame.validMask);
    wire.featureMask = frame.featureMask;
    wire.handStateFlags = frame.handStateFlags;
    wire.bodyYaw = VRPosePackAngle(frame.bodyYaw);
    VRPosePackTrackedPose(frame.hmd, wire.hmd);
    VRPosePackTrackedPose(frame.leftHand, wire.leftHand);
    VRPosePackTrackedPose(frame.rightHand, wire.rightHand);
    for (int finger = 0; finger < 5; ++finger)
    {
        wire.leftFingerCurls[finger] = VRPosePackFingerCurl(
            frame.leftFingerCurls[static_cast<size_t>(finger)]);
        wire.rightFingerCurls[finger] = VRPosePackFingerCurl(
            frame.rightFingerCurls[static_cast<size_t>(finger)]);
    }
    l4d2vr_pose::FinalizePacket(wire);

    std::string encodedPayload;
    if (!l4d2vr_pose::EncodePayload(wire, encodedPayload))
        return;

    // Local loopback makes the exact observer reconstruction available to the
    // local third-person camera even when no multiplayer relay is installed.
    ReceiveVRPosePayload(
        localPlayerIndex,
        frame.sequence,
        encodedPayload.c_str(),
        false,
        bodyYaw);

    if (!m_VRPoseServerCapable.load(std::memory_order_acquire))
        return;

    char command[192]{};
    const int written = std::snprintf(
        command,
        sizeof(command),
        "l4d2vr_pose_upload %u %s",
        static_cast<unsigned int>(frame.sequence),
        encodedPayload.c_str());
    if (written <= 0 || written >= static_cast<int>(sizeof(command)))
        return;

    // Pose samples are sequenced and interpolated, so stale reliable delivery
    // is less useful than the latest packet.
    ServerCmd(command, false);
}

bool Game::ReceiveVRPosePayload(
    int playerIndex,
    std::uint16_t sequence,
    const char* encodedPayload,
    bool fromServer,
    float encodedBodyYaw)
{
    if (!IsValidPlayerIndex(playerIndex) ||
        playerIndex <= 0 ||
        !encodedPayload)
    {
        return false;
    }

    l4d2vr_pose::WirePacket wire{};
    if (!l4d2vr_pose::DecodePayload(encodedPayload, wire))
        return false;

    VRPoseFrame frame{};
    frame.valid = true;
    frame.validMask = l4d2vr_pose::PacketValidMask(wire);
    frame.featureMask = l4d2vr_pose::PacketFeatureMask(wire);
    frame.handStateFlags = l4d2vr_pose::PacketHandStateFlags(wire);
    frame.sequence = sequence;
    frame.receivedTickMs = VRPoseTickMs();
    frame.hmd = VRPoseUnpackTrackedPose(wire.hmd);
    frame.leftHand = VRPoseUnpackTrackedPose(wire.leftHand);
    frame.rightHand = VRPoseUnpackTrackedPose(wire.rightHand);
    for (int finger = 0; finger < 5; ++finger)
    {
        frame.leftFingerCurls[static_cast<size_t>(finger)] =
            VRPoseUnpackFingerCurl(wire.leftFingerCurls[finger]);
        frame.rightFingerCurls[static_cast<size_t>(finger)] =
            VRPoseUnpackFingerCurl(wire.rightFingerCurls[finger]);
    }

    if (((frame.validMask & l4d2vr_pose::kValidHmd) != 0u &&
            !VRPoseTrackedPointSane(frame.hmd)) ||
        ((frame.validMask & l4d2vr_pose::kValidLeftHand) != 0u &&
            !VRPoseTrackedPointSane(frame.leftHand)) ||
        ((frame.validMask & l4d2vr_pose::kValidRightHand) != 0u &&
            !VRPoseTrackedPointSane(frame.rightHand)))
    {
        return false;
    }

    C_BasePlayer* observedPlayer = nullptr;
    int observedUserID = -1;
    VRPoseReadPlayerIdentity(
        this,
        playerIndex,
        observedPlayer,
        observedUserID);
    if ((frame.featureMask & l4d2vr_pose::kFeatureBodyYaw) != 0u)
    {
        frame.bodyYaw = VRPoseUnpackAngle(wire.bodyYaw);
        frame.bodyYawValid = true;
    }
    else if (std::isfinite(encodedBodyYaw))
    {
        frame.bodyYaw = VRPoseNormalizeAngle(encodedBodyYaw);
        frame.bodyYawValid = true;
    }
    else if (observedPlayer)
    {
        Vector observedOrigin{};
        QAngle observedAngles{};
        if (VRPoseReadPlayerTransform(
                observedPlayer,
                observedOrigin,
                observedAngles) &&
            std::isfinite(observedAngles.y))
        {
            frame.bodyYaw =
                VRPoseNormalizeAngle(observedAngles.y);
            frame.bodyYawValid = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_VRPoseMutex);
        Player& player = m_PlayersVRInfo[static_cast<std::size_t>(playerIndex)];
        const bool playerIdentityChanged =
            (observedPlayer &&
             player.pPlayer &&
             observedPlayer != player.pPlayer) ||
            (observedUserID >= 0 &&
             player.worldPoseUserID >= 0 &&
             observedUserID != player.worldPoseUserID);
        if (playerIdentityChanged)
        {
            player.previousWorldPose = VRPoseFrame{};
            player.latestWorldPose = VRPoseFrame{};
            player.worldPoseBlendWeight = 0.0f;
            player.worldPoseBlendTickMs = 0u;
        }
        if (observedPlayer)
            player.pPlayer = observedPlayer;
        if (observedUserID >= 0)
            player.worldPoseUserID = observedUserID;

        if (player.latestWorldPose.valid &&
            !l4d2vr_pose::IsSequenceNewer(
                sequence,
                player.latestWorldPose.sequence))
        {
            // A client module reload can restart its uint16 sequence without
            // changing the Source player slot/userID.  The relay has already
            // validated this sender session, so permit a sequence rebase only
            // after the old observer sample has been silent for one second.
            const bool mayRebaseAfterSilence =
                frame.receivedTickMs >
                    player.latestWorldPose.receivedTickMs &&
                frame.receivedTickMs -
                    player.latestWorldPose.receivedTickMs >=
                    1000u;
            if (!mayRebaseAfterSilence)
                return false;
            player.previousWorldPose = VRPoseFrame{};
            player.latestWorldPose = VRPoseFrame{};
            player.worldPoseBlendWeight = 0.0f;
            player.worldPoseBlendTickMs = 0u;
        }

        player.previousWorldPose = player.latestWorldPose.valid
            ? player.latestWorldPose
            : frame;
        player.latestWorldPose = frame;
    }

    if (fromServer)
        m_VRPoseServerCapable.store(true, std::memory_order_release);

    if (m_VR && m_VR->m_WorldModelVRPoseDebugLog)
    {
        static std::array<std::uint64_t, Game::kMaxPlayers> s_lastLogMs{};
        if (frame.receivedTickMs - s_lastLogMs[static_cast<std::size_t>(playerIndex)] >= 1000u)
        {
            s_lastLogMs[static_cast<std::size_t>(playerIndex)] = frame.receivedTickMs;
            Game::logMsg(
                "[VR][WorldPose] receive player=%d seq=%u source=%s bodyYaw=%.1f valid=%d features=0x%02X handState=0x%02X hmd=(%.1f %.1f %.1f) hmdRot=(%.1f %.1f %.1f) left=(%.1f %.1f %.1f) right=(%.1f %.1f %.1f)",
                playerIndex,
                static_cast<unsigned int>(sequence),
                fromServer ? "server" : "local",
                frame.bodyYaw,
                frame.bodyYawValid ? 1 : 0,
                static_cast<unsigned int>(frame.featureMask),
                static_cast<unsigned int>(frame.handStateFlags),
                frame.hmd.position.x,
                frame.hmd.position.y,
                frame.hmd.position.z,
                frame.hmd.angles.x,
                frame.hmd.angles.y,
                frame.hmd.angles.z,
                frame.leftHand.position.x,
                frame.leftHand.position.y,
                frame.leftHand.position.z,
                frame.rightHand.position.x,
                frame.rightHand.position.y,
                frame.rightHand.position.z);
        }
    }

    return true;
}

bool Game::GetInterpolatedVRPose(
    int playerIndex,
    float interpolationDelayMs,
    float staleAfterMs,
    VRPoseFrame& outPose,
    float& outFreshness) const
{
    outPose = VRPoseFrame{};
    outFreshness = 0.0f;
    if (!IsValidPlayerIndex(playerIndex) || playerIndex <= 0)
        return false;

    VRPoseFrame previous{};
    VRPoseFrame latest{};
    {
        std::lock_guard<std::mutex> lock(m_VRPoseMutex);
        const Player& player =
            m_PlayersVRInfo[static_cast<std::size_t>(playerIndex)];
        previous = player.previousWorldPose;
        latest = player.latestWorldPose;
    }

    if (!latest.valid)
        return false;

    const std::uint64_t nowMs = VRPoseTickMs();
    const float ageMs = static_cast<float>(
        nowMs >= latest.receivedTickMs
            ? nowMs - latest.receivedTickMs
            : 0u);
    const float staleStart = std::clamp(staleAfterMs, 100.0f, 2000.0f);
    const float staleFadeMs = std::clamp(staleStart, 100.0f, 500.0f);
    outFreshness = 1.0f - std::clamp(
        (ageMs - staleStart) / staleFadeMs,
        0.0f,
        1.0f);
    if (outFreshness <= 0.0f)
        return false;

    float fraction = 1.0f;
    if (previous.valid &&
        latest.receivedTickMs > previous.receivedTickMs)
    {
        const std::uint64_t delay = static_cast<std::uint64_t>(
            std::clamp(interpolationDelayMs, 0.0f, 250.0f));
        const std::uint64_t targetTick =
            (nowMs > delay) ? (nowMs - delay) : 0u;
        const float denominator = static_cast<float>(
            latest.receivedTickMs - previous.receivedTickMs);
        const float numerator = static_cast<float>(
            targetTick > previous.receivedTickMs
                ? targetTick - previous.receivedTickMs
                : 0u);
        fraction = std::clamp(numerator / denominator, 0.0f, 1.0f);
    }

    VRTrackedPoseLocal previousHmd =
        previous.valid ? previous.hmd : latest.hmd;
    VRTrackedPoseLocal previousLeftHand =
        previous.valid ? previous.leftHand : latest.leftHand;
    VRTrackedPoseLocal previousRightHand =
        previous.valid ? previous.rightHand : latest.rightHand;
    if (previous.valid &&
        previous.bodyYawValid &&
        latest.bodyYawValid)
    {
        // Body-local packet coordinates are meaningful only in the yaw frame
        // in which they were sampled. Rebase the older sample into the latest
        // yaw frame before interpolation, otherwise a turn makes stationary
        // controllers orbit the player origin and visibly breaks arm IK.
        previousHmd = VRPoseRebaseTrackedPointYaw(
            previous.hmd,
            previous.bodyYaw,
            latest.bodyYaw);
        previousLeftHand = VRPoseRebaseTrackedPointYaw(
            previous.leftHand,
            previous.bodyYaw,
            latest.bodyYaw);
        previousRightHand = VRPoseRebaseTrackedPointYaw(
            previous.rightHand,
            previous.bodyYaw,
            latest.bodyYaw);
    }

    outPose = latest;
    outPose.hmd = VRPoseInterpolateTrackedPoint(
        previousHmd,
        latest.hmd,
        fraction);
    outPose.leftHand = VRPoseInterpolateTrackedPoint(
        previousLeftHand,
        latest.leftHand,
        fraction);
    outPose.rightHand = VRPoseInterpolateTrackedPoint(
        previousRightHand,
        latest.rightHand,
        fraction);

    const bool leftFingerModeStable =
        previous.valid &&
        ((previous.handStateFlags ^ latest.handStateFlags) &
         l4d2vr_pose::kHandStateLeftNativeFingerAnimation) == 0u;
    if ((latest.featureMask &
         l4d2vr_pose::kFeatureLeftFingerCurls) != 0u)
    {
        const bool canInterpolate =
            leftFingerModeStable &&
            (previous.featureMask &
             l4d2vr_pose::kFeatureLeftFingerCurls) != 0u;
        outPose.leftFingerCurls = canInterpolate
            ? VRPoseInterpolateFingerCurls(
                previous.leftFingerCurls,
                latest.leftFingerCurls,
                fraction)
            : latest.leftFingerCurls;
    }

    const bool rightFingerModeStable =
        previous.valid &&
        ((previous.handStateFlags ^ latest.handStateFlags) &
         l4d2vr_pose::kHandStateRightNativeFingerAnimation) == 0u;
    if ((latest.featureMask &
         l4d2vr_pose::kFeatureRightFingerCurls) != 0u)
    {
        const bool canInterpolate =
            rightFingerModeStable &&
            (previous.featureMask &
             l4d2vr_pose::kFeatureRightFingerCurls) != 0u;
        outPose.rightFingerCurls = canInterpolate
            ? VRPoseInterpolateFingerCurls(
                previous.rightFingerCurls,
                latest.rightFingerCurls,
                fraction)
            : latest.rightFingerCurls;
    }
    return true;
}

float Game::AdvanceVRPoseBlendWeight(
    int playerIndex,
    float targetWeight,
    float blendSeconds)
{
    if (!IsValidPlayerIndex(playerIndex) || playerIndex <= 0)
        return 0.0f;

    const std::uint64_t nowMs = VRPoseTickMs();
    std::lock_guard<std::mutex> lock(m_VRPoseMutex);
    Player& player = m_PlayersVRInfo[static_cast<std::size_t>(playerIndex)];
    targetWeight = std::clamp(targetWeight, 0.0f, 1.0f);
    blendSeconds = std::clamp(blendSeconds, 0.05f, 0.50f);

    if (player.worldPoseBlendTickMs == 0u)
    {
        player.worldPoseBlendTickMs = nowMs;
        return player.worldPoseBlendWeight;
    }

    const float deltaSeconds = std::clamp(
        static_cast<float>(nowMs - player.worldPoseBlendTickMs) * 0.001f,
        0.0f,
        0.10f);
    player.worldPoseBlendTickMs = nowMs;
    const float step = std::clamp(
        deltaSeconds / blendSeconds,
        0.0f,
        1.0f);
    if (player.worldPoseBlendWeight < targetWeight)
    {
        player.worldPoseBlendWeight = std::min(
            targetWeight,
            player.worldPoseBlendWeight + step);
    }
    else
    {
        player.worldPoseBlendWeight = std::max(
            targetWeight,
            player.worldPoseBlendWeight - step);
    }
    return player.worldPoseBlendWeight;
}

// === Entity Access ===
C_BaseEntity* Game::GetClientEntity(int entityIndex)
{
    if (!m_ClientEntityList)
        return nullptr;

    return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntity(entityIndex));
}

// === Network Name Utility ===
char* Game::getNetworkName(uintptr_t* entity)
{
    __try
    {
        if (!entity)
            return nullptr;

        uintptr_t* vtable = reinterpret_cast<uintptr_t*>(*(entity + 0x8));
        if (!vtable)
            return nullptr;

        uintptr_t* getClientClassFn = reinterpret_cast<uintptr_t*>(*(vtable + 0x8));
        if (!getClientClassFn)
            return nullptr;

        uintptr_t* clientClass = reinterpret_cast<uintptr_t*>(*(getClientClassFn + 0x1));
        if (!clientClass)
            return nullptr;

        return reinterpret_cast<char*>(*(clientClass + 0x8));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

const char* Game::GetNetworkClassName(uintptr_t* entity) const
{
    __try
    {
        if (!entity)
            return nullptr;

        uintptr_t* vtable = reinterpret_cast<uintptr_t*>(*(entity + 0x8));
        if (!vtable)
            return nullptr;

        uintptr_t* getClientClassFn = reinterpret_cast<uintptr_t*>(*(vtable + 0x8));
        if (!getClientClassFn)
            return nullptr;

        uintptr_t* clientClass = reinterpret_cast<uintptr_t*>(*(getClientClassFn + 0x1));
        if (!clientClass)
            return nullptr;

        return reinterpret_cast<const char*>(*(clientClass + 0x8));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

int Game::FindRecvPropOffset(const char* networkName, const char* propName) const
{
    if (!m_BaseClientDll || !networkName || !*networkName || !propName || !*propName)
        return -1;

    static std::unordered_map<std::string, int> cache;
    const std::string key = std::string(networkName) + "::" + propName;
    auto cached = cache.find(key);
    if (cached != cache.end())
        return cached->second;

    const int offset = FindRecvPropOffsetSafe(m_BaseClientDll, networkName, propName);
    if (offset >= 0)
        cache[key] = offset;

    return offset;
}

// === Commands ===
void Game::ServerCmd(const char* szCmdString, bool reliable)
{
    if (m_EngineClient && szCmdString && *szCmdString)
        m_EngineClient->ServerCmd(szCmdString, reliable);
}

void Game::ClientCmd(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}

void Game::BeginConVarWritePermit()
{
    ++s_ConVarWritePermitDepth;
}

void Game::EndConVarWritePermit()
{
    if (s_ConVarWritePermitDepth > 0)
        --s_ConVarWritePermitDepth;
}

bool Game::HasConVarWritePermit()
{
    return s_ConVarWritePermitDepth > 0;
}

static SourceConVar* FindConVarInternal(void* cvarIface, const char* name)
{
    if (!cvarIface || !name || !*name)
        return nullptr;

    return reinterpret_cast<SourceICvar*>(cvarIface)->FindVar(name);
}

static SourceIConVar* GetConVarIConVar(SourceConVar* cvar)
{
    if (!cvar)
        return nullptr;

    return reinterpret_cast<SourceIConVar*>(reinterpret_cast<uint8_t*>(cvar) + sizeof(SourceConCommandBase));
}

static void* GetVTableEntry(void* objectBase, size_t index)
{
    if (!objectBase)
        return nullptr;

    __try
    {
        void*** object = reinterpret_cast<void***>(objectBase);
        void** vtable = (object != nullptr) ? *object : nullptr;
        return (vtable != nullptr) ? vtable[index] : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void* Game::FindConVar(const char* name) const
{
    return FindConVarInternal(m_Cvar, name);
}

const char* Game::GetConVarNameFromPointer(const void* convar) const
{
    __try
    {
        const SourceConCommandBase* base = reinterpret_cast<const SourceConCommandBase*>(convar);
        return (base && base->m_pszName) ? base->m_pszName : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

const char* Game::GetConVarNameFromIConVarPointer(const void* iconvar) const
{
    __try
    {
        if (!iconvar)
            return nullptr;

        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(iconvar);
        const SourceConCommandBase* base = reinterpret_cast<const SourceConCommandBase*>(bytes - sizeof(SourceConCommandBase));
        return (base && base->m_pszName) ? base->m_pszName : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void* Game::GetConVarStringSetValueTarget(const char* name) const
{
    return GetVTableEntry(GetConVarIConVar(FindConVarInternal(m_Cvar, name)), kIConVarVtableIndexSetValueString);
}

void* Game::GetConVarPrimaryStringSetValueTarget(const char* name) const
{
    return GetVTableEntry(FindConVarInternal(m_Cvar, name), kConVarVtableIndexSetValueString);
}

void* Game::GetConVarPrimaryFloatSetValueTarget(const char* name) const
{
    return GetVTableEntry(FindConVarInternal(m_Cvar, name), kConVarVtableIndexSetValueFloat);
}

void* Game::GetConVarPrimaryIntSetValueTarget(const char* name) const
{
    return GetVTableEntry(FindConVarInternal(m_Cvar, name), kConVarVtableIndexSetValueInt);
}

void* Game::GetConVarFloatSetValueTarget(const char* name) const
{
    return GetVTableEntry(GetConVarIConVar(FindConVarInternal(m_Cvar, name)), kIConVarVtableIndexSetValueFloat);
}

void* Game::GetConVarIntSetValueTarget(const char* name) const
{
    return GetVTableEntry(GetConVarIConVar(FindConVarInternal(m_Cvar, name)), kIConVarVtableIndexSetValueInt);
}

void* Game::GetConVarInternalStringSetValueTarget(const char* name) const
{
    return GetVTableEntry(FindConVarInternal(m_Cvar, name), kConVarVtableIndexInternalSetValueString);
}

void* Game::GetConVarInternalFloatSetValueTarget(const char* name) const
{
    return GetVTableEntry(FindConVarInternal(m_Cvar, name), kConVarVtableIndexInternalSetValueFloat);
}

void* Game::GetConVarInternalIntSetValueTarget(const char* name) const
{
    return GetVTableEntry(FindConVarInternal(m_Cvar, name), kConVarVtableIndexInternalSetValueInt);
}

static int FindRecvPropOffsetRecursive(const SourceRecvTable* table, const char* propName, int accumulatedOffset)
{
    if (!table || !propName || !*propName || !table->m_pProps || table->m_nProps <= 0)
        return -1;

    for (int i = 0; i < table->m_nProps; ++i)
    {
        const SourceRecvProp& prop = table->m_pProps[i];
        if (prop.m_pVarName && std::strcmp(prop.m_pVarName, propName) == 0)
            return accumulatedOffset + prop.m_Offset;

        if (prop.m_pDataTable && prop.m_pDataTable->m_pProps && prop.m_pDataTable->m_nProps > 0)
        {
            const int nestedOffset =
                FindRecvPropOffsetRecursive(prop.m_pDataTable, propName, accumulatedOffset + prop.m_Offset);
            if (nestedOffset >= 0)
                return nestedOffset;
        }
    }

    return -1;
}

static int FindRecvPropOffsetSafe(void* baseClientDll, const char* networkName, const char* propName)
{
    __try
    {
        auto* clientDll = reinterpret_cast<SourceBaseClientDLL*>(baseClientDll);
        if (!clientDll || !networkName || !*networkName || !propName || !*propName)
            return -1;

        auto* clientClass = reinterpret_cast<SourceClientClass*>(clientDll->GetAllClasses());
        while (clientClass)
        {
            const bool networkNameMatches =
                clientClass->m_pNetworkName &&
                std::strcmp(clientClass->m_pNetworkName, networkName) == 0;
            const bool recvTableNameMatches =
                clientClass->m_pRecvTable &&
                clientClass->m_pRecvTable->m_pNetTableName &&
                std::strcmp(
                    clientClass->m_pRecvTable->m_pNetTableName,
                    networkName) == 0;
            if (networkNameMatches || recvTableNameMatches)
                return FindRecvPropOffsetRecursive(clientClass->m_pRecvTable, propName, 0);

            clientClass = clientClass->m_pNext;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }

    return -1;
}

static SourceIConVar* AsIConVar(SourceConVar* cvar)
{
    return GetConVarIConVar(cvar);
}

int Game::GetConVarInt(const char* name, int fallback) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return fallback;

    __try
    {
        return cvar->GetIntValue();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

int Game::GetConVarIntDirect(const char* name, int fallback) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return fallback;

    __try
    {
        return cvar->GetIntValueDirect();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

float Game::GetConVarFloat(const char* name, float fallback) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return fallback;

    __try
    {
        return cvar->GetFloatValue();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

float Game::GetConVarFloatDirect(const char* name, float fallback) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return fallback;

    __try
    {
        return cvar->GetFloatValueDirect();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

static const char* TryGetConVarStringRaw(SourceConVar* cvar)
{
    __try
    {
        if (!cvar)
            return nullptr;

        const SourceConVar* parent = (cvar->m_pParent != nullptr) ? cvar->m_pParent : cvar;
        return parent->m_pszString;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

std::string Game::GetConVarString(const char* name) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return std::string();

    const char* raw = TryGetConVarStringRaw(cvar);
    return raw ? std::string(raw) : std::string();
}

int Game::GetConVarFlags(const char* name) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return -1;

    __try
    {
        return cvar->m_nFlags;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

bool Game::SetConVarFlags(const char* name, int flags) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return false;

    __try
    {
        SourceConVar* parent = (cvar->m_pParent != nullptr) ? cvar->m_pParent : cvar;
        parent->m_nFlags = flags;
        cvar->m_nFlags = flags;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Game::SetConVarString(const char* name, const char* value) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return false;

    __try
    {
        BeginConVarWritePermit();
        cvar->SetValue(value ? value : "");
        EndConVarWritePermit();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        EndConVarWritePermit();
        return false;
    }
}

bool Game::SetConVarInt(const char* name, int value) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return false;

    SourceIConVar* iconvar = AsIConVar(cvar);
    if (!iconvar)
        return false;

    __try
    {
        BeginConVarWritePermit();
        iconvar->SetValue(value);
        EndConVarWritePermit();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        EndConVarWritePermit();
        return false;
    }
}

bool Game::SetConVarFloat(const char* name, float value) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return false;

    __try
    {
        BeginConVarWritePermit();
        char buffer[64] = {};
        sprintf_s(buffer, "%.9g", static_cast<double>(value));
        cvar->SetValue(buffer);
        EndConVarWritePermit();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        EndConVarWritePermit();
        return false;
    }
}

bool Game::SetConVarBool(const char* name, bool value) const
{
    return SetConVarInt(name, value ? 1 : 0);
}

int Game::GetEntityEffects(const C_BaseEntity* entity, int fallback) const
{
    if (!entity)
        return fallback;

    static int s_effectsOffset = std::numeric_limits<int>::min();
    static bool s_loggedOffset = false;
    if (s_effectsOffset == std::numeric_limits<int>::min())
    {
        s_effectsOffset = FindRecvPropOffset("DT_BaseEntity", "m_fEffects");
        if (s_effectsOffset < 0)
            s_effectsOffset = 0xE0;

        s_loggedOffset = true;
    }

    __try
    {
        return *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(entity) + s_effectsOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

// === Rendering Thread Mode ===
int Game::GetMatQueueMode() const
{
    if (!m_MaterialSystem)
        return 0;

    void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
    if (!vtbl)
        return 0;

    // IMaterialSystem::GetThreadMode() is vfunc #11 in this ABI (0-based index).
    using tGetThreadMode = int(__thiscall*)(IMaterialSystem*);
    auto fn = reinterpret_cast<tGetThreadMode>(vtbl[11]);
    if (!fn)
        return 0;

    return fn(m_MaterialSystem);
}

#include "vr_config_overlay_embedded.inl"
