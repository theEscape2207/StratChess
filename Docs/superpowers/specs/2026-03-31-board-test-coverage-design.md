# Board Test Coverage — Design Spec

**Date**: 2026-03-31
**Status**: Approved
**Context**: Pre-De-Singleton Board refactor; establishes a comprehensive baseline so regressions are caught immediately when Board becomes non-singleton.

---

## Goal

Expand `[board]` test coverage from the current 6 cases (special move DoMove/UndoMove) to ~38 cases across four files, covering all move types, GameInfo state lifecycle, material score, and public query APIs. Scope is limited to the Board component only — no search, no evaluation.

---

## Files

| File | Tag | New cases | Status |
|---|---|---|---|
| `StratChessTests/BoardTests.cpp` | `[board]` | 0 (unchanged) | Existing |
| `StratChessTests/BoardMoveTests.cpp` | `[board_moves]` | 9 | New |
| `StratChessTests/BoardStateTests.cpp` | `[board_state]` | 12 | New |
| `StratChessTests/BoardApiTests.cpp` | `[board_api]` | 11 | New |

All three new files must be added to `StratChessTests.vcxproj` (ClCompile entries) and `.vcxproj.filters` (matching filter entries under `Source Files`).

TestDesign.md coverage map and Roadmap.md `[board]` entry must be updated to reflect the new files and tags.

---

## Section 1 — `BoardMoveTests.cpp` `[board_moves]`

**Purpose:** DoMove/UndoMove round-trip correctness for every move type not already covered in `BoardTests.cpp`. Verifies piece placement before, after DoMove, and after UndoMove.

**FEN constants:**

```cpp
// Kings and rooks with full castling rights (reused from BoardTests.cpp or redeclared)
static constexpr const char* FEN_CASTLING = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// White queen vs black rook for capture tests
static constexpr const char* FEN_CAPTURE  = "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn on e2 for double push
static constexpr const char* FEN_DPUSH    = "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1";

// White pawn on c7 for under-promotions (no black pieces on c8)
static constexpr const char* FEN_UPROMO   = "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// White pawn on c7 with black rook on b8 for capture-promotion
static constexpr const char* FEN_CAP_PROMO = "1r6/2P5/8/8/8/8/8/4K2k w - - 0 1";
```

**Test cases:**

| # | Name | Move | Key assertions |
|---|---|---|---|
| 1 | White queenside castling DoMove/UndoMove | `MakeMove(e1, c1, QUEEN_CASTLE)` | King on c1, rook on d1 after DoMove; e1/a1 restored after UndoMove |
| 2 | Black kingside castling DoMove/UndoMove | `MakeMove(e8, g8, KING_CASTLE)` | King on g8, rook on f8 after DoMove; e8/h8 restored after UndoMove |
| 3 | Black queenside castling DoMove/UndoMove | `MakeMove(e8, c8, QUEEN_CASTLE)` | King on c8, rook on d8 after DoMove; e8/a8 restored after UndoMove |
| 4 | Normal capture — white takes black | White queen d1×c1 (captures black rook) | Black rook gone, queen on c1 after DoMove; both restored after UndoMove |
| 5 | Normal capture — black takes white | Setup black-to-move mirror; black rook c1×d1 | White queen gone, rook on d1 after DoMove; both restored after UndoMove |
| 6 | Double pawn push DoMove/UndoMove | White e2→e4 | Pawn on e4, e2 empty after DoMove; pawn on e2 after UndoMove |
| 7 | Under-promotion to knight | `MakePromotion(c7, c8, WHITE_KNIGHT)` | Knight on c8, c7 empty after DoMove; pawn on c7 after UndoMove |
| 8 | Under-promotion to rook | `MakePromotion(c7, c8, WHITE_ROOK)` | Rook on c8, c7 empty after DoMove; pawn on c7 after UndoMove |
| 9 | Capture-promotion round-trip | `MakeMove(c7, b8, PROMOTION_QUEEN_CAPTURE)` | Queen on b8, black rook gone, c7 empty after DoMove; pawn on c7, rook on b8 after UndoMove |

**Note on cases 2 and 3:** `FEN_CASTLING` is white-to-move. For black castling tests, use `SetupFromFEN` with a FEN that sets black to move (e.g. `r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1`).

---

