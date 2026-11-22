#!/usr/bin/env python3
"""Fit the UCI_Elo table from a gauntlet against Stockfish 19 (#985).

    python fit_uci_elo_calibration.py <attempt dir>...                    # the fit (D5)
    python fit_uci_elo_calibration.py <stage-1 attempt dir>... --stage1-report
    python fit_uci_elo_calibration.py <smoke attempt dir> --smoke-check
    python fit_uci_elo_calibration.py <attempt dir>... --accept-floor     # owner's floor waiver

An attempt directory is what Run-UciEloCalibration.ps1 writes for one (UCI_Elo, Stockfish UCI_Elo)
match: games.pgn, match.log and, on a clean finish only, run.json. An attempt without run.json, with
complete != true, or with smoke == true (outside --smoke-check) is refused, as is a set of attempts
that disagree on the candidate binary, Stockfish binary, book or TC. Every (level, stage, opening)
the plan names must be covered by exactly one colour-swapped pair; a missing, duplicated or
one-coloured pair is refused. The opening of a game is the attempt's openingStart + Round - 1:
fastchess numbers both games of a pair with the same Round under -repeat and order=sequential.

Levels are identified by the UCI_Elo the gauntlet set, ceil(provisional inverse(L)), and the level
played is read back through the provisional table (--table, the table the games ran under).

The fit (design D5):
  1. per level, the Elo R maximising sum s ln E + (1-s) ln(1-E), E = 1/(1+10^((SF-R)/400)), over
     all its games; Stockfish's UCI_Elo is taken as exact;
  2. 95% percentile intervals from a bootstrap over opening IDs, stratified by stage, one resample
     applied to every level at once so adjacent differences carry the shared openings;
  3. weighted pool-adjacent-violators (weights 1/bootstrap variance); only the top suffix (the
     plan's last two levels, 14 and 19) may pool;
  4. the integer table: (round(R), level played) per unpooled level; a pooled or rounded-equal top
     suffix with pooled Elo E becomes (round(E) - 1, level 14 played) and (round(E), 19.0);
  5. kMinElo = level 0's anchor, kMaxElo = level 19's.
Gates, any failure prints the report but no initializer:
  G1 each interval endpoint within 50 Elo of its point estimate;
  G2 no pooling or rounded collision outside the top suffix (S2), and no adjacent difference whose
     interval lies wholly below zero (top suffix included: a significant reversal wants a human);
  G3 the emitted table, read back at each level played, gives a label inside that level's interval;
  G4 emitted keys strictly increase, front() is level 0.0, back() the table's top level;
  S1 level 0 below Stockfish's floor (1320) ships only while it scores at least 10% there.
--accept-floor (the owner's 2026-11-22 waiver) prints S1, and G1 on a level whose stage-2 opponent was
clamped at Stockfish's floor, as NOTE instead of FAIL; G2-G4 and G1 on every other level still bind.

Depth per level comes from StratChess's own move comments ({score/depth time}, the format
compare_lab_depth.py parses), attributed by side to move while walking the movetext, never by
position in the comment list. Mate scores are counted apart (a mate stop ends below the pick depth);
a missing or unparsable comment is counted, never dropped.

Exit codes: 0 pass / report printed, 1 a gate or stop failed, 2 input refused.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_TABLE = "1320:0,2600:19"
DEFAULT_LEVELS = "0,2,4,7,10,14,19"
DEFAULT_STAGE1 = "1-30"
DEFAULT_STAGE2 = "31-150"
SF_MIN, SF_MAX = 1320, 3190  # Stockfish 19's UCI_Elo range
PRECISION = 50.0  # G1
S1_MIN_SCORE = 0.10
RESAMPLES = 10_000
SEED = 985
STRAT_PREFIX = "StratChess"
BESTMOVE_WARNING = "Bestmove does not match beginning of last PV"

HEADER = re.compile(r'\[(\w+) "([^"]*)"\]')
TOKEN = re.compile(r"\{[^}]*\}|\$\d+|\d+\.+|1-0|0-1|1/2-1/2|\*|[^\s{}]+")
COMMENT = re.compile(r"^([+-]?)(M?)([\d.]+)/(\d+) ([\d.]+)s(?:,.*)?$", re.S)
RESULTS = {"1-0", "0-1", "1/2-1/2", "*"}


class InputError(Exception):
    """An attempt set the fit refuses to read."""


# --- Provisional table -------------------------------------------------------------------------


def parse_table(text: str) -> list[tuple[float, float]]:
    table = []
    for item in text.split(","):
        elo, level = item.split(":")
        table.append((float(elo), float(level)))
    for (e0, l0), (e1, l1) in zip(table, table[1:]):
        if e1 <= e0 or l1 <= l0:
            raise InputError(f"--table must increase strictly in both columns: {text}")
    return table


def level_for_elo(table: list[tuple[float, float]], elo: float) -> float:
    """StrengthLimit.cpp's level_for_elo over the given table."""
    if elo <= table[0][0]:
        return table[0][1]
    for (e0, l0), (e1, l1) in zip(table, table[1:]):
        if elo <= e1:
            return l0 + (l1 - l0) * (elo - e0) / (e1 - e0)
    return table[-1][1]


