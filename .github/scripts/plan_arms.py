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
  plan_arms.py verify --arms STR --shards N --rounds-per-shard N --opening-offset N
                     --candidate-name STR --reference-name STR --book PATH --shard-root PATH
                                              validate the whole batch before pooling

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
import subprocess
import sys
import tempfile

import pool_pentanomial

OPTION_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*=\S+$")
SHARD_LOG_RE = re.compile(r"shard-(\d+)[/\\]match\.log$")
ARTIFACT_RE = re.compile(r"strength-([1-9][0-9]*)-shard-(0|[1-9][0-9]*)$")
TAG_RE = re.compile(r'\[([A-Za-z][A-Za-z0-9_]*)\s+"((?:[^"\\]|\\["\\])*)"\]')


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


def read_pgn_headers(path):
    """Read fastchess header blocks without interpreting moves or completion order."""
    games = []
    headers = {}
    in_moves = False
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            if line.startswith("["):
                if in_moves:
                    games.append(headers)
                    headers = {}
                    in_moves = False
                match = TAG_RE.fullmatch(line)
                if not match:
                    raise PlanError(f"{path}:{number}: malformed PGN header")
                name, value = match.groups()
                if name in headers:
                    raise PlanError(f"{path}:{number}: duplicate PGN header {name}")
                headers[name] = re.sub(r'\\(["\\])', r'\1', value)
            else:
                if not headers:
                    raise PlanError(f"{path}:{number}: PGN moves without headers")
                in_moves = True
    if headers:
        games.append(headers)
    return games


def position_fields(text, source):
    fields = text.split()
    if len(fields) < 4:
        raise PlanError(f"{source}: missing four FEN/EPD position fields")
    return tuple(fields[:4])


def same_start(actual, assigned):
    # fastchess writes '-' for a book en-passant square that no legal capture can use.
    return actual[:3] == assigned[:3] and actual[3] in (assigned[3], "-")


