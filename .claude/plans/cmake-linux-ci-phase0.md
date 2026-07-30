# CMake + Linux CI — Phase 0 thin slice (issue #82)

## Goal

One outcome: **a green `ubuntu-latest` build and fast-tier test run**, so pull requests validate on
Linux at 1x billing instead of Windows at 2x.

`.vcxproj`, `.sln` and `build.ps1` remain untouched and authoritative for local Windows work. This
slice adds a second, parallel way to build; it does not replace the first.

## Why this scope and not more

Issue #81 records the forcing function: the GitHub Actions quota for July was exhausted and every
job — including the Linux `classify` job — failed. Minutes reset 2026-08-01. `build-and-test` runs
on `windows-2025-vs2026`, and Windows bills at 2x per minute against included minutes while Linux
bills at 1x. A Release+Debug matrix at ~6 minutes costs ~24 billed minutes per run where Linux would
cost ~12.

So the driver is cost, and the blocker is portability: nothing in the repository builds outside
MSVC/MSBuild on Windows.

### Explicitly out of scope

| Excluded | Why | Where it goes |
|---|---|---|
| Static-library extraction | Changes link topology; needs its own nps verification, which would land on the deadline's critical path | #83 |
| `-Werror` under GCC/Clang | Warning count is unknowable until first compile; must not block the budget fix | Follow-up sized by the actual count |
| Dependencies as real CMake packages | Needs a 21-file `#include` rewrite in the test sources | #166 |
| Windows/MSVC support in CMake | Would ship compiler-conditional code that Phase 0 CI never exercises | Phase 3 |
| `INTERPROCEDURAL_OPTIMIZATION` | See "Deferred: IPO" below | Phase 3 / #83 |
| Sanitizers, `clang-tidy`, `clang-format` | Additive once Linux CI exists | Phase 1 |
| Retiring `.vcxproj` | Only after CMake has covered local Windows work for weeks | Phase 3 |

## Findings that corrected the issue text

These were measured against the tree at `17869ee`, and differ from what #82's comment records.

- **SAL annotation count has drifted.** The issue states 88 across 23 files. Actual: **152 across 27
  files** (116 `_In_`, 32 `_Inout_`, 4 `_Out_`), of which **132 across 21 files** are in sources the
  build actually compiles — the remainder live in `Archived/`. Exactly three spellings are used; no
  `_opt_`, `_reads_` or `_writes_` variants exist. The chosen fix is count-independent, so this does
  not change the design — but the figure was presented as measured and is no longer accurate.
- **17 of those 27 files are headers, and headers do not include `StdAfx.h`.** This is the finding
  that determines the `Compat.h` delivery mechanism: putting the include inside the PCH *header*
  would leave macro visibility dependent on include order.
- **The dependency blocker is weaker than stated.** CI already acquires dependencies on a fresh
  runner today via `git clone --depth 1 --branch <tag>` plus `actions/cache` plus a generated
  `Directory.Build.user.props`. `FetchContent` is therefore a deliberate choice — it lets a developer
  run `cmake -B build` on a bare clone with no CI scaffolding — not a forced one.
- **All three dependencies are header-only include directories.** No `SPDLOG_COMPILED_LIB`, no linked
  libraries; Catch2 is consumed as the amalgamated `catch_amalgamated.hpp` + `.cpp` copied out of
  `extras/`. This is what makes include-only consumption viable and defers #166.
- **The Catch2 fast tier is self-contained.** No Catch2 test loads `Tests/*.json`. The
  `std::filesystem::current_path().parent_path() / "Tests/"` loaders in `TacticalTestRunner.cpp` and
  `Perft.cpp` are used by the executable's own suites, not by the fast tier. Working-directory
  fragility is therefore not a Phase 0 blocker — but it will be when those suites run under CMake.
- **`StratEngine/Archived/` contains SAL-carrying sources.** It is excluded from the `.vcxproj` build
  and must be excluded from the glob; otherwise Phase 0 would start compiling code the project has
  deliberately retired.
