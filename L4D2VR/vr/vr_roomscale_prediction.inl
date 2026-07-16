Vector VR::GetAimRenderCameraDelta() const
{
    if (!m_IsThirdPersonCamera)
        return m_ThirdPersonViewOrigin - m_SetupOrigin;

    // 1:1 roomscale decoupled camera already makes controller/viewmodel positions live in the
    // current VR camera anchor space. Adding the third-person render-center delta again makes
    // aim/debug/D3D lines drift away from the physical controller until ResetPosition recenters.
    if (m_Roomscale1To1Movement && m_Roomscale1To1DecoupleCamera && !m_ForceNonVRServerMovement)
        return Vector{ 0.0f, 0.0f, 0.0f };

    return m_ThirdPersonRenderCenter - m_SetupOrigin;
}

bool VR::ShouldUseRoomscale1To1ServerMove() const
{
    const bool hasServerEntityMove =
        m_Game &&
        m_Game->m_EngineTraceServer &&
        m_Game->m_Offsets &&
        m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.address &&
        m_Game->m_Offsets->CBaseEntity_SetOrigin_Server.address;

    return m_IsVREnabled &&
        m_Roomscale1To1Movement &&
        m_Roomscale1To1ServerMove &&
        Hooks::s_ServerUnderstandsVR &&
        !m_ForceNonVRServerMovement &&
        hasServerEntityMove;
}

namespace
{
    struct Roomscale1To1ServerVisualState
    {
        VR* owner = nullptr;
        Vector pendingServerVisualWorldDelta = {};
        Vector pendingServerVisualCorrectionWorld = {};
        bool pendingServerVisualCorrectionWorldValid = false;
    };

    Roomscale1To1ServerVisualState& GetRoomscale1To1ServerVisualState(VR* owner)
    {
        static Roomscale1To1ServerVisualState state;
        if (state.owner != owner)
        {
            state = {};
            state.owner = owner;
        }
        return state;
    }

    bool IsFiniteRoomscalePlanarVector(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    void NormalizeRoomscalePendingCorrection(Vector& correction, bool& valid)
    {
        correction.z = 0.0f;
        const float len = correction.Length();
        if (!std::isfinite(len) || len <= 0.01f)
        {
            correction = {};
            valid = false;
            return;
        }

        valid = true;
    }
}

void VR::QueueRoomscale1To1ServerMoveDelta(const Vector& worldDelta, const Vector& visualWorldDelta)
{
    Vector planarDelta(worldDelta.x, worldDelta.y, 0.0f);
    Vector planarVisualDelta(visualWorldDelta.x, visualWorldDelta.y, 0.0f);
    if (!IsFiniteRoomscalePlanarVector(planarDelta) || !IsFiniteRoomscalePlanarVector(planarVisualDelta))
        return;

    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);

    // The VR pose can update faster than server usercmd processing. Keep only the newest
    // direct server move request, but cancel the visual displacement that belonged to any
    // overwritten request. Otherwise the HMD camera continues forward while the player
    // entity never receives that discarded physical step.
    if (m_Roomscale1To1PendingServerWorldDeltaValid)
    {
        visualState.pendingServerVisualCorrectionWorld -= visualState.pendingServerVisualWorldDelta;
        NormalizeRoomscalePendingCorrection(
            visualState.pendingServerVisualCorrectionWorld,
            visualState.pendingServerVisualCorrectionWorldValid);
    }

    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;

    const float requestedLen = planarDelta.Length();
    if (!std::isfinite(requestedLen) || requestedLen <= 0.01f)
    {
        visualState.pendingServerVisualCorrectionWorld -= planarVisualDelta;
        NormalizeRoomscalePendingCorrection(
            visualState.pendingServerVisualCorrectionWorld,
            visualState.pendingServerVisualCorrectionWorldValid);
        return;
    }

    m_Roomscale1To1PendingServerWorldDelta = planarDelta;
    visualState.pendingServerVisualWorldDelta = planarVisualDelta;

    const float maxServerStep =
        std::max(1.0f, std::max(0.0f, m_Roomscale1To1MaxStepMeters) * std::max(1.0f, std::fabs(m_VRScale)));
    const float len = m_Roomscale1To1PendingServerWorldDelta.Length();
    if (std::isfinite(len) && len > maxServerStep && len > 0.0001f)
        m_Roomscale1To1PendingServerWorldDelta *= (maxServerStep / len);

    m_Roomscale1To1PendingServerWorldDeltaValid = true;
}

bool VR::ConsumeRoomscale1To1ServerMoveDelta(Vector& outWorldDelta, Vector& outVisualWorldDelta)
{
    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    if (!m_Roomscale1To1PendingServerWorldDeltaValid)
        return false;

    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    outWorldDelta = m_Roomscale1To1PendingServerWorldDelta;
    outVisualWorldDelta = visualState.pendingServerVisualWorldDelta;
    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;
    return outWorldDelta.Length() > 0.01f;
}

void VR::QueueRoomscale1To1ServerVisualCorrection(const Vector& worldCorrection)
{
    Vector planarCorrection(worldCorrection.x, worldCorrection.y, 0.0f);
    if (!IsFiniteRoomscalePlanarVector(planarCorrection))
        return;

    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    visualState.pendingServerVisualCorrectionWorld += planarCorrection;
    NormalizeRoomscalePendingCorrection(
        visualState.pendingServerVisualCorrectionWorld,
        visualState.pendingServerVisualCorrectionWorldValid);
}

void VR::ApplyRoomscale1To1VisualWorldCorrection(const Vector& worldCorrection)
{
    Vector planarCorrection(worldCorrection.x, worldCorrection.y, 0.0f);
    if (!IsFiniteRoomscalePlanarVector(planarCorrection))
        return;

    const float correctionLen = planarCorrection.Length();
    if (!std::isfinite(correctionLen) || correctionLen <= 0.01f || std::fabs(m_VRScale) <= 0.001f)
        return;

    const Vector correctionLocal = planarCorrection / m_VRScale;
    m_HmdPosCorrectedPrev += correctionLocal;
    if (m_Roomscale1To1PrevValid)
        m_Roomscale1To1PrevCorrectedAbs += correctionLocal;

    // A roomscale collision correction is a coordinate rebase, not tracked-head motion.
    // Shift the smoothed/cached values together so head smoothing does not repay the
    // rejected movement over several rendered frames and cached aim/controller positions
    // remain aligned until the next tracking update.
    if (m_HeadSmoothingInitialized)
        m_HmdPosSmoothed += correctionLocal;
    m_HmdPosLocalInWorld += planarCorrection;
    m_HmdPosAbs += planarCorrection;
    m_HmdPosAbsPrev += planarCorrection;
    m_SetupOriginToHMD += planarCorrection;
    m_LeftControllerPosAbs += planarCorrection;
    m_RightControllerPosAbs += planarCorrection;
}

void VR::ApplyPendingRoomscale1To1ServerVisualCorrection()
{
    Vector worldCorrection{};
    {
        std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
        auto& visualState = GetRoomscale1To1ServerVisualState(this);
        if (!visualState.pendingServerVisualCorrectionWorldValid)
            return;

        worldCorrection = visualState.pendingServerVisualCorrectionWorld;
        visualState.pendingServerVisualCorrectionWorld = {};
        visualState.pendingServerVisualCorrectionWorldValid = false;
    }

    ApplyRoomscale1To1VisualWorldCorrection(worldCorrection);

    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][servermove-visual-correct] world=(%.2f %.2f) len=%.2f",
            worldCorrection.x, worldCorrection.y, worldCorrection.Length());
    }
}

void VR::CancelRoomscale1To1ServerMoveDelta()
{
    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    if (m_Roomscale1To1PendingServerWorldDeltaValid)
    {
        visualState.pendingServerVisualCorrectionWorld -= visualState.pendingServerVisualWorldDelta;
        NormalizeRoomscalePendingCorrection(
            visualState.pendingServerVisualCorrectionWorld,
            visualState.pendingServerVisualCorrectionWorldValid);
    }

    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;
}

