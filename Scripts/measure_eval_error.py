#!/usr/bin/env python3
"""Build a dataset of positions joined to the engine's evaluation and an external oracle.

This is a dataset generator, not an analysis. It samples positions from lab PGNs on a
population selected by nothing but a quiet filter, and joins each one to three judgements:
the engine's per-term `eval` breakdown, the engine's own search score, and an external
oracle's score. The output is a JSONL row per position; the questions are asked afterwards,
ad hoc, against that file.

It is deliberately not a report. The first version of this module carried a statistics and
verdict layer -- calibration slopes, per-term correlations, predeclared branches -- and
every finding that survived review came instead from ad-hoc queries over the rows, while
the built-in report produced a headline that had to be withdrawn. Per-term correlation in
particular cannot detect a MISSING term: a term that is absent has zero variance and is
cleared by construction. Keep the join here and the reasoning outside it.

    python measure_eval_error.py --self-test
    python measure_eval_error.py <pgn-root> --engine-exe <exe> --limit 3000 \
        --rows-jsonl rows.jsonl

Three legs, in cost order. The oracle leg dominates; the static leg is free by comparison.

  1. sample   uniform over every ply in the corpus, stratified to equal counts per phase,
              deduplicated by FEN. No contested filter and no loss conditioning.
  2. quiet    keep a position only when the side to move is not in check and the oracle's
              depth-20 principal variation opens with a quiet move. One search serves both
              the filter and the join.
  3. floor    the engine's own search at --floor-depth on the surviving rows. Static
              evaluation disagreeing with the ORACLE spans two links -- evaluation against
              the engine's own verdict, and that search against a stronger one. Only the
              first is a statement about the evaluation, so the second is recorded rather
              than left folded into it.

POINT OF VIEW. Everything here is white-POV. The engine's `eval` prints two totals that
differ: the net column and `white pov:` are white-POV, `static eval:` is side-to-move POV.
Joining `static eval:` to a white-POV oracle inverts every black-to-move row silently and
turns noise into a finding, so this module never reads that line. `parse_breakdown`
asserts the net column, the printed sum and `white pov:` all agree on every row, which is
also what makes a drift in the table's format fail on the first position rather than in
the output.

Two rows of the table need their own rule. `material` is king-inclusive (10,000 cp a side)
and cancels in the net column; `endgame` is a net-only adjustment with no per-side split.
Everything downstream reads the net column, so both are handled by that choice.

POPULATION. Positions come from the engine's own lab games, so the evaluation under test
chose the moves that produced them, and lab results are lopsided enough that a signed mean
over the whole set mostly restates that skew. Cut by position class, not by grand mean.

The oracle is not in the checkout -- see analyze_external_quality.py for where it lives and
how --engine/STOCKFISH_PATH override the search.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_external_quality as aeq  # noqa: E402  (path must be set first)
import analyze_move_quality as amq  # noqa: E402

try:
    import chess
except ImportError as exc:  # pragma: no cover - environment guard, not test logic
    sys.exit(
        "measure_eval_error.py requires the 'python-chess' package.\n"
        "Install it with: pip install python-chess\n"
        f"(import failed: {exc})"
    )

# The oracle depth the quiet filter and the join both read. 20 rather than 12 because the
# filter needs a principal variation it can trust to be the position's actual best line.
ORACLE_DEPTH = 20

# The engine's own search depth for the noise-floor leg. Deep enough to see the tactics a
# static evaluation cannot, cheap enough to run on every surviving row.
FLOOR_DEPTH = 12

# A mate score is not on the centipawn scale. Tier 1 and Tier 2 clamp rather than drop,
# because the move that walked into the mate is the row worth counting; here the row is a
# *position* and a static evaluation has no mate to report, so a clamped +/-1000 against a
# static score would enter the mean as evaluation error when it is tactics. Counted and
# reported separately instead.
CLAMP_CP = aeq.CLAMP_CP

# The breakdown rows in the order cmd_eval prints them. `endgame` is last and is net-only.
TERMS = ("material", "pawns", "rooks", "pst", "mopup", "bishops", "castling", "mobility",
         "outposts", "shelter", "storm", "kingfiles", "kingattack", "endgame")

_ROW_RE = re.compile(r"^(\w+)\s*\|\s*(-?\d+|-)\s*\|\s*(-?\d+|-)\s*\|\s*(-?\d+)\s*$")
_SUM_RE = re.compile(r"^sum \(white pov\)\s*(-?\d+)$")
_WHITE_POV_RE = re.compile(r"^white pov: (-?\d+) cp$")
_PHASE_RE = re.compile(r"^phase: (\d+)/(\d+)$")
_SCALE_RE = re.compile(r"^endgame scale: (\d+)/(\d+)$")

_ENGINE_EXE = ""


class EvalProtocolError(RuntimeError):
    """The engine's `eval` output did not parse, or its three totals disagreed."""


