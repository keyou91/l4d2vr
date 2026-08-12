#pragma once

#include <iostream>
#include <cstdio>
#include <typeinfo>
#include <cstdint>

#include "MinHook.h"

// We call Game::logMsg / Game::errorMsg here, so we need the full declaration.
#include "game.h"

class VR;
class ITexture;
class IMaterial;
class CViewSetup;
class CUserCmd;
class QAngle;
class Vector;
class matrix3x4_t;
class edict_t;
class ModelRenderInfo_t;
class C_BasePlayer;
class Server_BaseEntity;

template <typename T>
struct Hook
{
	T fOriginal;
	LPVOID pTarget;
	bool isEnabled;

	int createHook(LPVOID targetFunc, LPVOID detourFunc)
	{
		if (!targetFunc)
		{
			Game::errorMsg("Failed to create hook: targetFunc is NULL (offset/signature scan failed?)");
			return 1;
		}

		if (MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<LPVOID*>(&fOriginal)) != MH_OK)
		{
			char errorString[512];
			sprintf_s(errorString, 512, "Failed to create hook with this signature: %s", typeid(T).name());
			Game::errorMsg(errorString);
			return 1;
		}
		pTarget = targetFunc;
		return 0;
	}

	int enableHook()
	{
		if (MH_EnableHook(pTarget) != MH_OK)
		{
			Game::errorMsg("Failed to enable hook");
			return 1;
		}
		isEnabled = true;
		return 0;
	}

	int disableHook()
	{
		if (MH_DisableHook(pTarget) != MH_OK)
		{
			Game::errorMsg("Failed to disable hook");
			return 1;
		}
		isEnabled = false;
		return 0;
	}
};


// Source Engine functions
typedef ITexture* (__thiscall* tGetRenderTarget)(void* thisptr);
typedef void(__thiscall* tRenderView)(void* thisptr, CViewSetup& setup, CViewSetup& hudViewSetup, int nClearFlags, int whatToDraw);
typedef bool(__thiscall* tCreateMove)(void* thisptr, float flInputSampleTime, CUserCmd* cmd);
typedef void(__thiscall* tEndFrame)(PVOID);
typedef void(__thiscall* tCalcViewModelView)(void* thisptr, void* owner, const Vector& eyePosition, const QAngle& eyeAngles);
typedef int(__cdecl* tFireTerrorBullets)(int playerId, const Vector& vecOrigin, const QAngle& vecAngles, int a4, int a5, int a6, float a7);
typedef float(__thiscall* tProcessUsercmds)(void* thisptr, edict_t* player, void* buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused);
typedef void(__thiscall* tServerGameClientsClientCommand)(
	void* thisptr,
	edict_t* player,
	const void* sourceCommand);
typedef int(__cdecl* tReadUsercmd)(void* buf, CUserCmd* move, CUserCmd* from);
typedef void(__thiscall* tWriteUsercmdDeltaToBuffer)(void* thisptr, int a1, void* buf, int from, int to, bool isnewcommand);
typedef int(__cdecl* tWriteUsercmd)(void* buf, CUserCmd* to, CUserCmd* from);
typedef int(__cdecl* tAdjustEngineViewport)(int& x, int& y, int& width, int& height);
typedef void(__thiscall* tViewport)(void* thisptr, int x, int y, int width, int height);
typedef void(__thiscall* tGetViewport)(void* thisptr, int& x, int& y, int& width, int& height);
typedef void(__thiscall* tDrawScreenSpaceRectangle)(
	void* thisptr, IMaterial* material,
	int destX, int destY, int width, int height,
	float srcX0, float srcY0, float srcX1, float srcY1,
	int srcWidth, int srcHeight,
	void* clientRenderable, int xDice, int yDice);
