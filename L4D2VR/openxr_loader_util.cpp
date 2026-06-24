#include "openxr_loader_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace
{
    std::string Trim(std::string value)
    {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }

    bool EqualsIgnoreCase(const std::string& a, const std::string& b)
    {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    bool FileExists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::string JoinPath(const std::string& left, const std::string& right)
    {
        if (left.empty())
            return right;
        if (left.back() == '\\' || left.back() == '/')
            return left + right;
        return left + "\\" + right;
    }

    std::string DirectoryOf(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return {};
        return path.substr(0, slash);
    }

    std::string LoadedModulePath(HMODULE module, const std::string& fallback)
    {
        char buffer[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(module, buffer, static_cast<DWORD>(sizeof(buffer)));
        if (length == 0 || length >= sizeof(buffer))
            return fallback;
        return buffer;
    }

    std::string FormatWinError(DWORD error)
    {
        char message[256] = {};
        DWORD length = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            message,
            static_cast<DWORD>(sizeof(message)),
            nullptr);

        std::string result = "error=" + std::to_string(error);
        if (length != 0)
        {
            while (length > 0 &&
                (message[length - 1] == '\r' || message[length - 1] == '\n' || message[length - 1] == '.'))
            {
                message[--length] = '\0';
            }
            result += " (";
            result += message;
            result += ")";
        }
        return result;
    }

    void AddCandidate(std::vector<std::string>& candidates, const std::string& rawPath)
    {
        std::string path = Trim(rawPath);
        if (path.empty())
            return;

        const auto existing = std::find_if(candidates.begin(), candidates.end(),
            [&path](const std::string& candidate)
            {
                return EqualsIgnoreCase(candidate, path);
            });
        if (existing == candidates.end())
            candidates.push_back(path);
    }

    void AddLoaderBesideActiveRuntime(std::vector<std::string>& candidates, const std::string& activeRuntimePath)
    {
        const std::string runtimePath = Trim(activeRuntimePath);
        const std::string runtimeDir = DirectoryOf(runtimePath);
        if (runtimeDir.empty())
            return;

        AddCandidate(candidates, JoinPath(runtimeDir, "openxr_loader.dll"));

#if defined(_WIN64)
        AddCandidate(candidates, JoinPath(JoinPath(runtimeDir, "bin"), "win64\\openxr_loader.dll"));
        AddCandidate(candidates, JoinPath(JoinPath(runtimeDir, "bin"), "win32\\openxr_loader.dll"));
#else
        AddCandidate(candidates, JoinPath(JoinPath(runtimeDir, "bin"), "win32\\openxr_loader.dll"));
        AddCandidate(candidates, JoinPath(JoinPath(runtimeDir, "bin"), "win64\\openxr_loader.dll"));
#endif
    }

    bool ReadRegistryString(HKEY root, REGSAM view, const char* subKey, const char* valueName, std::string& out)
    {
        HKEY key = nullptr;
        const LONG openResult = RegOpenKeyExA(root, subKey, 0, KEY_READ | view, &key);
        if (openResult != ERROR_SUCCESS)
            return false;

        DWORD type = 0;
        DWORD size = 0;
        LONG queryResult = RegQueryValueExA(key, valueName, nullptr, &type, nullptr, &size);
        if (queryResult != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) ||
            size == 0)
        {
            RegCloseKey(key);
            return false;
        }

        std::vector<char> buffer(size + 2, '\0');
        queryResult = RegQueryValueExA(key, valueName, nullptr, &type,
            reinterpret_cast<LPBYTE>(buffer.data()), &size);
        RegCloseKey(key);
        if (queryResult != ERROR_SUCCESS)
            return false;

        out = buffer.data();
        if (type == REG_EXPAND_SZ && !out.empty())
        {
            char expanded[MAX_PATH * 2] = {};
            const DWORD expandedLength = ExpandEnvironmentStringsA(
                out.c_str(),
                expanded,
                static_cast<DWORD>(sizeof(expanded)));
            if (expandedLength > 0 && expandedLength < sizeof(expanded))
                out = expanded;
        }

        out = Trim(out);
        return !out.empty();
    }

    void AddActiveRuntimeCandidates(std::vector<std::string>& candidates, std::vector<std::string>& notes)
    {
        constexpr const char* kOpenXrRuntimeKey = "SOFTWARE\\Khronos\\OpenXR\\1";

        struct View
        {
            HKEY root;
            const char* rootName;
            REGSAM view;
            const char* viewName;
        };

        const View views[] =
        {
#if !defined(_WIN64)
            { HKEY_CURRENT_USER, "HKCU", KEY_WOW64_32KEY, "32" },
            { HKEY_LOCAL_MACHINE, "HKLM", KEY_WOW64_32KEY, "32" },
#endif
            { HKEY_CURRENT_USER, "HKCU", KEY_WOW64_64KEY, "64" },
            { HKEY_LOCAL_MACHINE, "HKLM", KEY_WOW64_64KEY, "64" },
            { HKEY_CURRENT_USER, "HKCU", 0, "default" },
            { HKEY_LOCAL_MACHINE, "HKLM", 0, "default" },
        };

        for (const View& view : views)
        {
            std::string activeRuntime;
            if (!ReadRegistryString(view.root, view.view, kOpenXrRuntimeKey, "ActiveRuntime", activeRuntime))
                continue;

            std::ostringstream note;
            note << view.rootName << "\\" << kOpenXrRuntimeKey
                << " view=" << view.viewName
                << " ActiveRuntime=" << activeRuntime;
            notes.push_back(note.str());
            AddLoaderBesideActiveRuntime(candidates, activeRuntime);
        }
    }

    void AddEnvironmentCandidate(std::vector<std::string>& candidates, const char* variableName, const char* suffix)
    {
        char value[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableA(variableName, value, static_cast<DWORD>(sizeof(value)));
        if (length == 0 || length >= sizeof(value))
            return;

        AddCandidate(candidates, JoinPath(value, suffix));
    }

    void AddDefaultCandidates(std::vector<std::string>& candidates, std::vector<std::string>& notes)
    {
        char exePath[MAX_PATH] = {};
        const DWORD exeLength = GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(sizeof(exePath)));
        if (exeLength > 0 && exeLength < sizeof(exePath))
            AddCandidate(candidates, JoinPath(DirectoryOf(exePath), "openxr_loader.dll"));

#if !defined(_WIN64)
        char wow64Path[MAX_PATH] = {};
        const UINT wow64Length = GetSystemWow64DirectoryA(wow64Path, static_cast<UINT>(sizeof(wow64Path)));
        if (wow64Length > 0 && wow64Length < sizeof(wow64Path))
            AddCandidate(candidates, JoinPath(wow64Path, "openxr_loader.dll"));
#endif

        char systemPath[MAX_PATH] = {};
        const UINT systemLength = GetSystemDirectoryA(systemPath, static_cast<UINT>(sizeof(systemPath)));
        if (systemLength > 0 && systemLength < sizeof(systemPath))
            AddCandidate(candidates, JoinPath(systemPath, "openxr_loader.dll"));

        AddActiveRuntimeCandidates(candidates, notes);
        AddEnvironmentCandidate(candidates, "ProgramFiles", "OpenXR-Toolkit\\openxr_loader.dll");
        AddEnvironmentCandidate(candidates, "ProgramFiles(x86)", "OpenXR-Toolkit\\openxr_loader.dll");
    }

    bool TryLoadOpenXrLoader(
        const std::string& path,
        L4D2VR_OpenXrLoaderLoadResult& result,
        std::vector<std::string>& attempts)
    {
        if (!FileExists(path))
        {
            attempts.push_back(path + ": missing");
            return false;
        }

        HMODULE module = LoadLibraryA(path.c_str());
        if (!module)
        {
            const DWORD error = GetLastError();
            attempts.push_back(path + ": LoadLibrary failed " + FormatWinError(error));
            result.error = error;
            return false;
        }

        if (!GetProcAddress(module, "xrGetInstanceProcAddr"))
        {
            attempts.push_back(path + ": missing xrGetInstanceProcAddr export");
            FreeLibrary(module);
            result.error = ERROR_PROC_NOT_FOUND;
            return false;
        }

        result.module = module;
        result.path = LoadedModulePath(module, path);
        result.detail = "openxr_loader.dll loaded from " + result.path;
        result.error = ERROR_SUCCESS;
        return true;
    }

    std::string BuildFailureDetail(
        const std::vector<std::string>& attempts,
        const std::vector<std::string>& notes,
        DWORD fallbackError)
    {
        std::ostringstream stream;
        stream << "openxr_loader.dll was not found for this "
#if defined(_WIN64)
            << "64-bit"
#else
            << "32-bit"
#endif
            << " process";

        if (fallbackError != ERROR_SUCCESS)
            stream << "; default DLL search failed " << FormatWinError(fallbackError);

        if (!notes.empty())
        {
            stream << "; active runtimes:";
            for (const std::string& note : notes)
                stream << " [" << note << "]";
        }

        if (!attempts.empty())
        {
            stream << "; searched:";
            for (const std::string& attempt : attempts)
                stream << " [" << attempt << "]";
        }

        return stream.str();
    }
}

