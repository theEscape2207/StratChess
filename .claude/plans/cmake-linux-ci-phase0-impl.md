# CMake + Linux CI Phase 0 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make StratChessEvolved build and run its fast test tier on Linux under CMake + Ninja, so CI
validates every PR at 1x billing instead of 2x on Windows.

**Architecture:** A new `StratEngine/Compat.h` becomes the single place any compiler is named, reaching
every translation unit as the first entry in `target_precompile_headers`. A root `CMakeLists.txt`
globs the engine sources and defines two executables that mirror the existing `.vcxproj` targets
exactly. `.github/workflows/build-and-test.yml` gains a Linux job and demotes the Windows job to
nightly plus an on-demand label. The `.vcxproj`/`.sln`/`build.ps1` path is untouched and remains
authoritative for local Windows work.

**Tech Stack:** C++20, CMake ≥ 3.24, Ninja, GCC 13 (ubuntu-latest), Catch2 v3.13.0 (amalgamated),
spdlog v1.16.0, nlohmann/json v3.12.0, all via `FetchContent`.

**Design spec:** `.claude/plans/cmake-linux-ci-phase0.md` — read it before starting. It records why
each decision was taken and which parts of issue #82 it deliberately departs from.

## Global Constraints

- **The Windows build must not change behaviour.** `.vcxproj`, `.sln` and `build.ps1` are edited by no
  task in this plan. Every task that touches a source file must leave `.\build.ps1 all` green.
- **No `#pragma warning(disable)` may be added** to any source file (CLAUDE.md). Existing ones are
  guarded, never extended.
- **No `-Werror`** in this slice. `-Wall -Wextra` only; the warning count is an output of this work,
  not a gate on it.
- **No `if(MSVC)` branches** in `CMakeLists.txt`. Phase 0 targets GCC/Clang on Linux only.
- **No `INTERPROCEDURAL_OPTIMIZATION`.** Deliberate departure from #82's flag table — see spec.
- **`StratEngine/Archived/` must never be compiled** by either build system.
- **No engine behaviour may change.** This slice compiles the same code with a second toolchain.
  Search and evaluation results must be identical; `Move` stays 2 bytes.
- **C++ standard:** `cxx_std_20`, `CXX_EXTENSIONS OFF` (matches `stdcpp20` + `ConformanceMode true`).
- **Architecture flags:** `-mavx2 -mbmi2` (matches `AdvancedVectorExtensions2`; `_pext_u64` in
  `Magic.h` requires BMI2).
- **Commit messages stay short** (CLAUDE.md); detail belongs in the PR body.
- **Do not bypass the pre-commit hook.** A first commit in a fresh worktree rebuilds from scratch —
  allow 5-10 minutes, not the 2-minute default, or the commit is killed mid-build.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `StratEngine/Compat.h` | **Create.** The only file that names a compiler conditionally. SAL macros, `STRAT_FORCEINLINE`. | 1 |
| `StratEngine/defines.h` | **Modify.** Add `#include <cstdint>` — it uses `uint8_t` but includes only `<array>`. | 1 |
| `StratEngine/Board.h:93` | **Modify.** `__forceinline` → `STRAT_FORCEINLINE`. | 1 |
| `StratEngine/Config.cpp:25` | **Modify.** `_strnicmp` → portable helper. | 1 |
| `StratEngine/StdAfx.h` | **Modify.** Guard `_WIN32_WINNT` and the warning pragmas. | 1 |
| `StratEngine/MoveHelper.h` | **Modify.** Guard pragmas at lines 14, 15, 205. | 1 |
| `StratEngine/PieceHelper.h` | **Modify.** Guard pragmas at lines 14, 15, 126. | 1 |
| `StratEngine/SquareHelper.h` | **Modify.** Guard pragmas at lines 12, 13, 35. | 1 |
| `CMakeLists.txt` | **Create.** Root build definition: deps, two executables, flags. | 2 |
| `.gitignore` | **Modify.** Ignore `build/` and `compile_commands.json`. | 2 |
| `.github/workflows/build-and-test.yml` | **Modify.** Add `build-linux`; demote Windows; rewrite the gate. | 3 |
| `StratChessEvolved/Scripts/Get-ChangeTier.ps1` | **Modify.** Explicit `Build` rule for CMake files + self-test cases. | 4 |

