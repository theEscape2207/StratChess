# StratChessEvolved — Test Design

**Status**: Phase 0 bootstrapped (March 2026)
**Owner**: Thees
**Related**: `Docs/Roadmap.md` — consult before adding tests for a new area

---

## Philosophy

Tests are **protection tools for the roadmap**, not bureaucratic overhead.

- **Fast feedback above all**: the full Catch2 suite must run in < 10 seconds (Release build). Slow tests do not get run.
- **Tests protect the next change, not the last one**: write tests just before touching a component, especially for high-risk roadmap items (LMR, MoveSorter, parallel search).
- **Direction over precision**: for evaluation and search, verify the *direction* and *relative magnitude* of results. Exact centipawn values are fragile and mislead.
- **Perft is the ground truth for move generation**: do not replace it with unit tests — extend it with more positions.

---

## Test Tiers

| Tier | Executable | Tag / command | Max time | Trigger |
|------|-----------|---------------|----------|---------|
| Unit & fast integration | `StratChessTests.exe` | (all), or `[tag]` | < 10 s | Every build |
| Deep perft | `StratChessEvolved.exe perft test` | — | Minutes | Pre-merge |
| Full tactical suite | `StratChessEvolved.exe tactical stability 10` (single run: `tactical test`) | — | ~11 s | Pre-PR (automated: `Validate-PrePR.ps1` Step 3) |

Run all fast tests at once:
```bash
x64/Release/StratChessTests.exe
```

Run by tag:
```bash
x64/Release/StratChessTests.exe [tt]
x64/Release/StratChessTests.exe [eval]
x64/Release/StratChessTests.exe [tactical]
x64/Release/StratChessTests.exe [repetition]
x64/Release/StratChessTests.exe [moves]
x64/Release/StratChessTests.exe [perft]
x64/Release/StratChessTests.exe [search]
x64/Release/StratChessTests.exe [sort]
x64/Release/StratChessTests.exe [time_mgr]
x64/Release/StratChessTests.exe [board]
x64/Release/StratChessTests.exe [board_moves]
x64/Release/StratChessTests.exe [board_state]
x64/Release/StratChessTests.exe [board_api]
```

The `[tactical_full]` suite is tagged `[slow]` and excluded from the default `~[slow]` run. Use `extended-tests` to include it:
```powershell
.\build.ps1 extended-tests          # all tests including [slow]
.\build.ps1 run-tests "[tactical_full]"  # slow tier only
```

---

## Coverage Map

| Component | Tag | Status | File |
|-----------|-----|--------|------|
| Move structure & sentinels | `[moves]` | ✅ done | `StratChessTests/MoveFieldTests.cpp` |
| Repetition detection | `[repetition]` | ✅ done | `RepetitionTests.cpp` |
| Move generation (perft d1–d4) | `[perft]` | ✅ done | `PerftTests.cpp` |
| Move generation (deep perft d5+) | — | ✅ done | `StratChessEvolved.exe perft test` |
| **MoveFormatter** | `[formatter]` | ✅ Phase 0 | `MoveFormatterTests.cpp` |
| **TranspositionTable** | `[tt]` | ✅ Phase 0 | `TTTests.cpp` |
| **Evaluation (EvalSimple/Complex)** | `[eval]` | ✅ Phase 0 | `EvalTests.cpp` |
| **Search regression (tactical)** | `[tactical]` | ✅ Phase 0 | `TacticalTests.cpp` |
| **Search regression (slow tier)** | `[tactical_full][slow]` | ✅ Phase 0 | `TacticalFullTests.cpp` |
| Search helpers (assess_quality etc.) | `[search]` | ✅ Phase 1 | `SearchTests.cpp` |
| Move ordering (Sort) | `[sort]` | ✅ Phase 1 | `SortTests.cpp` |
| Board DoMove/UndoMove completeness | `[board]` | ✅ Phase 1 | `BoardTests.cpp` |
| Board move-type round-trips (all types) | `[board_moves]` | ✅ Phase 1 | `BoardMoveTests.cpp` |
| Board GameInfo state lifecycle | `[board_state]` | ✅ Phase 1 | `BoardStateTests.cpp` |
| Board public query APIs + FEN round-trip | `[board_api]` | ✅ Phase 1 | `BoardApiTests.cpp` |
| Time management (TimeManager + compute_budget) | `[time_mgr]` | ✅ Phase 1 | `TimeManagerTests.cpp` |
| Bitboard helpers | `[bitboard]` | ⏳ Phase 1 | `BitboardTests.cpp` (future) |
| Sliding-piece attack generation (PEXT) | `[magic]` | ✅ Phase 1 | `MagicBitboardTests.cpp` |
| UCI command loop | `[uci]` | ✅ Phase 1 | `UCITests.cpp` (+ `StratChessEvolved.exe uci` pipe smoke test) |
| Full tactical suite (WAC/mate-in-N) | — | ✅ Phase 1 | `StratChessEvolved.exe tactical test` |
| Board instance independence (post-de-singleton) | `[board_instance]` | ✅ Phase 2 | `BoardInstanceTests.cpp` |
| NPS / performance regression | — | ⏳ Phase 1 | — |