void VR::ClearRoomscale1To1ServerMoveDelta()
{
    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;
    visualState.pendingServerVisualCorrectionWorld = {};
    visualState.pendingServerVisualCorrectionWorldValid = false;
}

namespace
{
    constexpr unsigned int kTeleportPathTraceMask =
        CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE;
    // Landing validation intentionally ignores dynamic actors and movable props.
    // They are transient and made otherwise-valid flat ground fail unpredictably.
    // Keep static world, playerclip, window and grate collision so real teleports
    // still cannot be committed inside map geometry.
    constexpr unsigned int kTeleportLandingStaticTraceMask =
        CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE;
    constexpr float kTeleportArcGravityMetersPerSecondSq = 9.81f;
    constexpr float kTeleportArcStepSeconds = 0.055f;
    constexpr float kTeleportArcNominalTravelSeconds = 0.90f;
    // Start the floor probe only slightly above the candidate. Starting 128 units
    // above it crosses ordinary indoor ceilings, bridge undersides and vehicle roofs,
    // causing those overhead surfaces to be mistaken for the landing floor.
    constexpr float kTeleportLandingFloorSearchUpUnits = 4.0f;
    constexpr float kTeleportLandingFloorSearchDownUnits = 256.0f;
    constexpr float kTeleportLandingFloorLiftUnits = 2.0f;
    constexpr float kTeleportCeilingSlideEpsilonUnits = 1.0f;
    constexpr float kTeleportTwoPi = 6.28318530717958647692f;

    bool IsFiniteTeleportVector(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool NormalizeTeleportVectorInPlace(Vector& value)
    {
        const float lengthSq = value.LengthSqr();
        if (!std::isfinite(lengthSq) || lengthSq <= 0.000001f)
            return false;

        value *= (1.0f / std::sqrt(lengthSq));
        return IsFiniteTeleportVector(value);
    }

    void TraceTeleportClientRay(IEngineTrace* engineTrace, const Ray_t& ray, unsigned int mask, CTraceFilter* filter, CGameTrace& outTrace)
    {
        outTrace.fraction = 1.0f;
        outTrace.allsolid = false;
        outTrace.startsolid = false;
        outTrace.m_pEnt = nullptr;
        if (engineTrace)
            engineTrace->TraceRay(ray, mask, filter, &outTrace);
    }

    bool IsTeleportWalkableFloor(const CGameTrace& trace)
    {
        // The floor probe itself travels vertically downward, so it already rejects walls.
        // Do not reject steep ramps: L4D2 maps may use sloped walkable collision surfaces.
        return !trace.allsolid && !trace.startsolid && trace.fraction < 0.999f;
    }

    bool ValidateTeleportLandingClient(VR* vr, C_BasePlayer* localPlayer, const Vector& surfacePoint, Vector& outLandingPoint)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_EngineTrace || !localPlayer || !IsFiniteTeleportVector(surfacePoint))
            return false;

        IEngineTrace* engineTrace = vr->m_Game->m_EngineTrace;
        CTraceFilterSkipSelf filter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);

        Ray_t floorRay;
        floorRay.Init(
            surfacePoint + Vector(0.0f, 0.0f, kTeleportLandingFloorSearchUpUnits),
            surfacePoint - Vector(0.0f, 0.0f, kTeleportLandingFloorSearchDownUnits));
        CGameTrace floorTrace{};
        TraceTeleportClientRay(engineTrace, floorRay, kTeleportLandingStaticTraceMask, &filter, floorTrace);
        if (!IsTeleportWalkableFloor(floorTrace))
            return false;

        const bool crouched = vr->m_Roomscale1To1PhysicalCrouchActive;
        const float hullHeight = crouched ? 36.0f : 72.0f;
        // Use a slightly inset occupancy hull. Source movement resolves small contact
        // tolerances itself; requiring a zero-tolerance 32x32x72 static fit rejects
        // valid flat ground beside seams, tiny props and accumulated trace epsilon.
        const Vector occupancyMins(-14.0f, -14.0f, 1.0f);
        const Vector occupancyMaxs(14.0f, 14.0f, hullHeight - 1.0f);

        // Validate occupancy from the floor upward instead of sweeping the full hull
        // down from far above the destination. A high sweep intersects nearby ceilings
        // before it reaches an otherwise valid indoor floor. The small upward search
        // below still resolves ramp contact tolerances without depending on overhead
        // clearance beyond the player's actual hull height.
        Vector landingPoint = floorTrace.endpos + Vector(0.0f, 0.0f, kTeleportLandingFloorLiftUnits);
        if (!IsFiniteTeleportVector(landingPoint))
            return false;

        Vector playerOrigin{};
#ifdef _MSC_VER
        __try
        {
            playerOrigin = localPlayer->GetAbsOrigin();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        playerOrigin = localPlayer->GetAbsOrigin();
#endif
        if (!IsFiniteTeleportVector(playerOrigin))
            return false;

        const float scale = (std::max)(1.0f, std::fabs(vr->m_VRScale));
        const Vector playerToTarget = landingPoint - playerOrigin;
        const float distance = playerToTarget.Length();
        const float teleportMaxDistanceMeters = std::clamp(vr->m_TeleportMaxDistanceMeters, 0.25f, 50.0f);
        if (!std::isfinite(distance) || distance > teleportMaxDistanceMeters * scale)
            return false;

        bool occupancyClear = false;
        for (int liftStep = 0; liftStep <= 24; ++liftStep)
        {
            Vector candidate = landingPoint + Vector(0.0f, 0.0f, static_cast<float>(liftStep));
            Ray_t occupancyRay;
            occupancyRay.Init(candidate, candidate, occupancyMins, occupancyMaxs);
            CGameTrace occupancyTrace{};
            TraceTeleportClientRay(engineTrace, occupancyRay, kTeleportLandingStaticTraceMask, &filter, occupancyTrace);
            if (!occupancyTrace.startsolid && !occupancyTrace.allsolid && occupancyTrace.fraction >= 0.999f)
            {
                landingPoint = candidate;
                occupancyClear = true;
                break;
            }
        }
        if (!occupancyClear)
            return false;

        outLandingPoint = landingPoint;
        return true;
    }

    void DrawTeleportPreview(VR* vr)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_DebugOverlay || vr->m_TeleportArcPointCount < 2)
            return;

        const bool valid = vr->m_TeleportTargetValid;
        const bool visualScoutOnly = valid && vr->m_TeleportTargetVisualScoutOnly;
        const int r = valid ? (visualScoutOnly ? 48 : 32) : 255;
        const int g = valid ? (visualScoutOnly ? 180 : 220) : 64;
        const int b = valid ? (visualScoutOnly ? 255 : 120) : 48;
        const float duration = (std::max)(0.035f, vr->m_LastFrameDuration * 2.0f);

        for (int i = 1; i < vr->m_TeleportArcPointCount; ++i)
        {
            vr->m_Game->m_DebugOverlay->AddLineOverlay(
                vr->m_TeleportArcPoints[i - 1],
                vr->m_TeleportArcPoints[i],
                r, g, b, true, duration);
        }

        const Vector center = vr->m_TeleportMarkerWorld;
        if (!IsFiniteTeleportVector(center))
            return;

        constexpr int kRingSegments = 24;
        constexpr float kRingRadius = 16.0f;
        for (int i = 0; i < kRingSegments; ++i)
        {
            const float a0 = (static_cast<float>(i) / static_cast<float>(kRingSegments)) * kTeleportTwoPi;
            const float a1 = (static_cast<float>(i + 1) / static_cast<float>(kRingSegments)) * kTeleportTwoPi;
            const Vector p0 = center + Vector(std::cos(a0) * kRingRadius, std::sin(a0) * kRingRadius, 1.0f);
            const Vector p1 = center + Vector(std::cos(a1) * kRingRadius, std::sin(a1) * kRingRadius, 1.0f);
            vr->m_Game->m_DebugOverlay->AddLineOverlay(p0, p1, r, g, b, true, duration);
        }

        vr->m_Game->m_DebugOverlay->AddLineOverlay(
            center + Vector(0.0f, 0.0f, 2.0f),
            center + Vector(0.0f, 0.0f, valid ? 26.0f : 14.0f),
            r, g, b, true, duration);

        Vector facing = vr->m_HmdForward;
        facing.z = 0.0f;
        if (NormalizeTeleportVectorInPlace(facing))
        {
            const float yaw = vr->m_TeleportFacingYawOffset * (kTeleportTwoPi / 360.0f);
            const float c = std::cos(yaw);
            const float s = std::sin(yaw);
            const Vector oriented(
                (facing.x * c) - (facing.y * s),
                (facing.x * s) + (facing.y * c),
                0.0f);
            const Vector arrowBase = center + Vector(0.0f, 0.0f, 2.0f);
            const Vector arrowTip = arrowBase + (oriented * 28.0f);
            const Vector arrowSide(-oriented.y, oriented.x, 0.0f);
            vr->m_Game->m_DebugOverlay->AddLineOverlay(arrowBase, arrowTip, r, g, b, true, duration);
            vr->m_Game->m_DebugOverlay->AddLineOverlay(arrowTip, arrowTip - (oriented * 8.0f) + (arrowSide * 5.0f), r, g, b, true, duration);
            vr->m_Game->m_DebugOverlay->AddLineOverlay(arrowTip, arrowTip - (oriented * 8.0f) - (arrowSide * 5.0f), r, g, b, true, duration);
        }
    }
}

