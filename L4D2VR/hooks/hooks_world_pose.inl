    // ------------------------------------------------------------
    // Multiplayer VR world-model pose reconstruction
    //
    // Tracking samples remain player-yaw-local on the wire. This renderer
    // resolves them against the current ModelRenderInfo_t, then layers a
    // visual-only head/upper-body/dual-arm solve over Source's final matrices.
    // Each tracked arm uses the same analytic two-bone solve as the native
    // first-person viewmodel arms, but rebuilds the upper-arm-to-hand chain
    // from the model rest pose before every solve. Source locomotion remains
    // on the torso and lower body; fire, melee, deploy and reload shoulder/arm
    // rotations cannot become the next IK seed. Controller roll is shared
    // visually across upper arm and forearm while the hand keeps the exact
    // tracked orientation. Body-relative elbow, near-shoulder direction and
    // bounded twist continuity prevent persistent flips. Collision bones and
    // hitboxes remain completely native.
    // ------------------------------------------------------------
    struct HooksWorldPoseArmLayout
    {
        int clavicle = -1;
        int upperArm = -1;
        int forearm = -1;
        int hand = -1;
        bool palmToHandValid = false;
        vr_vm_stabilize::Mat3x4 palmToHand{};
        bool restChainValid = false;
        std::vector<int> restChainBones;
        std::vector<vr_vm_stabilize::Mat3x4> restChainLocals;
        std::vector<uint8_t> solveMask;
    };

    struct HooksWorldPoseFingerLayout
    {
        std::array<int, 15> bones{
            -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1,
        };
        std::array<vr_vm_stabilize::Mat3x4, 15> restLocals{};
        std::array<uint8_t, 15> restLocalValid{};
        int mappedSegments = 0;
    };

    struct HooksWorldPoseFingerPoseCache
    {
        bool valid = false;
        bool useRestPose = false;
        const void* poseKey = nullptr;
        std::array<vr_vm_stabilize::Mat3x4, 15> locals{};
        std::array<uint8_t, 15> localValid{};

        void Reset()
        {
            valid = false;
            useRestPose = false;
            poseKey = nullptr;
            locals = {};
            localValid = {};
        }
    };

    struct HooksWorldPoseStableTransformSamples
    {
        std::uint32_t count = 0u;
        bool hemisphereValid = false;
        float hemisphereX = 0.0f;
        float hemisphereY = 0.0f;
        float hemisphereZ = 0.0f;
        float hemisphereW = 1.0f;
        float sumX = 0.0f;
        float sumY = 0.0f;
        float sumZ = 0.0f;
        float sumW = 0.0f;
        Vector originSum{};

        void Reset()
        {
            *this = HooksWorldPoseStableTransformSamples{};
        }
    };

    struct HooksWorldPoseBoneLayout
    {
        const uint8_t* studioHdr = nullptr;
        bool valid = false;
        int numBones = 0;
        int head = -1;
        int neck = -1;
        int upperChest = -1;
        HooksWorldPoseArmLayout left{};
        HooksWorldPoseArmLayout right{};
        HooksWorldPoseFingerLayout leftFingers{};
        HooksWorldPoseFingerLayout rightFingers{};
        std::vector<int> parents;
    };

    struct HooksWorldPoseCalibration
    {
        const C_BaseEntity* entity = nullptr;
        const uint8_t* studioHdr = nullptr;
        std::uint32_t poseLockGeneration = 0u;
        bool hmdToHeadValid = false;
        bool leftBendBodyLocalValid = false;
        bool rightBendBodyLocalValid = false;
        bool leftTargetDirectionBodyLocalValid = false;
        bool rightTargetDirectionBodyLocalValid = false;
        bool leftTwistValid = false;
        bool rightTwistValid = false;
        bool hmdToHeadUsesSourceEyeAngles = false;
        const C_BaseCombatWeapon* rightGripWeapon = nullptr;
        bool rightGripPreviousLocalValid = false;
        bool rightGripShotBaselineValid = false;
        bool rightGripShotAxisHemisphereValid = false;
        int rightGripPreviousClip = -2147483647;
        std::uint64_t rightGripLastStereoSceneGeneration = 0u;
        float rightGripShotImpulseAngleRad = 0.0f;
        std::uint64_t rightGripShotCaptureUntilTickMs = 0u;
        std::uint64_t rightGripShotImpulseTickMs = 0u;
        bool hmdReferenceLocalPositionValid = false;
        bool hmdReferenceBodyYawValid = false;
        float hmdReferenceBodyYaw = 0.0f;
        Vector hmdReferenceLocalPosition{};
        vr_vm_stabilize::Mat3x4 hmdToHead{};
        std::uint64_t localThirdPersonWarmupToken = 0u;
        std::uint64_t headCalibrationStartTickMs = 0u;
        std::uint64_t headCalibrationLastSampleTickMs = 0u;
        std::uint64_t headCalibrationLastStereoSceneGeneration = 0u;
        std::uint64_t headCalibrationReadyTickMs = 0u;
        std::uint32_t headCalibrationEyeSampleCount = 0u;
        bool headCalibrationComplete = false;
        HooksWorldPoseStableTransformSamples hmdToHeadSamples{};
        HooksWorldPoseStableTransformSamples neckReferenceSamples{};
        HooksWorldPoseStableTransformSamples headReferenceSamples{};
        Vector leftBendBodyLocal{};
        Vector rightBendBodyLocal{};
        Vector leftTargetDirectionBodyLocal{};
        Vector rightTargetDirectionBodyLocal{};
        float leftTwistRadians = 0.0f;
        float rightTwistRadians = 0.0f;
        std::uint64_t leftBendUpdatedTickMs = 0u;
        std::uint64_t rightBendUpdatedTickMs = 0u;
        std::uint64_t leftTargetDirectionUpdatedTickMs = 0u;
        std::uint64_t rightTargetDirectionUpdatedTickMs = 0u;
        std::uint64_t leftTwistUpdatedTickMs = 0u;
        std::uint64_t rightTwistUpdatedTickMs = 0u;
        vr_vm_stabilize::Mat3x4 rightGripPreviousLocal{};
        vr_vm_stabilize::Mat3x4 rightGripShotBaselineLocal{};
        Vector rightGripShotAxisHemisphere{};
        Vector rightGripShotImpulseAxis{};
        HooksWorldPoseFingerPoseCache leftFingerPose{};
        HooksWorldPoseFingerPoseCache rightFingerPose{};
        bool neckReferenceLocalValid = false;
        bool headReferenceLocalValid = false;
        vr_vm_stabilize::Mat3x4 neckReferenceLocal{};
        vr_vm_stabilize::Mat3x4 headReferenceLocal{};
        bool visualBodyYawValid = false;
        bool visualBodyYawTurning = false;
        float visualBodyYaw = 0.0f;
        std::uint64_t visualBodyYawTickMs = 0u;
        std::uint64_t lastStereoSceneGeneration = 0u;

        void ResetHeadCalibration()
        {
            hmdToHeadValid = false;
            hmdToHeadUsesSourceEyeAngles = false;
            hmdToHead = {};
            hmdReferenceLocalPositionValid = false;
            hmdReferenceBodyYawValid = false;
            hmdReferenceBodyYaw = 0.0f;
            hmdReferenceLocalPosition = {};
            neckReferenceLocalValid = false;
            headReferenceLocalValid = false;
            neckReferenceLocal = {};
            headReferenceLocal = {};
            headCalibrationStartTickMs = 0u;
            headCalibrationLastSampleTickMs = 0u;
            headCalibrationLastStereoSceneGeneration = 0u;
            headCalibrationReadyTickMs = 0u;
            headCalibrationEyeSampleCount = 0u;
            headCalibrationComplete = false;
            hmdToHeadSamples.Reset();
            neckReferenceSamples.Reset();
            headReferenceSamples.Reset();
        }

        void Reset(
            const C_BaseEntity* newEntity,
            const uint8_t* newStudioHdr,
            std::uint32_t newPoseLockGeneration)
        {
            *this = HooksWorldPoseCalibration{};
            entity = newEntity;
            studioHdr = newStudioHdr;
            poseLockGeneration = newPoseLockGeneration;
        }
    };

    // Player-bone rendering can migrate between worker threads. Keep only the
    // genuinely temporal state (body yaw, elbow continuity and shot impulse)
    // per player and serialize that player's world-pose solve. The immutable
    // bind-pose data stores only the controller-palm to model-hand basis.
    std::array<
        HooksWorldPoseCalibration,
        Game::kMaxPlayers> g_HooksWorldPoseCalibrations{};
    std::array<
        std::mutex,
        Game::kMaxPlayers> g_HooksWorldPoseCalibrationMutexes{};

    inline void HooksWorldPoseClearArmContinuity(
        HooksWorldPoseCalibration& calibration,
        int side)
    {
        if (side < 0)
        {
            calibration.leftBendBodyLocalValid = false;
            calibration.leftTargetDirectionBodyLocalValid = false;
            calibration.leftTwistValid = false;
            calibration.leftBendBodyLocal = Vector{};
            calibration.leftTargetDirectionBodyLocal = Vector{};
            calibration.leftTwistRadians = 0.0f;
            calibration.leftBendUpdatedTickMs = 0u;
            calibration.leftTargetDirectionUpdatedTickMs = 0u;
            calibration.leftTwistUpdatedTickMs = 0u;
        }
        else
        {
            calibration.rightBendBodyLocalValid = false;
            calibration.rightTargetDirectionBodyLocalValid = false;
            calibration.rightTwistValid = false;
            calibration.rightBendBodyLocal = Vector{};
            calibration.rightTargetDirectionBodyLocal = Vector{};
            calibration.rightTwistRadians = 0.0f;
            calibration.rightBendUpdatedTickMs = 0u;
            calibration.rightTargetDirectionUpdatedTickMs = 0u;
            calibration.rightTwistUpdatedTickMs = 0u;
        }
    }

    struct HooksWorldPoseWeaponHandState
    {
        bool valid = false;
        int playerIndex = -1;
        int weaponEntityIndex = -1;
        const C_BaseEntity* playerEntity = nullptr;
        void* playerRenderable = nullptr;
        const C_BaseCombatWeapon* weaponEntity = nullptr;
        void* weaponRenderable = nullptr;
        const model_t* weaponWorldModel = nullptr;
        std::uint64_t updatedTickMs = 0u;
        bool controllerToFinalHandValid = false;
        Vector playerOrigin{};
        QAngle playerAngles{};
        vr_vm_stabilize::Mat3x4 nativeHand{};
        vr_vm_stabilize::Mat3x4 finalHand{};
        vr_vm_stabilize::Mat3x4 nativeToFinal{};
        vr_vm_stabilize::Mat3x4 controllerToFinalHand{};
    };

    std::mutex g_HooksWorldPoseWeaponHandMutex;
    std::array<
        HooksWorldPoseWeaponHandState,
        Game::kMaxPlayers> g_HooksWorldPoseWeaponHandStates{};
    std::atomic<void*>
        g_HooksWorldPosePendingWeaponRenderable{ nullptr };
    std::atomic<void*>
        g_HooksWorldPoseWeaponSetupBonesTarget{ nullptr };
    std::atomic<bool>
        g_HooksWorldPoseWeaponSetupBonesHookReady{ false };
    std::mutex g_HooksWorldPoseWeaponSetupBonesHookMutex;

    // SetupBones may be evaluated separately for the two stereo views. Keep
    // the first corrected result for this render snapshot and replay that
    // exact immutable matrix set to later eye calls. This avoids both eyes
    // observing hand states published at different points in the scene draw,
    // without routing arbitrary models through an additional draw-time path.
    struct HooksWorldPoseWeaponStereoBoneCache
    {
        bool valid = false;
        void* renderable = nullptr;
        std::uint64_t stereoSceneGeneration = 0u;
        int maxBones = 0;
        int boneMask = 0;
        std::uint64_t lastUsedTickMs = 0u;
        std::array<vr_vm_stabilize::Mat3x4, 512> bones{};
    };

    std::mutex g_HooksWorldPoseWeaponStereoBoneCacheMutex;
    std::atomic<std::uint64_t>
        g_HooksWorldPoseStereoSceneGeneration{ 1u };
    std::atomic<bool>
        g_HooksWorldPoseFingerRuntimeDisabled{ false };
    std::atomic<bool>
        g_HooksWorldPoseFingerFaultLogged{ false };
    std::atomic<bool>
        g_HooksWorldPoseFingerFirstRunLogged{ false };
    std::atomic<bool>
        g_HooksWorldPoseFingerFirstRunCompletedLogged{ false };
    std::array<
        HooksWorldPoseWeaponStereoBoneCache,
        Game::kMaxPlayers * 4> g_HooksWorldPoseWeaponStereoBoneCaches{};

    // The survivor is submitted independently for each eye too. Re-running
    // native locomotion sampling and the VR IK solve between those submissions
    // lets the two eyes see slightly different body/arm matrices, perceived as
    // whole-character shimmer while moving. Cache the first eye's immutable
    // final player bones for the rest of this actual stereo scene.
    struct HooksWorldPosePlayerStereoBoneCache
    {
        bool valid = false;
        const C_BaseEntity* entity = nullptr;
        const uint8_t* studioHdr = nullptr;
        std::uint64_t stereoSceneGeneration = 0u;
        int numBones = 0;
        std::uint64_t lastUsedTickMs = 0u;
        std::array<vr_vm_stabilize::Mat3x4, 512> bones{};
    };

    std::mutex g_HooksWorldPosePlayerStereoBoneCacheMutex;
    std::array<
        HooksWorldPosePlayerStereoBoneCache,
        Game::kMaxPlayers> g_HooksWorldPosePlayerStereoBoneCaches{};

    inline void HooksWorldPoseBeginStereoFrame()
    {
        g_HooksWorldPoseStereoSceneGeneration.fetch_add(
            1u,
            std::memory_order_acq_rel);
        {
			std::lock_guard<std::mutex> lock(
				g_HooksWorldPoseWeaponStereoBoneCacheMutex);
			for (HooksWorldPoseWeaponStereoBoneCache& cache :
				 g_HooksWorldPoseWeaponStereoBoneCaches)
			{
				cache.valid = false;
			}
		}
		{
			std::lock_guard<std::mutex> lock(
				g_HooksWorldPosePlayerStereoBoneCacheMutex);
			for (HooksWorldPosePlayerStereoBoneCache& cache :
				 g_HooksWorldPosePlayerStereoBoneCaches)
			{
				cache.valid = false;
			}
        }
    }

    inline bool HooksWorldPoseLoadStereoPlayerBones(
        const C_BaseEntity* entity,
        const uint8_t* studioHdr,
        std::uint64_t stereoSceneGeneration,
        int numBones,
        vr_vm_stabilize::Mat3x4* outBones,
        std::uint64_t now)
    {
        if (!entity || !studioHdr || stereoSceneGeneration == 0u ||
            numBones <= 0 ||
            numBones > 512 || !outBones)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPosePlayerStereoBoneCacheMutex);
        for (HooksWorldPosePlayerStereoBoneCache& cache :
             g_HooksWorldPosePlayerStereoBoneCaches)
        {
            if (!cache.valid ||
                cache.entity != entity ||
                cache.studioHdr != studioHdr ||
                cache.stereoSceneGeneration != stereoSceneGeneration ||
                cache.numBones != numBones)
            {
                continue;
            }
            for (int bone = 0; bone < numBones; ++bone)
                outBones[bone] = cache.bones[static_cast<size_t>(bone)];
            cache.lastUsedTickMs = now;
            return true;
        }
        return false;
    }

    inline void HooksWorldPoseStoreStereoPlayerBones(
        const C_BaseEntity* entity,
        const uint8_t* studioHdr,
        std::uint64_t stereoSceneGeneration,
        int numBones,
        const vr_vm_stabilize::Mat3x4* bones,
        std::uint64_t now)
    {
        if (!entity || !studioHdr || stereoSceneGeneration == 0u ||
            numBones <= 0 ||
            numBones > 512 || !bones)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPosePlayerStereoBoneCacheMutex);
        HooksWorldPosePlayerStereoBoneCache* destination = nullptr;
        for (HooksWorldPosePlayerStereoBoneCache& cache :
             g_HooksWorldPosePlayerStereoBoneCaches)
        {
            if (cache.valid && cache.entity == entity)
            {
                if (cache.stereoSceneGeneration > stereoSceneGeneration)
                    return;
                destination = &cache;
                break;
            }
            if (!destination || !cache.valid ||
                cache.lastUsedTickMs < destination->lastUsedTickMs)
            {
                destination = &cache;
            }
        }
        if (!destination)
            return;

        destination->valid = true;
        destination->entity = entity;
        destination->studioHdr = studioHdr;
        destination->stereoSceneGeneration = stereoSceneGeneration;
        destination->numBones = numBones;
        destination->lastUsedTickMs = now;
        for (int bone = 0; bone < numBones; ++bone)
            destination->bones[static_cast<size_t>(bone)] = bones[bone];
    }

    struct HooksWorldPoseWeaponAnchorCalibration
    {
        bool valid = false;
        void* renderable = nullptr;
        const C_BaseCombatWeapon* weaponEntity = nullptr;
        const model_t* weaponWorldModel = nullptr;
        int maxBones = 0;
        int anchorBone = -1;
        std::uint64_t lastUsedTickMs = 0u;
        vr_vm_stabilize::Mat3x4 handToWeaponAnchor{};
    };

    std::mutex g_HooksWorldPoseWeaponAnchorCalibrationMutex;
    std::array<
        HooksWorldPoseWeaponAnchorCalibration,
        8> g_HooksWorldPoseWeaponAnchorCalibrations{};

    inline bool HooksWorldPoseLoadStereoWeaponBones(
        void* renderable,
        std::uint64_t stereoSceneGeneration,
        int maxBones,
        int boneMask,
        vr_vm_stabilize::Mat3x4* outBones,
        std::uint64_t now)
    {
        if (!renderable ||
            stereoSceneGeneration == 0u ||
            maxBones <= 0 ||
            maxBones > 512 ||
            !outBones)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponStereoBoneCacheMutex);
        for (HooksWorldPoseWeaponStereoBoneCache& cache :
             g_HooksWorldPoseWeaponStereoBoneCaches)
        {
            if (!cache.valid ||
                cache.renderable != renderable ||
                cache.stereoSceneGeneration != stereoSceneGeneration ||
                cache.maxBones != maxBones ||
                cache.boneMask != boneMask)
            {
                continue;
            }

            for (int bone = 0; bone < maxBones; ++bone)
                outBones[bone] = cache.bones[static_cast<size_t>(bone)];
            cache.lastUsedTickMs = now;
            return true;
        }
        return false;
    }

    inline void HooksWorldPoseStoreStereoWeaponBones(
        void* renderable,
        std::uint64_t stereoSceneGeneration,
        int maxBones,
        int boneMask,
        const vr_vm_stabilize::Mat3x4* bones,
        std::uint64_t now)
    {
        if (!renderable ||
            stereoSceneGeneration == 0u ||
            maxBones <= 0 ||
            maxBones > 512 ||
            !bones)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponStereoBoneCacheMutex);
        HooksWorldPoseWeaponStereoBoneCache* destination = nullptr;
        for (HooksWorldPoseWeaponStereoBoneCache& cache :
             g_HooksWorldPoseWeaponStereoBoneCaches)
        {
            if (cache.valid &&
                cache.renderable == renderable &&
                cache.stereoSceneGeneration == stereoSceneGeneration &&
                cache.maxBones == maxBones &&
                cache.boneMask == boneMask)
            {
                destination = &cache;
                break;
            }
            if (!destination ||
                !cache.valid ||
                cache.lastUsedTickMs < destination->lastUsedTickMs)
            {
                destination = &cache;
            }
        }
        if (!destination)
            return;

        destination->valid = true;
        destination->renderable = renderable;
        destination->stereoSceneGeneration = stereoSceneGeneration;
        destination->maxBones = maxBones;
        destination->boneMask = boneMask;
        destination->lastUsedTickMs = now;
        for (int bone = 0; bone < maxBones; ++bone)
        {
            destination->bones[static_cast<size_t>(bone)] =
                bones[bone];
        }
    }

    inline bool HooksWorldPoseWeaponCorrectedInStereoScene(
        void* renderable,
        std::uint64_t stereoSceneGeneration)
    {
        if (!renderable || stereoSceneGeneration == 0u)
            return false;

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponStereoBoneCacheMutex);
        for (const HooksWorldPoseWeaponStereoBoneCache& cache :
             g_HooksWorldPoseWeaponStereoBoneCaches)
        {
            if (cache.valid &&
                cache.renderable == renderable &&
                cache.stereoSceneGeneration == stereoSceneGeneration)
            {
                return true;
            }
        }
        return false;
    }

    inline float HooksWorldPoseWrapAngle(float value)
    {
        if (!std::isfinite(value))
            return 0.0f;
        value -= 360.0f * std::floor((value + 180.0f) / 360.0f);
        return value;
    }

    inline QAngle HooksWorldPoseLerpAngles(
        const QAngle& from,
        const QAngle& to,
        float fraction)
    {
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        return QAngle(
            HooksWorldPoseWrapAngle(
                from.x + HooksWorldPoseWrapAngle(to.x - from.x) * fraction),
            HooksWorldPoseWrapAngle(
                from.y + HooksWorldPoseWrapAngle(to.y - from.y) * fraction),
            HooksWorldPoseWrapAngle(
                from.z + HooksWorldPoseWrapAngle(to.z - from.z) * fraction));
    }

    inline bool HooksWorldPoseArmChainValid(
        const HooksWorldPoseArmLayout& arm,
        const std::vector<int>& parents,
        int numBones)
    {
        if (arm.upperArm < 0 || arm.upperArm >= numBones ||
            arm.forearm < 0 || arm.forearm >= numBones ||
            arm.hand < 0 || arm.hand >= numBones)
        {
            return false;
        }

        return HooksNativeViewmodelHandsOnlyIsAncestor(
                parents,
                arm.forearm,
                arm.upperArm,
                numBones) &&
            HooksNativeViewmodelHandsOnlyIsAncestor(
                parents,
                arm.hand,
                arm.forearm,
                numBones);
    }

    inline void HooksWorldPoseResolveArm(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& parents,
        int numBones,
        int side,
        HooksWorldPoseArmLayout& arm)
    {
        const bool left = side < 0;
        const std::vector<const char*> handSuffixes = left
            ? std::vector<const char*>{
                "bip01_l_hand", "bip01_l_wrist", "l_hand", "hand_l",
            }
            : std::vector<const char*>{
                "bip01_r_hand", "bip01_r_wrist", "r_hand", "hand_r",
            };
        const std::vector<const char*> forearmSuffixes = left
            ? std::vector<const char*>{
                "bip01_l_forearm", "bip01_l_lowerarm",
                "l_forearm", "forearm_l",
            }
            : std::vector<const char*>{
                "bip01_r_forearm", "bip01_r_lowerarm",
                "r_forearm", "forearm_r",
            };
        const std::vector<const char*> upperArmSuffixes = left
            ? std::vector<const char*>{
                "bip01_l_upperarm", "bip01_l_upper_arm",
                "l_upperarm", "upperarm_l",
            }
            : std::vector<const char*>{
                "bip01_r_upperarm", "bip01_r_upper_arm",
                "r_upperarm", "upperarm_r",
            };
        const std::vector<const char*> clavicleSuffixes = left
            ? std::vector<const char*>{
                "bip01_l_clavicle", "bip01_l_collar",
                "l_clavicle", "clavicle_l",
            }
            : std::vector<const char*>{
                "bip01_r_clavicle", "bip01_r_collar",
                "r_clavicle", "clavicle_r",
            };

        HooksFirstPersonBodyFindBone(boneNames, handSuffixes, arm.hand);
        HooksFirstPersonBodyFindBone(
            boneNames,
            forearmSuffixes,
            arm.forearm);
        HooksFirstPersonBodyFindBone(
            boneNames,
            upperArmSuffixes,
            arm.upperArm);
        HooksFirstPersonBodyFindBone(
            boneNames,
            clavicleSuffixes,
            arm.clavicle);

        if (arm.hand < 0)
        {
            const std::vector<const char*> needles = {
                "hand", "wrist",
            };
            HooksNativeViewmodelHandsOnlyFindNamedBone(
                boneNames,
                needles,
                side,
                arm.hand);
        }

        // Reuse the first-person chain discovery rules, including ulna/radius,
        // elbow and unnamed-lower-arm fallbacks used by replacement rigs.
        if (arm.hand >= 0)
        {
            HooksNativeViewmodelHandsOnlySideInfo sideInfo{};
            sideInfo.side = side;
            sideInfo.hand = arm.hand;
            sideInfo.forearm = arm.forearm;
            HooksNativeViewmodelArmIkChain sharedChain{};
            if (HooksNativeViewmodelArmIkFindChain(
                    boneNames,
                    parents,
                    numBones,
                    sideInfo,
                    sharedChain))
            {
                arm.upperArm = sharedChain.upperArm;
                arm.forearm = sharedChain.forearm;
                arm.hand = sharedChain.hand;
            }
        }

        if (arm.forearm < 0 && arm.hand >= 0)
        {
            const std::vector<const char*> needles = {
                "forearm", "lowerarm", "lower_arm", "ulna", "radius",
            };
            HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
                boneNames,
                parents,
                numBones,
                arm.hand,
                needles,
                side,
                arm.forearm);
        }

        if (arm.upperArm < 0 && arm.forearm >= 0)
        {
            const std::vector<const char*> needles = {
                "upperarm", "upper_arm", "upper arm",
            };
            HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
                boneNames,
                parents,
                numBones,
                arm.forearm,
                needles,
                side,
                arm.upperArm);
        }

        if (arm.clavicle < 0 && arm.upperArm >= 0)
        {
            const std::vector<const char*> needles = {
                "clavicle", "collarbone", "collar",
            };
            HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
                boneNames,
                parents,
                numBones,
                arm.upperArm,
                needles,
                side,
                arm.clavicle);
        }

        if (!HooksWorldPoseArmChainValid(arm, parents, numBones))
        {
            arm.upperArm = -1;
            arm.forearm = -1;
            arm.hand = -1;
        }

        if (arm.clavicle >= 0 &&
            arm.upperArm >= 0 &&
            !HooksNativeViewmodelHandsOnlyIsAncestor(
                parents,
                arm.upperArm,
                arm.clavicle,
                numBones))
        {
            arm.clavicle = -1;
        }
    }

    inline bool HooksWorldPoseReadBindBoneModel(
        const uint8_t* studioHdr,
        int studioLength,
        int boneTableOffset,
        int boneStride,
        int numBones,
        int bone,
        vr_vm_stabilize::Mat3x4& outBindModel)
    {
        outBindModel = vr_vm_stabilize::Identity();
        constexpr std::uint64_t kPoseToBoneOffset = 0x60u;
        constexpr std::uint64_t kPoseToBoneBytes =
            sizeof(vr_vm_stabilize::Mat3x4);
        if (!studioHdr ||
            studioLength <= 0 ||
            boneTableOffset <= 0 ||
            boneStride < static_cast<int>(
                kPoseToBoneOffset + kPoseToBoneBytes) ||
            bone < 0 ||
            bone >= numBones)
        {
            return false;
        }

        const std::uint64_t boneOffset =
            static_cast<std::uint64_t>(boneTableOffset) +
            static_cast<std::uint64_t>(boneStride) *
                static_cast<std::uint64_t>(bone);
        const std::uint64_t poseEnd =
            boneOffset + kPoseToBoneOffset + kPoseToBoneBytes;
        if (poseEnd > static_cast<std::uint64_t>(studioLength))
            return false;

        vr_vm_stabilize::Mat3x4 poseToBone{};
        vr_vm_stabilize::Mat3x4 bindModel{};
        if (!vr_vm_stabilize::SafeRead(
                studioHdr +
                    static_cast<size_t>(boneOffset +
                                        kPoseToBoneOffset),
                poseToBone) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(poseToBone) ||
            !vr_vm_stabilize::InvertAffine(
                poseToBone,
                bindModel) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                bindModel,
                outBindModel))
        {
            return false;
        }
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outBindModel);
    }

    inline std::array<int, 4> HooksWorldPoseFindFingerRoots(
        const std::vector<std::string>& boneNames,
        int side)
    {
        std::array<int, 4> result{ { -1, -1, -1, -1 } };
        const char sideLetter = side < 0 ? 'l' : 'r';
        for (size_t bone = 0; bone < boneNames.size(); ++bone)
        {
            const std::string lower =
                vr_vm_stabilize::ToLowerAscii(boneNames[bone]);
            for (int finger = 1; finger <= 4; ++finger)
            {
                const std::string suffix =
                    std::string("bip01_") +
                    sideLetter +
                    "_finger" +
                    std::to_string(finger);
                if (HooksViewmodelAutoGripNameEndsWith(
                        lower,
                        suffix.c_str()))
                {
                    result[static_cast<size_t>(finger - 1)] =
                        static_cast<int>(bone);
                }
            }
        }
        return result;
    }

    inline bool HooksWorldPoseBuildBindPalmReference(
        const uint8_t* studioHdr,
        int studioLength,
        int boneTableOffset,
        int boneStride,
        int numBones,
        const std::vector<std::string>& boneNames,
        int side,
        HooksWorldPoseArmLayout& arm)
    {
        arm.palmToHandValid = false;
        if (arm.hand < 0 || arm.hand >= numBones)
            return false;

        // Only the controller-palm to model-hand orientation comes from bind
        // data. The analytic solve rebuilds the complete arm chain separately
        // from stable rest locals, so this reference never captures an action
        // animation frame.
        vr_vm_stabilize::Mat3x4 bindHand{};
        if (!HooksWorldPoseReadBindBoneModel(
                studioHdr,
                studioLength,
                boneTableOffset,
                boneStride,
                numBones,
                arm.hand,
                bindHand))
        {
            return false;
        }
        const Vector handOrigin =
            vr_vm_stabilize::GetOrigin(bindHand);

        const std::array<int, 4> fingerRoots =
            HooksWorldPoseFindFingerRoots(boneNames, side);
        if (!std::all_of(
                fingerRoots.begin(),
                fingerRoots.end(),
                [numBones](int bone)
                {
                    return bone >= 0 && bone < numBones;
                }))
        {
            return true;
        }

        Vector averageFingerOrigin{};
        std::array<Vector, 4> fingerOrigins{};
        for (size_t finger = 0; finger < fingerRoots.size(); ++finger)
        {
            vr_vm_stabilize::Mat3x4 bindFinger{};
            if (!HooksWorldPoseReadBindBoneModel(
                    studioHdr,
                    studioLength,
                    boneTableOffset,
                    boneStride,
                    numBones,
                    fingerRoots[finger],
                    bindFinger))
            {
                return true;
            }
            fingerOrigins[finger] =
                vr_vm_stabilize::GetOrigin(bindFinger);
            averageFingerOrigin += fingerOrigins[finger];
        }
        averageFingerOrigin *= 0.25f;
        const Vector palmForward =
            averageFingerOrigin - handOrigin;
        const Vector palmSide =
            fingerOrigins[0] - fingerOrigins[3];
        const Vector palmNormal =
            CrossProduct(palmForward, palmSide);
        vr_vm_stabilize::Mat3x4 bindPalm{};
        vr_vm_stabilize::Mat3x4 inverseBindPalm{};
        if (!HooksViewmodelAutoGripBuildRigidMatrix(
                handOrigin,
                palmForward,
                palmSide,
                palmNormal,
                bindPalm))
        {
            return true;
        }
        vr_vm_stabilize::InvertTR(
            bindPalm,
            inverseBindPalm);
        vr_vm_stabilize::Mul(
            inverseBindPalm,
            bindHand,
            arm.palmToHand);
        arm.palmToHand.m[0][3] = 0.0f;
        arm.palmToHand.m[1][3] = 0.0f;
        arm.palmToHand.m[2][3] = 0.0f;
        vr_vm_stabilize::Mat3x4 normalizedPalmToHand{};
        if (HooksViewmodelAutoGripNormalizeRigidMatrix(
                arm.palmToHand,
                normalizedPalmToHand))
        {
            arm.palmToHand = normalizedPalmToHand;
            arm.palmToHandValid = true;
        }
        return true;
    }

    inline void HooksWorldPoseBuildFingerLayout(
        void* drawState,
        int boneTableOffset,
        int boneStride,
        const std::vector<std::string>& boneNames,
        int numBones,
        int side,
        HooksWorldPoseFingerLayout& out)
    {
        out = HooksWorldPoseFingerLayout{};
        if (!drawState || boneTableOffset <= 0 || boneStride <= 0 ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            (side != -1 && side != 1))
        {
            return;
        }

        static const char* kLeftFingerBones[5][3] =
        {
            { "bip01_l_finger0", "bip01_l_finger01", "bip01_l_finger02" },
            { "bip01_l_finger1", "bip01_l_finger11", "bip01_l_finger12" },
            { "bip01_l_finger2", "bip01_l_finger21", "bip01_l_finger22" },
            { "bip01_l_finger3", "bip01_l_finger31", "bip01_l_finger32" },
            { "bip01_l_finger4", "bip01_l_finger41", "bip01_l_finger42" },
        };
        static const char* kRightFingerBones[5][3] =
        {
            { "bip01_r_finger0", "bip01_r_finger01", "bip01_r_finger02" },
            { "bip01_r_finger1", "bip01_r_finger11", "bip01_r_finger12" },
            { "bip01_r_finger2", "bip01_r_finger21", "bip01_r_finger22" },
            { "bip01_r_finger3", "bip01_r_finger31", "bip01_r_finger32" },
            { "bip01_r_finger4", "bip01_r_finger41", "bip01_r_finger42" },
        };

        for (int finger = 0; finger < 5; ++finger)
        {
            for (int segment = 0; segment < 3; ++segment)
            {
                const int slot = finger * 3 + segment;
                int bone = -1;
                if (!HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
                        boneNames,
                        side < 0
                            ? kLeftFingerBones[finger][segment]
                            : kRightFingerBones[finger][segment],
                        bone) ||
                    bone < 0 || bone >= numBones)
                {
                    continue;
                }

                out.bones[static_cast<size_t>(slot)] = bone;
                ++out.mappedSegments;
                vr_vm_stabilize::Mat3x4 restLocal{};
                if (HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
                        drawState,
                        boneTableOffset,
                        boneStride,
                        bone,
                        restLocal) &&
                    HooksNativeViewmodelHandsOnlyMatrixFinite(restLocal))
                {
                    out.restLocals[static_cast<size_t>(slot)] = restLocal;
                    out.restLocalValid[static_cast<size_t>(slot)] = 1u;
                }
            }
        }
    }

    inline bool HooksWorldPoseBuildArmSolveMask(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& parents,
        int numBones,
        int side,
        HooksWorldPoseArmLayout& arm)
    {
        arm.solveMask.assign(static_cast<size_t>(numBones), 0u);
        if ((side != -1 && side != 1) ||
            static_cast<int>(boneNames.size()) < numBones ||
            !HooksWorldPoseArmChainValid(arm, parents, numBones))
        {
            return false;
        }

        // Third-person action animations frequently rotate the clavicle as part
        // of fire/reload/melee gestures. Resetting only from upperArm leaves that
        // animated shoulder root in front of an otherwise stable analytic chain,
        // so tiny per-frame clavicle changes become large upper-arm corrections.
        // Include the same-side clavicle in the solved branch whenever it is a
        // real ancestor of the upper arm. The torso above it remains fully native.
        int solveRoot = arm.upperArm;
        if (arm.clavicle >= 0 && arm.clavicle < numBones &&
            HooksNativeViewmodelHandsOnlyIsAncestor(
                parents,
                arm.upperArm,
                arm.clavicle,
                numBones))
        {
            solveRoot = arm.clavicle;
        }

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyIsAncestor(
                    parents,
                    bone,
                    solveRoot,
                    numBones))
            {
                continue;
            }
            bool blockedByOppositeBranch = false;
            int current = bone;
            for (int guard = 0;
                 guard < numBones && current >= 0 && current < numBones;
                 ++guard)
            {
                const int namedSide =
                    HooksNativeViewmodelHandsOnlyBoneSide(
                        vr_vm_stabilize::ToLowerAscii(
                            boneNames[static_cast<size_t>(current)]));
                if (namedSide != 0 && namedSide != side)
                {
                    blockedByOppositeBranch = true;
                    break;
                }
                if (current == solveRoot)
                    break;
                current = parents[static_cast<size_t>(current)];
            }
            if (blockedByOppositeBranch)
                continue;
            arm.solveMask[static_cast<size_t>(bone)] = 1u;
        }

        return arm.solveMask[static_cast<size_t>(solveRoot)] != 0u &&
            arm.solveMask[static_cast<size_t>(arm.upperArm)] != 0u &&
            arm.solveMask[static_cast<size_t>(arm.forearm)] != 0u &&
            arm.solveMask[static_cast<size_t>(arm.hand)] != 0u;
    }

    inline bool HooksWorldPoseBuildArmRestChain(
        void* drawState,
        int boneIndex,
        int stride,
        const std::vector<int>& parents,
        int numBones,
        HooksWorldPoseArmLayout& arm)
    {
        arm.restChainValid = false;
        arm.restChainBones.clear();
        arm.restChainLocals.clear();
        if (!drawState || boneIndex <= 0 || stride <= 0 ||
            static_cast<int>(parents.size()) < numBones ||
            !HooksWorldPoseArmChainValid(arm, parents, numBones))
        {
            return false;
        }

        int restRoot = arm.upperArm;
        if (arm.clavicle >= 0 && arm.clavicle < numBones &&
            HooksNativeViewmodelHandsOnlyIsAncestor(
                parents,
                arm.upperArm,
                arm.clavicle,
                numBones))
        {
            restRoot = arm.clavicle;
        }

        std::vector<int> reverseChain;
        reverseChain.reserve(14u);
        bool sawForearm = false;
        bool sawUpperArm = false;
        bool sawRoot = false;
        int current = arm.hand;
        for (int guard = 0;
             guard < numBones && current >= 0 && current < numBones;
             ++guard)
        {
            reverseChain.push_back(current);
            if (current == arm.forearm)
                sawForearm = true;
            if (current == arm.upperArm)
                sawUpperArm = true;
            if (current == restRoot)
            {
                sawRoot = true;
                break;
            }
            const int parent = parents[static_cast<size_t>(current)];
            if (parent == current)
                break;
            current = parent;
        }
        if (!sawRoot || !sawUpperArm || !sawForearm || reverseChain.empty())
            return false;

        std::reverse(reverseChain.begin(), reverseChain.end());
        arm.restChainBones = reverseChain;
        arm.restChainLocals.reserve(reverseChain.size());
        for (int bone : reverseChain)
        {
            vr_vm_stabilize::Mat3x4 restLocal{};
            if (!HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
                    drawState,
                    boneIndex,
                    stride,
                    bone,
                    restLocal) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(restLocal))
            {
                arm.restChainBones.clear();
                arm.restChainLocals.clear();
                return false;
            }
            arm.restChainLocals.push_back(restLocal);
        }

        arm.restChainValid =
            arm.restChainBones.size() == arm.restChainLocals.size() &&
            !arm.restChainBones.empty() &&
            arm.restChainBones.front() == restRoot &&
            arm.restChainBones.back() == arm.hand;
        return arm.restChainValid;
    }

    inline bool HooksWorldPoseBuildBoneLayout(
        void* drawState,
        HooksWorldPoseBoneLayout& layout)
    {
        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(
                drawState,
                studioHdr) ||
            !studioHdr)
        {
            return false;
        }

        layout = HooksWorldPoseBoneLayout{};
        layout.studioHdr = studioHdr;

        std::vector<std::string> boneNames;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
                drawState,
                boneNames,
                layout.parents,
                layout.numBones,
                boneIndex,
                stride,
                numBonesOffset) ||
            layout.numBones <= 0 ||
            layout.numBones > 512 ||
            static_cast<int>(boneNames.size()) < layout.numBones ||
            static_cast<int>(layout.parents.size()) < layout.numBones)
        {
            return false;
        }
        HooksFirstPersonBodyFindBone(
            boneNames,
            {
                "bip01_head1", "bip01_head", "head1", "head",
            },
            layout.head);
        HooksFirstPersonBodyFindBone(
            boneNames,
            {
                "bip01_neck1", "bip01_neck", "neck1", "neck",
            },
            layout.neck);
        HooksFirstPersonBodyFindBone(
            boneNames,
            {
                "bip01_spine4", "spine4",
                "bip01_spine3", "spine3",
                "bip01_spine2", "spine2",
            },
            layout.upperChest);

        if (layout.head >= 0 &&
            (layout.neck < 0 ||
             !HooksNativeViewmodelHandsOnlyIsAncestor(
                layout.parents,
                layout.head,
                layout.neck,
                layout.numBones)))
        {
            const int parent =
                layout.parents[static_cast<size_t>(layout.head)];
            layout.neck =
                (parent >= 0 && parent < layout.numBones)
                    ? parent
                    : layout.head;
        }

        if (layout.neck >= 0 &&
            (layout.upperChest < 0 ||
             !HooksNativeViewmodelHandsOnlyIsAncestor(
                layout.parents,
                layout.neck,
                layout.upperChest,
                layout.numBones)))
        {
            int current =
                layout.parents[static_cast<size_t>(layout.neck)];
            layout.upperChest = -1;
            for (int guard = 0;
                 guard < layout.numBones &&
                 current >= 0 &&
                 current < layout.numBones;
                 ++guard)
            {
                const std::string lowerName =
                    vr_vm_stabilize::ToLowerAscii(
                        boneNames[static_cast<size_t>(current)]);
                if (lowerName.find("spine") != std::string::npos ||
                    lowerName.find("chest") != std::string::npos)
                {
                    layout.upperChest = current;
                    break;
                }
                current =
                    layout.parents[static_cast<size_t>(current)];
            }
        }

        HooksWorldPoseResolveArm(
            boneNames,
            layout.parents,
            layout.numBones,
            -1,
            layout.left);
        HooksWorldPoseResolveArm(
            boneNames,
            layout.parents,
            layout.numBones,
            1,
            layout.right);

        // Match the first-person solver's branch isolation rule. A malformed
        // replacement rig must never let one controller rotate both arms.
        if (HooksWorldPoseArmChainValid(
                layout.left,
                layout.parents,
                layout.numBones) &&
            HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones))
        {
            const bool leftOwnsRight =
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    layout.right.upperArm,
                    layout.left.upperArm,
                    layout.numBones);
            const bool rightOwnsLeft =
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    layout.left.upperArm,
                    layout.right.upperArm,
                    layout.numBones);
            if (leftOwnsRight)
                layout.left = HooksWorldPoseArmLayout{};
            if (rightOwnsLeft)
                layout.right = HooksWorldPoseArmLayout{};
            if (leftOwnsRight || rightOwnsLeft)
            {
                Game::logMsg(
                    "[VR][WorldPose] rejected overlapping left/right arm branches hdr=%p",
                    layout.studioHdr);
            }
        }
        if (HooksWorldPoseArmChainValid(
                layout.left,
                layout.parents,
                layout.numBones) &&
            !HooksWorldPoseBuildArmSolveMask(
                boneNames,
                layout.parents,
                layout.numBones,
                -1,
                layout.left))
        {
            layout.left = HooksWorldPoseArmLayout{};
        }
        if (HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones) &&
            !HooksWorldPoseBuildArmSolveMask(
                boneNames,
                layout.parents,
                layout.numBones,
                1,
                layout.right))
        {
            layout.right = HooksWorldPoseArmLayout{};
        }
        bool solveMasksOverlap = false;
        if (layout.left.solveMask.size() ==
                static_cast<size_t>(layout.numBones) &&
            layout.right.solveMask.size() ==
                static_cast<size_t>(layout.numBones))
        {
            for (int bone = 0; bone < layout.numBones; ++bone)
            {
                if (layout.left.solveMask[static_cast<size_t>(bone)] != 0u &&
                    layout.right.solveMask[static_cast<size_t>(bone)] != 0u)
                {
                    solveMasksOverlap = true;
                    break;
                }
            }
        }
        if (solveMasksOverlap)
        {
            layout.left = HooksWorldPoseArmLayout{};
            layout.right = HooksWorldPoseArmLayout{};
            Game::logMsg(
                "[VR][WorldPose] rejected intersecting analytic arm masks hdr=%p",
                layout.studioHdr);
        }
        if (HooksWorldPoseArmChainValid(
                layout.left,
                layout.parents,
                layout.numBones) &&
            !HooksWorldPoseBuildArmRestChain(
                drawState,
                boneIndex,
                stride,
                layout.parents,
                layout.numBones,
                layout.left))
        {
            Game::logMsg(
                "[VR][WorldPose] rejected left arm without stable rest chain hdr=%p",
                layout.studioHdr);
            layout.left = HooksWorldPoseArmLayout{};
        }
        if (HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones) &&
            !HooksWorldPoseBuildArmRestChain(
                drawState,
                boneIndex,
                stride,
                layout.parents,
                layout.numBones,
                layout.right))
        {
            Game::logMsg(
                "[VR][WorldPose] rejected right arm without stable rest chain hdr=%p",
                layout.studioHdr);
            layout.right = HooksWorldPoseArmLayout{};
        }
        HooksWorldPoseBuildFingerLayout(
            drawState,
            boneIndex,
            stride,
            boneNames,
            layout.numBones,
            -1,
            layout.leftFingers);
        HooksWorldPoseBuildFingerLayout(
            drawState,
            boneIndex,
            stride,
            boneNames,
            layout.numBones,
            1,
            layout.rightFingers);

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(
            studioHdr + 0x4C,
            studioLength);
        if (HooksWorldPoseArmChainValid(
                layout.left,
                layout.parents,
                layout.numBones))
        {
            HooksWorldPoseBuildBindPalmReference(
                studioHdr,
                studioLength,
                boneIndex,
                stride,
                layout.numBones,
                boneNames,
                -1,
                layout.left);
        }
        if (HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones))
        {
            HooksWorldPoseBuildBindPalmReference(
                studioHdr,
                studioLength,
                boneIndex,
                stride,
                layout.numBones,
                boneNames,
                1,
                layout.right);
        }

        const bool headValid =
            layout.head >= 0 &&
            layout.head < layout.numBones;
        const bool leftValid =
            HooksWorldPoseArmChainValid(
                layout.left,
                layout.parents,
                layout.numBones);
        const bool rightValid =
            HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones);
        layout.valid = headValid || leftValid || rightValid;

        if (layout.valid)
        {
            Game::logMsg(
                "[VR][WorldPose] bone layout ready hdr=%p bones=%d head=%d neck=%d chest=%d solver=analytic left=(%d %d %d %d rest=%d bindPalm=%d fingers=%d) right=(%d %d %d %d rest=%d bindPalm=%d fingers=%d)",
                layout.studioHdr,
                layout.numBones,
                layout.head,
                layout.neck,
                layout.upperChest,
                layout.left.clavicle,
                layout.left.upperArm,
                layout.left.forearm,
                layout.left.hand,
                layout.left.restChainValid ?
                    static_cast<int>(layout.left.restChainBones.size()) : 0,
                layout.left.palmToHandValid ? 1 : 0,
                layout.leftFingers.mappedSegments,
                layout.right.clavicle,
                layout.right.upperArm,
                layout.right.forearm,
                layout.right.hand,
                layout.right.restChainValid ?
                    static_cast<int>(layout.right.restChainBones.size()) : 0,
                layout.right.palmToHandValid ? 1 : 0,
                layout.rightFingers.mappedSegments);
            if (HooksWorldPoseArmChainValid(
                    layout.left,
                    layout.parents,
                    layout.numBones) &&
                !layout.left.palmToHandValid)
            {
                Game::logMsg(
                    "[VR][WorldPose] left bind palm unavailable; using position-only wrist fallback");
            }
            if (HooksWorldPoseArmChainValid(
                    layout.right,
                    layout.parents,
                    layout.numBones) &&
                !layout.right.palmToHandValid)
            {
                Game::logMsg(
                    "[VR][WorldPose] right bind palm unavailable; using position-only wrist fallback");
            }
        }
        return layout.valid;
    }

    inline HooksWorldPoseBoneLayout* HooksWorldPoseGetBoneLayout(
        void* drawState)
    {
        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(
                drawState,
                studioHdr) ||
            !studioHdr)
        {
            return nullptr;
        }

        static thread_local std::unordered_map<
            const uint8_t*,
            HooksWorldPoseBoneLayout> s_layouts;
        auto found = s_layouts.find(studioHdr);
        if (found != s_layouts.end())
            return found->second.valid ? &found->second : nullptr;

        if (s_layouts.size() >= 32u)
            s_layouts.clear();

        HooksWorldPoseBoneLayout newLayout{};
        HooksWorldPoseBuildBoneLayout(drawState, newLayout);
        auto inserted =
            s_layouts.emplace(studioHdr, std::move(newLayout));
        return inserted.first->second.valid
            ? &inserted.first->second
            : nullptr;
    }

    inline bool HooksWorldPoseBuildBodyLocalTransform(
        const VRTrackedPoseLocal& localPose,
        const ModelRenderInfo_t& info,
        vr_vm_stabilize::Mat3x4& out)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        const float bodyYaw = HooksWorldPoseWrapAngle(info.angles.y);
        QAngle::AngleVectors(
            QAngle(0.0f, bodyYaw, 0.0f),
            &forward,
            &right,
            &up);
        const Vector worldPosition =
            info.origin +
            forward * localPose.position.x +
            right * localPose.position.y +
            up * localPose.position.z;
        const QAngle worldAngles(
            HooksWorldPoseWrapAngle(localPose.angles.x),
            HooksWorldPoseWrapAngle(localPose.angles.y + bodyYaw),
            HooksWorldPoseWrapAngle(localPose.angles.z));
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(worldPosition) ||
            !std::isfinite(worldAngles.x) ||
            !std::isfinite(worldAngles.y) ||
            !std::isfinite(worldAngles.z))
        {
            return false;
        }

        vr_vm_stabilize::BuildFromOrgAngles(
            worldPosition,
            worldAngles,
            out);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
    }

    inline bool HooksWorldPoseWorldPositionToBodyLocal(
        const Vector& worldPosition,
        const ModelRenderInfo_t& info,
        Vector& outLocalPosition)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(
            QAngle(
                0.0f,
                HooksWorldPoseWrapAngle(info.angles.y),
                0.0f),
            &forward,
            &right,
            &up);
        const Vector delta = worldPosition - info.origin;
        outLocalPosition = Vector(
            DotProduct(delta, forward),
            DotProduct(delta, right),
            DotProduct(delta, up));
        return HooksNativeViewmodelHandsOnlyVectorFinite(
            outLocalPosition);
    }

    inline int HooksWorldPoseResolveEyeAnglesOffset(Game* game)
    {
        static std::once_flag s_resolveOnce;
        static int s_eyeAnglesOffset = -1;
        std::call_once(
            s_resolveOnce,
            [game]()
            {
                if (!game)
                    return;
                s_eyeAnglesOffset =
                    game->FindRecvPropOffset(
                        "DT_TerrorPlayer",
                        "m_angEyeAngles[0]");
                const bool usedFixedFallback =
                    s_eyeAnglesOffset < 0;
                if (s_eyeAnglesOffset < 0)
                {
                    // This is the verified offset for the supported L4D2
                    // client build. Keep it as a compatibility fallback if a
                    // replacement client table hides the array element name.
                    s_eyeAnglesOffset = 0x196C;
                }
                Game::logMsg(
                    "[VR][WorldPose] eye-angle offset=0x%X source=%s",
                    s_eyeAnglesOffset,
                    usedFixedFallback
                        ? "verified-fallback"
                        : "recvprop");
            });
        return s_eyeAnglesOffset;
    }

    inline bool HooksWorldPoseReadEyeAnglesAtOffset(
        const C_BaseEntity* entity,
        int eyeAnglesOffset,
        QAngle& outAngles)
    {
        if (!entity || eyeAnglesOffset < 0)
            return false;

        float pitch = 0.0f;
        float yaw = 0.0f;
#ifdef _MSC_VER
        __try
        {
#endif
            pitch =
                ReadNetvar<float>(
                    entity,
                    eyeAnglesOffset);
            yaw =
                ReadNetvar<float>(
                    entity,
                    eyeAnglesOffset +
                        static_cast<int>(sizeof(float)));
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
        if (!std::isfinite(pitch) ||
            !std::isfinite(yaw) ||
            std::fabs(pitch) > 720.0f ||
            std::fabs(yaw) > 720.0f)
        {
            return false;
        }

        outAngles = QAngle(
            HooksWorldPoseWrapAngle(pitch),
            HooksWorldPoseWrapAngle(yaw),
            0.0f);
        return true;
    }

    inline bool HooksWorldPoseReadSourceEyeAngles(
        Game* game,
        const C_BaseEntity* entity,
        QAngle& outAngles)
    {
        return HooksWorldPoseReadEyeAnglesAtOffset(
            entity,
            HooksWorldPoseResolveEyeAnglesOffset(game),
            outAngles);
    }

    inline void HooksWorldPoseUpdateVisualBodyYaw(
        const VR* vr,
        float targetBodyYaw,
        bool instant,
        HooksWorldPoseCalibration& calibration,
        float& outVisualBodyYaw,
        float& outYawError,
        bool& outTurning)
    {
        targetBodyYaw =
            HooksWorldPoseWrapAngle(targetBodyYaw);
        const std::uint64_t now =
            static_cast<std::uint64_t>(GetTickCount64());
        if (instant)
        {
            // The local third-person body is part of the player's own tracked
            // rig. Leaving the comfort deadzone/turn-rate limiter active here
            // makes the HMD, hands and torso use different yaw frames for
            // several rendered frames, visibly twisting the neck and mesh.
            calibration.visualBodyYawValid = true;
            calibration.visualBodyYawTurning = false;
            calibration.visualBodyYaw = targetBodyYaw;
            calibration.visualBodyYawTickMs = now;
            outVisualBodyYaw = targetBodyYaw;
            outYawError = 0.0f;
            outTurning = false;
            return;
        }
        if (!calibration.visualBodyYawValid ||
            !std::isfinite(calibration.visualBodyYaw))
        {
            calibration.visualBodyYawValid = true;
            calibration.visualBodyYawTurning = false;
            calibration.visualBodyYaw = targetBodyYaw;
            calibration.visualBodyYawTickMs = now;
        }

        float deltaSeconds = 0.0f;
        if (now > calibration.visualBodyYawTickMs)
        {
            deltaSeconds =
                std::clamp(
                    static_cast<float>(
                        now -
                        calibration.visualBodyYawTickMs) /
                        1000.0f,
                    0.0f,
                    0.10f);
        }
        calibration.visualBodyYawTickMs = now;

        const float deadzone =
            std::clamp(
                vr
                    ? vr->m_WorldModelVRPoseBodyYawDeadzoneDeg
                    : 35.0f,
                0.0f,
                90.0f);
        // Once turning begins, continue until the remaining offset is well
        // inside the entry threshold. This hysteresis prevents rapid
        // start/stop oscillation at the edge of the comfort cone.
        const float releaseThreshold =
            deadzone > 0.0f
                ? std::clamp(
                    deadzone * 0.45f,
                    std::min(3.0f, deadzone),
                    deadzone)
                : 0.0f;
        float yawError =
            HooksWorldPoseWrapAngle(
                targetBodyYaw -
                calibration.visualBodyYaw);
        if (!calibration.visualBodyYawTurning &&
            std::fabs(yawError) > deadzone)
        {
            calibration.visualBodyYawTurning = true;
        }

        if (calibration.visualBodyYawTurning &&
            deltaSeconds > 0.0f)
        {
            const float desiredVisualYaw =
                HooksWorldPoseWrapAngle(
                    targetBodyYaw -
                    std::copysign(
                        releaseThreshold,
                        yawError));
            const float desiredDelta =
                HooksWorldPoseWrapAngle(
                    desiredVisualYaw -
                    calibration.visualBodyYaw);
            const float turnSpeed =
                std::clamp(
                    vr
                        ? vr->
                            m_WorldModelVRPoseBodyYawTurnSpeedDegPerSecond
                        : 180.0f,
                    30.0f,
                    720.0f);
            const float maximumStep =
                turnSpeed * deltaSeconds;
            calibration.visualBodyYaw =
                HooksWorldPoseWrapAngle(
                    calibration.visualBodyYaw +
                    std::clamp(
                        desiredDelta,
                        -maximumStep,
                        maximumStep));
            yawError =
                HooksWorldPoseWrapAngle(
                    targetBodyYaw -
                    calibration.visualBodyYaw);
            if (std::fabs(yawError) <=
                releaseThreshold + 0.25f)
            {
                calibration.visualBodyYawTurning = false;
            }
        }

        outVisualBodyYaw = calibration.visualBodyYaw;
        outYawError =
            HooksWorldPoseWrapAngle(
                targetBodyYaw -
                calibration.visualBodyYaw);
        outTurning = calibration.visualBodyYawTurning;
    }

    inline bool HooksWorldPoseApplyVisualBodyYaw(
        const HooksWorldPoseBoneLayout& layout,
        const Vector& pivot,
        float sourceBodyYaw,
        float visualBodyYaw,
        float weight,
        vr_vm_stabilize::Mat3x4* bones,
        float& outAppliedBodyYaw)
    {
        outAppliedBodyYaw =
            HooksWorldPoseWrapAngle(sourceBodyYaw);
        if (!bones ||
            layout.numBones <= 0 ||
            layout.numBones > 512 ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(pivot) ||
            !std::isfinite(sourceBodyYaw) ||
            !std::isfinite(visualBodyYaw))
        {
            return false;
        }

        const float fraction =
            std::clamp(weight, 0.0f, 1.0f);
        const float yawOffset =
            HooksWorldPoseWrapAngle(
                visualBodyYaw -
                sourceBodyYaw) *
            fraction;
        outAppliedBodyYaw =
            HooksWorldPoseWrapAngle(
                sourceBodyYaw +
                yawOffset);
        if (std::fabs(yawOffset) <= 0.001f)
            return false;

        vr_vm_stabilize::Mat3x4 sourceAtPivot{};
        vr_vm_stabilize::Mat3x4 visualAtPivot{};
        vr_vm_stabilize::Mat3x4 inverseSource{};
        vr_vm_stabilize::Mat3x4 delta{};
        vr_vm_stabilize::BuildFromOrgAngles(
            pivot,
            QAngle(
                0.0f,
                HooksWorldPoseWrapAngle(sourceBodyYaw),
                0.0f),
            sourceAtPivot);
        vr_vm_stabilize::BuildFromOrgAngles(
            pivot,
            QAngle(
                0.0f,
                outAppliedBodyYaw,
                0.0f),
            visualAtPivot);
        vr_vm_stabilize::InvertTR(
            sourceAtPivot,
            inverseSource);
        vr_vm_stabilize::Mul(
            visualAtPivot,
            inverseSource,
            delta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(delta))
            return false;

        bool applied = false;
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 transformed{};
            vr_vm_stabilize::Mul(
                delta,
                bones[bone],
                transformed);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    transformed))
            {
                return false;
            }
            bones[bone] = transformed;
            applied = true;
        }
        return applied;
    }

    inline bool HooksWorldPoseBuildRigidBoneTransform(
        const vr_vm_stabilize::Mat3x4& bone,
        vr_vm_stabilize::Mat3x4& out)
    {
        QAngle angles{};
        const Vector origin = vr_vm_stabilize::GetOrigin(bone);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(origin) ||
            !HooksViewmodelAutoGripMatrixAngles(bone, angles))
        {
            return false;
        }
        vr_vm_stabilize::BuildFromOrgAngles(origin, angles, out);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
    }

    inline bool HooksWorldPoseGetActiveWeaponSafe(
        const C_BaseEntity* playerEntity,
        C_BaseCombatWeapon*& outWeapon,
        void*& outRenderable)
    {
        outWeapon = nullptr;
        outRenderable = nullptr;
        if (!playerEntity)
            return false;

#ifdef _MSC_VER
        __try
        {
#endif
            outWeapon =
                const_cast<C_BasePlayer*>(
                    reinterpret_cast<const C_BasePlayer*>(
                        playerEntity))->GetActiveWeapon();
            if (outWeapon)
            {
                outRenderable =
                    outWeapon->GetClientRenderable();
            }
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outWeapon = nullptr;
            outRenderable = nullptr;
        }
#endif
        return outWeapon != nullptr;
    }

    inline bool HooksWorldPoseReadWeaponClipSafe(
        const C_BaseCombatWeapon* weapon,
        int& outClip)
    {
        outClip = -2147483647;
        if (!weapon)
            return false;
#ifdef _MSC_VER
        __try
        {
#endif
            outClip = ReadNetvar<int>(
                weapon,
                VR::kClip1Offset);
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outClip = -2147483647;
            return false;
        }
#endif
        return true;
    }

    inline bool HooksWorldPoseReadEntityHandleSafe(
        const C_BaseEntity* entity,
        size_t offset,
        std::uint32_t& outHandle)
    {
        outHandle = 0u;
        if (!entity || offset > 0x10000u)
            return false;

#ifdef _MSC_VER
        __try
        {
#endif
            outHandle =
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(
                        entity) +
                    offset);
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outHandle = 0u;
            return false;
        }
