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
  (CMP0143–CMP0170). CMP0155 is handled (D2). CMP0156 (link-line de-duplication, which depends on
  the linker) could change link lines unseen. CMP0166 is inert, because nothing here uses
  `$<TARGET_PROPERTY>`. Validation steps 1–2 close this: expanded commands on both OSes, and the
  engine's hash.
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

CMP0155 makes Ninja scan C++20-and-later sources for `import` wherever the compiler supports
scanning. `CMAKE_CXX_STANDARD 23` is set globally (`CMakeLists.txt:38`) and Catch2 inherits it, so on
a supporting toolchain every translation unit, engine and Catch2 alike, gains a scan step. ccache does not cache that step, and its `dyndep` edges change the
graph that `cmake/CheckNinjaDepsRules.cmake` inspects. The project uses no modules.

Setting the variable globally, rather than per target, also covers dependency targets.

Rejected: leaving scanning on and measuring the cost. That pays for a feature with no user.

### D3: CMake owns tag-keyed shared checkouts

`strat_declare_dependency(<name> GIT_REPOSITORY <url> GIT_TAG <tag> [FetchContent args…])` lives in
`cmake/DependencySources.cmake`. The cache variable `STRAT_DEPS_SOURCE_CACHE` takes its initial
value from the environment variable of the same name, passed through `file(TO_CMAKE_PATH)` because
CI on Windows hands over backslashes. After that the cached value wins. When it is non-empty, the
helper does the following:

1. Use `<cache>/_deps/<name>-<tag>`. The `_deps` segment is deliberate. Every vendored-code filter in
   the tree keys on it:
   - `ExcludeHeaderFilterRegex` in `.clang-tidy` and `.clang-tidy-deep`;
   - `Get-Coverage.ps1`'s `$IgnoreFilenameRegex`.

   Placing the segment in the helper keeps them correct whatever directory the caller passes.
2. If that directory is missing:
   1. Take `file(LOCK <cache>/_deps/<name>-<tag>.lock GUARD FUNCTION TIMEOUT 600)` and re-check.
   2. Remove any leftover `<cache>/_deps/<name>-<tag>.tmp`. Only the lock holder writes it, so a
      leftover can only come from a killed configure. That makes recovery from an interrupted
      clone automatic, with nothing to remove by hand.
   3. Run `git clone --depth 1 --branch <tag>` into that `.tmp`.
   4. Publish it with `file(RENAME <tmp> <final> NO_REPLACE RESULT r)`.

   A failed clone or a non-zero `r` removes the `.tmp`, then raises `FATAL_ERROR` with git's output
   or `r`. A non-zero `r` should be unreachable under the lock and the re-check.
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

`build.ps1` builds only its own commit, so it sets only the new variable (D6).
`Compare-SearchEquivalence.ps1 -BaselineRef` runs the baseline commit's own `build.ps1`, so it needs
no change.

The strength lab's reference build (`strength.yml:341-351`) builds into
`$RUNNER_TEMP/reference/build`, which no cache step restores. A pre-change reference therefore
clones over the network today, and it keeps doing so after this change. That costs a few seconds per
run. A post-change reference picks up the workflow-level variable (D6) and uses the cached sources.
No bridge is added for old refs. The CI cache-hit claim covers the candidate and every
non-strength job; the reference is a hit only when it is a post-change commit.

### D6: Locations

