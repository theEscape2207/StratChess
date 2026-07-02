# De-Singleton Board Implementation Plan

> **For agentic workers:** execute phase-by-phase; every phase must build (`.\build.ps1 all`) and pass the fast test tier before its commit. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the `Board::Instance()` global singleton so that multiple independent `Board` objects can exist (the prerequisite for thread-local board copies in ThreadData / Lazy SMP), threading `Board&` explicitly through `MoveGenerator`, `EvalManager`, the player class hierarchy, and all runners/tests.

**Architecture:** Incremental bottom-up refactor — `Board` first becomes an ordinary copyable class while `Instance()` temporarily survives; then each consumer layer (MoveGenerator → Eval → Players → owners → tests) is converted to explicit `Board&` parameters; `Instance()` is deleted last. Every phase is a separately committed, green-building, test-passing state.

**Scope limits (explicitly out of scope):**
- No `ThreadData` extraction (separate roadmap item, follows this one)
- No from-scratch `Position` class (per prior decision: incremental Board refactor)
- No behavioral change to search, eval, or move generation — bit-identical single-threaded behavior
- No merge of the `GameInfo` parameter into `Board` in MoveGenerator signatures (YAGNI; revisit with ThreadData)
- `StratEngine/Archived/` untouched (not compiled)

---

## Design Decisions

1. **Keep `Instance()` alive until the final phase.** With ~50 engine call sites + ~90 test call sites, a big-bang change is unreviewable and undebuggable. Each phase converts one layer; not-yet-converted callers pass `Board::Instance()` explicitly as the argument, so the build stays green after every commit. Phase 7 deletes `Instance()` and the compiler proves nothing was missed.

2. **Bottom-up conversion order** (MoveGenerator → Eval → Players → owners): converting a callee's signature forces its callers to name a board; callers that already hold `m_Board` (all `PlayerAiBase` descendants) get their final form immediately, everything else temporarily passes `Instance()`.

3. **Board becomes copyable (`= default`).** Required by the follow-up ThreadData item (`Board board` member per thread). All members are value types (`std::array`, `std::vector`, scalars) — default copy semantics are correct. Copy is *never used in this task's hot paths*; it exists and is unit-tested so ThreadData doesn't have to re-open Board.

4. **Zobrist key tables stay global; initialization becomes idempotent + thread-safe.** The tables (`zobrist::piece_keys` etc.) must be identical for every Board instance — otherwise TT entries computed by different threads' boards would disagree. Today `zobrist::initialize()` is called from every `Board` ctor (Board.cpp:49) and unconditionally refills the tables; with many boards (and later, boards constructed on different threads) that is a re-init and a data race. Fix: wrap the fill in a function-local `static` lambda-call (C++ magic static — thread-safe, runs once).

5. **`GetBitBoards()` becomes `const`, returning `std::span<const BITBOARD>`.** Verified: every caller is read-only (AIPerplex.cpp:941, Eval.cpp:95, MoveGenerator.cpp:31/272/528/625). This is what allows `MoveGenerator` and `Eval` to take `const Board&`. The non-const overload is removed entirely.

6. **Parameter conventions:** `const Board&` as *first* parameter for stateless readers (`MoveGenerator`, `EvalManager::Evaluate`); mutable `Board&` for actors that DoMove/UndoMove (players, UCI handler, perft). References only — no `Board*`, no copies in any search path.

7. **Ownership:** each execution context owns exactly one `Board` by value — `Game` gains a `Board board_` member, `UciHandler` gains a `Board board_` member, the perft/tactical/FEN runners in `StratChessEvolved.cpp` create function-local boards, every `TEST_CASE` constructs its own local board. This kills the global-state-bleed problem that today forces the "every TEST_CASE must call SetupFromFEN first" isolation rule.

8. **Convenience constructor** `explicit Board(const std::string& fen)` — collapses the ubiquitous two-line `Board b; b.SetupFromFEN(fen);` test pattern. Default ctor keeps its current meaning (empty board, all squares `NO_PIECE`).

9. **Dead legacy test headers get deleted**, not migrated: `StratEngine/Tests/Unittests.h`, `StratEngine/Tests/RepetitionTests.h`, `StratEngine/Tests/Perft_unittests.h` are documented as retired in Roadmap.md (Catch2 migration, March 2026), reference only each other, and contain 15 `Board::Instance()` uses that would otherwise pollute the final grep check.

10. **`PlayerAiBase`'s default constructor is removed** — it exists only to satisfy "needs to be there" (its own comment) and binds `m_Board` to the singleton. With injection there is no meaningful default. If something actually needs it, that something also needs a board.

11. **Performance expectation: neutral to slightly positive.** `Board::Instance()` is a magic static — every call pays an initialization-guard check, including per-generated-move calls like `AddOfficerMoves`'s `GetPiece(to)` (MoveGenerator.cpp:301). Passing a reference removes that. No new indirection is introduced anywhere. Measured, not assumed: NPS baseline before Phase 1, comparison after Phase 7 (Validation Plan).

---

## Files Changed