#endif
        const unsigned int entityIndex =
            outHandle & 0x0FFFu;
        return outHandle != 0u &&
            outHandle != 0xFFFFFFFFu &&
            entityIndex > 0u &&
            entityIndex <= 2048u;
    }

    inline C_BaseEntity* HooksWorldPoseResolveHandleSafe(
        Game* game,
        std::uint32_t handle)
    {
        if (!game ||
            !game->m_ClientEntityList ||
            handle == 0u ||
            handle == 0xFFFFFFFFu)
        {
            return nullptr;
        }

        C_BaseEntity* entity = nullptr;
#ifdef _MSC_VER
        __try
        {
#endif
            entity =
                reinterpret_cast<C_BaseEntity*>(
                    game->m_ClientEntityList->
                        GetClientEntityFromHandle(
                            static_cast<int>(handle)));
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            entity = nullptr;
        }
#endif
        return entity;
    }

    inline int HooksWorldPoseResolveActiveWeaponEntityIndex(
        Game* game,
        const C_BaseEntity* playerEntity,
        const C_BaseCombatWeapon* activeWeapon)
    {
        if (!game || !playerEntity || !activeWeapon)
            return -1;

        // Verified L4D2 client DT_BaseCombatCharacter::m_hActiveWeapon.
        constexpr size_t kActiveWeaponHandleOffset = 0x1084u;
        std::uint32_t handle = 0u;
        if (!HooksWorldPoseReadEntityHandleSafe(
                playerEntity,
                kActiveWeaponHandleOffset,
                handle))
        {
            return -1;
        }

        const int entityIndex =
            static_cast<int>(handle & 0x0FFFu);
        C_BaseEntity* const resolved =
            HooksWorldPoseResolveHandleSafe(game, handle);
        return resolved == activeWeapon
            ? entityIndex
            : -1;
    }

    class HooksWorldPoseClientRenderableProbe
    {
    public:
        virtual IClientUnknown* GetIClientUnknown() = 0;
    };

    inline C_BaseEntity* HooksWorldPoseGetRenderableBaseEntitySafe(
        void* renderable)
    {
        if (!renderable)
            return nullptr;

        C_BaseEntity* entity = nullptr;
#ifdef _MSC_VER
        __try
        {
#endif
            IClientUnknown* const unknown =
                reinterpret_cast<
                    HooksWorldPoseClientRenderableProbe*>(
                        renderable)->GetIClientUnknown();
            if (unknown)
            {
                entity =
                    reinterpret_cast<C_BaseEntity*>(
                        unknown->GetBaseEntity());
            }
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            entity = nullptr;
        }
#endif
        return entity;
    }

    inline void* HooksWorldPoseGetEntityRenderableSafe(
        const C_BaseEntity* entity)
    {
        if (!entity)
            return nullptr;

        void* renderable = nullptr;
#ifdef _MSC_VER
        __try
        {
#endif
            renderable =
                const_cast<C_BaseEntity*>(entity)->
                    GetClientRenderable();
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            renderable = nullptr;
        }
#endif
        return renderable;
    }

    inline const model_t* HooksWorldPoseGetWeaponWorldModelSafe(
        Game* game,
        const C_BaseCombatWeapon* weapon)
    {
        if (!game ||
            !game->m_ModelInfo ||
            !weapon)
        {
            return nullptr;
        }

        // Verified L4D2 client
        // DT_BaseCombatWeapon::m_iWorldModelIndex.
        constexpr size_t kWorldModelIndexOffset = 0x974u;
        const model_t* model = nullptr;
#ifdef _MSC_VER
        __try
        {
#endif
            const int modelIndex =
                *reinterpret_cast<const int*>(
                    reinterpret_cast<const std::uint8_t*>(
                        weapon) +
                    kWorldModelIndexOffset);
            if (modelIndex > 0 && modelIndex < 65536)
            {
                model =
                    reinterpret_cast<const model_t*>(
                        game->m_ModelInfo->GetModel(
                            modelIndex));
            }
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            model = nullptr;
        }
#endif
        return model;
    }

    inline const model_t* HooksWorldPoseGetRenderableModelSafe(
        void* renderable)
    {
        if (!renderable)
            return nullptr;

        const model_t* model = nullptr;
#ifdef _MSC_VER
        __try
        {
#endif
            void** const vtable =
                *reinterpret_cast<void***>(renderable);
            if (vtable && vtable[8])
            {
                using GetModelFn =
                    const model_t* (__thiscall*)(void*);
                model =
                    reinterpret_cast<GetModelFn>(
                        vtable[8])(renderable);
            }
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            model = nullptr;
        }
#endif
        return model;
    }

    inline void HooksWorldPoseCollectLinkedEntities(
        Game* game,
        const C_BaseEntity* entity,
        std::array<const C_BaseEntity*, 8>& outEntities)
    {
        outEntities.fill(nullptr);
        if (!game || !entity)
            return;

        // L4D2 client offsets.  A held world model can be submitted as the
        // weapon itself or as a bonemerge child whose owner/moveparent points
        // at the active weapon or directly at the player.
        constexpr std::array<size_t, 2> kHandleOffsets = {
            0x138u, // DT_BaseEntity::m_hOwnerEntity
            0x134u, // DT_BaseEntity::moveparent
        };

        size_t count = 0u;
        for (const size_t offset : kHandleOffsets)
        {
            std::uint32_t handle = 0u;
            if (!HooksWorldPoseReadEntityHandleSafe(
                    entity,
                    offset,
                    handle))
            {
                continue;
            }
            C_BaseEntity* const linked =
                HooksWorldPoseResolveHandleSafe(
                    game,
                    handle);
            if (!linked || linked == entity)
                continue;

            bool duplicate = false;
            for (size_t index = 0u;
                 index < count;
                 ++index)
            {
                if (outEntities[index] == linked)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && count < outEntities.size())
                outEntities[count++] = linked;
        }

        // Some cosmetic/upgrade renderables are parented to the weapon, whose
        // own owner then points to the player.  Resolve one additional hop.
        const size_t firstLevelCount = count;
        for (size_t first = 0u;
             first < firstLevelCount &&
             count < outEntities.size();
             ++first)
        {
            const C_BaseEntity* const linked =
                outEntities[first];
            for (const size_t offset : kHandleOffsets)
            {
                std::uint32_t handle = 0u;
                if (!HooksWorldPoseReadEntityHandleSafe(
                        linked,
                        offset,
                        handle))
                {
                    continue;
                }
                C_BaseEntity* const second =
                    HooksWorldPoseResolveHandleSafe(
                        game,
                        handle);
                if (!second ||
                    second == entity ||
                    second == linked)
                {
                    continue;
                }

                bool duplicate = false;
                for (size_t index = 0u;
                     index < count;
                     ++index)
                {
                    if (outEntities[index] == second)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && count < outEntities.size())
                    outEntities[count++] = second;
            }
        }
    }

    inline bool HooksWorldPoseLoadWeaponAnchorCalibration(
        void* renderable,
        const HooksWorldPoseWeaponHandState& handState,
        int maxBones,
        HooksWorldPoseWeaponAnchorCalibration& outCalibration,
        std::uint64_t now)
    {
        outCalibration = HooksWorldPoseWeaponAnchorCalibration{};
        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponAnchorCalibrationMutex);
        for (HooksWorldPoseWeaponAnchorCalibration& calibration :
             g_HooksWorldPoseWeaponAnchorCalibrations)
        {
            if (!calibration.valid ||
                calibration.renderable != renderable ||
                calibration.weaponEntity != handState.weaponEntity ||
                calibration.weaponWorldModel != handState.weaponWorldModel ||
                calibration.maxBones != maxBones ||
                calibration.anchorBone < 0 ||
                calibration.anchorBone >= maxBones)
            {
                continue;
            }

            calibration.lastUsedTickMs = now;
            outCalibration = calibration;
            return true;
        }
        return false;
    }

    inline void HooksWorldPoseStoreWeaponAnchorCalibration(
        void* renderable,
        const HooksWorldPoseWeaponHandState& handState,
        int maxBones,
        int anchorBone,
        const vr_vm_stabilize::Mat3x4& handToWeaponAnchor,
        std::uint64_t now)
    {
        if (!renderable ||
            !handState.weaponEntity ||
            maxBones <= 0 ||
            maxBones > 512 ||
            anchorBone < 0 ||
            anchorBone >= maxBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                handToWeaponAnchor))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponAnchorCalibrationMutex);
        HooksWorldPoseWeaponAnchorCalibration* destination = nullptr;
        for (HooksWorldPoseWeaponAnchorCalibration& calibration :
             g_HooksWorldPoseWeaponAnchorCalibrations)
        {
            if (calibration.valid &&
                calibration.renderable == renderable &&
                calibration.weaponEntity == handState.weaponEntity &&
                calibration.weaponWorldModel == handState.weaponWorldModel)
            {
                destination = &calibration;
                break;
            }
            if (!destination ||
                !calibration.valid ||
                calibration.lastUsedTickMs < destination->lastUsedTickMs)
            {
                destination = &calibration;
            }
        }
        if (!destination)
            return;

        destination->valid = true;
        destination->renderable = renderable;
        destination->weaponEntity = handState.weaponEntity;
        destination->weaponWorldModel = handState.weaponWorldModel;
        destination->maxBones = maxBones;
        destination->anchorBone = anchorBone;
        destination->lastUsedTickMs = now;
        destination->handToWeaponAnchor = handToWeaponAnchor;
    }

    inline bool HooksWorldPoseBuildCurrentWeaponFinalHand(
        VR* vr,
        Game* game,
        const HooksWorldPoseWeaponHandState& handState,
        vr_vm_stabilize::Mat3x4& outFinalHand)
    {
        if (!vr ||
            !game ||
            !handState.controllerToFinalHandValid ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(
                handState.playerOrigin) ||
            !std::isfinite(handState.playerAngles.x) ||
            !std::isfinite(handState.playerAngles.y) ||
            !std::isfinite(handState.playerAngles.z))
        {
            return false;
        }

        VRPoseFrame currentPose{};
        float freshness = 0.0f;
        if (!game->GetInterpolatedVRPose(
                handState.playerIndex,
                vr->m_WorldModelVRPoseInterpolationMs,
                vr->m_WorldModelVRPoseStaleAfterMs,
                currentPose,
                freshness) ||
            freshness <= 0.0001f ||
            (currentPose.validMask &
             l4d2vr_pose::kValidRightHand) == 0u)
        {
            return false;
        }

        ModelRenderInfo_t playerInfo{};
        playerInfo.origin = handState.playerOrigin;
        playerInfo.angles = handState.playerAngles;

        // A weapon SetupBones call can precede its owner's DrawModel call. In
        // that order the published hand state is one frame old, so alternating
        // draw order makes the gun jump between old and current body origins
        // during locomotion. Read the owner's current render transform (the
        // same interpolated basis its next DrawModel uses) before rebuilding
        // the controller target; retain the published transform as fallback.
        if (handState.playerRenderable)
        {
            Vector currentRenderOrigin{};
            QAngle currentRenderAngles{};
            bool currentRenderTransformValid = false;
#ifdef _MSC_VER
            __try
            {
#endif
                void** const vtable =
                    *reinterpret_cast<void***>(
                        handState.playerRenderable);
                if (vtable && vtable[1] && vtable[2])
                {
                    using GetRenderOriginFn =
                        const Vector& (__thiscall*)(void*);
                    using GetRenderAnglesFn =
                        const QAngle& (__thiscall*)(void*);
                    const Vector& renderOrigin =
                        reinterpret_cast<GetRenderOriginFn>(
                            vtable[1])(
                                handState.playerRenderable);
                    const QAngle& renderAngles =
                        reinterpret_cast<GetRenderAnglesFn>(
                            vtable[2])(
                                handState.playerRenderable);
                    currentRenderOrigin = renderOrigin;
                    currentRenderAngles = renderAngles;
                    currentRenderTransformValid =
                        HooksNativeViewmodelHandsOnlyVectorFinite(
                            currentRenderOrigin) &&
                        std::isfinite(currentRenderAngles.x) &&
                        std::isfinite(currentRenderAngles.y) &&
                        std::isfinite(currentRenderAngles.z);
                }
#ifdef _MSC_VER
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                currentRenderTransformValid = false;
            }
#endif
            if (currentRenderTransformValid)
            {
                playerInfo.origin = currentRenderOrigin;
                playerInfo.angles = currentRenderAngles;
            }
        }
        if (currentPose.bodyYawValid)
        {
            playerInfo.angles.y = currentPose.bodyYaw;
        }
        vr_vm_stabilize::Mat3x4 currentController{};
        vr_vm_stabilize::Mat3x4 predictedFinal{};
        if (!HooksWorldPoseBuildBodyLocalTransform(
                currentPose.rightHand,
                playerInfo,
                currentController))
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            currentController,
            handState.controllerToFinalHand,
            predictedFinal);
        return HooksWorldPoseBuildRigidBoneTransform(
            predictedFinal,
            outFinalHand);
    }

    inline bool HooksWorldPosePublishWeaponHandState(
        VR* vr,
        Game* game,
        const C_BaseEntity* playerEntity,
        int playerIndex,
        const ModelRenderInfo_t& info,
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const vr_vm_stabilize::Mat3x4* finalBones,
        const vr_vm_stabilize::Mat3x4& rightControllerWorld,
        bool rightArmSolved,
        bool allowControllerPrediction)
    {
        if (!game ||
            !playerEntity ||
            !sourceBones ||
            !finalBones ||
            playerIndex <= 0 ||
            !game->IsValidPlayerIndex(playerIndex) ||
            !rightArmSolved ||
            layout.right.hand < 0 ||
            layout.right.hand >= layout.numBones)
        {
            return false;
        }

        C_BaseCombatWeapon* activeWeapon = nullptr;
        void* activeWeaponRenderable = nullptr;
        if (!HooksWorldPoseGetActiveWeaponSafe(
                playerEntity,
                activeWeapon,
                activeWeaponRenderable) ||
            !activeWeaponRenderable)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 nativeHand{};
        vr_vm_stabilize::Mat3x4 finalHand{};
        if (!vr_vm_stabilize::SafeRead(
                sourceBones + layout.right.hand,
                nativeHand) ||
            !vr_vm_stabilize::SafeRead(
                finalBones + layout.right.hand,
                finalHand) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(nativeHand) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(finalHand))
        {
            return false;
        }

        const Vector nativeOrigin =
            vr_vm_stabilize::GetOrigin(nativeHand);
        const Vector finalOrigin =
            vr_vm_stabilize::GetOrigin(finalHand);
        const float handDisplacement =
            (finalOrigin - nativeOrigin).Length();
        if (!std::isfinite(handDisplacement) ||
            handDisplacement > 96.0f)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 nativeRigid{};
        vr_vm_stabilize::Mat3x4 finalRigid{};
        vr_vm_stabilize::Mat3x4 inverseNative{};
        vr_vm_stabilize::Mat3x4 nativeToFinal{};
        if (!HooksWorldPoseBuildRigidBoneTransform(
                nativeHand,
                nativeRigid) ||
            !HooksWorldPoseBuildRigidBoneTransform(
                finalHand,
                finalRigid))
        {
            return false;
        }
        vr_vm_stabilize::InvertTR(
            nativeRigid,
            inverseNative);
        vr_vm_stabilize::Mul(
            finalRigid,
            inverseNative,
            nativeToFinal);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                nativeToFinal))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 controllerRigid{};
        vr_vm_stabilize::Mat3x4 inverseController{};
        vr_vm_stabilize::Mat3x4 controllerToFinalHand{};
        const bool controllerToFinalHandValid =
            allowControllerPrediction &&
            HooksWorldPoseBuildRigidBoneTransform(
                rightControllerWorld,
                controllerRigid);
        if (controllerToFinalHandValid)
        {
            vr_vm_stabilize::InvertTR(
                controllerRigid,
                inverseController);
            vr_vm_stabilize::Mul(
                inverseController,
                finalRigid,
                controllerToFinalHand);
        }
        const bool controllerRelationFinite =
            controllerToFinalHandValid &&
            HooksNativeViewmodelHandsOnlyMatrixFinite(
                controllerToFinalHand);

        HooksWorldPoseWeaponHandState published{};
        published.valid = true;
        published.playerIndex = playerIndex;
        published.weaponEntityIndex =
            HooksWorldPoseResolveActiveWeaponEntityIndex(
                game,
                playerEntity,
                activeWeapon);
        published.playerEntity = playerEntity;
        published.playerRenderable =
            HooksWorldPoseGetEntityRenderableSafe(
                playerEntity);
        published.weaponEntity = activeWeapon;
        published.weaponRenderable = activeWeaponRenderable;
        published.weaponWorldModel =
            HooksWorldPoseGetWeaponWorldModelSafe(
                game,
                activeWeapon);
        published.updatedTickMs =
            static_cast<std::uint64_t>(GetTickCount64());
        published.controllerToFinalHandValid =
            controllerRelationFinite;
        published.playerOrigin = info.origin;
        published.playerAngles = info.angles;
        published.nativeHand = nativeRigid;
        published.finalHand = finalRigid;
        published.nativeToFinal = nativeToFinal;
        if (controllerRelationFinite)
        {
            published.controllerToFinalHand =
                controllerToFinalHand;
        }

        {
            std::lock_guard<std::mutex> lock(
                g_HooksWorldPoseWeaponHandMutex);
            g_HooksWorldPoseWeaponHandStates[
                static_cast<size_t>(playerIndex)] =
                    published;
        }
        g_HooksWorldPosePendingWeaponRenderable.store(
            activeWeaponRenderable,
            std::memory_order_release);

        if (vr && vr->m_WorldModelVRPoseDebugLog)
        {
            static thread_local std::array<
                std::uint64_t,
                Game::kMaxPlayers> s_lastPublishLog{};
            std::uint64_t& last =
                s_lastPublishLog[
                    static_cast<size_t>(playerIndex)];
            if (published.updatedTickMs - last >= 1000u)
            {
                last = published.updatedTickMs;
                Game::logMsg(
                    "[VR][WorldPoseWeapon] hand cache player=%d weapon=%p weaponIndex=%d renderable=%p worldModel=%p displacement=%.1f prediction=%d",
                    playerIndex,
                    activeWeapon,
                    published.weaponEntityIndex,
                    activeWeaponRenderable,
                    published.weaponWorldModel,
                    handDisplacement,
                    published.controllerToFinalHandValid ? 1 : 0);
            }
        }
        return true;
    }

    inline void HooksWorldPoseClearWeaponHandState(int playerIndex)
    {
        if (playerIndex <= 0 ||
            playerIndex >= Game::kMaxPlayers)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponHandMutex);
        g_HooksWorldPoseWeaponHandStates[
            static_cast<size_t>(playerIndex)] =
                HooksWorldPoseWeaponHandState{};
    }

    inline bool HooksWorldPoseProbeWeaponSetupBonesTarget(
        void* renderable,
        void*& outTarget)
    {
        outTarget = nullptr;
        if (!renderable)
            return false;

#ifdef _MSC_VER
        __try
        {
#endif
            void** const vtable =
                *reinterpret_cast<void***>(renderable);
            if (!vtable)
                return false;

            // L4D2 IClientRenderable::SetupBones is slot 13. The preceding
            // slots (3, 8 and 9) are already verified by the local-body hook.
            outTarget = vtable[13];
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outTarget = nullptr;
            return false;
        }
#endif
        return outTarget != nullptr;
    }

    bool HooksWorldPoseEnsureWeaponSetupBonesHookMainThread(
        VR* vr,
        Game* game)
    {
        if (!vr ||
            !game ||
            !vr->m_WorldModelVRPoseEnabled ||
            !vr->m_FirstPersonControlReady.load(std::memory_order_acquire))
        {
            return false;
        }

        void* const renderable =
            g_HooksWorldPosePendingWeaponRenderable.load(
                std::memory_order_acquire);
        if (!renderable)
            return false;

        void* target = nullptr;
        if (!HooksWorldPoseProbeWeaponSetupBonesTarget(
                renderable,
                target) ||
            !HooksFirstPersonBodyExecutableAddress(target))
        {
            return false;
        }

        if (g_HooksWorldPoseWeaponSetupBonesHookReady.load(
                std::memory_order_acquire))
        {
            const bool sameTarget =
                target ==
                g_HooksWorldPoseWeaponSetupBonesTarget.load(
                    std::memory_order_acquire);
            if (!sameTarget &&
                vr->m_WorldModelVRPoseDebugLog)
            {
                static std::atomic<bool>
                    s_loggedDifferentTarget{ false };
                if (!s_loggedDifferentTarget.exchange(
                        true,
                        std::memory_order_acq_rel))
                {
                    Game::logMsg(
                        "[VR][WorldPoseWeapon] SetupBones target changed; current class remains on native path installed=%p new=%p renderable=%p",
                        g_HooksWorldPoseWeaponSetupBonesTarget.load(
                            std::memory_order_acquire),
                        target,
                        renderable);
                }
            }
            return sameTarget;
        }

        std::lock_guard<std::mutex> lock(
            g_HooksWorldPoseWeaponSetupBonesHookMutex);
        if (g_HooksWorldPoseWeaponSetupBonesHookReady.load(
                std::memory_order_acquire))
        {
            return target ==
                g_HooksWorldPoseWeaponSetupBonesTarget.load(
                    std::memory_order_acquire);
        }

        if (!Hooks::hkWorldPoseWeaponSetupBones.pTarget &&
            Hooks::hkWorldPoseWeaponSetupBones.createHook(
                target,
                reinterpret_cast<LPVOID>(
                    &Hooks::dWorldPoseWeaponSetupBones)) != 0)
        {
            return false;
        }
        if (!Hooks::hkWorldPoseWeaponSetupBones.isEnabled &&
            Hooks::hkWorldPoseWeaponSetupBones.enableHook() != 0)
        {
            return false;
        }

        g_HooksWorldPoseWeaponSetupBonesTarget.store(
            target,
            std::memory_order_release);
        g_HooksWorldPoseWeaponSetupBonesHookReady.store(
            true,
            std::memory_order_release);
        Game::logMsg(
            "[VR][WorldPoseWeapon] weapon SetupBones hook installed renderable=%p target=%p slot=13",
            renderable,
            target);
        return true;
    }

    inline bool HooksWorldPoseApplyWeaponSetupBones(
        VR* vr,
        Game* game,
        void* renderable,
        matrix3x4_t* boneToWorldOut,
        int maxBones,
        int boneMask,
        float currentTime)
    {
        if (!vr ||
            !vr->m_WorldModelVRPoseEnabled ||
            !vr->m_FirstPersonControlReady.load(std::memory_order_acquire) ||
            !game ||
            !renderable ||
            !boneToWorldOut ||
            maxBones <= 0 ||
            maxBones > 512)
        {
            return false;
        }
        // Stereo scene generation, renderable identity and the bone request
        // signature define cache lifetime. The two eye calls may carry tiny
        // currentTime differences and must still replay identical matrices.
        (void)currentTime;

        const std::uint64_t now =
            static_cast<std::uint64_t>(GetTickCount64());
        const std::uint64_t stereoSceneGeneration =
            g_HooksWorldPoseStereoSceneGeneration.load(
                std::memory_order_acquire);
        const std::uint32_t renderFrameSequence =
            vr->m_RenderFrameSeq.load(
                std::memory_order_acquire) &
            ~1u;
        auto* const setupBones =
            reinterpret_cast<vr_vm_stabilize::Mat3x4*>(
                boneToWorldOut);
        if (HooksWorldPoseLoadStereoWeaponBones(
                renderable,
                stereoSceneGeneration,
                maxBones,
                boneMask,
                setupBones,
                now))
        {
            if (vr->m_WorldModelVRPoseDebugLog)
            {
                static thread_local std::uint64_t
                    s_lastStereoReplayLog = 0u;
                if (now - s_lastStereoReplayLog >= 1000u)
                {
                    s_lastStereoReplayLog = now;
                    Game::logMsg(
                        "[VR][WorldPoseWeapon] stereo SetupBones cache replay renderable=%p frame=%u bones=%d mask=0x%X",
                        renderable,
                        renderFrameSequence,
                        maxBones,
                        boneMask);
                }
            }
            return true;
        }
        const C_BaseEntity* const renderableEntity =
            HooksWorldPoseGetRenderableBaseEntitySafe(
                renderable);
        const model_t* const renderableModel =
            HooksWorldPoseGetRenderableModelSafe(
                renderable);
        std::array<const C_BaseEntity*, 8>
            renderableLinks{};
        HooksWorldPoseCollectLinkedEntities(
            game,
            renderableEntity,
            renderableLinks);
        HooksWorldPoseWeaponHandState handState{};
        int matchMask = 0;
        int bestMatchPriority = 3;
        float bestSpatialDistance = FLT_MAX;
        float nearestModelCandidateDistance = FLT_MAX;
        int nearestModelCandidatePlayer = -1;
        {
            std::lock_guard<std::mutex> lock(
                g_HooksWorldPoseWeaponHandMutex);
            for (int playerIndex = 1;
                 playerIndex < Game::kMaxPlayers;
                 ++playerIndex)
            {
                const HooksWorldPoseWeaponHandState& candidate =
                    g_HooksWorldPoseWeaponHandStates[
                        static_cast<size_t>(playerIndex)];
                if (!candidate.valid ||
                    now < candidate.updatedTickMs ||
                    now - candidate.updatedTickMs > 500u)
                {
                    continue;
                }

                constexpr int kMatchRenderable = 1 << 0;
                constexpr int kMatchEntity = 1 << 1;
                constexpr int kMatchWorldModel = 1 << 2;
                constexpr int kMatchLinkedWeapon = 1 << 3;
                constexpr int kMatchSpatialHand = 1 << 4;
                int candidateMatchMask = 0;
                if (candidate.weaponRenderable == renderable)
                    candidateMatchMask |= kMatchRenderable;
                if (candidate.weaponEntity &&
                    candidate.weaponEntity ==
                        renderableEntity)
                {
                    candidateMatchMask |= kMatchEntity;
                }
                if (candidate.weaponWorldModel &&
                    candidate.weaponWorldModel ==
                        renderableModel)
                {
                    candidateMatchMask |= kMatchWorldModel;
                }
                for (const C_BaseEntity* const linked :
                     renderableLinks)
                {
                    if (linked &&
                        linked == candidate.weaponEntity)
                    {
                        candidateMatchMask |=
                            kMatchLinkedWeapon;
                        break;
                    }
                }

                // SetupBones is shared by every C_BaseAnimating. A model
                // pointer identifies only the asset, not the owning weapon:
                // every survivor holding the same gun has the same pointer.
                // Prefer an entity/renderable/owner-chain identity. An
                // anonymous bonemerge child may use the model pointer only as
                // a candidate, then must pass the native-hand proximity test
                // below before it becomes an identity match.
                constexpr int kIdentityMatchMask =
                    kMatchRenderable |
                    kMatchEntity |
                    kMatchLinkedWeapon;
                if (candidate.playerRenderable == renderable)
                {
                    continue;
                }

                const bool hardIdentityMatch =
                    (candidateMatchMask &
                     kIdentityMatchMask) != 0;
                float spatialDistance = FLT_MAX;
                if (!hardIdentityMatch &&
                    (candidateMatchMask &
                     kMatchWorldModel) != 0)
                {
                    const Vector nativeHandOrigin =
                        vr_vm_stabilize::GetOrigin(
                            candidate.nativeHand);
                    for (int bone = 0;
                         bone < maxBones;
                         ++bone)
                    {
                        vr_vm_stabilize::Mat3x4 source{};
                        if (!vr_vm_stabilize::SafeRead(
                                setupBones + bone,
                                source) ||
                            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                                source))
                        {
                            continue;
                        }
                        const float distance =
                            (vr_vm_stabilize::GetOrigin(source) -
                             nativeHandOrigin).Length();
                        if (std::isfinite(distance))
                        {
                            spatialDistance =
                                (std::min)(
                                    spatialDistance,
                                    distance);
                        }
                    }

                    // Anonymous bonemerge children expose only their model
                    // asset. Authorize one only when its already-built native
                    // bones are physically attached to this player's native
                    // weapon hand. Teammates using the same asset are far
                    // outside this small attachment radius.
                    constexpr float
                        kMaximumAnonymousWeaponHandDistance = 24.0f;
                    if (spatialDistance <
                        nearestModelCandidateDistance)
                    {
                        nearestModelCandidateDistance =
                            spatialDistance;
                        nearestModelCandidatePlayer =
                            candidate.playerIndex;
                    }
                    if (spatialDistance <=
                        kMaximumAnonymousWeaponHandDistance)
                    {
                        candidateMatchMask |=
                            kMatchSpatialHand;
                    }
                }

                const bool spatialIdentityMatch =
                    (candidateMatchMask &
                     kMatchSpatialHand) != 0;
                if (!hardIdentityMatch &&
                    !spatialIdentityMatch)
                {
                    continue;
                }

                const int matchPriority =
                    hardIdentityMatch ? 0 : 1;
                const float matchDistance =
                    hardIdentityMatch
                        ? 0.0f
                        : spatialDistance;
                if (matchPriority > bestMatchPriority ||
                    (matchPriority == bestMatchPriority &&
                     matchDistance >= bestSpatialDistance))
                {
                    continue;
                }

                handState = candidate;
                matchMask = candidateMatchMask;
                bestMatchPriority = matchPriority;
                bestSpatialDistance = matchDistance;
            }
        }
        if (!handState.valid)
        {
            if (vr->m_WorldModelVRPoseDebugLog &&
                nearestModelCandidatePlayer > 0 &&
                std::isfinite(
                    nearestModelCandidateDistance))
            {
                static thread_local std::uint64_t
                    s_lastRejectLog = 0u;
                if (now - s_lastRejectLog >= 1000u)
                {
                    s_lastRejectLog = now;
                    Game::logMsg(
                        "[VR][WorldPoseWeapon] SetupBones rejected anonymous model renderable=%p nearestPlayer=%d handDistance=%.1f limit=24.0",
                        renderable,
                        nearestModelCandidatePlayer,
                        nearestModelCandidateDistance);
                }
            }
            return false;
        }

        vr_vm_stabilize::Mat3x4 weaponCorrection =
            handState.nativeToFinal;
        bool currentControllerCorrection = false;
        int weaponAnchorBone = -1;
        HooksWorldPoseWeaponAnchorCalibration anchorCalibration{};
        bool anchorCalibrationValid =
            HooksWorldPoseLoadWeaponAnchorCalibration(
                renderable,
                handState,
                maxBones,
                anchorCalibration,
                now);
        if (anchorCalibrationValid)
        {
            weaponAnchorBone = anchorCalibration.anchorBone;
        }
        else
        {
            const Vector nativeHandOrigin =
                vr_vm_stabilize::GetOrigin(
                    handState.nativeHand);
            float nearestAnchorDistance = FLT_MAX;
            for (int bone = 0; bone < maxBones; ++bone)
            {
                vr_vm_stabilize::Mat3x4 source{};
                if (!vr_vm_stabilize::SafeRead(
                        setupBones + bone,
                        source) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(
                        source))
                {
                    continue;
                }
                const float distance =
                    (vr_vm_stabilize::GetOrigin(source) -
                     nativeHandOrigin).Length();
                if (std::isfinite(distance) &&
                    distance < nearestAnchorDistance)
                {
                    nearestAnchorDistance = distance;
                    weaponAnchorBone = bone;
                }
            }
        }

        vr_vm_stabilize::Mat3x4 sourceAnchorRigid{};
        const bool sourceAnchorValid =
            weaponAnchorBone >= 0 &&
            weaponAnchorBone < maxBones &&
            HooksWorldPoseBuildRigidBoneTransform(
                setupBones[weaponAnchorBone],
                sourceAnchorRigid);
        const std::uint64_t handStateAgeMs =
            now >= handState.updatedTickMs
                ? now - handState.updatedTickMs
                : UINT64_MAX;
        if (!anchorCalibrationValid &&
            sourceAnchorValid &&
            handStateAgeMs <= 8u)
        {
            vr_vm_stabilize::Mat3x4 inverseNativeHand{};
            vr_vm_stabilize::Mat3x4 handToWeaponAnchor{};
            vr_vm_stabilize::InvertTR(
                handState.nativeHand,
                inverseNativeHand);
            vr_vm_stabilize::Mul(
                inverseNativeHand,
                sourceAnchorRigid,
                handToWeaponAnchor);
            if (HooksNativeViewmodelHandsOnlyMatrixFinite(
                    handToWeaponAnchor))
            {
                HooksWorldPoseStoreWeaponAnchorCalibration(
                    renderable,
                    handState,
                    maxBones,
                    weaponAnchorBone,
                    handToWeaponAnchor,
                    now);
                anchorCalibration.valid = true;
                anchorCalibration.anchorBone = weaponAnchorBone;
                anchorCalibration.handToWeaponAnchor =
                    handToWeaponAnchor;
                anchorCalibrationValid = true;
            }
        }

        if (sourceAnchorValid &&
            anchorCalibrationValid)
        {
            vr_vm_stabilize::Mat3x4 currentFinalHand{};
            vr_vm_stabilize::Mat3x4 targetAnchor{};
            vr_vm_stabilize::Mat3x4 inverseSourceAnchor{};
            vr_vm_stabilize::Mat3x4 predictedCorrection{};
            if (HooksWorldPoseBuildCurrentWeaponFinalHand(
                    vr,
                    game,
                    handState,
                    currentFinalHand))
            {
                vr_vm_stabilize::Mul(
                    currentFinalHand,
                    anchorCalibration.handToWeaponAnchor,
                    targetAnchor);
                vr_vm_stabilize::InvertTR(
                    sourceAnchorRigid,
                    inverseSourceAnchor);
                vr_vm_stabilize::Mul(
                    targetAnchor,
                    inverseSourceAnchor,
                    predictedCorrection);

                // Lock the weapon attachment bone's complete transform to the
                // current VR hand. This removes both positional and rotational
                // motion inherited from Source's one-frame-old native hand.
                // Applying one rigid correction to every weapon bone preserves
                // all child-local firing mechanics (bolt, magazine, muzzle).
                if (HooksNativeViewmodelHandsOnlyMatrixFinite(
                        predictedCorrection))
                {
                    weaponCorrection = predictedCorrection;
                    currentControllerCorrection = true;
                }
            }
        }

        vr_vm_stabilize::Mat3x4* const bones = setupBones;
        std::vector<vr_vm_stabilize::Mat3x4> stagedBones(
            static_cast<size_t>(maxBones));
        for (int bone = 0; bone < maxBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(
                    bones + bone,
                    source) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    source))
            {
                return false;
            }

            vr_vm_stabilize::Mul(
                weaponCorrection,
                source,
                stagedBones[static_cast<size_t>(bone)]);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    stagedBones[static_cast<size_t>(bone)]))
            {
                return false;
            }
        }
        std::copy(
            stagedBones.begin(),
            stagedBones.end(),
            bones);

        // The first successful weapon solve of a stereo scene is authoritative
        // for both eyes. This includes the safe nativeToFinal fallback used
        // before a freshly switched weapon has an anchor calibration; allowing
        // the second eye to upgrade to a newer prediction would recreate the
        // original left/right gun divergence.
        HooksWorldPoseStoreStereoWeaponBones(
            renderable,
            stereoSceneGeneration,
            maxBones,
            boneMask,
            bones,
            now);

        if (vr->m_WorldModelVRPoseDebugLog)
        {
            static thread_local std::uint64_t
                s_lastApplyLog = 0u;
            if (now - s_lastApplyLog >= 1000u)
            {
                s_lastApplyLog = now;
                const Vector displacement =
                    vr_vm_stabilize::GetOrigin(
                        handState.finalHand) -
                    vr_vm_stabilize::GetOrigin(
                        handState.nativeHand);
                Game::logMsg(
                    "[VR][WorldPoseWeapon] SetupBones applied player=%d renderable=%p match=0x%X handDistance=%.1f bones=%d/%d age=%llu predicted=%d anchor=%d displacement=(%.1f %.1f %.1f)",
                    handState.playerIndex,
                    renderable,
                    matchMask,
                    bestSpatialDistance,
                    maxBones,
                    maxBones,
                    static_cast<unsigned long long>(
                        now - handState.updatedTickMs),
                    currentControllerCorrection ? 1 : 0,
                    weaponAnchorBone,
                    displacement.x,
                    displacement.y,
                    displacement.z);
            }
        }
        return true;
    }

    inline bool HooksWorldPoseBuildActiveWeaponBones(
        VR* vr,
        Game* game,
        void* drawState,
        const ModelRenderInfo_t& info,
        const C_BaseEntity* drawEntity,
        bool drawLooksLikeHeldWorldModel,
        const void* sourceBonePointer,
        vr_vm_stabilize::Mat3x4*& outBones,
        vr_vm_stabilize::Mat3x4& outNativeToFinal,
        int& outPlayerIndex,
        std::uint64_t& outAgeMs,
        Vector& outHandDisplacement,
        int& outMatchMask)
    {
        outBones = nullptr;
        outNativeToFinal = vr_vm_stabilize::Identity();
        outPlayerIndex = -1;
        outAgeMs = 0u;
        outHandDisplacement = Vector(0.0f, 0.0f, 0.0f);
        outMatchMask = 0;
        if (!vr ||
            !game ||
            !vr->m_WorldModelVRPoseEnabled ||
            !vr->m_FirstPersonControlReady.load(std::memory_order_acquire))
        {
            return false;
        }

        const C_BaseEntity* const renderableBaseEntity =
            HooksWorldPoseGetRenderableBaseEntitySafe(
                info.pRenderable);
        std::array<const C_BaseEntity*, 8>
            drawEntityLinks{};
        std::array<const C_BaseEntity*, 8>
            renderableEntityLinks{};
        HooksWorldPoseCollectLinkedEntities(
            game,
            drawEntity,
            drawEntityLinks);
        if (renderableBaseEntity != drawEntity)
        {
            HooksWorldPoseCollectLinkedEntities(
                game,
                renderableBaseEntity,
                renderableEntityLinks);
        }

        const std::uint64_t now =
            static_cast<std::uint64_t>(GetTickCount64());
        const std::uint64_t stereoSceneGeneration =
            g_HooksWorldPoseStereoSceneGeneration.load(
                std::memory_order_acquire);
        if (HooksWorldPoseWeaponCorrectedInStereoScene(
                info.pRenderable,
                stereoSceneGeneration))
        {
            // SetupBones already transformed this exact renderable during the
            // current stereo scene. This per-renderable decision cannot be
            // overwritten when other weapons interleave their own calls.
            return false;
        }
        HooksWorldPoseWeaponHandState handState{};
        {
            std::lock_guard<std::mutex> lock(
                g_HooksWorldPoseWeaponHandMutex);
            for (int playerIndex = 1;
                 playerIndex < Game::kMaxPlayers;
                 ++playerIndex)
            {
                const HooksWorldPoseWeaponHandState& candidate =
                    g_HooksWorldPoseWeaponHandStates[
                        static_cast<size_t>(playerIndex)];
                if (!candidate.valid ||
                    !candidate.weaponEntity)
                {
                    continue;
                }

                const bool exactWeaponWorldModel =
                    candidate.weaponWorldModel &&
                    info.pModel ==
                        candidate.weaponWorldModel;

                // Never reinterpret the survivor's own renderable as its gun,
                // even when a bonemerge submission reuses the player's entity
                // index or network class.  The one safe exception is an exact
                // active-weapon world-model pointer: L4D2 can submit that
                // bonemerge child through the player's renderable.
                if (info.pRenderable &&
                    candidate.playerRenderable &&
                    info.pRenderable ==
                        candidate.playerRenderable &&
                    !exactWeaponWorldModel)
                {
                    continue;
                }

                constexpr int kMatchRenderable = 1 << 0;
                constexpr int kMatchDrawEntity = 1 << 1;
                constexpr int kMatchRenderableEntity = 1 << 2;
                constexpr int kMatchEntityIndex = 1 << 3;
                constexpr int kMatchLinkedWeapon = 1 << 4;
                constexpr int kMatchPlayerWorldModel = 1 << 5;
                int matchMask = 0;
                if (info.pRenderable &&
                    candidate.weaponRenderable &&
                    info.pRenderable ==
                        candidate.weaponRenderable)
                {
                    matchMask |= kMatchRenderable;
                }
                if (drawEntity &&
                    drawEntity ==
                        candidate.weaponEntity)
                {
                    matchMask |= kMatchDrawEntity;
                }
                if (renderableBaseEntity &&
                    renderableBaseEntity ==
                        candidate.weaponEntity)
                {
                    matchMask |= kMatchRenderableEntity;
                }
                if (candidate.weaponEntityIndex > 0 &&
                    info.entity_index ==
                        candidate.weaponEntityIndex)
                {
                    matchMask |= kMatchEntityIndex;
                }

                for (const C_BaseEntity* const linked :
                     drawEntityLinks)
                {
                    if (linked ==
                        candidate.weaponEntity)
                    {
                        matchMask |=
                            kMatchLinkedWeapon;
                        break;
                    }
                }
                if ((matchMask &
                     kMatchLinkedWeapon) == 0)
                {
                    for (const C_BaseEntity* const linked :
                         renderableEntityLinks)
                    {
                        if (linked ==
                            candidate.weaponEntity)
                        {
                            matchMask |=
                                kMatchLinkedWeapon;
                            break;
                        }
                    }
                }

                // A bonemerge child can report the owner's player index and
                // class.  Accept that ambiguous form only when the exact
                // cached active-weapon world-model pointer also matches and
                // either the owner index or owner renderable identifies the
                // player.  Exact model identity keeps same-model ground drops
                // from entering this fallback.
                if (drawLooksLikeHeldWorldModel &&
                    exactWeaponWorldModel &&
                    (info.entity_index ==
                         candidate.playerIndex ||
                     (info.pRenderable &&
                      info.pRenderable ==
                          candidate.playerRenderable)))
                {
                    matchMask |=
                        kMatchPlayerWorldModel;
                }
                if (matchMask == 0)
                    continue;

                handState = candidate;
                outMatchMask = matchMask;
                break;
            }
        }
        if (!handState.valid ||
            now < handState.updatedTickMs)
        {
            return false;
        }

        outAgeMs = now - handState.updatedTickMs;
        // Weapon draws can precede their owner draw, so accept the immediately
        // previous render snapshot, but never carry a stale hand across a view
        // change, death, weapon switch, or a lost pose stream.
        constexpr std::uint64_t kMaximumWeaponHandAgeMs = 250u;
        if (outAgeMs > kMaximumWeaponHandAgeMs)
            return false;

        C_BaseEntity* const currentPlayerEntity =
            HooksSafeGetClientEntity(
                game,
                handState.playerIndex);
        if (!currentPlayerEntity ||
            currentPlayerEntity !=
                handState.playerEntity)
        {
            return false;
        }

        C_BaseCombatWeapon* currentActiveWeapon = nullptr;
        void* currentActiveWeaponRenderable = nullptr;
        HooksWorldPoseGetActiveWeaponSafe(
            currentPlayerEntity,
            currentActiveWeapon,
            currentActiveWeaponRenderable);
        if (!currentActiveWeapon ||
            currentActiveWeapon != handState.weaponEntity ||
            (currentActiveWeaponRenderable &&
             currentActiveWeaponRenderable !=
                 handState.weaponRenderable))
        {
            return false;
        }

        outPlayerIndex = handState.playerIndex;
        outHandDisplacement =
            vr_vm_stabilize::GetOrigin(
                handState.finalHand) -
            vr_vm_stabilize::GetOrigin(
                handState.nativeHand);
        outNativeToFinal =
            handState.nativeToFinal;

        // Some bonemerge draws deliberately pass no custom bone array.
        // A successful identity match is still useful: the caller applies
        // this same rigid delta to ModelRenderInfo_t as a safe fallback.
        if (!drawState || !sourceBonePointer)
            return true;

        int numBones = 0;
        int boneIndex = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryGetBoneTableLayout(
                drawState,
            numBones,
            boneIndex,
            numBonesOffset) ||
            numBones <= 0 ||
            numBones > 512)
        {
            return true;
        }

        uint32_t sequence =
            vr->m_RenderFrameSeq.load(
                std::memory_order_relaxed) &
            ~1u;
        if (sequence == 0u)
        {
            sequence =
                (static_cast<uint32_t>(GetTickCount()) << 1u) |
                2u;
        }
        vr_vm_stabilize::Mat3x4* const alignedBones =
            vr_vm_stabilize::AllocStableBones(
                numBones,
                sequence);
        if (!alignedBones)
            return true;

        const auto* sourceBones =
            reinterpret_cast<
                const vr_vm_stabilize::Mat3x4*>(
                    sourceBonePointer);
        bool copiedAllBones = true;
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(
                    sourceBones + bone,
                    source) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    source))
            {
                copiedAllBones = false;
                break;
            }
            vr_vm_stabilize::Mul(
                handState.nativeToFinal,
                source,
                alignedBones[bone]);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    alignedBones[bone]))
            {
                copiedAllBones = false;
                break;
            }
        }

        if (copiedAllBones)
            outBones = alignedBones;
        return true;
    }

    inline bool HooksWorldPoseBuildEyeToHeadCalibration(
        const QAngle& sourceEyeAngles,
        const vr_vm_stabilize::Mat3x4& sourceHeadBone,
        vr_vm_stabilize::Mat3x4& outEyeToHead)
    {
        if (!std::isfinite(sourceEyeAngles.x) ||
            !std::isfinite(sourceEyeAngles.y) ||
            !std::isfinite(sourceEyeAngles.z))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 sourceHeadRigid{};
        if (!HooksWorldPoseBuildRigidBoneTransform(
                sourceHeadBone,
                sourceHeadRigid))
        {
            return false;
        }

        // Source eye angles already contain the live look direction used to
        // pose the player head. Remove that direction from the native head to
        // retain only this model's bone-axis convention. Unlike calibrating
        // directly against the HMD, this cannot bake an arbitrary first-frame
        // idle/fire head tilt into the permanent tracker correction.
        const Vector headOrigin =
            vr_vm_stabilize::GetOrigin(sourceHeadRigid);
        vr_vm_stabilize::Mat3x4 sourceEyeWorld{};
        vr_vm_stabilize::Mat3x4 inverseSourceEye{};
        vr_vm_stabilize::BuildFromOrgAngles(
            headOrigin,
            sourceEyeAngles,
            sourceEyeWorld);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                sourceEyeWorld))
        {
            return false;
        }
        vr_vm_stabilize::InvertTR(
            sourceEyeWorld,
            inverseSourceEye);
        vr_vm_stabilize::Mul(
            inverseSourceEye,
            sourceHeadRigid,
            outEyeToHead);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                outEyeToHead))
        {
            return false;
        }

        // Head position is constrained separately against Source's current
        // neck. A rotation-only basis prevents HMD rotation from orbiting the
        // head around a learned eye-to-bone translation.
        outEyeToHead.m[0][3] = 0.0f;
        outEyeToHead.m[1][3] = 0.0f;
        outEyeToHead.m[2][3] = 0.0f;
        return true;
    }

    inline bool HooksWorldPoseBuildHmdToHeadCalibrationSample(
        const vr_vm_stabilize::Mat3x4& hmdWorld,
        const vr_vm_stabilize::Mat3x4& sourceHead,
        vr_vm_stabilize::Mat3x4& outHmdToHead)
    {
        vr_vm_stabilize::Mat3x4 sourceRigid{};
        vr_vm_stabilize::Mat3x4 inverseHmd{};
        if (!HooksWorldPoseBuildRigidBoneTransform(
                sourceHead,
                sourceRigid))
        {
            return false;
        }

        vr_vm_stabilize::InvertTR(
            hmdWorld,
            inverseHmd);
        vr_vm_stabilize::Mul(
            inverseHmd,
            sourceRigid,
            outHmdToHead);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                outHmdToHead))
        {
            return false;
        }

        // Head position is constrained separately against Source's native
        // head. This matrix only retains the HMD-to-head rotation sample.
        outHmdToHead.m[0][3] = 0.0f;
        outHmdToHead.m[1][3] = 0.0f;
        outHmdToHead.m[2][3] = 0.0f;
        return true;
    }

    inline bool HooksWorldPoseBuildStaticHandTarget(
        const HooksWorldPoseArmLayout& arm,
        const vr_vm_stabilize::Mat3x4& controllerWorld,
        const vr_vm_stabilize::Mat3x4& stableFallbackHand,
        const Vector& rotationOffsetDeg,
        vr_vm_stabilize::Mat3x4& outHandTarget)
    {
        const Vector controllerOrigin =
            vr_vm_stabilize::GetOrigin(controllerWorld);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                controllerOrigin))
        {
            return false;
        }

        if (arm.palmToHandValid)
        {
            // Source's QAngle matrix is [forward,right,up] and has the engine's
            // handedness. Rebuild the controller as the same proper palm frame
            // used to derive the bind reference: X=forward, Y=up,
            // Z=forward x up. Multiplying the immutable bind palm->hand
            // rotation then gives one wrist convention for every animation and
            // every weapon.
            const Vector controllerForward =
                HooksViewmodelAutoGripMatrixAxis(
                    controllerWorld,
                    0);
            const Vector controllerUp =
                HooksViewmodelAutoGripMatrixAxis(
                    controllerWorld,
                    2);
            const Vector controllerPalmNormal =
                CrossProduct(controllerForward, controllerUp);
            vr_vm_stabilize::Mat3x4 controllerPalm{};
            if (!HooksViewmodelAutoGripBuildRigidMatrix(
                    controllerOrigin,
                    controllerForward,
                    controllerUp,
                    controllerPalmNormal,
                    controllerPalm))
            {
                return false;
            }
            vr_vm_stabilize::Mul(
                controllerPalm,
                arm.palmToHand,
                outHandTarget);
        }
        else
        {
            // Some workshop skeletons do not expose four finger roots. They
            // still receive deterministic positional IK, but use the current
            // torso/clavicle with a rest-pose wrist basis. Never take wrist
            // orientation from a walk, fire, melee, reload or deploy frame.
            if (!HooksWorldPoseBuildRigidBoneTransform(
                    stableFallbackHand,
                    outHandTarget))
            {
                return false;
            }
            outHandTarget.m[0][3] = controllerOrigin.x;
            outHandTarget.m[1][3] = controllerOrigin.y;
            outHandTarget.m[2][3] = controllerOrigin.z;
        }

        // Apply user wrist tuning strictly as a local orientation. Preserve
        // the tracked controller origin explicitly so rotating either palm
        // cannot feed a false positional target into the elbow IK.
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                rotationOffsetDeg))
        {
            return false;
        }
        vr_vm_stabilize::Mat3x4 localRotation{};
        vr_vm_stabilize::Mat3x4 rotatedTarget{};
        vr_vm_stabilize::BuildFromOrgAngles(
            Vector(0.0f, 0.0f, 0.0f),
            QAngle(
                rotationOffsetDeg.x,
                rotationOffsetDeg.y,
                rotationOffsetDeg.z),
            localRotation);
        vr_vm_stabilize::Mul(
            outHandTarget,
            localRotation,
            rotatedTarget);
        rotatedTarget.m[0][3] = controllerOrigin.x;
        rotatedTarget.m[1][3] = controllerOrigin.y;
        rotatedTarget.m[2][3] = controllerOrigin.z;
        outHandTarget = rotatedTarget;

        return HooksNativeViewmodelHandsOnlyMatrixFinite(
            outHandTarget);
    }

    inline ozz::math::Float4x4 HooksWorldPoseToOzzMatrix(
        const vr_vm_stabilize::Mat3x4& source);
    inline bool HooksWorldPoseOzzCorrectionToMatrix(
        const ozz::math::SimdQuaternion& correction,
        vr_vm_stabilize::Mat3x4& out);

    inline bool HooksWorldPoseApplyShotWristRotation(
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int handBone,
        bool shotTriggered,
        std::uint64_t now,
        std::uint64_t stereoSceneGeneration,
        bool& previousLocalValid,
        vr_vm_stabilize::Mat3x4& previousLocal,
        std::uint64_t& lastStereoSceneGeneration,
        bool& shotBaselineValid,
        vr_vm_stabilize::Mat3x4& shotBaselineLocal,
        bool& shotAxisHemisphereValid,
        Vector& shotAxisHemisphere,
        Vector& shotImpulseAxis,
        float& shotImpulseAngleRad,
        std::uint64_t& shotCaptureUntilTickMs,
        std::uint64_t& impulseTickMs,
        vr_vm_stabilize::Mat3x4& inOutHandTarget)
    {
        if (!sourceBones ||
            handBone < 0 ||
            handBone >= layout.numBones ||
            static_cast<int>(layout.parents.size()) <
                layout.numBones)
        {
            return false;
        }

        const int parent =
            layout.parents[static_cast<size_t>(handBone)];
        if (parent < 0 ||
            parent >= layout.numBones ||
            parent == handBone)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 sourceHand{};
        vr_vm_stabilize::Mat3x4 sourceParent{};
        vr_vm_stabilize::Mat3x4 inverseSourceParent{};
        vr_vm_stabilize::Mat3x4 animatedLocal{};
        vr_vm_stabilize::Mat3x4 animatedRigid{};
        if (!vr_vm_stabilize::SafeRead(
                sourceBones + handBone,
                sourceHand) ||
            !vr_vm_stabilize::SafeRead(
                sourceBones + parent,
                sourceParent) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                sourceHand) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                sourceParent) ||
            !vr_vm_stabilize::InvertAffine(
                sourceParent,
                inverseSourceParent))
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            inverseSourceParent,
            sourceHand,
            animatedLocal);
        if (!HooksViewmodelAutoGripNormalizeRigidMatrix(
                animatedLocal,
                animatedRigid))
        {
            return false;
        }

        const bool newAnimationSample =
            stereoSceneGeneration != 0u &&
            stereoSceneGeneration != lastStereoSceneGeneration;
        if (shotTriggered)
        {
            shotBaselineLocal =
                previousLocalValid
                    ? previousLocal
                    : animatedRigid;
            shotBaselineValid = true;
            shotAxisHemisphereValid = false;
            shotImpulseAxis = Vector{};
            shotImpulseAngleRad = 0.0f;
            shotCaptureUntilTickMs = now + 100u;
            impulseTickMs = now;
        }

        if (newAnimationSample)
        {
            lastStereoSceneGeneration = stereoSceneGeneration;
            if (shotBaselineValid &&
                now <= shotCaptureUntilTickMs)
            {
                vr_vm_stabilize::Mat3x4 inverseBaseline{};
                vr_vm_stabilize::Mat3x4 shotDelta{};
                vr_vm_stabilize::InvertTR(
                    shotBaselineLocal,
                    inverseBaseline);
                vr_vm_stabilize::Mul(
                    inverseBaseline,
                    animatedRigid,
                    shotDelta);

                ozz::math::SimdFloat4 translation{};
                ozz::math::SimdFloat4 quaternion{};
                ozz::math::SimdFloat4 scale{};
                if (HooksNativeViewmodelHandsOnlyMatrixFinite(
                        shotDelta) &&
                    ozz::math::ToAffine(
                        HooksWorldPoseToOzzMatrix(shotDelta),
                        &translation,
                        &quaternion,
                        &scale))
                {
                    float quaternionValues[4]{};
                    ozz::math::StorePtrU(
                        quaternion,
                        quaternionValues);
                    bool quaternionFinite = true;
                    for (float component : quaternionValues)
                        quaternionFinite =
                            quaternionFinite &&
                            std::isfinite(component);
                    if (quaternionFinite)
                    {
                        if (quaternionValues[3] < 0.0f)
                        {
                            for (float& component : quaternionValues)
                                component = -component;
                        }
                        const ozz::math::SimdQuaternion shotRotation = {
                            ozz::math::simd_float4::Load(
                                quaternionValues[0],
                                quaternionValues[1],
                                quaternionValues[2],
                                quaternionValues[3]) };
                        float axisAngle[4]{};
                        ozz::math::StorePtrU(
                            ozz::math::ToAxisAngle(shotRotation),
                            axisAngle);
                        Vector candidateAxis(
                            axisAngle[0],
                            axisAngle[1],
                            axisAngle[2]);
                        float candidateAngle = axisAngle[3];
                        constexpr float kMinimumShotAngleRad =
                            0.5f * 3.14159265358979323846f / 180.0f;
                        constexpr float kMaximumShotAngleRad =
                            35.0f * 3.14159265358979323846f / 180.0f;
                        if (HooksNativeViewmodelHandsOnlyVectorFinite(
                                candidateAxis) &&
                            std::isfinite(candidateAngle) &&
                            candidateAngle >= kMinimumShotAngleRad &&
                            VectorNormalize(candidateAxis) != 0.0f)
                        {
                            if (!shotAxisHemisphereValid)
                            {
                                shotAxisHemisphere = candidateAxis;
                                shotAxisHemisphereValid = true;
                            }
                            if (DotProduct(
                                    candidateAxis,
                                    shotAxisHemisphere) >= 0.0f &&
                                candidateAngle > shotImpulseAngleRad)
                            {
                                shotImpulseAxis = candidateAxis;
                                shotImpulseAngleRad = std::min(
                                    candidateAngle,
                                    kMaximumShotAngleRad);
                                impulseTickMs = now;
                            }
                        }
                    }
                }
            }

            // This reference always advances, even while the shot baseline is
            // temporarily frozen. Walk/run/deploy animation therefore cannot
            // become the next shot's recoil baseline.
            previousLocal = animatedRigid;
            previousLocalValid = true;
        }

        constexpr std::uint64_t kShotDecayMs = 160u;
        if (!std::isfinite(shotImpulseAngleRad) ||
            shotImpulseAngleRad <= 0.0001f ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(
                shotImpulseAxis) ||
            impulseTickMs == 0u ||
            now < impulseTickMs ||
            (now > shotCaptureUntilTickMs &&
             now - shotCaptureUntilTickMs >= kShotDecayMs))
        {
            return false;
        }

        float impulseWeight = 1.0f;
        if (now > shotCaptureUntilTickMs)
        {
            impulseWeight =
                1.0f -
                static_cast<float>(
                    now - shotCaptureUntilTickMs) /
                    static_cast<float>(kShotDecayMs);
        }
        impulseWeight = std::clamp(impulseWeight, 0.0f, 1.0f);
        // Smoothly return to the tracked wrist without a velocity step.
        impulseWeight =
            impulseWeight * impulseWeight *
            (3.0f - 2.0f * impulseWeight);
        vr_vm_stabilize::Mat3x4 constrainedDelta{};
        vr_vm_stabilize::Mat3x4 animatedTarget{};
        const ozz::math::SimdQuaternion appliedRotation =
            ozz::math::SimdQuaternion::FromAxisAngle(
                ozz::math::simd_float4::Load(
                    shotImpulseAxis.x,
                    shotImpulseAxis.y,
                    shotImpulseAxis.z,
                    0.0f),
                ozz::math::simd_float4::Load1(
                    shotImpulseAngleRad * impulseWeight));
        if (!HooksWorldPoseOzzCorrectionToMatrix(
                appliedRotation,
                constrainedDelta))
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            inOutHandTarget,
            constrainedDelta,
            animatedTarget);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                animatedTarget))
        {
            return false;
        }
        inOutHandTarget = animatedTarget;
        return true;
    }


    inline bool HooksWorldPoseApplyDeltaToBranch(
        const HooksWorldPoseBoneLayout& layout,
        int rootBone,
        const vr_vm_stabilize::Mat3x4& delta,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones ||
            rootBone < 0 ||
            rootBone >= layout.numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(delta))
        {
            return false;
        }

        bool applied = false;
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    rootBone,
                    layout.numBones))
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 transformed{};
            vr_vm_stabilize::Mul(
                delta,
                bones[bone],
                transformed);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    transformed))
            {
                return false;
            }
            bones[bone] = transformed;
            applied = true;
        }
        return applied;
    }

    inline bool HooksWorldPoseOrientBranch(
        const HooksWorldPoseBoneLayout& layout,
        int rootBone,
        const vr_vm_stabilize::Mat3x4& target,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones ||
            rootBone < 0 ||
            rootBone >= layout.numBones)
        {
            return false;
        }

        QAngle currentAngles{};
        QAngle targetAngles{};
        if (!HooksViewmodelAutoGripMatrixAngles(
                bones[rootBone],
                currentAngles) ||
            !HooksViewmodelAutoGripMatrixAngles(
                target,
                targetAngles))
        {
            return false;
        }

        const QAngle blendedAngles =
            HooksWorldPoseLerpAngles(
                currentAngles,
                targetAngles,
                weight);
        const Vector pivot =
            vr_vm_stabilize::GetOrigin(bones[rootBone]);
        vr_vm_stabilize::Mat3x4 currentRigid{};
        vr_vm_stabilize::Mat3x4 blendedRigid{};
        vr_vm_stabilize::Mat3x4 inverseCurrent{};
        vr_vm_stabilize::Mat3x4 delta{};
        vr_vm_stabilize::BuildFromOrgAngles(
            pivot,
            currentAngles,
            currentRigid);
        vr_vm_stabilize::BuildFromOrgAngles(
            pivot,
            blendedAngles,
            blendedRigid);
        vr_vm_stabilize::InvertTR(
            currentRigid,
            inverseCurrent);
        vr_vm_stabilize::Mul(
            blendedRigid,
            inverseCurrent,
            delta);
        return HooksWorldPoseApplyDeltaToBranch(
            layout,
            rootBone,
            delta,
            bones);
    }

    inline bool HooksWorldPoseShareChildOrientationWithParent(
        const HooksWorldPoseBoneLayout& layout,
        int parentBone,
        int childBone,
        const vr_vm_stabilize::Mat3x4& childTarget,
        float parentShare,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones ||
            parentBone < 0 ||
            parentBone >= layout.numBones ||
            childBone < 0 ||
            childBone >= layout.numBones)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 currentRigid{};
        vr_vm_stabilize::Mat3x4 targetRigid{};
        vr_vm_stabilize::Mat3x4 inverseCurrent{};
        vr_vm_stabilize::Mat3x4 relativeRotation{};
        if (!HooksWorldPoseBuildRigidBoneTransform(
                bones[childBone],
                currentRigid) ||
            !HooksWorldPoseBuildRigidBoneTransform(
                childTarget,
                targetRigid))
        {
            return false;
        }
        currentRigid.m[0][3] = 0.0f;
        currentRigid.m[1][3] = 0.0f;
        currentRigid.m[2][3] = 0.0f;
        targetRigid.m[0][3] = 0.0f;
        targetRigid.m[1][3] = 0.0f;
        targetRigid.m[2][3] = 0.0f;
        vr_vm_stabilize::InvertTR(
            currentRigid,
            inverseCurrent);
        vr_vm_stabilize::Mul(
            targetRigid,
            inverseCurrent,
            relativeRotation);

        // Extract the relative quaternion, then keep only its twist around
        // world Z. This remains a pure yaw even for custom models whose head
        // bone axes are unusual or close to an Euler singularity.
        const float m00 = relativeRotation.m[0][0];
        const float m11 = relativeRotation.m[1][1];
        const float m22 = relativeRotation.m[2][2];
        float quaternionW = 1.0f;
        float quaternionX = 0.0f;
        float quaternionY = 0.0f;
        float quaternionZ = 0.0f;
        const float trace = m00 + m11 + m22;
        if (trace > 0.0f)
        {
            const float scale =
                std::sqrt(std::max(0.0f, trace + 1.0f)) *
                2.0f;
            if (scale <= 0.000001f)
                return false;
            quaternionW = 0.25f * scale;
            quaternionX =
                (relativeRotation.m[2][1] -
                 relativeRotation.m[1][2]) /
                scale;
            quaternionY =
                (relativeRotation.m[0][2] -
                 relativeRotation.m[2][0]) /
                scale;
            quaternionZ =
                (relativeRotation.m[1][0] -
                 relativeRotation.m[0][1]) /
                scale;
        }
        else if (m00 > m11 && m00 > m22)
        {
            const float scale =
                std::sqrt(std::max(
                    0.0f,
                    1.0f + m00 - m11 - m22)) *
                2.0f;
            if (scale <= 0.000001f)
                return false;
            quaternionW =
                (relativeRotation.m[2][1] -
                 relativeRotation.m[1][2]) /
                scale;
            quaternionX = 0.25f * scale;
            quaternionY =
                (relativeRotation.m[0][1] +
                 relativeRotation.m[1][0]) /
                scale;
            quaternionZ =
                (relativeRotation.m[0][2] +
                 relativeRotation.m[2][0]) /
                scale;
        }
        else if (m11 > m22)
        {
            const float scale =
                std::sqrt(std::max(
                    0.0f,
                    1.0f + m11 - m00 - m22)) *
                2.0f;
            if (scale <= 0.000001f)
                return false;
            quaternionW =
                (relativeRotation.m[0][2] -
                 relativeRotation.m[2][0]) /
                scale;
            quaternionX =
                (relativeRotation.m[0][1] +
                 relativeRotation.m[1][0]) /
                scale;
            quaternionY = 0.25f * scale;
            quaternionZ =
                (relativeRotation.m[1][2] +
                 relativeRotation.m[2][1]) /
                scale;
        }
        else
        {
            const float scale =
                std::sqrt(std::max(
                    0.0f,
                    1.0f + m22 - m00 - m11)) *
                2.0f;
            if (scale <= 0.000001f)
                return false;
            quaternionW =
                (relativeRotation.m[1][0] -
                 relativeRotation.m[0][1]) /
                scale;
            quaternionX =
                (relativeRotation.m[0][2] +
                 relativeRotation.m[2][0]) /
                scale;
            quaternionY =
                (relativeRotation.m[1][2] +
                 relativeRotation.m[2][1]) /
                scale;
            quaternionZ = 0.25f * scale;
        }
        if (!std::isfinite(quaternionW) ||
            !std::isfinite(quaternionX) ||
            !std::isfinite(quaternionY) ||
            !std::isfinite(quaternionZ))
        {
            return false;
        }
        const float twistLength =
            std::sqrt(
                quaternionW * quaternionW +
                quaternionZ * quaternionZ);
        if (!std::isfinite(twistLength) ||
            twistLength <= 0.000001f)
        {
            return false;
        }
        quaternionW /= twistLength;
        quaternionZ /= twistLength;
        constexpr float kRadiansToDegrees =
            180.0f / 3.14159265358979323846f;
        const float relativeYaw =
            HooksWorldPoseWrapAngle(
                2.0f *
                std::atan2(
                    quaternionZ,
                    quaternionW) *
                kRadiansToDegrees);
        const float sharedYawDelta =
            std::clamp(
                relativeYaw *
                    std::clamp(parentShare, 0.0f, 1.0f),
                -20.0f,
                20.0f);
        if (!std::isfinite(sharedYawDelta) ||
            std::fabs(sharedYawDelta) <= 0.0001f)
        {
            return false;
        }

        const Vector pivot =
            vr_vm_stabilize::GetOrigin(
                bones[parentBone]);
        constexpr float kDegreesToRadians =
            3.14159265358979323846f / 180.0f;
        const float angle =
            sharedYawDelta * kDegreesToRadians;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        vr_vm_stabilize::Mat3x4 delta =
            vr_vm_stabilize::Identity();
        delta.m[0][0] = cosine;
        delta.m[0][1] = -sine;
        delta.m[1][0] = sine;
        delta.m[1][1] = cosine;
        delta.m[0][3] =
            pivot.x -
            (cosine * pivot.x -
             sine * pivot.y);
        delta.m[1][3] =
            pivot.y -
            (sine * pivot.x +
             cosine * pivot.y);
        return HooksWorldPoseApplyDeltaToBranch(
            layout,
            parentBone,
            delta,
            bones);
    }

    inline bool HooksWorldPoseCaptureBoneLocalTransform(
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int bone,
        vr_vm_stabilize::Mat3x4& outLocal)
    {
        if (!sourceBones ||
            bone < 0 ||
            bone >= layout.numBones ||
            static_cast<int>(layout.parents.size()) <
                layout.numBones)
        {
            return false;
        }
        const int parent =
            layout.parents[static_cast<size_t>(bone)];
        if (parent < 0 ||
            parent >= layout.numBones ||
            parent == bone)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 parentWorld{};
        vr_vm_stabilize::Mat3x4 boneWorld{};
        vr_vm_stabilize::Mat3x4 inverseParent{};
        if (!vr_vm_stabilize::SafeRead(
                sourceBones + parent,
                parentWorld) ||
            !vr_vm_stabilize::SafeRead(
                sourceBones + bone,
                boneWorld) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                parentWorld) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                boneWorld) ||
            !vr_vm_stabilize::InvertAffine(
                parentWorld,
                inverseParent))
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            inverseParent,
            boneWorld,
            outLocal);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(
            outLocal);
    }

    inline bool HooksWorldPoseRestoreBoneLocalTransform(
        const HooksWorldPoseBoneLayout& layout,
        int bone,
        const vr_vm_stabilize::Mat3x4& referenceLocal,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones ||
            bone < 0 ||
            bone >= layout.numBones ||
            static_cast<int>(layout.parents.size()) <
                layout.numBones)
        {
            return false;
        }
        const int parent =
            layout.parents[static_cast<size_t>(bone)];
        if (parent < 0 ||
            parent >= layout.numBones ||
            parent == bone)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 stableWorld{};
        vr_vm_stabilize::Mat3x4 inverseCurrent{};
        vr_vm_stabilize::Mat3x4 delta{};
        vr_vm_stabilize::Mul(
            bones[parent],
            referenceLocal,
            stableWorld);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                stableWorld) ||
            !vr_vm_stabilize::InvertAffine(
                bones[bone],
                inverseCurrent))
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            stableWorld,
            inverseCurrent,
            delta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                delta))
        {
            return false;
        }
        return HooksWorldPoseApplyDeltaToBranch(
            layout,
            bone,
            delta,
            bones);
    }

    inline bool HooksWorldPoseResetArmChainToRestPose(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones || !arm.restChainValid ||
            arm.restChainBones.empty() ||
            arm.restChainBones.size() != arm.restChainLocals.size() ||
            arm.restChainBones.back() != arm.hand)
        {
            return false;
        }

        const int expectedRoot =
            (arm.clavicle >= 0 && arm.clavicle < layout.numBones &&
             HooksNativeViewmodelHandsOnlyIsAncestor(
                 layout.parents,
                 arm.upperArm,
                 arm.clavicle,
                 layout.numBones))
                ? arm.clavicle
                : arm.upperArm;
        if (arm.restChainBones.front() != expectedRoot)
            return false;

        // Rebuild the complete same-side shoulder root through hand. When a
        // valid clavicle exists it is restored relative to the current torso,
        // removing fire/reload/melee shoulder animation before IK. Intermediate
        // twist/helper bones are restored as well. The torso and everything
        // above the clavicle remain native.
        for (size_t index = 0; index < arm.restChainBones.size(); ++index)
        {
            const int bone = arm.restChainBones[index];
            if (!HooksWorldPoseRestoreBoneLocalTransform(
                    layout,
                    bone,
                    arm.restChainLocals[index],
                    bones))
            {
                return false;
            }
        }
        return true;
    }

    inline bool HooksWorldPoseBuildRestHandReference(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        vr_vm_stabilize::Mat3x4& outRestHand)
    {
        if (!sourceBones ||
            !HooksWorldPoseArmChainValid(
                arm,
                layout.parents,
                layout.numBones) ||
            !arm.restChainValid ||
            arm.restChainBones.empty() ||
            arm.restChainBones.size() != arm.restChainLocals.size())
        {
            return false;
        }

        const int restRoot = arm.restChainBones.front();
        if (restRoot < 0 || restRoot >= layout.numBones)
            return false;
        const int rootParent =
            layout.parents[static_cast<size_t>(restRoot)];
        if (rootParent < 0 ||
            rootParent >= layout.numBones ||
            rootParent == restRoot ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                sourceBones[rootParent]))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 stableWorld = sourceBones[rootParent];
        for (size_t index = 0; index < arm.restChainBones.size(); ++index)
        {
            if (index > 0u &&
                layout.parents[static_cast<size_t>(
                    arm.restChainBones[index])] !=
                    arm.restChainBones[index - 1u])
            {
                return false;
            }

            vr_vm_stabilize::Mat3x4 nextWorld{};
            vr_vm_stabilize::Mul(
                stableWorld,
                arm.restChainLocals[index],
                nextWorld);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(nextWorld))
                return false;
            stableWorld = nextWorld;
        }

        return HooksWorldPoseBuildRigidBoneTransform(
            stableWorld,
            outRestHand);
    }

    inline bool HooksWorldPoseBlendLocalTransform(
        const vr_vm_stabilize::Mat3x4& baseLocal,
        const vr_vm_stabilize::Mat3x4& solvedLocal,
        float weight,
        vr_vm_stabilize::Mat3x4& outLocal);

    inline bool HooksWorldPoseEnsureAnimationIndependentFingerBase(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        const HooksWorldPoseFingerLayout& fingers,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        bool useRestPose,
        const void* poseKey,
        HooksWorldPoseFingerPoseCache& cache)
    {
        if (!sourceBones ||
            arm.hand < 0 || arm.hand >= layout.numBones ||
            static_cast<int>(layout.parents.size()) < layout.numBones)
        {
            return false;
        }

        if (cache.valid &&
            cache.useRestPose == useRestPose &&
            cache.poseKey == poseKey)
        {
            return true;
        }

        cache.Reset();
        cache.useRestPose = useRestPose;
        cache.poseKey = poseKey;
        if (fingers.mappedSegments <= 0)
        {
            cache.valid = true;
            return true;
        }

        int captured = 0;
        for (int slot = 0; slot < 15; ++slot)
        {
            const int bone = fingers.bones[static_cast<size_t>(slot)];
            if (bone < 0 || bone >= layout.numBones ||
                !HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    arm.hand,
                    layout.numBones))
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 local{};
            bool localReady = false;
            if (useRestPose)
            {
                if (fingers.restLocalValid[static_cast<size_t>(slot)])
                {
                    local = fingers.restLocals[static_cast<size_t>(slot)];
                    localReady =
                        HooksNativeViewmodelHandsOnlyMatrixFinite(local);
                }
            }
            else
            {
                // Equipped hands deliberately capture one authored grip for
                // the current weapon key. The cache prevents later Source
                // animation frames from changing any finger local transform.
                localReady = HooksWorldPoseCaptureBoneLocalTransform(
                    layout,
                    sourceBones,
                    bone,
                    local);
            }
            if (!localReady)
            {
                cache.Reset();
                return false;
            }

            cache.locals[static_cast<size_t>(slot)] = local;
            cache.localValid[static_cast<size_t>(slot)] = 1u;
            ++captured;
        }

        cache.valid = captured == fingers.mappedSegments;
        if (!cache.valid)
            cache.Reset();
        return cache.valid;
    }

    inline bool HooksWorldPoseApplyAnimationIndependentFingerBase(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        const HooksWorldPoseFingerLayout& fingers,
        const HooksWorldPoseFingerPoseCache& cache,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones || !cache.valid ||
            arm.hand < 0 || arm.hand >= layout.numBones ||
            static_cast<int>(layout.parents.size()) < layout.numBones)
        {
            return false;
        }

        int expected = 0;
        for (uint8_t valid : cache.localValid)
        {
            if (valid)
                ++expected;
        }
        if (expected <= 0)
            return true;

        int applied = 0;
        for (int finger = 0; finger < 5; ++finger)
        {
            for (int segment = 0; segment < 3; ++segment)
            {
                const int slot = finger * 3 + segment;
                const int bone = fingers.bones[static_cast<size_t>(slot)];
                if (bone < 0 || bone >= layout.numBones ||
                    !cache.localValid[static_cast<size_t>(slot)])
                {
                    continue;
                }

                const int parent = layout.parents[static_cast<size_t>(bone)];
                if (parent < 0 || parent >= layout.numBones || parent == bone)
                    continue;

                vr_vm_stabilize::Mat3x4 world{};
                vr_vm_stabilize::Mul(
                    bones[parent],
                    cache.locals[static_cast<size_t>(slot)],
                    world);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(world))
                    return false;

                bones[bone] = world;
                ++applied;
            }
        }
        return applied == expected;
    }

    __declspec(noinline) bool HooksWorldPoseApplyTrackedFingerPoseFromCurls(
        VR* vr,
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        const std::array<float, 5>& curls,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        const HooksWorldPoseFingerLayout& fingers =
            side < 0 ? layout.leftFingers : layout.rightFingers;
        weight = std::clamp(weight, 0.0f, 1.0f);
        if (!vr || !bones || weight <= 0.0001f ||
            (side != -1 && side != 1) ||
            arm.hand < 0 || arm.hand >= layout.numBones ||
            fingers.mappedSegments <= 0 ||
            static_cast<int>(layout.parents.size()) <
                layout.numBones)
        {
            return false;
        }
        for (float curl : curls)
        {
            if (!std::isfinite(curl) ||
                curl < 0.0f ||
                curl > l4d2vr_pose::kFingerCurlMaximum + 0.001f)
            {
                return false;
            }
        }

        const float strength = std::clamp(
            vr->m_NativeViewmodelLeftHandOpenVRCurlStrength,
            0.0f,
            2.0f);
        const float direction = std::clamp(
            vr->m_NativeViewmodelLeftHandOpenVRCurlDirection,
            -1.0f,
            1.0f);
        if (strength <= 0.0001f ||
            std::fabs(direction) <= 0.0001f)
        {
            return false;
        }

        static const float kMaxCurlRadians[5][3] =
        {
            { 0.75f, 0.90f, 0.65f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
        };

        int applied = 0;
        for (int finger = 0; finger < 5; ++finger)
        {
            for (int segment = 0; segment < 3; ++segment)
            {
                const int slot = finger * 3 + segment;
                const int bone =
                    fingers.bones[static_cast<size_t>(slot)];
                if (bone < 0 || bone >= layout.numBones ||
                    !fingers.restLocalValid[static_cast<size_t>(slot)])
                {
                    continue;
                }
                const int parent =
                    layout.parents[static_cast<size_t>(bone)];
                if (parent < 0 || parent >= layout.numBones)
                    continue;

                vr_vm_stabilize::Mat3x4 local =
                    fingers.restLocals[static_cast<size_t>(slot)];
                const bool thumbRoot =
                    finger == 0 && segment == 0;
                if (thumbRoot)
                {
                    local =
                        HooksNativeViewmodelHandsOnlyApplyThumbRootAdjust(
                            vr,
                            side,
                            local);
                }
                else
                {
                    const float radians =
                        curls[static_cast<size_t>(finger)] *
                        kMaxCurlRadians[finger][segment] *
                        strength * direction * weight;
                    const vr_vm_stabilize::Mat3x4 rotation =
                        HooksNativeViewmodelHandsOnlyMakeLocalAxisRotation(
                            vr->m_NativeViewmodelLeftHandOpenVRCurlAxis,
                            radians);
                    vr_vm_stabilize::Mat3x4 adjusted{};
                    vr_vm_stabilize::Mul(local, rotation, adjusted);
                    local = adjusted;
                }

                vr_vm_stabilize::Mat3x4 world{};
                vr_vm_stabilize::Mul(bones[parent], local, world);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(world))
                    return false;
                bones[bone] = world;
                ++applied;
            }
        }
        return applied > 0;
    }

    __declspec(noinline) bool HooksWorldPoseApplyLocalTrackedFingerPose(
        VR* vr,
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!vr)
            return false;

        std::array<float, 5> curls{};
        const bool haveCurls = side < 0
            ? HooksNativeViewmodelHandsOnlyReadOpenVRLeftFingerCurls(
                vr,
                curls)
            : HooksNativeViewmodelHandsOnlyReadOpenVRRightFingerCurls(
                vr,
                curls);
        if (!haveCurls)
            return false;

        return HooksWorldPoseApplyTrackedFingerPoseFromCurls(
            vr,
            layout,
            arm,
            side,
            curls,
            weight,
            bones);
    }

