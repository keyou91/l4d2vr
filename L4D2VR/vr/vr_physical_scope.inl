namespace
{
    Vector ScopeViewmodelPointToScene(const CViewSetup& view, const Vector& point)
    {
        Vector forward{}, right{}, up{};
        QAngle::AngleVectors(QAngle(view.angles.x, view.angles.y, view.angles.z), &forward, &right, &up);
        const float sceneFov = std::clamp(view.fov, 1.0f, 179.0f);
        const float vmFov = std::clamp(view.fovViewmodel, 1.0f, 179.0f);
        const float sceneAspect = view.m_flAspectRatio > 0.001f ? view.m_flAspectRatio :
            static_cast<float>(view.width) / (std::max)(1, view.height);
        const float vmAspect = static_cast<float>(view.width) / (std::max)(1, view.height);
        constexpr float radians = 3.14159265358979323846f / 360.0f;
        const float xRatio = std::tan(sceneFov * radians) / std::tan(vmFov * radians);
        const float yRatio = xRatio * vmAspect / sceneAspect;
        const Vector delta = point - view.origin;
        return view.origin + forward * DotProduct(delta, forward) +
            right * (DotProduct(delta, right) * xRatio) + up * (DotProduct(delta, up) * yRatio);
    }
    D3DMATRIX ScopeD3DMatrix(const VrHandMatrix4& matrix)
    {
        D3DMATRIX output{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) output.m[r][c] = VrHandMath::Get(matrix, c, r);
        return output;
    }
}

void VR::PublishPhysicalScopeLens(const PhysicalScopeLensSnapshot& sample)
{
    const int eye = m_VrHandsActiveEyeIndex;
    const CViewSetup* view = m_VrHandsActiveEyeView;
    if (!IsPhysicalScopeMode() || !m_ScopeEnabled || !HasFreshPhysicalScopeTracking() || !view || eye < 0 || eye > 1 ||
        !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) || m_RenderTpObserver.load(std::memory_order_acquire) ||
        m_RenderPlayerControlledBySI.load(std::memory_order_acquire) ||
        sample.weaponId != m_PhysicalScopeWeaponId.load(std::memory_order_acquire) ||
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire) ||
        !l4d2vr_optics::Finite(sample.center) || !l4d2vr_optics::Finite(sample.forward) ||
        !l4d2vr_optics::Finite(sample.right) || !l4d2vr_optics::Finite(sample.up) ||
        !std::isfinite(sample.radius) || sample.radius <= 0.01f || sample.radius > 8.0f ||
        view->width <= 0 || view->height <= 0 || !std::isfinite(view->fov) ||
        view->fov < 1.0f || view->fov >= 179.0f || !std::isfinite(view->fovViewmodel) ||
        view->fovViewmodel < 1.0f || view->fovViewmodel >= 179.0f) return;
    PhysicalScopeLensSnapshot lens = sample;
    lens.sceneCenter = ScopeViewmodelPointToScene(*view, lens.center);
    lens.sceneForward = ScopeViewmodelPointToScene(*view, lens.center + lens.forward) - lens.sceneCenter;
    lens.sceneUp = ScopeViewmodelPointToScene(*view, lens.center + lens.up) - lens.sceneCenter;
    const Vector sceneEdge = ScopeViewmodelPointToScene(*view, lens.center + lens.right * lens.radius);
    lens.sceneRadius = (sceneEdge - lens.sceneCenter).Length();
    if (!l4d2vr_optics::Finite(lens.sceneCenter) || !l4d2vr_optics::Normalize(lens.sceneForward) ||
        !l4d2vr_optics::Normalize(lens.sceneUp)) return;
    lens.magnification = m_PhysicalScopeMagnification.load(std::memory_order_acquire);
    lens.trackingPose.origin = GetRightControllerAbsPos();
    QAngle::AngleVectors(GetRightControllerAbsAngle(),&lens.trackingPose.forward,&lens.trackingPose.right,&lens.trackingPose.up);
    if (!l4d2vr_optics::Valid(lens.trackingPose)) return;
    lens.publishedAt = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    if (!m_PhysicalScopeWeapons.Current().Valid() || m_PhysicalScopeWeapons.Current().id != lens.weaponId) return;
    lens.weapon = m_PhysicalScopeWeapons.Current();
    m_PhysicalScopeLenses[eye] = lens;
}

