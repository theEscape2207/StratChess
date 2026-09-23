---
name: analyze-games
description: Analyse strength-lab games or a position corpus to find what the engine plays wrong, or scope such a sweep as an issue. Use when asked to analyse, sweep, scan or mine games, PGNs or positions, or to find where the engine loses Elo.
---

# Analyse engine games

The deliverable is a **finding**: *the engine does X in positions like Y, the code that does it is
at Z, and changing it would mean W.* #598 is the model — "king safety is designed for ~600 cp of
swing (`Eval.h:516`) and never exceeds 71 cp". A number about the evaluation (a slope, a
correlation, a calibration line) is a **lead**: turn it into a chess statement or drop it.

## 1. Write the hypothesis card

Before downloading or querying anything, write four lines where the owner will read them:

- **Symptom** — what the engine does on the board, in chess terms.
- **Suspect** — the code that could cause it, as `file:line`, or "unnamed — the sweep's job".
- **Decides** — which result leads to which engine change, and which result kills the lead.
- **Budget** — what the owner set; otherwise one pass and one re-cut (step 3).

Done when every line is filled. An open request ("look at the games") gets a symptom that is a
question about play — "where do contested games turn lost?" — never a question about the instrument.

**Scoping only** (asked to file a sweep, not run it): the issue body is the card, the corpus (run
id, game count) and the expected output — "candidate engine issues, each a finding". File it per
`Docs/agents/issue-tracker.md` and stop.

## 2. Get the data

Read `reference/instruments.md` before the first query: which script answers which question,
where corpora already sit, and the traps that invert a headline. Persist the joined rows in
`StratChessSupport\` and query them ad hoc — each question needs its own cut.

## 3. Pilot, one pass, then the checkpoint

**Pilot on one shard first.** Run the whole pipeline end to end on a single shard: self-checks
pass, the reference's traps are checked, and the output has the shape that answers the card.
Repairs happen here, where a re-run costs minutes. Then run the full pass; a long one is fine —
start it in the background and check in, rather than holding the session open.

Run the cheapest cut that can confirm or kill the card. At its end, list candidate findings:

- **One or more** → step 4.
- **None** → re-cut the same rows once, by position class (phase, material balance, pawn structure,
  contested band), with the instrument unchanged.
- **None after the re-cut** → stop. Report in five lines what was cut and why the card's answer is
  "no engine change". A negative result is a finished analysis.

**Repair, don't refine.** When the instrument gives a *wrong* answer — a failed self-check, a
selection bias, a trap from the reference — repair it and re-run the same pass, once. A second
wrong answer from the full pass ends the analysis: report what failed and file the repair as a
tooling issue. Re-cuts of persisted rows never count against this. When it gives a correct answer that is not yet a finding, it stays as it is: a tighter error
bar, a statistics layer or a new report does not move the card. A capability it lacks entirely is a
one-paragraph tooling issue for the owner. A review that shows a statistic is wrong gets a repair;
a review that asks more of a statistic no finding rests on gets that statistic removed.

## 4. File each finding

One issue per candidate change, body per `triage-issue` → Make the Why concrete, shaped as:

- the chess statement and the code, `file:line`;
- reproducing FENs — aim for three; a rare but reproducible defect files with the one it has;
- the magnitude — how many games or positions of the corpus, and what it costs — labelled
  measured, bounded or unknown, with its population and denominator (which filter, how many rows
  it kept of how many);
- the change it points to, and the measurement that would validate it (proposed, never started:
  the budget is the owner's — `measure-strength`);
- supporting statistics in a collapsed `<details>` block.

Then reconcile labels per `triage-issue`. Done when every candidate from the checkpoint is an issue
or dropped with a stated reason. Report the card, finding → issue number, and what was dropped.
