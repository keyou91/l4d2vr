namespace
{
    constexpr int kEffectDimLight = 0x4; // Source EF_DIMLIGHT
    constexpr int kAutoFlashlightReadbackWidth = 32;
    constexpr int kAutoFlashlightReadbackHeight = 18;
    constexpr float kAutoFlashlightSampleIntervalSeconds = 0.25f;
    constexpr float kAutoFlashlightDarkHoldSeconds = 0.60f;
    constexpr float kAutoFlashlightBrightHoldSeconds = 1.50f;
    constexpr float kAutoFlashlightMinOnSeconds = 2.00f;
    constexpr float kAutoFlashlightMinOffSeconds = 0.80f;
    constexpr float kAutoFlashlightManualOverrideSeconds = 6.0f;

    // Avoid referencing the external IID_IDirect3DTexture9 symbol.
    static const GUID kAutoFlashlightIID_IDirect3DTexture9 =
    {
        0x85c31227, 0x3de5, 0x4f00,
        { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 }
    };

    constexpr UINT kSystemMouseInputSuppressQuitMessage = WM_APP + 0x4D53;
    std::atomic<long> g_SystemMouseInputSuppressActive{ 0 };
    std::atomic<long> g_SystemMouseInputSuppressStop{ 0 };
    std::atomic<DWORD> g_SystemMouseInputSuppressProcessId{ 0 };
    std::atomic<DWORD> g_SystemMouseInputSuppressThreadId{ 0 };
    HANDLE g_SystemMouseInputSuppressThreadHandle = nullptr;
    std::mutex g_SystemMouseInputSuppressThreadMutex;

    inline bool SystemMouseInputSuppressForegroundIsCurrentProcess()
    {
        const DWORD processId = g_SystemMouseInputSuppressProcessId.load(std::memory_order_acquire);
        if (processId == 0)
            return false;

        HWND foreground = GetForegroundWindow();
        if (!foreground)
            return false;

        for (HWND hwnd = foreground; hwnd != nullptr; hwnd = GetWindow(hwnd, GW_OWNER))
        {
            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(hwnd, &foregroundProcessId);
            if (foregroundProcessId == processId)
                return true;
        }

        return false;
    }

    LRESULT CALLBACK SystemMouseInputSuppressLowLevelProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION &&
            g_SystemMouseInputSuppressActive.load(std::memory_order_acquire) != 0 &&
            SystemMouseInputSuppressForegroundIsCurrentProcess())
        {
            return 1;
        }

        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    DWORD WINAPI SystemMouseInputSuppressThreadMain(LPVOID)
    {
        const DWORD threadId = GetCurrentThreadId();
        g_SystemMouseInputSuppressThreadId.store(threadId, std::memory_order_release);

        MSG msg{};
        PeekMessageA(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        HMODULE module = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&SystemMouseInputSuppressLowLevelProc),
            &module);

        HHOOK hook = SetWindowsHookExA(WH_MOUSE_LL, SystemMouseInputSuppressLowLevelProc, module, 0);
        if (!hook)
        {
            Game::logMsg("[ERROR] Failed to install system mouse suppression hook, error=%lu", GetLastError());
        }
        else
        {
            Game::logMsg("[VR][MouseSuppress] low-level mouse hook installed");
        }

        while (g_SystemMouseInputSuppressStop.load(std::memory_order_acquire) == 0)
        {
            const BOOL gotMessage = GetMessageA(&msg, nullptr, 0, 0);
            if (gotMessage <= 0)
                break;
            if (msg.message == kSystemMouseInputSuppressQuitMessage)
                break;
        }

        if (hook)
            UnhookWindowsHookEx(hook);

        g_SystemMouseInputSuppressActive.store(0, std::memory_order_release);
        g_SystemMouseInputSuppressThreadId.store(0, std::memory_order_release);
        return 0;
    }

    inline void EnsureSystemMouseInputSuppressThreadStarted()
    {
        std::lock_guard<std::mutex> lock(g_SystemMouseInputSuppressThreadMutex);
        if (g_SystemMouseInputSuppressThreadHandle)
            return;

        g_SystemMouseInputSuppressStop.store(0, std::memory_order_release);
        g_SystemMouseInputSuppressProcessId.store(GetCurrentProcessId(), std::memory_order_release);

        DWORD threadId = 0;
        HANDLE handle = CreateThread(nullptr, 0, SystemMouseInputSuppressThreadMain, nullptr, 0, &threadId);
        if (!handle)
        {
            Game::logMsg("[ERROR] Failed to create system mouse suppression hook thread, error=%lu", GetLastError());
            return;
        }

        g_SystemMouseInputSuppressThreadHandle = handle;
    }

    class SourceRenderCompletionFunctor final : public CFunctor
    {
    public:
        SourceRenderCompletionFunctor(
            VR* vr,
            uint32_t epoch,
            uint32_t markerId,
            uint32_t renderPoseToken,
            uint32_t renderFrameSeq,
            bool allowDuplicatePoseSubmit,
            const vr::HmdMatrix34_t* renderHmdPose)
            : m_VR(vr),
            m_Epoch(epoch),
            m_MarkerId(markerId),
            m_RenderPoseToken(renderPoseToken),
            m_RenderFrameSeq(renderFrameSeq),
            m_AllowDuplicatePoseSubmit(allowDuplicatePoseSubmit)
        {
            if (renderHmdPose)
            {
                m_RenderHmdPose = *renderHmdPose;
                m_RenderHmdPoseValid = true;
            }
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            VR* vr = m_VR;
            if (!vr)
                return;

            if (vr->m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != m_Epoch)
                return;

            vr->PublishSourceQueueCompletedFrame(
                m_Epoch,
                m_RenderPoseToken,
                m_RenderFrameSeq,
                m_AllowDuplicatePoseSubmit,
                m_MarkerId,
                m_RenderHmdPoseValid ? &m_RenderHmdPose : nullptr);
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        VR* m_VR = nullptr;
        uint32_t m_Epoch = 0;
        uint32_t m_MarkerId = 0;
        uint32_t m_RenderPoseToken = 0;
        uint32_t m_RenderFrameSeq = 0;
        bool m_AllowDuplicatePoseSubmit = false;
        vr::HmdMatrix34_t m_RenderHmdPose{};
        bool m_RenderHmdPoseValid = false;
    };

    class SourceRenderOwnershipReleaseFunctor final : public CFunctor
    {
    public:
        SourceRenderOwnershipReleaseFunctor(VR* vr, uint32_t epoch)
            : m_VR(vr), m_Epoch(epoch)
        {
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            VR* vr = m_VR;
            if (!vr || vr->m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != m_Epoch)
                return;

            uint32_t pending = vr->m_SourceRenderQueueAuxPendingCount.load(std::memory_order_acquire);
            while (pending != 0 &&
                !vr->m_SourceRenderQueueAuxPendingCount.compare_exchange_weak(
                    pending, pending - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
            }
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        VR* m_VR = nullptr;
        uint32_t m_Epoch = 0;
    };

    class VrHandsQueuedDrawFunctor final : public CFunctor
    {
    public:
        VrHandsQueuedDrawFunctor(
            VR* vr,
            uint32_t epoch,
            const CViewSetup& view,
            int eyeIndex,
            VrHandDrawPass drawPass)
            : m_VR(vr),
            m_Epoch(epoch),
            m_View(view),
            m_EyeIndex(eyeIndex),
            m_DrawPass(drawPass)
        {
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            VR* vr = m_VR;
            if (!vr)
                return;

            if (vr->m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != m_Epoch)
                return;

            if (m_DrawPass == VrHandDrawPass::WorldVisibilityMask)
            {
                vr->DrawVrHandsWorldDepthMaskForEyeImmediate(m_View, m_EyeIndex, true);
                return;
            }

            vr->DrawVrHandsForEyeImmediate(m_View, m_EyeIndex, m_DrawPass, true);
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        VR* m_VR = nullptr;
        uint32_t m_Epoch = 0;
        CViewSetup m_View{};
        int m_EyeIndex = -1;
        VrHandDrawPass m_DrawPass = VrHandDrawPass::WorldDepth;
    };

    inline bool IsSourceQueueReadableProtection(DWORD protect)
    {
        if (protect & PAGE_GUARD)
            return false;

        switch (protect & 0xff)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    inline bool IsSourceQueueReadableMemory(const void* ptr, size_t bytes)
    {
        if (!ptr || bytes == 0)
            return false;

        const uint8_t* p = reinterpret_cast<const uint8_t*>(ptr);
        size_t remaining = bytes;

        while (remaining > 0)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi)))
                return false;
            if (mbi.State != MEM_COMMIT)
                return false;
            if (!IsSourceQueueReadableProtection(mbi.Protect))
                return false;

            const uintptr_t cur = reinterpret_cast<uintptr_t>(p);
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= cur)
                return false;

            const size_t chunk = std::min<size_t>(remaining, size_t(regionEnd - cur));
            p += chunk;
            remaining -= chunk;
        }

        return true;
    }

    inline bool IsSourceCallQueueUsable(ICallQueue* callQueue, void** outVtable)
    {
        if (outVtable)
            *outVtable = nullptr;

        if (!IsSourceQueueReadableMemory(callQueue, sizeof(void*)))
            return false;

        void* const vtable = *reinterpret_cast<void**>(callQueue);
        if (outVtable)
            *outVtable = vtable;

        return IsSourceQueueReadableMemory(vtable, sizeof(void*));
    }

    inline ICallQueue* GetSourceRenderContextCallQueue(
        IMatRenderContext* renderContext,
        void** outContextVtable,
        void** outGetCallQueueFn)
    {
        if (outContextVtable)
            *outContextVtable = nullptr;
        if (outGetCallQueueFn)
            *outGetCallQueueFn = nullptr;

        if (!IsSourceQueueReadableMemory(renderContext, sizeof(void*)))
            return nullptr;

        void** const contextVtable = *reinterpret_cast<void***>(renderContext);
        if (outContextVtable)
            *outContextVtable = contextVtable;

        constexpr size_t kGetCallQueueAbsoluteSlot = 142;
        if (!IsSourceQueueReadableMemory(contextVtable + kGetCallQueueAbsoluteSlot, sizeof(void*)))
            return nullptr;

        void* const getCallQueuePtr = contextVtable[kGetCallQueueAbsoluteSlot];
        if (outGetCallQueueFn)
            *outGetCallQueueFn = getCallQueuePtr;
        if (!IsSourceQueueReadableMemory(getCallQueuePtr, 1))
            return nullptr;

        using GetCallQueueFn = ICallQueue*(__thiscall*)(IMatRenderContext*);
        return reinterpret_cast<GetCallQueueFn>(getCallQueuePtr)(renderContext);
    }

    inline uint32_t SourceMarkerLag(uint32_t queuedMarkerId, uint32_t completedMarkerId)
    {
        return queuedMarkerId - completedMarkerId;
    }

    inline float ComputePerceivedLuma(float r, float g, float b)
    {
        return 0.2126f * r +
            0.7152f * g +
            0.0722f * b;
    }

    inline float ComputePerceivedLumaFromArgb(uint32_t argb)
    {
        const float r = static_cast<float>((argb >> 16) & 0xFFu);
        const float g = static_cast<float>((argb >> 8) & 0xFFu);
        const float b = static_cast<float>(argb & 0xFFu);
        return ComputePerceivedLuma(r, g, b);
    }

    inline float ComputePercentile(std::vector<float> values, float percentile)
    {
        if (values.empty())
            return 255.0f;

        percentile = std::clamp(percentile, 0.0f, 1.0f);
        const size_t index = (std::min)(values.size() - 1,
            static_cast<size_t>(std::lround(percentile * static_cast<float>(values.size() - 1))));
        std::nth_element(values.begin(), values.begin() + index, values.end());
        return values[index];
    }

    struct FocusWindowSearchContext
    {
        DWORD processId = 0;
        HWND hwnd = nullptr;
    };

    inline BOOL CALLBACK FindFocusMainWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<FocusWindowSearchContext*>(lParam);
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

    inline HWND FindFocusMainWindow()
    {
        FocusWindowSearchContext ctx;
        ctx.processId = GetCurrentProcessId();
        EnumWindows(FindFocusMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hwnd;
    }

    inline bool IsCurrentProcessForegroundForFocusRecovery()
    {
        HWND foreground = GetForegroundWindow();
        if (!foreground)
            return true;

        DWORD foregroundProcessId = 0;
        GetWindowThreadProcessId(foreground, &foregroundProcessId);
        if (foregroundProcessId == GetCurrentProcessId())
            return true;

        HWND owner = GetWindow(foreground, GW_OWNER);
        if (owner)
        {
            DWORD ownerProcessId = 0;
            GetWindowThreadProcessId(owner, &ownerProcessId);
            if (ownerProcessId == GetCurrentProcessId())
                return true;
        }

        return false;
    }

    inline bool IsMainWindowDrawableForFocusRecovery()
    {
        HWND hwnd = FindFocusMainWindow();
        if (!hwnd)
            return true;
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
            return false;

        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return true;

        return rc.right > rc.left && rc.bottom > rc.top;
    }

    struct FocusShadowCvarSnapshot
    {
        int shadows = 1;
        int renderToTexture = 1;
        int flashlightDepthTexture = 1;
        int flashlightDepthRes = 1024;
        int halfUpdateRate = 0;
        int maxRendered = 16;
        bool valid = false;
    };

    inline void CaptureFocusShadowCvars(Game* game, FocusShadowCvarSnapshot& snapshot)
    {
        if (!game)
            return;

        snapshot.shadows = game->GetConVarIntDirect("r_shadows", snapshot.shadows);
        snapshot.renderToTexture = game->GetConVarIntDirect("r_shadowrendertotexture", snapshot.renderToTexture);
        snapshot.flashlightDepthTexture = game->GetConVarIntDirect("r_flashlightdepthtexture", snapshot.flashlightDepthTexture);
        snapshot.flashlightDepthRes = game->GetConVarIntDirect("r_flashlightdepthres", snapshot.flashlightDepthRes);
        snapshot.halfUpdateRate = game->GetConVarIntDirect("r_shadow_half_update_rate", snapshot.halfUpdateRate);
        snapshot.maxRendered = game->GetConVarIntDirect("r_shadowmaxrendered", snapshot.maxRendered);
        snapshot.valid = true;
    }

    inline void RestoreFocusShadowCvars(Game* game, const FocusShadowCvarSnapshot& snapshot)
    {
        if (!game || !snapshot.valid)
            return;

        game->SetConVarInt("r_shadows", snapshot.shadows);
        game->SetConVarInt("r_shadowrendertotexture", snapshot.renderToTexture);
        game->SetConVarInt("r_flashlightdepthtexture", snapshot.flashlightDepthTexture);
        game->SetConVarInt("r_flashlightdepthres", snapshot.flashlightDepthRes);
        game->SetConVarInt("r_shadow_half_update_rate", snapshot.halfUpdateRate);
        game->SetConVarInt("r_shadowmaxrendered", snapshot.maxRendered);
    }

    inline void UpdateFocusShadowRecovery(VR* vr, bool inGame)
    {
        if (!vr || !vr->m_Game)
            return;

        struct RecoveryState
        {
            bool initialized = false;
            bool wasWindowUsable = true;
            int restoreFrames = 0;
            bool rebuildRenderToTexture = false;
            FocusShadowCvarSnapshot shadow{};
        };
        static RecoveryState state;

        const bool windowUsable =
            IsCurrentProcessForegroundForFocusRecovery() &&
            IsMainWindowDrawableForFocusRecovery();

        if (!state.initialized)
        {
            state.initialized = true;
            state.wasWindowUsable = windowUsable;
            if (inGame && windowUsable)
                CaptureFocusShadowCvars(vr->m_Game, state.shadow);
        }

        if (inGame && state.wasWindowUsable && !windowUsable)
        {
            CaptureFocusShadowCvars(vr->m_Game, state.shadow);
            state.restoreFrames = 0;
            state.rebuildRenderToTexture = false;
        }
        else if (inGame && !state.wasWindowUsable && windowUsable)
        {
            state.restoreFrames = 8;
            state.rebuildRenderToTexture = state.shadow.valid && state.shadow.renderToTexture != 0;
        }
        else if (!inGame)
        {
            state.restoreFrames = 0;
            state.rebuildRenderToTexture = false;
            state.shadow.valid = false;
        }

        state.wasWindowUsable = windowUsable;

        if (!inGame || !windowUsable || state.restoreFrames <= 0 || !state.shadow.valid)
            return;

        if (state.rebuildRenderToTexture)
        {
            // Focus loss can leave Source's shadow RTT/atlas in a valid-but-stale state.
            // Flip the shadow RTT cvar only for this restore point, then put back the
            // value the user had before Alt-Tab/minimize.
            vr->m_Game->SetConVarInt("r_shadowrendertotexture", 0);
            vr->m_Game->SetConVarInt("r_shadowrendertotexture", state.shadow.renderToTexture);
            state.rebuildRenderToTexture = false;
        }

        RestoreFocusShadowCvars(vr->m_Game, state.shadow);
        --state.restoreFrames;
    }

    inline bool SafeScanItemModelLabelEntities(VR* vr)
    {
        if (!vr)
            return false;

#ifdef _MSC_VER
        __try
        {
            vr->ScanItemModelLabelEntitiesFromClientList();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        vr->ScanItemModelLabelEntitiesFromClientList();
        return true;
#endif
    }
}

void VR::ResetAutoFlashlightState()
{
    m_AutoFlashlightHasScreenLuma = false;
    m_AutoFlashlightCenterMedianLuma = 255.0f;
    m_AutoFlashlightCenterLowLuma = 255.0f;
    m_AutoFlashlightPeripheralMedianLuma = 255.0f;
    m_AutoFlashlightHasKnownState = false;
    m_AutoFlashlightLastKnownOn = false;
    m_AutoFlashlightDarkDecisionSamples = 0;
    m_AutoFlashlightBrightDecisionSamples = 0;
    m_AutoFlashlightNextSampleTime = {};
    m_AutoFlashlightLastToggleTime = {};
    m_AutoFlashlightManualOverrideUntil = {};
}

bool VR::QueryFlashlightState(C_BasePlayer* localPlayer, bool& outOn)
{
    if (!m_Game || !localPlayer)
        return false;

    const int effects = m_Game->GetEntityEffects(localPlayer, 0x7fffffff);
    if (effects == 0x7fffffff)
        return false;

    outOn = (effects & kEffectDimLight) != 0;
    m_AutoFlashlightHasKnownState = true;
    m_AutoFlashlightLastKnownOn = outOn;
    return true;
}

void VR::IssueFlashlightToggle(bool manual)
{
    if (!m_Game)
        return;

    m_Game->ClientCmd_Unrestricted("impulse 100");

    const auto now = std::chrono::steady_clock::now();
    m_AutoFlashlightLastToggleTime = now;
    if (m_AutoFlashlightHasKnownState)
        m_AutoFlashlightLastKnownOn = !m_AutoFlashlightLastKnownOn;
    m_AutoFlashlightDarkDecisionSamples = 0;
    m_AutoFlashlightBrightDecisionSamples = 0;

    if (manual)
    {
        m_AutoFlashlightManualOverrideUntil =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(kAutoFlashlightManualOverrideSeconds));
    }
}


namespace
{
    static void ClearRenderTargetColorForMenuTransition(VR* vr, IMatRenderContext* ctx, ITexture* target, int width, int height, unsigned char alpha)
    {
        if (!vr || !ctx || !target)
            return;

        if (width <= 0)
            width = target->GetMappingWidth();
        if (height <= 0)
            height = target->GetMappingHeight();
        width = (std::max)(1, width);
        height = (std::max)(1, height);

        ctx->SetRenderTarget(target);
        if (Hooks::hkViewport.fOriginal)
            Hooks::hkViewport.fOriginal(ctx, 0, 0, width, height);

        ctx->OverrideAlphaWriteEnable(true, true);
        ctx->ClearColor4ub(0, 0, 0, alpha);
        ctx->ClearBuffers(true, true, true);
        ctx->OverrideAlphaWriteEnable(false, true);
    }

    static bool ClearD3D9BlankTextureForMenuSubmit(VR* vr, const char* reason)
    {
        if (!vr || !g_D3DVR9)
            return false;

        IDirect3DSurface9* blankSurface = nullptr;
        {
            std::lock_guard<TextureStateMutex> textureLock(vr->m_TextureMutex);
            if (vr->m_CreatingTextureID != VR::Texture_None ||
                !vr->m_D9BlankSurface ||
                !vr->m_VKBlankTexture.m_VRTexture.handle)
            {
                return false;
            }

            blankSurface = vr->m_D9BlankSurface;
            blankSurface->AddRef();
        }

        IDirect3DDevice9* device = nullptr;
        HRESULT hr = blankSurface->GetDevice(&device);
        if (FAILED(hr) || !device)
        {
            blankSurface->Release();
            return false;
        }

        D3DSURFACE_DESC desc{};
        hr = blankSurface->GetDesc(&desc);
        if (FAILED(hr) || desc.Width == 0 || desc.Height == 0)
        {
            device->Release();
            blankSurface->Release();
            return false;
        }

        g_D3DVR9->LockDevice();
        bool locked = true;
        auto unlockDevice = [&]()
            {
                if (locked)
                {
                    g_D3DVR9->UnlockDevice();
                    locked = false;
                }
            };

        const HRESULT cooperativeHr = device->TestCooperativeLevel();
        if (cooperativeHr == D3DERR_DEVICELOST || cooperativeHr == D3DERR_DEVICENOTRESET || cooperativeHr == D3DERR_DRIVERINTERNALERROR)
        {
            unlockDevice();
            if (vr->m_RenderPipelineDebugLog)
            {
                Game::logMsg("[VR][MenuBlank] skip D3D clear during device transition reason=%s hr=0x%08X",
                    reason ? reason : "unknown", static_cast<unsigned int>(cooperativeHr));
            }
            device->Release();
            blankSurface->Release();
            return false;
        }

        IDirect3DSurface9* oldRenderTarget = nullptr;
        const HRESULT oldRtHr = device->GetRenderTarget(0, &oldRenderTarget);

        D3DVIEWPORT9 oldViewport{};
        const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

        D3DVIEWPORT9 viewport{};
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = desc.Width;
        viewport.Height = desc.Height;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;

        HRESULT clearHr = device->SetRenderTarget(0, blankSurface);
        if (SUCCEEDED(clearHr))
            clearHr = device->SetViewport(&viewport);
        if (SUCCEEDED(clearHr))
            clearHr = device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);