bool VR::ReadPhysicalScopeLens(int eye, PhysicalScopeLensSnapshot& lens)
{
    if (eye < 0 || eye > 1) return false;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    lens = m_PhysicalScopeLenses[eye];
    const uint64_t now = GetTickCount64();
    return l4d2vr_optics::Fresh(now,lens.publishedAt) &&
        lens.weapon.Valid() && lens.weapon == m_PhysicalScopeWeapons.Current();
}

bool VR::HasFreshPhysicalScopeTracking() const
{
    VRWorldPoseTrackingSnapshot pose{};
    if (!ReadWorldPoseTrackingSnapshot(pose) || !pose.hmdValid ||
        !l4d2vr_optics::Fresh(GetTickCount64(), pose.capturedAtMs)) return false;
    const bool left = IsGameplayHandLeftPhysical(false);
    const auto finiteAngles = [](const QAngle& angles) {
        return std::isfinite(angles.x) && std::isfinite(angles.y) && std::isfinite(angles.z);
    };
    return (left ? pose.leftHandValid : pose.rightHandValid) &&
        l4d2vr_optics::Finite(pose.hmdPosition) && finiteAngles(pose.hmdAngles) &&
        l4d2vr_optics::Finite(left ? pose.leftHandPosition : pose.rightHandPosition) &&
        finiteAngles(left ? pose.leftHandAngles : pose.rightHandAngles);
}

bool VR::ReadPhysicalScopeRender(PhysicalScopeRenderSnapshot& snapshot)
{
    if (!HasFreshPhysicalScopeTracking()) return false;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    snapshot = m_PhysicalScopeRender;
    return snapshot.weapon.Valid() && snapshot.weapon == m_PhysicalScopeWeapons.Current() &&
        l4d2vr_optics::Finite(snapshot.origin) &&
        std::isfinite(snapshot.angles.x) && std::isfinite(snapshot.angles.y) && std::isfinite(snapshot.angles.z) &&
        l4d2vr_optics::ValidRenderFov(snapshot.fov) &&
        std::isfinite(snapshot.magnification) && snapshot.magnification >= 1.0f && snapshot.magnification <= 16.0f &&
        l4d2vr_optics::Fresh(GetTickCount64(), snapshot.publishedAt);
}

void VR::PublishPhysicalScopeImage(const PhysicalScopeRenderSnapshot& snapshot, uint32_t renderEpoch)
{
    if (m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != renderEpoch) return;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    const uint64_t now = GetTickCount64();
    if (!snapshot.weapon.Valid())
    {
        // Legacy scopes render into the same surface. Their completion must
        // invalidate physical stamps rather than leave the previous label live.
        m_PhysicalScopeImage = {}; m_PhysicalScopeProcessedImage = {};
        return;
    }
    if (!(snapshot.weapon == m_PhysicalScopeWeapons.Current()) ||
        !l4d2vr_optics::Fresh(now, snapshot.publishedAt)) return;
    m_PhysicalScopeImage = {snapshot.weapon, snapshot.magnification, now, renderEpoch};
}

l4d2vr_optics::ImageStamp VR::ReadPhysicalScopeImage(bool processed)
{
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    return processed ? m_PhysicalScopeProcessedImage : m_PhysicalScopeImage;
}

