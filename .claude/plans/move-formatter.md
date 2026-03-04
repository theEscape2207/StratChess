# Plan: Introduce MoveFormatter — Centralise Move Presentation

**Date**: 2026-03-04
**Branch**: dreamy-snyder (worktree)
**Roadmap item**: 🟡 High — Introduce MoveFormatter — centralise move presentation

---

## Goal

Create a stateless `MoveFormatter` class with static methods that centralise all
move-to-string and string-to-move conversions. This removes fragile string-surgery
from `Game::PrintBoardAndMove`, restores the verbose English description that was lost
when `Move` dropped its piece fields, adds `+`/`#` annotations consistently (including
to `gamelist.txt`), adds the promotion suffix to coordinate output, and provides a
clean `ToUCI`/`FromUCI` API that the future UCI protocol layer will depend on.

**Scope**: formatting only. No changes to search, evaluation, or move generation.

---

## Design Decisions

1. **Stateless class with static methods** — no instance needed, no mutable state.
2. **Post-`DoMove` API contract** — all board-context methods assume the move has already
   been applied (`board.GetPiece(move.to())` returns the moved piece).
3. **`ToShort` calls `Move::Output(ePiece)` internally** — reuses the existing pseudo-LAN
   logic rather than duplicating it; appends `+` when `board.InCheck()`.
4. **`Move::Output()` / `Move::Output(ePiece)` kept as-is** — 12 callers in AIPerplex
   use coordinate-only output for search logging and do not need board context.
   Migration added to Roadmap as a future task.
5. **`ToSAN` omitted** — full Standard Algebraic Notation requires disambiguation logic
   (two rooks can both reach d1 → need file/rank qualifier). Deferred until a concrete
   consumer (PGN export) exists.
6. **Checkmate `#` not appended** — `InCheck()` is cheap; detecting checkmate requires
   legal move generation (expensive). Callers who know the game ended in mate can append
   `#` themselves. This is sufficient for all current consumers.
7. **`Perft::move_to_string` replaced by `MoveFormatter::ToUCI`** — eliminates an
   independent reimplementation and fixes a known bug where promotion-capture MoveTypes
   (values 12–15) received no suffix.
8. **Files in `StratEngine/`** — alongside `Move.h/cpp`, natural home.
9. **Unit tests added** — `StratChessTests/MoveFormatterTests.cpp`, tag `[formatter]`.

---

## Files Changed

| File | Change |
|------|--------|
| `StratEngine/MoveFormatter.h` | NEW — class declaration |
| `StratEngine/MoveFormatter.cpp` | NEW — implementation |
| `StratEngine/Game.cpp` | Replace 2 call sites; remove string surgery; add verbose line |
| `StratEngine/Tests/Perft.cpp` | Delete `move_to_string`; use `MoveFormatter::ToUCI` |
| `StratChessEvolved/StratChessEvolved.vcxproj` | Add MoveFormatter.h and .cpp |
| `StratChessEvolved/StratChessEvolved.vcxproj.filters` | Add filter entries |
| `StratChessTests/MoveFormatterTests.cpp` | NEW — unit tests |
| `StratChessTests/StratChessTests.vcxproj` | Add MoveFormatterTests.cpp |
| `StratChessTests/StratChessTests.vcxproj.filters` | Add filter entry |
| `Docs/Roadmap.md` | Mark ✅; add Move::Output migration note |
| `Docs/TestDesign.md` | Add `[formatter]` coverage row |

---

## Step-by-Step Changes

### 1. Create `StratEngine/MoveFormatter.h`

Declare the class with four static methods:
- `ToShort(const Move&, const Board&) -> std::string`
- `ToVerbose(const Move&, const Board&) -> std::string`
- `ToUCI(const Move&) -> std::string`
- `FromUCI(std::string_view, const Board&) -> Move`

Include `Move.h` (to allow returning `Move` by value). Forward-declare `Board`.

### 2. Create `StratEngine/MoveFormatter.cpp`

**`ToShort`**: calls `move.Output(board.GetPiece(move.to()))`, appends `+` if
`board.InCheck()`.

**`ToVerbose`**: switch on MoveType; uses `PieceHelper::FullName` for the piece name;
local `PieceTypeName(piece)` helper returns lowercase type names ("queen", "rook", etc.)
indexed by `static_cast<size_t>(piece) >> 1`; appends " and checks!" if `board.InCheck()`.
Castling phrase uses `move.to()` file to distinguish kingside/queenside.

**`ToUCI`**: local lambda converts square to two-char coord (no separator); switch on
MoveType appends lowercase promotion letter for all 8 promotion variants (fixes
promotion-capture omission in the old `move_to_string`).

**`FromUCI`**: parses 4–5 char string; local `parse_sq` lambda: `file = ch - 'a'`,
`rank = '8' - ch`, `sq = rank*8 + file`; infers MoveType from board state (castling by
king + file diff ±2; EP by pawn diagonal to empty square; double push by pawn + 2-rank
delta; else capture or quiet).

### 3. Modify `StratEngine/Game.cpp`

Add `#include "MoveFormatter.h"`.

