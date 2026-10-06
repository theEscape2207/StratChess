#!/usr/bin/env python3
"""Summarise the strength lab's per-shard CPU samples into one markdown table.

Each shard's cpu_sampler.sh writes cpu.tsv. A shard's figure is the mean of its
samples; the table gives the mean, minimum and maximum of those across shards.
Per-vCPU busy is sorted within each sample before averaging, so it shows how
evenly the load spreads over the vCPUs rather than which index was busiest.
Evidence only: a missing or empty file is reported, never fatal.

Usage:
    summarize_cpu.py shards/*/      (one argument per shard directory)
    summarize_cpu.py --self-test
"""

import argparse
from pathlib import Path
import sys
import tempfile

COLUMNS = ("elapsed_s", "busy_pct", "steal_pct", "per_cpu_busy_pct", "load_1m")


def read_shard(path):
    """Return (busy, steal, load, sorted per-vCPU) means, or None without samples."""
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    if not lines or tuple(lines[0].split("\t")) != COLUMNS:
        return None
    rows = []
    for line in lines[1:]:
        fields = line.split("\t")
        try:
            per_cpu = sorted((float(value) for value in fields[3].split(",")), reverse=True)
            rows.append((float(fields[1]), float(fields[2]), float(fields[4]), per_cpu))
        except (IndexError, ValueError):
            continue
    if not rows or len({len(row[3]) for row in rows}) != 1:
        return None
    count = len(rows)
    per_cpu = [sum(row[3][index] for row in rows) / count for index in range(len(rows[0][3]))]
    return (sum(row[0] for row in rows) / count, sum(row[1] for row in rows) / count,
            sum(row[2] for row in rows) / count, per_cpu)


def summarize(paths):
    shards = [shard for shard in (read_shard(path) for path in paths) if shard]
    lines = ["### Runner CPU use", "",
             f"Sampled once a minute on {len(shards)} of {len(paths)} shards. Busy excludes idle, "
             "iowait and steal; steal is vCPU time the hypervisor gave to another guest.", ""]
    if not shards:
        return "\n".join(lines + ["No CPU samples were retained."]) + "\n"
    lines += ["| | Mean | Min shard | Max shard |", "|---|---|---|---|"]
    for title, index, unit in (("Busy, all vCPUs", 0, "%"), ("Steal", 1, "%"), ("Load average (1 min)", 2, "")):
        values = [shard[index] for shard in shards]
        lines.append(f"| {title} | {sum(values) / len(values):.1f}{unit} | "
                     f"{min(values):.1f}{unit} | {max(values):.1f}{unit} |")
    widths = {len(shard[3]) for shard in shards}
    if len(widths) == 1:
        per_cpu = [sum(shard[3][index] for shard in shards) / len(shards) for index in range(widths.pop())]
        lines += ["", "Per-vCPU busy, busiest first: " + " / ".join(f"{value:.0f}%" for value in per_cpu) + "."]
    return "\n".join(lines) + "\n"


def self_test():
    failures = 0

    def expect(description, condition):
        nonlocal failures
        print(f"{'ok' if condition else 'FAIL'}: {description}")
        failures += not condition

    header = "\t".join(COLUMNS) + "\n"
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "a.tsv").write_text(header + "60\t70.0\t1.0\t90,80,60,50\t3.00\n"
                                    "120\t80.0\t3.0\t40,100,70,90\t3.40\n", encoding="utf-8")
        (root / "b.tsv").write_text(header + "60\t60.0\t0.0\t61,61,61,61\t2.60\n", encoding="utf-8")
        (root / "empty.tsv").write_text(header, encoding="utf-8")
        (root / "junk.tsv").write_text("not a sample file\n", encoding="utf-8")
        a = read_shard(root / "a.tsv")
        expect("a shard's means average its samples", [round(value, 6) for value in a[:3]] == [75.0, 2.0, 3.2])
        expect("per-vCPU busy is sorted within each sample before averaging", a[3] == [95.0, 85.0, 65.0, 45.0])
        expect("a header-only file has no samples", read_shard(root / "empty.tsv") is None)
        expect("an unrecognised file has no samples", read_shard(root / "junk.tsv") is None)
        expect("a missing file has no samples", read_shard(root / "missing.tsv") is None)
        report = summarize([root / name for name in ("a.tsv", "b.tsv", "empty.tsv", "missing.tsv")])
        expect("only shards with samples are counted", "on 2 of 4 shards" in report)
        expect("busy is the mean of shard means, with the range", "| Busy, all vCPUs | 67.5% | 60.0% | 75.0% |" in report)
        expect("load has no percent sign", "| Load average (1 min) | 2.9 | 2.6 | 3.2 |" in report)
        expect("per-vCPU spread is pooled across shards", "78% / 73% / 63% / 53%" in report)
        expect("no samples at all is reported, not fatal",
               "No CPU samples were retained." in summarize([root / "empty.tsv"]))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("shards", nargs="*", help="Shard directories, each holding its cpu.tsv")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    sys.stdout.write(summarize([Path(shard) / "cpu.tsv" for shard in args.shards]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