Task 1 is source-only and verifiable on Windows today. Task 2 is build-definition-only. Task 3 is
CI-only. Task 4 is independent and could be dropped without affecting the outcome.

---

### Task 1: Centralise MSVC-specific constructs behind `Compat.h`

Makes the source tree compilable by a non-MSVC compiler, without changing what MSVC sees. This task
delivers no CMake — its whole deliverable is "the Windows build still passes, and the MSVC-isms now
live in one place."

**Files:**
- Create: `StratEngine/Compat.h`
- Modify: `StratEngine/defines.h:3`
- Modify: `StratEngine/Board.h:93`
- Modify: `StratEngine/Config.cpp:4-25`
- Modify: `StratEngine/StdAfx.h:3-10`, `:32`
- Modify: `StratEngine/MoveHelper.h:13-15`, `:205`
- Modify: `StratEngine/PieceHelper.h:13-15`, `:126`
- Modify: `StratEngine/SquareHelper.h:11-13`, `:35`
- Test: no new test file. Verification is the existing Windows build + fast tier.

**Interfaces:**
- Consumes: nothing.
- Produces: `STRAT_FORCEINLINE` (function decorator macro) and no-op definitions of `_In_`, `_Inout_`,
  `_Out_` when `_MSC_VER` is undefined. Task 2 relies on `StratEngine/Compat.h` existing at that exact
  path and being safe to list first in `target_precompile_headers`.

**Why these edits and no others:** the 152 SAL annotations are *not* removed. `Compat.h` defines them
away off-MSVC, which keeps this task at 8 files instead of 33. Stripping them is mechanical cleanup
with no deadline value.

- [ ] **Step 1: Create `StratEngine/Compat.h`**

The three SAL spellings below are exhaustive as of `17869ee`, verified by enumerating every
`_Xxx_`-shaped token in the tree. No `_opt_`, `_reads_` or `_writes_` variants exist.

```cpp
#pragma once

// Constructs MSVC provides that GCC and Clang do not. This is the only place in
// the codebase where a compiler is named conditionally; everything else stays
// compiler-neutral and relies on these definitions.

#if defined(_MSC_VER)

#  include <sal.h>
#  define STRAT_FORCEINLINE __forceinline

#else

   // SAL source-annotation macros expand to nothing off MSVC. The annotations are
   // kept in the sources because they document parameter direction and MSVC's
   // analyser still consumes them.
#  define _In_
#  define _Inout_
#  define _Out_

#  define STRAT_FORCEINLINE inline __attribute__((always_inline))

#endif
```

- [ ] **Step 2: Add the missing `<cstdint>` to `defines.h`**

`defines.h` declares `enum eColor : uint8_t` but includes only `<array>`. MSVC's `<array>` drags in
`<cstdint>` transitively; libstdc++ is not obliged to. `StdAfx.h` includes `defines.h` *before* its
own `<cstdint>`, so the transitive include is the only thing making this work today.

In `StratEngine/defines.h`, replace line 3:

```cpp
#include <array>
```

with:

```cpp
#include <array>
#include <cstdint>
```

Keep the file's existing BOM and leading `#pragma once` untouched.

This is the one cleanup-shaped edit kept in this plan, because it is a plausible hard compile failure
rather than tidiness. Sweeping the rest of the tree for the same pattern is #167 item 5.

- [ ] **Step 3: Switch `Board.h` to `STRAT_FORCEINLINE`**

In `StratEngine/Board.h`, change line 93 from:

```cpp
	static __forceinline eSquare GetFirstPiece(BITBOARD mask) noexcept
```

to:

```cpp
	static STRAT_FORCEINLINE eSquare GetFirstPiece(BITBOARD mask) noexcept
```

`Board.h` does not include `StdAfx.h`, and must not start doing so. The macro arrives via the PCH
force-include under CMake, and via `sal.h`-free plain MSVC under `.vcxproj` — where
`STRAT_FORCEINLINE` is only defined once `Compat.h` is reachable. To keep the `.vcxproj` build working
without a PCH, add the include directly to `Board.h`, immediately after its `#pragma once`:

```cpp
#include "Compat.h"
```

- [ ] **Step 4: Replace `_strnicmp` in `Config.cpp`**

`_strnicmp` is MSVC-only. POSIX offers `strncasecmp`, but rather than branch on platform at the call
site, use a small standard-C++ helper.

In `StratEngine/Config.cpp`, add after the existing includes (after line 7, `#include "Game.h"`):

