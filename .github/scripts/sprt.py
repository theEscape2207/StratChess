#!/usr/bin/env python3
"""Waved pentanomial SPRT for a Full strength-lab run.

  sprt.py plan    cut every shard into W wave slices, fixed before any game is played
  sprt.py decide  the SPRT decision on the pooled counts of waves 1..k

A waved run plays each shard's R pairs in W slices cut at ceil(k*R/W), so wave k always holds the
first k/W of every shard in planned order, whatever the games' speed. W = 1 is a fixed-size run: its
one slice is the whole shard, its artifacts keep their pre-wave names and its decision is "fixed".

The statistic is pinned by golden values from archived lab runs in the self-test. Over N pairs
with mean per-game score m and population variance v of the per-pair scores (Ptnml categories score
0, 1/4, 1/2, 3/4 and 1):

  LLR = N * (s1 - s0) * (2m - s0 - s1) / (2v),  s = 1 / (1 + 10^(-Elo/400)), Elo0 = 0, Elo1 = 3

the normal approximation to the log-likelihood ratio of H1 (Elo1) over H0 (Elo0). v = 0 gives 0,
which never crosses. The bounds are ln(beta / (1 - alpha)) and ln((1 - beta) / alpha), +-ln 19 at
alpha = beta = 0.05, and both are inclusive: LLR >= upper is H1, LLR <= lower is H0. A run with no
crossing by wave W stops at the cap and passes only if Elo - err > 0 on the full batch, the fixed-run
rule, pooled by pool_pentanomial.pool().

A crossing takes priority over the cap fallback, at wave W as at any other: the fallback is only for
a run the SPRT left undecided. The two can disagree at the cap; the self-test pins one such batch,
where H0 at wave 4 beats Elo - err = +0.89.
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

import pool_pentanomial

ELO0 = 0.0
ELO1 = 3.0
ALPHA = 0.05
BETA = 0.05
WAVES = 4
FULL_GAMES = 26568
FULL_SHARDS = 18

LOWER = math.log(BETA / (1 - ALPHA))
UPPER = math.log((1 - BETA) / ALPHA)


def expected_score(elo):
    return 1 / (1 + 10 ** (-elo / 400))


S0 = expected_score(ELO0)
S1 = expected_score(ELO1)


def wave_cuts(rounds, waves):
    """Cumulative pairs per shard at the end of each wave; refuses a plan with an empty wave."""
    if waves < 1:
        raise ValueError(f"waves must be at least 1, got {waves}")
    if rounds < waves:
        raise ValueError(f"{rounds} pairs per shard cannot fill {waves} waves; need at least {waves}")
    # Integer ceil(k*R/W), with no float rounding at large R.
    return [-(-k * rounds // waves) for k in range(1, waves + 1)]


def wave_plan(shards, rounds, offset, waves):
    """W lists of {shard, start, pairs}; shard i's slices tile [offset + i*R + 1, offset + (i+1)*R]."""
    if shards < 1:
        raise ValueError(f"shards must be at least 1, got {shards}")
    if offset < 0:
        raise ValueError(f"opening offset must be non-negative, got {offset}")
    cuts = wave_cuts(rounds, waves)
    plan = []
    for k, cut in enumerate(cuts):
        before = cuts[k - 1] if k else 0
        plan.append([{"shard": i, "start": offset + i * rounds + before + 1, "pairs": cut - before}
                     for i in range(shards)])
    return plan


def checked_counts(counts):
    if not isinstance(counts, list) or len(counts) != 5 or any(not isinstance(n, int) or n < 0 for n in counts):
        raise ValueError(f"counts must be five non-negative integers, got {counts!r}")
    if sum(counts) == 0:
        raise ValueError("counts hold no pairs")
    return counts


def llr(counts):
    mean, variance = pool_pentanomial.score_and_variance(checked_counts(counts))
    if variance == 0:
        return 0.0
    return sum(counts) * (S1 - S0) * (2 * mean - S0 - S1) / (2 * variance)


def decision_for(value, wave, waves):
    """The decision an LLR gives at wave k of W; split out so the bounds are tested exactly."""
    if not 1 <= wave <= waves:
        raise ValueError(f"wave {wave} is outside 1..{waves}")
    if waves == 1:
        return "fixed"
    if value >= UPPER:
        return "H1"
    if value <= LOWER:
        return "H0"
    return "continue" if wave < waves else "cap"


def decide(counts, wave, waves):
    """The decision at wave k on the pooled counts of waves 1..k."""
    checked_counts(counts)
    return decision_for(0.0 if waves == 1 else llr(counts), wave, waves)


