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

`--worst-jsonl` additionally writes one record per faulted row, the evidence a
later attribution stage replays. Its format, and what an interrupted run leaves
behind, are in Docs/MoveQualityExport.md.

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
import hashlib
import json
import os
import platform
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from contextlib import ExitStack
from dataclasses import dataclass
from itertools import chain
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_move_quality as amq  # noqa: E402  (path must be set first)
import external_quality_export as exp  # noqa: E402

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

if (exp.CLAMP_CP, exp.MATE_CP) != (CLAMP_CP, MATE_CP):
    raise RuntimeError("the export helper's clamp constants have drifted from this module's")

# The export filter is the blunder counter, not a second threshold beside it:
# every exported row is one c[3] event, which is what lets the footer reconcile.
if exp.MIN_LEGACY_LOSS_CP != amq.BLUNDER_CP:
    raise RuntimeError("the export threshold has drifted from the blunder threshold")

ENGINE_DIR = "EngineTesting"
ENGINE_EXE = "stockfish.exe" if os.name == "nt" else "stockfish"

# The oracle's UCI options, applied in _engine() and recorded in the export
# manifest. They are named once so a run can never be described by settings it
# was not actually scored under.
ORACLE_THREADS = 1
ORACLE_HASH_MB = 64

_ENGINE = None
_ENGINE_PATH = ""
_DEPTH = 12
_GAME = None              # `ucinewgame` scope; a new object per game
_EXPORT = False           # workers collect blunder records only when asked to
_START = time.monotonic()


def new_cell() -> list:
    """A per-(build, phase) counter row.

    [rows, self blunders, self swing sum, oracle blunders, loss sum,
     agreed rows, agreed loss sum, finite clipping misses]
    """
    return [0, 0, 0.0, 0, 0.0, 0, 0.0, 0]


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
        _ENGINE.configure({"Threads": ORACLE_THREADS, "Hash": ORACLE_HASH_MB})
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


def _init_worker(engine_path: str, depth: int, export: bool = False) -> None:
    global _ENGINE_PATH, _DEPTH, _EXPORT
    _ENGINE_PATH, _DEPTH, _EXPORT = engine_path, depth, export
    _new_game()


def oracle_endpoint(board: chess.Board, pov: chess.Color):
    """-> (typed endpoint from `pov`, the oracle's best move or None).

    Terminal positions are scored without asking the engine: a search on a
    finished game returns no principal variation, and mate/stalemate have exact
    values anyway.

    One `analyse()` per non-terminal call, exactly as before: the typed score is
    read off the same info dictionary, never a second search.
    """
    if board.is_checkmate():
        # The side to move is the side that was mated.
        return exp.checkmate_endpoint(board.turn == pov), None
    if board.is_stalemate():
        return exp.drawn_endpoint("stalemate"), None
    if board.is_insufficient_material():
        return exp.drawn_endpoint("insufficient_material"), None
    info = _engine().analyse(board, chess.engine.Limit(depth=_DEPTH), game=_GAME)
    pv = info.get("pv")
    best = pv[0] if pv else None
    score = exp.endpoint_from_score(info["score"].pov(pov), best.uci() if best else None)
    return score, best


def oracle_eval(board: chess.Board, pov: chess.Color):
    """-> (clamped score in centipawns from `pov`, the oracle's best move or None)."""
    endpoint, best = oracle_endpoint(board, pov)
    return endpoint.legacy_cp, best


@dataclass(frozen=True)
class PlyMeta:
    """One played ply: the state before it and the annotation that judged it."""

    ply_index: int
    mover: chess.Color
    build: str
    bucket: str
    cp: int | None
    depth: int | None
    seconds: float | None
    note: str | None
    annotation: str | None
    # A bare `{book}` comment, not `note == "book"`: an adjudication note may
    # also read "book" on a ply that carries a real score and stays eligible.
    is_book: bool
    before_fen: str
    after_fen: str
    played_uci: str


@dataclass(frozen=True)
class GameScan:
    """One parsed game: its header context, every ply and which rows qualify."""

    game_index: int
    headers: dict
    setup_fen: str
    plies: tuple[PlyMeta, ...]
    eligible: tuple[int, ...]
    book_exit_ply: int | None
    book_exit_basis: str
    input_index: int = 0      # this PGN's position in the manifest's input list


