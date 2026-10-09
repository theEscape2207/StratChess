#!/usr/bin/env python3
"""Effective speed of a strength-lab candidate, read from the depth its games reached.

Bench nps measures a mostly cold transposition table; lab games fill it. For a change to TT or
memory latency the bench figure is therefore a floor (Docs/Workflow.md -> Speed and nps), and this
reads the speed the games actually ran at. Each move comment is the mover's `{score/depth time}`,
and both engines get the same clock, so a faster engine completes deeper iterations.

Each opening is played twice with colours swapped. For every own-move index k, the candidate's
depth is compared with the reference's at the same k while playing the same colour from the same
start position, so positions match early and drift apart later. The 95% interval clusters by
opening pair, the independent unit.

    python compare_lab_depth.py <run dir> [--ebf 1.88]

The run directory is laid out as `gh run download` writes it (`*/match.pgn`). Engines are told
apart by the `candidate` prefix fastchess gives the candidate's name.

Depth delta becomes effective speed as EBF ** delta. EBF is the effective branching factor at lab
depths: nodes(d+1) / nodes(d) per iteration, 1.8-2.1 on lab openings at depths 11-16 (#776; mean
1.88). Re-measure it after a change that reshapes the tree.

Read the delta against a null: an eval change with no speed effect read +0.02 plies (run
36495197163), because a different evaluation searches a different tree. Only a delta well
above that is a speed signal.
"""

from __future__ import annotations

import argparse
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path

MOVE = re.compile(r"\{[+-]?M?[\d.]+/(\d+) ([\d.]+)s\}")
HEADER = re.compile(r'\[(\w+) "([^"]*)"\]')
BUCKET_MOVES = 10
LAST_BUCKET = 4
ELO_PER_PERCENT = 1.7  # Docs/Workflow.md -> Speed and nps: an upper bound at 10+0.1


def read_games(run: Path) -> list[tuple[str, dict[str, str], list[int]]]:
    """Every game as (shard, headers, depth per ply in play order)."""
    games = []
    for pgn in sorted(run.glob("*/match.pgn")):
        text = pgn.read_text(encoding="utf-8", errors="replace")
        for chunk in re.split(r"(?=\[Event )", text):
            if not chunk.strip():
                continue
            headers = dict(HEADER.findall(chunk))
            body = chunk[chunk.rfind("]") + 1 :]
            depths = [int(d) for d, _ in MOVE.findall(body)]
            games.append((pgn.parent.name, headers, depths))
    return games


def own_moves(headers: dict[str, str], depths: list[int]) -> dict[str, list[int]]:
    """Depths split by colour; a FEN with Black to move puts Black's comment first."""
    fen = headers.get("FEN", "")
    black_first = len(fen.split()) > 1 and fen.split()[1] == "b"
    first, second = ("b", "w") if black_first else ("w", "b")
    return {first: depths[0::2], second: depths[1::2]}


def compare(games) -> tuple[dict[int, list[int]], list[float]]:
    """Per-bucket depth deltas (candidate - reference) and one mean delta per opening pair."""
    by_opening = defaultdict(list)
    for shard, headers, depths in games:
        by_opening[(shard, headers.get("FEN", ""))].append((headers, depths))

    buckets: dict[int, list[int]] = defaultdict(list)
    pair_means: list[float] = []
    for pair in by_opening.values():
        if len(pair) != 2:
            continue
        side = {}
        for headers, depths in pair:
            moves = own_moves(headers, depths)
            for colour, name in (("w", headers.get("White", "")), ("b", headers.get("Black", ""))):
                side[(colour, name.startswith("candidate"))] = moves[colour]
        deltas = []
        for colour in ("w", "b"):
            cand, ref = side.get((colour, True)), side.get((colour, False))
            if cand is None or ref is None:
                continue
            for k in range(min(len(cand), len(ref))):
                delta = cand[k] - ref[k]
                buckets[min(k // BUCKET_MOVES, LAST_BUCKET)].append(delta)
                deltas.append(delta)
        if deltas:
            pair_means.append(statistics.fmean(deltas))
    return buckets, pair_means


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("run", type=Path, help="strength-lab run directory (*/match.pgn)")
    parser.add_argument("--ebf", type=float, default=1.88, help="effective branching factor (default 1.88)")
    args = parser.parse_args()

    games = read_games(args.run)
    buckets, pair_means = compare(games)
    if len(pair_means) < 2:
        print(f"FAIL: {len(games)} games gave {len(pair_means)} opening pairs; need a candidate/reference run")
        return 1

    print(f"{args.run.name}: {len(games)} games, {len(pair_means)} opening pairs")
    for b in sorted(buckets):
        lo = b * BUCKET_MOVES
        label = f"{lo}+" if b == LAST_BUCKET else f"{lo}-{lo + BUCKET_MOVES - 1}"
        print(f"  own moves {label:>6}: n={len(buckets[b]):>8}  depth delta {statistics.fmean(buckets[b]):+.4f}")

    mean = statistics.fmean(pair_means)
    half = 1.96 * statistics.stdev(pair_means) / math.sqrt(len(pair_means))
    print(f"depth delta (candidate - reference): {mean:+.4f} [{mean - half:+.4f}, {mean + half:+.4f}]")
    for label, delta in (("point", mean), ("low", mean - half), ("high", mean + half)):
        speed = (args.ebf**delta - 1) * 100
        print(f"  EBF {args.ebf:.2f}, {label:>5}: effective speed {speed:+.2f}%  ~ {speed * ELO_PER_PERCENT:+.1f} Elo")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
