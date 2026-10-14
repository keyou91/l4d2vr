# Contribution verification

## Draft scope and evidence

This contribution includes the full experimental interaction branch: input and
reload lifecycle recovery, optional magazine release, native grip pickup/drop,
body inventory anchors, hand/ammo alignment, manual bolt and pump cycling,
independent dual-pistol tracking/aim and native detachment, physical melee,
weapon/bullet calibration adapted from PR #404, and separately negotiated
friend-hosted shell and detachable-magazine transactions. See `physical-interactions.md`,
`manual-reload-input.md`, `weapon-calibration.md` and `vr-backlog.md` for their
controls, limitations and remaining tests.

The review-candidate recipe rebuilds the Windows x86 DLL and pose server with
v143, then runs the complete CTest suite in both Release and Debug in TEMP.
Its verification.json records the exact source and artifact hashes and suite
counts. See playtest-candidate.md for the command, runtime prerequisites and
gameplay acceptance. Seven native ammo signatures matched uniquely against
the locally installed server binary. These are offline checks; a different game
binary must satisfy the guarded bindings before physical ammo support is used.

Manual throwing and grip-release dropping remain disabled in repository
defaults; mounted physical scopes are enabled. Saved user configuration is
preserved. The material tool derives overrides locally and
does not add Valve weapon models/materials or compiled game binaries to this PR.

This is an umbrella draft for maintainer feedback and can be divided into
smaller contributions. Headset and two-client acceptance remain pending;
compilation and state-machine tests do not establish gameplay success.

`VR interaction tests` runs the offline CTest suites on Windows x86 for relevant
pushes and pull requests, including drafts. Its build output is confined to the
runner temporary directory. It uses a read-only token and does not load L4D2 or
publish game binaries. Fork pull-request runs can require maintainer approval.
The existing manually triggered DLL-build workflow is separate.

Run the small offline tests in a temporary directory. They do not launch L4D2,
load its DLL, or modify the game installation.

```powershell
$testRoot = Join-Path $env:TEMP ('l4d2vr-tests-' + [guid]::NewGuid().ToString('N'))
cmake -S tests -B $testRoot -G 'Visual Studio 17 2022' -A Win32
cmake --build $testRoot --config Release --clean-first
ctest --test-dir $testRoot -C Release --output-on-failure
cmake --build $testRoot --config Debug
ctest --test-dir $testRoot -C Debug --output-on-failure --no-tests=error
```

Python 3 is needed for the controller-profile and material tests. CMake reports
whether it found an interpreter. The C++ suites remain available without Python.

For a DLL build, use the repository's x86 build instructions. Its normal build
has a post-build copy into a game installation. For offline verification, copy
the source to a temporary directory, remove the `PostBuildEvent` from the staged
project, and build that copy. Keep output and intermediate directories there.

Use `Rebuild` for the staged DLL solution. MSBuild can omit dependencies under
TEMP from its incremental tracking, so a header or `.inl` edit may otherwise
leave old object code in a seemingly passing build. The final checks verify
staged source hashes and use a full DLL/pose-server rebuild. The Debug hand test enables
the SDK's `VECTOR_PARANOIA` behavior, which seeds unspecified vector/angle
values with NaN and detects accidental assumptions about zero defaults.

The candidate recipe builds pinned MinHook 1.3.4 source with the same v143
toolchain because the bundled archive has incompatible link-time code generation.
It also builds the tracked D3DX proxy source. Both replacements remain in TEMP;
the repository dependency archives and installed game are unchanged by the build.
The native CI job uses this recipe and records fixed artifact paths and hashes.

## Local pistol material preparation

The tool derives two material overrides from an installed copy of L4D2. It does
not redistribute Valve materials, edit the game installation, or launch it.
Without `--output`, it writes into a new temporary directory.

```powershell
python tools/prepare_vr_materials.py --vpk 'PATH_TO_GAME/left4dead2/pak01_dir.vpk'
```

