"""Validate intended per-engine UCI options against the advertised domains.

The engine follows the usual UCI convention and ignores an unknown `setoption`
name in silence; a malformed or out-of-domain value gets an `info string` that no
one reads during a match. fastchess passes `option.X=Y` through without
knowing what the engine supports, so a typo or an out-of-range value produces a
measurement that ran for three hours against an engine configured exactly like
its reference -- a null result indistinguishable from a real one. That is the
failure the strength lab's header calls worse than no measurement at all, so
the options are checked against the engine's own advertised option table before
any shard starts.

The table check reproduces two of the engine's three gates: the advertised
per-option domain, and the unsigned-decimal value syntax
`SearchTuningSchema::read_uci` enforces. The third, the whole-struct `Validate()`
that `ParseUci` runs afterwards, can still reject a value that is individually in
range for a cross-field reason. So the options are then sent to a fresh engine,
in order, as fastchess sends them, and the engine's own reply decides: a
tuning option or Hash it applied is echoed as `info string <name> ...`, one it
refused says "not applied" or "previous value retained" (`UCIHandler.cpp`). A
refusal fails the check. An option the engine applies without an echo
(MultiPV, UCI_Elo, UCI_LimitStrength) passes with a warning that its
application is unproven.

Usage:
    validate_uci_options.py --engine <binary> --label candidate --options "A=1 B=true"
    validate_uci_options.py --self-test

An empty option string still queries and validates the advertised table. Resolution
includes defaults and the harness-owned Threads value; it does not read settings back.
"""

import argparse
import re
import subprocess
import sys

# "option name X type spin default D min L max H" / "... type check default true"
_SPIN = re.compile(r"^option name ([A-Za-z][A-Za-z0-9_]*) type spin default (-?[0-9]+) min (-?[0-9]+) max (-?[0-9]+)\s*$")
_CHECK = re.compile(r"^option name ([A-Za-z][A-Za-z0-9_]*) type check default (true|false)\s*$")

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
    """Return spin/check domains; reject ambiguous or unsupported advertisements."""
    table = {}
    for line in uci_output.splitlines():
        line = line.strip()
        spin = _SPIN.match(line)
        check = _CHECK.match(line)
        if spin or check:
            name = (spin or check).group(1)
            if name in table:
                raise ValueError(f"duplicate advertised option '{name}'")
            if spin:
                lo, hi, default = int(spin.group(3)), int(spin.group(4)), int(spin.group(2))
                if not lo <= default <= hi:
                    raise ValueError(f"advertised option '{name}' has invalid bounds/default")
                table[name] = ("spin", lo, hi, spin.group(2))
            else:
                table[name] = ("check", None, None, check.group(2))
        elif line == "option" or line.startswith("option ") or line.startswith("option\t"):
            raise ValueError(f"malformed or unsupported advertised option: {line}")
    return table


def validate(options, table, label):
    """Return (problems, warnings) about syntax/domains, not runtime application."""
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
        if normalize_value(kind, value) == normalize_value(kind, default):
            # A redundant default override changes no resolved setting.
            # Comparison preflight decides whether the complete conditions match.
            warnings.append(f"{label}: '{name}'={value} is already the engine's default, so it changes nothing")
    return problems, warnings


def normalize_value(kind, value):
    return int(value) if kind == "spin" else value == "true"


def resolve_options(options, table, label, threads=1):
    """Return (resolved map or None, problems, warnings), including the harness-owned Threads."""
    problems, warnings = validate(options, table, label)
    domain = table.get("Threads")
    if not domain or domain[0] != "spin" or not domain[1] <= threads <= domain[2]:
        problems.append(f"{label}: the advertised Threads domain must admit harness-owned Threads={threads}")
    if problems:
        return None, problems, warnings
    resolved = {name: normalize_value(kind, default)
                for name, (kind, _lo, _hi, default) in table.items()}
    for name, value in option_pairs(options):
        resolved[name] = normalize_value(table[name][0], value)
    resolved["Threads"] = threads
    return resolved, [], warnings


def option_pairs(options):
    """[name, value] pairs of an option string that has passed validate()."""
    return [item.split("=", 1) for item in options.split()]