#ifdef _MSC_VER
    __declspec(noinline) bool HooksWorldPoseApplyLocalTrackedFingerPoseGuarded(
        VR* vr,
        const HooksWorldPoseBoneLayout* layout,
        const HooksWorldPoseArmLayout* arm,
        int side,
        float weight,
        vr_vm_stabilize::Mat3x4* bones,
        unsigned long& outExceptionCode)
    {
        outExceptionCode = 0ul;
        if (!layout || !arm)
            return false;
        __try
        {
            return HooksWorldPoseApplyLocalTrackedFingerPose(
                vr,
                *layout,
                *arm,
                side,
                weight,
                bones);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outExceptionCode = static_cast<unsigned long>(
                GetExceptionCode());
            return false;
        }
    }
#else
    inline bool HooksWorldPoseApplyLocalTrackedFingerPoseGuarded(
        VR* vr,
        const HooksWorldPoseBoneLayout* layout,
        const HooksWorldPoseArmLayout* arm,
        int side,
        float weight,
        vr_vm_stabilize::Mat3x4* bones,
        unsigned long& outExceptionCode)
    {
        outExceptionCode = 0ul;
        return layout && arm &&
            HooksWorldPoseApplyLocalTrackedFingerPose(
                vr,
                *layout,
                *arm,
                side,
                weight,
                bones);
    }