```cpp
namespace {

// Case-insensitive comparison of the first n characters. Replaces MSVC's
// _strnicmp, which has no portable equivalent in the standard library.
bool iequals_n(std::string_view a, std::string_view b, std::size_t n)
{
	if (a.size() < n || b.size() < n) {
		return false;
	}
	for (std::size_t i = 0; i < n; ++i) {
		const auto ca = static_cast<unsigned char>(a[i]);
		const auto cb = static_cast<unsigned char>(b[i]);
		if (std::tolower(ca) != std::tolower(cb)) {
			return false;
		}
	}
	return true;
}

} // namespace
```

Then change line 25 from:

```cpp
	if (0 == _strnicmp(setupType.c_str(), FENKey.c_str(), 3))
```

to:

```cpp
	if (iequals_n(setupType, FENKey, 3))
```

Note the inverted sense: `_strnicmp` returns 0 on match, `iequals_n` returns `true` on match. Getting
this backwards silently flips FEN detection, so re-read the line after editing.

`Config.cpp` includes `StdAfx.h`, which provides `<string>`. Add `#include <cctype>` and
`#include <string_view>` to `StratEngine/StdAfx.h`'s alphabetically-sorted STL block — `<cctype>`
before `<cstdint>`, and `<string_view>` after `<string>` — per CLAUDE.md's rule that frequently-used
STL headers belong in `StdAfx.h`, not individual `.cpp` files.

- [ ] **Step 5: Guard `_WIN32_WINNT` and the pragmas in `StdAfx.h`**

In `StratEngine/StdAfx.h`, replace lines 1-10:

```cpp
#pragma once

#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0600
#endif

#include "defines.h"

#pragma warning (push)
#pragma warning (disable :4505 4530)
```

with:

```cpp
#pragma once

#include "Compat.h"

#if defined(_WIN32)
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#endif

#include "defines.h"

#if defined(_MSC_VER)
#  pragma warning (push)
#  pragma warning (disable :4505 4530)
#endif
```

And replace line 32 (`#pragma warning (pop)`) with:

```cpp
#if defined(_MSC_VER)
#  pragma warning (pop)
#endif
```

Guarding is required, not cosmetic: GCC warns on unknown pragmas under `-Wall`, so leaving these
unguarded would manufacture warnings in the very build whose warning count Task 3 exists to measure.

- [ ] **Step 6: Guard the pragmas in the three helper headers**

Identical treatment in each. In `StratEngine/MoveHelper.h`, replace lines 13-15:

```cpp
// remove annoying level 4 warnings
#pragma warning(push)
#pragma warning( disable : 4505 )	// Unreferenced local function has been removed
```

with:

```cpp
// remove annoying level 4 warnings
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning( disable : 4505 )	// Unreferenced local function has been removed
#endif
```

and line 205 (`#pragma warning (pop)`) with:

```cpp
#if defined(_MSC_VER)
#  pragma warning (pop)
#endif
```

Apply exactly the same two edits to `StratEngine/PieceHelper.h` (lines 13-15 and 126) and
`StratEngine/SquareHelper.h` (lines 11-13 and 35).

`PieceHelper.h`, `SquareHelper.h` and `defines.h` begin with a UTF-8 BOM. Preserve it — stripping it
produces a large spurious diff.

- [ ] **Step 7: Rebuild both configurations on Windows**

This is the verification for the whole task: the edits must be invisible to MSVC.

```powershell
.\build.ps1 all -Config Release
.\build.ps1 all -Config Debug
```

Expected: both succeed with zero warnings. Level4 + `/WX` is on, so any new warning is a build error —
if either fails, the guard placement is wrong. A likely mistake is guarding the `push` but not the
matching `pop`, which produces an unbalanced-pragma error.

- [ ] **Step 8: Run the fast tier**

```powershell
.\build.ps1 run-tests
```

Expected: PASS, with the **same test count as before the change**. Record the count — Task 3 compares
the Linux count against it. If the count differs, a source file has been dropped from the build.

- [ ] **Step 9: Commit**

```bash
git add StratEngine/Compat.h StratEngine/defines.h StratEngine/Board.h \
        StratEngine/Config.cpp StratEngine/StdAfx.h StratEngine/MoveHelper.h \
        StratEngine/PieceHelper.h StratEngine/SquareHelper.h
git commit -m "Move MSVC-specific constructs behind Compat.h"
```