        HRESULT transferHr = clearHr;
        if (SUCCEEDED(clearHr))
            transferHr = g_D3DVR9->TransferSurface(blankSurface, TRUE);

        if (SUCCEEDED(oldRtHr))
            device->SetRenderTarget(0, oldRenderTarget);
        if (haveOldViewport)
            device->SetViewport(&oldViewport);
        if (oldRenderTarget)
            oldRenderTarget->Release();

        unlockDevice();

        if (vr->m_RenderPipelineDebugLog)
        {
            Game::logMsg("[VR][MenuBlank] D3D-cleared blank texture reason=%s clear=0x%08X transfer=0x%08X size=%ux%u",
                reason ? reason : "unknown",
                static_cast<unsigned int>(clearHr),
                static_cast<unsigned int>(transferHr),
                static_cast<unsigned int>(desc.Width),
                static_cast<unsigned int>(desc.Height));
        }

        device->Release();
        blankSurface->Release();
        return SUCCEEDED(clearHr) && SUCCEEDED(transferHr);
    }

    static void ClearVRMenuTransitionResiduals(VR* vr, const char* reason)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_MaterialSystem)
            return;

        IMatRenderContext* ctx = vr->m_Game->m_MaterialSystem->GetRenderContext();
        if (!ctx)
        {
            vr->HandleMissingRenderContext("ClearVRMenuTransitionResiduals");
            return;
        }

        std::lock_guard<TextureStateMutex> textureLock(vr->m_TextureMutex);

        ITexture* oldRT = ctx->GetRenderTarget();
        int oldX = 0;
        int oldY = 0;
        int oldW = 0;
        int oldH = 0;
        const bool restoreViewport = Hooks::hkGetViewport.fOriginal && Hooks::hkViewport.fOriginal;
        if (restoreViewport)
            Hooks::hkGetViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);

        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_LeftEyeTexture,
            static_cast<int>(vr->m_RenderWidth), static_cast<int>(vr->m_RenderHeight), 255);
        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_RightEyeTexture,
            static_cast<int>(vr->m_RenderWidth), static_cast<int>(vr->m_RenderHeight), 255);
        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_LeftEyeSubmitTexture,
            static_cast<int>(vr->m_RenderWidth), static_cast<int>(vr->m_RenderHeight), 255);
        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_RightEyeSubmitTexture,
            static_cast<int>(vr->m_RenderWidth), static_cast<int>(vr->m_RenderHeight), 255);
        if (vr->m_DesktopMirrorEnabled)
        {
            ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_DesktopMirrorTexture,
                static_cast<int>(vr->m_RenderWidth), static_cast<int>(vr->m_RenderHeight), 255);
        }

        // HUD overlay must be transparent. Otherwise the next map can show the last captured HUD
        // before the new map's first VGUI paint refreshes vrHUD.
        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_HUDTexture, 0, 0, 0);

        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_ScopeTexture,
            static_cast<int>(vr->m_ScopeRTTSize), static_cast<int>(vr->m_ScopeRTTSize), 255);
        vr->m_QueuedScopeLensPostProcessPending.store(0, std::memory_order_release);
        vr->m_ScopeLensOverlayReady.store(0, std::memory_order_release);
        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_RearMirrorTexture,
            static_cast<int>(vr->m_RearMirrorRTTSize), static_cast<int>(vr->m_RearMirrorRTTSize), 255);
        ClearRenderTargetColorForMenuTransition(vr, ctx, vr->m_BlankTexture, 0, 0, 255);

        ctx->SetRenderTarget(oldRT);
        if (restoreViewport)
            Hooks::hkViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);

        vr->m_RenderedNewFrame.store(false, std::memory_order_release);
        vr->m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
        vr->m_RenderCompletedPoseToken.store(0, std::memory_order_release);
        vr->m_RenderCompletedDuplicatePoseFrameId.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> poseLock(vr->m_RenderCompletedHmdPoseMutex);
            vr->m_RenderCompletedHmdPoseValid = false;
        }
        vr->m_ReShadeVRCompatResolvedFrameId.store(0, std::memory_order_release);
        vr->m_PostPresentSubmitOverlayFrameId.store(0, std::memory_order_release);
        vr->m_ReShadeVRCompatPendingRenderReady.store(0, std::memory_order_release);
        vr->m_ReShadeVRCompatPendingRenderPoseToken.store(0, std::memory_order_release);
        vr->m_ReShadeVRCompatPendingRenderFrameSeq.store(0, std::memory_order_release);
        vr->m_ReShadeVRCompatPendingDuplicatePose.store(0, std::memory_order_release);
        vr->m_LastSubmittedPoseToken.store(0, std::memory_order_release);
        vr->m_SubmitInFlight.store(false, std::memory_order_release);
        vr->m_QueuedSubmitStaleStreak.store(0, std::memory_order_release);
        vr->m_RenderedHud.store(false, std::memory_order_release);
        vr->m_HudPaintedThisFrame.store(false, std::memory_order_release);
        vr->ClearQueuedHudFresh();
        vr->m_MenuBlankSubmitted = false;

        if (vr->m_RenderPipelineDebugLog)
            Game::logMsg("[VR][MenuTransition] cleared stale scene/HUD render targets reason=%s", reason ? reason : "unknown");
    }
}

namespace
{
    void UpdateDesktopMirrorOverlayHideEffective(VR* vr, bool queuedRendering)
    {
        if (!vr)
            return;

        // The user-facing setting is a request; the runtime flag is true only when the
        // clean desktop mirror target exists and rendering is single-threaded. Queued mode
        // must not insert a separate clean world RenderView: the two VR eye passes already
        // exercise Source's shared shadow RTT path heavily, and the extra pass can corrupt
        // persistent shadow output after a scene-pressure spike.

        const bool requested = vr->m_DesktopMirrorEnabled && vr->m_DesktopMirrorHidePluginOverlaysRequested;
        const bool texturesReady = vr->m_CreatedVRTextures.load(std::memory_order_acquire);
        const bool cleanTargetReady = (vr->m_DesktopMirrorTexture != nullptr);
        const bool cleanCopySafe = !queuedRendering;
        const bool effective = requested && cleanTargetReady && cleanCopySafe;
        const bool needRecreate = requested && cleanCopySafe && texturesReady && !vr->m_DesktopMirrorTexture;

        const bool changed = (vr->m_DesktopMirrorHidePluginOverlays != effective);
        vr->m_DesktopMirrorHidePluginOverlays = effective;

        // Older builds did not allocate desktopMirrorClean0 while mat_queue_mode was active.
        // If a live session enters that state, recreate all VR RTs once so the desktop mirror
        // never points at a missing clean target. Keep the runtime flag false until that happens.
        if (needRecreate)
        {
            std::lock_guard<TextureStateMutex> textureLock(vr->m_TextureMutex);
            vr->m_CreatedVRTextures.store(false, std::memory_order_release);
            vr->m_CreatingTextureID = VR::Texture_None;
        }

        if ((changed || needRecreate) && vr->m_RenderPipelineDebugLog)
        {
            Game::logMsg("[VR][DesktopMirror] HidePluginOverlays requested=%d effective=%d queue=%d; recreate=%d cleanTarget=%d",
                requested ? 1 : 0, effective ? 1 : 0, queuedRendering ? 1 : 0,
                needRecreate ? 1 : 0, vr->m_DesktopMirrorTexture ? 1 : 0);
        }
    }
}

bool VR::SampleAutoFlashlightScreenLuma(float& outCenterMedianLuma, float& outCenterLowLuma, float& outPeripheralMedianLuma, float& outMeanLuma)
{
    outCenterMedianLuma = 255.0f;
    outCenterLowLuma = 255.0f;
    outPeripheralMedianLuma = 255.0f;
    outMeanLuma = 255.0f;

    if (!g_D3DVR9 || !m_CreatedVRTextures.load(std::memory_order_acquire))
        return false;

    IDirect3DSurface9* src = nullptr;
    int sourceEyeIndex = 0;
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (m_CreatingTextureID != Texture_None)
            return false;

        // Sample the raw eye RT and apply its SteamVR bounds during the GPU downsample.
        // The raw eye RT is the stable source in both normal and ReShade-compat paths.
        if (m_D9LeftEyeSurface)
        {
            src = m_D9LeftEyeSurface;
            sourceEyeIndex = 0;
        }
        else if (m_D9RightEyeSurface)
        {
            src = m_D9RightEyeSurface;
            sourceEyeIndex = 1;
        }
        if (!src)
            return false;
        src->AddRef();
    }

    auto releaseSrc = [&]()
        {
            if (src)
            {
                src->Release();
                src = nullptr;
            }
        };

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = src->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        releaseSrc();
        return false;
    }

    auto releaseDevice = [&]()
        {
            if (device)
            {
                device->Release();
                device = nullptr;
            }
        };

    g_D3DVR9->LockDevice();
    bool locked = true;
    auto unlockDevice = [&]()
        {
            if (locked)
            {
                g_D3DVR9->UnlockDevice();
                locked = false;
            }
        };

    const HRESULT cooperativeHr = device->TestCooperativeLevel();
    if (cooperativeHr == D3DERR_DEVICELOST || cooperativeHr == D3DERR_DEVICENOTRESET || cooperativeHr == D3DERR_DRIVERINTERNALERROR)
    {
        unlockDevice();
        releaseDevice();
        releaseSrc();
        return false;
    }

    D3DSURFACE_DESC srcDesc{};
    if (FAILED(src->GetDesc(&srcDesc)) || srcDesc.Width == 0 || srcDesc.Height == 0)
    {
        unlockDevice();
        releaseDevice();
        releaseSrc();
        return false;
    }

    auto releaseAutoFlashlightSurfaces = [&]()
        {
            if (m_D9AutoFlashlightReadbackSurface)
            {
                m_D9AutoFlashlightReadbackSurface->Release();
                m_D9AutoFlashlightReadbackSurface = nullptr;
            }
            if (m_D9AutoFlashlightLumaSurface)
            {
                m_D9AutoFlashlightLumaSurface->Release();
                m_D9AutoFlashlightLumaSurface = nullptr;
            }
            if (m_D9AutoFlashlightLumaTexture)
            {
                m_D9AutoFlashlightLumaTexture->Release();
                m_D9AutoFlashlightLumaTexture = nullptr;
            }
            m_AutoFlashlightReadbackWidth = 0;
            m_AutoFlashlightReadbackHeight = 0;
        };

    if (m_AutoFlashlightReadbackWidth != kAutoFlashlightReadbackWidth ||
        m_AutoFlashlightReadbackHeight != kAutoFlashlightReadbackHeight ||
        !m_D9AutoFlashlightLumaTexture || !m_D9AutoFlashlightLumaSurface || !m_D9AutoFlashlightReadbackSurface)
    {
        releaseAutoFlashlightSurfaces();

        hr = device->CreateTexture(
            kAutoFlashlightReadbackWidth,
            kAutoFlashlightReadbackHeight,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &m_D9AutoFlashlightLumaTexture,
            nullptr);
        if (FAILED(hr) || !m_D9AutoFlashlightLumaTexture)
        {
            releaseAutoFlashlightSurfaces();
            unlockDevice();
            releaseDevice();
            releaseSrc();
            return false;
        }

        hr = m_D9AutoFlashlightLumaTexture->GetSurfaceLevel(0, &m_D9AutoFlashlightLumaSurface);
        if (FAILED(hr) || !m_D9AutoFlashlightLumaSurface)
        {
            releaseAutoFlashlightSurfaces();
            unlockDevice();
            releaseDevice();
            releaseSrc();
            return false;
        }

        hr = device->CreateOffscreenPlainSurface(
            kAutoFlashlightReadbackWidth,
            kAutoFlashlightReadbackHeight,
            D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,
            &m_D9AutoFlashlightReadbackSurface,
            nullptr);
        if (FAILED(hr) || !m_D9AutoFlashlightReadbackSurface)
        {
            releaseAutoFlashlightSurfaces();
            unlockDevice();
            releaseDevice();
            releaseSrc();
            return false;
        }

        m_AutoFlashlightReadbackWidth = kAutoFlashlightReadbackWidth;
        m_AutoFlashlightReadbackHeight = kAutoFlashlightReadbackHeight;
    }

    IDirect3DTexture9* srcTexture = nullptr;
    hr = src->GetContainer(kAutoFlashlightIID_IDirect3DTexture9, reinterpret_cast<void**>(&srcTexture));
    if (FAILED(hr) || !srcTexture)
    {
        unlockDevice();
        releaseDevice();
        releaseSrc();
        return false;
    }

    IDirect3DSurface9* oldRenderTarget = nullptr;
    const HRESULT oldRtHr = device->GetRenderTarget(0, &oldRenderTarget);
    D3DVIEWPORT9 oldViewport{};
    const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();

    float u0 = std::clamp(m_TextureBounds[sourceEyeIndex].uMin, 0.0f, 1.0f);
    float v0 = std::clamp(m_TextureBounds[sourceEyeIndex].vMin, 0.0f, 1.0f);
    float u1 = std::clamp(m_TextureBounds[sourceEyeIndex].uMax, 0.0f, 1.0f);
    float v1 = std::clamp(m_TextureBounds[sourceEyeIndex].vMax, 0.0f, 1.0f);
    if (u1 <= u0 || v1 <= v0)
    {
        u0 = 0.0f;
        v0 = 0.0f;
        u1 = 1.0f;
        v1 = 1.0f;
    }

    struct AutoFlashlightReadbackVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };
    static constexpr DWORD kAutoFlashlightReadbackFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    const float left = -0.5f;
    const float top = -0.5f;
    const float right = static_cast<float>(kAutoFlashlightReadbackWidth) - 0.5f;
    const float bottom = static_cast<float>(kAutoFlashlightReadbackHeight) - 0.5f;
    const AutoFlashlightReadbackVertex quad[4] =
    {
        { left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, u0, v0 },
        { right, top,    0.0f, 1.0f, 0xFFFFFFFFu, u1, v0 },
        { left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, u0, v1 },
        { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, u1, v1 }
    };

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = kAutoFlashlightReadbackWidth;
    viewport.Height = kAutoFlashlightReadbackHeight;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    bool ok = true;
    hr = device->SetRenderTarget(0, m_D9AutoFlashlightLumaSurface);
    ok = ok && SUCCEEDED(hr);
    if (ok)
    {
        device->SetViewport(&viewport);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kAutoFlashlightReadbackFvf);
        device->SetTexture(0, srcTexture);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(AutoFlashlightReadbackVertex));
        ok = ok && SUCCEEDED(hr);
    }

    if (ok)
    {
        hr = device->GetRenderTargetData(m_D9AutoFlashlightLumaSurface, m_D9AutoFlashlightReadbackSurface);
        ok = SUCCEEDED(hr);
    }

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }
    else
    {
        device->SetTexture(0, nullptr);
    }
    if (SUCCEEDED(oldRtHr) && oldRenderTarget)
        device->SetRenderTarget(0, oldRenderTarget);
    if (haveOldViewport)
        device->SetViewport(&oldViewport);
    if (oldRenderTarget)
        oldRenderTarget->Release();

    srcTexture->Release();

    if (!ok)
    {
        unlockDevice();
        releaseDevice();
        releaseSrc();
        return false;
    }

    D3DLOCKED_RECT lockedRect{};
    hr = m_D9AutoFlashlightReadbackSurface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr))
    {
        unlockDevice();
        releaseDevice();
        releaseSrc();
        return false;
    }

    std::vector<float> centerLumas;
    std::vector<float> peripheralLumas;
    centerLumas.reserve(kAutoFlashlightReadbackWidth * kAutoFlashlightReadbackHeight);
    peripheralLumas.reserve(kAutoFlashlightReadbackWidth * kAutoFlashlightReadbackHeight);
    float totalLuma = 0.0f;
    int totalPixels = 0;

    const int centerX0 = kAutoFlashlightReadbackWidth / 4;
    const int centerX1 = (kAutoFlashlightReadbackWidth * 3) / 4;
    const int centerY0 = kAutoFlashlightReadbackHeight / 4;
    const int centerY1 = (kAutoFlashlightReadbackHeight * 13) / 18;
    const int sideX0 = (kAutoFlashlightReadbackWidth * 7) / 32;
    const int sideX1 = (kAutoFlashlightReadbackWidth * 25) / 32;
    const int sideY0 = kAutoFlashlightReadbackHeight / 4;
    const int sideY1 = (kAutoFlashlightReadbackHeight * 13) / 18;
    const int topBandY0 = kAutoFlashlightReadbackHeight / 6;
    const int topBandY1 = kAutoFlashlightReadbackHeight / 3;

    const uint8_t* base = static_cast<const uint8_t*>(lockedRect.pBits);
    for (int y = 0; y < kAutoFlashlightReadbackHeight; ++y)
    {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(base + y * lockedRect.Pitch);
        for (int x = 0; x < kAutoFlashlightReadbackWidth; ++x)
        {
            const float luma = ComputePerceivedLumaFromArgb(row[x]);
            totalLuma += luma;
            ++totalPixels;

            const bool center = (x >= centerX0 && x < centerX1 && y >= centerY0 && y < centerY1);
            const bool sidePeripheral = ((x < sideX0 || x >= sideX1) && y >= sideY0 && y < sideY1);
            const bool topPeripheral = (x >= centerX0 && x < centerX1 && y >= topBandY0 && y < topBandY1);
            if (center)
                centerLumas.push_back(luma);
            if (sidePeripheral || topPeripheral)
                peripheralLumas.push_back(luma);
        }
    }

    m_D9AutoFlashlightReadbackSurface->UnlockRect();
    unlockDevice();
    releaseDevice();
    releaseSrc();

    if (centerLumas.empty() || peripheralLumas.empty() || totalPixels <= 0)
        return false;

    outCenterMedianLuma = ComputePercentile(centerLumas, 0.50f);
    outCenterLowLuma = ComputePercentile(centerLumas, 0.30f);
    outPeripheralMedianLuma = ComputePercentile(peripheralLumas, 0.50f);
    outMeanLuma = totalLuma / static_cast<float>(totalPixels);
    return true;
}

