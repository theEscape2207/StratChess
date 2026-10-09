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

    python compare_lab_depth.py <run dir> [--ebf 1.88 | --measure-ebf <engine exe>]
    python compare_lab_depth.py <StrengthLabPgn dir> --history [--runs 2026-08-28 2026-10-09 ...]

The run directory is laid out as `gh run download` writes it (`*/match.pgn`). Engines are told
apart by the `candidate` prefix fastchess gives the candidate's name.

Depth delta becomes effective speed as EBF ** delta. EBF is the effective branching factor at lab
depths: nodes(d+1) / nodes(d) per iteration, 1.8-2.1 on lab openings at depths 11-16 (#776; mean
1.88). `--measure-ebf` re-measures it on the run's own openings; do so after a change that
reshapes the tree.

Read the delta against a null: an eval change with no speed effect read +0.02 plies (run
36495197163), because a different evaluation searches a different tree. Only a delta well
above that is a speed signal.

`--history` prints one row per run under a directory of runs: game length, draw rate, where the
plies go, how games end, and mean depth per engine. `to2` and `after` split a decisive game at the
first |score| >= 2.00; `pre1` counts every game's plies before |score| first reaches 1.00. Depth across runs measures the search's shape, not its speed:
pruning raises it and extensions lower it, so only the within-run delta above isolates speed.
Lengths are compared on the openings every run played, since the book offset can differ; `--runs`
(folder-name prefixes) leaves out a short or differently configured run that would shrink that set.
"""

from __future__ import annotations

import argparse
import math
import re
import statistics
import subprocess
from collections import Counter, defaultdict
from pathlib import Path
from typing import NamedTuple

# The last move's comment also carries the end reason: `{+M1/1 0.000s, White mates}`.
MOVE = re.compile(r"\{([+-]?)(M?)([\d.]+)/(\d+) ([\d.]+)s[},]")
END = re.compile(r", (?:White |Black )?([^{},]+)\}\s*(?:1-0|0-1|1/2-1/2)\s*$")
HEADER = re.compile(r'\[(\w+) "([^"]*)"\]')
BUCKET_MOVES = 10
LAST_BUCKET = 4
ELO_PER_PERCENT = 1.7  # Docs/Workflow.md -> Speed and nps: an upper bound at 10+0.1
MATE_SCORE = 99.0
DECIDED = 2.0  # |score| at which a decisive game counts as won
LEVEL = 1.0  # |score| below which a position counts as balanced
EBF_FROM_DEPTH = 11  # the lab's depths at 10+0.1


class Game(NamedTuple):
    shard: str
    headers: dict[str, str]
    depths: list[int]  # per commented ply, in play order
    scores: list[float]  # mover-relative pawns; a mate is +-MATE_SCORE
    end: str  # "wins by adjudication", "Draw by 3-fold repetition", "mates", ...


def read_games(run: Path) -> list[Game]:
    """Every game of one run, in shard order."""
    games = []
    for pgn in sorted(run.glob("*/match.pgn")):
        text = pgn.read_text(encoding="utf-8", errors="replace")
        for chunk in re.split(r"(?=\[Event )", text):
            if not chunk.strip():
                continue
            headers = dict(HEADER.findall(chunk))
            body = chunk[chunk.rfind("]") + 1 :]
            depths, scores = [], []
            for sign, mate, value, depth, _ in MOVE.findall(body):
                magnitude = MATE_SCORE if mate else float(value)
                scores.append(-magnitude if sign == "-" else magnitude)
                depths.append(int(depth))
            end = END.search(body)
            games.append(Game(pgn.parent.name, headers, depths, scores, end.group(1) if end else "?"))
    return games


def own_moves(headers: dict[str, str], depths: list[int]) -> dict[str, list[int]]:
    """Depths split by colour; a FEN with Black to move puts Black's comment first."""
    fen = headers.get("FEN", "")
    black_first = len(fen.split()) > 1 and fen.split()[1] == "b"
    first, second = ("b", "w") if black_first else ("w", "b")
    return {first: depths[0::2], second: depths[1::2]}


def depth_by_engine(games: list[Game]) -> dict[str, list[int]]:
    """Every depth each engine reached, keyed by engine name."""
    by_engine: dict[str, list[int]] = defaultdict(list)
    for game in games:
        moves = own_moves(game.headers, game.depths)
        for colour, tag in (("w", "White"), ("b", "Black")):
            by_engine[game.headers.get(tag, "")].extend(moves[colour])
    return by_engine


