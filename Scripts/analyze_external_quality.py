#!/usr/bin/env python3
"""External-oracle move-quality scan over strength-lab PGNs (issue #77, Tier 2).

Tier 1 (analyze_move_quality.py) grades the engine's own homework: it reads the
`{score/depth time}` annotations the builds wrote, so a position both builds
misjudge in the same direction produces no swing and is invisible. This replays
the same moves under a strong external engine and reports centipawn loss against
*its* judgement.

Only the source of judgement changes. The contested filter, the phase buckets,
the blunder threshold and the parser all come from analyze_move_quality.py, so
the two tiers' tables describe the same rows and can be read side by side -- the
`self` columns below reproduce Tier 1 on exactly the rows Tier 2 judged.

Method notes, limits and the committed baselines live in Docs/MoveQuality.md;
read that before acting on any number this prints.

The oracle is not in the checkout. Put a Stockfish binary and its GPL-3 licence
in EngineTesting/ beside fastchess.exe, the way the opening book and fastchess
already live outside the repo; --engine or STOCKFISH_PATH override the search.

    gh run download <run_id> --repo theEscape2207/StratChess \
        -p 'strength-<run_id>-shard-*' -D <dir>
    python analyze_external_quality.py --self-test
    python analyze_external_quality.py <dir> --depth 12 --json out.json

The `noise` column is the mean loss over rows where the played move IS the
oracle's own first choice, where the residual can only be search instability.
Read it as what it is: a *conditional* residual over the rows the oracle already
agreed with, which are the narrower positions. It is a lower bound on the
oracle's error, not a bound on it, and it says nothing about the disagreement
rows that carry the report's signal (#483). See Docs/MoveQuality.md.
"""

from __future__ import annotations

import argparse
import atexit
import json
import os
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from itertools import chain
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_move_quality as amq  # noqa: E402  (path must be set first)

try:
    import chess
    import chess.engine
except ImportError as exc:  # pragma: no cover - environment guard, not test logic
    sys.exit(
        "analyze_external_quality.py requires the 'python-chess' package.\n"
        "Install it with: pip install python-chess\n"
        f"(import failed: {exc})"
    )

# Evals are clamped before any arithmetic: a mate score is not on the centipawn
# scale and would saturate every mean it entered. Tier 1 excludes mate scores for
# the same reason; clamping keeps the row rather than dropping it, because the
# move that walked into the mate is exactly the move worth counting.
CLAMP_CP = 1000
MATE_CP = 100000          # what python-chess substitutes before we clamp
REPORT_LOSS_CP = 300      # a row this bad that Tier 1 scored clean gets listed
ENGINE_DIR = "EngineTesting"
ENGINE_EXE = "stockfish.exe" if os.name == "nt" else "stockfish"

_ENGINE = None
_ENGINE_PATH = ""
_DEPTH = 12
_GAME = None              # `ucinewgame` scope; a new object per game
_START = time.monotonic()


def _log(message: str) -> None:
    """Progress line stamped with elapsed time, flushed as it is written.

    The scan runs long enough that its cost has to be read off its own output;
    inferring it afterwards from process CPU gets the answer wrong.
    """
    print(f"[{time.monotonic() - _START:7.1f}s] {message}", file=sys.stderr, flush=True)


def find_engine() -> str:
    """Locate the oracle binary outside the checkout, or return ''.

    STOCKFISH_PATH wins; otherwise walk up from this script looking for a sibling
    EngineTesting/, which is where the external test assets live. A worktree sits
    several levels below the repo root, so the walk has to be a walk.
    """
    env = os.environ.get("STOCKFISH_PATH")
    if env:
        return env
    for parent in Path(__file__).resolve().parents:
        candidate = parent.parent / ENGINE_DIR / ENGINE_EXE
        if candidate.is_file():
            return str(candidate)
    return ""


def _engine():
    global _ENGINE
    if _ENGINE is None:
        _ENGINE = chess.engine.SimpleEngine.popen_uci(_ENGINE_PATH)
        _ENGINE.configure({"Threads": 1, "Hash": 64})
        atexit.register(_close_engine)   # backstop only; the scan closes explicitly
    return _ENGINE


def _close_engine() -> None:
    global _ENGINE
    if _ENGINE is not None:
        engine, _ENGINE = _ENGINE, None
        try:
            engine.quit()
        except Exception:                       # pragma: no cover - shutdown race
            pass
        finally:
            # quit() normally takes the transport down with it, but if it raised
            # the event-loop thread is still waiting on a live subprocess, and a
            # worker holding one of those never exits.
            try:
                engine.close()
            except Exception:                   # pragma: no cover - shutdown race
                pass


