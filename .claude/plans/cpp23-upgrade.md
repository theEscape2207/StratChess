# C++23 Upgrade Analysis & Plan

**Created**: 2026-03-08
**Author**: Claude (analysis session)
**Status**: C++20 cleanup items completed; C++23 bump pending ThreadData milestone

---

## Goal

Identify which C++23 features benefit StratChessEvolved, when to upgrade the language standard, and how toolchain options (MSVC vs Clang/LLVM) fit the roadmap.

## Scope Limits

- Does **not** cover NNUE, tablebases, or any evaluation changes.
- The Clang/LLVM migration is explicitly coupled to the CMake migration (see Roadmap long-term section). It is **not** a standalone item.

---

## Part 1: What Was Already C++20 (Completed March 2026)

Two improvements were achievable within the declared `stdcpp20` standard and implemented immediately:

### 1a. `std::countr_zero` in `Board::GetFirstPiece`

**Before** — compiler-specific preprocessor fork:
```cpp
#if defined(_MSC_VER)
    const unsigned long index = static_cast<unsigned long>(_tzcnt_u64(mask));
#else
    const unsigned long index = static_cast<unsigned long>(__builtin_ctzll(mask));
#endif
```

**After** — portable C++20:
```cpp
return static_cast<eSquare>(std::countr_zero(mask));
```

MSVC compiles `std::countr_zero<uint64_t>` to the same `TZCNT` instruction as `_tzcnt_u64`. No behavioral change. Requires `<bit>` added to `StdAfx.h` (PCH).

### 1b. `std::format` in `Move::Output()`

**Before** — `std::stringstream` with intermediate `std::string` temporaries, called thousands of times per second in verbose search logging.

**After** — `std::format` returns `std::string` directly with a single allocation and no intermediate stream object:
```cpp
return std::format("{}-{}", strFrom, strTo);  // QUIET
return std::format("{}x{}", strFrom, strTo);  // CAPTURE
// etc.
```

Requires `<format>` added to `StdAfx.h` (PCH). `<sstream>` kept in PCH (4 other files still use stringstream).

---

## Part 2: True C++23 Benefits (Pending Standard Bump)

### Language Standard Change

The `.vcxproj` XML element changes in **both** `StratChessEvolved/StratChessEvolved.vcxproj` and `StratChessTests/StratChessTests.vcxproj` for the x64 Debug and Release configurations:

```xml
<!-- Before -->
<LanguageStandard>stdcpp20</LanguageStandard>

<!-- After -->
<LanguageStandard>stdcpplatest</LanguageStandard>
```

Note: MSBuild has no `stdcpp23` element; `stdcpplatest` is the standard practice. This means "adopt the newest standard this compiler version supports," which is C++23 on VS 2022 17.8+ and will advance to C++26 when MSVC adds it.

**Minimum VS 2022 version required**: 17.8 (for `std::mdspan` and `std::flat_map`). As of early 2026 the installed version is 17.12+, so the requirement is already satisfied.

### Priority 1 — `std::mdspan` for the history table (Primary Motivator)

**File**: `StratEngine/AIPerplex.h` line 147
**Dependency**: Extract ThreadData Structure (roadmap Critical item)

Current declaration:
```cpp
int32_t history_[2][64][64];
```

This raw C-style 3D array is one of the obstacles to clean per-thread state cloning for Lazy SMP. When `ThreadData` is extracted, each thread needs its own history table. With a raw array, the proposed `ThreadData` struct would hold an identical `int32_t history_[2][64][64]` per thread — semantically correct but inflexible.

With `std::mdspan`:
```cpp
// Owning storage in ThreadData:
std::array<int32_t, 2 * 64 * 64> history_storage{};

// Non-owning view — same access pattern, no copy overhead:
std::mdspan<int32_t, std::extents<int, 2, 64, 64>> history{ history_storage.data() };

// C++23 multidimensional operator[]:
history[side, mv.from(), mv.to()] += depth * depth;
```

Benefits:
- `history_storage` is trivially copyable (plain `std::array`) → thread cloning is a memcpy
- Non-owning view can be passed through helper functions without pointer arithmetic
- `std::atomic_ref<int32_t>` can wrap individual elements for lock-free Lazy SMP updates
- `age_history()` triple loop becomes a single `std::ranges::for_each` over the flat storage

### Priority 2 — `std::expected` for FENParser

**File**: `StratEngine/Utils/FENParser.h` / `Board.cpp` line 126
**Dependency**: De-Singleton Board (pairs naturally with API cleanup)

Current pattern — out-parameters + optional error:
```cpp
auto parseError = FENParser::ParseFEN(fen, state, pieces);
if (parseError) { spdlog::error("FEN error: {}", *parseError); return; }
// use state, pieces via out-params
```

