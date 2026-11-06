#!/usr/bin/env python3
"""Waved pentanomial SPRT for a Full strength-lab run.

  sprt.py plan       cut every shard into W wave slices, fixed before any game is played
  sprt.py decide     the SPRT decision on the pooled counts of waves 1..k
  sprt.py shape      print "S R P OFFSET" for verifying wave k at batch level
  sprt.py summarize  the run's outcome from every wave's outputs, for aggregate

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

A waved run plays at the cap the simulation validated, Full's 26,568 games on 18 shards; a
calibration run may be smaller, so a null smoke run can play all four waves.
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


def run_waves(sprt, games, shards, calibration, arms):
    """The run's wave count: 1 for a fixed-size run, WAVES for an SPRT one at the validated cap."""
    if not sprt:
        return 1
    if arms:
        raise ValueError("sprt cannot run a multi-arm screen")
    if not calibration and (games, shards) != (FULL_GAMES, FULL_SHARDS):
        raise ValueError(f"sprt runs at the cap the simulation validated, games={FULL_GAMES} shards={FULL_SHARDS}; "
                         f"got games={games} shards={shards}. A calibration run may be smaller")
    return WAVES


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


def wave_shape(plan, wave, slices):
    """(shards, R, pairs, offset) for verifying wave k at batch level: shard i's slice starts at book
    entry offset + i*R and plays `pairs` pairs. Refuses slices that are not wave k of the plan."""
    if not 1 <= wave <= len(plan):
        raise ValueError(f"wave {wave} is outside 1..{len(plan)}")
    if plan[wave - 1] != slices:
        raise ValueError(f"slices is not wave {wave} of the plan")
    if [piece["shard"] for piece in slices] != list(range(len(slices))):
        raise ValueError("the wave's slices are not shards 0..S-1 in order")
    sizes = {piece["pairs"] for piece in slices}
    if len(sizes) != 1:
        raise ValueError(f"the wave's slices differ in size: {sorted(sizes)}")
    return len(slices), sum(planned[0]["pairs"] for planned in plan), sizes.pop(), slices[0]["start"] - 1


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


def cumulative(prior, counts, wave):
    """Waves 1..k's counts: wave k's own plus waves 1..k-1's, which wave 1 has none of."""
    checked_counts(counts)
    if (wave == 1) != (prior is None):
        raise ValueError(f"wave {wave} needs the earlier waves' counts exactly when it is not wave 1")
    return counts if prior is None else [a + b for a, b in zip(checked_counts(prior), counts)]


def decide_outputs(counts, wave, waves):
    if waves == 1:
        return {"decision": decide(counts, wave, waves), "llr": "", "counts": compact(counts)}
    value = llr(counts)
    return {"decision": decision_for(value, wave, waves), "llr": f"{value:+.2f}", "counts": compact(counts)}


NO_SHARD_RAN = "No shard ran: setup or build did not complete."
UNDECIDED = "The wave ended `{}` without a decision: a check after the shards failed, or the run was cancelled."
EMPTY_SUMMARY = dict.fromkeys(("outcome", "wave", "reason", "counts", "pairs", "heading", "games", "replayed",
                               "repair_plan"), "")


def discarded(reason):
    return {**EMPTY_SUMMARY, "outcome": "discarded", "reason": reason, "heading": "## Strength lab — DISCARDED"}, ""


def played_waves(needs, waves):
    """Each wave's outputs up to the one that decided, or the reason the run has no decision.

    Read from outputs only: which artifacts exist cannot tell a planned stop from a discard.
    """
    played = []
    for wave in range(1, waves + 1):
        job = needs.get(f"wave-{wave}", {})
        result, outputs = job.get("result", "missing"), job.get("outputs") or {}
        prefix = f"Wave {wave}: " if waves > 1 else ""
        if outputs.get("status") == "discarded":
            return None, prefix + outputs.get("reason", "")
        if outputs.get("status") != "complete" or not outputs.get("decision"):
            return None, prefix + (NO_SHARD_RAN if wave == 1 and result == "skipped" else UNDECIDED.format(result))
        played.append(outputs)
        if outputs["decision"] != "continue":
            break
    decision = played[-1]["decision"]
    if not (decision == "fixed" if waves == 1 else
            decision in ("H1", "H0") or (decision == "cap" and len(played) == waves)):
        raise ValueError(f"wave {len(played)} of {waves} decided {decision!r}")
    return played, ""


