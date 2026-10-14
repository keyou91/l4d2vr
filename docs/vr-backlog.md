# VR interaction backlog

These are local work items for the contribution branch. They are not public
GitHub issues. Existing upstream reports are linked to avoid duplicate reports.
User observations describe earlier local builds and are not claims that a
specific commit caused a failure.

## VR-001: Recover manual reload and support-hand state

Priority: high. Reload can stop working after weapon changes or incapacitation.
VR Gloves currently provide the usable hand-tracking path.

Implemented offline: interaction ownership is checked before the grip-release
wait. Weapon/player changes, incapacitation, spectator state and input-mode
changes clear held-hand state, stale pose regions and old ammo updates. Normal
reload completion retains its pending settlement. Gameplay recovery remains
to be verified in a headset.

Chamber history now retains an unfinished cycle across weapon switches when
empty auto reload is suppressed. Native handle serials distinguish replacement
entities; unavailable handle data falls back to the current draw only.

- Audit stale weapon, magazine, bolt and support-grip ownership on weapon swap,
  downing, revival, death and map change.
- Check a full and empty pistol, Magnum and SMG for ten consecutive reloads,
  including same-type weapon swaps and downing/revival. Grip release must cancel
  ownership without leaving the hand attached.
- Verify slide travel, release and chamber state against actual ammunition.

