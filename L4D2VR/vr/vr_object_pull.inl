namespace
{
    // Object Pull input is produced on the render/input path and consumed by
    // WriteUsercmd on the game/network path. Keep this state translation-unit
    // local so vr_object_pull.inl remains self-contained and cannot get out of
    // sync with VR class declarations.
    static std::atomic<uint32_t> g_ObjectPullWireMailbox{ 0u };
    static std::chrono::steady_clock::time_point g_ObjectPullNextTargetScanAt{};
    static std::chrono::steady_clock::time_point g_ObjectPullNextBroadScanAt{};

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
        VR::ObjectPullTargetHint targetHint =
            VR::ObjectPullTargetHint::None;
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

    class ICollideableObjectPullProbe
    {
    public:
        virtual IHandleEntity* GetEntityHandle() = 0;
        virtual const Vector& OBBMins() const = 0;
        virtual const Vector& OBBMaxs() const = 0;
    };

    static bool ObjectPullReadClientWorldBounds(
        C_BaseEntity* entity,
        Vector& outMins,
        Vector& outMaxs)
    {
        outMins = {};
        outMaxs = {};
        if (!entity)
            return false;

        Vector localMins{};
        Vector localMaxs{};
        Vector origin{};
        QAngle angles{};
#ifdef _MSC_VER
        __try
        {
            void* collideableVoid = entity->GetCollideable();
            if (!collideableVoid)
                return false;
            auto* collideable =
                reinterpret_cast<ICollideableObjectPullProbe*>(collideableVoid);
            localMins = collideable->OBBMins();
            localMaxs = collideable->OBBMaxs();
            origin = entity->GetAbsOrigin();
            angles = entity->GetAbsAngles();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        void* collideableVoid = entity->GetCollideable();
        if (!collideableVoid)
            return false;
        auto* collideable =
            reinterpret_cast<ICollideableObjectPullProbe*>(collideableVoid);
        localMins = collideable->OBBMins();
        localMaxs = collideable->OBBMaxs();
        origin = entity->GetAbsOrigin();
        angles = entity->GetAbsAngles();
#endif

        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(localMins[axis]) ||
                !std::isfinite(localMaxs[axis]) ||
                localMins[axis] > localMaxs[axis] ||
                localMaxs[axis] - localMins[axis] > 8192.0f ||
                !std::isfinite(origin[axis]) ||
                !std::isfinite(angles[axis]))
            {
                return false;
            }
        }

        const Vector localCenter = (localMins + localMaxs) * 0.5f;
        const Vector localHalf = (localMaxs - localMins) * 0.5f;
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);

        // Source's local Y basis is left, while AngleVectors returns right.
        const Vector worldCenter =
            origin +
            forward * localCenter.x -
            right * localCenter.y +
            up * localCenter.z;
        const Vector worldHalf{
            std::fabs(forward.x) * localHalf.x +
                std::fabs(right.x) * localHalf.y +
                std::fabs(up.x) * localHalf.z,
            std::fabs(forward.y) * localHalf.x +
                std::fabs(right.y) * localHalf.y +
                std::fabs(up.y) * localHalf.z,
            std::fabs(forward.z) * localHalf.x +
                std::fabs(right.z) * localHalf.y +
                std::fabs(up.z) * localHalf.z
        };
        outMins = worldCenter - worldHalf;
        outMaxs = worldCenter + worldHalf;
        return
            std::isfinite(outMins.x) &&
            std::isfinite(outMins.y) &&
            std::isfinite(outMins.z) &&
            std::isfinite(outMaxs.x) &&
            std::isfinite(outMaxs.y) &&
            std::isfinite(outMaxs.z);
    }

    static bool ObjectPullRayIntersectsWorldBounds(
        const Vector& start,
        const Vector& forward,
        const Vector& mins,
        const Vector& maxs,
        float minimumAlong,
        float maximumAlong,
        float& outAlong)
    {
        float entry = minimumAlong;
        float exit = maximumAlong;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float direction = forward[axis];
            if (std::fabs(direction) <= 0.000001f)
            {
                if (start[axis] < mins[axis] || start[axis] > maxs[axis])
                    return false;
                continue;
            }

            const float inverseDirection = 1.0f / direction;
            float first = (mins[axis] - start[axis]) * inverseDirection;
            float second = (maxs[axis] - start[axis]) * inverseDirection;
            if (first > second)
                std::swap(first, second);
            entry = std::max(entry, first);
            exit = std::min(exit, second);
            if (entry > exit)
                return false;
        }
        outAlong = entry;
        return std::isfinite(outAlong);
    }

    static bool ObjectPullClientClassIsSupported(const char* className)
    {
        if (!className || !*className)
            return false;

        static const char* const rejectedTokens[] =
        {
            "projectile", "player", "infected", "witch", "door", "button",
            "world", "ammospawn", "ammostack", "ammopile"
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

    static VR::ObjectPullTargetHint ObjectPullTargetHintFromModelName(
        const char* modelName)
    {
        if (!modelName || !*modelName)
            return VR::ObjectPullTargetHint::None;

        // These are the actual one-piece world models used by L4D2's carry
        // objectives. Do not classify on a loose "gascan"/"gnome" substring:
        // map scenery such as wooden_barricade_gascans is also prop_dynamic.
        if (ObjectPullContainsNormalizedToken(modelName, "gascan001a"))
            return VR::ObjectPullTargetHint::GasCan;
        if (ObjectPullContainsNormalizedToken(modelName, "propanecanister001a") ||
            ObjectPullContainsNormalizedToken(modelName, "propanecanister01a"))
        {
            return VR::ObjectPullTargetHint::PropaneTank;
        }
        if (ObjectPullContainsNormalizedToken(modelName, "oxygentank01"))
            return VR::ObjectPullTargetHint::OxygenTank;
        if (ObjectPullContainsNormalizedToken(modelName, "fireworkcrate"))
            return VR::ObjectPullTargetHint::FireworksBox;
        if (ObjectPullContainsNormalizedToken(modelName, "gnomemdl"))
            return VR::ObjectPullTargetHint::Gnome;
        if (ObjectPullContainsNormalizedToken(modelName, "wcolamdl"))
            return VR::ObjectPullTargetHint::ColaBottles;
        return VR::ObjectPullTargetHint::None;
    }

    static bool ObjectPullModelNameIsSupported(const char* modelName)
    {
        if (!modelName || !*modelName)
            return false;

        static const char* const rejectedTokens[] =
        {
            "survivor", "infected", "witch", "door", "vehicle", "building",
            "ammostack", "ammopile", "ammospawn"
        };
        for (const char* token : rejectedTokens)
        {
            if (ObjectPullContainsNormalizedToken(modelName, token))
                return false;
        }

        if (ObjectPullTargetHintFromModelName(modelName) !=
            VR::ObjectPullTargetHint::None)
        {
            return true;
        }

        static const char* const supportedTokens[] =
        {
            "wmodelsweapons", "weaponsmelee", "weapon", "items",
            "oildrum", "barrel", "upgradepack", "ammopack", "firstaid",
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

        out.targetHint = ObjectPullTargetHintFromModelName(out.modelName);
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

            Vector boundsMins{};
            Vector boundsMaxs{};
            if (!ObjectPullReadClientWorldBounds(
                    entity,
                    boundsMins,
                    boundsMaxs))
                continue;

            const Vector assist{
                assistRadius,
                assistRadius,
                assistRadius
            };
            float along = 0.0f;
            if (!ObjectPullRayIntersectsWorldBounds(
                    start,
                    forward,
                    boundsMins - assist,
                    boundsMaxs + assist,
                    minDistance,
                    maxDistance,
                    along))
                continue;

            const Vector rayPoint = start + forward * along;
            const Vector aimPoint{
                std::clamp(rayPoint.x, boundsMins.x, boundsMaxs.x),
                std::clamp(rayPoint.y, boundsMins.y, boundsMaxs.y),
                std::clamp(rayPoint.z, boundsMins.z, boundsMaxs.z)
            };
            const float perpendicular = (aimPoint - rayPoint).Length();
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

            const VR::ObjectPullTargetHint targetHint =
                ObjectPullTargetHintFromModelName(modelName);
            if (vr->m_Game->m_EngineTrace)
            {
                CTraceFilterSkipSelf filter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);
                Ray_t visibilityRay;
                visibilityRay.Init(start, aimPoint);
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
                        (aimPoint - visibilityTrace.endpos).Length();
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
            best.hitPosition = aimPoint;
            best.entityOrigin = origin;
            best.distanceMeters = along / scale;
            best.entityIndex = entityIndex;
            best.targetHint = targetHint;
            ObjectPullCopyClassName(className, best.className, sizeof(best.className));
            ObjectPullCopyClassName(modelName, best.modelName, sizeof(best.modelName));
        }
        return best;
    }

    static bool ObjectPullClientTargetStillPointedAt(
        VR* vr,
        C_BasePlayer* localPlayer,
        C_BaseEntity* target,
        const Vector& start,
        Vector forward)
    {
        if (!vr ||
            !localPlayer ||
            !target ||
            !vr->m_Game ||
            !vr->m_Game->m_EngineTrace ||
            VectorNormalize(forward) <= 0.0001f)
        {
            return false;
        }

        Vector boundsMins{};
        Vector boundsMaxs{};
        if (!ObjectPullReadClientWorldBounds(
                target,
                boundsMins,
                boundsMaxs))
        {
            return false;
        }

        const float scale = std::max(1.0f, vr->m_VRScale);
        const float assistRadius =
            std::max(0.0f, vr->m_ObjectPullTargetAssistRadiusMeters) *
            scale;
        const Vector assist{
            assistRadius,
            assistRadius,
            assistRadius
        };
        float along = 0.0f;
        if (!ObjectPullRayIntersectsWorldBounds(
                start,
                forward,
                boundsMins - assist,
                boundsMaxs + assist,
                std::max(0.0f, vr->m_ObjectPullMinimumDistanceMeters) *
                    scale,
                std::max(0.1f, vr->m_ObjectPullMaxDistanceMeters) *
                    scale,
                along))
        {
            return false;
        }

        const Vector rayPoint = start + forward * along;
        const Vector aimPoint{
            std::clamp(rayPoint.x, boundsMins.x, boundsMaxs.x),
            std::clamp(rayPoint.y, boundsMins.y, boundsMaxs.y),
            std::clamp(rayPoint.z, boundsMins.z, boundsMaxs.z)
        };
        CTraceFilterSkipSelf filter(
            reinterpret_cast<IHandleEntity*>(localPlayer),
            0);
        Ray_t visibilityRay;
        visibilityRay.Init(start, aimPoint);
        CGameTrace visibilityTrace{};
        if (VR_SafeTraceRay(
                vr->m_Game->m_EngineTrace,
                visibilityRay,
                MASK_SHOT_HULL,
                &filter,
                visibilityTrace) &&
            visibilityTrace.m_pEnt &&
            visibilityTrace.m_pEnt != reinterpret_cast<void*>(target) &&
            visibilityTrace.fraction < 0.97f)
        {
            return false;
        }
        return true;
    }

    struct ObjectPullNativeGlowOverride
    {
        bool offsetsResolved = false;
        bool active = false;
        int glowTypeOffset = -1;
        int glowRangeOffset = -1;
        int glowRangeMinOffset = -1;
        C_BaseEntity* entity = nullptr;
        void* entityVtable = nullptr;
        int entityIndex = 0;
        int originalGlowType = 0;
        int originalGlowRange = 0;
        int originalGlowRangeMin = 0;
    };

    static ObjectPullNativeGlowOverride g_ObjectPullNativeGlow{};

    static void ObjectPullResolveNativeGlowOffsets(VR* vr)
    {
        ObjectPullNativeGlowOverride& glow = g_ObjectPullNativeGlow;
        if (glow.offsetsResolved || !vr || !vr->m_Game)
            return;

        glow.offsetsResolved = true;
        static const char* const tableNames[] =
        {
            "DT_GlowProperty",
            "DT_BaseEntity",
            "CBaseEntity",
            "DT_BaseAnimating",
            "CBaseAnimating",
            "DT_BaseCombatWeapon",
            "CBaseCombatWeapon"
        };
        for (const char* tableName : tableNames)
        {
            const int typeOffset = vr->m_Game->FindRecvPropOffset(
                tableName,
                "m_iGlowType");
            const int rangeOffset = vr->m_Game->FindRecvPropOffset(
                tableName,
                "m_nGlowRange");
            if (typeOffset < 0 || rangeOffset < 0)
                continue;

            glow.glowTypeOffset = typeOffset;
            glow.glowRangeOffset = rangeOffset;
            glow.glowRangeMinOffset = vr->m_Game->FindRecvPropOffset(
                tableName,
                "m_nGlowRangeMin");
            break;
        }

        if (glow.glowTypeOffset < 0 ||
            glow.glowRangeOffset < 0 ||
            glow.glowRangeMinOffset < 0)
        {
            // The checked-in L4D2 netvar dump records m_Glow at 0x278 and
            // DT_GlowProperty's type/range/rangeMin fields at +4/+8/+C.
            // These fixed offsets avoid walking cyclic nested RecvTables at
            // map start. Values are sanity-checked on every new target before
            // any write occurs.
            glow.glowTypeOffset = 0x27C;
            glow.glowRangeOffset = 0x280;
            glow.glowRangeMinOffset = 0x284;
        }

        if (vr->m_ObjectPullDebugLog)
        {
            Game::logMsg(
                "[VR][ObjectPull][client] native glow offsets type=%d range=%d rangeMin=%d",
                glow.glowTypeOffset,
                glow.glowRangeOffset,
                glow.glowRangeMinOffset);
        }
    }

    static bool ObjectPullSafeReadNativeGlow(
        C_BaseEntity* entity,
        int typeOffset,
        int rangeOffset,
        int rangeMinOffset,
        int& outType,
        int& outRange,
        int& outRangeMin)
    {
        if (!entity || typeOffset < 0 || rangeOffset < 0)
            return false;
#ifdef _MSC_VER
        __try
        {
            const uint8_t* base =
                reinterpret_cast<const uint8_t*>(entity);
            outType = *reinterpret_cast<const int*>(base + typeOffset);
            outRange = *reinterpret_cast<const int*>(base + rangeOffset);
            outRangeMin = rangeMinOffset >= 0
                ? *reinterpret_cast<const int*>(base + rangeMinOffset)
                : 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        const uint8_t* base =
            reinterpret_cast<const uint8_t*>(entity);
        outType = *reinterpret_cast<const int*>(base + typeOffset);
        outRange = *reinterpret_cast<const int*>(base + rangeOffset);
        outRangeMin = rangeMinOffset >= 0
            ? *reinterpret_cast<const int*>(base + rangeMinOffset)
            : 0;
        return true;
#endif
    }

    static bool ObjectPullSafeWriteNativeGlow(
        VR* vr,
        C_BaseEntity* entity,
        int typeOffset,
        int rangeOffset,
        int rangeMinOffset,
        int glowType,
        int glowRange,
        int glowRangeMin)
    {
        if (!vr ||
            !vr->m_Game ||
            !vr->m_Game->m_Offsets ||
            !entity ||
            typeOffset < static_cast<int>(sizeof(void*)) ||
            rangeOffset < 0 ||
            !vr->m_Game->m_Offsets->
                CGlowProperty_SetGlowType_Client.valid)
        {
            return false;
        }
#ifdef _MSC_VER
        __try
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(entity);
            *reinterpret_cast<int*>(base + rangeOffset) = glowRange;
            if (rangeMinOffset >= 0)
            {
                *reinterpret_cast<int*>(base + rangeMinOffset) =
                    glowRangeMin;
            }

            // m_iGlowType sits at +4 in the embedded CGlowProperty. The native
            // setter registers or unregisters its glow objects before storing
            // the value, which a raw netvar write cannot do.
            void* glowProperty = base + typeOffset - sizeof(void*);
            if (!*reinterpret_cast<void**>(glowProperty))
                return false;
            using SetGlowTypeFn = void(__thiscall*)(void*, int);
            auto setGlowType = reinterpret_cast<SetGlowTypeFn>(
                vr->m_Game->m_Offsets->
                    CGlowProperty_SetGlowType_Client.address);
            setGlowType(glowProperty, glowType);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        uint8_t* base = reinterpret_cast<uint8_t*>(entity);
        *reinterpret_cast<int*>(base + rangeOffset) = glowRange;
        if (rangeMinOffset >= 0)
            *reinterpret_cast<int*>(base + rangeMinOffset) = glowRangeMin;
        void* glowProperty = base + typeOffset - sizeof(void*);
        if (!*reinterpret_cast<void**>(glowProperty))
            return false;
        using SetGlowTypeFn = void(__thiscall*)(void*, int);
        auto setGlowType = reinterpret_cast<SetGlowTypeFn>(
            vr->m_Game->m_Offsets->
                CGlowProperty_SetGlowType_Client.address);
        setGlowType(glowProperty, glowType);
        return true;
#endif
    }

    static bool ObjectPullNativeGlowEntityStillValid(
        VR* vr,
        const ObjectPullNativeGlowOverride& glow)
    {
        if (!vr ||
            !glow.entity ||
            !glow.entityVtable ||
            glow.entityIndex <= 0)
        {
            return false;
        }
        C_BaseEntity* current = ObjectPullSafeGetClientEntity(
            vr,
            glow.entityIndex);
        void* currentVtable = nullptr;
        return
            current == glow.entity &&
            ObjectPullReadClientVtable(current, currentVtable) &&
            currentVtable == glow.entityVtable;
    }

    static void ObjectPullRestoreNativeGlow(VR* vr)
    {
        ObjectPullNativeGlowOverride& glow = g_ObjectPullNativeGlow;
        if (!glow.active)
            return;

        if (ObjectPullNativeGlowEntityStillValid(vr, glow))
        {
            ObjectPullSafeWriteNativeGlow(
                vr,
                glow.entity,
                glow.glowTypeOffset,
                glow.glowRangeOffset,
                glow.glowRangeMinOffset,
                glow.originalGlowType,
                glow.originalGlowRange,
                glow.originalGlowRangeMin);
        }

        glow.active = false;
        glow.entity = nullptr;
        glow.entityVtable = nullptr;
        glow.entityIndex = 0;
    }

    static void ObjectPullApplyNativeGlow(
        VR* vr,
        C_BaseEntity* entity,
        void* entityVtable,
        int entityIndex)
    {
        if (!vr || !vr->m_ObjectPullVisualsEnabled || !entity)
        {
            ObjectPullRestoreNativeGlow(vr);
            return;
        }

        ObjectPullResolveNativeGlowOffsets(vr);
        ObjectPullNativeGlowOverride& glow = g_ObjectPullNativeGlow;
        if (glow.glowTypeOffset < 0 || glow.glowRangeOffset < 0)
            return;

        const bool sameTarget =
            glow.active &&
            glow.entity == entity &&
            glow.entityVtable == entityVtable &&
            glow.entityIndex == entityIndex;
        if (!sameTarget)
        {
            ObjectPullRestoreNativeGlow(vr);

            int originalType = 0;
            int originalRange = 0;
            int originalRangeMin = 0;
            if (!ObjectPullSafeReadNativeGlow(
                    entity,
                    glow.glowTypeOffset,
                    glow.glowRangeOffset,
                    glow.glowRangeMinOffset,
                    originalType,
                    originalRange,
                    originalRangeMin))
            {
                return;
            }

            const bool originalGlowValuesPlausible =
                originalType >= 0 &&
                originalType <= 3 &&
                originalRange >= 0 &&
                originalRange <= 100000 &&
                originalRangeMin >= 0 &&
                originalRangeMin <= 100000;
            if (!originalGlowValuesPlausible)
            {
                if (vr->m_ObjectPullDebugLog)
                {
                    Game::logMsg(
                        "[VR][ObjectPull][client] native glow refused implausible values entity=%p index=%d type=%d range=%d rangeMin=%d",
                        entity,
                        entityIndex,
                        originalType,
                        originalRange,
                        originalRangeMin);
                }
                return;
            }

            glow.active = true;
            glow.entity = entity;
            glow.entityVtable = entityVtable;
            glow.entityIndex = entityIndex;
            glow.originalGlowType = originalType;
            glow.originalGlowRange = originalRange;
            glow.originalGlowRangeMin = originalRangeMin;
            if (vr->m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] native glow override entity=%p index=%d originalType=%d originalRange=%d originalRangeMin=%d",
                    entity,
                    entityIndex,
                    originalType,
                    originalRange,
                    originalRangeMin);
            }
        }

        // Glow type 3 is Source's native constant outline. Keep its engine-unit
        // range identical to the configured controller-ray selection distance.
        // Only the currently visible target receives this temporary override,
        // and its original fields are restored when aim leaves or pull launches.
        const int glowRange = std::max(
            1,
            static_cast<int>(
                std::ceil(
                    std::max(0.1f, vr->m_ObjectPullMaxDistanceMeters) *
                    std::max(1.0f, vr->m_VRScale))));
        const bool glowWriteSucceeded =
            ObjectPullSafeWriteNativeGlow(
            vr,
            entity,
            glow.glowTypeOffset,
            glow.glowRangeOffset,
            glow.glowRangeMinOffset,
            3,
            glowRange,
            0);
        if (vr->m_ObjectPullDebugLog && !sameTarget)
        {
            int readbackType = -1;
            int readbackRange = -1;
            int readbackRangeMin = -1;
            const bool readbackSucceeded =
                glowWriteSucceeded &&
                ObjectPullSafeReadNativeGlow(
                    entity,
                    glow.glowTypeOffset,
                    glow.glowRangeOffset,
                    glow.glowRangeMinOffset,
                    readbackType,
                    readbackRange,
                    readbackRangeMin);
            Game::logMsg(
                "[VR][ObjectPull][client] native glow applied entity=%p index=%d write=%d read=%d type=%d range=%d rangeMin=%d",
                entity,
                entityIndex,
                glowWriteSucceeded ? 1 : 0,
                readbackSucceeded ? 1 : 0,
                readbackType,
                readbackRange,
                readbackRangeMin);
        }
    }

}

