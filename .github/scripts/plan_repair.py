#!/usr/bin/env python3
"""Repair a lab wave that lost a shard to its hosted runner, inside the same run.

  plan_repair.py plan      decide from the wave's jobs and artifacts whether to replay, and which pieces
  plan_repair.py assemble  verify the replayed pieces and build the wave slices they replace

A wave is the run's unit of repair: a fixed-size run is one wave, whose slices are its whole shards
(sprt.py). A lost shard's range is its planned wave slice, and the next wave starts only after this
wave's repair, so a speculative wave never competes with the repair for job slots.

A shard is lost only when its job failed, the runner-loss annotation is its only failure annotation,
and it left no artifact. A toolkit failure also leaves no artifact, and an artifact means the runner
outlived the games. Every other failed shard discards the batch, as does a job listing that does not
hold exactly one finished record per shard. A lost job's log is gone, so a fatal line played before
the loss cannot be seen: an accepted risk, since a runner-caused time loss only taints games that get
replaced, and an engine bug common enough to move Elo also shows in the other shards.

Each piece is checked alone and the pieces of a shard are joined (batch_slices.py) into the wave
slice's own artifact name, so the workflow's verify and pool see the designed wave and count a replay
as the one slice it replaces. The raw pieces keep their replay artifact names (sprt.py names both).

The limit of 2 lost shards counts per run, across waves (--prior-lost). It is an alarm that the run's
infrastructure is wrong, not a budget. If losses come per runner-hour a waved run sees as many as a
fixed one; if they come per job, 3 or more losses in a waved run's 72 jobs have probability 0.0253%
at the inherited 1 in 600 (strength-lab.md). A per-wave limit would allow 8 before the alarm.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import batch_slices
import pool_pentanomial
import sprt

LOSS_ANNOTATION = "The hosted runner lost communication with the server"
# Three independent losses in a run essentially never happen, so they mean something is wrong with
# the run, and it stops for a person. Rates: strength-lab.md.
MAX_LOST_SHARDS = 2

Refused = batch_slices.Refused


def job_re(prefix):
    """A wave's shard jobs: a called workflow's jobs are listed as "<caller job> / <job>"."""
    return re.compile(re.escape(prefix) + r"match \((\d+)\)")


def lost_shards(jobs, annotations, artifact_names, run, shards, wave, waves, prefix, prior_lost):
    """The wave's lost shards, or raise Refused naming why the wave cannot be repaired."""
    pattern = job_re(prefix)
    records = {}
    for job in jobs:
        match = pattern.fullmatch(job.get("name", ""))
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
        if sprt.slice_artifact(run, wave, waves, shard) in artifact_names:
            raise Refused(f"shard {shard} left an artifact, so its runner outlived the games")
        lost.append(shard)
    if not lost:
        raise Refused("no shard job failed")
    if prior_lost + len(lost) > MAX_LOST_SHARDS:
        earlier = f" ({prior_lost} in earlier waves)" if prior_lost else ""
        raise Refused(f"{prior_lost + len(lost)} shards lost in the run{earlier}, "
                      f"more than the {MAX_LOST_SHARDS} repaired automatically")
    if len(lost) == shards:
        raise Refused("every shard was lost")
    return lost


def planned_by_shard(planned):
    """The wave's planned slices by shard, refusing a plan that is not one slice for each of 0..S-1."""
    by_shard = {piece["shard"]: piece for piece in planned}
    if len(by_shard) != len(planned) or set(by_shard) != set(range(len(planned))):
        raise Refused("the wave plan does not hold one slice for each shard")
    return by_shard