Allow a 5-10 minute timeout: the pre-commit hook runs `Validate-PreCommit.ps1`, which rebuilds.

---

### Task 2: Add the root `CMakeLists.txt`

**Files:**
- Create: `CMakeLists.txt` (repository root)
- Modify: `.gitignore`
- Test: no new test file. Verification is a configure run plus a Windows re-verify.

**Interfaces:**
- Consumes: `StratEngine/Compat.h` from Task 1, at that exact path.
- Produces: two targets, `StratChessEvolved` and `StratChessTests`, and a `compile_commands.json`.
  Task 3 invokes `cmake --build build --target StratChessTests` and runs
  `./build/StratChessTests '~[slow]'`.

**Source-list traps this task must handle** — both verified against the `.vcxproj` files, and both
silent if got wrong:

- `StratEngine/skak.cpp` is **entirely commented out** and appears in neither project. A bare glob
  pulls it in.
- `StratEngine/Tests/PerftRunner.cpp` is in the **app project only**, not the test project. The two
  targets genuinely have different engine source lists.

This task **works around** both rather than fixing them — preserving today's binary contents exactly
is what keeps Phase 0 free of any behavioural claim. Removing the underlying causes is #167.

- [ ] **Step 1: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(StratChess CXX)

# Phase 0 scope: Linux + GCC/Clang only. The .vcxproj/.sln build remains
# authoritative for Windows, so there are deliberately no if(MSVC) branches here
# -- compiler-conditional code that CI never exercises rots silently.
# See .claude/plans/cmake-linux-ci-phase0.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # matches MSVC ConformanceMode=true

# Consumed by clangd, VS Code, Zed and coding agents. Costs nothing to emit.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Pinned to the versions the Windows CI job already uses. Populated and consumed
# as include directories rather than via add_subdirectory: all three are
# header-only in this project, and Catch2 is used through its amalgamated
# distribution, so every existing #include line stays valid. Switching to real
# imported targets is issue #166.
# ---------------------------------------------------------------------------
include(FetchContent)

# SOURCE_SUBDIR points at a directory that does not exist in any of these repos.
# That makes FetchContent_MakeAvailable download and populate the sources but
# skip add_subdirectory(), because there is no CMakeLists.txt at that path -- so
# none of the dependencies' own CMake projects are configured, and no extra
# targets appear. This is the supported way to get populate-only behaviour:
# the bare FetchContent_Populate(name) form is deprecated as of CMake 3.30
# (policy CMP0169) and warns on the CMake that ubuntu-latest ships.
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG        v1.16.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)

FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2
    GIT_TAG        v3.13.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)

FetchContent_MakeAvailable(spdlog nlohmann_json catch2)

# ---------------------------------------------------------------------------
# Engine sources
#
# Globbed rather than listed: a hand-maintained list here would be a third copy
# to keep in sync with two .vcxproj files, and a .cpp missing from a project is
# a silent failure -- it passes by not running. CONFIGURE_DEPENDS re-runs the
# glob when the directory contents change.
#
# Two exclusions, both matching the .vcxproj files exactly:
#   Archived/       -- retired algorithms, kept for reference, never built.
#   skak.cpp        -- dead file, entirely commented out, in neither project.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/StratEngine/*.cpp)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/Archived/")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/skak\\.cpp$")

