"""Fixture tests for measure_eval_error.py (issue #593).

The script's own --self-test covers the parser, the sign conventions and the row round
trip. This covers what it cannot reach without a file on disk: PGN scanning, and
specifically that the scan does NOT inherit Tier 1's contested filter -- the one property
the whole dataset rests on, because a population selected on loss cannot answer whether an
evaluation is wrong on positions nobody flagged.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import measure_eval_error as mee  # noqa: E402

# Two games. The first carries scores well outside CONTESTED_CP on every ply, so Tier 1
# and Tier 2 would drop it entirely; this module must keep every one of its plies. The
# second has a two-ply book prefix, which is what the book-exit rule reads.
PGN = """[Event "fixture"]
[White "cand"]
[Black "ref"]
[Result "1-0"]

1. e4 {+9.00/12 0.1s} e5 {-9.10/12 0.1s} 2. Nf3 {+9.20/12 0.1s} Nc6 {-9.30/12 0.1s} 1-0

[Event "fixture"]
[White "cand"]
[Black "ref"]
[Result "0-1"]

1. d4 {book} d5 {book} 2. c4 {+0.20/12 0.1s} e6 {-0.15/12 0.1s} 0-1
"""


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "match.pgn"
        self.path.write_text(PGN, encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def scan(self):
        return list(mee.scan_positions(self.path))

    def test_every_ply_is_kept_including_uncontested_games(self):
        rows = self.scan()
        self.assertEqual(len(rows), 8, "4 plies per game, both games kept")

    def test_games_tier2_drops_entirely_are_still_present(self):
        # Game 0 is scored 900 cp on every ply, far outside amq.CONTESTED_CP; game 1's
        # only annotated plies have no same-mover successor to swing against. Tier 2's
        # scan yields neither, which is the conditioning this measurement must not inherit.
        import analyze_external_quality as aeq
        self.assertEqual(aeq.scan_games(self.path), [], "Tier 2 keeps neither game")
        self.assertEqual(len({r.game_key for r in self.scan()}), 2)
        self.assertEqual(len(self.scan()), 8)

    def test_first_position_is_the_starting_position(self):
        first = self.scan()[0]
        self.assertTrue(first.fen.startswith("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w"))
        self.assertEqual(first.phase, "opening")

    def test_book_prefix_sets_ply_since_book_exit(self):
        rows = [r for r in self.scan() if r.game_key.endswith(":1")]
        self.assertEqual([r.from_book for r in rows], [True, True, False, False])
        self.assertEqual([r.ply_since_book_exit for r in rows], [0, 0, 0, 1])

    def test_a_game_without_a_result_token_is_skipped(self):
        self.path.write_text(PGN.replace(" 1-0\n", " *\n").replace('"1-0"', '"*"'),
                             encoding="utf-8")
        self.assertEqual(len({r.game_key for r in self.scan()}), 1)


class SampleTest(unittest.TestCase):
    def pool(self, n=40):
        out = []
        for i in range(n):
            for name, _low in mee.amq.PHASE_BUCKETS:
                out.append(mee.Position(fen=f"{name}-{i}", phase=name, game_key=f"g{i % 7}",
                                        ply_index=i, ply_since_book_exit=i, from_book=False))
        return out

    def test_sample_is_reproducible_under_a_seed(self):
        a, _ = mee.stratified_sample(iter(self.pool()), 30, seed=7)
        b, _ = mee.stratified_sample(iter(self.pool()), 30, seed=7)
        self.assertEqual([p.fen for p in a], [p.fen for p in b])

    def test_a_different_seed_draws_a_different_sample(self):
        a, _ = mee.stratified_sample(iter(self.pool()), 30, seed=7)
        b, _ = mee.stratified_sample(iter(self.pool()), 30, seed=8)
        self.assertNotEqual([p.fen for p in a], [p.fen for p in b])

    def test_the_draw_is_uniform_not_the_first_rows_seen(self):
        # Reservoir sampling's failure mode is silently keeping the first k items, which
        # would make the sample the corpus's opening plies rather than a draw from it.
        sampled, _ = mee.stratified_sample(iter(self.pool()), 9, seed=3)
        indices = sorted(p.ply_index for p in sampled if p.phase == "endgame")
        self.assertNotEqual(indices, [0, 1, 2])

    def test_stats_report_the_pool_and_the_duplicates(self):
        pool = self.pool()
        _, stats = mee.stratified_sample(iter(pool + pool), 30, seed=1)
        self.assertEqual(stats["plies_seen"], len(pool) * 2)
        self.assertEqual(stats["duplicates_dropped"], len(pool))
        self.assertEqual(stats["unique_fens"], len(pool))
        self.assertEqual(set(stats["phase_pool"].values()), {40})

    def test_accepts_a_generator_without_materialising_it(self):
        sampled, stats = mee.stratified_sample((p for p in self.pool()), 30, seed=1)
        self.assertEqual(len(sampled), 30)
        self.assertEqual(stats["sampled"], 30)



if __name__ == "__main__":
    unittest.main()
