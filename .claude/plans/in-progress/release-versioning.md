# Versioned releases and version in UCI `id name` — Design

**Issue:** #924 (owner decisions in the issue's second comment override its triage)

## Goal

StratChessUI runs the engine from outside this repository and cannot tell which build it has: the
engine sends a bare `id name StratChess` (`StratEngine/UCIHandler.cpp:162`), there are no releases,
and the shipped exe needs the VC++ Redistributable. Measured on `origin/main` `84300283` with the
x64 `llvm-objdump -p` from VS 18's `VC\Tools\Llvm\x64\bin` (the `ARM64\bin` copy does not run on
this host), `StratChessEvolved.exe` imports `KERNEL32.dll`, `MSVCP140.dll`,
`MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll` and nine `api-ms-win-crt-*` UCRT forwarders
(`StratChessTests.exe` adds `VCRUNTIME140_1.dll` and `api-ms-win-crt-environment`). The goal is a
binary that names its source, runs on a bare Windows install, and is published by an
owner-initiated, repeatable procedure.

## Review focus

- **D5, CRT placement.** The owner approved static CRT "in the normal Windows Release preset". This
  design sets it in `CMakeLists.txt` instead, because a preset edit never reaches an existing build
  tree. Check that this keeps the owner's intent of one recipe for measurement, CI and release.
- **D3, build-time hash capture.** Its correctness rests on Ninja `restat` skipping recompiles when
  the hash is unchanged (Assumption A2), and on the #580 reproducibility gates still passing with a
  generated TU.
- **D9, the supported UCI contract** puts only `Threads` and `Hash` under compatibility. The owner
  approved it on 2026-10-26.

## Scope

**This change will:**

- give the binary one version identity, reported in `id name` (slice 1);
- link the C runtime statically in every Windows configuration, validated for search equivalence
  and speed (slice 2);
- add a reusable standalone-exe check (DLL imports + empty-directory UCI smoke) (slice 2);
- add a manual-dispatch release workflow that stages a **draft** Release, plus the maintainer and
  user documentation (slice 3).

**This change will not:**

- detect releases from Git tags, mark dirty trees, or bump the version after a release (owner
  amendments 1–2);
- put a SHA-256 in release notes (owner amendment 4);
- ship installers, zips, Linux/macOS assets, a portable non-BMI2 build, automatic UI updates, a
  release cadence, or an Elo gate per release (issue non-goals);
- change `MEASUREMENT_CONTRACT` (`UCIHandler.cpp:157`), the bench contract it emits, or the
  executable's target name `StratChessEvolved`;
- start any slice. This PR is the document only.

## Decisions

### D1: CMake owns the number

`project(StratChess VERSION 0.8.0 LANGUAGES CXX)` replaces `CMakeLists.txt:6`. `PROJECT_VERSION` is
the only place the number is written. Rejected: a `VERSION` file, which would be a second source of
truth for CMake to read and could drift from it.

### D2: Identity string and the release switch

- `STRAT_RELEASE` is a `BOOL` cache option, default `OFF`.
- `ON`: the version is `X.Y.Z`. No Git call is made.
- `OFF`: `X.Y.Z+<hash>`, where `<hash>` is `git rev-parse --short=8 HEAD` run in
  `CMAKE_SOURCE_DIR`. The hash is eight characters, longer only if Git needs more to be unambiguous,
  which matches the issue's `3bd1ae39` example. If Git is missing or the command fails (a source
  archive, an unsafe-directory refusal), the hash is `unknown`.
- There is no dirty marker, no tag detection and no `-dev` pre-release segment. `0.8.0+3bd1ae39`
  means "built after 0.8.0" (owner amendments 1–2). SemVer build metadata does not order builds,
  and no consumer orders development builds.

`UciHandler::cmd_uci` sends `"id name StratChess " + std::string(EngineVersion())`. `id author` is
unchanged.

### D3: One generated TU; the hash is captured at build time