**`PrintBoardAndMove`**: replace the piece lookup + `Output(movPiece)` + string-surgery
block with:
```cpp
sstream << "Last move: " << MoveFormatter::ToShort(move, board) << '\n';
sstream << MoveFormatter::ToVerbose(move, board) << '\n';
```

**`PrintGameMoves`**: replace piece lookup + `Output(movPiece)` with:
```cpp
movesFile_ << "Last move: " << MoveFormatter::ToShort(move, Board::Instance()) << '\n';
```
(check annotation now included automatically via `ToShort`).

### 4. Modify `StratEngine/Tests/Perft.cpp`

Add `#include "MoveFormatter.h"`. Delete `move_to_string` method definition and
declaration from `Perft.h`. In `divide()`, replace `move_to_string(move)` with
`MoveFormatter::ToUCI(move)`.

### 5. Update `StratEngine/Tests/Perft.h`

Remove `static std::string move_to_string(const Move&);` declaration.

### 6. Update project files

- `StratChessEvolved.vcxproj`: add `ClCompile` and `ClInclude` entries for MoveFormatter
- `StratChessEvolved.vcxproj.filters`: add filter entries (Source Files / Header Files)
- `StratChessTests.vcxproj`: add `ClCompile` for `MoveFormatterTests.cpp` and
  `ClCompile` for `MoveFormatter.cpp`
- `StratChessTests.vcxproj.filters`: add filter entry for test file

### 7. Create `StratChessTests/MoveFormatterTests.cpp`

Tests covering:
- `ToUCI`: all MoveType variants (quiet, capture, EP, both castle types, all 4 quiet
  promotions, all 4 promotion-captures)
- `ToShort`: basic move (no check), move that gives check, castling, promotion
- `ToVerbose`: quiet, capture, castling, promotion (spot checks)
- `FromUCI`: quiet, castling, EP, promotion round-trip

### 8. Update Roadmap.md and TestDesign.md

---

## Validation Plan

```bash
# Build main executable
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
  "StratChessEvolved.sln" //p:Configuration=Release //p:Platform=x64 //m //v:minimal

# Build and run test suite
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
  "StratChessTests/StratChessTests.vcxproj" //p:Configuration=Release //p:Platform=x64 //p:CL_MPCount=1 //v:minimal

StratChessTests/x64/Release/StratChessTests.exe            # all tests
StratChessTests/x64/Release/StratChessTests.exe [formatter] # new formatter tests
StratChessTests/x64/Release/StratChessTests.exe [perft]     # perft (uses ToUCI now)

# Deep perft (run from Tests/ directory)
cd Tests && ../x64/Release/StratChessEvolved.exe perft test

# Self-play (verify PrintBoardAndMove output)
# Set both sides to type 6 in game_settings.json, run StratChessEvolved.exe
```

---

## Key Correctness Properties

1. `ToUCI(move)` matches `Perft::move_to_string` output for all move types except
   promotion-captures (which now correctly include the suffix).
2. `ToShort(move, board)` output equals `move.Output(movPiece)` output plus optional
   `+` suffix — verified by running tests with and without check positions.
3. `FromUCI(ToUCI(move), board_before)` round-trips correctly for all non-ambiguous move
   types (quiet, capture, EP, castle, promotion) — verified by unit tests.
4. `PrintBoardAndMove` no longer uses `find`/`insert` string surgery.
5. `gamelist.txt` now contains `+` on checking moves (verified by self-play session).
6. All 50 unit tests pass with 214 assertions (47 pre-existing + 3 new test cases).

---

## Post-Review Fixes (applied after three-way code review)

The following issues were found and fixed before merging:

| # | Severity | Issue | Fix |
|---|----------|-------|-----|
| 1 | Bug | `FromUCI`: invalid 5-char promotion suffix (e.g. `"b7b8x"`) defaulted silently to queen instead of returning `Move{}` | Rewrote promotion switch with `default: return Move{};` |
| 2 | Doc | `MoveFormatter.h` class comment incorrectly included `FromUCI` in the "post-DoMove" contract | Split comment: `ToShort`/`ToVerbose` = post-DoMove; `FromUCI` = pre-DoMove |
| 3 | Test gap | Missing test for 5-char invalid promotion suffix | Added `SECTION("Malformed input returns empty move — invalid promotion suffix")` |
| 4 | Test gap | Round-trip pawn push test did not verify `MoveType::DOUBLE_PAWN_PUSH` was recovered | Added `CHECK(MoveHelper::AsType(rt) == MoveType::DOUBLE_PAWN_PUSH)` |
| 5 | Convention | `static` qualifier on functions inside anonymous namespace is redundant (anonymous namespace already provides internal linkage) | Removed `static` from `SquareToCoord` and `PieceTypeName` |
| 6 | Doc | `PrintGameMoves` in `Game.cpp` had no comment explaining the post-DoMove invariant, unlike `PrintBoardAndMove` which had one | Added call-site comment referencing `AddGameMove()` → `Run()` → post-DoMove |

Items noted but deferred (tracked in Roadmap):
- `SquareToCoord` duplicates `GetBoardCoord` from `Move.cpp` → move to `SquareHelper` when a second consumer appears
- `PieceTypeName` duplicates piece-type names → add `PieceHelper::TypeName(ePiece)` as single source of truth
