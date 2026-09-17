"""Check that per-engine UCI options the strength lab was dispatched with will
actually take effect.

The engine follows the usual UCI convention and ignores an unknown `setoption`
name, a malformed value and an out-of-domain value alike, in silence
(`UCIHandler.cpp:552-555`). fastchess passes `option.X=Y` through without
knowing what the engine supports, so a typo or an out-of-range value produces a
measurement that ran for three hours against an engine configured exactly like
its reference -- a null result indistinguishable from a real one. That is the
failure the strength lab's header calls worse than no measurement at all, so
the options are checked against the engine's own advertised option table before
any shard starts.

Usage:
    validate_uci_options.py --engine <binary> --label candidate --options "A=1 B=true"
    validate_uci_options.py --self-test

An empty option string is valid and checks nothing. The engine is asked for its
table with `uci`, so this costs one process start per side.
"""

import argparse
import re
import subprocess
import sys

# "option name X type spin default D min L max H" / "... type check default true"
_SPIN = re.compile(r"^option name (\S+) type spin default (-?\d+) min (-?\d+) max (-?\d+)\s*$")
_CHECK = re.compile(r"^option name (\S+) type check default (true|false)\s*$")

# Set by the harness itself on both engine command lines; accepting it here would
# leave which of the two wins up to fastchess's argument order.
RESERVED = ("Threads",)


def parse_option_table(uci_output):
    """{name: ("spin", lo, hi)} / {name: ("check", None, None)} from an engine's `uci` reply."""
    table = {}
    for line in uci_output.splitlines():
        line = line.strip()
        spin = _SPIN.match(line)
        if spin:
            table[spin.group(1)] = ("spin", int(spin.group(3)), int(spin.group(4)))
            continue
        check = _CHECK.match(line)
        if check:
            table[check.group(1)] = ("check", None, None)
    return table


def validate(options, table, label):
    """Returns a list of human-readable problems; empty means every option will take effect."""
    problems = []
    for item in options.split():
        if "=" not in item:
            problems.append(f"{label}: '{item}' is not Name=Value")
            continue
        name, value = item.split("=", 1)
        if name in RESERVED:
            problems.append(f"{label}: '{name}' is set by the harness on both engines and may not be overridden")
            continue
        if name not in table:
            known = ", ".join(sorted(table)) or "none"
            problems.append(f"{label}: the engine advertises no option '{name}'. It advertises: {known}")
            continue
        kind, lo, hi = table[name]
        if kind == "check":
            if value not in ("true", "false"):
                problems.append(f"{label}: '{name}' is a check option; '{value}' is not true or false")
            continue
        try:
            number = int(value)
        except ValueError:
            problems.append(f"{label}: '{name}' is a spin option; '{value}' is not an integer")
            continue
        if not lo <= number <= hi:
            problems.append(f"{label}: '{name}'={number} is outside the engine's advertised range [{lo}, {hi}]")
    return problems


def engine_option_table(engine):
    result = subprocess.run(
        [engine, "uci"],
        input="uci\nquit\n",
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    return parse_option_table(result.stdout)


SELF_TEST_UCI = """id name StratChessEvolved
option name Threads type spin default 1 min 1 max 32
option name Hash type spin default 64 min 1 max 4096
option name Contempt type spin default 0 min -100 max 100
option name ReverseFutility type check default true
uciok
"""


def self_test():
    table = parse_option_table(SELF_TEST_UCI)
    cases = [
        ("", [], "an empty option string checks nothing"),
        ("Contempt=20", [], "an in-range spin value"),
        ("Contempt=-100", [], "the lower bound is inclusive"),
        ("Contempt=100 ReverseFutility=false", [], "several options at once"),
        ("Contempt=101", ["outside the engine's advertised range"], "above the advertised maximum"),
        ("Contempt=-101", ["outside the engine's advertised range"], "below the advertised minimum"),
        ("Contmept=20", ["advertises no option"], "a misspelled name is the failure this exists for"),
        ("Contempt=high", ["is not an integer"], "a non-numeric spin value"),
        ("ReverseFutility=1", ["is not true or false"], "a check option takes true/false, not 1"),
        ("Threads=4", ["set by the harness"], "the reserved option"),
        ("Contempt", ["is not Name=Value"], "a bare name"),
    ]
    failures = 0
    for options, expected, description in cases:
        problems = validate(options, table, "candidate")
        ok = len(problems) == len(expected) and all(
            fragment in problem for fragment, problem in zip(expected, problems)
        )
        if not ok:
            failures += 1
            print(f"FAIL: {description}\n  options: {options!r}\n  got: {problems}")
        else:
            print(f"ok: {description}")

    if len(table) != 4:
        failures += 1
        print(f"FAIL: the option table parser found {len(table)} options in the sample, expected 4")

    print(f"\n{len(cases) + 1} checks, {failures} failed")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", help="Engine binary to ask for its option table")
    parser.add_argument("--options", default="", help="Whitespace-separated Name=Value pairs")
    parser.add_argument("--label", default="engine", help="Which side these options belong to, for the message")
    parser.add_argument("--self-test", action="store_true", help="Run the parser and validator checks and exit")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.engine:
        parser.error("--engine is required unless --self-test is given")

    if not args.options.strip():
        print(f"{args.label}: no UCI options requested")
        return 0

    table = engine_option_table(args.engine)
    if not table:
        print(f"::error::{args.label}: the engine advertised no options at all; it did not answer `uci` as expected")
        return 1

    problems = validate(args.options, table, args.label)
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1

    print(f"{args.label}: every requested option is advertised and in range -- {args.options}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
