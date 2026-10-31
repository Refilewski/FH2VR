<h1 align="center">FH2VR — Abandoned Project</h1>

<p align="center">
  An abandoned, experimental VR prototype for <strong>Forza Horizon 2 on Azimuth</strong>.<br />
  <strong>MONO ONLY — NOT STEREOSCOPIC VR.</strong> This project is no longer being developed or maintained.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Status-Abandoned-critical?style=for-the-badge" alt="Project status: Abandoned" />
  <img src="https://img.shields.io/badge/Rendering-Mono%20only-orange?style=for-the-badge" alt="Rendering: Mono only" />
  <a href="https://github.com/Refilewski/FH2VR/releases/tag/dev-latest"><img src="https://img.shields.io/badge/Releases-Legacy%20builds-5865F2?style=for-the-badge&logo=github" alt="Legacy development builds" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Source--available-2ea44f?style=for-the-badge" alt="Source-available license" /></a>
</p>

> [!CAUTION]
> **FH2VR IS ABANDONED AND NO LONGER MAINTAINED.** It does **not** provide real stereoscopic VR: the game scene is rendered as a **single mono image**, not separate views for the left and right eyes. All previous builds are experimental and unsupported. No updates, bug fixes or compatibility support are planned.

## Project status — Abandoned

**Development of FH2VR has been discontinued.** This repository is kept available for historical/reference purposes, not as an actively supported VR solution.