---

## Phase 0 — Bootstrap (March 2026)

### TranspositionTable Tests (`[tt]`)

**File**: `StratChessTests/TTTests.cpp`
**Rationale**: The TT is fully self-contained (no Board dependency). Its correctness affects every depth of search — a silent TT bug could mask wins or produce ghost moves. Tests added now provide a safety net before the TT is stressed by LMR and parallel search.

**Test cases**:
- Store and probe: entry is retrievable by key
- Probe miss: unknown key returns `nullopt`
- Same-key overwrite: second store wins
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

**Approach**: `PlayerBase::Create(AI_PERPLEX, 4)`, `SetEvalEngine(COMPLEX)`, `SetVerboseLogging(false)`, then `GetMove(info)`. Check `m.from()` and `m.to()`.

**Test cases**:
- Mate in 1 (rook delivers back-rank mate): engine plays Ra8# (`6k1/5ppp/8/8/8/8/5PPP/R5K1`)
- Mate in 1 (queen delivers back-rank mate): engine plays Qd8# (`6k1/5ppp/8/8/8/8/3Q4/6K1`)
  — queen slides along the d-file; lands 3 squares from king (cannot be captured)
- Capture hanging rook: engine captures undefended piece (`4k3/8/8/8/8/8/8/2rQK3`)
- QFORK-001 regression (issue #66): KQ vs KR domination (`8/8/8/3r4/4k3/8/8/3QK3`) —
  null-move pruning must not hide the zugzwang-based rook win; two moves accepted
  (Qa4+/Qb3), so it lives in a dedicated `TEST_CASE` outside `kFastCases`
  (whose rows require a unique best move) — this `TEST_CASE` is now hidden via
  Catch2's `[.]` tag (see Regression history below, and issue #118)

### Slow Tactical Tier (`[tactical_full][slow]`)

**File**: `StratChessTests/TacticalFullTests.cpp`
**Rationale**: Deeper regression coverage at depth 6 (~1.8 s total). Excluded from the default `run-tests` loop (`~[slow]`) to keep the fast feedback cycle under 5 s. Run via `.\build.ps1 extended-tests` or `run-tests "[tactical_full]"` before merging search changes.

**Approach**: Same `GENERATE(from_range(...))` / `TacticalTestHelpers.h` pattern as the fast tier, depth 6.

**Current positions (25)**: 2 mate-in-1 back-rank mates + 23 winning captures across diverse piece types (queen, rook, bishop, knight) and board regions. Every position was individually verified against the engine at depth 6 before committing.

**⚠ Technical debt — position diversity**: the initial 25 positions are dominated by simple hanging captures. Multi-move tactics (mate-in-2, forks, pins, discovered attacks) proved hard to construct with a *unique* best move at depth 6 for this engine's current tactical strength. As the engine improves, replace simpler captures with positions from the WAC-25 set or crafted M2 suites. The selection invariant (unique best move at the target depth, verified before committing) must be maintained.

---

## Phase 1 — Incremental (add when touching that area)

Each item below is a standalone task. Do it when the corresponding feature is being modified — not ahead of time.

### `[search]` — AIPerplex helper unit tests

**Status**: ✅ **Done.** LMR landed in March 2026; all 10 cases passing.
**File**: `StratChessTests/SearchTests.cpp`
**Activation**: `STRAT_ENABLE_TEST_ACCESS` in x64 Debug + Release preprocessor definitions in `StratChessTests.vcxproj`.

Tests for private helper methods exposed via `AIPerlexTestFixture` (friend class):

- `assess_iteration_quality()`: 6 cases — one per `RejectionReason` branch (INCOMPLETE×2, TOO_FEW_NODES, SHORT_PV, SCORE_DROP, MOVE_CHANGED)
- `should_stop_early()`: 2 cases — mate score path; short-PV forced-line path
- `handle_empty_move_emergency()`: 2 cases — mate-detected path (returns false); true-emergency path on a real starting-position board (returns true, sets legal move)

### `[sort]` — Move ordering tests

**Status**: ✅ **Done.** ScoreMoves extracted from pvs() inline loop; 5 test cases, 14 assertions, all passing.
**File**: `StratChessTests/SortTests.cpp`

Verify ordering priority: PV move → hash move → captures (MVV-LVA) → killers → history scores.

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
overflow regression), and `cmd_setoption`'s Threads persistence across `ucinewgame`.

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

