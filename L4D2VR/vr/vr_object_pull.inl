namespace
{
    // Controller input is produced on the render/input path. Client entity
    // inspection is requested through a mutex-protected snapshot and performed
    // by CreateMove on the Source client thread. Wire commands are consumed by
    // WriteUsercmd on the game/network path.
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
            int blockerIndex = -1;
            C_BaseEntity* blocker = ObjectPullResolveTraceEntity(
                vr,
                visibilityTrace.m_pEnt,
                blockerIndex);
            if (!blocker)
                return false;

            char blockerClass[128]{};
            char blockerModel[260]{};
            ObjectPullSafeCopyNetworkClassName(
                vr,
                blocker,
                blockerClass,
                sizeof(blockerClass));
            if (!blockerClass[0])
            {
                ObjectPullSafeCopyModelName(
                    vr,
                    blocker,
                    blockerModel,
                    sizeof(blockerModel));
            }

            const bool livingBlocker =
                ObjectPullClientNameIsLivingBlocker(
                    blockerClass,
                    blockerModel);
            const float obstructionGap =
                (aimPoint - visibilityTrace.endpos).Length();
            const float embeddedTolerance =
                std::max(assistRadius * 2.0f, 0.20f * scale);
            if (!livingBlocker &&
                obstructionGap > embeddedTolerance)
            {
                return false;
            }
        }
        return true;
    }

    struct ObjectPullNativeGlowOverride
    {
        bool active = false;
        VR* owner = nullptr;
        C_BaseEntity* entity = nullptr;
        void* entityVtable = nullptr;
        int entityIndex = 0;
        int originalGlowType = 0;
        int originalGlowRange = 0;
        int originalGlowRangeMin = 0;
    };

    static ObjectPullNativeGlowOverride g_ObjectPullNativeGlow{};

    // DT_BaseEntity contains m_Glow at 0x278. DT_GlowProperty stores
    // m_iGlowType/m_nGlowRange/m_nGlowRangeMin at +4/+8/+C.
    static constexpr int kObjectPullGlowTypeOffset = 0x27C;
    static constexpr int kObjectPullGlowRangeOffset = 0x280;
    static constexpr int kObjectPullGlowRangeMinOffset = 0x284;

    static bool ObjectPullSafeReadNativeGlow(
        C_BaseEntity* entity,
        int& outType,
        int& outRange,
        int& outRangeMin)
    {
        if (!entity)
            return false;

#ifdef _MSC_VER
        __try
        {
            const uint8_t* base =
                reinterpret_cast<const uint8_t*>(entity);
            outType = *reinterpret_cast<const int*>(
                base + kObjectPullGlowTypeOffset);
            outRange = *reinterpret_cast<const int*>(
                base + kObjectPullGlowRangeOffset);
            outRangeMin = *reinterpret_cast<const int*>(
                base + kObjectPullGlowRangeMinOffset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        const uint8_t* base =
            reinterpret_cast<const uint8_t*>(entity);
        outType = *reinterpret_cast<const int*>(
            base + kObjectPullGlowTypeOffset);
        outRange = *reinterpret_cast<const int*>(
            base + kObjectPullGlowRangeOffset);
        outRangeMin = *reinterpret_cast<const int*>(
            base + kObjectPullGlowRangeMinOffset);
        return true;
#endif
    }

    static bool ObjectPullSafeWriteNativeGlow(
        VR* vr,
        C_BaseEntity* entity,
        int glowType,
        int glowRange,
        int glowRangeMin)
    {
        if (!vr ||
            !vr->m_Game ||
            !vr->m_Game->m_Offsets ||
            !entity ||
            !vr->m_Game->m_Offsets->
                CGlowProperty_SetGlowType_Client.valid)
        {
            return false;
        }

#ifdef _MSC_VER
        __try
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(entity);
            void* glowProperty = base + 0x278;
            if (!*reinterpret_cast<void**>(glowProperty))
                return false;

            *reinterpret_cast<int*>(
                base + kObjectPullGlowRangeOffset) = glowRange;
            *reinterpret_cast<int*>(
                base + kObjectPullGlowRangeMinOffset) = glowRangeMin;

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
        void* glowProperty = base + 0x278;
        if (!*reinterpret_cast<void**>(glowProperty))
            return false;

        *reinterpret_cast<int*>(
            base + kObjectPullGlowRangeOffset) = glowRange;
        *reinterpret_cast<int*>(
            base + kObjectPullGlowRangeMinOffset) = glowRangeMin;

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
            glow.owner != vr ||
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
        ObjectPullNativeGlowOverride& glow =
            g_ObjectPullNativeGlow;
        if (!glow.active)
            return;

        if (ObjectPullNativeGlowEntityStillValid(vr, glow))
        {
            ObjectPullSafeWriteNativeGlow(
                vr,
                glow.entity,
                glow.originalGlowType,
                glow.originalGlowRange,
                glow.originalGlowRangeMin);
        }

        glow = {};
    }

    static void ObjectPullApplyNativeGlow(
        VR* vr,
        C_BaseEntity* entity,
        void* entityVtable,
        int entityIndex)
    {
        if (!vr ||
            !vr->m_ObjectPullVisualsEnabled ||
            !entity ||
            !entityVtable ||
            entityIndex <= 0)
        {
            ObjectPullRestoreNativeGlow(vr);
            return;
        }

        ObjectPullNativeGlowOverride& glow =
            g_ObjectPullNativeGlow;
        const bool sameTarget =
            glow.active &&
            glow.owner == vr &&
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
                    originalType,
                    originalRange,
                    originalRangeMin))
            {
                return;
            }

            if (originalType < 0 ||
                originalType > 3 ||
                originalRange < 0 ||
                originalRange > 100000 ||
                originalRangeMin < 0 ||
                originalRangeMin > 100000)
            {
                return;
            }

            glow.active = true;
            glow.owner = vr;
            glow.entity = entity;
            glow.entityVtable = entityVtable;
            glow.entityIndex = entityIndex;
            glow.originalGlowType = originalType;
            glow.originalGlowRange = originalRange;
            glow.originalGlowRangeMin = originalRangeMin;
        }

        const int glowRange = std::max(
            1,
            static_cast<int>(
                std::ceil(
                    std::max(
                        0.1f,
                        vr->m_ObjectPullMaxDistanceMeters) *
                    std::max(1.0f, vr->m_VRScale))));

        const bool applied = ObjectPullSafeWriteNativeGlow(
            vr,
            entity,
            3,
            glowRange,
            0);
        if (!applied)
        {
            glow = {};
            return;
        }

        if (vr->m_ObjectPullDebugLog && !sameTarget)
        {
            Game::logMsg(
                "[VR][ObjectPull][client] native glow applied on main thread entity=%p index=%d originalType=%d range=%d",
                entity,
                entityIndex,
                glow.originalGlowType,
                glowRange);
        }
    }

    struct ObjectPullMainThreadTargetState
    {
        VR* owner = nullptr;
        C_BaseEntity* entity = nullptr;
        void* entityVtable = nullptr;
        int entityIndex = 0;
        VR::ObjectPullTargetHint targetHint =
            VR::ObjectPullTargetHint::None;
        Vector hitPosition = { 0.0f, 0.0f, 0.0f };
        float distanceMeters = 0.0f;
        char className[128]{};
        char modelName[260]{};
    };

    static ObjectPullMainThreadTargetState g_ObjectPullMainThreadTarget{};

    static uint32_t ObjectPullAdvanceTargetRequestEpoch(uint32_t epoch)
    {
        ++epoch;
        return epoch != 0u ? epoch : 1u;
    }

    static void ObjectPullClearMainThreadTargetState(VR* vr)
    {
        ObjectPullRestoreNativeGlow(vr);
        g_ObjectPullMainThreadTarget = {};
        g_ObjectPullMainThreadTarget.owner = vr;
    }

    static void ObjectPullPublishClientTargetRequest(
        VR* vr,
        VR::ObjectPullClientTargetRequestMode mode,
        const Vector& start,
        Vector forward,
        bool forceBroadScan,
        int heldEntityIndex)
    {
        if (!vr)
            return;

        if (mode != VR::ObjectPullClientTargetRequestMode::Disabled &&
            VectorNormalize(forward) <= 0.0001f)
        {
            mode = VR::ObjectPullClientTargetRequestMode::Disabled;
            heldEntityIndex = 0;
            forceBroadScan = false;
        }

        std::lock_guard<std::mutex> lock(
            vr->m_ObjectPullClientTargetStateMutex);
        VR::ObjectPullClientTargetRequest& request =
            vr->m_ObjectPullClientTargetRequest;
        const bool requestIdentityChanged =
            request.mode != mode ||
            (mode == VR::ObjectPullClientTargetRequestMode::Hold &&
                request.heldEntityIndex != heldEntityIndex);
        if (requestIdentityChanged)
        {
            const VR::ObjectPullClientTargetSnapshot previousSnapshot =
                vr->m_ObjectPullClientTargetSnapshot;
            const bool preserveCurrentTarget =
                previousSnapshot.epoch == request.epoch &&
                previousSnapshot.valid &&
                ((request.mode ==
                        VR::ObjectPullClientTargetRequestMode::Scan &&
                    mode ==
                        VR::ObjectPullClientTargetRequestMode::Hold &&
                    previousSnapshot.entityIndex == heldEntityIndex) ||
                    (request.mode ==
                            VR::ObjectPullClientTargetRequestMode::Hold &&
                        mode ==
                            VR::ObjectPullClientTargetRequestMode::Scan));

            request.epoch = ObjectPullAdvanceTargetRequestEpoch(
                request.epoch);
            request.forceBroadScan = false;
            if (preserveCurrentTarget)
            {
                vr->m_ObjectPullClientTargetSnapshot = previousSnapshot;
                vr->m_ObjectPullClientTargetSnapshot.epoch = request.epoch;
            }
            else
            {
                vr->m_ObjectPullClientTargetSnapshot = {};
                vr->m_ObjectPullClientTargetSnapshot.epoch = request.epoch;
            }
        }

        request.mode = mode;
        request.visualsEnabled = vr->m_ObjectPullVisualsEnabled;
        request.heldEntityIndex =
            mode == VR::ObjectPullClientTargetRequestMode::Hold
                ? heldEntityIndex
                : 0;
        request.start = start;
        request.forward = forward;
        if (mode == VR::ObjectPullClientTargetRequestMode::Disabled)
        {
            request.forceBroadScan = false;
            vr->m_ObjectPullClientTargetSnapshot = {};
            vr->m_ObjectPullClientTargetSnapshot.epoch = request.epoch;
        }
        else if (forceBroadScan)
        {
            request.forceBroadScan = true;
        }
    }

    static bool ObjectPullCopyClientTargetRequestForMainThread(
        VR* vr,
        VR::ObjectPullClientTargetRequest& out)
    {
        if (!vr)
            return false;

        std::lock_guard<std::mutex> lock(
            vr->m_ObjectPullClientTargetStateMutex);
        out = vr->m_ObjectPullClientTargetRequest;
        if (out.forceBroadScan &&
            vr->m_ObjectPullClientTargetRequest.epoch == out.epoch)
        {
            vr->m_ObjectPullClientTargetRequest.forceBroadScan = false;
        }
        return true;
    }

    static bool ObjectPullReadPublishedClientTargetSnapshot(
        VR* vr,
        VR::ObjectPullClientTargetSnapshot& out)
    {
        out = {};
        if (!vr)
            return false;

        std::lock_guard<std::mutex> lock(
            vr->m_ObjectPullClientTargetStateMutex);
        const VR::ObjectPullClientTargetRequest& request =
            vr->m_ObjectPullClientTargetRequest;
        const VR::ObjectPullClientTargetSnapshot& snapshot =
            vr->m_ObjectPullClientTargetSnapshot;
        if (request.mode ==
                VR::ObjectPullClientTargetRequestMode::Disabled ||
            snapshot.epoch != request.epoch)
        {
            return false;
        }

        out = snapshot;
        return true;
    }

    static bool ObjectPullMainThreadTargetStillValid(
        VR* vr,
        ObjectPullMainThreadTargetState& state)
    {
        if (!vr ||
            state.owner != vr ||
            !state.entity ||
            !state.entityVtable ||
            state.entityIndex <= 0)
        {
            return false;
        }

        C_BaseEntity* current = ObjectPullSafeGetClientEntity(
            vr,
            state.entityIndex);
        void* currentVtable = nullptr;
        return
            current == state.entity &&
            ObjectPullReadClientVtable(current, currentVtable) &&
            currentVtable == state.entityVtable;
    }

    static void ObjectPullSetMainThreadTarget(
        VR* vr,
        const ObjectPullClientTraceResult& trace)
    {
        ObjectPullMainThreadTargetState& state =
            g_ObjectPullMainThreadTarget;
        state = {};
        state.owner = vr;
        state.entity = trace.entity;
        state.entityVtable = trace.vtable;
        state.entityIndex = trace.entityIndex;
        state.targetHint = trace.targetHint;
        state.hitPosition = trace.hitPosition;
        state.distanceMeters = trace.distanceMeters;
        ObjectPullCopyClassName(
            trace.className,
            state.className,
            sizeof(state.className));
        ObjectPullCopyClassName(
            trace.modelName,
            state.modelName,
            sizeof(state.modelName));
    }

    static void ObjectPullPublishClientTargetSnapshotFromMainThread(
        VR* vr,
        const VR::ObjectPullClientTargetRequest& request,
        const ObjectPullMainThreadTargetState* state,
        const ObjectPullClientTraceResult* diagnostic)
    {
        if (!vr)
            return;

        VR::ObjectPullClientTargetSnapshot snapshot{};
        snapshot.epoch = request.epoch;
        if (state &&
            state->owner == vr &&
            state->entity &&
            state->entityVtable &&
            state->entityIndex > 0)
        {
            snapshot.valid = true;
            snapshot.hitAnything = true;
            snapshot.entityIndex = state->entityIndex;
            snapshot.targetHint = state->targetHint;
            snapshot.hitPosition = state->hitPosition;
            snapshot.distanceMeters = state->distanceMeters;
            snapshot.entityAddress = reinterpret_cast<std::uintptr_t>(
                state->entity);
            snapshot.entityVtable = reinterpret_cast<std::uintptr_t>(
                state->entityVtable);
            ObjectPullCopyClassName(
                state->className,
                snapshot.className,
                sizeof(snapshot.className));
            ObjectPullCopyClassName(
                state->modelName,
                snapshot.modelName,
                sizeof(snapshot.modelName));
        }
        else if (diagnostic)
        {
            snapshot.hitAnything = diagnostic->hitAnything;
            snapshot.entityIndex = std::max(0, diagnostic->entityIndex);
            snapshot.targetHint = diagnostic->targetHint;
            snapshot.hitPosition = diagnostic->hitPosition;
            snapshot.distanceMeters = diagnostic->distanceMeters;
            ObjectPullCopyClassName(
                diagnostic->className,
                snapshot.className,
                sizeof(snapshot.className));
            ObjectPullCopyClassName(
                diagnostic->modelName,
                snapshot.modelName,
                sizeof(snapshot.modelName));
        }

        std::lock_guard<std::mutex> lock(
            vr->m_ObjectPullClientTargetStateMutex);
        const VR::ObjectPullClientTargetRequest& currentRequest =
            vr->m_ObjectPullClientTargetRequest;
        if (currentRequest.epoch != request.epoch ||
            currentRequest.mode != request.mode ||
            (request.mode ==
                    VR::ObjectPullClientTargetRequestMode::Hold &&
                currentRequest.heldEntityIndex !=
                    request.heldEntityIndex))
        {
            return;
        }
        vr->m_ObjectPullClientTargetSnapshot = snapshot;
    }

}

