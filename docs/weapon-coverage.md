# Stock weapon coverage and physical optics

This implementation covers the 19 stock firearm inventory types and the separate
dual-pistol viewmodel. A local asset audit checks all 20 MDL/VVD/VTX sets, their
checksums, LOD0 triangle indices, finite vertices and UVs, skin weights, bone
hierarchies, material references and texture headers. The current installation
passes those checks, including 46 referenced texture assets. Neither inspected
addon archive overrides weapon model geometry.

A second local audit checks the corresponding 20 world-model sets and compiles
the native magazine/bolt selectors against all 20 held rigs. Every selected held
bolt or pump has visible vertices weighted to it. Pump and chrome shotguns select
their fore-end slide; the auto shotgun and SPAS select their actual bolt rather
than a pump helper. Saved bone overrides still take precedence.

Every stock gun model now has an explicit inventory identity. A recently published
magazine/socket region from another stock weapon is rejected during a switch.
Unknown replacement paths retain the existing owner and freshness checks; they
are not assigned a guessed stock identity.

| Weapons | Aiming and model path | Reload/cycle path |
| --- | --- | --- |
| Pistol | Tracked gun, native stock mesh | Host pistol magazine/chamber transactions |
| Dual pistols | Independent tracked hands and native two-gun mesh | Intentional shared native button reload; enabled by default |
| Magnum | Tracked gun, native stock mesh | Host detachable-magazine transactions |
| Uzi, silenced SMG, MP5 | Tracked gun and support grip | Host detachable-magazine transactions |
| M16, AK47, Desert rifle | Tracked gun and support grip | Host detachable-magazine transactions; native firing cadence/burst behavior |
| SG552 | Tracked gun, support grip, mounted physical optic | Host detachable-magazine transactions |
| Hunting rifle, military sniper | Tracked gun, support grip, mounted physical optic | Host detachable-magazine transactions |
| AWP, Scout | Tracked gun, support grip, mounted physical optic | Host detachable-magazine transactions; per-shot bolt animation remains native |
| Pump and chrome shotguns | Tracked gun, retained support/pump grip | Host individual-shell insertion and manual pump |
| Auto shotgun, SPAS | Tracked gun and support grip | Host individual-shell insertion; manual bolt after an empty reload |
| Grenade launcher | Tracked gun and support grip | Explicit native single-round reload fallback; no synthetic detachable magazine |
| M60 | Tracked gun and support grip | Native stock ammo behavior; physical magazine requests reject this type |

Unsupported hosts retain conventional reload. Native clip capacity, reserve,
owner/weapon identity and authoritative replication govern negotiated reloads.
The negotiated physical backend also owns ordinary reload input. The client
blocks native single-pistol reload prediction before the first ejection when a
fresh host baseline matches the held hand. Magazine requests retry the same
identity at most three times while waiting for a reply; acknowledgements stop
retries even if ammo replication is still pending.
The existing gesture, request, prediction and ammo-conservation suites cover those
generic paths. They do not establish that every contact volume feels correct in VR.
The grenade launcher explicitly enters the native reload fallback even when
magazine-grip mode is enabled, so its release/reload action is not swallowed.

## Physical scopes

`ScopeEnabled=true` and `ScopePhysicalLensEnabled=true` select the new first-person
optic path. Stock hunting, military, SG552, AWP and Scout lenses are measured from
the native mesh and matched by model path and checksum. The final animated gun
bones place a circular lens in each eye render. It uses normal depth testing and
Source's render queue, so the optic is part of the gun's render rather than a
SteamVR overlay above the scene. The local gun's separate world model is excluded
from the scope camera image; native gun shadows are preserved.
The mounted lens follows the held weapon's near-clipping policy, including when
it is inside the scene near plane. It retains world depth testing and restores
the captured D3D pipeline state after drawing.
The fixed-function lens pass explicitly selects its vertex UVs, disables texture
transforms and geometry blending, and sets solid fill and its own clipping state.
State-setting failures stop the affected draw. Cleanup restores the render target
before the captured pipeline and viewport, preserving the native scissor state.

Each eye is evaluated separately against the optic axis, pupil and 3.5 to 25 cm
eye-relief interval. Off-axis or backwards viewing darkens the aperture. Looking
at a physical scoped gun uses two eye passes even if mono-eye copying is enabled,
so the other eye cannot borrow the aligned eye's magnified image. Looking
through the aligned optic requests a separate world render. The render FOV follows
`2 * atan(aperture radius / (eye relief * magnification))`, preserving the intended
angular magnification as the eye moves. `ScopeBaseMagnification` defaults to 2x;
the existing per-weapon FOV control and Use plus vertical left-stick adjustment
increase it, bounded to 16x. Horizontal size adjustment and manual floating-lens
placement do not resize or detach a physical optic.
Requests outside the supported render FOV range fail closed rather than showing
an image labeled with a magnification the projection cannot produce. Nonfinite
clip planes and queued camera values cannot enter the scope render.

The cached native mount is rebased onto the current controller pose for camera
updates; each queued eye draw keeps its own captured lens pose. Lens and image
samples expire after 100 ms. Tracking loss, empty hands, incapacitation and model
checksum mismatches disable the applicable path. No physical-scope activation
changes controller sensitivity, weapon aim, bullet origin or bullet calibration.
Mouse and third-person modes retain their existing scope behavior.

Physical lens and image snapshots also carry the native weapon handle, owner and
draw generation. Switching between guns of the same type or holstering/redrawing
a gun invalidates earlier snapshots. The scope camera's position, angles, FOV and
magnification are captured together. Image readiness is published after its Source
render commands in queue order, and a changed render-resource epoch rejects it.
The raw and processed images retain separate stamps, so an older processed lens
cannot borrow the readiness of a newer raw render or a different zoom setting.
Changing the gun, zoom or render-resource epoch requests a fresh RTT immediately,
while unchanged valid images keep the normal HMD refresh pacing.
HMD and gun-hand tracking must be fresh and finite; losing only the support hand
does not disable a one-handed optic.

The optics tests cover magnification ratios, eye relief, pupil rejection, pose
translation/rotation, model identity, non-finite input and stale samples. Native
depth, UV orientation, aperture placement, occlusion, reticle alignment and timing
still need headset tests on all five guns. Replacement models with different
checksums require measured optic profiles; this code does not place a guessed
floating lens on them.

## Materials and mesh limits

From the repository root, prepare all stock weapon material variants with:

```powershell
python tools/prepare_vr_materials.py --game "A:\SteamLibrary\steamapps\common\Left 4 Dead 2" --all-weapons --output "$env:TEMP\l4d2vr-material-review"
```

The helper resolves base, DLC and update assets, prepares 32 two-sided material
variants, and preserves their existing textures and shader parameters. Arms are
excluded. Existing output is never replaced. Preparation does not install assets,
launch the game or bundle Valve model/texture files in the source contribution.
Review/install the derived material files through the local addon workflow.

Two-sided materials expose existing reverse faces. They do not create absent
polygons. The topology report records open edges per material mesh; seams between
materials, moving parts, magazine wells and barrel openings can all produce such
edges. This is not a watertightness or visual-completeness certificate. Missing
exterior faces require inspecting and rebuilding the relevant mesh; those repairs
and full replacement models have not been implemented by this audit.

The current stock pistol and Magnum world models contain magazine and bolt
bones. They can supply articulated geometry, but their authored dimensions differ
from the held models even after muzzle alignment. A full overlap would duplicate
surfaces and moving parts. No combined replacement model is installed; any hybrid
needs measured alignment, selective surface replacement and preserved skinning.
