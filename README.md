# FH2VR

Experimental VR/head-tracking mod for **Forza Horizon 2 Xbox One 1.0.0.64 on Azimuth**, built as a native FH2ML mod.

## Current status: 0.3.1-dev prototype

This is still experimental. The current build proves several VR paths:

- reads live HMD orientation from SteamVR through OpenVR;
- hooks the FH2 **Driver / cockpit** camera through FH2ML and applies head yaw/pitch;
- can run without a headset through a keyboard mock head-tracking mode;
- has a temporary GDI -> D3D11 -> OpenVR headset preview path;
- includes an experimental **alternate-eye rendering (AER) probe** that alternates a real OpenVR eye offset against FH2's cached camera matrix and retains separate left/right preview textures;
- keeps the previous mono preview as a fallback.

The AER probe is not yet final VR: projection asymmetry, frame/eye synchronization and direct renderer capture still need work.

## Controls

- **F6** — toggle keyboard mock head tracking.
- **Left / Right Arrow** — mock yaw.
- **Up / Down Arrow** — mock pitch.
- **F7** — toggle head tracking globally.
- **F8** — recenter.
- **F9** — toggle headset preview.
- **F10** — toggle experimental AER. Enabling it also enables the headset preview and resets eye swapping.
- **F11** — swap the actual left/right textures submitted to the OpenVR compositor.

The first valid SteamVR pose is automatically used as the initial center.

## 0.3.1-dev AER experiment

The probe uses `IVRSystem::GetEyeToHeadTransform` for the real runtime-provided left/right eye offsets instead of hard-coding IPD.

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

Extra Options identifies `[2,6,10]` as the third matrix column/look direction. That layout matches the usual DirectX row-vector view matrix convention where the basis vectors are stored in columns and view translation lives in `[12,13,14]`. The 0.3.1 probe therefore applies lateral eye translation through `matrix[12] -= eyeX` instead of modifying `[3,7,11]` as 0.3.0 did.

This is still a live hypothesis test. If `camera+0x260` is only a gameplay/cache matrix and is not consumed by the final renderer, the two retained frames can still be visually identical. The next renderer-facing step would then be a real Present/backbuffer hook and matrix/depth investigation.

### What to look for

With AER enabled in cockpit view:

1. The FH2ML log should show OpenVR left/right eye offsets.
2. The first successful camera write should log `AER probe reached cached FH2 view matrix at camera+0x260` and the before/after `matrix[12]` value.
3. Near cockpit geometry should have noticeable left/right parallax compared with the distant road/world.
4. Press **F11**. In 0.3.1 this now really swaps the textures sent to `Eye_Left` and `Eye_Right`. If the two eye renders are genuinely different, the stereo depth should visibly invert or become much worse/better.

Do not use this prototype for long sessions if it causes eye strain or discomfort.

## Headset preview limitations

The current preview is still a temporary desktop/client capture path:

- capture targets roughly 60 FPS but depends on desktop/GDI throughput;
- FH2 must be visible and not minimized;
- another window covering FH2 can appear in the headset;
- AER frame-to-eye synchronization is approximate because capture is not hooked to the game's real Present yet;
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

1. Validate the corrected 0.3.1 AER translation and true F11 eye swap on Quest 3.
2. Confirm which graphics API Azimuth exposes for FH2 and hook the real Present path.
3. Replace GDI capture with direct backbuffer capture.
4. Synchronize eye selection to actual rendered frames.
5. Match OpenVR per-eye projection/FOV.
6. If AER is viable, harden it; otherwise move to depth reprojection or a renderer-level dual-view path.
7. Add HUD/menu handling, roll and positional 6DoF.
