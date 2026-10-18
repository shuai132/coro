"""Regression tests for scoped Cachegrind and allocation records."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "record_instructions", Path(__file__).resolve().parents[1] / "benchmark" / "record_instructions.py"
)
RECORDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECORDER)


class InstructionRecordTests(unittest.TestCase):
    EVENTS = "Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw Bc Bcm Bi Bim".split()

    def profile(self, events=None, values=None):
        events = self.EVENTS if events is None else events
        values = [42] + [0] * 12 if values is None else values
        return f"events: {' '.join(events)}\nsummary: {' '.join(map(str, values))}\n"

    def test_reads_all_counters_by_event_name(self):
        expected = dict(zip(self.EVENTS, range(1, 14)))
        self.assertEqual(RECORDER.read_events(self.profile(self.EVENTS[::-1], range(13, 0, -1))), expected)

    def test_accepts_zero_misses(self):
        self.assertEqual(RECORDER.read_events(self.profile()), dict(zip(self.EVENTS, [42] + [0] * 12)))

    def test_ignores_per_line_counts(self):
        self.assertEqual(RECORDER.read_events("fl: source.cpp\nfn: f\n1 999\n" + self.profile())["Ir"], 42)

    def test_rejects_missing_or_invalid_summary(self):
        for text in ("", "events: Ir\nsummary: 42\n", "events: Ir\n",
                     self.profile(values=[0] * 13), self.profile(values=[42] + [-1] * 12),
                     self.profile(values=[42] + ["broken"] * 12), self.profile(values=[42]),
                     self.profile() + "summary: 42\n", self.profile() + "events: Ir\n",
                     self.profile(self.EVENTS + ["Dr"], [42] + [0] * 13)):
            with self.subTest(text=text), self.assertRaises(ValueError):
                RECORDER.read_events(text)

    def test_rejects_each_missing_counter(self):
        for missing in self.EVENTS:
            events = [event for event in self.EVENTS if event != missing]
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                RECORDER.read_events(self.profile(events, [42] * len(events)))


class AllocationRecordTests(unittest.TestCase):
    def record(self, **changes):
        result = {"scenario": "task", "iterations": 17, "checksum": 153,
                  "new_calls": 4, "new_requested_bytes": 210, "new_peak_live_bytes": 200, "new_live_bytes": 7}
        result.update(changes)
        return result

    def test_reads_allocation_counters(self):
        record = self.record()
        expected = {key: value for key, value in record.items() if key.startswith("new_")}
        self.assertEqual(RECORDER.read_allocations(json.dumps(record), "task", 17), expected)

    def test_accepts_zero_allocations(self):
        counters = dict.fromkeys(("new_calls", "new_requested_bytes", "new_peak_live_bytes", "new_live_bytes"), 0)
        self.assertEqual(RECORDER.read_allocations(json.dumps(self.record(**counters)), "task", 17), counters)

    def test_rejects_wrong_workload_and_invalid_counters(self):
        for changes in ({"scenario": "nested"}, {"iterations": 18}, {"checksum": 0},
                        {"new_calls": -1}, {"new_calls": True}, {"new_calls": 1.5}, {"new_calls": 0},
                        {"new_live_bytes": 201}, {"new_peak_live_bytes": 211}, {"new_requested_bytes": "210"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                RECORDER.read_allocations(json.dumps(self.record(**changes)), "task", 17)

    def test_rejects_missing_or_malformed_record(self):
        for missing in self.record():
            record = self.record()
            del record[missing]
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                RECORDER.read_allocations(json.dumps(record), "task", 17)
        for text in ("", "[]", "null", "{}", "{broken"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                RECORDER.read_allocations(text, "task", 17)


@unittest.skipUnless(os.environ.get("CORO_ALLOCATIONS_BINARY"), "Requires the allocation measurement executable")
class AllocationRegionTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([os.environ["CORO_ALLOCATIONS_BINARY"], *args], check=True,
                              capture_output=True, text=True, timeout=60).stdout

    def measure(self, case, iterations, outside=0):
        return RECORDER.read_allocations(self.invoke(case, str(iterations), str(outside)), case, iterations)

    def test_counter_accuracy_and_scope_isolation(self):
        self.invoke("--check-allocations")

    def test_all_scenarios_release_measured_allocations(self):
        cases = self.invoke("--list").splitlines()
        self.assertTrue(cases)
        for case in cases:
            for iterations in (0, 1, 17):
                with self.subTest(case=case, iterations=iterations):
                    self.assertEqual(self.measure(case, iterations)["new_live_bytes"], 0)

    def test_only_selected_allocations_are_counted(self):
        original = self.measure("task", 17)
        self.assertEqual(original, self.measure("task", 17, 1000000))
        longer = self.measure("task", 1000)
        self.assertGreater(longer["new_calls"], original["new_calls"])
        self.assertGreater(longer["new_requested_bytes"], original["new_requested_bytes"])


@unittest.skipUnless(os.environ.get("CORO_INSTRUCTIONS_BINARY"), "Requires the ARM64 measurement executable")
class MeasurementRegionTests(unittest.TestCase):
    def measure(self, iterations, outside=0):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "profile.cgout"
            subprocess.run(RECORDER.cachegrind_command(profile) + [
                # Keep argv lengths fixed so this check does not also change the stack layout.
                os.environ["CORO_INSTRUCTIONS_BINARY"], "task", f"{iterations:010d}", f"{outside:010d}",
            ], check=True, capture_output=True, text=True, timeout=60)
            return RECORDER.read_events(profile.read_text())

    def test_only_selected_work_is_counted(self):
        original = self.measure(17)
        self.assertEqual(original, self.measure(17, 10000))
        longer = self.measure(1000)
        for event in ("Ir", "Dr", "Dw", "Bc", "Bi"):
            with self.subTest(event=event):
                self.assertGreater(longer[event], original[event])


if __name__ == "__main__":
    unittest.main()
