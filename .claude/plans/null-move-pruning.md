# Null-Move Pruning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix and complete the existing (uncommitted, broken) Null-Move Pruning
scaffold in `AIPerplex::pvs()` — guard it against zugzwang, mate-score
contamination, and consecutive null moves, fix a latent en-passant bug in
`Board::DoNullMove`, add unit/regression test coverage, then enable it by
default.

**Architecture:** NMP stays at its existing integration point in `pvs()`
(after the TT probe, before move generation). The scattered inline guard
condition is extracted into a private, friend-testable helper
`should_try_null_move(depth, beta, ply, is_pv_node, in_check)`. A new
ply-indexed `last_move_was_null_[MAX_PLY]` array (same shape as `killers_`)
tracks consecutive null moves. `Board::DoNullMove` gains an en-passant clear
to match what every real `DoMove` already does.

**Tech Stack:** C++20, Catch2 v3 (`StratChessTests`), MSVC `/W4 /WX`, existing
`AIPerlexTestFixture` friend-test pattern.

---

## Background — why this plan exists

An uncommitted scaffold already sits in the working tree:
`StratEngine/AIPerplex.h` has `tuning_.null_move_enabled/null_move_reduction/null_move_min_depth`
(disabled by default), and `StratEngine/AIPerplex.cpp` has a first-pass cutoff
block in `pvs()`. `StratEngine/Board.cpp` already has `DoNullMove()`/`UndoNullMove()`
primitives with doc comments anticipating this exact use.

That scaffold currently:
- **Does not build** — `const bool in_check = m_Board.InCheck();` is declared
  twice in the same `pvs()` scope (once by the new NMP block, once later by
  the pre-existing LMR logic).
- **Has no zugzwang guard** — will mis-prune in king+pawn endgames where the
  null-move assumption ("passing is never better than moving") is false.
- **Has no mate-score guard** — a null-move fail-high near
  `GameValues::Mate_Threshold` doesn't correspond to a real line and can
  corrupt mate-distance reporting.
- **Has no consecutive-null guard** — nothing stops the search from
  "passing" twice in a row.
- **Depends on a buggy `Board::DoNullMove`** — every real `DoMove` clears
  `gameInfo_.epSquare` (and toggles the zobrist EP key) unless the move is a
  double pawn push (`Board.cpp:393-396`). `DoNullMove` does neither, so a
  pending en-passant right can illegally survive one extra ply inside a
  null-move subtree.

This plan fixes all of the above, in test-first order, then enables the
feature.

---

### Task 1: Fix the build error and extract the guard into a testable helper

**Files:**
- Modify: `StratEngine/AIPerplex.h`
- Modify: `StratEngine/AIPerplex.cpp:264-345` (the `pvs()` NMP block + the
  pre-existing `in_check` declaration further down)
- Test: `StratChessTests/SearchTests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `StratChessTests/SearchTests.cpp`, inside `AIPerlexTestFixture` (after
the existing `emergency(...)` method):

```cpp
    bool try_null_move(int depth, int beta, int ply, bool is_pv_node, bool in_check) const
        { return ai->should_try_null_move(depth, beta, ply, is_pv_node, in_check); }
```

Then append these `TEST_CASE`s at the end of the file:

```cpp
// ============================================================================
// should_try_null_move tests
// ============================================================================

TEST_CASE("Search - should_try_null_move: disabled returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = false;

    REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: PV node returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, 0, 1, /*is_pv_node=*/true, false) == false);
}

TEST_CASE("Search - should_try_null_move: in check returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, 0, 1, false, /*in_check=*/true) == false);
}

