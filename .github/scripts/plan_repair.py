#!/usr/bin/env python3
"""Repair a lab batch that lost a shard to its hosted runner, inside the same run.

  plan_repair.py plan      decide from the run's jobs and artifacts whether to replay, and which slices
  plan_repair.py assemble  verify the replayed slices and build the shard directories they replace

A shard is lost only when its job failed, the runner-loss annotation is its only failure annotation,
and it left no artifact. A toolkit failure also leaves no artifact, and an artifact means the runner
outlived the games. Every other failed shard discards the batch, as does a job listing that does not
hold exactly one finished record per shard. A lost job's log is gone (404 on all three seen), so a
fatal line played before the loss cannot be seen: an accepted risk, since a runner-caused time loss
only taints games that get replaced, and an engine bug common enough to move Elo also shows in the
other shards. Rates behind MAX_LOST_SHARDS: 3 losses in the 100 runs since 2026-08-07, all single.

Each slice is checked alone by the unchanged verify_batch and read_shard, then the slices of a shard
are joined into strength-<run>-shard-<N>, so the workflow's verify and pool see the designed batch
and count a replay as the one shard it replaces. The raw slices stay as strength-<run>-replay-<N>-<i>.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import plan_arms
import pool_pentanomial

LOSS_ANNOTATION = "The hosted runner lost communication with the server"
# Two independent losses in a batch are about 1 in 2,400 batches, three essentially never: three or
# more means something is wrong with the run, so it stops for a person.
MAX_LOST_SHARDS = 2
JOB_RE = re.compile(r"match \((\d+)\)")


class Refused(Exception):
    pass


def lost_shards(jobs, annotations, artifact_names, run, shards):
    """The lost shards, or raise Refused naming why the batch cannot be repaired."""
    records = {}
    for job in jobs:
        match = JOB_RE.fullmatch(job.get("name", ""))
        if match:
            records.setdefault(int(match.group(1)), []).append(job)
    for shard in range(shards):
        found = records.get(shard, [])
        if len(found) != 1 or found[0].get("status") != "completed":
            raise Refused(f"expected one finished job for shard {shard}, found {len(found)}")
    if set(records) != set(range(shards)):
        raise Refused(f"job listing has shards beyond 0..{shards - 1}")

    lost = []
    for shard in range(shards):
        job = records[shard][0]
        if job["conclusion"] == "success":
            continue
        if job["conclusion"] != "failure":
            raise Refused(f"shard {shard} concluded {job['conclusion']}")
        failures = [note["message"] for note in annotations.get(job["id"], [])
                    if note.get("annotation_level") == "failure"]
        if len(failures) != 1 or not failures[0].startswith(LOSS_ANNOTATION):
            raise Refused(f"shard {shard} failed without the runner-loss annotation alone")
        if f"strength-{run}-shard-{shard}" in artifact_names:
            raise Refused(f"shard {shard} left an artifact, so its runner outlived the games")
        lost.append(shard)
    if not lost:
        raise Refused("no shard job failed")
    if len(lost) > MAX_LOST_SHARDS:
        raise Refused(f"{len(lost)} shards lost, more than the {MAX_LOST_SHARDS} repaired automatically")
    if len(lost) == shards:
        raise Refused("every shard was lost")
    return lost


def plan_slices(lost, shards, rounds, offset):
    """Contiguous slices covering each lost shard, sizes within one pair, never more jobs than the batch."""
    count = min(shards // len(lost), rounds)
    slices = []
    for shard in lost:
        size, extra = divmod(rounds, count)
        start = offset + shard * rounds + 1
        for index in range(count):
            pairs = size + (1 if index < extra else 0)
            slices.append({"shard": shard, "index": index, "start": start, "pairs": pairs})
            start += pairs
    return slices


def plan(jobs, annotations, artifact_names, run, shards, rounds, offset, arms):
    """The planner's verdict: status `repair` with slices, or `discard` with the reason."""
    try:
        if arms.strip():
            raise Refused("a multi-arm run is not repaired automatically")
        lost = lost_shards(jobs, annotations, artifact_names, run, shards)
    except Refused as error:
        return {"status": "discard", "reason": str(error), "lost": [], "slices": []}
    slices = plan_slices(lost, shards, rounds, offset)
    return {"status": "repair", "reason": "", "lost": lost, "slices": slices}


def replayed_text(lost, slices):
    """The report's row: names the replay, so a repaired batch cannot pass for an ordinary one."""
    if len(lost) == 1:
        return f"shard {lost[0]}, {len(slices)} slices (runner lost)"
    return f"shards {' and '.join(str(shard) for shard in lost)}, {len(slices)} slices (runners lost)"


