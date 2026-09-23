---
name: open-pull-request
description: Open or update a PR on this repo — code review, reviewer dispatch, New-PullRequest.ps1,
  PR body conventions and post-merge cleanup. Use when finishing a branch, opening/updating a PR,
  pushing review follow-ups, or cleaning up after a merge.
---

## 1. Review first

The one thing the script cannot do for you. Address every finding before step 2.

### Code review: every PR outside the Docs tier

`Scripts/Get-ChangeTier.ps1` prints the tier. Every tier except Docs gets this review, whatever its
size. Load skill `code-review` (Claude: `mattpocock-skills:code-review`, not the built-in
`/code-review`) and give it these inputs, so it never has to ask the user:

- **Fixed point:** `origin/main`.
- **Spec:** the issue the PR cites (`Closes`/`Refs #N`) plus any `.claude/plans/` document on the
  branch. With neither, tell it "no spec available".
- **Standards sources:** `Docs/agents/simplify.md` and CLAUDE.md → Development Guidelines.
- **Append to the Standards brief:** "Also apply question 4 of `Docs/agents/simplify.md`: scan
  every function the diff changes and the rest of each touched file, no other file. Report it under
  a separate `Nearby debt` heading with `file:line`; these items are not findings."

Run each axis in its own subagent, in parallel or one after the other. An agent that cannot spawn
subagents runs both in its own context, and the Review line (step 3) records `inline`.

Every finding is fixed, rejected with a reason, or filed as an issue. Each nearby-debt item:

- **The change makes it worse** (copies the duplication, extends the workaround): a finding against
  this PR.
- **Otherwise**, if you would accept a PR to fix it, file a new issue: search open issues first
  (`gh issue list --search`), label `needs-triage`, and give `file:line` and this PR in the body.
  Drop anything smaller. Never fix it in this PR.

### Specialised reviewers

Check the diff and dispatch if it touches the domain:

```
git diff --name-only origin/main...HEAD
```

- `Eval.cpp/.h` → `eval-reviewer`. **`Eval.h` counts** — every term weight lives there
  (`PASSED_PAWN_*`, `BISHOP_PAIR_*`, `CONNECTED_ROOKS_*`, `CASTLING_*`, the mobility weights,
  `PASSED_PAWN_RANK_SCALE`), so the whole evaluation can be retuned without touching `Eval.cpp`.
- `defines.h` → `eval-reviewer` **when the diff touches `g_Eval_Bitboards` (the PSTs) or
  `g_iPieceValues` (material values)**. They live there, not in `Eval.cpp`. Other `defines.h` edits
  do not need it.
- `AIPerplex.cpp/.h`, `ThreadData.h` (killers/history), `Sort.cpp/.h` (MVV-LVA) → `search-reviewer`

**Default is to dispatch**; the script only reminds, it never blocks. A narrow self-certification
carve-out exists for logging-only diffs — its six conditions are in `Docs/Workflow.md` → When
`search-reviewer` may be skipped. Read them before claiming a skip, and state the skip in the PR
body so it is auditable.

Brief a reviewer with the diff as a file (`git diff origin/main...HEAD > <scratchpad>/review.diff`)
and the tests already run with their results. Brief neutrally; adjudicate every finding it raises.
Warnings in test output are findings. Address all findings in one pass, recording why any is
rejected.

## 2. Open it

```
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\New-PullRequest.ps1 -Title "…" [-Draft] [-NoPr] [-BodyFile <path>]
```

Sync → validate → push → create/update, stopping at the first failure. Its validation step scopes
itself to the change tier, so there is no judgement call to make (tier table and fail-closed
guarantees: `Docs/Workflow.md` → Validation tiers). Engine tier runs ~2 min warm, ~5 min cold; a
*failed* run exits early, not late.

**Never bypass it with a bare `git push`** to update an open PR: the push succeeds but
`Validate-PrePR.ps1` never runs, leaving the merged state covered only by the pre-commit hook (this
happened on PR #148). Same rule for creation — never `gh pr create`.

If a build-freshness check fails right after a `CMakeLists.txt` edit or a merge, delete both exes and
rebuild before retrying — it is a known false positive.

## 3. PR body

Written manually as **Summary / Test plan / Notes** — `--body-file` only, never inline `--body`
(backtick spans execute in bash and post a mangled public comment). `New-PullRequest.ps1 -BodyFile`
bypasses `.github/pull_request_template.md`, so supply the structure yourself.

- Auto-close needs GitHub's exact keywords: `Closes #N` / `Fixes #N` / `Resolves #N`. "closing #N" is
  prose and leaves the issue open.
- State **which approved design decisions changed during implementation, and why** — the specialised
  reviewers read the diff while the cross-agent reviewer reads the design doc, and nothing else
  checks that the two still agree.
- **A PR that changes the engine binary** (Engine tier, or a compiler flag) carries a **Measurement**
  line in its Test plan: the instrument and its result (equivalence identical, a bench nps delta, an
  SPRT or lab Elo), the run still pending, or why none applies. Load skill `measure-strength` to pick
  the instrument, and to check one you already ran: its rules catch silently invalid results.
- **A PR outside the Docs tier** carries a **Review** line in its Test plan:
  `Review: code-review, Standards n / Spec m: x fixed, y rejected (reasons), filed #a #b`. Write
  `Spec: skipped (no spec)` when no spec was given, and `inline` when both axes ran in one context.
- Include motivation, design reasoning and expected impact for anything non-trivial. Keep it short;
  detail goes in chat.
- Update the body when a follow-up commit fulfils a "will do X later" note in it.

## 4. Review rounds

**Batch follow-ups into one push.** `Get-ChangeTier.ps1` classifies the whole PR diff
(`origin/main...HEAD`), not the latest commit, so a comment-only fix on an Engine PR still reruns the
full Engine tier locally and in CI. Collect the round's findings, address them together, push once.

Check green with `Get-PrChecks.ps1 [-Pr n] [-Wait]` (exit 0 green / 1 failed / 2 running).

A **cross-agent review** — a second agent reviewing the design doc and commenting on the PR before
merge — is routed by the user, not dispatched from here. So report a pushed PR as **awaiting
review**, not done. Answering it: skill `cross-agent-review`.

## 5. After it merges

The user merges via the GitHub web UI. Cleanup is part of finishing the task, not something they
should have to ask for:

```
# per-task worktree
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Remove-Worktree.ps1 -Name <task> -SyncMaster [-FromInside]

# working in place
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Remove-MergedBranches.ps1 -SyncMaster
```

Both verify the merge before deleting. Squash-merges and locked directories need care:
`Docs/Workflow.md` → Worktree removal gotchas.