def _init_worker(engine_path: str, depth: int) -> None:
    global _ENGINE_PATH, _DEPTH
    _ENGINE_PATH, _DEPTH = engine_path, depth
    _new_game()


def oracle_eval(board: chess.Board, pov: chess.Color):
    """-> (clamped score in centipawns from `pov`, the oracle's best move or None).

    Terminal positions are scored without asking the engine: a search on a
    finished game returns no principal variation, and mate/stalemate have exact
    values anyway.
    """
    if board.is_checkmate():
        # The side to move is the side that was mated.
        return (-CLAMP_CP if board.turn == pov else CLAMP_CP), None
    if board.is_stalemate() or board.is_insufficient_material():
        return 0, None
    info = _engine().analyse(board, chess.engine.Limit(depth=_DEPTH), game=_GAME)
    cp = info["score"].pov(pov).score(mate_score=MATE_CP)
    pv = info.get("pv")
    return max(-CLAMP_CP, min(CLAMP_CP, cp)), (pv[0] if pv else None)


def extract(path: Path):
    """-> [[row, ...], ...], one list of contested rows per game.

    A row survives exactly the conditions Tier 1's contested self-swing needs: an
    annotation on this move and on the same mover's next one, and |own cp| within
    CONTESTED_CP. Grouping by game is what lets the bootstrap resample games.
    """
    games = []
    shard = path.parent.name
    for headers, movetext in amq.parse_games(path):
        where = f"{shard} round {headers.get('Round', '?')}"
        moves, saw_result = amq.split_movetext(movetext, where)
        if not saw_result or headers.get("Result", "*") == "*":
            continue
        fen = headers.get("FEN")
        board = chess.Board(fen) if fen else chess.Board()
        builds = {chess.WHITE: headers.get("White", "?"), chess.BLACK: headers.get("Black", "?")}
        recs = []
        try:
            for idx, (san, comment) in enumerate(moves):
                mover = board.turn
                bucket = amq.phase_bucket(amq.board_phase(board))
                cp = None
                if comment is not None:
                    cp, _mate, _depth, _secs, _note = amq.parse_comment(
                        comment, f"{where} move {idx}")
                before = board.fen()
                move = board.parse_san(san)
                board.push(move)
                recs.append((mover, builds[mover], bucket, cp, before, board.fen(), move.uci()))
        except ValueError as exc:               # truncated or corrupt game
            # ValueError only, so that amq.ParseError propagates. An annotation
            # shape the parser does not recognise means a changed fastchess
            # version; skipping those games would silently drop whatever it now
            # spells differently and bias every cell in the report.
            print(f"warning: {where}: {exc}", file=sys.stderr)
            continue

        rows = []
        for i, (mover, build, bucket, cp, before, after, played) in enumerate(recs):
            if i + 2 >= len(recs):
                continue
            cp_next = recs[i + 2][3]
            if cp is None or cp_next is None or abs(cp) > amq.CONTESTED_CP:
                continue
            rows.append((build, bucket, mover, cp - cp_next, before, after, played))
        if rows:
            games.append(rows)
    return games


def score_batch(games):
    """-> [(cells, worst), ...], one entry per game, scored by one oracle process.

    Counters are per game so the bootstrap can resample whole games; plies inside
    one game share its opening, its builds and its result, so they are nothing
    like independent draws.

    **The engine's lifetime ends inside the task, never at interpreter exit.**
    python-chess runs the engine on a background event loop whose thread waits on
    the subprocess, so a pool worker still holding one does not exit and the
    parent waits for it forever -- which looks exactly like a finished run whose
    report never prints.

    Games stay independent of each other because each is scored under its own
    `game` token: python-chess sends `ucinewgame` and waits for `readyok` when
    the token changes, and Stockfish clears its hash there. Scoring a batch is
    therefore the same experiment as scoring its games one process at a time,
    for a fraction of the NNUE loads.
    """
    results = []
    try:
        for rows in games:
            _new_game()
            cache: dict[str, tuple] = {}
            cells: dict = defaultdict(lambda: [0, 0, 0.0, 0, 0.0, 0, 0.0])
            worst = []
            _score_rows(rows, cache, cells, worst)
            worst.sort(reverse=True)
            results.append((dict(cells), worst[:5]))
    finally:
        _close_engine()
    return results


def _new_game() -> None:
    """Start a fresh `ucinewgame` scope, so hash state does not cross games."""
    global _GAME
    _GAME = object()


