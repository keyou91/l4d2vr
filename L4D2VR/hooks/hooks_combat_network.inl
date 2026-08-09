void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
	const bool profilePresentSpike =
		m_VR && m_VR->m_PresentSpikeDebugLog;
	const auto presentSpikeEndFrameEntry = profilePresentSpike
		? std::chrono::steady_clock::now()
		: std::chrono::steady_clock::time_point{};
	const bool logQueuedEndFrame =
		m_VR &&
		m_VR->m_RenderPipelineDebugLog &&
		m_Game &&
		m_Game->GetMatQueueMode() != 0;

	const uint32_t beforeCompleted = logQueuedEndFrame
		? m_VR->m_RenderCompletedFrameId.load(std::memory_order_acquire)
		: 0;
	const uint32_t beforeSubmitted = logQueuedEndFrame
		? m_VR->m_LastSubmittedFrameId.load(std::memory_order_acquire)
		: 0;
	const uint32_t beforePose = logQueuedEndFrame
		? m_VR->m_SubmitPoseToken.load(std::memory_order_acquire)
		: 0;
	const bool beforeRenderedNew = logQueuedEndFrame
		? m_VR->m_RenderedNewFrame.load(std::memory_order_acquire)
		: false;

	hkEndFrame.fOriginal(ecx);

	if (profilePresentSpike)
	{
		const auto presentSpikeEndFrameExit = std::chrono::steady_clock::now();
		const uint64_t entryUs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				presentSpikeEndFrameEntry.time_since_epoch()).count());
		const uint64_t exitUs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				presentSpikeEndFrameExit.time_since_epoch()).count());

		m_VR->m_PresentSpikeEndFrameSeq.fetch_add(1u, std::memory_order_acq_rel);
		m_VR->m_PresentSpikeEndFrameEntryUs.store(entryUs, std::memory_order_relaxed);
		m_VR->m_PresentSpikeEndFrameExitUs.store(exitUs, std::memory_order_relaxed);
		m_VR->m_PresentSpikeEndFrameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
		m_VR->m_PresentSpikeEndFrameSeq.fetch_add(1u, std::memory_order_release);
	}

	if (logQueuedEndFrame)
	{
		static thread_local std::chrono::steady_clock::time_point s_lastMaterialEndFrameLog{};
		if (!ShouldThrottleLog(s_lastMaterialEndFrameLog, m_VR->m_RenderPipelineDebugLogHz))
		{
			const int queueMode = m_Game->GetMatQueueMode();
			const uint32_t afterCompleted = m_VR->m_RenderCompletedFrameId.load(std::memory_order_acquire);
			const uint32_t afterSubmitted = m_VR->m_LastSubmittedFrameId.load(std::memory_order_acquire);
			const uint32_t afterPose = m_VR->m_SubmitPoseToken.load(std::memory_order_acquire);
			const bool afterRenderedNew = m_VR->m_RenderedNewFrame.load(std::memory_order_acquire);
			Game::logMsg("[VR][Queued][MaterialEndFrame] tid=%lu q=%d completed=%u->%u submitted=%u->%u pose=%u->%u renderedNew=%d->%d inFlight=%d",
				GetCurrentThreadId(), queueMode,
				beforeCompleted, afterCompleted,
				beforeSubmitted, afterSubmitted,
				beforePose, afterPose,
				beforeRenderedNew ? 1 : 0,
				afterRenderedNew ? 1 : 0,
				m_VR->m_SubmitInFlight.load(std::memory_order_acquire) ? 1 : 0);
		}
	}
}

static inline void CallCalcViewModelViewOriginal(void* ecx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled || !Hooks::m_VR->m_ViewmodelDisableMoveBob || !owner)
	{
		Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
		return;
	}

	C_BasePlayer* ownerPlayer = reinterpret_cast<C_BasePlayer*>(owner);
	const Vector savedVelocity = ownerPlayer->m_vecVelocity;
	ownerPlayer->m_vecVelocity = Vector{ 0.0f, 0.0f, 0.0f };
	Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
	ownerPlayer->m_vecVelocity = savedVelocity;
}


void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	if (m_VR->m_IsVREnabled)
	{
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		const bool multiCoreQueued = (queueMode == 2);
		const bool forceDisableMoveBob = m_VR->m_ViewmodelDisableMoveBob;
		// Real-controller VM-pose hands need a rigid shared root. Source may add
		// viewmodel bob/lag after the controller pose is supplied, which makes the
		// visible weapon drift relative to the standalone glove during wrist rotation.
		const bool forceControllerHardLock =
			!m_VR->m_MouseModeEnabled &&
			(m_VR->m_VrHandsRightUseViewmodelPose || m_VR->IsVrHandsTwoHandedGripPoseActive());

		// ------------------------------------------------------------
		// Single-thread path (mat_queue_mode 0/1)
		//
		// 你的“单线程 vm 正常版本”里就是这个行为：把 controller 侧的
		// viewmodel 推荐位姿当作 eye 输入喂给 CalcViewModelView。
		//
		// 这样 engine 生成的 viewmodel/weapon/attachment(枪口闪光/烟雾)
		// 都会围绕枪来算，而不会 head-locked 在 HMD 上。
		// ------------------------------------------------------------
		if (!multiCoreQueued)
		{
			vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
			if (!forceDisableMoveBob && !forceControllerHardLock)
			{
				hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
				return;
			}

			CallCalcViewModelViewOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				ent->GetAbsOrigin() = vecNewOrigin;
				ent->GetAbsAngles() = vecNewAngles;
			}
			return;
		}

		// ------------------------------------------------------------
		// Multi-core queued path (mat_queue_mode 2)
		//
		// Use the render-frame viewmodel target as the CalcViewModelView input.
		// The previous queued path called the original function with engine eye
		// origin/angles first, then moved the finished model to the controller target.
		// That left Source viewmodel bob/lag/animation bones based on a different
		// frame basis from the final DrawModelExecute bone delta, which can jitter
		// on some queued-render schedules even when the controller is still.
		// ------------------------------------------------------------
		struct RenderSnapshotTLSGuard
		{
			bool enabled = false;
			bool prev = false;
			RenderSnapshotTLSGuard(bool en)
			{
				enabled = en;
				if (enabled)
				{
					prev = VR::t_UseRenderFrameSnapshot;
					VR::t_UseRenderFrameSnapshot = true;
				}
			}
			~RenderSnapshotTLSGuard()
			{
				if (enabled)
					VR::t_UseRenderFrameSnapshot = prev;
			}
		} tlsGuard(true);

		if (!m_VR->m_QueuedViewmodelStabilize && !forceDisableMoveBob && !forceControllerHardLock)
		{
			CallCalcViewModelViewOriginal(ecx, owner, eyePosition, eyeAngles);
			return;
		}

		const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos();
		const QAngle targetAngles = m_VR->GetRecommendedViewmodelAbsAngle();

		// Call engine viewmodel logic around the same controller/render-frame basis
		// that DrawModelExecute will later use.
		CallCalcViewModelViewOriginal(ecx, owner, targetOrigin, targetAngles);

		{
			// Capture what the engine produced (before we force exact entity state) for debug.
			Vector engineOrigin = {};
			QAngle engineAngles = {};
			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				engineOrigin = ent->GetAbsOrigin();
				engineAngles = ent->GetAbsAngles();
			}

			bool originSet = false;
			if (m_Game && m_Game->m_Offsets && m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address && ecx)
			{
				using SetAbsOriginFn = void(__thiscall*)(void*, const Vector&);
				auto setAbsOrigin = (SetAbsOriginFn)m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address;
				setAbsOrigin(ecx, targetOrigin);
				originSet = true;
			}

			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				ent->GetAbsAngles() = targetAngles;
				if (!originSet)
					ent->GetAbsOrigin() = targetOrigin;
			}

			if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
			{
				static thread_local std::chrono::steady_clock::time_point s_last{};
				if (!ShouldThrottleLog(s_last, m_VR->m_QueuedViewmodelStabilizeDebugLogHz))
				{
					const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
					const uint32_t tid = (uint32_t)GetCurrentThreadId();

					const float dx = targetOrigin.x - engineOrigin.x;
					const float dy = targetOrigin.y - engineOrigin.y;
					const float dz = targetOrigin.z - engineOrigin.z;
					const float dpos = sqrtf(dx * dx + dy * dy + dz * dz);

					Game::logMsg(
						"[VR][VM][queue] tid=%u qmode=%d seq=%u dpos=%.2f eyeO=(%.2f %.2f %.2f) eyeA=(%.2f %.2f %.2f) tgtO=(%.2f %.2f %.2f) tgtA=(%.2f %.2f %.2f) engO=(%.2f %.2f %.2f) engA=(%.2f %.2f %.2f)",
						tid, queueMode, seq, dpos,
						eyePosition.x, eyePosition.y, eyePosition.z,
						eyeAngles.x, eyeAngles.y, eyeAngles.z,
						targetOrigin.x, targetOrigin.y, targetOrigin.z,
						targetAngles.x, targetAngles.y, targetAngles.z,
						engineOrigin.x, engineOrigin.y, engineOrigin.z,
						engineAngles.x, engineAngles.y, engineAngles.z);
				}
			}
		}

		return; // we already called original
	}

	return hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
}

namespace
{
	static constexpr int kMaxGameLaserBeamEffects = 1;
	static constexpr int kMaxGameLaserDotEffects = 1;

	using tCreateParticleEffect = void* (__thiscall*)(void* thisptr, const char* particleName, int attachType, int attachment, Vector originOffset, int unknown);
	using tStopParticleEffect = void(__thiscall*)(void* thisptr, void* effect);
	using tParticleSetControlPointPosition = void(__thiscall*)(void* thisptr, int controlPoint, const Vector& position);
	using tParticleSetControlPointForwardVector = void(__thiscall*)(void* thisptr, int controlPoint, const Vector& forward);

	struct LocalWorldLaserParticleState
	{
		void* beamEffects[kMaxGameLaserBeamEffects] = {};
		void* dotEffects[kMaxGameLaserDotEffects] = {};
		int beamCount = 0;
		int dotCount = 0;
		void* particleProperty = nullptr;
		C_BaseCombatWeapon* weapon = nullptr;
		void* owner = nullptr;
	};

	static LocalWorldLaserParticleState s_localWorldLaserParticle;

	static inline bool TryReadPointer(const void* ptrAddress, void*& out)
	{
		out = nullptr;
		if (!IsReadableMemoryRange(ptrAddress, sizeof(void*)))
			return false;

		__try
		{
			out = *reinterpret_cast<void* const*>(ptrAddress);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = nullptr;
			return false;
		}
	}

