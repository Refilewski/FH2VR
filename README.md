# FH2VR

Experimental VR mod for **Forza Horizon 2 Xbox One 1.0.0.64 running through Azimuth**.

FH2VR can be used in two ways:

- **Standalone** — a single `FH2VR.exe` watches for `azimuth.exe`, extracts its embedded native runtime to `%LOCALAPPDATA%\FH2VR\runtime\<sha256>\FH2VR.dll`, and attaches automatically. FH2ML is not required and Azimuth helper files are not replaced.
- **FH2ML** — `FH2VR.dll` can still be installed as a normal FH2ML mod.

## Current status: 0.4.0-dev

Confirmed live on Quest 3 / SteamVR / wired Meta Link:

- native FH2 D3D12 backbuffer capture;
- GPU conversion from `R10G10B10A2_UNORM` to OpenVR-friendly `R8G8B8A8_UNORM`;
- per-eye OpenVR projection bounds from `GetProjectionRaw()`;
- FH2 Driver-camera FOV matched to the Quest frustum;
- yaw/pitch head tracking in the FH2 camera;
- roll-neutral compositor pose so the world does not tilt with your head;
- stable-frame pose reprojection;
- automatic flat GUI/menu overlay using FH2 UI-state detection ported from Azimuth-RPC;
- resilient overlay recreation after OpenVR restarts;
- frozen gameplay frame behind automatically detected menus to avoid the old menu-on-menu duplication;
- standalone attach path without replacing `azimuth-tls.dll`.

### M10 — positional parallax

The current development renderer adds an experimental positional-head-movement layer. Headset X/Y/Z translation relative to the initial VR pose shifts and slightly scales the source frustum, so leaning left/right/up/down/forward/back produces a positional parallax response instead of rotation-only tracking.

This is deliberately conservative and does **not** pretend to be true depth-based stereo. FH2 is still rendered as one mono scene; M10 approximates positional 6DoF until a real scene-depth/stereo path exists.

## Controls

FH2VR intentionally exposes only two keys:

- **F1** — VR ON/OFF. Enabling VR also enables head tracking and automatically recenters the current head pose.
- **F2** — manual HUD/GUI flat overlay ON/OFF. Automatic menu/map/garage/loading detection works independently.

The old F5-F12/debug controls, mock tracking, arrow-key camera controls, manual tracking disable and AER diagnostics are gone from the user keyboard path.

## GUI behavior

Normal gameplay:

```text
FH2 world -> full VR scene
```

When FH2VR detects Pause Menu, Photo Mode, World Map, Horizon Hub, Garage or Loading:

```text
last gameplay frame -> VR background
current FH2 frame   -> flat 16:9 menu overlay
```

**F2** can force the flat overlay manually, which is useful for the live HUD/minimap while the VR scene remains behind it.

The overlay still receives FH2's final composite backbuffer rather than a dedicated UI-only render target.

## Renderer pipeline

```text
FH2 D3D12 backbuffer (R10G10B10A2)
        |
        v
captured DIRECT queue / Present
        |
        v
retained D3D12 source texture
        |
        v
GPU compute conversion
R10G10B10A2 -> R8G8B8A8
        |
        v
projection + FOV match
        |
        v
M10 HMD X/Y/Z parallax shift/scale
        |
        v
OpenVR DirectX12 Submit_TextureWithPose
        |
        v
SteamVR / Quest 3
```

## Requirements

- Windows x64
- Azimuth
- Forza Horizon 2 build 1.0.0.64
- SteamVR/OpenVR
- D3D12-capable GPU
- tracked HMD

FH2ML is required only for the FH2ML package, not for standalone.

Current live target: **Meta Quest 3 over wired Quest Link**.

## Install — standalone

Download the GitHub Actions artifact:

```text
FH2VR-Standalone-0.4.0-dev
```

The standalone runtime is embedded in `FH2VR.exe`. There is no `Payload/FH2VR.dll` to keep next to it.

Run `FH2VR.exe`, then start Azimuth normally. FH2VR automatically detects `azimuth.exe`, attaches, and waits for the supported FH2 image before initializing the VR runtime.

The tray menu is intentionally minimal:

```text
FH2VR 0.4.0-dev
<status>
----------------
Exit
```

Standalone does **not** replace `azimuth-tls.dll`, install a helper, create a `mods` entry, or require FH2ML.

Once FH2 is ready, press **F1** to enable VR. **F2** manually toggles the flat HUD/GUI overlay.

## Install — FH2ML

Download:

```text
FH2VR-0.4.0-dev
```

Install it as a normal FH2ML mod. Do not keep multiple installed copies with the same `fh2vr` mod ID.

## Build

Native runtime / FH2ML package:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package --component FH2VR
```

Standalone single-EXE build stages the native runtime as an embedded resource before publishing:

```powershell
New-Item -ItemType Directory -Force standalone/Embedded
Copy-Item build/Release/FH2VR.dll standalone/Embedded/FH2VR.dll
dotnet publish standalone/FH2VR.Standalone.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Dependencies fetched by CMake:

- Valve OpenVR SDK `v2.15.6`
- MinHook `v1.3.4`

Their upstream license files are packaged under `licenses/`.

## Remaining renderer limits

- scene rendering is still mono rather than true geometric stereo;
- M10 positional parallax is screen/frustum based, not depth-aware;
- the flat GUI uses the final FH2 composite rather than an isolated HUD/frontend target.
