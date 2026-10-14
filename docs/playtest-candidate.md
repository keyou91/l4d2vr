# Review candidate and gameplay acceptance

Build a candidate from the current source with Python 3, Git, CMake, Visual
Studio 2022 C++ Build Tools and a Windows SDK:

```powershell
python -B -X utf8 tools/build_candidate.py
```

The script creates a new directory under TEMP, snapshots the source, builds the
pinned MinHook dependency and D3DX proxy, rebuilds the native x86 DLL and pose
server, and runs all interaction suites in Release and Debug. It prints the
candidate directory. Logs and a source/artifact hash receipt stay with it. It
does not modify the checkout or install files into a game. MinHook preparation
requires access to its pinned public GitHub commit. The native CI job uses this
same script; its separate interaction job runs the complete test suite.

Use the candidate with an existing L4D2VR installation prepared according to
the root README. The candidate contains replacement binaries and review notes;
it does not bundle Valve models, Workshop assets or the optional VR Gloves pack.
D3DX9_43.dll and the existing OpenVR runtime remain required. Keep the game
closed, back up its root d3d9.dll and symbols, then copy the candidate replacements.
Preserve saved vr/config.txt, config.sample and controller bindings. Do not copy
a default configuration over a working personal setup.

The runtime-reference directory contains the matching action manifest, stock
profiles and default settings for review. New manual actions require that manifest.
Back up and update vr/SteamVRActionManifest/action_manifest.json when adopting
them, then merge or explicitly select the wanted controller profile. Keep custom
bindings and saved settings; the reference directory is not an automatic installer.

For a friend-hosted check, use matching client builds. Dedicated-server pose
relay installation is separate: l4d2vr_pose_server.dll and its VDF belong in the
server's left4dead2/addons directory. Pose relay support alone does not provide
authoritative physical ammunition transactions.

Start with the existing controller profile. For the optional Quest manual
reload layout, follow manual-reload-input.md. Dual pistols intentionally use
shared native button reload. Unsupported ammunition hosts retain native reload.
Installed two-sided materials can be retained; the material tool derives them
locally from the owner's game assets when needed.

Startup reports the failed manifest/control lookup and retries incomplete input
acquisition once per second. CreateMove discovery has a bounded retry window
after initialization. A ready input acquisition appears in vrmod_log.txt as
"[VR][Input] acquisition ready". Preserve that line and any preceding failures
when reporting tracked controllers with no working game input.

Optional glove assets are resolved from the owner's SteamVR runtime with UTF8
paths. A missing or corrupt side retries once per second while a healthy side
remains loaded. If neither asset is usable, native hands are the temporary
fallback; restoring an asset resumes requested gloves. Native mode does not
require these external GLBs. Keep the saved hand mode and HideArms preference.

## First session

Record the candidate commit and DLL hash from verification.json. Start SteamVR
with both controllers tracked, launch the game through Steam, and create a local
campaign. Confirm the new vrmod_log.txt session belongs to this candidate.

1. Confirm both visible hands, left-stick movement, turning, pickup and release.
   Repeat after pausing and changing weapons. Recenter once.
2. Fire and reload a single pistol, Magnum and each SMG. On a supported host,
   eject the magazine, grab a fresh one, insert it and cycle an empty chamber.
   A tactical change retains the existing chambered round.
3. Insert individual shells in each shotgun. Check an empty pump/chrome gun
   accepts several shells before pumping; auto/SPAS empty reloads need the bolt.
   Switch away before completing the action, then return and finish it.
4. Reload dual pistols with the shared button. Check native reload on an
   unsupported host, and with manual reload or VR hands disabled. Check the
   grenade launcher's native reload and the M60's stock ammunition behavior.
5. Look through hunting, military, SG552, AWP and Scout optics at two zoom levels.
   Check each eye, aperture placement, focus, depth occlusion and reticle alignment.
6. Cancel a grenade throw by switching slots, then perform a normal throw and
   an intentional inventory drop. Check no wrong weapon moves or duplicate item
   appears. Exercise tracking loss while holding a magazine or slide; release and
   grab again after recovery.

Then check two matching clients: remote hand/body motion, late join, owner
replacement, manual ammo settlement and capability timeout/fallback. Keep the
log and any crash dump with the exact candidate identity.

Compilation, offline fixtures and CI establish only the behavior they exercise.
The candidate remains pending gameplay acceptance until this session passes.
The earlier pipe-bomb crash cause and release-only rendering parity remain open.