@dataclass(frozen=True)
class Position:
    """One sampled position, before any oracle or engine call has touched it."""

    fen: str
    phase: str
    game_key: str
    ply_index: int
    ply_since_book_exit: int | None
    from_book: bool


@dataclass
class Row:
    """One position that survived the quiet filter, with both judgements joined."""

    fen: str
    phase: str
    game_key: str
    band: str
    material: tuple                     # amq.material_classes names, or ()
    white_to_move: bool
    static_cp: int                      # white pov
    oracle_cp: int                      # white pov, clamped
    oracle_is_mate: bool
    terms: dict                         # term -> net contribution, white pov
    eval_phase: int
    endgame_scale: int
    floor_cp: int | None = None         # the engine's own search, white pov

    @property
    def error(self) -> int:
        """Signed error: positive means the engine likes White more than the oracle does."""
        return self.static_cp - self.oracle_cp

    @property
    def stm_error(self) -> int:
        """The same error from the side to move's point of view.

        The evaluator is colour-symmetric by construction -- engine_check asserts the
        mirror of a position scores its exact negation -- so a white-pov bias cannot come
        from the evaluation preferring White. It can only come from the sample's
        side-to-move balance meeting something the ORACLE scores asymmetrically, and the
        first candidate is the tempo the oracle credits to the side to move and the static
        evaluation has no term for. In side-to-move pov that cancellation is undone: a
        missing tempo term reads as a consistent negative here and as roughly nothing in
        the white-pov mean.
        """
        return self.error if self.white_to_move else -self.error

    @property
    def floor_error(self) -> int | None:
        """Static minus the engine's OWN search: what the quiet filter failed to remove."""
        return None if self.floor_cp is None else self.static_cp - self.floor_cp


# --------------------------------------------------------------------------------------
# Stage 1 -- sampling
# --------------------------------------------------------------------------------------


def _book_exit_of(plies, headers):
    """Reuse Tier 2's book-exit rule rather than restating its prefix logic."""
    return aeq._book_exit(plies, headers)


@dataclass(frozen=True)
class _BookPly:
    """The one field aeq._book_exit reads, so its rule can be reused without a full scan."""

    is_book: bool


def scan_positions(path: Path):
    """Yield a Position for every ply of every finished game in one PGN.

    Deliberately NOT aeq.scan_games: that applies Tier 1's contested filter and drops any
    game with no eligible row, which is exactly the conditioning this measurement exists to
    avoid. Only games that fail to parse are skipped, and those are reported.

    A generator because the full corpus is ~1.6M plies; materialising them all costs about
    a gigabyte for a sample of a few thousand.
    """
    shard = path.parent.name
    for game_index, (headers, movetext) in enumerate(amq.parse_games(path)):
        where = f"{shard} round {headers.get('Round', '?')}"
        moves, saw_result = amq.split_movetext(movetext, where)
        if not saw_result or headers.get("Result", "*") == "*":
            continue
        fen = headers.get("FEN")
        board = chess.Board(fen) if fen else chess.Board()
        game_key = f"{shard}:{game_index}"
        book_flags = [_BookPly(is_book=(comment == "book")) for _san, comment in moves]
        book_exit_ply, _basis = _book_exit_of(tuple(book_flags), headers)
        rows: list[Position] = []
        try:
            for idx, (san, _comment) in enumerate(moves):
                since = None if book_exit_ply is None else max(0, idx - book_exit_ply)
                rows.append(Position(
                    fen=board.fen(),
                    phase=amq.phase_bucket(amq.board_phase(board)),
                    game_key=game_key,
                    ply_index=idx,
                    ply_since_book_exit=since,
                    from_book=book_exit_ply is not None and idx < book_exit_ply,
                ))
                board.push(board.parse_san(san))
        except ValueError as exc:               # truncated or corrupt game
            print(f"warning: {where}: {exc}", file=sys.stderr)
            continue
        yield from rows


