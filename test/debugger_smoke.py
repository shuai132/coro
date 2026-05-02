"""Integration: python3 test/debugger_smoke.py gdb|lldb build/coro_debug [debugger-path] [--core]."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kind", choices=["gdb", "lldb"])
parser.add_argument("executable")
parser.add_argument("debugger", nargs="?")
parser.add_argument("--core", action="store_true", help="also save and inspect a core dump")
options = parser.parse_args()
script = Path(__file__).resolve().parents[1] / "tools/debugger/coro_debug.py"
debugger = options.debugger or options.kind


def run_commands(commands, core=None):
    if options.kind == "gdb":
        args = [debugger, "-nx", "-batch", options.executable]
        if core:
            args.extend(["-c", str(core)])
        option = "-ex"
    else:
        args = [debugger, "--no-lldbinit", "-b"]
        # Mach-O cores identify the original image. Adding it again can duplicate sections.
        args.extend(["-c", str(core)] if core else [options.executable])
        option = "-o"
    for command in commands:
        args.extend([option, command])
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    print(result.stdout)
    if result.returncode:
        raise SystemExit(result.returncode)
    return result.stdout


def check_chain(output):
    for depth, name in enumerate(["debug_leaf", "debug_middle", "debug_root"]):
        pattern = r"#%d id:\d+ frame=0x[0-9a-f]+ \[live\].*%s.*coro_debug.cpp:\d+" % (depth, name)
        if not re.search(pattern, output):
            raise SystemExit("missing async frame: " + name)


if options.kind == "gdb":
    load = "source %s" % script
    stop = "break coro_debug_checkpoint"
    save = "generate-core-file"
else:
    load = 'command script import "%s"' % script
    stop = "breakpoint set -n coro_debug_checkpoint"
    save = "process save-core"
stack_commands = ["coro-list", "coro-bt id:4"]
if options.kind == "gdb":
    # A stop in C code must not prevent lookup of the C++ registry symbols.
    stack_commands = ["set language c", *stack_commands, "set language auto", *stack_commands]
with tempfile.TemporaryDirectory(prefix="coro-debug-") as directory:
    core = Path(directory) / "fixture.core"
    commands = [load, stop, "run", *stack_commands]
    if options.core:
        commands.append(("%s %s" if options.kind == "gdb" else '%s "%s"') % (save, core))
    commands.append("continue")
    output = run_commands(commands)
    check_chain(output)
    expected_exit = "exited normally" if options.kind == "gdb" else "exited with status = 0"
    if expected_exit not in output:
        raise SystemExit("fixture did not exit normally")
    if options.core:
        check_chain(run_commands([load, *stack_commands], core))
print("Debugger async stack smoke test passed.")
