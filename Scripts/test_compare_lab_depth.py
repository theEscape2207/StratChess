"""Fixture tests for compare_lab_depth.py.

The property the comparison rests on is that each depth is credited to the right engine and colour:
a Black-to-move opening puts Black's comment first, and a swap there would compare the candidate
with itself. The history and EBF helpers are checked on the same fixture.
"""

from __future__ import annotations

import contextlib
import io
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_lab_depth as cld  # noqa: E402

WHITE_FEN = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3"
BLACK_FEN = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"


def game(fen: str, white: str, black: str, moves: str) -> str:
    return f'[Event "fixture"]\n[White "{white}"]\n[Black "{black}"]\n[FEN "{fen}"]\n[Result "1-0"]\n\n{moves} 1-0\n\n'


# Candidate always reaches depth 12, reference depth 10, whichever colour it plays.
PGN = (
    game(WHITE_FEN, "candidate-x", "reference-y", "3. Bb5 {+0.30/12 0.5s} a6 {-0.30/10 0.5s} 4. Ba4 {+0.31/12 0.5s, White wins by adjudication}")
    + game(WHITE_FEN, "reference-y", "candidate-x", "3. Bb5 {+0.30/10 0.5s} a6 {-0.30/12 0.5s} 4. Ba4 {+M1/10 0.5s, White mates}")
    + game(BLACK_FEN, "candidate-x", "reference-y", "1... e5 {-0.10/10 0.5s} 2. Nf3 {+0.10/12 0.5s}")
    + game(BLACK_FEN, "reference-y", "candidate-x", "1... e5 {-0.10/12 0.5s} 2. Nf3 {+0.10/10 0.5s}")
)


class CompareLabDepthTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        shard = self.root / "run-1" / "strength-1-shard-0"
        shard.mkdir(parents=True)
        (shard / "match.pgn").write_text(PGN, encoding="utf-8")
        self.games = cld.read_games(self.root / "run-1")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_reads_every_game_and_the_comment_carrying_the_end(self) -> None:
        self.assertEqual(len(self.games), 4)
        self.assertEqual(self.games[1].depths, [10, 12, 10])
        self.assertEqual(self.games[1].scores, [0.30, -0.30, cld.MATE_SCORE])
        self.assertEqual([g.end for g in self.games], ["wins by adjudication", "mates", "?", "?"])

    def test_black_to_move_credits_first_comment_to_black(self) -> None:
        headers, depths = self.games[2].headers, self.games[2].depths
        self.assertEqual(cld.own_moves(headers, depths), {"b": [10], "w": [12]})

    def test_every_delta_is_candidate_minus_reference(self) -> None:
        buckets, pair_means = cld.compare(self.games)
        self.assertEqual(pair_means, [2.0, 2.0])
        self.assertTrue(all(delta == 2 for delta in buckets[0]))

    def test_summary_splits_decisive_games_at_the_deciding_score(self) -> None:
        summary = cld.summarise(self.games)
        # Only game 2 reaches |2.00|: the mate at index 2, with 1 of its 3 plies after it.
        self.assertEqual((summary.to_decided, summary.after_decided), (2.0, 1.0))
        # Plies before |1.00|: game 1 never reaches it (3), game 2 at index 2, games 3-4 end at 2.
        self.assertEqual(summary.level, 2.25)
        self.assertEqual(summary.depth, {"candidate-x": 12.0, "reference-y": 10.0})
        self.assertEqual(summary.draw_rate, 0.0)

    def test_lengths_compare_only_openings_every_run_played(self) -> None:
        other = [g for g in self.games if g.headers["FEN"] == BLACK_FEN][:1]
        common, means = cld.common_opening_plies({"a": self.games, "b": other})
        self.assertEqual(common, 1)
        self.assertEqual(means, {"a": 2.0, "b": 2.0})

    def test_history_reads_each_run_under_the_root(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(cld.history(self.root), 0)
            self.assertEqual(cld.history(self.root / "run-1" / "strength-1-shard-0"), 1)
        self.assertIn("openings every run played: 2", out.getvalue())


class EbfTest(unittest.TestCase):
    def test_ratio_per_iteration_from_the_first_lab_depth(self) -> None:
        ratios = cld.ebf_from_nodes([{10: 100, 11: 200, 12: 400}, {11: 50, 13: 150}])
        self.assertEqual(sorted(ratios), [11, 12])
        self.assertTrue(all(math.isclose(r, math.log(2)) for r in ratios[11] + ratios[12]))


if __name__ == "__main__":
    unittest.main()