bool VR::ShouldUseTeleportServerMove() const
{
    const bool hasServerEntityMove =
        m_Game &&
        m_Game->m_EngineTraceServer &&
        m_Game->m_Offsets &&
        m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.address &&
        m_Game->m_Offsets->CBaseEntity_SetOrigin_Server.address;

    return m_IsVREnabled &&
        Hooks::s_ServerUnderstandsVR &&
        !m_ForceNonVRServerMovement &&
        hasServerEntityMove;
}

void VR::BeginTeleportTargeting()
{
    if (!m_IsVREnabled)
        return;

    // While scouting, Teleport starts another detached-camera relocation. Use is
    // the only normal return-to-body command, so scouting can be chained indefinitely.

    m_TeleportTargetingActive = true;
    m_TeleportCommitPending = false;
    m_TeleportTargetValid = false;
    m_TeleportTargetVisualScoutOnly = false;
    m_TeleportArcPointCount = 0;
    m_TeleportFacingYawOffset = 0.0f;
    m_TeleportFacingTurnPressed = false;
    ClearTeleportServerTarget();
    CancelRoomscale1To1ServerMoveDelta();
    m_Roomscale1To1PrevValid = false;
}

void VR::EndTeleportTargeting(bool commit)
{
    if (!m_TeleportTargetingActive)
        return;

    if (commit)
    {
        m_TeleportCommitPending = true;
        return;
    }

    CancelTeleportTargeting();
}

void VR::CancelTeleportTargeting()
{
    m_TeleportTargetingActive = false;
    m_TeleportCommitPending = false;
    m_TeleportTargetValid = false;
    m_TeleportTargetVisualScoutOnly = false;
    m_TeleportArcPointCount = 0;
    m_TeleportFacingYawOffset = 0.0f;
    m_TeleportFacingTurnPressed = false;
}

void VR::QueueTeleportServerTarget(const Vector& targetWorld)
{
    if (!IsFiniteTeleportVector(targetWorld))
        return;

    CancelRoomscale1To1ServerMoveDelta();
    std::lock_guard<std::mutex> lock(m_TeleportServerMoveMutex);
    m_TeleportPendingServerTarget = targetWorld;
    m_TeleportPendingServerTargetValid = true;
}

bool VR::ConsumeTeleportServerTarget(Vector& outTargetWorld)
{
    std::lock_guard<std::mutex> lock(m_TeleportServerMoveMutex);
    if (!m_TeleportPendingServerTargetValid)
        return false;

    outTargetWorld = m_TeleportPendingServerTarget;
    m_TeleportPendingServerTarget = {};
    m_TeleportPendingServerTargetValid = false;
    return IsFiniteTeleportVector(outTargetWorld);
}

void VR::ClearTeleportServerTarget()
{
    std::lock_guard<std::mutex> lock(m_TeleportServerMoveMutex);
    m_TeleportPendingServerTarget = {};
    m_TeleportPendingServerTargetValid = false;
}

