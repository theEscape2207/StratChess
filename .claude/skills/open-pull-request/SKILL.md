---
name: open-pull-request
description: Open or update a PR on this repo — reviewer dispatch, New-PullRequest.ps1, PR body
  conventions and post-merge cleanup. Use when finishing a branch, opening/updating a PR, pushing
  review follow-ups, or cleaning up after a merge.
---

## 1. Dispatch a specialised reviewer first

The one thing the script cannot do for you. Check the diff and dispatch if it touches the domain:

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
body so it is auditable. Address findings before opening the PR.

## 2. Open it

```
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\New-PullRequest.ps1 -Title "…" [-Draft] [-NoPr] [-BodyFile <path>]
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
review**, not done. What to send and how to rank findings: `Docs/Workflow.md` → Cross-agent review.

## 5. After it merges

The user merges via the GitHub web UI. Cleanup is part of finishing the task, not something they
should have to ask for:

```
# per-task worktree
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Remove-Worktree.ps1 -Name <task> -SyncMaster [-FromInside]

# working in place
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Remove-MergedBranches.ps1 -SyncMaster
```

Both verify the merge before deleting. Squash-merges and locked directories need care:
`Docs/Workflow.md` → Worktree removal gotchas.
