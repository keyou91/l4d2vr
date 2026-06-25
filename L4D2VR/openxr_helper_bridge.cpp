#include "openxr_helper_bridge.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "game.h"
#include "openxr_bridge_protocol.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    std::atomic<bool> g_OpenXrHelperStarted{ false };
    std::mutex g_OpenXrBridgeStateMutex;
    L4D2VROpenXrBridgeState* g_OpenXrBridgeState = nullptr;

    void Trim(std::string& value)
    {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    }

    bool ParseBool(std::string value, bool fallback)
    {
        Trim(value);
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (value == "1" || value == "true" || value == "on" || value == "yes")
            return true;
        if (value == "0" || value == "false" || value == "off" || value == "no")
            return false;
        return fallback;
    }

    uint32_t ParseUint(std::string value, uint32_t fallback, uint32_t minValue, uint32_t maxValue)
    {
        Trim(value);
        if (value.empty())
            return fallback;

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
        if (!end || *end != '\0')
            return fallback;

        return std::clamp(static_cast<uint32_t>(parsed), minValue, maxValue);
    }

    std::unordered_map<std::string, std::string> ReadConfigValues()
    {
        std::unordered_map<std::string, std::string> values;

        auto parsePath = [&](const char* path)
            {
                std::ifstream stream(path);
                if (!stream)
                    return;

                std::string line;
                while (std::getline(stream, line))
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

                    Trim(line);
                    if (line.empty())
                        continue;

                    const size_t eq = line.find('=');
                    if (eq == std::string::npos)
                        continue;

                    std::string key = line.substr(0, eq);
                    std::string value = line.substr(eq + 1);
                    Trim(key);
                    Trim(value);
                    if (key.size() >= 3 &&
                        static_cast<unsigned char>(key[0]) == 0xEF &&
                        static_cast<unsigned char>(key[1]) == 0xBB &&
                        static_cast<unsigned char>(key[2]) == 0xBF)
                    {
                        key.erase(0, 3);
                        Trim(key);
                    }

                    if (!key.empty())
                        values[key] = value;
                }
            };

        parsePath("VR\\config.txt");
        parsePath("VR\\config2.txt");
        return values;
    }

    std::wstring CurrentDirectoryPath()
    {
        DWORD required = GetCurrentDirectoryW(0, nullptr);
        if (required == 0)
            return L".";

        std::wstring path(required, L'\0');
        DWORD written = GetCurrentDirectoryW(required, path.data());
        if (written == 0 || written >= required)
            return L".";

        path.resize(written);
        return path;
    }

    std::wstring QuoteArg(const std::wstring& value)
    {
        std::wstring out = L"\"";
        for (wchar_t ch : value)
        {
            if (ch == L'"')
                out += L'\\';
            out += ch;
        }
        out += L"\"";
        return out;
    }

    std::string StatusName(uint32_t status)
    {
        switch (static_cast<L4D2VROpenXrBridgeStatus>(status))
        {
        case L4D2VROpenXrBridgeStatus::Idle: return "Idle";
        case L4D2VROpenXrBridgeStatus::Starting: return "Starting";
        case L4D2VROpenXrBridgeStatus::LoaderLoaded: return "LoaderLoaded";
        case L4D2VROpenXrBridgeStatus::InstanceCreated: return "InstanceCreated";
        case L4D2VROpenXrBridgeStatus::SessionCreated: return "SessionCreated";
        case L4D2VROpenXrBridgeStatus::SessionRunning: return "SessionRunning";
        case L4D2VROpenXrBridgeStatus::SubmittedFrame: return "SubmittedFrame";
        case L4D2VROpenXrBridgeStatus::Completed: return "Completed";
        case L4D2VROpenXrBridgeStatus::Failed: return "Failed";
        case L4D2VROpenXrBridgeStatus::WaitingForSharedTextures: return "WaitingForSharedTextures";
        case L4D2VROpenXrBridgeStatus::SharedTexturesReady: return "SharedTexturesReady";
        default: return "Unknown";
        }
    }

    bool SameSharedTextureDesc(
        const L4D2VROpenXrSharedTextureDesc& lhs,
        const L4D2VROpenXrSharedTextureDesc& rhs)
    {
        return lhs.valid == rhs.valid &&
            lhs.width == rhs.width &&
            lhs.height == rhs.height &&
            lhs.format == rhs.format &&
            lhs.sampleCount == rhs.sampleCount &&
            lhs.handleType == rhs.handleType &&
            lhs.queueFamilyIndex == rhs.queueFamilyIndex &&
            lhs.kmtHandle == rhs.kmtHandle &&
            lhs.image == rhs.image &&
            lhs.uMin == rhs.uMin &&
            lhs.vMin == rhs.vMin &&
            lhs.uMax == rhs.uMax &&
            lhs.vMax == rhs.vMax &&
            lhs.renderFovXDeg == rhs.renderFovXDeg &&
            lhs.renderAspect == rhs.renderAspect;
    }
}

