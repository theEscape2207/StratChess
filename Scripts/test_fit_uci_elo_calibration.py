"""Fixture tests for fit_uci_elo_calibration.py.

Synthetic attempt directories exercise reading, plan coverage and the bootstrap end to end; the
table rule and the gates are driven directly through evaluate() with seeded synthetic samples, so
each gate's case is placed exactly where the design puts it.
"""

from __future__ import annotations

import contextlib
import io
import json
import math
import random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fit_uci_elo_calibration as fit  # noqa: E402

TABLE = fit.parse_table("1320:0,2600:19")  # the table the 2026-11-21 gauntlet ran under
RESULT = {1.0: "1-0", 0.0: "0-1", 0.5: "1/2-1/2"}


def game_text(rnd: int, strat_white: bool, strat_score: float, sf: int, depth: int = 1, comment_less: bool = False) -> str:
    white, black = ("StratChess", f"SF-{sf}") if strat_white else (f"SF-{sf}", "StratChess")
    white_score = strat_score if strat_white else 1.0 - strat_score
    strat_c = "" if comment_less else f" {{+0.30/{depth} 0.100s}}"
    sf_c = " {-0.20/14 0.500s}"
    w_c, b_c = (strat_c, sf_c) if strat_white else (sf_c, strat_c)
    return (
        f'[Event "fixture"]\n[Round "{rnd}"]\n[White "{white}"]\n[Black "{black}"]\n'
        f'[Result "{RESULT[white_score]}"]\n[GameDuration "00:01:30"]\n\n'
        f"1. e4 {{book}} e5 {{book}} 2. Nf3{w_c} Nc6{b_c} 3. Bb5{w_c} a6 {{-0.10/15 0.4s, adjudication}} {RESULT[white_score]}\n\n"
    )


def write_attempt(
    root: Path,
    name: str,
    uci: int,
    sf: int,
    stage: int,
    pairs: list[tuple[float, float]],
    opening_start: int = 1,
    smoke: bool = False,
    candidate: str = "aaa",
    with_run: bool = True,
    pgn_extra: str = "",
) -> Path:
    """pairs: StratChess's score with White, then with Black, per opening from opening_start."""
    path = root / name
    path.mkdir(parents=True)
    text = ""
    for i, (as_white, as_black) in enumerate(pairs, start=1):
        text += game_text(i, True, as_white, sf) + game_text(i, False, as_black, sf)
    (path / "games.pgn").write_text(text + pgn_extra, encoding="utf-8")
    (path / "match.log").write_text("", encoding="utf-8")
    if with_run:
        run = {
            "complete": True, "smoke": smoke, "stage": stage, "uciElo": uci, "sfElo": sf,
            "games": 2 * len(pairs) + pgn_extra.count("[Event"), "openingStart": opening_start,
            "openingEnd": opening_start + len(pairs) - 1, "tc": "60+0.6", "concurrency": 8,
            "machine": "fixture", "wallSeconds": 100.0 * len(pairs),
            "candidate": {"commit": "c0ffee", "sha256": candidate},
            "stockfish": {"sha256": "sf"}, "book": {"sha256": "book"},
        }
        (path / "run.json").write_text(json.dumps(run), encoding="utf-8")
    return path


def outcomes(p: float, n: int, rng: random.Random) -> list[tuple[float, float]]:
    return [(float(rng.random() < p), float(rng.random() < p)) for _ in range(n)]


def quiet(fn, *args):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return fn(*args)


def normal_samples(points: list[float], sds: list[float], n: int = 4000, seed: int = 1) -> list[list[float]]:
    rng = random.Random(seed)
    return [[p + s * rng.gauss(0, 1) for p, s in zip(points, sds)] for _ in range(n)]


TARGETS = [0, 2, 4, 7, 10, 14, 19]
PLAYED = [fit.level_for_elo(TABLE, fit.uci_elo_for_level(TABLE, L)) for L in TARGETS]


def run_eval(
    points: list[float], sds: list[float], level0_score: float = 0.5, clamped: list[bool] | None = None, accept_floor: bool = False
) -> fit.Fit:
    return fit.evaluate(TARGETS, PLAYED, points, normal_samples(points, sds), level0_score, 19.0, clamped, accept_floor)


class TableTest(unittest.TestCase):
    def test_design_d3_table(self) -> None:
        expect = {0: 1320, 2: 1455, 4: 1590, 7: 1792, 10: 1994, 14: 2264, 19: 2600}
        self.assertEqual({L: fit.uci_elo_for_level(TABLE, L) for L in expect}, expect)
        self.assertEqual([fit.pick_depth(p) for p in PLAYED], [1, 3, 5, 8, 11, 15, 20])
        self.assertAlmostEqual(PLAYED[5], 14.0125, places=4)


class ReadTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def collect(self, attempts: list[Path], stages: dict[int, list[int]], levels: list[int] = [0]) -> list[fit.Level]:
        return fit.collect([fit.read_attempt(a) for a in attempts], TABLE, levels, stages)

    def test_comment_less_move_is_counted_and_attributed_by_side(self) -> None:
        text = game_text(1, False, 0.5, 1320, comment_less=True)
        path = self.root / "a"
        path.mkdir()
        (path / "games.pgn").write_text(text, encoding="utf-8")
        headers, body = fit.read_pgn(path / "games.pgn")[0]
        moves = fit.side_moves(headers, body)
        # StratChess is Black: its one move (Nc6) lacks a comment; White's two keep theirs.
        self.assertEqual(moves["b"], [None, "-0.10/15 0.4s, adjudication"])
        self.assertEqual(moves["w"], ["-0.20/14 0.500s", "-0.20/14 0.500s"])
        black = fit.tally(moves["b"])
        self.assertEqual((black.missing, black.depths), (1, [15]))

    def test_black_first_fen_starts_with_black(self) -> None:
        headers = {"FEN": "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"}
        moves = fit.side_moves(headers, "1... e5 {+0.1/7 0.1s} 2. Nf3 {+0.2/9 0.1s} *")
        self.assertEqual(moves, {"b": ["+0.1/7 0.1s"], "w": ["+0.2/9 0.1s"]})

    def test_missing_run_json_refused(self) -> None:
        a = write_attempt(self.root, "a", 1320, 1320, 1, [(1, 0)], with_run=False)
        with self.assertRaisesRegex(fit.InputError, "no run.json"):
            fit.read_attempt(a)

    def test_smoke_refused(self) -> None:
        a = write_attempt(self.root, "a", 1320, 1320, 1, [(1, 0)], smoke=True)
        with self.assertRaisesRegex(fit.InputError, "smoke"):
            fit.read_attempt(a)

    def test_mixed_candidate_refused(self) -> None:
        a = write_attempt(self.root, "a", 1320, 1320, 1, [(1, 0)])
        b = write_attempt(self.root, "b", 1320, 1320, 2, [(1, 0)], opening_start=2, candidate="bbb")
        with self.assertRaisesRegex(fit.InputError, "candidate binar"):
            self.collect([a, b], {1: [1], 2: [2]})

    def test_duplicate_pair_refused(self) -> None:
        a = write_attempt(self.root, "a", 1320, 1320, 1, [(1, 0)])
        b = write_attempt(self.root, "b", 1320, 1320, 1, [(1, 0)])
        with self.assertRaisesRegex(fit.InputError, "duplicate"):
            self.collect([a, b], {1: [1]})

    def test_missing_pair_refused(self) -> None:
        a = write_attempt(self.root, "a", 1320, 1320, 1, [(1, 0)])
        with self.assertRaisesRegex(fit.InputError, "missing"):
            self.collect([a], {1: [1, 2]})

    def test_one_coloured_pair_refused(self) -> None:
        a = write_attempt(self.root, "a", 1320, 1320, 1, [])
        (a / "games.pgn").write_text(game_text(1, True, 1.0, 1320) + game_text(1, True, 0.0, 1320), encoding="utf-8")
        run = json.loads((a / "run.json").read_text())
        run.update(games=2, openingEnd=1)
        (a / "run.json").write_text(json.dumps(run))
        with self.assertRaisesRegex(fit.InputError, "one colour"):
            self.collect([a], {1: [1]})


class BootstrapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def level(self, name: str, sf1: int, sf2: int, p1: list, p2: list) -> fit.Level:
        a = write_attempt(self.root, f"{name}1", 1320, sf1, 1, p1)
        b = write_attempt(self.root, f"{name}2", 1320, sf2, 2, p2, opening_start=len(p1) + 1)
        stages = {1: list(range(1, len(p1) + 1)), 2: list(range(len(p1) + 1, len(p1) + len(p2) + 1))}
        return fit.collect([fit.read_attempt(a), fit.read_attempt(b)], TABLE, [0], stages)[0]

    def estimate(self, level: fit.Level) -> tuple[float, tuple[float, float]]:
        samples = fit.bootstrap([level], 2000, 7)
        return fit.mle(fit.group(level.games)), fit.interval([row[0] for row in samples])

    def test_recovers_known_elo_inside_interval(self) -> None:
        rng = random.Random(3)
        truth = 1500.0
        p1 = outcomes(fit.expected(truth, 1320), 30, rng)
        p2 = outcomes(fit.expected(truth, 1500), 120, rng)
        r, (lo, hi) = self.estimate(self.level("k", 1320, 1500, p1, p2))
        self.assertLess(lo, truth)
        self.assertGreater(hi, truth)
        self.assertLess(abs(r - truth), 60)

    def test_correlated_pairs_widen_the_interval(self) -> None:
        # Same totals (half the points), but one fixture's pairs are WW/LL, the other's W-L splits.
        correlated = [(1.0, 1.0), (0.0, 0.0)] * 15
        independent = [(1.0, 0.0), (0.0, 1.0)] * 15
        _, (clo, chi) = self.estimate(self.level("c", 1600, 1600, correlated[:10], correlated[10:]))
        _, (ilo, ihi) = self.estimate(self.level("i", 1600, 1600, independent[:10], independent[10:]))
        self.assertGreater(chi - clo, 1.5 * (ihi - ilo))

    def test_clamped_mismatched_opponent_misses_g1(self) -> None:
        # Level 0 scoring ~95% against Stockfish's 1320 floor in both stages (the clamp binds).
        rng = random.Random(5)
        p1, p2 = outcomes(0.95, 30, rng), outcomes(0.95, 120, rng)
        level = self.level("m", 1320, 1320, p1, p2)
        r, (lo, hi) = self.estimate(level)
        self.assertTrue(r - lo > fit.PRECISION or hi - r > fit.PRECISION)


