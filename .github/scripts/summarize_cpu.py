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
from collections import namedtuple
from pathlib import Path
from statistics import fmean
import subprocess
import sys
import tempfile

COLUMNS = ("elapsed_s", "busy_pct", "steal_pct", "per_cpu_busy_pct", "load_1m")


Shard = namedtuple("Shard", "busy steal load per_cpu")


def column_means(rows):
    return [fmean(column) for column in zip(*rows)]


def read_shard(path):
    """Return the shard's mean Shard, per-vCPU sorted busiest first, or None without samples."""
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except (OSError, ValueError):
        return None
    if not lines or tuple(lines[0].split("\t")) != COLUMNS:
        return None
    rows = []
    for line in lines[1:]:
        fields = line.split("\t")
        try:
            per_cpu = sorted((float(value) for value in fields[3].split(",")), reverse=True)
            rows.append(Shard(float(fields[1]), float(fields[2]), float(fields[4]), per_cpu))
        except (IndexError, ValueError):
            continue
    # A row cut short when the sampler was killed must not discard the shard.
    rows = [row for row in rows if rows and len(row.per_cpu) == len(rows[0].per_cpu)]
    if not rows:
        return None
    busy, steal, load = column_means((row.busy, row.steal, row.load) for row in rows)
    return Shard(busy, steal, load, column_means(row.per_cpu for row in rows))


def summarize(paths):
    shards = [shard for shard in (read_shard(path) for path in paths) if shard]
    lines = ["### Runner CPU use", "",
             f"Sampled on {len(shards)} of {len(paths)} shards. Busy excludes idle, iowait and steal; "
             "steal is vCPU time the hypervisor gave to another guest.", ""]
    if not shards:
        return "\n".join(lines + ["No CPU samples were retained."]) + "\n"
    lines += ["| | Mean | Min shard | Max shard |", "|---|---|---|---|"]
    for title, field, unit in (("Busy, all vCPUs", "busy", "%"), ("Steal", "steal", "%"),
                               ("Load average (1 min)", "load", "")):
        values = [getattr(shard, field) for shard in shards]
        lines.append(f"| {title} | {fmean(values):.1f}{unit} | {min(values):.1f}{unit} | {max(values):.1f}{unit} |")
    if len({len(shard.per_cpu) for shard in shards}) == 1:
        per_cpu = column_means(shard.per_cpu for shard in shards)
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
        (root / "binary.tsv").write_bytes(header.encode() + b"\xff\xfe\n")
        expect("an undecodable file has no samples, not an exception", read_shard(root / "binary.tsv") is None)
        (root / "cut.tsv").write_text(header + "60\t70.0\t1.0\t90,80,60,50\t3.00\n120\t80.0\t3.0\t40,100\t3.40\n",
                                      encoding="utf-8")
        expect("a row cut short skips that row, not the shard", read_shard(root / "cut.tsv").busy == 70.0)
        report = summarize([root / name for name in ("a.tsv", "b.tsv", "empty.tsv", "missing.tsv")])
        expect("only shards with samples are counted", "on 2 of 4 shards" in report)
        expect("busy is the mean of shard means, with the range", "| Busy, all vCPUs | 67.5% | 60.0% | 75.0% |" in report)
        expect("load has no percent sign", "| Load average (1 min) | 2.9 | 2.6 | 3.2 |" in report)
        expect("per-vCPU spread is pooled across shards", "78% / 73% / 63% / 53%" in report)
        expect("no samples at all is reported, not fatal",
               "No CPU samples were retained." in summarize([root / "empty.tsv"]))
        result = subprocess.run([sys.executable, __file__, str(root / "shards/*/")],
                                capture_output=True, text=True, timeout=30, check=False)
        expect("an unmatched glob counts no shards", result.returncode == 0 and "on 0 of 0 shards" in result.stdout)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("shards", nargs="*", help="Shard directories, each holding its cpu.tsv")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    # An unmatched shell glob arrives literally; it is no shard.
    shards = [Path(shard) for shard in args.shards if Path(shard).is_dir()]
    sys.stdout.write(summarize([shard / "cpu.tsv" for shard in shards]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
