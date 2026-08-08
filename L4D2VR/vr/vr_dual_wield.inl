namespace
{
    static inline size_t VR_IndependentDualWieldAimIndex(IndependentDualWieldHand hand)
    {
        return hand == IndependentDualWieldHand::PhysicalLeft ? 0u : 1u;
    }

    static inline bool VR_IndependentDualWieldReadFlag(
        const void* entity,
        int offset,
        uint8_t& outValue)
    {
        outValue = 0;
        if (!entity || offset < 0)
            return false;
#ifdef _MSC_VER
        __try
        {
            outValue = *reinterpret_cast<const uint8_t*>(
                reinterpret_cast<const uint8_t*>(entity) + offset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outValue = 0;
            return false;
        }
#else
        outValue = *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(entity) + offset);
        return true;
#endif
    }

    static inline bool VR_IndependentDualWieldGetWeaponId(
        C_WeaponCSBase* weapon,
        C_WeaponCSBase::WeaponID& outWeaponId)
    {
        outWeaponId = C_WeaponCSBase::WeaponID::NONE;
        if (!weapon)
            return false;
#ifdef _MSC_VER
        __try
        {
            outWeaponId = weapon->GetWeaponID();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outWeaponId = C_WeaponCSBase::WeaponID::NONE;
            return false;
        }
#else
        outWeaponId = weapon->GetWeaponID();
        return true;
#endif
    }

    static inline IndependentDualWieldHand VR_IndependentDualWieldOtherHand(
        IndependentDualWieldHand hand)
    {
        return hand == IndependentDualWieldHand::PhysicalLeft
            ? IndependentDualWieldHand::PhysicalRight
            : IndependentDualWieldHand::PhysicalLeft;
    }
}

const char* VR::IndependentDualWieldHandName(IndependentDualWieldHand hand) const
{
    switch (hand)
    {
    case IndependentDualWieldHand::PhysicalLeft:
        return "left";
    case IndependentDualWieldHand::PhysicalRight:
        return "right";
    default:
        return "none";
    }
}

void VR::ResetIndependentDualWieldInputState()
{
    m_IndependentDualWieldLeftFireDown.store(false, std::memory_order_release);
    m_IndependentDualWieldRightFireDown.store(false, std::memory_order_release);
    m_IndependentDualWieldLeftFireDownPrev = false;
    m_IndependentDualWieldRightFireDownPrev = false;
}

void VR::ResetIndependentDualWieldState(const char* reason, bool logTransition)
{
    bool wasActive = false;
    std::uintptr_t oldWeapon = 0;
    std::string oldClass;
    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        wasActive = m_IndependentDualWieldActive.load(std::memory_order_relaxed);
        oldWeapon = m_IndependentDualWieldWeaponTag;
        oldClass = m_IndependentDualWieldNetworkClass;

        m_IndependentDualWieldActive.store(false, std::memory_order_release);
        m_IndependentDualWieldWeaponTag = 0;
        m_IndependentDualWieldNetworkClass.clear();
        m_IndependentDualWieldIsDualOffset = -1;
        m_IndependentDualWieldHasDualOffset = -1;
        m_IndependentDualWieldAimStates = {};
        m_IndependentDualWieldUsercmdPoseSnapshots = {};
        m_IndependentDualWieldLeftFriendlyFireLatched = false;
        m_IndependentDualWieldRightFriendlyFireLatched = false;
    }

    ResetIndependentDualWieldInputState();
    m_IndependentDualWieldPendingHand.store(
        static_cast<uint8_t>(IndependentDualWieldHand::PhysicalRight),
        std::memory_order_release);
    m_IndependentDualWieldLastActualShotCommand.store(0, std::memory_order_release);
    m_IndependentDualWieldLastActualShotHand.store(
        static_cast<uint8_t>(IndependentDualWieldHand::None),
        std::memory_order_release);

    if (wasActive)
    {
        std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
        for (auto& eye : m_D3DAimLineOverlayEyes)
            eye = {};
        for (auto& eye : m_D3DAimLineOverlayEyesSecondary)
            eye = {};
        m_D3DAimLineWorldStart = {};
        m_D3DAimLineWorldEnd = {};
        m_HasD3DAimLineWorldSegment = false;
        m_D3DAimLineWorldStartSecondary = {};
        m_D3DAimLineWorldEndSecondary = {};
        m_HasD3DAimLineWorldSegmentSecondary = false;
    }

    if (logTransition && wasActive)
    {
        Game::logMsg(
            "[VR][DualWield][State] exit reason=%s weapon=%p class=%s",
            reason ? reason : "unknown",
            reinterpret_cast<void*>(oldWeapon),
            oldClass.empty() ? "unknown" : oldClass.c_str());
    }
}

