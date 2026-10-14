#pragma once
#include "sigscanner.h"
#include "game.h"


struct Offset
{
    std::string moduleName;
    int offset;
    int address;
    std::string signature;
    int sigOffset;
    bool optional;
    bool valid;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0, bool optional = false)
    {
        this->moduleName = moduleName;
        this->offset = currentOffset;
        this->address = 0;      // Always initialize to safe value
        this->signature = signature;
        this->sigOffset = sigOffset;
        this->optional = optional;
        this->valid = false;

        int newOffset = SigScanner::VerifyOffset(moduleName, currentOffset, signature, sigOffset);
        if (newOffset > 0)
        {
            this->offset = newOffset;
        }
        if (newOffset == -1)
        {
            // Keep address=0 and valid=false so hook setup can safely skip.
            if (!optional)
            {
                Game::errorMsg(("Signature not found: " + signature).c_str());
            }
            return;
        }

        HMODULE mod = GetModuleHandle(moduleName.c_str());
        if (!mod)
        {
            if (!optional)
                Game::errorMsg(("Module not loaded: " + moduleName).c_str());
            return;
        }

        this->address = (uintptr_t)mod + this->offset;
        this->valid = (this->address != 0);
    }
};

class Offsets
{
public:
    Offset RenderView =                  { "client.dll", 0x1D6C30, "55 8B EC 81 EC ? ? ? ? 53 56 57 8B D9" };
    Offset g_pClientMode =               { "client.dll", 0x228351, "89 04 B5 ? ? ? ? E8", 3 };
    Offset CalcViewModelView =           { "client.dll", 0x287270, "55 8B EC 83 EC 48 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 10" };
    Offset ClientFireTerrorBullets =     { "client.dll", 0x2F4350, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10"};
    // FX_Tracer: cdecl, eight 4-byte stack arguments; attachment -1 emits
    // from its explicit start instead of rebinding to a native viewmodel.
    Offset ClientFxTracer = { "client.dll", 0x001ACAC0,
        "55 8B EC 83 EC 60 53 0F 57 C0 F3 0F 10 0D ? ? ? ? 33 C0 F3 0F 11 45 B8", 0, true };
    Offset WriteUsercmdDeltaToBuffer =   { "client.dll", 0x134790, "55 8B EC 83 EC 60 0F 57 C0 8B 55 0C" };
    Offset WriteUsercmd =                { "client.dll", 0x1AAD50, "55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 10 56 57" };
    Offset g_pppInput =                  { "client.dll", 0xA8A22, "8B 0D ? ? ? ? 8B 01 8B 50 58 FF E2", 2 };
    Offset AdjustEngineViewport =        { "client.dll", 0x31A890, "55 8B EC 8B 0D ? ? ? ? 85 C9 74 17" };
    Offset TestMeleeSwingClient =        { "client.dll", 0x30C040, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B 75 08 57 8B D9 E8 ? ? ? ? 8B" };
    Offset GetMeleeWeaponInfoClient =    { "client.dll", 0x30B570, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset IsSplitScreen =               { "client.dll", 0x1B2A60, "33 C0 83 3D ? ? ? ? ? 0F 9D C0" };
    Offset PrePushRenderTarget =         { "client.dll", 0xA8C80, "55 8B EC 8B C1 56 8B 75 08 8B 0E 89 08 8B 56 04 89" };
    Offset UpdateLaserSight =            { "client.dll", 0x25DD80, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 28 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F1 E8 ? ? ? ? 8B F8 83 FF FF 0F 84 ? ? ? ?", 0, true };
    Offset ParticleSetControlPointPosition = { "client.dll", 0x15BD10, "55 8B EC 53 56 8B 75 0C 57 8B F9 BB 01 00 00 00 84 9F B1 03 00 00 0F 84 ? ? ? ? 83 BF B4 03 00 00 FF", 0, true };
    Offset CreateParticleEffect =        { "client.dll", 0x152200, "55 8B EC 56 57 8B 7D 08 8B F1 8B 0D ? ? ? ? 57 E8 ? ? ? ? 85 C0 75 17 57 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 08 33 C0 5F 5E 5D C2 1C 00 8B 4D 20 8B 55 14 51 83 EC 0C 8B CC 89 11 8B 55 18 89 51 04 8B 55 1C 89 51 08", 0, true };
    Offset StopParticleEffect =          { "client.dll", 0x1523C0, "55 8B EC 51 53 56 8B 75 08 57 8B F9 8B 5F 14 85 F6 74 61 33 C0 85 DB 0F 8E ? ? ? ? 8B 57 08 83 C2 14 39 32 74 11 40 83 C2 18 3B C3 7C F4 5F 5E 5B 8B E5 5D C2 04 00", 0, true };
    Offset ParticleSetControlPointForwardVector = { "client.dll", 0x15C030, "55 8B EC 8B 45 0C 56 57 8B 7D 08 8B F1 50 57 8D 4E 10 E8 ? ? ? ? 57 8B CE E8 ? ? ? ? 5F 5E 5D C2 08 00", 0, true };
    Offset DrawLaserBeam =               { "client.dll", 0x0EF660, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC C8 04 00 00 A1 ? ? ? ? 33 C5 89 45 FC A1 ? ? ? ? D9 BD BA FB FF FF D9 40 0C 8B 4B 0C 0F B7 85 BA FB FF FF D8 0D ? ? ? ? 0D 00 0C 00 00", 0, true };
    Offset UpdateFlashlight =             { "client.dll", 0x0EE820, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 55 18 53 8B 5D 0C 56 8B F1", 0, true };
    Offset UpdateFlashlightColor =        { "client.dll", 0x0EEB10, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 14 53 56 89 85 ? ? ? ? 8B 45 18 8B F1", 0, true };
    Offset ClientPlayerEyeVectors =       { "client.dll", 0x20450,
        "55 8B EC 56 8B F1 8B 86 ? ? ? ? 83 F8 FF 74 5B",
        0,
        true
    };
    Offset ClientPlayerEyePosition =      { "client.dll", 0x20EB0,
        "55 8B EC 56 8B F1 8B 86 ? ? ? ? 83 F8 FF 74 5D",
        0,
        true
    };
    Offset ClientFindUseEntity =          { "client.dll", 0x283200,
        "55 8B EC 81 EC 78 0B 00 00 A1 ? ? ? ? 33 C5 89 45 FC 0F 57 C9 F3 0F 10 45 0C 0F 2F C8 F3 0F 10 55 08",
        0,
        true
    };
    // CGlowProperty::SetGlowType. Grip Pull calls this only from the
    // CreateMove/main-thread target update path.
    Offset CGlowProperty_SetGlowType_Client = { "client.dll", 0x109A20,
        "55 8B EC 53 8B 5D 08 56 57 8B F9 39 5F 04 8D 77 04 74 0D 8B 46 FC 8B 10 8D 4E FC 56 FF D2",
        0,
        true
    };
    Offset ServerFireTerrorBullets =     { "server.dll", 0x3C3FC0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
    Offset ReadUserCmd =                 { "server.dll", 0x205100, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
    Offset ProcessUsercmds =             { "server.dll", 0xEF710, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };
    Offset CBaseEntity_entindex =        { "server.dll", 0x25390, "8B 41 28 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC CC CC CC CC CC CC CC CC CC CC 55"};
    Offset TestMeleeSwingServer = { "server.dll", 0x003EB2E0,
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 48 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 56 8B 73 08 57 89 4D 98 E8 ? ? ? ? 8B F8", 0 };
    // CTerrorPlayer::WeaponShootPosition: thiscall output Vector*, ret4.
    Offset PhysicalMeleeShootPosition = { "server.dll", 0x002FC430,
        "55 8B EC 56 8B F1 E8 ? ? ? ? 8B CE 85 C0 74 1C E8 ? ? ? ? 8B 10 8B 75 08 8B C8 8B 82 70 04 00 00 56 FF D0", 0, true };
    Offset DoMeleeSwingServer =          { "server.dll", 0x3E84C0, "55 8B EC 83 EC 3C 53 56 8B F1 E8 ? ? ? ? 8B D8 85" };
    Offset StartMeleeSwingServer =       { "server.dll", 0x3E8780, "55 8B EC 53 56 8B F1 8B 86 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? 8B" };
    Offset PrimaryAttackServer =         { "server.dll", 0x3E8AB0, "56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? 83 F8 FF" };
    Offset ItemPostFrameServer =         { "server.dll", 0x3E8BA0, "56 57 8B F1 E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 53" };
    Offset MolotovProjectileCreate =     { "server.dll", 0x003DA4E0, "51 8B CE E8 ? ? ? ? D9 EE 6A 00 51 D9 1C 24 68 ? ? ? ? 8B CE E8 ? ? ? ? D9 EE 6A 00 51 D9 1C 24 68 ? ? ? ? 8B CE E8 ? ? ? ? 8B 0D ? ? ? ? 8B 11 8B 42 18 6A 00 6A 00 68 ? ? ? ? FF D0 8B F8 85 FF 74 ? 85 DB 74 ?", -0x1E0, true };
    Offset PipeBombProjectileCreate =    { "server.dll", 0x003DE720, "55 8B EC 8B 45 18 8B 4D 0C 53 8B 5D 08 56 57 50 51 53 68 ? ? ? ? E8 ? ? ? ? 8B 7D 10 D9 47 08 83 EC 20 DD 5C 24 28 8B F0 D9 47 04 DD 5C 24 20 D9 07 DD 5C 24 18 D9 43 08 DD 5C 24 10 D9 43 04 DD 5C 24 08 D9 03 DD 1C 24 68 ? ? ? ? E8 ? ? ? ? 8B 15 ? ? ? ? D9 42 2C 83 C4 30 8B CE D9 1C 24 E8 ? ? ? ?", 0, true };
    Offset VomitJarProjectileCreate =    { "server.dll", 0x003F4190, "8B 3E 8B CB E8 ? ? ? ? 50 8B 87 90 01 00 00 8B CE FF D0 8B 4D 14 51 8B CE E8 ? ? ? ? 5F 8B C6 5E 5B 5D C3", -0x1BE, true };
    // Global CreateEntityByName wrapper. The normal carry-weapon release path can
    // bypass CWeaponCarry::CreatePhysicsProp, but every replacement physics_prop
    // still passes through this factory.
    Offset ManualCarryCreateEntityByName = { "server.dll", 0x001196B0,
        "55 8B EC 56 8B 75 0C 57 8B 7D 08 83 FE FF 74 27 8B 0D ? ? ? ? 8B 01 8B 50 58 56 FF D2 A3 ? ? ? ?",
        0,
        true
    };
    // World weapons materialized from repeatable map spawn points must be
    // positioned before DispatchSpawn so their model and physics initialize at
    // the source pickup rather than briefly existing at the world origin.
    Offset CBaseEntity_SetAbsOrigin_Server = { "server.dll", 0x00063A90,
        "55 8B EC 83 EC 0C 56 57 8B 7D 08 F3 0F 10 07 F3 0F 11 45 08 8B 45 08 25 00 00 80 7F",
        0,
        true
    };
    Offset DispatchSpawn_Server = { "server.dll", 0x00209580,
        "55 8B EC 51 53 56 8B 75 08 57 85 F6 0F 84 ? ? ? ? 8B 0D ? ? ? ? 8B 01 8B 50 68",
        0,
        true
    };
    // CWeaponCarry::CreatePhysicsProp. Unlike the global entity factory, this
    // gives us the source carry weapon as `this`, so a prop created before the
    // release command can be paired with that release later.
    Offset ManualCarryCreatePhysicsProp = { "server.dll", 0x003C8210,
        "55 8B EC 81 EC A0 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 74 10 8B 07 8B 90 68 01 00 00",
        0,
        true
    };
    // CBaseCombatCharacter::Weapon_Drop(CBaseCombatWeapon*, Vector*, Vector*).
    // Unlike CWeaponCarry::DropToPhysicsProp, this works for ordinary guns,
    // melee weapons, packs and medicine. It detaches the existing weapon entity
    // from the survivor and runs the native next-weapon selection path.
    Offset ManualInventoryWeaponDrop = { "server.dll", 0x00045360,
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 18 01 00 00 A1 ? ? ? ? 33 C5 89 45 FC 8B 43 0C 56 8B 73 08 57 8B F9",
        0,
        true
    };
    // CPistol::RemoveDualWeapons(bool force). The signature also verifies the
    // two pistol flag offsets used by the split transaction. Skip on mismatch.
    Offset PistolRemoveDualWeapons = { "server.dll", 0x003E85C0,
        "55 8B EC 56 8B F1 80 BE DD 17 00 00 00 75 07 32 C0 5E 5D C2 04 00 80 7D 08 00 75 1C 80 BE 4D 14 00 00 00",
        0, true
    };
    // Reload is virtual slot 281 on the supported L4D2 server ABI. Verified
    // against the installed gun/shotgun vtables; skip when signatures differ.
    Offset PhysicalGunReload = { "server.dll", 0x003E8860,
        "55 8B EC 83 EC 08 53 56 8B F1 E8 ? ? ? ? 8B D8 85 DB 0F 84 ? ? ? ? 8B 83 3C 2E 00 00", 0, true };
    Offset PhysicalShotgunReload = { "server.dll", 0x003C2F80,
        "55 8B EC 83 EC 08 56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 B4 1C 00 00", 0, true };
    Offset PhysicalGunOwner = { "server.dll", 0x003EC5D0,
        "56 E8 ? ? ? ? 8B F0 85 F6 74 14 8B 06 8B 90 68 01 00 00 8B CE FF D2 84 C0 74 04 8B C6 5E C3", 0, true };
    // Native shell transaction proofs. Literal displacements validate the
    // server ammo array (0x1874), weapon script handle and clip-capacity field.
    Offset PhysicalShellAmmoCount = { "server.dll", 0x00046DF0,
        "55 8B EC 56 8B 75 08 57 8B F9 83 FE FF 75 08 5F 33 C0 5E 5D C2 04 00 56 E8 ? ? ? ? 8B C8 E8 ? ? ? ? 84 C0 B8 E7 03 00 00 75 07 8B 84 B7 74 18 00 00 5F 5E 5D C2 04 00", 0, true };
    Offset PhysicalShellMaxClip = { "server.dll", 0x0004A810,
          "0F B7 81 68 14 00 00 50 E8 ? ? ? ? 8B 80 60 01 00 00 83 C4 04 C3", 0, true };
    // CAmmoDef::IsInfiniteAmmo, also called by native GetAmmoCount. Keep the
    // ammo-definition result authoritative rather than assuming pistols are infinite.
    Offset PhysicalAmmoInfinite = { "server.dll", 0x0002D2C0,
        "55 8B EC 8B 45 08 83 F8 01 7C 29 3B 41 04 7D 24 6B C0 34 03 C8 8B 41 2C 83 F8 FF 75 0D 8B 49 38 85 C9 74 06 8B 41 1C 8B 40 30 83 F8 FE 0F 94 C0 5D C2 04 00", 0, true };
    // CTerrorPlayer::PlayerRunCommand(CUserCmd*, IMoveHelper*), thiscall, ret 8.
    // Native command execution is distinct from packet deserialization.
    Offset PistolPlayerRunCommand = { "server.dll", 0x00319F50,
        "55 8B EC 83 EC 18 A1 ? ? ? ? 53 33 DB 89 5D F8 89 5D FC 57 8B F9 39 58 08", 0, true };
    // Native gun firing boundary: thiscall, no stack arguments, void return.
    // Pistol ownership/layout checks restrict accounting to supported CPistol.
    Offset PistolGunFire = { "server.dll", 0x003E9370,
        "55 8B EC 83 EC 34 53 56 8B F1 E8 ? ? ? ? 8B D8 85 DB 0F 84 ? ? ? ? 80 BE 2A 15 00 00 00", 0, true };
    // CPrediction::RunCommand: thiscall (player, command, move helper), ret 12.
    // Capture the command being simulated instead of the latest CreateMove pose.
    Offset ClientPistolRunCommand = { "client.dll", 0x0017B460,
        "55 8B EC 83 EC 18 53 56 8B 75 08 57 8B 7D 0C 57 8B D9 89 BE 28 14 00 00 E8 ? ? ? ?", 0, true };
    // Client gun firing boundary: thiscall, no stack arguments, void return.
    Offset ClientPistolGunFire = { "client.dll", 0x0030C4C0,
        "55 8B EC 83 EC 2C 56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 80 BE 5A 0A 00 00 00", 0, true };
    // C_Pistol::GetPrimaryAttackActivity: thiscall, no stack arguments, int return.
    Offset ClientPistolFireActivity = { "client.dll", 0x00305A10,
        "56 8B F1 8B 06 8B 90 58 06 00 00 FF D2 84 C0 8B 86 84 09 00 00 74 15", 0, true };
    // C_Pistol reload entries: thiscall, no stack arguments, bool/void returns.
    Offset ClientPistolReload = { "client.dll", 0x0030C150,
        "56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 8C 1F 00 00 83 F8 FF", 0, true };
    Offset ClientPistolFinishReload = { "client.dll", 0x0030C2E0,
        "56 8B F1 E8 ? ? ? ? 33 C0 39 86 E8 0C 00 00 74 06 89 86 E8 0C 00 00 38 86 EE 0C 00 00", 0, true };
    Offset PhysicalShellEdictAccessor = { "server.dll", 0x000ED4B0,
        "51 8B 0D ? ? ? ? 8B 11 8B 82 84 01 00 00 FF D0 C3", 0, true };
    Offset PhysicalShellReloadLayout = { "server.dll", 0x003C30C4,
        "8B 86 0C 14 00 00 50 8B CF E8 ? ? ? ? 8B 16 8B 9E 14 14 00 00 89 45 F8 8B 82 0C 05 00 00 8B CE FF D0 2B C3", 0, true };
    Offset PhysicalShellReloadState = { "server.dll", 0x003C2FF6,
        "83 BE F0 17 00 00 00 53 0F 85 ? ? ? ?", 0, true };
    Offset PhysicalShellActiveHandle = { "server.dll", 0x000464F0,
        "8B 81 D4 19 00 00 83 F8 FF 74 23 8B 15 ? ? ? ? 8B C8 81 E1 FF 0F 00 00", 0, true };
    // CBaseCombatCharacter::RemovePlayerItem and UTIL_Remove. Empty-hand mode
    // uses them to destroy only the hidden placeholder pistol before a pickup.
    Offset ManualEmptyHandsRemovePlayerItem = { "server.dll", 0x00045200,
        "55 8B EC 51 83 7D 08 00 53 56 8B D9 57 89 5D FC 74 4F 8D BB F4 18 00 00 33 F6 8B D7",
        0,
        true
    };
    Offset ManualEmptyHandsUtilRemove = { "server.dll", 0x002071E0,
        "55 8B EC 8B 45 08 85 C0 74 0C 83 C0 1C 50 E8 ? ? ? ? 83 C4 04 5D C3",
        0,
        true
    };
    // Exact L4D2 CTakeDamageInfo constructor and CBaseEntity::TakeDamage wrapper.
    // Using these avoids guessing target vtable slots in collision handling.
    Offset CTakeDamageInfoCtor_Server = { "server.dll", 0x001E9030,
        "55 8B EC D9 45 10 8B 55 0C 56 8B F1 8B 4D 14 83 C8 FF 89 46 30 89 46 34 89 46 38",
        0,
        true
    };
    Offset CBaseEntity_TakeDamage_Server = { "server.dll", 0x00055C70,
        "55 8B EC 83 EC 6C 57 8B F9 8B 0D ? ? ? ? 85 C9 0F 84 ? ? ? ? 8B 11 56 8B 75 08",
        0,
        true
    };
    Offset GetPrimaryAttackActivity =    { "server.dll", 0x3E7630, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?" };
    Offset GetActiveWeapon =             { "server.dll", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64 };
    Offset GetMeleeWeaponInfo =          { "server.dll", 0x3E67D0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset EyePosition =                 { "server.dll", 0x6D610, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" };
    Offset ServerPlayerEyeAngles =       { "server.dll", 0x7B0F0,
        "55 8B EC 83 EC 64 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 64 80 3D ? ? ? ? 00 75 5B 8B 87 ? ? ? ? 83 F8 FF 74 50",
        0,
        true
    };
    Offset ServerPlayerEyePosition =     { "server.dll", 0x7B2A0,
        "55 8B EC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 71 80 3D ? ? ? ? 00 75 68 8B 87 ? ? ? ? 83 F8 FF 74 5D",
        0,
        true
    };
    Offset FindUseEntity =               { "server.dll", 0x34E6C0,
        "55 8B EC B8 80 13 00 00 E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 0F 57 C9 F3 0F 10 45 0C 0F 2F C8 F3 0F 10 55 08",
        0,
        true
    };
    Offset PlayerUse =                   { "server.dll", 0x312AC0,
        "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B F1 8B 9E B8 1C 00 00 8B 06 8B 90 28 01 00 00",
        0,
        true
    };
    // Server CBaseEntity helpers used by VR-only roomscale movement on local/listen servers.
    Offset CBaseEntity_GetAbsOrigin_Server = { "server.dll", 0x28D10,
        "56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8D 86 ? ? ? ? 5E C3",
        0,
        true
    };
    // CBaseEntity::VPhysicsCollision. CPhysicsProp and the carry weapon
    // implementations route their real VPhysics contacts through this base
    // method, whose event includes both server entities and pre-impact speed.
    Offset CBaseEntity_VPhysicsCollision_Server = { "server.dll", 0x56590,
        "55 8B EC 51 53 56 8B 75 0C 33 DB 57 8B 7D 08 85 FF 0F 94 C3 89 4D FC 8B 44 9E 68 89 45 0C 85 C0",
        0,
        true
    };
    // CTerrorPlayer::OnShovedBySurvivor. This is the native survivor-push
    // reaction used for player-controlled and AI special infected.
    Offset CTerrorPlayer_OnShovedBySurvivor_Server = { "server.dll", 0x32BB00,
        "55 8B EC 81 EC F8 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 8B 5D 08 56 57 8B 7D 0C 8B F1 89 5D 80 E8 ? ? ? ? 84 C0 0F 85 ? ? ? ?",
        0,
        true
    };
    // server.dll statically links the VC10 RTTI helper. It is used to obtain
    // the INextBot responder implemented by common infected and witches.
    Offset Server_RTDynamicCast = { "server.dll", 0x55E1EE,
        "6A 24 68 ? ? ? ? E8 ? ? ? ? 8B 75 08 85 F6 75 08 33 C0 E8 ? ? ? ? C3 83 65 FC 00 8B CE E8 ? ? ? ? 8B D0 89 55 E4",
        0,
        true
    };
    Offset Server_RTTI_CBaseEntity = { "server.dll", 0x74A1D0,
        "? ? ? ? 00 00 00 00 2E 3F 41 56 43 42 61 73 65 45 6E 74 69 74 79 40 40 00",
        0,
        true
    };
    Offset Server_RTTI_INextBot = { "server.dll", 0x76AB48,
        "? ? ? ? 00 00 00 00 2E 3F 41 56 49 4E 65 78 74 42 6F 74 40 40 00",
        0,
        true
    };
    // INextBotEventResponder::OnShoved(CBaseEntity*). The current L4D2
    // responder vtable exposes this propagation routine at slot 28.
    Offset INextBotEventResponder_OnShoved_Server = { "server.dll", 0x298FE0,
        "55 8B EC 56 57 8B F9 8B 07 8B 50 04 FF D2 8B F0 85 F6 74 1F 53 8B 5D 08 8B 06 8B 50 70 53 8B CE FF D2 8B 07 8B 50 08 56 8B CF FF D2 8B F0 85 F6 75 E6 5B 5F 5E 5D C2 04 00",
        0,
        true
    };
    Offset CBaseEntity_SetOrigin_Server = { "server.dll", 0x521A0,
        "55 8B EC 8B 01 8B 55 08 8B 80 ? ? ? ? 6A 00 6A 00 52 FF D0 5D C2 04 00",
        0,
        true
    };
    // CBaseEntity::SetAbsOrigin (client) - used for viewmodel stabilization.
    Offset CBaseEntity_SetAbsOrigin_Client = { "client.dll", 0,
        "55 8B EC 56 57 8B F1 E8 ?? ?? ?? ?? 8B 7D 08 F3 0F 10 07",
        0
    };

    Offset GetRenderTarget =             { "materialsystem.dll", 0x2CD30, "83 79 4C 00" };
    Offset Viewport =                    { "materialsystem.dll", 0x2E010, "55 8B EC 83 EC 28 8B C1" };
    Offset GetViewport =                 { "materialsystem.dll", 0x2D240, "55 8B EC 8B 41 4C 8B 49 40 8D 04 C0 83 7C 81 ? ?" };
    Offset PushRenderTargetAndViewport = { "materialsystem.dll", 0x2D5F0, "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89" };
    Offset PopRenderTargetAndViewport =  { "materialsystem.dll", 0x2CE80, "56 8B F1 83 7E 4C 00" };

    Offset DrawModelExecute =            { "engine.dll", 0xE05E0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B" };
    Offset VGui_Paint =                  { "engine.dll", 0x115CE0, "55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38" };
};
