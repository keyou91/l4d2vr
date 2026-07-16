void VR::GetAimLineColor(int& r, int& g, int& b, int& a) const
{
    if (m_SpecialInfectedBlindSpotWarningActive)
    {
        r = m_AimLineWarningColorR;
        g = m_AimLineWarningColorG;
        b = m_AimLineWarningColorB;
    }
    else if (m_EffectiveAttackRangeIndicatorEnabled && m_AimLineEffectiveAttackRangeActive)
    {
        r = m_EffectiveAttackRangeColorR;
        g = m_EffectiveAttackRangeColorG;
        b = m_EffectiveAttackRangeColorB;
    }
    else if (m_SpecialInfectedPreWarningActive)
    {
        r = m_AimLineWarningColorR;
        g = m_AimLineWarningColorG;
        b = m_AimLineWarningColorB;
    }
    else
    {
        r = m_AimLineColorR;
        g = m_AimLineColorG;
        b = m_AimLineColorB;
    }

	a = m_AimLineColorA;
	const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
	if (m_ScopeAimLineOnlyInScope
		&& m_ThirdPersonFrontViewEnabled
		&& m_IsThirdPersonCamera
		&& m_ScopeWeaponIsFirearm
		&& queueMode == 0
		&& !m_ScopeRenderingPass)
		a = 0;
}


namespace vr_render_snapshot_cache_viewmodel
{
    struct ViewCache
    {
        uint32_t seq = 0;
        Vector viewAng{ 0.0f, 0.0f, 0.0f };
        Vector viewLeft{ 0.0f, 0.0f, 0.0f };
        Vector viewRight{ 0.0f, 0.0f, 0.0f };
    };

    inline ViewCache& TLS()
    {
        static thread_local ViewCache cache{};
        return cache;
    }

    inline bool Refresh(const VR* vr, ViewCache& c)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const uint32_t s1 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u))
                continue;

            const float ax = vr->m_RenderViewAngX.load(std::memory_order_relaxed);
            const float ay = vr->m_RenderViewAngY.load(std::memory_order_relaxed);
            const float az = vr->m_RenderViewAngZ.load(std::memory_order_relaxed);

            const float lx = vr->m_RenderViewOriginLeftX.load(std::memory_order_relaxed);
            const float ly = vr->m_RenderViewOriginLeftY.load(std::memory_order_relaxed);
            const float lz = vr->m_RenderViewOriginLeftZ.load(std::memory_order_relaxed);

            const float rx = vr->m_RenderViewOriginRightX.load(std::memory_order_relaxed);
            const float ry = vr->m_RenderViewOriginRightY.load(std::memory_order_relaxed);
            const float rz = vr->m_RenderViewOriginRightZ.load(std::memory_order_relaxed);

            const uint32_t s2 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == s2 && !(s2 & 1u))
            {
                c.seq = s2;
                c.viewAng = Vector(ax, ay, az);
                c.viewLeft = Vector(lx, ly, lz);
                c.viewRight = Vector(rx, ry, rz);
                return true;
            }
        }
        return false;
    }

    inline bool Get(const VR* vr, ViewCache& c)
    {
        if (!VR::t_UseRenderFrameSnapshot)
            return false;

        const uint32_t s = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
        if (s != 0 && !(s & 1u) && s == c.seq)
            return true;

        return Refresh(vr, c);
    }
}

Vector VR::GetViewAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_viewmodel::TLS();
        if (vr_render_snapshot_cache_viewmodel::Get(this, cache))
            return cache.viewAng;
    }

    return Vector(m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z);
}


float VR::GetMovementYawDeg()
{
    if (!m_MoveDirectionFromController)
    {
        if (m_MouseModeEnabled)
        {
            // In mouse mode, locomotion follows the body yaw (turning), not head yaw.
            float yaw = m_RotationOffset;
            // Wrap to [-180, 180]
            yaw -= 360.0f * std::floor((yaw + 180.0f) / 360.0f);
            return yaw;
        }

        Vector hmdAng = GetViewAngle();
        return hmdAng.y;
    }

    // Use the dominant (right) controller yaw as the movement basis.
    // This is intentionally yaw-only; pitch/roll should not affect locomotion.
    QAngle ctrlAng = GetRightControllerAbsAngle();
    return ctrlAng.y;
}

Vector VR::GetViewOriginLeft()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_viewmodel::TLS();
        if (vr_render_snapshot_cache_viewmodel::Get(this, cache))
            return cache.viewLeft;
    }

    Vector viewOriginLeft;

    viewOriginLeft = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginLeft = viewOriginLeft + (m_HmdRight * (-((m_Ipd * m_IpdScale * m_VRScale) / 2)));

    return viewOriginLeft;
}


Vector VR::GetViewOriginRight()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_viewmodel::TLS();
        if (vr_render_snapshot_cache_viewmodel::Get(this, cache))
            return cache.viewRight;
    }

    Vector viewOriginRight;

    viewOriginRight = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginRight = viewOriginRight + (m_HmdRight * (m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}



bool VR::CanApplyResetPositionNow() const
{
    return m_ResetPositionStableFrames.load(std::memory_order_acquire) >= kResetPositionStableFramesRequired;
}

void VR::ResetPosition()
{
    if (m_TeleportVisualScoutActive)
    {
        // A detached scout camera must not be pulled back by manual or automatic
        // VR recenter paths. Use is the explicit return-to-body action.
        return;
    }

    if (!CanApplyResetPositionNow())
    {
        m_ResetPositionDeferredPending.store(1u, std::memory_order_release);
        return;
    }

    m_ResetPositionDeferredPending.store(0u, std::memory_order_release);
    m_CameraAnchor += m_SetupOrigin - m_HmdPosAbs;
    m_HeightOffset += m_SetupOrigin.z - m_HmdPosAbs.z;
    m_Roomscale1To1PrevValid = false;
    m_Roomscale1To1PrevCorrectedAbs = {};
    m_Roomscale1To1LastEngineEyeValid = false;
    m_Roomscale1To1LastEngineEye = {};
    m_Roomscale1To1PendingVisualWorldDeltaValid = false;
    m_Roomscale1To1PendingVisualWorldDelta = {};
    ClearRoomscale1To1ServerMoveDelta();
    CancelTeleportTargeting();
    ClearTeleportServerTarget();
    ClearTeleportVisualScout();
    m_Roomscale1To1StandingHmdZValid = false;
    m_Roomscale1To1PhysicalCrouchActive = false;
}

std::string VR::GetMeleeWeaponName(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return std::string();

    if (!m_Game || !m_Game->m_Offsets)
    {
        return std::string();
    }

    typedef CMeleeWeaponInfoStore* (__thiscall* tGetMeleeWepInfo)(void* thisptr);
    static tGetMeleeWepInfo oGetMeleeWepInfo = nullptr;

    if (!oGetMeleeWepInfo)
        oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfoClient.address);

    if (!oGetMeleeWepInfo)
        return std::string();

    CMeleeWeaponInfoStore* meleeWepInfo = oGetMeleeWepInfo(weapon);
    if (!meleeWepInfo)
        return std::string();

    std::string meleeName(meleeWepInfo->meleeWeaponName);
    std::transform(meleeName.begin(), meleeName.end(), meleeName.begin(), ::tolower);
    return meleeName;
}

std::string VR::WeaponIdToString(int weaponId) const
{
    static const std::vector<std::string> weaponNames =
    {
        "none",
        "pistol",
        "uzi",
        "pumpshotgun",
        "autoshotgun",
        "m16a1",
        "hunting_rifle",
        "mac10",
        "shotgun_chrome",
        "scar",
        "sniper_military",
        "spas",
        "first_aid_kit",
        "molotov",
        "pipe_bomb",
        "pain_pills",
        "gascan",
        "propane_tank",
        "oxygen_tank",
        "melee",
        "chainsaw",
        "grenade_launcher",
        "ammo_pack",
        "adrenaline",
        "defibrillator",
        "vomitjar",
        "ak47",
        "gnome_chompski",
        "cola_bottles",
        "fireworks_box",
        "incendiary_ammo",
        "frag_ammo",
        "magnum",
        "mp5",
        "sg552",
        "awp",
        "scout",
        "m60",
        "tank_claw",
        "hunter_claw",
        "charger_claw",
        "boomer_claw",
        "smoker_claw",
        "spitter_claw",
        "jockey_claw",
        "machinegun",
        "vomit",
        "splat",
        "pounce",
        "lounge",
        "pull",
        "choke",
        "rock",
        "physics",
        "ammo",
        "upgrade_item"
    };

    size_t index = static_cast<size_t>(weaponId);
    if (index < weaponNames.size())
        return weaponNames[index];

    return std::string();
}

std::string VR::NormalizeViewmodelAdjustKey(const std::string& rawKey) const
{
    const std::string weaponPrefix = "weapon:";
    if (rawKey.rfind(weaponPrefix, 0) == 0)
    {
        std::string weaponIdString = rawKey.substr(weaponPrefix.size());
        if (!weaponIdString.empty() &&
            std::all_of(weaponIdString.begin(), weaponIdString.end(), [](unsigned char c) { return std::isdigit(c); }))
        {
            try
            {
                int weaponId = std::stoi(weaponIdString);
                std::string weaponName = WeaponIdToString(weaponId);
                if (!weaponName.empty())
                    return weaponPrefix + weaponName;
            }
            catch (...)
            {
            }
        }
    }

    return rawKey;
}

std::string VR::BuildViewmodelAdjustKey(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return "weapon:none";

    C_WeaponCSBase::WeaponID weaponId = weapon->GetWeaponID();

    if (weaponId == C_WeaponCSBase::WeaponID::MELEE)
    {
        std::string meleeName = GetMeleeWeaponName(weapon);
        if (!meleeName.empty())
            return "melee:" + meleeName;

        return "melee:unknown";
    }

    std::string weaponName = WeaponIdToString(static_cast<int>(weaponId));
    if (!weaponName.empty())
        return "weapon:" + weaponName;

    return "weapon:" + std::to_string(static_cast<int>(weaponId));
}

ViewmodelAdjustment& VR::EnsureViewmodelAdjustment(const std::string& key)
{
    auto [it, inserted] = m_ViewmodelAdjustments.emplace(key, m_DefaultViewmodelAdjust);
    return it->second;
}

void VR::RefreshActiveViewmodelAdjustment(C_BasePlayer* localPlayer)
{
    C_WeaponCSBase* activeWeapon = localPlayer ? (C_WeaponCSBase*)localPlayer->GetActiveWeapon() : nullptr;
    std::string adjustKey = BuildViewmodelAdjustKey(activeWeapon);

    m_CurrentViewmodelKey = adjustKey;

    if (m_LastLoggedViewmodelKey != m_CurrentViewmodelKey)
    {
        m_LastLoggedViewmodelKey = m_CurrentViewmodelKey;
    }

    ViewmodelAdjustment& adjustment = EnsureViewmodelAdjustment(adjustKey);
    m_ViewmodelPosAdjust = adjustment.position;
    m_ViewmodelAngAdjust = adjustment.angle;
}

void VR::LoadViewmodelAdjustments()
{
    m_ViewmodelAdjustments.clear();

    if (m_ViewmodelAdjustmentSavePath.empty())
    {
        ParseHapticsConfigFile();
        return;
    }

    std::ifstream adjustmentStream(m_ViewmodelAdjustmentSavePath);
    if (!adjustmentStream)
    {
        return;
    }

    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    auto parseVector3 = [&](const std::string& raw, const Vector& defaults)->Vector
        {
            Vector result = defaults;
            std::stringstream ss(raw);
            std::string token;
            float* components[3] = { &result.x, &result.y, &result.z };
            int index = 0;

            while (std::getline(ss, token, ',') && index < 3)
            {
                trim(token);
                if (!token.empty())
                {
                    try
                    {
                        *components[index] = std::stof(token);
                    }
                    catch (...)
                    {
                    }
                }
                ++index;
            }

            return result;
        };

    std::string line;
    while (std::getline(adjustmentStream, line))
    {
        trim(line);
        if (line.empty())
            continue;

        size_t eq = line.find('=');
        size_t separator = line.find(';', eq == std::string::npos ? 0 : eq + 1);

        if (eq == std::string::npos || separator == std::string::npos || separator <= eq)
            continue;

        std::string key = line.substr(0, eq);
        trim(key);
        if (key.empty())
            continue;

        std::string posStr = line.substr(eq + 1, separator - eq - 1);
        std::string angStr = line.substr(separator + 1);

        std::string normalizedKey = NormalizeViewmodelAdjustKey(key);

        Vector posAdjust = parseVector3(posStr, m_DefaultViewmodelAdjust.position);
        Vector angAdjustVec = parseVector3(angStr, Vector{ m_DefaultViewmodelAdjust.angle.x, m_DefaultViewmodelAdjust.angle.y, m_DefaultViewmodelAdjust.angle.z });

        m_ViewmodelAdjustments[normalizedKey] = { posAdjust, { angAdjustVec.x, angAdjustVec.y, angAdjustVec.z } };
    }

    m_ViewmodelAdjustmentsDirty = false;
}

void VR::SaveViewmodelAdjustments()
{
    if (m_ViewmodelAdjustmentSavePath.empty())
    {
        return;
    }

    std::ofstream adjustmentStream(m_ViewmodelAdjustmentSavePath, std::ios::trunc);
    if (!adjustmentStream)
    {
        return;
    }

    for (const auto& [key, adjustment] : m_ViewmodelAdjustments)
    {
        adjustmentStream << key << '=' << adjustment.position.x << ',' << adjustment.position.y << ',' << adjustment.position.z;
        adjustmentStream << ';' << adjustment.angle.x << ',' << adjustment.angle.y << ',' << adjustment.angle.z << "\n";
    }

    m_ViewmodelAdjustmentsDirty = false;
}


std::string VR::BuildScopeAdjustKey(C_WeaponCSBase* weapon) const
{
    return BuildViewmodelAdjustKey(weapon);
}

ScopeAdjustment& VR::EnsureScopeAdjustment(const std::string& key)
{
    auto [it, inserted] = m_ScopeAdjustments.emplace(key, m_DefaultScopeAdjust);
    return it->second;
}

void VR::RefreshActiveScopeAdjustment(C_BasePlayer* localPlayer)
{
    C_WeaponCSBase* activeWeapon = localPlayer ? (C_WeaponCSBase*)localPlayer->GetActiveWeapon() : nullptr;
    std::string adjustKey = BuildScopeAdjustKey(activeWeapon);

    m_CurrentScopeAdjustmentKey = adjustKey;

    if (m_LastLoggedScopeAdjustmentKey != m_CurrentScopeAdjustmentKey)
    {
        m_LastLoggedScopeAdjustmentKey = m_CurrentScopeAdjustmentKey;
    }

    ScopeAdjustment& adjustment = EnsureScopeAdjustment(adjustKey);
    m_ScopeFov = std::clamp(adjustment.fov, m_ScopeFovMin, m_ScopeFovMax);
    m_ScopeOverlayWidthMeters = std::clamp(adjustment.widthMeters, m_ScopeSizeMin, m_ScopeSizeMax);
    m_ScopeOverlayXOffset = adjustment.overlayOffset.x;
    m_ScopeOverlayYOffset = adjustment.overlayOffset.y;
    m_ScopeOverlayZOffset = adjustment.overlayOffset.z;
}

void VR::LoadScopeAdjustments()
{
    m_ScopeAdjustments.clear();

    if (m_ScopeAdjustmentSavePath.empty())
        return;

    std::ifstream adjustmentStream(m_ScopeAdjustmentSavePath);
    if (!adjustmentStream)
        return;

    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    auto parseFloat = [&](const std::string& raw, float fallback)->float
        {
            try
            {
                size_t parsed = 0;
                float value = std::stof(raw, &parsed);
                if (parsed == 0 || !std::isfinite(value))
                    return fallback;
                return value;
            }
            catch (...)
            {
                return fallback;
            }
        };

    auto parseVector3 = [&](const std::string& raw, const Vector& defaults)->Vector
        {
            Vector result = defaults;
            std::stringstream ss(raw);
            std::string token;
            float* components[3] = { &result.x, &result.y, &result.z };
            int index = 0;

            while (std::getline(ss, token, ',') && index < 3)
            {
                trim(token);
                if (!token.empty())
                    *components[index] = parseFloat(token, *components[index]);
                ++index;
            }

            return result;
        };

    std::string line;
    while (std::getline(adjustmentStream, line))
    {
        trim(line);
        if (line.empty())
            continue;

        size_t eq = line.find('=');
        size_t firstSeparator = line.find(';', eq == std::string::npos ? 0 : eq + 1);
        size_t secondSeparator = firstSeparator == std::string::npos ? std::string::npos : line.find(';', firstSeparator + 1);

        if (eq == std::string::npos || firstSeparator == std::string::npos || secondSeparator == std::string::npos || firstSeparator <= eq)
            continue;

        std::string key = line.substr(0, eq);
        trim(key);
        if (key.empty())
            continue;

        std::string widthStr = line.substr(eq + 1, firstSeparator - eq - 1);
        std::string offsetStr = line.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1);
        std::string fovStr = line.substr(secondSeparator + 1);
        trim(widthStr);
        trim(offsetStr);
        trim(fovStr);

        ScopeAdjustment adjustment = m_DefaultScopeAdjust;
        adjustment.widthMeters = parseFloat(widthStr, adjustment.widthMeters);
        adjustment.overlayOffset = parseVector3(offsetStr, adjustment.overlayOffset);
        adjustment.fov = parseFloat(fovStr, adjustment.fov);

        std::string normalizedKey = NormalizeViewmodelAdjustKey(key);
        m_ScopeAdjustments[normalizedKey] = adjustment;
    }

    m_ScopeAdjustmentsDirty = false;
}

void VR::SaveScopeAdjustments()
{
    if (m_ScopeAdjustmentSavePath.empty())
        return;

    std::ofstream adjustmentStream(m_ScopeAdjustmentSavePath, std::ios::trunc);
    if (!adjustmentStream)
        return;

    for (const auto& [key, adjustment] : m_ScopeAdjustments)
    {
        const float widthMeters = std::clamp(adjustment.widthMeters, m_ScopeSizeMin, m_ScopeSizeMax);
        const float fov = std::clamp(adjustment.fov, m_ScopeFovMin, m_ScopeFovMax);
        adjustmentStream << key << '=' << widthMeters;
        adjustmentStream << ';' << adjustment.overlayOffset.x << ',' << adjustment.overlayOffset.y << ',' << adjustment.overlayOffset.z;
        adjustmentStream << ';' << fov << "\n";
    }

    m_ScopeAdjustmentsDirty = false;
}