bool VR::RefreshIndependentDualWieldState(C_BasePlayer* localPlayer)
{
    if (!m_IsVREnabled || !localPlayer || !m_Game)
    {
        ResetIndependentDualWieldState("no-local-player", true);
        return false;
    }

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(
        localPlayer->GetActiveWeapon());
    C_WeaponCSBase::WeaponID weaponId = C_WeaponCSBase::WeaponID::NONE;
    if (!VR_IndependentDualWieldGetWeaponId(weapon, weaponId) ||
        weaponId != C_WeaponCSBase::WeaponID::PISTOL)
    {
        ResetIndependentDualWieldState("weapon-not-pistol", true);
        return false;
    }

    const std::uintptr_t weaponTag = reinterpret_cast<std::uintptr_t>(weapon);
    const std::string networkClass = "CPistol";

    bool identityChanged = false;
    int isDualOffset = -1;
    int hasDualOffset = -1;
    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        identityChanged =
            m_IndependentDualWieldWeaponTag != weaponTag ||
            m_IndependentDualWieldNetworkClass != networkClass;
        if (!identityChanged)
        {
            isDualOffset = m_IndependentDualWieldIsDualOffset;
            hasDualOffset = m_IndependentDualWieldHasDualOffset;
        }
    }

    if (identityChanged)
    {
        // L4D2 dual wield is exclusive to CPistol. Keep recognition independent
        // from GetNetworkClassName/FindRecvPropOffset: Refresh can run from both
        // main and queued-render paths, while the RecvProp helper owns a mutable
        // process-wide cache. The current x86 CPistol layout is 0xCEC/0xCED and
        // every read remains guarded and restricted to boolean values below.
        isDualOffset = 0xCEC;
        hasDualOffset = 0xCED;
        const char* offsetSource = "pistol-layout";

        bool wasActive = false;
        {
            std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
            wasActive = m_IndependentDualWieldActive.load(std::memory_order_relaxed);
            m_IndependentDualWieldWeaponTag = weaponTag;
            m_IndependentDualWieldNetworkClass = networkClass;
            m_IndependentDualWieldIsDualOffset = isDualOffset;
            m_IndependentDualWieldHasDualOffset = hasDualOffset;
            m_IndependentDualWieldAimStates = {};
            m_IndependentDualWieldUsercmdPoseSnapshots = {};
            m_IndependentDualWieldLeftFriendlyFireLatched = false;
            m_IndependentDualWieldRightFriendlyFireLatched = false;
        }
        ResetIndependentDualWieldInputState();
        m_IndependentDualWieldPendingHand.store(
            static_cast<uint8_t>(IndependentDualWieldHand::PhysicalRight),
            std::memory_order_release);
        m_IndependentDualWieldLastActualShotCommand.store(0, std::memory_order_release);
        m_IndependentDualWieldLastActualShotHand.store(
            static_cast<uint8_t>(IndependentDualWieldHand::None),
            std::memory_order_release);

        Game::logMsg(
            "[VR][DualWield][RecvProp] weapon=%p weaponId=%d class=%s source=%s isDual=0x%X hasDual=0x%X expected=(0xCEC,0xCED) previousActive=%d",
            reinterpret_cast<void*>(weaponTag),
            static_cast<int>(weaponId),
            networkClass.c_str(),
            offsetSource,
            isDualOffset,
            hasDualOffset,
            wasActive ? 1 : 0);
    }

    if (isDualOffset < 0 || hasDualOffset < 0)
    {
        const bool wasActive = m_IndependentDualWieldActive.exchange(
            false, std::memory_order_acq_rel);
        if (wasActive)
        {
            Game::logMsg(
                "[VR][DualWield][State] exit reason=recvprop-missing weapon=%p class=%s isDual=0x%X hasDual=0x%X",
                reinterpret_cast<void*>(weaponTag),
                networkClass.c_str(),
                isDualOffset,
                hasDualOffset);
        }
        return false;
    }

    uint8_t isDualValue = 0;
    uint8_t hasDualValue = 0;
    const bool readIsDual =
        VR_IndependentDualWieldReadFlag(weapon, isDualOffset, isDualValue);
    const bool readHasDual =
        VR_IndependentDualWieldReadFlag(weapon, hasDualOffset, hasDualValue);
    const bool valuesValid =
        readIsDual && readHasDual && isDualValue <= 1u && hasDualValue <= 1u;
    const bool active = valuesValid && (isDualValue != 0u || hasDualValue != 0u);
    const bool previous = m_IndependentDualWieldActive.exchange(
        active, std::memory_order_acq_rel);

    if (!valuesValid && !previous)
    {
        static std::chrono::steady_clock::time_point s_lastInvalidFlagReadLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastInvalidFlagReadLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastInvalidFlagReadLog).count() >= 1.0f)
        {
            s_lastInvalidFlagReadLog = now;
            Game::logMsg(
                "[VR][DualWield][State] flag-read-invalid weapon=%p class=%s offsets=(0x%X,0x%X) values=(%u,%u) read=(%d,%d)",
                reinterpret_cast<void*>(weaponTag),
                networkClass.c_str(),
                isDualOffset,
                hasDualOffset,
                static_cast<unsigned int>(isDualValue),
                static_cast<unsigned int>(hasDualValue),
                readIsDual ? 1 : 0,
                readHasDual ? 1 : 0);
        }
    }

    if (previous != active)
    {
        if (active)
        {
            m_IndependentDualWieldPendingHand.store(
                static_cast<uint8_t>(IndependentDualWieldHand::PhysicalRight),
                std::memory_order_release);
            Game::logMsg(
                "[VR][DualWield][State] enter weapon=%p class=%s flags=(isDual=%u hasDual=%u) offsets=(0x%X,0x%X) sharedWeaponState=1",
                reinterpret_cast<void*>(weaponTag),
                networkClass.c_str(),
                static_cast<unsigned int>(isDualValue),
                static_cast<unsigned int>(hasDualValue),
                isDualOffset,
                hasDualOffset);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
                m_IndependentDualWieldAimStates = {};
                m_IndependentDualWieldUsercmdPoseSnapshots = {};
                m_IndependentDualWieldLeftFriendlyFireLatched = false;
                m_IndependentDualWieldRightFriendlyFireLatched = false;
            }
            {
                std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
                for (auto& eye : m_D3DAimLineOverlayEyesSecondary)
                    eye = {};
                m_D3DAimLineWorldStartSecondary = {};
                m_D3DAimLineWorldEndSecondary = {};
                m_HasD3DAimLineWorldSegmentSecondary = false;
            }
            ResetIndependentDualWieldInputState();
            m_IndependentDualWieldPendingHand.store(
                static_cast<uint8_t>(IndependentDualWieldHand::PhysicalRight),
                std::memory_order_release);
            Game::logMsg(
                "[VR][DualWield][State] exit reason=%s weapon=%p class=%s flags=(isDual=%u hasDual=%u) read=(%d,%d)",
                valuesValid ? "flags-clear" : "flag-read-invalid",
                reinterpret_cast<void*>(weaponTag),
                networkClass.c_str(),
                static_cast<unsigned int>(isDualValue),
                static_cast<unsigned int>(hasDualValue),
                readIsDual ? 1 : 0,
                readHasDual ? 1 : 0);
        }
    }

    return active;
}