def uci_elo_for_level(table: list[tuple[float, float]], level: float) -> int:
    """ceil(inverse(level)): the smallest UCI_Elo whose level is at least the target (design D3)."""
    for (e0, l0), (e1, l1) in zip(table, table[1:]):
        if level <= l1:
            return math.ceil(e0 + (e1 - e0) * (level - l0) / (l1 - l0) - 1e-9)
    raise InputError(f"level {level} is above the table")


def pick_depth(level: float) -> int:
    return 1 + int(level)


def round_half_up(x: float) -> int:
    return math.floor(x + 0.5)


def parse_range(text: str) -> list[int]:
    first, last = (int(v) for v in text.split("-"))
    return list(range(first, last + 1))


# --- PGN -----------------------------------------------------------------------------------------


@dataclass
class EngineMoves:
    depths: list[int] = field(default_factory=list)  # non-mate moves
    mates: int = 0
    missing: int = 0
    unparsable: int = 0


def side_moves(headers: dict[str, str], body: str) -> dict[str, list[str | None]]:
    """Each colour's non-book move comments in play order; None marks a move without one."""
    fen = headers.get("FEN", "").split()
    side = "b" if len(fen) > 1 and fen[1] == "b" else "w"
    moves: list[list] = []  # [colour, comment]
    after_move = False
    for tok in TOKEN.findall(body):
        if tok.startswith("{"):
            if after_move:
                moves[-1][1] = tok[1:-1].strip()
            after_move = False
        elif tok.startswith("$") or tok[0].isdigit() and tok.endswith(".") or tok in RESULTS:
            after_move = False
        else:
            moves.append([side, None])
            side = "b" if side == "w" else "w"
            after_move = True
    out: dict[str, list[str | None]] = {"w": [], "b": []}
    for colour, comment in moves:
        if comment == "book":
            continue
        out[colour].append(comment)
    return out


def tally(comments: list[str | None]) -> EngineMoves:
    moves = EngineMoves()
    for comment in comments:
        if comment is None:
            moves.missing += 1
            continue
        m = COMMENT.match(comment)
        if not m:
            moves.unparsable += 1
        elif m.group(2):
            moves.mates += 1
        else:
            moves.depths.append(int(m.group(4)))
    return moves


def seconds(duration: str) -> float | None:
    parts = duration.split(":")
    if len(parts) != 3:
        return None
    h, m, s = parts
    return int(h) * 3600 + int(m) * 60 + float(s)


@dataclass
class Game:
    attempt: str
    uci_elo: int
    stage: int
    sf_elo: int
    opening: int
    strat_white: bool
    score: float  # StratChess's
    seconds: float | None
    strat: EngineMoves
    sf: EngineMoves