	static inline bool TryWritePointer(void* ptrAddress, void* value)
	{
		if (!ptrAddress)
			return false;

		__try
		{
			*reinterpret_cast<void**>(ptrAddress) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static inline bool TryReadInt(const void* ptrAddress, int& out)
	{
		out = 0;
		if (!IsReadableMemoryRange(ptrAddress, sizeof(int)))
			return false;

		__try
		{
			out = *reinterpret_cast<const int*>(ptrAddress);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = 0;
			return false;
		}
	}

	static inline void* CreateParticleEffect(void* particleProperty, const char* particleName, int attachType, int attachment, const Vector& originOffset, int unknown = 0)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->CreateParticleEffect.valid)
			return nullptr;
		if (!particleProperty || !particleName || !particleName[0])
			return nullptr;

		auto createParticleEffect = reinterpret_cast<tCreateParticleEffect>(
			Hooks::m_Game->m_Offsets->CreateParticleEffect.address);
		if (!createParticleEffect)
			return nullptr;

		__try
		{
			return createParticleEffect(particleProperty, particleName, attachType, attachment, originOffset, unknown);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	static inline void StopParticleEffect(void* particleProperty, void* effect)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->StopParticleEffect.valid)
			return;
		if (!particleProperty || !effect || !IsReadableMemoryRange(particleProperty, sizeof(void*)) || !IsReadableMemoryRange(effect, sizeof(void*)))
			return;

		auto stopParticleEffect = reinterpret_cast<tStopParticleEffect>(
			Hooks::m_Game->m_Offsets->StopParticleEffect.address);
		if (!stopParticleEffect)
			return;

		__try
		{
			stopParticleEffect(particleProperty, effect);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	static inline void ClearLocalWorldLaserParticle()
	{
		if (s_localWorldLaserParticle.particleProperty)
		{
			for (void* effect : s_localWorldLaserParticle.beamEffects)
			{
				if (effect)
					StopParticleEffect(s_localWorldLaserParticle.particleProperty, effect);
			}
			for (void* effect : s_localWorldLaserParticle.dotEffects)
			{
				if (effect)
					StopParticleEffect(s_localWorldLaserParticle.particleProperty, effect);
			}
		}

		s_localWorldLaserParticle = {};
	}

	static inline bool SetParticleControlPoint(void* effect, int controlPoint, const Vector& position)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->ParticleSetControlPointPosition.valid)
			return false;
		if (!effect || !IsReadableMemoryRange(effect, sizeof(void*)))
			return false;

		auto setControlPoint = reinterpret_cast<tParticleSetControlPointPosition>(
			Hooks::m_Game->m_Offsets->ParticleSetControlPointPosition.address);
		if (!setControlPoint)
			return false;

		__try
		{
			setControlPoint(effect, controlPoint, position);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static inline bool SetParticleControlPointForwardVector(void* effect, int controlPoint, const Vector& forward)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->ParticleSetControlPointForwardVector.valid)
			return false;
		if (!effect || !IsReadableMemoryRange(effect, sizeof(void*)))
			return false;

		Vector normalizedForward = forward;
		if (normalizedForward.IsZero())
			return false;
		VectorNormalize(normalizedForward);

		auto setForward = reinterpret_cast<tParticleSetControlPointForwardVector>(
			Hooks::m_Game->m_Offsets->ParticleSetControlPointForwardVector.address);
		if (!setForward)
			return false;

		__try
		{
			setForward(effect, controlPoint, normalizedForward);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static inline bool HasLocalWorldLaserParticle()
	{
		for (void* effect : s_localWorldLaserParticle.beamEffects)
		{
			if (effect)
				return true;
		}
		for (void* effect : s_localWorldLaserParticle.dotEffects)
		{
			if (effect)
				return true;
		}
		return false;
	}

	static inline bool LocalWorldLaserParticleHasInvalidEffect()
	{
		for (void* effect : s_localWorldLaserParticle.beamEffects)
		{
			if (effect && !IsReadableMemoryRange(effect, sizeof(void*)))
				return true;
		}
		for (void* effect : s_localWorldLaserParticle.dotEffects)
		{
			if (effect && !IsReadableMemoryRange(effect, sizeof(void*)))
				return true;
		}
		return false;
	}

	static inline int GetGameLaserBeamCount(const VR* vr)
	{
		if (!vr || vr->m_GameLaserSightColorA <= 0)
			return 0;

		const float thickness = std::clamp(vr->m_GameLaserSightThickness, 0.0f, 8.0f);
		const float alpha = std::clamp(static_cast<float>(vr->m_GameLaserSightColorA) / 255.0f, 0.0f, 1.0f);

		int count = 1;
		if (thickness >= 0.20f || alpha >= 0.35f)
			count = 3;
		if (thickness >= 0.50f || alpha >= 0.65f)
			count = 5;
		if (thickness >= 1.00f || alpha >= 0.85f)
			count = 9;

		return std::clamp(count, 1, kMaxGameLaserBeamEffects);
	}

	static inline int GetGameLaserDotCount(const VR* vr)
	{
		// Keep only the main laser beam.
		// The extra dot uses another weapon_laser_sight effect and makes the result look multi-stroked.
		(void)vr;
		return 0;
	}

	static inline void GetLaserBillboardBasis(VR* vr, const Vector& direction, Vector& right, Vector& up)
	{
		right = vr ? vr->m_HmdRight : Vector{ 0.0f, 0.0f, 0.0f };
		up = vr ? vr->m_HmdUp : Vector{ 0.0f, 0.0f, 0.0f };

		if (right.IsZero() && vr)
			right = vr->m_RightControllerRight;
		if (up.IsZero() && vr)
			up = vr->m_RightControllerUp;
		if (up.IsZero())
			up = Vector{ 0.0f, 0.0f, 1.0f };

		if (right.IsZero())
			right = CrossProduct(up, direction);
		if (right.IsZero())
			right = CrossProduct(Vector{ 0.0f, 0.0f, 1.0f }, direction);

		if (!right.IsZero())
			VectorNormalize(right);
		if (!up.IsZero())
			VectorNormalize(up);
	}

	static inline void SetLaserParticleLine(void* effect, const Vector& start, const Vector& end)
	{
		if (!effect)
			return;

		Vector direction = end - start;
		if (direction.IsZero())
			return;
		VectorNormalize(direction);

		SetParticleControlPoint(effect, 1, start);
		SetParticleControlPoint(effect, 2, end);
		SetParticleControlPoint(effect, 3, start + direction * 32.0f);
		SetParticleControlPointForwardVector(effect, 1, direction);
	}

	static inline void ClearOriginalLocalLaserParticleSlot(void* terrorPlayer, int effectOffset)
	{
		if (!terrorPlayer)
			return;

		auto* base = reinterpret_cast<uint8_t*>(terrorPlayer);
		void* effect = nullptr;
		void* effectAddress = base + effectOffset;
		if (!TryReadPointer(effectAddress, effect) || !effect)
			return;

		void* particleProperty = base + 0x2A8;
		StopParticleEffect(particleProperty, effect);
		TryWritePointer(effectAddress, nullptr);
	}

	static inline void ClearOriginalLocalLaserParticles(void* terrorPlayer)
	{
		ClearOriginalLocalLaserParticleSlot(terrorPlayer, 0x28D0);
		ClearOriginalLocalLaserParticleSlot(terrorPlayer, 0x28E8);
	}

	static inline bool GetLaserAimSegment(Vector& start, Vector& end)
	{
		VR* vr = Hooks::m_VR;
		if (!vr || !vr->m_IsVREnabled)
			return false;

		if (vr->m_HasAimLine)
		{
			start = vr->m_AimLineStart;
			end = vr->m_AimLineEnd;

			Vector direction = end - start;
			if (!direction.IsZero())
				return true;
		}

		QAngle controllerAng = vr->GetRightControllerAbsAngle();
		Vector direction{}, right{}, up{};
		QAngle::AngleVectors(controllerAng, &direction, &right, &up);
		if (direction.IsZero())
			direction = vr->m_LastAimDirection.IsZero() ? vr->m_HmdForward : vr->m_LastAimDirection;
		if (direction.IsZero())
			return false;

		VectorNormalize(direction);
		start = vr->GetRightControllerViewmodelAbsPos() + direction * 2.0f;
        if (vr->m_ForceNonVRServerMovement && vr->m_HasNonVRAimSolution)
            end = vr->m_NonVRAimHitPoint;
        else
            end = start + direction * 8192.0f;

		return !((end - start).IsZero());
	}

	static inline Vector BuildLaserSightEndOffset(const Vector& direction)
	{
		VR* vr = Hooks::m_VR;
		if (!vr)
			return Vector{ 0.0f, 0.0f, 0.0f };

		Vector right = vr->m_HmdRight;
		Vector up = vr->m_HmdUp;

		if (right.IsZero())
			right = vr->m_RightControllerRight;
		if (up.IsZero())
			up = vr->m_RightControllerUp;
		if (up.IsZero())
			up = Vector(0.0f, 0.0f, 1.0f);

		if (right.IsZero())
			right = CrossProduct(up, direction);
		if (right.IsZero())
			right = CrossProduct(Vector(0.0f, 0.0f, 1.0f), direction);

		if (!right.IsZero())
			VectorNormalize(right);
		if (!up.IsZero())
			VectorNormalize(up);

		const Vector& offset = vr->m_GameLaserSightEndOffset;
		return right * offset.x + up * offset.y + direction * offset.z;
	}

	static inline bool SuppressGameLaserSightBeamForD3DAimLine(const VR* vr)
	{
		return vr && vr->m_IsVREnabled && vr->m_D3DAimLineOverlayEnabled;
	}

	static inline void UpdateLocalWorldLaserParticle(void* terrorPlayer)
	{
		if (!terrorPlayer || !Hooks::m_Game || !Hooks::m_Game->m_EngineClient || !Hooks::m_VR)
			return;

		VR* vr = Hooks::m_VR;
		const int localPlayerIndex = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
		C_BaseEntity* localEntity = Hooks::m_Game->GetClientEntity(localPlayerIndex);
		if (localEntity && reinterpret_cast<void*>(localEntity) != terrorPlayer)
			return;

		auto* localPlayer = reinterpret_cast<C_BasePlayer*>(localEntity ? localEntity : terrorPlayer);
		if (!localPlayer)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		if (!vr->m_IsVREnabled || !vr->m_GameLaserSightBeamEnabled || SuppressGameLaserSightBeamForD3DAimLine(vr))
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		C_BaseCombatWeapon* activeWeapon = nullptr;
		__try
		{
			activeWeapon = localPlayer->GetActiveWeapon();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			activeWeapon = nullptr;
		}
		if (!activeWeapon)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		auto* weapon = reinterpret_cast<C_WeaponCSBase*>(activeWeapon);
		if (!vr->IsWeaponLaserSightActive(weapon))
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		Vector start{}, end{};
		if (!GetLaserAimSegment(start, end))
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		Vector direction = end - start;
		if (direction.IsZero())
		{
			ClearLocalWorldLaserParticle();
			return;
		}
		VectorNormalize(direction);

		const Vector laserEnd = end + BuildLaserSightEndOffset(direction);
		Vector laserDirection = laserEnd - start;
		if (laserDirection.IsZero())
			laserDirection = direction;
		VectorNormalize(laserDirection);

		const int desiredBeamCount = GetGameLaserBeamCount(vr);
		const int desiredDotCount = GetGameLaserDotCount(vr);
		if (desiredBeamCount <= 0)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		void* particleProperty = reinterpret_cast<uint8_t*>(activeWeapon) + 0x2A8;
		if (s_localWorldLaserParticle.weapon != activeWeapon ||
			s_localWorldLaserParticle.owner != terrorPlayer ||
			s_localWorldLaserParticle.particleProperty != particleProperty ||
			s_localWorldLaserParticle.beamCount != desiredBeamCount ||
			s_localWorldLaserParticle.dotCount != desiredDotCount)
		{
			ClearLocalWorldLaserParticle();
		}

		if (LocalWorldLaserParticleHasInvalidEffect())
			ClearLocalWorldLaserParticle();

		if (!HasLocalWorldLaserParticle())
		{
			const auto* weaponBase = reinterpret_cast<const uint8_t*>(activeWeapon);
			int attachment = 0;
			TryReadInt(weaponBase + 0xCE0, attachment);
			const int attachType = (attachment > 0) ? 5 : 0;
			if (attachment <= 0)
				attachment = 0;

			const Vector originOffset{ 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < desiredBeamCount; ++i)
				s_localWorldLaserParticle.beamEffects[i] = CreateParticleEffect(particleProperty, "weapon_laser_sight", attachType, attachment, originOffset);
			for (int i = 0; i < desiredDotCount; ++i)
				s_localWorldLaserParticle.dotEffects[i] = CreateParticleEffect(particleProperty, "weapon_laser_sight", 0, 0, originOffset);

			s_localWorldLaserParticle.beamCount = desiredBeamCount;
			s_localWorldLaserParticle.dotCount = desiredDotCount;
			s_localWorldLaserParticle.particleProperty = particleProperty;
			s_localWorldLaserParticle.weapon = activeWeapon;
			s_localWorldLaserParticle.owner = terrorPlayer;

			if (!HasLocalWorldLaserParticle())
			{
				static bool s_loggedCreateFailed = false;
				if (!s_loggedCreateFailed)
				{
					s_loggedCreateFailed = true;
				}
				return;
			}

			static bool s_loggedCreated = false;
			if (!s_loggedCreated)
			{
				s_loggedCreated = true;
			}
		}

		Vector right{}, up{};
		GetLaserBillboardBasis(vr, laserDirection, right, up);
		const float radius = std::clamp(vr->m_GameLaserSightThickness, 0.0f, 8.0f);
		static const float kBeamPattern[kMaxGameLaserBeamEffects][2] = {
			{ 0.0f, 0.0f }
		};

		for (int i = 0; i < s_localWorldLaserParticle.beamCount && i < kMaxGameLaserBeamEffects; ++i)
		{
			void* effect = s_localWorldLaserParticle.beamEffects[i];
			if (!effect)
				continue;

			const Vector offset = right * (kBeamPattern[i][0] * radius) + up * (kBeamPattern[i][1] * radius);
			SetLaserParticleLine(effect, start + offset, laserEnd + offset);
		}

		const float dotRadius = std::max(2.0f, radius * 3.0f);
		Vector diagonalA = right + up;
		Vector diagonalB = right - up;
		if (!diagonalA.IsZero())
			VectorNormalize(diagonalA);
		if (!diagonalB.IsZero())
			VectorNormalize(diagonalB);

		const Vector dotAxes[kMaxGameLaserDotEffects] = {
			right
		};
		for (int i = 0; i < s_localWorldLaserParticle.dotCount && i < kMaxGameLaserDotEffects; ++i)
		{
			void* effect = s_localWorldLaserParticle.dotEffects[i];
			if (!effect)
				continue;

			Vector axis = dotAxes[i];
			if (axis.IsZero())
				continue;
			VectorNormalize(axis);
			SetLaserParticleLine(effect, laserEnd - axis * dotRadius, laserEnd + axis * dotRadius);
		}
	}

	static inline char AsciiLower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}

	static inline bool TextureNameIsFlashlight(const char* textureName)
	{
		if (!textureName || !textureName[0])
			return true;

		const char needle[] = "flashlight";
		__try
		{
			for (const char* p = textureName; *p; ++p)
			{
				const char* a = p;
				const char* b = needle;
				while (*a && *b && AsciiLower(*a) == *b)
				{
					++a;
					++b;
				}
				if (!*b)
					return true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		return false;
	}

	static inline bool IsFiniteVector(const Vector& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	static inline bool BuildHmdFlashlightPose(int entIndex, const char* textureName, Vector& origin, Vector& forward, Vector& right, Vector& up)
	{
		VR* vr = Hooks::m_VR;
		if (!vr || !vr->m_IsVREnabled || !vr->m_FlashlightFollowHmd)
			return false;
		if (vr->IsThirdPersonCameraActive())
			return false;
		if (!Hooks::m_Game || !Hooks::m_Game->m_EngineClient || !Hooks::m_Game->m_EngineClient->IsInGame())
			return false;

		const int localPlayerIndex = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
		if (localPlayerIndex <= 0 || entIndex != localPlayerIndex)
			return false;
		if (!vr->m_FlashlightFollowHmdForFirearms && vr->m_ScopeWeaponIsFirearm)
			return false;
		if (!TextureNameIsFlashlight(textureName))
			return false;

		origin = vr->m_HmdPosAbs;
		forward = vr->m_HmdForward;
		right = vr->m_HmdRight;
		up = vr->m_HmdUp;

		if (forward.IsZero() || right.IsZero() || up.IsZero())
			QAngle::AngleVectors(vr->m_HmdAngAbs, &forward, &right, &up);

		if (forward.IsZero() || !IsFiniteVector(origin) || !IsFiniteVector(forward) ||
			!IsFiniteVector(right) || !IsFiniteVector(up))
			return false;

		VectorNormalize(forward);
		if (!right.IsZero())
			VectorNormalize(right);
		if (!up.IsZero())
			VectorNormalize(up);

		return true;
	}

	static inline bool BuildAnglesToTarget(const Vector& origin, const Vector& target, QAngle& angles)
	{
		Vector to = target - origin;
		if (to.IsZero())
			return false;

		VectorNormalize(to);
		QAngle ang;
		QAngle::VectorAngles(to, ang);
		NormalizeAndClampViewAngles(ang);
		angles = ang;
		return true;
	}

	static inline bool BuildAnglesFromDirection(Vector direction, QAngle& angles)
	{
		if (direction.IsZero())
			return false;

		VectorNormalize(direction);
		QAngle ang;
		QAngle::VectorAngles(direction, ang);
		NormalizeAndClampViewAngles(ang);
		angles = ang;
		return true;
	}

	static inline bool BuildAnglesToAimLineTarget(VR* vr, const Vector& origin, QAngle& angles)
	{
		if (!vr || !vr->m_IsVREnabled || vr->m_ForceNonVRServerMovement || !vr->m_HasAimLine || vr->m_HasThrowArc)
			return false;

		return BuildAnglesToTarget(origin, vr->m_AimLineEnd, angles);
	}

	static inline bool BuildAnglesToEncodedServerAim(VR* vr, const Vector& origin, QAngle& angles)
	{
		if (!vr || !vr->m_IsVREnabled || vr->m_ForceNonVRServerMovement)
			return false;

		if (vr->m_HasAimLine && !vr->m_HasThrowArc && BuildAnglesToTarget(origin, vr->m_AimLineEnd, angles))
			return true;

		if (vr->m_HasThrowArc && BuildAnglesFromDirection(vr->m_LastAimDirection, angles))
			return true;

		return false;
	}

	static inline bool TryBuildLocalAimLineBulletVisualRay(VR* vr, Vector& origin, QAngle& angles)
	{
		if (!vr || !vr->m_IsVREnabled || vr->m_MouseModeEnabled || vr->IsScopeActive())
			return false;

		if (vr->m_HasAimLine && !vr->m_HasThrowArc)
		{
			const Vector start = vr->m_AimLineStart;
			const Vector end = vr->m_AimLineEnd;
			if (IsFiniteVector(start) && IsFiniteVector(end) && !(end - start).IsZero())
			{
				origin = start;
				return BuildAnglesToTarget(origin, end, angles);
			}
		}

		return false;
	}

	static inline bool ApplyLocalViewmodelBulletVisualPose(VR* vr, Vector& origin, QAngle& angles)
	{
		if (!vr || !vr->m_IsVREnabled || (!vr->m_BulletVisualsUseMuzzleSmoke && !vr->m_BulletVisualsUseViewmodelPose))
			return false;
		if ((!vr->m_VrHandsRightUseViewmodelPose && !vr->IsVrHandsTwoHandedGripPoseActive()) || vr->IsScopeActive())
			return false;

		const QAngle oldAngles = angles;
		const Vector oldOrigin = origin;
		const char* source = "viewmodel-root";
		const char* muzzleFailReason = "disabled";
		uint32_t muzzleFailAgeMs = 0;
		uint32_t muzzleFailSeq = 0;

		if (vr->m_BulletVisualsUseMuzzleSmoke)
		{
			muzzleFailReason = "no-cache";
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				const uint32_t s1 = vr->m_ViewmodelMuzzleSmokePoseSeq.load(std::memory_order_acquire);
				muzzleFailSeq = s1;
				if (s1 == 0)
					break;
				if (s1 & 1u)
				{
					muzzleFailReason = "writing";
					continue;
				}

				const Vector muzzleOrigin(
					vr->m_ViewmodelMuzzleSmokePosX.load(std::memory_order_relaxed),
					vr->m_ViewmodelMuzzleSmokePosY.load(std::memory_order_relaxed),
					vr->m_ViewmodelMuzzleSmokePosZ.load(std::memory_order_relaxed));
				QAngle muzzleAngles(
					vr->m_ViewmodelMuzzleSmokeAngX.load(std::memory_order_relaxed),
					vr->m_ViewmodelMuzzleSmokeAngY.load(std::memory_order_relaxed),
					vr->m_ViewmodelMuzzleSmokeAngZ.load(std::memory_order_relaxed));
				const uint32_t tick = vr->m_ViewmodelMuzzleSmokePoseTickMs.load(std::memory_order_relaxed);
				const uint32_t renderSeq = vr->m_ViewmodelMuzzleSmokeRenderFrameSeq.load(std::memory_order_relaxed);

				const uint32_t s2 = vr->m_ViewmodelMuzzleSmokePoseSeq.load(std::memory_order_acquire);
				if (s1 != s2 || (s2 & 1u))
				{
					muzzleFailReason = "torn";
					continue;
				}

				const uint32_t ageMs = GetTickCount() - tick;
				muzzleFailAgeMs = ageMs;
				if (ageMs > 250u)
				{
					muzzleFailReason = "stale";
					break;
				}
				if (!IsFiniteVector(muzzleOrigin) ||
					!std::isfinite(muzzleAngles.x) ||
					!std::isfinite(muzzleAngles.y) ||
					!std::isfinite(muzzleAngles.z))
				{
					muzzleFailReason = "invalid";
					break;
				}

				NormalizeAndClampViewAngles(muzzleAngles);
				origin = muzzleOrigin;
				angles = muzzleAngles;
				source = "muzzlesmoke";

				if (vr->m_NonVRServerMovementEffectsDebugLog || vr->m_BulletVisualsUseMuzzleSmoke)
				{
					static thread_local std::chrono::steady_clock::time_point s_lastMuzzleFx{};
					const float logHz = vr->m_NonVRServerMovementEffectsDebugLog ? vr->m_NonVRServerMovementEffectsDebugLogHz : 2.0f;
					if (!ShouldThrottleLog(s_lastMuzzleFx, logHz))
					{
						const uint32_t tid = static_cast<uint32_t>(GetCurrentThreadId());
						Game::logMsg(
							"[VR][FX][bullets][muzzlesmoke] tid=%u seq=%u renderSeq=%u ageMs=%u oldOrigin=(%.2f %.2f %.2f) muzzleOrigin=(%.2f %.2f %.2f) oldAngles=(%.2f %.2f %.2f) muzzleAngles=(%.2f %.2f %.2f)",
							tid,
							s2,
							renderSeq,
							ageMs,
							oldOrigin.x, oldOrigin.y, oldOrigin.z,
							origin.x, origin.y, origin.z,
							oldAngles.x, oldAngles.y, oldAngles.z,
							angles.x, angles.y, angles.z);
					}
				}
		return true;
	}
		}

		if (!vr->m_BulletVisualsUseViewmodelPose)
			return false;

		const Vector viewmodelOrigin = vr->GetRecommendedViewmodelAbsPos();
		if (!IsFiniteVector(viewmodelOrigin))
			return false;

		QAngle viewmodelVisualAngles = vr->GetRecommendedViewmodelAbsAngle();
		NormalizeAndClampViewAngles(viewmodelVisualAngles);

		Vector viewmodelForward{};
		QAngle::AngleVectors(viewmodelVisualAngles, &viewmodelForward, nullptr, nullptr);
		if (viewmodelForward.IsZero())
			return false;

		origin = viewmodelOrigin;
		angles = viewmodelVisualAngles;

		if (vr->m_NonVRServerMovementEffectsDebugLog || vr->m_BulletVisualsUseMuzzleSmoke)
		{
			static thread_local std::chrono::steady_clock::time_point s_lastVmBulletFx{};
			const float logHz = vr->m_NonVRServerMovementEffectsDebugLog ? vr->m_NonVRServerMovementEffectsDebugLogHz : 2.0f;
			if (!ShouldThrottleLog(s_lastVmBulletFx, logHz))
			{
				const uint32_t tid = static_cast<uint32_t>(GetCurrentThreadId());
				Game::logMsg(
					"[VR][FX][bullets][viewmodel] tid=%u source=%s muzzleFail=%s muzzleSeq=%u muzzleAgeMs=%u oldOrigin=(%.2f %.2f %.2f) vmOrigin=(%.2f %.2f %.2f) oldAngles=(%.2f %.2f %.2f) newAngles=(%.2f %.2f %.2f)",
					tid,
					source,
					muzzleFailReason,
					muzzleFailSeq,
					muzzleFailAgeMs,
					oldOrigin.x, oldOrigin.y, oldOrigin.z,
					origin.x, origin.y, origin.z,
					oldAngles.x, oldAngles.y, oldAngles.z,
					viewmodelVisualAngles.x, viewmodelVisualAngles.y, viewmodelVisualAngles.z);
			}
		}
		return true;
	}
}

void __fastcall Hooks::dUpdateFlashlight(void* ecx, void* edx, int entIndex, const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, float fov, float farZ, float linearAtten, bool castsShadows, const char* textureName)
{
	Vector hmdOrigin{}, hmdForward{}, hmdRight{}, hmdUp{};
	if (BuildHmdFlashlightPose(entIndex, textureName, hmdOrigin, hmdForward, hmdRight, hmdUp))
	{
		hkUpdateFlashlight.fOriginal(ecx, entIndex, hmdOrigin, hmdForward, hmdRight, hmdUp, fov, farZ, linearAtten, castsShadows, textureName);
		return;
	}

	hkUpdateFlashlight.fOriginal(ecx, entIndex, origin, forward, right, up, fov, farZ, linearAtten, castsShadows, textureName);
}

void __fastcall Hooks::dUpdateFlashlightColor(void* ecx, void* edx, int entIndex, const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, int color, bool castsShadows, int textureId, const Vector& colorVector, bool something)
{
	Vector hmdOrigin{}, hmdForward{}, hmdRight{}, hmdUp{};
	if (BuildHmdFlashlightPose(entIndex, nullptr, hmdOrigin, hmdForward, hmdRight, hmdUp))
	{
		hkUpdateFlashlightColor.fOriginal(ecx, entIndex, hmdOrigin, hmdForward, hmdRight, hmdUp, color, castsShadows, textureId, colorVector, something);
		return;
	}

	hkUpdateFlashlightColor.fOriginal(ecx, entIndex, origin, forward, right, up, color, castsShadows, textureId, colorVector, something);
}

void __fastcall Hooks::dUpdateLaserSight(void* ecx, void* edx)
{
	const bool hasVr = (m_VR != nullptr);
	const bool vrEnabled = hasVr && m_VR->m_IsVREnabled;
	const bool suppressGameLaserSightBeam = SuppressGameLaserSightBeamForD3DAimLine(m_VR);
	const bool replacementEnabled = vrEnabled
		&& !suppressGameLaserSightBeam
		&& m_VR->m_GameLaserSightBeamEnabled
		&& m_VR->m_GameLaserSightReplaceParticle;

	bool isLocalPlayer = false;
	bool skipOriginalForLocalPlayer = false;
	if ((replacementEnabled || suppressGameLaserSightBeam) && m_Game && m_Game->m_EngineClient)
	{
		const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
		C_BaseEntity* localEntity = m_Game->GetClientEntity(localPlayerIndex);
		isLocalPlayer = (localEntity != nullptr) && (reinterpret_cast<void*>(localEntity) == ecx);
		skipOriginalForLocalPlayer = replacementEnabled && isLocalPlayer;
	}

	if (suppressGameLaserSightBeam)
	{
		if (isLocalPlayer)
			ClearOriginalLocalLaserParticles(ecx);
		else
			hkUpdateLaserSight.fOriginal(ecx);

		ClearLocalWorldLaserParticle();
		return;
	}

	if (!replacementEnabled)
	{
		hkUpdateLaserSight.fOriginal(ecx);
		ClearLocalWorldLaserParticle();
		return;
	}

	if (!skipOriginalForLocalPlayer)
		hkUpdateLaserSight.fOriginal(ecx);
	else
		ClearOriginalLocalLaserParticles(ecx);

	// Keep the helper behavior identical to the original code path:
	// it silently ignores non-local callers instead of clearing the local replacement particle.
	UpdateLocalWorldLaserParticle(ecx);
}

int Hooks::dServerFireTerrorBullets(int playerId, const Vector& vecOrigin, const QAngle& vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// Server host
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		const bool scopeActive = m_VR->IsScopeActive();
		vecNewOrigin = m_VR->m_MouseModeEnabled ? GetMouseModeGunOriginAbs(m_VR) : m_VR->GetRightControllerViewmodelAbsPos();
		if (!scopeActive)
			TryBuildLocalAimLineBulletVisualRay(m_VR, vecNewOrigin, vecNewAngles);

		// ForceNonVRServerMovement: aim the *visual* bullet line to the solved hit point (H)
		// so what you see matches what the remote non-VR server will hit.
		if (m_VR->m_ForceNonVRServerMovement && m_VR->m_HasNonVRAimSolution)
		{
			Vector to = m_VR->m_NonVRAimHitPoint - vecNewOrigin;
			if (!to.IsZero())
			{
				VectorNormalize(to);
				QAngle ang;
				QAngle::VectorAngles(to, ang);
				NormalizeAndClampViewAngles(ang);
				vecNewAngles = ang;
			}
			else
			{
				vecNewAngles = m_VR->m_NonVRAimAngles;
			}
		}
		else if (scopeActive)
		{
			vecNewOrigin = m_VR->GetScopeCameraAbsPos();
			vecNewAngles = m_VR->GetScopeCameraAbsAngle();
		}
		else if (BuildAnglesToAimLineTarget(m_VR, vecNewOrigin, vecNewAngles))
		{
			// Aim-line target already includes the viewmodel-layer equivalent ray.
		}
		// Third-person convergence
		else if (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
		{
			Vector to = m_VR->m_AimConvergePoint - vecNewOrigin;
			if (!to.IsZero())
			{
				VectorNormalize(to);
				QAngle ang;
				QAngle::VectorAngles(to, ang);
				NormalizeAndClampViewAngles(ang);
				vecNewAngles = ang;
			}
			else
			{
				vecNewAngles = m_VR->m_MouseModeEnabled ? GetMouseModeFallbackAimAngles(m_VR) : m_VR->GetRightControllerAbsAngle();
			}
		}
		else if (m_VR->m_MouseModeEnabled)
		{
			const Vector target = (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
				? m_VR->m_AimConvergePoint
				: GetMouseModeDefaultTargetAbs(m_VR);

			QAngle ang;
			if (GetMouseModeAimAnglesToTarget(m_VR, vecNewOrigin, target, ang))
				vecNewAngles = ang;
			else
				vecNewAngles = GetMouseModeFallbackAimAngles(m_VR);
		}
		else
		{
			vecNewAngles = m_VR->GetRightControllerAbsAngle();
		}
	}
	// Clients
	else if (m_Game->IsValidPlayerIndex(playerId) && m_Game->m_PlayersVRInfo[playerId].isUsingVR)
	{
		vecNewOrigin = m_Game->m_PlayersVRInfo[playerId].controllerPos;
		vecNewAngles = m_Game->m_PlayersVRInfo[playerId].controllerAngle;
	}

	return hkServerFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}

int Hooks::dClientFireTerrorBullets(
	int playerId,
	const Vector& vecOrigin,
	const QAngle& vecAngles,
	int a4, int a5, int a6,
	float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// 只改本地玩家的“本地预测/表现”
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		// If looking through scope: bullets originate from scope camera and go through its center
		const bool scopeActive = m_VR->IsScopeActive();
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		// In mat_queue_mode!=0, client-side bullet FX can lag behind the rendered gun/aim line if they
		// read main-thread poses. Treat this hook as visual-only and allow it to consume the render-frame snapshot.
		struct RenderSnapshotTLSGuard
		{
			bool enabled = false;
			bool prev = false;
			RenderSnapshotTLSGuard(bool en)
			{
				enabled = en;
				if (enabled)
				{
					prev = VR::t_UseRenderFrameSnapshot;
					VR::t_UseRenderFrameSnapshot = true;
				}
			}
			~RenderSnapshotTLSGuard()
			{
				if (enabled)
					VR::t_UseRenderFrameSnapshot = prev;
			}
		} tlsGuard(queueMode == 2);


		if (!m_VR->m_ForceNonVRServerMovement)
		{
			// VR-aware server：默认起点/方向都跟控制器；鼠标模式改用鼠标枪口锚点 + 目标方向
			if (scopeActive)
			{
				vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				vecNewAngles = m_VR->GetScopeCameraAbsAngle();
			}
			else
			{
				if (m_VR->m_MouseModeEnabled)
				{
					vecNewOrigin = GetMouseModeGunOriginAbs(m_VR);

					if (!BuildAnglesToAimLineTarget(m_VR, vecNewOrigin, vecNewAngles))
					{
						const Vector target = (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
							? m_VR->m_AimConvergePoint
							: GetMouseModeDefaultTargetAbs(m_VR);

						QAngle ang;
						if (GetMouseModeAimAnglesToTarget(m_VR, vecNewOrigin, target, ang))
							vecNewAngles = ang;
						else
							vecNewAngles = GetMouseModeFallbackAimAngles(m_VR);
					}
				}
				else
				{
					vecNewOrigin = m_VR->GetRightControllerViewmodelAbsPos();

					if (TryBuildLocalAimLineBulletVisualRay(m_VR, vecNewOrigin, vecNewAngles))
					{
						// Local bullet FX ray is the already-projected viewmodel aim line.
					}
					else if (BuildAnglesToAimLineTarget(m_VR, vecNewOrigin, vecNewAngles))
					{
						// Aim-line target already includes the viewmodel-layer equivalent ray.
					}
					else if (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
					{
						Vector to = m_VR->m_AimConvergePoint - vecNewOrigin;
						if (!to.IsZero())
						{
							VectorNormalize(to);
							QAngle ang;
							QAngle::VectorAngles(to, ang);
							NormalizeAndClampViewAngles(ang);
							vecNewAngles = ang;
						}
						else
						{
							vecNewAngles = m_VR->GetRightControllerAbsAngle();
						}
					}
					else
					{
						vecNewAngles = m_VR->GetRightControllerAbsAngle();
					}
				}
			}
		}
		else
		{
			// Non-VR server：服务器判定仍以 eye(vecOrigin)+cmd->viewangles 为准（我们不会改服务器判定）。
			// 但客户端的弹道线/枪口火焰/烟雾等特效可以选择从控制器枪口发出（纯视觉）。
			if (m_VR->m_NonVRServerMovementEffectsFromController)
			{
				if (scopeActive)
				{
					vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				}
				else
				{
					vecNewOrigin = m_VR->m_MouseModeEnabled ? GetMouseModeGunOriginAbs(m_VR) : m_VR->GetRightControllerViewmodelAbsPos();
					if (!m_VR->m_MouseModeEnabled)
						TryBuildLocalAimLineBulletVisualRay(m_VR, vecNewOrigin, vecNewAngles);
				}

				// 开关决定：要不要把 angles 替换成“控制器纯角度/汇聚角度”
				// - true  : 覆盖 angles（通常会让本地弹道更“直/更跟手”，但只是本地表现）
				// - false : 保持 vecAngles（通常包含引擎/服务器那套散布偏转 → 看起来更“标准散布”）
				if (m_VR->m_NonVRServerMovementAngleOverride)
				{
					// Prefer the solved eye-based aim. This keeps client prediction + hit feedback
					// consistent with what the non-VR server will do.
					if (m_VR->m_HasNonVRAimSolution)
					{
						vecNewAngles = m_VR->m_NonVRAimAngles;
					}
					else
					{
						if (!scopeActive && m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
						{
							Vector to = m_VR->m_AimConvergePoint - vecNewOrigin; // 注意：这里是 vecOrigin
							if (!to.IsZero())
							{
								VectorNormalize(to);
								QAngle ang;
								QAngle::VectorAngles(to, ang);
								NormalizeAndClampViewAngles(ang);
								vecNewAngles = ang;
							}
							else
							{
								vecNewAngles = m_VR->GetRightControllerAbsAngle();
							}
						}
						else
						{
							vecNewAngles = m_VR->GetRightControllerAbsAngle();
						}
					}
				}
				// else：不动 vecNewAngles = vecAngles（保留“标准散布”的那套）
			}
		}

		// Final bullet FX alignment: apply hit-point offset AFTER all angle overrides.
		// This is intentionally visual-only (client FX). It does not affect server hit registration.
		if (m_VR->m_IsVREnabled
			&& m_VR->m_ForceNonVRServerMovement
			&& m_VR->m_HasNonVRAimSolution)
		{
			const int qmode = (m_Game ? m_Game->GetMatQueueMode() : 0);
			Vector visualOff = m_VR->m_BulletVisualHitOffset;
			if (qmode != 0)
			{
				visualOff.x += m_VR->m_QueuedBulletVisualHitOffset.x;
				visualOff.y += m_VR->m_QueuedBulletVisualHitOffset.y;
				visualOff.z += m_VR->m_QueuedBulletVisualHitOffset.z;
			}

			if (!visualOff.IsZero())
			{
				Vector originForDir = vecNewOrigin;
				Vector targetH = m_VR->m_NonVRAimHitPoint;

				Vector fwd = targetH - originForDir;
				if (!fwd.IsZero())
				{
					VectorNormalize(fwd);
					Vector worldUp(0.0f, 0.0f, 1.0f);
					Vector right;
					CrossProduct(worldUp, fwd, right);
					if (right.LengthSqr() < 1e-6f)
					{
						worldUp = Vector(0.0f, 1.0f, 0.0f);
						CrossProduct(worldUp, fwd, right);
					}
					VectorNormalize(right);
					Vector up;
					CrossProduct(fwd, right, up);
					VectorNormalize(up);

					const float su = m_VR->m_VRScale;
					const Vector offSu = visualOff * su;
					targetH += (fwd * offSu.x) + (right * offSu.y) + (up * offSu.z);

					Vector to = targetH - originForDir;
					if (!to.IsZero())
					{
						VectorNormalize(to);
						QAngle ang;
						QAngle::VectorAngles(to, ang);
						NormalizeAndClampViewAngles(ang);
						vecNewAngles = ang;

						if (m_VR->m_NonVRServerMovementEffectsDebugLog)
						{
							static thread_local std::chrono::steady_clock::time_point s_lastOff{};
							if (!ShouldThrottleLog(s_lastOff, m_VR->m_NonVRServerMovementEffectsDebugLogHz))
							{
								const uint32_t tid = (uint32_t)GetCurrentThreadId();
								const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
								Game::logMsg(
									"[VR][FX][bullets][offset] tid=%u qmode=%d seq=%u offTotal=(%.3f %.3f %.3f)m base=(%.3f %.3f %.3f)m qExtra=(%.3f %.3f %.3f)m origin=(%.2f %.2f %.2f) H=(%.2f %.2f %.2f)",
									tid, qmode, seq,
									visualOff.x, visualOff.y, visualOff.z,
									m_VR->m_BulletVisualHitOffset.x, m_VR->m_BulletVisualHitOffset.y, m_VR->m_BulletVisualHitOffset.z,
									m_VR->m_QueuedBulletVisualHitOffset.x, m_VR->m_QueuedBulletVisualHitOffset.y, m_VR->m_QueuedBulletVisualHitOffset.z,
									originForDir.x, originForDir.y, originForDir.z,
									targetH.x, targetH.y, targetH.z);
							}
						}
					}
				}
			}
		}



		const bool viewmodelBulletPoseApplied = ApplyLocalViewmodelBulletVisualPose(m_VR, vecNewOrigin, vecNewAngles);
		if (viewmodelBulletPoseApplied && !scopeActive && m_VR->m_HasAimLine && !m_VR->m_HasThrowArc)
			BuildAnglesToTarget(vecNewOrigin, m_VR->m_AimLineEnd, vecNewAngles);
		C_BasePlayer* localPlayerForSpread = (m_Game != nullptr) ? (C_BasePlayer*)m_Game->GetClientEntity(playerId) : nullptr;
		C_WeaponCSBase* activeWeaponForSpread = localPlayerForSpread ? (C_WeaponCSBase*)localPlayerForSpread->GetActiveWeapon() : nullptr;
		m_VR->NotifyVrHandsRealBulletSpreadClientShot(localPlayerForSpread, activeWeaponForSpread, vecNewOrigin, vecNewAngles, a7);
		const Vector predictedHitOrigin = vecNewOrigin;
		const QAngle predictedHitAngles = vecNewAngles;

		if (m_VR->m_IsVREnabled && m_Game && m_Game->m_EngineClient
			&& playerId == m_Game->m_EngineClient->GetLocalPlayer())
		{
			int weaponId = (int)C_WeaponCSBase::WeaponID::NONE;
			C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerId);
			if (localPlayer)
			{
				C_WeaponCSBase* activeWeapon = (C_WeaponCSBase*)localPlayer->GetActiveWeapon();
				if (activeWeapon)
					weaponId = (int)activeWeapon->GetWeaponID();
			}
			m_VR->TriggerWeaponFireHaptics(weaponId, false);
		}

		// RightAmmoHUD: hit-based target HP bar has been removed.
	// The ammo HUD now shows HP%% for the *aimed* special infected (and Witch) and hides instantly on leave.
		if (m_VR->m_IsVREnabled && m_Game && m_Game->m_EngineClient
			&& playerId == m_Game->m_EngineClient->GetLocalPlayer())
		{
			// Start a fresh shot window once per FireTerrorBullets call.
			// RegisterPotentialKillSoundHit keeps a VR-corrected impact candidate for
			// later hurt/death events and also serves as the fallback path for
			// common-infected hit feedback when local hurt events are unavailable.
			m_VR->BeginPredictedHitFeedbackShot(m_VR->m_CurrentPredictedHitFeedbackCmdNumber);
			// Use the final VR-corrected shot ray for predicted hit feedback so the
			// hit sound / hit indicator matches the actual VR muzzle origin and aim.
			m_VR->RegisterPotentialKillSoundHit(predictedHitOrigin, predictedHitAngles);
		}

		const auto original = hkClientFireTerrorBullets.fOriginal;
		if (!original)
			return 0;

		return original(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
	}

	const auto original = hkClientFireTerrorBullets.fOriginal;
	if (!original)
		return 0;
	return original(playerId, vecOrigin, vecAngles, a4, a5, a6, a7);
}


// === 用下面这整个函数替换你当前的 Hooks::dProcessUsercmds ===
void __fastcall Hooks::dServerGameClientsClientCommand(
	void* ecx,
	void* edx,
	edict_t* player,
	const void* sourceCommand)
{
	(void)edx;
	if (m_Game &&
		m_Game->HandleBuiltinVRPoseRelayCommand(
			player,
			sourceCommand))
	{
		// Consume only the private pose commands. Every ordinary Source
		// command continues through the original game-DLL handler.
		return;
	}

	const auto original =
		hkServerGameClientsClientCommand.fOriginal;
	if (original)
		original(ecx, player, sourceCommand);
}

float __fastcall Hooks::dProcessUsercmds(void* ecx, void* edx, edict_t* player,
	void* buf, int numcmds, int totalcmds,
	int dropped_packets, bool ignore, bool paused)
{
	// ★ 进入该钩子，说明本进程正在跑“服务器”逻辑（listen/dedicated）
	m_ServerCommandControllerAimOverride = false;
	m_ServerCommandControllerAimPlayer = nullptr;
	m_ServerCommandControllerAimReason = 0;
	m_ServerPacketSawVRUsercmd = false;

	Hooks::s_ServerUnderstandsVR = true;

	// Function pointer for CBaseEntity::entindex
	typedef int(__thiscall* tEntindex)(void* thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

	IServerUnknown* pUnknown = player->m_pUnk;
	Server_BaseEntity* pPlayer = (Server_BaseEntity*)pUnknown->GetBaseEntity();
	m_Game->m_CurrentUsercmdPlayer = pPlayer;
	m_Game->m_CurrentUsercmdEdict = player;

	int index = oEntindex(pPlayer);
	m_Game->m_CurrentUsercmdID = index;

	const bool preOriginalHasValidPlayer = m_Game->IsValidPlayerIndex(index);
	if (m_VR && preOriginalHasValidPlayer)
	{
		const auto now = std::chrono::steady_clock::now();
		const bool serverUseAimActive =
			m_VR->m_ServerUseControllerAimActive ||
			(m_VR->m_ServerUseControllerAimUntil.time_since_epoch().count() != 0 &&
				now <= m_VR->m_ServerUseControllerAimUntil);
		if (serverUseAimActive)
		{
			static std::chrono::steady_clock::time_point s_lastServerUseVtableLog{};
			if (s_lastServerUseVtableLog.time_since_epoch().count() == 0 ||
				std::chrono::duration<float>(now - s_lastServerUseVtableLog).count() >= 0.50f)
			{
				s_lastServerUseVtableLog = now;
				uintptr_t* vtable = nullptr;
				uintptr_t eyePositionSlot = 0;
				uintptr_t eyeAnglesSlot = 0;
				uintptr_t playerUseSlot = 0;
				uintptr_t slot6d8 = 0;
				uintptr_t findUseSlot = 0;
				uintptr_t isUseableSlot = 0;
#ifdef _MSC_VER
				__try
				{
					vtable = *reinterpret_cast<uintptr_t**>(pPlayer);
					if (vtable)
					{
						eyePositionSlot = vtable[0x230 / sizeof(uintptr_t)];
						eyeAnglesSlot = vtable[0x234 / sizeof(uintptr_t)];
						playerUseSlot = vtable[0x6D4 / sizeof(uintptr_t)];
						slot6d8 = vtable[0x6D8 / sizeof(uintptr_t)];
						findUseSlot = vtable[0x6DC / sizeof(uintptr_t)];
						isUseableSlot = vtable[0x6E0 / sizeof(uintptr_t)];
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					vtable = nullptr;
					eyePositionSlot = 0;
					eyeAnglesSlot = 0;
					playerUseSlot = 0;
					slot6d8 = 0;
					findUseSlot = 0;
					isUseableSlot = 0;
				}
#else
				vtable = *reinterpret_cast<uintptr_t**>(pPlayer);
				if (vtable)
				{
					eyePositionSlot = vtable[0x230 / sizeof(uintptr_t)];
					eyeAnglesSlot = vtable[0x234 / sizeof(uintptr_t)];
					playerUseSlot = vtable[0x6D4 / sizeof(uintptr_t)];
					slot6d8 = vtable[0x6D8 / sizeof(uintptr_t)];
					findUseSlot = vtable[0x6DC / sizeof(uintptr_t)];
					isUseableSlot = vtable[0x6E0 / sizeof(uintptr_t)];
				}
#endif
				Game::logMsg(
					"[VR][UseAim] server player vslots player=%p vtbl=%p eyePos=%p eyeAng=%p playerUse=%p slot6d8=%p findUse=%p isUseable=%p usercmd=%d isVR=%d hasAim=%d",
					pPlayer,
					vtable,
					reinterpret_cast<void*>(eyePositionSlot),
					reinterpret_cast<void*>(eyeAnglesSlot),
					reinterpret_cast<void*>(playerUseSlot),
					reinterpret_cast<void*>(slot6d8),
					reinterpret_cast<void*>(findUseSlot),
					reinterpret_cast<void*>(isUseableSlot),
					index,
					m_Game->m_PlayersVRInfo[index].isUsingVR ? 1 : 0,
					m_VR->m_HasNonVRAimSolution ? 1 : 0);
			}
		}
	}

	ApplyServerTeleportMove(pPlayer, pUnknown, index);
	ApplyServerRoomscale1To1Move(pPlayer, pUnknown, index);

	m_ServerProcessingUsercmd = true;
	m_ServerProcessingUsercmdPlayer = pPlayer;
	m_ServerProcessingUsercmdPlayerIndex = index;
	float result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);
	if (m_ServerPacketSawVRUsercmd &&
		m_Game->IsValidPlayerIndex(index))
	{
		m_Game->ObserveBuiltinVRPoseRelayClient(
			index,
			player);
	}
	m_ServerPacketSawVRUsercmd = false;
	ObjectPullUpdateServer(index, pPlayer);
	// Custom inventory throws suppress stock IN_ATTACK/IN_USE, then invoke the
	// game's generic Weapon_Drop path here.
	const bool inventoryDropExecuted =
		ManualInventoryThrowExecutePendingDrop(index, pPlayer);
	Server_WeaponCSBase* activeWeaponAfterUsercmd = nullptr;
	bool activeWeaponReadSucceeded = false;
	if (pPlayer)
	{
		typedef Server_WeaponCSBase* (__thiscall* tGetActiveWeaponAfterUsercmd)(void* thisptr);
		static tGetActiveWeaponAfterUsercmd getActiveWeaponAfterUsercmd =
			reinterpret_cast<tGetActiveWeaponAfterUsercmd>(m_Game->m_Offsets->GetActiveWeapon.address);
#ifdef _MSC_VER
		__try
		{
			activeWeaponAfterUsercmd = getActiveWeaponAfterUsercmd(pPlayer);
			activeWeaponReadSucceeded = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			activeWeaponAfterUsercmd = nullptr;
		}
#else
		activeWeaponAfterUsercmd = getActiveWeaponAfterUsercmd(pPlayer);
		activeWeaponReadSucceeded = true;
#endif
	}
	const bool detachedThrowApplied = ManualCarryThrowApplyPendingAfterWeaponDetached(
		index,
		pPlayer,
		activeWeaponAfterUsercmd,
		activeWeaponReadSucceeded);
	ManualEmptyHandsPlaceholderUpdate(
		index,
		pPlayer,
		inventoryDropExecuted || detachedThrowApplied);
	ManualCarryImpactUpdate();
	m_ServerProcessingUsercmd = false;
	m_ServerProcessingUsercmdPlayer = nullptr;
	m_ServerProcessingUsercmdPlayerIndex = -1;

	m_ServerCommandControllerAimOverride = false;
	m_ServerCommandControllerAimPlayer = nullptr;
	m_ServerCommandControllerAimReason = 0;

	// A blocked direct roomscale move is injected into one decoded server CUserCmd so
	// Source's normal player movement can run StepMove. Reconcile the resulting planar
	// displacement after ProcessUsercmds returns, then correct the VR camera only by the
	// difference between accepted movement and the physical HMD movement.
	FinalizePendingRoomscaleServerCmdFallback(pPlayer, index);

	// ===== 你原有的“近战挥砍检测/追踪”逻辑，保持不变 =====
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(index);

	if (hasValidPlayer && m_Game->m_PlayersVRInfo[index].isUsingVR && m_Game->m_PlayersVRInfo[index].isMeleeing)
	{
		typedef Server_WeaponCSBase* (__thiscall* tGetActiveWep)(void* thisptr);
		static tGetActiveWep oGetActiveWep = (tGetActiveWep)(m_Game->m_Offsets->GetActiveWeapon.address);
		Server_WeaponCSBase* curWep = oGetActiveWep(pPlayer);

		if (curWep)
		{
			int wepID = curWep->GetWeaponID();
			if (wepID == 19) // melee weapon
			{
				if (m_Game->m_PlayersVRInfo[index].isNewSwing)
				{
					m_Game->m_PlayersVRInfo[index].isNewSwing = false;
					curWep->entitiesHitThisSwing = 0;
				}

				typedef void* (__thiscall* tGetMeleeWepInfo)(void* thisptr);
				static tGetMeleeWepInfo oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfo.address);
				void* meleeWepInfo = oGetMeleeWepInfo(curWep);

				Vector initialForward, initialRight, initialUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].prevControllerAngle, &initialForward, &initialRight, &initialUp);
				Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0f);
				VectorNormalize(initialMeleeDirection);

				Vector finalForward, finalRight, finalUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].controllerAngle, &finalForward, &finalRight, &finalUp);
				Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0f);
				VectorNormalize(finalMeleeDirection);

				const float swingDot = std::clamp(DotProduct(initialMeleeDirection, finalMeleeDirection), -1.0f, 1.0f);
				Vector pivot;
				CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
				bool canTraceSwing = true;
				if (VectorNormalize(pivot) <= 0.0001f)
				{
					if (swingDot > -0.999f)
					{
						canTraceSwing = false;
					}
					else
					{
						pivot = initialUp;
						canTraceSwing = VectorNormalize(pivot) > 0.0001f;
					}
				}

				float swingAngle = acosf(swingDot) * 180.0f / 3.14159265f;
				if (!std::isfinite(swingAngle) || swingAngle <= 0.01f)
					canTraceSwing = false;

				if (canTraceSwing)
				{
					m_Game->m_Hooks->hkGetPrimaryAttackActivity.fOriginal(curWep, meleeWepInfo); // Needed to call TestMeleeSwingCollision

					m_Game->m_PerformingMelee = true;

					Vector traceDirection = initialMeleeDirection;
					int numTraces = 10;
					float traceAngle = swingAngle / numTraces;
					bool confirmedMeleeCollision = false;
					for (int i = 0; i < numTraces; ++i)
					{
						traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
						const int entitiesHitBefore = curWep->entitiesHitThisSwing;
						const int collisionResult = m_Game->m_Hooks->hkTestMeleeSwingCollisionServer.fOriginal(curWep, traceDirection);
						const int entitiesHitAfter = curWep->entitiesHitThisSwing;
						if (collisionResult != 0 || entitiesHitAfter > entitiesHitBefore)
							confirmedMeleeCollision = true;
					}

					m_Game->m_PerformingMelee = false;

					if (confirmedMeleeCollision && m_VR && index == m_Game->m_EngineClient->GetLocalPlayer())
						m_VR->NotifyMeleeHitConfirmed(0);
				}
			}
		}
	}
	else if (hasValidPlayer)
	{
		m_Game->m_PlayersVRInfo[index].isNewSwing = true;
	}

