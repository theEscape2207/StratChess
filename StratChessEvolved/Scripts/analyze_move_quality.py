#!/usr/bin/env python3
"""Move-quality scan over strength-lab PGNs (issue #77, Tier 1).

Every strength-lab run writes a fully annotated PGN of every game it plays; the
run summary consumes one number from it (pooled Elo), which says whether a
change helped and nothing about where. This reads the same PGNs and reports
where the engine's own judgement moves, bucketed by game phase, build and
remaining clock.

Method notes, limits and the committed baseline live in Docs/MoveQuality.md; read
that before acting on any number this prints.

Fetch a run's corpus first (artifacts are retained 90 days):

    gh run download <run_id> --repo theEscape2207/StratChess \
        -p 'strength-<run_id>-shard-*' -D <dir>
    python analyze_move_quality.py <dir> --self-check
    python analyze_move_quality.py <dir> --json out.json

--self-check is the gate, not a formality: it asserts that every game and every
annotation in the input was accounted for, and that the score perspective below
is the one the corpus actually uses.

Annotation format, confirmed against fastchess v1.8.0-alpha artifacts:

  9. dxc5 {+0.91/12 0.439s} dxc4 {-1.03/12 0.461s}

Scores are MOVER-RELATIVE (standard UCI convention), not White-relative: in the
line above both builds agree White stands better. Every formula here depends on
that; --self-check fails loudly if a corpus ever violates it.

Mate scores are spelled `+M13/14 0.080s`. They are counted, never averaged: a
mate score is not on the centipawn scale and saturates any mean it enters.

Every blunder statistic is reported twice, over all positions and over CONTESTED
positions only (the mover reported no more than +/-150 cp before moving). Read
the contested rows. The unrestricted ones are dominated by already-lost
positions, where the losing side flails and its swings cost nothing -- on the
first baseline that distinction inverted the conclusion.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import chess

# Game phase mirrors Eval.h (PHASE_KNIGHT/BISHOP/ROOK/QUEEN, MAX_GAME_PHASE) --
# Eval.h is the source of truth; a drifted copy here shifts bucket boundaries,
# it does not invalidate the numbers inside a bucket.
PHASE_WEIGHT = {chess.KNIGHT: 1, chess.BISHOP: 1, chess.ROOK: 2, chess.QUEEN: 4}
MAX_GAME_PHASE = 24
# Endgame boundary is MOPUP_MAX_LOSER_PHASE (Eval.h): approximately where
# eval_mopup starts to apply. The engine gates on the loser's phase and this
# buckets on the total, so the alignment is indicative, not exact.
PHASE_BUCKETS = (("opening", 20), ("middlegame", 7), ("endgame", 0))

HEADER_RE = re.compile(r'^\[(\w+)\s+"(.*)"\]$')
# `+0.91/12 0.439s` or `+M13/14 0.080s`, with an optional adjudication note.
SCORE_RE = re.compile(
    r"^([+-]?)(M?)(\d+(?:\.\d+)?)/(\d+)\s+([\d.]+)s(?:,\s*(.+))?$"
)
TOKEN_RE = re.compile(r"\{([^}]*)\}|(\S+)")
MOVENO_RE = re.compile(r"^\d+\.(\.\.)?$")
RESULT_TOKENS = {"1-0", "0-1", "1/2-1/2", "*"}

BLUNDER_CP = 150          # self-swing at or above this is a "blunder" here
BIG_BLUNDER_CP = 300
ADVANTAGE_CP = 150        # "held a winning-ish edge" threshold for squander stats
CONTESTED_CP = 150        # a position is contested while the mover reports no more than this
CLOCK_BANDS = ((8.0, "clock>8s"), (5.0, "clock5-8s"), (2.0, "clock2-5s"), (0.0, "clock<2s"))


def phase_bucket(phase: int) -> str:
    for name, low in PHASE_BUCKETS:
        if phase >= low:
            return name
    return "endgame"


def clock_band(remaining: float) -> str:
    for low, name in CLOCK_BANDS:
        if remaining >= low:
            return name
    return "clock<2s"


class ParseError(RuntimeError):
    pass


def parse_games(path: Path):
    """Yield (headers, [(san, comment), ...]) for each game in a PGN file."""
    headers: dict[str, str] = {}
    movetext: list[str] = []
    in_moves = False
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            stripped = line.strip()
            if stripped.startswith("[") and not in_moves:
                m = HEADER_RE.match(stripped)
                if m:
                    headers[m.group(1)] = m.group(2)
                continue
            if not stripped:
                if in_moves and movetext:
                    yield headers, " ".join(movetext)
                    headers, movetext, in_moves = {}, [], False
                continue
            in_moves = True
            movetext.append(stripped)
    if in_moves and movetext:
        yield headers, " ".join(movetext)


def split_movetext(movetext: str, where: str):
    """-> [(san, comment_or_None)], and whether the game ended with a result token."""
    moves: list[list] = []
    saw_result = False
    for m in TOKEN_RE.finditer(movetext):
        comment, token = m.group(1), m.group(2)
        if comment is not None:
            if not moves:
                raise ParseError(f"{where}: comment before any move: {{{comment}}}")
            moves[-1][1] = comment
            continue
        if MOVENO_RE.match(token) or token in ("...",):
            continue
        if token in RESULT_TOKENS:
            saw_result = True
            continue
        moves.append([token, None])
    return moves, saw_result


def parse_comment(comment: str, where: str):
    """-> (score_cp | None, mate_signed | None, depth, seconds, note)."""
    if comment == "book":
        return None, None, None, None, "book"
    m = SCORE_RE.match(comment.strip())
    if not m:
        raise ParseError(f"{where}: unrecognised score comment {{{comment}}}")
    sign, mate, value, depth, secs, note = m.groups()
    s = -1 if sign == "-" else 1
    if mate:
        return None, s * int(value), int(depth), float(secs), note
    return s * int(round(float(value) * 100)), None, int(depth), float(secs), note


PAWN_RICH = 3  # both sides holding this many pawns => no fortress / bare-piece ending


def material_classes(board: chess.Board):
    """Drawish material configurations, as (class, stronger colour) pairs.

    Named for what the defender can hold, which is what the evaluator has no
    concept of (issue #128):
      KminorK   K + one minor vs bare K -- drawn by insufficient material
      RvsMinor  KR vs KB / KR vs KN, pawnless -- the defender normally holds
      RminorR   KRB / KRN vs KR, pawnless -- a fortress the eval scores a piece up
      OCB       one bishop each on opposite colours, pawns allowed, nothing else
    """
    def counts(c):
        return tuple(len(board.pieces(pt, c)) for pt in
                     (chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN))

    white, black = counts(chess.WHITE), counts(chess.BLACK)
    found = []
    for strong, weak, color in ((white, black, chess.WHITE), (black, white, chess.BLACK)):
        p, n, b, r, q = strong
        wp, wn, wb, wr, wq = weak
        if (p, q, r) == (0, 0, 1) and n + b == 1 and (wp, wn, wb, wr, wq) == (0, 0, 0, 1, 0):
            found.append(("RminorR", color))
        if (p, n, b, r, q) == (0, 0, 0, 1, 0) and (wp, wr, wq) == (0, 0, 0) and wn + wb == 1:
            found.append(("RvsMinor", color))
        if (p, r, q) == (0, 0, 0) and n + b == 1 and weak == (0, 0, 0, 0, 0):
            found.append(("KminorK", color))
    if (white[1], black[1], white[3], black[3], white[4], black[4]) == (0, 0, 0, 0, 0, 0) \
            and white[2] == black[2] == 1:
        wb = board.pieces(chess.BISHOP, chess.WHITE)
        bb = board.pieces(chess.BISHOP, chess.BLACK)
        dark = chess.SquareSet(chess.BB_DARK_SQUARES)
        if (next(iter(wb)) in dark) != (next(iter(bb)) in dark):
            found.append(("OCB", chess.WHITE))  # stronger side resolved from the score
    return found


def board_phase(board: chess.Board) -> int:
    phase = 0
    for piece_type, weight in PHASE_WEIGHT.items():
        phase += weight * len(board.pieces(piece_type, chess.WHITE))
        phase += weight * len(board.pieces(piece_type, chess.BLACK))
    return min(phase, MAX_GAME_PHASE)


def time_control(headers: dict[str, str]) -> tuple[float, float]:
    tc = headers.get("TimeControl", "")
    m = re.match(r"^(\d+(?:\.\d+)?)(?:\+(\d+(?:\.\d+)?))?$", tc)
    if not m:
        return (0.0, 0.0)
    return float(m.group(1)), float(m.group(2) or 0.0)


def new_stats() -> dict:
    return {
        "games": 0,
        "games_skipped": 0,
        "moves": 0,
        "mate_scores": 0,
        "book_moves": 0,
        # keyed by (build, phase) and (build, clock band)
        "swing": defaultdict(lambda: [0, 0, 0, 0, 0.0]),   # n, blunders, big, mateflips, sum_abs
        # Same, restricted to CONTESTED positions (|score before the move| <= 150cp).
        # An engine that is already lost reports large swings while flailing; those
        # cost nothing. Only a mistake made from a playable position costs points.
        "swing_contested": defaultdict(lambda: [0, 0, 0, 0, 0.0]),
        "piece_moves": Counter(),         # denominator for the per-piece blunder rate
        "piece_blunders": Counter(),      # contested only
        "phase_clock": defaultdict(lambda: [0, 0, 0.0]),  # (phase, band) -> n, blunders, sum_abs
        "final_score": Counter(),         # (termination reason, best final score bucket)
        "swing_clock": defaultdict(lambda: [0, 0, 0.0]),
        "gap": defaultdict(lambda: [0, 0.0]),
        "blunder_ctx": Counter(),
        "calib": Counter(),          # (phase, score_bucket, outcome) -> n
        "calib_fine": Counter(),     # (phase, cp rounded to 25, outcome) -> n
        # Same again, split on whether both sides still hold >= PAWN_RICH pawns.
        # This is the control that separates "the endgame eval is optimistic"
        # from "pawn-poor material is drawish and the eval does not know it".
        "calib_pawns": Counter(),    # (phase, rich?, cp rounded to 50, outcome) -> n
        # Per-GAME reach of the drawish material classes, counted once at the
        # first position that reaches each class. Plies would over-weight the
        # long games, which are exactly the drawn ones.
        "material_class": Counter(),  # (class, score bucket, outcome) -> n
        # --self-check: signed self-swing summed per eventual outcome. A build
        # that loses must, on aggregate, have watched its own score fall.
        "swing_by_result": defaultdict(lambda: [0, 0.0]),
        "comment_count": 0,          # {..} comments seen by the tokenizer
        "results": Counter(),
        "terminations": Counter(),
        "draw_reasons": Counter(),
        "win_reasons": Counter(),
        "plies": Counter(),          # histogram bucket -> n
        "long_games": 0,
        "squander": Counter(),       # (phase_of_peak, outcome) for peaks >= ADVANTAGE_CP
        "squander_games": 0,
        "mate_claims": Counter(),    # (build, outcome) for games where it announced mate
        "draw_peak": Counter(),      # (draw reason, best peak score bucket)
        "depth_by_phase": defaultdict(lambda: [0, 0]),
        "time_by_phase": defaultdict(lambda: [0, 0.0]),
        "score_start": Counter(),
        "examples": [],
    }


SCORE_EDGES = (-1000, -500, -300, -150, -50, 50, 150, 300, 500, 1000)


def score_bucket(cp: int) -> str:
    for edge in SCORE_EDGES:
        if cp < edge:
            return f"<{edge}"
    return f">={SCORE_EDGES[-1]}"


def analyse_file(path_str: str) -> dict:
    path = Path(path_str)
    st = new_stats()
    shard = path.parent.name
    for headers, movetext in parse_games(path):
        where = f"{shard} round {headers.get('Round', '?')}"
        moves, saw_result = split_movetext(movetext, where)
        st["comment_count"] += sum(1 for _san, c in moves if c is not None)
        if not saw_result or headers.get("Result", "*") == "*":
            st["games_skipped"] += 1
            continue
        try:
            analyse_game(headers, moves, st, where)
        except ParseError:
            raise
        except ValueError as exc:  # illegal SAN etc. -- truncated/corrupt game
            st["games_skipped"] += 1
            print(f"warning: {where}: {exc}", file=sys.stderr)
    return finalise(st)


def analyse_game(headers, moves, st, where):
    fen = headers.get("FEN")
    board = chess.Board(fen) if fen else chess.Board()
    builds = {chess.WHITE: headers.get("White", "?"), chess.BLACK: headers.get("Black", "?")}
    result = headers.get("Result")
    st["games"] += 1
    st["results"][result] += 1
    st["terminations"][headers.get("Termination", "?")] += 1

    base, inc = time_control(headers)
    remaining = {chess.WHITE: base, chess.BLACK: base}

    recs = []  # per ply: dict
    reached: dict = {}   # material class -> (stronger colour, score reported there)
    for idx, (san, comment) in enumerate(moves):
        mover = board.turn
        phase = board_phase(board)
        cp = mate = depth = secs = note = None
        if comment is not None:
            cp, mate, depth, secs, note = parse_comment(comment, f"{where} move {idx}")
        if note == "book":
            st["book_moves"] += 1
        signed = cp if cp is not None else (
            (100000 if mate > 0 else -100000) if mate is not None else None)
        if signed is not None:
            for cls, color in material_classes(board):
                if cls in reached:
                    continue
                if cls == "OCB":
                    # Symmetric class: the score decides which side is stronger.
                    white_view = signed if mover == chess.WHITE else -signed
                    reached[cls] = ((chess.WHITE if white_view >= 0 else chess.BLACK),
                                    abs(white_view))
                elif color == mover:
                    reached[cls] = (color, signed)
        try:
            move = board.parse_san(san)
        except Exception as exc:
            raise ValueError(f"illegal SAN {san!r} at ply {idx}: {exc}") from exc
        gives_check = board.gives_check(move)
        is_capture = board.is_capture(move)
        piece = board.piece_type_at(move.from_square)
        pawn_rich = (len(board.pieces(chess.PAWN, chess.WHITE)) >= PAWN_RICH
                     and len(board.pieces(chess.PAWN, chess.BLACK)) >= PAWN_RICH)
        board.push(move)
        band = None
        if secs is not None and base:
            remaining[mover] = remaining[mover] - secs + inc
            band = clock_band(remaining[mover])
        recs.append(
            dict(ply=idx, mover=mover, build=builds[mover], phase=phase,
                 bucket=phase_bucket(phase), cp=cp, mate=mate, depth=depth,
                 secs=secs, note=note, band=band, san=san, check=gives_check,
                 capture=is_capture, piece=piece, pawn_rich=pawn_rich)
        )

    # The final position is reached by the last push, so the loop above never
    # classified it -- and that is exactly where KminorK lives, since fastchess
    # ends the game the moment the material becomes insufficient.
    last_seen: dict = {}
    for r in recs:
        if r["cp"] is not None:
            last_seen[r["mover"]] = r["cp"]
        elif r["mate"] is not None:
            last_seen[r["mover"]] = 100000 if r["mate"] > 0 else -100000
    for cls, color in material_classes(board):
        if cls in reached:
            continue
        if cls == "OCB":
            view = last_seen.get(chess.WHITE)
            if view is not None:
                reached[cls] = ((chess.WHITE if view >= 0 else chess.BLACK), abs(view))
        elif color in last_seen:
            reached[cls] = (color, last_seen[color])

    total_plies = len(moves)
    st["plies"][min(total_plies // 20 * 20, 260)] += 1
    if total_plies > 200:
        st["long_games"] += 1

    final_note = recs[-1]["note"] if recs else None
    if result == "1/2-1/2":
        st["draw_reasons"][final_note or "unknown"] += 1
    else:
        st["win_reasons"][final_note or "unknown"] += 1

    # Outcome from each mover's perspective.
    def outcome_for(color):
        if result == "1/2-1/2":
            return "draw"
        won = (result == "1-0") == (color == chess.WHITE)
        return "win" if won else "loss"

    peak = {chess.WHITE: None, chess.BLACK: None}
    for i, r in enumerate(recs):
        if r["cp"] is None and r["mate"] is None:
            continue
        st["moves"] += 1
        if r["mate"] is not None:
            st["mate_scores"] += 1
        if r["depth"] is not None:
            d = st["depth_by_phase"][r["bucket"]]
            d[0] += 1
            d[1] += r["depth"]
        if r["secs"] is not None:
            t = st["time_by_phase"][r["bucket"]]
            t[0] += 1
            t[1] += r["secs"]
        if r["cp"] is not None:
            st["calib"][(r["bucket"], score_bucket(r["cp"]), outcome_for(r["mover"]))] += 1
            fine = max(-600, min(600, int(round(r["cp"] / 25.0)) * 25))
            st["calib_fine"][(r["bucket"], fine, outcome_for(r["mover"]))] += 1
            if 0 <= r["cp"] < 600:
                st["calib_pawns"][(r["bucket"], "pawn-rich" if r["pawn_rich"] else "pawn-poor",
                                   r["cp"] // 50 * 50, outcome_for(r["mover"]))] += 1
            prev = peak[r["mover"]]
            if prev is None or r["cp"] > prev[0]:
                peak[r["mover"]] = (r["cp"], r["bucket"])
        elif r["mate"] is not None and r["mate"] > 0:
            peak[r["mover"]] = (100000, r["bucket"])

        # Cross-build gap: this build's score plus the opponent's score for the
        # position it just handed over. Zero means the two builds agree.
        nxt = recs[i + 1] if i + 1 < len(recs) else None
        if nxt is not None and r["cp"] is not None and nxt["cp"] is not None:
            g = st["gap"][(r["build"], r["bucket"])]
            g[0] += 1
            g[1] += abs(r["cp"] + nxt["cp"])

        # Self-swing: the build's own admission that the position got worse.
        nn = recs[i + 2] if i + 2 < len(recs) else None
        if nn is None:
            continue
        s = st["swing"][(r["build"], r["bucket"])]
        if r["mate"] is not None and r["mate"] > 0 and not (nn["mate"] is not None and nn["mate"] > 0):
            s[3] += 1  # announced mate, then did not still see one
        if r["cp"] is None or nn["cp"] is None:
            continue
        swing = r["cp"] - nn["cp"]
        s[0] += 1
        s[4] += abs(swing)
        byres = st["swing_by_result"][outcome_for(r["mover"])]
        byres[0] += 1
        byres[1] += swing
        contested = abs(r["cp"]) <= CONTESTED_CP
        if contested:
            if r["piece"]:
                st["piece_moves"][chess.piece_name(r["piece"])] += 1
            c = st["swing_contested"][(r["build"], r["bucket"])]
            c[0] += 1
            c[4] += abs(swing)
            if swing >= BLUNDER_CP:
                c[1] += 1
                if r["piece"]:
                    st["piece_blunders"][chess.piece_name(r["piece"])] += 1
            if swing >= BIG_BLUNDER_CP:
                c[2] += 1
            if r["band"]:
                p = st["phase_clock"][(r["bucket"], r["band"])]
                p[0] += 1
                p[2] += abs(swing)
                if swing >= BLUNDER_CP:
                    p[1] += 1
        if swing >= BLUNDER_CP:
            s[1] += 1
            st["blunder_ctx"][("phase", r["bucket"])] += 1
            st["blunder_ctx"][("piece", chess.piece_name(r["piece"]))] += 1
            st["blunder_ctx"][("kind", "capture" if r["capture"] else "quiet")] += 1
            st["blunder_ctx"][("reply_capture", str(nxt["capture"]) if nxt else "?")] += 1
            st["blunder_ctx"][("reply_check", str(nxt["check"]) if nxt else "?")] += 1
            st["blunder_ctx"][("band", r["band"] or "?")] += 1
            st["blunder_ctx"][("depth", str(min(r["depth"] or 0, 20) // 4 * 4))] += 1
            if len(st["examples"]) < 40 and swing >= BIG_BLUNDER_CP:
                st["examples"].append(
                    dict(where=where, ply=r["ply"], san=r["san"], before=r["cp"],
                         after=nn["cp"], bucket=r["bucket"], build=r["build"])
                )
        if swing >= BIG_BLUNDER_CP:
            s[2] += 1
        if r["band"]:
            c = st["swing_clock"][(r["build"], r["band"])]
            c[0] += 1
            if swing >= BLUNDER_CP:
                c[1] += 1
            c[2] += abs(swing)

    for color, pk in peak.items():
        if pk is None or pk[0] < ADVANTAGE_CP:
            continue
        st["squander_games"] += 1
        st["squander"][(score_bucket(min(pk[0], 99999)), pk[1], outcome_for(color))] += 1

    for cls, (color, cp) in reached.items():
        bucket = ">=250" if cp >= 250 else (">=100" if cp >= 100 else "<100")
        st["material_class"][(cls, bucket, outcome_for(color))] += 1

    # A build that announced forced mate and then did not win the game.
    for color in (chess.WHITE, chess.BLACK):
        if any(r["mover"] == color and r["mate"] is not None and r["mate"] > 0 for r in recs):
            st["mate_claims"][(builds[color], outcome_for(color))] += 1

    # For drawn games: how big an edge did the better-placed side report?
    if result == "1/2-1/2":
        best = max((pk[0] for pk in peak.values() if pk is not None), default=None)
        if best is not None:
            st["draw_peak"][(final_note or "unknown", score_bucket(min(best, 99999)))] += 1

    # The LAST score each side reported, which is what it still believed at the
    # moment the game ended. Unlike the peak this cannot be a transient spike:
    # a side claiming +2 on its final move of a drawn game misjudged the very
    # position it was standing in.
    last = {}
    for r in recs:
        if r["cp"] is not None:
            last[r["mover"]] = r["cp"]
        elif r["mate"] is not None:
            last[r["mover"]] = 100000 if r["mate"] > 0 else -100000
    if last:
        best_final = max(last.values())
        st["final_score"][(final_note or "unknown", score_bucket(min(best_final, 99999)))] += 1

    if recs and recs[0]["cp"] is not None:
        st["score_start"][score_bucket(recs[0]["cp"])] += 1


def finalise(st: dict) -> dict:
    """Make the stats JSON-serialisable (tuple keys -> joined strings)."""
    out = {}
    for k, v in st.items():
        if isinstance(v, Counter):
            out[k] = {"|".join(map(str, key)) if isinstance(key, tuple) else str(key): n
                      for key, n in v.items()}
        elif isinstance(v, defaultdict):
            out[k] = {("|".join(map(str, key)) if isinstance(key, tuple) else str(key)): list(val)
                      for key, val in v.items()}
        else:
            out[k] = v
    return out


def merge(a: dict, b: dict) -> dict:
    for k, v in b.items():
        if isinstance(v, int):
            a[k] = a.get(k, 0) + v
        elif isinstance(v, list):
            a.setdefault(k, []).extend(v)
        elif isinstance(v, dict):
            dst = a.setdefault(k, {})
            for key, val in v.items():
                if isinstance(val, list):
                    cur = dst.get(key)
                    dst[key] = val[:] if cur is None else [x + y for x, y in zip(cur, val)]
                else:
                    dst[key] = dst.get(key, 0) + val
    return a


def pct(n, d):
    return f"{100.0 * n / d:5.1f}%" if d else "    -"


def report(st: dict, out=sys.stdout) -> None:
    w = out.write
    w(f"Games parsed        : {st['games']}\n")
    w(f"Games skipped       : {st['games_skipped']}\n")
    w(f"Annotated moves     : {st['moves']}\n")
    w(f"Mate scores         : {st['mate_scores']}\n")
    w(f"Book moves          : {st['book_moves']}\n\n")

    w("Results / terminations\n")
    for key in ("results", "terminations"):
        for k, n in sorted(st[key].items(), key=lambda kv: -kv[1]):
            w(f"  {key[:4]:5s} {k:36s} {n:7d}  {pct(n, st['games'])}\n")
    w("\nDraw reasons (of drawn games)\n")
    draws = sum(st["draw_reasons"].values())
    for k, n in sorted(st["draw_reasons"].items(), key=lambda kv: -kv[1]):
        w(f"  {k:44s} {n:7d}  {pct(n, draws)} of draws, {pct(n, st['games'])} of games\n")
    w("\nDecisive-game reasons\n")
    wins = sum(st["win_reasons"].values())
    for k, n in sorted(st["win_reasons"].items(), key=lambda kv: -kv[1]):
        w(f"  {k:44s} {n:7d}  {pct(n, wins)}\n")
    w(f"\nGames over 200 plies: {st['long_games']}  ({pct(st['long_games'], st['games'])})\n")
    w("Game-length histogram (plies from the book position)\n")
    for k in sorted(st["plies"], key=lambda x: int(x)):
        w(f"  {int(k):3d}+ {st['plies'][k]:7d}  {pct(st['plies'][k], st['games'])}\n")

    w("\nSelf-swing (s(t) - s(t+2), positive = own score got worse)\n")
    w(f"  {'build':22s} {'phase':12s} {'moves':>9s} {'mean|swing|':>11s} "
      f"{'>=150cp':>9s} {'rate':>7s} {'>=300cp':>9s} {'rate':>7s} {'mateflip':>8s}\n")
    for key in sorted(st["swing"]):
        n, bl, big, mf, sa = st["swing"][key]
        build, phase = key.split("|")
        w(f"  {build:22s} {phase:12s} {n:9d} {sa / n if n else 0:11.1f} "
          f"{bl:9d} {pct(bl, n)} {big:9d} {pct(big, n)} {mf:8d}\n")

    w(f"\nSelf-swing in CONTESTED positions only (|score before the move| <= {CONTESTED_CP}cp)\n")
    w(f"  {'build':22s} {'phase':12s} {'moves':>9s} {'mean|swing|':>11s} "
      f"{'>=150cp':>9s} {'rate':>7s} {'>=300cp':>9s} {'rate':>7s}\n")
    for key in sorted(st["swing_contested"]):
        n, bl, big, _mf, sa = st["swing_contested"][key]
        build, phase = key.split("|")
        w(f"  {build:22s} {phase:12s} {n:9d} {sa / n if n else 0:11.1f} "
          f"{bl:9d} {pct(bl, n)} {big:9d} {pct(big, n)}\n")

    w("\nContested blunder rate by phase x remaining clock (both builds pooled)\n")
    for key in sorted(st["phase_clock"]):
        n, bl, sa = st["phase_clock"][key]
        phase, band = key.split("|")
        w(f"  {phase:12s} {band:12s} {n:9d} mean|swing| {sa / n if n else 0:6.1f} "
          f"blunders {bl:7d} {pct(bl, n)}\n")

    w("\nContested blunder rate by moved piece (rate, not share)\n")
    for p, n in sorted(st["piece_moves"].items(), key=lambda kv: -kv[1]):
        b = st["piece_blunders"].get(p, 0)
        w(f"  {p:8s} moves {n:9d}  blunders {b:7d}  {pct(b, n)}\n")

    w("\nSelf-swing by remaining clock\n")
    for key in sorted(st["swing_clock"]):
        n, bl, sa = st["swing_clock"][key]
        build, band = key.split("|")
        w(f"  {build:22s} {band:12s} {n:9d} mean|swing| {sa / n if n else 0:7.1f} "
          f"blunders {bl:7d} {pct(bl, n)}\n")

    w("\nCross-build disagreement (|s_X(t) + s_Y(t+1)|)\n")
    for key in sorted(st["gap"]):
        n, s = st["gap"][key]
        build, phase = key.split("|")
        w(f"  {build:22s} {phase:12s} {n:9d} mean {s / n if n else 0:7.1f} cp\n")

    w("\nSearch depth / time by phase\n")
    for k in sorted(st["depth_by_phase"]):
        n, d = st["depth_by_phase"][k]
        tn, ts = st["time_by_phase"].get(k, [0, 0.0])
        w(f"  {k:12s} moves {n:9d}  mean depth {d / n if n else 0:5.2f}  "
          f"mean time {ts / tn if tn else 0:6.3f}s\n")

    w("\nBlunder context (self-swing >= 150cp)\n")
    groups = defaultdict(list)
    for key, n in st["blunder_ctx"].items():
        g, v = key.split("|", 1)
        groups[g].append((v, n))
    for g in sorted(groups):
        tot = sum(n for _, n in groups[g])
        w(f"  {g}\n")
        for v, n in sorted(groups[g], key=lambda kv: -kv[1]):
            w(f"    {v:20s} {n:8d}  {pct(n, tot)}\n")

    w("\nEval calibration: outcome for the side to move, by reported score\n")
    calib = defaultdict(lambda: Counter())
    for key, n in st["calib"].items():
        phase, bucket, outcome = key.split("|")
        calib[(phase, bucket)][outcome] += n
    order = [f"<{e}" for e in SCORE_EDGES] + [f">={SCORE_EDGES[-1]}"]
    for phase in ("opening", "middlegame", "endgame"):
        w(f"  {phase}\n")
        for bucket in order:
            c = calib.get((phase, bucket))
            if not c:
                continue
            tot = sum(c.values())
            score = (c["win"] + 0.5 * c["draw"]) / tot
            w(f"    score {bucket:8s} n={tot:8d}  W {pct(c['win'], tot)} "
              f"D {pct(c['draw'], tot)} L {pct(c['loss'], tot)}  observed {score:.3f}\n")

    w("\nObserved score for the side to move, by reported score (25cp steps)\n")
    fine = defaultdict(lambda: Counter())
    for key, n in st["calib_fine"].items():
        phase, cp, outcome = key.split("|")
        fine[(phase, int(cp))][outcome] += n
    cps = sorted({k[1] for k in fine})
    w(f"  {'cp':>6s} " + "".join(f"{p:>26s}" for p in ("opening", "middlegame", "endgame")) + "\n")
    for cp in cps:
        if cp < 0:
            continue
        row = f"  {cp:6d} "
        for phase in ("opening", "middlegame", "endgame"):
            c = fine.get((phase, cp))
            if not c:
                row += f"{'-':>26s}"
                continue
            tot = sum(c.values())
            row += f"{(c['win'] + 0.5 * c['draw']) / tot:>14.3f} (n={tot:7d})"
        w(row + "\n")

    w(f"\nSame, split on pawn count (pawn-rich = both sides hold >= {PAWN_RICH} pawns)\n")
    w("  This is the control: if the endgame column is only worse in the pawn-poor\n"
      "  half, the eval is not phase-miscalibrated, it is blind to drawish material.\n")
    pc = defaultdict(lambda: Counter())
    for key, n in st["calib_pawns"].items():
        phase, rich, cp, outcome = key.split("|")
        pc[(phase, rich, int(cp))][outcome] += n
    for rich in ("pawn-rich", "pawn-poor"):
        w(f"  {rich}\n")
        w(f"    {'cp':>6s}" + "".join(f"{p:>24s}" for p in
                                      ("opening", "middlegame", "endgame")) + "\n")
        for cp in range(0, 600, 50):
            row = f"    {cp:6d}"
            for phase in ("opening", "middlegame", "endgame"):
                c = pc.get((phase, rich, cp))
                tot = sum(c.values()) if c else 0
                if tot < 200:
                    row += f"{'-':>24s}"
                else:
                    row += f"{(c['win'] + 0.5 * c['draw']) / tot:>12.3f} (n={tot:6d})"
            w(row + "\n")

    w("\nDrawish material classes, counted once per game at the position that reached them\n")
    mc = defaultdict(lambda: Counter())
    for key, n in st["material_class"].items():
        cls, bucket, outcome = key.split("|")
        mc[(cls, bucket)][outcome] += n
    w(f"  {'class':10s} {'stronger side reported':24s} {'games':>7s} {'of all':>7s} "
      f"{'observed':>9s}   W/D/L\n")
    for (cls, bucket) in sorted(mc):
        c = mc[(cls, bucket)]
        tot = sum(c.values())
        w(f"  {cls:10s} {bucket:24s} {tot:7d} {pct(tot, st['games'])} "
          f"{(c['win'] + 0.5 * c['draw']) / tot:9.3f}   {c['win']}/{c['draw']}/{c['loss']}\n")

    w("\nSquandered advantages: peak reported score of a side vs its result\n")
    sq = defaultdict(lambda: Counter())
    for key, n in st["squander"].items():
        bucket, phase, outcome = key.split("|")
        sq[(bucket, phase)][outcome] += n
    for (bucket, phase) in sorted(sq, key=lambda k: (order.index(k[0]) if k[0] in order else 99, k[1])):
        c = sq[(bucket, phase)]
        tot = sum(c.values())
        w(f"  peak {bucket:8s} ({phase:11s}) n={tot:7d}  won {pct(c['win'], tot)} "
          f"drew {pct(c['draw'], tot)} lost {pct(c['loss'], tot)}\n")

    w("\nGames where a build announced forced mate, by that build's result\n")
    claims = defaultdict(lambda: Counter())
    for key, n in st["mate_claims"].items():
        build, outcome = key.split("|")
        claims[build][outcome] += n
    for build, c in sorted(claims.items()):
        tot = sum(c.values())
        w(f"  {build:22s} n={tot:6d}  won {pct(c['win'], tot)} drew {pct(c['draw'], tot)} "
          f"lost {pct(c['loss'], tot)}\n")

    w("\nDrawn games by the best peak score either side reported\n")
    dp = defaultdict(lambda: Counter())
    for key, n in st["draw_peak"].items():
        reason, bucket = key.split("|")
        dp[reason][bucket] += n
    for reason, c in sorted(dp.items(), key=lambda kv: -sum(kv[1].values())):
        tot = sum(c.values())
        ge = sum(n for b, n in c.items() if b in (">=1000", "<1000", "<500", "<300"))
        w(f"  {reason:40s} n={tot:6d}  peak >=150cp for one side: {ge:6d} {pct(ge, tot)}\n")

    w("\nHow the game ended vs the LAST score the better-placed side reported\n")
    fs = defaultdict(lambda: Counter())
    for key, n in st["final_score"].items():
        reason, bucket = key.split("|")
        fs[reason][bucket] += n
    for reason, c in sorted(fs.items(), key=lambda kv: -sum(kv[1].values())):
        tot = sum(c.values())
        w(f"  {reason:40s} n={tot:6d}\n")
        for bucket in order:
            if c.get(bucket):
                w(f"      last score {bucket:8s} {c[bucket]:6d}  {pct(c[bucket], tot)}\n")

    if st["examples"]:
        w("\nSample large swings (>=300cp)\n")
        for e in st["examples"][:20]:
            w(f"  {e['where']:28s} ply {e['ply']:3d} {e['san']:8s} "
              f"{e['before']:+6d} -> {e['after']:+6d} ({e['bucket']}, {e['build']})\n")


def self_check(st: dict, files: list[str], out=sys.stdout) -> bool:
    """Assert the invariants that would otherwise fail silently."""
    ok = True

    def check(name, passed, detail):
        nonlocal ok
        ok = ok and passed
        out.write(f"  [{'PASS' if passed else 'FAIL'}] {name}: {detail}\n")

    events = 0
    comments = 0
    for f in files:
        text = Path(f).read_text(encoding="utf-8", errors="replace")
        events += text.count("[Event ")
        comments += text.count("{")
    check("game count", st["games"] + st["games_skipped"] == events,
          f"parsed {st['games']} + skipped {st['games_skipped']} vs {events} [Event ] tags")
    check("comment count", st["comment_count"] == comments,
          f"tokenised {st['comment_count']} vs {comments} '{{' in the input")

    # Scores are mover-relative (D2). Under a White-relative reading every
    # self-swing would be inverted for one side, and this check would fail.
    lost = st["swing_by_result"].get("loss", [0, 0.0])
    won = st["swing_by_result"].get("win", [0, 0.0])
    lmean = lost[1] / lost[0] if lost[0] else 0.0
    wmean = won[1] / won[0] if won[0] else 0.0
    check("score perspective", lmean > 0 > wmean,
          f"mean signed self-swing: losing side {lmean:+.2f} cp, winning side {wmean:+.2f} cp")
    check("mate scores excluded from means",
          st["mate_scores"] > 0 and st["moves"] > st["mate_scores"],
          f"{st['mate_scores']} mate annotations counted separately")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="directory holding strength-<run>-shard-*/match.pgn")
    ap.add_argument("--json", help="also write the merged raw stats here")
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 4, 18))
    ap.add_argument("--self-check", action="store_true",
                    help="verify the parser invariants and exit non-zero if any fails")
    args = ap.parse_args()

    files = sorted(str(p) for p in Path(args.root).rglob("*.pgn"))
    if not files:
        print(f"no .pgn under {args.root}", file=sys.stderr)
        return 2
    print(f"scanning {len(files)} PGN file(s) with {args.jobs} worker(s)", file=sys.stderr)

    merged: dict = {}
    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for part in ex.map(analyse_file, files):
            merge(merged, part)

    if args.self_check:
        print("self-check")
        if not self_check(merged, files):
            return 1
        return 0

    report(merged)
    if args.json:
        Path(args.json).write_text(json.dumps(merged, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
