# ccache on the Windows CI build — Design

**Issue:** #377 (split out of #92, whose Linux half landed as PR #375)

## Goal

`build-and-test (Release)` is the critical path of a Build- or Engine-tier run now that #375 took the
four Linux configurations to 41–59 s on a warm cache. This puts ccache in front of clang-cl on that
one leg.

`.claude/plans/ccache-linux-ci.md` is the parent document. D1 (pinned upstream release, not a package
manager), D2 (`CMAKE_CXX_COMPILER_LAUNCHER`, not a compiler wrapper), D3 (one cache entry per
configuration), D6 (a failed install degrades the job rather than failing it) and D7 (per-job
statistics with `--zero-stats`) carry over unchanged and are not restated here. Only what differs on
Windows is below.

## Scope

**This change will:**

- Put `ccache` in front of `clang-cl` on the `build-and-test` job's **Release** leg only (D9).
- Teach `.github/actions/setup-ccache` to install the Windows release asset, keeping one action and
  therefore one place where the version and its hashes move together (D8).
- Leave `build.ps1` untouched, using CMake's environment-variable initialisation of the launcher
  (D10).

**This change will not:**

- Touch the Debug leg, or any debug-information format. See D9, and the ceiling it implies.
- Touch `CMakeLists.txt`, `CMakePresets.json` or anything a local build sees. A developer's build is
  bit-for-bit the same command lines as before.
- Re-open #92's Linux decisions.

## Decisions

### D8: one composite action, two install steps

`.github/actions/setup-ccache` gains a `runner.os == 'Windows'` install step beside the existing
Linux one, rather than a second action being added next to it.

The existing action's own comment gives the reason: the version and its SHA-256 must move together
across every configuration, because two copies make a partial bump possible that silently leaves
configurations on different ccache versions. That argument gets *stronger* with a second platform,
not weaker — there are now two assets per version, and they are cut from the same release.

The Windows step is PowerShell rather than bash. Windows runners do have Git Bash, but the tools the
Linux step leans on are not all reliably there (`sha256sum`, an unzipper), whereas
`Invoke-WebRequest`, `Get-FileHash` and `Expand-Archive` are built into the shell the job already
uses. Betting a required check on which utilities Git for Windows happens to ship is the wrong bet to
take for stylistic symmetry.

Two steps cannot share one `id`, so the action's `available` output becomes
`steps.install.outputs.available || steps.install-windows.outputs.available` — exactly one of the two
ever runs, and the other contributes an empty string.

**The Linux install step is not rewritten.** Folding both platforms into one `pwsh` step would read
better and would put a required check that currently works at the mercy of a refactor whose only
benefit is symmetry. Rejected on those grounds.

### D9: Release only, and the ceiling that follows

The `build-and-test` job is a two-leg matrix and both legs must finish, so the job's duration is the
slower leg. Build-step durations across 27 successful merge runs on `main`:

| Leg | n | Median | Range |
|---|---|---|---|
| `build-and-test (Release)` | 27 | 183 s | 154–253 s |
| `build-and-test (Debug)` | 27 | 156 s | 114–204 s |

**Caching Release alone therefore moves the Windows job down to the Debug leg, not to a warm-cache
build time.** The prize is the gap between the legs, and no more.

This is recorded because #377's opening post says otherwise: it costs the work against a ceiling of
"316 s → 175 s", which assumes Windows leaves the critical path entirely. It does not, and that
number should not be quoted again.

**Measured after landing, and the gap turned out to be gone.** The table above is pre-#378 history.
#378 (Catch2 unity units) merged first and took ~90 s off the Release leg's *cold* build, which is
the leg this caches — so it removed most of the very gap this change exists to claim. Two attempts on
one commit, this PR:

| Attempt | Release Build | Debug Build | Job = slower leg |
|---|---|---|---|
| Cold ccache (0 % hits) | 148 s | 161 s | 161 s |
| Warm ccache (93.5 % hits, 100 direct) | **58 s** | 156 s | **156 s** |

ccache does what it claims — the Release build falls from 148 s to 58 s — and the job moves by about
5 s, because Debug now bounds it. **Release-only caching is not expected to clear #92's ≥20 s
criterion on its own, and it is being landed anyway**: it is correct, green, free at runtime, and it
retires the open question of whether clang-cl plus ccache works at all, which is the only genuinely
uncertain part of caching the Debug leg. Windows leaves the critical path when *both* legs are
cached, and not before — which is **#380**.

Release only is still the right first step, for the reason #377 gives: caching Debug requires
`/Z7`, ccache cannot cache `/Zi`, and that is a change to how the shipping toolchain builds rather
than an added cache. It is also more work than it looks — `cmake_minimum_required(VERSION 3.24)`
leaves CMP0141 unset, so `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` is inert and `/Zi` arrives through
`CMAKE_CXX_FLAGS_DEBUG`; enabling `/Z7` means raising the minimum to 3.25 and setting the policy, or
editing the flags variable by hand. Neither belongs in a change whose premise is "add a cache".