void VR::PublishPhysicalScopeProcessedImage(const l4d2vr_optics::ImageStamp& source)
{
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    if (source.weapon.Valid() && source.weapon == m_PhysicalScopeWeapons.Current() &&
        source.weapon == m_PhysicalScopeImage.weapon && source.completedAt == m_PhysicalScopeImage.completedAt &&
        source.magnification == m_PhysicalScopeImage.magnification &&
        source.renderEpoch == m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire))
        m_PhysicalScopeProcessedImage = source;
    else m_PhysicalScopeProcessedImage = {};
}

void VR::UpdatePhysicalScope()
{
    m_ScopeActive = false;
    const float magnification = l4d2vr_optics::Magnification(m_ScopeFov, m_ScopeFovMax, m_ScopeBaseMagnification);
    m_PhysicalScopeMagnification.store(magnification, std::memory_order_release);
    if (!m_ScopeEnabled || !ScopeWeaponEligible() || !m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) || m_RenderPlayerControlledBySI.load(std::memory_order_acquire) ||
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire)) return;
    if (!HasFreshPhysicalScopeTracking()) return;
    PhysicalScopeLensSnapshot lens{};
    if (!ReadPhysicalScopeLens(0, lens) && !ReadPhysicalScopeLens(1, lens)) return;
    l4d2vr_optics::Pose current{};
    current.origin = GetRightControllerAbsPos();
    QAngle::AngleVectors(GetRightControllerAbsAngle(),&current.forward,&current.right,&current.up);
    Vector center(0,0,0), forward(0,0,0), up(0,0,0);
    if (!l4d2vr_optics::RebasePoint(lens.sceneCenter,lens.trackingPose,current,center) ||
        !l4d2vr_optics::RebaseVector(lens.sceneForward,lens.trackingPose,current,forward) ||
        !l4d2vr_optics::RebaseVector(lens.sceneUp,lens.trackingPose,current,up)) return;
    lens.sceneCenter = center; lens.sceneForward = forward; lens.sceneUp = up;
    auto left = l4d2vr_optics::EvaluateEye(GetViewOriginLeft(), lens.sceneCenter, lens.sceneForward, lens.sceneRadius, m_VRScale, magnification);
    auto right = l4d2vr_optics::EvaluateEye(GetViewOriginRight(), lens.sceneCenter, lens.sceneForward, lens.sceneRadius, m_VRScale, magnification);
    const auto eye = left.alpha >= right.alpha ? left : right;
    if (eye.alpha <= 0.0f) return;
    m_PhysicalScopeRenderFov = l4d2vr_optics::RenderFov(lens.sceneRadius, eye.relief, magnification);
    if (m_PhysicalScopeRenderFov <= 0.0f) return;
    m_ScopeCameraPosAbs = lens.sceneCenter;
    QAngle::VectorAngles(lens.sceneForward, lens.sceneUp, m_ScopeCameraAngAbs);
    {
        std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
        if (!(lens.weapon == m_PhysicalScopeWeapons.Current())) return;
        m_PhysicalScopeRender = {lens.weapon, m_ScopeCameraPosAbs, m_ScopeCameraAngAbs,
            m_PhysicalScopeRenderFov, magnification, GetTickCount64()};
    }
    m_ScopeActive = true;
}

