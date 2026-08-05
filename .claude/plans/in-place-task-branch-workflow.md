# In-place task branches: one worktree, N sequential branches

## Goal

Make "one worktree, many sequential task branches" a first-class workflow alongside the existing
per-task worktree model, by moving the two invariants it depends on out of the driver's discipline
and into the tooling — and by fixing the defect the mode exposed: `Sync-Master.ps1` cannot run from
a worktree at all, so in a mode that never removes worktrees, nothing ever syncs `master`.

## The two modes, and why both are worth having

| | Per-task worktree | In-place task branches |
|---|---|---|
| Start | `New-Worktree.ps1 -Name x` | `New-TaskBranch.ps1 -Name x` |
| Finish | `Remove-Worktree.ps1 -Name x -SyncMaster` | `Remove-MergedBranches.ps1 -SyncMaster` |
| Parallel tasks | Yes — park one, switch to another | No — sequential only |
| Build directory | Cold per worktree; first build needs network | Stays warm across tasks |
| Path depth | Deeper (`.claude/worktrees/<name>/…`) | Repository root |
| Cleanup failure mode | Orphaned directories and unregistered worktrees | `git branch -D` |

Neither dominates. The worktree model is right when work must be parked and resumed, or when two
tasks are genuinely in flight. In-place is right for a run of small sequential PRs — four of them in
one session, in the case that prompted this — where per-task directory churn buys nothing and the
warm build directory is worth real minutes.

**The catch this plan addresses:** the worktree model enforces two invariants *structurally*, and
in-place mode leaves both to discipline.

1. **Every task forks fresh from `origin/main`.** `New-Worktree.ps1` fetches and forks from
   `origin/main` unconditionally and never reads the current branch. In-place, a hand-typed
   `git checkout -b` can branch off the previous task, dragging its commits into the next PR.
2. **Uncommitted work cannot leak between tasks.** Separate working trees make it impossible.
   In-place, `git checkout` carries a dirty tree across branches whenever it can do so without
   conflict.

## Key findings

Established while writing this plan; each one changes a step.

1. **The two scripts resolve their target repository differently, and only one is right.**
   `New-Worktree.ps1` asks git (`rev-parse --path-format=absolute --git-common-dir`) and is
   documented as safe from anywhere. `Sync-Master.ps1` derives its root from `$PSScriptRoot` — the
   copy of the file that was invoked — so running a worktree's copy makes it operate on that
   worktree and attempt `git checkout master`, which git refuses because `master` is checked out in
   the main checkout.
2. **`Sync-Master.ps1` already computes the right answer, and throws it away.** Its failure path
   greps `git worktree list` for `[master]` to tell the user where to go. Promoting that from
   diagnostic to routing is most of the fix.
3. **The stash stack is shared across all worktrees** (CLAUDE.md). `Sync-Master.ps1` stashes
   uncommitted changes by design, so a delegated sync must **refuse** on a dirty target rather than
   stash — another session could pop those changes.
4. **Nothing keys on the `worktree-` branch prefix.** `delete-merged-branch.yml` deletes any merged
   PR head branch except `master`/`main`; `Remove-Worktree.ps1` resolves branches by worktree path.
   The prefix is therefore a free choice, so it stays as-is: one prefix, zero migration, and it
   simply stops meaning "has a directory".
5. **`Get-ChangeTier.ps1` fails closed on new scripts.** Anything unrecognised under `Scripts/` is
   Engine tier, with a self-test (`FAIL CLOSED: new script`) asserting it. New tooling must be
   enumerated deliberately — which makes this change Build tier, since it edits the classifier.
6. **`Remove-Worktree.ps1` is 217 lines of hard-won traps** — refusing to remove the worktree you
   stand in, resolving branches from `worktree list --porcelain`, catching detached auto-mode
   worktrees with orphaned sibling branches. None of it may be re-implemented.
7. **Git refuses even a ref-only update to a branch checked out elsewhere.** Verified:
   `git fetch origin main:master` from a worktree returns *"refusing to fetch into branch
   'refs/heads/master' checked out at …"*. There is no worktree-side workaround, which is why
   delegation is the only design available.

## Design decisions

**A parallel verb set, not `-InPlace` flags on the existing scripts.** `New-Worktree.ps1 -InPlace`
would create no worktree, so the name lies at every call site, and both existing scripts would grow a
second path through logic that is already subtle (finding 6). Separate small scripts keep each name
honest and leave the worktree scripts untouched.

**The delegated sync refuses rather than stashes.** Finding 3. `Sync-Master.ps1` keeps stashing when
it operates on its own worktree, because those are the caller's own changes; it must not do so to
another working copy.

**Merged-ness is verified, never inferred from a name.** `git merge-base --is-ancestor <branch>
origin/main`, per the project's own rule after an earlier near-miss. `git branch -d` is not
sufficient: it asks whether the branch merged into the *current* branch, which is the wrong question.

**A merged current branch is reported, not auto-switched.** Silently moving someone's HEAD is
surprising; `New-TaskBranch.ps1` is the thing that moves you off it, deliberately.

**Untracked files do not block `New-TaskBranch.ps1`.** They survive a checkout unchanged and cannot
leak into a commit by themselves; `build/` is gitignored and would otherwise trip every run. Only
tracked modifications block.

## Files changed

| File | Change |
|---|---|
| `StratChessEvolved/Scripts/Sync-Master.ps1` | Route to the worktree holding `master`; refuse on a dirty delegated target |
| `StratChessEvolved/Scripts/New-TaskBranch.ps1` | **New** — fork `worktree-<name>` from `origin/main` in the current worktree |
| `StratChessEvolved/Scripts/Remove-MergedBranches.ps1` | **New** — delete verified-merged local branches; optional `-SyncMaster` |
| `StratChessEvolved/Scripts/Get-ChangeTier.ps1` | Classify both new scripts as Tooling; one self-test case each |
| `CLAUDE.md` | Two script rows; a note that both modes exist and what each guarantees |
| `Docs/Workflow.md` | The two modes, the two invariants, and which script enforces each |