Related: [native hands #406](https://github.com/keyou91/l4d2vr/issues/406),
[intermittent reload #343](https://github.com/keyou91/l4d2vr/issues/343).

## VR-002: Make grip pickup and release work with native weapons

Priority: high. Right-grip release did not drop weapons in the earlier prototype.

October 4 headset feedback confirms grip release drops weapons in `cd6e3a9`.
Table pickup, drop placement, free-hand orientation, ammo placement and reload
timing produced additional failures. `51239cf` addresses those paths but still
needs headset verification. Later offline checks protect grip packets from
Object Pull conflicts and retain per-player reload ownership across them.

- Trace contact pickup and the client/server drop request through native weapon
  ownership. Distinguish close-range grip pickup from remote Grip Pull.
- Holding grip near a reachable weapon must pick it up. Releasing grip must
  create a visible, usable dropped weapon when this optional mode is enabled.
- Check two clients and prevent a released grip from dropping a subsequently
  selected weapon. Keep the existing control preset available.

## VR-003: Verify independent dual-pistol aiming and shared reload

Priority: high. Earlier dual pistols followed only the right controller and
aimed both shots from that hand; the prototype also lost reload functionality.

Confirmed single-to-pair pickup contents and executed native shot deltas now
feed a per-player/per-weapon ammunition ledger. Known partitions preserve the
selected pistol's ammunition when splitting; unknown partial pairs and native
ammunition changes fall back to a conservative balanced split. Retained single
pistols keep their hand identity for another pickup. Dual pistols intentionally
keep the shared native button reload requested for this control scheme.
Independent dual magazine gestures are deferred. Larger capacity variants and
gameplay acceptance remain unfinished.

An optional native firing-boundary adapter now captures actual gun identity
after weapon selection, correlating bullet events with clip consumption and
retaining firing-event ordinals within a command. Hand intent is no longer
discarded merely because packet decoding sees the previous weapon. Native
timing, effects and client prediction still need headset/multiplayer acceptance.

A versioned host-to-client pistol-ammo feed now carries known per-hand contents
with the native shared clip. Current weapon handle/serial, session, message and
command ordering, owner changes, native replica matching and expiry constrain
client acceptance. Its first consumer publishes coherent hand counts from input
processing. A bounded client prediction journal now accounts for native clip
consumption by command/firing ordinal and removes acknowledged events. Replay
projects only the preceding events at that simulation point. Bullet/delta
mismatches invalidate attribution until a newer authoritative baseline arrives.
Known empty hands are gated in trigger routing and at both native gun-firing
boundaries after protocol-v3 acknowledgement. Unknown data, missing bindings,
old clients/hosts and non-default infinite-ammo modes keep native behavior.
Independent chamber/magazine operations remain unfinished. Actual delivery,
prediction timing, empty-hand behavior and replication need matching builds.

The host ammo ledger now contains an independent magazine/chamber policy for
each hand. Ejection keeps only that pistol's chambered round; insertion preserves
an empty chamber until cycling; reinsertion restores the removed magazine's
contents without consuming reserve. Shots auto feed only while the magazine is
attached, and the retained single pistol preserves its chamber and catch state.
Failed native writers leave both hands unchanged. Replacing a cached magazine
reports its released rounds for the later object handoff. Native/external ammo
changes revoke inferred physical state. This policy is tested offline;
native gameplay and hand-control acceptance are still pending.
Host transactions and client settlement are now connected to this policy.
Queued requests wait for native command execution, then validate the current
entity/session/clip/reserve before committing through the verified native writer.
Deduplication and hand-specific rate limits protect both hands. Protocol v3
replicates physical, magazine and chamber masks plus the applied reload sequence;
client settlement also requires matching native ammo, including intervening shots.
Counts and chamber gates are published coherently in one atomic sample. Firing
and host stock reload now respect physical ownership. Single right-hand pistol
ejection, retained/fresh magazine insertion and slide gestures now use that API
when a supported host state and the optional native bindings are available.
An empty reload waits for acknowledged chambering after the slide stroke;
tactical insertion retains its chamber. Pending gestures retain their backend
and native handle through transient snapshot gaps. Current native ammo modes,
unsupported capacities and dual pistols still use their existing fallbacks.
Retained-left single gestures are connected below. Dual pistols retain the
intentional shared button reload. Persistent magazine-object ownership remains
unfinished.

Retained-left pistol contact geometry now uses the same controller frame as the
visible weapon. Retargeting precedes magazine/slide and calibration publication;
stock orphaned clip/bullet/slide bones follow the weapon too. Snapshots carry
their gun hand, reject contacts from the previous hand, and rebase against that
hand's current anchor. Bolt snapshots now publish an anchor as well. Tracking
freshness and connected poses constrain the shared controller frame rather than
using attack-action availability as a rendering gate. Offline geometry checks
cover a freely moving right hand, left-gun motion/rotation and hand changes.
Headset, replacement-model and queued-render alignment remain unverified.

The reload-support role now also selects finger-curl input, anatomical network
grip curls, glove/native curl overrides, calibrated held-ammo anchoring, waist
side and interaction/weapon haptics. Glove grip state is a physical-hand mask,
so switching the support hand invalidates cached palettes and moves the correct
hand into the viewmodel projection layer. The existing left-handed layout keeps
its default physical-side mapping. Reload-support input ownership is coordinated
with pickup as described below.

Grip pickup now respects the active reload-support hand. A reserved hand cannot
issue contact pickup or inherit a late pickup's held grip, and remains gated
until it releases before a new pickup. Menu, unavailable-action and eligibility
boundaries also require release. Native pistol ownership still follows actual
inventory changes; the gun hand's release/drop behavior is preserved.

Retained-left single pistols now route the existing magazine/slide gestures
through the left-hand host transaction path. The free right grip supplies reload
interaction input; right jump and flashlight retain their gameplay functions.
Controller positions, probe angles, gun-relative movement, chamber masks and
reply settlement follow the retained hand. A matching host state and optional
native bindings replace the shared reload pulse; unsupported states retain the
native fallback. Hidden-magazine contact publication uses a retargeted copy, and
the slide's runtime sign hint is converted back to the native pre-retarget frame.
Empty/tactical, finite/infinite and retained-magazine sequences are tested for
both single hands. Native hand/model alignment, queued rendering, downing and
matching co-op acceptance still need headset testing. Dual pistols intentionally
use their shared native button reload. Persistent magazine objects remain unfinished.

Native pickup hand assignment now remaps verified single-pistol physical state
to the receiving hand. Attached-magazine and chamber bits plus removed contents
follow the same gun when its hand changes; the previous hand's bits are cleared.
Different native ammo, capacity, entity serial or owner scope clears that state
instead of retaining stale masks. Both transfer directions and loaded/empty
chambers are checked offline. World-entity state transfer across players and
pair-join preservation still need implementation and native acceptance.

Client native reload prediction is now guarded by separately validated pistol
reload and reload-completion bindings. Fresh matching physical ownership blocks
refill attempts and clears reload flags without changing clip/reserve counters
or shared firing cadence. Current player/weapon, session, age and ordinary ammo
mode constrain the guard. Transaction initiation requires both optional client
bindings. Native timing, interruption/recovery and remote acceptance still need
testing before the physical gestures are enabled.

The optional client prediction-command hook now selects the command-specific
hand ray for native local bullet effects and hit-feedback attribution, with
owner/weapon/handle/age guards and exception-safe scope restoration. Missing scoped
poses retain the native ray rather than adopting a newer hand's shot; haptics
and extra hit attribution are omitted when the hand is unknown. This provides the client execution context
used by the per-hand ammo prediction journal. Native timing, replay,
first-shot weapon selection and feedback still need headset acceptance.

The server now has an optional native execution hook and per-command hand/pose
history, so firing uses the simulated command rather than the latest decoded
packet. Owner/edict serial, age, duplicates and ring rollover are covered offline.
This is the execution prerequisite for per-hand ammo ownership; native shots,
effects and actual independent magazines/chambers still need implementation and
gameplay acceptance.

Physical reload suppression now consumes the same immutable command history.
Transient grip/pull input preserves the last simulated gate for the same weapon;
later decoded packets cannot grant or suppress an earlier command's reload.
Replay decisions, player/weapon edict replacement and weapon changes are checked
offline. Native execution timing and multiplayer reloads remain unverified.

Further offline fixes remove the dual-shot early return that skipped firing
haptics and predicted hit feedback. The chosen ray is retained through the
shared feedback path, and haptics select its firing hand. Live cached poses are
scoped to the player/weapon with a bounded age. Exact prediction replay timing,
native muzzle effects, animations and headset feedback remain to be verified.

- Complete independent hand tracking, shot aim, effects and haptics. Preserve
  the selected hand on retransmitted input commands.
- Define magazine and chamber handling for two pistols while respecting the
  native shared ammunition model. Capacity must remain correct below 15 rounds
  and with supported replacement models.
- Test alternating and simultaneous triggers, loaded/empty reloads, swapping
  between single and dual pistols, and recovery after revival.

The [30-round capacity fix #371](https://github.com/keyou91/l4d2vr/pull/371)
is already in the base and does not implement these independent interactions.

## VR-004: Finish optional manual pump cycling

Priority: medium. Shell insertion worked in an earlier build, while the shotgun
cycled automatically and the fore-end grab did not work reliably.

- Couple backward and forward fore-end travel to the native firing and chamber
  state. Prevent another shot until cycling completes in manual mode.
- Check partial travel, grip release halfway through, interrupted shell loading,
  empty ammunition, and weapon changes. Automatic shotguns must retain their
  automatic cycle.

Related: [shotgun empty reload #385](https://github.com/keyou91/l4d2vr/issues/385).

Further offline fixes allow a chambered shotgun to fire after a partial top-up,
protect pending shell settlement from firing or a second insertion, and move
the pump grab region with the visible fore-end. Unknown shell reserve data no
longer causes a guessed clip increase. These paths await headset testing.

## VR-005: Stabilize body inventory and magazine poses

Priority: high. Earlier body anchors showed debug spheres and magazines moved
unpredictably. Picking up a magazine must give it a consistent hand pose.

- Use a consistent body transform for visible ammo, grab regions and holsters.
  Check head turning, room-scale movement, snap turning and recentering.
- Calibrate magazine orientation and insertion per weapon without preserving an
  accidental wrist angle from pickup. Keep debug geometry disabled by default.
- Treat full-body IK as a separate implementation task, including shoulder,
  elbow and torso behavior. Body visibility alone is not a physical body.

Further offline fixes give fresh ammunition a calibrated controller-local
orientation instead of capturing an accidental pickup angle. Its model grip
point remains at the palm across wrist motion in the shared input/render path.
Invalid capture leaves the item at the body anchor and does not take the hand.
Source vector/angle calibration defaults are initialized explicitly. Reload,
shell-port alignment and glove/native-hand visuals still need a headset test.

Rendered first-person pelvis sampling now supplies character-specific waist
height and torso yaw to the shared inventory frame, retaining saved offsets.
Samples are owner-scoped, bounded and short-lived; missing rigs retain the fixed
head-relative fallback. Input/render translation and inter-frame turns have
offline coverage. Character appearance, crouching/leaning, reach and body
collision still require headset work; this does not complete physical body IK.

Related: [magazine pose #392](https://github.com/keyou91/l4d2vr/issues/392),
[body IK #393](https://github.com/keyou91/l4d2vr/issues/393).

## VR-006: Verify multiplayer pose replication and physical melee

Priority: high. The user could not see the host's VR movements.

Implemented offline: another player's command cannot consume the host's queued
manual-reload ammo update. This is separate from the remaining pose-replication
checks below.

Further offline changes retain physical melee pose and swing intent per command,
detect wrist-led swings, remove walking velocity, and reject repeated collision
commands. Sweep history is scoped to a living owner and melee weapon. Native
collision/gore calls are retained; headset and two-client outcomes remain untested.

Remote pose history now uses a dedicated entity/userID identity. A main-thread
refresh invalidates missing or replaced slot occupants without native identity
queries from queued rendering. A recovered tracker starts from its latest valid
sample; continuously valid trackers retain interpolation and body-yaw rebasing.
This prevents interpolation from invalid payload components and reuse of a
departed player's pose. Transport and visible two-client movement still need
gameplay acceptance.

- Verify the committed listen-server acknowledgement with two VR clients,
  including a late join, compatibility fallback and map change.
- Check both hands and melee sweeps on the other client. Physical swings must
  use native damage and gore without requiring a button in the selected mode.
- Keep dedicated-server relay capability distinct from VR user-command support.

Related: [remote tracking #408](https://github.com/keyou91/l4d2vr/issues/408).

## VR-007: Investigate crashes with evidence

Priority: high. A friend crashed using a pipe bomb. Another player's upstream
report associates crashes with Grip Pull; neither establishes the friend's cause.

Pending projectile throws, inventory drops and detached carry items now use
separate consumers. A slot change alone cannot authorize dropped-entity movement.
Deferred calls validate native owner and source serials; spawned physics props
are paired with the exact source invocation and validated before use. Missing
owner/edict bindings disable the custom drop path and retain native behavior.
These changes address routing and entity lifetime defects found in the code;
they do not establish the cause of the earlier pipe-bomb crash.

- Obtain a matching crash dump and logs before attributing the pipe-bomb crash.
- Audit object-pull entity lifetime and client/server transitions. Compare
  enabled and disabled Grip Pull using a controlled future gameplay test.
- Preserve the separately diagnosed shadow-renderer workaround while investigating
  these other failures. Avoid changing unrelated settings during a crash test.

Related: [Grip Pull crash report #400](https://github.com/keyou91/l4d2vr/issues/400).

Related campaign-load report: [Pimax Dream Air with sboys3 #410](https://github.com/keyou91/l4d2vr/issues/410).
Its attached log reaches local survivor rendering but has no exception address
or stack trace. Several final render/cache messages belong to release code that
is absent from the public source, so this branch cannot establish binary parity
or infer a crash cause from the last message.

The controller-tip driver boundary now validates interfaces, device indices,
input errors, model-property errors, bounded string termination and finite tip
transforms. Missing optional controller components fall back to the controller
origin. Overlay intersections also check pose-array bounds and connected poses.
Offline driver mocks cover these failures and recovery. These changes fix a
concrete uninitialized-string path, but are not a confirmed resolution of #410.
Matching crash-dump and Pimax campaign-load evidence remain required.

## VR-008: Complete pistol, Magnum and ammunition models

Priority: medium. Rotated stock weapons expose missing or culled surfaces.

- Distinguish back-face culling from genuinely missing mesh geometry. The local
  material tool addresses existing surfaces only.
- Check the full weapon, magazine, slide and chamber from all angles, and align
  interaction regions to the visible parts. Use locally derived game assets;
  do not bundle proprietary assets in the contribution.

## VR-009: Verify the integrated calibration in VR

Priority: high before release. The PR integration is subject to offline checks;
headset alignment and multiplayer outcomes still require validation.

- Compare zero offsets with the previous committed behavior. Check a small
  position offset and each rotation with the aim line visible and hidden.
- Check client and host impacts, backup input commands, AutoGrip, scopes, mouse
  mode, left-handed mode and the dual-pistol prototype.
- Confirm saved settings persist on restart with an older sample config.

Reference: [calibration PR #404](https://github.com/keyou91/l4d2vr/pull/404).

## VR-010: Add authoritative remote physical ammunition requests

Priority: high for friend-hosted play. Physical ammo needs its own negotiated
host transaction; pose and physical-control acknowledgements alone cannot
establish authoritative ammunition on the friend's host.

Implemented offline on October 8: a separate one-shell protocol negotiates a
connection token, validates the owned weapon's network serial and recent input,
reads native server capacity/reserve, and transfers one existing round. The
guest waits for a matching result and Source ammo replication. Duplicate requests
reuse their result; stale or changed ammo/ownership is rejected. Unsupported or
timed-out leases restore conventional reload. Friend-hosted delivery and native
replication still need testing.

Detachable-magazine transactions are also implemented offline: explicit eject
retains at most one chambered round, and insert transfers available native
reserve up to native capacity. The host reads the engine's infinite-ammo rule;
it does not assume all pistols are infinite. Duplicate requests cannot transfer
ammo twice, and insertion requires a preceding accepted ejection. Source ammo
replication and the matching result must both arrive before the guest advances.
Empty reloads still require the slide; a late slide region cannot automatically
complete that remote reload. Native dual pistols keep their shared reload
fallback. Magazine protocol version 2 implements grip catch/reinsert with the
actual removed rounds, preserving partial and empty contents without spending
reserve. Withdrawal from the well is required before return. Contact and server
accounting are covered offline; headset feel remains unverified. Floor pickup,
persistent magazine storage and transfer to another player remain unfinished.

The supported listen-host now uses the same magazine ledger as the guest,
instead of filling a predicted clip to its default maximum. Tests include a
partial reserve and spending the retained chamber between eject/insert.
Backend changes fence old queued updates. Single-magazine local prediction and
its queued clip/reserve writers have been removed. Unsupported magazine backends
use native reload, including the Magazine Release button, while retaining support
grip and clearing virtual ammo/chamber state. The local shotgun writer and
reserve-offset search have also been retired. Both host and guest load one
native-confirmed shell per insertion. Unsupported/expired shell support restores
stock reload, pumping and audio; changing the capability fences current hand
state. Local command delivery, actual replication and physical cycling still
require gameplay verification.

- Define a separately negotiated request/acknowledgement for physical shell and
  magazine completion, scoped to the player's actual owned weapon and entity
  serial. Reject duplicate, stale and incompatible requests.
- Read clip capacity and reserve on the server. Consume only confirmed physical
  insertions and replicate the result without client-side reserve-slot guesses.
- Preserve conventional reload on unsupported servers. Do not enable remote
  shell loading by treating pose or grip acknowledgement as ammo support.
- Test the guest and host separately, including backup commands, map/weapon
  changes, interruption, no reserve ammo and a late join.
