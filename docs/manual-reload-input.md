# Optional manual reload input

The existing controller bindings remain the default. The optional Quest profile
uses left grip for off-hand support, magazines and the slide; right A for magazine
release; right stick press for jump; and left X for the flashlight.

To use it, back up `vr/SteamVRActionManifest/bindings_oculus_touch.json` in the game
installation and replace it with `bindings_oculus_touch_manual_reload.json` from
this repository. Restart SteamVR and the game so the binding is reloaded. Enable
these VR settings:

- `MagazineInteractionEnabled=true`
- `MagazineInteractionUseButtonGripInput=true`
- `MagazineInteractionSeparateButtonInput=false`
- `MagazineReleaseButtonRequired=true`
- `MagazineInteractionQuickReloadMode=false`

The profile is for right-handed Quest controllers. Left-handed users can assign
the optional Off Hand Grip and Magazine Release actions to their preferred hands
in SteamVR. Other controller profiles can bind those actions without adopting
this layout.

In button mode, running out of ammunition does not eject a detachable magazine.
Magazine Release can eject a loaded or empty magazine. Off-hand grip then handles
the loose magazine and the existing slide interaction. A loaded magazine change
retains one existing chambered round; an empty pistol does not gain a round from
its infinite reserve.

On a host with a negotiated physical reload backend, ordinary Reload button or
gesture input cannot start a stock reload. This applies to the single pistol,
Magnum, SMGs, rifles and shotguns, including before the first magazine ejection.
Unsupported hosts keep conventional reload; dual pistols and the grenade
launcher retain their explicit native fallback.

If manual reload or VR hands are turned off, Magazine Release becomes a native
reload request for a live reloadable gun. Its brief input pulse remains tied to
the same owner and native weapon handle, so a weapon switch cannot inherit it.

For an empty single pistol, Magnum or SMG, press Magazine Release, grab a fresh
magazine from the body ammo position with the off-hand grip, guide it into the
well, then grip and pull the slide or charging handle before releasing it.
Shotguns accept individual shells at their loading port; pump shotguns also need
the pump cycle. An empty pump or chrome shotgun accepts multiple shells before
you choose to grab the pump and chamber one. A dry pump stroke keeps its chamber
empty. A normal shot with shells remaining still requires the usual pump stroke.
A transaction advances only after host acknowledgement and native
ammo replication. Lost replies retry the same request up to three times, without
applying its ammo transfer twice. Timeout or a changed weapon cancels the pending
gesture.

Physical reload gestures require fresh, finite headset and controller poses.
Tracking loss pauses new contact and movement; already submitted ammo operations
can still settle from the host reply and native replication. If tracking drops
while holding a slide or pump, release the grip and grab it again after tracking
returns so a changed controller pose cannot count as a stroke. Native weapon
handles also fence reload state when a replacement gun reuses an entity address.
With retained chamber history enabled, switching away from a loaded gun before
cycling does not complete its action. Returning restores the existing cycle and
blocks firing while its current grab region is being acquired. Auto shotgun and
SPAS can finish an empty reload with their already loaded shell after a switch;
they do not need another reserve shell to make the bolt available.

Magazine, slide and support-grip contact boxes reject nonfinite or reversed
bounds and degenerate transforms. Invalid contact cannot count as a grab.
An unusable bolt pull calibration uses a physical default of 0.055 meters for
slides or 0.09 meters for pump/chrome shotguns. Valid per-gun calibration remains
unchanged; recovery still requires the actual pull and return stroke.

In optional quick reload mode, a picked-up shell stays attached while its
request is pending or receives a retryable rejection. In physical grip mode,
releasing a pending shell remains a release even if the grip is pressed again.
A retained rejected shell must leave and re-enter the loading port to retry;
the host still decides whether an existing round can be transferred.

Dual pistols intentionally use a button reload because both hands hold a gun.
With this Quest profile, right A reloads both pistols together using the native
animation. `DualPistolsNativeReloadFallbackEnabled=true` is the default. Manual
magazine and slide gestures remain available for a single pistol in either hand
on a supported host. Native button reload, hand tracking without gloves and
recovery after incapacitation still need in-game verification. The existing VR
Gloves workaround remains available.

An older workshop `config.sample` must not erase the saved optional release
setting. A commented sample default seeds a missing setting; an active sample
assignment can still override a saved value under the repository's current
startup behavior.
Saved ScopePhysicalLensEnabled and ScopeBaseMagnification also survive removal
of unknown options against an older sample.