bool VR::GetIndependentDualWieldPhysicalDigitalActionData(
    vr::VRActionHandle_t actionHandle,
    bool physicalLeft,
    vr::InputDigitalActionData_t& digitalDataOut)
{
    digitalDataOut = {};
    if (!m_Input || actionHandle == vr::k_ulInvalidActionHandle)
        return false;

    vr::VRInputValueHandle_t& inputSource = physicalLeft
        ? m_IndependentDualWieldPhysicalLeftInputSource
        : m_IndependentDualWieldPhysicalRightInputSource;
    if (inputSource == vr::k_ulInvalidInputValueHandle)
    {
        const char* path = physicalLeft
            ? "/user/hand/left"
            : "/user/hand/right";
        if (m_Input->GetInputSourceHandle(path, &inputSource) !=
            vr::VRInputError_None)
        {
            inputSource = vr::k_ulInvalidInputValueHandle;
            return false;
        }
    }

    return m_Input->GetDigitalActionData(
        actionHandle,
        &digitalDataOut,
        sizeof(digitalDataOut),
        inputSource) == vr::VRInputError_None;
}

bool VR::UpdateIndependentDualWieldInputState(
    bool& attackDown,
    bool& attackJustPressed)
{
    if (!IsIndependentDualWieldActive())
    {
        ResetIndependentDualWieldInputState();
        return false;
    }

    auto readPhysicalHand = [&](bool physicalLeft, bool& down) -> bool
        {
            vr::InputDigitalActionData_t primary{};
            vr::InputDigitalActionData_t secondary{};
            const bool primaryOk = GetIndependentDualWieldPhysicalDigitalActionData(
                m_ActionPrimaryAttack, physicalLeft, primary);
            const bool secondaryOk = GetIndependentDualWieldPhysicalDigitalActionData(
                m_ActionSecondaryAttack, physicalLeft, secondary);
            const bool primaryActive = primaryOk && primary.bActive;
            const bool secondaryActive = secondaryOk && secondary.bActive;
            down = (primaryActive && primary.bState) ||
                (secondaryActive && secondary.bState);
            return primaryActive || secondaryActive;
        };

    bool leftDown = false;
    bool rightDown = false;
    const bool leftValid = readPhysicalHand(true, leftDown);
    const bool rightValid = readPhysicalHand(false, rightDown);
    if (!leftValid || !rightValid)
    {
        static std::chrono::steady_clock::time_point s_lastInputFailureLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastInputFailureLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastInputFailureLog).count() >= 1.0f)
        {
            s_lastInputFailureLog = now;
            Game::logMsg(
                "[VR][DualWield][Input] physical action query incomplete leftValid=%d rightValid=%d left=%d right=%d",
                leftValid ? 1 : 0,
                rightValid ? 1 : 0,
                leftDown ? 1 : 0,
                rightDown ? 1 : 0);
        }
    }

    if (!leftValid && !rightValid)
    {
        // Do not silently reuse the normal single-gun action state. That would make
        // physical-hand routing failures look like intermittent right-gun shots.
        attackDown = false;
        attackJustPressed = false;
        m_IndependentDualWieldLeftFireDownPrev = false;
        m_IndependentDualWieldRightFireDownPrev = false;
        m_IndependentDualWieldLeftFireDown.store(false, std::memory_order_release);
        m_IndependentDualWieldRightFireDown.store(false, std::memory_order_release);
        return false;
    }

    const bool leftPressed = leftDown && !m_IndependentDualWieldLeftFireDownPrev;
    const bool rightPressed = rightDown && !m_IndependentDualWieldRightFireDownPrev;
    const bool changed =
        leftDown != m_IndependentDualWieldLeftFireDownPrev ||
        rightDown != m_IndependentDualWieldRightFireDownPrev;

    m_IndependentDualWieldLeftFireDownPrev = leftDown;
    m_IndependentDualWieldRightFireDownPrev = rightDown;
    m_IndependentDualWieldLeftFireDown.store(leftDown, std::memory_order_release);
    m_IndependentDualWieldRightFireDown.store(rightDown, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        if (!leftDown)
            m_IndependentDualWieldLeftFriendlyFireLatched = false;
        if (!rightDown)
            m_IndependentDualWieldRightFriendlyFireLatched = false;
    }

    attackDown = leftDown || rightDown;
    attackJustPressed = leftPressed || rightPressed;

    if (changed)
    {
        Game::logMsg(
            "[VR][DualWield][Input] left=%d right=%d pressed=(%d,%d) leftHanded=%d swapActions=%d",
            leftDown ? 1 : 0,
            rightDown ? 1 : 0,
            leftPressed ? 1 : 0,
            rightPressed ? 1 : 0,
            m_LeftHanded ? 1 : 0,
            m_LeftHandedSwapInputActions ? 1 : 0);
    }

    return true;
}

