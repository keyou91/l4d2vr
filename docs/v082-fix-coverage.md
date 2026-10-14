# v0.8.2 fix coverage

The [v0.8.2 release](https://github.com/keyou91/l4d2vr/releases/tag/V.0.8.2)
targets public commit `7c61019`, which is already this contribution's base.
The published package identifies itself internally as 1.1.7 and contains frame
generation and cache reclamation code absent from that public source checkout.
This branch therefore does not claim complete parity with the release binary.
The work below implements or checks the documented behavior in our source;
it is not a source-level cherry-pick of unavailable release changes.

| Release change | Contribution status |
|---|---|
| Native reload interrupting manual reload | Existing authoritative ammo transactions and command-scoped native reload gates address this path. Handset and host/guest acceptance remain pending. |
| Support grip without enabling manual reload | Existing input processing calls support-grip handling independently of the manual-reload switch. Weapon-box publication also accepts active VR hands. Needs a headset check with manual reload disabled. |
| Climbing while looking straight ahead | Added direction-button intent from analog movement while the dynamically resolved native move type says the player is attached to a ladder. Jump, use, reload and attack buttons are preserved. Native/gameplay acceptance remains pending. |
| Aim guide during weapon calibration | Calibration now temporarily permits the D3D aim guide even when ordinary aim-line or laser-sight visibility is disabled. Queued rendering reads an atomic calibration snapshot. Saved settings are preserved. |
| Bullet/muzzle alignment | Existing independent weapon/viewmodel/bullet calibration is adapted from PR #404. Replacement-model and headset alignment remain pending. |
| Multicore shadow/water reflections | Existing render-target isolation is present, but parity with the new release fix is not established. Needs render-capture/headset evidence and corresponding upstream source. |
| New X crosshair and new defaults | The released X-crosshair implementation is absent from the public source. This branch preserves saved controls/settings and does not claim that renderer feature. |
| Frame generation and cache reclamation | The corresponding implementation is absent from the tagged public source. These features have not been ported. |

Offline builds and tests establish the tested policy and compilation only.
They do not establish visual or gameplay equivalence with the released binary.
