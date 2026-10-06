#!/usr/bin/env python3
"""Pool per-shard fastchess results into one Elo figure with a correct error bar.

Colour-swapped game pairs are correlated: the same opening is played twice with
the sides reversed, so the two games are not independent samples. Pooling raw
W/L/D counts and applying an independent-games formula therefore understates the
variance, producing an interval that is wrong in the direction of looking more
precise than the data supports.

fastchess already reports the pentanomial breakdown -- Ptnml(0-2), the count of
pairs scoring 0, 0.5, 1, 1.5 and 2 points -- so pooling is elementwise addition
of five integers per shard, and the statistics are computed once over the sum.
This reads the counts from each shard's match log rather than re-deriving them
from PGNs: the number fastchess printed is the number being pooled, which
removes a whole class of parsing disagreement.

Validate with --self-test, which checks the formula reproduces the Elo and the
interval that fastchess itself reported on real matches from this project.
With --expect-pairs-per-shard N, every log must contain exactly N pairs before
any report is printed. Omit it only for historical standalone pooling.
--summary-tsv appends each pool's headline as a row, and --render-summary turns
those rows into one table across a run's arms.
"""

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile

# Score per game for each pentanomial category: a pair is worth 2 points, so a
# pair scoring 0/0.5/1/1.5/2 corresponds to a per-game score of 0/0.25/0.5/0.75/1.
CATEGORY_SCORES = (0.0, 0.25, 0.5, 0.75, 1.0)

# fastchess reports 95% intervals.
Z_95 = 1.959963984540054

PTNML_RE = re.compile(r"Ptnml\(0-2\):[^\S\r\n]*([^\r\n]*)")
ELO_RE = re.compile(r"^\s*Elo:\s*(-?[0-9.]+)\s*\+/-\s*([0-9.]+)", re.MULTILINE)


def elo_from_score(score):
    """Logistic Elo from a per-game score in (0, 1)."""
    if score <= 0.0:
        return float("-inf")
    if score >= 1.0:
        return float("inf")
    return -400.0 * math.log10(1.0 / score - 1.0)


def pool(counts):
    """Elo and 95% half-width from pooled pentanomial counts.

    counts: five pair counts, [LL, LD, DD+LW, DW, WW].
    Returns (elo, half_width, pairs, score).
    """
    pairs = sum(counts)
    if pairs == 0:
        raise ValueError("no pairs to pool")

    score = sum(n * s for n, s in zip(counts, CATEGORY_SCORES)) / pairs
    variance = sum(n * (s - score) ** 2 for n, s in zip(counts, CATEGORY_SCORES)) / pairs
    # Standard error of the mean over PAIRS, not games -- the pair is the
    # independent unit, which is the entire point of the pentanomial model.
    stderr = math.sqrt(variance / pairs)

    elo = elo_from_score(score)
    # Transform the interval rather than the point estimate: the score-to-Elo map
    # is non-linear, so a symmetric interval in score is asymmetric in Elo.
    # fastchess reports the half-width, which is what this matches.
    upper = elo_from_score(min(score + Z_95 * stderr, 1.0 - 1e-12))
    lower = elo_from_score(max(score - Z_95 * stderr, 1e-12))
    return elo, (upper - lower) / 2.0, pairs, score


def parse_counts(text):
    """Last Ptnml line in a log, as five ints. None if absent or malformed."""
    matches = PTNML_RE.findall(text)
    if not matches:
        return None
    # fastchess may append its WL/DD ratio after the bracketed counts.
    match = re.match(r"\[\s*([0-9]+(?:\s*,\s*[0-9]+){4})\s*\]", matches[-1])
    if not match:
        return None
    return [int(n) for n in match.group(1).split(",")]


def read_shard(path, expected_pairs=None):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    counts = parse_counts(text)
    if counts is None:
        raise ValueError(
            f"{path}: no Ptnml(0-2) line found. The shard did not finish, or the "
            f"fastchess output format changed -- refusing to pool a partial batch."
        )
    if expected_pairs is not None and sum(counts) != expected_pairs:
        raise ValueError(f"{path}: expected {expected_pairs} pairs, got {sum(counts)}; "
                         "refusing to pool an incomplete or excess-count shard")
    return counts


