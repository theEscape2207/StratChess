# StratChessEvolved — Test Design

**Owner**: Thees
**Related**: `Docs/Roadmap.md` — consult before adding tests for a new area

---

## Philosophy

Tests are **protection tools for the roadmap**, not bureaucratic overhead.

- **Fast feedback above all**: the fast Catch2 tier must run in < 10 seconds (Release build). The
  `[slow]` tier is deliberate, and runs pre-PR and nightly.
- **Tests protect the next change, not the last one**: write tests just before touching a component, especially for high-risk roadmap items (LMR, MoveSorter, parallel search).
- **Direction over precision**: for evaluation and search, verify the *direction* and *relative magnitude* of results. Exact centipawn values are fragile and mislead.
- **Perft is the ground truth for move generation**: do not replace it with unit tests — extend it with more positions.

---

## Test Tiers

| Tier | Executable | Tag / command | Max time | Trigger |
|------|-----------|---------------|----------|---------|
| Unit & fast integration | `StratChessTests.exe` | `~[slow]`, or `[tag]` | < 10 s | PR CI and local validation |
| Extended Catch2 | `StratChessTests.exe` | all, including `[slow]` | Minutes | Pre-PR and nightly |
| Deep perft | `StratChessEvolved.exe perft test` | — | Minutes | Release PR CI |
| Tactical stability | `StratChessEvolved.exe tactical stability 10` | one run: `tactical test` | Seconds | Pre-PR; 100 runs nightly |
| Corpus move-gen sweep | `Scripts\Run-PerftCheck.ps1` | — | ~25 min | On demand — see [the corpus sweep](#corpus-move-generation-sweep-perftcheck) |

Run all fast tests at once:
```bash
build/windows-clang-cl/StratChessTests.exe "~[slow]"
```

Run by tag:
```bash
build/windows-clang-cl/StratChessTests.exe [tt]
build/windows-clang-cl/StratChessTests.exe [eval]
build/windows-clang-cl/StratChessTests.exe [tactical]
build/windows-clang-cl/StratChessTests.exe [repetition]
build/windows-clang-cl/StratChessTests.exe [moves]
build/windows-clang-cl/StratChessTests.exe [perft]
build/windows-clang-cl/StratChessTests.exe [search]
build/windows-clang-cl/StratChessTests.exe [sort]
build/windows-clang-cl/StratChessTests.exe [time_mgr]
build/windows-clang-cl/StratChessTests.exe [board]
build/windows-clang-cl/StratChessTests.exe [board_moves]
build/windows-clang-cl/StratChessTests.exe [board_state]
build/windows-clang-cl/StratChessTests.exe [board_api]
```

The `[tactical_full]` suite is tagged `[slow]` and excluded from the default `~[slow]` run. Use `extended-tests` to include it:
```powershell
.\build.ps1 extended-tests          # all tests including [slow]
.\build.ps1 run-tests "[tactical_full]"  # slow tier only
```

---

## Coverage Map

| Component | Tag | File |
|-----------|-----|------|
| Move structure & sentinels | `[moves]` | `StratChessTests/MoveFieldTests.cpp` |
| Repetition detection | `[repetition]` | `RepetitionTests.cpp` |
| Move generation (perft d1–d4) | `[perft]` | `PerftTests.cpp` |
| Move generation (deep perft d5+) | — | `StratChessEvolved.exe perft test` |
| Move generation (142,953-position corpus, d1–d4) | — | `Scripts\Run-PerftCheck.ps1` |
| **MoveFormatter** | `[formatter]` | `MoveFormatterTests.cpp` |
| **TranspositionTable** | `[tt]` | `TTTests.cpp` |
| **Evaluation (EvalSimple/Complex)** | `[eval]` | `EvalTests.cpp` |
| **Search regression (tactical)** | `[tactical]` | `TacticalTests.cpp` |
| **Search regression (slow tier)** | `[tactical_full][slow]` | `TacticalFullTests.cpp` |
| Search helpers (assess_quality etc.) | `[search]` | `SearchTests.cpp` |
| Move ordering (Sort) | `[sort]` | `SortTests.cpp` |
| Board DoMove/UndoMove completeness | `[board]` | `BoardTests.cpp` |
| Board move-type round-trips (all types) | `[board_moves]` | `BoardMoveTests.cpp` |
| Board position-state lifecycle (ep, castling, clock, last move) | `[board_state]` | `BoardStateTests.cpp` |
| Fifty-move rule (clock advance/reset via DoMove, root, draw reporting) | `[fifty_move]` | `FiftyMoveRuleTests.cpp` |
| Board public query APIs + FEN round-trip | `[board_api]` | `BoardApiTests.cpp` |
| Time management (TimeManager + compute_budget) | `[time_mgr]` | `TimeManagerTests.cpp` |
| Sliding-piece attack generation (PEXT) | `[magic]` | `MagicBitboardTests.cpp` |
| UCI command loop | `[uci]` | `UCITests.cpp` (+ `StratChessEvolved.exe uci` pipe smoke test) |
| PV legality (`pv_replays_legally`) | `[pv]` | `PVIntegrityTests.cpp` |
| Full tactical suite (WAC/mate-in-N) | — | `StratChessEvolved.exe tactical test` |
| Board instance independence (post-de-singleton) | `[board_instance]` | `BoardInstanceTests.cpp` |
| Game loop outcome handling (`Game::Run`) | `[game]` | `GameLoopTests.cpp` |
| Human player's non-interactive terminal paths | `[player_human]` | `PlayerHumanTests.cpp` |
| External integer parsing (argv, JSON keys) | `[argparse]` | `ArgParseTests.cpp` |
| Settings-file parsing and its failure modes | `[config]` | `ConfigTests.cpp` |

---

## Existing component coverage

### TranspositionTable Tests (`[tt]`)

**File**: `StratChessTests/TTTests.cpp`
**Rationale**: The TT is fully self-contained (no Board dependency). Its correctness affects every depth of search — a silent TT bug could mask wins or produce ghost moves. Tests added now provide a safety net before the TT is stressed by LMR and parallel search.

**Test cases**:
- Store and probe: entry is retrievable by key
- Probe miss: unknown key returns `nullopt`
- Same-key store: the incoming entry is scored against the one it would replace — a store that
  outranks it wins (ties included), a quiescence store or a shallower one is declined, and an
  accepted store with no move keeps the move already there
- Non-mate score: unaffected by normalization at any ply
- Winning mate round-trip: `normalize`/`denormalize` at same ply recovers original value
- Losing mate round-trip: same for negative mate
- Mate score ply adjustment: probe at different ply returns adjusted distance
- `clear()`: removes all entries; subsequent probes return `nullopt`
- `entry_count` increments on new key, does not increment on overwrite
- `pv_count` tracks `PV_NODE` entries correctly
- `clear()` resets both counters to zero

### Evaluation Tests (`[eval]`)

**File**: `StratChessTests/EvalTests.cpp`
**Rationale**: Evaluation has zero tests today. The roadmap plans King Safety and Mobility evaluation — these tests establish a baseline that regressions will break.

**Approach**: `Board::Instance().SetupFromFEN(fen)` then `EvalManager::Create(type)->Evaluate()` — same pattern as RepetitionTests.

**Test cases**:
- Starting position: EvalSimple within ±200 cp (symmetric, tables may not sum to exactly 0)
- Starting position: EvalComplex within ±200 cp
- White extra queen (black has no queen), white to move: EvalSimple > 500 cp
- Black extra queen (white has no queen), black to move: EvalSimple > 500 cp
- White extra queen: EvalComplex > 500 cp
- Both engines agree: both positive when white has material advantage
- EvalComplex doubled pawn penalty: score lower with doubled pawns than without
- EvalComplex rook on 7th: endgame position with rook on 7th scores positively
- Mop-up evaluation (issue #70): decisively-won pawnless ending scores higher with the losing
  king cornered vs. centered
- Mop-up evaluation: gated off once either side has a pawn on the board
- Mop-up evaluation: gated off below the 400 cp decisive-material threshold
- Color symmetry (issue #125): `getEvalBoard` mirrors a Black piece's square vertically
  (`square ^ 56`), not by 180-degree rotation — direct cases via a test-local `EvalProbe`
  subclass, plus file-preservation/rank-inversion checked over all 64 squares
- Color symmetry: whole-position cases via a file-local `MirrorFen` FEN color-mirror helper —
  starting position, a queen-PST asymmetry regression (queen on c6), a middlegame with
  rooks on open/half-open files, an endgame that trips the king-PST switch, and a pawnless
  mop-up ending — `Evaluate(fen) == Evaluate(MirrorFen(fen))` for both evaluators
- Rook open-file definition (issue #126): an enemy knight sharing the rook's file must not
  demote an open file to half-open (`all_black`/`all_white` narrowed to `black_pawns`/`white_pawns`)
  — an enemy pawn on the file still demotes it (guard); an own pawn behind the rook still leaves
  the file half-open/open, pinning the deliberate D5 decision in
  `.claude/plans/passed-and-backwards-pawn-terms.md`
- Color symmetry: the #126 knight-on-file and pawn-on-file positions added to the `MirrorFen`
  whole-position symmetry cases above, since the two new masks are the most likely place for a
  color asymmetry to be introduced
- Term-level tests (issue #127 restructure): `EvalComplex::Evaluate()` now builds an
  `EvalContext` and sums four private per-term functions (`eval_pawns`, `eval_rooks`, `eval_pst`,
  `eval_mopup`), each `(const EvalContext&, eColor) -> int`. `EvalComplexTestFixture` (a
  `STRAT_ENABLE_TEST_ACCESS` friend, same mechanism as the AIPerplex/UciHandler fixtures) builds
  an `EvalContext` from a `Board` and forwards to each term, so terms are asserted on directly
  instead of only inferred from whole-position deltas:
  - `eval_pawns`: a normal (non-isolated, non-doubled) structure scores exactly 0; the doubled
    a-file pair (`FEN_WHITE_DOUBLED`) scores exactly `-(DOUBLED_PAWN_PENALTY + 2*ISOLATED_PAWN_PENALTY)`
  - `eval_rooks`: a 7th-rank rook on a fully open file scores exactly `ROOK_ON_7TH_BONUS + OPEN_FILE`;
    the issue #126 knight-on-file case and the D5 own-pawn-behind case are re-asserted as exact
    term-level equalities, not just whole-position deltas
  - `eval_pst`: the king receives exactly one PST contribution, from the stage-selected table —
    verified against an independently-computed expected value (`EvalProbe::GetPositionalScore`
    plus a direct `g_Eval_Bitboards` lookup) for both the middlegame and endgame tables
  - `eval_mopup`: only the winning color gets a nonzero contribution; both colors get 0 below the
    decisive-material threshold
  - `eval_mobility` (#98, #113): a central knight outscores a cornered one; an open-file rook
    outscores one boxed in behind its own pawns; a square covered by an enemy pawn stops counting
    (the safe-mobility mask); an own piece blocks a bishop's diagonal while an enemy piece on the
    same square does not (pinning the capture-target convention); a lone queen scores nonzero (#113,
    so the queen cannot be silently skipped); and a bare king scores exactly 0 (king mobility is
    #97's, not this term's)
  - Passed and backwards pawns (#116): the span masks are asserted for **content** before any term
    consumes them — `g_bbPassedMaskWhite[d4]` is exactly the c/d/e files ahead, an a- or h-file pawn's
    span never reaches the opposite edge (the wraparound bug this kind of generator invites), and a
    pawn on the promotion rank has an empty span. Then the term itself: a passer outscores the same
    pawn with an enemy pawn in its span; the bonus grows as the pawn advances and is larger at low
    phase than high (the tapering property); an a-file passer is detected past a black h-pawn; a
    backwards pawn is penalised, and a pawn failing **either** clause is not — the clause-(a) case
    differs from its comparison by one added pawn that contributes nothing itself, so the whole delta
    is the penalty disappearing
  - Kingless board: a default-constructed `Board` (no kings, all bitboards zero) evaluates to
    exactly 0, as it did before the restructure. This is a **Debug-build** regression test in
    substance: `Board::GetFirstPiece` has an `assert(mask != 0)` precondition, so an unguarded king
    lookup asserts in Debug and reads out of bounds in Release — where the suite would pass anyway.
    Reachable in shipping code because `UciHandler::board_` is default-constructed and never set to
    the start position, so a UCI `eval` issued before any `position` command lands here.
  - Structural check: the four terms plus raw material, summed the same way `Evaluate()` sums
    them, reproduce `Evaluate()`'s result exactly across every whole-position FEN used by the
    color-symmetry cases above
  - `EvalComplex::Breakdown()` (issue #129 phase 2 — the public production path the UCI `eval`
    command reads): every row equals the corresponding `EvalComplexTestFixture` term call, and
    `material` equals `Board::GetMaterialScore`, across the same FEN set. Tied to the already-
    tested terms rather than asserted in isolation — the failure mode worth guarding is
    `Breakdown()` reporting something other than what `Evaluate()` sums, which self-consistent
    output would never reveal
  - `Breakdown().total` agrees with `Evaluate()`, *and* the rows reproduce it: material plus the
    four terms, summed white-minus-black, up to the side-to-move sign — the latter is what makes
    the printed net column trustworthy. D8's stronger claim (that `total` *is* `Evaluate()`'s
    return value, not a correct re-derivation of its sign flip) is structural and enforced by the
    code, not by these assertions; what they catch is a re-derivation that is *wrong*
  - `Breakdown().phase` matches what `BuildContext` computes: `MAX_GAME_PHASE` at full material,
    4 for K+Q vs bare king, 0 for bare kings, unchanged by adding a full pawn set (pawns carry no
    phase weight), and clamped rather than extrapolating on a three-queens-a-side promotion overshoot

**Tapered evaluation (`[eval]`, issue #99)**. The taper is asserted on its *endpoints* wherever
possible rather than on a blended value at one position's particular phase — a blended assertion
silently depends on the phase weights, so it would start testing something else the moment those
change.

- `BlendPhase` is exact at both endpoints (`phase == MAX_GAME_PHASE` yields `mg`, `phase == 0`
  yields `eg`) and moves monotonically between them. The classic tapering bug is an off-by-one
  that makes neither endpoint reproduce its own input, so both are asserted directly
- `eval_pst`: the king's two endpoints are exactly `g_Eval_Bitboards[5]` and `[6]` plus the
  independently-computed non-king PST sum — and the two tables are asserted to *differ* at the
  test square, so the blend cannot be a no-op masquerading as one. A companion case ties the
  reported value to `BlendPhase(pair, phase)` for the position's own phase
- `eval_rooks`: the 7th-rank bonus appears only at the `eg` endpoint, the open-file bonus at both
- **No cliff**: two positions differing by exactly one minor piece — which used to straddle the old
  `min(material) <= 11500` threshold — now move the king's contribution by far less than the ~100 cp
  jump the hard switch produced. This is the property the change exists to create, so it is tested
  directly rather than inferred
- King centralization is worth strictly more at low phase than at high phase
- **#118 item 4 regression**: in a gated pawnless K+Q vs K+R position, walking the winning king
  toward the cornered loser must *raise* the score. Written before the fix and confirmed failing
  (approach cost 4 cp); it gains 4 cp after. The test also guards its own premise by asserting
  mop-up is actually active in both positions, so it cannot pass vacuously

### Tactical Tests (`[tactical]`)

**File**: `StratChessTests/TacticalTests.cpp`
**Rationale**: Direct regression tests for search correctness. If LMR, aspiration windows, or move ordering changes break tactical play, these catch it fast. Each test uses `AIPerplex` at depth 4 — fast on simple positions (< 100 ms each), finds forced results reliably.

**Approach**: `PlayerBase::Create(AI_PERPLEX, 4)`, `SetEvalEngine(COMPLEX)`, `SetVerboseLogging(false)`, then `GetMove(limits).best_move`. Check `m.from()` and `m.to()`.

**Test cases**:
- Mate in 1 (rook delivers back-rank mate): engine plays Ra8# (`6k1/5ppp/8/8/8/8/5PPP/R5K1`)
- Mate in 1 (queen delivers back-rank mate): engine plays Qd8# (`6k1/5ppp/8/8/8/8/3Q4/6K1`)
  — queen slides along the d-file; lands 3 squares from king (cannot be captured)
- Capture hanging rook: engine captures undefended piece (`4k3/8/8/8/8/8/8/2rQK3`)

### Slow Tactical Tier (`[tactical_full][slow]`)

**File**: `StratChessTests/TacticalFullTests.cpp`
**Rationale**: Deeper regression coverage at depth 6 (~1.8 s total). Excluded from the default `run-tests` loop (`~[slow]`) to keep the fast feedback cycle under 5 s. Run via `.\build.ps1 extended-tests` or `run-tests "[tactical_full]"` before merging search changes.

**Approach**: Same `GENERATE(from_range(...))` / `TacticalTestHelpers.h` pattern as the fast tier, depth 6.

**Current positions (25)**: 2 mate-in-1 back-rank mates + 23 winning captures across diverse piece types (queen, rook, bishop, knight) and board regions. Every position was individually verified against the engine at depth 6 before committing.

**⚠ Technical debt — position diversity**: the initial 25 positions are dominated by simple hanging captures. Multi-move tactics (mate-in-2, forks, pins, discovered attacks) proved hard to construct with a *unique* best move at depth 6 for this engine's current tactical strength. As the engine improves, replace simpler captures with positions from the WAC-25 set or crafted M2 suites. The selection invariant (unique best move at the target depth, verified before committing) must be maintained.

---

## Component-specific coverage

Each item below is a standalone task. Do it when the corresponding feature is being modified — not ahead of time.

### `[search]` — AIPerplex helper unit tests

**Status**: ✅ **Done.** LMR landed in March 2026; all 10 cases passing.
**File**: `StratChessTests/SearchTests.cpp`
**Activation**: `STRAT_ENABLE_TEST_ACCESS`, applied to the `StratChessTests` target only by `CMakeLists.txt`.

Tests for private helper methods exposed via `AIPerlexTestFixture` (friend class):

- `assess_iteration_quality()`: 6 cases — one per `RejectionReason` branch (INCOMPLETE×2, TOO_FEW_NODES, SHORT_PV, SCORE_DROP, MOVE_CHANGED)
- `should_stop_early()`: 2 cases — mate score path; short-PV forced-line path
- `handle_empty_move_emergency()`: 2 cases — mate-detected path (returns false); true-emergency path on a real starting-position board (returns true, sets legal move)

### `[sort]` — Move ordering tests

**Status**: ✅ **Done.** ScoreMoves extracted from pvs() inline loop; 5 test cases, all passing.
**File**: `StratChessTests/SortTests.cpp`

Verify ordering priority: hash move → captures (MVV-LVA) → killers → history scores.

The hash move is the top tier, and one case asserts that an *empty* hash move promotes nothing. That
is not a trivial case: an empty `Move` reads as `h1 → h1` under `Move`'s from/to equality, and it is
what made the removed previous-iteration PV tier provably dead rather than merely unused — it was fed
an empty move at every node, so it matched nothing, ever.

### `[board]` — DoMove/UndoMove completeness

**Status**: ✅ **Done.** Move layout Phases 3 & 4 landed in March 2026 (6 test cases, 33 assertions). Extended March 2026: `BoardMoveTests.cpp` (9 cases, `[board_moves]`), `BoardStateTests.cpp` (12 cases, `[board_state]`), `BoardApiTests.cpp` (11 cases, `[board_api]`) add full move-type, GameInfo state, and API coverage.
**File**: `StratChessTests/BoardTests.cpp`

- En passant DoMove/UndoMove: captured pawn restored correctly
- Castling DoMove/UndoMove: rook and king both moved and restored
- Promotion move generation: white pawn b7→b8 generates `MoveType::PROMOTION_QUEEN`; capture-promotion c7xb8 generates queen promo with `PieceHelper::IsActual(m.Content)` true (these Move-field checks were in the retired `MoveGeneratorPromotionTests.h` and are not covered by perft)
- Promotion DoMove/UndoMove: pawn replaced by promoted piece, restored on undo
- Zobrist hash: `get_zobrist_hash()` identical before and after a DoMove/UndoMove cycle

### `[time_mgr]` — Time management unit tests

**Status**: ✅ **Done.** Clock-aware time management landed March 2026.
**File**: `StratChessTests/TimeManagerTests.cpp`

**Formula tests (6 cases, no sleep):**
- Blitz midgame: `compute_budget(150000ms, 2000ms, 0)` → soft ≈ 6.6 s, hard ≈ 19.8 s
- Classical with movestogo: `compute_budget(3600000ms, 0ms, 20)` → soft = 3 min, hard = 9 min
- Increment-heavy time trouble: soft ≥ 100 ms (floor clamp enforced)
- Time trouble (remaining < overhead): no crash, soft ≥ 100 ms
- Zero increment, no movestogo: soft ≈ 2 s
- Invariant: `hard >= soft >= 100 ms` across 4 input combinations

**TimeManager timing tests (4 cases, short sleeps ≤ 60 ms each):**
- Two-arg `start(soft=20ms, hard=60ms)`: `should_stop_iteration()` fires after 25 ms, `should_stop_search()` still false; then true after 65 ms
- One-arg `start(20ms)`: both methods fire together (backward compat)
- `stop()` fires `should_stop_search()` immediately
- `elapsed()` increases monotonically

**Testing the abort path needs a node limit, not the clock.** `SearchLimits::fixed_nodes(N)` /
`go nodes N` exists for this: at `Threads=1` node counting is deterministic, so the abort lands at a
reproducible point and a test can assert on what the interrupted search produced. The clock cannot do
that. A zero budget is passed through verbatim and latches on its first check, but that check is
either the 1024-node poll or the end of depth 1 — always too shallow to reach the behaviour worth
asserting — and any non-zero budget is wall-clock dependent. Related: `[nodes]` cases in
`SearchTests.cpp`, whose upper bound is deliberately loose because the poll cadence and the node
budget count different things (see the comment on the poll in `pvs()`).

A node-limit test must fail rather than hang when the mechanism regresses. Choose a depth cap the
search can actually finish: with no working limit, the only remaining bound is the one-hour fallback
clock, so a cap that is too deep turns a real defect into a CI timeout with no diagnostic.

**The abort-path regression test is `cmd_go: a node-limited search never reports a spliced pv`**
(`[uci]`). It drives `go nodes N` over four budgets from `position startpos moves e2e4 e7e5 g1f3` and
replays every emitted PV. The budget that reproduces #310 is **10,000**: before the unwind guard it
reported `pv d7d5 e4d5 d5e4 …` at depth 5 — a black move from a square that by then holds a white
pawn. Budgets are not interchangeable, and one was found by sweeping rather than reasoned to; a new
abort defect will land at whatever budget it lands at, which is why the test checks several and why a
future one should be added rather than an existing one retuned.

### `[pv]` — PV legality

**File**: `StratChessTests/PVIntegrityTests.cpp`

`pv_replays_legally(root, line)` (`StratEngine/PVIntegrity.h`) is the invariant behind the Debug
assertion in `AIPerplex::emit_iteration_info`, tested directly because the assertion fires from inside
a search where the input that produced it is no longer reachable. Both halves of legality need their
own case: `ComputeLegalMoves` is pseudo-legal, so membership in it does not rule out leaving one's own
king in check, and `DoMove` executes any from/to/flags triple it is handed, so playing a move does not
prove the position offered it. The flag cases are not padding — `Move` equality ignores flags, so a
membership test written with `==` passes every one of them.

Two `[search][pv]` cases in `SearchTests.cpp` cover the abort paths the end-to-end UCI test cannot
force deterministically, both through `AIPerlexTestFixture`:

- **A `pvs()` frame that aborts at entry leaves an empty row 0.** Seed row 0 (standing in for a
  completed aspiration retry), latch the abort with `StopSearch()`, run one node. The entry exit
  returns a fabricated `GameValues::Draw`, and an empty row is what makes `iterative_deepening()`
  reject it as INCOMPLETE instead of reporting `score cp 0`.
- **A stale row 1 is not spliced onto the emergency move.** `PVTable::update` copies row `ply + 1`,
  so `handle_empty_move_emergency()` must clear row 1 first or publish a two-move line whose tail
  came from another position.

Both are red against the code before their fix — row 0 keeps its seeded move, and row 0 comes out
length 2 — which is the only reason they are worth having.

### `[bitboard]` — Bitboard helper tests

**When**: opportunistically (low priority)
**File**: `StratChessTests/BitboardTests.cpp`

Basic operations from `defines.h`: set bit, clear bit, popcount, LSB extraction.

### `[magic]` — Sliding-piece attack generation tests

**Status**: ✅ **Done.** Landed with the PEXT magic-bitboard refactor (July 2026), replacing
rotated-bitboard attack generation. See `.claude/plans/magic-bitboards-sliding-piece-attacks.md`.
**File**: `StratChessTests/MagicBitboardTests.cpp`

Hand-verified `RookAttacks`/`BishopAttacks` bitboards for open cross/diagonal (empty board),
corner squares with blockers, and fully-blocked-adjacent cases — independent of perft, which
only proves attack generation is *consistent* with legal move counts, not that any individual
attack bitboard is correct in isolation.

### `[uci]` — UCI command tests

**File**: `StratChessTests/UCITests.cpp`
**Access**: `UciHandlerTestFixture` (`STRAT_ENABLE_TEST_ACCESS`) drives private command
handlers (`cmd_position`, `cmd_setoption`, `cmd_ucinewgame`, `cmd_eval`) directly, without a
running `run()` loop or piped stdin.

Covers `parse_go()` parameter parsing, `cmd_position` move replay (including the MAX_PLY
overflow regression), `cmd_setoption`'s Threads persistence across `ucinewgame`, and Hash option
advertisement, allocation reporting, replacement, persistence, malformed-input and in-search
refusal contracts.

Also covers the mid-search refusal (#178): `position` and `setoption` are rejected while a search
runs. The fixture sets the `searching_` flag directly rather than starting a real search — the
contract under test is the guard's, not the scheduler's, and racing a live search would make the
cases timing-dependent. One case exists specifically to pin *why* the flag is needed: a
`search_thread_.joinable()` guard would look equivalent and would reject the `position` of every
normal `go` → `bestmove` → `position` cycle, because a `std::thread` stays joinable after its
function returns.

### `[argparse]` — External integer parsing

**File**: `StratChessTests/ArgParseTests.cpp`

`Engine::parse_int` is what every `argv` and JSON-key integer goes through. The cases that matter are
the rejections: `std::stoi` accepts trailing garbage (`"12abc"` → 12) and reports the rest by
throwing, which is how `perft run abc` used to kill the process with no message at all (#178).
Covers blank input, trailing text, embedded spaces, and one past each `int` boundary.

### `[config]` — Settings-file parsing

**File**: `StratChessTests/ConfigTests.cpp`

Each case writes a `game_settings.json` to a temp file and asserts what `Config::ReadConfigFile`
does with it: truncated JSON, valid JSON without a `"game"` key, and a key of the wrong type all
throw a *diagnosable* nlohmann exception, which `main` turns into a clean exit 1 rather than a
`0xC0000409` fail-fast. A missing file is deliberately **not** an exception — defaults are a usable
outcome, and the message says so.

Two guards against the suite passing for the wrong reason: a well-formed document must parse and
yield the expected depths, and a commented document must parse — `game_settings.json` ships heavily
commented, so a regression in `ignore_comments` would break the shipped file rather than a test.
`Config(nullptr)` is safe for all of these because `pGame_` is dereferenced only on the FEN path.

**`cmd_eval` (issue #129 phase 1 — static-eval introspection)**:
- Works before any `position` command (default-constructed, empty `board_`) without crashing.
- **Honesty invariant**: the printed `static eval:` score is a real parse of the emitted
  number, asserted equal to `EvalManager::Create(COMPLEX)->Evaluate(Board(fen))` for the same
  FEN — the property that makes the tool trustworthy for #117's tuner and #127's byte-identity
  check, since it proves `eval` never computes a parallel score.
- Output contains neither `bestmove` nor `info` — `eval` must not look like a search response.
- `white pov:` matches the stated sign convention: equals the side-to-move score when White is
  to move, equals its negation when Black is to move — checked against two positions with a
  whole-rook material imbalance so a sign bug can't hide behind a near-zero score.

**`cmd_eval` per-term breakdown (issue #129 phase 2)**: asserted on the *printed* table, not on
`EvalBreakdown` directly — a breakdown that is right internally and mis-rendered is still a
debugging tool that lies, and #117 reads the output rather than the struct. `extract_term_row`
matches rows on the first whitespace-delimited token, so changing the column widths cannot quietly
turn these into no-ops.

- **Extended honesty invariant**: for each of five positions (middlegame, symmetric startpos,
  endgame, Black-to-move, mop-up-active), every row's `net` equals its own `white - black`; the
  net column sums to the printed `sum (white pov)` line; that sum equals the `white pov:` line;
  and it equals a white-relative `Evaluate()` computed independently from a fresh `Board`.
- Game phase is reported and correct (`phase: 24/24` at full material, `phase: 4/24` for K+Q vs K)
  — it is not derivable from the rows, yet sets where between the mg and eg endpoints every
  tapered term landed, and gates mop-up. Replaced the `stage: <name>` line in issue #99.
- A term active for exactly one side shows it in the per-color split: mop-up in a pawnless K+Q
  vs K is positive for White and exactly 0 for Black. This is what pins the columns as carrying
  independent information rather than both being derived from `net`.

Batch-mode FEN scoring (`StratChessEvolved.exe eval <path>`) is a CLI subcommand, not a UCI
command — see `.claude/plans/uci-eval-command-term-breakdown.md` (D4). `evalrunner`'s per-line
classification (blank/comment/malformed/valid) is extracted into `FenBatch::ClassifyLine`
(`StratEngine/Utils/FenBatch.h`, header-only) and covered by `[uci]` cases in `UCITests.cpp`
(issue #140) — this is what makes the guard a regression-tested invariant rather than a
manually-verified one. The two-tier form (field-count pre-filter, then `FENParser::ParseFEN`)
collapsed to a single tier in issue #143: the pre-filter advertised "need at least 4" while the
parser's regex actually demanded all six fields, and `ParseFEN` now counts fields itself ahead
of its regex. The `[uci]` cases cover 4-, 5- and 6-field FENs, the halfmove/fullmove defaults
(0 and 1) they imply, and that EPD operations remain rejected.

They also cover the counter bounds (`GameState.h`): a halfmove clock past 150 or a fullmove counter
past 5899 is rejected with a diagnostic and leaves the board untouched, a value exactly at either
bound round-trips through `ExtractFEN`, and fullmove `0` loads as `1` — the one input the parser
repairs rather than passes through. A `position ... moves` list past 11797 plies is refused whole,
matching the illegal-token path. These bound the *input*; playing on from a position loaded at a
bound is legal and is covered by `[board_api]`.

The surrounding CLI plumbing (file I/O, stdout/stderr framing, line
numbering) remains covered by manual validation, matching the existing convention for
`perft`/`tactical` CLI runners in this file.

**Corpus tooling**: `StratChessEvolved/Scripts/build_corpus.py` harvests, validates and
deduplicates FENs from perft/tactical JSON, `StratChessTests/*.cpp` literals, the openings PGN,
and optional self-play PGNs (`--pgn-dir`) into a single file consumable by `eval <path>` above.
It is the tool behind #127's score-identity corpus (8574 positions, byte-identical scores
before/after the `EvalContext` restructure) and is the reusable form of that workflow for any
future refactor claiming behaviour preservation (#131), as well as the seed of #117's Texel
tuning corpus. Requires `pip install python-chess`; run with no arguments from a clean checkout
for an in-repo-assets-only corpus, or see `--help` for `--pgn-dir`/`--every-n-plies`/`--out`.

### Full tactical suite in main executable

**Files**: `StratEngine/Tests/TacticalTestRunner.h/cpp`, `Tests/tactical_test_cases.json`
**Invocation**: run from `Tests/` directory: `StratChessEvolved.exe tactical test`
(defaults to `tactical_test_cases.json`) or pass a staging filename. The JSON is the source of truth
for the current cases and expected moves; do not duplicate its count or contents here.

**Stability mode**: `StratChessEvolved.exe tactical stability [N] [filename] [threads]` (N
defaults to 10, threads defaults to 1) runs the whole suite N consecutive times and fails
when a run fails the normal gate policy or a position's pass/fail result *flips*. A consistent,
tolerated non-mate failure does not break stability; a flip detects nondeterministic search. The
policy is unit-tested in `SuitePolicyTests.cpp` (`[suite_policy]`). `Validate-PrePR.ps1` runs ten
single-threaded repetitions; Nightly runs 100.

The optional `threads` argument reaches `AIPerplex::SetThreads()` before search; `AIPerplex` clamps
it to `[1, 32]`. Use it to validate Lazy SMP—for example,
`tactical stability 20 tactical_test_cases.json 4` must have no failing runs or flips.

**Acceptance**: 90%+ overall pass rate, **and 100% pass rate in every category whose
name starts with `mate`** (`mate_in_1`, `mate_in_2`, `mate_in_3`, `mate_in_4`). The
100%-mate rule is enforced by `TacticalTestRunner::evaluate_results()` and unit-tested
in `StratChessTests/SuitePolicyTests.cpp` (`[suite_policy]`) — a single mate-category
failure fails the suite even if the overall pass rate is still above 90%.

**Growing the suite**: new candidates never go straight into `tactical_test_cases.json`.
Stage them in `Tests/tactical_staging.json` (same schema, transient — never committed),
run `StratChessEvolved.exe tactical test tactical_staging.json` from `Tests/`, and
reconcile: for mate categories, an engine move that differs from the EPD key is only
accepted if `Scripts/verify_mate_key.py "<FEN>" <uci_move> <N>` prints `CONFIRMED`
(ground truth via python-chess, not manual analysis); for non-mate categories the
engine's move must strictly match the EPD `bm`. Once a candidate is confirmed, move its
entry into `tactical_test_cases.json` and delete it from the staging file.

**Depth stability is a hard requirement, not a preference.** A position whose best move changes
with search depth is not a regression test. Before promoting a candidate, run it at several depths
around its target—4–8 is a reasonable band—and require the **same** expected move at every one.
A position that only solves from depth 6 upward is fine; commit it with `depth: 6`, having verified
6 through 9. Prefer positions decided by a decisive material or mating margin: the ones decided by
a few centipawns are exactly what produces a razor-thin tie that any eval change can tip.

**`best_moves` asserts any objectively equivalent decisive continuation, not only the puzzle's
historical key move** — the suite checks chess truth, not this engine's preferences. Widen a list
only on external evidence, never because this engine also scores a move highly.

## Test Isolation Rules

- Each `TEST_CASE` constructs its own local `Board` (via the FEN constructor, or the default constructor + `SetupFromFEN`) — no shared global board state between tests.
- TT tests use a fresh `TranspositionTable(1)` (1 MB) per test — never the AIPerplex-internal TT.
- Tactical tests create a fresh `AIPerplex` per test — this allocates a 256 MB TT, which is acceptable on development machines (virtual memory is lazy-paged).

---

## Regression Protocol

When a bug is found and fixed:
1. Create a minimal reproduction FEN
2. Add a `TEST_CASE` that would have caught it (repetition, perft, eval, or tactical)
3. Include the bug report in the test comment (like the existing BUG-1 through BUG-4 in `RepetitionTests.cpp`)
4. Commit test and fix together

---

## Writing tests — mechanics

### Running the suite

The test binary lives under `build/<preset>/` and runs from any directory;
`Scripts/Get-BuildArtifact.ps1 -Target StratChessTests` resolves the path. Filter by tag with a
Catch2 argument:

```bash
build/windows-clang-cl/StratChessTests.exe            # all tests
build/windows-clang-cl/StratChessTests.exe "[eval]"   # one tag (see Coverage Map above)
```

`build.ps1 run-tests` excludes `[slow]`; `build.ps1 extended-tests` includes it.

Framework is Catch2 v3, amalgamated, fetched and pinned by `FetchContent` into `build/_deps`.

Adding a new `.cpp` needs no project edit — `CMakeLists.txt` globs `StratChessTests/*.cpp` with
`CONFIGURE_DEPENDS`, so creating the file is enough.

### Tactical test positions

`StratChessTests/TacticalTestHelpers.h` provides the shared `TacticalCase` struct and
`make_tactical_engine()` factory. Use the `GENERATE(from_range(kCases))` pattern — see
`TacticalTests.cpp`.

Constructing FEN positions (all four are bugs that have actually bitten):

- **Always append ` w - - 0 1`.** A FEN with fewer than four fields is rejected by the parser, so
  the position is never applied: `position` is declined and the engine answers for whatever the
  board still held. `Board::SetupFromFEN` returns `false` here — in a test, `REQUIRE()` it.
- **The side not to move may not be in check.** Such a position is illegal and is now rejected at
  load: `SetupFromFEN` returns `false`, and `Board(fen)` asserts in Debug builds — which is how a
  bad position announces itself, since the assert aborts the run. Adjacent kings are the same rule.
  Previously these loaded silently and the engine answered with a king capture (bug #45), so the
  older FEN constants carry hand-verification notes; keep writing them, they document intent.
- **Verify uniqueness against the engine**, not by eye:
  `(printf "uci\nisready\nposition fen FEN w - - 0 1\ngo depth N\n" | ./build/windows-clang-cl/StratChessEvolved.exe 2>/dev/null | grep "^bestmove")`
- In Git Bash use parentheses `(cmd; cmd) | pipe` for multi-command UCI pipes — braces `{ }` fail.

### Constructing AIPerplex in a test

The constructor re-enables verbose logging and leaves `Eval` null, so the order matters:

```cpp
Board board(fen);                                   // before Create()
auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth, board);
AIPerplex::SetVerboseLogging(false);                // after Create()
ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX); // before GetMove()
Move m = ai->GetMove().best_move;                   // GetMove() returns a SearchResult
```

### Deep perft

Must run from the `Tests/` directory — the executable looks for `perft_test_cases.json` in the
working directory:

```bash
cd Tests
../build/windows-clang-cl/StratChessEvolved.exe perft test
```

Sources: `StratEngine/Tests/Perft.h/cpp` + `Tests/perft_test_cases.json`.

### Corpus move-generation sweep (perftcheck)

The breadth instrument. [perftcheck](https://grandchesstree.com/perftcheck) (Apache-2.0) drives the
engine over UCI with `go perft <depth>` and compares its divide output against a Stockfish/TGCT
oracle across **142,953 positions at depths 1–4** — roughly a thousand times the breadth of the
committed suite, which is where move-generation faults are actually found (#195).

```powershell
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-PerftCheck.ps1"
... Run-PerftCheck.ps1 -Limit 5000                       # bounded sanity run, seconds
... Run-PerftCheck.ps1 -ClassifyReport <report.json>      # re-read a past run, no engine needed
```

`perftcheck.exe` (~84 MB, not committed) lives in `EngineTesting\` beside `fastchess.exe`; the script
prints the download URL if it is missing. Cost per case is superlinear — the corpus is ordered roughly
simplest-first — so a `-Limit` run's rate does not extrapolate to the whole.

**A clean sweep does not mean zero failures.** The corpus contains positions the FEN parser correctly
rejects (pawns on rank 1/8, nine pawns, over 32 pieces); the engine resets to the start position and
answers for that, which the oracle scores as a mismatch. Those failures carry a fingerprint — an
actual node count equal to the start position's perft for that depth — and the script buckets them
mechanically, failing only on what is left over. Classify by that rule, never by eye: reading the raw
counts as move-generation discrepancies is exactly the mistake #200 had to correct.

**Result, 2026-08-05** (`8edd327`, edge cases included): 561,641 checks, **561,568 passed**, 73
failures over 19 distinct positions, **every one carrying the rejected-FEN fingerprint**, 23.4 min.
On every legally reachable position in the corpus at depths 1–4, `MoveGenerator` matches the oracle
exactly. Full detail and the failure classification: #198.

## AIPerplex Test Access

To test private search helper methods, `AIPerplex.h` contains a conditional friend declaration:

```cpp
#ifdef STRAT_ENABLE_TEST_ACCESS
    friend class AIPerlexTestFixture;
#endif
```

`CMakeLists.txt` defines `STRAT_ENABLE_TEST_ACCESS` for `StratChessTests` only; production does not
receive it.

The macro is intentionally absent from the `StratChessEvolved` target.

## Game Loop Test Access

`Game::TestAccess` (declared in `Game.h`, defined in `GameLoopTests.cpp`) builds a game around an
explicit FEN and scripted players, avoiding settings and log files. It makes game-loop outcomes,
including the fifty-move transition and `HUMAN_EXITED`, deterministic. Test-created games set
`owns_logging_ = false` so their destruction cannot drop loggers used by later tests.

## The terminal-result contract

A player reports a terminal result with a null `best_move` and `SearchResult::game_state`. Test both
sides: `GameLoopTests.cpp` verifies `Game::Run()` consumes it (including the score channel), while
`SearchTests.cpp` and `PlayerHumanTests.cpp` verify each producer reports it correctly.

The verdict is per-call, and only an **aborted** search can carry the previous call's verdict out: any
search that completes a root frame overwrites it anyway. So the `[search]` cases that matter abort
before the first root frame finishes — `AIPerplex` via a node limit on a position whose depth-1
iteration costs more than one 1024-visit poll interval (asserted in the test, not assumed), `AIAgent`
via a budget already spent on entry. A stale terminal verdict makes
`handle_empty_move_emergency()` return no move at all, so both cases check `best_move` as well.