	if (hasValidPlayer)
	{
		m_Game->m_PlayersVRInfo[index].prevControllerAngle = m_Game->m_PlayersVRInfo[index].controllerAngle;
	}

	if (m_VR && hasValidPlayer && m_Game->m_EngineClient && m_Game->m_Offsets->GetActiveWeapon.address)
	{
		const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
		if (localPlayerIndex > 0 && index == localPlayerIndex)
		{
			typedef Server_WeaponCSBase* (__thiscall* tGetActiveWep)(void* thisptr);
			static tGetActiveWep oGetActiveWepForMagazineInteraction =
				(tGetActiveWep)(m_Game->m_Offsets->GetActiveWeapon.address);
			Server_WeaponCSBase* curWep = oGetActiveWepForMagazineInteraction ? oGetActiveWepForMagazineInteraction(pPlayer) : nullptr;
			if (curWep)
			{
				const int weaponId = curWep->GetWeaponID();
				m_VR->MarkMagazineInteractionServerHookSeen(weaponId);
				if (MagazineInteractionWeaponIdIsShotgun(weaponId))
					m_VR->TryApplyMagazineInteractionShotgunServerReloadAbort(curWep, weaponId, pPlayer);
				else
					m_VR->TryApplyMagazineInteractionServerClipCommit(curWep, weaponId, pPlayer);
			}
		}
	}