Release is also the leg that needs no debug-information decision at all: **there is no debug-info
flag on the Release command lines.** Verified by inspection of `compile_commands.json` — zero of 204
entries carry `/Z7`, `/Zi` or `/ZI`, against 105 of 105 in Debug. #377's `/Z7` problem is entirely a
Debug problem.

### D10: the launcher arrives as an environment variable, so `build.ps1` is untouched

The Linux jobs call `cmake` directly and append `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`. The Windows
job does not call `cmake`; it calls `build.ps1`, which drives a preset and accepts no pass-through
for extra cache variables.

CMake initialises `CMAKE_<LANG>_COMPILER_LAUNCHER` from an environment variable of the same name at
first configure. Setting `CMAKE_CXX_COMPILER_LAUNCHER: ccache` in the Build step's `env` therefore
reaches the configure without `build.ps1` learning anything about ccache.

**Rejected:** adding a `-CompilerLauncher` parameter to `build.ps1`. It is the wrapper every
developer, the pre-commit hook and every measurement script goes through, and this would grow its
surface for a need that exists only inside one CI job. The environment variable is the supported
CMake mechanism, not a trick.

*Verified locally:* configuring with only the environment variable set produces
`CMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache` in `CMakeCache.txt` and `LAUNCHER = ccache` in
`build.ninja`.

D6's trap applies unchanged and is why the variable is set conditionally: CMake does not check that a
launcher exists, so configuring with it against a missing ccache succeeds and then fails at the first
Ninja edge with `ccache: not found`. The fallback path must drop the variable, not merely skip the
install.

### D11: the byte-identical binary gate does not transfer, and the object-level gate is stronger

#375's gate was a byte-identical binary cold versus warm. **That gate cannot be run on Windows: the
clang-cl build is not reproducible on `main` today, with ccache nowhere in the picture.**

Measured locally — two clean Release builds of the same commit, no ccache, differ in **59 of 73**
object files. The cause is not subtle: compiling one translation unit twice, two seconds apart,
produces objects differing in exactly one byte, at offset 4 — the COFF header's `TimeDateStamp`. The
same pair compiled with `/Brepro` is byte-identical.

So the gate becomes: **cold versus warm with ccache, compared at the object level.** ccache replays a
stored object byte-for-byte, which makes the cached path *more* reproducible than the uncached one.
Measured locally on this repository:

| Comparison | Objects differing |
|---|---|
| Two plain builds, no ccache | 59 of 73 |
| ccache cold vs ccache warm | 1 of 73 — CMake's own compiler-probe artifact, not a project TU |

Every project translation unit came back identical across the cold/warm pair, at a 100 % direct hit
rate over 98 cacheable calls.

**Rejected:** adding `/Brepro` to make the byte-identical binary gate available. It would work, and a
reproducible Windows build may well be worth having on its own merits — but it changes the objects
the shipping binary is linked from, inside a change whose premise is that it cannot affect the
binary. Filed as **#381**.

### D12: the kill criterion is inherited, and re-derived

#92's criterion — revert if the median run wall clock does not drop by at least 20 s, or the
FetchContent entry starts missing — **does not apply to this change on its own, and saying so is the
point of this section.** D9's post-landing measurement puts the job-level saving at about 5 s,
because Debug bounds the job. Read literally, the criterion says revert.

It is landed regardless, deliberately, and the justification has to be something other than wall
clock: this retires the last genuinely uncertain part of caching Windows (does clang-cl plus ccache
work — yes, 93.5 % hits on the runner image), at no runtime cost and with a proven degradation path.
The wall-clock criterion transfers to the Debug follow-up, where it is the whole question, and is
read against **both** legs cached rather than one.

What would make this change wrong rather than merely unspectacular is the eviction arm, and that arm
does still apply: this adds a fifth ccache entry against the same repository-wide 10 GB budget, and
the Windows entry is the largest of them. If the one-week `gh cache list` check shows the FetchContent
entry missing, revert this — a change buying 5 s must not cost a dependency clone.

The eviction arm matters more here than it did on Linux. This adds a fifth ccache entry against the
same repository-wide 10 GB budget, and the Windows entry is the largest of them, so `gh cache list`
at the one-week sitting is checking a tighter budget than #92's was.

## Assumptions I cannot verify from the code

- **The `windows-x86_64` ccache asset runs on the `windows-2025-vs2026` image.** The binary was
  exercised locally on Windows 11 with the same VS 18 / LLVM 22 toolchain, which is close but not the
  runner image. *Verified by:* the first CI run — the action's `--version` probe reports
  `available=false` and warns rather than failing if it does not run.
- **ccache's hashing covers everything that distinguishes this configuration.** Taken on trust from
  ccache's documented behaviour, as on Linux, and backed by the per-configuration key. *Verified by:*
  the cold-vs-warm object comparison in Validation, which would surface a wrong-object hit.