def gh_lines(path, jq):
    """Every page of a listing; a partial one must never read as an absent job or artifact."""
    output = subprocess.run(["gh", "api", "--paginate", path, "--jq", jq],
                            check=True, capture_output=True, text=True).stdout
    return [json.loads(line) for line in output.splitlines() if line.strip()]


def fetch_inventory(repo, run, read=gh_lines):
    jobs = read(f"repos/{repo}/actions/runs/{run}/jobs?filter=latest&per_page=100",
                ".jobs[] | {id, name, status, conclusion, check_run_url}")
    annotations = {}
    for job in jobs:
        if JOB_RE.fullmatch(job.get("name", "")) and job.get("conclusion") != "success":
            path = job["check_run_url"].removeprefix("https://api.github.com/")
            annotations[job["id"]] = read(f"{path}/annotations?per_page=100", ".[] | {annotation_level, message}")
    artifacts = read(f"repos/{repo}/actions/runs/{run}/artifacts?per_page=100", ".artifacts[] | {name}")
    return jobs, annotations, {artifact["name"] for artifact in artifacts}


def write_outputs(result, path):
    """lost_a and lost_b rather than a list, because the workflow uploads each assembled shard in its own step."""
    lost = [str(shard) for shard in result["lost"]] + ["", ""]
    text = replayed_text(result["lost"], result["slices"]) if result["lost"] else ""
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(f"status={result['status']}\n")
        handle.write(f"reason={result['reason']}\n")
        handle.write(f"slices={json.dumps(result['slices'], separators=(',', ':'))}\n")
        handle.write(f"lost_a={lost[0]}\nlost_b={lost[1]}\n")
        handle.write(f"replayed={text}\n")


def check_coverage(slices, rounds, offset):
    """The slices of each shard must cover its pairs exactly and in order."""
    by_shard = {}
    for piece in sorted(slices, key=lambda piece: (piece["shard"], piece["index"])):
        by_shard.setdefault(piece["shard"], []).append(piece)
    for shard, pieces in by_shard.items():
        expected = offset + shard * rounds + 1
        for piece in pieces:
            if piece["start"] != expected:
                raise Refused(f"shard {shard} slice {piece['index']} starts at {piece['start']}, expected {expected}")
            expected += piece["pairs"]
        if expected != offset + (shard + 1) * rounds + 1:
            raise Refused(f"the slices of shard {shard} do not cover its {rounds} pairs")
    return by_shard


def verified_counts(piece, directory, run, names, book):
    """The slice's pair counts, after the batch's own checks of its PGN and of its log."""
    try:
        counts = pool_pentanomial.read_shard(str(directory / "match.log"), piece["pairs"])
        with tempfile.TemporaryDirectory() as root:
            shutil.copytree(directory, Path(root) / f"strength-{run}-shard-0")
            plan_arms.verify_batch([], 1, piece["pairs"], piece["start"] - 1, *names, str(book), root)
    except (plan_arms.PlanError, ValueError, OSError) as error:
        raise Refused(f"{directory}: {error}") from error
    return counts


def assemble(slices, run, rounds, offset, names, book, replays, shard_root):
    """Build strength-<run>-shard-<N> from each lost shard's verified slices."""
    for shard, pieces in check_coverage(slices, rounds, offset).items():
        target = Path(shard_root) / f"strength-{run}-shard-{shard}"
        if target.exists():
            raise Refused(f"{target} exists, but shard {shard} was planned as lost")
        total = [0, 0, 0, 0, 0]
        pgn = []
        logs = []
        played = 0
        for piece in pieces:
            directory = Path(replays) / f"strength-{run}-replay-{shard}-{piece['index']}"
            if not directory.is_dir():
                raise Refused(f"{directory} is missing")
            counts = verified_counts(piece, directory, run, names, book)
            total = [a + b for a, b in zip(total, counts)]
            base = played
            pgn.append(re.sub(r'^\[Round "(\d+)"\]', lambda m: f'[Round "{int(m.group(1)) + base}"]',
                              (directory / "match.pgn").read_text(encoding="utf-8"), flags=re.M))
            logs.append((directory / "match.log").read_text(encoding="utf-8", errors="replace"))
            played += piece["pairs"]
        target.mkdir(parents=True)
        (target / "match.pgn").write_text("".join(pgn), encoding="utf-8")
        # The pool reads a log's last Ptnml line. No cpu.tsv: these samples come from several runners,
        # so the report's per-shard figures would describe no one shard. They stay in the slices.
        (target / "match.log").write_text("".join(logs) + f"\nPtnml(0-2): {total}\n", encoding="utf-8")
        print(f"Assembled shard {shard} from {len(pieces)} verified slices, {played} pairs: {total}")


