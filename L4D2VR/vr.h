#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "openvr.h"
#include "vector.h"
#include "vr_hands/vr_hand_types.h"
#include <cstdint>
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>
#define MAX_STR_LEN 256

class Game;
class C_BaseEntity;
class C_BasePlayer;
class C_WeaponCSBase;

bool L4D2VR_ApplyRecommendedVideoSettings();
extern "C" void L4D2VR_D3D9_SetForceDeviceLock(int enabled);
class CUserCmd;
struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
class ITexture;
class IMaterial;
class IMatRenderContext;
class CViewSetup;
class IGameEvent;
class IGameEventListener2;
class IGameEventManager2;
class VrHandSystem;
enum class VrHandDrawPass;

struct ViewmodelAdjustment
{
	Vector position;
	QAngle angle;
};

struct ScopeAdjustment
{
	float fov = 20.0f;
	float widthMeters = 0.30f;
	Vector overlayOffset = { 0.02f, 0.04f, -0.06f };
};


struct TrackedDevicePoseData
{
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

struct SharedTextureHolder
{
	vr::VRVulkanTextureData_t m_VulkanData{};
	vr::Texture_t m_VRTexture{};
};

using TextureStateMutex = std::recursive_mutex;

struct PendingOverlayTextureBind
{
	vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
	const vr::Texture_t* texture = nullptr;
	IDirect3DSurface9* surface = nullptr;
	bool hideOnFailure = false;
};

struct PendingOverlayTextureBindBatch
{
	static constexpr size_t kCapacity = 8;
	std::array<PendingOverlayTextureBind, kCapacity> binds{};
	size_t count = 0;

	bool Stage(
		vr::VROverlayHandle_t overlay,
		const vr::Texture_t* texture,
		IDirect3DSurface9* surface = nullptr,
		bool hideOnFailure = false)
	{
		if (!texture || count >= binds.size())
			return false;
		binds[count++] = { overlay, texture, surface, hideOnFailure };
		return true;
	}

	void Clear()
	{
		count = 0;
	}
};

struct CustomActionBinding
{
	std::string command;
	std::string releaseCommand;
	std::optional<WORD> virtualKey;
	bool holdVirtualKey = false;
	bool usePressReleaseCommands = false;
};

struct LeftHandedMirroredActionSource
{
	vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
	vr::VRInputValueHandle_t restrictToDevice = vr::k_ulInvalidInputValueHandle;
};

struct LeftHandedDigitalActionFrameCache
{
	uint64_t frameSerial = 0;
	bool dataValid = false;
	bool previousStateValid = false;
	bool previousState = false;
	vr::InputDigitalActionData_t data{};
};

struct LeftHandedAnalogActionFrameCache
{
	uint64_t frameSerial = 0;
	bool dataValid = false;
	vr::InputAnalogActionData_t data{};
};

struct WeaponHapticsProfile
{
	float durationSeconds = 0.0f;
	float frequency = 0.0f;
	float amplitude = 0.0f;
};

struct HapticMixState
{
	bool pending = false;
	float amplitude = 0.0f;
	float frequency = 0.0f;
	float durationSeconds = 0.0f;
	float weight = 0.0f;
	int priority = -1;
	std::chrono::steady_clock::time_point lastSubmit{};
};

enum class MagazineInteractionManualState
{
	Idle,
	HoldingOldMagazine,
	WaitingForFreshMagazine,
	HoldingFreshMagazine,
	WaitingForBackendReload,
	WaitingForBoltGrab,
	HoldingBolt,
	AutoBolting
};

struct MagazineInteractionBoxSnapshot
{
	Vector origin;
	Vector axisX = { 1.0f, 0.0f, 0.0f };
	Vector axisY = { 0.0f, 1.0f, 0.0f };
	Vector axisZ = { 0.0f, 0.0f, 1.0f };
	Vector pullAxisWorld = { 0.0f, 0.0f, 0.0f };
	Vector modelOrigin = { 0.0f, 0.0f, 0.0f };
	Vector modelAxisX = { 1.0f, 0.0f, 0.0f };
	Vector modelAxisY = { 0.0f, 1.0f, 0.0f };
	Vector modelAxisZ = { 0.0f, 0.0f, 1.0f };
	Vector viewmodelAnchorOrigin = { 0.0f, 0.0f, 0.0f };
	Vector viewmodelAnchorAxisX = { 1.0f, 0.0f, 0.0f };
	Vector viewmodelAnchorAxisY = { 0.0f, 1.0f, 0.0f };
	Vector viewmodelAnchorAxisZ = { 0.0f, 0.0f, 1.0f };
	Vector mins;
	Vector maxs;
	uint32_t frameSeq = 0;
	uint32_t publishSeq = 0;
	int entityIndex = -1;
	int boneIndex = -1;
	bool modelBasisValid = false;
	bool viewmodelAnchorBasisValid = false;
	std::string modelName;
	std::chrono::steady_clock::time_point publishedAt{};
};

struct MagazineInteractionCalibrationBone
{
	int index = -1;
	int parent = -1;
	int magazineScore = 0;
	int boltScore = 0;
	bool validOrigin = false;
	Vector origin = { 0.0f, 0.0f, 0.0f };
	std::string name;
};

struct MagazineInteractionCalibrationSnapshot
{
	bool valid = false;
	std::string modelName;
	std::string sourceClassName;
	uint32_t modelFingerprint = 0;
	uint32_t boneSignature = 0;
	uint32_t renderFrameSeq = 0;
	uint32_t publishSeq = 0;
	int entityIndex = -1;
	int weaponId = 0;
	int inferredWeaponId = 0;
	int sourceScore = 0;
	int numBones = 0;
	int recommendedMagazineBone = -1;
	int recommendedBoltBone = -1;
	bool sourceIsViewmodelClass = false;
	std::vector<MagazineInteractionCalibrationBone> bones;
	std::chrono::steady_clock::time_point publishedAt{};
};

struct D3DAimLineOverlayEyeState
{
	bool valid = false;
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float widthPixels = 0.0f;
	float outlinePixels = 0.0f;
	float endpointPixels = 0.0f;
	uint32_t color = 0;
	uint32_t outlineColor = 0;
};

class VR
{
public:
	Game* m_Game = nullptr;

	vr::IVRSystem* m_System = nullptr;
	vr::IVRInput* m_Input = nullptr;
	vr::IVROverlay* m_Overlay = nullptr;
	vr::IVRCompositor* m_Compositor = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle;
	vr::VROverlayHandle_t m_HUDTopHandle;
	std::array<vr::VROverlayHandle_t, 4> m_HUDBottomHandles{};
	// Gun-mounted scope overlay (render-to-texture lens)
	vr::VROverlayHandle_t m_ScopeHandle = vr::k_ulOverlayHandleInvalid;
	// Rear mirror overlay (off-hand)
	vr::VROverlayHandle_t m_RearMirrorHandle = vr::k_ulOverlayHandleInvalid;
	// Hand HUD overlays (raw, controller-anchored)
	vr::VROverlayHandle_t m_LeftWristHudHandle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t m_RightAmmoHudHandle = vr::k_ulOverlayHandleInvalid;
	// Standalone in-game HUD alert panel for special infected intent sense.
	vr::VROverlayHandle_t m_SpecialInfectedIntentSenseHudHandle = vr::k_ulOverlayHandleInvalid;


	float m_HorizontalOffsetLeft;
	float m_VerticalOffsetLeft;
	float m_HorizontalOffsetRight;
	float m_VerticalOffsetRight;

	uint32_t m_RenderWidth;
	uint32_t m_RenderHeight;
	uint32_t m_AntiAliasing = 0;
	bool m_EyeRenderTargetMatchProjectionAspect = false;
	float m_Aspect;
	float m_Fov;

	vr::VRTextureBounds_t m_TextureBounds[2];
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount];

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward;
	Vector m_HmdRight;
	Vector m_HmdUp;

	Vector m_HmdPosLocalInWorld = { 0,0,0 };

	Vector m_LeftControllerForward;
	Vector m_LeftControllerRight;
	Vector m_LeftControllerUp;

	Vector m_RightControllerForwardUnforced = { 0,0,0 };
	Vector m_RightControllerForward;
	Vector m_RightControllerRight;
	Vector m_RightControllerUp;

	Vector m_ViewmodelForward;
	Vector m_ViewmodelRight;
	Vector m_ViewmodelUp;

	Vector m_HmdPosAbs = { 0,0,0 };
	Vector m_HmdPosAbsPrev = { 0,0,0 };
	QAngle m_HmdAngAbs;

	Vector m_HmdPosCorrectedPrev = { 0,0,0 };
	Vector m_HmdPosLocalPrev = { 0,0,0 };
	Vector m_LeftControllerPosSmoothed = { 0,0,0 };
	Vector m_RightControllerPosSmoothed = { 0,0,0 };
	QAngle m_LeftControllerAngSmoothed = { 0,0,0 };
	QAngle m_RightControllerAngSmoothed = { 0,0,0 };

	Vector m_SetupOrigin = { 0,0,0 };
	QAngle m_SetupAngles = { 0,0,0 };
	Vector m_SetupOriginPrev = { 0,0,0 };
	Vector m_CameraAnchor = { 0,0,0 };
	Vector m_SetupOriginToHMD = { 0,0,0 };

	float m_HeightOffset = 0.0;
	static constexpr uint32_t kResetPositionStableFramesRequired = 8;
	std::atomic<uint32_t> m_ResetPositionStableFrames{ 0 };
	std::atomic<uint32_t> m_ResetPositionDeferredPending{ 0 };
	bool m_ResetPositionStableEyeZValid = false;
	float m_ResetPositionStableEyeZ = 0.0f;
	bool m_AutoRecenterSmooth = true;
	float m_AutoRecenterSoftStartDistance = 18.0f;
	float m_AutoRecenterMaxSpeed = 18.0f;
	float m_AutoRecenterHardDistance = 150.0f;
	bool m_RoomscaleActive = false;
	bool m_IsThirdPersonCamera = false;
	// Death camera flicker guard: after we detect the local player has died,
	// force first-person rendering for a short cooldown to avoid 1P<->3P thrash
	// during Source's deathcam/freeze-cam transitions.
	std::chrono::steady_clock::time_point m_DeathFirstPersonLockEnd{};
	bool m_DeathWasAlivePrev = true;
	// When a CustomAction is bound to +walk (press/release), we can optionally treat it
	// as a signal that the gameplay camera has been forced into a third-person mode
	// (e.g. slide mods that switch to 3P while +walk is held).
	bool m_CustomWalkHeld = false;
	bool m_ThirdPersonRenderOnCustomWalk = false;
	// If enabled, render in third-person by default to avoid camera mode flicker.
	// Only a small whitelist of explicitly-handled cases will remain first-person.
	bool m_ThirdPersonDefault = false;
	// If true, third-person camera placement/orbit follows HMD head turns.
	// If false, the rendered view still follows the HMD, but the third-person camera center/offset
	// is placed using the engine/body camera basis so turning your head does not drag the whole camera.
	bool m_ThirdPersonCameraFollowHmd = true;
	// Optional front-observer mode for third-person rendering.
	// When enabled, 3P camera is placed in front of the player and looks back at the player.
	bool m_ThirdPersonFrontViewEnabled = false;
	// In third-person front view, if true use eye/HMD as the scope+aim source.
	// If false, keep scope+aim driven by right controller (recommended).
	bool m_ThirdPersonFrontScopeFromEye = false;
	bool m_ObserverThirdPerson = false;
	// Map-load / reconnect camera stabilization.
	// Source can transiently report observer-like netvars right after joining/changing maps.
	// If we treat that as "real" observer state, we briefly force third-person then snap back.
	int m_ThirdPersonMapLoadCooldownMs = 1500;
	bool m_ThirdPersonMapLoadCooldownPending = false;
	bool m_HadLocalPlayerPrev = false;
	bool m_WasInGamePrev = false;
	std::chrono::steady_clock::time_point m_ThirdPersonMapLoadCooldownEnd{};
	int m_ThirdPersonHoldFrames = 0;
	Vector m_ThirdPersonViewOrigin = { 0,0,0 };
	QAngle m_ThirdPersonViewAngles = { 0,0,0 };
	// Center of the actual VR render camera used this frame (HMD-aimed 3P camera center).
	// Used to keep aim line / overlays in sync when third-person camera is smoothed.
	Vector m_ThirdPersonRenderCenter = { 0,0,0 };
	bool m_ThirdPersonPoseInitialized = false;
	float m_ThirdPersonCameraSmoothing = 0.85f;
	float m_ThirdPersonVRCameraOffset = 38.0f;
	// Front-view third-person camera local offset in camera basis:
	// x=front/back, y=left/right, z=up/down.
	Vector m_ThirdPersonFrontVRCameraOffset = { 80.0f, 30.0f, -15.0f };
	// Third-person scope overlay local offset in body basis (meters):
	// x=front/back, y=left/right, z=up/down.
	Vector m_ThirdPersonScopeOverlayOffset = { 1.5f, 0.4f, -0.3f };
	// Third-person front-view overlay has independent placement/size/FOV/rotation so tuning it
	// does not alter the weapon-mounted scope overlay or normal third-person overlay.
	float m_ThirdPersonFrontViewOverlayWidthMeters = 0.3f;
	float m_ThirdPersonFrontViewOverlayFov = 20.0f;
	Vector m_ThirdPersonFrontViewOverlayOffset = { 1.5f, 0.4f, -0.3f };
	QAngle m_ThirdPersonFrontViewOverlayAngleOffset = { -45.0f, -5.0f, -5.0f };
	Vector m_LeftControllerPosAbs;
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosAbs;
	QAngle m_RightControllerAngAbs;

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;

	// --- Multicore rendering snapshot bridging (mat_queue_mode!=0) ---
	// Main thread publishes a stable copy of key tracking/view parameters; render thread consumes it
	// and computes per-frame view/controller data from a render-thread pose sample.
	std::atomic<uint32_t> m_RenderViewParamsSeq{ 0 };
	std::atomic<float> m_RenderCameraAnchorX{ 0.0f };
	std::atomic<float> m_RenderCameraAnchorY{ 0.0f };
	std::atomic<float> m_RenderCameraAnchorZ{ 0.0f };
	std::atomic<float> m_RenderRotationOffset{ 0.0f };
	std::atomic<float> m_RenderVRScale{ 1.0f };
	std::atomic<float> m_RenderIpdScale{ 1.0f };
	std::atomic<float> m_RenderEyeZ{ 0.0f };
	std::atomic<float> m_RenderIpd{ 0.065f };
	std::atomic<float> m_RenderHmdPosLocalPrevX{ 0.0f };
	std::atomic<float> m_RenderHmdPosLocalPrevY{ 0.0f };
	std::atomic<float> m_RenderHmdPosLocalPrevZ{ 0.0f };
	std::atomic<float> m_RenderHmdPosCorrectedPrevX{ 0.0f };
	std::atomic<float> m_RenderHmdPosCorrectedPrevY{ 0.0f };
	std::atomic<float> m_RenderHmdPosCorrectedPrevZ{ 0.0f };
	std::atomic<float> m_RenderViewmodelPosOffsetX{ 0.0f };
	std::atomic<float> m_RenderViewmodelPosOffsetY{ 0.0f };
	std::atomic<float> m_RenderViewmodelPosOffsetZ{ 0.0f };
	std::atomic<float> m_RenderViewmodelAngOffsetX{ 0.0f };
	std::atomic<float> m_RenderViewmodelAngOffsetY{ 0.0f };
	std::atomic<float> m_RenderViewmodelAngOffsetZ{ 0.0f };

	// Local-player & camera state snapshot for the render thread (mat_queue_mode!=0).
	// NOTE: These are written under the same seqlock as m_RenderViewParamsSeq.
	std::atomic<uint32_t> m_RenderHasLocalPlayer{ 0 };
	std::atomic<float> m_RenderLocalEyePosX{ 0.0f };
	std::atomic<float> m_RenderLocalEyePosY{ 0.0f };
	std::atomic<float> m_RenderLocalEyePosZ{ 0.0f };
	std::atomic<uint32_t> m_RenderHasViewEntityOverride{ 0 };
	std::atomic<int> m_RenderViewEntityHandle{ 0 };
	std::atomic<uint32_t> m_RenderBeingRevived{ 0 };
	std::atomic<uint32_t> m_RenderRevivingOther{ 0 };
	std::atomic<uint32_t> m_RenderUsingMountedGun{ 0 };
	std::atomic<uint32_t> m_RenderPlayerIncap{ 0 };
	std::atomic<uint32_t> m_RenderPlayerControlledBySI{ 0 };
	std::atomic<uint32_t> m_RenderInThirdPersonMapLoadCooldown{ 0 };

	// Third-person state debug snapshot (subset used by the render hook).
	std::atomic<uint32_t> m_RenderTpWantsThirdPerson{ 0 };
	std::atomic<uint32_t> m_RenderTpObserver{ 0 };
	std::atomic<uint32_t> m_RenderTpDead{ 0 };
	std::atomic<int> m_RenderTpLifeState{ 0 };
	std::atomic<int> m_RenderTpObserverMode{ 0 };
	std::atomic<int> m_RenderTpObserverTarget{ 0 };
	std::atomic<uint32_t> m_RenderTpIncap{ 0 };
	std::atomic<uint32_t> m_RenderTpLedge{ 0 };
	std::atomic<uint32_t> m_RenderTpTongue{ 0 };
	std::atomic<uint32_t> m_RenderTpPinned{ 0 };
	std::atomic<uint32_t> m_RenderTpSelfMedkit{ 0 };

	// Aim-line gating computed on the update thread; render thread only consumes.
	std::atomic<uint32_t> m_RenderAimLineAllowed{ 0 };
	std::atomic<uint32_t> m_RenderAimLineShow{ 0 };
	std::atomic<uint32_t> m_RenderWeaponLaserSightActive{ 0 };