	return result;
}

namespace
{
	static bool ServerWeaponIdIsThrowable(int weaponId)
	{
		return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::MOLOTOV) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PIPE_BOMB) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::VOMITJAR);
	}

	static bool ServerWeaponIdIsManualCarryThrowable(int weaponId)
	{
		return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::GASCAN) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PROPANE_TANK) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::OXYGEN_TANK) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::GNOME_CHOMPSKI) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::COLA_BOTTLES) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::FIREWORKS_BOX);
	}

	static const char* ServerControllerAimReasonName(int reason)
	{
		switch (reason)
		{
		case 2: return "throw";
		case 3: return "throw-grace";
		case 4: return "mounted";
		default: return "unknown";
		}
	}

	static bool TryGetServerCurrentWeapon(Server_WeaponCSBase*& weapon, int& weaponId)
	{
		weapon = nullptr;
		weaponId = static_cast<int>(C_WeaponCSBase::WeaponID::NONE);

		if (!Hooks::m_Game ||
			!Hooks::m_Game->m_CurrentUsercmdPlayer ||
			!Hooks::m_Game->m_Offsets ||
			!Hooks::m_Game->m_Offsets->GetActiveWeapon.address)
		{
			return false;
		}

		typedef Server_WeaponCSBase* (__thiscall* tGetActiveWep)(void* thisptr);
		static tGetActiveWep oGetActiveWep = (tGetActiveWep)(Hooks::m_Game->m_Offsets->GetActiveWeapon.address);
		if (!oGetActiveWep)
			return false;

#ifdef _MSC_VER
		__try
		{
			weapon = oGetActiveWep(Hooks::m_Game->m_CurrentUsercmdPlayer);
			if (weapon)
				weaponId = weapon->GetWeaponID();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			weapon = nullptr;
			weaponId = static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
			return false;
		}
#else
		weapon = oGetActiveWep(Hooks::m_Game->m_CurrentUsercmdPlayer);
		if (weapon)
			weaponId = weapon->GetWeaponID();
#endif

		return weapon != nullptr;
	}

	static bool IsCurrentServerPlayerUsingMountedWeapon()
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_CurrentUsercmdPlayer)
			return false;