static void ObjectPullPublishWireCommand(
    uint8_t wireCommand,
    int targetEntityIndex,
    VR::ObjectPullTargetHint targetHint)
{
    constexpr uint32_t kCommandMask = 0xFFu;
    constexpr uint32_t kEntityMask = 0x7FFu;
    constexpr uint32_t kTargetHintMask = 0x7u;

    const uint32_t command = static_cast<uint32_t>(wireCommand) & kCommandMask;
    const uint32_t entity = static_cast<uint32_t>(
        std::clamp(targetEntityIndex, 0, 2047)) & kEntityMask;
    const uint32_t hint =
        static_cast<uint32_t>(targetHint) & kTargetHintMask;
    const uint32_t payload =
        command |
        (entity << 8) |
        (hint << 19);

    // A single release-store publishes command and entity index coherently.
    // Do not suppress repeated stores here: Begin/Continue reliability windows
    // intentionally republish the current state for later usercmd batches.
    g_ObjectPullWireMailbox.store(payload, std::memory_order_release);
}

void VR::ResetObjectPullInput(bool sendCancel)
{
    ObjectPullRestoreNativeGlow(this);
    m_ObjectPullPhase = ObjectPullClientPhase::Idle;
    m_ObjectPullClientTarget = nullptr;
    m_ObjectPullClientTargetVtable = nullptr;
    m_ObjectPullClientTargetEntityIndex = 0;
    m_ObjectPullWireTargetEntityIndex = 0;
    m_ObjectPullClientTargetHint = ObjectPullTargetHint::None;
    m_ObjectPullWireTargetHint = ObjectPullTargetHint::None;
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
            m_ObjectPullWireTargetEntityIndex,
            m_ObjectPullWireTargetHint);
    }
    else
    {
        m_ObjectPullRequireActionRelease = false;
        m_ObjectPullActionDownPrev = false;
        m_ObjectPullCancelRepeatUntil = {};
        m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
        ObjectPullPublishWireCommand(
            kObjectPullWireNone,
            0,
            ObjectPullTargetHint::None);
    }
}