typedef int(__thiscall* tTestMeleeSwingCollision)(void* thisptr, Vector const& vec);
typedef void(__thiscall* tDoMeleeSwing)(void* thisptr);
typedef void(__thiscall* tStartMeleeSwing)(void* thisptr, void* player, bool a3);
typedef int(__thiscall* tPrimaryAttack)(void* thisptr);
typedef void(__thiscall* tItemPostFrame)(void* thisptr);
typedef void* (__cdecl* tThrowableProjectileCreate)(const Vector& position, const QAngle& angles, const Vector& velocity, const Vector& angularVelocity, void* owner);
typedef void* (__cdecl* tManualCarryCreateEntityByName)(const char* className, int forcedEdictIndex, bool runScriptHook);
typedef void* (__thiscall* tManualCarryCreatePhysicsProp)(void* thisptr);
typedef void(__thiscall* tCBaseEntityVPhysicsCollision)(void* thisptr, int index, void* collisionEvent);
typedef int(__thiscall* tGetPrimaryAttackActivity)(void* thisptr, void* meleeInfo);
typedef Vector* (__thiscall* tEyePosition)(void* thisptr, Vector* eyePos);
typedef void(__thiscall* tEyeVectors)(void* thisptr, Vector* forward, Vector* right, Vector* up);
typedef const QAngle* (__thiscall* tEyeAngles)(void* thisptr);
typedef Server_BaseEntity* (__thiscall* tFindUseEntity)(void* thisptr, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra);
typedef C_BaseEntity* (__thiscall* tClientFindUseEntity)(void* thisptr, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra);
typedef void(__thiscall* tPlayerUse)(void* thisptr, void* useEntity);
typedef void(__thiscall* tDrawModelExecute)(void* thisptr, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld);
typedef bool(__thiscall* tFirstPersonBodyRenderableShouldDraw)(void* thisptr);
typedef void* (__thiscall* tFirstPersonBodyRenderableGetModel)(void* thisptr);
typedef int(__thiscall* tFirstPersonBodyRenderableDrawModel)(void* thisptr, int flags, const void* instance);
typedef bool(__thiscall* tWorldPoseWeaponSetupBones)(
	void* thisptr,
	matrix3x4_t* boneToWorldOut,
	int maxBones,
	int boneMask,
	float currentTime);
