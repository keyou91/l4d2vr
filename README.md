<div align="center">
  
  # L4D2 VR Prototype
  ### Use this mod at your own risk of getting VAC banned.<br>Use the `-insecure` launch option to help protect yourself. (Also contains lots of flashing lights)
</div>

## Video demo
[<img width="640" height="360" alt="image" src="https://github.com/user-attachments/assets/63a16c57-621e-40fc-a581-5f64eafa9633" />](https://www.youtube.com/watch?v=J9vw8VXJWZM)

## Things that work
* Singleplayer and [multiplayer](#how-to-play-multiplayer)
* 6DoF VR view
* Full controllers two-handed interaction
* Desktop mirror
* [Workshop content](https://steamcommunity.com/workshop/browse?appid=550)
* [Reshard](#reshard)
* Multi-core rendering support. **(Enable in [VR Config Menu](#vr-config-menu), not the video setting menu)**
* Anti-aliasing support. **(Enable in [VR Config Menu](#vr-config-menu), not the video setting menu)**
  
## Things that need fixing
* Interactions and throwables require you to aim with your face. (Except for servers that do not support non‑VR)

## How to play multiplayer
* You can join any server to play, but if the server wasn't created by VR some VR-exclusive features. 
* Versus works, but it's barely been tested.

## How to use
1. Download [L4D2VR.zip](https://github.com/liu547161153/l4d2vr/releases) and extract the files to your Left 4 Dead 2 directory `steamapps/common/Left 4 Dead 2`
2. Launch SteamVR, then launch Left 4 Dead 2 with these [launch options](https://help.steampowered.com/en/faqs/view/7D01-D2DD-D75E-2955): <br>`-heapsize 524288 -processheap -high -novid -windowed`
    - If you use a desktop client resolution of 2k or higher, add `-bigfonts` to the launch options to make in-game text larger; otherwise, the text on the HUD will be very small.
3. Join or create your campaign and enjoy the game.
4. To recenter the camera height, press down on the left stick. To see the HUD, aim the left controller up or down.

## VR Config Menu
1. Start the game and get to the level.
2. Press the left controller **Y** button to open the pause menu.
3. The **VR Config** button will appear at the top right of the pause menu.
4. Click to open the L4D2VR Config Panel.
5. Change the settings and press the Save button to save, or press the Reload button to undo all changes.

## Reshard
1. Download [ReShade](https://reshade.me/).
2. Select the rendering API Left 4 Dead 2 uses: Vulkan + OpenXR.
> Optional: Select effects to install: [VRToolkit](https://github.com/retroluxfilm/reshade-vrtoolkit) by retroluxfilm.

## Troubleshooting
### If the game isn't loading in VR:
* Disable SteamVR theater in [SteamVR settings](https://img.itch.zone/aW1nLzE0MTMyNTY4LnBuZw==/original/pKSXNc.png)

### If the game shows "Failed to create D3D device!":
* L4D2VR uses a custom `d3d9.dll` based on DXVK. Even though the error says D3D, the failure is often the Vulkan backend failing to initialize.
* Update the GPU driver from NVIDIA, AMD, or Intel. Windows Update drivers are often too old for DXVK 2.x.
* Make sure the GPU and driver support Vulkan 1.3. Very old GPUs and some virtual/remote desktop display adapters will not work.
* Do not launch the Steam "Vulkan" launch option and do not add `-vulkan`. Launch the normal DirectX 9 game with the L4D2VR files installed.
* Remove forced display launch options such as `-w`, `-h`, `-fullscreen`, or unusual refresh-rate settings, then try windowed/default video settings.
* Check `left4dead2_d3d9.log` next to `left4dead2.exe`; lines such as "No adapters found" or "A Vulkan 1.3 capable driver is required" indicate a driver/GPU support problem.

### If the game is stuttering: 
* Steam Settings -> Shader Pre-Caching -> Allow background processing of Vulkan shaders.

### If the game is crashing:
* Lowering video settings.
* Disabling all add-ons, then Steam > Left 4 Dead 2 > Verifying integrity of game files.
* Re-installing the game.

## Build instructions
1. `git clone --recurse-submodules https://github.com/liu547161153/l4d2vr.git`
2. Initialize submodules:
   ```powershell
   git submodule update --init --recursive
   ```
3. Install [DirectX SDK](https://www.microsoft.com/en-us/download/details.aspx?id=6812): request **DirectX Headers and Libs**
4. Run the fixed build script (locks target to `Release|x86`):
   ```powershell
   .\build_release_x86.ps1
   ```
   or:
   ```cmd
   build_release_x86.cmd
   ```
5. (Optional/manual) Open l4d2vr.sln and build `Release|x86`.

> Note: After building, it will attempt to copy the new d3d9.dll to `steamapps/common/Left 4 Dead 2`

## Dev note: VTable lookup
For quick vtable inspection, use [Asherkin's VTable Dumper](https://asherkin.github.io/vtable/).
It can be used for `server.dll`-side symbols and also for `engine.dll` targets (drop the binary and search symbol names like `bob`, `viewmodel`, etc.).

## Utilizes code from
* [VirtualFortress2](https://github.com/PinkMilkProductions/VirtualFortress2)
* [gmcl_openvr](https://github.com/Planimeter/gmcl_openvr/)
* [DXVK](https://github.com/doitsujin/dxvk)
* [source-sdk-2013](https://github.com/ValveSoftware/source-sdk-2013/)
