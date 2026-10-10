---
name: open-pull-request
description: Open or update a PR on this repo — code review, reviewer dispatch, New-PullRequest.ps1,
  PR body conventions and post-merge cleanup. Use when finishing a branch, opening/updating a PR,
  pushing review follow-ups, or cleaning up after a merge.
---

## 1. Review first

The one thing the script cannot do for you. Address every finding before step 2.

### Code review: every PR outside the Docs tier

`Scripts/Get-ChangeTier.ps1` prints the tier. Every tier except Docs gets one of three review
modes. The first hard trigger that applies picks **full**. Otherwise you pick, and size the review
to the risk:

- **inline:** a trivial fix whose cause and whole effect you can read off the changed functions and
  their callers, already shown working by a before/after run or a focused check. Review it in your
  own context against the inputs below. Example: the first commit of #780.
- **light:** anything else without a hard trigger. Both axes run in one fresh subagent on a cheaper
  model (Claude: `sonnet`; Codex: Luna 6). Where none can be selected, it runs on the session's
  model, recorded as `light (session model)`.
- **full:** each axis runs in its own subagent on the session's model.

Hard triggers, at any size:

- Engine or Build tier.
- A new executable file.
- A change to which paths get deleted, or the removal of a guard, safety check or recovery path.
- A change to what a measurement means: binary or option selection, pooling, statistics, validity.

Load skill `code-review` (Claude: `mattpocock-skills:code-review`, not the built-in
`/code-review`) and give it these inputs, so it never has to ask the user. The mode above replaces
only its dispatch step; its briefs, smell baseline and two-axis report still apply.

- **Fixed point:** `origin/main`.
- **Spec:** the issue the PR cites (`Closes`/`Refs #N`) plus any `.claude/plans/` document the
  branch added, including one deleted after Harvest:
  `git log --diff-filter=A --name-only origin/main..HEAD -- .claude/plans`, then `git show
  <sha>:<path>`. With neither, tell it "no spec available".
- **Standards sources:** `Docs/CodingStandards.md`, and `Docs/EngineContracts.md` for a diff
  under `StratEngine/`.
- **Append to the Standards brief:** "Also apply question 4 of `Docs/CodingStandards.md`. Report it
  under a separate `Nearby debt` heading with `file:line`; these items are not findings. List each behaviour the diff removes (a recovery path,
  a guard, a message) and whether anything still needs it."

Question 4 covers only the changed functions, so every mode applies it. A stale comment that slips
past a light review now and then is an accepted cost. An agent that cannot spawn subagents runs
light and full in its own context and records the mode with `(no subagents)`.

Every finding is fixed, rejected with a reason, or filed as an issue. A Spec finding rejected by
reading the spec differently edits the spec (the issue or plan) to state that reading, in the same
PR. Each nearby-debt item:

- **The change makes it worse** (copies the duplication, extends the workaround): a finding against
  this PR.
- **A stale comment or dead code:** fix it in this PR, in its own commit. The fix changes no
  behaviour and stays inside files the PR already touches.
- **Otherwise** (duplication, a workaround): drop it. Periodic smell sweeps cover it, so file no
  issue.

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

They run on the session's model whatever the code-review mode. **Default is to dispatch**; the
script only reminds, it never blocks. A narrow self-certification
carve-out exists for logging-only diffs — its six conditions are in `Docs/Workflow.md` → When
`search-reviewer` may be skipped. Read them before claiming a skip, and state the skip in the PR
body so it is auditable.

Brief a reviewer with the diff as a file and the tests already run with their results. Write review
files as UTF-8 into the worktree's `build/`, which git ignores: a path outside the worktree can be
unreadable to a reviewer's tools, and Windows PowerShell's `>` writes UTF-16, which reads as binary.
Create `build/` if it is missing (a Docs or Tooling change never builds), then
`git diff origin/main...HEAD --output=build/review.diff` does both. Brief neutrally; adjudicate every finding it raises.
Warnings in test output are findings. Address all findings in one pass, recording why any is
rejected.

**A reviewer that fails to run** (it cannot reach a tool, or errors before reviewing) gets one retry
with the cause fixed in its brief. After a second failure, review that axis inline and record
`inline`. Check on a running reviewer at most every 15–20 minutes: each check re-reads the
controller's whole context.

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

**Changelog:** follow `Docs/Changelog.md`'s heading convention, using today's Copenhagen date
provisionally instead of `Unreleased`. After merge, the cleanup's `-SyncMaster` warns about a
header dated other than its merge day; correct it in the next PR.

## 3. PR body

Written manually as **Summary / Test plan / Notes** — `--body-file` only, never inline `--body`
(backtick spans execute in bash and post a mangled public comment). `New-PullRequest.ps1 -BodyFile`
bypasses `.github/pull_request_template.md`, so supply the structure yourself.

- Auto-close needs GitHub's exact keywords: `Closes #N` / `Fixes #N` / `Resolves #N`. "closing #N" is
  prose and leaves the issue open.
- State **which approved design decisions changed during implementation, and why** — the design
  review approved the doc, not the diff, and this is where the user sees what moved.
- **A PR that changes the engine binary** (Engine tier, or a compiler flag) carries a **Measurement**
  line in its Test plan: the instrument and its result (equivalence identical, a bench nps delta, an
  SPRT or lab Elo), the run still pending, or why none applies. Load skill `measure-strength` to pick
  the instrument, and to check one you already ran: its rules catch silently invalid results.
- **A PR outside the Docs tier** carries a **Review** line in its Test plan:
  `Review: <mode>, Standards n / Spec m: x fixed, y rejected, filed #a #b; cost tk tokens, s min`.
  The cost sums the subagent reports; it feeds the value-versus-cost call on this review, so write
  `cost unknown` rather than estimate. Write `Spec: skipped (no spec)` when no spec was given. An
  inline review gives its reason instead of a cost: `Review: inline — <why it is trivial>`.
  Notes lists each rejected finding on its own line: the finding in a few words, then the reason.
- Include motivation, design reasoning and expected impact for anything non-trivial. Keep it short;
  detail goes in chat.
- Update the body when a follow-up commit fulfils a "will do X later" note in it.

## 4. Review rounds

**Batch follow-ups into one push.** `Get-ChangeTier.ps1` classifies the whole PR diff
(`origin/main...HEAD`), not the latest commit, so a comment-only fix on an Engine PR still reruns the
full Engine tier locally and in CI. Collect the round's findings, address them together, push once.

Check green with `Get-PrChecks.ps1 [-Pr n] [-Wait]` (exit 0 green / 1 failed / 2 running).

A pushed PR gets no cross-agent round; step 1 is its review. Report it as **awaiting merge**
once checks are green: the user merges.

## 5. After it merges

The user merges via the GitHub web UI. Cleanup is part of finishing the task, not something they
should have to ask for:

```
# per-task worktree
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Remove-Worktree.ps1 -Name <task> -SyncMaster [-FromInside]

# Codex-managed worktree (use the registered path shown by Get-Worktrees.ps1)
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Remove-Worktree.ps1 -Path <registered-worktree-path> -SyncMaster

# working in place
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Remove-MergedBranches.ps1 -SyncMaster
```

The cleanup scripts verify the merge before deleting. Squash-merges and locked directories need care:
`Docs/Workflow.md` → Worktree removal gotchas.