**StratEngine (engine):**
| File | Change |
|---|---|
| `Board.h` / `Board.cpp` | public ctor, FEN ctor, copyable, const `GetBitBoards`, thread-safe zobrist init, `InCheck` passes `*this`; `Instance()` deleted in Phase 7 |
| `MoveGenerator.h` / `.cpp` | `const Board&` first param on `ComputeLegalMoves`, `ComputeCaptures`, `GetAttackBoard` + private helpers |
| `Eval.h` / `Eval.cpp` | `Evaluate(const Board&)` pure-virtual + both overrides |
| `PlayerBase.h` / `PlayerBase.cpp` | `Create(type, max_depth, Board&)` factory |
| `PlayerAI.h` / `PlayerAI.cpp` | `PlayerAiBase(Board&, unsigned)`; default ctor removed; `Quiescent`/`GetBestMove` call sites |
| `AIPerplex.h` / `AIPerplex.cpp` | ctor gains `Board&`; MoveGenerator/Eval call sites use `m_Board` |
| `AIBasic.h/.cpp`, `AIAgent.h/.cpp`, `ABIterative.h/.cpp` | same ctor + call-site pattern |
| `PlayerHuman.h` / `PlayerHuman.cpp` | ctor gains `Board&`, stores `board_` ref; 4 call sites |
| `Game.h` / `Game.cpp` | `Board board_` member; 4 call sites + factory call + Config call |
| `Config.h` / `Config.cpp` | `ReadConfigFile`/`ReadBoardSetup`/`ReadFEN` gain `Board&` param |
| `UCIHandler.h` / `UCIHandler.cpp` | `Board board_` member; 8 call sites + factory call |
| `Tests/Perft.h` / `Perft.cpp`, `Tests/PerftRunner.cpp` | thread board through internal ComputeLegalMoves calls; local board in runners |
| `Tests/TacticalTestRunner.h` / `.cpp` | local board per position, passed to factory |
| `Tests/Unittests.h`, `Tests/RepetitionTests.h`, `Tests/Perft_unittests.h` | **deleted** (retired, dead) |

**StratChessEvolved (app):** `StratChessEvolved.cpp` — local boards in `test_fen_integration` (line 29) and `perftrunner` (line 147).

**StratChessTests:** `BoardTests.cpp`, `BoardMoveTests.cpp`, `BoardStateTests.cpp`, `BoardApiTests.cpp`, `EvalTests.cpp`, `MoveFormatterTests.cpp`, `PerftTests.cpp`, `RepetitionTests.cpp`, `SearchTests.cpp`, `SortTests.cpp`, `TacticalTests.cpp`, `TacticalFullTests.cpp`, `TacticalTestHelpers.h` — all migrated to local boards; **new** `BoardInstanceTests.cpp` (`[board_instance]`) + `.vcxproj`/`.filters` entries.

**Docs:** `Docs/Roadmap.md`, `Docs/TestDesign.md` (Phase 2 status, isolation rules), `CLAUDE.md` ("AIPerplex in tests" snippet, Key Source Files).

**Not touched:** `StratEngine/Archived/*` (not compiled), `MoveFormatter.h/.cpp` (already takes `const Board&`), `Sort.h/.cpp`, `MoveHelper.h`, `TranspositionTable.*` (no Board dependency).

---

## Step-by-Step Changes

### Phase 0 — Baseline measurements (no code change)

- [x] **0.1** Record perft NPS baseline (move generation + Do/UndoMove throughput), 3 runs:
  ```bash
  ./x64/Release/StratChessEvolved.exe perft run 6
  ```
  Save the three NPS numbers into the PR notes / this plan's Validation section.

  **Baseline captured 2026-07-01** (after Phase 1 committed, before Phase 2 starts touching MoveGenerator):
  | Run | Nodes | Time | NPS |
  |---|---|---|---|
  | 1 | 119,060,324 | 3922 ms | 30,357,043 |
  | 2 | 119,060,324 | 3895 ms | 30,567,477 |
  | 3 | 119,060,324 | 3915 ms | 30,411,321 |

  Node count is deterministic and identical across runs (as expected). NPS baseline ≈ 30.4M/s avg. Phase 7 final comparison acceptance band: ±3% → 29.5M–31.4M/s.
- [x] **0.2** Record search baseline: run `[tactical_full]` and note total wall time; run one self-play move set (`"type": 6` both sides) and note 3–4 `GetMove complete: ... nodes=..., time=...ms` lines from stdout.

  **Self-play baseline captured 2026-07-01** (45 s window, `game_settings.json` already `"type": 6`/depth 15 both sides):
  ```
  move=e2-e4, score=15,  depth=15, time=4671ms,  nodes=11,432,010, stable=yes
  move=e7-e6, score=0,   depth=15, time=5102ms,  nodes=12,442,363, stable=yes
  move=d2-d4, score=11,  depth=15, time=5970ms,  nodes=14,748,434, stable=yes
  move=d7-d5, score=-1,  depth=15, time=7201ms,  nodes=18,006,852, stable=yes
  move=b1-d2, score=10,  depth=15, time=13423ms, nodes=33,641,058, stable=NO
  move=c8-d7, score=-2,  depth=15, time=5785ms,  nodes=14,526,252, stable=yes
  ```
  Depth 15 reached consistently, node counts and timings in the expected range, stability behaving normally (one `stable=NO` is a normal instability event, not a bug). This shape (move/score/depth/nodes/stability distribution) is the qualitative reference for Phase 7's final self-play check — not run to completion, so no `[tactical_full]` wall-time number was captured this pass (deferred to Phase 7's full validation run).