void VR::SuppressTeleportViewmodelForMs(uint32_t durationMs)
{
    if (durationMs == 0)
        return;

    const uint32_t until = static_cast<uint32_t>(::GetTickCount()) + durationMs;
    uint32_t current = m_TeleportViewmodelSuppressUntilMs.load(std::memory_order_relaxed);
    while (static_cast<int32_t>(until - current) > 0 &&
        !m_TeleportViewmodelSuppressUntilMs.compare_exchange_weak(
            current, until, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

bool VR::ShouldSuppressTeleportViewmodelRender() const
{
    const uint32_t until = m_TeleportViewmodelSuppressUntilMs.load(std::memory_order_relaxed);
    const uint32_t now = static_cast<uint32_t>(::GetTickCount());
    return static_cast<int32_t>(until - now) > 0;
}

void VR::ClearTeleportVisualScout()
{
    m_TeleportVisualScoutActive = false;
    m_TeleportVisualScoutUseExitArmed = false;
    m_TeleportVisualScoutCameraAnchor = {};
    m_TeleportVisualScoutReturnRotationOffset = 0.0f;
    m_TeleportVisualScoutBodyViewAngles = {};
    m_TeleportVisualScoutBodyViewAnglesValid = false;
}

void VR::EnterTeleportVisualScout(const Vector& targetWorld)
{
    if (!IsFiniteTeleportVector(targetWorld) || !m_Game || !m_Game->m_EngineClient)
        return;

    const int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = (localIndex > 0)
        ? reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localIndex))
        : nullptr;
    if (!localPlayer)
        return;

    // Visual scouting targets the rendered HMD observation point directly. Unlike a
    // real teleport landing target, it is not a player-origin / feet position.
    if (!IsFiniteTeleportVector(m_HmdPosAbs) || !IsFiniteTeleportVector(m_HmdPosLocalInWorld))
        return;

    const Vector scoutDelta = targetWorld - m_HmdPosAbs;
    const float scoutDistance = scoutDelta.Length();
    if (!std::isfinite(scoutDistance) || scoutDistance <= 0.01f)
        return;

    const bool relocatingExistingScout = m_TeleportVisualScoutActive;
    if (!relocatingExistingScout)
    {
        m_TeleportVisualScoutReturnRotationOffset = m_RotationOffset;
        m_TeleportVisualScoutBodyViewAngles = {};
        m_TeleportVisualScoutBodyViewAnglesValid = false;
        if (m_Game->m_EngineClient)
        {
            QAngle bodyAngles{};
            m_Game->m_EngineClient->GetViewAngles(bodyAngles);
            if (std::isfinite(bodyAngles.x) && std::isfinite(bodyAngles.y) && std::isfinite(bodyAngles.z))
            {
                m_TeleportVisualScoutBodyViewAngles = bodyAngles;
                m_TeleportVisualScoutBodyViewAnglesValid = true;
            }
        }

        // If Use was held as a playerclip-bypass modifier when scouting began, require
        // it to be released once before it can act as the return-to-body command.
        m_TeleportVisualScoutUseExitArmed = !PressedDigitalAction(m_ActionUse);
    }
    // Anchor the selected world point to the rendered HMD itself. Do not carry a
    // stale pre-scout roomscale offset into the detached camera basis.
    m_TeleportVisualScoutCameraAnchor =
        targetWorld + Vector(0.0f, 0.0f, 64.0f) - m_HmdPosLocalInWorld;
    m_TeleportPendingCameraPlanarRecenterTarget = {};
    m_TeleportPendingCameraPlanarRecenterValid = false;
    m_TeleportCameraClipSuppressFrames = 0;
    m_TeleportVisualScoutActive = true;
    SuppressTeleportViewmodelForMs(TELEPORT_VIEWMODEL_SUPPRESS_MS);

    // A scout camera is intentionally detached from the server origin. Clear all
    // server-move and delayed engine-origin compensation state so neither path can
    // pull the camera back or apply the offset twice.
    ClearTeleportServerTarget();
    ClearRoomscale1To1ServerMoveDelta();
    m_TeleportPendingEngineAnchorWorldDelta = {};
    m_TeleportEngineAnchorCompensationFrames = 0;
    m_AutoResetPositionPending = false;
    m_ResetPositionAfterObserverTargetSwitchPending = false;
    m_ResetPositionAfterMountedGunExitPending = false;
    m_Roomscale1To1PrevValid = false;
    m_Roomscale1To1LastEngineEyeValid = false;
    m_Roomscale1To1PendingVisualWorldDelta = {};
    m_Roomscale1To1PendingVisualWorldDeltaValid = false;
}

void VR::ExitTeleportVisualScout()
{
    if (!m_TeleportVisualScoutActive)
        return;

    const float returnRotationOffset = m_TeleportVisualScoutReturnRotationOffset;
    ClearTeleportVisualScout();
    m_TeleportPendingCameraPlanarRecenterTarget = {};
    m_TeleportPendingCameraPlanarRecenterValid = false;
    m_TeleportCameraClipSuppressFrames = 0;
    SuppressTeleportViewmodelForMs(TELEPORT_VIEWMODEL_SUPPRESS_MS);

    // Recenter directly on the current authoritative body origin. Do not route this
    // through ResetPosition(): the detached scout Z offset must not leak into HeightOffset.
    if (!m_SetupOrigin.IsZero() && IsFiniteTeleportVector(m_SetupOrigin) && IsFiniteTeleportVector(m_HmdPosLocalInWorld))
    {
        m_CameraAnchor = m_SetupOrigin + Vector(0.0f, 0.0f, 64.0f) - m_HmdPosLocalInWorld;
        m_HeightOffset = m_CameraAnchor.z - m_SetupOrigin.z;
        m_HmdPosAbs = m_CameraAnchor - Vector(0.0f, 0.0f, 64.0f) + m_HmdPosLocalInWorld;
        m_HmdPosAbsPrev = m_HmdPosAbs;
        m_SetupOriginToHMD = m_HmdPosAbs - m_SetupOrigin;
    }

    m_RotationOffset = returnRotationOffset;
    m_RotationOffset -= 360.0f * std::floor(m_RotationOffset / 360.0f);
    m_TeleportPendingEngineAnchorWorldDelta = {};
    m_TeleportEngineAnchorCompensationFrames = 0;
    m_AutoResetPositionPending = false;
    m_ResetPositionAfterObserverTargetSwitchPending = false;
    m_ResetPositionAfterMountedGunExitPending = false;
    m_Roomscale1To1PrevValid = false;
    m_Roomscale1To1LastEngineEyeValid = false;
    m_Roomscale1To1PendingVisualWorldDelta = {};
    m_Roomscale1To1PendingVisualWorldDeltaValid = false;
    ClearRoomscale1To1ServerMoveDelta();
    CancelTeleportTargeting();
}

void VR::QueueTeleportVisualWorldDelta(const Vector& worldDelta, const Vector& landingTarget)
{
    if (!IsFiniteTeleportVector(worldDelta) || !IsFiniteTeleportVector(landingTarget))
        return;

    const float len = worldDelta.Length();
    if (!std::isfinite(len) || len <= 0.01f)
        return;

    std::lock_guard<std::mutex> lock(m_TeleportServerMoveMutex);
    m_TeleportPendingVisualWorldDelta += worldDelta;
    m_TeleportPendingVisualWorldDeltaValid = true;
    m_TeleportPendingVisualLandingTarget = landingTarget;
    m_TeleportPendingVisualLandingTargetValid = true;
}

void VR::ApplyPendingTeleportVisualWorldDelta()
{
    Vector worldDelta{};
    Vector landingTarget{};
    bool landingTargetValid = false;
    {
        std::lock_guard<std::mutex> lock(m_TeleportServerMoveMutex);
        if (!m_TeleportPendingVisualWorldDeltaValid)
            return;

        worldDelta = m_TeleportPendingVisualWorldDelta;
        landingTarget = m_TeleportPendingVisualLandingTarget;
        landingTargetValid = m_TeleportPendingVisualLandingTargetValid;
        m_TeleportPendingVisualWorldDelta = {};
        m_TeleportPendingVisualWorldDeltaValid = false;
        m_TeleportPendingVisualLandingTarget = {};
        m_TeleportPendingVisualLandingTargetValid = false;
    }

    if (!IsFiniteTeleportVector(worldDelta))
        return;

    SuppressTeleportViewmodelForMs(TELEPORT_VIEWMODEL_SUPPRESS_MS);
    ClearTeleportVisualScout();
    m_CameraAnchor += worldDelta;
    m_HmdPosAbs += worldDelta;
    m_HmdPosAbsPrev += worldDelta;
    m_SetupOriginToHMD += worldDelta;
    m_LeftControllerPosAbs += worldDelta;
    m_RightControllerPosAbs += worldDelta;

    // Recenter after UpdateTracking has refreshed m_HmdPosLocalInWorld for the
    // current frame. Using the previous tracking sample here can push the camera
    // beyond the accepted landing point when the user is looking forward or sideways.
    if (landingTargetValid && IsFiniteTeleportVector(landingTarget))
    {
        m_TeleportPendingCameraPlanarRecenterTarget = landingTarget;
        m_TeleportPendingCameraPlanarRecenterValid = true;
        m_TeleportCameraClipSuppressFrames = 12;
    }

    if (IsFiniteTeleportVector(m_SetupOrigin))
        m_SetupOriginToHMD = m_HmdPosAbs - m_SetupOrigin;

    // The client origin update caused by server SetOrigin may be observed slightly later.
    // Consume that matching engine-origin delta once so the visual anchor is not shifted twice.
    m_TeleportPendingEngineAnchorWorldDelta += worldDelta;
    m_TeleportEngineAnchorCompensationFrames = 12;

    m_Roomscale1To1PrevValid = false;
    m_Roomscale1To1LastEngineEyeValid = false;
    m_Roomscale1To1PendingVisualWorldDelta = {};
    m_Roomscale1To1PendingVisualWorldDeltaValid = false;
    ClearRoomscale1To1ServerMoveDelta();
}

void VR::ConsumeTeleportEngineAnchorCompensation(Vector& engineOriginDelta)
{
    if (m_TeleportEngineAnchorCompensationFrames <= 0)
        return;

    --m_TeleportEngineAnchorCompensationFrames;
    Vector expected = m_TeleportPendingEngineAnchorWorldDelta;
    if (!IsFiniteTeleportVector(expected) || expected.Length() <= 0.01f)
    {
        m_TeleportPendingEngineAnchorWorldDelta = {};
        m_TeleportEngineAnchorCompensationFrames = 0;
        return;
    }

    const float expectedLenSq = expected.LengthSqr();
    const float originLen = engineOriginDelta.Length();
    if (expectedLenSq > 0.01f && std::isfinite(originLen) && originLen > 0.5f)
    {
        const float projection = engineOriginDelta.Dot(expected) / expectedLenSq;
        if (std::isfinite(projection) && projection > 0.05f)
        {
            const float consumeScale = std::clamp(projection, 0.0f, 1.0f);
            const Vector consumed = expected * consumeScale;
            engineOriginDelta -= consumed;
            m_TeleportPendingEngineAnchorWorldDelta -= consumed;
        }
    }

    if (m_TeleportPendingEngineAnchorWorldDelta.Length() <= 0.5f || m_TeleportEngineAnchorCompensationFrames <= 0)
    {
        m_TeleportPendingEngineAnchorWorldDelta = {};
        m_TeleportEngineAnchorCompensationFrames = 0;
    }
}

void VR::UpdateTeleportTargeting()
{
    if (!m_TeleportTargetingActive)
        return;

    m_TeleportTargetValid = false;
    m_TeleportTargetVisualScoutOnly = false;
    m_TeleportArcPointCount = 0;
    CancelRoomscale1To1ServerMoveDelta();
    m_Roomscale1To1PrevValid = false;

    C_BasePlayer* localPlayer = nullptr;
    if (m_Game && m_Game->m_EngineClient && m_Game->m_ClientEntityList)
    {
        const int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
        if (localIndex > 0)
            localPlayer = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localIndex));
    }

    bool canTrace = localPlayer != nullptr && m_Game && m_Game->m_EngineTrace;
    bool canCommit = localPlayer != nullptr && !m_PlayerControlledBySI && !m_UsingMountedGunPrev;
    if (localPlayer)
    {
        const auto* base = reinterpret_cast<const std::uint8_t*>(localPlayer);
        const int lifeState = static_cast<int>(*reinterpret_cast<const std::uint8_t*>(base + kLifeStateOffset));
        const int observerMode = *reinterpret_cast<const int*>(base + kObserverModeOffset);
        const bool incapacitated = *reinterpret_cast<const std::uint8_t*>(base + kIsIncapacitatedOffset) != 0;
        const bool hangingFromLedge = *reinterpret_cast<const std::uint8_t*>(base + kIsHangingFromLedgeOffset) != 0;
        const bool playerStateValid = lifeState == 0 && observerMode == 0;
        canTrace = canTrace && playerStateValid;
        canCommit = canCommit && playerStateValid && !incapacitated && !hangingFromLedge;
    }

    vr::InputAnalogActionData_t facingTurn{};
    if (GetAnalogActionData(m_ActionTurn, facingTurn))
    {
        if (m_SnapTurning)
        {
            if (!m_TeleportFacingTurnPressed && facingTurn.x > 0.5f)
            {
                m_TeleportFacingYawOffset += m_SnapTurnAngle;
                m_TeleportFacingTurnPressed = true;
            }
            else if (!m_TeleportFacingTurnPressed && facingTurn.x < -0.5f)
            {
                m_TeleportFacingYawOffset -= m_SnapTurnAngle;
                m_TeleportFacingTurnPressed = true;
            }
            else if (facingTurn.x < 0.3f && facingTurn.x > -0.3f)
            {
                m_TeleportFacingTurnPressed = false;
            }
        }
        else
        {
            constexpr float kDeadzone = 0.2f;
            const float absX = std::fabs(facingTurn.x);
            if (absX > kDeadzone)
            {
                const float normalized = (absX - kDeadzone) / (1.0f - kDeadzone);
                const float signedNormalized = (facingTurn.x < 0.0f) ? -normalized : normalized;
                m_TeleportFacingYawOffset += signedNormalized * m_TurnSpeed * (m_LastFrameDuration * 1000.0f);
            }
        }
        while (m_TeleportFacingYawOffset > 180.0f) m_TeleportFacingYawOffset -= 360.0f;
        while (m_TeleportFacingYawOffset < -180.0f) m_TeleportFacingYawOffset += 360.0f;
    }

    Vector direction = m_RightControllerForwardUnforced;
    if (direction.IsZero() || !IsFiniteTeleportVector(direction))
        direction = m_RightControllerForward;
    NormalizeTeleportVectorInPlace(direction);

    const float scale = (std::max)(1.0f, std::fabs(m_VRScale));
    const float teleportMaxDistanceMeters = std::clamp(m_TeleportMaxDistanceMeters, 0.25f, 50.0f);
    const float teleportMaxDistanceUnits = teleportMaxDistanceMeters * scale;
    // Use the runtime distance parameter as the sole range source. Keep a fixed nominal
    // travel time and derive velocity linearly from the configured maximum so preview,
    // client validation and server validation remain consistent.
    const float teleportArcSpeedMetersPerSecond =
        teleportMaxDistanceMeters / kTeleportArcNominalTravelSeconds;
    Vector current = m_RightControllerPosAbs;
    m_TeleportArcPoints[0] = current;
    m_TeleportArcPointCount = 1;
    m_TeleportMarkerWorld = current;

    const bool canUseServerMove = ShouldUseTeleportServerMove();
    const bool visualScoutOnly =
        m_TeleportVisualScoutActive ||
        (!canUseServerMove && m_TeleportVisualScoutOnNonVRServerEnabled);
    if (visualScoutOnly)
    {
        // Camera-only scouting is intentionally collision-free. It is a probe view, not
        // authoritative locomotion: walls, ceilings, low spaces and missing floor support
        // must not block the preview. Keep a straight controller ray so the destination is
        // deterministic even when the target is inside geometry or behind multiple walls.
        if (localPlayer && canCommit &&
            IsFiniteTeleportVector(current) && IsFiniteTeleportVector(direction) && !direction.IsZero())
        {
            const Vector scoutTarget = current + (direction * teleportMaxDistanceUnits);
            if (IsFiniteTeleportVector(scoutTarget))
            {
                m_TeleportArcPoints[m_TeleportArcPointCount++] = scoutTarget;
                m_TeleportTargetWorld = scoutTarget;
                m_TeleportMarkerWorld = scoutTarget;
                m_TeleportTargetValid = true;
                m_TeleportTargetVisualScoutOnly = true;
            }
        }
    }
    else
    {
        Vector velocity = direction * (teleportArcSpeedMetersPerSecond * scale);
        const Vector gravity(0.0f, 0.0f, -kTeleportArcGravityMetersPerSecondSq * scale);
        // Holding Use together with Teleport ignores playerclip barriers only. Real
        // solid walls, windows and grates still stop the arc, and the destination
        // still has to pass the normal landing occupancy validation.
        const bool ignorePlayerClip = PressedDigitalAction(m_ActionUse);
        const unsigned int pathTraceMask = ignorePlayerClip
            ? (kTeleportPathTraceMask & ~CONTENTS_PLAYERCLIP)
            : kTeleportPathTraceMask;
        bool hitSurface = false;
        Vector hitPoint = current;
        float traveledDistanceUnits = 0.0f;
        if (canTrace && IsFiniteTeleportVector(current) && IsFiniteTeleportVector(direction) && !direction.IsZero())
        {
            CTraceFilterSkipNPCsAndPlayers pathFilter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);
            for (int i = 0; i < TELEPORT_ARC_SEGMENTS; ++i)
            {
                Vector next = current + (velocity * kTeleportArcStepSeconds) + (gravity * (0.5f * kTeleportArcStepSeconds * kTeleportArcStepSeconds));
                velocity += gravity * kTeleportArcStepSeconds;

                bool reachedMaxDistance = false;
                const Vector segment = next - current;
                const float segmentDistance = segment.Length();
                if (!std::isfinite(segmentDistance) || segmentDistance <= 0.0001f)
                    break;

                const float remainingDistance = teleportMaxDistanceUnits - traveledDistanceUnits;
                if (remainingDistance <= 0.0001f)
                    break;
                if (segmentDistance > remainingDistance)
                {
                    next = current + (segment * (remainingDistance / segmentDistance));
                    reachedMaxDistance = true;
                }

                Ray_t pathRay;
                pathRay.Init(current, next);
                CGameTrace pathTrace{};
                TraceTeleportClientRay(m_Game->m_EngineTrace, pathRay, pathTraceMask, &pathFilter, pathTrace);

                const Vector renderedEnd = (pathTrace.fraction < 0.999f) ? pathTrace.endpos : next;
                const float renderedDistance = (renderedEnd - current).Length();
                if (std::isfinite(renderedDistance))
                    traveledDistanceUnits += renderedDistance;
                m_TeleportArcPoints[m_TeleportArcPointCount++] = renderedEnd;
                m_TeleportMarkerWorld = renderedEnd;

                if (pathTrace.allsolid || pathTrace.startsolid || pathTrace.fraction < 0.999f)
                {
                    const bool hitCeilingUnderside =
                        !pathTrace.allsolid &&
                        !pathTrace.startsolid &&
                        pathTrace.fraction < 0.999f &&
                        IsFiniteTeleportVector(pathTrace.plane.normal) &&
                        pathTrace.plane.normal.z < -0.5f;
                    if (hitCeilingUnderside)
                    {
                        // Low ceilings should constrain the arc rather than terminate
                        // teleport targeting. Remove only the velocity component pushing
                        // into the underside, nudge back into free space, then let gravity
                        // carry the trajectory forward and downward. Solid walls still stop it.
                        const float inwardVelocity = velocity.Dot(pathTrace.plane.normal);
                        if (std::isfinite(inwardVelocity) && inwardVelocity < 0.0f)
                            velocity -= pathTrace.plane.normal * inwardVelocity;

                        current = pathTrace.endpos + (pathTrace.plane.normal * kTeleportCeilingSlideEpsilonUnits);
                        m_TeleportArcPoints[m_TeleportArcPointCount - 1] = current;
                        m_TeleportMarkerWorld = current;
                        continue;
                    }

                    hitSurface = !pathTrace.allsolid && !pathTrace.startsolid;
                    hitPoint = pathTrace.endpos;
                    break;
                }

                current = next;
                if (reachedMaxDistance)
                    break;
            }
        }

        Vector landingPoint{};
        bool landingValid = hitSurface && ValidateTeleportLandingClient(this, localPlayer, hitPoint, landingPoint);
        // Allow the preview endpoint to hover above a valid floor. This is useful on
        // second-storey floors, balcony edges and other places where the arc stops in
        // open air slightly above the walkable surface instead of colliding with it.
        if (!landingValid && m_TeleportArcPointCount > 1)
            landingValid = ValidateTeleportLandingClient(this, localPlayer, m_TeleportMarkerWorld, landingPoint);

        if (landingValid)
        {
            m_TeleportTargetWorld = landingPoint;
            m_TeleportMarkerWorld = landingPoint;
            m_TeleportTargetValid = canCommit;
            m_TeleportTargetVisualScoutOnly = false;
        }
    }

    DrawTeleportPreview(this);

    if (m_TeleportCommitPending)
    {
        if (m_TeleportTargetValid)
        {
            if (m_TeleportTargetVisualScoutOnly)
                EnterTeleportVisualScout(m_TeleportTargetWorld);
            else
                QueueTeleportServerTarget(m_TeleportTargetWorld);

            m_RotationOffset -= m_TeleportFacingYawOffset;
            m_RotationOffset -= 360.0f * std::floor(m_RotationOffset / 360.0f);
        }
        CancelTeleportTargeting();
    }
}

