"""JSONL export contract for Tier 2 blunder evidence.

Defines the typed oracle scores, loss arithmetic, record builders and the
manifest/blunder*/complete stream writer/reader that `analyze_external_quality.py`
will use to persist every eligible contested row whose legacy clamped loss is at
least `MIN_LEGACY_LOSS_CP`. The schema, eligibility and score-perspective rules
are fixed by `.claude/plans/not-started/tier2-blunder-evidence-export.md`;
consult that design, and `Docs/MoveQuality.md` for the surrounding method, before
changing anything here. This module does no PGN parsing and launches no engine.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

SCHEMA_VERSION = 1
CLAMP_CP = 1000            # must equal analyze_external_quality.CLAMP_CP
MATE_CP = 100000           # must equal analyze_external_quality.MATE_CP
MIN_LEGACY_LOSS_CP = 150
SELECTION_ID = "tier2_contested_v1"
ORACLE_POLICY = "bare_fen_per_game_hash_v1"

_VALID_MOVERS = ("white", "black")
_VALID_MATE_WINNERS = ("mover", "opponent")
_VALID_DRAW_SOURCES = ("stalemate", "insufficient_material")
_VALID_BOOK_BASIS = ("explicit_book_prefix", "setup_assumed", "unknown")


def _clamp(value: int) -> int:
    return max(-CLAMP_CP, min(CLAMP_CP, value))


@dataclass(frozen=True)
class RawScore:
    """A finite centipawn score or a mate distance, never both."""

    kind: str                 # "cp" or "mate"
    value: int | None = None  # cp only
    moves: int | None = None  # mate only, nonnegative
    winner: str | None = None # mate only, "mover" or "opponent"

    @classmethod
    def cp(cls, value: int) -> "RawScore":
        return cls(kind="cp", value=value)

    @classmethod
    def mate(cls, moves: int, winner: str) -> "RawScore":
        if moves < 0:
            raise ValueError(f"mate moves must be nonnegative, got {moves}")
        if winner not in _VALID_MATE_WINNERS:
            raise ValueError(f"winner must be one of {_VALID_MATE_WINNERS}, got {winner!r}")
        return cls(kind="mate", moves=moves, winner=winner)

    @property
    def is_finite(self) -> bool:
        return self.kind == "cp"

    def to_json(self) -> dict:
        if self.kind == "cp":
            return {"kind": "cp", "value": self.value}
        return {"kind": "mate", "moves": self.moves, "winner": self.winner}


@dataclass(frozen=True)
class OracleResult:
    """One endpoint (before or after the played move), from the mover's POV."""

    score: RawScore
    legacy_cp: int
    finite_clipped: bool
    source: str                # "search" | "checkmate" | "stalemate" | "insufficient_material"
    best_move_uci: str | None

    def to_json(self) -> dict:
        return {
            "score": self.score.to_json(),
            "legacy_cp": self.legacy_cp,
            "finite_clipped": self.finite_clipped,
            "source": self.source,
            "best_move_uci": self.best_move_uci,
        }


def endpoint_from_score(score, best_move_uci: str | None) -> OracleResult:
    """Build an endpoint from a searched, non-terminal position.

    `score` is a `chess.engine.Score` with the mover's POV already applied
    (the caller passes `info["score"].pov(pov)`), matching the existing
    `oracle_eval` conversion this preserves.
    """
    if score.is_mate():
        mate_moves = score.mate()
        raw = RawScore.mate(abs(mate_moves), "mover" if mate_moves > 0 else "opponent")
        legacy_cp = _clamp(score.score(mate_score=MATE_CP))
        return OracleResult(score=raw, legacy_cp=legacy_cp, finite_clipped=False,
                           source="search", best_move_uci=best_move_uci)
    value = score.score()
    raw = RawScore.cp(value)
    return OracleResult(score=raw, legacy_cp=_clamp(value), finite_clipped=abs(value) > CLAMP_CP,
                        source="search", best_move_uci=best_move_uci)


def checkmate_endpoint(mover_is_mated: bool) -> OracleResult:
    """Build a terminal endpoint for a checkmated board; no engine call needed."""
    winner = "opponent" if mover_is_mated else "mover"
    legacy_cp = -CLAMP_CP if mover_is_mated else CLAMP_CP
    return OracleResult(score=RawScore.mate(0, winner), legacy_cp=legacy_cp, finite_clipped=False,
                        source="checkmate", best_move_uci=None)


def drawn_endpoint(source: str) -> OracleResult:
    """Build a terminal endpoint for a drawn board (stalemate or bare material)."""
    if source not in _VALID_DRAW_SOURCES:
        raise ValueError(f"source must be one of {_VALID_DRAW_SOURCES}, got {source!r}")
    return OracleResult(score=RawScore.cp(0), legacy_cp=0, finite_clipped=False,
                        source=source, best_move_uci=None)


def legacy_loss_cp(before: OracleResult, after: OracleResult) -> int:
    return max(0, before.legacy_cp - after.legacy_cp)


