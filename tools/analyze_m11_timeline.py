#!/usr/bin/env python3
"""Summarize FH2VR M11.5 CPU camera/D3D12/Present timelines.

No dependencies outside the Python standard library. This script does not
identify a GPU world pass; it only checks CPU-side event ordering.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import os
import re
import statistics
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Event:
    time_us: float
    tid: int
    name: str
    arg_a: int
    tx: float
    ty: float
    tz: float


def read_capture(path: Path) -> tuple[dict[str, str], list[Event]]:
    metadata: dict[str, str] = {}
    csv_lines: list[str] = []
    with path.open("r", encoding="utf-8-sig", errors="replace") as file:
        for line in file:
            if line.lstrip().startswith("#"):
                for key, value in re.findall(r"([a-z_]+)=(\S+)", line):
                    metadata[key] = value.strip()
            elif line.strip():
                csv_lines.append(line)
    if not csv_lines:
        raise ValueError("Missing CSV data")
    reader = csv.DictReader(csv_lines)
    required = {"time_us", "thread_id", "event", "arg_a", "view_tx", "view_ty", "view_tz"}
    if reader.fieldnames is None or not required.issubset(reader.fieldnames):
        raise ValueError("Not an M11 timeline CSV (missing columns)")
    result = []
    for row in reader:
        try:
            result.append(Event(
                float(row["time_us"]),
                int(row["thread_id"]),
                row["event"],
                int(row["arg_a"]),
                float(row["view_tx"]),
                float(row["view_ty"]),
                float(row["view_tz"]),
            ))
        except (TypeError, ValueError) as exc:
            raise ValueError("Invalid timeline row: " + str(row)) from exc
    result.sort(key=lambda item: item.time_us)
    return metadata, result


def median_or_none(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def report(meta: dict[str, str], events: list[Event], max_frames: int = 10) -> str:
    # Original capture order is CPU ordering; it does not identify any GPU pass.
    cameras = [e for e in events if e.name == "camera-post-update"]
    valid_cameras = [e for e in cameras if e.arg_a == 1]
    executes = [e for e in events if e.name == "game-direct-execute"]
    presents = [e for e in events if e.name == "game-present"]
    output = [
        "FH2VR M11.4-M11.6 timeline summary (CPU hook timestamps; not GPU timings)",
        f"Events: camera={len(cameras)} valid_camera={len(valid_cameras)} "
        f"direct_execute={len(executes)} present={len(presents)}",
        f"Threads: camera={len({e.tid for e in cameras})} "
        f"execute={len({e.tid for e in executes})} "
        f"present={len({e.tid for e in presents})}",
        f"Capture: probe_installed={meta.get('probe_installed', '?')} "
        f"camera_ready={meta.get('camera_ready_before_capture', '?')} "
        f"dropped={meta.get('dropped', '?')}",
    ]
    if "camera_hooks_during_capture" in meta:
        output.append(
            f"Hook calls during capture={meta['camera_hooks_during_capture']} "
            f"valid={meta.get('camera_valid_during_capture', '?')} "
            f"last_valid_age_at_start_ms={meta.get('camera_last_valid_age_start_ms', '?')}"
        )
    warnings = []
    if meta.get("probe_installed") == "no":
        warnings.append("Camera probe was not installed.")
    if meta.get("camera_ready_before_capture") == "no":
        warnings.append("Capture started without an active Driver camera.")
    if meta.get("dropped", "0").isdigit() and int(meta.get("dropped", "0")):
        warnings.append("Event buffer overflowed; some frame intervals may be incomplete.")
    if not valid_cameras:
        warnings.append("No valid camera view matrix samples.")
        if meta.get("camera_ready_before_capture") == "yes":
            warnings.append(
                "Camera was previously active, but no valid events were recorded. "
                "Older M11.4/M11.5 builds used a cumulative readiness counter; "
                "inspect runtime camera mode and use the M11.6 fresh-window gate."
            )
    if meta.get("camera_hooks_during_capture") == "0":
        warnings.append("No camera callbacks occurred during capture (M11.6 diagnostic).")
    slot_start = meta.get("camera_hook_slot_at_start")
    slot_end = meta.get("camera_hook_slot_at_end")
    if slot_start is not None or slot_end is not None:
        output.append(f"Camera hook slot: start={slot_start or '?'} end={slot_end or '?'} (1=ours, 0=elsewhere, -1=unknown)")
        if slot_start == "0" or slot_end == "0":
            warnings.append("The M11 camera probe no longer occupies the Driver camera vtable slot.")
        elif slot_start == "-1" or slot_end == "-1":
            warnings.append("Camera probe slot ownership could not be verified.")
    if not executes:
        warnings.append("No game-owned DIRECT queue executions.")
    if len(presents) < 2:
        warnings.append("Fewer than two Present calls; no full frame intervals.")
    if valid_cameras and len({(e.tx, e.ty, e.tz) for e in valid_cameras}) <= 1:
        warnings.append("Camera view translation did not change; verify actual driving/movement.")
    if len(presents) >= 2:
        intervals = [
            b.time_us - a.time_us
            for a, b in zip(presents, presents[1:])
            if b.time_us > a.time_us
        ]
        period = median_or_none(intervals)
        if period is not None:
            output.append(f"Present cadence: median={period / 1000:.3f} ms "
                          f"(~{1_000_000 / period:.1f} FPS across sampled CPU presents)")

        times = [event.time_us for event in events]
        camera_counts = []
        execute_counts = []
        camera_to_present = []
        preview = []
        for previous, current in zip(presents, presents[1:]):
            first = bisect.bisect_right(times, previous.time_us)
            last = bisect.bisect_right(times, current.time_us)
            frame_events = events[first:last]
            frame_cameras = [e for e in frame_events if e.name == "camera-post-update" and e.arg_a == 1]
            frame_executes = [e for e in frame_events if e.name == "game-direct-execute"]
            camera_counts.append(len(frame_cameras))
            execute_counts.append(len(frame_executes))
            lag = current.time_us - frame_cameras[-1].time_us if frame_cameras else None
            if lag is not None:
                camera_to_present.append(lag)
            if len(preview) < max_frames:
                preview.append(
                    f"  frame {len(camera_counts):3d}: camera={len(frame_cameras)} "
                    f"execute={len(frame_executes)} "
                    f"last_camera_to_present={lag / 1000:.3f}ms" if lag is not None else
                    f"  frame {len(camera_counts):3d}: camera=0 "
                    f"execute={len(frame_executes)} last_camera_to_present=n/a"
                )
        output.append(f"Per Present interval: median camera={statistics.median(camera_counts):g}, "
                      f"median DIRECT executions={statistics.median(execute_counts):g}")
        output.append(
            f"Frames with valid camera update={sum(n > 0 for n in camera_counts)}/{len(camera_counts)}"
        )
        lag = median_or_none(camera_to_present)
        if lag is not None:
            output.append(f"Camera-after-update -> next Present (same interval): "
                          f"median={lag / 1000:.3f} ms")
        output.append("First frame intervals:")
        output.extend(preview)
    if warnings:
        output.append("Warnings:")
        output.extend(f"  - {warning}" for warning in warnings)
    output.append("For M11.4 captures, camera coordinates were sampled before the chained callback despite the post-update event name.")
    output.append("M11.5/M11.6 read after the callback; this does not prove the game rebuilt the cached view matrix inside the callback.")
    output.append("These correlations do not prove render-pass order or native stereo viability.")
    return "\n".join(output)


def main() -> None:
    default_root = Path(os.getenv("LOCALAPPDATA", ".")) / "FH2VR" / "logs"
    candidates = [
        default_root / "FH2VR-FrameTimeline-M11.6.csv",
        default_root / "FH2VR-FrameTimeline-M11.5.csv",
        default_root / "FH2VR-FrameTimeline-M11.4.csv",
    ]
    default_csv = next((candidate for candidate in candidates if candidate.exists()), candidates[0])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="?", type=Path, default=default_csv,
                        help="M11.4 or M11.5 CSV (defaults to latest existing capture)")
    parser.add_argument("--frames", type=int, default=10,
                        help="number of example Present intervals to show")
    args = parser.parse_args()
    if args.frames < 0:
        parser.error("--frames must be >= 0")
    try:
        meta, events = read_capture(args.csv)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(report(meta, events, args.frames))


if __name__ == "__main__":
    main()
