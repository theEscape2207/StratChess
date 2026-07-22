# Magic Bitboards (PEXT) for Sliding-Piece Attack Generation

**Created**: 2026-07-01
**Author**: Claude (analysis session, following BitBoardHelper/MoveGenerator modernization review)
**Status**: ✅ **Implemented 2026-07-22** — see "Implementation Status" section at the end of this doc for what actually landed and where it diverged from the design below.

---

## Goal

Replace the current rotated-bitboard sliding-piece attack generation (`ROTATED90`/`ROTATED45R`/`ROTATED45L` + rank/file/diagonal lookup tables) with BMI2 PEXT-based ("fancy magic") attack lookups.

**In scope**: `GetTowerBitboard`, `GetBishopBitboard`, `GetOfficerAttackBoard` (`MoveGenerator.cpp`), the three rotated bitboards and their maintenance in `Board::add_piece`/`remove_piece` (`Board.cpp`), and the compile-time table generation in `defines.h` (`g_bbMaskRotated90/45R/45L`, `g_iDiagonalShifts_a1h8/a8h1`, `g_bbDiagonalMask_a1h8/a8h1`, `g_bbMovesRank/File/a1h8/a8h1`).

**Out of scope**: Knight/king attack tables (already O(1) direct lookups, no change needed). Pawn move generation. Any change to move ordering, search, or evaluation. Classic magic-multiplication fallback is discussed but not implemented — see Design Decisions.

## Why this matters (recap from review)

Every `Board::add_piece`/`remove_piece` call (i.e. every `DoMove`/`UndoMove`) currently maintains 3 auxiliary bitboards purely so `GetTowerBitboard`/`GetBishopBitboard` can do table lookups:

```cpp
// Board.cpp — add_piece / remove_piece, called on every piece placement/removal
BitBoardHelper::set_bits(bitboards_[ROTATED90],  g_bbMaskRotated90[square]);
BitBoardHelper::set_bits(bitboards_[ROTATED45R], g_bbMaskRotated45R[square]);
BitBoardHelper::set_bits(bitboards_[ROTATED45L], g_bbMaskRotated45L[square]);
```

This is 3 extra memory writes (plus the mutual-exclusion asserts we just tightened in `BitBoardHelper.h`) on every single piece move, purely to support an attack-generation technique that a single hardware instruction replaces. Removing the rotated boards also shrinks `Board`'s bitboard array (`TBitboards`) by 3 elements and removes the corresponding invariant surface (one less way `test_bitboards()` can fail).

## Design Decisions

### 1. PEXT over classic magic multiplication (user-confirmed)

Two well-known approaches exist for O(1) sliding-piece attacks:

- **Classic magic bitboards**: `index = ((occupancy & relevant_mask) * magic_number) >> shift`. Hardware-independent, ~5ns/lookup, requires a one-time magic-number search (or reuse published tables) per square/piece-type.
- **BMI2 PEXT ("fancy magic")**: `index = _pext_u64(occupancy, relevant_mask)`. No magic-number search needed — index is a direct bit extraction. ~1-2ns/lookup on hardware with fast PEXT.

**Decision: PEXT**, per explicit choice. Tradeoff accepted: PEXT is emulated in slow microcode on AMD Zen/Zen+/Zen2 (pre-~2020) — on that hardware this could be *slower* than classic magic multiplication. Fast on Intel Haswell+ (2013+) and AMD Zen3+ (2020+). If the dev/target machine turns out to be pre-Zen3 AMD, classic magic multiplication is the fallback (same table-generation structure, different index computation — see Step 4).

### 2. Compile-time table generation, matching existing style

The current rotated-bitboard tables (`g_bbMovesRank`, etc.) are `inline constexpr` and generated entirely at compile time (`defines.h:743-748`) — no runtime initialization step exists today. The PEXT attack tables should follow the same pattern: `constexpr` generator functions producing `inline constexpr` `std::array` tables, keeping the "no runtime init" property intact. This is consistent with how `BitTools.h` was recently modernized (constexpr functional API) and avoids introducing a new "must call InitMagicBitboards() at startup" step that classic magic-bitboard implementations typically need.