void VR::UpdateAutoFlashlight(C_BasePlayer* localPlayer)
{
    const auto now = std::chrono::steady_clock::now();
    auto debugLog = [this](const char* format, auto... args)
        {
            if (m_AutoFlashlightDebugLog && !ShouldThrottle(m_AutoFlashlightLastDebugLog, m_AutoFlashlightDebugLogHz))
                Game::logMsg(format, args...);
        };

    const bool wantsScreenLuma = m_AutoFlashlightEnabled || m_VrHandsEnabled;
    if (!wantsScreenLuma)
    {
        ResetAutoFlashlightState();
        debugLog("[VR][AutoFlashlight] skip: disabled");
        return;
    }

    if (!m_Game)
    {
        ResetAutoFlashlightState();
        debugLog("[VR][AutoFlashlight] skip: missing game");
        return;
    }

    if (!m_Game->m_EngineClient)
    {
        ResetAutoFlashlightState();
        debugLog("[VR][AutoFlashlight] skip: missing engine client");
        return;
    }

    if (!m_Game->m_EngineClient->IsInGame())
    {
        ResetAutoFlashlightState();
        debugLog("[VR][AutoFlashlight] skip: not in game");
        return;
    }

    if (!localPlayer)
    {
        ResetAutoFlashlightState();
        debugLog("[VR][AutoFlashlight] skip: missing local player");
        return;
    }

    if (m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible())
    {
        debugLog("[VR][AutoFlashlight] skip: menu cursor visible");
        return;
    }

    __try
    {
        const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
        const int teamNum = *reinterpret_cast<const int*>(base + kTeamNumOffset);
        const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);
        const int obsMode = *reinterpret_cast<const int*>(base + kObserverModeOffset);
        if (teamNum == 1 || lifeState != 0 || obsMode != 0)
        {
            debugLog("[VR][AutoFlashlight] skip: player inactive team=%d life=%u observer=%d", teamNum, lifeState, obsMode);
            return;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        debugLog("[VR][AutoFlashlight] skip: player state read exception");
        return;
    }

    const bool manualOverrideActive =
        m_AutoFlashlightManualOverrideUntil.time_since_epoch().count() != 0 &&
        now < m_AutoFlashlightManualOverrideUntil;
    if (manualOverrideActive && !m_VrHandsEnabled)
    {
        debugLog("[VR][AutoFlashlight] skip: manual override remaining=%.2fs",
            std::chrono::duration<float>(m_AutoFlashlightManualOverrideUntil - now).count());
        return;
    }

    if (m_AutoFlashlightNextSampleTime.time_since_epoch().count() != 0 &&
        now < m_AutoFlashlightNextSampleTime)
    {
        return;
    }

    m_AutoFlashlightNextSampleTime =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(kAutoFlashlightSampleIntervalSeconds));

    float centerMedian = 255.0f;
    float centerLow = 255.0f;
    float peripheralMedian = 255.0f;
    float meanLuma = 255.0f;
    if (!SampleAutoFlashlightScreenLuma(centerMedian, centerLow, peripheralMedian, meanLuma))
    {
        debugLog("[VR][AutoFlashlight] skip: screen luma readback failed");
        return;
    }

    m_AutoFlashlightHasScreenLuma = true;
    m_AutoFlashlightCenterMedianLuma = centerMedian;
    m_AutoFlashlightCenterLowLuma = centerLow;
    m_AutoFlashlightPeripheralMedianLuma = peripheralMedian;

    if (!m_AutoFlashlightEnabled || manualOverrideActive)
    {
        m_AutoFlashlightDarkDecisionSamples = 0;
        m_AutoFlashlightBrightDecisionSamples = 0;
        return;
    }

    bool flashlightOn = false;
    const bool stateValid = QueryFlashlightState(localPlayer, flashlightOn);
    if (!stateValid)
    {
        if (!m_AutoFlashlightHasKnownState)
        {
            debugLog("[VR][AutoFlashlight] skip: flashlight state unavailable");
            return;
        }

        flashlightOn = m_AutoFlashlightLastKnownOn;
    }

    const float darkGateLuma = std::min(centerMedian, centerLow + 10.0f);
    const float brightGateLuma = peripheralMedian;
    const int darkSamplesRequired = (std::max)(1, static_cast<int>(std::ceil(kAutoFlashlightDarkHoldSeconds / kAutoFlashlightSampleIntervalSeconds)));
    const int brightSamplesRequired = (std::max)(1, static_cast<int>(std::ceil(kAutoFlashlightBrightHoldSeconds / kAutoFlashlightSampleIntervalSeconds)));
    const float elapsedSinceToggle =
        (m_AutoFlashlightLastToggleTime.time_since_epoch().count() == 0)
        ? 9999.0f
        : std::chrono::duration<float>(now - m_AutoFlashlightLastToggleTime).count();

    bool shouldToggle = false;
    const char* decision = "hold";

    if (!flashlightOn)
    {
        m_AutoFlashlightBrightDecisionSamples = 0;
        if (darkGateLuma <= m_AutoFlashlightDarkThreshold)
        {
            m_AutoFlashlightDarkDecisionSamples = (std::min)(m_AutoFlashlightDarkDecisionSamples + 1, 1024);
            if (elapsedSinceToggle >= kAutoFlashlightMinOffSeconds &&
                m_AutoFlashlightDarkDecisionSamples >= darkSamplesRequired)
            {
                shouldToggle = true;
                decision = "toggle_on";
            }
            else
            {
                decision = (elapsedSinceToggle < kAutoFlashlightMinOffSeconds) ? "dark_wait_min_off" : "dark_wait_streak";
            }
        }
        else
        {
            m_AutoFlashlightDarkDecisionSamples = 0;
            decision = "hold_not_dark";
        }
    }
    else
    {
        m_AutoFlashlightDarkDecisionSamples = 0;
        if (brightGateLuma >= m_AutoFlashlightBrightThreshold)
        {
            m_AutoFlashlightBrightDecisionSamples = (std::min)(m_AutoFlashlightBrightDecisionSamples + 1, 1024);
            if (elapsedSinceToggle >= kAutoFlashlightMinOnSeconds &&
                m_AutoFlashlightBrightDecisionSamples >= brightSamplesRequired)
            {
                shouldToggle = true;
                decision = "toggle_off";
            }
            else
            {
                decision = (elapsedSinceToggle < kAutoFlashlightMinOnSeconds) ? "bright_wait_min_on" : "bright_wait_streak";
            }
        }
        else
        {
            m_AutoFlashlightBrightDecisionSamples = 0;
            decision = "hold_not_bright";
        }
    }

    debugLog("[VR][AutoFlashlight] screen centerMed=%.1f centerLow=%.1f peripheralMed=%.1f mean=%.1f darkGate=%.1f brightGate=%.1f darkStreak=%d/%d brightStreak=%d/%d on=%d stateValid=%d dark<=%.1f bright>=%.1f elapsed=%.2fs decision=%s",
        centerMedian, centerLow, peripheralMedian, meanLuma,
        darkGateLuma, brightGateLuma,
        m_AutoFlashlightDarkDecisionSamples, darkSamplesRequired,
        m_AutoFlashlightBrightDecisionSamples, brightSamplesRequired,
        flashlightOn ? 1 : 0, stateValid ? 1 : 0,
        m_AutoFlashlightDarkThreshold, m_AutoFlashlightBrightThreshold,
        elapsedSinceToggle, decision);

    if (shouldToggle)
    {
        IssueFlashlightToggle(false);
        if (m_AutoFlashlightDebugLog)
        {
            Game::logMsg("[VR][AutoFlashlight] toggled %s centerMed=%.1f centerLow=%.1f peripheralMed=%.1f darkGate=%.1f brightGate=%.1f",
                flashlightOn ? "off" : "on",
                centerMedian, centerLow, peripheralMedian, darkGateLuma, brightGateLuma);
        }
    }
}

void VR::UpdateSystemMouseInputSuppression(bool inGame, bool hasLocalPlayer)
{
    const bool active =
        m_SystemMouseInputSuppressAfterMapLoad &&
        m_IsVREnabled &&
        inGame &&
        hasLocalPlayer;

    if (active)
        EnsureSystemMouseInputSuppressThreadStarted();

    g_SystemMouseInputSuppressActive.store(active ? 1 : 0, std::memory_order_release);

    if (m_SystemMouseInputSuppressActive != active)
    {
        m_SystemMouseInputSuppressActive = active;
        Game::logMsg("[VR][MouseSuppress] %s inGame=%d localPlayer=%d",
            active ? "active" : "inactive",
            inGame ? 1 : 0,
            hasLocalPlayer ? 1 : 0);
    }
}

extern "C" void __cdecl L4D2VR_ShutdownSystemMouseInputSuppression()
{
    g_SystemMouseInputSuppressActive.store(0, std::memory_order_release);
    g_SystemMouseInputSuppressStop.store(1, std::memory_order_release);

    HANDLE handle = nullptr;
    DWORD threadId = 0;
    {
        std::lock_guard<std::mutex> lock(g_SystemMouseInputSuppressThreadMutex);
        handle = g_SystemMouseInputSuppressThreadHandle;
        g_SystemMouseInputSuppressThreadHandle = nullptr;
        threadId = g_SystemMouseInputSuppressThreadId.load(std::memory_order_acquire);
    }

    if (threadId != 0)
        PostThreadMessageA(threadId, kSystemMouseInputSuppressQuitMessage, 0, 0);

    if (handle)
        CloseHandle(handle);
}

void VR::Update()
{
    if (!m_IsInitialized || !g_Game)
        return;

    if (!m_Game || m_Game != g_Game)
        m_Game = g_Game;

    if (!m_Game->m_Initialized)
        return;

    static bool s_WasInGameLastUpdate = false;
    const bool inGameAtUpdateStart = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    const bool returnedToMainMenu = s_WasInGameLastUpdate && !inGameAtUpdateStart;
    s_WasInGameLastUpdate = inGameAtUpdateStart;

    bool hasLocalPlayerAtUpdateStart = false;
    if (inGameAtUpdateStart && m_Game->m_EngineClient)
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        hasLocalPlayerAtUpdateStart = playerIndex > 0 && m_Game->GetClientEntity(playerIndex) != nullptr;
    }
    UpdateSystemMouseInputSuppression(inGameAtUpdateStart, hasLocalPlayerAtUpdateStart);

    if (m_VrRecommendedVideoSettingsEnabled &&
        (m_VrRecommendedVideoSettingsApplyPending || returnedToMainMenu))
    {
        ApplyVrRecommendedVideoSettingsIfNeeded(
            returnedToMainMenu ? "returned_to_main_menu" : "enabled");
    }

    if (m_IsVREnabled && g_D3DVR9)
    {
        // Prevents crashing at menu
        if (!inGameAtUpdateStart)
        {
            IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
            if (!rndrContext)
            {
                HandleMissingRenderContext("VR::Update");
                return;
            }

            if (returnedToMainMenu)
                ClearVRMenuTransitionResiduals(this, "Update returned to main menu");

            rndrContext->SetRenderTarget(NULL);
            m_Game->m_CachedArmsModel = false;
            m_CreatedVRTextures.store(false, std::memory_order_release); // Have to recreate textures otherwise some workshop maps won't render
        }
    }

    UpdateFocusShadowRecovery(this, inGameAtUpdateStart);

    const bool queuedAtFrameStart = (m_Game && (m_Game->GetMatQueueMode() != 0));

    const uint32_t updateThreadId = static_cast<uint32_t>(::GetCurrentThreadId());
    const uint32_t renderThreadIdAtUpdate = m_RenderThreadId.load(std::memory_order_acquire);
    const bool suppressQueuedReShadeSubmitOnRenderThread =
        queuedAtFrameStart &&
        m_ReShadeVRCompat &&
        renderThreadIdAtUpdate != 0 &&
        renderThreadIdAtUpdate == updateThreadId;

    UpdateDesktopMirrorOverlayHideEffective(this, queuedAtFrameStart);
    if (!queuedAtFrameStart)
        SubmitVRTextures();

    bool posesValid = UpdatePosesAndActions();
    UpdateAutoMatQueueMode();
    const int queueModeAfterAuto = m_Game ? m_Game->GetMatQueueMode() : 0;
    static int s_LastObservedQueueMode = -999;
    if (s_LastObservedQueueMode != queueModeAfterAuto)
    {
        const uint32_t completedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
        if (queueModeAfterAuto != 0 && m_CreatedVRTextures.load(std::memory_order_acquire))
        {
            std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
            if (!m_D9LeftEyeSubmitSurface || !m_D9RightEyeSubmitSurface)
            {
                // A manual q=0 -> queued switch can reuse textures created before the
                // submit-snapshot policy was active. Let the next RenderView recreate
                // the full target set before publishing another queued frame.
                m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
                m_CreatedVRTextures.store(false, std::memory_order_release);
            }
        }
        if (queueModeAfterAuto == 0)
        {
            m_LastSubmittedFrameId.store(completedFrameId, std::memory_order_release);
            m_RenderCompletedPoseToken.store(0, std::memory_order_release);
            m_RenderCompletedDuplicatePoseFrameId.store(0, std::memory_order_release);
            {
                std::lock_guard<std::mutex> poseLock(m_RenderCompletedHmdPoseMutex);
                m_RenderCompletedHmdPoseValid = false;
            }
            m_ReShadeVRCompatPendingRenderReady.store(0, std::memory_order_release);
            m_ReShadeVRCompatPendingRenderPoseToken.store(0, std::memory_order_release);
            m_ReShadeVRCompatPendingRenderFrameSeq.store(0, std::memory_order_release);
            m_ReShadeVRCompatPendingDuplicatePose.store(0, std::memory_order_release);
            m_RenderedNewFrame.store(false, std::memory_order_release);
            m_SubmitInFlight.store(false, std::memory_order_release);
            m_QueuedSubmitStaleStreak.store(0, std::memory_order_release);
        }

        if (m_RenderPipelineDebugLog)
        {
            Game::logMsg("[VR][QueueMode] changed %d->%d inGame=%d completed=%u submitted=%u pose=%u",
                s_LastObservedQueueMode,
                queueModeAfterAuto,
                inGameAtUpdateStart ? 1 : 0,
                completedFrameId,
                m_LastSubmittedFrameId.load(std::memory_order_acquire),
                m_SubmitPoseToken.load(std::memory_order_acquire));
        }
        s_LastObservedQueueMode = queueModeAfterAuto;
    }
    ApplyShadowSettingsIfNeeded();
    ApplyFlashlightEnhancementIfNeeded();
    ApplyLocalVScriptConvarsIfNeeded();
    if (!posesValid)
    {
        // Continue using the last known poses so smoothing and aim helpers stay active.
    }

    if (queuedAtFrameStart && !suppressQueuedReShadeSubmitOnRenderThread)
    {
        SubmitVRTextures();

        // Scope lens GPU post-process is delayed until after the eye submit in queued rendering.
        // This keeps the processed overlay texture from racing Source's material queue.
        if (m_QueuedScopeLensPostProcessPending.load(std::memory_order_acquire) != 0u)
        {
            float lensHz = GetHmdDisplayFrequencyHz();
            if (!std::isfinite(lensHz) || lensHz <= 1.0f)
                lensHz = 90.0f;
            lensHz = std::clamp(lensHz, 1.0f, 240.0f);

            if (m_QueuedScopeLensPostProcessPending.exchange(0u, std::memory_order_acq_rel) != 0u)
            {
                const bool scopeLensOk = ApplyScopeLensPostProcess();
                if (m_RenderPipelineDebugLog)
                {
                    static std::chrono::steady_clock::time_point s_lastScopeLensAfterSubmitLog{};
                    if (!ShouldThrottle(s_lastScopeLensAfterSubmitLog, m_RenderPipelineDebugLogHz))
                    {
                        Game::logMsg("[VR][ScopeLens][QueuedAfterSubmit] tid=%lu q=%d hz=%.1f ok=%d",
                            GetCurrentThreadId(),
                            m_Game ? m_Game->GetMatQueueMode() : -1,
                            lensHz,
                            scopeLensOk ? 1 : 0);
                    }
                }
            }
        }
    }
    else if (suppressQueuedReShadeSubmitOnRenderThread && m_RenderPipelineDebugLog)
    {
        static thread_local std::chrono::steady_clock::time_point s_lastQueuedReShadeUpdateSubmitSkipLog{};
        if (!ShouldThrottle(s_lastQueuedReShadeUpdateSubmitSkipLog, m_RenderPipelineDebugLogHz))
        {
            Game::logMsg("[VR][Queued][ReShadeUpdateSubmitSkip] tid=%lu renderTid=%u completed=%u submitted=%u renderedNew=%d",
                ::GetCurrentThreadId(),
                renderThreadIdAtUpdate,
                m_RenderCompletedFrameId.load(std::memory_order_acquire),
                m_LastSubmittedFrameId.load(std::memory_order_acquire),
                m_RenderedNewFrame.load(std::memory_order_acquire) ? 1 : 0);
        }
    }

    // Auto ResetPosition shortly after a level finishes loading.

    {
        const bool inGame = m_Game->m_EngineClient->IsInGame();
        auto setNativeLeftHandFreezePlaneContextActive = [&](bool active)
            {
                if (m_NativeViewmodelHandsOnlyFreezePlaneContextActive == active)
                    return;

                m_NativeViewmodelHandsOnlyFreezePlaneContextActive = active;
                m_NativeViewmodelHandsOnlyFreezePlaneGeneration.fetch_add(1u, std::memory_order_acq_rel);
            };
        auto resetNativeLeftHandFreezeForMapChange = [&]()
            {
                const bool hadState =
                    m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev ||
                    m_NativeViewmodelLeftHandFreezePending ||
                    m_NativeViewmodelLeftHandFreezeReady.load(std::memory_order_acquire) != 0u ||
                    m_NativeViewmodelLeftHandFreezeHasSurvivorCharacter;
                m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev = false;
                m_NativeViewmodelLeftHandFreezePending = false;
                m_NativeViewmodelLeftHandFreezeDueTime = {};
                m_NativeViewmodelLeftHandFreezeReady.store(0u, std::memory_order_release);
                m_NativeViewmodelLeftHandFreezeHasSurvivorCharacter = false;
                m_NativeViewmodelLeftHandFreezeSurvivorCharacter = -1;
                if (hadState)
                    m_NativeViewmodelLeftHandFreezeGeneration.fetch_add(1u, std::memory_order_acq_rel);
            };
        auto invalidateNativeLeftHandFreezeForCharacterChange = [&](int oldCharacter, int newCharacter)
            {
                const bool hadState =
                    m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev ||
                    m_NativeViewmodelLeftHandFreezePending ||
                    m_NativeViewmodelLeftHandFreezeReady.load(std::memory_order_acquire) != 0u;
                m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev = false;
                m_NativeViewmodelLeftHandFreezePending = false;
                m_NativeViewmodelLeftHandFreezeDueTime = {};
                m_NativeViewmodelLeftHandFreezeReady.store(0u, std::memory_order_release);
                if (hadState)
                    m_NativeViewmodelLeftHandFreezeGeneration.fetch_add(1u, std::memory_order_acq_rel);
                m_NativeViewmodelHandsOnlyFreezePlaneGeneration.fetch_add(1u, std::memory_order_acq_rel);
                if (m_VrHandsDebugLog)
                {
                    Game::logMsg(
                        "[VR][NativeHandsOnly] left-hand animation freeze invalidated survivorCharacter %d -> %d",
                        oldCharacter,
                        newCharacter);
                }
            };
        if (!inGame)
        {
            m_AutoResetPositionPending = false;
            m_AutoResetHadLocalPlayerPrev = false;
            m_LocalVScriptConvarsMapAuditPending = false;
            m_LocalVScriptConvarsHadLocalPlayerPrev = false;
            setNativeLeftHandFreezePlaneContextActive(false);
            resetNativeLeftHandFreezeForMapChange();
        }
        else
        {
            int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
            C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
            const bool hasLocalPlayer = (localPlayer != nullptr);
            const auto now = std::chrono::steady_clock::now();
            bool hasLiveLocalPlayerWithWeapon = false;
            bool hasLocalSurvivorCharacter = false;
            int localSurvivorCharacter = -1;
            if (hasLocalPlayer)
            {
                const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
                const int teamNum = *reinterpret_cast<const int*>(base + kTeamNumOffset);
                const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);
                const int obsMode = *reinterpret_cast<const int*>(base + kObserverModeOffset);
                C_WeaponCSBase* activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
                if (teamNum == 2)
                {
                    localSurvivorCharacter = *reinterpret_cast<const int*>(base + kSurvivorCharacterOffset);
                    hasLocalSurvivorCharacter =
                        localSurvivorCharacter >= 0 &&
                        localSurvivorCharacter <= 16;
                }
                hasLiveLocalPlayerWithWeapon =
                    teamNum != 1 &&
                    lifeState == 0 &&
                    obsMode == 0 &&
                    activeWeapon &&
                    activeWeapon->GetWeaponID() != C_WeaponCSBase::WeaponID::NONE;
            }

            const bool twoHandedGripPoseActive = IsVrHandsTwoHandedGripPoseActive();
            const bool nativeLeftHandFreezeEnabled =
                m_NativeViewmodelHandsOnly && !twoHandedGripPoseActive;
            const bool nativeHandsOnlyFreezePlaneContextEnabled =
                m_NativeViewmodelHandsOnly;
            if (hasLocalSurvivorCharacter)
            {
                if (!m_NativeViewmodelLeftHandFreezeHasSurvivorCharacter)
                {
                    m_NativeViewmodelLeftHandFreezeHasSurvivorCharacter = true;
                    m_NativeViewmodelLeftHandFreezeSurvivorCharacter = localSurvivorCharacter;
                }
                else if (m_NativeViewmodelLeftHandFreezeSurvivorCharacter != localSurvivorCharacter)
                {
                    const int oldCharacter = m_NativeViewmodelLeftHandFreezeSurvivorCharacter;
                    invalidateNativeLeftHandFreezeForCharacterChange(oldCharacter, localSurvivorCharacter);
                    m_NativeViewmodelLeftHandFreezeHasSurvivorCharacter = true;
                    m_NativeViewmodelLeftHandFreezeSurvivorCharacter = localSurvivorCharacter;
                }
            }

            setNativeLeftHandFreezePlaneContextActive(nativeHandsOnlyFreezePlaneContextEnabled && hasLiveLocalPlayerWithWeapon);
            if (!m_NativeViewmodelHandsOnly)
            {
                resetNativeLeftHandFreezeForMapChange();
            }
            else if (twoHandedGripPoseActive)
            {
                m_NativeViewmodelLeftHandFreezePending = false;
                m_NativeViewmodelLeftHandFreezeDueTime = {};
                if (hasLiveLocalPlayerWithWeapon)
                {
                    m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev = true;
                    m_NativeViewmodelLeftHandFreezeReady.store(1u, std::memory_order_release);
                }
            }
            if (nativeLeftHandFreezeEnabled && hasLiveLocalPlayerWithWeapon)
            {
                if (!m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev)
                {
                    const int freezeDelayMs =
                        (int)(std::max)(0.0f, m_NativeViewmodelLeftHandFreezeAfterMapSeconds * 1000.0f);
                    m_NativeViewmodelLeftHandFreezeReady.store(0u, std::memory_order_release);

                    if (freezeDelayMs <= 0)
                    {
                        m_NativeViewmodelLeftHandFreezePending = false;
                        m_NativeViewmodelLeftHandFreezeDueTime = {};
                        m_NativeViewmodelLeftHandFreezeReady.store(1u, std::memory_order_release);
                        if (m_VrHandsDebugLog)
                            Game::logMsg("[VR][NativeHandsOnly] left-hand animation freeze ready immediately");
                    }
                    else
                    {
                        m_NativeViewmodelLeftHandFreezePending = true;
                        m_NativeViewmodelLeftHandFreezeDueTime = now + std::chrono::milliseconds(freezeDelayMs);
                        if (m_VrHandsDebugLog)
                        {
                            Game::logMsg(
                                "[VR][NativeHandsOnly] left-hand animation freeze armed delay=%.2fs",
                                m_NativeViewmodelLeftHandFreezeAfterMapSeconds);
                        }
                    }
                }

                m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev = true;
                if (m_NativeViewmodelLeftHandFreezePending &&
                    now >= m_NativeViewmodelLeftHandFreezeDueTime)
                {
                    m_NativeViewmodelLeftHandFreezePending = false;
                    m_NativeViewmodelLeftHandFreezeReady.store(1u, std::memory_order_release);
                    if (m_VrHandsDebugLog)
                        Game::logMsg("[VR][NativeHandsOnly] left-hand animation freeze ready");
                }
            }

            if (hasLocalPlayer && !m_AutoResetHadLocalPlayerPrev && m_AutoResetPositionAfterLoadSeconds > 0.0f)
            {
                m_AutoResetPositionPending = true;
                const int ms = (int)(std::max)(0.0f, m_AutoResetPositionAfterLoadSeconds * 1000.0f);
                m_AutoResetPositionDueTime = now + std::chrono::milliseconds(ms);
            }

            m_AutoResetHadLocalPlayerPrev = hasLocalPlayer;

            if (!m_LocalVScriptConvarsEnabled || !m_LocalVScriptConvarsLogEnabled)
            {
                m_LocalVScriptConvarsMapAuditPending = false;
            }
            else if (hasLocalPlayer && !m_LocalVScriptConvarsHadLocalPlayerPrev)
            {
                m_LocalVScriptConvarsMapAuditPending = true;
                const int auditDelayMs =
                    (int)(std::max)(0.0f, m_LocalVScriptConvarsMapAuditDelaySeconds * 1000.0f);
                m_LocalVScriptConvarsMapAuditDueTime = now + std::chrono::milliseconds(auditDelayMs);
            }

            m_LocalVScriptConvarsHadLocalPlayerPrev = hasLocalPlayer;

            if (m_AutoResetPositionPending && hasLocalPlayer)
            {
                if (now >= m_AutoResetPositionDueTime)
                {
                    ResetPosition();
                    m_AutoResetPositionPending = false;
                }
            }

            if (m_LocalVScriptConvarsMapAuditPending && hasLocalPlayer && now >= m_LocalVScriptConvarsMapAuditDueTime)
            {
                AuditLocalVScriptConvarsCurrentValues("map_enter");
                m_LocalVScriptConvarsMapAuditPending = false;
            }
        }
    }

    {
        C_BasePlayer* localPlayer = nullptr;
        if (m_Game->m_EngineClient->IsInGame())
        {
            const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
            localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
        }

        {
            static bool s_itemLabelHadStableLocalPlayer = false;
            static std::chrono::steady_clock::time_point s_itemLabelScanReadyAt{};
            const auto now = std::chrono::steady_clock::now();

            if (!m_ItemModelLabelEnabled || !localPlayer)
            {
                s_itemLabelHadStableLocalPlayer = false;
                s_itemLabelScanReadyAt = {};
                if (!m_ItemModelLabelEnabled)
                    m_ProjectedItemLabels.clear();
            }
            else
            {
                if (!s_itemLabelHadStableLocalPlayer)
                {
                    s_itemLabelHadStableLocalPlayer = true;
                    s_itemLabelScanReadyAt = now + std::chrono::seconds(3);
                }

                if (s_itemLabelScanReadyAt.time_since_epoch().count() != 0 && now >= s_itemLabelScanReadyAt)
                {
                    if (!SafeScanItemModelLabelEntities(this))
                    {
                        m_ProjectedItemLabels.clear();
                        s_itemLabelHadStableLocalPlayer = false;
                        s_itemLabelScanReadyAt = now + std::chrono::seconds(3);
                    }
                }
            }
        }

        UpdateAutoFlashlight(localPlayer);
    }

    UpdateTracking();
    UpdateNativeViewmodelLeftHandOpenVRFingerCurls();
    UpdateKillSoundFeedback();
    UpdateMeleeHitHaptics();
    PumpSpeechToTextCapture();
    PumpSpeechToTextResults();
    if (!m_TextToSpeechEnabled && !m_SpeechToTextSendVoiceEnabled)
        ShutdownTextToSpeechServer();
    UpdateKillIndicatorOverlays();
    UpdateDamageFeedback();


    if (!m_Game->m_VguiSurface)
    {
        PumpSpeechToTextVoiceBroadcast();
        FlushHapticMixer();
        return;
    }

    const bool menuInputActive =
        (m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused()) ||
        m_Game->m_VguiSurface->IsCursorVisible();

    if (menuInputActive)
    {
        CancelTeleportTargeting();
        ProcessMenuInput();
    }
    else
        ProcessInput();

    PumpSpeechToTextVoiceBroadcast();
    FlushHapticMixer();
}

