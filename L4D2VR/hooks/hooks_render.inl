namespace
{
	enum class HooksNekoPostPass : int
	{
		Outside = 0,
		MainEye,
		Scope,
		RearMirror,
	};

	struct HooksNekoPostProbeThreadState
	{
		std::uint64_t frameSerial = 0;
		HooksNekoPostPass pass = HooksNekoPostPass::Outside;
		int eyeIndex = 0;
		unsigned int candidateCalls = 0;
		IMatRenderContext* context = nullptr;
		ITexture* takeoverTarget = nullptr;
		bool takeoverEnabled = false;
	};

	thread_local HooksNekoPostProbeThreadState g_NekoPostProbeState{};
	std::atomic<std::uint64_t> g_NekoPostProbeFrameSerial{ 0 };
	std::atomic<unsigned int> g_NekoPostLumaProbeMask{ 0 };

	const char* HooksNekoPostPassName(HooksNekoPostPass pass)
	{
		switch (pass)
		{
		case HooksNekoPostPass::MainEye: return "main-eye";
		case HooksNekoPostPass::Scope: return "scope";
		case HooksNekoPostPass::RearMirror: return "rear-mirror";
		default: return "outside";
		}
	}

	void HooksNekoPostProbeSurfaceLuma(
		unsigned int stageBit,
		const char* stageName,
		IDirect3DDevice9* device,
		IDirect3DSurface9* source)
	{
		if (!device || !source)
			return;
		// stageBit==0 is reserved for the bounded temporal probe. It deliberately
		// bypasses the one-shot mask; its own cadence and lifetime are controlled by
		// HooksNekoPostProbeTemporalState below.
		if (stageBit && (g_NekoPostLumaProbeMask.fetch_or(
			stageBit, std::memory_order_acq_rel) & stageBit))
		{
			return;
		}

		D3DSURFACE_DESC sourceDesc{};
		HRESULT const sourceDescHr = source->GetDesc(&sourceDesc);
		const bool sourceIsEightBit =
			sourceDesc.Format == D3DFMT_A8R8G8B8 ||
			sourceDesc.Format == D3DFMT_X8R8G8B8;
		const bool sourceIsFloat =
			sourceDesc.Format == D3DFMT_A16B16G16R16F;
		if (FAILED(sourceDescHr) || (!sourceIsEightBit && !sourceIsFloat))
		{
			Game::logMsg(
				"[VR][NekoPostLuma] stage=%s ok=0 reason=source-format src=%ux%u fmt=%d desc=0x%08lx tid=%lu",
				stageName ? stageName : "<null>",
				sourceDesc.Width,
				sourceDesc.Height,
				static_cast<int>(sourceDesc.Format),
				static_cast<unsigned long>(sourceDescHr),
				GetCurrentThreadId());
			return;
		}

		constexpr UINT kProbeSize = 64;
		IDirect3DSurface9* reduced = nullptr;
		IDirect3DSurface9* readback = nullptr;
		const D3DFORMAT probeFormat = sourceIsFloat
			? D3DFMT_A8R8G8B8
			: sourceDesc.Format;
		HRESULT const createReducedHr = device->CreateRenderTarget(
			kProbeSize,
			kProbeSize,
			probeFormat,
			D3DMULTISAMPLE_NONE,
			0,
			FALSE,
			&reduced,
			nullptr);
		HRESULT stretchHr = E_FAIL;
		HRESULT createReadbackHr = E_FAIL;
		HRESULT readbackHr = E_FAIL;
		HRESULT lockHr = E_FAIL;
		D3DLOCKED_RECT locked{};
		if (SUCCEEDED(createReducedHr) && reduced)
		{
			stretchHr = device->StretchRect(
				source, nullptr, reduced, nullptr, D3DTEXF_LINEAR);
			if (SUCCEEDED(stretchHr))
			{
				createReadbackHr = device->CreateOffscreenPlainSurface(
					kProbeSize,
					kProbeSize,
					probeFormat,
					D3DPOOL_SYSTEMMEM,
					&readback,
					nullptr);
				if (SUCCEEDED(createReadbackHr) && readback)
				{
					readbackHr = device->GetRenderTargetData(reduced, readback);
					if (SUCCEEDED(readbackHr))
						lockHr = readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
				}
			}
		}

		if (SUCCEEDED(lockHr) && locked.pBits)
		{
			std::uint64_t redSum = 0;
			std::uint64_t greenSum = 0;
			std::uint64_t blueSum = 0;
			std::uint64_t lumaSum = 0;
			unsigned int histogram[256]{};
			unsigned int darkPixels = 0;
			unsigned int brightPixels = 0;
			for (UINT y = 0; y < kProbeSize; ++y)
			{
				const unsigned char* const row =
					reinterpret_cast<const unsigned char*>(locked.pBits) +
					static_cast<std::size_t>(y) * locked.Pitch;
				for (UINT x = 0; x < kProbeSize; ++x)
				{
					const unsigned char blue = row[x * 4 + 0];
					const unsigned char green = row[x * 4 + 1];
					const unsigned char red = row[x * 4 + 2];
					const unsigned int luma =
						(54u * red + 183u * green + 19u * blue + 128u) >> 8;
					redSum += red;
					greenSum += green;
					blueSum += blue;
					lumaSum += luma;
					++histogram[luma];
					if (luma < 32)
						++darkPixels;
					if (luma > 224)
						++brightPixels;
				}
			}

			constexpr unsigned int kPixelCount = kProbeSize * kProbeSize;
			auto percentile = [&](unsigned int numerator, unsigned int denominator)
			{
				const unsigned int threshold =
					(kPixelCount * numerator + denominator - 1) / denominator;
				unsigned int cumulative = 0;
				for (unsigned int i = 0; i < 256; ++i)
				{
					cumulative += histogram[i];
					if (cumulative >= threshold)
						return i;
				}
				return 255u;
			};

			Game::logMsg(
				"[VR][NekoPostLuma] stage=%s ok=1 src=%ux%u fmt=%d avgRGB=%llu/%llu/%llu avgY=%llu p05=%u p50=%u p95=%u dark=%u bright=%u tid=%lu",
				stageName ? stageName : "<null>",
				sourceDesc.Width,
				sourceDesc.Height,
				static_cast<int>(sourceDesc.Format),
				static_cast<unsigned long long>(redSum / kPixelCount),
				static_cast<unsigned long long>(greenSum / kPixelCount),
				static_cast<unsigned long long>(blueSum / kPixelCount),
				static_cast<unsigned long long>(lumaSum / kPixelCount),
				percentile(5, 100),
				percentile(50, 100),
				percentile(95, 100),
				darkPixels,
				brightPixels,
				GetCurrentThreadId());
			readback->UnlockRect();
		}
		else
		{
			Game::logMsg(
				"[VR][NekoPostLuma] stage=%s ok=0 reason=readback createRT=0x%08lx stretch=0x%08lx createSys=0x%08lx read=0x%08lx lock=0x%08lx tid=%lu",
				stageName ? stageName : "<null>",
				static_cast<unsigned long>(createReducedHr),
				static_cast<unsigned long>(stretchHr),
				static_cast<unsigned long>(createReadbackHr),
				static_cast<unsigned long>(readbackHr),
				static_cast<unsigned long>(lockHr),
				GetCurrentThreadId());
		}

		if (readback)
			readback->Release();
		if (reduced)
			reduced->Release();
	}

	void HooksNekoPostProbeBoundSamplers(IDirect3DDevice9* device)
	{
		if (!device)
			return;

		static std::atomic<bool> s_logged{ false };
		if (s_logged.exchange(true, std::memory_order_acq_rel))
			return;

		for (DWORD stage = 0; stage < 10; ++stage)
		{
			IDirect3DBaseTexture9* base = nullptr;
			const HRESULT getHr = device->GetTexture(stage, &base);
			DWORD samplerSrgb = 0;
			const HRESULT srgbHr = device->GetSamplerState(
				stage, D3DSAMP_SRGBTEXTURE, &samplerSrgb);
			if (!base)
			{
				Game::logMsg(
					"[VR][NekoPostSampler] stage=%lu bound=0 get=0x%08lx srgb=%lu srgbHr=0x%08lx",
					static_cast<unsigned long>(stage),
					static_cast<unsigned long>(getHr),
					static_cast<unsigned long>(samplerSrgb),
					static_cast<unsigned long>(srgbHr));
				continue;
			}

			IDirect3DTexture9* texture = nullptr;
			const HRESULT queryHr = base->QueryInterface(
				__uuidof(IDirect3DTexture9),
				reinterpret_cast<void**>(&texture));
			D3DSURFACE_DESC desc{};
			HRESULT descHr = E_NOINTERFACE;
			if (texture)
				descHr = texture->GetLevelDesc(0, &desc);
			Game::logMsg(
				"[VR][NekoPostSampler] stage=%lu bound=1 type=%d get=0x%08lx query=0x%08lx desc=0x%08lx size=%ux%u fmt=%d levels=%lu srgb=%lu srgbHr=0x%08lx",
				static_cast<unsigned long>(stage),
				static_cast<int>(base->GetType()),
				static_cast<unsigned long>(getHr),
				static_cast<unsigned long>(queryHr),
				static_cast<unsigned long>(descHr),
				desc.Width,
				desc.Height,
				static_cast<int>(desc.Format),
				texture ? static_cast<unsigned long>(texture->GetLevelCount()) : 0ul,
				static_cast<unsigned long>(samplerSrgb),
				static_cast<unsigned long>(srgbHr));

			if (texture && (stage < 2 || stage == 8))
			{
				IDirect3DSurface9* surface = nullptr;
				if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface)) && surface)
				{
					char stageName[32]{};
					sprintf_s(stageName, "sampler%lu", static_cast<unsigned long>(stage));
					const unsigned int lumaBit = stage == 8
						? 1024u
						: (128u << stage);
					HooksNekoPostProbeSurfaceLuma(
						lumaBit,
						stageName,
						device,
						surface);
					surface->Release();
				}
			}
			if (texture)
				texture->Release();
			base->Release();
		}
	}

	void HooksNekoPostProbeTemporalState(
		IDirect3DDevice9* device,
		IDirect3DSurface9* outputSurface)
	{
		if (!device || !outputSurface ||
			g_NekoPostProbeState.pass != HooksNekoPostPass::MainEye ||
			(g_NekoPostProbeState.eyeIndex != 1 &&
				g_NekoPostProbeState.eyeIndex != 2))
		{
			return;
		}

		// Capture paired eyes at 10 Hz for six seconds. The synchronous readback is
		// intentionally short-lived and only active when NekoEnginePostProbeLog is
		// enabled, so normal play does not retain this diagnostic cost.
		static std::atomic<ULONGLONG> s_firstTick{ 0 };
		static std::atomic<ULONGLONG> s_lastPairTick{ 0 };
		static std::atomic<unsigned int> s_nextSequence{ 0 };
		static std::atomic<unsigned int> s_pendingRightSequence{ 0 };
		const ULONGLONG now = GetTickCount64();
		ULONGLONG firstTick = s_firstTick.load(std::memory_order_acquire);
		if (firstTick == 0)
		{
			s_firstTick.compare_exchange_strong(
				firstTick, now, std::memory_order_acq_rel, std::memory_order_acquire);
			firstTick = s_firstTick.load(std::memory_order_acquire);
		}
		const ULONGLONG elapsedMs = now >= firstTick ? now - firstTick : 0;
		if (elapsedMs > 6000)
			return;

		unsigned int sequence = 0;
		if (g_NekoPostProbeState.eyeIndex == 1)
		{
			ULONGLONG lastTick = s_lastPairTick.load(std::memory_order_acquire);
			if (lastTick != 0 && now < lastTick + 100)
				return;
			if (!s_lastPairTick.compare_exchange_strong(
				lastTick, now, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return;
			}
			sequence = s_nextSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
			s_pendingRightSequence.store(sequence, std::memory_order_release);
		}
		else
		{
			sequence = s_pendingRightSequence.exchange(0, std::memory_order_acq_rel);
			if (sequence == 0)
				return;
		}

		Game::logMsg(
			"[VR][NekoPostTemporal] seq=%u elapsedMs=%llu frame=%llu eye=%d adaptedLum=%.6f tid=%lu",
			sequence,
			static_cast<unsigned long long>(elapsedMs),
			static_cast<unsigned long long>(g_NekoPostProbeState.frameSerial),
			g_NekoPostProbeState.eyeIndex,
			Hooks::m_Game
				? Hooks::m_Game->GetConVarFloat(
					"mat_neko_engine_post_adapted_lum", -9999.0f)
				: -9999.0f,
			GetCurrentThreadId());

		for (DWORD stage = 3; stage <= 4; ++stage)
		{
			IDirect3DBaseTexture9* base = nullptr;
			if (FAILED(device->GetTexture(stage, &base)) || !base)
				continue;
			IDirect3DTexture9* texture = nullptr;
			if (SUCCEEDED(base->QueryInterface(
				__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture))) &&
				texture)
			{
				IDirect3DSurface9* surface = nullptr;
				if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface)) && surface)
				{
					char stageName[64]{};
					sprintf_s(
						stageName,
						"temporal-%u-eye%d-sampler%lu",
						sequence,
						g_NekoPostProbeState.eyeIndex,
						static_cast<unsigned long>(stage));
					HooksNekoPostProbeSurfaceLuma(0, stageName, device, surface);
					surface->Release();
				}
				texture->Release();
			}
			base->Release();
		}

		char outputName[64]{};
		sprintf_s(
			outputName,
			"temporal-%u-eye%d-output",
			sequence,
			g_NekoPostProbeState.eyeIndex);
		HooksNekoPostProbeSurfaceLuma(0, outputName, device, outputSurface);
	}

	int HooksNekoPostReadMaterialInt(
		IMaterial* material,
		const char* name,
		int fallback = -9999)
	{
		if (!material || !name)
			return fallback;
		__try
		{
			bool found = false;
			void* const var = material->FindVar(name, &found, false);
			if (!found || !var)
				return fallback;
			void** const vtable = *reinterpret_cast<void***>(var);
			if (!vtable || !vtable[26])
				return fallback;
			using GetIntValueFn = int(__thiscall*)(void*);
			return reinterpret_cast<GetIntValueFn>(vtable[26])(var);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return fallback;
		}
	}

	float HooksNekoPostReadMaterialFloat(
		IMaterial* material,
		const char* name,
		float fallback = -9999.0f)
	{
		if (!material || !name)
			return fallback;
		__try
		{
			bool found = false;
			void* const var = material->FindVar(name, &found, false);
			if (!found || !var)
				return fallback;
			void** const vtable = *reinterpret_cast<void***>(var);
			if (!vtable || !vtable[27])
				return fallback;
			using GetFloatValueFn = float(__thiscall*)(void*);
			return reinterpret_cast<GetFloatValueFn>(vtable[27])(var);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return fallback;
		}
	}

	ITexture* HooksNekoPostReadMaterialTexture(
		IMaterial* material,
		const char* name)
	{
		if (!material || !name)
			return nullptr;
		__try
		{
			bool found = false;
			void* const var = material->FindVar(name, &found, false);
			if (!found || !var)
				return nullptr;
			void** const vtable = *reinterpret_cast<void***>(var);
			if (!vtable || !vtable[0])
				return nullptr;
			using GetTextureValueFn = ITexture* (__thiscall*)(void*);
			return reinterpret_cast<GetTextureValueFn>(vtable[0])(var);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	HRESULT HooksNekoPostApplyColorTransfer(
		VR* vr,
		IDirect3DDevice9* device,
		IDirect3DSurface9* sourceTextureSurface,
		IDirect3DSurface9* destination,
		float gamma,
		bool decodeSrgb)
	{
		if (!vr || !device || !sourceTextureSurface || !destination)
			return E_POINTER;

		if (vr->m_D9NekoPostOutputTransferShaderDevice != device)
		{
			if (vr->m_D9NekoPostOutputTransferPixelShader)
			{
				vr->m_D9NekoPostOutputTransferPixelShader->Release();
				vr->m_D9NekoPostOutputTransferPixelShader = nullptr;
			}
			vr->m_D9NekoPostOutputTransferShaderDevice = device;
		}

		if (!vr->m_D9NekoPostOutputTransferPixelShader)
		{
			static const char* const kShaderSource = R"HLSL(
sampler2D gSource : register(s0);
float4 gTransfer : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 color = tex2D(gSource, uv);
    float3 c = saturate(color.rgb);
    if (gTransfer.y > 0.5)
    {
        float3 low = c / 12.92;
        float3 high = pow((c + 0.055) / 1.055, 2.4);
        color.rgb = lerp(high, low, step(c, 0.04045));
    }
    else
    {
        color.rgb = pow(c, gTransfer.xxx);
    }
    return color;
}
)HLSL";
			ID3DXBuffer* bytecode = nullptr;
			ID3DXBuffer* errors = nullptr;
			const HRESULT compileHr = D3DXCompileShader(
				kShaderSource,
				static_cast<UINT>(std::strlen(kShaderSource)),
				nullptr,
				nullptr,
				"main",
				"ps_3_0",
				0,
				&bytecode,
				&errors,
				nullptr);
			if (FAILED(compileHr) || !bytecode)
			{
				static std::atomic<bool> s_loggedCompileFailure{ false };
				if (!s_loggedCompileFailure.exchange(true, std::memory_order_acq_rel))
				{
					Game::logMsg(
						"[VR][NekoPostTransfer] shader compile failed hr=0x%08lx error=%s",
						static_cast<unsigned long>(compileHr),
						errors
							? static_cast<const char*>(errors->GetBufferPointer())
							: "<none>");
				}
				if (errors)
					errors->Release();
				if (bytecode)
					bytecode->Release();
				return compileHr;
			}

			const HRESULT createHr = device->CreatePixelShader(
				static_cast<const DWORD*>(bytecode->GetBufferPointer()),
				&vr->m_D9NekoPostOutputTransferPixelShader);
			bytecode->Release();
			if (errors)
				errors->Release();
			if (FAILED(createHr) || !vr->m_D9NekoPostOutputTransferPixelShader)
				return createHr;
		}

		IDirect3DTexture9* sourceTexture = nullptr;
		const HRESULT containerHr = sourceTextureSurface->GetContainer(
			__uuidof(IDirect3DTexture9),
			reinterpret_cast<void**>(&sourceTexture));
		if (FAILED(containerHr) || !sourceTexture)
			return containerHr;

		D3DSURFACE_DESC destinationDesc{};
		HRESULT hr = destination->GetDesc(&destinationDesc);
		if (FAILED(hr))
		{
			sourceTexture->Release();
			return hr;
		}

		IDirect3DSurface9* oldRenderTarget = nullptr;
		const HRESULT oldRenderTargetHr = device->GetRenderTarget(0, &oldRenderTarget);
		D3DVIEWPORT9 oldViewport{};
		const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));
		IDirect3DStateBlock9* stateBlock = nullptr;
		if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
			stateBlock->Capture();

		D3DVIEWPORT9 viewport{};
		viewport.Width = destinationDesc.Width;
		viewport.Height = destinationDesc.Height;
		viewport.MinZ = 0.0f;
		viewport.MaxZ = 1.0f;
		struct TransferVertex
		{
			float x;
			float y;
			float z;
			float rhw;
			float u;
			float v;
		};
		const float right = static_cast<float>(destinationDesc.Width) - 0.5f;
		const float bottom = static_cast<float>(destinationDesc.Height) - 0.5f;
		const TransferVertex quad[4] =
		{
			{ -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
			{ right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
			{ -0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
			{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f }
		};
		const float transfer[4] = {
			std::clamp(gamma, 1.0f, 4.0f),
			decodeSrgb ? 1.0f : 0.0f,
			0.0f,
			0.0f
		};

		hr = device->SetRenderTarget(0, destination);
		if (SUCCEEDED(hr))
			hr = device->SetViewport(&viewport);
		if (SUCCEEDED(hr))
		{
			device->SetVertexShader(nullptr);
			device->SetPixelShader(vr->m_D9NekoPostOutputTransferPixelShader);
			device->SetPixelShaderConstantF(0, transfer, 1);
			device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			device->SetTexture(0, sourceTexture);
			device->SetRenderState(D3DRS_ZENABLE, FALSE);
			device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			hr = device->DrawPrimitiveUP(
				D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TransferVertex));
		}

		if (stateBlock)
		{
			stateBlock->Apply();
			stateBlock->Release();
		}
		else
		{
			device->SetTexture(0, nullptr);
			device->SetPixelShader(nullptr);
		}
		if (SUCCEEDED(oldRenderTargetHr) && oldRenderTarget)
			device->SetRenderTarget(0, oldRenderTarget);
		if (haveOldViewport)
			device->SetViewport(&oldViewport);
		if (oldRenderTarget)
			oldRenderTarget->Release();
		sourceTexture->Release();
		return hr;
	}

	void HooksNekoPostProbeEnter(
		IMatRenderContext* context,
		std::uint64_t frameSerial,
		HooksNekoPostPass pass,
		int eyeIndex,
		ITexture* takeoverTarget,
		bool takeoverEnabled)
	{
		ITexture* const previousTakeoverTarget =
			g_NekoPostProbeState.takeoverTarget;
		g_NekoPostProbeState.frameSerial = frameSerial;
		g_NekoPostProbeState.pass = pass;
		g_NekoPostProbeState.eyeIndex = eyeIndex;
		g_NekoPostProbeState.candidateCalls = 0;
		g_NekoPostProbeState.context = context;
		g_NekoPostProbeState.takeoverTarget = takeoverTarget;
		g_NekoPostProbeState.takeoverEnabled = takeoverEnabled;
		if (g_NekoPostProbeState.takeoverTarget)
			g_NekoPostProbeState.takeoverTarget->AddRef();
		if (previousTakeoverTarget)
			previousTakeoverTarget->Release();
	}

	void HooksNekoPostProbeLeave()
	{
		// In the queued renderer this marker executes after RenderView has
		// completely finished, including Left4Neko's wrapper code that runs after
		// the raw Neko_Engine_Post draw.  Capture the real D3D9 backbuffer here so
		// the VR eye receives the final wrapped result, and use StretchRect to keep
		// this a format-preserving pixel copy rather than another material pass.
		if (g_NekoPostProbeState.takeoverEnabled &&
			g_NekoPostProbeState.candidateCalls != 0 &&
			Hooks::m_VR &&
			Hooks::m_VR->m_IsVREnabled &&
			Hooks::m_VR->m_L4NNekoEnginePostLaunchEnabled &&
			Hooks::m_VR->m_NekoEnginePostVRTakeover &&
			Hooks::m_VR->m_NekoEnginePostVRCaptureBackBuffer)
		{
			IDirect3DSurface9* targetSurface = nullptr;
			switch (g_NekoPostProbeState.pass)
			{
			case HooksNekoPostPass::MainEye:
				targetSurface = g_NekoPostProbeState.eyeIndex == 1
					? Hooks::m_VR->m_D9LeftEyeSurface
					: Hooks::m_VR->m_D9RightEyeSurface;
				break;
			case HooksNekoPostPass::Scope:
				targetSurface = Hooks::m_VR->m_D9ScopeSurface;
				break;
			case HooksNekoPostPass::RearMirror:
				targetSurface = Hooks::m_VR->m_D9RearMirrorSurface;
				break;
			default:
				break;
			}

			HRESULT getDeviceHr = E_POINTER;
			HRESULT getBackBufferHr = E_POINTER;
			HRESULT copyHr = E_POINTER;
			HRESULT transferHr = S_FALSE;
			HRESULT correctedCopyHr = S_FALSE;
			bool transferApplied = false;
			IDirect3DDevice9* device = nullptr;
			IDirect3DSurface9* backBuffer = nullptr;
			D3DSURFACE_DESC sourceDesc{};
			D3DSURFACE_DESC targetDesc{};
			if (targetSurface)
			{
				targetSurface->AddRef();
				getDeviceHr = targetSurface->GetDevice(&device);
				if (SUCCEEDED(getDeviceHr) && device)
				{
					getBackBufferHr = device->GetBackBuffer(
						0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
					if (SUCCEEDED(getBackBufferHr) && backBuffer)
					{
						backBuffer->GetDesc(&sourceDesc);
						targetSurface->GetDesc(&targetDesc);
						if (Hooks::m_VR->m_NekoEnginePostProbeLog &&
							g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
							g_NekoPostProbeState.eyeIndex == 1)
						{
							HooksNekoPostProbeSurfaceLuma(
								4u, "wrapper-output", device, backBuffer);
						}
						copyHr = device->StretchRect(
							backBuffer,
							nullptr,
							targetSurface,
							nullptr,
							D3DTEXF_NONE);
						if (SUCCEEDED(copyHr) &&
							Hooks::m_VR->m_NekoEnginePostProbeLog &&
							g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
							g_NekoPostProbeState.eyeIndex == 1)
						{
							HooksNekoPostProbeSurfaceLuma(
								8u, "copied-eye", device, targetSurface);
						}

						// Neko's gamma-space final pass raises the VR eye's dark and
						// middle values substantially (the GPU probe sees Y=203 with no
						// pixels below 32).  The desktop compositor normally supplies the
						// complementary output transfer, but our eye RT is submitted as a
						// plain UNORM texture. Use the eye as a temporary sampled copy,
						// apply that missing transfer into the separate backbuffer, then
						// copy the corrected pixels back into the eye. This retains Neko's
						// tonemap/local-contrast result while restoring VR contrast.
						if (SUCCEEDED(copyHr) &&
							Hooks::m_VR->m_NekoEnginePostVROutputGammaCorrection &&
							!Hooks::m_VR->m_NekoEnginePostVRDecodeInputSrgb)
						{
							transferHr = HooksNekoPostApplyColorTransfer(
								Hooks::m_VR,
								device,
								targetSurface,
								backBuffer,
								Hooks::m_VR->m_NekoEnginePostVROutputGamma,
								false);
							if (SUCCEEDED(transferHr))
							{
								correctedCopyHr = device->StretchRect(
									backBuffer,
									nullptr,
									targetSurface,
									nullptr,
									D3DTEXF_NONE);
								transferApplied = SUCCEEDED(correctedCopyHr);
								if (transferApplied)
									copyHr = correctedCopyHr;
							}
							if (transferApplied &&
								Hooks::m_VR->m_NekoEnginePostProbeLog &&
								g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
								g_NekoPostProbeState.eyeIndex == 1)
							{
								HooksNekoPostProbeSurfaceLuma(
									16u, "transfer-output", device, backBuffer);
								HooksNekoPostProbeSurfaceLuma(
									32u, "corrected-eye", device, targetSurface);
							}
						}
					}
				}
			}

			static std::atomic<unsigned int> s_tailCaptureLogBudget{ 16 };
			unsigned int remaining =
				s_tailCaptureLogBudget.load(std::memory_order_acquire);
			while (remaining > 0 &&
				!s_tailCaptureLogBudget.compare_exchange_weak(
					remaining,
					remaining - 1,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
			}
			if (remaining > 0)
			{
				Game::logMsg(
					"[VR][NekoPostTakeover][PassEndCapture] copied=%d transfer=%d gamma=%.3f pass=%s eye=%d target=%s src=%ux%u fmt=%d ms=%d dst=%ux%u fmt=%d ms=%d getDev=0x%08lx getBB=0x%08lx copy=0x%08lx transferHr=0x%08lx correctedCopy=0x%08lx tid=%lu",
					SUCCEEDED(copyHr) ? 1 : 0,
					transferApplied ? 1 : 0,
					Hooks::m_VR->m_NekoEnginePostVROutputGamma,
					HooksNekoPostPassName(g_NekoPostProbeState.pass),
					g_NekoPostProbeState.eyeIndex,
					DebugTextureName(g_NekoPostProbeState.takeoverTarget),
					sourceDesc.Width,
					sourceDesc.Height,
					static_cast<int>(sourceDesc.Format),
					static_cast<int>(sourceDesc.MultiSampleType),
					targetDesc.Width,
					targetDesc.Height,
					static_cast<int>(targetDesc.Format),
					static_cast<int>(targetDesc.MultiSampleType),
					static_cast<unsigned long>(getDeviceHr),
					static_cast<unsigned long>(getBackBufferHr),
					static_cast<unsigned long>(copyHr),
					static_cast<unsigned long>(transferHr),
					static_cast<unsigned long>(correctedCopyHr),
					GetCurrentThreadId());
			}

			if (backBuffer)
				backBuffer->Release();
			if (device)
				device->Release();
			if (targetSurface)
				targetSurface->Release();
		}

		if (g_NekoPostProbeState.frameSerial != 0)
		{
			ITexture* currentRt = nullptr;
			ITexture* copy0 = nullptr;
			ITexture* copy1 = nullptr;
			int vpX = 0;
			int vpY = 0;
			int vpW = 0;
			int vpH = 0;
			const bool haveViewport = DebugGetViewport(
				g_NekoPostProbeState.context, vpX, vpY, vpW, vpH);
			if (g_NekoPostProbeState.context)
			{
				__try
				{
					currentRt = g_NekoPostProbeState.context->GetRenderTarget();
					void** const vtable = *reinterpret_cast<void***>(g_NekoPostProbeState.context);
					if (vtable && vtable[19])
					{
						using GetFramebufferCopyFn = ITexture* (__thiscall*)(void*, int);
						GetFramebufferCopyFn const getCopy =
							reinterpret_cast<GetFramebufferCopyFn>(vtable[19]);
						copy0 = getCopy(g_NekoPostProbeState.context, 0);
						copy1 = getCopy(g_NekoPostProbeState.context, 1);
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					currentRt = nullptr;
					copy0 = nullptr;
					copy1 = nullptr;
				}
			}
			Game::logMsg(
				"[VR][NekoPostProbe][PassEnd] frame=%llu pass=%s eye=%d calls=%u tid=%lu rt=%s vp=%d,%d %dx%d haveVp=%d fb0=%s fb1=%s",
				static_cast<unsigned long long>(g_NekoPostProbeState.frameSerial),
				HooksNekoPostPassName(g_NekoPostProbeState.pass),
				g_NekoPostProbeState.eyeIndex,
				g_NekoPostProbeState.candidateCalls,
				GetCurrentThreadId(),
				DebugTextureName(currentRt),
				vpX, vpY, vpW, vpH, haveViewport ? 1 : 0,
				DebugTextureName(copy0),
				DebugTextureName(copy1));
		}
		ITexture* const takeoverTarget = g_NekoPostProbeState.takeoverTarget;
		g_NekoPostProbeState = HooksNekoPostProbeThreadState{};
		if (takeoverTarget)
			takeoverTarget->Release();
	}

	class HooksNekoPostProbePassFunctor final : public CFunctor
	{
	public:
		HooksNekoPostProbePassFunctor(
			IMatRenderContext* context,
			std::uint64_t frameSerial,
			HooksNekoPostPass pass,
			int eyeIndex,
			ITexture* takeoverTarget,
			bool takeoverEnabled,
			bool begin)
			: m_Context(context),
			m_FrameSerial(frameSerial),
			m_Pass(pass),
			m_EyeIndex(eyeIndex),
			m_TakeoverTarget(takeoverTarget),
			m_TakeoverEnabled(takeoverEnabled),
			m_Begin(begin)
		{
			if (m_TakeoverTarget)
				m_TakeoverTarget->AddRef();
		}

		~HooksNekoPostProbePassFunctor() override
		{
			if (m_TakeoverTarget)
				m_TakeoverTarget->Release();
		}

		int AddRef() override
		{
			return static_cast<int>(m_Refs.fetch_add(1, std::memory_order_acq_rel) + 1);
		}

		int Release() override
		{
			const long remaining = m_Refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (remaining == 0)
				delete this;
			return static_cast<int>(remaining);
		}

		void operator()() override
		{
			if (m_Begin)
				HooksNekoPostProbeEnter(
					m_Context,
					m_FrameSerial,
					m_Pass,
					m_EyeIndex,
					m_TakeoverTarget,
					m_TakeoverEnabled);
			else
				HooksNekoPostProbeLeave();
		}

	private:
		std::atomic<long> m_Refs{ 0 };
		IMatRenderContext* m_Context = nullptr;
		std::uint64_t m_FrameSerial = 0;
		HooksNekoPostPass m_Pass = HooksNekoPostPass::Outside;
		int m_EyeIndex = 0;
		ITexture* m_TakeoverTarget = nullptr;
		bool m_TakeoverEnabled = false;
		bool m_Begin = false;
	};

	class ScopedNekoPostProbePass
	{
	public:
		ScopedNekoPostProbePass(
			IMatRenderContext* context,
			int queueMode,
			std::uint64_t frameSerial,
			HooksNekoPostPass pass,
			int eyeIndex,
			ITexture* takeoverTarget,
			bool takeoverEnabled)
			: m_FrameSerial(frameSerial),
			m_Pass(pass),
			m_EyeIndex(eyeIndex)
		{
			if (!context || (frameSerial == 0 && !takeoverEnabled))
				return;

			if (queueMode != 0)
			{
				m_CallQueue = context->GetCallQueue();
				if (!m_CallQueue)
				{
					static std::atomic<bool> s_loggedMissingQueue{ false };
					if (!s_loggedMissingQueue.exchange(true, std::memory_order_acq_rel))
						Game::logMsg("[VR][NekoPostProbe] call queue unavailable; sampled queued passes cannot be labelled");
					return;
				}
				m_CallQueue->QueueFunctor(new HooksNekoPostProbePassFunctor(
					context,
					m_FrameSerial,
					m_Pass,
					m_EyeIndex,
					takeoverTarget,
					takeoverEnabled,
					true));
			}
			else
			{
				HooksNekoPostProbeEnter(
					context,
					m_FrameSerial,
					m_Pass,
					m_EyeIndex,
					takeoverTarget,
					takeoverEnabled);
			}
			m_Active = true;
		}

		~ScopedNekoPostProbePass()
		{
			if (!m_Active)
				return;
			if (m_CallQueue)
			{
				m_CallQueue->QueueFunctor(new HooksNekoPostProbePassFunctor(
					nullptr,
					m_FrameSerial,
					m_Pass,
					m_EyeIndex,
					nullptr,
					false,
					false));
			}
			else
			{
				HooksNekoPostProbeLeave();
			}
		}

	private:
		ICallQueue* m_CallQueue = nullptr;
		std::uint64_t m_FrameSerial = 0;
		HooksNekoPostPass m_Pass = HooksNekoPostPass::Outside;
		int m_EyeIndex = 0;
		bool m_Active = false;
	};

	std::uint64_t HooksNekoPostProbeSampleFrame(VR* vr)
	{
		if (!vr || !vr->m_L4NNekoEnginePostLaunchEnabled ||
			!vr->m_NekoEnginePostProbeLog ||
			vr->m_NekoEnginePostProbeLogHz <= 0.0f)
		{
			return 0;
		}

		static thread_local std::chrono::steady_clock::time_point s_lastSample{};
		if (ShouldThrottleLog(s_lastSample, vr->m_NekoEnginePostProbeLogHz))
			return 0;
		return g_NekoPostProbeFrameSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	bool HooksNekoPostContainsI(const char* value, const char* needle)
	{
		if (!value || !needle || !*needle)
			return false;
		const size_t needleLength = std::strlen(needle);
		for (const char* cursor = value; *cursor; ++cursor)
		{
			if (_strnicmp(cursor, needle, needleLength) == 0)
				return true;
		}
		return false;
	}
}

static DWORD TimedWaitForPoseEvent(HANDLE event, DWORD timeoutMs)
{
	VR* const vr = Hooks::m_VR;
	if (!vr || !vr->m_RenderPipelineDebugLog)
		return WaitForSingleObject(event, timeoutMs);

	const auto waitStart = std::chrono::steady_clock::now();
	const DWORD result = WaitForSingleObject(event, timeoutMs);
	const auto waitEnd = std::chrono::steady_clock::now();

	vr->m_PoseWaitCount.fetch_add(1, std::memory_order_relaxed);
	const uint64_t waitUs = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(
			waitEnd - waitStart).count());
	const uint64_t requestedUs = static_cast<uint64_t>(timeoutMs) * 1000ull;
	if (waitUs > requestedUs + 3000ull)
	{
		vr->m_PoseWaitOvershootCount.fetch_add(1, std::memory_order_relaxed);
		uint64_t previousMax =
			vr->m_PoseWaitOvershootUsMax.load(std::memory_order_relaxed);
		while (waitUs > previousMax &&
			!vr->m_PoseWaitOvershootUsMax.compare_exchange_weak(
				previousMax,
				waitUs,
				std::memory_order_relaxed))
		{
		}
	}

	return result;
}

bool __fastcall Hooks::dFirstPersonBodyRenderableShouldDraw(void* ecx, void* edx)
{
	const bool originalResult = hkFirstPersonBodyRenderableShouldDraw.fOriginal
		? hkFirstPersonBodyRenderableShouldDraw.fOriginal(ecx)
		: false;

	void* const localRenderable =
		g_FirstPersonBodyLocalRenderable.load(std::memory_order_acquire);
	if (!ecx || ecx != localRenderable ||
		!g_FirstPersonBodyPlayerReady.load(std::memory_order_acquire) ||
		!m_VR || !m_VR->m_IsVREnabled || !m_VR->m_FirstPersonBodyEnabled)
	{
		return originalResult;
	}

	// L4D2 can evaluate ShouldDraw while the local player is still in its
	// spawn transition and never repeat the registration decision after the
	// player becomes alive. Keep the renderable registered; GetModel below
	// withholds the world model until the player is actually drawable.
	return true;
}

void* __fastcall Hooks::dFirstPersonBodyRenderableGetModel(void* ecx, void* edx)
{
	void* const originalModel = hkFirstPersonBodyRenderableGetModel.fOriginal
		? hkFirstPersonBodyRenderableGetModel.fOriginal(ecx)
		: nullptr;

	void* const localRenderable =
		g_FirstPersonBodyLocalRenderable.load(std::memory_order_acquire);
	C_BasePlayer* const localPlayer =
		g_FirstPersonBodyLocalPlayer.load(std::memory_order_acquire);
	if (!ecx || ecx != localRenderable || !localPlayer ||
		!g_FirstPersonBodyPlayerReady.load(std::memory_order_acquire) ||
		!m_VR || !m_VR->m_IsVREnabled || !m_VR->m_FirstPersonBodyEnabled ||
		!m_Game || !m_Game->m_ModelInfo)
	{
		return originalModel;
	}

	constexpr std::ptrdiff_t kModelIndexOffset = 0x140;
	constexpr std::ptrdiff_t kEffectsOffset = 0xE0;
	constexpr std::ptrdiff_t kLifeStateOffset = 0x147;
	constexpr int kEffectNoDraw = 0x20;
	std::uint16_t modelIndex = 0;
	int effects = kEffectNoDraw;
	std::uint8_t lifeState = 2;
	bool incapacitated = false;
	__try
	{
		const std::uint8_t* const playerBytes =
			reinterpret_cast<const std::uint8_t*>(localPlayer);
		modelIndex = *reinterpret_cast<const std::uint16_t*>(
			playerBytes + kModelIndexOffset);
		effects = *reinterpret_cast<const int*>(playerBytes + kEffectsOffset);
		lifeState = *(playerBytes + kLifeStateOffset);
		incapacitated = *(playerBytes + 0x1EA9) != 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return originalModel;
	}

	const bool anchorLifeStateValid =
		lifeState == 0 || (incapacitated && lifeState == 1);
	if ((effects & kEffectNoDraw) != 0 || !anchorLifeStateValid || modelIndex == 0)
		return originalModel;

	void* indexedModel = nullptr;
	__try
	{
		indexedModel = m_Game->m_ModelInfo->GetModel(static_cast<int>(modelIndex));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		indexedModel = nullptr;
	}

	return indexedModel ? indexedModel : originalModel;
}


int __fastcall Hooks::dFirstPersonBodyRenderableDrawModel(
    void* ecx,
    void* edx,
    int flags,
    const void* instance)
{
    void* const localRenderable =
        g_FirstPersonBodyLocalRenderable.load(std::memory_order_acquire);
    C_BasePlayer* const localPlayer =
        g_FirstPersonBodyLocalPlayer.load(std::memory_order_acquire);
    const bool featureOwnsLocalRenderable =
        ecx && ecx == localRenderable && localPlayer &&
        g_FirstPersonBodyPlayerReady.load(std::memory_order_acquire) &&
        g_FirstPersonBodyActualFirstPerson.load(std::memory_order_acquire) &&
        m_VR && m_VR->m_IsVREnabled && m_VR->m_FirstPersonBodyEnabled;

    if (!featureOwnsLocalRenderable)
    {
        return hkFirstPersonBodyRenderableDrawModel.fOriginal
            ? hkFirstPersonBodyRenderableDrawModel.fOriginal(ecx, flags, instance)
            : 0;
    }

    HooksFirstPersonBodyEyeSceneState* const bodyState =
        g_FirstPersonBodyPublishedState.load(std::memory_order_acquire);
    const bool eyeActive =
        bodyState != nullptr && bodyState->bodyActive &&
        bodyState->localPlayerRenderable == localRenderable &&
        bodyState->playerGeneration ==
            g_FirstPersonBodyPlayerGeneration.load(std::memory_order_acquire) &&
        InterlockedCompareExchange(
            &g_FirstPersonBodyEyeSceneActive, 0, 0) != 0;

    // ShouldDraw/GetModel stay enabled between eye calls solely to keep Source's
    // leaf registration alive. Never let that registration draw pixels into a
    // desktop, water, reflection, shadow-only, or other non-eye RenderView.
    if (!eyeActive)
        return 0;

    const int originalResult = hkFirstPersonBodyRenderableDrawModel.fOriginal
        ? hkFirstPersonBodyRenderableDrawModel.fOriginal(ecx, flags, instance)
        : 0;
    if (originalResult != 0)
        return originalResult;

    // The engine has already selected this renderable and entered the native
    // player DrawModel stage. CTerrorPlayer rejects the local first-person
    // color draw here, so submit the indexed world model while Source's model
    // and material state is still valid. This avoids the out-of-band RenderView
    // submission that previously invalidated queued DXVK shader constants.
    static thread_local bool s_nativeStageSubmissionActive = false;
    if (s_nativeStageSubmissionActive)
        return originalResult;

    s_nativeStageSubmissionActive = true;
    int bypassResult = 0;
    const bool submitted = HooksFirstPersonBodySubmitNativeStageModelSafe(
        localPlayer,
        ecx,
        flags,
        &bypassResult);
    s_nativeStageSubmissionActive = false;

    if (!submitted)
    {
        static std::atomic<bool> s_loggedSubmissionFailure{ false };
        if (!s_loggedSubmissionFailure.exchange(true, std::memory_order_acq_rel))
            Game::logMsg("[VR][FirstPersonBody] native-stage body submission failed");
        return originalResult;
    }

    return bypassResult;
}

ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
	ITexture* result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dDrawScreenSpaceRectangle(
	void* ecx,
	void* edx,
	IMaterial* material,
	int destX,
	int destY,
	int width,
	int height,
	float srcX0,
	float srcY0,
	float srcX1,
	float srcY1,
	int srcWidth,
	int srcHeight,
	void* clientRenderable,
	int xDice,
	int yDice)
{
	// This detour is installed only for the explicit L4N launch mode. Keep a
	// second runtime guard so a partially initialized/tearing-down VR object can
	// never apply Neko material, RT, framebuffer-copy, or diagnostic behavior to
	// an unrelated post-processing path.
	if (m_VR && !m_VR->m_L4NNekoEnginePostLaunchEnabled)
	{
		if (hkDrawScreenSpaceRectangle.fOriginal)
		{
			hkDrawScreenSpaceRectangle.fOriginal(
				ecx, material, destX, destY, width, height,
				srcX0, srcY0, srcX1, srcY1, srcWidth, srcHeight,
				clientRenderable, xDice, yDice);
		}
		return;
	}

	static std::atomic<ULONGLONG> s_desktopNekoFirstCandidateTick{ 0 };
	static std::atomic<bool> s_desktopNekoBaselineCaptured{ false };
	constexpr ULONGLONG kDesktopNekoBaselineDelayMs = 20000;
	const char* const processCommandLine = GetCommandLineA();
	const bool noHmdLaunch =
		processCommandLine &&
		HooksNekoPostContainsI(processCommandLine, "-nohmd");
	if (noHmdLaunch)
	{
		static std::atomic<bool> s_loggedDesktopProbeArmed{ false };
		if (!s_loggedDesktopProbeArmed.exchange(true, std::memory_order_acq_rel))
		{
			Game::logMsg(
				"[VR][NekoPostDesktopBaseline] armed=1 trigger=-nohmd vr=%p game=%p tid=%lu",
				m_VR,
				m_Game,
				GetCurrentThreadId());
		}
	}
	const bool probeActive =
		g_NekoPostProbeState.frameSerial != 0 &&
		m_VR &&
		m_VR->m_L4NNekoEnginePostLaunchEnabled &&
		m_VR->m_NekoEnginePostProbeLog;
	const bool takeoverPassActive =
		g_NekoPostProbeState.takeoverEnabled &&
		g_NekoPostProbeState.takeoverTarget != nullptr &&
		m_VR &&
		m_VR->m_IsVREnabled &&
		m_VR->m_L4NNekoEnginePostLaunchEnabled &&
		m_VR->m_NekoEnginePostVRTakeover;
	// -nohmd keeps this DLL and Left4Neko active but intentionally allocates no
	// eye surfaces.  Observe the native final Neko draw in that mode so its real
	// sampler inputs can be compared with the VR substitutions.  Wait a few
	// seconds after the first in-game candidate so startup/default CVar values do
	// not win the one-shot capture before the persisted filter settings settle.
	const bool desktopBaselinePending =
		noHmdLaunch &&
		!s_desktopNekoBaselineCaptured.load(std::memory_order_acquire);
	if (!probeActive && !takeoverPassActive && !desktopBaselinePending)
	{
		if (hkDrawScreenSpaceRectangle.fOriginal)
		{
			hkDrawScreenSpaceRectangle.fOriginal(
				ecx,
				material,
				destX,
				destY,
				width,
				height,
				srcX0,
				srcY0,
				srcX1,
				srcY1,
				srcWidth,
				srcHeight,
				clientRenderable,
				xDice,
				yDice);
		}
		return;
	}

	char materialName[160] = "<null>";
	char shaderName[96] = "<null>";
	if (material)
	{
		__try
		{
			const char* const name = material->GetName();
			const char* const shader = material->GetShaderName();
			if (name && *name)
				strncpy_s(materialName, name, _TRUNCATE);
			if (shader && *shader)
				strncpy_s(shaderName, shader, _TRUNCATE);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			strcpy_s(materialName, "<bad-material>");
			strcpy_s(shaderName, "<bad-shader>");
		}
	}

	const bool candidate =
		HooksNekoPostContainsI(materialName, "dev/engine_post") ||
		HooksNekoPostContainsI(materialName, "neko_engine_post") ||
		HooksNekoPostContainsI(shaderName, "neko_engine_post");
	bool desktopBaseline = false;
	if (desktopBaselinePending && candidate)
	{
		const ULONGLONG now = GetTickCount64();
		ULONGLONG firstCandidate =
			s_desktopNekoFirstCandidateTick.load(std::memory_order_acquire);
		if (firstCandidate == 0)
		{
			s_desktopNekoFirstCandidateTick.compare_exchange_strong(
				firstCandidate,
				now,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			firstCandidate =
				s_desktopNekoFirstCandidateTick.load(std::memory_order_acquire);
			if (firstCandidate == now)
			{
				Game::logMsg(
					"[VR][NekoPostDesktopBaseline] candidate=1 waitingMs=%llu mat=%s shader=%s tid=%lu",
					static_cast<unsigned long long>(kDesktopNekoBaselineDelayMs),
					materialName,
					shaderName,
					GetCurrentThreadId());
			}
		}
		if (now >= firstCandidate + kDesktopNekoBaselineDelayMs)
		{
			bool expected = false;
			desktopBaseline =
				s_desktopNekoBaselineCaptured.compare_exchange_strong(
					expected,
					true,
					std::memory_order_acq_rel,
					std::memory_order_acquire);
		}
	}
	const bool sampled = probeActive && candidate;
	const bool takeover = takeoverPassActive && candidate;
	const unsigned int callIndex = desktopBaseline
		? 1u
		: (sampled || takeover)
		? ++g_NekoPostProbeState.candidateCalls
		: 0;
	if ((desktopBaseline ||
		(takeover &&
			g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
			g_NekoPostProbeState.eyeIndex == 1)) &&
		material && callIndex == 1)
	{
		static std::atomic<bool> s_loggedNekoParameters{ false };
		if (!s_loggedNekoParameters.exchange(true, std::memory_order_acq_rel))
		{
			ITexture* const baseTexture =
				HooksNekoPostReadMaterialTexture(material, "$BASETEXTURE");
			ITexture* const framebufferTexture =
				HooksNekoPostReadMaterialTexture(material, "$FBTEXTURE");
			Game::logMsg(
				"[VR][NekoPostParams][Material] mode=%s base=%s fb=%s allowAfter=%d allowVignette=%d allowNoise=%d allowLocal=%d aa=%d bloom=%d nekoBloom=%d after=%d local=%d vignette=%d blurredVignette=%d noise=%d vomit=%d fade=%d desaturate=%d lookups=%d linearWrite=%d linearBase=%d linear1=%d tvGamma=%.4f noiseScale=%.4f",
				desktopBaseline ? "desktop-nohmd" : "vr-takeover",
				DebugTextureName(baseTexture),
				DebugTextureName(framebufferTexture),
				HooksNekoPostReadMaterialInt(material, "$ALLOW_AFTER"),
				HooksNekoPostReadMaterialInt(material, "$ALLOWVIGNETTE"),
				HooksNekoPostReadMaterialInt(material, "$ALLOWNOISE"),
				HooksNekoPostReadMaterialInt(material, "$ALLOWLOCALCONTRAST"),
				HooksNekoPostReadMaterialInt(material, "$AAENABLE"),
				HooksNekoPostReadMaterialInt(material, "$BLOOMENABLE"),
				HooksNekoPostReadMaterialInt(material, "$NEKO_BLOOM"),
				HooksNekoPostReadMaterialInt(material, "$AFTER"),
				HooksNekoPostReadMaterialInt(material, "$LOCALCONTRASTENABLE"),
				HooksNekoPostReadMaterialInt(material, "$VIGNETTEENABLE"),
				HooksNekoPostReadMaterialInt(material, "$BLURREDVIGNETTEENABLE"),
				HooksNekoPostReadMaterialInt(material, "$NOISEENABLE"),
				HooksNekoPostReadMaterialInt(material, "$VOMITENABLE"),
				HooksNekoPostReadMaterialInt(material, "$FADE"),
				HooksNekoPostReadMaterialInt(material, "$DESATURATEENABLE"),
				HooksNekoPostReadMaterialInt(material, "$NUM_LOOKUPS"),
				HooksNekoPostReadMaterialInt(material, "$LINEARWRITE"),
				HooksNekoPostReadMaterialInt(material, "$LINEARREAD_BASETEXTURE"),
				HooksNekoPostReadMaterialInt(material, "$LINEARREAD_TEXTURE1"),
				HooksNekoPostReadMaterialFloat(material, "$TV_GAMMA"),
				HooksNekoPostReadMaterialFloat(material, "$NOISESCALE"));

			if (m_Game)
			{
				Game::logMsg(
					"[VR][NekoPostParams][CVar] mode=%s gamma=%.4f tonemap=%d forceLinear=%d allowInvert=%d preTonemap=%d after=%d adaptedLum=%.4f lumCompareScale=%.4f lumAllowTonemap=%d bloomMode=%d bloomScale=%.4f localScale=%.4f localEdge=%.4f vignette=%d colorCorrection=%d",
					desktopBaseline ? "desktop-nohmd" : "vr-takeover",
					m_Game->GetConVarFloat("mat_neko_gamma", -9999.0f),
					m_Game->GetConVarInt("mat_neko_tonemapping_algorithm", -9999),
					m_Game->GetConVarInt("mat_neko_tonemapping_force_linear", -9999),
					m_Game->GetConVarInt("mat_neko_allow_invert_tonemap", -9999),
					m_Game->GetConVarInt("mat_neko_pre_tonemapping", -9999),
					m_Game->GetConVarInt("mat_neko_engine_post_after", -9999),
					m_Game->GetConVarFloat("mat_neko_engine_post_adapted_lum", -9999.0f),
					m_Game->GetConVarFloat("mat_neko_lumance_compare_scale", -9999.0f),
					m_Game->GetConVarInt("mat_neko_luminance_compare_allow_tonemap", -9999),
					m_Game->GetConVarInt("mat_nekobloom_blend_mode", -9999),
					m_Game->GetConVarFloat("mat_nekobloom_scale", -9999.0f),
					m_Game->GetConVarFloat("mat_local_contrast_scale_override", -9999.0f),
					m_Game->GetConVarFloat("mat_local_contrast_edge_scale_override", -9999.0f),
					m_Game->GetConVarInt("mat_vignette_enable", -9999),
					m_Game->GetConVarInt("mat_colorcorrection", -9999));
			}
		}
	}

	IMatRenderContext* const context =
		reinterpret_cast<IMatRenderContext*>(ecx);
	ITexture* beforeRt = nullptr;
	ITexture* beforeCopy0 = nullptr;
	ITexture* beforeCopy1 = nullptr;
	int beforeVpX = 0;
	int beforeVpY = 0;
	int beforeVpW = 0;
	int beforeVpH = 0;
	bool haveBeforeViewport = false;
	if ((sampled || desktopBaseline) && context)
	{
		haveBeforeViewport = DebugGetViewport(
			context, beforeVpX, beforeVpY, beforeVpW, beforeVpH);
		__try
		{
			beforeRt = context->GetRenderTarget();
			void** const vtable = *reinterpret_cast<void***>(context);
			if (vtable && vtable[19])
			{
				using GetFramebufferCopyFn = ITexture* (__thiscall*)(void*, int);
				GetFramebufferCopyFn const getCopy =
					reinterpret_cast<GetFramebufferCopyFn>(vtable[19]);
				beforeCopy0 = getCopy(context, 0);
				beforeCopy1 = getCopy(context, 1);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			beforeRt = nullptr;
			beforeCopy0 = nullptr;
			beforeCopy1 = nullptr;
		}
	}

	ITexture* takeoverPreviousRt = nullptr;
	int takeoverPreviousVpX = 0;
	int takeoverPreviousVpY = 0;
	int takeoverPreviousVpW = 0;
	int takeoverPreviousVpH = 0;
	bool takeoverHavePreviousViewport = false;
	bool takeoverApplied = false;
	bool takeoverCapturedBackBuffer = false;
	const bool takeoverUseBackBufferCapture =
		takeover && m_VR && m_VR->m_NekoEnginePostVRCaptureBackBuffer;
	const bool takeoverUseNativeFullFrameSource =
		takeoverUseBackBufferCapture && m_VR &&
		m_VR->m_NekoEnginePostVRUseNativeFullFrameSource;
	ITexture* takeoverPreviousFramebufferCopy0 = nullptr;
	bool takeoverReboundFramebufferCopy0 = false;
	ITexture* takeoverFramebufferSource =
		g_NekoPostProbeState.takeoverTarget;
	HRESULT takeoverDecodeInputHr = S_FALSE;
	bool takeoverDecodedInput = false;
	bool takeoverHdrSceneInput = false;
	void* takeoverFramebufferTextureVar = nullptr;
	ITexture* takeoverPreviousFramebufferTexture = nullptr;
	bool takeoverReboundFramebufferTexture = false;
	HRESULT takeoverRefreshSmallInputHr = S_FALSE;
	void* takeoverBaseTextureVar = nullptr;
	ITexture* takeoverPreviousBaseTexture = nullptr;
	bool takeoverReboundBaseTexture = false;
	void* takeoverBloomEnableVar = nullptr;
	int takeoverPreviousBloomEnable = 0;
	bool takeoverDisabledStandardBloom = false;
	void* takeoverNekoBloomVar = nullptr;
	int takeoverPreviousNekoBloom = 0;
	int takeoverDrawNekoBloom = 0;
	bool takeoverOverrodeNekoBloom = false;
	bool takeoverTargetCleared = false;
	IDirect3DDevice9* takeoverDevice = nullptr;
	HRESULT takeoverGetActualTargetHr = E_FAIL;
	HRESULT takeoverActualTargetDescHr = E_FAIL;
	HRESULT takeoverClearActualTargetHr = E_FAIL;
	D3DSURFACE_DESC takeoverActualTargetDesc{};
	DWORD takeoverSrgbBefore = 0;
	DWORD takeoverSrgbAfter = 0;
	DWORD takeoverBlendBefore = 0;
	DWORD takeoverBlendAfter = 0;
	DWORD takeoverSrcBlendAfter = 0;
	DWORD takeoverDstBlendAfter = 0;
	DWORD takeoverBlendOpAfter = 0;
	DWORD takeoverSampler0SrgbAfter = 0;
	DWORD takeoverSampler1SrgbAfter = 0;
	bool takeoverHaveD3DState = false;
	if (takeover && context && !takeoverUseBackBufferCapture)
	{
		__try
		{
			if (m_VR && m_VR->m_D9LeftEyeSurface &&
				SUCCEEDED(m_VR->m_D9LeftEyeSurface->GetDevice(&takeoverDevice)) &&
				takeoverDevice)
			{
				takeoverHaveD3DState =
					SUCCEEDED(takeoverDevice->GetRenderState(
						D3DRS_SRGBWRITEENABLE, &takeoverSrgbBefore)) &&
					SUCCEEDED(takeoverDevice->GetRenderState(
						D3DRS_ALPHABLENDENABLE, &takeoverBlendBefore));
			}

			takeoverPreviousRt = context->GetRenderTarget();
			takeoverHavePreviousViewport = DebugGetViewport(
				context,
				takeoverPreviousVpX,
				takeoverPreviousVpY,
				takeoverPreviousVpW,
				takeoverPreviousVpH);
			context->SetRenderTarget(g_NekoPostProbeState.takeoverTarget);
			takeoverApplied = true;
			if (hkViewport.fOriginal)
			{
					hkViewport.fOriginal(
					context,
					destX,
					destY,
					width,
					height);
			}
			// Left4Neko has already captured this eye into framebuffer-copy slot 0.
			// Its normal destination is the default backbuffer, which is not still
			// holding the unprocessed eye scene. A redirected eye RT is, so clear it
			// before the final pass to prevent blend/discard branches from adding the
			// processed result over the original and lifting the whole image to white.
			if (m_VR->m_NekoEnginePostVRClearTarget)
			{
				context->OverrideAlphaWriteEnable(true, true);
				context->ClearColor4ub(0, 0, 0, 255);
				context->ClearBuffers(true, false, false);
				context->OverrideAlphaWriteEnable(false, true);
				takeoverTargetCleared = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			takeoverApplied = false;
			if (takeoverDevice)
			{
				takeoverDevice->Release();
				takeoverDevice = nullptr;
			}
		}
	}
	else if (takeoverUseBackBufferCapture && context)
	{
		__try
		{
			// This is the path used by the default configuration.  L4N has already
			// switched the material-system destination back to the default target,
			// so clear that *actual D3D target* before its final native draw.  Merely
			// clearing the eye ITexture (the direct-path behaviour above) never
			// touched this target.  A black destination is required when any Neko
			// snapshot/branch uses alpha or additive blending; otherwise the old eye
			// image underneath is accumulated a second time and the result washes out.
			if (m_VR && m_VR->m_D9LeftEyeSurface &&
				SUCCEEDED(m_VR->m_D9LeftEyeSurface->GetDevice(&takeoverDevice)) &&
				takeoverDevice)
			{
				// Sample the unprocessed eye before clearing the separate default
				// backbuffer.  This must live in the capture branch: the direct-eye
				// branch above is only an opt-in fallback and is not the default path.
				if (m_VR->m_NekoEnginePostProbeLog &&
					g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
					g_NekoPostProbeState.eyeIndex == 1 &&
					callIndex == 1)
				{
					HooksNekoPostProbeSurfaceLuma(
						1u,
						"input-eye",
						takeoverDevice,
						m_VR->m_D9LeftEyeSurface);
				}
				takeoverHaveD3DState =
					SUCCEEDED(takeoverDevice->GetRenderState(
						D3DRS_SRGBWRITEENABLE, &takeoverSrgbBefore)) &&
					SUCCEEDED(takeoverDevice->GetRenderState(
						D3DRS_ALPHABLENDENABLE, &takeoverBlendBefore));

				// The fallback path replaces L4N's two native post inputs with copies
				// derived from the VR eye.  The desktop baseline proves the native pair
				// is intentional: sampler0 remains _rt_smallfb0 (a nearly constant
				// auxiliary texture) while sampler1 is the FP16 _rt_fullframefb.  Do not
				// rebuild either input when native-source mode is selected.
				IDirect3DSurface9* const eyeSourceSurface =
					g_NekoPostProbeState.eyeIndex == 1
					? m_VR->m_D9LeftEyeSurface
					: (g_NekoPostProbeState.eyeIndex == 2
						? m_VR->m_D9RightEyeSurface
						: nullptr);
				D3DSURFACE_DESC eyeSourceDesc{};
				if (!takeoverUseNativeFullFrameSource &&
					eyeSourceSurface &&
					SUCCEEDED(eyeSourceSurface->GetDesc(&eyeSourceDesc)) &&
					eyeSourceDesc.Format == D3DFMT_A16B16G16R16F)
				{
					takeoverHdrSceneInput = true;
					takeoverFramebufferSource =
						g_NekoPostProbeState.eyeIndex == 1
						? m_VR->m_LeftEyeTexture
						: m_VR->m_RightEyeTexture;
				}
				else if (!takeoverUseNativeFullFrameSource &&
					m_VR->m_NekoEnginePostVRDecodeInputSrgb &&
					eyeSourceSurface &&
					m_VR->m_NekoPostLinearInputTexture &&
					m_VR->m_D9NekoPostLinearInputSurface)
				{
					takeoverDecodeInputHr = HooksNekoPostApplyColorTransfer(
						m_VR,
						takeoverDevice,
						eyeSourceSurface,
						m_VR->m_D9NekoPostLinearInputSurface,
						1.0f,
						true);
					if (SUCCEEDED(takeoverDecodeInputHr))
					{
						takeoverFramebufferSource =
							m_VR->m_NekoPostLinearInputTexture;
						takeoverDecodedInput = true;
						if (m_VR->m_NekoEnginePostProbeLog &&
							g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
							g_NekoPostProbeState.eyeIndex == 1 &&
							callIndex == 1)
						{
							HooksNekoPostProbeSurfaceLuma(
								64u,
								"decoded-input",
								takeoverDevice,
								m_VR->m_D9NekoPostLinearInputSurface);
						}
					}
				}

				// Diagnostic fallback only: synthesize the small input from the eye.
				// Native-source mode deliberately preserves L4N's _rt_smallfb0 exactly.
				IDirect3DSurface9* const smallInputSource =
					takeoverHdrSceneInput
					? eyeSourceSurface
					: (takeoverDecodedInput && m_VR->m_D9NekoPostLinearInputSurface
					? m_VR->m_D9NekoPostLinearInputSurface
					: eyeSourceSurface);
				if (!takeoverUseNativeFullFrameSource &&
					smallInputSource &&
					m_VR->m_NekoPostSmallInputTexture &&
					m_VR->m_D9NekoPostSmallInputSurface)
				{
					takeoverRefreshSmallInputHr = takeoverDevice->StretchRect(
						smallInputSource,
						nullptr,
						m_VR->m_D9NekoPostSmallInputSurface,
						nullptr,
						D3DTEXF_LINEAR);
					if (SUCCEEDED(takeoverRefreshSmallInputHr) &&
						m_VR->m_NekoEnginePostProbeLog &&
						g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
						g_NekoPostProbeState.eyeIndex == 1 &&
						callIndex == 1)
					{
						HooksNekoPostProbeSurfaceLuma(
							512u,
							"small-input",
							takeoverDevice,
							m_VR->m_D9NekoPostSmallInputSurface);
					}
				}

				if (m_VR->m_NekoEnginePostVRClearTarget)
				{
					IDirect3DSurface9* actualTarget = nullptr;
					takeoverGetActualTargetHr =
						takeoverDevice->GetRenderTarget(0, &actualTarget);
					if (SUCCEEDED(takeoverGetActualTargetHr) && actualTarget)
					{
						takeoverActualTargetDescHr =
							actualTarget->GetDesc(&takeoverActualTargetDesc);
						if (SUCCEEDED(takeoverActualTargetDescHr))
						{
							D3DRECT const clearRect = {
								0,
								0,
								static_cast<LONG>(takeoverActualTargetDesc.Width),
								static_cast<LONG>(takeoverActualTargetDesc.Height)
							};
							takeoverClearActualTargetHr = takeoverDevice->Clear(
								1,
								&clearRect,
								D3DCLEAR_TARGET,
								D3DCOLOR_ARGB(255, 0, 0, 0),
								1.0f,
								0);
							takeoverTargetCleared =
								SUCCEEDED(takeoverClearActualTargetHr);
						}
						actualTarget->Release();
					}
				}
			}

			// Neko_Engine_Post obtains the full-frame scene through framebuffer-copy
			// slot 0. Keep L4N's own _rt_FullFrameFB for the normal path: it includes
			// the pre-final-pass data expected by its tonemappers. The eye/linear RT
			// substitution remains available as a fallback. $BaseTexture is the
			// auxiliary _rt_smallfb0 and is rebound separately below.
			void** const contextVtable = *reinterpret_cast<void***>(context);
			if (contextVtable && contextVtable[18] && contextVtable[19])
			{
				using SetFramebufferCopyFn =
					void(__thiscall*)(void*, ITexture*, int);
				using GetFramebufferCopyFn =
					ITexture* (__thiscall*)(void*, int);
				GetFramebufferCopyFn const getFramebufferCopy =
					reinterpret_cast<GetFramebufferCopyFn>(contextVtable[19]);
				SetFramebufferCopyFn const setFramebufferCopy =
					reinterpret_cast<SetFramebufferCopyFn>(contextVtable[18]);
				takeoverPreviousFramebufferCopy0 =
					getFramebufferCopy(context, 0);
				if (takeoverUseNativeFullFrameSource)
				{
					takeoverFramebufferSource =
						takeoverPreviousFramebufferCopy0;
				}
				else
				{
					setFramebufferCopy(
						context,
						takeoverFramebufferSource,
						0);
					takeoverReboundFramebufferCopy0 = true;
				}
			}

			// The Neko shader does not resolve framebuffer-copy slot 0 lazily; its
			// primary sampler is the already-bound $FBTEXTURE. Leave it unchanged in
			// native-source mode, and only substitute it for the fallback path.
			if (material)
			{
				// Bind the synthetic small-frame input only on the diagnostic fallback.
				bool foundBaseTexture = false;
				takeoverBaseTextureVar = material->FindVar(
					"$BASETEXTURE", &foundBaseTexture, false);
				// S_FALSE is the native-path sentinel: no private small texture was
				// refreshed. SUCCEEDED(S_FALSE) is true, so using SUCCEEDED here used
				// to replace L4N's valid _rt_smallfb0 with an uninitialised black
				// nekopostsmallinput0 even while nativeSource=1.
				if (!takeoverUseNativeFullFrameSource &&
					takeoverRefreshSmallInputHr == S_OK &&
					foundBaseTexture && takeoverBaseTextureVar &&
					m_VR->m_NekoPostSmallInputTexture)
				{
					void** const baseVarVtable =
						*reinterpret_cast<void***>(takeoverBaseTextureVar);
					if (baseVarVtable && baseVarVtable[0] && baseVarVtable[14])
					{
						using GetTextureValueFn = ITexture* (__thiscall*)(void*);
						using SetTextureValueFn = void(__thiscall*)(void*, ITexture*);
						takeoverPreviousBaseTexture =
							reinterpret_cast<GetTextureValueFn>(baseVarVtable[0])(
								takeoverBaseTextureVar);
						reinterpret_cast<SetTextureValueFn>(baseVarVtable[14])(
							takeoverBaseTextureVar,
							m_VR->m_NekoPostSmallInputTexture);
						takeoverReboundBaseTexture = true;
					}
				}

				// The passive desktop baseline observes $BLOOMENABLE=0 and
				// $NEKO_BLOOM=1.  L4N's VR invocation can arrive with the standard
				// bloom combo set to 1, which is a real material divergence and lifts
				// midtones.  Match the desktop combo for the raw draw, then restore it;
				// NekoBloom remains enabled and untouched.
				if (takeover)
				{
					bool foundBloomEnable = false;
					takeoverBloomEnableVar = material->FindVar(
						"$BLOOMENABLE", &foundBloomEnable, false);
					if (foundBloomEnable && takeoverBloomEnableVar)
					{
						void** const bloomEnableVtable =
							*reinterpret_cast<void***>(takeoverBloomEnableVar);
						if (bloomEnableVtable &&
							bloomEnableVtable[4] && bloomEnableVtable[26])
						{
							using GetIntValueFn = int(__thiscall*)(void*);
							using SetIntValueFn = void(__thiscall*)(void*, int);
							takeoverPreviousBloomEnable =
								reinterpret_cast<GetIntValueFn>(
									bloomEnableVtable[26])(takeoverBloomEnableVar);
							if (takeoverPreviousBloomEnable != 0)
							{
								reinterpret_cast<SetIntValueFn>(
									bloomEnableVtable[4])(
										takeoverBloomEnableVar,
										0);
								takeoverDisabledStandardBloom = true;
							}
						}
					}
				}

				bool foundFramebufferTexture = false;
				takeoverFramebufferTextureVar = material->FindVar(
					"$FBTEXTURE", &foundFramebufferTexture, false);
				if (foundFramebufferTexture && takeoverFramebufferTextureVar)
				{
					void** const varVtable =
						*reinterpret_cast<void***>(takeoverFramebufferTextureVar);
					if (varVtable && varVtable[0] && varVtable[14])
					{
						using GetTextureValueFn = ITexture* (__thiscall*)(void*);
						using SetTextureValueFn = void(__thiscall*)(void*, ITexture*);
					takeoverPreviousFramebufferTexture =
						reinterpret_cast<GetTextureValueFn>(varVtable[0])(
							takeoverFramebufferTextureVar);
					if (takeoverUseNativeFullFrameSource)
					{
						takeoverFramebufferSource =
							takeoverPreviousFramebufferTexture;
					}
					else
					{
						reinterpret_cast<SetTextureValueFn>(varVtable[14])(
							takeoverFramebufferTextureVar,
							takeoverFramebufferSource);
						takeoverReboundFramebufferTexture = true;
					}
					}
				}

				// Desktop L4N reaches this draw with NEKO_BLOOM=1. In VR the wrapper can
				// leave the same shared material at 0 even though the native auxiliary
				// inputs are present. Match the desktop snapshot in native mode. The
				// explicit fallback diagnostic may still force it off.
				if (takeoverUseNativeFullFrameSource ||
					m_VR->m_NekoEnginePostVRDisableNekoBloom)
				{
					bool foundNekoBloom = false;
					takeoverNekoBloomVar = material->FindVar(
						"$NEKO_BLOOM", &foundNekoBloom, false);
					if (foundNekoBloom && takeoverNekoBloomVar)
					{
						void** const bloomVarVtable =
							*reinterpret_cast<void***>(takeoverNekoBloomVar);
						if (bloomVarVtable && bloomVarVtable[4] && bloomVarVtable[26])
						{
							using GetIntValueFn = int(__thiscall*)(void*);
							using SetIntValueFn = void(__thiscall*)(void*, int);
							takeoverPreviousNekoBloom =
								reinterpret_cast<GetIntValueFn>(bloomVarVtable[26])(
									takeoverNekoBloomVar);
							takeoverDrawNekoBloom = takeoverUseNativeFullFrameSource
								? 1
								: 0;
							if (takeoverPreviousNekoBloom != takeoverDrawNekoBloom)
							{
								reinterpret_cast<SetIntValueFn>(bloomVarVtable[4])(
									takeoverNekoBloomVar,
									takeoverDrawNekoBloom);
								takeoverOverrodeNekoBloom = true;
							}
						}
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// Keep any successfully-completed rebind flags intact so the cleanup
			// below can restore state even if the other rebind faults midway.
			static std::atomic<bool> s_loggedFramebufferRebindFault{ false };
			if (!s_loggedFramebufferRebindFault.exchange(
				true, std::memory_order_acq_rel))
			{
				Game::logMsg(
					"[VR][NekoPostTakeover] framebuffer source rebind fault");
			}
		}
	}

	if (hkDrawScreenSpaceRectangle.fOriginal)
	{
		hkDrawScreenSpaceRectangle.fOriginal(
			ecx,
			material,
			destX,
			destY,
			width,
			height,
			srcX0,
			srcY0,
			srcX1,
			srcY1,
			srcWidth,
			srcHeight,
			clientRenderable,
			xDice,
			yDice);
	}

	if (desktopBaseline)
	{
		IDirect3DDevice9* desktopDevice = nullptr;
		const HRESULT getDeviceHr = g_D3DVR9
			? g_D3DVR9->GetD3DDevice(&desktopDevice)
			: E_NOINTERFACE;
		if (SUCCEEDED(getDeviceHr) && desktopDevice)
		{
			HooksNekoPostProbeBoundSamplers(desktopDevice);
			IDirect3DSurface9* desktopOutput = nullptr;
			const HRESULT getOutputHr =
				desktopDevice->GetRenderTarget(0, &desktopOutput);
			if (SUCCEEDED(getOutputHr) && desktopOutput)
			{
				HooksNekoPostProbeSurfaceLuma(
					2048u,
					"desktop-raw-neko-output",
					desktopDevice,
					desktopOutput);
				desktopOutput->Release();
			}
			Game::logMsg(
				"[VR][NekoPostDesktopBaseline] captured=1 getDevice=0x%08lx getOutput=0x%08lx mat=%s shader=%s tid=%lu",
				static_cast<unsigned long>(getDeviceHr),
				static_cast<unsigned long>(getOutputHr),
				materialName,
				shaderName,
				GetCurrentThreadId());
			desktopDevice->Release();
		}
		else
		{
			Game::logMsg(
				"[VR][NekoPostDesktopBaseline] captured=0 getDevice=0x%08lx mat=%s shader=%s tid=%lu",
				static_cast<unsigned long>(getDeviceHr),
				materialName,
				shaderName,
				GetCurrentThreadId());
		}
	}

	if (takeoverUseBackBufferCapture && takeoverDevice && m_VR &&
		m_VR->m_NekoEnginePostProbeLog &&
		g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
		g_NekoPostProbeState.eyeIndex == 1 &&
		callIndex == 1)
	{
		HooksNekoPostProbeBoundSamplers(takeoverDevice);
		IDirect3DSurface9* rawOutput = nullptr;
		if (SUCCEEDED(takeoverDevice->GetRenderTarget(0, &rawOutput)) && rawOutput)
		{
			HooksNekoPostProbeSurfaceLuma(
				2u, "raw-neko-output", takeoverDevice, rawOutput);
			rawOutput->Release();
		}
	}

	if (takeoverUseBackBufferCapture && takeoverDevice && m_VR &&
		m_VR->m_NekoEnginePostProbeLog &&
		g_NekoPostProbeState.pass == HooksNekoPostPass::MainEye &&
		callIndex == 1)
	{
		IDirect3DSurface9* temporalOutput = nullptr;
		if (SUCCEEDED(takeoverDevice->GetRenderTarget(0, &temporalOutput)) &&
			temporalOutput)
		{
			HooksNekoPostProbeTemporalState(takeoverDevice, temporalOutput);
			temporalOutput->Release();
		}
	}

	if (takeoverReboundBaseTexture && takeoverBaseTextureVar)
	{
		__try
		{
			void** const baseVarVtable =
				*reinterpret_cast<void***>(takeoverBaseTextureVar);
			if (baseVarVtable && baseVarVtable[14])
			{
				using SetTextureValueFn = void(__thiscall*)(void*, ITexture*);
				reinterpret_cast<SetTextureValueFn>(baseVarVtable[14])(
					takeoverBaseTextureVar,
					takeoverPreviousBaseTexture);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static std::atomic<bool> s_loggedBaseTextureRestoreFault{ false };
			if (!s_loggedBaseTextureRestoreFault.exchange(
				true, std::memory_order_acq_rel))
			{
				Game::logMsg(
					"[VR][NekoPostTakeover] $BASETEXTURE restore fault");
			}
		}
	}

	// In native takeover mode this is a shared queued material, not draw-local
	// state. Restoring 1 immediately after recording the draw races the render
	// worker and makes successive eyes/frames alternate between the desktop
	// combination (0/1) and L4N's pre-draw combination (1/0). Keep the verified
	// desktop value stable between native draws. The diagnostic fallback still
	// restores its temporary override.
	if (takeoverDisabledStandardBloom && takeoverBloomEnableVar &&
		!takeoverUseNativeFullFrameSource)
	{
		__try
		{
			void** const bloomEnableVtable =
				*reinterpret_cast<void***>(takeoverBloomEnableVar);
			if (bloomEnableVtable && bloomEnableVtable[4])
			{
				using SetIntValueFn = void(__thiscall*)(void*, int);
				reinterpret_cast<SetIntValueFn>(bloomEnableVtable[4])(
					takeoverBloomEnableVar,
					takeoverPreviousBloomEnable);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static std::atomic<bool> s_loggedBloomEnableRestoreFault{ false };
			if (!s_loggedBloomEnableRestoreFault.exchange(
				true, std::memory_order_acq_rel))
			{
				Game::logMsg(
					"[VR][NekoPostTakeover] $BLOOMENABLE restore fault");
			}
		}
	}

	if (takeoverReboundFramebufferCopy0 && context)
	{
		__try
		{
			void** const contextVtable = *reinterpret_cast<void***>(context);
			if (contextVtable && contextVtable[18])
			{
				using SetFramebufferCopyFn =
					void(__thiscall*)(void*, ITexture*, int);
				reinterpret_cast<SetFramebufferCopyFn>(contextVtable[18])(
					context,
					takeoverPreviousFramebufferCopy0,
					0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static std::atomic<bool> s_loggedFramebufferCopyRestoreFault{ false };
			if (!s_loggedFramebufferCopyRestoreFault.exchange(
				true, std::memory_order_acq_rel))
			{
				Game::logMsg(
					"[VR][NekoPostTakeover] framebuffer-copy slot 0 restore fault");
			}
		}
	}

	if (takeoverReboundFramebufferTexture && takeoverFramebufferTextureVar)
	{
		__try
		{
			void** const varVtable =
				*reinterpret_cast<void***>(takeoverFramebufferTextureVar);
			if (varVtable && varVtable[14])
			{
				using SetTextureValueFn = void(__thiscall*)(void*, ITexture*);
				reinterpret_cast<SetTextureValueFn>(varVtable[14])(
					takeoverFramebufferTextureVar,
					takeoverPreviousFramebufferTexture);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static std::atomic<bool> s_loggedFramebufferTextureRestoreFault{ false };
			if (!s_loggedFramebufferTextureRestoreFault.exchange(
				true, std::memory_order_acq_rel))
			{
				Game::logMsg(
					"[VR][NekoPostTakeover] $FBTEXTURE restore fault");
			}
		}
	}

	if (takeoverOverrodeNekoBloom && takeoverNekoBloomVar &&
		!takeoverUseNativeFullFrameSource)
	{
		__try
		{
			void** const bloomVarVtable =
				*reinterpret_cast<void***>(takeoverNekoBloomVar);
			if (bloomVarVtable && bloomVarVtable[4])
			{
				using SetIntValueFn = void(__thiscall*)(void*, int);
				reinterpret_cast<SetIntValueFn>(bloomVarVtable[4])(
						takeoverNekoBloomVar,
						takeoverPreviousNekoBloom);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static std::atomic<bool> s_loggedNekoBloomRestoreFault{ false };
			if (!s_loggedNekoBloomRestoreFault.exchange(
				true, std::memory_order_acq_rel))
			{
				Game::logMsg(
					"[VR][NekoPostTakeover] $NEKO_BLOOM restore fault");
			}
		}
	}

	if (takeoverDevice)
	{
		const bool readAfterState =
			SUCCEEDED(takeoverDevice->GetRenderState(
				D3DRS_SRGBWRITEENABLE, &takeoverSrgbAfter)) &&
			SUCCEEDED(takeoverDevice->GetRenderState(
				D3DRS_ALPHABLENDENABLE, &takeoverBlendAfter)) &&
			SUCCEEDED(takeoverDevice->GetRenderState(
				D3DRS_SRCBLEND, &takeoverSrcBlendAfter)) &&
			SUCCEEDED(takeoverDevice->GetRenderState(
				D3DRS_DESTBLEND, &takeoverDstBlendAfter)) &&
			SUCCEEDED(takeoverDevice->GetRenderState(
				D3DRS_BLENDOP, &takeoverBlendOpAfter)) &&
			SUCCEEDED(takeoverDevice->GetSamplerState(
				0, D3DSAMP_SRGBTEXTURE, &takeoverSampler0SrgbAfter)) &&
			SUCCEEDED(takeoverDevice->GetSamplerState(
				1, D3DSAMP_SRGBTEXTURE, &takeoverSampler1SrgbAfter));
		takeoverHaveD3DState = takeoverHaveD3DState && readAfterState;
		takeoverDevice->Release();
		takeoverDevice = nullptr;
	}

	if (takeoverApplied && context && !takeoverUseBackBufferCapture)
	{
		__try
		{
			context->SetRenderTarget(takeoverPreviousRt);
			if (takeoverHavePreviousViewport && hkViewport.fOriginal)
			{
				hkViewport.fOriginal(
					context,
					takeoverPreviousVpX,
					takeoverPreviousVpY,
					takeoverPreviousVpW,
					takeoverPreviousVpH);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static std::atomic<bool> s_loggedRestoreFault{ false };
			if (!s_loggedRestoreFault.exchange(true, std::memory_order_acq_rel))
				Game::logMsg("[VR][NekoPostTakeover] RT/viewport restore fault");
		}
	}

	if (takeover)
	{
		static std::atomic<unsigned int> s_takeoverLogBudget{ 12 };
		unsigned int remaining = s_takeoverLogBudget.load(std::memory_order_acquire);
		while (remaining > 0 &&
			!s_takeoverLogBudget.compare_exchange_weak(
				remaining,
				remaining - 1,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
		}
		if (remaining > 0)
		{
			Game::logMsg(
				"[VR][NekoPostTakeover] applied=%d captured=%d deferred=%d nativeSource=%d hdrScene=%d decoded=%d decodeHr=0x%08lx source=%s smallHr=0x%08lx reboundBase=%d base=%s->%s stdBloom=%d->%d stdBloomGated=%d reboundFB0=%d fb0=%s->%s reboundFBTex=%d fbTex=%s->%s nekoBloom=%d->%d gated=%d cleared=%d actualRT=%ux%u fmt=%d getRT=0x%08lx getDesc=0x%08lx clear=0x%08lx pass=%s eye=%d target=%s restoredRT=%s viewport=%d,%d %dx%d d3d=%d srgb=%lu->%lu blend=%lu->%lu src=%lu dst=%lu op=%lu sampSrgb=%lu/%lu tid=%lu",
				takeoverApplied ? 1 : 0,
				takeoverCapturedBackBuffer ? 1 : 0,
				takeoverUseBackBufferCapture ? 1 : 0,
				takeoverUseNativeFullFrameSource ? 1 : 0,
				takeoverHdrSceneInput ? 1 : 0,
				takeoverDecodedInput ? 1 : 0,
				static_cast<unsigned long>(takeoverDecodeInputHr),
				DebugTextureName(takeoverFramebufferSource),
				static_cast<unsigned long>(takeoverRefreshSmallInputHr),
				takeoverReboundBaseTexture ? 1 : 0,
				DebugTextureName(takeoverPreviousBaseTexture),
				DebugTextureName(m_VR ? m_VR->m_NekoPostSmallInputTexture : nullptr),
				takeoverPreviousBloomEnable,
				takeoverDisabledStandardBloom
					? 0
					: takeoverPreviousBloomEnable,
				takeoverDisabledStandardBloom ? 1 : 0,
				takeoverReboundFramebufferCopy0 ? 1 : 0,
				DebugTextureName(takeoverPreviousFramebufferCopy0),
				DebugTextureName(takeoverFramebufferSource),
				takeoverReboundFramebufferTexture ? 1 : 0,
				DebugTextureName(takeoverPreviousFramebufferTexture),
				DebugTextureName(takeoverFramebufferSource),
				takeoverPreviousNekoBloom,
				takeoverOverrodeNekoBloom
					? takeoverDrawNekoBloom
					: takeoverPreviousNekoBloom,
				takeoverOverrodeNekoBloom ? 1 : 0,
				takeoverTargetCleared ? 1 : 0,
				takeoverActualTargetDesc.Width,
				takeoverActualTargetDesc.Height,
				static_cast<int>(takeoverActualTargetDesc.Format),
				static_cast<unsigned long>(takeoverGetActualTargetHr),
				static_cast<unsigned long>(takeoverActualTargetDescHr),
				static_cast<unsigned long>(takeoverClearActualTargetHr),
				HooksNekoPostPassName(g_NekoPostProbeState.pass),
				g_NekoPostProbeState.eyeIndex,
				DebugTextureName(g_NekoPostProbeState.takeoverTarget),
				DebugTextureName(takeoverPreviousRt),
				takeoverPreviousVpX,
				takeoverPreviousVpY,
				takeoverPreviousVpW,
				takeoverPreviousVpH,
				takeoverHaveD3DState ? 1 : 0,
				static_cast<unsigned long>(takeoverSrgbBefore),
				static_cast<unsigned long>(takeoverSrgbAfter),
				static_cast<unsigned long>(takeoverBlendBefore),
				static_cast<unsigned long>(takeoverBlendAfter),
				static_cast<unsigned long>(takeoverSrcBlendAfter),
				static_cast<unsigned long>(takeoverDstBlendAfter),
				static_cast<unsigned long>(takeoverBlendOpAfter),
				static_cast<unsigned long>(takeoverSampler0SrgbAfter),
				static_cast<unsigned long>(takeoverSampler1SrgbAfter),
				GetCurrentThreadId());
		}
	}

	if ((!sampled && !desktopBaseline) || !context)
		return;

	ITexture* afterRt = nullptr;
	ITexture* afterCopy0 = nullptr;
	ITexture* afterCopy1 = nullptr;
	int afterVpX = 0;
	int afterVpY = 0;
	int afterVpW = 0;
	int afterVpH = 0;
	const bool haveAfterViewport = DebugGetViewport(
		context, afterVpX, afterVpY, afterVpW, afterVpH);
	__try
	{
		afterRt = context->GetRenderTarget();
		void** const vtable = *reinterpret_cast<void***>(context);
		if (vtable && vtable[19])
		{
			using GetFramebufferCopyFn = ITexture* (__thiscall*)(void*, int);
			GetFramebufferCopyFn const getCopy =
				reinterpret_cast<GetFramebufferCopyFn>(vtable[19]);
			afterCopy0 = getCopy(context, 0);
			afterCopy1 = getCopy(context, 1);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		afterRt = nullptr;
		afterCopy0 = nullptr;
		afterCopy1 = nullptr;
	}

	int beforeMapW = 0;
	int beforeMapH = 0;
	int beforeActualW = 0;
	int beforeActualH = 0;
	DebugTextureFullSize(
		beforeRt, beforeMapW, beforeMapH, beforeActualW, beforeActualH);
	int afterMapW = 0;
	int afterMapH = 0;
	int afterActualW = 0;
	int afterActualH = 0;
	DebugTextureFullSize(
		afterRt, afterMapW, afterMapH, afterActualW, afterActualH);

	Game::logMsg(
		"[VR][NekoPostProbe][Call] frame=%llu pass=%s eye=%d call=%u tid=%lu ctx=%p mat=%s shader=%s dst=%d,%d %dx%d src=(%.1f,%.1f)-(%.1f,%.1f) tex=%dx%d dice=%dx%d beforeRT=%s(map=%dx%d actual=%dx%d) beforeVP=%d,%d %dx%d haveVP=%d beforeFB0=%s beforeFB1=%s afterRT=%s(map=%dx%d actual=%dx%d) afterVP=%d,%d %dx%d haveVP=%d afterFB0=%s afterFB1=%s",
		static_cast<unsigned long long>(g_NekoPostProbeState.frameSerial),
		HooksNekoPostPassName(g_NekoPostProbeState.pass),
		g_NekoPostProbeState.eyeIndex,
		callIndex,
		GetCurrentThreadId(),
		context,
		materialName,
		shaderName,
		destX, destY, width, height,
		srcX0, srcY0, srcX1, srcY1,
		srcWidth, srcHeight,
		xDice, yDice,
		DebugTextureName(beforeRt), beforeMapW, beforeMapH, beforeActualW, beforeActualH,
		beforeVpX, beforeVpY, beforeVpW, beforeVpH, haveBeforeViewport ? 1 : 0,
		DebugTextureName(beforeCopy0), DebugTextureName(beforeCopy1),
		DebugTextureName(afterRt), afterMapW, afterMapH, afterActualW, afterActualH,
		afterVpX, afterVpY, afterVpW, afterVpH, haveAfterViewport ? 1 : 0,
		DebugTextureName(afterCopy0), DebugTextureName(afterCopy1));
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
		struct NestedFirstPersonBodyEyeStateScope
		{
			LONG previousOverride = 0;
			HooksFirstPersonBodyEyeSceneState* previousState = nullptr;

			NestedFirstPersonBodyEyeStateScope()
			{
				previousOverride = InterlockedExchange(
					&g_FirstPersonBodyEyeSceneActive, 0);
				previousState = g_FirstPersonBodyPublishedState.exchange(
					nullptr, std::memory_order_acq_rel);
			}

			~NestedFirstPersonBodyEyeStateScope()
			{
				g_FirstPersonBodyPublishedState.store(
					previousState, std::memory_order_release);
				InterlockedExchange(
					&g_FirstPersonBodyEyeSceneActive, previousOverride);
			}
		} nestedFirstPersonBodyEyeStateScope;

		if (m_VR && m_VR->m_RenderPipelineDebugLog && m_Game && m_Game->m_MaterialSystem)
		{
			static std::atomic<int> s_nestedRenderViewProbeBudget{ 160 };
			const int probeIndex = s_nestedRenderViewProbeBudget.fetch_sub(1, std::memory_order_acq_rel);
			if (probeIndex > 0)
			{
				CRefPtr<IMatRenderContext> probeContextRef;
				probeContextRef = m_Game->m_MaterialSystem->GetRenderContext();
				IMatRenderContext* const probeContext = probeContextRef;
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
			CRefPtr<IMatRenderContext> nestedContextRef;
			nestedContextRef = m_Game->m_MaterialSystem->GetRenderContext();
			IMatRenderContext* const nestedContext = nestedContextRef;
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

	// RenderView can outlive CreateMove during disconnect/loading. Clear the
	// process-wide registration immediately when there is no live VR game
	// lifecycle, including before any missing-RT/context pass-through return.
	const bool firstPersonBodyLifecycleActive =
		m_VR && m_VR->m_IsVREnabled && m_VR->m_FirstPersonBodyEnabled &&
		m_Game && m_Game->m_EngineClient &&
		m_Game->m_EngineClient->IsInGame();
	if (!firstPersonBodyLifecycleActive)
	{
		g_FirstPersonBodyActualFirstPerson.store(false, std::memory_order_release);
		HooksFirstPersonBodyClearLocalRenderable();
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

	CRefPtr<IMatRenderContext> rndrContextRef;
	rndrContextRef = m_Game->m_MaterialSystem->GetRenderContext();
	IMatRenderContext* const rndrContext = rndrContextRef;
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
	const bool l4nNekoPostEnabled =
		m_VR->m_L4NNekoEnginePostLaunchEnabled;
	const bool l4nNekoTakeoverEnabled =
		l4nNekoPostEnabled && m_VR->m_NekoEnginePostVRTakeover;
	const std::uint64_t nekoPostProbeFrameSerial = l4nNekoPostEnabled
		? HooksNekoPostProbeSampleFrame(m_VR)
		: 0;
	if (nekoPostProbeFrameSerial != 0)
	{
		Game::logMsg(
			"[VR][NekoPostProbe][FrameBegin] frame=%llu q=%d producerTid=%lu left=%s right=%s",
			static_cast<unsigned long long>(nekoPostProbeFrameSerial),
			queueMode,
			GetCurrentThreadId(),
			DebugTextureName(m_VR->m_LeftEyeTexture),
			DebugTextureName(m_VR->m_RightEyeTexture));
	}
	// Normal queued rendering uses Source call-queue markers to own the device for
	// complete render-command intervals. The activity gate remains a fallback for
	// calls outside a proven interval.
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
	struct SourceRenderExecutionScope
	{
		VR* vr = nullptr;
		IMatRenderContext* context = nullptr;
		bool trackQueueTail = false;
		bool headAcquireAttempted = false;
		bool sourceFrameAcquireQueued = false;
		bool tailReleaseQueued = false;

		SourceRenderExecutionScope(
			VR* owner,
			IMatRenderContext* renderContext,
			bool queued)
			: vr(owner),
			context(renderContext),
			trackQueueTail(queued && owner && renderContext)
		{
		}

		void QueueHeadAcquire()
		{
			if (headAcquireAttempted || !trackQueueTail)
				return;

			headAcquireAttempted = true;
			sourceFrameAcquireQueued = vr->QueueSourceRenderOwnershipAcquireMarker(context);
		}

		bool QueueTailRelease()
		{
			if (!trackQueueTail || tailReleaseQueued)
				return true;

			if (!vr->QueueSourceRenderOwnershipReleaseMarker(context, sourceFrameAcquireQueued))
				return false;

			tailReleaseQueued = true;
			return true;
		}

		void HandOffToCompletionMarker()
		{
			tailReleaseQueued = true;
		}

		bool ReleasesSourceFrameOwnership() const
		{
			return sourceFrameAcquireQueued;
		}

		~SourceRenderExecutionScope()
		{
			if (trackQueueTail && !tailReleaseQueued && !QueueTailRelease())
			{
				vr->m_SourceRenderQueueOwnershipUncertain.store(true, std::memory_order_release);
				vr->m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
			}
		}
	};
	// Enter the producer window before any queued pass-through branch. Even a
	// non-drawable-window RenderView can append commands that outlive this call.
	SourceRenderQueueBuildScope sourceRenderQueueBuildScope(m_VR, queueMode != 0);
	SourceRenderExecutionScope sourceRenderExecutionScope(
		m_VR,
		rndrContext,
		queueMode != 0);
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
		sourceRenderExecutionScope.QueueHeadAcquire();
		callOriginalRenderView(setup, hudViewSetup, nClearFlags, whatToDraw);
		renderContextStateGuard.Restore();
		if (!sourceRenderExecutionScope.QueueTailRelease())
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

		sourceRenderExecutionScope.QueueHeadAcquire();
		callOriginalRenderView(setup, hudViewSetup, nClearFlags, whatToDraw);
		if (queueMode != 0)
		{
			// Top-level water/offscreen passes can return while their material commands
			// are still executing. Restore the caller's RT/viewport before appending the
			// tail marker, then hand ownership back only when that marker runs.
			renderContextStateGuard.Restore();
			if (!sourceRenderExecutionScope.QueueTailRelease())
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
	Vector queuedThirdPersonBodyPhaseCorrection{ 0,0,0 };
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

	// FirstPersonBody never accepts a cached or torn player snapshot. A failed
	// seqlock read may still use the camera cache for general rendering, but the
	// body remains disabled for this scene.
	bool firstPersonBodyQueuedPlayerSnapshotValid = false;
	bool firstPersonBodyQueuedPlayerEligible = false;

	if (queueMode != 0 && m_VR && m_VR->m_System && vr::VRCompositor())
	{
		// Remember which thread is producing render snapshots (used by other render-time hooks).
		m_VR->m_RenderThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);

		// Apply the requested priority once for each worker thread. If Source replaces
		// its queued render worker, the new thread gets its own thread_local state.
		static thread_local bool s_renderThreadPriorityBoosted = false;
		if (!s_renderThreadPriorityBoosted && m_VR->m_QueuedRenderThreadPriorityBoost > 0)
		{
			const int desiredPriority = (m_VR->m_QueuedRenderThreadPriorityBoost >= 2)
				? THREAD_PRIORITY_HIGHEST
				: THREAD_PRIORITY_ABOVE_NORMAL;
			s_renderThreadPriorityBoosted =
				SetThreadPriority(GetCurrentThread(), desiredPriority) != 0;

			if (m_VR->m_RenderPipelineDebugLog)
			{
				Game::logMsg(
					"[VR][Queued][RenderThreadPriority] tid=%lu requested=%d boosted=%d",
					GetCurrentThreadId(),
					desiredPriority,
					s_renderThreadPriorityBoosted ? 1 : 0);
			}
		}

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
			Vector cameraAnchorReference{};
			Vector bodyVelocity{};
			bool cameraAnchorPhaseAlignEligible = false;
			float rotationOffset = 0.0f;
			float vrScale = 1.0f;
			float ipdScale = 1.0f;
			float eyeZ = 0.0f;
			float ipd = 0.065f;
			Vector hmdPosLocalPrev{};
			Vector hmdPosCorrectedPrev{};
			Vector viewmodelPosOffset{};
			QAngle viewmodelAngOffset{};
			float weaponAimPitchOffsetDeg = -45.0f;

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
			vp.cameraAnchorReference.x = m_VR->m_RenderCameraAnchorReferenceX.load(std::memory_order_relaxed);
			vp.cameraAnchorReference.y = m_VR->m_RenderCameraAnchorReferenceY.load(std::memory_order_relaxed);
			vp.bodyVelocity.x = m_VR->m_RenderBodyVelocityX.load(std::memory_order_relaxed);
			vp.bodyVelocity.y = m_VR->m_RenderBodyVelocityY.load(std::memory_order_relaxed);
			vp.cameraAnchorPhaseAlignEligible =
				(m_VR->m_RenderCameraAnchorPhaseAlignEligible.load(std::memory_order_relaxed) != 0);
			vp.rotationOffset = m_VR->m_RenderRotationOffset.load(std::memory_order_acquire);
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
			vp.weaponAimPitchOffsetDeg =
				m_VR->m_RenderWeaponAimPitchOffsetDeg.load(std::memory_order_relaxed);
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
				firstPersonBodyQueuedPlayerSnapshotValid = true;
				const bool firstPersonBodyQueuedLifeStateValid =
					vp.tpLifeState == 0 ||
					(vp.tpIncap && vp.tpLifeState == 1);
				firstPersonBodyQueuedPlayerEligible =
					vp.hasLocalPlayer &&
					!vp.hasViewEntityOverride &&
					!vp.tpDead &&
					firstPersonBodyQueuedLifeStateValid &&
					vp.tpObserverMode == 0 &&
					!vp.tpObserver &&
					!vp.tpWantsThirdPerson &&
					!vp.inThirdPersonMapLoadCooldown;
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
				vp.cameraAnchorReference.x = m_VR->m_RenderCameraAnchorReferenceX.load(std::memory_order_relaxed);
				vp.cameraAnchorReference.y = m_VR->m_RenderCameraAnchorReferenceY.load(std::memory_order_relaxed);
				vp.bodyVelocity.x = m_VR->m_RenderBodyVelocityX.load(std::memory_order_relaxed);
				vp.bodyVelocity.y = m_VR->m_RenderBodyVelocityY.load(std::memory_order_relaxed);
				vp.cameraAnchorPhaseAlignEligible =
					(m_VR->m_RenderCameraAnchorPhaseAlignEligible.load(std::memory_order_relaxed) != 0);
				vp.rotationOffset = m_VR->m_RenderRotationOffset.load(std::memory_order_acquire);
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
				vp.weaponAimPitchOffsetDeg =
					m_VR->m_RenderWeaponAimPitchOffsetDeg.load(std::memory_order_relaxed);
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

		// Pair the update-thread body anchor with this Source scene frame.
		//
		// Queued rendering lets Present/VR::Update publish a newer (or older) body anchor
		// than the CViewSetup currently being rendered. The old path mixed those time
		// domains and then hid the resulting hold/jump motion with a low-pass filter.
		// That necessarily added roughly QueuedRenderViewSmoothMs of locomotion lag.
		//
		// In normal first person cameraAnchor and cameraAnchorReference translate together.
		// CViewSetup::origin also contains flat-screen camera impulses (collision/view
		// shake/ground bumps), so applying its full delta makes VR hypersensitive. Keep
		// only the component along the player's physical velocity and bound it to a
		// plausible queued-frame travel distance. This remains an instantaneous phase
		// correction: the full world anchor is never low-pass filtered.
		Vector queuedAnchorTarget = vp.cameraAnchor;
		Vector queuedBodyPhaseDelta{};
		Vector queuedBodyPhaseCorrection{};
		bool queuedBodyPhaseAligned = false;
		auto IsFiniteXY = [](const Vector& v)
			{
				return std::isfinite(v.x) && std::isfinite(v.y);
			};
		if (vpOk &&
			vp.cameraAnchorPhaseAlignEligible &&
			IsFiniteXY(vp.cameraAnchor) &&
			IsFiniteXY(vp.cameraAnchorReference) &&
			IsFiniteXY(setup.origin))
		{
			queuedBodyPhaseDelta.x = setup.origin.x - vp.cameraAnchorReference.x;
			queuedBodyPhaseDelta.y = setup.origin.y - vp.cameraAnchorReference.y;

			// The existing first/third-person detector treats 20 Source units of
			// planar separation as a shoulder camera. Reject instead of clamping so
			// teleports or a missed state transition can never become a slow drift.
			constexpr float kMaxQueuedBodyPhaseDelta = 20.0f;
			const float phaseDeltaSq =
				queuedBodyPhaseDelta.x * queuedBodyPhaseDelta.x +
				queuedBodyPhaseDelta.y * queuedBodyPhaseDelta.y;
			if (phaseDeltaSq < (kMaxQueuedBodyPhaseDelta * kMaxQueuedBodyPhaseDelta))
			{
				queuedBodyPhaseAligned = true;

				const float bodySpeedSq =
					vp.bodyVelocity.x * vp.bodyVelocity.x +
					vp.bodyVelocity.y * vp.bodyVelocity.y;
				if (IsFiniteXY(vp.bodyVelocity) && std::isfinite(bodySpeedSq) &&
					bodySpeedSq > (0.5f * 0.5f))
				{
					const float bodySpeed = std::sqrt(bodySpeedSq);
					const Vector bodyDirection(
						vp.bodyVelocity.x / bodySpeed,
						vp.bodyVelocity.y / bodySpeed,
						0.0f);

					float alongTravel =
						queuedBodyPhaseDelta.x * bodyDirection.x +
						queuedBodyPhaseDelta.y * bodyDirection.y;

					// mat_queue_mode is intentionally kept near one frame ahead. A
					// 45 ms physical-travel window covers low-tick scenes while
					// rejecting camera impulses that cannot be body locomotion.
					const float maxAlongTravel =
						std::clamp(0.75f + bodySpeed * 0.045f, 0.75f, 12.0f);
					alongTravel = std::clamp(
						alongTravel,
						-maxAlongTravel,
						maxAlongTravel);

					queuedBodyPhaseCorrection.x = bodyDirection.x * alongTravel;
					queuedBodyPhaseCorrection.y = bodyDirection.y * alongTravel;
					queuedAnchorTarget.x += queuedBodyPhaseCorrection.x;
					queuedAnchorTarget.y += queuedBodyPhaseCorrection.y;
				}
			}
		}

		// Keep the old filter only as a fallback for states where exact frame pairing
		// is deliberately unavailable (third person, observer, roomscale-decoupled,
		// scout/scripted cameras, transitions). Normal first-person locomotion snaps
		// to the paired target even if the legacy smoothing setting is non-zero.
		static thread_local bool s_viewSmoothValid = false;
		static thread_local Vector s_viewSmoothAnchor{};
		static thread_local float s_viewSmoothRot = 0.0f;
		static thread_local std::chrono::steady_clock::time_point s_viewSmoothLastT{};
		static thread_local bool s_bodyPhaseAlignedPrev = false;
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
			const bool phaseAlignTransition =
				queuedBodyPhaseAligned != s_bodyPhaseAlignedPrev;
			if (!s_viewSmoothValid || phaseAlignTransition || queuedBodyPhaseAligned)
			{
				s_viewSmoothAnchor = queuedAnchorTarget;
				s_viewSmoothRot = Wrap360(vp.rotationOffset);
				s_viewSmoothValid = true;
				didSnapSmooth = true;
			}
			else
			{
				const Vector errA = queuedAnchorTarget - s_viewSmoothAnchor;
				const float errYaw = AngleDeltaDeg(vp.rotationOffset, s_viewSmoothRot);

				// Large discontinuities -> snap immediately.
				if (errA.LengthSqr() > (256.0f * 256.0f) || std::fabs(errYaw) > 120.0f)
				{
					s_viewSmoothAnchor = queuedAnchorTarget;
					s_viewSmoothRot = Wrap360(vp.rotationOffset);
					didSnapSmooth = true;
				}
				else if (smoothMsCfg <= 0)
				{
					// Smoothing disabled -> follow exactly.
					s_viewSmoothAnchor = queuedAnchorTarget;
					s_viewSmoothRot = Wrap360(vp.rotationOffset);
				}
				else
				{
					// If we're basically synced, snap to avoid tiny residual lag.
					const bool smallErr = (errA.LengthSqr() < (0.05f * 0.05f)) && (std::fabs(errYaw) < 0.05f);
					if (smallErr)
					{
						s_viewSmoothAnchor = queuedAnchorTarget;
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
			s_bodyPhaseAlignedPrev = queuedBodyPhaseAligned;
		}

		const Vector extrapAnchor = s_viewSmoothValid ? s_viewSmoothAnchor : queuedAnchorTarget;
		float extrapRot = s_viewSmoothValid ? s_viewSmoothRot : vp.rotationOffset;

		// Keep a queued third-person camera phase-locked to the Source scene frame.
		// The world model is rendered from setup.origin, while the latest HMD pose is
		// rebuilt from extrapAnchor. Without this correction, ordinary locomotion can
		// put the camera and local player model on adjacent queued frames, making the
		// model visibly shake. Only body-velocity-aligned planar travel is accepted;
		// tracked HMD translation remains entirely in the render-pose snapshot.
		static thread_local QueuedThirdPersonBodyPhaseAligner s_tpBodyPhaseAligner;
		queuedThirdPersonBodyPhaseCorrection = s_tpBodyPhaseAligner.Update(
			smoothedSetupOrigin,
			extrapAnchor,
			vp.bodyVelocity,
			vpOk && vp.hasLocalPlayer);

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
				m_VR->m_System;
			bool havePoses = false;
			if (useTrackingRenderPose)
			{
				const vr::ETrackingUniverseOrigin trackingOrigin = m_VR->GetCachedTrackingUniverseOrigin();
				float predicted = m_VR->SampleQueuedTrackingPredictionSeconds();
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
						const DWORD wr = TimedWaitForPoseEvent(m_VR->m_PoseWaiterEvent, remaining);
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
				if (TimedWaitForPoseEvent(m_VR->m_PoseWaiterEvent, firstWait) == WAIT_OBJECT_0)
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
					if (TimedWaitForPoseEvent(m_VR->m_PoseWaiterEvent, firstWait) == WAIT_OBJECT_0)
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
							const DWORD wr = TimedWaitForPoseEvent(m_VR->m_PoseWaiterEvent, remaining);
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

			// ProcessInput runs after UpdateTracking publishes the full ViewParams
			// snapshot. Rotation is independently refreshed after ProcessInput, so
			// late-latch that scalar after any pose wait. It is safe to apply the
			// latest body yaw in the same normal first-person path where translation
			// is frame-paired; no low-pass delay is introduced.
			if (queuedBodyPhaseAligned)
			{
				const float latestRotationOffset =
					m_VR->m_RenderRotationOffset.load(std::memory_order_acquire);
				if (std::isfinite(latestRotationOffset))
				{
					vp.rotationOffset = latestRotationOffset;
					extrapRot = Wrap360(latestRotationOffset);
					s_viewSmoothRot = extrapRot;
				}
			}

			// Periodic diagnostics (piggyback on QueuedViewmodelStabilizeDebugLog).
			if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
			{
				static thread_local std::chrono::steady_clock::time_point s_lastStatusLog{};
				if (!ShouldThrottleLog(s_lastStatusLog, 1.0f))
				{
					const Vector smoothErrA = queuedAnchorTarget - extrapAnchor;
					const float smoothErrYaw = AngleDeltaDeg(vp.rotationOffset, extrapRot);
					Game::logMsg("[VR][Queued][RenderView] status q=%d vpSeq=%u poseSeq=%u havePoses=%d waitCfg=%d waitEff=%d phase=%d phaseD=(%.3f %.3f) phaseC=(%.3f %.3f) bodyV=(%.2f %.2f) snap=%d smoothMs=%d alpha=%.3f errD=%.3f errYaw=%.3f pendD=%.4f pendYaw=%.3f vpRot=%.2f smRot=%.2f tick=%.1fms",
						queueMode, (unsigned)vpSeqEven, (unsigned)poseSeq, havePoses ? 1 : 0,
						waitMsCfg, waitMs,
						queuedBodyPhaseAligned ? 1 : 0,
						queuedBodyPhaseDelta.x, queuedBodyPhaseDelta.y,
						queuedBodyPhaseCorrection.x, queuedBodyPhaseCorrection.y,
						vp.bodyVelocity.x, vp.bodyVelocity.y,
						didSnapSmooth ? 1 : 0, smoothMsCfg, alpha,
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
					float predicted = m_VR->SampleQueuedTrackingPredictionSeconds();
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

						// Match the configurable weapon-hand calibration used by
						// the main tracking/gameplay path.
						ctrlF = VectorRotate(ctrlF, ctrlR, vp.weaponAimPitchOffsetDeg);
						ctrlU = VectorRotate(ctrlU, ctrlR, vp.weaponAimPitchOffsetDeg);

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

	if (!localPlayerValid)
	{
		g_FirstPersonBodyActualFirstPerson.store(false, std::memory_order_release);
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

	const bool materialPreviewActive =
		m_VR->m_MaterialPreviewActive.load(std::memory_order_acquire) &&
		!stateIsDeadOrObserver && !hasViewEntityOverride;
	const bool defaultThirdPersonNow = m_VR->m_ThirdPersonDefault && !stateIsDeadOrObserver && !hasViewEntityOverride;
	bool renderThirdPerson = materialPreviewActive || defaultThirdPersonNow || customWalkThirdPersonNow || engineThirdPersonNow || tpStateDbg.selfMedkit || (m_VR->m_ThirdPersonHoldFrames > 0);
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
	const bool wasRenderingThirdPerson =
		m_VR->m_IsThirdPersonCamera;
	m_VR->m_IsThirdPersonCamera = renderThirdPerson;
	if (!renderThirdPerson)
	{
		m_VR->m_WorldModelVRPoseLocalThirdPersonWarmupUntilTickMs.store(
			0u,
			std::memory_order_release);
	}
	else if (!wasRenderingThirdPerson)
	{
		// A saved calibration previously let WorldPose enter on the first
		// queued survivor draw, while Source was still replacing its 1P bone
		// submission with the 3P one. An in-session five-second calibration
		// hid this lifecycle bug by supplying an accidental delay.
		constexpr std::uint64_t kWorldPoseThirdPersonWarmupMs = 750u;
		m_VR->m_WorldModelVRPoseLocalThirdPersonWarmupUntilTickMs.store(
			static_cast<std::uint64_t>(GetTickCount64()) +
				kWorldPoseThirdPersonWarmupMs,
			std::memory_order_release);
	}
	// This is the only authoritative camera decision used by FirstPersonBody.
	// Death first-person locks and in-eye observer modes deliberately make
	// renderThirdPerson false, so life/observer/view-entity checks must remain
	// independent. Fail closed on every detached or transitional camera.
	const bool firstPersonBodyLifeStateValid =
		tpStateDbg.lifeState == 0 ||
		(tpStateDbg.incap && tpStateDbg.lifeState == 1);
	const bool firstPersonBodyPlayerEligible =
		(queueMode == 0)
		? (localPlayerValid &&
			firstPersonBodyLifeStateValid &&
			!tpStateDbg.dead &&
			tpStateDbg.observerMode == 0 &&
			!hasViewEntityOverride &&
			!inMapLoadCooldown)
		: (firstPersonBodyQueuedPlayerSnapshotValid &&
			firstPersonBodyQueuedPlayerEligible);
	const bool firstPersonBodyActualFirstPerson =
		firstPersonBodyPlayerEligible &&
		!m_VR->m_TeleportVisualScoutActive.load(std::memory_order_acquire) &&
		!renderThirdPerson;
	g_FirstPersonBodyActualFirstPerson.store(
		firstPersonBodyActualFirstPerson,
		std::memory_order_release);

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
			// Preserve the original VR projection verbatim at ordinary distances.
			view.zNear = 6;
			// Entity proximity is sampled on the update thread. RenderView may run on
			// Source's queued render thread, so it consumes only published atomics.
			if (m_VR->m_RenderVRNearClipCharacterContactActive.load(std::memory_order_acquire) != 0)
			{
				view.zNear = std::clamp(
					m_VR->m_VRNearClipEffective.load(std::memory_order_relaxed),
					0.1f,
					6.0f);
			}
			// Viewmodels share the scene projection so their depth values remain
			// directly comparable with the world depth buffer. Their near-plane
			// clipping is suppressed per draw when VRNearClipSelfBody is enabled.
			view.zNearViewmodel = view.zNear;
			// Native Source viewmodels use a separate projection plus a compressed
			// 0..0.1 depth range so they always draw over nearby world geometry.
			// VR viewmodels are positioned in world space, so keep their Z projection
			// identical to the eye scene; DrawModelExecute restores the full depth
			// range and the existing eye depth buffer can then occlude them correctly.
			view.zFarViewmodel = view.zFar;
		};

	NormalizeViewSetupForVREye(leftEyeView);
	NormalizeViewSetupForVREye(rightEyeView);
	// Keep VR tracking base tied to the real player eye, NOT the shoulder camera.
	// IMPORTANT (VR compatibility):
	// Some VScript mods (e.g. slide mods) temporarily enable point_viewcontrol_survivor via
	// CBasePlayer::m_hViewEntity. In that case, setup.origin can jump to an attachment-driven
	// camera that does NOT match the HMD eye origin and can appear "too high" in VR.
	// So: only borrow setup.origin.z when the player has no active view-entity override.
	// Keep the render-frame setup origin local in queued mode. VR::UpdateTracking
	// owns m_SetupOrigin/m_SetupOriginPrev and uses their delta to advance
	// m_CameraAnchor; writing that plain Vector from the queued render thread can
	// make the update thread miss or double-apply a locomotion step.
	Vector renderSetupOrigin = eyeOrigin;
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
			renderSetupOrigin.z = s_stepZSmooth;
		}
		else
		{
			s_stepZSmoothValid = false;
			s_stepZSmoothLastT = {};
		}
	}
	const QAngle renderSetupAngles(setup.angles.x, setup.angles.y, setup.angles.z);
	if (queueMode == 0)
	{
		m_VR->m_SetupOrigin = renderSetupOrigin;
		m_VR->m_SetupAngles = renderSetupAngles;
	}
	else
	{
		// Publish render-camera state instead of writing update-thread-owned
		// Vector/QAngle objects from the queued render thread.
		uint32_t setupSeq = m_VR->m_RenderSetupCameraSeq.load(std::memory_order_relaxed);
		m_VR->m_RenderSetupCameraSeq.store(setupSeq + 1u, std::memory_order_release);
		m_VR->m_RenderSetupOriginX.store(renderSetupOrigin.x, std::memory_order_relaxed);
		m_VR->m_RenderSetupOriginY.store(renderSetupOrigin.y, std::memory_order_relaxed);
		m_VR->m_RenderSetupOriginZ.store(renderSetupOrigin.z, std::memory_order_relaxed);
		m_VR->m_RenderSetupAnglesX.store(renderSetupAngles.x, std::memory_order_relaxed);
		m_VR->m_RenderSetupAnglesY.store(renderSetupAngles.y, std::memory_order_relaxed);
		m_VR->m_RenderSetupAnglesZ.store(renderSetupAngles.z, std::memory_order_relaxed);
		m_VR->m_RenderSetupCameraSeq.store(setupSeq + 2u, std::memory_order_release);
	}

	Vector leftOrigin, rightOrigin;
	Vector viewAngles = m_VR->GetViewAngle();
	Vector renderViewAngles = viewAngles;
	const bool thirdPersonFrontViewActive = renderThirdPerson
		&& (materialPreviewActive || m_VR->m_ThirdPersonFrontViewEnabled)
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
		const float bodyYaw = (queueMode == 0)
			? m_VR->m_RotationOffset
			: m_VR->m_RenderRotationOffset.load(std::memory_order_acquire);
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
				if (queueMode == 0)
				{
					m_VR->ResetPosition();
				}
				else
				{
					// ResetPosition mutates the update-thread camera anchor. Queue
					// an atomic request instead of doing that from the render thread.
					m_VR->m_ResetPositionDeferredPending.store(1u, std::memory_order_release);
				}
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
		//     the player. When decoupled from HMD, head turning no longer drags the whole camera
		//     position/orbit, while roomscale translation still moves it.
		QAngle renderCamAng(viewAngles.x, viewAngles.y, viewAngles.z);
		if (m_VR->m_HmdForward.IsZero())
			renderCamAng = engineCamAngles;

		QAngle cameraBasisAng = renderCamAng;
		if (!m_VR->m_ThirdPersonCameraFollowHmd || m_VR->m_HmdForward.IsZero())
			cameraBasisAng = engineCamAngles;
		static thread_local bool s_materialPreviewPlacementInitialized = false;
		static thread_local float s_materialPreviewPlacementYaw = 0.0f;
		static thread_local float s_materialPreviewTurnOffsetPrev = 0.0f;
		if (materialPreviewActive)
		{
			const float materialPreviewTurnOffset = wrapYawDeg((queueMode == 0)
				? m_VR->m_RotationOffset
				: m_VR->m_RenderRotationOffset.load(std::memory_order_acquire));
			if (!s_materialPreviewPlacementInitialized)
			{
				s_materialPreviewPlacementYaw = wrapYawDeg(viewAngles.y);
				s_materialPreviewTurnOffsetPrev = materialPreviewTurnOffset;
				s_materialPreviewPlacementInitialized = true;
			}
			else
			{
				const float turnDelta = wrapYawDeg(
					materialPreviewTurnOffset - s_materialPreviewTurnOffsetPrev);
				s_materialPreviewPlacementYaw = wrapYawDeg(
					s_materialPreviewPlacementYaw + turnDelta);
				s_materialPreviewTurnOffsetPrev = materialPreviewTurnOffset;
			}
			cameraBasisAng.Init(0.0f, s_materialPreviewPlacementYaw, 0.0f);
		}
		else
		{
			s_materialPreviewPlacementInitialized = false;
		}

		if (thirdPersonFrontViewActive)
		{
			// In front-view mode, keep the main third-person camera yaw aligned with the scope yaw
			// so thumbstick/scope turning also recenters the character in the main view.
			float frontYaw = 0.0f;
			if (!materialPreviewActive && m_VR->ShouldRenderScope())
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
			if (materialPreviewActive)
			{
				renderCamAng.x += m_VR->m_MaterialPreviewCameraPitch.load(
					std::memory_order_acquire);
				renderCamAng.y = wrapYawDeg(
					renderCamAng.y +
					m_VR->m_MaterialPreviewCameraYaw.load(
						std::memory_order_acquire));
				renderCamAng.z = wrapYawDeg(
					renderCamAng.z +
					m_VR->m_MaterialPreviewCameraRoll.load(
						std::memory_order_acquire));
			}
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
		bool baseCenterUsesTrackedAnchor = false;
		if (materialPreviewActive)
		{
			Vector trackedHmdForward;
			QAngle::AngleVectors(renderCamAng, &trackedHmdForward, nullptr, nullptr);
			const Vector trackedEyeCenter =
				(m_VR->GetViewOriginLeft() + m_VR->GetViewOriginRight()) * 0.5f;
			baseCenter = trackedEyeCenter + (trackedHmdForward * eyeZ);
			baseCenterUsesTrackedAnchor = true;
		}
		else if (stateWantsThirdPerson)
		{
			// Dead/observer camera must follow engine view, not HMD position.
			baseCenter = stateIsDeadOrObserver ? engineCamOrigin : m_VR->m_HmdPosAbs;
			baseCenterUsesTrackedAnchor = !stateIsDeadOrObserver;
		}
		else
		{
			baseCenter = (engineThirdPersonNow || customWalkThirdPersonNow)
				? engineCamOrigin
				: m_VR->m_HmdPosAbs;
			baseCenterUsesTrackedAnchor =
				!engineThirdPersonNow && !customWalkThirdPersonNow;
		}
		// Keep the multicore model-jitter fix from be529d59: whenever the
		// restored camera path uses a tracked render anchor, align its body/world
		// phase with Source's queued scene without changing the camera policy.
		if (queueMode != 0 && baseCenterUsesTrackedAnchor)
			baseCenter += queuedThirdPersonBodyPhaseCorrection;
		Vector camCenter = baseCenter + (basisFwd * (-eyeZ));
		if (thirdPersonFrontViewActive)
		{
			const float materialPreviewCmToWorld = m_VR->m_VRScale * 0.01f;
			const Vector configuredOffset = materialPreviewActive
				? Vector{
					m_VR->m_MaterialPreviewCameraDistance.load(std::memory_order_acquire) * materialPreviewCmToWorld,
					m_VR->m_MaterialPreviewCameraHorizontal.load(std::memory_order_acquire) * materialPreviewCmToWorld,
					m_VR->m_MaterialPreviewCameraVertical.load(std::memory_order_acquire) * materialPreviewCmToWorld }
				: m_VR->m_ThirdPersonFrontVRCameraOffset;
			camCenter = camCenter
				+ (basisFwd * (-configuredOffset.x))
				+ (basisRight * configuredOffset.y)
				+ (basisUp * configuredOffset.z);
		}
		else if (m_VR->m_ThirdPersonVRCameraOffset > 0.0f)
		{
			camCenter = camCenter +
				(basisFwd * (-m_VR->m_ThirdPersonVRCameraOffset));
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
		m_VR->m_ThirdPersonRenderCenter = renderSetupOrigin;
	}

	leftEyeView.origin = leftOrigin;
	leftEyeView.angles = renderViewAngles;

	// Queued render: draw aim line from the render-thread snapshot so it stays glued to the hand/gun.
	// IMPORTANT: must run after we compute m_SetupOrigin / m_ThirdPersonRenderCenter for this frame.
	// The legacy clean Source-eye path is intentionally single-thread only. Queued mode
	// captures its clean desktop image from the completed selected-eye submit snapshot
	// immediately before DXVK draws item labels and the D3D aim line.
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

	// The queued clean desktop-mirror RenderView path was removed deliberately.
	// Queued mode fills desktopMirrorClean0 from its completed submit snapshot instead.

	const Vector sharedCenterOrigin(
		(leftEyeView.origin.x + rightEyeView.origin.x) * 0.5f,
		(leftEyeView.origin.y + rightEyeView.origin.y) * 0.5f,
		(leftEyeView.origin.z + rightEyeView.origin.z) * 0.5f);

	auto renderEyeScene = [&](int eyeIndex, ITexture* eyeTexture,
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
			if (drawPreViewLaser && m_VR->m_IsVREnabled)
				m_VR->RenderDrawGameLaserSight(localPlayer);
			if (m_VR->m_IsVREnabled)
				m_VR->BeginVrHandsEyeRender(eyeView, eyeIndex == 1 ? 0 : 1);

			struct FirstPersonBodyEyeSceneScope
			{
				LONG previousOverride = 0;
				HooksFirstPersonBodyEyeSceneState* previousState = nullptr;
				HooksFirstPersonBodyEyeSceneState* state = nullptr;

				FirstPersonBodyEyeSceneScope(
					VR* owner,
					const CViewSetup& view,
					const Vector& centerEyePosition,
					int eyeIndex,
					bool actualFirstPerson,
					bool renderBody)
				{
					previousOverride = InterlockedExchange(
						&g_FirstPersonBodyEyeSceneActive, 0);
					previousState = g_FirstPersonBodyPublishedState.exchange(
						nullptr, std::memory_order_acq_rel);

					const bool hooksReady =
						g_FirstPersonBodyRenderableHooksReady.load(std::memory_order_acquire);
					const bool playerReady =
						g_FirstPersonBodyPlayerReady.load(std::memory_order_acquire);
					void* const localRenderable =
						g_FirstPersonBodyLocalRenderable.load(std::memory_order_acquire);
					const bool enable =
						owner &&
						localRenderable &&
						playerReady &&
						actualFirstPerson &&
						g_FirstPersonBodyActualFirstPerson.load(std::memory_order_acquire) &&
						owner->m_IsVREnabled &&
						owner->m_FirstPersonBodyEnabled &&
						hooksReady;
					if (!enable)
						return;

					state = &g_FirstPersonBodyProducerStates[eyeIndex == 2 ? 1 : 0];
					*state = HooksFirstPersonBodyEyeSceneState{};
					state->view = view;
					state->centerEyePosition = centerEyePosition;
					state->eyeIndex = eyeIndex;
					state->sceneSerial = g_FirstPersonBodySceneSerial.fetch_add(
						1, std::memory_order_acq_rel);
					// Keep the scene/bone pipeline active while incapacitated. Only the
					// final body color draw is disabled; shoulder anchors are still built
					// and published for the separate first-person arm IK.
					state->bodyActive = true;
					state->renderBody = renderBody;
					state->playerGeneration =
						g_FirstPersonBodyPlayerGeneration.load(std::memory_order_acquire);
					state->localPlayerIndex =
						g_FirstPersonBodyLocalPlayerIndex.load(std::memory_order_acquire);
					state->localPlayerRenderable = localRenderable;
					state->activeWeaponRenderable =
						g_FirstPersonBodyActiveWeaponRenderable.load(std::memory_order_acquire);

					g_FirstPersonBodyPublishedState.store(
						state,
						std::memory_order_release);
					InterlockedExchange(&g_FirstPersonBodyEyeSceneActive, 1);

				}

				~FirstPersonBodyEyeSceneScope()
				{
					InterlockedExchange(&g_FirstPersonBodyEyeSceneActive, 0);
					g_FirstPersonBodyPublishedState.store(
						previousState, std::memory_order_release);
					InterlockedExchange(
						&g_FirstPersonBodyEyeSceneActive, previousOverride);
				}
			};

			{
				ScopedNekoPostProbePass nekoPostProbePass(
					rndrContext,
					queueMode,
					nekoPostProbeFrameSerial,
					HooksNekoPostPass::MainEye,
					eyeIndex,
					eyeTexture,
					l4nNekoTakeoverEnabled);
				FirstPersonBodyEyeSceneScope firstPersonBodyScope(
					m_VR,
					eyeView,
					sharedCenterOrigin,
					eyeIndex,
					firstPersonBodyActualFirstPerson,
					!playerIncap);
				callOriginalRenderView(eyeView, eyeHud, nClearFlags, whatToDraw);
			}

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

	// Do not hold the device while this hook performs pose/camera CPU preparation.
	// Acquire immediately before the first queued eye command and retain ownership
	// through both eyes, scope/rear-mirror work and the completion tail marker.
	sourceRenderExecutionScope.QueueHeadAcquire();
	const auto stereoSceneStartTime = std::chrono::steady_clock::now();
	// m_RenderFrameSeq is a view-snapshot sequence and can remain unchanged
	// while physical VR tracking and stereo rendering continue. Start a fresh
	// weapon-bone cache lifetime for every actual stereo scene so old hand poses
	// can never be replayed across rendered frames.
	HooksWorldPoseBeginStereoFrame();
	{
		renderEyeScene(1, m_VR->m_LeftEyeTexture, leftEyeView, hudLeft, true);
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
			renderEyeScene(2, m_VR->m_RightEyeTexture, rightEyeView, hudRight, false);
		}
		if (desktopMirrorHidePluginOverlaysSingleCopyActive && m_VR->m_DesktopMirrorEye != 0)
			m_VR->CopyEyeToDesktopMirrorTexture(1);
		drawPostEyeWork(1, m_VR->m_RightEyeTexture, rightEyeView);
	}
	timingStereoSceneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stereoSceneStartTime).count();

	auto renderToTexture_SetRT = [&](ITexture* target, int texW, int texH, QAngle passAngles,
		CViewSetup& view, CViewSetup& hud, HooksNekoPostPass nekoPostPass)
		{
			// Scope/rear-mirror RTTs are allowed in queued rendering. Do not touch
			// EngineClient viewangles in queueMode!=0; CViewSetup already carries
			// the offscreen camera pose for the render thread.
			CRefPtr<IMatRenderContext> rcRef;
			rcRef = m_Game->m_MaterialSystem->GetRenderContext();
			IMatRenderContext* const rc = rcRef;
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

			{
				ScopedNekoPostProbePass nekoPostProbeScope(
					rc,
					queueMode,
					nekoPostProbeFrameSerial,
					nekoPostPass,
					0,
					target,
					l4nNekoTakeoverEnabled);
				callOriginalRenderView(view, hud, nClearFlags, whatToDraw);
			}

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
		const bool thirdPersonFrontViewScope =
			m_VR->m_ThirdPersonFrontViewEnabled && m_VR->m_IsThirdPersonCamera;
		scopeView.fov = thirdPersonFrontViewScope
			? m_VR->m_ThirdPersonFrontViewOverlayFov
			: m_VR->m_ScopeFov;
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
			scopeAngles, scopeView, hudScope, HooksNekoPostPass::Scope);
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
			mirrorAngles, mirrorView, hudMirror, HooksNekoPostPass::RearMirror);

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
			sourceRenderExecutionScope.ReleasesSourceFrameOwnership(),
			renderHmdPoseForSubmitValid ? &renderHmdPoseForSubmit : nullptr))
		{
			sourceRenderExecutionScope.HandOffToCompletionMarker();
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
