# Dependency source cache — Design

**Issue:** #860

## Goal

Every local build tree points `FETCHCONTENT_BASE_DIR` at one shared directory, so worktrees and
presets share not just dependency sources but Catch2's compiled output (`catch2-build`) and the
population sub-builds. That is a latent race and a source of mismatched `Catch2.lib` for the test
binary. It is also the only reason the cache survives CMP0168 OLD: under NEW, population stamps live
in each build tree, so every fresh tree re-clones (#857). OLD will be removed, so the cache has to
share **sources only**. Build output stays private to each tree.

## Review focus

- **D1, the 3.26–3.30 policy flip.** Raising the minimum to 3.30 switches 28 policies to NEW
  (CMP0143–CMP0170). CMP0155 is handled (D2). CMP0156 (link-line de-duplication) and CMP0166 could
  change generated link lines unseen. The `build.ninja` diff in Validation is the only check; is it
  enough?
- **D3, CMake owning the clone.** It runs `git clone` at configure time under `file(LOCK)`. Is it
  correct under concurrent configures, an interrupted clone, and a pin bump?
- **D5, compatibility with older refs.** Scripts that build pre-change commits pass both the old and
  the new variable. Check that no current build can end up sharing build output.

## Scope

**This change will:**

- Raise `cmake_minimum_required` and `CMakePresets.json` `cmakeMinimumRequired` to 3.30, and turn
  off C++ module scanning.
- Add `cmake/DependencySources.cmake`: one tag-keyed shared checkout per dependency, which feeds
  `FETCHCONTENT_SOURCE_DIR_<NAME>`.
- Make each build tree's `_deps` private.
- Switch `build.ps1`, `Measure-CpuProfile.ps1`, `WslBuild.ps1`, the dependency-release checker and
  every workflow cache to the new layout.
- Update `Docs/Workflow.md` (Dependency cache, Support folders), `Docs/Dependencies.md` (Bumping a
  version) and `Docs/CI.md`, and add a changelog entry.

**This change will not:**

- change pins, `FIND_PACKAGE_ARGS` (still deliberately absent), Catch2's unity batching or the
  ccache setup (#509/#510/#511);
- adopt C++ modules;
- delete `StratChessSupport\Deps`. Pre-change commits' `build.ps1` still uses it, through
  `Compare-SearchEquivalence.ps1 -BaselineRef` (D5);
- prune stale `<name>-<tag>` checkouts automatically.

## Decisions

### D1: Minimum CMake 3.30

3.30 is the lowest version where CMP0168 defaults to NEW, so no `if(POLICY)` guard and no OLD
code path remain. It also allows `EXCLUDE_FROM_ALL` in `FetchContent_Declare` (3.28), which replaces
the `set_property(TARGET spdlog PROPERTY EXCLUDE_FROM_ALL TRUE)` workaround.

Lowest CMake per environment:

| Environment | CMake |
|---|---|
| local VS 18 | 4.3.1 |
| CI `ubuntu-26.04` | 4.4.4 |
| CI `windows-2025-vs2026` | 4.4.4 on PATH |
| WSL `Ubuntu-26.04`, the distro `WslBuild.ps1` uses | 4.2 |

Rejected:

- **Guarded `cmake_policy(SET CMP0168 NEW)` at 3.25.** It keeps a dual path for a version nothing
  here runs.
- **A 4.x minimum.** Nothing concrete is gained, and 4.1 stays available as a ceiling.
- **`3.25...3.30` range syntax.** It flips the same policies, so it buys nothing over a plain 3.30.

The owner approved raising the minimum for this issue (2026-10-20). That narrows #382's "keep 3.25"
for this case only.

Cost: WSL `Ubuntu-24.04` (CMake 3.28) can no longer configure. No script or CI job uses it. The
owner plans to remove that distro (2026-10-20). Its only unique tools are valgrind, gdb and clang-22,
and all three can be reinstalled on 26.04.

### D2: `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)` before `FetchContent`

CMP0155 makes Ninja scan every C++20-and-later source for `import`. `CMAKE_CXX_STANDARD 23` is set
globally (`CMakeLists.txt:38`) and Catch2 inherits it, so every translation unit, engine and Catch2
alike, would gain a scan step. ccache does not cache that step, and its `dyndep` edges change the
graph that `cmake/CheckNinjaDepsRules.cmake` inspects. The project uses no modules.

Setting the variable globally, rather than per target, also covers dependency targets.

Rejected: leaving scanning on and measuring the cost. That pays for a feature with no user.

### D3: CMake owns tag-keyed shared checkouts

`strat_declare_dependency(<name> GIT_REPOSITORY <url> GIT_TAG <tag> [FetchContent args…])` lives in
`cmake/DependencySources.cmake`. When the cache variable `STRAT_DEPS_SOURCE_CACHE` is non-empty, it
does the following:

1. Use `<cache>/_deps/<name>-<tag>`. The `_deps` segment is deliberate. Every vendored-code filter in
   the tree keys on it:
   - `ExcludeHeaderFilterRegex` in `.clang-tidy` and `.clang-tidy-deep`;
   - `Get-Coverage.ps1`'s `$IgnoreFilenameRegex`.

   Placing the segment in the helper keeps them correct whatever directory the caller passes.
2. If that directory is missing, take `file(LOCK <cache>/_deps/<name>-<tag>.lock GUARD FUNCTION
   TIMEOUT 600)` and re-check. Then run `git clone --depth 1 --branch <tag>` into
   `<cache>/_deps/<name>-<tag>.tmp-<random>`, and `file(RENAME)` it into place. A clone that fails
   is a `FATAL_ERROR` that prints git's output. The temp directory is removed on both paths.
3. Set `FETCHCONTENT_SOURCE_DIR_<NAME_UPPER>` to that directory.

Then, in every case, call `FetchContent_Declare(<name> <all args>)`.

With the variable empty, the helper only declares, and the dependency is fetched into the build
tree's own `_deps`. That covers a bare `cmake` run, VS folder mode without `build.ps1`, and any
caller that doesn't opt in.

Why tag-keyed: a pin bump selects a new directory, so no checkout is ever updated in place, and the
in-place update that `Dependencies.md` step 2 relies on goes away. Because a directory exists only
after a successful rename, its existence means the clone completed.

Rejected:

- **`build.ps1` owns the clone.** Bare CMake, `Measure-CpuProfile.ps1`, `WslBuild.ps1` and CI would
  each need their own copy.
- **A `cmake -P` script.** Every caller would have to run it first.
- **FetchContent populating a shared `SOURCE_DIR`.** Under NEW it re-clones over sources that other
  trees are building against (#857).
- **A dependency provider (3.24).** More machinery for the same three `FETCHCONTENT_SOURCE_DIR_*`
  assignments.

### D4: Pins stay single-sourced through the helper's arguments

The three declarations become `strat_declare_dependency(...)` calls with the same keyword layout.
`.github/scripts/check_dependency_releases.py:25` matches `strat_declare_dependency\(` instead of
`FetchContent_Declare\(`, and `test_dependency_releases.py` follows.

Rejected: literal `FetchContent_Declare` blocks plus a separate helper call. FetchContent has no
public API to read back a declared `GIT_TAG`, so the tag would be written twice.

### D5: Build output is private to the tree, enforced in `CMakeLists.txt`

`set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")` is a normal variable set before
`include(FetchContent)`. It shadows any `-D FETCHCONTENT_BASE_DIR` (verified by a probe, below), and
`CMakePresets.json` drops its entry.

That lets the scripts that build **arbitrary refs** (`Measure-CpuProfile.ps1 -Before`, and
`WslBuild.ps1` through `Compare-BenchLinux.ps1 -Baseline`) pass both variables:

- the old `FETCHCONTENT_BASE_DIR=<old shared dir>`, so a pre-change commit still reuses its cache
  offline;
- the new `STRAT_DEPS_SOURCE_CACHE`, which a post-change commit uses, while ignoring the old one.

`build.ps1` builds only its own commit, so it passes only the new variable.
`Compare-SearchEquivalence.ps1 -BaselineRef` runs the baseline commit's own `build.ps1`, so it needs
no change.

### D6: Locations

| Where | `STRAT_DEPS_SOURCE_CACHE` |
|---|---|
| `build.ps1`, `Measure-CpuProfile.ps1` | `<repos>/StratChessSupport/DepSources` (new folder; `Deps\` stays for pre-change refs) |
| `WslBuild.ps1` | `$HOME/strat-wsl-dep-sources` |
| CI, every job | `${{ runner.temp }}/DepSources`, cached by `actions/cache` |

In CI, every `cmake-deps-*` / `win-deps-*` key is renamed to `dep-sources-…`: 5 in
`build-and-test.yml`, 8 in `nightly.yml` and 1 in `strength.yml`. The new name makes the old caches
unreachable rather than corrupting them. Keeping `runner.temp` outside the checkout keeps it out of
`git status`, and outside `build/`.

**Existing local trees.** `build.ps1` passes `-D` only on a fresh configure. A tree configured
before this change is re-configured by Ninja itself, because `CMakeLists.txt` changed. That re-run
has no `STRAT_DEPS_SOURCE_CACHE`, so the tree would fetch into its own `_deps` over the network and
never use the shared checkouts.

To prevent that, `build.ps1` checks an existing tree's `CMakeCache.txt` before building. When the
variable is missing, it re-runs the configure in place with `-D STRAT_DEPS_SOURCE_CACHE=…` and prints
a one-line notice. The value is then cached for good. This mirrors the launcher reconciliation
already beside it (`Get-CompilerLauncherAction`).

## Assumptions I cannot verify from the code

- **`FETCHCONTENT_SOURCE_DIR_<NAME>` skips population under NEW and keeps `<name>-build` private.**
  Verified by a probe: CMake 4.3.1, a 3.30 minimum, an unreachable `GIT_REPOSITORY`, two fresh trees.
  Both configured with no network access, each with its own `_deps/fake-build` and no `-subbuild`.
  It was not run against the real three dependencies; Validation covers that.
- **A normal `FETCHCONTENT_BASE_DIR` shadows `-D`.** Verified by a probe: passed
  `-D FETCHCONTENT_BASE_DIR=<shared>` and set the normal variable. The output landed in
  `<tree>/_deps`, and `<shared>` was never created.
- **`file(LOCK)` and `file(RENAME)` behave on Windows and Linux** while another process holds the
  lock, and when the destination exists, `RENAME` fails rather than merging. Taken from CMake's
  documentation; not run. The concurrent-configure step in Validation settles it.
- **The CI images' CMake.** Read from the `actions/runner-images` READMEs on 2026-10-20, not from a
  job log. The first CI run settles it, because a lower version fails at `cmake_minimum_required`.
- **`git` is on PATH wherever configure runs.** FetchContent already requires it, so nothing changes.

## Invariants

- No build tree's `build.ninja` references a `*-build` or `*-subbuild` path outside that tree.
- A configure that has `STRAT_DEPS_SOURCE_CACHE` set and finds the checkout present makes no
  network access.
- A shared checkout is never modified after its rename.
- The engine binary is unchanged.
- No `build.ninja` contains module-scan rules or `dyndep`.
- No new configure warnings on Windows clang-cl or CI Linux.

## Validation

Build-tier change: full CI runs on the PR. No Elo match and no bench, because no engine flag
changes. Any change D1 makes to compile or link lines would surface in the `build.ninja` diff and in
`Compare-SearchEquivalence.ps1`.

1. **Policy flip.** Configure `windows-clang-cl` on `main` and on the branch, and diff the
   `StratChessEvolved` and `StratChessTests` compile and link commands in `build.ninja`. Expected
   differences are paths only (the dependency include directories); anything else gets explained in
   the PR. The configure log shows zero warnings. CI Linux's configure step likewise.
2. **No scanning.** `grep -c dyndep build.ninja` returns 0 (module builds always use dyndep) for all three Windows presets and
   one CI Linux job, read from the log via a temporary `grep` step or the uploaded tree.
3. **Private output.** Release and coverage in one worktree, plus a second worktree: no
   `catch2-build` path outside each tree appears in its `build.ninja`.
4. **Offline and pin bump.** With the checkouts present, a fresh worktree configures with no
   `Cloning into` in the log. A scratch `GIT_TAG` change to the previous Catch2 tag clones
   `catch2-v<old>` and leaves `catch2-v3.16.1` byte-identical (`git -C … status --porcelain` empty,
   same HEAD).
5. **Concurrency.** Two fresh configures start together against an empty `DepSources`. Both
   succeed, one clone per dependency, and no `.tmp-*` remains.
6. **Older ref.** `Measure-CpuProfile.ps1 -Before 683f30f` and `Compare-BenchLinux.ps1 -Baseline
   683f30f` both configure the old ref without cloning. A pre-existing worktree's first `build.ps1`
   run after the pull prints the reconfigure notice and does not clone.
7. **Equivalence.** `Compare-SearchEquivalence.ps1 -After <branch exe>` passes.
8. **Build time**, Windows clang-cl Release, median and range of ≥3 runs each, before and after:
   - fresh-tree configure;
   - cold full `build.ps1` with `CCACHE_DISABLE=1`;
   - warm no-op rebuild.

   Expected: configure the same or faster (no sub-builds); the other two within spread. A regression
   beyond spread blocks the PR until explained. CI: compare the Build step (not the job total)
   against recent `main` runs. The first run after the key rename is a cold ccache run on every leg,
   because the dependency include paths change, so compare the second run.
9. **Checker.** `test_dependency_releases.py` passes, and the checker still reads all three pins.

## Cost

- **Size:** 50–200 lines over about 14 files:
  - `CMakeLists.txt`, the new `cmake/DependencySources.cmake`, `CMakePresets.json`;
  - `build.ps1`, `Measure-CpuProfile.ps1`, `WslBuild.ps1`;
  - three workflows, the checker and its test;
  - `Docs/Workflow.md`, `Docs/Dependencies.md`, `Docs/CI.md`, the changelog.
- **Blast radius:** Build tier. Every build path's configure step changes. The CI caches are reset
  once, and one cold ccache run follows.
- **Review:** one code review. Validation steps 1–8 take about 1 h locally, mostly step 8.
- **Optional, priced alone:**
  - **The `file(LOCK)`**, ~6 lines. It covers two configures racing on an empty cache or a pin
    bump. Without it, a loser's `RENAME` fails and that configure errors once. Recommended: keep.
  - **`build.ps1`'s in-place reconfigure of old trees**, ~10 lines. Without it, every pre-existing
    tree fetches into its own `_deps` over the network and never uses the shared checkouts. That is
    correct but defeats the cache until the tree is deleted. Droppable if a "delete your build
    directories" note is acceptable instead.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| why 3.30, scanning off (D1, D2) | comment above `cmake_minimum_required` and the `CMAKE_CXX_SCAN_FOR_MODULES` line |
| tag-keyed checkouts, the `_deps` segment, the lock (D3) | header comment of `cmake/DependencySources.cmake` |
| base directory forced private; both variables passed for older refs (D5) | comment at the `FETCHCONTENT_BASE_DIR` line; comment in `Measure-CpuProfile.ps1` / `WslBuild.ps1` |
| layout and locations (D6); `Deps\` kept for older refs | `Docs/Workflow.md` → Dependency cache, Support folders |
| a pin bump clones a new directory; CI key rename | `Docs/Dependencies.md` → Bumping a version |
| build-time before/after | `Docs/Changelog.md` entry |