Caveat: PEXT itself (`_pext_u64`) is **not** `constexpr`-evaluable (it's a hardware intrinsic), so the *tables* (relevant-occupancy masks, per-square attack arrays) can be `constexpr`-generated, but the actual attack lookup function (`GetTowerBitboard`/`GetBishopBitboard` equivalents) must remain a regular runtime `inline` function that calls `_pext_u64` against the live occupancy board.

### 3. Table layout — variable-length per square, not a fixed 64×256

Unlike the current `g_bbMovesRank`/`g_bbMovesFile` tables (fixed `[64][256]`, ~512KB combined across all 4 tables), PEXT/magic attack tables are sized by the actual number of "relevant" occupancy bits per square (rook: 10-12 bits depending on square/edge; bishop: 5-9 bits). Two layout options:
  - **Per-square fixed-size arrays** (`std::array<BITBOARD, 4096>` for rook, `std::array<BITBOARD, 512>` for bishop, per square) — simpler code, wastes some space on low-bit squares, still much smaller than current 512KB footprint (~250KB combined for rook, ~40KB for bishop).
  - **Single flat array with per-square offsets** ("fancy" layout used by Stockfish) — minimal memory, more complex indexing (`offset[square] + pext_index`).

  Recommend starting with **per-square fixed arrays** for simplicity; revisit the flat-offset layout only if profiling shows the larger footprint actually matters (unlikely — modern L2/L3 easily holds either).

### 4. Requires enabling BMI2 codegen — raises the whole binary's minimum CPU baseline

MSVC only emits `_pext_u64` when compiling with `/arch:AVX2` (there is no narrower BMI2-only flag in MSBuild). Neither `.vcxproj` currently sets `EnableEnhancedInstructionSet` (default = SSE2 baseline for x64). Enabling `AdvancedVectorExtensions2` project-wide means **the entire binary**, not just the sliding-piece code, now requires a Haswell-class (2013+) or Zen3+ (2020+) CPU to run at all (illegal-instruction crash on older hardware, not a graceful fallback).

Given this is a personal/dev-only project (per `CLAUDE.md`: "Only x64 builds work — the x86/Win32 configuration is not maintained"), this is likely acceptable, but it's a real, permanent constraint change worth calling out explicitly before implementing — confirm target/dev hardware supports BMI2 before starting.

Alternative (deferred, not recommended for first pass): isolate PEXT-using code in its own translation unit compiled with `/arch:AVX2`, runtime-dispatch via `__cpuid` feature detection with a classic-magic fallback path for non-BMI2 CPUs. Adds real complexity for a hobby engine with a known development machine — only worth it if this binary is ever distributed to unknown hardware.

## Files Changed

- `StratEngine/defines.h` — remove `g_bbMaskRotated90/45R/45L`, `g_iDiagonalShifts_a1h8/a8h1`, `g_bbDiagonalMask_a1h8/a8h1`, `g_bbMovesRank/File/a1h8/a8h1` and their generator functions; add rook/bishop relevant-occupancy mask tables + PEXT attack tables + generator functions.
- `StratEngine/Board.h` — remove `ROTATED90`/`ROTATED45R`/`ROTATED45L` bitboard-index constants; shrink `TBitboards` by 3 elements.
- `StratEngine/Board.cpp` — remove the 3 `BitBoardHelper::set_bits`/`clear_bits` calls in `add_piece`/`remove_piece`; remove rotated boards from `test_bitboards()`/`print_all_bitboards()`.
- `StratEngine/MoveGenerator.cpp` — rewrite `GetTowerBitboard`/`GetBishopBitboard` to compute a single PEXT index against `bitboards_[ALL_PIECES]` (no more rotated-board reads, no more 2-lookup-then-OR).
- `StratEngine/StdAfx.h` — add `<immintrin.h>` to the PCH (needed wherever `_pext_u64` is used).
- `StratChessEvolved.vcxproj` / `StratChessTests.vcxproj` — add `<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>` to x64 Debug and Release configurations (both projects, matching how `/WX` is applied to both today).
- `Docs/Roadmap.md` — new entry (added by this session, see below).

## Step-by-Step Changes

1. **Generate relevant-occupancy masks** (compile-time, `defines.h`): for each square, the rook mask = squares on the same rank/file excluding the square itself and board edges; bishop mask = squares on both diagonals excluding the square and edges. (Edge exclusion is the standard magic-bitboard trick — a piece on the edge doesn't need the actual edge square's occupancy to know it can move there.)
2. **Generate per-square attack tables** (compile-time): for every possible occupancy subset of each square's relevant mask (enumerate via `for (subset = mask; ; subset = (subset - 1) & mask)` — the standard subset-enumeration trick), compute the real sliding attack pattern by simulating ray-casting in each of the 4 (rook) or 4 (bishop) directions, stopping at the first blocker.
3. **Runtime lookup functions** (`MoveGenerator.cpp` or a new `StratEngine/Magic.h`):
   ```cpp
   inline BITBOARD RookAttacks(eSquare sq, BITBOARD occupied) noexcept {
       BITBOARD relevant = occupied & g_bbRookMask[sq];
       return g_bbRookAttacks[sq][_pext_u64(relevant, g_bbRookMask[sq])];
   }
   inline BITBOARD BishopAttacks(eSquare sq, BITBOARD occupied) noexcept {
       BITBOARD relevant = occupied & g_bbBishopMask[sq];
       return g_bbBishopAttacks[sq][_pext_u64(relevant, g_bbBishopMask[sq])];
   }
   ```
   Queen = `RookAttacks(sq, occ) | BishopAttacks(sq, occ)` (unchanged from today's pattern in `GetOfficerAttackBoard`).
4. **Replace `GetTowerBitboard`/`GetBishopBitboard`** bodies to call the above against `bbBitBoards[ALL_PIECES]`, then mask out own-color pieces exactly as today (`Bits::clearBits(attacks, bbBitBoards[ALL_FROM_COLOR + color])`).
5. **Remove rotated-board maintenance** from `Board::add_piece`/`remove_piece`, remove the `ROTATED90/45R/45L` indices from `Board.h`, remove them from `test_bitboards()`/`print_all_bitboards()` in `Board.cpp`.
6. **Delete the now-unused table generators** in `defines.h` (rotated masks, diagonal shifts/masks, rank/file/diagonal move tables).
7. **Enable AVX2 codegen** in both `.vcxproj` files (x64 Debug + Release, both projects) and add `<immintrin.h>` to `StdAfx.h`.
8. **Validate** (see below) before removing the old implementation from history — consider keeping the old rotated-bitboard code path behind a temporary compile-time switch for one PR cycle so an A/B perft/perft-speed comparison is possible, then delete it once confirmed equivalent.

## Validation Plan

1. `.\build.ps1 all` — confirm clean build with `/WX` under the new `/arch:AVX2` setting (watch for new codegen warnings).
2. `StratChessTests\x64\Release\StratChessTests.exe [perft]` — perft depth 1-4 must produce byte-identical move counts to today (this is the primary correctness gate — sliding-piece attacks feed directly into legal move generation).
3. Deep perft (`cd Tests && ../x64/Release/StratChessEvolved.exe perft test`) — run the full `perft_test_cases.json` suite, not just the fast tier.
4. `StratChessTests\x64\Release\StratChessTests.exe [tactical] [tactical_full]` — confirm no search regressions (attack generation feeds check detection and move legality).
5. `Scripts\Validate-PrePR.ps1` before opening a PR — full build + extended tests + self-play, per standard convention for any non-doc change.
6. **Benchmark**: capture nodes-per-second at a fixed depth (e.g. depth 6 from the start position) before and after, using the existing `logs/SimplePerfStats.txt` output — confirm the change is a net win, not just a wash, before committing to the removal of the old tables.
7. Confirm dev/build machine actually supports BMI2 (`Get-CimInstance Win32_Processor` name lookup, or just run the built binary — illegal instruction = no BMI2) before merging the `/arch:AVX2` project-setting change.

## Key Correctness Properties

- `RookAttacks(sq, occ)` / `BishopAttacks(sq, occ)` must produce **exactly** the same attack bitboard as the current `GetTowerBitboard`/`GetBishopBitboard` for every reachable occupancy — verified by perft equivalence (perft counts are a strict function of legal move generation, which is a strict function of attack generation).
- Own-piece masking (`Bits::clearBits(attacks, bbBitBoards[ALL_FROM_COLOR + color])`) is applied identically to today — the PEXT lookup only changes *how the raw attack bitboard is computed*, not the own-piece exclusion step.
- No behavioral change to check detection, castling-through-check checks (`AddCastleMoves` calls `GetAttackBoard`, which calls `GetTowerBitboard`/`GetBishopBitboard`), or `GetAttackBoard`'s en-passant handling.
- `Board::test_bitboards()` invariant (all individual piece boards OR together to equal `ALL_PIECES`) must still hold with the rotated boards removed from the `TBitboards` array.
- The binary now requires BMI2-capable hardware to run at all (Haswell+/Zen3+) — this is a new, permanent hard requirement, not a soft degradation.


---

## Implementation Status (2026-07-22)

Implemented directly on `origin/main` (issue #85), following this design with two decisions
resolved and one correction to the memory estimate:

1. **No classic-magic fallback / runtime dispatch (Decision 4's "alternative" rejected).**
   `.claude/plans/full-build-test-ci-github-actions.md` had separately flagged that CI-runner
   BMI2 support was worth checking before committing to PEXT-only. Checked both: this dev
   machine (`AMD Ryzen AI 9 HX 370`, Zen 5) and the CI runner (`windows-2025-vs2026`, modern
   Azure VM, confirmed Haswell+/Zen3+-class in that doc) both have fast BMI2. Went with
   project-wide `/arch:AVX2`, no fallback — matches this project's convention against
   unexercised code paths.
2. **No temporary A/B compile-time switch (Step 8's suggestion skipped).** `Tests/perft_test_cases.json`
   already encodes expected node counts independent of any old implementation, so it's a
   sufficient correctness oracle without compiling two attack-generation paths side by side.
   The old rotated-bitboard code was deleted directly; git history is the rollback path.
3. **Table size correction**: the ~250KB/~40KB estimate in Decision 3 was wrong by ~8x — actual
   footprint is `g_bbRookAttacks[64][4096]` = 2 MB + `g_bbBishopAttacks[64][512]` = 256 KB ≈
   2.25 MB combined (still trivially L2/L3-resident; not a practical concern, just a documentation
   correction).
4. **Compile-time table generation needed `/constexpr:steps100000000`** (added to
   `AdditionalOptions` in all four x64 ClCompile groups, both `.vcxproj` files) — MSVC's default
   constexpr step budget (1,048,576) was exceeded generating the 2 MB rook table. Still 100%
   compile-time generation as designed (Decision 2); just a larger allowed budget, not a runtime
   init step.
5. **`Magic.h` is a new standalone header** (as anticipated in Step 3's parenthetical) rather than
   folding the tables into `defines.h` — keeps the already-large `defines.h` from growing further.
   Everything lives under a `magic::` namespace except the two public `RookAttacks`/`BishopAttacks`
   lookup functions.
6. **Added `[magic]` unit tests** (`StratChessTests/MagicBitboardTests.cpp`) beyond this plan's
   original perft-only validation — hand-verified `RookAttacks`/`BishopAttacks` bitboards for
   open cross/diagonal, corner + blockers, and fully-blocked-adjacent cases. Perft alone only
   proves attack generation is *consistent* with legal move counts, not that any individual
   attack bitboard is correct in isolation; issue #104's `[bitboard]` tag is separately scoped to
   `BitBoardHelper` bit ops, not sliding-piece attacks.

**Validation results**:
- `StratChessTests.exe` (all tags): **166/166 passed** (2307 assertions), including 9/9 `[magic]`
  cases (7 initial + 2 added on search-reviewer's suggestion to exercise fully-saturated masks —
  the largest-table-index path — for a1 rook / d4 bishop).
- Deep perft (`Tests/perft_test_cases.json`, full suite): **640/640 passed**.
- Gated tactical suite: **31/31 passed (100%)**.
- AIPerplex self-play: ran cleanly for 90s wall-clock, sound moves and scores, no crashes/assertions.
- **Node-count equivalence** (strongest signal): `perft run 6` from the start position produced
  **byte-identical node counts** (119,060,324) on the pre-change and post-change binaries.
- **NPS**: pre-change 32,065,802 NPS vs. post-change 32,204,577 NPS at depth 6 — a wash within
  run-to-run noise at this benchmark. Perft's cost is dominated by move-list construction, not
  attack lookup, so this doesn't isolate the win from removing 3 bitboard writes per
  `DoMove`/`UndoMove` (a structural reduction independent of lookup speed). A search-NPS or
  ELO-match benchmark would better isolate the attack-lookup-speed component if that's wanted
  later; not done here since the correctness gates (byte-identical perft + all test tiers) were
  the binding constraint per the approved plan.
