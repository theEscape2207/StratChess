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

The check is necessary, not sufficient. It reproduces two of the engine's three
gates: the advertised per-option domain, and the unsigned-decimal value syntax
`SearchTuningSchema::read_uci` enforces. It does NOT reproduce the whole-struct
`Validate()` that `ParseUci` runs afterwards, which can still reject a value
that is individually in range for a cross-field reason
(`Code::InvalidCombination`, `SearchTuningSchema.cpp:139-152`). No UCI-exposed
field takes part in such a constraint today, so nothing can currently pass here
and be rejected there -- but a green check means "advertised, well-formed and in
range", not "certain to be applied".

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

# What SearchTuningSchema::read_uci accepts for an arithmetic field: it rejects
# any character outside this set before parsing (SearchTuningSchema.cpp:120), so
# a sign, a plus or a digit separator is refused by the engine however sensible
# it looks against the advertised min. Python's int() is strictly wider, and
# accepting one here is exactly the silent no-op this script exists to prevent.
_UNSIGNED_DECIMAL = re.compile(r"[0-9]+")

# Set by the harness itself on both engine command lines; accepting it here would
# leave which of the two wins up to fastchess's argument order.
RESERVED = ("Threads",)


def parse_option_table(uci_output):
    """{name: (kind, lo, hi, default)} from an engine's `uci` reply; lo/hi are None for a check."""
    table = {}
    for line in uci_output.splitlines():
        line = line.strip()
        spin = _SPIN.match(line)
        if spin:
            table[spin.group(1)] = ("spin", int(spin.group(3)), int(spin.group(4)), spin.group(2))
            continue
        check = _CHECK.match(line)
        if check:
            table[check.group(1)] = ("check", None, None, check.group(2))
    return table


def validate(options, table, label):
    """Returns (problems, warnings). An empty problems list means every option will take effect."""
    problems = []
    warnings = []
    seen = set()
    for item in options.split():
        if "=" not in item:
            problems.append(f"{label}: '{item}' is not Name=Value")
            continue
        name, value = item.split("=", 1)
        if name in RESERVED:
            problems.append(f"{label}: '{name}' is set by the harness on both engines and may not be overridden")
            continue
        if name in seen:
            # fastchess forwards both, the engine applies whichever arrives last, and the run's
            # configuration ends up decided by argument order rather than by the dispatch.
            problems.append(f"{label}: '{name}' is set more than once")
            continue
        seen.add(name)
        if name not in table:
            known = ", ".join(sorted(table)) or "none"
            problems.append(f"{label}: the engine advertises no option '{name}'. It advertises: {known}")
            continue
        kind, lo, hi, default = table[name]
        if kind == "check":
            if value not in ("true", "false"):
                problems.append(f"{label}: '{name}' is a check option; '{value}' is not true or false")
                continue
        else:
            if not _UNSIGNED_DECIMAL.fullmatch(value):
                problems.append(
                    f"{label}: '{name}'='{value}' is not an unsigned decimal integer. The engine's UCI "
                    f"parser accepts digits only -- no sign, plus or separator -- whatever the advertised "
                    f"minimum is, and ignores anything else in silence"
                )
                continue
            number = int(value)
            if not lo <= number <= hi:
                problems.append(f"{label}: '{name}'={number} is outside the engine's advertised range [{lo}, {hi}]")
                continue
        if value == default:
            # Not an error: a deliberate no-op is how the plumbing is smoke-tested. But an
            # accidental one configures the candidate identically to its reference and spends the
            # whole batch on a null result, which is the same outcome as a typo.
            warnings.append(f"{label}: '{name}'={value} is already the engine's default, so it changes nothing")
    return problems, warnings


def engine_option_table(engine, label):
    """(table, error). A non-None error means the engine could not be asked."""
    try:
        result = subprocess.run(
            [engine, "uci"],
            input="uci\nquit\n",
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as problem:
        # A missing, non-executable or hung binary is an engine problem; without this it would
        # surface as a Python traceback with no annotation and read as a defect in this script.
        return {}, f"{label}: could not query {engine}: {problem}"
    return parse_option_table(result.stdout), None


SELF_TEST_UCI = """id name StratChessEvolved
option name Threads type spin default 1 min 1 max 32
option name Hash type spin default 64 min 1 max 4096
option name Contempt type spin default 0 min -100 max 100
option name ReverseFutility type check default true
uciok
"""


def self_test():
    table = parse_option_table(SELF_TEST_UCI)
    # (options, expected problem fragments, expected warning fragments, description)
    cases = [
        ("", [], [], "an empty option string checks nothing"),
        ("Contempt=20", [], [], "an in-range spin value"),
        ("Contempt=100 ReverseFutility=false", [], [], "several options at once"),
        ("Contempt=101", ["outside the engine's advertised range"], [], "above the advertised maximum"),
        ("Contmept=20", ["advertises no option"], [], "a misspelled name is the failure this exists for"),
        ("Contempt=high", ["not an unsigned decimal integer"], [], "a non-numeric spin value"),
        # The engine's read_uci rejects every one of these before it looks at the advertised min,
        # so an option with a negative minimum cannot be set over UCI at all until that parser
        # learns to read a sign. The checker has to say so rather than pass the value through.
        ("Contempt=-100", ["not an unsigned decimal integer"], [], "a negative value the advertised min allows"),
        ("Contempt=+5", ["not an unsigned decimal integer"], [], "a leading plus"),
        ("Contempt=1_0", ["not an unsigned decimal integer"], [], "a digit separator int() would accept"),
        ("ReverseFutility=1", ["is not true or false"], [], "a check option takes true/false, not 1"),
        ("Threads=4", ["set by the harness"], [], "the reserved option"),
        ("Contempt", ["is not Name=Value"], [], "a bare name"),
        ("Contempt=20 Contempt=40", ["set more than once"], [], "a duplicate name, decided by argument order"),
        ("Contempt=0", [], ["is already the engine's default"], "a spin value equal to the default warns"),
        ("ReverseFutility=true", [], ["is already the engine's default"], "a check value equal to the default warns"),
        ("Contempt=101", ["outside"], [], "an out-of-range value does not also warn about the default"),
    ]
    failures = 0
    for options, expected_problems, expected_warnings, description in cases:
        problems, warnings = validate(options, table, "candidate")
        ok = (
            len(problems) == len(expected_problems)
            and all(f in p for f, p in zip(expected_problems, problems))
            and len(warnings) == len(expected_warnings)
            and all(f in w for f, w in zip(expected_warnings, warnings))
        )
        if not ok:
            failures += 1
            print(f"FAIL: {description}\n  options: {options!r}\n  problems: {problems}\n  warnings: {warnings}")
        else:
            print(f"ok: {description}")

    if len(table) != 4:
        failures += 1
        print(f"FAIL: the option table parser found {len(table)} options in the sample, expected 4")
    if table["Contempt"] != ("spin", -100, 100, "0"):
        failures += 1
        print(f"FAIL: the parser lost a spin option's bounds or default: {table['Contempt']}")

    print(f"\n{len(cases) + 2} checks, {failures} failed")
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

    table, error = engine_option_table(args.engine, args.label)
    if error:
        print(f"::error::{error}")
        return 1
    if not table:
        print(f"::error::{args.label}: the engine advertised no options at all; it did not answer `uci` as expected")
        return 1

    problems, warnings = validate(args.options, table, args.label)
    for warning in warnings:
        print(f"::warning::{warning}")
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1

    print(f"{args.label}: every requested option is advertised and in range -- {args.options}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