void VR::UpdateObjectPullClientTargetMainThread(
    C_BasePlayer* localPlayer)
{
    ObjectPullClientTargetRequest request{};
    if (!ObjectPullCopyClientTargetRequestForMainThread(
            this,
            request))
    {
        return;
    }

    ObjectPullMainThreadTargetState& state =
        g_ObjectPullMainThreadTarget;
    if (state.owner != this)
    {
        ObjectPullClearMainThreadTargetState(this);
        g_ObjectPullNextTargetScanAt = {};
        g_ObjectPullNextBroadScanAt = {};
    }

    const bool featureAvailable =
        request.mode != ObjectPullClientTargetRequestMode::Disabled &&
        localPlayer &&
        m_Game &&
        m_Game->m_EngineTrace &&
        m_Game->m_ClientEntityList;
    if (!featureAvailable)
    {
        ObjectPullClearMainThreadTargetState(this);
        g_ObjectPullNextTargetScanAt = {};
        g_ObjectPullNextBroadScanAt = {};
        ObjectPullPublishClientTargetSnapshotFromMainThread(
            this,
            request,
            nullptr,
            nullptr);
        return;
    }

    Vector forward = request.forward;
    if (!std::isfinite(request.start.x) ||
        !std::isfinite(request.start.y) ||
        !std::isfinite(request.start.z) ||
        VectorNormalize(forward) <= 0.0001f)
    {
        ObjectPullClearMainThreadTargetState(this);
        ObjectPullPublishClientTargetSnapshotFromMainThread(
            this,
            request,
            nullptr,
            nullptr);
        return;
    }

    bool currentTargetValid =
        ObjectPullMainThreadTargetStillValid(this, state);
    if (!currentTargetValid)
        ObjectPullClearMainThreadTargetState(this);

    if (request.mode == ObjectPullClientTargetRequestMode::Hold)
    {
        if (!currentTargetValid ||
            request.heldEntityIndex <= 0 ||
            state.entityIndex != request.heldEntityIndex)
        {
            ObjectPullClearMainThreadTargetState(this);
        }

        ObjectPullApplyNativeGlow(
            this,
            state.entity,
            state.entityVtable,
            state.entityIndex);
        ObjectPullPublishClientTargetSnapshotFromMainThread(
            this,
            request,
            state.entity ? &state : nullptr,
            nullptr);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool targetScanDue =
        request.forceBroadScan ||
        !state.entity ||
        g_ObjectPullNextTargetScanAt.time_since_epoch().count() == 0 ||
        now >= g_ObjectPullNextTargetScanAt;
    ObjectPullClientTraceResult diagnostic{};
    const ObjectPullClientTraceResult* diagnosticPtr = nullptr;
    if (targetScanDue)
    {
        g_ObjectPullNextTargetScanAt = now +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / 60.0f));
        const bool broadScanDue =
            g_ObjectPullNextBroadScanAt.time_since_epoch().count() == 0 ||
            now >= g_ObjectPullNextBroadScanAt;
        const bool allowBroadScan =
            request.forceBroadScan || broadScanDue;
        if (allowBroadScan)
        {
            // The fallback list walk is only needed for pickups that do not
            // participate in MASK_SHOT_HULL. Ten scans per second keeps those
            // targets responsive without probing 192 arbitrary entities every
            // render frame.
            g_ObjectPullNextBroadScanAt = now +
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(0.10f));
        }

        diagnostic = ObjectPullTraceClientTarget(
            this,
            localPlayer,
            request.start,
            forward,
            allowBroadScan);
        diagnosticPtr = &diagnostic;
        if (diagnostic.supported)
        {
            ObjectPullSetMainThreadTarget(this, diagnostic);
            if (!ObjectPullMainThreadTargetStillValid(this, state))
                ObjectPullClearMainThreadTargetState(this);
        }
        else
        {
            currentTargetValid =
                ObjectPullMainThreadTargetStillValid(this, state);
            if (!currentTargetValid ||
                !ObjectPullClientTargetStillPointedAt(
                    this,
                    localPlayer,
                    state.entity,
                    request.start,
                    forward))
            {
                ObjectPullClearMainThreadTargetState(this);
            }
        }
    }

    ObjectPullApplyNativeGlow(
        this,
        state.entity,
        state.entityVtable,
        state.entityIndex);
    ObjectPullPublishClientTargetSnapshotFromMainThread(
        this,
        request,
        state.entity ? &state : nullptr,
        diagnosticPtr);
}