**Interface (slice 1's contract):**

- `StratEngine/Version.h` declares `std::string_view EngineVersion();`. It is included only by
  `UCIHandler.cpp` and the test.
- The definition lives in `${CMAKE_BINARY_DIR}/generated/Version.cpp`, rendered from
  `cmake/Version.cpp.in`. That template sits outside `StratEngine/`, so the `GLOB_RECURSE` at
  `CMakeLists.txt:173` never sees it.
- The generated file is added to both executables with `target_sources`. The test target compiles
  the engine separately, so it needs its own copy.

**When the hash is captured.** Under `STRAT_RELEASE=OFF`, an `add_custom_target(strat_version ALL
BYPRODUCTS <generated>/Version.cpp ...)` runs `cmake -P cmake/GenerateVersion.cmake` on every build.
The script renders to a temporary file, then copies it over the generated file with
`file(COPY_FILE ... ONLY_IF_DIFFERENT)`. Both executables `add_dependencies` on it. Under
`STRAT_RELEASE=ON`, a `configure_file` at configure time writes the file instead, with no custom
target.

Rejected alternatives:

- **Configure-time `execute_process`.** Ninja re-runs CMake only when `CMakeLists.txt` or a glob
  changes. A tree would keep reporting the hash it was configured at across any number of commits,
  which is a wrong identity: exactly the failure the issue exists to remove.
- **Configure-time plus `CMAKE_CONFIGURE_DEPENDS` on `.git/HEAD`.** In a worktree `.git` is a file.
  A commit moves `refs/heads/<branch>` or `packed-refs`, not `HEAD`. Tracking all three is fragile,
  and each hit costs a full re-configure, which is more than the build step costs.
- **The hash in a header.** Every includer would recompile on each commit and every ccache entry
  that includes it would go cold. Owner amendment 3 forbids it.

**What it costs.** One `git` process and one `cmake -P` process per build (A1). A new commit
recompiles `Version.cpp` in each target and relinks both exes, including the engine's ThinLTO link.
Nothing else recompiles, and ccache misses only that one object per target. No timestamp is ever
written (#580).

### D4: Reproducibility and the measurement contract stay separate

- `Test-ReleaseReproducibility.ps1` builds one commit twice, so both builds embed the same hash. Its
  scope, objects under `CMakeFiles/<target>.dir/` (line 85), picks up the two new `Version.cpp.obj`
  files automatically, and it hard-codes no artifact count.
- `MEASUREMENT_CONTRACT` (`UCIHandler.cpp:144-157`) already states it is "not a build id and not an
  engine version". It keeps its own number and its own `info string benchcontract` line.
- The strength lab's identity rule (`Docs/CI.md`, "Input identity") says generated engine inputs
  must extend it. The version TU carries no chess behaviour, so it is excluded by design. Two
  commits with identical tracked engine inputs are still refused, because input identity alone
  refuses them; only the binary-bytes arm of that rule can no longer match across commits.
  `Docs/CI.md` records this in slice 1.

### D5: Static CRT, set in `CMakeLists.txt` before the dependencies

```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

This goes beside `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` (`CMakeLists.txt:55`), above
`FetchContent_MakeAvailable` (`:142`). Placement:

- **Before the dependencies.** The variable initialises each target's `MSVC_RUNTIME_LIBRARY` when
  the target is created, so it has to be set before Catch2's targets exist. Catch2 is the only
  dependency that compiles anything: spdlog's library is `EXCLUDE_FROM_ALL` and consumed
  header-only, and nlohmann_json is an `INTERFACE` target.
- **Catch2 honours it.** Its `cmake_minimum_required(VERSION 3.16)` puts CMP0091 (3.15) at NEW in
  its scope. If the runtimes disagree, the link fails with `LNK2038` (RuntimeLibrary mismatch). It
  cannot pass silently.
- **Linux is unaffected.** Non-MSVC-ABI toolchains ignore the variable.

**Why `CMakeLists.txt` and not the preset.** `build.ps1` configures only when `CMakeCache.txt` is
missing (`Docs/Workflow.md`, Compiler cache). A preset `cacheVariables` edit therefore never reaches
an existing tree, and the freshness guard fails until every worktree's `build/` is deleted.
A `CMakeLists.txt` edit regenerates in place. Setting it here also covers the configures that bypass
the presets' defaults, such as `Measure-CpuProfile.ps1`'s own `cmake --preset ... -D` line.
Rejected: a packaging-only configuration, which the owner declined because shipped and measured
binaries would differ.

**Effects on other configurations:**

- **Debug** gets `/MTd`. It still defines `_DEBUG`, so `_ITERATOR_DEBUG_LEVEL=2` and `/RTC1` stay
  as they are. `/Z7` is independent of the runtime choice.
- **Sanitizers:** no Windows configuration exists to affect. `STRAT_SANITIZE` and
  `STRAT_STDLIB_DEBUG` are refused on clang-cl (the `FATAL_ERROR`s in `CMakeLists.txt`).
- **Coverage** inherits `/MT` through `windows-clang-cl-coverage`. Whether the profile runtime
  links against it is unverified (A3).
- **ccache:** `/MT` replaces `/MD` on every compile line, so each local tree and each Windows CI
  key (`ccache-windows-clang-cl-{release,debug}`, Nightly's `-release`) misses once and then runs
  warm again. Nothing in the `/showIncludes` depfile override or the `base_dir` guard depends on the
  runtime flag. Linux keys are untouched.
- **Old reference binaries** (`elo-reference-v2`, any merge base before slice 2) are `/MD`. Slice
  2's bench measures how large that difference is. Its `Measurements/local.md` note tells later
  local Elo readers that the reference crosses the CRT boundary.

### D6: One reusable standalone-exe check

`Scripts/Test-StandaloneExe.ps1 -Exe <path> [-ExpectedId <string>] [-Depth <n>] [-SelfTest]`
(slice 2's contract with slice 3). It exits 0 when every check passes and 1 otherwise, printing one
PASS/FAIL line per check:

1. **Imports.** It locates `llvm-objdump` through `vswhere` under `VC\Tools\Llvm\x64\bin`, never
   `ARM64`, and runs `-p` on the exe. Every `DLL Name` must be on an allowlist of OS-shipped DLLs.
   The allowlist starts as what slice 2 measures, expected to be `KERNEL32.dll` alone, since `/MT`
   also links the UCRT statically. Each entry is added with its reason. An unlisted import fails,
   so a new `MSVCP*`, `VCRUNTIME*` or `api-ms-win-crt-*` import fails too.
2. **Empty-directory UCI smoke.** It copies the exe alone into a fresh temporary directory and runs
   it with that directory as the working directory and no arguments, which is UCI mode
   (`StratChessEvolved.cpp`, `argc == 1`) and is how a GUI launches it. `UciDriver.ps1` hard-codes
   the `uci` launch argument (`:65`, `:75`), so slice 2 gives it an optional argument list whose
   default stays `uci`, which leaves its existing callers unchanged. It drives `uci` and `isready`
   through the driver's read-until-reply loop (piped input searches nothing), then
   `position startpos` / `go depth <n>` (default 8) through `bestmove`, then `quit`, and requires
   exit 0 within a timeout. `uciok`, `readyok` and `bestmove` must appear. With `-ExpectedId`, the
   `id name` line must equal it exactly.
3. **Filesystem.** After the process exits, the directory must contain only the exe. This
   establishes standalone operation that leaves no output files. It is not a filesystem trace, so
   it does not prove the absence of every read or every transient write. That corrected form, from
   the triage, is the fact the issue's acceptance asked to record. Only `--log-commands` writes, to
   `logs/uci_commands_<pid>.log`, or with `--log-commands=<path>` to that path verbatim, which may
   be outside `logs/` (`StratChessEvolved.cpp:291-292`, `UCIHandler.cpp:105`).

It lives in `Scripts/` because `.gitignore:372-380` allowlists only `*.py`, `*.yml` and `action.yml`
under `.github/`. A `.ps1` there would be silently untracked. Release CI invokes it, so slice 2 adds
it to `Get-ChangeTier.ps1`'s Build list, with a classifier self-test case. `Get-ChangeTier.ps1:31`
requires a new gate script to join that list; an unlisted `Scripts/*.ps1` classifies as Tooling.

### D7: The release workflow, `.github/workflows/release.yml`

`on: workflow_dispatch` with one required input, `tag`. The workflow never creates a tag and has no
`push: tags` trigger, so the `uci-facing` label never triggers a release. `concurrency: release`
with `cancel-in-progress: false`. Every job sets `timeout-minutes` (`Test-WorkflowTimeouts.ps1`).

**`build`** runs on `windows-2025-vs2026` with `contents: read`:

1. Check out the tag with `fetch-depth: 0`.
2. Fail in seconds unless all three hold:
   - the tag matches `^v(\d+)\.(\d+)\.(\d+)$`;
   - `X.Y.Z` equals `project(... VERSION X.Y.Z` in `CMakeLists.txt`;
   - `git merge-base --is-ancestor <tag commit> origin/main` succeeds. This is the provenance
     check: the tag must sit on `main` history.
3. Fail early if `gh release view <tag>` finds a published release. A read token cannot see
   drafts, so this check catches published releases only; `stage` makes the authoritative check.
4. Build with `.\build.ps1 all -Publish`. This is a new `windows-clang-cl-publish` preset that
   inherits `windows-clang-cl` and adds only `STRAT_RELEASE=ON`. The `-Publish` switch follows
   `-Coverage`'s precedent and accepts Release only. The build is **uncached** for the same reason
   as Nightly's `extended-tests` (`Docs/CI.md`): a wrong cached object must not reach the shipped
   binary.
5. Run the fast tier (`~[slow]`), then `Test-CodeAlignment.ps1 -MapPath` on that tree's map.
6. Run `Test-StandaloneExe.ps1 -ExpectedId "StratChess X.Y.Z"`. An exact plain id proves both that
   `STRAT_RELEASE` reached the build and that the tag matches the built version.
7. Upload `StratChess-X.Y.Z-windows-x64.exe` (a renamed copy) plus a generated
   `release-notes.md` skeleton as a workflow artifact. The skeleton holds:
   - the requirements line ("x64 Windows, CPU with AVX2 + BMI2. Single .exe, no runtime install.");
   - the source SHA, `clang-cl --version` and the runner image;
   - empty *Integration impact*, *Strength* and *Fixes* headings for the owner to fill in.

**`stage`** runs on `ubuntu-latest` with `contents: write`, needs `build`, and is the only job
holding a write token:

- **No release for the tag:** `gh release create <tag> --draft --verify-tag --notes-file
  release-notes.md <asset>`.
- **Draft exists** (a re-run): re-check `isDraft` immediately before the write, then `gh release
  upload <tag> <asset> --clobber`. The notes are left alone, since the owner may have edited them.
- **Published:** fail. The workflow never runs `--clobber` against a published release and never
  deletes a release or a tag.

The decision logic (tag shape, version match, which of the three release states applies) lives in
`.github/scripts/release_stage.py` with a `--self-test`. The PR gate's Linux Release leg runs it
like the other workflow helpers, so the mismatch, missing-tag and published-state branches are
covered by fixtures rather than only by a live run.

**The re-check is not atomic.** `--clobber` deletes the old asset before uploading the new one, and
the owner can publish between the re-check and the write. Workflow concurrency serializes runs, not
the owner's action on the release page. Invariant I8 therefore rests on an operating rule, recorded
in `Docs/Releasing.md`: **publish only when no `release.yml` run for the tag is queued or running,
the latest run succeeded, and the draft shows the asset.** If a staging upload fails, the draft may
be left without its asset; re-dispatching the same tag recovers it. A release published without its
asset is fixed by a new PATCH version, never by uploading to the published release. This is an
operator race, not an attacker scenario. Rejected: an atomic server-side draft-only write, which
the CLI does not offer.

**Failure behaviour.** Any failure in `build` stops before `stage`, so no release object is created
or changed. Re-dispatching the same tag is the retry. A defective *published* binary gets a new
PATCH version and notes pointing to it. The owner reviews the draft and publishes it by hand.
Rejected: a `push: tags` trigger, which would publish on tag creation and remove the owner's review
step the issue approved.

### D8: Documentation touchpoints

| File | Change | Slice |
|---|---|---|
| `Docs/EngineGuide.md` | New "Versions and requirements" section: `id name` format and what `+<hash>`/`+unknown` mean; AVX2 + BMI2, x64 Windows, single exe, no runtime install; UCI reads and writes nothing beside the exe, `--log-commands` paths as in D6; subscribing via Watch → Custom → Releases or `/releases.atom`; the D9 contract | 1 (identity), 2 (requirements, files), 3 (subscribe, D9 contract — moved from slice 1 in implementation so the contract lands with the release docs) |
| `Docs/Releasing.md` (new) | Maintainer procedure: bump `project(VERSION)` in a PR, merge, confirm green, tag `vX.Y.Z` on that `main` commit, dispatch `release.yml`, curate notes from `uci-facing` PRs and Changelog headlines, then publish. Strength lines only with a linked measurement (reference, conditions, interval); lab GCC rows are labelled as GCC. Retry and defective-release handling as in D7 | 3 |
| `Docs/CI.md` | New `release.yml` section and an index row; the generated-version exclusion in "Input identity" (D4) | 1, 3 |
| `Docs/Workflow.md` | Raw-CMake preset list gains `windows-clang-cl-publish`; one line that Windows builds link the CRT statically | 2, 3 |
| `.claude/skills/open-pull-request/SKILL.md` | One rule under "3. PR body": apply `uci-facing` (`gh pr edit <n> --add-label uci-facing`) when a PR changes anything a UCI client can observe, meaning any advertised option (tuning options included), `info` output including `info string` diagnostics, `bestmove`, `id` lines or protocol behaviour. The label is a notification and is broader than D9, which decides only the version consequence. Loads `writing-for-agents` first | 3 |
| `Docs/Changelog.md` | One entry per slice | 1–3 |

### D9: The supported UCI contract

From 1.0, the compatibility rules (MAJOR for a break; before 1.0, MINOR for breaking changes,
additive features or measured strength gains, PATCH for fixes) apply to:

- the `id` lines;
- the commands `uci`, `isready`, `ucinewgame`, `position`, `go` (with the parameters EngineGuide
  documents), `stop` and `quit`;
- the `Threads` and `Hash` options: name, type, range and default semantics as advertised, and their
  `setoption` behaviour;
- the `info` fields EngineGuide's "Interpret search output" defines;
- the `bestmove` line.

The `SearchTuningSchema` options (`LmrLogBase` … `StabilityTimeFloor`) are advertised but
**unsupported tuning controls**: they may be renamed or removed in any MINOR. `info string` lines
are diagnostics, not contract. Rejected: putting every advertised option under contract, which would
make each tuning experiment that retires a knob a MAJOR change after 1.0. The owner approved this
boundary on 2026-10-26. No UI should depend on the unsupported options.

## Assumptions I cannot verify from the code

- **A1: the build-time version step adds little to each build.** It costs one `git` plus one
  `cmake -P` per build, and a relink of both exes after each commit. Not measured. Settled in slice
  1 by timing `build.ps1 all` three ways: no change (expect sub-second over today's no-op), after an
  empty commit (expect one recompile per target and two links), and with nothing changed again.
- **A2: Ninja skips dependents when `Version.cpp` is unchanged.** CMake emits `restat = 1` for
  custom commands with `BYPRODUCTS` under Ninja, so an unchanged file recompiles nothing. Taken from
  CMake's documentation, not observed. Settled by A1's third build reporting no compile edge.
- **A3: the coverage runtime links under `/MT`.** clang-cl's `clang_rt.profile` is assumed to link
  against the static CRT. Settled in slice 2 by one `Get-Coverage.ps1` run.
- **A4: static CRT has no meaningful nps effect.** The hot path calls little CRT code (UCRT `malloc`
  and `memcpy` are the same code either way), but placement moves. Settled by slice 2's bench (Validation).
- **A5: `/MT` removes every `MSVCP*`, `VCRUNTIME*` and `api-ms-win-crt-*` import.** Settled by
  `Test-StandaloneExe.ps1` in slice 2; the measured list becomes the allowlist.
- **A6: UCI mode needs no adjacent file and leaves no output file.** The source shows verbose
  logging off (`UciHandler::DefaultSearchConfig()`), spdlog off, and no `game_settings.json` read.
  Settled by D6's empty-directory check, not by reading the source. That check is not a filesystem
  trace.
- **A7: GitHub behaves as D7 assumes.** That covers four things:
  - a draft's assets stay invisible to `/releases.atom` until publication;
  - `gh release upload --clobber` works on a draft;
  - `--verify-tag` refuses a missing tag;
  - the release page shows each asset's digest (owner amendment 4);
  - `workflow_dispatch` needs the workflow file on the default branch. This last one is taken from
    GitHub's documentation, and it is why slice 3 has no pre-merge live run.

  The draft path is settled by the first real `v0.8.0` dispatch and its re-dispatch. Feed visibility,
  the digest and published-state refusal are settled at and after the owner's first publication
  (slice 3 Validation). Neither is pre-merge evidence.
- **A8: the runner image has the tool D6 needs.** `windows-2025-vs2026` is assumed to ship
  `VC\Tools\Llvm\x64\bin\llvm-objdump.exe`. It comes with the VS Clang component `build.ps1` already
  requires. Settled by the first `v0.8.0` dispatch.

## Invariants

- **I1:** Only a build configured with `STRAT_RELEASE=ON` reports a plain `X.Y.Z`. Every other
  build's version contains `+`.
- **I2:** Two builds of one commit and configuration are byte-identical within
  `Test-ReleaseReproducibility.ps1`'s scope, in both modes. No timestamp enters the version.
- **I3:** A commit that changes no compiled source recompiles only `Version.cpp` per target.
- **I4:** `benchcontract` stays 2 and stays independent of the version.
- **I5:** Search is node-identical across both code slices: same node counts, best moves and
  `info string` lines.
- **I6:** Catch2 and both project targets agree on the CRT in Release and Debug.
- **I7:** A shipped exe imports only allowlisted OS DLLs, needs no adjacent file, and writes
  nothing in UCI mode without `--log-commands`.
- **I8:** The workflow never modifies a published release, asset or tag, **provided** the owner
  follows D7's publication rule: publish only when no run for the tag is queued or running, the
  latest run succeeded, and the asset is present. Without that rule, a publish between the
  re-check and the upload is unguarded.

## Validation

**Slice 1: version identity** (Engine tier: `CMakeLists.txt`, `UCIHandler.cpp`).

- A Catch2 `[uci]` test:
  - the first `cmd_uci` line equals `"id name StratChess " + EngineVersion()`;
  - `EngineVersion()` matches `^\d+\.\d+\.\d+(\+([0-9a-f]{8,}|unknown))?$`;
  - its numeric part equals `PROJECT_VERSION`, passed to the test target as a define.

  Falsify it by reverting the `send` line and watching it fail. The test does not assert the `+`
  suffix, because `release.yml` runs the same suite under `STRAT_RELEASE=ON`.
- I1: configure a scratch tree with `-DSTRAT_RELEASE=ON` and check `id name StratChess 0.8.0`. On a
  normal tree, check `+<HEAD short hash>`. Point `PATH` away from Git and rebuild to see `+unknown`.
- A1/A2/I3: the three timed builds, with ccache stats showing one miss per target after the empty
  commit.
- I2: `Test-ReleaseReproducibility.ps1`, both modes. It builds the ordinary preset, so it covers
  the `+<hash>` path. It is not evidence for `STRAT_RELEASE=ON`, whose file is a fixed
  `configure_file` with no Git input.
- I5: `Compare-SearchEquivalence.ps1 -After <exe> -BaselineRef origin/main`. Its `id name` fixture
  lines (`Compare-SearchEquivalence.ps1:323`, `Run-Bench.ps1:230`) are self-test data and parse
  nothing.
- No bench: the change adds no per-node work, only one startup call. Slice 2's bench baseline
  already contains it.
- Linux is covered by CI's GCC legs, which build `all`.
- **Acceptance:** `id name` identifies the source state (I1), only one TU recompiles per commit
  (I3), reproducibility and equivalence pass.

**Slice 2: standalone recipe** (Build tier, plus the new script).

- `Test-StandaloneExe.ps1 -SelfTest` against fixture `objdump` output and `FakeUciEngine.ps1`.
- A real run on the slice-2 Release exe settles A5, A6 and I7, and fixes the allowlist.
- `build.ps1 all -Config Debug` and `run-tests` pass (I6: `LNK2038` would fail the link).
- `Get-Coverage.ps1` completes (A3).
- `Test-ReleaseReproducibility.ps1`, both modes (I2).
- I5: `Compare-SearchEquivalence.ps1`.
- **Speed:** a paired `Compare-Bench.ps1` series, per `measure-strength`'s regression check:
  - the baseline is the merge base built by its own `build.ps1 main` in a detached worktree, never
    the equivalence cache's exe;
  - the series runs in a quiet window, announced first.

  **Acceptance threshold, fixed before the run:** the 95% lower bound must be at least −0.5%. That
  is a verdict of **No slowdown**, or **Speedup**, which `Compare-Bench.ps1` tests first and whose
  lower bounds exceed zero. *Unresolved* or *Slowdown* escalates once, as the skill prescribes:
  `New-OrderedBuildPair.ps1`, then `-Control -Rounds 60 -Affinity 4`. That series must also read
  No slowdown or Speedup. Anything else, including a relink that cannot produce an ordered pair,
  stops slice 2 and goes back to the owner with the numbers. No verdict carries an Elo inference.
  The benefit here is distribution, not strength, so no stated benefit pre-approves a cost. Record
  the result in `Measurements/local.md` with exe sizes before and after.
- **No Elo match.** The change is node-identical by I5, and a bench verdict is not an Elo claim. The
  strength lab builds with GCC on Linux and is untouched.
- **Acceptance:** `Test-StandaloneExe.ps1` passes on the Release exe with no redistributable
  import (I7), Debug and coverage build and pass, and the bench verdict meets the threshold above.

**Slice 3: workflow and docs** (Build tier for `release.yml`; Docs for the rest).

- `Test-WorkflowTimeouts.ps1` and `Test-WorkflowCcachePaths.ps1` pass.
- `release_stage.py --self-test` runs in the PR gate. Its fixtures cover each branch on its own: a
  malformed tag, a tag that does not exist, a version mismatch, no release yet, an existing draft,
  and a published release. The last must refuse without writing.
- A workflow that is not yet on the default branch cannot be dispatched, and the provenance check
  refuses any tag outside `main` history. **There is therefore no pre-merge live run.** The live
  evidence comes after merge, in this order:
  1. The owner tags the first real `v0.8.0` on `main` and dispatches. The run must stage a draft
     whose downloaded asset reports `id name StratChess 0.8.0` and passes `Test-StandaloneExe.ps1`
     on the dev machine.
  2. A re-dispatch of the same tag replaces only the asset and leaves the notes alone.
  3. A dispatch with `v9.9.9` (no such tag) must fail before any build. Version mismatch is
     covered by the fixtures only; the owner declined a live check, which would need a junk tag.
  4. Record the job's duration (A7 draft path, A8).
- After the owner's first publication: a re-dispatch of `v0.8.0` must fail without writing (I8),
  and the feed and digest are checked (A7). The publication itself is the owner's act.
- **Acceptance:** the PR merges on the fixtures plus the workflow guards. The slice closes once
  steps 1–3 pass on `v0.8.0`. A failure there is fixed by a follow-up PR before publication.

## Cost

- **Size:** about 500–800 lines in total, across about 20 distinct files. These are forecasts.
  By slice:

  | Slice | Lines | Files |
  |---|---|---|
  | 1 | 100–150 | `CMakeLists.txt`, two `cmake/` files, `Version.h`, `UCIHandler.cpp`, one test, EngineGuide, CI.md, Changelog |
  | 2 | 180–280 | one `CMakeLists.txt` line, `Test-StandaloneExe.ps1` with its self-test, the `UciDriver.ps1` launch argument, a `Get-ChangeTier.ps1` Build entry with a self-test case, EngineGuide, Workflow.md, Measurements, Changelog |
  | 3 | 250–380 | `release.yml`, `release_stage.py` with fixtures, a `CMakePresets.json` preset, the `build.ps1` switch, `Releasing.md`, CI.md, EngineGuide, the skill line, Changelog |

- **Blast radius:**
  - Slice 1 is Engine tier: every build gains the version step, and every commit relinks both exes.
  - Slice 2 is Build tier: the Windows CRT changes for every developer and CI leg, and each ccache
    misses once.
  - Slice 3 adds a workflow that gates nothing.
- **Review:** one code review per slice, 170–270k tokens and 3–5 min each in the #624 pilot. No
  domain review: nothing here touches search or evaluation.
- **Measurement:** slice 2's bench is about 5 quiet minutes, or about 35 more if it escalates. No
  lab time.
- **Optional, each priced alone:**
  - *Provenance check* (`merge-base --is-ancestor`): about 5 lines. It stops a tag on an unmerged
    branch from shipping. Keep it.
  - *Mismatched-tag live check*: dropped by the owner (2026-10-26), because the fixtures cover the
    same branch.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1–D3: version source, identity format, why build-time capture, single TU | comment above the version block in `CMakeLists.txt`; `cmake/GenerateVersion.cmake` header |
| D2 meaning of `+<hash>` / `+unknown` for users | `Docs/EngineGuide.md` → Versions and requirements |
| D4 version vs `MEASUREMENT_CONTRACT` | existing comment at `UCIHandler.cpp:144` gains one line pointing at `Version.h` |
| D4 lab input identity excludes the generated TU | `Docs/CI.md` → Input identity |
| D5 static CRT, placement before deps, `LNK2038` as the guard | comment beside `CMAKE_MSVC_RUNTIME_LIBRARY` in `CMakeLists.txt` |
| D5 measured nps and size change | `Measurements/local.md` row, headline in `Docs/Changelog.md` |
| D6 allowlist reasons, UCI filesystem fact | `Test-StandaloneExe.ps1` help; `Docs/EngineGuide.md` → Logging and runtime files |
| D7 dispatch, draft, retry, never-overwrite | `Docs/Releasing.md`; `release.yml` comments; `Docs/CI.md` |
| D7 publication rule behind I8; recovery from a failed upload | `Docs/Releasing.md`; `release_stage.py` header |
| D9 supported UCI contract | `Docs/EngineGuide.md` → Versions and requirements |
| `uci-facing` rule | `open-pull-request` skill |

Approved decisions that change during implementation are recorded here and in that slice's PR body.
