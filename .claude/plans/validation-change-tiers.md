# Scope Validation to the Diff — Change Tiers

**Issue**: #124 · **Depth**: full plan · **Status**: in progress

## Goal

Stop over-validating changes that cannot break anything, in both places it currently happens:

1. **CI** — `.github/workflows/build-and-test.yml` runs the full ~6 min deps-fetch + MSBuild + test
   cycle on every PR, including pure documentation edits.
2. **`Scripts\Validate-PrePR.ps1`** — always runs full build + extended `[slow]` tests + a 10-run
   tactical stability suite + self-play, including for changes to measurement scripts that are never
   compiled and never invoked by the engine.

Both get their decision from **one shared classifier**, so the two definitions cannot drift.

**Scope limit**: classification and gating only. No change to what any gate *does* when it runs.

## Motivating examples (both real)

- **PR #123** — a one-line `CLAUDE.md` edit, full ~6 min CI cycle.
- **PR #133** — SPRT support: `Scripts\Run-EloMatch.ps1` + three docs. Per the checklist it ran a
  full build, extended `[slow]` tests, a 10-run tactical stability suite, and a self-play game.
  None of those gates can be affected by a measurement script. It passed, as it was always going to,
  several minutes later.

## Design decisions

**D1 — Four tiers, strictest-wins on mixed diffs.**

| Tier | Matches | Validation |
|---|---|---|
| `Docs` | `*.md` (anywhere), `Docs/**`, `.claude/plans/**` | Nothing beyond the pre-commit hook |
| `Tooling` | `Scripts/Run-EloMatch.ps1`, `Scripts/Run-Tests.ps1`, `Scripts/Sync-Master.ps1`, `Scripts/verify_mate_key.py` | PowerShell syntax parse of the changed scripts |
| `Build` | `build.ps1`, `Scripts/Validate-*.ps1`, `.githooks/**`, `.github/**`, `*.vcxproj*`, `*.props`, `Directory.Build.*` | Full |
| `Engine` | everything else — `*.cpp`, `*.h`, `*.json`, **and anything unrecognised** | Full |

**D2 — Fail closed.** The final rule is `default → Engine`. There is no "else → cheap" branch. An
unrecognised path must cost time, never skip validation. This is the single most important property:
a classifier that silently under-validates on an unfamiliar path is worse than no classifier.

**D3 — The validation scripts classify as `Build`, not `Tooling`.** `Validate-PrePR.ps1`,
`Validate-PreCommit.ps1` and `Get-ChangeTier.ps1` itself are the machinery that decides whether
validation runs. If a change to them could take its own shortcut, a classifier bug would be
*self-concealing*: it would disable validation and then not validate the change that disabled it.
They get the full run, always. (This PR is itself `Build` tier for exactly this reason.)

**D4 — Single source of truth: `Scripts\Get-ChangeTier.ps1`.** Both consumers call the same script.
The alternative — a `paths-filter` config in CI plus a duplicate pattern list in PowerShell — is two
lists that will disagree within a few months, and the disagreement would be invisible until it
mattered.

**D5 — CI fast path applies to `pull_request` only; `push: main` always runs the full job.** The
push-to-main run has a second purpose beyond validation: it warms the `actions/cache` deps cache on
the default branch, which `actions/cache` scopes per-branch (documented in `CLAUDE.md`). Skipping it
on docs-only pushes would slowly let that cache lapse and make the *next* code PR pay a cold-cache
build. Cheap insurance; deliberate asymmetry, documented in the workflow.

**D6 — CI needs full history to diff.** `actions/checkout@v4` defaults to a shallow single-commit
fetch, which makes `git diff origin/main...HEAD` impossible. The classifier job checks out with
`fetch-depth: 0`. This is why classification lives in its own small job rather than inline in
`build-and-test` — the expensive job keeps its shallow checkout.

**D7 — `-Force` escape hatch** on `Validate-PrePR.ps1` for when the author's judgement disagrees with
the classifier.

**D8 — Report the decision loudly.** Both consumers print the detected tier, the file that decided
it, and which gates were consequently skipped. Silent skipping is indistinguishable from a
validation gap when someone reads the output later.

**D9 — The classifier is self-testable.** `-Paths` accepts an explicit file list instead of shelling
out to git, and `-SelfTest` runs a table of assertions over known inputs. The repo has no PowerShell
test framework (Catch2 is C++-only), so a built-in self-test is the pragmatic gate — and it makes
the fail-closed property (D2) directly assertable rather than assumed.