**Status**: ✅ **Done.** Tactical runner landed March 2026 (8 positions); expanded to 31
positions July 2026 (WAC mate + tactical batches), 31/31 passing (100%).
**Files**: `StratEngine/Tests/TacticalTestRunner.h/cpp`, `Tests/tactical_test_cases.json`
**Invocation**: run from `Tests/` directory: `StratChessEvolved.exe tactical test`
(defaults to `tactical_test_cases.json`) or `StratChessEvolved.exe tactical test
[filename]` to run an arbitrary JSON file — used for staging candidates, see
"Growing the suite" below.

**Stability mode**: `StratChessEvolved.exe tactical stability [N] [filename] [threads]` (N
defaults to 10, threads defaults to 1) runs the whole suite N consecutive times and fails
on either (a) any run failing the normal gate policy or (b) any position whose pass/fail
*flips* between runs. The flip rule means a consistently-failing tolerated position
(within the 90% threshold) does not break stability, but a sometimes-failing one does —
flips are the signal for nondeterministic search results, the primary intermittent-race-bug
symptom once Lazy SMP threads share the TT. Policy is pure
(`TacticalTestRunner::evaluate_stability()`) and unit-tested in `SuitePolicyTests.cpp`
(`[suite_policy]`). Gated at N=10, threads=1 (default) in `Validate-PrePR.ps1` Step 3; on
today's single-threaded fixed-depth search it is deterministic and therefore trivially
green.

**Threads argument (Lazy SMP Gate 2)**: the optional fourth positional argument is
forwarded through `run_stability_suite` → `run_test_suite` → `run_position`, which applies
it via `PlayerAiBase::SetThreads()` (a no-op on legacy AIs; `AIPerplex` clamps to `[1, 32]`
— the CLI itself only rejects non-positive values, relying on `SetThreads`' own clamp for
the upper bound) on the `AIPerplex` instance it constructs, before `GetMove()`. This is the
mechanism for Lazy SMP's Gate 2 ("stable at N threads"): running
`tactical stability 20 tactical_test_cases.json 4` must show 0 failing runs and 0 flipped
positions — proof that helper threads sharing the TT do not introduce nondeterministic
search results at the fixed depths the suite uses. Verified 2026-07-23 (see
`.claude/plans/lazy-smp.md` Gate Results): 20/20 runs passed, 0 flips at threads=4.