- [x] **0.3** Commit this plan file.

### Phase 1 — Board becomes an ordinary copyable class (Instance() survives)

**Files:** `StratEngine/Board.h`, `StratEngine/Board.cpp`, new `StratChessTests/BoardInstanceTests.cpp`, `StratChessTests/StratChessTests.vcxproj` + `.filters`

- [x] **1.1** `Board.h`: make the constructor public, add the FEN convenience ctor, default all copy/move operations, mark `Instance()` as temporary:
  ```cpp
  public:
      // TEMPORARY during de-singleton migration — deleted in the final phase.
      static inline Board& Instance() noexcept
      {
          static Board _instance;
          return _instance;
      }

      Board();                                   // empty board (all squares NO_PIECE)
      explicit Board(const std::string& fen);    // constructs, then SetupFromFEN(fen)
      ~Board() = default;

      Board(const Board&) = default;
      Board& operator=(const Board&) = default;
      Board(Board&&) = default;
      Board& operator=(Board&&) = default;
  ```
  Remove the old `private:` ctor/dtor block and the deleted copy/move declarations under the "Non-copyable / non-movable (singleton)" comment (Board.h:99-107).
- [x] **1.2** `Board.h`: replace the non-const accessor (line 87) with a const one:
  ```cpp
  std::span<const BITBOARD> GetBitBoards() const noexcept;
  ```
- [x] **1.3** `Board.cpp`: implement both:
  ```cpp
  Board::Board(const std::string& fen) : Board()
  {
      SetupFromFEN(fen);
  }

  std::span<const BITBOARD> Board::GetBitBoards() const noexcept
  {
      return { bitboards_.data(), bitboards_.size() };
  }
  ```
  Fix any caller that stored the span in a non-const context (compiler will list them; all verified read-only).
- [x] **1.4** `Board.cpp`: make `zobrist::initialize()` run-once and thread-safe — wrap the existing table-filling body:
  ```cpp
  void initialize() noexcept
  {
      static const bool once = [] {
          // ... existing rng + table-filling body, unchanged ...
          return true;
      }();
      (void)once;
  }
  ```
- [x] **1.5** New `StratChessTests/BoardInstanceTests.cpp`, tag `[board_instance]`. Deliberately does **not** use `MoveGenerator` — at this phase the generator still reads the singleton, so generated moves would not belong to the local boards under test. A hand-built quiet pawn push (legal from the start position) exercises DoMove/UndoMove instead:
  ```cpp
  #include <catch2/catch_amalgamated.hpp>
  #include "Board.h"
  #include "Move.h"

  static_assert(std::is_copy_constructible_v<Board>);
  static_assert(std::is_copy_assignable_v<Board>);

  namespace {
  constexpr auto kStartFEN =
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  constexpr auto kKiwipeteFEN =
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

  // e2-e3: plain quiet pawn push, legal from the start position, no generator needed
  Move make_e2e3() { return MoveFactory::MakeMove(e2, e3, MoveType::QUIET); }
  }

  TEST_CASE("Two boards hold independent state", "[board_instance]") {
      Board a(kStartFEN);
      Board b(kKiwipeteFEN);
      REQUIRE(a.get_zobrist_hash() != b.get_zobrist_hash());
      REQUIRE(a.ExtractFEN() != b.ExtractFEN());
  }

  TEST_CASE("Mutating one board does not affect another", "[board_instance]") {
      Board a(kStartFEN);
      Board b(kStartFEN);
      const auto hashA = a.get_zobrist_hash();
      REQUIRE(b.DoMove(make_e2e3()));
      REQUIRE(a.get_zobrist_hash() == hashA);          // a untouched
      REQUIRE(b.get_zobrist_hash() != hashA);          // b advanced
  }

  TEST_CASE("Same FEN yields same zobrist hash across instances", "[board_instance]") {
      Board a(kKiwipeteFEN);
      Board b(kKiwipeteFEN);
      REQUIRE(a.get_zobrist_hash() == b.get_zobrist_hash());   // global key tables shared
  }

  TEST_CASE("Copied board equals original and diverges independently", "[board_instance]") {
      Board original(kStartFEN);
      Board copy = original;
      REQUIRE(copy.get_zobrist_hash() == original.get_zobrist_hash());
      REQUIRE(copy.ExtractFEN() == original.ExtractFEN());

      const Move m = make_e2e3();
      REQUIRE(copy.DoMove(m));
      REQUIRE(copy.get_zobrist_hash() != original.get_zobrist_hash());
      copy.UndoMove(m);
      REQUIRE(copy.get_zobrist_hash() == original.get_zobrist_hash());
  }
  ```
  (Verify `MoveFactory::MakeMove` spelling/location against `Move.h` at implementation time; MoveGenerator.cpp:304 is the reference usage.)
- [x] **1.6** Add `BoardInstanceTests.cpp` to `StratChessTests.vcxproj` (`ClCompile`) **and** `.vcxproj.filters`.
- [x] **1.7** `.\build.ps1 all` + fast tests green → commit: `De-singleton Board phase 1: Board constructible, copyable, const GetBitBoards`