typedef void(__thiscall* tPushRenderTargetAndViewport)(void* thisptr, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
typedef void(__thiscall* tPopRenderTargetAndViewport)(void* thisptr);
typedef void(__thiscall* tVgui_Paint)(void* thisptr, int mode);
typedef int(__cdecl* tIsSplitScreen)();
typedef DWORD* (__thiscall* tPrePushRenderTarget)(void* thisptr, int a2);
typedef void(__thiscall* tUpdateLaserSight)(void* thisptr);
typedef void(__thiscall* tUpdateFlashlight)(void* thisptr, int entIndex, const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, float fov, float farZ, float linearAtten, bool castsShadows, const char* textureName);
typedef void(__thiscall* tUpdateFlashlightColor)(void* thisptr, int entIndex, const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, int color, bool castsShadows, int textureId, const Vector& colorVector, bool something);
typedef void(__thiscall* tConVarSetValueString)(void* thisptr, const char* value);
typedef void(__thiscall* tConVarSetValueFloat)(void* thisptr, float value);
typedef void(__thiscall* tConVarSetValueInt)(void* thisptr, int value);
typedef void(__thiscall* tConVarInternalSetValueString)(void* thisptr, const char* value);
typedef void(__thiscall* tConVarInternalSetValueFloat)(void* thisptr, float value);
typedef void(__thiscall* tConVarInternalSetValueInt)(void* thisptr, int value);
typedef void(__cdecl* tHudUserMessage)(void* msgData);
// L4D2 IEngineSoundClient003 vfuncs #5 and #6. The recipient filter reference is passed as a pointer at the ABI boundary.
typedef void(__thiscall* tEmitSoundAttenuation)(void* thisptr, void* filter, int entIndex, int channel, const char* sample, float volume, float attenuation, int flags, int pitch, const Vector* origin, const Vector* direction, void* origins, bool updatePositions, float soundTime, int speakerEntity);
typedef void(__thiscall* tEmitSoundLevel)(void* thisptr, void* filter, int entIndex, int channel, const char* sample, float volume, int soundLevel, int flags, int pitch, const Vector* origin, const Vector* direction, void* origins, bool updatePositions, float soundTime, int speakerEntity);


class Hooks
{
public:
	static inline Game* m_Game;
	static inline VR* m_VR;

	static inline Hook<tGetRenderTarget> hkGetRenderTarget;
	static inline Hook<tRenderView> hkRenderView;
	static inline Hook<tCreateMove> hkCreateMove;
	static inline Hook<tEndFrame> hkEndFrame;
	static inline Hook<tCalcViewModelView> hkCalcViewModelView;
	static inline Hook<tFireTerrorBullets> hkServerFireTerrorBullets;
	static inline Hook<tFireTerrorBullets> hkClientFireTerrorBullets;
	static inline Hook<tProcessUsercmds> hkProcessUsercmds;
	static inline Hook<tServerGameClientsClientCommand>
		hkServerGameClientsClientCommand;
	static inline Hook<tReadUsercmd> hkReadUsercmd;
	static inline Hook<tWriteUsercmdDeltaToBuffer> hkWriteUsercmdDeltaToBuffer;
	static inline Hook<tWriteUsercmd> hkWriteUsercmd;
	static inline Hook<tAdjustEngineViewport> hkAdjustEngineViewport;
	static inline Hook<tViewport> hkViewport;
	static inline Hook<tGetViewport> hkGetViewport;
	static inline Hook<tDrawScreenSpaceRectangle> hkDrawScreenSpaceRectangle;
	static inline Hook<tTestMeleeSwingCollision> hkTestMeleeSwingCollisionClient;
	static inline Hook<tTestMeleeSwingCollision> hkTestMeleeSwingCollisionServer;
	static inline Hook<tDoMeleeSwing> hkDoMeleeSwingServer;
	static inline Hook<tStartMeleeSwing> hkStartMeleeSwingServer;
	static inline Hook<tPrimaryAttack> hkPrimaryAttackServer;
	static inline Hook<tItemPostFrame> hkItemPostFrameServer;
	static inline Hook<tThrowableProjectileCreate> hkMolotovProjectileCreate;
	static inline Hook<tThrowableProjectileCreate> hkPipeBombProjectileCreate;
	static inline Hook<tThrowableProjectileCreate> hkVomitJarProjectileCreate;
	static inline Hook<tManualCarryCreateEntityByName> hkManualCarryCreateEntityByName;
	static inline Hook<tManualCarryCreatePhysicsProp> hkManualCarryCreatePhysicsProp;
	static inline Hook<tCBaseEntityVPhysicsCollision> hkCBaseEntityVPhysicsCollision;
	static inline Hook<tGetPrimaryAttackActivity> hkGetPrimaryAttackActivity;
	static inline Hook<tEyePosition> hkEyePosition;
	static inline Hook<tEyePosition> hkServerPlayerEyePosition;
	static inline Hook<tEyePosition> hkClientPlayerEyePosition;
	static inline Hook<tEyeVectors> hkClientPlayerEyeVectors;
	static inline Hook<tEyeAngles> hkServerPlayerEyeAngles;
	static inline Hook<tFindUseEntity> hkFindUseEntity;
	static inline Hook<tClientFindUseEntity> hkClientFindUseEntity;
	static inline Hook<tPlayerUse> hkPlayerUse;
	static inline Hook<tDrawModelExecute> hkDrawModelExecute;
	static inline Hook<tFirstPersonBodyRenderableShouldDraw> hkFirstPersonBodyRenderableShouldDraw;
	static inline Hook<tFirstPersonBodyRenderableGetModel> hkFirstPersonBodyRenderableGetModel;
	static inline Hook<tFirstPersonBodyRenderableDrawModel> hkFirstPersonBodyRenderableDrawModel;
	static inline Hook<tWorldPoseWeaponSetupBones> hkWorldPoseWeaponSetupBones;
	static inline Hook<tPushRenderTargetAndViewport> hkPushRenderTargetAndViewport;
	static inline Hook<tPopRenderTargetAndViewport> hkPopRenderTargetAndViewport;
	static inline Hook<tVgui_Paint> hkVgui_Paint;
	static inline Hook<tIsSplitScreen> hkIsSplitScreen;
	static inline Hook<tPrePushRenderTarget> hkPrePushRenderTarget;
	static inline Hook<tUpdateLaserSight> hkUpdateLaserSight;
	static inline Hook<tUpdateFlashlight> hkUpdateFlashlight;
	static inline Hook<tUpdateFlashlightColor> hkUpdateFlashlightColor;
	static inline Hook<tConVarSetValueString> hkConVarSetValueString;
	static inline Hook<tConVarSetValueFloat> hkConVarSetValueFloat;
	static inline Hook<tConVarSetValueInt> hkConVarSetValueInt;
	static inline Hook<tConVarSetValueString> hkConVarPrimarySetValueString;
	static inline Hook<tConVarSetValueFloat> hkConVarPrimarySetValueFloat;
	static inline Hook<tConVarSetValueInt> hkConVarPrimarySetValueInt;
	static inline Hook<tConVarInternalSetValueString> hkConVarInternalSetValueString;
	static inline Hook<tConVarInternalSetValueFloat> hkConVarInternalSetValueFloat;
	static inline Hook<tConVarInternalSetValueInt> hkConVarInternalSetValueInt;
	static inline Hook<tHudUserMessage> hkSayText;
	static inline Hook<tHudUserMessage> hkSayText2;
	static inline Hook<tHudUserMessage> hkTextMsg;
	static inline Hook<tEmitSoundAttenuation> hkEmitSoundAttenuation;
	static inline Hook<tEmitSoundLevel> hkEmitSoundLevel;
	static bool s_ServerUnderstandsVR;
	static inline bool s_ManualThrowHooksReady = false;
	static inline bool s_ManualCarryThrowHookReady = false;
	static inline bool s_ManualCarryThrowPropSpawnHookReady = false;
	static inline bool s_ManualCarryImpactKnockbackReady = false;

	Hooks() {};
	Hooks(Game* game);

	~Hooks();

	// -nohmd exits before Game/VR/Hooks construction.  Install only the raw
	// DrawScreenSpaceRectangle detour needed for the passive Neko desktop
	// baseline probe, without starting OpenVR or enabling any gameplay hooks.
	static bool InitializeNoHmdNekoPostProbe();

	int initSourceHooks();

	// Detour functions
	static ITexture* __fastcall dGetRenderTarget(void* ecx, void* edx);
	static void __fastcall dRenderView(void* ecx, void* edx, CViewSetup& setup, CViewSetup& hudViewSetup, int nClearFlags, int whatToDraw);
	static bool __fastcall dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd);
	static void __fastcall dEndFrame(void* ecx, void* edx);
	static void __fastcall dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles);
	static int dServerFireTerrorBullets(int playerId, const Vector& vecOrigin, const QAngle& vecAngles, int a4, int a5, int a6, float a7);
	static int dClientFireTerrorBullets(int playerId, const Vector& vecOrigin, const QAngle& vecAngles, int a4, int a5, int a6, float a7);
	static float __fastcall dProcessUsercmds(void* ecx, void* edx, edict_t* player, void* buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused);
	static void __fastcall dServerGameClientsClientCommand(
		void* ecx,
		void* edx,
		edict_t* player,
		const void* sourceCommand);
	static int dReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from);
	static void __fastcall dWriteUsercmdDeltaToBuffer(void* ecx, void* edx, int a1, void* buf, int from, int to, bool isnewcommand);
	static int dWriteUsercmd(void* buf, CUserCmd* to, CUserCmd* from);
	static void dAdjustEngineViewport(int& x, int& y, int& width, int& height);
	static void __fastcall dViewport(void* ecx, void* edx, int x, int y, int width, int height);
	static void __fastcall dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height);
	static void __fastcall dDrawScreenSpaceRectangle(
		void* ecx, void* edx, IMaterial* material,
		int destX, int destY, int width, int height,
		float srcX0, float srcY0, float srcX1, float srcY1,
		int srcWidth, int srcHeight,
		void* clientRenderable, int xDice, int yDice);
	static int __fastcall dTestMeleeSwingCollisionClient(void* ecx, void* edx, Vector const& vec);
	static int __fastcall dTestMeleeSwingCollisionServer(void* ecx, void* edx, Vector const& vec);
	static void __fastcall dDoMeleeSwingServer(void* ecx, void* edx);
	static void __fastcall dStartMeleeSwingServer(void* ecx, void* edx, void* player, bool a3);
	static int __fastcall dPrimaryAttackServer(void* ecx, void* edx);
	static void __fastcall dItemPostFrameServer(void* ecx, void* edx);
	static void* __cdecl dMolotovProjectileCreate(const Vector& position, const QAngle& angles, const Vector& velocity, const Vector& angularVelocity, void* owner);
	static void* __cdecl dPipeBombProjectileCreate(const Vector& position, const QAngle& angles, const Vector& velocity, const Vector& angularVelocity, void* owner);
	static void* __cdecl dVomitJarProjectileCreate(const Vector& position, const QAngle& angles, const Vector& velocity, const Vector& angularVelocity, void* owner);
	static void* __cdecl dManualCarryCreateEntityByName(const char* className, int forcedEdictIndex, bool runScriptHook);
	static void* __fastcall dManualCarryCreatePhysicsProp(void* ecx, void* edx);
	static void __fastcall dCBaseEntityVPhysicsCollision(void* ecx, void* edx, int index, void* collisionEvent);
	static int __fastcall dGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo);
	static Vector* __fastcall dEyePosition(void* ecx, void* edx, Vector* eyePos);
	static Vector* __fastcall dServerPlayerEyePosition(void* ecx, void* edx, Vector* eyePos);
	static Vector* __fastcall dClientPlayerEyePosition(void* ecx, void* edx, Vector* eyePos);
	static void __fastcall dClientPlayerEyeVectors(void* ecx, void* edx, Vector* forward, Vector* right, Vector* up);
	static const QAngle* __fastcall dServerPlayerEyeAngles(void* ecx, void* edx);
	static Server_BaseEntity* __fastcall dFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra);
	static C_BaseEntity* __fastcall dClientFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra);
	static void __fastcall dPlayerUse(void* ecx, void* edx, void* useEntity);
	static void __fastcall dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld);
	static bool __fastcall dFirstPersonBodyRenderableShouldDraw(void* ecx, void* edx);
	static void* __fastcall dFirstPersonBodyRenderableGetModel(void* ecx, void* edx);
	static int __fastcall dFirstPersonBodyRenderableDrawModel(void* ecx, void* edx, int flags, const void* instance);
	static bool __fastcall dWorldPoseWeaponSetupBones(
		void* ecx,
		void* edx,
		matrix3x4_t* boneToWorldOut,
		int maxBones,
		int boneMask,
		float currentTime);
	static void __fastcall dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
	static void __fastcall dPopRenderTargetAndViewport(void* ecx, void* edx);
	static void __fastcall dVGui_Paint(void* ecx, void* edx, int mode);
	static int __fastcall dIsSplitScreen();
	static DWORD* __fastcall dPrePushRenderTarget(void* ecx, void* edx, int a2);
	static void __fastcall dUpdateLaserSight(void* ecx, void* edx);
	static void __fastcall dUpdateFlashlight(void* ecx, void* edx, int entIndex, const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, float fov, float farZ, float linearAtten, bool castsShadows, const char* textureName);
	static void __fastcall dUpdateFlashlightColor(void* ecx, void* edx, int entIndex, const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, int color, bool castsShadows, int textureId, const Vector& colorVector, bool something);
	static void __fastcall dConVarSetValueString(void* ecx, void* edx, const char* value);
	static void __fastcall dConVarSetValueFloat(void* ecx, void* edx, float value);
	static void __fastcall dConVarSetValueInt(void* ecx, void* edx, int value);
	static void __fastcall dConVarPrimarySetValueString(void* ecx, void* edx, const char* value);
	static void __fastcall dConVarPrimarySetValueFloat(void* ecx, void* edx, float value);
	static void __fastcall dConVarPrimarySetValueInt(void* ecx, void* edx, int value);
	static void __fastcall dConVarInternalSetValueString(void* ecx, void* edx, const char* value);
	static void __fastcall dConVarInternalSetValueFloat(void* ecx, void* edx, float value);
	static void __fastcall dConVarInternalSetValueInt(void* ecx, void* edx, int value);
	static void dSayText(void* msgData);
	static void dSayText2(void* msgData);
	static void dTextMsg(void* msgData);
	static void __fastcall dEmitSoundAttenuation(void* ecx, void* edx, void* filter, int entIndex, int channel, const char* sample, float volume, float attenuation, int flags, int pitch, const Vector* origin, const Vector* direction, void* origins, bool updatePositions, float soundTime, int speakerEntity);
	static void __fastcall dEmitSoundLevel(void* ecx, void* edx, void* filter, int entIndex, int channel, const char* sample, float volume, int soundLevel, int flags, int pitch, const Vector* origin, const Vector* direction, void* origins, bool updatePositions, float soundTime, int speakerEntity);

	// HUD render-target interception uses a small state machine to detect the
	// engine's "push HUD RT" sequence:
	//   PopRenderTargetAndViewport -> IsSplitScreen -> PrePushRenderTarget -> PushRenderTargetAndViewport
	// In mat_queue_mode != 0 (queued/multicore), this sequence isn't reliable, so we
	// fall back to explicit VGui_Paint redirection.
	enum class HUDPushStep : int
	{
		None = -1,
		AfterPop,
		AfterIsSplitScreen,
		ReadyToOverride,
	};

	static inline thread_local HUDPushStep m_HUDStep = HUDPushStep::None;
	static inline thread_local bool m_PushedHud = false;
	static inline thread_local bool m_ServerCommandControllerAimOverride = false;
	static inline thread_local void* m_ServerCommandControllerAimPlayer = nullptr;
	static inline thread_local Vector m_ServerCommandControllerAimOrigin = { 0.0f, 0.0f, 0.0f };
	static inline thread_local QAngle m_ServerCommandControllerAimAngles = { 0.0f, 0.0f, 0.0f };
	static inline thread_local int m_ServerCommandControllerAimReason = 0;
	static inline thread_local bool m_ServerProcessingUsercmd = false;
	static inline thread_local void* m_ServerProcessingUsercmdPlayer = nullptr;
	static inline thread_local int m_ServerProcessingUsercmdPlayerIndex = -1;
	static inline thread_local bool m_ServerPacketSawVRUsercmd = false;
};
