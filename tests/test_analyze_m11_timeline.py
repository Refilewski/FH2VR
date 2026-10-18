"""Fast offline tests for historical M11.4 and experimental M11.5 traces."""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ANALYZER = Path(__file__).resolve().parents[1] / "tools" / "analyze_m11_timeline.py"
spec = importlib.util.spec_from_file_location("fh2vr_m11_timeline_analyzer", ANALYZER)
analyzer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = analyzer
spec.loader.exec_module(analyzer)


def make_csv(records, *, ready="yes", installed="yes", dropped="0"):
    header = (
        "# FH2VR M11.4 camera-aware frame timing capture, no VR required\n"
        f"# camera_ready_before_capture={ready} camera_wait_ms=500 "
        f"probe_installed={installed} camera_hook_calls=12 camera_view_valid_calls=8\n"
        f"# samples={len(records)} dropped={dropped} capture_ms=22000\n"
        "index,time_us,thread_id,event,arg_a,arg_b,view_tx,view_ty,view_tz\n"
    )
    return header + "\n".join(
        f"{i},{time},{thread},{event},{arg_a},0,{x},{y},{z}"
        for i, (time, thread, event, arg_a, x, y, z) in enumerate(records)
    ) + "\n"


class TimelineAnalyzerTests(unittest.TestCase):
    def load(self, source):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "FH2VR-FrameTimeline-M11.4.csv"
            path.write_text(source, encoding="utf-8")
            return analyzer.read_capture(path)

    def test_camera_and_render_event_correlation(self):
        rows = [
            (1000, 11, "game-present", 1, 0, 0, 0),
            (2000, 22, "camera-post-update", 1, 11, 22, 33),
            (3000, 33, "game-direct-execute", 2, 0, 0, 0),
            (6000, 11, "game-present", 1, 0, 0, 0),
            (7000, 22, "camera-post-update", 1, 12, 22, 33),
            (8000, 33, "game-direct-execute", 1, 0, 0, 0),
            (11000, 11, "game-present", 1, 0, 0, 0),
        ]
        metadata, events = self.load(make_csv(rows))
        self.assertEqual(metadata["probe_installed"], "yes")
        self.assertEqual(metadata["camera_ready_before_capture"], "yes")
        self.assertEqual(metadata["dropped"], "0")
        self.assertEqual(metadata["camera_hook_calls"], "12")
        result = analyzer.report(metadata, events)
        self.assertIn("camera=2 valid_camera=2 direct_execute=2 present=3", result)
        self.assertIn("median=5.000 ms", result)
        self.assertIn("median camera=1", result)
        self.assertIn("median DIRECT executions=1", result)
        self.assertIn("median=4.000 ms", result)
        self.assertIn("Frames with valid camera update=2/2", result)

    def test_missing_camera_reports_diagnostics_without_inventing_causality(self):
        rows = [
            (0, 11, "game-present", 1, 0, 0, 0),
            (1000, 33, "game-direct-execute", 1, 0, 0, 0),
            (6000, 11, "game-present", 1, 0, 0, 0),
        ]
        metadata, events = self.load(make_csv(rows, ready="no", installed="no", dropped="5"))
        result = analyzer.report(metadata, events)
        self.assertIn("probe_installed=no", result)
        self.assertIn("Camera probe was not installed", result)
        self.assertIn("Capture started without an active Driver camera", result)
        self.assertIn("Event buffer overflowed", result)
        self.assertIn("No valid camera view matrix samples", result)
        self.assertIn("These correlations do not prove render-pass order", result)

    def test_historical_stale_camera_gate_warning(self):
        rows = [
            (1000, 11, "game-present", 1, 0, 0, 0),
            (2000, 33, "game-direct-execute", 1, 0, 0, 0),
            (6000, 11, "game-present", 1, 0, 0, 0),
        ]
        meta, events = self.load(make_csv(rows, ready="yes", installed="yes"))
        result = analyzer.report(meta, events)
        self.assertIn("Camera was previously active, but no valid events", result)

    def test_m11_6_explicit_no_camera_during_capture(self):
        rows = [
            (1000, 11, "game-present", 1, 0, 0, 0),
            (6000, 11, "game-present", 1, 0, 0, 0),
        ]
        data = make_csv(rows)
        data = data.replace(
            "# samples=", 
            "# camera_hooks_during_capture=0 camera_valid_during_capture=0 "
            "camera_last_valid_age_start_ms=9900\n# samples="
        )
        meta, events = self.load(data)
        result = analyzer.report(meta, events)
        self.assertIn("Hook calls during capture=0 valid=0", result)
        self.assertIn("No camera callbacks occurred during capture", result)

    def test_camera_slot_replaced_is_reported(self):
        rows = [(1000, 11, "game-present", 1, 0, 0, 0),
                (6000, 11, "game-present", 1, 0, 0, 0)]
        data = make_csv(rows).replace(
            "# samples=",
            "# camera_hook_slot_at_start=1 camera_hook_slot_at_end=0\n# samples="
        )
        meta, events = self.load(data)
        report = analyzer.report(meta, events)
        self.assertIn("Camera hook slot: start=1 end=0", report)
        self.assertIn("no longer occupies the Driver camera vtable slot", report)

    def test_m11_6_one_camera_per_present_and_phase_order(self):
        rows = [
            (0, 11, "game-present", 0, 0, 0, 0),
            (2000, 11, "game-direct-execute", 3, 0, 0, 0),
            (3000, 22, "camera-post-update", 1, 1, 2, 3),
            (12980, 11, "game-direct-execute", 5, 0, 0, 0),
            (13000, 11, "game-present", 0, 0, 0, 0),
            (15100, 11, "game-direct-execute", 3, 0, 0, 0),
            (16000, 22, "camera-post-update", 1, 2, 3, 4),
            (25980, 11, "game-direct-execute", 5, 0, 0, 0),
            (26000, 11, "game-present", 0, 0, 0, 0),
        ]
        meta, events = self.load(make_csv(rows))
        result = analyzer.report(meta, events)
        self.assertIn("Frames with valid camera update=2/2", result)
        self.assertIn("Full camera/3-list/5-list intervals=2/2", result)
        self.assertIn("CPU phase order execute3 -> camera -> execute5: 2/2", result)
        self.assertIn("execute3=2.050ms", result)

    def test_m11_7_matrix_phase_change_outside_camera_callback(self):
        csv_text = (
            "# camera_ready_before_capture=yes probe_installed=yes dropped=0\n"
            "index,time_us,thread_id,event,arg_a,arg_b,view_tx,view_ty,view_tz\n"
            "0,0,11,game-present,0,512,0,0,0\n"
            "1,2000,11,game-direct-execute,3,1,1,2,3\n"
            "2,2500,22,camera-pre-update,1,0,4,2,3\n"
            "3,3000,22,camera-post-update,1,0,4,2,3\n"
            "4,12980,11,game-direct-execute,5,1,7,2,3\n"
            "5,13000,11,game-present,0,512,0,0,0\n"
        )
        meta, events = self.load(csv_text)
        result = analyzer.report(meta, events)
        self.assertIn("complete ordered 3/pre/post/5 frames=1/1", result)
        self.assertIn("3-list -> camera-pre: changed >0.01 in 1/1", result)
        self.assertIn("camera-pre -> camera-post: changed >0.01 in 0/1", result)
        self.assertIn("camera-post -> 5-list: changed >0.01 in 1/1", result)

    def test_invalid_header_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing columns"):
            self.load("index,event,time_us\n0,game-present,1\n")

    def test_bad_numeric_row_is_rejected(self):
        csv_data = make_csv([(1000, 11, "game-present", 1, 0, 0, 0)])
        with self.assertRaisesRegex(ValueError, "Invalid timeline row"):
            self.load(csv_data.replace(",1000,", ",not_a_time,"))

    def test_unsorted_events_are_sorted_before_frame_analysis(self):
        rows = [
            (5000, 22, "camera-post-update", 1, 2, 3, 4),
            (1000, 11, "game-present", 1, 0, 0, 0),
            (6000, 11, "game-present", 1, 0, 0, 0),
        ]
        metadata, events = self.load(make_csv(rows))
        self.assertEqual([e.time_us for e in events], [1000, 5000, 6000])
        self.assertIn("Frames with valid camera update=1/1", analyzer.report(metadata, events))


if __name__ == "__main__":
    unittest.main()
