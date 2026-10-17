# FH2VR

Experimental VR/head-tracking mod for **Forza Horizon 2 Xbox One 1.0.0.64 on Azimuth**, built as a native FH2ML mod.

## Current status: 0.3.2-dev diagnostic prototype

This is still experimental. The current build proves several VR paths:

- reads live HMD orientation from SteamVR through OpenVR;
- hooks the FH2 **Driver / cockpit** camera through FH2ML and applies head yaw/pitch;
- can run without a headset through a keyboard mock head-tracking mode;
- has a temporary GDI -> D3D11 -> OpenVR headset preview path;
- includes an experimental **alternate-eye rendering (AER) probe** that alternates a real OpenVR eye offset against FH2's cached camera matrix and retains separate left/right preview textures;
- adds diagnostics to determine whether `camera+0x260` actually affects the rendered world before more work is spent on GDI synchronization.

The AER probe is not yet final VR: projection asymmetry, frame/eye synchronization and direct renderer capture still need work.

## Controls

- **F6** — toggle keyboard mock head tracking.
- **Left / Right Arrow** — mock yaw.
- **Up / Down Arrow** — mock pitch.
- **F7** — toggle head tracking globally.
- **F8** — recenter.
- **F9** — toggle headset preview.
- **F10** — toggle experimental AER. Enabling it also enables the headset preview and resets eye swapping/debug amplification.
- **F11** — swap the actual left/right textures submitted to the OpenVR compositor.
- **F12** — toggle the diagnostic **x15 eye-translation multiplier**. This intentionally exaggerates the camera offset so a renderer-facing matrix should move the view very obviously.

The first valid SteamVR pose is automatically used as the initial center.

## 0.3.2-dev AER diagnostic

The probe uses `IVRSystem::GetEyeToHeadTransform` for the runtime-provided left/right eye offsets instead of hard-coding IPD.

For the FH2 camera side it follows the camera chain confirmed by Extra Options:

```text
GAME_RVA_PLAYER_MANAGER (0x34ABA60)
 -> game + 0x48
 -> simulation + 0x98
 -> manager + 0x2D0
 -> views -> active view
 -> view + 0xC0
 -> cached camera
 -> camera + 0x260 (4x4 matrix)
```

Extra Options identifies `[2,6,10]` as the third matrix column/look direction. The current hypothesis is a DirectX-style row-vector view matrix with translation in `[12,13,14]`, so the lateral eye probe writes `matrix[12] = base - eyeX`.

### Anti-drift handling

0.3.2 no longer blindly subtracts the next eye offset from whatever FH2VR wrote previously. It tracks the last unmodified base and last written translation. If FH2 has not rebuilt the matrix between eye updates, the next eye is derived from the same base rather than accumulating offsets. If FH2 has changed the matrix, the new value becomes the fresh base.

### Matrix dump and x15 perturbation

Enabling AER requests a one-time dump of all 16 floats before the eye offset is applied. Pressing **F12** requests another dump and changes the eye offset multiplier between `1x` and `15x`.

At Quest-like IPD, x15 is roughly a half-meter lateral perturbation per eye. If `camera+0x260` feeds the world renderer, that should be unmistakable. If the world remains visually unchanged, this matrix is probably only gameplay/cache data or is overwritten before rendering.

### L/R image-difference diagnostic

The temporary GDI capture path now keeps a small CPU-side sample of the latest retained left/right frames and logs a mean absolute RGB difference approximately once per second:

```text
AER L/R mean RGB diff = 0.42 (normal IPD)
AER L/R mean RGB diff = 18.37 (F12 x15)
```

This value is only diagnostic. Motion, head movement and temporal mismatch can raise it even when the camera is identical, so the best comparison is with the car stopped and the headset held still. A large increase when switching from normal IPD to F12 x15 is much more meaningful than the absolute number alone.

### Recommended test

1. Start FH2 in cockpit view and enable **F10**.
2. Keep the car stopped and hold the headset as still as practical for a few seconds.
3. Note the `AER L/R mean RGB diff` values and the dumped `0x260` matrix.
4. Press **F12**. If the matrix is renderer-facing, the exaggerated lateral shift should be obvious and the L/R diff should usually rise strongly.
5. Press **F11**. This really swaps the retained textures sent to `Eye_Left` and `Eye_Right`; if the two captures contain useful stereo disparity, the apparent depth should invert or become clearly worse/better.

Do not keep testing if the headset causes eye strain, headache, nausea or dizziness.

## Headset preview limitations

The current preview is still a temporary desktop/client capture path:

- capture targets roughly 60 FPS but depends on desktop/GDI throughput;
- FH2 must be visible and not minimized;
- another window covering FH2 can appear in the headset;
- AER frame-to-eye synchronization is approximate because capture is not hooked to the game's real Present yet;
- temporal differences can inflate the L/R diff diagnostic;
- per-eye projection/FOV is not yet matched to the HMD;
- the eventual target is direct renderer capture and properly synchronized per-eye submission.

## Testing without a headset

1. Install FH2VR in FH2ML Manager.
2. Start FH2 and switch to the **Driver / cockpit** camera.
3. Press **F6** to enable mock mode.
4. Hold the arrow keys and verify yaw/pitch.
5. Press **F8** to recenter.

## Requirements

- Windows x64;
- Azimuth supported by FH2ML;
- FH2ML Host API v1;
- Forza Horizon 2 build 1.0.0.64;
- SteamVR/OpenVR and a tracked HMD for real VR testing;
- a D3D11-capable GPU for the temporary preview texture.

## Install

Build or download the FH2VR package and install it as a normal FH2ML mod. The development manifest is enabled by default.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package --component FH2VR
```

CMake fetches Valve OpenVR SDK **v2.15.6** at configure time.

## Reverse-engineering notes

Confirmed FH2/Extra Options data currently used by the project:

- Driver camera vtable: `0x2E78120`
- Driver update slot: `0x20`
- Driver heading/forward: `camera + 0x1C0`
- view-dirty byte: `camera + 0x21A`
- Driver FOV callback slot: `0x70`
- native mounted-camera FOV: `0x10340F0`
- player manager global: `0x34ABA60`
- cached camera matrix: separate active-view camera object at `+0x260`

## Roadmap

1. Use 0.3.2 x15 perturbation, matrix dump and L/R diff logging to decide whether `camera+0x260` affects the world render.
2. Confirm which graphics API Azimuth exposes for FH2 and hook the real `IDXGISwapChain::Present` path.
3. Replace GDI capture with direct backbuffer capture and bind eye selection to actual presented frames.
4. Move to `Submit_TextureWithPose` using the pose associated with each rendered eye frame.
5. Match OpenVR per-eye projection/FOV using `GetProjectionRaw` and the confirmed FH2 FOV path.
6. If AER is viable, harden it; otherwise move to depth reprojection or a renderer-level dual-view path.
7. Add HUD/menu handling, roll and positional 6DoF.