It verifies VPK size and CRC, preserves shader parameters, refuses unresolved
patch materials, and refuses to overwrite existing output. The overrides make
existing pistol and Deagle surfaces visible from both sides. Missing geometry
still requires model work; this tool does not reconstruct it.

## Remaining gameplay checks

- Two VR clients on a friend-hosted local server: join after the initial fallback
  window, confirm VR movement and pose relay, then test melee and projectiles.
- Single detachable-magazine weapons: loaded and empty button release, insertion,
  and the existing slide interaction, with legacy mode also checked.
- Same-type weapon changes and incapacitation/revival: confirm support grip and
  manual reload state recover.
- Headset visuals from different pistol/Deagle angles: confirm the local material
  overrides behave correctly with the active models and skins.
- Grip pickup/drop and body inventory: table contact, world placement, hand/ammo
  poses, snap turning, looking down and tracking-loss recovery.
- Fresh ammo grip: repeat pickup at different wrist/gun angles, check the
  configured grip rotation, palm contact and the same pose in queued rendering.
- Dual pistols: each hand's aim, alternating/simultaneous triggers, detachment,
  pickup and the native shared-ammo reload fallback.
- Pistol ammunition: join unequal clips, fire from one hand, and drop either
  pistol. Retained plus dropped ammunition must match the native shared total,
  including native reloads, weapon replacement, owner changes and unknown pairs.
- Single right-pistol gestures: eject with Magazine Release, catch/reinsert an
  existing magazine or insert a fresh one, then cycle an empty chamber with the
  slide. A loaded chamber needs no cycle. Verify matching host acknowledgement
  and native clip/reserve before completion, retained rounds, rejected insertion
  while grip is held, missing/late slide regions, snapshot gaps, timeout recovery
  and downing/weapon-switch restoration. Repeat with a retained-left pistol using
  right grip, including native gun drop while the support hand holds ammo. Jump
  and flashlight must keep working. Dual pistols keep the shared native reload
  fallback; unsupported single-hand hosts/states retain native reload.
- Native firing boundary: switch to pistols and immediately fire either hand;
  verify multi-shot frames, effects, native clip replication and optional-hook
  fallback. Runtime per-hand ammo delivery and replication acceptance remain
  unfinished.
- Pistol-ammo feed: matching capability/session, loaded/empty and unequal clips,
  snapshot-before/after-native-replication delivery, weapon/player changes,
  menu/paused recovery, heartbeat/timeout and an older unsupported host.
- Client pistol prediction: replay an older left shot after a newer right shot;
  check ray, haptic hand and hit-feedback command, owner/weapon replacement,
  capture expiry, non-prediction events and the optional-hook fallback.
- Client pistol reload guard: stock reload and empty auto reload, a timed reload
  started before physical ownership, magazine ejection/insertion, loaded other
  hand, expiry/revocation, weapon/player changes, downing and native fallback.
  Confirm that the guard never writes clip/reserve or resets firing cooldowns.
- Per-hand ammo prediction: acknowledgement and older-protocol fallback; right
  empty/left loaded and the reverse; simultaneous triggers; native reload and
  down/revive; server acknowledgement arriving during replay; same-command
  multiple firing events; native infinite-ammo modes; clip replication and
  missing/unexpected native bullet events. Confirm no duplicate consumption or
  guessed clip/reserve writes on either player.
- Native pistol execution: command-to-hand association across delayed commands,
  backup packets, weapon changes, two players and the optional-hook fallback.
- Executed reload policy: packet batching, grip/pull commands, ordinary reload
  permission, weapon switching, revival and the optional-hook fallback.
- Pump shotguns: partial tube top-up, empty loading, rear/forward strokes,
  continuous support grip and re-grabbing a partially moved fore-end.
- Physical melee: trigger released, wrist-led and translated swings, native
  damage/gore, per-swing hit limits and duplicate input commands.
- Remote shells: matching host/guest builds, real clip/reserve replication,
  stale/duplicate requests, timeout/older-host fallback and map changes.
- Remote magazines: loaded and empty ejection, partial reserve, native finite
  and infinite pistol ammo, insertion then slide cycling, weapon changes and
  both orders of result/replicated-ammo delivery.
