# #81 Phase 3 — Retire `.vcxproj`

**Goal:** make CMake the only build system. Delete the `.sln`, both `.vcxproj`, both
`.vcxproj.filters` and `Directory.Build.props`, with every script, workflow and document that
currently drives MSBuild moved onto CMake presets.

**Closes:** #81 Phase 3 (the `.vcxproj` half), and #84 — adoption of the clang binary happens by
virtue of clang-cl becoming the shipping preset.

## Decisions taken with the project owner (2026-08-03)

1. **Both Windows compilers stay supported.** `windows-msvc` and `windows-clang-cl` presets. MSVC is
   the fallback and remains fully usable for daily development including Edit and Continue; the
   project owner intends to try clang-cl as the daily driver, with MSVC as the escape hatch.
2. **clang-cl is the shipping compiler.** Releases and every measurement use it.
3. **`build.ps1` survives as a thin wrapper** over `cmake --preset`, keeping its current verbs and
   its first-run `core.hooksPath` setup. This keeps the blast radius inside one file.
4. **No new Windows CI leg.** Windows CI keeps its current trigger policy (push to `main`, or the
   `windows-ci` label) and simply builds the clang-cl preset instead of the solution.
5. **clang-tidy / clang-format are out of scope** — split to #175.

## Out of scope

- #83 static library extraction. It changes link topology and owes its own nps measurement; folding
  it in here would make this PR's equivalence gate unattributable.
- #167 cleanup items (SAL annotations, `<cstdint>`, `far`/`near`, `StdAfx.cpp`).
- Historical `.claude/plans/**` and `Docs/superpowers/**` documents. They record past work and
  describe the build system as it was at the time. Rewriting them would falsify the record.

## What CMake already covers