class GateTest(unittest.TestCase):
    POINTS = [1400.0, 1550.0, 1700.0, 1900.0, 2100.0, 2400.0, 2500.0]
    SDS = [20.0] * 7

    def test_clean_fit_passes_and_reads_back(self) -> None:
        result = run_eval(self.POINTS, self.SDS)
        self.assertEqual(result.failures, [])
        self.assertEqual(result.anchors[0], (1400, 0.0))
        self.assertEqual(result.anchors[-1], (2500, 19.0))
        self.assertFalse(result.plateau)

    def test_reversed_top_pair_emits_one_elo_onset(self) -> None:
        points = self.POINTS[:5] + [2420.0, 2400.0]
        result = run_eval(points, self.SDS)
        self.assertEqual(result.failures, [])
        self.assertTrue(result.plateau)
        e = fit.round_half_up(2410.0)
        self.assertEqual(result.anchors[5], (e - 1, PLAYED[5]))
        self.assertEqual(result.anchors[6], (e, 19.0))

    def test_rounded_tie_in_suffix_takes_the_same_path(self) -> None:
        points = self.POINTS[:5] + [2410.2, 2410.4]
        result = run_eval(points, self.SDS)
        self.assertEqual(result.failures, [])
        self.assertTrue(result.plateau)
        self.assertEqual(result.anchors[5:], [(2409, PLAYED[5]), (2410, 19.0)])

    def test_suffix_growing_down_to_10_fails_g2(self) -> None:
        points = self.POINTS[:4] + [2450.0, 2420.0, 2400.0]
        result = run_eval(points, self.SDS)
        self.assertTrue(any(f.startswith("G2/S2") for f in result.failures), result.failures)

    def test_pooled_onset_outside_14s_interval_fails_g3(self) -> None:
        # A barely reversed, very precise pair: pooled E = 2419.5 puts the onset at 2419, just
        # under 14's interval (~[2419.2, 2420.8]), while the difference is not significant.
        points = self.POINTS[:5] + [2420.0, 2419.0]
        sds = self.SDS[:5] + [0.4, 0.4]
        result = run_eval(points, sds)
        self.assertTrue(any(f.startswith("G3 level 14") for f in result.failures), result.failures)
        self.assertFalse(any(f.startswith("G2") for f in result.failures), result.failures)

    def test_level0_under_ten_percent_fails_s1(self) -> None:
        points = [1100.0] + self.POINTS[1:]
        self.assertTrue(any(f.startswith("S1") for f in run_eval(points, self.SDS, level0_score=0.08).failures))
        self.assertFalse(any(f.startswith("S1") for f in run_eval(points, self.SDS, level0_score=0.12).failures))

    def test_accept_floor_waives_s1_with_a_note(self) -> None:
        points = [1100.0] + self.POINTS[1:]
        result = run_eval(points, self.SDS, level0_score=0.08, clamped=[True] + [False] * 6, accept_floor=True)
        self.assertEqual(result.failures, [])
        self.assertTrue(any(n.startswith("S1 level 0") for n in result.notes), result.notes)

    def test_accept_floor_waives_g1_only_on_clamped_levels(self) -> None:
        sds = [40.0, 40.0] + self.SDS[2:3] + [40.0] + self.SDS[4:]
        clamped = [True, True] + [False] * 5
        result = run_eval(self.POINTS, sds, clamped=clamped, accept_floor=True)
        self.assertEqual([f[:10] for f in result.failures], ["G1 level 7"])
        self.assertEqual(sum(n.startswith("G1 level") for n in result.notes), 2, result.notes)
        without = run_eval(self.POINTS, sds, clamped=clamped)
        self.assertEqual(sum(f.startswith("G1") for f in without.failures), 3, without.failures)

    def test_wide_interval_fails_g1(self) -> None:
        sds = self.SDS[:3] + [40.0] + self.SDS[4:]
        self.assertTrue(any(f.startswith("G1 level 7") for f in run_eval(self.POINTS, sds).failures))


if __name__ == "__main__":
    unittest.main()
