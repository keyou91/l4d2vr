// Included by game.cpp after the private Source command/edict relay helpers.
#include "sdk/sdk_server.h"

namespace
{
    bool RemoteShellBackendReady(const Game* game)
    {
        return game && game->m_Offsets && Hooks::hkPhysicalShotgunReload.isEnabled &&
            game->m_Offsets->PhysicalShellAmmoCount.valid && game->m_Offsets->PhysicalShellMaxClip.valid &&
            game->m_Offsets->PhysicalShellEdictAccessor.valid && game->m_Offsets->PhysicalGunOwner.valid &&
            game->m_Offsets->PhysicalShellReloadLayout.valid &&
            game->m_Offsets->PhysicalShellReloadState.valid && game->m_Offsets->PistolRemoveDualWeapons.valid &&
            game->m_Offsets->PhysicalShellActiveHandle.valid &&
            game->m_Offsets->PhysicalShotgunReload.valid && game->m_Offsets->GetActiveWeapon.valid &&
            game->m_Offsets->CBaseEntity_entindex.valid;
    }
    bool RemoteMagazineBackendReady(const Game* game)
    {
        return RemoteShellBackendReady(game) && Hooks::hkPhysicalGunReload.isEnabled &&
            game->m_Offsets->PhysicalGunReload.valid && game->m_Offsets->PhysicalAmmoInfinite.valid;
    }
    bool ReadNativeInfiniteAmmo(const Game* game, int ammoType, bool& infinite)
    {
        // The complete GetAmmoCount signature proves these two relative calls.
        // Validate the getter and predicate before calling this supported ABI.
        auto* code = reinterpret_cast<const unsigned char*>(game->m_Offsets->PhysicalShellAmmoCount.address);
        if (code[24] != 0xe8 || code[31] != 0xe8) return false;
        auto* getter = code + 29 + *reinterpret_cast<const int*>(code + 25);
        const auto* predicate = code + 36 + *reinterpret_cast<const int*>(code + 32);
        const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleA("server.dll"));
        if (!module) return false;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
        const auto end = module + pe->OptionalHeader.SizeOfImage;
        const auto address = reinterpret_cast<uintptr_t>(getter);
        if (address < module || address >= end || end - address < 6 || getter[0] != 0xb8 || getter[5] != 0xc3 ||
            predicate != reinterpret_cast<const unsigned char*>(game->m_Offsets->PhysicalAmmoInfinite.address)) return false;
        using GetDefinition = void* (__cdecl*)();
        using IsInfinite = bool (__thiscall*)(void*, int);
        void* definition = reinterpret_cast<GetDefinition>(getter)();
        if (!definition) return false;
        infinite = reinterpret_cast<IsInfinite>(game->m_Offsets->PhysicalAmmoInfinite.address)(definition, ammoType);
        return true;
    }
    bool RemoteShellWritable(const void* address, size_t length)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
            return false;
        const auto begin = reinterpret_cast<uintptr_t>(address);
        const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        return begin < end && length <= end - begin;
    }
    struct RemoteShellNativeState
    {
        int* clip = nullptr;
        int* reserve = nullptr;
        edict_t* player = nullptr;
        edict_t* weapon = nullptr;
        unsigned short* playerChangeSerial = nullptr;
        unsigned short* weaponChangeSerial = nullptr;
        unsigned char* inReload = nullptr;
        int* reloadState = nullptr;
        unsigned char oldInReload = 0;
        int oldReloadState = 0;
        int oldClip = 0, oldReserve = 0;
        bool infinite = false;
        void* playerObject = nullptr;
        void* weaponObject = nullptr;
        bool dualPistol = false;
    };
    bool ReadRemoteShellNative(Game* game, edict_t* entity, int index,
        const l4d2vr_shell::Request& request, l4d2vr_shell::Snapshot& state, RemoteShellNativeState& native,
        bool& unsupported, bool magazine = false, bool pistol = false)
    {
        unsupported = magazine ? !RemoteMagazineBackendReady(game) : !RemoteShellBackendReady(game);
        if (unsupported || !entity || !game->IsValidPlayerIndex(index)) return false;
#ifdef _MSC_VER
        __try
        {
#endif
            void* player = entity->m_pUnk ? entity->m_pUnk->GetBaseEntity() : nullptr;
            if (!player || !game->m_PlayersVRInfo[index].isUsingVR) return false;
            auto* playerBytes = static_cast<unsigned char*>(player);
            // These owner/state fields are the existing supported server ABI.
            // The shotgun reload signature verifies its 0x1cb4 state access.
            if (playerBytes[0xf0] != 0 || *reinterpret_cast<int*>(playerBytes + 0x238) != 2 ||
                (*reinterpret_cast<unsigned*>(playerBytes + 0x1cb4) & 0x801u) != 0u) return false;
            using Active = void* (__thiscall*)(void*);
            using Owner = void* (__thiscall*)(void*);
            using Index = int (__thiscall*)(void*);
            using Capacity = int (__thiscall*)(void*);
            using Ammo = int (__thiscall*)(void*, int);
            using Accessor = unsigned short* (__thiscall*)(edict_t*);
            auto* weapon = static_cast<Server_WeaponCSBase*>(
                reinterpret_cast<Active>(game->m_Offsets->GetActiveWeapon.address)(player));
            if (!weapon || reinterpret_cast<Owner>(game->m_Offsets->PhysicalGunOwner.address)(weapon) != player)
                return false;
            const int id = weapon->GetWeaponID();
            if (id != request.weaponId || (magazine ? !l4d2vr_remote_mag::IsDetachable(id) : !l4d2vr_shell::IsShotgun(id)))
                return false;
            auto* weaponBytes = reinterpret_cast<unsigned char*>(weapon);
            if (pistol && (id != 1 || weaponBytes[0x17dd] > 1u)) return false;
            if (magazine && !pistol && id == 1 && weaponBytes[0x17dd] != 0u) return false; // native dual-pistol fallback
            native.playerObject = player; native.weaponObject = weapon;
            native.dualPistol = pistol && weaponBytes[0x17dd] != 0u;
            // CBaseEntity::entindex's signature validates the edict pointer at 0x28.
            edict_t* weaponEdict = *reinterpret_cast<edict_t**>(weaponBytes + 0x28);
            int weaponIndex = -1; unsigned weaponSerial = 0u;
            int ownerIndex = -1; unsigned ownerSerial = 0u;
            if (!game->ReadServerEdictIdentity(entity, ownerIndex, ownerSerial, player) || ownerIndex != index ||
                !game->ReadServerEdictIdentity(weaponEdict, weaponIndex, weaponSerial, weapon) ||
                !weaponEdict || (weaponEdict->m_fStateFlags & 2u) != 0u ||
                reinterpret_cast<Index>(game->m_Offsets->CBaseEntity_entindex.address)(weapon) != weaponIndex ||
                (weaponEdict->m_fStateFlags & 4u) == 0u) return false;
            // SendProxy_EHandleToInt transmits the entity-list handle serial,
            // not the edict allocation serial. Project that native handle into
            // the client's 12-entry/10-serial-bit representation.
            const uint32_t activeHandle = *reinterpret_cast<uint32_t*>(playerBytes + 0x19d4);
            if (activeHandle == 0xffffffffu || (activeHandle & 0xfffu) != static_cast<unsigned>(weaponIndex) ||
                !l4d2vr_shell::HandleMatches(request.handle, static_cast<int>(activeHandle & 0xfffu), activeHandle >> 12))
                return false;
            const int ammoType = *reinterpret_cast<int*>(weaponBytes + 0x140c);
            if (ammoType < 0 || ammoType >= 32) { unsupported = true; return false; }
            native.clip = reinterpret_cast<int*>(weaponBytes + 0x1414);
            native.reserve = reinterpret_cast<int*>(playerBytes + 0x1874) + ammoType;
            native.player = entity; native.weapon = weaponEdict;
            native.oldClip = *native.clip; native.oldReserve = *native.reserve;
            native.inReload = weaponBytes + 0x144d;
            native.reloadState = magazine ? nullptr : reinterpret_cast<int*>(weaponBytes + 0x17f0);
            native.oldInReload = *native.inReload;
            native.oldReloadState = native.reloadState ? *native.reloadState : 0;
            const int reserve = reinterpret_cast<Ammo>(game->m_Offsets->PhysicalShellAmmoCount.address)(player, ammoType);
            if (magazine && !ReadNativeInfiniteAmmo(game, ammoType, native.infinite)) { unsupported = true; return false; }
            if (!native.infinite && reserve != native.oldReserve) { unsupported = true; return false; }
            auto accessor = reinterpret_cast<Accessor>(game->m_Offsets->PhysicalShellEdictAccessor.address);
            unsigned short* playerAccessor = accessor(entity);
            unsigned short* weaponAccessor = accessor(weaponEdict);
            if (!playerAccessor || !weaponAccessor) { unsupported = true; return false; }
            native.playerChangeSerial = playerAccessor + 1; native.weaponChangeSerial = weaponAccessor + 1;
            if (!RemoteShellWritable(native.clip, sizeof(int)) || !RemoteShellWritable(native.reserve, sizeof(int)) ||
                !RemoteShellWritable(&entity->m_fStateFlags, sizeof(int)) ||
                !RemoteShellWritable(&weaponEdict->m_fStateFlags, sizeof(int)) ||
                !RemoteShellWritable(native.playerChangeSerial, sizeof(unsigned short)) ||
                !RemoteShellWritable(native.weaponChangeSerial, sizeof(unsigned short)))
            { unsupported = true; return false; }
            if (!RemoteShellWritable(native.inReload, sizeof(unsigned char)) ||
                (native.reloadState && !RemoteShellWritable(native.reloadState, sizeof(int))))
            { unsupported = true; return false; }
            const auto& vrPlayer = game->m_PlayersVRInfo[index];
            const uint64_t now = GetTickCount64();
            const bool inputFresh = vrPlayer.lastDecodedUsercmdTickMs != 0u &&
                now >= vrPlayer.lastDecodedUsercmdTickMs && now - vrPlayer.lastDecodedUsercmdTickMs < 2500u;
            state = {true, request.handle, inputFresh ? vrPlayer.lastDecodedUsercmd : 0u,
                id, native.oldClip, native.oldReserve,
                reinterpret_cast<Capacity>(game->m_Offsets->PhysicalShellMaxClip.address)(weapon)};
            return true;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { unsupported = true; return false; }
#endif
    }
    bool WriteRemoteShellNative(const RemoteShellNativeState& state, int clip, int reserve)
    {
#ifdef _MSC_VER
        __try
        {
#endif
            if (!state.clip || !state.reserve || *state.clip != state.oldClip || *state.reserve != state.oldReserve)
                return false;
            *state.reserve = reserve; *state.clip = clip;
            // End a conventional reload that started before negotiation. Its
            // timed loop must not insert additional shells after this one.
            *state.inReload = 0;
            if (state.reloadState) *state.reloadState = 0;
            // Mirror CBaseEdict::StateChanged: full change plus cleared change
            // serial, so the host's clip and reserve reach the guest via Source.
            state.player->m_fStateFlags |= 0x101;
            state.weapon->m_fStateFlags |= 0x101;
            *state.playerChangeSerial = 0; *state.weaponChangeSerial = 0;
            return true;
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            __try
            {
                if (state.reserve) *state.reserve = state.oldReserve;
                if (state.clip) *state.clip = state.oldClip;
                if (state.inReload) *state.inReload = state.oldInReload;
                if (state.reloadState) *state.reloadState = state.oldReloadState;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return false;
        }
#endif
    }
    bool ParseRemoteShellRequest(const SourceCCommand& command, l4d2vr_shell::Request& request)
    {
        if (command.ArgC() != 8) return false;
        uint32_t values[7]{};
        constexpr uint32_t limits[7] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u,
            0x7fffffffu, 64u, 128u, 5000u};
        for (int arg = 1; arg <= 7; ++arg)
            if (!l4d2vr_shell::ParseNumber(command.Arg(arg), limits[arg - 1], values[arg - 1])) return false;
        request = {values[0], values[1], values[2], values[3], static_cast<int>(values[4]),
            static_cast<int>(values[5]), static_cast<int>(values[6])};
        return true;
    }
}