void VR::ParseConfigFile()
{
    //  򵥵  trim
    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    std::unordered_map<std::string, std::string> userConfig;
    size_t parsedEntryCount = 0;

    auto configValueAllowsSemicolon = [&](std::string key)->bool
        {
            trim(key);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            static constexpr char overrideSuffix[] = "overrides";
            const size_t suffixLength = sizeof(overrideSuffix) - 1;
            return key.size() >= suffixLength &&
                key.compare(key.size() - suffixLength, suffixLength, overrideSuffix) == 0;
        };

    auto configValueAllowsHash = [&](std::string key)->bool
        {
            trim(key);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            return key == "manualreloadmagazineboneoverrides" ||
                key == "magazineinteractionboltboneoverrides";
        };

    auto parseConfigPath = [&](const char* path)->bool
        {
            std::ifstream configStream(path);
            if (!configStream)
                return false;

            std::string line;
            while (std::getline(configStream, line))
            {
                // ȥ  ע  
                size_t cut = std::string::npos;
                size_t p1 = line.find("//");
                size_t p2 = line.find('#');
                size_t eqBeforeComment = line.find('=');
                size_t p3 = line.find(';');
                if (p1 != std::string::npos) cut = p1;
                if (p2 != std::string::npos &&
                    (eqBeforeComment == std::string::npos ||
                        p2 < eqBeforeComment ||
                        !configValueAllowsHash(line.substr(0, eqBeforeComment))))
                {
                    cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
                }
                if (p3 != std::string::npos &&
                    (eqBeforeComment == std::string::npos ||
                        p3 < eqBeforeComment ||
                        !configValueAllowsSemicolon(line.substr(0, eqBeforeComment))))
                {
                    cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
                }
                if (cut != std::string::npos) line.erase(cut);

                trim(line);
                if (line.empty()) continue;

                //      key=value
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;

                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                trim(key); trim(value);
                if (key.size() >= 3 &&
                    static_cast<unsigned char>(key[0]) == 0xEF &&
                    static_cast<unsigned char>(key[1]) == 0xBB &&
                    static_cast<unsigned char>(key[2]) == 0xBF)
                {
                    key.erase(0, 3);
                    trim(key);
                }
                if (!key.empty())
                {
                    userConfig[key] = value;
                    ++parsedEntryCount;
                }
            }

            return true;
        };

    const bool loadedConfig = parseConfigPath("VR\\config.txt");
    const bool loadedConfig2 = parseConfigPath("VR\\config2.txt");
    if (!loadedConfig && !loadedConfig2)
        return;

    // С   ߣ   Ĭ  ֵ İ ȫ  ȡ
    auto hasConfigKey = [&](const char* k)->bool {
        return userConfig.find(k) != userConfig.end();
        };
    auto getBool = [&](const char* k, bool defVal)->bool {
        auto it = userConfig.find(k);
        if (it == userConfig.end()) return defVal;
        std::string v = it->second;
        // תСд
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
        if (v == "0" || v == "false" || v == "off" || v == "no")  return false;
        return defVal;
        };
    auto getFloat = [&](const char* k, float defVal)->float {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "true" || v == "on" || v == "yes") return 1.0f;
        if (v == "false" || v == "off" || v == "no") return 0.0f;
        try { return std::stof(it->second); }
        catch (...) { return defVal; }
        };
    auto getInt = [&](const char* k, int defVal)->int {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "true" || v == "on" || v == "yes") return 1;
        if (v == "false" || v == "off" || v == "no") return 0;
        try { return std::stoi(it->second); }
        catch (...) { return defVal; }
        };
    auto getColor = [&](const char* k, int defR, int defG, int defB, int defA)->std::array<int, 4> {
        std::array<int, 4> defaults{ defR, defG, defB, defA };
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defaults;

        std::array<int, 4> color = defaults;
        std::stringstream ss(it->second);
        std::string token;
        int index = 0;
        while (std::getline(ss, token, ',') && index < 4)
        {
            trim(token);
            if (!token.empty())
            {
                try
                {
                    color[index] = std::clamp(std::stoi(token), 0, 255);
                }
                catch (...)
                {
                    color[index] = defaults[index];
                }
            }
            ++index;
        }

        for (int& component : color)
        {
            component = std::clamp(component, 0, 255);
        }

        return color;
        };
    auto getVector3 = [&](const char* k, const Vector& defVal)->Vector {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        Vector result = defVal;
        std::stringstream ss(it->second);
        std::string token;
        float* components[3] = { &result.x, &result.y, &result.z };
        int index = 0;

        while (std::getline(ss, token, ',') && index < 3)
        {
            trim(token);
            if (!token.empty())
            {
                try
                {
                    *components[index] = std::stof(token);
                }
                catch (...)
                {
                }
            }
            ++index;
        }

        return result;
        };

    auto getFloatList = [&](const char* k, const char* defVal = nullptr) -> std::vector<float>
        {
            std::vector<float> values;
            const auto it = userConfig.find(k);
            if (it == userConfig.end() && !defVal)
                return values;

            const std::string source = (it == userConfig.end()) ? defVal : it->second;
            std::stringstream ss(source);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                trim(token);
                if (token.empty())
                    continue;

                try
                {
                    values.push_back(std::stof(token));
                }
                catch (...)
                {
                }
            }

            return values;
        };

    auto getString = [&](const char* k, const std::string& defVal)->std::string {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        std::string value = it->second;
        trim(value);

        return value.empty() ? defVal : value;
        };

    auto getStringList = [&](const char* k, const char separator = ',')->std::vector<std::string> {
        std::vector<std::string> values;
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty())
            return values;

        std::stringstream ss(it->second);
        std::string token;
        while (std::getline(ss, token, separator))
        {
            trim(token);
            if (!token.empty())
                values.push_back(token);
        }
        return values;
        };

    const std::string injectedCmd = getString("cmd", getString("Cmd", "exec cam.cfg"));
    if (!injectedCmd.empty())
    {
        m_Game->ClientCmd_Unrestricted(injectedCmd.c_str());
    }

    auto parseVirtualKey = [&](const std::string& rawValue)->std::optional<WORD>
        {
            std::string value = rawValue;
            trim(value);
            if (value.size() < 5) // key:<x>
                return std::nullopt;

            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });

            const std::string prefix = "key:";
            if (value.rfind(prefix, 0) != 0)
                return std::nullopt;

            std::string keyToken = value.substr(prefix.size());
            trim(keyToken);
            if (keyToken.empty())
                return std::nullopt;

            static const std::unordered_map<std::string, WORD> keyLookup = {
                { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN },
                { "tab", VK_TAB }, { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE },
                { "shift", VK_SHIFT }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
                { "ctrl", VK_CONTROL }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
                { "alt", VK_MENU }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU },
                { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "insert", VK_INSERT },
                { "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
                { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }
            };

            auto lookupIt = keyLookup.find(keyToken);
            if (lookupIt != keyLookup.end())
                return lookupIt->second;

            if (keyToken.size() == 1)
            {
                char ch = keyToken[0];
                if (ch >= 'a' && ch <= 'z')
                    return static_cast<WORD>('A' + (ch - 'a'));
                if (ch >= '0' && ch <= '9')
                    return static_cast<WORD>(ch);
            }

            if (keyToken.size() > 1 && keyToken[0] == 'f')
            {
                try
                {
                    int functionIndex = std::stoi(keyToken.substr(1));
                    if (functionIndex >= 1 && functionIndex <= 24)
                        return static_cast<WORD>(VK_F1 + functionIndex - 1);
                }
                catch (...)
                {
                }
            }

            return std::nullopt;
        };

    auto parseCustomActionBinding = [&](const char* key, CustomActionBinding& binding)
        {
            binding.command = getString(key, binding.command);
            binding.holdVirtualKey = false;
            binding.releaseCommand.clear();
            binding.usePressReleaseCommands = false;

            std::string trimmedCommand = binding.command;
            trim(trimmedCommand);
            std::string normalized = trimmedCommand;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return std::tolower(c); });

            const std::string holdPrefix = "hold:";
            if (normalized.rfind(holdPrefix, 0) == 0)
            {
                binding.holdVirtualKey = true;
                trimmedCommand = trimmedCommand.substr(holdPrefix.size());
                trim(trimmedCommand);
                binding.command = trimmedCommand;
            }
            // Custom alias helper: CustomActionXCommand=alias:aliasName:+speed|wait 30|-speed
            // The section after the alias name uses '|' as a stand-in for ';' to avoid config comment stripping.
            const std::string aliasPrefix = "alias:";
            if (normalized.rfind(aliasPrefix, 0) == 0)
            {
                std::string aliasDefinition = trimmedCommand.substr(aliasPrefix.size());
                trim(aliasDefinition);

                size_t separator = aliasDefinition.find(':');
                if (separator != std::string::npos)
                {
                    std::string aliasName = aliasDefinition.substr(0, separator);
                    std::string aliasBody = aliasDefinition.substr(separator + 1);
                    trim(aliasName);
                    trim(aliasBody);

                    if (!aliasName.empty() && !aliasBody.empty())
                    {
                        std::replace(aliasBody.begin(), aliasBody.end(), '|', ';');
                        std::string aliasCommand = "alias " + aliasName + " \"" + aliasBody + "\"";
                        m_Game->ClientCmd_Unrestricted(aliasCommand.c_str());

                        binding.command = aliasName;
                        normalized = aliasName;
                    }
                }
            }

            binding.virtualKey = parseVirtualKey(binding.command);
            if (!binding.command.empty() && binding.virtualKey.has_value())
            {
            }
            else if (!trimmedCommand.empty() && trimmedCommand[0] == '+' && trimmedCommand.size() > 1)
            {
                binding.usePressReleaseCommands = true;
                binding.releaseCommand = "-" + trimmedCommand.substr(1);
                binding.command = trimmedCommand;
            }
        };


    m_SnapTurnAngle = getFloat("SnapTurnAngle", m_SnapTurnAngle);
    m_TurnSpeed = getFloat("TurnSpeed", m_TurnSpeed);
    // Locomotion direction: default is HMD-yaw-based. Optional hand-yaw-based.
    m_MoveDirectionFromController = getBool("MoveDirectionFromController", m_MoveDirectionFromController);
    m_MoveDirectionFromController = getBool("MovementDirectionFromController", m_MoveDirectionFromController);

    m_InventoryGestureRange = getFloat("InventoryGestureRange", m_InventoryGestureRange);
    m_InventoryChestOffset = getVector3("InventoryChestOffset", m_InventoryChestOffset);
    m_InventoryBackOffset = getVector3("InventoryBackOffset", m_InventoryBackOffset);
    m_InventoryLeftWaistOffset = getVector3("InventoryLeftWaistOffset", m_InventoryLeftWaistOffset);
    m_InventoryRightWaistOffset = getVector3("InventoryRightWaistOffset", m_InventoryRightWaistOffset);
    m_InventoryBodyOriginOffset = getVector3("InventoryBodyOriginOffset", m_InventoryBodyOriginOffset);
    m_InventoryHudMarkerDistance = getFloat("InventoryHudMarkerDistance", m_InventoryHudMarkerDistance);
    m_InventoryHudMarkerUpOffset = getFloat("InventoryHudMarkerUpOffset", m_InventoryHudMarkerUpOffset);
    m_InventoryHudMarkerSeparation = getFloat("InventoryHudMarkerSeparation", m_InventoryHudMarkerSeparation);

    m_InventoryQuickSwitchEnabled = getBool("InventoryQuickSwitchEnabled", m_InventoryQuickSwitchEnabled);
    m_InventoryQuickSwitchOffset = getVector3("InventoryQuickSwitchOffset", m_InventoryQuickSwitchOffset);
    m_InventoryQuickSwitchZoneRadius = getFloat("InventoryQuickSwitchZoneRadius", m_InventoryQuickSwitchZoneRadius);
    if (!m_InventoryQuickSwitchEnabled)
    {
        m_InventoryQuickSwitchActive = false;
        m_InventoryQuickSwitchArmed = false;
    }
    m_DrawInventoryAnchors = getBool("ShowInventoryAnchors", m_DrawInventoryAnchors);
    const auto inventoryColor = getColor("InventoryAnchorColor", m_InventoryAnchorColorR, m_InventoryAnchorColorG, m_InventoryAnchorColorB, m_InventoryAnchorColorA);
    m_InventoryAnchorColorR = inventoryColor[0];
    m_InventoryAnchorColorG = inventoryColor[1];
    m_InventoryAnchorColorB = inventoryColor[2];
    m_InventoryAnchorColorA = inventoryColor[3];
    const std::unordered_map<std::string, vr::VRActionHandle_t*> actionLookup =
    {
        {"jump", &m_ActionJump},
        {"primaryattack", &m_ActionPrimaryAttack},
        {"secondaryattack", &m_ActionSecondaryAttack},
        {"reload", &m_ActionReload},
        {"walk", &m_ActionWalk},
        {"turn", &m_ActionTurn},
        {"use", &m_ActionUse},
        {"nextitem", &m_ActionNextItem},
        {"previtem", &m_ActionPrevItem},
        {"resetposition", &m_ActionResetPosition},
        {"crouch", &m_ActionCrouch},
        {"flashlight", &m_ActionFlashlight},
        {"activatevr", &m_ActionActivateVR},
        {"menuselect", &m_MenuSelect},
        {"menuback", &m_MenuBack},
        {"menuup", &m_MenuUp},
        {"menudown", &m_MenuDown},
        {"menuleft", &m_MenuLeft},
        {"menuright", &m_MenuRight},
        {"spray", &m_Spray},
        {"scoreboard", &m_Scoreboard},
        {"showhud", &m_ToggleHUD},
        // Compatibility alias for existing combo settings.
        {"togglehud", &m_ToggleHUD},
        {"pause", &m_Pause}
    };

    auto parseActionCombo = [&](const char* key, const ActionCombo& defaultCombo) -> ActionCombo
        {
            auto it = userConfig.find(key);
            if (it == userConfig.end())
                return defaultCombo;

            std::string rawValue = it->second;
            trim(rawValue);
            std::transform(rawValue.begin(), rawValue.end(), rawValue.begin(), [](unsigned char c) { return std::tolower(c); });

            // Allow disabling a combo by setting it to "false" in the config file.
            if (rawValue == "false")
            {
                return ActionCombo{};
            }

            ActionCombo combo{};
            std::stringstream ss(rawValue);
            std::string token;
            int index = 0;
            while (std::getline(ss, token, '+') && index < 2)
            {
                trim(token);
                std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::tolower(c); });
                if (token.empty())
                {
                    ++index;
                    continue;
                }

                auto actionIt = actionLookup.find(token);
                if (actionIt != actionLookup.end())
                {
                    if (index == 0)
                        combo.primary = actionIt->second;
                    else
                        combo.secondary = actionIt->second;
                }
                ++index;
            }

            if (!combo.primary || !combo.secondary)
            {
                return defaultCombo;
            }

            return combo;
        };

    m_VoiceRecordCombo = parseActionCombo("VoiceRecordCombo", m_VoiceRecordCombo);
    m_QuickTurnCombo = parseActionCombo("QuickTurnCombo", m_QuickTurnCombo);
    m_ViewmodelAdjustCombo = parseActionCombo("ViewmodelAdjustCombo", m_ViewmodelAdjustCombo);
    parseCustomActionBinding("CustomAction1Command", m_CustomAction1Binding);
    parseCustomActionBinding("CustomAction2Command", m_CustomAction2Binding);
    parseCustomActionBinding("CustomAction3Command", m_CustomAction3Binding);
    parseCustomActionBinding("CustomAction4Command", m_CustomAction4Binding);
    parseCustomActionBinding("CustomAction5Command", m_CustomAction5Binding);

    m_LeftHanded = getBool("LeftHanded", m_LeftHanded);
    m_LeftHandedSwapInputActions = getBool("LeftHandedSwapInputActions", m_LeftHandedSwapInputActions);
    m_VRScale = getFloat("VRScale", m_VRScale);
    m_IpdScale = getFloat("IPDScale", m_IpdScale);
    {
        auto clampVector = [](Vector value, float minValue, float maxValue)
        {
            value.x = std::clamp(value.x, minValue, maxValue);
            value.y = std::clamp(value.y, minValue, maxValue);
            value.z = std::clamp(value.z, minValue, maxValue);
            return value;
        };
        m_VrHandsLeftPoseOffsetMeters = clampVector(getVector3("VrHandsLeftPoseOffsetMeters", m_VrHandsLeftPoseOffsetMeters), -1.0f, 1.0f);
        m_VrHandsLeftPoseRotationOffsetDeg = clampVector(getVector3("VrHandsLeftPoseRotationOffsetDeg", m_VrHandsLeftPoseRotationOffsetDeg), -180.0f, 180.0f);
        m_VrHandsRightPoseOffsetMeters = clampVector(getVector3("VrHandsRightPoseOffsetMeters", m_VrHandsRightPoseOffsetMeters), -1.0f, 1.0f);
        m_VrHandsRightPoseRotationOffsetDeg = clampVector(getVector3("VrHandsRightPoseRotationOffsetDeg", m_VrHandsRightPoseRotationOffsetDeg), -180.0f, 180.0f);
        m_VrHandsLeftHandedViewmodelPoseOffsetMeters = clampVector(getVector3("VrHandsLeftHandedViewmodelPoseOffsetMeters", m_VrHandsLeftHandedViewmodelPoseOffsetMeters), -1.0f, 1.0f);
        m_VrHandsLeftHandedViewmodelPoseRotationOffsetDeg = clampVector(getVector3("VrHandsLeftHandedViewmodelPoseRotationOffsetDeg", m_VrHandsLeftHandedViewmodelPoseRotationOffsetDeg), -180.0f, 180.0f);
    }
    // Built-in config overlay placement. Keep this in the normal hot-reload path.
    m_ConfigOverlayDistanceMeters = std::clamp(getFloat("ConfigOverlayDistanceMeters", m_ConfigOverlayDistanceMeters), 0.6f, 3.0f);
    m_ConfigOverlaySizeMeters = std::clamp(getFloat("ConfigOverlaySizeMeters", m_ConfigOverlaySizeMeters), 0.8f, 4.0f);
    m_AutoRecenterSmooth = getBool("AutoRecenterSmooth", m_AutoRecenterSmooth);
    m_AutoRecenterSoftStartDistance = std::clamp(getFloat("AutoRecenterSoftStartDistance", m_AutoRecenterSoftStartDistance), 0.0f, 150.0f);
    m_AutoRecenterMaxSpeed = std::clamp(getFloat("AutoRecenterMaxSpeed", m_AutoRecenterMaxSpeed), 0.0f, 300.0f);
    m_AutoRecenterHardDistance = std::max(
        m_AutoRecenterSoftStartDistance + 1.0f,
        getFloat("AutoRecenterHardDistance", m_AutoRecenterHardDistance));
    m_ThirdPersonVRCameraOffset = std::max(0.0f, getFloat("ThirdPersonVRCameraOffset", m_ThirdPersonVRCameraOffset));
    {
        // Backward-compatible parsing:
        // - old form: ThirdPersonFrontVRCameraOffset=80
        // - new form: ThirdPersonFrontVRCameraOffset=80,0,0
        const float legacyForward = getFloat("ThirdPersonFrontVRCameraOffset", m_ThirdPersonVRCameraOffset);
        const Vector frontDefault = { legacyForward, 0.0f, 0.0f };
        m_ThirdPersonFrontVRCameraOffset = getVector3("ThirdPersonFrontVRCameraOffset", frontDefault);
        // Optional per-axis overrides for easier live tuning.
        m_ThirdPersonFrontVRCameraOffset.x = getFloat("ThirdPersonFrontVRCameraOffsetX", m_ThirdPersonFrontVRCameraOffset.x);
        m_ThirdPersonFrontVRCameraOffset.y = getFloat("ThirdPersonFrontVRCameraOffsetY", m_ThirdPersonFrontVRCameraOffset.y);
        m_ThirdPersonFrontVRCameraOffset.z = getFloat("ThirdPersonFrontVRCameraOffsetZ", m_ThirdPersonFrontVRCameraOffset.z);
    }
    m_ThirdPersonCameraSmoothing = std::clamp(getFloat("ThirdPersonCameraSmoothing", m_ThirdPersonCameraSmoothing), 0.0f, 0.99f);
    m_ThirdPersonMapLoadCooldownMs = std::max(0, getInt("ThirdPersonMapLoadCooldownMs", m_ThirdPersonMapLoadCooldownMs));
    m_ThirdPersonRenderOnCustomWalk = getBool("ThirdPersonRenderOnCustomWalk", m_ThirdPersonRenderOnCustomWalk);
    m_ThirdPersonDefault = getBool("ThirdPersonDefault", m_ThirdPersonDefault);
    m_ThirdPersonCameraFollowHmd = getBool("ThirdPersonCameraFollowHmd", m_ThirdPersonCameraFollowHmd);
    m_ThirdPersonFrontViewEnabled = getBool("ThirdPersonFrontViewEnabled", m_ThirdPersonFrontViewEnabled);
    m_ThirdPersonFrontScopeFromEye = getBool("ThirdPersonFrontScopeFromEye", m_ThirdPersonFrontScopeFromEye);
    m_ThirdPersonScopeOverlayOffset = getVector3("ThirdPersonScopeOverlayOffset", m_ThirdPersonScopeOverlayOffset);
    m_ThirdPersonScopeOverlayOffset.x = getFloat("ThirdPersonScopeOverlayOffsetX", m_ThirdPersonScopeOverlayOffset.x);
    m_ThirdPersonScopeOverlayOffset.y = getFloat("ThirdPersonScopeOverlayOffsetY", m_ThirdPersonScopeOverlayOffset.y);
    m_ThirdPersonScopeOverlayOffset.z = getFloat("ThirdPersonScopeOverlayOffsetZ", m_ThirdPersonScopeOverlayOffset.z);
    m_HideArms = getBool("HideArms", m_HideArms);
    m_NativeViewmodelHandsOnly = getBool("NativeViewmodelHandsOnly", m_NativeViewmodelHandsOnly);
    m_NativeViewmodelHandsOnlyWristKeepFraction = std::clamp(
        getFloat(
            "NativeViewmodelHandsOnlyWristKeepUnits",
            getFloat("NativeViewmodelHandsOnlyWristKeepFraction", m_NativeViewmodelHandsOnlyWristKeepFraction)),
        0.0f,
        8.0f);
    m_NativeViewmodelHandsOnlyTrimUnits = std::clamp(
        getFloat("NativeViewmodelHandsOnlyTrimUnits", m_NativeViewmodelHandsOnlyTrimUnits),
        -32.0f,
        32.0f);
    m_NativeViewmodelHandsOnlyArmBendScale = std::clamp(
        getFloat("NativeViewmodelHandsOnlyArmBendScale", m_NativeViewmodelHandsOnlyArmBendScale),
        0.0f,
        1.0f);
    m_NativeViewmodelHandsOnlyCutRotationDeg =
        getVector3("NativeViewmodelHandsOnlyCutRotationDeg", m_NativeViewmodelHandsOnlyCutRotationDeg);
    m_NativeViewmodelHandsOnlyCutRotationDeg.x = std::clamp(m_NativeViewmodelHandsOnlyCutRotationDeg.x, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyCutRotationDeg.y = std::clamp(m_NativeViewmodelHandsOnlyCutRotationDeg.y, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyCutRotationDeg.z = std::clamp(m_NativeViewmodelHandsOnlyCutRotationDeg.z, -89.0f, 89.0f);
    auto nonZeroCutRotation = [](const Vector& value)->bool {
        return std::fabs(value.x) > 0.0001f ||
            std::fabs(value.y) > 0.0001f ||
            std::fabs(value.z) > 0.0001f;
        };
    const bool useLegacySharedCutRotation =
        hasConfigKey("NativeViewmodelHandsOnlyCutRotationDeg") &&
        nonZeroCutRotation(m_NativeViewmodelHandsOnlyCutRotationDeg);
    m_NativeViewmodelHandsOnlyLeftCutRotationDeg =
        getVector3(
            "NativeViewmodelHandsOnlyLeftCutRotationDeg",
            useLegacySharedCutRotation
                ? m_NativeViewmodelHandsOnlyCutRotationDeg
                : m_NativeViewmodelHandsOnlyLeftCutRotationDeg);
    m_NativeViewmodelHandsOnlyLeftCutRotationDeg.x = std::clamp(m_NativeViewmodelHandsOnlyLeftCutRotationDeg.x, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyLeftCutRotationDeg.y = std::clamp(m_NativeViewmodelHandsOnlyLeftCutRotationDeg.y, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyLeftCutRotationDeg.z = std::clamp(m_NativeViewmodelHandsOnlyLeftCutRotationDeg.z, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyRightCutRotationDeg =
        getVector3(
            "NativeViewmodelHandsOnlyRightCutRotationDeg",
            useLegacySharedCutRotation
                ? m_NativeViewmodelHandsOnlyCutRotationDeg
                : m_NativeViewmodelHandsOnlyRightCutRotationDeg);
    m_NativeViewmodelHandsOnlyRightCutRotationDeg.x = std::clamp(m_NativeViewmodelHandsOnlyRightCutRotationDeg.x, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyRightCutRotationDeg.y = std::clamp(m_NativeViewmodelHandsOnlyRightCutRotationDeg.y, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyRightCutRotationDeg.z = std::clamp(m_NativeViewmodelHandsOnlyRightCutRotationDeg.z, -89.0f, 89.0f);
    m_NativeViewmodelHandsOnlyAutoCutRotation = getBool(
        "NativeViewmodelHandsOnlyAutoCutRotation",
        m_NativeViewmodelHandsOnlyAutoCutRotation);
    auto parseNativeHandsOnlyCutRotationVector = [&](std::string value, Vector& outValue) -> bool
        {
            trim(value);
            std::replace(value.begin(), value.end(), '/', ',');
            std::stringstream ss(value);
            std::string componentText;
            float components[3] = {};
            for (int i = 0; i < 3; ++i)
            {
                if (!std::getline(ss, componentText, ','))
                    return false;
                trim(componentText);
                if (componentText.empty())
                    return false;
                char* end = nullptr;
                const float component = std::strtof(componentText.c_str(), &end);
                if (!end || *end != '\0' || !std::isfinite(component))
                    return false;
                components[i] = std::clamp(component, -89.0f, 89.0f);
            }

            std::string extra;
            while (std::getline(ss, extra, ','))
            {
                trim(extra);
                if (!extra.empty())
                    return false;
            }

            outValue = Vector(components[0], components[1], components[2]);
            return true;
        };
    auto parseNativeHandsOnlyCutRotationOverrides = [&](const std::string& spec)
        {
            m_NativeViewmodelHandsOnlyCutRotationOverrides.clear();
            std::stringstream entries(spec);
            std::string entry;
            while (std::getline(entries, entry, ';'))
            {
                trim(entry);
                if (entry.empty())
                    continue;

                std::vector<std::string> parts;
                std::stringstream partStream(entry);
                std::string part;
                while (std::getline(partStream, part, '|'))
                {
                    trim(part);
                    parts.push_back(part);
                }

                if (parts.size() < 2)
                {
                    Game::logMsg("[VR][Config] ignored invalid NativeViewmodelHandsOnlyCutRotationOverrides entry=%s", entry.c_str());
                    continue;
                }

                NativeViewmodelHandsOnlyCutRotationOverride overrideEntry{};
                overrideEntry.modelPattern = parts[0];
                std::transform(
                    overrideEntry.modelPattern.begin(),
                    overrideEntry.modelPattern.end(),
                    overrideEntry.modelPattern.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                if (overrideEntry.modelPattern.empty())
                {
                    Game::logMsg("[VR][Config] ignored invalid NativeViewmodelHandsOnlyCutRotationOverrides entry=%s", entry.c_str());
                    continue;
                }

                if (!parts[1].empty())
                    overrideEntry.hasLeft = parseNativeHandsOnlyCutRotationVector(parts[1], overrideEntry.left);
                if (parts.size() >= 3 && !parts[2].empty())
                    overrideEntry.hasRight = parseNativeHandsOnlyCutRotationVector(parts[2], overrideEntry.right);

                if (!overrideEntry.hasLeft && !overrideEntry.hasRight)
                {
                    Game::logMsg("[VR][Config] ignored invalid NativeViewmodelHandsOnlyCutRotationOverrides entry=%s", entry.c_str());
                    continue;
                }

                m_NativeViewmodelHandsOnlyCutRotationOverrides.push_back(overrideEntry);
            }
        };
    m_NativeViewmodelHandsOnlyCutRotationOverridesSpec =
        getString("NativeViewmodelHandsOnlyCutRotationOverrides", "");
    parseNativeHandsOnlyCutRotationOverrides(m_NativeViewmodelHandsOnlyCutRotationOverridesSpec);
    m_NativeViewmodelRightHandAnimationKeepUnits = std::clamp(
        getFloat("NativeViewmodelRightHandAnimationKeepUnits", m_NativeViewmodelRightHandAnimationKeepUnits),
        -16.0f,
        16.0f);
    m_NativeViewmodelLeftHandFreezeAfterMapSeconds = std::clamp(
        getFloat("NativeViewmodelLeftHandFreezeAfterMapSeconds", m_NativeViewmodelLeftHandFreezeAfterMapSeconds),
        0.0f,
        600.0f);
    m_NativeViewmodelHandsOnlyFreezePoseLock = getBool(
        "NativeViewmodelHandsOnlyFreezePoseLock",
        m_NativeViewmodelHandsOnlyFreezePoseLock);
    auto clampNativePoseVector = [](Vector value, float minValue, float maxValue)
    {
        value.x = std::clamp(value.x, minValue, maxValue);
        value.y = std::clamp(value.y, minValue, maxValue);
        value.z = std::clamp(value.z, minValue, maxValue);
        return value;
    };
    m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters = clampNativePoseVector(
        getVector3("NativeViewmodelHandsOnlyFreezePoseOffsetMeters", m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters),
        -2.0f,
        2.0f);
    m_NativeViewmodelHandsOnlyFreezePoseRotationOffsetDeg = clampNativePoseVector(
        getVector3("NativeViewmodelHandsOnlyFreezePoseRotationOffsetDeg", m_NativeViewmodelHandsOnlyFreezePoseRotationOffsetDeg),
        -180.0f,
        180.0f);
    m_NativeViewmodelHandsOnlyLeftFreezePoseRotationOffsetDeg = clampNativePoseVector(
        getVector3(
            "NativeViewmodelHandsOnlyLeftFreezePoseRotationOffsetDeg",
            m_NativeViewmodelHandsOnlyFreezePoseRotationOffsetDeg),
        -180.0f,
        180.0f);
    m_NativeViewmodelHandsOnlyRightFreezePoseRotationOffsetDeg = clampNativePoseVector(
        getVector3(
            "NativeViewmodelHandsOnlyRightFreezePoseRotationOffsetDeg",
            m_NativeViewmodelHandsOnlyFreezePoseRotationOffsetDeg),
        -180.0f,
        180.0f);
    m_NativeViewmodelLeftHandPoseOffsetMeters = clampNativePoseVector(
        getVector3("NativeViewmodelLeftHandPoseOffsetMeters", m_NativeViewmodelLeftHandPoseOffsetMeters),
        -1.0f,
        1.0f);
    m_NativeViewmodelLeftHandPoseRotationOffsetDeg = clampNativePoseVector(
        getVector3("NativeViewmodelLeftHandPoseRotationOffsetDeg", m_NativeViewmodelLeftHandPoseRotationOffsetDeg),
        -180.0f,
        180.0f);
    m_NativeViewmodelLeftHandOpenVRSkeleton = getBool(
        "NativeViewmodelLeftHandOpenVRSkeleton",
        m_NativeViewmodelLeftHandOpenVRSkeleton);
    m_NativeViewmodelLeftHandOpenVRCurlStrength = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRCurlStrength", m_NativeViewmodelLeftHandOpenVRCurlStrength),
        0.0f,
        2.0f);
    m_NativeViewmodelLeftHandOpenVRCurlScale = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRCurlScale", m_NativeViewmodelLeftHandOpenVRCurlScale),
        0.0f,
        2.0f);
    m_NativeViewmodelLeftHandOpenVRCurlDirection = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRCurlDirection", m_NativeViewmodelLeftHandOpenVRCurlDirection),
        -1.0f,
        1.0f);
    m_NativeViewmodelLeftHandOpenVRCurlAxis = std::clamp(
        getInt("NativeViewmodelLeftHandOpenVRCurlAxis", m_NativeViewmodelLeftHandOpenVRCurlAxis),
        0,
        2);
    m_NativeViewmodelLeftHandOpenVRThumbRootOffsetUnits = clampNativePoseVector(
        getVector3("NativeViewmodelLeftHandOpenVRThumbRootOffsetUnits", m_NativeViewmodelLeftHandOpenVRThumbRootOffsetUnits),
        -16.0f,
        16.0f);
    m_NativeViewmodelLeftHandOpenVRThumbRootRotationOffsetDeg = clampNativePoseVector(
        getVector3("NativeViewmodelLeftHandOpenVRThumbRootRotationOffsetDeg", m_NativeViewmodelLeftHandOpenVRThumbRootRotationOffsetDeg),
        -90.0f,
        90.0f);
    m_NativeViewmodelLeftHandOpenVRInitialCurl[0] = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRThumbInitialCurl", m_NativeViewmodelLeftHandOpenVRInitialCurl[0]),
        -1.0f,
        1.0f);
    m_NativeViewmodelLeftHandOpenVRInitialCurl[1] = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRIndexInitialCurl", m_NativeViewmodelLeftHandOpenVRInitialCurl[1]),
        -1.0f,
        1.0f);
    m_NativeViewmodelLeftHandOpenVRInitialCurl[2] = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRMiddleInitialCurl", m_NativeViewmodelLeftHandOpenVRInitialCurl[2]),
        -1.0f,
        1.0f);
    m_NativeViewmodelLeftHandOpenVRInitialCurl[3] = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRRingInitialCurl", m_NativeViewmodelLeftHandOpenVRInitialCurl[3]),
        -1.0f,
        1.0f);
    m_NativeViewmodelLeftHandOpenVRInitialCurl[4] = std::clamp(
        getFloat("NativeViewmodelLeftHandOpenVRPinkyInitialCurl", m_NativeViewmodelLeftHandOpenVRInitialCurl[4]),
        -1.0f,
        1.0f);
    const bool vrHandsEntryEnabled = getBool("VrHandsEnabled", false);
    m_VrHandsGlovesEnabled = getBool("VrHandsGlovesEnabled", false);
    m_VrHandsGloveFingerMaxCurl[0] = std::clamp(
        getFloat("VrHandsGloveThumbMaxCurl", m_VrHandsGloveFingerMaxCurl[0]),
        0.0f,
        1.0f);
    m_VrHandsGloveFingerMaxCurl[1] = std::clamp(
        getFloat("VrHandsGloveIndexMaxCurl", m_VrHandsGloveFingerMaxCurl[1]),
        0.0f,
        1.0f);
    m_VrHandsGloveFingerMaxCurl[2] = std::clamp(
        getFloat("VrHandsGloveMiddleMaxCurl", m_VrHandsGloveFingerMaxCurl[2]),
        0.0f,
        1.0f);
    m_VrHandsGloveFingerMaxCurl[3] = std::clamp(
        getFloat("VrHandsGloveRingMaxCurl", m_VrHandsGloveFingerMaxCurl[3]),
        0.0f,
        1.0f);
    m_VrHandsGloveFingerMaxCurl[4] = std::clamp(
        getFloat("VrHandsGlovePinkyMaxCurl", m_VrHandsGloveFingerMaxCurl[4]),
        0.0f,
        1.0f);
    m_VrHandsEnabled = vrHandsEntryEnabled && m_VrHandsGlovesEnabled;
    if (m_VrHandsEnabled)
    {
        if (m_VrHandsGlovesRuntimeFallback && m_VrHands)
            m_VrHands.reset();
        m_VrHandsGlovesRuntimeFallback = false;
        m_NativeViewmodelHandsOnly = false;
        m_HideArms = true;
    }
    else if (vrHandsEntryEnabled)
    {
        m_VrHandsGlovesRuntimeFallback = false;
        m_NativeViewmodelHandsOnly = true;
        m_HideArms = false;
    }
    else if (m_NativeViewmodelHandsOnly)
    {
        m_VrHandsGlovesRuntimeFallback = false;
        m_HideArms = false;
    }
    else
    {
        m_VrHandsGlovesRuntimeFallback = false;
        m_HideArms = false;
    }
    m_VrHandsMotionRangeWithoutController = getBool("VrHandsMotionRangeWithoutController", m_VrHandsMotionRangeWithoutController);
    m_VrHandsDebugLog = getBool("VrHandsDebugLog", m_VrHandsDebugLog);
    m_VrHandsModelScale = std::clamp(getFloat("VrHandsModelScale", m_VrHandsModelScale), 0.25f, 4.0f);
    m_VrHandsRightUseViewmodelPose = getBool("VrHandsRightUseViewmodelPose", m_VrHandsRightUseViewmodelPose);
    m_VrHandsTwoHandedGripTargetBoxScale = std::clamp(getFloat("VrHandsTwoHandedGripTargetBoxScale", m_VrHandsTwoHandedGripTargetBoxScale), 0.25f, 8.0f);
    m_VrHandsRealBulletSpreadEnabled = getBool("VrHandsRealBulletSpreadEnabled", m_VrHandsRealBulletSpreadEnabled);
    m_VrHandsTwoHandedAimMountFriendly = getBool("VrHandsTwoHandedAimMountFriendly", m_VrHandsTwoHandedAimMountFriendly);
    m_VrHandsVirtualStockEnabled = getBool("VrHandsVirtualStockEnabled", m_VrHandsVirtualStockEnabled);
    m_VrHandsTwoHandedAimStrength = std::clamp(getFloat("VrHandsTwoHandedAimStrength", m_VrHandsTwoHandedAimStrength), 0.0f, 1.0f);
    m_VrHandsTwoHandedAimSmoothingSeconds = std::clamp(getFloat("VrHandsTwoHandedAimSmoothingSeconds", m_VrHandsTwoHandedAimSmoothingSeconds), 0.0f, 0.25f);
    m_VrHandsTwoHandedAimMinHandDistanceMeters = std::clamp(getFloat("VrHandsTwoHandedAimMinHandDistanceMeters", m_VrHandsTwoHandedAimMinHandDistanceMeters), 0.0f, 1.0f);
    m_VrHandsTwoHandedAimMaxHandDistanceMeters = std::clamp(getFloat("VrHandsTwoHandedAimMaxHandDistanceMeters", m_VrHandsTwoHandedAimMaxHandDistanceMeters), 0.0f, 2.0f);
    m_VrHandsTwoHandedAimOffhandOffsetMeters = getVector3("VrHandsTwoHandedAimOffhandOffsetMeters", m_VrHandsTwoHandedAimOffhandOffsetMeters);
    m_VrHandsVirtualStockOffsetMeters = getVector3("VrHandsVirtualStockOffsetMeters", m_VrHandsVirtualStockOffsetMeters);
    m_VrHandsVirtualStockStrength = std::clamp(getFloat("VrHandsVirtualStockStrength", m_VrHandsVirtualStockStrength), 0.0f, 1.0f);
    m_MagazineBoxDebugEnabled = getBool("MagazineBoxDebugEnabled", m_MagazineBoxDebugEnabled);
    m_ViewmodelBoneLabelsEnabled = getBool("ViewmodelBoneLabelsEnabled", m_ViewmodelBoneLabelsEnabled);
    m_MagazineBoxDebugFallbackHalfExtentsMeters = getVector3("MagazineBoxDebugFallbackHalfExtentsMeters", m_MagazineBoxDebugFallbackHalfExtentsMeters);
    m_MagazineBoxDebugPaddingMeters = getVector3("MagazineBoxDebugPaddingMeters", m_MagazineBoxDebugPaddingMeters);
    m_MagazineInteractionEnabled = getBool("MagazineInteractionEnabled", m_MagazineInteractionEnabled);
    if (!m_VrHandsEnabled && !m_NativeViewmodelHandsOnly)
        m_MagazineInteractionEnabled = false;
    m_MagazineInteractionQuickReloadMode = getBool("MagazineInteractionQuickReloadMode", m_MagazineInteractionQuickReloadMode);
    m_MagazineInteractionUseButtonGripInput = getBool("MagazineInteractionUseButtonGripInput", m_MagazineInteractionUseButtonGripInput);
    m_MagazineInteractionSuppressEmptyClipAutoReload = getBool("MagazineInteractionSuppressEmptyClipAutoReload", m_MagazineInteractionSuppressEmptyClipAutoReload);
    m_MagazineInteractionShotgunShellsPerInsert = std::clamp(getInt("MagazineInteractionShotgunShellsPerInsert", m_MagazineInteractionShotgunShellsPerInsert), 1, 8);
    m_MagazineInteractionThumbIndexCurlStart = std::clamp(getFloat("MagazineInteractionThumbIndexCurlStart", m_MagazineInteractionThumbIndexCurlStart), 0.0f, 1.0f);
    m_MagazineInteractionThumbIndexCurlRelease = std::clamp(getFloat("MagazineInteractionThumbIndexCurlRelease", m_MagazineInteractionThumbIndexCurlRelease), 0.0f, m_MagazineInteractionThumbIndexCurlStart);
    m_MagazineInteractionThreeFingerCurlStart = std::clamp(getFloat("MagazineInteractionThreeFingerCurlStart", m_MagazineInteractionThreeFingerCurlStart), 0.0f, 1.0f);
    m_MagazineInteractionThreeFingerCurlRelease = std::clamp(getFloat("MagazineInteractionThreeFingerCurlRelease", m_MagazineInteractionThreeFingerCurlRelease), 0.0f, m_MagazineInteractionThreeFingerCurlStart);
    m_MagazineInteractionGrabPaddingMeters = std::clamp(getFloat("MagazineInteractionGrabPaddingMeters", m_MagazineInteractionGrabPaddingMeters), 0.0f, 0.25f);
    m_MagazineInteractionPullTriggerMeters = std::clamp(getFloat("MagazineInteractionPullTriggerMeters", m_MagazineInteractionPullTriggerMeters), 0.0f, 0.50f);
    m_MagazineInteractionPullTriggerByMagazineMeters = std::clamp(getFloat("MagazineInteractionPullTriggerByMagazineMeters", m_MagazineInteractionPullTriggerByMagazineMeters), 0.0f, 0.50f);
    m_MagazineInteractionFreshMagazineGrabRangeMeters = std::clamp(getFloat("MagazineInteractionFreshMagazineGrabRangeMeters", m_MagazineInteractionFreshMagazineGrabRangeMeters), 0.0f, 0.50f);
    m_MagazineInteractionFreshMagazinePickupOffsetMeters = getVector3("MagazineInteractionFreshMagazinePickupOffsetMeters", m_MagazineInteractionFreshMagazinePickupOffsetMeters);
    m_MagazineInteractionFreshMagazinePickupOffsetMeters.x = std::clamp(m_MagazineInteractionFreshMagazinePickupOffsetMeters.x, -1.50f, 1.50f);
    m_MagazineInteractionFreshMagazinePickupOffsetMeters.y = std::clamp(m_MagazineInteractionFreshMagazinePickupOffsetMeters.y, -1.50f, 1.50f);
    m_MagazineInteractionFreshMagazinePickupOffsetMeters.z = std::clamp(m_MagazineInteractionFreshMagazinePickupOffsetMeters.z, -1.50f, 1.50f);
    m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters = getVector3("MagazineInteractionFreshMagazineBoxHalfExtentsMeters", m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters);
    m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters = getVector3("MagazineInteractionFreshMagazineSocketLocalOffsetMeters", m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters);
    m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters.x = std::clamp(m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters.x, -0.50f, 0.50f);
    m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters.y = std::clamp(m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters.y, -0.50f, 0.50f);
    m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters.z = std::clamp(m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters.z, -0.50f, 0.50f);
    const Vector legacyFreshMagazineWristAnchorOffsetMeters =
        getVector3("MagazineInteractionFreshMagazineMiddleFingerAnchorOffsetMeters", m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters);
    m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters =
        getVector3("MagazineInteractionFreshMagazineWristAnchorOffsetMeters", legacyFreshMagazineWristAnchorOffsetMeters);
    m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters.x = std::clamp(m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters.x, -0.25f, 0.25f);
    m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters.y = std::clamp(m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters.y, -0.25f, 0.25f);
    m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters.z = std::clamp(m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters.z, -0.25f, 0.25f);
    m_MagazineInteractionMagazineBoxHalfExtentsMeters = getVector3("MagazineInteractionMagazineBoxHalfExtentsMeters", m_MagazineInteractionMagazineBoxHalfExtentsMeters);
    m_MagazineInteractionMagazineBoxHalfExtentsMeters.x = std::clamp(m_MagazineInteractionMagazineBoxHalfExtentsMeters.x, 0.0f, 0.50f);
    m_MagazineInteractionMagazineBoxHalfExtentsMeters.y = std::clamp(m_MagazineInteractionMagazineBoxHalfExtentsMeters.y, 0.0f, 0.50f);
    m_MagazineInteractionMagazineBoxHalfExtentsMeters.z = std::clamp(m_MagazineInteractionMagazineBoxHalfExtentsMeters.z, 0.0f, 0.50f);
    m_MagazineInteractionMagazineBoxLocalOffsetMeters = getVector3("MagazineInteractionMagazineBoxLocalOffsetMeters", m_MagazineInteractionMagazineBoxLocalOffsetMeters);
    m_MagazineInteractionMagazineBoxLocalOffsetMeters.x = std::clamp(m_MagazineInteractionMagazineBoxLocalOffsetMeters.x, -0.50f, 0.50f);
    m_MagazineInteractionMagazineBoxLocalOffsetMeters.y = std::clamp(m_MagazineInteractionMagazineBoxLocalOffsetMeters.y, -0.50f, 0.50f);
    m_MagazineInteractionMagazineBoxLocalOffsetMeters.z = std::clamp(m_MagazineInteractionMagazineBoxLocalOffsetMeters.z, -0.50f, 0.50f);
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg = getVector3("MagazineInteractionMagazineBoxLocalRotationOffsetDeg", m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg);
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg.x = std::clamp(m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg.x, -180.0f, 180.0f);
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg.y = std::clamp(m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg.y, -180.0f, 180.0f);
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg.z = std::clamp(m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg.z, -180.0f, 180.0f);
    m_MagazineInteractionSocketCaptureRadiusMeters = std::clamp(getFloat("MagazineInteractionSocketCaptureRadiusMeters", m_MagazineInteractionSocketCaptureRadiusMeters), 0.0f, 0.25f);
    m_MagazineInteractionSocketCaptureAngleDeg = std::clamp(getFloat("MagazineInteractionSocketCaptureAngleDeg", m_MagazineInteractionSocketCaptureAngleDeg), 0.0f, 89.0f);
    m_MagazineInteractionSocketRequiredDepthMeters = std::clamp(getFloat("MagazineInteractionSocketRequiredDepthMeters", m_MagazineInteractionSocketRequiredDepthMeters), 0.0f, 0.25f);
    m_MagazineInteractionSocketRequiredOverlapFraction = std::clamp(getFloat("MagazineInteractionSocketRequiredOverlapFraction", m_MagazineInteractionSocketRequiredOverlapFraction), 0.0f, 1.0f);
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters = getVector3("MagazineInteractionSocketCaptureBoxHalfExtentsMeters", m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters);
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters.x = std::clamp(m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters.x, 0.0f, 0.50f);
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters.y = std::clamp(m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters.y, 0.0f, 0.50f);
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters.z = std::clamp(m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters.z, 0.0f, 0.50f);
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters = getVector3("MagazineInteractionSocketCaptureBoxLocalOffsetMeters", m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters);
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters.x = std::clamp(m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters.x, -0.50f, 0.50f);
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters.y = std::clamp(m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters.y, -0.50f, 0.50f);
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters.z = std::clamp(m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters.z, -0.50f, 0.50f);
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg = getVector3("MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg", m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg);
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg.x = std::clamp(m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg.x, -180.0f, 180.0f);
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg.y = std::clamp(m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg.y, -180.0f, 180.0f);
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg.z = std::clamp(m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg.z, -180.0f, 180.0f);
    m_MagazineInteractionBoltGrabPaddingMeters = std::clamp(getFloat("MagazineInteractionBoltGrabPaddingMeters", m_MagazineInteractionBoltGrabPaddingMeters), 0.0f, 0.25f);
    m_MagazineInteractionBoltPullDistanceMeters = std::clamp(getFloat("MagazineInteractionBoltPullDistanceMeters", m_MagazineInteractionBoltPullDistanceMeters), 0.0f, 0.25f);
    m_MagazineInteractionBoltReturnDistanceMeters = std::clamp(getFloat("MagazineInteractionBoltReturnDistanceMeters", m_MagazineInteractionBoltReturnDistanceMeters), 0.0f, 0.10f);
    m_MagazineInteractionBoltBoxHalfExtentsMeters = getVector3("MagazineInteractionBoltBoxHalfExtentsMeters", m_MagazineInteractionBoltBoxHalfExtentsMeters);
    m_MagazineInteractionBoltBoxHalfExtentsMeters.x = getFloat("MagazineInteractionBoltBoxHalfExtentsMetersX", m_MagazineInteractionBoltBoxHalfExtentsMeters.x);
    m_MagazineInteractionBoltBoxHalfExtentsMeters.y = getFloat("MagazineInteractionBoltBoxHalfExtentsMetersY", m_MagazineInteractionBoltBoxHalfExtentsMeters.y);
    m_MagazineInteractionBoltBoxHalfExtentsMeters.z = getFloat("MagazineInteractionBoltBoxHalfExtentsMetersZ", m_MagazineInteractionBoltBoxHalfExtentsMeters.z);
    m_MagazineInteractionBoltBoxHalfExtentsMeters.x = std::clamp(m_MagazineInteractionBoltBoxHalfExtentsMeters.x, 0.005f, 0.25f);
    m_MagazineInteractionBoltBoxHalfExtentsMeters.y = std::clamp(m_MagazineInteractionBoltBoxHalfExtentsMeters.y, 0.005f, 0.25f);
    m_MagazineInteractionBoltBoxHalfExtentsMeters.z = std::clamp(m_MagazineInteractionBoltBoxHalfExtentsMeters.z, 0.005f, 0.25f);
    m_MagazineInteractionBoltBoxLocalOffsetMeters = getVector3("MagazineInteractionBoltBoxLocalOffsetMeters", m_MagazineInteractionBoltBoxLocalOffsetMeters);
    m_MagazineInteractionBoltBoxLocalOffsetMeters.x = getFloat("MagazineInteractionBoltBoxLocalOffsetMetersX", m_MagazineInteractionBoltBoxLocalOffsetMeters.x);
    m_MagazineInteractionBoltBoxLocalOffsetMeters.y = getFloat("MagazineInteractionBoltBoxLocalOffsetMetersY", m_MagazineInteractionBoltBoxLocalOffsetMeters.y);
    m_MagazineInteractionBoltBoxLocalOffsetMeters.z = getFloat("MagazineInteractionBoltBoxLocalOffsetMetersZ", m_MagazineInteractionBoltBoxLocalOffsetMeters.z);
    m_MagazineInteractionBoltBoxLocalOffsetMeters.x = std::clamp(m_MagazineInteractionBoltBoxLocalOffsetMeters.x, -0.25f, 0.25f);
    m_MagazineInteractionBoltBoxLocalOffsetMeters.y = std::clamp(m_MagazineInteractionBoltBoxLocalOffsetMeters.y, -0.25f, 0.25f);
    m_MagazineInteractionBoltBoxLocalOffsetMeters.z = std::clamp(m_MagazineInteractionBoltBoxLocalOffsetMeters.z, -0.25f, 0.25f);
    m_MagazineInteractionBoltPullAxisLocal = getVector3("MagazineInteractionBoltPullAxisLocal", m_MagazineInteractionBoltPullAxisLocal);
    m_MagazineInteractionStaleSeconds = std::clamp(getFloat("MagazineInteractionStaleSeconds", m_MagazineInteractionStaleSeconds), 0.02f, 1.0f);
    m_MagazineInteractionMagazineBoneOverridesSpec = getString("ManualReloadMagazineBoneOverrides", m_MagazineInteractionMagazineBoneOverridesSpec);
    m_MagazineInteractionMagazineBoneOverrides.clear();
    m_MagazineInteractionMagazineBoneProfileOverrides.clear();
    m_MagazineInteractionBoltBoneOverridesSpec = getString("MagazineInteractionBoltBoneOverrides", m_MagazineInteractionBoltBoneOverridesSpec);
    m_MagazineInteractionBoltBoneOverrides.clear();
    m_MagazineInteractionBoltBoneProfileOverrides.clear();
    m_MagazineInteractionMagazineBoxHalfExtentsMetersOverridesSpec = getString("MagazineInteractionMagazineBoxHalfExtentsMetersOverrides", m_MagazineInteractionMagazineBoxHalfExtentsMetersOverridesSpec);
    m_MagazineInteractionMagazineBoxHalfExtentsMetersOverrides.clear();
    m_MagazineInteractionMagazineBoxHalfExtentsMetersProfileOverrides.clear();
    m_MagazineInteractionMagazineBoxLocalOffsetMetersOverridesSpec = getString("MagazineInteractionMagazineBoxLocalOffsetMetersOverrides", m_MagazineInteractionMagazineBoxLocalOffsetMetersOverridesSpec);
    m_MagazineInteractionMagazineBoxLocalOffsetMetersOverrides.clear();
    m_MagazineInteractionMagazineBoxLocalOffsetMetersProfileOverrides.clear();
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverridesSpec = getString("MagazineInteractionMagazineBoxLocalRotationOffsetDegOverrides", m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverridesSpec);
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverrides.clear();
    m_MagazineInteractionMagazineBoxLocalRotationOffsetDegProfileOverrides.clear();
    m_MagazineInteractionBoltPullAxisLocalOverridesSpec = getString("MagazineInteractionBoltPullAxisLocalOverrides", m_MagazineInteractionBoltPullAxisLocalOverridesSpec);
    m_MagazineInteractionBoltPullAxisLocalOverrides.clear();
    m_MagazineInteractionBoltPullAxisLocalProfileOverrides.clear();
    m_MagazineInteractionBoltBoxHalfExtentsMetersOverridesSpec = getString("MagazineInteractionBoltBoxHalfExtentsMetersOverrides", m_MagazineInteractionBoltBoxHalfExtentsMetersOverridesSpec);
    m_MagazineInteractionBoltBoxHalfExtentsMetersOverrides.clear();
    m_MagazineInteractionBoltBoxHalfExtentsMetersProfileOverrides.clear();
    m_MagazineInteractionBoltBoxLocalOffsetMetersOverridesSpec = getString("MagazineInteractionBoltBoxLocalOffsetMetersOverrides", m_MagazineInteractionBoltBoxLocalOffsetMetersOverridesSpec);
    m_MagazineInteractionBoltBoxLocalOffsetMetersOverrides.clear();
    m_MagazineInteractionBoltBoxLocalOffsetMetersProfileOverrides.clear();
    m_MagazineInteractionBoltGrabPaddingMetersOverridesSpec = getString("MagazineInteractionBoltGrabPaddingMetersOverrides", m_MagazineInteractionBoltGrabPaddingMetersOverridesSpec);
    m_MagazineInteractionBoltGrabPaddingMetersOverrides.clear();
    m_MagazineInteractionBoltGrabPaddingMetersProfileOverrides.clear();
    m_MagazineInteractionBoltPullDistanceMetersOverridesSpec = getString("MagazineInteractionBoltPullDistanceMetersOverrides", m_MagazineInteractionBoltPullDistanceMetersOverridesSpec);
    m_MagazineInteractionBoltPullDistanceMetersOverrides.clear();
    m_MagazineInteractionBoltPullDistanceMetersProfileOverrides.clear();
    m_MagazineInteractionBoltReturnDistanceMetersOverridesSpec = getString("MagazineInteractionBoltReturnDistanceMetersOverrides", m_MagazineInteractionBoltReturnDistanceMetersOverridesSpec);
    m_MagazineInteractionBoltReturnDistanceMetersOverrides.clear();
    m_MagazineInteractionBoltReturnDistanceMetersProfileOverrides.clear();
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverridesSpec = getString("MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides", m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverridesSpec);
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides.clear();
    m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersProfileOverrides.clear();
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverridesSpec = getString("MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides", m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverridesSpec);
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides.clear();
    m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersProfileOverrides.clear();
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverridesSpec = getString("MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides", m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverridesSpec);
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides.clear();
    m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegProfileOverrides.clear();
    m_MagazineInteractionSocketCaptureAngleDegOverridesSpec = getString("MagazineInteractionSocketCaptureAngleDegOverrides", m_MagazineInteractionSocketCaptureAngleDegOverridesSpec);
    m_MagazineInteractionSocketCaptureAngleDegOverrides.clear();
    m_MagazineInteractionSocketCaptureAngleDegProfileOverrides.clear();
    m_MagazineInteractionSocketRequiredDepthMetersOverridesSpec = getString("MagazineInteractionSocketRequiredDepthMetersOverrides", m_MagazineInteractionSocketRequiredDepthMetersOverridesSpec);
    m_MagazineInteractionSocketRequiredDepthMetersOverrides.clear();
    m_MagazineInteractionSocketRequiredDepthMetersProfileOverrides.clear();
    m_MagazineInteractionSocketRequiredOverlapFractionOverridesSpec = getString("MagazineInteractionSocketRequiredOverlapFractionOverrides", m_MagazineInteractionSocketRequiredOverlapFractionOverridesSpec);
    m_MagazineInteractionSocketRequiredOverlapFractionOverrides.clear();
    m_MagazineInteractionSocketRequiredOverlapFractionProfileOverrides.clear();
    {
        auto normalizeBoneOverrideWeaponKey = [&](std::string value)
            {
                trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::replace(value.begin(), value.end(), '-', '_');
                std::replace(value.begin(), value.end(), ' ', '_');
                return value;
            };

        auto normalizeBoneOverrideProfileKey = [&](std::string value)
            {
                trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::replace(value.begin(), value.end(), ' ', '_');
                auto isHex = [](char ch)
                    {
                        return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
                    };
                if (value.size() == 21u &&
                    value.rfind("fp", 0) == 0 &&
                    value.compare(10u, 3u, "_bs") == 0)
                {
                    bool valid = true;
                    for (size_t i = 2u; i < 10u; ++i)
                        valid = valid && isHex(value[i]);
                    for (size_t i = 13u; i < 21u; ++i)
                        valid = valid && isHex(value[i]);
                    if (valid)
                        return std::string("bs") + value.substr(13u, 8u);
                }
                return value;
            };

        auto boneOverrideProfileKeyLooksValid = [&](const std::string& value)
            {
                if (value.size() != 10u || value.rfind("bs", 0) != 0)
                    return false;
                for (size_t i = 2u; i < 10u; ++i)
                {
                    if (!std::isxdigit(static_cast<unsigned char>(value[i])))
                        return false;
                }
                return true;
            };

        const std::unordered_map<std::string, int> weaponBoneOverrideAliases =
        {
            { "pistol", C_WeaponCSBase::WeaponID::PISTOL },
            { "pistols", C_WeaponCSBase::WeaponID::PISTOL },
            { "dual_pistol", C_WeaponCSBase::WeaponID::PISTOL },
            { "dual_pistols", C_WeaponCSBase::WeaponID::PISTOL },
            { "uzi", C_WeaponCSBase::WeaponID::UZI },
            { "smg", C_WeaponCSBase::WeaponID::UZI },
            { "m16", C_WeaponCSBase::WeaponID::M16A1 },
            { "m16a", C_WeaponCSBase::WeaponID::M16A1 },
            { "m16a1", C_WeaponCSBase::WeaponID::M16A1 },
            { "rifle", C_WeaponCSBase::WeaponID::M16A1 },
            { "hunting_rifle", C_WeaponCSBase::WeaponID::HUNTING_RIFLE },
            { "huntingrifle", C_WeaponCSBase::WeaponID::HUNTING_RIFLE },
            { "mac10", C_WeaponCSBase::WeaponID::MAC10 },
            { "mac_10", C_WeaponCSBase::WeaponID::MAC10 },
            { "scar", C_WeaponCSBase::WeaponID::SCAR },
            { "desert_rifle", C_WeaponCSBase::WeaponID::SCAR },
            { "sniper_military", C_WeaponCSBase::WeaponID::SNIPER_MILITARY },
            { "military_sniper", C_WeaponCSBase::WeaponID::SNIPER_MILITARY },
            { "ak", C_WeaponCSBase::WeaponID::AK47 },
            { "ak47", C_WeaponCSBase::WeaponID::AK47 },
            { "magnum", C_WeaponCSBase::WeaponID::MAGNUM },
            { "deagle", C_WeaponCSBase::WeaponID::MAGNUM },
            { "desert_eagle", C_WeaponCSBase::WeaponID::MAGNUM },
            { "pistol_magnum", C_WeaponCSBase::WeaponID::MAGNUM },
            { "mp5", C_WeaponCSBase::WeaponID::MP5 },
            { "sg552", C_WeaponCSBase::WeaponID::SG552 },
            { "awp", C_WeaponCSBase::WeaponID::AWP },
            { "scout", C_WeaponCSBase::WeaponID::SCOUT },
            { "m60", C_WeaponCSBase::WeaponID::M60 },
            { "machinegun_m60", C_WeaponCSBase::WeaponID::M60 },
            { "pump", C_WeaponCSBase::WeaponID::PUMPSHOTGUN },
            { "pumpshotgun", C_WeaponCSBase::WeaponID::PUMPSHOTGUN },
            { "pump_shotgun", C_WeaponCSBase::WeaponID::PUMPSHOTGUN },
            { "shotgun", C_WeaponCSBase::WeaponID::PUMPSHOTGUN },
            { "chrome", C_WeaponCSBase::WeaponID::SHOTGUN_CHROME },
            { "shotgun_chrome", C_WeaponCSBase::WeaponID::SHOTGUN_CHROME },
            { "chrome_shotgun", C_WeaponCSBase::WeaponID::SHOTGUN_CHROME },
            { "auto", C_WeaponCSBase::WeaponID::AUTOSHOTGUN },
            { "autoshotgun", C_WeaponCSBase::WeaponID::AUTOSHOTGUN },
            { "auto_shotgun", C_WeaponCSBase::WeaponID::AUTOSHOTGUN },
            { "shotgun_spas", C_WeaponCSBase::WeaponID::SPAS },
            { "spas", C_WeaponCSBase::WeaponID::SPAS },
            { "spas12", C_WeaponCSBase::WeaponID::SPAS }
        };

        auto parseBoneOverrideWeaponId = [&](const std::string& rawKey, int& outWeaponId)
            {
                const std::string key = normalizeBoneOverrideWeaponKey(rawKey);
                if (key.empty())
                    return false;

                char* end = nullptr;
                const long numeric = std::strtol(key.c_str(), &end, 10);
                if (end && *end == '\0' && numeric > 0 && numeric <= static_cast<long>(C_WeaponCSBase::WeaponID::UPGRADE_ITEM))
                {
                    outWeaponId = static_cast<int>(numeric);
                    return true;
                }

                const auto it = weaponBoneOverrideAliases.find(key);
                if (it == weaponBoneOverrideAliases.end())
                    return false;

                outWeaponId = it->second;
                return true;
            };

        auto parseBoneOverrideSpec = [&](
            const char* configName,
            const std::string& spec,
            std::unordered_map<int, std::vector<std::string>>& outOverrides,
            std::unordered_map<std::string, std::vector<std::string>>& outProfileOverrides,
            bool allowProfileOverrides)
            {
                std::string normalizedSpec = spec;
                std::replace(normalizedSpec.begin(), normalizedSpec.end(), ';', ',');
                std::stringstream ss(normalizedSpec);
                std::string entry;
                while (std::getline(ss, entry, ','))
                {
                    trim(entry);
                    if (entry.empty())
                        continue;

                    const size_t separator = entry.find(':');
                    if (separator == std::string::npos)
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    std::string weaponKey = entry.substr(0, separator);
                    std::string boneName = entry.substr(separator + 1);
                    trim(weaponKey);
                    trim(boneName);

                    int weaponId = 0;
                    if (boneName.empty())
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    if (parseBoneOverrideWeaponId(weaponKey, weaponId))
                    {
                        outOverrides[weaponId].push_back(boneName);
                        continue;
                    }

                    const std::string profileKey = normalizeBoneOverrideProfileKey(weaponKey);
                    if (!boneOverrideProfileKeyLooksValid(profileKey))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }
                    if (!allowProfileOverrides)
                        continue;

                    outProfileOverrides[profileKey].push_back(boneName);
                }
            };

        auto parseBoltPullAxisOverrideVector = [&](std::string value, Vector& outAxis)
            {
                trim(value);
                std::replace(value.begin(), value.end(), '|', ',');
                std::replace(value.begin(), value.end(), '/', ',');
                std::stringstream ss(value);
                std::string componentText;
                float components[3] = {};
                for (int i = 0; i < 3; ++i)
                {
                    if (!std::getline(ss, componentText, ','))
                        return false;
                    trim(componentText);
                    if (componentText.empty())
                        return false;
                    char* end = nullptr;
                    const float component = std::strtof(componentText.c_str(), &end);
                    if (!end || *end != '\0' || !std::isfinite(component))
                        return false;
                    components[i] = component;
                }

                std::string extra;
                while (std::getline(ss, extra, ','))
                {
                    trim(extra);
                    if (!extra.empty())
                        return false;
                }

                const Vector axis(components[0], components[1], components[2]);
                const float length = axis.Length();
                if (!(length > 0.0001f))
                    return false;

                outAxis = axis * (1.0f / length);
                return true;
            };

        auto parseBoltPullAxisOverrideSpec = [&](
            const char* configName,
            const std::string& spec,
            std::unordered_map<int, Vector>& outOverrides,
            std::unordered_map<std::string, Vector>* outProfileOverrides,
            bool allowProfileOverrides)
            {
                std::stringstream ss(spec);
                std::string entry;
                while (std::getline(ss, entry, ';'))
                {
                    trim(entry);
                    if (entry.empty())
                        continue;

                    size_t separator = entry.find(':');
                    if (separator == std::string::npos)
                        separator = entry.find('=');
                    if (separator == std::string::npos)
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    std::string weaponKey = entry.substr(0, separator);
                    std::string axisText = entry.substr(separator + 1);
                    trim(weaponKey);
                    trim(axisText);

                    int weaponId = 0;
                    Vector axis{};
                    if (axisText.empty() ||
                        !parseBoltPullAxisOverrideVector(axisText, axis))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    if (parseBoneOverrideWeaponId(weaponKey, weaponId))
                    {
                        outOverrides[weaponId] = axis;
                        continue;
                    }

                    const std::string profileKey = normalizeBoneOverrideProfileKey(weaponKey);
                    if (!outProfileOverrides || !boneOverrideProfileKeyLooksValid(profileKey))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }
                    if (!allowProfileOverrides)
                        continue;

                    (*outProfileOverrides)[profileKey] = axis;
                }
            };

        auto parseVector3OverrideValue = [&](std::string value, Vector& outValue)
            {
                trim(value);
                std::replace(value.begin(), value.end(), '|', ',');
                std::replace(value.begin(), value.end(), '/', ',');
                std::stringstream ss(value);
                std::string componentText;
                float components[3] = {};
                for (int i = 0; i < 3; ++i)
                {
                    if (!std::getline(ss, componentText, ','))
                        return false;
                    trim(componentText);
                    if (componentText.empty())
                        return false;
                    char* end = nullptr;
                    const float component = std::strtof(componentText.c_str(), &end);
                    if (!end || *end != '\0' || !std::isfinite(component))
                        return false;
                    components[i] = component;
                }

                std::string extra;
                while (std::getline(ss, extra, ','))
                {
                    trim(extra);
                    if (!extra.empty())
                        return false;
                }

                outValue = Vector(components[0], components[1], components[2]);
                return true;
            };

        auto parseVector3OverrideSpec = [&](
            const char* configName,
            const std::string& spec,
            float minValue,
            float maxValue,
            std::unordered_map<int, Vector>& outOverrides,
            std::unordered_map<std::string, Vector>* outProfileOverrides,
            bool allowProfileOverrides)
            {
                std::stringstream ss(spec);
                std::string entry;
                while (std::getline(ss, entry, ';'))
                {
                    trim(entry);
                    if (entry.empty())
                        continue;

                    size_t separator = entry.find(':');
                    if (separator == std::string::npos)
                        separator = entry.find('=');
                    if (separator == std::string::npos)
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    std::string weaponKey = entry.substr(0, separator);
                    std::string valueText = entry.substr(separator + 1);
                    trim(weaponKey);
                    trim(valueText);

                    int weaponId = 0;
                    Vector value{};
                    if (valueText.empty() ||
                        !parseVector3OverrideValue(valueText, value))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    value.x = std::clamp(value.x, minValue, maxValue);
                    value.y = std::clamp(value.y, minValue, maxValue);
                    value.z = std::clamp(value.z, minValue, maxValue);
                    if (parseBoneOverrideWeaponId(weaponKey, weaponId))
                    {
                        outOverrides[weaponId] = value;
                        continue;
                    }

                    const std::string profileKey = normalizeBoneOverrideProfileKey(weaponKey);
                    if (!outProfileOverrides || !boneOverrideProfileKeyLooksValid(profileKey))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }
                    if (!allowProfileOverrides)
                        continue;

                    (*outProfileOverrides)[profileKey] = value;
                }
            };

        auto parseFloatOverrideSpec = [&](
            const char* configName,
            const std::string& spec,
            float minValue,
            float maxValue,
            std::unordered_map<int, float>& outOverrides,
            std::unordered_map<std::string, float>* outProfileOverrides,
            bool allowProfileOverrides)
            {
                std::string normalizedSpec = spec;
                std::replace(normalizedSpec.begin(), normalizedSpec.end(), ',', ';');
                std::stringstream ss(normalizedSpec);
                std::string entry;
                while (std::getline(ss, entry, ';'))
                {
                    trim(entry);
                    if (entry.empty())
                        continue;

                    size_t separator = entry.find(':');
                    if (separator == std::string::npos)
                        separator = entry.find('=');
                    if (separator == std::string::npos)
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    std::string weaponKey = entry.substr(0, separator);
                    std::string valueText = entry.substr(separator + 1);
                    trim(weaponKey);
                    trim(valueText);

                    int weaponId = 0;
                    char* end = nullptr;
                    const float value = std::strtof(valueText.c_str(), &end);
                    if (valueText.empty() ||
                        !end ||
                        *end != '\0' ||
                        !std::isfinite(value))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }

                    const float clampedValue = std::clamp(value, minValue, maxValue);
                    if (parseBoneOverrideWeaponId(weaponKey, weaponId))
                    {
                        outOverrides[weaponId] = clampedValue;
                        continue;
                    }

                    const std::string profileKey = normalizeBoneOverrideProfileKey(weaponKey);
                    if (!outProfileOverrides || !boneOverrideProfileKeyLooksValid(profileKey))
                    {
                        Game::logMsg("[VR][Config] ignored invalid %s entry=%s", configName, entry.c_str());
                        continue;
                    }
                    if (!allowProfileOverrides)
                        continue;

                    (*outProfileOverrides)[profileKey] = clampedValue;
                }
            };

        parseBoneOverrideSpec(
            "ManualReloadMagazineBoneOverrides",
            m_MagazineInteractionMagazineBoneOverridesSpec,
            m_MagazineInteractionMagazineBoneOverrides,
            m_MagazineInteractionMagazineBoneProfileOverrides,
            false);
        parseBoneOverrideSpec(
            "MagazineInteractionBoltBoneOverrides",
            m_MagazineInteractionBoltBoneOverridesSpec,
            m_MagazineInteractionBoltBoneOverrides,
            m_MagazineInteractionBoltBoneProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionMagazineBoxHalfExtentsMetersOverrides",
            m_MagazineInteractionMagazineBoxHalfExtentsMetersOverridesSpec,
            0.0f,
            0.50f,
            m_MagazineInteractionMagazineBoxHalfExtentsMetersOverrides,
            &m_MagazineInteractionMagazineBoxHalfExtentsMetersProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionMagazineBoxLocalOffsetMetersOverrides",
            m_MagazineInteractionMagazineBoxLocalOffsetMetersOverridesSpec,
            -0.50f,
            0.50f,
            m_MagazineInteractionMagazineBoxLocalOffsetMetersOverrides,
            &m_MagazineInteractionMagazineBoxLocalOffsetMetersProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionMagazineBoxLocalRotationOffsetDegOverrides",
            m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverridesSpec,
            -180.0f,
            180.0f,
            m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverrides,
            &m_MagazineInteractionMagazineBoxLocalRotationOffsetDegProfileOverrides,
            false);
        parseBoltPullAxisOverrideSpec(
            "MagazineInteractionBoltPullAxisLocalOverrides",
            m_MagazineInteractionBoltPullAxisLocalOverridesSpec,
            m_MagazineInteractionBoltPullAxisLocalOverrides,
            &m_MagazineInteractionBoltPullAxisLocalProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionBoltBoxHalfExtentsMetersOverrides",
            m_MagazineInteractionBoltBoxHalfExtentsMetersOverridesSpec,
            0.005f,
            0.25f,
            m_MagazineInteractionBoltBoxHalfExtentsMetersOverrides,
            &m_MagazineInteractionBoltBoxHalfExtentsMetersProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionBoltBoxLocalOffsetMetersOverrides",
            m_MagazineInteractionBoltBoxLocalOffsetMetersOverridesSpec,
            -0.25f,
            0.25f,
            m_MagazineInteractionBoltBoxLocalOffsetMetersOverrides,
            &m_MagazineInteractionBoltBoxLocalOffsetMetersProfileOverrides,
            false);
        parseFloatOverrideSpec(
            "MagazineInteractionBoltGrabPaddingMetersOverrides",
            m_MagazineInteractionBoltGrabPaddingMetersOverridesSpec,
            0.0f,
            0.25f,
            m_MagazineInteractionBoltGrabPaddingMetersOverrides,
            &m_MagazineInteractionBoltGrabPaddingMetersProfileOverrides,
            false);
        parseFloatOverrideSpec(
            "MagazineInteractionBoltPullDistanceMetersOverrides",
            m_MagazineInteractionBoltPullDistanceMetersOverridesSpec,
            0.0f,
            0.25f,
            m_MagazineInteractionBoltPullDistanceMetersOverrides,
            &m_MagazineInteractionBoltPullDistanceMetersProfileOverrides,
            false);
        parseFloatOverrideSpec(
            "MagazineInteractionBoltReturnDistanceMetersOverrides",
            m_MagazineInteractionBoltReturnDistanceMetersOverridesSpec,
            0.0f,
            0.10f,
            m_MagazineInteractionBoltReturnDistanceMetersOverrides,
            &m_MagazineInteractionBoltReturnDistanceMetersProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides",
            m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverridesSpec,
            0.0f,
            0.50f,
            m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides,
            &m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides",
            m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverridesSpec,
            -0.50f,
            0.50f,
            m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides,
            &m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersProfileOverrides,
            false);
        parseVector3OverrideSpec(
            "MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides",
            m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverridesSpec,
            -180.0f,
            180.0f,
            m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides,
            &m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegProfileOverrides,
            false);
        parseFloatOverrideSpec(
            "MagazineInteractionSocketCaptureAngleDegOverrides",
            m_MagazineInteractionSocketCaptureAngleDegOverridesSpec,
            0.0f,
            89.0f,
            m_MagazineInteractionSocketCaptureAngleDegOverrides,
            &m_MagazineInteractionSocketCaptureAngleDegProfileOverrides,
            false);
        parseFloatOverrideSpec(
            "MagazineInteractionSocketRequiredDepthMetersOverrides",
            m_MagazineInteractionSocketRequiredDepthMetersOverridesSpec,
            0.0f,
            0.25f,
            m_MagazineInteractionSocketRequiredDepthMetersOverrides,
            &m_MagazineInteractionSocketRequiredDepthMetersProfileOverrides,
            false);
        parseFloatOverrideSpec(
            "MagazineInteractionSocketRequiredOverlapFractionOverrides",
            m_MagazineInteractionSocketRequiredOverlapFractionOverridesSpec,
            0.0f,
            1.0f,
            m_MagazineInteractionSocketRequiredOverlapFractionOverrides,
            &m_MagazineInteractionSocketRequiredOverlapFractionProfileOverrides,
            false);

        auto normalizeCustomGunKey = [&](std::string value)
            {
                trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return value;
            };

        auto customGunKeyAllowsHash = [&](const std::string& normalizedKey)
            {
                return normalizedKey == "manualreloadmagazinebone" ||
                    normalizedKey == "manualreloadmagazineboneoverride" ||
                    normalizedKey == "manualreloadmagazineboneoverrides" ||
                    normalizedKey == "magazineinteractionboltbone" ||
                    normalizedKey == "magazineinteractionboltboneoverride" ||
                    normalizedKey == "magazineinteractionboltboneoverrides";
            };

        auto customGunProfileKeyFromFilename = [&](const std::filesystem::path& path)
            {
                std::string stem = path.stem().string();
                std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto isHex = [](char ch)
                    {
                        const unsigned char c = static_cast<unsigned char>(ch);
                        return std::isxdigit(c) != 0;
                    };

                for (size_t bs = stem.find("bs"); bs != std::string::npos; bs = stem.find("bs", bs + 1))
                {
                    if (bs + 10u > stem.size())
                        continue;

                    bool valid = true;
                    for (size_t i = bs + 2u; i < bs + 10u; ++i)
                        valid = valid && isHex(stem[i]);
                    if (valid)
                        return stem.substr(bs, 10u);
                }
                return std::string();
            };

        auto parseCustomGunProfileFile = [&](const std::filesystem::path& path)
            {
                std::string profileKey = customGunProfileKeyFromFilename(path);
                if (profileKey.empty())
                    return false;

                std::ifstream input(path);
                if (!input)
                    return false;

                std::unordered_map<std::string, std::string> fileValues;
                std::string line;
                while (std::getline(input, line))
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    const size_t eqBeforeComment = line.find('=');
                    size_t cut = std::string::npos;
                    const size_t p1 = line.find("//");
                    const size_t p2 = line.find('#');
                    const size_t p3 = line.find(';');
                    std::string keyBeforeComment =
                        eqBeforeComment == std::string::npos ? std::string() : line.substr(0, eqBeforeComment);
                    const std::string normalizedKeyBeforeComment = normalizeCustomGunKey(keyBeforeComment);
                    if (p1 != std::string::npos)
                        cut = p1;
                    if (p2 != std::string::npos &&
                        (eqBeforeComment == std::string::npos ||
                            p2 < eqBeforeComment ||
                            !customGunKeyAllowsHash(normalizedKeyBeforeComment)))
                    {
                        cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
                    }
                    if (p3 != std::string::npos &&
                        (eqBeforeComment == std::string::npos || p3 < eqBeforeComment))
                    {
                        cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
                    }
                    if (cut != std::string::npos)
                        line.erase(cut);

                    trim(line);
                    if (line.empty())
                        continue;

                    const size_t eq = line.find('=');
                    if (eq == std::string::npos)
                        continue;

                    std::string key = line.substr(0, eq);
                    std::string value = line.substr(eq + 1);
                    trim(key);
                    trim(value);
                    if (key.empty())
                        continue;

                    const std::string normalizedKey = normalizeCustomGunKey(key);
                    if (normalizedKey == "profilekey")
                    {
                        const std::string explicitProfileKey = normalizeBoneOverrideProfileKey(value);
                        if (boneOverrideProfileKeyLooksValid(explicitProfileKey))
                            profileKey = explicitProfileKey;
                        continue;
                    }
                    fileValues[normalizedKey] = value;
                }

                if (!boneOverrideProfileKeyLooksValid(profileKey))
                    return false;

                bool applied = false;
                auto applyBone = [&](const char* key, std::unordered_map<std::string, std::vector<std::string>>& map)
                    {
                        const auto it = fileValues.find(key);
                        if (it == fileValues.end())
                            return;
                        std::string value = it->second;
                        trim(value);
                        if (value.empty())
                            return;
                        map[profileKey] = { value };
                        applied = true;
                    };
                auto applyVector = [&](
                    const char* key,
                    float minValue,
                    float maxValue,
                    std::unordered_map<std::string, Vector>& map)
                    {
                        const auto it = fileValues.find(key);
                        if (it == fileValues.end())
                            return;
                        Vector value{};
                        if (!parseVector3OverrideValue(it->second, value))
                            return;
                        value.x = std::clamp(value.x, minValue, maxValue);
                        value.y = std::clamp(value.y, minValue, maxValue);
                        value.z = std::clamp(value.z, minValue, maxValue);
                        map[profileKey] = value;
                        applied = true;
                    };
                auto applyFloat = [&](
                    const char* key,
                    float minValue,
                    float maxValue,
                    std::unordered_map<std::string, float>& map)
                    {
                        const auto it = fileValues.find(key);
                        if (it == fileValues.end())
                            return;
                        char* end = nullptr;
                        const float parsed = std::strtof(it->second.c_str(), &end);
                        if (it->second.empty() || !end || *end != '\0' || !std::isfinite(parsed))
                            return;
                        map[profileKey] = std::clamp(parsed, minValue, maxValue);
                        applied = true;
                    };
                auto applyAxis = [&](const char* key, std::unordered_map<std::string, Vector>& map)
                    {
                        const auto it = fileValues.find(key);
                        if (it == fileValues.end())
                            return;
                        Vector axis{};
                        if (!parseBoltPullAxisOverrideVector(it->second, axis))
                            return;
                        map[profileKey] = axis;
                        applied = true;
                    };

                applyBone("manualreloadmagazinebone", m_MagazineInteractionMagazineBoneProfileOverrides);
                applyBone("manualreloadmagazineboneoverride", m_MagazineInteractionMagazineBoneProfileOverrides);
                applyBone("manualreloadmagazineboneoverrides", m_MagazineInteractionMagazineBoneProfileOverrides);
                applyBone("magazineinteractionboltbone", m_MagazineInteractionBoltBoneProfileOverrides);
                applyBone("magazineinteractionboltboneoverride", m_MagazineInteractionBoltBoneProfileOverrides);
                applyBone("magazineinteractionboltboneoverrides", m_MagazineInteractionBoltBoneProfileOverrides);

                applyVector("magazineinteractionmagazineboxhalfextentsmeters", 0.0f, 0.50f, m_MagazineInteractionMagazineBoxHalfExtentsMetersProfileOverrides);
                applyVector("magazineinteractionmagazineboxlocaloffsetmeters", -0.50f, 0.50f, m_MagazineInteractionMagazineBoxLocalOffsetMetersProfileOverrides);
                applyVector("magazineinteractionmagazineboxlocalrotationoffsetdeg", -180.0f, 180.0f, m_MagazineInteractionMagazineBoxLocalRotationOffsetDegProfileOverrides);
                applyVector("magazineinteractionsocketcaptureboxhalfextentsmeters", 0.0f, 0.50f, m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersProfileOverrides);
                applyVector("magazineinteractionsocketcaptureboxlocaloffsetmeters", -0.50f, 0.50f, m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersProfileOverrides);
                applyVector("magazineinteractionsocketcaptureboxlocalrotationoffsetdeg", -180.0f, 180.0f, m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegProfileOverrides);
                applyVector("magazineinteractionboltboxhalfextentsmeters", 0.005f, 0.25f, m_MagazineInteractionBoltBoxHalfExtentsMetersProfileOverrides);
                applyVector("magazineinteractionboltboxlocaloffsetmeters", -0.25f, 0.25f, m_MagazineInteractionBoltBoxLocalOffsetMetersProfileOverrides);
                applyAxis("magazineinteractionboltpullaxislocal", m_MagazineInteractionBoltPullAxisLocalProfileOverrides);
                applyFloat("magazineinteractionsocketcaptureangledeg", 0.0f, 89.0f, m_MagazineInteractionSocketCaptureAngleDegProfileOverrides);
                applyFloat("magazineinteractionsocketrequireddepthmeters", 0.0f, 0.25f, m_MagazineInteractionSocketRequiredDepthMetersProfileOverrides);
                applyFloat("magazineinteractionsocketrequiredoverlapfraction", 0.0f, 1.0f, m_MagazineInteractionSocketRequiredOverlapFractionProfileOverrides);
                applyFloat("magazineinteractionboltgrabpaddingmeters", 0.0f, 0.25f, m_MagazineInteractionBoltGrabPaddingMetersProfileOverrides);
                applyFloat("magazineinteractionboltpulldistancemeters", 0.0f, 0.25f, m_MagazineInteractionBoltPullDistanceMetersProfileOverrides);
                applyFloat("magazineinteractionboltreturndistancemeters", 0.0f, 0.10f, m_MagazineInteractionBoltReturnDistanceMetersProfileOverrides);

                return applied;
            };

        auto loadCustomGunProfiles = [&]()
            {
                const std::filesystem::path customGunDir("VR\\customguns");
                std::error_code ec;
                if (!std::filesystem::exists(customGunDir, ec) ||
                    !std::filesystem::is_directory(customGunDir, ec))
                {
                    return;
                }

                std::vector<std::filesystem::path> profileFiles;
                for (std::filesystem::directory_iterator it(customGunDir, ec), end; !ec && it != end; it.increment(ec))
                {
                    const std::filesystem::directory_entry& entry = *it;
                    if (!entry.is_regular_file(ec))
                        continue;
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (ext != ".txt" && ext != ".cfg")
                        continue;
                    profileFiles.push_back(entry.path());
                }
                std::sort(profileFiles.begin(), profileFiles.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b)
                    {
                        const std::string aKey = customGunProfileKeyFromFilename(a);
                        const std::string bKey = customGunProfileKeyFromFilename(b);
                        auto canonicalRank = [&](const std::filesystem::path& path, const std::string& key)
                            {
                                if (key.empty())
                                    return 0;
                                std::string stem = path.stem().string();
                                std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                const std::string suffix = std::string("_") + key;
                                const bool legacyFpBsName =
                                    stem.find("_fp") != std::string::npos &&
                                    stem.find("_bs") != std::string::npos;
                                return stem.size() >= suffix.size() &&
                                    stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                                    !legacyFpBsName
                                    ? 1
                                    : 0;
                            };
                        const int aRank = canonicalRank(a, aKey);
                        const int bRank = canonicalRank(b, bKey);
                        if (aRank != bRank)
                            return aRank < bRank;
                        return a.filename().string() < b.filename().string();
                    });
                for (const std::filesystem::path& path : profileFiles)
                    parseCustomGunProfileFile(path);
            };
        loadCustomGunProfiles();
    }
    m_MagazineInteractionMagazineInsertionAxisLocal = getVector3("ManualReloadMagazineInsertionAxisLocal", m_MagazineInteractionMagazineInsertionAxisLocal);
    m_MagazineInteractionMagazineHandOffsetMeters = getVector3("ManualReloadMagazineHandOffsetMeters", m_MagazineInteractionMagazineHandOffsetMeters);
    m_MagazineInteractionMagazineHandRotationOffsetDeg = getVector3("ManualReloadMagazineHandRotationOffsetDeg", m_MagazineInteractionMagazineHandRotationOffsetDeg);
    m_SplitArmsToControllers = getBool("SplitArmsToControllers", m_SplitArmsToControllers);
    m_HudDistance = getFloat("HudDistance", m_HudDistance);
    m_HudSize = getFloat("HudSize", m_HudSize);
    m_FixedHudXOffset = std::clamp(getFloat("FixedHudXOffset", m_FixedHudXOffset), -2.0f, 2.0f);
    m_FixedHudYOffset = std::clamp(getFloat("FixedHudYOffset", m_FixedHudYOffset), -2.0f, 2.0f);
    m_TopHudCurvature = std::clamp(getFloat("TopHudCurvature", m_TopHudCurvature), 0.0f, 1.0f);
    m_HudFollowHmdMovement = getBool("HudFollowHmdMovement", m_HudFollowHmdMovement);
    m_HudAlwaysVisible = getBool("HudAlwaysVisible", m_HudAlwaysVisible);
    m_HudToggleState = m_HudAlwaysVisible ||
        m_HudLiftGestureActive.load(std::memory_order_acquire) != 0;

    // Hand HUD background opacity (0..1)
    m_LeftWristHudBgAlpha = std::clamp(getFloat("LeftWristHudBgAlpha", m_LeftWristHudBgAlpha), 0.0f, 1.0f);
    m_RightAmmoHudBgAlpha = std::clamp(getFloat("RightAmmoHudBgAlpha", m_RightAmmoHudBgAlpha), 0.0f, 1.0f);

    // Right ammo HUD: crop ratio (U max). Removes unused blank area on the right.
    m_RightAmmoHudUVMaxU = std::clamp(getFloat("RightAmmoHudUVMaxU", m_RightAmmoHudUVMaxU), 0.05f, 1.0f);

    // Hand HUD alpha (legacy): used as an extra BACKGROUND opacity multiplier (0..1).
    // We no longer apply IVROverlay::SetOverlayAlpha to the whole overlay because it makes text/bars fade too.
    const float leftBgMul = std::clamp(getFloat("LeftWristHudAlpha", m_LeftWristHudAlpha), 0.0f, 1.0f);
    const float rightBgMul = std::clamp(getFloat("RightAmmoHudAlpha", m_RightAmmoHudAlpha), 0.0f, 1.0f);
    m_LeftWristHudBgAlpha = std::clamp(m_LeftWristHudBgAlpha * leftBgMul, 0.0f, 1.0f);
    m_RightAmmoHudBgAlpha = std::clamp(m_RightAmmoHudBgAlpha * rightBgMul, 0.0f, 1.0f);
    m_LeftWristHudAlpha = 1.0f;
    m_RightAmmoHudAlpha = 1.0f;

    // Left wrist HUD: battery label font scale (1..4)
    m_LeftWristHudBatteryTextScale = std::clamp(getInt("LeftWristHudBatteryTextScale", m_LeftWristHudBatteryTextScale), 1, 4);

    // Hand HUD temp health decay (HP per second)
    m_HandHudTempHealthDecayRate = std::max(0.0f, getFloat("HandHudTempHealthDecayRate", m_HandHudTempHealthDecayRate));

    // Hand HUD overlays (SteamVR overlay, raw)
    m_LeftWristHudEnabled = getBool("LeftWristHudEnabled", m_LeftWristHudEnabled);
    m_LeftWristHudWidthMeters = std::clamp(getFloat("LeftWristHudWidthMeters", m_LeftWristHudWidthMeters), 0.01f, 1.0f);
    m_LeftWristHudXOffset = getFloat("LeftWristHudXOffset", m_LeftWristHudXOffset);
    m_LeftWristHudYOffset = getFloat("LeftWristHudYOffset", m_LeftWristHudYOffset);
    m_LeftWristHudZOffset = getFloat("LeftWristHudZOffset", m_LeftWristHudZOffset);
    {
        const Vector def = { m_LeftWristHudAngleOffset.x, m_LeftWristHudAngleOffset.y, m_LeftWristHudAngleOffset.z };
        const Vector ang = getVector3("LeftWristHudAngleOffset", def);
        m_LeftWristHudAngleOffset = { ang.x, ang.y, ang.z };
    }

    m_LeftWristHudCurvature = std::clamp(getFloat("LeftWristHudCurvature", m_LeftWristHudCurvature), 0.0f, 1.0f);
    m_LeftWristHudShowBattery = getBool("LeftWristHudShowBattery", m_LeftWristHudShowBattery);
    m_LeftWristHudShowTeammates = getBool("LeftWristHudShowTeammates", m_LeftWristHudShowTeammates);

    m_RightAmmoHudEnabled = getBool("RightAmmoHudEnabled", m_RightAmmoHudEnabled);
    m_RightAmmoHudWidthMeters = std::clamp(getFloat("RightAmmoHudWidthMeters", m_RightAmmoHudWidthMeters), 0.01f, 1.0f);
    m_RightAmmoHudXOffset = getFloat("RightAmmoHudXOffset", m_RightAmmoHudXOffset);
    m_RightAmmoHudYOffset = getFloat("RightAmmoHudYOffset", m_RightAmmoHudYOffset);
    m_RightAmmoHudZOffset = getFloat("RightAmmoHudZOffset", m_RightAmmoHudZOffset);
    {
        const Vector def = { m_RightAmmoHudAngleOffset.x, m_RightAmmoHudAngleOffset.y, m_RightAmmoHudAngleOffset.z };
        const Vector ang = getVector3("RightAmmoHudAngleOffset", def);
        m_RightAmmoHudAngleOffset = { ang.x, ang.y, ang.z };
    }


    // Hand HUD: world-quad mode (HMD-relative overlays using GPU textures)
    m_HandHudWorldQuadEnabled = getBool("HandHudWorldQuadEnabled", m_HandHudWorldQuadEnabled);
    m_HandHudWorldQuadAttachToControllers = getBool("HandHudWorldQuadAttachToControllers", m_HandHudWorldQuadAttachToControllers);
    m_HandHudWorldQuadDistanceMeters = std::clamp(getFloat("HandHudWorldQuadDistanceMeters", m_HandHudWorldQuadDistanceMeters), 0.05f, 5.0f);
    m_HandHudWorldQuadYOffsetMeters = std::clamp(getFloat("HandHudWorldQuadYOffsetMeters", m_HandHudWorldQuadYOffsetMeters), -2.0f, 2.0f);
    m_HandHudWorldQuadXGapMeters = std::clamp(getFloat("HandHudWorldQuadXGapMeters", m_HandHudWorldQuadXGapMeters), 0.0f, 0.5f);
    {
        const Vector def = { m_HandHudWorldQuadAngleOffset.x, m_HandHudWorldQuadAngleOffset.y, m_HandHudWorldQuadAngleOffset.z };
        const Vector ang = getVector3("HandHudWorldQuadAngleOffset", def);
        m_HandHudWorldQuadAngleOffset = { ang.x, ang.y, ang.z };
    }

    // Debug: hand HUD update diagnostics (prints periodic state + overlay errors).
    m_HandHudDebugLog = getBool("HandHudDebugLog", m_HandHudDebugLog);
    m_HandHudDebugLogHz = std::clamp(getFloat("HandHudDebugLogHz", m_HandHudDebugLogHz), 0.0f, 240.0f);

    m_AntiAliasing = static_cast<uint32_t>(std::max(0, getInt("AntiAliasing", 0)));
    m_FixedHudDistanceOffset = getFloat("FixedHudDistanceOffset", m_FixedHudDistanceOffset);
    float controllerSmoothingValue = m_ControllerSmoothing;
    const bool hasControllerSmoothing = userConfig.find("ControllerSmoothing") != userConfig.end();
    const bool hasHeadSmoothing = userConfig.find("HeadSmoothing") != userConfig.end();
    if (hasControllerSmoothing)
        controllerSmoothingValue = getFloat("ControllerSmoothing", controllerSmoothingValue);
    else if (hasHeadSmoothing) // Backward compatibility: old configs used HeadSmoothing
        controllerSmoothingValue = getFloat("HeadSmoothing", controllerSmoothingValue);
    m_ControllerSmoothing = std::clamp(controllerSmoothingValue, 0.0f, 0.99f);

    float headSmoothingValue = m_HeadSmoothing;
    if (hasHeadSmoothing)
        headSmoothingValue = getFloat("HeadSmoothing", headSmoothingValue);
    else
        headSmoothingValue = controllerSmoothingValue; // Match controller smoothing by default
    m_HeadSmoothing = std::clamp(headSmoothingValue, 0.0f, 0.99f);
    m_AutoBunnyHop = getBool("AutoBunnyHop", m_AutoBunnyHop);
    m_AutoAirStrafe = getBool("AutoAirStrafe", m_AutoAirStrafe);
    m_AutoAirStrafeLandingSpeedPreserve = getBool("AutoAirStrafeLandingSpeedPreserve", m_AutoAirStrafeLandingSpeedPreserve);
    m_AutoAirStrafeMaxGainPerHop = std::clamp(getFloat("AutoAirStrafeMaxGainPerHop", m_AutoAirStrafeMaxGainPerHop), 0.0f, 100.0f);
    m_AutoAirStrafeTargetSpeed = std::clamp(getFloat("AutoAirStrafeTargetSpeed", m_AutoAirStrafeTargetSpeed), 0.0f, 2000.0f);
    m_AutoAirStrafeSpeedProjection = std::clamp(getFloat("AutoAirStrafeSpeedProjection", m_AutoAirStrafeSpeedProjection), 0.0f, 29.0f);
    m_AutoAirStrafeMaxTurnBrakeProjection = std::clamp(getFloat("AutoAirStrafeMaxTurnBrakeProjection", m_AutoAirStrafeMaxTurnBrakeProjection), 0.0f, 30.0f);
    m_AutoAirStrafeTurnResponsiveness = std::clamp(getFloat("AutoAirStrafeTurnResponsiveness", m_AutoAirStrafeTurnResponsiveness), 0.0f, 1.0f);
    m_AutoAirStrafeDebugLog = getBool("AutoAirStrafeDebugLog", m_AutoAirStrafeDebugLog);
    m_AutoAirStrafeDebugLogHz = std::clamp(getFloat("AutoAirStrafeDebugLogHz", m_AutoAirStrafeDebugLogHz), 0.0f, 60.0f);
    m_LedgeGuardEnabled = getBool("LedgeGuardEnabled", m_LedgeGuardEnabled);
    m_LedgeGuardProbeDistance = std::clamp(getFloat("LedgeGuardProbeDistance", m_LedgeGuardProbeDistance), 8.0f, 128.0f);
    m_LedgeGuardProbeHeight = std::clamp(getFloat("LedgeGuardProbeHeight", m_LedgeGuardProbeHeight), 4.0f, 64.0f);
    m_LedgeGuardDropDistance = std::clamp(getFloat("LedgeGuardDropDistance", m_LedgeGuardDropDistance), 24.0f, 256.0f);
    m_LedgeGuardMinMoveSpeed = std::clamp(getFloat("LedgeGuardMinMoveSpeed", m_LedgeGuardMinMoveSpeed), 0.0f, 450.0f);
    m_LedgeGuardDebugLog = getBool("LedgeGuardDebugLog", m_LedgeGuardDebugLog);
    m_LedgeGuardDebugLogHz = std::clamp(getFloat("LedgeGuardDebugLogHz", m_LedgeGuardDebugLogHz), 0.0f, 60.0f);
    m_MotionGesturesEnabled = getBool("MotionGesturesEnabled", m_MotionGesturesEnabled);
    m_MotionGestureSwingEnabled = getBool("MotionGestureSwingEnabled", m_MotionGestureSwingEnabled);
    m_MotionGesturePushEnabled = getBool("MotionGesturePushEnabled", m_MotionGesturePushEnabled);
    m_MotionGestureDownSwingEnabled = getBool("MotionGestureDownSwingEnabled", m_MotionGestureDownSwingEnabled);
    m_MotionGestureJumpEnabled = getBool("MotionGestureJumpEnabled", m_MotionGestureJumpEnabled);
    m_MotionGestureSwingThreshold = std::max(0.0f, getFloat("MotionGestureSwingThreshold", m_MotionGestureSwingThreshold));
    m_MotionGesturePushThreshold = std::max(0.0f, getFloat("MotionGesturePushThreshold", m_MotionGesturePushThreshold));
    m_MotionGestureDownSwingThreshold = std::max(0.0f, getFloat("MotionGestureDownSwingThreshold", m_MotionGestureDownSwingThreshold));
    m_MotionGestureJumpThreshold = std::max(0.0f, getFloat("MotionGestureJumpThreshold", m_MotionGestureJumpThreshold));
    if (!m_MotionGestureSwingEnabled)
        m_MotionGestureSwingThreshold = 999.0f;
    if (!m_MotionGesturePushEnabled)
        m_MotionGesturePushThreshold = 999.0f;
    if (!m_MotionGestureDownSwingEnabled)
        m_MotionGestureDownSwingThreshold = 999.0f;
    if (!m_MotionGestureJumpEnabled)
        m_MotionGestureJumpThreshold = 999.0f;
    m_MotionGestureCooldown = std::max(0.0f, getFloat("MotionGestureCooldown", m_MotionGestureCooldown));
    m_MotionGestureHoldDuration = std::max(0.0f, getFloat("MotionGestureHoldDuration", m_MotionGestureHoldDuration));
    m_ViewmodelAdjustEnabled = getBool("ViewmodelAdjustEnabled", m_ViewmodelAdjustEnabled);
    m_ViewmodelAdjustMoveSpeed = std::clamp(getFloat("ViewmodelAdjustMoveSpeed", m_ViewmodelAdjustMoveSpeed), 0.1f, 5.0f);
    m_ViewmodelAdjustRotateSpeed = std::clamp(getFloat("ViewmodelAdjustRotateSpeed", m_ViewmodelAdjustRotateSpeed), 0.1f, 5.0f);
    m_AimLineThickness = std::max(0.0f, getFloat("AimLineThickness", m_AimLineThickness));
    m_AimLineEnabled = getBool("AimLineEnabled", m_AimLineEnabled);
    m_AimLineOnlyWhenLaserSight = getBool("AimLineOnlyWhenLaserSight", m_AimLineOnlyWhenLaserSight);
    m_AimLineConfigEnabled = m_AimLineEnabled;
    m_BlockFireOnFriendlyAimEnabled = getBool("BlockFireOnFriendlyAimEnabled", m_BlockFireOnFriendlyAimEnabled);
    m_BlockFireOnFriendlyAimRadiusMeters = std::clamp(getFloat("BlockFireOnFriendlyAimRadiusMeters", m_BlockFireOnFriendlyAimRadiusMeters), 0.0f, 0.5f);
    m_AutoRepeatSemiAutoFire = getBool("AutoRepeatSemiAutoFire", m_AutoRepeatSemiAutoFire);
    m_AutoRepeatSemiAutoFireHz = std::max(0.0f, getFloat("AutoRepeatSemiAutoFireHz", m_AutoRepeatSemiAutoFireHz));
    m_AutoRepeatSprayPushEnabled = getBool("AutoRepeatSprayPushEnabled", m_AutoRepeatSprayPushEnabled);
    m_AutoRepeatSprayPushDelayTicks = std::clamp(getInt("AutoRepeatSprayPushDelayTicks", m_AutoRepeatSprayPushDelayTicks), 0, 8);
    m_AutoRepeatSprayPushHoldTicks = std::clamp(getInt("AutoRepeatSprayPushHoldTicks", m_AutoRepeatSprayPushHoldTicks), 1, 8);

    m_AutoFastMelee = getBool("AutoFastMelee", m_AutoFastMelee);
    m_AutoFastMeleeUseWeaponSwitch = getBool("AutoFastMeleeUseWeaponSwitch", getBool("AutoFastMeleeUseShove", m_AutoFastMeleeUseWeaponSwitch));
    m_HitSoundEnabled = getBool("HitSoundEnabled", m_HitSoundEnabled);
    m_HitSoundPlaybackCooldownSeconds = std::clamp(getFloat("HitSoundPlaybackCooldownSeconds", m_HitSoundPlaybackCooldownSeconds), 0.0f, 0.25f);
    m_HitSoundSpec = getString("HitSoundSpec", m_HitSoundSpec);
    m_HitSoundVolume = std::clamp(getFloat("HitSoundVolume", m_HitSoundVolume), 0.0f, 2.0f);
    m_KillSoundEnabled = getBool("KillSoundEnabled", m_KillSoundEnabled);
    m_KillSoundDetectionWindowSeconds = std::clamp(getFloat("KillSoundDetectionWindowSeconds", m_KillSoundDetectionWindowSeconds), 0.05f, 1.0f);
    m_KillSoundPlaybackCooldownSeconds = std::clamp(getFloat("KillSoundPlaybackCooldownSeconds", m_KillSoundPlaybackCooldownSeconds), 0.0f, 0.25f);
    m_KillSoundNormalSpec = getString("KillSoundNormalSpec", m_KillSoundNormalSpec);
    m_KillSoundHeadshotSpec = getString("KillSoundHeadshotSpec", m_KillSoundHeadshotSpec);
    m_KillSoundVolume = std::clamp(getFloat("KillSoundVolume", m_KillSoundVolume), 0.0f, 2.0f);
    m_HeadshotSoundVolume = std::clamp(getFloat("HeadshotSoundVolume", m_HeadshotSoundVolume), 0.0f, 2.0f);
    m_SpeechToTextEnabled = getBool("SpeechToTextEnabled", m_SpeechToTextEnabled);
    m_SpeechToTextSendChatEnabled = getBool("SpeechToTextSendChatEnabled", m_SpeechToTextSendChatEnabled);
    m_SpeechToTextSendVoiceEnabled = getBool("SpeechToTextSendVoiceEnabled", m_SpeechToTextSendVoiceEnabled);
    m_SpeechToTextSendVoiceLoopbackEnabled = getBool("SpeechToTextSendVoiceLoopbackEnabled", m_SpeechToTextSendVoiceLoopbackEnabled);
    m_SpeechToTextMinimumRecordSeconds = std::clamp(getFloat("SpeechToTextMinimumRecordSeconds", m_SpeechToTextMinimumRecordSeconds), 0.05f, 30.0f);
    m_SpeechToTextCommandPrefix = getString("SpeechToTextCommandPrefix", m_SpeechToTextCommandPrefix);
    m_SpeechToTextModel = getString("SpeechToTextModel", m_SpeechToTextModel);
    m_SpeechToTextLanguage = getString("SpeechToTextLanguage", m_SpeechToTextLanguage);
    m_SpeechToTextSendVoiceCommandPrefix = getString("SpeechToTextSendVoiceCommandPrefix", m_SpeechToTextSendVoiceCommandPrefix);
    m_SpeechToTextSendVoiceModel = getString("SpeechToTextSendVoiceModel", m_SpeechToTextSendVoiceModel);
    m_SpeechToTextSendVoiceWorkingDir = getString("SpeechToTextSendVoiceWorkingDir", m_SpeechToTextSendVoiceWorkingDir);
    m_SpeechToTextSendVoiceReferenceAudio = getString("SpeechToTextSendVoiceReferenceAudio", m_SpeechToTextSendVoiceReferenceAudio);
    m_SpeechToTextSendVoicePromptText = getString("SpeechToTextSendVoicePromptText", m_SpeechToTextSendVoicePromptText);
    m_SpeechToTextSendVoicePromptLanguage = getString("SpeechToTextSendVoicePromptLanguage", m_SpeechToTextSendVoicePromptLanguage);
    m_SpeechToTextSendVoiceLanguage = getString("SpeechToTextSendVoiceLanguage", m_SpeechToTextSendVoiceLanguage);
    m_SpeechToTextSendVoiceTextSplitMethod = getString("SpeechToTextSendVoiceTextSplitMethod", m_SpeechToTextSendVoiceTextSplitMethod);
    m_TextToSpeechEnabled = getBool("TextToSpeechEnabled", m_TextToSpeechEnabled);
    m_TextToSpeechSurvivorOnly = getBool("TextToSpeechSurvivorOnly", m_TextToSpeechSurvivorOnly);
    m_TextToSpeechIncludeSpeakerName = getBool("TextToSpeechIncludeSpeakerName", m_TextToSpeechIncludeSpeakerName);
    m_TextToSpeechSkipOwnMessages = getBool("TextToSpeechSkipOwnMessages", m_TextToSpeechSkipOwnMessages);
    m_TextToSpeechVolume = std::clamp(getFloat("TextToSpeechVolume", m_TextToSpeechVolume), 0.0f, 2.0f);
    m_TextToSpeechCommandPrefix = getString("TextToSpeechCommandPrefix", m_TextToSpeechCommandPrefix);
    m_TextToSpeechModel = getString("TextToSpeechModel", m_TextToSpeechModel);
    m_TextToSpeechWorkingDir = getString("TextToSpeechWorkingDir", m_TextToSpeechWorkingDir);
    m_TextToSpeechServerPort = std::clamp(getInt("TextToSpeechServerPort", m_TextToSpeechServerPort), 1, 65535);
    m_TextToSpeechReferenceAudio = getString("TextToSpeechReferenceAudio", m_TextToSpeechReferenceAudio);
    m_TextToSpeechPromptText = getString("TextToSpeechPromptText", m_TextToSpeechPromptText);
    m_TextToSpeechPromptLanguage = getString("TextToSpeechPromptLanguage", m_TextToSpeechPromptLanguage);
    m_TextToSpeechLanguage = getString("TextToSpeechLanguage", m_TextToSpeechLanguage);
    m_TextToSpeechTextSplitMethod = getString("TextToSpeechTextSplitMethod", m_TextToSpeechTextSplitMethod);
    m_TextToSpeechWhitelistRegexes = getString("TextToSpeechWhitelistRegexes", m_TextToSpeechWhitelistRegexes);
    m_TextToSpeechWhitelistSeparator = getString("TextToSpeechWhitelistSeparator", m_TextToSpeechWhitelistSeparator);
    m_FeedbackSoundSpatialBlend = std::clamp(getFloat("FeedbackSoundSpatialBlend", m_FeedbackSoundSpatialBlend), 0.0f, 1.0f);
    m_FeedbackSoundSpatialRange = std::clamp(getFloat("FeedbackSoundSpatialRange", m_FeedbackSoundSpatialRange), 64.0f, 8192.0f);
    m_FeedbackSoundDebugForceChannel = std::clamp(getInt("FeedbackSoundDebugForceChannel", m_FeedbackSoundDebugForceChannel), -1, 1);
    m_FeedbackSoundDebugLog = getBool("FeedbackSoundDebugLog", m_FeedbackSoundDebugLog);
    m_FeedbackSoundDebugLogHz = std::clamp(getFloat("FeedbackSoundDebugLogHz", m_FeedbackSoundDebugLogHz), 0.0f, 20.0f);
    SyncVrmodFeedbackGameSounds();
    m_HitIndicatorEnabled = getBool("HitIndicatorEnabled", m_HitIndicatorEnabled);
    m_KillIndicatorEnabled = getBool("KillIndicatorEnabled", m_KillIndicatorEnabled);
    m_KillIndicatorDebugLog = getBool("KillIndicatorDebugLog", m_KillIndicatorDebugLog);
    m_KillIndicatorDebugLogHz = std::clamp(getFloat("KillIndicatorDebugLogHz", m_KillIndicatorDebugLogHz), 0.0f, 20.0f);
    m_KillIndicatorLifetimeSeconds = std::clamp(getFloat("KillIndicatorLifetimeSeconds", m_KillIndicatorLifetimeSeconds), 0.10f, 3.0f);
    m_KillIndicatorSizePixels = std::clamp(getFloat("KillIndicatorSizePixels", m_KillIndicatorSizePixels), 16.0f, 512.0f);
    m_KillIndicatorRiseUnits = std::clamp(getFloat("KillIndicatorRiseUnits", m_KillIndicatorRiseUnits), 0.0f, 128.0f);
    m_KillIndicatorMaxDistance = std::clamp(getFloat("KillIndicatorMaxDistance", m_KillIndicatorMaxDistance), 128.0f, 16384.0f);
    m_KillIndicatorMaterialBaseSpec = getString("KillIndicatorMaterialBaseSpec", m_KillIndicatorMaterialBaseSpec);
    m_KillIndicatorHitMaterial = nullptr;
    m_KillIndicatorNormalMaterial = nullptr;
    m_KillIndicatorHeadshotMaterial = nullptr;
    m_MeleeAimLineEnabled = getBool("MeleeAimLineEnabled", m_MeleeAimLineEnabled);
    auto aimColor = getColor("AimLineColor", m_AimLineColorR, m_AimLineColorG, m_AimLineColorB, m_AimLineColorA);
    m_AimLineColorR = aimColor[0];
    m_AimLineColorG = aimColor[1];
    m_AimLineColorB = aimColor[2];
    m_AimLineColorA = aimColor[3];
    m_EffectiveAttackRangeIndicatorEnabled = getBool("EffectiveAttackRangeIndicatorEnabled", m_EffectiveAttackRangeIndicatorEnabled);
    m_EffectiveAttackRangeAutoFireEnabled = getBool("EffectiveAttackRangeAutoFireEnabled", m_EffectiveAttackRangeAutoFireEnabled);
    auto effectiveRangeColor = getColor("EffectiveAttackRangeColor",
        m_EffectiveAttackRangeColorR, m_EffectiveAttackRangeColorG, m_EffectiveAttackRangeColorB, m_AimLineColorA);
    m_EffectiveAttackRangeColorR = effectiveRangeColor[0];
    m_EffectiveAttackRangeColorG = effectiveRangeColor[1];
    m_EffectiveAttackRangeColorB = effectiveRangeColor[2];
    m_EffectiveAttackRangeDebugLog = getBool("EffectiveAttackRangeDebugLog", m_EffectiveAttackRangeDebugLog);
    m_EffectiveAttackRangeDebugLogHz = std::clamp(getFloat("EffectiveAttackRangeDebugLogHz", m_EffectiveAttackRangeDebugLogHz), 0.0f, 20.0f);
    m_EffectiveAttackRangeHoldSeconds = std::clamp(getFloat("EffectiveAttackRangeHoldSeconds", m_EffectiveAttackRangeHoldSeconds), 0.0f, 1.0f);
    m_EffectiveAttackRangeCacheSeconds = std::clamp(getFloat("EffectiveAttackRangeCacheSeconds", m_EffectiveAttackRangeCacheSeconds), 0.0f, 1.0f);
    m_EffectiveAttackRangeCacheDistanceTolerance = std::clamp(getFloat("EffectiveAttackRangeCacheDistanceTolerance", m_EffectiveAttackRangeCacheDistanceTolerance), 0.0f, 256.0f);
    m_EffectiveAttackRangeCacheSpreadTolerance = std::clamp(getFloat("EffectiveAttackRangeCacheSpreadTolerance", m_EffectiveAttackRangeCacheSpreadTolerance), 0.0f, 10.0f);
    m_EffectiveAttackRangeCacheDirectionDot = std::clamp(getFloat("EffectiveAttackRangeCacheDirectionDot", m_EffectiveAttackRangeCacheDirectionDot), 0.0f, 1.0f);
    m_EffectiveAttackRangeMeleeDistance = std::clamp(getFloat("EffectiveAttackRangeMeleeDistance", m_EffectiveAttackRangeMeleeDistance), 1.0f, 512.0f);
    m_EffectiveAttackRangeMeleeFanAngle = std::clamp(getFloat("EffectiveAttackRangeMeleeFanAngle", m_EffectiveAttackRangeMeleeFanAngle), 0.0f, 180.0f);
    m_EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds = std::clamp(getFloat("EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds", m_EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds), 0.05f, 5.0f);
    m_EffectiveAttackRangeHitPointTolerance = std::clamp(getFloat("EffectiveAttackRangeHitPointTolerance", m_EffectiveAttackRangeHitPointTolerance), 0.0f, 64.0f);
    m_EffectiveAttackRangeHitPointSpreadScale = std::clamp(getFloat("EffectiveAttackRangeHitPointSpreadScale", m_EffectiveAttackRangeHitPointSpreadScale), 0.0f, 2.0f);
    m_EffectiveAttackRangeHitPointMaxTolerance = std::clamp(getFloat("EffectiveAttackRangeHitPointMaxTolerance", m_EffectiveAttackRangeHitPointMaxTolerance), m_EffectiveAttackRangeHitPointTolerance, 64.0f);
    m_D3DAimLineOverlayEnabled = getBool("D3DAimLineOverlayEnabled", m_D3DAimLineOverlayEnabled);
    m_D3DAimLineOverlaySyncAimLineColor = getBool("D3DAimLineOverlaySyncAimLineColor", m_D3DAimLineOverlaySyncAimLineColor);
    m_D3DAimLineOverlayWidthPixels = std::clamp(getFloat("D3DAimLineOverlayWidthPixels", m_D3DAimLineOverlayWidthPixels), 0.0f, 64.0f);
    m_D3DAimLineOverlayOutlinePixels = std::clamp(getFloat("D3DAimLineOverlayOutlinePixels", m_D3DAimLineOverlayOutlinePixels), 0.0f, 64.0f);
    m_D3DAimLineOverlayEndpointPixels = std::clamp(getFloat("D3DAimLineOverlayEndpointPixels", m_D3DAimLineOverlayEndpointPixels), 0.0f, 128.0f);
    auto d3dAimColor = getColor("D3DAimLineOverlayColor",
        m_D3DAimLineOverlayColorR, m_D3DAimLineOverlayColorG, m_D3DAimLineOverlayColorB, m_D3DAimLineOverlayColorA);
    m_D3DAimLineOverlayColorR = d3dAimColor[0];
    m_D3DAimLineOverlayColorG = d3dAimColor[1];
    m_D3DAimLineOverlayColorB = d3dAimColor[2];
    m_D3DAimLineOverlayColorA = d3dAimColor[3];
    auto d3dAimOutlineColor = getColor("D3DAimLineOverlayOutlineColor",
        m_D3DAimLineOverlayOutlineColorR, m_D3DAimLineOverlayOutlineColorG, m_D3DAimLineOverlayOutlineColorB, m_D3DAimLineOverlayOutlineColorA);
    m_D3DAimLineOverlayOutlineColorR = d3dAimOutlineColor[0];
    m_D3DAimLineOverlayOutlineColorG = d3dAimOutlineColor[1];
    m_D3DAimLineOverlayOutlineColorB = d3dAimOutlineColor[2];
    m_D3DAimLineOverlayOutlineColorA = d3dAimOutlineColor[3];
    m_AimLinePersistence = std::max(0.0f, getFloat("AimLinePersistence", m_AimLinePersistence));
    m_AimLineFrameDurationMultiplier = std::max(0.0f, getFloat("AimLineFrameDurationMultiplier", m_AimLineFrameDurationMultiplier));
    m_AimLineMaxHz = GetHmdDisplayFrequencyHz();
    m_GameLaserSightBeamEnabled = getBool("GameLaserSightBeamEnabled", m_GameLaserSightBeamEnabled);
    m_GameLaserSightReplaceParticle = getBool("GameLaserSightReplaceParticle", m_GameLaserSightReplaceParticle);
    m_GameLaserSightThickness = std::clamp(getFloat("GameLaserSightThickness", m_GameLaserSightThickness), 0.0f, 8.0f);
    auto gameLaserColor = getColor("GameLaserSightColor", m_GameLaserSightColorR, m_GameLaserSightColorG, m_GameLaserSightColorB, m_GameLaserSightColorA);
    m_GameLaserSightColorR = gameLaserColor[0];
    m_GameLaserSightColorG = gameLaserColor[1];
    m_GameLaserSightColorB = gameLaserColor[2];
    m_GameLaserSightColorA = gameLaserColor[3];
    m_GameLaserSightEndOffset = getVector3("GameLaserSightEndOffset", m_GameLaserSightEndOffset);
    m_GameLaserSightEndOffset.x = std::clamp(m_GameLaserSightEndOffset.x, -256.0f, 256.0f);
    m_GameLaserSightEndOffset.y = std::clamp(m_GameLaserSightEndOffset.y, -256.0f, 256.0f);
    m_GameLaserSightEndOffset.z = std::clamp(m_GameLaserSightEndOffset.z, -256.0f, 256.0f);
    m_ThrowArcLandingOffset = std::max(-10000.0f, std::min(10000.0f, getFloat("ThrowArcLandingOffset", m_ThrowArcLandingOffset)));
    m_ThrowArcMaxHz = GetHmdDisplayFrequencyHz();
    // Debug / memory
    const bool prevVASLog = m_DebugVASLog;
    m_DebugVASLog = getBool("DebugVASLog", m_DebugVASLog);
    m_LazyScopeRearMirrorRTT = getBool("LazyScopeRearMirrorRTT", m_LazyScopeRearMirrorRTT);
    if (!prevVASLog && m_DebugVASLog)
        LogVAS("DebugVASLog enabled");
    const bool prevAutoMatQueueMode = m_AutoMatQueueMode;
    m_AutoMatQueueMode = getBool("AutoMatQueueMode", m_AutoMatQueueMode);
    if (m_AutoMatQueueMode != prevAutoMatQueueMode)
    {
        m_AutoMatQueueModeLastRequested = -999;
        m_AutoMatQueueModeLastCmdTime = {};
        m_MenuFpsMaxSent = false;
        m_MenuFpsMaxLastHz = 0;
    }

    const bool prevVrRecommendedVideoSettingsEnabled = m_VrRecommendedVideoSettingsEnabled;
    m_VrRecommendedVideoSettingsEnabled =
        getBool("VrRecommendedVideoSettingsEnabled", m_VrRecommendedVideoSettingsEnabled);
    if (m_VrRecommendedVideoSettingsEnabled)
    {
        if (!prevVrRecommendedVideoSettingsEnabled || !m_VrRecommendedVideoSettingsAppliedThisSession)
            m_VrRecommendedVideoSettingsApplyPending = true;
    }
    else
    {
        m_VrRecommendedVideoSettingsApplyPending = false;
        m_VrRecommendedVideoSettingsAppliedThisSession = false;
    }

    // Multicore rendering: explicit minimum render-thread wait budget (ms) for a fresher
    // WaitGetPoses() snapshot in queued mode. 0 disables fixed waiting, but the render hook can
    // still auto-wait during real HMD motion to avoid reusing the same pose sample.
    // 1~3 = light wait, 5+ = prioritize stability, -1 = strong sync (wait up to ~50ms).
    m_QueuedRenderPoseWaitMs = std::clamp(getInt("QueuedRenderPoseWaitMs", m_QueuedRenderPoseWaitMs), -1, 20);
    // 0 = fully strict pose-token submit. Higher values periodically bypass the strict fresh-pose
    // wait and allow a new rendered frame from the same WaitGetPoses() snapshot to submit.
    m_QueuedRenderPoseRelaxPercent = std::clamp(getInt("QueuedRenderPoseRelaxPercent", m_QueuedRenderPoseRelaxPercent), 0, 100);
    m_QueuedRenderPoseFromTracking = getBool("QueuedRenderPoseFromTracking", m_QueuedRenderPoseFromTracking);
    // Multicore rendering: present-side wait budget (ms) for a fresh dRenderView frame before submit.
    // 0 = no wait (max FPS, can increase stale-frame submits), 1~3 = usually best balance.
    m_QueuedSubmitWaitMs = std::clamp(getInt("QueuedSubmitWaitMs", m_QueuedSubmitWaitMs), 0, 20);
    // true = only submit frames whose render-completed pose advanced (less ghosting, can skip frames).
    // false = original submit-pose-token route (smoother cadence, can submit stale render-pose frames).
    m_QueuedSubmitUseRenderPoseToken = getBool("QueuedSubmitUseRenderPoseToken", m_QueuedSubmitUseRenderPoseToken);

    // Queued rendering: optional render-thread FPS cap as % of HMD refresh.
    // 0 = unlimited, 100 = match HMD refresh.
    m_QueuedRenderMaxFps = std::clamp(getInt("QueuedRenderMaxFps", m_QueuedRenderMaxFps), 0, 240);
    // Queued rendering: smart FPS cap engagement. When enabled, the FPS cap is only applied when
    // the render thread is detected to be outrunning pose updates during body motion or HMD motion.
    m_QueuedRenderMaxFpsSmart = getBool("QueuedRenderMaxFpsSmart", m_QueuedRenderMaxFpsSmart);
    // Queued rendering: limit how many extra render frames may reuse the same WaitGetPoses() snapshot.
    // -1 = disabled, 0 = never reuse (most stable), 1 = allow 1 reuse (2 frames per pose), etc.
    m_QueuedRenderMaxFramesAhead = std::clamp(getInt("QueuedRenderMaxFramesAhead", m_QueuedRenderMaxFramesAhead), -1, 6);

    // Queued rendering: render-thread smoothing time constant (ms) for cameraAnchor/rotationOffset.
    // 0 = off, 20~80 typical, higher = smoother but more latency.
    m_QueuedRenderViewSmoothMs = std::clamp(getInt("QueuedRenderViewSmoothMs", m_QueuedRenderViewSmoothMs), 0, 250);
    // First-person stair/step smoothing: damp the engine's vertical step camera before it drives
    // the VR world anchor. 0 disables; higher = steadier stairs but slower vertical catch-up.
    m_StairStepCameraSmoothMs = std::clamp(getInt("StairStepCameraSmoothMs", m_StairStepCameraSmoothMs), 0, 300);

    // Queued rendering: HMD pose smoothing time constant (ms) for visual stability.
    // 0 = off. Higher values can soften stale-pose stepping a bit, but they do not produce fresher
    // poses, so they are not a full fix for queued-render ghosting during real head motion.
    m_QueuedRenderHmdSmoothMs = std::clamp(getInt("QueuedRenderHmdSmoothMs", m_QueuedRenderHmdSmoothMs), 0, 250);
    // Queued rendering: feed HMD yaw through the same rotationOffset path used by thumbstick turns.
    m_QueuedRenderHmdYawUsesTurnPath = getBool("QueuedRenderHmdYawUsesTurnPath", m_QueuedRenderHmdYawUsesTurnPath);

    // Queued rendering: stabilize first-person viewmodel (disable engine bob/lag in queued mode).
    m_QueuedViewmodelStabilize = getBool("QueuedViewmodelStabilize", m_QueuedViewmodelStabilize);
    // Global: hard-lock first-person viewmodel pose after engine calc (all queue modes).
    m_ViewmodelDisableMoveBob = getBool("ViewmodelDisableMoveBob", m_ViewmodelDisableMoveBob);
    m_EyeRenderTargetMatchProjectionAspect = getBool("EyeRenderTargetMatchProjectionAspect", m_EyeRenderTargetMatchProjectionAspect);
    m_QueuedViewmodelStabilizeDebugLog = getBool("QueuedViewmodelStabilizeDebugLog", m_QueuedViewmodelStabilizeDebugLog);
    m_QueuedViewmodelStabilizeDebugLogHz = std::max(0.0f, getFloat("QueuedViewmodelStabilizeDebugLogHz", m_QueuedViewmodelStabilizeDebugLogHz));
    m_RenderPipelineDebugLog = getBool("RenderPipelineDebugLog", m_RenderPipelineDebugLog);
    m_RenderPipelineDebugLogHz = std::clamp(getFloat("RenderPipelineDebugLogHz", m_RenderPipelineDebugLogHz), 0.0f, 60.0f);
    m_QueuedSourceMarkerDebugLog = getBool("QueuedSourceMarkerDebugLog", m_QueuedSourceMarkerDebugLog);
    m_QueuedSourceMarkerDebugLogHz = std::clamp(getFloat("QueuedSourceMarkerDebugLogHz", m_QueuedSourceMarkerDebugLogHz), 0.0f, 60.0f);
    m_RightEyeCopyFromLeft = getBool("RightEyeCopyFromLeft", m_RightEyeCopyFromLeft);

    // ReShade compatibility: ReShade's D3D9 runtime can leave the device/backbuffer state in
    // a state that is valid for flat desktop Present but invalid for our per-eye VR RT chain.
    // This enables a guarded path around each eye render and avoids submitting VR after a
    // post-Present idle wait that includes ReShade's desktop backbuffer work.
    const bool oldReShadeVRCompat = m_ReShadeVRCompat;
    m_ReShadeVRCompat = getBool("ReShadeVRCompat", m_ReShadeVRCompat);
    // ReShade injects D3D work outside Source's known producer windows and therefore
    // retains the conservative all-call lock. Normal queued rendering uses DXVK's
    // scoped exclusive transactions instead of locking every draw call.
    L4D2VR_D3D9_SetForceDeviceLock(m_ReShadeVRCompat ? 1 : 0);
    if (oldReShadeVRCompat != m_ReShadeVRCompat)
    {
        m_ReShadeVRCompatResolvedFrameId.store(0, std::memory_order_release);
        m_ReShadeVRCompatPendingRenderReady.store(0, std::memory_order_release);
        m_ReShadeVRCompatPendingRenderPoseToken.store(0, std::memory_order_release);
        m_ReShadeVRCompatPendingRenderFrameSeq.store(0, std::memory_order_release);
        m_ReShadeVRCompatPendingDuplicatePose.store(0, std::memory_order_release);
        m_RenderCompletedDuplicatePoseFrameId.store(0, std::memory_order_release);
    }
    if (m_Compositor && oldReShadeVRCompat != m_ReShadeVRCompat)
        ConfigureExplicitTiming();

    // Bullet FX alignment: fine-tune client-side tracer/impact visuals.
    // Units: meters in aim-ray space (X=forward, Y=right, Z=up). Visual-only.
    const bool hasBulletFxOff = (userConfig.find("BulletVisualHitOffset") != userConfig.end());
    const bool hasQueuedFxOff = (userConfig.find("QueuedBulletVisualHitOffset") != userConfig.end());

    m_BulletVisualHitOffset = getVector3("BulletVisualHitOffset", m_BulletVisualHitOffset);
    m_BulletVisualHitOffset.x = std::clamp(m_BulletVisualHitOffset.x, -1.0f, 1.0f);
    m_BulletVisualHitOffset.y = std::clamp(m_BulletVisualHitOffset.y, -1.0f, 1.0f);
    m_BulletVisualHitOffset.z = std::clamp(m_BulletVisualHitOffset.z, -1.0f, 1.0f);

    // Additional correction only when queued rendering is enabled (mat_queue_mode!=0).
    m_QueuedBulletVisualHitOffset = getVector3("QueuedBulletVisualHitOffset", m_QueuedBulletVisualHitOffset);
    m_QueuedBulletVisualHitOffset.x = std::clamp(m_QueuedBulletVisualHitOffset.x, -1.0f, 1.0f);
    m_QueuedBulletVisualHitOffset.y = std::clamp(m_QueuedBulletVisualHitOffset.y, -1.0f, 1.0f);
    m_QueuedBulletVisualHitOffset.z = std::clamp(m_QueuedBulletVisualHitOffset.z, -1.0f, 1.0f);
    m_BulletVisualsUseMuzzleSmoke = getBool("BulletVisualsUseMuzzleSmoke", m_BulletVisualsUseMuzzleSmoke);
    m_BulletVisualsUseViewmodelPose = getBool("BulletVisualsUseViewmodelPose", m_BulletVisualsUseViewmodelPose);

    // Backward compatibility: older configs used only QueuedBulletVisualHitOffset.
    // If BulletVisualHitOffset is not set but QueuedBulletVisualHitOffset is set,
    // treat the queued value as the base offset (applies in both single/queued render).
    if (!hasBulletFxOff && hasQueuedFxOff)
    {
        m_BulletVisualHitOffset = m_QueuedBulletVisualHitOffset;
        m_QueuedBulletVisualHitOffset = { 0.0f, 0.0f, 0.0f };
    }

    if (hasBulletFxOff)
    {
    }
    if (hasQueuedFxOff)
    {
    }

    // Gun-mounted scope
    m_ScopeEnabled = getBool("ScopeEnabled", m_ScopeEnabled);
    m_ScopeRTTSize = std::clamp(getInt("ScopeRTTSize", m_ScopeRTTSize), 128, 4096);
    m_DefaultScopeAdjust.fov = std::clamp(getFloat("ScopeDefaultFov", m_DefaultScopeAdjust.fov), 1.0f, 179.0f);
    {
        const auto fovRange = getFloatList("ScopeMagnificationFovRange", "3,20");
        if (fovRange.size() >= 2 && std::isfinite(fovRange[0]) && std::isfinite(fovRange[1]))
        {
            m_ScopeFovMin = std::clamp(std::min(fovRange[0], fovRange[1]), 1.0f, 179.0f);
            m_ScopeFovMax = std::clamp(std::max(fovRange[0], fovRange[1]), 1.0f, 179.0f);
        }
        m_DefaultScopeAdjust.fov = std::clamp(m_DefaultScopeAdjust.fov, m_ScopeFovMin, m_ScopeFovMax);
    }
    m_ScopeFovAdjustSpeed = std::clamp(getFloat("ScopeMagnificationAdjustSpeed", m_ScopeFovAdjustSpeed), 0.0f, 180.0f);
    {
        const auto sizeRange = getFloatList("ScopeSizeRange", "0.03,0.30");
        if (sizeRange.size() >= 2 && std::isfinite(sizeRange[0]) && std::isfinite(sizeRange[1]))
        {
            m_ScopeSizeMin = std::clamp(std::min(sizeRange[0], sizeRange[1]), 0.001f, 5.0f);
            m_ScopeSizeMax = std::clamp(std::max(sizeRange[0], sizeRange[1]), 0.001f, 5.0f);
            if (m_ScopeSizeMax < m_ScopeSizeMin)
                m_ScopeSizeMax = m_ScopeSizeMin;
        }
    }
    m_ScopeSizeAdjustSpeed = std::clamp(getFloat("ScopeSizeAdjustSpeed", m_ScopeSizeAdjustSpeed), 0.0f, 5.0f);
    m_ScopeOffsetAdjustMoveSpeed = std::clamp(getFloat("ScopeOffsetAdjustMoveSpeed", m_ScopeOffsetAdjustMoveSpeed), 0.1f, 5.0f);
    m_ScopeAimSensitivityFovReductionRate = std::clamp(getFloat("ScopeAimSensitivityFovReductionRate", m_ScopeAimSensitivityFovReductionRate), 0.0f, 4.0f);
    m_ScopeZNear = std::clamp(getFloat("ScopeZNear", m_ScopeZNear), 0.1f, 64.0f);

    m_ScopeCameraOffset = getVector3("ScopeCameraOffset", m_ScopeCameraOffset);
    { Vector tmp = getVector3("ScopeCameraAngleOffset", Vector{ m_ScopeCameraAngleOffset.x, m_ScopeCameraAngleOffset.y, m_ScopeCameraAngleOffset.z }); m_ScopeCameraAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z }; }

    m_DefaultScopeAdjust.widthMeters = std::clamp(getFloat("ScopeDefaultOverlayWidthMeters", m_DefaultScopeAdjust.widthMeters), m_ScopeSizeMin, m_ScopeSizeMax);
    m_DefaultScopeAdjust.overlayOffset = getVector3("ScopeDefaultOverlayOffset", m_DefaultScopeAdjust.overlayOffset);
    m_ScopeFov = m_DefaultScopeAdjust.fov;
    m_ScopeOverlayWidthMeters = m_DefaultScopeAdjust.widthMeters;
    m_ScopeOverlayXOffset = m_DefaultScopeAdjust.overlayOffset.x;
    m_ScopeOverlayYOffset = m_DefaultScopeAdjust.overlayOffset.y;
    m_ScopeOverlayZOffset = m_DefaultScopeAdjust.overlayOffset.z;
    { Vector tmp = getVector3("ScopeOverlayAngleOffset", Vector{ m_ScopeOverlayAngleOffset.x, m_ScopeOverlayAngleOffset.y, m_ScopeOverlayAngleOffset.z }); m_ScopeOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z }; }
    m_ScopeAimLineOnlyInScope = getBool("ScopeAimLineOnlyInScope", m_ScopeAimLineOnlyInScope);
    m_ScopeReticleAlpha = std::clamp(getFloat("ScopeReticleAlpha", m_ScopeReticleAlpha), 0.0f, 1.0f);
    m_ScopeHideLocalPlayerModelInScope = getBool("ScopeHideLocalPlayerModelInScope", m_ScopeHideLocalPlayerModelInScope);

    m_ScopeOverlayAlwaysVisible = getBool("ScopeOverlayAlwaysVisible", m_ScopeOverlayAlwaysVisible);
    m_ScopeOverlayIdleAlpha = std::clamp(getFloat("ScopeOverlayIdleAlpha", m_ScopeOverlayIdleAlpha), 0.0f, 1.0f);
    m_ScopeRequireLookThrough = getBool("ScopeRequireLookThrough", m_ScopeRequireLookThrough);
    m_ScopeLookThroughDistanceMeters = std::clamp(getFloat("ScopeLookThroughDistanceMeters", m_ScopeLookThroughDistanceMeters), 0.01f, 2.0f);
    m_ScopeLookThroughAngleDeg = std::clamp(getFloat("ScopeLookThroughAngleDeg", m_ScopeLookThroughAngleDeg), 1.0f, 89.0f);

    // Scope stabilization (visual only)
    m_ScopeStabilizationEnabled = getBool("ScopeStabilizationEnabled", m_ScopeStabilizationEnabled);
    m_ScopeStabilizationMinCutoff = std::clamp(getFloat("ScopeStabilizationMinCutoff", m_ScopeStabilizationMinCutoff), 0.05f, 30.0f);
    m_ScopeStabilizationBeta = std::clamp(getFloat("ScopeStabilizationBeta", m_ScopeStabilizationBeta), 0.0f, 5.0f);
    m_ScopeStabilizationDCutoff = std::clamp(getFloat("ScopeStabilizationDCutoff", m_ScopeStabilizationDCutoff), 0.05f, 30.0f);
    if (!m_ScopeStabilizationEnabled)
    {
        m_ScopeStabilizationInit = false;
        m_ScopeStabilizationLastTime = {};
    }

    // Rear mirror
    m_RearMirrorEnabled = getBool("RearMirrorEnabled", m_RearMirrorEnabled);
    m_DesktopRearMirrorWindowEnabled = getBool("DesktopRearMirrorWindowEnabled", m_DesktopRearMirrorWindowEnabled);
    m_RearMirrorShowOnlyOnSpecialWarning = getBool("RearMirrorShowOnlyOnSpecialWarning", m_RearMirrorShowOnlyOnSpecialWarning);
    m_RearMirrorSpecialShowHoldSeconds = std::max(0.0f, getFloat("RearMirrorSpecialShowHoldSeconds", m_RearMirrorSpecialShowHoldSeconds));
    m_RearMirrorRTTSize = std::clamp(getInt("RearMirrorRTTSize", m_RearMirrorRTTSize), 128, 4096);
    m_RearMirrorRTTMaxHz = std::clamp(getFloat("RearMirrorRTTMaxHz", m_RearMirrorRTTMaxHz), 0.0f, 240.0f);
    m_RearMirrorFov = std::clamp(getFloat("RearMirrorFov", m_RearMirrorFov), 1.0f, 179.0f);
    m_RearMirrorZNear = std::clamp(getFloat("RearMirrorZNear", m_RearMirrorZNear), 0.1f, 64.0f);

    m_RearMirrorCameraOffset = getVector3("RearMirrorCameraOffset", m_RearMirrorCameraOffset);
    {
        Vector tmp = getVector3(
            "RearMirrorCameraAngleOffset",
            Vector{ m_RearMirrorCameraAngleOffset.x, m_RearMirrorCameraAngleOffset.y, m_RearMirrorCameraAngleOffset.z });
        m_RearMirrorCameraAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }

    m_RearMirrorOverlayWidthMeters = std::max(0.01f, getFloat("RearMirrorOverlayWidthMeters", m_RearMirrorOverlayWidthMeters));
    m_RearMirrorOverlayXOffset = getFloat("RearMirrorOverlayXOffset", m_RearMirrorOverlayXOffset);
    m_RearMirrorOverlayYOffset = getFloat("RearMirrorOverlayYOffset", m_RearMirrorOverlayYOffset);
    m_RearMirrorOverlayZOffset = getFloat("RearMirrorOverlayZOffset", m_RearMirrorOverlayZOffset);
    {
        Vector tmp = getVector3(
            "RearMirrorOverlayAngleOffset",
            Vector{ m_RearMirrorOverlayAngleOffset.x, m_RearMirrorOverlayAngleOffset.y, m_RearMirrorOverlayAngleOffset.z });
        m_RearMirrorOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }
    m_RearMirrorAlpha = std::clamp(getFloat("RearMirrorAlpha", m_RearMirrorAlpha), 0.0f, 1.0f);
    m_RearMirrorFlipHorizontal = getBool("RearMirrorFlipHorizontal", m_RearMirrorFlipHorizontal);

    // Hide the rear mirror when the aim line/ray intersects it (prevents blocking your view while aiming).
    m_RearMirrorHideWhenAimLineHits = getBool("RearMirrorHideWhenAimLineHits", m_RearMirrorHideWhenAimLineHits);
    m_RearMirrorAimLineHideHoldSeconds = std::clamp(getFloat("RearMirrorAimLineHideHoldSeconds", m_RearMirrorAimLineHideHoldSeconds), 0.0f, 1.0f);
    if (!m_RearMirrorHideWhenAimLineHits)
        m_RearMirrorAimLineHideUntil = {};

    // Rear mirror hint: enlarge overlay when special-infected arrows are visible in the mirror render pass
    m_RearMirrorSpecialWarningDistance = std::max(0.0f, getFloat("RearMirrorSpecialWarningDistance", m_RearMirrorSpecialWarningDistance));
    if (m_RearMirrorSpecialWarningDistance <= 0.0f)
        m_RearMirrorSpecialEnlargeActive = false;

    // Auto reset tracking origin after a level loads (0 disables)
    m_AutoResetPositionAfterLoadSeconds = std::clamp(
        getFloat("AutoResetPositionAfterLoadSeconds", m_AutoResetPositionAfterLoadSeconds),
        0.0f, 60.0f);

    // Damage feedback overall amplitude scale (0..1).
    // Applies after profile/merge/damage scaling so you can globally tame or mute hurt haptics
    // without re-tuning each individual damage profile.
    m_DamageFeedbackOverallScale = std::clamp(
        getFloat("DamageFeedbackOverallScale", m_DamageFeedbackOverallScale),
        0.0f, 1.0f);

    m_ConfigForceNonVRServerMovement = getBool("ForceNonVRServerMovement", m_ConfigForceNonVRServerMovement);
    m_ForceNonVRServerMovement = m_ConfigForceNonVRServerMovement || m_ServerHookFallbackForcedNonVRServerMovement;

    // Non-VR server movement: make client-side bullet/muzzle effects originate from controller (visual-only).
    m_NonVRServerMovementEffectsFromController = getBool("NonVRServerMovementEffectsFromController", m_NonVRServerMovementEffectsFromController);
    m_NonVRServerMovementEffectsDebugLog = getBool("NonVRServerMovementEffectsDebugLog", m_NonVRServerMovementEffectsDebugLog);
    m_NonVRServerMovementEffectsDebugLogHz = std::max(0.0f, getFloat("NonVRServerMovementEffectsDebugLogHz", m_NonVRServerMovementEffectsDebugLogHz));
    m_Roomscale1To1Movement = getBool("Roomscale1To1Movement", m_Roomscale1To1Movement);
    m_Roomscale1To1MaxStepMeters = getFloat("Roomscale1To1MaxStepMeters", m_Roomscale1To1MaxStepMeters);
    m_TeleportMaxDistanceMeters = std::clamp(getFloat("TeleportMaxDistanceMeters", m_TeleportMaxDistanceMeters), 0.25f, 50.0f);
    m_TeleportVisualScoutOnNonVRServerEnabled = getBool("TeleportVisualScoutOnNonVRServerEnabled", m_TeleportVisualScoutOnNonVRServerEnabled);
    m_Roomscale1To1DebugLog = getBool("Roomscale1To1DebugLog", m_Roomscale1To1DebugLog);
    m_Roomscale1To1DebugLogHz = getFloat("Roomscale1To1DebugLogHz", m_Roomscale1To1DebugLogHz);

    // 1:1 roomscale cmd movement / camera decoupling
    m_Roomscale1To1DecoupleCamera = getBool("Roomscale1To1DecoupleCamera", m_Roomscale1To1DecoupleCamera);
    m_Roomscale1To1DisableWhileThumbstick = getBool("Roomscale1To1DisableWhileThumbstick", m_Roomscale1To1DisableWhileThumbstick);
    m_Roomscale1To1ServerMove = getBool("Roomscale1To1ServerMove", m_Roomscale1To1ServerMove);
    m_Roomscale1To1MovementScale = std::clamp(getFloat("Roomscale1To1MovementScale", m_Roomscale1To1MovementScale), 0.0f, 4.0f);
    m_Roomscale1To1MinApplyMeters = std::max(0.0f, getFloat("Roomscale1To1MinApplyMeters", m_Roomscale1To1MinApplyMeters));
    m_Roomscale1To1PhysicalCrouch = getBool("Roomscale1To1PhysicalCrouch", m_Roomscale1To1PhysicalCrouch);
    m_Roomscale1To1CrouchEnterMeters = std::clamp(getFloat("Roomscale1To1CrouchEnterMeters", m_Roomscale1To1CrouchEnterMeters), 0.05f, 1.0f);
    m_Roomscale1To1CrouchExitMeters = std::clamp(getFloat("Roomscale1To1CrouchExitMeters", m_Roomscale1To1CrouchExitMeters), 0.0f, m_Roomscale1To1CrouchEnterMeters);

    // Mouse mode (desktop-style aiming while staying in VR rendering)
    m_MouseModeEnabled = getBool("MouseModeEnabled", m_MouseModeEnabled);
    m_SystemMouseInputSuppressAfterMapLoad = getBool("SystemMouseInputSuppressAfterMapLoad", m_SystemMouseInputSuppressAfterMapLoad);
    m_MouseModeAimFromHmd = getBool("MouseModeAimFromHmd", m_MouseModeAimFromHmd);
    m_MouseModeHmdAimSensitivity = std::clamp(getFloat("MouseModeHmdAimSensitivity", m_MouseModeHmdAimSensitivity), 0.0f, 3.0f);
    m_MouseModeYawSensitivity = getFloat("MouseModeYawSensitivity", m_MouseModeYawSensitivity);
    m_MouseModePitchSensitivity = getFloat("MouseModePitchSensitivity", m_MouseModePitchSensitivity);
    m_MouseModePitchAffectsView = getBool("MouseModePitchAffectsView", m_MouseModePitchAffectsView);
    m_MouseModeTurnSmoothing = getFloat("MouseModeTurnSmoothing", m_MouseModeTurnSmoothing);
    m_MouseModePitchSmoothing = getFloat("MouseModePitchSmoothing", m_MouseModePitchSmoothing);
    m_MouseModeViewmodelAnchorOffset = getVector3("MouseModeViewmodelAnchorOffset", m_MouseModeViewmodelAnchorOffset);
    m_MouseModeScopedViewmodelAnchorOffset = getVector3("MouseModeScopedViewmodelAnchorOffset", m_MouseModeViewmodelAnchorOffset);
    // Mouse-mode: if non-zero, place the scope overlay using the OpenVR HMD tracking pose
    // (meters in tracking space), so the overlay can't disappear due to unit mismatches.
    m_MouseModeScopeOverlayOffset = getVector3("MouseModeScopeOverlayOffset", m_MouseModeScopeOverlayOffset);
    m_MouseModeScopeOverlayAngleOffsetSet = (userConfig.find("MouseModeScopeOverlayAngleOffset") != userConfig.end());
    if (m_MouseModeScopeOverlayAngleOffsetSet)
    {
        Vector tmp = getVector3("MouseModeScopeOverlayAngleOffset", Vector{ m_MouseModeScopeOverlayAngleOffset.x, m_MouseModeScopeOverlayAngleOffset.y, m_MouseModeScopeOverlayAngleOffset.z });
        m_MouseModeScopeOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }
    m_MouseModeScopeToggleKey = parseVirtualKey(getString("MouseModeScopeToggleKey", "key:q"));
    m_MouseModeAimConvergeDistance = getFloat("MouseModeAimConvergeDistance", m_MouseModeAimConvergeDistance);

    m_NonVRMeleeSwingThreshold = std::max(0.0f, getFloat("NonVRMeleeSwingThreshold", m_NonVRMeleeSwingThreshold));
    m_NonVRMeleeSwingCooldown = std::max(0.0f, getFloat("NonVRMeleeSwingCooldown", m_NonVRMeleeSwingCooldown));
    m_NonVRMeleeHoldTime = std::max(0.0f, getFloat("NonVRMeleeHoldTime", m_NonVRMeleeHoldTime));
    m_SnapTurning = getBool("SnapTurning", m_SnapTurning);
    m_NonVRMeleeAttackDelay = std::max(0.0f, getFloat("NonVRMeleeAttackDelay", m_NonVRMeleeAttackDelay));
    m_NonVRMeleeAimLockTime = std::max(0.0f, getFloat("NonVRMeleeAimLockTime", m_NonVRMeleeAimLockTime));
    m_NonVRMeleeHysteresis = std::clamp(getFloat("NonVRMeleeHysteresis", m_NonVRMeleeHysteresis), 0.1f, 0.95f);
    m_NonVRMeleeAngVelThreshold = std::max(0.0f, getFloat("NonVRMeleeAngVelThreshold", m_NonVRMeleeAngVelThreshold));
    m_NonVRMeleeSwingDirBlend = std::clamp(getFloat("NonVRMeleeSwingDirBlend", m_NonVRMeleeSwingDirBlend), 0.0f, 1.0f);
    m_RequireSecondaryAttackForItemSwitch = getBool("RequireSecondaryAttackForItemSwitch", m_RequireSecondaryAttackForItemSwitch);
    m_SpecialInfectedWarningActionEnabled = getBool("SpecialInfectedAutoEvade", m_SpecialInfectedWarningActionEnabled);
    m_SpecialInfectedAutoEvadeIgnoreBehind = getBool("SpecialInfectedAutoEvadeIgnoreBehind", m_SpecialInfectedAutoEvadeIgnoreBehind);
    m_SpecialInfectedDodgeActive = getBool("SpecialInfectedDodgeEnabled", m_SpecialInfectedDodgeActive);
    m_SpecialInfectedDodgeDistance = std::max(0.0f, getFloat("SpecialInfectedDodgeDistance", m_SpecialInfectedDodgeDistance));
    if (!m_SpecialInfectedDodgeActive || m_SpecialInfectedDodgeDistance <= 0.0f)
    {
        if (m_SpecialInfectedDodgeDistance <= 0.0f)
            m_SpecialInfectedDodgeActive = false;
        std::lock_guard<std::mutex> lock(m_SpecialInfectedDodgeMutex);
        m_SpecialInfectedDodgeThreats.clear();
        m_LastSpecialInfectedDodgeScanTime = {};
    }
    m_SpecialInfectedArrowEnabled = getBool("SpecialInfectedArrowEnabled", m_SpecialInfectedArrowEnabled);
    m_SpecialInfectedDebug = getBool("SpecialInfectedDebug", m_SpecialInfectedDebug);
    m_SpecialInfectedArrowDebugLog = getBool("SpecialInfectedArrowDebugLog", m_SpecialInfectedArrowDebugLog);
    m_SpecialInfectedArrowDebugLogHz = std::clamp(getFloat("SpecialInfectedArrowDebugLogHz", m_SpecialInfectedArrowDebugLogHz), 0.0f, 20.0f);
    m_ItemModelLabelEnabled = getBool("ItemModelLabelEnabled", m_ItemModelLabelEnabled);
    m_ItemModelLabelShowWeapons = getBool("ItemModelLabelShowWeapons", m_ItemModelLabelShowWeapons);
    m_ItemModelLabelShowThrowables = getBool("ItemModelLabelShowThrowables", m_ItemModelLabelShowThrowables);
    m_ItemModelLabelShowMedical = getBool("ItemModelLabelShowMedical", m_ItemModelLabelShowMedical);
    m_ItemModelLabelDebugLog = getBool("ItemModelLabelDebugLog", m_ItemModelLabelDebugLog);
    m_ItemModelLabelBlacklist.clear();
    for (std::string token : getStringList("ItemModelLabelBlacklist"))
    {
        std::transform(token.begin(), token.end(), token.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!token.empty())
            m_ItemModelLabelBlacklist.insert(token);
    }
    // Drop stale projected labels when config is reloaded so newly blacklisted text disappears immediately.
    m_ProjectedItemLabels.clear();
    m_LastItemModelLabelTime.clear();
    ClearQueuedProjectedItemLabels();
    m_ItemModelLabelMaxHz = std::max(0.0f, getFloat("ItemModelLabelMaxHz", m_ItemModelLabelMaxHz));
    m_ItemModelLabelScanHz = std::clamp(getFloat("ItemModelLabelScanHz", m_ItemModelLabelScanHz), 1.0f, 60.0f);
    m_ItemModelLabelTextScale = std::clamp(getFloat("ItemModelLabelTextScale", m_ItemModelLabelTextScale), 0.25f, 4.0f);
    m_ItemModelLabelMaxDistance = std::max(0.0f, getFloat("ItemModelLabelMaxDistance", m_ItemModelLabelMaxDistance));
    m_ItemModelLabelMaxVisiblePerEye = std::clamp(getInt("ItemModelLabelMaxVisiblePerEye", m_ItemModelLabelMaxVisiblePerEye), 1, 64);
    m_DesktopMirrorEnabled = getBool("DesktopMirrorEnabled", m_DesktopMirrorEnabled);
    {
        std::string eye = getString("DesktopMirrorEye", m_DesktopMirrorEye == 0 ? "left" : "right");
        std::transform(eye.begin(), eye.end(), eye.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
            });
        if (eye == "left" || eye == "0")
            m_DesktopMirrorEye = 0;
        else if (eye == "right" || eye == "1")
            m_DesktopMirrorEye = 1;
    }
    m_DesktopMirrorKeepAspect = getBool("DesktopMirrorKeepAspect", m_DesktopMirrorKeepAspect);
    m_DesktopMirrorLinearFilter = getBool("DesktopMirrorLinearFilter", m_DesktopMirrorLinearFilter);
    m_DesktopMirrorHidePluginOverlaysRequested =
        getBool("DesktopMirrorHidePluginOverlays",
            getBool("m_DesktopMirrorHidePluginOverlays", m_DesktopMirrorHidePluginOverlaysRequested));
    // Keep requested/effective state separate. The clean mirror target is single-thread
    // only. In queued/multicore mode the regular eye is mirrored directly so we do not
    // insert an extra clean world RenderView into Source's shared shadow RTT pipeline.
    const bool desktopMirrorTexturesReady = m_CreatedVRTextures.load(std::memory_order_acquire);
    const bool desktopMirrorCleanTargetReady = (m_DesktopMirrorTexture != nullptr);
    const bool desktopMirrorCleanCopySafe = !m_Game || m_Game->GetMatQueueMode() == 0;
    m_DesktopMirrorHidePluginOverlays =
        m_DesktopMirrorEnabled &&
        m_DesktopMirrorHidePluginOverlaysRequested &&
        desktopMirrorCleanTargetReady &&
        desktopMirrorCleanCopySafe;
    if (m_DesktopMirrorEnabled && m_DesktopMirrorHidePluginOverlaysRequested && desktopMirrorCleanCopySafe && desktopMirrorTexturesReady && !m_DesktopMirrorTexture)
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        m_CreatedVRTextures.store(false, std::memory_order_release);
        m_CreatingTextureID = Texture_None;
    }
    m_ItemModelLabelPlayerSuppressRadius = std::max(0.0f, getFloat("ItemModelLabelPlayerSuppressRadius", m_ItemModelLabelPlayerSuppressRadius));
    m_ItemModelLabelPlayerSuppressMinZ = getFloat("ItemModelLabelPlayerSuppressMinZ", m_ItemModelLabelPlayerSuppressMinZ);
    m_ItemModelLabelPlayerSuppressMaxZ = getFloat("ItemModelLabelPlayerSuppressMaxZ", m_ItemModelLabelPlayerSuppressMaxZ);
    if (m_ItemModelLabelPlayerSuppressMinZ > m_ItemModelLabelPlayerSuppressMaxZ)
        std::swap(m_ItemModelLabelPlayerSuppressMinZ, m_ItemModelLabelPlayerSuppressMaxZ);
    m_SpecialInfectedArrowSize = std::max(0.0f, getFloat("SpecialInfectedArrowSize", m_SpecialInfectedArrowSize));
    m_SpecialInfectedArrowHeight = std::max(0.0f, getFloat("SpecialInfectedArrowHeight", m_SpecialInfectedArrowHeight));
    m_SpecialInfectedArrowThickness = std::max(0.0f, getFloat("SpecialInfectedArrowThickness", m_SpecialInfectedArrowThickness));
    m_SpecialInfectedBlindSpotDistance = std::max(0.0f, getFloat("SpecialInfectedBlindSpotDistance", m_SpecialInfectedBlindSpotDistance));
    m_SpecialInfectedIntentSenseEnabled = getBool("SpecialInfectedIntentSenseEnabled", m_SpecialInfectedIntentSenseEnabled);
    m_SpecialInfectedIntentSenseHudEnabled = getBool("SpecialInfectedIntentSenseHudEnabled", m_SpecialInfectedIntentSenseHudEnabled);
    m_DesktopIntentSenseHudWindowEnabled = getBool("DesktopIntentSenseHudWindowEnabled", m_DesktopIntentSenseHudWindowEnabled);
    m_SpecialInfectedIntentSenseHapticsEnabled = getBool("SpecialInfectedIntentSenseHapticsEnabled", m_SpecialInfectedIntentSenseHapticsEnabled);
    m_SpecialInfectedIntentSenseUseLookFallback = getBool("SpecialInfectedIntentSenseUseLookFallback", m_SpecialInfectedIntentSenseUseLookFallback);
    m_SpecialInfectedIntentSenseWarnFront = getBool("SpecialInfectedIntentSenseWarnFront", m_SpecialInfectedIntentSenseWarnFront);
    m_SpecialInfectedIntentSenseDistance = std::max(0.0f, getFloat("SpecialInfectedIntentSenseDistance", m_SpecialInfectedIntentSenseDistance));
    m_SpecialInfectedIntentSenseLookDot = std::clamp(getFloat("SpecialInfectedIntentSenseLookDot", m_SpecialInfectedIntentSenseLookDot), 0.0f, 1.0f);
    m_SpecialInfectedIntentSenseCooldownSeconds = std::max(0.0f, getFloat("SpecialInfectedIntentSenseCooldownSeconds", m_SpecialInfectedIntentSenseCooldownSeconds));
    m_SpecialInfectedIntentSenseHudWidthMeters = std::clamp(getFloat("SpecialInfectedIntentSenseHudWidthMeters", m_SpecialInfectedIntentSenseHudWidthMeters), 0.10f, 1.50f);
    m_SpecialInfectedIntentSenseHudMarginXMeters = std::clamp(getFloat("SpecialInfectedIntentSenseHudMarginXMeters", m_SpecialInfectedIntentSenseHudMarginXMeters), -1.0f, 1.0f);
    m_SpecialInfectedIntentSenseHudMarginYMeters = std::clamp(getFloat("SpecialInfectedIntentSenseHudMarginYMeters", m_SpecialInfectedIntentSenseHudMarginYMeters), -1.0f, 1.0f);
    m_SpecialInfectedIntentSenseHudMaxLines = std::clamp(getInt("SpecialInfectedIntentSenseHudMaxLines", m_SpecialInfectedIntentSenseHudMaxLines), 1, 8);
    m_SpecialInfectedIntentSenseHapticDuration = std::clamp(getFloat("SpecialInfectedIntentSenseHapticDuration", m_SpecialInfectedIntentSenseHapticDuration), 0.0f, 0.5f);
    m_SpecialInfectedIntentSenseHapticFrequency = std::clamp(getFloat("SpecialInfectedIntentSenseHapticFrequency", m_SpecialInfectedIntentSenseHapticFrequency), 0.0f, 320.0f);
    m_SpecialInfectedIntentSenseHapticAmplitude = std::clamp(getFloat("SpecialInfectedIntentSenseHapticAmplitude", m_SpecialInfectedIntentSenseHapticAmplitude), 0.0f, 1.0f);
    m_SpecialInfectedPreWarningAutoAimConfigEnabled = getBool("SpecialInfectedPreWarningAutoAimEnabled", m_SpecialInfectedPreWarningAutoAimConfigEnabled);
    m_SpecialInfectedPreWarningEvadeDistance = std::max(0.0f, getFloat("SpecialInfectedPreWarningEvadeDistance", m_SpecialInfectedPreWarningEvadeDistance));
    m_SpecialInfectedPreWarningEvadeCooldown = std::max(0.0f, getFloat("SpecialInfectedPreWarningEvadeCooldown", m_SpecialInfectedPreWarningEvadeCooldown));
    if (!m_SpecialInfectedPreWarningAutoAimConfigEnabled)
        m_SpecialInfectedPreWarningAutoAimEnabled = false;
    m_SpecialInfectedPreWarningDistance = std::max(0.0f, getFloat("SpecialInfectedPreWarningDistance", m_SpecialInfectedPreWarningDistance));
    m_SpecialInfectedPreWarningTargetUpdateInterval = std::max(0.0f, getFloat("SpecialInfectedPreWarningTargetUpdateInterval", m_SpecialInfectedPreWarningTargetUpdateInterval));
    m_SpecialInfectedOverlayMaxHz = std::max(0.0f, getFloat("SpecialInfectedOverlayMaxHz", m_SpecialInfectedOverlayMaxHz));
    m_SpecialInfectedTraceMaxHz = std::max(0.0f, getFloat("SpecialInfectedTraceMaxHz", m_SpecialInfectedTraceMaxHz));
    const float preWarningAimAngle = getFloat("SpecialInfectedPreWarningAimAngle", m_SpecialInfectedPreWarningAimAngle);
    m_SpecialInfectedPreWarningAimAngle = m_SpecialInfectedDebug
        ? std::max(0.0f, preWarningAimAngle)
        : std::clamp(preWarningAimAngle, 0.0f, 10.0f);

    const float aimSnapDistance = getFloat("SpecialInfectedPreWarningAimSnapDistance", m_SpecialInfectedPreWarningAimSnapDistance);
    m_SpecialInfectedPreWarningAimSnapDistance = m_SpecialInfectedDebug
        ? std::max(0.0f, aimSnapDistance)
        : std::clamp(aimSnapDistance, 0.0f, 30.0f);

    const float releaseDistance = getFloat("SpecialInfectedPreWarningAimReleaseDistance", m_SpecialInfectedPreWarningAimReleaseDistance);
    m_SpecialInfectedPreWarningAimReleaseDistance = m_SpecialInfectedDebug
        ? std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance))
        : std::clamp(std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance)), 0.0f, 40.0f);

    const float autoAimLerp = getFloat("SpecialInfectedAutoAimLerp", m_SpecialInfectedAutoAimLerp);
    m_SpecialInfectedAutoAimLerp = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimLerp)
        : std::clamp(autoAimLerp, 0.0f, 0.3f);

    const float autoAimCooldown = getFloat("SpecialInfectedAutoAimCooldown", m_SpecialInfectedAutoAimCooldown);
    m_SpecialInfectedAutoAimCooldown = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimCooldown)
        : std::max(0.5f, autoAimCooldown);
    m_SpecialInfectedWarningSecondaryHoldDuration = std::max(0.0f, getFloat("SpecialInfectedWarningSecondaryHoldDuration", m_SpecialInfectedWarningSecondaryHoldDuration));
    m_SpecialInfectedWarningPostAttackDelay = std::max(0.0f, getFloat("SpecialInfectedWarningPostAttackDelay", m_SpecialInfectedWarningPostAttackDelay));
    m_SpecialInfectedWarningJumpHoldDuration = std::max(0.0f, getFloat("SpecialInfectedWarningJumpHoldDuration", m_SpecialInfectedWarningJumpHoldDuration));
    auto specialInfectedArrowColor = getColor("SpecialInfectedArrowColor", m_SpecialInfectedArrowDefaultColor.r, m_SpecialInfectedArrowDefaultColor.g, m_SpecialInfectedArrowDefaultColor.b, 255);
    const bool hasGlobalArrowColor = userConfig.find("SpecialInfectedArrowColor") != userConfig.end();
    m_SpecialInfectedArrowDefaultColor.r = specialInfectedArrowColor[0];
    m_SpecialInfectedArrowDefaultColor.g = specialInfectedArrowColor[1];
    m_SpecialInfectedArrowDefaultColor.b = specialInfectedArrowColor[2];

    if (hasGlobalArrowColor)
    {
        for (auto& color : m_SpecialInfectedArrowColors)
        {
            color.r = m_SpecialInfectedArrowDefaultColor.r;
            color.g = m_SpecialInfectedArrowDefaultColor.g;
            color.b = m_SpecialInfectedArrowDefaultColor.b;
        }
    }

    static const std::array<std::pair<SpecialInfectedType, const char*>, 8> arrowColorConfigKeys =
    {
        std::make_pair(SpecialInfectedType::Boomer, "Boomer"),
        std::make_pair(SpecialInfectedType::Smoker, "Smoker"),
        std::make_pair(SpecialInfectedType::Hunter, "Hunter"),
        std::make_pair(SpecialInfectedType::Spitter, "Spitter"),
        std::make_pair(SpecialInfectedType::Jockey, "Jockey"),
        std::make_pair(SpecialInfectedType::Charger, "Charger"),
        std::make_pair(SpecialInfectedType::Tank, "Tank"),
        std::make_pair(SpecialInfectedType::Witch, "Witch")
    };

    for (const auto& [type, suffix] : arrowColorConfigKeys)
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex >= m_SpecialInfectedArrowColors.size())
            continue;

        std::string key = std::string("SpecialInfectedArrowColor") + suffix;
        auto color = getColor(key.c_str(), m_SpecialInfectedArrowColors[typeIndex].r, m_SpecialInfectedArrowColors[typeIndex].g, m_SpecialInfectedArrowColors[typeIndex].b, 255);
        m_SpecialInfectedArrowColors[typeIndex].r = color[0];
        m_SpecialInfectedArrowColors[typeIndex].g = color[1];
        m_SpecialInfectedArrowColors[typeIndex].b = color[2];
    }

    static const std::array<std::pair<SpecialInfectedType, const char*>, 8> preWarningOffsetConfigKeys =
    {
        std::make_pair(SpecialInfectedType::Boomer, "Boomer"),
        std::make_pair(SpecialInfectedType::Smoker, "Smoker"),
        std::make_pair(SpecialInfectedType::Hunter, "Hunter"),
        std::make_pair(SpecialInfectedType::Spitter, "Spitter"),
        std::make_pair(SpecialInfectedType::Jockey, "Jockey"),
        std::make_pair(SpecialInfectedType::Charger, "Charger"),
        std::make_pair(SpecialInfectedType::Tank, "Tank"),
        std::make_pair(SpecialInfectedType::Witch, "Witch")
    };

    for (const auto& [type, suffix] : preWarningOffsetConfigKeys)
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex >= m_SpecialInfectedPreWarningAimOffsets.size())
            continue;

        std::string key = std::string("SpecialInfectedPreWarningAimOffset") + suffix;
        m_SpecialInfectedPreWarningAimOffsets[typeIndex] = getVector3(key.c_str(), m_SpecialInfectedPreWarningAimOffsets[typeIndex]);
    }

    // Client shadow manager / flashlight shadow controls discovered from client.dll.
    m_ShadowTweaksEnabled = getBool("ShadowTweaksEnabled", m_ShadowTweaksEnabled);
    m_ShadowCvarShadows = std::clamp(getInt("r_shadows", m_ShadowCvarShadows), 0, 1);
    m_ShadowCvarRenderToTexture = std::clamp(getInt("r_shadowrendertotexture", m_ShadowCvarRenderToTexture), 0, 1);
    m_ShadowCvarFlashlightDepthTexture = std::clamp(getInt("r_flashlightdepthtexture", m_ShadowCvarFlashlightDepthTexture), 0, 1);
    m_ShadowCvarFlashlightDepthRes = std::clamp(getInt("r_flashlightdepthres", m_ShadowCvarFlashlightDepthRes), 64, 2048);
    m_ShadowCvarHalfUpdateRate = std::clamp(getInt("r_shadow_half_update_rate", m_ShadowCvarHalfUpdateRate), 0, 1);
    m_ShadowCvarMaxRendered = std::clamp(getInt("r_shadowmaxrendered", m_ShadowCvarMaxRendered), 0, 32);
    m_ShadowCvarMaxRenderableDist = std::clamp(getFloat("cl_max_shadow_renderable_dist", m_ShadowCvarMaxRenderableDist), 0.0f, 8192.0f);
    m_ShadowCvarFlashlightDetailProps = std::clamp(getInt("r_FlashlightDetailProps", m_ShadowCvarFlashlightDetailProps), 0, 2);
    m_ShadowCvarMobSimpleShadows = std::clamp(getInt("z_mob_simple_shadows", m_ShadowCvarMobSimpleShadows), 0, 2);
    m_ShadowCvarWorldLightShadows = std::clamp(getInt("r_shadowfromworldlights", m_ShadowCvarWorldLightShadows), 0, 1);
    m_ShadowCvarFlashlightModels = std::clamp(getInt("r_flashlightmodels", m_ShadowCvarFlashlightModels), 0, 1);
    m_ShadowCvarShadowsOnRenderables = std::clamp(getInt("r_shadows_on_renderables_enable", m_ShadowCvarShadowsOnRenderables), 0, 1);
    m_ShadowCvarFlashlightRenderModels = std::clamp(getInt("r_flashlightrendermodels", m_ShadowCvarFlashlightRenderModels), 0, 1);
    m_ShadowCvarPlayerShadowDist = std::clamp(getFloat("cl_player_shadow_dist", m_ShadowCvarPlayerShadowDist), 0.0f, 8192.0f);
    m_ShadowCvarInfectedShadows = std::clamp(getInt("z_infected_shadows", m_ShadowCvarInfectedShadows), 0, 1);
    m_ShadowCvarNbShadowBlobbyDist = std::clamp(getFloat("nb_shadow_blobby_dist", m_ShadowCvarNbShadowBlobbyDist), 0.0f, 8192.0f);
    m_ShadowCvarNbShadowCullDist = std::clamp(getFloat("nb_shadow_cull_dist", m_ShadowCvarNbShadowCullDist), 0.0f, 8192.0f);
    m_ShadowCvarFlashlightInfectedShadows = std::clamp(getInt("r_flashlightinfectedshadows", m_ShadowCvarFlashlightInfectedShadows), 0, 1);
    m_ShadowEntityTweaksEnabled = getBool("ShadowEntityTweaksEnabled", m_ShadowEntityTweaksEnabled);
    m_ShadowEntityDisableShadows = getBool("ShadowControlDisableShadows", m_ShadowEntityDisableShadows);
    m_ShadowEntityMaxDist = std::clamp(getFloat("ShadowControlMaxDist", m_ShadowEntityMaxDist), 0.0f, 8192.0f);
    m_ShadowEntityLocalLightShadows = getBool("ShadowControlLocalLightShadows", m_ShadowEntityLocalLightShadows);
    m_ShadowProjectedTextureEnableShadows = getBool("ProjectedTextureEnableShadows", m_ShadowProjectedTextureEnableShadows);
    m_ShadowProjectedTextureQuality = std::clamp(getInt("ProjectedTextureShadowQuality", m_ShadowProjectedTextureQuality), 0, 3);
    if (m_ShadowEntityTweaksEnabled)
    {
        m_ShadowEntityTweaksEnabled = false;
        ResetShadowEntityOverrideTracking();
    }
    m_ShadowSettingsDirty.store(true, std::memory_order_release);

    m_FlashlightEnhancementEnabled =
        getBool("FlashlightEnhancementEnabled", m_FlashlightEnhancementEnabled);
    m_FlashlightFollowHmd = getBool("FlashlightFollowHmd", m_FlashlightFollowHmd);
    m_FlashlightFollowHmdForFirearms = getBool("FlashlightFollowHmdForFirearms", m_FlashlightFollowHmdForFirearms);
    m_FlashlightEnhancementSettingsDirty.store(true, std::memory_order_release);
    m_LocalVScriptConvarsEnabled =
        getBool("LocalVScriptConvarsEnabled", m_LocalVScriptConvarsEnabled);
    m_LocalVScriptConvarsLogEnabled =
        getBool("LocalVScriptConvarsLogEnabled", m_LocalVScriptConvarsLogEnabled);
    m_LocalVScriptConvarsBlockExternalWrites =
        getBool("LocalVScriptConvarsBlockExternalWrites", m_LocalVScriptConvarsBlockExternalWrites);
    m_LocalVScriptConvarsPath =
        getString("LocalVScriptConvarsPath", m_LocalVScriptConvarsPath);
    if (m_LocalVScriptConvarsPath.empty())
        m_LocalVScriptConvarsPath = "VR\\local_client_convars.nut";
    m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
    m_AutoFlashlightEnabled = getBool("AutoFlashlightEnabled", m_AutoFlashlightEnabled);
    m_AutoFlashlightDarkThreshold =
        std::clamp(getFloat("AutoFlashlightDarkThreshold", m_AutoFlashlightDarkThreshold), 0.0f, 255.0f);
    m_AutoFlashlightBrightThreshold =
        std::clamp(getFloat("AutoFlashlightBrightThreshold", m_AutoFlashlightBrightThreshold), 0.0f, 255.0f);
    if (m_AutoFlashlightBrightThreshold < m_AutoFlashlightDarkThreshold + 5.0f)
        m_AutoFlashlightBrightThreshold = std::min(255.0f, m_AutoFlashlightDarkThreshold + 5.0f);
    m_AutoFlashlightDebugLog = getBool("AutoFlashlightDebugLog", m_AutoFlashlightDebugLog);

    ParseHapticsConfigFile();
}

