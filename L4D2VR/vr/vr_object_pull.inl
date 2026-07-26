namespace
{
    // Object Pull input is produced on the render/input path and consumed by
    // WriteUsercmd on the game/network path. Keep this state translation-unit
    // local so vr_object_pull.inl remains self-contained and cannot get out of
    // sync with VR class declarations.
    static std::atomic<uint32_t> g_ObjectPullWireMailbox{ 0u };
    static std::chrono::steady_clock::time_point g_ObjectPullNextTargetScanAt{};
    static std::chrono::steady_clock::time_point g_ObjectPullNextBroadScanAt{};
    static std::chrono::steady_clock::time_point g_ObjectPullNextVisualUpdateAt{};

    struct ObjectPullClientTraceResult
    {
        bool hitAnything = false;
        bool supported = false;
        C_BaseEntity* entity = nullptr;
        void* vtable = nullptr;
        Vector hitPosition = { 0.0f, 0.0f, 0.0f };
        Vector entityOrigin = { 0.0f, 0.0f, 0.0f };
        float distanceMeters = 0.0f;
        int entityIndex = -1;
        char className[128]{};
        char modelName[260]{};
    };

    struct ObjectPullTrackingSample
    {
        bool valid = false;
        Vector handRelativeToHmd = { 0.0f, 0.0f, 0.0f };
        Vector handRelativeVelocity = { 0.0f, 0.0f, 0.0f };
    };

    static Vector ObjectPullConvertOpenVRPosition(
        const ::vr::HmdMatrix34_t& matrix)
    {
        return Vector(
            -matrix.m[2][3],
            -matrix.m[0][3],
            matrix.m[1][3]);
    }

    static Vector ObjectPullConvertOpenVRVector(
        const ::vr::HmdVector3_t& value)
    {
        return Vector(
            -value.v[2],
            -value.v[0],
            value.v[1]);
    }

    static bool ObjectPullReadLiveTrackingSample(
        VR* instance,
        bool gunHandPhysicalLeft,
        ObjectPullTrackingSample& out)
    {
        out = {};
        if (!instance || !instance->m_System)
            return false;

        const ::vr::ETrackedControllerRole role = gunHandPhysicalLeft
            ? ::vr::TrackedControllerRole_LeftHand
            : ::vr::TrackedControllerRole_RightHand;
        const ::vr::TrackedDeviceIndex_t controllerIndex =
            instance->m_System->GetTrackedDeviceIndexForControllerRole(role);
        if (controllerIndex == ::vr::k_unTrackedDeviceIndexInvalid ||
            controllerIndex >= ::vr::k_unMaxTrackedDeviceCount)
        {
            return false;
        }

        const ::vr::ETrackingUniverseOrigin trackingOrigin =
            instance->m_Compositor
                ? instance->m_Compositor->GetTrackingSpace()
                : ::vr::TrackingUniverseStanding;
        // OpenVR only needs to fill entries through the controller index. Avoid
        // zeroing and requesting all 64 device poses on every armed-frame sample.
        std::array<::vr::TrackedDevicePose_t,
            ::vr::k_unMaxTrackedDeviceCount> livePoses;
        const uint32_t poseCount = std::min<uint32_t>(
            static_cast<uint32_t>(controllerIndex) + 1u,
            ::vr::k_unMaxTrackedDeviceCount);
        instance->m_System->GetDeviceToAbsoluteTrackingPose(
            trackingOrigin,
            0.0f,
            livePoses.data(),
            poseCount);

        const ::vr::TrackedDevicePose_t& hmdPose =
            livePoses[::vr::k_unTrackedDeviceIndex_Hmd];
        const ::vr::TrackedDevicePose_t& controllerPose =
            livePoses[controllerIndex];
        if (!hmdPose.bPoseIsValid || !controllerPose.bPoseIsValid)
            return false;

        const Vector hmdPosition = ObjectPullConvertOpenVRPosition(
            hmdPose.mDeviceToAbsoluteTracking);
        const Vector controllerPosition = ObjectPullConvertOpenVRPosition(
            controllerPose.mDeviceToAbsoluteTracking);
        const Vector hmdVelocity = ObjectPullConvertOpenVRVector(
            hmdPose.vVelocity);
        const Vector controllerVelocity = ObjectPullConvertOpenVRVector(
            controllerPose.vVelocity);
        out.handRelativeToHmd = controllerPosition - hmdPosition;
        out.handRelativeVelocity = controllerVelocity - hmdVelocity;
        out.valid =
            std::isfinite(out.handRelativeToHmd.x) &&
            std::isfinite(out.handRelativeToHmd.y) &&
            std::isfinite(out.handRelativeToHmd.z) &&
            std::isfinite(out.handRelativeVelocity.x) &&
            std::isfinite(out.handRelativeVelocity.y) &&
            std::isfinite(out.handRelativeVelocity.z);
        return out.valid;
    }

    static bool ObjectPullContainsNormalizedToken(
        const char* source,
        const char* normalizedToken)
    {
        if (!source || !normalizedToken || !*normalizedToken)
            return false;

        for (const char* start = source; *start; ++start)
        {
            if (!std::isalnum(static_cast<unsigned char>(*start)))
                continue;

            const char* cursor = start;
            const char* token = normalizedToken;
            while (*token)
            {
                while (*cursor &&
                    !std::isalnum(static_cast<unsigned char>(*cursor)))
                {
                    ++cursor;
                }
                if (!*cursor ||
                    std::tolower(static_cast<unsigned char>(*cursor)) !=
                        std::tolower(static_cast<unsigned char>(*token)))
                {
                    break;
                }
                ++cursor;
                ++token;
            }
            if (!*token)
                return true;
        }
        return false;
    }

    static bool ObjectPullReadClientVtable(const void* entity, void*& outVtable)
    {
        outVtable = nullptr;
        if (!entity)
            return false;
#ifdef _MSC_VER
        __try
        {
            outVtable = *reinterpret_cast<void* const*>(entity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outVtable = nullptr;
        }
#else
        outVtable = *reinterpret_cast<void* const*>(entity);
#endif
        return outVtable != nullptr;
    }

    static bool ObjectPullReadClientOrigin(C_BaseEntity* entity, Vector& outOrigin)
    {
        outOrigin = {};
        if (!entity)
            return false;
#ifdef _MSC_VER
        __try
        {
            outOrigin = entity->GetAbsOrigin();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outOrigin = {};
            return false;
        }
#else
        outOrigin = entity->GetAbsOrigin();
#endif
        return std::isfinite(outOrigin.x) && std::isfinite(outOrigin.y) && std::isfinite(outOrigin.z);
    }

    static bool ObjectPullClientClassIsSupported(const char* className)
    {
        if (!className || !*className)
            return false;

        static const char* const rejectedTokens[] =
        {
            "projectile", "player", "infected", "witch", "door", "button", "world"
        };
        for (const char* token : rejectedTokens)
        {
            if (ObjectPullContainsNormalizedToken(className, token))
                return false;
        }

        static const char* const supportedTokens[] =
        {
            "weapon", "rifle", "shotgun", "pistol", "smg", "sniper",
            "grenadelauncher", "chainsaw", "molotov", "pipebomb", "vomitjar",
            "painpills", "adrenaline", "firstaid", "defibrillator", "upgradepack",
            "ammopack", "gascan", "propanetank", "oxygentank", "fireworkcrate",
            "gnome", "colabottles", "physicsprop", "propphysics", "physbox",
            "funcphysbox", "breakableprop"
        };
        for (const char* token : supportedTokens)
        {
            if (ObjectPullContainsNormalizedToken(className, token))
                return true;
        }
        return false;
    }

    static bool ObjectPullModelNameIsSupported(const char* modelName)
    {
        if (!modelName || !*modelName)
            return false;

        static const char* const rejectedTokens[] =
        {
            "survivor", "infected", "witch", "door", "vehicle", "building"
        };
        for (const char* token : rejectedTokens)
        {
            if (ObjectPullContainsNormalizedToken(modelName, token))
                return false;
        }

        static const char* const supportedTokens[] =
        {
            "wmodelsweapons", "weaponsmelee", "weapon", "items",
            "oildrum", "barrel", "gascan", "propanetank", "oxygentank",
            "firework", "gnome", "cola", "ammo", "upgradepack", "firstaid",
            "defibrillator", "painpills", "adrenaline", "molotov", "pipebomb",
            "vomitjar", "chainsaw"
        };
        for (const char* token : supportedTokens)
        {
            if (ObjectPullContainsNormalizedToken(modelName, token))
                return true;
        }
        return false;
    }

    static bool ObjectPullClientNameIsLivingBlocker(
        const char* className,
        const char* modelName)
    {
        const char* source = className && *className
            ? className
            : modelName;
        return ObjectPullContainsNormalizedToken(source, "infected") ||
            ObjectPullContainsNormalizedToken(source, "witch") ||
            ObjectPullContainsNormalizedToken(source, "player") ||
            ObjectPullContainsNormalizedToken(source, "survivor");
    }

    static bool ObjectPullCopyClassName(const char* source, char* destination, size_t capacity)
    {
        if (!destination || capacity == 0)
            return false;
        destination[0] = '\0';
        if (!source || !*source)
            return false;
        std::strncpy(destination, source, capacity - 1);
        destination[capacity - 1] = '\0';
        return true;
    }

    static int ObjectPullSafeGetHighestClientEntityIndex(VR* vr)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_ClientEntityList)
            return 0;
#ifdef _MSC_VER
        __try
        {
            return std::clamp(vr->m_Game->m_ClientEntityList->GetHighestEntityIndex(), 0, 2047);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
#else
        return std::clamp(vr->m_Game->m_ClientEntityList->GetHighestEntityIndex(), 0, 2047);
#endif
    }

    static C_BaseEntity* ObjectPullSafeGetClientEntity(VR* vr, int entityIndex)
    {
        if (!vr || !vr->m_Game || !vr->m_Game->m_ClientEntityList || entityIndex <= 0)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return static_cast<C_BaseEntity*>(
                vr->m_Game->m_ClientEntityList->GetClientEntity(entityIndex));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return static_cast<C_BaseEntity*>(
            vr->m_Game->m_ClientEntityList->GetClientEntity(entityIndex));
#endif
    }

    static C_BaseEntity* ObjectPullResolveTraceEntity(VR* vr, void* traceEntity, int& outEntityIndex)
    {
        struct CacheEntry
        {
            void* traceEntity = nullptr;
            C_BaseEntity* entity = nullptr;
            int entityIndex = -1;
        };
        static std::array<CacheEntry, 24> cache{};
        static size_t nextCacheEntry = 0;

        outEntityIndex = -1;
        if (!vr || !traceEntity)
            return nullptr;

        for (CacheEntry& entry : cache)
        {
            if (entry.traceEntity != traceEntity || entry.entityIndex <= 0)
                continue;

            C_BaseEntity* current = ObjectPullSafeGetClientEntity(
                vr, entry.entityIndex);
            if (current == entry.entity &&
                reinterpret_cast<void*>(current) == traceEntity)
            {
                outEntityIndex = entry.entityIndex;
                return current;
            }
            entry = {};
        }

        const int highestIndex = ObjectPullSafeGetHighestClientEntityIndex(vr);
        for (int entityIndex = 1; entityIndex <= highestIndex; ++entityIndex)
        {
            C_BaseEntity* entity = ObjectPullSafeGetClientEntity(vr, entityIndex);
            if (entity && reinterpret_cast<void*>(entity) == traceEntity)
            {
                CacheEntry& entry = cache[nextCacheEntry++ % cache.size()];
                entry.traceEntity = traceEntity;
                entry.entity = entity;
                entry.entityIndex = entityIndex;
                outEntityIndex = entityIndex;
                return entity;
            }
        }
        return nullptr;
    }

    static bool ObjectPullSafeCopyNetworkClassName(
        VR* vr,
        C_BaseEntity* entity,
        char* destination,
        size_t capacity)
    {
        if (!destination || capacity == 0)
            return false;
        destination[0] = '\0';
        if (!vr || !vr->m_Game || !entity)
            return false;
#ifdef _MSC_VER
        __try
        {
            const char* className = vr->m_Game->GetNetworkClassName(
                reinterpret_cast<uintptr_t*>(entity));
            return ObjectPullCopyClassName(className, destination, capacity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
            return false;
        }
#else
        const char* className = vr->m_Game->GetNetworkClassName(
            reinterpret_cast<uintptr_t*>(entity));
        return ObjectPullCopyClassName(className, destination, capacity);
#endif
    }

    class IClientRenderableObjectPullProbe
    {
    public:
        virtual void* GetIClientUnknown() = 0;
        virtual const Vector& GetRenderOrigin() = 0;
        virtual const QAngle& GetRenderAngles() = 0;
        virtual bool ShouldDraw() = 0;
        virtual int GetRenderFlags() = 0;
        virtual void Unused() const = 0;
        virtual void* GetShadowHandle() const = 0;
        virtual void* RenderHandle() = 0;
        virtual void* GetModel() const = 0;
    };

    static bool ObjectPullSafeCopyModelName(
        VR* vr,
        C_BaseEntity* entity,
        char* destination,
        size_t capacity)
    {
        if (!destination || capacity == 0)
            return false;
        destination[0] = '\0';
        if (!vr || !vr->m_Game || !vr->m_Game->m_ModelInfo || !entity)
            return false;
#ifdef _MSC_VER
        __try
        {
            void* renderableVoid = entity->GetClientRenderable();
            if (!renderableVoid)
                return false;
            auto* renderable = reinterpret_cast<IClientRenderableObjectPullProbe*>(renderableVoid);
            void* model = renderable->GetModel();
            if (!model)
                return false;
            const char* modelName = vr->m_Game->m_ModelInfo->GetModelName(model);
            return ObjectPullCopyClassName(modelName, destination, capacity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
            return false;
        }
#else
        void* renderableVoid = entity->GetClientRenderable();
        if (!renderableVoid)
            return false;
        auto* renderable = reinterpret_cast<IClientRenderableObjectPullProbe*>(renderableVoid);
        void* model = renderable->GetModel();
        if (!model)
            return false;
        const char* modelName = vr->m_Game->m_ModelInfo->GetModelName(model);
        return ObjectPullCopyClassName(modelName, destination, capacity);
#endif
    }

    static bool ObjectPullEvaluateClientTrace(
        VR* vr,
        const Vector& start,
        float scale,
        const CGameTrace& trace,
        ObjectPullClientTraceResult& out)
    {
        if (!vr || !vr->m_Game || !trace.m_pEnt || trace.startsolid || trace.allsolid)
            return false;

        out.hitAnything = true;
        out.hitPosition = trace.endpos;
        out.distanceMeters = (trace.endpos - start).Length() / scale;

        const bool distanceAllowed =
            out.distanceMeters >= std::max(0.0f, vr->m_ObjectPullMinimumDistanceMeters) &&
            out.distanceMeters <= std::max(0.1f, vr->m_ObjectPullMaxDistanceMeters);
        if (!distanceAllowed)
            return false;

        out.entity = ObjectPullResolveTraceEntity(vr, trace.m_pEnt, out.entityIndex);
        if (!out.entity)
            return false;

        ObjectPullSafeCopyNetworkClassName(
            vr, out.entity, out.className, sizeof(out.className));
        bool supported = ObjectPullClientClassIsSupported(out.className);
        if (!supported)
        {
            ObjectPullSafeCopyModelName(
                vr, out.entity, out.modelName, sizeof(out.modelName));
            supported = ObjectPullModelNameIsSupported(out.modelName);
        }
        if (!supported)
            return false;

        if (!ObjectPullReadClientVtable(out.entity, out.vtable))
            return false;

        if (!ObjectPullReadClientOrigin(out.entity, out.entityOrigin))
            out.entityOrigin = out.hitPosition;

        out.supported = true;
        return true;
    }

    static ObjectPullClientTraceResult ObjectPullFindClientCandidateAlongRay(
        VR* vr, C_BasePlayer* localPlayer, const Vector& start, Vector forward);

    static ObjectPullClientTraceResult ObjectPullTraceClientTarget(
        VR* vr,
        C_BasePlayer* localPlayer,
        const Vector& start,
        Vector forward,
        bool allowBroadScan)
    {
        ObjectPullClientTraceResult result{};
        if (!vr || !localPlayer || !vr->m_Game || !vr->m_Game->m_EngineTrace)
            return result;
        if (VectorNormalize(forward) <= 0.0001f)
            return result;

        const float scale = std::max(1.0f, vr->m_VRScale);
        const Vector end = start + forward * (std::max(0.1f, vr->m_ObjectPullMaxDistanceMeters) * scale);
        CTraceFilterSkipSelf filter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);

        CGameTrace exactTrace{};
        Ray_t exactRay;
        exactRay.Init(start, end);
        if (VR_SafeTraceRay(vr->m_Game->m_EngineTrace, exactRay, MASK_SHOT_HULL, &filter, exactTrace))
        {
            ObjectPullClientTraceResult exactResult{};
            if (ObjectPullEvaluateClientTrace(vr, start, scale, exactTrace, exactResult))
                return exactResult;
            if (exactResult.hitAnything)
            {
                result = exactResult;
            }
        }

        const float assistUnits = std::max(0.0f, vr->m_ObjectPullTargetAssistRadiusMeters) * scale;
        if (assistUnits <= 0.01f)
            return result;

        const Vector mins{ -assistUnits, -assistUnits, -assistUnits };
        const Vector maxs{ assistUnits, assistUnits, assistUnits };
        CGameTrace assistTrace{};
        Ray_t assistRay;
        assistRay.Init(start, end, mins, maxs);
        if (VR_SafeTraceRay(vr->m_Game->m_EngineTrace, assistRay, MASK_SHOT_HULL, &filter, assistTrace))
        {
            ObjectPullClientTraceResult assistResult{};
            if (ObjectPullEvaluateClientTrace(vr, start, scale, assistTrace, assistResult))
                return assistResult;
            if (assistResult.hitAnything)
            {
                if (!result.hitAnything)
                    result = assistResult;
            }
        }

        // Some L4D2 pickup entities do not reliably participate in MASK_SHOT_HULL.
        // Preserve the broad candidate search, but run it only on the throttled
        // broad-scan cadence instead of every render frame.
        if (!allowBroadScan)
            return result;

        ObjectPullClientTraceResult scanned = ObjectPullFindClientCandidateAlongRay(
            vr, localPlayer, start, forward);
        if (scanned.supported)
            return scanned;
        return result;
    }

    static ObjectPullClientTraceResult ObjectPullFindClientCandidateAlongRay(
        VR* vr,
        C_BasePlayer* localPlayer,
        const Vector& start,
        Vector forward)
    {
        ObjectPullClientTraceResult best{};
        if (!vr || !localPlayer || !vr->m_Game || !vr->m_Game->m_ClientEntityList)
            return best;
        if (VectorNormalize(forward) <= 0.0001f)
            return best;

        const float scale = std::max(1.0f, vr->m_VRScale);
        const float minDistance = std::max(0.0f, vr->m_ObjectPullMinimumDistanceMeters) * scale;
        const float maxDistance = std::max(0.1f, vr->m_ObjectPullMaxDistanceMeters) * scale;
        const float assistRadius = std::max(0.10f, vr->m_ObjectPullTargetAssistRadiusMeters) * scale;
        float bestScore = FLT_MAX;

        const int highestIndex = ObjectPullSafeGetHighestClientEntityIndex(vr);
        if (highestIndex <= 0)
            return best;

        // Spread the fallback entity-list search across frames. A full 2047-entry
        // sweep can produce a visible CPU spike in VR even when its average cost
        // looks small. Exact/swept traces remain immediate; this path checks at
        // most 192 entities per update and rotates through the list.
        static int scanCursor = 1;
        if (scanCursor <= 0 || scanCursor > highestIndex)
            scanCursor = 1;
        constexpr int kMaxEntitiesPerSlice = 192;
        const int sliceCount = std::min(highestIndex, kMaxEntitiesPerSlice);
        for (int checked = 0; checked < sliceCount; ++checked)
        {
            const int entityIndex = scanCursor;
            ++scanCursor;
            if (scanCursor > highestIndex)
                scanCursor = 1;

            C_BaseEntity* entity = ObjectPullSafeGetClientEntity(vr, entityIndex);
            if (!entity || entity == reinterpret_cast<C_BaseEntity*>(localPlayer))
                continue;

            Vector origin{};
            if (!ObjectPullReadClientOrigin(entity, origin))
                continue;
            const Vector toEntity = origin - start;
            const float along = toEntity.Dot(forward);
            if (!std::isfinite(along) || along < minDistance || along > maxDistance)
                continue;
            const Vector closest = start + forward * along;
            const float perpendicular = (origin - closest).Length();
            if (!std::isfinite(perpendicular) || perpendicular > assistRadius)
                continue;

            const float score = perpendicular + along * 0.0025f;
            if (score >= bestScore)
                continue;

            // Class/model virtual calls and string matching are substantially
            // more expensive than the geometric rejection above. Only inspect
            // entities already inside the narrow aim cylinder.
            char className[128]{};
            char modelName[260]{};
            ObjectPullSafeCopyNetworkClassName(vr, entity, className, sizeof(className));
            bool supported = ObjectPullClientClassIsSupported(className);
            if (!supported)
            {
                ObjectPullSafeCopyModelName(vr, entity, modelName, sizeof(modelName));
                supported = ObjectPullModelNameIsSupported(modelName);
            }
            if (!supported)
                continue;

            if (vr->m_Game->m_EngineTrace)
            {
                CTraceFilterSkipSelf filter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);
                Ray_t visibilityRay;
                visibilityRay.Init(start, origin);
                CGameTrace visibilityTrace{};
                if (VR_SafeTraceRay(
                    vr->m_Game->m_EngineTrace,
                    visibilityRay,
                    MASK_SHOT_HULL,
                    &filter,
                    visibilityTrace) &&
                    visibilityTrace.m_pEnt &&
                    visibilityTrace.m_pEnt != reinterpret_cast<void*>(entity) &&
                    visibilityTrace.fraction < 0.97f)
                {
                    int blockerIndex = -1;
                    C_BaseEntity* blocker = ObjectPullResolveTraceEntity(
                        vr,
                        visibilityTrace.m_pEnt,
                        blockerIndex);
                    if (!blocker)
                        continue;

                    char blockerClass[128]{};
                    char blockerModel[260]{};
                    ObjectPullSafeCopyNetworkClassName(
                        vr, blocker, blockerClass, sizeof(blockerClass));
                    if (!blockerClass[0])
                    {
                        ObjectPullSafeCopyModelName(
                            vr, blocker, blockerModel, sizeof(blockerModel));
                    }
                    const bool livingBlocker =
                        ObjectPullClientNameIsLivingBlocker(
                            blockerClass,
                            blockerModel);
                    const float obstructionGap =
                        (origin - visibilityTrace.endpos).Length();
                    const float embeddedTolerance =
                        std::max(assistRadius * 2.0f, 0.20f * scale);
                    if (!livingBlocker && obstructionGap > embeddedTolerance)
                        continue;
                }
            }

            void* vtable = nullptr;
            if (!ObjectPullReadClientVtable(entity, vtable))
                continue;
            bestScore = score;
            best.hitAnything = true;
            best.supported = true;
            best.entity = entity;
            best.vtable = vtable;
            best.hitPosition = origin;
            best.entityOrigin = origin;
            best.distanceMeters = along / scale;
            best.entityIndex = entityIndex;
            ObjectPullCopyClassName(className, best.className, sizeof(best.className));
            ObjectPullCopyClassName(modelName, best.modelName, sizeof(best.modelName));
        }
        return best;
    }

    static void ObjectPullDrawMarker(
        IVDebugOverlay* overlay,
        const Vector& position,
        float radius,
        int r,
        int g,
        int b,
        int a,
        float duration)
    {
        if (!overlay || radius <= 0.0f)
            return;
        const Vector mins{ -radius, -radius, -radius };
        const Vector maxs{ radius, radius, radius };
        const QAngle angles{ 0.0f, 0.0f, 0.0f };
        overlay->AddBoxOverlay(position, mins, maxs, angles, r, g, b, a, duration);
        overlay->AddLineOverlay(
            position - Vector(radius * 1.6f, 0.0f, 0.0f),
            position + Vector(radius * 1.6f, 0.0f, 0.0f),
            r, g, b, true, duration);
        overlay->AddLineOverlay(
            position - Vector(0.0f, radius * 1.6f, 0.0f),
            position + Vector(0.0f, radius * 1.6f, 0.0f),
            r, g, b, true, duration);
        overlay->AddLineOverlay(
            position - Vector(0.0f, 0.0f, radius * 1.6f),
            position + Vector(0.0f, 0.0f, radius * 1.6f),
            r, g, b, true, duration);
    }


}