void Game::ResetRemoteShellServerClients()
{
    std::lock_guard<std::mutex> lock(m_RemoteShellServerMutex);
    m_RemoteShellServerClients.fill(RemoteShellServerClient{});
}

void Game::OfferRemoteShellProtocol(edict_t* entity)
{
    if (!RemoteShellBackendReady(this)) return;
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return;
    uint32_t token = 0u;
    {
        std::lock_guard<std::mutex> lock(m_RemoteShellServerMutex);
        auto& client = m_RemoteShellServerClients[index];
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
    std::snprintf(command, sizeof(command), "l4d2vr_shell_capability %u %u\n", l4d2vr_shell::kVersion, token);
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, command);
}

bool Game::HandleRemoteShellCommand(edict_t* entity, const void* sourceCommand)
{
    VRPoseRelayCommandView view{};
    if (!VRPoseRelayReadCommand(sourceCommand, view) || std::strcmp(view.name, "l4d2vr_shell_insert_v1") != 0)
        return false;
    l4d2vr_shell::Request request{};
    if (!ParseRemoteShellRequest(*static_cast<const SourceCCommand*>(sourceCommand), request)) return true;
    int index = -1; unsigned serial = 0u;
    if (!VRPoseRelayReadEdictIdentity(entity, index, serial)) return true;
    l4d2vr_shell::Reply reply{};
    {
        std::lock_guard<std::mutex> lock(m_RemoteShellServerMutex);
        auto& client = m_RemoteShellServerClients[index];
        if (client.entity != entity || client.serial != serial || !client.ledger.Token()) return true;
        if (request.token != client.ledger.Token()) return true;
        l4d2vr_shell::Snapshot snapshot{}; RemoteShellNativeState native{};
        bool unsupported = false;
        ReadRemoteShellNative(this, entity, index, request, snapshot, native, unsupported);
        reply = unsupported ? client.ledger.Unsupported(request) :
            client.ledger.Apply(request, snapshot, GetTickCount64(),
                [&](int clip, int reserve) { return WriteRemoteShellNative(native, clip, reserve); });
    }
    char response[192]{};
    std::snprintf(response, sizeof(response), "l4d2vr_shell_result %u %u %u %u %d %d %d %u %d %d\n",
        request.token, request.sequence, request.handle, request.command, request.weaponId, request.clip, request.reserve,
        static_cast<unsigned>(reply.status), std::max(0, reply.clip), std::max(0, reply.reserve));
    VRPoseRelaySendClientCommand(m_ServerPluginHelpers, entity, response);
    Game::logMsg("[VR][RemoteShell][server] player=%d seq=%u status=%u weaponId=%d clip=%d reserve=%d",
        index, request.sequence, static_cast<unsigned>(reply.status), request.weaponId, reply.clip, reply.reserve);
    return true;
}