template <typename T>
static inline T VR_ReadShadowEntityField(const void* entity, int offset, T fallback)
{
    if (!entity || offset < 0)
        return fallback;

    __try
    {
        return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(entity) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

template <typename T>
static inline void VR_WriteShadowEntityField(void* entity, int offset, T value)
{
    if (!entity || offset < 0)
        return;

    __try
    {
        *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(entity) + offset) = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void VR::CaptureShadowCvarDefaults()
{
    if (!m_Game)
        return;

    m_ShadowOrigShadows = m_Game->GetConVarInt("r_shadows", m_ShadowOrigShadows);
    m_ShadowOrigRenderToTexture = m_Game->GetConVarInt("r_shadowrendertotexture", m_ShadowOrigRenderToTexture);
    m_ShadowOrigFlashlightDepthTexture = m_Game->GetConVarInt("r_flashlightdepthtexture", m_ShadowOrigFlashlightDepthTexture);
    m_ShadowOrigFlashlightDepthRes = m_Game->GetConVarInt("r_flashlightdepthres", m_ShadowOrigFlashlightDepthRes);
    m_ShadowOrigHalfUpdateRate = m_Game->GetConVarInt("r_shadow_half_update_rate", m_ShadowOrigHalfUpdateRate);
    m_ShadowOrigMaxRendered = m_Game->GetConVarInt("r_shadowmaxrendered", m_ShadowOrigMaxRendered);
    m_ShadowOrigMaxRenderableDist = m_Game->GetConVarFloat("cl_max_shadow_renderable_dist", m_ShadowOrigMaxRenderableDist);
    m_ShadowOrigFlashlightDetailProps = m_Game->GetConVarInt("r_FlashlightDetailProps", m_ShadowOrigFlashlightDetailProps);
    m_ShadowOrigMobSimpleShadows = m_Game->GetConVarInt("z_mob_simple_shadows", m_ShadowOrigMobSimpleShadows);
    m_ShadowOrigWorldLightShadows = m_Game->GetConVarInt("r_shadowfromworldlights", m_ShadowOrigWorldLightShadows);
    m_ShadowOrigFlashlightModels = m_Game->GetConVarInt("r_flashlightmodels", m_ShadowOrigFlashlightModels);
    m_ShadowOrigShadowsOnRenderables = m_Game->GetConVarInt("r_shadows_on_renderables_enable", m_ShadowOrigShadowsOnRenderables);
    m_ShadowOrigFlashlightRenderModels = m_Game->GetConVarInt("r_flashlightrendermodels", m_ShadowOrigFlashlightRenderModels);
    m_ShadowOrigPlayerShadowDist = m_Game->GetConVarFloat("cl_player_shadow_dist", m_ShadowOrigPlayerShadowDist);
    m_ShadowOrigInfectedShadows = m_Game->GetConVarInt("z_infected_shadows", m_ShadowOrigInfectedShadows);
    m_ShadowOrigNbShadowBlobbyDist = m_Game->GetConVarFloat("nb_shadow_blobby_dist", m_ShadowOrigNbShadowBlobbyDist);
    m_ShadowOrigNbShadowCullDist = m_Game->GetConVarFloat("nb_shadow_cull_dist", m_ShadowOrigNbShadowCullDist);
    m_ShadowOrigFlashlightInfectedShadows = m_Game->GetConVarInt("r_flashlightinfectedshadows", m_ShadowOrigFlashlightInfectedShadows);
    m_ShadowOriginalsCaptured = true;
}

void VR::RestoreShadowCvarDefaults()
{
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        m_ShadowProtectedConvars.clear();
    }
    if (!m_Game || !m_ShadowOriginalsCaptured)
        return;

    m_Game->SetConVarInt("r_shadows", m_ShadowOrigShadows);
    m_Game->SetConVarInt("r_shadowrendertotexture", m_ShadowOrigRenderToTexture);
    m_Game->SetConVarInt("r_flashlightdepthtexture", m_ShadowOrigFlashlightDepthTexture);
    m_Game->SetConVarInt("r_flashlightdepthres", m_ShadowOrigFlashlightDepthRes);
    m_Game->SetConVarInt("r_shadow_half_update_rate", m_ShadowOrigHalfUpdateRate);
    m_Game->SetConVarInt("r_shadowmaxrendered", m_ShadowOrigMaxRendered);
    m_Game->SetConVarFloat("cl_max_shadow_renderable_dist", m_ShadowOrigMaxRenderableDist);
    m_Game->SetConVarInt("r_FlashlightDetailProps", m_ShadowOrigFlashlightDetailProps);
    m_Game->SetConVarInt("z_mob_simple_shadows", m_ShadowOrigMobSimpleShadows);
    m_Game->SetConVarInt("r_shadowfromworldlights", m_ShadowOrigWorldLightShadows);
    m_Game->SetConVarInt("r_flashlightmodels", m_ShadowOrigFlashlightModels);
    m_Game->SetConVarInt("r_shadows_on_renderables_enable", m_ShadowOrigShadowsOnRenderables);
    m_Game->SetConVarInt("r_flashlightrendermodels", m_ShadowOrigFlashlightRenderModels);
    m_Game->SetConVarFloat("cl_player_shadow_dist", m_ShadowOrigPlayerShadowDist);
    m_Game->SetConVarInt("z_infected_shadows", m_ShadowOrigInfectedShadows);
    m_Game->SetConVarFloat("nb_shadow_blobby_dist", m_ShadowOrigNbShadowBlobbyDist);
    m_Game->SetConVarFloat("nb_shadow_cull_dist", m_ShadowOrigNbShadowCullDist);
    m_Game->SetConVarInt("r_flashlightinfectedshadows", m_ShadowOrigFlashlightInfectedShadows);
}

void VR::ResetShadowEntityOverrideTracking()
{
    m_ShadowEntityOverridesApplied = false;
    m_ShadowEntityLastRefreshTime = {};
    m_ShadowEntityOffsetsLogged = false;
    m_ShadowControlEntityDefaults.clear();
    m_EnvProjectedTextureEntityDefaults.clear();
}

void VR::RestoreShadowEntityDefaults()
{
    if (!m_Game || !m_Game->m_ClientEntityList)
    {
        ResetShadowEntityOverrideTracking();
        return;
    }

    const int shadowControlDisableOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bDisableShadows");
    const int shadowControlMaxDistOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_flShadowMaxDist");
    const int shadowControlLocalLightOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bEnableLocalLightShadows");
    const int projectedTextureEnableOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_bEnableShadows");
    const int projectedTextureQualityOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_nShadowQuality");

    int restoredShadowControls = 0;
    for (const auto& [entityIndex, defaults] : m_ShadowControlEntityDefaults)
    {
        C_BaseEntity* entity = m_Game->GetClientEntity(entityIndex);
        if (!entity)
            continue;

        VR_WriteShadowEntityField<bool>(entity, shadowControlDisableOffset, defaults.disableShadows);
        VR_WriteShadowEntityField<float>(entity, shadowControlMaxDistOffset, defaults.maxDist);
        VR_WriteShadowEntityField<bool>(entity, shadowControlLocalLightOffset, defaults.enableLocalLightShadows);
        ++restoredShadowControls;
    }

    int restoredProjectedTextures = 0;
    for (const auto& [entityIndex, defaults] : m_EnvProjectedTextureEntityDefaults)
    {
        C_BaseEntity* entity = m_Game->GetClientEntity(entityIndex);
        if (!entity)
            continue;

        VR_WriteShadowEntityField<bool>(entity, projectedTextureEnableOffset, defaults.enableShadows);
        VR_WriteShadowEntityField<int>(entity, projectedTextureQualityOffset, defaults.shadowQuality);
        ++restoredProjectedTextures;
    }

    if (restoredShadowControls > 0 || restoredProjectedTextures > 0)
    {
    }

    ResetShadowEntityOverrideTracking();
}

void VR::ApplyShadowEntityOverrides(bool forceRefresh)
{
    if (!m_Game || !m_Game->m_ClientEntityList || !m_Game->m_EngineClient)
        return;

    if (!m_Game->m_EngineClient->IsInGame())
    {
        ResetShadowEntityOverrideTracking();
        return;
    }

    if (!m_ShadowEntityTweaksEnabled)
    {
        if (m_ShadowEntityOverridesApplied)
            RestoreShadowEntityDefaults();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh && m_ShadowEntityLastRefreshTime != std::chrono::steady_clock::time_point{} &&
        now - m_ShadowEntityLastRefreshTime < std::chrono::milliseconds(250))
    {
        return;
    }

    const int shadowControlDisableOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bDisableShadows");
    const int shadowControlMaxDistOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_flShadowMaxDist");
    const int shadowControlLocalLightOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bEnableLocalLightShadows");
    const int projectedTextureEnableOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_bEnableShadows");
    const int projectedTextureQualityOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_nShadowQuality");

    const bool hasShadowControlOffsets =
        shadowControlDisableOffset >= 0 && shadowControlMaxDistOffset >= 0 && shadowControlLocalLightOffset >= 0;
    const bool hasProjectedTextureOffsets =
        projectedTextureEnableOffset >= 0 && projectedTextureQualityOffset >= 0;

    if (!hasShadowControlOffsets && !hasProjectedTextureOffsets)
    {
        if (!m_ShadowEntityOffsetsLogged)
        {
            m_ShadowEntityOffsetsLogged = true;
        }
        m_ShadowEntityTweaksEnabled = false;
        return;
    }

    const int highestEntityIndex = m_Game->m_ClientEntityList->GetHighestEntityIndex();
    int touchedShadowControls = 0;
    int touchedProjectedTextures = 0;

    for (int entityIndex = 0; entityIndex <= highestEntityIndex; ++entityIndex)
    {
        C_BaseEntity* entity = m_Game->GetClientEntity(entityIndex);
        if (!entity)
            continue;

        const char* className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
        if (!className || !*className)
            continue;

        if (hasShadowControlOffsets &&
            (std::strcmp(className, "CShadowControl") == 0 || std::strcmp(className, "C_ShadowControl") == 0))
        {
            auto [it, inserted] = m_ShadowControlEntityDefaults.try_emplace(entityIndex);
            if (inserted)
            {
                it->second.disableShadows = VR_ReadShadowEntityField<bool>(entity, shadowControlDisableOffset, false);
                it->second.maxDist = VR_ReadShadowEntityField<float>(entity, shadowControlMaxDistOffset, 0.0f);
                it->second.enableLocalLightShadows =
                    VR_ReadShadowEntityField<bool>(entity, shadowControlLocalLightOffset, false);
            }

            VR_WriteShadowEntityField<bool>(entity, shadowControlDisableOffset, m_ShadowEntityDisableShadows);
            VR_WriteShadowEntityField<float>(entity, shadowControlMaxDistOffset, m_ShadowEntityMaxDist);
            VR_WriteShadowEntityField<bool>(entity, shadowControlLocalLightOffset, m_ShadowEntityLocalLightShadows);
            ++touchedShadowControls;
            continue;
        }

        if (hasProjectedTextureOffsets &&
            (std::strcmp(className, "CEnvProjectedTexture") == 0 || std::strcmp(className, "C_EnvProjectedTexture") == 0))
        {
            auto [it, inserted] = m_EnvProjectedTextureEntityDefaults.try_emplace(entityIndex);
            if (inserted)
            {
                it->second.enableShadows = VR_ReadShadowEntityField<bool>(entity, projectedTextureEnableOffset, true);
                it->second.shadowQuality = VR_ReadShadowEntityField<int>(entity, projectedTextureQualityOffset, 0);
            }

            VR_WriteShadowEntityField<bool>(entity, projectedTextureEnableOffset, m_ShadowProjectedTextureEnableShadows);
            VR_WriteShadowEntityField<int>(entity, projectedTextureQualityOffset, m_ShadowProjectedTextureQuality);
            ++touchedProjectedTextures;
        }
    }

    m_ShadowEntityLastRefreshTime = now;
    m_ShadowEntityOffsetsLogged = false;
    m_ShadowEntityOverridesApplied = (touchedShadowControls > 0 || touchedProjectedTextures > 0);

    if (forceRefresh || touchedShadowControls > 0 || touchedProjectedTextures > 0)
    {
    }
}

void VR::ApplyShadowSettingsIfNeeded(bool forceApply)
{
    const bool dirty = m_ShadowSettingsDirty.exchange(false, std::memory_order_acq_rel);
    const bool wantsEntityRefresh = false;
    const bool needsEntityRefresh =
        wantsEntityRefresh &&
        (dirty ||
            m_ShadowEntityLastRefreshTime == std::chrono::steady_clock::time_point{} ||
            (std::chrono::steady_clock::now() - m_ShadowEntityLastRefreshTime) >= std::chrono::milliseconds(250));

    if (!dirty && !forceApply && !needsEntityRefresh)
        return;

    if (!m_Game || !m_Game->m_Initialized)
    {
        if (dirty || forceApply)
            m_ShadowSettingsDirty.store(true, std::memory_order_release);
        return;
    }

    if (!dirty && !forceApply)
        return;

    if (!m_ShadowTweaksEnabled)
    {
        if (m_ShadowTweaksApplied)
        {
            RestoreShadowCvarDefaults();
        }
        else
        {
            std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
            m_ShadowProtectedConvars.clear();
        }

        m_ShadowTweaksApplied = false;
        return;
    }

    if (!m_ShadowOriginalsCaptured)
        CaptureShadowCvarDefaults();

    int appliedCount = 0;
    std::unordered_set<std::string> protectedConvars;
    auto applyInt = [&](const char* name, int value)
        {
            if (m_Game->SetConVarInt(name, value))
            {
                ++appliedCount;
                protectedConvars.insert(name);
            }
        };

    applyInt("r_shadows", m_ShadowCvarShadows);
    applyInt("r_shadowrendertotexture", m_ShadowCvarRenderToTexture);
    applyInt("r_flashlightdepthtexture", m_ShadowCvarFlashlightDepthTexture);
    applyInt("r_flashlightdepthres", m_ShadowCvarFlashlightDepthRes);
    applyInt("r_shadow_half_update_rate", m_ShadowCvarHalfUpdateRate);
    applyInt("r_shadowmaxrendered", m_ShadowCvarMaxRendered);
    if (m_Game->SetConVarFloat("cl_max_shadow_renderable_dist", m_ShadowCvarMaxRenderableDist))
    {
        ++appliedCount;
        protectedConvars.insert("cl_max_shadow_renderable_dist");
    }
    applyInt("r_FlashlightDetailProps", m_ShadowCvarFlashlightDetailProps);
    applyInt("z_mob_simple_shadows", m_ShadowCvarMobSimpleShadows);
    applyInt("r_shadowfromworldlights", m_ShadowCvarWorldLightShadows);
    applyInt("r_flashlightmodels", m_ShadowCvarFlashlightModels);
    applyInt("r_shadows_on_renderables_enable", m_ShadowCvarShadowsOnRenderables);
    applyInt("r_flashlightrendermodels", m_ShadowCvarFlashlightRenderModels);
    if (m_Game->SetConVarFloat("cl_player_shadow_dist", m_ShadowCvarPlayerShadowDist))
    {
        ++appliedCount;
        protectedConvars.insert("cl_player_shadow_dist");
    }
    applyInt("z_infected_shadows", m_ShadowCvarInfectedShadows);
    if (m_Game->SetConVarFloat("nb_shadow_blobby_dist", m_ShadowCvarNbShadowBlobbyDist))
    {
        ++appliedCount;
        protectedConvars.insert("nb_shadow_blobby_dist");
    }
    if (m_Game->SetConVarFloat("nb_shadow_cull_dist", m_ShadowCvarNbShadowCullDist))
    {
        ++appliedCount;
        protectedConvars.insert("nb_shadow_cull_dist");
    }
    applyInt("r_flashlightinfectedshadows", m_ShadowCvarFlashlightInfectedShadows);

    m_ShadowTweaksApplied = (appliedCount > 0);
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        m_ShadowProtectedConvars = std::move(protectedConvars);
    }
}

void VR::CaptureFlashlightEnhancementDefaults()
{
    if (!m_Game)
        return;

    m_FlashlightEnhancementOrig3rdPersonRange =
        m_Game->GetConVarFloat("r_flashlight_3rd_person_range", m_FlashlightEnhancementOrig3rdPersonRange);
    m_FlashlightEnhancementOrigBrightness =
        m_Game->GetConVarFloat("r_flashlightbrightness", m_FlashlightEnhancementOrigBrightness);
    m_FlashlightEnhancementOrigFov =
        m_Game->GetConVarFloat("r_flashlightfov", m_FlashlightEnhancementOrigFov);
    m_FlashlightEnhancementOrig3rdPersonRangeFlags =
        m_Game->GetConVarFlags("r_flashlight_3rd_person_range");
    m_FlashlightEnhancementOrigBrightnessFlags =
        m_Game->GetConVarFlags("r_flashlightbrightness");
    m_FlashlightEnhancementOrigFovFlags =
        m_Game->GetConVarFlags("r_flashlightfov");
    m_FlashlightEnhancementOriginalsCaptured = true;
}

void VR::RestoreFlashlightEnhancementDefaults()
{
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        m_FlashlightEnhancementProtectedConvars.clear();
    }
    if (!m_Game || !m_FlashlightEnhancementOriginalsCaptured)
        return;

    m_Game->SetConVarFloat("r_flashlight_3rd_person_range", m_FlashlightEnhancementOrig3rdPersonRange);
    m_Game->SetConVarFloat("r_flashlightbrightness", m_FlashlightEnhancementOrigBrightness);
    m_Game->SetConVarFloat("r_flashlightfov", m_FlashlightEnhancementOrigFov);
    if (m_FlashlightEnhancementOrig3rdPersonRangeFlags >= 0)
        m_Game->SetConVarFlags("r_flashlight_3rd_person_range", m_FlashlightEnhancementOrig3rdPersonRangeFlags);
    if (m_FlashlightEnhancementOrigBrightnessFlags >= 0)
        m_Game->SetConVarFlags("r_flashlightbrightness", m_FlashlightEnhancementOrigBrightnessFlags);
    if (m_FlashlightEnhancementOrigFovFlags >= 0)
        m_Game->SetConVarFlags("r_flashlightfov", m_FlashlightEnhancementOrigFovFlags);

    m_FlashlightEnhancementOrig3rdPersonRangeFlags = -1;
    m_FlashlightEnhancementOrigBrightnessFlags = -1;
    m_FlashlightEnhancementOrigFovFlags = -1;
    m_FlashlightEnhancementOriginalsCaptured = false;
}

