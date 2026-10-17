<h1 align="center">FH2VR</h1>

<p align="center">
  Experimental VR support for <strong>Forza Horizon 2 on Azimuth</strong>.<br />
  Adds headset look tracking, positional head movement, SteamVR output and a VR-friendly GUI path to the Xbox One version of FH2 running on PC.
</p>

<p align="center">
  <a href="https://github.com/Refilewski/FH2VR/releases/tag/dev-latest"><img src="https://img.shields.io/badge/Download-Development%20build-5865F2?style=for-the-badge&logo=github" alt="Download development build" /></a>
  <a href="https://github.com/Refilewski/FH2VR/issues"><img src="https://img.shields.io/badge/Report-Issue-d73a4a?style=for-the-badge&logo=github" alt="Report an issue" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Source--available-2ea44f?style=for-the-badge" alt="Source-available license" /></a>
</p>

> [!WARNING]
> FH2VR is experimental software. The current renderer is still mono and positional movement is an approximation rather than true geometric stereo. Expect bugs, visual artifacts and compatibility issues while the project is in development.

## Features

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

## Compatibility

FH2VR currently targets:

- **Forza Horizon 2 Xbox One 1.0.0.64**
- **Azimuth on Windows x64**
- **SteamVR / OpenVR**
- a D3D12-capable GPU
- a tracked VR headset

The main test setup is currently **Meta Quest 3 over wired Meta Link + SteamVR**.

Other SteamVR-compatible headsets may work, but are not yet confirmed.

Unknown FH2 builds are not supported.

## Download & install

Download the newest development build from:

**[FH2VR Development Release](https://github.com/Refilewski/FH2VR/releases/tag/dev-latest)**

Two packages are published automatically.

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

The current development renderer also adds positional head movement by shifting/scaling the rendered source according to headset X/Y/Z movement.

This makes leaning in the cockpit feel more responsive, but it is still a **mono approximation**. Nearby and distant objects do not yet move independently as they would with true depth-aware reprojection or native stereo rendering.

## Current limitations

- The game scene is still rendered as one mono image rather than true stereo geometry.
- Positional head movement is screen/frustum based, not depth-aware.
- Large head translations are limited by the available source image/FOV margin.
- The flat GUI path uses FH2's final composite frame rather than a dedicated UI-only render target.
- Compatibility with other FH2 camera/rendering mods is still being tested.
- Disconnect/reconnect recovery is experimental.

If something looks wrong, disable VR with **F1** before restarting SteamVR or Azimuth.

## Reporting issues

When opening an issue, include:

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

GitHub Actions builds and validates both packages automatically. Development releases are published under the moving `dev-latest` tag; version tags such as `v0.4.0` can publish normal versioned releases.

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
