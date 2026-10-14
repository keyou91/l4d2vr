namespace
{
    bool PistolAmmoBackendReady(const Game* game)
    {
        return game && game->m_VR && game->m_VR->m_DualPistolsIndependentHandsEnabled &&
            game->m_ServerPluginHelpers && Hooks::hkPistolPlayerRunCommand.isEnabled && Hooks::hkPistolGunFire.isEnabled &&
            game->m_Offsets && game->m_Offsets->PhysicalShellActiveHandle.valid &&
            game->m_Offsets->GetActiveWeapon.valid && game->m_Offsets->CBaseEntity_entindex.valid;
    }
    bool ReadPistolAmmoRecipient(Game* game, int index, const l4d2vr_pistol::AmmoSnapshot& snapshot,
        edict_t*& entity, uint32_t& handle)
    {
        if (!PistolAmmoBackendReady(game) || index <= 0 || !game->IsValidPlayerIndex(index) || !snapshot.Valid()) return false;
#ifdef _MSC_VER
        __try
        {
#endif
            auto* owner = reinterpret_cast<unsigned char*>(snapshot.owner);
            auto* weapon = reinterpret_cast<unsigned char*>(snapshot.weapon);
            entity = *reinterpret_cast<edict_t**>(owner + 0x28);
            auto* weaponEntity = *reinterpret_cast<edict_t**>(weapon + 0x28);
            using Active = void* (__thiscall*)(void*);
            int ownerIndex = -1, weaponIndex = -1; unsigned ownerSerial = 0u, weaponSerial = 0u;
            if (!game->ReadServerEdictIdentity(entity, ownerIndex, ownerSerial, owner) ||
                !game->ReadServerEdictIdentity(weaponEntity, weaponIndex, weaponSerial, weapon) || ownerIndex != index ||
                ownerSerial != snapshot.ownerSerial || weaponSerial != snapshot.weaponSerial ||
                *reinterpret_cast<const int*>(weapon + 0x1414) != snapshot.clip ||
                reinterpret_cast<Active>(game->m_Offsets->GetActiveWeapon.address)(owner) != weapon) return false;
            const uint32_t nativeHandle = *reinterpret_cast<uint32_t*>(owner + 0x19d4);
            handle = ((nativeHandle >> 12) & 0x3ffu) << 12 | (nativeHandle & 0xfffu);
            return nativeHandle != 0xffffffffu && l4d2vr_shell::ValidHandle(handle) &&
                (handle & 0xfffu) == static_cast<unsigned>(weaponIndex);
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
    }
}

void Game::ResetPistolAmmoServerClients()
{
    std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
    m_PistolAmmoServerClients.fill(PistolAmmoServerClient{});
}
void Game::OfferPistolAmmoProtocol(edict_t* entity)
{
    if (!PistolAmmoBackendReady(this)) return;
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return;
    uint32_t token = 0u;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (client.entity != entity || client.serial != serial || !client.sender.Token())
        {
            const uint32_t executed = client.entity == entity && client.serial == serial ? client.executedCommand : 0u;
            static std::atomic<uint32_t> next{(static_cast<uint32_t>(GetTickCount64()) ^ GetCurrentProcessId()) | 1u};
            token = next.fetch_add(1u, std::memory_order_relaxed);
            if (!token) token = next.fetch_add(1u, std::memory_order_relaxed);
            client = {}; client.entity = entity; client.serial = serial; client.sender.Reset(token); client.reload.Reset(token);
            client.executedCommand = executed;
        }
        token = client.sender.Token();
    }
    char command[96]{};
    std::snprintf(command, sizeof(command), "l4d2vr_pistol_ammo_cap %u %u\n", l4d2vr_pistol_sync::kVersion, token);
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, command);
}
void Game::PublishPistolAmmoState(int index, const l4d2vr_pistol::AmmoSnapshot& snapshot, int command)
{
    if (command <= 0) return;
    edict_t* entity = nullptr; uint32_t handle = 0u;
    if (!ReadPistolAmmoRecipient(this, index, snapshot, entity, handle)) return;
    l4d2vr_pistol_sync::State state{};
    state.handle = handle; state.command = static_cast<uint32_t>(command);
    state.capacity = snapshot.capacity; state.clip = snapshot.clip; state.dual = snapshot.dual;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoMutex);
        state.known = m_PistolAmmo[index].Counts(snapshot, state.right, state.left);
        l4d2vr_pistol::MagazineState magazines{};
        if (state.known && m_PistolAmmo[index].MagazineInfo(snapshot, magazines))
        {
            state.physical = magazines.physical; state.attached = magazines.attached; state.chambered = magazines.chambered;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (client.entity != entity || client.serial != snapshot.ownerSerial) return;
        state.reloadSequence = client.reloadSequence;
        if (!client.sender.Prepare(state, GetTickCount64(), state)) return;
    }
    char response[192]{};
    std::snprintf(response, sizeof(response), "l4d2vr_pistol_ammo_state %u %u %u %u %d %d %d %d %u %u %u %u %u %u\n",
        state.token, state.sequence, state.handle, state.command, state.capacity, state.clip, state.right, state.left,
        state.known ? 1u : 0u, state.dual ? 1u : 0u, state.physical, state.attached, state.chambered, state.reloadSequence);
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, response);
}