def verdict(decision, counts):
    """Whether the run passes, and by which rule: a crossing decides, the cap falls back to Elo - err > 0."""
    if decision == "H1":
        return {"passes": True, "rule": "sprt"}
    if decision == "H0":
        return {"passes": False, "rule": "sprt"}
    if decision == "cap":
        elo, err, _, _ = pool_pentanomial.pool(checked_counts(counts))
        return {"passes": elo - err > 0, "rule": "cap"}
    raise ValueError(f"decision {decision!r} is not a verdict")


def slice_artifact(run, wave, waves, shard):
    """A wave slice's artifact; a one-wave run keeps the pre-wave shard name."""
    if waves == 1:
        return f"strength-{run}-shard-{shard}"
    return f"strength-{run}-wave-{wave}-shard-{shard}"


def replay_artifact(run, wave, waves, shard, index):
    """A repair piece's artifact; a one-wave run keeps the pre-wave replay name."""
    if waves == 1:
        return f"strength-{run}-replay-{shard}-{index}"
    return f"strength-{run}-wave-{wave}-replay-{shard}-{index}"


def compact(value):
    return json.dumps(value, separators=(",", ":"))


def write_outputs(path, values):
    with open(path, "a", encoding="utf-8") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value}\n")


def decide_outputs(counts, wave, waves):
    if waves == 1:
        return {"decision": decide(counts, wave, waves), "llr": "", "counts": compact(counts)}
    value = llr(counts)
    return {"decision": decision_for(value, wave, waves), "llr": f"{value:+.2f}", "counts": compact(counts)}


# Cumulative wave counts of archived 18 x 555-pair lab runs, cut at ceil(k*555/4) pairs per shard,
# with each LLR computed independently of this module. PGNs: StratChessSupport\StrengthLabPgn.
CAP_FAIL = [824, 2301, 3750, 2268, 847]
GOLDEN_RUNS = [
    # 31191858114: +38.34 +/- 4.26 on the full batch; H1 at wave 1.
    ([[290, 326, 951, 446, 489]], [5.672879135812635], "H1"),
    # 33781814837: -9.98 +/- 3.73; H0 at wave 1.
    ([[246, 617, 927, 504, 208]], [-3.113374196827115], "H0"),
    # 33802622783: +0.23 +/- 3.62; no crossing, so the cap, which does not pass.
    ([[200, 555, 975, 553, 219], [408, 1155, 1885, 1143, 413],
      [599, 1723, 2829, 1727, 628], CAP_FAIL],
     [0.223471909414182, -0.6998399939951975, -0.0435880655625557, -1.1231084857364602], "cap"),
]
# CAP_FAIL with 55 pairs moved from 0-2 to 2-0: LLR +2.25, Elo - err = +0.44.
CAP_PASS = [769, 2301, 3750, 2268, 902]
# The SPRT and Elo - err disagree at the cap: every wave-4 pair drawn, 86 of 13,284 pairs at
# 1.5-0.5. LLR -77.06, Elo - err = +0.89.
CAP_DISAGREES = [0, 0, 13198, 86, 0]


def plan_tiles_shards(shards, rounds, offset, waves):
    """Whether shard i's W non-empty slices cover [offset + i*R + 1, offset + (i+1)*R] in order, no gap."""
    plan = wave_plan(shards, rounds, offset, waves)
    if len(plan) != waves or any(len(wave) != shards for wave in plan):
        return False
    for i in range(shards):
        expected = offset + i * rounds + 1
        for piece in (wave[i] for wave in plan):
            if piece["shard"] != i or piece["pairs"] < 1 or piece["start"] != expected:
                return False
            expected += piece["pairs"]
        if expected != offset + (i + 1) * rounds + 1:
            return False
    return True