#ifdef _MSC_VER
		__try
		{
			const auto* base = reinterpret_cast<const uint8_t*>(Hooks::m_Game->m_CurrentUsercmdPlayer);
			return base[VR::kUsingMountedGunOffset] != 0 || base[VR::kUsingMountedWeaponOffset] != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		const auto* base = reinterpret_cast<const uint8_t*>(Hooks::m_Game->m_CurrentUsercmdPlayer);
		return base[VR::kUsingMountedGunOffset] != 0 || base[VR::kUsingMountedWeaponOffset] != 0;
#endif
	}
}

int Hooks::dReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from)
{
	m_ServerCommandControllerAimOverride = false;
	m_ServerCommandControllerAimPlayer = nullptr;
	m_ServerCommandControllerAimReason = 0;

	Hooks::s_ServerUnderstandsVR = true;
	hkReadUsercmd.fOriginal(buf, move, from);
	if (move && move->tick_count < 0)
		m_ServerPacketSawVRUsercmd = true;

	int i = m_Game->m_CurrentUsercmdID;
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(i);
	if (m_VR->m_EncodeVRUsercmd && move->tick_count < 0) // Signal for VR CUserCmd
	{
		move->tick_count *= -1;

		if (move->command_number < 0)
		{
			move->command_number *= -1;
			if (hasValidPlayer)
			{
				m_Game->m_PlayersVRInfo[i].isMeleeing = true;
			}
		}
		else
		{
			if (hasValidPlayer)
			{
				m_Game->m_PlayersVRInfo[i].isMeleeing = false;
			}
		}

		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].isUsingVR = true;
			m_Game->m_PlayersVRInfo[i].controllerAngle.x = (float)move->mousedx / 10;
			m_Game->m_PlayersVRInfo[i].controllerAngle.y = (float)move->mousedy / 10;
			m_Game->m_PlayersVRInfo[i].controllerPos.x = move->viewangles.z;
			m_Game->m_PlayersVRInfo[i].controllerPos.y = move->upmove;
		}

		// Decode controllerAngle.z
		int rollEncoding = move->command_number / 10000000;
		move->command_number -= rollEncoding * 10000000;
		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].controllerAngle.z = (rollEncoding * 2) - 180;
		}

		// Decode viewangles.x
		int decodedZInt = (move->viewangles.x / 10000);
		float decodedAngle = fabsf((float)(move->viewangles.x - (decodedZInt * 10000)) / 10);
		decodedAngle -= 360.0f;
		float decodedZ = (float)decodedZInt / 10.0f;

		if (hasValidPlayer)
		{
			Player& vrPlayer = m_Game->m_PlayersVRInfo[static_cast<size_t>(i)];
			vrPlayer.controllerPos.z = decodedZ;

			Vector playerRelativePosition{};
			bool hasPlayerRelativePosition =
				IsLocalServerUsercmdContext() &&
				TryGetManualThrowUsercmdPlayerRelativePosition(
					m_VR,
					move->command_number,
					vrPlayer.controllerPos,
					playerRelativePosition);
			if (!hasPlayerRelativePosition)
			{
				Vector playerOrigin{};
				if (ManualThrowGetPlayerOrigin(
					m_Game->m_CurrentUsercmdPlayer,
					playerOrigin))
				{
					playerRelativePosition =
						vrPlayer.controllerPos - playerOrigin;
					hasPlayerRelativePosition = true;
				}
			}

			ManualThrowRecordPoseSample(
				vrPlayer,
				move->tick_count,
				vrPlayer.controllerPos,
				playerRelativePosition,
				hasPlayerRelativePosition,
				vrPlayer.controllerAngle);
		}

		move->viewangles.x = decodedAngle;
		move->viewangles.z = 0;
		move->upmove = 0;

		constexpr int kIN_USE = (1 << 5);
		const uint8_t objectPullCommand = move->impulse;
		if (objectPullCommand >= VR::kObjectPullWireBegin &&
			objectPullCommand <= VR::kObjectPullWireCancel)
		{
			// weaponselect is serialized with MAX_EDICT_BITS and carries the
			// exact client/server network entity index for Object Pull. Once the
			// impulse has identified this packet as Object Pull, weaponsubtype's
			// six serialized bits safely carry the narrow map-prop model hint.
			const int objectPullEntityIndex = move->weaponselect;
			const int rawObjectPullTargetHint =
				move->weaponsubtype;
			const uint8_t objectPullTargetHint =
				rawObjectPullTargetHint >= 0 &&
				rawObjectPullTargetHint <=
					static_cast<int>(
						VR::ObjectPullTargetHint::ColaBottles)
				? static_cast<uint8_t>(
					rawObjectPullTargetHint)
				: static_cast<uint8_t>(
					VR::ObjectPullTargetHint::None);
			if (m_VR->m_ObjectPullDebugLog)
			{
				Game::logMsg(
					"[VR][ObjectPull][server] incoming wire command=%u entityIndex=%d targetHint=%u player=%d tick=%d",
					static_cast<unsigned int>(objectPullCommand),
					objectPullEntityIndex,
					static_cast<unsigned int>(objectPullTargetHint),
					i,
					move->tick_count);
			}
			move->impulse = 0;
			move->weaponselect = 0;
			move->weaponsubtype = 0;
			if (hasValidPlayer)
			{
				const bool acceptedObjectPullCommand =
					ObjectPullDecodeServerCommand(
						i,
						objectPullCommand,
						move->command_number,
						move->tick_count,
						objectPullEntityIndex,
						objectPullTargetHint);
				if (acceptedObjectPullCommand &&
					objectPullCommand ==
						VR::kObjectPullWireCatch &&
					ObjectPullPrepareNativePickupUsercmd(
						i,
						objectPullEntityIndex))
				{
					// Feed one ordinary +use edge into this exact server
					// command so the stock pickup transaction owns inventory
					// replacement and same-type ammo transfer.
					move->buttons |= kIN_USE;
				}
			}
		}

		Player* vrPlayerState = hasValidPlayer
			? &m_Game->m_PlayersVRInfo[static_cast<size_t>(i)]
			: nullptr;
		constexpr int kIN_ATTACK = (1 << 0);
		constexpr int kIN_RELOAD = (1 << 13);
		if (vrPlayerState)
		{
			ManualEmptyHandsPlaceholderPrepareForUse(
				i,
				m_Game->m_CurrentUsercmdPlayer,
				*vrPlayerState,
				(move->buttons & kIN_USE) != 0);
		}

		Server_WeaponCSBase* serverWeapon = nullptr;
		int serverWeaponId = static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
		TryGetServerCurrentWeapon(serverWeapon, serverWeaponId);
		if (m_VR && serverWeapon)
		{
			m_VR->MarkMagazineInteractionServerHookSeen(serverWeaponId);
			if (!MagazineInteractionWeaponIdIsShotgun(serverWeaponId))
				m_VR->TryApplyMagazineInteractionServerClipCommit(serverWeapon, serverWeaponId, m_Game->m_CurrentUsercmdPlayer);
		}
		const bool serverWeaponIsDummyPistol =
			vrPlayerState &&
			ManualEmptyHandsPlaceholderIsTrackedDummy(*vrPlayerState, serverWeapon);
		const uintptr_t weaponTag = reinterpret_cast<uintptr_t>(serverWeapon);
		const bool previousAttackDown = vrPlayerState && vrPlayerState->throwableAimPrevAttackDown;
		const bool previousWeaponThrowable = vrPlayerState && vrPlayerState->throwableAimPrevWeaponThrowable;
		const int previousThrowableWeaponId = vrPlayerState
			? vrPlayerState->throwableAimWeaponId
			: static_cast<int>(C_WeaponCSBase::WeaponID::NONE);

		if (vrPlayerState && weaponTag != vrPlayerState->throwableAimWeaponTag)
			vrPlayerState->throwableAimWeaponTag = weaponTag;

		constexpr uint32_t kManualCarryThrowWeaponShift = 26u;
		constexpr uint32_t kManualCarryThrowWeaponMask = (0x3Fu << kManualCarryThrowWeaponShift);
		const uint32_t encodedCarryValue =
			(static_cast<uint32_t>(move->buttons) & kManualCarryThrowWeaponMask) >>
			kManualCarryThrowWeaponShift;
		const int encodedCarryWeaponId = encodedCarryValue > 0u
			? static_cast<int>(encodedCarryValue - 1u)
			: static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
		// Remove the private client-to-listen-server marker before Source processes
		// the command as gameplay input.
		move->buttons = static_cast<int>(
			static_cast<uint32_t>(move->buttons) & ~kManualCarryThrowWeaponMask);
		if (serverWeaponIsDummyPistol)
		{
			// The hidden pistol exists only so Source's native IN_ATTACK2 shove and
			// cooldown logic stay intact. It may never shoot or reload.
			move->buttons &= ~(kIN_ATTACK | kIN_RELOAD);
		}
		const bool attackDown = (move->buttons & kIN_ATTACK) != 0;
		const bool activeWeaponIsThrowable = ServerWeaponIdIsThrowable(serverWeaponId);
		const bool activeWeaponIsCarryThrowable = ServerWeaponIdIsManualCarryThrowable(serverWeaponId);
		const bool encodedCarryRelease =
			attackDown && ServerWeaponIdIsManualCarryThrowable(encodedCarryWeaponId);
		const bool encodedInventoryRelease =
			!attackDown && ManualInventoryThrowWeaponIdRequiresCustomDrop(encodedCarryWeaponId);
		const bool releasedThrowable = previousWeaponThrowable && previousAttackDown && !attackDown;
		const int releasedWeaponId = activeWeaponIsThrowable ? serverWeaponId : previousThrowableWeaponId;
		const bool manualThrowActive = s_ManualThrowHooksReady && m_VR && m_VR->m_ManualThrowEnabled;
		const bool manualCarryThrowActive =
			ManualCarryThrowBackendIsReady(encodedCarryWeaponId) &&
			m_VR && m_VR->m_ManualThrowEnabled;
		const bool manualInventoryThrowActive =
			ManualInventoryThrowBackendIsReady(encodedCarryWeaponId) &&
			m_VR && m_VR->m_ManualThrowEnabled &&
			!serverWeaponIsDummyPistol;
		bool commandControllerAim = false;
		int commandControllerAimReason = 0;

		if (vrPlayerState && activeWeaponIsThrowable && attackDown && !previousAttackDown)
			vrPlayerState->manualThrowPending = {};
		if (vrPlayerState && !manualThrowActive)
			vrPlayerState->manualThrowPending = {};
		if (vrPlayerState && manualThrowActive && releasedThrowable)
		{
			ManualThrowPreparePending(
				*vrPlayerState,
				m_Game->m_CurrentUsercmdPlayer,
				serverWeapon,
				releasedWeaponId,
				move->tick_count,
				false);
		}
		if (vrPlayerState && manualCarryThrowActive && encodedCarryRelease &&
			vrPlayerState->manualCarryThrowLastDecodedReleaseTick != move->tick_count)
		{
			const bool sourceMatchesRelease =
				serverWeapon && activeWeaponIsCarryThrowable && serverWeaponId == encodedCarryWeaponId;
			if (sourceMatchesRelease)
			{
				vrPlayerState->manualCarryThrowLastDecodedReleaseTick = move->tick_count;
				const bool prepared = ManualThrowPreparePending(
					*vrPlayerState,
					m_Game->m_CurrentUsercmdPlayer,
					serverWeapon,
					encodedCarryWeaponId,
					move->tick_count,
					false);
				Game::logMsg(
					"[VR][ManualCarryThrow] release decoded player=%d tick=%d encodedWeaponId=%d serverWeaponId=%d source=%p sourceMatch=1 detachedApply=1 prepared=%d velocity=(%.1f %.1f %.1f)",
					i,
					move->tick_count,
					encodedCarryWeaponId,
					serverWeaponId,
					serverWeapon,
					prepared ? 1 : 0,
					vrPlayerState->manualThrowPending.velocity.x,
					vrPlayerState->manualThrowPending.velocity.y,
					vrPlayerState->manualThrowPending.velocity.z);
			}
			else
			{
				Game::logMsg(
					"[VR][ManualCarryThrow] release source ignored player=%d tick=%d encodedWeaponId=%d serverWeaponId=%d source=%p",
					i,
					move->tick_count,
					encodedCarryWeaponId,
					serverWeaponId,
					serverWeapon);
			}
		}
		if (vrPlayerState && manualInventoryThrowActive && encodedInventoryRelease &&
			vrPlayerState->manualCarryThrowLastDecodedReleaseTick != move->tick_count)
		{
			const bool sourceMatchesRelease = serverWeapon &&
				ManualInventoryThrowWeaponIdRequiresCustomDrop(serverWeaponId) &&
				serverWeaponId == encodedCarryWeaponId;
			if (sourceMatchesRelease)
			{
				vrPlayerState->manualCarryThrowLastDecodedReleaseTick = move->tick_count;
				const bool prepared = ManualThrowPreparePending(
					*vrPlayerState,
					m_Game->m_CurrentUsercmdPlayer,
					serverWeapon,
					encodedCarryWeaponId,
					move->tick_count,
					true);
				Game::logMsg(
					"[VR][ManualInventoryThrow] release decoded player=%d tick=%d weaponId=%d source=%p prepared=%d velocity=(%.1f %.1f %.1f)",
					i,
					move->tick_count,
					encodedCarryWeaponId,
					serverWeapon,
					prepared ? 1 : 0,
					vrPlayerState->manualThrowPending.velocity.x,
					vrPlayerState->manualThrowPending.velocity.y,
					vrPlayerState->manualThrowPending.velocity.z);
			}
		}

		// Projectile Create hooks already replace the spawn pose and release velocity.
		// Do not redirect server EyePosition/EyeAngles while the trigger is held: that
		// changes the weapon's prepared viewmodel pose and pulls it behind the controller.
		if (vrPlayerState)
			vrPlayerState->throwableAimTicks = 0;

		if (vrPlayerState)
		{
			vrPlayerState->throwableAimPrevAttackDown = activeWeaponIsThrowable && attackDown;
			vrPlayerState->throwableAimPrevWeaponThrowable = activeWeaponIsThrowable;
			if (activeWeaponIsThrowable)
				vrPlayerState->throwableAimWeaponId = serverWeaponId;
		}

		if (IsCurrentServerPlayerUsingMountedWeapon())
		{
			commandControllerAim = true;
			commandControllerAimReason = 4;
		}

		if (commandControllerAim)
		{
			const Player& vrPlayer = m_Game->m_PlayersVRInfo[i];
			m_ServerCommandControllerAimOverride = true;
			m_ServerCommandControllerAimPlayer = m_Game->m_CurrentUsercmdPlayer;
			m_ServerCommandControllerAimOrigin = vrPlayer.controllerPos;
			m_ServerCommandControllerAimAngles = vrPlayer.controllerAngle;
			NormalizeAndClampViewAngles(m_ServerCommandControllerAimAngles);
			m_ServerCommandControllerAimReason = commandControllerAimReason;

			static std::chrono::steady_clock::time_point s_lastServerCommandAimLog{};
			const auto now = std::chrono::steady_clock::now();
			if (s_lastServerCommandAimLog.time_since_epoch().count() == 0 ||
				std::chrono::duration<float>(now - s_lastServerCommandAimLog).count() >= 0.50f)
			{
				s_lastServerCommandAimLog = now;
				Game::logMsg(
					"[VR][UseAim] server command controller eye override reason=%s weaponId=%d buttons=0x%X origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)",
					ServerControllerAimReasonName(commandControllerAimReason),
					serverWeaponId,
					move->buttons,
					m_ServerCommandControllerAimOrigin.x,
					m_ServerCommandControllerAimOrigin.y,
					m_ServerCommandControllerAimOrigin.z,
					m_ServerCommandControllerAimAngles.x,
					m_ServerCommandControllerAimAngles.y,
					m_ServerCommandControllerAimAngles.z);
			}
		}
	}
	else
	{
		if (hasValidPlayer)
		{
			Player& vrPlayer = m_Game->m_PlayersVRInfo[static_cast<size_t>(i)];
			vrPlayer.isUsingVR = false;
			ManualThrowResetPlayerState(vrPlayer);
		}
	}

	// If the planar SetOrigin sweep hit a stair or another obstruction, route this
	// displacement through Source's ordinary server movement for one command.
	InjectPendingRoomscaleServerCmdFallback(i, move);

	return 1;
}