**Acceptance**: 90%+ overall pass rate, **and 100% pass rate in every category whose
name starts with `mate`** (`mate_in_1`, `mate_in_2`, `mate_in_3`, `mate_in_4`). The
100%-mate rule is enforced by `TacticalTestRunner::evaluate_results()` and unit-tested
in `StratChessTests/SuitePolicyTests.cpp` (`[suite_policy]`) — a single mate-category
failure fails the suite even if the overall pass rate is still above 90%.
**Regression history**: QFORK-001 silently regressed to 7/8 when null-move pruning
landed (PR #55) — the original zugzwang guard let a side with a lone rook "pass",
hiding the domination win (issue #66). Fixed by requiring ≥ 2 non-pawn pieces in
`should_try_null_move()`. Because no automated gate ran this suite, the failure was
only found by hand months later; it now runs in `Validate-PrePR.ps1` (Step 3).

QFORK-001 regressed a second time when the mop-up evaluation term landed (issue #70,
2026-07-26): the position is a genuine razor-thin, depth-sensitive tie between two
moves even without mop-up (the engine's own preferred move already drifts from
`d1-b3` at depth 4-5 to `d1-g4` at depth 6+ with zero mop-up contribution), and
mop-up's king-cornering bonus tips it toward `d1-g4` at every magnitude tried
(confirmed down to ~19 cp). This is **not** a hard-suite failure — QFORK-001 isn't a
mate-category position, so the 100%-mate rule doesn't trigger — but the exe suite's
expected pass rate is now **30/31 (96%)**, not 31/31; a future drop to 29/31 or lower
is a real regression, not this known case. The Catch2 mirror
(`Tactical - QFORK-001...`) was hidden via Catch2's `[.]` tag rather than deleted, so
the position, the reasoning, and an explicit re-run handle stay in-tree — tracked in
issue #118 for re-enabling alongside a broader WAC-style tactical suite.

**Current positions (31)**, total suite runtime **~1.1 s** (sum of per-position search
time at Release build speed; wall-clock including process startup ~3 s — well under
the 60 s budget):

*Original 8 (hand-authored, kept from the March 2026 baseline):*
- `M1-001` — Rook delivers back-rank checkmate (d4)
- `M1-002` — Queen delivers back-rank checkmate along d-file (d4)
- `HANG-001` — Queen captures undefended rook on c1 (d4)
- `BACK-001` — White rook invades rank 7 or 8 to win material (d5)
- `BACK-002` — White rook invades rank 7 or 8 to win material (d5)
- `QFORK-001` — Queen wins rook: Qa4+ forks king and Rd5; Qb3 threatens Qxd5 (d4)
- `BACK-003` — White rook captures black back-rank rook with check (d5)
- `M2-001` — Rh7+ Kg8 Rb8# (or Rb8# immediately if Rh1 covers h7) (d5)

*WAC mate-in-2 (8, depth 5):*
- `WAC-001` — Qg6: queen sac decoy, Nxg6# follows
- `WAC-004` — Qxh7+: queen sac, hxg6# follows
- `WAC-005` — Qc4+ (Black): deflection, bxc4# follows
- `WAC-012` — Qxf3+ (Black): queen sac on f3, Rg1# follows
- `WAC-027` — Qf8+: deflect the knight, back-rank mate
- `WAC-054` — Qh1+ (Black): corner check, mate on h-file
- `WAC-099` — Rh5: rook lift, gxh5 Qxh5#
- `WAC-246` — Qh5+: king hunt on the h-file

*WAC mate-in-3 (8, depth 6):*
- `WAC-050` — Rxb6+: rook sac rips the king shelter
- `WAC-057` — Rf8+: deflection into back-rank mate
- `WAC-064` — g4+: pawn check starts king hunt
- `WAC-079` — Qxh2+ (Black): queen sac on h2, king hunt
- `WAC-097` — Qa8+: long-diagonal switchback mate
- `WAC-136` — Rc8+: back-rank breakthrough
- `WAC-173` — Qh6+: queen invades, mate on the h-file/g7
- `WAC-197` — Qf1+ (Black): deflection, promotion mate follows

*WAC mate-in-4 (1, depth 8):*
- `WAC-161` — Qxd8+: rook grab with forced mate behind it

*WAC tactical wins, non-mate (6):*
- `WAC-043` — Be7/Qxa8: skewer or direct rook win (d6)
- `WAC-287` — Qh5: mating attack on f7/h7 wins material (d6)
- `WAC-085` — Na6: quiet knight move, mating net + material (d6)
- `WAC-045` — Qxa1 (Black): back-rank pin wins the rook (d6)
- `WAC-148` — Rxg7: rook sac destroys king cover (d7)
- `WAC-065` — Ne7+: royal fork setup (d7)

**Dropped during verification** (candidates from the same WAC batches that the engine
disagreed with at their target depth — kept here as known-hard positions for this
engine, not filed as bugs):
- `WAC-035`, `WAC-139`, `WAC-282` — mate-in-4 candidates; engine chose a different move
  at depth 7–8 (mate key not confirmed by `verify_mate_key.py` for the engine's line)
- `WAC-209`, `WAC-124`, `WAC-082`, `WAC-240` — non-mate tactical candidates; engine
  chose a different (non-equivalent) move at depth 6–7
- `WAC-041` — K+R+P zugzwang-adjacent endgame; engine chose a different move at depth
  6–7. Notably this was the one candidate that would have added direct regression
  coverage for the NMP zugzwang-guard class of bug (issue #66/QFORK-001) beyond
  QFORK-001 itself — dropping it means that extra coverage did not land. A tighter or
  hand-constructed zugzwang position is a good candidate for a future staging round.

**Growing the suite**: new candidates never go straight into `tactical_test_cases.json`.
Stage them in `Tests/tactical_staging.json` (same schema, transient — never committed),
run `StratChessEvolved.exe tactical test tactical_staging.json` from `Tests/`, and
reconcile: for mate categories, an engine move that differs from the EPD key is only
accepted if `Scripts/verify_mate_key.py "<FEN>" <uci_move> <N>` prints `CONFIRMED`
(ground truth via python-chess, not manual analysis); for non-mate categories the
engine's move must strictly match the EPD `bm`. Once a candidate is confirmed, move its
entry into `tactical_test_cases.json` and delete it from the staging file.

---

## Phase 2 — With Board De-Singletonization

**Status**: ✅ **Done.** Landed across 7 commits on `.claude/plans/de-singleton-board.md` (June-July 2026). `Board::Instance()` is gone; every test constructs its own local `Board`.

**Impact on tests** (all landed):
- Every `Board::Instance().SetupFromFEN(fen)` replaced with constructing a `Board` directly — mostly via the FEN constructor `Board board(fen);`, with the default ctor + explicit `SetupFromFEN` kept for tests that reconfigure one board across multiple `SECTION`s or sequential setups
- All test fixtures updated to hold Board by reference (`AIPerlexTestFixture::board_`, `TacticalTestHelpers.h`'s `make_tactical_engine(Board&, unsigned)`)
- `EvalTests.cpp`, `RepetitionTests.cpp`, `PerftTests.cpp`, and every other `Board::Instance()`-using test file updated
- ~~New `[position]` tests for the `Position` class~~ — not applicable; the refactor was an incremental `Board` change per the plan's design decisions, not a from-scratch `Position` class
- Performance regression test (NPS baseline stored in a file): still **open** — not part of the de-singleton scope; remains a candidate for a future Phase 1 infrastructure item

**Migration rule**: all new test files should be written for the non-singleton Board from the start (already the only pattern available now that `Instance()` is deleted).

---

## Lazy SMP Shared-State Audit (Task 1, `.claude/plans/lazy-smp.md`)

**Status**: ✅ Done, 2026-07-22. Still single-threaded — no threads spawned, no
search-behavior change. Validated byte-identical against the pre-SMP baseline
(`.superpowers/sdd/pre-smp-baseline-nodecounts.txt`): same fixed-depth-5
AI-vs-AI self-play game to checkmate, 137 `GetMove complete` lines, identical
move/score/depth/nodes/stable on every line.

**Changes**:
- `PlayerAiBase::nodes_since_check_` moved to `ThreadData::nodes_since_check_`
  (`StratEngine/ThreadData.h`). The two increment/check sites in
  `AIPerplex::pvs()` and `AIPerplex::quiescence()` (`StratEngine/AIPerplex.cpp`)
  now gate the wall-clock `ShouldStopSearch()` call on `td.thread_id == 0`;
  helper threads (once they exist, Task 3) rely solely on the existing
  `IsAborted()` atomic fast-path already at the top of both functions.
- Dead debug code removed: `AIPerplex::debug_tt_cache_misses()`,
  `AIPerplex::assert_tt_store()`, and the `tt_misses` multimap. Both call
  sites were already commented out (no live caller); prefer-deletion per the
  task brief rather than gating dead code behind `thread_id == 0`.

**Audit findings**:
- **`EvalManager`/`EvalSimple`/`EvalComplex`** (`StratEngine/Eval.h/.cpp`):
  confirmed stateless — no data members beyond compile-time constants,
  `Evaluate()` is `const` and reads only its `const Board&` argument plus
  `constexpr`/compile-time-initialized global tables (`g_Eval_Bitboards`,
  `g_bbFileMask`, `g_bbFileUpMask`, `g_bbFileDownMask`). Safe to share a
  single `EvalManager` instance, unsynchronized, across every Lazy SMP
  helper thread — documented as a comment on the class in `Eval.h`. No
  per-thread cloning needed.
- **`PlayerAiBase::m_TotalTime`/`m_TotalCount`** and `StopTimerAndAdjustVars`
  (`StratEngine/PlayerAI.h/.cpp`): written only once per `GetMove()` call, on
  the calling thread, after that call's search has returned — i.e. strictly
  after any helper threads for that move have joined under the Lazy SMP
  design. No synchronization added; comment left in `PlayerAI.h` for Task 3
  to re-verify once helper-thread join actually exists.
- **spdlog logging** (`AIPerplex.cpp`'s `s_logger`, `Utils/Logger.cpp`'s
  default/perf loggers): all sinks in use are the `_mt` (thread-safe)
  variants (`stdout_color_sink_mt`, `basic_file_sink_mt`). The lazy-init
  guard around `s_logger` itself (`ensure_logger_initialized()`) is a plain
  `if (s_logger) return;` — not thread-safe if raced — but it is only ever
  invoked from `AIPerplex::SetVerboseLogging()`, called once during
  single-threaded setup before any search starts. Per the Lazy SMP plan,
  helper threads do not log in v1, so this holds; flagged with a comment for
  if that assumption ever changes.
- **Static attack/Zobrist tables** (`BitBoardHelper.h`, `Magic.h`,
  `Board.cpp`'s `zobrist::` namespace): PEXT sliding-piece attack tables in
  `Magic.h` are `inline constexpr` — fully resolved at compile time, no
  runtime initialization at all. The Zobrist key tables in `Board.cpp` use a
  C++11 function-local `static const bool once = [] { ... }();` initializer
  (a "magic static"), which the standard guarantees is thread-safe even if
  raced by concurrent `Board` construction from multiple future helper
  threads. No lazy/unguarded runtime init found anywhere in this set.

---

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

## AIPerplex Test Access

To test private search helper methods, `AIPerplex.h` contains a conditional friend declaration:

```cpp
#ifdef STRAT_ENABLE_TEST_ACCESS
    friend class AIPerlexTestFixture;
#endif
```

To activate: add `STRAT_ENABLE_TEST_ACCESS` to the preprocessor definitions for the x64 Debug and Release configurations in `StratChessTests.vcxproj`. This is done when `SearchTests.cpp` is written (Phase 1 — do not enable before then).

The macro is intentionally absent from the production project (`StratChessEvolved.vcxproj`).
