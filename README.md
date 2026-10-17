# FH2VR

Experimental VR mod for **Forza Horizon 2 Xbox One 1.0.0.64 running through Azimuth**.

FH2VR can currently be used in two ways:

- **Standalone** — `FH2VR.exe` attaches the FH2VR runtime to the running `azimuth.exe`; no FH2ML installation is required and no Azimuth helper files are replaced.
- **FH2ML** — `FH2VR.dll` can still be installed as a normal FH2ML mod.

## Current status: 0.4.0-dev

Live on Quest 3 / SteamVR / wired Meta Link:

- native FH2 D3D12 backbuffer capture;
- GPU conversion from `R10G10B10A2_UNORM` to OpenVR-friendly `R8G8B8A8_UNORM`;
- per-eye OpenVR projection bounds from `GetProjectionRaw()`;
- FH2 Driver-camera FOV matched to the Quest frustum;
- yaw/pitch head tracking in the FH2 camera;
- roll-neutral compositor pose so the world does not tilt with your head;
- stable-frame pose reprojection for smoother HMD motion;
- automatic flat GUI/menu overlay using FH2 UI-state detection ported from Azimuth-RPC;
- M9 resilient overlay recreation after OpenVR restarts;
- M9 frozen gameplay frame behind automatically detected menus to avoid the old menu-on-menu scene duplication.

The current renderer is still mono scene rendering rather than true geometric stereo/depth reprojection, but the live VR path is usable and smooth with sensible FH2 graphics settings.

## Controls

FH2VR intentionally exposes **only two keys**:

- **F1** — toggle VR ON/OFF. Enabling VR also enables head tracking and automatically recenters the current head pose.
- **F2** — toggle the manual HUD/GUI flat overlay. Automatic menu/map/garage/loading detection works independently and does not require pressing F2.

All old debug controls were removed from the runtime keyboard path: F5-F12, mock tracking, arrow-key camera control, manual tracking disable, AER eye swap and x15 diagnostics are no longer user keybinds.

## GUI behavior

During normal gameplay:

```text
FH2 world -> full VR scene
```

When FH2VR detects Pause Menu, Photo Mode, World Map, Horizon Hub, Garage or Loading:

```text
last gameplay frame -> VR background
current FH2 frame   -> flat 16:9 menu overlay
```

**F2** can force the flat overlay manually, which is useful for showing the live HUD/minimap on a virtual 16:9 screen while the VR scene remains behind it.

The overlay currently receives FH2's final composite backbuffer rather than a dedicated UI-only render target. Isolating the real HUD/frontend target is a later renderer step.

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

FH2ML is required only for the FH2ML package, not for the standalone package.

Current live target: **Meta Quest 3 over wired Quest Link**.

## Install — standalone

Download the GitHub Actions artifact:

```text
FH2VR-Standalone-0.4.0-dev
```

Keep the package layout intact:

```text
FH2VR.exe
Payload/
  FH2VR.dll
```

Run `FH2VR.exe`. It waits for `azimuth.exe` and automatically attaches the runtime when Azimuth starts. You can also use **Attach now** from the tray menu.

If you want the launcher to start Azimuth for you, use **Select Azimuth folder...** once and then **Launch Azimuth**.

The standalone path does **not** replace `azimuth-tls.dll`, install a helper, create a `mods` entry, or require FH2ML.

Once FH2 is ready, press **F1** to enable VR. **F2** manually toggles the flat HUD/GUI overlay.

## Install — FH2ML

Download the GitHub Actions artifact:

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

Standalone launcher:

```powershell
dotnet publish standalone/FH2VR.Standalone.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Dependencies fetched by CMake:

- Valve OpenVR SDK `v2.15.6`
- MinHook `v1.3.4`

Their upstream license files are packaged under `licenses/`.

## Next renderer work

1. Continue hardening automatic GUI transitions and frozen-world behavior.
2. Find the FH2 HUD/frontend render target so the flat overlay can contain UI only.
3. Probe the D3D12 scene depth target.
4. Investigate color + depth per-eye reprojection / true stereo synthesis.
5. Add positional 6DoF after the stereo/depth path is stable.
