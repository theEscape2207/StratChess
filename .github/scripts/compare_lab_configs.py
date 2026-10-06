#!/usr/bin/env python3
"""Resolve and retain the strength lab's intended comparison before shards start.

Only advertised spin/check domains are supported. Equal tracked engine/build
inputs or equal staged bytes, equal resolved options and equal time controls
require an explicit calibration declaration. Different inputs permit a code
comparison; they do not prove different chess behaviour.
"""

import argparse
from decimal import Decimal
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from plan_arms import PlanError, label, parse_arms
from validate_uci_options import engine_option_table, resolve_options, SELF_TEST_UCI

BUILD_INPUTS = ("CMakeLists.txt", "cmake", "StratEngine", "StratChessEvolved")
DECIMAL = r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)"
TIME_CONTROL = re.compile(rf"({DECIMAL})\+({DECIMAL})")
LIMITATION = (
    "These are intended configurations validated against advertisement, domains and "
    "value syntax, not runtime readback or proof that setoption was applied. "
    "Cross-field engine validation can still reject individually valid settings."
)


def normalize_time_control(text):
    match = TIME_CONTROL.fullmatch(text.strip())
    if not match:
        raise ValueError(f"unsupported time control {text!r}; expected decimal seconds+increment")
    base, increment = (Decimal(value) for value in match.groups())
    if not base.is_finite() or not increment.is_finite() or base <= 0 or increment < 0:
        raise ValueError(f"invalid time control {text!r}; base must be > 0 and increment >= 0")
    return base, increment


def decimal_text(number):
    text = format(number, "f")
    return text.rstrip("0").rstrip(".") if "." in text else text


def time_control_text(control):
    return "+".join(decimal_text(number) for number in control)


def git_output(repo, *arguments):
    result = subprocess.run(["git", "-C", str(repo), *arguments], capture_output=True,
                            timeout=30, check=False)
    if result.returncode:
        raise ValueError(f"git {' '.join(arguments)} failed: "
                         f"{result.stderr.decode('utf-8', errors='replace').strip()}")
    return result.stdout


def source_identity(repo, revision):
    """Resolve a full commit and compare actual path/mode/object entries, not SHAs."""
    sha = git_output(repo, "rev-parse", "--verify", "--end-of-options",
                     f"{revision}^{{commit}}").decode("ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", sha):
        raise ValueError(f"could not resolve full revision for {revision!r}")
    tree = git_output(repo, "ls-tree", "-r", "-z", "--full-tree", sha, "--", *BUILD_INPUTS)
    entries = []
    for record in tree.split(b"\0"):
        if record:
            metadata, path = record.split(b"\t", 1)
            mode, kind, object_id = metadata.split()
            entries.append((path, mode, kind, object_id))
    if not entries or not any(entry[0] == b"CMakeLists.txt" for entry in entries):
        raise ValueError(f"{sha}: engine/build input entries or CMakeLists.txt are missing")
    return sha, tuple(entries)