bool VR::GetObjectPullNativeHighlightIdentity(
    int& entityIndex,
    std::uintptr_t& entityAddress,
    std::uintptr_t& entityVtable)
{
    entityIndex = 0;
    entityAddress = 0;
    entityVtable = 0;

    std::lock_guard<std::mutex> lock(
        m_ObjectPullClientTargetStateMutex);
    const ObjectPullClientTargetRequest& request =
        m_ObjectPullClientTargetRequest;
    const ObjectPullClientTargetSnapshot& snapshot =
        m_ObjectPullClientTargetSnapshot;
    if (request.mode == ObjectPullClientTargetRequestMode::Disabled ||
        !request.visualsEnabled ||
        snapshot.epoch != request.epoch ||
        !snapshot.valid ||
        snapshot.entityIndex <= 0 ||
        snapshot.entityAddress == 0 ||
        snapshot.entityVtable == 0)
    {
        return false;
    }

    entityIndex = snapshot.entityIndex;
    entityAddress = snapshot.entityAddress;
    entityVtable = snapshot.entityVtable;
    return true;
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
    ObjectPullPublishClientTargetRequest(
        this,
        ObjectPullClientTargetRequestMode::Disabled,
        {},
        {},
        false,
        0);
    m_ObjectPullPhase = ObjectPullClientPhase::Idle;
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
        ObjectPullPublishClientTargetRequest(
            this,
            ObjectPullClientTargetRequestMode::Disabled,
            {},
            {},
            false,
            0);

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

    ObjectPullClientTargetSnapshot targetSnapshot{};
    bool targetSnapshotCurrent = false;
    if (m_ObjectPullPhase == ObjectPullClientPhase::Idle ||
        m_ObjectPullPhase == ObjectPullClientPhase::Targeting)
    {
        ObjectPullPublishClientTargetRequest(
            this,
            ObjectPullClientTargetRequestMode::Scan,
            gunHandPosition,
            gunHandForward,
            actionJustPressed,
            0);
        targetSnapshotCurrent =
            ObjectPullReadPublishedClientTargetSnapshot(
                this,
                targetSnapshot);

        if (targetSnapshotCurrent && targetSnapshot.valid)
        {
            const bool targetChanged =
                m_ObjectPullClientTargetEntityIndex !=
                    targetSnapshot.entityIndex ||
                m_ObjectPullClientTargetHint !=
                    targetSnapshot.targetHint;
            m_ObjectPullPhase = ObjectPullClientPhase::Targeting;
            m_ObjectPullClientTargetEntityIndex =
                targetSnapshot.entityIndex;
            m_ObjectPullClientTargetHint =
                targetSnapshot.targetHint;
            m_ObjectPullClientTargetPoint =
                targetSnapshot.hitPosition;
            m_ObjectPullLastTargetDistanceMeters =
                targetSnapshot.distanceMeters;

            if (targetChanged && m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] pointed target index=%d class=%s model=%s distance=%.2fm assist=%.2fm hand=%s",
                    targetSnapshot.entityIndex,
                    targetSnapshot.className[0]
                        ? targetSnapshot.className
                        : "<none>",
                    targetSnapshot.modelName[0]
                        ? targetSnapshot.modelName
                        : "<none>",
                    targetSnapshot.distanceMeters,
                    m_ObjectPullTargetAssistRadiusMeters,
                    gunHandPhysicalLeft ? "left" : "right");
            }
        }
        else
        {
            m_ObjectPullPhase = ObjectPullClientPhase::Idle;
            m_ObjectPullClientTargetEntityIndex = 0;
            m_ObjectPullClientTargetHint = ObjectPullTargetHint::None;
            m_ObjectPullClientTargetPoint = {};
            m_ObjectPullLastTargetDistanceMeters = 0.0f;

            if (actionJustPressed && m_ObjectPullDebugLog)
            {
                Game::logMsg(
                    "[VR][ObjectPull][client] grip pressed without pointed pull target hit=%d index=%d class=%s model=%s distance=%.2fm min=%.2fm max=%.2fm",
                    targetSnapshotCurrent && targetSnapshot.hitAnything ? 1 : 0,
                    targetSnapshotCurrent
                        ? targetSnapshot.entityIndex
                        : 0,
                    targetSnapshotCurrent && targetSnapshot.className[0]
                        ? targetSnapshot.className
                        : "<none>",
                    targetSnapshotCurrent && targetSnapshot.modelName[0]
                        ? targetSnapshot.modelName
                        : "<none>",
                    targetSnapshotCurrent
                        ? targetSnapshot.distanceMeters
                        : 0.0f,
                    m_ObjectPullMinimumDistanceMeters,
                    m_ObjectPullMaxDistanceMeters);
            }
        }
    }

    if (m_ObjectPullPhase == ObjectPullClientPhase::Targeting &&
        actionJustPressed &&
        !m_ObjectPullRequireActionRelease)
    {
        // The CreateMove thread owns all client-entity inspection. The update
        // thread only arms the entity index from its coherent target snapshot.
        ObjectPullTrackingSample armTracking{};
        if (ObjectPullReadLiveTrackingSample(
                this,
                gunHandPhysicalLeft,
                armTracking))
        {
            m_ObjectPullPhase = ObjectPullClientPhase::Armed;
            ObjectPullPublishClientTargetRequest(
                this,
                ObjectPullClientTargetRequestMode::Hold,
                gunHandPosition,
                gunHandForward,
                false,
                m_ObjectPullClientTargetEntityIndex);
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
                    "[VR][ObjectPull][client] grip armed pointed index=%d distance=%.2fm hand=%s",
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

    if (m_ObjectPullPhase == ObjectPullClientPhase::Armed)
    {
        ObjectPullPublishClientTargetRequest(
            this,
            ObjectPullClientTargetRequestMode::Hold,
            gunHandPosition,
            gunHandForward,
            false,
            m_ObjectPullClientTargetEntityIndex);
        targetSnapshotCurrent =
            ObjectPullReadPublishedClientTargetSnapshot(
                this,
                targetSnapshot);
        const bool targetValid =
            targetSnapshotCurrent &&
            targetSnapshot.valid &&
            targetSnapshot.entityIndex ==
                m_ObjectPullClientTargetEntityIndex;

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
                    ObjectPullPublishClientTargetRequest(
                        this,
                        ObjectPullClientTargetRequestMode::Scan,
                        gunHandPosition,
                        gunHandForward,
                        false,
                        0);
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
                m_ObjectPullWireTargetEntityIndex =
                    m_ObjectPullClientTargetEntityIndex;
                m_ObjectPullWireTargetHint =
                    m_ObjectPullClientTargetHint;
                ObjectPullPublishClientTargetRequest(
                    this,
                    ObjectPullClientTargetRequestMode::Disabled,
                    {},
                    {},
                    false,
                    0);
                m_ObjectPullClientTargetEntityIndex = 0;
                m_ObjectPullClientTargetHint =
                    ObjectPullTargetHint::None;
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
                m_ObjectPullDesiredWireCommand =
                    kObjectPullWireBegin;
                TriggerPhysicalHandHapticPulse(
                    gunHandPhysicalLeft,
                    0.025f,
                    90.0f,
                    0.35f);

                if (m_ObjectPullDebugLog)
                {
                    Game::logMsg(
                        "[VR][ObjectPull][client] gesture triggered distance=%.3fm speed=%.2fm/s mode=%s; selection index and outline cleared",
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
                m_ObjectPullDesiredWireCommand =
                    kObjectPullWireNone;
                ObjectPullPublishClientTargetRequest(
                    this,
                    ObjectPullClientTargetRequestMode::Scan,
                    gunHandPosition,
                    gunHandForward,
                    false,
                    0);
            }
        }
    }
    else if (m_ObjectPullPhase == ObjectPullClientPhase::Pulling)
    {
        ObjectPullPublishClientTargetRequest(
            this,
            ObjectPullClientTargetRequestMode::Disabled,
            {},
            {},
            false,
            0);
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
        ObjectPullPublishClientTargetRequest(
            this,
            ObjectPullClientTargetRequestMode::Disabled,
            {},
            {},
            false,
            0);
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
