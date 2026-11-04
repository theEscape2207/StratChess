#!/usr/bin/env python3
"""Pool a lab batch whose lost shard was replayed by a separate repair run.

Refuses unless both runs' comparison.md match, the lost shard left no artifact, the
original run has no shard beyond the batch, every other shard passes verify_batch at its
assigned openings, and no log carries a failure strength.yml treats as fatal. comparison.md
omits concurrency, so the dispatch must repeat it. Single-candidate batches only: an arm's
suffixed engine name fails the check. Procedure and sizes:
.claude/skills/measure-strength/reference/strength-lab.md.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import plan_arms
import pool_pentanomial


class Refused(Exception):
    pass


def artifact(root, run, suffix):
    path = Path(root) / f"strength-{run}-{suffix}"
    if not path.is_dir():
        raise Refused(f"{path} is missing")
    return path


def comparison_text(root, run):
    path = artifact(root, run, "comparison") / "comparison.md"
    if not path.is_file() or path.stat().st_size == 0:
        raise Refused(f"{path} is missing or empty")
    return path.read_text(encoding="utf-8")


def engine_names(log):
    try:
        revisions = pool_pentanomial.read_revisions(str(log))
    except (ValueError, OSError) as error:
        raise Refused(f"{log}: {error}") from error
    if not revisions:
        raise Refused(f"{log} names no candidate and reference revisions")
    return f"candidate-{revisions[0]}", f"reference-{revisions[1]}"


def verify(shards, pairs, offset, names, book, root):
    try:
        plan_arms.verify_batch([], shards, pairs, offset, *names, str(book), str(root))
    except (plan_arms.PlanError, ValueError, OSError) as error:
        raise Refused(f"verify at opening offset {offset}: {error}") from error


def fatal_pattern():
    """The match job's own list, so a shard cannot pass here on a failure that turned its job red."""
    script = Path(__file__).resolve().with_name("play_shard.sh")
    match = re.search(r"^\s*fatal_re='([^']+)'$", script.read_text(encoding="utf-8"), flags=re.M)
    if not match:
        raise Refused(f"{script} defines no fatal_re")
    return re.compile(match.group(1))