def plan_slices(lost, planned):
    """Contiguous pieces covering each lost shard's planned slice, sizes within one pair, never more jobs
    than the wave."""
    by_shard = planned_by_shard(planned)
    slices = []
    for shard in lost:
        pairs = by_shard[shard]["pairs"]
        count = min(len(planned) // len(lost), pairs)
        size, extra = divmod(pairs, count)
        start = by_shard[shard]["start"]
        for index in range(count):
            pairs = size + (1 if index < extra else 0)
            slices.append({"shard": shard, "index": index, "start": start, "pairs": pairs})
            start += pairs
    return slices


def plan(jobs, annotations, artifact_names, run, planned, arms, wave, waves, prefix, prior_lost):
    """The planner's verdict: status `repair` with slices, or `discard` with the reason."""
    try:
        if arms.strip():
            raise Refused("a multi-arm run is not repaired automatically")
        planned_by_shard(planned)
        lost = lost_shards(jobs, annotations, artifact_names, run, len(planned), wave, waves, prefix, prior_lost)
    except Refused as error:
        return {"status": "discard", "reason": str(error), "lost": [], "slices": []}
    slices = plan_slices(lost, planned)
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


def fetch_inventory(repo, run, prefix, read=gh_lines):
    jobs = read(f"repos/{repo}/actions/runs/{run}/jobs?filter=latest&per_page=100",
                ".jobs[] | {id, name, status, conclusion, check_run_url}")
    annotations = {}
    pattern = job_re(prefix)
    for job in jobs:
        if pattern.fullmatch(job.get("name", "")) and job.get("conclusion") != "success":
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


def slices_by_shard(slices, planned):
    """Each lost shard's pieces in order, refusing any that do not cover its planned slice exactly."""
    plan_for = planned_by_shard(planned)
    by_shard = {}
    for piece in sorted(slices, key=lambda piece: (piece["shard"], piece["index"])):
        by_shard.setdefault(piece["shard"], []).append(piece)
    for shard, pieces in by_shard.items():
        if shard not in plan_for:
            raise Refused(f"shard {shard} is not in the wave plan")
        expected = plan_for[shard]["start"]
        for piece in pieces:
            if piece["start"] != expected:
                raise Refused(f"shard {shard} slice {piece['index']} starts at {piece['start']}, expected {expected}")
            expected += piece["pairs"]
        if expected != plan_for[shard]["start"] + plan_for[shard]["pairs"]:
            raise Refused(f"the slices of shard {shard} do not cover its {plan_for[shard]['pairs']} planned pairs")
    return by_shard


def assemble(slices, planned, run, wave, waves, names, book, replays, shard_root):
    """Build each lost shard's wave-slice artifact from its verified replayed pieces."""
    for shard, pieces in slices_by_shard(slices, planned).items():
        target = Path(shard_root) / sprt.slice_artifact(run, wave, waves, shard)
        if target.exists():
            raise Refused(f"{target} exists, but shard {shard} was planned as lost")
        verified = []
        for piece in pieces:
            directory = Path(replays) / sprt.replay_artifact(run, wave, waves, shard, piece["index"])
            if not directory.is_dir():
                raise Refused(f"{directory} is missing")
            counts = batch_slices.verify_piece(directory, piece["start"], piece["pairs"], run, names, book)
            verified.append((directory, piece["pairs"], counts))
        played, total = batch_slices.join_slices(verified, target)
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

    # The jobs API lists a called workflow's job as "<caller job> / <job>".
    def job(shard, conclusion="success", status="completed", prefix="wave-1 / ", base=100):
        return {"id": base + shard, "name": f"{prefix}match ({shard})", "status": status, "conclusion": conclusion,
                "check_run_url": f"https://api.github.com/repos/o/r/check-runs/{base + shard}"}

    def note(message, level="failure"):
        return {"annotation_level": level, "message": message}

    def verdict(failed=None, artifacts=None, jobs=None, arms="", shards=18, rounds=558, offset=0,
                wave=1, waves=1, prior_lost=0):
        """failed maps a shard to (conclusion, its annotations); the rest succeeded and uploaded."""
        failed = failed or {}
        prefix = f"wave-{wave} / "
        listed = jobs if jobs is not None else [
            job(shard, failed[shard][0] if shard in failed else "success", prefix=prefix) for shard in range(shards)]
        notes = {100 + shard: failed[shard][1] for shard in failed}
        names = {sprt.slice_artifact(1, wave, waves, shard) for shard in range(shards) if shard not in failed}
        planned = sprt.wave_plan(shards, rounds, offset, waves)[wave - 1]
        return plan(listed, notes, names | set(artifacts or ()), 1, planned, arms, wave, waves, prefix, prior_lost)

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
    discards("an unprefixed job listing is not this wave's", "shard 0",
             jobs=[job(s, "failure" if s == 6 else "success", prefix="") for s in range(18)])

    # Waves: a lost shard's range is its planned wave slice, its artifact the wave slice's.
    wave_two = verdict({6: lost}, rounds=738, wave=2, waves=4)
    expect("a lost wave slice is replayed from its own planned range",
           wave_two["status"] == "repair" and wave_two["slices"][0]["start"] == 6 * 738 + 185 + 1
           and sum(s["pairs"] for s in wave_two["slices"]) == 184 and len(wave_two["slices"]) == 18)
    other_waves = [job(s, "failure" if s == 6 else "success", prefix="wave-1 / ", base=300) for s in range(18)]
    expect("another wave's failed jobs are ignored",
           verdict({6: lost}, rounds=738, wave=2, waves=4, jobs=other_waves + [
               job(s, "failure" if s == 6 else "success", prefix="wave-2 / ") for s in range(18)])["status"] == "repair")
    discards("the wave slice's own artifact discards", "left an artifact", {6: lost}, rounds=738, wave=2, waves=4,
             artifacts={"strength-1-wave-2-shard-6"})
    expect("another wave's artifact of the shard does not",
           verdict({6: lost}, rounds=738, wave=2, waves=4,
                   artifacts={"strength-1-wave-1-shard-6", "strength-1-shard-6"})["status"] == "repair")
    expect("one earlier loss and one now repairs", verdict({6: lost}, prior_lost=1)["status"] == "repair")
    discards("the limit counts the run's earlier losses", "3 shards lost in the run (1 in earlier waves)",
             {6: lost, 7: lost}, prior_lost=1)
    discards("two earlier losses leave none to repair", "3 shards lost in the run (2 in earlier waves)",
             {6: lost}, prior_lost=2)
    for bad in ([{"shard": 0, "start": 1, "pairs": 5}, {"shard": 0, "start": 6, "pairs": 5}],
                [{"shard": 1, "start": 1, "pairs": 5}]):
        result = plan([job(0, "failure")], {100: [note(loss)]}, set(), 1, bad, "", 1, 1, "wave-1 / ", 0)
        expect(f"a malformed wave plan discards {bad}", result["status"] == "discard" and "wave plan" in result["reason"])

    for shards, rounds, lost_list, waves, wave in [
            (18, 558, [6], 1, 1), (18, 740, [6], 1, 1), (20, 900, [0], 1, 1), (18, 7, [17], 1, 1),
            (6, 36, [1], 1, 1), (3, 10, [0, 2], 1, 1), (18, 41, [2, 3], 1, 1),
            (18, 738, [6], 4, 2), (18, 738, [0, 17], 4, 4), (2, 7, [1], 4, 3)]:
        planned = sprt.wave_plan(shards, rounds, 5, waves)[wave - 1]
        slices = plan_slices(lost_list, planned)
        for shard in lost_list:
            pieces = [s for s in slices if s["shard"] == shard]
            sizes = {s["pairs"] for s in pieces}
            slices_by_shard(pieces, planned)
            expect(f"{shards}x{rounds} wave {wave}/{waves} lost {shard}: covers, even, within slots",
                   sum(sizes_ := [s["pairs"] for s in pieces]) == planned[shard]["pairs"]
                   and pieces[0]["start"] == planned[shard]["start"] and max(sizes) - min(sizes) <= 1
                   and sizes_ == sorted(sizes_, reverse=True) and len(slices) <= shards)

    pages = []

    def fake_read(path, _jq):
        pages.append(path)
        if "/jobs" in path:
            return [job(0), job(1, "failure"), job(0, "failure", prefix="wave-2 / ", base=200)]
        if "/annotations" in path:
            return [note(loss)]
        return [{"name": "strength-1-shard-0"}]

    jobs, notes, names = fetch_inventory("o/r", 1, "wave-1 / ", read=fake_read)
    expect("annotations are read only for this wave's failed shards",
           notes == {101: [note(loss)]} and len(pages) == 3)
    expect("the job's own check run is read", any(p.startswith("repos/o/r/check-runs/101/annotations") for p in pages))
    expect("the inventory plans", plan(jobs, notes, names, 1, sprt.wave_plan(2, 4, 0, 1)[0], "",
                                       1, 1, "wave-1 / ", 0)["lost"] == [1])

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

    def write_slice(root, shard, index, opening, pairs, counts, candidate=names[0], waves=1, wave=1):
        directory = Path(root) / "replays" / sprt.replay_artifact(1, wave, waves, shard, index)
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

    def attempt(mutate=None, slices=good, waves=1):
        with tempfile.TemporaryDirectory() as root:
            book = Path(root) / "book.epd"
            book.write_text("\n".join(book_lines) + "\n", encoding="utf-8")
            write_slice(root, 1, 0, 4, 2, [0, 0, 1, 1, 0], waves=waves)
            write_slice(root, 1, 1, 6, 1, [0, 0, 0, 1, 0], waves=waves)
            (Path(root) / "shards").mkdir()
            if mutate:
                mutate(Path(root))
            # 3 shards x 3 pairs at offset 1; at waves > 1 only the names differ.
            planned = sprt.wave_plan(3, 3, 1, 1)[0]
            assemble(slices, planned, 1, 1, waves, names, book, Path(root) / "replays", Path(root) / "shards")
            target = Path(root) / "shards" / sprt.slice_artifact(1, 1, waves, 1)
            return ((target / "match.pgn").read_text(encoding="utf-8"), (target / "match.log").read_text(encoding="utf-8"),
                    (target / "cpu.tsv").exists())

    pgn, log, has_cpu = attempt()
    rounds_seen = [int(n) for n in re.findall(r'\[Round "(\d+)"\]', pgn)]
    expect("rounds run 1..R across slices", rounds_seen == [1, 1, 2, 2, 3, 3])
    expect("the summed counts are the log's last line", pool_pentanomial.parse_counts(log) == [0, 0, 1, 2, 0])
    expect("the raw logs are kept", log.count("Results of") == 2)
    expect("the assembled log names one revision pair", pool_pentanomial.parse_revisions(log) == ("abc1234", "abc1234"))
    expect("no CPU samples from mixed runners", not has_cpu)
    waved_pgn, waved_log, _ = attempt(waves=4)
    expect("a waved run assembles under the wave slice's names", waved_pgn == pgn and waved_log == log)

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

    def wave_arguments(command):
        command.add_argument("--slices", required=True, help="this wave's planned slices from sprt.py plan, as JSON")
        command.add_argument("--run", required=True)
        command.add_argument("--wave", type=int, required=True)
        command.add_argument("--waves", type=int, required=True)

    planning = commands.add_parser("plan", help="classify the wave's failed shards and plan the replay")
    wave_arguments(planning)
    planning.add_argument("--repo", required=True)
    planning.add_argument("--job-prefix", required=True, help='the caller job name and " / ", e.g. "wave-1 / "')
    planning.add_argument("--prior-lost", type=int, required=True, help="shards lost in the run's earlier waves")
    planning.add_argument("--arms", default="")
    planning.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    assembling = commands.add_parser("assemble", help="verify the pieces and build the wave slices they replace")
    wave_arguments(assembling)
    assembling.add_argument("--pieces", required=True, help="the planner's replay pieces (its slices output), as JSON")
    assembling.add_argument("--candidate-name", required=True)
    assembling.add_argument("--reference-name", required=True)
    assembling.add_argument("--book", required=True)
    assembling.add_argument("--replays", required=True)
    assembling.add_argument("--shards", required=True)

    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.command == "plan":
        jobs, annotations, names = fetch_inventory(args.repo, args.run, args.job_prefix)
        result = plan(jobs, annotations, names, args.run, json.loads(args.slices), args.arms, args.wave, args.waves,
                      args.job_prefix, args.prior_lost)
        write_outputs(result, args.output)
        print(json.dumps(result, indent=2))
        return 0
    if args.command == "assemble":
        try:
            assemble(json.loads(args.pieces), json.loads(args.slices), args.run, args.wave, args.waves,
                     (args.candidate_name, args.reference_name), args.book, args.replays, args.shards)
        except Refused as error:
            print(f"REFUSED: {error}", file=sys.stderr)
            return 2
        return 0
    parser.error("a command is required")


if __name__ == "__main__":
    sys.exit(main())