- **`Get-ChangeTier.ps1` fails closed to `Engine`.** An unrecognised path such as `CMakeLists.txt`
  gets the strictest tier, so full validation runs. Safe by default; an explicit `Build` rule is a
  correctness improvement, not a fix.
- **There is no precompiled header today.** CLAUDE.md describes `StratEngine/StdAfx.h` as "the PCH",
  but neither `.vcxproj` sets `PrecompiledHeader` — `StdAfx.h` is an ordinary common-include header
  and `StdAfx.cpp` is a vestigial PCH-creator stub. `target_precompile_headers` in CMake is therefore
  a **new** mechanism, not a translation of an existing one. It is still the right vehicle for
  `Compat.h` (it force-includes into every TU), but the choice should be understood as additive.
  CLAUDE.md's Build section is inaccurate here and should be corrected separately.
- **`skak.cpp` is dead code excluded from both projects**, and `Tests/PerftRunner.cpp` is in the app
  project but *not* the test project. So the two targets have genuinely different engine source
  lists, and the glob needs two explicit exclusions to stay faithful. Without them a glob silently
  changes what each binary contains.
- **`defines.h` uses `uint8_t` but includes only `<array>`**, and `StdAfx.h` includes `defines.h`
  *before* its `<cstdint>`. MSVC's `<array>` pulls in `<cstdint>` transitively; libstdc++ is not
  obliged to. This is the single most likely first compile failure on GCC, and the fix is a one-line
  `#include <cstdint>` in `defines.h`.

The rest of #82's portability table verified exactly: 12 `#pragma warning` directives in 4 files, one
`__forceinline` (`Board.h:93`), one `_strnicmp` (`Config.cpp:25`), one `_WIN32_WINNT` (`StdAfx.h:3`).

## Design decisions

### Source list: glob, not a hand-maintained list

```cmake
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/StratEngine/*.cpp)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/Archived/")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/skak\\.cpp$")   # dead file, in neither .vcxproj
```

CLAUDE.md calls out that a `.cpp` missing from a project is a silent failure — it passes by not
running. A hand-maintained CMake list would create a third source list to keep in sync with two
`.vcxproj` files, reproducing exactly that failure mode. `CONFIGURE_DEPENDS` answers the standard
objection to globbing by re-running the glob when the directory contents change.

The residual risk is the mirror image and is accepted: a file added to CMake but not to `.vcxproj`
breaks the Windows build, which is caught by the local build rather than by CI. That window closes
at Phase 3.

### `Compat.h` reaches every TU through the PCH, not through includes

```cmake
target_precompile_headers(StratEngine PRIVATE
    ${CMAKE_SOURCE_DIR}/StratEngine/Compat.h    # MUST precede StdAfx.h
    ${CMAKE_SOURCE_DIR}/StratEngine/StdAfx.h)
```

CMake generates a `cmake_pch.hxx` that includes both and force-includes it into every translation
unit of the target. The macros are therefore defined before any header is parsed, regardless of
include order, and **no source file gains an include**.

The alternative of adding `#include "Compat.h"` to the 17 affected headers is more conventional and
self-documenting, but it is a 17-file diff on a deadline slice, and those headers are also compiled
by the authoritative `.vcxproj` — so a mistake there breaks the Windows build too.

Note this is an **additive** mechanism, not a reuse: the `.vcxproj` files configure no precompiled
header at all (see findings). The PCH is being introduced here because it is the cheapest reliable
force-include, and the compile-time benefit is incidental.

### Dependencies: `FetchContent`, consumed as include directories

Pinned at the versions CI already uses: spdlog `v1.16.0`, nlohmann/json `v3.12.0`, Catch2 `v3.13.0`.

Populate and add include directories rather than `add_subdirectory` with imported targets. This keeps
every existing `#include` line unchanged — including the 21 test files that say
`<catch_amalgamated.hpp>` — and configures no extra CMake projects. The idiomatic-targets version is
filed as #166.