bool VR::GetWalkAxis(float& x, float& y) {
    vr::InputAnalogActionData_t d;
    bool hasAxis = false;
    if (GetAnalogActionData(m_ActionWalk, d)) {
        x = d.x;
        y = d.y;
        hasAxis = true;
    }
    else
    {
        x = y = 0.0f;
    }
    return hasAxis;
}

void VR::LogVAS(const char* tag)
{
    if (!m_DebugVASLog || !tag || !*tag)
        return;

    const VASStats st = QueryVASStats();
    Game::logMsg(
        "[VR][VAS] %s | free %.1f MiB (largest %.1f MiB) | reserved %.1f MiB | committed %.1f MiB",
        tag,
        BytesToMiB(st.freeTotal),
        BytesToMiB(st.freeLargest),
        BytesToMiB(st.reserved),
        BytesToMiB(st.committed));
}

void VR::ReleaseVRRenderTargetsForDeviceReset()
{
    // D3D9 Reset requires all D3DPOOL_DEFAULT resources owned by the app to be released.
    // The VR render targets are created through Source's material system, but DXVK still
    // counts their backing D3D resources as reset-blocking default-pool resources.
    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);

    auto SafeReleaseD3D = [](auto*& ptr)
        {
            if (!ptr)
                return;
            ptr->Release();
            ptr = nullptr;
        };

    auto SafeReleaseSourceTexture = [](ITexture*& ptr)
        {
            if (!ptr)
                return;
            ptr->Release();
            ptr = nullptr;
        };

    ReleaseVrHandsD3DResources();

    m_CreatedVRTextures.store(false, std::memory_order_release);
    m_MenuBlankSubmitted = false;
    m_BackBufferTextureValid = false;
    m_CreatingTextureID = Texture_None;

    InvalidateSourceRenderQueueMarkers();
    m_RenderCompletedFrameId.store(0, std::memory_order_release);
    m_RenderCompletedPoseToken.store(0, std::memory_order_release);
    m_RenderCompletedDuplicatePoseFrameId.store(0, std::memory_order_release);
    m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
    m_ReShadeVRCompatResolvedFrameId.store(0, std::memory_order_release);
    m_PostPresentSubmitOverlayFrameId.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingRenderReady.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingRenderPoseToken.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingRenderFrameSeq.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingDuplicatePose.store(0, std::memory_order_release);
    m_LastSubmittedFrameId.store(0, std::memory_order_release);
    m_SubmitPoseToken.store(0, std::memory_order_release);
    m_LastSubmittedPoseToken.store(0, std::memory_order_release);
    m_SubmitInFlight.store(false, std::memory_order_release);
    m_LastSubmittedCompositorFrameIndex.store(0, std::memory_order_release);
    m_QueuedSubmitStaleStreak.store(0, std::memory_order_release);
    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_RenderedHud.store(false, std::memory_order_release);
    m_HudPaintedThisFrame.store(false, std::memory_order_release);
    ClearQueuedHudFresh();

    for (int eyeIndex = 0; eyeIndex < static_cast<int>(m_D3DAimLineOverlayBackupSurfaces.size()); ++eyeIndex)
    {
        m_D3DAimLineOverlayBackupValid[eyeIndex] = false;
        SafeReleaseD3D(m_D3DAimLineOverlayBackupSurfaces[eyeIndex]);
    }

    SafeReleaseD3D(m_D9AutoFlashlightReadbackSurface);
    SafeReleaseD3D(m_D9AutoFlashlightLumaSurface);
    SafeReleaseD3D(m_D9AutoFlashlightLumaTexture);
    m_AutoFlashlightReadbackWidth = 0;
    m_AutoFlashlightReadbackHeight = 0;

    DestroyHandHudWorldQuadTextures();
    for (size_t i = 0; i < m_D9SpecialInfectedIntentSenseHudDynTex.size(); ++i)
    {
        SafeReleaseD3D(m_D9SpecialInfectedIntentSenseHudDynSurface[i]);
        SafeReleaseD3D(m_D9SpecialInfectedIntentSenseHudDynTex[i]);
        m_D9SpecialInfectedIntentSenseHudDynW[i] = 0;
        m_D9SpecialInfectedIntentSenseHudDynH[i] = 0;
        std::memset(&m_VKSpecialInfectedIntentSenseHudDyn[i], 0, sizeof(m_VKSpecialInfectedIntentSenseHudDyn[i]));
    }
    m_SpecialInfectedIntentSenseHudDynFront = 0;
    DestroyKillIndicatorOverlayTextures();
    DestroyItemLabelOverlayTexture();

    SafeReleaseD3D(m_D9LeftEyeSurface);
    SafeReleaseD3D(m_D9RightEyeSurface);
    SafeReleaseD3D(m_D9LeftEyeSubmitSurface);
    SafeReleaseD3D(m_D9RightEyeSubmitSurface);
    SafeReleaseD3D(m_D9HUDSurface);
    SafeReleaseD3D(m_D9ScopeLensSurface);
    SafeReleaseD3D(m_D9ScopeLensTexture);
    SafeReleaseD3D(m_D9ScopeLensScratchSurface);
    SafeReleaseD3D(m_D9ScopeLensScratchTexture);
    m_D9ScopeLensScratchW = 0;
    m_D9ScopeLensScratchH = 0;
    m_D9ScopeLensScratchFormat = 0;
    m_QueuedScopeLensPostProcessPending.store(0, std::memory_order_release);
    m_ScopeLensOverlayReady.store(0, std::memory_order_release);
    SafeReleaseD3D(m_D9ScopeSurface);
    SafeReleaseD3D(m_D9RearMirrorSurface);
    SafeReleaseD3D(m_D9DesktopCompanionRearMirrorReadback);
    SafeReleaseD3D(m_D9DesktopMirrorSurface);
    SafeReleaseD3D(m_D9BlankSurface);

    SafeReleaseSourceTexture(m_LeftEyeTexture);
    SafeReleaseSourceTexture(m_RightEyeTexture);
    SafeReleaseSourceTexture(m_LeftEyeSubmitTexture);
    SafeReleaseSourceTexture(m_RightEyeSubmitTexture);
    SafeReleaseSourceTexture(m_HUDTexture);
    SafeReleaseSourceTexture(m_ScopeTexture);
    SafeReleaseSourceTexture(m_RearMirrorTexture);
    SafeReleaseSourceTexture(m_DesktopMirrorTexture);
    SafeReleaseSourceTexture(m_BlankTexture);

    std::memset(&m_VKLeftEye, 0, sizeof(m_VKLeftEye));
    std::memset(&m_VKRightEye, 0, sizeof(m_VKRightEye));
    std::memset(&m_VKHUD, 0, sizeof(m_VKHUD));
    std::memset(&m_VKScope, 0, sizeof(m_VKScope));
    std::memset(&m_VKScopeLens, 0, sizeof(m_VKScopeLens));
    std::memset(&m_VKRearMirror, 0, sizeof(m_VKRearMirror));
    std::memset(&m_VKBlankTexture, 0, sizeof(m_VKBlankTexture));
    std::memset(&m_VKBackBuffer, 0, sizeof(m_VKBackBuffer));
}

bool VR::RefreshBackBufferTexture(bool forceRefresh)
{
    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);

    if (!g_D3DVR9)
        return false;

    if (!forceRefresh && m_BackBufferTextureValid && m_VKBackBuffer.m_VRTexture.handle)
        return true;

    SharedTextureHolder refreshed{};
    const HRESULT hr = g_D3DVR9->GetBackBufferData(&refreshed);
    if (FAILED(hr) || !refreshed.m_VRTexture.handle)
    {
        m_BackBufferTextureValid = false;
        std::memset(&m_VKBackBuffer, 0, sizeof(m_VKBackBuffer));
        Game::logMsg("[VR][D3DReset] failed to refresh backbuffer VR texture hr=0x%08X", static_cast<unsigned int>(hr));
        return false;
    }

    m_VKBackBuffer = refreshed;
    // GetBackBufferData fills the handle with the address of the target holder.
    // Copying through a temporary would otherwise leave a dangling handle.
    m_VKBackBuffer.m_VRTexture.handle = &m_VKBackBuffer.m_VulkanData;
    m_BackBufferTextureValid = true;
    return true;
}

void VR::CreateVRTextures()
{
    // CreateNamedRenderTargetTextureEx re-enters DXVK and populates m_VK* via m_CreatingTextureID.
    // Serialize the whole sequence so submit/update threads never observe partially rewritten Vulkan descriptors.
    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
    if (m_CreatedVRTextures.load(std::memory_order_acquire))
        return;

    // If a menu/video reset invalidated the textures, release the previous backing resources
    // before creating replacements with the same render-target names.
    ReleaseVRRenderTargetsForDeviceReset();

    // ReleaseVRRenderTargetsForDeviceReset clears the flag while holding the same recursive
    // texture mutex; keep creating the fresh textures in this call.

    LogVAS("before CreateVRTextures");

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    int backBufferWidth = 0;
    int backBufferHeight = 0;
    m_Game->m_MaterialSystem->GetBackBufferDimensions(backBufferWidth, backBufferHeight);

    m_Game->m_MaterialSystem->isGameRunning = false;
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();
    m_Game->m_MaterialSystem->isGameRunning = true;
    const ImageFormat backBufferFormat = m_Game->m_MaterialSystem->GetBackBufferFormat();
    const ImageFormat eyeFormat = backBufferFormat;
    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, eyeFormat, MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);
    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, eyeFormat, MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);

    // Queued rendering needs a stable consumer pair: Source writes leftEye0/rightEye0,
    // then its completion functor snapshots both into these submit RTs. OpenVR never
    // reads the raw eye pair while Source is already building the next frame.
    // ReShade/MSAA use the same pair for crop pre-bake / resolve respectively.
    const bool queuedEyeIsolationRequested =
        m_AutoMatQueueMode ||
        (m_Game && m_Game->GetMatQueueMode() != 0);
    const bool useDedicatedEyeSubmitTextures =
        queuedEyeIsolationRequested ||
        m_ReShadeVRCompat ||
        m_AntiAliasing == 2 || m_AntiAliasing == 4 || m_AntiAliasing == 8 || m_AntiAliasing == 16;
    if (useDedicatedEyeSubmitTextures)
    {
        m_CreatingTextureID = Texture_LeftEyeSubmit;
        m_LeftEyeSubmitTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEyeSubmit0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, eyeFormat, MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_NOMIP);
        m_CreatingTextureID = Texture_RightEyeSubmit;
        m_RightEyeSubmitTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEyeSubmit0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, eyeFormat, MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_NOMIP);
    }
    else
    {
        m_LeftEyeSubmitTexture = nullptr;
        m_RightEyeSubmitTexture = nullptr;
        m_D9LeftEyeSubmitSurface = nullptr;
        m_D9RightEyeSubmitSurface = nullptr;
    }

    const bool createDesktopMirrorCleanTarget =
        m_DesktopMirrorEnabled &&
        m_DesktopMirrorHidePluginOverlaysRequested &&
        (!m_Game || m_Game->GetMatQueueMode() == 0);
    m_DesktopMirrorHidePluginOverlays = false;
    if (createDesktopMirrorCleanTarget)
    {
        // Full-size clean eye RTT for desktop mirroring. This is intentionally not
        // submitted to SteamVR. It is used only by the single-threaded cheap post-eye
        // D3D copy path. Queued/multicore rendering mirrors the regular eye directly;
        // it must not add an extra clean world RenderView.
        m_CreatingTextureID = Texture_DesktopMirror;
        m_DesktopMirrorTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "desktopMirrorClean0",
            static_cast<int>(m_RenderWidth),
            static_cast<int>(m_RenderHeight),
            RT_SIZE_NO_CHANGE,
            eyeFormat,
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
        m_DesktopMirrorHidePluginOverlays = (m_DesktopMirrorTexture != nullptr);
    }

    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, backBufferFormat, MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    if (m_RenderPipelineDebugLog)
    {
        int hudMapW = 0;
        int hudMapH = 0;
        int hudActualW = 0;
        int hudActualH = 0;
        if (m_HUDTexture)
        {
            hudMapW = m_HUDTexture->GetMappingWidth();
            hudMapH = m_HUDTexture->GetMappingHeight();
            hudActualW = m_HUDTexture->GetActualWidth();
            hudActualH = m_HUDTexture->GetActualHeight();
        }

        Game::logMsg("[VR][DesktopHUD][CreateRT] tid=%lu win=%dx%d bb=%dx%d eye=%ux%u hudTex=%s map=%dx%d actual=%dx%d format=%d",
            GetCurrentThreadId(), windowWidth, windowHeight, backBufferWidth, backBufferHeight,
            m_RenderWidth, m_RenderHeight,
            m_HUDTexture ? m_HUDTexture->GetName() : "<null>",
            hudMapW, hudMapH, hudActualW, hudActualH, static_cast<int>(backBufferFormat));
    }

    // Optional RTTs: scope + rear-mirror can be extremely expensive in 32-bit VAS
    // when their sizes are set high. Only pre-create them if requested.
    const bool wantScope = (!m_LazyScopeRearMirrorRTT) || m_ScopeEnabled;
    const bool wantRearMirror = (!m_LazyScopeRearMirrorRTT) || m_RearMirrorEnabled || m_DesktopRearMirrorWindowEnabled;

    if (wantScope)
    {
        // Square RTT for gun-mounted scope lens
        m_CreatingTextureID = Texture_Scope;
        m_ScopeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "vrScope",
            static_cast<int>(m_ScopeRTTSize),
            static_cast<int>(m_ScopeRTTSize),
            RT_SIZE_NO_CHANGE,
            IMAGE_FORMAT_BGRA8888,
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
    }

    if (wantRearMirror)
    {
        // Square RTT for off-hand rear mirror
        m_CreatingTextureID = Texture_RearMirror;
        m_RearMirrorTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "vrRearMirror",
            static_cast<int>(m_RearMirrorRTTSize),
            static_cast<int>(m_RearMirrorRTTSize),
            RT_SIZE_NO_CHANGE,
            m_Game->m_MaterialSystem->GetBackBufferFormat(),
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
    }

    m_CreatingTextureID = Texture_Blank;
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", 512, 512, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    // CreateNamedRenderTargetTextureEx can reuse video memory containing the last map's
    // HUD/eye pixels. Clear all VR RTs before they are ever submitted or exposed as overlays.
    ClearVRMenuTransitionResiduals(this, "CreateVRTextures");

    // New textures should not inherit old render/submit bookkeeping.
    InvalidateSourceRenderQueueMarkers();
    m_RenderCompletedFrameId.store(0, std::memory_order_release);
    m_RenderCompletedPoseToken.store(0, std::memory_order_release);
    m_RenderCompletedDuplicatePoseFrameId.store(0, std::memory_order_release);
    m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
    m_ReShadeVRCompatResolvedFrameId.store(0, std::memory_order_release);
    m_PostPresentSubmitOverlayFrameId.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingRenderReady.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingRenderPoseToken.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingRenderFrameSeq.store(0, std::memory_order_release);
    m_ReShadeVRCompatPendingDuplicatePose.store(0, std::memory_order_release);
    m_LastSubmittedFrameId.store(0, std::memory_order_release);
    m_SubmitPoseToken.store(0, std::memory_order_release);
    m_LastSubmittedPoseToken.store(0, std::memory_order_release);
    m_SubmitInFlight.store(false, std::memory_order_release);
    m_LastSubmittedCompositorFrameIndex.store(0, std::memory_order_release);
    m_QueuedSubmitStaleStreak.store(0, std::memory_order_release);

    m_CreatedVRTextures.store(true, std::memory_order_release);

    LogVAS("after CreateVRTextures");
}

void VR::EnsureOpticsRTTTextures()
{
    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return;

    const bool needScope = (m_ScopeEnabled && !m_ScopeTexture);
    const bool needRearMirror = ((m_RearMirrorEnabled || m_DesktopRearMirrorWindowEnabled) && !m_RearMirrorTexture);
    if (!needScope && !needRearMirror)
        return;

    LogVAS("before EnsureOpticsRTTTextures");

    m_Game->m_MaterialSystem->isGameRunning = false;
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();
    m_Game->m_MaterialSystem->isGameRunning = true;

    if (needScope)
    {
        m_CreatingTextureID = Texture_Scope;
        m_ScopeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "vrScope",
            static_cast<int>(m_ScopeRTTSize),
            static_cast<int>(m_ScopeRTTSize),
            RT_SIZE_NO_CHANGE,
            IMAGE_FORMAT_BGRA8888,
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
    }

    if (needRearMirror)
    {
        m_CreatingTextureID = Texture_RearMirror;
        m_RearMirrorTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "vrRearMirror",
            static_cast<int>(m_RearMirrorRTTSize),
            static_cast<int>(m_RearMirrorRTTSize),
            RT_SIZE_NO_CHANGE,
            m_Game->m_MaterialSystem->GetBackBufferFormat(),
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
    }

    m_CreatingTextureID = Texture_None;
    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    LogVAS("after EnsureOpticsRTTTextures");
}

void VR::InvalidateSourceRenderQueueMarkers()
{
    m_SourceRenderQueueMarkerEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_SourceRenderQueueMarkerQueuedId.store(0, std::memory_order_release);
    m_SourceRenderQueueMarkerCompletedId.store(0, std::memory_order_release);
    m_SourceRenderQueueAuxPendingCount.store(0, std::memory_order_release);
    m_SourceRenderQueueOwnershipUncertain.store(false, std::memory_order_release);
    m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
    m_PostPresentSubmitOverlayFrameId.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> poseLock(m_RenderCompletedHmdPoseMutex);
        m_RenderCompletedHmdPoseValid = false;
    }
}

