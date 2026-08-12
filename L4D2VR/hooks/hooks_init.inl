namespace
{
	bool HooksAddressIsExecutable(void* address)
	{
		if (!address)
			return false;

		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (!VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) ||
			memoryInfo.State != MEM_COMMIT ||
			(memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			return false;
		}

		const DWORD protection = memoryInfo.Protect & 0xff;
		return protection == PAGE_EXECUTE ||
			protection == PAGE_EXECUTE_READ ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;
	}

	// left4neko replaces IMatRenderContext::DrawScreenSpaceRectangle in the live
	// vtable with its own pre/original/post wrapper. Resolve the pristine slot
	// from materialsystem.dll on disk so our diagnostic detour sits inside that
	// wrapper, exactly where left4neko invokes the engine implementation.
	void* HooksResolvePristineVtableTarget(
		HMODULE module,
		void** liveVtable,
		size_t slot,
		DWORD* vtableRvaOut,
		DWORD* targetRvaOut)
	{
		if (vtableRvaOut)
			*vtableRvaOut = 0;
		if (targetRvaOut)
			*targetRvaOut = 0;
		if (!module || !liveVtable)
			return nullptr;

		char modulePath[MAX_PATH]{};
		const DWORD pathLength = GetModuleFileNameA(module, modulePath, MAX_PATH);
		if (pathLength == 0 || pathLength >= MAX_PATH)
			return nullptr;

		HANDLE file = CreateFileA(
			modulePath,
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return nullptr;

		DWORD sizeHigh = 0;
		const DWORD fileSize = GetFileSize(file, &sizeHigh);
		if (fileSize == INVALID_FILE_SIZE || sizeHigh != 0 || fileSize < sizeof(IMAGE_DOS_HEADER))
		{
			CloseHandle(file);
			return nullptr;
		}

		BYTE* const image = static_cast<BYTE*>(
			HeapAlloc(GetProcessHeap(), 0, fileSize));
		if (!image)
		{
			CloseHandle(file);
			return nullptr;
		}

		DWORD bytesRead = 0;
		const bool readSucceeded =
			ReadFile(file, image, fileSize, &bytesRead, nullptr) != FALSE &&
			bytesRead == fileSize;
		CloseHandle(file);
		if (!readSucceeded)
		{
			HeapFree(GetProcessHeap(), 0, image);
			return nullptr;
		}

		void* result = nullptr;
		do
		{
			const IMAGE_DOS_HEADER* const dos =
				reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
				static_cast<DWORD>(dos->e_lfanew) > fileSize - sizeof(IMAGE_NT_HEADERS32))
			{
				break;
			}

			const IMAGE_NT_HEADERS32* const nt =
				reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
			{
				break;
			}

			const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(module);
			const uintptr_t vtableAddress = reinterpret_cast<uintptr_t>(liveVtable);
			if (vtableAddress < moduleBase ||
				vtableAddress - moduleBase >= nt->OptionalHeader.SizeOfImage)
			{
				break;
			}

			const DWORD vtableRva = static_cast<DWORD>(vtableAddress - moduleBase);
			const uint64_t entryRva64 =
				static_cast<uint64_t>(vtableRva) + slot * sizeof(DWORD);
			if (entryRva64 + sizeof(DWORD) > nt->OptionalHeader.SizeOfImage)
				break;
			const DWORD entryRva = static_cast<DWORD>(entryRva64);

			const IMAGE_SECTION_HEADER* const sections = IMAGE_FIRST_SECTION(nt);
			DWORD entryFileOffset = 0;
			bool entryMapped = false;
			for (WORD sectionIndex = 0;
				sectionIndex < nt->FileHeader.NumberOfSections;
				++sectionIndex)
			{
				const IMAGE_SECTION_HEADER& section = sections[sectionIndex];
				const DWORD sectionStart = section.VirtualAddress;
				const DWORD sectionEnd = sectionStart + section.SizeOfRawData;
				if (entryRva >= sectionStart && entryRva + sizeof(DWORD) <= sectionEnd)
				{
					entryFileOffset =
						section.PointerToRawData + (entryRva - sectionStart);
					entryMapped = entryFileOffset + sizeof(DWORD) <= fileSize;
					break;
				}
			}
			if (!entryMapped)
				break;

			const DWORD preferredTarget =
				*reinterpret_cast<const DWORD*>(image + entryFileOffset);
			if (preferredTarget < nt->OptionalHeader.ImageBase)
				break;
			const DWORD targetRva = preferredTarget - nt->OptionalHeader.ImageBase;
			if (targetRva >= nt->OptionalHeader.SizeOfImage)
				break;

			bool targetIsCode = false;
			for (WORD sectionIndex = 0;
				sectionIndex < nt->FileHeader.NumberOfSections;
				++sectionIndex)
			{
				const IMAGE_SECTION_HEADER& section = sections[sectionIndex];
				const DWORD sectionSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
				if (targetRva >= section.VirtualAddress &&
					targetRva < section.VirtualAddress + sectionSize &&
					(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0)
				{
					targetIsCode = true;
					break;
				}
			}
			if (!targetIsCode)
				break;

			void* const runtimeTarget =
				reinterpret_cast<void*>(moduleBase + targetRva);
			if (!HooksAddressIsExecutable(runtimeTarget))
				break;

			if (vtableRvaOut)
				*vtableRvaOut = vtableRva;
			if (targetRvaOut)
				*targetRvaOut = targetRva;
			result = runtimeTarget;
		} while (false);

		HeapFree(GetProcessHeap(), 0, image);
		return result;
	}
}

bool Hooks::s_ServerUnderstandsVR = false;

bool Hooks::InitializeNoHmdNekoPostProbe()
{
	int launchArgCount = 0;
	LPWSTR* launchArgs = CommandLineToArgvW(GetCommandLineW(), &launchArgCount);
	bool l4nNekoPostEnabled = false;
	if (launchArgs)
	{
		for (int i = 0; i < launchArgCount; ++i)
		{
			if (_wcsicmp(launchArgs[i], L"-l4n_use_neko_engine_post") == 0)
			{
				l4nNekoPostEnabled = true;
				break;
			}
		}
		LocalFree(launchArgs);
	}
	if (!l4nNekoPostEnabled)
		return false;

	static std::atomic<bool> s_initializationAttempted{ false };
	bool expected = false;
	if (!s_initializationAttempted.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return hkDrawScreenSpaceRectangle.pTarget != nullptr &&
			hkDrawScreenSpaceRectangle.isEnabled;
	}

	Game::logMsg(
		"[VR][NekoPostDesktopBaseline] init=1 mode=minimal-nohmd commandLine=%s",
		GetCommandLineA() ? GetCommandLineA() : "<null>");

	HMODULE materialModule = nullptr;
	for (int attempt = 0; attempt < 600 && !materialModule; ++attempt)
	{
		materialModule = GetModuleHandleA("materialsystem.dll");
		if (!materialModule)
			Sleep(50);
	}
	if (!materialModule)
	{
		Game::logMsg(
			"[VR][NekoPostDesktopBaseline] init=0 reason=materialsystem-timeout");
		return false;
	}

	using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
	const auto createInterface = reinterpret_cast<CreateInterfaceFn>(
		GetProcAddress(materialModule, "CreateInterface"));
	if (!createInterface)
	{
		Game::logMsg(
			"[VR][NekoPostDesktopBaseline] init=0 reason=create-interface-missing module=%p",
			materialModule);
		return false;
	}

	int returnCode = 0;
	IMaterialSystem* materialSystem = nullptr;
	IMatRenderContext* context = nullptr;
	for (int attempt = 0; attempt < 600 && !context; ++attempt)
	{
		if (!materialSystem)
		{
			materialSystem = static_cast<IMaterialSystem*>(
				createInterface("VMaterialSystem080", &returnCode));
		}
		if (materialSystem)
			context = materialSystem->GetRenderContext();
		if (!context)
			Sleep(50);
	}
	if (!materialSystem)
	{
		Game::logMsg(
			"[VR][NekoPostDesktopBaseline] init=0 reason=material-interface-missing code=%d",
			returnCode);
		return false;
	}

	if (!context)
	{
		Game::logMsg(
			"[VR][NekoPostDesktopBaseline] init=0 reason=render-context-missing");
		return false;
	}

	void** const liveVtable = *reinterpret_cast<void***>(context);
	DWORD vtableRva = 0;
	DWORD targetRva = 0;
	void* const pristineTarget = HooksResolvePristineVtableTarget(
		materialModule,
		liveVtable,
		104,
		&vtableRva,
		&targetRva);
	void* const liveTarget = liveVtable ? liveVtable[104] : nullptr;

	const MH_STATUS initializeStatus = MH_Initialize();
	const bool minHookReady =
		initializeStatus == MH_OK ||
		initializeStatus == MH_ERROR_ALREADY_INITIALIZED;
	const bool created =
		minHookReady &&
		pristineTarget &&
		hkDrawScreenSpaceRectangle.createHook(
			pristineTarget,
			reinterpret_cast<LPVOID>(&dDrawScreenSpaceRectangle)) == 0;
	const bool enabled = created &&
		hkDrawScreenSpaceRectangle.enableHook() == 0;

	Game::logMsg(
		"[VR][NekoPostDesktopBaseline] init=%d mh=%d live=%p raw=%p vtableRva=%08lX targetRva=%08lX hookCreated=%d hookEnabled=%d",
		enabled ? 1 : 0,
		static_cast<int>(initializeStatus),
		liveTarget,
		pristineTarget,
		static_cast<unsigned long>(vtableRva),
		static_cast<unsigned long>(targetRva),
		created ? 1 : 0,
		enabled ? 1 : 0);

	context->Release();
	return enabled;
}

Hooks::Hooks(Game* game)
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
	}

	m_Game = game;
	m_VR = m_Game->m_VR;

	m_HUDStep = HUDPushStep::None;
	m_PushedHud = false;

	initSourceHooks();
	Game::logMsg("[VR][FirstPersonBody] HMD-anchored bone-masked local body path prepared");

	hkGetRenderTarget.enableHook();
	if (hkEndFrame.pTarget)
		hkEndFrame.enableHook();
	hkCalcViewModelView.enableHook();
	hkServerFireTerrorBullets.enableHook();
	hkClientFireTerrorBullets.enableHook();
	hkProcessUsercmds.enableHook();
	if (hkServerGameClientsClientCommand.pTarget)
	{
		hkServerGameClientsClientCommand.enableHook();
		Game::logMsg(
			"[VR][WorldPose] built-in listen-server relay hook enabled target=%p",
			hkServerGameClientsClientCommand.pTarget);
	}
	hkReadUsercmd.enableHook();
	hkWriteUsercmdDeltaToBuffer.enableHook();
	hkWriteUsercmd.enableHook();
	hkAdjustEngineViewport.enableHook();
	hkViewport.enableHook();
	hkGetViewport.enableHook();
	if (m_VR && m_VR->m_L4NNekoEnginePostLaunchEnabled &&
		hkDrawScreenSpaceRectangle.pTarget)
		hkDrawScreenSpaceRectangle.enableHook();
	hkCreateMove.enableHook();
	hkTestMeleeSwingCollisionClient.enableHook();
	hkTestMeleeSwingCollisionServer.enableHook();
	hkDoMeleeSwingServer.enableHook();
	hkStartMeleeSwingServer.enableHook();
	hkPrimaryAttackServer.enableHook();
	hkItemPostFrameServer.enableHook();
	if (s_ManualCarryThrowPropSpawnHookReady)
	{
		bool propSpawnHookEnabled = false;
		if (hkManualCarryCreatePhysicsProp.pTarget)
			propSpawnHookEnabled = hkManualCarryCreatePhysicsProp.enableHook() == 0;
		if (hkManualCarryCreateEntityByName.pTarget)
			propSpawnHookEnabled =
				hkManualCarryCreateEntityByName.enableHook() == 0 || propSpawnHookEnabled;
		s_ManualCarryThrowPropSpawnHookReady = propSpawnHookEnabled;
	}
	if (s_ManualCarryImpactKnockbackReady && hkCBaseEntityVPhysicsCollision.pTarget)
	{
		s_ManualCarryImpactKnockbackReady =
			hkCBaseEntityVPhysicsCollision.enableHook() == 0;
	}
	if (s_ManualCarryThrowHookReady)
	{
		Game::logMsg(
			"[VR][ManualCarryThrow] detached-weapon apply armed teleportSlot=118 createPhysicsHook=%p entityFactoryHook=%p propSpawnReady=%d",
			hkManualCarryCreatePhysicsProp.pTarget,
			hkManualCarryCreateEntityByName.pTarget,
			s_ManualCarryThrowPropSpawnHookReady ? 1 : 0);
	}
	Game::logMsg(
		"[VR][ManualCarryImpact] native area shove %s collisionHook=%p getOrigin=%p terrorShove=%p commonShove=%p rttiCast=%p trace=%p",
		s_ManualCarryImpactKnockbackReady ? "ready" : "unavailable",
		hkCBaseEntityVPhysicsCollision.pTarget,
		reinterpret_cast<void*>(m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.address),
		reinterpret_cast<void*>(m_Game->m_Offsets->CTerrorPlayer_OnShovedBySurvivor_Server.address),
		reinterpret_cast<void*>(m_Game->m_Offsets->INextBotEventResponder_OnShoved_Server.address),
		reinterpret_cast<void*>(m_Game->m_Offsets->Server_RTDynamicCast.address),
		m_Game->m_EngineTraceServer);
	if (s_ManualThrowHooksReady)
	{
		const bool molotovEnabled = hkMolotovProjectileCreate.enableHook() == 0;
		const bool pipeBombEnabled = hkPipeBombProjectileCreate.enableHook() == 0;
		const bool vomitJarEnabled = hkVomitJarProjectileCreate.enableHook() == 0;
		s_ManualThrowHooksReady = molotovEnabled && pipeBombEnabled && vomitJarEnabled;
		if (s_ManualThrowHooksReady)
		{
			Game::logMsg("[VR][ManualThrow] projectile Create hooks enabled molotov=%p pipe=%p vomitjar=%p",
				hkMolotovProjectileCreate.pTarget,
				hkPipeBombProjectileCreate.pTarget,
				hkVomitJarProjectileCreate.pTarget);
		}
		else
		{
			Game::logMsg("[VR][ManualThrow] failed to enable all projectile Create hooks; manual throw remains inactive");
		}
	}
	hkGetPrimaryAttackActivity.enableHook();
	hkEyePosition.enableHook();
	if (hkServerPlayerEyePosition.pTarget &&
		hkServerPlayerEyeAngles.pTarget &&
		(hkFindUseEntity.pTarget || hkPlayerUse.pTarget))
	{
		hkServerPlayerEyePosition.enableHook();
		hkServerPlayerEyeAngles.enableHook();
		if (hkFindUseEntity.pTarget)
			hkFindUseEntity.enableHook();
		if (hkPlayerUse.pTarget)
			hkPlayerUse.enableHook();
		Game::logMsg(
			"[VR][Use] installed L4D2 FindUseEntity/PlayerUse controller-pose hooks findUse=%p playerUse=%p",
			hkFindUseEntity.pTarget,
			hkPlayerUse.pTarget);
	}
	if (hkClientFindUseEntity.pTarget &&
		hkClientPlayerEyePosition.pTarget &&
		hkClientPlayerEyeVectors.pTarget)
	{
		hkClientPlayerEyePosition.enableHook();
		hkClientPlayerEyeVectors.enableHook();
		hkClientFindUseEntity.enableHook();
		Game::logMsg(
			"[VR][Use] installed L4D2 client FindUseEntity highlight controller-pose hook findUse=%p eyePos=%p eyeVectors=%p",
			hkClientFindUseEntity.pTarget,
			hkClientPlayerEyePosition.pTarget,
			hkClientPlayerEyeVectors.pTarget);
	}
	hkDrawModelExecute.enableHook();
	hkRenderView.enableHook();
	hkPushRenderTargetAndViewport.enableHook();
	hkPopRenderTargetAndViewport.enableHook();
	hkVgui_Paint.enableHook();
	hkIsSplitScreen.enableHook();
	hkPrePushRenderTarget.enableHook();
	if (hkSayText.pTarget)
		hkSayText.enableHook();
	if (hkSayText2.pTarget)
		hkSayText2.enableHook();
	if (hkTextMsg.pTarget)
		hkTextMsg.enableHook();
	if (hkEmitSoundAttenuation.pTarget)
		hkEmitSoundAttenuation.enableHook();
	if (hkEmitSoundLevel.pTarget)
		hkEmitSoundLevel.enableHook();
	if (hkUpdateLaserSight.pTarget)
		hkUpdateLaserSight.enableHook();
	if (hkUpdateFlashlight.pTarget)
		hkUpdateFlashlight.enableHook();
	if (hkUpdateFlashlightColor.pTarget)
		hkUpdateFlashlightColor.enableHook();
	if (hkConVarSetValueString.pTarget)
		hkConVarSetValueString.enableHook();
	if (hkConVarSetValueFloat.pTarget)
		hkConVarSetValueFloat.enableHook();
	if (hkConVarSetValueInt.pTarget)
		hkConVarSetValueInt.enableHook();
	if (hkConVarPrimarySetValueString.pTarget)
		hkConVarPrimarySetValueString.enableHook();
	if (hkConVarPrimarySetValueFloat.pTarget)
		hkConVarPrimarySetValueFloat.enableHook();
	if (hkConVarPrimarySetValueInt.pTarget)
		hkConVarPrimarySetValueInt.enableHook();
	if (hkConVarInternalSetValueString.pTarget)
		hkConVarInternalSetValueString.enableHook();
	if (hkConVarInternalSetValueFloat.pTarget)
		hkConVarInternalSetValueFloat.enableHook();
	if (hkConVarInternalSetValueInt.pTarget)
		hkConVarInternalSetValueInt.enableHook();
}