Vector VR::GetIndependentDualWieldPhysicalControllerPos(bool physicalLeft) const
{
    VR* mutableThis = const_cast<VR*>(this);
    const bool useLogicalLeft = physicalLeft ? !m_LeftHanded : m_LeftHanded;
    return useLogicalLeft
        ? mutableThis->GetLeftControllerAbsPos()
        : mutableThis->GetRightControllerAbsPos();
}

QAngle VR::GetIndependentDualWieldPhysicalControllerAngle(bool physicalLeft) const
{
    VR* mutableThis = const_cast<VR*>(this);
    const bool useLogicalLeft = physicalLeft ? !m_LeftHanded : m_LeftHanded;

    // The main tracking fields retain the anatomical OpenVR orientation before the
    // logical off-hand -45 degree model calibration and before gameplay auto-aim.
    // The render thread instead consumes the published render-frame snapshot, so
    // undo only the logical-left calibration there.
    QAngle controllerAngles{};
    bool applyWeaponPitch = true;
    if (t_UseRenderFrameSnapshot)
    {
        controllerAngles = useLogicalLeft
            ? mutableThis->GetLeftControllerAbsAngle()
            : mutableThis->GetRightControllerAbsAngle();
        applyWeaponPitch = useLogicalLeft;
    }
    else
    {
        controllerAngles = useLogicalLeft
            ? m_LeftControllerTrackedAngAbs
            : m_RightControllerTrackedAngAbs;
    }

    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(controllerAngles, &forward, &right, &up);
    if (forward.IsZero() || right.IsZero() || up.IsZero())
        return controllerAngles;

    if (t_UseRenderFrameSnapshot && useLogicalLeft)
    {
        forward = VectorRotate(forward, right, 45.0f);
        up = VectorRotate(up, right, 45.0f);
    }

    if (applyWeaponPitch)
    {
        forward = VectorRotate(forward, right, m_WeaponAimPitchOffsetDeg);
        up = VectorRotate(up, right, m_WeaponAimPitchOffsetDeg);
    }

    QAngle result{};
    QAngle::VectorAngles(forward, up, result);
    NormalizeAndClampViewAngles(result);
    return result;
}

void VR::GetIndependentDualWieldViewmodelPose(
    bool physicalLeft,
    Vector& outPosition,
    QAngle& outAngles) const
{
    const Vector controllerPosition =
        GetIndependentDualWieldPhysicalControllerPos(physicalLeft);
    const QAngle controllerAngles =
        GetIndependentDualWieldPhysicalControllerAngle(physicalLeft);

    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(controllerAngles, &forward, &right, &up);
    if (forward.IsZero() || right.IsZero() || up.IsZero())
    {
        outPosition = controllerPosition;
        outAngles = controllerAngles;
        return;
    }

    forward = VectorRotate(forward, up, m_ViewmodelAngOffset.y);
    right = VectorRotate(right, up, m_ViewmodelAngOffset.y);
    forward = VectorRotate(forward, right, m_ViewmodelAngOffset.x);
    up = VectorRotate(up, right, m_ViewmodelAngOffset.x);
    right = VectorRotate(right, forward, m_ViewmodelAngOffset.z);
    up = VectorRotate(up, forward, m_ViewmodelAngOffset.z);

    outPosition = controllerPosition
        - (forward * m_ViewmodelPosOffset.x)
        - (right * m_ViewmodelPosOffset.y)
        - (up * m_ViewmodelPosOffset.z);
    QAngle::VectorAngles(forward, up, outAngles);
    NormalizeAndClampViewAngles(outAngles);
}

