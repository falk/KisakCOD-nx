# KisakCOD

## About the project
An open source fully-buildable reimplementation of Call of Duty 4

Aimed towards mod developers and COD4 enthusiasts.

![licimg](./GPLv3_Logo.png)

### Development Blog
Learn about the Development of KisakCOD here: [https://lwss.github.io/Duty-Of-Kisak/](https://lwss.github.io/Duty-Of-Kisak/)

## Nintendo Switch port (work in progress)

This fork runs the single-player campaign on the Nintendo Switch as a homebrew NRO.
It is single-player only for now: multiplayer is not ported yet.

> **Note:** This port is very much AI-generated. Most of its code, tests and docs were written by AI coding agents, directed and tested on real hardware by a human. Review it with that in mind.

The target is **720p at 60 fps** on a stock handheld Switch. It is not there in every scene yet.

**Status**
- Every campaign map loads. Only the first missions (Killhouse, Cargoship) have had real play-testing.
- Tested on a Switch OLED only.
- Busy scenes (rain, heavy particles) are still GPU-bound and can drop below 60 fps.

**What changed for the Switch**
- **New renderer.** The game's Direct3D 9 renderer runs on deko3d, the Switch's low-level GPU API.
  It is being moved over piece by piece: a D3D9 layer remains, but more and more of the renderer calls deko3d directly.
  Shaders are translated on first use and cached on the SD card.
- **Original game files on a 64-bit console.** The retail game files are laid out for 32-bit PCs; the port converts them as they load.
- **Native audio**, through the Switch's audio renderer.
- **Gyro aim** while aiming down sights, and HD rumble.

**Performance work so far**
- The CPU prepares the next frame while the GPU draws the current one, with up to two frames in flight.
- Rendering runs on its own thread, using the Switch's three CPU cores.
- Dynamic resolution renders at 75-100% of 720p to hold 60 fps.
- The renderer does less per-draw work, copies less, and uses cheaper shadow filtering.
- NEON-optimised character skinning, and a build with link-time and profile-guided optimisation.

**Not done yet**
- Multiplayer.
- Play-testing the full campaign. Most missions have only been loaded, not played through.
- A steady 60 fps at 720p in the busiest scenes. A temporal upscaler is planned.
- Docked mode tuning. Only handheld has been tested.
- Known issues: an occasional GPU fault still under investigation, and a crash on one mission (Bog) in some runs.

### Building for Switch

Requires [devkitPro](https://devkitpro.org/) with `DEVKITPRO` set, and these devkitPro packages:
`switch-dev`, `deko3d`, `switch-openal-soft`, `switch-ffmpeg`.
The build also needs CMake 3.21 or newer, Ninja, git, and bison, flex and python3 with `mako` (they build the shader compiler).
MojoShader and UAM are fetched by CMake, so the first configure needs network access.

```
cmake --preset switch-sp
cmake --build --preset switch-sp     # -> build/switch-sp/KisakCOD-sp.nro
nxlink -s build/switch-sp/KisakCOD-sp.nro
```

To stream the console log to a PC over nxlink, configure with `-DKISAK_SWITCH_LOG_HOST=<pc ip>`.
Link-time optimisation is on; `-DKISAK_SWITCH_LTO=OFF` gives a faster link for development.

### SD card layout

Game data is not included. Copy the files from your own PC copy of Call of Duty 4:

```
sdmc:/switch/kisakcod/
├── KisakCOD-sp.nro
├── retail/
│   ├── main/             iw_00.iwd ... iw_11.iwd, localized_english_iw*.iwd
│   │   └── video/        *.bik cinematics from the game's main/video/
│   └── zone/
│       └── english/      *.ff fastfiles from the game's zone/english/
└── deko9-cache/          created on first run (shader and pipeline cache)
```

Launch it from the Homebrew Menu opened through title override (hold R while starting a game). Applet mode, from the album, is not supported.

### Switch port credits

Code adapted from:
- [Zeptoniator/KisakCOD `android-port-bootstrap`](https://github.com/Zeptoniator/KisakCOD/tree/android-port-bootstrap): the Android port whose 32-bit fastfile walk this port's retail loader follows (wire semantics).
- [RSDuck/duckstation `switch-port`](https://github.com/RSDuck/duckstation/tree/switch-port): crash handler.
- [Snapdragon Game Super Resolution](https://github.com/SnapdragonStudios/snapdragon-gsr) and [AMD FidelityFX FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR): upscaling and sharpening shaders.
- [DXVK](https://github.com/doitsujin/dxvk): the vendored D3D9 headers.

Libraries: [devkitPro](https://devkitpro.org/) (libnx, deko3d, uam), [MojoShader](https://github.com/icculus/mojoshader), [FFmpeg](https://ffmpeg.org/), [dr_libs](https://github.com/mackron/dr_libs), [minimp3](https://github.com/lieff/minimp3).

Used as references while building the port:
- [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools) and [CoD-FF-Tools](https://github.com/primetime43/CoD-FF-Tools): fastfile format oracles.
- [jm2/kisakcod](https://github.com/jm2/kisakcod): 64-bit (native64/disk32) layout oracle.

## Current Requirements
- Windows OS
- Visual Studio 2022
- CMake >= 3.16
- [DirectX SDK 2010](https://www.microsoft.com/en-us/download/details.aspx?id=6812)
- Steam with a copy of [Call of Duty 4](https://store.steampowered.com/app/7940/Call_of_Duty_4_Modern_Warfare_2007/)


## How to build
1) Install the above requirements and Clone repo
2) Open a terminal and run `generate-project.bat`
3) Open .sln projects that are generated in `build-sp`, `build-mp`, and `build-dedi` respectively. 
4) Copy COD4 Game files to `bin/(BUILD_TYPE)/*` (Don't try to cherry-pick them, small files like localization.txt are needed)
5) Copy `deps/binklib/binkw32.dll` as well ^^
6) Copy all files in `deps/msslib/dlls/*` ^^ 
7) Copy `deps/steamsdk/steam_api.dll`  ^^
8) Run the game via Visual Studio play button or just the .exe


```
Keep in Mind: This is a ~20 year old game with some known exploits. We will try to fix these as we become aware of them.
However, there is a non-zero chance of some type of binary exploitation when playing online. Use a sandbox (Sandboxie?) for peace of mind. 
```

## Known Issues
(Use the **[issues](https://github.com/SwagSoftware/KisakCOD/issues)** section)

## Troubleshooting
- ***Can't Connect to Dedicated Server*** :
  -  Check `net_ip` and `net_port`, the server will increment the port if the preferred one isn't available but the client won't sweep upwards.
 - ***DLL Error upon launch*** :
   - You didn't copy over the necessary runtime DLL's

## FAQ
- Can we use AI in this project?
  - Yes you can, but you're still responsible for whatever you commit. In general, you should have the AI be assisting you, and not carrying you. We have started using AI to help de-bug, and it's been extremely helpful.

## Credits and Special Thanks
- ***All Original COD4 Developers (for creating one of the best games of all time)***
- https://github.com/PJayB/jk3src (Jedi Academy fork with .sln)
- https://github.com/voron00/CoD2rev_Server - Useful yacc code for the gsc scripting here
- https://github.com/shiversoftdev/BO3Enhanced - Viewed as reference code for some of the Steam API Auth
- [RAD Game Tools](https://www.radgametools.com/) for their Bink and Miles Sound System libraries.
- [ODE Physics](https://www.ode.org/) COD4 uses a modified version of this physics engine.


## Discord
[Join the KisakCOD Discord](https://discord.gg/9uqntRWMA3)