### Phase 2 — Thread `const Board&` through MoveGenerator

**Files:** `MoveGenerator.h`, `MoveGenerator.cpp`, `Board.cpp` (+ every caller)

- [x] **2.1** `MoveGenerator.h`: forward-declare `class Board;` and change signatures — public:
  ```cpp
  static void ComputeCaptures(_In_ const Board& board, _In_ const GameInfo& info, _Inout_ MoveList& moveList);
  static void ComputeLegalMoves(_In_ const Board& board, _In_ const GameInfo& info, _Inout_ MoveList& moveList);
  static BITBOARD GetAttackBoard(const Board& board, eColor) noexcept;
  ```
  private (only those that touch the board beyond the `bbBitBoards` pointer they already receive):
  ```cpp
  static void AddOfficerMoves(const Board& board, MoveList& moveList, BITBOARD bbAttack, eSquare from);
  static void AddPawnCaptures([[maybe_unused]] const Board& board, MoveList& moveList, const BITBOARD*, Move pawnMove, eColor color);
  static void AddCastleMoves(const Board& board, MoveList& moveList, eColor color, const BITBOARD* bbBitBoards, const GameInfo& info);
  ```
  `GetAnyEnPassantAttackingPawns` (line 625 uses `Instance()` only for bitboards): changed to take `const BITBOARD* bbBitBoards` as first param instead of a Board. Both `IsAttacked` overloads had zero callers (both marked "unused"/"currently unused" in their own comments) — **deleted** rather than converted, per the plan's contingency.

  **Deviation from the plan found during implementation**: `GeneratePawnCaptures` forwards to `AddPawnCaptures` (which now needs `const Board&`), so it also gained a `const Board&` first param — the same forwarding case as `GenerateOfficerMoves`→`AddOfficerMoves`, just missed in the original private-function list above.

  **`[[maybe_unused]]` needed on `AddPawnCaptures`'s `board` param**: every use of `board` inside that function is inside an `assert(...)` call, and Release builds define `NDEBUG` (asserts compile to nothing) — confirmed via `grep NDEBUG StratChessEvolved.vcxproj`. Without the attribute this is an unused-parameter warning under `/W4 /WX`. Matches the existing project convention (see `AIPerplex.h:190-191` / `AIPerplex.cpp:1121-1122` for the same pattern) and the approved-suppression rule in `CLAUDE.md`.
- [x] **2.2** `MoveGenerator.cpp`: replace every `Board::Instance()` (lines 29, 31, 270, 272, 301, 372, 412, 424, 527, 548, 625) with the `board` parameter / passed-down `bbBitBoards`. Representative:
  ```cpp
  void MoveGenerator::ComputeLegalMoves(_In_ const Board& board, _In_ const GameInfo& info, _Inout_ MoveList& moveList)
  {
      assert(moveList.empty());
      const auto color  = board.GetCurrentColor();
      const auto boards = board.GetBitBoards();
      ...
      GenerateOfficerMoves(board, boards.data(), moveList, KNIGHT, color, false);
      ...
  }
  ```
  Note: `GenerateOfficerMoves` forwards to `AddOfficerMoves` (which needs `board.GetPiece`), so it also gains the `const Board&` first param.
- [x] **2.3** `Board.cpp:587` (`InCheck`): `MoveGenerator::GetAttackBoard(*this, byColor);` — this removes the Board→singleton cycle.
- [x] **2.4** Update all callers (compiler-driven; pass what is in scope):
  - `AIPerplex.cpp:353, 586, 973` → `m_Board` (final form)
  - `PlayerAI.cpp:55` (`Quiescent`) → `m_Board` (final form)
  - `AIAgent.cpp:97`, `AIBasic.cpp:61`, `ABIterative.cpp:97` → `m_Board` (final form)
  - `PlayerHuman.cpp:155` → `Board::Instance()` (temporary, finalized Phase 4)
  - `Tests/Perft.cpp:170, 200, 252, 317` → the `Board&` these functions already receive as parameter
  - `StratChessTests`: `BoardTests.cpp:117,134`, `SortTests.cpp:41,70,95,125,164` → the local `Board& board = Board::Instance();` already in scope in each `TEST_CASE` (final form for those locals; the `Board::Instance()` binding itself is migrated in Phase 6). `SearchTests.cpp:83` has no local board (uses `Board::Instance()` directly, temporary). `BoardInstanceTests.cpp` has no MoveGenerator calls by design.
- [x] **2.5** Build + fast tests + `[perft]` green → commit: `De-singleton Board phase 2: MoveGenerator takes explicit Board&`

  **Validation results**: full build (both configs, Level4+/WX) clean. Fast tier: 1557 assertions / 129 test cases, unchanged from Phase 1. Deep perft (`Tests/perft_test_cases.json`): 640/640 passed, node counts identical to pre-Phase-2, NPS ≈31-33M/s per position (in line with or above the Phase 0 baseline — no regression from removing the `Board::Instance()` magic-static overhead per move-generation call).

  **Pre-existing tactical suite finding (not a regression)**: `StratChessEvolved.exe tactical test` returns 7/8 (87%, below the 90% documented threshold) — position `QFORK-001` fails. Verified via `git stash` that this **also fails identically at the Phase 1 commit**, before any MoveGenerator changes — so it predates this refactor and is unrelated to it. `Docs/TestDesign.md` currently (incorrectly) documents this suite as 8/8. Flagged as a separate background task (task_83e8a759) rather than fixed here, since fixing search/eval behavior is out of this plan's scope (Design Decision: "No behavioral change to search, eval, or move generation").

