---
name: cross-agent-review
description: Review a design doc, plan, spec or issue another agent wrote, or answer the review of your own. Use when asked to review or cross-review such an artifact, or to address a `*.review.md` file or review comment.
---

# Cross-agent review

A second agent reviews an artifact before work or merge; the user routes every hand-off. Why, what is
worth sending, and how this splits from `search-reviewer` / `eval-reviewer`: `Docs/Workflow.md` →
Cross-agent review.

## The channel

- **A file in a worktree** (a draft design doc or plan): the review is `<artifact>.review.md` beside
  it — `foo.md` → `foo.review.md`. One review covering several files takes the primary file's name.
  `*.review.md` is gitignored: never committed.
- **An issue or a pushed PR**: a comment, through `--body-file`.

Either way, the artifact's author edits the artifact; the reviewer edits only the review.

## Ranks

| Rank | Meaning |
|---|---|
| **Blocking** | Proceeding without it risks a wrong or unverifiable result |
| **Add** | A real gap worth closing, but the artifact is sound without it |
| **Clarify** | Wording or framing, no behaviour at stake |

## Reviewer

1. Read the artifact, its originating issue with comments, and every source file, script and doc it
   cites, at current `origin/main`. Aim hardest at the design doc's "Assumptions I cannot verify from
   the code" section and at every number: re-derive it, or find who measured it.
2. Look for **provenance** (a number nobody measured, a default that has since changed) and **logical
   form** (a dichotomy that does not hold, a gate that cannot fail). Also look for **proportion**: a
   plan heavier than its diff, or a check that is true by construction.
3. Write the review: a header (reviewer model, date, baseline commit, scope), one line naming what
   you verified as correct, then numbered findings under the three ranks. Each finding cites
   `file:line` evidence and proposes a concrete change.
4. Report to the user: the review's path or comment URL, and the count per rank.

Done when every cited claim is verified, refuted or marked unverified, and every finding is ranked
with evidence.

## Author

1. **Adjudicate** each finding on its merits. The reviewer is strong on provenance and logic, weaker
   on what is worth changing, so push back with reasoning where a point does not hold.
2. Under each finding write `**Disposition:** accepted — <what changed>`, `rejected — <reason>` or
   `deferred — <where tracked>`. A Blocking finding is closed with evidence proportionate to the
   claim: source, a specification, a focused test or explicit reasoning where it settles the
   question, and a measurement when the claim is about runtime or external behaviour.
3. Edit the artifact, then report to the user: counts per disposition, and any rejected Blocking
   finding by name.

Done when every finding carries a disposition.

## Lifecycle

**One round per artifact.** A second round happens only for a disputed or newly found Blocking
finding, and covers only those. When the artifact lands, carry each rejected finding and its reason
into the PR body so the exchange stays auditable, then delete the `.review.md`.