def read_pgn(path: Path) -> list[tuple[dict[str, str], str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    games = []
    for chunk in re.split(r"(?=\[Event )", text):
        if not chunk.strip():
            continue
        headers = dict(HEADER.findall(chunk))
        body = HEADER.sub("", chunk)
        games.append((headers, body))
    return games


@dataclass
class Attempt:
    path: Path
    run: dict
    games: list[Game]


def read_attempt(path: Path, allow_smoke: bool = False) -> Attempt:
    run_json = path / "run.json"
    if not run_json.is_file():
        raise InputError(f"{path}: no run.json (a failed or unfinished attempt is never read)")
    run = json.loads(run_json.read_text(encoding="utf-8-sig"))
    if run.get("complete") is not True:
        raise InputError(f"{path}: run.json does not say complete")
    if run.get("smoke") and not allow_smoke:
        raise InputError(f"{path}: a smoke attempt is never fitted")
    start, end = int(run["openingStart"]), int(run["openingEnd"])
    games = []
    for headers, body in read_pgn(path / "games.pgn"):
        white, black = headers.get("White", ""), headers.get("Black", "")
        strat_white = white.startswith(STRAT_PREFIX)
        if strat_white == black.startswith(STRAT_PREFIX):
            raise InputError(f"{path}: cannot tell StratChess apart in {white} vs {black}")
        result = headers.get("Result", "*")
        if result not in ("1-0", "0-1", "1/2-1/2"):
            raise InputError(f"{path}: unfinished game (Result {result})")
        white_score = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}[result]
        opening = start + int(headers["Round"]) - 1
        if not start <= opening <= end:
            raise InputError(f"{path}: Round {headers['Round']} is outside openings {start}-{end}")
        comments = side_moves(headers, body)
        own, theirs = ("w", "b") if strat_white else ("b", "w")
        games.append(
            Game(
                attempt=path.name,
                uci_elo=int(run["uciElo"]),
                stage=int(run["stage"]),
                sf_elo=int(run["sfElo"]),
                opening=opening,
                strat_white=strat_white,
                score=white_score if strat_white else 1.0 - white_score,
                seconds=seconds(headers.get("GameDuration", "")),
                strat=tally(comments[own]),
                sf=tally(comments[theirs]),
            )
        )
    if len(games) != int(run["games"]):
        raise InputError(f"{path}: games.pgn holds {len(games)} games, run.json says {run['games']}")
    return Attempt(path, run, games)


def check_consistent(attempts: list[Attempt]) -> None:
    keys = {
        "candidate binary": lambda r: r["candidate"]["sha256"],
        "Stockfish binary": lambda r: r["stockfish"]["sha256"],
        "book": lambda r: r["book"]["sha256"],
        "TC": lambda r: r["tc"],
    }
    for name, key in keys.items():
        values = {key(a.run) for a in attempts}
        if len(values) > 1:
            raise InputError(f"attempts mix {len(values)} different {name}s: {sorted(values)}")


# --- Plan coverage -------------------------------------------------------------------------------


@dataclass
class Level:
    target: int  # L
    uci_elo: int
    played: float
    games: list[Game]

    @property
    def depth(self) -> int:
        return pick_depth(self.played)


def collect(
    attempts: list[Attempt], table, plan_levels: list[int], stage_openings: dict[int, list[int]]
) -> list[Level]:
    check_consistent(attempts)
    by_elo = {uci_elo_for_level(table, L): L for L in plan_levels}
    cells: dict[tuple[int, int, int], list[Game]] = defaultdict(list)
    for attempt in attempts:
        stage = int(attempt.run["stage"])
        if stage not in stage_openings:
            raise InputError(f"{attempt.path}: stage {stage} is not part of this run's plan")
        uci = int(attempt.run["uciElo"])
        if uci not in by_elo:
            raise InputError(f"{attempt.path}: UCI_Elo {uci} is not a plan level ({sorted(by_elo)})")
        for g in attempt.games:
            cells[(by_elo[uci], stage, g.opening)].append(g)
    levels = []
    for L in plan_levels:
        uci = uci_elo_for_level(table, L)
        games: list[Game] = []
        for stage, openings in stage_openings.items():
            for o in openings:
                pair = cells.pop((L, stage, o), [])
                if len(pair) != 2:
                    what = "missing" if not pair else ("incomplete" if len(pair) == 1 else "duplicate")
                    raise InputError(f"level {L} stage {stage} opening {o}: {what} pair ({len(pair)} games)")
                if pair[0].strat_white == pair[1].strat_white:
                    raise InputError(f"level {L} stage {stage} opening {o}: both games have one colour")
                if pair[0].attempt != pair[1].attempt:
                    raise InputError(f"level {L} stage {stage} opening {o}: a pair split across attempts")
                games.extend(pair)
        levels.append(Level(L, uci, level_for_elo(table, uci), games))
    if cells:
        (L, stage, o), _ = next(iter(cells.items()))
        raise InputError(f"level {L} stage {stage} opening {o} is outside the plan")
    return levels


# --- Estimation ----------------------------------------------------------------------------------


def expected(r: float, sf: float) -> float:
    return 1.0 / (1.0 + 10.0 ** ((sf - r) / 400.0))


def mle(groups: dict[int, list[float]]) -> float:
    """R maximising the fractional-Bernoulli likelihood; +-inf when every game is won or lost."""
    total = sum(s for s, _ in groups.values())
    n = sum(c for _, c in groups.values())
    if total <= 0:
        return -math.inf
    if total >= n:
        return math.inf
    lo, hi = min(groups) - 4000.0, max(groups) + 4000.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if sum(s - c * expected(mid, sf) for sf, (s, c) in groups.items()) > 0:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def group(games: list[Game]) -> dict[int, list[float]]:
    out: dict[int, list[float]] = defaultdict(lambda: [0.0, 0])
    for g in games:
        out[g.sf_elo][0] += g.score
        out[g.sf_elo][1] += 1
    return dict(out)


def bootstrap(levels: list[Level], resamples: int, seed: int) -> list[list[float]]:
    """resamples x levels estimates; openings drawn per stage, one draw shared by every level."""
    units: dict[int, list[int]] = defaultdict(list)
    for g in levels[0].games:
        if g.opening not in units[g.stage]:
            units[g.stage].append(g.opening)
    pair: list[dict[tuple[int, int], tuple[int, float]]] = []
    for level in levels:
        cell: dict[tuple[int, int], list] = {}
        for g in level.games:
            sf_elo, s = cell.get((g.stage, g.opening), (g.sf_elo, 0.0))
            cell[(g.stage, g.opening)] = (sf_elo, s + g.score)
        pair.append(cell)
    rng = random.Random(seed)
    samples = []
    for _ in range(resamples):
        draws = Counter()
        for stage, openings in units.items():
            for o in rng.choices(openings, k=len(openings)):
                draws[(stage, o)] += 1
        row = []
        for cell in pair:
            groups: dict[int, list[float]] = defaultdict(lambda: [0.0, 0])
            for key, times in draws.items():
                sf_elo, s = cell[key]
                groups[sf_elo][0] += s * times
                groups[sf_elo][1] += 2 * times
            row.append(mle(groups))
        samples.append(row)
    return samples


def percentile(sorted_values: list[float], q: float) -> float:
    pos = q * (len(sorted_values) - 1)
    lo, hi = math.floor(pos), math.ceil(pos)
    a, b = sorted_values[lo], sorted_values[hi]
    if math.isinf(a) or math.isinf(b) or lo == hi:
        return a if pos - lo < 0.5 or math.isinf(a) else b
    return a + (b - a) * (pos - lo)


def interval(values: list[float]) -> tuple[float, float]:
    clean = sorted(v if not math.isnan(v) else math.inf for v in values)
    return percentile(clean, 0.025), percentile(clean, 0.975)


def pava(values: list[float], weights: list[float]) -> list[list[int]]:
    """Weighted pool-adjacent-violators for a non-decreasing fit; returns the blocks of indices."""
    blocks: list[tuple[list[int], float, float]] = []  # indices, value, weight
    for i, (v, w) in enumerate(zip(values, weights)):
        blocks.append(([i], v, w))
        while len(blocks) > 1 and blocks[-2][1] > blocks[-1][1]:
            (i0, v0, w0), (i1, v1, w1) = blocks[-2], blocks[-1]
            blocks[-2:] = [(i0 + i1, (v0 * w0 + v1 * w1) / (w0 + w1), w0 + w1)]
    return [b[0] for b in blocks]


# --- Fit and gates -------------------------------------------------------------------------------


@dataclass
class Fit:
    points: list[float]
    intervals: list[tuple[float, float]]
    diffs: list[tuple[float, float]]
    blocks: list[list[int]]
    anchors: list[tuple[int, float]]
    plateau: bool
    failures: list[str]
    notes: list[str]


def weighted_mean(values: list[float], weights: list[float]) -> float:
    return sum(v * w for v, w in zip(values, weights)) / sum(weights)


def evaluate(
    targets: list[int],
    played: list[float],
    points: list[float],
    samples: list[list[float]],
    level0_score: float,
    top_level: float,
    clamped: list[bool] | None = None,
    accept_floor: bool = False,
) -> Fit:
    """D5 steps 3-5 and the gates, from point estimates and joint bootstrap samples."""
    n = len(targets)
    columns = [[row[k] for row in samples] for k in range(n)]
    intervals = [interval(col) for col in columns]
    diffs = [interval([row[k + 1] - row[k] for row in samples]) for k in range(n - 1)]
    weights = []
    for col in columns:
        finite = [v for v in col if math.isfinite(v)]
        var = statistics.pvariance(finite) if len(finite) == len(col) and len(finite) > 1 else math.inf
        weights.append(1.0 / var if var > 0 else 1e12)
    failures: list[str] = []
    notes: list[str] = []
    suffix = [n - 2, n - 1] if n >= 2 else []

    # G1
    for k in range(n):
        lo, hi = intervals[k]
        if not (math.isfinite(points[k]) and points[k] - lo <= PRECISION and hi - points[k] <= PRECISION):
            miss = f"G1 level {targets[k]}: interval [{lo:.0f}, {hi:.0f}] around {points[k]:.0f} is wider than +-{PRECISION:.0f}"
            if accept_floor and clamped is not None and clamped[k] and math.isfinite(points[k]):
                notes.append(f"{miss} (waived by --accept-floor: stage-2 opponent clamped at {SF_MIN})")
            else:
                failures.append(miss)

    if not all(math.isfinite(p) for p in points):
        failures.append("fit: a saturated level has no finite estimate")
        return Fit(points, intervals, diffs, [[k] for k in range(n)], [], False, failures, notes)

    # G2 / S2
    blocks = pava(points, weights)
    for b in blocks:
        if len(b) > 1 and b != suffix:
            failures.append(f"G2/S2 levels {[targets[k] for k in b]} pool outside the top suffix")
    rounded = [round_half_up(p) for p in points]
    for k in range(n - 1):
        if [k, k + 1] != suffix and rounded[k + 1] <= rounded[k] and not any(len(b) > 1 and k in b for b in blocks):
            failures.append(f"G2/S2 levels {targets[k]} and {targets[k + 1]} collide when rounded ({rounded[k]}, {rounded[k + 1]})")
        if diffs[k][1] < 0:
            failures.append(f"G2 level {targets[k + 1]} is significantly below level {targets[k]} (difference interval [{diffs[k][0]:.0f}, {diffs[k][1]:.0f}])")

    # Table
    anchors = [(rounded[k], played[k]) for k in range(n)]
    plateau = False
    if suffix and (suffix in blocks or rounded[suffix[1]] <= rounded[suffix[0]]):
        e = weighted_mean([points[k] for k in suffix], [weights[k] for k in suffix])
        anchors[suffix[0]] = (round_half_up(e) - 1, played[suffix[0]])
        anchors[suffix[1]] = (round_half_up(e), played[suffix[1]])
        plateau = True
        notes.append(
            f"top suffix pooled at {e:.1f}: UCI_Elo {round_half_up(e)} and up plays level {played[suffix[1]]:g}; "
            f"levels between {played[suffix[0]]:g} and {played[suffix[1]]:g} are reachable only through the one-Elo step"
        )

    # G3
    for k in range(n):
        lo, hi = intervals[k]
        label = anchors[k][0]
        if not lo <= label <= hi:
            failures.append(f"G3 level {targets[k]}: table label {label} is outside [{lo:.0f}, {hi:.0f}]")

    # G4
    for (e0, l0), (e1, l1) in zip(anchors, anchors[1:]):
        if e1 <= e0 or l1 <= l0:
            failures.append(f"G4 anchors not strictly increasing: ({e0}, {l0:g}) then ({e1}, {l1:g})")
    if anchors[0][1] != 0.0 or anchors[-1][1] != top_level:
        failures.append(f"G4 front/back levels are {anchors[0][1]:g}/{anchors[-1][1]:g}, need 0/{top_level:g}")

    # S1
    if points[0] < SF_MIN:
        notes.append(f"level 0 ({points[0]:.0f}) sits below Stockfish's floor {SF_MIN}: an extrapolation")
        if level0_score < S1_MIN_SCORE:
            miss = f"S1 level 0 scores {level0_score:.1%} (< {S1_MIN_SCORE:.0%})"
            if accept_floor:
                notes.append(f"{miss}: shipped as an extrapolation by --accept-floor")
            else:
                failures.append(f"{miss}: the floor is the owner's decision")

    return Fit(points, intervals, diffs, blocks, anchors, plateau, failures, notes)


# --- Reports -------------------------------------------------------------------------------------


def nearest_rank(values: list[int], q: float) -> int:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, math.ceil(q * len(ordered)) - 1))]


