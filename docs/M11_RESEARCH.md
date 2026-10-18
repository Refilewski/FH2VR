# FH2VR M11.4–M11.6: camera / D3D12 timeline research

> Research-only branch. This is **not** a stereo renderer. Keep PR #10 unmerged until the traces are understood. The existing M10 mono path remains the user-facing build.

## Goal

Find when the FH2 Driver camera updates relative to the game's primary D3D12 DIRECT queue and swapchain Present. This timing data helps narrow down where an independent left/right world pass might be possible. Timestamps alone **do not** prove which command list draws the world or whether a second eye render is safe.

## First reproduce the unchanged M11.4 baseline

The original M11.4 diagnostic build is **commit `7ead44d26189f240ace587159d42f9b1b8c083db`**, from [successful GitHub Actions run #127](https://github.com/Refilewski/FH2VR/actions/runs/39120533828). Download the `FH2VR-Standalone-0.4.0-dev` artifact from **that exact run** (artifact ID `12134340662`, confirmed available on 18 Oct 2026). This produces:

```text
%LOCALAPPDATA%\FH2VR\logs\FH2VR-FrameTimeline-M11.4.csv
```

This is the correct build if the task is specifically to complete M11.4's outstanding hardware-free test. Do **not** substitute the public `dev-latest` release or treat a newer PR artifact as identical.

After reviewing M11.4 logs, the later experimental research branch revision may be tested separately. Its post-callback view reads write `FH2VR-FrameTimeline-M11.5.csv`. That distinction matters because M11.4's event name said `camera-post-update` even though the recorded matrix translation came from **before** the chained callback. The M11.5 instrumentation fixes which side of the callback is sampled, but does not prove the cached matrix is recomputed inside that callback.

Analyze either CSV with:

```powershell
python tools/analyze_m11_timeline.py "$env:LOCALAPPDATA\FH2VR\logs\FH2VR-FrameTimeline-M11.4.csv"
```

The Python analyzer and its tests live only in the research branch. It can run on existing CSV files and does not inject anything into Azimuth.

## M11.5 in-game result (user capture, 18 October 2026)

The user supplied `FH2VR-FrameTimeline-M11.5.csv` (not a headset test). **This capture is conclusive about the CPU hook activity within this 22-second window, but not about the world-render function.**

- Header: `probe_installed=yes`, `camera_ready_before_capture=yes`, `camera_wait_ms=0`, `camera_hook_calls=135`, `camera_view_valid_calls=135`, `samples=19706`, `dropped=0`.
- Recorded events: **camera 0**, **ExecuteCommandLists 18315**, **Present 1391**. No nonzero `view_t*` fields. All Execute and Present events were on thread ID `33348`.
- Present rate ~63.2 FPS over 22 s; median Present-to-Present interval **15.78165 ms**.
- Execute argument counts: 15533 with one list, 1391 with three lists, 1391 with five lists. Between each of 1390 complete Present intervals, there was exactly **one** three-list submission and **one** five-list submission; total Execute calls per interval ranged **12–15** (median 13). These are queue submissions, **not proven separate world passes**.
- **Likely instrumentation issue:** the M11.5 `camera_ready_before_capture=yes` gate only looked at cumulative valid callbacks. The 135 successful calls may all predate recording. The CSV alone cannot identify which camera mode or game state produced them, nor prove that the hook is used for the active world view.

## Follow-up: M11.6 fresh-camera capture gate (research branch only)

The experimental M11.6 update improves diagnostic confidence **without camera writes, stereo changes, yaw/pitch changes or any Azimuth file modifications**.

- After the initial delay, require **five new valid camera callbacks in a rolling two-second window**, with the last one seen within 500 ms, rather than relying on the process-lifetime total. Remove the extra 500 ms delay after readiness.
- Still fall back after 45 seconds with `camera_ready_before_capture=no`, and record 22 seconds of GPU-hook events for diagnostics.
- Emit **`FH2VR-FrameTimeline-M11.6.csv`** to avoid overwriting the provided M11.5 evidence.
- Header adds `camera_hook_calls_before_capture`, `camera_view_valid_calls_before_capture`, `camera_hooks_during_capture`, `camera_valid_during_capture`, `camera_last_valid_age_start_ms`, and `camera_last_valid_age_end_ms`. A value of `-1` for age means no valid callback was observed.
- Header also reports read-only `camera_hook_slot_at_start` and `camera_hook_slot_at_end`: `1` means M11 probe still owns the Driver vtable slot, `0` means the slot points elsewhere, `-1` means the state is unavailable. This distinguishes an overwritten hook from a hook that is intact but not called. The probe does **not** rewrite the slot during this check.
- Run the branch-specific **standalone Actions artifact** and remain in FH2 Driver free-roam during the measurement. Do **not** use the ordinary `dev-latest` release or inject a second runtime into the same Azimuth PID.
- If callbacks are still absent or the fresh gate times out, investigate whether gameplay drives the Driver vtable slot at all rather than inferring anything about stereo from Execute/Present counts.

## Run one capture (no VR headset required)

1. Open **PR #10** > **Checks** > successful **Build FH2VR** workflow, download its standalone or FH2ML artifact. Do not use the `dev-latest` release: it is built from the ordinary M10 code, not this research branch.
2. Use **one** runtime: either `FH2VR.exe` standalone or the FH2ML package. Do not launch both.
3. Launch Azimuth / FH2 1.0.0.64. Get into free roam in the **Driver camera** and drive/turn normally.
4. The probe looks for active Driver camera callbacks before starting the 22-second timeline. Leave the game in free roam long enough for the capture to complete (up to about 75 seconds after startup if the camera activates late).
5. Close the game normally and find:
   - `%LOCALAPPDATA%\FH2VR\logs\FH2VR-FrameTimeline-M11.6.csv` (when testing current branch; retain the older M11.5 CSV separately)
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