	// Render-thread computed snapshot (updated once per dRenderView call).
	std::atomic<uint32_t> m_RenderFrameSeq{ 0 };
	std::atomic<float> m_RenderViewAngX{ 0.0f };
	std::atomic<float> m_RenderViewAngY{ 0.0f };
	std::atomic<float> m_RenderViewAngZ{ 0.0f };
	std::atomic<float> m_RenderViewOriginLeftX{ 0.0f };
	std::atomic<float> m_RenderViewOriginLeftY{ 0.0f };
	std::atomic<float> m_RenderViewOriginLeftZ{ 0.0f };
	std::atomic<float> m_RenderViewOriginRightX{ 0.0f };
	std::atomic<float> m_RenderViewOriginRightY{ 0.0f };
	std::atomic<float> m_RenderViewOriginRightZ{ 0.0f };
	std::atomic<float> m_RenderLeftControllerPosAbsX{ 0.0f };
	std::atomic<float> m_RenderLeftControllerPosAbsY{ 0.0f };
	std::atomic<float> m_RenderLeftControllerPosAbsZ{ 0.0f };
	std::atomic<float> m_RenderLeftControllerAngAbsX{ 0.0f };
	std::atomic<float> m_RenderLeftControllerAngAbsY{ 0.0f };
	std::atomic<float> m_RenderLeftControllerAngAbsZ{ 0.0f };
	std::atomic<float> m_RenderRightControllerPosAbsX{ 0.0f };
	std::atomic<float> m_RenderRightControllerPosAbsY{ 0.0f };
	std::atomic<float> m_RenderRightControllerPosAbsZ{ 0.0f };
	std::atomic<float> m_RenderRightControllerAngAbsX{ 0.0f };
	std::atomic<float> m_RenderRightControllerAngAbsY{ 0.0f };
	std::atomic<float> m_RenderRightControllerAngAbsZ{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelPosX{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelPosY{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelPosZ{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelAngX{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelAngY{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelAngZ{ 0.0f };
	std::atomic<uint32_t> m_ViewmodelMuzzleSmokePoseSeq{ 0 };
	std::atomic<uint32_t> m_ViewmodelMuzzleSmokePoseTickMs{ 0 };
	std::atomic<uint32_t> m_ViewmodelMuzzleSmokeRenderFrameSeq{ 0 };
	std::atomic<float> m_ViewmodelMuzzleSmokePosX{ 0.0f };
	std::atomic<float> m_ViewmodelMuzzleSmokePosY{ 0.0f };
	std::atomic<float> m_ViewmodelMuzzleSmokePosZ{ 0.0f };
	std::atomic<float> m_ViewmodelMuzzleSmokeAngX{ 0.0f };
	std::atomic<float> m_ViewmodelMuzzleSmokeAngY{ 0.0f };
	std::atomic<float> m_ViewmodelMuzzleSmokeAngZ{ 0.0f };

	// Render thread id (captured in dRenderView) used to gate render-only snapshot reads.
	std::atomic<uint32_t> m_RenderThreadId{ 0 };

	// True on the render thread while inside dRenderView when mat_queue_mode!=0.
	static inline thread_local bool t_UseRenderFrameSnapshot = false;

	// --- Pose waiter (mat_queue_mode!=0) ---
	// WaitGetPoses() is a hard pacing barrier. If we call it on the queued render thread, we can
	// destroy mat_queue_mode 2 throughput. Instead, in queued mode we run a tiny "pose waiter" thread
	// that blocks in WaitGetPoses() and publishes a seqlock snapshot. Render/main threads only read.
	std::atomic<bool> m_PoseWaiterStarted{ false };
	std::atomic<bool> m_PoseWaiterEnabled{ false };
	std::atomic<uint32_t> m_PoseWaiterSeq{ 0 };
	std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> m_PoseWaiterPoses{};

	HANDLE m_PoseWaiterEvent = NULL;
	// In queued (mat_queue_mode!=0) rendering, this is the explicit minimum wait budget for a fresher pose
	// snapshot on the render thread. 0 disables fixed waiting, but the render hook may still do a small
	// adaptive wait during real HMD motion to avoid reusing the same pose sample. -1 = strong sync
	// (wait up to ~50ms).
	int m_QueuedRenderPoseWaitMs = 0;
	// Percentage of repeated-pose render frames allowed to bypass the strict fresh-pose wait and submit.
	// 0 = fully strict; 100 = submit every repeated pose.
	int m_QueuedRenderPoseRelaxPercent = 20;
	// Experimental: in queued full-sync mode, render from a non-blocking tracking prediction instead
	// of waiting for a fresh WaitGetPoses snapshot.
	bool m_QueuedRenderPoseFromTracking = false;
	std::atomic<uint32_t> m_QueuedRenderPoseFromTrackingSeq{ 0 };
	std::atomic<int> m_CachedTrackingUniverseOrigin{ static_cast<int>(vr::TrackingUniverseStanding) };
	std::atomic<int> m_CachedTrackingPredictionUsec{ 0 };
	inline vr::ETrackingUniverseOrigin GetCachedTrackingUniverseOrigin() const
	{
		const int origin = m_CachedTrackingUniverseOrigin.load(std::memory_order_acquire);
		if (origin < static_cast<int>(vr::TrackingUniverseSeated) ||
			origin > static_cast<int>(vr::TrackingUniverseRawAndUncalibrated))
			return vr::TrackingUniverseStanding;
		return static_cast<vr::ETrackingUniverseOrigin>(origin);
	}
	inline void CacheQueuedTrackingPredictionSeconds(float seconds)
	{
		if (!(seconds >= 0.0f && seconds <= 0.5f))
			seconds = 0.0f;
		const int usec = static_cast<int>(seconds * 1000000.0f + 0.5f);
		m_CachedTrackingPredictionUsec.store(usec, std::memory_order_release);
	}
	inline float GetQueuedTrackingPredictionSeconds() const
	{
		const int usec = m_CachedTrackingPredictionUsec.load(std::memory_order_acquire);
		if (usec <= 0)
			return 0.0f;
		return std::clamp(static_cast<float>(usec) / 1000000.0f, 0.0f, 0.030f);
	}
	inline float SampleQueuedTrackingPredictionSeconds()
	{
		// Sample the compositor at the render-pose acquisition point. The old cache
		// setter had no caller, which silently reduced queued tracking prediction to
		// zero seconds and made the pose increasingly stale as render latency grew.
		if (m_Compositor)
		{
			const float runtimeSeconds = m_Compositor->GetFrameTimeRemaining();
			if (std::isfinite(runtimeSeconds) && runtimeSeconds >= 0.0f && runtimeSeconds <= 0.5f)
				CacheQueuedTrackingPredictionSeconds(runtimeSeconds);
		}
		return GetQueuedTrackingPredictionSeconds();
	}

	// Queued rendering: optional render-thread FPS cap, expressed as a percentage of the HMD refresh rate.
	// 0 = unlimited, 100 = match HMD refresh, 80 = 80% of HMD refresh, etc.
	int m_QueuedRenderMaxFps = 0;

	// Cached HMD refresh rate (Hz) used for FPS caps, etc. Updated on demand (thread-safe).
	std::atomic<float> m_HmdDisplayFrequencyHz{ 0.0f };
	std::atomic<uint32_t> m_HmdDisplayFrequencyHzLastUpdateMs{ 0 };

	inline float GetHmdDisplayFrequencyHz(bool forceRefresh = false)
	{
		float hz = m_HmdDisplayFrequencyHz.load(std::memory_order_relaxed);
		const uint32_t nowMs = static_cast<uint32_t>(::GetTickCount());
		const uint32_t lastMs = m_HmdDisplayFrequencyHzLastUpdateMs.load(std::memory_order_relaxed);

		const bool stale = (hz <= 1.0f) || (lastMs == 0) || ((nowMs - lastMs) > 2000u);
		if ((forceRefresh || stale) && m_System)
		{
			vr::ETrackedPropertyError err = vr::TrackedProp_Success;
			const float v = m_System->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &err);
			if (err == vr::TrackedProp_Success && std::isfinite(v) && v > 1.0f && v < 1000.0f)
			{
				hz = v;
				m_HmdDisplayFrequencyHz.store(hz, std::memory_order_relaxed);
			}
			// Always mark an update attempt so we don't spam the runtime if it fails.
			m_HmdDisplayFrequencyHzLastUpdateMs.store(nowMs, std::memory_order_relaxed);
		}
		return hz;
	}

	inline int GetQueuedRenderMaxFpsEffective(bool forceRefreshHz = false)
	{
		const int pct = std::max(0, m_QueuedRenderMaxFps);
		if (pct <= 0)
			return 0;

		float hz = GetHmdDisplayFrequencyHz(forceRefreshHz);
		if (!(hz > 1.0f))
			hz = 90.0f; // safe fallback

		const double cap = (double)hz * ((double)pct / 100.0);
		int capI = (int)std::lround(cap);
		capI = std::clamp(capI, 1, 360);
		return capI;
	}


	// Queued rendering: when true, the Max FPS cap is only applied when instability is detected
	// (stale pose reuse during body motion or HMD translation/rotation). This avoids needlessly
	// capping FPS in already-stable scenes. When false, the cap is always enforced when
	// QueuedRenderMaxFps>0.
	bool m_QueuedRenderMaxFpsSmart = true;
	// Queued rendering: limit how many extra render frames may reuse the same WaitGetPoses() snapshot.
	// -1 = disabled, 0 = never reuse (most stable), 1 = allow 1 reuse (2 frames per pose), etc.
	int m_QueuedRenderMaxFramesAhead = -1;

	// Queued rendering: render-thread smoothing time constant (ms) for cameraAnchor/rotationOffset.
	// 0 = off (follow snapshot exactly), 20~80 = typical. Higher = smoother but more latency.
	int m_QueuedRenderViewSmoothMs = 25;
	// First-person stair/step smoothing: Source's step-smoothed setup.origin.z is flat-screen camera
	// motion. Low-pass it before it becomes the VR world anchor so stairs do not yank the whole view.
	int m_StairStepCameraSmoothMs = 90;

	// Queued rendering: HMD pose smoothing time constant (ms) for visual stability.
	// 0 = off. Higher values can soften visible stepping from stale pose reuse, but they do not fetch
	// fresher poses and therefore cannot fully remove queued-render ghosting during real head motion.
	// Higher = smoother but more latency between head movement and world.
	int m_QueuedRenderHmdSmoothMs = 0;
	// Queued rendering: optionally route HMD yaw deltas through m_RotationOffset as if they were
	// stick turns. Final view direction stays the same, but multicore render smoothing/snapshots
	// now see head-yaw turns on the same path as thumbstick turning, which can reduce ghosting.
	bool m_QueuedRenderHmdYawUsesTurnPath = false;

	// Queued (mat_queue_mode!=0) viewmodel stabilization: prevents first-person viewmodel ghosting
	// when engine viewmodel bob/lag runs on a decoupled thread. 
	bool m_QueuedViewmodelStabilize = true;
	// Global viewmodel stabilization: hard-lock first-person viewmodel pose after engine calc
	// in all queue modes (mat_queue_mode 0/1/2), useful to disable movement bob/sway.
	bool m_ViewmodelDisableMoveBob = false;
	// Debug logging for queued viewmodel stabilization (prints viewmodel pose + engine-produced pose).
	bool  m_QueuedViewmodelStabilizeDebugLog = false;
	float m_QueuedViewmodelStabilizeDebugLogHz = 6.0f; // max prints per second; 0 disables throttling
	// Render/HUD/multicore pipeline diagnostics. Default off; logs key frame boundaries only.
	bool  m_RenderPipelineDebugLog = false;
	float m_RenderPipelineDebugLogHz = 2.0f;
	// Source material queue marker diagnostics for queued/multicore frame completion pacing.
	bool  m_QueuedSourceMarkerDebugLog = false;
	float m_QueuedSourceMarkerDebugLogHz = 4.0f;
	// Performance mode: skip the real right-eye Source RenderView in single-threaded rendering
	// and copy the completed left-eye render target into the right-eye render target.
	// This improves CPU time but removes true stereo depth; default off.
	bool m_RightEyeCopyFromLeft = false;
	std::chrono::steady_clock::time_point m_RenderPipelineLastSubmitLog{};
	// Bullet FX alignment: optional visual-only offset applied to
		// client-side bullet tracers/impact effects so they can be tuned to match the aim line.
		// Units: meters in aim-ray space (X=forward, Y=right, Z=up). Applies in all render modes.
	Vector m_BulletVisualHitOffset = { 0.0f, 0.0f, 0.0f };
	// Additional offset only when queued rendering is enabled (mat_queue_mode!=0).
	// Lets you apply extra correction for render-thread decoupling without affecting single-thread.
	Vector m_QueuedBulletVisualHitOffset = { 0.0f, 0.0f, 0.02f };
	// Prefer the visible viewmodel's muzzlesmoke bone/empty for local client bullet/tracer FX.
	bool m_BulletVisualsUseMuzzleSmoke = false;
	// Client-side bullet/tracer FX can be emitted from the viewmodel pose when
	// the visible gun is projected in Source's viewmodel layer. Visual-only.
	bool m_BulletVisualsUseViewmodelPose = false;

	Vector m_ViewmodelPosAdjust = { 0,0,0 };
	QAngle m_ViewmodelAngAdjust = { 0,0,0 };
	ViewmodelAdjustment m_DefaultViewmodelAdjust{ {0,0,0}, {0,0,0} };
	std::unordered_map<std::string, ViewmodelAdjustment> m_ViewmodelAdjustments{};
	std::string m_CurrentViewmodelKey;
	std::string m_LastLoggedViewmodelKey;
	bool m_ViewmodelAdjustmentsDirty = false;
	std::string m_ViewmodelAdjustmentSavePath;
	bool m_ViewmodelAdjustEnabled = false;
	float m_ViewmodelAdjustMoveSpeed = 1.0f;
	float m_ViewmodelAdjustRotateSpeed = 1.0f;
	// Resolve the visible viewmodel grip from a model-local hand/attachment anchor instead
	// of assuming every replacement model shares the stock model origin.
	bool m_ViewmodelAutoGripAlignEnabled = true;
	bool m_ViewmodelAutoGripAlignRotation = false;
	bool m_ViewmodelAutoGripAlignDebugLog = false;
	Vector m_ViewmodelAutoGripTargetOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_ViewmodelAutoGripTargetRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	float m_ViewmodelAutoGripPalmCenterFraction = 0.45f;
	// AutoGrip discovers models on the render thread, while per-weapon manual adjustments
	// are owned by the main update thread. Pair the model cache key with the active
	// adjustment-key hash so a cache miss can clear exactly that weapon before sampling.
	std::atomic<uint32_t> m_ViewmodelAutoGripCurrentAdjustKeyHash{ 0u };
	std::atomic<uint64_t> m_ViewmodelAutoGripManualClearRequest{ 0u };
	std::atomic<uint64_t> m_ViewmodelAutoGripManualClearCompleted{ 0u };
	// Exact controller aim ray after applying the same rigid draw delta as the
	// local CBaseViewModel. This avoids relying on inconsistent MOD muzzle bones.
	std::atomic<uint32_t> m_ViewmodelAutoGripAimPoseSeq{ 0u };
	std::atomic<uint32_t> m_ViewmodelAutoGripAimPoseTickMs{ 0u };
	std::atomic<uint32_t> m_ViewmodelAutoGripAimPoseAdjustKeyHash{ 0u };
	std::atomic<float> m_ViewmodelAutoGripAimPosX{ 0.0f };
	std::atomic<float> m_ViewmodelAutoGripAimPosY{ 0.0f };
	std::atomic<float> m_ViewmodelAutoGripAimPosZ{ 0.0f };
	std::atomic<float> m_ViewmodelAutoGripAimDirX{ 0.0f };
	std::atomic<float> m_ViewmodelAutoGripAimDirY{ 0.0f };
	std::atomic<float> m_ViewmodelAutoGripAimDirZ{ 0.0f };

	bool m_AdjustingViewmodel = false;
	std::string m_AdjustingKey;
	Vector m_AdjustStartLeftPos = { 0,0,0 };
	QAngle m_AdjustStartLeftAng = { 0,0,0 };
	Vector m_AdjustStartViewmodelPos = { 0,0,0 };
	QAngle m_AdjustStartViewmodelAng = { 0,0,0 };
	QAngle m_AdjustStickViewmodelAng = { 0,0,0 };
	Vector m_AdjustStartViewmodelForward = { 0,0,0 };
	Vector m_AdjustStartViewmodelRight = { 0,0,0 };
	Vector m_AdjustStartViewmodelUp = { 0,0,0 };
	std::chrono::steady_clock::time_point m_AdjustSuppressControllerUntil{};
	bool m_AdjustControllerSuppressed = false;

	ScopeAdjustment m_DefaultScopeAdjust{};
	std::unordered_map<std::string, ScopeAdjustment> m_ScopeAdjustments{};
	std::string m_CurrentScopeAdjustmentKey;
	std::string m_LastLoggedScopeAdjustmentKey;
	bool m_ScopeAdjustmentsDirty = false;
	std::string m_ScopeAdjustmentSavePath;
	float m_ScopeSizeMin = 0.03f;
	float m_ScopeSizeMax = 0.30f;
	float m_ScopeSizeAdjustSpeed = 0.12f;      // meters per second at full stick deflection
	float m_ScopeOffsetAdjustMoveSpeed = 1.0f;
	bool m_AdjustingScope = false;
	std::string m_AdjustingScopeKey;
	Vector m_AdjustStartScopeLeftPos = { 0,0,0 };
	Vector m_AdjustStartScopeOverlayOffset = { 0.02f, 0.04f, -0.06f };

	Vector m_AimLineStart = { 0,0,0 };
	Vector m_AimLineEnd = { 0,0,0 };

	// Third-person convergence: point hit by the *rendered* aim ray (camera/reticle ray).
	// Bullets may be steered to aim at this point so the rendered line and bullet direction
	// intersect at P. If something blocks the bullet path, it will hit earlier  we do NOT
	// move P to the blocking surface.
	Vector m_AimConvergePoint = { 0,0,0 };
	bool m_HasAimConvergePoint = false;

	Vector m_LastAimDirection = { 0,0,0 };
	Vector m_LastUnforcedAimDirection = { 0,0,0 };
	bool m_HasAimLine = false;
	float m_AimLineThickness = 0.15f;
	bool m_AimLineEnabled = true;
	bool m_AimLineConfigEnabled = true;
	bool m_AimLineOnlyWhenLaserSight = false;
	bool m_ScopeForcingAimLine = false;
	bool m_MeleeAimLineEnabled = false;
	bool m_D3DAimLineOverlayEnabled = false;
	bool m_D3DAimLineOverlaySyncAimLineColor = true;
	float m_D3DAimLineOverlayWidthPixels = 2.0f;
	float m_D3DAimLineOverlayOutlinePixels = 1.0f;
	float m_D3DAimLineOverlayEndpointPixels = 1.5f;
	int m_D3DAimLineOverlayColorR = 255;
	int m_D3DAimLineOverlayColorG = 0;
	int m_D3DAimLineOverlayColorB = 0;
	int m_D3DAimLineOverlayColorA = 100;
	int m_D3DAimLineOverlayOutlineColorR = 255;
	int m_D3DAimLineOverlayOutlineColorG = 0;
	int m_D3DAimLineOverlayOutlineColorB = 0;
	int m_D3DAimLineOverlayOutlineColorA = 1;
	mutable std::mutex m_D3DAimLineOverlayMutex;
	std::array<D3DAimLineOverlayEyeState, 2> m_D3DAimLineOverlayEyes{};
	std::array<IDirect3DSurface9*, 2> m_D3DAimLineOverlayBackupSurfaces{};
	std::array<bool, 2> m_D3DAimLineOverlayBackupValid{};
	Vector m_D3DAimLineWorldStart = { 0,0,0 };
	Vector m_D3DAimLineWorldEnd = { 0,0,0 };
	bool m_HasD3DAimLineWorldSegment = false;
	// Mounted gun (minigun/.50cal) state.
	// We force first-person rendering while using the mounted gun (see hooks.cpp).
	// On exit we do a one-shot ResetPosition to avoid accumulated anchor drift.
	bool m_UsingMountedGunPrev = false;
	bool m_ResetPositionAfterMountedGunExitPending = false;
	// When the local player is pinned / controlled by special infected (smoked, pounced, jockey,
	// charger carry/pummel), the body animation can cause the aim line to jitter wildly.
	// Disable the aim line in those states.
	bool m_PlayerControlledBySI = false;
	float m_AimLinePersistence = 0.02f;
	float m_AimLineFrameDurationMultiplier = 0.0f;
	int m_AimLineColorR = 255;
	int m_AimLineColorG = 0;
	int m_AimLineColorB = 0;
	int m_AimLineColorA = 100;
	struct EffectiveAttackRangeWeaponData
	{
		bool valid = false;
		float minDuckingSpread = 0.0f;
		float minStandingSpread = 0.0f;
		float minInAirSpread = 0.0f;
		float maxMovementSpread = 0.0f;
		float pelletScatterPitch = 0.0f;
		float pelletScatterYaw = 0.0f;
		float range = 8192.0f;
		float maxPlayerSpeed = 250.0f;
		int bullets = 1;
		std::string source;
	};
	bool m_EffectiveAttackRangeIndicatorEnabled = true;
	bool m_EffectiveAttackRangeAutoFireEnabled = false;
	bool m_EffectiveAttackRangeAutoFireActive = false;
	bool m_EffectiveAttackRangeAutoFirePrevAttackDown = false;
	bool m_AimLineEffectiveAttackRangeActive = false;
	std::chrono::steady_clock::time_point m_AimLineEffectiveAttackRangeHoldUntil{};
	bool m_AimLineEffectiveAttackRangeCacheValid = false;
	bool m_AimLineEffectiveAttackRangeCacheResult = false;
	std::uintptr_t m_AimLineEffectiveAttackRangeTarget = 0;
	bool m_AimLineEffectiveAttackRangeTargetIsWitch = false;
	std::uintptr_t m_AimLineEffectiveAttackRangeCacheEntity = 0;
	std::uintptr_t m_AimLineEffectiveAttackRangeCacheWeapon = 0;
	std::chrono::steady_clock::time_point m_AimLineEffectiveAttackRangeCacheTime{};
	float m_AimLineEffectiveAttackRangeCacheDistance = 0.0f;
	float m_AimLineEffectiveAttackRangeCacheSpread = -1.0f;
	Vector m_AimLineEffectiveAttackRangeCacheHitPos = { 0.0f, 0.0f, 0.0f };
	Vector m_AimLineEffectiveAttackRangeCacheDirection = { 0.0f, 0.0f, 0.0f };
	bool m_AimLineEffectiveAttackRangeCacheRelaxedMovement = false;
	int m_EffectiveAttackRangeColorR = 0;
	int m_EffectiveAttackRangeColorG = 255;
	int m_EffectiveAttackRangeColorB = 0;
	bool m_EffectiveAttackRangeDebugLog = false;
	float m_EffectiveAttackRangeDebugLogHz = 2.0f;
	float m_EffectiveAttackRangeHoldSeconds = 0.25f;
	float m_EffectiveAttackRangeCacheSeconds = 0.15f;
	float m_EffectiveAttackRangeCacheDistanceTolerance = 24.0f;
	float m_EffectiveAttackRangeCacheSpreadTolerance = 0.10f;
	float m_EffectiveAttackRangeCacheDirectionDot = 0.9990f;
	float m_EffectiveAttackRangeMeleeDistance = 70.0f;
	float m_EffectiveAttackRangeMeleeFanAngle = 90.0f;
	float m_EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds = 0.60f;
	float m_EffectiveAttackRangeHitPointTolerance = 8.0f;
	float m_EffectiveAttackRangeHitPointSpreadScale = 0.50f;
	float m_EffectiveAttackRangeHitPointMaxTolerance = 24.0f;
	std::uintptr_t m_EffectiveAttackRangeDebugLastEntity = 0;
	std::chrono::steady_clock::time_point m_EffectiveAttackRangeDebugLastLog{};
	bool m_EffectiveAttackRangeWeaponDataLoaded = false;
	std::chrono::steady_clock::time_point m_EffectiveAttackRangeWeaponDataLastLoad{};
	std::array<EffectiveAttackRangeWeaponData, 64> m_EffectiveAttackRangeWeaponData{};
	bool m_GameLaserSightBeamEnabled = true;
	bool m_GameLaserSightReplaceParticle = true;
	float m_GameLaserSightThickness = 0.55f;
	int m_GameLaserSightColorR = 255;
	int m_GameLaserSightColorG = 0;
	int m_GameLaserSightColorB = 0;
	int m_GameLaserSightColorA = 220;
	Vector m_GameLaserSightEndOffset = { 0.0f, 0.0f, 0.0f };
	static constexpr int THROW_ARC_SEGMENTS = 16;
	std::array<Vector, THROW_ARC_SEGMENTS + 1> m_LastThrowArcPoints{};
	bool m_HasThrowArc = false;
	bool m_LastAimWasThrowable = false;
	bool m_ManualThrowEnabled = false;
	std::atomic<bool> m_ManualInventoryEmptyHandsActive{ false };
	// CreateMove publishes whether the local player has a throwable equipped and
	// whether its final IN_ATTACK state is held. DrawModelExecute consumes this
	// atomically so queued rendering can freeze only animation-local viewmodel
	// motion while continuing to follow the live controller anchor.
	std::atomic<uint32_t> m_ManualThrowViewmodelInputState{ 0 };
	float m_ManualThrowVelocityScale = 4.0f;
	float m_ManualThrowHorizontalVelocityScale = 1.0f;
	float m_ManualThrowVerticalVelocityScale = 1.0f;
	float m_ManualThrowArcLiftRatio = 0.0f;
	int m_ManualThrowVelocityWindowTicks = 5;
	float m_ManualThrowPeakVelocityBlend = 0.5f;
	float m_ManualThrowMaxVelocity = 1400.0f;
	float m_ThrowArcBaseDistance = 500.0f;
	float m_ThrowArcMinDistance = 20.0f;
	float m_ThrowArcMaxDistance = 2200.0f;
	float m_ThrowArcHeightRatio = 0.25f;
	float m_ThrowArcPitchScale = 6.0f;
	float m_ThrowArcLandingOffset = -90.0f;
	// Tracks the duration of the previous frame so the aim line can persist when the framerate dips.
	float m_LastFrameDuration = 1.0f / 90.0f;

	// --- Spike control / throttling ---
	// Heavy work can happen many times per frame (notably from dDrawModelExecute).
	// These knobs cap how often we do expensive debug-overlay primitives and trace tests.
	float m_AimLineMaxHz = 100.0f;              // runtime cap follows HMD refresh rate
	float m_ThrowArcMaxHz = 100.0f;             // runtime cap follows HMD refresh rate
	float m_SpecialInfectedOverlayMaxHz = 20.0f; // caps arrow drawing + prewarning refresh per entity
	float m_SpecialInfectedTraceMaxHz = 15.0f;   // caps TraceRay per entity

	std::chrono::steady_clock::time_point m_LastAimLineDrawTime{};
	std::chrono::steady_clock::time_point m_LastThrowArcDrawTime{};
	std::chrono::steady_clock::time_point m_LastPostMirrorAimLineDrawTime{};
	std::chrono::steady_clock::time_point m_LastPostMirrorThrowArcDrawTime{};
	std::array<std::chrono::steady_clock::time_point, 2> m_LastD3DAimLineOverlayUpdateTime{};
	mutable std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastSpecialInfectedOverlayTime{};
	mutable std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastSpecialInfectedTraceTime{};
	mutable std::unordered_map<int, bool> m_LastSpecialInfectedTraceResult{};

	float m_Ipd;
	float m_EyeZ;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_LeftEyeSubmit,
		Texture_RightEyeSubmit,
		Texture_HUD,
		Texture_Scope,
		Texture_RearMirror,
		Texture_DesktopMirror,
		Texture_Blank
	};

	ITexture* m_LeftEyeTexture = nullptr;
	ITexture* m_RightEyeTexture = nullptr;
	ITexture* m_LeftEyeSubmitTexture = nullptr;
	ITexture* m_RightEyeSubmitTexture = nullptr;
	ITexture* m_HUDTexture = nullptr;
	ITexture* m_ScopeTexture = nullptr;
	ITexture* m_RearMirrorTexture = nullptr;
	ITexture* m_DesktopMirrorTexture = nullptr;
	ITexture* m_BlankTexture = nullptr;

	IDirect3DSurface9* m_D9LeftEyeSurface = nullptr;
	IDirect3DSurface9* m_D9RightEyeSurface = nullptr;
	IDirect3DSurface9* m_D9LeftEyeSubmitSurface = nullptr;
	IDirect3DSurface9* m_D9RightEyeSubmitSurface = nullptr;
	IDirect3DSurface9* m_D9HUDSurface = nullptr;
	IDirect3DSurface9* m_D9ScopeSurface = nullptr;
	// GPU-side scope lens post-process surfaces. The raw scope RTT is copied and masked into
	// a separate processed overlay texture so SteamVR never samples the square RTT directly.
	IDirect3DTexture9* m_D9ScopeLensScratchTexture = nullptr; // GPU render-target copy of the raw scope RTT
	IDirect3DSurface9* m_D9ScopeLensScratchSurface = nullptr;
	IDirect3DTexture9* m_D9ScopeLensTexture = nullptr;        // processed circular scope overlay texture
	IDirect3DSurface9* m_D9ScopeLensSurface = nullptr;        // processed circular scope overlay surface
	uint32_t m_D9ScopeLensScratchW = 0;
	uint32_t m_D9ScopeLensScratchH = 0;
	uint32_t m_D9ScopeLensScratchFormat = 0;
	IDirect3DSurface9* m_D9RearMirrorSurface = nullptr;
	IDirect3DSurface9* m_D9DesktopMirrorSurface = nullptr;
	IDirect3DSurface9* m_D9BlankSurface = nullptr;

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer;
	SharedTextureHolder m_VKHUD;
	SharedTextureHolder m_VKScope;
	SharedTextureHolder m_VKScopeLens;
	SharedTextureHolder m_VKRearMirror;
	SharedTextureHolder m_VKBlankTexture;
	bool m_BackBufferTextureValid = false;

	// Protects VR texture lifecycle and SteamVR texture submissions when render/update threads overlap.
	mutable TextureStateMutex m_TextureMutex;

	// If enabled, scope / rear-mirror render-target textures are created only when the feature is enabled.
	// This can save large chunks of 32-bit VAS when ScopeRTTSize/RearMirrorRTTSize are high.
	bool m_LazyScopeRearMirrorRTT = true;
	// Debug: log Virtual Address Space (VAS) stats at key allocation points.
	bool m_DebugVASLog = false;

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	std::atomic<bool> m_RenderedNewFrame{ false };
	std::atomic<bool> m_RenderedHud{ false };
	std::atomic<bool> m_NativeDesktopHudPainted{ false };
	// Main menu only needs one blank stereo submit to clear the last scene frame.
	// After that, keep driving the menu with IVROverlay to avoid hammering the compositor.
	bool m_MenuBlankSubmitted = false;
	// Cold startup has no previous VR scene to clear. Avoid submitting the menu blank texture
	// until at least one real eye frame has reached the compositor.
	bool m_HasSubmittedSceneFrame = false;
	// Guard against duplicate compositor submits in the same pose frame.
	// Updated by UpdatePosesAndActions(), consumed by SubmitVRTextures().
	std::atomic<uint32_t> m_SubmitPoseToken{ 0 };
	std::atomic<uint32_t> m_LastSubmittedPoseToken{ 0 };
	std::atomic<bool> m_SubmitInFlight{ false };
	std::atomic<uint32_t> m_LastSubmittedCompositorFrameIndex{ 0 };
	// Render-thread -> submit-thread frame handoff (queued/multicore mode).
	// dRenderView increments m_RenderCompletedFrameId and signals m_RenderFrameReadyEvent
	// when a full stereo frame is rendered into eye textures.
	std::atomic<uint32_t> m_RenderCompletedFrameId{ 0 };
	std::atomic<uint32_t> m_RenderCompletedPoseToken{ 0 };
	std::atomic<uint32_t> m_RenderCompletedDuplicatePoseFrameId{ 0 };
	std::mutex m_RenderCompletedHmdPoseMutex;
	vr::HmdMatrix34_t m_RenderCompletedHmdPose{};
	bool m_RenderCompletedHmdPoseValid = false;
	std::atomic<uint32_t> m_LastSubmittedFrameId{ 0 };
	HANDLE m_RenderFrameReadyEvent = NULL;
	// Source queued-render completion marker. Each full stereo render appends a tiny
	// functor to IMatRenderContext::GetCallQueue(); the material worker publishes the
	// frame only after all preceding Source render commands have executed. Epoch drops
	// callbacks left behind by map transitions or D3D9 device resets.
	std::atomic<uint32_t> m_SourceRenderQueueMarkerEpoch{ 1 };
	std::atomic<uint32_t> m_SourceRenderQueueMarkerQueuedId{ 0 };
	std::atomic<uint32_t> m_SourceRenderQueueMarkerCompletedId{ 0 };
	// Independent top-level water/offscreen RenderView passes do not publish stereo
	// frames, but their queued D3D work must still block raw Present-side postwork.
	std::atomic<uint32_t> m_SourceRenderQueueAuxPendingCount{ 0 };
	std::atomic<bool> m_SourceRenderQueueOwnershipUncertain{ false };
	// A completed queued stereo pair is copied into dedicated submit RTs before its
	// completion marker is published. Source may then render the next frame into the
	// raw eye RTs while OpenVR consumes this stable snapshot.
	std::atomic<bool> m_QueuedEyeSubmitIsolationReady{ false };
	// The marker id alone cannot describe ownership while dRenderView is still building
	// a queued frame: the marker is appended only at the end of the command stream. Keep
	// the producer phase visible to Present so it cannot touch the single-buffered eye
	// textures in that gap.
	std::atomic<uint32_t> m_SourceRenderQueueBuildCount{ 0 };
	// Couples producer 0->1 transitions with Present's final idle recheck. Present
	// holds this only across auxiliary overlay consumption, never across pose waits.
	mutable std::recursive_mutex m_SourceRenderConsumerGate;
	inline bool IsSourceRenderQueueBusy() const
	{
		if (m_SourceRenderQueueBuildCount.load(std::memory_order_acquire) != 0)
			return true;
		if (m_SourceRenderQueueAuxPendingCount.load(std::memory_order_acquire) != 0)
			return true;
		if (m_SourceRenderQueueOwnershipUncertain.load(std::memory_order_acquire))
			return true;

		return m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire) !=
			m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
	}
	// Present-side wait budget (ms) for a fresh rendered frame in mat_queue_mode!=0.
	// 0 disables waiting. Used as an upper bound by adaptive submit-wait logic.
	int m_QueuedSubmitWaitMs = 0;
	// Queued submit policy switch:
	// true = submit only frames whose render-completed pose token advanced (less ghosting, may skip frames)
	// false = original submit-pose-token path (smoother cadence, can submit stale render-pose frames)
	bool m_QueuedSubmitUseRenderPoseToken = true;
	// Count of consecutive presents where submit thread observes no newer rendered frame.
	// Used to apply submit wait adaptively only when stale-frame pressure persists.
	std::atomic<uint32_t> m_QueuedSubmitStaleStreak{ 0 };
	// True once VGui_Paint has been redirected into m_HUDTexture for the current VR frame.
	std::atomic<bool> m_HudPaintedThisFrame{ false };
	std::atomic<bool> m_CreatedVRTextures{ false };
	// Used by extra offscreen passes (scope RTT): prevents HUD hooks from hijacking RT stack
	bool m_SuppressHudCapture = false;
	bool m_CompositorExplicitTiming = false;
	bool m_CompositorNeedsHandoff = false;
	TextureID m_CreatingTextureID = Texture_None;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;
	bool m_AutoBunnyHop = false;
	bool m_AutoAirStrafe = false;
	bool m_AutoAirStrafeLandingSpeedPreserve = true;
	float m_AutoAirStrafeMaxGainPerHop = 0.0f;
	float m_AutoAirStrafeTargetSpeed = 0.0f;
	float m_AutoAirStrafeSpeedProjection = 0.0f;
	float m_AutoAirStrafeMaxTurnBrakeProjection = 2.0f;
	float m_AutoAirStrafeTurnResponsiveness = 0.25f;
	bool m_AutoAirStrafeFlip = false;
	bool m_AutoAirStrafeDebugLog = false;
	float m_AutoAirStrafeDebugLogHz = 4.0f;
	bool m_AutoAirStrafeDebugWasAirborne = false;
	float m_AutoAirStrafeDebugAirStartSpeed = 0.0f;
	float m_AutoAirStrafeDebugAirPeakSpeed = 0.0f;
	float m_AutoAirStrafeDebugLastSpeed = 0.0f;
	int m_AutoAirStrafeDebugAirTicks = 0;
	int m_AutoAirStrafeDebugActiveTicks = 0;
	std::chrono::steady_clock::time_point m_AutoAirStrafeDebugLastLog{};
	bool m_AutoAirStrafeDebugHavePrevSpeed = false;
	int m_AutoAirStrafeDebugPrevCmd = 0;
	float m_AutoAirStrafeDebugPrevSpeed = 0.0f;
	float m_AutoAirStrafeDebugPrevVx = 0.0f;
	float m_AutoAirStrafeDebugPrevVy = 0.0f;
	float m_AutoAirStrafeDebugPrevYaw = 0.0f;
	bool m_AutoAirStrafeDebugPrevGround = false;
	bool m_LedgeGuardEnabled = false;
	float m_LedgeGuardProbeDistance = 36.0f;
	float m_LedgeGuardProbeHeight = 18.0f;
	float m_LedgeGuardDropDistance = 96.0f;
	float m_LedgeGuardMinMoveSpeed = 1.0f;
	bool m_LedgeGuardDebugLog = false;
	float m_LedgeGuardDebugLogHz = 2.0f;
	std::chrono::steady_clock::time_point m_LedgeGuardDebugLastLog{};
	// Any locomotion (keyboard WASD, thumbstick, etc.) detected in the current CreateMove.
	// Used to avoid conflicts with 1:1 roomscale movement/camera decoupling.
	bool m_LocomotionActive = false;
	bool m_CrouchToggleActive = false;
	bool m_VoiceRecordActive = false;
	bool m_VoiceRecordCmdOwned = false;
	bool m_SpeechToTextCaptureActive = false;
	bool m_QuickTurnTriggered = false;
	bool m_PrimaryAttackDown = false;
	// We drive +attack/+attack2 via ClientCmd for VR actions. In mouse-mode, spamming "-attack"
	// every frame prevents real mouse buttons from working, so we only send +/- when VR actually
	// changes state, and only release if we were the one who pressed.
	bool m_PrimaryAttackCmdOwned = false;
	bool m_SecondaryAttackCmdOwned = false;
	// True while a VR overlay click is held on an in-game VGUI panel.
	// If the panel closes before SteamVR sends MouseButtonUp, ProcessInput releases it.
	bool m_InGameVguiMouseDown = false;
	bool m_JumpCmdOwned = false;
	bool m_UseCmdOwned = false;
	bool m_ServerUseControllerAimActive = false;
	std::chrono::steady_clock::time_point m_ServerUseControllerAimUntil{};
	bool m_ReloadCmdOwned = false;
	bool m_DuckCmdOwned = false;
	bool m_LeftGripPressedPrev = false;
	bool m_RightGripPressedPrev = false;

	// When GRIP is consumed for inventory switching, suppress SteamVR digital actions from that hand
	// until GRIP is released (prevents GRIP-bound Jump etc).
	bool m_SuppressActionsUntilGripReleaseLeft = false;
	bool m_SuppressActionsUntilGripReleaseRight = false;

	struct ActionCombo
	{
		vr::VRActionHandle_t* primary = nullptr;
		vr::VRActionHandle_t* secondary = nullptr;
	};

	ActionCombo m_VoiceRecordCombo{ &m_ActionCrouch, &m_ActionReload };
	ActionCombo m_QuickTurnCombo{ &m_ActionSecondaryAttack, &m_ActionCrouch };
	ActionCombo m_ViewmodelAdjustCombo{ &m_ActionSecondaryAttack, &m_ActionUse };
	bool m_SpeechToTextEnabled = false;
	bool m_SpeechToTextSendChatEnabled = true;
	bool m_SpeechToTextSendVoiceEnabled = false;
	bool m_SpeechToTextSendVoiceLoopbackEnabled = false;
	float m_SpeechToTextMinimumRecordSeconds = 0.30f;
	std::string m_SpeechToTextCommandPrefix = "VR\\speech\\whisper-cli.exe";
	std::string m_SpeechToTextModel = "VR\\speech\\models\\ggml-base.bin";
	std::string m_SpeechToTextLanguage = "zh";
	std::string m_SpeechToTextSendVoiceCommandPrefix;
	std::string m_SpeechToTextSendVoiceModel;
	std::string m_SpeechToTextSendVoiceWorkingDir;
	std::string m_SpeechToTextSendVoiceReferenceAudio;
	std::string m_SpeechToTextSendVoicePromptText;
	std::string m_SpeechToTextSendVoicePromptLanguage;
	std::string m_SpeechToTextSendVoiceLanguage;
	std::string m_SpeechToTextSendVoiceTextSplitMethod;
	bool m_TextToSpeechEnabled = false;
	bool m_TextToSpeechSurvivorOnly = true;
	bool m_TextToSpeechIncludeSpeakerName = true;
	bool m_TextToSpeechSkipOwnMessages = true;
	float m_TextToSpeechVolume = 1.0f;
	std::string m_TextToSpeechCommandPrefix = "python api_v2.py";
	std::string m_TextToSpeechModel = "VR\\speech\\GPT-SoVITS\\GPT_SoVITS\\configs\\tts_infer.yaml";
	std::string m_TextToSpeechWorkingDir = "VR\\speech\\GPT-SoVITS";
	int m_TextToSpeechServerPort = 9880;
	std::string m_TextToSpeechReferenceAudio = "VR\\speech\\GPT-SoVITS\\reference.wav";
	std::string m_TextToSpeechPromptText;
	std::string m_TextToSpeechPromptLanguage = "zh";
	std::string m_TextToSpeechLanguage = "zh";
	std::string m_TextToSpeechTextSplitMethod = "cut5";
	std::string m_TextToSpeechWhitelistRegexes;
	std::string m_TextToSpeechWhitelistSeparator = "__VR_REGEX_SPLIT__";

	// action sets
	// /actions/main contains gameplay inputs. /actions/base contains the existing
	// SteamVR pose, haptic and skeletal bindings used by the independent hand renderer.
	vr::VRActionSetHandle_t m_ActionSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActionSetHandle_t m_BaseActionSet = vr::k_ulInvalidActionSetHandle;
	std::array<vr::VRActiveActionSet_t, 2> m_ActiveActionSets{};

	// actions
	vr::VRActionHandle_t m_ActionJump = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionPrimaryAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSecondaryAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionReload = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionWalk = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionTurn = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionBooleanTurnLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionBooleanTurnRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionUse = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionTeleport = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionNextItem = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionPrevItem = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionResetPosition = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionCrouch = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionFlashlight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionInventoryGripLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionInventoryGripRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionInventoryQuickSwitch = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSpecialInfectedAutoAimToggle = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSpecialInfectedDodgeToggle = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionLedgeGuardToggle = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionEffectiveAttackRangeAutoFireToggle = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSpeechToText = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionActivateVR = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuSelect = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuBack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuUp = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuDown = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Spray = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Scoreboard = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ToggleHUD = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Pause = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_NonVRServerMovementAngleToggle = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_CustomAction1 = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_CustomAction2 = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_CustomAction3 = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_CustomAction4 = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_CustomAction5 = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionScopeToggle = vr::k_ulInvalidActionHandle;
	std::unordered_map<vr::VRActionHandle_t, std::vector<LeftHandedMirroredActionSource>> m_LeftHandedDigitalActionSwapMap;
	std::unordered_map<vr::VRActionHandle_t, std::vector<LeftHandedMirroredActionSource>> m_LeftHandedAnalogActionSwapMap;
	std::unordered_set<vr::VRActionHandle_t> m_LeftHandedDigitalHandBoundActions;
	std::unordered_set<vr::VRActionHandle_t> m_LeftHandedAnalogHandBoundActions;
	std::unordered_map<vr::VRActionHandle_t, LeftHandedDigitalActionFrameCache> m_LeftHandedDigitalActionFrameCache;
	std::unordered_map<vr::VRActionHandle_t, LeftHandedAnalogActionFrameCache> m_LeftHandedAnalogActionFrameCache;
	std::chrono::steady_clock::time_point m_LeftHandedInputSwapNextRefresh{};
	uint64_t m_LeftHandedInputBindingHash = 0;
	uint64_t m_InputActionStateFrameSerial = 0;
	bool m_LeftHandedInputSwapRuntimeEnabled = false;
	bool m_WeaponHapticsEnabled = true;
	std::unordered_map<std::string, WeaponHapticsProfile> m_WeaponHapticsOverrides;
	WeaponHapticsProfile m_DefaultWeaponHapticsProfile = { 0.018f, 130.0f, 0.32f };
	WeaponHapticsProfile m_MeleeSwingHapticsProfile = { 0.035f, 95.0f, 0.72f };
	WeaponHapticsProfile m_ShoveHapticsProfile = { 0.022f, 120.0f, 0.58f };
	HapticMixState m_LeftHapticMix{};
	HapticMixState m_RightHapticMix{};
	float m_HapticMixMinIntervalSeconds = 0.005f;

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	float m_RotationOffset = 0;
	std::chrono::steady_clock::time_point m_PrevFrameTime;
	std::chrono::steady_clock::time_point m_LastCompositorErrorLog{};

	float m_TurnSpeed = 0.3f;
	bool m_SnapTurning = false;
	float m_SnapTurnAngle = 45.0f;
	bool m_LeftHanded = false;
	bool m_LeftHandedSwapInputActions = false;
	// If false: movement (walk axis) follows HMD yaw ("head-oriented locomotion").
	// If true:  movement follows the right-hand controller yaw ("hand-oriented locomotion").
	bool m_MoveDirectionFromController = false;
	// Mouse aiming / mouse steering mode (desktop-style).
	// When enabled:
	//  - Mouse X rotates the body (yaw) via m_RotationOffset.
	//  - Mouse Y controls aim pitch (see MouseModePitchAffectsView).
	//  - Viewmodel is anchored to an HMD-relative offset (MouseModeViewmodelAnchorOffset).
	//  - Aim line starts at the anchored viewmodel point, but converges to the mouse-aim ray
	//    at MouseModeAimConvergeDistance (scheme B).
	bool m_MouseModeEnabled = false;
	// System-level mouse suppression while playing in a loaded map.
	// This installs a Win32 low-level mouse hook and eats mouse movement/buttons/wheel
	// before Source sees them. It intentionally does not rely on CUserCmd filtering.
	bool m_SystemMouseInputSuppressAfterMapLoad = false;
	bool m_SystemMouseInputSuppressActive = false;
	// Mouse-mode aiming source.
	// If false (default): aim direction is driven by the accumulated mouse pitch + body yaw (m_RotationOffset).
	// If true:            aim direction follows the HMD center ray (view direction), while the aim line origin
	//                     remains at the mouse-mode viewmodel anchor (so we do NOT move the aim line to the HMD).
	bool m_MouseModeAimFromHmd = true;
	// Mouse-mode HMD aim sensitivity (only when MouseModeAimFromHmd is true).
	// 1.0 = 1:1 head rotation, 0 = frozen at enable, >1 amplifies head motion.
	float m_MouseModeHmdAimSensitivity = 1.0f;
	QAngle m_MouseModeHmdAimReferenceAng = { 0.0f, 0.0f, 0.0f };
	bool m_MouseModeHmdAimReferenceInitialized = false;
	// If true, mouse Y also tilts the rendered view (adds a pitch offset on top of head tracking).
	// This makes it possible to aim high/low without physically tilting your head (more like flatscreen).
	bool m_MouseModePitchAffectsView = true;
	// Additional pitch applied to the HMD view (degrees). Only used when MouseModePitchAffectsView is true.
	float m_MouseModeViewPitchOffset = 0.0f;
	// Degrees per mouse-count (tune to taste; negative inverts)
	float m_MouseModeYawSensitivity = 0.01f;
	float m_MouseModePitchSensitivity = 0.01f;
	// Mouse-mode yaw smoothing time constant (seconds).
	//
	// Implementation note (scheme A): we smooth by "draining" a remaining yaw delta.
	// - CreateMove converts cmd->mousedx to a yaw delta in degrees and accumulates it into
	//   m_MouseModeYawDeltaRemainingDeg.
	// - UpdateTracking runs at VR render rate and applies a fraction of that remaining delta per frame.
	//   This guarantees the total applied rotation equals the total mouse input (no "coasting" past the
	//   user's actual movement), while still smoothing the motion.
	// - 0 disables yaw smoothing (legacy: apply yaw directly on CreateMove ticks).
	float m_MouseModeTurnSmoothing = 0.03f;
	// Internal state for scheme A (delta drain).
	float m_MouseModeYawDeltaRemainingDeg = 0.0f;
	bool  m_MouseModeYawDeltaInitialized = false;
	// Legacy (scheme B) target-yaw smoothing fields (kept for compatibility / diff minimization).
	float m_MouseModeYawTarget = 0.0f;      // degrees in [0,360)
	bool m_MouseModeYawTargetInitialized = false;
	float m_MouseModePitchSmoothing = 0.03f; // seconds; 0 disables smoothing (pitch)
	float m_MouseModePitchTarget = 0.0f;      // degrees (aim pitch)
	bool m_MouseModePitchTargetInitialized = false;
	float m_MouseModeViewPitchTargetOffset = 0.0f; // degrees; only used when MouseModePitchAffectsView
	bool m_MouseModeViewPitchTargetOffsetInitialized = false;
	// Independent aim pitch (deg). Initialized to the current HMD pitch on enable.
	float m_MouseAimPitchOffset = 0.0f;
	bool m_MouseAimInitialized = false;
	// HMD-local offset for the viewmodel anchor (meters; scaled by VRScale).
	Vector m_MouseModeViewmodelAnchorOffset = { 0.42f, 0.0f, -0.28f };
	// Optional HMD-local anchor to use while mouse-mode scope is toggled on (meters; scaled by VRScale).
	// If you want a more "ADS"-like viewmodel position when using ScopeRTT in mouse mode, set this.
	Vector m_MouseModeScopedViewmodelAnchorOffset = { 0.35f, 0.0f, -0.13f };
	// Mouse-mode: scope overlay offset relative to the HMD in OpenVR tracking space (meters).
	// x = right, y = up, z = back (towards the player's face).
	// If non-zero, mouse mode will place the scope overlay using the HMD tracking pose
	// so it won't disappear due to mismatched game-units vs meters when using absolute overlays.
	Vector m_MouseModeScopeOverlayOffset = { 0.0f, -0.02f, -0.3f };
	QAngle m_MouseModeScopeOverlayAngleOffset = { 0.0f, 0.0f, 0.0f };
	bool   m_MouseModeScopeOverlayAngleOffsetSet = false;
	// Mouse-mode scope hotkeys (keyboard). Format in config.txt: key:X, key:F1..F12 (see parseVirtualKey).
	std::optional<WORD> m_MouseModeScopeToggleKey;
	bool m_MouseModeScopeToggleActive = false;
	bool m_MouseModeScopeToggleKeyDownPrev = false;
	// Convergence distance (Source units). Aim ray from the viewmodel anchor is steered to intersect
	// the HMD-center ray at this distance. Set <= 0 to disable convergence (use raw viewmodel ray).
	float m_MouseModeAimConvergeDistance = 2048.0f;

	float m_VRScale = 43.2f;
	float m_IpdScale = 1.0f;
	// Independent-GLB-hand render calibration only. These values never alter tracked controller poses,
	// gameplay input, viewmodels, aim lines, gestures or inventory anchors.
	// Hand-local axes: X=right, Y=up, Z=back. Position is meters; rotation is X/Y/Z degrees.
	Vector m_VrHandsLeftPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_VrHandsLeftPoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_VrHandsRightPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_VrHandsRightPoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	// Left-handed mode uses gameplay-right for the physical left/gun hand and gameplay-left
	// for the physical right/off hand. These offsets are named by physical hand.
	Vector m_VrHandsLeftHandedLeftPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_VrHandsLeftHandedTwoHandedRightPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_VrHandsLeftHandedViewmodelPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_VrHandsLeftHandedViewmodelPoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	// Built-in config overlay placement. Parsed by ParseConfigFile() so config hot-reload can update it.
	float m_ConfigOverlayDistanceMeters = 1.35f;
	float m_ConfigOverlaySizeMeters = 2.05f;
	bool m_HideArms = false;
	bool m_NativeViewmodelHandsOnly = false;
	float m_NativeViewmodelHandsOnlyWristKeepFraction = 0.0f;
	float m_NativeViewmodelHandsOnlyTrimUnits = 0.0f;
	float m_NativeViewmodelHandsOnlyArmBendScale = 1.0f;
	Vector m_NativeViewmodelHandsOnlyCutRotationDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelHandsOnlyLeftCutRotationDeg = { 0.0f, -25.0f, 0.0f };
	Vector m_NativeViewmodelHandsOnlyRightCutRotationDeg = { 8.0f, -25.0f, 0.0f };
	bool m_NativeViewmodelHandsOnlyAutoCutRotation = true;
	struct NativeViewmodelHandsOnlyCutRotationOverride
	{
		std::string modelPattern;
		Vector left = { 0.0f, 0.0f, 0.0f };
		Vector right = { 0.0f, 0.0f, 0.0f };
		bool hasLeft = false;
		bool hasRight = false;
	};
	std::string m_NativeViewmodelHandsOnlyCutRotationOverridesSpec;
	std::vector<NativeViewmodelHandsOnlyCutRotationOverride> m_NativeViewmodelHandsOnlyCutRotationOverrides;
	float m_NativeViewmodelRightHandAnimationKeepUnits = 4.0f;
	float m_NativeViewmodelLeftHandFreezeAfterMapSeconds = 0.0f;
	bool m_NativeViewmodelHandsOnlyFreezePoseLock = true;
	Vector m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters = { 0.55f, 0.18f, -0.18f };
	Vector m_NativeViewmodelHandsOnlyFreezePoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelHandsOnlyLeftFreezePoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelHandsOnlyRightFreezePoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelLeftHandPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelLeftHandPoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelRightHandPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelRightHandPoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	bool m_NativeViewmodelLeftHandOpenVRSkeleton = true;
	float m_NativeViewmodelLeftHandOpenVRCurlStrength = 1.0f;
	float m_NativeViewmodelLeftHandOpenVRCurlScale = 1.0f;
	float m_NativeViewmodelLeftHandOpenVRCurlDirection = 1.0f;
	int m_NativeViewmodelLeftHandOpenVRCurlAxis = 2;
	std::array<float, 5> m_NativeViewmodelLeftHandOpenVRInitialCurl = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelLeftHandOpenVRThumbRootOffsetUnits = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelLeftHandOpenVRThumbRootRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelRightHandOpenVRThumbRootOffsetUnits = { 0.0f, 0.0f, 0.0f };
	Vector m_NativeViewmodelRightHandOpenVRThumbRootRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	vr::VRActionHandle_t m_NativeViewmodelLeftHandOpenVRAction = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_NativeViewmodelRightHandOpenVRAction = vr::k_ulInvalidActionHandle;
	mutable std::mutex m_NativeViewmodelLeftHandOpenVRFingerCurlMutex;
	std::array<float, 5> m_NativeViewmodelLeftHandOpenVRFingerCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	std::chrono::steady_clock::time_point m_NativeViewmodelLeftHandOpenVRFingerCurlsAt{};
	bool m_NativeViewmodelLeftHandOpenVRFingerCurlsValid = false;
	mutable std::mutex m_NativeViewmodelRightHandOpenVRFingerCurlMutex;
	std::array<float, 5> m_NativeViewmodelRightHandOpenVRFingerCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	std::chrono::steady_clock::time_point m_NativeViewmodelRightHandOpenVRFingerCurlsAt{};
	bool m_NativeViewmodelRightHandOpenVRFingerCurlsValid = false;
	bool m_NativeViewmodelLeftHandFreezeHadLocalPlayerPrev = false;
	bool m_NativeViewmodelLeftHandFreezePending = false;
	std::chrono::steady_clock::time_point m_NativeViewmodelLeftHandFreezeDueTime{};
	std::atomic<uint32_t> m_NativeViewmodelLeftHandFreezeReady{ 0 };
	std::atomic<uint32_t> m_NativeViewmodelLeftHandFreezeGeneration{ 1 };
	bool m_NativeViewmodelLeftHandFreezeHasSurvivorCharacter = false;
	int m_NativeViewmodelLeftHandFreezeSurvivorCharacter = -1;
	bool m_NativeViewmodelHandsOnlyFreezePlaneContextActive = false;
	std::atomic<uint32_t> m_NativeViewmodelHandsOnlyFreezePlaneGeneration{ 1 };
	// Independent GLB + ozz-animation VR hand renderer. In queued rendering, raw D3D9 hand
	// draws are inserted into Source's material call queue so they run at the correct eye-RT point.
	bool m_VrHandsEnabled = false;
	bool m_VrHandsGlovesEnabled = false;
	std::array<float, 5> m_VrHandsGloveFingerMaxCurl = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	bool m_VrHandsGlovesRuntimeFallback = false;
	bool m_VrHandsGlovesFallbackLogged = false;
	bool m_VrHandsMotionRangeWithoutController = false;
	bool m_VrHandsRightUseViewmodelPose = false;
	float m_VrHandsTwoHandedGripTargetBoxScale = 1.0f;
	bool m_VrHandsTwoHandedGripHeldMode = false;
	bool m_VrHandsRealBulletSpreadEnabled = false;
	bool m_VrHandsTwoHandedAimMountFriendly = false;
	bool m_VrHandsVirtualStockEnabled = false;
	bool m_VrHandsVirtualStockHeldPistol = false;
	float m_VrHandsTwoHandedAimStrength = 1.0f;
	float m_VrHandsTwoHandedAimSmoothingSeconds = 0.025f;
	float m_VrHandsTwoHandedAimMinHandDistanceMeters = 0.12f;
	float m_VrHandsTwoHandedAimMaxHandDistanceMeters = 0.85f;
	Vector m_VrHandsTwoHandedAimOffhandOffsetMeters = { 0.12f, 0.0f, 0.0f };
	Vector m_VrHandsVirtualStockOffsetMeters = { -0.28f, 0.16f, -0.12f };
	float m_VrHandsVirtualStockStrength = 0.65f;
	bool m_VrHandsTwoHandedAimSmoothingValid = false;
	Vector m_VrHandsTwoHandedAimForwardSmoothed = { 1.0f, 0.0f, 0.0f };
	Vector m_VrHandsTwoHandedAimUpSmoothed = { 0.0f, 0.0f, 1.0f };
	bool m_VrHandsTwoHandedGripActive = false;
	int m_VrHandsTwoHandedGripWeaponId = 0;
	std::chrono::steady_clock::time_point m_VrHandsTwoHandedMountFriendlyGripEnteredAt{};
	bool m_VrHandsTwoHandedMountFriendlyGripContact = false;
	bool IsVrHandsTwoHandedGripPoseActive() const
	{
		return m_VrHandsTwoHandedGripActive;
	}
	Vector m_VrHandsRealBulletSpreadAimDirection = { 0.0f, 0.0f, 0.0f };
	std::chrono::steady_clock::time_point m_VrHandsRealBulletSpreadAimHoldUntil{};
	std::chrono::steady_clock::time_point m_VrHandsRealBulletSpreadLastShotAt{};
	int m_VrHandsRealBulletSpreadLastWeaponId = 0;
	int m_VrHandsRealBulletSpreadBurstShotCount = 0;
	uint32_t m_VrHandsRealBulletSpreadBurstSeed = 0;
	bool m_VrHandsDebugLog = false;
	float m_VrHandsModelScale = 1.0f;
	std::unique_ptr<VrHandSystem> m_VrHands;
	const CViewSetup* m_VrHandsActiveEyeView = nullptr;
	int m_VrHandsActiveEyeIndex = -1;
	bool m_VrHandsWorldMaskDrawn = false;

	// Debug overlay: identify the current weapon viewmodel's magazine bone and draw a solid box over it.
	bool m_MagazineBoxDebugEnabled = false;
	bool m_ViewmodelBoneLabelsEnabled = false;
	Vector m_MagazineBoxDebugFallbackHalfExtentsMeters = { 0.025f, 0.095f, 0.018f };
	Vector m_MagazineBoxDebugPaddingMeters = { 0.002f, 0.002f, 0.002f };
	// Independent magazine interaction prototype. It consumes the current weapon magazine OBB and
	// lets configured off-hand grip input claim physical reload interactions before normal input.
	bool m_MagazineInteractionEnabled = false;
	bool m_MagazineInteractionQuickReloadMode = false;
	bool m_MagazineInteractionUseButtonGripInput = true;
	bool m_MagazineInteractionUseButtonDisbleReloadCommand = false;
	bool m_MagazineInteractionSeparateButtonInput = false;
	bool m_MagazineInteractionSuppressEmptyClipAutoReload = false;
	int m_MagazineInteractionShotgunShellsPerInsert = 1;
	float m_MagazineInteractionThumbIndexCurlStart = 0.62f;
	float m_MagazineInteractionThumbIndexCurlRelease = 0.42f;
	float m_MagazineInteractionThreeFingerCurlStart = 0.62f;
	float m_MagazineInteractionThreeFingerCurlRelease = 0.42f;
	float m_MagazineInteractionGrabPaddingMeters = 0.12f;
	float m_MagazineInteractionPullTriggerMeters = 0.08f;
	float m_MagazineInteractionPullTriggerByMagazineMeters = 0.025f;
	float m_MagazineInteractionFreshMagazineGrabRangeMeters = 0.18f;
	Vector m_MagazineInteractionFreshMagazinePickupOffsetMeters = { 0.45f, -0.28f, 0.25f };
	Vector m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters = { 0.055f, 0.045f, 0.12f };
	Vector m_MagazineInteractionFreshMagazineSocketLocalOffsetMeters = { -0.12f, 0.0f, 0.0f };
	Vector m_MagazineInteractionFreshMagazineWristAnchorOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_MagazineInteractionMagazineBoxHalfExtentsMeters = { 0.0f, 0.0f, 0.0f };
	std::string m_MagazineInteractionMagazineBoxHalfExtentsMetersOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionMagazineBoxHalfExtentsMetersOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionMagazineBoxHalfExtentsMetersProfileOverrides;
	Vector m_MagazineInteractionMagazineBoxLocalOffsetMeters = { 0.0f, 0.0f, 0.0f };
	std::string m_MagazineInteractionMagazineBoxLocalOffsetMetersOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionMagazineBoxLocalOffsetMetersOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionMagazineBoxLocalOffsetMetersProfileOverrides;
	Vector m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	std::string m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionMagazineBoxLocalRotationOffsetDegProfileOverrides;
	float m_MagazineInteractionSocketCaptureRadiusMeters = 0.06f;
	float m_MagazineInteractionSocketCaptureAngleDeg = 35.0f;
	float m_MagazineInteractionSocketRequiredDepthMeters = 0.04f;
	std::string m_MagazineInteractionSocketRequiredDepthMetersOverridesSpec;
	std::unordered_map<int, float> m_MagazineInteractionSocketRequiredDepthMetersOverrides;
	std::unordered_map<std::string, float> m_MagazineInteractionSocketRequiredDepthMetersProfileOverrides;
	float m_MagazineInteractionSocketRequiredOverlapFraction = 0.45f;
	std::string m_MagazineInteractionSocketRequiredOverlapFractionOverridesSpec;
	std::unordered_map<int, float> m_MagazineInteractionSocketRequiredOverlapFractionOverrides;
	std::unordered_map<std::string, float> m_MagazineInteractionSocketRequiredOverlapFractionProfileOverrides;
	Vector m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	// Optional exact per-weapon socket capture tuning. Config format follows the weapon aliases used by BoneOverrides.
	// Vector overrides use semicolon-separated entries because values contain commas:
	// m16:0.06,0.04,0.12;ak47:0.05,0.04,0.10.
	std::string m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersProfileOverrides;
	std::string m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersProfileOverrides;
	std::string m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegProfileOverrides;
	std::string m_MagazineInteractionSocketCaptureAngleDegOverridesSpec;
	std::unordered_map<int, float> m_MagazineInteractionSocketCaptureAngleDegOverrides;
	std::unordered_map<std::string, float> m_MagazineInteractionSocketCaptureAngleDegProfileOverrides;
	float m_MagazineInteractionBoltGrabPaddingMeters = 0.10f;
	std::string m_MagazineInteractionBoltGrabPaddingMetersOverridesSpec;
	std::unordered_map<int, float> m_MagazineInteractionBoltGrabPaddingMetersOverrides;
	std::unordered_map<std::string, float> m_MagazineInteractionBoltGrabPaddingMetersProfileOverrides;
	float m_MagazineInteractionBoltPullDistanceMeters = 0.055f;
	std::string m_MagazineInteractionBoltPullDistanceMetersOverridesSpec;
	std::unordered_map<int, float> m_MagazineInteractionBoltPullDistanceMetersOverrides;
	std::unordered_map<std::string, float> m_MagazineInteractionBoltPullDistanceMetersProfileOverrides;
	float m_MagazineInteractionBoltReturnDistanceMeters = 0.018f;
	std::string m_MagazineInteractionBoltReturnDistanceMetersOverridesSpec;
	std::unordered_map<int, float> m_MagazineInteractionBoltReturnDistanceMetersOverrides;
	std::unordered_map<std::string, float> m_MagazineInteractionBoltReturnDistanceMetersProfileOverrides;
	Vector m_MagazineInteractionBoltBoxHalfExtentsMeters = { 0.045f, 0.035f, 0.035f };
	std::string m_MagazineInteractionBoltBoxHalfExtentsMetersOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionBoltBoxHalfExtentsMetersOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionBoltBoxHalfExtentsMetersProfileOverrides;
	Vector m_MagazineInteractionBoltBoxLocalOffsetMeters = { 0.0f, 0.0f, 0.0f };
	std::string m_MagazineInteractionBoltBoxLocalOffsetMetersOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionBoltBoxLocalOffsetMetersOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionBoltBoxLocalOffsetMetersProfileOverrides;
	Vector m_MagazineInteractionBoltPullAxisLocal = { 0.0f, 0.0f, 1.0f };
	// Optional exact per-weapon bolt pull axes. Config format:
	// m16:0,0,1;scar:0,1,0;magnum:-1,0,0. Empty falls back to MagazineInteractionBoltPullAxisLocal.
	std::string m_MagazineInteractionBoltPullAxisLocalOverridesSpec;
	std::unordered_map<int, Vector> m_MagazineInteractionBoltPullAxisLocalOverrides;
	std::unordered_map<std::string, Vector> m_MagazineInteractionBoltPullAxisLocalProfileOverrides;
	float m_MagazineInteractionStaleSeconds = 0.20f;
	mutable std::mutex m_MagazineInteractionBoxMutex;
	mutable std::mutex m_MagazineInteractionBoltBoxMutex;
	mutable std::mutex m_VrHandsTwoHandedGripWeaponBoxMutex;
	mutable std::mutex m_MagazineInteractionPoseMutex;
	mutable std::mutex m_MagazineInteractionHandAnchorMutex;
	MagazineInteractionBoxSnapshot m_MagazineInteractionBox{};
	MagazineInteractionBoxSnapshot m_MagazineInteractionBoltBox{};
	MagazineInteractionBoxSnapshot m_VrHandsTwoHandedGripWeaponBox{};
	MagazineInteractionBoxSnapshot m_MagazineInteractionShotgunStableCaptureBox{};
	bool m_MagazineInteractionBoxValid = false;
	bool m_MagazineInteractionBoltBoxValid = false;
	bool m_VrHandsTwoHandedGripWeaponBoxValid = false;
	VrHandMatrix4 m_MagazineInteractionNativeLeftWristWorld{};
	bool m_MagazineInteractionNativeLeftWristValid = false;
	std::chrono::steady_clock::time_point m_MagazineInteractionNativeLeftWristPublishedAt{};
	uint32_t m_MagazineInteractionPublishSeq = 0;
	uint32_t m_MagazineInteractionBoltPublishSeq = 0;
	uint32_t m_VrHandsTwoHandedGripWeaponBoxPublishSeq = 0;
	bool m_MagazineInteractionLeftHandHolding = false;
	bool m_MagazineInteractionReloadTriggered = false;
	bool m_MagazineInteractionReloadCommandPending = false;
	bool m_MagazineInteractionReloadCommandIssued = false;
	std::chrono::steady_clock::time_point m_MagazineInteractionReloadCommandHoldUntil{};
	bool m_MagazineInteractionSuppressLeftInputUntilRelease = false;
	bool m_MagazineInteractionOldMagazinePulled = false;
	bool m_MagazineInteractionChamberEmpty = false;
	bool m_MagazineInteractionOneInChamber = false;
	bool m_MagazineInteractionOldMagazineContactActive = false;
	bool m_MagazineInteractionFreshMagazineContactActive = false;
	bool m_MagazineInteractionBoltContactActive = false;
	bool m_MagazineInteractionShotgunShellMode = false;
	bool m_MagazineInteractionServerClipSettlementActive = false;
	bool m_MagazineInteractionShotgunServerReloadAbortPending = false;
	bool m_MagazineInteractionShotgunDirectShellCommitPending = false;
	bool m_MagazineInteractionShotgunDirectShellServerClipCommitted = false;
	bool m_MagazineInteractionShotgunDirectShellServerReserveCommitted = false;
	bool m_MagazineInteractionServerClipCommitPending = false;
	bool m_MagazineInteractionServerClipCommitted = false;
	bool m_MagazineInteractionServerReserveCommitted = false;
	bool m_MagazineInteractionServerClipReserveHoldActive = false;
	int m_MagazineInteractionShotgunShellsLoadedThisSession = 0;
	int m_MagazineInteractionShotgunLastInterruptedClip = -1;
	int m_MagazineInteractionShotgunDirectShellTargetClip = -1;
	int m_MagazineInteractionShotgunDirectShellAmmoType = -1;
	int m_MagazineInteractionShotgunDirectShellTargetReserve = -1;
	int m_MagazineInteractionShotgunDirectShellExpectedPriorReserve = -1;
	int m_MagazineInteractionShotgunDirectShellWeaponId = 0;
	int m_MagazineInteractionServerClipTarget = -1;
	int m_MagazineInteractionServerClipAmmoType = -1;
	int m_MagazineInteractionServerClipTargetReserve = -1;
	int m_MagazineInteractionServerClipWeaponId = 0;
	int m_MagazineInteractionServerClipExpectedPrior = -1;
	int m_MagazineInteractionServerClipOffset = 0;
	int m_MagazineInteractionServerReserveExpectedPrior = -1;
	int m_MagazineInteractionServerReserveOffset = 0;
	int m_MagazineInteractionServerClipReserveHoldAmmoType = -1;
	int m_MagazineInteractionServerClipReserveHoldReserve = -1;
	int m_MagazineInteractionServerClipReserveHoldOffset = -1;
	MagazineInteractionManualState m_MagazineInteractionState = MagazineInteractionManualState::Idle;
	C_WeaponCSBase* m_MagazineInteractionWeapon = nullptr;
	int m_MagazineInteractionWeaponId = 0;
	int m_MagazineInteractionWeaponEntityIndex = -1;
	std::atomic<int> m_MagazineInteractionCurrentWeaponId{ 0 };
	int m_MagazineInteractionAnyServerHookWeaponId = 0;
	std::chrono::steady_clock::time_point m_MagazineInteractionAnyServerHookLastSeen{};
	int m_MagazineInteractionServerHookWeaponId = 0;
	std::chrono::steady_clock::time_point m_MagazineInteractionServerHookLastSeen{};
	int m_MagazineInteractionShotgunServerHookWeaponId = 0;
	std::chrono::steady_clock::time_point m_MagazineInteractionShotgunServerHookLastSeen{};
	int m_MagazineInteractionStartClip = -1;
	int m_MagazineInteractionMagazineBoneIndex = -1;
	int m_MagazineInteractionViewmodelEntityIndex = -1;
	std::string m_MagazineInteractionMagazineModelName;
	MagazineInteractionBoxSnapshot m_MagazineInteractionSocketBox{};
	MagazineInteractionBoxSnapshot m_MagazineInteractionSocketCaptureBox{};
	MagazineInteractionBoxSnapshot m_MagazineInteractionBoltRestBox{};
	bool m_MagazineInteractionSocketValid = false;
	bool m_MagazineInteractionBoltRestValid = false;
	VrHandMatrix4 m_MagazineInteractionSocketWorld{};
	VrHandMatrix4 m_MagazineInteractionSocketCaptureWorld{};
	VrHandMatrix4 m_MagazineInteractionShotgunStableCaptureModelLocal{};
	VrHandMatrix4 m_MagazineInteractionBoltRestWorld{};
	VrHandMatrix4 m_MagazineInteractionBoltWorld{};
	VrHandMatrix4 m_MagazineInteractionControllerToMagazine{};
	VrHandMatrix4 m_MagazineInteractionDetachedMagazineWorld{};
	bool m_MagazineInteractionFreshPickupBasisValid = false;
	Vector m_MagazineInteractionFreshPickupForward{};
	Vector m_MagazineInteractionFreshPickupRight{};
	float m_MagazineInteractionFreshPickupHmdYawOffsetDeg = 0.0f;
	float m_MagazineInteractionFreshPickupRotationOffset = 0.0f;
	Vector m_MagazineInteractionBoltPullAxisWorld{};
	Vector m_MagazineInteractionBoltInputAxisWorld{};
	Vector m_MagazineInteractionGrabStartLeftControllerPosAbs{};
	Vector m_MagazineInteractionHeldMagazineCenterOffsetLocal{};
	Vector m_MagazineInteractionBoltGrabStartLeftControllerPosAbs{};
	float m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
	float m_MagazineInteractionBoltPullDistance = 0.0f;
	float m_MagazineInteractionBoltMaxPullDistance = 0.0f;
	bool m_MagazineInteractionBoltGrabRequiresGripRelease = false;
	bool m_MagazineInteractionBoltReachedRear = false;
	bool m_MagazineInteractionBoltPullAxisSignLocked = false;
	bool m_MagazineInteractionBoltStageBeforeBackendReloadComplete = false;
	bool m_MagazineInteractionBoltCompletedBeforeBackendReload = false;
	bool m_MagazineInteractionThumbIndexCurlDownPrev = false;
	bool m_MagazineInteractionThreeFingerCurlDownPrev = false;
	bool m_MagazineInteractionShotgunStableCaptureValid = false;
	std::chrono::steady_clock::time_point m_MagazineInteractionStarted{};
	std::chrono::steady_clock::time_point m_MagazineInteractionFreshGrabbedAt{};
	std::chrono::steady_clock::time_point m_MagazineInteractionPostInsertStarted{};
	std::chrono::steady_clock::time_point m_MagazineInteractionBoltStageStarted{};
	std::chrono::steady_clock::time_point m_MagazineInteractionBoltGrabbedAt{};
	std::chrono::steady_clock::time_point m_MagazineInteractionShotgunServerReloadAbortUntil{};
	std::chrono::steady_clock::time_point m_MagazineInteractionShotgunDirectShellCommitUntil{};
	std::chrono::steady_clock::time_point m_MagazineInteractionServerClipCommitUntil{};
	std::chrono::steady_clock::time_point m_MagazineInteractionNativeReloadSuppressUntil{};
	std::chrono::steady_clock::time_point m_MagazineInteractionViewmodelFreezeDeferredUntil{};
	int m_MagazineInteractionNativeReloadSuppressWeaponId = 0;
	std::string m_MagazineInteractionSyntheticClipOutSample;
	std::chrono::steady_clock::time_point m_MagazineInteractionSyntheticClipOutStarted{};
	std::string m_MagazineInteractionSyntheticClipInSample;
	std::chrono::steady_clock::time_point m_MagazineInteractionSyntheticClipInStarted{};
	std::string m_MagazineInteractionSyntheticBoltBackSample;
	std::chrono::steady_clock::time_point m_MagazineInteractionSyntheticBoltBackStarted{};
	std::string m_MagazineInteractionSyntheticBoltForwardSample;
	std::chrono::steady_clock::time_point m_MagazineInteractionSyntheticBoltForwardStarted{};
	std::string m_MagazineInteractionCapturedBoltBackSample;
	std::string m_MagazineInteractionCapturedBoltForwardSample;
	int m_MagazineInteractionCapturedBoltBackSoundScore = -1;
	int m_MagazineInteractionCapturedBoltForwardSoundScore = -1;
	std::chrono::steady_clock::time_point m_MagazineInteractionEmptyFireSoundLastPlayed{};
	std::atomic<uint32_t> m_MagazineInteractionLeftHandPoseActive{ 0 };

	// Optional exact per-weapon visual magazine bone overrides. Empty keeps automatic detection.
	// Config format: ak47:Magazine_Main,m16a1:v_weapon.M4A1_Clip,scar:j_mag1.
	std::string m_MagazineInteractionMagazineBoneOverridesSpec;
	std::unordered_map<int, std::vector<std::string>> m_MagazineInteractionMagazineBoneOverrides;
	std::unordered_map<std::string, std::vector<std::string>> m_MagazineInteractionMagazineBoneProfileOverrides;
	// Optional exact per-weapon bolt/charging-handle bone overrides for MagazineInteraction.
	// Same format and aliases as the magazine bone override config key.
	std::string m_MagazineInteractionBoltBoneOverridesSpec;
	std::unordered_map<int, std::vector<std::string>> m_MagazineInteractionBoltBoneOverrides;
	std::unordered_map<std::string, std::vector<std::string>> m_MagazineInteractionBoltBoneProfileOverrides;
	std::atomic<uint32_t> m_MagazineInteractionCurrentModelFingerprint{ 0 };
	std::atomic<uint32_t> m_MagazineInteractionCurrentBoneSignature{ 0 };
	mutable std::mutex m_MagazineInteractionCalibrationMutex;
	MagazineInteractionCalibrationSnapshot m_MagazineInteractionCalibrationSnapshot{};
	uint32_t m_MagazineInteractionCalibrationPublishSeq = 0;
	std::atomic<bool> m_MagazineInteractionCalibrationOverlayActive{ false };
	std::atomic<int> m_MagazineInteractionCalibrationSelectedBone{ -1 };
	std::atomic<int> m_MagazineInteractionCalibrationStep{ 0 };
	std::atomic<bool> m_MagazineInteractionCalibrationPreviewAnchorValid{ false };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewAnchorOriginX{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewAnchorOriginY{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewAnchorOriginZ{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewAnchorPitchDeg{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewAnchorYawDeg{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewAnchorRollDeg{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewForwardMeters{ 0.75f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewRightMeters{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewUpMeters{ -0.08f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewPitchDeg{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewYawDeg{ 0.0f };
	std::atomic<float> m_MagazineInteractionCalibrationPreviewRollDeg{ 0.0f };
	// Shared magazine/socket axis tuning used by MagazineInteraction.
	Vector m_MagazineInteractionMagazineInsertionAxisLocal = { 0.0f, -1.0f, 0.0f };
	Vector m_MagazineInteractionMagazineHandOffsetMeters = { 0.0f, 0.0f, 0.0f };
	Vector m_MagazineInteractionMagazineHandRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
	bool m_SplitArmsToControllers = false;
	float m_HudDistance = 1.3f;
	float m_FixedHudXOffset = 0.0f;
	float m_FixedHudYOffset = 0.25f;
	float m_FixedHudDistanceOffset = 0.25f;
	float m_HudSize = 1.3f;
	float m_TopHudCurvature = 0.2f;
	bool m_HudFollowHmdMovement = false;
	bool m_HudAlwaysVisible = false;
	bool m_HudToggleState = false;
	// Runtime HUD requests used by HudAlwaysVisible=false. ShowHUD is a hold action;
	// the off-hand lift gesture is suppressed while that hand is gripping a two-handed weapon.
	std::atomic<uint32_t> m_HudShowActionHeld{ 0 };
	std::atomic<uint32_t> m_HudLiftGestureActive{ 0 };
	std::chrono::steady_clock::time_point m_HudLiftGestureVisibleUntil{};
	std::chrono::steady_clock::time_point m_HudChatVisibleUntil{};
	// Queued rendering (mat_queue_mode!=0): keep HUD visibility stable for a short
	// window after a successful HUD capture so transient render-thread misses don't
	// cause top-HUD flicker when frame rate dips.
	std::atomic<uint64_t> m_QueuedHudFreshUntilMs{ 0 };

	// Hand HUD background opacity (0..1). Applies to the panel fill only (text/icons stay opaque).
	float m_LeftWristHudBgAlpha = 0.85f;
	float m_RightAmmoHudBgAlpha = 0.70f;

	// Right ammo HUD: maximum visible width fraction (U max).
	// The HUD now auto-computes a tight width that fits the ammo string and then clamps to this value.
	// 1.0 = no clamp (recommended).
	float m_RightAmmoHudUVMaxU = 1.0f;

	// Hand HUD alpha (legacy, 0..1): treated as an extra BACKGROUND opacity multiplier.
	// We intentionally do NOT apply IVROverlay::SetOverlayAlpha to the whole overlay,
	// because it makes text/bars fade too.
	float m_LeftWristHudAlpha = 0.5f;
	float m_RightAmmoHudAlpha = 0.5f;

	// Left wrist HUD: battery label font scale (1..4) for DrawText5x7.
	int   m_LeftWristHudBatteryTextScale = 1;

	// Hand HUD: client-side temp health (m_healthBuffer) decay rate (HP per second).
	// L4D2 default is ~0.27, but servers can override; expose as config for now.
	float m_HandHudTempHealthDecayRate = 0.27f;

	// ----------------------------
	// Hand HUD overlays (SteamVR overlays, raw textures)
	// ----------------------------
	bool  m_LeftWristHudEnabled = false;
	float m_LeftWristHudWidthMeters = 0.1f;
	float m_LeftWristHudXOffset = 0.01f;
	float m_LeftWristHudYOffset = 0.01f;
	float m_LeftWristHudZOffset = -0.0f;
	QAngle m_LeftWristHudAngleOffset = { -75.0f, 0.0f, 0.0f };

	bool  m_RightAmmoHudEnabled = false;
	float m_RightAmmoHudWidthMeters = 0.13f; // width reduced by ~2/3 by default
	float m_RightAmmoHudXOffset = -0.07f;
	float m_RightAmmoHudYOffset = 0.03f;
	float m_RightAmmoHudZOffset = -0.09f;
	QAngle m_RightAmmoHudAngleOffset = { -75.0f, 0.0f, 0.0f };

	float m_LeftWristHudCurvature = 0.0f;
	bool  m_LeftWristHudShowBattery = true;
	bool  m_LeftWristHudShowTeammates = true;

	// ----------------------------
	// Hand HUD: world-quad mode (HMD-relative overlays using GPU textures)
	//
	// This mode keeps the existing hand HUD drawing code, but uploads the pixels into
	// a dynamic D3D9 texture and presents it as a standard overlay texture (Vulkan)
	// placed relative to the HMD as a quad HUD.
	// ----------------------------
	bool  m_HandHudWorldQuadEnabled = true;
	// If true, keep the HUD attached to left/right controllers (tracked-device-relative),
	// but still use the GPU texture upload path.
	bool  m_HandHudWorldQuadAttachToControllers = true;
	// Distance in meters in front of the HMD (positive). Internally applied as -Z.
	float m_HandHudWorldQuadDistanceMeters = 0.5f;
	// Vertical offset in meters (positive up). Negative moves down.
	float m_HandHudWorldQuadYOffsetMeters = 0.2f;
	// Horizontal gap between left/right panels when both are visible.
	float m_HandHudWorldQuadXGapMeters = 0.01f;
	// Additional rotation applied to both panels (pitch,yaw,roll in degrees).
	QAngle m_HandHudWorldQuadAngleOffset = { 40.0f, 0.0f, 0.0f };

	// Backing textures for world-quad hand HUD (dynamic D3D9 textures mirrored to Vulkan).
	IDirect3DTexture9* m_D9LeftWristHudDynTex = nullptr;
	IDirect3DSurface9* m_D9LeftWristHudDynSurface = nullptr;
	SharedTextureHolder m_VKLeftWristHudDyn{};
	int m_D9LeftWristHudDynW = 0;
	int m_D9LeftWristHudDynH = 0;

	IDirect3DTexture9* m_D9RightAmmoHudDynTex = nullptr;
	IDirect3DSurface9* m_D9RightAmmoHudDynSurface = nullptr;
	SharedTextureHolder m_VKRightAmmoHudDyn{};
	int m_D9RightAmmoHudDynW = 0;
	int m_D9RightAmmoHudDynH = 0;

	// Standalone special infected intent-sense HUD dynamic textures.
	// Use a real two-texture GPU ring: SteamVR/OpenVR can cache a submitted texture handle,
	// so updating and resubmitting the same handle may show only the first frame on some runtimes.
	// No SetOverlayRaw fallback is used; failures should stay visible in logs/tests.
	std::array<IDirect3DTexture9*, 2> m_D9SpecialInfectedIntentSenseHudDynTex{};
	std::array<IDirect3DSurface9*, 2> m_D9SpecialInfectedIntentSenseHudDynSurface{};
	std::array<SharedTextureHolder, 2> m_VKSpecialInfectedIntentSenseHudDyn{};
	std::array<int, 2> m_D9SpecialInfectedIntentSenseHudDynW{};
	std::array<int, 2> m_D9SpecialInfectedIntentSenseHudDynH{};
	uint8_t m_SpecialInfectedIntentSenseHudDynFront = 0;
	// Debug logging for hand HUD update stalls (UpdateHandHudOverlays).
	bool  m_HandHudDebugLog = false;
	float m_HandHudDebugLogHz = 1.0f; // max prints per second; 0 disables throttling
	std::chrono::steady_clock::time_point m_HandHudDebugLastLog{};
	std::chrono::steady_clock::time_point m_HandHudDebugLastCall{};
	std::chrono::steady_clock::time_point m_HandHudDebugLastLeftUpload{};
	std::chrono::steady_clock::time_point m_HandHudDebugLastRightUpload{};
	uint32_t m_HandHudDebugLeftUploadCount = 0;
	uint32_t m_HandHudDebugRightUploadCount = 0;
	int m_HandHudDebugLastLeftSetRawErr = 0;
	int m_HandHudDebugLastRightSetRawErr = 0;
	int m_HandHudDebugLastLeftShowErr = 0;
	int m_HandHudDebugLastRightShowErr = 0;

	// Serialize all IVROverlay calls. OpenVR overlay APIs are not guaranteed thread-safe,
	// and concurrent access can lead to persistent EVROverlayError_RequestFailed.
	std::mutex m_VROverlayMutex{};
	// Hand HUD overlay recovery state (for SetOverlayRaw failures).
	uint32_t m_HandHudLeftConsecutiveRawFails = 0;
	uint32_t m_HandHudRightConsecutiveRawFails = 0;
	std::chrono::steady_clock::time_point m_HandHudLastOverlayRecover{};
	uint32_t m_HandHudOverlayRecoverCount = 0;

	// Double-buffered pixel storage to avoid flicker/tearing when SteamVR reads the same buffer we are updating.
	std::array<std::vector<uint8_t>, 2> m_LeftWristHudPixels{};
	std::array<std::vector<uint8_t>, 2> m_RightAmmoHudPixels{};
	std::array<std::vector<uint8_t>, 2> m_SpecialInfectedIntentSenseHudPixels{};
	uint8_t m_LeftWristHudPixelsFront = 0;
	uint8_t m_RightAmmoHudPixelsFront = 0;
	uint8_t m_SpecialInfectedIntentSenseHudPixelsFront = 0;
	int m_LeftWristHudTexW = 256;
	int m_LeftWristHudTexH = 128;
	int m_RightAmmoHudTexW = 256;
	int m_RightAmmoHudTexH = 128;
	int m_SpecialInfectedIntentSenseHudTexW = 512;
	int m_SpecialInfectedIntentSenseHudTexH = 192;
	// Hand HUD temp-health decay state (per player index).
	// We only get m_healthBuffer (amount) + m_healthBufferTime (start time).
	// The engine computes the decayed remaining value at draw time; we replicate that using wall-clock.
	struct TempHealthDecayState
	{
		float rawBuffer = 0.0f;
		float rawBufferTime = 0.0f;
		std::chrono::steady_clock::time_point wallStart{};
		float lastRemaining = 0.0f;
		bool initialized = false;
	};
	static constexpr size_t kHandHudPlayerSlots = 65; // Source MAX_PLAYERS (incl. 1..64)
	std::array<TempHealthDecayState, kHandHudPlayerSlots> m_HandHudTempHealthStates{};

	// Cached values (avoid redrawing every frame)
	int  m_LastHudHealth = -9999;
	int  m_LastHudTempHealth = -9999;
	int  m_LastHudThrowable = -1;
	int  m_LastHudMedItem = -1;
	int  m_LastHudPillItem = -1;
	int  m_LastHudCommonKills = -9999;
	int  m_LastHudSpecialKills = -9999;
	bool m_LastHudIncap = false;
	bool m_LastHudLedge = false;
	bool m_LastHudThirdStrike = false;
	bool m_LastHudAimTargetVisible = false;
	int  m_LastHudAimTargetIndex = -1;
	int  m_LastHudAimTargetPct = -1;
	int  m_LastHudClip = -9999;
	int  m_LastHudReserve = -9999;
	int  m_LastHudUpg = -9999;
	int  m_LastHudUpgBits = 0;
	int  m_LastHudWeaponId = -1;

	// Right ammo HUD: last drawn hit-target state (for change detection).
	bool m_LastHudHitVisible = false;
	int  m_LastHudHitPct = -1;
	std::uintptr_t m_LastHudHitTag = 0;

	// Hand HUD rendering caches (avoid re-rendering static background)
	std::vector<uint8_t> m_LeftWristHudBgCache{};
	int m_LeftWristHudBgCacheW = 0;
	int m_LeftWristHudBgCacheH = 0;
	uint8_t m_LeftWristHudBgCacheA = 0;
	uint32_t m_LastHudTeammatesHash = 0;
	uint32_t m_LastHudAimTargetNameHash = 0;
	bool m_LastSpecialInfectedIntentSenseHudVisible = false;
	int m_LastSpecialInfectedIntentSenseHudRevisionDrawn = 0;
	std::chrono::steady_clock::time_point m_LastSpecialInfectedIntentSenseHudUploadTime{};

	// Standalone desktop companion windows. They mirror selected plugin HUD surfaces to
	// normal desktop windows instead of drawing them into the game backbuffer.
	HWND m_DesktopRearMirrorWindow = nullptr;
	HWND m_DesktopIntentSenseHudWindow = nullptr;
	IDirect3DSurface9* m_D9DesktopCompanionRearMirrorReadback = nullptr;
	std::mutex m_DesktopCompanionHudMutex{};
	std::vector<uint8_t> m_DesktopCompanionRearMirrorBgra{};
	std::vector<uint8_t> m_DesktopCompanionIntentHudBgra{};
	int m_DesktopCompanionRearMirrorW = 0;
	int m_DesktopCompanionRearMirrorH = 0;
	int m_DesktopCompanionIntentHudW = 0;
	int m_DesktopCompanionIntentHudH = 0;
	bool m_DesktopRearMirrorWindowEnabled = false;
	bool m_DesktopIntentSenseHudWindowEnabled = false;
	bool m_DesktopRearMirrorWindowShown = false;
	bool m_DesktopIntentSenseHudWindowShown = false;
	std::chrono::steady_clock::time_point m_LastDesktopCompanionRearMirrorCopyTime{};

	std::vector<uint8_t> m_RightAmmoHudBgCache{};
	int m_RightAmmoHudBgCacheW = 0;
	int m_RightAmmoHudBgCacheH = 0;
	int m_RightAmmoHudBgCacheVisW = 0;
	uint8_t m_RightAmmoHudBgCacheA = 0;
	// Dynamic maxima for percentage thresholds (works even if weapon scripts change clip/reserve sizes)
	int  m_HudMaxClipObserved = 0;
	int  m_HudMaxReserveObserved = 0;

	float m_ControllerSmoothing = 0.0f;
	bool m_ControllerSmoothingInitialized = false;
	float m_HeadSmoothing = 0.0f;
	bool m_HeadSmoothingInitialized = false;
	Vector m_HmdPosSmoothed = { 0,0,0 };
	QAngle m_HmdAngSmoothed = { 0,0,0 };
	bool m_QueuedRenderHmdYawTurnPathInitialized = false;
	float m_QueuedRenderHmdYawTurnPathPrevLocalYaw = 0.0f;
	CustomActionBinding m_CustomAction1Binding{ "thirdpersonshoulder" };
	CustomActionBinding m_CustomAction2Binding{};
	CustomActionBinding m_CustomAction3Binding{};
	CustomActionBinding m_CustomAction4Binding{};
	CustomActionBinding m_CustomAction5Binding{};

	bool m_MotionGesturesEnabled = true;
	bool m_MotionGestureSwingEnabled = true;
	bool m_MotionGesturePushEnabled = true;
	bool m_MotionGestureDownSwingEnabled = true;
	bool m_MotionGestureJumpEnabled = true;
	float m_MotionGestureSwingThreshold = 2.0f;
	float m_MotionGesturePushThreshold = 1.5f;
	float m_MotionGestureDownSwingThreshold = 2.0f;
	float m_MotionGestureJumpThreshold = 2.0f;
	float m_MotionGestureCooldown = 0.8f;
	float m_MotionGestureHoldDuration = 0.2f;

	bool m_MotionGestureInitialized = false;
	std::chrono::steady_clock::time_point m_LastGestureUpdateTime{};
	Vector m_PrevLeftControllerLocalPos = { 0,0,0 };
	Vector m_PrevRightControllerLocalPos = { 0,0,0 };
	Vector m_PrevHmdLocalPos = { 0,0,0 };
	std::chrono::steady_clock::time_point m_SecondaryAttackGestureHoldUntil{};
	std::chrono::steady_clock::time_point m_ReloadGestureHoldUntil{};
	std::chrono::steady_clock::time_point m_JumpGestureHoldUntil{};
	std::chrono::steady_clock::time_point m_SecondaryGestureCooldownEnd{};
	std::chrono::steady_clock::time_point m_ReloadGestureCooldownEnd{};
	std::chrono::steady_clock::time_point m_JumpGestureCooldownEnd{};
	float m_InventoryGestureRange = 0.16f;
	Vector m_InventoryChestOffset = { 0.45f, 0.0f, 0.5f };
	Vector m_InventoryBackOffset = { 0.12f, 0.0f, 0.1f };
	Vector m_InventoryLeftWaistOffset = { 0.45f, -0.28f, 0.25f };
	Vector m_InventoryRightWaistOffset = { 0.45f, 0.28f, 0.25f };

	// Inventory quick-switch (Half-Life: Alyx style): press/hold a bind to spawn 4 zones around the RIGHT hand.
	// When enabled, the legacy body-anchored inventory switching is disabled.
	bool m_InventoryQuickSwitchEnabled = true;
	Vector m_InventoryQuickSwitchOffset = { 0.05f, 0.2f, 0.2f }; // meters (forward,right,up) in quick-switch basis
	float m_InventoryQuickSwitchZoneRadius = 0.15f;               // meters, selection radius per zone

	// Runtime state for quick-switch
	bool m_InventoryQuickSwitchActive = false;
	bool m_InventoryQuickSwitchArmed = false;
	// Stored in *tracking-local* Source units (i.e. rightControllerAbs - (CameraAnchor - (0,0,64))).
	// This keeps selection stable while the player translates in-game (stick movement), while
	// still allowing debug rendering by adding the same anchor back to get world space.
	Vector m_InventoryQuickSwitchOrigin = { 0,0,0 };
	Vector m_InventoryQuickSwitchForward = { 1,0,0 };
	Vector m_InventoryQuickSwitchRight = { 0,1,0 };

	// Legacy inventory: swallow Reload/Crouch until release when consumed by inventory switching.
	bool m_BlockReloadUntilRelease = false;
	bool m_BlockCrouchUntilRelease = false;

	// Inventory anchor basis: apply offsets in a BODY space (yaw-only), not head pitch/roll.
	// This makes anchors stable when you look up/down.
	Vector m_InventoryBodyOriginOffset = { -0.1f, 0.0f, -0.28f }; // meters (forward,right,up) in body space

	// Optional front-of-view debug helper markers (purely visual) so anchors behind/at waist are still discoverable.
	float m_InventoryHudMarkerDistance = 0.45f;   // meters forward from head
	float m_InventoryHudMarkerUpOffset = -0.10f;  // meters up (+) / down (-)
	float m_InventoryHudMarkerSeparation = 0.14f; // meters between markers horizontally
	// Auto mat_queue_mode management for multicore rendering.
	// When enabled, the mod will keep mat_queue_mode=1 in menus/loading/pause/scoreboard,
	// and switch to mat_queue_mode=2 once fully in-game.
	bool m_AutoMatQueueMode = false;
	int  m_AutoMatQueueModeLastRequested = -999;
	std::chrono::steady_clock::time_point m_AutoMatQueueModeLastCmdTime{};

	// One-shot menu ConVar injection: apply launch-style visual defaults after the first main-menu entry.
	bool m_MainMenuOneShotVisualCvarsInjected = false;

	// Optional persisted video.txt profile for VR. Applied once when enabled and again after returning to the main menu.
	bool m_VrRecommendedVideoSettingsEnabled = false;
	bool m_VrRecommendedVideoSettingsApplyPending = false;
	bool m_VrRecommendedVideoSettingsAppliedThisSession = false;

	// Auto fps_max in main menu: set fps_max to match HMD refresh rate when VR is active.
	bool m_MenuFpsMaxSent = false;
	int  m_MenuFpsMaxLastHz = 0;

	struct ShadowControlEntityDefaults
	{
		bool disableShadows = false;
		float maxDist = 0.0f;
		bool enableLocalLightShadows = false;
	};

	struct EnvProjectedTextureEntityDefaults
	{
		bool enableShadows = true;
		int shadowQuality = 0;
	};

	// Shadow controls discovered from L4D2's client-side shadow manager / flashlight paths.
	// We stage config changes here and apply them on the main update thread via VEngineCvar007.
	bool m_ShadowTweaksEnabled = false;
	int m_ShadowCvarShadows = 1;
	int m_ShadowCvarRenderToTexture = 0;
	int m_ShadowCvarFlashlightDepthTexture = 0;
	int m_ShadowCvarFlashlightDepthRes = 256;
	int m_ShadowCvarHalfUpdateRate = 1;
	int m_ShadowCvarMaxRendered = 0;
	float m_ShadowCvarMaxRenderableDist = 0.0f;
	int m_ShadowCvarFlashlightDetailProps = 0;
	int m_ShadowCvarMobSimpleShadows = 1;
	int m_ShadowCvarWorldLightShadows = 1;
	int m_ShadowCvarFlashlightModels = 1;
	int m_ShadowCvarShadowsOnRenderables = 1;
	int m_ShadowCvarFlashlightRenderModels = 1;
	float m_ShadowCvarPlayerShadowDist = 0.0f;
	int m_ShadowCvarInfectedShadows = 1;
	float m_ShadowCvarNbShadowBlobbyDist = 493.820007f;
	float m_ShadowCvarNbShadowCullDist = 500.584991f;
	int m_ShadowCvarFlashlightInfectedShadows = 1;
	bool m_ShadowTweaksApplied = false;
	bool m_ShadowOriginalsCaptured = false;
	int m_ShadowOrigShadows = 1;
	int m_ShadowOrigRenderToTexture = 1;
	int m_ShadowOrigFlashlightDepthTexture = 1;
	int m_ShadowOrigFlashlightDepthRes = 1024;
	int m_ShadowOrigHalfUpdateRate = 0;
	int m_ShadowOrigMaxRendered = 32;
	float m_ShadowOrigMaxRenderableDist = 3000.0f;
	int m_ShadowOrigFlashlightDetailProps = 0;
	int m_ShadowOrigMobSimpleShadows = 0;
	int m_ShadowOrigWorldLightShadows = 1;
	int m_ShadowOrigFlashlightModels = 1;
	int m_ShadowOrigShadowsOnRenderables = 1;
	int m_ShadowOrigFlashlightRenderModels = 1;
	float m_ShadowOrigPlayerShadowDist = 3000.0f;
	int m_ShadowOrigInfectedShadows = 1;
	float m_ShadowOrigNbShadowBlobbyDist = 3000.0f;
	float m_ShadowOrigNbShadowCullDist = 3000.0f;
	int m_ShadowOrigFlashlightInfectedShadows = 1;
	bool m_ShadowEntityTweaksEnabled = false;
	bool m_ShadowEntityDisableShadows = false;
	float m_ShadowEntityMaxDist = 1200.0f;
	bool m_ShadowEntityLocalLightShadows = false;
	bool m_ShadowProjectedTextureEnableShadows = true;
	int m_ShadowProjectedTextureQuality = 0;
	bool m_ShadowEntityOffsetsLogged = false;
	bool m_ShadowEntityOverridesApplied = false;
	std::chrono::steady_clock::time_point m_ShadowEntityLastRefreshTime{};
	std::unordered_map<int, ShadowControlEntityDefaults> m_ShadowControlEntityDefaults;
	std::unordered_map<int, EnvProjectedTextureEntityDefaults> m_EnvProjectedTextureEntityDefaults;
	std::atomic<bool> m_ShadowSettingsDirty{ true };

	// Desktop-window mirror. This copies one VR eye into the implicit D3D9 swapchain
	// backbuffer before Present(), so the normal desktop game window can show a live
	// mirror without re-rendering the scene.
	//   DesktopMirrorEnabled=true/false
	//   DesktopMirrorEye=right/left or 1/0
	//   DesktopMirrorKeepAspect=true/false
	//   DesktopMirrorLinearFilter=true/false
	//   DesktopMirrorHidePluginOverlays=true/false
	// When hiding plugin overlays, the selected eye is mirrored through desktopMirrorClean0
	// before VR-only post overlays are drawn. This clean target is single-thread only.
	// Queued/multicore rendering mirrors the regular eye directly: inserting another clean
	// world RenderView can destabilize Source's shared shadow RTT state under scene pressure.
	bool m_DesktopMirrorEnabled = true;
	int  m_DesktopMirrorEye = 1; // 0 = left eye, 1 = right eye
	bool m_DesktopMirrorKeepAspect = true;
	bool m_DesktopMirrorLinearFilter = true;
	// Requested value from config.txt. The runtime effective flag below is true only
	// when the clean desktop mirror target exists.
	bool m_DesktopMirrorHidePluginOverlaysRequested = true;
	// Runtime effective value. External mirror code can keep reading this field.
	bool m_DesktopMirrorHidePluginOverlays = true;
	bool m_DesktopMirrorCleanRenderingPass = false;

	// ReShadeVRCompat=true/false
	// When enabled, the VR render path uses conservative per-eye D3D9 RT binding,
	// forces DXVK D3D9 device locking for ReShade + Source queued rendering, submits
	// full-eye texture bounds, and disables application-managed explicit compositor
	// timing. This is intended only for real SteamVR/ALVR HMDs with ReShade loaded.
	bool m_ReShadeVRCompat = false;
	// ReShade + mat_queue_mode 2 needs an additional coarse guard around eye RT writes,
	// post-Present resolve, and SteamVR submit. Per-call D3D9 locking is not enough here:
	// the Present thread can otherwise resolve half-written eye textures when complex
	// scenes make the queued render worker lag behind.
	mutable std::recursive_mutex m_ReShadeVRCompatSurfaceMutex;
	std::atomic<uint32_t> m_ReShadeVRCompatResolvedFrameId{ 0 };
	// Raw D3D overlays must be composited into a submit snapshot at most once.
	std::atomic<uint32_t> m_PostPresentSubmitOverlayFrameId{ 0 };
	// In Source queued rendering, RenderView can return before MaterialSystem::EndFrame
	// has flushed all D3D work. ReShadeVRCompat resolves eye RTs after Present, so only
	// publish a completed stereo frame after EndFrame has finished.
	std::atomic<uint32_t> m_ReShadeVRCompatPendingRenderReady{ 0 };
	std::atomic<uint32_t> m_ReShadeVRCompatPendingRenderPoseToken{ 0 };
	std::atomic<uint32_t> m_ReShadeVRCompatPendingRenderFrameSeq{ 0 };
	std::atomic<uint32_t> m_ReShadeVRCompatPendingDuplicatePose{ 0 };


	bool m_FlashlightEnhancementEnabled = false;
	bool m_FlashlightFollowHmd = true;
	bool m_FlashlightFollowHmdForFirearms = false;
	bool m_FlashlightEnhancementApplied = false;
	bool m_FlashlightEnhancementOriginalsCaptured = false;
	float m_FlashlightEnhancement3rdPersonRange = 300.0f;
	float m_FlashlightEnhancementBrightness = 0.5f;
	float m_FlashlightEnhancementFov = 80.0f;
	float m_FlashlightEnhancementOrig3rdPersonRange = 60.0f;
	float m_FlashlightEnhancementOrigBrightness = 0.25f;
	float m_FlashlightEnhancementOrigFov = 53.0f;
	int m_FlashlightEnhancementOrig3rdPersonRangeFlags = -1;
	int m_FlashlightEnhancementOrigBrightnessFlags = -1;
	int m_FlashlightEnhancementOrigFovFlags = -1;
	std::atomic<bool> m_FlashlightEnhancementSettingsDirty{ true };
	struct LocalVScriptConvarEntry
	{
		std::string name;
		std::string value;
		std::string originalValue;
		int flags = 0;
		bool lockProtected = false;
	};
	bool m_LocalVScriptConvarsEnabled = false;
	bool m_LocalVScriptConvarsLogEnabled = false;
	bool m_LocalVScriptConvarsBlockExternalWrites = true;
	std::string m_LocalVScriptConvarsPath = "VR\\local_client_convars.nut";
	std::vector<LocalVScriptConvarEntry> m_LocalVScriptConvars{};
	std::unordered_set<std::string> m_ShadowProtectedConvars{};
	std::unordered_set<std::string> m_FlashlightEnhancementProtectedConvars{};
	bool m_LocalVScriptConvarsApplied = false;
	std::atomic<bool> m_LocalVScriptConvarsDirty{ true };
	float m_LocalVScriptConvarsMapAuditDelaySeconds = 1.0f;
	bool m_LocalVScriptConvarsMapAuditPending = false;
	std::chrono::steady_clock::time_point m_LocalVScriptConvarsMapAuditDueTime{};
	bool m_LocalVScriptConvarsHadLocalPlayerPrev = false;
	float m_LocalVScriptConvarsBlockedWriteLogHz = 1.0f;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_LocalVScriptConvarBlockedWriteLastLog{};
	mutable std::mutex m_LocalVScriptConvarLockMutex;
	bool m_AutoFlashlightEnabled = false;
	float m_AutoFlashlightDarkThreshold = 45.0f;
	float m_AutoFlashlightBrightThreshold = 80.0f;
	bool m_AutoFlashlightDebugLog = false;
	float m_AutoFlashlightDebugLogHz = 2.0f;
	bool m_AutoFlashlightHasScreenLuma = false;
	float m_AutoFlashlightCenterMedianLuma = 255.0f;
	float m_AutoFlashlightCenterLowLuma = 255.0f;
	float m_AutoFlashlightPeripheralMedianLuma = 255.0f;
	bool m_AutoFlashlightHasKnownState = false;
	bool m_AutoFlashlightLastKnownOn = false;
	int m_AutoFlashlightDarkDecisionSamples = 0;
	int m_AutoFlashlightBrightDecisionSamples = 0;
	std::chrono::steady_clock::time_point m_AutoFlashlightNextSampleTime{};
	std::chrono::steady_clock::time_point m_AutoFlashlightLastToggleTime{};
	std::chrono::steady_clock::time_point m_AutoFlashlightManualOverrideUntil{};
	std::chrono::steady_clock::time_point m_AutoFlashlightLastDebugLog{};
	IDirect3DTexture9* m_D9AutoFlashlightLumaTexture = nullptr;
	IDirect3DSurface9* m_D9AutoFlashlightLumaSurface = nullptr;
	IDirect3DSurface9* m_D9AutoFlashlightReadbackSurface = nullptr;
	int m_AutoFlashlightReadbackWidth = 0;
	int m_AutoFlashlightReadbackHeight = 0;

	bool m_DrawInventoryAnchors = true;
	int m_InventoryAnchorColorR = 0;
	int m_InventoryAnchorColorG = 255;
	int m_InventoryAnchorColorB = 255;
	int m_InventoryAnchorColorA = 255;
	bool m_ServerHookFallbackPending = false;
	int m_ServerHookFallbackDelayMs = 250;
	std::chrono::steady_clock::time_point m_ServerHookFallbackCheckTime{};
	bool m_ServerHookFallbackForcedNonVRServerMovement = false;
	bool m_ConfigForceNonVRServerMovement = false;
	bool m_ForceNonVRServerMovement = false;
	bool m_Roomscale1To1Movement = false;
	float m_Roomscale1To1MaxStepMeters = 0.35f;

	// Roomscale 1:1 movement:
	// Local/listen VR servers can consume HMD planar movement directly through server.dll.
	// Other servers fall back to standard CUserCmd movement for compatibility.
	bool m_Roomscale1To1DecoupleCamera = true;
	bool m_Roomscale1To1DisableWhileThumbstick = true;
	bool m_Roomscale1To1ServerMove = true;
	float m_Roomscale1To1MovementScale = 1.0f;
	float m_Roomscale1To1MinApplyMeters = 0.005f;
	bool m_Roomscale1To1PhysicalCrouch = true;
	float m_Roomscale1To1CrouchEnterMeters = 0.25f;
	float m_Roomscale1To1CrouchExitMeters = 0.18f;

	Vector m_Roomscale1To1PrevCorrectedAbs = {};
	bool m_Roomscale1To1PrevValid = false;
	Vector m_Roomscale1To1LastEngineEye = {};
	bool m_Roomscale1To1LastEngineEyeValid = false;
	Vector m_Roomscale1To1PendingVisualWorldDelta = {};
	bool m_Roomscale1To1PendingVisualWorldDeltaValid = false;
	std::mutex m_Roomscale1To1ServerMoveMutex;
	Vector m_Roomscale1To1PendingServerWorldDelta = {};
	bool m_Roomscale1To1PendingServerWorldDeltaValid = false;
	float m_Roomscale1To1StandingHmdZ = 0.0f;
	bool m_Roomscale1To1StandingHmdZValid = false;
	bool m_Roomscale1To1PhysicalCrouchActive = false;

	// Alyx-style teleport locomotion. This is intentionally independent from the
	// roomscale small-step movement queue: teleport targets must never degrade into
	// ordinary CUserCmd movement when validation rejects the destination.
	static constexpr int TELEPORT_ARC_SEGMENTS = 96;
	static constexpr uint32_t TELEPORT_VIEWMODEL_SUPPRESS_MS = 75u;
	// Config: TeleportMaxDistanceMeters. Shared by real teleport and camera-only scouting.
	// Keep preview, client validation and server validation on the same runtime value.
	float m_TeleportMaxDistanceMeters = 20.0f;
	// Config: TeleportVisualScoutOnNonVRServerEnabled. Disabled by default.
	// Only when enabled may a server without the VR SetOrigin path fall back to
	// the detached camera-only scout mode. Real teleport behavior is unchanged.
	bool m_TeleportVisualScoutOnNonVRServerEnabled = false;
	std::array<Vector, TELEPORT_ARC_SEGMENTS + 1> m_TeleportArcPoints{};
	int m_TeleportArcPointCount = 0;
	bool m_TeleportTargetingActive = false;
	bool m_TeleportCommitPending = false;
	bool m_TeleportTargetValid = false;
	// True when the server-side SetOrigin path is unavailable. This preview uses
	// a collision-free controller ray; releasing teleport enters a camera-only scout view.
	bool m_TeleportTargetVisualScoutOnly = false;
	Vector m_TeleportTargetWorld = {};
	Vector m_TeleportMarkerWorld = {};
	float m_TeleportFacingYawOffset = 0.0f;
	bool m_TeleportFacingTurnPressed = false;
	std::mutex m_TeleportServerMoveMutex;
	Vector m_TeleportPendingServerTarget = {};
	bool m_TeleportPendingServerTargetValid = false;
	Vector m_TeleportPendingVisualWorldDelta = {};
	bool m_TeleportPendingVisualWorldDeltaValid = false;
	Vector m_TeleportPendingVisualLandingTarget = {};
	bool m_TeleportPendingVisualLandingTargetValid = false;
	Vector m_TeleportPendingEngineAnchorWorldDelta = {};
	int m_TeleportEngineAnchorCompensationFrames = 0;
	// Applied after the current HMD local pose has been refreshed. This avoids
	// using the previous tracking sample when recentering the camera on a real
	// teleport landing point.
	Vector m_TeleportPendingCameraPlanarRecenterTarget = {};
	bool m_TeleportPendingCameraPlanarRecenterValid = false;
	int m_TeleportCameraClipSuppressFrames = 0;
	// Hide first-person viewmodels briefly while a teleport camera warp settles.
	// DrawModelExecute may consume one queued frame produced before the new anchor was published.
	std::atomic<uint32_t> m_TeleportViewmodelSuppressUntilMs{ 0 };
	// Remote-server fallback: the rendered camera may scout without moving the
	// authoritative player entity. Teleport may be used repeatedly while detached;
	// a fresh Use press exits scouting and returns to the real body position.
	bool m_TeleportVisualScoutActive = false;
	bool m_TeleportVisualScoutUseExitArmed = false;
	Vector m_TeleportVisualScoutCameraAnchor = {};
	float m_TeleportVisualScoutReturnRotationOffset = 0.0f;
	QAngle m_TeleportVisualScoutBodyViewAngles = {};
	bool m_TeleportVisualScoutBodyViewAnglesValid = false;

	// Debug logging for 1:1 roomscale cmd movement.
	bool m_Roomscale1To1DebugLog = false;
	float m_Roomscale1To1DebugLogHz = 4.0f; // max prints per second; 0 disables throttling
	std::chrono::steady_clock::time_point m_Roomscale1To1DebugLastEncode{};
	bool m_NonVRServerMovementAngleOverride = true;
	// Non-VR server movement: make client-side bullet/muzzle effects originate from controller (visual-only).
	bool m_NonVRServerMovementEffectsFromController = true;
	bool m_NonVRServerMovementEffectsDebugLog = false;
	float m_NonVRServerMovementEffectsDebugLogHz = 4.0f;

	// Non-VR server movement: client-side melee gesture -> IN_ATTACK tuning (ForceNonVRServerMovement=true only)
	// These are intentionally separate from generic MotionGesture* knobs.
	float m_NonVRMeleeSwingThreshold = 999.0f;     // controller linear speed threshold (m/s-ish in tracking space)
	float m_NonVRMeleeSwingCooldown = 0.30f;     // seconds between synthetic swings
	float m_NonVRMeleeHoldTime = 0.06f;          // seconds to hold IN_ATTACK after trigger (reduces "dropped swings")
	float m_NonVRMeleeAttackDelay = 0.04f;     // seconds to wait after gesture before starting IN_ATTACK (adds "wind-up")
	float m_NonVRMeleeAimLockTime = 0.12f;       // seconds to lock viewangles after trigger (stabilizes swing direction)
	float m_NonVRMeleeHysteresis = 0.60f;        // re-arm threshold = threshold * hysteresis
	float m_NonVRMeleeAngVelThreshold = 0.0f;    // deg/s, 0 = disabled (optional wrist-flick trigger)
	float m_NonVRMeleeSwingDirBlend = 0.0f;      // 0..1 blend locked aim toward velocity direction
	bool m_RequireSecondaryAttackForItemSwitch = false;

	// ----------------------------
	// Non-VR server aim consistency (ForceNonVRServerMovement)
	//
	// When playing on a server that does NOT run the VR plugin, the server will
	// calculate bullet traces from the player's eye position using cmd->viewangles.
	// To keep the rendered aim line and actual hit point consistent (both in 1P/3P),
	// we solve an "aim hit point" H each frame:
	//   P = point hit by controller ray (or max distance)
	//   H = point hit by eye ray towards P  (what the server will actually hit)
	//   m_NonVRAimAngles = angles from eye -> H (what we send in cmd->viewangles)
	std::chrono::steady_clock::time_point m_LastNonVRAimSolveTime{};
	float m_NonVRAimSolveMaxHz = 90.0f; // throttle traces; 0 disables throttling
	Vector m_NonVRAimDesiredPoint = { 0.0f, 0.0f, 0.0f }; // P
	Vector m_NonVRAimHitPoint = { 0.0f, 0.0f, 0.0f };     // H
	QAngle m_NonVRAimAngles = { 0.0f, 0.0f, 0.0f };
	bool m_HasNonVRAimSolution = false;

	struct RgbColor
	{
		int r;
		int g;
		int b;
	};

	enum class SpecialInfectedType
	{
		None = -1,
		Boomer,
		Smoker,
		Hunter,
		Spitter,
		Jockey,
		Charger,
		Tank,
		Witch,
		Count
	};

	enum class ItemModelLabelCategory
	{
		None = 0,
		Firearm,
		Melee,
		Throwable,
		MedicalPack,
		Medicine
	};

	struct ItemModelLabelInfo
	{
		ItemModelLabelCategory category = ItemModelLabelCategory::None;
		std::string label;
	};

	struct ProjectedItemLabel
	{
		Vector worldPos = { 0,0,0 };
		Vector stableAnchorWorldPos = { 0,0,0 };
		ItemModelLabelCategory category = ItemModelLabelCategory::None;
		std::string label;
		int sourceEntityIndex = 0;
		std::chrono::steady_clock::time_point lastSeen{};
		std::chrono::steady_clock::time_point stableSince{};
	};

	struct ProjectedViewmodelBoneLabel
	{
		Vector worldPos = { 0,0,0 };
		std::string label;
		bool hasName = false;
		bool highlighted = false;
	};

	struct ProjectedSpecialInfectedArrow
	{
		Vector origin = { 0,0,0 };
		SpecialInfectedType type = SpecialInfectedType::None;
		int sourceEntityIndex = 0;
		std::chrono::steady_clock::time_point lastSeen{};
	};


	struct CachedProjectedItemLabelTexture
	{
		IDirect3DTexture9* texture = nullptr;
		int width = 0;
		int height = 0;
		std::chrono::steady_clock::time_point lastUsed{};
	};

	// Queued rendering publishes the same textured labels used by the single-threaded path,
	// but defers the actual D3D draw until DXVK reaches the safe eye-submit point.
	struct QueuedProjectedItemLabelDraw
	{
		std::string text;
		int screenX = 0;
		int screenY = 0;
		int fontPx = 0;
		int colorR = 255;
		int colorG = 255;
		int colorB = 255;
		int colorA = 255;
	};

	static constexpr int kZombieClassOffset = 0x1c90;
	static constexpr int kLifeStateOffset = 0x147;
	static constexpr int kTeamNumOffset = 0xE4; // DT_BaseEntity::m_iTeamNum
	static constexpr int kObserverModeOffset = 0x1450; // C_BasePlayer::m_iObserverMode
	static constexpr int kObserverTargetOffset = 0x1454; // C_BasePlayer::m_hObserverTarget
	static constexpr int kGroundEntityOffset = 0x13C; // DT_BasePlayer::m_hGroundEntity
	static constexpr int kFlagsOffset = 0xF0; // DT_BasePlayer::m_fFlags
	static constexpr int kVelocityXOffset = 0x100; // DT_BasePlayer::m_vecVelocity[0]
	static constexpr int kVelocityYOffset = 0x104; // DT_BasePlayer::m_vecVelocity[1]
	static constexpr int kVelocityZOffset = 0x108; // DT_BasePlayer::m_vecVelocity[2]
	static constexpr int kOnGroundFlag = 0x1; // FL_ONGROUND

	// Common netvars (from offsets.txt) used by hand HUD overlays
	static constexpr int kHealthOffset = 0xEC;               // DT_BasePlayer::m_iHealth
	static constexpr int kMaxHealthOffset = 0x1FDC;           // DT_TerrorPlayer::m_iMaxHealth (also used by special infected players)
	static constexpr int kAmmoArrayOffset = 0xF24;            // DT_BasePlayer::m_iAmmo (int array)
	static constexpr int kHealthBufferOffset = 0x1FAC;        // DT_TerrorPlayer::m_healthBuffer (temporary HP)
	static constexpr int kHealthBufferTimeOffset = 0x1FB0;    // DT_TerrorPlayer::m_healthBufferTime
	static constexpr int kSurvivorCharacterOffset = 0x1C8C;  // DT_TerrorPlayer::m_survivorCharacter
	static constexpr int kIsOnThirdStrikeOffset = 0x1EC0;     // DT_TerrorPlayer::m_bIsOnThirdStrike
	static constexpr int kIsHangingFromLedgeOffset = 0x25EC;  // DT_TerrorPlayer::m_isHangingFromLedge
	static constexpr int kMissionZombieKillsOffset = 0x24AC;   // DT_TerrorPlayer::m_missionZombieKills[0] (current chapter)
	static constexpr int kCheckpointZombieKillsOffset = 0x2488; // DT_TerrorPlayer::m_checkpointZombieKills[0] (checkpoint stats; some servers only update this)
	static constexpr int kCheckpointHeadshotsOffset = 0x2568;   // DT_TerrorPlayer::m_checkpointHeadshots
	static constexpr int kZombieKillsMaxIndex = 8;             // 0=common, 1..8=special categories (smoker..tank/witch)
	// Weapon netvars (from offsets.txt)
	static constexpr int kClip1Offset = 0x984;                // DT_BaseCombatWeapon::m_iClip1
	static constexpr int kPrimaryAmmoTypeOffset = 0x97C;       // DT_BaseCombatWeapon::m_iPrimaryAmmoType
	static constexpr int kUpgradedPrimaryAmmoLoadedOffset = 0xCB8; // m_nUpgradedPrimaryAmmoLoaded
	static constexpr int kUpgradeBitVecOffset = 0xCF0;         // m_upgradeBitVec (incendiary/explosive/laser bits)


	// Aim-line friendly-fire guard (client-side fire suppression)
	vr::VRActionHandle_t m_ActionFriendlyFireBlockToggle = vr::k_ulInvalidActionHandle;
	bool m_BlockFireOnFriendlyAimEnabled = false; // toggled by SteamVR binding
	bool m_AimLineHitsFriendly = false;           // updated from a ray trace (aim ray)
	// Extra radius (meters) for the friendly-fire aim guard trace.
	// 0 = legacy thin ray; >0 uses a swept hull (fat ray) to reduce misses from spread/latency.
	float m_BlockFireOnFriendlyAimRadiusMeters = 0.05f;


	// Aim-line teammate HUD hint (left wrist HUD):
	// - When the aim line stays on a teammate for >= 2 seconds, show "Name:XX%".
	// - After aim leaves teammates, keep the last shown target for 5 seconds.
	int m_AimTeammateCandidateIndex = -1;
	std::chrono::steady_clock::time_point m_AimTeammateCandidateSince{};
	int m_AimTeammateDisplayIndex = -1;
	std::chrono::steady_clock::time_point m_AimTeammateDisplayUntil{};
	int m_AimTeammateLastRawIndex = -1;
	std::chrono::steady_clock::time_point m_AimTeammateLastRawTime{};

	struct PendingKillSoundHit
	{
		std::uintptr_t entityTag = 0;
		std::chrono::steady_clock::time_point expiresAt{};
		uint32_t shotSerial = 0;
		bool headshot = false;
		Vector impactPos = { 0,0,0 };
	};

	struct PredictedHitFeedbackEmission
	{
		std::uintptr_t entityTag = 0;
		std::uintptr_t weaponTag = 0;
		int clip1 = -2147483647;
		uint32_t shotSerial = 0;
		Vector start = { 0,0,0 };
		Vector dir = { 0,0,0 };
		std::chrono::steady_clock::time_point emittedAt{};
	};

	struct ActiveKillIndicator
	{
		Vector worldPos = { 0,0,0 };
		std::chrono::steady_clock::time_point startedAt{};
		bool killConfirmed = true;
		bool headshot = false;
		int overlaySlot = -1;
	};

	struct KillIndicatorOverlayTexture
	{
		IDirect3DTexture9* d3dTexture = nullptr;
		IDirect3DSurface9* d3dSurface = nullptr;
		SharedTextureHolder sharedTexture{};
		int width = 0;
		int height = 0;
		uint32_t uploadedFrameIndex = UINT32_MAX;
		bool uploadedFromDecodedFrames = false;
	};

	struct KillIndicatorOverlaySlot
	{
		vr::VROverlayHandle_t overlayHandle = vr::k_ulOverlayHandleInvalid;
		int materialIndex = -1;
		bool visible = false;
	};

	struct PendingKillSoundEvent
	{
		std::uintptr_t entityTag = 0;
		bool headshot = false;
		std::chrono::steady_clock::time_point receivedAt{};
	};

	enum class DamageFeedbackType
	{
		CommonHit,
		SpecialHit,
		HeavyHit,
		Explosion,
		Fire,
		Acid
	};

	enum class ControlledDamageState
	{
		None,
		SmokerTongue,
		HunterPounce,
		JockeyRide,
		ChargerCarry,
		ChargerPummel
	};

	struct PendingDamageFeedback
	{
		bool active = false;
		DamageFeedbackType type = DamageFeedbackType::CommonHit;
		ControlledDamageState controlState = ControlledDamageState::None;
		int maxDamage = 0;
		int mergedCount = 0;
		float directionalBiasSum = 0.0f;
		float directionalBiasPeak = 0.0f;
		int directionalSampleCount = 0;
		std::chrono::steady_clock::time_point queuedAt{};
	};

	bool m_HitSoundEnabled = false;
	float m_HitSoundPlaybackCooldownSeconds = 0.03f;
	std::string m_HitSoundSpec = "game:vrmod/hit.wav";
	float m_HitSoundVolume = 1.2f;
	bool m_HitSoundPending = false;
	uint32_t m_HitSoundPendingMergedCount = 0;
	uint32_t m_HitSoundPendingShotSerial = 0;
	std::uintptr_t m_HitSoundPendingEntityTag = 0;
	Vector m_HitSoundPendingWorldPos = { 0,0,0 };
	std::chrono::steady_clock::time_point m_HitSoundPendingQueuedAt{};
	bool m_KillSoundEnabled = false;
	float m_KillSoundDetectionWindowSeconds = 0.75f;
	float m_KillSoundPlaybackCooldownSeconds = 0.04f;
	std::string m_KillSoundNormalSpec = "game:vrmod/kill.wav";
	std::string m_KillSoundHeadshotSpec = "game:vrmod/headshot.wav";
	float m_KillSoundVolume = 1.8f;
	float m_HeadshotSoundVolume = 1.3f;
	float m_FeedbackSoundSpatialBlend = 0.0f;
	float m_FeedbackSoundSpatialRange = 1000.0f;
	int m_FeedbackSoundDebugForceChannel = 0; // -1 = left only, 1 = right only
	bool m_FeedbackSoundDebugLog = false;
	float m_FeedbackSoundDebugLogHz = 1.0f;
	bool m_HitIndicatorEnabled = false;
	bool m_KillIndicatorEnabled = false;
	bool m_KillIndicatorDebugLog = false;
	float m_KillIndicatorDebugLogHz = 1.0f;
	float m_KillIndicatorLifetimeSeconds = 0.8f;
	float m_KillIndicatorSizePixels = 160.0f;
	float m_KillIndicatorRiseUnits = 18.0f;
	float m_KillIndicatorMaxDistance = 4096.0f;
	std::string m_KillIndicatorMaterialBaseSpec = "overlays/2965700751";
	std::vector<PendingKillSoundHit> m_PendingKillSoundHits;
	std::vector<PendingKillSoundEvent> m_PendingKillSoundEvents;
	std::vector<ActiveKillIndicator> m_ActiveKillIndicators;
	int m_LastKillSoundCommonKills = -1;
	int m_LastKillSoundSpecialKills = -1;
	int m_LastKillCounterMissionSum = -1;
	int m_LastKillCounterCheckpointSum = -1;
	char m_KillCounterPreferredSource = 'N';
	float m_PredictedHitFeedbackDedupWindowSeconds = 0.015f;
	Vector m_LastPredictedHitFeedbackStart = { 0,0,0 };
	Vector m_LastPredictedHitFeedbackDir = { 0,0,0 };
	std::uintptr_t m_LastPredictedHitFeedbackEntityTag = 0;
	std::chrono::steady_clock::time_point m_LastPredictedHitFeedbackTime{};
	uint32_t m_PredictedHitFeedbackShotSerial = 0;
	uint32_t m_LastPredictedHitSoundShotSerial = 0;
	int m_CurrentPredictedHitFeedbackCmdNumber = 0;
	std::array<uint32_t, 64> m_RecentPredictedHitFeedbackShotSerials{};
	uint32_t m_RecentPredictedHitFeedbackShotSerialCursor = 0;
	std::array<PredictedHitFeedbackEmission, 32> m_RecentPredictedHitFeedbackEmissions{};
	uint32_t m_RecentPredictedHitFeedbackEmissionCursor = 0;
	std::chrono::steady_clock::time_point m_LastPredictedHitFeedbackShotTime{};
	std::chrono::steady_clock::time_point m_LastHitSoundPlaybackTime{};
	std::chrono::steady_clock::time_point m_LastHitSoundQueueAcceptedTime{};
	Vector m_LastHitSoundQueueAcceptedWorldPos = { 0,0,0 };
	std::uintptr_t m_LastHitSoundQueueAcceptedEntityTag = 0;
	std::array<uint32_t, 64> m_RecentHitSoundPlaybackShotSerials{};
	uint32_t m_RecentHitSoundPlaybackShotSerialCursor = 0;
	std::chrono::steady_clock::time_point m_LastKillSoundPlaybackTime{};
	std::chrono::steady_clock::time_point m_LastKillSoundEventRegisterAttempt{};
	std::chrono::steady_clock::time_point m_LastKillIndicatorTrimTime{};
	std::chrono::steady_clock::time_point m_LastKillIndicatorDebugLogTime{};
	std::chrono::steady_clock::time_point m_LastFeedbackSoundDebugLogTime{};
	uint32_t m_KillIndicatorStatsHitSpawned = 0;
	uint32_t m_KillIndicatorStatsKillSpawned = 0;
	uint32_t m_KillIndicatorStatsHitMerged = 0;
	uint32_t m_KillIndicatorStatsRecycled = 0;
	uint32_t m_KillIndicatorStatsTrimmed = 0;
	uint32_t m_KillIndicatorStatsPeakActive = 0;
	uint32_t m_HitSoundStatsQueued = 0;
	uint32_t m_HitSoundStatsMerged = 0;
	uint32_t m_HitSoundStatsFlushed = 0;
	struct FeedbackSoundWorkerJob
	{
		enum class Type
		{
			PlayFile,
			WarmupFile,
			ResetState
		};

		Type type = Type::PlayFile;
		std::string resolvedPath;
		int leftVolume = 1000;
		int rightVolume = 1000;
		bool preferLoadedPathReuse = true;
	};
	std::mutex m_FeedbackSoundWorkerMutex{};
	std::condition_variable m_FeedbackSoundWorkerCv{};
	std::deque<FeedbackSoundWorkerJob> m_FeedbackSoundWorkerJobs;
	std::atomic<bool> m_FeedbackSoundWorkerStarted{ false };
	std::string m_FeedbackSoundWarmupSignature;
	struct TextToSpeechRuntimeConfig
	{
		std::string commandPrefixSpec;
		std::string modelSpec;
		std::string workingDirSpec;
		int serverPort = 9880;
		std::string referenceAudioSpec;
		std::string promptText;
		std::string promptLanguage;
		std::string textLanguage;
		std::string textSplitMethod;
		float volume = 1.0f;
		bool includeSpeakerName = true;
		std::string resolvedPrefix;
		std::string resolvedWorkingDir;
		std::string resolvedConfigPath;
		std::string resolvedLaunchConfigPath;
		std::string resolvedReferenceAudioPath;
		std::string resolvedDevice;
		bool resolvedIsHalf = false;
		std::string resolvedBertBasePath;
		std::string resolvedCnHubertBasePath;
		std::string resolvedT2SWeightsPath;
		std::string resolvedVitsWeightsPath;
		std::string resolvedModelVersion;
		bool hotSwitchProfileValid = false;
	};
	struct SpeechWorkerJob
	{
		enum class Type
		{
			TranscribeWave,
			SpeakText
		};

		Type type = Type::TranscribeWave;
		std::string inputPath;
		std::string outputPath;
		std::string speaker;
		std::string text;
		bool allowWhenTextToSpeechDisabled = false;
		bool includeSpeakerName = true;
		bool playLocally = true;
		bool sendToVoiceChat = false;
		bool useSpeechToTextSendVoiceProfile = false;
	};
	struct PendingSpeechVoiceBroadcast
	{
		std::string wavPath;
	};
	struct SpeechCaptureBuffer
	{
		WAVEHDR header{};
		std::array<char, 4096> bytes{};
		bool prepared = false;
	};
	std::mutex m_SpeechWorkerMutex{};
	std::condition_variable m_SpeechWorkerCv{};
	std::deque<SpeechWorkerJob> m_SpeechWorkerJobs;
	std::atomic<bool> m_SpeechWorkerStarted{ false };
	std::mutex m_SpeechResultMutex{};
	std::deque<std::string> m_PendingSpeechToTextChatMessages;
	std::mutex m_SpeechVoiceBroadcastMutex{};
	std::deque<PendingSpeechVoiceBroadcast> m_PendingSpeechVoiceBroadcasts;
	std::mutex m_SpeechCaptureMutex{};
	HWAVEIN m_SpeechCaptureWaveIn = nullptr;
	std::array<SpeechCaptureBuffer, 4> m_SpeechCaptureBuffers{};
	std::vector<int16_t> m_SpeechCapturePcm;
	bool m_SpeechCaptureStopping = false;
	std::chrono::steady_clock::time_point m_SpeechCaptureStartedAt{};
	bool m_SpeechVoiceBroadcastActive = false;
	bool m_AutoVoiceRecordRequested = false;
	bool m_SpeechVoiceLoopbackCmdOwned = false;
	std::chrono::steady_clock::time_point m_SpeechVoiceBroadcastStopAt{};
	uint64_t m_SpeechTempSerial = 1;
	std::mutex m_TextToSpeechServerMutex{};
	HANDLE m_TextToSpeechServerProcess = nullptr;
	HANDLE m_TextToSpeechServerJob = nullptr;
	DWORD m_TextToSpeechServerProcessId = 0;
	std::string m_TextToSpeechServerLaunchSignature;
	std::string m_TextToSpeechServerModelSignature;
	IMaterial* m_KillIndicatorHitMaterial = nullptr;
	IMaterial* m_KillIndicatorNormalMaterial = nullptr;
	IMaterial* m_KillIndicatorHeadshotMaterial = nullptr;
	std::array<KillIndicatorOverlayTexture, 3> m_KillIndicatorOverlayTextures{};
	std::array<KillIndicatorOverlaySlot, 16> m_KillIndicatorOverlaySlots{};
	uint64_t m_NextKillIndicatorOverlaySerial = 1;
	IGameEventManager2* m_KillSoundEventManager = nullptr;
	IGameEventListener2* m_KillSoundEventListener = nullptr;
	bool m_KillSoundEventListenerRegistered = false;
	IGameEventManager2* m_MeleeHitHapticsEventManager = nullptr;
	IGameEventListener2* m_MeleeHitHapticsEventListener = nullptr;
	bool m_MeleeHitHapticsEventListenerRegistered = false;
	IGameEventManager2* m_DamageFeedbackEventManager = nullptr;
	IGameEventListener2* m_DamageFeedbackEventListener = nullptr;
	bool m_DamageFeedbackEventListenerRegistered = false;
	bool m_DamageFeedbackEnabled = false;
	bool m_DamageDirectionalEnabled = false;
	bool m_DamageSustainEnabled = false;
	bool m_LandingHapticsEnabled = true;
	bool m_CameraShakeHapticsEnabled = false;
	WeaponHapticsProfile m_DamageCommonHapticsProfile = { 0.016f, 135.0f, 0.24f };
	WeaponHapticsProfile m_DamageSpecialHapticsProfile = { 0.020f, 112.0f, 0.38f };
	WeaponHapticsProfile m_DamageHeavyHapticsProfile = { 0.030f, 82.0f, 0.62f };
	WeaponHapticsProfile m_DamageExplosionHapticsProfile = { 0.036f, 72.0f, 0.74f };
	WeaponHapticsProfile m_DamageFireHapticsProfile = { 0.018f, 106.0f, 0.28f };
	WeaponHapticsProfile m_DamageAcidHapticsProfile = { 0.014f, 156.0f, 0.32f };
	WeaponHapticsProfile m_DamageSmokerControlHapticsProfile = { 0.020f, 96.0f, 0.30f };
	WeaponHapticsProfile m_DamageHunterControlHapticsProfile = { 0.018f, 138.0f, 0.36f };
	WeaponHapticsProfile m_DamageJockeyControlHapticsProfile = { 0.016f, 152.0f, 0.30f };
	WeaponHapticsProfile m_DamageChargerCarryHapticsProfile = { 0.028f, 88.0f, 0.52f };
	WeaponHapticsProfile m_DamageChargerPummelHapticsProfile = { 0.044f, 66.0f, 0.82f };
	float m_DamageSmokerControlDirectionalBiasScale = 1.00f;
	float m_DamageHunterControlDirectionalBiasScale = 0.85f;
	float m_DamageJockeyControlDirectionalBiasScale = 0.75f;
	float m_DamageChargerCarryDirectionalBiasScale = 0.65f;
	float m_DamageChargerPummelDirectionalBiasScale = 0.55f;
	float m_DamageScaleStart = 6.0f;
	float m_DamageScalePerPoint = 0.008f;
	float m_DamageScaleMaxBonus = 0.24f;
	float m_DamageAmplitudeMin = 0.05f;
	float m_DamageAmplitudeMax = 1.0f;
	float m_DamageFeedbackOverallScale = 1.0f;
	float m_DamageFireSustainSeconds = 1.4f;
	float m_DamageAcidSustainSeconds = 1.6f;
	WeaponHapticsProfile m_DamageFireSustainPulse = { 0.016f, 110.0f, 0.20f };
	WeaponHapticsProfile m_DamageAcidSustainPulse = { 0.010f, 170.0f, 0.24f };
	float m_DamageFireSustainIntervalSeconds = 0.11f;
	float m_DamageAcidSustainIntervalSeconds = 0.08f;
	std::chrono::steady_clock::time_point m_LastDamageFeedbackEventRegisterAttempt{};
	std::chrono::steady_clock::time_point m_LastDamageFeedbackEventSeenTime{};
	std::chrono::steady_clock::time_point m_LastMeleeHitHapticsEventRegisterAttempt{};
	std::chrono::steady_clock::time_point m_LastMeleeHitHapticsTriggerTime{};
	std::uintptr_t m_LastMeleeHitHapticsEntityTag = 0;
	PendingDamageFeedback m_PendingDamageFeedback{};
	std::chrono::steady_clock::time_point m_LastDamageFeedbackTriggerTime{};
	int m_LastObservedDamageHealth = -1;
	float m_DamageFeedbackMergeWindowSeconds = 0.045f;
	float m_DamageFeedbackMinTriggerIntervalSeconds = 0.030f;
	float m_DamageFeedbackMergedHitBonus = 0.04f;
	std::chrono::steady_clock::time_point m_AcidSustainUntil{};
	std::chrono::steady_clock::time_point m_FireSustainUntil{};
	std::chrono::steady_clock::time_point m_NextAcidSustainPulse{};
	std::chrono::steady_clock::time_point m_NextFireSustainPulse{};
	std::chrono::steady_clock::time_point m_LastCameraShakeHapticsPulse{};
	std::chrono::steady_clock::time_point m_LandingAirborneSince{};
	bool m_WasOnGroundForHaptics = true;
	float m_LastVerticalSpeedForHaptics = 0.0f;
	float m_LandingPeakDownwardSpeedForHaptics = 0.0f;
	int m_LastAirborneHealthForHaptics = -1;
	float m_LandingMinAirTime = 0.08f;
	float m_LandingMinDownwardSpeed = 140.0f;
	WeaponHapticsProfile m_LandingMediumHapticsProfile = { 0.028f, 92.0f, 0.52f };
	WeaponHapticsProfile m_LandingDamageHapticsProfile = { 0.042f, 72.0f, 0.88f };
	bool m_CameraShakeStateInitialized = false;
	Vector m_LastCameraShakeOrigin = { 0,0,0 };
	QAngle m_LastCameraShakeAngles = { 0,0,0 };
	float m_CameraShakeAngleThreshold = 5.0f;
	float m_CameraShakeAngleRange = 18.0f;
	float m_CameraShakePosThreshold = 8.0f;
	float m_CameraShakePosRange = 64.0f;
	float m_CameraShakeHmdAngVelMax = 120.0f;
	float m_CameraShakePulseIntervalSeconds = 0.12f;
	float m_CameraShakePulseAmpMin = 0.12f;
	float m_CameraShakePulseAmpMax = 0.46f;
	float m_CameraShakePulseFreqMin = 80.0f;
	float m_CameraShakePulseFreqMax = 100.0f;
	float m_CameraShakePulseDurMin = 0.012f;
	float m_CameraShakePulseDurMax = 0.028f;

	// Right ammo HUD: show *aimed* special-infected (and Witch) HP%% (client-side, visual-only).
	// - Updated from the aim-ray trace (same trace used for the teammate aim hint).
	// - Hidden immediately when the aim ray leaves the target.
	std::atomic<long long> m_HudAimTargetTimeTicks{ 0 }; // optional: for debugging/telemetry
	std::atomic<std::uintptr_t> m_HudAimTargetTag{ 0 };
	std::atomic<int> m_HudAimTargetMaxHealth{ 0 };
	std::atomic<int> m_HudAimTargetPct{ 0 };

	inline void ClearAmmoHudAimTarget()
	{
		m_HudAimTargetTimeTicks.store(0, std::memory_order_relaxed);
		m_HudAimTargetTag.store(0, std::memory_order_relaxed);
		m_HudAimTargetMaxHealth.store(0, std::memory_order_relaxed);
		m_HudAimTargetPct.store(0, std::memory_order_relaxed);
	}

	inline void NotifyAmmoHudAimTarget(std::uintptr_t tag, int hp, int maxHealthCandidate)
	{
		if (tag == 0 || hp <= 0)
		{
			ClearAmmoHudAimTarget();
			return;
		}

		// Store timestamp (steady_clock ticks)
		const auto now = std::chrono::steady_clock::now();
		m_HudAimTargetTimeTicks.store((long long)now.time_since_epoch().count(), std::memory_order_relaxed);

		const std::uintptr_t prevTag = m_HudAimTargetTag.load(std::memory_order_relaxed);
		int storedMax = (prevTag == tag) ? m_HudAimTargetMaxHealth.load(std::memory_order_relaxed) : 0;

		int maxHp = maxHealthCandidate;
		// Sanity clamp: avoid bogus reads for non-player NPCs.
		if (maxHp <= 0 || maxHp > 20000)
			maxHp = 0;
		if (storedMax > 0)
			maxHp = (maxHp > 0) ? std::max(maxHp, storedMax) : storedMax;
		if (maxHp <= 0)
			maxHp = std::max(1, hp);
		else
			maxHp = std::max(maxHp, hp);

		m_HudAimTargetTag.store(tag, std::memory_order_relaxed);
		m_HudAimTargetMaxHealth.store(maxHp, std::memory_order_relaxed);

		const long long num = (long long)hp * 100LL + (long long)maxHp / 2LL;
		int pct = (int)(num / (long long)maxHp);
		if (pct < 0) pct = 0;
		if (pct > 100) pct = 100;
		if (hp > 0 && pct == 0) pct = 1;
		m_HudAimTargetPct.store(pct, std::memory_order_relaxed);
	}

	// Back-compat: old name used by previous hit-based implementation.
	inline void NotifyAmmoHudHitTarget(std::uintptr_t tag, int hp, int maxHealthCandidate)
	{
		NotifyAmmoHudAimTarget(tag, hp, maxHealthCandidate);
	}


	std::chrono::steady_clock::time_point m_LastFriendlyFireGuardLogTime{};
	// Latch suppression while attack is held (prevents flicker causing intermittent firing).
	bool m_FriendlyFireGuardLatched = false;

	// Auto-repeat semi-auto / single-shot guns while holding IN_ATTACK (client-side input pulse).
	// Config: AutoRepeatSemiAutoFire (true/false)
	//         AutoRepeatSemiAutoFireHz (float, pulses per second; 0 disables)
	bool m_AutoRepeatSemiAutoFire = false;
	float m_AutoRepeatSemiAutoFireHz = 12.0f;
	bool m_AutoRepeatHoldPrev = false;
	std::chrono::steady_clock::time_point m_AutoRepeatNextPulse{};
	// Pump/chrome shotgun + AWP/scout spray-push while auto-repeat is active.
	// Config:
	//   AutoRepeatSprayPushEnabled
	//   AutoRepeatSprayPushDelayTicks
	//   AutoRepeatSprayPushHoldTicks
	bool m_AutoRepeatSprayPushEnabled = false;
	int m_AutoRepeatSprayPushDelayTicks = 0;
	int m_AutoRepeatSprayPushHoldTicks = 1;

	// Auto fast-melee (client-side hold-to-pulse + optional weapon-switch cancel).
	// Config:
	//   AutoFastMelee
	//   AutoFastMeleeUseWeaponSwitch
	bool m_AutoFastMelee = false;
	bool m_AutoFastMeleeUseWeaponSwitch = true;

	// Auto ResetPosition after a level finishes loading.
	// Config: AutoResetPositionAfterLoadSeconds (0 disables)
	float m_AutoResetPositionAfterLoadSeconds = 5.0f;
	bool m_AutoResetPositionPending = false;
	std::chrono::steady_clock::time_point m_AutoResetPositionDueTime{};
	bool m_AutoResetHadLocalPlayerPrev = false;
	// Spectator/observer camera behavior.
	// When enabled, we try to switch the engine spectator camera to free-roaming
	// (spec_mode 6) once per observer session, instead of default chase cam locked
	// to a teammate.
	// Config: ObserverDefaultFreeCam (true/false)
	bool m_ObserverDefaultFreeCam = true;
	bool m_ObserverWasActivePrev = false;
	bool m_ObserverForcedFreeCamThisObserver = false;
	// Some servers place the client into observer mode immediately on join, but
	// early spec_mode commands can be ignored during connection/spawn. We retry a
	// few times until the engine actually reports roaming (mode 6), then stop so
	// the user can still manually change spectator mode afterwards.
	std::chrono::steady_clock::time_point m_ObserverLastFreeCamAttempt{};
	int m_ObserverFreeCamAttemptCount = 0;
	// Observer in-eye (obsMode 4) target switch: auto ResetPosition to re-align anchors.
	int m_ObserverInEyeTargetPrev = 0;
	bool m_ObserverInEyeWasActivePrev = false;
	bool m_ResetPositionAfterObserverTargetSwitchPending = false;
	// CTerrorPlayer netvars (from offsets.txt). These are used for a special-case in the
	// friendly-fire aim guard: if the aim ray hits a teammate who is currently pinned/
	// controlled, we allow a "see-through" trace to hit the attacker behind them.
	// NOTE: These offsets can change between game builds.
	static constexpr int kIsIncapacitatedOffset = 0x1EA9; // DT_TerrorPlayer::m_isIncapacitated
	static constexpr int kTongueOwnerOffset = 0x1F6C; // DT_TerrorPlayer::m_tongueOwner
	static constexpr int kPummelAttackerOffset = 0x2720; // DT_TerrorPlayer::m_pummelAttacker
	static constexpr int kCarryAttackerOffset = 0x2714; // DT_TerrorPlayer::m_carryAttacker
	static constexpr int kPounceAttackerOffset = 0x272C; // DT_TerrorPlayer::m_pounceAttacker
	static constexpr int kJockeyAttackerOffset = 0x274C; // DT_TerrorPlayer::m_jockeyAttacker
	// Mounted gun (fixed machine-gun / turret) support:
	// When the player is using a mounted gun, their aim ray can intersect the gun/base itself,
	// causing the aim line + third-person convergence point to jitter wildly.
	// We treat the mounted-gun entity as "transparent" for aim traces by skipping the current use-entity.
	// NOTE: These offsets can change between game builds.
	static constexpr int kUsingMountedGunOffset = 0x1EBA;  // DT_TerrorPlayer::m_usingMountedGun
	static constexpr int kUsingMountedWeaponOffset = 0x1EBB;  // DT_TerrorPlayer::m_usingMountedWeapon (minigun/gatling uses this)
	static constexpr int kUseEntityHandleOffset = 0x1480;  // DT_BasePlayer::m_hUseEntity
	bool m_SpecialInfectedArrowEnabled = false;
	bool m_SpecialInfectedDebug = false;
	bool m_SpecialInfectedArrowDebugLog = false;
	float m_SpecialInfectedArrowDebugLogHz = 2.0f;
	float m_SpecialInfectedArrowSize = 12.0f;
	float m_SpecialInfectedArrowHeight = 36.0f;
	float m_SpecialInfectedArrowThickness = 0.0f;
	RgbColor m_SpecialInfectedArrowDefaultColor{ 255, 64, 0 };
	std::array<RgbColor, static_cast<size_t>(SpecialInfectedType::Count)> m_SpecialInfectedArrowColors{
		RgbColor{ 120, 220, 80 },   // Boomer
		RgbColor{ 180, 80, 255 },   // Smoker
		RgbColor{ 0, 170, 255 },    // Hunter
		RgbColor{ 60, 220, 120 },   // Spitter
		RgbColor{ 255, 140, 20 },   // Jockey
		RgbColor{ 0, 200, 200 },    // Charger
		RgbColor{ 240, 40, 40 },    // Tank
		RgbColor{ 255, 255, 255 }   // Witch
	};
	float m_SpecialInfectedBlindSpotDistance = 105.0f;
	float m_SpecialInfectedBlindSpotWarningDuration = 0.5f;
	bool m_SpecialInfectedBlindSpotWarningActive = false;
	std::chrono::steady_clock::time_point m_LastSpecialInfectedWarningTime{};
	// Special infected intent sense: warn when a special infected targets or clearly looks at the local player.
	// Uses CTerrorPlayer handle netvars from offsets.txt first, then an optional angle/LOS fallback.
	static constexpr int kSpecialIntentTongueVictimOffset = 0x1F68;
	static constexpr int kSpecialIntentPounceVictimOffset = 0x2728;
	static constexpr int kSpecialIntentJockeyVictimOffset = 0x2748;
	static constexpr int kSpecialIntentCarryVictimOffset = 0x2718;
	static constexpr int kSpecialIntentPummelVictimOffset = 0x271C;
	static constexpr int kSpecialIntentQueuedPummelVictimOffset = 0x2724;
	static constexpr int kSpecialIntentLookatPlayerOffset = 0x2924;
	static constexpr int kSpecialIntentAngRotationOffset = 0x118;
	bool m_SpecialInfectedIntentSenseEnabled = false;
	bool m_SpecialInfectedIntentSenseHudEnabled = true;
	bool m_SpecialInfectedIntentSenseHapticsEnabled = true;
	bool m_SpecialInfectedIntentSenseUseLookFallback = true;
	// If false, suppress front/front-left/front-right alerts except selected high-priority intent threats.
	bool m_SpecialInfectedIntentSenseWarnFront = false;
	float m_SpecialInfectedIntentSenseDistance = 1200.0f;
	float m_SpecialInfectedIntentSenseLookDot = 0.88f;
	float m_SpecialInfectedIntentSenseCooldownSeconds = 1.25f;
	float m_SpecialInfectedIntentSenseMaxContinuousVisibleSeconds = 30.0f;
	float m_SpecialInfectedIntentSenseHudWidthMeters = 0.46f;
	float m_SpecialInfectedIntentSenseHudMarginXMeters = 0.025f;
	float m_SpecialInfectedIntentSenseHudMarginYMeters = 0.025f;
	int m_SpecialInfectedIntentSenseHudMaxLines = 4;
	float m_SpecialInfectedIntentSenseHapticDuration = 0.045f;
	float m_SpecialInfectedIntentSenseHapticFrequency = 120.0f;
	float m_SpecialInfectedIntentSenseHapticAmplitude = 0.38f;
	std::chrono::steady_clock::time_point m_LastSpecialInfectedIntentSenseAlertTime{};
	std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastSpecialInfectedIntentSenseEntityAlertTime{};
	struct SpecialInfectedIntentSenseThreat
	{
		SpecialInfectedType type = SpecialInfectedType::None;
		std::string directionText;
		float distanceMeters = 0.0f;
		float rightBias = 0.0f;
		bool front = false;
		uint32_t scanRevision = 0;
		std::chrono::steady_clock::time_point firstSeen{};
		std::chrono::steady_clock::time_point lastSeen{};
		Vector lastOrigin{ 0.0f, 0.0f, 0.0f };
		std::chrono::steady_clock::time_point stationarySince{};
		bool hasLastOrigin = false;
		bool suppressedByLongTrack = false;
		bool suppressedByStationaryOrigin = false;
	};
	// Intent-sense HUD text is rendered through a standalone OpenVR overlay.
	// Do not use engine DebugOverlay screen text here: that call path can crash on L4D2's VDebugOverlay build.
	struct SpecialInfectedIntentSenseHudLine
	{
		std::string text;
		RgbColor color{ 255, 238, 218 };
	};
	std::mutex m_SpecialInfectedIntentSenseHudMutex{};
	std::unordered_map<int, SpecialInfectedIntentSenseThreat> m_SpecialInfectedIntentSenseThreats{};
	std::vector<SpecialInfectedIntentSenseHudLine> m_SpecialInfectedIntentSenseHudLines{};
	uint32_t m_SpecialInfectedIntentSenseScanRevision = 0;
	int m_SpecialInfectedIntentSenseHudRevision = 0;
	struct SpecialInfectedDodgeThreat
	{
		Vector origin{ 0.0f, 0.0f, 0.0f };
		SpecialInfectedType type = SpecialInfectedType::None;
		int entityIndex = -1;
		float distanceSq = 0.0f;
	};
	bool m_SpecialInfectedDodgeActive = false;
	float m_SpecialInfectedDodgeDistance = 260.0f;
	mutable std::mutex m_SpecialInfectedDodgeMutex{};
	std::vector<SpecialInfectedDodgeThreat> m_SpecialInfectedDodgeThreats{};
	std::chrono::steady_clock::time_point m_LastSpecialInfectedDodgeScanTime{};
	float m_SpecialInfectedPreWarningEvadeDistance = 260.0f;
	float m_SpecialInfectedPreWarningEvadeCooldown = 0.85f;
	bool m_SpecialInfectedAutoEvadeIgnoreBehind = true;
	int m_LastSpecialInfectedEvadeEntityIndex = -1;
	bool m_SpecialInfectedPreWarningEvadeArmed = true;
	std::chrono::steady_clock::time_point m_SpecialInfectedPreWarningEvadeCooldownEnd{};
	bool m_SpecialInfectedPreWarningEvadeTriggered = false;
	float m_SpecialInfectedWarningSecondaryHoldDuration = 0.15f;
	float m_SpecialInfectedWarningPostAttackDelay = 0.1f;
	float m_SpecialInfectedWarningJumpHoldDuration = 0.2f;
	bool m_SpecialInfectedWarningActionEnabled = false;
	float m_SpecialInfectedPreWarningDistance = 750.0f;
	float m_SpecialInfectedPreWarningTargetUpdateInterval = 0.1f;
	float m_SpecialInfectedPreWarningAimAngle = 5.0f;
	float m_SpecialInfectedPreWarningAimSnapDistance = 18.0f;
	float m_SpecialInfectedPreWarningAimReleaseDistance = 28.0f;
	bool m_SpecialInfectedPreWarningAutoAimConfigEnabled = false;
	bool m_SpecialInfectedPreWarningAutoAimEnabled = false;
	bool m_SpecialInfectedPreWarningActive = false;
	bool m_SpecialInfectedPreWarningInRange = false;
	Vector m_SpecialInfectedPreWarningTarget = { 0.0f, 0.0f, 0.0f };
	int m_SpecialInfectedPreWarningTargetEntityIndex = -1;
	bool m_SpecialInfectedPreWarningTargetIsPlayer = false;
	float m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
	Vector m_SpecialInfectedAutoAimDirection = { 0.0f, 0.0f, 0.0f };
	float m_SpecialInfectedAutoAimLerp = 0.4f;
	float m_SpecialInfectedAutoAimCooldown = 0.5f;
	std::chrono::steady_clock::time_point m_SpecialInfectedAutoAimCooldownEnd{};
	std::array<Vector, static_cast<size_t>(SpecialInfectedType::Count)> m_SpecialInfectedPreWarningAimOffsets{
		Vector{ 0.0f, 0.0f, 0.0f }, // Boomer
		Vector{ 0.0f, 0.0f, 0.0f }, // Smoker
		Vector{ 0.0f, 0.0f, 0.0f }, // Hunter
		Vector{ 0.0f, 0.0f, 0.0f }, // Spitter
		Vector{ 0.0f, 0.0f, 0.0f }, // Jockey
		Vector{ 0.0f, 0.0f, 0.0f }, // Charger
		Vector{ 0.0f, 0.0f, 0.0f }, // Tank
		Vector{ 0.0f, 0.0f, 0.0f }  // Witch
	};
	std::chrono::steady_clock::time_point m_LastSpecialInfectedPreWarningSeenTime{};
	std::chrono::steady_clock::time_point m_LastSpecialInfectedPreWarningTargetUpdateTime{};
	Vector m_SpecialInfectedWarningTarget = { 0.0f, 0.0f, 0.0f };
	bool m_SpecialInfectedWarningTargetActive = false;
	bool m_SuppressPlayerInput = false;
	enum class SpecialInfectedWarningActionStep
	{
		None,
		PressSecondaryAttack,
		ReleaseSecondaryAttack,
		PressJump,
		ReleaseJump
	};
	SpecialInfectedWarningActionStep m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::None;
	bool m_SpecialInfectedWarningAttack2CmdOwned = false;
	bool m_SpecialInfectedWarningJumpCmdOwned = false;
	std::chrono::steady_clock::time_point m_SpecialInfectedWarningNextActionTime{};
	bool m_ItemModelLabelEnabled = false;
	bool m_ItemModelLabelShowWeapons = true;
	bool m_ItemModelLabelShowThrowables = true;
	bool m_ItemModelLabelShowMedical = true;
	bool m_ItemModelLabelDebugLog = false;
	// Comma-separated exact display-text blacklist parsed from ItemModelLabelBlacklist.
	// Tokens are trimmed and compared case-insensitively for ASCII text.
	std::unordered_set<std::string> m_ItemModelLabelBlacklist{};
	float m_ItemModelLabelMaxHz = 60.0f;
	float m_ItemModelLabelScanHz = 6.0f;
	float m_ItemModelLabelTextScale = 1.0f;
	float m_ItemModelLabelMaxDistance = 4096.0f;
	int m_ItemModelLabelMaxVisiblePerEye = 18;
	float m_ItemModelLabelPlayerSuppressRadius = 96.0f;
	float m_ItemModelLabelPlayerSuppressMinZ = -32.0f;
	float m_ItemModelLabelPlayerSuppressMaxZ = 128.0f;
	mutable std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastItemModelLabelTime{};
	std::unordered_map<int, ProjectedItemLabel> m_ProjectedItemLabels{};
	std::unordered_map<std::string, CachedProjectedItemLabelTexture> m_ItemLabelTextureCache{};
	mutable std::recursive_mutex m_ItemLabelTextureCacheMutex{};
	mutable std::mutex m_QueuedProjectedItemLabelMutex{};
	std::array<std::vector<QueuedProjectedItemLabelDraw>, 2> m_QueuedProjectedItemLabelEyes{};
	mutable std::mutex m_ViewmodelBoneLabelMutex{};
	std::vector<ProjectedViewmodelBoneLabel> m_ViewmodelBoneLabels{};
	mutable std::mutex m_ProjectedSpecialInfectedArrowMutex;
	std::unordered_map<int, ProjectedSpecialInfectedArrow> m_ProjectedSpecialInfectedArrows{};
	int m_AimLineWarningColorR = 255;
	int m_AimLineWarningColorG = 255;
	int m_AimLineWarningColorB = 0;

	// ----------------------------
	// Gun-mounted scope (RTT overlay)
	// ----------------------------
	bool  m_ScopeEnabled = false;
	int   m_ScopeRTTSize = 512;               // square RTT size in pixels
	std::chrono::steady_clock::time_point m_LastScopeRTTRenderTime{};
	float m_ScopeFov = 20.0f;                  // active per-weapon FOV; smaller = more zoom
	float m_ScopeFovMin = 3.0f;                // lower clamp for per-weapon magnification adjustment
	float m_ScopeFovMax = 20.0f;               // upper clamp for per-weapon magnification adjustment
	float m_ScopeFovAdjustSpeed = 18.0f;       // degrees per second at full stick deflection
	float m_ScopeAimSensitivityFovReductionRate = 0.75f;
	bool  m_ScopeFovAdjustSuppressWalk = false;
	bool  m_ScopeFovAdjustingThisFrame = false;
	bool  m_ScopeAnalogAdjustActive = false;
	float m_ScopeZNear = 2.0f;                 // game units

	// Scope camera pose relative to gun hand (game units, in controller basis fwd/right/up)
	Vector m_ScopeCameraOffset = { 12.0f, 0.0f, 3.0f };
	QAngle m_ScopeCameraAngleOffset = { 0.0f, 0.0f, 0.0f };

	// Overlay placement relative to tracked device (meters, in controller local space)
	float  m_ScopeOverlayWidthMeters = 0.3f;
	float  m_ScopeOverlayXOffset = 0.02f;
	float  m_ScopeOverlayYOffset = 0.04f;
	float  m_ScopeOverlayZOffset = -0.06f;
	QAngle m_ScopeOverlayAngleOffset = { -45.0f, -5.0f, -5.0f };
	// If true, when scoped-in the aim line is rendered only during the scope RTT pass.
	bool  m_ScopeAimLineOnlyInScope = true;
	// Alpha scale for the scope lens reticle drawn into the RTT. 0 = hidden, 1 = default opacity.
	float m_ScopeReticleAlpha = 1.0f;
	std::atomic<uint32_t> m_QueuedScopeLensPostProcessPending{ 0 };
	std::atomic<uint32_t> m_ScopeLensOverlayReady{ 0 };
	std::chrono::steady_clock::time_point m_LastScopeLensPostProcessTime{};
	// If true, hide the local player model while rendering scope RTT (prevents head/body obstruction).
	bool  m_ScopeHideLocalPlayerModelInScope = true;

	// Look-through activation (HMD -> scope camera)
	bool  m_ScopeRequireLookThrough = true;
	float m_ScopeLookThroughDistanceMeters = 0.5f;
	float m_ScopeLookThroughAngleDeg = 60.0f;
	bool  m_ScopeOverlayAlwaysVisible = false;
	float m_ScopeOverlayIdleAlpha = 0.5f;
	// Scope stabilization (visual only): smooth the scope RTT camera pose when scoped-in.
	// This reduces high-magnification jitter without changing shooting / aim direction.
	bool  m_ScopeStabilizationEnabled = true;
	float m_ScopeStabilizationMinCutoff = 0.5f;  // Hz (lower = smoother, more latency)
	float m_ScopeStabilizationBeta = 0.5f;      // responsiveness to fast motion
	float m_ScopeStabilizationDCutoff = 1.0f;    // Hz (derivative low-pass cutoff)

	// Scoped aim sensitivity scaling is derived from the current realtime scope FOV.
	// At ScopeFovMax the gain is 1.0; as FOV decreases, aim sensitivity decreases.
	bool   m_ScopeAimSensitivityInit = false;
	QAngle m_ScopeAimSensitivityBaseAng = { 0.0f, 0.0f, 0.0f };

	// Runtime state
	Vector m_ScopeCameraPosAbs = { 0.0f, 0.0f, 0.0f };
	QAngle m_ScopeCameraAngAbs = { 0.0f, 0.0f, 0.0f };
	bool   m_ScopeActive = false;
	bool   m_ScopeToggleActive = false;

	bool   m_ScopeWeaponIsFirearm = false;

	// Scope stabilization filter state (One Euro filter)
	bool   m_ScopeStabilizationInit = false;
	Vector m_ScopeStabPos = { 0.0f, 0.0f, 0.0f };
	Vector m_ScopeStabPosDeriv = { 0.0f, 0.0f, 0.0f };
	QAngle m_ScopeStabAng = { 0.0f, 0.0f, 0.0f };
	QAngle m_ScopeStabAngDeriv = { 0.0f, 0.0f, 0.0f };
	std::chrono::steady_clock::time_point m_ScopeStabilizationLastTime{};

	Vector GetScopeCameraAbsPos() const { return m_ScopeCameraPosAbs; }
	QAngle GetScopeCameraAbsAngle() const { return m_ScopeCameraAngAbs; }
	bool   IsMouseModeScopeActive() const { return m_MouseModeEnabled && m_ScopeEnabled && m_ScopeWeaponIsFirearm && m_MouseModeScopeToggleActive; }
	bool   IsSteamVRScopeToggleActive() const { return m_ScopeEnabled && m_ScopeWeaponIsFirearm && m_ScopeToggleActive; }
	float  GetScopeAimSensitivityScale() const
	{
		if (!IsScopeActive())
			return 1.0f;

		const float maxFov = std::max(m_ScopeFovMin, m_ScopeFovMax);
		if (!(maxFov > 0.01f) || !(m_ScopeAimSensitivityFovReductionRate > 0.0f))
			return 1.0f;

		const float ratio = std::clamp(m_ScopeFov / maxFov, 0.01f, 1.0f);
		return std::clamp(std::pow(ratio, m_ScopeAimSensitivityFovReductionRate), 0.05f, 1.0f);
	}
	float  GetMouseModeScopeSensitivityScale() const
	{
		return IsMouseModeScopeActive() ? GetScopeAimSensitivityScale() : 1.0f;
	}
	bool   IsScopeActive() const { return m_ScopeEnabled && (m_ScopeActive || IsMouseModeScopeActive() || IsSteamVRScopeToggleActive()); }
	bool   ShouldRenderScope() const
	{
		const bool forceScopeForThirdPersonFrontView = m_ThirdPersonFrontViewEnabled && m_IsThirdPersonCamera;
		return m_ScopeEnabled
			&& (m_ScopeWeaponIsFirearm || forceScopeForThirdPersonFrontView)
			&& (m_ScopeOverlayAlwaysVisible || IsScopeActive() || forceScopeForThirdPersonFrontView);
	}
	bool   ShouldUpdateScopeRTT();
	bool   ApplyScopeLensPostProcess();
	void   ToggleScope();
	void   ToggleMouseModeScope();
	void   UpdateScopeAimLineState();

	// ----------------------------
	// Rear mirror (off-hand)
	// ----------------------------
	bool  m_RearMirrorEnabled = false;
	// If enabled, the rear mirror overlay/RTT stays hidden most of the time,
	// and only pops up briefly when a special infected is detected behind you.
	bool  m_RearMirrorShowOnlyOnSpecialWarning = false;
	// Seconds to keep the mirror visible after a special infected warning.
	float m_RearMirrorSpecialShowHoldSeconds = 0.50f;
	std::chrono::steady_clock::time_point m_LastRearMirrorAlertTime{};
	int   m_RearMirrorRTTSize = 512;
	float m_RearMirrorRTTMaxHz = 45.0f;
	std::chrono::steady_clock::time_point m_LastRearMirrorRTTRenderTime{};
	float m_RearMirrorFov = 85.0f;
	float m_RearMirrorZNear = 6.0f;

	Vector m_RearMirrorCameraOffset = { 0.0f, 0.0f, 0.0f };
	QAngle m_RearMirrorCameraAngleOffset = { 0.0f, 0.0f, 0.0f };

	float  m_RearMirrorOverlayWidthMeters = 0.10f;
	float  m_RearMirrorOverlayXOffset = -0.01f;
	float  m_RearMirrorOverlayYOffset = 0.02f;
	float  m_RearMirrorOverlayZOffset = 0.08f;
	QAngle m_RearMirrorOverlayAngleOffset = { 0.0f, 180.0f, 0.0f };
	float  m_RearMirrorAlpha = 1.0f;

	// If true, mirror the rear-mirror texture horizontally (left/right).
	bool  m_RearMirrorFlipHorizontal = false;

	// When the aim line/ray intersects the rear-mirror overlay in view, hide the mirror to avoid blocking aim.
	bool  m_RearMirrorHideWhenAimLineHits = true;
	float m_RearMirrorAimLineHideHoldSeconds = 0.08f;
	std::chrono::steady_clock::time_point m_RearMirrorAimLineHideUntil{};

	// Rear mirror hint: when special-infected arrows are visible in the mirror pass,
	// temporarily enlarge the rear-mirror overlay (2x width).
	// Distance is in Source units (same as SpecialInfected* distances). <= 0 disables this hint.
	float  m_RearMirrorSpecialWarningDistance = 180.0f;
	float  m_RearMirrorSpecialEnlargeHoldSeconds = 0.35f;
	bool   m_ScopeRenderingPass = false;
	bool   m_RearMirrorRenderingPass = false;
	bool   m_RearMirrorSawSpecialThisPass = false;	// set from DrawModelExecute during mirror RTT pass
	bool   m_RearMirrorSpecialEnlargeActive = false;
	std::chrono::steady_clock::time_point m_LastRearMirrorSpecialSeenTime{};

	Vector m_RearMirrorCameraPosAbs = { 0.0f, 0.0f, 0.0f };
	QAngle m_RearMirrorCameraAngAbs = { 0.0f, 0.0f, 0.0f };

	Vector GetRearMirrorCameraAbsPos() const { return m_RearMirrorCameraPosAbs; }
	QAngle GetRearMirrorCameraAbsAngle() const { return m_RearMirrorCameraAngAbs; }
	bool   ShouldRenderRearMirror() const;
	bool   ShouldUpdateRearMirrorRTT();
	void   NotifyRearMirrorSpecialWarning();

	VR();
	VR(Game* game);
	~VR();
	int SetActionManifest(const char* fileName);
	void InstallApplicationManifest(const char* fileName);
	void Update();
	void ApplyShadowSettingsIfNeeded(bool forceApply = false);
	void ApplyFlashlightEnhancementIfNeeded();
	void ApplyLocalVScriptConvarsIfNeeded();
	void ApplyVrRecommendedVideoSettingsIfNeeded(const char* reason);
	void AuditLocalVScriptConvarsCurrentValues(const char* reason);
	bool TryGetTrackedProtectedConvarValue(const char* name, std::string& outValue) const;
	void CaptureFlashlightEnhancementDefaults();
	void RestoreFlashlightEnhancementDefaults();
	void RestoreLocalVScriptConvars();
	bool ShouldBlockExternalProtectedConvarWrite(const char* name, const char* requestedValue);
	void CaptureShadowCvarDefaults();
	void RestoreShadowCvarDefaults();
	void ApplyShadowEntityOverrides(bool forceRefresh);
	void RestoreShadowEntityDefaults();
	void ResetShadowEntityOverrideTracking();
	void UpdateAutoMatQueueMode();
	void ReleaseVRRenderTargetsForDeviceReset();
	void ReleaseVrHandsD3DResources();
	bool DrawVrHandsForEye(const CViewSetup& view, int eyeIndex, VrHandDrawPass drawPass);
	bool DrawVrHandsForEyeImmediate(const CViewSetup& view, int eyeIndex, VrHandDrawPass drawPass, bool allowQueuedMode);
	bool DrawVrHandsWorldDepthMaskForEyeImmediate(const CViewSetup& view, int eyeIndex, bool allowQueuedMode);
	bool QueueVrHandsDrawForEye(IMatRenderContext* renderContext, const CViewSetup& view, int eyeIndex, VrHandDrawPass drawPass);
	void FallbackVrHandsGlovesToNative(const char* reason);
	void PublishMagazineInteractionBox(
		const Vector& origin,
		const Vector& axisX,
		const Vector& axisY,
		const Vector& axisZ,
		const Vector& mins,
		const Vector& maxs,
		uint32_t frameSeq,
		int entityIndex,
		int boneIndex,
		const char* modelName,
		bool modelBasisValid,
		const Vector& modelOrigin,
		const Vector& modelAxisX,
		const Vector& modelAxisY,
		const Vector& modelAxisZ);
	void PublishMagazineInteractionBoltBox(
		const Vector& origin,
		const Vector& axisX,
		const Vector& axisY,
		const Vector& axisZ,
		const Vector& mins,
		const Vector& maxs,
		const Vector& pullAxisWorld,
		uint32_t frameSeq,
		int entityIndex,
		int boneIndex,
		const char* modelName);
	void PublishVrHandsTwoHandedGripWeaponBox(
		const Vector& origin,
		const Vector& axisX,
		const Vector& axisY,
		const Vector& axisZ,
		const Vector& mins,
		const Vector& maxs,
		uint32_t frameSeq,
		int entityIndex,
		const char* modelName);
	void PublishMagazineInteractionNativeLeftWristAnchor(
		const Vector& origin,
		const Vector& axisX,
		const Vector& axisY,
		const Vector& axisZ);
	void PublishMagazineInteractionCalibrationSnapshot(
		const char* modelName,
		const char* sourceClassName,
		uint32_t modelFingerprint,
		uint32_t boneSignature,
		uint32_t renderFrameSeq,
		int entityIndex,
		int weaponId,
		int inferredWeaponId,
		int sourceScore,
		int numBones,
		int recommendedMagazineBone,
		int recommendedBoltBone,
		bool sourceIsViewmodelClass,
		const std::vector<MagazineInteractionCalibrationBone>& bones);
	bool GetMagazineInteractionBox(MagazineInteractionBoxSnapshot& outSnapshot) const;
	bool GetMagazineInteractionBoltBox(MagazineInteractionBoxSnapshot& outSnapshot) const;
	bool GetVrHandsTwoHandedGripWeaponBox(MagazineInteractionBoxSnapshot& outSnapshot) const;
	bool GetMagazineInteractionNativeLeftWristAnchor(VrHandMatrix4& outWorld) const;
	bool HasFreshMagazineInteractionDebugBoxWork() const;
	bool GetMagazineInteractionCalibrationSnapshot(MagazineInteractionCalibrationSnapshot& outSnapshot) const;
	void UpdateNativeViewmodelLeftHandOpenVRFingerCurls();
	bool GetNativeViewmodelLeftHandOpenVRFingerCurls(std::array<float, 5>& outCurls) const;
	void UpdateNativeViewmodelRightHandOpenVRFingerCurls();
	bool GetNativeViewmodelRightHandOpenVRFingerCurls(std::array<float, 5>& outCurls) const;
	bool ReadMagazineInteractionFingerCurls(std::array<float, 5>& outCurls);
	bool UpdateMagazineInteraction(
		C_BasePlayer* localPlayer,
		bool leftGripDown,
		bool leftGripJustPressed,
		bool leftSupportHandDown,
    	bool leftSupportHandJustPressed,
		bool allowGameplayInputOnTwoHandedGripRelease);
	void MarkMagazineInteractionReloadCommandIssued();
	bool IsMagazineInteractionReloadCommandActive() const;
	bool ShouldSuppressMagazineInteractionEmptyClipAutoReload(C_BasePlayer* localPlayer) const;
	bool IsMagazineInteractionLeftHandActive() const;
	bool IsMagazineInteractionManualActive() const;
	bool IsMagazineInteractionNativeReloadSuppressActive() const;
	bool IsMagazineInteractionViewmodelOverrideActive() const;
	bool IsMagazineInteractionBlockingFire() const;
	void MarkMagazineInteractionServerHookSeen(int serverWeaponId);
	bool IsMagazineInteractionAnyServerHookActive() const;
	bool IsMagazineInteractionServerHookActive(int weaponId) const;
	void MarkMagazineInteractionShotgunServerHookSeen(int serverWeaponId);
	bool IsMagazineInteractionShotgunServerHookActive(int weaponId) const;
	void QueueMagazineInteractionServerClipCommit(
		int targetClip,
		int ammoType,
		int targetReserve,
		int expectedPriorClip,
		int expectedPriorReserve,
		const char* reason,
		float holdSeconds);
	bool TryApplyMagazineInteractionServerClipCommit(
		void* serverWeapon,
		int serverWeaponId,
		void* serverPlayer = nullptr);
	void QueueMagazineInteractionShotgunServerReloadAbort(const char* reason);
	void QueueMagazineInteractionShotgunDirectShellCommit(
		int targetClip,
		int ammoType,
		int targetReserve,
		int expectedPriorReserve,
		const char* reason);
	bool TryApplyMagazineInteractionShotgunServerReloadAbort(
		void* serverWeapon,
		int serverWeaponId,
		void* serverPlayer = nullptr);
	bool ApplyMagazineInteractionShotgunClientReloadAbort(
		C_WeaponCSBase* clientWeapon,
		int clientWeaponId,
		const char* reason);
	bool ShouldFreezeMagazineInteractionViewmodel() const;
	bool ShouldHideMagazineInteractionNativeClip() const;
	bool ShouldDrawMagazineInteractionDetachedMagazine() const;
	bool GetMagazineInteractionDetachedMagazineWorld(VrHandMatrix4& outWorld) const;
	bool ShouldMoveMagazineInteractionBolt() const;
	bool GetMagazineInteractionBoltWorld(VrHandMatrix4& outWorld) const;
	void PlayMagazineInteractionBlockedFireEmptySound();
	bool CaptureMagazineInteractionSound(int entityIndex, const char* sample, float volume, int flags, int pitch);
	void CancelMagazineInteractionManual();
	void BeginVrHandsEyeRender(const CViewSetup& view, int eyeIndex);
	void DrawVrHandsWorldDepthMaskBeforeViewmodel();
	void FinishVrHandsEyeRender();
	bool RefreshBackBufferTexture(bool forceRefresh = false);
	void CreateVRTextures();
	void EnsureOpticsRTTTextures();
	void LogVAS(const char* tag);
	void HandleMissingRenderContext(const char* location);
	void SubmitVRTextures();
	vr::EVROverlayError SetOverlayTextureSynchronized(
		vr::IVROverlay* overlay,
		vr::VROverlayHandle_t overlayHandle,
		const vr::Texture_t* texture,
		bool producerPrepared);
	void InvalidateSourceRenderQueueMarkers();
	bool QueueSourceRenderOwnershipAcquireMarker(IMatRenderContext* renderContext);
	bool QueueSourceRenderOwnershipReleaseMarker(IMatRenderContext* renderContext, bool releaseSourceFrameOwnership);
	bool QueueSourceRenderCompletionMarker(IMatRenderContext* renderContext, uint32_t renderPoseToken, uint32_t renderFrameSeq, bool allowDuplicatePoseSubmit, bool releaseSourceFrameOwnership, const vr::HmdMatrix34_t* renderHmdPose = nullptr);
	void PublishSourceQueueCompletedFrame(uint32_t sourceQueueEpoch, uint32_t renderPoseToken, uint32_t renderFrameSeq, bool allowDuplicatePoseSubmit, uint32_t sourceQueueMarkerId, const vr::HmdMatrix34_t* renderHmdPose = nullptr);
	uint32_t PublishRenderCompletedFrame(uint32_t renderPoseToken, uint32_t renderFrameSeq, bool allowDuplicatePoseSubmit, const char* sourceTag, uint32_t sourceQueueMarkerId = 0, const vr::HmdMatrix34_t* renderHmdPose = nullptr);
	void LogCompositorError(const char* action, vr::EVRCompositorError error);
	void RepositionOverlays();
	void UpdateHudLiftGestureState(bool inGame);
	bool IsGameplayHudRequested() const;
	void MarkQueuedHudFresh(uint64_t holdMs = 300);
	void ClearQueuedHudFresh();
	bool IsQueuedHudFresh() const;
	void UpdateRearMirrorOverlayTransform();
	void UpdateScopeOverlayTransform();
	void UpdateHandHudOverlays(PendingOverlayTextureBindBatch* pendingTextureBinds = nullptr);
	void DestroyHandHudWorldQuadTextures();
	void GetPoses();
	bool UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void ProcessInput();
	void SendVirtualKey(WORD virtualKey);
	void SendVirtualKeyDown(WORD virtualKey);
	void SendVirtualKeyUp(WORD virtualKey);
	void SendFunctionKey(WORD virtualKey);
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t& hmdMat);
	vr::HmdMatrix34_t VMatrixToHmdMatrix(const VMatrix& vMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	vr::HmdMatrix34_t BuildThirdPersonSubmitPose() const;
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetLeftControllerAbsAngle();
	Vector GetLeftControllerAbsPos();
	QAngle GetRightControllerAbsAngle();
	Vector GetRightControllerAbsPos();
	Vector GetRightControllerViewmodelAbsPos();
	bool ResolvePavlovTwoHandedAimBasis(
		const Vector& leftControllerPosAbs,
		const Vector& rightControllerPosAbs,
		const Vector& rightControllerForward,
		const Vector& rightControllerRight,
		const Vector& rightControllerUp,
		const Vector& hmdPosAbs,
		const Vector& hmdForward,
		const Vector& hmdRight,
		const Vector& hmdUp,
		float sourceUnitsPerMeter,
		Vector& outForward,
		Vector& outRight,
		Vector& outUp) const;
	void ApplyPavlovTwoHandedAimSmoothing(Vector& forward, Vector& right, Vector& up);
	void ResetPavlovTwoHandedAimSmoothing();
	Vector GetRecommendedViewmodelAbsPos();
	QAngle GetRecommendedViewmodelAbsAngle();
	// Mouse-mode: compute the eye-center ray used for aiming (mouse pitch+yaw or HMD-based, optionally sensitivity-scaled).
	void GetMouseModeEyeRay(Vector& eyeDirOut, QAngle* eyeAngOut = nullptr);
	void UpdateSystemMouseInputSuppression(bool inGame, bool hasLocalPlayer);
	void UpdateTracking();
	void UpdateMotionGestures(C_BasePlayer* localPlayer);
	void UpdateAutoFlashlight(C_BasePlayer* localPlayer);
	void ResetAutoFlashlightState();
	bool SampleAutoFlashlightScreenLuma(float& outCenterMedianLuma, float& outCenterLowLuma, float& outPeripheralMedianLuma, float& outMeanLuma);
	void IssueFlashlightToggle(bool manual);
	bool QueryFlashlightState(C_BasePlayer* localPlayer, bool& outOn);
	bool UpdateThirdPersonViewState(const Vector& cameraOrigin, const Vector& cameraAngles);
	Vector GetViewAngle();
	// Yaw (degrees) used as the movement basis for the walk axis.
	// Default: HMD yaw. Optional: right-controller yaw (hand-oriented locomotion).
	float GetMovementYawDeg();
	Vector GetViewOriginLeft();
	Vector GetViewOriginRight();
	Vector GetThirdPersonViewOrigin() const { return m_ThirdPersonViewOrigin; }
	QAngle GetThirdPersonViewAngles() const { return m_ThirdPersonViewAngles; }
	bool IsThirdPersonCameraActive() const { return m_IsThirdPersonCamera; }
	bool CanApplyResetPositionNow() const;
	// Death flicker guard (see m_DeathFirstPersonLockEnd).
	void RefreshDeathFirstPersonLock(const C_BasePlayer* localPlayer);
	bool IsDeathFirstPersonLockActive() const;
	// Map-load / reconnect cooldown: suppress observer-driven 3P latching for a short window.
	bool IsThirdPersonMapLoadCooldownActive() const;
	bool PressedDigitalAction(vr::VRActionHandle_t& actionHandle, bool checkIfActionChanged = false);
	bool GetDigitalActionData(vr::VRActionHandle_t& actionHandle, vr::InputDigitalActionData_t& digitalDataOut);
	bool GetAnalogActionData(vr::VRActionHandle_t& actionHandle, vr::InputAnalogActionData_t& analogDataOut);
	void RefreshLeftHandedInputActionSwapMaps(bool force = false);
	bool GetLeftHandedMirroredDigitalActionData(vr::VRActionHandle_t actionHandle, vr::InputDigitalActionData_t& digitalDataOut);
	bool GetLeftHandedMirroredAnalogActionData(vr::VRActionHandle_t actionHandle, vr::InputAnalogActionData_t& analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t& poseRaw, TrackedDevicePoseData& poseOut);
	void PoseWaiterThreadMain();
	bool ReadPoseWaiterSnapshot(vr::TrackedDevicePose_t* outPoses, uint32_t* outSeq = nullptr) const;
	// leftHand follows the project's gameplay hand ordering after LeftHanded remapping.
	bool IsGameplayHandLeftPhysical(bool leftHand) const;
	vr::TrackedDeviceIndex_t GetPhysicalControllerIndexForHand(bool leftHand) const;
	void TriggerLegacyHapticPulse(vr::TrackedDeviceIndex_t deviceIndex, float durationSeconds, float amplitude) const;
	void TriggerPhysicalHandHapticPulse(bool leftHand, float durationSeconds, float frequency, float amplitude, int priority = 1);
	void FlushHapticMixer();
	WeaponHapticsProfile GetWeaponHapticsProfile(int weaponId) const;
	void TriggerWeaponFireHaptics(int weaponId, bool leftHand = false);
	void TriggerMeleeSwingHaptics(bool leftHand = false);
	void TriggerShoveHaptics(bool leftHand = false);
	void NotifyMeleeHitConfirmed(std::uintptr_t entityTag = 0);
	void ParseConfigFile();
	void ParseHapticsConfigFile();
	void LoadViewmodelAdjustments();
	void SaveViewmodelAdjustments();
	void RefreshActiveViewmodelAdjustment(C_BasePlayer* localPlayer);
	ViewmodelAdjustment& EnsureViewmodelAdjustment(const std::string& key);
	void LoadScopeAdjustments();
	void SaveScopeAdjustments();
	void RefreshActiveScopeAdjustment(C_BasePlayer* localPlayer);
	ScopeAdjustment& EnsureScopeAdjustment(const std::string& key);
	std::string BuildScopeAdjustKey(C_WeaponCSBase* weapon) const;
	std::string BuildViewmodelAdjustKey(C_WeaponCSBase* weapon) const;
	std::string WeaponIdToString(int weaponId) const;
	std::string NormalizeViewmodelAdjustKey(const std::string& rawKey) const;
	std::string GetMeleeWeaponName(C_WeaponCSBase* weapon) const;
	void WaitForConfigUpdate();
	bool GetWalkAxis(float& x, float& y);
	void UpdateNonVRAimSolution(C_BasePlayer* localPlayer, bool forceFresh = false, bool allowWithoutForceNonVR = false);
	// Friendly-fire aim guard:
	// - m_AimLineHitsFriendly is computed from a ray trace and may flicker at hitbox edges.
	// - While the attack button is held, flicker can effectively create press/release edges.
	// To make this robust, we compute the friendly-hit from the current aim ray on CreateMove
	// ticks and latch suppression until the user releases attack.
	bool ShouldSuppressPrimaryFire(const CUserCmd* cmd, C_BasePlayer* localPlayer);
	bool UpdateFriendlyFireAimHit(C_BasePlayer* localPlayer);
	void UpdateAimTeammateHudTarget(C_BasePlayer* localPlayer, const Vector& start, const Vector& end, bool aimLineActive);
	bool GetAimTeammateHudInfo(int& outPlayerIndex, int& outPercent, char* outName, size_t outNameSize);
	int GetIncapMaxHealth() const;
	void BeginPredictedHitFeedbackShot(int commandNumber = 0);
	void RegisterPotentialKillSoundHit(const Vector& start, const QAngle& angles);
	void UpdateMeleeHitHaptics();
	void EnsureMeleeHitHapticsEventListener();
	void HandleMeleeHitHapticsGameEvent(IGameEvent* event);
	void UpdateKillSoundFeedback();
	void EnsureKillSoundEventListener();
	void HandleKillSoundGameEvent(IGameEvent* event);
	void EnsureDamageFeedbackEventListener();
	void HandleDamageFeedbackGameEvent(IGameEvent* event);
	void UpdateDamageFeedback();
	DamageFeedbackType ClassifyDamageFeedbackType(const char* weaponName, int damage) const;
	ControlledDamageState ResolveLocalSpecialControlState(C_BasePlayer* localPlayer, C_BaseEntity** outControllerEntity = nullptr) const;
	WeaponHapticsProfile GetDamageHapticsProfile(DamageFeedbackType type) const;
	WeaponHapticsProfile GetControlledDamageHapticsProfile(ControlledDamageState state) const;
	float GetControlledDamageDirectionalBiasScale(ControlledDamageState state) const;
	void TriggerImpactHapticsBothHands(float amplitude, float frequency, float durationSeconds, int priority = 1);
	void TriggerDirectionalDamageHaptics(float amplitude, float frequency, float durationSeconds, float rightBias, int priority = 1);
	void QueuePendingKillSoundEvent(std::uintptr_t entityTag, bool headshot);
	bool ConsumePendingKillSoundEvent(std::chrono::steady_clock::time_point now, bool& outHeadshot, std::uintptr_t& outEntityTag);
	bool ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial);
	bool ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial, char* outSource);
	bool ReadLocalHeadshotCounter(C_BasePlayer* localPlayer, int& outHeadshots) const;
	bool IsKillSoundTargetEntity(const C_BaseEntity* entity) const;
	bool ConsumePendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos = nullptr);
	bool FindPendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos = nullptr, uint32_t preferredShotSerial = 0);
	void PlayHitSound(const Vector* worldPos = nullptr);
	void PlayKillSound(bool headshot, const Vector* worldPos = nullptr);
	bool TryPlayKillSoundSpec(const std::string& spec, float baseVolume = 1.0f, const Vector* worldPos = nullptr, bool preferLoadedPathReuse = true);
	bool TryPlayGameSoundFileLocal(const std::string& soundPath, float baseVolume = 1.0f, const Vector* worldPos = nullptr);
	void QueueHitSoundPlayback(const Vector* worldPos = nullptr, uint32_t shotSerial = 0, std::uintptr_t entityTag = 0);
	void FlushPendingHitSound(std::chrono::steady_clock::time_point now);
	void ComputeFeedbackSoundStereoVolumes(const Vector* worldPos, float baseVolume, int& outLeftVolume, int& outRightVolume) const;
	void SyncVrmodFeedbackGameSounds() const;
	void EnsureFeedbackSoundWarmup();
	void EnsureFeedbackSoundWorkerThread();
	bool EnqueueFeedbackSoundPlayback(const std::string& resolvedPath, int leftVolume, int rightVolume, bool preferLoadedPathReuse = true);
	void EnqueueFeedbackSoundWarmupPath(const std::string& resolvedPath);
	void ResetFeedbackSoundWorkerState();
	void FeedbackSoundWorkerMain();
	void EnsureSpeechWorkerThread();
	void SpeechWorkerMain();
	bool BeginSpeechToTextCapture();
	void EndSpeechToTextCapture(bool queueTranscription = true);
	void PumpSpeechToTextCapture();
	void PumpSpeechToTextResults();
	void PumpSpeechToTextVoiceBroadcast();
	void QueueSpeechToTextVoicePlayback(const std::string& text);
	void QueueGeneratedSpeechVoiceBroadcast(const std::string& wavPath);
	void UpdateVoiceRecordCommandState();
	void StopVoiceRecordCommandNow(bool disableVoiceInputFromFile = true);
	void QueueChatTextToSpeech(const std::string& speaker, const std::string& text);
	void HandleHudChatLine(const std::string& speaker, const std::string& text);
	TextToSpeechRuntimeConfig BuildTextToSpeechRuntimeConfig(bool useSpeechToTextSendVoiceProfile) const;
	bool EnsureTextToSpeechServerReady(const TextToSpeechRuntimeConfig& config);
	void ShutdownTextToSpeechServer();
	void SpawnHitIndicator(const Vector& worldPos);
	void SpawnKillIndicator(bool headshot, const Vector& worldPos);
	void DrawKillIndicators(IMatRenderContext* renderContext, ITexture* hudTexture);
	void DrawProjectedItemLabels(IMatRenderContext* renderContext, const CViewSetup& view, int eyeIndex);
	void RecordProjectedSpecialInfectedArrow(int entityIndex, const Vector& origin, SpecialInfectedType type);
	void DrawCachedSpecialInfectedArrowsDebugOverlay();
	void DrawProjectedSpecialInfectedArrows(IMatRenderContext* renderContext, const CViewSetup& view);
	void DrawPostMirrorPluginOverlays(IMatRenderContext* renderContext, C_BasePlayer* localPlayer, const CViewSetup& view, int eyeIndex);
	bool PrepareLeftEyeSurvivorGlowCopy();
	bool CopyLeftEyeSurvivorGlowToRightEye();
	bool CopyLeftEyeToRightEyeTexture();
	bool CopyEyeToDesktopMirrorTexture(int eyeIndex);
	bool CompositeHudToDesktopMirrorTexture();
	IDirect3DTexture9* GetOrCreateProjectedItemLabelTexture(
		IDirect3DDevice9* device,
		const std::string& text,
		int fontPx,
		int colorR,
		int colorG,
		int colorB,
		int colorA,
		int& outWidth,
		int& outHeight);
	void UpdateKillIndicatorOverlays();
	IMaterial* ResolveHitIndicatorMaterial();
	IMaterial* ResolveKillIndicatorMaterial(bool headshot);
	void DestroyKillIndicatorOverlayTextures();
	void DestroyKillIndicatorOverlayTexture(int materialIndex);
	bool EnsureKillIndicatorOverlayTexture(int materialIndex, int width, int height);
	bool UploadKillIndicatorOverlayTexture(int materialIndex, const uint8_t* rgba, int width, int height, uint32_t frameIndex = 0, bool fromDecodedFrames = false);
	void DestroyItemLabelOverlayTexture();
	void PublishQueuedProjectedItemLabels(int eyeIndex, std::vector<QueuedProjectedItemLabelDraw> draws);
	void ClearQueuedProjectedItemLabelEye(int eyeIndex);
	void ClearQueuedProjectedItemLabels();
	void DrawQueuedProjectedItemLabelsToSurface(IDirect3DDevice9* device, int eyeIndex, IDirect3DSurface9* target, bool manageScene = true);
	void PumpDesktopCompanionWindows();
	void UpdateDesktopRearMirrorWindow(bool visible);
	void UpdateDesktopIntentSenseHudWindow(const uint8_t* rgba, int width, int height, bool visible);
	void DestroyDesktopCompanionWindows();
	static LRESULT CALLBACK DesktopCompanionWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void TrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool clearAll = false);
	void MaybeTrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool force = false);
	void MaybeLogKillIndicatorStats(std::chrono::steady_clock::time_point now);
	void DestroyKillIndicatorOverlay(ActiveKillIndicator& indicator);
	bool EnsureKillIndicatorOverlaySlot(int slotIndex);
	int AcquireKillIndicatorOverlaySlot(int materialIndex) const;
	int FindReusableKillIndicatorIndex(bool preferNonKill) const;
	void AddOrRecycleKillIndicator(const Vector& worldPos, bool killConfirmed, bool headshot, std::chrono::steady_clock::time_point now, bool preferNonKill);
	bool BuildKillIndicatorOverlayPixels(IMaterial* material, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight, uint32_t preferredFrameIndex = UINT32_MAX, bool* outUsedDecodedFrames = nullptr);
	bool ComputeKillIndicatorOverlayTransform(const Vector& worldPos, vr::HmdMatrix34_t& outTransform) const;
	// Mounted gun helper: returns the entity the player is currently "using" (turret/mounted gun) if any.
	// Used to skip that entity in aim-related traces so the aim line doesn't collide with the gun platform.
	bool IsUsingMountedGun(const C_BasePlayer* localPlayer) const;
	C_BaseEntity* GetMountedGunUseEntity(C_BasePlayer* localPlayer) const;
	bool m_EncodeVRUsercmd = true;
	void UpdateAimingLaser(C_BasePlayer* localPlayer);
	void UpdateD3DAimLineOverlayForView(C_BasePlayer* localPlayer, const CViewSetup& view, int eyeIndex);
	void ClearD3DAimLineOverlayEye(int eyeIndex);
	void ClearD3DAimLineOverlay();
	bool GetD3DAimLineOverlayEye(int eyeIndex, D3DAimLineOverlayEyeState& out) const;
	bool ApplyD3DAimLineOcclusionFromAimLine(C_BasePlayer* localPlayer, Vector& start, Vector& end);
	// In queued (multicore) rendering, the render thread uses a render-frame pose snapshot.
	// Draw the aim line from the render hook (dRenderView) using that snapshot to avoid head-turn ghosting.
	void RenderDrawAimLineQueued(C_BasePlayer* localPlayer);
	bool BuildRenderAimLineSegment(C_BasePlayer* localPlayer, Vector& start, Vector& end);
	bool ShouldShowAimLine(C_WeaponCSBase* weapon) const;
	bool IsThrowableWeapon(C_WeaponCSBase* weapon) const;
	bool ShouldDrawAimLine(C_WeaponCSBase* weapon) const;
	bool IsVrHandsRealBulletSpreadWeapon(C_WeaponCSBase* weapon) const;
	void ClearVrHandsRealBulletSpreadAimLine();
	bool ApplyVrHandsRealBulletSpreadAimLine(C_WeaponCSBase* weapon, const Vector& origin, Vector& direction);
	bool ApplyVrHandsRealBulletSpreadAimAngles(C_WeaponCSBase* weapon, QAngle& angles);
	bool ApplyVrHandsRealBulletSpreadServerViewAngles(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, QAngle& angles);
	bool NotifyVrHandsRealBulletSpreadClientShot(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, const Vector& origin, QAngle& angles, float fireSpreadArgument);
	bool IsWeaponLaserSightActive(C_WeaponCSBase* weapon) const;
	float CalculateThrowArcDistance(const Vector& pitchSource, bool* clampedToMax = nullptr) const;
	void DrawAimLine(const Vector& start, const Vector& end);
	void RenderDrawGameLaserSight(C_BasePlayer* localPlayer);
	void DrawThrowArc(const Vector& origin, const Vector& forward, const Vector& pitchSource);
	void DrawThrowArcFromCache(float duration);
	void DrawLineWithThickness(const Vector& start, const Vector& end, float duration);
	bool IsLocalPlayerEntityIndex(int entityIndex) const;
	SpecialInfectedType GetSpecialInfectedType(const C_BaseEntity* entity) const;
	SpecialInfectedType GetSpecialInfectedTypeFromModel(const std::string& modelName) const;
	bool TryGetItemModelLabelInfo(const std::string& modelName, ItemModelLabelInfo& out) const;
	bool IsItemModelLabelBlacklisted(const std::string& label) const;
	bool IsEntityAlive(const C_BaseEntity* entity) const;
	void DrawSpecialInfectedArrow(const Vector& origin, SpecialInfectedType type);
	// Stable wrappers used by core rendering/hooks. They are always defined in vr.cpp.
	// If special_infected_features.cpp is linked, it registers the real implementations;
	// otherwise these wrappers safely no-op so stripped builds still link.
	void DrawItemModelLabel(int entityIndex, const std::string& modelName, const Vector& modelOrigin, const C_BaseEntity* entity, const char* className);
	void ScanSpecialInfectedEntitiesFromClientList();
	void ScanItemModelLabelEntitiesFromClientList();
	void ApplyOptionalCreateMoveFeatures(CUserCmd* cmd, int stage, bool inputJumpHeld, bool hadWalkAxis, float walkNx, float walkNy, bool suppressScopeWalk);

	// Optional implementations supplied by special_infected_features.cpp. Do not call these
	// directly from core code because stripped builds intentionally omit their definitions.
	void DrawItemModelLabelImpl(int entityIndex, const std::string& modelName, const Vector& modelOrigin, const C_BaseEntity* entity, const char* className);
	void ScanSpecialInfectedEntitiesFromClientListImpl();
	void ScanItemModelLabelEntitiesFromClientListImpl();
	void ApplyOptionalCreateMoveFeaturesImpl(CUserCmd* cmd, int stage, bool inputJumpHeld, bool hadWalkAxis, float walkNx, float walkNy, bool suppressScopeWalk);
	void RefreshSpecialInfectedPreWarning(const Vector& infectedOrigin, SpecialInfectedType type, int entityIndex, bool isPlayerClass);
	void RefreshSpecialInfectedBlindSpotWarning(const Vector& infectedOrigin);
	void BeginSpecialInfectedIntentSenseScan();
	void FinishSpecialInfectedIntentSenseScan();
	void RefreshSpecialInfectedIntentSense(const C_BaseEntity* entity, const Vector& infectedOrigin, SpecialInfectedType type, int entityIndex);
	bool IsSpecialInfectedTargetingLocalPlayer(const C_BaseEntity* entity, const Vector& infectedOrigin, SpecialInfectedType type, int entityIndex, float& outRightBias, const char*& outDirectionText, float& outDistanceMeters, bool& outFront);
	bool NotifySpecialInfectedIntentSense(SpecialInfectedType type, const char* directionText, float distanceMeters, float rightBias, bool front, int entityIndex, const Vector& infectedOrigin);
	bool HasLineOfSightToSpecialInfected(const Vector& infectedOrigin, int entityIndex) const;
	bool IsSpecialInfectedInBlindSpot(const Vector& infectedOrigin) const;
	void UpdateSpecialInfectedWarningState();
	void UpdateSpecialInfectedPreWarningState();
	Vector GetAimRenderCameraDelta() const;
	bool ApplyMovementLedgeGuard(CUserCmd* cmd, bool suppressScopeWalk);
	void ApplyRoomscale1To1Move(CUserCmd* cmd, float inputSampleTime, bool controlLocomotionActive);
	bool ShouldUseRoomscale1To1ServerMove() const;
	void QueueRoomscale1To1ServerMoveDelta(const Vector& worldDelta, const Vector& visualWorldDelta);
	bool ConsumeRoomscale1To1ServerMoveDelta(Vector& outWorldDelta, Vector& outVisualWorldDelta);
	void QueueRoomscale1To1ServerVisualCorrection(const Vector& worldCorrection);
	void ApplyPendingRoomscale1To1ServerVisualCorrection();
	void ApplyRoomscale1To1VisualWorldCorrection(const Vector& worldCorrection);
	void CancelRoomscale1To1ServerMoveDelta();
	void ClearRoomscale1To1ServerMoveDelta();
	bool ShouldUseTeleportServerMove() const;
	void BeginTeleportTargeting();
	void EndTeleportTargeting(bool commit);
	void CancelTeleportTargeting();
	void UpdateTeleportTargeting();
	void QueueTeleportServerTarget(const Vector& targetWorld);
	bool ConsumeTeleportServerTarget(Vector& outTargetWorld);
	void ClearTeleportServerTarget();
	void QueueTeleportVisualWorldDelta(const Vector& worldDelta, const Vector& landingTarget);
	void ApplyPendingTeleportVisualWorldDelta();
	void ConsumeTeleportEngineAnchorCompensation(Vector& engineOriginDelta);
	void SuppressTeleportViewmodelForMs(uint32_t durationMs);
	bool ShouldSuppressTeleportViewmodelRender() const;
	void EnterTeleportVisualScout(const Vector& targetWorld);
	void ExitTeleportVisualScout();
	void ClearTeleportVisualScout();
	void OnPredictionRunCommand(CUserCmd* cmd);
	void OnPrimaryAttackServerDecision(CUserCmd* cmd, bool fromSecondaryPrediction);
	void StartSpecialInfectedWarningAction();
	void UpdateSpecialInfectedWarningAction();
	void ResetSpecialInfectedWarningAction();
	void GetAimLineColor(int& r, int& g, int& b, int& a) const;
	void UpdateAimLineEffectiveAttackRange(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, C_BaseEntity* hitEntity, const Vector& start, const Vector& end, const Vector& hitPos, bool hasAimHit);
	bool IsEffectiveAttackRangeTarget(const C_BaseEntity* entity) const;
	bool IsEffectiveAttackRangeWitchTarget(const C_BaseEntity* entity) const;
	void LogEffectiveAttackRangeTarget(C_BaseEntity* entity, C_WeaponCSBase* weapon, float distance, float maxRange, float spreadDegrees, bool cached, const char* dataSource);
	bool EnsureEffectiveAttackRangeWeaponDataLoaded();
	const EffectiveAttackRangeWeaponData* GetEffectiveAttackRangeWeaponData(C_WeaponCSBase* weapon);
	float GetEffectiveAttackRangeSpreadDegrees(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, const EffectiveAttackRangeWeaponData& data) const;
	float GetEffectiveAttackRangeHitPointTolerance(const Vector& start, const Vector& centerHitPos, float spreadDegrees, float maxRange) const;
	bool DoesEffectiveAttackRangeSpreadConeHitTarget(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, C_BaseEntity* hitEntity, const Vector& start, const Vector& end, const Vector& centerHitPos, float spreadDegrees, float maxRange) const;
	bool TryFindEffectiveAttackRangeMeleeFanTarget(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, const Vector& start, const Vector& end, float maxDistance, C_BaseEntity*& outTarget, Vector& outTargetPos, float& outDistance) const;
	void FinishFrame();
	void ConfigureExplicitTiming();
};
