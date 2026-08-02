    // ------------------------------------------------------------
    // Multiplayer VR world-model pose reconstruction
    //
    // Tracking samples remain player-yaw-local on the wire.  This renderer
    // resolves them against the current ModelRenderInfo_t, then layers a
    // visual-only head/upper-body/dual-arm solve over Source's final matrices.
    // Each tracked arm is solved once with ozz-animation's official
    // IKTwoBoneJob. Source locomotion remains the input pose, while the
    // controller supplies a deterministic wrist goal and an explicit
    // body-space elbow pole. Lower-body locomotion, collision bones, and
    // hitboxes remain completely native.
    // ------------------------------------------------------------
    struct HooksWorldPoseArmLayout
    {
        int clavicle = -1;
        int upperArm = -1;
        int forearm = -1;
        int hand = -1;
        bool palmToHandValid = false;
        bool midAxisValid = false;
        bool bindChainLocalValid = false;
        bool bindHandFromHeadValid = false;
        vr_vm_stabilize::Mat3x4 palmToHand{};
        Vector midAxisLocal{};
        vr_vm_stabilize::Mat3x4 bindUpperArmLocal{};
        vr_vm_stabilize::Mat3x4 bindForearmLocal{};
        vr_vm_stabilize::Mat3x4 bindHandLocal{};
        Vector bindHandFromHeadModel{};
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
        std::vector<int> parents;
    };

    struct HooksWorldPoseCalibration
    {
        const C_BaseEntity* entity = nullptr;
        const uint8_t* studioHdr = nullptr;
        bool hmdToHeadValid = false;
        bool leftPoleBodyLocalValid = false;
        bool rightPoleBodyLocalValid = false;
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
        bool nativeHeadReferenceLocalPositionValid = false;
        bool standingUpperBodyReferenceLocalPositionValid = false;
        bool standingUpperBodyReferenceSawDucking = false;
        float hmdReferenceBodyYaw = 0.0f;
        std::uint64_t standingUpperBodyReferenceCandidateSinceTickMs = 0u;
        Vector hmdReferenceLocalPosition{};
        Vector nativeHeadReferenceLocalPosition{};
        Vector standingUpperBodyReferenceLocalPosition{};
        vr_vm_stabilize::Mat3x4 hmdToHead{};
        Vector leftPoleBodyLocal{};
        Vector rightPoleBodyLocal{};
        vr_vm_stabilize::Mat3x4 rightGripPreviousLocal{};
        vr_vm_stabilize::Mat3x4 rightGripShotBaselineLocal{};
        Vector rightGripShotAxisHemisphere{};
        Vector rightGripShotImpulseAxis{};
        bool neckReferenceLocalValid = false;
        bool headReferenceLocalValid = false;
        bool leftArmBodyCarryReferenceValid = false;
        bool rightArmBodyCarryReferenceValid = false;
        vr_vm_stabilize::Mat3x4 neckReferenceLocal{};
        vr_vm_stabilize::Mat3x4 headReferenceLocal{};
        vr_vm_stabilize::Mat3x4 leftArmBodyCarryReference{};
        vr_vm_stabilize::Mat3x4 rightArmBodyCarryReference{};
        bool visualBodyYawValid = false;
        bool visualBodyYawTurning = false;
        float visualBodyYaw = 0.0f;
        std::uint64_t visualBodyYawTickMs = 0u;
        void Reset(const C_BaseEntity* newEntity, const uint8_t* newStudioHdr)
        {
            *this = HooksWorldPoseCalibration{};
            entity = newEntity;
            studioHdr = newStudioHdr;
        }
    };

    // Player-bone rendering can migrate between worker threads. Keep only the
    // genuinely temporal state (body yaw, pole continuity and shot impulse)
    // per player and serialize that player's world-pose solve. Wrist basis and
    // hinge axis are immutable bind-pose data stored in the bone layout.
    std::array<
        HooksWorldPoseCalibration,
        Game::kMaxPlayers> g_HooksWorldPoseCalibrations{};
    std::array<
        std::mutex,
        Game::kMaxPlayers> g_HooksWorldPoseCalibrationMutexes{};

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
        g_HooksWorldPoseOzzRuntimeDisabled{ false };
    std::atomic<bool>
        g_HooksWorldPoseOzzFaultLogged{ false };
    std::atomic<bool>
        g_HooksWorldPoseOzzFirstRunLogged{ false };
    std::atomic<bool>
        g_HooksWorldPoseOzzFirstRunCompletedLogged{ false };
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

    inline bool HooksWorldPoseBuildBindArmReference(
        const uint8_t* studioHdr,
        int studioLength,
        int boneTableOffset,
        int boneStride,
        int numBones,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& parents,
        int headBone,
        int side,
        HooksWorldPoseArmLayout& arm)
    {
        arm.palmToHandValid = false;
        arm.midAxisValid = false;
        arm.bindChainLocalValid = false;
        arm.bindHandFromHeadValid = false;
        // The caller has already validated ancestry. Keep this reader bounded
        // to the three joints required by IK.
        if (arm.upperArm < 0 ||
            arm.upperArm >= numBones ||
            arm.forearm < 0 ||
            arm.forearm >= numBones ||
            arm.hand < 0 ||
            arm.hand >= numBones)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 bindUpper{};
        vr_vm_stabilize::Mat3x4 bindForearm{};
        vr_vm_stabilize::Mat3x4 bindHand{};
        vr_vm_stabilize::Mat3x4 bindClavicle{};
        vr_vm_stabilize::Mat3x4 bindHead{};
        if (static_cast<int>(parents.size()) < numBones ||
            !HooksWorldPoseReadBindBoneModel(
                studioHdr,
                studioLength,
                boneTableOffset,
                boneStride,
                numBones,
                arm.clavicle,
                bindClavicle) ||
            !HooksWorldPoseReadBindBoneModel(
                studioHdr,
                studioLength,
                boneTableOffset,
                boneStride,
                numBones,
                arm.upperArm,
                bindUpper) ||
            !HooksWorldPoseReadBindBoneModel(
                studioHdr,
                studioLength,
                boneTableOffset,
                boneStride,
                numBones,
                arm.forearm,
                bindForearm) ||
            !HooksWorldPoseReadBindBoneModel(
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

        auto buildBindLocal = [&](int bone,
                                  const vr_vm_stabilize::Mat3x4& bindBone,
                                  vr_vm_stabilize::Mat3x4& outLocal)
            {
                if (bone < 0 || bone >= numBones)
                    return false;
                const int parent = parents[static_cast<size_t>(bone)];
                if (parent < 0 || parent >= numBones || parent == bone)
                    return false;
                vr_vm_stabilize::Mat3x4 bindParent{};
                vr_vm_stabilize::Mat3x4 inverseParent{};
                if (!HooksWorldPoseReadBindBoneModel(
                        studioHdr,
                        studioLength,
                        boneTableOffset,
                        boneStride,
                        numBones,
                        parent,
                        bindParent))
                {
                    return false;
                }
                vr_vm_stabilize::InvertTR(bindParent, inverseParent);
                vr_vm_stabilize::Mul(inverseParent, bindBone, outLocal);
                return HooksNativeViewmodelHandsOnlyMatrixFinite(outLocal);
            };
        // Keep the clavicle on Source's current torso pose. Restoring it to
        // the studio bind pose forces both shoulders back into the authored
        // T-pose as soon as calibration enables IK. Only the actual arm chain
        // is made deterministic before the two-bone solve.
        arm.bindChainLocalValid =
            buildBindLocal(
                arm.upperArm,
                bindUpper,
                arm.bindUpperArmLocal) &&
            buildBindLocal(
                arm.forearm,
                bindForearm,
                arm.bindForearmLocal) &&
            buildBindLocal(
                arm.hand,
                bindHand,
                arm.bindHandLocal);

        if (headBone >= 0 &&
            headBone < numBones &&
            HooksWorldPoseReadBindBoneModel(
                studioHdr,
                studioLength,
                boneTableOffset,
                boneStride,
                numBones,
                headBone,
                bindHead))
        {
            arm.bindHandFromHeadModel =
                vr_vm_stabilize::GetOrigin(bindHand) -
                vr_vm_stabilize::GetOrigin(bindHead);
            arm.bindHandFromHeadValid =
                HooksNativeViewmodelHandsOnlyVectorFinite(
                    arm.bindHandFromHeadModel) &&
                arm.bindHandFromHeadModel.LengthSqr() > 1.0f;
        }

        const Vector upperOrigin =
            vr_vm_stabilize::GetOrigin(bindUpper);
        const Vector forearmOrigin =
            vr_vm_stabilize::GetOrigin(bindForearm);
        const Vector handOrigin =
            vr_vm_stabilize::GetOrigin(bindHand);
        const Vector middleToStart = upperOrigin - forearmOrigin;
        const Vector middleToEnd = handOrigin - forearmOrigin;
        const float lengthProduct =
            middleToStart.Length() * middleToEnd.Length();
        Vector axisModel =
            CrossProduct(middleToStart, middleToEnd);
        float axisLength = axisModel.Length();
        if (!std::isfinite(lengthProduct) ||
            !std::isfinite(axisLength) ||
            lengthProduct <= 0.0001f ||
            axisLength <= lengthProduct * 0.02f)
        {
            Vector bindChainDirection = handOrigin - upperOrigin;
            if (VectorNormalize(bindChainDirection) == 0.0f)
                return false;
            Vector anatomicalPole(
                0.15f,
                -static_cast<float>(side),
                -0.45f);
            anatomicalPole -=
                bindChainDirection *
                DotProduct(anatomicalPole, bindChainDirection);
            if (VectorNormalize(anatomicalPole) == 0.0f)
                return false;
            axisModel =
                CrossProduct(bindChainDirection, anatomicalPole);
            axisLength = axisModel.Length();
        }
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(axisModel) ||
            !std::isfinite(axisLength) ||
            VectorNormalize(axisModel) == 0.0f)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 inverseForearm{};
        vr_vm_stabilize::InvertTR(
            bindForearm,
            inverseForearm);
        arm.midAxisLocal = Vector(
            inverseForearm.m[0][0] * axisModel.x +
                inverseForearm.m[0][1] * axisModel.y +
                inverseForearm.m[0][2] * axisModel.z,
            inverseForearm.m[1][0] * axisModel.x +
                inverseForearm.m[1][1] * axisModel.y +
                inverseForearm.m[1][2] * axisModel.z,
            inverseForearm.m[2][0] * axisModel.x +
                inverseForearm.m[2][1] * axisModel.y +
                inverseForearm.m[2][2] * axisModel.z);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                arm.midAxisLocal) ||
            VectorNormalize(arm.midAxisLocal) == 0.0f)
        {
            return false;
        }
        arm.midAxisValid = true;

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

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(
            studioHdr + 0x4C,
            studioLength);
        if (HooksWorldPoseArmChainValid(
                layout.left,
                layout.parents,
                layout.numBones))
        {
            HooksWorldPoseBuildBindArmReference(
                studioHdr,
                studioLength,
                boneIndex,
                stride,
                layout.numBones,
                boneNames,
                layout.parents,
                layout.head,
                -1,
                layout.left);
        }
        if (HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones))
        {
            HooksWorldPoseBuildBindArmReference(
                studioHdr,
                studioLength,
                boneIndex,
                stride,
                layout.numBones,
                boneNames,
                layout.parents,
                layout.head,
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
                layout.numBones) &&
            layout.left.midAxisValid;
        const bool rightValid =
            HooksWorldPoseArmChainValid(
                layout.right,
                layout.parents,
                layout.numBones) &&
            layout.right.midAxisValid;
        layout.valid = headValid || leftValid || rightValid;

        if (layout.valid)
        {
            Game::logMsg(
                "[VR][WorldPose] bone layout ready hdr=%p bones=%d head=%d neck=%d chest=%d left=(%d %d %d %d bindPalm=%d bindAxis=%d) right=(%d %d %d %d bindPalm=%d bindAxis=%d)",
                layout.studioHdr,
                layout.numBones,
                layout.head,
                layout.neck,
                layout.upperChest,
                layout.left.clavicle,
                layout.left.upperArm,
                layout.left.forearm,
                layout.left.hand,
                layout.left.palmToHandValid ? 1 : 0,
                layout.left.midAxisValid ? 1 : 0,
                layout.right.clavicle,
                layout.right.upperArm,
                layout.right.forearm,
                layout.right.hand,
                layout.right.palmToHandValid ? 1 : 0,
                layout.right.midAxisValid ? 1 : 0);
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
                    layout.left,
                    layout.parents,
                    layout.numBones) &&
                !layout.left.midAxisValid)
            {
                Game::logMsg(
                    "[VR][WorldPose] left bind hinge axis unavailable; arm IK disabled");
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
            if (HooksWorldPoseArmChainValid(
                    layout.right,
                    layout.parents,
                    layout.numBones) &&
                !layout.right.midAxisValid)
            {
                Game::logMsg(
                    "[VR][WorldPose] right bind hinge axis unavailable; arm IK disabled");
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

    inline void HooksWorldPosePublishWeaponHandState(
        VR* vr,
        Game* game,
        const C_BaseEntity* playerEntity,
        int playerIndex,
        const ModelRenderInfo_t& info,
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const vr_vm_stabilize::Mat3x4* finalBones,
        const vr_vm_stabilize::Mat3x4& rightControllerWorld,
        bool rightHandPoseValid)
    {
        if (!game ||
            !playerEntity ||
            !sourceBones ||
            !finalBones ||
            playerIndex <= 0 ||
            !game->IsValidPlayerIndex(playerIndex) ||
            !rightHandPoseValid ||
            layout.right.hand < 0 ||
            layout.right.hand >= layout.numBones)
        {
            return;
        }

        C_BaseCombatWeapon* activeWeapon = nullptr;
        void* activeWeaponRenderable = nullptr;
        if (!HooksWorldPoseGetActiveWeaponSafe(
                playerEntity,
                activeWeapon,
                activeWeaponRenderable) ||
            !activeWeaponRenderable)
        {
            return;
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
            return;
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
            return;
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
            return;
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
            return;
        }

        vr_vm_stabilize::Mat3x4 controllerRigid{};
        vr_vm_stabilize::Mat3x4 inverseController{};
        vr_vm_stabilize::Mat3x4 controllerToFinalHand{};
        const bool controllerToFinalHandValid =
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
                    "[VR][WorldPoseWeapon] hand cache player=%d weapon=%p weaponIndex=%d renderable=%p worldModel=%p displacement=%.1f",
                    playerIndex,
                    activeWeapon,
                    published.weaponEntityIndex,
                    activeWeaponRenderable,
                    published.weaponWorldModel,
                    handDisplacement);
            }
        }
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
            !vr->m_WorldModelVRPoseEnabled)
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
            !vr->m_WorldModelVRPoseEnabled)
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

    inline bool HooksWorldPoseEnsureHeadOrientationCalibration(
        const vr_vm_stabilize::Mat3x4& hmdWorld,
        const vr_vm_stabilize::Mat3x4& sourceHead,
        bool& calibrationValid,
        vr_vm_stabilize::Mat3x4& hmdToHead)
    {
        if (!calibrationValid)
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
                hmdToHead);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    hmdToHead))
            {
                return false;
            }

            // Head position is constrained separately against Source's native
            // head. This matrix only retains the stable HMD-to-head rotation.
            hmdToHead.m[0][3] = 0.0f;
            hmdToHead.m[1][3] = 0.0f;
            hmdToHead.m[2][3] = 0.0f;
            calibrationValid = true;
        }
        return true;
    }

    inline bool HooksWorldPoseBuildStaticHandTarget(
        const HooksWorldPoseArmLayout& arm,
        const vr_vm_stabilize::Mat3x4& controllerWorld,
        const vr_vm_stabilize::Mat3x4& currentHand,
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
            // still receive deterministic positional IK, but we deliberately
            // do not learn an arbitrary walk/fire/deploy wrist rotation from
            // the first rendered frame.
            if (!HooksWorldPoseBuildRigidBoneTransform(
                    currentHand,
                    outHandTarget))
            {
                return false;
            }
            outHandTarget.m[0][3] = controllerOrigin.x;
            outHandTarget.m[1][3] = controllerOrigin.y;
            outHandTarget.m[2][3] = controllerOrigin.z;
        }

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

    inline bool HooksWorldPoseTranslateBranch(
        const HooksWorldPoseBoneLayout& layout,
        int rootBone,
        Vector translation,
        float maxDistance,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(translation))
            return false;
        const float distance = translation.Length();
        if (!std::isfinite(distance) || distance <= 0.001f)
            return false;
        if (maxDistance > 0.0f && distance > maxDistance)
            translation *= maxDistance / distance;

        vr_vm_stabilize::Mat3x4 delta =
            vr_vm_stabilize::Identity();
        delta.m[0][3] = translation.x;
        delta.m[1][3] = translation.y;
        delta.m[2][3] = translation.z;
        return HooksWorldPoseApplyDeltaToBranch(
            layout,
            rootBone,
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

    inline bool HooksWorldPoseEnsureHeadChainReference(
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        HooksWorldPoseCalibration& calibration)
    {
        if (calibration.neckReferenceLocalValid &&
            calibration.headReferenceLocalValid)
        {
            return true;
        }
        if (layout.neck < 0 ||
            layout.neck >= layout.numBones ||
            layout.head < 0 ||
            layout.head >= layout.numBones)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 neckLocal{};
        vr_vm_stabilize::Mat3x4 headLocal{};
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
            return false;
        }

        calibration.neckReferenceLocal =
            neckLocal;
        calibration.headReferenceLocal =
            headLocal;
        calibration.neckReferenceLocalValid = true;
        calibration.headReferenceLocalValid = true;
        return true;
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

    inline bool HooksWorldPoseApplyBodyCarryToArmTarget(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        const vr_vm_stabilize::Mat3x4& bodyFrame,
        const vr_vm_stabilize::Mat3x4* bones,
        bool allowReferenceCapture,
        bool& referenceValid,
        vr_vm_stabilize::Mat3x4& bodyLocalReference,
        vr_vm_stabilize::Mat3x4& inOutHandTarget)
    {
        if (!bones ||
            arm.clavicle < 0 ||
            arm.clavicle >= layout.numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(bodyFrame) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(inOutHandTarget))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 currentRoot{};
        if (!HooksWorldPoseBuildRigidBoneTransform(
                bones[arm.clavicle],
                currentRoot))
        {
            return false;
        }

        if (!referenceValid)
        {
            if (!allowReferenceCapture)
                return false;
            vr_vm_stabilize::Mat3x4 inverseBody{};
            if (!vr_vm_stabilize::InvertAffine(
                    bodyFrame,
                    inverseBody))
            {
                return false;
            }
            vr_vm_stabilize::Mul(
                inverseBody,
                currentRoot,
                bodyLocalReference);
            referenceValid =
                HooksNativeViewmodelHandsOnlyMatrixFinite(
                    bodyLocalReference);
            if (!referenceValid)
                return false;
        }

        // Keep the clavicle attached to Source's animated torso. Measure its
        // current native rigid motion relative to the captured body-local
        // shoulder, then carry the wrist goal by that exact same delta. Ozz
        // subsequently solves the bind upper/forearm chain between the live
        // shoulder and carried wrist, so body motion reaches the whole arm
        // instead of stopping at the upper arm.
        vr_vm_stabilize::Mat3x4 referenceWorld{};
        vr_vm_stabilize::Mat3x4 inverseReference{};
        vr_vm_stabilize::Mat3x4 bodyCarryDelta{};
        vr_vm_stabilize::Mat3x4 carriedTarget{};
        vr_vm_stabilize::Mul(
            bodyFrame,
            bodyLocalReference,
            referenceWorld);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(referenceWorld) ||
            !vr_vm_stabilize::InvertAffine(
                referenceWorld,
                inverseReference))
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            currentRoot,
            inverseReference,
            bodyCarryDelta);
        const Vector shoulderDisplacement =
            vr_vm_stabilize::GetOrigin(currentRoot) -
            vr_vm_stabilize::GetOrigin(referenceWorld);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(bodyCarryDelta) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(
                shoulderDisplacement) ||
            shoulderDisplacement.LengthSqr() > 4096.0f)
        {
            return false;
        }
        vr_vm_stabilize::Mul(
            bodyCarryDelta,
            inOutHandTarget,
            carriedTarget);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(carriedTarget))
            return false;
        inOutHandTarget = carriedTarget;
        return true;
    }

    inline bool HooksWorldPoseRestoreHeadChain(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseCalibration& calibration,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!calibration.neckReferenceLocalValid ||
            !calibration.headReferenceLocalValid)
        {
            return false;
        }

        bool changed =
            HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                layout.neck,
                calibration.neckReferenceLocal,
                bones);
        changed =
            HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                layout.head,
                calibration.headReferenceLocal,
                bones) ||
            changed;
        return changed;
    }

    inline bool HooksWorldPoseRestoreBindArmChain(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones ||
            !arm.bindChainLocalValid ||
            layout.numBones <= 0 ||
            layout.numBones > 512)
        {
            return false;
        }

        std::vector<vr_vm_stabilize::Mat3x4> staged(
            bones,
            bones + layout.numBones);
        if (!HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                arm.upperArm,
                arm.bindUpperArmLocal,
                staged.data()) ||
            !HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                arm.forearm,
                arm.bindForearmLocal,
                staged.data()) ||
            !HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                arm.hand,
                arm.bindHandLocal,
                staged.data()))
        {
            return false;
        }
        std::copy(staged.begin(), staged.end(), bones);
        return true;
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

    // The Source render hook is entered from game-owned x86 worker stacks and
    // custom survivor models can contain unusual scale/shear. Keep the foreign
    // SIMD call behind its own SEH frame so a bad model can disable VR arm IK
    // without terminating the game. This wrapper deliberately owns no C++
    // objects that require unwinding; the caller's mutex/vector lifetimes then
    // finish normally after a failed job.