## Section 2 — `BoardStateTests.cpp` `[board_state]`

**Purpose:** Verify that `GameInfo` fields (castling rights, EP square, game state, fifty-move counter), material scores, and side-to-move are correctly updated by DoMove and fully restored by UndoMove. These are the fields De-Singleton Board will carry per-thread.

**FEN constants:**

```cpp
// Full castling rights, all rooks in place
static constexpr const char* FEN_FULL_RIGHTS = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// White rook on h7 can capture black rook on h8 — strips BLACK_KINGSIDE rights
static constexpr const char* FEN_ROOK_CAPTURE = "4k2r/7R/8/8/8/8/8/4K3 w k - 0 1";

// White pawn on e2 for double push / EP tests
static constexpr const char* FEN_EP_SETUP = "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1";

// Quiet rook position for side-to-move and fifty-count tests
static constexpr const char* FEN_QUIET = "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// White queen vs black rook for material score tests
static constexpr const char* FEN_MATERIAL = "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn on c7 for promotion material test
static constexpr const char* FEN_PROMO_MAT = "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";
```

**Test cases:**

| # | Name | Key assertions |
|---|---|---|
| 1 | Castling rights stripped after king move | White king e1→e2; `WHITE_BOTH` bits cleared; restored on UndoMove |
| 2 | Castling rights stripped after rook move | White rook a1→a3; `WHITE_QUEENSIDE` cleared, `WHITE_KINGSIDE` intact; restored on UndoMove |
| 3 | Castling rights stripped after rook captured | White rook h7×h8 captures black rook; `BLACK_KINGSIDE` cleared, other rights intact; restored on UndoMove |
| 4 | EP square set after double pawn push | White e2→e4; `gameInfo_.epSquare == e3` after DoMove |
| 5 | EP square cleared after non-EP follow-up | After white e2→e4, make a quiet king move; `epSquare == NO_SQUARE` |
| 6 | EP square restored on UndoMove of double push | Undo e2→e4; `epSquare == NO_SQUARE` |
| 7 | Side-to-move flips after DoMove | Starts WHITE; after any DoMove → BLACK; after UndoMove → WHITE |
| 8 | Material score decremented after capture | White queen captures black rook; `GetMaterialScore(BLACK)` decremented by rook value; restored on UndoMove |
| 9 | Material score updated after promotion | Pawn promotes to queen; `GetMaterialScore(WHITE)` increases by queen−pawn delta; restored on UndoMove |
| 10 | Fifty-move counter resets on pawn move | Set fiftyCount = 10 via FEN; pawn moves; counter == 0 after DoMove; restored on UndoMove |
| 11 | Fifty-move counter increments on quiet move | Set fiftyCount = 5 via FEN; quiet rook move; counter == 6 after DoMove; restored on UndoMove |
| 12 | Fifty-move draw triggered at 50 | Set fiftyCount = 49 via FEN; quiet move; `gameInfo_.gameState == DRAW_50_MOVES`; restored to `STILL_PLAYING` on UndoMove |

**Note on cases 10–12:** Use FEN halfmove clock field to pre-set `fiftyCount` (e.g. `"8/8/3k4/8/8/3K4/8/R7 w - - 10 1"`).

---

## Section 3 — `BoardApiTests.cpp` `[board_api]`

**Purpose:** Verify `GetCapturedPiece`, `GetEffectiveMovPiece`, and `ExtractFEN` in isolation. These are the query APIs most likely to silently break when Board is passed by reference instead of accessed via singleton.

**FEN constants:**

```cpp
// Quiet rook position
static constexpr const char* FEN_QUIET     = "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// White queen vs black rook for capture API test
static constexpr const char* FEN_CAPTURE   = "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn on d5; black pawn on e5; EP square e6
static constexpr const char* FEN_EP        = "8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1";

// White pawn on c7 for promotion API test
static constexpr const char* FEN_PROMO     = "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// Standard starting position
static constexpr const char* FEN_START     = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Position with active EP square (black pawn just pushed d7-d5, white on d4)
static constexpr const char* FEN_EP_ACTIVE = "4k3/8/8/3pP3/8/8/8/4K3 b - d6 0 1";

// Position with partial castling rights
static constexpr const char* FEN_PARTIAL_RIGHTS = "r3k3/8/8/8/8/8/8/4K2R w Kq - 0 1";

// Black to move
static constexpr const char* FEN_BLACK_TO_MOVE = "4k3/8/8/8/8/8/8/R3K3 b - - 17 34";
```

