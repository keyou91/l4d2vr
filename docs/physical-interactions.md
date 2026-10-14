# Experimental physical interactions

Remote shotgun shell insertion is now implemented as a separately negotiated
protocol. Both the guest and listen-server host need this build. The host reads
its actual owned weapon, script clip capacity and native reserve. One request
transfers exactly one existing round, ends any earlier conventional reload, and
marks the weapon and owner for Source replication. The guest waits for both a
matching result and replicated clip/reserve values; it does not write predicted
ammo for this path. The listen-host uses the same transaction as the guest.
Each physical insertion requests one shell; the older batch-insert setting does
not cause multiple shells to be created by a single gesture on this path.

Connection tokens, weapon network-handle serials, command freshness, expected
ammo, sequence watermarks and a duplicate-response cache protect the transaction.
The token binds a session; it is not an authentication credential. A request
cannot target someone else's gun or supply a desired clip count. Native function
signatures gate support. A missing backend, unsupported layout or a 2.5-second
reply/replication timeout preserves conventional reload and disables remote
shell insertion for that lease. A new map/connection can negotiate again.

Detachable-magazine weapons use a second negotiated protocol and require the
matching physical-control capability for native reload suppression. Ejection retains
at most one existing chambered round; insertion fills to the native capacity
using available reserve. The host checks the engine's infinite-ammo rule,
including when a pistol's ammo definition is finite. A preceding accepted
ejection is required, duplicate requests reuse their result, and the guest
waits for both the result and replicated ammunition. Empty reloads still need
a physical slide cycle. A missing slide region is retried rather than completing
the remote reload automatically. Native firing timers are preserved.

The listen-server host now prefers that same magazine protocol even when its
local weapon hooks are active. This avoids the old local full-clip prediction
when only a partial reserve exists. A capability change starts a fresh reload
session before choosing its backend; it cannot splice a local predicted
ejection into an authoritative insertion. Native dual pistols and left-held
single pistols retain their shared reload fallback. Without the magazine
protocol, Magazine Release requests a short native reload pulse; stock reload
buttons and empty-clip automatic reload remain available. There is no virtual
magazine, body ammo preview or physical empty-chamber latch in that fallback.
Support grip remains available. Menus, unavailable gameplay control and session
changes cancel the pulse. The old single-magazine predicted clip/reserve writers
and clip-offset search have been removed. Local shotgun clip/reserve prediction,
its queued writer and reserve-offset search have also been removed. Shell
transactions require the matching physical-control capability. Backend changes
clear the current shell/pump interaction; unsupported or expired shell support
uses stock reload and pumping, including native pump sounds. Support grip remains
available, and Magazine Release requests native reload in that fallback too.

An ejection acknowledgement confirms ammunition but does not clear a previously
observed empty physical chamber. A predicted/native refill therefore cannot
skip the required slide cycle; a loaded tactical reload still keeps its round.

Magazine protocol version 2 adds catching and reinserting the ejected magazine.
Hold left grip with the palm at the magazine well, then press Magazine Release.
After the native ejection is confirmed, the caught magazine uses the calibrated
hand grip. Withdraw it from the well and return it to reinsert its actual rounds;
reserve ammunition stays unchanged. Empty and partially used magazines remain
empty/partial even with native infinite pistol ammo. A chamber fired after
ejection still needs its physical slide cycle after reinsertion.