def depth_summary(level: Level) -> dict:
    depths, mates, missing, unparsable = [], 0, 0, 0
    for g in level.games:
        depths += g.strat.depths
        mates += g.strat.mates
        missing += g.strat.missing
        unparsable += g.strat.unparsable
    if not depths:
        return {"median": "-", "p10": "-", "p90": "-", "reached": 0.0, "mates": mates, "missing": missing, "unparsable": unparsable, "clock_bound": True}
    reached = sum(d >= level.depth for d in depths) / len(depths)
    return {
        "median": statistics.median(depths),
        "p10": nearest_rank(depths, 0.10),
        "p90": nearest_rank(depths, 0.90),
        "reached": reached,
        "mates": mates,
        "missing": missing,
        "unparsable": unparsable,
        "clock_bound": reached < 0.5,
    }


def wdl(games: list[Game]) -> str:
    w = sum(g.score == 1.0 for g in games)
    d = sum(g.score == 0.5 for g in games)
    return f"{w}-{d}-{len(games) - w - d}"


def score(games: list[Game]) -> float:
    return sum(g.score for g in games) / len(games)


def depth_cells(level: Level) -> str:
    d = depth_summary(level)
    bound = " clock-bound" if d["clock_bound"] else ""
    return f"{d['median']} ({d['p10']}-{d['p90']}) | {d['reached']:.0%}{bound} | {d['mates']} | {d['missing'] + d['unparsable']}"