def raw_loss_cp(before: OracleResult, after: OracleResult) -> int | None:
    """Finite-scale loss, or None unless both endpoints are finite (Invariant 5)."""
    if before.score.is_finite and after.score.is_finite:
        return max(0, before.score.value - after.score.value)
    return None


def is_exported(before: OracleResult, after: OracleResult) -> bool:
    return legacy_loss_cp(before, after) >= MIN_LEGACY_LOSS_CP


def is_finite_clipping_miss(before: OracleResult, after: OracleResult) -> bool:
    """True when legacy clamping hid a finite loss that would itself have qualified."""
    if legacy_loss_cp(before, after) >= MIN_LEGACY_LOSS_CP:
        return False
    raw = raw_loss_cp(before, after)
    return raw is not None and raw >= MIN_LEGACY_LOSS_CP


def manifest_record(*, source_run: str | None, source_root: str, inputs: list[dict],
                    producer: dict, oracle: dict, scan: dict) -> dict:
    return {
        "type": "manifest",
        "schema_version": SCHEMA_VERSION,
        "source_run": source_run,
        "source_root": source_root,
        "inputs": inputs,
        "producer": producer,
        "oracle": oracle,
        "scan": scan,
        "selection": {
            "id": SELECTION_ID,
            "contested_abs_cp": 150,
            "min_legacy_loss_cp": MIN_LEGACY_LOSS_CP,
            "clamp_cp": CLAMP_CP,
        },
        "oracle_policy": ORACLE_POLICY,
    }


def blunder_record(*, input_index, game_index, ply_index, headers, build, mover, setup_fen,
                   moves_before_uci, before_fen, after_fen, played_move_uci, phase,
                   ply_since_book_exit, book_exit_basis, annotation, engine_score_cp,
                   engine_depth, engine_time_s, next_annotation, next_engine_score_cp,
                   self_swing_cp, oracle_before: OracleResult, oracle_after: OracleResult) -> dict:
    if mover not in _VALID_MOVERS:
        raise ValueError(f"mover must be one of {_VALID_MOVERS}, got {mover!r}")
    if book_exit_basis not in _VALID_BOOK_BASIS:
        raise ValueError(f"book_exit_basis must be one of {_VALID_BOOK_BASIS}, got {book_exit_basis!r}")
    return {
        "type": "blunder",
        "schema_version": SCHEMA_VERSION,
        "row_id": f"{input_index}:{game_index}:{ply_index}",
        "input_index": input_index,
        "game_index": game_index,
        "ply_index": ply_index,
        "headers": headers,
        "build": build,
        "mover": mover,
        "setup_fen": setup_fen,
        "moves_before_uci": moves_before_uci,
        "before_fen": before_fen,
        "after_fen": after_fen,
        "played_move_uci": played_move_uci,
        "phase": phase,
        "ply_since_book_exit": ply_since_book_exit,
        "book_exit_basis": book_exit_basis,
        "annotation": annotation,
        "engine_score_cp": engine_score_cp,
        "engine_depth": engine_depth,
        "engine_time_s": engine_time_s,
        "next_annotation": next_annotation,
        "next_engine_score_cp": next_engine_score_cp,
        "self_swing_cp": self_swing_cp,
        "oracle_before": oracle_before.to_json(),
        "oracle_after": oracle_after.to_json(),
        "legacy_loss_cp": legacy_loss_cp(oracle_before, oracle_after),
        "raw_loss_cp": raw_loss_cp(oracle_before, oracle_after),
    }


def complete_record(*, eligible_rows: int, exported_rows: int, finite_clipping_misses: int,
                    scored_games: int, cells: list[dict]) -> dict:
    sum_eligible = sum(c["eligible_rows"] for c in cells)
    sum_exported = sum(c["exported_rows"] for c in cells)
    sum_misses = sum(c["finite_clipping_misses"] for c in cells)
    all_counts = [eligible_rows, exported_rows, finite_clipping_misses, scored_games]
    for c in cells:
        all_counts.extend((c["eligible_rows"], c["exported_rows"], c["finite_clipping_misses"]))
    if any(v < 0 for v in all_counts):
        raise ValueError("all counts must be nonnegative")
    if sum_eligible != eligible_rows:
        raise ValueError(f"cell eligible_rows sum {sum_eligible} != global eligible_rows {eligible_rows}")
    if sum_exported != exported_rows:
        raise ValueError(f"cell exported_rows sum {sum_exported} != global exported_rows {exported_rows}")
    if sum_misses != finite_clipping_misses:
        raise ValueError(
            f"cell finite_clipping_misses sum {sum_misses} != global finite_clipping_misses "
            f"{finite_clipping_misses}"
        )
    if finite_clipping_misses > eligible_rows - exported_rows:
        raise ValueError("finite_clipping_misses exceeds eligible_rows - exported_rows")
    sorted_cells = sorted(cells, key=lambda c: (c["build"], c["phase"]))
    return {
        "type": "complete",
        "schema_version": SCHEMA_VERSION,
        "eligible_rows": eligible_rows,
        "exported_rows": exported_rows,
        "finite_clipping_misses": finite_clipping_misses,
        "scored_games": scored_games,
        "cells": [
            {
                "build": c["build"],
                "phase": c["phase"],
                "eligible_rows": c["eligible_rows"],
                "exported_rows": c["exported_rows"],
                "finite_clipping_misses": c["finite_clipping_misses"],
            }
            for c in sorted_cells
        ],
    }