OpenXrHelperLaunchConfig L4D2VR_ReadOpenXrHelperLaunchConfig()
{
    OpenXrHelperLaunchConfig config{};
    const auto values = ReadConfigValues();

    auto get = [&](const char* key) -> std::string
        {
            const auto it = values.find(key);
            return it != values.end() ? it->second : std::string();
        };

    if (const std::string value = get("OpenXRHelper"); !value.empty())
        config.enabled = ParseBool(value, config.enabled);
    if (const std::string value = get("OpenXRHelperSubmitTestFrames"); !value.empty())
        config.submitTestFrames = ParseUint(value, config.submitTestFrames, 0, 1000000);
    if (const std::string value = get("OpenXRHelperWaitReadySeconds"); !value.empty())
        config.waitReadySeconds = ParseUint(value, config.waitReadySeconds, 1, 600);

    return config;
}

bool L4D2VR_StartOpenXrHelper(const OpenXrHelperLaunchConfig& config)
{
    if (!config.enabled)
        return false;

    bool expected = false;
    if (!g_OpenXrHelperStarted.compare_exchange_strong(expected, true))
        return true;

    const std::wstring gameRoot = CurrentDirectoryPath();
    const std::wstring helperDir = gameRoot + L"\\VR\\openxr_helper64";
    const std::wstring helperExe = helperDir + L"\\OpenXRHelper64.exe";
    const std::wstring helperLog = helperDir + L"\\openxr_helper64_from_game.log";

    if (GetFileAttributesW(helperExe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Game::logMsg("[VR][OpenXRHelper] helper executable not found: %ls", helperExe.c_str());
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    wchar_t mappingName[128] = {};
    std::swprintf(
        mappingName,
        ARRAYSIZE(mappingName),
        L"Local\\L4D2VR_OpenXRHelper_%lu_%lu",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetTickCount()));

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(L4D2VROpenXrBridgeState),
        mappingName);
    if (!mapping)
    {
        Game::logMsg("[VR][OpenXRHelper] CreateFileMapping failed err=%lu", GetLastError());
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    auto* state = static_cast<L4D2VROpenXrBridgeState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(L4D2VROpenXrBridgeState)));
    if (!state)
    {
        Game::logMsg("[VR][OpenXRHelper] MapViewOfFile failed err=%lu", GetLastError());
        CloseHandle(mapping);
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    *state = L4D2VROpenXrBridgeState{};
    state->gamePid = GetCurrentProcessId();
    state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::Starting);
    state->heartbeatTickMs = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
        g_OpenXrBridgeState = state;
    }

    std::wstringstream commandLine;
    commandLine
        << QuoteArg(helperExe)
        << L" --mapping " << QuoteArg(mappingName)
        << L" --parent " << GetCurrentProcessId()
        << L" --frames " << config.submitTestFrames
        << L" --wait-ready-sec " << config.waitReadySeconds
        << L" --log " << QuoteArg(helperLog);

    std::wstring commandLineText = commandLine.str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
        nullptr,
        commandLineText.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        helperDir.c_str(),
        &si,
        &pi))
    {
        const DWORD err = GetLastError();
        Game::logMsg("[VR][OpenXRHelper] CreateProcess failed err=%lu", err);
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::Failed);
        state->exitCode = static_cast<int32_t>(err);
        {
            std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
            if (g_OpenXrBridgeState == state)
                g_OpenXrBridgeState = nullptr;
        }
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    state->helperPid = pi.dwProcessId;
    Game::logMsg(
        "[VR][OpenXRHelper] launched pid=%lu frames=%u waitReadySeconds=%u exe=%ls",
        static_cast<unsigned long>(pi.dwProcessId),
        config.submitTestFrames,
        config.waitReadySeconds,
        helperExe.c_str());

    CloseHandle(pi.hThread);

    std::thread([process = pi.hProcess, mapping, state]()
        {
            const DWORD wait = WaitForSingleObject(process, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(process, &exitCode);

            uint32_t status = 0;
            uint32_t submittedFrames = 0;
            char detailCopy[256] = {};
            {
                std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
                if (state)
                {
                    status = state->status;
                    submittedFrames = state->submittedFrames;
                    std::snprintf(detailCopy, sizeof(detailCopy), "%s", state->detail);
                }
                if (g_OpenXrBridgeState == state)
                    g_OpenXrBridgeState = nullptr;
            }
            Game::logMsg(
                "[VR][OpenXRHelper] exited wait=0x%08lX code=%lu status=%s submittedFrames=%u detail=%s",
                static_cast<unsigned long>(wait),
                static_cast<unsigned long>(exitCode),
                StatusName(status).c_str(),
                submittedFrames,
                detailCopy);

            if (state)
                UnmapViewOfFile(state);
            CloseHandle(mapping);
            CloseHandle(process);
            g_OpenXrHelperStarted.store(false);
        }).detach();

    return true;
}