def compare(games: list[Game]) -> tuple[dict[int, list[int]], list[float]]:
    """Per-bucket depth deltas (candidate - reference) and one mean delta per opening pair."""
    by_opening = defaultdict(list)
    for game in games:
        by_opening[(game.shard, game.headers.get("FEN", ""))].append(game)

    buckets: dict[int, list[int]] = defaultdict(list)
    pair_means: list[float] = []
    for pair in by_opening.values():
        if len(pair) != 2:
            continue
        side = {}
        for game in pair:
            moves = own_moves(game.headers, game.depths)
            for colour, tag in (("w", "White"), ("b", "Black")):
                side[(colour, game.headers.get(tag, "").startswith("candidate"))] = moves[colour]
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


def plies(game: Game) -> int:
    return int(game.headers.get("PlyCount", len(game.depths)))


def first_reaching(scores: list[float], threshold: float) -> int | None:
    """Index of the first ply whose |score| reaches the threshold."""
    return next((i for i, score in enumerate(scores) if abs(score) >= threshold), None)


class RunSummary(NamedTuple):
    games: int
    time_control: str
    plies: float
    draw_rate: float
    to_decided: float  # decisive games: plies until |score| first reaches DECIDED
    after_decided: float  # decisive games: plies from there to the end
    level: float  # every game: plies before |score| first reaches LEVEL
    ends: Counter
    depth: dict[str, float]


def summarise(games: list[Game]) -> RunSummary:
    decisive = [g for g in games if g.headers.get("Result") in ("1-0", "0-1")]
    crossings = [(first_reaching(g.scores, DECIDED), plies(g)) for g in decisive]
    crossings = [(c, n) for c, n in crossings if c is not None]
    levels = [first_reaching(g.scores, LEVEL) for g in games]
    draws = sum(g.headers.get("Result") == "1/2-1/2" for g in games)
    return RunSummary(
        games=len(games),
        time_control=games[0].headers.get("TimeControl", "?"),
        plies=statistics.fmean(plies(g) for g in games),
        draw_rate=draws / len(games),
        to_decided=statistics.fmean(c for c, _ in crossings) if crossings else math.nan,
        after_decided=statistics.fmean(n - c for c, n in crossings) if crossings else math.nan,
        level=statistics.fmean(len(g.scores) if c is None else c for c, g in zip(levels, games)),
        ends=Counter(g.end for g in games),
        depth={name: statistics.fmean(d) for name, d in sorted(depth_by_engine(games).items()) if d},
    )


def common_opening_plies(runs: dict[str, list[Game]]) -> tuple[int, dict[str, float]]:
    """Mean game length per run over the openings every run played."""
    by_run = {}
    for name, games in runs.items():
        by_fen = defaultdict(list)
        for game in games:
            by_fen[game.headers.get("FEN", "")].append(plies(game))
        by_run[name] = by_fen
    common = set.intersection(*(set(by_fen) for by_fen in by_run.values()))
    means = {
        name: statistics.fmean(n for fen in common for n in by_fen[fen]) if common else math.nan
        for name, by_fen in by_run.items()
    }
    return len(common), means


def history(root: Path, prefixes: list[str] | None = None) -> int:
    runs = {}
    for run in sorted(p for p in root.iterdir() if p.is_dir()):
        if prefixes and not run.name.startswith(tuple(prefixes)):
            continue
        games = read_games(run)
        if games:
            runs[run.name] = games
    if not runs:
        print(f"FAIL: no */match.pgn under any directory of {root}")
        return 1

    print(f"{'run':<34} {'tc':>9} {'games':>6} {'plies':>6} {'draw':>6} {'to2':>5} {'after':>5} {'pre1':>5}  depth")
    summaries = {name: summarise(games) for name, games in runs.items()}
    for name, s in summaries.items():
        depth = "  ".join(f"{engine}: {d:.2f}" for engine, d in s.depth.items())
        print(
            f"{name:<34} {s.time_control:>9} {s.games:>6} {s.plies:>6.1f} {s.draw_rate:>6.1%}"
            f" {s.to_decided:>5.1f} {s.after_decided:>5.1f} {s.level:>5.1f}  {depth}"
        )
    for name, s in summaries.items():
        ends = ", ".join(f"{end} {n / s.games:.1%}" for end, n in s.ends.most_common(5))
        print(f"  {name}: {ends}")

    common, means = common_opening_plies(runs)
    print(f"openings every run played: {common}")
    if common:
        for name, mean in means.items():
            print(f"  {name}: {mean:.1f} plies")
    return 0


def ebf_from_nodes(nodes_per_position: list[dict[int, int]], from_depth: int) -> dict[int, list[float]]:
    """log(nodes(d) / nodes(d-1)) per position, keyed by d, for every d >= from_depth."""
    ratios: dict[int, list[float]] = defaultdict(list)
    for nodes in nodes_per_position:
        for d in sorted(nodes):
            if d >= from_depth and nodes.get(d - 1, 0) > 0:
                ratios[d].append(math.log(nodes[d] / nodes[d - 1]))
    return ratios