def stratified_sample(positions, limit: int, seed: int):
    """-> (sampled rows, stats) -- equal counts per phase, deduplicated by FEN.

    Deduplication is what stops the shared opening book from supplying most of the opening
    stratum many times over; the first game to reach a FEN keeps it, so every row still has
    a game to cluster on. Phases are filled to `limit // 3` each, and a phase with fewer
    unique positions than that contributes all of them rather than being resampled.

    Reservoir sampling per phase, over a single pass: the corpus does not fit comfortably
    in memory, and holding it only to draw a few thousand rows from it is the one part of
    this that would have to be rewritten to run the full 18 shards. The draw is uniform
    over each phase's unique positions, which is what stratification needs, and the seed
    makes it reproducible.
    """
    rng = random.Random(seed)
    per_phase = max(1, limit // len(amq.PHASE_BUCKETS))
    reservoir: dict[str, list[Position]] = defaultdict(list)
    counted: dict[str, int] = defaultdict(int)
    seen: set[str] = set()
    plies = duplicates = 0

    for pos in positions:
        plies += 1
        if pos.fen in seen:
            duplicates += 1
            continue
        seen.add(pos.fen)
        pool = reservoir[pos.phase]
        index = counted[pos.phase]
        counted[pos.phase] += 1
        if len(pool) < per_phase:
            pool.append(pos)
        else:
            j = rng.randrange(index + 1)
            if j < per_phase:
                pool[j] = pos

    sampled: list[Position] = []
    short = {}
    for name, _low in amq.PHASE_BUCKETS:
        pool = reservoir.get(name, [])
        if counted[name] <= per_phase:
            short[name] = counted[name]
        sampled.extend(pool)
    sampled.sort(key=lambda p: (p.phase, p.game_key, p.ply_index))
    stats = {
        "plies_seen": plies,
        "unique_fens": len(seen),
        "duplicates_dropped": duplicates,
        "per_phase_target": per_phase,
        "phase_pool": {name: counted[name] for name, _low in amq.PHASE_BUCKETS},
        "phase_short": short,
        "sampled": len(sampled),
        "from_book": sum(1 for p in sampled if p.from_book),
    }
    return sampled, stats


# --------------------------------------------------------------------------------------
# Stage 2 -- the engine's static evaluation
# --------------------------------------------------------------------------------------


def _is_quiet_move(board: chess.Board, move: chess.Move) -> bool:
    """A move a quiescence search would not extend on: no capture, promotion or check."""
    if board.is_capture(move) or move.promotion is not None:
        return False
    return not board.gives_check(move)


def parse_breakdown(text: str):
    """-> (white-pov total, {term: net}, phase, endgame_scale) from one `eval` reply.

    Raises EvalProtocolError when the table does not parse, or when the net column, the
    printed sum and `white pov:` disagree. That check is the format tripwire: cmd_eval
    prints all three precisely so a drift between the terms and Evaluate() is visible, and
    a parser that silently read the wrong column would otherwise produce a clean-looking
    report built on inverted or partial rows.
    """
    nets: dict[str, int] = {}
    printed_sum = white_pov = phase = scale = None
    for line in text.splitlines():
        line = line.strip()
        row = _ROW_RE.match(line)
        if row:
            name, _white, _black, net = row.groups()
            if name in TERMS:
                nets[name] = int(net)
            continue
        for pattern, setter in ((_SUM_RE, "sum"), (_WHITE_POV_RE, "pov"),
                                (_PHASE_RE, "phase"), (_SCALE_RE, "scale")):
            hit = pattern.match(line)
            if not hit:
                continue
            if setter == "sum":
                printed_sum = int(hit.group(1))
            elif setter == "pov":
                white_pov = int(hit.group(1))
            elif setter == "phase":
                phase = int(hit.group(1))
            else:
                scale = int(hit.group(1))
            break

    missing = [t for t in TERMS if t not in nets]
    if missing:
        raise EvalProtocolError(f"eval table is missing term rows: {', '.join(missing)}")
    if printed_sum is None or white_pov is None or phase is None or scale is None:
        raise EvalProtocolError("eval reply is missing sum, white pov, phase or endgame scale")
    net_sum = sum(nets.values())
    if not (net_sum == printed_sum == white_pov):
        raise EvalProtocolError(
            f"eval totals disagree: net column {net_sum}, printed sum {printed_sum}, "
            f"white pov {white_pov}")
    return white_pov, nets, phase, scale


class StaticEval:
    """A persistent engine process answering `eval` and `go depth N`, both white-POV."""

    def __init__(self, exe: str):
        # Resolved and checked here rather than left to Popen: a missing binary otherwise
        # surfaces as a bare WinError 2 from deep inside subprocess, and a relative path
        # that resolves against a worker's working directory rather than this one is how a
        # run ends up measuring a stale build (CLAUDE.md, Subagent Dispatch).
        resolved = Path(exe).resolve()
        if not resolved.is_file():
            raise SystemExit(f"engine binary not found: {exe} (resolved to {resolved})")
        exe = str(resolved)
        self.exe = exe
        self.proc = subprocess.Popen(
            [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
            encoding="utf-8", errors="replace",
        )
        self._send("uci")
        self._read_until(lambda line: line == "uciok")
        self._send("isready")
        self._read_until(lambda line: line == "readyok")

    def _send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _read_until(self, done) -> list[str]:
        """Read until `done` matches. A piped engine answers asynchronously; reading a
        fixed number of lines instead would desynchronise the stream on the first reply
        that grew a line, and every later position would be parsed from the wrong text."""
        assert self.proc.stdout is not None
        lines: list[str] = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise EvalProtocolError(f"{self.exe} closed its output unexpectedly")
            line = line.strip()
            lines.append(line)
            if done(line):
                return lines

    def breakdown(self, fen: str):
        self._send(f"position fen {fen}")
        self._send("eval")
        text = "\n".join(self._read_until(lambda line: line.startswith("white pov:")))
        return parse_breakdown(text)

    def search_white_pov(self, fen: str, depth: int) -> int | None:
        """The engine's own search score in white-POV cp, or None for a mate score."""
        self._send("ucinewgame")
        self._send("isready")
        self._read_until(lambda line: line == "readyok")
        self._send(f"position fen {fen}")
        self._send(f"go depth {depth}")
        lines = self._read_until(lambda line: line.startswith("bestmove"))
        score = None
        for line in lines:
            if not line.startswith("info ") or " score " not in line:
                continue
            if " score mate " in line:
                score = "mate"
                continue
            hit = re.search(r" score cp (-?\d+)", line)
            if hit:
                score = int(hit.group(1))
        if score is None or score == "mate":
            return None
        white_to_move = chess.Board(fen).turn == chess.WHITE
        return score if white_to_move else -score

    def close(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=5)
        except Exception:                       # pragma: no cover - shutdown race
            self.proc.kill()


# --------------------------------------------------------------------------------------
# Stage 2/3 workers
# --------------------------------------------------------------------------------------


def _init_oracle(engine_path: str, depth: int) -> None:
    aeq._init_worker(engine_path, depth)


def oracle_batch(fens):
    """-> [(fen, quiet, oracle_cp, is_mate, best_uci or ''), ...] for one batch.

    The engine is closed before the task returns, not left to the worker's atexit. A
    worker still holding a live UCI subprocess does not exit when the pool shuts down --
    measured at 90+ seconds against 0.4 with an explicit close -- and on Windows killing
    the worker would orphan the engine rather than collect it. Re-opening per batch costs
    one process start per ~25 positions, and gives every search a cold table, which is the
    condition #483's 1.24 s figure was measured under.
    """
    out = []
    for fen in fens:
        board = chess.Board(fen)
        if board.is_game_over(claim_draw=False):
            out.append((fen, False, 0, False, ""))
            continue
        endpoint, best = aeq.oracle_endpoint(board, chess.WHITE)
        quiet = (not board.is_check()) and best is not None and _is_quiet_move(board, best)
        is_mate = abs(endpoint.legacy_cp) >= CLAMP_CP
        out.append((fen, quiet, int(endpoint.legacy_cp), is_mate, best.uci() if best else ""))
    aeq._close_engine()
    return out


def _init_static(engine_exe: str) -> None:
    global _ENGINE_EXE
    _ENGINE_EXE = engine_exe


def floor_batch(args):
    """-> [(fen, engine's own white-pov search score or None), ...] for one batch.

    Opened and closed inside the task for the reason oracle_batch states: a worker holding
    a live UCI subprocess does not exit when the pool shuts down.
    """
    fens, depth = args
    static = StaticEval(_ENGINE_EXE)
    try:
        return [(fen, static.search_white_pov(fen, depth)) for fen in fens]
    finally:
        static.close()


# --------------------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------------------


def batches(items, size):
    for i in range(0, len(items), size):
        yield items[i:i + size]


def run(root: Path, engine_exe: str, oracle_path: str, limit: int, jobs: int, seed: int,
        depth: int, floor_depth: int, skip_floor: bool, batch: int):
    """-> (rows, stats). Sample, filter, join, and hand back the rows."""
    shards = sorted(root.rglob("match.pgn"))
    if not shards:
        shards = sorted(root.rglob("*.pgn"))
    if not shards:
        raise SystemExit(f"no PGN files under {root}")
    print(f"parsing {len(shards)} PGN file(s)...", file=sys.stderr)

    def every_position():
        for shard in shards:
            yield from scan_positions(shard)

    sampled, sample_stats = stratified_sample(every_position(), limit, seed)
    print(f"sampled {len(sampled)} of {sample_stats['unique_fens']} unique positions",
          file=sys.stderr)

    by_fen = {p.fen: p for p in sampled}
    fens = [p.fen for p in sampled]

    started = time.monotonic()
    results = []
    with ProcessPoolExecutor(max_workers=jobs, initializer=_init_oracle,
                             initargs=(oracle_path, depth)) as pool:
        for chunk in pool.map(oracle_batch, list(batches(fens, batch))):
            results.extend(chunk)
            rate = len(results) / max(1e-9, time.monotonic() - started)
            print(f"  oracle {len(results)}/{len(fens)}  ({rate:.1f}/s)", file=sys.stderr)

    quiet_stats = {"scanned": len(results), "quiet": 0, "in_check": 0,
                   "tactical_pv": 0, "terminal": 0}
    survivors = []
    for fen, quiet, cp, is_mate, best in results:
        board = chess.Board(fen)
        if board.is_game_over(claim_draw=False):
            quiet_stats["terminal"] += 1
            continue
        if board.is_check():
            quiet_stats["in_check"] += 1
            continue
        if not quiet:
            quiet_stats["tactical_pv"] += 1
            continue
        quiet_stats["quiet"] += 1
        survivors.append((fen, cp, is_mate, best))

    print(f"  {quiet_stats['quiet']} quiet rows; reading static eval...", file=sys.stderr)
    static = StaticEval(engine_exe)
    rows: list[Row] = []
    try:
        for fen, oracle_cp, is_mate, _best in survivors:
            white_pov, nets, eval_phase, scale = static.breakdown(fen)
            pos = by_fen[fen]
            board = chess.Board(fen)
            rows.append(Row(
                fen=fen, phase=pos.phase, game_key=pos.game_key,
                band=aeq.band_of(pos.ply_since_book_exit),
                material=tuple(name for name, _c in amq.material_classes(board)),
                white_to_move=board.turn == chess.WHITE,
                static_cp=white_pov, oracle_cp=oracle_cp, oracle_is_mate=is_mate,
                terms=nets, eval_phase=eval_phase, endgame_scale=scale,
            ))
    finally:
        static.close()

    if not skip_floor and rows:
        print(f"  engine's own search at depth {floor_depth}...", file=sys.stderr)
        floor_fens = [r.fen for r in rows]
        floor_started = time.monotonic()
        scores: dict[str, int | None] = {}
        with ProcessPoolExecutor(max_workers=jobs, initializer=_init_static,
                                 initargs=(engine_exe,)) as pool:
            work = [(chunk, floor_depth) for chunk in batches(floor_fens, batch)]
            for chunk in pool.map(floor_batch, work):
                scores.update(dict(chunk))
                rate = len(scores) / max(1e-9, time.monotonic() - floor_started)
                print(f"  search {len(scores)}/{len(floor_fens)}  ({rate:.1f}/s)",
                      file=sys.stderr)
        for row in rows:
            row.floor_cp = scores.get(row.fen)

    stats = {
        "sampling": sample_stats,
        "quiet": quiet_stats,
        "rows": len(rows),
        "rows_mate_scored": sum(1 for r in rows if r.oracle_is_mate),
        "meta": {
            "oracle_depth": depth,
            "floor_depth": None if skip_floor else floor_depth,
            "jobs": jobs, "seed": seed, "limit": limit,
            "engine": str(Path(engine_exe).resolve()),
            "oracle": str(Path(oracle_path).resolve()),
            "inputs": [str(s) for s in shards],
            "elapsed_s": round(time.monotonic() - started, 1),
        },
    }
    return rows, stats


def report_counts(stats, out=sys.stdout) -> None:
    """What the run produced, so a shrunken sample is visible before anyone queries it."""
    w = out.write
    sample, quiet = stats["sampling"], stats["quiet"]
    w("Sampling (unselected: no contested filter, no loss conditioning)\n")
    w(f"  plies seen           : {sample['plies_seen']}\n")
    w(f"  unique FENs          : {sample['unique_fens']}"
      f" ({sample['duplicates_dropped']} duplicates dropped)\n")
    w(f"  sampled              : {sample['sampled']}"
      f" (target {sample['per_phase_target']}/phase, {sample['from_book']} from book)\n")
    if sample["phase_short"]:
        w(f"  phases short of target: {sample['phase_short']}\n")
    w(f"  quiet survivors      : {quiet['quiet']} of {quiet['scanned']}"
      f" ({100.0 * quiet['quiet'] / max(1, quiet['scanned']):.1f}%)\n")
    w(f"    dropped in check   : {quiet['in_check']}\n")
    w(f"    dropped tactical PV: {quiet['tactical_pv']}\n")
    w(f"    dropped terminal   : {quiet['terminal']}\n")
    w(f"  rows written         : {stats['rows']}"
      f" ({stats['rows_mate_scored']} carry a mate-clamped oracle score)\n")
    w("\nA mate-clamped oracle score is not on the centipawn scale; exclude those rows\n"
      "from any centipawn cut rather than treating the clamp as an evaluation error.\n")


def write_rows(rows, path: Path) -> None:
    """Every surviving row, so a later question costs a re-read rather than a re-run.

    The oracle leg is ~20 minutes; the analyses it feeds are seconds. Persisting the join
    is what keeps a follow-up cut -- by term, by band, by anything not thought of yet --
    from re-booking the expensive half.
    """
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps({
                "fen": row.fen, "phase": row.phase, "band": row.band,
                "game_key": row.game_key, "material": list(row.material),
                "white_to_move": row.white_to_move,
                "static_cp": row.static_cp, "oracle_cp": row.oracle_cp,
                "oracle_is_mate": row.oracle_is_mate, "error": row.error,
                "stm_error": row.stm_error, "floor_cp": row.floor_cp,
                "eval_phase": row.eval_phase, "endgame_scale": row.endgame_scale,
                "terms": row.terms,
            }) + "\n")


# --------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------

# Real `eval` output, captured from a clang-cl Release build. Held as a fixture so the
# parser has a regression test with no engine present -- nightly runs --self-test on a
# runner that has no Stockfish and no lab corpus. It does NOT protect against a format
# drift in the engine; parse_breakdown's three-way total check does that, on the first row
# of any real run.
_EVAL_FIXTURE = """term       |  white |  black |    net
-----------+--------+--------+-------
material   |  13900 |  13900 |      0
pawns      |      0 |      0 |      0
rooks      |      0 |      0 |      0
pst        |      9 |     -3 |     12
mopup      |      0 |      0 |      0
bishops    |     30 |     30 |      0
castling   |      0 |      0 |      0
mobility   |    -66 |    -66 |      0
outposts   |      0 |      0 |      0
shelter    |     29 |     29 |      0
storm      |     -2 |     -2 |      0
kingfiles  |      0 |      0 |      0
kingattack |      0 |     -4 |      4
endgame    |      - |      - |      0
-----------+--------+--------+-------
sum (white pov)                    16
phase: 24/24
endgame scale: 16/16
static eval: -16 cp (Black to move; positive favours the side to move)
white pov: 16 cp"""


def self_test(out=sys.stdout) -> bool:
    ok = True

    def check(name, passed, detail=""):
        """`detail` is printed only on a failure: it is there to diagnose, not to decorate."""
        nonlocal ok
        ok = ok and passed
        suffix = f" -- {detail}" if (detail and not passed) else ""
        out.write(f"{'PASS' if passed else 'FAIL'}: {name}{suffix}\n")

    total, nets, phase, scale = parse_breakdown(_EVAL_FIXTURE)
    check("fixture parses to the white-pov total", total == 16, f"got {total}")
    check("every term row is read", set(nets) == set(TERMS),
          f"missing {sorted(set(TERMS) - set(nets))}")
    check("net-only rows parse", nets["endgame"] == 0)
    check("phase and endgame scale are read", (phase, scale) == (24, 16), f"got {phase}/{scale}")

    # The tripwire this module exists to avoid: `static eval:` is -16 for a black-to-move
    # position whose white-pov value is +16. Reading the wrong line inverts the row.
    check("the fixture's static eval really does differ in sign from white pov",
          "static eval: -16 cp" in _EVAL_FIXTURE and total == 16)

    bad = _EVAL_FIXTURE.replace("sum (white pov)                    16",
                                "sum (white pov)                    99")
    try:
        parse_breakdown(bad)
        check("disagreeing totals are rejected", False, "no exception raised")
    except EvalProtocolError:
        check("disagreeing totals are rejected", True)

    truncated = "\n".join(line for line in _EVAL_FIXTURE.splitlines()
                          if not line.startswith("mopup"))
    try:
        parse_breakdown(truncated)
        check("a missing term row is rejected", False, "no exception raised")
    except EvalProtocolError:
        check("a missing term row is rejected", True)

    board = chess.Board("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4")
    check("a capture is not quiet", not _is_quiet_move(board, chess.Move.from_uci("f3e5")))
    check("a developing move is quiet", _is_quiet_move(board, chess.Move.from_uci("b1c3")))
    check("a checking move is not quiet",
          not _is_quiet_move(chess.Board("4k3/8/8/8/8/8/8/R3K3 w Q - 0 1"),
                             chess.Move.from_uci("a1a8")))
    promo = chess.Board("4k3/P7/8/8/8/8/8/4K3 w - - 0 1")
    check("a promotion is not quiet", not _is_quiet_move(promo, chess.Move.from_uci("a7a8q")))

    row = Row(fen="8/8/8/8/8/8/8/K6k w - - 0 1", phase="endgame", game_key="g", band="10+",
              material=(), white_to_move=True, static_cp=100, oracle_cp=-100,
              oracle_is_mate=False, terms={t: 0 for t in TERMS}, eval_phase=0,
              endgame_scale=16, floor_cp=40)
    check("signed error is engine minus oracle", row.error == 200, f"got {row.error}")
    check("floor error is engine minus its own search", row.floor_error == 60,
          f"got {row.floor_error}")
    check("side-to-move pov keeps the sign for White", row.stm_error == 200)
    black = Row(**{**row.__dict__, "white_to_move": False})
    check("side-to-move pov flips it for Black", black.stm_error == -200,
          f"got {black.stm_error}")

    pool = []
    for i in range(30):
        for name, _low in amq.PHASE_BUCKETS:
            pool.append(Position(fen=f"fen-{name}-{i}", phase=name, game_key=f"g{i % 5}",
                                 ply_index=i, ply_since_book_exit=i, from_book=False))
    sampled, stats = stratified_sample(pool, 30, seed=1)
    counts = {name: sum(1 for p in sampled if p.phase == name) for name, _ in amq.PHASE_BUCKETS}
    check("the sample is stratified to equal counts per phase",
          len(set(counts.values())) == 1 and sum(counts.values()) == 30, f"{counts}")
    check("duplicate FENs are dropped once",
          stratified_sample(pool + pool, 30, seed=1)[1]["duplicates_dropped"] == len(pool))

    dup_only = [Position(fen="same", phase="endgame", game_key="g0", ply_index=0,
                         ply_since_book_exit=0, from_book=False)] * 4
    check("a phase short of its target contributes all it has",
          stratified_sample(dup_only, 30, seed=1)[1]["phase_short"].get("endgame") == 1)

    # The round trip the ad-hoc queries depend on: a row read back must carry the term
    # breakdown and both search scores, or every later cut is reconstructing them.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "rows.jsonl"
        write_rows([row], path)
        back = json.loads(path.read_text(encoding="utf-8").splitlines()[0])
    check("a written row keeps its term breakdown", set(back["terms"]) == set(TERMS))
    for field_name in ("fen", "phase", "static_cp", "oracle_cp", "floor_cp",
                       "white_to_move", "endgame_scale", "material"):
        check(f"a written row keeps {field_name}", field_name in back)

    out.write(f"\n{'SELF-TEST PASSED' if ok else 'SELF-TEST FAILED'}\n")
    return ok


def engine_check(engine_exe: str, out=sys.stdout) -> bool:
    """Live check that the built engine still speaks the table this module parses."""
    ok = True
    static = StaticEval(engine_exe)
    try:
        # Every position here must evaluate away from zero, or the mirror check passes
        # trivially: 0 == -0 tells us nothing about whether the sign convention holds.
        for fen, label in (
            ("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3", "black to move"),
            ("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQ1RK1 b kq - 5 5", "castled"),
            ("4k3/8/8/8/8/8/4P3/R3K3 w Q - 0 1", "rook and pawn up"),
        ):
            white_pov, nets, phase, scale = static.breakdown(fen)
            mirror = chess.Board(fen).mirror().fen()
            mirrored_pov, _n, _p, _s = static.breakdown(mirror)
            passed = mirrored_pov == -white_pov
            ok = ok and passed
            out.write(f"{'PASS' if passed else 'FAIL'}: {label} is colour-symmetric "
                      f"({white_pov} vs {mirrored_pov} mirrored)\n")
            out.write(f"      phase {phase}, scale {scale}, "
                      f"{sum(1 for v in nets.values() if v)} non-zero terms\n")
    finally:
        static.close()
    out.write(f"\n{'ENGINE CHECK PASSED' if ok else 'ENGINE CHECK FAILED'}\n")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", nargs="?", type=Path, help="directory of lab PGN shards")
    ap.add_argument("--engine-exe", default="", help="StratChessEvolved.exe under test")
    ap.add_argument("--engine", default="", help="oracle binary (default: STOCKFISH_PATH)")
    ap.add_argument("--limit", type=int, default=3000, help="positions to sample (default 3000)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--seed", type=int, default=20260922)
    ap.add_argument("--depth", type=int, default=ORACLE_DEPTH)
    ap.add_argument("--floor-depth", type=int, default=FLOOR_DEPTH)
    ap.add_argument("--batch", type=int, default=25)
    ap.add_argument("--skip-floor", action="store_true",
                    help="omit the engine's own search leg")
    ap.add_argument("--rows-jsonl", type=Path,
                    help="write every surviving row here -- this is the point of the run")
    ap.add_argument("--stats-json", type=Path, help="write the run's counts here")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--engine-check", action="store_true",
                    help="live check of the eval table against a built engine")
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1
    if args.engine_check:
        if not args.engine_exe:
            ap.error("--engine-check needs --engine-exe")
        return 0 if engine_check(args.engine_exe) else 1
    if not args.engine_exe:
        ap.error("--engine-exe is required: give the worktree-relative binary under test")
    oracle = args.engine or aeq.find_engine()
    if not oracle:
        ap.error("no oracle found; set STOCKFISH_PATH or pass --engine")
    if args.root is None:
        ap.error("a PGN root is required unless --self-test or --engine-check is given")
    if not args.rows_jsonl:
        ap.error("--rows-jsonl is required: the row export is what this run produces")

    rows, stats = run(args.root, args.engine_exe, oracle, args.limit, args.jobs,
                      args.seed, args.depth, args.floor_depth, args.skip_floor, args.batch)
    write_rows(rows, args.rows_jsonl)
    report_counts(stats)
    if args.stats_json:
        args.stats_json.write_text(json.dumps(stats, indent=2), encoding="utf-8")
    print(f"\nwrote {len(rows)} rows to {args.rows_jsonl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