void VR::ApplyRoomscale1To1Move(CUserCmd* cmd, float inputSampleTime, bool controlLocomotionActive)
{
    const Vector cur = m_HmdPosCorrectedPrev;

    auto resetReference = [&]()
        {
            m_Roomscale1To1PrevCorrectedAbs = cur;
            m_Roomscale1To1PrevValid = true;
        };

    m_Roomscale1To1PendingVisualWorldDelta = {};
    m_Roomscale1To1PendingVisualWorldDeltaValid = false;

    if (!cmd || !m_Roomscale1To1Movement || m_TeleportTargetingActive || m_TeleportVisualScoutActive)
    {
        m_Roomscale1To1PrevValid = false;
        CancelRoomscale1To1ServerMoveDelta();
        m_Roomscale1To1StandingHmdZValid = false;
        m_Roomscale1To1PhysicalCrouchActive = false;
        return;
    }

    constexpr int kIN_DUCK = (1 << 2);
    if (m_Roomscale1To1PhysicalCrouch)
    {
        if (!m_Roomscale1To1StandingHmdZValid)
        {
            m_Roomscale1To1StandingHmdZ = cur.z;
            m_Roomscale1To1StandingHmdZValid = true;
            m_Roomscale1To1PhysicalCrouchActive = false;
        }

        if (!m_Roomscale1To1PhysicalCrouchActive && cur.z > m_Roomscale1To1StandingHmdZ)
            m_Roomscale1To1StandingHmdZ = cur.z;

        const float enterM = std::clamp(m_Roomscale1To1CrouchEnterMeters, 0.05f, 1.0f);
        const float exitM = std::clamp(m_Roomscale1To1CrouchExitMeters, 0.0f, enterM);
        const float crouchDepthM = m_Roomscale1To1StandingHmdZ - cur.z;
        if (!m_Roomscale1To1PhysicalCrouchActive && crouchDepthM >= enterM)
            m_Roomscale1To1PhysicalCrouchActive = true;
        else if (m_Roomscale1To1PhysicalCrouchActive && crouchDepthM <= exitM)
            m_Roomscale1To1PhysicalCrouchActive = false;

        if (m_Roomscale1To1PhysicalCrouchActive)
            cmd->buttons |= kIN_DUCK;
    }
    else
    {
        m_Roomscale1To1StandingHmdZValid = false;
        m_Roomscale1To1PhysicalCrouchActive = false;
    }

    if (m_Roomscale1To1DisableWhileThumbstick && controlLocomotionActive)
    {
        resetReference();
        CancelRoomscale1To1ServerMoveDelta();
        return;
    }

    if (!m_Roomscale1To1PrevValid)
    {
        resetReference();
        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
        {
            Game::logMsg("[VR][1to1][cmdmove] init cmd=%d tick=%d hmd=(%.3f %.3f %.3f)",
                cmd->command_number, cmd->tick_count, cur.x, cur.y, cur.z);
        }
        return;
    }

    Vector deltaM = cur - m_Roomscale1To1PrevCorrectedAbs;
    deltaM.z = 0.0f;
    m_Roomscale1To1PrevCorrectedAbs = cur;

    const float lenM = std::sqrt((deltaM.x * deltaM.x) + (deltaM.y * deltaM.y));
    const float noiseDeadzoneM = std::clamp(m_Roomscale1To1MinApplyMeters, 0.0f, 0.005f);
    if (lenM <= noiseDeadzoneM)
        return;

    const float movementScale = std::clamp(m_Roomscale1To1MovementScale, 0.0f, 4.0f);

    const float maxStepM = std::max(0.0f, m_Roomscale1To1MaxStepMeters);
    if (maxStepM > 0.0f && lenM > maxStepM)
    {
        const float s = maxStepM / lenM;
        deltaM.x *= s;
        deltaM.y *= s;
    }

    const Vector gameDeltaM(deltaM.x * movementScale, deltaM.y * movementScale, 0.0f);

    // The physical HMD pose has already moved the camera by deltaM. When the configured
    // roomscale multiplier is not 1.0, immediately rebase the visual camera by the
    // remaining scaled displacement. Server/client movement reconciliation must then
    // compare against the scaled visual delta, otherwise the player entity can move
    // farther or shorter than the rendered camera until ResetPosition is used.
    const Vector visualScaleCorrectionM = gameDeltaM - deltaM;
    const Vector visualScaleCorrectionWorld(
        visualScaleCorrectionM.x * m_VRScale,
        visualScaleCorrectionM.y * m_VRScale,
        0.0f);
    ApplyRoomscale1To1VisualWorldCorrection(visualScaleCorrectionWorld);

    m_Roomscale1To1PendingVisualWorldDelta = Vector(gameDeltaM.x * m_VRScale, gameDeltaM.y * m_VRScale, 0.0f);
    m_Roomscale1To1PendingVisualWorldDeltaValid =
        m_Roomscale1To1PendingVisualWorldDelta.Length() > 0.01f;

    if (ShouldUseRoomscale1To1ServerMove())
    {
        Vector serverWorldDelta(gameDeltaM.x * m_VRScale, gameDeltaM.y * m_VRScale, 0.0f);
        QueueRoomscale1To1ServerMoveDelta(serverWorldDelta, m_Roomscale1To1PendingVisualWorldDelta);
        m_Roomscale1To1PendingVisualWorldDelta = {};
        m_Roomscale1To1PendingVisualWorldDeltaValid = false;

        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
        {
            Game::logMsg("[VR][1to1][servermove-queue] cmd=%d tick=%d scale=%.3f hmdM=(%.3f %.3f) gameM=(%.3f %.3f) world=(%.1f %.1f)",
                cmd->command_number, cmd->tick_count,
                movementScale,
                deltaM.x, deltaM.y,
                gameDeltaM.x, gameDeltaM.y,
                serverWorldDelta.x, serverWorldDelta.y);
        }
        return;
    }

    if (movementScale <= 0.0001f)
        return;

    float dt = inputSampleTime;
    if (dt <= 0.0001f)
        dt = 1.0f / 30.0f;
    dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 15.0f);

    Vector roomWorldVelocity(gameDeltaM.x * m_VRScale / dt, gameDeltaM.y * m_VRScale / dt, 0.0f);
    const float roomSpeed = std::sqrt((roomWorldVelocity.x * roomWorldVelocity.x) + (roomWorldVelocity.y * roomWorldVelocity.y));
    const float maxCmdSpeed = (m_AdjustingViewmodel || m_AdjustingScope) ? 25.0f : 250.0f;
    if (roomSpeed > maxCmdSpeed && roomSpeed > 0.0001f)
    {
        const float s = maxCmdSpeed / roomSpeed;
        roomWorldVelocity.x *= s;
        roomWorldVelocity.y *= s;
    }

    QAngle cmdYawOnly(0.0f, cmd->viewangles.y, 0.0f);
    Vector cmdForward, cmdRight, cmdUp;
    QAngle::AngleVectors(cmdYawOnly, &cmdForward, &cmdRight, &cmdUp);

    Vector worldMove = (cmdForward * cmd->forwardmove) + (cmdRight * cmd->sidemove);
    worldMove = worldMove + roomWorldVelocity;

    cmd->forwardmove = DotProduct(worldMove, cmdForward);
    cmd->sidemove = DotProduct(worldMove, cmdRight);

    constexpr int kIN_FORWARD = (1 << 3);
    constexpr int kIN_BACK = (1 << 4);
    constexpr int kIN_MOVELEFT = (1 << 9);
    constexpr int kIN_MOVERIGHT = (1 << 10);
    if (cmd->forwardmove > 0.5f)      cmd->buttons |= kIN_FORWARD;
    else if (cmd->forwardmove < -0.5f) cmd->buttons |= kIN_BACK;
    if (cmd->sidemove > 0.5f)         cmd->buttons |= kIN_MOVERIGHT;
    else if (cmd->sidemove < -0.5f)   cmd->buttons |= kIN_MOVELEFT;

    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][cmdmove] cmd=%d tick=%d dt=%.4f scale=%.3f hmdM=(%.3f %.3f) gameM=(%.3f %.3f) vel=(%.1f %.1f) move=(%.1f %.1f)",
            cmd->command_number, cmd->tick_count, dt,
            movementScale,
            deltaM.x, deltaM.y,
            gameDeltaM.x, gameDeltaM.y,
            roomWorldVelocity.x, roomWorldVelocity.y,
            cmd->forwardmove, cmd->sidemove);
    }
}

