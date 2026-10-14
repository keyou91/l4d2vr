// Included after game_remote_shells.inl to share the verified native ammo adapter.
void Game::ResetRemoteMagazineServerClients()
{
    std::lock_guard<std::mutex> lock(m_RemoteMagazineServerMutex);
    m_RemoteMagazineServerClients.fill(RemoteMagazineServerClient{});
}

void Game::OfferRemoteMagazineProtocol(edict_t* entity)
{
    if (!RemoteMagazineBackendReady(this)) return;
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return;
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(m_RemoteMagazineServerMutex);
        auto& client = m_RemoteMagazineServerClients[index];
        if (client.entity != entity || client.serial != serial || !client.ledger.Token())
        {
            static std::atomic<uint32_t> next{
                (static_cast<uint32_t>(GetTickCount64()) ^ (GetCurrentProcessId() << 16)) | 1u};
            token = next.fetch_add(1u, std::memory_order_relaxed);
            if (!token) token = next.fetch_add(1u, std::memory_order_relaxed);
            client = {}; client.entity = entity; client.serial = serial; client.ledger.Reset(token);
        }
        token = client.ledger.Token();
    }
    char command[96]{};
    std::snprintf(command, sizeof(command), "l4d2vr_mag_capability %u %u\n", l4d2vr_remote_mag::kVersion, token);
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, command);
}

bool Game::HandleRemoteMagazineCommand(edict_t* entity, const void* sourceCommand)
{
    VRPoseRelayCommandView view{};
    if (!VRPoseRelayReadCommand(sourceCommand, view) || std::strcmp(view.name, "l4d2vr_mag_action_v2") != 0)
        return false;
    const auto& command = *static_cast<const SourceCCommand*>(sourceCommand);
    if (command.ArgC() != 9) return true;
    uint32_t values[8]{};
    constexpr uint32_t limits[8] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u,
        0x7fffffffu, 64u, l4d2vr_remote_mag::kMaxClip, 5000u, 2u};
    for (int arg = 1; arg <= 8; ++arg)
        if (!l4d2vr_shell::ParseNumber(command.Arg(arg), limits[arg - 1], values[arg - 1])) return true;
    l4d2vr_remote_mag::Request request{values[0], values[1], values[2], values[3],
        static_cast<int>(values[4]), static_cast<int>(values[5]), static_cast<int>(values[6]),
        static_cast<l4d2vr_remote_mag::Action>(values[7])};
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return true;
    l4d2vr_remote_mag::Reply reply{};
    {
        std::lock_guard<std::mutex> lock(m_RemoteMagazineServerMutex);
        auto& client = m_RemoteMagazineServerClients[index];
        if (client.entity != entity || client.serial != serial || !client.ledger.Token() ||
            request.token != client.ledger.Token()) return true;
        l4d2vr_shell::Request ammoRequest{request.token, request.sequence, request.handle, request.command,
            request.weaponId, request.clip, request.reserve};
        l4d2vr_shell::Snapshot ammo{}; RemoteShellNativeState native{};
        bool unsupported = false;
        ReadRemoteShellNative(this, entity, index, ammoRequest, ammo, native, unsupported, true);
        l4d2vr_remote_mag::Snapshot snapshot{ammo.eligible, ammo.handle, ammo.latestCommand,
            ammo.weaponId, ammo.clip, ammo.reserve, ammo.capacity, native.infinite};
        reply = unsupported ? client.ledger.Unsupported(request) :
            client.ledger.Apply(request, snapshot, GetTickCount64(),
                [&](int clip, int reserve) { return WriteRemoteShellNative(native, clip, reserve); });
    }
    char response[192]{};
    std::snprintf(response, sizeof(response), "l4d2vr_mag_result %u %u %u %u %d %d %d %u %u %d %d\n",
        request.token, request.sequence, request.handle, request.command, request.weaponId, request.clip, request.reserve,
        static_cast<unsigned>(request.action), static_cast<unsigned>(reply.status),
        std::max(0, reply.clip), std::max(0, reply.reserve));
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, response);
    Game::logMsg("[VR][RemoteMagazine][server] player=%d seq=%u action=%u status=%u weaponId=%d clip=%d reserve=%d",
        index, request.sequence, static_cast<unsigned>(request.action), static_cast<unsigned>(reply.status),
        request.weaponId, reply.clip, reply.reserve);
    return true;
}
