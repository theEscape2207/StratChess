---
name: triage-issue
description: Investigate and refine a GitHub issue in this repository, clarify its correctness, playing-strength, and performance impact with an honest magnitude, recommend an implementation plan or closure, and reconcile priority, category, and readiness labels. Use when asked to triage, clarify, scope, label, make ready, or recommend closing an issue.
---

# Triage a repository issue

Triage produces a decision-ready issue, not a longer restatement of the report. Completion is one of:

- an updated issue description containing evidence, a materially clearer Why, and a reviewable plan
  of attack; or
- a documented recommendation to close, with evidence showing that the issue is resolved, a
  duplicate, invalid, unactionable, or not worth its cost.

Label changes must agree with that outcome.

## Load the repository rules

Read these before investigating:

- `CLAUDE.md`;
- `Docs/agents/issue-tracker.md` for GitHub operations and body-file requirements;
- `Docs/agents/triage-labels.md` for readiness-role mappings; and
- `Docs/agents/domain.md`, then the domain documents it routes to when they exist.

Read any area-specific contract or workflow document named by `CLAUDE.md` before drawing conclusions
about that area.

## Establish the current state

Fetch the issue body, comments, state, and labels with `gh issue view`. Fetch the current label catalog
with `gh label list`; the live tracker is authoritative. Never invent or create a label during triage.

Search the code, tests, documentation, issues, pull requests, and relevant history far enough to answer:

- Does the reported behavior still exist on the current default branch?
- Is another issue or merged change already the real owner?
- What prerequisites or unresolved decisions block implementation?
- Which claims are measured facts, bounded estimates, external precedent, or speculation?

Prefer a focused spike when a cheap experiment can replace speculation. Record the command, input,
result, and limitation in the issue; do not keep throwaway spike code unless the user separately asks
for an implementation.

## Clarify Why and magnitude

Cover each applicable impact axis explicitly. Say "no material impact" when an axis genuinely does not
apply rather than manufacturing a benefit.

### Correctness and quality

Identify the violated contract and observable failure mode. Distinguish chess-rules correctness,
engine robustness, evaluation/search quality, statistical validity, tooling correctness, and
maintainability. State who or what encounters the problem and under which conditions.

### Playing strength

For an engine-behavior change, state the plausible Elo effect and the evidence behind it. Preserve
error bars and experimental conditions when citing project measurements. Do not convert nps into Elo,
present external-engine results as this engine's forecast, or call an unresolved point estimate a
measurement. Use skill `measure-strength` when choosing or interpreting a benchmark, match, or SPRT.

### Runtime and operational performance

Identify whether the change affects per-node search, static evaluation, startup, scripts, CI, or an
offline workflow. Quantify both relative and absolute magnitude when evidence permits: percent, nps,
positions per second, wall-clock time, corpus size, CI minutes, or expected frequency. Separate runtime
cost from one-time development or training cost. Name the benchmark needed when magnitude is unknown.

Use calibrated language:

- **measured** only with a reproducible result and its uncertainty or run-to-run spread;
- **bounded estimate** when code structure or a spike supports a range;
- **unknown until measured** when neither exists.

## Choose the outcome

### Keep and plan

Rewrite or extend the issue body so a reviewer can find, without reading the triage conversation:

1. the current problem and evidence;
2. Why it matters across the applicable axes above;
3. scope, non-goals, dependencies, and unresolved decisions;
4. a suggested plan split into independently reviewable slices;
5. acceptance and validation criteria; and
6. a readiness gate when choices or prerequisites remain.

Plans should identify contracts and evidence, not prescribe speculative implementation details. Keep
valuable original context and links, but remove claims disproved by the investigation.

### Recommend closure

State the recommended disposition and evidence. Link the duplicate, resolving commit/PR, superseding
design, failed premise, or cost/benefit reason. Explain any residual risk or follow-up. Do not close the
issue unless the user explicitly asked for closure; a triage request authorizes the documented
recommendation and label reconciliation, not irreversible disposition by assumption.

## Reconcile labels

Inspect descriptions in the live label catalog rather than inferring semantics from names.

- Remove `needs-triage` when the investigation is complete.
- Add `ready-for-agent` only when scope, dependencies, acceptance criteria, and validation are settled
  well enough for an unattended implementation. Remove it when any material choice remains.
- Treat missing canonical roles such as `needs-info` or `ready-for-human` as prose states unless the
  tracker actually contains those labels.
- Choose priority from demonstrated magnitude, urgency, dependencies, and the repository's live label
  descriptions. `priority:critical` retains its project-specific meaning; it is not generic severity.
- Apply every category that materially owns the work, but remove categories made stale by the revised
  scope. Reconcile type and disposition labels (`bug`, `enhancement`, `wontfix`, `duplicate`, etc.) the
  same way.

Before mutating labels, summarize the intended additions and removals and check that every target label
exists.

## Publish safely and verify

Write the complete body to a UTF-8 file and update with `gh issue edit <number> --body-file <path>`.
Every non-trivial comment also uses `--body-file`; never interpolate Markdown or Unicode through an
inline shell body. Apply labels with explicit `--add-label` and `--remove-label` arguments.

Fetch the issue again after publishing. Verify the title/body, key numeric findings, non-ASCII
characters, headings, and final label set. If verification differs, repair from the body file before
reporting completion.

Report the outcome, evidence gathered, issue fields changed, labels added/removed, remaining blockers,
and whether the issue is ready for an agent.