def _score_rows(rows, cache, cells, worst) -> None:
    for build, bucket, mover, self_swing, before, after, played in rows:
        if before not in cache:
            # Keyed by full FEN, and the FEN fixes the side to move, so a cached
            # score is always from the same point of view as its reader.
            cache[before] = oracle_eval(chess.Board(before), mover)
        cp_before, best = cache[before]
        cp_after, _ = oracle_eval(chess.Board(after), mover)
        loss = max(0, cp_before - cp_after)
        c = cells[(build, bucket)]
        c[0] += 1
        c[1] += 1 if self_swing >= amq.BLUNDER_CP else 0
        c[2] += abs(self_swing)
        c[3] += 1 if loss >= amq.BLUNDER_CP else 0
        c[4] += loss
        if best is not None and best.uci() == played:
            c[5] += 1
            c[6] += loss
        if loss >= REPORT_LOSS_CP and self_swing < amq.BLUNDER_CP:
            worst.append((loss, self_swing, build, bucket, before, played))


def merge_cells(dst, src) -> None:
    for key, counters in src.items():
        row = dst[key]
        for i, v in enumerate(counters):
            row[i] += v


def total(games, key, field) -> float:
    return sum(g[key][field] for g in games if key in g)


def rate(games, key, num, den=0):
    n = total(games, key, den)
    return 100.0 * total(games, key, num) / n if n else None


def mean(games, key, num, den=0):
    n = total(games, key, den)
    return total(games, key, num) / n if n else None


def report(cells, per_game, worst, depth, samples, out=sys.stdout) -> None:
    w = out.write
    rows = sum(c[0] for c in cells.values())
    w(f"\nOracle depth {depth}, {len(per_game)} games, {rows} contested rows, "
      f"{samples} bootstrap resamples\n\n")
    header = (f"{'build':<22}{'phase':<12}{'n':>8}{'self ACPL':>11}{'self blu%':>11}"
              f"{'ext ACPL':>11}{'ext blu%':>11}{'agree%':>9}{'noise':>8}\n")
    w(header)
    w("-" * (len(header) - 1) + "\n")
    for key in sorted(cells):
        n = cells[key][0]
        w(f"{key[0]:<22}{key[1]:<12}{n:>8}"
          f"{mean(per_game, key, 2):>11.1f}{rate(per_game, key, 1):>11.2f}"
          f"{mean(per_game, key, 4):>11.1f}{rate(per_game, key, 3):>11.2f}"
          f"{rate(per_game, key, 5):>9.1f}"
          f"{(mean(per_game, key, 6, 5) or 0.0):>8.1f}\n")

    # The bootstrap is single-threaded and takes minutes on a full corpus, with
    # every oracle process sitting idle: without this line that stretch looks
    # exactly like a hang.
    _log(f"resampling {len(per_game)} games x {samples} for intervals")
    w("\nExternal blunder rate, 95% interval (games resampled)\n")
    for key in sorted(cells):
        point, lo, hi = amq.bootstrap(per_game, lambda g, k=key: rate(g, k, 3), samples=samples)
        w(f"  {key[0]:<22}{key[1]:<12}{amq.ci(point, lo, hi, 2):>26}%\n")
    w("\nExternal ACPL, 95% interval (games resampled)\n")
    for key in sorted(cells):
        point, lo, hi = amq.bootstrap(per_game, lambda g, k=key: mean(g, k, 4), samples=samples)
        w(f"  {key[0]:<22}{key[1]:<12}{amq.ci(point, lo, hi, 1):>26}\n")

    w(f"\nWorst rows the oracle faults and Tier 1 does not (loss >= {REPORT_LOSS_CP}cp)\n")
    for loss, swing, build, bucket, fen, played in sorted(worst, reverse=True)[:20]:
        w(f"  -{loss:>4}cp  self {swing:>+5}  {build} {bucket:<11} {played}  {fen}\n")


def batches(games, size: int):
    """-> [[game, ...], ...], `size` games each. One oracle process per batch.

    Sized so every worker gets several, because a batch is the unit of work the
    pool can hand out: one batch per worker would leave workers idle behind the
    slowest game in their share.
    """
    size = max(1, size)
    return [games[i:i + size] for i in range(0, len(games), size)]