- Magazine fallback: unsupported and timed-out hosts must use stock ammo and
  reload, including Magazine Release while the support grip is held. Check menu,
  weapon/player changes and restoration of the negotiated physical path.
- Retained magazine: grip catch at the well, withdrawal and return, partial and
  empty magazines, unchanged reserve, spending the chamber while it is removed,
  release/cancellation, incompatible capacity and a failed native insertion.

Offline builds and tests do not confirm those headset or multiplayer results.
Full body IK, independent dual-pistol magazines/chambers, complete missing mesh
geometry, dropped-magazine floor recovery/storage and dedicated-server physical ammo remain
unfinished. The reported pipe-bomb crash and missing remote movement have not
been proven fixed. These limitations are part of the draft review scope.

## Client interface map-load crash

A local map-loading crash exposed an incorrect client-interface declaration in
receive-table discovery. It called slot 9 of VClient016, a HUD input method,
where the installed Windows x86 client exposes its class-list getter at slot 7.
The unexpected argument convention corrupted the input-hook stack. The same
declaration exists in upstream v0.8.2 source; the ladder-state lookup exposed it
during this map-loading test. This does not establish the cause of issue #410
or the behavior of its published release binary.

Discovery now checks that slot 7 is inside client.dll and matches the native
MOV EAX,[address]; RET getter, then reads the module-owned class-list pointer.
It does not invoke an unchecked virtual method. An unsupported getter or an
out-of-module pointer returns an unavailable offset. Regression checks include
the installed getter bytes, an ASLR relocation, the old HUD method bytes,
truncated code and invalid or overflowing image bounds. Native map-loading and
headset acceptance must be recorded separately from these software checks.
## Native entity identity and physical capability

After the receive-interface fix, a solo hotel-map test reached a controllable
survivor, but physical interaction negotiation remained unavailable. Live
inspection showed native player index 1 and a 32-bit allocation serial of 396 at
edict byte 4. The copied newer-Source layout treated that serial as a cached
index, so it rejected this player and kept physical drop, dual-hand firing and
manual reload/pump behavior in native fallback.