bool L4D2VR_OpenXrHelperBridgeIsStarted()
{
    return g_OpenXrHelperStarted.load();
}

bool L4D2VR_OpenXrHelperHasSubmittedFrame()
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    return state->submittedFrames > 0 &&
        state->status == static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::SubmittedFrame);
}

bool L4D2VR_ReadOpenXrHmdPose(L4D2VROpenXrPoseDesc& pose, uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t gen0 = state->trackingPoseGeneration;
        if (gen0 == 0 || (gen0 & 1u))
            continue;

        L4D2VROpenXrPoseDesc snapshot = state->hmdPose;
        const uint32_t gen1 = state->trackingPoseGeneration;
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

bool L4D2VR_ReadOpenXrRuntimeViewConfig(L4D2VROpenXrRuntimeViewConfigDesc& config, uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t gen0 = state->runtimeViewConfigGeneration;
        if (gen0 == 0 || (gen0 & 1u))
            continue;

        L4D2VROpenXrRuntimeViewConfigDesc snapshot = state->runtimeViewConfig;
        const uint32_t gen1 = state->runtimeViewConfigGeneration;
        if (gen0 == gen1 && !(gen1 & 1u) && snapshot.valid)
        {
            config = snapshot;
            if (generation)
                *generation = gen1;
            return true;
        }
    }

    return false;
}

void L4D2VR_PublishOpenXrGameRenderPose(const L4D2VROpenXrPoseDesc& pose)
{
    if (!pose.valid)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    ++state->gameRenderPoseGeneration;
    state->gameRenderPose = pose;
    ++state->gameRenderPoseGeneration;
    state->heartbeatTickMs = GetTickCount64();
}

void L4D2VR_PublishOpenXrSharedTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& texture)
{
    if (eyeIndex >= L4D2VR_OPENXR_EYE_COUNT || !texture.valid)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    const uint32_t readyBit = (1u << eyeIndex);
    if ((state->sharedTexturesReadyMask & readyBit) &&
        SameSharedTextureDesc(state->eyeTextures[eyeIndex], texture))
    {
        state->heartbeatTickMs = GetTickCount64();
        return;
    }

    state->eyeTextures[eyeIndex] = texture;
    state->sharedTexturesReadyMask |= readyBit;
    ++state->sharedTextureGeneration;
    state->heartbeatTickMs = GetTickCount64();

    if ((state->sharedTexturesReadyMask & L4D2VR_OPENXR_EYES_READY_MASK) == L4D2VR_OPENXR_EYES_READY_MASK)
    {
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::SharedTexturesReady);
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "shared eye textures ready gen=%u L=%ux%u R=%ux%u",
            state->sharedTextureGeneration,
            state->eyeTextures[L4D2VR_OPENXR_EYE_LEFT].width,
            state->eyeTextures[L4D2VR_OPENXR_EYE_LEFT].height,
            state->eyeTextures[L4D2VR_OPENXR_EYE_RIGHT].width,
            state->eyeTextures[L4D2VR_OPENXR_EYE_RIGHT].height);
    }
    else
    {
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::WaitingForSharedTextures);
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "waiting for shared eye textures mask=0x%X",
            state->sharedTexturesReadyMask);
    }
}

void L4D2VR_PublishOpenXrSharedTextureFrame(uint32_t frameId)
{
    if (frameId == 0)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    if ((state->sharedTexturesReadyMask & L4D2VR_OPENXR_EYES_READY_MASK) != L4D2VR_OPENXR_EYES_READY_MASK)
        return;

    ++state->sharedTextureFrameGeneration;
    state->sharedTextureFrameId = frameId;
    ++state->sharedTextureFrameGeneration;
    state->heartbeatTickMs = GetTickCount64();
}
