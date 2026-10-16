# FH2VR

Experimental VR/head-tracking mod for **Forza Horizon 2 Xbox One 1.0.0.64 on Azimuth**, built as a native FH2ML mod.

## Current status: 0.1.1-dev prototype

This is **not full stereoscopic VR yet**. The current milestone proves the camera path:

- reads live HMD orientation from SteamVR through OpenVR;
- can run without a headset through a keyboard mock head-tracking mode;
- hooks the FH2 **Driver / cockpit** camera through FH2ML;
- applies head yaw and pitch after the game's normal mounted-camera update;
- chains the camera callback that was present when FH2VR initialized, so an already-loaded camera mod can remain in the chain;
- uses FH2ML `GetGameModule`, `RegisterMod`, `RegisterPatch` and `Log` instead of scanning the process.

Roll, positional 6DoF movement and stereo eye rendering are later milestones.

## Controls

- **F6** — toggle keyboard mock head tracking.
- **Left / Right Arrow** — mock yaw.
- **Up / Down Arrow** — mock pitch.
- **F7** — toggle head tracking globally.
- **F8** — recenter. In mock mode this resets yaw/pitch to zero; with a real HMD it uses the current headset direction as forward.

The first valid SteamVR pose is automatically used as the initial center.

## Testing without a headset

You can validate the FH2 camera hook without SteamVR hardware:

1. Install and enable FH2VR in FH2ML Manager.
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
- SteamVR/OpenVR runtime and a tracked HMD only when testing real headset tracking.

The tracking worker runs outside FH2ML's synchronous native initializer. If OpenVR is unavailable, FH2VR keeps handling hotkeys and retries SteamVR periodically instead of blocking game startup.

## Install

Build or download the FH2VR package, then install it as a normal FH2ML mod. The package contains `FH2VR.dll`, `mod.json`, `README.md` and `THIRD_PARTY.md`.

The manifest is disabled by default while this is an experimental prototype. Enable it in the FH2ML Manager after installation.

## Compatibility note

FH2VR hooks the Driver camera vtable update slot and calls whatever callback was already there before applying head movement. This is intentionally friendly to existing camera hooks. However, a mod loaded **after** FH2VR that replaces the same slot can still override FH2VR; patch overlap should be treated as an ordering issue during the prototype phase.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package --component FH2VR
```

CMake fetches Valve OpenVR SDK **v2.15.6** at configure time. No OpenVR binaries are committed to this repository.

## Roadmap

1. Validate cockpit yaw/pitch through keyboard mock mode and then on real hardware; correct axis/sign mapping if required.
2. Add roll and positional head movement after identifying the full mounted-camera basis/position safely.
3. Find and hook FH2's view/projection path.
4. Render per-eye views and submit them to a VR compositor.
5. Add VR-specific HUD/menu handling and comfort options.

The current target is intentionally simple: **prove that FH2's cockpit camera can be driven reliably before touching stereo rendering**.