void VR::OnPredictionRunCommand(CUserCmd* cmd)
{
    (void)cmd;
}

void VR::SendVirtualKey(WORD virtualKey)
{
    SendVirtualKeyDown(virtualKey);
    SendVirtualKeyUp(virtualKey);
}

void VR::SendVirtualKeyDown(WORD virtualKey)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;

    SendInput(1, &input, sizeof(INPUT));
}

void VR::SendVirtualKeyUp(WORD virtualKey)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(1, &input, sizeof(INPUT));
}

void VR::SendFunctionKey(WORD virtualKey)
{
    SendVirtualKey(virtualKey);
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t& hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
}

vr::HmdMatrix34_t VR::VMatrixToHmdMatrix(const VMatrix& vMat)
{
    vr::HmdMatrix34_t hmdMat = { 0 };

    hmdMat.m[0][0] = vMat.m[0][0];
    hmdMat.m[1][0] = vMat.m[0][1];
    hmdMat.m[2][0] = vMat.m[0][2];

    hmdMat.m[0][1] = vMat.m[1][0];
    hmdMat.m[1][1] = vMat.m[1][1];
    hmdMat.m[2][1] = vMat.m[1][2];

    hmdMat.m[0][2] = vMat.m[2][0];
    hmdMat.m[1][2] = vMat.m[2][1];
    hmdMat.m[2][2] = vMat.m[2][2];

    hmdMat.m[0][3] = vMat.m[3][0];
    hmdMat.m[1][3] = vMat.m[3][1];
    hmdMat.m[2][3] = vMat.m[3][2];

    return hmdMat;
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String,
            buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = { 0 };
        vr::RenderModel_ComponentState_t componentState = { 0 };

        if (vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState))
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

    if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid)
        return false;

    vr::TrackedDevicePose_t& controllerPose = m_Poses[deviceIndex];

    if (!controllerPose.bPoseIsValid)
        return false;

    VMatrix controllerVMatrix = VMatrixFromHmdMatrix(controllerPose.mDeviceToAbsoluteTracking);
    VMatrix tipVMatrix = VMatrixFromHmdMatrix(GetControllerTipMatrix(controllerRole));
    tipVMatrix.MatrixMul(controllerVMatrix, controllerVMatrix);

    vr::VROverlayIntersectionParams_t  params = { 0 };
    vr::VROverlayIntersectionResults_t results = { 0 };

    params.eOrigin = vr::VRCompositor()->GetTrackingSpace();
    params.vSource = { controllerVMatrix.m[3][0],  controllerVMatrix.m[3][1],  controllerVMatrix.m[3][2] };
    params.vDirection = { -controllerVMatrix.m[2][0], -controllerVMatrix.m[2][1], -controllerVMatrix.m[2][2] };

    return m_Overlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}