def analyse(root: Path, engine_path: str, depth: int, jobs: int, limit: int, shards: int,
            batch: int):
    cells: dict = defaultdict(lambda: [0, 0, 0.0, 0, 0.0, 0, 0.0])
    per_game: list = []
    worst: list = []
    files = sorted(root.rglob("*.pgn")) if root.is_dir() else [root]
    if not files:
        print(f"no .pgn under {root}", file=sys.stderr)
        return None, None, None
    # Shards are independent samples of the same match, so a prefix of them is a
    # smaller run of the same experiment, not a biased one.
    if shards:
        files = files[:shards]
    with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker,
                             initargs=(engine_path, depth)) as ex:
        # One shard at a time: the whole corpus of FENs at once is gigabytes, and
        # a shard is already wide enough to keep every worker busy.
        for n, path in enumerate(files, 1):
            started = time.monotonic()
            games = extract(path)
            if limit:
                games = games[:limit]
            groups = batches(games, batch)
            _log(f"shard {n}/{len(files)} {path.parent.name}: {len(games)} games extracted, "
                 f"{len(groups)} batch(es)")
            for part, part_worst in chain.from_iterable(ex.map(score_batch, groups)):
                merge_cells(cells, part)
                per_game.append(part)
                worst.extend(part_worst)
            _log(f"shard {n}/{len(files)} scored in {time.monotonic() - started:.0f}s, "
                 f"{sum(c[0] for c in cells.values())} rows so far")
    return dict(cells), per_game, worst