- **A warm Windows entry stays warm across ordinary merges.** The Catch2 translation units are pinned
  by `GIT_TAG` and cannot be invalidated by anything committed here, but our own sources are a
  different matter and the average hit rate over real merges is what D12 is read against. *Verified
  by:* D7's per-run statistics, which label every post-landing sample.

## Invariants

- The Windows job produces a working binary with a warm cache as with a cold one, and every project
  object is byte-identical across the pair (D11).
- A cache miss is a slowdown, never a failure. The same holds for a missing ccache binary, which is
  what D6's fallback exists for and what the degradation test exercises.
- The Debug leg's command lines are untouched: `/Zi`, `/RTC1` and `_ITERATOR_DEBUG_LEVEL` are exactly
  as they were.
- No local build changes. `build.ps1`, the presets and `CMakeLists.txt` are not in the diff.
- `build-and-test-result` semantics are untouched — no job added, none removed, no condition changed.

## Validation

**Build tier** — the diff is `.github/` plus documentation. No Elo match and no `Run-Bench`: ccache
changes how the binary is produced, not what it is, which is what D11's comparison asserts.

- **The cold-vs-warm object comparison (D11)**, run on the landing PR: two runs on the same commit,
  the first populating the cache and the second hitting it, with a temporary step emitting object
  hashes. The step comes out before merge. The local run of exactly this comparison is in the PR body
  as the prior expectation.
- **The degradation path (D6) gets its own test**, as it did on Linux: point the Windows download URL
  at a nonexistent asset on a throwaway commit and confirm the leg still goes green, emits the
  warning annotation, and builds at roughly its pre-ccache duration. A fallback path that has never
  run is an assumption.
- **The number, read from ordinary merges**, at the same sitting as #92's one-week `gh cache list`
  check. `build-and-test (Release)`'s **Build step**, not the job total. At least 8 warm-cache samples
  each side, every post-landing sample labelled with its D7 hit rate so a cold run is identifiable
  rather than an unexplained outlier.
- **Windows job totals vary by ~66 s run-to-run** (#377), which is larger than the effect being
  measured. The Build step is tighter than the job total but this still needs the sample count above;
  a single before/after pair resolves nothing here. Re-run merge commits one at a time if the
  observational data comes out ambiguous — **never batch `gh run rerun`**, since the runs share a
  concurrency group and each queued re-run cancels the previous one while reporting success (#372's
  method note).

**The comparison is against a moving baseline.** PR #378 (Catch2 unity units) lands first and takes
~48 s off the same leg's cold build. Both "before" and "after" samples must come from after #378
merges, or this change gets credited with #378's saving.

## Relationship to #378

#378 makes the cold build cheap; this makes the warm build cheap. They compose and neither
substitutes for the other — that was the reasoning for keeping them separate, and it survives: after
#378 the Catch2 slab is 40 CPU-seconds rather than 232, so ccache has less to win back on this leg
than it would have had, while the engine's own translation units — which #378 does not touch and
which are 54 % of the build (#83) — are exactly what a warm cache serves.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why one action with two install steps, and why the Windows half is PowerShell (D8) | comment in `.github/actions/setup-ccache/action.yml`, beside the Windows install step |
| Release only, and that the ceiling is the Debug leg rather than a warm build (D9) | comment in `build-and-test.yml` beside the ccache step; `Docs/CI.md` |
| Release carries no debug-info flag at all, so `/Z7` is a Debug-only problem (D9) | comment in `build-and-test.yml`; a note on #377 |
| The launcher arrives as an environment variable so `build.ps1` stays out of it (D10) | comment in `build-and-test.yml`, beside the Build step's `env` |
| The Windows build is not byte-reproducible (COFF `TimeDateStamp`), so the gate is object-level (D11) | `Docs/CI.md`; #381 for `/Brepro` |
| Release-only caching moves the job ~5 s because Debug bounds it, and why it landed anyway (D9, D12) | `Docs/Changelog.md`; #380 |
| Windows asset name and its pinned SHA-256; the bump policy covers both assets together (D8) | `.github/actions/setup-ccache/action.yml`; `Docs/CI.md` |
| Measured before/after wall clock, and the cold-vs-warm object comparison | `Docs/Changelog.md` and the PR body |

Delete this file once the row destinations above exist, the degradation test has been run, and the
one-week `gh cache list` reading has been recorded on #377. D9's measurement and D11's method are the
two things here that must outlive it — D9 because #380 is costed against it, D11 because it is the
only record of why the Linux gate does not transfer.

## Execution notes

Not durable; delete with this section when the work lands.

- The workflow and action diff, the `Docs/CI.md` and `Docs/Changelog.md` edits: one PR. Splitting
  them would put a Build-tier and a Docs-tier change through two validation cycles for well under a
  hundred lines.
- `/.github/*` is blanket-ignored behind an allowlist that permits `/.github/actions/*/action.yml`.
  Editing the existing action is safe; any *other* file added under the action's directory would be
  silently skipped by `git add`.