void VR::ApplyFlashlightEnhancementIfNeeded()
{
    const bool dirty = m_FlashlightEnhancementSettingsDirty.exchange(false, std::memory_order_acq_rel);
    if (!dirty)
        return;

    if (!m_Game || !m_Game->m_Initialized)
    {
        m_FlashlightEnhancementSettingsDirty.store(true, std::memory_order_release);
        return;
    }

    if (!m_FlashlightEnhancementEnabled)
    {
        if (m_FlashlightEnhancementApplied && m_FlashlightEnhancementOriginalsCaptured)
        {
            RestoreFlashlightEnhancementDefaults();
        }
        else
        {
            std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
            m_FlashlightEnhancementProtectedConvars.clear();
        }

        m_FlashlightEnhancementApplied = false;
        return;
    }

    if (!m_FlashlightEnhancementOriginalsCaptured)
        CaptureFlashlightEnhancementDefaults();

    constexpr int kFlashlightEnhancementCheatFlag = 1 << 14;
    int appliedCount = 0;
    std::unordered_set<std::string> protectedConvars;
    auto applyFloat = [&](const char* name, float value, int originalFlags)
        {
            const bool hadCheatFlag =
                originalFlags >= 0 && (originalFlags & kFlashlightEnhancementCheatFlag) != 0;
            if (hadCheatFlag)
                m_Game->SetConVarFlags(name, originalFlags & ~kFlashlightEnhancementCheatFlag);

            if (m_Game->SetConVarFloat(name, value))
            {
                ++appliedCount;
                protectedConvars.insert(name);
            }
            else if (hadCheatFlag)
            {
                m_Game->SetConVarFlags(name, originalFlags);
            }
        };

    applyFloat(
        "r_flashlight_3rd_person_range",
        m_FlashlightEnhancement3rdPersonRange,
        m_FlashlightEnhancementOrig3rdPersonRangeFlags);
    applyFloat(
        "r_flashlightbrightness",
        m_FlashlightEnhancementBrightness,
        m_FlashlightEnhancementOrigBrightnessFlags);
    applyFloat(
        "r_flashlightfov",
        m_FlashlightEnhancementFov,
        m_FlashlightEnhancementOrigFovFlags);

    m_FlashlightEnhancementApplied = (appliedCount > 0);
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        m_FlashlightEnhancementProtectedConvars = std::move(protectedConvars);
    }
}

