    // ------------------------------------------------------------
    // Multiplayer VR world-model pose reconstruction
    //
    // Tracking samples remain player-yaw-local on the wire.  This renderer
    // resolves them against the current ModelRenderInfo_t, then layers a
    // visual-only head/upper-body/dual-arm solve over Source's final animated
    // matrices.  Lower-body locomotion, collision bones, and hitboxes remain
    // completely native.
    // ------------------------------------------------------------
    struct HooksWorldPoseArmLayout
    {
        int clavicle = -1;
        int upperArm = -1;
        int forearm = -1;
        int hand = -1;
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
        bool leftControllerToHandValid = false;
        bool rightControllerToHandValid = false;
        bool hmdToHeadUsesSourceEyeAngles = false;
        const C_BaseCombatWeapon* rightGripWeapon = nullptr;
        bool rightGripRootReferenceValid = false;
        bool hmdReferenceLocalPositionValid = false;
        bool nativeHeadReferenceLocalPositionValid = false;
        Vector hmdReferenceLocalPosition{};
        Vector nativeHeadReferenceLocalPosition{};
        vr_vm_stabilize::Mat3x4 hmdToHead{};
        vr_vm_stabilize::Mat3x4 leftControllerToHand{};
        vr_vm_stabilize::Mat3x4 rightControllerToHand{};
        vr_vm_stabilize::Mat3x4 rightGripRootReferenceLocal{};
        bool neckReferenceLocalValid = false;
        bool headReferenceLocalValid = false;
        vr_vm_stabilize::Mat3x4 neckReferenceLocal{};
        vr_vm_stabilize::Mat3x4 headReferenceLocal{};
        bool visualBodyYawValid = false;
        bool visualBodyYawTurning = false;
        float visualBodyYaw = 0.0f;
        std::uint64_t visualBodyYawTickMs = 0u;
        bool passiveReferenceValid = false;
        int passiveReferenceBones = 0;
        std::vector<vr_vm_stabilize::Mat3x4> passiveReferenceLocal;
        std::vector<uint8_t> passiveReferenceLocalValid;

        void Reset(const C_BaseEntity* newEntity, const uint8_t* newStudioHdr)
        {
            *this = HooksWorldPoseCalibration{};
            entity = newEntity;
            studioHdr = newStudioHdr;
        }
    };

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
        vr_vm_stabilize::Mat3x4 nativeHand{};
        vr_vm_stabilize::Mat3x4 finalHand{};
        vr_vm_stabilize::Mat3x4 nativeToFinal{};
    };

    std::mutex g_HooksWorldPoseWeaponHandMutex;
    std::array<
        HooksWorldPoseWeaponHandState,
        Game::kMaxPlayers> g_HooksWorldPoseWeaponHandStates{};
    std::atomic<void*>
        g_HooksWorldPosePendingWeaponRenderable{ nullptr };
    std::atomic<void*>
        g_HooksWorldPoseWeaponSetupBonesTarget{ nullptr };
    std::atomic<void*>
        g_HooksWorldPoseLastSetupBonesRenderable{ nullptr };
    std::atomic<std::uint64_t>
        g_HooksWorldPoseLastSetupBonesAppliedTickMs{ 0u };
    std::atomic<bool>
        g_HooksWorldPoseWeaponSetupBonesHookReady{ false };
    std::mutex g_HooksWorldPoseWeaponSetupBonesHookMutex;

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
                "[VR][WorldPose] bone layout ready hdr=%p bones=%d head=%d neck=%d chest=%d left=(%d %d %d %d) right=(%d %d %d %d)",
                layout.studioHdr,
                layout.numBones,
                layout.head,
                layout.neck,
                layout.upperChest,
                layout.left.clavicle,
                layout.left.upperArm,
                layout.left.forearm,
                layout.left.hand,
                layout.right.clavicle,
                layout.right.upperArm,
                layout.right.forearm,
                layout.right.hand);
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
        float sourceBodyYaw,
        HooksWorldPoseCalibration& calibration,
        float& outVisualBodyYaw,
        float& outYawError,
        bool& outTurning)
    {
        sourceBodyYaw =
            HooksWorldPoseWrapAngle(sourceBodyYaw);
        const std::uint64_t now =
            static_cast<std::uint64_t>(GetTickCount64());
        if (!calibration.visualBodyYawValid ||
            !std::isfinite(calibration.visualBodyYaw))
        {
            calibration.visualBodyYawValid = true;
            calibration.visualBodyYawTurning = false;
            calibration.visualBodyYaw = sourceBodyYaw;
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
                sourceBodyYaw -
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
                    sourceBodyYaw -
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
                    sourceBodyYaw -
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
                sourceBodyYaw -
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

    inline void HooksWorldPosePublishWeaponHandState(
        VR* vr,
        Game* game,
        const C_BaseEntity* playerEntity,
        int playerIndex,
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const vr_vm_stabilize::Mat3x4* finalBones,
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
        published.nativeHand = nativeRigid;
        published.finalHand = finalRigid;
        published.nativeToFinal = nativeToFinal;

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
        int maxBones)
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

        const std::uint64_t now =
            static_cast<std::uint64_t>(GetTickCount64());
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
                    const auto* const setupBones =
                        reinterpret_cast<
                            const vr_vm_stabilize::Mat3x4*>(
                                boneToWorldOut);
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

        int transformedBones = 0;
        vr_vm_stabilize::Mat3x4* const bones =
            reinterpret_cast<vr_vm_stabilize::Mat3x4*>(
                boneToWorldOut);
        for (int bone = 0; bone < maxBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(
                    bones + bone,
                    source) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    source))
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 transformed{};
            vr_vm_stabilize::Mul(
                handState.nativeToFinal,
                source,
                transformed);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    transformed))
            {
                continue;
            }
            bones[bone] = transformed;
            ++transformedBones;
        }
        if (transformedBones <= 0)
            return false;

        g_HooksWorldPoseLastSetupBonesRenderable.store(
            renderable,
            std::memory_order_release);
        g_HooksWorldPoseLastSetupBonesAppliedTickMs.store(
            now,
            std::memory_order_release);

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
                    "[VR][WorldPoseWeapon] SetupBones applied player=%d renderable=%p match=0x%X handDistance=%.1f bones=%d/%d age=%llu displacement=(%.1f %.1f %.1f)",
                    handState.playerIndex,
                    renderable,
                    matchMask,
                    bestSpatialDistance,
                    transformedBones,
                    maxBones,
                    static_cast<unsigned long long>(
                        now - handState.updatedTickMs),
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
        if (info.pRenderable &&
            g_HooksWorldPoseLastSetupBonesRenderable.load(
                std::memory_order_acquire) ==
                info.pRenderable)
        {
            const std::uint64_t setupBonesTick =
                g_HooksWorldPoseLastSetupBonesAppliedTickMs.load(
                    std::memory_order_acquire);
            if (now >= setupBonesTick &&
                now - setupBonesTick <= 100u)
            {
                // SetupBones already transformed the exact renderable's
                // output. Do not apply the same hand delta again in the
                // DrawModelExecute fallback.
                return false;
            }
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

    inline bool HooksWorldPoseEnsureCalibration(
        const vr_vm_stabilize::Mat3x4& trackerWorld,
        const vr_vm_stabilize::Mat3x4& sourceBone,
        float maximumTranslation,
        bool& calibrationValid,
        vr_vm_stabilize::Mat3x4& trackerToBone)
    {
        if (!calibrationValid)
        {
            vr_vm_stabilize::Mat3x4 sourceRigid{};
            vr_vm_stabilize::Mat3x4 inverseTracker{};
            if (!HooksWorldPoseBuildRigidBoneTransform(
                    sourceBone,
                    sourceRigid))
            {
                return false;
            }

            vr_vm_stabilize::InvertTR(
                trackerWorld,
                inverseTracker);
            vr_vm_stabilize::Mul(
                inverseTracker,
                sourceRigid,
                trackerToBone);
            Vector offset =
                vr_vm_stabilize::GetOrigin(trackerToBone);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    trackerToBone) ||
                !HooksNativeViewmodelHandsOnlyVectorFinite(offset))
            {
                return false;
            }

            // A controller is the wrist target, not a parent transform for an
            // arbitrary first-frame native hand offset.  Preserve only its
            // rotation calibration so rotating a stationary controller cannot
            // make the wrist orbit around it.  HMD-to-head translation is a
            // small anatomical eye/head-center offset and is tightly capped.
            maximumTranslation =
                std::clamp(maximumTranslation, 0.0f, 32.0f);
            const float offsetLength = offset.Length();
            if (maximumTranslation <= 0.0f)
            {
                offset = Vector(0.0f, 0.0f, 0.0f);
            }
            else if (std::isfinite(offsetLength) &&
                     offsetLength > maximumTranslation)
            {
                offset *= maximumTranslation / offsetLength;
            }
            trackerToBone.m[0][3] = offset.x;
            trackerToBone.m[1][3] = offset.y;
            trackerToBone.m[2][3] = offset.z;
            calibrationValid = true;
        }
        return true;
    }

    inline bool HooksWorldPoseApplyGripRootAnimation(
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int handBone,
        bool& referenceValid,
        vr_vm_stabilize::Mat3x4& referenceLocal,
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
        if (!HooksWorldPoseBuildRigidBoneTransform(
                animatedLocal,
                animatedRigid))
        {
            return false;
        }

        if (!referenceValid)
        {
            referenceLocal = animatedRigid;
            referenceValid = true;
            return true;
        }

        vr_vm_stabilize::Mat3x4 inverseReference{};
        vr_vm_stabilize::Mat3x4 localDelta{};
        vr_vm_stabilize::InvertTR(
            referenceLocal,
            inverseReference);
        vr_vm_stabilize::Mul(
            inverseReference,
            animatedRigid,
            localDelta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                localDelta))
        {
            referenceLocal = animatedRigid;
            return false;
        }

        Vector deltaOrigin =
            vr_vm_stabilize::GetOrigin(localDelta);
        QAngle deltaAngles{};
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                deltaOrigin) ||
            !HooksViewmodelAutoGripMatrixAngles(
                localDelta,
                deltaAngles))
        {
            referenceLocal = animatedRigid;
            return false;
        }

        // Grip and recoil motion at the hand root is small.  Bound the
        // extracted local delta so a custom animation discontinuity or a
        // weapon-sequence transition cannot wrench the tracked wrist away
        // from its controller target.
        constexpr float kMaximumGripRootTranslation = 3.0f;
        constexpr float kMaximumGripRootAngle = 35.0f;
        const float deltaDistance = deltaOrigin.Length();
        if (!std::isfinite(deltaDistance) ||
            deltaDistance > 12.0f)
        {
            referenceLocal = animatedRigid;
            return true;
        }
        if (deltaDistance >
            kMaximumGripRootTranslation)
        {
            deltaOrigin *=
                kMaximumGripRootTranslation /
                deltaDistance;
        }
        deltaAngles.x = std::clamp(
            HooksWorldPoseWrapAngle(deltaAngles.x),
            -kMaximumGripRootAngle,
            kMaximumGripRootAngle);
        deltaAngles.y = std::clamp(
            HooksWorldPoseWrapAngle(deltaAngles.y),
            -kMaximumGripRootAngle,
            kMaximumGripRootAngle);
        deltaAngles.z = std::clamp(
            HooksWorldPoseWrapAngle(deltaAngles.z),
            -kMaximumGripRootAngle,
            kMaximumGripRootAngle);

        vr_vm_stabilize::Mat3x4 constrainedDelta{};
        vr_vm_stabilize::Mat3x4 animatedTarget{};
        vr_vm_stabilize::BuildFromOrgAngles(
            deltaOrigin,
            deltaAngles,
            constrainedDelta);
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

    inline bool HooksWorldPoseApplyOffhandRotationOffset(
        VR* vr,
        const vr_vm_stabilize::Mat3x4& controllerWorld,
        vr_vm_stabilize::Mat3x4& inOutHandTarget)
    {
        if (!vr ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                controllerWorld) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(
                inOutHandTarget))
        {
            return false;
        }

        const Vector rotationOffsetDeg =
            vr->m_WorldModelVRPoseOffhandRotationOffsetDeg;
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                rotationOffsetDeg))
        {
            return false;
        }
        if (rotationOffsetDeg.LengthSqr() <= 0.000001f)
            return false;

        const Vector controllerOrigin =
            vr_vm_stabilize::GetOrigin(controllerWorld);
        const Vector controllerForward(
            controllerWorld.m[0][0],
            controllerWorld.m[1][0],
            controllerWorld.m[2][0]);
        const Vector controllerRight(
            controllerWorld.m[0][1],
            controllerWorld.m[1][1],
            controllerWorld.m[2][1]);
        const Vector controllerUp(
            controllerWorld.m[0][2],
            controllerWorld.m[1][2],
            controllerWorld.m[2][2]);

        // This parameter is authored specifically for the observer-visible
        // world model. Recreate the controller-local OpenVR basis used by the
        // hand target, but do not inherit the first-person viewmodel setting:
        // custom survivor skeletons commonly need different wrist tuning.
        const VrHandMatrix4 nativeControllerWorld =
            HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes(
                controllerOrigin,
                controllerForward,
                controllerRight,
                controllerUp);
        const VrHandMatrix4 localRotation =
            HooksNativeViewmodelHandsOnlyBuildLocalTransform(
                1.0f,
                Vector(0.0f, 0.0f, 0.0f),
                rotationOffsetDeg);
        const vr_vm_stabilize::Mat3x4 nativeController =
            HooksVrHandMatrixToMat3x4(
                nativeControllerWorld);
        const vr_vm_stabilize::Mat3x4 correctedNativeController =
            HooksVrHandMatrixToMat3x4(
                VrHandMath::Multiply(
                    nativeControllerWorld,
                    localRotation));

        vr_vm_stabilize::Mat3x4 inverseNativeController{};
        vr_vm_stabilize::Mat3x4 worldRotationDelta{};
        vr_vm_stabilize::Mat3x4 adjustedTarget{};
        vr_vm_stabilize::InvertTR(
            nativeController,
            inverseNativeController);
        vr_vm_stabilize::Mul(
            correctedNativeController,
            inverseNativeController,
            worldRotationDelta);
        vr_vm_stabilize::Mul(
            worldRotationDelta,
            inOutHandTarget,
            adjustedTarget);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                adjustedTarget))
        {
            return false;
        }

        // This setting is a wrist rotation calibration.  Preserve the IK
        // target position so the local rotation cannot orbit the wrist around
        // the controller or change the solved arm reach.
        const Vector targetOrigin =
            vr_vm_stabilize::GetOrigin(inOutHandTarget);
        adjustedTarget.m[0][3] = targetOrigin.x;
        adjustedTarget.m[1][3] = targetOrigin.y;
        adjustedTarget.m[2][3] = targetOrigin.z;
        inOutHandTarget = adjustedTarget;
        return true;
    }

    inline bool HooksWorldPoseBuildRotationDelta(
        Vector fromDirection,
        Vector toDirection,
        const Vector& pivot,
        float fraction,
        float maxAngleDegrees,
        vr_vm_stabilize::Mat3x4& out)
    {
        out = vr_vm_stabilize::Identity();
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(fromDirection) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(toDirection) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(pivot) ||
            VectorNormalize(fromDirection) == 0.0f ||
            VectorNormalize(toDirection) == 0.0f)
        {
            return false;
        }

        const float dot =
            std::clamp(
                DotProduct(fromDirection, toDirection),
                -1.0f,
                1.0f);
        float angle = std::acos(dot);
        if (!std::isfinite(angle) || angle <= 0.00001f)
            return false;

        angle *= std::clamp(fraction, 0.0f, 1.0f);
        if (std::isfinite(maxAngleDegrees) &&
            maxAngleDegrees > 0.0f)
        {
            constexpr float kDegreesToRadians =
                3.14159265358979323846f / 180.0f;
            angle = std::min(
                angle,
                maxAngleDegrees * kDegreesToRadians);
        }
        if (angle <= 0.00001f)
            return false;

        Vector axis =
            CrossProduct(fromDirection, toDirection);
        if (VectorNormalize(axis) == 0.0f)
        {
            const Vector fallback =
                std::fabs(fromDirection.z) < 0.8f
                    ? Vector(0.0f, 0.0f, 1.0f)
                    : Vector(0.0f, 1.0f, 0.0f);
            axis = CrossProduct(fromDirection, fallback);
            if (VectorNormalize(axis) == 0.0f)
                return false;
        }

        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const float oneMinusCosine = 1.0f - cosine;

        out.m[0][0] = cosine + x * x * oneMinusCosine;
        out.m[0][1] = x * y * oneMinusCosine - z * sine;
        out.m[0][2] = x * z * oneMinusCosine + y * sine;
        out.m[1][0] = y * x * oneMinusCosine + z * sine;
        out.m[1][1] = cosine + y * y * oneMinusCosine;
        out.m[1][2] = y * z * oneMinusCosine - x * sine;
        out.m[2][0] = z * x * oneMinusCosine - y * sine;
        out.m[2][1] = z * y * oneMinusCosine + x * sine;
        out.m[2][2] = cosine + z * z * oneMinusCosine;

        out.m[0][3] =
            pivot.x -
            (out.m[0][0] * pivot.x +
             out.m[0][1] * pivot.y +
             out.m[0][2] * pivot.z);
        out.m[1][3] =
            pivot.y -
            (out.m[1][0] * pivot.x +
             out.m[1][1] * pivot.y +
             out.m[1][2] * pivot.z);
        out.m[2][3] =
            pivot.z -
            (out.m[2][0] * pivot.x +
             out.m[2][1] * pivot.y +
             out.m[2][2] * pivot.z);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
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

    inline bool HooksWorldPoseRotateBranchToward(
        const HooksWorldPoseBoneLayout& layout,
        int rootBone,
        const Vector& fromDirection,
        const Vector& toDirection,
        const Vector& pivot,
        float fraction,
        float maxAngleDegrees,
        vr_vm_stabilize::Mat3x4* bones)
    {
        vr_vm_stabilize::Mat3x4 delta{};
        if (!HooksWorldPoseBuildRotationDelta(
                fromDirection,
                toDirection,
                pivot,
                fraction,
                maxAngleDegrees,
                delta))
        {
            return false;
        }
        return HooksWorldPoseApplyDeltaToBranch(
            layout,
            rootBone,
            delta,
            bones);
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

    inline bool HooksWorldPoseCapturePassiveReferencePose(
        const HooksWorldPoseBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        HooksWorldPoseCalibration& calibration)
    {
        if (calibration.passiveReferenceValid &&
            static_cast<int>(
                calibration.passiveReferenceLocal.size()) ==
                layout.numBones &&
            static_cast<int>(
                calibration.passiveReferenceLocalValid.size()) ==
                layout.numBones)
        {
            return true;
        }
        if (!sourceBones ||
            layout.numBones <= 0 ||
            layout.numBones > 512 ||
            static_cast<int>(layout.parents.size()) <
                layout.numBones)
        {
            return false;
        }

        std::vector<vr_vm_stabilize::Mat3x4> referenceLocal;
        std::vector<uint8_t> referenceValid;
        try
        {
            referenceLocal.assign(
                static_cast<size_t>(layout.numBones),
                vr_vm_stabilize::Identity());
            referenceValid.assign(
                static_cast<size_t>(layout.numBones),
                0u);
        }
        catch (...)
        {
            return false;
        }

        int captured = 0;
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            const bool headDescendant =
                layout.head >= 0 &&
                bone != layout.head &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    layout.head,
                    layout.numBones);
            const bool leftHandDescendant =
                layout.left.hand >= 0 &&
                bone != layout.left.hand &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    layout.left.hand,
                    layout.numBones);
            const bool rightHandDescendant =
                layout.right.hand >= 0 &&
                bone != layout.right.hand &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    layout.right.hand,
                    layout.numBones);
            if (!headDescendant &&
                !leftHandDescendant &&
                !rightHandDescendant)
            {
                continue;
            }

            const int parent =
                layout.parents[static_cast<size_t>(bone)];
            if (parent < 0 ||
                parent >= layout.numBones ||
                parent == bone)
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 boneWorld{};
            vr_vm_stabilize::Mat3x4 parentWorld{};
            vr_vm_stabilize::Mat3x4 inverseParent{};
            vr_vm_stabilize::Mat3x4 boneLocal{};
            if (!vr_vm_stabilize::SafeRead(
                    sourceBones + bone,
                    boneWorld) ||
                !vr_vm_stabilize::SafeRead(
                    sourceBones + parent,
                    parentWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    boneWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    parentWorld) ||
                !vr_vm_stabilize::InvertAffine(
                    parentWorld,
                    inverseParent))
            {
                continue;
            }
            vr_vm_stabilize::Mul(
                inverseParent,
                boneWorld,
                boneLocal);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    boneLocal))
            {
                continue;
            }

            referenceLocal[static_cast<size_t>(bone)] =
                boneLocal;
            referenceValid[static_cast<size_t>(bone)] = 1u;
            ++captured;
        }

        if (captured <= 0)
            return false;
        calibration.passiveReferenceLocal =
            std::move(referenceLocal);
        calibration.passiveReferenceLocalValid =
            std::move(referenceValid);
        calibration.passiveReferenceBones = captured;
        calibration.passiveReferenceValid = true;
        return true;
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

    inline bool HooksWorldPoseBlendLocalTransform(
        const vr_vm_stabilize::Mat3x4& animatedLocal,
        const vr_vm_stabilize::Mat3x4& referenceLocal,
        float weight,
        vr_vm_stabilize::Mat3x4& out)
    {
        weight = std::clamp(weight, 0.0f, 1.0f);
        if (weight <= 0.0001f)
        {
            out = animatedLocal;
            return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
        }
        if (weight >= 0.9999f)
        {
            out = referenceLocal;
            return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
        }

        QAngle animatedAngles{};
        QAngle referenceAngles{};
        if (!HooksViewmodelAutoGripMatrixAngles(
                animatedLocal,
                animatedAngles) ||
            !HooksViewmodelAutoGripMatrixAngles(
                referenceLocal,
                referenceAngles))
        {
            return false;
        }

        const Vector animatedOrigin =
            vr_vm_stabilize::GetOrigin(animatedLocal);
        const Vector referenceOrigin =
            vr_vm_stabilize::GetOrigin(referenceLocal);
        const Vector blendedOrigin =
            animatedOrigin +
            (referenceOrigin - animatedOrigin) * weight;
        const QAngle blendedAngles =
            HooksWorldPoseLerpAngles(
                animatedAngles,
                referenceAngles,
                weight);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                blendedOrigin))
        {
            return false;
        }

        vr_vm_stabilize::BuildFromOrgAngles(
            blendedOrigin,
            blendedAngles,
            out);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
    }

    inline bool HooksWorldPoseStabilizePassiveDescendants(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseCalibration& calibration,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        bool stabilizeHead,
        bool stabilizeLeftHand,
        bool stabilizeRightHand,
        float weight,
        vr_vm_stabilize::Mat3x4* bones,
        bool& outChanged)
    {
        outChanged = false;
        if (!sourceBones ||
            !bones ||
            !calibration.passiveReferenceValid ||
            layout.numBones <= 0 ||
            layout.numBones > 512 ||
            static_cast<int>(layout.parents.size()) <
                layout.numBones ||
            static_cast<int>(
                calibration.passiveReferenceLocal.size()) <
                layout.numBones ||
            static_cast<int>(
                calibration.passiveReferenceLocalValid.size()) <
                layout.numBones)
        {
            return false;
        }

        std::array<uint8_t, 512> candidates{};
        std::array<uint8_t, 512> resolved{};
        int candidateCount = 0;
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            if (!calibration.passiveReferenceLocalValid[
                    static_cast<size_t>(bone)])
            {
                continue;
            }

            const bool headDescendant =
                stabilizeHead &&
                layout.head >= 0 &&
                bone != layout.head &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    layout.head,
                    layout.numBones);
            const bool leftHandDescendant =
                stabilizeLeftHand &&
                layout.left.hand >= 0 &&
                bone != layout.left.hand &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    layout.left.hand,
                    layout.numBones);
            const bool rightHandDescendant =
                stabilizeRightHand &&
                layout.right.hand >= 0 &&
                bone != layout.right.hand &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.parents,
                    bone,
                    layout.right.hand,
                    layout.numBones);
            if (!headDescendant &&
                !leftHandDescendant &&
                !rightHandDescendant)
            {
                continue;
            }

            candidates[static_cast<size_t>(bone)] = 1u;
            ++candidateCount;
        }
        if (candidateCount <= 0)
            return true;

        int resolvedCount = 0;
        int appliedCount = 0;
        for (int pass = 0;
             pass < layout.numBones &&
             resolvedCount < candidateCount;
             ++pass)
        {
            bool progressed = false;
            for (int bone = 0; bone < layout.numBones; ++bone)
            {
                const size_t index = static_cast<size_t>(bone);
                if (!candidates[index] || resolved[index])
                    continue;

                const int parent = layout.parents[index];
                if (parent < 0 ||
                    parent >= layout.numBones ||
                    parent == bone)
                {
                    resolved[index] = 1u;
                    ++resolvedCount;
                    progressed = true;
                    continue;
                }
                if (candidates[static_cast<size_t>(parent)] &&
                    !resolved[static_cast<size_t>(parent)])
                {
                    continue;
                }

                vr_vm_stabilize::Mat3x4 sourceBone{};
                vr_vm_stabilize::Mat3x4 sourceParent{};
                vr_vm_stabilize::Mat3x4 inverseSourceParent{};
                vr_vm_stabilize::Mat3x4 animatedLocal{};
                vr_vm_stabilize::Mat3x4 blendedLocal{};
                vr_vm_stabilize::Mat3x4 stabilizedWorld{};
                if (!vr_vm_stabilize::SafeRead(
                        sourceBones + bone,
                        sourceBone) ||
                    !vr_vm_stabilize::SafeRead(
                        sourceBones + parent,
                        sourceParent) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(
                        sourceBone) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(
                        sourceParent) ||
                    !vr_vm_stabilize::InvertAffine(
                        sourceParent,
                        inverseSourceParent))
                {
                    resolved[index] = 1u;
                    ++resolvedCount;
                    progressed = true;
                    continue;
                }
                vr_vm_stabilize::Mul(
                    inverseSourceParent,
                    sourceBone,
                    animatedLocal);
                if (!HooksWorldPoseBlendLocalTransform(
                        animatedLocal,
                        calibration.passiveReferenceLocal[index],
                        weight,
                        blendedLocal))
                {
                    resolved[index] = 1u;
                    ++resolvedCount;
                    progressed = true;
                    continue;
                }
                vr_vm_stabilize::Mul(
                    bones[parent],
                    blendedLocal,
                    stabilizedWorld);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                        stabilizedWorld))
                {
                    resolved[index] = 1u;
                    ++resolvedCount;
                    progressed = true;
                    continue;
                }

                bones[bone] = stabilizedWorld;
                resolved[index] = 1u;
                ++resolvedCount;
                ++appliedCount;
                progressed = true;
            }
            if (!progressed)
                break;
        }

        outChanged = appliedCount > 0;
        return true;
    }

    inline bool HooksWorldPoseSolveArm(
        const HooksWorldPoseBoneLayout& layout,
        const HooksWorldPoseArmLayout& arm,
        int side,
        const Vector& bodyRight,
        const vr_vm_stabilize::Mat3x4& calibratedHandTarget,
        float weight,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!HooksWorldPoseArmChainValid(
                arm,
                layout.parents,
                layout.numBones) ||
            !bones)
        {
            return false;
        }

        Vector currentHand =
            vr_vm_stabilize::GetOrigin(bones[arm.hand]);
        const Vector fullTargetHand =
            vr_vm_stabilize::GetOrigin(calibratedHandTarget);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(currentHand) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(fullTargetHand))
        {
            return false;
        }
        Vector targetHand =
            currentHand +
            (fullTargetHand - currentHand) *
                std::clamp(weight, 0.0f, 1.0f);

        Vector shoulder =
            vr_vm_stabilize::GetOrigin(bones[arm.upperArm]);
        Vector elbow =
            vr_vm_stabilize::GetOrigin(bones[arm.forearm]);
        float upperLength = (elbow - shoulder).Length();
        float forearmLength = (currentHand - elbow).Length();
        if (!std::isfinite(upperLength) ||
            !std::isfinite(forearmLength) ||
            upperLength < 1.0f ||
            forearmLength < 1.0f ||
            upperLength > 64.0f ||
            forearmLength > 64.0f)
        {
            return false;
        }

        const float maximumReach =
            upperLength + forearmLength;
        const float targetDistance =
            (targetHand - shoulder).Length();
        if (arm.clavicle >= 0 &&
            arm.clavicle < layout.numBones &&
            targetDistance > maximumReach * 0.90f)
        {
            const Vector clavicle =
                vr_vm_stabilize::GetOrigin(
                    bones[arm.clavicle]);
            const Vector shoulderFromClavicle =
                shoulder - clavicle;
            const Vector targetFromClavicle =
                targetHand - clavicle;
            const float reachFraction = std::clamp(
                (targetDistance / maximumReach - 0.90f) / 0.25f,
                0.0f,
                1.0f);
            HooksWorldPoseRotateBranchToward(
                layout,
                arm.clavicle,
                shoulderFromClavicle,
                targetFromClavicle,
                clavicle,
                reachFraction * 0.75f,
                25.0f,
                bones);

            shoulder =
                vr_vm_stabilize::GetOrigin(
                    bones[arm.upperArm]);
            elbow =
                vr_vm_stabilize::GetOrigin(
                    bones[arm.forearm]);
            currentHand =
                vr_vm_stabilize::GetOrigin(
                    bones[arm.hand]);
            upperLength = (elbow - shoulder).Length();
            forearmLength = (currentHand - elbow).Length();
        }

        Vector shoulderToTarget =
            targetHand - shoulder;
        float distance = shoulderToTarget.Length();
        if (!std::isfinite(distance) ||
            VectorNormalize(shoulderToTarget) == 0.0f)
        {
            shoulderToTarget = currentHand - shoulder;
            distance = shoulderToTarget.Length();
            if (!std::isfinite(distance) ||
                VectorNormalize(shoulderToTarget) == 0.0f)
            {
                return false;
            }
        }

        const float minimumDistance =
            std::fabs(upperLength - forearmLength) + 0.05f;
        const float maximumDistance =
            upperLength + forearmLength - 0.05f;
        if (maximumDistance <= minimumDistance)
            return false;
        distance = std::clamp(
            distance,
            minimumDistance,
            maximumDistance);
        const Vector solvedWrist =
            shoulder + shoulderToTarget * distance;

        Vector pole =
            elbow - shoulder;
        pole -= shoulderToTarget *
            DotProduct(pole, shoulderToTarget);
        if (VectorNormalize(pole) == 0.0f)
        {
            pole =
                bodyRight * static_cast<float>(side) +
                Vector(0.0f, 0.0f, -0.25f);
            pole -= shoulderToTarget *
                DotProduct(pole, shoulderToTarget);
            if (VectorNormalize(pole) == 0.0f)
                return false;
        }

        const float alongUpper =
            (upperLength * upperLength +
             distance * distance -
             forearmLength * forearmLength) /
            (2.0f * distance);
        const float bendSquared =
            std::max(
                0.0f,
                upperLength * upperLength -
                    alongUpper * alongUpper);
        const Vector solvedElbow =
            shoulder +
            shoulderToTarget * alongUpper +
            pole * std::sqrt(bendSquared);

        bool changed = HooksWorldPoseRotateBranchToward(
            layout,
            arm.upperArm,
            elbow - shoulder,
            solvedElbow - shoulder,
            shoulder,
            1.0f,
            179.0f,
            bones);

        const Vector rotatedElbow =
            vr_vm_stabilize::GetOrigin(
                bones[arm.forearm]);
        const Vector rotatedHand =
            vr_vm_stabilize::GetOrigin(
                bones[arm.hand]);
        changed =
            HooksWorldPoseRotateBranchToward(
                layout,
                arm.forearm,
                rotatedHand - rotatedElbow,
                solvedWrist - rotatedElbow,
                rotatedElbow,
                1.0f,
                179.0f,
                bones) ||
            changed;

        changed =
            HooksWorldPoseOrientBranch(
                layout,
                arm.hand,
                calibratedHandTarget,
                weight,
                bones) ||
            changed;
        return changed;
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

        static thread_local std::array<
            HooksWorldPoseCalibration,
            Game::kMaxPlayers> s_calibrations;
        HooksWorldPoseCalibration& calibration =
            s_calibrations[
                static_cast<size_t>(info.entity_index)];
        if (calibration.entity != entity ||
            calibration.studioHdr != layout->studioHdr)
        {
            calibration.Reset(
                entity,
                layout->studioHdr);
        }
        if (!calibration.passiveReferenceValid)
        {
            const bool captured =
                HooksWorldPoseCapturePassiveReferencePose(
                    *layout,
                    sourceBones,
                    calibration);
            if (captured &&
                vr->m_WorldModelVRPoseDebugLog)
            {
                Game::logMsg(
                    "[VR][WorldPose] passive reference captured player=%d bones=%d",
                    info.entity_index,
                calibration.passiveReferenceBones);
            }
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

        // Procedural hair/finger chains in custom survivor models cannot be
        // safely blended between Source's idle pose and a stabilized reference
        // pose: the mixed parent-local transforms visibly fold and scatter
        // those descendants.  During normal VR IK, make the tracked head/hands
        // and their descendants fully authoritative.  The outer blend weight
        // still provides the normal fade back to Source for suppressed states.
        constexpr float kWorldPoseTrackingAuthority = 1.0f;
        constexpr float kWorldPosePassiveAuthority = 1.0f;
        const float trackingWeight =
            std::clamp(
                weight * kWorldPoseTrackingAuthority,
                0.0f,
                1.0f);

        const float sourceBodyYaw =
            HooksWorldPoseWrapAngle(info.angles.y);
        float visualBodyYaw = sourceBodyYaw;
        float appliedBodyYaw = sourceBodyYaw;
        float visualBodyYawError = 0.0f;
        bool visualBodyYawTurning = false;
        HooksWorldPoseUpdateVisualBodyYaw(
            vr,
            sourceBodyYaw,
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
            calibration.rightControllerToHandValid = false;
            calibration.rightGripRootReferenceValid = false;
        }

        const bool hmdTransformValid =
            (pose.validMask & l4d2vr_pose::kValidHmd) != 0u &&
            layout->head >= 0 &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.hmd,
                info,
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
                // The native runtime head and m_angEyeAngles already follow
                // the same HMD pitch/yaw in VR. Calibrate those two live
                // transforms directly. Trying to "neutralize" eye angles here
                // subtracts the tracked look rotation a second time and bakes
                // a large upward pitch into hmdToHead.
                const bool calibrationReady =
                    HooksWorldPoseEnsureCalibration(
                        hmdWorld,
                        sourceBones[layout->head],
                        0.0f,
                        calibration.hmdToHeadValid,
                        calibration.hmdToHead);
                if (calibrationReady &&
                    calibration.hmdToHeadValid)
                {
                    calibration.hmdToHeadUsesSourceEyeAngles = false;
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
            if (!calibration.hmdReferenceLocalPositionValid ||
                !calibration.
                    nativeHeadReferenceLocalPositionValid)
            {
                calibration.hmdReferenceLocalPosition =
                    pose.hmd.position;
                calibration.hmdReferenceLocalPositionValid = true;
                if (sourceNativeHeadLocalValid)
                {
                    calibration.nativeHeadReferenceLocalPosition =
                        sourceNativeHeadLocal;
                    calibration.
                        nativeHeadReferenceLocalPositionValid =
                            true;
                }
            }

            // Source's current head already contains crouch, jump, locomotion
            // and breathing.  Layer only a tightly bounded room-scale HMD
            // displacement over that native head.  In particular, never make
            // the head an absolute child of player origin: that would apply a
            // second crouch displacement and stretch the neck.
            rawHmdLocalDelta =
                pose.hmd.position -
                calibration.hmdReferenceLocalPosition;

            // A large horizontal delta is a player/camera-anchor rebase, not
            // a usable anatomical head offset. Re-anchor it instead of
            // leaving the upper chest permanently pinned at the clamp.
            const float rawHorizontalLength =
                std::sqrt(
                    rawHmdLocalDelta.x *
                        rawHmdLocalDelta.x +
                    rawHmdLocalDelta.y *
                        rawHmdLocalDelta.y);
            constexpr float kHmdHorizontalRebaseDistance = 6.0f;
            if (std::isfinite(rawHorizontalLength) &&
                rawHorizontalLength >
                    kHmdHorizontalRebaseDistance)
            {
                calibration.hmdReferenceLocalPosition.x =
                    pose.hmd.position.x;
                calibration.hmdReferenceLocalPosition.y =
                    pose.hmd.position.y;
                rawHmdLocalDelta.x = 0.0f;
                rawHmdLocalDelta.y = 0.0f;
            }

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
            const float horizontalLength =
                std::sqrt(
                    appliedHmdLocalDelta.x *
                        appliedHmdLocalDelta.x +
                    appliedHmdLocalDelta.y *
                        appliedHmdLocalDelta.y);
            constexpr float kMaximumHmdHorizontalOffset = 3.0f;
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
            appliedHmdLocalDelta.z =
                std::clamp(
                    appliedHmdLocalDelta.z,
                    -2.0f,
                    2.0f);

            Vector bodyForwardForHead{};
            Vector bodyRightForHead{};
            Vector bodyUpForHead{};
            QAngle::AngleVectors(
                QAngle(
                    0.0f,
                    HooksWorldPoseWrapAngle(info.angles.y),
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
            HooksWorldPoseBuildBodyLocalTransform(
                pose.leftHand,
                info,
                leftControllerWorld) &&
            HooksWorldPoseEnsureCalibration(
                leftControllerWorld,
                bones[layout->left.hand],
                0.0f,
                calibration.leftControllerToHandValid,
                calibration.leftControllerToHand);
        bool leftWristRotationOffsetApplied = false;
        if (leftPoseValid)
        {
            vr_vm_stabilize::Mul(
                leftControllerWorld,
                calibration.leftControllerToHand,
                leftHandTarget);
            leftWristRotationOffsetApplied =
                HooksWorldPoseApplyOffhandRotationOffset(
                    vr,
                    leftControllerWorld,
                    leftHandTarget);
        }

        const bool rightPoseValid =
            (pose.validMask &
             l4d2vr_pose::kValidRightHand) != 0u &&
            HooksWorldPoseArmChainValid(
                layout->right,
                layout->parents,
                layout->numBones) &&
            HooksWorldPoseBuildBodyLocalTransform(
                pose.rightHand,
                info,
                rightControllerWorld) &&
            HooksWorldPoseEnsureCalibration(
                rightControllerWorld,
                bones[layout->right.hand],
                0.0f,
                calibration.rightControllerToHandValid,
                calibration.rightControllerToHand);
        bool weaponGripRootAnimationValid = false;
        if (rightPoseValid)
        {
            vr_vm_stabilize::Mul(
                rightControllerWorld,
                calibration.rightControllerToHand,
                rightHandTarget);
            if (heldWeaponForGrip)
            {
                weaponGripRootAnimationValid =
                    HooksWorldPoseApplyGripRootAnimation(
                        *layout,
                        sourceBones,
                        layout->right.hand,
                        calibration.rightGripRootReferenceValid,
                        calibration.rightGripRootReferenceLocal,
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
        if (leftPoseValid)
        {
            changed =
                HooksWorldPoseSolveArm(
                    *layout,
                    layout->left,
                    -1,
                    bodyRight,
                    leftHandTarget,
                    trackingWeight,
                    bones) ||
                changed;
        }
        if (rightPoseValid)
        {
            changed =
                HooksWorldPoseSolveArm(
                    *layout,
                    layout->right,
                    1,
                    bodyRight,
                    rightHandTarget,
                    trackingWeight,
                    bones) ||
                changed;
        }

        // Only the weapon-holding (right) palm/finger branch keeps Source's
        // current local grip and firing animation.  The off hand remains fully
        // stabilized so its native idle cannot fight the left controller.
        const bool preserveWeaponHandGrip =
            rightPoseValid &&
            heldWeaponForGrip != nullptr;

        bool passiveDescendantsChanged = false;
        if (calibration.passiveReferenceValid)
        {
            const bool passiveStabilizationValid =
                HooksWorldPoseStabilizePassiveDescendants(
                    *layout,
                    calibration,
                    sourceBones,
                    hmdPoseValid,
                    leftPoseValid,
                    rightPoseValid &&
                        !preserveWeaponHandGrip,
                    weight *
                        kWorldPosePassiveAuthority,
                    bones,
                    passiveDescendantsChanged);
            if (passiveStabilizationValid)
            {
                changed =
                    passiveDescendantsChanged ||
                    changed;
            }
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
            *layout,
            sourceBones,
            bones,
            rightPoseValid);

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
                Game::logMsg(
                    "[VR][WorldPose] IK player=%d local=%d weight=%.2f tracking=%.2f freshness=%.2f suppressed=%d bodyYaw=%.1f->%.1f applied=%.1f error=%.1f turning=%d head=%d headChain=%d headRef=runtime calib=%s hmdLocalRot=(%.1f %.1f %.1f) hmdWorldRot=(%.1f %.1f %.1f) eyeRot=(%.1f %.1f %.1f) headRot=(%.1f %.1f %.1f)->(%.1f %.1f %.1f) hmdRaw=(%.1f %.1f %.1f) nativeHeadDelta=(%.1f %.1f %.1f) nativeComp=(%.1f %.1f %.1f) hmdDelta=(%.1f %.1f %.1f) neckLen=%.2f->%.2f left=%d leftWristOffset=%d right=%d gripNative=%d gripRoot=%d passive=%d passiveBones=%d",
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
                        ? "eye-neutral"
                        : "hmd",
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
                    headAnglesValid ? finalHeadAngles.z : 0.0f,
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
                    finalNeckLength,
                    leftPoseValid ? 1 : 0,
                    leftWristRotationOffsetApplied ? 1 : 0,
                    rightPoseValid ? 1 : 0,
                    preserveWeaponHandGrip ? 1 : 0,
                    weaponGripRootAnimationValid ? 1 : 0,
                    passiveDescendantsChanged ? 1 : 0,
                    calibration.passiveReferenceBones);
            }
        }

        outBones = bones;
        return true;
    }
