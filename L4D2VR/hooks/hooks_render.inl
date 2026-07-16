ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
	ITexture* result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, CViewSetup& hudViewSetup, int nClearFlags, int whatToDraw)
{
	// Source can call RenderView recursively while rendering the world, most visibly for water
	// reflection/refraction render targets. The VR hook may only hijack the outer scene call.
	// Nested calls must go straight through, otherwise water RTs can be filled with the VR eye
	// scene/HUD and appear as a second full scene on the water surface in mat_queue_mode 2.
	static thread_local int s_originalRenderViewDepth = 0;
	static thread_local int s_vrEyeRenderPass = 0;
	static thread_local bool s_vrSharedCenterValid = false;
	static thread_local Vector s_vrSharedCenterOrigin{};
	static thread_local Vector s_vrSharedEyeOrigin{};

	auto rtNameContainsI = [](const char* haystack, const char* needle) -> bool
		{
			if (!haystack || !needle || !*needle)
				return false;

			const size_t needleLen = std::strlen(needle);
			for (const char* p = haystack; *p; ++p)
			{
				if (_strnicmp(p, needle, needleLen) == 0)
					return true;
			}
			return false;
		};

	auto isWaterReflectionRenderTarget = [&](ITexture* texture) -> bool
		{
			if (!texture)
				return false;

			const char* name = DebugTextureName(texture);
			if (!name || !*name || name[0] == '<')
				return false;

			// L4D2/Source water uses offscreen reflection/refraction RTs. In queued mode these
			// can enter RenderView as separate calls, not only recursive calls. Do not treat
			// them as the outer VR scene.
			return rtNameContainsI(name, "water") ||
				rtNameContainsI(name, "reflect") ||
				rtNameContainsI(name, "refract");
		};

	auto classifySharedStereoRenderTarget = [&](ITexture* texture) -> const char*
		{
			if (isWaterReflectionRenderTarget(texture))
				return "water";
			return nullptr;
		};

	if (s_originalRenderViewDepth > 0)
	{
		if (m_VR && m_VR->m_RenderPipelineDebugLog && m_Game && m_Game->m_MaterialSystem)
		{
			static std::atomic<int> s_nestedRenderViewProbeBudget{ 160 };
			const int probeIndex = s_nestedRenderViewProbeBudget.fetch_sub(1, std::memory_order_acq_rel);
			if (probeIndex > 0)
			{
				IMatRenderContext* probeContext = m_Game->m_MaterialSystem->GetRenderContext();
				ITexture* probeRt = DebugCurrentRenderTarget(probeContext);
				int rtMapW = 0;
				int rtMapH = 0;
				int rtActualW = 0;
				int rtActualH = 0;
				DebugTextureFullSize(probeRt, rtMapW, rtMapH, rtActualW, rtActualH);
				int vpX = 0;
				int vpY = 0;
				int vpW = 0;
				int vpH = 0;
				const bool haveVp = DebugGetViewport(probeContext, vpX, vpY, vpW, vpH);
				const int nestedQueueMode = m_Game ? m_Game->GetMatQueueMode() : 0;
				Game::logMsg("[VR][RenderView][ProbeNested] left=%d depth=%d eye=%d q=%d tid=%lu rt=%s(map=%dx%d actual=%dx%d) setup=%dx%d unscaled=%dx%d hud=%dx%d vp=%d,%d %dx%d haveVp=%d clear=0x%X draw=0x%X",
					probeIndex,
					s_originalRenderViewDepth,
					s_vrEyeRenderPass,
					nestedQueueMode,
					GetCurrentThreadId(),
					DebugTextureName(probeRt), rtMapW, rtMapH, rtActualW, rtActualH,
					setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
					hudViewSetup.width, hudViewSetup.height,
					vpX, vpY, vpW, vpH, haveVp ? 1 : 0,
					nClearFlags, whatToDraw);
			}
		}

		if (s_vrEyeRenderPass != 0 && s_vrSharedCenterValid && m_Game && m_Game->m_MaterialSystem)
		{
			IMatRenderContext* nestedContext = m_Game->m_MaterialSystem->GetRenderContext();
			ITexture* nestedRt = DebugCurrentRenderTarget(nestedContext);
			const char* sharedRtReason = classifySharedStereoRenderTarget(nestedRt);
			if (sharedRtReason != nullptr)
			{
				const Vector eyeToCenter = s_vrSharedCenterOrigin - s_vrSharedEyeOrigin;
				CViewSetup sharedView = setup;
				CViewSetup sharedHud = hudViewSetup;
				sharedView.origin += eyeToCenter;
				sharedHud.origin += eyeToCenter;

				if (m_VR->m_RenderPipelineDebugLog)
				{
					static thread_local std::chrono::steady_clock::time_point s_lastSharedCenterLog{};
					if (!ShouldThrottleLog(s_lastSharedCenterLog, m_VR->m_RenderPipelineDebugLogHz))
					{
						Game::logMsg("[VR][RenderView][SharedRTCenter] reason=%s eye=%d tid=%lu rt=%s delta=(%.3f %.3f %.3f) setup=%dx%d clear=0x%X draw=0x%X",
							sharedRtReason,
							s_vrEyeRenderPass,
							GetCurrentThreadId(),
							DebugTextureName(nestedRt),
							eyeToCenter.x, eyeToCenter.y, eyeToCenter.z,
							setup.width, setup.height,
							nClearFlags, whatToDraw);
					}
				}

				return hkRenderView.fOriginal(ecx, sharedView, sharedHud, nClearFlags, whatToDraw);
			}
		}

		return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}

	auto callOriginalRenderView = [&](CViewSetup& view, CViewSetup& hud, int clearFlags, int drawFlags)
		{
			struct OriginalRenderViewScope
			{
				int& depth;
				explicit OriginalRenderViewScope(int& d) : depth(d) { ++depth; }
				~OriginalRenderViewScope() { --depth; }
			};

			OriginalRenderViewScope scope(s_originalRenderViewDepth);
			hkRenderView.fOriginal(ecx, view, hud, clearFlags, drawFlags);
		};

	static EngineThirdPersonCamSmoother s_engineTpCam;

	if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
		m_VR->CreateVRTextures();

	// Scope / rear-mirror RTTs may be created lazily (see LazyScopeRearMirrorRTT).
	// Ensure they're available before any offscreen passes try to render into them.
	m_VR->EnsureOpticsRTTTextures();

	if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) ||
		!m_VR->m_LeftEyeTexture ||
		!m_VR->m_RightEyeTexture ||
		m_VR->m_RenderWidth == 0 ||
		m_VR->m_RenderHeight == 0)
	{
		if (m_VR->m_RenderPipelineDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_lastMissingEyeRtLog{};
			if (!ShouldThrottleLog(s_lastMissingEyeRtLog, m_VR->m_RenderPipelineDebugLogHz))
			{
				Game::logMsg("[VR][RenderView][PassThrough] reason=missing-eye-rt tid=%lu created=%d left=%d right=%d eye=%ux%u setup=%dx%d clear=0x%X draw=0x%X",
					GetCurrentThreadId(),
					m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) ? 1 : 0,
					m_VR->m_LeftEyeTexture ? 1 : 0,
					m_VR->m_RightEyeTexture ? 1 : 0,
					m_VR->m_RenderWidth,
					m_VR->m_RenderHeight,
					setup.width, setup.height,
					nClearFlags, whatToDraw);
			}
		}

		callOriginalRenderView(setup, hudViewSetup, nClearFlags, whatToDraw);
		return;
	}

	IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
	if (!rndrContext)
	{
		m_VR->HandleMissingRenderContext("Hooks::dRenderView");
		callOriginalRenderView(setup, hudViewSetup, nClearFlags, whatToDraw);
		return;
	}
	struct RenderContextStateGuard
	{
		IMatRenderContext* ctx = nullptr;
		ITexture* rt = nullptr;
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
		bool hasViewport = false;
		bool restored = false;
		RenderContextStateGuard(IMatRenderContext* renderContext)
			: ctx(renderContext)
		{
			if (!ctx)
				return;
			rt = ctx->GetRenderTarget();
			if (hkGetViewport.fOriginal)
			{
				hkGetViewport.fOriginal(ctx, x, y, w, h);
				hasViewport = true;
			}
		}
		void Restore()
		{
			if (!ctx || restored)
				return;
			ctx->SetRenderTarget(rt);
			if (hasViewport && hkViewport.fOriginal)
				hkViewport.fOriginal(ctx, x, y, w, h);
			restored = true;
		}
		~RenderContextStateGuard() { Restore(); }
	};
	RenderContextStateGuard renderContextStateGuard(rndrContext);
	const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
	// Normal queued rendering uses DXVK's lightweight activity gate and takes an
	// exclusive lock only for Present/plugin transactions. ReShade injects work
	// outside those known windows, so it retains the conservative all-call lock.
	L4D2VR_D3D9_SetForceDeviceLock(m_VR->m_ReShadeVRCompat ? 1 : 0);
	struct SourceRenderQueueBuildScope
	{
		VR* vr = nullptr;
		bool active = false;
		SourceRenderQueueBuildScope(VR* owner, bool enabled)
			: vr(owner), active(enabled && owner != nullptr)
		{
			if (active)
			{
				std::lock_guard<std::recursive_mutex> consumerGate(vr->m_SourceRenderConsumerGate);
				vr->m_SourceRenderQueueBuildCount.fetch_add(1, std::memory_order_acq_rel);
			}
		}
		~SourceRenderQueueBuildScope()
		{
			if (active)
			{
				std::lock_guard<std::recursive_mutex> consumerGate(vr->m_SourceRenderConsumerGate);
				vr->m_SourceRenderQueueBuildCount.fetch_sub(1, std::memory_order_acq_rel);
			}
		}
	};
	// Enter the producer window before any queued pass-through branch. Even a
	// non-drawable-window RenderView can append commands that outlive this call.
	SourceRenderQueueBuildScope sourceRenderQueueBuildScope(m_VR, queueMode != 0);
	const bool inGameForWindowState =
		m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
	const bool vrWindowDrawable = DebugIsCurrentProcessMainWindowDrawable();
	if (queueMode != 0 && inGameForWindowState && !vrWindowDrawable)
	{
		if (m_VR->m_RenderPipelineDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_lastInactiveWindowPassLog{};
			if (!ShouldThrottleLog(s_lastInactiveWindowPassLog, m_VR->m_RenderPipelineDebugLogHz))
			{
				Game::logMsg("[VR][RenderView][PassThrough] reason=window-not-drawable tid=%lu drawable=%d rt=%s setup=%dx%d clear=0x%X draw=0x%X",
					GetCurrentThreadId(),
					vrWindowDrawable ? 1 : 0,
					DebugTextureName(renderContextStateGuard.rt),
					setup.width, setup.height,
					nClearFlags, whatToDraw);
			}
		}

		m_VR->m_RenderedNewFrame.store(false, std::memory_order_release);
		callOriginalRenderView(setup, hudViewSetup, nClearFlags, whatToDraw);
		renderContextStateGuard.Restore();
		if (!m_VR->QueueSourceRenderOwnershipReleaseMarker(rndrContext))
		{
			m_VR->m_SourceRenderQueueOwnershipUncertain.store(true, std::memory_order_release);
			m_VR->m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
			if (m_VR->m_RenderPipelineDebugLog)
				Game::logMsg("[VR][Queued][InactiveWindowOwnershipMarkerMissing] tid=%lu", GetCurrentThreadId());
		}
		return;
	}

	auto isVRManagedRenderTarget = [&](ITexture* texture) -> bool
		{
			if (!texture)
				return false;

			return texture == m_VR->m_LeftEyeTexture ||
				texture == m_VR->m_RightEyeTexture ||
				texture == m_VR->m_LeftEyeSubmitTexture ||
				texture == m_VR->m_RightEyeSubmitTexture ||
				texture == m_VR->m_HUDTexture ||
				texture == m_VR->m_ScopeTexture ||
				texture == m_VR->m_RearMirrorTexture ||
				texture == m_VR->m_DesktopMirrorTexture ||
				texture == m_VR->m_BlankTexture;
		};

	auto viewSizeMatches = [](int w, int h, int targetW, int targetH) -> bool
		{
			if (w <= 0 || h <= 0 || targetW <= 0 || targetH <= 0)
				return false;

			return std::abs(w - targetW) <= 4 && std::abs(h - targetH) <= 4;
		};

	auto isQueuedOffscreenRenderView = [&](const char*& reason) -> bool
		{
			reason = nullptr;
			if (queueMode == 0)
				return false;

			ITexture* texture = renderContextStateGuard.rt;
			if (texture && isVRManagedRenderTarget(texture))
				return false;

			int backBufferW = 0;
			int backBufferH = 0;
			int windowW = 0;
			int windowH = 0;
			DebugBackBufferDimensions(m_Game ? m_Game->m_MaterialSystem : nullptr, backBufferW, backBufferH);
			DebugRenderContextWindowSize(rndrContext, windowW, windowH);
			const bool haveMainViewSize =
				(backBufferW > 0 && backBufferH > 0) ||
				(windowW > 0 && windowH > 0);
			if (!haveMainViewSize)
				return false;

			auto isMainViewSized = [&](int w, int h) -> bool
				{
					return viewSizeMatches(w, h, backBufferW, backBufferH) ||
						viewSizeMatches(w, h, windowW, windowH);
				};

			// A null MaterialSystem render target is normally Source's current backbuffer,
			// In queued mode the main scene often arrives here as rt=<null>, so treating it
			// as an offscreen pass freezes VR submission on the last completed stereo frame.
			if (!texture)
				return false;

			const bool setupLooksMain =
				isMainViewSized(setup.width, setup.height) ||
				isMainViewSized(setup.m_nUnscaledWidth, setup.m_nUnscaledHeight) ||
				(renderContextStateGuard.hasViewport && isMainViewSized(renderContextStateGuard.w, renderContextStateGuard.h));
			if (setupLooksMain)
				return false;

			int rtMapW = 0;
			int rtMapH = 0;
			int rtActualW = 0;
			int rtActualH = 0;
			DebugTextureFullSize(texture, rtMapW, rtMapH, rtActualW, rtActualH);
			const bool textureLooksMain =
				isMainViewSized(rtMapW, rtMapH) ||
				isMainViewSized(rtActualW, rtActualH);
			if (!textureLooksMain)
			{
				reason = "queued-offscreen-rt";
				return true;
			}

			return false;
		};

	const char* passThroughReason = nullptr;
	if (isWaterReflectionRenderTarget(renderContextStateGuard.rt))
		passThroughReason = "water-reflection";
	else
		isQueuedOffscreenRenderView(passThroughReason);

	if (m_VR->m_RenderPipelineDebugLog && queueMode != 0)
	{
		static std::atomic<int> s_topRenderViewProbeBudget{ 260 };
		const int probeIndex = s_topRenderViewProbeBudget.fetch_sub(1, std::memory_order_acq_rel);
		if (probeIndex > 0)
		{
			int rtMapW = 0;
			int rtMapH = 0;
			int rtActualW = 0;
			int rtActualH = 0;
			DebugTextureFullSize(renderContextStateGuard.rt, rtMapW, rtMapH, rtActualW, rtActualH);
			int backBufferW = 0;
			int backBufferH = 0;
			int windowW = 0;
			int windowH = 0;
			DebugBackBufferDimensions(m_Game ? m_Game->m_MaterialSystem : nullptr, backBufferW, backBufferH);
			DebugRenderContextWindowSize(rndrContext, windowW, windowH);
			const char* rtName = DebugTextureName(renderContextStateGuard.rt);
			const bool vrManagedRt = isVRManagedRenderTarget(renderContextStateGuard.rt);
			const bool waterRt = isWaterReflectionRenderTarget(renderContextStateGuard.rt);
			auto nearSize = [&](int w, int h, int targetW, int targetH) -> bool
				{
					return viewSizeMatches(w, h, targetW, targetH);
				};
			const bool setupMain =
				nearSize(setup.width, setup.height, backBufferW, backBufferH) ||
				nearSize(setup.m_nUnscaledWidth, setup.m_nUnscaledHeight, backBufferW, backBufferH) ||
				nearSize(setup.width, setup.height, windowW, windowH) ||
				nearSize(setup.m_nUnscaledWidth, setup.m_nUnscaledHeight, windowW, windowH) ||
				(renderContextStateGuard.hasViewport &&
					(nearSize(renderContextStateGuard.w, renderContextStateGuard.h, backBufferW, backBufferH) ||
						nearSize(renderContextStateGuard.w, renderContextStateGuard.h, windowW, windowH)));
			const bool rtMain =
				nearSize(rtMapW, rtMapH, backBufferW, backBufferH) ||
				nearSize(rtActualW, rtActualH, backBufferW, backBufferH) ||
				nearSize(rtMapW, rtMapH, windowW, windowH) ||
				nearSize(rtActualW, rtActualH, windowW, windowH);

			Game::logMsg("[VR][RenderView][ProbeTop] left=%d decision=%s reason=%s tid=%lu q=%d rt=%s(map=%dx%d actual=%dx%d vr=%d water=%d) setup=%dx%d unscaled=%dx%d hud=%dx%d vp=%d,%d %dx%d haveVp=%d win=%dx%d bb=%dx%d setupMain=%d rtMain=%d clear=0x%X draw=0x%X",
				probeIndex,
				passThroughReason ? "pass-through" : "vr-hijack",
				passThroughReason ? passThroughReason : "none",
				GetCurrentThreadId(),
				queueMode,
				rtName, rtMapW, rtMapH, rtActualW, rtActualH,
				vrManagedRt ? 1 : 0,
				waterRt ? 1 : 0,
				setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
				hudViewSetup.width, hudViewSetup.height,
				renderContextStateGuard.x, renderContextStateGuard.y, renderContextStateGuard.w, renderContextStateGuard.h,
				renderContextStateGuard.hasViewport ? 1 : 0,
				windowW, windowH, backBufferW, backBufferH,
				setupMain ? 1 : 0,
				rtMain ? 1 : 0,
				nClearFlags, whatToDraw);
		}
	}

	if (passThroughReason != nullptr)
	{
		if (m_VR->m_RenderPipelineDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_lastOffscreenPassLog{};
			if (!ShouldThrottleLog(s_lastOffscreenPassLog, m_VR->m_RenderPipelineDebugLogHz))
			{
				int rtMapW = 0, rtMapH = 0, rtActualW = 0, rtActualH = 0;
				DebugTextureFullSize(renderContextStateGuard.rt, rtMapW, rtMapH, rtActualW, rtActualH);
				Game::logMsg("[VR][RenderView][PassThrough] reason=%s tid=%lu q=%d rt=%s(map=%dx%d actual=%dx%d) setup=%dx%d unscaled=%dx%d hud=%dx%d vp=%d,%d %dx%d clear=0x%X draw=0x%X",
					passThroughReason,
					GetCurrentThreadId(),
					queueMode,
					DebugTextureName(renderContextStateGuard.rt), rtMapW, rtMapH, rtActualW, rtActualH,
					setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
					hudViewSetup.width, hudViewSetup.height,
					renderContextStateGuard.x, renderContextStateGuard.y, renderContextStateGuard.w, renderContextStateGuard.h,
					nClearFlags, whatToDraw);
			}
		}

		callOriginalRenderView(setup, hudViewSetup, nClearFlags, whatToDraw);
		if (queueMode != 0)
		{
			// Top-level water/offscreen passes can return while their material commands
			// are still executing. Restore the caller's RT/viewport before appending the
			// tail marker, then hand ownership back only when that marker runs.
			renderContextStateGuard.Restore();
			if (!m_VR->QueueSourceRenderOwnershipReleaseMarker(rndrContext))
			{
				// No execution fence means this submit snapshot cannot be considered safe
				// while the orphaned offscreen command stream may still be active.
				m_VR->m_SourceRenderQueueOwnershipUncertain.store(true, std::memory_order_release);
				m_VR->m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
				if (m_VR->m_RenderPipelineDebugLog)
					Game::logMsg("[VR][Queued][OffscreenOwnershipMarkerMissing] tid=%lu reason=%s",
						GetCurrentThreadId(), passThroughReason);
			}
		}
		return;
	}

	// Reset "HUD painted" flag once per VR frame (prevents double HUD captures across eyes).
	m_VR->m_HudPaintedThisFrame.store(false, std::memory_order_release);

	// --- Multicore (queued) rendering stabilization ---
	// When mat_queue_mode!=0, the render thread is decoupled from the main/update thread.
	// If we keep using main-thread-computed m_HmdPosAbs/m_HmdAngAbs/m_RightControllerPosAbs inside rendering,
	// we get tearing/jitter (data races) and head-turn ghosting (poses sampled on the wrong thread).
	//
	// Read the dedicated pose-waiter snapshot, combine it with the main-thread seqlock snapshot of
	// camera anchor/scale/offsets, then publish a render-thread snapshot that all render-time getters
	// can read consistently during this dRenderView. When full frame sync is enabled, repeated pose
	// snapshots are paced here before scene rendering so stale HMD poses do not enter the eye RTs.
	uint32_t renderPoseTokenUsed = 0;
	bool renderPoseAllowDuplicateSubmit = false;
	vr::HmdMatrix34_t renderHmdPoseForSubmit{};
	bool renderHmdPoseForSubmitValid = false;
	bool renderPoseUsesTrackingPrediction = false;
	const auto renderHookStartTime = std::chrono::steady_clock::now();
	bool timingPoseAcquireAttempted = false;
	bool timingPoseAcquireFresh = false;
	bool timingPoseAcquireRelaxed = false;
	double timingPoseAcquireMs = 0.0;
	DWORD timingPoseAcquireBudgetMs = 0;
	uint32_t timingPoseSeqBeforeAcquire = 0;
	uint32_t timingPoseSeqAfterAcquire = 0;
	double timingStereoSceneMs = 0.0;
	struct RenderSnapshotTLSGuard
	{
		bool enable = false;
		RenderSnapshotTLSGuard(bool e) : enable(e) { if (enable) VR::t_UseRenderFrameSnapshot = true; }
		~RenderSnapshotTLSGuard() { if (enable) VR::t_UseRenderFrameSnapshot = false; }
	};
	RenderSnapshotTLSGuard __renderTls(queueMode != 0);

	if (m_VR->m_RenderPipelineDebugLog)
	{
		static thread_local std::chrono::steady_clock::time_point s_lastRenderPipeLog{};
		if (!ShouldThrottleLog(s_lastRenderPipeLog, m_VR->m_RenderPipelineDebugLogHz))
		{
			ITexture* currentRt = DebugCurrentRenderTarget(rndrContext);
			int rtMapW = 0;
			int rtMapH = 0;
			int rtActualW = 0;
			int rtActualH = 0;
			DebugTextureFullSize(currentRt, rtMapW, rtMapH, rtActualW, rtActualH);

			ITexture* hudTexture = nullptr;
			{
				std::lock_guard<TextureStateMutex> textureLock(m_VR->m_TextureMutex);
				hudTexture = m_VR->m_HUDTexture;
			}
			int hudMapW = 0;
			int hudMapH = 0;
			int hudActualW = 0;
			int hudActualH = 0;
			DebugTextureFullSize(hudTexture, hudMapW, hudMapH, hudActualW, hudActualH);

			int windowW = 0;
			int windowH = 0;
			int backBufferW = 0;
			int backBufferH = 0;
			int clientW = 0;
			int clientH = 0;
			int vpX = 0;
			int vpY = 0;
			int vpW = 0;
			int vpH = 0;
			DebugRenderContextWindowSize(rndrContext, windowW, windowH);
			DebugBackBufferDimensions(m_Game ? m_Game->m_MaterialSystem : nullptr, backBufferW, backBufferH);
			DebugClientRectSize(clientW, clientH);
			DebugGetViewport(rndrContext, vpX, vpY, vpW, vpH);

			const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
			Game::logMsg("[VR][DesktopHUD][RenderView] tid=%lu q=%d inGame=%d setup=%dx%d unscaled=%dx%d hudSetup=%dx%d hudUnscaled=%dx%d win=%dx%d client=%dx%d bb=%dx%d vp=%d,%d %dx%d rt=%s(map=%dx%d actual=%dx%d) hudTex=%s(map=%dx%d actual=%dx%d) eye=%ux%u clear=0x%X draw=0x%X created=%d",
				GetCurrentThreadId(), queueMode, inGame ? 1 : 0,
				setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
				hudViewSetup.width, hudViewSetup.height, hudViewSetup.m_nUnscaledWidth, hudViewSetup.m_nUnscaledHeight,
				windowW, windowH, clientW, clientH, backBufferW, backBufferH, vpX, vpY, vpW, vpH,
				DebugTextureName(currentRt), rtMapW, rtMapH, rtActualW, rtActualH,
				DebugTextureName(hudTexture), hudMapW, hudMapH, hudActualW, hudActualH,
				m_VR->m_RenderWidth, m_VR->m_RenderHeight,
				nClearFlags, whatToDraw,
				m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) ? 1 : 0);
		}
	}

	if (queueMode != 0 && m_VR && m_VR->m_System && vr::VRCompositor())
	{
		// Remember which thread is producing render snapshots (used by other render-time hooks).
		m_VR->m_RenderThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);


		static thread_local bool s_paceInit = false;
		static thread_local std::chrono::steady_clock::time_point s_nextPace{};
		static thread_local std::chrono::steady_clock::time_point s_smartPaceUntil{};

		// Optional FPS cap: pace the render thread in queued mode when it is outrunning pose updates.
		// Smart mode only engages the cap after we detect stale pose reuse during real motion
		// (body locomotion or HMD translation/rotation), so stable scenes can still run uncapped.
		const int maxFpsEff = m_VR->GetQueuedRenderMaxFpsEffective();
		const bool smartCap = m_VR->m_QueuedRenderMaxFpsSmart;
		const auto nowP = std::chrono::steady_clock::now();
		const bool smartActive = (s_smartPaceUntil.time_since_epoch().count() != 0) && (nowP < s_smartPaceUntil);
		const bool doCapNow = (maxFpsEff > 0) && (!smartCap || smartActive);
		if (doCapNow)
		{
			const double targetSec = 1.0 / (double)maxFpsEff;
			const auto targetDur = std::chrono::duration<double>(targetSec);

			if (!s_paceInit)
			{
				s_nextPace = nowP;
				s_paceInit = true;
			}

			if (nowP < s_nextPace)
			{
				auto rem = s_nextPace - nowP;
				const auto remUs = std::chrono::duration_cast<std::chrono::microseconds>(rem).count();
				if (remUs > 1500)
					Sleep((DWORD)((remUs - 1000) / 1000));
				while (std::chrono::steady_clock::now() < s_nextPace)
					SwitchToThread();
			}
			else
			{
				// If we're far behind (alt-tab/breakpoint), resync.
				const double late = std::chrono::duration<double>(nowP - s_nextPace).count();
				if (late > 0.25)
					s_nextPace = nowP;
			}

			s_nextPace += std::chrono::duration_cast<std::chrono::steady_clock::duration>(targetDur);
		}
		else
		{
			// Not currently capping: reset pacing timeline so we don't apply stale delays when re-enabled.
			s_paceInit = false;
		}

		// Track per-render-call view-origin deltas to reduce model/camera stepping.
		// In queued rendering, the engine camera can update at tick-rate (30/60Hz) while we still
		// render at HMD rate (90Hz+). That can feel like micro-stutter during stick locomotion/turning
		// (even when frametime graphs look flat). We smooth the engine-provided setup origin between
		// tick samples and use that for anchor deltas.
		static EngineThirdPersonCamSmoother s_engineSetupCam;
		QAngle __rawSetupAngles(setup.angles.x, setup.angles.y, setup.angles.z);
		s_engineSetupCam.PushRaw(setup.origin, __rawSetupAngles);
		Vector smoothedSetupOrigin = setup.origin;
		QAngle smoothedSetupAngles = __rawSetupAngles;
		if (s_engineSetupCam.ShouldSmooth())
			s_engineSetupCam.GetSmoothed(smoothedSetupOrigin, smoothedSetupAngles);

		static thread_local Vector s_prevSmoothedSetupOrigin{};
		static thread_local QAngle s_prevSmoothedSetupAngles{};
		static thread_local bool s_prevSmoothedSetupValid = false;
		Vector pendingOriginDelta{};
		float pendingYawDelta = 0.0f;
		if (s_prevSmoothedSetupValid)
		{
			pendingOriginDelta = smoothedSetupOrigin - s_prevSmoothedSetupOrigin;
			pendingYawDelta = AngleDeltaDeg(smoothedSetupAngles.y, s_prevSmoothedSetupAngles.y);
		}
		s_prevSmoothedSetupOrigin = smoothedSetupOrigin;
		s_prevSmoothedSetupAngles = smoothedSetupAngles;
		s_prevSmoothedSetupValid = true;

		struct ViewParams
		{
			Vector cameraAnchor{};
			float rotationOffset = 0.0f;
			float vrScale = 1.0f;
			float ipdScale = 1.0f;
			float eyeZ = 0.0f;
			float ipd = 0.065f;
			Vector hmdPosLocalPrev{};
			Vector hmdPosCorrectedPrev{};
			Vector viewmodelPosOffset{};
			QAngle viewmodelAngOffset{};

			// Extra render-thread state (written by VR::UpdateTracking under the same seqlock).
			bool hasLocalPlayer = false;
			Vector localEyePos{};
			bool hasViewEntityOverride = false;
			int viewEntityHandle = 0;
			bool beingRevived = false;
			bool revivingOther = false;
			bool usingMountedGun = false;
			bool playerIncap = false;
			bool playerControlledBySI = false;
			bool inThirdPersonMapLoadCooldown = false;

			// Subset of ThirdPersonStateDebug for render-thread consumption.
			bool tpWantsThirdPerson = false;
			bool tpObserver = false;
			bool tpDead = false;
			int tpLifeState = 0;
			int tpObserverMode = 0;
			int tpObserverTarget = 0;
			bool tpIncap = false;
			bool tpLedge = false;
			bool tpTongue = false;
			bool tpPinned = false;
			bool tpSelfMedkit = false;
		};

		ViewParams vp{};
		bool vpOk = false;
		uint32_t vpSeqEven = 0;
		// Read main-thread view params with a seqlock. Under heavy load the render thread can
		// consistently collide with the writer; a small yield here avoids freezing the snapshot
		// (which feels like micro-stutter during stick movement/turning).
		for (int attempt = 0; attempt < 32 && !vpOk; ++attempt)
		{
			const uint32_t s1 = m_VR->m_RenderViewParamsSeq.load(std::memory_order_acquire);
			if (s1 == 0 || (s1 & 1u))
			{
				SwitchToThread();
				continue;
			}

			vp.cameraAnchor.x = m_VR->m_RenderCameraAnchorX.load(std::memory_order_relaxed);
			vp.cameraAnchor.y = m_VR->m_RenderCameraAnchorY.load(std::memory_order_relaxed);
			vp.cameraAnchor.z = m_VR->m_RenderCameraAnchorZ.load(std::memory_order_relaxed);
			vp.rotationOffset = m_VR->m_RenderRotationOffset.load(std::memory_order_relaxed);
			vp.vrScale = m_VR->m_RenderVRScale.load(std::memory_order_relaxed);
			vp.ipdScale = m_VR->m_RenderIpdScale.load(std::memory_order_relaxed);
			vp.eyeZ = m_VR->m_RenderEyeZ.load(std::memory_order_relaxed);
			vp.ipd = m_VR->m_RenderIpd.load(std::memory_order_relaxed);
			vp.hmdPosLocalPrev.x = m_VR->m_RenderHmdPosLocalPrevX.load(std::memory_order_relaxed);
			vp.hmdPosLocalPrev.y = m_VR->m_RenderHmdPosLocalPrevY.load(std::memory_order_relaxed);
			vp.hmdPosLocalPrev.z = m_VR->m_RenderHmdPosLocalPrevZ.load(std::memory_order_relaxed);
			vp.hmdPosCorrectedPrev.x = m_VR->m_RenderHmdPosCorrectedPrevX.load(std::memory_order_relaxed);
			vp.hmdPosCorrectedPrev.y = m_VR->m_RenderHmdPosCorrectedPrevY.load(std::memory_order_relaxed);
			vp.hmdPosCorrectedPrev.z = m_VR->m_RenderHmdPosCorrectedPrevZ.load(std::memory_order_relaxed);
			vp.viewmodelPosOffset.x = m_VR->m_RenderViewmodelPosOffsetX.load(std::memory_order_relaxed);
			vp.viewmodelPosOffset.y = m_VR->m_RenderViewmodelPosOffsetY.load(std::memory_order_relaxed);
			vp.viewmodelPosOffset.z = m_VR->m_RenderViewmodelPosOffsetZ.load(std::memory_order_relaxed);
			vp.viewmodelAngOffset.x = m_VR->m_RenderViewmodelAngOffsetX.load(std::memory_order_relaxed);
			vp.viewmodelAngOffset.y = m_VR->m_RenderViewmodelAngOffsetY.load(std::memory_order_relaxed);
			vp.viewmodelAngOffset.z = m_VR->m_RenderViewmodelAngOffsetZ.load(std::memory_order_relaxed);
			vp.hasLocalPlayer = (m_VR->m_RenderHasLocalPlayer.load(std::memory_order_relaxed) != 0);
			vp.localEyePos.x = m_VR->m_RenderLocalEyePosX.load(std::memory_order_relaxed);
			vp.localEyePos.y = m_VR->m_RenderLocalEyePosY.load(std::memory_order_relaxed);
			vp.localEyePos.z = m_VR->m_RenderLocalEyePosZ.load(std::memory_order_relaxed);
			vp.hasViewEntityOverride = (m_VR->m_RenderHasViewEntityOverride.load(std::memory_order_relaxed) != 0);
			vp.viewEntityHandle = m_VR->m_RenderViewEntityHandle.load(std::memory_order_relaxed);
			vp.beingRevived = (m_VR->m_RenderBeingRevived.load(std::memory_order_relaxed) != 0);
			vp.revivingOther = (m_VR->m_RenderRevivingOther.load(std::memory_order_relaxed) != 0);
			vp.usingMountedGun = (m_VR->m_RenderUsingMountedGun.load(std::memory_order_relaxed) != 0);
			vp.playerIncap = (m_VR->m_RenderPlayerIncap.load(std::memory_order_relaxed) != 0);
			vp.playerControlledBySI = (m_VR->m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) != 0);
			vp.inThirdPersonMapLoadCooldown = (m_VR->m_RenderInThirdPersonMapLoadCooldown.load(std::memory_order_relaxed) != 0);

			vp.tpWantsThirdPerson = (m_VR->m_RenderTpWantsThirdPerson.load(std::memory_order_relaxed) != 0);
			vp.tpObserver = (m_VR->m_RenderTpObserver.load(std::memory_order_relaxed) != 0);
			vp.tpDead = (m_VR->m_RenderTpDead.load(std::memory_order_relaxed) != 0);
			vp.tpLifeState = m_VR->m_RenderTpLifeState.load(std::memory_order_relaxed);
			vp.tpObserverMode = m_VR->m_RenderTpObserverMode.load(std::memory_order_relaxed);
			vp.tpObserverTarget = m_VR->m_RenderTpObserverTarget.load(std::memory_order_relaxed);
			vp.tpIncap = (m_VR->m_RenderTpIncap.load(std::memory_order_relaxed) != 0);
			vp.tpLedge = (m_VR->m_RenderTpLedge.load(std::memory_order_relaxed) != 0);
			vp.tpTongue = (m_VR->m_RenderTpTongue.load(std::memory_order_relaxed) != 0);
			vp.tpPinned = (m_VR->m_RenderTpPinned.load(std::memory_order_relaxed) != 0);
			vp.tpSelfMedkit = (m_VR->m_RenderTpSelfMedkit.load(std::memory_order_relaxed) != 0);

			const uint32_t s2 = m_VR->m_RenderViewParamsSeq.load(std::memory_order_acquire);
			if (s1 == s2 && !(s2 & 1u))
			{
				vpSeqEven = s2;
				vpOk = true;
			}
			else
			{
				SwitchToThread();
			}
		}
		// If we fail to read a consistent seqlock snapshot, do NOT freeze the render-frame snapshot.
		// Freezing for a frame or two feels like "stutter + ghosting" during stick locomotion/turning,
		// even when CPU/GPU frametime graphs look flat.
		static thread_local ViewParams s_cachedVp{};
		static thread_local uint32_t s_cachedVpSeqEven = 0;
		static thread_local bool s_cachedVpValid = false;
		static thread_local int s_vpMissStreak = 0;

		auto ReadViewParamsUnsafe = [&]()
			{
				vp.cameraAnchor.x = m_VR->m_RenderCameraAnchorX.load(std::memory_order_relaxed);
				vp.cameraAnchor.y = m_VR->m_RenderCameraAnchorY.load(std::memory_order_relaxed);
				vp.cameraAnchor.z = m_VR->m_RenderCameraAnchorZ.load(std::memory_order_relaxed);
				vp.rotationOffset = m_VR->m_RenderRotationOffset.load(std::memory_order_relaxed);
				vp.vrScale = m_VR->m_RenderVRScale.load(std::memory_order_relaxed);
				vp.ipdScale = m_VR->m_RenderIpdScale.load(std::memory_order_relaxed);
				vp.eyeZ = m_VR->m_RenderEyeZ.load(std::memory_order_relaxed);
				vp.ipd = m_VR->m_RenderIpd.load(std::memory_order_relaxed);
				vp.hmdPosLocalPrev.x = m_VR->m_RenderHmdPosLocalPrevX.load(std::memory_order_relaxed);
				vp.hmdPosLocalPrev.y = m_VR->m_RenderHmdPosLocalPrevY.load(std::memory_order_relaxed);
				vp.hmdPosLocalPrev.z = m_VR->m_RenderHmdPosLocalPrevZ.load(std::memory_order_relaxed);
				vp.hmdPosCorrectedPrev.x = m_VR->m_RenderHmdPosCorrectedPrevX.load(std::memory_order_relaxed);
				vp.hmdPosCorrectedPrev.y = m_VR->m_RenderHmdPosCorrectedPrevY.load(std::memory_order_relaxed);
				vp.hmdPosCorrectedPrev.z = m_VR->m_RenderHmdPosCorrectedPrevZ.load(std::memory_order_relaxed);
				vp.viewmodelPosOffset.x = m_VR->m_RenderViewmodelPosOffsetX.load(std::memory_order_relaxed);
				vp.viewmodelPosOffset.y = m_VR->m_RenderViewmodelPosOffsetY.load(std::memory_order_relaxed);
				vp.viewmodelPosOffset.z = m_VR->m_RenderViewmodelPosOffsetZ.load(std::memory_order_relaxed);
				vp.viewmodelAngOffset.x = m_VR->m_RenderViewmodelAngOffsetX.load(std::memory_order_relaxed);
				vp.viewmodelAngOffset.y = m_VR->m_RenderViewmodelAngOffsetY.load(std::memory_order_relaxed);
				vp.viewmodelAngOffset.z = m_VR->m_RenderViewmodelAngOffsetZ.load(std::memory_order_relaxed);
				vp.hasLocalPlayer = (m_VR->m_RenderHasLocalPlayer.load(std::memory_order_relaxed) != 0);
				vp.localEyePos.x = m_VR->m_RenderLocalEyePosX.load(std::memory_order_relaxed);
				vp.localEyePos.y = m_VR->m_RenderLocalEyePosY.load(std::memory_order_relaxed);
				vp.localEyePos.z = m_VR->m_RenderLocalEyePosZ.load(std::memory_order_relaxed);
				vp.hasViewEntityOverride = (m_VR->m_RenderHasViewEntityOverride.load(std::memory_order_relaxed) != 0);
				vp.viewEntityHandle = m_VR->m_RenderViewEntityHandle.load(std::memory_order_relaxed);
				vp.beingRevived = (m_VR->m_RenderBeingRevived.load(std::memory_order_relaxed) != 0);
				vp.revivingOther = (m_VR->m_RenderRevivingOther.load(std::memory_order_relaxed) != 0);
				vp.usingMountedGun = (m_VR->m_RenderUsingMountedGun.load(std::memory_order_relaxed) != 0);
				vp.playerIncap = (m_VR->m_RenderPlayerIncap.load(std::memory_order_relaxed) != 0);
				vp.playerControlledBySI = (m_VR->m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) != 0);
				vp.inThirdPersonMapLoadCooldown = (m_VR->m_RenderInThirdPersonMapLoadCooldown.load(std::memory_order_relaxed) != 0);

				vp.tpWantsThirdPerson = (m_VR->m_RenderTpWantsThirdPerson.load(std::memory_order_relaxed) != 0);
				vp.tpObserver = (m_VR->m_RenderTpObserver.load(std::memory_order_relaxed) != 0);
				vp.tpDead = (m_VR->m_RenderTpDead.load(std::memory_order_relaxed) != 0);
				vp.tpLifeState = m_VR->m_RenderTpLifeState.load(std::memory_order_relaxed);
				vp.tpObserverMode = m_VR->m_RenderTpObserverMode.load(std::memory_order_relaxed);
				vp.tpObserverTarget = m_VR->m_RenderTpObserverTarget.load(std::memory_order_relaxed);
				vp.tpIncap = (m_VR->m_RenderTpIncap.load(std::memory_order_relaxed) != 0);
				vp.tpLedge = (m_VR->m_RenderTpLedge.load(std::memory_order_relaxed) != 0);
				vp.tpTongue = (m_VR->m_RenderTpTongue.load(std::memory_order_relaxed) != 0);
				vp.tpPinned = (m_VR->m_RenderTpPinned.load(std::memory_order_relaxed) != 0);
				vp.tpSelfMedkit = (m_VR->m_RenderTpSelfMedkit.load(std::memory_order_relaxed) != 0);
			};

		if (vpOk)
		{
			s_cachedVp = vp;
			s_cachedVpValid = true;
			s_cachedVpSeqEven = vpSeqEven;
			s_vpMissStreak = 0;
		}
		else
		{
			++s_vpMissStreak;

			if (s_cachedVpValid)
			{
				vp = s_cachedVp;
				vpOk = true;
				vpSeqEven = s_cachedVpSeqEven;
			}
			else
			{
				// First frames after map load: accept a torn read rather than rendering with a stale snapshot.
				ReadViewParamsUnsafe();
				vpOk = true;
			}

		}

		// Render-thread smoothing of camera anchor / body yaw in queued rendering.