def _book_exit(plies: tuple[PlyMeta, ...], headers: dict) -> tuple[int | None, str]:
    """-> (book_exit_ply, basis), derived once per game from the book plies.

    A `{book}` annotation seen after real play makes the boundary unknowable
    rather than guessed at, because the prefix rule no longer holds.
    """
    prefix = 0
    for ply in plies:
        if not ply.is_book:
            break
        prefix += 1
    if any(ply.is_book for ply in plies[prefix:]):
        return None, "unknown"
    if prefix > 0:
        return prefix, "explicit_book_prefix"
    if headers.get("SetUp") == "1" and "FEN" in headers:
        return 0, "setup_assumed"
    return None, "unknown"


def scan_games(path: Path, input_index: int = 0) -> list[GameScan]:
    """Parse one PGN into per-game context, per-ply metadata and eligible row indices."""
    scans = []
    shard = path.parent.name
    for game_index, (headers, movetext) in enumerate(amq.parse_games(path)):
        where = f"{shard} round {headers.get('Round', '?')}"
        moves, saw_result = amq.split_movetext(movetext, where)
        if not saw_result or headers.get("Result", "*") == "*":
            continue
        fen = headers.get("FEN")
        board = chess.Board(fen) if fen else chess.Board()
        setup_fen = board.fen()
        builds = {chess.WHITE: headers.get("White", "?"), chess.BLACK: headers.get("Black", "?")}
        plies: list[PlyMeta] = []
        try:
            for idx, (san, comment) in enumerate(moves):
                mover = board.turn
                bucket = amq.phase_bucket(amq.board_phase(board))
                cp = depth = seconds = note = None
                if comment is not None:
                    cp, _mate, depth, seconds, note = amq.parse_comment(
                        comment, f"{where} move {idx}")
                before = board.fen()
                move = board.parse_san(san)
                board.push(move)
                plies.append(PlyMeta(
                    ply_index=idx, mover=mover, build=builds[mover], bucket=bucket, cp=cp,
                    depth=depth, seconds=seconds, note=note, annotation=comment,
                    is_book=comment == "book",
                    before_fen=before, after_fen=board.fen(), played_uci=move.uci(),
                ))
        except ValueError as exc:               # truncated or corrupt game
            # ValueError only, so that amq.ParseError propagates. An annotation
            # shape the parser does not recognise means a changed fastchess
            # version; skipping those games would silently drop whatever it now
            # spells differently and bias every cell in the report.
            print(f"warning: {where}: {exc}", file=sys.stderr)
            continue

        eligible = tuple(
            i for i in range(len(plies))
            if i + 2 < len(plies)
            and plies[i].cp is not None
            and plies[i + 2].cp is not None
            and abs(plies[i].cp) <= amq.CONTESTED_CP
        )
        if not eligible:
            continue
        book_exit_ply, book_exit_basis = _book_exit(plies, headers)
        scans.append(GameScan(
            game_index=game_index, headers=dict(headers), setup_fen=setup_fen,
            plies=tuple(plies), eligible=eligible,
            book_exit_ply=book_exit_ply, book_exit_basis=book_exit_basis,
            input_index=input_index,
        ))
    return scans


def extract(path: Path):
    """-> [[row, ...], ...], one list of contested rows per game.

    A row survives exactly the conditions Tier 1's contested self-swing needs: an
    annotation on this move and on the same mover's next one, and |own cp| within
    CONTESTED_CP. Grouping by game is what lets the bootstrap resample games.
    """
    games = []
    for scan in scan_games(path):
        rows = []
        for i in scan.eligible:
            ply, ply_next = scan.plies[i], scan.plies[i + 2]
            rows.append((ply.build, ply.bucket, ply.mover, ply.cp - ply_next.cp,
                         ply.before_fen, ply.after_fen, ply.played_uci))
        games.append(rows)
    return games


def moves_before(scan: GameScan, ply_index: int) -> list[str]:
    """The UCI moves played before this decision, book moves included."""
    return [p.played_uci for p in scan.plies[:ply_index]]


def ply_since_book_exit(scan: GameScan, ply_index: int) -> int | None:
    """Plies since the game left book, or None when the exit is unknown."""
    if scan.book_exit_ply is None:
        return None
    delta = ply_index - scan.book_exit_ply
    assert delta >= 0, "an eligible ply cannot precede its own game's book exit"
    return delta


def mover_name(color: chess.Color) -> str:
    """-> "white" | "black"."""
    return "white" if color == chess.WHITE else "black"


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
        for scan in games:
            _new_game()
            cache: dict[str, tuple] = {}
            cells: dict = defaultdict(new_cell)
            worst = []
            records: list | None = [] if _EXPORT else None
            _score_rows(scan, cache, cells, worst, records)
            worst.sort(reverse=True)
            results.append((dict(cells), worst[:5], records or []))
    finally:
        _close_engine()
    return results