def binary_hash(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def maps_equal(left, right):
    # bool and int compare equal in Python; UCI check and spin values do not.
    return (left.keys() == right.keys()
            and all(type(left[name]) is type(right[name]) and left[name] == right[name]
                    for name in left))


def value_text(value):
    return str(value).lower() if isinstance(value, bool) else str(value)


def markdown(value):
    return str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")


def settings_table(settings):
    return ["| Option | Resolved value |", "|---|---|"] + [
        f"| {markdown(name)} | {value_text(value)} |" for name, value in sorted(settings.items())]


def compare(args):
    """Collect evidence independently, retaining partial failures without fabricated defaults."""
    problems, warnings = [], []
    sides = {}
    for name in ("candidate", "reference"):
        side = {"requested_revision": getattr(args, f"{name}_revision"),
                "engine": getattr(args, name)}
        sides[name] = side
        try:
            side["revision"], side["entries"] = source_identity(args.repo, side["requested_revision"])
            side["hash"] = binary_hash(side["engine"])
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            problems.append(f"{name}: identity acquisition failed: {error}")
        try:
            side["tc"] = normalize_time_control(getattr(args, f"{name}_tc"))
        except ValueError as error:
            problems.append(f"{name}: {error}")
        side["table"], error = engine_option_table(side["engine"], name)
        if error:
            problems.append(error)

    arms = []
    try:
        if args.arms.strip() and args.candidate_options.strip():
            raise PlanError("candidate-options and arms may not both be supplied")
        parsed = parse_arms(args.arms)
        arms = [{"label": label(index), "options": options} for index, options in enumerate(parsed)]
        if not arms:
            arms = [{"label": "candidate", "options": args.candidate_options}]
    except PlanError as error:
        problems.append(str(error))

    reference = sides["reference"]
    if reference["table"]:
        reference["settings"], errors, notices = resolve_options(
            args.reference_options, reference["table"], "reference", args.threads)
        problems.extend(errors)
        warnings.extend(notices)
    for arm in arms:
        if sides["candidate"]["table"]:
            arm["settings"], errors, notices = resolve_options(
                arm["options"], sides["candidate"]["table"], f"arm {arm['label']}", args.threads)
            problems.extend(errors)
            warnings.extend(notices)
    for index, arm in enumerate(arms):
        if arm.get("settings") is not None:
            for previous in arms[:index]:
                if previous.get("settings") is not None and maps_equal(arm["settings"], previous["settings"]):
                    problems.append(f"arm {arm['label']} duplicates resolved settings of arm {previous['label']}; "
                                    "calibration does not permit duplicate arms")

    complete = not problems
    source_equal = binary_equal = None
    if all("entries" in side and "hash" in side for side in sides.values()):
        source_equal = sides["candidate"]["entries"] == reference["entries"]
        binary_equal = sides["candidate"]["hash"] == reference["hash"]
    if complete:
        for arm in arms:
            arm["options_equal"] = maps_equal(arm["settings"], reference["settings"])
            arm["tc_equal"] = sides["candidate"]["tc"] == reference["tc"]
            arm["identical"] = (source_equal or binary_equal) and arm["options_equal"] and arm["tc_equal"]
            if arm["identical"] and not args.calibration:
                problems.append(f"arm {arm['label']}: identical intended comparison; declare calibration "
                                "explicitly to run this null control")

    lines = ["## Intended strength-lab comparison", "", LIMITATION, "",
             f"Preflight: **{'REFUSED' if problems else 'PASSED'}**. "
             f"Resolved comparison: **{'complete' if complete else 'incomplete'}**.",
             f"Calibration declared: **{'yes' if args.calibration else 'no'}**.", "",
             f"Shared CMake definitions: `{markdown(args.cmake_defines or '(none)')}`.",
             f"Shared toolchain / Release recipe: `{markdown(args.toolchain)}`.",
             f"Definitions and harness-owned Threads={args.threads} are shared and cannot distinguish the sides.", "",
             "| Evidence | Candidate | Reference |", "|---|---|---|"]
    for title, key in (("Revision", "revision"), ("Binary SHA-256", "hash")):
        lines.append(f"| {title} | {markdown(sides['candidate'].get(key, 'unavailable'))} | "
                     f"{markdown(reference.get(key, 'unavailable'))} |")
    lines.append(f"| Time control (requested) | {markdown(args.candidate_tc)} | {markdown(args.reference_tc)} |")
    lines.append("| Time control (normalized) | " + " | ".join(
        time_control_text(side["tc"]) if "tc" in side else "unavailable" for side in sides.values()) + " |")
    lines += ["", "Source/build input entries: " + ("equal" if source_equal else "different")
              if source_equal is not None else "Source/build input entries: unavailable",
              "Staged binary bytes: " + ("equal" if binary_equal else "different")
              if binary_equal is not None else "Staged binary bytes: unavailable",
              "Identity paths: CMakeLists.txt, cmake/, StratEngine/, StratChessEvolved/.",
              "Different inputs/bytes permit a code comparison; they do not prove different chess behaviour."]
    if all(side["table"] for side in sides.values()):
        lines += ["", "### Advertisement differences", "",
                  "| Option | Candidate domain/default | Reference domain/default |", "|---|---|---|"]
        differences = 0
        for name in sorted(sides["candidate"]["table"].keys() | reference["table"].keys()):
            candidate_domain = sides["candidate"]["table"].get(name)
            reference_domain = reference["table"].get(name)
            if candidate_domain != reference_domain:
                differences += 1
                lines.append(f"| {name} | {markdown(candidate_domain or 'not advertised')} | "
                             f"{markdown(reference_domain or 'not advertised')} |")
        if not differences:
            lines.append("| (none) | equal | equal |")
    lines += ["", "### Reference settings", "", f"Requested overrides: `{markdown(args.reference_options or '(none)')}`.", ""]
    lines += settings_table(reference["settings"]) if reference.get("settings") is not None else ["Unavailable: query or validation failed."]
    for arm in arms:
        lines += ["", f"### Arm {arm['label']}", "", f"Requested overrides: `{markdown(arm['options'] or '(none)')}`.", ""]
        if arm.get("settings") is None:
            lines.append("Unavailable: query or validation failed.")
            continue
        lines += settings_table(arm["settings"])
        if "identical" not in arm:
            lines += ["", "Comparison incomplete; no identity classification made."]
            continue
        conditions = []
        if not (source_equal or binary_equal):
            conditions.append("source/build inputs and binary bytes differ")
        if not arm["tc_equal"]:
            conditions.append("time control differs")
        if not arm["options_equal"]:
            conditions.append("resolved options differ")
        lines += ["", "Classification: **" + ("identical intended conditions" if arm["identical"]
                  else "; ".join(conditions)) + "**.", "",
                  "| Setting difference | Candidate | Reference |", "|---|---|---|"]
        for name in sorted(arm["settings"].keys() | reference["settings"].keys()):
            if (name not in arm["settings"] or name not in reference["settings"]
                    or type(arm["settings"][name]) is not type(reference["settings"][name])
                    or arm["settings"][name] != reference["settings"][name]):
                lines.append(f"| {name} | {value_text(arm['settings'].get(name, 'not advertised'))} | "
                             f"{value_text(reference['settings'].get(name, 'not advertised'))} |")
        if arm["options_equal"]:
            lines.append("| (none) | equal | equal |")
    if problems:
        lines += ["", "### Refusal diagnostics", ""] + [f"- {markdown(problem)}" for problem in problems]
    if warnings:
        lines += ["", "### Validation notices", ""] + [f"- {markdown(warning)}" for warning in warnings]
    return "\n".join(lines) + "\n", problems, warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("candidate", "reference", "candidate-revision", "reference-revision", "output"):
        parser.add_argument(f"--{name}")
    parser.add_argument("--repo", default=".")
    for name in ("candidate-options", "reference-options", "arms", "cmake-defines"):
        parser.add_argument(f"--{name}", default="")
    parser.add_argument("--candidate-tc", default="10+0.1")
    parser.add_argument("--reference-tc", default="10+0.1")
    parser.add_argument("--toolchain", default="clang-cl Release")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--calibration", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    for name in ("candidate", "reference", "candidate_revision", "reference_revision", "output"):
        if not getattr(args, name):
            parser.error(f"--{name.replace('_', '-')} is required unless --self-test is given")
    report, problems, warnings = compare(args)
    try:
        Path(args.output).write_text(report, encoding="utf-8")
    except OSError as error:
        print(f"::error::could not retain comparison: {error}", file=sys.stderr)
        return 1
    for warning in warnings:
        print(f"::warning::{warning}")
    for problem in problems:
        print(f"::error::{problem}", file=sys.stderr)
    if not problems:
        print(f"Comparison preflight passed; retained {args.output}")
    return 1 if problems else 0


def self_test():
    """Drive the production CLI against subprocess engines and real Git histories."""
    failures = []
    checks = 0

    def expect(name, condition):
        nonlocal checks
        checks += 1
        print(f"{'ok' if condition else 'FAIL'}: {name}")
        if not condition:
            failures.append(name)

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        repo = root / "repo"
        repo.mkdir()
        git_output(repo, "init")
        git_output(repo, "config", "user.name", "Comparison fixture")
        git_output(repo, "config", "user.email", "fixture@example.invalid")
        git_output(repo, "config", "core.autocrlf", "false")
        for relative in ("CMakeLists.txt", "cmake/pins.cmake", "StratEngine/search.h",
                         "StratChessEvolved/main.cpp", "Docs/fixture.md"):
            path = repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("initial\n", encoding="utf-8")

        def commit():
            git_output(repo, "add", ".")
            git_output(repo, "-c", "commit.gpgsign=false", "commit", "-m", "fixture")
            return git_output(repo, "rev-parse", "HEAD").decode("ascii").strip()

        original = commit()
        (repo / "Docs/fixture.md").write_text("docs-only\n", encoding="utf-8")
        docs = commit()
        code_revisions = []
        for relative in ("StratEngine/search.h", "CMakeLists.txt", "cmake/pins.cmake",
                         "StratChessEvolved/main.cpp"):
            (repo / relative).write_text("changed\n", encoding="utf-8")
            code_revisions.append(commit())
        expect("docs-only revisions retain equal engine/build entries",
               source_identity(repo, original)[1] == source_identity(repo, docs)[1])
        for revision, description in zip(code_revisions, ("engine header", "CMake", "dependency pin", "CLI source")):
            expect(f"{description} changes identity", source_identity(repo, docs)[1] != source_identity(repo, revision)[1])

        script = root / "stub.py"
        script.write_text(
            "import pathlib, sys\n"
            "root = pathlib.Path(__file__).parent\n"
            "side = sys.argv[1]\n"
            "commands = sys.stdin.read()\n"
            "assert commands == 'uci\\nquit\\n'\n"
            "with (root / (side + '.calls')).open('a') as handle: handle.write('query\\n')\n"
            "print((root / (side + '.uci')).read_text())\n"
            "sys.exit(int((root / (side + '.status')).read_text()))\n", encoding="utf-8")
        engines = {}
        for side in ("candidate", "reference"):
            path = root / (side + (".cmd" if os.name == "nt" else ".sh"))
            if os.name == "nt":
                wrapper = f'@echo off\n"{sys.executable}" "{script}" {side}\n'
            else:
                import shlex
                wrapper = f"#!/bin/sh\nexec {shlex.quote(sys.executable)} {shlex.quote(str(script))} {side}\n"
            path.write_text(wrapper, encoding="utf-8")
            path.chmod(0o755)
            engines[side] = path
            (root / f"{side}.uci").write_text(SELF_TEST_UCI, encoding="utf-8")
            (root / f"{side}.status").write_text("0", encoding="utf-8")

        def run(name, success, fragments=(), **overrides):
            values = dict(candidate=engines["candidate"], reference=engines["reference"],
                          candidate_revision=docs, reference_revision=original, repo=repo,
                          candidate_options="", reference_options="", arms="",
                          candidate_tc="10+0.10", reference_tc="10.0+0.1",
                          cmake_defines="-DFIXTURE=ON", toolchain="fixture Release", output=root / "comparison.md")
            values.update(overrides)
            for side in engines:
                (root / f"{side}.calls").write_text("", encoding="utf-8")
            arguments = [sys.executable, str(Path(__file__).resolve())]
            for key, value in values.items():
                if key == "calibration":
                    if value:
                        arguments.append("--calibration")
                else:
                    arguments.append(f"--{key.replace('_', '-')}={value}")
            result = subprocess.run(arguments, capture_output=True, text=True, timeout=30, check=False)
            report = values["output"].read_text(encoding="utf-8") if values["output"].exists() else ""
            condition = ((result.returncode == 0) == success and LIMITATION in report
                         and all(fragment in report for fragment in fragments))
            expect(name, condition)
            if not condition:
                print(result.stdout, result.stderr, report)
            return report

        run("default-on vs empty defaults refuses with retained complete comparison", False,
            ("identical intended comparison", "Resolved comparison: **complete**", "Source/build input entries: equal", "Staged binary bytes: different"),
            candidate_options="ReverseFutility=true")
        expect("both empty/default override sides queried exactly once",
               all((root / f"{side}.calls").read_text().splitlines() == ["query"] for side in engines))
        run("order, redundant defaults and zero padding cannot defeat equality", False,
            ("identical intended comparison",), candidate_options="ReverseFutility=true Hash=064",
            reference_options="Hash=64 ReverseFutility=true")
        run("a real option difference passes across unequal docs-only revisions", True,
            ("resolved options differ", "| ReverseFutility | false | true |"), candidate_options="ReverseFutility=false")
        run("unequal time control passes", True, ("time control differs",), candidate_tc="5+0.1")
        run("declared null calibration passes", True, ("Calibration declared: **yes**", "identical intended conditions"), calibration=True)
        run("declared known-sign calibration passes", True, ("time control differs",), calibration=True, candidate_tc="5+0.1")
        run("distinct code/build entries with unequal binary bytes pass", True,
            ("source/build inputs and binary bytes differ",), candidate_revision=code_revisions[-1])
        run("equal binaries override unequal source identity", False, ("identical intended comparison", "Staged binary bytes: equal"),
            reference=engines["candidate"], candidate_revision=code_revisions[-1])
        run("duplicate resolved arms fail even in calibration", False, ("duplicates resolved settings",),
            arms="Hash=65; Hash=065 ReverseFutility=true", calibration=True)
        run("all multi-arm settings and classifications retained", True,
            ("### Arm A", "### Arm B", "| Hash | 65 |", "| Hash | 66 |", "resolved options differ"), arms="Hash=65;Hash=66")
        expect("multi-arm run queries each side once, not once per arm",
               all((root / f"{side}.calls").read_text().splitlines() == ["query"] for side in engines))
        run("mixed null and differing arms require calibration", False, ("arm A: identical intended comparison",),
            arms="Hash=64;Hash=65")
        run("mixed calibration classifies each arm separately", True,
            ("identical intended conditions", "resolved options differ"), arms="Hash=64;Hash=65", calibration=True)
        run("ambiguous candidate and arms rejected", False, ("may not both be supplied",), arms="Hash=65", candidate_options="Hash=66")
        run("harness-owned Threads reaches both resolved sides", True,
            ("harness-owned Threads=4", "| Threads | 4 |"), candidate_options="Hash=65", threads=4)
        run("harness-owned Threads outside the advertised domain fails closed", False,
            ("must admit harness-owned Threads=33",), candidate_options="Hash=65", threads=33)

        older = SELF_TEST_UCI.replace("option name ReverseFutility type check default true\n", "")
        (root / "reference.uci").write_text(older, encoding="utf-8")
        run("older table omits candidate-only options without fabricated defaults", True,
            ("not advertised", "resolved options differ"))
        run("older table rejects override of absent option", False,
            ("advertises no option", "Resolved comparison: **incomplete**"), reference_options="ReverseFutility=false")
        (root / "reference.uci").write_text(SELF_TEST_UCI, encoding="utf-8")
        for value in ("0+0.1", "10", "10+NaN", "10+-0.1", "1e1+0.1", "10+0.1+0.1"):
            run(f"unsupported time control {value} fails closed", False, ("time control",), candidate_tc=value)
        run("missing source revision fails closed and retains diagnostics", False,
            ("identity acquisition failed", "Resolved comparison: **incomplete**"), candidate_revision="not-a-revision")
        run("missing staged binary fails closed", False, ("identity acquisition failed", "could not query"), candidate=root / "missing")

        (root / "candidate.status").write_text("7", encoding="utf-8")
        run("nonzero engine exit fails despite complete reply", False, ("exited with status 7", "Unavailable: query or validation failed"))
        (root / "candidate.status").write_text("0", encoding="utf-8")
        query_cases = [
            (SELF_TEST_UCI.replace("uciok", ""), "incomplete reply", "complete uciok"),
            ("uciok\n", "empty table", "advertised no options"),
            (SELF_TEST_UCI.replace("uciok", "option name Hash type spin default 1 min 1 max 2\nuciok"), "duplicate advertisement", "duplicate advertised option"),
            (SELF_TEST_UCI.replace("uciok", "option name Book type string default none\nuciok"), "unsupported advertisement", "malformed or unsupported"),
            (SELF_TEST_UCI.replace("default 64 min 1 max 4096", "default 64 min 100 max 50"), "invalid advertised bounds", "invalid bounds/default"),
            (SELF_TEST_UCI.replace("type check default true", "type check default yes"), "malformed advertisement", "malformed or unsupported"),
        ]
        for output, name, fragment in query_cases:
            (root / "candidate.uci").write_text(output, encoding="utf-8")
            run(name + " fails closed", False, (fragment, "Resolved comparison: **incomplete**"))
        (root / "candidate.uci").write_text(SELF_TEST_UCI, encoding="utf-8")

        # The standalone validator must query even when no overrides were supplied.
        (root / "candidate.uci").write_text("uciok\n", encoding="utf-8")
        validator = Path(__file__).with_name("validate_uci_options.py")
        result = subprocess.run([sys.executable, str(validator), "--engine", str(engines["candidate"])],
                                capture_output=True, text=True, timeout=30, check=False)
        expect("standalone empty overrides fail when the engine advertises no options",
               result.returncode == 1 and "advertised no options" in result.stdout)
    expect("check true and spin 1 are distinct normalized conditions", not maps_equal({"X": True}, {"X": 1}))
    print(f"\n{checks} checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