#endif

    inline bool HooksWorldPoseTryApplyLocalTrackedFingerPose(
        VR* vr,
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (g_HooksWorldPoseFingerRuntimeDisabled.load(
                std::memory_order_acquire))
        {
            return false;
        }
        if (!g_HooksWorldPoseFingerFirstRunLogged.exchange(
                true,
                std::memory_order_acq_rel))
        {
            const int mapped = side < 0
                ? layout.leftFingers.mappedSegments
                : layout.rightFingers.mappedSegments;
            Game::logMsg(
                "[VR][WorldPose] tracked fingers first run begin side=%d mapped=%d",
                side,
                mapped);
        }

        unsigned long exceptionCode = 0ul;
        const bool applied =
            HooksWorldPoseApplyLocalTrackedFingerPoseGuarded(
                vr,
                &layout,
                &arm,
                side,
                weight,
                bones,
                exceptionCode);
        if (exceptionCode != 0ul)
        {
            g_HooksWorldPoseFingerRuntimeDisabled.store(
                true,
                std::memory_order_release);
            bool expected = false;
            if (g_HooksWorldPoseFingerFaultLogged.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel))
            {
                Game::logMsg(
                    "[VR][WorldPose] tracked fingers exception=0x%08lX; finger takeover disabled for this process",
                    exceptionCode);
            }
            return false;
        }
        if (applied &&
            !g_HooksWorldPoseFingerFirstRunCompletedLogged.exchange(
                true,
                std::memory_order_acq_rel))
        {
            Game::logMsg(
                "[VR][WorldPose] tracked fingers first run completed side=%d",
                side);
        }
        return applied;
    }

    inline bool HooksWorldPoseTryApplyNetworkTrackedFingerPose(
        VR* vr,
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        const std::array<float, 5>& curls,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (g_HooksWorldPoseFingerRuntimeDisabled.load(
                std::memory_order_acquire))
        {
            return false;
        }

        bool applied = false;
        unsigned long exceptionCode = 0ul;
#ifdef _MSC_VER
        __try
        {
            applied = HooksWorldPoseApplyTrackedFingerPoseFromCurls(
                vr,
                layout,
                arm,
                side,
                curls,
                weight,
                bones);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            exceptionCode = static_cast<unsigned long>(GetExceptionCode());
        }
#else
        applied = HooksWorldPoseApplyTrackedFingerPoseFromCurls(
            vr,
            layout,
            arm,
            side,
            curls,
            weight,
            bones);
#endif
        if (exceptionCode != 0ul)
        {
            g_HooksWorldPoseFingerRuntimeDisabled.store(
                true,
                std::memory_order_release);
            bool expected = false;
            if (g_HooksWorldPoseFingerFaultLogged.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel))
            {
                Game::logMsg(
                    "[VR][WorldPose] network finger exception=0x%08lX; finger takeover disabled for this process",
                    exceptionCode);
            }
            return false;
        }
        return applied;
    }

    inline bool HooksWorldPoseRestoreHeadChain(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseCalibration& calibration,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!calibration.neckReferenceLocalValid ||
            !calibration.headReferenceLocalValid)
        {
            return false;
        }

        weight = std::clamp(weight, 0.0f, 1.0f);
        bool changed = false;
        const std::array<int, 2> chain{
            layout.neck,
            layout.head,
        };
        const std::array<const vr_vm_stabilize::Mat3x4*, 2> references{
            &calibration.neckReferenceLocal,
            &calibration.headReferenceLocal,
        };
        for (size_t index = 0; index < chain.size(); ++index)
        {
            vr_vm_stabilize::Mat3x4 currentLocal{};
            vr_vm_stabilize::Mat3x4 blendedLocal{};
            if (!HooksWorldPoseCaptureBoneLocalTransform(
                    layout,
                    bones,
                    chain[index],
                    currentLocal) ||
                !HooksWorldPoseBlendLocalTransform(
                    currentLocal,
                    *references[index],
                    weight,
                    blendedLocal))
            {
                continue;
            }
            changed =
                HooksWorldPoseRestoreBoneLocalTransform(
                    layout,
                    chain[index],
                    blendedLocal,
                    bones) ||
                changed;
        }
        return changed;
    }

    inline ozz::math::Float4x4 HooksWorldPoseToOzzMatrix(
        const vr_vm_stabilize::Mat3x4& source)
    {
        ozz::math::Float4x4 result{};
        result.cols[0] = ozz::math::simd_float4::Load(
            source.m[0][0],
            source.m[1][0],
            source.m[2][0],
            0.0f);
        result.cols[1] = ozz::math::simd_float4::Load(
            source.m[0][1],
            source.m[1][1],
            source.m[2][1],
            0.0f);
        result.cols[2] = ozz::math::simd_float4::Load(
            source.m[0][2],
            source.m[1][2],
            source.m[2][2],
            0.0f);
        result.cols[3] = ozz::math::simd_float4::Load(
            source.m[0][3],
            source.m[1][3],
            source.m[2][3],
            1.0f);
        return result;
    }

    inline bool HooksWorldPoseOzzCorrectionToMatrix(
        const ozz::math::SimdQuaternion& correction,
        vr_vm_stabilize::Mat3x4& out)
    {
        float quaternion[4]{};
        ozz::math::StorePtrU(correction.xyzw, quaternion);
        for (float component : quaternion)
        {
            if (!std::isfinite(component))
                return false;
        }

        const ozz::math::Float4x4 rotation =
            ozz::math::Float4x4::FromQuaternion(
                correction.xyzw);
        float column0[4]{};
        float column1[4]{};
        float column2[4]{};
        ozz::math::StorePtrU(rotation.cols[0], column0);
        ozz::math::StorePtrU(rotation.cols[1], column1);
        ozz::math::StorePtrU(rotation.cols[2], column2);

        out = vr_vm_stabilize::Identity();
        out.m[0][0] = column0[0];
        out.m[1][0] = column0[1];
        out.m[2][0] = column0[2];
        out.m[0][1] = column1[0];
        out.m[1][1] = column1[1];
        out.m[2][1] = column1[2];
        out.m[0][2] = column2[0];
        out.m[1][2] = column2[1];
        out.m[2][2] = column2[2];
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
    }

    // Solve tracked world-model arms with the same analytic two-bone path as
    // the first-person viewmodel. Each side is staged from one immutable
    // post-body/head pose and only its isolated descendant mask is published.
    struct HooksWorldPoseQuaternion
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    inline bool HooksWorldPoseNormalizeQuaternion(
        HooksWorldPoseQuaternion& value)
    {
        const float lengthSquared =
            value.x * value.x +
            value.y * value.y +
            value.z * value.z +
            value.w * value.w;
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f)
            return false;
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        value.x *= inverseLength;
        value.y *= inverseLength;
        value.z *= inverseLength;
        value.w *= inverseLength;
        return std::isfinite(value.x) &&
            std::isfinite(value.y) &&
            std::isfinite(value.z) &&
            std::isfinite(value.w);
    }

    inline bool HooksWorldPoseMatrixQuaternion(
        const vr_vm_stabilize::Mat3x4& matrix,
        HooksWorldPoseQuaternion& out)
    {
        vr_vm_stabilize::Mat3x4 rigid{};
        if (!HooksViewmodelAutoGripNormalizeRigidMatrix(matrix, rigid))
            return false;

        const float trace =
            rigid.m[0][0] + rigid.m[1][1] + rigid.m[2][2];
        if (trace > 0.0f)
        {
            const float scale = std::sqrt(trace + 1.0f) * 2.0f;
            if (!std::isfinite(scale) || scale <= 0.000001f)
                return false;
            out.w = 0.25f * scale;
            out.x = (rigid.m[2][1] - rigid.m[1][2]) / scale;
            out.y = (rigid.m[0][2] - rigid.m[2][0]) / scale;
            out.z = (rigid.m[1][0] - rigid.m[0][1]) / scale;
        }
        else if (rigid.m[0][0] > rigid.m[1][1] &&
                 rigid.m[0][0] > rigid.m[2][2])
        {
            const float scale = std::sqrt(
                1.0f + rigid.m[0][0] - rigid.m[1][1] - rigid.m[2][2]) *
                2.0f;
            if (!std::isfinite(scale) || scale <= 0.000001f)
                return false;
            out.w = (rigid.m[2][1] - rigid.m[1][2]) / scale;
            out.x = 0.25f * scale;
            out.y = (rigid.m[0][1] + rigid.m[1][0]) / scale;
            out.z = (rigid.m[0][2] + rigid.m[2][0]) / scale;
        }
        else if (rigid.m[1][1] > rigid.m[2][2])
        {
            const float scale = std::sqrt(
                1.0f + rigid.m[1][1] - rigid.m[0][0] - rigid.m[2][2]) *
                2.0f;
            if (!std::isfinite(scale) || scale <= 0.000001f)
                return false;
            out.w = (rigid.m[0][2] - rigid.m[2][0]) / scale;
            out.x = (rigid.m[0][1] + rigid.m[1][0]) / scale;
            out.y = 0.25f * scale;
            out.z = (rigid.m[1][2] + rigid.m[2][1]) / scale;
        }
        else
        {
            const float scale = std::sqrt(
                1.0f + rigid.m[2][2] - rigid.m[0][0] - rigid.m[1][1]) *
                2.0f;
            if (!std::isfinite(scale) || scale <= 0.000001f)
                return false;
            out.w = (rigid.m[1][0] - rigid.m[0][1]) / scale;
            out.x = (rigid.m[0][2] + rigid.m[2][0]) / scale;
            out.y = (rigid.m[1][2] + rigid.m[2][1]) / scale;
            out.z = 0.25f * scale;
        }
        return HooksWorldPoseNormalizeQuaternion(out);
    }

    inline bool HooksWorldPoseSlerpQuaternion(
        const HooksWorldPoseQuaternion& from,
        HooksWorldPoseQuaternion to,
        float weight,
        HooksWorldPoseQuaternion& out)
    {
        weight = std::clamp(weight, 0.0f, 1.0f);
        float dot =
            from.x * to.x +
            from.y * to.y +
            from.z * to.z +
            from.w * to.w;
        if (!std::isfinite(dot))
            return false;
        if (dot < 0.0f)
        {
            dot = -dot;
            to.x = -to.x;
            to.y = -to.y;
            to.z = -to.z;
            to.w = -to.w;
        }
        dot = std::clamp(dot, 0.0f, 1.0f);

        float fromScale = 1.0f - weight;
        float toScale = weight;
        if (dot < 0.9995f)
        {
            const float angle = std::acos(dot);
            const float sine = std::sin(angle);
            if (std::isfinite(angle) &&
                std::isfinite(sine) &&
                std::fabs(sine) > 0.000001f)
            {
                fromScale = std::sin((1.0f - weight) * angle) / sine;
                toScale = std::sin(weight * angle) / sine;
            }
        }
        out.x = from.x * fromScale + to.x * toScale;
        out.y = from.y * fromScale + to.y * toScale;
        out.z = from.z * fromScale + to.z * toScale;
        out.w = from.w * fromScale + to.w * toScale;
        return HooksWorldPoseNormalizeQuaternion(out);
    }

    inline vr_vm_stabilize::Mat3x4 HooksWorldPoseQuaternionMatrix(
        const HooksWorldPoseQuaternion& value,
        const Vector& origin)
    {
        vr_vm_stabilize::Mat3x4 out = vr_vm_stabilize::Identity();
        const float xx = value.x * value.x;
        const float yy = value.y * value.y;
        const float zz = value.z * value.z;
        const float xy = value.x * value.y;
        const float xz = value.x * value.z;
        const float yz = value.y * value.z;
        const float wx = value.w * value.x;
        const float wy = value.w * value.y;
        const float wz = value.w * value.z;
        out.m[0][0] = 1.0f - 2.0f * (yy + zz);
        out.m[0][1] = 2.0f * (xy - wz);
        out.m[0][2] = 2.0f * (xz + wy);
        out.m[1][0] = 2.0f * (xy + wz);
        out.m[1][1] = 1.0f - 2.0f * (xx + zz);
        out.m[1][2] = 2.0f * (yz - wx);
        out.m[2][0] = 2.0f * (xz - wy);
        out.m[2][1] = 2.0f * (yz + wx);
        out.m[2][2] = 1.0f - 2.0f * (xx + yy);
        out.m[0][3] = origin.x;
        out.m[1][3] = origin.y;
        out.m[2][3] = origin.z;
        return out;
    }

    inline bool HooksWorldPoseAccumulateStableTransformSample(
        const vr_vm_stabilize::Mat3x4& sample,
        HooksWorldPoseStableTransformSamples& samples)
    {
        HooksWorldPoseQuaternion rotation{};
        const Vector origin = vr_vm_stabilize::GetOrigin(sample);
        if (!HooksWorldPoseMatrixQuaternion(sample, rotation) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(origin))
        {
            return false;
        }

        if (!samples.hemisphereValid)
        {
            samples.hemisphereX = rotation.x;
            samples.hemisphereY = rotation.y;
            samples.hemisphereZ = rotation.z;
            samples.hemisphereW = rotation.w;
            samples.hemisphereValid = true;
        }
        const float hemisphereDot =
            rotation.x * samples.hemisphereX +
            rotation.y * samples.hemisphereY +
            rotation.z * samples.hemisphereZ +
            rotation.w * samples.hemisphereW;
        if (!std::isfinite(hemisphereDot))
            return false;
        if (hemisphereDot < 0.0f)
        {
            rotation.x = -rotation.x;
            rotation.y = -rotation.y;
            rotation.z = -rotation.z;
            rotation.w = -rotation.w;
        }

        samples.sumX += rotation.x;
        samples.sumY += rotation.y;
        samples.sumZ += rotation.z;
        samples.sumW += rotation.w;
        samples.originSum += origin;
        ++samples.count;
        return std::isfinite(samples.sumX) &&
            std::isfinite(samples.sumY) &&
            std::isfinite(samples.sumZ) &&
            std::isfinite(samples.sumW) &&
            HooksNativeViewmodelHandsOnlyVectorFinite(samples.originSum);
    }

    inline bool HooksWorldPoseFinalizeStableTransformSamples(
        const HooksWorldPoseStableTransformSamples& samples,
        vr_vm_stabilize::Mat3x4& out)
    {
        if (samples.count == 0u)
            return false;
        HooksWorldPoseQuaternion averageRotation{};
        averageRotation.x = samples.sumX;
        averageRotation.y = samples.sumY;
        averageRotation.z = samples.sumZ;
        averageRotation.w = samples.sumW;
        if (!HooksWorldPoseNormalizeQuaternion(averageRotation))
            return false;

        const float inverseCount =
            1.0f / static_cast<float>(samples.count);
        const Vector averageOrigin =
            samples.originSum * inverseCount;
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(averageOrigin))
            return false;
        out = HooksWorldPoseQuaternionMatrix(
            averageRotation,
            averageOrigin);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
    }

    inline bool HooksWorldPoseUpdateStableHeadCalibration(
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const vr_vm_stabilize::Mat3x4& hmdWorld,
        bool sourceEyeAnglesValid,
        const QAngle& sourceEyeAngles,
        std::uint64_t stereoSceneGeneration,
        std::uint64_t now,
        HooksWorldPoseCalibration& calibration)
    {
        // Do not substitute raw mstudiobone bind rotations here. Workshop
        // survivor replacements can retarget the runtime head basis, and
        // applying their raw studio rotation directly can reverse the head.
        // Averaging the already-retargeted runtime basis removes transient
        // idle/fire/transition tilt without losing custom-model conventions.
        if (calibration.headCalibrationComplete)
        {
            return calibration.hmdToHeadValid &&
                calibration.neckReferenceLocalValid &&
                calibration.headReferenceLocalValid;
        }
        if (!sourceBones ||
            layout.neck < 0 ||
            layout.neck >= layout.numBones ||
            layout.head < 0 ||
            layout.head >= layout.numBones ||
            stereoSceneGeneration == 0u)
        {
            return false;
        }
        if (calibration.headCalibrationLastStereoSceneGeneration ==
                stereoSceneGeneration)
        {
            return calibration.hmdToHeadValid &&
                calibration.neckReferenceLocalValid &&
                calibration.headReferenceLocalValid;
        }

        // Sample at a bounded rate so the averaging window represents time,
        // rather than the user's render refresh rate. If tracking disappears
        // during initial calibration, discard the incomplete window instead
        // of combining poses from two unrelated transitions.
        constexpr std::uint64_t kHeadCalibrationSampleIntervalMs = 33u;
        constexpr std::uint64_t kHeadCalibrationGapResetMs = 500u;
        if (calibration.headCalibrationLastSampleTickMs != 0u &&
            now > calibration.headCalibrationLastSampleTickMs +
                kHeadCalibrationGapResetMs)
        {
            calibration.ResetHeadCalibration();
        }
        if (calibration.headCalibrationLastSampleTickMs != 0u &&
            now < calibration.headCalibrationLastSampleTickMs +
                kHeadCalibrationSampleIntervalMs)
        {
            calibration.headCalibrationLastStereoSceneGeneration =
                stereoSceneGeneration;
            return calibration.hmdToHeadValid &&
                calibration.neckReferenceLocalValid &&
                calibration.headReferenceLocalValid;
        }

        vr_vm_stabilize::Mat3x4 neckLocal{};
        vr_vm_stabilize::Mat3x4 headLocal{};
        vr_vm_stabilize::Mat3x4 hmdToHeadSample{};
        if (!HooksWorldPoseCaptureBoneLocalTransform(
                layout,
                sourceBones,
                layout.neck,
                neckLocal) ||
            !HooksWorldPoseCaptureBoneLocalTransform(
                layout,
                sourceBones,
                layout.head,
                headLocal))
        {
            return calibration.hmdToHeadValid &&
                calibration.neckReferenceLocalValid &&
                calibration.headReferenceLocalValid;
        }

        bool usedSourceEyeAngles = false;
        if (sourceEyeAnglesValid)
        {
            usedSourceEyeAngles =
                HooksWorldPoseBuildEyeToHeadCalibration(
                    sourceEyeAngles,
                    sourceBones[layout.head],
                    hmdToHeadSample);
        }
        if (!usedSourceEyeAngles &&
            !HooksWorldPoseBuildHmdToHeadCalibrationSample(
                hmdWorld,
                sourceBones[layout.head],
                hmdToHeadSample))
        {
            return calibration.hmdToHeadValid &&
                calibration.neckReferenceLocalValid &&
                calibration.headReferenceLocalValid;
        }

        HooksWorldPoseStableTransformSamples hmdSamples =
            calibration.hmdToHeadSamples;
        HooksWorldPoseStableTransformSamples neckSamples =
            calibration.neckReferenceSamples;
        HooksWorldPoseStableTransformSamples headSamples =
            calibration.headReferenceSamples;
        if (!HooksWorldPoseAccumulateStableTransformSample(
                hmdToHeadSample,
                hmdSamples) ||
            !HooksWorldPoseAccumulateStableTransformSample(
                neckLocal,
                neckSamples) ||
            !HooksWorldPoseAccumulateStableTransformSample(
                headLocal,
                headSamples))
        {
            return calibration.hmdToHeadValid &&
                calibration.neckReferenceLocalValid &&
                calibration.headReferenceLocalValid;
        }

        calibration.hmdToHeadSamples = hmdSamples;
        calibration.neckReferenceSamples = neckSamples;
        calibration.headReferenceSamples = headSamples;
        if (usedSourceEyeAngles)
            ++calibration.headCalibrationEyeSampleCount;
        if (calibration.headCalibrationStartTickMs == 0u)
            calibration.headCalibrationStartTickMs = now;
        calibration.headCalibrationLastSampleTickMs = now;
        calibration.headCalibrationLastStereoSceneGeneration =
            stereoSceneGeneration;

        const std::uint64_t elapsed =
            now >= calibration.headCalibrationStartTickMs
                ? now - calibration.headCalibrationStartTickMs
                : 0u;
        constexpr std::uint32_t kHeadCalibrationMinimumSamples = 10u;
        constexpr std::uint64_t kHeadCalibrationMinimumMs = 300u;
        constexpr std::uint64_t kHeadCalibrationRefineMs = 2000u;
        if (hmdSamples.count >= kHeadCalibrationMinimumSamples &&
            elapsed >= kHeadCalibrationMinimumMs)
        {
            vr_vm_stabilize::Mat3x4 stableHmdToHead{};
            vr_vm_stabilize::Mat3x4 stableNeckLocal{};
            vr_vm_stabilize::Mat3x4 stableHeadLocal{};
            if (HooksWorldPoseFinalizeStableTransformSamples(
                    hmdSamples,
                    stableHmdToHead) &&
                HooksWorldPoseFinalizeStableTransformSamples(
                    neckSamples,
                    stableNeckLocal) &&
                HooksWorldPoseFinalizeStableTransformSamples(
                    headSamples,
                    stableHeadLocal))
            {
                const bool wasReady = calibration.hmdToHeadValid;
                stableHmdToHead.m[0][3] = 0.0f;
                stableHmdToHead.m[1][3] = 0.0f;
                stableHmdToHead.m[2][3] = 0.0f;
                calibration.hmdToHead = stableHmdToHead;
                calibration.neckReferenceLocal = stableNeckLocal;
                calibration.headReferenceLocal = stableHeadLocal;
                calibration.hmdToHeadValid = true;
                calibration.neckReferenceLocalValid = true;
                calibration.headReferenceLocalValid = true;
                calibration.hmdToHeadUsesSourceEyeAngles =
                    calibration.headCalibrationEyeSampleCount * 2u >=
                    hmdSamples.count;
                if (!wasReady)
                    calibration.headCalibrationReadyTickMs = now;
                if (elapsed >= kHeadCalibrationRefineMs)
                    calibration.headCalibrationComplete = true;
            }
        }

        return calibration.hmdToHeadValid &&
            calibration.neckReferenceLocalValid &&
            calibration.headReferenceLocalValid;
    }

    inline float HooksWorldPoseHeadCalibrationBlendWeight(
        const HooksWorldPoseCalibration& calibration,
        std::uint64_t now)
    {
        if (!calibration.hmdToHeadValid ||
            calibration.headCalibrationReadyTickMs == 0u ||
            now < calibration.headCalibrationReadyTickMs)
        {
            return 0.0f;
        }
        constexpr float kHeadCalibrationBlendMs = 200.0f;
        float weight = std::clamp(
            static_cast<float>(
                now - calibration.headCalibrationReadyTickMs) /
                kHeadCalibrationBlendMs,
            0.0f,
            1.0f);
        return weight * weight * (3.0f - 2.0f * weight);
    }

    inline bool HooksWorldPoseBlendLocalTransform(
        const vr_vm_stabilize::Mat3x4& baseLocal,
        const vr_vm_stabilize::Mat3x4& solvedLocal,
        float weight,
        vr_vm_stabilize::Mat3x4& outLocal)
    {
        HooksWorldPoseQuaternion baseRotation{};
        HooksWorldPoseQuaternion solvedRotation{};
        HooksWorldPoseQuaternion blendedRotation{};
        if (!HooksWorldPoseMatrixQuaternion(baseLocal, baseRotation) ||
            !HooksWorldPoseMatrixQuaternion(solvedLocal, solvedRotation) ||
            !HooksWorldPoseSlerpQuaternion(
                baseRotation,
                solvedRotation,
                weight,
                blendedRotation))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 baseRigid{};
        if (!HooksViewmodelAutoGripNormalizeRigidMatrix(
                baseLocal,
                baseRigid))
        {
            return false;
        }
        baseRigid.m[0][3] = 0.0f;
        baseRigid.m[1][3] = 0.0f;
        baseRigid.m[2][3] = 0.0f;
        const vr_vm_stabilize::Mat3x4 blendedRigid =
            HooksWorldPoseQuaternionMatrix(
                blendedRotation,
                Vector(0.0f, 0.0f, 0.0f));
        vr_vm_stabilize::Mat3x4 inverseBaseRigid{};
        vr_vm_stabilize::Mat3x4 rotationDelta{};
        vr_vm_stabilize::Mat3x4 baseLinear = baseLocal;
        baseLinear.m[0][3] = 0.0f;
        baseLinear.m[1][3] = 0.0f;
        baseLinear.m[2][3] = 0.0f;
        vr_vm_stabilize::InvertTR(baseRigid, inverseBaseRigid);
        vr_vm_stabilize::Mul(
            blendedRigid,
            inverseBaseRigid,
            rotationDelta);
        vr_vm_stabilize::Mul(rotationDelta, baseLinear, outLocal);

        const Vector baseOrigin = vr_vm_stabilize::GetOrigin(baseLocal);
        const Vector solvedOrigin = vr_vm_stabilize::GetOrigin(solvedLocal);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(baseOrigin) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(solvedOrigin))
        {
            return false;
        }
        const Vector blendedOrigin =
            baseOrigin * (1.0f - weight) + solvedOrigin * weight;
        outLocal.m[0][3] = blendedOrigin.x;
        outLocal.m[1][3] = blendedOrigin.y;
        outLocal.m[2][3] = blendedOrigin.z;
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outLocal);
    }

    inline bool HooksWorldPoseBlendAnalyticArmSolution(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        const vr_vm_stabilize::Mat3x4* baseBones,
        const vr_vm_stabilize::Mat3x4* solvedBones,
        float weight,
        vr_vm_stabilize::Mat3x4* outBones)
    {
        weight = std::clamp(weight, 0.0f, 1.0f);
        if (!baseBones ||
            !solvedBones ||
            !outBones ||
            weight <= 0.0001f ||
            !HooksWorldPoseArmChainValid(
                arm,
                layout.parents,
                layout.numBones) ||
            !arm.restChainValid ||
            arm.restChainBones.empty() ||
            arm.restChainBones.size() != arm.restChainLocals.size() ||
            static_cast<int>(arm.solveMask.size()) != layout.numBones)
        {
            return false;
        }

        std::vector<vr_vm_stabilize::Mat3x4> stagedBones(
            baseBones,
            baseBones + layout.numBones);
        // Blend every kinematic link from upper arm through hand. Replacement
        // rigs can place twist/helper bones between the named joints; leaving
        // those links on the native action pose would reintroduce the exact
        // animation twist that the analytic solve removed.
        for (int joint : arm.restChainBones)
        {
            vr_vm_stabilize::Mat3x4 baseLocal{};
            vr_vm_stabilize::Mat3x4 solvedLocal{};
            vr_vm_stabilize::Mat3x4 blendedLocal{};
            if (!HooksWorldPoseCaptureBoneLocalTransform(
                    layout,
                    baseBones,
                    joint,
                    baseLocal) ||
                !HooksWorldPoseCaptureBoneLocalTransform(
                    layout,
                    solvedBones,
                    joint,
                    solvedLocal) ||
                !HooksWorldPoseBlendLocalTransform(
                    baseLocal,
                    solvedLocal,
                    weight,
                    blendedLocal) ||
                !HooksWorldPoseRestoreBoneLocalTransform(
                    layout,
                    joint,
                    blendedLocal,
                    stagedBones.data()))
            {
                return false;
            }
        }

        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            if (arm.solveMask[static_cast<size_t>(bone)] == 0u)
                continue;
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    stagedBones[static_cast<size_t>(bone)]))
            {
                return false;
            }
        }
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            if (arm.solveMask[static_cast<size_t>(bone)] != 0u)
            {
                outBones[bone] =
                    stagedBones[static_cast<size_t>(bone)];
            }
        }
        return true;
    }

    inline bool HooksWorldPoseBlendUnitDirections(
        const Vector& fromDirection,
        const Vector& toDirection,
        const Vector& fallbackTangent,
        float weight,
        Vector& outDirection)
    {
        Vector from{};
        Vector to{};
        if (!HooksNativeViewmodelArmIkNormalize(fromDirection, from) ||
            !HooksNativeViewmodelArmIkNormalize(toDirection, to))
        {
            return false;
        }

        weight = std::clamp(weight, 0.0f, 1.0f);
        const float dot = std::clamp(
            DotProduct(from, to),
            -1.0f,
            1.0f);
        if (dot > 0.9995f)
        {
            return HooksNativeViewmodelArmIkNormalize(
                from * (1.0f - weight) + to * weight,
                outDirection);
        }

        if (dot < -0.9995f)
        {
            // Near the antipode, tracker noise can reverse the tiny projected
            // component and alternate between the two possible great-circle
            // arcs. Route through a bend-plane midpoint so the cached elbow
            // hemisphere selects one stable arc while still reaching the exact
            // raw direction at weight 1.
            Vector midpoint{};
            if (!HooksNativeViewmodelArmIkProjectOntoPlane(
                    fallbackTangent,
                    from,
                    midpoint))
            {
                const Vector fallbackAxis =
                    std::fabs(from.z) < 0.75f
                        ? Vector(0.0f, 0.0f, 1.0f)
                        : Vector(0.0f, 1.0f, 0.0f);
                if (!HooksNativeViewmodelArmIkProjectOntoPlane(
                        fallbackAxis,
                        from,
                        midpoint))
                {
                    return false;
                }
            }

            if (weight <= 0.5f)
            {
                const float segmentWeight = weight * 2.0f;
                return HooksNativeViewmodelArmIkNormalize(
                    from * (1.0f - segmentWeight) +
                        midpoint * segmentWeight,
                    outDirection);
            }
            const float segmentWeight = (weight - 0.5f) * 2.0f;
            return HooksNativeViewmodelArmIkNormalize(
                midpoint * (1.0f - segmentWeight) +
                    to * segmentWeight,
                outDirection);
        }

        Vector tangent{};
        if (!HooksNativeViewmodelArmIkNormalize(
                to - from * dot,
                tangent))
        {
            // Exactly opposite directions have no unique shortest arc. Follow
            // the cached elbow plane when possible, then choose a deterministic
            // body-independent perpendicular as the last resort.
            if (!HooksNativeViewmodelArmIkProjectOntoPlane(
                    fallbackTangent,
                    from,
                    tangent))
            {
                const Vector fallbackAxis =
                    std::fabs(from.z) < 0.75f
                        ? Vector(0.0f, 0.0f, 1.0f)
                        : Vector(0.0f, 1.0f, 0.0f);
                if (!HooksNativeViewmodelArmIkProjectOntoPlane(
                        fallbackAxis,
                        from,
                        tangent))
                {
                    return false;
                }
            }
        }

        const float angle = std::acos(dot);
        const float blendedAngle = angle * weight;
        return std::isfinite(blendedAngle) &&
            HooksNativeViewmodelArmIkNormalize(
                from * std::cos(blendedAngle) +
                    tangent * std::sin(blendedAngle),
                outDirection);
    }

    inline bool HooksWorldPoseSolveArmAnalytic(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp,
        const Vector& elbowPoleBias,
        const vr_vm_stabilize::Mat3x4& handTarget,
        float weight,
        bool& bendBodyLocalValid,
        Vector& bendBodyLocal,
        std::uint64_t& bendUpdatedTickMs,
        bool& targetDirectionBodyLocalValid,
        Vector& targetDirectionBodyLocal,
        std::uint64_t& targetDirectionUpdatedTickMs,
        bool& twistValid,
        float& twistRadians,
        std::uint64_t& twistUpdatedTickMs,
        std::uint64_t now,
        bool& outTargetWithinReach,
        const vr_vm_stabilize::Mat3x4* baseBones,
        vr_vm_stabilize::Mat3x4* outBones)
    {
        outTargetWithinReach = false;
        weight = std::clamp(weight, 0.0f, 1.0f);
        if ((side != -1 && side != 1) ||
            !baseBones ||
            !outBones ||
            weight <= 0.0001f ||
            !HooksWorldPoseArmChainValid(
                arm,
                layout.parents,
                layout.numBones) ||
            !arm.restChainValid ||
            arm.restChainBones.empty() ||
            arm.restChainBones.size() != arm.restChainLocals.size() ||
            static_cast<int>(arm.solveMask.size()) != layout.numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(handTarget))
        {
            return false;
        }

        constexpr std::uint64_t kContinuityTimeoutMs = 750u;
        if (bendBodyLocalValid &&
            (bendUpdatedTickMs == 0u ||
             now < bendUpdatedTickMs ||
             now - bendUpdatedTickMs > kContinuityTimeoutMs))
        {
            bendBodyLocalValid = false;
            bendUpdatedTickMs = 0u;
        }
        if (targetDirectionBodyLocalValid &&
            (targetDirectionUpdatedTickMs == 0u ||
             now < targetDirectionUpdatedTickMs ||
             now - targetDirectionUpdatedTickMs > kContinuityTimeoutMs))
        {
            targetDirectionBodyLocalValid = false;
            targetDirectionUpdatedTickMs = 0u;
        }
        if (twistValid &&
            (!std::isfinite(twistRadians) ||
             twistUpdatedTickMs == 0u ||
             now < twistUpdatedTickMs ||
             now - twistUpdatedTickMs > kContinuityTimeoutMs))
        {
            twistValid = false;
            twistRadians = 0.0f;
            twistUpdatedTickMs = 0u;
        }

        Vector previousBendWorld{};
        const Vector* previousBend = nullptr;
        if (bendBodyLocalValid)
        {
            previousBendWorld =
                HooksNativeViewmodelArmIkBodyLocalDirectionToWorld(
                    bendBodyLocal,
                    bodyForward,
                    bodyRight,
                    bodyUp);
            if (HooksNativeViewmodelArmIkNormalize(
                    previousBendWorld,
                    previousBendWorld))
            {
                previousBend = &previousBendWorld;
            }
            else
            {
                bendBodyLocalValid = false;
                bendUpdatedTickMs = 0u;
            }
        }

        float previousTwistRadians = 0.0f;
        const float* previousTwist = nullptr;
        if (twistValid)
        {
            previousTwistRadians =
                HooksNativeViewmodelArmIkWrapRadians(twistRadians);
            if (std::isfinite(previousTwistRadians))
            {
                previousTwist = &previousTwistRadians;
            }
            else
            {
                twistValid = false;
                twistRadians = 0.0f;
                twistUpdatedTickMs = 0u;
            }
        }

        const Vector rawTarget =
            vr_vm_stabilize::GetOrigin(handTarget);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(rawTarget))
            return false;

        // Solve from a stable model-rest clavicle-to-hand chain rather than
        // Source's current fire/melee/deploy arm animation. The live torso stays
        // the parent frame, while the same-side clavicle, upper arm, forearm and
        // hand path all come from rest data before analytic IK.
        std::vector<vr_vm_stabilize::Mat3x4> candidateBones(
            baseBones,
            baseBones + layout.numBones);
        if (!HooksWorldPoseResetArmChainToRestPose(
                layout,
                arm,
                candidateBones.data()))
        {
            return false;
        }

        const Vector shoulder =
            vr_vm_stabilize::GetOrigin(candidateBones[arm.upperArm]);
        const Vector restElbow =
            vr_vm_stabilize::GetOrigin(candidateBones[arm.forearm]);
        const Vector restHand =
            vr_vm_stabilize::GetOrigin(candidateBones[arm.hand]);
        const float upperLength = (restElbow - shoulder).Length();
        const float lowerLength = (restHand - restElbow).Length();
        const float targetDistance = (rawTarget - shoulder).Length();
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(shoulder) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(restElbow) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(restHand) ||
            !std::isfinite(upperLength) ||
            !std::isfinite(lowerLength) ||
            !std::isfinite(targetDistance) ||
            upperLength < 0.25f ||
            lowerLength < 0.25f ||
            upperLength > 256.0f ||
            lowerLength > 256.0f)
        {
            return false;
        }

        const float reachEpsilon = std::max(
            0.01f,
            (upperLength + lowerLength) * 0.0005f);
        const float minimumReach =
            std::fabs(upperLength - lowerLength) + reachEpsilon;
        const float maximumReach =
            upperLength + lowerLength - reachEpsilon;
        const float worldMinimumReach = std::max(
            minimumReach,
            (upperLength + lowerLength) * 0.12f);
        if (!(worldMinimumReach < maximumReach))
            return false;
        outTargetWithinReach =
            targetDistance >= worldMinimumReach &&
            targetDistance <= maximumReach;

        Vector rawTargetDirection{};
        const bool rawTargetDirectionValid =
            HooksNativeViewmodelArmIkNormalize(
                rawTarget - shoulder,
                rawTargetDirection);
        Vector stableTargetDirection{};
        bool stableTargetDirectionValid = false;
        if (targetDirectionBodyLocalValid)
        {
            stableTargetDirection =
                HooksNativeViewmodelArmIkBodyLocalDirectionToWorld(
                    targetDirectionBodyLocal,
                    bodyForward,
                    bodyRight,
                    bodyUp);
            stableTargetDirectionValid =
                HooksNativeViewmodelArmIkNormalize(
                    stableTargetDirection,
                    stableTargetDirection);
            if (!stableTargetDirectionValid)
            {
                targetDirectionBodyLocalValid = false;
                targetDirectionUpdatedTickMs = 0u;
            }
        }

        const float directionReleaseStart = worldMinimumReach;
        const float directionReleaseEnd = worldMinimumReach * 1.75f;
        Vector targetDirectionWorld{};
        if (stableTargetDirectionValid && rawTargetDirectionValid)
        {
            float releaseWeight = std::clamp(
                (targetDistance - directionReleaseStart) /
                    (directionReleaseEnd - directionReleaseStart),
                0.0f,
                1.0f);
            releaseWeight =
                releaseWeight * releaseWeight *
                (3.0f - 2.0f * releaseWeight);
            const Vector fallbackTangent = previousBend
                ? *previousBend
                : bodyUp;
            if (!HooksWorldPoseBlendUnitDirections(
                    stableTargetDirection,
                    rawTargetDirection,
                    fallbackTangent,
                    releaseWeight,
                    targetDirectionWorld))
            {
                return false;
            }
        }
        else if (stableTargetDirectionValid)
        {
            targetDirectionWorld = stableTargetDirection;
        }
        else if (rawTargetDirectionValid)
        {
            targetDirectionWorld = rawTargetDirection;
        }
        else if (!HooksNativeViewmodelArmIkNormalize(
                     restHand - shoulder,
                     targetDirectionWorld))
        {
            return false;
        }

        const float solvedTargetDistance = std::clamp(
            targetDistance,
            worldMinimumReach,
            maximumReach);
        const Vector solvedTarget =
            shoulder + targetDirectionWorld * solvedTargetDistance;
        vr_vm_stabilize::Mat3x4 constrainedHandTarget = handTarget;
        constrainedHandTarget.m[0][3] = solvedTarget.x;
        constrainedHandTarget.m[1][3] = solvedTarget.y;
        constrainedHandTarget.m[2][3] = solvedTarget.z;

        HooksNativeViewmodelArmIkChain chain{};
        chain.side = side;
        chain.upperArm = arm.upperArm;
        chain.forearm = arm.forearm;
        chain.hand = arm.hand;

        Vector bendWorld{};
        float solvedTwistRadians = 0.0f;
        if (!HooksNativeViewmodelArmIkApplyArm(
                layout.parents,
                layout.numBones,
                chain,
                shoulder,
                constrainedHandTarget,
                bodyForward,
                bodyRight,
                bodyUp,
                previousBend,
                false,
                candidateBones.data(),
                bendWorld,
                previousTwist,
                &solvedTwistRadians,
                true,
                1.0f,
                true,
                &elbowPoleBias) ||
            !HooksWorldPoseBlendAnalyticArmSolution(
                layout,
                arm,
                baseBones,
                candidateBones.data(),
                weight,
                outBones))
        {
            return false;
        }

        // Publish continuity from the pure analytic candidate, never from the
        // partially blended render result. Native animation can remain visible
        // while tracking fades, but it cannot become the next frame's IK seed.
        Vector publishedBendWorld = bendWorld;
        const Vector publishedShoulder =
            vr_vm_stabilize::GetOrigin(candidateBones[arm.upperArm]);
        const Vector publishedElbow =
            vr_vm_stabilize::GetOrigin(candidateBones[arm.forearm]);
        const Vector publishedHand =
            vr_vm_stabilize::GetOrigin(candidateBones[arm.hand]);
        Vector publishedTargetDirection{};
        Vector publishedBendCandidate{};
        if (HooksNativeViewmodelArmIkNormalize(
                publishedHand - publishedShoulder,
                publishedTargetDirection) &&
            HooksNativeViewmodelArmIkProjectOntoPlane(
                publishedElbow - publishedShoulder,
                publishedTargetDirection,
                publishedBendCandidate))
        {
            publishedBendWorld = publishedBendCandidate;
        }

        Vector bendLocal =
            HooksNativeViewmodelArmIkWorldDirectionToBodyLocal(
                publishedBendWorld,
                bodyForward,
                bodyRight,
                bodyUp);
        if (HooksNativeViewmodelArmIkNormalize(bendLocal, bendLocal))
        {
            bendBodyLocal = bendLocal;
            bendBodyLocalValid = true;
            bendUpdatedTickMs = now;
        }
        else
        {
            bendBodyLocalValid = false;
            bendUpdatedTickMs = 0u;
        }
        // Keep the direction captured on entry into the near-shoulder region
        // fixed while the raw target crosses the singularity. The published
        // direction follows a smooth great-circle release and refreshes the
        // cache only after it reaches the outer radius.
        const Vector cachedTargetDirectionWorld =
            stableTargetDirectionValid &&
                targetDistance < directionReleaseEnd
                ? stableTargetDirection
                : targetDirectionWorld;
        Vector targetDirectionLocal =
            HooksNativeViewmodelArmIkWorldDirectionToBodyLocal(
                cachedTargetDirectionWorld,
                bodyForward,
                bodyRight,
                bodyUp);
        if (HooksNativeViewmodelArmIkNormalize(
                targetDirectionLocal,
                targetDirectionLocal))
        {
            targetDirectionBodyLocal = targetDirectionLocal;
            targetDirectionBodyLocalValid = true;
            targetDirectionUpdatedTickMs = now;
        }
        else
        {
            targetDirectionBodyLocalValid = false;
            targetDirectionUpdatedTickMs = 0u;
        }

        solvedTwistRadians =
            HooksNativeViewmodelArmIkWrapRadians(solvedTwistRadians);
        if (std::isfinite(solvedTwistRadians))
        {
            twistRadians = solvedTwistRadians;
            twistValid = true;
            twistUpdatedTickMs = now;
        }
        else
        {
            twistRadians = 0.0f;
            twistValid = false;
            twistUpdatedTickMs = 0u;
        }
        return true;
    }

    inline bool HooksWorldPosePlayerStateSuppressesIK(
        Game* game,
        const C_BaseEntity* entity,
        std::uint32_t* outReasonMask = nullptr)
    {
        if (outReasonMask)
            *outReasonMask = 0u;
        if (!game || !entity)
        {
            if (outReasonMask)
                *outReasonMask = 0x80000000u;
            return true;
        }

        const C_BasePlayer* player =
            reinterpret_cast<const C_BasePlayer*>(entity);
        bool suppressed = true;
#ifdef _MSC_VER
        __try
        {
#endif
            const int team =
                ReadNetvar<int>(player, 0xE4);
            const int lifeState =
                static_cast<int>(
                    ReadNetvar<uint8_t>(player, 0x147));
            const bool incapacitated =
                ReadNetvar<uint8_t>(player, 0x1EA9) != 0;
            const bool hangingFromLedge =
                ReadNetvar<uint8_t>(player, 0x25EC) != 0;
            const bool mountedGun =
                ReadNetvar<uint8_t>(player, 0x1EBA) != 0 ||
                ReadNetvar<uint8_t>(player, 0x1EBB) != 0;
            // Do not infer current ladder attachment solely from
            // m_vecLadderNormal at 0x13C8. Runtime diagnostics show that this
            // vector remains non-zero during normal movement in this build,
            // which would suppress VR IK permanently. Restore ladder
            // suppression only after obtaining a verified active-state source.
            const bool controlledBySI =
                IsPlayerControlledBySI(player);
            const bool doingUseOrRevive =
                IsPlayerDoingUseOrReviveAction(player);
            std::uint32_t reasonMask = 0u;
            if (team != 2)
                reasonMask |= (1u << 0);
            if (lifeState != 0)
                reasonMask |= (1u << 1);
            if (incapacitated)
                reasonMask |= (1u << 2);
            if (hangingFromLedge)
                reasonMask |= (1u << 3);
            if (mountedGun)
                reasonMask |= (1u << 4);
            if (controlledBySI)
                reasonMask |= (1u << 6);
            if (doingUseOrRevive)
                reasonMask |= (1u << 7);
            suppressed = reasonMask != 0u;
            if (outReasonMask)
                *outReasonMask = reasonMask;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            suppressed = true;
            if (outReasonMask)
                *outReasonMask = 0x40000000u;
        }
#endif
        return suppressed;
    }

    inline bool HooksWorldPoseBuildPlayerBones(
        VR* vr,
        Game* game,
        void* drawState,
        const ModelRenderInfo_t& info,
        const C_BaseEntity* entity,
        bool localFirstPersonEyeActive,
        const void* sourceBonePointer,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!vr ||
            !game ||
            !drawState ||
            !entity ||
            !sourceBonePointer ||
            !vr->m_WorldModelVRPoseEnabled ||
            !vr->m_FirstPersonControlReady.load(std::memory_order_acquire) ||
            info.entity_index <= 0 ||
            !game->IsValidPlayerIndex(info.entity_index))
        {
            if (game &&
                info.entity_index > 0 &&
                game->IsValidPlayerIndex(info.entity_index))
            {
                HooksWorldPoseClearWeaponHandState(
                    info.entity_index);
            }
            return false;
        }

        const int localPlayerIndex =
            game->m_EngineClient
                ? game->m_EngineClient->GetLocalPlayer()
                : -1;
        const bool localPlayer =
            info.entity_index == localPlayerIndex;
        const std::uint64_t worldPoseNow =
            static_cast<std::uint64_t>(GetTickCount64());
        std::uint64_t localThirdPersonWarmupUntil = 0u;
        bool localThirdPersonWarmupActive = false;
        if (localPlayer && vr->m_IsThirdPersonCamera)
        {
            localThirdPersonWarmupUntil =
                vr->m_WorldModelVRPoseLocalThirdPersonWarmupUntilTickMs.load(
                    std::memory_order_acquire);
            if (localThirdPersonWarmupUntil == 0u)
            {
                // Fail closed if a queued model draw observes the camera flag
                // before the authoritative RenderView transition publishes
                // its deadline. This also guarantees eventual progress.
                constexpr std::uint64_t kFallbackWarmupMs = 750u;
                const std::uint64_t fallbackUntil =
                    worldPoseNow + kFallbackWarmupMs;
                std::uint64_t expected = 0u;
                vr->m_WorldModelVRPoseLocalThirdPersonWarmupUntilTickMs
                    .compare_exchange_strong(
                        expected,
                        fallbackUntil,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire);
                localThirdPersonWarmupUntil =
                    expected == 0u ? fallbackUntil : expected;
            }
            localThirdPersonWarmupActive =
                worldPoseNow < localThirdPersonWarmupUntil;
        }
        const bool localPoseAllowed =
            !localPlayer ||
            (vr->m_WorldModelVRPoseLocalThirdPerson &&
             !localThirdPersonWarmupActive &&
             (vr->m_IsThirdPersonCamera ||
              !localFirstPersonEyeActive));

        VRPoseFrame pose{};
        float freshness = 0.0f;
        const bool hasPose =
            game->GetInterpolatedVRPose(
                info.entity_index,
                vr->m_WorldModelVRPoseInterpolationMs,
                vr->m_WorldModelVRPoseStaleAfterMs,
                pose,
                freshness);
        std::uint32_t suppressionReasonMask = 0u;
        const bool stateSuppressed =
            HooksWorldPosePlayerStateSuppressesIK(
                game,
                entity,
                &suppressionReasonMask);
        const float targetWeight =
            (hasPose &&
             localPoseAllowed &&
             !stateSuppressed)
                ? freshness
                : 0.0f;
        const float weight =
            game->AdvanceVRPoseBlendWeight(
                info.entity_index,
                targetWeight,
                vr->m_WorldModelVRPoseBlendSeconds);
        if (!hasPose || weight <= 0.0001f)
        {
            if (vr->m_WorldModelVRPoseDebugLog)
            {
                static thread_local std::array<
                    std::uint64_t,
                    Game::kMaxPlayers> s_lastSkipDebugTick{};
                const std::uint64_t now =
                    static_cast<std::uint64_t>(GetTickCount64());
                std::uint64_t& last =
                    s_lastSkipDebugTick[
                        static_cast<size_t>(info.entity_index)];
                if (now - last >= 1000u)
                {
                    last = now;
                    Game::logMsg(
                        "[VR][WorldPose] pre-IK skip player=%d local=%d hasPose=%d freshness=%.2f weight=%.2f thirdPerson=%d firstPersonBodyEye=%d localAllowed=%d warmupMs=%llu suppressed=0x%08X",
                        info.entity_index,
                        localPlayer ? 1 : 0,
                        hasPose ? 1 : 0,
                        freshness,
                        weight,
                        vr->m_IsThirdPersonCamera ? 1 : 0,
                        localFirstPersonEyeActive ? 1 : 0,
                        localPoseAllowed ? 1 : 0,
                        static_cast<unsigned long long>(
                            localThirdPersonWarmupActive
                                ? localThirdPersonWarmupUntil - worldPoseNow
                                : 0u),
                        static_cast<unsigned int>(
                            suppressionReasonMask));
                }
            }
            HooksWorldPoseClearWeaponHandState(
                info.entity_index);
            return false;
        }

        HooksWorldPoseBoneLayout* const layout =
            HooksWorldPoseGetBoneLayout(drawState);
        if (!layout || !layout->valid)
        {
            if (vr->m_WorldModelVRPoseDebugLog)
            {
                static thread_local std::array<
                    std::uint64_t,
                    Game::kMaxPlayers> s_lastLayoutSkipDebugTick{};
                const std::uint64_t now =
                    static_cast<std::uint64_t>(GetTickCount64());
                std::uint64_t& last =
                    s_lastLayoutSkipDebugTick[
                        static_cast<size_t>(info.entity_index)];
                if (now - last >= 1000u)
                {
                    last = now;
                    Game::logMsg(
                        "[VR][WorldPose] layout skip player=%d state=%p bones=%p",
                        info.entity_index,
                        drawState,
                        sourceBonePointer);
                }
            }
            HooksWorldPoseClearWeaponHandState(
                info.entity_index);
            return false;
        }

        uint32_t sequence =
            vr->m_RenderFrameSeq.load(
                std::memory_order_relaxed) &
            ~1u;
        if (sequence == 0u)
        {
            sequence =
                (static_cast<uint32_t>(GetTickCount()) << 1u) |
                2u;
        }
        vr_vm_stabilize::Mat3x4* const bones =
            vr_vm_stabilize::AllocStableBones(
                layout->numBones,
                sequence);
        if (!bones)
        {
            HooksWorldPoseClearWeaponHandState(
                info.entity_index);
            return false;
        }

        const std::uint64_t stereoNow =
            static_cast<std::uint64_t>(GetTickCount64());
        const std::uint64_t stereoSceneGeneration =
            g_HooksWorldPoseStereoSceneGeneration.load(
                std::memory_order_acquire);
        if (HooksWorldPoseLoadStereoPlayerBones(
                entity,
                layout->studioHdr,
                stereoSceneGeneration,
                layout->numBones,
                bones,
                stereoNow))
        {
            outBones = bones;
            return true;
        }

        const auto* sourceBones =
            reinterpret_cast<
                const vr_vm_stabilize::Mat3x4*>(
                    sourceBonePointer);
        for (int bone = 0; bone < layout->numBones; ++bone)
        {
            if (!vr_vm_stabilize::SafeRead(
                    sourceBones + bone,
                    bones[bone]) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    bones[bone]))
            {
                HooksWorldPoseClearWeaponHandState(
                    info.entity_index);
                return false;
            }
        }

        std::unique_lock<std::mutex> calibrationLock(
            g_HooksWorldPoseCalibrationMutexes[
                static_cast<size_t>(info.entity_index)]);
        // Another render worker may have completed this player while we were
        // waiting for its persistent pose state. Reuse that exact result.
        if (HooksWorldPoseLoadStereoPlayerBones(
                entity,
                layout->studioHdr,
                stereoSceneGeneration,
                layout->numBones,
                bones,
                stereoNow))
        {
            outBones = bones;
            return true;
        }
        HooksWorldPoseCalibration& calibration =
            g_HooksWorldPoseCalibrations[
                static_cast<size_t>(info.entity_index)];
        const std::uint32_t poseLockGeneration =
            vr->m_NativeViewmodelLeftHandFreezeGeneration.load(
                std::memory_order_acquire);
        if (calibration.entity != entity ||
            calibration.studioHdr != layout->studioHdr ||
            calibration.poseLockGeneration != poseLockGeneration)
        {
            calibration.Reset(
                entity,
                layout->studioHdr,
                poseLockGeneration);
        }
        if (localPlayer &&
            vr->m_IsThirdPersonCamera &&
            localThirdPersonWarmupUntil != 0u &&
            calibration.localThirdPersonWarmupToken !=
                localThirdPersonWarmupUntil)
        {
            calibration.ResetHeadCalibration();
            calibration.localThirdPersonWarmupToken =
                localThirdPersonWarmupUntil;
        }
        if (calibration.lastStereoSceneGeneration >
                stereoSceneGeneration)
        {
            return false;
        }
        calibration.lastStereoSceneGeneration =
            stereoSceneGeneration;
        bool headChainReferenceValid = false;

        vr_vm_stabilize::Mat3x4 hmdWorld{};
        vr_vm_stabilize::Mat3x4 leftControllerWorld{};
        vr_vm_stabilize::Mat3x4 rightControllerWorld{};
        vr_vm_stabilize::Mat3x4 headTarget{};
        vr_vm_stabilize::Mat3x4 leftHandTarget{};
        vr_vm_stabilize::Mat3x4 rightHandTarget{};
        QAngle sourceEyeAngles{};
        bool sourceEyeAnglesValid = false;
        Vector rawHmdLocalDelta(0.0f, 0.0f, 0.0f);
        Vector bodyLeanLocalMeters(0.0f, 0.0f, 0.0f);

        // The outer blend weight provides the normal fade back to Source for
        // stale tracking and suppressed player states.
        constexpr float kWorldPoseTrackingAuthority = 1.0f;
        const float trackingWeight =
            std::clamp(
                weight * kWorldPoseTrackingAuthority,
                0.0f,
                1.0f);

        const float sourceBodyYaw =
            HooksWorldPoseWrapAngle(info.angles.y);
        float targetVisualBodyYaw = sourceBodyYaw;
        const bool instantLocalTrackedBodyYaw =
            localPlayer &&
            pose.bodyYawValid &&
            (pose.validMask & l4d2vr_pose::kValidHmd) != 0u &&
            std::isfinite(pose.hmd.angles.y);
        if (instantLocalTrackedBodyYaw)
        {
            // Use the same local tracked-body yaw as the first-person body.
            // Snap/smooth turning rotates the torso immediately, while physical
            // HMD yaw stays head-only inside the configured comfort cone.
            const float headWorldYaw =
                HooksWorldPoseWrapAngle(
                    pose.bodyYaw +
                    pose.hmd.angles.y);
            targetVisualBodyYaw = HooksTrackedBodyResolveVisualYaw(
                vr,
                headWorldYaw);
        }
        float visualBodyYaw = sourceBodyYaw;
        float appliedBodyYaw = sourceBodyYaw;
        float visualBodyYawError = 0.0f;
        bool visualBodyYawTurning = false;
        HooksWorldPoseUpdateVisualBodyYaw(
            vr,
            targetVisualBodyYaw,
            instantLocalTrackedBodyYaw,
            calibration,
            visualBodyYaw,
            visualBodyYawError,
            visualBodyYawTurning);
        const bool visualBodyYawApplied =
            HooksWorldPoseApplyVisualBodyYaw(
                *layout,
                info.origin,
                sourceBodyYaw,
                visualBodyYaw,
                trackingWeight,
                bones,
                appliedBodyYaw);

        Vector bodyForward{};
        Vector bodyRight{};
        Vector bodyUp{};
        QAngle::AngleVectors(
            QAngle(0.0f, appliedBodyYaw, 0.0f),
            &bodyForward,
            &bodyRight,
            &bodyUp);

        C_BaseCombatWeapon* heldWeaponForGrip = nullptr;
        void* heldWeaponRenderableForGrip = nullptr;
        const bool localEmptyHandsPlaceholder =
            localPlayer &&
            vr->m_ManualInventoryEmptyHandsActive.load(
                std::memory_order_acquire);
        if (!localEmptyHandsPlaceholder)
        {
            HooksWorldPoseGetActiveWeaponSafe(
                entity,
                heldWeaponForGrip,
                heldWeaponRenderableForGrip);
        }
        if (calibration.rightGripWeapon !=
            heldWeaponForGrip)
        {
            calibration.rightGripWeapon =
                heldWeaponForGrip;
            calibration.rightGripPreviousLocalValid = false;
            calibration.rightGripShotBaselineValid = false;
            calibration.rightGripShotAxisHemisphereValid = false;
            calibration.rightGripPreviousClip = -2147483647;
            calibration.rightGripLastStereoSceneGeneration = 0u;
            calibration.rightGripShotImpulseAngleRad = 0.0f;
            calibration.rightGripShotCaptureUntilTickMs = 0u;
            calibration.rightGripShotImpulseTickMs = 0u;
            calibration.rightGripShotAxisHemisphere = Vector{};
            calibration.rightGripShotImpulseAxis = Vector{};
            // The static bind palm-to-hand basis lives in the bone layout and
            // is intentionally unaffected by weapon switches. Only transient
            // shot sampling state is reset here.
        }
        const std::uint64_t gripNow =
            static_cast<std::uint64_t>(GetTickCount64());
        bool weaponShotTriggered = false;
        if (heldWeaponForGrip)
        {
            int currentClip = -2147483647;
            if (HooksWorldPoseReadWeaponClipSafe(
                    heldWeaponForGrip,
                    currentClip))
            {
                if (calibration.rightGripPreviousClip >= 0 &&
                    currentClip >= 0 &&
                    currentClip < calibration.rightGripPreviousClip)
                {
                    weaponShotTriggered = true;
                }
                calibration.rightGripPreviousClip = currentClip;
            }
        }

        // The packet's local coordinates were encoded in its sampled body-yaw
        // frame, which can differ substantially from the current render yaw
        // while the player turns. Decode every tracker in that same frame.
        ModelRenderInfo_t poseFrameInfo = info;
        if (pose.bodyYawValid)
        {
            poseFrameInfo.angles.y = pose.bodyYaw;
        }

        const bool hmdTransformValid =
            (pose.validMask & l4d2vr_pose::kValidHmd) != 0u &&
            layout->head >= 0 &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.hmd,
                poseFrameInfo,
                hmdWorld);
        if (hmdTransformValid)
        {
            sourceEyeAnglesValid =
                HooksWorldPoseReadSourceEyeAngles(
                    game,
                    entity,
                    sourceEyeAngles);
            headChainReferenceValid =
                HooksWorldPoseUpdateStableHeadCalibration(
                    *layout,
                    sourceBones,
                    hmdWorld,
                    sourceEyeAnglesValid,
                    sourceEyeAngles,
                    stereoSceneGeneration,
                    worldPoseNow,
                    calibration);
        }
        const bool hmdPoseValid =
            hmdTransformValid &&
            calibration.hmdToHeadValid;
        const float headTrackingWeight =
            hmdPoseValid
                ? trackingWeight *
                    HooksWorldPoseHeadCalibrationBlendWeight(
                        calibration,
                        worldPoseNow)
                : 0.0f;
        if (hmdPoseValid)
        {
            vr_vm_stabilize::Mul(
                hmdWorld,
                calibration.hmdToHead,
                headTarget);

            if (!calibration.hmdReferenceLocalPositionValid)
            {
                calibration.hmdReferenceLocalPosition =
                    pose.hmd.position;
                calibration.hmdReferenceLocalPositionValid = true;
                if (pose.bodyYawValid)
                {
                    calibration.hmdReferenceBodyYaw = pose.bodyYaw;
                    calibration.hmdReferenceBodyYawValid = true;
                }
            }

            // Convert physical HMD translation into the current visible body's
            // planar frame. Player origin motion is already removed by the pose
            // packet, so 1:1 room movement naturally leaves only the residual
            // offset inside the lean envelope. No chest/head translation is
            // applied here; the offset drives an upper-chest rotation below.
            Vector hmdReferenceFramePosition =
                pose.hmd.position;
            if (pose.bodyYawValid &&
                calibration.hmdReferenceBodyYawValid)
            {
                Vector poseForward{};
                Vector poseRight{};
                Vector poseUp{};
                Vector referenceForward{};
                Vector referenceRight{};
                Vector referenceUp{};
                QAngle::AngleVectors(
                    QAngle(0.0f, pose.bodyYaw, 0.0f),
                    &poseForward,
                    &poseRight,
                    &poseUp);
                QAngle::AngleVectors(
                    QAngle(
                        0.0f,
                        calibration.hmdReferenceBodyYaw,
                        0.0f),
                    &referenceForward,
                    &referenceRight,
                    &referenceUp);
                const Vector hmdWorldOffset =
                    poseForward * pose.hmd.position.x +
                    poseRight * pose.hmd.position.y +
                    poseUp * pose.hmd.position.z;
                hmdReferenceFramePosition = Vector(
                    DotProduct(hmdWorldOffset, referenceForward),
                    DotProduct(hmdWorldOffset, referenceRight),
                    DotProduct(hmdWorldOffset, referenceUp));
            }
            rawHmdLocalDelta =
                hmdReferenceFramePosition -
                calibration.hmdReferenceLocalPosition;

            const float hmdDeltaFrameYaw =
                calibration.hmdReferenceBodyYawValid
                    ? calibration.hmdReferenceBodyYaw
                    : HooksWorldPoseWrapAngle(info.angles.y);
            Vector referenceForward{};
            Vector referenceRight{};
            Vector referenceUp{};
            QAngle::AngleVectors(
                QAngle(0.0f, hmdDeltaFrameYaw, 0.0f),
                &referenceForward,
                &referenceRight,
                &referenceUp);
            const Vector rawHmdPlanarWorld =
                referenceForward * rawHmdLocalDelta.x +
                referenceRight * rawHmdLocalDelta.y;
            const float unitsPerMeter =
                (std::isfinite(vr->m_VRScale) &&
                 std::fabs(vr->m_VRScale) > 0.001f)
                    ? std::fabs(vr->m_VRScale)
                    : 43.2f;
            bodyLeanLocalMeters = Vector(
                DotProduct(rawHmdPlanarWorld, bodyForward) / unitsPerMeter,
                DotProduct(rawHmdPlanarWorld, bodyRight) / unitsPerMeter,
                0.0f);
        }

        vr_vm_stabilize::Mat3x4 leftStableFallbackHand{};
        vr_vm_stabilize::Mat3x4 rightStableFallbackHand{};
        const bool leftStableFallbackValid =
            layout->left.palmToHandValid ||
            HooksWorldPoseBuildRestHandReference(
                *layout,
                layout->left,
                bones,
                leftStableFallbackHand);
        const bool rightStableFallbackValid =
            layout->right.palmToHandValid ||
            HooksWorldPoseBuildRestHandReference(
                *layout,
                layout->right,
                bones,
                rightStableFallbackHand);

        const bool leftPoseValid =
            leftStableFallbackValid &&
            (pose.validMask &
             l4d2vr_pose::kValidLeftHand) != 0u &&
            HooksWorldPoseArmChainValid(
                layout->left,
                layout->parents,
                layout->numBones) &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.leftHand,
                poseFrameInfo,
                leftControllerWorld) &&
            HooksWorldPoseBuildStaticHandTarget(
                layout->left,
                leftControllerWorld,
                layout->left.palmToHandValid
                    ? bones[layout->left.hand]
                    : leftStableFallbackHand,
                vr->m_WorldModelVRPoseLeftHandRotationOffsetDeg,
                leftHandTarget);

        const bool rightPoseValid =
            rightStableFallbackValid &&
            (pose.validMask &
             l4d2vr_pose::kValidRightHand) != 0u &&
            HooksWorldPoseArmChainValid(
                layout->right,
                layout->parents,
                layout->numBones) &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.rightHand,
                poseFrameInfo,
                rightControllerWorld) &&
            HooksWorldPoseBuildStaticHandTarget(
                layout->right,
                rightControllerWorld,
                layout->right.palmToHandValid
                    ? bones[layout->right.hand]
                    : rightStableFallbackHand,
                vr->m_WorldModelVRPoseRightHandRotationOffsetDeg,
                rightHandTarget);

        bool weaponGripRotationValid = false;
        if (rightPoseValid)
        {
            if (heldWeaponForGrip)
            {
                weaponGripRotationValid =
                    HooksWorldPoseApplyShotWristRotation(
                        *layout,
                        sourceBones,
                        layout->right.hand,
                        weaponShotTriggered,
                        gripNow,
                        stereoSceneGeneration,
                        calibration.rightGripPreviousLocalValid,
                        calibration.rightGripPreviousLocal,
                        calibration.rightGripLastStereoSceneGeneration,
                        calibration.rightGripShotBaselineValid,
                        calibration.rightGripShotBaselineLocal,
                        calibration.rightGripShotAxisHemisphereValid,
                        calibration.rightGripShotAxisHemisphere,
                        calibration.rightGripShotImpulseAxis,
                        calibration.rightGripShotImpulseAngleRad,
                        calibration.rightGripShotCaptureUntilTickMs,
                        calibration.rightGripShotImpulseTickMs,
                        rightHandTarget);
            }
        }

        bool changed = visualBodyYawApplied;
        if (hmdPoseValid &&
            HooksNativeViewmodelHandsOnlyMatrixFinite(
                headTarget))
        {
            // Physical HMD X/Y drives a bounded upper-chest tilt instead of
            // translating the whole upper body. With 1:1 room movement enabled,
            // player-origin motion consumes only the displacement beyond the same
            // lean radius; without 1:1, this simply saturates at the configured
            // maximum lean and never moves the player/body origin.
            int bodyLeanRoot = -1;
            if (layout->upperChest >= 0 &&
                layout->upperChest < layout->numBones &&
                layout->upperChest != layout->head)
            {
                bodyLeanRoot = layout->upperChest;
            }
            else if (layout->neck >= 0 &&
                layout->neck < layout->numBones &&
                layout->neck != layout->head)
            {
                bodyLeanRoot = layout->neck;
            }
            if (bodyLeanRoot >= 0)
            {
                const Vector leanPivot =
                    vr_vm_stabilize::GetOrigin(bones[bodyLeanRoot]);
                vr_vm_stabilize::Mat3x4 leanDelta{};
                if (HooksTrackedBodyBuildLeanDelta(
                        vr,
                        leanPivot,
                        bodyForward,
                        bodyRight,
                        bodyUp,
                        bodyLeanLocalMeters,
                        trackingWeight,
                        leanDelta))
                {
                    changed =
                        HooksWorldPoseApplyDeltaToBranch(
                            *layout,
                            bodyLeanRoot,
                            leanDelta,
                            bones) ||
                        changed;
                }
            }

            // Filter Source's neck/head idle channels before applying HMD
            // orientation. Rebuilding both local links from a fixed reference
            // prevents the animated neck parent from dragging the tracked
            // head, while upper-chest breathing still moves the whole chain.
            if (headChainReferenceValid)
            {
                changed =
                    HooksWorldPoseRestoreHeadChain(
                        *layout,
                        calibration,
                        headTrackingWeight,
                        bones) ||
                    changed;
                // Let the stable neck absorb a modest share of the HMD
                // rotation, then finish the exact target on the head below.
                // This keeps the neck skin from being twisted between a
                // static parent and a fully rotated head.
                constexpr float kHmdNeckRotationShare = 0.35f;
                changed =
                    HooksWorldPoseShareChildOrientationWithParent(
                        *layout,
                        layout->neck,
                        layout->head,
                        headTarget,
                        kHmdNeckRotationShare *
                            headTrackingWeight,
                        bones) ||
                    changed;
            }

            // Rotate only around the final head origin. The head and every
            // passive hair/face descendant follow as a rigid branch.
            changed =
                HooksWorldPoseOrientBranch(
                    *layout,
                    layout->head,
                    headTarget,
                    headTrackingWeight,
                    bones) ||
                changed;
        }

        // Solve both sides from the exact same post-body/head pose. Publishing
        // only the cached side mask makes the result independent of solve order.
        const std::vector<vr_vm_stabilize::Mat3x4> armBaseBones(
            bones,
            bones + layout->numBones);
        bool leftArmSolved = false;
        bool rightArmSolved = false;
        bool leftTargetWithinReach = false;
        bool rightTargetWithinReach = false;
        if (leftPoseValid)
        {
            leftArmSolved =
                HooksWorldPoseSolveArmAnalytic(
                    *layout,
                    layout->left,
                    -1,
                    bodyForward,
                    bodyRight,
                    bodyUp,
                    vr->m_NativeViewmodelArmElbowPoleBias,
                    leftHandTarget,
                    trackingWeight,
                    calibration.leftBendBodyLocalValid,
                    calibration.leftBendBodyLocal,
                    calibration.leftBendUpdatedTickMs,
                    calibration.leftTargetDirectionBodyLocalValid,
                    calibration.leftTargetDirectionBodyLocal,
                    calibration.leftTargetDirectionUpdatedTickMs,
                    calibration.leftTwistValid,
                    calibration.leftTwistRadians,
                    calibration.leftTwistUpdatedTickMs,
                    stereoNow,
                    leftTargetWithinReach,
                    armBaseBones.data(),
                    bones);
            if (!leftArmSolved)
                HooksWorldPoseClearArmContinuity(calibration, -1);
            changed = leftArmSolved || changed;
        }
        else
        {
            HooksWorldPoseClearArmContinuity(calibration, -1);
        }
        if (rightPoseValid)
        {
            rightArmSolved =
                HooksWorldPoseSolveArmAnalytic(
                    *layout,
                    layout->right,
                    1,
                    bodyForward,
                    bodyRight,
                    bodyUp,
                    vr->m_NativeViewmodelArmElbowPoleBias,
                    rightHandTarget,
                    trackingWeight,
                    calibration.rightBendBodyLocalValid,
                    calibration.rightBendBodyLocal,
                    calibration.rightBendUpdatedTickMs,
                    calibration.rightTargetDirectionBodyLocalValid,
                    calibration.rightTargetDirectionBodyLocal,
                    calibration.rightTargetDirectionUpdatedTickMs,
                    calibration.rightTwistValid,
                    calibration.rightTwistRadians,
                    calibration.rightTwistUpdatedTickMs,
                    stereoNow,
                    rightTargetWithinReach,
                    armBaseBones.data(),
                    bones);
            if (!rightArmSolved)
                HooksWorldPoseClearArmContinuity(calibration, 1);
            changed = rightArmSolved || changed;
        }
        else
        {
            HooksWorldPoseClearArmContinuity(calibration, 1);
        }
        // Finger transforms are the final local-pose layer. The local
        // third-person model keeps its existing direct OpenVR path. Remote
        // players consume protocol-v2 state: native weapon/support hands retain
        // Source animation, while free/empty hands are rebuilt from the stable
        // rest pose and receive the transmitted anatomical finger curls.
        if (localPlayer && leftArmSolved)
        {
            const bool leftBaseReady =
                HooksWorldPoseEnsureAnimationIndependentFingerBase(
                    *layout,
                    layout->left,
                    layout->leftFingers,
                    sourceBones,
                    true,
                    nullptr,
                    calibration.leftFingerPose);
            if (!leftBaseReady ||
                !HooksWorldPoseApplyAnimationIndependentFingerBase(
                    *layout,
                    layout->left,
                    layout->leftFingers,
                    calibration.leftFingerPose,
                    bones))
            {
                HooksWorldPoseClearWeaponHandState(info.entity_index);
                return false;
            }
            changed =
                HooksWorldPoseTryApplyLocalTrackedFingerPose(
                    vr,
                    *layout,
                    layout->left,
                    -1,
                    trackingWeight,
                    bones) ||
                changed;
            changed = true;
        }
        else if (!localPlayer && leftArmSolved)
        {
            const bool useNativeAnimation =
                (pose.handStateFlags &
                 l4d2vr_pose::kHandStateLeftNativeFingerAnimation) != 0u;
            if (!useNativeAnimation)
            {
                const bool leftBaseReady =
                    HooksWorldPoseEnsureAnimationIndependentFingerBase(
                        *layout,
                        layout->left,
                        layout->leftFingers,
                        sourceBones,
                        true,
                        nullptr,
                        calibration.leftFingerPose);
                if (!leftBaseReady ||
                    !HooksWorldPoseApplyAnimationIndependentFingerBase(
                        *layout,
                        layout->left,
                        layout->leftFingers,
                        calibration.leftFingerPose,
                        bones))
                {
                    HooksWorldPoseClearWeaponHandState(info.entity_index);
                    return false;
                }
                if ((pose.featureMask &
                     l4d2vr_pose::kFeatureLeftFingerCurls) != 0u)
                {
                    changed =
                        HooksWorldPoseTryApplyNetworkTrackedFingerPose(
                            vr,
                            *layout,
                            layout->left,
                            -1,
                            pose.leftFingerCurls,
                            trackingWeight,
                            bones) ||
                        changed;
                }
                changed = true;
            }
        }
        if (localPlayer && rightArmSolved)
        {
            const bool rightUsesTrackedRest =
                localEmptyHandsPlaceholder;
            const void* rightFingerPoseKey =
                rightUsesTrackedRest
                    ? nullptr
                    : static_cast<const void*>(heldWeaponForGrip);
            const bool rightBaseReady =
                HooksWorldPoseEnsureAnimationIndependentFingerBase(
                    *layout,
                    layout->right,
                    layout->rightFingers,
                    sourceBones,
                    rightUsesTrackedRest,
                    rightFingerPoseKey,
                    calibration.rightFingerPose);
            if (!rightBaseReady ||
                !HooksWorldPoseApplyAnimationIndependentFingerBase(
                    *layout,
                    layout->right,
                    layout->rightFingers,
                    calibration.rightFingerPose,
                    bones))
            {
                HooksWorldPoseClearWeaponHandState(info.entity_index);
                return false;
            }
            if (rightUsesTrackedRest)
            {
                changed =
                    HooksWorldPoseTryApplyLocalTrackedFingerPose(
                        vr,
                        *layout,
                        layout->right,
                        1,
                        trackingWeight,
                        bones) ||
                    changed;
            }
            changed = true;
        }
        else if (!localPlayer && rightArmSolved)
        {
            const bool useNativeAnimation =
                (pose.handStateFlags &
                 l4d2vr_pose::kHandStateRightNativeFingerAnimation) != 0u;
            if (!useNativeAnimation)
            {
                const bool rightBaseReady =
                    HooksWorldPoseEnsureAnimationIndependentFingerBase(
                        *layout,
                        layout->right,
                        layout->rightFingers,
                        sourceBones,
                        true,
                        nullptr,
                        calibration.rightFingerPose);
                if (!rightBaseReady ||
                    !HooksWorldPoseApplyAnimationIndependentFingerBase(
                        *layout,
                        layout->right,
                        layout->rightFingers,
                        calibration.rightFingerPose,
                        bones))
                {
                    HooksWorldPoseClearWeaponHandState(info.entity_index);
                    return false;
                }
                if ((pose.featureMask &
                     l4d2vr_pose::kFeatureRightFingerCurls) != 0u)
                {
                    changed =
                        HooksWorldPoseTryApplyNetworkTrackedFingerPose(
                            vr,
                            *layout,
                            layout->right,
                            1,
                            pose.rightFingerCurls,
                            trackingWeight,
                            bones) ||
                        changed;
                }
                changed = true;
            }
        }

        // Never let a stale controller relation keep driving the separate
        // weapon renderable after the right arm failed to publish. A reachable,
        // fully weighted solve may predict a draw-before-owner weapon directly
        // from the controller; constrained or fading solves fall back to the
        // exact native-to-final hand correction published by this frame.
        if (localEmptyHandsPlaceholder || !rightArmSolved)
        {
            HooksWorldPoseClearWeaponHandState(
                info.entity_index);
        }

        if (!changed)
            return false;
        for (int bone = 0; bone < layout->numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    bones[bone]))
            {
                HooksWorldPoseClearWeaponHandState(
                    info.entity_index);
                return false;
            }
        }

        // The active weapon is a separate renderable/bonemerge draw and never
        // sees this temporary player-bone copy.  Publish the exact native and
        // final right-hand transforms so its later draw can receive the same
        // rigid correction without mutating Source's shared bone cache.
        if (!localEmptyHandsPlaceholder && rightArmSolved)
        {
            if (!HooksWorldPosePublishWeaponHandState(
                    vr,
                    game,
                    entity,
                    info.entity_index,
                    info,
                    *layout,
                    sourceBones,
                    bones,
                    rightControllerWorld,
                    rightArmSolved,
                    rightTargetWithinReach &&
                        trackingWeight >= 0.999f))
            {
                HooksWorldPoseClearWeaponHandState(
                    info.entity_index);
            }
        }

        HooksWorldPoseStoreStereoPlayerBones(
            entity,
            layout->studioHdr,
            stereoSceneGeneration,
            layout->numBones,
            bones,
            stereoNow);

        if (vr->m_WorldModelVRPoseDebugLog)
        {
            static thread_local std::array<
                std::uint64_t,
                Game::kMaxPlayers> s_lastDebugTick{};
            const std::uint64_t now =
                static_cast<std::uint64_t>(
                    GetTickCount64());
            std::uint64_t& last =
                s_lastDebugTick[
                    static_cast<size_t>(info.entity_index)];
            if (now - last >= 1000u)
            {
                last = now;
                float nativeNeckLength = -1.0f;
                float finalNeckLength = -1.0f;
                QAngle targetHeadAngles{};
                QAngle finalHeadAngles{};
                QAngle hmdWorldAngles{};
                bool headAnglesValid = false;
                const bool hmdWorldAnglesValid =
                    hmdPoseValid &&
                    HooksViewmodelAutoGripMatrixAngles(
                        hmdWorld,
                        hmdWorldAngles);
                if (layout->neck >= 0 &&
                    layout->neck < layout->numBones &&
                    layout->head >= 0 &&
                    layout->head < layout->numBones)
                {
                    nativeNeckLength =
                        (vr_vm_stabilize::GetOrigin(
                            sourceBones[layout->head]) -
                         vr_vm_stabilize::GetOrigin(
                            sourceBones[layout->neck])).Length();
                    finalNeckLength =
                        (vr_vm_stabilize::GetOrigin(
                            bones[layout->head]) -
                         vr_vm_stabilize::GetOrigin(
                            bones[layout->neck])).Length();
                }
                if (hmdPoseValid &&
                    layout->head >= 0 &&
                    layout->head < layout->numBones)
                {
                    headAnglesValid =
                        HooksViewmodelAutoGripMatrixAngles(
                            headTarget,
                            targetHeadAngles) &&
                        HooksViewmodelAutoGripMatrixAngles(
                            bones[layout->head],
                            finalHeadAngles);
                }
                // Keep diagnostics in small type-homogeneous calls. Apart from
                // being easier to read, this avoids placing more than fifty
                // mixed x86 varargs on a game-owned render-worker stack.
                Game::logMsg(
                    "[VR][WorldPose] IK player=%d local=%d weight=%.2f tracking=%.2f freshness=%.2f suppressed=%d bodyYaw=%.1f->%.1f applied=%.1f error=%.1f turning=%d head=%d headChain=%d eyeNeutral=%d",
                    info.entity_index,
                    localPlayer ? 1 : 0,
                    weight,
                    trackingWeight,
                    freshness,
                    stateSuppressed ? 1 : 0,
                    sourceBodyYaw,
                    visualBodyYaw,
                    appliedBodyYaw,
                    visualBodyYawError,
                    visualBodyYawTurning ? 1 : 0,
                    hmdPoseValid ? 1 : 0,
                    headChainReferenceValid ? 1 : 0,
                    calibration.hmdToHeadUsesSourceEyeAngles
                        ? 1
                        : 0);
                Game::logMsg(
                    "[VR][WorldPose] IK rotations hmdLocal=(%.1f %.1f %.1f) hmdWorld=(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f) head=(%.1f %.1f %.1f)->(%.1f %.1f %.1f)",
                    pose.hmd.angles.x,
                    pose.hmd.angles.y,
                    pose.hmd.angles.z,
                    hmdWorldAnglesValid
                        ? hmdWorldAngles.x
                        : 0.0f,
                    hmdWorldAnglesValid
                        ? hmdWorldAngles.y
                        : 0.0f,
                    hmdWorldAnglesValid
                        ? hmdWorldAngles.z
                        : 0.0f,
                    sourceEyeAnglesValid
                        ? sourceEyeAngles.x
                        : 0.0f,
                    sourceEyeAnglesValid
                        ? sourceEyeAngles.y
                        : 0.0f,
                    sourceEyeAnglesValid
                        ? sourceEyeAngles.z
                        : 0.0f,
                    headAnglesValid ? targetHeadAngles.x : 0.0f,
                    headAnglesValid ? targetHeadAngles.y : 0.0f,
                    headAnglesValid ? targetHeadAngles.z : 0.0f,
                    headAnglesValid ? finalHeadAngles.x : 0.0f,
                    headAnglesValid ? finalHeadAngles.y : 0.0f,
                    headAnglesValid ? finalHeadAngles.z : 0.0f);
                Game::logMsg(
                    "[VR][WorldPose] IK head calibration samples=%u eyeSamples=%u complete=%d blend=%.2f token=%llu generation=%u",
                    static_cast<unsigned int>(
                        calibration.hmdToHeadSamples.count),
                    static_cast<unsigned int>(
                        calibration.headCalibrationEyeSampleCount),
                    calibration.headCalibrationComplete ? 1 : 0,
                    headTrackingWeight,
                    static_cast<unsigned long long>(
                        calibration.localThirdPersonWarmupToken),
                    static_cast<unsigned int>(
                        calibration.poseLockGeneration));
                Game::logMsg(
                    "[VR][WorldPose] IK offsets rawLocal=(%.1f %.1f %.1f) leanM=(%.3f %.3f) leanMaxM=%.3f neck=%.2f->%.2f",
                    rawHmdLocalDelta.x,
                    rawHmdLocalDelta.y,
                    rawHmdLocalDelta.z,
                    bodyLeanLocalMeters.x,
                    bodyLeanLocalMeters.y,
                    vr->m_BodyLeanMaxOffsetMeters,
                    nativeNeckLength,
                    finalNeckLength);
                Game::logMsg(
                    "[VR][WorldPose] IK arms solver=analytic leftTarget=%d leftSolved=%d leftContinuity=%d leftReachable=%d rightTarget=%d rightSolved=%d rightContinuity=%d rightReachable=%d shotWrist=%d shotTriggered=%d",
                    leftPoseValid ? 1 : 0,
                    leftArmSolved ? 1 : 0,
                    calibration.leftBendBodyLocalValid ? 1 : 0,
                    leftTargetWithinReach ? 1 : 0,
                    rightPoseValid ? 1 : 0,
                    rightArmSolved ? 1 : 0,
                    calibration.rightBendBodyLocalValid ? 1 : 0,
                    rightTargetWithinReach ? 1 : 0,
                    weaponGripRotationValid ? 1 : 0,
                    weaponShotTriggered ? 1 : 0);
                if (leftPoseValid)
                {
                    const Vector leftShoulder =
                        vr_vm_stabilize::GetOrigin(
                            bones[layout->left.upperArm]);
                    const Vector leftGoal =
                        vr_vm_stabilize::GetOrigin(leftHandTarget);
                    const Vector leftWrist =
                        vr_vm_stabilize::GetOrigin(
                            bones[layout->left.hand]);
                    const float leftWristError =
                        (leftWrist - leftGoal).Length();
                    Game::logMsg(
                        "[VR][WorldPose] IK left shoulder=(%.2f %.2f %.2f) wrist=(%.2f %.2f %.2f) goal=(%.2f %.2f %.2f) error=%.2f",
                        leftShoulder.x,
                        leftShoulder.y,
                        leftShoulder.z,
                        leftWrist.x,
                        leftWrist.y,
                        leftWrist.z,
                        leftGoal.x,
                        leftGoal.y,
                        leftGoal.z,
                        leftWristError);
                }
                if (rightPoseValid)
                {
                    const Vector rightShoulder =
                        vr_vm_stabilize::GetOrigin(
                            bones[layout->right.upperArm]);
                    const Vector rightGoal =
                        vr_vm_stabilize::GetOrigin(rightHandTarget);
                    const Vector rightWrist =
                        vr_vm_stabilize::GetOrigin(
                            bones[layout->right.hand]);
                    const float rightWristError =
                        (rightWrist - rightGoal).Length();
                    Game::logMsg(
                        "[VR][WorldPose] IK right shoulder=(%.2f %.2f %.2f) wrist=(%.2f %.2f %.2f) goal=(%.2f %.2f %.2f) error=%.2f",
                        rightShoulder.x,
                        rightShoulder.y,
                        rightShoulder.z,
                        rightWrist.x,
                        rightWrist.y,
                        rightWrist.z,
                        rightGoal.x,
                        rightGoal.y,
                        rightGoal.z,
                        rightWristError);
                }
            }
        }

        outBones = bones;
        return true;
    }