### Phase 3 — Thread `const Board&` through EvalManager

**Files:** `Eval.h`, `Eval.cpp`, `AIPerplex.cpp`, `PlayerAI.cpp`, `StratChessTests/EvalTests.cpp`

- [x] **3.1** `Eval.h`: forward-declare `class Board;`, change the interface:
  ```cpp
  virtual int Evaluate(const Board& board) const = 0;
  ```
  and both overrides in `EvalSimple` / `EvalComplex`.
- [x] **3.2** `Eval.cpp`: delete `const Board& board = Board::Instance();` (line 32) and `Board& board = Board::Instance();` (line 80) — `board` is now the parameter. Body otherwise unchanged (`GetBitBoards()` at :95 now returns the const span).
- [x] **3.3** Callers: `AIPerplex.cpp:540, 571` and `PlayerAI.cpp:39` → `Eval->Evaluate(m_Board)` (final form).
- [x] **3.4** Migrate `EvalTests.cpp` to its final non-singleton form now (it is the only test file whose call sites this phase breaks):
  ```cpp
  Board board(fen);
  const int score = EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board);
  ```
  The one test reusing a single board across two FENs ("penalises doubled pawns...") uses `Board board;` (default ctor) + two sequential `SetupFromFEN` calls instead of the one-line FEN ctor, since it needs the same object reconfigured twice.
- [x] **3.5** Build + fast tests (`[eval]` in particular) green → commit: `De-singleton Board phase 3: EvalManager::Evaluate takes explicit Board&`

  **Validation results**: full build (both configs, Level4/WX) clean. `[eval]`: 11 assertions / 8 test cases, unchanged. Full fast tier: 1557 assertions / 129 test cases, unchanged from Phase 2. Deep perft: 640/640, unchanged (this phase doesn't touch move generation, included as a sanity check).

### Phase 4 — Inject Board through the player construction chain

**Files:** `PlayerBase.h`, `PlayerBase.cpp`, `PlayerAI.h`, `AIPerplex.h/.cpp`, `AIBasic.h/.cpp`, `AIAgent.h/.cpp`, `ABIterative.h/.cpp`, `PlayerHuman.h/.cpp` + factory callers

- [x] **4.1** `PlayerBase.h`:
  ```cpp
  static std::unique_ptr<PlayerBase> Create(ePlayerTypes type, unsigned max_depth, Board& board);
  ```
  (forward-declare `class Board;`)
- [x] **4.2** `PlayerAI.h` (`PlayerAiBase`): constructor injection, delete the default ctor:
  ```cpp
  protected:
      explicit PlayerAiBase(Board& board, unsigned md) :
          m_Board(board),
          max_depth_(md)
      {
      }
  ```
  (`Board& m_Board;` member and all 26 `m_Board` uses in AIPerplex.cpp are already correct.)
- [x] **4.3** Each AI ctor gains `Board&` and forwards: `AIPerplex(Board& board, unsigned md) : PlayerAiBase(board, md) ...` (AIPerplex.h:30, AIPerplex.cpp:67), same for `AIBasic`, `AIAgent`, `ABIterative`.

  **Deviation from the plan found during implementation**: `AIAgent`/`ABIterative` don't derive directly from `PlayerAiBase` — they go through an intermediate `PlayerAiIterBase` (`PlayerAiIterBase.h`, not in the plan's file list), whose constructor also needed the `Board&` forwarding treatment: `explicit PlayerAiIterBase(Board& board, unsigned md) : PlayerAiBase(board, md) { ... }`.
- [x] **4.4** `PlayerHuman`: ctor gains `Board& board`, store `Board& board_;` member; replace `Board::Instance()` at PlayerHuman.cpp:19, 109, 155 (from Phase 2), 161 with `board_`.

  **Deviation from the plan found during implementation**: `IsAnyLegalMoves` is a `static` method (no `board_` member to reach), and it calls the mutating `Board::IsLegalMove()`, so it needed its own `Board& board` parameter rather than just reading `board_` — `static bool IsAnyLegalMoves(Board& board, const GameInfo& info, MoveList& moveList)`. Its lambda at PlayerHuman.cpp:161 captures `board` by reference.
- [x] **4.5** `PlayerBase.cpp:16`: factory passes `board` into every branch of the switch.
- [x] **4.6** Factory callers (temporary singleton args, finalized in Phase 5/6):
  - `Game.cpp:176` → `PlayerBase::Create(type, config.depth, Board::Instance())`
  - `UCIHandler.cpp:43` → same pattern
  - `Tests/TacticalTestRunner.cpp:53` → same pattern
  - `StratChessTests/TacticalTestHelpers.h:20` (`make_tactical_engine` helper — passes `Board::Instance()` internally, function signature unchanged for now), `SearchTests.cpp:46` → same pattern