bool VR::UpdateObjectPullInput(
    C_BasePlayer* localPlayer,
    bool gripActionDown)
{
    const auto now = std::chrono::steady_clock::now();
    const bool targetScanDue =
        g_ObjectPullNextTargetScanAt.time_since_epoch().count() == 0 ||
        now >= g_ObjectPullNextTargetScanAt;
    const bool broadScanDue =
        g_ObjectPullNextBroadScanAt.time_since_epoch().count() == 0 ||
        now >= g_ObjectPullNextBroadScanAt;
    const bool actionJustPressed =
        gripActionDown && !m_ObjectPullActionDownPrev;
    const bool actionJustReleased =
        !gripActionDown && m_ObjectPullActionDownPrev;
    m_ObjectPullActionDownPrev = gripActionDown;
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

    const bool holdingCatchActionDown = gripActionDown;
    if ((actionJustPressed || actionJustReleased) && m_ObjectPullDebugLog)
    {
        Game::logMsg(
            "[VR][ObjectPull][client] grip %s hand=%s",
            actionJustPressed ? "pressed" : "released",
            gunHandPhysicalLeft ? "left" : "right");
    }

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

        if (m_ObjectPullPhase != ObjectPullClientPhase::Idle ||
            g_ObjectPullNativeGlow.active)
        {
            const bool hadServerFlight =
                m_ObjectPullPhase == ObjectPullClientPhase::Pulling ||
                m_ObjectPullPhase == ObjectPullClientPhase::Held;
            ResetObjectPullInput(hadServerFlight);
            m_ObjectPullActionDownPrev = gripActionDown;
            m_ObjectPullRequireActionRelease = gripActionDown;
        }
        else if (
            m_ObjectPullCancelRepeatUntil.time_since_epoch().count() == 0 ||
            now > m_ObjectPullCancelRepeatUntil)
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
            ObjectPullPublishWireCommand(
                kObjectPullWireNone,
                0,
                ObjectPullTargetHint::None);
        }
        return false;
    }

    ObjectPullClientTraceResult preview{};
    if ((m_ObjectPullPhase == ObjectPullClientPhase::Idle ||
            m_ObjectPullPhase == ObjectPullClientPhase::Targeting) &&
        (actionJustPressed || targetScanDue))
    {
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
            const bool targetChanged =
                m_ObjectPullClientTarget != preview.entity ||
                m_ObjectPullClientTargetEntityIndex != preview.entityIndex;
            m_ObjectPullPhase = ObjectPullClientPhase::Targeting;
            m_ObjectPullClientTarget = preview.entity;
            m_ObjectPullClientTargetVtable = preview.vtable;
            m_ObjectPullClientTargetEntityIndex = preview.entityIndex;
            m_ObjectPullClientTargetHint = preview.targetHint;
            m_ObjectPullClientTargetPoint = preview.hitPosition;
            m_ObjectPullLastTargetDistanceMeters =
                (gunHandPosition - preview.hitPosition).Length() /
                std::max(1.0f, m_VRScale);
            ObjectPullApplyNativeGlow(
                this,
                preview.entity,
                preview.vtable,
                preview.entityIndex);

            if (targetChanged && m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] pointed target entity=%p index=%d class=%s model=%s distance=%.2fm assist=%.2fm hand=%s",
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
        else
        {
            const bool keepCurrentTarget =
                m_ObjectPullPhase == ObjectPullClientPhase::Targeting &&
                ObjectPullClientTargetStillPointedAt(
                    this,
                    localPlayer,
                    m_ObjectPullClientTarget,
                    gunHandPosition,
                    gunHandForward);
            if (!keepCurrentTarget)
            {
                ObjectPullRestoreNativeGlow(this);
                m_ObjectPullPhase = ObjectPullClientPhase::Idle;
                m_ObjectPullClientTarget = nullptr;
                m_ObjectPullClientTargetVtable = nullptr;
                m_ObjectPullClientTargetEntityIndex = 0;
                m_ObjectPullClientTargetHint = ObjectPullTargetHint::None;
                m_ObjectPullClientTargetPoint = {};
            }

            if (actionJustPressed && m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] grip pressed without pointed pull target hit=%d index=%d class=%s model=%s distance=%.2fm min=%.2fm max=%.2fm",
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
    }

    if (m_ObjectPullPhase == ObjectPullClientPhase::Targeting)
    {
        void* currentVtable = nullptr;
        const bool targetValid =
            ObjectPullReadClientVtable(
                m_ObjectPullClientTarget,
                currentVtable) &&
            currentVtable == m_ObjectPullClientTargetVtable;
        if (!targetValid)
        {
            ObjectPullRestoreNativeGlow(this);
            m_ObjectPullPhase = ObjectPullClientPhase::Idle;
            m_ObjectPullClientTarget = nullptr;
            m_ObjectPullClientTargetVtable = nullptr;
            m_ObjectPullClientTargetEntityIndex = 0;
            m_ObjectPullClientTargetHint = ObjectPullTargetHint::None;
            m_ObjectPullClientTargetPoint = {};
        }
        else
        {
            ObjectPullApplyNativeGlow(
                this,
                m_ObjectPullClientTarget,
                m_ObjectPullClientTargetVtable,
                m_ObjectPullClientTargetEntityIndex);
        }

        if (m_ObjectPullPhase == ObjectPullClientPhase::Targeting &&
            actionJustPressed &&
            !m_ObjectPullRequireActionRelease)
        {
            // Pointing alone owns selection and native outline. Grip snapshots
            // the gesture origin only when the player chooses to pull.
            ObjectPullTrackingSample armTracking{};
            if (ObjectPullReadLiveTrackingSample(
                    this,
                    gunHandPhysicalLeft,
                    armTracking))
            {
                m_ObjectPullPhase = ObjectPullClientPhase::Armed;
                m_ObjectPullArmPosition = gunHandPosition;
                m_ObjectPullArmHandRelativeToHmd =
                    armTracking.handRelativeToHmd;
                m_ObjectPullLastHandRelativeToHmd =
                    m_ObjectPullArmHandRelativeToHmd;
                m_ObjectPullArmTowardBodyDirection =
                    m_ObjectPullArmHandRelativeToHmd * -1.0f;
                if (VectorNormalize(
                        m_ObjectPullArmTowardBodyDirection) <= 0.0001f)
                {
                    m_ObjectPullArmTowardBodyDirection = {};
                }
                m_ObjectPullArmForward = gunHandForward;
                m_ObjectPullArmAngles = gunHandAngles;
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
                        "[VR][ObjectPull][client] grip armed pointed entity=%p index=%d distance=%.2fm hand=%s",
                        m_ObjectPullClientTarget,
                        m_ObjectPullClientTargetEntityIndex,
                        m_ObjectPullLastTargetDistanceMeters,
                        gunHandPhysicalLeft ? "left" : "right");
                }
            }
            else if (m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] grip pressed on pointed target but live tracking pose is unavailable");
            }
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
            ResetObjectPullInput(false);
            m_ObjectPullActionDownPrev = gripActionDown;
            m_ObjectPullRequireActionRelease = gripActionDown;
        }
        else
        {
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
                    m_ObjectPullPhase = ObjectPullClientPhase::Targeting;
                    m_ObjectPullGestureSampleTime = {};
                    m_ObjectPullGesturePeakDistanceMeters = 0.0f;
                    m_ObjectPullGesturePeakSpeedMetersPerSecond = 0.0f;
                    ObjectPullApplyNativeGlow(
                        this,
                        m_ObjectPullClientTarget,
                        m_ObjectPullClientTargetVtable,
                        m_ObjectPullClientTargetEntityIndex);
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
                // The native selection outline ends on the trigger frame.
                // Begin uses the original pointing pose for server validation;
                // Continue uses the current pose as a fixed ballistic target.
                m_ObjectPullPhase = ObjectPullClientPhase::Pulling;
                m_ObjectPullWireTargetEntityIndex = m_ObjectPullClientTargetEntityIndex;
                m_ObjectPullWireTargetHint = m_ObjectPullClientTargetHint;
                ObjectPullRestoreNativeGlow(this);
                m_ObjectPullClientTarget = nullptr;
                m_ObjectPullClientTargetVtable = nullptr;
                m_ObjectPullClientTargetEntityIndex = 0;
                m_ObjectPullClientTargetHint = ObjectPullTargetHint::None;
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
                        "[VR][ObjectPull][client] grip released before pull peakDistance=%.3fm peakSpeed=%.2fm/s currentDistance=%.3fm threshold=%.3fm; returning to pointed target",
                        m_ObjectPullGesturePeakDistanceMeters,
                        m_ObjectPullGesturePeakSpeedMetersPerSecond,
                        trackingDisplacementMeters,
                        configuredDistance);
                }
                m_ObjectPullPhase = ObjectPullClientPhase::Targeting;
                m_ObjectPullGestureSampleTime = {};
                m_ObjectPullGesturePeakDistanceMeters = 0.0f;
                m_ObjectPullGesturePeakSpeedMetersPerSecond = 0.0f;
                m_ObjectPullDesiredWireCommand = kObjectPullWireNone;
                ObjectPullApplyNativeGlow(
                    this,
                    m_ObjectPullClientTarget,
                    m_ObjectPullClientTargetVtable,
                    m_ObjectPullClientTargetEntityIndex);
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
                    "[VR][ObjectPull][client] grip catch requested");
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
                Game::logMsg("[VR][ObjectPull][client] grip released after catch");
            ResetObjectPullInput(true);
        }
        else
        {
            m_ObjectPullDesiredWireCommand = kObjectPullWireCatch;
        }
    }

    const bool catchActionConsumed =
        holdingCatchActionDown &&
        (m_ObjectPullPhase == ObjectPullClientPhase::Armed ||
            m_ObjectPullPhase == ObjectPullClientPhase::Pulling ||
            m_ObjectPullPhase == ObjectPullClientPhase::Held);

    if (m_ObjectPullPhase == ObjectPullClientPhase::Idle ||
        m_ObjectPullPhase == ObjectPullClientPhase::Targeting ||
        m_ObjectPullPhase == ObjectPullClientPhase::Armed)
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
        m_ObjectPullWireTargetEntityIndex,
        m_ObjectPullWireTargetHint);
    return catchActionConsumed;
}