### Two executables, engine sources compiled into each

This mirrors the `.vcxproj` arrangement exactly, where `StratChessEvolved` and `StratChessTests` each
compile the full engine source list. Compiling the engine once into a library is the obvious CMake
idiom and is precisely why it is #83's job: it changes link topology and owes an nps measurement.
Phase 0 deliberately preserves the current topology so it owes none.

### Linux and GCC/Clang only — no `if(MSVC)` branches

`.vcxproj` owns Windows for the whole of Phase 0. MSVC branches in `CMakeLists.txt` would be written
but never exercised by Phase 0 CI, and unverified compiler-conditional code is worse than absent
code: it looks supported and silently is not.

### Deferred: IPO

`INTERPROCEDURAL_OPTIMIZATION` is **not** enabled, departing from #82's flag table.

That table's stated purpose is avoiding an nps regression. The nps-relevant binary is the one that
actually plays, and in Phase 0 that is still the MSVC/`.vcxproj` build — CMake produces nothing that
ships. Enabling GCC LTO would put an untested link step on the Aug 1 critical path for no Phase 0
benefit. It gets wired when CMake becomes authoritative (Phase 3) or when #83 needs it to span the
library boundary, which is where the 5-15% figure in #83 actually applies.

### CI: Linux gates, Windows advisory

`build-and-test-result` is the single always-present required check for branch protection. It is
rewritten to require the new Linux job and to accept `skipped` from the demoted Windows job, while
still failing if Windows ran and failed. Branch protection needs no reconfiguration, and a real
MSVC-only regression still blocks when the nightly or `windows-ci`-labelled run catches it.

### Linux runs both Release and Debug