bool VR::QueueVrHandsDrawForEye(
    IMatRenderContext* renderContext,
    const CViewSetup& view,
    int eyeIndex,
    VrHandDrawPass drawPass)
{
    const bool drawGloves = m_VrHandsEnabled && m_Input;
    const bool calibrationOverlayActive =
        m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
    const bool drawMagazineDebugBoxes =
        (m_MagazineBoxDebugEnabled || calibrationOverlayActive) &&
        drawPass != VrHandDrawPass::WorldVisibilityMask &&
        HasFreshMagazineInteractionDebugBoxWork();
    if (!renderContext ||
        (!drawGloves && !drawMagazineDebugBoxes) ||
        !m_IsVREnabled ||
        !m_Game)
    {
        return false;
    }

    if (m_Game->GetMatQueueMode() == 0)
        return false;

    void* contextVtable = nullptr;
    void* getCallQueueFn = nullptr;
    ICallQueue* callQueue = GetSourceRenderContextCallQueue(renderContext, &contextVtable, &getCallQueueFn);
    if (!callQueue)
    {
        if (m_RenderPipelineDebugLog)
        {
            static std::chrono::steady_clock::time_point s_lastMissingVrHandsQueueLog{};
            if (!ShouldThrottle(s_lastMissingVrHandsQueueLog, m_RenderPipelineDebugLogHz))
            {
                Game::logMsg("[VR][VrHands][Queued] source call queue missing; skipped queued hand draw tid=%lu ctx=%p ctxVtable=%p getCallQueue=%p eye=%d pass=%d",
                    GetCurrentThreadId(),
                    renderContext,
                    contextVtable,
                    getCallQueueFn,
                    eyeIndex,
                    static_cast<int>(drawPass));
            }
        }
        return false;
    }

    void* callQueueVtable = nullptr;
    if (!IsSourceCallQueueUsable(callQueue, &callQueueVtable))
    {
        if (m_RenderPipelineDebugLog)
        {
            static std::chrono::steady_clock::time_point s_lastInvalidVrHandsQueueLog{};
            if (!ShouldThrottle(s_lastInvalidVrHandsQueueLog, m_RenderPipelineDebugLogHz))
            {
                Game::logMsg("[VR][VrHands][Queued] source call queue invalid; skipped queued hand draw tid=%lu ctx=%p ctxVtable=%p getCallQueue=%p queue=%p queueVtable=%p eye=%d pass=%d",
                    GetCurrentThreadId(),
                    renderContext,
                    contextVtable,
                    getCallQueueFn,
                    callQueue,
                    callQueueVtable,
                    eyeIndex,
                    static_cast<int>(drawPass));
            }
        }
        return false;
    }

    const uint32_t epoch = m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire);
    callQueue->QueueFunctor(new VrHandsQueuedDrawFunctor(this, epoch, view, eyeIndex, drawPass));

    if (m_VrHandsDebugLog)
    {
        static thread_local std::chrono::steady_clock::time_point s_lastQueuedVrHandsDrawLog{};
        if (!ShouldThrottle(s_lastQueuedVrHandsDrawLog, m_RenderPipelineDebugLogHz))
        {
            Game::logMsg("[VR][VrHands][Queued] queued hand draw tid=%lu epoch=%u eye=%d pass=%d ctx=%p queue=%p",
                GetCurrentThreadId(),
                epoch,
                eyeIndex,
                static_cast<int>(drawPass),
                renderContext,
                callQueue);
        }
    }
    return true;
}

bool VR::QueueSourceRenderOwnershipReleaseMarker(IMatRenderContext* renderContext)
{
    if (!renderContext)
        return false;

    ICallQueue* callQueue = GetSourceRenderContextCallQueue(renderContext, nullptr, nullptr);
    if (!callQueue || !IsSourceCallQueueUsable(callQueue, nullptr))
        return false;

    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
    const uint32_t epoch = m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire);
    m_SourceRenderQueueAuxPendingCount.fetch_add(1, std::memory_order_acq_rel);
    callQueue->QueueFunctor(new SourceRenderOwnershipReleaseFunctor(this, epoch));
    return true;
}

bool VR::QueueSourceRenderCompletionMarker(
    IMatRenderContext* renderContext,
    uint32_t renderPoseToken,
    uint32_t renderFrameSeq,
    bool allowDuplicatePoseSubmit,
    const vr::HmdMatrix34_t* renderHmdPose)
{
    if (!renderContext)
        return false;

    const bool sourceMarkerDebugLog = m_QueuedSourceMarkerDebugLog;
    const bool queueDebugLog = m_RenderPipelineDebugLog || sourceMarkerDebugLog;
    const float queueDebugHz = sourceMarkerDebugLog ? m_QueuedSourceMarkerDebugLogHz : m_RenderPipelineDebugLogHz;

    void* contextVtable = nullptr;
    void* getCallQueueFn = nullptr;
    ICallQueue* callQueue = GetSourceRenderContextCallQueue(renderContext, &contextVtable, &getCallQueueFn);
    if (!callQueue)
    {
        if (queueDebugLog)
        {
            static std::chrono::steady_clock::time_point s_lastMissingSourceQueueLog{};
            if (!ShouldThrottle(s_lastMissingSourceQueueLog, queueDebugHz))
            {
                const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
                const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
                Game::logMsg("[VR][Queued][SourceQueueMissing] tid=%lu ctx=%p ctxVtable=%p getCallQueue=%p queuedMarker=%u completedMarker=%u markerLag=%u frameSeq=%u renderPose=%u",
                    GetCurrentThreadId(),
                    renderContext,
                    contextVtable,
                    getCallQueueFn,
                    queuedMarkerId,
                    completedMarkerId,
                    SourceMarkerLag(queuedMarkerId, completedMarkerId),
                    renderFrameSeq,
                    renderPoseToken);
            }
        }
        return false;
    }

    void* callQueueVtable = nullptr;
    if (!IsSourceCallQueueUsable(callQueue, &callQueueVtable))
    {
        if (queueDebugLog)
        {
            static std::chrono::steady_clock::time_point s_lastInvalidSourceQueueLog{};
            if (!ShouldThrottle(s_lastInvalidSourceQueueLog, queueDebugHz))
            {
                const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
                const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
                Game::logMsg("[VR][Queued][SourceQueueInvalid] tid=%lu ctx=%p ctxVtable=%p getCallQueue=%p queue=%p vtable=%p queuedMarker=%u completedMarker=%u markerLag=%u frameSeq=%u renderPose=%u",
                    GetCurrentThreadId(),
                    renderContext,
                    contextVtable,
                    getCallQueueFn,
                    callQueue,
                    callQueueVtable,
                    queuedMarkerId,
                    completedMarkerId,
                    SourceMarkerLag(queuedMarkerId, completedMarkerId),
                    renderFrameSeq,
                    renderPoseToken);
            }
        }
        return false;
    }

    uint32_t epoch = 0;
    uint32_t markerId = 0;
    uint32_t completedMarkerIdBeforeQueue = 0;
    {
        // Reset/texture recreation invalidates the epoch while holding this same mutex.
        // Keep epoch capture, marker allocation and queue insertion indivisible so an
        // old-epoch functor can never leave a new marker permanently outstanding.
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        epoch = m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire);
        markerId = m_SourceRenderQueueMarkerQueuedId.fetch_add(1, std::memory_order_acq_rel) + 1;
        completedMarkerIdBeforeQueue = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
        callQueue->QueueFunctor(new SourceRenderCompletionFunctor(
            this,
            epoch,
            markerId,
            renderPoseToken,
            renderFrameSeq,
            allowDuplicatePoseSubmit,
            renderHmdPose));
    }

    if (sourceMarkerDebugLog)
    {
        static thread_local std::chrono::steady_clock::time_point s_lastSourceMarkerQueueLog{};
        if (!ShouldThrottle(s_lastSourceMarkerQueueLog, m_QueuedSourceMarkerDebugLogHz))
        {
            const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
            const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
            Game::logMsg("[VR][Queued][SourceMarkerQueue] tid=%lu marker=%u epoch=%u frameSeq=%u renderPose=%u allowDup=%d queuedBefore=%u completedBefore=%u lagBefore=%u queuedMarker=%u completedMarker=%u markerLag=%u completedFrame=%u submittedFrame=%u stale=%u ctx=%p queue=%p ctxVtable=%p getCallQueue=%p queueVtable=%p",
                GetCurrentThreadId(),
                markerId,
                epoch,
                renderFrameSeq,
                renderPoseToken,
                allowDuplicatePoseSubmit ? 1 : 0,
                markerId - 1,
                completedMarkerIdBeforeQueue,
                SourceMarkerLag(markerId - 1, completedMarkerIdBeforeQueue),
                queuedMarkerId,
                completedMarkerId,
                SourceMarkerLag(queuedMarkerId, completedMarkerId),
                m_RenderCompletedFrameId.load(std::memory_order_acquire),
                m_LastSubmittedFrameId.load(std::memory_order_acquire),
                m_QueuedSubmitStaleStreak.load(std::memory_order_acquire),
                renderContext,
                callQueue,
                contextVtable,
                getCallQueueFn,
                callQueueVtable);
        }
    }
    return true;
}

void VR::PublishSourceQueueCompletedFrame(
    uint32_t sourceQueueEpoch,
    uint32_t renderPoseToken,
    uint32_t renderFrameSeq,
    bool allowDuplicatePoseSubmit,
    uint32_t sourceQueueMarkerId,
    const vr::HmdMatrix34_t* renderHmdPose)
{
    // This mutex is the ownership token for both the submit surfaces and their
    // published frame/pose generation. Present holds it while consuming a pair.
    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);

    if (m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != sourceQueueEpoch)
        return;

    m_SourceRenderQueueOwnershipUncertain.store(false, std::memory_order_release);

    const bool canSnapshotQueuedEyes =
        !m_ReShadeVRCompat &&
        g_D3DVR9 != nullptr &&
        m_CreatedVRTextures.load(std::memory_order_acquire) &&
        m_CreatingTextureID == Texture_None &&
        m_D9LeftEyeSurface != nullptr &&
        m_D9RightEyeSurface != nullptr &&
        m_D9LeftEyeSubmitSurface != nullptr &&
        m_D9RightEyeSubmitSurface != nullptr;

    if (canSnapshotQueuedEyes)
    {
        IDirect3DDevice9* device = nullptr;
        HRESULT deviceHr = m_D9LeftEyeSurface->GetDevice(&device);
        HRESULT leftHr = E_FAIL;
        HRESULT rightHr = E_FAIL;
        HRESULT overlayHr = E_FAIL;
        HRESULT leftTransferHr = E_FAIL;
        HRESULT rightTransferHr = E_FAIL;
        HRESULT submissionReadyHr = E_FAIL;
        HRESULT lockHr = E_FAIL;

        if (SUCCEEDED(deviceHr) && device)
        {
            // One real DXVK device lock makes the stereo copy an indivisible producer
            // transaction. Publish happens while textureLock is still held, so Present
            // cannot pair these pixels with an older pose/frame generation.
            lockHr = g_D3DVR9->LockDevice();
            if (SUCCEEDED(lockHr))
            {
                leftHr = device->StretchRect(
                    m_D9LeftEyeSurface, nullptr,
                    m_D9LeftEyeSubmitSurface, nullptr,
                    D3DTEXF_NONE);
                rightHr = device->StretchRect(
                    m_D9RightEyeSurface, nullptr,
                    m_D9RightEyeSubmitSurface, nullptr,
                    D3DTEXF_NONE);
                if (SUCCEEDED(leftHr) && SUCCEEDED(rightHr))
                {
                    // Item labels and the D3D aim line are part of this exact snapshot
                    // generation. Drawing them later from Present would invalidate the
                    // prepared layout/visibility proof and force a main-thread CS drain.
                    overlayHr = g_D3DVR9->DrawQueuedEyeSubmitOverlays(this);

                    // OpenVR's Vulkan path requires submitted images in transfer-source
                    // layout. These barriers stay ordered behind the stereo copies.
                    if (SUCCEEDED(overlayHr))
                    {
                        leftTransferHr = g_D3DVR9->TransferSurface(m_D9LeftEyeSubmitSurface, FALSE);
                        rightTransferHr = g_D3DVR9->TransferSurface(m_D9RightEyeSubmitSurface, FALSE);
                    }

                    if (SUCCEEDED(leftTransferHr) && SUCCEEDED(rightTransferHr))
                    {
                        // Establish the publish-time visibility proof while the producer
                        // still owns the D3D9 device: flush the copies/layout barriers,
                        // wait only for DXVK's CPU command stream to hand them to VkQueue,
                        // and briefly serialize that queue. The consumer can then use the
                        // lightweight prepared gate without draining the next frame's CS work.
                        submissionReadyHr = g_D3DVR9->LockSubmissionQueue();
                        if (SUCCEEDED(submissionReadyHr))
                            g_D3DVR9->UnlockSubmissionQueue();
                    }
                }
                g_D3DVR9->UnlockDevice();
            }
            device->Release();
        }

        if (SUCCEEDED(leftHr) && SUCCEEDED(rightHr) &&
            SUCCEEDED(overlayHr) &&
            SUCCEEDED(leftTransferHr) && SUCCEEDED(rightTransferHr) &&
            SUCCEEDED(submissionReadyHr))
        {
            m_QueuedEyeSubmitIsolationReady.store(true, std::memory_order_release);
            const uint32_t publishedFrameId = PublishRenderCompletedFrame(
                renderPoseToken,
                renderFrameSeq,
                allowDuplicatePoseSubmit,
                "source-queue-snapshot",
                sourceQueueMarkerId,
                renderHmdPose);
            m_PostPresentSubmitOverlayFrameId.store(publishedFrameId, std::memory_order_release);
            return;
        }

        // A half-copied pair is never consumable. Retire the ownership marker so the
        // queue cannot wedge permanently, but do not publish a new render generation.
        m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
        m_RenderedNewFrame.store(false, std::memory_order_release);
        m_SourceRenderQueueMarkerCompletedId.store(sourceQueueMarkerId, std::memory_order_release);
        if (m_RenderFrameReadyEvent)
            SetEvent(m_RenderFrameReadyEvent);

        if (m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog)
        {
            Game::logMsg("[VR][Queued][SnapshotDrop] tid=%lu epoch=%u marker=%u frameSeq=%u device=0x%08X lock=0x%08X left=0x%08X right=0x%08X overlay=0x%08X xferL=0x%08X xferR=0x%08X ready=0x%08X",
                GetCurrentThreadId(),
                sourceQueueEpoch,
                sourceQueueMarkerId,
                renderFrameSeq,
                static_cast<unsigned int>(deviceHr),
                static_cast<unsigned int>(lockHr),
                static_cast<unsigned int>(leftHr),
                static_cast<unsigned int>(rightHr),
                static_cast<unsigned int>(overlayHr),
                static_cast<unsigned int>(leftTransferHr),
                static_cast<unsigned int>(rightTransferHr),
                static_cast<unsigned int>(submissionReadyHr));
        }
        return;
    }

    if (m_ReShadeVRCompat)
    {
        // ReShade publishes the Source completion here; Present owns its dedicated
        // resolve/pre-bake transaction before the pair becomes submit-ready.
        PublishRenderCompletedFrame(
            renderPoseToken,
            renderFrameSeq,
            allowDuplicatePoseSubmit,
            "source-queue-direct",
            sourceQueueMarkerId,
            renderHmdPose);
        return;
    }

    // A normal queued frame without the dedicated snapshot resources is not safe to
    // publish. In particular, never associate a newer pose/frame id with pixels left
    // in the submit pair by an older successful snapshot.
    m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_SourceRenderQueueMarkerCompletedId.store(sourceQueueMarkerId, std::memory_order_release);
    if (m_RenderFrameReadyEvent)
        SetEvent(m_RenderFrameReadyEvent);

    if (m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog)
    {
        Game::logMsg("[VR][Queued][SnapshotUnavailable] tid=%lu epoch=%u marker=%u frameSeq=%u created=%d creating=%d d3dvr=%p rawL=%p rawR=%p submitL=%p submitR=%p",
            GetCurrentThreadId(),
            sourceQueueEpoch,
            sourceQueueMarkerId,
            renderFrameSeq,
            m_CreatedVRTextures.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<int>(m_CreatingTextureID),
            g_D3DVR9,
            m_D9LeftEyeSurface,
            m_D9RightEyeSurface,
            m_D9LeftEyeSubmitSurface,
            m_D9RightEyeSubmitSurface);
    }
}

uint32_t VR::PublishRenderCompletedFrame(
    uint32_t renderPoseToken,
    uint32_t renderFrameSeq,
    bool allowDuplicatePoseSubmit,
    const char* sourceTag,
    uint32_t sourceQueueMarkerId,
    const vr::HmdMatrix34_t* renderHmdPose)
{
    if (renderPoseToken == 0)
        renderPoseToken = m_SubmitPoseToken.load(std::memory_order_acquire);

    const uint32_t completedFrameId = m_RenderCompletedFrameId.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_RenderCompletedPoseToken.store(renderPoseToken, std::memory_order_release);
    m_RenderCompletedDuplicatePoseFrameId.store(allowDuplicatePoseSubmit ? completedFrameId : 0u, std::memory_order_release);
    {
        std::lock_guard<std::mutex> poseLock(m_RenderCompletedHmdPoseMutex);
        if (renderHmdPose)
        {
            m_RenderCompletedHmdPose = *renderHmdPose;
            m_RenderCompletedHmdPoseValid = true;
        }
        else
        {
            m_RenderCompletedHmdPoseValid = false;
        }
    }
    if (sourceQueueMarkerId != 0)
        m_SourceRenderQueueMarkerCompletedId.store(sourceQueueMarkerId, std::memory_order_release);
    m_RenderedNewFrame.store(true, std::memory_order_release);
    if (m_RenderFrameReadyEvent)
        SetEvent(m_RenderFrameReadyEvent);

    if (m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog)
    {
        static thread_local std::chrono::steady_clock::time_point s_lastSourceQueuePublishLog{};
        const float logHz = m_QueuedSourceMarkerDebugLog ? m_QueuedSourceMarkerDebugLogHz : m_RenderPipelineDebugLogHz;
        if (!ShouldThrottle(s_lastSourceQueuePublishLog, logHz))
        {
            const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
            const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
            Game::logMsg("[VR][Queued][RenderComplete:%s] tid=%lu completed=%u frameSeq=%u renderPose=%u marker=%u queuedMarker=%u completedMarker=%u markerLag=%u poseSeq=%u submitPose=%u lastSubmitted=%u duplicateFrame=%u stale=%u renderedNew=%d",
                (sourceTag && *sourceTag) ? sourceTag : "unknown",
                GetCurrentThreadId(),
                completedFrameId,
                renderFrameSeq,
                renderPoseToken,
                sourceQueueMarkerId,
                queuedMarkerId,
                completedMarkerId,
                SourceMarkerLag(queuedMarkerId, completedMarkerId),
                m_PoseWaiterSeq.load(std::memory_order_acquire),
                m_SubmitPoseToken.load(std::memory_order_acquire),
                m_LastSubmittedFrameId.load(std::memory_order_acquire),
                m_RenderCompletedDuplicatePoseFrameId.load(std::memory_order_acquire),
                m_QueuedSubmitStaleStreak.load(std::memory_order_acquire),
                m_RenderedNewFrame.load(std::memory_order_acquire) ? 1 : 0);
        }
    }

    return completedFrameId;
}

vr::EVROverlayError VR::SetOverlayTextureSynchronized(
    vr::IVROverlay* overlay,
    vr::VROverlayHandle_t overlayHandle,
    const vr::Texture_t* texture,
    bool producerPrepared)
{
    if (!overlay || !texture)
        return vr::VROverlayError_RequestFailed;

    if (!g_D3DVR9)
        return overlay->SetOverlayTexture(overlayHandle, texture);

    // All D3D producer work must be complete before entering either queue gate.
    // The prepared form only serializes native VkQueue access; the heavy form also
    // flushes/synchronizes DXVK's D3D command stream before taking that same gate.
    const HRESULT lockHr = producerPrepared
        ? g_D3DVR9->LockPreparedSubmissionQueue()
        : g_D3DVR9->LockSubmissionQueue();
    if (FAILED(lockHr))
        return vr::VROverlayError_RequestFailed;

    const vr::EVROverlayError result = overlay->SetOverlayTexture(overlayHandle, texture);
    g_D3DVR9->UnlockSubmissionQueue();
    return result;
}