# PerftRunner.cpp is in StratChessEvolved.vcxproj but NOT StratChessTests.vcxproj.
# Preserving that asymmetry keeps each binary's contents identical to today's.
set(ENGINE_SOURCES_TESTS ${ENGINE_SOURCES})
list(FILTER ENGINE_SOURCES_TESTS EXCLUDE REGEX "/PerftRunner\\.cpp$")

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/StratChessTests/*.cpp)

# ---------------------------------------------------------------------------
# Shared compile settings
#
# -mavx2 -mbmi2 matches AdvancedVectorExtensions2; Magic.h's _pext_u64 needs BMI2.
# -Werror is deliberately absent: this code has only ever been /W4 /WX-clean
# against MSVC, and the GCC warning count is unknown until it first compiles.
# Tightening is a follow-up sized by the actual count.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)   # std::jthread — Lazy SMP helpers in AIPerplex::GetMove

function(strat_configure_target tgt)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -mavx2 -mbmi2)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_SOURCE_DIR}/StratEngine
        ${spdlog_SOURCE_DIR}/include
        ${nlohmann_json_SOURCE_DIR}/include)
    # Compat.h MUST precede StdAfx.h: CMake force-includes the generated header
    # into every TU, which is how the SAL and STRAT_FORCEINLINE definitions reach
    # the 17 headers that never include StdAfx.h themselves.
    target_precompile_headers(${tgt} PRIVATE
        ${CMAKE_SOURCE_DIR}/StratEngine/Compat.h
        ${CMAKE_SOURCE_DIR}/StratEngine/StdAfx.h)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
endfunction()

# ---------------------------------------------------------------------------
# Targets
#
# The engine is compiled into both executables, mirroring the .vcxproj layout.
# Extracting it as a static library is issue #83: it changes link topology and
# owes its own nps measurement, so Phase 0 deliberately leaves topology alone.
#
# INTERPROCEDURAL_OPTIMIZATION is deliberately NOT set. The nps-relevant binary
# is still the MSVC one; CMake ships nothing yet. See the spec.
# ---------------------------------------------------------------------------
add_executable(StratChessEvolved
    ${ENGINE_SOURCES}
    ${CMAKE_SOURCE_DIR}/StratChessEvolved/StratChessEvolved.cpp)
strat_configure_target(StratChessEvolved)

add_executable(StratChessTests
    ${ENGINE_SOURCES_TESTS}
    ${TEST_SOURCES}
    ${catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp)
strat_configure_target(StratChessTests)
target_compile_definitions(StratChessTests PRIVATE STRAT_ENABLE_TEST_ACCESS)
target_include_directories(StratChessTests PRIVATE ${catch2_SOURCE_DIR}/extras)
```

- [ ] **Step 2: Ignore CMake build output**

Append to `.gitignore`:

```gitignore

# CMake (Phase 0 Linux build — see .claude/plans/cmake-linux-ci-phase0.md)
build/
compile_commands.json
```

- [ ] **Step 3: Verify the source lists resolve as intended**

This is the step that catches the two traps. Configure is unavailable on Windows for this
Linux-only `CMakeLists.txt`, so check the lists directly instead of assuming:

```bash
# Should list 24 files: 25 non-Archived .cpp minus skak.cpp
find StratEngine -name '*.cpp' -not -path '*/Archived/*' ! -name 'skak.cpp' | sort | wc -l

# Should list 23: the above minus PerftRunner.cpp
find StratEngine -name '*.cpp' -not -path '*/Archived/*' \
     ! -name 'skak.cpp' ! -name 'PerftRunner.cpp' | sort | wc -l
```

Expected: `24` then `23`. Cross-check both against the `ClCompile` entries:

```bash
grep -o 'ClCompile Include="[^"]*"' StratChessEvolved/StratChessEvolved.vcxproj | wc -l   # 26
grep -o 'ClCompile Include="[^"]*"' StratChessTests/StratChessTests.vcxproj   | wc -l   # 46
```

The app's 26 = 24 engine + 1 app main + 1 (`UCIHandler.cpp`, already inside the 24 — recount by
listing, not arithmetic, if these disagree). Treat any mismatch as a real finding and reconcile
against the `.vcxproj` before proceeding; a silently different source list is exactly the failure
mode the glob was chosen to avoid.

- [ ] **Step 4: Confirm the Windows build is still untouched**

`CMakeLists.txt` is additive, but confirm nothing regressed:

```powershell
.\build.ps1 run-tests
```

Expected: PASS, same test count recorded in Task 1 Step 8.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt .gitignore
git commit -m "Add CMake build for Linux"
```

---

### Task 3: Add the Linux CI job and demote Windows

**Files:**
- Modify: `.github/workflows/build-and-test.yml`
- Test: the workflow is its own test; it cannot be verified until the quota resets on 2026-08-01.

**Interfaces:**
- Consumes: the `StratChessTests` target from Task 2, built into `build/`.
- Produces: a required check named `build-and-test-result` — the same name branch protection already
  requires, so no branch-protection reconfiguration is needed.

**Context:** `build-and-test-result` is the single always-present required check. It must keep that
name and keep succeeding when a job is legitimately skipped, or PRs become unmergeable.

- [ ] **Step 1: Change the workflow triggers**

Replace the `on:` block at the top of `.github/workflows/build-and-test.yml`:

```yaml
on:
  pull_request:
    branches: [main]
  push:
    branches: [main]
```

with:

```yaml
on:
  pull_request:
    branches: [main]
  push:
    branches: [main]
  # Windows is demoted to nightly. It bills at 2x per minute against included
  # minutes where Linux bills at 1x, and that multiplier is what exhausted the
  # July quota (see issue #81). Every PR now runs Linux; Windows runs nightly,
  # or on demand via the 'windows-ci' label.
  schedule:
    - cron: '0 3 * * *'
```

- [ ] **Step 2: Add the `build-linux` job**

Insert immediately after the `classify` job, before `build-and-test`:

```yaml
  build-linux:
    needs: classify
    if: github.event_name == 'push' || github.event_name == 'schedule' || needs.classify.outputs.is_full == 'true'
    runs-on: ubuntu-latest
    # Release and Debug are not redundant -- the same asymmetry documented for the
    # Windows job in issue #146 is compiler-independent. Debug keeps assert() and
    # the #ifndef NDEBUG bitboard/mailbox tripwire in Eval.cpp live; Release is the
    # only leg that runs the optimizer. Two Linux legs at 1x still bill less than
    # one Windows leg at 2x.
    strategy:
      fail-fast: false
      matrix:
        config: [Release, Debug]
    steps:
      - uses: actions/checkout@v4

      - name: Install Ninja
        run: sudo apt-get update && sudo apt-get install -y ninja-build

      # Keyed on the pinned dependency tags, matching the Windows job's approach.
      # Not matrix-scoped: FetchContent sources have no per-configuration content,
      # so both legs want the identical cache.
      - name: Cache FetchContent dependencies
        uses: actions/cache@v4
        with:
          path: build/_deps
          key: cmake-deps-spdlog-v1.16.0-json-v3.12.0-catch2-v3.13.0

      - name: Configure
        run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=${{ matrix.config }}

      - name: Build
        run: cmake --build build --target StratChessTests --parallel

      # Same '~[slow]' filter as the Windows job, deliberately: the legs must
      # cover the same cases. The fast tier is self-contained -- no Catch2 test
      # reads Tests/*.json -- so the working directory does not matter here.
      - name: Run fast tests
        run: ./build/StratChessTests '~[slow]'
```

- [ ] **Step 3: Gate the Windows job**

Change the `build-and-test` job's `if:` from:

```yaml
    if: github.event_name == 'push' || needs.classify.outputs.is_full == 'true'
```

to:

```yaml
    # Nightly, or on demand via the 'windows-ci' label. Pushes to main still run
    # so the actions/cache deps cache on the default branch does not lapse.
    if: >-
      github.event_name == 'schedule'
      || github.event_name == 'push'
      || contains(github.event.pull_request.labels.*.name, 'windows-ci')
```

Leave the rest of that job — the matrix, the deps steps, the `build.ps1` invocation — exactly as it is.

- [ ] **Step 4: Rewrite the result gate**

Replace the whole `build-and-test-result` job with:

```yaml
  # Branch protection requires a single always-present check, and this is it --
  # the name must not change. Linux is the gate: it runs on every PR, so a PR is
  # mergeable on Linux alone, which is the entire point of the budget fix.
  # Windows is advisory in the sense that being SKIPPED is fine, but a Windows
  # job that actually ran and failed still blocks -- an MSVC-only regression
  # (e.g. a Release-only C4189 under /WX) must not merge unnoticed.
  build-and-test-result:
    needs: [classify, build-linux, build-and-test]
    if: always()
    runs-on: ubuntu-latest
    steps:
      - name: Report
        shell: bash
        run: |
          echo "tier=${{ needs.classify.outputs.tier }}"
          echo "linux result=${{ needs.build-linux.result }}"
          echo "windows result=${{ needs.build-and-test.result }}"

          case "${{ needs.build-linux.result }}" in
            success) echo "Linux build and tests passed." ;;
            skipped) echo "Linux skipped: '${{ needs.classify.outputs.tier }}' tier cannot affect the build." ;;
            *)       echo "Linux build and tests did not pass."; exit 1 ;;
          esac

          case "${{ needs.build-and-test.result }}" in
            success) echo "Windows build and tests passed." ;;
            skipped) echo "Windows skipped: demoted to nightly / 'windows-ci' label." ;;
            *)       echo "Windows build and tests did not pass."; exit 1 ;;
          esac

          exit 0
```

- [ ] **Step 5: Validate the YAML parses**

A syntax error here is only discovered by pushing, which costs a run.

```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/build-and-test.yml')); print('YAML OK')"
```

Expected: `YAML OK`.

Then confirm the gate job still has its required name and that both job names it depends on exist:

```bash
grep -nE '^  (build-linux|build-and-test|build-and-test-result|classify):' .github/workflows/build-and-test.yml
```

Expected: all four, in that order after `classify`.

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/build-and-test.yml
git commit -m "Add Linux CI job, demote Windows to nightly"
```

---

### Task 4: Classify CMake files as the `Build` tier

Optional and independent. `Get-ChangeTier.ps1` already fails closed to `Engine` for unrecognised
paths, so `CMakeLists.txt` currently gets *stricter* validation than it needs. This makes the intent
explicit rather than incidental — and unlike every other task here, it has a real test.

**Files:**
- Modify: `StratChessEvolved/Scripts/Get-ChangeTier.ps1`
- Test: the script's own `-SelfTest` mode.

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: nothing later tasks rely on.

- [ ] **Step 1: Add the failing self-test cases**

In `StratChessEvolved/Scripts/Get-ChangeTier.ps1`, add to the `$cases` array (after the
`'vcxproj -> Build'` case, keeping the existing formatting style):

```powershell
        @{ Name = 'CMakeLists -> Build';        Files = @('CMakeLists.txt');                                     Expect = 'Build' }
        @{ Name = 'cmake module -> Build';      Files = @('cmake/Toolchain.cmake');                              Expect = 'Build' }
```

- [ ] **Step 2: Run the self-test to verify it fails**

```powershell
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Get-ChangeTier.ps1 -SelfTest
```

Expected: FAIL on both new cases, each reporting `Expect = Build` but got `Engine` — because the
fail-closed default currently catches them.

- [ ] **Step 3: Add the classification rule**

In `Get-TierForPath`, add immediately after the existing `*.props`/`*.sln` rule (currently line 86):

```powershell
    if ($p -like 'CMakeLists.txt' -or $p -like '*/CMakeLists.txt') { return 'Build' }
    if ($p -like '*.cmake')                                        { return 'Build' }