- [x] **4.7** Build + fast tests green → commit: `De-singleton Board phase 4: players receive Board by injection`

  **Validation results**: full build (both configs, Level4/WX) clean on first attempt. Full fast tier: 1557 assertions / 129 test cases, unchanged. Deep perft: 640/640, unchanged. Self-play sanity checks (base classes `PlayerAI`/`PlayerBase` changed, so both hierarchies per CLAUDE.md's rule): AIAgent (`"type": 3`, 20 s) ran cleanly — legal move sequences, increasing search depths, correct board printout; AIPerplex (`"type": 6`, 25 s, default config) reproduced the **exact same node counts** as the Phase 0 baseline (11,432,010 / 12,442,363 / 14,748,434 for the first three moves) — fully deterministic, no behavioral drift. `game_settings.json` was temporarily edited for the AIAgent run and restored byte-for-byte afterward (verified via `git status`).

### Phase 5 — Owners own their boards (engine code singleton-free)

**Files:** `Game.h/.cpp`, `Config.h/.cpp`, `UCIHandler.h/.cpp`, `StratChessEvolved.cpp`, `Tests/Perft.cpp`, `Tests/PerftRunner.cpp`, `Tests/TacticalTestRunner.cpp`

- [x] **5.1** `Game.h`: add `Board board_;` as the **first** data member (players hold a reference into it — it must be constructed before and destroyed after `m_pPlayers`). `Game.cpp`: replace `Board::Instance()` at lines 93, 243, 368, 398 with `board_`; factory call passes `board_`; config call passes `board_`.
- [x] **5.2** `Config.h/.cpp`: `ReadConfigFile(const std::string& filename, Board& board)`, `ReadBoardSetup(const json& config, Board& board)`, `ReadFEN(const std::string& fen, Board& board)` — replaces `Board::Instance()` at Config.cpp:33, 41, 47.
- [x] **5.3** `UCIHandler.h`: add `Board board_;` member (declared before `ai_`). `UCIHandler.cpp`: replace lines 69, 75, 84, 95, 97, 102, 111, 112 with `board_`; `init_ai()` passes `board_` to the factory.
- [x] **5.4** `StratChessEvolved.cpp`: `test_fen_integration` (line 29) and `perftrunner` (line 147) use a function-local `Board board;`.
- [x] **5.5** `Tests/Perft.cpp:369` and `Tests/PerftRunner.cpp:48` (the two `// TODO: Update when Board is no longer singleton` sites): local `Board board;` — and delete those TODO comments.

  **Note**: `Tests/PerftRunner.cpp`'s `perftrunner_main()` was confirmed to have zero callers anywhere in the codebase (dead code, superseded by the `perftrunner()` static function in `StratChessEvolved.cpp`) — still fixed since it's part of the compiled build and must stay Level4/WX-clean, but left in place rather than deleted (out of scope for this refactor).
- [x] **5.6** `Tests/TacticalTestRunner.cpp:57-58`: local `Board board(pos.fen);` per position, passed to the factory; update the stale "Sets up Board::Instance() internally" comment in `TacticalTestRunner.h:37`.
- [x] **5.7** Verify engine is singleton-free:
  ```bash
  grep -rn "Board::Instance" StratEngine/ StratChessEvolved/ --include=*.cpp --include=*.h | grep -v Archived
  ```
  Expected: only `Board.h` (the accessor itself) and the three retired `Tests/*` headers (deleted next phase).

  **Confirmed**: exactly the 14 expected hits, all in `Perft_unittests.h` (6) and `RepetitionTests.h` (8) — nothing in `Board.h` itself (the accessor definition doesn't contain the literal call-site string) and nothing else anywhere in engine or app code.
- [x] **5.8** Build + fast tests + a quick self-play sanity game green → commit: `De-singleton Board phase 5: Game/UCI/runners own their boards`

  **Validation results**: full build (both configs, Level4/WX) clean. Full fast tier: 1557/129, unchanged. Deep perft: 640/640, unchanged. Tactical suite: 7/8 (87%) — same pre-existing NMP issue as Phase 2/before (issue #66), not a regression. UCI smoke test (`UciHandler` now owns `board_` instead of the singleton — first phase this could plausibly break): `position startpos moves e2e4` + `go depth 8` returned a well-formed `bestmove e7e6` with sane `info depth 8 score cp -19 nodes 134273` line. Self-play (AIPerplex, `"type": 6`, 25 s): reproduced the exact same node counts as the Phase 0/4 baseline (11,432,010 / 12,442,363 / 14,748,434) — fully deterministic.

### Phase 6 — Migrate the test suite; delete dead legacy headers

**Files:** all 12 `StratChessTests/*.cpp` listed above, `TacticalTestHelpers.h`; delete `StratEngine/Tests/Unittests.h`, `RepetitionTests.h`, `Perft_unittests.h`

- [x] **6.1** Mechanical per-file migration rule:
  - `Board::Instance().SetupFromFEN(fen);` → `Board board(fen);`
  - every subsequent `Board::Instance().X(...)` in that `TEST_CASE` → `board.X(...)`
  - MoveGenerator calls gain the `board` argument
  - order: `Board` local must be declared **before** any `PlayerBase::Create(..., board)` and any `GetGameInfo()` snapshot

  **Deviation from the plan found during implementation**: several files reuse a single `Board` object across multiple `SECTION`s or across sequential setup calls within one `TEST_CASE` (e.g. `MoveFormatterTests.cpp`'s `ToShort`/`ToVerbose`/`FromUCI` suites, `BoardStateTests.cpp`'s fifty-move-counter tests). These use `Board board;` (default ctor) once, with the existing per-`SECTION` `board.SetupFromFEN(...)` calls left untouched — the same pattern already established in Phase 3 for `EvalTests.cpp`'s doubled-pawn test. `SearchTests.cpp` needed a different treatment (see 6.3).
- [x] **6.2** `TacticalTestHelpers.h`: factory helper takes the board:
  ```cpp
  inline std::unique_ptr<PlayerBase> make_tactical_engine(Board& board, unsigned depth)
  {
      auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth, board);
      AIPerplex::SetVerboseLogging(false);
      ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
      return ai;
  }
  ```
  Callers (`TacticalTests.cpp`, `TacticalFullTests.cpp`) construct `Board board(tc.fen + " w - - 0 1"-style full FEN);` first, then the engine, then `board.GetGameInfo()`. `TacticalFullTests.cpp`'s NMP-comparison test (which runs the search twice, once with NMP disabled and once enabled) uses two independent `Board` objects (`board_disabled`/`board_enabled`), each fed the same starting FEN — preserves the original "fresh identical starting position for both searches" semantics now that state isn't implicitly shared via the singleton.
- [x] **6.3** `SearchTests.cpp` fixture: give `AIPerlexTestFixture` (or the test setup around it) a `Board board_;` member declared **before** `ai_owner` so the reference outlives the AI.

  **Deviation from the plan found during implementation**: the fixture's constructor gained an optional `const std::string& fen` parameter (defaulting to the standard starting position already used by 7 of 8 `should_try_null_move` test cases), constructing `board_(fen)` in the initializer list. This let every standard-position test case drop its now-redundant explicit `Board::Instance().SetupFromFEN(...)` call entirely and just write `AIPerlexTestFixture fix;`; the one zugzwang test with a different FEN passes it explicitly: `AIPerlexTestFixture fix("8/8/8/3k4/8/3K4/3P4/8 w - - 0 1");`. The free-standing `AnyLegalMove()` helper (used by several `assess_iteration_quality` tests, doesn't touch the fixture) constructs its own local `Board`.
- [x] **6.4** Delete `StratEngine/Tests/Unittests.h`, `StratEngine/Tests/RepetitionTests.h`, `StratEngine/Tests/Perft_unittests.h`; remove their `ClInclude`/`Filter` entries from `StratEngine.vcxproj`(+`.filters`) if present.

  **Confirmed**: none of the three were referenced in any `.vcxproj`/`.vcxproj.filters` (they were never part of the compiled build — dead since the Catch2 migration, per Roadmap.md's "Retired" note). Only cross-reference was `Unittests.h` including `RepetitionTests.h`; nothing else in the codebase included any of the three. Deleted outright, no project-file changes needed.
- [x] **6.5** Full grep — zero hits outside `Board.h` and `Archived/`:
  ```bash
  grep -rn "Board::Instance" --include=*.cpp --include=*.h . | grep -v Archived
  ```
  **Confirmed**: single hit, a comment in `BoardInstanceTests.cpp` explaining *why* that file avoids `MoveGenerator` (not an actual call site). Zero hits in compiled code anywhere.
- [x] **6.6** `.\build.ps1 extended-tests` (all tiers incl. `[slow]`) green → commit: `De-singleton Board phase 6: tests construct boards directly; retire dead test headers`

  **Validation results**: full build (both configs, Level4/WX) clean on first attempt. Extended tests (all tiers incl. `[slow]`): 1611 assertions / 132 test cases, all green. Deep perft: 640/640, unchanged. Tactical suite: 7/8 (87%) — same pre-existing NMP issue (#66), not a regression. Self-play (AIPerplex, 25 s): reproduced the exact Phase 0 baseline node counts for the first four moves, confirming determinism held through the full test-suite migration.

### Phase 7 — Delete Instance(); documentation

**Files:** `Board.h`, `Docs/Roadmap.md`, `Docs/TestDesign.md`, `CLAUDE.md`

- [x] **7.1** Delete the `Instance()` accessor and its "TEMPORARY" comment from `Board.h`. Build both projects — the compiler is the completeness proof.

  **Confirmed**: both configs built clean with zero errors on first attempt after deletion — the compiler found no remaining callers anywhere.
- [x] **7.2** Doc updates:
  - `CLAUDE.md`: "AIPerplex in tests" snippet → board-first pattern (`Board board(fen);` + `Create(..., board)`); confirmed no other singleton mentions remained anywhere in the file.
  - `Docs/TestDesign.md`: Phase 2 marked done (with a summary of what landed); isolation rule replaced with "each TEST_CASE constructs its own Board"; NPS-regression-file item re-flagged as open (retargeted to Phase 1, since there's no more Phase 2 to defer to); the stale coverage-map row for a never-built `Position` class replaced with the real `[board_instance]` tag/file that Phase 1 actually added.
  - `Docs/Roadmap.md`: De-Singleton Board section removed from Critical Priority and a full "Completed Work" entry added (July 2026) summarizing what landed, validation performed, and what it unblocks; ThreadData's status line updated to "now unblocked"; the Magic Bitboards item's stale "De-Singleton Board above" cross-reference fixed (the section it pointed to no longer exists at that location); doc's `Last Updated` date bumped.

  **Note**: no PR number was available at doc-update time (PR not yet opened) — the Roadmap entry references the plan file and branch instead; update with the PR number once opened, per normal convention for other Completed Work entries that do cite a PR.
- [x] **7.3** Run the full Validation Plan below → commit: `De-singleton Board phase 7: remove Instance(); docs`

---

## Validation Plan

**Per phase (before each commit):**
```powershell
.\build.ps1 all           # Release|x64, both projects, warnings-as-errors
.\build.ps1 run-tests     # fast tier
```

**Final (after Phase 7, before PR) — all run 2026-07-02, all PASS:**
1. `cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"` — full build + extended `[slow]` tests + self-play. ✅ Full build PASS, extended tests PASS (1611 assertions/132 cases), self-play PASS (7 moves logged).
2. Deep perft (ground truth for movegen): `cd Tests && ../x64/Release/StratChessEvolved.exe perft test` — all cases must pass with identical node counts. ✅ 640/640, node counts identical throughout (e.g. starting position depth 6: 119,060,324, unchanged from Phase 0).
3. Tactical suite: `cd Tests && ../x64/Release/StratChessEvolved.exe tactical test` — 8/8. ⚠️ **7/8 (87%)** — the pre-existing null-move-pruning issue found during Phase 2 (QFORK-001, tracked in [issue #66](https://github.com/theEscape2207/StratChess/issues/66), root-caused to PR #55's NMP, confirmed unrelated to this refactor via `git stash` bisection against the Phase 1 commit). Not a regression from this work; not blocking.
4. Self-play **both** hierarchies (base classes changed): AIPerplex vs AIPerplex (`"type": 6`) *and* AIAgent (`"type": 3`) per the PlayerAI/PlayerBase rule in CLAUDE.md; run from `StratChessEvolved/`, verify clean termination and sane `GetMove complete` lines. ✅ Both clean; AIPerplex reproduced exact Phase 0 baseline node counts (fully deterministic); AIAgent produced legal, increasing-depth search output. `game_settings.json` restored byte-for-byte after the AIAgent run.
5. UCI smoke test (UCIHandler now owns the board): `(printf "uci\nisready\nposition startpos moves e2e4\ngo depth 8\nquit\n") | ./x64/Release/StratChessEvolved.exe` → well-formed `bestmove`. ✅ `bestmove e7e6`, `nodes 134273` — identical to the Phase 5 run.
6. **NPS comparison vs Phase 0 baseline**: `perft run 6` ×3 and `[tactical_full]` wall time. Acceptance: within ±3% (expected neutral-to-positive per Design Decision 11). Any regression beyond that must be explained before the PR opens. ✅ Baseline avg ≈30.4M NPS (30,357,043 / 30,567,477 / 30,411,321); final avg ≈30.7M NPS (31,414,333 / 31,282,271 / 29,536,175) — **+1%, within band, slightly positive** as predicted. Node counts bit-identical in every run.
7. Dispatch **search-reviewer** if any `AIPerplex.cpp` diff went beyond mechanical `m_Board`/`Evaluate(m_Board)` argument threading (per pre-PR checklist; eval-reviewer likewise for `Eval.cpp` — expected mechanical-only). ✅ Verified via `git diff origin/main...HEAD -- StratEngine/AIPerplex.cpp StratEngine/Eval.cpp StratEngine/Eval.h`: every hunk is either a constructor-signature change or an added `m_Board`/`board` argument — zero logic changes. **Reviewer dispatch skipped** — condition for triggering it was not met.

---

## Key Correctness Properties

1. **Zobrist key identity across instances** — the global key tables are filled exactly once (thread-safe); two boards loaded from the same FEN produce the same hash (`[board_instance]` test). Without this, a future shared TT across thread boards is silently corrupt.
2. **Perft equivalence** — node counts at every depth are bit-identical before/after (fast `[perft]` + deep suite). Move generation semantics must not change.
3. **Search determinism preserved** — fixed-depth searches return the same best move and node counts as before the refactor (`[tactical]`, `[tactical_full]`, `[search]` unchanged).
4. **No board copies in hot paths** — `Board` is passed by reference everywhere in this task; the copy ctor exists solely for the ThreadData follow-up and is exercised only in `[board_instance]` tests.
5. **Reference lifetime safety** — every `Board&` stored in a player outlives the player: `Game::board_` and `UciHandler::board_` are declared before the players/AI members they feed; test fixtures declare the board before the engine.
6. **Singleton fully gone** — `grep -rn "Board::Instance"` matches nothing outside `Archived/` after Phase 7; the compiler enforces it because the accessor no longer exists.
7. **Test isolation upgraded** — no shared board state between TEST_CASEs; the old "must SetupFromFEN first" footgun is structurally impossible.