def self_test(out=sys.stdout) -> bool:
    """Fixtures for the scoring definitions, plus a live oracle check if present.

    The live checks are the ones that matter: a point-of-view slip in
    oracle_eval() inverts every loss in the report without failing anything, and
    no fixture that stubs the engine out can catch it.
    """
    failures = []

    def check(name, passed, detail):
        out.write(f"  [{'PASS' if passed else 'FAIL'}] {name}: {detail}\n")
        if not passed:
            failures.append(name)

    # Contested-row extraction must agree with Tier 1's filter, including the
    # "same mover two plies later" restriction that costs the last two rows.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "match.pgn"
        path.write_text(amq.SELF_TEST_PGN, encoding="utf-8")
        games = extract(path)
    rows = [r for g in games for r in g]
    check("extract skips the corrupt fixture game", len(games) == 1, f"{len(games)} game(s)")

    # An illegal move is one game lost; an unreadable annotation is a changed
    # fastchess version, and must stop the run rather than quietly shrink it.
    bad_comment = amq.SELF_TEST_PGN.replace("{+0.20/10 0.500s}", "{+0.20/10 500ms}", 1)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "match.pgn"
        path.write_text(bad_comment, encoding="utf-8")
        try:
            extract(path)
            raised = False
        except amq.ParseError:
            raised = True
    check("an unreadable annotation aborts instead of skipping the game", raised,
          "ParseError" if raised else "no exception")
    check("row phases are Tier 1 buckets",
          bool(rows) and {r[1] for r in rows} <= {name for name, _low in amq.PHASE_BUCKETS},
          f"{sorted({r[1] for r in rows})}")

    # Bootstrap plumbing: a corpus half of whose games blunder twice in four rows
    # and half not at all must recover 25%, over an interval that contains it and
    # is not the degenerate point interval an all-identical corpus would give.
    synthetic = [{("b", "middlegame"): [4, 1, 0.0, 2 * (i % 2), 400.0, 0, 0.0]}
                 for i in range(40)]
    point, lo, hi = amq.bootstrap(synthetic, lambda g: rate(g, ("b", "middlegame"), 3), samples=200)
    check("bootstrap recovers a known rate",
          point == 25.0 and lo is not None and lo < 25.0 < hi,
          f"{amq.ci(point, lo, hi, 1)}%")
    # Batching must partition, not sample: every game exactly once, in order.
    split = batches(list(range(10)), 4)
    check("batches partition the shard", [g for b in split for g in b] == list(range(10))
          and [len(b) for b in split] == [4, 4, 2], f"{split}")
    check("a batch size of zero is still one game per batch",
          len(batches(list(range(3)), 0)) == 3, f"{batches(list(range(3)), 0)}")

    check("ACPL is a per-row mean, not a per-game one",
          mean(synthetic, ("b", "middlegame"), 4) == 100.0,
          f"{mean(synthetic, ('b', 'middlegame'), 4)}")

    engine_path = find_engine()
    if not engine_path:
        check("oracle binary present", False,
              f"no {ENGINE_DIR}/{ENGINE_EXE} beside the repo and no STOCKFISH_PATH; "
              "the point-of-view checks cannot run")
        return not failures

    _init_worker(engine_path, 8)
    try:
        # Point of view. The same position read from both sides; a pov slip makes
        # one of these two agree in sign with the other.
        white_up = chess.Board("4k3/8/8/8/8/8/8/3QK3 w - - 0 1")
        cp_white, best = oracle_eval(white_up, chess.WHITE)
        cp_black, _ = oracle_eval(white_up, chess.BLACK)
        check("a queen up scores positive for its owner", cp_white > 300, f"{cp_white}cp")
        check("the same position scores negative for the other side",
              cp_black < -300, f"{cp_black}cp")
        check("the oracle returns a legal best move",
              best is not None and best in white_up.legal_moves, f"{best}")

        # Clamping, and the terminal shortcut that never reaches the engine.
        mate_soon, _ = oracle_eval(chess.Board("6k1/5ppp/8/8/8/8/8/R3K2R w KQ - 0 1"), chess.WHITE)
        check("a mate score is clamped", mate_soon <= CLAMP_CP, f"{mate_soon}cp")
        mated, mated_best = oracle_eval(chess.Board("7k/5QK1/8/8/8/8/8/8 b - - 0 1"), chess.WHITE)
        check("checkmate is scored for the mating side without a search",
              mated == CLAMP_CP and mated_best is None, f"{mated}cp, best {mated_best}")
        stale, _ = oracle_eval(chess.Board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"), chess.WHITE)
        check("stalemate is a draw", stale == 0, f"{stale}cp")

        # Loss direction end to end: hanging the queen must cost, and the sign of
        # the cost must not depend on which colour blundered.
        for fen, blunder, colour in (("3rk3/8/8/8/8/8/8/3QK3 w - - 0 1", "d1d8", "White"),
                                     ("3qk3/8/8/8/8/8/8/3RK3 b - - 0 1", "d8d1", "Black")):
            board = chess.Board(fen)
            pov = board.turn
            before, _ = oracle_eval(board, pov)
            board.push(chess.Move.from_uci(blunder))
            after, _ = oracle_eval(board, pov)
            check(f"{colour} trading its queen for a rook is a loss, not a gain",
                  before - after >= amq.BLUNDER_CP, f"{before}cp -> {after}cp")
    finally:
        _close_engine()
    return not failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", nargs="?",
                    help="directory holding strength-<run>-shard-*/match.pgn")
    ap.add_argument("--engine", default="",
                    help=f"oracle binary (default: ../{ENGINE_DIR}/{ENGINE_EXE})")
    ap.add_argument("--depth", type=int, default=12, help="fixed oracle search depth (default 12)")
    # A quarter of the box by default. This runs for hours on an interactive
    # machine, and one worker is one busy engine process: a default sized to the
    # core count makes the machine unusable for as long as the scan lasts.
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 4),
                    help="parallel oracle processes, each one busy core (default: a quarter)")
    ap.add_argument("--games", type=int, default=0, help="cap games per shard (0 = all)")
    ap.add_argument("--shards", type=int, default=0,
                    help="scan only the first N shards (0 = all); each is an independent sample")
    # One oracle process per batch. Batching saves NNUE loads and costs balance,
    # and the balance is worth more: 24 games per process measured 18% slower
    # than one, over runs whose reports were byte-identical. The knob stays so a
    # different box can be measured rather than assumed.
    ap.add_argument("--batch", type=int, default=1,
                    help="games per oracle process (default 1; larger measured slower)")
    ap.add_argument("--samples", type=int, default=amq.BOOT_SAMPLES,
                    help="bootstrap resamples behind every interval")
    ap.add_argument("--json", help="also write the merged raw counters here")
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in fixtures (no corpus needed) and exit")
    args = ap.parse_args()

    if args.self_test:
        print("self-test")
        return 0 if self_test() else 1
    if not args.root:
        ap.error("root is required unless --self-test is given")

    engine_path = args.engine or find_engine()
    if not engine_path or not Path(engine_path).is_file():
        print(f"no oracle binary: put one in {ENGINE_DIR}/{ENGINE_EXE} beside the repo, "
              "or pass --engine / set STOCKFISH_PATH", file=sys.stderr)
        return 2
    _log(f"oracle {engine_path} at depth {args.depth}, {args.jobs} worker(s)")

    cells, per_game, worst = analyse(Path(args.root), engine_path, args.depth,
                                     args.jobs, args.games, args.shards, args.batch)
    if cells is None:
        return 2
    if not cells:
        print("no contested rows found", file=sys.stderr)
        return 1

    report(cells, per_game, worst, args.depth, args.samples)
    # stdout is block-buffered when it is a file or a pipe, so an interpreter that
    # never reaches its own exit loses the whole report.
    sys.stdout.flush()
    if args.json:
        Path(args.json).write_text(
            json.dumps({"depth": args.depth, "games": len(per_game),
                        "cells": {"|".join(k): v for k, v in cells.items()}}, indent=1),
            encoding="utf-8")
    _log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