This is a catch/reinsert interaction, with one retained opportunity per owned
weapon. Releasing the caught item, replacing it with a fresh magazine or starting
a new ejection ends that opportunity in the current client flow. Picking a
dropped magazine up from the floor, persistent magazine storage and transfer
between players remain unfinished. Version 1 magazine hosts/clients use native
reload fallback; both players need version 2 for this interaction. Retained
rounds never become reserve ammo and duplicate requests cannot insert them twice.
Independent dual-pistol ammunition and dedicated-server ammo support are also
unfinished. Native bindings and transaction tests passed offline; client/server
delivery, Source replication ordering and interaction feel remain unverified.
The edict notification and network-handle definitions follow the
[L4D2 SDK headers](https://github.com/alliedmodders/hl2sdk/tree/l4d2/public).
Handle serial comparison follows the SDK's
[send proxy](https://github.com/alliedmodders/hl2sdk/blob/l4d2/game/server/sendproxy.cpp).

For a future friend-hosted test, confirm `[VR][RemoteShell][client] protocol=1`
on the guest. Load one shell into a partial and an empty shotgun, check both
clip and reserve on the host, then test pumping and firing. Repeat with a weapon
switch, downing/revival, no reserve ammo and a map change. Check server
`[VR][RemoteShell][server]` and guest confirmation logs. Also test an older host
and a deliberately interrupted connection for conventional reload recovery.

For remote magazines, test right-A ejection from a loaded and an empty gun,
then grip a fresh magazine at the body anchor and insert it. Check actual host
clip and reserve, a partial reserve, firing the retained chambered round, and
the empty-gun slide cycle. Repeat across a weapon switch, downing/revival and
map change. `[VR][RemoteMagazine]` logs the action, sequence and result. Both
players need the matching build; older hosts retain conventional reload.

This branch contains opt-in grip pickup/drop, body inventory access, manual
pump cycling, and independently tracked dual pistols. These features have
automated state and geometry coverage and compile in the Windows x86 Release
build. Headset and multiplayer validation remain required.

Enable `ManualThrowEnabled`, `GripReleaseDropEnabled`, and
`DualPistolsIndependentHandsEnabled` to test pistol detachment on a server
running the same build. Keep `DualPistolsNativeReloadFallbackEnabled` enabled
for dual pistols and for a single pistol retained in the gameplay left hand.
The repository config leaves pickup/drop and independent tracking switches off
by default. Shared dual-pistol button reload is enabled by default.
The server also advertises `l4d2vr_interaction_ack 1` separately from its pose
relay acknowledgement. Grip transport, independent pistol commands and native
reload suppression require that matching capability. A pose-only or older
server cannot enable them just by acknowledging VR movement. The capability
is cleared when the client changes server sessions; ordinary controls remain
available without it.

With the supplied Quest binding:

An optional native player-command hook now selects the dual-pistol ray from the
command Source actually executes. The server stores each decoded command's hand
and pose separately; a newer packet or altered duplicate cannot replace it.
Execution context is scoped to the living native owner/edict serial and cleared
after the native call. Missing/expired poses use the existing native path, and a
signature mismatch retains the older packet-based compatibility path. This fixes
the command-to-ray association in source and replay tests; actual host/guest
firing, prediction and effects still need native gameplay verification. Per-hand
magazine/chamber ammunition remains unfinished.

- Right grip picks up inventory items and drops the held item on release.
- Left grip can pick up a loose pistol when the pistol hand is free.
- Each trigger aims and fires its own pistol while dual pistols are equipped.
- Both pistol hands use the common predicted hit-feedback path. Firing haptics
  select the firing gameplay hand and follow the left-handed input mapping.
  Cached live shot rays are scoped to the current player/weapon and expire;
  they cannot redirect feedback for a subsequently selected gun.
- Releasing either grip splits a pair into a retained single pistol and a
  stock world `weapon_pistol`. The retained pistol stays in the other hand.
- Releasing both grips queues the second drop until the native split is
  confirmed. Tracking loss and holding grips across an ordinary weapon switch
  do not create release events.
- Right A releases a magazine for physical reloads. For dual pistols and a
  single pistol held in the left hand, it requests native reload instead.
- Left X toggles the flashlight; right stick click jumps.

Pistol splitting uses the native `CPistol::RemoveDualWeapons` function after
preparing a world pistol. Signature and clip-layout checks disable the
transaction if the server binary differs. A server ledger retains the contents
of each hand after confirmed pickup and observed native shots. When that
partition is known, detachment preserves the selected pistol's count instead
of halving both clips. Unknown partial pairs and unexplained native ammunition
changes use the existing balanced partition; the shared total is conserved.
Player and weapon edict generations scope that ledger. A scoped native pickup
hook restores the sum of the two clips
when an existing single pistol becomes a pair. Drop markers 60 and 61 are
stripped before native gameplay input and carry a command-number watermark
to reject duplicate backup commands. Switching the command pose between
hands never contributes a spurious throwing velocity.

An optional validated native gun-firing hook captures the pistol that actually
fires after weapon selection. The decoder retains hand intent before that
selection executes; the native bullet callback still validates pistol type.
Bullet events must match the native clip decrease before per-hand accounting
advances. Separate firing events within one command retain their ordinal, while
replayed events cannot spend the same rounds again. Without the optional firing
binding, the existing command-level accounting remains available. Execution
scope is restored on normal return, C++ exceptions and structured exceptions.

The pair still uses L4D2's shared clip, firing cadence, animations, and native
reload. A split uses the stock single-pistol model, so the distinct left-hand
Glock appearance is not retained after detachment. Independent magazines,
chambers, safeties, and per-pistol cosmetic identity remain unfinished.
Per-hand accounting controls detachment and negotiated empty-hand firing, using validated native script
capacities up to 15 rounds per pistol. Larger capacities retain the legacy
split path. It does not replace the shared reload. Pickup-hand metadata is consumed from the simulated grip command;
older hosts ignore that metadata and retain their existing pickup behavior.

Matching listen-host/client builds negotiate pistol-ammo protocol v3 with a
session acknowledgement. The host publishes the native clip with known right/left
contents after command execution, and sends heartbeat snapshots while unchanged.
The client accepts only its session token, increasing sequence numbers,
nondecreasing command numbers, the current weapon handle and matching native
dual state. A bounded journal projects actual native client shots newer than the
host acknowledgement. Each firing event has a command/ordinal key; an older
replay excludes newer events and cannot charge an existing event twice. Bullet
events must match native clip loss. Projected hand counts must sum to the actual
client clip. Unknown contents, owner changes, mismatched replication, unexpected
consumption and snapshots older than one second expose
no independent counts. Both hand counts are published atomically for subsequent
hand interactions. In ordinary finite-clip mode, a known empty pistol is filtered
from trigger routing and blocked at both native firing boundaries, while the
loaded pistol remains usable. The host gates only after a matching client
acknowledges the session. Missing hooks, unknown partitions, older protocol
versions and nonzero/unreadable `sv_infinite_ammo` modes retain native behavior.
The journal never writes predicted clip/reserve counters. Independent magazine
and chamber transactions remain unfinished.

The per-hand magazine/chamber policy is connected to validated host transactions.
Requests wait until their input command has been simulated, then recheck the
current weapon, clip, reserve and session. Duplicate requests reuse the result;
hand-specific rate limits allow the other hand to act independently. Native
clip/reserve replication and protocol-v3 chamber masks settle the client result,
including when the other pistol fires while a request is pending. Empty chambers
block native firing, and host stock reload is suppressed while physical ownership is
active. Failed writers commit no magazine state. Fresh replacement reports
released cached contents for object handoff.

Optional verified client reload and reload-completion hooks now suppress native
prediction for the currently held pistol while its fresh host state reports
physical ownership. They clear only reload flags, leaving ammo counters and
shared firing cooldowns untouched. Unknown/expired state, another player or
weapon, incapacitation and non-default ammo modes retain native behavior.
New pistol transactions require both client reload bindings to be available.

Single right-hand pistols now connect magazine release, retained/fresh magazine
insertion and slide gestures to the request/response API. The host confirms the
native ammo update before the gesture advances. An empty reload waits for a
separate acknowledged slide cycle; a tactical reload preserves its chamber and
needs no cycle. Quick Reload does not auto-cycle this path. Its client cleanup
clears only reload flags, preserving native ammo and shared firing timers.
The same native handle and owner keep an active gesture on its chosen backend
through a transient snapshot gap. A new weapon or lost capability resets it.
The host's magazine/chamber state restores unfinished right-pistol interactions
after a lifecycle boundary when fresh model contact regions are available.

Retained-left single pistols now use the same host transaction path for their
left-hand magazine/chamber. Their free right grip grabs ammo and operates the
slide. Right jump and flashlight keep their existing functions. Grip reservation
prevents that hand from also requesting weapon pickup. Unsupported host state or
native bindings retain conventional reload. The source-side controller and
mechanical-frame routing is tested offline; headset/model and co-op acceptance
are pending.

Dual pistols intentionally use their shared native button reload. Both hands
hold a gun, so separate magazine and slide gestures are outside the current
interaction design. Persistent magazine objects and native headset/co-op
acceptance remain unfinished.

An optional client prediction-command hook scopes local pistol bullet effects,
hand haptics and predicted hit feedback to the command Source is simulating.
An older left-hand command cannot borrow the latest right-hand ray. Cached
commands also require the current player/weapon, native weapon handle serial
and a bounded capture age. With
the hook active, missing or replaced poses preserve the native bullet ray and
omit additional haptics and predicted hit attribution. An unsupported hook retains the earlier latest-pose
fallback. Native prediction replay and first-shot weapon changes need gameplay
acceptance; independent reloads remain unfinished.
First-person bone retargeting does not yet separate remote world-model
pistols. Body inventory follows tracked head position and yaw; full body IK
and character-specific avatar fitting remain separate work.

When the optional native command-execution hook is available, physical reload
suppression follows the command being simulated, including commands without a
shot. Grip and Object Pull commands retain the preceding simulated policy only
for the same weapon and edict generation. Replayed commands keep their original
decision. Switching or replacing weapons during a command cannot apply the old
weapon's gate to the new weapon.
Without that optional hook, the existing decoded-command fallback remains.
Native timing, weapon switches and two-client reloads still need gameplay tests.

Grip release places the item at the tracked hand with no throwing impulse.
Trigger-release throwing remains separate. Grip pickup uses nearby native
item selection with surface contact and controller aim on the server.
Physical reloads send a per-player reload gate; validated gun and shotgun
Reload hooks prevent the native automatic reload while the physical system
owns it. The gate is lifted for its explicit backend reload, and native dual
reload remains available. Both client and server need this build.

Free right gloves use the mirrored left glove calibration by default, rather
than the held viewmodel hand calibration. Advanced overrides are
`VrHandsRightFreePoseOffsetMeters` and `VrHandsRightFreePoseRotationOffsetDeg`.
Hand skeleton initialization and palette failures are independent per hand.
Unavailable skeletons and palette failures retry, while another valid hand can
draw. If a preferred native viewmodel hand capture is unavailable, a valid
controller-backed skeletal palette uses scene placement until capture returns.
Successful skeletal API calls still require valid curls, bone transforms and
finite skinning palettes. Bone reads are staged before replacing retained data;
failed or partial reads cannot supply a fresh fallback. Each free-hand palette
carries its validated action-frame identity. Native viewmodel hand captures
retain their independent placement and recovery path.
Partial renderer retries reuse an existing declaration or shader while creating
only missing resources, so repeated shader failures do not leak those objects.
Full-arm readiness requires advancing, fresh tracking samples for 200 ms;
repeated draws or animation-freeze generations cannot establish tracking.
Physical controller identity prefers valid SteamVR hand assignments. If one is
missing, a unique connected controller with the corresponding explicit driver
role hint supplies that hand. Ambiguous hints, disconnected devices and invalid
poses remain unavailable. Left-handed gameplay swaps the resolved pair once;
anatomical model hands continue to use their physical controller.

Inactive, failed or nonfinite mirrored stick reads do not replace keyboard
movement with zero axes. Custom held keys and commands retain their original
release target and reconcile current action state even when a release edge was
missed. Menus, input suppression, detached scout mode and disabled VR release
those inputs. Shared bindings transfer ownership without releasing the target
between buttons. These software checks still require headset verification.

Action-frame availability is separate from pose availability. A failed SteamVR
action update rejects stale button, stick, grip and finger-curl data, releases
owned held commands, and cancels teleport targeting. The first recovered frame
can resume held controls without replaying one-shot presses. Pending ammo
requests still poll the actual owner and weapon; input loss cannot clear their
required chamber cycle or start another gesture.
Specific hand inputs also validate the connected resolved controller and source
path. Unknown curl or grip samples retain held ammunition while replies settle;
they do not count as a release. Grip discontinuities persist across skipped
native commands, requiring a deliberate fresh grip cycle before a drop or pickup.
Walk and Turn stick axes must stay within their supported range before turning
or scope adjustment consumes them.

Tracking starts from explicit zero pose fields. Invalid or nonfinite samples
retain the last usable position and angles while clearing linear and angular
velocity. Queued rendering rejects those samples before converting a fresh
output, retaining its existing camera fallback until usable tracking returns.
Raw tracking rotations also reject collapsed, mirrored, scaled or sheared
bases while accepting small rounding errors. This check applies to tracked
device poses; scaled skeletal and weapon transforms retain their own rules.

Fresh magazines and shells use a repeatable controller-local grip with
`ManualReloadMagazineHandRotationOffsetDeg` applied at pickup. The grip point
lands at the palm; the gun and wrist orientation at the body anchor do not set
the held item's rotation. Input contact and queued rendering use the same grip
transform, including with non-centered replacement-model bounds. Invalid anchor
or controller data cannot enter the held-ammo state. Pulling an attached old
magazine retains its original relative pose until it is removed.

Body previews and waiting ammunition share the torso's upright frame. Rotating
the gun does not rotate a spare magazine on the waist. The frame preserves model
handedness, while the existing body-relative positions and saved offsets remain.

With body inventory and the first-person body enabled, a valid rendered pelvis
now supplies the inventory frame's height and torso yaw. This fits waist ammo
and holsters to the selected character instead of using one eye-to-waist distance.
Saved body/slot offsets still adjust that frame. The same relative sample is used
for input contact and queued rendering; a stick turn between samples is applied
once. Missing/malformed rigs, stale samples, observer state or incapacitation
use the fixed tracked-head frame. The optional `BodyGripInventoryUseModelPelvis`
setting defaults to true and can disable fitting. This is an inventory fit to
the rendered skeleton; collision and full physical body IK remain unfinished.

Hand, body and tracking defaults explicitly initialize Source vectors/angles.
Their SDK default constructors leave values uninitialized, so empty braces do
not guarantee a zero calibration. These fixes prevent undefined default offsets;
they do not establish the cause of earlier headset or multiplayer failures.

An observed empty chamber stays empty until its required manual bolt cycle is completed.
Manual pump movement uses a closed rest pose rebased onto the current gun;
moving the pump hand does not steer the gun's aim. Native automatic pump
sounds are muted, while the sound from an actual manual stroke is allowed.

With empty-clip auto reload suppression enabled, chamber history is retained per
weapon across ordinary inventory switches. Inserting a magazine or receiving a
predicted clip refill does not clear an observed empty chamber. A completed
slide/bolt cycle clears it. Native entity handles distinguish replacement guns
at a reused address. When the handle cannot be resolved, history is limited to
the current draw. Death, incapacitation, disabled physical reload and player
replacement clear ownership; native dual-pistol reload remains separate.

A shotgun with a chambered round can fire after a partial tube reload. Holding
the next shell, cycling the action, an unfinished backend reload or a pending
shell ammunition update blocks firing. Inserting a shell into an empty gun still
requires a cycle. Shell insertion waits for a readable native ammo slot instead
of guessing a reserve or overlapping a previous insertion. The short settlement
gate clears on local server completion, session reset or a bounded timeout. Releasing a
partly moved pump retains its position; the grab region follows that position.

Grip pickup and release take priority over a simultaneous Object Pull packet,
including the controller pose associated with the grip action. Object Pull
and transient grip actions preserve the current reload gate for the same
weapon, rather than treating a missing reload impulse as permission to reload.
A different player entity starts a fresh reload command sequence. Controller
profile validation covers all default bindings and the optional Quest preset;
Knuckles and Cosmos keep analog turning without undeclared legacy turn actions.
Ordinary weapon swaps discard stale magazine and hand poses but retain the
same player's outstanding pump cycles. Death, incapacitation, disabled VR
interaction, and player replacement clear that cycling ownership.
Before a queued inventory drop executes, the server rechecks the living owner,
weapon identity, active inventory selection and bounded command age. A weapon
switch or an expired request cancels the transaction before inventory changes.

Physical melee uses the tracked weapon hand without an attack-button press on
a VR-aware server. Detection removes HMD translation and includes wrist rotation
as estimated weapon-tip motion. Rest arms the next swing; tracking loss, menus,
incapacitation and weapon/player changes require resting again before damage.
Each real input command retains its hand pose and swing state, including when
it is sent again as a backup. Stale tracking samples cannot start a swing.

The server selects the newest decoded pose in each packet and rejects commands
already used for a collision sweep. Weapon changes and long command gaps rebase
the sweep rather than tracing between unrelated poses. Collision calls still
use the native melee damage path and its per-swing hit list. This retains native
damage/gore handling, but blade alignment, hit feedback and multiplayer results
need a headset test. This is a directional native melee sweep, not a complete
rigid-body blade simulation. Very short gestures between network packets still
need evaluation. With `VrHandsDebugLog`, `[VR][PhysicalMelee]` identifies the
server player and command that started a traced swing.

For melee testing, release the attack trigger, rest the weapon hand briefly,
then try horizontal, overhead, straight and wrist-led swings. Walking with the
hand still should not attack. Check the same zombie across one continuous swing,
then rest and swing again. Repeat after a weapon swap, opening a menu, revival
and a map change. Compare damage and gore on the host and a joining VR client.

For reload testing, empty a pistol, insert a magazine, switch away before cycling
the slide, then draw it again and finish the cycle. Repeat with a different gun
of the same type. With a loaded shotgun, insert one shell into a partly filled
tube, release the shell grip and try firing after settlement. Check loading an
empty gun and cycling it after one shell. Release and re-grab the pump midway
through both strokes. These changes still need headset validation.

For a headset test, start with both grips released, then grip each gun. Fire
with each trigger and check its aim. Release one grip, confirm that a world
pistol falls at that hand and the other pistol stays held, then grip the
dropped pistol to recover the pair. Repeat with the other hand, both grips
released together, partially spent clips, a weapon switch, and revival.
Check `vrmod_log.txt` for `[VR][PistolDetach]` and `[VR][PistolPickup]` events
with `VrHandsDebugLog` enabled.

`[VR][GripDrop][input]` reports final eligibility, release state, and the number
of extra camera samples. These samples keep the live grip latch, rather than
resetting it simply because their command number is zero. `blockers` is a
hexadecimal mask: 0x1 inactive VR, 0x2 outside a game, 0x4 unreadable player,
0x8 dead/observer/wrong team, 0x10 incapacitated, 0x20 frozen, 0x40 paused,
0x80 menu cursor, 0x100 mouse mode, 0x200 teleport scout, 0x400 suppressed input,
0x800 control not ready, 0x1000 special infected control, 0x2000 mounted weapon,
0x8000 missing inventory weapon, and 0x10000 unreadable session. Server support
and the inventory-drop backend are logged separately.