**D10 — Classify the working tree, not just committed history.** Found by testing, and it was a real
hole: `git diff origin/main...HEAD` only sees *commits*, so running `Validate-PrePR.ps1` with
uncommitted work produced an empty diff → `Docs` → every gate skipped. That is precisely when
someone reaches for a validation script, and it silently under-validated. The classifier now unions
the committed diff with `git status --porcelain` (staged, unstaged and untracked; renames resolve to
the destination path). Validate what is on disk, not merely what has been recorded.

This branch is the regression case: before the fix it classified `Docs` with an empty file list;
after, `Build` (decided by `.github/workflows/build-and-test.yml`). The union path is exercised
behaviourally rather than in `-SelfTest`, since `-SelfTest` deliberately bypasses git via `-Paths`.

## Files changed

| File | Change |
|---|---|
| `Scripts/Get-ChangeTier.ps1` | **New** — the shared classifier + `-SelfTest` |
| `Scripts/Validate-PrePR.ps1` | Classify, gate the four steps, `-Force` / `-BaseRef`, report |
| `.github/workflows/build-and-test.yml` | New `classify` job; `build-and-test` gated on its output |
| `CLAUDE.md` | Pre-PR checklist step 2 → the tier table (it is the doc both consumers cite) |
| `Docs/Changelog.md` | Dated entry |

## Step-by-step

1. **`Get-ChangeTier.ps1`** — params `-BaseRef` (default `origin/main`), `-Paths`, `-SelfTest`.
   Returns an object with `Tier`, `DecidingFile`, `ChangedFiles`. Order the rules most-specific
   first (`Scripts/Validate-*.ps1` must be tested before a general `Scripts/*.ps1` rule, or the
   validators would fall into `Tooling` — the exact D3 hazard).
2. **Self-test table** covering: docs-only; tooling-only; a mixed docs+tooling diff (→ `Tooling`);
   docs + one `.cpp` (→ `Engine`); `build.ps1` (→ `Build`); `Validate-PrePR.ps1` (→ `Build`, **not**
   `Tooling`); an unrecognised path such as `foo/bar.xyz` (→ `Engine`, the fail-closed case);
   an empty diff (→ `Docs`, nothing to validate).
3. **`Validate-PrePR.ps1`** — classify up front; on `Docs` print and exit 0; on `Tooling` run the
   syntax parse and exit on its result; on `Build`/`Engine` run the existing four steps untouched.
   `-Force` bypasses to the full path. The summary table gains the tier line.
4. **CI** — `classify` job (`fetch-depth: 0`, runs the classifier, sets an output);
   `build-and-test` gets `needs: classify` and an `if:` allowing it through when the event is a push
   to `main` (D5) or the tier is `Build`/`Engine`.
5. **CLAUDE.md** — replace the binary doc-only/everything-else wording in step 2 with the tier table
   and a pointer to the script.

## Validation plan

- `Get-ChangeTier.ps1 -SelfTest` — all cases pass, including fail-closed and the `Validate-*` case.
- Syntax parse of both changed `.ps1` files via `Parser::ParseInput`.
- **Behavioural check against real diffs**: run the classifier with `-Paths` reproducing PR #123
  (→ `Docs`), PR #133 (→ `Tooling`), and this PR's own file list (→ `Build`).
- `Validate-PrePR.ps1 -Force` on this branch — must still run all four gates and pass, proving the
  gating did not break the full path. **This is the important one**: the risk of this change is
  silently disabling validation, not a wrong tier.
- CI: the workflow change is validated by the PR's own CI run — this diff touches `.github/**`, so
  it classifies as `Build` and must run the full job. If the PR's CI is skipped, the gate is wrong.

## Key correctness properties

1. **Fail closed** — an unrecognised path yields `Engine`. Asserted in `-SelfTest`.
2. **Strictest-wins** — a mixed diff never validates at the weaker tier. Asserted.
3. **Self-referential safety** — changes to `Validate-*.ps1` / `Get-ChangeTier.ps1` / `build.ps1`
   never take a shortcut. Asserted.
4. **One definition** — CI and the local script call the same file; no duplicated pattern list.
5. **Full path unchanged** — for `Build`/`Engine`, `Validate-PrePR.ps1` behaves exactly as before,
   including running all four checks before exiting so every failure is visible at once.
6. **Push-to-main still warms the cache** (D5).
7. **Decision is visible** — tier, deciding file, and skipped gates are printed by both consumers.