bool Game::HandlePistolAmmoCommand(edict_t* entity, const void* sourceCommand)
{
    VRPoseRelayCommandView view{};
    if (!VRPoseRelayReadCommand(sourceCommand, view) || std::strcmp(view.name, "l4d2vr_pistol_ammo_ack") != 0) return false;
    const auto& command = *static_cast<const SourceCCommand*>(sourceCommand);
    uint32_t version = 0u, token = 0u;
    if (command.ArgC() != 3 || !l4d2vr_shell::ParseNumber(command.Arg(1), 255u, version) ||
        !l4d2vr_shell::ParseNumber(command.Arg(2), 0xffffffffu, token)) return true;
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return true;
    bool newlyReady = false;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (client.entity == entity && client.serial == serial)
        {
            const bool wasReady = client.sender.Ready();
            const bool acknowledged = client.sender.Acknowledge(version, token);
            if (acknowledged && version == 0u)
            {
                client.sender.Reset(0u); client.reload.Reset(0u);
                client.havePending = false; client.reloadSequence = 0u;
            }
            newlyReady = client.sender.Ready() && !wasReady;
        }
    }
    if (newlyReady) Game::logMsg("[VR][PistolAmmo][server] player=%d protocol=%u acknowledged", index, version);
    return true;
}

bool Game::PistolAmmoClientReady(int index, unsigned ownerSerial)
{
    if (index <= 0 || !IsValidPlayerIndex(index)) return false;
    std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
    const auto& client = m_PistolAmmoServerClients[index];
    return client.entity && client.serial == ownerSerial && client.sender.Ready();
}

bool Game::HandlePistolMagazineCommand(edict_t* entity, const void* sourceCommand)
{
    VRPoseRelayCommandView view{};
    if (!VRPoseRelayReadCommand(sourceCommand, view) || std::strcmp(view.name, "l4d2vr_pistol_mag_v1") != 0) return false;
    const auto& command = *static_cast<const SourceCCommand*>(sourceCommand);
    uint32_t values[8]{};
    constexpr uint32_t limits[8] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u, 0x7fffffffu, 30u, 5000u, 2u, 3u};
    if (command.ArgC() != 9) return true;
    for (int arg = 1; arg <= 8; ++arg)
        if (!l4d2vr_shell::ParseNumber(command.Arg(arg), limits[arg - 1], values[arg - 1])) return true;
    const l4d2vr_pistol_reload::Request request{values[0], values[1], values[2], values[3],
        static_cast<int>(values[4]), static_cast<int>(values[5]),
        static_cast<l4d2vr_pistol::Hand>(values[6]), static_cast<l4d2vr_pistol::MagazineAction>(values[7])};
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return true;
    std::uint32_t executed = 0u;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (client.entity != entity || client.serial != serial || !client.sender.Ready() ||
            request.token != client.sender.Token()) return true;
        const uint64_t now = GetTickCount64();
        if (client.havePending && now >= client.queuedAt && now - client.queuedAt <= 1500u) return true;
        client.pending = request; client.havePending = true; client.queuedAt = GetTickCount64();
        executed = client.executedCommand;
    }
    if (request.command <= executed) ProcessPistolMagazineRequests(index, static_cast<int>(executed), serial);
    return true;
}

bool Game::RelinquishPistolPhysicalReload(int index, const l4d2vr_pistol::AmmoSnapshot& snapshot, int command)
{
    edict_t* entity = nullptr;
    uint32_t handle = 0u;
    if (command <= 0 || !ReadPistolAmmoRecipient(this, index, snapshot, entity, handle) ||
        !l4d2vr_shell::HandleMatches(handle, static_cast<int>(handle & 0xfffu), snapshot.weaponSerial))
        return false;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (client.entity != entity || client.serial != snapshot.ownerSerial || !client.sender.Ready() ||
            static_cast<uint32_t>(command) <= client.executedCommand)
            return false;
        std::lock_guard<std::mutex> ammoLock(m_PistolAmmoMutex);
        if (!m_PistolAmmo[index].RelinquishPhysical(snapshot)) return false;
        // Revoke pending requests and replay results before offering a new token.
        // A late same-clip Cycle must not restore the relinquished ownership.
        client.sender.Reset(0u);
        client.reload.Reset(0u);
        client.havePending = false;
        client.pending = {};
        client.queuedAt = 0u;
        client.reloadSequence = 0u;
        client.executedCommand = std::max(client.executedCommand, static_cast<uint32_t>(command));
    }
    OfferPistolAmmoProtocol(entity);
    Game::logMsg("[VR][PistolAmmo][server] native reload relinquished physical ownership player=%d command=%d handle=0x%X",
        index, command, handle);
    return true;
}