def _new_game() -> None:
    """Start a fresh `ucinewgame` scope, so hash state does not cross games."""
    global _GAME
    _GAME = object()


def _score_rows(scan, cache, cells, worst, records=None) -> None:
    """Score one game's eligible rows, optionally collecting their export records.

    `records` is None when the export is off, and the arithmetic below is then
    exactly what it was before the export existed: same searches, same order,
    same counters. An exported row is a c[3] event, so the two can never disagree.
    """
    for i in scan.eligible:
        ply, ply_next = scan.plies[i], scan.plies[i + 2]
        mover, before, after = ply.mover, ply.before_fen, ply.after_fen
        self_swing = ply.cp - ply_next.cp
        if before not in cache:
            # Keyed by full FEN, and the FEN fixes the side to move, so a cached
            # score is always from the same point of view as its reader.
            cache[before] = oracle_endpoint(chess.Board(before), mover)
        endpoint_before, best = cache[before]
        endpoint_after, _ = oracle_endpoint(chess.Board(after), mover)
        loss = exp.legacy_loss_cp(endpoint_before, endpoint_after)
        c = cells[(ply.build, ply.bucket)]
        c[0] += 1
        c[1] += 1 if self_swing >= amq.BLUNDER_CP else 0
        c[2] += abs(self_swing)
        c[3] += 1 if loss >= amq.BLUNDER_CP else 0
        c[4] += loss
        if best is not None and best.uci() == ply.played_uci:
            c[5] += 1
            c[6] += loss
        # Counted over every eligible row, exported or not, so it measures rows
        # the clamp hid rather than rows the export happened to keep.
        if exp.is_finite_clipping_miss(endpoint_before, endpoint_after):
            c[7] += 1
        if loss >= REPORT_LOSS_CP and self_swing < amq.BLUNDER_CP:
            worst.append((loss, self_swing, ply.build, ply.bucket, before, ply.played_uci))
        if records is not None and exp.is_exported(endpoint_before, endpoint_after):
            records.append(_blunder_record(scan, i, endpoint_before, endpoint_after))


def _blunder_record(scan, i: int, endpoint_before, endpoint_after) -> dict:
    """Build one exported row. The move prefix is materialized only here."""
    ply, ply_next = scan.plies[i], scan.plies[i + 2]
    return exp.blunder_record(
        input_index=scan.input_index, game_index=scan.game_index, ply_index=ply.ply_index,
        headers=scan.headers, build=ply.build, mover=mover_name(ply.mover),
        setup_fen=scan.setup_fen, moves_before_uci=moves_before(scan, i),
        before_fen=ply.before_fen, after_fen=ply.after_fen, played_move_uci=ply.played_uci,
        phase=ply.bucket, ply_since_book_exit=ply_since_book_exit(scan, i),
        book_exit_basis=scan.book_exit_basis, annotation=ply.annotation,
        engine_score_cp=ply.cp, engine_depth=ply.depth, engine_time_s=ply.seconds,
        next_annotation=ply_next.annotation, next_engine_score_cp=ply_next.cp,
        self_swing_cp=ply.cp - ply_next.cp,
        oracle_before=endpoint_before, oracle_after=endpoint_after,
    )


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


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _input_entries(root: Path, files: list) -> list:
    """Identify each scanned PGN by path and content, in manifest order.

    The relative path is POSIX-separated so an artifact reads the same on either
    platform; a single-file root has nothing to be relative to and uses its name.
    """
    return [{"relative_path": path.relative_to(root).as_posix() if root.is_dir() else path.name,
             "sha256": _sha256(path)}
            for path in files]


def _manifest(root: Path, files: list, engine_path: str, depth: int, jobs: int, limit: int,
              shards: int, batch: int, source_run: str | None) -> dict:
    """Everything needed to identify what was scanned, and by what.

    Hashes rather than a checkout state: the artifact must stay verifiable after
    the branch that produced it is gone.
    """
    return exp.manifest_record(
        source_run=source_run,
        source_root=str(root.resolve()),
        inputs=_input_entries(root, files),
        producer={
            "python": platform.python_version(),
            "python_chess": chess.__version__,
            "analyze_external_quality_sha256": _sha256(Path(__file__).resolve()),
            "analyze_move_quality_sha256": _sha256(Path(amq.__file__).resolve()),
            "external_quality_export_sha256": _sha256(Path(exp.__file__).resolve()),
        },
        oracle={
            "binary": Path(engine_path).name,
            "sha256": _sha256(Path(engine_path)),
            "depth": depth,
            "threads": ORACLE_THREADS,
            "hash_mb": ORACLE_HASH_MB,
        },
        scan={"jobs": jobs, "batch": batch, "shards": shards, "games": limit},
    )


