#!/usr/bin/env python3
"""Verify and join the contiguous pieces of one lab shard.

A lost shard's pairs are replayed as several pieces. Each piece is checked alone by the batch's own
verify and log reader, then the pieces are joined into one shard directory that the batch-level
verify and pool read as if one job had played it. A waved run's shard is joined the same way from
its wave slices, each already verified by its own wave's decide.

  batch_slices.py join   join waves 1..k of every shard under the shard's own artifact name

plan_repair.py's self-test exercises the pieces; test_strength_workflow.py the join.
"""

import argparse
import json
import re
import sys
import shutil
import tempfile
from pathlib import Path

import plan_arms
import pool_pentanomial
import sprt


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


def join_waves(plan, waves_played, run, slices_root, target_root):
    """Join each shard's slices of waves 1..k into target_root under its pre-wave artifact name.

    Each slice's log must hold its planned pairs, and the root may hold no slice outside waves 1..k:
    either would join something other than the decided batch.
    """
    waves = len(plan)
    if not 1 <= waves_played <= waves:
        raise Refused(f"wave {waves_played} is outside 1..{waves}")
    slices_root = Path(slices_root)
    wanted = {sprt.slice_artifact(run, wave, waves, piece["shard"])
              for wave in range(1, waves_played + 1) for piece in plan[wave - 1]}
    extra = sorted({path.name for path in slices_root.iterdir()} - wanted)
    if extra:
        raise Refused(f"{slices_root} holds slices outside waves 1..{waves_played}: {extra}")
    for shard in range(len(plan[0])):
        pieces = []
        for wave in range(1, waves_played + 1):
            name = sprt.slice_artifact(run, wave, waves, shard)
            pairs = plan[wave - 1][shard]["pairs"]
            try:
                counts = pool_pentanomial.read_shard(str(slices_root / name / "match.log"), pairs)
            except (ValueError, OSError) as error:
                raise Refused(str(error)) from error
            pieces.append((slices_root / name, pairs, counts))
        join_slices(pieces, Path(target_root) / sprt.slice_artifact(run, 1, 1, shard))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    joining = commands.add_parser("join", help="join waves 1..k of every shard")
    joining.add_argument("--wave-plan", required=True, help="sprt.py plan's whole plan, as JSON")
    joining.add_argument("--waves-played", type=int, required=True, help="the deciding wave k")
    joining.add_argument("--run", required=True)
    joining.add_argument("--slices", required=True, help="the downloaded wave slices")
    joining.add_argument("--target", required=True, help="where the joined shards are written")
    args = parser.parse_args()
    try:
        join_waves(json.loads(args.wave_plan), args.waves_played, args.run, args.slices, args.target)
    except (Refused, ValueError) as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1
    print(f"Joined waves 1..{args.waves_played} of {len(json.loads(args.wave_plan)[0])} shards into {args.target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