#ifdef _MSC_VER
    __declspec(noinline) bool HooksWorldPoseRunOzzTwoBoneJobGuarded(
        const ozz::animation::IKTwoBoneJob* job,
        unsigned long& outExceptionCode)
    {
        outExceptionCode = 0ul;
        if (!job)
            return false;
        __try
        {
            return job->Run();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outExceptionCode = static_cast<unsigned long>(
                GetExceptionCode());
            return false;
        }
    }
#else
    inline bool HooksWorldPoseRunOzzTwoBoneJobGuarded(
        const ozz::animation::IKTwoBoneJob* job,
        unsigned long& outExceptionCode)
    {
        outExceptionCode = 0ul;
        return job && job->Run();
    }
#endif

    inline bool HooksWorldPoseProjectPoleVector(
        const Vector& candidate,
        const Vector& targetDirection,
        Vector& outPole)
    {
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(candidate) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(targetDirection))
        {
            return false;
        }
        outPole =
            candidate -
            targetDirection *
                DotProduct(candidate, targetDirection);
        const float poleLength = outPole.Length();
        if (!std::isfinite(poleLength) ||
            poleLength <= 0.02f)
        {
            return false;
        }
        outPole *= 1.0f / poleLength;
        return true;
    }

    inline float HooksWorldPoseTwoBoneDistanceForAngle(
        float upperLength,
        float lowerLength,
        float elbowAngleRadians)
    {
        const float distanceSquared =
            upperLength * upperLength +
            lowerLength * lowerLength -
            2.0f * upperLength * lowerLength *
                std::cos(elbowAngleRadians);
        return std::sqrt(std::max(distanceSquared, 0.0f));
    }

    inline bool HooksWorldPoseConstrainArmTarget(
        int side,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp,
        const Vector& start,
        const Vector& middle,
        const Vector& end,
        const Vector& rawTarget,
        Vector& outTarget,
        float& outArmLength)
    {
        outArmLength = 0.0f;
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(start) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(middle) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(end) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(rawTarget))
        {
            return false;
        }

        const float upperLength = (middle - start).Length();
        const float lowerLength = (end - middle).Length();
        const float armLength = upperLength + lowerLength;
        if (!std::isfinite(upperLength) ||
            !std::isfinite(lowerLength) ||
            !std::isfinite(armLength) ||
            upperLength <= 0.5f ||
            lowerLength <= 0.5f ||
            armLength <= 1.0f)
        {
            return false;
        }
        outArmLength = armLength;

        const Vector anatomicalOutward =
            bodyRight * static_cast<float>(side);
        const Vector rawDelta = rawTarget - start;
        float forward = DotProduct(rawDelta, bodyForward);
        float outward = DotProduct(rawDelta, anatomicalOutward);
        const float up = DotProduct(rawDelta, bodyUp);

        // Shoulder workspace. Crossing the chest is legal, but reaching far
        // through the opposite shoulder or simultaneously behind the back and
        // across the torso is not. Keeping this in body space also makes the
        // limits rotate atomically with snap turns.
        constexpr float kMaxAcrossRatio = 0.48f;
        constexpr float kMaxBehindRatio = 0.24f;
        const float maxAcross = armLength * kMaxAcrossRatio;
        const float maxBehind = armLength * kMaxBehindRatio;
        outward = std::max(outward, -maxAcross);
        forward = std::max(forward, -maxBehind);
        if (outward < 0.0f && forward < 0.0f)
        {
            const float acrossUnit = -outward / maxAcross;
            const float behindUnit = -forward / maxBehind;
            const float combined = std::sqrt(
                acrossUnit * acrossUnit +
                behindUnit * behindUnit);
            if (std::isfinite(combined) && combined > 1.0f)
            {
                outward /= combined;
                forward /= combined;
            }
        }

        Vector constrainedDelta =
            bodyForward * forward +
            anatomicalOutward * outward +
            bodyUp * up;
        float targetDistance = constrainedDelta.Length();
        if (!std::isfinite(targetDistance))
            return false;

        // The ozz job preserves both bone lengths, but without a caller-side
        // joint limit it can still approach the two singular humanly invalid
        // states: a folded-back elbow or a perfectly locked elbow. Project the
        // wrist onto the distances produced by a 18..165 degree elbow angle.
        constexpr float kDegreesToRadians =
            0.01745329251994329577f;
        const float minDistance =
            HooksWorldPoseTwoBoneDistanceForAngle(
                upperLength,
                lowerLength,
                18.0f * kDegreesToRadians);
        const float maxDistance =
            HooksWorldPoseTwoBoneDistanceForAngle(
                upperLength,
                lowerLength,
                165.0f * kDegreesToRadians);
        if (!std::isfinite(minDistance) ||
            !std::isfinite(maxDistance) ||
            minDistance <= 0.0f ||
            maxDistance <= minDistance)
        {
            return false;
        }

        if (targetDistance <= 0.001f)
        {
            constrainedDelta =
                anatomicalOutward * 0.55f +
                bodyForward * 0.65f -
                bodyUp * 0.25f;
            if (VectorNormalize(constrainedDelta) == 0.0f)
                return false;
            constrainedDelta *= minDistance;
        }
        else if (targetDistance < minDistance ||
                 targetDistance > maxDistance)
        {
            const float legalDistance = std::clamp(
                targetDistance,
                minDistance,
                maxDistance);
            constrainedDelta *= legalDistance / targetDistance;
        }

        outTarget = start + constrainedDelta;
        return HooksNativeViewmodelHandsOnlyVectorFinite(outTarget);
    }

    inline bool HooksWorldPoseBuildExplicitPoleVector(
        int side,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp,
        const Vector& start,
        const Vector& target,
        float armLength,
        bool& cachedPoleBodyLocalValid,
        Vector& cachedPoleBodyLocal,
        Vector& outPole)
    {
        Vector targetDirection = target - start;
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(targetDirection) ||
            VectorNormalize(targetDirection) == 0.0f)
        {
            return false;
        }

        const Vector anatomicalOutward =
            bodyRight * static_cast<float>(side);
        if (!std::isfinite(armLength) || armLength <= 1.0f)
            return false;

        const Vector targetDelta = target - start;
        const float targetOutward =
            DotProduct(targetDelta, anatomicalOutward) / armLength;
        const float targetUp =
            DotProduct(targetDelta, bodyUp) / armLength;
        const float crossBody = std::clamp(
            -targetOutward / 0.48f,
            0.0f,
            1.0f);
        const float raised = std::clamp(
            targetUp / 0.80f,
            0.0f,
            1.0f);
        const float lowered = std::clamp(
            -targetUp / 0.80f,
            0.0f,
            1.0f);

        // The elbow intent follows the wrist region. A cross-body wrist gets
        // a stronger outward/forward elbow, while an overhead wrist sheds the
        // fixed downward bias. This keeps the elbow on its anatomical side
        // instead of reusing a pole hemisphere from an unrelated pose.
        const Vector desiredPole =
            anatomicalOutward * (1.10f + 0.85f * crossBody) +
            bodyForward *
                (0.32f + 0.48f * crossBody + 0.28f * raised) -
            bodyUp *
                (0.34f + 0.18f * lowered - 0.24f * raised);
        const Vector desiredPoleProjectedRaw =
            desiredPole -
            targetDirection *
                DotProduct(desiredPole, targetDirection);
        const float desiredPoleLength = desiredPole.Length();
        const float desiredProjectionRatio =
            desiredPoleLength > 0.0001f
                ? desiredPoleProjectedRaw.Length() / desiredPoleLength
                : 0.0f;
        // Persist the previous pole in body-local coordinates. That lets a
        // snap turn rotate the cached direction with the survivor instead of
        // pinning the elbow to an obsolete world-space hemisphere.
        Vector cachedPoleWorld{};
        Vector cachedPoleProjected{};
        bool cachedPoleProjectedValid = false;
        if (cachedPoleBodyLocalValid &&
            HooksNativeViewmodelHandsOnlyVectorFinite(
                cachedPoleBodyLocal))
        {
            cachedPoleWorld =
                bodyForward * cachedPoleBodyLocal.x +
                bodyRight * cachedPoleBodyLocal.y +
                bodyUp * cachedPoleBodyLocal.z;
            cachedPoleProjectedValid =
                HooksWorldPoseProjectPoleVector(
                    cachedPoleWorld,
                    targetDirection,
                    cachedPoleProjected);
        }

        if (!HooksWorldPoseProjectPoleVector(
                desiredPole,
                targetDirection,
                outPole))
        {
            if (!cachedPoleProjectedValid)
            {
                if (!HooksWorldPoseProjectPoleVector(
                        anatomicalOutward,
                        targetDirection,
                        outPole) &&
                    !HooksWorldPoseProjectPoleVector(
                        bodyUp * -1.0f,
                        targetDirection,
                        outPole) &&
                    !HooksWorldPoseProjectPoleVector(
                        bodyForward,
                        targetDirection,
                        outPole))
                {
                    return false;
                }
            }
            else
            {
                outPole = cachedPoleProjected;
            }
        }

        if (cachedPoleProjectedValid &&
            std::isfinite(desiredProjectionRatio))
        {
            // Inside the near-alignment cone the projected anatomical vector
            // amplifies tiny target noise. The cache is used only in that
            // cone; outside it the current anatomical intent wins, so a hand
            // crossing the chest cannot drag an obsolete pole to the back.
            constexpr float kPoleHoldProjectionRatio = 0.08f;
            constexpr float kPoleReleaseProjectionRatio = 0.14f;
            if (desiredProjectionRatio <=
                kPoleHoldProjectionRatio)
            {
                outPole = cachedPoleProjected;
            }
            else if (desiredProjectionRatio <
                     kPoleReleaseProjectionRatio)
            {
                const float release =
                    (desiredProjectionRatio -
                     kPoleHoldProjectionRatio) /
                    (kPoleReleaseProjectionRatio -
                     kPoleHoldProjectionRatio);
                if (DotProduct(
                        cachedPoleProjected,
                        outPole) >= 0.0f)
                {
                    outPole =
                        cachedPoleProjected * (1.0f - release) +
                        outPole * release;
                }
                if (VectorNormalize(outPole) == 0.0f)
                    return false;
            }
        }

        cachedPoleBodyLocal = Vector(
            DotProduct(outPole, bodyForward),
            DotProduct(outPole, bodyRight),
            DotProduct(outPole, bodyUp));
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                cachedPoleBodyLocal) ||
            VectorNormalize(cachedPoleBodyLocal) == 0.0f)
        {
            cachedPoleBodyLocalValid = false;
            return false;
        }
        cachedPoleBodyLocalValid = true;
        return true;
    }

    inline bool HooksWorldPoseSolveArmWithOzz(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp,
        const vr_vm_stabilize::Mat3x4& handTarget,
        float weight,
        bool& poleBodyLocalValid,
        Vector& poleBodyLocal,
        bool& outReached,
        vr_vm_stabilize::Mat3x4* bones)
    {
        outReached = false;
        weight = std::clamp(weight, 0.0f, 1.0f);
        if (!bones ||
            g_HooksWorldPoseOzzRuntimeDisabled.load(
                std::memory_order_acquire) ||
            weight <= 0.0001f ||
            !HooksWorldPoseArmChainValid(
                arm,
                layout.parents,
                layout.numBones) ||
            !arm.midAxisValid ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(
                arm.midAxisLocal) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(handTarget))
        {
            return false;
        }

        const Vector start =
            vr_vm_stabilize::GetOrigin(bones[arm.upperArm]);
        const Vector middle =
            vr_vm_stabilize::GetOrigin(bones[arm.forearm]);
        const Vector end =
            vr_vm_stabilize::GetOrigin(bones[arm.hand]);
        const Vector rawTarget =
            vr_vm_stabilize::GetOrigin(handTarget);
        Vector target{};
        float armLength = 0.0f;
        Vector explicitPole{};
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(start) ||
            !HooksWorldPoseConstrainArmTarget(
                side,
                bodyForward,
                bodyRight,
                bodyUp,
                start,
                middle,
                end,
                rawTarget,
                target,
                armLength) ||
            !HooksWorldPoseBuildExplicitPoleVector(
                side,
                bodyForward,
                bodyRight,
                bodyUp,
                start,
                target,
                armLength,
                poleBodyLocalValid,
                poleBodyLocal,
                explicitPole))
        {
            return false;
        }
        vr_vm_stabilize::Mat3x4 startLocal{};
        vr_vm_stabilize::Mat3x4 middleLocal{};
        if (!HooksWorldPoseCaptureBoneLocalTransform(
                layout,
                bones,
                arm.upperArm,
                startLocal) ||
            !HooksWorldPoseCaptureBoneLocalTransform(
                layout,
                bones,
                arm.forearm,
                middleLocal))
        {
            return false;
        }

        // IKTwoBoneJob inverts these matrices. Source permits model-authored
        // scale/shear in final bone matrices, while ozz expects ordinary model
        // transforms. Strip scale/shear before the SIMD inversion so a valid
        // position chain cannot become singular only because of skinning data.
        vr_vm_stabilize::Mat3x4 startJointRigid{};
        vr_vm_stabilize::Mat3x4 middleJointRigid{};
        vr_vm_stabilize::Mat3x4 endJointRigid{};
        if (!HooksViewmodelAutoGripNormalizeRigidMatrix(
                bones[arm.upperArm],
                startJointRigid) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                bones[arm.forearm],
                middleJointRigid) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                bones[arm.hand],
                endJointRigid))
        {
            return false;
        }
        const ozz::math::Float4x4 startJoint =
            HooksWorldPoseToOzzMatrix(startJointRigid);
        const ozz::math::Float4x4 middleJoint =
            HooksWorldPoseToOzzMatrix(middleJointRigid);
        const ozz::math::Float4x4 endJoint =
            HooksWorldPoseToOzzMatrix(endJointRigid);
        ozz::math::SimdQuaternion startCorrection =
            ozz::math::SimdQuaternion::identity();
        ozz::math::SimdQuaternion middleCorrection =
            ozz::math::SimdQuaternion::identity();

        ozz::animation::IKTwoBoneJob job;
        job.target = ozz::math::simd_float4::Load(
            target.x,
            target.y,
            target.z,
            1.0f);
        job.mid_axis = ozz::math::simd_float4::Load(
            arm.midAxisLocal.x,
            arm.midAxisLocal.y,
            arm.midAxisLocal.z,
            0.0f);
        job.pole_vector = ozz::math::simd_float4::Load(
            explicitPole.x,
            explicitPole.y,
            explicitPole.z,
            0.0f);
        job.twist_angle = 0.0f;
        // A tracked wrist must remain attached to its controller. Soften IK is
        // useful for authored animation, but its intentional fall-behind near
        // full extension looks like locomotion-driven hand pumping in VR.
        job.soften = 1.0f;
        job.weight = weight;
        job.start_joint = &startJoint;
        job.mid_joint = &middleJoint;
        job.end_joint = &endJoint;
        job.start_joint_correction = &startCorrection;
        job.mid_joint_correction = &middleCorrection;
        job.reached = &outReached;

        // Run() reports parameter validity. reached is deliberately not used
        // as a fallback condition: softening and partial weight legitimately
        // set it to false while still returning the correct stable rotations.
        if (!g_HooksWorldPoseOzzFirstRunLogged.exchange(
                true,
                std::memory_order_acq_rel))
        {
            Game::logMsg(
                "[VR][WorldPose] ozz IK first run begin side=%d weight=%.3f",
                side,
                weight);
        }
        unsigned long ozzExceptionCode = 0ul;
        if (!HooksWorldPoseRunOzzTwoBoneJobGuarded(
                &job,
                ozzExceptionCode))
        {
            if (ozzExceptionCode != 0ul)
            {
                g_HooksWorldPoseOzzRuntimeDisabled.store(
                    true,
                    std::memory_order_release);
                bool expected = false;
                if (g_HooksWorldPoseOzzFaultLogged.compare_exchange_strong(
                        expected,
                        true,
                        std::memory_order_acq_rel))
                {
                    Game::logMsg(
                        "[VR][WorldPose] ozz IK runtime exception=0x%08lX; arm IK disabled for this process",
                        ozzExceptionCode);
                }
            }
            return false;
        }
        if (!g_HooksWorldPoseOzzFirstRunCompletedLogged.exchange(
                true,
                std::memory_order_acq_rel))
        {
            Game::logMsg(
                "[VR][WorldPose] ozz IK first run completed reached=%d",
                outReached ? 1 : 0);
        }

        vr_vm_stabilize::Mat3x4 startCorrectionMatrix{};
        vr_vm_stabilize::Mat3x4 middleCorrectionMatrix{};
        vr_vm_stabilize::Mat3x4 correctedStartLocal{};
        vr_vm_stabilize::Mat3x4 correctedMiddleLocal{};
        if (!HooksWorldPoseOzzCorrectionToMatrix(
                startCorrection,
                startCorrectionMatrix) ||
            !HooksWorldPoseOzzCorrectionToMatrix(
                middleCorrection,
                middleCorrectionMatrix))
        {
            return false;
        }

        // This is the exact order used by the official sample:
        // local_rotation = local_rotation * correction. Restoring each local
        // matrix propagates that correction through every intervening twist,
        // wrist, palm and finger bone.
        vr_vm_stabilize::Mul(
            startLocal,
            startCorrectionMatrix,
            correctedStartLocal);
        vr_vm_stabilize::Mul(
            middleLocal,
            middleCorrectionMatrix,
            correctedMiddleLocal);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                correctedStartLocal) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                correctedMiddleLocal))
        {
            return false;
        }

        // Stage the complete chain off to the side. Branch propagation can
        // touch many twist/palm/finger descendants; publishing the shoulder
        // before a later forearm or wrist failure would leave a finite but
        // anatomically half-solved arm in the stereo cache.
        std::vector<vr_vm_stabilize::Mat3x4> stagedBones(
            bones,
            bones + layout.numBones);
        if (!HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                arm.upperArm,
                correctedStartLocal,
                stagedBones.data()) ||
            !HooksWorldPoseRestoreBoneLocalTransform(
                layout,
                arm.forearm,
                correctedMiddleLocal,
                stagedBones.data()))
        {
            return false;
        }

        // IKTwoBoneJob solves end position only. Controller orientation is a
        // separate final wrist operation and keeps the solver independent of
        // weapon/deploy/walk animation state.
        if (!HooksWorldPoseOrientBranch(
                layout,
                arm.hand,
                handTarget,
                weight,
                stagedBones.data()))
        {
            return false;
        }
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
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
            info.entity_index <= 0 ||
            !game->IsValidPlayerIndex(info.entity_index))
        {
            return false;
        }

        const int localPlayerIndex =
            game->m_EngineClient
                ? game->m_EngineClient->GetLocalPlayer()
                : -1;
        const bool localPlayer =
            info.entity_index == localPlayerIndex;
        WorldModelVRPoseCalibrationSnapshot localCalibration{};
        const bool localCalibrationValid =
            !localPlayer ||
            vr->GetWorldModelVRPoseCalibrationSnapshot(localCalibration);
        if (!localCalibrationValid)
        {
            return false;
        }
        const bool localPoseAllowed =
            !localPlayer ||
            (vr->m_WorldModelVRPoseLocalThirdPerson &&
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
                        "[VR][WorldPose] pre-IK skip player=%d local=%d hasPose=%d freshness=%.2f weight=%.2f thirdPerson=%d firstPersonBodyEye=%d localAllowed=%d suppressed=0x%08X",
                        info.entity_index,
                        localPlayer ? 1 : 0,
                        hasPose ? 1 : 0,
                        freshness,
                        weight,
                        vr->m_IsThirdPersonCamera ? 1 : 0,
                        localFirstPersonEyeActive ? 1 : 0,
                        localPoseAllowed ? 1 : 0,
                        static_cast<unsigned int>(
                            suppressionReasonMask));
                }
            }
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
            return false;

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
                return false;
            }
        }

        std::unique_lock<std::mutex> calibrationLock(
            g_HooksWorldPoseCalibrationMutexes[
                static_cast<size_t>(info.entity_index)]);
        // Another render worker may have completed this player while we were
        // waiting for its persistent calibration. Reuse that exact result.
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
        if (calibration.entity != entity ||
            calibration.studioHdr != layout->studioHdr)
        {
            calibration.Reset(
                entity,
                layout->studioHdr);
        }
        const bool headChainReferenceValid =
            HooksWorldPoseEnsureHeadChainReference(
                *layout,
                sourceBones,
                calibration);

        vr_vm_stabilize::Mat3x4 hmdWorld{};
        vr_vm_stabilize::Mat3x4 leftControllerWorld{};
        vr_vm_stabilize::Mat3x4 rightControllerWorld{};
        vr_vm_stabilize::Mat3x4 headTarget{};
        vr_vm_stabilize::Mat3x4 leftHandTarget{};
        vr_vm_stabilize::Mat3x4 rightHandTarget{};
        QAngle sourceEyeAngles{};
        bool sourceEyeAnglesValid = false;
        Vector rawHmdLocalDelta(0.0f, 0.0f, 0.0f);
        Vector nativeHeadLocalDelta(0.0f, 0.0f, 0.0f);
        Vector nativeHeadCompensation(0.0f, 0.0f, 0.0f);
        Vector appliedHmdLocalDelta(0.0f, 0.0f, 0.0f);
        Vector appliedHmdWorldDelta(0.0f, 0.0f, 0.0f);
        bool playerDucking = false;
        HooksFirstPersonBodyReadCrouchedSafe(
            reinterpret_cast<C_BasePlayer*>(
                const_cast<C_BaseEntity*>(entity)),
            &playerDucking);

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
            // Match the local rendered torso to the same world yaw already
            // used by the tracked head and hands. This intentionally includes
            // physical HMD yaw as well as snap/smooth-turn body yaw.
            targetVisualBodyYaw =
                HooksWorldPoseWrapAngle(
                    pose.bodyYaw +
                    pose.hmd.angles.y);
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
            if (!calibration.hmdToHeadValid)
            {
                if (sourceEyeAnglesValid &&
                    HooksWorldPoseBuildEyeToHeadCalibration(
                        sourceEyeAngles,
                        sourceBones[layout->head],
                        calibration.hmdToHead))
                {
                    calibration.hmdToHeadValid = true;
                    calibration.hmdToHeadUsesSourceEyeAngles = true;
                }
                else
                {
                    // Guarded compatibility fallback for unusual servers
                    // where m_angEyeAngles is unavailable. The supported L4D2
                    // path above is deterministic and does not learn an
                    // animated first-frame head pose.
                    const bool calibrationReady =
                        HooksWorldPoseEnsureHeadOrientationCalibration(
                            hmdWorld,
                            sourceBones[layout->head],
                            calibration.hmdToHeadValid,
                            calibration.hmdToHead);
                    if (calibrationReady &&
                        calibration.hmdToHeadValid)
                    {
                        calibration.hmdToHeadUsesSourceEyeAngles = false;
                    }
                }
            }
        }
        const bool hmdPoseValid =
            hmdTransformValid &&
            calibration.hmdToHeadValid;
        if (hmdPoseValid)
        {
            vr_vm_stabilize::Mul(
                hmdWorld,
                calibration.hmdToHead,
                headTarget);

            const Vector sourceNativeHead =
                vr_vm_stabilize::GetOrigin(
                    sourceBones[layout->head]);
            Vector sourceNativeHeadLocal{};
            const bool sourceNativeHeadLocalValid =
                HooksWorldPoseWorldPositionToBodyLocal(
                    sourceNativeHead,
                    info,
                    sourceNativeHeadLocal);
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
            // Never learn a standing model-head reference from a crouched
            // frame. If a map first draws while crouched, defer this one
            // reference until the survivor next stands up.
            if (!calibration.nativeHeadReferenceLocalPositionValid &&
                sourceNativeHeadLocalValid &&
                !playerDucking)
            {
                calibration.nativeHeadReferenceLocalPosition =
                    sourceNativeHeadLocal;
                calibration.nativeHeadReferenceLocalPositionValid = true;
            }

            // Source's current head already contains crouch, jump, locomotion
            // and breathing.  Layer only a tightly bounded room-scale HMD
            // displacement over that native head.  In particular, never make
            // the head an absolute child of player origin: that would apply a
            // second crouch displacement and stretch the neck.
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

            appliedHmdLocalDelta =
                rawHmdLocalDelta;
            if (sourceNativeHeadLocalValid &&
                calibration.nativeHeadReferenceLocalPositionValid)
            {
                nativeHeadLocalDelta =
                    sourceNativeHeadLocal -
                    calibration.nativeHeadReferenceLocalPosition;

                // Do not subtract ordinary head/neck animation in X/Y or
                // small vertical breathing/bob: applying that inverse motion
                // to the whole upper chest would cancel native animation.
                // Only remove a clear, same-direction gross vertical motion
                // that both the tracker and Source already applied (crouch,
                // stand-up or jump).
                constexpr float kGrossVerticalMotion = 3.0f;
                if (std::fabs(rawHmdLocalDelta.z) >
                        kGrossVerticalMotion &&
                    std::fabs(nativeHeadLocalDelta.z) >
                        kGrossVerticalMotion &&
                    rawHmdLocalDelta.z *
                        nativeHeadLocalDelta.z >
                            0.0f)
                {
                    nativeHeadCompensation.z =
                        std::copysign(
                            std::min(
                                std::fabs(rawHmdLocalDelta.z),
                                std::fabs(nativeHeadLocalDelta.z)),
                            rawHmdLocalDelta.z);
                }
                appliedHmdLocalDelta -=
                    nativeHeadCompensation;
            }

            // Room-scale leaning is only a subtle upper-body layer. Never let
            // physical HMD height translate the torso vertically; crouch and
            // jump height remain owned by Source's body animation. Horizontal
            // lean is deliberately reduced to one third of tracked travel.
            constexpr float kUpperBodyHorizontalTrackingGain =
                1.0f / 3.0f;
            appliedHmdLocalDelta.x *=
                kUpperBodyHorizontalTrackingGain;
            appliedHmdLocalDelta.y *=
                kUpperBodyHorizontalTrackingGain;
            appliedHmdLocalDelta.z = 0.0f;
            const float horizontalLength =
                std::sqrt(
                    appliedHmdLocalDelta.x *
                        appliedHmdLocalDelta.x +
                    appliedHmdLocalDelta.y *
                        appliedHmdLocalDelta.y);
            // Saturate continuously at the anatomical limit. Do not rebase
            // the reference while the user remains outside it: rebasing makes
            // the torso snap to zero and repeatedly oscillate at the boundary.
            constexpr float kMaximumHmdHorizontalOffset = 1.0f;
            if (std::isfinite(horizontalLength) &&
                horizontalLength >
                    kMaximumHmdHorizontalOffset)
            {
                const float scale =
                    kMaximumHmdHorizontalOffset /
                    horizontalLength;
                appliedHmdLocalDelta.x *= scale;
                appliedHmdLocalDelta.y *= scale;
            }
            appliedHmdLocalDelta.z = 0.0f;

            Vector bodyForwardForHead{};
            Vector bodyRightForHead{};
            Vector bodyUpForHead{};
            const float hmdDeltaFrameYaw =
                calibration.hmdReferenceBodyYawValid
                    ? calibration.hmdReferenceBodyYaw
                    : HooksWorldPoseWrapAngle(info.angles.y);
            QAngle::AngleVectors(
                QAngle(
                    0.0f,
                    hmdDeltaFrameYaw,
                    0.0f),
                &bodyForwardForHead,
                &bodyRightForHead,
                &bodyUpForHead);
            const Vector nativeHead =
                vr_vm_stabilize::GetOrigin(
                    bones[layout->head]);
            appliedHmdWorldDelta =
                bodyForwardForHead * appliedHmdLocalDelta.x +
                bodyRightForHead * appliedHmdLocalDelta.y +
                bodyUpForHead * appliedHmdLocalDelta.z;
            const Vector constrainedHead =
                nativeHead +
                appliedHmdWorldDelta;
            headTarget.m[0][3] = constrainedHead.x;
            headTarget.m[1][3] = constrainedHead.y;
            headTarget.m[2][3] = constrainedHead.z;
        }

        const bool leftPoseValid =
            (pose.validMask &
             l4d2vr_pose::kValidLeftHand) != 0u &&
            HooksWorldPoseArmChainValid(
                layout->left,
                layout->parents,
                layout->numBones) &&
            layout->left.midAxisValid &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.leftHand,
                poseFrameInfo,
                leftControllerWorld) &&
            HooksWorldPoseBuildStaticHandTarget(
                layout->left,
                leftControllerWorld,
                bones[layout->left.hand],
                leftHandTarget);

        const bool rightPoseValid =
            (pose.validMask &
             l4d2vr_pose::kValidRightHand) != 0u &&
            HooksWorldPoseArmChainValid(
                layout->right,
                layout->parents,
                layout->numBones) &&
            layout->right.midAxisValid &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.rightHand,
                poseFrameInfo,
                rightControllerWorld) &&
            HooksWorldPoseBuildStaticHandTarget(
                layout->right,
                rightControllerWorld,
                bones[layout->right.hand],
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
            // Move the upper-body chain as one rigid branch. Rotating chest
            // and neck independently toward a nearby HMD target preserves
            // mathematical bone lengths, but heavily weighted custom meshes
            // can still render that bend as a long rubber neck. A bounded
            // shared translation keeps chest->neck->head spacing unchanged.
            int hmdTranslationRoot = -1;
            if (layout->upperChest >= 0 &&
                layout->upperChest < layout->numBones &&
                layout->upperChest != layout->head)
            {
                hmdTranslationRoot =
                    layout->upperChest;
            }
            else if (layout->neck >= 0 &&
                layout->neck < layout->numBones &&
                layout->neck != layout->head)
            {
                hmdTranslationRoot =
                    layout->neck;
            }
            if (hmdTranslationRoot >= 0)
            {
                changed =
                    HooksWorldPoseTranslateBranch(
                        *layout,
                        hmdTranslationRoot,
                        appliedHmdWorldDelta *
                            trackingWeight,
                        4.0f,
                        bones) ||
                    changed;
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
                        kHmdNeckRotationShare,
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
                    trackingWeight,
                    bones) ||
                changed;
        }

        Vector bodyForward{};
        Vector bodyRight{};
        Vector bodyUp{};
        QAngle::AngleVectors(
            QAngle(
                0.0f,
                appliedBodyYaw,
                0.0f),
            &bodyForward,
            &bodyRight,
            &bodyUp);

        vr_vm_stabilize::Mat3x4 armBodyFrame{};
        vr_vm_stabilize::BuildFromOrgAngles(
            info.origin +
                appliedHmdWorldDelta * trackingWeight,
            QAngle(0.0f, appliedBodyYaw, 0.0f),
            armBodyFrame);
        bool leftArmBodyCarryApplied = false;
        bool rightArmBodyCarryApplied = false;
        if (leftPoseValid)
        {
            leftArmBodyCarryApplied =
                HooksWorldPoseApplyBodyCarryToArmTarget(
                    *layout,
                    layout->left,
                    armBodyFrame,
                    bones,
                    !playerDucking,
                    calibration.leftArmBodyCarryReferenceValid,
                    calibration.leftArmBodyCarryReference,
                    leftHandTarget);
        }
        if (rightPoseValid)
        {
            rightArmBodyCarryApplied =
                HooksWorldPoseApplyBodyCarryToArmTarget(
                    *layout,
                    layout->right,
                    armBodyFrame,
                    bones,
                    !playerDucking,
                    calibration.rightArmBodyCarryReferenceValid,
                    calibration.rightArmBodyCarryReference,
                    rightHandTarget);
        }

        // Calibration validates a coherent HMD/controller body scale; it must
        // not replace the live wrist goal. The target remains the controller's
        // absolute world position plus only the current torso/clavicle carry
        // delta measured above. Adding a T-pose bind offset here a second time
        // leaves both arms spread after calibration.

        // Keep Source's live clavicle exactly attached to the animated torso.
        // Only the actual IK chain below it is restored to deterministic bind
        // locals. Ozz then solves from that current anatomical shoulder to the
        // body-carried controller goal, without creating a detached sleeve or
        // consuming separate walk/fire animation in upperArm/forearm/hand.
        bool leftBindChainRestored = false;
        bool rightBindChainRestored = false;
        if (leftPoseValid)
        {
            leftBindChainRestored =
                HooksWorldPoseRestoreBindArmChain(
                    *layout,
                    layout->left,
                    bones);
        }
        if (rightPoseValid)
        {
            rightBindChainRestored =
                HooksWorldPoseRestoreBindArmChain(
                    *layout,
                    layout->right,
                    bones);
        }
        changed = leftBindChainRestored ||
            rightBindChainRestored ||
            changed;

        // Controller packets remain at the player's physical room height when
        // the game crouch button is pressed, while Source lowers the native
        // chest and shoulder chain. Derive the hand drop from that native
        // standing-to-current upper-body displacement so the tracked wrists
        // stay attached to the animated shoulders without a user-tuned crouch
        // parameter. Keep only the vertical component: locomotion sway in X/Y
        // must not pull a held weapon back and forth.
        if (!calibration
                 .standingUpperBodyReferenceLocalPositionValid &&
            playerDucking)
        {
            calibration.standingUpperBodyReferenceSawDucking = true;
            calibration
                .standingUpperBodyReferenceCandidateSinceTickMs = 0u;
        }
        int stanceReferenceBone = -1;
        if (layout->upperChest >= 0 &&
            layout->upperChest < layout->numBones)
        {
            stanceReferenceBone = layout->upperChest;
        }
        else if (layout->neck >= 0 &&
                 layout->neck < layout->numBones)
        {
            stanceReferenceBone = layout->neck;
        }
        if (stanceReferenceBone >= 0 && !localPlayer)
        {
            const Vector sourceUpperBodyWorld =
                vr_vm_stabilize::GetOrigin(
                    sourceBones[stanceReferenceBone]);
            Vector sourceUpperBodyLocal{};
            if (HooksWorldPoseWorldPositionToBodyLocal(
                    sourceUpperBodyWorld,
                    info,
                    sourceUpperBodyLocal))
            {
                if (!calibration
                         .standingUpperBodyReferenceLocalPositionValid &&
                    !playerDucking)
                {
                    bool standingReferenceReady =
                        !calibration
                             .standingUpperBodyReferenceSawDucking;
                    if (!standingReferenceReady)
                    {
                        std::uint64_t& candidateSince =
                            calibration
                                .standingUpperBodyReferenceCandidateSinceTickMs;
                        if (candidateSince == 0u)
                            candidateSince = stereoNow;
                        constexpr std::uint64_t
                            kStandTransitionSettleMs = 350u;
                        standingReferenceReady =
                            stereoNow >= candidateSince &&
                            stereoNow - candidateSince >=
                                kStandTransitionSettleMs;
                    }
                    if (standingReferenceReady)
                    {
                        calibration.standingUpperBodyReferenceLocalPosition =
                            sourceUpperBodyLocal;
                        calibration
                            .standingUpperBodyReferenceLocalPositionValid =
                                true;
                        calibration
                            .standingUpperBodyReferenceCandidateSinceTickMs =
                                0u;
                    }
                }

                if (calibration
                        .standingUpperBodyReferenceLocalPositionValid)
                {
                    const float nativeUpperBodyDrop =
                        sourceUpperBodyLocal.z -
                        calibration
                            .standingUpperBodyReferenceLocalPosition.z;
                    // Continue following the native stand-up transition for a
                    // few frames after the duck flag clears. Small breathing
                    // and walk bob are deliberately ignored.
                    constexpr float kStandingTransitionThreshold = -6.0f;
                    if (playerDucking ||
                        nativeUpperBodyDrop <
                            kStandingTransitionThreshold)
                    {
                        const float handDrop = std::clamp(
                            nativeUpperBodyDrop,
                            -48.0f,
                            0.0f);
                        const Vector handDropWorld = bodyUp * handDrop;
                        if (leftPoseValid &&
                            !leftArmBodyCarryApplied)
                        {
                            leftHandTarget.m[0][3] += handDropWorld.x;
                            leftHandTarget.m[1][3] += handDropWorld.y;
                            leftHandTarget.m[2][3] += handDropWorld.z;
                        }
                        if (rightPoseValid &&
                            !rightArmBodyCarryApplied)
                        {
                            rightHandTarget.m[0][3] += handDropWorld.x;
                            rightHandTarget.m[1][3] += handDropWorld.y;
                            rightHandTarget.m[2][3] += handDropWorld.z;
                        }
                    }
                }
            }
        }
        bool leftArmSolved = false;
        bool rightArmSolved = false;
        bool leftArmReached = false;
        bool rightArmReached = false;
        if (leftPoseValid)
        {
            leftArmSolved =
                HooksWorldPoseSolveArmWithOzz(
                    *layout,
                    layout->left,
                    -1,
                    bodyForward,
                    bodyRight,
                    bodyUp,
                    leftHandTarget,
                    trackingWeight,
                    calibration.leftPoleBodyLocalValid,
                    calibration.leftPoleBodyLocal,
                    leftArmReached,
                    bones);
            changed = leftArmSolved || changed;
        }
        if (rightPoseValid)
        {
            rightArmSolved =
                HooksWorldPoseSolveArmWithOzz(
                    *layout,
                    layout->right,
                    1,
                    bodyForward,
                    bodyRight,
                    bodyUp,
                    rightHandTarget,
                    trackingWeight,
                    calibration.rightPoleBodyLocalValid,
                    calibration.rightPoleBodyLocal,
                    rightArmReached,
                    bones);
            changed = rightArmSolved || changed;
        }

        if (!changed)
            return false;
        for (int bone = 0; bone < layout->numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    bones[bone]))
            {
                return false;
            }
        }

        // The active weapon is a separate renderable/bonemerge draw and never
        // sees this temporary player-bone copy.  Publish the exact native and
        // final right-hand transforms so its later draw can receive the same
        // rigid correction without mutating Source's shared bone cache.
        HooksWorldPosePublishWeaponHandState(
            vr,
            game,
            entity,
            info.entity_index,
            info,
            *layout,
            sourceBones,
            bones,
            rightControllerWorld,
            rightPoseValid);

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
                    "[VR][WorldPose] IK offsets raw=(%.1f %.1f %.1f) native=(%.1f %.1f %.1f) comp=(%.1f %.1f %.1f) applied=(%.1f %.1f %.1f) neck=%.2f->%.2f",
                    rawHmdLocalDelta.x,
                    rawHmdLocalDelta.y,
                    rawHmdLocalDelta.z,
                    nativeHeadLocalDelta.x,
                    nativeHeadLocalDelta.y,
                    nativeHeadLocalDelta.z,
                    nativeHeadCompensation.x,
                    nativeHeadCompensation.y,
                    nativeHeadCompensation.z,
                    appliedHmdLocalDelta.x,
                    appliedHmdLocalDelta.y,
                    appliedHmdLocalDelta.z,
                    nativeNeckLength,
                    finalNeckLength);
                Game::logMsg(
                    "[VR][WorldPose] IK arms left=%d carry=%d solved=%d axis=%d reached=%d right=%d carry=%d solved=%d axis=%d reached=%d shotWrist=%d shotTriggered=%d",
                    leftPoseValid ? 1 : 0,
                    leftArmBodyCarryApplied ? 1 : 0,
                    leftArmSolved ? 1 : 0,
                    layout->left.midAxisValid ? 1 : 0,
                    leftArmReached ? 1 : 0,
                    rightPoseValid ? 1 : 0,
                    rightArmBodyCarryApplied ? 1 : 0,
                    rightArmSolved ? 1 : 0,
                    layout->right.midAxisValid ? 1 : 0,
                    rightArmReached ? 1 : 0,
                    weaponGripRotationValid ? 1 : 0,
                    weaponShotTriggered ? 1 : 0);
                if (leftPoseValid)
                {
                    const Vector leftShoulder =
                        vr_vm_stabilize::GetOrigin(
                            bones[layout->left.upperArm]);
                    const Vector leftGoal =
                        vr_vm_stabilize::GetOrigin(leftHandTarget);
                    Game::logMsg(
                        "[VR][WorldPose] IK left shoulder=(%.2f %.2f %.2f) goal=(%.2f %.2f %.2f)",
                        leftShoulder.x,
                        leftShoulder.y,
                        leftShoulder.z,
                        leftGoal.x,
                        leftGoal.y,
                        leftGoal.z);
                }
                if (rightPoseValid)
                {
                    const Vector rightShoulder =
                        vr_vm_stabilize::GetOrigin(
                            bones[layout->right.upperArm]);
                    const Vector rightGoal =
                        vr_vm_stabilize::GetOrigin(rightHandTarget);
                    Game::logMsg(
                        "[VR][WorldPose] IK right shoulder=(%.2f %.2f %.2f) goal=(%.2f %.2f %.2f)",
                        rightShoulder.x,
                        rightShoulder.y,
                        rightShoulder.z,
                        rightGoal.x,
                        rightGoal.y,
                        rightGoal.z);
                }
            }
        }

        outBones = bones;
        return true;
    }
