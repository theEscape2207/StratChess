# CI trigger correctness and `windows-ci` label scope (issues #185, #187)

Two corrections to how CI decides what to run. Both touch
`.github/workflows/build-and-test.yml` and the same two documents, and neither is meaningful without
the other being at least considered, so they land together. Split them if review prefers.

## Goal and scope limits

- **#185** — make the push trigger tier-aware, so a docs-only merge to `main` stops running the full
  Linux validation.
- **#187** — narrow the documented `windows-ci` rule to diffs that can actually change what the
  Windows build produces.

**Not in scope**: adding a Debug leg to `Validate-PrePR.ps1` (option 2 in #187 — it trades CI minutes
for local wall time on every PR and deserves its own decision); removing the `windows-ci` label
entirely (option 3); anything about `sanitize-linux`'s content, which #186 settled.

## #185 — the push trigger

`classify` runs `Get-ChangeTier.ps1 -BaseRef origin/main`. On a push to `main`, `origin/main` **is**
`HEAD`, so the diff is empty, and an empty diff returns the weakest tier by design. Observed on run
30816658035, a merge that touched `.github/**`:

```
Change tier: Docs (decided by: )
Changed files:            <- empty
```

`is_full` is therefore always `false` on a push, which is why every job carries
`github.event_name == 'push' ||`. Remove that clause without fixing the classifier and the jobs stop
running on `main` altogether.

### Design decision: `github.event.before`, not `HEAD^`

#185's body proposes `-BaseRef HEAD^`. `github.event.before` is better and is what this implements:
it is the tip `main` had before the push, so it stays correct when a push carries more than one
commit, whereas `HEAD^` only ever covers the last. For this repo's actual shape — one merge commit
per push, every PR landing as a merge rather than a squash (checked against the last 12 commits on
`main`) — the two are identical, so this costs nothing and is correct in the case that would
otherwise be silently wrong.

Selected inline, so the PR path is untouched:

```yaml
BASE_REF: ${{ github.event_name == 'push' && github.event.before || 'origin/main' }}
```

**Failure behaviour is already right and must stay that way.** If the ref is unreachable — a force
push, or the all-zeros SHA on branch creation — `git diff` fails and `Get-ChangeTier.ps1` warns and
returns Engine tier. Fail closed. Do not add a fallback that guesses a cheaper tier.

Then drop `github.event_name == 'push' ||` from `build-linux` and `sanitize-linux`. The Windows job's
condition reads `github.event.pull_request.labels`, which is already null on a push, so it is
untouched.

### The deps cache, which is the one thing that could go wrong

The push run also warms `actions/cache` on the default branch, and caches are branch-scoped: a PR can
only restore a cache saved on `main`. After this change `main` only builds on Build/Engine merges.

Safe, for two reasons that should both hold before this lands:
1. The key is static (`cmake-deps-spdlog-v1.16.0-json-v3.12.0-catch2-v3.13.0`) and only changes on a
   dependency bump — which is a `CMakeLists.txt` edit, i.e. Build tier, i.e. still runs.
2. Eviction is 7 days without **access**, and a restore counts as access. PR runs restore it on every
   Build/Engine PR, so it stays alive.

## #187 — the `windows-ci` rule

Current rule in CLAUDE.md and `Docs/Workflow.md`: apply the label to *"any PR touching
`StratEngine/**`, the CMake files or the workflow itself"* — essentially all engine work, on the 2x
job, by policy.

What the job uniquely adds is far smaller, and shrank again when #186 landed:

| Claimed | Actually covered by |
|---|---|
| Clean checkout | `build-linux` — same `CONFIGURE_DEPENDS` glob from a fresh clone |
| clang-cl / lld-link / ThinLTO / MSVC STL | `Validate-PrePR.ps1`, locally, before every PR |
| OOB and UB in engine data structures | `sanitize-linux` (#186), more thoroughly |

Residual unique coverage: **Windows-specific Debug** — `Validate-PrePR.ps1` never passes `-Config`,
so it takes `build.ps1`'s Release default, while the Windows job runs a Release+Debug matrix.

New rule: label when the diff can change **what the Windows build produces** — `CMakePresets.json`,
`build.ps1`, `StratEngine/Compat.h`, the clang-cl branch of `strat_configure_target`, or any
`_MSC_VER`/`_WIN32` conditional. Ordinary engine changes do not qualify.

**State the dependency explicitly**, because it is what makes the narrowing safe: the rule assumes
`Validate-PrePR.ps1` actually ran. A PR pushed around `New-PullRequest.ps1` (the PR #148 failure mode
CLAUDE.md already warns about) has no local Windows build behind it and should be labelled regardless.

Also worth a line: the label did not exist in the repository until 2026-08-04, so between #182 and
then the gate referenced something nobody could apply. Exposure was nil — the only merge in the
window was #183, which would not have qualified — but a gate whose label does not exist should be
recorded rather than quietly fixed.

## Files

- `.github/workflows/build-and-test.yml` — `BASE_REF` on `classify`; drop the push clause from two
  job conditions; refresh the comments that explain both.
- `CLAUDE.md` — the CI paragraph.
- `Docs/Workflow.md` — the CI section.
- `Docs/Changelog.md` — one entry.

`Get-ChangeTier.ps1` itself needs **no change**: the bug is in which ref the workflow hands it, not in
how it classifies. Its `-SelfTest` table operates on file lists and cannot express a ref-resolution
case, so nothing is added there; the fix is verified against the real run instead.

## Validation

- `Get-ChangeTier.ps1 -SelfTest` — unchanged behaviour expected, but the diff touches the machinery
  it guards.
- `Validate-PrePR.ps1` via `New-PullRequest.ps1`. Build tier, so the full gate set.
- **On the PR itself**: `classify` must still report a real tier (Build, from `.github/**`) and both
  Linux jobs must run — this PR is its own test of the PR path.
- **After merge**: the push run on `main` is the actual test of the changed path. It should classify
  Build (not Docs, and not an empty file list) and run both Linux jobs. Watch that run rather than
  assuming.

## Invariants

- A push to `main` classifies against what the push actually contained, never against itself.
- An unreachable base ref still fails closed to Engine tier.
- Docs and Tooling merges run no build jobs; Build and Engine merges run exactly what a PR would.
- Nothing changes for the `pull_request` path.