void VR::SubmitVRTextures()
{
    if (!m_Compositor)
        return;

    const bool inGame = (m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame());
    bool hasLocalPlayer = false;
    if (inGame && m_Game)
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        hasLocalPlayer = (m_Game->GetClientEntity(playerIndex) != nullptr);
    }
    const bool sceneReadyForStaleResubmit = inGame && hasLocalPlayer;
    const bool queued = (m_Game && (m_Game->GetMatQueueMode() != 0));
    bool renderedNewFrame = m_RenderedNewFrame.load(std::memory_order_acquire);
    if (inGame && !hasLocalPlayer)
        renderedNewFrame = false;

    if (!inGame && renderedNewFrame)
    {
        // A render can complete on the old map after the engine has already returned to
        // the main menu. Never let that stale stereo/HUD frame enter the menu path.
        ClearVRMenuTransitionResiduals(this, "Submit not in game with pending frame");
        renderedNewFrame = false;
    }

    // In the main menu, keep the scene side of the compositor static after the first
    // successful blank submit. Repeating blank Vulkan submits every frame is unnecessary
    // because the visible menu is an overlay, and on some NVIDIA setups it eventually
    // trips driver/runtime instability inside vrclient/vulkan.
    if (!renderedNewFrame && !inGame && m_MenuBlankSubmitted)
    {
        if (!RefreshBackBufferTexture(false))
            return;

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        SetOverlayTextureSynchronized(
            vr::VROverlay(),
            m_MainMenuHandle,
            &m_VKBackBuffer.m_VRTexture,
            false);
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        vr::VROverlay()->HideOverlay(m_HUDTopHandle);
        for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
            vr::VROverlay()->HideOverlay(overlay);
        vr::VROverlay()->HideOverlay(m_ScopeHandle);
        vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
        UpdateDesktopRearMirrorWindow(false);
        vr::VROverlay()->HideOverlay(m_LeftWristHudHandle);
        vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);
        m_CompositorNeedsHandoff = false;
        return;
    }

    if (renderedNewFrame || inGame)
        m_MenuBlankSubmitted = false;

    static const vr::VRTextureBounds_t fullEyeSubmitBounds{ 0.0f, 0.0f, 1.0f, 1.0f };

    const bool reshadeSubmitPrebakedToFullBounds =
        m_ReShadeVRCompat &&
        m_D9LeftEyeSubmitSurface != nullptr &&
        m_D9RightEyeSubmitSurface != nullptr;

    // Normal path: submit the raw eye textures with SteamVR's asymmetric per-eye crop.
    // ReShade/OpenVR compatibility path: the D3D9 Present path has already copied the
    // cropped part of each eye texture into a dedicated full-frame submit texture, so
    // submit with full bounds. This avoids ALVR/ReShade dropping the first eye while
    // preserving the same effective projection crop as the normal path.
    const vr::VRTextureBounds_t* leftEyeSubmitBounds = reshadeSubmitPrebakedToFullBounds
        ? &fullEyeSubmitBounds
        : &(m_TextureBounds)[0];
    const vr::VRTextureBounds_t* rightEyeSubmitBounds = reshadeSubmitPrebakedToFullBounds
        ? &fullEyeSubmitBounds
        : &(m_TextureBounds)[1];

    if (m_QueuedSourceMarkerDebugLog && queued && sceneReadyForStaleResubmit)
    {
        static std::chrono::steady_clock::time_point s_lastSourceMarkerSubmitLog{};
        if (!ShouldThrottle(s_lastSourceMarkerSubmitLog, m_QueuedSourceMarkerDebugLogHz))
        {
            const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
            const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
            const uint32_t renderCompletedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
            const uint32_t lastSubmittedFrameId = m_LastSubmittedFrameId.load(std::memory_order_acquire);
            Game::logMsg("[VR][Queued][SourceMarkerSubmit] tid=%lu renderedNew=%d completed=%u submitted=%u frameLag=%u queuedMarker=%u completedMarker=%u markerLag=%u submitPose=%u renderPose=%u lastPose=%u duplicateFrame=%u stale=%u submitWaitMs=%d useRenderPose=%d",
                GetCurrentThreadId(),
                renderedNewFrame ? 1 : 0,
                renderCompletedFrameId,
                lastSubmittedFrameId,
                renderCompletedFrameId - lastSubmittedFrameId,
                queuedMarkerId,
                completedMarkerId,
                SourceMarkerLag(queuedMarkerId, completedMarkerId),
                m_SubmitPoseToken.load(std::memory_order_acquire),
                m_RenderCompletedPoseToken.load(std::memory_order_acquire),
                m_LastSubmittedPoseToken.load(std::memory_order_acquire),
                m_RenderCompletedDuplicatePoseFrameId.load(std::memory_order_acquire),
                m_QueuedSubmitStaleStreak.load(std::memory_order_acquire),
                std::clamp(m_QueuedSubmitWaitMs, 0, 20),
                m_QueuedSubmitUseRenderPoseToken ? 1 : 0);
        }
    }

    if (m_RenderPipelineDebugLog && !ShouldThrottle(m_RenderPipelineLastSubmitLog, m_RenderPipelineDebugLogHz))
    {
        const uint32_t renderCompletedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
        const uint32_t lastSubmittedFrameId = m_LastSubmittedFrameId.load(std::memory_order_acquire);
        const uint32_t currentPoseToken = m_SubmitPoseToken.load(std::memory_order_acquire);
        const uint32_t lastSubmittedPoseToken = m_LastSubmittedPoseToken.load(std::memory_order_acquire);
        int windowW = 0;
        int windowH = 0;
        int backBufferW = 0;
        int backBufferH = 0;
        int hudMapW = 0;
        int hudMapH = 0;
        int hudActualW = 0;
        int hudActualH = 0;
        if (m_Game && m_Game->m_MaterialSystem)
        {
            if (IMatRenderContext* ctx = m_Game->m_MaterialSystem->GetRenderContext())
                ctx->GetWindowSize(windowW, windowH);
            m_Game->m_MaterialSystem->GetBackBufferDimensions(backBufferW, backBufferH);
        }
        {
            std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
            if (m_HUDTexture)
            {
                hudMapW = m_HUDTexture->GetMappingWidth();
                hudMapH = m_HUDTexture->GetMappingHeight();
                hudActualW = m_HUDTexture->GetActualWidth();
                hudActualH = m_HUDTexture->GetActualHeight();
            }
        }

        Game::logMsg("[VR][DesktopHUD][Submit] tid=%lu q=%d inGame=%d renderedNew=%d completed=%u submitted=%u pose=%u lastPose=%u submitInFlight=%d renderedHud=%d hudPainted=%d menuBlank=%d win=%dx%d bb=%dx%d hudTex=%dx%d actual=%dx%d",
            GetCurrentThreadId(), queued ? 1 : 0, inGame ? 1 : 0,
            renderedNewFrame ? 1 : 0,
            renderCompletedFrameId, lastSubmittedFrameId,
            currentPoseToken, lastSubmittedPoseToken,
            m_SubmitInFlight.load(std::memory_order_acquire) ? 1 : 0,
            m_RenderedHud.load(std::memory_order_acquire) ? 1 : 0,
            m_HudPaintedThisFrame.load(std::memory_order_acquire) ? 1 : 0,
            m_MenuBlankSubmitted ? 1 : 0,
            windowW, windowH, backBufferW, backBufferH, hudMapW, hudMapH, hudActualW, hudActualH);
    }

    struct SubmitInFlightGuard
    {
        std::atomic<bool>* flag = nullptr;
        ~SubmitInFlightGuard()
        {
            if (flag)
                flag->store(false, std::memory_order_release);
        }
    } submitInFlightGuard{};

    uint32_t poseToken = 0;
    uint32_t renderPoseToken = 0;
    uint32_t compositorFrameIndex = 0;
    if (queued)
    {
        bool expectedSubmitInFlight = false;
        if (!m_SubmitInFlight.compare_exchange_strong(expectedSubmitInFlight, true, std::memory_order_acq_rel))
            return;
        submitInFlightGuard.flag = &m_SubmitInFlight;

        poseToken = m_SubmitPoseToken.load(std::memory_order_acquire);
        if (poseToken == 0)
            return;
        const uint32_t lastSubmittedTokenAtPoseRead = m_LastSubmittedPoseToken.load(std::memory_order_acquire);
        if (!m_QueuedSubmitUseRenderPoseToken && poseToken == lastSubmittedTokenAtPoseRead)
            return;

        auto queryCompositorFrameIndex = [&]() -> uint32_t
            {
                vr::Compositor_FrameTiming timing{};
                timing.m_nSize = sizeof(timing);
                if (m_Compositor->GetFrameTiming(&timing, 0) && timing.m_nFrameIndex != 0)
                    return timing.m_nFrameIndex;
                return 0;
            };

        compositorFrameIndex = queryCompositorFrameIndex();
        const uint32_t lastSubmittedCompositorFrameIndex = m_LastSubmittedCompositorFrameIndex.load(std::memory_order_acquire);
        if (compositorFrameIndex != 0 && compositorFrameIndex == lastSubmittedCompositorFrameIndex)
        {
            // Same compositor frame index: treat as already handled for this submit token.
            if (!m_QueuedSubmitUseRenderPoseToken)
                m_LastSubmittedPoseToken.store(poseToken, std::memory_order_release);
            return;
        }
    }

    if (queued && sceneReadyForStaleResubmit && m_QueuedSubmitUseRenderPoseToken)
    {
        uint32_t completedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
        uint32_t lastSubmittedFrameId = m_LastSubmittedFrameId.load(std::memory_order_acquire);
        const bool haveUnsubmittedFrame = (completedFrameId != 0 && completedFrameId != lastSubmittedFrameId);

        if (!renderedNewFrame && haveUnsubmittedFrame)
            renderedNewFrame = true;

        bool waitedForRenderFrame = false;
        DWORD waitResult = WAIT_TIMEOUT;
        const uint32_t staleStreakBefore = m_QueuedSubmitStaleStreak.load(std::memory_order_acquire);
        const int submitWaitMs = std::clamp(m_QueuedSubmitWaitMs, 0, 20);
        if (!renderedNewFrame && submitWaitMs > 0 && m_RenderFrameReadyEvent)
        {
            waitedForRenderFrame = true;
            waitResult = WaitForSingleObject(m_RenderFrameReadyEvent, static_cast<DWORD>(submitWaitMs));
            renderedNewFrame = m_RenderedNewFrame.load(std::memory_order_acquire);
            completedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
            lastSubmittedFrameId = m_LastSubmittedFrameId.load(std::memory_order_acquire);
            if (!renderedNewFrame && completedFrameId != 0 && completedFrameId != lastSubmittedFrameId)
                renderedNewFrame = true;
        }

        if (renderedNewFrame)
            m_QueuedSubmitStaleStreak.store(0, std::memory_order_release);
        else
            m_QueuedSubmitStaleStreak.fetch_add(1, std::memory_order_acq_rel);

        if ((m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog) && (waitedForRenderFrame || !renderedNewFrame))
        {
            static std::chrono::steady_clock::time_point s_lastQueuedSubmitWaitLog{};
            const float logHz = m_QueuedSourceMarkerDebugLog ? m_QueuedSourceMarkerDebugLogHz : m_RenderPipelineDebugLogHz;
            if (!ShouldThrottle(s_lastQueuedSubmitWaitLog, logHz))
            {
                const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
                const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
                Game::logMsg("[VR][Queued][SubmitWait] tid=%lu waitMs=%d waited=%d waitResult=0x%08lX renderedNew=%d completed=%u submitted=%u frameLag=%u queuedMarker=%u completedMarker=%u markerLag=%u pose=%u renderPose=%u lastPose=%u stale=%u->%u",
                    GetCurrentThreadId(), submitWaitMs,
                    waitedForRenderFrame ? 1 : 0,
                    static_cast<unsigned long>(waitResult),
                    renderedNewFrame ? 1 : 0,
                    completedFrameId,
                    lastSubmittedFrameId,
                    completedFrameId - lastSubmittedFrameId,
                    queuedMarkerId,
                    completedMarkerId,
                    SourceMarkerLag(queuedMarkerId, completedMarkerId),
                    poseToken,
                    m_RenderCompletedPoseToken.load(std::memory_order_acquire),
                    m_LastSubmittedPoseToken.load(std::memory_order_acquire),
                    staleStreakBefore,
                    m_QueuedSubmitStaleStreak.load(std::memory_order_acquire));
            }
        }
    }

    if (queued && sceneReadyForStaleResubmit && m_QueuedSubmitUseRenderPoseToken)
    {
        renderPoseToken = m_RenderCompletedPoseToken.load(std::memory_order_acquire);
        uint32_t effectivePoseToken = (renderPoseToken != 0) ? renderPoseToken : poseToken;
        const uint32_t lastSubmittedToken = m_LastSubmittedPoseToken.load(std::memory_order_acquire);
        const uint32_t poseCheckCompletedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
        const uint32_t poseCheckLastSubmittedFrameId = m_LastSubmittedFrameId.load(std::memory_order_acquire);
        const uint32_t duplicatePoseFrameId = m_RenderCompletedDuplicatePoseFrameId.load(std::memory_order_acquire);
        const bool haveUnsubmittedRenderFrame =
            poseCheckCompletedFrameId != 0 &&
            poseCheckCompletedFrameId != poseCheckLastSubmittedFrameId;
        bool allowDuplicatePoseSubmit =
            haveUnsubmittedRenderFrame &&
            duplicatePoseFrameId == poseCheckCompletedFrameId;

        if (effectivePoseToken == lastSubmittedToken && poseToken != lastSubmittedToken && !allowDuplicatePoseSubmit && m_RenderFrameReadyEvent)
        {
            const DWORD duplicatePoseWaitMs = static_cast<DWORD>(std::clamp(m_QueuedSubmitWaitMs, 0, 20));
            if (duplicatePoseWaitMs > 0)
            {
                const DWORD duplicateWaitResult = WaitForSingleObject(m_RenderFrameReadyEvent, duplicatePoseWaitMs);
                const uint32_t waitedRenderPoseToken = m_RenderCompletedPoseToken.load(std::memory_order_acquire);
                const uint32_t waitedCompletedFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
                if (waitedRenderPoseToken != 0)
                {
                    renderPoseToken = waitedRenderPoseToken;
                    effectivePoseToken = waitedRenderPoseToken;
                }
                allowDuplicatePoseSubmit =
                    waitedCompletedFrameId != 0 &&
                    waitedCompletedFrameId != m_LastSubmittedFrameId.load(std::memory_order_acquire) &&
                    m_RenderCompletedDuplicatePoseFrameId.load(std::memory_order_acquire) == waitedCompletedFrameId;

                if (m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog)
                {
                    static std::chrono::steady_clock::time_point s_lastQueuedSubmitPoseWaitLog{};
                    const float logHz = m_QueuedSourceMarkerDebugLog ? m_QueuedSourceMarkerDebugLogHz : m_RenderPipelineDebugLogHz;
                    if (!ShouldThrottle(s_lastQueuedSubmitPoseWaitLog, logHz))
                    {
                        const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
                        const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
                        const uint32_t completedNow = m_RenderCompletedFrameId.load(std::memory_order_acquire);
                        const uint32_t submittedNow = m_LastSubmittedFrameId.load(std::memory_order_acquire);
                        Game::logMsg("[VR][Queued][SubmitPoseWait] tid=%lu waitMs=%lu result=0x%08lX currentPose=%u renderPose=%u lastPose=%u completed=%u submitted=%u frameLag=%u queuedMarker=%u completedMarker=%u markerLag=%u renderedNew=%d",
                            GetCurrentThreadId(),
                            static_cast<unsigned long>(duplicatePoseWaitMs),
                            static_cast<unsigned long>(duplicateWaitResult),
                            poseToken,
                            renderPoseToken,
                            lastSubmittedToken,
                            completedNow,
                            submittedNow,
                            completedNow - submittedNow,
                            queuedMarkerId,
                            completedMarkerId,
                            SourceMarkerLag(queuedMarkerId, completedMarkerId),
                            m_RenderedNewFrame.load(std::memory_order_acquire) ? 1 : 0);
                    }
                }
            }
        }

        if (effectivePoseToken == 0 || (effectivePoseToken == lastSubmittedToken && !allowDuplicatePoseSubmit))
        {
            if (m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog)
            {
                static std::chrono::steady_clock::time_point s_lastQueuedSubmitSkipLog{};
                const float logHz = m_QueuedSourceMarkerDebugLog ? m_QueuedSourceMarkerDebugLogHz : m_RenderPipelineDebugLogHz;
                if (!ShouldThrottle(s_lastQueuedSubmitSkipLog, logHz))
                {
                    const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
                    const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
                    const uint32_t completedNow = m_RenderCompletedFrameId.load(std::memory_order_acquire);
                    const uint32_t submittedNow = m_LastSubmittedFrameId.load(std::memory_order_acquire);
                    Game::logMsg("[VR][Queued][SubmitSkip] tid=%lu currentPose=%u renderPose=%u lastPose=%u completed=%u submitted=%u frameLag=%u queuedMarker=%u completedMarker=%u markerLag=%u duplicateFrame=%u renderedNew=%d",
                        GetCurrentThreadId(),
                        poseToken,
                        renderPoseToken,
                        lastSubmittedToken,
                        completedNow,
                        submittedNow,
                        completedNow - submittedNow,
                        queuedMarkerId,
                        completedMarkerId,
                        SourceMarkerLag(queuedMarkerId, completedMarkerId),
                        m_RenderCompletedDuplicatePoseFrameId.load(std::memory_order_acquire),
                        m_RenderedNewFrame.load(std::memory_order_acquire) ? 1 : 0);
                }
            }
            return;
        }
        if (effectivePoseToken == lastSubmittedToken && allowDuplicatePoseSubmit && (m_RenderPipelineDebugLog || m_QueuedSourceMarkerDebugLog))
        {
            static std::chrono::steady_clock::time_point s_lastQueuedSubmitDuplicatePoseLog{};
            const float logHz = m_QueuedSourceMarkerDebugLog ? m_QueuedSourceMarkerDebugLogHz : m_RenderPipelineDebugLogHz;
            if (!ShouldThrottle(s_lastQueuedSubmitDuplicatePoseLog, logHz))
            {
                const uint32_t queuedMarkerId = m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
                const uint32_t completedMarkerId = m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
                const uint32_t completedNow = m_RenderCompletedFrameId.load(std::memory_order_acquire);
                const uint32_t submittedNow = m_LastSubmittedFrameId.load(std::memory_order_acquire);
                Game::logMsg("[VR][Queued][SubmitDuplicatePose] tid=%lu currentPose=%u renderPose=%u lastPose=%u completed=%u submitted=%u frameLag=%u queuedMarker=%u completedMarker=%u markerLag=%u duplicateFrame=%u",
                    GetCurrentThreadId(),
                    poseToken,
                    renderPoseToken,
                    lastSubmittedToken,
                    completedNow,
                    submittedNow,
                    completedNow - submittedNow,
                    queuedMarkerId,
                    completedMarkerId,
                    SourceMarkerLag(queuedMarkerId, completedMarkerId),
                    m_RenderCompletedDuplicatePoseFrameId.load(std::memory_order_acquire));
            }
        }
        poseToken = effectivePoseToken;
    }

    bool successfulSubmit = false;
    bool frameHandled = false;
    bool timingDataSubmitted = false;
    uint32_t submitSnapshotFrameId = 0;

    auto finalizeSubmitState = [&](bool markFrameHandled)
        {
            if (!queued || !markFrameHandled)
                return;

            frameHandled = true;
            m_LastSubmittedPoseToken.store(poseToken, std::memory_order_release);
            if (compositorFrameIndex != 0)
                m_LastSubmittedCompositorFrameIndex.store(compositorFrameIndex, std::memory_order_release);

            const uint32_t renderedFrameId = submitSnapshotFrameId != 0
                ? submitSnapshotFrameId
                : m_RenderCompletedFrameId.load(std::memory_order_acquire);
            if (renderedFrameId != 0)
                m_LastSubmittedFrameId.store(renderedFrameId, std::memory_order_release);
        };

    auto ensureTimingData = [&]()
        {
            if (!m_CompositorExplicitTiming || timingDataSubmitted)
                return;

            vr::EVRCompositorError timingError = m_Compositor->SubmitExplicitTimingData();
            if (timingError != vr::VRCompositorError_None)
            {
                LogCompositorError("SubmitExplicitTimingData", timingError);
            }

            timingDataSubmitted = true;
        };

    vr::HmdMatrix34_t completedRenderHmdPose{};
    bool useRenderPoseTextureSubmit = false;
    if (queued &&
        sceneReadyForStaleResubmit &&
        m_QueuedRenderPoseFromTracking &&
        m_QueuedSubmitUseRenderPoseToken &&
        !m_ReShadeVRCompat)
    {
        std::lock_guard<std::mutex> poseLock(m_RenderCompletedHmdPoseMutex);
        if (m_RenderCompletedHmdPoseValid)
        {
            completedRenderHmdPose = m_RenderCompletedHmdPose;
            useRenderPoseTextureSubmit = true;
        }
    }

    auto submitEye = [&](vr::EVREye eye, vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds)
        {
            if (!texture || !texture->handle)
                return false;

            ensureTimingData();

            vr::Texture_t textureCopy{};
            vr::VRTextureWithPose_t textureWithPose{};
            const vr::Texture_t* submitTexture = &textureCopy;
            vr::EVRSubmitFlags submitFlags = vr::Submit_Default;
            {
                std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
                if (!texture->handle)
                    return false;

                textureCopy = *texture;
                if (useRenderPoseTextureSubmit)
                {
                    textureWithPose.handle = textureCopy.handle;
                    textureWithPose.eType = textureCopy.eType;
                    textureWithPose.eColorSpace = textureCopy.eColorSpace;
                    textureWithPose.mDeviceToAbsoluteTracking = completedRenderHmdPose;
                    submitTexture = &textureWithPose;
                    submitFlags = vr::Submit_TextureWithPose;
                }
            }

            vr::EVRCompositorError submitError = m_Compositor->Submit(eye, submitTexture, bounds, submitFlags);
            if (submitError != vr::VRCompositorError_None &&
                useRenderPoseTextureSubmit &&
                submitError != vr::VRCompositorError_AlreadySubmitted)
            {
                if (m_RenderPipelineDebugLog)
                {
                    static std::chrono::steady_clock::time_point s_lastTextureWithPoseFallbackLog{};
                    if (!ShouldThrottle(s_lastTextureWithPoseFallbackLog, m_RenderPipelineDebugLogHz))
                    {
                        Game::logMsg("[VR][Queued][TextureWithPoseFallback] tid=%lu eye=%d err=%d renderPose=%u completed=%u submitted=%u",
                            GetCurrentThreadId(),
                            static_cast<int>(eye),
                            static_cast<int>(submitError),
                            m_RenderCompletedPoseToken.load(std::memory_order_acquire),
                            m_RenderCompletedFrameId.load(std::memory_order_acquire),
                            m_LastSubmittedFrameId.load(std::memory_order_acquire));
                    }
                }
                submitError = m_Compositor->Submit(eye, &textureCopy, bounds, vr::Submit_Default);
            }
            if (submitError != vr::VRCompositorError_None)
            {
                if (queued && submitError == vr::VRCompositorError_AlreadySubmitted)
                {
                    // Another submit already consumed this compositor frame.
                    finalizeSubmitState(true);
                    return false;
                }
                LogCompositorError("Submit", submitError);
                return false;
            }

            return true;
        };

    PendingOverlayTextureBindBatch pendingOverlayTextureBinds{};
    auto stageOverlayTextureBind = [&](vr::VROverlayHandle_t overlay, const vr::Texture_t* texture,
        IDirect3DSurface9* surface, bool hideOnFailure = false)
        {
            return pendingOverlayTextureBinds.Stage(overlay, texture, surface, hideOnFailure);
        };

    auto submitStereoPair = [&](vr::Texture_t* leftTexture, const vr::VRTextureBounds_t* leftBounds,
        vr::Texture_t* rightTexture, const vr::VRTextureBounds_t* rightBounds) -> bool
        {
            // Keep the submit descriptors, pixels and frame/pose generation together.
            // The Source completion functor takes the same mutex before replacing the pair.
            std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);

            const bool sceneEyePair =
                leftTexture == &m_VKLeftEye.m_VRTexture &&
                rightTexture == &m_VKRightEye.m_VRTexture;
            const bool preparedQueuedEyePair =
                queued && sceneEyePair &&
                !m_ReShadeVRCompat &&
                m_QueuedEyeSubmitIsolationReady.load(std::memory_order_acquire);
            if (queued && sceneReadyForStaleResubmit && sceneEyePair &&
                !m_ReShadeVRCompat &&
                !preparedQueuedEyePair)
            {
                return false;
            }

            submitSnapshotFrameId = m_RenderCompletedFrameId.load(std::memory_order_acquire);
            if (queued && sceneEyePair && m_QueuedSubmitUseRenderPoseToken)
            {
                useRenderPoseTextureSubmit = false;
                const uint32_t lockedRenderPoseToken = m_RenderCompletedPoseToken.load(std::memory_order_acquire);
                if (lockedRenderPoseToken != 0)
                    poseToken = lockedRenderPoseToken;

                if (sceneReadyForStaleResubmit &&
                    m_QueuedRenderPoseFromTracking &&
                    !m_ReShadeVRCompat)
                {
                    std::lock_guard<std::mutex> poseLock(m_RenderCompletedHmdPoseMutex);
                    if (m_RenderCompletedHmdPoseValid)
                    {
                        completedRenderHmdPose = m_RenderCompletedHmdPose;
                        useRenderPoseTextureSubmit = true;
                    }
                }
            }

            // Queue overlay layout transitions before taking the native VkQueue gate.
            // A staged overlay forces the heavy gate below so these new D3D commands
            // are flushed and synchronized exactly once for the whole consumer batch.
            if (g_D3DVR9)
            {
                for (size_t i = 0; i < pendingOverlayTextureBinds.count; ++i)
                {
                    PendingOverlayTextureBind& bind = pendingOverlayTextureBinds.binds[i];
                    if (bind.surface && FAILED(g_D3DVR9->TransferSurface(bind.surface, FALSE)))
                        bind.texture = nullptr;
                }
            }

            // Queued normal eyes were copied, transitioned and flushed before their
            // completion generation was published. Other paths (single-threaded raw
            // eyes and the ReShade resolve path) still prepare their layout here.
            if (g_D3DVR9 && sceneEyePair && !preparedQueuedEyePair)
            {
                IDirect3DSurface9* leftSurface = m_D9LeftEyeSubmitSurface
                    ? m_D9LeftEyeSubmitSurface : m_D9LeftEyeSurface;
                IDirect3DSurface9* rightSurface = m_D9RightEyeSubmitSurface
                    ? m_D9RightEyeSubmitSurface : m_D9RightEyeSurface;
                if (!leftSurface || !rightSurface ||
                    FAILED(g_D3DVR9->TransferSurface(leftSurface, FALSE)) ||
                    FAILED(g_D3DVR9->TransferSurface(rightSurface, FALSE)))
                {
                    return false;
                }
            }

            struct SubmissionQueueGuard
            {
                bool locked = false;
                ~SubmissionQueueGuard()
                {
                    if (locked && g_D3DVR9)
                        g_D3DVR9->UnlockSubmissionQueue();
                }
            } submissionQueueGuard;

            if (g_D3DVR9)
            {
                const bool canUsePreparedQueueGate =
                    preparedQueuedEyePair && pendingOverlayTextureBinds.count == 0;
                const HRESULT queueLockHr = canUsePreparedQueueGate
                    ? g_D3DVR9->LockPreparedSubmissionQueue()
                    : g_D3DVR9->LockSubmissionQueue();
                if (FAILED(queueLockHr))
                    return false;
                submissionQueueGuard.locked = true;
            }

            // All D3D producers have completed before this point. Publish the scene's
            // Vulkan-backed overlays while the same native queue gate used for the eye
            // pair is held, avoiding one queue drain/lock cycle per overlay.
            if (vr::IVROverlay* overlayApi = vr::VROverlay())
            {
                for (size_t i = 0; i < pendingOverlayTextureBinds.count; ++i)
                {
                    const PendingOverlayTextureBind& bind = pendingOverlayTextureBinds.binds[i];
                    const vr::EVROverlayError bindError = bind.texture
                        ? overlayApi->SetOverlayTexture(bind.overlay, bind.texture)
                        : vr::VROverlayError_RequestFailed;
                    if (bindError != vr::VROverlayError_None && bind.hideOnFailure)
                        overlayApi->HideOverlay(bind.overlay);
                }
            }
            pendingOverlayTextureBinds.Clear();

            if (queued)
            {
                if (!submitEye(vr::Eye_Left, leftTexture, leftBounds))
                    return false;

                if (!submitEye(vr::Eye_Right, rightTexture, rightBounds))
                    return false;

                successfulSubmit = true;
                finalizeSubmitState(true);
                if (m_CompositorExplicitTiming)
                {
                    m_CompositorNeedsHandoff = true;
                    FinishFrame();
                }
                return true;
            }

            const bool leftOk = submitEye(vr::Eye_Left, leftTexture, leftBounds);
            const bool rightOk = submitEye(vr::Eye_Right, rightTexture, rightBounds);
            successfulSubmit = leftOk || rightOk;
            if (successfulSubmit && m_CompositorExplicitTiming)
            {
                m_CompositorNeedsHandoff = true;
                FinishFrame();
            }
            return successfulSubmit;
        };

    auto hideHudOverlays = [&]()
        {
            vr::VROverlay()->HideOverlay(m_HUDTopHandle);
            for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
                vr::VROverlay()->HideOverlay(overlay);
        };

    const vr::VRTextureBounds_t topBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    auto applyHudTexture = [&](vr::VROverlayHandle_t overlay, const vr::VRTextureBounds_t& bounds)
        {
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &bounds);
            stageOverlayTextureBind(overlay, &m_VKHUD.m_VRTexture, m_D9HUDSurface);
        };

    auto applyScopeTexture = [&](vr::VROverlayHandle_t overlay) -> bool
        {
            static const vr::VRTextureBounds_t full{ 0.0f, 0.0f, 1.0f, 1.0f };
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &full);
            if (m_ScopeLensOverlayReady.load(std::memory_order_acquire) == 0 ||
                !m_VKScopeLens.m_VRTexture.handle)
            {
                return false;
            }

            return stageOverlayTextureBind(overlay, &m_VKScopeLens.m_VRTexture, m_D9ScopeLensSurface, true);
        };
    auto applyRearMirrorTexture = [&](vr::VROverlayHandle_t overlay)
        {
            vr::VRTextureBounds_t bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
            if (m_RearMirrorFlipHorizontal)
                std::swap(bounds.uMin, bounds.uMax);
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &bounds);
            stageOverlayTextureBind(overlay, &m_VKRearMirror.m_VRTexture, m_D9RearMirrorSurface);
        };

    if (!renderedNewFrame)
    {
        // In mat_queue_mode!=0 (queued/multicore), the render thread can lag behind the submit thread.
        // When that happens, m_RenderedNewFrame may still be false even though we're already in-game.
        // Showing the backbuffer/menu overlay in this state creates an extra compositor layer and can cause
        // severe stutter (and a visible backbuffer rectangle). Instead, just re-submit the last eye textures.
        if (sceneReadyForStaleResubmit)
        {
            // Ensure the menu overlay is not left visible while in-game.
            if (vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
                vr::VROverlay()->HideOverlay(m_MainMenuHandle);

            // During map load we can be in-game before fresh VR render targets are recreated.
            // Do not submit stale menu/blank Vulkan handles here; wait for the first rendered frame.
            const bool texturesReady = m_CreatedVRTextures.load(std::memory_order_acquire);
            if (!texturesReady)
                return;

            submitStereoPair(&m_VKLeftEye.m_VRTexture, leftEyeSubmitBounds,
                &m_VKRightEye.m_VRTexture, rightEyeSubmitBounds);
            if (successfulSubmit)
                m_HasSubmittedSceneFrame = true;

            return;
        }

        if (!RefreshBackBufferTexture(false))
            return;

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        SetOverlayTextureSynchronized(
            vr::VROverlay(),
            m_MainMenuHandle,
            &m_VKBackBuffer.m_VRTexture,
            false);
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        hideHudOverlays();
        vr::VROverlay()->HideOverlay(m_ScopeHandle);
        vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
        UpdateDesktopRearMirrorWindow(false);
        vr::VROverlay()->HideOverlay(m_LeftWristHudHandle);
        vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);

        if (!sceneReadyForStaleResubmit)
        {
            if (!m_HasSubmittedSceneFrame)
            {
                m_Compositor->ClearLastSubmittedFrame();
                m_MenuBlankSubmitted = true;
                m_CompositorNeedsHandoff = false;
                return;
            }

            if (!m_BlankTexture)
                CreateVRTextures();

            if (m_BlankTexture)
            {
                if (!ClearD3D9BlankTextureForMenuSubmit(this, "Submit main menu blank"))
                    return;

                submitStereoPair(&m_VKBlankTexture.m_VRTexture, nullptr,
                    &m_VKBlankTexture.m_VRTexture, nullptr);
                if (successfulSubmit)
                    m_MenuBlankSubmitted = true;
            }
        }

        return;
    }


    vr::VROverlay()->HideOverlay(m_MainMenuHandle);
    const bool focusedVguiHud =
        (m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused()) ||
        (m_Game && m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible());
    const bool wantsTopHudOverlay = focusedVguiHud || IsGameplayHudRequested();
    if (wantsTopHudOverlay)
    {
        applyHudTexture(m_HUDTopHandle, topBounds);
        vr::VROverlay()->ShowOverlay(m_HUDTopHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_HUDTopHandle);
    }
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
        vr::VROverlay()->HideOverlay(overlay);

    // Scope overlay independent of HUD cursor mode
    if (m_ScopeTexture && m_ScopeEnabled)
    {
        const bool scopeOverlayTextureReady = applyScopeTexture(m_ScopeHandle);
        const float alpha = IsScopeActive() ? 1.0f : std::clamp(m_ScopeOverlayIdleAlpha, 0.0f, 1.0f);
        vr::VROverlay()->SetOverlayAlpha(m_ScopeHandle, alpha);

        vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (m_LeftHanded)
            std::swap(leftControllerIndex, rightControllerIndex);
        const vr::TrackedDeviceIndex_t gunControllerIndex = rightControllerIndex;

        // Absolute scope overlay modes:
        // - mouse mode: no tracked gun controller required
        // - third-person: body-anchored scope overlay, not controller-mounted
        const bool useThirdPersonBodyAnchor = m_IsThirdPersonCamera && !m_MouseModeEnabled;
        if (m_MouseModeEnabled || useThirdPersonBodyAnchor)
            UpdateScopeOverlayTransform();

        const bool canShowScope = scopeOverlayTextureReady
            && ShouldRenderScope()
            && (m_MouseModeEnabled || useThirdPersonBodyAnchor || gunControllerIndex != vr::k_unTrackedDeviceIndexInvalid);
        if (canShowScope)
            vr::VROverlay()->ShowOverlay(m_ScopeHandle);
        else
            vr::VROverlay()->HideOverlay(m_ScopeHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_ScopeHandle);
    }

    if (m_RearMirrorTexture && (m_RearMirrorEnabled || m_DesktopRearMirrorWindowEnabled))
    {
        if (m_RearMirrorEnabled)
        {
            applyRearMirrorTexture(m_RearMirrorHandle);
            vr::VROverlay()->SetOverlayAlpha(m_RearMirrorHandle, std::clamp(m_RearMirrorAlpha, 0.0f, 1.0f));

            // Body-anchored rear mirror: update absolute transform every frame.
            UpdateRearMirrorOverlayTransform();
        }

        // Keep the "special warning" enlarge hint bounded even if the mirror RTT pass
        // is temporarily not running (e.g., pop-up mode).
        if (m_RearMirrorSpecialWarningDistance > 0.0f && m_RearMirrorSpecialEnlargeActive)
        {
            if (m_LastRearMirrorSpecialSeenTime.time_since_epoch().count() == 0)
            {
                m_RearMirrorSpecialEnlargeActive = false;
            }
            else
            {
                const auto now = std::chrono::steady_clock::now();
                const float elapsed = std::chrono::duration<float>(now - m_LastRearMirrorSpecialSeenTime).count();
                if (elapsed > m_RearMirrorSpecialEnlargeHoldSeconds)
                    m_RearMirrorSpecialEnlargeActive = false;
            }
        }

        const auto shouldHideRearMirrorDueToAimLine = [&]() -> bool
            {
                if (!m_RearMirrorHideWhenAimLineHits)
                    return false;

                const auto now = std::chrono::steady_clock::now();
                if (now < m_RearMirrorAimLineHideUntil)
                    return true;

                // Build an aim ray consistent with UpdateAimingLaser(), even if the aim line isn't rendered this frame.
                Vector dir = m_RightControllerForward;
                if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
                    dir = m_RightControllerForwardUnforced;
                if (dir.IsZero())
                    dir = m_LastAimDirection;
                if (dir.IsZero())
                    return false;
                VectorNormalize(dir);

                Vector rayStart = m_RightControllerPosAbs;
                // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
                Vector camDelta = GetAimRenderCameraDelta();
                if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
                    rayStart += camDelta;

                rayStart = rayStart + dir * 2.0f;
                Vector rayEnd = rayStart + dir * 8192.0f;

                // Mirror quad in world space (Source units), matching UpdateRearMirrorOverlayTransform().
                Vector fwd = m_HmdForward;
                fwd.z = 0.0f;
                if (VectorNormalize(fwd) == 0.0f)
                    fwd = { 1.0f, 0.0f, 0.0f };

                const Vector up = { 0.0f, 0.0f, 1.0f };
                Vector right = CrossProduct(fwd, up);
                if (VectorNormalize(right) == 0.0f)
                    right = { 0.0f, -1.0f, 0.0f };

                const Vector back = { -fwd.x, -fwd.y, -fwd.z };

                const Vector bodyOrigin =
                    m_HmdPosAbs
                    + (fwd * (m_InventoryBodyOriginOffset.x * m_VRScale))
                    + (right * (m_InventoryBodyOriginOffset.y * m_VRScale))
                    + (up * (m_InventoryBodyOriginOffset.z * m_VRScale));

                const Vector mirrorCenter =
                    bodyOrigin
                    + (fwd * (m_RearMirrorOverlayXOffset * m_VRScale))
                    + (right * (m_RearMirrorOverlayYOffset * m_VRScale))
                    + (up * (m_RearMirrorOverlayZOffset * m_VRScale));

                const float deg2rad = 3.14159265358979323846f / 180.0f;
                const float pitch = m_RearMirrorOverlayAngleOffset.x * deg2rad;
                const float yaw = m_RearMirrorOverlayAngleOffset.y * deg2rad;
                const float roll = m_RearMirrorOverlayAngleOffset.z * deg2rad;

                const float cp = cosf(pitch), sp = sinf(pitch);
                const float cy = cosf(yaw), sy = sinf(yaw);
                const float cr = cosf(roll), sr = sinf(roll);

                const float Rx[3][3] = {
                    {1.0f, 0.0f, 0.0f},
                    {0.0f, cp,   -sp},
                    {0.0f, sp,   cp}
                };
                const float Ry[3][3] = {
                    {cy,   0.0f, sy},
                    {0.0f, 1.0f, 0.0f},
                    {-sy,  0.0f, cy}
                };
                const float Rz[3][3] = {
                    {cr,   -sr,  0.0f},
                    {sr,   cr,   0.0f},
                    {0.0f, 0.0f, 1.0f}
                };

                auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
                    {
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
                    };

                float RyRx[3][3];
                float Roff[3][3];
                mul33(Ry, Rx, RyRx);
                mul33(Rz, RyRx, Roff);

                // Parent yaw-only basis (columns: right, up, back)
                const float B[3][3] = {
                    { right.x, up.x, back.x },
                    { right.y, up.y, back.y },
                    { right.z, up.z, back.z }
                };

                float Rworld[3][3];
                mul33(B, Roff, Rworld);

                Vector axisX = { Rworld[0][0], Rworld[1][0], Rworld[2][0] };
                Vector axisY = { Rworld[0][1], Rworld[1][1], Rworld[2][1] };
                Vector axisZ = { Rworld[0][2], Rworld[1][2], Rworld[2][2] };
                if (axisX.IsZero() || axisY.IsZero() || axisZ.IsZero())
                    return false;
                VectorNormalize(axisX);
                VectorNormalize(axisY);
                VectorNormalize(axisZ);

                float mirrorWidthMeters = (std::max)(0.01f, m_RearMirrorOverlayWidthMeters);
                if (m_RearMirrorSpecialWarningDistance > 0.0f && m_RearMirrorSpecialEnlargeActive)
                    mirrorWidthMeters *= 2.0f;

                const float halfExtent = 0.5f * mirrorWidthMeters * m_VRScale;
                const float pad = 0.01f * m_VRScale; // ~1cm padding to reduce flicker on edges
                const float halfX = halfExtent + pad;
                const float halfY = halfExtent + pad;

                const Vector seg = rayEnd - rayStart;
                const float denom = DotProduct(seg, axisZ);
                if (fabsf(denom) < 1e-6f)
                    return false;

                const float t = DotProduct(mirrorCenter - rayStart, axisZ) / denom;
                if (t < 0.0f || t > 1.0f)
                    return false;

                const Vector hit = rayStart + seg * t;
                const Vector d = hit - mirrorCenter;
                const float lx = DotProduct(d, axisX);
                const float ly = DotProduct(d, axisY);

                if (fabsf(lx) <= halfX && fabsf(ly) <= halfY)
                {
                    m_RearMirrorAimLineHideUntil = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(m_RearMirrorAimLineHideHoldSeconds));
                    return true;
                }

                return false;
            };

        const bool rearMirrorContentVisible = ShouldRenderRearMirror();
        const bool showVrRearMirror = m_RearMirrorEnabled && rearMirrorContentVisible && !shouldHideRearMirrorDueToAimLine();
        if (showVrRearMirror)
            vr::VROverlay()->ShowOverlay(m_RearMirrorHandle);
        else
            vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
        UpdateDesktopRearMirrorWindow(m_DesktopRearMirrorWindowEnabled && rearMirrorContentVisible);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
        UpdateDesktopRearMirrorWindow(false);
    }

    // Upload and bind dynamic intent/hand HUD textures on every fresh frame.
    UpdateHandHudOverlays(&pendingOverlayTextureBinds);

    submitStereoPair(&m_VKLeftEye.m_VRTexture, leftEyeSubmitBounds,
        &m_VKRightEye.m_VRTexture, rightEyeSubmitBounds);
    if (successfulSubmit)
        m_HasSubmittedSceneFrame = true;

    if (!queued || successfulSubmit || frameHandled)
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        if (!queued || m_RenderCompletedFrameId.load(std::memory_order_acquire) == submitSnapshotFrameId)
            m_RenderedNewFrame.store(false, std::memory_order_release);
    }
}

