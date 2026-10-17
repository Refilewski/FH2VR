# FH2VR

Experimental VR/head-tracking mod for **Forza Horizon 2 Xbox One 1.0.0.64 on Azimuth**, built as a native FH2ML mod.

## Current status: 0.4.0-dev renderer prototype

FH2VR now has a native D3D12 -> OpenVR path. Live testing on Quest 3 has confirmed:

- live SteamVR/OpenVR HMD orientation tracking;
- Driver/cockpit head yaw and pitch through the FH2 camera hook;
- FH2/Azimuth presents through **D3D12**;
- the game swapchain is **1920x1080, R10G10B10A2_UNORM, 3 buffers**;
- the real FH2 DIRECT command queue can be captured through `ExecuteCommandLists`;
- the current backbuffer can be copied every Present;
- `R10G10B10A2_UNORM` is rejected by SteamVR with compositor error 105, so FH2VR converts it on-GPU to **R8G8B8A8_UNORM**;
- the converted D3D12 texture successfully displays in Quest 3 through `TextureType_DirectX12` and `Submit_TextureWithPose`.

The remaining major visual problem is a strong cross-eye / projection mismatch. Removing GDI/BitBlt did not remove it, so the current focus is projection/FOV rather than capture.

## Current M4.1 projection + FOV diagnostic

M4 keeps the proven M3 D3D12 capture + R10->R8 conversion path and changes only the OpenVR submit stage.

It reads:

```text
IVRSystem::GetProjectionRaw(Eye_Left)
IVRSystem::GetProjectionRaw(Eye_Right)
```

and builds the combined tangent frustum using the same construction used by Valve's SteamVR sample. The same mono frame is then submitted with **different `VRTextureBounds_t` for left and right eyes**.

The renderer log records:

```text
M4 OpenVR projection raw: L=[...] R=[...]
combinedFov=...x... deg
tanHalf=(...,...)
boundsL=[...]
boundsR=[...]
M4 OpenVR per-eye projection submit OK
```

M4.1 additionally logs the live FH2 Driver-camera FOV path without changing it:

```text
Driver FOV diagnostic: callback=... result=... camera+0x1D0=... rad (... deg)
```

This lets one runtime test compare the HMD-required combined frustum against FH2's actual mounted-camera FOV.

This is still a projection diagnostic, not true stereo rendering.

## Confirmed camera/FOV data

Relevant confirmed FH2/Extra Options data:

- Driver camera vtable: `0x2E78120`
- Driver update slot: `0x20`
- Driver FOV callback slot: `0x70`
- Driver heading/forward: `camera + 0x1C0`
- mounted-camera base FOV: `camera + 0x1D0`
- view-dirty byte: `camera + 0x21A`
- native mounted-camera FOV routine: `0x10340F0`
- player manager global: `0x34ABA60`
- cached active-view camera matrix: separate camera object at `+0x260`

The old `camera+0x260` AER path is kept only as a diagnostic. At the observed ~32-36 FPS game rate, alternate-eye rendering would provide only ~16-18 newly rendered frames per eye, so it is no longer the primary VR plan.

## Controls

- **F6** — toggle keyboard mock head tracking
- **Left / Right Arrow** — mock yaw
- **Up / Down Arrow** — mock pitch
- **F7** — toggle head tracking globally
- **F8** — recenter
- **F9** — toggle headset preview / native D3D12 OpenVR submission
- **F10** — legacy experimental AER toggle
- **F11** — legacy AER compositor eye swap
- **F12** — legacy x15 matrix perturbation + Driver camera scan request

For current M4/M4.1 testing, use **F9 only** unless specifically testing one of the legacy diagnostics.

## Renderer pipeline

```text
FH2 D3D12 backbuffer (R10G10B10A2)
        |
        v
Present / captured DIRECT queue
        |
        v
retained D3D12 source texture
        |
        v
compute conversion
R10G10B10A2 -> R8G8B8A8
        |
        v
OpenVR DirectX12 submit
        |
        +---- left eye bounds from GetProjectionRaw
        |
        +---- right eye bounds from GetProjectionRaw
        |
        v
SteamVR / Quest 3
```

## Testing M4.1

1. Install the latest `FH2VR-0.4.0-dev` artifact.
2. Start FH2 and enter Driver/cockpit view.
3. Press **F9** only.
4. Check whether the cross-eye effect is smaller, unchanged or worse.
5. Exit and inspect `FH2VR_RendererProbe.log`.

Useful lines are the M4 projection log and the Driver FOV diagnostic. Together they show the HMD-required frustum and the game's current camera FOV.

Keep projection tests brief if the image causes eye strain, headache, nausea or dizziness.

## Requirements

- Windows x64
- Azimuth supported by FH2ML
- FH2ML Host API v1
- Forza Horizon 2 build 1.0.0.64
- SteamVR/OpenVR
- a D3D12-capable GPU
- a tracked HMD for real VR testing

Current live target: **Meta Quest 3 over wired Quest Link**.

## Install

Download the latest GitHub Actions artifact named:

```text
FH2VR-0.4.0-dev
```

and install it as a normal FH2ML mod. Avoid keeping multiple copies of the mod with the same `fh2vr` ID installed at once.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package --component FH2VR
```

CMake fetches:

- Valve OpenVR SDK `v2.15.6`
- MinHook `v1.3.4`

The package also carries their upstream license files under `licenses/`.

## Roadmap

1. Live-test M4 per-eye OpenVR bounds and compare the result with the old symmetric submission.
2. Compare Quest combined FOV against the logged Driver mounted-camera FOV.
3. If projection improves but scale remains wrong, match FH2 Driver FOV to the HMD combined frustum.
4. Add a D3D12 depth-resource probe (`CreateDepthStencilView` / depth-capable resources) and identify the real scene depth target.
5. Use color + depth for per-eye reprojection if a stable depth source is available.
6. Separate scene from HUD/UI if necessary.
7. Add positional 6DoF and renderer-level improvements after the projection/depth path is stable.
