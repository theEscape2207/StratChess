#!/usr/bin/env python3
"""Route the strength lab's shards to candidate arms, and group their logs back.

A multi-arm run screens several candidate option sets in one batch. Each arm is
an option set in the candidate_uci_options syntax; arms are separated by ';'.
Shard i plays arm i mod K, so every arm is spread through the run's openings
instead of holding one contiguous slice of them. Each arm is pooled on its own
shards only, so an arm is just a smaller batch against the shared reference.

  plan_arms.py matrix --arms STR --shards N   JSON list, one {label, options} per shard
  plan_arms.py list   --arms STR              one "LABEL<TAB>OPTIONS" line per arm
  plan_arms.py logs   --arms STR --shards N --arm LABEL LOG...
                                              the logs of LABEL's shards, one per line

An empty --arms is the single-candidate run: matrix gives every shard an empty
label, and list prints nothing.

Validate with --self-test, which checks the routing against fixture logs with
distinct pentanomial counts per shard.
"""

import argparse
import json
import os
import re
import string
import sys
import tempfile

import pool_pentanomial

OPTION_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*=\S+$")
SHARD_LOG_RE = re.compile(r"shard-(\d+)[/\\]match\.log$")


class PlanError(Exception):
    pass


def parse_arms(text):
    """Arms as a list of option strings, refusing anything ambiguous."""
    if not text.strip():
        return []
    arms = [arm.strip() for arm in text.split(";")]
    seen = []
    for index, arm in enumerate(arms):
        tokens = arm.split()
        if not tokens:
            raise PlanError(f"arm {index + 1} is empty")
        names = {}
        for token in tokens:
            if not OPTION_RE.match(token):
                raise PlanError(f"arm {index + 1}: '{token}' is not Name=Value")
            name, value = token.split("=", 1)
            if name in names:
                raise PlanError(f"arm {index + 1} sets {name} twice")
            names[name] = value
        # Option order is irrelevant to the engine, so it is irrelevant here.
        if names in seen:
            raise PlanError(f"arm {index + 1} repeats an earlier arm")
        seen.append(names)
    if len(arms) > len(string.ascii_uppercase):
        raise PlanError(f"{len(arms)} arms; at most {len(string.ascii_uppercase)}")
    return [" ".join(arm.split()) for arm in arms]


def label(index):
    return string.ascii_uppercase[index]


def check_shards(arms, shards):
    if arms and shards % len(arms):
        raise PlanError(f"{shards} shards do not divide evenly among {len(arms)} arms")


def matrix(arms, shards):
    check_shards(arms, shards)
    if not arms:
        return [{"label": "", "options": ""} for _ in range(shards)]
    return [{"label": label(i % len(arms)), "options": arms[i % len(arms)]}
            for i in range(shards)]


def logs_for_arm(arms, shards, wanted, logs):
    """The logs of one arm's shards, refusing a missing or doubled shard."""
    check_shards(arms, shards)
    by_shard = {}
    for path in logs:
        match = SHARD_LOG_RE.search(path)
        if not match:
            raise PlanError(f"{path}: no shard index in the path")
        shard = int(match.group(1))
        if shard >= shards or shard in by_shard:
            raise PlanError(f"{path}: shard {shard} is out of range or seen twice")
        by_shard[shard] = path
    missing = sorted(set(range(shards)) - set(by_shard))
    if missing:
        raise PlanError(f"no log for shard(s) {missing}; refusing to pool a partial batch")
    labels = [label(i) for i in range(len(arms))]
    if wanted not in labels:
        raise PlanError(f"no arm '{wanted}'")
    return [by_shard[s] for s in range(shards) if label(s % len(arms)) == wanted]


def self_test():
    failures = []

    def expect(name, condition):
        print(f"{'ok      ' if condition else 'FAIL    '}{name}")
        if not condition:
            failures.append(name)

    def raises(name, fn):
        try:
            fn()
        except PlanError:
            expect(name, True)
        else:
            expect(name, False)

    arms = parse_arms("SingularMarginFactor=1 ;SingularMarginFactor=4; SingularMinDepth=10")
    expect("three arms parsed", arms == ["SingularMarginFactor=1", "SingularMarginFactor=4",
                                         "SingularMinDepth=10"])
    expect("empty arms is the single-candidate run",
           matrix(parse_arms(""), 2) == [{"label": "", "options": ""}] * 2)
    expect("shards interleave A B C A B C",
           [m["label"] for m in matrix(arms, 6)] == list("ABCABC"))
    expect("each shard carries its arm's options",
           matrix(arms, 6)[4]["options"] == "SingularMarginFactor=4")

    raises("empty arm refused", lambda: parse_arms("A=1;;B=2"))
    raises("trailing ';' refused", lambda: parse_arms("A=1;"))
    raises("bare name refused", lambda: parse_arms("A"))
    raises("option set twice in one arm refused", lambda: parse_arms("A=1 A=2"))
    raises("identical arms refused", lambda: parse_arms("A=1 B=2;B=2 A=1"))
    raises("uneven shards refused", lambda: matrix(arms, 4))

    # Distinct counts per shard, so a log routed to the wrong arm changes a sum.
    counts = {s: [s, 10 + s, 20 + s, 30 + s, 40 + s] for s in range(6)}
    with tempfile.TemporaryDirectory() as root:
        logs = []
        for shard, c in counts.items():
            directory = os.path.join(root, f"strength-1-shard-{shard}")
            os.makedirs(directory)
            path = os.path.join(directory, "match.log")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(f"Ptnml(0-2): [{', '.join(map(str, c))}]\n")
            logs.append(path)

        for arm, shard_ids in (("A", [0, 3]), ("B", [1, 4]), ("C", [2, 5])):
            routed = logs_for_arm(arms, 6, arm, logs)
            ids = [int(SHARD_LOG_RE.search(p).group(1)) for p in routed]
            total = [sum(col) for col in zip(*(pool_pentanomial.read_shard(p) for p in routed))]
            want = [sum(col) for col in zip(*(counts[s] for s in shard_ids))]
            expect(f"arm {arm} gets shards {shard_ids} and their summed counts",
                   ids == shard_ids and total == want)

        raises("missing shard log refused", lambda: logs_for_arm(arms, 6, "A", logs[:-1]))
        raises("doubled shard log refused", lambda: logs_for_arm(arms, 6, "A", logs + logs[:1]))
        raises("unknown arm refused", lambda: logs_for_arm(arms, 6, "D", logs))

    print("\nself-test:", "FAIL" if failures else "PASS")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", choices=["matrix", "list", "logs"])
    parser.add_argument("logs", nargs="*", help="shard logs, for the logs command")
    parser.add_argument("--arms", default="")
    parser.add_argument("--shards", type=int, default=0)
    parser.add_argument("--arm", default="")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.command:
        parser.error("no command given")

    try:
        arms = parse_arms(args.arms)
        if args.command == "matrix":
            print(json.dumps(matrix(arms, args.shards)))
        elif args.command == "list":
            for index, arm in enumerate(arms):
                print(f"{label(index)}\t{arm}")
        else:
            print("\n".join(logs_for_arm(arms, args.shards, args.arm, args.logs)))
    except PlanError as error:
        print(f"::error::{error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