void VR::ApplyVrRecommendedVideoSettingsIfNeeded(const char* reason)
{
    if (!m_VrRecommendedVideoSettingsEnabled)
        return;

    const bool ok = L4D2VR_ApplyRecommendedVideoSettings();
    m_VrRecommendedVideoSettingsApplyPending = false;
    m_VrRecommendedVideoSettingsAppliedThisSession = true;

    Game::logMsg(
        "[VR][VideoSettings] %s VR recommended video settings reason=%s",
        ok ? "applied" : "failed to apply",
        (reason && *reason) ? reason : "unspecified");
}

namespace
{
    constexpr int kLocalVScriptFlagGameDll = 1 << 2;
    constexpr int kLocalVScriptFlagClientDll = 1 << 3;
    constexpr int kLocalVScriptFlagReplicated = 1 << 13;
    constexpr int kLocalVScriptFlagCheat = 1 << 14;
    enum class LocalVScriptValueKind
    {
        String,
        Bool,
        Int,
        Float
    };

    inline void TrimLocalVScriptValue(std::string& value)
    {
        auto ltrim = [](std::string& s)
        {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch)
                {
                    return !std::isspace(ch);
                }));
        };
        auto rtrim = [](std::string& s)
        {
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch)
                {
                    return !std::isspace(ch);
                }).base(), s.end());
        };

        ltrim(value);
        rtrim(value);
    }

    void StripLocalVScriptLineComment(std::string& line)
    {
        bool inString = false;
        bool escaped = false;
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            const char ch = line[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (inString && ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (!inString && ch == '/' && line[i + 1] == '/')
            {
                line.erase(i);
                return;
            }
        }
    }

    bool TryParseLocalVScriptBool(const std::string& rawValue, bool& outValue)
    {
        std::string value = rawValue;
        TrimLocalVScriptValue(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });

        if (value == "true" || value == "1" || value == "on" || value == "yes")
        {
            outValue = true;
            return true;
        }

        if (value == "false" || value == "0" || value == "off" || value == "no")
        {
            outValue = false;
            return true;
        }

        return false;
    }

    LocalVScriptValueKind InferLocalVScriptValueKind(
        const std::string& rawValue,
        bool& outBool,
        int& outInt,
        float& outFloat)
    {
        std::string value = rawValue;
        TrimLocalVScriptValue(value);

        if (TryParseLocalVScriptBool(value, outBool))
            return LocalVScriptValueKind::Bool;

        char* intEnd = nullptr;
        const long parsedInt = std::strtol(value.c_str(), &intEnd, 10);
        if (intEnd != nullptr && *value.c_str() != '\0' && *intEnd == '\0')
        {
            outInt = static_cast<int>(parsedInt);
            return LocalVScriptValueKind::Int;
        }

        char* floatEnd = nullptr;
        const float parsedFloat = std::strtof(value.c_str(), &floatEnd);
        if (floatEnd != nullptr && *value.c_str() != '\0' && *floatEnd == '\0' && std::isfinite(parsedFloat))
        {
            outFloat = parsedFloat;
            return LocalVScriptValueKind::Float;
        }

        return LocalVScriptValueKind::String;
    }

    const char* GetLocalVScriptValueKindName(LocalVScriptValueKind kind)
    {
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
            return "bool";
        case LocalVScriptValueKind::Int:
            return "int";
        case LocalVScriptValueKind::Float:
            return "float";
        default:
            return "string";
        }
    }

    bool LocalVScriptValuesEquivalent(const std::string& requestedRaw, const std::string& actualRaw)
    {
        std::string requested = requestedRaw;
        std::string actual = actualRaw;
        TrimLocalVScriptValue(requested);
        TrimLocalVScriptValue(actual);

        if (requested == actual)
            return true;

        bool requestedBool = false;
        bool actualBool = false;
        if (TryParseLocalVScriptBool(requested, requestedBool) &&
            TryParseLocalVScriptBool(actual, actualBool))
        {
            return requestedBool == actualBool;
        }

        char* requestedEnd = nullptr;
        char* actualEnd = nullptr;
        const double requestedNumber = std::strtod(requested.c_str(), &requestedEnd);
        const double actualNumber = std::strtod(actual.c_str(), &actualEnd);
        const bool requestedIsNumber =
            requestedEnd != nullptr && *requested.c_str() != '\0' && *requestedEnd == '\0';
        const bool actualIsNumber =
            actualEnd != nullptr && *actual.c_str() != '\0' && *actualEnd == '\0';

        if (requestedIsNumber && actualIsNumber)
            return std::fabs(requestedNumber - actualNumber) <= 0.0001;

        return false;
    }

    bool ShouldThrottleLocalVScriptLog(
        std::chrono::steady_clock::time_point& last,
        float maxHz)
    {
        if (maxHz <= 0.0f)
            return false;

        const auto now = std::chrono::steady_clock::now();
        const auto minInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / maxHz));
        if (last.time_since_epoch().count() != 0 && now - last < minInterval)
            return true;

        last = now;
        return false;
    }

    const char* GetLocalVScriptSkipReason(const std::string& name, int flags)
    {
        (void)name;
        if ((flags & kLocalVScriptFlagGameDll) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return "FCVAR_GAMEDLL without FCVAR_CLIENTDLL";
        if ((flags & kLocalVScriptFlagReplicated) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return "FCVAR_REPLICATED without FCVAR_CLIENTDLL";

        return "unknown";
    }

    bool ShouldSkipLocalVScriptConvar(const std::string& name, int flags)
    {
        (void)name;
        if ((flags & kLocalVScriptFlagGameDll) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return true;

        if ((flags & kLocalVScriptFlagReplicated) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return true;

        return false;
    }

    bool ParseLocalVScriptConvarsFile(
        const std::string& path,
        std::vector<VR::LocalVScriptConvarEntry>& outEntries)
    {
        outEntries.clear();
        if (path.empty())
            return false;

        std::ifstream stream(path);
        if (!stream)
            return false;

        const std::regex pattern(
            R"VSCVAR(^\s*Convars\.SetValue\(\s*"([^"]+)"\s*,\s*(.+?)\s*\)\s*;?\s*$)VSCVAR");

        std::unordered_map<std::string, size_t> lastByName;
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            StripLocalVScriptLineComment(line);
            TrimLocalVScriptValue(line);
            if (line.empty())
                continue;

            std::smatch match;
            if (!std::regex_match(line, match, pattern))
                continue;

            std::string name = match[1].str();
            std::string value = match[2].str();
            TrimLocalVScriptValue(name);
            TrimLocalVScriptValue(value);
            if (name.empty())
                continue;

            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            {
                value = value.substr(1, value.size() - 2);
            }

            VR::LocalVScriptConvarEntry entry;
            entry.name = std::move(name);
            entry.value = std::move(value);
            auto existing = lastByName.find(entry.name);
            if (existing != lastByName.end())
            {
                outEntries[existing->second].value = entry.value;
            }
            else
            {
                lastByName.emplace(entry.name, outEntries.size());
                outEntries.push_back(std::move(entry));
            }
        }

        return true;
    }

    bool ShouldLogLocalVScriptConvars(const VR* vr)
    {
        return vr != nullptr && vr->m_LocalVScriptConvarsLogEnabled;
    }

    template <typename... Args>
    void LocalVScriptLog(const VR* vr, const char* format, Args... args)
    {
        if (ShouldLogLocalVScriptConvars(vr))
            Game::logMsg(format, args...);
    }
}

void VR::RestoreLocalVScriptConvars()
{
    if (!m_Game)
        return;

    std::vector<LocalVScriptConvarEntry> entriesToRestore;
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        if (!m_LocalVScriptConvarsApplied)
        {
            m_LocalVScriptConvars.clear();
            m_LocalVScriptConvarBlockedWriteLastLog.clear();
            return;
        }

        entriesToRestore = m_LocalVScriptConvars;
        m_LocalVScriptConvars.clear();
        m_LocalVScriptConvarsApplied = false;
        m_LocalVScriptConvarBlockedWriteLastLog.clear();
    }

    if (entriesToRestore.empty())
    {
        return;
    }

    LocalVScriptLog(
        this,
        "[VR][LocalVScriptConvars] Restore begin tracked=%zu",
        entriesToRestore.size());

    int restoredCount = 0;
    int restoreFailedCount = 0;
    int restoreVerifyMismatchCount = 0;
    for (const LocalVScriptConvarEntry& entry : entriesToRestore)
    {
        if (entry.name.empty())
            continue;

        if ((entry.flags & kLocalVScriptFlagCheat) != 0)
        {
            const int writableFlags = entry.flags & ~kLocalVScriptFlagCheat;
            const bool cleared = m_Game->SetConVarFlags(entry.name.c_str(), writableFlags);
        }

        const std::string beforeValue = m_Game->GetConVarString(entry.name.c_str());
        const bool setOk = m_Game->SetConVarString(entry.name.c_str(), entry.originalValue.c_str());
        const std::string afterValue = m_Game->GetConVarString(entry.name.c_str());
        std::string directReadback = "-";
        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        const LocalVScriptValueKind kind =
            InferLocalVScriptValueKind(entry.originalValue, boolValue, intValue, floatValue);
        bool verified = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
        {
            const int afterDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadback = std::to_string(afterDirect);
            verified = (afterDirect == (boolValue ? 1 : 0));
            break;
        }
        case LocalVScriptValueKind::Int:
        {
            const int afterDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadback = std::to_string(afterDirect);
            verified = (afterDirect == intValue);
            break;
        }
        case LocalVScriptValueKind::Float:
        {
            const float afterDirect = m_Game->GetConVarFloatDirect(entry.name.c_str(), -999999.0f);
            char buffer[64] = {};
            sprintf_s(buffer, "%.9g", static_cast<double>(afterDirect));
            directReadback = buffer;
            verified = std::fabs(afterDirect - floatValue) <= 0.0001f;
            break;
        }
        default:
            directReadback = afterValue;
            verified = LocalVScriptValuesEquivalent(entry.originalValue, afterValue);
            break;
        }

        if (setOk)
        {
            ++restoredCount;
            if (!verified)
                ++restoreVerifyMismatchCount;
        }
        else
            ++restoreFailedCount;

        if ((entry.flags & kLocalVScriptFlagCheat) != 0)
        {
            const bool restoredFlags = m_Game->SetConVarFlags(entry.name.c_str(), entry.flags);
        }

        LocalVScriptLog(
            this,
            "[VR][LocalVScriptConvars] Restore name=%s kind=%s flags=0x%08X before='%s' target='%s' after='%s' afterDirect='%s' setOk=%d verified=%d",
            entry.name.c_str(),
            GetLocalVScriptValueKindName(kind),
            entry.flags,
            beforeValue.c_str(),
            entry.originalValue.c_str(),
            afterValue.c_str(),
            directReadback.c_str(),
            setOk ? 1 : 0,
            verified ? 1 : 0);
    }

    LocalVScriptLog(
        this,
        "[VR][LocalVScriptConvars] Restore done restored=%d verifyMismatch=%d restoreFailed=%d",
        restoredCount,
        restoreVerifyMismatchCount,
        restoreFailedCount);
}

bool VR::ShouldBlockExternalProtectedConvarWrite(const char* name, const char* requestedValue)
{
    if (!m_LocalVScriptConvarsBlockExternalWrites || !name || !*name)
        return false;

    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        if (m_LocalVScriptConvarsApplied)
        {
            auto entryIt = std::find_if(
                m_LocalVScriptConvars.begin(),
                m_LocalVScriptConvars.end(),
                [&](const LocalVScriptConvarEntry& entry)
                {
                    return entry.lockProtected && entry.name == name;
                });
            if (entryIt != m_LocalVScriptConvars.end())
            {
                auto& last = m_LocalVScriptConvarBlockedWriteLastLog[entryIt->name];
                if (!ShouldThrottleLocalVScriptLog(last, m_LocalVScriptConvarsBlockedWriteLogHz))
                {
                    LocalVScriptLog(
                        this,
                        "[VR][LocalVScriptConvars] Blocked external write name=%s requested='%s' locked='%s'",
                        entryIt->name.c_str(),
                        requestedValue ? requestedValue : "",
                        entryIt->value.c_str());
                }
                return true;
            }
        }

        if (m_ShadowProtectedConvars.find(name) != m_ShadowProtectedConvars.end())
            return true;

        if (m_FlashlightEnhancementProtectedConvars.find(name) != m_FlashlightEnhancementProtectedConvars.end())
            return true;
    }

    return false;
}