| Where | `STRAT_DEPS_SOURCE_CACHE` |
|---|---|
| `build.ps1`, `Measure-CpuProfile.ps1` | `<repos>/StratChessSupport/DepSources` (new folder; `Deps\` stays for pre-change refs) |
| `WslBuild.ps1` | `$HOME/strat-wsl-dep-sources` |
| CI, every job | `${{ github.workspace }}/build/dep-sources`, cached by `actions/cache` |

**Transport is the environment variable, everywhere except WSL.**

- **CI.** A workflow-level `env: STRAT_DEPS_SOURCE_CACHE: ${{ github.workspace }}/build/dep-sources`
  goes in each of the three workflows. Workflow-level `env` can read the `github` context, but not
  `runner`, hence `github.workspace` rather than `runner.temp`. `build/` is gitignored. Every raw
  `cmake -S` call and every `build.ps1` call (Windows PR legs, nightly lint and coverage) then
  inherits it, with no per-call `-D`. This matches how the Windows legs already hand ccache settings
  to `build.ps1` through the environment (`build-and-test.yml:494`).
- **`build.ps1`.** It drops its `GITHUB_ACTIONS` skip for this variable. When the variable is unset,
  it sets `$env:STRAT_DEPS_SOURCE_CACHE` to the local `DepSources` path, for itself and every child
  process. In CI it leaves the workflow's value alone.

  This also fixes **existing local trees**. Ninja re-runs CMake by itself after the pull, because
  `CMakeLists.txt` changed, and that child inherits the variable, so the old tree adopts the shared
  checkouts on its first build. No reconfigure logic is needed.
- **`Measure-CpuProfile.ps1`** sets the same environment variable before configuring, alongside the
  old `-D` (D5).
- **`WslBuild.ps1`** passes `-D` in `build.sh`, because the environment does not cross `wsl --exec`.

In CI, every `cmake-deps-*` / `win-deps-*` key is renamed to `dep-sources-…`: 5 in
`build-and-test.yml`, 8 in `nightly.yml` and 1 in `strength.yml`. The new name makes the old caches
unreachable rather than corrupting them.

## Assumptions I cannot verify from the code

- **`FETCHCONTENT_SOURCE_DIR_<NAME>` skips population under NEW and keeps `<name>-build` private.**
  Verified by a probe: CMake 4.3.1, a 3.30 minimum, an unreachable `GIT_REPOSITORY`, two fresh trees.
  Both configured with no network access, each with its own `_deps/fake-build` and no `-subbuild`.
  It was not run against the real three dependencies; Validation covers that.
- **A normal `FETCHCONTENT_BASE_DIR` shadows `-D`.** Verified by a probe: passed
  `-D FETCHCONTENT_BASE_DIR=<shared>` and set the normal variable. The output landed in
  `<tree>/_deps`, and `<shared>` was never created.
- **`file(LOCK)` blocks a second configure, and the OS releases the lock when the holding process is
  killed.** `file(RENAME … NO_REPLACE)` fails if the destination exists; without `NO_REPLACE` it
  would replace it. Both are taken from CMake's documentation and not run. Validation steps 6 and 7
  settle them.
- **Workflow-level `env` can read `github.workspace`.** Taken from GitHub's context-availability
  table and not run. The PR's first CI run settles it: the configure log shows the checkouts under
  `build/dep-sources/_deps/`.
- **The CI images' CMake.** Read from the `actions/runner-images` READMEs on 2026-10-20, not from a
  job log. The first CI run settles it, because a lower version fails at `cmake_minimum_required`.
- **`git` is on PATH wherever configure runs.** FetchContent already requires it, so nothing changes.

## Invariants

- No build tree's `build.ninja` references a `*-build` or `*-subbuild` path outside that tree.
- A configure that has `STRAT_DEPS_SOURCE_CACHE` set and finds the checkout present makes no
  network access.
- A shared checkout is never modified after its rename.
- The shipping Release engine is byte-identical: same SHA-256, built at the same path with the same
  toolchain.
- Expanded compile, archive and link commands differ only in dependency source paths.
- No `build.ninja` contains module-scan rules or `dyndep`.
- No new configure warnings on Windows clang-cl or Linux.

## Validation

Build-tier change: full CI runs on the PR. No Elo match. No bench, conditional on step 2: an
identical engine binary cannot change speed.

1. **Expanded commands.** For each configuration below, build on `main` and on the branch at the same
   build path, then run `ninja -t commands StratChessEvolved StratChessTests`. That fully resolves
   every compile, archive and link command, Catch2's included.
   - Configurations: Windows `windows-clang-cl` and `-debug`, plus WSL `Ubuntu-26.04` GCC Release
     and Debug with `-DSTRAT_SANITIZE=address,undefined`. WSL has the same GCC as the CI image.
   - Normalize only the source, build and dependency-cache roots. Keep flags, library order and
     repetitions visible.
   - Record the CMake and compiler versions for each side (they must match).
   - Any difference other than those roots blocks acceptance until explained. If an engine codegen
     flag changes, the no-bench decision is revisited.

   Each configure log also shows zero warnings, and so does CI's.
2. **Binary identity.** Build the Release engine with `build.ps1 main`, on `main` and then on the
   branch, in this worktree at the same path. Compare `Get-FileHash` SHA-256.
   - **Identical:** the invariant is closed, and the search-equivalence run is skipped as redundant.
   - **Different:** find where the images differ (`llvm-objdump -h` section sizes, then the
     section). Explain it, then run `Compare-SearchEquivalence.ps1 -After <branch exe>`. An
     equivalence PASS alone does not close an unexplained difference.
   - Same build directory on both sides, per `Docs/CI.md` (`/Z7` embeds object paths). The
     dependency include roots still differ. If they reach the image at all, the place to look is the
     debug records; that is the first suspect, not an explanation.
3. **No scanning.** `grep -c dyndep build.ninja` returns 0 for all three Windows presets and for the
   WSL Linux configurations in step 1. Module builds always use dyndep.
4. **Private output.** Release and coverage in one worktree, plus a second worktree: no
   `catch2-build` path outside each tree appears in its `build.ninja`.
5. **Offline and pin bump.** With the checkouts present, a fresh worktree configures with no
   `Cloning into` in the log. A scratch `GIT_TAG` change to the previous Catch2 tag clones
   `catch2-v<old>` and leaves `catch2-v3.16.1` byte-identical (`git -C … status --porcelain` empty,
   same HEAD).
6. **Concurrency.** Two fresh configures start together against an empty `DepSources`. Both
   succeed, one clone per dependency, and no `.tmp` remains.
7. **Interrupted clone.** Kill a configure while it is cloning Catch2 into an empty cache.
   `catch2-v3.16.1` must not exist, and only `catch2-v3.16.1.tmp` may. Re-run the configure: it
   removes the `.tmp`, clones and succeeds.
8. **Older refs and existing trees.**
   - `Measure-CpuProfile.ps1 -Before 683f30f` and `Compare-BenchLinux.ps1 -Baseline 683f30f` both
     configure the old ref without cloning.
   - A worktree configured on `main` and then switched to the branch builds without cloning. Its
     `CMakeCache.txt` gains `STRAT_DEPS_SOURCE_CACHE`.
   - In CI: a Windows `build.ps1` leg's second run shows a cache hit and no `Cloning into`. A
     strength-lab dispatch with the minimum `games` and `reference_ref=683f30f` builds both sides,
     and the reference clones as it does today (D5). It runs only if the owner approves the lab
     time. Without it, the reference step is unchanged by construction: the same command, the same
     build directory, and a pre-change `CMakeLists.txt` that ignores the variable.
9. **Build time**, Windows clang-cl Release, median and range of ≥3 runs each, before and after:
   - fresh-tree configure;
   - cold full `build.ps1` with `CCACHE_DISABLE=1`;
   - warm no-op rebuild.

   Expected: configure the same or faster (no sub-builds); the other two within spread. A regression
   beyond spread blocks the PR until explained. CI: compare the Build step (not the job total)
   against recent `main` runs. The first run after the key rename is a cold ccache run on every leg,
   because the dependency include paths change, so compare the second run.
10. **Checker.** `test_dependency_releases.py` passes, and the checker still reads all three pins.

## Cost

- **Size:** 50–200 lines over about 14 files:
  - `CMakeLists.txt`, the new `cmake/DependencySources.cmake`, `CMakePresets.json`;
  - `build.ps1`, `Measure-CpuProfile.ps1`, `WslBuild.ps1`;
  - three workflows, the checker and its test;
  - `Docs/Workflow.md`, `Docs/Dependencies.md`, `Docs/CI.md`, the changelog.
- **Blast radius:** Build tier. Every build path's configure step changes. The CI caches are reset
  once, and one cold ccache run follows.
- **Review:** one code review. Validation takes about 1.5–2 h locally: step 1's four configuration
  pairs and step 9's timings dominate. The strength-lab dispatch in step 8 is optional and needs the
  owner's approval.
- **Optional, priced alone:**
  - **The `file(LOCK)`**, ~6 lines. It covers two configures racing on an empty cache or a pin
    bump. Without it, a loser's `RENAME` fails and that configure errors once. Recommended: keep.
    Recovery from an interrupted clone depends on it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| why 3.30, scanning off (D1, D2) | comment above `cmake_minimum_required` and the `CMAKE_CXX_SCAN_FOR_MODULES` line |
| tag-keyed checkouts, the `_deps` segment, the lock (D3) | header comment of `cmake/DependencySources.cmake` |
| base directory forced private; both variables passed for older refs (D5) | comment at the `FETCHCONTENT_BASE_DIR` line; comment in `Measure-CpuProfile.ps1` / `WslBuild.ps1` |
| env-variable transport, CI workflow-level `env` (D6) | `build.ps1` comment at the variable; workflow `env:` comment |
| layout and locations (D6); `Deps\` kept for older refs | `Docs/Workflow.md` → Dependency cache, Support folders |
| a pin bump clones a new directory; CI key rename | `Docs/Dependencies.md` → Bumping a version |
| build-time before/after | `Docs/Changelog.md` entry |