bool VR::GetIndependentDualWieldAimState(
    IndependentDualWieldHand hand,
    IndependentDualWieldAimState& out) const
{
    out = {};
    if (hand != IndependentDualWieldHand::PhysicalLeft &&
        hand != IndependentDualWieldHand::PhysicalRight)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
    out = m_IndependentDualWieldAimStates[
        VR_IndependentDualWieldAimIndex(hand)];
    return out.valid;
}

IndependentDualWieldHand VR::GetIndependentDualWieldPendingHand() const
{
    const IndependentDualWieldHand hand = static_cast<IndependentDualWieldHand>(
        m_IndependentDualWieldPendingHand.load(std::memory_order_acquire));
    return hand == IndependentDualWieldHand::PhysicalLeft ||
        hand == IndependentDualWieldHand::PhysicalRight
        ? hand
        : IndependentDualWieldHand::PhysicalRight;
}

bool VR::UpdateIndependentDualWieldAimStates(
    C_BasePlayer* localPlayer,
    bool drawDebugLines)
{
    if (!IsIndependentDualWieldActive() || !localPlayer || !m_Game ||
        !m_Game->m_EngineTrace)
    {
        return false;
    }

    C_WeaponCSBase* activeWeapon = reinterpret_cast<C_WeaponCSBase*>(
        localPlayer->GetActiveWeapon());
    if (!activeWeapon)
        return false;

    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
    IClientEntityList* entityList = m_Game->m_ClientEntityList;
    IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(
        entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
    IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(
        entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
    CTraceFilterSkipThreeEntities traceFilterThree(
        reinterpret_cast<IHandleEntity*>(localPlayer),
        safeMountedUseEnt,
        safeActiveWeapon,
        0);
    CTraceFilter* traceFilter = static_cast<CTraceFilter*>(&traceFilterThree);

    float friendlyGuardRadiusMeters = m_BlockFireOnFriendlyAimRadiusMeters;
    if (m_VRScale > 1.0f)
    {
        const float speed2D = localPlayer->m_vecVelocity.Length2D() / m_VRScale;
        float t = (speed2D - 0.2f) / (3.0f - 0.2f);
        t = std::clamp(t, 0.0f, 1.0f);
        friendlyGuardRadiusMeters = std::clamp(
            friendlyGuardRadiusMeters + 0.06f * t,
            0.0f,
            0.5f);
    }
    const float hullRadius = friendlyGuardRadiusMeters > 0.0f
        ? friendlyGuardRadiusMeters * m_VRScale
        : 0.0f;
    const Vector hullMins(-hullRadius, -hullRadius, -hullRadius);
    const Vector hullMaxs(hullRadius, hullRadius, hullRadius);

    int localTeam = 0;
    VR_TryReadI32(reinterpret_cast<const unsigned char*>(localPlayer),
        kTeamNumOffset, localTeam);

    auto isFriendlyTrace = [&](const CGameTrace& trace) -> bool
        {
            C_BaseEntity* entity = reinterpret_cast<C_BaseEntity*>(trace.m_pEnt);
            if (!entity || entity == localPlayer || !IsEntityAlive(entity))
                return false;

            int team = 0;
            unsigned char lifeState = 1;
            const unsigned char* base = reinterpret_cast<const unsigned char*>(entity);
            if (!VR_TryReadI32(base, kTeamNumOffset, team) ||
                !VR_TryReadU8(base, kLifeStateOffset, lifeState))
            {
                return false;
            }
            return lifeState == 0 && localTeam > 0 && team == localTeam;
        };

    auto traceFriendly = [&](const Vector& start, const Vector& end) -> bool
        {
            CGameTrace lineTrace{};
            Ray_t lineRay{};
            lineRay.Init(start, end);
            VR_SafeTraceRay(
                m_Game->m_EngineTrace,
                lineRay,
                STANDARD_TRACE_MASK,
                traceFilter,
                lineTrace);
            if (isFriendlyTrace(lineTrace))
                return true;

            if (hullRadius <= 0.01f)
                return false;

            CGameTrace hullTrace{};
            Ray_t hullRay{};
            hullRay.Init(start, end, hullMins, hullMaxs);
            VR_SafeTraceRay(
                m_Game->m_EngineTrace,
                hullRay,
                MASK_SHOT_HULL,
                traceFilter,
                hullTrace);
            return isFriendlyTrace(hullTrace);
        };

    auto buildHand = [&](bool physicalLeft, IndependentDualWieldAimState& out)
        {
            out = {};
            const QAngle controllerAngles =
                GetIndependentDualWieldPhysicalControllerAngle(physicalLeft);
            const Vector controllerOrigin =
                GetIndependentDualWieldPhysicalControllerPos(physicalLeft);
            Vector direction{};
            QAngle::AngleVectors(controllerAngles, &direction, nullptr, nullptr);
            if (controllerOrigin.IsZero() || direction.IsZero())
                return;
            VectorNormalize(direction);

            // Independent dual wield is controller-authored. Do not inherit the
            // viewmodel position offset or pass through the viewmodel-layer
            // formatter: both visually detach the ray from the physical hand.
            Vector start = controllerOrigin;
            Vector fullEnd = start + direction * 8192.0f;

            CGameTrace visibleTrace{};
            Ray_t visibleRay{};
            visibleRay.Init(start, fullEnd);
            VR_SafeTraceRay(
                m_Game->m_EngineTrace,
                visibleRay,
                STANDARD_TRACE_MASK,
                traceFilter,
                visibleTrace);
            const bool hit = visibleTrace.fraction > 0.0f &&
                visibleTrace.fraction < 1.0f &&
                !visibleTrace.startsolid &&
                !visibleTrace.allsolid;
            const Vector visibleEnd = hit ? visibleTrace.endpos : fullEnd;

            const bool friendlyGun = m_BlockFireOnFriendlyAimEnabled
                ? traceFriendly(start, fullEnd)
                : false;
            bool friendlyEye = false;
            if (m_BlockFireOnFriendlyAimEnabled)
            {
                Vector eyeDirection = visibleEnd - localPlayer->EyePosition();
                if (!eyeDirection.IsZero())
                {
                    VectorNormalize(eyeDirection);
                    const Vector eyeStart =
                        localPlayer->EyePosition() + eyeDirection * 2.0f;
                    const Vector eyeEnd = eyeStart + eyeDirection * 8192.0f;
                    friendlyEye = traceFriendly(eyeStart, eyeEnd);
                }
            }

            out.valid = true;
            out.hitsFriendly = friendlyGun || friendlyEye;
            out.origin = controllerOrigin;
            out.start = start;
            out.end = visibleEnd;
            out.visibleEnd = visibleEnd;
            out.direction = direction;
            out.hitEntityTag = reinterpret_cast<std::uintptr_t>(
                hit ? visibleTrace.m_pEnt : nullptr);
        };

    IndependentDualWieldAimState left{};
    IndependentDualWieldAimState right{};
    buildHand(true, left);
    buildHand(false, right);
    if (!left.valid || !right.valid)
    {
        {
            std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
            m_IndependentDualWieldAimStates = {};
        }
        {
            std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
            for (auto& eye : m_D3DAimLineOverlayEyes)
                eye = {};
            for (auto& eye : m_D3DAimLineOverlayEyesSecondary)
                eye = {};
            m_D3DAimLineWorldStart = {};
            m_D3DAimLineWorldEnd = {};
            m_HasD3DAimLineWorldSegment = false;
            m_D3DAimLineWorldStartSecondary = {};
            m_D3DAimLineWorldEndSecondary = {};
            m_HasD3DAimLineWorldSegmentSecondary = false;
        }
        m_HasAimLine = false;
        m_AimLineHitsFriendly = false;

        static std::chrono::steady_clock::time_point s_lastInvalidAimLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastInvalidAimLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastInvalidAimLog).count() >= 1.0f)
        {
            s_lastInvalidAimLog = now;
            const Vector leftPos = GetIndependentDualWieldPhysicalControllerPos(true);
            const Vector rightPos = GetIndependentDualWieldPhysicalControllerPos(false);
            const QAngle leftAngles = GetIndependentDualWieldPhysicalControllerAngle(true);
            const QAngle rightAngles = GetIndependentDualWieldPhysicalControllerAngle(false);
            Game::logMsg(
                "[VR][DualWield][Aim] invalid left=%d right=%d leftPos=(%.1f %.1f %.1f) leftAng=(%.1f %.1f %.1f) rightPos=(%.1f %.1f %.1f) rightAng=(%.1f %.1f %.1f)",
                left.valid ? 1 : 0,
                right.valid ? 1 : 0,
                leftPos.x, leftPos.y, leftPos.z,
                leftAngles.x, leftAngles.y, leftAngles.z,
                rightPos.x, rightPos.y, rightPos.z,
                rightAngles.x, rightAngles.y, rightAngles.z);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        m_IndependentDualWieldAimStates[0] = left;
        m_IndependentDualWieldAimStates[1] = right;
    }

    const IndependentDualWieldHand selectedHand =
        GetIndependentDualWieldPendingHand();
    const IndependentDualWieldAimState& selected =
        selectedHand == IndependentDualWieldHand::PhysicalLeft ? left : right;

    const bool aimLineVisible =
        ShouldShowAimLine(activeWeapon) && ShouldDrawAimLine(activeWeapon);
    m_AimLineStart = selected.start;
    m_AimLineEnd = selected.end;
    m_LastAimDirection = selected.direction;
    m_AimLineHitsFriendly = selected.hitsFriendly;
    m_HasAimLine = selected.valid && aimLineVisible;
    m_HasAimConvergePoint = false;
    m_HasThrowArc = false;
    m_LastAimWasThrowable = false;

    UpdateAimTeammateHudTarget(
        localPlayer,
        selected.start,
        selected.end,
        selected.valid && aimLineVisible);

    {
        std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
        m_D3DAimLineWorldStart = left.start;
        m_D3DAimLineWorldEnd = left.visibleEnd;
        m_HasD3DAimLineWorldSegment = aimLineVisible && left.valid &&
            !((left.visibleEnd - left.start).IsZero());
        m_D3DAimLineWorldStartSecondary = right.start;
        m_D3DAimLineWorldEndSecondary = right.visibleEnd;
        m_HasD3DAimLineWorldSegmentSecondary = aimLineVisible && right.valid &&
            !((right.visibleEnd - right.start).IsZero());
    }

    if (drawDebugLines)
    {
        DrawAimLine(left.start, left.visibleEnd);
        DrawAimLine(right.start, right.visibleEnd);
    }

    return true;
}

IndependentDualWieldHand VR::PrepareIndependentDualWieldUsercmd(
    CUserCmd* cmd,
    C_BasePlayer* localPlayer)
{
    if (!cmd || cmd->command_number <= 0 ||
        !RefreshIndependentDualWieldState(localPlayer))
    {
        return IndependentDualWieldHand::None;
    }

    if (localPlayer)
        UpdateIndependentDualWieldAimStates(localPlayer, false);

    const bool leftDown = m_IndependentDualWieldLeftFireDown.load(
        std::memory_order_acquire);
    const bool rightDown = m_IndependentDualWieldRightFireDown.load(
        std::memory_order_acquire);
    IndependentDualWieldHand selected = GetIndependentDualWieldPendingHand();
    if (leftDown && !rightDown)
        selected = IndependentDualWieldHand::PhysicalLeft;
    else if (rightDown && !leftDown)
        selected = IndependentDualWieldHand::PhysicalRight;

    IndependentDualWieldAimState leftAim{};
    IndependentDualWieldAimState rightAim{};
    bool leftFriendlyFireLatched = false;
    bool rightFriendlyFireLatched = false;
    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        leftAim = m_IndependentDualWieldAimStates[0];
        rightAim = m_IndependentDualWieldAimStates[1];
        leftFriendlyFireLatched = m_IndependentDualWieldLeftFriendlyFireLatched;
        rightFriendlyFireLatched = m_IndependentDualWieldRightFriendlyFireLatched;
    }

    IndependentDualWieldAimState selectedAim =
        selected == IndependentDualWieldHand::PhysicalLeft ? leftAim : rightAim;
    const IndependentDualWieldHand other =
        VR_IndependentDualWieldOtherHand(selected);
    const IndependentDualWieldAimState otherAim =
        other == IndependentDualWieldHand::PhysicalLeft ? leftAim : rightAim;
    const bool otherDown = other == IndependentDualWieldHand::PhysicalLeft
        ? leftDown
        : rightDown;
    const bool selectedLatched = selected == IndependentDualWieldHand::PhysicalLeft
        ? leftFriendlyFireLatched
        : rightFriendlyFireLatched;
    const bool otherLatched = other == IndependentDualWieldHand::PhysicalLeft
        ? leftFriendlyFireLatched
        : rightFriendlyFireLatched;
    const bool selectedBlocked = selectedAim.hitsFriendly || selectedLatched;
    const bool otherBlocked = otherAim.hitsFriendly || otherLatched;
    if (selectedBlocked && otherDown && otherAim.valid && !otherBlocked)
    {
        selected = other;
        selectedAim = otherAim;
    }

    if (leftDown != rightDown)
    {
        m_IndependentDualWieldPendingHand.store(
            static_cast<uint8_t>(selected),
            std::memory_order_release);
    }

    const bool requestedAttack = (cmd->buttons & (1 << 0)) != 0;
    if (requestedAttack && !selectedAim.valid)
    {
        cmd->buttons &= ~(1 << 0);
        static std::chrono::steady_clock::time_point s_lastInvalidAimBlockLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastInvalidAimBlockLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastInvalidAimBlockLog).count() >= 0.50f)
        {
            s_lastInvalidAimBlockLog = now;
            Game::logMsg(
                "[VR][DualWield][Schedule] blocked cmd=%d hand=%s because selected aim ray was invalid",
                cmd->command_number,
                IndependentDualWieldHandName(selected));
        }
    }

    IndependentDualWieldUsercmdPoseSnapshot snapshot{};
    snapshot.valid = true;
    snapshot.attackDown = requestedAttack && selectedAim.valid;
    snapshot.commandNumber = cmd->command_number;
    snapshot.hand = selected;
    snapshot.position = selectedAim.valid
        ? selectedAim.origin
        : GetIndependentDualWieldPhysicalControllerPos(
            selected == IndependentDualWieldHand::PhysicalLeft);
    snapshot.aimEnd = selectedAim.valid
        ? selectedAim.end
        : snapshot.position;
    snapshot.angles = GetIndependentDualWieldPhysicalControllerAngle(
        selected == IndependentDualWieldHand::PhysicalLeft);
    Vector aimDirection = snapshot.aimEnd - snapshot.position;
    if (!aimDirection.IsZero())
    {
        VectorNormalize(aimDirection);
        QAngle::VectorAngles(aimDirection, snapshot.angles);
        NormalizeAndClampViewAngles(snapshot.angles);
    }

    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        const size_t slot = static_cast<size_t>(cmd->command_number) %
            kIndependentDualWieldUsercmdPoseSnapshotCount;
        m_IndependentDualWieldUsercmdPoseSnapshots[slot] = snapshot;
    }

    if (snapshot.attackDown)
    {
        static std::atomic<int> s_lastScheduledCommand{ 0 };
        const int previousLogged = s_lastScheduledCommand.exchange(
            cmd->command_number, std::memory_order_acq_rel);
        if (previousLogged != cmd->command_number)
        {
            Game::logMsg(
                "[VR][DualWield][Schedule] cmd=%d hand=%s triggers=(%d,%d) aimValid=(%d,%d) friendly=(%d,%d) latched=(%d,%d)",
                cmd->command_number,
                IndependentDualWieldHandName(selected),
                leftDown ? 1 : 0,
                rightDown ? 1 : 0,
                leftAim.valid ? 1 : 0,
                rightAim.valid ? 1 : 0,
                leftAim.hitsFriendly ? 1 : 0,
                rightAim.hitsFriendly ? 1 : 0,
                leftFriendlyFireLatched ? 1 : 0,
                rightFriendlyFireLatched ? 1 : 0);
        }
    }

    return selected;
}

