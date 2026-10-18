"""Record scoped Cachegrind counters and C++ heap allocations on ARM64 Linux."""

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
EVENTS = "Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw Bc Bcm Bi Bim".split()
ALLOCATION_FIELDS = ("new_calls", "new_requested_bytes", "new_peak_live_bytes", "new_live_bytes")
CACHES = {
    "I1": {"size_bytes": 32768, "associativity": 8, "line_size_bytes": 64},
    "D1": {"size_bytes": 32768, "associativity": 8, "line_size_bytes": 64},
    "LL": {"size_bytes": 1048576, "associativity": 16, "line_size_bytes": 64},
}


def read_events(text):
    events = [line.split()[1:] for line in text.splitlines() if line.startswith("events:")]
    summaries = [line.split()[1:] for line in text.splitlines() if line.startswith("summary:")]
    if len(events) != 1 or len(summaries) != 1 or len(set(events[0])) != len(events[0]):
        raise ValueError("Missing or ambiguous Cachegrind events/summary")
    if not set(EVENTS).issubset(events[0]):
        raise ValueError("Missing Cachegrind counters; enable cache and branch simulation")
    if len(events[0]) != len(summaries[0]) or not all(value.isdecimal() for value in summaries[0]):
        raise ValueError("Invalid Cachegrind summary")
    values = dict(zip(events[0], map(int, summaries[0])))
    if values["Ir"] <= 0:
        raise ValueError("The measured region contains no instructions")
    return {event: values[event] for event in EVENTS + sorted(set(values) - set(EVENTS))}


def cachegrind_command(profile):
    # Use a fixed synthetic model instead of the host's detected cache geometry.
    return ["valgrind", "--tool=cachegrind", "--instr-at-start=no", "--cache-sim=yes", "--branch-sim=yes"] + [
        f"--{name}={cache['size_bytes']},{cache['associativity']},{cache['line_size_bytes']}"
        for name, cache in CACHES.items()
    ] + [f"--cachegrind-out-file={profile}"]


def read_allocations(text, case, iterations):
    record = json.loads(text)
    if not isinstance(record, dict) or record.get("scenario") != case:
        raise ValueError("Missing or mismatched allocation scenario")
    for field, expected in (("iterations", iterations), ("checksum", iterations * (iterations + 1) // 2)):
        if type(record.get(field)) is not int or record[field] != expected:
            raise ValueError(f"Invalid allocation workload {field}")
    if any(type(record.get(field)) is not int or record[field] < 0 for field in ALLOCATION_FIELDS):
        raise ValueError("Missing or invalid allocation counters")
    if not 0 <= record["new_live_bytes"] <= record["new_peak_live_bytes"] <= record["new_requested_bytes"]:
        raise ValueError("Inconsistent allocation byte counts")
    if record["new_calls"] == 0 and record["new_requested_bytes"] != 0:
        raise ValueError("Allocated bytes without any allocations")
    return {field: record[field] for field in ALLOCATION_FIELDS}


def run(*args, capture=False):
    return subprocess.run(args, check=True, text=True, capture_output=capture, timeout=300).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build_release" / "instructions")
    parser.add_argument("--output", type=Path, default=ROOT / "benchmark" / "instructions-arm64.json")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64"):
        parser.error("Recording requires ARM64 Linux; use an ARM64 Linux VM/container on macOS")

    build = args.build_dir.resolve()
    run("cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=g++-13",
        "-DCMAKE_CXX_FLAGS=", "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG",
        "-DCORO_BUILD_TEST=OFF", "-DCORO_BUILD_EXAMPLES=OFF", "-DCORO_BUILD_BENCHMARKS=OFF",
        "-DCORO_BUILD_INSTRUCTION_BENCHMARKS=ON", "-DCORO_DISABLE_EXCEPTION=OFF", "-DCORO_SINGLE_THREAD=OFF",
        "-DCORO_ENABLE_FRAME_CACHE=OFF", "-DCORO_ENABLE_LOCAL_STORAGE=OFF", "-DCORO_DEBUG_ASYNC_STACK=OFF",
        "-DCORO_ENABLE_SANITIZE_ADDRESS=OFF", "-DCORO_ENABLE_SANITIZE_THREAD=OFF")
    run("cmake", "--build", str(build), "--target", "coro_instructions", "coro_instructions_check", "coro_instructions_allocations", "-j", "2")
    run("ctest", "--test-dir", str(build), "-R", "^coro_instructions", "--output-on-failure")
    binary = str(build / "coro_instructions")
    allocations_binary = str(build / "coro_instructions_allocations")
    cases = run(binary, "--list", capture=True).splitlines()
    if not cases or len(set(cases)) != len(cases):
        raise ValueError("Missing or duplicated benchmark scenarios")
    if run(allocations_binary, "--list", capture=True).splitlines() != cases:
        raise ValueError("Allocation and Cachegrind scenarios differ")

    result = {
        "schema_version": 3,
        "workload_version": 1,
        "platform": "aarch64-linux",
        "compiler": run("g++-13", "--version", capture=True).splitlines()[0],
        "valgrind": run("valgrind", "--version", capture=True).strip(),
        "runtime_packages": run("dpkg-query", "-W", "-f=${Package}=${Version}\n", "libc6", "libstdc++6", capture=True).splitlines(),
        "configuration": {
            "language_standard": "c++20",
            "optimization": "-O3 -DNDEBUG -g -foptimize-sibling-calls -march=armv8-a -mtune=generic",
            "exceptions": True,
            "single_thread_mode": False,
            "frame_cache": False,
            "iterations": 10000,
            "warmup_iterations": 100,
            "cache_simulation": CACHES,
            "branch_simulation": True,
            "allocation_measurement": "separate_process_cpp_new",
        },
        "metrics": {},
    }
    for case in cases:
        profile = build / f"{case}.cgout"
        run(*cachegrind_command(profile), binary, case, "10000")
        result["metrics"][case] = read_events(profile.read_text())
        allocations = run(allocations_binary, case, "10000", capture=True)
        (build / f"{case}.allocations.json").write_text(allocations)
        result["metrics"][case].update(read_allocations(allocations, case, 10000))

    # Replace the record only after every scenario and metadata query succeeds.
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, delete=False, encoding="utf-8") as temporary:
        temporary.write(json.dumps(result, indent=2) + "\n")
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, output)
    finally:
        temporary_path.unlink(missing_ok=True)
    print(f"Recorded {len(cases)} scenarios in {output.name}")


if __name__ == "__main__":
    main()