The decision is also informed by **Azimuth's plans to add VR support in future releases**. That potential implementation belongs to Azimuth, **not** FH2VR. No release date, feature list or guarantee of availability is implied here; follow [Azimuth's official page](https://almost-there-games.itch.io/azimuth) for announcements.

**Important limitation:** FH2VR's renderer is **monoscopic, not stereoscopic**. Sending a single game frame to a VR headset, even with headset tracking and screen-space motion effects, is **not equivalent** to rendering distinct left-eye and right-eye camera views with correct binocular depth.

Please do **not** interpret this repository, its builds or screenshots as native FH2 VR support. Existing builds may still run on older compatible setups, but they are preserved **as-is, without support**.

## Features (last experimental builds)

These features describe the **last historical prototype**, not a maintained or fully functional VR product.

- VR output through SteamVR / OpenVR.
- Head-tracked yaw and pitch in the FH2 Driver camera.
- Roll-neutral world rendering so leaning your head does not tilt the car/world.
- Experimental positional head movement for leaning left/right/up/down/forward/back.
- FH2 camera FOV matched to the headset projection.
- Automatic handling for Pause Menu, Photo Mode, World Map, Horizon Hub, Garage and Loading.
- Flat VR GUI/menu overlay with frozen gameplay behind automatically detected menus.
- Automatic OpenVR reconnect after temporary headset / SteamVR disconnects.
- Two installation modes: a standalone Windows app or an FH2ML mod.
- Standalone mode does not replace `azimuth-tls.dll` and does not require FH2ML.

## Historical compatibility

The last FH2VR development builds targeted:

- **Forza Horizon 2 Xbox One 1.0.0.64**
- **Azimuth on Windows x64**
- **SteamVR / OpenVR**
- a D3D12-capable GPU
- a tracked VR headset

The original test setup was **Meta Quest 3 over wired Meta Link + SteamVR**.

Other SteamVR-compatible headsets were not fully validated. New Azimuth or FH2 builds **have not been tested against this abandoned prototype**.

## Legacy builds & installation (unsupported)

The [last FH2VR development builds](https://github.com/Refilewski/FH2VR/releases/tag/dev-latest) remain available **for archival/testing purposes only**. They are **not** a supported VR solution, and should not be confused with planned future VR support in Azimuth.

Historically, the repository produced two packages.

### Standalone

Download:

```text
FH2VR-Standalone-dev-latest.zip
```

Then:

1. Extract the ZIP anywhere outside the Azimuth game folder.
2. Run `FH2VR.exe`.
3. Start Azimuth and Forza Horizon 2 normally.
4. FH2VR detects `azimuth.exe` and attaches automatically.
5. Once FH2 is running, press **F1** to enable VR.

The standalone build is a single EXE with the native FH2VR runtime embedded inside it. At runtime it extracts a private copy under `%LOCALAPPDATA%\FH2VR\runtime\...` and uses that copy without replacing Azimuth helper files.

### FH2ML

Download:

```text
FH2VR-FH2ML-dev-latest.zip
```

Install it as a normal FH2ML mod.

Do **not** run the standalone EXE and the FH2ML build at the same time. Use one FH2VR runtime path per Azimuth process.

## Controls

| Key | Action |
| --- | --- |
| **F1** | Toggle VR on/off. Enabling VR recenters the current headset pose. |
| **F2** | Toggle the manual flat GUI/HUD overlay. |

Automatic menu/map/garage/loading handling works independently of F2.

## How the current renderer works

FH2VR captures FH2's native D3D12 backbuffer, converts it on the GPU to an OpenVR-compatible texture, adjusts the projection/FOV for the headset and submits the result to SteamVR.

The last experimental renderer approximates positional head movement by shifting/scaling the rendered source according to headset X/Y/Z movement.

**This is mono, NOT stereo.** The game does not render a dedicated scene for each eye. As a result, binocular depth, geometry-correct head-position parallax and true room-scale VR are **not provided**. Near and distant geometry cannot respond independently to movement as they would with real stereo rendering or depth-aware reprojection.

## Known limitations of the abandoned prototype

- **Monoscopic rendering only: no distinct left-eye/right-eye geometry, no true stereoscopic depth.**
- Positional head movement is screen/frustum based, not depth-aware.
- Large head translations are limited by the available source image/FOV margin.
- The flat GUI path uses FH2's final composite frame rather than a dedicated UI-only render target.
- Compatibility with other FH2 camera/rendering mods is still being tested.
- Disconnect/reconnect recovery is experimental.

If something looks wrong, disable VR with **F1** before restarting SteamVR or Azimuth.

## Legacy troubleshooting notes

**The project is abandoned.** Bug reports and feature requests are not actively monitored or supported. If you are examining the old code or testing historical builds, useful environment details include:

- headset model;
- connection method (Meta Link, Steam Link, Virtual Desktop, native SteamVR, etc.);
- GPU;
- Azimuth version;
- whether you use standalone or FH2ML;
- other FH2 mods that are active;
- what you were doing when the problem happened;
- whether restarting SteamVR, FH2VR or Azimuth changes the result.

Useful symptoms include wrong head-tracking direction, image stretching, flicker, frozen SteamVR view, GUI overlay problems, reconnect failures or crashes.

Do not upload copyrighted game files, DLC, license keys or full game memory dumps.

## Build from source

Requirements:

- Windows x64
- Visual Studio Build Tools with C++ support
- CMake
- .NET 8 SDK for the standalone launcher

Native runtime / FH2ML package:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package --component FH2VR
```

Standalone:

```powershell
New-Item -ItemType Directory -Force standalone/Embedded
Copy-Item build/Release/FH2VR.dll standalone/Embedded/FH2VR.dll
dotnet publish standalone/FH2VR.Standalone.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

This repository retains its historical GitHub Actions build/release workflow and legacy `dev-latest` tag. The project is **no longer maintained**; automated builds, compatibility and future releases are not promised.

## Third-party components

FH2VR uses:

- Valve OpenVR SDK
- MinHook
- the FH2ML SDK header for the FH2ML package

Their licensing information is documented in [THIRD_PARTY.md](THIRD_PARTY.md) and the upstream license files are included with packaged builds where required.

## License

FH2VR is **source-available**, not OSI open source.

The [FH2VR Use-Only License 1.0](LICENSE) permits inspection, local compilation and personal use. Redistribution, publishing modified builds, incorporating FH2VR code into another project or distributing derivative implementations requires separate permission from Refilewski.

## Disclaimer

FH2VR is an independent fan project by **Refilewski**. It is not affiliated with or endorsed by Microsoft, Xbox, Playground Games, Turn 10, Valve, Meta or Azimuth.

No game, DLC, game executable, license key or other copyrighted Forza Horizon 2 content is included in this repository or its releases.