def refuse_fatal_lines(logs):
    pattern = fatal_pattern()
    for log in logs:
        with open(log, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if pattern.search(line):
                    raise Refused(f"{log}: harness or engine failure: {line.strip()}")


def shard_indices(root, run):
    found = (re.fullmatch(rf"strength-{re.escape(str(run))}-shard-(\d+)", path.name) for path in Path(root).iterdir())
    return {int(match.group(1)) for match in found if match}


def pool_repaired(args):
    if not 0 <= args.lost_shard < args.shards:
        raise Refused(f"lost shard {args.lost_shard} is outside 0..{args.shards - 1}")
    if args.repair_shards <= 0 or args.pairs_per_shard % args.repair_shards:
        raise Refused(f"{args.repair_shards} repair shards do not divide {args.pairs_per_shard} pairs")
    repair_pairs = args.pairs_per_shard // args.repair_shards

    original = comparison_text(args.root, args.original_run)
    repair = comparison_text(args.root, args.repair_run)
    if original != repair:
        differing = [f"  original: {a}\n  repair:   {b}" for a, b in
                     zip(original.splitlines(), repair.splitlines()) if a != b]
        raise Refused("the two runs' comparison.md differ:\n" + "\n".join(differing[:10]))
    print("PASS: comparison.md identical (revisions, binary SHA-256, UCI settings, time controls)")

    original_shards = shard_indices(args.root, args.original_run)
    if args.lost_shard in original_shards:
        raise Refused(f"shard {args.lost_shard} has an artifact: a lost runner uploads nothing")
    beyond = sorted(shard for shard in original_shards if shard >= args.shards)
    if beyond:
        raise Refused(f"run {args.original_run} has shard(s) {beyond} beyond the {args.shards} stated")

    book = artifact(args.root, args.original_run, "comparison") / "UHO_4060_v3.epd"
    survivors = [shard for shard in range(args.shards) if shard != args.lost_shard]
    if not survivors:
        raise Refused("the batch has no surviving shard")
    names = engine_names(artifact(args.root, args.original_run, f"shard-{survivors[0]}") / "match.log")

    # verify_batch demands a complete batch, so each survivor is checked as a one-shard batch at its own offset.
    survivor_logs = []
    with tempfile.TemporaryDirectory() as temp:
        for shard in survivors:
            source = artifact(args.root, args.original_run, f"shard-{shard}")
            root = Path(temp) / str(shard)
            shutil.copytree(source, root / f"strength-{args.original_run}-shard-0")
            verify(1, args.pairs_per_shard, args.opening_offset + shard * args.pairs_per_shard, names, book, root)
            survivor_logs.append(str(source / "match.log"))
    print(f"PASS: {len(survivors)} surviving shards verified at their assigned openings")

    repair_logs = []
    with tempfile.TemporaryDirectory() as root:
        for shard in range(args.repair_shards):
            source = artifact(args.root, args.repair_run, f"shard-{shard}")
            shutil.copytree(source, Path(root) / source.name)
            repair_logs.append(str(source / "match.log"))
        verify(args.repair_shards, repair_pairs, args.opening_offset + args.lost_shard * args.pairs_per_shard,
               names, book, root)
    print(f"PASS: {args.repair_shards} repair shards verified, replaying shard {args.lost_shard}'s openings")

    logs = survivor_logs + repair_logs
    refuse_fatal_lines(logs)
    print(f"PASS: no fatal harness or engine failure in {len(logs)} logs\n")


    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool_pentanomial.py")
    returncode = subprocess.run([sys.executable, tool, *logs], check=False).returncode
    if returncode:
        return returncode

    # The pool above counts each small repair shard as a shard; the ledger counts the replay as the one it replaces.
    replay = [sum(column) for column in zip(*(pool_pentanomial.read_shard(log) for log in repair_logs))]
    tally = pool_pentanomial.shard_tally([pool_pentanomial.read_shard(log) for log in survivor_logs] + [replay])
    print(f"\nFor the ledger: {tally['candidate']} of {args.shards} shards favour the candidate, "
          "the replay as one shard")
    return 0


def self_test():
    book_lines = [f"8/8/8/8/8/8/{rank}/4k3 w - -" for rank in ["4K3", "3K4", "2K5", "1K6", "K7", "5K2", "6K1", "7K"]]
    failures = []

    def shard_dir(root, run, shard, start, pairs, counts, revision="abc1234", suffix="", extra_log=""):
        directory = Path(root) / f"strength-{run}-shard-{shard}"
        directory.mkdir(parents=True)
        candidate, reference = f"candidate-{revision}{suffix}", f"reference-{revision}"
        pgn = ""
        for round_number in range(1, pairs + 1):
            for white, black in [(candidate, reference), (reference, candidate)]:
                pgn += (f'[Event "fixture"]\n[Round "{round_number}"]\n[White "{white}"]\n[Black "{black}"]\n'
                        f'[FEN "{book_lines[start]} 0 9"]\n[Result "1/2-1/2"]\n\n1/2-1/2\n\n')
        (directory / "match.pgn").write_text(pgn, encoding="utf-8")
        (directory / "match.log").write_text(
            f"Results of {candidate} vs {reference} (10+0.1, 1t, 192MB, book.epd):\n{extra_log}Ptnml(0-2): {counts}\n",
            encoding="utf-8")

    def comparison_dir(root, run):
        directory = Path(root) / f"strength-{run}-comparison"
        directory.mkdir(parents=True)
        (directory / "comparison.md").write_text("hash abc\n", encoding="utf-8")
        (directory / "UHO_4060_v3.epd").write_text("\n".join(book_lines) + "\n", encoding="utf-8")

    # Three shards of two pairs at offset 1 with shard 1 lost: openings 1 and 5 survive, 3 and 4 replay it.
    def fixture(root):
        comparison_dir(root, 1)
        comparison_dir(root, 2)
        shard_dir(root, 1, 0, 1, 2, [0, 0, 0, 2, 0])
        shard_dir(root, 1, 2, 5, 2, [0, 2, 0, 0, 0])
        shard_dir(root, 2, 0, 3, 1, [0, 0, 0, 1, 0])
        shard_dir(root, 2, 1, 4, 1, [0, 0, 1, 0, 0])

    def arguments(root, **overrides):
        values = dict(root=root, original_run=1, repair_run=2, shards=3, pairs_per_shard=2, lost_shard=1,
                      repair_shards=2, opening_offset=1)
        values.update(overrides)
        return argparse.Namespace(**values)

    def expect(name, condition):
        print(f"{'ok      ' if condition else 'FAIL    '}{name}")
        if not condition:
            failures.append(name)

    def refused(name, reason, mutate=None, **overrides):
        with tempfile.TemporaryDirectory() as root:
            fixture(root)
            if mutate:
                mutate(Path(root))
            try:
                pool_repaired(arguments(root, **overrides))
            except Refused as error:
                expect(name, reason in str(error))
            else:
                expect(name, False)

    with tempfile.TemporaryDirectory() as root:
        fixture(root)
        expect("a complete repair pools", pool_repaired(arguments(root)) == 0)
        tool = os.path.abspath(__file__)
        command = [sys.executable, tool, "--root", root, "--original-run", "1", "--repair-run", "2", "--shards", "3",
                   "--pairs-per-shard", "2", "--lost-shard", "1", "--repair-shards", "2", "--opening-offset", "1"]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        expect("the CLI pools the designed batch", result.returncode == 0 and "6 pairs" in result.stdout)
        # Survivors: one for, one against. Replay: one pair for and one even, so for, as one shard.
        expect("the replay counts as one shard for the ledger", "2 of 3 shards" in result.stdout)

    def differing_comparison(root):
        (root / "strength-2-comparison/comparison.md").write_text("hash def\n", encoding="utf-8")

    def missing_repair_shard(root):
        shutil.rmtree(root / "strength-2-shard-1")

    def missing_survivor(root):
        shutil.rmtree(root / "strength-1-shard-2")

    def replace_shard(run, shard, *rest, **options):
        def mutate(root):
            shutil.rmtree(root / f"strength-{run}-shard-{shard}")
            shard_dir(root, run, shard, *rest, **options)
        return mutate

    def finished_lost_shard(root):
        shard_dir(root, 1, 1, 3, 2, [2, 0, 0, 0, 0])

    def extra_shard(root):
        shard_dir(root, 1, 3, 7, 2, [0, 0, 2, 0, 0])

    time_loss = "Warning; fixture loses on time\n"
    refused("differing comparison.md is refused", "comparison.md differ", differing_comparison)
    refused("a missing repair shard is refused", "strength-2-shard-1", missing_repair_shard)
    refused("a missing survivor is refused", "strength-1-shard-2", missing_survivor)
    refused("a repair at the wrong openings is refused", "round 1 FEN",
            replace_shard(2, 0, 1, 1, [0, 0, 0, 1, 0]))
    refused("a survivor at the wrong openings is refused", "round 1 FEN",
            replace_shard(1, 2, 3, 2, [0, 2, 0, 0, 0]))
    refused("a repair naming another revision is refused", "expected candidate-abc1234",
            replace_shard(2, 1, 4, 1, [0, 0, 1, 0, 0], revision="def5678"))
    refused("an arm's engine name is refused", "expected candidate-abc1234",
            replace_shard(1, 2, 5, 2, [0, 2, 0, 0, 0], suffix="-armA"))
    refused("a short shard is refused", "expected 2 pairs", replace_shard(1, 2, 5, 2, [0, 1, 0, 0, 0]))
    refused("a time loss in a survivor is refused", "loses on time",
            replace_shard(1, 2, 5, 2, [0, 2, 0, 0, 0], extra_log=time_loss))
    refused("a time loss in a repair shard is refused", "loses on time",
            replace_shard(2, 1, 4, 1, [0, 0, 1, 0, 0], extra_log=time_loss))
    refused("a finished shard cannot be replaced by a replay", "has an artifact", finished_lost_shard)
    refused("a shard beyond the stated batch is refused", "beyond the", extra_shard)
    refused("repair shards that do not divide the pairs are refused", "do not divide", repair_shards=3)
    refused("a lost shard outside the batch is refused", "outside", lost_shard=3)
    print("Repair pooling self-test " + ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", help="directory holding both runs' strength-<run>-* artifact directories")
    parser.add_argument("--original-run")
    parser.add_argument("--repair-run")
    parser.add_argument("--shards", type=int, help="shards in the designed batch")
    parser.add_argument("--pairs-per-shard", type=int, help="pairs per shard in the designed batch")
    parser.add_argument("--lost-shard", type=int)
    parser.add_argument("--repair-shards", type=int)
    parser.add_argument("--opening-offset", type=int, default=0, help="the original run's opening_offset")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    required = ["root", "original_run", "repair_run", "shards", "pairs_per_shard", "lost_shard", "repair_shards"]
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("missing: " + ", ".join(missing))
    try:
        return pool_repaired(args)
    except Refused as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