static void ObjectPullPublishWireCommand(uint8_t wireCommand, int targetEntityIndex)
{
    constexpr uint32_t kCommandMask = 0xFFu;
    constexpr uint32_t kEntityMask = 0x7FFu;

    const uint32_t command = static_cast<uint32_t>(wireCommand) & kCommandMask;
    const uint32_t entity = static_cast<uint32_t>(
        std::clamp(targetEntityIndex, 0, 2047)) & kEntityMask;
    const uint32_t payload = command | (entity << 8);

    // A single release-store publishes command and entity index coherently.
    // Do not suppress repeated stores here: Begin/Continue reliability windows
    // intentionally republish the current state for later usercmd batches.
    g_ObjectPullWireMailbox.store(payload, std::memory_order_release);
}

void VR::ResetObjectPullInput(bool sendCancel)
{
    m_ObjectPullPhase = ObjectPullClientPhase::Idle;
    m_ObjectPullClientTarget = nullptr;
    m_ObjectPullClientTargetVtable = nullptr;
    m_ObjectPullClientTargetEntityIndex = 0;
    m_ObjectPullWireTargetEntityIndex = 0;
    m_ObjectPullClientTargetPoint = {};
    m_ObjectPullArmPosition = {};
    m_ObjectPullArmHandRelativeToHmd = {};
    m_ObjectPullArmTowardBodyDirection = {};
    m_ObjectPullLastHandRelativeToHmd = {};
    m_ObjectPullArmForward = {};
    m_ObjectPullArmAngles = {};
    m_ObjectPullCurrentControllerPosition = {};
    m_ObjectPullCurrentControllerAngles = {};
    m_ObjectPullBeginRepeatUntil = {};
    m_ObjectPullLaunchRepeatUntil = {};
    m_ObjectPullCatchEnableAt = {};
    m_ObjectPullFlightExpireAt = {};
    m_ObjectPullGestureSampleTime = {};
    g_ObjectPullNextTargetScanAt = {};
    g_ObjectPullNextBroadScanAt = {};
    g_ObjectPullNextVisualUpdateAt = {};
    m_ObjectPullLastTargetDistanceMeters = 0.0f;
    m_ObjectPullGesturePeakDistanceMeters = 0.0f;
    m_ObjectPullGesturePeakSpeedMetersPerSecond = 0.0f;

    if (sendCancel)
    {
        const auto now = std::chrono::steady_clock::now();
        m_ObjectPullRequireActionRelease = m_ObjectPullActionDownPrev;
        m_ObjectPullCancelRepeatUntil = now +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(0.15f));
        m_ObjectPullDesiredWireCommand = kObjectPullWireCancel;
        ObjectPullPublishWireCommand(
            m_ObjectPullDesiredWireCommand,
            m_ObjectPullWireTargetEntityIndex);
    }
    else
    {
        m_ObjectPullRequireActionRelease = false;
        m_ObjectPullActionDownPrev = false;
        m_ObjectPullCatchActionDownPrev = false;
        m_ObjectPullCancelRepeatUntil = {};
        m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
        ObjectPullPublishWireCommand(kObjectPullWireNone, 0);
    }
}