def measure_ebf(exe: Path, fens: list[str], depth: int) -> list[dict[int, int]]:
    """Cumulative nodes per completed iteration of `go depth` on each FEN."""
    engine = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(command: str) -> None:
        engine.stdin.write(command + "\n")
        engine.stdin.flush()

    def read_until(token: str) -> list[str]:
        lines = []
        while line := engine.stdout.readline():
            lines.append(line)
            if line.startswith(token):
                return lines
        raise SystemExit(f"FAIL: {exe.name} exited before '{token}'")

    try:
        send("uci")
        read_until("uciok")
        results = []
        for fen in fens:
            send("ucinewgame")
            send(f"position fen {fen}")
            send("isready")
            read_until("readyok")
            send(f"go depth {depth}")
            nodes = {}
            for line in read_until("bestmove"):
                d, n = re.search(r"\bdepth (\d+)", line), re.search(r"\bnodes (\d+)", line)
                if line.startswith("info") and " pv " in line and d and n:
                    nodes[int(d.group(1))] = int(n.group(1))
            results.append(nodes)
        send("quit")
        engine.wait(timeout=30)
    finally:
        if engine.poll() is None:
            engine.kill()
    return results


def opening_fens(games: list[Game], count: int) -> list[str]:
    return list(dict.fromkeys(g.headers["FEN"] for g in games if "FEN" in g.headers))[:count]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("run", type=Path, help="strength-lab run directory (*/match.pgn), or a directory of runs")
    parser.add_argument("--history", action="store_true", help="one summary row per run under RUN")
    parser.add_argument("--runs", nargs="+", metavar="PREFIX", help="--history: only run folders with these prefixes")
    parser.add_argument("--ebf", type=float, default=1.88, help="effective branching factor (default 1.88)")
    parser.add_argument("--measure-ebf", type=Path, metavar="EXE", help="measure EBF with this engine instead")
    parser.add_argument("--ebf-positions", type=int, default=40, help="openings searched by --measure-ebf")
    parser.add_argument("--ebf-depth", type=int, default=16, help="depth searched by --measure-ebf")
    args = parser.parse_args()

    if args.history:
        return history(args.run, args.runs)

    games = read_games(args.run)
    buckets, pair_means = compare(games)
    if len(pair_means) < 2:
        print(f"FAIL: {len(games)} games gave {len(pair_means)} opening pairs; need a candidate/reference run")
        return 1

    print(f"{args.run.name}: {len(games)} games, {len(pair_means)} opening pairs")
    for engine, depths in sorted(depth_by_engine(games).items()):
        q = statistics.quantiles(depths, n=10)
        print(f"  {engine}: depth mean {statistics.fmean(depths):.2f}, median {statistics.median(depths):g}, p10-p90 {q[0]:g}-{q[-1]:g}")
    for b in sorted(buckets):
        lo = b * BUCKET_MOVES
        label = f"{lo}+" if b == LAST_BUCKET else f"{lo}-{lo + BUCKET_MOVES - 1}"
        print(f"  own moves {label:>6}: n={len(buckets[b]):>8}  depth delta {statistics.fmean(buckets[b]):+.4f}")

    ebf = args.ebf
    if args.measure_ebf:
        ratios = ebf_from_nodes(measure_ebf(args.measure_ebf, opening_fens(games, args.ebf_positions), args.ebf_depth), EBF_FROM_DEPTH)
        for d in sorted(ratios):
            print(f"  EBF at depth {d:>2}: {math.exp(statistics.fmean(ratios[d])):.3f} (n={len(ratios[d])})")
        pooled = [r for rs in ratios.values() for r in rs]
        if not pooled:
            print(f"FAIL: no iterations at depth {EBF_FROM_DEPTH} or deeper; raise --ebf-depth")
            return 1
        ebf = math.exp(statistics.fmean(pooled))

    mean = statistics.fmean(pair_means)
    half = 1.96 * statistics.stdev(pair_means) / math.sqrt(len(pair_means))
    print(f"depth delta (candidate - reference): {mean:+.4f} [{mean - half:+.4f}, {mean + half:+.4f}]")
    for label, delta in (("point", mean), ("low", mean - half), ("high", mean + half)):
        speed = (ebf**delta - 1) * 100
        print(f"  EBF {ebf:.2f}, {label:>5}: effective speed {speed:+.2f}%  ~ {speed * ELO_PER_PERCENT:+.1f} Elo")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
