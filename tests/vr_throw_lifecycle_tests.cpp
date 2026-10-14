#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_throw_lifecycle.h"
#include "../L4D2VR/vr_grip_release.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "throw_pending_type.inl"

struct C_WeaponCSBase
{
    enum WeaponID { NONE=0, PISTOL=1, MOLOTOV=13, PIPE_BOMB=14, GASCAN=16,
        PROPANE_TANK=17, OXYGEN_TANK=18, VOMITJAR=24, GNOME_CHOMPSKI=27,
        COLA_BOTTLES=28, FIREWORKS_BOX=29 };
};
struct Entity;
struct edict_t { int index=0; unsigned serial=1u; bool live=true; Entity* object=nullptr; };
struct Entity
{
    void* vtable=reinterpret_cast<void*>(0x12345678u);
    unsigned char padding[0x28-sizeof(void*)]{};
    edict_t* edict=nullptr;
    void* owner=nullptr;
    Entity* active=nullptr;
    int weaponId=0;
    int GetWeaponID() const { return weaponId; }
};
static_assert(offsetof(Entity,edict)==0x28,"The fixture exercises the actual native edict pointer access");
using Server_WeaponCSBase=Entity;
struct Player
{
    bool isUsingVR=true;
    int manualThrowLastTick=100;
    Vector controllerPos{10.0f,20.0f,30.0f};
    QAngle controllerAngle{0.0f,0.0f,0.0f};
    ManualThrowPending manualThrowPending{};
};
struct Offset { uintptr_t address=0u; bool valid=true; };
struct Offsets { Offset PhysicalGunOwner,ManualInventoryWeaponDrop,GetActiveWeapon,CBaseEntity_entindex; };
struct Game
{
    Offsets* m_Offsets=nullptr;
    void* m_CurrentUsercmdPlayer=nullptr;
    int m_CurrentUsercmdID=1;
    std::array<Player,3> m_PlayersVRInfo{};
    bool IsValidPlayerIndex(int index) const { return index>0 && index<3; }
    bool ReadServerEdictIdentity(const edict_t* edict,int& index,unsigned& serial,const void* expected) const
    {
        if(!m_Offsets || !m_Offsets->CBaseEntity_entindex.valid || !edict || !edict->live ||
            edict->object!=expected || edict->index<=0 || edict->object->edict!=edict) return false;
        index=edict->index; serial=edict->serial; return true;
    }
    static void logMsg(const char*,...) {}
};
struct VR
{
    bool m_ManualThrowEnabled=true;
    int m_ManualThrowVelocityWindowTicks=5;
    float m_ManualThrowPeakVelocityBlend=0.5f,m_ManualThrowVelocityScale=1.0f;
    float m_ManualThrowHorizontalVelocityScale=1.0f,m_ManualThrowVerticalVelocityScale=1.0f;
    float m_ManualThrowArcLiftRatio=0.0f,m_ManualThrowMaxVelocity=1400.0f;
};
enum class ManualThrowableKind {Molotov,PipeBomb,VomitJar};
using tThrowableProjectileCreate=void*(__cdecl*)(const Vector&,const QAngle&,const Vector&,const Vector&,void*);
template<class T> struct Hook { T fOriginal=nullptr; };
struct Hooks
{
    static inline Game* m_Game=nullptr;
    static inline VR* m_VR=nullptr;
    static inline bool s_ManualThrowHooksReady=true;
    static inline bool s_ManualCarryThrowHookReady=true,s_ManualCarryThrowPropSpawnHookReady=true;
    static inline Hook<void*(__thiscall*)(void*)> hkManualCarryCreatePhysicsProp;
    static inline Hook<void*(__cdecl*)(const char*,int,bool)> hkManualCarryCreateEntityByName;
    static void* __fastcall dManualCarryCreatePhysicsProp(void*,void*);
    static void* __cdecl dManualCarryCreateEntityByName(const char*,int,bool);
};
static int checks=0,dropCalls=0,teleports=0,impacts=0,creates=0;
static Entity* spawned=nullptr;
static Entity* replacementWeapon=nullptr;
static void* teleported=nullptr;
static bool removeCarrySource=false;
static Vector nativePosition{100.0f,200.0f,300.0f},receivedPosition{0.0f,0.0f,0.0f};
static void Require(bool result,const char* message)
{
    ++checks;
    if(!result) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
static bool Same(const Vector& a,const Vector& b) { return (a-b).LengthSqr()<0.00001f; }
static bool IsFiniteViewAngle(const QAngle& a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
static bool ManualCarryThrowWeaponIdIsSupported(C_WeaponCSBase::WeaponID id)
{ return id==16 || id==17 || id==18 || id==27 || id==28 || id==29; }
static bool ManualInventoryThrowWeaponIdIsSupported(int id) { return id>0; }
static bool ManualInventoryThrowWeaponIdRequiresCustomDrop(int id)
{ return id==13 || id==14 || id==24 || id==1; }
static bool ManualCarryThrowWeaponIdUsesSpawnedPhysicsProp(int id)
{ return id==17 || id==18 || id==27 || id==29; }
static const char* ManualCarryThrowWeaponName(int) { return "fixture"; }
static bool ManualEmptyHandsPlaceholderOwnerIsLiveSurvivor(void* owner)
{ return owner && static_cast<Entity*>(owner)->edict->live; }
static bool ManualThrowGetPlayerOrigin(void*,Vector& origin) { origin=Vector(0.0f,0.0f,0.0f); return true; }
static int ManualPistolExecuteSplit(int,void*,const ManualThrowPending&) { return 0; }
static bool ManualThrowEstimateReleaseVelocity(Player&,int,int,float,Vector& velocity,Vector& angular)
{ velocity=Vector(1.0f,2.0f,3.0f); angular=Vector(4.0f,5.0f,6.0f); return true; }
static bool ManualCarryThrowTeleportDroppedEntity(void* entity,const ManualThrowPending&,bool)
{ ++teleports; teleported=entity; return true; }
static void ManualCarryImpactArm(void*,const ManualThrowPending&) { ++impacts; }
static void* __fastcall NativeOwner(void* object,void*) { return static_cast<Entity*>(object)->owner; }
static void* __fastcall NativeActive(void* object,void*) { return static_cast<Entity*>(object)->active; }
static int __fastcall NativeIndex(void* object,void*) { return static_cast<Entity*>(object)->edict->index; }
static void __fastcall NativeDrop(void* object,void*,void* weapon,const Vector*,const Vector*)
{
    ++dropCalls; static_cast<Entity*>(weapon)->owner=nullptr;
    static_cast<Entity*>(object)->active=replacementWeapon;
}
static void* __cdecl Projectile(const Vector& position,const QAngle&,const Vector&,const Vector&,void*)
{ ++creates; receivedPosition=position; return reinterpret_cast<void*>(0x777u); }
static void* __cdecl CreateNamed(const char*,int,bool) { return spawned; }
static void* __fastcall CreateCarry(void* object,void*)
{
    auto* source=static_cast<Entity*>(object);
    Hooks::dManualCarryCreateEntityByName("physics_prop",-1,false);
    source->owner=nullptr;
    if(removeCarrySource) source->edict->live=false;
    return spawned;
}

#include "throw_lifecycle_functions.inl"

struct Scene
{
    Entity owner{},source{},other{},prop{};
    edict_t ownerEdict{1,11u,true,&owner},sourceEdict{5,21u,true,&source};
    edict_t otherEdict{6,31u,true,&other},propEdict{7,41u,true,&prop};
    Offsets offsets{};
    Game game{};
    VR vr{};
    Scene(int id)
    {
        owner.edict=&ownerEdict; owner.active=&source;
        source.edict=&sourceEdict; source.owner=&owner; source.weaponId=id;
        other.edict=&otherEdict; prop.edict=&propEdict;
        offsets.PhysicalGunOwner.address=reinterpret_cast<uintptr_t>(&NativeOwner);
        offsets.GetActiveWeapon.address=reinterpret_cast<uintptr_t>(&NativeActive);
        offsets.CBaseEntity_entindex.address=reinterpret_cast<uintptr_t>(&NativeIndex);
        offsets.ManualInventoryWeaponDrop.address=reinterpret_cast<uintptr_t>(&NativeDrop);
        game.m_Offsets=&offsets; game.m_CurrentUsercmdPlayer=&owner;
        Hooks::m_Game=&game; Hooks::m_VR=&vr;
        Hooks::hkManualCarryCreatePhysicsProp.fOriginal=reinterpret_cast<void*(__thiscall*)(void*)>(&CreateCarry);
        Hooks::hkManualCarryCreateEntityByName.fOriginal=&CreateNamed;
        spawned=&prop; replacementWeapon=&other;
        dropCalls=teleports=impacts=creates=0; teleported=nullptr; removeCarrySource=false;
        for(auto& slot:s_ManualCarrySpawnedPhysicsProps) slot={};
    }
    Player& PlayerState() { return game.m_PlayersVRInfo[1]; }
    bool Prepare(bool inventory=false)
    { return ManualThrowPreparePending(PlayerState(),&owner,&source,source.weaponId,100,inventory); }
    void Create(ManualThrowableKind kind=ManualThrowableKind::PipeBomb)
    { ManualThrowProjectileCreate(kind,&Projectile,nativePosition,QAngle(0.0f,0.0f,0.0f),Vector(10.0f,0.0f,0.0f),Vector(0.0f,0.0f,0.0f),&owner); }
};

int main()
{
    for(const auto kind : {ManualThrowableKind::Molotov,ManualThrowableKind::PipeBomb,ManualThrowableKind::VomitJar})
    {
        Scene s(ManualThrowExpectedWeaponId(kind));
        Require(s.Prepare(),"A normal native grenade release binds its actual owner and source identity");
        const Vector physical=s.PlayerState().manualThrowPending.origin;
        s.Create(kind);
        Require(creates==1 && Same(receivedPosition,physical) && !s.PlayerState().manualThrowPending.valid,
            "Native projectile Create consumes exactly its projectile pending");
    }
    {
        Scene s(14); Require(s.Prepare(),"A grenade release prepares before a slot switch");
        s.owner.active=&s.other;
        Require(!ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==0 && impacts==0,
            "Switching slots is not a native grenade drop or physical detachment");
        Require(s.PlayerState().manualThrowPending.valid,"The carry consumer cannot steal a native projectile pending");
        Require(!ManualInventoryThrowExecutePendingDrop(1,&s.owner) && s.PlayerState().manualThrowPending.valid,
            "The inventory consumer cannot clear a delayed native projectile pending");
    }
    {
        Scene s(14); Require(s.Prepare(true),"Use plus trigger creates an inventory-only pending");
        s.Create();
        Require(Same(receivedPosition,nativePosition) && s.PlayerState().manualThrowPending.valid,
            "A delayed projectile keeps native arguments and cannot consume inventory pending");
        Require(ManualInventoryThrowExecutePendingDrop(1,&s.owner) && dropCalls==1,"The inventory route executes one native drop");
        Require(ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==1 && teleported==&s.source,
            "A confirmed inventory drop may position its same-serial dropped weapon");
    }
    for(const bool ownerReuse : {false,true})
    {
        Scene s(14); Require(s.Prepare(true),"A deferred inventory request captures current serials");
        if(ownerReuse) ++s.ownerEdict.serial; else ++s.sourceEdict.serial;
        Require(!ManualInventoryThrowExecutePendingDrop(1,&s.owner) && dropCalls==0 && !s.PlayerState().manualThrowPending.valid,
            "Same-address same-vtable owner or weapon serial reuse cannot drop a replacement entity");
    }
    {
        Scene s(14); Require(s.Prepare(),"A projectile request captures its source serial");
        ++s.sourceEdict.serial; s.Create();
        Require(Same(receivedPosition,nativePosition),"Source serial reuse falls back to native projectile arguments");
    }
    for(const bool ownerBindingUnavailable : {false,true})
    {
        Scene s(14);
        if(ownerBindingUnavailable) s.offsets.PhysicalGunOwner.valid=false; else s.sourceEdict.live=false;
        if(ownerBindingUnavailable)
            Require(!ManualInventoryThrowBackendIsReady(14) && !ManualCarryThrowBackendIsReady(16),
                "Missing owner binding cannot advertise custom input suppression or drop support");
        Require(!s.Prepare(),"Unavailable native identity or owner binding cannot invent manual throw pending");
        s.Create(); Require(creates==1 && Same(receivedPosition,nativePosition),"Unavailable manual proof preserves native projectile Create");
    }
    {
        Scene s(14); s.offsets.CBaseEntity_entindex.valid=false;
        Require(!ManualInventoryThrowBackendIsReady(14) && !ManualCarryThrowBackendIsReady(16),
            "Missing native identity binding cannot advertise custom carry or inventory drop support");
        Require(!s.Prepare(),"Missing identity binding preserves native projectile preparation ownership");
        s.Create(); Require(Same(receivedPosition,nativePosition),"Missing identity binding keeps native Create arguments unchanged");
    }
    {
        Scene s(14); s.source.weaponId=16;
        Require(!ManualThrowPreparePending(s.PlayerState(),&s.owner,&s.source,14,100,false),
            "A switch-time source of a different native weapon type cannot masquerade as the released pipe bomb");
    }
    {
        Scene s(16); Require(s.Prepare(),"A normal carried object prepares its actual source");
        s.owner.active=&s.other;
        Require(!ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==0,
            "An inactive but still-owned carry object is not detached");
    }
    {
        Scene s(16); Require(s.Prepare(),"A normal carry release prepares before native detachment");
        s.source.owner=nullptr; s.owner.active=&s.other;
        Require(ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==1 && impacts==1,
            "A confirmed owner-cleared carry object keeps its physical throw velocity");
    }
    for(const bool deleteInsideCreate : {false,true})
    {
        Scene s(17); Require(s.Prepare(),"A carry prop release captures its source before native creation");
        removeCarrySource=deleteInsideCreate;
        Hooks::dManualCarryCreatePhysicsProp(&s.source,nullptr); s.owner.active=&s.other;
        Require(ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==1 && teleported==&s.prop,
            "Exact source/prop capture survives source deletion inside native CreatePhysicsProp");
    }
    for(const bool reuseSource : {false,true})
    {
        Scene s(17); Require(s.Prepare(),"A carry request captures serials for later native creation");
        if(reuseSource) ++s.sourceEdict.serial;
        Hooks::dManualCarryCreatePhysicsProp(&s.source,nullptr); s.owner.active=&s.other;
        if(!reuseSource) ++s.propEdict.serial;
        Require(!ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==0,
            "Same-address source or spawned-prop serial reuse cannot teleport the replacement");
    }
    {
        Scene s(17); Require(s.Prepare(),"An ordinary carry request waits for its own native factory");
        Hooks::dManualCarryCreateEntityByName("physics_prop",-1,false);
        s.source.owner=nullptr; s.owner.active=&s.other;
        Require(!ManualCarryThrowApplyPendingAfterWeaponDetached(1,&s.owner,&s.other,true) && teleports==0,
            "An unrelated generic physics_prop allocation cannot become the pending throw target");
    }
    {
        Scene s(14); Require(s.Prepare(),"The age guard receives a valid native pending");
        s.PlayerState().manualThrowLastTick=std::numeric_limits<int>::max();
        s.PlayerState().manualThrowPending.releaseTick=std::numeric_limits<int>::min();
        s.Create(); Require(Same(receivedPosition,nativePosition) && !s.PlayerState().manualThrowPending.valid,
            "Stale tick pairs are rejected without signed subtraction overflow");
    }
    std::printf("PASS: native throw lifecycle (%d checks)\n",checks);
}