The Release/Debug asymmetry documented for the Windows job (issue #146) is compiler-independent in
substance: Debug keeps `assert()` and the `#ifndef NDEBUG` bitboard/mailbox tripwire in `Eval.cpp`
live, while Release is the only leg that runs the optimizer and builds what ships. Demoting Windows
to nightly would lose that coverage on every PR unless Linux picks it up. Two Linux legs at 1x bill
less than one Windows leg at 2x, so this is cheaper than the status quo and covers more.

## Files changed

### Added

- `StratEngine/Compat.h` — the single home for MSVC-specific constructs.
- `CMakeLists.txt` (repository root).

### Modified — 6 source files, all small

(`StdAfx.h` appears twice below; it carries two independent changes.)

| File | Change |
|---|---|
| `StratEngine/Board.h:93` | `__forceinline` → `STRAT_FORCEINLINE` |
| `StratEngine/Config.cpp:25` | `_strnicmp` → portable case-insensitive compare |
| `StratEngine/StdAfx.h:3-6` | `_WIN32_WINNT` guarded with `#if defined(_WIN32)` |
| `StratEngine/StdAfx.h:9-32` | `#pragma warning push/disable/pop` guarded with `#if defined(_MSC_VER)` |
| `StratEngine/MoveHelper.h:14,15,205` | same guard |
| `StratEngine/PieceHelper.h:14,15,126` | same guard |
| `StratEngine/SquareHelper.h:12,13,35` | same guard |

The SAL annotations are **not** edited — `Compat.h` defines them away. That is what keeps this at 6
files instead of 27, and it is a deliberate deferral, not an oversight: stripping them is a mechanical
cleanup with no deadline value.

Guarding the pragmas is required, not cosmetic: GCC warns on unknown pragmas under `-Wall`, so
leaving them unguarded would manufacture 12 warnings in the very build whose warning count we are
trying to measure.

### Modified — CI

- `.github/workflows/build-and-test.yml` — new `build-linux` job; Windows job moved to `schedule` plus
  a `windows-ci` label; `build-and-test-result` gate logic updated.

### Optional

- `StratChessEvolved/Scripts/Get-ChangeTier.ps1` — explicit `Build` rule for `CMakeLists.txt` and
  `*.cmake`, plus a self-test case. Behaviour is already correct via fail-closed; this makes the
  intent explicit rather than incidental.

## `Compat.h` sketch

```cpp
#pragma once

// Constructs MSVC provides that GCC/Clang do not. This is the only place in the
// codebase where a compiler is named.
#if defined(_MSC_VER)
#  include <sal.h>
#  define STRAT_FORCEINLINE __forceinline
#else
   // SAL source-annotation macros expand to nothing off MSVC. The annotations
   // are kept in place because they document intent and MSVC still consumes them.
   // These three are the complete set used by this codebase — no _opt_ or
   // _reads_/_writes_ variants appear anywhere.
#  define _In_
#  define _Inout_
#  define _Out_
#  define STRAT_FORCEINLINE inline __attribute__((always_inline))
#endif
```

The three spellings above are exhaustive as of `17869ee`, verified by enumerating every `_Xxx_`-shaped
token in the tree. If a future annotation introduces a fourth spelling it will fail to compile off
MSVC, which is the correct failure mode — noisy, not silent.

## Validation plan

Phase 0's proof is the CI run itself, and that cannot happen before the quota resets on 2026-08-01.
What is verifiable before then:

1. **`.vcxproj` build stays green** — `.\build.ps1 all -Config Release` and `-Config Debug`. This is
   the load-bearing local check: it proves the 6 source edits did not break the authoritative build.
2. **Fast tier stays green on Windows** — `.\build.ps1 run-tests`, same test count as before.
3. **`Validate-PrePR.ps1`** — the change touches `StratEngine/*.h` and `*.cpp`, so this classifies as
   the `Engine` tier and runs the full local set.
4. **CMake configure is syntactically sound** — best-effort only. There is no Linux machine in this
   environment, so a genuine Linux build cannot be run locally. Do not report the CMake path as
   verified on the strength of a configure step.

After the quota resets:

5. **Both Linux legs green**, `~[slow]` test count matching the Windows legs.
6. **Record the GCC `-Wall -Wextra` warning count** and file the `-Werror` follow-up sized by it.
7. **Confirm billed minutes** actually dropped, which is the entire point of the exercise.

## Invariants after this change

- `.vcxproj`, `.sln` and `build.ps1` are unchanged in behaviour; the Windows workflow is unaffected.
- No `#pragma warning(disable)` is added to any source file (CLAUDE.md).
- `StratEngine/Archived/` is still not built, by either build system.
- The fast-tier test count is identical on Windows and Linux.
- `Compat.h` is the only file where a compiler is named conditionally.
- `Move` remains 2 bytes, and no engine behaviour changes — this slice compiles the same code with a
  second toolchain and must not alter search or evaluation results.

## Risks, ordered

1. **Unknown GCC warning count.** This code has only ever been `/W4 /WX`-clean against one compiler.
   Mitigated by shipping without `-Werror`; the count becomes a follow-up issue rather than a blocker.
2. **The quota is down until 2026-08-01.** No design choice fixes this; the Linux job cannot be proven
   green before then. The work can be complete and merged-ready without being verified.
3. **`std::format` requires GCC ≥ 13.** Eight uses in the tree. `ubuntu-latest` (24.04) ships GCC 13,
   so this should hold, but it is the most likely hard compile failure and the first thing to check if
   the build fails to compile at all.
4. **Dual source-of-truth window.** A `.cpp` present in CMake but absent from `.vcxproj` breaks
   Windows only, caught locally rather than by CI. Accepted; closes at Phase 3.
5. **`GLOB_RECURSE` picks up files a human would not have listed.** The `Archived/` filter is the known
   case; any future directory of non-built sources needs the same treatment, and nothing enforces it.

## Sequencing after this lands

Per #81: Phase 1 is #83 (static library), #92 (ccache on Linux), the `-Werror` tightening sized by
finding 6, and sanitizers. #166 (dependencies as CMake packages) also unblocks here. #84's clang-cl
bake-off is independent and needs no CMake — it can run in parallel at any point.
