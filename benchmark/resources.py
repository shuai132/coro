"""Report CPU, memory and allocations from separate fresh processes."""
import argparse
import subprocess


PRIMARY_LIBRARIES = (0, 1, 2)


def parse_table(text):
    lines = [line.strip() for line in text.splitlines() if line.strip().startswith("|")]
    assert len(lines) >= 3, text
    cells = lambda line: [value.strip() for value in line.strip("|").split("|")]
    header = cells(lines[0])
    assert len(cells(lines[1])) == len(header), text
    rows = []
    for line in lines[2:]:
        values = cells(line)
        assert len(values) == len(header), text
        rows.append(dict(zip(header, values)))
    return rows


def collect(binary, allocations, count, diagnostics=False):
    rows = []
    for work in range(3):
        for library in (range(6) if diagnostics else PRIMARY_LIBRARIES):
            fields = []
            for executable in (binary, allocations):
                result = subprocess.run(
                    [executable, "--resource", str(library), str(work), str(count)],
                    check=True, text=True, capture_output=True, timeout=60,
                )
                parsed = parse_table(result.stdout)
                assert len(parsed) == 1, result.stdout
                fields.append(parsed[0])
            row = fields[0]
            row.update({k: v for k, v in fields[1].items() if k.startswith("new_")})
            assert int(row["checksum"]) == count * (count + 1) // 2
            assert int(row["peak_rss_bytes"]) > 0
            assert int(row["new_calls"]) > 0
            assert int(row["new_peak_live_bytes"]) <= int(row["new_requested_bytes"])
            assert 0 <= int(row["new_retained_bytes"]) <= int(row["new_peak_live_bytes"])
            rows.append(row)
    return rows


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("allocations")
    parser.add_argument("count", type=int, nargs="?", default=200000)
    parser.add_argument("--diagnostics", action="store_true")
    args = parser.parse_args()
    rows = collect(args.binary, args.allocations, args.count, args.diagnostics)
    print("# Benchmark resource report\n")
    print(f"Iterations per workload: {args.count}. CPU/RSS and allocations use separate fresh processes.\n")
    print("C++ new counters exclude direct malloc/aligned_alloc. RSS includes process startup and runtime libraries.\n")
    for work in ("immediate", "nested_8", "post"):
        selected = [row for row in rows if row["workload"] == work]
        print(f"## {work}\n")
        print("| Library | CPU ms | Peak RSS MiB | C++ new calls | Requested B | Peak live B | Retained B |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for row in selected:
            cpu_ms = (int(row["user_cpu_us"]) + int(row["system_cpu_us"])) / 1000
            print(f"| {row['library']} | {cpu_ms:.3f} | {int(row['peak_rss_bytes']) / 1048576:.3f} | "
                  f"{row['new_calls']} | {row['new_requested_bytes']} | "
                  f"{row['new_peak_live_bytes']} | {row['new_retained_bytes']} |")
        print()
