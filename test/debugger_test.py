"""Run with python3 test/debugger_test.py; no debugger installation required."""

import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "coro_debug", Path(__file__).resolve().parents[1] / "tools/debugger/coro_debug.py"
)
debug = importlib.util.module_from_spec(spec)
spec.loader.exec_module(debug)


def node(identity, parent=0):
    return dict(id=identity, frame=identity * 16, parent=parent, external=0,
                completed=False, function="task%d" % identity, file="test.cpp", line=identity)


class FakeBackend:
    def __init__(self):
        self.records = {
            16: dict(next_=32, prev_=0, id_=1, frame_=256, parent_id_=0,
                     external_parent_=0, completed_=0, function_="root", file_="test.cpp", line_=1),
            32: dict(next_=0, prev_=16, id_=2, frame_=512, parent_id_=1,
                     external_parent_=0, completed_=0, function_="child", file_="test.cpp", line_=2),
        }
        self.updating = False

    def global_int(self, name):
        return self.updating

    def head(self):
        return 16

    def dereference(self, pointer):
        return self.records[pointer]

    integer = staticmethod(int)
    string = staticmethod(str)
    field = staticmethod(lambda record, key: record[key])


class DebuggerTest(unittest.TestCase):
    def test_snapshot_and_chain(self):
        nodes = debug.snapshot(FakeBackend())
        result = debug.render(nodes, "bt", "0x200")
        self.assertIn("#0 id:2", result)
        self.assertIn("#1 id:1", result)
        self.assertEqual(result, debug.render(nodes, "bt", "id:2"))

    def test_completed_and_empty(self):
        self.assertIn("No live", debug.render({}, "list", ""))
        item = node(1)
        item["completed"] = True
        self.assertIn("[completed]", debug.describe(item))

    def test_missing_parent_and_reused_address(self):
        # IDs prevent a reused frame address from becoming a false parent.
        nodes = {1: node(1, 9), 2: node(2)}
        nodes[2]["frame"] = 9 * 16
        self.assertIn("parent id:9 no longer live", debug.backtrace(nodes, 1))

    def test_external_parent(self):
        item = node(1)
        item["external"] = 255
        self.assertIn("external parent frame=0xff", debug.backtrace({1: item}, 1))

    def test_cycles_and_depth(self):
        self.assertIn("cycle", debug.backtrace({1: node(1, 2), 2: node(2, 1)}, 1))
        nodes = {i: node(i, i + 1) for i in range(1, debug.MAX_DEPTH + 2)}
        self.assertIn("depth limit", debug.backtrace(nodes, 1))

    def test_corrupt_registry(self):
        backend = FakeBackend()
        backend.records[32]["next_"] = 16
        with self.assertRaisesRegex(RuntimeError, "cyclic"):
            debug.snapshot(backend)
        backend = FakeBackend()
        backend.records[32]["prev_"] = 0
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            debug.snapshot(backend)

    def test_interrupted_update(self):
        backend = FakeBackend()
        backend.updating = True
        with self.assertRaisesRegex(RuntimeError, "update interrupted"):
            debug.snapshot(backend)

    def test_invalid_arguments(self):
        for command, args in [("list", "extra"), ("bt", ""), ("bt", "id:7"), ("bt", "resume()")]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                debug.render({1: node(1)}, command, args)


if __name__ == "__main__":
    unittest.main()