The main SDK declarations now use L4D2's 16-byte edict with a full serial and no
cached index. Shared server identity checks call the verified native entindex
function, verify the reciprocal entity/edict pointer and retain the full serial.
The relay, pistol accounting, reload gates, drops and ammo transactions use those
checks. The optional pose plugin derives indices from ServerActivate's edict
array with the native stride. Tests cover the observed survivor, array bounds
and alignment, invalid/free entities and distinct generations above 16 bits.
Negotiation and interaction acceptance still require a matching native test.
The layout follows the [L4D2 SDK edict header](https://github.com/alliedmodders/hl2sdk/blob/l4d2/public/edict.h).
## Server-to-client reply transport

Native negotiation progressed after correcting entity identity, but the host's
capability and result commands appeared as unknown server commands. The built-in
relay used IServerPluginHelpers::ClientCommand, which executes a fake client
command on the server. Replies now use the verified VEngineServer022 client
transport, with the correct x86 variadic calling convention and a fixed printf
format. Its module bounds and native argument setup are checked before calling;
unsupported code retains fallback. Tests check the observed native signature,
argument convention and preservation of literal percent characters.
The helper's fake-command semantics are demonstrated by
[SourceMod's queue implementation](https://github.com/alliedmodders/sourcemod/blob/master/core/HalfLife2.cpp).
Native delivery and physical-control acceptance remain separate checks.
## Preserve local builds during testing

An accepted Workshop update replaced the custom DLL and controller bindings
mid-test. Create `VR/disable_workshop_updates.txt` in the game directory to pin
a locally compiled test build. Its presence skips the Workshop update prompt and
helper launch; an unreadable pin policy also skips that update check. Remove the
file when intentionally returning to Workshop-managed updates. The opt-out does
not unsubscribe the Workshop item or change Steam's content management.
## Held-grip pickup contact query

Grip pickup no longer relies only on FindUseEntity publishing a contact during
ordinary use/highlight evaluation. Each held gameplay grip queries a small native
entity overlap around its controller, checks the existing item category and hand
contact rules, and publishes a hand-specific contact offer. Equipment with a
networked owner is excluded. Left-hand pickup remains restricted to pistols.
The existing release reservation and pickup latch create IN_USE once; the native
server remains responsible for finding and authorizing the actual pickup.
This path still requires headset contact and multiplayer acceptance.

## Pump support and throwable hand continuity

Requiring a pump cycle no longer switches the glove to its free-hand pose.
The held support wrist and fingers receive the pump's travel even when the
native rig places that wrist beside the weapon branch. Each bone moves once;
the receiver, gun hand and elbow retain their existing transforms. Releasing
grip removes that wrist from pump travel. Native reload suppression also leaves
continuous pump support available. Parent-chain regression checks cover sibling
and nested wrists, release and malformed cycles.

With manual throw enabled, the throwing glove follows its tracked controller
through the throwable's preparation and release instead of taking the native
throw animation's wrist pose. This requires headset verification after pipe,
molotov and bile throws and a switch back to a firearm.

Bounded debug logs now distinguish physical ammo backends from native fallback,
shell-to-port contact, request deferral, server acknowledgement and replicated
ammo confirmation. The reported automatic reload and failed shell insertion
remain pending native reproduction in the current build; these diagnostics do
not establish that those failures are fixed.

## Command-scoped pistol tracer origins

Native client inspection showed that the bullet helper fetches WeaponShootPosition
through EyePosition as a second origin even after FireTerrorBullets receives the
VR ray. The existing client EyePosition hook now supplies the firing pistol's
origin only inside that exact predicted bullet call, for that player. The normal
view, movement and use paths retain their existing scopes. Native-scope cleanup
restores the prior origin on normal, nested and exception exits.

The weapon tracer handler also rebinds its effect to a native muzzle attachment.
An optional, signature-verified FX_Tracer hook removes that binding only for the
matching command's player, weapon or owned viewmodel. It preserves Source's
endpoint, material, velocity, sound and particle selection. Unrelated effects
and unavailable native bindings retain their normal path. Native viewmodel
ownership comes from receive properties, with unavailable metadata rejected.

The final independent-pistol draw captures each gun's authored muzzle attachment
relative to its own controller. The executing command rebases the selected muzzle
to that command's hand pose. A mutex protects the pair and weapon identity; stale,
non-finite, hidden and mismatched owner/weapon/serial/mode samples are rejected.
If a current attachment is unavailable, the validated command's controller origin
remains available. Bounded PistolFX logs report the hand and attachment availability.
Geometry and ownership regression checks do not prove native visual acceptance.

Muzzle flashes use the native animation parity/event path, which remains separate
from this tracer change and still needs a command-to-event binding. Independent
dual-pistol button reload, left-pistol grip alignment and multiplayer acceptance
remain pending in native gameplay.

## Pistol firing animation selection

Stock dual-pistol sequences identify right primary fire, left secondary fire and
separate last-round animations. The native selector chooses between these from
shared clip parity, which can disagree with the physical trigger that fired.
An optional signature-verified selector hook now chooses the executing command's
hand inside the existing native gun-fire scope. Available predicted per-hand ammo
chooses that gun's last-round animation; infinite ammo retains loaded animations.
Unknown ammo preserves native last-round information. Single pistols and unrelated
or unscoped activity requests retain their normal selector result.

This supplies the correct firing animation and its stock muzzle-flash event.
The event's native attachment positioning remains a separate pending change;
this selector alone does not establish that flashes follow the retargeted gun.
Native ABI, stock sequence metadata and per-hand selection checks are software
evidence. Headset, prediction replay and multiplayer acceptance remain pending.

## HMD flashlight mount

`FlashlightHmdForwardOffsetMeters` moves an HMD-follow light in front of the eyes
to avoid placing it inside the local character's head. Its default is 0.18m;
zero restores the previous viewpoint mount. CreateMove checks a small hull from
the head toward the mount against world geometry and movable props, skipping the
owner and held weapon. A wall shortens that displacement with a small clearance;
a solid start or unavailable trace leaves it at the viewpoint.

The light callbacks consume one matching origin/basis sample. They perform no
new collision queries on render workers. Owner, timestamp, scale and live offset
changes reject stale samples; a fresh tracked viewpoint provides the fallback.
Existing firearm-follow preference, colour, range and shadow settings remain
unchanged. Geometry and sample-lifetime checks cover software behavior. Head-shadow
visibility, walls, leaning, turns, character changes and down/revive still need
native headset testing. This does not establish full body/head alignment.

## Left-pistol glove grip position

The left gun's calibrated transfer now aligns the primary gun's authored wrist
grip point with the currently skinned left glove wrist. It uses the same glove
offset, rotation and model scale as rendering, including physical-hand selection
in left-handed mode. The correction changes translation while preserving the
calibrated gun rotation. That avoids treating a glove art correction as gun aim.
Missing native wrist bones, unloaded or stale glove data and implausible grip
displacements retain the earlier controller transfer. Muzzle capture and contact
publication see the corrected final gun bones. This still needs headset alignment,
roll, handedness, scale and native-glove fallback acceptance.

## Physical melee native origins and result type

The manual sweep already invokes the game's melee collision routine without an
attack button. That routine fetches the survivor's WeaponShootPosition, which
can bypass the generic eye-position override previously used by the sweep.
An optional, signature-verified shoot-position hook now supplies each interpolated
hand origin only during that survivor's native collision call. Eye getters share
the same owner-specific scope. Native-scope cleanup restores it on nested and
exception exits; the sweep no longer temporarily overwrites shared player pose.

Client and server collision routines return a boolean in AL. Their hook type now
matches that native result rather than interpreting the whole EAX register as a
hit. The server collision signature now identifies its full entry instead of a
short fragment before it. The native hit counter remains at verified offset 17F0.
Existing native damage, hit limits, gore and duplicate-command guards remain in
use. Binding and scope checks are software evidence; actual melee contact, damage,
gore, reach, weapon variants and friend-hosted acceptance remain unverified.

## Recovering shell request delivery

A pending shell insertion can resend its identical request up to three times,
at least500ms apart, before the existing2500ms timeout. The token, sequence,
weapon handle, command and ammo baseline remain unchanged. The server's existing
duplicate-result cache answers an applied retry without spending another round.
An acknowledged transaction waits for native replication without resending.
Owner/weapon/generation changes cancel retries, and command sending occurs outside
the request mutex to allow synchronous replies.

While the grip remains held, the pending shell stays visible and follows the hand.
Definitive stale-baseline, rate-limit and no-space/ammo rejections retain that
held shell. The player withdraws from the port and returns before retrying, avoiding
automatic request spam. Applied transactions consume the visual shell after
acknowledgement and replication. Timeout or ambiguous backend failures do not
restore a possibly consumed round. Unit checks cover lost replies, single native
consumption, retry limits, cancellation and contact rearming. Native multiplayer
delivery, shell geometry and the reported automatic reload still need testing.

## Recovering pistol magazine request delivery

Pending pistol ejection, insertion, reinsertion and slide-cycle transactions can
resend the identical request up to three times, at least 500 ms apart, within the
existing 2500 ms timeout. The host's duplicate-result cache returns the settled
transaction without changing ammo again. A valid reply stops retries while the
client waits for the matching native ammo state. Sending occurs outside the
request mutex, allowing synchronous replies.

Owner, weapon handle and single/dual-mode changes cancel pending requests. Paused
or suppressed input does not resend, and disabling physical reload cancels it.
Tests drop replies after host application for both hands, single and dual ammo
states, empty and tactical reloads, and every magazine action. They verify one
host mutation, unchanged opposite-hand ammo, replication waits, retry limits,
timeout, invalid replies and cancellation. The wire version is unchanged.
This strengthens the existing single-pistol gesture path and the dual-capable
transaction backend. Dual pistols intentionally use a shared native button
reload; native headset and matching-build multiplayer acceptance remain pending.