bool VR::DrawPhysicalScopeLensForEye(const CViewSetup& view, int eyeIndex, const PhysicalScopeLensSnapshot& lens)
{
    if (!IsPhysicalScopeMode() || !m_ScopeEnabled || !HasFreshPhysicalScopeTracking() || !g_D3DVR9 || eyeIndex < 0 || eyeIndex > 1 ||
        !l4d2vr_optics::Fresh(GetTickCount64(),lens.publishedAt) ||
        lens.weaponId != m_PhysicalScopeWeaponId.load(std::memory_order_acquire) ||
        !l4d2vr_optics::Finite(lens.center) || !l4d2vr_optics::Finite(lens.right) || !l4d2vr_optics::Finite(lens.up) ||
        !std::isfinite(lens.radius) || lens.radius <= 0.0f ||
        view.width <= 0 || view.height <= 0 || !std::isfinite(view.fovViewmodel) ||
        view.fovViewmodel < 1.0f || view.fovViewmodel >= 179.0f ||
        !l4d2vr_optics::Finite(view.origin) || !l4d2vr_optics::Finite(view.angles) ||
        !l4d2vr_optics::ValidClipPlanes(view.zNearViewmodel, view.zFarViewmodel)) return false;
    l4d2vr_optics::ImageStamp scopeImage{};
    l4d2vr_optics::ImageStamp processedImage{};
    {
        std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
        if (!lens.weapon.Valid() || !(lens.weapon == m_PhysicalScopeWeapons.Current())) return false;
        scopeImage = m_PhysicalScopeImage;
        processedImage = m_PhysicalScopeProcessedImage;
    }
    struct DeviceLock
    {
        bool locked = false;
        explicit DeviceLock(bool required)
        { if (required && g_D3DVR9) locked = SUCCEEDED(g_D3DVR9->LockDevice()); }
        ~DeviceLock() { if (locked && g_D3DVR9) g_D3DVR9->UnlockDevice(); }
    } deviceLock(m_Game && m_Game->GetMatQueueMode() != 0);
    if (m_Game && m_Game->GetMatQueueMode() != 0 && !deviceLock.locked) return false;
    std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
    IDirect3DSurface9* surface = eyeIndex == 0 ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (m_CreatingTextureID != Texture_None || !surface) return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device) return false;
    struct Restore
    {
        IDirect3DDevice9* device;
        IDirect3DStateBlock9* state = nullptr;
        IDirect3DSurface9* target = nullptr;
        IDirect3DSurface9* depth = nullptr;
        D3DVIEWPORT9 viewport{};
        bool haveViewport = false;
        bool captured = false;
        bool restored = false;
        bool RestoreState()
        {
            if (restored) return true;
            restored = true;
            bool ok = true;
            // SetRenderTarget resets viewport and scissor state. Restore it
            // before applying the captured pipeline, including its scissor rect.
            if (target) ok = SUCCEEDED(device->SetRenderTarget(0, target)) && ok;
            if (state && captured) ok = SUCCEEDED(state->Apply()) && ok;
            if (haveViewport) ok = SUCCEEDED(device->SetViewport(&viewport)) && ok;
            return ok;
        }
        ~Restore()
        {
            (void)RestoreState();
            if (state) state->Release();
            if (target) target->Release();
            if (depth) depth->Release();
            device->Release();
        }
    } restore{device};
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &restore.state)) || !restore.state ||
        !(restore.captured = SUCCEEDED(restore.state->Capture())) || FAILED(device->GetRenderTarget(0, &restore.target)) ||
        FAILED(device->GetDepthStencilSurface(&restore.depth)) || !restore.depth) return false;
    restore.haveViewport = SUCCEEDED(device->GetViewport(&restore.viewport));
    if (!restore.haveViewport || FAILED(device->SetRenderTarget(0, surface))) return false;
    D3DVIEWPORT9 viewport{0,0,static_cast<DWORD>(view.width),static_cast<DWORD>(view.height),0.0f,1.0f};
    if (FAILED(device->SetViewport(&viewport))) return false;
    const D3DMATRIX world = ScopeD3DMatrix(VrHandMath::Identity());
    const D3DMATRIX camera = ScopeD3DMatrix(VrHandMath::BuildSourceView(view.origin, view.angles));
    const D3DMATRIX projection = ScopeD3DMatrix(VrHandMath::BuildPerspective(view.fovViewmodel,
        static_cast<float>(view.width) / view.height, view.zNearViewmodel, view.zFarViewmodel));
    struct Vertex { Vector position; D3DCOLOR color; float u, v; };
    std::array<Vertex,66> vertices{};
    vertices[0] = {lens.center,D3DCOLOR_ARGB(255,255,255,255),0.5f,0.5f};
    for (size_t i = 1; i < vertices.size(); ++i)
    {
        const float angle = static_cast<float>(i - 1) * (2.0f * 3.14159265358979323846f / 64.0f);
        const float x = std::cos(angle), y = std::sin(angle);
        vertices[i] = {lens.center + lens.right * (x * lens.radius) + lens.up * (y * lens.radius),
            D3DCOLOR_ARGB(255,255,255,255),0.5f + x * 0.49f,0.5f - y * 0.49f};
    }
    bool ok = SUCCEEDED(device->SetVertexShader(nullptr)) && SUCCEEDED(device->SetPixelShader(nullptr)) &&
        SUCCEEDED(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)) &&
        SUCCEEDED(device->SetTransform(D3DTS_WORLD,&world)) && SUCCEEDED(device->SetTransform(D3DTS_VIEW,&camera)) &&
        SUCCEEDED(device->SetTransform(D3DTS_PROJECTION,&projection));
    // Establish the fixed-function state instead of inheriting native material
    // blending, generated UVs, wireframe fill or a prior scissor test.
    ok = ok && SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,TRUE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CLIPPING,m_VRNearClipSelfBody ? FALSE : TRUE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CLIPPLANEENABLE,0u)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_DISABLE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_LIGHTING,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_FOGENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_STENCILENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE,15u)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_WRAP0,0u)) &&
        SUCCEEDED(device->SetTexture(0,nullptr)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0u)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_TEXTUREFACTOR,D3DCOLOR_ARGB(255,0,0,0))) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    if (ok) ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,64,vertices.data(),sizeof(Vertex)));
    const auto eye = l4d2vr_optics::EvaluateEye(view.origin, lens.sceneCenter, lens.sceneForward,
        lens.sceneRadius, m_VRScale, lens.magnification);
    IDirect3DTexture9* rawTexture = nullptr;
    if (m_D9ScopeSurface)
        m_D9ScopeSurface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&rawTexture));
    IDirect3DTexture9* texture = rawTexture;
    if (!texture && m_ScopeLensOverlayReady.load(std::memory_order_acquire))
    {
        texture = m_D9ScopeLensTexture;
        scopeImage = processedImage;
    }
    if (ok && eye.alpha > 0.0f && texture &&
        l4d2vr_optics::ImageMatches(scopeImage, lens.weapon, lens.magnification,
            m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire), GetTickCount64()))
    {
        const auto brightness = static_cast<unsigned char>(eye.alpha * 255.0f);
        for (auto& vertex : vertices) vertex.color = D3DCOLOR_ARGB(255,brightness,brightness,brightness);
        ok = SUCCEEDED(device->SetTexture(0,texture)) &&
            SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE)) &&
            SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE)) &&
            SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        if (ok) ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,64,vertices.data(),sizeof(Vertex)));
        if (ok && rawTexture && m_ScopeReticleAlpha > 0.0f)
        {
            const Vector center = lens.center - lens.forward * 0.005f;
            const float extent = lens.radius * 0.25f;
            const auto opacity = static_cast<unsigned char>(std::clamp(m_ScopeReticleAlpha,0.0f,1.0f) * 255.0f);
            const D3DCOLOR color = D3DCOLOR_ARGB(opacity,0,0,0);
            const Vertex reticle[] = {
                {center-lens.right*extent,color,0,0},{center+lens.right*extent,color,0,0},
                {center-lens.up*extent,color,0,0},{center+lens.up*extent,color,0,0}};
            ok = SUCCEEDED(device->SetTexture(0,nullptr)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA));
            if (ok) ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_LINELIST,2,reticle,sizeof(Vertex)));
        }
    }
    if (rawTexture) rawTexture->Release();
    const bool restored = restore.RestoreState();
    return ok && restored;
}