def verify_batch(arms, shards, rounds, offset, candidate, reference, book, root):
    """Assert artifact, routing, assigned-start and completion consistency for every shard.

    Only round one's opening assignment is checked; this does not establish move
    legality, every opening's assignment, or tamper-proof artifact provenance.
    """
    if shards <= 0 or rounds <= 0 or offset < 0:
        raise PlanError("shards and rounds-per-shard must be positive; opening-offset nonnegative")
    if not candidate or not reference or candidate == reference:
        raise PlanError("distinct nonempty candidate-name and reference-name are required")
    routing = matrix(arms, shards)
    by_shard = {}
    run_ids = set()
    for directory, _, _ in os.walk(root):
        if directory == root:
            continue
        name = os.path.basename(directory)
        if "shard-" not in name:
            continue
        match = ARTIFACT_RE.fullmatch(name)
        if not match:
            raise PlanError(f"{directory}: noncanonical shard artifact directory")
        run_id, index = match.groups()
        shard = int(index)
        if shard >= shards or shard in by_shard:
            raise PlanError(f"{directory}: shard {shard} is out of range or seen twice")
        run_ids.add(run_id)
        by_shard[shard] = directory
    missing = sorted(set(range(shards)) - set(by_shard))
    if missing:
        raise PlanError(f"no artifact for shard(s) {missing}; refusing a partial batch")
    if len(run_ids) != 1:
        raise PlanError("shard artifacts belong to different runs")

    needed = {offset + shard * rounds for shard in range(shards)}
    starts = {}
    with open(book, encoding="utf-8") as handle:
        index = 0
        for line in handle:
            if not line.strip():
                continue
            if index in needed:
                starts[index] = position_fields(line, f"{book}: nonblank entry {index}")
            index += 1
    if needed - set(starts):
        raise PlanError(f"{book}: missing assigned nonblank EPD entries {sorted(needed - set(starts))}")

    seen_starts = set()
    for shard in range(shards):
        directory = by_shard[shard]
        log = os.path.join(directory, "match.log")
        pgn = os.path.join(directory, "match.pgn")
        if not os.path.isfile(log) or not os.path.isfile(pgn):
            raise PlanError(f"{directory}: both match.log and match.pgn are required")
        pool_pentanomial.read_shard(log, rounds)
        games = read_pgn_headers(pgn)
        if len(games) != 2 * rounds:
            raise PlanError(f"{pgn}: expected {2 * rounds} games, got {len(games)}")
        arm = routing[shard]["label"]
        expected_candidate = candidate + (f"-arm{arm}" if arm else "")
        expected_colours = {(expected_candidate, reference), (reference, expected_candidate)}
        by_round = {}
        for game in games:
            colours = (game.get("White"), game.get("Black"))
            if colours not in expected_colours:
                raise PlanError(f"{pgn}: expected {expected_candidate} versus {reference}, got {colours}")
            round_text = game.get("Round", "")
            if not re.fullmatch(r"[1-9][0-9]*", round_text):
                raise PlanError(f"{pgn}: missing or invalid Round header {round_text!r}")
            round_number = int(round_text)
            if round_number > rounds:
                raise PlanError(f"{pgn}: round {round_number} outside 1..{rounds}")
            by_round.setdefault(round_number, []).append(game)
        if set(by_round) != set(range(1, rounds + 1)):
            raise PlanError(f"{pgn}: missing round(s) {sorted(set(range(1, rounds + 1)) - set(by_round))}")
        for round_number, pair in by_round.items():
            if len(pair) != 2 or {(game["White"], game["Black"]) for game in pair} != expected_colours:
                raise PlanError(f"{pgn}: round {round_number} requires two opposite-colour games")
        assigned = starts[offset + shard * rounds]
        for game in by_round[1]:
            actual = position_fields(game.get("FEN", ""), f"{pgn}: round 1")
            if not same_start(actual, assigned):
                raise PlanError(f"{pgn}: round 1 FEN differs from assigned nonblank EPD entry "
                                f"{offset + shard * rounds}")
        if assigned in seen_starts:
            raise PlanError(f"{pgn}: round 1 FEN repeats another shard's start")
        seen_starts.add(assigned)


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

    # Minimal pinned fastchess headers: a completed round 2 precedes round 1.
    first_fen = "rnbqkb1r/1p3p1p/p3pnp1/8/2PP4/5N2/P3BPPP/RNBQK2R w KQkq - 0 9"
    second_fen = "rnbqk2r/pp4pp/2pbp1n1/3p1p2/3P4/2PBPNB1/PP3PPP/RN1QK2R w KQkq - 0 9"
    third_fen = "8/8/8/8/8/4k3/8/4K3 w - - 0 1"
    candidate, reference = "candidate-a922cee", "reference-4dafbdd"

    def game(round_number, white, black, fen):
        return (f'[Event "Fastchess Tournament"]\n[Round "{round_number}"]\n'
                f'[White "{white}"]\n[Black "{black}"]\n[Result "1-0"]\n'
                f'[SetUp "1"]\n[FEN "{fen}"]\n\n1-0\n\n')

    def cli_case(name, arm_text="", replacements=(), additions=None, removed=(),
                 overrides=(), omitted=(), success=False):
        with tempfile.TemporaryDirectory() as root:
            shard_root = os.path.join(root, "shards")
            book = os.path.join(root, "openings.epd")
            # Blank lines do not advance the zero-based EPD index; FEN clocks are ignored.
            with open(book, "w", encoding="utf-8") as handle:
                handle.write(f"{third_fen}\n\n{' '.join(first_fen.split()[:4])} id \"start\";\n"
                             f"{second_fen}\n\n{third_fen}\n{second_fen}\n")
            files = {}
            for shard, start in enumerate((first_fen, third_fen)):
                arm = ("-arm" + label(shard)) if arm_text else ""
                engine = candidate + arm
                prefix = f"strength-37125713346-shard-{shard}/"
                files[prefix + "match.log"] = "Ptnml(0-2): [0, 0, 2, 0, 0]\n"
                files[prefix + "match.pgn"] = (game(2, engine, reference, second_fen)
                                               + game(1, engine, reference, start)
                                               + game(2, reference, engine, second_fen)
                                               + game(1, reference, engine, start))
            for path, old, new in replacements:
                files[path] = files[path].replace(old, new)
            if additions:
                for destination, source in additions.items():
                    files[destination] = files[source]
            for path in removed:
                del files[path]
            for path, text in files.items():
                target = os.path.join(shard_root, path)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "w", encoding="utf-8") as handle:
                    handle.write(text)
            command = [sys.executable, os.path.abspath(__file__), "verify", "--arms", arm_text,
                       "--shards", "2", "--rounds-per-shard", "2", "--opening-offset", "1",
                       "--candidate-name", candidate, "--reference-name", reference,
                       "--book", book, "--shard-root", shard_root, *overrides]
            for flag in omitted:
                index = command.index(flag)
                del command[index:index + 2]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            expect(name, (result.returncode == 0) == success
                   and ("Verified" in result.stdout) == success
                   and "Traceback" not in result.stderr)

    log0 = "strength-37125713346-shard-0/match.log"
    pgn0 = "strength-37125713346-shard-0/match.pgn"
    log1 = "strength-37125713346-shard-1/match.log"
    pgn1 = "strength-37125713346-shard-1/match.pgn"
    cli_case("CLI single candidate, blank EPD lines and out-of-order rounds", success=True)
    cli_case("CLI multi-arm round routing", "X=1;X=2", success=True)
    cli_case("CLI swapped arm PGN refused", "X=1;X=2",
             [(pgn1, "-armB", "-armA")])
    cli_case("CLI short parseable log refused", replacements=[(log0, "[0, 0, 2", "[0, 0, 1")])
    cli_case("CLI excess log refused", replacements=[(log1, "[0, 0, 2", "[0, 0, 3")])
    cli_case("CLI compensating pair counts refused",
             replacements=[(log0, "[0, 0, 2", "[0, 0, 1"), (log1, "[0, 0, 2", "[0, 0, 3")])
    cli_case("CLI complete log with short PGN refused", replacements=[
        (pgn0, game(1, reference, candidate, first_fen), "")])
    cli_case("CLI missing round one refused", replacements=[(pgn0, '[Round "1"]', '[Round "2"]')])
    cli_case("CLI excess round one refused", replacements=[
        (pgn0, '[Round "2"]', '[Round "1"]')])
    cli_case("CLI round outside range refused", replacements=[(pgn0, '[Round "2"]', '[Round "3"]')])
    cli_case("CLI same-colour pair refused", replacements=[
        (pgn0, game(1, reference, candidate, first_fen), game(1, candidate, reference, first_fen))])
    cli_case("CLI both round-one FENs checked", replacements=[
        (pgn0, game(1, reference, candidate, first_fen), game(1, reference, candidate, second_fen))])
    cli_case("CLI missing player name refused", replacements=[
        (pgn0, f'[White "{candidate}"]\n', "")])
    cli_case("CLI duplicate header refused", replacements=[
        (pgn0, '[Round "1"]', '[Round "1"]\n[Round "1"]')])
    cli_case("CLI malformed header refused", replacements=[
        (pgn0, '[Round "1"]', '[Round 1]')])
    cli_case("CLI missing FEN refused", replacements=[(pgn0, f'[FEN "{first_fen}"]\n', "")])
    cli_case("CLI missing PGN refused", removed=[pgn1])
    cli_case("CLI missing log refused", removed=[log1])
    cli_case("CLI missing artifact index refused", removed=[log1, pgn1])
    for prefix, name in (("nested/strength-37125713346-shard-0", "duplicate artifact index"),
                         ("strength-37125713346-shard-2", "unexpected artifact index"),
                         ("strength-37125713346-shard-00", "noncanonical artifact index")):
        cli_case(f"CLI {name} refused", additions={prefix + "/match.log": log0,
                                                  prefix + "/match.pgn": pgn0})
    cli_case("CLI incorrect opening offset refused", overrides=["--opening-offset", "0"])
    cli_case("CLI absent assigned EPD entry refused", overrides=["--opening-offset", "100"])
    cli_case("CLI omitted planned pair count refused", omitted=["--rounds-per-shard"])
    cli_case("CLI omitted book refused", omitted=["--book"])
    for flag, value in (("--rounds-per-shard", "0"), ("--shards", "0"), ("--opening-offset", "-1")):
        cli_case(f"CLI invalid {flag} refused", overrides=[flag, value])

    with tempfile.TemporaryDirectory() as root:
        book = os.path.join(root, "book.epd")
        with open(book, "w", encoding="utf-8") as handle:
            handle.write((first_fen + "\n") * 2)
        for shard in range(2):
            directory = os.path.join(root, f"strength-1-shard-{shard}")
            os.makedirs(directory)
            with open(os.path.join(directory, "match.log"), "w", encoding="utf-8") as handle:
                handle.write("Ptnml(0-2): [0, 0, 1, 0, 0]\n")
            with open(os.path.join(directory, "match.pgn"), "w", encoding="utf-8") as handle:
                handle.write(game(1, candidate, reference, first_fen)
                             + game(1, reference, candidate, first_fen))
        raises("assigned start FENs must be distinct", lambda: verify_batch(
            [], 2, 1, 0, candidate, reference, book, root))

    # The book's en-passant square has no legal capture; fastchess writes '-'.
    book_ep = "rnb1k2r/ppq2pbp/2pp1np1/4p3/P1BPP3/2N2N1P/1PPB1PP1/R2QK2R w KQkq e6 0 9"

    def ep_case(pgn_ep):
        with tempfile.TemporaryDirectory() as root:
            book = os.path.join(root, "book.epd")
            with open(book, "w", encoding="utf-8") as handle:
                handle.write(book_ep + "\n")
            directory = os.path.join(root, "strength-1-shard-0")
            os.makedirs(directory)
            with open(os.path.join(directory, "match.log"), "w", encoding="utf-8") as handle:
                handle.write("Ptnml(0-2): [0, 0, 1, 0, 0]\n")
            fen = book_ep.replace(" e6 ", f" {pgn_ep} ")
            with open(os.path.join(directory, "match.pgn"), "w", encoding="utf-8") as handle:
                handle.write(game(1, candidate, reference, fen) + game(1, reference, candidate, fen))
            verify_batch([], 1, 1, 0, candidate, reference, book, root)

    try:
        ep_case("-")
        expect("unusable book en-passant square written as '-' accepted", True)
    except PlanError:
        expect("unusable book en-passant square written as '-' accepted", False)
    raises("different en-passant square refused", lambda: ep_case("d6"))

    print("\nself-test:", "FAIL" if failures else "PASS")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", choices=["matrix", "list", "logs", "verify"])
    parser.add_argument("logs", nargs="*", help="shard logs, for the logs command")
    parser.add_argument("--arms", default="")
    parser.add_argument("--shards", type=int, default=0)
    parser.add_argument("--arm", default="")
    parser.add_argument("--rounds-per-shard", type=pool_pentanomial.positive_int)
    parser.add_argument("--opening-offset", type=int)
    parser.add_argument("--candidate-name")
    parser.add_argument("--reference-name")
    parser.add_argument("--book")
    parser.add_argument("--shard-root")
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
        elif args.command == "logs":
            print("\n".join(logs_for_arm(arms, args.shards, args.arm, args.logs)))
        else:
            required = ("rounds_per_shard", "opening_offset", "candidate_name", "reference_name",
                        "book", "shard_root")
            missing = ["--" + name.replace("_", "-") for name in required
                       if getattr(args, name) is None]
            if missing:
                raise PlanError("verify requires " + ", ".join(missing))
            verify_batch(arms, args.shards, args.rounds_per_shard, args.opening_offset,
                         args.candidate_name, args.reference_name, args.book, args.shard_root)
            print(f"Verified {args.shards} shards with {args.rounds_per_shard} pairs each.")
    except (PlanError, OSError, ValueError) as error:
        # stderr, so the annotation survives the workflow capturing stdout with $(...).
        print(f"::error::{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
