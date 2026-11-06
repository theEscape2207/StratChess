#!/usr/bin/env python3
"""Verify and join the contiguous pieces of one lab shard.

A shard's pairs can be played as several pieces: the replayed slices of a lost shard, or (in a waved
run) the shard's wave slices. Each piece is checked alone by the batch's own verify and log reader,
then the pieces are joined into one shard directory that the batch-level verify and pool read as if
one job had played it. Repair and wave aggregation share this one copy.

Imported only; plan_repair.py's self-test exercises it.
"""

import re
import shutil
import tempfile
from pathlib import Path

import plan_arms
import pool_pentanomial


class Refused(Exception):
    pass


def verify_piece(directory, start, pairs, run, names, book):
    """The piece's pair counts, after the batch's checks of its PGN and of its log's total.

    A piece never carries an arm: the repair planner refuses multi-arm runs.
    """
    try:
        with tempfile.TemporaryDirectory() as root:
            shutil.copytree(directory, Path(root) / f"strength-{run}-shard-0")
            plan_arms.verify_batch([], 1, pairs, start - 1, *names, str(book), root)
        return pool_pentanomial.read_shard(str(Path(directory) / "match.log"))
    except (plan_arms.PlanError, ValueError, OSError) as error:
        raise Refused(f"{directory}: {error}") from error


def join_slices(pieces, target):
    """Join verified contiguous pieces, given in planned order as (directory, pairs, counts), into target.

    Rounds are renumbered 1..total and the summed Ptnml is the log's last line, which the pool reads.
    No cpu.tsv: the pieces ran on different runners, so the report's per-shard figures would describe no
    one shard. The samples stay in the pieces.
    """
    total = [0, 0, 0, 0, 0]
    pgn = []
    logs = []
    played = 0
    for directory, pairs, counts in pieces:
        directory = Path(directory)
        total = [a + b for a, b in zip(total, counts)]
        base = played
        pgn.append(re.sub(r'^\[Round "(\d+)"\]', lambda m: f'[Round "{int(m.group(1)) + base}"]',
                          (directory / "match.pgn").read_text(encoding="utf-8"), flags=re.M))
        logs.append((directory / "match.log").read_text(encoding="utf-8", errors="replace"))
        played += pairs
    target = Path(target)
    target.mkdir(parents=True)
    (target / "match.pgn").write_text("".join(pgn), encoding="utf-8")
    (target / "match.log").write_text("".join(logs) + f"\nPtnml(0-2): {total}\n", encoding="utf-8")
    return played, total