```

- [ ] **Step 4: Run the self-test to verify it passes**

```powershell
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Get-ChangeTier.ps1 -SelfTest
```

Expected: all cases PASS, including the two `FAIL CLOSED:` cases, which must still expect `Engine`.
If a fail-closed case broke, the new rule is too broad.

- [ ] **Step 5: Commit**

```bash
git add StratChessEvolved/Scripts/Get-ChangeTier.ps1
git commit -m "Classify CMake files as Build tier"
```

---

## Verification before opening the PR

1. `.\build.ps1 all -Config Release` and `-Config Debug` — both green, zero warnings.
2. `.\build.ps1 run-tests` — same test count as recorded in Task 1 Step 8.
3. `Validate-PrePR.ps1` — the diff touches `StratEngine/*.h` and `*.cpp`, so this is the `Engine`
   tier and runs the full local set.
4. `git diff --name-only origin/main...HEAD` — confirm no `.vcxproj`, `.sln` or `build.ps1` appears.

**Reviewer dispatch:** the diff touches no `Eval.cpp`, `AIPerplex.cpp/.h`, `ThreadData.h` or
`Sort.cpp/.h`, so neither `eval-reviewer` nor `search-reviewer` is triggered by CLAUDE.md's checklist.
This is not the logging-only self-certification carve-out; it is simply out of both reviewers' scope.
State that reasoning in the PR body so the skip is auditable.

**What cannot be verified locally:** there is no Linux machine in this environment, and the Actions
quota is exhausted until 2026-08-01. The CMake path is therefore *unproven* at PR time. Say so
plainly in the PR body rather than implying the Linux job is known-green — the first real run is the
first evidence.

## After the first successful Linux run

- Record the GCC `-Wall -Wextra` warning count and open the `-Werror` follow-up sized by it.
- Confirm the Linux `~[slow]` test count matches the Windows count.
- Confirm billed minutes actually dropped — that is the outcome this whole slice exists for.

Cleanup deliberately **not** in this plan is tracked in **#167** (`low-hanging-fruit`): deleting
`skak.cpp`, resolving the `PerftRunner.cpp` asymmetry, correcting CLAUDE.md's inaccurate "`StdAfx.h`
is the PCH" claim, and deciding the fate of the 152 SAL annotations. This plan works around each of
those rather than fixing them, so none of it is a prerequisite.