bool VR::GetObjectPullUsercmdData(
    int commandNumber,
    uint8_t& wireCommand,
    Vector& position,
    QAngle& angles,
    bool& overridePose,
    int& targetEntityIndex,
    ObjectPullTargetHint& targetHint)
{
    wireCommand = kObjectPullWireNone;
    position = {};
    angles = {};
    overridePose = false;
    targetEntityIndex = 0;
    targetHint = ObjectPullTargetHint::None;
    if (commandNumber <= 0)
        return false;

    constexpr uint32_t kCommandMask = 0xFFu;
    constexpr uint32_t kEntityMask = 0x7FFu;
    constexpr uint32_t kTargetHintMask = 0x7u;
    const uint32_t mailbox =
        g_ObjectPullWireMailbox.load(std::memory_order_acquire);
    const uint8_t publishedCommand = static_cast<uint8_t>(
        mailbox & kCommandMask);
    const int publishedEntityIndex = static_cast<int>(
        (mailbox >> 8) & kEntityMask);
    const ObjectPullTargetHint publishedTargetHint =
        static_cast<ObjectPullTargetHint>(
            (mailbox >> 19) & kTargetHintMask);
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
        targetHint = snapshot.targetHint;
        return wireCommand != kObjectPullWireNone;
    }

    if (publishedCommand == kObjectPullWireNone)
        return false;

    snapshot = {};
    snapshot.valid = true;
    snapshot.commandNumber = commandNumber;
    snapshot.wireCommand = publishedCommand;
    snapshot.targetEntityIndex = publishedEntityIndex;
    snapshot.targetHint = publishedTargetHint;

    // The network thread supplies its own current controller pose. Object Pull
    // only crosses threads through the packed atomic command/entity mailbox.
    wireCommand = snapshot.wireCommand;
    targetEntityIndex = snapshot.targetEntityIndex;
    targetHint = snapshot.targetHint;
    return true;
}