def positive_int(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def shard_order(path):
    """Numeric shard order, so shard-2 sorts before shard-10."""
    match = re.search(r"shard-([0-9]+)", path)
    return (0, int(match.group(1)), path) if match else (1, 0, path)


def half_points(counts):
    """Candidate half-points over a shard's pairs; a 50% shard totals 2 per pair."""
    return sum(n * i for i, n in enumerate(counts))


def favours(counts):
    """Which side a shard's score favours: 'candidate', 'reference' or 'even'."""
    # Compared in exact integers, so a 50% shard is "even", never a float near it.
    if half_points(counts) > 2 * sum(counts):
        return "candidate"
    if half_points(counts) < 2 * sum(counts):
        return "reference"
    return "even"


def shard_tally(shard_counts):
    tally = {"candidate": 0, "reference": 0, "even": 0}
    for counts in shard_counts:
        tally[favours(counts)] += 1
    return tally


def interval_text(elo, err):
    lower, upper = elo - err, elo + err
    side = ("above 0" if lower > 0 else "below 0" if upper < 0 else "spans 0")
    return f"[{lower:+.2f}, {upper:+.2f}], {side}"


def summary_lines(elo, err, pairs, shard_counts):
    """The figures a ledger row and a stopping rule are read from."""
    tally = shard_tally(shard_counts)
    lines = [
        f"Interval (Elo +/- error): {interval_text(elo, err)}",
        f"Shards favouring the candidate by score: {tally['candidate']} of {len(shard_counts)} "
        f"({tally['reference']} the reference, {tally['even']} even)",
    ]
    if abs(elo) < err and elo != 0:
        # The error bar shrinks with the square root of the games played.
        needed = 2 * pairs * (err / abs(elo)) ** 2
        lines.append(f"Games for this point estimate to exclude 0: about {round(needed, -2):,.0f} "
                     f"({needed / (2 * pairs):.1f}x this batch), if the estimate holds")
    return lines


def render_summary(path):
    """Cross-arm table from --summary-tsv rows, best point estimate flagged."""
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            fields = line.rstrip("\r\n").split("\t")
            if len(fields) != 6:
                raise ValueError(f"{path}: malformed summary row {line!r}")
            label, elo, err, games, favoured, shards = fields
            rows.append((label, float(elo), float(err), int(games), int(favoured), int(shards)))
    if not rows:
        raise ValueError(f"{path}: no summary rows")
    best = max(rows, key=lambda row: row[1])
    out = ["| Arm | Games | Elo | Interval | Shards favouring |", "|---|---|---|---|---|"]
    for label, elo, err, games, favoured, shards in rows:
        mark = " (best)" if len(rows) > 1 and label == best[0] else ""
        out.append(f"| {label}{mark} | {games} | {elo:+.2f} +/- {err:.2f} "
                   f"| {interval_text(elo, err)} | {favoured} of {shards} |")
    if len(rows) > 1:
        out.append("")
        out.append(f"The best arm is the best of {len(rows)}, so its estimate is biased upward. "
                   "Confirm it on fresh openings before changing a default.")
    return "\n".join(out)


# Real (counts, elo, half_width) triples printed by the pinned fastchess build on
# this project's own matches. Sources: Docs/EloLog.md and local logs/elo runs.
# They span 6 to 3500 games so the check is sensitive to precision, not just to
# gross error.
SELF_TEST_CASES = [
    ([173, 248, 646, 368, 315], 40.28, 9.81),      # 3500 games, clang-cl vs MSVC
    ([38, 46, 112, 59, 56], 27.43, 24.00),          # 622 games, same comparison
    ([3, 4, 3, 3, 2], -34.86, 122.83),              # 30 games, 2+0.02 self-play
    ([1, 3, 5, 4, 2], 34.86, 101.20),               # 30 games, 5+0.05 self-play
    ([2, 1, 2, 3, 2], 34.86, 163.76),               # 20 games
    ([1, 3, 4, 1, 1], -34.86, 122.07),              # 20 games
    ([0, 0, 5, 1, 0], 29.02, 52.57),                # 12 games, self-play
]


def self_test():
    ok = True
    print(f"{'counts':<28} {'elo':>9} {'expected':>9} {'+/-':>9} {'expected':>9}  ")
    for counts, want_elo, want_err in SELF_TEST_CASES:
        elo, err, _, _ = pool(counts)
        # fastchess prints 2 decimals, so agreement to 0.02 is exact agreement
        # up to its own rounding.
        good = abs(elo - want_elo) <= 0.02 and abs(err - want_err) <= 0.02
        ok &= good
        print(
            f"{str(counts):<28} {elo:>9.2f} {want_elo:>9.2f} {err:>9.2f} {want_err:>9.2f}"
            f"  {'ok' if good else 'MISMATCH'}"
        )
    with tempfile.TemporaryDirectory() as root:
        logs = [os.path.join(root, f"shard-{i}.log") for i in range(2)]

        def run_case(name, counts, flags, success, payload=None, missing=False):
            nonlocal ok
            for path, row in zip(logs, counts):
                with open(path, "w", encoding="utf-8") as handle:
                    handle.write(payload if payload is not None
                                 else f"Ptnml(0-2): [{', '.join(map(str, row))}]\n")
            if missing:
                os.remove(logs[-1])
            result = subprocess.run([sys.executable, os.path.abspath(__file__), *flags, *logs],
                                    capture_output=True, text=True, check=False)
            good = ((result.returncode == 0) == success
                    and ("**Pooled:" in result.stdout) == success
                    and (success or not result.stdout)
                    and "Traceback" not in result.stderr)
            ok &= good
            print(f"{'ok' if good else 'FAIL'} {name}")

        flags = ["--expect-shards", "2", "--expect-pairs-per-shard", "15"]
        run_case("CLI complete shards preserve pooled formula", [[3, 4, 3, 3, 2]] * 2,
                 flags, True)
        run_case("CLI parseable short shard refused", [[3, 4, 3, 3, 1], [3, 4, 3, 3, 2]],
                 flags, False)
        run_case("CLI excess pairs refused", [[3, 4, 3, 3, 3], [3, 4, 3, 3, 2]],
                 flags, False)
        run_case("CLI compensating counts refused before any report",
                 [[3, 4, 3, 3, 1], [3, 4, 3, 3, 3]], flags, False)
        run_case("CLI standalone historical counts remain optional",
                 [[3, 4, 3, 3, 1], [3, 4, 3, 3, 3]], ["--expect-shards", "2"], True)
        run_case("CLI malformed final counts fail cleanly", [[0, 0, 15, 0, 0]] * 2,
                 flags, False, payload="Ptnml(0-2): [0, 0, 15, 0, 0]\nPtnml(0-2): [0, , 15, 0, 0]\n")
        run_case("CLI missing counts fail cleanly", [[0, 0, 15, 0, 0]] * 2,
                 flags, False, payload="interrupted match\n")
        run_case("CLI missing log fails cleanly before output", [[0, 0, 15, 0, 0]] * 2,
                 flags, False, missing=True)
        for count in ("0", "-1"):
            run_case(f"CLI nonpositive pair count {count} refused", [[0, 0, 15, 0, 0]] * 2,
                     ["--expect-pairs-per-shard", count], False)
        for invalid in ("Ptnml(0-2): [1, 2, 3, 4]\n",
                        "Ptnml(0-2): [1, 2, , 4, 5]\n",
                        "Ptnml(0-2): [1, 2, 3, 4, 5\n"):
            good = parse_counts("Ptnml(0-2): [1, 2, 3, 4, 5]\n" + invalid) is None
            ok &= good
            print(f"{'ok' if good else 'FAIL'} malformed final counts refused")
        good = parse_counts("Ptnml(0-2): [53, 151, 289, 179, 68], WL/DD Ratio: 1.58\n") == [
            53, 151, 289, 179, 68]
        ok &= good
        print(f"{'ok' if good else 'FAIL'} appended fastchess ratio accepted")

        good = ([favours(c) for c in ([0, 0, 2, 0, 0], [0, 0, 1, 1, 0], [0, 1, 1, 0, 0])]
                == ["even", "candidate", "reference"])
        ok &= good
        print(f"{'ok' if good else 'FAIL'} shard favour by score, ties even")
        good = (interval_text(2.98, 3.23) == "[-0.25, +6.21], spans 0"
                and interval_text(6.99, 6.16).endswith("above 0")
                and interval_text(-9.29, 5.04).endswith("below 0"))
        ok &= good
        print(f"{'ok' if good else 'FAIL'} interval sides")
        lines = summary_lines(2.98, 3.23, 12348, [[0, 0, 1, 1, 0]] * 13 + [[0, 1, 1, 0, 0]] * 5)
        good = ("13 of 18 (5 the reference, 0 even)" in lines[1]
                and "about 29,000 (1.2x this batch)" in lines[2])
        ok &= good
        print(f"{'ok' if good else 'FAIL'} summary lines: shard tally and games to exclude 0")

        tsv = os.path.join(root, "arms.tsv")
        for arm, row in (("A", [0, 0, 2, 0, 0]), ("B", [0, 0, 1, 1, 0])):
            with open(logs[0], "w", encoding="utf-8") as handle:
                handle.write(f"Ptnml(0-2): {row}\n")
            subprocess.run([sys.executable, os.path.abspath(__file__), "--label", arm,
                            "--summary-tsv", tsv, logs[0]], capture_output=True, check=True)
        table = render_summary(tsv)
        good = "| B (best) | 4 |" in table and "best of 2" in table and "| A |" in table
        ok &= good
        print(f"{'ok' if good else 'FAIL'} cross-arm summary flags the best arm")
    print("\nself-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", help="per-shard fastchess match logs")
    parser.add_argument("--self-test", action="store_true",
                        help="check the formula against known fastchess output")
    parser.add_argument("--expect-shards", type=int, default=0,
                        help="refuse to pool unless exactly this many logs are given")
    parser.add_argument("--expect-pairs-per-shard", type=positive_int,
                        help="require this many pairs in every log before printing any report")
    parser.add_argument("--label", help="arm name written to --summary-tsv")
    parser.add_argument("--summary-tsv",
                        help="append this pool's headline as one tab-separated row")
    parser.add_argument("--render-summary", metavar="TSV",
                        help="print a cross-arm table from rows written by --summary-tsv")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if args.render_summary:
        try:
            print(render_summary(args.render_summary))
        except (OSError, ValueError) as error:
            print(f"::error::{error}", file=sys.stderr)
            return 1
        return 0

    if not args.logs:
        parser.error("no shard logs given")

    # A biased subset wearing a full batch's error bar is worse than no result:
    # if three shards of twenty died, the surviving seventeen are not a random
    # sample of the batch. Refuse rather than report.
    if args.expect_shards and len(args.logs) != args.expect_shards:
        raise SystemExit(
            f"expected {args.expect_shards} shard logs, got {len(args.logs)}. "
            f"Refusing to pool a partial batch."
        )

    try:
        shard_counts = [read_shard(path, args.expect_pairs_per_shard) for path in args.logs]
        total = [sum(column) for column in zip(*shard_counts)]
        elo, err, pairs, score = pool(total)
    except (OSError, ValueError) as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1

    print("| Shard | Pairs | Ptnml(0-2) | Score | Favours |")
    print("|---|---|---|---|---|")
    for path, counts in sorted(zip(args.logs, shard_counts), key=lambda row: shard_order(row[0])):
        shard_pairs = sum(counts)
        print(f"| `{path}` | {shard_pairs} | {counts} | {25 * half_points(counts) / shard_pairs:.2f}% "
              f"| {favours(counts)} |")

    print()
    print(f"**Pooled: {elo:+.2f} +/- {err:.2f} Elo** "
          f"({pairs} pairs = {2 * pairs} games, score {100 * score:.2f}%)")
    print()
    print(f"Pooled Ptnml(0-2): {total}")
    print()
    for line in summary_lines(elo, err, pairs, shard_counts):
        print(f"- {line}")

    if args.summary_tsv:
        tally = shard_tally(shard_counts)
        with open(args.summary_tsv, "a", encoding="utf-8") as handle:
            handle.write(f"{args.label or ''}\t{elo:.2f}\t{err:.2f}\t{2 * pairs}\t"
                         f"{tally['candidate']}\t{len(shard_counts)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