L4D2VR_OpenXrLoaderLoadResult L4D2VR_LoadOpenXrLoader()
{
    L4D2VR_OpenXrLoaderLoadResult result{};
    std::vector<std::string> candidates;
    std::vector<std::string> notes;
    std::vector<std::string> attempts;

    AddDefaultCandidates(candidates, notes);

    for (const std::string& candidate : candidates)
    {
        if (TryLoadOpenXrLoader(candidate, result, attempts))
            return result;
    }

    HMODULE module = LoadLibraryA("openxr_loader.dll");
    DWORD fallbackError = ERROR_SUCCESS;
    if (!module)
    {
        fallbackError = GetLastError();
    }
    else if (!GetProcAddress(module, "xrGetInstanceProcAddr"))
    {
        attempts.push_back("default DLL search: missing xrGetInstanceProcAddr export");
        FreeLibrary(module);
        result.error = ERROR_PROC_NOT_FOUND;
    }
    else
    {
        result.module = module;
        result.path = LoadedModulePath(module, "openxr_loader.dll");
        result.detail = "openxr_loader.dll loaded from " + result.path;
        result.error = ERROR_SUCCESS;
        return result;
    }

    if (fallbackError != ERROR_SUCCESS)
        result.error = fallbackError;
    result.detail = BuildFailureDetail(attempts, notes, fallbackError);
    return result;
}
