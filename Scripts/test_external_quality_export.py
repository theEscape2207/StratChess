"""Unit tests for external_quality_export.py (no engine, no network).

Exercises the fixture table pinned by the Tier 2 blunder evidence export design
(.claude/plans/not-started/tier2-blunder-evidence-export.md), for the
package that owns the score/format contract: typed scores, loss arithmetic,
record field order and the manifest/blunder*/complete stream.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import chess.engine as engine  # noqa: E402  (path must be set first)

import external_quality_export as exp  # noqa: E402


def _finite(before_value, after_value):
    before = exp.endpoint_from_score(engine.Cp(before_value), None)
    after = exp.endpoint_from_score(engine.Cp(after_value), None)
    return before, after


def _sample_oracle():
    before = exp.endpoint_from_score(engine.Cp(0), "e2e4")
    after = exp.endpoint_from_score(engine.Cp(-200), "e7e5")
    return before, after


def _blunder_kwargs(oracle_before, oracle_after, **overrides):
    kwargs = dict(
        input_index=0, game_index=0, ply_index=0, headers={}, build="a", mover="white",
        setup_fen="s", moves_before_uci=[], before_fen="b", after_fen="a",
        played_move_uci="e2e4", phase="opening", ply_since_book_exit=None,
        book_exit_basis="unknown", annotation=None, engine_score_cp=0, engine_depth=None,
        engine_time_s=None, next_annotation=None, next_engine_score_cp=0, self_swing_cp=0,
        oracle_before=oracle_before, oracle_after=oracle_after,
    )
    kwargs.update(overrides)
    return kwargs


class RawScoreTests(unittest.TestCase):
    def test_cp_to_json(self):
        self.assertEqual(exp.RawScore.cp(-350).to_json(), {"kind": "cp", "value": -350})

    def test_cp_to_json_key_order(self):
        self.assertEqual(list(exp.RawScore.cp(1).to_json().keys()), ["kind", "value"])

    def test_mate_to_json(self):
        score = exp.RawScore.mate(3, "opponent")
        self.assertEqual(score.to_json(), {"kind": "mate", "moves": 3, "winner": "opponent"})

    def test_mate_to_json_key_order(self):
        self.assertEqual(list(exp.RawScore.mate(1, "mover").to_json().keys()),
                          ["kind", "moves", "winner"])

    def test_is_finite(self):
        self.assertTrue(exp.RawScore.cp(0).is_finite)
        self.assertFalse(exp.RawScore.mate(1, "mover").is_finite)

    def test_mate_rejects_negative_moves(self):
        with self.assertRaises(ValueError):
            exp.RawScore.mate(-1, "mover")

    def test_mate_rejects_bad_winner(self):
        with self.assertRaises(ValueError):
            exp.RawScore.mate(1, "nobody")

    def test_rejects_unknown_kind(self):
        with self.assertRaises(ValueError):
            exp.RawScore(kind="invalid")

    def test_cp_rejects_non_integer_value(self):
        for value in (None, 1.5, True, "100"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                exp.RawScore.cp(value)

    def test_cp_rejects_mate_fields(self):
        with self.assertRaises(ValueError):
            exp.RawScore(kind="cp", value=0, winner="mover")

    def test_mate_rejects_cp_value(self):
        with self.assertRaises(ValueError):
            exp.RawScore(kind="mate", moves=1, winner="mover", value=0)


class EndpointConstructorTests(unittest.TestCase):
    def test_finite_within_clamp_not_clipped(self):
        r = exp.endpoint_from_score(engine.Cp(1000), "e2e4")
        self.assertEqual(r.legacy_cp, 1000)
        self.assertFalse(r.finite_clipped)
        self.assertEqual(r.source, "search")
        self.assertEqual(r.best_move_uci, "e2e4")
        self.assertEqual(r.score, exp.RawScore.cp(1000))

    def test_finite_beyond_clamp_is_clipped(self):
        r = exp.endpoint_from_score(engine.Cp(-1500), None)
        self.assertEqual(r.legacy_cp, -1000)
        self.assertTrue(r.finite_clipped)
        self.assertEqual(r.score.value, -1500)

    def test_mate_winner_mover(self):
        r = exp.endpoint_from_score(engine.Mate(3), "d1d8")
        self.assertEqual(r.score, exp.RawScore.mate(3, "mover"))
        self.assertEqual(r.legacy_cp, 1000)
        self.assertFalse(r.finite_clipped)
        self.assertEqual(r.source, "search")

    def test_mate_winner_opponent(self):
        r = exp.endpoint_from_score(engine.Mate(-2), None)
        self.assertEqual(r.score, exp.RawScore.mate(2, "opponent"))
        self.assertEqual(r.legacy_cp, -1000)

    def test_mate_zero_is_opponent(self):
        r = exp.endpoint_from_score(engine.Mate(0), None)
        self.assertEqual(r.score, exp.RawScore.mate(0, "opponent"))
        self.assertEqual(r.legacy_cp, -1000)

    def test_checkmate_endpoint_mover_mated(self):
        r = exp.checkmate_endpoint(mover_is_mated=True)
        self.assertEqual(r.score, exp.RawScore.mate(0, "opponent"))
        self.assertEqual(r.legacy_cp, -1000)
        self.assertEqual(r.source, "checkmate")
        self.assertIsNone(r.best_move_uci)

    def test_checkmate_endpoint_mover_delivers(self):
        r = exp.checkmate_endpoint(mover_is_mated=False)
        self.assertEqual(r.score, exp.RawScore.mate(0, "mover"))
        self.assertEqual(r.legacy_cp, 1000)

    def test_drawn_endpoint_stalemate(self):
        r = exp.drawn_endpoint("stalemate")
        self.assertEqual(r.score, exp.RawScore.cp(0))
        self.assertEqual(r.legacy_cp, 0)
        self.assertFalse(r.finite_clipped)
        self.assertIsNone(r.best_move_uci)

    def test_drawn_endpoint_insufficient_material(self):
        r = exp.drawn_endpoint("insufficient_material")
        self.assertEqual(r.source, "insufficient_material")

    def test_drawn_endpoint_rejects_other_sources(self):
        with self.assertRaises(ValueError):
            exp.drawn_endpoint("checkmate")

    def test_oracle_result_to_json_field_order(self):
        r = exp.endpoint_from_score(engine.Cp(10), "e2e4")
        self.assertEqual(list(r.to_json().keys()),
                          ["score", "legacy_cp", "finite_clipped", "source", "best_move_uci"])


class LossArithmeticTests(unittest.TestCase):
    def test_loss_149_not_exported(self):
        before, after = _finite(0, -149)
        self.assertEqual(exp.legacy_loss_cp(before, after), 149)
        self.assertFalse(exp.is_exported(before, after))

    def test_loss_150_exported(self):
        before, after = _finite(0, -150)
        self.assertEqual(exp.legacy_loss_cp(before, after), 150)
        self.assertTrue(exp.is_exported(before, after))

    def test_before0_after_minus1000(self):
        before, after = _finite(0, -1000)
        self.assertEqual(exp.legacy_loss_cp(before, after), 1000)
        self.assertEqual(exp.raw_loss_cp(before, after), 1000)
        self.assertFalse(before.finite_clipped)
        self.assertFalse(after.finite_clipped)

    def test_before0_after_minus1500(self):
        before, after = _finite(0, -1500)
        self.assertEqual(exp.legacy_loss_cp(before, after), 1000)
        self.assertEqual(exp.raw_loss_cp(before, after), 1500)
        self.assertFalse(before.finite_clipped)
        self.assertTrue(after.finite_clipped)

    def test_before1000_after_minus1000(self):
        before, after = _finite(1000, -1000)
        self.assertEqual(exp.legacy_loss_cp(before, after), 2000)
        self.assertEqual(exp.raw_loss_cp(before, after), 2000)
        self.assertFalse(before.finite_clipped)
        self.assertFalse(after.finite_clipped)

    def test_before1200_after1000_is_a_clipping_miss(self):
        before, after = _finite(1200, 1000)
        self.assertFalse(exp.is_exported(before, after))
        self.assertTrue(exp.is_finite_clipping_miss(before, after))

    def test_before1150_after1000_is_a_clipping_miss(self):
        before, after = _finite(1150, 1000)
        self.assertFalse(exp.is_exported(before, after))
        self.assertTrue(exp.is_finite_clipping_miss(before, after))

    def test_finite_raw_loss_149_is_not_a_miss(self):
        before, after = _finite(1149, 1000)
        self.assertEqual(exp.raw_loss_cp(before, after), 149)
        self.assertFalse(exp.is_finite_clipping_miss(before, after))

    def test_mate_endpoint_never_a_clipping_miss(self):
        before = exp.endpoint_from_score(engine.Cp(1200), None)
        after = exp.checkmate_endpoint(mover_is_mated=True)
        self.assertIsNone(exp.raw_loss_cp(before, after))
        self.assertFalse(exp.is_finite_clipping_miss(before, after))

    def test_finite_to_mate(self):
        before = exp.endpoint_from_score(engine.Cp(0), None)
        after = exp.checkmate_endpoint(mover_is_mated=True)
        self.assertIsNone(exp.raw_loss_cp(before, after))
        self.assertEqual(after.legacy_cp, -1000)
        self.assertEqual(exp.legacy_loss_cp(before, after), 1000)

    def test_mate_to_finite(self):
        before = exp.endpoint_from_score(engine.Mate(2), None)
        after = exp.endpoint_from_score(engine.Cp(-500), None)
        self.assertEqual(before.legacy_cp, 1000)
        self.assertIsNone(exp.raw_loss_cp(before, after))
        self.assertEqual(exp.legacy_loss_cp(before, after), 1500)

    def test_both_mate_directions(self):
        winning = exp.checkmate_endpoint(mover_is_mated=False)
        losing = exp.checkmate_endpoint(mover_is_mated=True)
        self.assertEqual(winning.legacy_cp, 1000)
        self.assertEqual(losing.legacy_cp, -1000)
        self.assertIsNone(exp.raw_loss_cp(winning, losing))
        self.assertEqual(exp.legacy_loss_cp(winning, losing), 2000)

    def test_mate_zero_terminal_scores(self):
        self.assertEqual(exp.drawn_endpoint("stalemate").legacy_cp, 0)
        self.assertEqual(exp.checkmate_endpoint(mover_is_mated=True).legacy_cp, -1000)
        self.assertEqual(exp.checkmate_endpoint(mover_is_mated=False).legacy_cp, 1000)


class ManifestRecordTests(unittest.TestCase):
    def test_field_order_and_selection(self):
        record = exp.manifest_record(
            source_run="run-1", source_root="/root",
            inputs=[{"relative_path": "a/b.pgn", "sha256": "abc"}],
            producer={"python": "3.12"}, oracle={"binary": "stockfish"}, scan={"jobs": 1},
        )
        self.assertEqual(list(record.keys()), [
            "type", "schema_version", "source_run", "source_root", "inputs",
            "producer", "oracle", "scan", "selection", "oracle_policy",
        ])
        self.assertEqual(record["type"], "manifest")
        self.assertEqual(record["schema_version"], 1)
        self.assertEqual(record["selection"], {
            "id": "tier2_contested_v1", "contested_abs_cp": 150,
            "min_legacy_loss_cp": 150, "clamp_cp": 1000,
        })
        self.assertEqual(record["oracle_policy"], "bare_fen_per_game_hash_v1")

    def test_source_run_defaults_to_none(self):
        record = exp.manifest_record(source_run=None, source_root="/root", inputs=[],
                                      producer={}, oracle={}, scan={})
        self.assertIsNone(record["source_run"])


class BlunderRecordTests(unittest.TestCase):
    def test_field_order(self):
        before, after = _sample_oracle()
        record = exp.blunder_record(**_blunder_kwargs(
            before, after, input_index=0, game_index=1, ply_index=4, headers={"White": "a"},
            setup_fen="startpos", moves_before_uci=["e2e4"], before_fen="fen-before",
            after_fen="fen-after", ply_since_book_exit=0, book_exit_basis="setup_assumed",
            annotation="note", engine_score_cp=0, engine_depth=12, engine_time_s=1.0,
            next_annotation="next", next_engine_score_cp=-200, self_swing_cp=200,
        ))
        self.assertEqual(list(record.keys()), [
            "type", "schema_version", "row_id", "input_index", "game_index", "ply_index",
            "headers", "build", "mover", "setup_fen", "moves_before_uci", "before_fen",
            "after_fen", "played_move_uci", "phase", "ply_since_book_exit", "book_exit_basis",
            "annotation", "engine_score_cp", "engine_depth", "engine_time_s", "next_annotation",
            "next_engine_score_cp", "self_swing_cp", "oracle_before", "oracle_after",
            "legacy_loss_cp", "raw_loss_cp",
        ])
        self.assertEqual(record["row_id"], "0:1:4")
        self.assertEqual(record["legacy_loss_cp"], 200)
        self.assertEqual(record["raw_loss_cp"], 200)

    def test_row_id_distinguishes_same_fen_different_ordinals(self):
        before, after = _sample_oracle()
        kwargs = _blunder_kwargs(before, after, before_fen="same", after_fen="same2")
        r1 = exp.blunder_record(**{**kwargs, "input_index": 0, "game_index": 0})
        r2 = exp.blunder_record(**{**kwargs, "input_index": 0, "game_index": 1})
        self.assertNotEqual(r1["row_id"], r2["row_id"])
        self.assertEqual(r1["before_fen"], r2["before_fen"])

    def test_rejects_bad_mover(self):
        before, after = _sample_oracle()
        with self.assertRaises(ValueError):
            exp.blunder_record(**_blunder_kwargs(before, after, mover="sideways"))

    def test_rejects_bad_book_exit_basis(self):
        before, after = _sample_oracle()
        with self.assertRaises(ValueError):
            exp.blunder_record(**_blunder_kwargs(before, after, book_exit_basis="nope"))


class CompleteRecordTests(unittest.TestCase):
    def test_field_order_and_sorted_cells(self):
        cells = [
            {"build": "b", "phase": "endgame", "eligible_rows": 2, "exported_rows": 1,
             "finite_clipping_misses": 0},
            {"build": "a", "phase": "opening", "eligible_rows": 3, "exported_rows": 1,
             "finite_clipping_misses": 1},
        ]
        record = exp.complete_record(eligible_rows=5, exported_rows=2, finite_clipping_misses=1,
                                      scored_games=4, cells=cells)
        self.assertEqual(list(record.keys()), [
            "type", "schema_version", "eligible_rows", "exported_rows",
            "finite_clipping_misses", "scored_games", "cells",
        ])
        self.assertEqual([c["build"] for c in record["cells"]], ["a", "b"])
        self.assertEqual(list(record["cells"][0].keys()), [
            "build", "phase", "eligible_rows", "exported_rows", "finite_clipping_misses"])

    def test_rejects_mismatched_eligible_sum(self):
        with self.assertRaises(ValueError):
            exp.complete_record(eligible_rows=10, exported_rows=0, finite_clipping_misses=0,
                                 scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                          "eligible_rows": 1, "exported_rows": 0,
                                                          "finite_clipping_misses": 0}])

    def test_rejects_mismatched_exported_sum(self):
        with self.assertRaises(ValueError):
            exp.complete_record(eligible_rows=1, exported_rows=5, finite_clipping_misses=0,
                                 scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                          "eligible_rows": 1, "exported_rows": 0,
                                                          "finite_clipping_misses": 0}])

    def test_rejects_mismatched_miss_sum(self):
        with self.assertRaises(ValueError):
            exp.complete_record(eligible_rows=1, exported_rows=0, finite_clipping_misses=1,
                                 scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                          "eligible_rows": 1, "exported_rows": 0,
                                                          "finite_clipping_misses": 0}])

    def test_rejects_misses_exceeding_gap(self):
        with self.assertRaises(ValueError):
            exp.complete_record(eligible_rows=2, exported_rows=1, finite_clipping_misses=5,
                                 scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                          "eligible_rows": 2, "exported_rows": 1,
                                                          "finite_clipping_misses": 5}])

    def test_rejects_negative_counts(self):
        with self.assertRaises(ValueError):
            exp.complete_record(eligible_rows=-1, exported_rows=0, finite_clipping_misses=0,
                                 scored_games=0, cells=[])

    def test_rejects_non_integer_global_count(self):
        # 0.0 and True still reconcile against the sums, so only the type check catches them.
        for field, value in (("eligible_rows", 0.0), ("scored_games", True)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                counts = dict(eligible_rows=0, exported_rows=0, finite_clipping_misses=0,
                              scored_games=0, cells=[])
                counts[field] = value
                exp.complete_record(**counts)

    def test_rejects_non_integer_cell_count(self):
        with self.assertRaises(ValueError):
            exp.complete_record(eligible_rows=1, exported_rows=0, finite_clipping_misses=0,
                                 scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                          "eligible_rows": 1.0,
                                                          "exported_rows": 0,
                                                          "finite_clipping_misses": 0}])

    def test_zero_row_complete_is_valid(self):
        record = exp.complete_record(eligible_rows=0, exported_rows=0, finite_clipping_misses=0,
                                      scored_games=0, cells=[])
        self.assertEqual(record["exported_rows"], 0)
        self.assertEqual(record["cells"], [])


class ExportWriterTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / "out.jsonl"

    @staticmethod
    def _manifest():
        return exp.manifest_record(source_run=None, source_root="r", inputs=[],
                                    producer={}, oracle={}, scan={})

    @staticmethod
    def _empty_complete():
        return exp.complete_record(eligible_rows=0, exported_rows=0, finite_clipping_misses=0,
                                    scored_games=0, cells=[])

    def test_write_blunder_before_manifest_raises(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        with self.assertRaises(ValueError):
            writer.write_blunder({"type": "blunder"})

    def test_write_complete_before_manifest_raises(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        with self.assertRaises(ValueError):
            writer.write_complete({"type": "complete"})

    def test_second_manifest_raises(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        writer.write_manifest(self._manifest())
        with self.assertRaises(ValueError):
            writer.write_manifest(self._manifest())

    def test_write_after_complete_raises(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        writer.write_manifest(self._manifest())
        writer.write_complete(self._empty_complete())
        with self.assertRaises(ValueError):
            writer.write_complete(self._empty_complete())
        before, after = _sample_oracle()
        with self.assertRaises(ValueError):
            writer.write_blunder(exp.blunder_record(**_blunder_kwargs(before, after)))

    def test_close_does_not_synthesize_completion(self):
        writer = exp.ExportWriter(self.path)
        writer.write_manifest(self._manifest())
        writer.close()
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_exclusive_creation_rejects_existing_file(self):
        self.path.write_text("existing", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            exp.ExportWriter(self.path)
        self.assertEqual(self.path.read_text(encoding="utf-8"), "existing")

    def test_exclusive_creation_rejects_existing_partial_file(self):
        writer = exp.ExportWriter(self.path)
        writer.write_manifest(self._manifest())
        writer.close()
        original = self.path.read_bytes()
        with self.assertRaises(FileExistsError):
            exp.ExportWriter(self.path)
        self.assertEqual(self.path.read_bytes(), original)

    def test_flush_makes_content_readable_before_close(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        writer.write_manifest(self._manifest())
        content = self.path.read_text(encoding="utf-8")
        self.assertIn('"type":"manifest"', content)

    def test_zero_row_artifact_round_trips(self):
        writer = exp.ExportWriter(self.path)
        writer.write_manifest(self._manifest())
        writer.write_complete(self._empty_complete())
        writer.close()
        manifest, blunders, complete = exp.read_artifact(self.path)
        self.assertEqual(blunders, [])
        self.assertEqual(complete["exported_rows"], 0)
        self.assertEqual(manifest["type"], "manifest")

    def test_context_manager_closes_file(self):
        with exp.ExportWriter(self.path) as writer:
            writer.write_manifest(self._manifest())
        with self.assertRaises(ValueError):
            writer.write_manifest(self._manifest())


class ReadArtifactDefectTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / "artifact.jsonl"

    def _write_lines(self, lines):
        self.path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    @staticmethod
    def _manifest_line():
        return json.dumps(exp.manifest_record(source_run=None, source_root="r", inputs=[],
                                               producer={}, oracle={}, scan={}))

    @staticmethod
    def _complete_line(exported=0):
        cells = [{"build": "a", "phase": "opening", "eligible_rows": exported,
                  "exported_rows": exported, "finite_clipping_misses": 0}] if exported else []
        return json.dumps(exp.complete_record(eligible_rows=exported, exported_rows=exported,
                                               finite_clipping_misses=0, scored_games=exported,
                                               cells=cells))

    @staticmethod
    def _blunder_line(**overrides):
        before, after = _sample_oracle()
        return json.dumps(exp.blunder_record(**_blunder_kwargs(before, after, **overrides)))

    @staticmethod
    def _cell(build="a", phase="opening", exported=1):
        return {"build": build, "phase": phase, "eligible_rows": exported,
                "exported_rows": exported, "finite_clipping_misses": 0}

    def _footer_line(self, cells):
        total = sum(c["exported_rows"] for c in cells)
        return json.dumps(exp.complete_record(eligible_rows=total, exported_rows=total,
                                               finite_clipping_misses=0, scored_games=total,
                                               cells=cells))

    def test_malformed_json_rejected(self):
        self._write_lines([self._manifest_line(), "{not json", self._complete_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_unsupported_schema_version_rejected(self):
        bad = exp.manifest_record(source_run=None, source_root="r", inputs=[],
                                   producer={}, oracle={}, scan={})
        bad["schema_version"] = 2
        self._write_lines([json.dumps(bad), self._complete_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_missing_manifest_rejected(self):
        self._write_lines([self._complete_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_duplicate_manifest_rejected(self):
        self._write_lines([self._manifest_line(), self._manifest_line(), self._complete_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_missing_complete_rejected(self):
        self._write_lines([self._manifest_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_duplicate_complete_rejected(self):
        self._write_lines([self._manifest_line(), self._complete_line(), self._complete_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_record_after_completion_rejected(self):
        self._write_lines([self._manifest_line(), self._complete_line(), self._manifest_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_unknown_type_rejected(self):
        self._write_lines([self._manifest_line(),
                            json.dumps({"type": "mystery", "schema_version": 1}),
                            self._complete_line()])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_blunder_before_manifest_rejected(self):
        before, after = _sample_oracle()
        blunder = exp.blunder_record(**_blunder_kwargs(before, after))
        self._write_lines([json.dumps(blunder), self._manifest_line(), self._complete_line(1)])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_exported_rows_mismatch_rejected(self):
        complete = exp.complete_record(eligible_rows=1, exported_rows=1, finite_clipping_misses=0,
                                        scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                                 "eligible_rows": 1,
                                                                 "exported_rows": 1,
                                                                 "finite_clipping_misses": 0}])
        self._write_lines([self._manifest_line(), json.dumps(complete)])  # claims 1, has 0
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_cell_sum_mismatch_rejected(self):
        complete = exp.complete_record(eligible_rows=2, exported_rows=0, finite_clipping_misses=0,
                                        scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                                 "eligible_rows": 2,
                                                                 "exported_rows": 0,
                                                                 "finite_clipping_misses": 0}])
        complete["eligible_rows"] = 3  # tamper: now disagrees with the cell it was built from
        self._write_lines([self._manifest_line(), json.dumps(complete)])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_truncated_completion_record_rejected(self):
        complete = json.loads(self._complete_line())
        del complete["finite_clipping_misses"]
        self._write_lines([self._manifest_line(), json.dumps(complete)])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_cell_missing_a_counter_rejected(self):
        complete = json.loads(self._complete_line(exported=1))
        del complete["cells"][0]["exported_rows"]
        self._write_lines([self._manifest_line(), json.dumps(complete)])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_nan_and_infinity_tokens_rejected(self):
        # json.loads accepts these by default; the artifact format is JSON, which does not.
        for token in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(token=token):
                row = '{"type":"blunder","schema_version":1,"legacy_loss_cp":' + token + '}'
                self._write_lines([self._manifest_line(), row, self._complete_line(1)])
                with self.assertRaises(ValueError):
                    exp.read_artifact(self.path)

    def test_non_integer_footer_count_rejected(self):
        complete = json.loads(self._complete_line())
        # Both reconcile against the empty cell list; only the type is wrong.
        complete["eligible_rows"] = 0.0
        complete["scored_games"] = True
        self._write_lines([self._manifest_line(), json.dumps(complete)])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_duplicate_row_id_rejected(self):
        row = self._blunder_line()
        self._write_lines([self._manifest_line(), row, row,
                            self._footer_line([self._cell(exported=2)])])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_row_id_disagreeing_with_indices_rejected(self):
        row = json.loads(self._blunder_line())
        row["row_id"] = "9:9:9"
        self._write_lines([self._manifest_line(), json.dumps(row),
                            self._footer_line([self._cell()])])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_row_missing_an_identity_field_rejected(self):
        row = json.loads(self._blunder_line())
        del row["phase"]
        self._write_lines([self._manifest_line(), json.dumps(row),
                            self._footer_line([self._cell()])])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_row_filed_under_the_wrong_build_rejected(self):
        # The defect the footer sums cannot see: totals reconcile (2 rows, 2 claimed)
        # while one row sits in a build the single declared cell does not cover.
        self._write_lines([self._manifest_line(),
                            self._blunder_line(),
                            self._blunder_line(build="b", ply_index=1),
                            self._footer_line([self._cell(exported=2)])])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_row_outside_every_declared_cell_rejected(self):
        rows = [self._blunder_line(build="A"), self._blunder_line(build="A", ply_index=1)]
        self._write_lines([self._manifest_line(), *rows,
                            self._footer_line([self._cell(exported=2)])])
        with self.assertRaisesRegex(ValueError, "have no cell"):
            exp.read_artifact(self.path)

    def test_per_cell_exported_count_mismatch_rejected(self):
        # Both rows are build "a"; the footer splits them one per build.
        self._write_lines([self._manifest_line(),
                            self._blunder_line(),
                            self._blunder_line(ply_index=1),
                            self._footer_line([self._cell(), self._cell(build="b")])])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_duplicate_cell_rejected(self):
        complete = json.loads(self._footer_line([self._cell(exported=2)]))
        complete["cells"] = [self._cell(), self._cell()]
        self._write_lines([self._manifest_line(), self._blunder_line(),
                            self._blunder_line(ply_index=1), json.dumps(complete)])
        with self.assertRaises(ValueError):
            exp.read_artifact(self.path)

    def test_rows_across_two_cells_round_trip(self):
        self._write_lines([self._manifest_line(),
                            self._blunder_line(),
                            self._blunder_line(build="b", ply_index=1),
                            self._footer_line([self._cell(), self._cell(build="b")])])
        _, blunders, complete = exp.read_artifact(self.path)
        self.assertEqual(len(blunders), 2)
        self.assertEqual(complete["exported_rows"], 2)

    def test_valid_artifact_round_trips(self):
        before, after = _sample_oracle()
        blunder = exp.blunder_record(**_blunder_kwargs(
            before, after, ply_index=2, headers={"White": "a"}, moves_before_uci=["e2e4"],
            ply_since_book_exit=0, book_exit_basis="setup_assumed", annotation="x",
            engine_depth=12, engine_time_s=0.5, next_annotation="y", next_engine_score_cp=-200,
            self_swing_cp=200,
        ))
        complete = exp.complete_record(eligible_rows=1, exported_rows=1, finite_clipping_misses=0,
                                        scored_games=1, cells=[{"build": "a", "phase": "opening",
                                                                 "eligible_rows": 1,
                                                                 "exported_rows": 1,
                                                                 "finite_clipping_misses": 0}])
        self._write_lines([self._manifest_line(), json.dumps(blunder), json.dumps(complete)])
        manifest, blunders, comp = exp.read_artifact(self.path)
        self.assertEqual(len(blunders), 1)
        self.assertEqual(comp["exported_rows"], 1)
        self.assertEqual(manifest["type"], "manifest")


class CheckOutputPathTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def test_rejects_existing_file(self):
        p = self.dir / "out.jsonl"
        p.write_text("x", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            exp.check_output_path(p)

    def test_rejects_missing_parent_directory(self):
        p = self.dir / "missing" / "out.jsonl"
        with self.assertRaises(ValueError):
            exp.check_output_path(p)

    def test_rejects_collision_with_json_path(self):
        p = self.dir / "out.jsonl"
        with self.assertRaises(ValueError):
            exp.check_output_path(p, json_path=p)

    def test_allows_distinct_paths(self):
        p = self.dir / "out.jsonl"
        j = self.dir / "out.json"
        exp.check_output_path(p, json_path=j)  # must not raise


class SerializationTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / "out.jsonl"

    def test_lf_line_endings(self):
        writer = exp.ExportWriter(self.path)
        writer.write_manifest(exp.manifest_record(source_run=None, source_root="r", inputs=[],
                                                    producer={}, oracle={}, scan={}))
        writer.close()
        raw = self.path.read_bytes()
        self.assertNotIn(b"\r\n", raw)
        self.assertTrue(raw.endswith(b"\n"))

    def test_nan_rejected(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        with self.assertRaises(ValueError):
            writer.write_manifest({"type": "manifest", "schema_version": 1, "bad": float("nan")})

    def test_infinity_rejected(self):
        writer = exp.ExportWriter(self.path)
        self.addCleanup(writer.close)
        with self.assertRaises(ValueError):
            writer.write_manifest({"type": "manifest", "schema_version": 1, "bad": float("inf")})

    def test_non_ascii_preserved(self):
        writer = exp.ExportWriter(self.path)
        writer.write_manifest(exp.manifest_record(source_run="Zürich", source_root="r", inputs=[],
                                                    producer={}, oracle={}, scan={}))
        writer.close()
        raw = self.path.read_text(encoding="utf-8")
        self.assertIn("Zürich", raw)
        self.assertNotIn("\\u", raw)


if __name__ == "__main__":
    unittest.main()