C++23 replacement — value-or-error return:
```cpp
auto result = FENParser::ParseFEN(fen);
if (!result) { spdlog::error("FEN error: {}", result.error()); return; }
auto [state, pieces] = *result;
```

Structural improvement: the error path is impossible to silently ignore, and out-parameters are eliminated.

### Priority 3 — `std::flat_multimap` for TT diagnostics

**File**: `StratEngine/AIPerplex.h` line 175
**Cost**: Near-zero once the standard is bumped.

```cpp
// Before (red-black tree, pointer-chasing iteration):
std::multimap<std::uint64_t, int> tt_misses;

// After (sorted contiguous vector, cache-friendly iteration):
std::flat_multimap<std::uint64_t, int> tt_misses;
```

The `tt_misses` structure is built by repeated `emplace` calls then iterated once in sorted order — exactly the batch-insert / sort-once / iterate pattern `std::flat_multimap` is optimised for.

### Priority 4 — `std::views::enumerate` in `pvs()` move scoring

**File**: `StratEngine/AIPerplex.cpp` lines 308–336

Replaces the manual index variable in the move scoring loop:
```cpp
// Before:
for (int i = 0; i < n; ++i) {
    const Move& mv = moveList[i];
    // compute score s
    scored_idx[i] = { s, i };
}

// After (C++23):
for (auto [i, mv] : std::views::enumerate(moveList) | std::views::take(n)) {
    scored_idx[i] = { score_move(mv, ...), static_cast<int>(i) };
}
```

Reduces surface area for off-by-one errors. Low priority on its own; free once the standard is bumped and the move-scoring logic is relocated to `MoveSorter` / `MoveOrdering`.

### Features That Don't Apply Yet

| Feature | Reason |
|---|---|
| `std::print` / `std::println` | spdlog provides structured logging; no gain replacing it |
| `std::generator` (coroutines) | `MoveList` stack array is hard to beat for the hot path |
| `std::byteswap` | Useful for magic bitboards; current rotated-board approach doesn't need it |
| `std::atomic_ref` (actually C++20) | Needed for parallel history updates, but only when Lazy SMP lands |

---

## Part 3: Toolchain — MSVC vs Clang/LLVM

### Keep MSVC Until CMake Migration

**Current**: MSVC toolset v145 (VS 2022) / MSBuild / `build.ps1`

Migrating to Clang/LLVM as a standalone change (MSBuild + Clang) is high cost, low reward:
- Requires custom `PlatformToolset` in vcxproj or a LLVM Visual Studio extension
- LTCG behaviour changes; NPS regression risk without careful `/GL` + LTO equivalence verification
- No cross-platform benefit without CMake anyway

**Recommendation**: Combine Clang/LLVM adoption with the long-term CMake migration item. At that point it is additive:

| Advantage | Notes |
|---|---|
| Leading-edge C++23/26 language conformance | Clang often implements proposals ahead of MSVC |
| `clang-tidy` static analysis | Replaces or supplements PVS-Studio |
| `clang-format` enforcement | CI-enforceable formatting |
| Linux/Mac builds for free | Direct result of CMake + Clang |
| `std::countr_zero` → `__builtin_ctzll` (identical) | No instruction change on x64 |

MSVC C++23 standard **library** coverage is complete for all features relevant to this project (VS 2022 17.8+). Clang is not needed for the standard bump.

---

## Execution Plan

### Already Done (March 2026)
- [x] Add `<bit>` and `<format>` to `StdAfx.h` PCH
- [x] Replace `GetFirstPiece` preprocessor fork with `std::countr_zero`
- [x] Replace `Move::Output()` `std::stringstream` with `std::format`

### Do With ThreadData Extraction
- [ ] Bump `.vcxproj` to `stdcpplatest` (both projects, x64 configurations)
- [ ] Implement `std::mdspan` for `history_` in `ThreadData`
- [ ] Adopt `std::flat_multimap` for `tt_misses`

### Do With De-Singleton Board
- [ ] Migrate `FENParser::ParseFEN` to `std::expected<ParsedFEN, std::string>`

### Do With MoveOrdering Refactor
- [ ] Use `std::views::enumerate` in the move scoring loop

### Do With CMake Migration (Long-term)
- [ ] Switch compiler to Clang/LLVM
- [ ] Add `clang-tidy` and `clang-format` to CI

---

## Key Correctness Properties

- `std::countr_zero(mask)` produces identical assembly to `_tzcnt_u64(mask)` on x64 MSVC — zero behavioral change
- `std::format` output is character-for-character identical to the `stringstream` output for all move types — verified by existing `[formatter]` test suite (65 assertions)
- `stdcpplatest` on VS 2022 17.8+ enables C++23 standard library features without enabling any unstable language extensions
- The `/WX` (warnings as errors) build constraint is unaffected; C++23 deprecations do not touch any APIs used by this project