void VR::LogCompositorError(const char* action, vr::EVRCompositorError error)
{
    if (error == vr::VRCompositorError_None || !action)
        return;

    constexpr auto kLogCooldown = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();

    if (now - m_LastCompositorErrorLog < kLogCooldown)
        return;

    m_LastCompositorErrorLog = now;
}


void VR::GetPoseData(vr::TrackedDevicePose_t& poseRaw, TrackedDevicePoseData& poseOut)
{
    if (poseRaw.bPoseIsValid)
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        Vector vel;
        QAngle ang;
        QAngle angvel;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(mat.m[1][2]) * (180.0 / 3.141592654);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
        vel.x = -poseRaw.vVelocity.v[2];
        vel.y = -poseRaw.vVelocity.v[0];
        vel.z = poseRaw.vVelocity.v[1];
        angvel.x = -poseRaw.vAngularVelocity.v[2] * (180.0 / 3.141592654);
        angvel.y = -poseRaw.vAngularVelocity.v[0] * (180.0 / 3.141592654);
        angvel.z = poseRaw.vAngularVelocity.v[1] * (180.0 / 3.141592654);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceVel = vel;
        poseOut.TrackedDeviceAng = ang;
        poseOut.TrackedDeviceAngVel = angvel;
    }
}

void VR::UpdateRearMirrorOverlayTransform()
{
    if (!m_RearMirrorEnabled || m_RearMirrorHandle == vr::k_ulOverlayHandleInvalid)
        return;

    // We place the rear mirror in tracking space (meters), anchored to the same "body origin"
    // used by the inventory system: InventoryBodyOriginOffset is (forward,right,up) in body space.
    const vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmdPose.bPoseIsValid)
        return;

    const vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    const Vector hmdPos = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };

    const Vector up = { 0.0f, 1.0f, 0.0f }; // OpenVR tracking space: +Y is up

    // Yaw-only forward (flattened to the floor plane).
    Vector fwd = { -hmdMat.m[0][2], 0.0f, -hmdMat.m[2][2] };
    if (VectorNormalize(fwd) == 0.0f)
        fwd = { 0.0f, 0.0f, -1.0f };

    Vector right = CrossProduct(fwd, up);
    if (VectorNormalize(right) == 0.0f)
        right = { 1.0f, 0.0f, 0.0f };

    // "Back" axis for a right-handed basis (OpenVR commonly treats +Z as "back").
    const Vector back = { -fwd.x, -fwd.y, -fwd.z };

    // Body origin in tracking space (meters)
    const Vector bodyOrigin =
        hmdPos
        + (fwd * m_InventoryBodyOriginOffset.x)
        + (right * m_InventoryBodyOriginOffset.y)
        + (up * m_InventoryBodyOriginOffset.z);

    // Rear mirror overlay position relative to that body origin (meters)
    const Vector mirrorPos =
        bodyOrigin
        + (fwd * m_RearMirrorOverlayXOffset)
        + (right * m_RearMirrorOverlayYOffset)
        + (up * m_RearMirrorOverlayZOffset);

    // Build a yaw-only parent rotation from (right, up, back) and then apply the user angle offset.
    const float deg2rad = 3.14159265358979323846f / 180.0f;

    const float pitch = m_RearMirrorOverlayAngleOffset.x * deg2rad;
    const float yaw = m_RearMirrorOverlayAngleOffset.y * deg2rad;
    const float roll = m_RearMirrorOverlayAngleOffset.z * deg2rad;

    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cr = cosf(roll), sr = sinf(roll);

    const float Rx[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, cp,   -sp},
        {0.0f, sp,   cp}
    };
    const float Ry[3][3] = {
        {cy,   0.0f, sy},
        {0.0f, 1.0f, 0.0f},
        {-sy,  0.0f, cy}
    };
    const float Rz[3][3] = {
        {cr,   -sr,  0.0f},
        {sr,   cr,   0.0f},
        {0.0f, 0.0f, 1.0f}
    };

    auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        };

    float RyRx[3][3];
    float Roff[3][3];
    mul33(Ry, Rx, RyRx);
    mul33(Rz, RyRx, Roff);

    // Parent yaw-only basis (columns: right, up, back)
    const float B[3][3] = {
        { right.x, up.x, back.x },
        { right.y, up.y, back.y },
        { right.z, up.z, back.z }
    };

    float Rworld[3][3];
    mul33(B, Roff, Rworld);

    vr::HmdMatrix34_t mirrorAbs = {
        Rworld[0][0], Rworld[0][1], Rworld[0][2], mirrorPos.x,
        Rworld[1][0], Rworld[1][1], Rworld[1][2], mirrorPos.y,
        Rworld[2][0], Rworld[2][1], Rworld[2][2], mirrorPos.z
    };

    const vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();
    vr::VROverlay()->SetOverlayTransformAbsolute(m_RearMirrorHandle, trackingOrigin, &mirrorAbs);
    float mirrorWidth = (std::max)(0.01f, m_RearMirrorOverlayWidthMeters);
    if (m_RearMirrorSpecialWarningDistance > 0.0f && m_RearMirrorSpecialEnlargeActive)
        mirrorWidth *= 2.0f;
    vr::VROverlay()->SetOverlayWidthInMeters(m_RearMirrorHandle, mirrorWidth);
}

