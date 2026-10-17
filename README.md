# FH2VR

Experimental VR/head-tracking mod for **Forza Horizon 2 Xbox One 1.0.0.64 on Azimuth**, built as a native FH2ML mod.

## Current status: 0.2.1-dev prototype

This is **not full stereoscopic VR yet**. The current prototype proves two important paths:

- reads live HMD orientation from SteamVR through OpenVR;
- hooks the FH2 **Driver / cockpit** camera through FH2ML and applies head yaw/pitch;
- can run without a headset through a keyboard mock head-tracking mode;
- has an experimental **mono headset preview** that captures the visible FH2 client area, uploads it to a D3D11 texture and submits the same image to both eyes through the OpenVR compositor;
- center-crops the mono source to the headset's recommended per-eye aspect ratio instead of stretching the full 16:9 frame;
- targets roughly 60 FPS for the temporary GDI capture path instead of the original 30 FPS cap;
- chains the camera callback that was present when FH2VR initialized, so an already-loaded camera mod can remain in the chain;
- uses FH2ML `GetGameModule`, `RegisterMod`, `RegisterPatch` and `Log` instead of scanning the process.

The mono preview is only a compositor/capture milestone. It is not stereo, does not use per-eye projection and is intentionally separate from the final render-hook design.

## Controls

- **F6** — toggle keyboard mock head tracking.
- **Left / Right Arrow** — mock yaw.
- **Up / Down Arrow** — mock pitch.
- **F7** — toggle head tracking globally.
- **F8** — recenter. In mock mode this resets yaw/pitch to zero; with a real HMD it uses the current headset direction as forward.
- **F9** — toggle the experimental mono headset preview.

The first valid SteamVR pose is automatically used as the initial center.

## Testing real hardware

1. Start Meta Quest Link / SteamVR and confirm the HMD is tracked.
2. Install FH2VR in FH2ML Manager.
3. Start FH2 and switch to the **Driver / cockpit** camera.
4. Confirm yaw/pitch head tracking works and press **F8** if you need to recenter.
5. Press **F9** to restart OpenVR as a scene application and begin submitting the mono FH2 preview to the headset.
6. Press **F9** again to leave compositor mode and return to tracking-only mode.

### Mono preview limitations

The preview deliberately uses a simple capture path so the compositor can be validated before reverse-engineering FH2's renderer:

- the same image is submitted to both eyes, so there is no stereoscopic depth yet;
- capture now targets roughly 60 FPS, but actual throughput depends on the desktop/GDI capture path and game resolution;
- FH2 must be visible and not minimized;
- because the prototype captures the displayed client region, another window covering the game can appear in the headset preview;
- the source is center-cropped to the HMD eye aspect to avoid horizontal squeezing, so some left/right content is intentionally lost;
- the game's real projection/FOV is still not matched to the HMD;
- the eventual target is a direct D3D12/DXGI render/view/projection hook, not desktop capture.

## Testing without a headset

You can validate the FH2 camera hook without SteamVR hardware:

1. Install FH2VR in FH2ML Manager.
2. Start FH2 and switch to the **Driver / cockpit** camera.
3. Press **F6** to enable mock mode.
4. Hold the arrow keys and verify that the cockpit view follows yaw/pitch input.
5. Press **F8** to return to the centered view.

Mock mode bypasses SteamVR pose reading while it is enabled, so an HMD does not need to be connected.

## Requirements

- Windows x64;
- Azimuth supported by your FH2ML installation;
- FH2ML with Host API v1;
- Forza Horizon 2 build 1.0.0.64;
- SteamVR/OpenVR runtime and a tracked HMD for real headset tracking/preview;
- a D3D11-capable GPU for the temporary mono preview texture.

The tracking worker runs outside FH2ML's synchronous native initializer. If OpenVR is unavailable, FH2VR keeps handling hotkeys and retries SteamVR periodically instead of blocking game startup.

## Install

Build or download the FH2VR package, then install it as a normal FH2ML mod. The package contains `FH2VR.dll`, `mod.json`, `README.md` and `THIRD_PARTY.md`.

The current development manifest is enabled by default so fresh test packages load without requiring an extra toggle in FH2ML Manager.

## Compatibility note

FH2VR hooks the Driver camera vtable update slot and calls whatever callback was already there before applying head movement. This is intentionally friendly to existing camera hooks. However, a mod loaded **after** FH2VR that replaces the same slot can still override FH2VR; patch overlap should be treated as an ordering issue during the prototype phase.

The F9 preview temporarily initializes OpenVR as a scene application because compositor submission is not available to the normal tracking-only background mode.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package --component FH2VR
```

CMake fetches Valve OpenVR SDK **v2.15.6** at configure time. No OpenVR binaries are committed to this repository.

## Reverse-engineering notes

Extra Options confirms that the Driver camera vtable is at RVA `0x2E78120`. Its FOV callback lives at vtable slot `0x70`, and the native mounted-camera FOV routine is at RVA `0x10340F0`. Those addresses give FH2VR a concrete next path for matching the game's camera FOV to the HMD before the full per-eye projection hook is found.

## Roadmap

1. Validate the faster aspect-correct mono compositor preview on real Quest hardware.
2. Hook/override the Driver camera FOV using the confirmed mounted-camera FOV path.
3. Replace desktop/client capture with a direct FH2 D3D12/DXGI render-target hook.
4. Find and hook FH2's full view/projection path.
5. Add roll and positional 6DoF movement using the complete camera basis/position.
6. Render independent left/right eye views with correct IPD and projection and submit them to the VR compositor.
7. Add VR-specific HUD/menu handling and comfort options.

The current target is intentionally incremental: **first make the mono headset path comfortable enough to test, then move capture into the renderer and split it into true per-eye views**.
