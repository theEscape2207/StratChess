# Build cleanup and `-Werror` on Linux (#167 partial, #169)

## Goal

Remove dead code that the CMake port had to work around, and close the warning gate on the Linux
build so it matches the `/WX` gate MSBuild has always had.

Forked from `origin/main` at `9f45233` (PR #168, CMake + Linux CI Phase 0).

## What investigation established

Both issues asked questions their authors could not answer. Answers found before implementing:

### `MoveHelper::IsValid` is NOT dead — it is assert-only

`#169` offered "delete it if genuinely dead" as the first option. It is not dead: `Board.cpp:301` and
`Board.cpp:473` both call it inside `assert()`.

That also explains the otherwise-odd warning counts exactly. `MoveHelper.h` is included by ~32
translation units; `IsValid` is a `static` free function in namespace `MoveHelper`, so it has internal
linkage in each one. Only `Board.cpp` uses it, and only when `assert()` is live:

| | TUs warning | Total |
|---|---|---|
| Release | all 32 (`NDEBUG` compiles the asserts out) | 32 |
| Debug | 31 (Board.cpp uses it) | 31 |

So the correct fix is `[[maybe_unused]]` — which is exactly the suppression CLAUDE.md already
sanctions for this situation ("`[[maybe_unused]]` for params used only in `assert()`"). Deleting the
function would delete a live invariant check; `-Wno-unused-function` would blanket-suppress a
diagnostic worth keeping.

### `Tests/PerftRunner.cpp` is a dead duplicate, not a deliberate asymmetry

`#167` asked whether the app-only/test-excluded split was intent or drift. It is neither — the file is
simply superseded:

- `perftrunner_main` has **no header declaration and no callers** anywhere in the tree.
- `StratChessEvolved.cpp:162` defines its own `static int perftrunner(int, char**)` handling the same
  four commands (`test`, `run`, `divide`, `detailed`) **plus** a FEN argument the old one lacks.
- Its `print_usage()` is non-`static`, so it occupies the global namespace for nothing.

Deleting it resolves the asymmetry by removing one side of it.

### `skak.cpp` is entirely commented out

Every line, including its `main()`. In neither `.vcxproj`. Only reason it needed a CMake glob
exclusion.

## Scope

### In

1. Delete `StratEngine/skak.cpp` and `StratEngine/Tests/PerftRunner.cpp`.
2. Drop both now-unnecessary `list(FILTER ...)` exclusions from `CMakeLists.txt`, and the second
   source list they existed to produce.
3. Deregister both files from `StratChessEvolved.vcxproj` / `.filters`.
4. Mark `MoveHelper::IsValid` `[[maybe_unused]]`, and delete the `#pragma warning(disable: 4505)`
   block it made necessary in `MoveHelper.h`.
5. Add `-Werror` to `strat_configure_target` in `CMakeLists.txt`.
6. Correct CLAUDE.md's claim that `StdAfx.h` is a precompiled header.

### Out, with reasons

- **The SAL annotations (#167 item 4).** A 27-file change turning on a real design question — whether
  to strip them or earn them with an MSVC `/analyze` job. It deserves its own PR and its own
  argument; bundled here it would drown this diff in mechanical noise. #167 stays open, narrowed to
  exactly that question.
- **The blanket `<cstdint>` sweep (#167 item 5).** 23 files use fixed-width types without including
  `<cstdint>`, but all are satisfied through `defines.h`, which includes it — this is robust, not
  fragile. The genuinely fragile cases (`<filesystem>`, `<iomanip>`, `<cmath>`, `<cstdlib>`) were
  already fixed in PR #168. #167 item 5 itself says this belongs to `include-what-you-use` or
  `clang-tidy misc-include-cleaner` once #84 lands; hand-editing 23 files now would pre-empt the tool
  and change no behaviour.
- **`PieceHelper.h` / `SquareHelper.h` 4505 pragmas.** GCC does not currently warn about anything in
  them, so there is no evidence their suppressions are removable. Removing a guard on the strength of
  "probably fine" is how `/WX` builds break. Leave until something demonstrates they are dead.

## Files changed

| File | Change |
|---|---|
| `StratEngine/skak.cpp` | **delete** |
| `StratEngine/Tests/PerftRunner.cpp` | **delete** |
| `StratEngine/MoveHelper.h` | `[[maybe_unused]]` on `IsValid`; remove the 4505 `#if defined(_MSC_VER)` block and its `pop` |
| `CMakeLists.txt` | drop both `list(FILTER ...)` exclusions and `ENGINE_SOURCES_TESTS`; add `-Werror` |
| `StratChessEvolved/StratChessEvolved.vcxproj` | remove the `PerftRunner.cpp` `ClCompile` entry |
| `StratChessEvolved/StratChessEvolved.vcxproj.filters` | remove its matching entry |
| `CLAUDE.md` | correct the PCH claim |

`skak.cpp` is in neither `.vcxproj`, so it needs no project deregistration.

## The CLAUDE.md correction

Current text is wrong twice over — it was already untrue for MSVC (neither project sets
`PrecompiledHeader`), and PR #168 removed the CMake PCH too, so no build anywhere precompiles it:

> `StratEngine/StdAfx.h` is the PCH — add frequently-used STL headers there, alphabetically inside
> the `#pragma warning push/pop` block, not in individual `.cpp` files.

The *advice* is still right and must survive; only the mechanism claim is false. Replace with wording
that keeps the instruction and drops the false premise, e.g. "`StratEngine/StdAfx.h` is the shared
common-include header (no build precompiles it) — add frequently-used STL headers there...".

## Validation

1. `.\build.ps1 all -Config Release` and `-Config Debug` — green, zero warnings under Level4 + `/WX`.
   Deleting `PerftRunner.cpp` from the app project is the step that could break this.
2. `.\build.ps1 run-tests` — **250 test cases / 3520 assertions**, unchanged.
3. **Linux, GCC 13.3.0, with `-Werror` active** — Release and Debug must both build clean and pass
   the fast tier at 250/3520. This is the gate that matters: `-Werror` means any warning the
   `[[maybe_unused]]` change failed to silence is now a hard failure. Method:
   `.claude` memory `reference-wsl-linux-validation-method` (`git archive` → extract on native ext4;
   building over `/mnt/c` fails at `FetchContent`).
4. `Validate-PrePR.ps1`.

## Invariants afterwards

- No engine behaviour change; test count identical on both toolchains.
- `StratEngine/Archived/` still never built — its glob exclusion stays.
- `IsValid` still executes inside `assert()` in Debug; `[[maybe_unused]]` suppresses a diagnostic,
  it does not remove the call.
- Both build systems compile the same engine source list — with `PerftRunner.cpp` gone, the app and
  test targets no longer need different lists at all.