bool VR::UpdateObjectPullInput(C_BasePlayer* localPlayer, bool objectPullActionDown, bool catchActionDown)
{
    const auto now = std::chrono::steady_clock::now();
    const bool targetScanDue =
        g_ObjectPullNextTargetScanAt.time_since_epoch().count() == 0 ||
        now >= g_ObjectPullNextTargetScanAt;
    const bool broadScanDue =
        g_ObjectPullNextBroadScanAt.time_since_epoch().count() == 0 ||
        now >= g_ObjectPullNextBroadScanAt;
    const bool visualUpdateDue =
        g_ObjectPullNextVisualUpdateAt.time_since_epoch().count() == 0 ||
        now >= g_ObjectPullNextVisualUpdateAt;
    const bool actionJustPressed =
        objectPullActionDown && !m_ObjectPullActionDownPrev;
    const bool actionJustReleased =
        !objectPullActionDown && m_ObjectPullActionDownPrev;
    m_ObjectPullActionDownPrev = objectPullActionDown;
    if (actionJustReleased)
        m_ObjectPullRequireActionRelease = false;

    const bool gunHandPhysicalLeft = IsGameplayHandLeftPhysical(false);
    // GetPoses() remaps physical controllers in left-handed mode, so the
    // logical weapon hand is always stored in the right-controller fields.
    const Vector gunHandPosition = GetRightControllerAbsPos();
    Vector gunHandForward = m_RightControllerForward;
    const QAngle gunHandAngles = GetRightControllerAbsAngle();
    VectorNormalize(gunHandForward);
    m_ObjectPullCurrentControllerPosition = gunHandPosition;
    m_ObjectPullCurrentControllerAngles = gunHandAngles;

    const bool holdingCatchActionDown = catchActionDown;
    const bool catchActionJustPressed =
        holdingCatchActionDown && !m_ObjectPullCatchActionDownPrev;
    const bool catchActionJustReleased =
        !holdingCatchActionDown && m_ObjectPullCatchActionDownPrev;
    if ((catchActionJustPressed || catchActionJustReleased) && m_ObjectPullDebugLog)
    {
        Game::logMsg(
            "[VR][ObjectPull][client] catch action %s hand=%s",
            catchActionJustPressed ? "pressed" : "released",
            gunHandPhysicalLeft ? "left" : "right");
    }
    m_ObjectPullCatchActionDownPrev = holdingCatchActionDown;

    const bool featureAvailable =
        m_ObjectPullEnabled &&
        m_EncodeVRUsercmd &&
        !m_ForceNonVRServerMovement &&
        localPlayer &&
        m_Game &&
        m_Game->m_EngineTrace &&
        m_Game->m_ClientEntityList &&
        !m_TeleportTargetingActive &&
        !IsScopeActive() &&
        !m_AdjustingViewmodel &&
        !m_AdjustingScope;

    if (!featureAvailable)
    {
        if (actionJustPressed && m_ObjectPullDebugLog)
        {
            Game::logMsg(
                "[VR][ObjectPull][client] unavailable enabled=%d encode=%d forceNonVR=%d player=%p trace=%p entityList=%p teleport=%d scope=%d adjustViewmodel=%d adjustScope=%d",
                m_ObjectPullEnabled ? 1 : 0,
                m_EncodeVRUsercmd ? 1 : 0,
                m_ForceNonVRServerMovement ? 1 : 0,
                localPlayer,
                m_Game ? m_Game->m_EngineTrace : nullptr,
                m_Game ? m_Game->m_ClientEntityList : nullptr,
                m_TeleportTargetingActive ? 1 : 0,
                IsScopeActive() ? 1 : 0,
                m_AdjustingViewmodel ? 1 : 0,
                m_AdjustingScope ? 1 : 0);
        }

        if (m_ObjectPullPhase != ObjectPullClientPhase::Idle)
            ResetObjectPullInput(true);
        else if (
            m_ObjectPullCancelRepeatUntil.time_since_epoch().count() == 0 ||
            now > m_ObjectPullCancelRepeatUntil)
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
            ObjectPullPublishWireCommand(kObjectPullWireNone, 0);
        }
        return false;
    }

    // The flying object is not a current selection. Pressing Object Pull again
    // abandons only its catch tracking; the old object keeps its physical velocity.
    if (actionJustPressed &&
        m_ObjectPullPhase == ObjectPullClientPhase::Pulling)
    {
        if (m_ObjectPullDebugLog)
        {
            Game::logMsg(
                "[VR][ObjectPull][client] new selection started while previous object continues under physics");
        }

        ResetObjectPullInput(false);
        m_ObjectPullActionDownPrev = true;
        m_ObjectPullRequireActionRelease = false;
        m_ObjectPullCatchActionDownPrev = holdingCatchActionDown;
    }

    ObjectPullClientTraceResult preview{};
    bool previewEvaluated = false;
    if (m_ObjectPullPhase == ObjectPullClientPhase::Idle &&
        objectPullActionDown &&
        !m_ObjectPullRequireActionRelease &&
        (actionJustPressed || targetScanDue))
    {
        previewEvaluated = true;
        g_ObjectPullNextTargetScanAt = now +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / 60.0f));
        const bool allowBroadScan = actionJustPressed || broadScanDue;
        if (allowBroadScan)
        {
            g_ObjectPullNextBroadScanAt = now +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(1.0f / 60.0f));
        }
        preview = ObjectPullTraceClientTarget(
            this,
            localPlayer,
            gunHandPosition,
            gunHandForward,
            allowBroadScan);
        if (preview.supported)
        {
            m_ObjectPullPhase = ObjectPullClientPhase::Armed;
            m_ObjectPullClientTarget = preview.entity;
            m_ObjectPullClientTargetVtable = preview.vtable;
            m_ObjectPullClientTargetEntityIndex = preview.entityIndex;
            m_ObjectPullClientTargetPoint = preview.entityOrigin;
            m_ObjectPullArmPosition = gunHandPosition;
            // Gesture detection uses only the live OpenVR tracking matrices
            // sampled on this update. Cached game-frame poses and world-space
            // viewmodel/controller positions are intentionally not accepted.
            ObjectPullTrackingSample armTracking{};
            if (!ObjectPullReadLiveTrackingSample(
                this,
                gunHandPhysicalLeft,
                armTracking))
            {
                if (m_ObjectPullDebugLog)
                {
                    Game::logMsg(
                        "[VR][ObjectPull][client] supported target found but live tracking pose is unavailable");
                }
                ResetObjectPullInput(false);
                m_ObjectPullActionDownPrev = objectPullActionDown;
                m_ObjectPullCatchActionDownPrev = holdingCatchActionDown;
                return false;
            }
            m_ObjectPullArmHandRelativeToHmd =
                armTracking.handRelativeToHmd;
            m_ObjectPullLastHandRelativeToHmd =
                m_ObjectPullArmHandRelativeToHmd;
            m_ObjectPullArmTowardBodyDirection =
                m_ObjectPullArmHandRelativeToHmd * -1.0f;
            if (VectorNormalize(m_ObjectPullArmTowardBodyDirection) <= 0.0001f)
                m_ObjectPullArmTowardBodyDirection = {};
            m_ObjectPullArmForward = gunHandForward;
            m_ObjectPullArmAngles = gunHandAngles;
            m_ObjectPullLastTargetDistanceMeters =
                (gunHandPosition - preview.entityOrigin).Length() /
                std::max(1.0f, m_VRScale);
            m_ObjectPullGestureSampleTime = now;
            m_ObjectPullGesturePeakDistanceMeters = 0.0f;
            m_ObjectPullGesturePeakSpeedMetersPerSecond = 0.0f;
            m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
            m_ObjectPullCancelRepeatUntil = {};
            TriggerPhysicalHandHapticPulse(
                gunHandPhysicalLeft,
                0.018f,
                75.0f,
                0.28f);

            if (m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] armed entity=%p index=%d class=%s model=%s distance=%.2fm assist=%.2fm hand=%s",
                    preview.entity,
                    preview.entityIndex,
                    preview.className[0]
                        ? preview.className
                        : "<none>",
                    preview.modelName[0]
                        ? preview.modelName
                        : "<none>",
                    preview.distanceMeters,
                    m_ObjectPullTargetAssistRadiusMeters,
                    gunHandPhysicalLeft ? "left" : "right");
            }
        }
        else if (actionJustPressed && m_ObjectPullDebugLog)
        {
            Game::logMsg(
                "[VR][ObjectPull][client] no supported target hit=%d index=%d class=%s model=%s distance=%.2fm min=%.2fm max=%.2fm",
                preview.hitAnything ? 1 : 0,
                preview.entityIndex,
                preview.className[0]
                    ? preview.className
                    : "<none>",
                preview.modelName[0]
                    ? preview.modelName
                    : "<none>",
                preview.distanceMeters,
                m_ObjectPullMinimumDistanceMeters,
                m_ObjectPullMaxDistanceMeters);
        }
    }

    if (m_ObjectPullPhase == ObjectPullClientPhase::Armed)
    {
        void* currentVtable = nullptr;
        const bool targetValid =
            ObjectPullReadClientVtable(
                m_ObjectPullClientTarget,
                currentVtable) &&
            currentVtable == m_ObjectPullClientTargetVtable;

        if (!targetValid)
        {
            if (m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] target invalid; clearing armed selection");
            }
            ResetObjectPullInput(true);
        }
        else
        {
            if (m_ObjectPullVisualsEnabled && visualUpdateDue)
            {
                Vector liveTargetOrigin{};
                if (ObjectPullReadClientOrigin(
                    m_ObjectPullClientTarget,
                    liveTargetOrigin))
                {
                    m_ObjectPullClientTargetPoint = liveTargetOrigin;
                }
            }

            ObjectPullTrackingSample currentTracking{};
            if (!ObjectPullReadLiveTrackingSample(
                this,
                gunHandPhysicalLeft,
                currentTracking))
            {
                if (actionJustReleased)
                {
                    if (m_ObjectPullDebugLog)
                    {
                        Game::logMsg(
                            "[VR][ObjectPull][client] gesture cancelled because live tracking pose became unavailable");
                    }
                    ResetObjectPullInput(true);
                }
                return false;
            }

            const Vector currentHandRelativeToHmd =
                currentTracking.handRelativeToHmd;
            const Vector trackedRelativeVelocity =
                currentTracking.handRelativeVelocity;
            const Vector trackingDeltaFromArm =
                currentHandRelativeToHmd -
                m_ObjectPullArmHandRelativeToHmd;

            // Measure the controller's real displacement relative to the HMD.
            // Stick locomotion and player-world movement translate neither side
            // of this tracking-space vector, so they cannot trigger the pull.
            const float trackingDisplacementMeters =
                trackingDeltaFromArm.Length();
            const float pullDistanceMeters =
                trackingDisplacementMeters;

            float pullSpeedMetersPerSecond =
                trackedRelativeVelocity.Length();
            if (!std::isfinite(pullSpeedMetersPerSecond))
                pullSpeedMetersPerSecond = 0.0f;
            if (m_ObjectPullGestureSampleTime.time_since_epoch().count() != 0)
            {
                const float sampleSeconds = std::chrono::duration<float>(
                    now - m_ObjectPullGestureSampleTime).count();
                if (sampleSeconds > 0.001f && sampleSeconds < 0.20f)
                {
                    const Vector trackingSampleDelta =
                        currentHandRelativeToHmd -
                        m_ObjectPullLastHandRelativeToHmd;
                    const float sampledSpeed =
                        trackingSampleDelta.Length() / sampleSeconds;
                    if (std::isfinite(sampledSpeed))
                    {
                        pullSpeedMetersPerSecond = std::max(
                            pullSpeedMetersPerSecond,
                            sampledSpeed);
                    }
                }
            }
            m_ObjectPullLastHandRelativeToHmd =
                currentHandRelativeToHmd;
            m_ObjectPullGestureSampleTime = now;
            m_ObjectPullGesturePeakDistanceMeters = std::max(
                m_ObjectPullGesturePeakDistanceMeters,
                pullDistanceMeters);
            m_ObjectPullGesturePeakSpeedMetersPerSecond = std::max(
                m_ObjectPullGesturePeakSpeedMetersPerSecond,
                pullSpeedMetersPerSecond);

            const float configuredDistance =
                std::max(0.01f, m_ObjectPullGestureDistanceMeters);
            const bool fullDistanceGesture =
                pullDistanceMeters >= configuredDistance;
            const bool fastFlickGesture =
                pullDistanceMeters >=
                    std::max(0.025f, configuredDistance * 0.35f) &&
                pullSpeedMetersPerSecond >= 0.55f;

            if (fullDistanceGesture || fastFlickGesture)
            {
                // The selected entity and its outline end on the trigger frame.
                // Begin uses the original pointing pose for server validation;
                // Continue uses the current pose as a fixed ballistic target.
                m_ObjectPullPhase = ObjectPullClientPhase::Pulling;
                m_ObjectPullWireTargetEntityIndex = m_ObjectPullClientTargetEntityIndex;
                m_ObjectPullClientTarget = nullptr;
                m_ObjectPullClientTargetVtable = nullptr;
                m_ObjectPullClientTargetEntityIndex = 0;
                m_ObjectPullClientTargetPoint = {};
                m_ObjectPullRequireActionRelease = true;
                m_ObjectPullBeginRepeatUntil = now +
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(0.09f));
                m_ObjectPullCatchEnableAt = now +
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(0.18f));
                m_ObjectPullLaunchRepeatUntil = now +
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(0.40f));
                m_ObjectPullFlightExpireAt = now +
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(8.00f));
                m_ObjectPullDesiredWireCommand = kObjectPullWireBegin;
                TriggerPhysicalHandHapticPulse(
                    gunHandPhysicalLeft,
                    0.025f,
                    90.0f,
                    0.35f);

                if (m_ObjectPullDebugLog)
                {
                    Game::logMsg(
                        "[VR][ObjectPull][client] gesture triggered distance=%.3fm speed=%.2fm/s mode=%s; selection entity and outline cleared",
                        pullDistanceMeters,
                        pullSpeedMetersPerSecond,
                        fullDistanceGesture ? "distance" : "speed");
                }
            }
            else if (actionJustReleased)
            {
                if (m_ObjectPullDebugLog)
                {
                    Game::logMsg(
                        "[VR][ObjectPull][client] gesture not triggered peakDistance=%.3fm peakSpeed=%.2fm/s currentDistance=%.3fm threshold=%.3fm; armed selection released",
                        m_ObjectPullGesturePeakDistanceMeters,
                        m_ObjectPullGesturePeakSpeedMetersPerSecond,
                        trackingDisplacementMeters,
                        configuredDistance);
                }
                ResetObjectPullInput(true);
            }
        }
    }
    else if (m_ObjectPullPhase == ObjectPullClientPhase::Pulling)
    {
        if (
            m_ObjectPullFlightExpireAt.time_since_epoch().count() != 0 &&
            now > m_ObjectPullFlightExpireAt)
        {
            if (m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] flight catch window expired");
            }
            ResetObjectPullInput(true);
        }
        else if (
            m_ObjectPullBeginRepeatUntil.time_since_epoch().count() != 0 &&
            now <= m_ObjectPullBeginRepeatUntil)
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireBegin;
        }
        else if (
            holdingCatchActionDown &&
            m_ObjectPullCatchEnableAt.time_since_epoch().count() != 0 &&
            now >= m_ObjectPullCatchEnableAt)
        {
            m_ObjectPullPhase = ObjectPullClientPhase::Held;
            m_ObjectPullDesiredWireCommand = kObjectPullWireCatch;
            TriggerPhysicalHandHapticPulse(
                gunHandPhysicalLeft,
                0.035f,
                120.0f,
                0.55f);

            if (m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] catch action requested");
            }
        }
        else if (
            m_ObjectPullLaunchRepeatUntil.time_since_epoch().count() != 0 &&
            now <= m_ObjectPullLaunchRepeatUntil)
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireContinue;
        }
        else
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
        }
    }
    else if (m_ObjectPullPhase == ObjectPullClientPhase::Held)
    {
        if (!holdingCatchActionDown)
        {
            if (m_ObjectPullDebugLog)
                Game::logMsg("[VR][ObjectPull][client] catch action released");
            ResetObjectPullInput(true);
        }
        else
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireCatch;
        }
    }

    if (m_ObjectPullVisualsEnabled &&
        visualUpdateDue &&
        m_Game &&
        m_Game->m_DebugOverlay)
    {
        bool drewVisual = false;
        IVDebugOverlay* overlay = m_Game->m_DebugOverlay;
        const float scale = std::max(1.0f, m_VRScale);
        const float duration = 0.030f;
        const float targetRadius =
            std::max(2.0f, 0.065f * scale);
        const float handRadius =
            std::max(1.5f, 0.035f * scale);

        if (m_ObjectPullPhase == ObjectPullClientPhase::Idle &&
            objectPullActionDown &&
            !m_ObjectPullRequireActionRelease &&
            previewEvaluated)
        {
            drewVisual = true;
            if (preview.hitAnything)
            {
                const int r = 255;
                const int g = preview.supported ? 140 : 45;
                const int b = preview.supported ? 20 : 45;
                overlay->AddLineOverlay(
                    gunHandPosition,
                    preview.hitPosition,
                    r,
                    g,
                    b,
                    true,
                    duration);
                ObjectPullDrawMarker(
                    overlay,
                    preview.hitPosition,
                    handRadius,
                    r,
                    g,
                    b,
                    220,
                    duration);
                ObjectPullDrawMarker(
                    overlay,
                    preview.hitPosition,
                    handRadius * 1.8f,
                    r,
                    g,
                    b,
                    120,
                    duration);
            }
            else
            {
                const Vector aimEnd =
                    gunHandPosition +
                    gunHandForward *
                        (std::max(
                            0.1f,
                            m_ObjectPullMaxDistanceMeters) *
                            scale);
                overlay->AddLineOverlay(
                    gunHandPosition,
                    aimEnd,
                    180,
                    180,
                    180,
                    true,
                    duration);
            }
        }
        else if (m_ObjectPullPhase == ObjectPullClientPhase::Armed)
        {
            drewVisual = true;
            ObjectPullDrawMarker(
                overlay,
                m_ObjectPullClientTargetPoint,
                targetRadius,
                255,
                195,
                35,
                230,
                duration);
            ObjectPullDrawMarker(
                overlay,
                m_ObjectPullClientTargetPoint,
                targetRadius * 1.45f,
                255,
                195,
                35,
                110,
                duration);
            overlay->AddLineOverlay(
                gunHandPosition,
                m_ObjectPullClientTargetPoint,
                255,
                195,
                35,
                true,
                duration);

            Vector targetDirection =
                m_ObjectPullArmPosition -
                m_ObjectPullClientTargetPoint;
            if (VectorNormalize(targetDirection) <= 0.0001f)
                targetDirection = m_ObjectPullArmForward * -1.0f;
            const Vector gestureEnd =
                m_ObjectPullArmPosition +
                targetDirection *
                    (std::max(
                        0.01f,
                        m_ObjectPullGestureDistanceMeters) *
                        scale);
            overlay->AddLineOverlay(
                m_ObjectPullArmPosition,
                gestureEnd,
                255,
                210,
                50,
                true,
                duration);
            overlay->AddLineOverlay(
                m_ObjectPullArmPosition,
                gunHandPosition,
                255,
                245,
                150,
                true,
                duration);
            ObjectPullDrawMarker(
                overlay,
                gestureEnd,
                handRadius,
                255,
                210,
                50,
                220,
                duration);
        }
        // Pulling and Held intentionally draw nothing. The launched object is
        // no longer selected and follows normal Source physics immediately.
        if (drewVisual)
        {
            g_ObjectPullNextVisualUpdateAt = now +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(1.0f / 45.0f));
        }
    }

    const bool catchActionConsumed =
        holdingCatchActionDown &&
        (m_ObjectPullPhase == ObjectPullClientPhase::Pulling ||
            m_ObjectPullPhase == ObjectPullClientPhase::Held);

    if (m_ObjectPullPhase == ObjectPullClientPhase::Idle)
    {
        if (
            m_ObjectPullCancelRepeatUntil.time_since_epoch().count() != 0 &&
            now <= m_ObjectPullCancelRepeatUntil)
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireCancel;
        }
        else
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
        }
    }

    ObjectPullPublishWireCommand(
        m_ObjectPullDesiredWireCommand,
        m_ObjectPullWireTargetEntityIndex);
    return catchActionConsumed;
}