def run_engine(engine, commands):
    """(output lines, failure) for commands sent to `engine uci`; failure says what went wrong, else None."""
    try:
        result = subprocess.run(
            [engine, "uci"],
            input="\n".join(commands) + "\n",
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    except (OSError, UnicodeError, subprocess.SubprocessError) as problem:
        # A missing, non-executable or hung binary is an engine problem; without this it would
        # surface as a Python traceback with no annotation and read as a defect in this script.
        return [], f"could not run {engine}: {problem}"
    if result.returncode:
        return [], f"exited with status {result.returncode}"
    lines = [line.strip() for line in result.stdout.splitlines()]
    if "uciok" not in lines:
        return [], "did not return a complete uciok reply"
    return lines, None


# What the engine prints when it refuses a setoption whose name it knows.
_REFUSED = ("not applied", "previous value retained")


def classify_replies(options, replies, label):
    """(problems, warnings) from the engine's reply lines to each setoption, in order.

    options must already have passed validate(), so every item is Name=Value.
    """
    problems = []
    warnings = []
    items = option_pairs(options)
    if len(replies) < len(items):
        problems.append(f"{label}: the engine answered {len(replies)} of {len(items)} setoption commands")
        return problems, warnings
    for (name, value), reply in zip(items, replies):
        infos = [line for line in reply if line.startswith("info string ")]
        refused = [line for line in infos if any(marker in line for marker in _REFUSED)]
        if refused:
            problems.append(f"{label}: the engine refused '{name}'={value}: {refused[0]}")
        elif not any(line.lower().startswith(f"info string {name.lower()} ") for line in infos):
            warnings.append(f"{label}: the engine confirms no '{name}' setting, so its application is unproven")
    return problems, warnings


def apply_options(engine, options, label):
    """(problems, warnings) from sending options, already validated, to a fresh engine.

    Each setoption is followed by isready, so the engine's replies split at each readyok.
    """
    if not options.split():
        return [], []
    commands = ["uci"]
    for name, value in option_pairs(options):
        commands += [f"setoption name {name} value {value}", "isready"]
    commands.append("quit")
    lines, failure = run_engine(engine, commands)
    if failure:
        return [f"{label}: the setoption run {failure}"], []
    replies = []
    current = []
    for line in lines[lines.index("uciok") + 1:]:
        if line == "readyok":
            replies.append(current)
            current = []
        else:
            current.append(line)
    return classify_replies(options, replies, label)


def engine_option_table(engine, label):
    """(table, error). A non-None error means query or advertisement validation failed."""
    lines, failure = run_engine(engine, ["uci", "quit"])
    if failure:
        return {}, f"{label}: UCI query {failure}"
    try:
        table = parse_option_table("\n".join(lines))
    except ValueError as problem:
        return {}, f"{label}: {problem}"
    if not table:
        return {}, f"{label}: the engine advertised no options at all"
    return table, None


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
        ("Hash=064", [], ["is already the engine's default"], "normalized zero-padded defaults warn"),
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

    extra_checks = 0

    def expect(description, condition):
        nonlocal failures, extra_checks
        extra_checks += 1
        print(f"{'ok' if condition else 'FAIL'}: {description}")
        if not condition:
            failures += 1

    for output, description in (
        (SELF_TEST_UCI + "option name Hash type spin default 64 min 1 max 4096\n", "duplicate advertised names fail"),
        ("option name Book type string default none\n", "unsupported advertised types fail"),
        ("option name Hash type spin default 4 min 5 max 10\n", "out-of-domain defaults fail"),
        ("option name Bad Name type check default true\n", "unsupported advertised names fail"),
        ("option name Hash type spin default nope min 1 max 5\n", "malformed advertised values fail"),
    ):
        try:
            parse_option_table(output)
        except ValueError:
            expect(description, True)
        else:
            expect(description, False)
    resolved, errors, _ = resolve_options("Hash=064", table, "candidate")
    expect("resolution includes normalized defaults and forced Threads",
           not errors and resolved == {"Threads": 1, "Hash": 64, "Contempt": 0, "ReverseFutility": True})
    resolved, errors, _ = resolve_options("", dict(table, Threads=("spin", 1, 32, "4")), "candidate")
    expect("harness Threads overrides the advertised default", not errors and resolved["Threads"] == 1)
    resolved, errors, _ = resolve_options("Hash=0", table, "candidate")
    expect("invalid overrides produce no resolved map", resolved is None and bool(errors))
    threads_table = dict(table, Threads=("spin", 2, 32, "2"))
    resolved, errors, _ = resolve_options("", threads_table, "candidate")
    expect("resolution refuses an invalid harness-owned Threads domain", resolved is None and bool(errors))
    resolved, errors, _ = resolve_options("", table, "candidate", threads=4)
    expect("a harness-owned Threads=4 is resolved", not errors and resolved["Threads"] == 4)
    resolved, errors, _ = resolve_options("", table, "candidate", threads=33)
    expect("a harness-owned Threads above the advertised max is refused", resolved is None and bool(errors))
    reply_cases = [
        ("Contempt=20", [["info string Contempt 20"]], 0, 0, "an echoed tuning option is confirmed"),
        ("Hash=128", [["info string hash 128 MiB (4194304 buckets)"]], 0, 0, "Hash's own echo confirms it"),
        ("Contempt=20", [["info string Contempt not applied: invalid combination; previous value retained"]], 1, 0,
         "FALSIFY: a refused option fails"),
        ("UCI_LimitStrength=yes", [["info string UCI_LimitStrength: value must be true or false; previous value retained"]],
         1, 0, "a 'previous value retained' reply is a refusal"),
        ("Contempt=20", [[]], 0, 1, "FALSIFY: an option the engine never echoes is unproven, not confirmed"),
        ("Contempt=20", [["info string ContemptX 20"]], 0, 1, "an echo of a longer name does not confirm this one"),
        ("Contempt=20 Hash=128", [["info string Contempt 20"]], 1, 0, "FALSIFY: a missing reply fails"),
    ]
    for options, replies, want_problems, want_warnings, description in reply_cases:
        problems, warnings = classify_replies(options, replies, "candidate")
        expect(description, len(problems) == want_problems and len(warnings) == want_warnings)

    print(f"\n{len(cases) + 2 + extra_checks} checks, {failures} failed")
    return 1 if failures else 0


def report(problems, warnings):
    """Print problems and warnings as workflow annotations; True when there are problems."""
    for warning in warnings:
        print(f"::warning::{warning}")
    for problem in problems:
        print(f"::error::{problem}")
    return bool(problems)


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

    table, error = engine_option_table(args.engine, args.label)
    if error:
        print(f"::error::{error}")
        return 1
    # Options reach the engine only once they pass the table check.
    if report(*validate(args.options, table, args.label)) or report(*apply_options(args.engine, args.options, args.label)):
        return 1

    print(f"{args.label}: every requested option is advertised, in range and accepted -- {args.options}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
