"""Unit tests for external_quality_export.py (no engine, no network).

Covers the score/format contract and the analyzer's use of it: typed scores, loss
arithmetic, record field order, the manifest/blunder*/complete stream and the
failure ordering around it. Docs/MoveQualityExport.md is the format itself.
"""

import io
import json
import sys
import tempfile
import unittest
import unittest.mock
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import chess  # noqa: E402  (path must be set first)
import chess.engine as engine  # noqa: E402

import analyze_external_quality as aeq  # noqa: E402
import analyze_move_quality as amq  # noqa: E402
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


class ExtractionTests(unittest.TestCase):
    """Fixtures for scan_games() and the extract() projection built on it."""

    SKIP_THEN_SURVIVE_PGN = """\
[Event "no-result"]
[White "A"]
[Black "B"]
[Result "*"]

1. e4 e5 *

[Event "survivor"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} e5 {-0.10/10 0.100s} 2. Nf3 {+0.20/10 0.100s} Nc6 {-0.20/10 0.100s} 1-0
"""

    CORRUPT_THEN_SURVIVE_PGN = """\
[Event "corrupt"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} Zz9 {+0.10/10 0.100s} 1-0

[Event "survivor"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} e5 {-0.10/10 0.100s} 2. Nf3 {+0.20/10 0.100s} Nc6 {-0.20/10 0.100s} 1-0
"""

    BOOK_PREFIX_PGN = """\
[Event "book-prefix"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {book} e5 {book} 2. Nf3 {+0.10/10 0.100s} Nc6 {-0.10/10 0.100s}
3. Bb5 {+0.20/10 0.100s} a6 {-0.20/10 0.100s} 1-0
"""

    # A non-standard setup, so the replay invariant exercises a real setup_fen
    # rather than one that happens to equal the standard start.
    SETUP_ASSUMED_PGN = """\
[Event "setup-assumed"]
[White "A"]
[Black "B"]
[Result "1-0"]
[SetUp "1"]
[FEN "r3k3/8/8/8/8/8/8/R3K3 w - - 0 1"]

1. Ra2 {+0.10/10 0.100s} Ra7 {-0.10/10 0.100s} 2. Ra3 {+0.20/10 0.100s} Ra6 {-0.20/10 0.100s} 1-0
"""

    # A scored ply whose adjudication note reads "book". It is not a book move:
    # it carries a real score and stays eligible.
    BOOK_NOTE_PGN = """\
[Event "book-note"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s, book} e5 {-0.10/10 0.100s, book} 2. Nf3 {+0.20/10 0.100s}
Nc6 {-0.20/10 0.100s} 1-0
"""

    BOOK_PREFIX_THEN_NOTE_PGN = """\
[Event "book-prefix-then-note"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {book} e5 {book} 2. Nf3 {+0.10/10 0.100s} Nc6 {-0.10/10 0.100s}
3. Bb5 {+0.20/10 0.100s, book} a6 {-0.20/10 0.100s} 1-0
"""

    MATE_SCORE_PGN = """\
[Event "mate-score"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} e5 {-0.10/10 0.100s} 2. Qh5 {+M3/12 0.100s} Nc6 {-0.10/10 0.100s}
3. Bc4 {+0.20/10 0.100s} Nf6 {-0.20/10 0.100s} 1-0
"""

    PLAIN_PGN = """\
[Event "plain"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} e5 {-0.10/10 0.100s} 2. Nf3 {+0.20/10 0.100s} Nc6 {-0.20/10 0.100s} 1-0
"""

    LATE_BOOK_PGN = """\
[Event "late-book"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} e5 {-0.10/10 0.100s} 2. Nf3 {book} Nc6 {-0.20/10 0.100s}
3. Bb5 {+0.20/10 0.100s} a6 {-0.20/10 0.100s} 1-0
"""

    @staticmethod
    def _write(tmp, text):
        path = Path(tmp) / "match.pgn"
        path.write_text(text, encoding="utf-8")
        return path

    def _scan(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            return aeq.scan_games(self._write(tmp, text))

    def _extract(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            return aeq.extract(self._write(tmp, text))

    # Captured from extract() before it was reprojected onto scan_games(). The
    # literal rows are the point: a shape assertion would survive the very change
    # this guards against.
    SELF_TEST_ROWS = [[
        ("A", "opening", True, -10,
         "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
         "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1", "e2e4"),
        ("B", "opening", False, 885,
         "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
         "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2", "e7e5"),
    ]]

    def test_extract_rows_are_unchanged(self):
        self.assertEqual(self._extract(amq.SELF_TEST_PGN), self.SELF_TEST_ROWS)

    def test_extract_equals_projection_of_scan_games(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(tmp, amq.SELF_TEST_PGN)
            extracted = aeq.extract(path)
            scans = aeq.scan_games(path)
        expected = []
        for scan in scans:
            rows = []
            for i in scan.eligible:
                ply, ply_next = scan.plies[i], scan.plies[i + 2]
                rows.append((ply.build, ply.bucket, ply.mover, ply.cp - ply_next.cp,
                             ply.before_fen, ply.after_fen, ply.played_uci))
            expected.append(rows)
        self.assertEqual(extracted, expected)

    def test_game_index_counts_a_skipped_no_result_game(self):
        scans = self._scan(self.SKIP_THEN_SURVIVE_PGN)
        self.assertEqual(len(scans), 1)
        self.assertEqual(scans[0].game_index, 1)

    def test_game_index_counts_a_corrupt_game(self):
        scans = self._scan(self.CORRUPT_THEN_SURVIVE_PGN)
        self.assertEqual(len(scans), 1)
        self.assertEqual(scans[0].game_index, 1)

    def test_replay_invariant_for_every_eligible_row(self):
        fixtures = (amq.SELF_TEST_PGN, self.BOOK_PREFIX_PGN, self.SETUP_ASSUMED_PGN,
                    self.PLAIN_PGN, self.LATE_BOOK_PGN, self.MATE_SCORE_PGN,
                    self.BOOK_NOTE_PGN, self.BOOK_PREFIX_THEN_NOTE_PGN)
        checked = 0
        for text in fixtures:
            for scan in self._scan(text):
                for i in scan.eligible:
                    ply = scan.plies[i]
                    board = chess.Board(scan.setup_fen)
                    for uci in aeq.moves_before(scan, i):
                        board.push_uci(uci)
                    self.assertEqual(board.fen(), ply.before_fen)
                    board.push_uci(ply.played_uci)
                    self.assertEqual(board.fen(), ply.after_fen)
                    checked += 1
        self.assertGreater(checked, 0)

    def test_book_exit_explicit_prefix(self):
        scan = self._scan(self.BOOK_PREFIX_PGN)[0]
        self.assertEqual(scan.book_exit_basis, "explicit_book_prefix")
        self.assertEqual(scan.book_exit_ply, 2)
        self.assertEqual(aeq.ply_since_book_exit(scan, 2), 0)

    def test_book_exit_setup_assumed(self):
        scan = self._scan(self.SETUP_ASSUMED_PGN)[0]
        self.assertEqual(scan.book_exit_basis, "setup_assumed")
        self.assertEqual(scan.book_exit_ply, 0)
        self.assertEqual(scan.setup_fen, "r3k3/8/8/8/8/8/8/R3K3 w - - 0 1")
        self.assertTrue(scan.eligible)
        for i in scan.eligible:
            self.assertEqual(aeq.ply_since_book_exit(scan, i), i)

    def test_book_exit_unknown_when_neither_signal_present(self):
        scan = self._scan(self.PLAIN_PGN)[0]
        self.assertEqual(scan.book_exit_basis, "unknown")
        self.assertIsNone(scan.book_exit_ply)
        self.assertIsNone(aeq.ply_since_book_exit(scan, 0))

    def test_a_scored_ply_with_a_book_note_is_not_a_book_move(self):
        # parse_comment returns note="book" here as well, so classifying by the
        # note would place the book exit after a row that stays eligible and
        # give that row a negative ply_since_book_exit.
        scan = self._scan(self.BOOK_NOTE_PGN)[0]
        self.assertEqual(scan.plies[0].note, "book")
        self.assertEqual(scan.plies[0].cp, 10)
        self.assertFalse(scan.plies[0].is_book)
        self.assertIn(0, scan.eligible)
        self.assertIsNone(scan.book_exit_ply)
        self.assertEqual(scan.book_exit_basis, "unknown")
        for i in scan.eligible:
            self.assertIsNone(aeq.ply_since_book_exit(scan, i))

    def test_book_note_after_a_book_prefix_does_not_reopen_the_exit(self):
        scan = self._scan(self.BOOK_PREFIX_THEN_NOTE_PGN)[0]
        self.assertEqual(scan.book_exit_basis, "explicit_book_prefix")
        self.assertEqual(scan.book_exit_ply, 2)
        self.assertEqual(scan.plies[4].note, "book")
        self.assertFalse(scan.plies[4].is_book)
        for i in scan.eligible:
            self.assertGreaterEqual(aeq.ply_since_book_exit(scan, i), 0)

    def test_book_exit_unknown_when_book_annotation_follows_real_play(self):
        scan = self._scan(self.LATE_BOOK_PGN)[0]
        self.assertEqual(scan.book_exit_basis, "unknown")
        self.assertIsNone(scan.book_exit_ply)

    def test_annotation_matches_raw_comment_and_parse_comment_fields(self):
        scan = self._scan(amq.SELF_TEST_PGN)[0]
        mate_ply = scan.plies[4]
        self.assertEqual(mate_ply.annotation, "+M1/13 0.100s, Win by checkmate")
        cp, _mate, depth, seconds, note = amq.parse_comment(mate_ply.annotation, "x")
        self.assertIsNone(cp)
        self.assertEqual(mate_ply.cp, cp)
        self.assertEqual(mate_ply.depth, depth)
        self.assertEqual(mate_ply.seconds, seconds)
        self.assertEqual(mate_ply.note, note)
        self.assertEqual(mate_ply.note, "Win by checkmate")

    def test_book_plies_are_never_eligible(self):
        scan = self._scan(self.BOOK_PREFIX_PGN)[0]
        book_indices = {i for i, p in enumerate(scan.plies) if p.is_book}
        self.assertTrue(book_indices)
        self.assertFalse(book_indices & set(scan.eligible))

    def test_mate_scored_ply_is_never_eligible(self):
        # Ply 2 has two plies after it, so only its null cp can disqualify it.
        # Ply 0 falls with it: the contested filter reads the score two plies on.
        scan = self._scan(self.MATE_SCORE_PGN)[0]
        self.assertIsNone(scan.plies[2].cp)
        self.assertEqual(len(scan.plies), 6)
        self.assertEqual(scan.eligible, (1, 3))

    def test_unreadable_annotation_raises_parse_error(self):
        bad = amq.SELF_TEST_PGN.replace("{+0.20/10 0.500s}", "{+0.20/10 500ms}", 1)
        with self.assertRaises(amq.ParseError):
            self._scan(bad)
        with self.assertRaises(amq.ParseError):
            self._extract(bad)


class _FakeOracle:
    """Stands in for Stockfish: scripted White-POV scores, and a log of every call.

    Scores are scripted from White's point of view precisely so the tests can
    tell a point-of-view slip from a correct read: the module must flip them for
    a Black mover, on the after-position as much as the before-position.
    """

    def __init__(self, scores):
        self.scores = scores
        self.calls = []

    def analyse(self, board, limit, game=None):
        fen = board.fen()
        self.calls.append(fen)
        score = self.scores[fen]
        pv = [next(iter(board.legal_moves))] if board.legal_moves else []
        return {"score": engine.PovScore(score, chess.WHITE), "pv": pv}


class ScoringIntegrationTests(unittest.TestCase):
    """The scoring path: unchanged searches and counters, plus the export rows."""

    PGN = ExtractionTests.PLAIN_PGN

    def _scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "match.pgn"
            path.write_text(self.PGN, encoding="utf-8")
            return aeq.scan_games(path, input_index=3)[0]

    def _run(self, scan, white_cp, records=None):
        """Score `scan` against a fake oracle; -> (fake, cells, worst)."""
        scores = {fen: engine.Cp(cp) for fen, cp in white_cp.items()}
        fake = _FakeOracle(scores)
        cells = defaultdict(aeq.new_cell)
        worst = []
        with unittest.mock.patch.object(aeq, "_engine", lambda: fake):
            aeq._score_rows(scan, {}, cells, worst, records)
        return fake, dict(cells), worst

    @staticmethod
    def _fens(scan):
        """-> (start, after White's first move, after Black's reply)."""
        return (scan.plies[0].before_fen, scan.plies[0].after_fen, scan.plies[1].after_fen)

    def test_oracle_eval_projects_the_endpoint_legacy_cp(self):
        board = chess.Board()
        fake = _FakeOracle({board.fen(): engine.Cp(1200)})
        with unittest.mock.patch.object(aeq, "_engine", lambda: fake):
            endpoint, best = aeq.oracle_endpoint(board, chess.WHITE)
            legacy_cp, eval_best = aeq.oracle_eval(board, chess.WHITE)
        self.assertEqual(endpoint.score.value, 1200)      # raw score survives
        self.assertEqual(endpoint.legacy_cp, 1000)        # legacy projection clamps
        self.assertTrue(endpoint.finite_clipped)
        self.assertEqual(legacy_cp, endpoint.legacy_cp)
        self.assertEqual(eval_best, best)

    def test_export_off_and_on_issue_the_same_searches_and_counters(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        cp = {start: 20, after_e4: 30, after_e5: 900}
        off_fake, off_cells, off_worst = self._run(scan, cp, records=None)
        records = []
        on_fake, on_cells, on_worst = self._run(scan, cp, records=records)
        self.assertEqual(off_fake.calls, on_fake.calls)
        self.assertEqual(off_cells, on_cells)
        self.assertEqual(off_worst, on_worst)
        self.assertTrue(records)  # the run did have something to export

    def test_the_before_position_is_searched_once_per_distinct_fen(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        fake, _cells, _worst = self._run(scan, {start: 20, after_e4: 30, after_e5: 25})
        # Two rows: each searches its own before and after position. The middle
        # FEN is row 0's after and row 1's before, and is not cached across those
        # roles -- only repeated `before` lookups are.
        self.assertEqual(fake.calls, [start, after_e4, after_e4, after_e5])

    def test_the_after_endpoint_uses_the_original_movers_point_of_view(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        # White is fine throughout; Black's reply walks into +900 for White,
        # which is a 870cp loss for Black and nothing at all read from White's side.
        records = []
        _fake, cells, _worst = self._run(scan, {start: 20, after_e4: 30, after_e5: 900}, records)
        black_cell = cells[("B", "opening")]
        self.assertEqual(black_cell[3], 1)
        self.assertEqual(black_cell[4], 870.0)
        self.assertEqual(cells[("A", "opening")][3], 0)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["mover"], "black")
        self.assertEqual(records[0]["oracle_before"]["score"], {"kind": "cp", "value": -30})
        self.assertEqual(records[0]["oracle_after"]["score"], {"kind": "cp", "value": -900})
        self.assertEqual(records[0]["legacy_loss_cp"], 870)

    def test_every_exported_row_is_a_blunder_count_event(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        for label, cp in (("both", {start: 900, after_e4: -900, after_e5: 900}),
                          ("neither", {start: 20, after_e4: 30, after_e5: 25}),
                          ("black only", {start: 20, after_e4: 30, after_e5: 900})):
            with self.subTest(label):
                records = []
                _fake, cells, _worst = self._run(scan, cp, records)
                self.assertEqual(len(records), sum(c[3] for c in cells.values()))

    def test_finite_clipping_misses_are_counted_but_never_exported(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        # White: +1200 -> +1100 raw, both clamped to +1000, so the legacy loss is
        # 0 while the finite loss is 100 -- below the threshold, not a miss.
        # Black: -1100 -> -1400 raw (White +1100 -> +1400), legacy 0 and finite 300.
        records = []
        _fake, cells, _worst = self._run(scan, {start: 1200, after_e4: 1100, after_e5: 1400},
                                          records)
        self.assertEqual(cells[("A", "opening")][7], 0)
        self.assertEqual(cells[("B", "opening")][7], 1)
        self.assertEqual(records, [])
        for cell in cells.values():
            self.assertLessEqual(cell[7], cell[0] - cell[3])

    def test_terminal_positions_are_scored_without_a_search(self):
        # Fool's mate: White is to move and mated, so White reads -1000 and the
        # mate is scored for Black in zero moves.
        mated = "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"
        cases = (
            (mated, chess.WHITE, "checkmate", -1000, {"kind": "mate", "moves": 0,
                                                       "winner": "opponent"}),
            (mated, chess.BLACK, "checkmate", 1000, {"kind": "mate", "moves": 0,
                                                      "winner": "mover"}),
            ("7k/8/8/8/8/8/5q2/7K w - - 0 1", chess.WHITE, "stalemate", 0,
             {"kind": "cp", "value": 0}),
            ("4k3/8/8/8/8/8/8/4K3 w - - 0 1", chess.WHITE, "insufficient_material", 0,
             {"kind": "cp", "value": 0}),
        )
        for fen, pov, source, legacy, raw in cases:
            with self.subTest(source=source, pov="white" if pov else "black"):
                fake = _FakeOracle({})
                board = chess.Board(fen)
                with unittest.mock.patch.object(aeq, "_engine", lambda: fake):
                    endpoint, best = aeq.oracle_endpoint(board, pov)
                self.assertEqual(fake.calls, [])
                self.assertEqual(endpoint.source, source)
                self.assertEqual(endpoint.legacy_cp, legacy)
                self.assertEqual(endpoint.score.to_json(), raw)
                self.assertFalse(endpoint.finite_clipped)
                self.assertIsNone(best)
                self.assertIsNone(endpoint.best_move_uci)

    def test_exported_record_carries_its_source_metadata(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        records = []
        self._run(scan, {start: 20, after_e4: 30, after_e5: 900}, records)
        row = records[0]
        self.assertEqual(row["row_id"], f"3:{scan.game_index}:1")
        self.assertEqual(row["input_index"], 3)
        self.assertEqual(row["ply_index"], 1)
        self.assertEqual(row["build"], "B")
        self.assertEqual(row["phase"], "opening")
        self.assertEqual(row["setup_fen"], scan.setup_fen)
        self.assertEqual(row["moves_before_uci"], [scan.plies[0].played_uci])
        self.assertEqual(row["played_move_uci"], scan.plies[1].played_uci)
        self.assertEqual(row["annotation"], scan.plies[1].annotation)
        self.assertEqual(row["engine_score_cp"], scan.plies[1].cp)
        self.assertEqual(row["next_annotation"], scan.plies[3].annotation)
        self.assertEqual(row["next_engine_score_cp"], scan.plies[3].cp)
        self.assertEqual(row["self_swing_cp"], scan.plies[1].cp - scan.plies[3].cp)
        self.assertEqual(row["book_exit_basis"], "unknown")
        self.assertIsNone(row["ply_since_book_exit"])
        self.assertEqual(row["headers"]["Event"], "plain")

    # An eight-ply game and a fixed score sequence spanning the clamp, captured
    # from _score_rows() before the export existed. The counters are the contract:
    # the export may add slot 7 and its records, and must move nothing else.
    PARITY_PGN = """\
[Event "parity"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+0.10/10 0.100s} e5 {-0.10/10 0.100s} 2. Nf3 {+0.20/10 0.100s} Nc6 {-0.20/10 0.100s}
3. Bb5 {+0.30/10 0.100s} a6 {-1.20/10 0.100s} 4. Ba4 {+0.25/10 0.100s} Nf6 {-0.30/10 0.100s} 1-0
"""
    PARITY_SCORES = (50, -400, 900, 1200, 1100, -1500, 20, 60, 1400, 1250, -90, 700)
    PARITY_CELLS = {("A", "opening"): [3, 0, 25.0, 2, 2450.0, 0, 0.0],
                    ("B", "opening"): [3, 0, 200.0, 1, 930.0, 0, 0.0]}

    def test_counters_and_searches_match_the_pre_export_run(self):
        scores = iter(self.PARITY_SCORES)

        class _Sequenced(_FakeOracle):
            def analyse(self, board, limit, game=None):
                self.calls.append(board.fen())
                pv = [next(iter(board.legal_moves))] if board.legal_moves else []
                return {"score": engine.PovScore(engine.Cp(next(scores)), chess.WHITE), "pv": pv}

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "match.pgn"
            path.write_text(self.PARITY_PGN, encoding="utf-8")
            scan = aeq.scan_games(path)[0]
        fake = _Sequenced({})
        cells, worst, records = defaultdict(aeq.new_cell), [], []
        with unittest.mock.patch.object(aeq, "_engine", lambda: fake):
            aeq._score_rows(scan, {}, cells, worst, records)
        self.assertEqual(len(fake.calls), 12)
        self.assertEqual({k: v[:7] for k, v in cells.items()}, self.PARITY_CELLS)
        self.assertEqual(len(worst), 3)
        self.assertEqual([r["row_id"] for r in records], ["0:0:0", "0:0:2", "0:0:5"])
        self.assertEqual(sum(c[7] for c in cells.values()), 2)

    def test_a_record_replays_to_its_own_before_position(self):
        scan = self._scan()
        start, after_e4, after_e5 = self._fens(scan)
        records = []
        self._run(scan, {start: 20, after_e4: 30, after_e5: 900}, records)
        for row in records:
            board = chess.Board(row["setup_fen"])
            for uci in row["moves_before_uci"]:
                board.push_uci(uci)
            self.assertEqual(board.fen(), row["before_fen"])
            board.push_uci(row["played_move_uci"])
            self.assertEqual(board.fen(), row["after_fen"])


class _InProcessPool:
    """Stands in for the process pool: same initializer contract, no subprocess.

    Export is a worker initarg, so keeping that contract exactly is what makes an
    in-process run a faithful test of the parent's writing.
    """

    created: list = []

    def __init__(self, max_workers=None, initializer=None, initargs=()):
        self.max_workers = max_workers
        self.shut_down = False
        _InProcessPool.created.append(self)
        initializer(*initargs)

    def map(self, fn, iterable):
        return [fn(item) for item in iterable]

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.shut_down = True


class _SequencedOracle(_FakeOracle):
    """Answers in call order rather than by position, for the parity fixture."""

    def __init__(self, scores):
        super().__init__({})
        self._scores = iter(scores)

    def analyse(self, board, limit, game=None):
        self.calls.append(board.fen())
        pv = [next(iter(board.legal_moves))] if board.legal_moves else []
        return {"score": engine.PovScore(engine.Cp(next(self._scores)), chess.WHITE), "pv": pv}


class _SideToMoveOracle(_FakeOracle):
    """Scores by side to move alone, so every eligible row is a blunder.

    Order-independent by construction, which is what lets it compare runs whose
    scheduling differs.
    """

    def __init__(self):
        super().__init__({})

    def analyse(self, board, limit, game=None):
        self.calls.append(board.fen())
        pv = [next(iter(board.legal_moves))] if board.legal_moves else []
        cp = engine.Cp(900 if board.turn == chess.WHITE else 0)
        return {"score": engine.PovScore(cp, chess.WHITE), "pv": pv}


class ExportRunTests(unittest.TestCase):
    """analyse() and main() end to end: preflight, writing, and completion."""

    PGN = ScoringIntegrationTests.PARITY_PGN
    SCORES = ScoringIntegrationTests.PARITY_SCORES

    QUIET_PGN = """\
[Event "quiet"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 {+9.00/10 0.100s} e5 {-9.00/10 0.100s} 2. Nf3 {+9.00/10 0.100s} Nc6 {-9.00/10 0.100s} 1-0
"""

    def setUp(self):
        self._globals = (aeq._ENGINE_PATH, aeq._DEPTH, aeq._EXPORT)
        _InProcessPool.created = []
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "corpus"
        (self.root / "shard-1").mkdir(parents=True)
        self.pgn = self.root / "shard-1" / "match.pgn"
        self.pgn.write_text(self.PGN, encoding="utf-8")
        self.out = Path(self._tmp.name) / "evidence.jsonl"

    def tearDown(self):
        aeq._ENGINE_PATH, aeq._DEPTH, aeq._EXPORT = self._globals
        self._tmp.cleanup()

    def _analyse(self, oracle, *, export_path=None, source_run=None, root=None, batch=1,
                 jobs=1, limit=0, shards=0):
        """Run analyse() over `root` against `oracle`; -> its three results."""
        with unittest.mock.patch.object(aeq, "ProcessPoolExecutor", _InProcessPool), \
                unittest.mock.patch.object(aeq, "_engine", lambda: oracle), \
                unittest.mock.patch.object(aeq, "_log", lambda message: None):
            return aeq.analyse(root or self.root, str(self.pgn), 12, jobs, limit, shards,
                               batch, export_path, source_run)

    # --- option-off compatibility ------------------------------------------

    def test_export_off_writes_nothing_and_reports_the_same_numbers(self):
        off = self._analyse(_SequencedOracle(self.SCORES))
        on = self._analyse(_SequencedOracle(self.SCORES), export_path=self.out)
        self.assertEqual(off, on)
        self.assertFalse((Path(self._tmp.name) / "evidence-off.jsonl").exists())

    def test_export_off_leaves_workers_uninitialized_for_export(self):
        self._analyse(_SequencedOracle(self.SCORES))
        self.assertFalse(aeq._EXPORT)
        self._analyse(_SequencedOracle(self.SCORES), export_path=self.out)
        self.assertTrue(aeq._EXPORT)

    def test_the_pool_is_shut_down_on_both_paths(self):
        self._analyse(_SequencedOracle(self.SCORES))
        self._analyse(_SequencedOracle(self.SCORES), export_path=self.out)
        self.assertEqual([pool.shut_down for pool in _InProcessPool.created], [True, True])

    # --- a successful scan --------------------------------------------------

    def test_a_successful_scan_reconciles(self):
        cells, per_game, _worst = self._analyse(_SequencedOracle(self.SCORES),
                                                export_path=self.out)
        manifest, blunders, complete = exp.read_artifact(self.out)
        self.assertEqual(manifest["type"], "manifest")
        self.assertEqual([row["row_id"] for row in blunders], ["0:0:0", "0:0:2", "0:0:5"])
        self.assertEqual(complete["eligible_rows"], sum(c[0] for c in cells.values()))
        self.assertEqual(complete["exported_rows"], sum(c[3] for c in cells.values()))
        self.assertEqual((complete["eligible_rows"], complete["exported_rows"]), (6, 3))
        self.assertEqual(complete["finite_clipping_misses"], 2)
        self.assertEqual(complete["scored_games"], len(per_game))
        self.assertEqual([(c["build"], c["phase"], c["exported_rows"]) for c in complete["cells"]],
                         [("A", "opening", 2), ("B", "opening", 1)])

    def test_zero_eligible_rows_still_completes(self):
        self.pgn.write_text(self.QUIET_PGN, encoding="utf-8")
        cells, _per_game, _worst = self._analyse(_SideToMoveOracle(), export_path=self.out)
        self.assertEqual(cells, {})
        _manifest, blunders, complete = exp.read_artifact(self.out)
        self.assertEqual(blunders, [])
        self.assertEqual(complete["cells"], [])
        self.assertEqual((complete["eligible_rows"], complete["exported_rows"]), (0, 0))
        # A game with no eligible row is dropped before scoring, so it is not one
        # of the games scored -- the same count the report prints.
        self.assertEqual(complete["scored_games"], 0)

    def test_jobs_and_batch_variants_export_identical_rows(self):
        (self.root / "shard-2").mkdir()
        (self.root / "shard-2" / "match.pgn").write_text(self.PGN, encoding="utf-8")
        rows = []
        for index, (jobs, batch) in enumerate(((1, 1), (4, 1), (2, 8))):
            out = Path(self._tmp.name) / f"variant-{index}.jsonl"
            self._analyse(_SideToMoveOracle(), export_path=out, jobs=jobs, batch=batch)
            _manifest, blunders, complete = exp.read_artifact(out)
            rows.append((blunders, complete))
        for blunders, complete in rows[1:]:
            self.assertEqual(blunders, rows[0][0])
            self.assertEqual(complete, rows[0][1])
        self.assertEqual(len(rows[0][0]), 12)      # every eligible row, both shards

    # --- the manifest -------------------------------------------------------

    def test_manifest_identifies_its_inputs_and_its_oracle(self):
        self._analyse(_SequencedOracle(self.SCORES), export_path=self.out,
                      source_run="33989392373")
        manifest, _blunders, _complete = exp.read_artifact(self.out)
        self.assertEqual(manifest["source_run"], "33989392373")
        self.assertEqual(manifest["source_root"], str(self.root.resolve()))
        self.assertEqual(manifest["inputs"],
                         [{"relative_path": "shard-1/match.pgn",
                           "sha256": aeq._sha256(self.pgn)}])
        self.assertEqual(manifest["oracle"],
                         {"binary": self.pgn.name, "sha256": aeq._sha256(self.pgn),
                          "depth": 12, "threads": 1, "hash_mb": 64})
        self.assertEqual(manifest["scan"], {"jobs": 1, "batch": 1, "shards": 0, "games": 0})
        producer = manifest["producer"]
        self.assertEqual(producer["python_chess"], chess.__version__)
        self.assertEqual(producer["external_quality_export_sha256"],
                         aeq._sha256(Path(exp.__file__).resolve()))
        self.assertEqual(producer["analyze_external_quality_sha256"],
                         aeq._sha256(Path(aeq.__file__).resolve()))

    def test_manifest_records_the_oracle_options_actually_configured(self):
        self.assertEqual((aeq.ORACLE_THREADS, aeq.ORACLE_HASH_MB), (1, 64))
        source = Path(aeq.__file__).read_text(encoding="utf-8")
        self.assertIn('_ENGINE.configure({"Threads": ORACLE_THREADS, "Hash": ORACLE_HASH_MB})',
                      source)

    def test_a_single_file_root_is_identified_by_its_name(self):
        self._analyse(_SequencedOracle(self.SCORES), export_path=self.out, root=self.pgn)
        manifest, _blunders, _complete = exp.read_artifact(self.out)
        self.assertEqual([entry["relative_path"] for entry in manifest["inputs"]], ["match.pgn"])

    def test_only_the_scanned_shards_are_listed(self):
        (self.root / "shard-2").mkdir()
        (self.root / "shard-2" / "match.pgn").write_text(self.PGN, encoding="utf-8")
        self._analyse(_SideToMoveOracle(), export_path=self.out, shards=1)
        manifest, _blunders, _complete = exp.read_artifact(self.out)
        self.assertEqual([entry["relative_path"] for entry in manifest["inputs"]],
                         ["shard-1/match.pgn"])
        self.assertEqual(manifest["scan"]["shards"], 1)

    # --- failure ordering ---------------------------------------------------

    def test_an_interrupted_scan_leaves_no_completion(self):
        (self.root / "shard-2").mkdir()
        (self.root / "shard-2" / "match.pgn").write_text(self.PGN, encoding="utf-8")
        real_score_batch = aeq.score_batch
        seen = []

        def failing(games):
            seen.append(games)
            if len(seen) > 1:
                raise RuntimeError("worker died")
            return real_score_batch(games)

        with unittest.mock.patch.object(aeq, "score_batch", failing):
            with self.assertRaises(RuntimeError):
                self._analyse(_SideToMoveOracle(), export_path=self.out)
        lines = self.out.read_text(encoding="utf-8").splitlines()
        kinds = [json.loads(line)["type"] for line in lines]
        self.assertEqual(kinds, ["manifest"] + ["blunder"] * 6)
        with self.assertRaises(ValueError):
            exp.read_artifact(self.out)

    def test_a_report_failure_keeps_the_completion(self):
        def explode(*args, **kwargs):
            raise ZeroDivisionError("bootstrap failed")

        with self.assertRaises(ZeroDivisionError):
            self._main(_SequencedOracle(self.SCORES), ["--worst-jsonl", str(self.out)],
                       report=explode)
        _manifest, blunders, complete = exp.read_artifact(self.out)
        self.assertEqual(len(blunders), complete["exported_rows"])

    # --- the CLI ------------------------------------------------------------

    def _main(self, oracle, extra, report=None):
        """Run main() over the fixture corpus; the report is still built, into a buffer.

        `report` replaces it outright, which is how the failure-ordering test puts
        an exception after scoring has finished.
        """
        real_report = aeq.report

        def quiet(cells, per_game, worst, depth, samples, out=None):
            real_report(cells, per_game, worst, depth, samples, out=io.StringIO())

        argv = ["analyze_external_quality.py", str(self.root), "--engine", str(self.pgn),
                "--jobs", "1", "--samples", "10", *extra]
        with unittest.mock.patch.object(sys, "argv", argv), \
                unittest.mock.patch.object(aeq, "ProcessPoolExecutor", _InProcessPool), \
                unittest.mock.patch.object(aeq, "_engine", lambda: oracle), \
                unittest.mock.patch.object(aeq, "_log", lambda message: None), \
                unittest.mock.patch.object(aeq, "report", report or quiet):
            return aeq.main()

    def test_main_writes_a_reconciling_artifact(self):
        self.assertEqual(self._main(_SequencedOracle(self.SCORES),
                                    ["--worst-jsonl", str(self.out)]), 0)
        _manifest, blunders, complete = exp.read_artifact(self.out)
        self.assertEqual(len(blunders), 3)
        self.assertEqual(complete["exported_rows"], 3)

    def test_preflight_rejects_an_existing_output_before_any_search(self):
        self.out.write_text("previous partial\n", encoding="utf-8")
        self.assertEqual(self._main(_SequencedOracle(self.SCORES),
                                    ["--worst-jsonl", str(self.out)]), 2)
        self.assertEqual(self.out.read_text(encoding="utf-8"), "previous partial\n")
        self.assertEqual(_InProcessPool.created, [])

    def test_preflight_rejects_a_missing_output_directory(self):
        missing = Path(self._tmp.name) / "absent" / "evidence.jsonl"
        self.assertEqual(self._main(_SequencedOracle(self.SCORES),
                                    ["--worst-jsonl", str(missing)]), 2)
        self.assertEqual(_InProcessPool.created, [])

    def test_preflight_rejects_a_collision_with_the_json_path(self):
        self.assertEqual(self._main(_SequencedOracle(self.SCORES),
                                    ["--worst-jsonl", str(self.out),
                                     "--json", str(self.out)]), 2)
        self.assertFalse(self.out.exists())
        self.assertEqual(_InProcessPool.created, [])

    def test_source_run_without_an_export_path_is_rejected(self):
        with self.assertRaises(SystemExit) as raised:
            with unittest.mock.patch.object(sys, "stderr", io.StringIO()):
                self._main(_SequencedOracle(self.SCORES), ["--source-run", "33989392373"])
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(_InProcessPool.created, [])

    def test_no_eligible_rows_keeps_its_cli_error_and_its_artifact(self):
        self.pgn.write_text(self.QUIET_PGN, encoding="utf-8")
        self.assertEqual(self._main(_SideToMoveOracle(),
                                    ["--worst-jsonl", str(self.out)]), 1)
        _manifest, blunders, complete = exp.read_artifact(self.out)
        self.assertEqual((blunders, complete["exported_rows"]), ([], 0))

if __name__ == "__main__":
    unittest.main()