**Test cases:**

| # | Name | Key assertions |
|---|---|---|
| 1 | `GetCapturedPiece` — quiet move | `MakeQuiet(a1, h1)` → returns `NO_PIECE` |
| 2 | `GetCapturedPiece` — normal capture | White queen d1×c1 → returns `BLACK_ROOK` |
| 3 | `GetCapturedPiece` — EP capture | White d5×e6 EP → returns `BLACK_PAWN` (piece is NOT on e6) |
| 4 | `GetEffectiveMovPiece` — quiet move | `MakeQuiet(a1, h1)` → returns `WHITE_ROOK` |
| 5 | `GetEffectiveMovPiece` — promotion | `MakePromotion(c7, c8, WHITE_QUEEN)` → returns `WHITE_QUEEN`, not `WHITE_PAWN` |
| 6 | `ExtractFEN` — starting position round-trip | `SetupFromFEN(FEN_START)` → `ExtractFEN()` equals `FEN_START` |
| 7 | `ExtractFEN` — EP square preserved | `SetupFromFEN(FEN_EP_ACTIVE)` → `ExtractFEN()` contains `d6` EP field |
| 8 | `ExtractFEN` — stripped castling rights | `SetupFromFEN(FEN_PARTIAL_RIGHTS)` → `ExtractFEN()` contains `Kq` only |
| 9 | `ExtractFEN` — black to move | `SetupFromFEN(FEN_BLACK_TO_MOVE)` → `ExtractFEN()` contains ` b ` |
| 10 | `ExtractFEN` — halfmove clock | `SetupFromFEN(FEN_BLACK_TO_MOVE)` → `ExtractFEN()` contains `17` in halfmove field |
| 11 | `ExtractFEN` — fullmove number | `SetupFromFEN(FEN_BLACK_TO_MOVE)` → `ExtractFEN()` contains `34` in fullmove field |

---

## Infrastructure Changes

- Add `BoardMoveTests.cpp`, `BoardStateTests.cpp`, `BoardApiTests.cpp` to `StratChessTests.vcxproj` as `ClCompile` entries
- Add matching `<Filter>` entries in `StratChessTests.vcxproj.filters` under `Source Files`
- Update `Docs/TestDesign.md` coverage map — add new rows for `[board_moves]`, `[board_state]`, `[board_api]`
- Update `Docs/Roadmap.md` — the `[board]` entry in Phase 1 Infrastructure already shows ✅; confirm it also references the new tags
- New tags available for isolated runs: `StratChessTests.exe [board_moves]`, `[board_state]`, `[board_api]`

---

## Test Isolation Rules (inherited from TestDesign.md)

- Every `TEST_CASE` using `Board::Instance()` must call `SetupFromFEN(...)` as its first action
- No test should rely on state left by a preceding test
- `MakePromotion` / `MakeMove` / `MakeQuiet` / `MakeEnPassant` from `MoveFactory` — use the existing factory, do not construct raw `Move` structs

---

## Validation Plan

1. `.\build.ps1 tests` — clean build of test project
2. `StratChessTests/x64/Release/StratChessTests.exe [board_moves]` — all 9 cases pass
3. `StratChessTests/x64/Release/StratChessTests.exe [board_state]` — all 12 cases pass
4. `StratChessTests/x64/Release/StratChessTests.exe [board_api]` — all 11 cases pass
5. `.\build.ps1 run-tests` — full fast suite still passes (no regressions in other tags)

---

## Key Correctness Properties

- Every DoMove/UndoMove pair must leave the board in bit-for-bit identical state to before the move (mailbox, bitboards, Zobrist hash, GameInfo, material scores, side-to-move)
- `GetCapturedPiece` must be called **before** DoMove — the piece on `to` is gone after
- `GetEffectiveMovPiece` for promotions returns the promoted piece, not the pawn — this is critical for move ordering and material score update logic
- `ExtractFEN` must be a lossless round-trip: any FEN loaded via `SetupFromFEN` must produce an identical FEN string via `ExtractFEN` (modulo equivalent formatting)