def sprt_report(plan, played):
    """The waved report's SPRT block: the test's settings and every played wave pooled cumulatively."""
    shards, cuts = len(plan[0]), [0]
    for wave in plan:
        cuts.append(cuts[-1] + wave[0]["pairs"])
    cap = 2 * shards * cuts[-1]
    lines = [f"SPRT [{ELO0:g}, {ELO1:g}], α = β = {ALPHA:g}: H1 at LLR ≥ {UPPER:+.2f}, "
             f"H0 at ≤ {LOWER:+.2f}, cap {cap:,} games".replace("-", "−"), "",
             "| Wave | Games | Elo ± err | LLR | Decision |", "|---|---|---|---|---|"]
    for wave, outputs in enumerate(played, 1):
        elo, err, _, _ = pool_pentanomial.pool(checked_counts(json.loads(outputs["counts"])))
        lines.append(f"| {wave} | {2 * shards * cuts[wave]:,} | {elo:+.2f} ± {err:.2f} | {outputs['llr']} | "
                     f"{outputs['decision']} |")
    return "\n".join(lines) + "\n"


def summarize(needs, calibration):
    """The run's outcome from strength.yml's `needs`: the step outputs aggregate reads, and the report's
    SPRT block, empty for a fixed run. A discard carries its reason and no figures."""
    setup = needs.get("setup", {})
    if setup.get("result") != "success":
        return discarded(NO_SHARD_RAN)
    plan = json.loads(setup["outputs"]["wave_plan"])
    waves = len(plan)
    played, reason = played_waves(needs, waves)
    if played is None:
        return discarded(reason)
    wave, decision = len(played), played[-1]["decision"]
    counts = checked_counts(json.loads(played[-1]["counts"]))
    pairs = sum(planned[0]["pairs"] for planned in plan[:wave])
    repaired = [(k, outputs["slices"]) for k, outputs in enumerate(played, 1) if outputs.get("lost_a")]
    if waves == 1:
        # The marker's payload stays the pre-wave repair plan.
        repair_plan = repaired[0][1] if repaired else ""
    else:
        repair_plan = compact([{"wave": k, "slices": json.loads(slices)} for k, slices in repaired]) if repaired else ""
    summary = {**EMPTY_SUMMARY, "outcome": decision, "wave": str(wave), "counts": compact(counts),
               "pairs": str(pairs), "heading": "## Strength lab", "repair_plan": repair_plan,
               "replayed": "; ".join(outputs["replayed"] for outputs in played if outputs.get("replayed"))}
    if waves == 1:
        return summary, ""
    label = "## Strength lab" + (" (calibration)" if calibration else "")
    if decision == "cap":
        passes = verdict(decision, counts)["passes"]
        summary["heading"] = f"{label} — {'passes at the cap (Elo − err > 0)' if passes else 'does not pass at the cap'}"
        stop = "undecided at the cap"
    else:
        summary["heading"] = f"{label} — {'passes' if decision == 'H1' else 'does not pass'} " \
                             f"(SPRT {decision}, wave {wave} of {waves})"
        stop = f"stopped by SPRT {decision}"
    shards = len(plan[0])
    cap_games = 2 * shards * sum(planned[0]["pairs"] for planned in plan)
    summary["games"] = f"{2 * shards * pairs:,} of {cap_games:,} planned = {shards} shards × {wave} of {waves} waves, {stop}"
    return summary, sprt_report(plan, played)


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

    full = wave_plan(FULL_SHARDS, 738, 0, WAVES)
    check("wave shape: Full wave 2 verifies 184 pairs from entry 185, spacing 738",
          wave_shape(full, 2, full[1]) == (18, 738, 184, 185))
    one = wave_plan(2, 2, 1, 1)
    check("wave shape: a fixed run is its whole shards", wave_shape(one, 1, one[0]) == (2, 2, 2, 1))
    check("wave shape refuses another wave's slices or a wave outside the plan",
          raises(lambda: wave_shape(full, 2, full[0])) and raises(lambda: wave_shape(full, 5, full[0]))
          and raises(lambda: wave_shape(full, 0, full[0])))

    check("artifact names at W = 1 keep the pre-wave form",
          slice_artifact(42, 1, 1, 3) == "strength-42-shard-3"
          and replay_artifact(42, 1, 1, 3, 0) == "strength-42-replay-3-0")
    check("artifact names at W = 4 carry the wave",
          slice_artifact(42, 2, 4, 17) == "strength-42-wave-2-shard-17"
          and replay_artifact(42, 4, 4, 0, 5) == "strength-42-wave-4-replay-0-5")

    def needs(plan, *waves, setup="success"):
        """strength.yml's needs: setup with the plan, then each called wave's (result, outputs); later waves skip."""
        jobs = {"setup": {"result": setup, "outputs": {"wave_plan": compact(plan)} if setup == "success" else {}},
                "build": {"result": "success", "outputs": {}}}
        for k in range(1, len(plan) + 1):
            result, outputs = waves[k - 1] if k <= len(waves) else ("skipped", {})
            jobs[f"wave-{k}"] = {"result": result, "outputs": outputs}
        return jobs

    def complete(decision, counts, llr_text="", replayed="", slices=""):
        return {"status": "complete", "reason": "", "decision": decision, "llr": llr_text, "counts": compact(counts),
                "lost_total": "1" if slices else "0", "replayed": replayed, "lost_a": "1" if slices else "",
                "lost_b": "", "slices": slices}

    fixed_plan, golden_plan = wave_plan(2, 2, 1, 1), wave_plan(18, 555, 0, 4)
    pieces = compact([{"shard": 1, "index": 0, "start": 4, "pairs": 1}, {"shard": 1, "index": 1, "start": 5, "pairs": 1}])
    summary, report = summarize(needs(fixed_plan, ("success", complete("fixed", [0, 0, 4, 0, 0]))), True)
    check("summarize: a fixed run keeps the plain heading, no Games text, no SPRT block",
          summary == {**EMPTY_SUMMARY, "outcome": "fixed", "wave": "1", "counts": "[0,0,4,0,0]", "pairs": "2",
                      "heading": "## Strength lab"} and report == "")
    summary, _ = summarize(needs(fixed_plan, ("failure", complete("fixed", [0, 0, 4, 0, 0], "",
                                                                  "shard 1, 2 slices (runner lost)", pieces))), False)
    check("summarize: a repaired fixed run keeps the Replayed text and the marker's payload",
          summary["replayed"] == "shard 1, 2 slices (runner lost)" and summary["repair_plan"] == pieces)
    discard = {"status": "discarded", "reason": "Shard jobs reported `failure`. Not repaired: shard 2 failed."}
    for name, run_needs, reason in (
            ("a discarded wave's reason", needs(fixed_plan, ("failure", discard)), discard["reason"]),
            ("setup that failed", needs(fixed_plan, setup="failure"), NO_SHARD_RAN),
            ("a wave that never ran", needs(fixed_plan, ("skipped", {})), NO_SHARD_RAN),
            ("a wave with no outputs", needs(fixed_plan, ("failure", {})), UNDECIDED.format("failure")),
            ("a decision without a complete status", needs(fixed_plan, ("failure", {"decision": "fixed"})),
             UNDECIDED.format("failure"))):
        summary, report = summarize(run_needs, False)
        check(f"summarize: a fixed run discards on {name}, with no figures",
              summary == {**EMPTY_SUMMARY, "outcome": "discarded", "reason": reason,
                          "heading": "## Strength lab — DISCARDED"} and report == "")
    summary, report = summarize(needs(golden_plan, ("success", complete("H1", GOLDEN_RUNS[0][0][0], "+5.67"))), False)
    check("summarize: H1 at wave 1 heads, counts and sizes the report",
          summary["outcome"] == "H1" and summary["wave"] == "1" and summary["pairs"] == "139"
          and summary["heading"] == "## Strength lab — passes (SPRT H1, wave 1 of 4)"
          and summary["games"] == "5,004 of 19,980 planned = 18 shards × 1 of 4 waves, stopped by SPRT H1"
          and summary["counts"] == compact(GOLDEN_RUNS[0][0][0]) and summary["repair_plan"] == "")
    check("summarize: the SPRT block states the test and pools each played wave",
          report.splitlines() == ["SPRT [0, 3], α = β = 0.05: H1 at LLR ≥ +2.94, H0 at ≤ −2.94, cap 19,980 games", "",
                                  "| Wave | Games | Elo ± err | LLR | Decision |", "|---|---|---|---|---|",
                                  "| 1 | 5,004 | +36.09 ± 8.46 | +5.67 | H1 |"])
    summary, _ = summarize(needs(golden_plan, ("success", complete("H1", GOLDEN_RUNS[0][0][0], "+5.67"))), True)
    check("summarize: a calibration run keeps its label in the heading",
          summary["heading"] == "## Strength lab (calibration) — passes (SPRT H1, wave 1 of 4)")
    wave1 = complete("continue", [200, 555, 975, 553, 219], "+0.22")
    summary, report = summarize(needs(golden_plan, ("success", wave1),
                                      ("success", complete("H0", [446, 1172, 1902, 1057, 427], "-2.97"))), False)
    check("summarize: H0 at wave 2, two rows",
          summary["heading"] == "## Strength lab — does not pass (SPRT H0, wave 2 of 4)"
          and summary["games"] == "10,008 of 19,980 planned = 18 shards × 2 of 4 waves, stopped by SPRT H0"
          and summary["pairs"] == "278" and len(report.splitlines()) == 6
          and report.splitlines()[-1].endswith("| -2.97 | H0 |"))
    capped = [("success", complete("continue", counts, f"{llr(counts):+.2f}")) for counts in GOLDEN_RUNS[2][0][:3]]
    for name, counts, heading in (("passes", CAP_PASS, "## Strength lab — passes at the cap (Elo − err > 0)"),
                                  ("fails", CAP_FAIL, "## Strength lab — does not pass at the cap")):
        summary, report = summarize(needs(golden_plan, *capped, ("success", complete("cap", counts, "-1.12"))), False)
        check(f"summarize: the cap {name} by Elo - err", summary["heading"] == heading and summary["wave"] == "4"
              and summary["games"] == "19,980 of 19,980 planned = 18 shards × 4 of 4 waves, undecided at the cap"
              and len(report.splitlines()) == 8)
    for name, wave3, reason in (
            ("a discard at wave 3", ("failure", discard), "Wave 3: " + discard["reason"]),
            ("a missing decision at wave 3", ("failure", {}), "Wave 3: " + UNDECIDED.format("failure")),
            ("a wave 3 that never ran", ("skipped", {}), "Wave 3: " + UNDECIDED.format("skipped"))):
        summary, report = summarize(needs(golden_plan, ("success", wave1), ("success", wave1), wave3), False)
        check(f"summarize: {name} discards the run, with no figures",
              summary == {**EMPTY_SUMMARY, "outcome": "discarded", "reason": reason,
                          "heading": "## Strength lab — DISCARDED"} and report == "")
    summary, report = summarize(needs(golden_plan, ("success", wave1),
                                      ("failure", complete("continue", [400, 1100, 1950, 1100, 454], "+1.00",
                                                           "wave 2: shard 1, 2 slices (runner lost)", pieces)),
                                      ("success", complete("H1", [550, 1600, 2900, 1700, 756], "+6.83"))), False)
    check("summarize: a repaired wave 2 then H1 at wave 3 names the wave and carries its plan",
          summary["heading"] == "## Strength lab — passes (SPRT H1, wave 3 of 4)"
          and summary["replayed"] == "wave 2: shard 1, 2 slices (runner lost)"
          and json.loads(summary["repair_plan"]) == [{"wave": 2, "slices": json.loads(pieces)}]
          and len(report.splitlines()) == 7)
    check("summarize refuses a decision its wave cannot make",
          raises(lambda: summarize(needs(golden_plan, *capped, ("success", complete("continue", CAP_FAIL))), False))
          and raises(lambda: summarize(needs(golden_plan, ("success", complete("cap", CAP_FAIL))), False))
          and raises(lambda: summarize(needs(golden_plan, ("success", complete("fixed", CAP_FAIL))), False))
          and raises(lambda: summarize(needs(fixed_plan, ("success", complete("H1", [0, 0, 4, 0, 0]))), False)))

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

        def plan_cli(sprt, calibration, games, shards, rounds, arms=""):
            return cli("plan", "--sprt", sprt, "--calibration", calibration, "--games", str(games), "--shards",
                       str(shards), "--rounds", str(rounds), "--opening-offset", "5", "--arms", arms)

        code, values, clean = plan_cli("true", "true", 28, 2, 7)
        check("CLI plan writes four waves for a calibration SPRT run", code == 0 and clean
              and values.get("waves") == "4" and json.loads(values.get("wave_plan", "null")) == wave_plan(2, 7, 5, 4)
              and " " not in values["wave_plan"])
        code, values, clean = plan_cli("true", "false", FULL_GAMES, FULL_SHARDS, 738)
        check("CLI plan writes four waves at the cap", code == 0 and clean and values.get("waves") == "4")
        code, values, clean = plan_cli("false", "false", 28, 2, 7, arms="X=1;X=2")
        check("CLI plan writes one wave without sprt", code == 0 and clean and values.get("waves") == "1"
              and json.loads(values["wave_plan"]) == wave_plan(2, 7, 5, 1))
        for name, args in (("an empty wave", ("true", "true", 12, 2, 3)),
                           ("sprt below the cap without calibration", ("true", "false", 20088, FULL_SHARDS, 558)),
                           ("sprt on other shards without calibration", ("true", "false", FULL_GAMES, 20, 664)),
                           ("sprt with arms", ("true", "true", 28, 2, 7, "X=1;X=2"))):
            code, values, clean = plan_cli(*args)
            check(f"CLI plan refuses {name} cleanly, writing nothing", code == 1 and clean and not values)
        code, values, clean = cli("decide", "--counts", "[290,326,951,446,489]", "--prior-counts", "", "--wave", "1",
                                  "--waves", "4")
        check("CLI decide writes H1, LLR and counts", code == 0 and clean
              and values == {"decision": "H1", "llr": "+5.67", "counts": "[290,326,951,446,489]"})
        code, values, clean = cli("decide", "--counts", "[225, 578, 921, 541, 219]", "--prior-counts",
                                  "[599, 1723, 2829, 1727, 628]", "--wave", "4", "--waves", "4")
        check("CLI decide adds the earlier waves and writes the cap, LLR to two decimals, counts compact",
              code == 0 and clean
              and values == {"decision": "cap", "llr": "-1.12", "counts": "[824,2301,3750,2268,847]"})
        code, values, clean = cli("decide", "--counts", "[0,0,1,0,0]", "--prior-counts", "", "--wave", "1",
                                  "--waves", "1")
        check("CLI decide writes fixed with an empty LLR", code == 0 and clean
              and values == {"decision": "fixed", "llr": "", "counts": "[0,0,1,0,0]"})
        for name, prior, wave in (("wave 2 without earlier counts", "", "2"),
                                  ("wave 1 with earlier counts", "[0,0,1,0,0]", "1")):
            code, values, clean = cli("decide", "--counts", "[0,0,1,0,0]", "--prior-counts", prior, "--wave", wave,
                                      "--waves", "4")
            check(f"CLI decide refuses {name} cleanly, writing nothing", code == 1 and clean and not values)
        shape = subprocess.run([sys.executable, os.path.abspath(__file__), "shape", "--wave-plan", compact(full),
                                "--wave", "2", "--slices", compact(full[1])], capture_output=True, text=True)
        check("CLI shape prints S R P OFFSET", shape.returncode == 0 and shape.stdout.split() == ["18", "738", "184", "185"])
        shape = subprocess.run([sys.executable, os.path.abspath(__file__), "shape", "--wave-plan", compact(full),
                                "--wave", "1", "--slices", compact(full[1])], capture_output=True, text=True)
        check("CLI shape refuses mismatched slices cleanly", shape.returncode == 1 and not shape.stdout
              and "Traceback" not in shape.stderr)
        report_path = os.path.join(root, "sprt.md")
        code, values, clean = cli("summarize", "--needs", compact(needs(golden_plan, ("success", wave1), (
            "success", complete("H0", [446, 1172, 1902, 1057, 427], "-2.97")))), "--calibration", "false",
                                  "--report", report_path)
        with open(report_path, encoding="utf-8") as handle:
            written = handle.read()
        check("CLI summarize writes the outcome and the SPRT block", code == 0 and clean
              and values["outcome"] == "H0" and values["counts"] == "[446,1172,1902,1057,427]"
              and set(values) == set(EMPTY_SUMMARY) and written.startswith("SPRT [0, 3]"))
        code, values, clean = cli("summarize", "--needs", "{", "--calibration", "false", "--report", report_path)
        check("CLI summarize refuses malformed needs cleanly, writing nothing", code == 1 and clean and not values)
        for bad in ("[1,2,3,4]", "not json", "[0,0,0,0,0]"):
            code, values, clean = cli("decide", "--counts", bad, "--prior-counts", "", "--wave", "1", "--waves", "4")
            check(f"CLI decide refuses {bad!r} cleanly, writing nothing", code == 1 and clean and not values)

    print("\nSPRT self-test " + ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")

    planning = commands.add_parser("plan", help="write the run's wave count and plan as waves and wave_plan")
    planning.add_argument("--sprt", choices=["true", "false"], required=True)
    planning.add_argument("--calibration", choices=["true", "false"], required=True)
    planning.add_argument("--games", type=int, required=True, help="the games requested")
    planning.add_argument("--shards", type=int, required=True)
    planning.add_argument("--rounds", type=int, required=True, help="pairs per shard")
    planning.add_argument("--opening-offset", type=int, required=True)
    planning.add_argument("--arms", required=True, help="the candidate_arms input")
    planning.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    deciding = commands.add_parser("decide", help="write decision, llr and counts for wave k")
    deciding.add_argument("--counts", required=True, help="wave k's pooled Ptnml(0-2), as JSON")
    deciding.add_argument("--prior-counts", required=True,
                          help="waves 1..k-1's cumulative counts as JSON; empty for wave 1")
    deciding.add_argument("--wave", type=int, required=True)
    deciding.add_argument("--waves", type=int, required=True)
    deciding.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    shaping = commands.add_parser("shape", help='print "shards rounds pairs offset" for plan_arms.py verify')
    shaping.add_argument("--wave-plan", required=True, help="sprt.py plan's whole plan, as JSON")
    shaping.add_argument("--wave", type=int, required=True)
    shaping.add_argument("--slices", required=True, help="this wave's slices, as JSON")

    summarizing = commands.add_parser("summarize", help="write the run's outcome for aggregate")
    summarizing.add_argument("--needs", required=True, help="strength.yml's toJSON(needs)")
    summarizing.add_argument("--calibration", choices=["true", "false"], required=True)
    summarizing.add_argument("--report", required=True, help="the file the SPRT block is written to")
    summarizing.add_argument("--output", required=True, help="the file GitHub reads step outputs from")

    args = parser.parse_args()
    if args.self_test:
        return self_test()
    try:
        if args.command == "shape":
            # Printed rather than written as outputs: the calling step reads it at once.
            print(*wave_shape(json.loads(args.wave_plan), args.wave, json.loads(args.slices)))
            return 0
        if args.command == "plan":
            waves = run_waves(args.sprt == "true", args.games, args.shards, args.calibration == "true", args.arms)
            plan = wave_plan(args.shards, args.rounds, args.opening_offset, waves)
            write_outputs(args.output, {"waves": waves, "wave_plan": compact(plan)})
            print(f"{waves} wave(s), cumulative pairs per shard {wave_cuts(args.rounds, waves)}")
            return 0
        if args.command == "decide":
            prior = json.loads(args.prior_counts) if args.prior_counts else None
            outputs = decide_outputs(cumulative(prior, json.loads(args.counts), args.wave), args.wave, args.waves)
            write_outputs(args.output, outputs)
            print(f"wave {args.wave} of {args.waves}: {outputs}")
            return 0
        if args.command == "summarize":
            summary, report = summarize(json.loads(args.needs), args.calibration == "true")
            with open(args.report, "w", encoding="utf-8") as handle:
                handle.write(report)
            write_outputs(args.output, summary)
            print(f"{summary['outcome']} at wave {summary['wave'] or '-'}")
            return 0
    except ValueError as error:  # json.JSONDecodeError is a ValueError
        print(f"::error::{error}", file=sys.stderr)
        return 1
    parser.error("a command is required")


if __name__ == "__main__":
    sys.exit(main())
