# L4D2VR Dedicated Server Plugin

This branch keeps the full L4D2VR source tree, but the Visual Studio solution is scoped
to building the dedicated server plugin. It does not build or install the old client-side
`d3d9.dll` proxy as part of this server target.

## What It Builds

- `l4d2vr_server.dll`: a Source server plugin loaded by the dedicated server.
- `l4d2vr_server.vdf`: the plugin descriptor for `left4dead2/addons`.

The plugin waits for `server.dll`, resolves the dedicated-server signatures it needs,
and installs MinHook hooks for the server-side VR usercmd path.

## Runtime Behavior

The dedicated server plugin keeps the server-side pieces needed by VR clients:

- decodes the VR pose data packed into `CUserCmd`;
- uses controller origin/angles for server bullet direction;
- preserves server melee swing traces from controller motion;
- routes use, throwables, and mounted weapon aiming through controller pose when available.
- accepts dedicated-server extra usercmd payloads for teleport targets and 1:1 roomscale
  movement deltas;
- validates teleport landings with `EngineTraceServer003`, then moves the server player
  with the server `CBaseEntity::SetOrigin` path;
- applies roomscale server movement directly when the hull sweep is clear, and falls
  back to Source's normal movement command path when blocked.

Non-VR clients continue through the original server code path.

## Build

Open a Visual Studio Developer PowerShell, then run:

```powershell
.\build_release_x86.ps1
```

or:

```cmd
build_release_x86.cmd
```

The solution intentionally exposes only x86 configurations because Left 4 Dead 2's
dedicated server target is 32-bit. A successful Release build writes:

```text
D:\l4d2vr\Release\l4d2vr_server.dll
```

If the local Left 4 Dead 2 Dedicated Server install exists at the configured path, the
post-build step copies both files to:

```text
D:\SteamLibrary\steamapps\common\Left 4 Dead 2 Dedicated Server\left4dead2\addons\l4d2vr_server.dll
D:\SteamLibrary\steamapps\common\Left 4 Dead 2 Dedicated Server\left4dead2\addons\l4d2vr_server.vdf
```

## Install Manually

Copy these files into the server's `left4dead2\addons` directory:

```text
l4d2vr_server.dll
l4d2vr_server.vdf
```

The VDF points Source's server plugin loader at `../left4dead2/addons/l4d2vr_server`.
