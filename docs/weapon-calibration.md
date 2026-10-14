# Weapon and bullet calibration

Adapted from [PR #404](https://github.com/keyou91/l4d2vr/pull/404),
by Renan Fagundes, source commit `2790c14a39f33c63a30ecc74f9790e26510a513b`.

The advanced settings panel exposes two separate adjustments:

- `ViewmodelPoseOffset` and `ViewmodelPoseRotationOffsetDeg` move or rotate
  the rendered weapon in addition to its existing per-weapon adjustments.
- `BulletAimOffset` and `BulletAimRotationPitchDeg`, `BulletAimRotationYawDeg`,
  `BulletAimRotationRollDeg` adjust the shot ray independently of weapon placement.

`WeaponAimYawOffsetDeg` and `WeaponAimRollOffsetDeg` extend the existing logical
weapon-hand pitch calibration. All added settings default to zero. Position
values use Source units; bullet position components are forward, right and up
in the calibrated basis. Rotations use degrees. Start with small adjustments.

Bullet calibration applies to VR firearms, including the new physical optic mode.
Legacy floating scopes retain their scope-camera ray. Mouse mode, mounted weapons, melee, throwable items, idle
hand poses and object-pull commands retain their existing pose paths. The
preserved dual-pistol experiment keeps its selected hand's shot pose; it does
not yet apply these bullet offsets to its independent shot paths.

For calibrated shots, client effects and VR-aware server input use the corrected
ray even when the aim line is hidden. AutoGrip's corrected ray is reused when
available. With zero bullet offsets, the existing bullet origin and angle
representation are retained. Invalid numeric values are discarded, and saved
calibration survives startup with an older or forcing `config.sample`.

Offline regression tests cover coordinate axes, rotation, default preservation,
invalid input, shot-path exclusions and sample-default policy. They do not
establish headset alignment or multiplayer hit registration. The remaining
checks are recorded in [the backlog](vr-backlog.md).
