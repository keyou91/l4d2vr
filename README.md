# L4D2VR Dedicated Server Plugin

This branch keeps the full L4D2VR source tree, but the Visual Studio solution is scoped
to building the dedicated server plugin. It does not build or install the old client-side
`d3d9.dll` proxy as part of this server target.

## What It Builds

- `l4d2vr_server.dll`: a Source server plugin loaded by the dedicated server.
- `l4d2vr_server.so`: the Linux Source server plugin build for `server_srv.so`.
- `l4d2vr_server.vdf`: the plugin descriptor for `left4dead2/addons`.

The plugin waits for `server.dll`, resolves the dedicated-server signatures it needs,
and installs MinHook hooks for the server-side VR usercmd path.

On Linux the plugin waits for `server_srv.so`, resolves the needed local ELF symbols
from the loaded module, and installs equivalent x86 inline hooks. It does not use the
client `d3d9.dll` proxy path.

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

## Server Config

On plugin load, `l4d2vr_server_log.txt` is recreated instead of appended. The plugin
also creates `l4d2vr_server_config.txt` if it does not already exist. Edit it and
restart the server, or change map, to reload the values.

Available settings:

```text
TeleportEnabled=true
TeleportBlockWhileControlled=true
TeleportBlockWhileIncapacitated=true
TeleportMaxDistanceUnits=2500
# Optional alternative; if set after TeleportMaxDistanceUnits it overrides it.
# TeleportMaxDistanceMeters=20
TeleportCooldownSeconds=1.0

MeleeSwingEnabled=true
MeleeSwingBlockWhileControlled=true
MeleeSwingBlockWhileIncapacitated=true
MeleeSwingCooldownSeconds=0.35
MeleeSwingBurstWindowSeconds=2.0
MeleeSwingBurstMax=4
```

Distances are Source units. Rough reference: 43.2 units is about 1 meter.

## Build Windows

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

## Build Linux

Build on a Linux host that has a 32-bit toolchain available:

```bash
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install g++-multilib
./build_release_linux.sh
```

The Linux build writes:

```text
Release/linux/l4d2vr_server.so
Release/linux/l4d2vr_server.vdf
```

The current Linux resolver targets the 32-bit L4D2 dedicated server binaries:

```text
left4dead2/bin/server_srv.so
bin/engine_srv.so
```

The `server_srv.so` symbol table is used for the server hooks; the engine interfaces
come from Source's plugin loader.

## Install Manually

### Windows

Copy these files into the server's `left4dead2\addons` directory:

```text
l4d2vr_server.dll
l4d2vr_server.vdf
```

### Linux

Copy these files into the server's `left4dead2/addons` directory:

```text
l4d2vr_server.so
l4d2vr_server.vdf
```

The VDF points Source's server plugin loader at `../left4dead2/addons/l4d2vr_server`.