`CMakeLists.txt` and `CMakePresets.json` (PR #168, PR #174) already provide both targets on Linux
(GCC/Clang) and Windows (clang-cl), `STRAT_ENABLE_TEST_ACCESS`, `-Werror`/`/WX` clean on all three,
ThinLTO on the engine in Release, and `compile_commands.json`. The clang-cl Release binary is
verified equivalent to the MSBuild binary (identical node counts and best moves) and ~25% faster.

## Findings that shape this plan

**Preprocessor-define parity is a non-task.** The `.vcxproj` files define `WIN32`, `_CONSOLE`,
`_DEBUG`/`NDEBUG`. Searching every non-archived header and source for conditional use finds
**`NDEBUG` only** (4 uses, all in `BitBoardHelper.h`), which CMake already sets for Release.
`_CONSOLE`, `_WINDOWS`, `WIN32`, `_DEBUG` and `_WIN32_WINNT` appear in no `#if`/`#ifdef`/`defined()`
anywhere. Do not spend effort replicating defines nothing reads.

**MSVC through CMake is currently broken, silently.** `strat_configure_target` branches on
`CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"`, which real `cl.exe` also satisfies. Configure
succeeds; the build then fails:

```
cl : Command line warning D9002 : ignoring unknown option '-mavx2'
cl : Command line warning D9002 : ignoring unknown option '-mbmi2'
cl : Command line warning D9002 : ignoring unknown option '/clang:-Wall'
cl : Command line warning D9002 : ignoring unknown option '/clang:-fconstexpr-steps=100000000'
base.h(458): error C2338: static assertion failed: 'Unicode support requires compiling with /utf-8'
Magic.h(145): error C2131: expression did not evaluate to a constant
```

The D9002 lines matter as much as the errors: **`-mavx2 -mbmi2` are silently dropped**, so a naive
MSVC preset would build a working but slower binary with no AVX2/BMI2. That is exactly the class of
failure the nps gate in Task 1 exists to catch.

**`/utf-8` is absent from `CMakeLists.txt` entirely.** Both `.vcxproj` files have always passed it.
clang-cl defaults to UTF-8 input so it has not bitten, but MSVC needs it (spdlog static-asserts on it).

**The two build systems have been compiling against different library code** (found via PR #176).
`.vcxproj` resolves spdlog and nlohmann from sibling checkouts through `$(DepsRoot)`, so its versions
drift with whatever is checked out there — the local `json/` sits 240 commits past `v3.11.3` on
develop — while CMake pins v3.12.0. Both report identical `NLOHMANN_JSON_VERSION_*` macros, so
comparing versions does not reveal it. It produced a real crash: `game` mode died instantly on every
CMake-built binary, because the pinned nlohmann's stream operator does not ignore the comments in
`game_settings.json` while the drifting checkout's does. Retiring `.vcxproj` deletes the unpinned
mechanism outright, which is a substantive reason to do this beyond having one build system.

**Dependency acquisition changes cost per worktree.** `Directory.Build.props` resolves spdlog and
nlohmann from *sibling checkouts* shared by every worktree. `FetchContent` clones per build tree
instead, so each new worktree pays a clone on first configure. Consider setting
`FETCHCONTENT_BASE_DIR` to a machine-level shared cache; CLAUDE.md's existing "give `git commit` a
5-10 minute timeout on first build in a fresh worktree" note needs re-checking against the new cost.

## Tasks

Ordered so that Tasks 1-4 are reversible and `.vcxproj` still works throughout. Task 5 is the
point of no return.

### Task 1 — CMake Windows parity, and an MSVC preset

- Narrow the clang-cl condition to `CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
  CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"`.
- Add an MSVC branch: `/utf-8 /constexpr:steps100000000 /arch:AVX2 /permissive- /W4 /WX`, and
  `/FI<Compat.h>`. Note `/arch:AVX2` is MSVC's spelling of what `-mavx2 -mbmi2` does for the others;
  `_pext_u64` needs BMI2, which `/arch:AVX2` covers on MSVC.
- Add `/utf-8` to the clang-cl branch too, for exactness with what `.vcxproj` shipped.
- Add `windows-msvc` and `windows-msvc-debug` presets (`CMAKE_CXX_COMPILER: cl`), keeping
  `CMAKE_RC_COMPILER: llvm-rc` — the long-path fix is compiler-independent.
- Confirm `ConformanceMode` parity: `.vcxproj` sets it, CMake's `CXX_EXTENSIONS OFF` does not imply
  `/permissive-` on MSVC, hence setting it explicitly above.

**Verify:** all four Windows presets configure and build clean. Then the nps gate below — this is
what proves no flag was silently dropped.

### Task 2 — One place that knows where binaries live

Presets emit to `build/<preset>/`; scripts currently assume `x64/Release/`. Rather than hardcode the
mapping in five scripts, add `StratChessEvolved/Scripts/Get-BuildArtifact.ps1` that takes a target
and config and prints an absolute path, invoked with `-File` like every other script here.

Consumers to update: `Validate-PreCommit.ps1`, `Validate-PrePR.ps1`, `Run-Tests.ps1`,
`Run-EloMatch.ps1`, `Run-Bench.ps1`.

**`Run-EloMatch.ps1` and `Run-Bench.ps1` must default to the clang-cl Release artifact and print
which build they used.** With two Windows compilers in play, measuring an MSVC-built candidate
against a clang-built reference would read as a ~40 Elo regression that does not exist. Making the
build visible in the output is the cheap guard.

### Task 3 — `build.ps1` becomes a CMake wrapper

Keep every verb (`main`, `tests`, `all`, `run-tests`, `extended-tests`) and `-Config`. Add a
`-Preset` parameter defaulting to `windows-clang-cl`, so MSVC is one flag away. Keep the first-run
`core.hooksPath` setup — it is why fresh worktrees get the pre-commit hook. `vswhere` discovery
changes from locating MSBuild to locating the VS environment CMake and clang-cl need.

**Verify:** each verb, both configs, both presets.

### Task 4 — CI

`.github/workflows/build-and-test.yml`: the Windows job builds `cmake --preset windows-clang-cl`
(Release and Debug) instead of the solution. Trigger policy unchanged. The Linux job is untouched.

### Task 5 — Delete

`StratChessEvolved.sln`, `StratChessEvolved/StratChessEvolved.vcxproj`(`.filters`),
`StratChessTests/StratChessTests.vcxproj`(`.filters`), `Directory.Build.props`,
`Directory.Build.user.props.example`.

Leave `Get-ChangeTier.ps1`'s `*.vcxproj` / `*.sln` / `*.props` rules in place. They cost nothing and
still classify correctly if such a file is ever reintroduced.

### Task 6 — Documentation

- **`CLAUDE.md`** — the Build section, and **delete the "Adding files to the solution" bullet**
  (`ClInclude` / `<Filter>` / "a `.cpp` missing from the project is silently never compiled").
  CMake globs with `CONFIGURE_DEPENDS`; that hazard ceases to exist. Also re-check the fresh-worktree
  build-timeout note against `FetchContent` clone cost.
- **`Docs/Workflow.md`** — raw MSBuild invocation section, validation tiers.
- **`.claude/skills/run-tests/SKILL.md`**.

## Validation gates

1. **MSVC faithfulness (the important one).** `windows-msvc` Release vs the current MSBuild Release
   binary: identical node counts and best moves via `Run-Bench.ps1`, **and nps within noise**
   (per-config spread is under 2%, so a real regression from a dropped `/arch:AVX2` would be obvious).
   Run 5 passes per build — one sample has misled this project before (#161).
2. **clang-cl equivalence** re-confirmed at this commit: identical nodes and best moves vs MSBuild.
2b. **Every `main()` entry point, on every Windows preset** — `game`, `perft`, `tactical`, `eval`,
   `test-fen`, and bare (UCI). Added after PR #176, where `game` mode crashed on all CMake-built
   binaries and no existing check caught it: `Run-Bench` and `Run-EloMatch` both drive UCI, and CI
   runs the Catch2 binary, so nothing outside the app ever reached `Game::Init`. `perft`, `tactical`
   and `eval` have still never been run from a CMake-built binary — treat them as unverified rather
   than assuming `game` was the only casualty.
3. **Debug builds green on both Windows presets**, and the Debug test suite passes. Release hides
   out-of-bounds reads that Debug catches.
4. **Linux unchanged** — GCC Release and Debug via CI, which already runs on every PR.
5. **Fresh-worktree cold path**: `New-Worktree.ps1` then `build.ps1 all`, timed, with no
   pre-existing `_deps` cache.
6. `Validate-PrePR.ps1` at whatever tier it self-classifies (expected Engine, since it fails closed).

## Risks

- **The measurement footgun** from two Windows compilers, addressed in Task 2. It is the most likely
  thing to cause a wrong conclusion later, and it will not announce itself.
- **Re-pinning the Elo reference.** Once the shipping binary is clang-built, the reference in
  `Docs/EloLog.md` must be re-pinned to a clang build, or the next change's match silently inherits
  the +40 Elo compiler delta. Do this as part of this PR and note it in the EloLog row.
- **No CI covers the MSVC preset.** Accepted: the project owner builds it daily, which is faster
  feedback than CI would give.
- **Rollback.** Until Task 5 lands, `.vcxproj` still works. After it, rollback is `git revert`.