namespace vr_render_snapshot_cache_roomscale
{
    struct HandCache
    {
        uint32_t seq = 0;
        Vector leftPos{ 0.0f, 0.0f, 0.0f };
        QAngle leftAng{ 0.0f, 0.0f, 0.0f };
        Vector rightPos{ 0.0f, 0.0f, 0.0f };
        QAngle rightAng{ 0.0f, 0.0f, 0.0f };
        Vector vmPos{ 0.0f, 0.0f, 0.0f };
        QAngle vmAng{ 0.0f, 0.0f, 0.0f };
    };

    inline HandCache& TLS()
    {
        static thread_local HandCache cache{};
        return cache;
    }

    inline bool Refresh(const VR* vr, HandCache& c)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const uint32_t s1 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u))
                continue;

            const float rpx = vr->m_RenderRightControllerPosAbsX.load(std::memory_order_relaxed);
            const float rpy = vr->m_RenderRightControllerPosAbsY.load(std::memory_order_relaxed);
            const float rpz = vr->m_RenderRightControllerPosAbsZ.load(std::memory_order_relaxed);

            const float rax = vr->m_RenderRightControllerAngAbsX.load(std::memory_order_relaxed);
            const float ray = vr->m_RenderRightControllerAngAbsY.load(std::memory_order_relaxed);
            const float raz = vr->m_RenderRightControllerAngAbsZ.load(std::memory_order_relaxed);

            const float lpx = vr->m_RenderLeftControllerPosAbsX.load(std::memory_order_relaxed);
            const float lpy = vr->m_RenderLeftControllerPosAbsY.load(std::memory_order_relaxed);
            const float lpz = vr->m_RenderLeftControllerPosAbsZ.load(std::memory_order_relaxed);

            const float lax = vr->m_RenderLeftControllerAngAbsX.load(std::memory_order_relaxed);
            const float lay = vr->m_RenderLeftControllerAngAbsY.load(std::memory_order_relaxed);
            const float laz = vr->m_RenderLeftControllerAngAbsZ.load(std::memory_order_relaxed);

            const float vpx = vr->m_RenderRecommendedViewmodelPosX.load(std::memory_order_relaxed);
            const float vpy = vr->m_RenderRecommendedViewmodelPosY.load(std::memory_order_relaxed);
            const float vpz = vr->m_RenderRecommendedViewmodelPosZ.load(std::memory_order_relaxed);

            const float vax = vr->m_RenderRecommendedViewmodelAngX.load(std::memory_order_relaxed);
            const float vay = vr->m_RenderRecommendedViewmodelAngY.load(std::memory_order_relaxed);
            const float vaz = vr->m_RenderRecommendedViewmodelAngZ.load(std::memory_order_relaxed);

            const uint32_t s2 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == s2 && !(s2 & 1u))
            {
                c.seq = s2;
                c.leftPos = Vector(lpx, lpy, lpz);
                c.leftAng = QAngle(lax, lay, laz);
                c.rightPos = Vector(rpx, rpy, rpz);
                c.rightAng = QAngle(rax, ray, raz);
                c.vmPos = Vector(vpx, vpy, vpz);
                c.vmAng = QAngle(vax, vay, vaz);
                return true;
            }
        }
        return false;
    }

    inline bool Get(const VR* vr, HandCache& c)
    {
        if (!VR::t_UseRenderFrameSnapshot)
            return false;

        const uint32_t s = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
        if (s != 0 && !(s & 1u) && s == c.seq)
            return true;

        return Refresh(vr, c);
    }
}