class ExportWriter:
    """Writes the manifest / blunder* / complete stream, enforcing that order.

    Opens PATH with exclusive creation so a previous partial file is never
    silently overwritten. Every record is flushed immediately, so an
    interruption keeps exactly what was scored before it. `close()` never
    synthesizes a completion record: an interrupted scan stays footer-less.
    """

    _BEFORE_MANIFEST = "before_manifest"
    _IN_BLUNDERS = "in_blunders"
    _DONE = "done"

    def __init__(self, path: Path) -> None:
        self._file = open(path, "x", encoding="utf-8", newline="\n")
        self._state = self._BEFORE_MANIFEST

    def _write(self, record: dict) -> None:
        line = json.dumps(record, allow_nan=False, ensure_ascii=False, separators=(",", ":"))
        self._file.write(line + "\n")
        self._file.flush()

    def write_manifest(self, record: dict) -> None:
        if self._state != self._BEFORE_MANIFEST:
            raise ValueError("manifest must be the first record written, and written exactly once")
        self._write(record)
        self._state = self._IN_BLUNDERS

    def write_blunder(self, record: dict) -> None:
        if self._state != self._IN_BLUNDERS:
            raise ValueError(
                "blunder record requires the manifest to already be written and the stream "
                "not yet completed"
            )
        self._write(record)

    def write_complete(self, record: dict) -> None:
        if self._state != self._IN_BLUNDERS:
            raise ValueError(
                "complete record requires the manifest to already be written, and may be "
                "written only once"
            )
        self._write(record)
        self._state = self._DONE

    def close(self) -> None:
        if not self._file.closed:
            self._file.close()

    def __enter__(self) -> "ExportWriter":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


def read_artifact(path: Path) -> tuple[dict, list[dict], dict]:
    """Parse and validate a written artifact, returning (manifest, blunders, complete).

    Rejects malformed JSON, an unsupported schema version, a missing or
    duplicated manifest/complete record, any record after completion, an
    unknown record type, and counts that fail to reconcile.
    """
    manifest: dict | None = None
    complete: dict | None = None
    blunders: list[dict] = []
    with open(path, "r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"malformed JSON on line {line_no}: {exc}") from exc
            version = record.get("schema_version")
            if version != SCHEMA_VERSION:
                raise ValueError(f"unsupported schema_version {version!r} on line {line_no}")
            if complete is not None:
                raise ValueError(f"record on line {line_no} follows the completion record")
            record_type = record.get("type")
            if record_type == "manifest":
                if manifest is not None:
                    raise ValueError(f"duplicate manifest record on line {line_no}")
                manifest = record
            elif record_type == "blunder":
                if manifest is None:
                    raise ValueError(f"blunder record on line {line_no} precedes the manifest")
                blunders.append(record)
            elif record_type == "complete":
                if manifest is None:
                    raise ValueError(f"complete record on line {line_no} precedes the manifest")
                complete = record
            else:
                raise ValueError(f"unknown record type {record_type!r} on line {line_no}")
    if manifest is None:
        raise ValueError("artifact is missing its manifest record")
    if complete is None:
        raise ValueError("artifact is missing its completion record")
    # A hand-edited or truncated footer is a rejection, not a KeyError: the caller
    # asked whether this artifact is usable, and every answer has to be that verdict.
    counters = ("eligible_rows", "exported_rows", "finite_clipping_misses")
    for field in (*counters, "scored_games", "cells"):
        if field not in complete:
            raise ValueError(f"completion record is missing {field!r}")
    for n, cell in enumerate(complete["cells"]):
        missing = [f for f in ("build", "phase", *counters) if f not in cell]
        if missing:
            raise ValueError(f"cell {n} is missing {', '.join(missing)}")
    if complete["exported_rows"] != len(blunders):
        raise ValueError(
            f"exported_rows {complete['exported_rows']} does not match {len(blunders)} blunder records"
        )
    for field in counters:
        cell_sum = sum(c[field] for c in complete["cells"])
        if cell_sum != complete[field]:
            raise ValueError(f"cell {field} sum {cell_sum} does not match global {field} {complete[field]}")
    return manifest, blunders, complete


def check_output_path(path: Path, json_path: Path | None = None) -> None:
    """Preflight the export destination before any expensive work starts."""
    if path.exists():
        raise FileExistsError(f"output path already exists: {path}")
    if not path.parent.is_dir():
        raise ValueError(f"output directory does not exist: {path.parent}")
    if json_path is not None and path.resolve() == json_path.resolve():
        raise ValueError(f"--worst-jsonl and --json must not resolve to the same path: {path}")