def stage2_opponent(r: float) -> int:
    return min(SF_MAX, max(SF_MIN, round_half_up(r / 10) * 10))


def stage1_report(levels: list[Level], attempts: list[Attempt], stage2_games: int) -> int:
    print("| Level | UCI_Elo | Played | Pick depth | SF | Games | W-D-L | Score | Estimate | Stage-2 SF | Game s (mean) | Depth med (p10-p90) | Reached | Mates | Missing |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    stops = []
    wall_per_game = {}
    for a in attempts:
        wall_per_game[int(a.run["uciElo"])] = float(a.run["wallSeconds"]) / int(a.run["games"])
    for level in levels:
        groups = group(level.games)
        s = score(level.games)
        r = mle(groups)
        flag = ""
        if not math.isfinite(r):
            flag = " saturated"
            n = len(level.games)
            r = mle({sf: [min(max(t, 0.5), n - 0.5), c] for sf, (t, c) in groups.items()})
        sfs = "/".join(str(sf) for sf in sorted(groups))
        times = [g.seconds for g in level.games if g.seconds is not None]
        mean_s = f"{statistics.mean(times):.0f}" if times else "-"
        print(f"| {level.target} | {level.uci_elo} | {level.played:.4f} | {level.depth} | {sfs} | {len(level.games)} | {wdl(level.games)} | {s:.1%}{flag} | {r:.0f} | {stage2_opponent(r)} | {mean_s} | {depth_cells(level)} |")
        if level.target == levels[0].target and level.played == 0.0 and s < S1_MIN_SCORE:
            stops.append(f"S1 level 0 scores {s:.1%} against SF {sfs} (< {S1_MIN_SCORE:.0%}): stop, the floor is the owner's decision")
    print()
    total = 0.0
    print("Stage 2 priced from each level's stage-1 wall clock per game:")
    for level in levels:
        per_game = wall_per_game.get(level.uci_elo)
        if per_game is None:
            continue
        total += per_game * stage2_games
        print(f"  level {level.target}: {per_game:.1f} s/game x {stage2_games} = {per_game * stage2_games / 3600:.2f} h")
    print(f"  stage 2 total: {total / 3600:.2f} h ({stage2_games * len(levels)} games)")
    for stop in stops:
        print(f"STOP {stop}")
    return 1 if stops else 0


def smoke_check(attempt: Attempt, table) -> int:
    played = level_for_elo(table, int(attempt.run["uciElo"]))
    depth = pick_depth(played)
    strat, sf = EngineMoves(), EngineMoves()
    for g in attempt.games:
        for total, part in ((strat, g.strat), (sf, g.sf)):
            total.depths += part.depths
            total.mates += part.mates
            total.missing += part.missing
            total.unparsable += part.unparsable
    log = attempt.path / "match.log"
    warnings = log.read_text(encoding="utf-8", errors="replace").count(BESTMOVE_WARNING) if log.is_file() else None
    off_depth = Counter(d for d in strat.depths if d != depth)
    print(f"smoke attempt {attempt.path.name}: {len(attempt.games)} games, UCI_Elo {attempt.run['uciElo']} -> level {played:g}, pick depth {depth}")
    print(f"  StratChess moves: {len(strat.depths)} at depths {dict(Counter(strat.depths))}, {strat.mates} mate-scored, {strat.missing} without a comment, {strat.unparsable} unparsable")
    print(f"  Stockfish moves:  {len(sf.depths)}, median depth {statistics.median(sf.depths) if sf.depths else '-'} (attribution check: it must differ)")
    print(f"  fastchess '{BESTMOVE_WARNING}': {warnings if warnings is not None else 'no match.log'}")
    failures = []
    if not attempt.run.get("smoke"):
        failures.append("run.json is not a smoke attempt")
    if not strat.depths:
        failures.append("no StratChess move comment parsed")
    if strat.missing or strat.unparsable:
        failures.append(f"{strat.missing} StratChess moves lack a comment, {strat.unparsable} are unparsable")
    if off_depth:
        failures.append(f"StratChess moves off the pick depth {depth}: {dict(off_depth)}")
    for f in failures:
        print(f"FAIL {f}")
    print("PASS" if not failures else "smoke-check FAILED")
    return 1 if failures else 0


def fmt_level(level: float, top: float) -> str:
    if level == top:
        return "kMaxLevel"
    return f"{level:.10g}" if level != int(level) else f"{level:.1f}"


def fit_report(levels: list[Level], fit: Fit, top_level: float, accept_floor: bool = False) -> int:
    print("| Level | UCI_Elo set | Played | Pick depth | SF | Games | W-D-L | Score | Elo | 95% CI | Next - this, 95% CI | Anchor | Depth med (p10-p90) | Reached | Mates | Missing |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for k, level in enumerate(levels):
        lo, hi = fit.intervals[k]
        diff = f"[{fit.diffs[k][0]:+.0f}, {fit.diffs[k][1]:+.0f}]" if k < len(fit.diffs) else "-"
        anchor = fit.anchors[k][0] if fit.anchors else "-"
        sfs = "/".join(str(sf) for sf in sorted(group(level.games)))
        print(f"| {level.target} | {level.uci_elo} | {level.played:.4f} | {level.depth} | {sfs} | {len(level.games)} | {wdl(level.games)} | {score(level.games):.1%} | {fit.points[k]:.0f} | [{lo:.0f}, {hi:.0f}] | {diff} | {anchor} | {depth_cells(level)} |")
    print()
    for note in fit.notes:
        print(f"NOTE {note}")
    for failure in fit.failures:
        print(f"FAIL {failure}")
    if fit.failures:
        print("Gates failed: no initializer emitted.")
        return 1
    waived = "; S1 and floor-clamped G1 waived by --accept-floor" if accept_floor else ""
    print(f"All gates pass (G1-G4, S1{waived}).\n")
    print(f"inline constexpr int kMinElo = {fit.anchors[0][0]};")
    print(f"inline constexpr int kMaxElo = {fit.anchors[-1][0]};")
    body = []
    for k, (elo, level) in enumerate(fit.anchors):
        e = "kMinElo" if k == 0 else "kMaxElo" if k == len(fit.anchors) - 1 else str(elo)
        body.append(f"Anchor{{{e}, {fmt_level(level, top_level)}}}")
    print("constexpr std::array kAnchors{" + ", ".join(body) + "};")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("attempts", nargs="+", type=Path)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--stage1-report", action="store_true")
    mode.add_argument("--smoke-check", action="store_true")
    ap.add_argument("--accept-floor", action="store_true", help="owner waiver: S1 and G1 on floor-clamped levels print as NOTE")
    ap.add_argument("--table", default=DEFAULT_TABLE, help="provisional elo:level table the games ran under")
    ap.add_argument("--levels", default=DEFAULT_LEVELS)
    ap.add_argument("--stage1-openings", default=DEFAULT_STAGE1)
    ap.add_argument("--stage2-openings", default=DEFAULT_STAGE2)
    ap.add_argument("--resamples", type=int, default=RESAMPLES)
    ap.add_argument("--seed", type=int, default=SEED)
    args = ap.parse_args(argv)
    try:
        table = parse_table(args.table)
        if args.smoke_check:
            if len(args.attempts) != 1:
                raise InputError("--smoke-check reads exactly one attempt")
            return smoke_check(read_attempt(args.attempts[0], allow_smoke=True), table)
        attempts = [read_attempt(p) for p in args.attempts]
        plan = [int(v) for v in args.levels.split(",")]
        stages = {1: parse_range(args.stage1_openings)}
        if args.stage1_report:
            levels = collect(attempts, table, plan, stages)
            return stage1_report(levels, attempts, 2 * len(parse_range(args.stage2_openings)))
        stages[2] = parse_range(args.stage2_openings)
        levels = collect(attempts, table, plan, stages)
    except InputError as e:
        print(f"REFUSED: {e}", file=sys.stderr)
        return 2
    points = [mle(group(level.games)) for level in levels]
    samples = bootstrap(levels, args.resamples, args.seed)
    level0 = levels[0]
    clamped = [any(g.stage == 2 and g.sf_elo == SF_MIN for g in lv.games) for lv in levels]
    fit = evaluate(
        [lv.target for lv in levels], [lv.played for lv in levels], points, samples, score(level0.games), table[-1][1],
        clamped, args.accept_floor,
    )
    print(f"Fit: {len(levels)} levels, {sum(len(lv.games) for lv in levels)} games, {args.resamples} opening-pair resamples (seed {args.seed}), TC {attempts[0].run['tc']}, concurrency {attempts[0].run['concurrency']}, machine {attempts[0].run['machine']}")
    print(f"candidate {attempts[0].run['candidate']['commit']} sha256 {attempts[0].run['candidate']['sha256'][:12]}")
    print()
    return fit_report(levels, fit, table[-1][1], args.accept_floor)


if __name__ == "__main__":
    sys.exit(main())