void __fastcall Hooks::dWriteUsercmdDeltaToBuffer(void* ecx, void* edx, int a1, void* buf, int from, int to, bool isnewcommand)
{
	return hkWriteUsercmdDeltaToBuffer.fOriginal(ecx, a1, buf, from, to, isnewcommand);
}

int Hooks::dWriteUsercmd(void* buf, CUserCmd* to, CUserCmd* from)
{
	static int s_lastButtons = 0;
	static uint32_t s_lastObjectPullLoggedWirePayload = 0u;

	const bool localUsingMountedWeapon = m_VR &&
		m_VR->m_IsVREnabled &&
		!m_VR->m_ForceNonVRServerMovement &&
		IsLocalClientUsingMountedWeapon();

	// Final outgoing-command guard for manual reload. CreateMove already clears IN_ATTACK,
	// but keep the wire path authoritative as well so standard and encoded server commands
	// both remain decoupled from the local physical magazine interaction.
	if (to && m_VR && !localUsingMountedWeapon)
	{
		constexpr int kMagazineInteractionInAttack = (1 << 0);
		constexpr int kMagazineInteractionInReload = (1 << 13);
		const bool magazineInteractionBlocksFire = m_VR->IsMagazineInteractionBlockingFire();
		if (magazineInteractionBlocksFire)
		{
			if ((to->buttons & kMagazineInteractionInAttack) != 0 && (s_lastButtons & kMagazineInteractionInAttack) == 0)
				m_VR->PlayMagazineInteractionBlockedFireEmptySound();
			to->buttons &= ~kMagazineInteractionInAttack; // IN_ATTACK
		}
		if (m_VR->IsMagazineInteractionLeftHandActive() &&
			!m_VR->IsMagazineInteractionReloadCommandActive())
		{
			to->buttons &= ~kMagazineInteractionInReload; // IN_RELOAD
		}
		if (m_VR->IsMagazineInteractionNativeReloadSuppressActive() &&
			!m_VR->IsMagazineInteractionReloadCommandActive())
		{
			to->buttons &= ~kMagazineInteractionInReload; // IN_RELOAD
		}
		const bool suppressMagazineEmptyClipAutoReload =
			m_VR->ShouldSuppressMagazineInteractionEmptyClipAutoReload(nullptr);
		if (suppressMagazineEmptyClipAutoReload)
		{
			if ((to->buttons & kMagazineInteractionInAttack) != 0 && (s_lastButtons & kMagazineInteractionInAttack) == 0)
				m_VR->PlayMagazineInteractionBlockedFireEmptySound();
			to->buttons &= ~(kMagazineInteractionInAttack | kMagazineInteractionInReload);
		}
	}

	// VR 未启用：原样走引擎
	if (!m_VR->m_IsVREnabled)
		return hkWriteUsercmd.fOriginal(buf, to, from);

	// 只有（配置开启编码）且（本进程确实在跑服务器钩子＝能解码）且（未强制走非 VR 标准）时才编码
	const bool canEncode = (m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement);

	if (!canEncode)
	{
		// 非 VR 服务器：不要动 tick_count/command_number/viewangles.z/upmove 等
		// 保持标准 CUserCmd，让 dCreateMove 写入的 forwardmove/sidemove 正常生效
		return hkWriteUsercmd.fOriginal(buf, to, from);
	}

	// ======== 以下为原有“编码”逻辑，保持不变，仅包在 canEncode 分支内 ========
	CInput* m_Input = **(CInput***)(m_Game->m_Offsets->g_pppInput.address);
	CVerifiedUserCmd* pVerifiedCommands = *(CVerifiedUserCmd**)((uintptr_t)m_Input + 0xF0);
	CVerifiedUserCmd* pVerified = &pVerifiedCommands[(to->command_number) % 150];

	if (to && !localUsingMountedWeapon && m_VR->ShouldSuppressPrimaryFire(to, nullptr))
	{
		to->buttons &= ~(1 << 0); // IN_ATTACK
	}

	const int originalCommandNum = to->command_number;
	const uint8_t originalImpulse = to->impulse;
	uint8_t objectPullCommand = VR::kObjectPullWireNone;
	Vector objectPullPosition{};
	QAngle objectPullAngles{};
	bool objectPullOverridePose = false;
	int objectPullTargetEntityIndex = 0;
	VR::ObjectPullTargetHint objectPullTargetHint =
		VR::ObjectPullTargetHint::None;
	const bool hasObjectPullCommand = m_VR->GetObjectPullUsercmdData(
		originalCommandNum,
		objectPullCommand,
		objectPullPosition,
		objectPullAngles,
		objectPullOverridePose,
		objectPullTargetEntityIndex,
		objectPullTargetHint);

	Vector controllerPos{};
	QAngle controllerAngles{};
	if (objectPullOverridePose)
	{
		controllerPos = objectPullPosition;
		controllerAngles = objectPullAngles;
	}
	else if (!TryGetManualThrowUsercmdControllerPose(
		m_VR,
		originalCommandNum,
		controllerPos,
		controllerAngles) &&
		!BuildEncodedVRUsercmdControllerPose(
			m_VR,
			m_Game,
			controllerPos,
			controllerAngles))
	{
		return hkWriteUsercmd.fOriginal(buf, to, from);
	}
	const int originalWeaponSelect = to->weaponselect;
	const int originalWeaponSubtype = to->weaponsubtype;
	if (hasObjectPullCommand)
	{
		to->impulse = objectPullCommand;
		to->weaponselect = objectPullTargetEntityIndex;
		to->weaponsubtype = static_cast<int>(objectPullTargetHint);
		const uint32_t objectPullLogPayload =
			static_cast<uint32_t>(objectPullCommand) |
			(static_cast<uint32_t>(
				std::clamp(objectPullTargetEntityIndex, 0, 2047)) << 8) |
			(static_cast<uint32_t>(objectPullTargetHint) << 19);
		if (m_VR->m_ObjectPullDebugLog &&
			s_lastObjectPullLoggedWirePayload != objectPullLogPayload)
		{
			Game::logMsg(
				"[VR][ObjectPull][client] outgoing wire command=%u entityIndex=%d targetHint=%u usercmd=%d",
				static_cast<unsigned int>(objectPullCommand),
				objectPullTargetEntityIndex,
				static_cast<unsigned int>(objectPullTargetHint),
				originalCommandNum);
			s_lastObjectPullLoggedWirePayload = objectPullLogPayload;
		}
	}
	else
	{
		s_lastObjectPullLoggedWirePayload = 0u;
	}

	// Signal to the server that this CUserCmd has VR info. The controller pose above
	// belongs to this exact command, including when Source retransmits it as backup.
	to->tick_count *= -1;
	if (to && (to->buttons & (1 << 0)) != 0 && m_Game && m_Game->m_EngineClient)
	{
		const int lpIdx = m_Game->m_EngineClient->GetLocalPlayer();
		C_BasePlayer* localPlayer = (lpIdx > 0) ? reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(lpIdx)) : nullptr;
		C_WeaponCSBase* activeWeaponForSpread = localPlayer
			? reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon())
			: nullptr;
		m_VR->ApplyVrHandsRealBulletSpreadAimAngles(activeWeaponForSpread, controllerAngles);
	}

	to->mousedx = (int)(controllerAngles.x * 10.0f); // Strip off 2nd decimal to save bits.
	to->mousedy = (int)(controllerAngles.y * 10.0f);
	int rollEncoding = (((int)controllerAngles.z + 180) / 2 * 10000000);
	to->command_number += rollEncoding;

	if (VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > 1.1f)
	{
		to->command_number *= -1; // Signal to server that melee swing in motion
	}

	float xAngleOrig = to->viewangles.x; // 备份

	to->viewangles.z = controllerPos.x;
	to->upmove = controllerPos.y;

	// Space in CUserCmd is tight, so encode viewangle.x and controllerPos.z together.
	// Encoding will overflow if controllerPos.z goes beyond +-21474.8
	int encodedAngle = (int)((xAngleOrig + 360.0f) * 10.0f);
	int encoding = (int)(controllerPos.z * 10.0f) * 10000;
	encoding += (encoding < 0) ? -encodedAngle : encodedAngle;
	to->viewangles.x = (float)encoding;

	// 写入
	hkWriteUsercmd.fOriginal(buf, to, from);

	// 还原本地 CUserCmd
	to->viewangles.x = xAngleOrig;
	to->tick_count *= -1;
	to->viewangles.z = 0.0f;
	to->upmove = 0.0f;
	to->command_number = originalCommandNum;
	to->impulse = originalImpulse;
	to->weaponselect = originalWeaponSelect;
	to->weaponsubtype = originalWeaponSubtype;

	// 重算校验，否则多人下枪声会异常
	pVerified->m_cmd = *to;
	pVerified->m_crc = to->GetChecksum();

	s_lastButtons = to->buttons;

	return 1;
}

