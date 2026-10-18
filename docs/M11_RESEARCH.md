# FH2VR M11.5: camera / D3D12 timeline research

> Research-only branch. This is **not** a stereo renderer. Keep PR #10 unmerged until the traces are understood. The existing M10 mono path remains the user-facing build.

## Goal

Find when the FH2 Driver camera updates relative to the game's primary D3D12 DIRECT queue and swapchain Present. This timing data helps narrow down where an independent left/right world pass might be possible. Timestamps alone **do not** prove which command list draws the world or whether a second eye render is safe.

## Run one capture (no VR headset required)

1. Open **PR #10** > **Checks** > successful **Build FH2VR** workflow, download its standalone or FH2ML artifact. Do not use the `dev-latest` release: it is built from the ordinary M10 code, not this research branch.
2. Use **one** runtime: either `FH2VR.exe` standalone or the FH2ML package. Do not launch both.
3. Launch Azimuth / FH2 1.0.0.64. Get into free roam in the **Driver camera** and drive/turn normally.
4. The probe looks for active Driver camera callbacks before starting the 22-second timeline. Leave the game in free roam long enough for the capture to complete (up to about 75 seconds after startup if the camera activates late).
5. Close the game normally and find:
   - `%LOCALAPPDATA%\FH2VR\logs\FH2VR-FrameTimeline-M11.5.csv`
   - `%LOCALAPPDATA%\FH2VR\logs\FH2VR-Callstack-M11.2.log`
   - `%LOCALAPPDATA%\FH2VR\logs\FH2VR-CameraResearch.log` (if generated)

Only these FH2VR diagnostic logs are needed. Do **not** upload game binaries, license keys, game saves, or complete memory dumps.

## What the CSV means

- `camera-post-update` is recorded **after** the chained Driver camera callback. `view_t*` are read from the cached camera view matrix after that update. Earlier M11.4 builds accidentally logged the *pre-update* values under this label.
- `game-direct-execute` marks game-owned command-list submission on the primary DIRECT queue, not FH2VR's texture-conversion commands. `arg_a` is the submitted command-list count.
- `game-present` marks the primary swapchain before FH2VR's copy/conversion. `arg_a` is the sync interval and `arg_b` the Present flags.
- `time_us` comes from QueryPerformanceCounter on each hook's calling thread. It is a monotonic ordering aid, **not** a GPU timestamp.
- The header reports `probe_installed`, `camera_ready_before_capture`, camera callback counts, sample count and overflow (`dropped`).

## Interpreting traces before attempting stereo

First verify that the camera probe installed, that camera data is valid and changing in Driver camera, and that `dropped=0`. Then inspect camera-to-queue-to-Present ordering across several consecutive frames and distinct thread IDs. If the camera callback is not in the world-render path, or multiple camera updates map to the same Present, gather more targeted call stacks before inserting stereo camera shifts.

## Known constraints

- The live display remains single-image mono with M10.2 UV-based positional parallax.
- No second world pass, independent eye projection or depth target has yet been validated.
- Hooks should be considered research-only and may affect frame pacing. Do not publish M11 artifacts as stable releases.