def self_test():
    failures = []

    def expect(name, condition):
        print(f"{'ok      ' if condition else 'FAIL    '}{name}")
        if not condition:
            failures.append(name)

    # Shape and annotation text as captured from run 41631693815, whose match (6) lost its runner.
    loss = ("The hosted runner lost communication with the server. Anything in your workflow that terminates the "
            "runner process, starves it for CPU/Memory, or blocks its network access can cause this error.")
    fatal = "Shard 2: harness/engine failures detected -- batch is not measurable."

    def job(shard, conclusion="success", status="completed"):
        return {"id": 100 + shard, "name": f"match ({shard})", "status": status, "conclusion": conclusion,
                "check_run_url": f"https://api.github.com/repos/o/r/check-runs/{100 + shard}"}

    def note(message, level="failure"):
        return {"annotation_level": level, "message": message}

    def verdict(failed=None, artifacts=None, jobs=None, arms="", shards=18, rounds=558, offset=0):
        """failed maps a shard to (conclusion, its annotations); the rest succeeded and uploaded."""
        failed = failed or {}
        listed = jobs if jobs is not None else [
            job(shard, failed[shard][0] if shard in failed else "success") for shard in range(shards)]
        notes = {100 + shard: failed[shard][1] for shard in failed}
        names = {f"strength-1-shard-{shard}" for shard in range(shards) if shard not in failed}
        return plan(listed, notes, names | set(artifacts or ()), 1, shards, rounds, offset, arms)

    lost = ("failure", [note(loss)])
    one = verdict({6: lost})
    expect("one lost shard repairs", one["status"] == "repair" and one["lost"] == [6])
    expect("18 slices of 31 pairs from the shard's own opening",
           len(one["slices"]) == 18 and {s["pairs"] for s in one["slices"]} == {31}
           and one["slices"][0]["start"] == 6 * 558 + 1 and one["slices"][1]["start"] == 6 * 558 + 32)
    offsetted = verdict({6: lost}, offset=100)
    expect("the opening offset moves the slices", offsetted["slices"][0]["start"] == 100 + 6 * 558 + 1)
    two = verdict({3: lost, 9: lost})
    expect("two lost shards share the jobs", two["lost"] == [3, 9] and len(two["slices"]) == 18
           and sum(s["pairs"] for s in two["slices"] if s["shard"] == 9) == 558)
    expect("a warning beside the loss still repairs",
           verdict({6: ("failure", [note(loss), note("PV warning", "warning")])})["status"] == "repair")

    def discards(name, reason, *args, **options):
        result = verdict(*args, **options)
        expect(name, result["status"] == "discard" and reason in result["reason"] and not result["slices"])

    discards("three lost discard", "more than", {1: lost, 2: lost, 3: lost})
    discards("all lost discard", "every shard", {0: lost, 1: lost}, shards=2, rounds=4)
    discards("a fatal shard discards", "shard 2 failed", {2: ("failure", [note(fatal)])})
    discards("a fatal shard beside a lost one discards", "shard 2 failed", {2: ("failure", [note(fatal)]), 6: lost})
    discards("no annotation (toolkit failure) discards", "shard 6 failed", {6: ("failure", [])})
    discards("a reworded annotation discards", "shard 6 failed", {6: ("failure", [note("Runner went away")])})
    discards("loss and fatal on one job discard", "shard 6 failed", {6: ("failure", [note(loss), note(fatal)])})
    discards("the annotation with an artifact discards", "left an artifact", {6: lost},
             artifacts={"strength-1-shard-6"})
    discards("a timed-out or cancelled shard discards", "concluded cancelled", {6: ("cancelled", [])})
    discards("multi-arm discards", "multi-arm", {6: lost}, arms="X=1;X=2")
    discards("no failed shard discards", "no shard job failed")
    every = [job(shard) for shard in range(18)]
    discards("a missing job discards", "shard 5", jobs=every[:5] + every[6:])
    discards("a duplicate beside a missing job discards", "shard 4", jobs=every[:5] + [every[4]] + every[6:])
    discards("an unfinished job discards", "shard 0", jobs=[job(0, None, "in_progress")] + every[1:])
    discards("a job beyond the batch discards", "beyond", jobs=every + [job(18)])
    expect("other jobs in the listing are ignored",
           verdict({6: lost}, jobs=[{"id": 1, "name": "build", "status": "completed", "conclusion": "success"}]
                   + [job(s, "failure" if s == 6 else "success") for s in range(18)])["status"] == "repair")

    for shards, rounds, lost_list in [(18, 558, [6]), (18, 740, [6]), (20, 900, [0]), (18, 7, [17]),
                                      (6, 36, [1]), (3, 10, [0, 2]), (18, 41, [2, 3])]:
        slices = plan_slices(lost_list, shards, rounds, 5)
        for shard in lost_list:
            pieces = [s for s in slices if s["shard"] == shard]
            sizes = {s["pairs"] for s in pieces}
            check_coverage(pieces, rounds, 5)
            expect(f"{shards}x{rounds} lost {shard}: covers, even, within slots",
                   sum(sizes_ := [s["pairs"] for s in pieces]) == rounds and max(sizes) - min(sizes) <= 1
                   and sizes_ == sorted(sizes_, reverse=True) and len(slices) <= shards)

    pages = []

    def fake_read(path, _jq):
        pages.append(path)
        if "/jobs" in path:
            return [job(0), job(1, "failure")]
        if "/annotations" in path:
            return [note(loss)]
        return [{"name": "strength-1-shard-0"}]

    jobs, notes, names = fetch_inventory("o/r", 1, fake_read)
    expect("annotations are read only for failed shards", notes == {101: [note(loss)]} and len(pages) == 3)
    expect("the job's own check run is read", any(p.startswith("repos/o/r/check-runs/101/annotations") for p in pages))
    expect("the inventory plans", plan(jobs, notes, names, 1, 2, 4, 0, "")["lost"] == [1])

    expect("the report names one replay", replayed_text([6], one["slices"]) == "shard 6, 18 slices (runner lost)")
    expect("the report names two", replayed_text([3, 9], two["slices"]) == "shards 3 and 9, 18 slices (runners lost)")
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "output"
        write_outputs(two, path)
        written = dict(line.split("=", 1) for line in path.read_text(encoding="utf-8").splitlines())
        expect("outputs carry both shards and the slices", written["lost_a"] == "3" and written["lost_b"] == "9"
               and json.loads(written["slices"]) == two["slices"] and written["status"] == "repair")

    book_lines = [f"8/8/8/8/8/8/{rank}/4k3 w - -" for rank in ["4K3", "3K4", "2K5", "1K6", "K7", "5K2", "6K1", "7K"]]
    names = ("candidate-abc1234", "reference-abc1234")

    def write_slice(root, shard, index, opening, pairs, counts, candidate=names[0]):
        directory = Path(root) / "replays" / f"strength-1-replay-{shard}-{index}"
        directory.mkdir(parents=True)
        pgn = ""
        for number in range(1, pairs + 1):
            for white, black in [(candidate, names[1]), (names[1], candidate)]:
                pgn += (f'[Event "fixture"]\n[Round "{number}"]\n[White "{white}"]\n[Black "{black}"]\n'
                        f'[FEN "{book_lines[opening + number - 1]} 0 9"]\n[Result "1/2-1/2"]\n\n1/2-1/2\n\n')
        (directory / "match.pgn").write_text(pgn, encoding="utf-8")
        (directory / "match.log").write_text(
            f"Results of {candidate} vs {names[1]} (10+0.1, 1t, 192MB, book.epd):\nPtnml(0-2): {counts}\n",
            encoding="utf-8")
        (directory / "cpu.tsv").write_text("elapsed_s\n", encoding="utf-8")

    # Shard 1 of 3 shards of 3 pairs at offset 1: openings 4, 5, 6 (0-based) replayed as slices of 2 and 1.
    good = [{"shard": 1, "index": 0, "start": 5, "pairs": 2}, {"shard": 1, "index": 1, "start": 7, "pairs": 1}]

    def attempt(mutate=None, slices=good):
        with tempfile.TemporaryDirectory() as root:
            book = Path(root) / "book.epd"
            book.write_text("\n".join(book_lines) + "\n", encoding="utf-8")
            write_slice(root, 1, 0, 4, 2, [0, 0, 1, 1, 0])
            write_slice(root, 1, 1, 6, 1, [0, 0, 0, 1, 0])
            (Path(root) / "shards").mkdir()
            if mutate:
                mutate(Path(root))
            assemble(slices, 1, 3, 1, names, book, Path(root) / "replays", Path(root) / "shards")
            target = Path(root) / "shards" / "strength-1-shard-1"
            return ((target / "match.pgn").read_text(encoding="utf-8"), (target / "match.log").read_text(encoding="utf-8"),
                    (target / "cpu.tsv").exists())

    pgn, log, has_cpu = attempt()
    rounds_seen = [int(n) for n in re.findall(r'\[Round "(\d+)"\]', pgn)]
    expect("rounds run 1..R across slices", rounds_seen == [1, 1, 2, 2, 3, 3])
    expect("the summed counts are the log's last line", pool_pentanomial.parse_counts(log) == [0, 0, 1, 2, 0])
    expect("the raw logs are kept", log.count("Results of") == 2)
    expect("the assembled log names one revision pair", pool_pentanomial.parse_revisions(log) == ("abc1234", "abc1234"))
    expect("no CPU samples from mixed runners", not has_cpu)

    def refused(name, reason, mutate=None, slices=good):
        try:
            attempt(mutate, slices)
        except Refused as error:
            expect(name, reason in str(error))
        else:
            expect(name, False)

    def replace(index, *rest, **options):
        def mutate(root):
            shutil.rmtree(root / "replays" / f"strength-1-replay-1-{index}")
            write_slice(root, 1, index, *rest, **options)
        return mutate

    def missing_slice(root):
        shutil.rmtree(root / "replays" / "strength-1-replay-1-1")

    def existing_target(root):
        (root / "shards" / "strength-1-shard-1").mkdir()

    def no_counts(root):
        (root / "replays" / "strength-1-replay-1-1" / "match.log").write_text("no counts\n", encoding="utf-8")

    def compensating(root):
        # Each PGN is sound, and the totals still reach R: only the per-slice log check catches this.
        (root / "replays" / "strength-1-replay-1-0" / "match.log").write_text("Ptnml(0-2): [0, 0, 1, 0, 0]\n", encoding="utf-8")
        (root / "replays" / "strength-1-replay-1-1" / "match.log").write_text("Ptnml(0-2): [0, 0, 2, 0, 0]\n", encoding="utf-8")

    refused("a missing slice is refused", "is missing", missing_slice)
    refused("a slice at the wrong opening is refused", "round 1 FEN", replace(1, 0, 1, [0, 0, 0, 1, 0]))
    refused("a wrong engine name is refused", "expected candidate-abc1234",
            replace(1, 6, 1, [0, 0, 0, 1, 0], candidate="candidate-def5678"))
    refused("a short slice is refused", "expected 1 pairs", replace(1, 6, 1, [0, 0, 0, 2, 0]))
    refused("compensating slice logs are refused", "expected 2 pairs", compensating)
    refused("a slice log with no counts is refused", "no Ptnml", no_counts)
    refused("a target that already exists is refused", "planned as lost", existing_target)
    refused("slices that leave a gap are refused", "starts at", None,
            [good[0], {"shard": 1, "index": 1, "start": 8, "pairs": 1}])
    refused("slices that fall short are refused", "do not cover", None, [good[0]])

    print("Repair planner self-test " + ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")

    planning = commands.add_parser("plan", help="classify the run's failed shards and plan the replay")
    planning.add_argument("--repo", required=True)
    planning.add_argument("--run", required=True)
    planning.add_argument("--shards", type=int, required=True)
    planning.add_argument("--rounds", type=int, required=True)
    planning.add_argument("--opening-offset", type=int, required=True)
    planning.add_argument("--arms", default="")
    planning.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    assembling = commands.add_parser("assemble", help="verify the slices and build the shards they replace")
    assembling.add_argument("--slices", required=True, help="the planner's slices, as JSON")
    assembling.add_argument("--run", required=True)
    assembling.add_argument("--rounds", type=int, required=True)
    assembling.add_argument("--opening-offset", type=int, required=True)
    assembling.add_argument("--candidate-name", required=True)
    assembling.add_argument("--reference-name", required=True)
    assembling.add_argument("--book", required=True)
    assembling.add_argument("--replays", required=True)
    assembling.add_argument("--shards", required=True)

    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.command == "plan":
        jobs, annotations, names = fetch_inventory(args.repo, args.run)
        result = plan(jobs, annotations, names, args.run, args.shards, args.rounds, args.opening_offset, args.arms)
        write_outputs(result, args.output)
        print(json.dumps(result, indent=2))
        return 0
    if args.command == "assemble":
        try:
            assemble(json.loads(args.slices), args.run, args.rounds, args.opening_offset,
                     (args.candidate_name, args.reference_name), args.book, args.replays, args.shards)
        except Refused as error:
            print(f"REFUSED: {error}", file=sys.stderr)
            return 2
        return 0
    parser.error("a command is required")
    return 2


if __name__ == "__main__":
    sys.exit(main())