bool VR::GetObjectPullUsercmdData(
    int commandNumber,
    uint8_t& wireCommand,
    Vector& position,
    QAngle& angles,
    bool& overridePose,
    int& targetEntityIndex)
{
    wireCommand = kObjectPullWireNone;
    position = {};
    angles = {};
    overridePose = false;
    targetEntityIndex = 0;
    if (commandNumber <= 0)
        return false;

    constexpr uint32_t kCommandMask = 0xFFu;
    constexpr uint32_t kEntityMask = 0x7FFu;
    const uint32_t mailbox =
        g_ObjectPullWireMailbox.load(std::memory_order_acquire);
    const uint8_t publishedCommand = static_cast<uint8_t>(
        mailbox & kCommandMask);
    const int publishedEntityIndex = static_cast<int>(
        (mailbox >> 8) & kEntityMask);
    const size_t index =
        static_cast<size_t>(commandNumber) %
        kObjectPullUsercmdSnapshotCount;
    ObjectPullUsercmdSnapshot& snapshot =
        m_ObjectPullUsercmdSnapshots[index];

    // Once a usercmd has been assigned a wire payload, return the exact same
    // payload for Source backup-command retransmission. A newly published state
    // is first consumed by a later command number instead of mutating an old one.
    if (snapshot.valid && snapshot.commandNumber == commandNumber)
    {
        wireCommand = snapshot.wireCommand;
        targetEntityIndex = snapshot.targetEntityIndex;
        return wireCommand != kObjectPullWireNone;
    }

    if (publishedCommand == kObjectPullWireNone)
        return false;

    snapshot = {};
    snapshot.valid = true;
    snapshot.commandNumber = commandNumber;
    snapshot.wireCommand = publishedCommand;
    snapshot.targetEntityIndex = publishedEntityIndex;

    // The network thread supplies its own current controller pose. Object Pull
    // only crosses threads through the packed atomic command/entity mailbox.
    wireCommand = snapshot.wireCommand;
    targetEntityIndex = snapshot.targetEntityIndex;
    return true;
}