def self_test():
    failures = []

    def check(name, condition):
        print(f"{'ok  ' if condition else 'FAIL'} {name}")
        if not condition:
            failures.append(name)

    def raises(call):
        try:
            call()
        except ValueError:
            return True
        return False

    for waves_counts, want_llrs, want in GOLDEN_RUNS:
        got = [llr(counts) for counts in waves_counts]
        check(f"golden LLR {[f'{v:+.4f}' for v in want_llrs]}",
              all(math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-12) for a, b in zip(got, want_llrs)))
        decisions = [decide(counts, k + 1, WAVES) for k, counts in enumerate(waves_counts)]
        check(f"golden decisions end {want}, earlier waves continue",
              decisions == ["continue"] * (len(decisions) - 1) + [want])

    check("lower bound is ln(beta/(1-alpha)) = -ln 19", LOWER == math.log(BETA / (1 - ALPHA))
          and math.isclose(LOWER, -math.log(19), rel_tol=1e-15))
    check("upper bound is ln((1-beta)/alpha) = +ln 19, shown as 2.94",
          UPPER == math.log((1 - BETA) / ALPHA) and math.isclose(UPPER, 2.944438979166440, rel_tol=1e-14)
          and f"{UPPER:+.2f}" == "+2.94")
    check("H1 inclusive at the computed upper bound",
          decision_for(UPPER, 1, 4) == "H1" and decision_for(math.nextafter(UPPER, 0), 1, 4) == "continue")
    check("H0 inclusive at the computed lower bound",
          decision_for(LOWER, 2, 4) == "H0" and decision_for(math.nextafter(LOWER, 0), 2, 4) == "continue")
    check("v = 0 gives LLR 0 and never crosses",
          llr([0, 0, 10, 0, 0]) == 0.0 and llr([0, 0, 0, 0, 7]) == 0.0
          and decide([0, 0, 0, 0, 7], 1, 4) == "continue" and decide([7, 0, 0, 0, 0], 4, 4) == "cap")
    check("a crossing at wave 4 is the SPRT's, not the cap's",
          decision_for(UPPER, 4, 4) == "H1" and decision_for(LOWER, 4, 4) == "H0")
    check("cap passes when Elo - err > 0", decide(CAP_PASS, 4, 4) == "cap"
          and verdict("cap", CAP_PASS) == {"passes": True, "rule": "cap"})
    check("cap fails when Elo - err <= 0", verdict("cap", CAP_FAIL) == {"passes": False, "rule": "cap"})
    elo, err, _, _ = pool_pentanomial.pool(CAP_DISAGREES)
    check("H0 at wave 4 wins over Elo - err > 0",
          f"{llr(CAP_DISAGREES):.2f}" == "-77.06" and f"{elo - err:+.2f}" == "+0.89"
          and decide(CAP_DISAGREES, 4, 4) == "H0"
          and verdict("H0", CAP_DISAGREES) == {"passes": False, "rule": "sprt"})
    check("H1 passes by the SPRT", verdict("H1", [0, 0, 1, 0, 0]) == {"passes": True, "rule": "sprt"})
    check("continue and fixed are not verdicts",
          raises(lambda: verdict("continue", CAP_PASS)) and raises(lambda: verdict("fixed", CAP_PASS)))
    check("a one-wave run is fixed, whatever its counts",
          decide([0, 0, 0, 0, 900], 1, 1) == "fixed" and decide(CAP_DISAGREES, 1, 1) == "fixed")
    check("waves outside 1..W refused", raises(lambda: decide(CAP_PASS, 0, 4))
          and raises(lambda: decide(CAP_PASS, 5, 4)))
    check("malformed counts refused", all(raises(lambda c=c: llr(c)) for c in
                                          ([0, 0, 0, 0, 0], [1, 2, 3, 4], [1, 2, 3, 4, -5], [1, 2, 3.0, 4, 5],
                                           "12345")))

    check("cuts (738, 4)", wave_cuts(738, 4) == [185, 369, 554, 738])
    check("cuts (900, 1)", wave_cuts(900, 1) == [900])
    check("cuts (7, 4)", wave_cuts(7, 4) == [2, 4, 6, 7])
    check("Full plan: 185/184/185/184 pairs, 6,660/13,284/19,944/26,568 games",
          [sum(s["pairs"] for s in wave) for wave in wave_plan(FULL_SHARDS, 738, 0, WAVES)]
          == [3330, 3312, 3330, 3312]
          and [2 * FULL_SHARDS * c for c in wave_cuts(738, 4)] == [6660, 13284, 19944, FULL_GAMES])
    check("R = 3 refused for four waves, R = 4 accepted",
          raises(lambda: wave_cuts(3, 4)) and wave_cuts(4, 4) == [1, 2, 3, 4])
    check("zero waves, zero shards and a negative offset refused",
          raises(lambda: wave_cuts(5, 0)) and raises(lambda: wave_plan(0, 5, 0, 1))
          and raises(lambda: wave_plan(2, 5, -1, 1)))
    check("every shard's slices tile its range contiguously and disjointly",
          all(plan_tiles_shards(*case) for case in ((18, 738, 0, 4), (2, 36, 26568, 4), (18, 740, 10, 1),
                                                     (3, 7, 5, 4), (5, 4, 0, 4), (1, 1001, 99, 3))))
    check("one-wave plan is the whole shard",
          wave_plan(2, 740, 7, 1) == [[{"shard": 0, "start": 8, "pairs": 740},
                                        {"shard": 1, "start": 748, "pairs": 740}]])

    check("artifact names at W = 1 keep the pre-wave form",
          slice_artifact(42, 1, 1, 3) == "strength-42-shard-3"
          and replay_artifact(42, 1, 1, 3, 0) == "strength-42-replay-3-0")
    check("artifact names at W = 4 carry the wave",
          slice_artifact(42, 2, 4, 17) == "strength-42-wave-2-shard-17"
          and replay_artifact(42, 4, 4, 0, 5) == "strength-42-wave-4-replay-0-5")

    with tempfile.TemporaryDirectory() as root:
        def cli(*args):
            output = os.path.join(root, "github_output")
            if os.path.exists(output):
                os.remove(output)
            result = subprocess.run([sys.executable, os.path.abspath(__file__), *args, "--output", output],
                                    capture_output=True, text=True, check=False)
            values = {}
            if os.path.exists(output):
                with open(output, encoding="utf-8") as handle:
                    values = dict(line.rstrip("\n").split("=", 1) for line in handle if line.strip())
            clean = "Traceback" not in result.stderr
            return result.returncode, values, clean

        code, values, clean = cli("plan", "--shards", "2", "--rounds", "7", "--opening-offset", "5",
                                  "--waves", "4")
        check("CLI plan writes wave_plan", code == 0 and clean
              and json.loads(values.get("wave_plan", "null")) == wave_plan(2, 7, 5, 4)
              and " " not in values["wave_plan"])
        code, values, clean = cli("plan", "--shards", "2", "--rounds", "3", "--opening-offset", "0",
                                  "--waves", "4")
        check("CLI plan refuses an empty wave cleanly, writing nothing", code == 1 and clean and not values)
        code, values, clean = cli("decide", "--counts", "[290,326,951,446,489]", "--wave", "1", "--waves", "4")
        check("CLI decide writes H1, LLR and counts", code == 0 and clean
              and values == {"decision": "H1", "llr": "+5.67", "counts": "[290,326,951,446,489]"})
        code, values, clean = cli("decide", "--counts", "[824, 2301, 3750, 2268, 847]", "--wave", "4",
                                  "--waves", "4")
        check("CLI decide writes the cap, LLR to two decimals, counts compact", code == 0 and clean
              and values == {"decision": "cap", "llr": "-1.12", "counts": "[824,2301,3750,2268,847]"})
        code, values, clean = cli("decide", "--counts", "[0,0,1,0,0]", "--wave", "1", "--waves", "1")
        check("CLI decide writes fixed with an empty LLR", code == 0 and clean
              and values == {"decision": "fixed", "llr": "", "counts": "[0,0,1,0,0]"})
        for bad in ("[1,2,3,4]", "not json", "[0,0,0,0,0]"):
            code, values, clean = cli("decide", "--counts", bad, "--wave", "1", "--waves", "4")
            check(f"CLI decide refuses {bad!r} cleanly, writing nothing", code == 1 and clean and not values)

    print("\nSPRT self-test " + ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")

    planning = commands.add_parser("plan", help="write the run's wave plan as wave_plan")
    planning.add_argument("--shards", type=int, required=True)
    planning.add_argument("--rounds", type=int, required=True, help="pairs per shard")
    planning.add_argument("--opening-offset", type=int, required=True)
    planning.add_argument("--waves", type=int, required=True)
    planning.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    deciding = commands.add_parser("decide", help="write decision, llr and counts for wave k")
    deciding.add_argument("--counts", required=True, help="pooled Ptnml(0-2) of waves 1..k, as JSON")
    deciding.add_argument("--wave", type=int, required=True)
    deciding.add_argument("--waves", type=int, required=True)
    deciding.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    args = parser.parse_args()
    if args.self_test:
        return self_test()
    try:
        if args.command == "plan":
            plan = wave_plan(args.shards, args.rounds, args.opening_offset, args.waves)
            write_outputs(args.output, {"wave_plan": compact(plan)})
            print(f"{args.waves} wave(s), cumulative pairs per shard {wave_cuts(args.rounds, args.waves)}")
            return 0
        if args.command == "decide":
            outputs = decide_outputs(json.loads(args.counts), args.wave, args.waves)
            write_outputs(args.output, outputs)
            print(f"wave {args.wave} of {args.waves}: {outputs}")
            return 0
    except ValueError as error:  # json.JSONDecodeError is a ValueError
        print(f"::error::{error}", file=sys.stderr)
        return 1
    parser.error("a command is required")


if __name__ == "__main__":
    sys.exit(main())