Hooks::~Hooks()
{
	if (MH_Uninitialize() != MH_OK)
	{
		Game::errorMsg("Failed to uninitialize MinHook");
	}
}


int Hooks::initSourceHooks()
{
	LPVOID pGetRenderTargetVFunc = (LPVOID)(m_Game->m_Offsets->GetRenderTarget.address);
	hkGetRenderTarget.createHook(pGetRenderTargetVFunc, &dGetRenderTarget);

	if (m_VR && m_VR->m_L4NNekoEnginePostLaunchEnabled &&
		m_Game->m_MaterialSystem)
	{
		void** materialVTable = *reinterpret_cast<void***>(m_Game->m_MaterialSystem);
		if (materialVTable)
		{
			// IMaterialSystem::EndFrame is vfunc #37 in VMaterialSystem080.
			hkEndFrame.createHook(materialVTable[37], &dEndFrame);
		}
	}

	LPVOID pRenderViewVFunc = (LPVOID)(m_Game->m_Offsets->RenderView.address);
	hkRenderView.createHook(pRenderViewVFunc, &dRenderView);

	LPVOID calcViewModelViewAddr = (LPVOID)(m_Game->m_Offsets->CalcViewModelView.address);
	hkCalcViewModelView.createHook(calcViewModelViewAddr, &dCalcViewModelView);

	LPVOID serverFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ServerFireTerrorBullets.address);
	hkServerFireTerrorBullets.createHook(serverFireTerrorBulletsAddr, &dServerFireTerrorBullets);

	LPVOID clientFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ClientFireTerrorBullets.address);
	hkClientFireTerrorBullets.createHook(clientFireTerrorBulletsAddr, &dClientFireTerrorBullets);

	LPVOID ProcessUsercmdsAddr = (LPVOID)(m_Game->m_Offsets->ProcessUsercmds.address);
	hkProcessUsercmds.createHook(ProcessUsercmdsAddr, &dProcessUsercmds);

	if (m_Game->m_ServerGameClients)
	{
		void** serverGameClientsVtable =
			*reinterpret_cast<void***>(m_Game->m_ServerGameClients);
		if (serverGameClientsVtable && serverGameClientsVtable[5])
		{
			// ServerGameClients003 slot 5:
			// ClientCommand(edict_t*, const CCommand&).
			hkServerGameClientsClientCommand.createHook(
				serverGameClientsVtable[5],
				&dServerGameClientsClientCommand);
		}
	}

	LPVOID ReadUserCmdAddr = (LPVOID)(m_Game->m_Offsets->ReadUserCmd.address);
	hkReadUsercmd.createHook(ReadUserCmdAddr, &dReadUsercmd);

	LPVOID WriteUsercmdDeltaToBufferAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmdDeltaToBuffer.address);
	hkWriteUsercmdDeltaToBuffer.createHook(WriteUsercmdDeltaToBufferAddr, &dWriteUsercmdDeltaToBuffer);

	LPVOID WriteUsercmdAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmd.address);
	hkWriteUsercmd.createHook(WriteUsercmdAddr, &dWriteUsercmd);

	LPVOID AdjustEngineViewportAddr = (LPVOID)(m_Game->m_Offsets->AdjustEngineViewport.address);
	hkAdjustEngineViewport.createHook(AdjustEngineViewportAddr, &dAdjustEngineViewport);

	LPVOID ViewportAddr = (LPVOID)(m_Game->m_Offsets->Viewport.address);
	hkViewport.createHook(ViewportAddr, &dViewport);

	LPVOID GetViewportAddr = (LPVOID)(m_Game->m_Offsets->GetViewport.address);
	hkGetViewport.createHook(GetViewportAddr, &dGetViewport);

	if (m_Game->m_MaterialSystem)
	{
		IMatRenderContext* const context =
			m_Game->m_MaterialSystem->GetRenderContext();
		if (context)
		{
			void** const liveVtable = *reinterpret_cast<void***>(context);
			HMODULE const materialModule = GetModuleHandleA("materialsystem.dll");
			DWORD vtableRva = 0;
			DWORD targetRva = 0;
			void* const pristineTarget = HooksResolvePristineVtableTarget(
				materialModule,
				liveVtable,
				104,
				&vtableRva,
				&targetRva);
			void* const liveTarget = liveVtable ? liveVtable[104] : nullptr;
			if (pristineTarget &&
				hkDrawScreenSpaceRectangle.createHook(
					pristineTarget,
					reinterpret_cast<LPVOID>(&dDrawScreenSpaceRectangle)) == 0)
			{
				Game::logMsg(
					"[VR][NekoPostProbe] prepared raw DSR slot=104 live=%p raw=%p vtableRva=%08lX targetRva=%08lX",
					liveTarget,
					pristineTarget,
					static_cast<unsigned long>(vtableRva),
					static_cast<unsigned long>(targetRva));
			}
			else
			{
				Game::logMsg(
					"[VR][NekoPostProbe] raw DSR unavailable; probe disabled live=%p raw=%p",
					liveTarget,
					pristineTarget);
			}
			context->Release();
		}
	}

	LPVOID MeleeSwingClientAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingClient.address);
	hkTestMeleeSwingCollisionClient.createHook(MeleeSwingClientAddr, &dTestMeleeSwingCollisionClient);

	LPVOID MeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingServer.address);
	hkTestMeleeSwingCollisionServer.createHook(MeleeSwingServerAddr, &dTestMeleeSwingCollisionServer);

	LPVOID DoMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->DoMeleeSwingServer.address);
	hkDoMeleeSwingServer.createHook(DoMeleeSwingServerAddr, &dDoMeleeSwingServer);

	LPVOID StartMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->StartMeleeSwingServer.address);
	hkStartMeleeSwingServer.createHook(StartMeleeSwingServerAddr, &dStartMeleeSwingServer);

	LPVOID PrimaryAttackServerAddr = (LPVOID)(m_Game->m_Offsets->PrimaryAttackServer.address);
	hkPrimaryAttackServer.createHook(PrimaryAttackServerAddr, &dPrimaryAttackServer);

	LPVOID ItemPostFrameServerAddr = (LPVOID)(m_Game->m_Offsets->ItemPostFrameServer.address);
	hkItemPostFrameServer.createHook(ItemPostFrameServerAddr, &dItemPostFrameServer);

	s_ManualCarryThrowHookReady = true;
	s_ManualCarryThrowPropSpawnHookReady = false;
	if (m_Game->m_Offsets->ManualCarryCreatePhysicsProp.valid)
	{
		hkManualCarryCreatePhysicsProp.createHook(
			reinterpret_cast<LPVOID>(m_Game->m_Offsets->ManualCarryCreatePhysicsProp.address),
			reinterpret_cast<LPVOID>(&dManualCarryCreatePhysicsProp));
	}
	if (m_Game->m_Offsets->ManualCarryCreateEntityByName.valid)
	{
		hkManualCarryCreateEntityByName.createHook(
			reinterpret_cast<LPVOID>(m_Game->m_Offsets->ManualCarryCreateEntityByName.address),
			reinterpret_cast<LPVOID>(&dManualCarryCreateEntityByName));
	}
	s_ManualCarryThrowPropSpawnHookReady =
		hkManualCarryCreatePhysicsProp.pTarget != nullptr ||
		hkManualCarryCreateEntityByName.pTarget != nullptr;
	if (!s_ManualCarryThrowPropSpawnHookReady)
	{
		Game::logMsg(
			"[VR][ManualCarryThrow] spawned physics-prop hook unavailable; propane/oxygen/gnome/fireworks manual throw disabled");
	}
	if (m_Game->m_Offsets->CBaseEntity_VPhysicsCollision_Server.valid)
	{
		hkCBaseEntityVPhysicsCollision.createHook(
			reinterpret_cast<LPVOID>(m_Game->m_Offsets->CBaseEntity_VPhysicsCollision_Server.address),
			reinterpret_cast<LPVOID>(&dCBaseEntityVPhysicsCollision));
	}
	s_ManualCarryImpactKnockbackReady =
		m_Game->m_EngineTraceServer &&
		m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.valid &&
		m_Game->m_Offsets->CTerrorPlayer_OnShovedBySurvivor_Server.valid &&
		m_Game->m_Offsets->Server_RTDynamicCast.valid &&
		m_Game->m_Offsets->Server_RTTI_CBaseEntity.valid &&
		m_Game->m_Offsets->Server_RTTI_INextBot.valid &&
		m_Game->m_Offsets->INextBotEventResponder_OnShoved_Server.valid &&
		hkCBaseEntityVPhysicsCollision.pTarget != nullptr;

	s_ManualThrowHooksReady = false;
	if (m_Game->m_Offsets->MolotovProjectileCreate.valid &&
		m_Game->m_Offsets->PipeBombProjectileCreate.valid &&
		m_Game->m_Offsets->VomitJarProjectileCreate.valid)
	{
		hkMolotovProjectileCreate.createHook(
			reinterpret_cast<LPVOID>(m_Game->m_Offsets->MolotovProjectileCreate.address),
			reinterpret_cast<LPVOID>(&dMolotovProjectileCreate));
		hkPipeBombProjectileCreate.createHook(
			reinterpret_cast<LPVOID>(m_Game->m_Offsets->PipeBombProjectileCreate.address),
			reinterpret_cast<LPVOID>(&dPipeBombProjectileCreate));
		hkVomitJarProjectileCreate.createHook(
			reinterpret_cast<LPVOID>(m_Game->m_Offsets->VomitJarProjectileCreate.address),
			reinterpret_cast<LPVOID>(&dVomitJarProjectileCreate));

		s_ManualThrowHooksReady =
			hkMolotovProjectileCreate.pTarget != nullptr &&
			hkPipeBombProjectileCreate.pTarget != nullptr &&
			hkVomitJarProjectileCreate.pTarget != nullptr;
	}
	if (!s_ManualThrowHooksReady)
	{
		Game::logMsg("[VR][ManualThrow] projectile Create hooks unavailable; manual throw remains inactive");
	}

	LPVOID GetPrimaryAttackActivityAddr = (LPVOID)(m_Game->m_Offsets->GetPrimaryAttackActivity.address);
	hkGetPrimaryAttackActivity.createHook(GetPrimaryAttackActivityAddr, &dGetPrimaryAttackActivity);

	LPVOID EyePositionAddr = (LPVOID)(m_Game->m_Offsets->EyePosition.address);
	hkEyePosition.createHook(EyePositionAddr, &dEyePosition);

	if (m_Game->m_Offsets->ServerPlayerEyePosition.valid &&
		m_Game->m_Offsets->ServerPlayerEyeAngles.valid &&
		(m_Game->m_Offsets->FindUseEntity.valid || m_Game->m_Offsets->PlayerUse.valid))
	{
		LPVOID serverPlayerEyePositionAddr = (LPVOID)(m_Game->m_Offsets->ServerPlayerEyePosition.address);
		LPVOID serverPlayerEyeAnglesAddr = (LPVOID)(m_Game->m_Offsets->ServerPlayerEyeAngles.address);
		hkServerPlayerEyePosition.createHook(serverPlayerEyePositionAddr, &dServerPlayerEyePosition);
		hkServerPlayerEyeAngles.createHook(serverPlayerEyeAnglesAddr, &dServerPlayerEyeAngles);
		if (m_Game->m_Offsets->FindUseEntity.valid)
		{
			LPVOID findUseEntityAddr = (LPVOID)(m_Game->m_Offsets->FindUseEntity.address);
			hkFindUseEntity.createHook(findUseEntityAddr, &dFindUseEntity);
		}
		if (m_Game->m_Offsets->PlayerUse.valid)
		{
			LPVOID playerUseAddr = (LPVOID)(m_Game->m_Offsets->PlayerUse.address);
			hkPlayerUse.createHook(playerUseAddr, &dPlayerUse);
		}
	}

	if (m_Game->m_Offsets->ClientFindUseEntity.valid &&
		m_Game->m_Offsets->ClientPlayerEyePosition.valid &&
		m_Game->m_Offsets->ClientPlayerEyeVectors.valid)
	{
		LPVOID clientFindUseEntityAddr = (LPVOID)(m_Game->m_Offsets->ClientFindUseEntity.address);
		LPVOID clientPlayerEyePositionAddr = (LPVOID)(m_Game->m_Offsets->ClientPlayerEyePosition.address);
		LPVOID clientPlayerEyeVectorsAddr = (LPVOID)(m_Game->m_Offsets->ClientPlayerEyeVectors.address);
		hkClientFindUseEntity.createHook(clientFindUseEntityAddr, &dClientFindUseEntity);
		hkClientPlayerEyePosition.createHook(clientPlayerEyePositionAddr, &dClientPlayerEyePosition);
		hkClientPlayerEyeVectors.createHook(clientPlayerEyeVectorsAddr, &dClientPlayerEyeVectors);
	}

	LPVOID DrawModelExecuteAddr = (LPVOID)(m_Game->m_Offsets->DrawModelExecute.address);
	hkDrawModelExecute.createHook(DrawModelExecuteAddr, &dDrawModelExecute);

	LPVOID PushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PushRenderTargetAndViewport.address);
	hkPushRenderTargetAndViewport.createHook(PushRenderTargetAddr, &dPushRenderTargetAndViewport);

	LPVOID PopRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PopRenderTargetAndViewport.address);
	hkPopRenderTargetAndViewport.createHook(PopRenderTargetAddr, &dPopRenderTargetAndViewport);

	LPVOID VGui_PaintAddr = (LPVOID)(m_Game->m_Offsets->VGui_Paint.address);
	hkVgui_Paint.createHook(VGui_PaintAddr, &dVGui_Paint);

	LPVOID IsSplitScreenAddr = (LPVOID)(m_Game->m_Offsets->IsSplitScreen.address);
	hkIsSplitScreen.createHook(IsSplitScreenAddr, &dIsSplitScreen);

	LPVOID PrePushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PrePushRenderTarget.address);
	hkPrePushRenderTarget.createHook(PrePushRenderTargetAddr, &dPrePushRenderTarget);

	LPVOID SayTextAddr = reinterpret_cast<LPVOID>(m_Game->m_BaseClient + kClientSayTextHandlerOffset);
	hkSayText.createHook(SayTextAddr, &dSayText);

	LPVOID SayText2Addr = reinterpret_cast<LPVOID>(m_Game->m_BaseClient + kClientSayText2HandlerOffset);
	hkSayText2.createHook(SayText2Addr, &dSayText2);

	LPVOID TextMsgAddr = reinterpret_cast<LPVOID>(m_Game->m_BaseClient + kClientTextMsgHandlerOffset);
	hkTextMsg.createHook(TextMsgAddr, &dTextMsg);

	// alliedmodders/hl2sdk l4d2 public/engine/IEngineSound.h defines:
	//   #5 EmitSound(... float flAttenuation ...)
	//   #6 EmitSound(... soundlevel_t iSoundlevel ...)
	// No specialDSP stack argument exists in the L4D2 ABI. Hook both verified overloads
	// so MagazineInteraction can suppress native reload sounds while physical insertion is active.
	if (m_Game->m_EngineSound)
	{
		void** engineSoundVTable = nullptr;
#if defined(_MSC_VER)
		__try
		{
			engineSoundVTable = *reinterpret_cast<void***>(m_Game->m_EngineSound);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			engineSoundVTable = nullptr;
		}
#else
		engineSoundVTable = *reinterpret_cast<void***>(m_Game->m_EngineSound);
#endif

		constexpr size_t kEmitSoundAttenuationVFunc = 5;
		constexpr size_t kEmitSoundLevelVFunc = 6;
		if (engineSoundVTable &&
			engineSoundVTable[kEmitSoundAttenuationVFunc] &&
			engineSoundVTable[kEmitSoundLevelVFunc])
		{
			const int attenuationHookResult = hkEmitSoundAttenuation.createHook(
				engineSoundVTable[kEmitSoundAttenuationVFunc],
				&dEmitSoundAttenuation);
			const int levelHookResult = hkEmitSoundLevel.createHook(
				engineSoundVTable[kEmitSoundLevelVFunc],
				&dEmitSoundLevel);
			if (attenuationHookResult == 0 && levelHookResult == 0)
				Game::logMsg("[VR][MagazineInteraction][Audio] verified L4D2 IEngineSoundClient003 EmitSound hooks installed");
			else
				Game::logMsg("[VR][MagazineInteraction][Audio] warning: one or more verified EmitSound hooks failed to install");
		}
		else
		{
			Game::logMsg("[VR][MagazineInteraction][Audio] disabled: IEngineSoundClient003 vtable unavailable");
		}
	}
	else
	{
		Game::logMsg("[VR][MagazineInteraction][Audio] disabled: IEngineSoundClient003 interface unavailable");
	}

	if (m_Game->m_Offsets->UpdateLaserSight.valid)
	{
		LPVOID UpdateLaserSightAddr = (LPVOID)(m_Game->m_Offsets->UpdateLaserSight.address);
		hkUpdateLaserSight.createHook(UpdateLaserSightAddr, &dUpdateLaserSight);
	}

	if (m_Game->m_Offsets->UpdateFlashlight.valid)
	{
		LPVOID UpdateFlashlightAddr = (LPVOID)(m_Game->m_Offsets->UpdateFlashlight.address);
		hkUpdateFlashlight.createHook(UpdateFlashlightAddr, &dUpdateFlashlight);
	}

	if (m_Game->m_Offsets->UpdateFlashlightColor.valid)
	{
		LPVOID UpdateFlashlightColorAddr = (LPVOID)(m_Game->m_Offsets->UpdateFlashlightColor.address);
		hkUpdateFlashlightColor.createHook(UpdateFlashlightColorAddr, &dUpdateFlashlightColor);
	}

	const char* conVarSamples[] = { "name", "r_shadows", "cl_ragdoll_limit" };
	for (const char* sample : conVarSamples)
	{
		if (!hkConVarSetValueString.pTarget)
		{
			LPVOID target = m_Game->GetConVarStringSetValueTarget(sample);
			if (target)
				hkConVarSetValueString.createHook(target, &dConVarSetValueString);
		}

		if (!hkConVarSetValueFloat.pTarget)
		{
			LPVOID target = m_Game->GetConVarFloatSetValueTarget(sample);
			if (target)
				hkConVarSetValueFloat.createHook(target, &dConVarSetValueFloat);
		}

		if (!hkConVarSetValueInt.pTarget)
		{
			LPVOID target = m_Game->GetConVarIntSetValueTarget(sample);
			if (target)
				hkConVarSetValueInt.createHook(target, &dConVarSetValueInt);
		}

		if (hkConVarSetValueString.pTarget &&
			hkConVarSetValueFloat.pTarget &&
			hkConVarSetValueInt.pTarget)
			break;
	}

	uintptr_t clientModeAddress = m_Game->m_Offsets->g_pClientMode.address;
	if (!clientModeAddress)
	{
		Game::errorMsg("g_pClientMode address was null; aborting CreateMove hook installation");
		return 0;
	}

	void* clientMode = nullptr;
	constexpr int kMaxAttempts = 500;
	for (int attempt = 0; attempt < kMaxAttempts && !clientMode; ++attempt)
	{
		uintptr_t clientModePtr = *reinterpret_cast<uintptr_t*>(clientModeAddress);
		if (clientModePtr)
		{
			uintptr_t clientModeValue = *reinterpret_cast<uintptr_t*>(clientModePtr);
			if (clientModeValue)
			{
				clientMode = reinterpret_cast<void*>(clientModeValue);
				break;
			}
		}

		Sleep(10);
	}

	if (!clientMode)
	{
		Game::errorMsg("Timed out waiting for g_pClientMode; CreateMove hook not installed");
		return 0;
	}

	void*** clientModePtr = reinterpret_cast<void***>(clientMode);
	void** clientModeVTable = (clientModePtr != nullptr) ? *clientModePtr : nullptr;
	if (!clientModeVTable)
	{
		Game::errorMsg("Client mode vtable pointer was null; CreateMove hook not installed");
		return 0;
	}

	hkCreateMove.createHook(clientModeVTable[27], dCreateMove);
	return 1;
}