def _completion(cells: dict, scored_games: int) -> dict:
    """The footer, read straight off the counters the report is built from.

    Same slots, so a footer that reconciles is a footer describing this report:
    eligible rows are c[0], exported rows the c[3] blunders, misses c[7].
    """
    return exp.complete_record(
        eligible_rows=sum(c[0] for c in cells.values()),
        exported_rows=sum(c[3] for c in cells.values()),
        finite_clipping_misses=sum(c[7] for c in cells.values()),
        scored_games=scored_games,
        cells=[{"build": build, "phase": phase, "eligible_rows": c[0],
                "exported_rows": c[3], "finite_clipping_misses": c[7]}
               for (build, phase), c in cells.items()],
    )


def analyse(root: Path, engine_path: str, depth: int, jobs: int, limit: int, shards: int,
            batch: int, export_path: Path | None = None, source_run: str | None = None):
    cells: dict = defaultdict(new_cell)
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
    with ExitStack() as stack:
        writer = None
        if export_path is not None:
            writer = stack.enter_context(exp.ExportWriter(export_path))
            writer.write_manifest(_manifest(root, files, engine_path, depth, jobs, limit,
                                            shards, batch, source_run))
        ex = stack.enter_context(ProcessPoolExecutor(
            max_workers=jobs, initializer=_init_worker,
            initargs=(engine_path, depth, writer is not None)))
        # One shard at a time: the whole corpus of FENs at once is gigabytes, and
        # a shard is already wide enough to keep every worker busy.
        for n, path in enumerate(files, 1):
            started = time.monotonic()
            games = scan_games(path, n - 1)
            if limit:
                games = games[:limit]
            groups = batches(games, batch)
            _log(f"shard {n}/{len(files)} {path.parent.name}: {len(games)} games extracted, "
                 f"{len(groups)} batch(es)")
            for part, part_worst, records in chain.from_iterable(ex.map(score_batch, groups)):
                merge_cells(cells, part)
                per_game.append(part)
                worst.extend(part_worst)
                # Written and released as each result arrives: holding the corpus
                # of records to write at the end would defeat the shard loop.
                for record in records:
                    writer.write_blunder(record)
            _log(f"shard {n}/{len(files)} scored in {time.monotonic() - started:.0f}s, "
                 f"{sum(c[0] for c in cells.values())} rows so far")
        # The footer means "scoring finished", so it is written here rather than
        # after the report: a failed report leaves a complete, valid artifact,
        # and an interrupted scan leaves a footer-less one.
        if writer is not None:
            writer.write_complete(_completion(cells, len(per_game)))
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
    ap.add_argument("--worst-jsonl",
                    help="write the blunder-evidence export here (see Docs/MoveQualityExport.md)")
    ap.add_argument("--source-run", default=None,
                    help="run identifier recorded in the export manifest")
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in fixtures (no corpus needed) and exit")
    args = ap.parse_args()

    if args.self_test:
        print("self-test")
        return 0 if self_test() else 1
    if not args.root:
        ap.error("root is required unless --self-test is given")
    # Never inferred from the corpus directory: a run id is a claim about where
    # the games came from, and only the operator can make it.
    if args.source_run is not None and not args.worst_jsonl:
        ap.error("--source-run only applies to --worst-jsonl")

    export_path = Path(args.worst_jsonl) if args.worst_jsonl else None
    if export_path is not None:
        # Before the oracle starts: hours of searches must not end on a
        # destination that was unusable from the outset.
        try:
            exp.check_output_path(export_path, Path(args.json) if args.json else None)
        except (FileExistsError, ValueError) as exc:
            print(exc, file=sys.stderr)
            return 2

    engine_path = args.engine or find_engine()
    if not engine_path or not Path(engine_path).is_file():
        print(f"no oracle binary: put one in {ENGINE_DIR}/{ENGINE_EXE} beside the repo, "
              "or pass --engine / set STOCKFISH_PATH", file=sys.stderr)
        return 2
    _log(f"oracle {engine_path} at depth {args.depth}, {args.jobs} worker(s)")

    cells, per_game, worst = analyse(Path(args.root), engine_path, args.depth,
                                     args.jobs, args.games, args.shards, args.batch,
                                     export_path, args.source_run)
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