bool VR::TryGetTrackedProtectedConvarValue(const char* name, std::string& outValue) const
{
    outValue.clear();
    if (!name || !*name)
        return false;

    std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
    if (!m_LocalVScriptConvarsApplied)
        return false;

    auto entryIt = std::find_if(
        m_LocalVScriptConvars.begin(),
        m_LocalVScriptConvars.end(),
        [&](const LocalVScriptConvarEntry& entry)
        {
            return entry.lockProtected && entry.name == name;
        });
    if (entryIt == m_LocalVScriptConvars.end())
        return false;

    outValue = entryIt->value;
    return true;
}

void VR::ApplyLocalVScriptConvarsIfNeeded()
{
    const bool dirty = m_LocalVScriptConvarsDirty.exchange(false, std::memory_order_acq_rel);
    if (!dirty)
        return;

    if (!m_Game || !m_Game->m_Initialized)
    {
        m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
        return;
    }

    if (m_LocalVScriptConvarsApplied)
        RestoreLocalVScriptConvars();

    if (!m_LocalVScriptConvarsEnabled)
        return;

    std::vector<LocalVScriptConvarEntry> parsedEntries;
    if (!ParseLocalVScriptConvarsFile(m_LocalVScriptConvarsPath, parsedEntries))
    {
        LocalVScriptLog(
            this,
            "[VR][LocalVScriptConvars] Apply parse failed path=%s",
            m_LocalVScriptConvarsPath.c_str());
        return;
    }

    LocalVScriptLog(
        this,
        "[VR][LocalVScriptConvars] Apply begin path=%s parsed=%zu",
        m_LocalVScriptConvarsPath.c_str(),
        parsedEntries.size());

    int appliedCount = 0;
    int skippedCount = 0;
    int missingCount = 0;
    int writeFailedCount = 0;
    int verifyMismatchCount = 0;
    std::vector<LocalVScriptConvarEntry> appliedEntries;

    for (LocalVScriptConvarEntry& entry : parsedEntries)
    {
        entry.flags = m_Game->GetConVarFlags(entry.name.c_str());
        if (entry.flags < 0)
        {
            ++missingCount;
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] Missing name=%s requested='%s'",
                entry.name.c_str(),
                entry.value.c_str());
            continue;
        }

        if (ShouldSkipLocalVScriptConvar(entry.name, entry.flags))
        {
            ++skippedCount;
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] Skip name=%s flags=0x%08X requested='%s' reason=%s",
                entry.name.c_str(),
                entry.flags,
                entry.value.c_str(),
                GetLocalVScriptSkipReason(entry.name, entry.flags));
            continue;
        }

        entry.originalValue = m_Game->GetConVarString(entry.name.c_str());
        if ((entry.flags & kLocalVScriptFlagCheat) != 0)
        {
            const int writableFlags = entry.flags & ~kLocalVScriptFlagCheat;
            const bool cleared = m_Game->SetConVarFlags(entry.name.c_str(), writableFlags);
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] ApplyFlag name=%s action=clear_cheat flagsBefore=0x%08X flagsAfter=0x%08X setOk=%d",
                entry.name.c_str(),
                entry.flags,
                writableFlags,
                cleared ? 1 : 0);
        }
        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        const LocalVScriptValueKind kind =
            InferLocalVScriptValueKind(entry.value, boolValue, intValue, floatValue);

        bool setOk = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
            setOk = m_Game->SetConVarBool(entry.name.c_str(), boolValue);
            break;
        case LocalVScriptValueKind::Int:
            setOk = m_Game->SetConVarInt(entry.name.c_str(), intValue);
            break;
        case LocalVScriptValueKind::Float:
            setOk = m_Game->SetConVarFloat(entry.name.c_str(), floatValue);
            break;
        default:
            setOk = m_Game->SetConVarString(entry.name.c_str(), entry.value.c_str());
            break;
        }
        const std::string readbackValue = m_Game->GetConVarString(entry.name.c_str());
        std::string directReadbackValue = "-";
        bool verified = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
        {
            const int readbackDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadbackValue = std::to_string(readbackDirect);
            verified = (readbackDirect == (boolValue ? 1 : 0));
            break;
        }
        case LocalVScriptValueKind::Int:
        {
            const int readbackDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadbackValue = std::to_string(readbackDirect);
            verified = (readbackDirect == intValue);
            break;
        }
        case LocalVScriptValueKind::Float:
        {
            const float readbackDirect = m_Game->GetConVarFloatDirect(entry.name.c_str(), -999999.0f);
            char buffer[64] = {};
            sprintf_s(buffer, "%.9g", static_cast<double>(readbackDirect));
            directReadbackValue = buffer;
            verified = std::fabs(readbackDirect - floatValue) <= 0.0001f;
            break;
        }
        default:
            directReadbackValue = readbackValue;
            verified = LocalVScriptValuesEquivalent(entry.value, readbackValue);
            break;
        }

        if (!setOk)
        {
            ++writeFailedCount;
            if ((entry.flags & kLocalVScriptFlagCheat) != 0)
            {
                const bool restoredFlags = m_Game->SetConVarFlags(entry.name.c_str(), entry.flags);
            }
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] WriteFail name=%s kind=%s flags=0x%08X before='%s' requested='%s' after='%s' afterDirect='%s' setOk=%d verified=%d",
                entry.name.c_str(),
                GetLocalVScriptValueKindName(kind),
                entry.flags,
                entry.originalValue.c_str(),
                entry.value.c_str(),
                readbackValue.c_str(),
                directReadbackValue.c_str(),
                setOk ? 1 : 0,
                verified ? 1 : 0);
            continue;
        }

        entry.lockProtected = true;
        appliedEntries.push_back(entry);
        if (verified)
            ++appliedCount;
        else
            ++verifyMismatchCount;

        LocalVScriptLog(
            this,
            "[VR][LocalVScriptConvars] Apply name=%s kind=%s flags=0x%08X before='%s' requested='%s' after='%s' afterDirect='%s' setOk=%d verified=%d",
            entry.name.c_str(),
            GetLocalVScriptValueKindName(kind),
            entry.flags,
            entry.originalValue.c_str(),
            entry.value.c_str(),
            readbackValue.c_str(),
            directReadbackValue.c_str(),
            setOk ? 1 : 0,
            verified ? 1 : 0);
    }

    size_t trackedCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        m_LocalVScriptConvars = std::move(appliedEntries);
        m_LocalVScriptConvarsApplied = !m_LocalVScriptConvars.empty();
        m_LocalVScriptConvarBlockedWriteLastLog.clear();
        trackedCount = m_LocalVScriptConvars.size();
    }

    LocalVScriptLog(
        this,
        "[VR][LocalVScriptConvars] Apply done path=%s parsed=%zu applied=%d verifyMismatch=%d writeFailed=%d missing=%d skipped=%d trackedForRestore=%zu",
        m_LocalVScriptConvarsPath.c_str(),
        parsedEntries.size(),
        appliedCount,
        verifyMismatchCount,
        writeFailedCount,
        missingCount,
        skippedCount,
        trackedCount);
}

