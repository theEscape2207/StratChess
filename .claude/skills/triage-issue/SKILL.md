---
name: triage-issue
description: Investigate and refine a GitHub issue in this repository, clarify its correctness, playing-strength, and performance impact with an honest magnitude, recommend an implementation plan or closure, and reconcile priority, category, and readiness labels. Use when asked to triage, clarify, scope, label, make ready, or recommend closing an issue.
---

# Triage a repository issue

Finish with either an evidence-backed issue and reviewable plan, or a documented recommendation to
close. Labels must agree with that outcome.

## Start with repository evidence

Read `CLAUDE.md`, then:

- `Docs/agents/issue-tracker.md` for GitHub operations and body-file rules;
- `Docs/agents/triage-labels.md` for canonical readiness-role mappings; and
- `Docs/agents/domain.md`, including the domain documents it routes to when they exist.

Fetch the issue body, comments, state, and labels, plus the live label catalog. Search current code,
tests, docs, issues, PRs, and relevant history far enough to establish whether the problem still
exists, who owns it, what blocks it, and whether each claim is measured, estimated, or speculative.

A cheap local probe may replace speculation; record its command, input, result, and limitation. Triage
may name an Elo match, SPRT, or other paid measurement, but never starts one—the measurement budget is
the project owner's call. Use `measure-strength` to choose and interpret the instrument.

## Make the Why concrete

- **Correctness / quality:** name the violated contract and observable failure. Distinguish chess-rule
  correctness, robustness, evaluation/search quality, statistical validity, tooling, and maintenance.
- **Playing strength:** give a plausible Elo magnitude only with evidence. Preserve error bars and
  conditions; never convert nps to Elo or present another engine's result as this engine's forecast.
- **Runtime / operations:** identify the affected path and quantify relative and absolute cost where
  possible—nps, percent, positions/s, wall time, corpus size, CI minutes, or frequency. Separate runtime
  cost from one-time development or training cost.

Call a result **measured** only with a reproducible result and uncertainty or spread; call it a
**bounded estimate** only when code or a probe supports a range; otherwise say **unknown until
measured** and name the needed benchmark.

## Publish without erasing history

Preserve the reporter's original problem statement unless the user explicitly requested a rewrite or
closure recommendation. Default unattended triage publishes the full investigation as a comment with
`gh issue comment <n> --body-file <path>`, then adds a short dated body section linking its stable
`#issuecomment-<id>` URL. Obtain comment URLs with:

```text
gh issue view <n> --json comments --jq '.comments[] | {url, createdAt}'
```

When a rewrite is explicitly authorized, preserve useful original context while correcting disproved
claims. Follow `Docs/agents/issue-tracker.md` rather than restating its mutation commands.

For a kept issue, make the finding cover current evidence; impact and magnitude; scope, non-goals,
dependencies, and unresolved choices; reviewable slices; acceptance criteria; and the readiness gate.
State whether `CLAUDE.md`'s design-document trigger applies before implementation, and why.

For closure, link the duplicate, resolving change, superseding design, failed premise, or cost/benefit
reason and state residual risk. Do not close unless the user explicitly asked for closure.

## Reconcile labels

- Remove `needs-triage` once investigation is complete.
- Add `ready-for-agent` only when scope, dependencies, choices, acceptance criteria, and validation are
  settled; remove it when a material decision remains.
- `priority:critical` requires the issue to be blocking or explicit agreement from the project owner.
- Apply every materially owning category; remove stale category, priority, readiness, type, and
  disposition labels.

`Docs/agents/triage-labels.md` wins for canonical readiness-role mapping; the live tracker wins for
existence and label descriptions. If they conflict, report the drift and do not mutate the affected
label. Summarize intended label additions/removals before applying them.

## Verify

Re-fetch after publishing and compare the title/body, linked comment, key numbers, non-ASCII text,
headings, and labels with the body files and intended label set. Repair any mismatch before reporting
the outcome, changed fields, remaining blockers, and readiness.