## Implementation steps

### Step 1 — `Sync-Master.ps1` routing

1. After resolving `$RepoRoot` as today, parse `git worktree list --porcelain` into path → branch.
2. Find the worktree whose branch is `refs/heads/master`. Three cases:
   - **It is the current worktree** → existing code path unchanged, including the stash logic.
   - **It is another worktree** → set the git target to that path and take the delegated path below.
   - **No worktree holds `master`** (e.g. every checkout is detached) → existing code path; the
     `git checkout master` it performs will succeed because nothing holds the branch.
3. Delegated path:
   - Refuse if `git -C <path> status --porcelain` is non-empty, or a merge/rebase is in progress
     (`MERGE_HEAD`, `rebase-merge/`, `rebase-apply/`). Print the path and stop; do not stash.
   - `git fetch origin main` — refs and objects are shared by every worktree, so this needs no
     `-C` and is safe from anywhere.
   - `git -C <path> merge origin/main --ff-only`, falling back to a real merge on divergence,
     reusing today's reporting.
   - Never switch branches and never stash: `master` is already HEAD at that path.
4. Update `.NOTES`, which currently instructs the reader to run the script from the main checkout.

### Step 2 — `New-TaskBranch.ps1`

Modelled on `New-Worktree.ps1`, sharing its validation verbatim where it applies.

1. `-Name` mandatory, kebab-case-validated with the same regex and the same error text;
   `-BranchName` override; derived name is `worktree-<Name>`.
2. Refuse outside a git repository.
3. Refuse if the working tree has **tracked** modifications (`git status --porcelain` lines whose
   status is not `??`). List them. Untracked files pass with no comment.
4. Warn — do not block — if the current branch holds commits not in `origin/main`
   (`git rev-list --count origin/main..HEAD` > 0), naming the count and the branch.
5. `git fetch origin main`; report the resolved `origin/main` short SHA as `New-Worktree.ps1` does.
6. Refuse if the branch already exists.
7. `git checkout -b <branch> origin/main`.
8. Print the same "Next:" block as `New-Worktree.ps1`, minus the `cd`.

### Step 3 — `Remove-MergedBranches.ps1`

1. `-SyncMaster` and `-DryRun` switches.
2. `git fetch origin main` so ancestry is judged against current `origin/main`.
3. Collect branches checked out anywhere via `git worktree list --porcelain`.
4. For each local branch except `master`, `main`, the current branch and anything from (3):
   `git merge-base --is-ancestor <branch> origin/main` → delete with `git branch -D`, or list under
   `-DryRun`.
5. Report skipped branches with the reason (unmerged / checked out elsewhere / current).
6. If the **current** branch is itself merged, say so and point at `New-TaskBranch.ps1`.
7. `-SyncMaster` → invoke `Sync-Master.ps1` from `$PSScriptRoot`, which Step 1 has made
   location-independent.

### Step 4 — classifier and docs

1. `Get-ChangeTier.ps1`: a `Tooling` rule for each new script, placed with the other enumerated
   helpers, plus a self-test case each. Run `-SelfTest`.
2. `CLAUDE.md`: two rows in the Scripts table; amend the `Sync-Master.ps1` row, which currently
   implies a location constraint that will no longer exist.
3. `Docs/Workflow.md`: the two modes, when each is right, the two invariants, and the enforcing
   script for each.

## Validation plan

Tooling tier: `Validate-PrePR.ps1` syntax-parses these scripts and runs nothing else, so behaviour
must be checked by hand.

| Check | Method |
|---|---|
| Forks from `origin/main`, not HEAD | Run from a branch ahead of main; assert `git merge-base HEAD origin/main` equals `origin/main` |
| Refuses a dirty tree | Modify a tracked file; expect refusal and a non-zero exit |
| Rejects a bad name / existing branch | Two negative cases |
| Untracked files do not block | Create an untracked file; expect success |
| `Remove-MergedBranches` deletes only verified-merged branches | `-DryRun` first; then a throwaway merged branch and a throwaway unmerged one |
| Skips current and other-worktree branches | Assert this worktree's own branch survives |
| `Sync-Master` from a worktree | Run here; expect the delegated path and "already up to date" |
| Classifier | `Get-ChangeTier.ps1 -SelfTest` green |

**Known coverage gap.** The "refuse when the delegated target is dirty" path needs the main checkout
to be dirty, and an agent session confined to a worktree cannot arrange that. It is verified by
inspection only; the project owner can exercise it in one command from the main checkout, or accept
it untested. It is recorded here rather than quietly counted as covered.

## Invariants after this work

1. Every task branch, in either mode, is forked from a freshly fetched `origin/main` — never from
   `master`, never from the previous task's branch.
2. No script stashes or switches branches in a working tree other than the one it was invoked in.
   Exactly one cross-worktree mutation exists — advancing `master` by fast-forward, or by a merge
   commit when it has diverged — and it happens only in a tree first verified clean and free of an
   in-progress merge or rebase. (That mutation does update files in the other tree; it is the point
   of the sync, and it is safe precisely because the tree was verified clean first.)
3. A branch is deleted only after `merge-base --is-ancestor` proves it is contained in
   `origin/main`. Names are never evidence.
4. `Sync-Master.ps1` succeeds from anywhere in the repository, or explains precisely which working
   tree is blocking it.
5. New scripts under `Scripts/` remain Engine tier until deliberately classified, and every
   classification carries a self-test case.