void VR::AuditLocalVScriptConvarsCurrentValues(const char* reason)
{
    if (!m_Game)
        return;

    std::vector<LocalVScriptConvarEntry> entriesToAudit;
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        if (!m_LocalVScriptConvarsApplied || m_LocalVScriptConvars.empty())
        {
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] Audit skip reason=%s tracked=0",
                reason ? reason : "");
            return;
        }

        entriesToAudit = m_LocalVScriptConvars;
    }

    LocalVScriptLog(
        this,
        "[VR][LocalVScriptConvars] Audit begin reason=%s tracked=%zu",
        reason ? reason : "",
        entriesToAudit.size());

    int matchCount = 0;
    int mismatchCount = 0;
    int missingCount = 0;
    for (const LocalVScriptConvarEntry& entry : entriesToAudit)
    {
        if (entry.name.empty())
            continue;

        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        const LocalVScriptValueKind kind =
            InferLocalVScriptValueKind(entry.value, boolValue, intValue, floatValue);
        const int currentFlags = m_Game->GetConVarFlags(entry.name.c_str());
        if (currentFlags < 0)
        {
            ++missingCount;
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] Audit name=%s kind=%s expected='%s' flags=missing current='<missing>' currentDirect='-' matches=0",
                entry.name.c_str(),
                GetLocalVScriptValueKindName(kind),
                entry.value.c_str());
            continue;
        }

        const std::string currentValue = m_Game->GetConVarString(entry.name.c_str());
        std::string currentDirectValue = "-";
        bool matches = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
        {
            const int currentDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            currentDirectValue = std::to_string(currentDirect);
            matches = (currentDirect == (boolValue ? 1 : 0));
            break;
        }
        case LocalVScriptValueKind::Int:
        {
            const int currentDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            currentDirectValue = std::to_string(currentDirect);
            matches = (currentDirect == intValue);
            break;
        }
        case LocalVScriptValueKind::Float:
        {
            const float currentDirect = m_Game->GetConVarFloatDirect(entry.name.c_str(), -999999.0f);
            char buffer[64] = {};
            sprintf_s(buffer, "%.9g", static_cast<double>(currentDirect));
            currentDirectValue = buffer;
            matches = std::fabs(currentDirect - floatValue) <= 0.0001f;
            break;
        }
        default:
            currentDirectValue = currentValue;
            matches = LocalVScriptValuesEquivalent(entry.value, currentValue);
            break;
        }

        if (matches)
            ++matchCount;
        else
            ++mismatchCount;

        if (!matches)
        {
            LocalVScriptLog(
                this,
                "[VR][LocalVScriptConvars] Audit name=%s kind=%s expected='%s' flags=0x%08X current='%s' currentDirect='%s' matches=0",
                entry.name.c_str(),
                GetLocalVScriptValueKindName(kind),
                entry.value.c_str(),
                currentFlags,
                currentValue.c_str(),
                currentDirectValue.c_str());
        }
    }

    LocalVScriptLog(
        this,
        "[VR][LocalVScriptConvars] Audit done reason=%s tracked=%zu matches=%d mismatches=%d missing=%d",
        reason ? reason : "",
        entriesToAudit.size(),
        matchCount,
        mismatchCount,
        missingCount);
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectoryA(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    FILETIME configLastModified{};
    FILETIME config2LastModified{};
    FILETIME hapticsLastModified{};
    FILETIME localVScriptLastModified{};
    bool localVScriptMissing = false;
    while (1)
    {
        WIN32_FILE_ATTRIBUTE_DATA fileAttributes{};
        WIN32_FILE_ATTRIBUTE_DATA config2Attributes{};
        const bool hasConfig = GetFileAttributesExA("VR\\config.txt", GetFileExInfoStandard, &fileAttributes) != FALSE;
        const bool hasConfig2 = GetFileAttributesExA("VR\\config2.txt", GetFileExInfoStandard, &config2Attributes) != FALSE;
        if (!hasConfig && !hasConfig2)
        {
            m_Game->errorMsg("config.txt/config2.txt not found.");
            return;
        }

        bool configReloadNeeded = false;
        if (hasConfig && CompareFileTime(&fileAttributes.ftLastWriteTime, &configLastModified) != 0)
        {
            configLastModified = fileAttributes.ftLastWriteTime;
            configReloadNeeded = true;
        }
        if (hasConfig2 && CompareFileTime(&config2Attributes.ftLastWriteTime, &config2LastModified) != 0)
        {
            config2LastModified = config2Attributes.ftLastWriteTime;
            configReloadNeeded = true;
        }

        if (configReloadNeeded)
        {
            try
            {
                ParseConfigFile();
            }
            catch (const std::invalid_argument&)
            {
                m_Game->errorMsg("Failed to parse config.txt/config2.txt");
            }
            catch (...)
            {
                m_Game->errorMsg("Failed to parse config.txt/config2.txt");
            }
        }

        WIN32_FILE_ATTRIBUTE_DATA hapticsAttributes{};
        if (GetFileAttributesExA("VR\\haptics_config.txt", GetFileExInfoStandard, &hapticsAttributes))
        {
            if (CompareFileTime(&hapticsAttributes.ftLastWriteTime, &hapticsLastModified) != 0)
            {
                const bool configDidNotReloadThisLoop = !configReloadNeeded;
                hapticsLastModified = hapticsAttributes.ftLastWriteTime;
                if (configDidNotReloadThisLoop)
                {
                    try
                    {
                        ParseHapticsConfigFile();
                    }
                    catch (const std::invalid_argument&)
                    {
                        m_Game->errorMsg("Failed to parse haptics_config.txt");
                    }
                }
            }
        }

        if (!m_LocalVScriptConvarsPath.empty())
        {
            WIN32_FILE_ATTRIBUTE_DATA localVScriptAttributes{};
            if (GetFileAttributesExA(m_LocalVScriptConvarsPath.c_str(), GetFileExInfoStandard, &localVScriptAttributes))
            {
                if (localVScriptMissing ||
                    CompareFileTime(&localVScriptAttributes.ftLastWriteTime, &localVScriptLastModified) != 0)
                {
                    localVScriptLastModified = localVScriptAttributes.ftLastWriteTime;
                    localVScriptMissing = false;
                    m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
                }
            }
            else if (!localVScriptMissing)
            {
                ZeroMemory(&localVScriptLastModified, sizeof(localVScriptLastModified));
                localVScriptMissing = true;
                m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
            }
        }

        if (fileChangeHandle == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        if (!FindNextChangeNotification(fileChangeHandle))
        {
            Sleep(1000);
            continue;
        }
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}