void VR::UpdateScopeOverlayTransform()
{
    if (!m_ScopeEnabled)
        return;

    // Default behavior (controllers available): scope overlay is tracked-device-relative and updated in RepositionOverlays().
    // Absolute transform is used in:
    // - mouse mode (no tracked gun controller),
    // - third-person mode (scope overlay should not be controller-mounted).
    const bool useThirdPersonBodyAnchor = m_IsThirdPersonCamera && !m_MouseModeEnabled;
    const bool useThirdPersonEyeAnchor = useThirdPersonBodyAnchor && m_ThirdPersonFrontViewEnabled;
    if ((!m_MouseModeEnabled && !useThirdPersonBodyAnchor) || m_ScopeHandle == vr::k_ulOverlayHandleInvalid)
        return;

    const float scopeWidth = (std::max)(0.01f, m_ScopeOverlayWidthMeters);

    // IMPORTANT:
    // - OpenVR absolute overlay transforms are in tracking-space *meters*.
    // - Most of our in-game positions (m_HmdPosAbs, viewmodel anchors, etc.) are in Source units.
    // If we feed Source units into SetOverlayTransformAbsolute, the overlay ends up kilometers away.
    // So for mouse mode we build the transform directly from the OpenVR HMD tracking pose.

    const vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmdPose.bPoseIsValid)
        return;

    const vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    const Vector hmdPos = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] }; // meters

    // Tracking-space basis (meters): columns of the 3x4 matrix
    // right = +X, up = +Y, back = +Z (OpenVR commonly treats -Z as forward)
    Vector parentRight = { hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0] };
    Vector parentUp = { hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1] };
    Vector parentBack = { hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2] };
    if (VectorNormalize(parentRight) == 0.0f || VectorNormalize(parentUp) == 0.0f || VectorNormalize(parentBack) == 0.0f)
        return;

    Vector overlayPos = hmdPos;
    if (useThirdPersonBodyAnchor)
    {
        if (useThirdPersonEyeAnchor)
        {
            // Third-person front-view mode: bind scope overlay to eye/HMD.
            Vector fwd = { -parentBack.x, -parentBack.y, -parentBack.z };
            if (VectorNormalize(fwd) == 0.0f)
                fwd = { 0.0f, 0.0f, -1.0f };

            overlayPos = hmdPos
                + (fwd * m_ThirdPersonScopeOverlayOffset.x)
                + (parentRight * m_ThirdPersonScopeOverlayOffset.y)
                + (parentUp * m_ThirdPersonScopeOverlayOffset.z);
        }
        else
        {
            // Third-person: anchor scope overlay near the player body, not the gun hand.
            const Vector up = { 0.0f, 1.0f, 0.0f }; // OpenVR tracking space: +Y is up
            Vector fwd = { -hmdMat.m[0][2], 0.0f, -hmdMat.m[2][2] }; // yaw-only forward
            if (VectorNormalize(fwd) == 0.0f)
                fwd = { 0.0f, 0.0f, -1.0f };
            Vector right = CrossProduct(fwd, up);
            if (VectorNormalize(right) == 0.0f)
                right = { 1.0f, 0.0f, 0.0f };
            const Vector back = { -fwd.x, -fwd.y, -fwd.z };

            parentRight = right;
            parentUp = up;
            parentBack = back;

            const Vector bodyOrigin =
                hmdPos
                + (fwd * m_InventoryBodyOriginOffset.x)
                + (right * m_InventoryBodyOriginOffset.y)
                + (up * m_InventoryBodyOriginOffset.z);

            overlayPos = bodyOrigin
                + (fwd * m_ThirdPersonScopeOverlayOffset.x)
                + (right * m_ThirdPersonScopeOverlayOffset.y)
                + (up * m_ThirdPersonScopeOverlayOffset.z);
        }
    }
    else
    {
        // Mouse mode: HMD-anchored offset (meters). If not set, fall back to existing scope offsets.
        if (!m_MouseModeScopeOverlayOffset.IsZero())
        {
            overlayPos += parentRight * m_MouseModeScopeOverlayOffset.x;
            overlayPos += parentUp * m_MouseModeScopeOverlayOffset.y;
            overlayPos += parentBack * m_MouseModeScopeOverlayOffset.z;
        }
        else
        {
            overlayPos += parentRight * m_ScopeOverlayXOffset;
            overlayPos += parentUp * m_ScopeOverlayYOffset;
            overlayPos += parentBack * m_ScopeOverlayZOffset;
        }
    }

    const QAngle a = ((m_MouseModeEnabled && m_MouseModeScopeOverlayAngleOffsetSet)
        ? m_MouseModeScopeOverlayAngleOffset
        : m_ScopeOverlayAngleOffset);
    const float cx = std::cos(DEG2RAD(a.x)), sx = std::sin(DEG2RAD(a.x));
    const float cy = std::cos(DEG2RAD(a.y)), sy = std::sin(DEG2RAD(a.y));
    const float cz = std::cos(DEG2RAD(a.z)), sz = std::sin(DEG2RAD(a.z));

    const float Roff00 = cy * cz;
    const float Roff01 = -cy * sz;
    const float Roff02 = sy;
    const float Roff10 = sx * sy * cz + cx * sz;
    const float Roff11 = -sx * sy * sz + cx * cz;
    const float Roff12 = -sx * cy;
    const float Roff20 = -cx * sy * cz + sx * sz;
    const float Roff21 = cx * sy * sz + sx * cz;
    const float Roff22 = cx * cy;

    // World basis = ParentBasis * OffsetRotation.
    const float B00 = parentRight.x, B01 = parentUp.x, B02 = parentBack.x;
    const float B10 = parentRight.y, B11 = parentUp.y, B12 = parentBack.y;
    const float B20 = parentRight.z, B21 = parentUp.z, B22 = parentBack.z;

    const float R00 = B00 * Roff00 + B01 * Roff10 + B02 * Roff20;
    const float R01 = B00 * Roff01 + B01 * Roff11 + B02 * Roff21;
    const float R02 = B00 * Roff02 + B01 * Roff12 + B02 * Roff22;
    const float R10 = B10 * Roff00 + B11 * Roff10 + B12 * Roff20;
    const float R11 = B10 * Roff01 + B11 * Roff11 + B12 * Roff21;
    const float R12 = B10 * Roff02 + B11 * Roff12 + B12 * Roff22;
    const float R20 = B20 * Roff00 + B21 * Roff10 + B22 * Roff20;
    const float R21 = B20 * Roff01 + B21 * Roff11 + B22 * Roff21;
    const float R22 = B20 * Roff02 + B21 * Roff12 + B22 * Roff22;

    vr::HmdMatrix34_t scopeAbs = {
        R00, R01, R02, overlayPos.x,
        R10, R11, R12, overlayPos.y,
        R20, R21, R22, overlayPos.z
    };
    const vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();
    vr::VROverlay()->SetOverlayTransformAbsolute(m_ScopeHandle, trackingOrigin, &scopeAbs);
    vr::VROverlay()->SetOverlayWidthInMeters(m_ScopeHandle, scopeWidth);
}

bool VR::ShouldRenderRearMirror() const
{
    if (!m_RearMirrorEnabled && !m_DesktopRearMirrorWindowEnabled)
        return false;

    // Default behavior: always render/show when enabled.
    if (!m_RearMirrorShowOnlyOnSpecialWarning)
        return true;

    // Pop-up mode: only render/show for a short time after a special-infected warning.
    if (m_RearMirrorSpecialShowHoldSeconds <= 0.0f)
        return false;

    const auto now = std::chrono::steady_clock::now();

    bool alertActive = false;
    if (m_LastRearMirrorAlertTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastRearMirrorAlertTime).count();
        alertActive = (elapsed <= m_RearMirrorSpecialShowHoldSeconds);
    }

    // Also keep it visible if the mirror pass recently saw special-infected arrows (enlarge hint).
    bool hintActive = false;
    if (m_LastRearMirrorSpecialSeenTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastRearMirrorSpecialSeenTime).count();
        hintActive = (elapsed <= m_RearMirrorSpecialEnlargeHoldSeconds);
    }

    return alertActive || hintActive;
}

void VR::NotifyRearMirrorSpecialWarning()
{
    m_LastRearMirrorAlertTime = std::chrono::steady_clock::now();
}

bool VR::ShouldUpdateScopeRTT()
{
    // Keep the scope RTT pacing aligned with the HMD refresh rate.
    // This avoids exposing another manual config knob while still preventing multiple scope updates per HMD frame.
    float effectiveMaxHz = GetHmdDisplayFrequencyHz();
    if (!std::isfinite(effectiveMaxHz) || effectiveMaxHz <= 1.0f)
        effectiveMaxHz = 90.0f;
    effectiveMaxHz = std::clamp(effectiveMaxHz, 1.0f, 240.0f);

    return !ShouldThrottle(m_LastScopeRTTRenderTime, effectiveMaxHz);
}
bool VR::ShouldUpdateRearMirrorRTT()
{
    // The rear mirror is a full extra scene render. Throttling this can significantly reduce CPU spikes.
    return !ShouldThrottle(m_LastRearMirrorRTTRenderTime, m_RearMirrorRTTMaxHz);
}
void VR::RepositionOverlays()
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
    Vector hmdForwardYaw = { -hmdMat.m[0][2], 0.0f, -hmdMat.m[2][2] };
    Vector hmdForwardFull = { -hmdMat.m[0][2], -hmdMat.m[1][2], -hmdMat.m[2][2] };
    Vector hmdRightFull = { hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0] };
    Vector hmdUpFull = { hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1] };

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    vr::HmdMatrix34_t menuTransform =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f
    };

    vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();

    // Reposition main menu overlay
    if (!RefreshBackBufferTexture(false))
        return;

    float renderWidth = static_cast<float>(m_VKBackBuffer.m_VulkanData.m_nWidth);
    float renderHeight = static_cast<float>(m_VKBackBuffer.m_VulkanData.m_nHeight);
    if (renderWidth <= 0.0f || renderHeight <= 0.0f)
        return;

    float widthRatio = static_cast<float>(windowWidth) / renderWidth;
    float heightRatio = static_cast<float>(windowHeight) / renderHeight;
    menuTransform.m[0][0] *= widthRatio;
    menuTransform.m[1][1] *= heightRatio;

    hmdForwardYaw[1] = 0.0f;
    VectorNormalize(hmdForwardYaw);

    Vector menuDistance = hmdForwardYaw * 3;
    Vector menuNewPos = menuDistance + hmdPosition;

    menuTransform.m[0][3] = menuNewPos.x;
    menuTransform.m[1][3] = menuNewPos.y - 0.25;
    menuTransform.m[2][3] = menuNewPos.z;

    float xScale = menuTransform.m[0][0];
    float hmdRotationDegrees = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);

    menuTransform.m[0][0] *= cos(hmdRotationDegrees);
    menuTransform.m[0][2] = sin(hmdRotationDegrees);
    menuTransform.m[2][0] = -sin(hmdRotationDegrees) * xScale;
    menuTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_MainMenuHandle, 1.5 * (1.0 / heightRatio));

    auto buildFacingTransform = [&](const Vector& position)
        {
            vr::HmdMatrix34_t transform =
            {
                1.0f, 0.0f, 0.0f, position.x,
                0.0f, 1.0f, 0.0f, position.y,
                0.0f, 0.0f, 1.0f, position.z
            };

            float cosYaw = cos(hmdRotationDegrees);
            float sinYaw = sin(hmdRotationDegrees);
            transform.m[0][0] *= cosYaw;
            transform.m[0][2] = sinYaw;
            transform.m[2][0] = -sinYaw;
            transform.m[2][2] *= cosYaw;

            return transform;
        };

    auto buildHmdFollowTransform = [&](const Vector& position)
        {
            vr::HmdMatrix34_t transform =
            {
                hmdMat.m[0][0], hmdMat.m[0][1], hmdMat.m[0][2], position.x,
                hmdMat.m[1][0], hmdMat.m[1][1], hmdMat.m[1][2], position.y,
                hmdMat.m[2][0], hmdMat.m[2][1], hmdMat.m[2][2], position.z
            };
            return transform;
        };

    const bool hudFollowHmdMovement = m_HudFollowHmdMovement;
    Vector hudForward = hudFollowHmdMovement ? hmdForwardFull : hmdForwardYaw;
    if (VectorNormalize(hudForward) == 0.0f)
        hudForward = hmdForwardYaw;

    const float cosYaw = cosf(hmdRotationDegrees);
    const float sinYaw = sinf(hmdRotationDegrees);
    Vector hudRight = hudFollowHmdMovement ? hmdRightFull : Vector(cosYaw, 0.0f, -sinYaw);
    Vector hudUp = hudFollowHmdMovement ? hmdUpFull : Vector(0.0f, 1.0f, 0.0f);
    VectorNormalize(hudRight);
    VectorNormalize(hudUp);

    // Reposition HUD overlays
    Vector hudDistance = hudForward * (m_HudDistance + m_FixedHudDistanceOffset);
    Vector hudNewPos = hudDistance + hmdPosition;
    hudNewPos += hudRight * m_FixedHudXOffset;
    hudNewPos += hudUp * (-0.25f + m_FixedHudYOffset);

    vr::HmdMatrix34_t hudTopTransform = hudFollowHmdMovement ? buildHmdFollowTransform(hudNewPos) : buildFacingTransform(hudNewPos);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDTopHandle, trackingOrigin, &hudTopTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDTopHandle, m_HudSize);
    vr::VROverlay()->SetOverlayCurvature(m_HUDTopHandle, (std::max)(0.0f, m_TopHudCurvature));

    // Place the intent-sense panel in the upper-right area of the same in-game HUD plane.
    if (m_SpecialInfectedIntentSenseHudHandle != vr::k_ulOverlayHandleInvalid)
    {
        const float hudAspect = (windowWidth > 0)
            ? ((std::max)(1.0f, static_cast<float>(windowHeight)) / (std::max)(1.0f, static_cast<float>(windowWidth)))
            : 0.5625f;
        const float panelWidth = std::clamp(m_SpecialInfectedIntentSenseHudWidthMeters, 0.10f, 1.50f);
        const float panelHeight = panelWidth * ((std::max)(1, m_SpecialInfectedIntentSenseHudTexH) / (float)(std::max)(1, m_SpecialInfectedIntentSenseHudTexW));

        const float hudHeightMeters = m_HudSize * hudAspect;
        const float xOff = (m_HudSize * 0.5f) - (panelWidth * 0.5f) - m_SpecialInfectedIntentSenseHudMarginXMeters;
        const float yOff = (hudHeightMeters * 0.5f) - (panelHeight * 0.5f) - m_SpecialInfectedIntentSenseHudMarginYMeters;
        Vector panelPos = hudNewPos + hudRight * xOff + hudUp * yOff;
        vr::HmdMatrix34_t intentHudTransform = hudFollowHmdMovement ? buildHmdFollowTransform(panelPos) : buildFacingTransform(panelPos);
        vr::VROverlay()->SetOverlayTransformAbsolute(m_SpecialInfectedIntentSenseHudHandle, trackingOrigin, &intentHudTransform);
        vr::VROverlay()->SetOverlayWidthInMeters(m_SpecialInfectedIntentSenseHudHandle, panelWidth);
        // Intent-sense HUD is an independent flat overlay. Do not inherit the main game HUD curvature.
        vr::VROverlay()->SetOverlayCurvature(m_SpecialInfectedIntentSenseHudHandle, 0.0f);
    }

    for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
    {
        vr::VROverlay()->HideOverlay(m_HUDBottomHandles[i]);
    }

    // Scope overlay placement:
    // - First-person controller mode: tracked-device-relative to gun hand.
    // - Mouse mode / third-person: absolute body/HMD-anchored transform.
    {
        const bool useThirdPersonBodyAnchor = m_IsThirdPersonCamera && !m_MouseModeEnabled;
        if (m_MouseModeEnabled || useThirdPersonBodyAnchor)
        {
            UpdateScopeOverlayTransform();
        }
        else
        {
            vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
            if (m_LeftHanded)
                std::swap(leftControllerIndex, rightControllerIndex);

            const vr::TrackedDeviceIndex_t gunControllerIndex = rightControllerIndex;
            if (gunControllerIndex != vr::k_unTrackedDeviceIndexInvalid)
            {
                const float deg2rad = 3.14159265358979323846f / 180.0f;

                auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
                    {
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
                    };

                const QAngle scopeAngle = (m_MouseModeEnabled && m_MouseModeScopeOverlayAngleOffsetSet)
                    ? m_MouseModeScopeOverlayAngleOffset
                    : m_ScopeOverlayAngleOffset;
                const float pitch = scopeAngle.x * deg2rad;
                const float yaw = scopeAngle.y * deg2rad;
                const float roll = scopeAngle.z * deg2rad;

                const float cp = cosf(pitch), sp = sinf(pitch);
                const float cy = cosf(yaw), sy = sinf(yaw);
                const float cr = cosf(roll), sr = sinf(roll);

                const float Rx[3][3] = {
                    {1.0f, 0.0f, 0.0f},
                    {0.0f, cp,   -sp},
                    {0.0f, sp,   cp}
                };
                const float Ry[3][3] = {
                    {cy,   0.0f, sy},
                    {0.0f, 1.0f, 0.0f},
                    {-sy,  0.0f, cy}
                };
                const float Rz[3][3] = {
                    {cr,   -sr,  0.0f},
                    {sr,   cr,   0.0f},
                    {0.0f, 0.0f, 1.0f}
                };

                float RyRx[3][3];
                float R[3][3];
                mul33(Ry, Rx, RyRx);
                mul33(Rz, RyRx, R);

                vr::HmdMatrix34_t scopeRelative = {
                    R[0][0], R[0][1], R[0][2], m_ScopeOverlayXOffset,
                    R[1][0], R[1][1], R[1][2], m_ScopeOverlayYOffset,
                    R[2][0], R[2][1], R[2][2], m_ScopeOverlayZOffset
                };

                vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_ScopeHandle, gunControllerIndex, &scopeRelative);
                vr::VROverlay()->SetOverlayWidthInMeters(m_ScopeHandle, (std::max)(0.01f, m_ScopeOverlayWidthMeters));
            }
        }
    }

    // Rear mirror overlay is now body-anchored (InventoryBodyOriginOffset); keep it updated per-frame.
    if (m_RearMirrorEnabled)
    {
        UpdateRearMirrorOverlayTransform();
    }
}