bool VR::TryGetIndependentDualWieldUsercmdPose(
    int commandNumber,
    IndependentDualWieldUsercmdPoseSnapshot& out) const
{
    out = {};
    if (commandNumber <= 0)
        return false;

    std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
    const size_t slot = static_cast<size_t>(commandNumber) %
        kIndependentDualWieldUsercmdPoseSnapshotCount;
    const IndependentDualWieldUsercmdPoseSnapshot& snapshot =
        m_IndependentDualWieldUsercmdPoseSnapshots[slot];
    if (!snapshot.valid || snapshot.commandNumber != commandNumber)
        return false;
    out = snapshot;
    return true;
}

bool VR::TryGetIndependentDualWieldPoseForEncoding(
    int commandNumber,
    Vector& outPosition,
    QAngle& outAngles,
    IndependentDualWieldHand* outHand) const
{
    IndependentDualWieldUsercmdPoseSnapshot snapshot{};
    if (!IsIndependentDualWieldActive() ||
        !TryGetIndependentDualWieldUsercmdPose(commandNumber, snapshot))
    {
        return false;
    }

    outPosition = snapshot.shotResolved
        ? snapshot.resolvedPosition
        : snapshot.position;
    outAngles = snapshot.shotResolved
        ? snapshot.resolvedAngles
        : snapshot.angles;
    if (outHand)
        *outHand = snapshot.hand;
    return true;
}