//
// Why: under mat_queue_mode 2, cameraAnchor/rotationOffset are produced on the update thread.
// Even when seqlock prevents tearing, those values can still advance in \"steps\" (tick cadence),
// which SteamVR reprojection turns into ghosting / micro-stutter during stick locomotion/turning.
//
// Strategy: run a tiny 1st-order low-pass filter on the render thread. This turns step updates
// into continuous motion at HMD rate. The smoothing is only \"active\" while there's a meaningful
// error between the filtered state and the latest snapshot; when idle, we snap to avoid lag.
		static thread_local bool s_viewSmoothValid = false;
		static thread_local Vector s_viewSmoothAnchor{};
		static thread_local float s_viewSmoothRot = 0.0f;
		static thread_local std::chrono::steady_clock::time_point s_viewSmoothLastT{};
		bool didSnapSmooth = false;

		auto Wrap360 = [&](float a) -> float
			{
				a -= 360.0f * std::floor(a / 360.0f);
				return a;
			};

		const int smoothMsCfg = std::max(0, m_VR->m_QueuedRenderViewSmoothMs);
		float alpha = 1.0f;
		if (smoothMsCfg > 0)
		{
			const auto nowSmooth = std::chrono::steady_clock::now();
			float dt = 0.0f;
			if (s_viewSmoothLastT.time_since_epoch().count() != 0)
				dt = std::chrono::duration<float>(nowSmooth - s_viewSmoothLastT).count();
			s_viewSmoothLastT = nowSmooth;

			// Clamp dt so we don't get a giant alpha after alt-tab / breakpoint.
			dt = std::clamp(dt, 0.0f, 0.050f);
			const float tau = (float)smoothMsCfg / 1000.0f;
			alpha = (tau > 0.0f) ? (1.0f - std::exp(-dt / tau)) : 1.0f;
			alpha = std::clamp(alpha, 0.0f, 1.0f);
		}

		if (vpOk)
		{
			if (!s_viewSmoothValid)
			{
				s_viewSmoothAnchor = vp.cameraAnchor;
				s_viewSmoothRot = Wrap360(vp.rotationOffset);
				s_viewSmoothValid = true;
				didSnapSmooth = true;
			}
			else
			{
				const Vector errA = vp.cameraAnchor - s_viewSmoothAnchor;
				const float errYaw = AngleDeltaDeg(vp.rotationOffset, s_viewSmoothRot);

				// Large discontinuities -> snap immediately.
				if (errA.LengthSqr() > (256.0f * 256.0f) || std::fabs(errYaw) > 120.0f)
				{
					s_viewSmoothAnchor = vp.cameraAnchor;
					s_viewSmoothRot = Wrap360(vp.rotationOffset);
					didSnapSmooth = true;
				}
				else if (smoothMsCfg <= 0)
				{
					// Smoothing disabled -> follow exactly.
					s_viewSmoothAnchor = vp.cameraAnchor;
					s_viewSmoothRot = Wrap360(vp.rotationOffset);
				}
				else
				{
					// If we're basically synced, snap to avoid tiny residual lag.
					const bool smallErr = (errA.LengthSqr() < (0.05f * 0.05f)) && (std::fabs(errYaw) < 0.05f);
					if (smallErr)
					{
						s_viewSmoothAnchor = vp.cameraAnchor;
						s_viewSmoothRot = Wrap360(vp.rotationOffset);
						didSnapSmooth = true;
					}
					else
					{
						s_viewSmoothAnchor += errA * alpha;
						s_viewSmoothRot = Wrap360(s_viewSmoothRot + errYaw * alpha);
					}
				}
			}
		}

		const Vector extrapAnchor = s_viewSmoothValid ? s_viewSmoothAnchor : vp.cameraAnchor;
		const float extrapRot = s_viewSmoothValid ? s_viewSmoothRot : vp.rotationOffset;

		// For debug: show how \"steppy\" the engine view inputs are (not used for smoothing).
		const float pendingDeltaSq = pendingOriginDelta.LengthSqr();

		// Heuristic: treat body locomotion/turn as "active" if we see meaningful anchor delta
		// or view-rotation offset changes. This is separate from HMD motion detection below.
		bool locomotionNow = (pendingDeltaSq > (0.05f * 0.05f));

		static thread_local bool s_prevRotValid = false;
		static thread_local float s_prevRot = 0.0f;
		if (s_prevRotValid)
		{
			float dRot = vp.rotationOffset - s_prevRot;
			dRot -= 360.0f * std::floor((dRot + 180.0f) / 360.0f);
			if (std::fabs(dRot) > 0.01f)
				locomotionNow = true;
		}
		s_prevRot = vp.rotationOffset;
		s_prevRotValid = true;


		if (vpOk)
		{
			std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses{};

			// In queued mode, avoid blocking on WaitGetPoses in the render hook.
			// Prefer explicit non-blocking tracking prediction when requested; otherwise read the
			// pose waiter snapshot (WaitGetPoses on a dedicated thread).
			uint32_t poseSeq = 0;
			const bool useTrackingRenderPose =
				(queueMode != 0) &&
				m_VR->m_QueuedRenderPoseFromTracking &&
				m_VR->m_QueuedSubmitUseRenderPoseToken &&
				m_VR->m_System &&
				!m_VR->m_ReShadeVRCompat;
			bool havePoses = false;
			if (useTrackingRenderPose)
			{
				const vr::ETrackingUniverseOrigin trackingOrigin = m_VR->GetCachedTrackingUniverseOrigin();
				float predicted = m_VR->GetQueuedTrackingPredictionSeconds();
				if (!(predicted >= 0.0f && predicted <= 0.5f))
					predicted = 0.0f;
				m_VR->m_System->GetDeviceToAbsoluteTrackingPose(trackingOrigin, predicted, renderPoses.data(), vr::k_unMaxTrackedDeviceCount);
				havePoses = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
				if (havePoses)
				{
					const uint32_t rawTrackingSeq = m_VR->m_QueuedRenderPoseFromTrackingSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
					poseSeq = 0x80000000u | (rawTrackingSeq & 0x7fffffffu);
					renderPoseTokenUsed = poseSeq;
					renderPoseUsesTrackingPrediction = true;
					renderHmdPoseForSubmit = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
					renderHmdPoseForSubmitValid = true;
				}
			}
			if (!havePoses)
				havePoses = m_VR->ReadPoseWaiterSnapshot(renderPoses.data(), &poseSeq);

			// Pose snapshot reuse tracking (render-thread local). Used to keep queued rendering
			// from producing multiple stereo frames from one HMD pose.
			static thread_local uint32_t s_lastPoseSeq = 0;
			static thread_local uint32_t s_lastWaitAttemptSeq = 0;
			static thread_local int s_poseReuseCount = 0;
			static thread_local int s_poseRelaxAccumulator = 0;
			const uint32_t poseSeqBeforeRender = s_lastPoseSeq;

			auto waitForNewPoseSnapshot = [&](uint32_t oldSeq, DWORD timeoutMs) -> bool
				{
					if (!m_VR->m_PoseWaiterEvent || oldSeq == 0 || timeoutMs == 0)
						return false;

					const DWORD startTicks = GetTickCount();
					DWORD remaining = timeoutMs;
					while (remaining > 0)
					{
						const DWORD wr = WaitForSingleObject(m_VR->m_PoseWaiterEvent, remaining);
						if (wr != WAIT_OBJECT_0)
							break;

						uint32_t poseSeq2 = 0;
						if (m_VR->ReadPoseWaiterSnapshot(renderPoses.data(), &poseSeq2) && poseSeq2 != 0 && poseSeq2 != oldSeq)
						{
							poseSeq = poseSeq2;
							havePoses = true;
							return true;
						}

						const DWORD elapsed = GetTickCount() - startTicks;
						if (elapsed >= timeoutMs)
							break;
						remaining = timeoutMs - elapsed;
					}
					return false;
				};

			auto hmdFramePoseWaitMs = [&]() -> DWORD
				{
					float hmdHz = m_VR->GetHmdDisplayFrequencyHz();
					if (!(hmdHz > 1.0f))
						hmdHz = 90.0f;
					return static_cast<DWORD>(std::clamp((int)std::ceil(1000.0f / hmdHz) + 4, 6, 30));
				};

			const bool fullSyncSubmitGate =
				(queueMode != 0) &&
				m_VR->m_QueuedSubmitUseRenderPoseToken;
			const int waitMsCfg = m_VR->m_QueuedRenderPoseWaitMs;
			bool preRenderPoseAcquireAttempted = false;
			bool preRenderPoseAcquireFresh = false;
			bool preRenderPoseAcquireRelaxed = false;
			DWORD preRenderPoseAcquireWaitMs = 0;

			// Full frame sync means the eye scene should be built from a pose token that can submit.
			// Acquire that fresh waiter pose here, before view/projection and both eye RenderView calls.
			if (!renderPoseUsesTrackingPrediction && fullSyncSubmitGate && !havePoses && m_VR->m_PoseWaiterEvent)
			{
				const DWORD firstWait = std::min<DWORD>(hmdFramePoseWaitMs(), 5u);
				if (WaitForSingleObject(m_VR->m_PoseWaiterEvent, firstWait) == WAIT_OBJECT_0)
					havePoses = m_VR->ReadPoseWaiterSnapshot(renderPoses.data(), &poseSeq);
			}

			if (!renderPoseUsesTrackingPrediction && fullSyncSubmitGate && havePoses && poseSeq != 0 && poseSeq == s_lastPoseSeq)
			{
				const int relaxPct = std::clamp(m_VR->m_QueuedRenderPoseRelaxPercent, 0, 100);
				bool relaxThisPose = false;
				if (relaxPct > 0)
				{
					s_poseRelaxAccumulator = std::min(199, s_poseRelaxAccumulator + relaxPct);
					if (s_poseRelaxAccumulator >= 100)
					{
						s_poseRelaxAccumulator -= 100;
						relaxThisPose = true;
						renderPoseAllowDuplicateSubmit = true;
					}
				}
				else
				{
					s_poseRelaxAccumulator = 0;
				}

				preRenderPoseAcquireAttempted = true;
				preRenderPoseAcquireRelaxed = relaxThisPose;
				timingPoseAcquireAttempted = true;
				timingPoseAcquireRelaxed = relaxThisPose;
				timingPoseSeqBeforeAcquire = poseSeq;
				if (!relaxThisPose)
				{
					const uint32_t oldPoseSeq = poseSeq;
					preRenderPoseAcquireWaitMs =
						(waitMsCfg < 0) ? 50u :
						(waitMsCfg > 0) ? static_cast<DWORD>(std::clamp(waitMsCfg, 1, 200)) :
						hmdFramePoseWaitMs();
					s_lastWaitAttemptSeq = oldPoseSeq;
					const auto acquireStart = std::chrono::steady_clock::now();
					preRenderPoseAcquireFresh = waitForNewPoseSnapshot(oldPoseSeq, preRenderPoseAcquireWaitMs);
					timingPoseAcquireMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart).count();
				}
				timingPoseAcquireFresh = preRenderPoseAcquireFresh;
				timingPoseAcquireBudgetMs = preRenderPoseAcquireWaitMs;
				timingPoseSeqAfterAcquire = poseSeq;

				if (preRenderPoseAcquireFresh)
				{
					s_poseReuseCount = 0;
					s_poseRelaxAccumulator = 0;
				}
				else if (m_VR->m_RenderPipelineDebugLog)
				{
					static thread_local std::chrono::steady_clock::time_point s_lastPoseAcquireLog{};
					if (!ShouldThrottleLog(s_lastPoseAcquireLog, m_VR->m_RenderPipelineDebugLogHz))
					{
						Game::logMsg("[VR][Queued][RenderPoseAcquire] tid=%lu q=%d waitMs=%lu poseSeq=%u lastPoseSeq=%u relaxPct=%d relaxed=%d fresh=%d submitPose=%u lastSubmitted=%u",
							GetCurrentThreadId(),
							queueMode,
							static_cast<unsigned long>(preRenderPoseAcquireWaitMs),
							poseSeq,
							s_lastPoseSeq,
							relaxPct,
							preRenderPoseAcquireRelaxed ? 1 : 0,
							preRenderPoseAcquireFresh ? 1 : 0,
							m_VR->m_SubmitPoseToken.load(std::memory_order_acquire),
							m_VR->m_LastSubmittedPoseToken.load(std::memory_order_acquire));
					}
				}
			}

			// Heuristic: treat fast HMD rotation as "active" (helps decide whether to nudge pose waiting).
			bool headTurningNow = false;
			if (havePoses && renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
			{
				const auto& av = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].vAngularVelocity;
				const double ax = (double)av.v[0];
				const double ay = (double)av.v[1];
				const double az = (double)av.v[2];
				if (std::isfinite(ax) && std::isfinite(ay) && std::isfinite(az))
				{
					const double radPerSec = std::sqrt(ax * ax + ay * ay + az * az);
					const double degPerSec = radPerSec * (180.0 / 3.14159265358979323846);
					headTurningNow = (degPerSec > 50.0);
				}
			}

			// Optional pacing knob: in queued mode the render thread can outrun VR pose updates.
			// Waiting a bit for a fresh WaitGetPoses() snapshot trades throughput for stability.
			int waitMs = waitMsCfg;

			const int maxAheadCfgRaw = std::clamp(m_VR->m_QueuedRenderMaxFramesAhead, -1, 6);
			const int maxAheadCfg = fullSyncSubmitGate ? -1 : maxAheadCfgRaw;

			const bool motionNow = (locomotionNow || headTurningNow);

			// Update pose snapshot reuse count early (before optional waiting), so smart pacing can react
			// even when QueuedRenderPoseWaitMs==0 and MaxFramesAhead is disabled.
			if (havePoses && poseSeq != 0)
			{
				if (poseSeq != s_lastPoseSeq)
					s_poseReuseCount = 0;
				else
					++s_poseReuseCount;
			}

			// Smart FPS pacing pre-trigger: if we're moving/turning and reusing the same pose snapshot,
			// engage the FPS cap for a short window (hysteresis).
			if (m_VR->GetQueuedRenderMaxFpsEffective() > 0 && m_VR->m_QueuedRenderMaxFpsSmart)
			{
				if (motionNow && havePoses && poseSeq != 0 && poseSeq == s_lastPoseSeq && s_poseReuseCount >= 1)
				{
					const int holdMs = (s_poseReuseCount >= 2) ? 500 : 250;
					auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
					if (s_smartPaceUntil.time_since_epoch().count() == 0 || until > s_smartPaceUntil)
						s_smartPaceUntil = until;
				}
			}

			// Non-full-sync frames-ahead limiter. Full-sync has already acquired a submit-ready pose
			// above, so do not run this older optional wait path on top of it.
			if (!fullSyncSubmitGate && (waitMs != 0 || maxAheadCfg >= 0) && m_VR->m_PoseWaiterEvent)
			{

				const int maxFpsEff = m_VR->GetQueuedRenderMaxFpsEffective();
				DWORD timeoutMs = (waitMs < 0) ? 50u : (DWORD)std::clamp(waitMs, 0, 200);

				DWORD aheadTimeoutMs = 0;
				if (maxAheadCfg >= 0)
				{
					if (maxFpsEff > 0)
					{
						const double intervalMs = (1000.0 / (double)maxFpsEff);
						aheadTimeoutMs = (DWORD)std::clamp((int)std::ceil(intervalMs) + 3, 2, 50);
					}
					else
					{
						aheadTimeoutMs = 15u;
					}
				}

				DWORD effectiveTimeoutMs = timeoutMs;
				if (effectiveTimeoutMs == 0 && maxAheadCfg >= 0)
					effectiveTimeoutMs = aheadTimeoutMs;

				// Early-frame assist: if we have no snapshot yet, wait briefly for the first one.
				if (!havePoses && effectiveTimeoutMs > 0)
				{
					const DWORD firstWait = (effectiveTimeoutMs < 5u) ? effectiveTimeoutMs : 5u;
					if (WaitForSingleObject(m_VR->m_PoseWaiterEvent, firstWait) == WAIT_OBJECT_0)
						havePoses = m_VR->ReadPoseWaiterSnapshot(renderPoses.data(), &poseSeq);
				}


				const bool exceededAhead =
					(maxAheadCfg >= 0) &&
					havePoses && poseSeq != 0 &&
					poseSeq == s_lastPoseSeq &&
					(s_poseReuseCount > maxAheadCfg);

				// Smart FPS pacing trigger (strong): if MaxFramesAhead is exceeded during motion, extend the
				// pacing window so the render thread settles into a stable cadence.
				if (m_VR->GetQueuedRenderMaxFpsEffective() > 0 && m_VR->m_QueuedRenderMaxFpsSmart)
				{
					if (motionNow && exceededAhead)
					{
						const int holdMs = 600;
						auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
						if (s_smartPaceUntil.time_since_epoch().count() == 0 || until > s_smartPaceUntil)
							s_smartPaceUntil = until;
					}
				}


				// If we already consumed this snapshot, optionally wait for the next one.
				if (havePoses && poseSeq != 0 && poseSeq == s_lastPoseSeq && effectiveTimeoutMs > 0)
				{
					const bool shouldWaitNext = exceededAhead || (timeoutMs > 0 && poseSeq != s_lastWaitAttemptSeq);
					if (shouldWaitNext)
					{
						// If the user didn't opt into waiting but we're moving/turning and reusing the same pose,
						// do a tiny auto-wait to avoid "stutter + ghosting" during thumbstick locomotion / head turns.
						if (!exceededAhead && waitMsCfg == 0 && waitMs > 0)
						{
							static thread_local std::chrono::steady_clock::time_point s_lastAutoWaitLog{};
							const auto nowLog = std::chrono::steady_clock::now();
							if (s_lastAutoWaitLog.time_since_epoch().count() == 0 ||
								std::chrono::duration<float>(nowLog - s_lastAutoWaitLog).count() > 1.0f)
							{
								s_lastAutoWaitLog = nowLog;
							}
						}

						const uint32_t wantSeq = poseSeq;
						s_lastWaitAttemptSeq = wantSeq;
						DWORD startTicks = GetTickCount();
						DWORD remaining = effectiveTimeoutMs;
						while (remaining > 0)
						{
							const DWORD wr = WaitForSingleObject(m_VR->m_PoseWaiterEvent, remaining);
							if (wr != WAIT_OBJECT_0)
								break;

							uint32_t poseSeq2 = 0;
							if (m_VR->ReadPoseWaiterSnapshot(renderPoses.data(), &poseSeq2) && poseSeq2 != 0 && poseSeq2 != wantSeq)
							{
								poseSeq = poseSeq2;
								// New pose -> reset reuse counter.
								s_poseReuseCount = 0;
								break;
							}

							const DWORD elapsed = GetTickCount() - startTicks;
							if (elapsed >= effectiveTimeoutMs)
								break;
							remaining = effectiveTimeoutMs - elapsed;
						}
					}
				}

				if (havePoses)
					s_lastPoseSeq = poseSeq;
			}
			const bool finalPoseRepeatsPrevious =
				havePoses &&
				poseSeq != 0 &&
				poseSeq == poseSeqBeforeRender;
			if (!renderPoseAllowDuplicateSubmit &&
				!preRenderPoseAcquireAttempted &&
				(queueMode != 0) &&
				m_VR->m_QueuedSubmitUseRenderPoseToken &&
				finalPoseRepeatsPrevious)
			{
				const int relaxPct = std::clamp(m_VR->m_QueuedRenderPoseRelaxPercent, 0, 100);
				if (relaxPct > 0)
				{
					s_poseRelaxAccumulator = std::min(199, s_poseRelaxAccumulator + relaxPct);
					if (s_poseRelaxAccumulator >= 100)
					{
						s_poseRelaxAccumulator -= 100;
						renderPoseAllowDuplicateSubmit = true;
					}
				}
				else
				{
					s_poseRelaxAccumulator = 0;
				}
			}
			// Update last poseSeq even when we didn't enter the wait block.
			if (havePoses && poseSeq != 0)
			{
				if (!finalPoseRepeatsPrevious)
					s_poseRelaxAccumulator = 0;
				s_lastPoseSeq = poseSeq;
			}
			if (havePoses && poseSeq != 0)
				renderPoseTokenUsed = poseSeq;

			// Periodic diagnostics (piggyback on QueuedViewmodelStabilizeDebugLog).
			if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
			{
				static thread_local std::chrono::steady_clock::time_point s_lastStatusLog{};
				if (!ShouldThrottleLog(s_lastStatusLog, 1.0f))
				{
					const Vector smoothErrA = vp.cameraAnchor - extrapAnchor;
					const float smoothErrYaw = AngleDeltaDeg(vp.rotationOffset, extrapRot);
					Game::logMsg("[VR][Queued][RenderView] status q=%d vpSeq=%u poseSeq=%u havePoses=%d waitCfg=%d waitEff=%d snap=%d smoothMs=%d alpha=%.3f errD=%.3f errYaw=%.3f pendD=%.4f pendYaw=%.3f vpRot=%.2f smRot=%.2f tick=%.1fms",
						queueMode, (unsigned)vpSeqEven, (unsigned)poseSeq, havePoses ? 1 : 0,
						waitMsCfg, waitMs, didSnapSmooth ? 1 : 0, smoothMsCfg, alpha,
						smoothErrA.Length(), smoothErrYaw,
						std::sqrt(pendingDeltaSq), pendingYawDelta,
						vp.rotationOffset, extrapRot,
						s_engineSetupCam.tickIntervalSec * 1000.0f);
				}
			}

			if (!havePoses)
			{
				if (m_VR->m_System)
				{
					const vr::ETrackingUniverseOrigin trackingOrigin = m_VR->GetCachedTrackingUniverseOrigin();
					float predicted = m_VR->GetQueuedTrackingPredictionSeconds();
					if (!(predicted >= 0.0f && predicted <= 0.5f))
						predicted = 0.0f;
					m_VR->m_System->GetDeviceToAbsoluteTrackingPose(trackingOrigin, predicted, renderPoses.data(), vr::k_unMaxTrackedDeviceCount);
				}
				renderPoseTokenUsed = m_VR->m_PoseWaiterSeq.load(std::memory_order_acquire);
			}

			if (renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
			{
				TrackedDevicePoseData hmdPose{};
				m_VR->GetPoseData(renderPoses[vr::k_unTrackedDeviceIndex_Hmd], hmdPose);
				QAngle hmdAngLocal = hmdPose.TrackedDeviceAng;
				Vector hmdPosLocal = hmdPose.TrackedDevicePos;

				// Predict corrected position using the last main-thread corrected frame as a base.
				Vector hmdPosCorrected = vp.hmdPosCorrectedPrev + (hmdPosLocal - vp.hmdPosLocalPrev);
				VectorPivotXY(hmdPosCorrected, vp.hmdPosCorrectedPrev, extrapRot);

				hmdAngLocal.y += extrapRot;
				hmdAngLocal.y -= 360.0f * std::floor((hmdAngLocal.y + 180.0f) / 360.0f);

				// Optional HMD pose smoothing (visual-only). This can soften stepping from stale pose reuse,
				// but it does not fetch a fresher pose and cannot fully solve queued-render head-motion ghosting.
				const int hmdSmoothMsCfg = std::max(0, m_VR->m_QueuedRenderHmdSmoothMs);
				static thread_local bool s_hmdSmoothValid = false;
				static thread_local Vector s_hmdSmoothPos{};
				static thread_local QAngle s_hmdSmoothAng{};
				static thread_local std::chrono::steady_clock::time_point s_hmdSmoothLastT{};

				float hmdAlpha = 1.0f;
				if (hmdSmoothMsCfg > 0)
				{
					const auto nowSmooth = std::chrono::steady_clock::now();
					float dt = 0.0f;
					if (s_hmdSmoothLastT.time_since_epoch().count() != 0)
						dt = std::chrono::duration<float>(nowSmooth - s_hmdSmoothLastT).count();
					s_hmdSmoothLastT = nowSmooth;

					// Clamp dt so we don't get a giant alpha after alt-tab / breakpoint.
					dt = std::clamp(dt, 0.0f, 0.050f);
					const float tau = (float)hmdSmoothMsCfg / 1000.0f;
					hmdAlpha = (tau > 0.0f) ? (1.0f - std::exp(-dt / tau)) : 1.0f;
					hmdAlpha = std::clamp(hmdAlpha, 0.0f, 1.0f);
				}

				auto Wrap180 = [&](float a) -> float
					{
						a -= 360.0f * std::floor((a + 180.0f) / 360.0f);
						return a;
					};

				if (hmdSmoothMsCfg > 0)
				{
					if (!s_hmdSmoothValid)
					{
						s_hmdSmoothPos = hmdPosCorrected;
						s_hmdSmoothAng = hmdAngLocal;
						s_hmdSmoothAng.x = Wrap180(s_hmdSmoothAng.x);
						s_hmdSmoothAng.y = Wrap180(s_hmdSmoothAng.y);
						s_hmdSmoothAng.z = Wrap180(s_hmdSmoothAng.z);
						s_hmdSmoothValid = true;
					}
					else
					{
						const Vector errP = hmdPosCorrected - s_hmdSmoothPos;
						const float errX = AngleDeltaDeg(hmdAngLocal.x, s_hmdSmoothAng.x);
						const float errY = AngleDeltaDeg(hmdAngLocal.y, s_hmdSmoothAng.y);
						const float errZ = AngleDeltaDeg(hmdAngLocal.z, s_hmdSmoothAng.z);

						// Large discontinuities -> snap immediately (teleport/reset).
						if (errP.LengthSqr() > (1.0f * 1.0f) || (std::fabs(errX) > 90.0f) || (std::fabs(errY) > 90.0f) || (std::fabs(errZ) > 90.0f))
						{
							s_hmdSmoothPos = hmdPosCorrected;
							s_hmdSmoothAng = hmdAngLocal;
							s_hmdSmoothAng.x = Wrap180(s_hmdSmoothAng.x);
							s_hmdSmoothAng.y = Wrap180(s_hmdSmoothAng.y);
							s_hmdSmoothAng.z = Wrap180(s_hmdSmoothAng.z);
						}
						else
						{
							// If we're basically synced, snap to avoid tiny residual lag.
							const bool smallErr = (errP.LengthSqr() < (0.0005f * 0.0005f))
								&& (std::fabs(errX) < 0.05f) && (std::fabs(errY) < 0.05f) && (std::fabs(errZ) < 0.05f);
							if (smallErr)
							{
								s_hmdSmoothPos = hmdPosCorrected;
								s_hmdSmoothAng = hmdAngLocal;
								s_hmdSmoothAng.x = Wrap180(s_hmdSmoothAng.x);
								s_hmdSmoothAng.y = Wrap180(s_hmdSmoothAng.y);
								s_hmdSmoothAng.z = Wrap180(s_hmdSmoothAng.z);
							}
							else
							{
								s_hmdSmoothPos += errP * hmdAlpha;
								s_hmdSmoothAng.x = Wrap180(s_hmdSmoothAng.x + errX * hmdAlpha);
								s_hmdSmoothAng.y = Wrap180(s_hmdSmoothAng.y + errY * hmdAlpha);
								s_hmdSmoothAng.z = Wrap180(s_hmdSmoothAng.z + errZ * hmdAlpha);
							}
						}
					}

					// Apply smoothed pose.
					hmdPosCorrected = s_hmdSmoothPos;
					hmdAngLocal = s_hmdSmoothAng;
				}


				Vector hmdForward, hmdRight, hmdUp;
				QAngle::AngleVectors(hmdAngLocal, &hmdForward, &hmdRight, &hmdUp);

				// Advance the anchor by the tick-rate origin delta observed on the render thread.
				Vector cameraAnchor = extrapAnchor;
				Vector hmdPosAbs = cameraAnchor - Vector(0, 0, 64) + (hmdPosCorrected * vp.vrScale);

				const float ipdSu = (vp.ipd * vp.ipdScale * vp.vrScale);
				const float eyeZSu = (vp.eyeZ * vp.vrScale);
				Vector viewCenter = hmdPosAbs + (hmdForward * (-eyeZSu));
				Vector viewLeft = viewCenter + (hmdRight * (-(ipdSu * 0.5f)));
				Vector viewRight = viewCenter + (hmdRight * (+(ipdSu * 0.5f)));

				// Controllers + viewmodel (visual only, no gameplay auto-aim overrides here).
				vr::TrackedDeviceIndex_t leftIdx = m_VR->m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
				vr::TrackedDeviceIndex_t rightIdx = m_VR->m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
				if (m_VR->m_LeftHanded)
				{
					std::swap(leftIdx, rightIdx);
				}

				Vector leftCtrlPosAbs = m_VR->m_LeftControllerPosAbs;
				QAngle leftCtrlAngAbs = m_VR->m_LeftControllerAngAbs;
				Vector rightCtrlPosAbs = m_VR->m_RightControllerPosAbs;
				Vector rightCtrlViewmodelPosAbs = rightCtrlPosAbs;
				QAngle rightCtrlAngAbs = m_VR->m_RightControllerAngAbs;
				Vector vmForward = m_VR->m_ViewmodelForward;
				Vector vmRight = m_VR->m_ViewmodelRight;
				Vector vmUp = m_VR->m_ViewmodelUp;
				Vector vmPosAbs = m_VR->GetRecommendedViewmodelAbsPos();
				QAngle vmAngAbs = m_VR->GetRecommendedViewmodelAbsAngle();
				Vector rightCtrlForward{};
				Vector rightCtrlRight{};
				Vector rightCtrlUp{};
				QAngle::AngleVectors(rightCtrlAngAbs, &rightCtrlForward, &rightCtrlRight, &rightCtrlUp);
				bool rightCtrlBasisValid =
					VectorNormalize(rightCtrlForward) != 0.0f &&
					VectorNormalize(rightCtrlRight) != 0.0f &&
					VectorNormalize(rightCtrlUp) != 0.0f;

				auto buildRenderControllerBasis = [&](const vr::TrackedDevicePose_t& pose, Vector& posLocal, Vector& ctrlF, Vector& ctrlR, Vector& ctrlU) -> bool
					{
						if (!pose.bPoseIsValid)
							return false;

						const vr::HmdMatrix34_t& mat = pose.mDeviceToAbsoluteTracking;
						posLocal.x = -mat.m[2][3];
						posLocal.y = -mat.m[0][3];
						posLocal.z = mat.m[1][3];

						// Build the controller basis directly from the OpenVR pose matrix.
						// The old queued path converted matrix -> Euler -> basis every render frame.
						// Near Source QAngle singularities, tiny runtime pose noise can become large
						// yaw/roll flips, which moves the viewmodel even when the controller is still.
						ctrlF = Vector(mat.m[2][2], mat.m[0][2], -mat.m[1][2]);
						ctrlR = Vector(-mat.m[2][0], -mat.m[0][0], mat.m[1][0]);
						ctrlU = Vector(-mat.m[2][1], -mat.m[0][1], mat.m[1][1]);

						if (VectorNormalize(ctrlF) == 0.0f || VectorNormalize(ctrlR) == 0.0f || VectorNormalize(ctrlU) == 0.0f)
							return false;

						if (std::fabs(extrapRot) > 0.0001f)
						{
							const Vector worldUp(0.0f, 0.0f, 1.0f);
							ctrlF = VectorRotate(ctrlF, worldUp, extrapRot);
							ctrlR = VectorRotate(ctrlR, worldUp, extrapRot);
							ctrlU = VectorRotate(ctrlU, worldUp, extrapRot);
						}

						return true;
					};

				if (leftIdx != vr::k_unTrackedDeviceIndexInvalid && leftIdx < vr::k_unMaxTrackedDeviceCount && renderPoses[leftIdx].bPoseIsValid)
				{
					Vector ctrlPosLocal{}, ctrlF{}, ctrlR{}, ctrlU{};
					if (buildRenderControllerBasis(renderPoses[leftIdx], ctrlPosLocal, ctrlF, ctrlR, ctrlU))
					{
						Vector hmdToCtrl = ctrlPosLocal - hmdPosLocal;
						Vector ctrlPosCorrected = hmdPosCorrected + hmdToCtrl;
						VectorPivotXY(ctrlPosCorrected, hmdPosCorrected, extrapRot);

						ctrlF = VectorRotate(ctrlF, ctrlR, -45.0);
						ctrlU = VectorRotate(ctrlU, ctrlR, -45.0);

						leftCtrlPosAbs = cameraAnchor - Vector(0, 0, 64) + (ctrlPosCorrected * vp.vrScale);
						QAngle::VectorAngles(ctrlF, ctrlU, leftCtrlAngAbs);
					}
				}

				if (rightIdx != vr::k_unTrackedDeviceIndexInvalid && rightIdx < vr::k_unMaxTrackedDeviceCount && renderPoses[rightIdx].bPoseIsValid)
				{
					Vector ctrlPosLocal{}, ctrlF{}, ctrlR{}, ctrlU{};
					if (buildRenderControllerBasis(renderPoses[rightIdx], ctrlPosLocal, ctrlF, ctrlR, ctrlU))
					{
						Vector hmdToCtrl = ctrlPosLocal - hmdPosLocal;
						Vector ctrlPosCorrected = hmdPosCorrected + hmdToCtrl;
						VectorPivotXY(ctrlPosCorrected, hmdPosCorrected, extrapRot);

						// 45° downward tilt, matches main tracking path.
						ctrlF = VectorRotate(ctrlF, ctrlR, -45.0);
						ctrlU = VectorRotate(ctrlU, ctrlR, -45.0);

						rightCtrlPosAbs = cameraAnchor - Vector(0, 0, 64) + (ctrlPosCorrected * vp.vrScale);
						rightCtrlViewmodelPosAbs = rightCtrlPosAbs;
						QAngle::VectorAngles(ctrlF, ctrlU, rightCtrlAngAbs);
						rightCtrlForward = ctrlF;
						rightCtrlRight = ctrlR;
						rightCtrlUp = ctrlU;
						rightCtrlBasisValid = true;
					}
				}

				if (rightCtrlBasisValid)
				{
					Vector aimForward = rightCtrlForward;
					Vector aimRight = rightCtrlRight;
					Vector aimUp = rightCtrlUp;
					if (m_VR->ResolvePavlovTwoHandedAimBasis(
						leftCtrlPosAbs,
						rightCtrlPosAbs,
						rightCtrlForward,
						rightCtrlRight,
						rightCtrlUp,
						hmdPosAbs,
						hmdForward,
						hmdRight,
						hmdUp,
						vp.vrScale,
						aimForward,
						aimRight,
						aimUp))
					{
						QAngle::VectorAngles(aimForward, aimUp, rightCtrlAngAbs);
					}

					// Viewmodel basis from effective weapon aim + per-weapon offsets.
					vmForward = aimForward;
					vmRight = aimRight;
					vmUp = aimUp;
					// Yaw offset
					vmForward = VectorRotate(vmForward, vmUp, vp.viewmodelAngOffset.y);
					vmRight = VectorRotate(vmRight, vmUp, vp.viewmodelAngOffset.y);
					// Pitch offset
					vmForward = VectorRotate(vmForward, vmRight, vp.viewmodelAngOffset.x);
					vmUp = VectorRotate(vmUp, vmRight, vp.viewmodelAngOffset.x);
					// Roll offset
					vmRight = VectorRotate(vmRight, vmForward, vp.viewmodelAngOffset.z);
					vmUp = VectorRotate(vmUp, vmForward, vp.viewmodelAngOffset.z);

					vmPosAbs = rightCtrlViewmodelPosAbs
						- (vmForward * vp.viewmodelPosOffset.x)
						- (vmRight * vp.viewmodelPosOffset.y)
						- (vmUp * vp.viewmodelPosOffset.z);
					QAngle::VectorAngles(vmForward, vmUp, vmAngAbs);
				}

				// Publish render-frame snapshot with a seqlock.
				uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
				m_VR->m_RenderFrameSeq.store(seq + 1, std::memory_order_release);

				m_VR->m_RenderViewAngX.store(hmdAngLocal.x, std::memory_order_relaxed);
				m_VR->m_RenderViewAngY.store(hmdAngLocal.y, std::memory_order_relaxed);
				m_VR->m_RenderViewAngZ.store(hmdAngLocal.z, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginLeftX.store(viewLeft.x, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginLeftY.store(viewLeft.y, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginLeftZ.store(viewLeft.z, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginRightX.store(viewRight.x, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginRightY.store(viewRight.y, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginRightZ.store(viewRight.z, std::memory_order_relaxed);
				m_VR->m_RenderLeftControllerPosAbsX.store(leftCtrlPosAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderLeftControllerPosAbsY.store(leftCtrlPosAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderLeftControllerPosAbsZ.store(leftCtrlPosAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderLeftControllerAngAbsX.store(leftCtrlAngAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderLeftControllerAngAbsY.store(leftCtrlAngAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderLeftControllerAngAbsZ.store(leftCtrlAngAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerPosAbsX.store(rightCtrlPosAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerPosAbsY.store(rightCtrlPosAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerPosAbsZ.store(rightCtrlPosAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerAngAbsX.store(rightCtrlAngAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerAngAbsY.store(rightCtrlAngAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerAngAbsZ.store(rightCtrlAngAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelPosX.store(vmPosAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelPosY.store(vmPosAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelPosZ.store(vmPosAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelAngX.store(vmAngAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelAngY.store(vmAngAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelAngZ.store(vmAngAbs.z, std::memory_order_relaxed);

				m_VR->m_RenderFrameSeq.store(seq + 2, std::memory_order_release);
			}
		}
	}

	// ------------------------------
	// Third-person camera fix:
	// If engine is in third-person, setup.origin is a shoulder camera,
	// but our VR hook normally overwrites it with HMD first-person.
	// That makes the local player model show up "in your face" and looks like ghosting/double image.
	//
	// IMPORTANT (thread-safety):
	// In mat_queue_mode!=0, this hook runs on the render thread. Do NOT touch engine/client entity state
	// (GetLocalPlayer/GetClientEntity/ReadNetvar/GetActiveWeapon/TraceRay/etc) here.
	// Instead, consume the update-thread seqlock snapshot (vp.*) published by VR::UpdateTracking.
	// ------------------------------
	C_BasePlayer* localPlayer = nullptr;
	bool localPlayerValid = false;
	bool hasViewEntityOverride = false;
	Vector eyeOrigin = setup.origin;

	bool revivingOther = false;
	bool beingRevived = false;
	bool usingMountedGun = false;
	bool playerIncap = false;
	bool inMapLoadCooldown = false;

	ThirdPersonStateDebug tpStateDbg{};
	bool rawStateWantsThirdPerson = false;
	bool rawStateObserver = false;

	if (queueMode == 0)
	{
		if (m_Game && m_Game->m_EngineClient)
		{
			int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
			localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
		}
		localPlayerValid = (localPlayer != nullptr);
		hasViewEntityOverride = (localPlayer && HandleValid(ReadNetvar<int>(localPlayer, 0x142c))); // m_hViewEntity

		if (localPlayer)
			eyeOrigin = localPlayer->EyePosition();

		// Revive state
		if (localPlayer)
		{
			const int reviveOwner = ReadNetvar<int>(localPlayer, 0x1f88);   // m_reviveOwner (someone reviving you)
			const int reviveTarget = ReadNetvar<int>(localPlayer, 0x1f8c);  // m_reviveTarget (you reviving someone)
			beingRevived = HandleValid(reviveOwner);
			revivingOther = HandleValid(reviveTarget);
		}

		usingMountedGun = m_VR->IsUsingMountedGun(localPlayer);
		if (localPlayer)
			playerIncap = (ReadNetvar<int>(localPlayer, 0x1ea9) != 0); // m_isIncapacitated

		// Also expose a simple "player is pinned/controlled" flag so VR can disable jittery aim line.
		m_VR->m_PlayerControlledBySI = IsPlayerControlledBySI(localPlayer);

		rawStateWantsThirdPerson = ShouldForceThirdPersonByState(localPlayer, m_Game->m_ClientEntityList, localPlayer, &tpStateDbg);
		rawStateObserver = (tpStateDbg.observerMode != 0) && (tpStateDbg.dead || HandleValid(tpStateDbg.observerTarget));
		inMapLoadCooldown = m_VR->IsThirdPersonMapLoadCooldownActive();
	}
	else
	{
		// Render thread (queued rendering): consume update-thread snapshot only.
			// NOTE: This scope is outside the earlier "ViewParams vp" snapshot reader.
			// Read the same atomic snapshot directly.
		localPlayerValid = (m_VR->m_RenderHasLocalPlayer.load(std::memory_order_relaxed) != 0);
		hasViewEntityOverride = (m_VR->m_RenderHasViewEntityOverride.load(std::memory_order_relaxed) != 0);
		eyeOrigin.x = m_VR->m_RenderLocalEyePosX.load(std::memory_order_relaxed);
		eyeOrigin.y = m_VR->m_RenderLocalEyePosY.load(std::memory_order_relaxed);
		eyeOrigin.z = m_VR->m_RenderLocalEyePosZ.load(std::memory_order_relaxed);
		beingRevived = (m_VR->m_RenderBeingRevived.load(std::memory_order_relaxed) != 0);
		revivingOther = (m_VR->m_RenderRevivingOther.load(std::memory_order_relaxed) != 0);
		usingMountedGun = (m_VR->m_RenderUsingMountedGun.load(std::memory_order_relaxed) != 0);
		playerIncap = (m_VR->m_RenderPlayerIncap.load(std::memory_order_relaxed) != 0);
		inMapLoadCooldown = (m_VR->m_RenderInThirdPersonMapLoadCooldown.load(std::memory_order_relaxed) != 0);

		tpStateDbg.dead = (m_VR->m_RenderTpDead.load(std::memory_order_relaxed) != 0);
		tpStateDbg.lifeState = m_VR->m_RenderTpLifeState.load(std::memory_order_relaxed);
		tpStateDbg.observerMode = m_VR->m_RenderTpObserverMode.load(std::memory_order_relaxed);
		tpStateDbg.observerTarget = m_VR->m_RenderTpObserverTarget.load(std::memory_order_relaxed);
		tpStateDbg.incap = (m_VR->m_RenderTpIncap.load(std::memory_order_relaxed) != 0);
		tpStateDbg.ledge = (m_VR->m_RenderTpLedge.load(std::memory_order_relaxed) != 0);
		tpStateDbg.tongue = (m_VR->m_RenderTpTongue.load(std::memory_order_relaxed) != 0);
		tpStateDbg.pinned = (m_VR->m_RenderTpPinned.load(std::memory_order_relaxed) != 0);
		tpStateDbg.selfMedkit = (m_VR->m_RenderTpSelfMedkit.load(std::memory_order_relaxed) != 0);

		rawStateWantsThirdPerson = (m_VR->m_RenderTpWantsThirdPerson.load(std::memory_order_relaxed) != 0);
		rawStateObserver = (m_VR->m_RenderTpObserver.load(std::memory_order_relaxed) != 0);
	}

	// Heuristic: in true third-person, the engine camera origin is noticeably away from eye position.
	// IMPORTANT: stairs/step-smoothing can create large Z deltas between setup.origin and EyePosition().
	// So prefer XY distance for "real" third-person detection.
	Vector camDelta = (setup.origin - eyeOrigin);
	const float camDist3D = camDelta.Length();
	camDelta.z = 0.0f;
	const float camDistXY = camDelta.Length();

	// - XY threshold must be low enough to catch "near" third-person modes,
	//   but still high enough to ignore stairs/step-smoothing Z deltas.
	// - 3D is a fallback for edge cases.
	constexpr float kThirdPersonXY = 20.0f;
	constexpr float kThirdPerson3D = 90.0f;

	// Heuristic (smoothed): low-pass filter the delta + apply hysteresis before deciding "engine third-person".
	static bool s_engineTpDetectInit = false;
	static Vector s_engineTpDeltaEmaXY{ 0,0,0 };
	static float s_engineTpDistEma3D = 0.0f;
	static bool s_engineTpLatched = false;

	auto ResetEngineTpDetect = [&]()
		{
			s_engineTpDetectInit = false;
			s_engineTpDeltaEmaXY = { 0,0,0 };
			s_engineTpDistEma3D = 0.0f;
			s_engineTpLatched = false;
		};

	if (!localPlayerValid)
	{
		ResetEngineTpDetect();
	}
	else
	{
		// 0=no smoothing (instant), 0.9=heavy smoothing.
		constexpr float kDetectSmooth = 0.70f;
		const float alpha = 1.0f - kDetectSmooth;

		if (!s_engineTpDetectInit)
		{
			s_engineTpDeltaEmaXY = camDelta; // camDelta is XY-only
			s_engineTpDistEma3D = camDist3D;
			s_engineTpDetectInit = true;
		}
		else
		{
			s_engineTpDeltaEmaXY.x += (camDelta.x - s_engineTpDeltaEmaXY.x) * alpha;
			s_engineTpDeltaEmaXY.y += (camDelta.y - s_engineTpDeltaEmaXY.y) * alpha;
			s_engineTpDeltaEmaXY.z = 0.0f;
			s_engineTpDistEma3D += (camDist3D - s_engineTpDistEma3D) * alpha;
		}

		const float emaDistXY = s_engineTpDeltaEmaXY.Length();
		// Hysteresis: use a lower threshold to exit to prevent oscillation.
		constexpr float kThirdPersonXY_On = kThirdPersonXY;
		constexpr float kThirdPersonXY_Off = kThirdPersonXY * 0.75f;
		constexpr float kThirdPerson3D_On = kThirdPerson3D;
		constexpr float kThirdPerson3D_Off = kThirdPerson3D * 0.78f;

		if (!s_engineTpLatched)
		{
			if (emaDistXY > kThirdPersonXY_On || s_engineTpDistEma3D > kThirdPerson3D_On)
				s_engineTpLatched = true;
		}
		else
		{
			if (emaDistXY < kThirdPersonXY_Off && s_engineTpDistEma3D < kThirdPerson3D_Off)
				s_engineTpLatched = false;
		}
	}

	bool engineThirdPersonNow = (localPlayerValid && s_engineTpLatched);

	// Only force TP when reviving others. Being revived keeps current FP/TP decision.
	if (revivingOther)
		engineThirdPersonNow = true;

	// Mounted gun (.50cal/minigun): always force first-person rendering.
	if (usingMountedGun)
	{
		engineThirdPersonNow = false;
		ResetEngineTpDetect();
	}

	const bool customWalkThirdPersonNow = m_VR->m_ThirdPersonRenderOnCustomWalk && m_VR->m_CustomWalkHeld;

	// Some scripts/mods use point_viewcontrol_survivor via m_hViewEntity.
	// Only ignore view-entity overrides during unstable INCAP windows.
	if (hasViewEntityOverride && !customWalkThirdPersonNow)
	{
		const bool unstableViewEntity = playerIncap;
		if (unstableViewEntity)
		{
			engineThirdPersonNow = false;
			ResetEngineTpDetect();
		}
	}

	QAngle rawSetupAngles(setup.angles.x, setup.angles.y, setup.angles.z);
	// Capture and optionally smooth the engine camera (tick-rate 3P -> HMD-rate continuous).
	if (engineThirdPersonNow)
		s_engineTpCam.PushRaw(setup.origin, rawSetupAngles);
	else
		s_engineTpCam.Reset();

	Vector engineCamOrigin = setup.origin;
	QAngle engineCamAngles = rawSetupAngles;
	if (s_engineTpCam.ShouldSmooth())
		s_engineTpCam.GetSmoothed(engineCamOrigin, engineCamAngles);

	// State-based third-person (dead/observer/ledge/tongue/pinned/self-heal)
	bool stateWantsThirdPerson = rawStateWantsThirdPerson;
	bool stateObserver = rawStateObserver;

	// Map-load / reconnect stabilization:
	// Suppress *observer-driven* third-person in that window; other state reasons still apply.
	if (inMapLoadCooldown && tpStateDbg.lifeState == 0 && rawStateObserver && !tpStateDbg.dead)
	{
		stateObserver = false;
		stateWantsThirdPerson = tpStateDbg.dead || tpStateDbg.ledge || tpStateDbg.tongue || tpStateDbg.pinned || tpStateDbg.selfMedkit;
	}

	// Observer render lock based on m_iObserverMode (prevents 1P/3P oscillation -> flash while spectating).
	enum class ObserverRenderPref : int { None = 0, InEye = 1, Third = 2 };
	static ObserverRenderPref s_obsPref = ObserverRenderPref::None;
	static ObserverRenderPref s_obsCandidate = ObserverRenderPref::None;
	static int s_obsCandidateFrames = 0;
	constexpr int kObserverPrefLatchFrames = 6;

	auto PrefFromObserverMode = [](int obsMode) -> ObserverRenderPref
		{
			if (obsMode == 4) return ObserverRenderPref::InEye;
			if (obsMode == 5 || obsMode == 6) return ObserverRenderPref::Third;
			return ObserverRenderPref::None;
		};

	if (!stateObserver)
	{
		s_obsPref = ObserverRenderPref::None;
		s_obsCandidate = ObserverRenderPref::None;
		s_obsCandidateFrames = 0;
	}
	else
	{
		const ObserverRenderPref desired = PrefFromObserverMode(tpStateDbg.observerMode);

		// Conservative default on entry: render 3P until we see a stable 4/5/6.
		if (s_obsPref == ObserverRenderPref::None && desired == ObserverRenderPref::None)
			s_obsPref = ObserverRenderPref::Third;

		if (desired != ObserverRenderPref::None)
		{
			if (desired != s_obsCandidate)
			{
				s_obsCandidate = desired;
				s_obsCandidateFrames = 0;
			}
			else
			{
				++s_obsCandidateFrames;
				if (s_obsCandidateFrames >= kObserverPrefLatchFrames)
				{
					s_obsPref = s_obsCandidate;
					s_obsCandidate = ObserverRenderPref::None;
					s_obsCandidateFrames = 0;
				}
			}
		}
		else
		{
			s_obsCandidate = ObserverRenderPref::None;
			s_obsCandidateFrames = 0;
		}
	}

	const bool observerForceInEye = stateObserver && (s_obsPref == ObserverRenderPref::InEye);
	const bool observerForceThird = stateObserver && (s_obsPref == ObserverRenderPref::Third);

	// If observer mode requests in-eye, don't treat "observer" as a reason to force third-person.
	if (observerForceInEye)
		stateWantsThirdPerson = tpStateDbg.dead || tpStateDbg.ledge || tpStateDbg.tongue || tpStateDbg.pinned || tpStateDbg.selfMedkit;

	const bool stateIsDeadOrObserver = tpStateDbg.dead || (stateObserver && !observerForceInEye);

	// Death transition anti-flicker: lock to first-person for a short window right after death.
	// Source can move through observer/deathcam modes during the same transition, so this
	// lock must override observer render preferences too.
	static std::chrono::steady_clock::time_point s_deathFpLockUntil{};
	static bool s_prevDead = false;
	const bool nowDead = tpStateDbg.dead;
	const auto nowTp = std::chrono::steady_clock::now();

	// Revive recovery: getting up from incapacitated can leave the engine in a transient camera mode.
	static bool s_prevIncap = false;
	static bool s_incapEnteredThirdPerson = false;
	static bool s_reviveForceFirstPerson = true;
	static std::chrono::steady_clock::time_point s_reviveRecoverUntil{};
	const bool nowIncap = tpStateDbg.incap;
	if (!s_prevIncap && nowIncap)
	{
		const bool prevHadThirdPerson = m_VR->m_IsThirdPersonCamera || (m_VR->m_ThirdPersonHoldFrames > 0);
		s_incapEnteredThirdPerson = prevHadThirdPerson || customWalkThirdPersonNow || engineThirdPersonNow;
	}
	if (s_prevIncap && !nowIncap)
	{
		s_reviveForceFirstPerson = !s_incapEnteredThirdPerson;
		if (s_reviveForceFirstPerson)
			s_reviveRecoverUntil = nowTp + std::chrono::milliseconds(400);
		else
			s_reviveRecoverUntil = std::chrono::steady_clock::time_point{};
		m_VR->m_ThirdPersonHoldFrames = 0;
		m_VR->m_IsThirdPersonCamera = false;
		s_engineTpCam.Reset();
		if (queueMode == 0)
			m_VR->ResetPosition();
	}
	s_prevIncap = nowIncap;
	const bool inReviveRecover = (s_reviveRecoverUntil.time_since_epoch().count() != 0) && (nowTp < s_reviveRecoverUntil);

	if (nowDead && !s_prevDead)
		s_deathFpLockUntil = nowTp + std::chrono::seconds(10);
	s_prevDead = nowDead;
	const bool renderDeathFpLockActive =
		(s_deathFpLockUntil.time_since_epoch().count() != 0) && (nowTp < s_deathFpLockUntil);
	const bool forceFirstPersonAfterDeath = renderDeathFpLockActive || m_VR->IsDeathFirstPersonLockActive();

	constexpr int kEngineThirdPersonHoldFrames = 2;
	constexpr int kStateThirdPersonHoldFrames = 2;
	constexpr int kSelfMedkitHoldFrames = 6;
	constexpr int kDeadOrObserverHoldFrames = 30;
	const bool hadThirdPerson = m_VR->m_IsThirdPersonCamera || (m_VR->m_ThirdPersonHoldFrames > 0);
	bool allowStateThirdPerson = stateWantsThirdPerson && (stateIsDeadOrObserver || tpStateDbg.selfMedkit || engineThirdPersonNow || customWalkThirdPersonNow || hadThirdPerson);
	if (forceFirstPersonAfterDeath)
		allowStateThirdPerson = false;

	if (allowStateThirdPerson)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kStateThirdPersonHoldFrames);
	if (tpStateDbg.selfMedkit)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kSelfMedkitHoldFrames);
	if (stateIsDeadOrObserver)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kDeadOrObserverHoldFrames);

	constexpr int kWalkThirdPersonHoldFrames = 3;
	if (customWalkThirdPersonNow)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kWalkThirdPersonHoldFrames);

	if (engineThirdPersonNow)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kEngineThirdPersonHoldFrames);
	else if (localPlayerValid && !allowStateThirdPerson && !tpStateDbg.selfMedkit && m_VR->m_ThirdPersonHoldFrames > 0)
		m_VR->m_ThirdPersonHoldFrames--;

	const bool defaultThirdPersonNow = m_VR->m_ThirdPersonDefault && !stateIsDeadOrObserver && !hasViewEntityOverride;
	bool renderThirdPerson = defaultThirdPersonNow || customWalkThirdPersonNow || engineThirdPersonNow || tpStateDbg.selfMedkit || (m_VR->m_ThirdPersonHoldFrames > 0);
	if (usingMountedGun)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
	}
	if (inReviveRecover && s_reviveForceFirstPerson && !usingMountedGun && !customWalkThirdPersonNow && !stateIsDeadOrObserver && !tpStateDbg.selfMedkit)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
		s_engineTpCam.Reset();
	}
	if (observerForceInEye)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
		s_engineTpCam.Reset();
	}
	else if (observerForceThird)
	{
		renderThirdPerson = true;
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kDeadOrObserverHoldFrames);
	}
	if (forceFirstPersonAfterDeath)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
		s_engineTpCam.Reset();
	}

	// Expose third-person camera to VR helpers (aim line, overlays, etc.)
	m_VR->m_IsThirdPersonCamera = renderThirdPerson;

	// ------------------------------
	// Third-person shake damping:
	// Tank stomps / explosions can apply strong screen-shake to the engine camera.
	// In VR third-person, that feels *way* worse than on a flat screen, so we apply an
	// extra low-pass filter to the engine camera origin/angles while actively rendering 3P.
	//
	// Notes:
	//  - Disabled for death/observer and view-entity cameras (cutscenes), to preserve intended choreography.
	//  - Controlled by config: ThirdPersonCameraSmoothing (0..0.99). Higher = smoother (less shake).
	// ------------------------------
	static bool s_tpShakeInit = false;
	static Vector s_tpShakeOrigin{ 0,0,0 };
	static QAngle s_tpShakeAngles{ 0,0,0 };

	auto ResetTpShake = [&]()
		{
			s_tpShakeInit = false;
			s_tpShakeOrigin = { 0,0,0 };
			s_tpShakeAngles = { 0,0,0 };
		};

	const float tpShakeSmooth = std::clamp(m_VR->m_ThirdPersonCameraSmoothing, 0.0f, 0.99f);

	if (!renderThirdPerson || stateIsDeadOrObserver || hasViewEntityOverride || tpShakeSmooth <= 0.0f)
	{
		ResetTpShake();
	}
	else
	{
		const float lerpFactor = 1.0f - tpShakeSmooth;

		if (!s_tpShakeInit)
		{
			s_tpShakeOrigin = engineCamOrigin;
			s_tpShakeAngles = engineCamAngles;
			s_tpShakeInit = true;
		}
		else
		{
			// Smooth origin (component-wise)
			s_tpShakeOrigin.x += (engineCamOrigin.x - s_tpShakeOrigin.x) * lerpFactor;
			s_tpShakeOrigin.y += (engineCamOrigin.y - s_tpShakeOrigin.y) * lerpFactor;
			s_tpShakeOrigin.z += (engineCamOrigin.z - s_tpShakeOrigin.z) * lerpFactor;

			// Smooth angles with wrap-around
			auto smoothAngle = [&](float target, float& cur)
				{
					float diff = target - cur;
					diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
					cur += diff * lerpFactor;
				};

			smoothAngle(engineCamAngles.x, s_tpShakeAngles.x);
			smoothAngle(engineCamAngles.y, s_tpShakeAngles.y);
			smoothAngle(engineCamAngles.z, s_tpShakeAngles.z);
		}

		engineCamOrigin = s_tpShakeOrigin;
		engineCamAngles = s_tpShakeAngles;
	}

	// Always capture the view the engine is rendering this frame.
	// In true third-person, setup.origin is the shoulder camera; in first-person it matches the eye.
	m_VR->m_ThirdPersonViewOrigin = engineCamOrigin;
	m_VR->m_ThirdPersonViewAngles.Init(engineCamAngles.x, engineCamAngles.y, engineCamAngles.z);
	CViewSetup leftEyeView = setup;
	CViewSetup rightEyeView = setup;

	auto NormalizeViewSetupForVREye = [&](CViewSetup& view)
		{
			const int eyeWidth = static_cast<int>(m_VR->m_RenderWidth);
			const int eyeHeight = static_cast<int>(m_VR->m_RenderHeight);

			// Source keeps both scaled and unscaled viewport fields in CViewSetup.
			// If the unscaled fields are left at the desktop/backbuffer size,
			// queued rendering can intermittently render an eye RT through a desktop-sized
			// viewport after VGUI/HUD capture or a resolution switch. That shows up as
			// flicker/misalignment once the desktop mirror is larger than 1080p.
			view.x = 0;
			view.y = 0;
			view.m_nUnscaledX = 0;
			view.m_nUnscaledY = 0;
			view.width = eyeWidth;
			view.height = eyeHeight;
			view.m_nUnscaledWidth = eyeWidth;
			view.m_nUnscaledHeight = eyeHeight;
			view.fov = m_VR->m_Fov;
			view.fovViewmodel = m_VR->m_Fov;
			view.m_flAspectRatio = m_VR->m_Aspect;
			view.zNear = 6;
			view.zNearViewmodel = 6;
		};

	NormalizeViewSetupForVREye(leftEyeView);
	NormalizeViewSetupForVREye(rightEyeView);
	// Keep VR tracking base tied to the real player eye, NOT the shoulder camera.
	// IMPORTANT (VR compatibility):
	// Some VScript mods (e.g. slide mods) temporarily enable point_viewcontrol_survivor via
	// CBasePlayer::m_hViewEntity. In that case, setup.origin can jump to an attachment-driven
	// camera that does NOT match the HMD eye origin and can appear "too high" in VR.
	// So: only borrow setup.origin.z when the player has no active view-entity override.
	m_VR->m_SetupOrigin = eyeOrigin;
	{
		static bool s_stepZSmoothValid = false;
		static float s_stepZSmooth = 0.0f;
		static std::chrono::steady_clock::time_point s_stepZSmoothLastT{};
		const bool useEngineStepZ = !renderThirdPerson && !hasViewEntityOverride && !usingMountedGun;
		if (useEngineStepZ)
		{
			const float rawStepZ = setup.origin.z;
			const int smoothMs = std::max(0, m_VR->m_StairStepCameraSmoothMs);
			if (smoothMs <= 0)
			{
				s_stepZSmooth = rawStepZ;
				s_stepZSmoothValid = true;
			}
			else if (!s_stepZSmoothValid || std::fabs(rawStepZ - s_stepZSmooth) > 48.0f)
			{
				s_stepZSmooth = rawStepZ;
				s_stepZSmoothValid = true;
				s_stepZSmoothLastT = std::chrono::steady_clock::now();
			}
			else
			{
				const auto now = std::chrono::steady_clock::now();
				float dt = 0.0f;
				if (s_stepZSmoothLastT.time_since_epoch().count() != 0)
					dt = std::chrono::duration<float>(now - s_stepZSmoothLastT).count();
				s_stepZSmoothLastT = now;
				dt = std::clamp(dt, 0.0f, 0.050f);
				const float tau = static_cast<float>(smoothMs) / 1000.0f;
				const float alpha = (tau > 0.0f) ? std::clamp(1.0f - std::exp(-dt / tau), 0.0f, 1.0f) : 1.0f;
				s_stepZSmooth += (rawStepZ - s_stepZSmooth) * alpha;
			}
			m_VR->m_SetupOrigin.z = s_stepZSmooth;
		}
		else
		{
			s_stepZSmoothValid = false;
			s_stepZSmoothLastT = {};
		}
	}
	m_VR->m_SetupAngles.Init(setup.angles.x, setup.angles.y, setup.angles.z);

	Vector leftOrigin, rightOrigin;
	Vector viewAngles = m_VR->GetViewAngle();
	Vector renderViewAngles = viewAngles;
	const bool thirdPersonFrontViewActive = renderThirdPerson
		&& m_VR->m_ThirdPersonFrontViewEnabled
		&& !hasViewEntityOverride
		&& !stateIsDeadOrObserver;
	auto wrapYawDeg = [](float yaw)
		{
			yaw -= 360.0f * std::floor((yaw + 180.0f) / 360.0f);
			return yaw;
		};

	// Recenter the VR anchors once per threshold when yaw turns left/right a lot.
	// Requirement: if yaw turns beyond 60° (left or right), do a one-shot ResetPosition.
	// Note: this now applies in both first-person and third-person rendering.
	{
		static bool s_yawResetInit = false;
		static float s_yawResetBase = 0.0f;
		const float bodyYaw = m_VR->m_RotationOffset; // wrapped to [0, 360)

		if (!s_yawResetInit)
		{
			s_yawResetBase = bodyYaw;
			s_yawResetInit = true;
		}
		else
		{
			float diff = bodyYaw - s_yawResetBase;
			// Normalize to [-180, 180] to handle wrap-around.
			diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
			if (std::fabs(diff) >= 60.0f)
			{
				m_VR->ResetPosition();
				s_yawResetBase = bodyYaw;
			}
		}
	}


	// ------------------------------
	// Third-person wall collision (camera push-in):
	// When we synthesize/offset a 3P camera (especially state-forced 3P),
	// the desired camera center can end up behind world geometry.
	// Trace a small hull from the anchor to the desired camera and clamp the distance
	// so the camera never passes through walls.
	// We snap *in* immediately on collision, and ease *out* to avoid popping.
	// Disabled for scripted view-entity cameras (point_viewcontrol_survivor) to preserve choreography.
	// ------------------------------
	static bool s_tpWallCollInit = false;
	static float s_tpWallCollDist = 0.0f;
	auto ResetTpWallColl = [&]() { s_tpWallCollInit = false; s_tpWallCollDist = 0.0f; };
	if (!renderThirdPerson || hasViewEntityOverride || !m_Game || !m_Game->m_EngineTrace)
		ResetTpWallColl();
	if (renderThirdPerson)
	{
		// Third-person uses two different angle bases:
		//  1) renderCamAng: the orientation the user actually looks through this frame.
		//     This should continue to follow the HMD so head turning still looks around naturally.
		//  2) cameraBasisAng: the basis used to place the third-person camera center behind/in front of
		//     the player. When decoupled from HMD, head turning no longer drags the whole third-person
		//     camera position/orbit, while roomscale translation still moves it.
		QAngle renderCamAng(viewAngles.x, viewAngles.y, viewAngles.z);
		if (m_VR->m_HmdForward.IsZero())
			renderCamAng = engineCamAngles;

		QAngle cameraBasisAng = renderCamAng;
		if (!m_VR->m_ThirdPersonCameraFollowHmd || m_VR->m_HmdForward.IsZero())
			cameraBasisAng = engineCamAngles;

		if (thirdPersonFrontViewActive)
		{
			// In front-view mode, keep the main third-person camera yaw aligned with the scope yaw
			// so thumbstick/scope turning also recenters the character in the main view.
			float frontYaw = 0.0f;
			if (m_VR->ShouldRenderScope())
			{
				frontYaw = m_VR->GetScopeCameraAbsAngle().y;
			}
			else
			{
				frontYaw = m_VR->m_RotationOffset;
				frontYaw -= 360.0f * std::floor((frontYaw + 180.0f) / 360.0f);
			}

			frontYaw = wrapYawDeg(frontYaw + 180.0f);
			renderCamAng.y = frontYaw;
			cameraBasisAng.y = frontYaw;
		}

		renderViewAngles.x = renderCamAng.x;
		renderViewAngles.y = renderCamAng.y;
		renderViewAngles.z = renderCamAng.z;

		Vector renderRight;
		QAngle::AngleVectors(renderCamAng, nullptr, &renderRight, nullptr);

		Vector basisFwd, basisRight, basisUp;
		QAngle::AngleVectors(cameraBasisAng, &basisFwd, &basisRight, &basisUp);

		const float ipd = (m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale);
		const float eyeZ = (m_VR->m_EyeZ * m_VR->m_VRScale);

		// Treat camera origin as "head center", apply SteamVR eye-to-head offsets.
		// If we're forcing third-person (state) while the engine is in first-person, use HMD position to synthesize a stable 3p camera.
		// IMPORTANT:
		// engineThirdPersonNow can flicker during pinned/incap/use actions.
		// If stateWantsThirdPerson is true, always synthesize from HMD to avoid camera "jumping"
		// between setup.origin and HmdPosAbs.
		Vector baseCenter;
		if (stateWantsThirdPerson)
		{
			// Dead/observer camera must follow engine view, not HMD position.
			baseCenter = stateIsDeadOrObserver ? engineCamOrigin : m_VR->m_HmdPosAbs;
		}
		else
		{
			baseCenter = (engineThirdPersonNow || customWalkThirdPersonNow) ? engineCamOrigin : m_VR->m_HmdPosAbs;
		}
		Vector camCenter = baseCenter + (basisFwd * (-eyeZ));
		if (thirdPersonFrontViewActive)
		{
			const Vector& configuredOffset = m_VR->m_ThirdPersonFrontVRCameraOffset;
			camCenter = camCenter
				+ (basisFwd * (-configuredOffset.x))
				+ (basisRight * configuredOffset.y)
				+ (basisUp * configuredOffset.z);
		}
		else if (m_VR->m_ThirdPersonVRCameraOffset > 0.0f)
		{
			camCenter = camCenter + (basisFwd * (-m_VR->m_ThirdPersonVRCameraOffset));
		}
		// Camera collision: clamp camera distance when something blocks the line from the anchor to the desired camera.
		// This prevents the third-person render camera from going through walls (common when using ThirdPersonVRCameraOffset).
		if (queueMode == 0 && m_Game && m_Game->m_EngineTrace && !hasViewEntityOverride)
		{
			const Vector desiredCamCenter = camCenter;
			Vector delta = desiredCamCenter - baseCenter;
			const float desiredDist = delta.Length();
			if (desiredDist > 0.01f)
			{
				Vector dir = delta;
				VectorNormalize(dir);

				constexpr unsigned int kThirdPersonCamMask = (CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_GRATE | CONTENTS_PLAYERCLIP);
				const Vector hullMins(-4.0f, -4.0f, -4.0f);
				const Vector hullMaxs(4.0f, 4.0f, 4.0f);
				Ray_t ray;
				ray.Init(baseCenter, desiredCamCenter, hullMins, hullMaxs);
				CTraceFilterSkipNPCsAndPlayers filter((IHandleEntity*)localPlayer, 0);
				trace_t tr;
				m_Game->m_EngineTrace->TraceRay(ray, kThirdPersonCamMask, &filter, &tr);

				float targetDist = desiredDist;
				if (tr.fraction < 1.0f && !tr.allsolid && !tr.startsolid)
					targetDist = (tr.endpos - baseCenter).Length();
				else if (tr.allsolid || tr.startsolid)
					targetDist = 0.0f;

				if (!s_tpWallCollInit)
				{
					s_tpWallCollDist = targetDist;
					s_tpWallCollInit = true;
				}
				else
				{
					// Snap in instantly when blocked, ease out when unblocked to avoid popping.
					if (targetDist < s_tpWallCollDist)
						s_tpWallCollDist = targetDist;
					else
						s_tpWallCollDist += (targetDist - s_tpWallCollDist) * 0.18f;
				}

				camCenter = baseCenter + (dir * s_tpWallCollDist);
			}
			else
			{
				ResetTpWallColl();
			}
		}
		// Expose the actual VR render camera center used for third-person this frame.
		// This includes HMD-aim yaw and any VR camera offsets, and is used to keep aim line and overlays aligned.
		m_VR->m_ThirdPersonRenderCenter = camCenter;
		leftOrigin = camCenter + (renderRight * (-(ipd * 0.5f)));
		rightOrigin = camCenter + (renderRight * (+(ipd * 0.5f)));
	}
	else if (observerForceInEye)
	{
		// Observer in-eye: use engine camera origin (target eye) but apply stereo IPD; aim with HMD.
		QAngle camAng(viewAngles.x, viewAngles.y, viewAngles.z);
		if (m_VR->m_HmdForward.IsZero())
			camAng = engineCamAngles;
		renderViewAngles.x = camAng.x;
		renderViewAngles.y = camAng.y;
		renderViewAngles.z = camAng.z;

		Vector fwd, right, up;
		QAngle::AngleVectors(camAng, &fwd, &right, &up);

		const float ipd = (m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale);

		// engineCamOrigin is already an eye origin; don't apply EyeZ again.
		m_VR->m_ThirdPersonRenderCenter = engineCamOrigin;
		leftOrigin = engineCamOrigin + (right * (-(ipd * 0.5f)));
		rightOrigin = engineCamOrigin + (right * (+(ipd * 0.5f)));
	}
	else
	{
		// Normal VR first-person
		leftOrigin = m_VR->GetViewOriginLeft();
		rightOrigin = m_VR->GetViewOriginRight();
		// Keep this sane even in 1P (unused there, but prevents stale deltas if 3P toggles).
		m_VR->m_ThirdPersonRenderCenter = m_VR->m_SetupOrigin;
	}

	leftEyeView.origin = leftOrigin;
	leftEyeView.angles = renderViewAngles;

	// Queued render: draw aim line from the render-thread snapshot so it stays glued to the hand/gun.
	// IMPORTANT: must run after we compute m_SetupOrigin / m_ThirdPersonRenderCenter for this frame.
	// DesktopMirrorHidePluginOverlays is intentionally single-thread only. Running an extra
	// clean Source RenderView in queued mode adds another world pass on top of both VR eyes
	// and can destabilize Source's shared shadow RTT state under scene pressure.
	const bool desktopMirrorHidePluginOverlaysSingleCopyActive =
		m_VR->m_IsVREnabled &&
		m_VR->m_DesktopMirrorEnabled &&
		m_VR->m_DesktopMirrorHidePluginOverlays &&
		(m_VR->m_DesktopMirrorTexture != nullptr) &&
		(queueMode == 0);
	if (m_VR->m_IsVREnabled && queueMode != 0)
	{
		m_VR->RenderDrawAimLineQueued(localPlayer);
	}

	// --- IMPORTANT: avoid "dragging/ghosting" when turning with thumbstick ---
	// Do NOT permanently overwrite engine viewangles.
	//
	// NOTE: In mat_queue_mode != 0, dRenderView runs on the render thread.
	// Calling IEngineClient::GetViewAngles/SetViewAngles from the render thread is NOT thread-safe
	// and can intermittently corrupt client state (a common symptom is NX/DEP execute-at-NULL crashes).
	// So we only touch engine viewangles in single-threaded mode.
	QAngle prevEngineAngles;
	bool touchedEngineAngles = false;
	if (queueMode == 0 && m_Game && m_Game->m_EngineClient)
	{
		m_Game->m_EngineClient->GetViewAngles(prevEngineAngles);

		// The CViewSetup below controls what the player sees. Source's audio listener
		// and other global view-angle consumers can still read IEngineClient viewangles.
		// In front-view 3P the render camera intentionally looks back at the player,
		// so using renderViewAngles here inverts front/back audio. Use a separate
		// HMD/player-facing listener angle instead.
		QAngle listenerAngles = BuildVRAudioListenerAngles(m_VR, viewAngles);
		m_Game->m_EngineClient->SetViewAngles(listenerAngles);
		touchedEngineAngles = true;
	}

	// Align HUD view to the same origin/angles; otherwise you can get a second layer that
	// appears to "follow the controller / stick" (classic double-image artifact).
	CViewSetup hudLeft = hudViewSetup;
	NormalizeViewSetupForVREye(hudLeft);
	hudLeft.origin = leftEyeView.origin;
	hudLeft.angles = renderViewAngles;

	rightEyeView.origin = rightOrigin;
	rightEyeView.angles = renderViewAngles;
	CViewSetup hudRight = hudViewSetup;
	NormalizeViewSetupForVREye(hudRight);
	hudRight.origin = rightEyeView.origin;
	hudRight.angles = renderViewAngles;

	std::unique_lock<std::recursive_mutex> reshadeQueuedSurfaceLock;
	if (queueMode != 0 && m_VR->m_ReShadeVRCompat)
		reshadeQueuedSurfaceLock = std::unique_lock<std::recursive_mutex>(m_VR->m_ReShadeVRCompatSurfaceMutex);

	// The queued clean desktop-mirror RenderView path was removed deliberately.
	// Queued mode mirrors the regular eye; only single-thread mode uses desktopMirrorClean0.

	const Vector sharedCenterOrigin(
		(leftEyeView.origin.x + rightEyeView.origin.x) * 0.5f,
		(leftEyeView.origin.y + rightEyeView.origin.y) * 0.5f,
		(leftEyeView.origin.z + rightEyeView.origin.z) * 0.5f);

	auto renderEyeScene = [&](int eyeIndex, ITexture* eyeTexture, LPDIRECT3DSURFACE9 reshadeSurface,
		CViewSetup& eyeView, CViewSetup& eyeHud, bool drawPreViewLaser)
		{
			struct EyeRenderTargetScope
			{
				IMatRenderContext* ctx = nullptr;
				ITexture* oldRT = nullptr;
				int oldX = 0;
				int oldY = 0;
				int oldW = 0;
				int oldH = 0;
				bool hasViewport = false;
				bool pushed = false;

				EyeRenderTargetScope(IMatRenderContext* renderContext, ITexture* target, int width, int height)
					: ctx(renderContext)
				{
					if (!ctx || !target)
						return;

					if (hkPushRenderTargetAndViewport.fOriginal && hkPopRenderTargetAndViewport.fOriginal)
					{
						hkPushRenderTargetAndViewport.fOriginal(ctx, target, nullptr, 0, 0, width, height);
						pushed = true;
						return;
					}

					oldRT = ctx->GetRenderTarget();
					if (hkGetViewport.fOriginal && hkViewport.fOriginal)
					{
						hkGetViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);
						hasViewport = true;
					}
					ctx->SetRenderTarget(target);
					if (hkViewport.fOriginal)
						hkViewport.fOriginal(ctx, 0, 0, width, height);
				}

				~EyeRenderTargetScope()
				{
					if (!ctx)
						return;
					if (pushed)
					{
						hkPopRenderTargetAndViewport.fOriginal(ctx);
						return;
					}
					ctx->SetRenderTarget(oldRT);
					if (hasViewport && hkViewport.fOriginal)
						hkViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);
				}
			};
			struct EyeSharedCenterScope
			{
				int& pass;
				bool& valid;
				Vector& center;
				Vector& eyeOrigin;
				int oldPass = 0;
				bool oldValid = false;
				Vector oldCenter{};
				Vector oldEyeOrigin{};

				EyeSharedCenterScope(int& passRef, bool& validRef, Vector& centerRef, Vector& eyeOriginRef,
					int eyeIndex, const Vector& newCenter, const Vector& newEyeOrigin)
					: pass(passRef), valid(validRef), center(centerRef), eyeOrigin(eyeOriginRef)
				{
					oldPass = pass;
					oldValid = valid;
					oldCenter = center;
					oldEyeOrigin = eyeOrigin;
					pass = eyeIndex;
					valid = true;
					center = newCenter;
					eyeOrigin = newEyeOrigin;
				}

				~EyeSharedCenterScope()
				{
					pass = oldPass;
					valid = oldValid;
					center = oldCenter;
					eyeOrigin = oldEyeOrigin;
				}
			};

			EyeRenderTargetScope eyeRtScope(
				rndrContext,
				eyeTexture,
				static_cast<int>(m_VR->m_RenderWidth),
				static_cast<int>(m_VR->m_RenderHeight));
			EyeSharedCenterScope sharedCenterScope(
				s_vrEyeRenderPass,
				s_vrSharedCenterValid,
				s_vrSharedCenterOrigin,
				s_vrSharedEyeOrigin,
				eyeIndex,
				sharedCenterOrigin,
				eyeView.origin);
			ScopedReShadeVRCompatD3D9StateGuard reshadeGuard(m_VR, reshadeSurface);
			if (drawPreViewLaser && m_VR->m_IsVREnabled)
				m_VR->RenderDrawGameLaserSight(localPlayer);
			if (m_VR->m_IsVREnabled)
				m_VR->BeginVrHandsEyeRender(eyeView, eyeIndex == 1 ? 0 : 1);
			callOriginalRenderView(eyeView, eyeHud, nClearFlags, whatToDraw);
			if (m_VR->m_IsVREnabled)
				m_VR->FinishVrHandsEyeRender();
		};

	auto drawPostEyeWork = [&](int eyeIndex, ITexture* eyeTexture, CViewSetup& eyeView)
		{
			if (!m_VR->m_IsVREnabled || !eyeTexture)
				return;

			ITexture* oldRT = nullptr;
			int oldX = 0, oldY = 0, oldW = 0, oldH = 0;
			bool haveOldViewport = false;
			bool pushed = false;
			if (hkPushRenderTargetAndViewport.fOriginal && hkPopRenderTargetAndViewport.fOriginal)
			{
				hkPushRenderTargetAndViewport.fOriginal(
					rndrContext,
					eyeTexture,
					nullptr,
					0,
					0,
					static_cast<int>(m_VR->m_RenderWidth),
					static_cast<int>(m_VR->m_RenderHeight));
				pushed = true;
			}
			else
			{
				oldRT = rndrContext->GetRenderTarget();
				if (hkGetViewport.fOriginal && hkViewport.fOriginal)
				{
					hkGetViewport.fOriginal(rndrContext, oldX, oldY, oldW, oldH);
					haveOldViewport = true;
				}
				rndrContext->SetRenderTarget(eyeTexture);
				if (hkViewport.fOriginal)
					hkViewport.fOriginal(rndrContext, 0, 0, m_VR->m_RenderWidth, m_VR->m_RenderHeight);
			}

			m_VR->DrawPostMirrorPluginOverlays(rndrContext, localPlayer, eyeView, eyeIndex);
			m_VR->UpdateD3DAimLineOverlayForView(localPlayer, eyeView, eyeIndex);

			if (pushed)
				hkPopRenderTargetAndViewport.fOriginal(rndrContext);
			else
			{
				rndrContext->SetRenderTarget(oldRT);
				if (haveOldViewport && hkViewport.fOriginal)
					hkViewport.fOriginal(rndrContext, oldX, oldY, oldW, oldH);
			}
		};

	const bool copyRightEyeFromLeft =
		m_VR->m_RightEyeCopyFromLeft &&
		queueMode == 0 &&
		m_VR->m_IsVREnabled;
	bool rightEyeCopiedFromLeft = false;

	const auto stereoSceneStartTime = std::chrono::steady_clock::now();
	{
		renderEyeScene(1, m_VR->m_LeftEyeTexture, m_VR->m_D9LeftEyeSurface, leftEyeView, hudLeft, true);
		if (desktopMirrorHidePluginOverlaysSingleCopyActive && m_VR->m_DesktopMirrorEye == 0)
			m_VR->CopyEyeToDesktopMirrorTexture(0);
		if (copyRightEyeFromLeft)
			rightEyeCopiedFromLeft = m_VR->CopyLeftEyeToRightEyeTexture();
		drawPostEyeWork(0, m_VR->m_LeftEyeTexture, leftEyeView);
	}
	m_PushedHud = false;

	{
		if (!rightEyeCopiedFromLeft)
		{
			renderEyeScene(2, m_VR->m_RightEyeTexture, m_VR->m_D9RightEyeSurface, rightEyeView, hudRight, false);
		}
		if (desktopMirrorHidePluginOverlaysSingleCopyActive && m_VR->m_DesktopMirrorEye != 0)
			m_VR->CopyEyeToDesktopMirrorTexture(1);
		drawPostEyeWork(1, m_VR->m_RightEyeTexture, rightEyeView);
	}
	timingStereoSceneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stereoSceneStartTime).count();

	auto renderToTexture_SetRT = [&](ITexture* target, int texW, int texH, QAngle passAngles,
		CViewSetup& view, CViewSetup& hud)
		{
			// Scope/rear-mirror RTTs are allowed in queued rendering. Do not touch
			// EngineClient viewangles in queueMode!=0; CViewSetup already carries
			// the offscreen camera pose for the render thread.
			IMatRenderContext* rc = m_Game->m_MaterialSystem->GetRenderContext();
			if (!rc)
			{
				m_VR->HandleMissingRenderContext("Hooks::dRenderView(offscreen)");
				return;
			}

			const bool prevSuppress = m_VR->m_SuppressHudCapture;
			m_VR->m_SuppressHudCapture = true;

			int oldX = 0, oldY = 0, oldW = 0, oldH = 0;
			bool hasOldViewport = false;
			if (hkGetViewport.fOriginal)
			{
				hkGetViewport.fOriginal(rc, oldX, oldY, oldW, oldH);
				hasOldViewport = true;
			}
			ITexture* oldRT = rc->GetRenderTarget();

			rc->SetRenderTarget(target);
			if (hkViewport.fOriginal)
				hkViewport.fOriginal(rc, 0, 0, texW, texH);

			rc->ClearColor4ub(0, 0, 0, 255);
			rc->ClearBuffers(true, true, true);

			QAngle oldEngineAngles;
			bool touchedAngles = false;
			if (queueMode == 0 && m_Game && m_Game->m_EngineClient)
			{
				m_Game->m_EngineClient->GetViewAngles(oldEngineAngles);
				m_Game->m_EngineClient->SetViewAngles(passAngles);
				touchedAngles = true;
			}

			callOriginalRenderView(view, hud, nClearFlags, whatToDraw);

			if (touchedAngles && m_Game && m_Game->m_EngineClient)
				m_Game->m_EngineClient->SetViewAngles(oldEngineAngles);

			rc->SetRenderTarget(oldRT);
			if (hasOldViewport && hkViewport.fOriginal)
				hkViewport.fOriginal(rc, oldX, oldY, oldW, oldH);

			m_VR->m_SuppressHudCapture = prevSuppress;
		};

	bool scopeLensPostProcessPending = false;

	// ----------------------------
	// Scope RTT pass: render from scope camera into vrScope RTT
	// ----------------------------
	if (m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) && m_VR->ShouldRenderScope() && m_VR->m_ScopeTexture && m_VR->ShouldUpdateScopeRTT())
	{
		CViewSetup scopeView = setup;
		scopeView.x = 0;
		scopeView.y = 0;
		scopeView.m_nUnscaledX = 0;
		scopeView.m_nUnscaledY = 0;
		scopeView.width = m_VR->m_ScopeRTTSize;
		scopeView.m_nUnscaledWidth = m_VR->m_ScopeRTTSize;
		scopeView.height = m_VR->m_ScopeRTTSize;
		scopeView.m_nUnscaledHeight = m_VR->m_ScopeRTTSize;
		scopeView.fov = m_VR->m_ScopeFov;
		scopeView.m_flAspectRatio = 1.0f;
		scopeView.fovViewmodel = scopeView.fov;
		scopeView.zNear = m_VR->m_ScopeZNear;
		scopeView.zNearViewmodel = 99999.0f; // hard-clip viewmodel so scope image is "world only"

		QAngle scopeAngles = m_VR->GetScopeCameraAbsAngle();
		scopeView.origin = m_VR->GetScopeCameraAbsPos();
		scopeView.angles.x = scopeAngles.x;
		scopeView.angles.y = scopeAngles.y;
		scopeView.angles.z = scopeAngles.z;

		CViewSetup hudScope = hudViewSetup;
		hudScope.origin = scopeView.origin;
		hudScope.angles = scopeView.angles;

		m_VR->m_ScopeRenderingPass = true;

		renderToTexture_SetRT(m_VR->m_ScopeTexture,
			m_VR->m_ScopeRTTSize, m_VR->m_ScopeRTTSize,
			scopeAngles, scopeView, hudScope);
		m_VR->m_ScopeRenderingPass = false;
		scopeLensPostProcessPending = true;
	}

	// ----------------------------
	// Rear mirror RTT pass: render from HMD with 180 yaw into vrRearMirror RTT
	// ----------------------------
	if (m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) && m_VR->ShouldRenderRearMirror() && m_VR->m_RearMirrorTexture && m_VR->ShouldUpdateRearMirrorRTT())
	{
		CViewSetup mirrorView = setup;
		mirrorView.x = 0;
		mirrorView.y = 0;
		mirrorView.m_nUnscaledX = 0;
		mirrorView.m_nUnscaledY = 0;
		mirrorView.width = m_VR->m_RearMirrorRTTSize;
		mirrorView.m_nUnscaledWidth = m_VR->m_RearMirrorRTTSize;
		mirrorView.height = m_VR->m_RearMirrorRTTSize;
		mirrorView.m_nUnscaledHeight = m_VR->m_RearMirrorRTTSize;
		mirrorView.fov = m_VR->m_RearMirrorFov;
		mirrorView.m_flAspectRatio = 1.0f;
		mirrorView.fovViewmodel = mirrorView.fov;
		mirrorView.zNear = m_VR->m_RearMirrorZNear;
		mirrorView.zNearViewmodel = 99999.0f;

		QAngle mirrorAngles = m_VR->GetRearMirrorCameraAbsAngle();
		mirrorView.origin = m_VR->GetRearMirrorCameraAbsPos();
		mirrorView.angles.x = mirrorAngles.x;
		mirrorView.angles.y = mirrorAngles.y;
		mirrorView.angles.z = mirrorAngles.z;

		CViewSetup hudMirror = hudViewSetup;
		hudMirror.origin = mirrorView.origin;
		hudMirror.angles = mirrorView.angles;

		// Mark mirror RTT pass so DrawModelExecute can tag special-infected arrows seen in this pass.
		m_VR->m_RearMirrorRenderingPass = true;
		m_VR->m_RearMirrorSawSpecialThisPass = false;

		renderToTexture_SetRT(m_VR->m_RearMirrorTexture,
			m_VR->m_RearMirrorRTTSize, m_VR->m_RearMirrorRTTSize,
			mirrorAngles, mirrorView, hudMirror);

		m_VR->m_RearMirrorRenderingPass = false;
		const auto rmNow = std::chrono::steady_clock::now();
		if (m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
		{
			if (m_VR->m_RearMirrorSawSpecialThisPass)
			{
				m_VR->m_LastRearMirrorSpecialSeenTime = rmNow;
				m_VR->m_RearMirrorSpecialEnlargeActive = true;
			}

			if (m_VR->m_RearMirrorSpecialEnlargeActive)
			{
				const float elapsed = std::chrono::duration<float>(rmNow - m_VR->m_LastRearMirrorSpecialSeenTime).count();
				if (elapsed > m_VR->m_RearMirrorSpecialEnlargeHoldSeconds)
					m_VR->m_RearMirrorSpecialEnlargeActive = false;
			}
		}
		else
		{
			// Disabled
			m_VR->m_RearMirrorSpecialEnlargeActive = false;
		}
	}

	if (scopeLensPostProcessPending)
	{
		if (queueMode != 0)
		{
			// Source queued rendering can execute the RenderView command stream after this
			// hook returns. Running the lens pass here can be overwritten by the
			// queued square RTT draw, so defer it to the post-present update path.
			m_VR->m_QueuedScopeLensPostProcessPending.store(1u, std::memory_order_release);
		}
		else
		{
			m_VR->ApplyScopeLensPostProcess();
		}
	}

	// Restore engine angles immediately after our stereo render (single-threaded only).
	if (touchedEngineAngles && m_Game && m_Game->m_EngineClient)
		m_Game->m_EngineClient->SetViewAngles(prevEngineAngles);

	// Queue the outer RT/viewport restore before the completion marker. Previously the
	// guard restored from its destructor after QueueSourceRenderCompletionMarker(), so
	// Present could observe a "completed" frame while Source still had state commands
	// behind the marker.
	renderContextStateGuard.Restore();

	const uint32_t renderFrameSeq = m_VR->m_RenderFrameSeq.load(std::memory_order_acquire);
	if (queueMode != 0)
	{
		if (renderPoseTokenUsed == 0)
			renderPoseTokenUsed = m_VR->m_SubmitPoseToken.load(std::memory_order_acquire);

		if (m_VR->m_RenderPipelineDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_lastQueuedFrameTimingLog{};
			if (!ShouldThrottleLog(s_lastQueuedFrameTimingLog, m_VR->m_RenderPipelineDebugLogHz))
			{
				const double hookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderHookStartTime).count();
				const float hmdHz = m_VR->GetHmdDisplayFrequencyHz();
				Game::logMsg("[VR][Queued][FrameTiming] tid=%lu q=%d hmdHz=%.1f hookMs=%.3f poseWaitMs=%.3f poseBudgetMs=%lu poseAttempt=%d poseFresh=%d poseRelaxed=%d poseBefore=%u poseAfter=%u stereoMs=%.3f renderPose=%u poseSrc=%s submitPose=%u lastSubmitted=%u completed=%u submitted=%u",
					GetCurrentThreadId(),
					queueMode,
					hmdHz,
					hookMs,
					timingPoseAcquireMs,
					static_cast<unsigned long>(timingPoseAcquireBudgetMs),
					timingPoseAcquireAttempted ? 1 : 0,
					timingPoseAcquireFresh ? 1 : 0,
					timingPoseAcquireRelaxed ? 1 : 0,
					timingPoseSeqBeforeAcquire,
					timingPoseSeqAfterAcquire,
					timingStereoSceneMs,
					renderPoseTokenUsed,
					renderPoseUsesTrackingPrediction ? "tracking" : "waiter",
					m_VR->m_SubmitPoseToken.load(std::memory_order_acquire),
					m_VR->m_LastSubmittedPoseToken.load(std::memory_order_acquire),
					m_VR->m_RenderCompletedFrameId.load(std::memory_order_acquire),
					m_VR->m_LastSubmittedFrameId.load(std::memory_order_acquire));
			}
		}

		// Append a completion marker behind this stereo frame's material commands. The
		// material worker publishes the frame only after the queued eye writes have run.
		if (m_VR->QueueSourceRenderCompletionMarker(
			rndrContext,
			renderPoseTokenUsed,
			renderFrameSeq,
			renderPoseAllowDuplicateSubmit,
			renderHmdPoseForSubmitValid ? &renderHmdPoseForSubmit : nullptr))
		{
			return;
		}

		if (m_VR->m_ReShadeVRCompat)
		{
			// Fallback for contexts that unexpectedly expose no Source call queue.
			// ReShade resolves eye RTs after Present, so retain the older EndFrame gate.
			m_VR->m_ReShadeVRCompatPendingRenderPoseToken.store(renderPoseTokenUsed, std::memory_order_release);
			m_VR->m_ReShadeVRCompatPendingRenderFrameSeq.store(renderFrameSeq, std::memory_order_release);
			m_VR->m_ReShadeVRCompatPendingDuplicatePose.store(renderPoseAllowDuplicateSubmit ? 1u : 0u, std::memory_order_release);
			m_VR->m_ReShadeVRCompatPendingRenderReady.store(1, std::memory_order_release);

			if (m_VR->m_RenderPipelineDebugLog)
			{
				static thread_local std::chrono::steady_clock::time_point s_lastRenderPendingLog{};
				if (!ShouldThrottleLog(s_lastRenderPendingLog, m_VR->m_RenderPipelineDebugLogHz))
				{
					Game::logMsg("[VR][Queued][RenderCompletePendingFallback] tid=%lu q=%d completed=%u frameSeq=%u renderPose=%u poseSeq=%u submitPose=%u lastSubmitted=%u renderedNew=%d",
						GetCurrentThreadId(), queueMode,
						m_VR->m_RenderCompletedFrameId.load(std::memory_order_acquire),
						renderFrameSeq,
						renderPoseTokenUsed,
						m_VR->m_PoseWaiterSeq.load(std::memory_order_acquire),
						m_VR->m_SubmitPoseToken.load(std::memory_order_acquire),
						m_VR->m_LastSubmittedFrameId.load(std::memory_order_acquire),
						m_VR->m_RenderedNewFrame.load(std::memory_order_acquire) ? 1 : 0);
				}
			}
			return;
		}

		// Without a usable Source call queue there is no execution-order proof that the
		// queued stereo commands have finished. Keep reprojecting the last stable submit
		// snapshot instead of publishing half-executed raw eye textures.
		m_VR->m_SourceRenderQueueOwnershipUncertain.store(true, std::memory_order_release);
		m_VR->m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
		if (m_VR->m_RenderPipelineDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_lastMissingMarkerDropLog{};
			if (!ShouldThrottleLog(s_lastMissingMarkerDropLog, m_VR->m_RenderPipelineDebugLogHz))
			{
				Game::logMsg("[VR][Queued][RenderCompleteDropNoCallQueue] tid=%lu q=%d frameSeq=%u renderPose=%u",
					GetCurrentThreadId(), queueMode, renderFrameSeq, renderPoseTokenUsed);
			}
		}
		return;
	}

	m_VR->PublishRenderCompletedFrame(
		renderPoseTokenUsed,
		renderFrameSeq,
		renderPoseAllowDuplicateSubmit,
		queueMode != 0 ? "render-hook-fallback" : "single-thread",
		0,
		renderHmdPoseForSubmitValid ? &renderHmdPoseForSubmit : nullptr);
}