TEST_CASE("Search - should_try_null_move: depth below minimum returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled  = true;
    fix.ai->tuning().null_move_min_depth = 3;

    REQUIRE(fix.try_null_move(/*depth=*/2, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: mate-score beta returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, GameValues::Mate_Threshold, 1, false, false) == false);
    REQUIRE(fix.try_null_move(4, -GameValues::Mate_Threshold, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: zugzwang (no non-pawn material) returns false", "[search]")
{
    // White: king + pawn only. Black: king only. No non-pawn material for
    // the side to move (white) -> zugzwang guard must refuse NMP.
    Board::Instance().SetupFromFEN("8/8/8/3k4/8/3K4/3P4/8 w - - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: consecutive null move returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;
    fix.ai->last_move_was_null_[2] = true;   // ply 2 was reached via a null move

    REQUIRE(fix.try_null_move(4, 0, /*ply=*/2, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: otherwise-eligible position returns true", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled  = true;
    fix.ai->tuning().null_move_min_depth = 3;

    REQUIRE(fix.try_null_move(4, 0, 1, false, false) == true);
}
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run:
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [search]"
```
Expected: **compile error** — `should_try_null_move` and `last_move_was_null_`
are not members of `AIPerplex` yet. This confirms the test exercises code
that doesn't exist.

- [ ] **Step 3: Add the member, the helper declaration, and fix the build error**

In `StratEngine/AIPerplex.h`, inside the `private:` section, add the new
member next to `killers_` (after the `killers_[MAX_PLY][MAX_KILLERS]`
declaration):

```cpp
	// Null-move consecutive-pass guard: last_move_was_null_[ply] is true when
	// the move that led to this ply was itself a null move. Indexed the same
	// way as killers_; cleared at search start and reset immediately after
	// each null-move attempt completes (see pvs()).
	bool last_move_was_null_[MAX_PLY]{};
```

Add the helper declaration next to the other `// Quality assessment` /
search-helper declarations (near `assess_iteration_quality`):

```cpp
	bool should_try_null_move(int depth, int beta, int ply, bool is_pv_node, bool in_check) const;
```

Add a clear method next to `clear_killers()`:

```cpp
	void clear_null_move_flags() noexcept;
```

In `StratEngine/AIPerplex.cpp`, replace the entire broken NMP block
(currently lines 331-344, the duplicate `in_check` declaration through the
end of the null-move `if` block) **and** the later duplicate declaration
(currently `const bool in_check = m_Board.InCheck();` right before
`bool moveFound = false;`) with a single consolidated version:

```cpp
	// Get PV move from previous iteration
	if (is_pv_node && ply > 0) {
		pv_move = pv_table.get_pv_move(ply);
	}

	const bool in_check = m_Board.InCheck();

	// Null-move pruning: cheap cutoff attempt before move generation.
	// should_try_null_move() centralises every guard (zugzwang, mate-score,
	// consecutive-null, PV/in-check/depth) so it can be unit tested directly.
	if (should_try_null_move(depth, beta, ply, is_pv_node, in_check)) {
		const int R = tuning_.null_move_reduction;
		last_move_was_null_[ply + 1] = true;
		m_Board.DoNullMove();
		int null_score = -pvs(depth - 1 - R, -beta, -beta + 1, ply + 1, false, tt, pv_table);
		m_Board.UndoNullMove();
		last_move_was_null_[ply + 1] = false;
		if (null_score >= beta) {
			tt.store(key, static_cast<int16_t>(null_score), static_cast<int16_t>(depth),
				static_cast<int16_t>(ply), Move::EmptyMove(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
			return null_score;
		}
	}

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(info, moveList);

	bool first_child = true;
	int best_value = -GameValues::Search_Init;
	Move best_move;


	// Stack-allocated scored index array — zero heap allocation per call.
	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	const int n = static_cast<int>(moveList.size());
	const eColor side = m_Board.GetCurrentColor();

	MoveSorter::ScoreMoves(moveList, n, m_Board, side,
		pv_move, hash_move,
		killers_[ply][0], killers_[ply][1],
		history_, scored_idx);

	bool moveFound = false;
```

(This removes the now-redundant second `const bool in_check = m_Board.InCheck();`
that used to sit right before `bool moveFound = false;` — `in_check` is now
computed exactly once, earlier, and reused by both the NMP block and the
existing LMR logic later in the function.)

Add the helper implementation in `StratEngine/AIPerplex.cpp`, next to
`should_stop_early`:

```cpp
bool AIPerplex::should_try_null_move(int depth, int beta, int ply, bool is_pv_node, bool in_check) const
{
	if (!tuning_.null_move_enabled) return false;
	if (is_pv_node || in_check) return false;
	if (depth < tuning_.null_move_min_depth) return false;
	if (std::abs(beta) >= GameValues::Mate_Threshold) return false;
	if (last_move_was_null_[ply]) return false;

	// Zugzwang guard: refuse to "pass" for a side that has no non-pawn
	// material — the null-move assumption ("a free pass is never better
	// than moving") is false in king+pawn endgames.
	const eColor side = m_Board.GetCurrentColor();
	const auto boards = m_Board.GetBitBoards();
	const BITBOARD non_pawn_material =
		boards[KNIGHT + side] | boards[BISHOP + side] | boards[ROOK + side] | boards[QUEEN + side];
	return non_pawn_material != 0;
}
```

Add `clear_null_move_flags()` next to `clear_killers()`:

```cpp
void AIPerplex::clear_null_move_flags() noexcept
{
	std::memset(last_move_was_null_, 0, sizeof(last_move_was_null_));
}
```

In `iterative_deepening()`, call it alongside the existing `clear_killers()`
call at the start of the search:

```cpp
	clear_killers();	// Clear killer moves at the start of the search
	clear_null_move_flags();
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [search]"
```
Expected: PASS — all 8 new `should_try_null_move` cases plus the pre-existing
`[search]` cases (assess/stop_early/emergency) pass.

- [ ] **Step 5: Full build check**

Run:
```
.\build.ps1 all
```
Expected: clean build, zero warnings under `/W4 /WX` for both
`StratChessEvolved` and `StratChessTests`.

- [ ] **Step 6: Commit**

```bash
git add StratEngine/AIPerplex.h StratEngine/AIPerplex.cpp StratChessTests/SearchTests.cpp
git commit -m "fix: guard null-move pruning (zugzwang, mate-score, consecutive-null)"
```

---

### Task 2: Fix `Board::DoNullMove` en-passant bug

**Files:**
- Modify: `StratEngine/Board.cpp:775-791`
- Test: `StratChessTests/BoardTests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `StratChessTests/BoardTests.cpp` (the file already defines
`FEN_EP = "8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1"` at the top — white pawn d5,
black pawn just played e7-e5, en-passant square e6 pending):

```cpp
TEST_CASE("Board - DoNullMove clears pending en-passant right; UndoNullMove restores it", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP);

    REQUIRE(board.GetGameInfo().epSquare == e6);
    const uint64_t hash_before = board.get_zobrist_hash();

    board.DoNullMove();

    REQUIRE(board.GetGameInfo().epSquare == NO_SQUARE);
    REQUIRE(board.get_zobrist_hash() != hash_before);

    board.UndoNullMove();

    REQUIRE(board.GetGameInfo().epSquare == e6);
    REQUIRE(board.get_zobrist_hash() == hash_before);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [board]"
```
Expected: FAIL on `REQUIRE(board.GetGameInfo().epSquare == NO_SQUARE)` —
`DoNullMove()` currently leaves `epSquare` at `e6`.

- [ ] **Step 3: Fix `Board::DoNullMove`**

In `StratEngine/Board.cpp`, replace the current `DoNullMove()` body:

```cpp
void Board::DoNullMove()
{
	// Record snapshot for undo
	capturedHistory_[currentPly_] = ePiece::NO_PIECE;
	gameInfoHistory_[currentPly_] = gameInfo_;
	irreversiblePlyHistory_[currentPly_] = last_irreversible_ply_;
	zobrist_history_[currentPly_] = zobrist_hash_;

	if (sideToMove_ == eColor::BLACK)
		gameInfo_.fullMoveCount++;

	currentPly_++;

	// Switch side and update zobrist via change_player
	change_player();
	push_position();
}
```

with:

```cpp
void Board::DoNullMove()
{
	// Record snapshot for undo
	capturedHistory_[currentPly_] = ePiece::NO_PIECE;
	gameInfoHistory_[currentPly_] = gameInfo_;
	irreversiblePlyHistory_[currentPly_] = last_irreversible_ply_;
	zobrist_history_[currentPly_] = zobrist_hash_;

	// A null move forfeits any pending en-passant right, exactly like any
	// other non-double-push move does in DoMove() (see GetEnPassantSquare
	// usage there) — otherwise a stale EP square would illegally survive
	// one extra ply inside the null-move subtree.
	if (gameInfo_.epSquare != NO_SQUARE) {
		update_zobrist_ep(gameInfo_.epSquare, NO_SQUARE);
		gameInfo_.epSquare = NO_SQUARE;
	}

	if (sideToMove_ == eColor::BLACK)
		gameInfo_.fullMoveCount++;

	currentPly_++;

	// Switch side and update zobrist via change_player
	change_player();
	push_position();
}
```

(`UndoNullMove()` already restores the full `gameInfo_` struct — including
`epSquare` — from `gameInfoHistory_[currentPly_]`, and restores
`zobrist_hash_` from `zobrist_history_[currentPly_]`, so it needs no change.)

- [ ] **Step 4: Run test to verify it passes**

Run:
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [board]"
```
Expected: PASS — including all pre-existing `[board]` cases (no regressions).

- [ ] **Step 5: Commit**

```bash
git add StratEngine/Board.cpp StratChessTests/BoardTests.cpp
git commit -m "fix: Board::DoNullMove forfeits pending en-passant right"
```

---

### Task 3: Tactical regression test — guard prevents zugzwang mis-pruning

**Files:**
- Modify: `StratChessTests/TacticalFullTests.cpp`

- [ ] **Step 1: Write the test**

Append to `StratChessTests/TacticalFullTests.cpp` (after the existing
`TEST_CASE` that runs `kSlowCases`):

```cpp
// ---------------------------------------------------------------------------
// Null-move pruning: guard must not alter search results in a zugzwang
// position. White has king+pawn only, Black has king only — should_try_null_move's
// zugzwang guard must refuse NMP here, so enabling/disabling NMP must produce
// byte-identical search results (same nodes, move, and score).
// ---------------------------------------------------------------------------
TEST_CASE("Tactical (full) - null-move pruning guard is a no-op in K+P endgame", "[tactical_full][slow]")
{
    const char* fen = "8/8/8/3k4/8/3K4/3P4/8 w - - 0 1";
    constexpr unsigned depth = 5;

    Board::Instance().SetupFromFEN(fen);
    auto ai_disabled = make_tactical_engine(depth);
    ai_disabled->tuning().null_move_enabled = false;
    GameInfo info_disabled = Board::Instance().GetGameInfo();
    Move move_disabled = ai_disabled->GetMove(info_disabled);
    SearchResult result_disabled = ai_disabled->GetLastResult();

    Board::Instance().SetupFromFEN(fen);
    auto ai_enabled = make_tactical_engine(depth);
    ai_enabled->tuning().null_move_enabled = true;
    GameInfo info_enabled = Board::Instance().GetGameInfo();
    Move move_enabled = ai_enabled->GetMove(info_enabled);
    SearchResult result_enabled = ai_enabled->GetLastResult();

    REQUIRE(move_disabled == move_enabled);
    REQUIRE(result_disabled.best_score == result_enabled.best_score);
    REQUIRE(result_disabled.nodes_searched == result_enabled.nodes_searched);
}
```

`make_tactical_engine()` returns `std::unique_ptr<PlayerBase>`; `tuning()` is
declared on `AIPerplex`, not `PlayerBase`, so this needs a small addition to
`TacticalTestHelpers.h` — add right after the existing `make_tactical_engine`
function:

```cpp
// Down-casts a tactical engine to AIPerplex so tests can reach tuning_ via
// the public tuning() accessor (e.g. to toggle null_move_enabled).
inline AIPerplex& as_perplex(std::unique_ptr<PlayerBase>& ai)
{
    return *static_cast<AIPerplex*>(ai.get());
}
```

Then use `as_perplex(ai_disabled).tuning().null_move_enabled = false;` /
`as_perplex(ai_enabled).tuning().null_move_enabled = true;` in place of the
direct `ai_disabled->tuning()` calls above (the `std::unique_ptr<PlayerBase>`
return type does not expose `tuning()` directly).

- [ ] **Step 2: Run test to verify it passes**

Run:
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [tactical_full]"
```
Expected: PASS. If it fails on `nodes_searched`, the zugzwang guard in
`should_try_null_move` is not correctly excluding this position — re-check
the `non_pawn_material` bitboard computation in Task 1 before proceeding.

- [ ] **Step 3: Run the full extended suite**

Run:
```
.\build.ps1 extended-tests
```
Expected: all tests pass, including `[slow]`.

- [ ] **Step 4: Commit**

```bash
git add StratChessTests/TacticalFullTests.cpp StratChessTests/TacticalTestHelpers.h
git commit -m "test: zugzwang regression case for null-move pruning guard"
```

---

### Task 4a: Fix `m_infoSeq` desync on null-move ply increment

**Discovered during Task 4's `extended-tests` run** (after flipping `null_move_enabled`
to `true`): every tactical test crashed with an "invalid vector subscript" /
out-of-range exception inside `pvs()`. Root cause:

`PlayerAiBase::m_infoSeq` (a `std::vector<GameInfo>`, `PlayerAI.h:223`) is kept
in lockstep with the search's `ply` parameter, but **only** by
`AddMoveToSeq(move, ply)` (`PlayerAI.cpp:196-217`), which every real move calls
right after `DoMove()`. The null-move block added in Task 1
(`AIPerplex.cpp:337-349`) calls `m_Board.DoNullMove()` and recurses into
`pvs(..., ply + 1, ...)` but never calls anything to extend `m_infoSeq` for
that synthetic ply. The very first line of the recursive call,
`GameInfo info = GetLastBoardInfo(ply)` → `m_infoSeq.at(ply)`
(`AIPerplex.cpp:284`), then indexes one past the end of `m_infoSeq` and
throws/crashes. This is why Tasks 1-3's tests never caught it: Task 1's tests
exercise `should_try_null_move()` as a pure predicate (never actually calling
`DoNullMove`/recursing), and Task 3's zugzwang test is specifically
constructed so the guard *prevents* NMP from firing at all — neither test
exercises a *successful* null-move recursion.

A second, related hazard found during root-cause analysis: `pvs()` currently
takes `const GameInfo& info = GetLastBoardInfo(ply);` (`AIPerplex.cpp:284`) —
a reference into `m_infoSeq`'s storage — and uses it again later at
`MoveGenerator::ComputeLegalMoves(info, moveList)` (`AIPerplex.cpp:352`), with
the null-move block sitting in between. Once the null-move block is fixed to
call an `m_infoSeq`-extending method, that call can trigger a
`std::vector::emplace_back` reallocation, which would invalidate the `info`
reference between its two uses — a latent dangling-reference bug that only
exists once Task 4a's primary fix is applied, not before. Both issues are
fixed together below.

**Files:**
- Modify: `StratEngine/PlayerAI.h`
- Modify: `StratEngine/PlayerAI.cpp`
- Modify: `StratEngine/AIPerplex.cpp`
- Test: a `[tactical]`-style test that forces `null_move_enabled = true` on a
  non-zugzwang position at a depth that guarantees NMP actually fires and
  recurses — proving the crash is fixed, not just guarded around.

**Fix:**

1. In `StratEngine/PlayerAI.cpp`, extract `AddMoveToSeq`'s size-bookkeeping
   into a small private helper, then add a null-move variant that sources its
   `GameInfo` directly from the board (no `Move` to derive it from):
   ```cpp
   void PlayerAiBase::StoreInfoAtPly(size_t ply, const GameInfo& info)
   {
       const size_t infoSize = m_infoSeq.size();

       if (ply + 1 == infoSize)
           m_infoSeq.emplace_back(info);
       else if (infoSize == ply + 2)
           m_infoSeq[ply + 1] = info;
       else if (infoSize > ply + 2)
       {
           m_infoSeq.erase(m_infoSeq.begin() + static_cast<int>(ply + 1), m_infoSeq.end());
           m_infoSeq.emplace_back(info);
       }
       else
           assert(!"BoardInfo update - Somebody hasn't handled all cases");
   }

   void PlayerAiBase::AddMoveToSeq(const Move& move, size_t ply)
   {
       GameInfo info = GetLastBoardInfo(ply);
       info.UpdateBoardInfo(move, m_Board.GetPiece(move.to()));
       StoreInfoAtPly(ply, info);
   }

   // Null-move counterpart: no Move to derive info from, so snapshot the
   // board's current GameInfo directly (the board has already had
   // DoNullMove() applied by the caller before this is called).
   void PlayerAiBase::AddNullMoveToSeq(size_t ply)
   {
       StoreInfoAtPly(ply, m_Board.GetGameInfo());
   }
   ```
   Add `void AddNullMoveToSeq(size_t ply);` and `void StoreInfoAtPly(size_t ply, const GameInfo& info);`
   declarations to `PlayerAI.h` next to the existing `AddMoveToSeq` declaration.
2. In `StratEngine/AIPerplex.cpp`'s `pvs()`, call the new method with the
   *current* `ply` (matching how `AddMoveToSeq(move, ply)` is always called
   with the pre-recursion ply, not `ply + 1`), right after `DoNullMove()`:
   ```cpp
   last_move_was_null_[ply + 1] = true;
   m_Board.DoNullMove();
   AddNullMoveToSeq(ply);
   int null_score = -pvs(depth - 1 - R, -beta, -beta + 1, ply + 1, false, tt, pv_table);
   ```
3. In the same function, change the reference to a value copy to eliminate
   the dangling-reference hazard described above:
   ```cpp
   GameInfo info = GetLastBoardInfo(ply);   // was: const GameInfo& info = ...
   ```
4. Add a regression test that actually exercises a successful null-move
   recursion (not just the guard predicate, and not just a guard-blocking
   zugzwang position): force `null_move_enabled = true` via the public
   `tuning()` accessor on a normal, material-rich position (e.g. the starting
   position) at a depth (6+) deep enough that some non-PV node is guaranteed
   to satisfy every guard and actually call `DoNullMove()`/recurse, then
   assert `GetMove()` returns a non-null, legal move and does not throw.

**Validation:** `.\build.ps1 all`, then `.\build.ps1 extended-tests` — must
pass with zero failures (this is exactly the run that caught the bug
originally; it must go green after the fix).

### Task 4: Enable null-move pruning by default and validate via self-play

**Files:**
- Modify: `StratEngine/AIPerplex.h:58`

- [ ] **Step 1: Flip the default**

In `StratEngine/AIPerplex.h`, change:

```cpp
		bool null_move_enabled  = false; // disabled by default; opt-in after validation
```

to:

```cpp
		bool null_move_enabled  = true;  // enabled by default (validated via tests + self-play; see .claude/plans/null-move-pruning.md)
```

- [ ] **Step 2: Full build**

Run:
```
.\build.ps1 all
```
Expected: clean build, zero warnings.

- [ ] **Step 3: Full test suite**

Run:
```
.\build.ps1 extended-tests
```
Expected: all tests pass (including the Task 1-3 additions and every
pre-existing tactical/search/board case).

- [ ] **Step 4: Self-play validation**

Use the `self-play-validate` skill to run AIPerplex vs AIPerplex with the new
default. Confirm: the game completes (checkmate/stalemate/draw), no crashes,
PVs remain stable across iterations, and node counts at comparable depths are
lower than the pre-NMP baseline (check `logs/SimplePerfStats.txt`).

- [ ] **Step 5: Commit**

```bash
git add StratEngine/AIPerplex.h
git commit -m "feat: enable null-move pruning by default"
```

---

## Self-Review Notes

- **Spec coverage**: all six design-decision items (integration point,
  extracted guard helper, zugzwang guard, mate-score guard, consecutive-null
  guard, `Board::DoNullMove` EP fix) map to Task 1 or Task 2. Rollout maps to
  Task 4. Tactical/self-play validation maps to Task 3 and Task 4.
- **Type consistency**: `should_try_null_move(int depth, int beta, int ply, bool is_pv_node, bool in_check) const`
  is used identically in its declaration (Task 1, `AIPerplex.h`), its
  definition (Task 1, `AIPerplex.cpp`), its call site inside `pvs()` (Task 1),
  and every test call through `AIPerlexTestFixture::try_null_move` (Task 1).
  `last_move_was_null_[MAX_PLY]` is declared once and referenced with
  matching name everywhere (`pvs()`, `clear_null_move_flags()`, the test
  fixture's direct friend access).