bool VR::CommitIndependentDualWieldClientShot(
    int commandNumber,
    IndependentDualWieldHand hand,
    const Vector& resolvedPosition,
    const QAngle& resolvedAngles)
{
    if (!IsIndependentDualWieldActive() || commandNumber <= 0 ||
        (hand != IndependentDualWieldHand::PhysicalLeft &&
            hand != IndependentDualWieldHand::PhysicalRight))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_IndependentDualWieldMutex);
        const size_t slot = static_cast<size_t>(commandNumber) %
            kIndependentDualWieldUsercmdPoseSnapshotCount;
        IndependentDualWieldUsercmdPoseSnapshot& snapshot =
            m_IndependentDualWieldUsercmdPoseSnapshots[slot];
        if (!snapshot.valid || snapshot.commandNumber != commandNumber ||
            snapshot.hand != hand)
        {
            return false;
        }
        if (snapshot.shotResolved)
            return false;

        snapshot.shotResolved = true;
        snapshot.resolvedPosition = resolvedPosition;
        snapshot.resolvedAngles = resolvedAngles;
    }

    const int previousCommand =
        m_IndependentDualWieldLastActualShotCommand.exchange(
            commandNumber, std::memory_order_acq_rel);
    if (previousCommand == commandNumber)
        return false;

    m_IndependentDualWieldLastActualShotHand.store(
        static_cast<uint8_t>(hand), std::memory_order_release);

    const bool leftDown = m_IndependentDualWieldLeftFireDown.load(
        std::memory_order_acquire);
    const bool rightDown = m_IndependentDualWieldRightFireDown.load(
        std::memory_order_acquire);
    IndependentDualWieldHand next = hand;
    if (leftDown && rightDown)
        next = VR_IndependentDualWieldOtherHand(hand);
    else if (leftDown)
        next = IndependentDualWieldHand::PhysicalLeft;
    else if (rightDown)
        next = IndependentDualWieldHand::PhysicalRight;
    m_IndependentDualWieldPendingHand.store(
        static_cast<uint8_t>(next), std::memory_order_release);
    return true;
}

void VR::TriggerIndependentDualWieldFireHaptics(
    int weaponId,
    IndependentDualWieldHand hand)
{
    if (!m_WeaponHapticsEnabled ||
        (hand != IndependentDualWieldHand::PhysicalLeft &&
            hand != IndependentDualWieldHand::PhysicalRight))
    {
        return;
    }

    const WeaponHapticsProfile profile = GetWeaponHapticsProfile(weaponId);
    TriggerPhysicalHandHapticPulse(
        hand == IndependentDualWieldHand::PhysicalLeft,
        profile.durationSeconds,
        profile.frequency,
        profile.amplitude);
}