QAngle VR::GetLeftControllerAbsAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.leftAng;
    }

    return m_LeftControllerAngAbs;
}


Vector VR::GetLeftControllerAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.leftPos;
    }

    return m_LeftControllerPosAbs;
}


QAngle VR::GetRightControllerAbsAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.rightAng;
    }

    return m_RightControllerAngAbs;
}


Vector VR::GetRightControllerAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.rightPos;
    }

    return m_RightControllerPosAbs;
}


Vector VR::GetRightControllerViewmodelAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
        {
            Vector forward{}, right{}, up{};
            QAngle::AngleVectors(cache.vmAng, &forward, &right, &up);
            if (!forward.IsZero() && !right.IsZero() && !up.IsZero())
            {
                VectorNormalize(forward);
                VectorNormalize(right);
                VectorNormalize(up);
                return cache.vmPos
                    + (forward * m_ViewmodelPosOffset.x)
                    + (right * m_ViewmodelPosOffset.y)
                    + (up * m_ViewmodelPosOffset.z);
            }
        }
    }

    return m_RightControllerPosAbs;
}


Vector VR::GetRecommendedViewmodelAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.vmPos;
    }

    Vector viewmodelPos = GetRightControllerViewmodelAbsPos();
    if (m_MouseModeEnabled)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        viewmodelPos = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));
    }
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}


QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.vmAng;
    }

    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    return result;
}


void VR::HandleMissingRenderContext(const char* location)
{
    m_CreatedVRTextures.store(false, std::memory_order_release);
    m_QueuedEyeSubmitIsolationReady.store(false, std::memory_order_release);
    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_RenderedHud.store(false, std::memory_order_release);
    ClearQueuedHudFresh();
}

void VR::FinishFrame()
{
    if (!m_Compositor || !m_CompositorExplicitTiming)
        return;

    if (!m_CompositorNeedsHandoff)
        return;

    m_Compositor->PostPresentHandoff();
    m_CompositorNeedsHandoff = false;
}

void VR::GetMouseModeEyeRay(Vector& eyeDirOut, QAngle* eyeAngOut)
{
    eyeDirOut = { 0.0f, 0.0f, 0.0f };

    // Reset reference when not using HMD-driven aiming (so re-entering re-centers).
    if (!m_MouseModeEnabled || !m_MouseModeAimFromHmd)
        m_MouseModeHmdAimReferenceInitialized = false;

    QAngle eyeAng{ 0.0f, 0.0f, 0.0f };

    if (m_MouseModeAimFromHmd)
    {
        // Capture a reference direction the first frame HMD aiming becomes active.
        if (!m_MouseModeHmdAimReferenceInitialized)
        {
            // IMPORTANT:
            //  - m_HmdAngAbs already includes m_RotationOffset (mouse/body yaw).
            //  - MouseModeHmdAimSensitivity is meant to scale *HMD* deltas only.
            // If we capture/scale the full absolute angle, mouse yaw gets scaled too,
            // which makes MouseModeHmdAimSensitivity < 1 feel like mouse turning is "stuck",
            // and > 1 feel like mouse turning is too fast.
            //
            // So: keep the reference in "head-local" space (yaw with body yaw removed).
            m_MouseModeHmdAimReferenceAng = m_HmdAngAbs;
            m_MouseModeHmdAimReferenceAng.y -= m_RotationOffset;
            // Wrap yaw to [-180, 180]
            m_MouseModeHmdAimReferenceAng.y -= 360.0f * std::floor((m_MouseModeHmdAimReferenceAng.y + 180.0f) / 360.0f);
            m_MouseModeHmdAimReferenceAng.z = 0.0f;
            NormalizeAndClampViewAngles(m_MouseModeHmdAimReferenceAng);
            m_MouseModeHmdAimReferenceInitialized = true;
        }

        const float sens = std::clamp(m_MouseModeHmdAimSensitivity, 0.0f, 3.0f);

        // Scale pitch/yaw deltas around the captured reference.
        // Use "head-local" yaw (absolute yaw minus body yaw) so mouse turning stays 1:1.
        QAngle cur = m_HmdAngAbs;
        cur.y -= m_RotationOffset;
        // Wrap yaw to [-180, 180]
        cur.y -= 360.0f * std::floor((cur.y + 180.0f) / 360.0f);
        cur.z = 0.0f;
        NormalizeAndClampViewAngles(cur);

        auto wrapDelta = [](float deg)
            {
                deg -= 360.0f * std::floor((deg + 180.0f) / 360.0f);
                return deg;
            };

        const float dPitch = wrapDelta(cur.x - m_MouseModeHmdAimReferenceAng.x);
        const float dYaw = wrapDelta(cur.y - m_MouseModeHmdAimReferenceAng.y);

        eyeAng.x = m_MouseModeHmdAimReferenceAng.x + dPitch * sens;
        // Convert back to absolute yaw by re-applying body yaw.
        eyeAng.y = m_RotationOffset + (m_MouseModeHmdAimReferenceAng.y + dYaw * sens);
        eyeAng.z = 0.0f;
        NormalizeAndClampViewAngles(eyeAng);

        Vector right, up;
        QAngle::AngleVectors(eyeAng, &eyeDirOut, &right, &up);
        if (!eyeDirOut.IsZero())
            VectorNormalize(eyeDirOut);
    }
    else
    {
        // Default mouse-mode eye ray: mouse pitch + body yaw (m_RotationOffset).
        const float pitch = std::clamp(m_MouseAimPitchOffset, -89.f, 89.f);
        const float yaw = m_RotationOffset;
        eyeAng = QAngle(pitch, yaw, 0.f);
        NormalizeAndClampViewAngles(eyeAng);

        Vector right, up;
        QAngle::AngleVectors(eyeAng, &eyeDirOut, &right, &up);
        if (!eyeDirOut.IsZero())
            VectorNormalize(eyeDirOut);
    }

    if (eyeAngOut)
        *eyeAngOut = eyeAng;
}