void Game::ProcessPistolMagazineRequests(int index, int command, unsigned ownerSerial)
{
    if (index <= 0 || !IsValidPlayerIndex(index) || command <= 0) return;
    edict_t* entity = nullptr;
    l4d2vr_pistol_reload::Request request{};
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (!client.entity || client.serial != ownerSerial) return;
        client.executedCommand = std::max(client.executedCommand, static_cast<uint32_t>(command));
        if (static_cast<uint32_t>(command) < client.executedCommand) return;
        const uint64_t now = GetTickCount64();
        expired = now < client.queuedAt || now - client.queuedAt > 1500u;
        if (!client.havePending || (client.pending.command > client.executedCommand && !expired)) return;
        request = client.pending; client.havePending = false; entity = client.entity;
    }
    l4d2vr_pistol_reload::Reply reply{request, l4d2vr_shell::Status::Stale, {}};
    l4d2vr_shell::Snapshot nativeAmmo{}; RemoteShellNativeState native{};
    bool unsupported = false;
    const l4d2vr_shell::Request nativeRequest{request.token, request.sequence, request.handle, request.command, 1, request.clip, request.reserve};
    const bool eligible = !expired && ReadRemoteShellNative(this, entity, index, nativeRequest, nativeAmmo, native, unsupported, true, true);
    l4d2vr_pistol::AmmoSnapshot ammo{};
    if (eligible)
        ammo = {reinterpret_cast<uintptr_t>(native.playerObject), reinterpret_cast<uintptr_t>(native.weaponObject),
            ownerSerial, static_cast<unsigned>(native.weapon->m_NetworkSerialNumber), nativeAmmo.clip,
            native.dualPistol, nativeAmmo.capacity};
    const l4d2vr_pistol_reload::Snapshot snapshot{eligible && ammo.Valid() && PistolAmmoBackendReady(this) &&
        m_VR && m_VR->m_MagazineInteractionEnabled && GetConVarIntDirect("sv_infinite_ammo", -1) == 0,
        nativeAmmo.handle, static_cast<uint32_t>(command), ammo, nativeAmmo.reserve, native.infinite};
    int settledClip = nativeAmmo.clip;
    {
        std::lock_guard<std::mutex> lock(m_PistolAmmoServerMutex);
        auto& client = m_PistolAmmoServerClients[index];
        if (client.entity != entity || client.serial != ownerSerial ||
            !client.sender.Ready() || client.sender.Token() != request.token) return;
        if (!expired)
        {
            std::lock_guard<std::mutex> ammoLock(m_PistolAmmoMutex);
            reply = client.reload.Apply(request, snapshot, GetTickCount64(), m_PistolAmmo[index],
                [&](int clip, int reserve)
                {
                    const bool ok = WriteRemoteShellNative(native, clip, reserve);
                    if (ok) settledClip = clip;
                    return ok;
                });
            if (reply.status == l4d2vr_shell::Status::Applied)
                client.reloadSequence = std::max(client.reloadSequence, request.sequence);
        }
    }
    char response[256]{};
    std::snprintf(response, sizeof(response), "l4d2vr_pistol_mag_result %u %u %u %u %d %d %u %u %u %d %d %d %d %d %u %u %u\n",
        request.token, request.sequence, request.handle, request.command, request.clip, request.reserve,
        static_cast<unsigned>(request.hand), static_cast<unsigned>(request.action), static_cast<unsigned>(reply.status),
        std::max(0, reply.result.clip), std::max(0, reply.result.reserve), reply.result.removed, reply.result.added,
        reply.result.released, reply.result.state.physical, reply.result.state.attached, reply.result.state.chambered);
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, response);
    Game::logMsg("[VR][PistolMagazine][server] player=%d command=%u seq=%u hand=%u action=%u status=%u clip=%d reserve=%d",
        index, request.command, request.sequence, static_cast<unsigned>(request.hand), static_cast<unsigned>(request.action),
        static_cast<unsigned>(reply.status), reply.result.clip, reply.result.reserve);
    if (reply.status == l4d2vr_shell::Status::Applied)
    {
        ammo.clip = settledClip;
        PublishPistolAmmoState(index, ammo, command);
    }
}
