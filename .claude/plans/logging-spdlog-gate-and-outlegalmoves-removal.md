# Plan: AIPerplex Logging (spdlog Level Gate) + outLegalMoves Removal

## Context

Two problems addressed together:

1. **AIPerplex log helpers repeat a 3-line guard** (`IsVerboseLoggingEnabled` → `ensure_logger_initialized` → `if (!s_logger)`) in every one of 7 private methods plus 3 inline sites. The fix is to use spdlog's own level system as the single gate, initialized once in `SetVerboseLogging()`, so log helpers only need a null-check.

2. **`outLegalMoves` / `legalmoves.txt` is a pre-spdlog diagnostic stream** that predates the current logging strategy. It is a global `std::ofstream` declared `extern` in 6 active files, written by the legacy agents (AIBasic, AIAgent, ABIterative) and Game. `PRINT_MOVES` is always defined so the writes ARE active. AIPerplex already never uses it. The entire pattern should be migrated to spdlog and removed.

Post-change the plan also documents which files are created in which context, and adds `aiperplex.log` to `.gitignore`.

**Addendum**: After the main refactor, all log file paths were consolidated under `logs/` for consistency — `aiperplex.log` → `logs/aiperplex.log`, `SimplePerfStats.txt` → `logs/SimplePerfStats.txt`, `gamelist.txt` → `logs/gamelist.txt`. All four runtime log files now land in the same `logs/` subdirectory.

---

## Design Decisions

### AIPerplex: spdlog level as the sole gate
- `SetVerboseLogging(true)` calls `ensure_logger_initialized()` (creates `logs/aiperplex.log` once) then sets `s_logger->set_level(debug)`.
- `SetVerboseLogging(false)` sets `s_logger->set_level(off)` — spdlog internally no-ops any log call.
- Log helpers only need `if (!s_logger) return;`. The `IsVerboseLoggingEnabled()` bool check and `ensure_logger_initialized()` are removed from all helpers.
- **Trade-off**: `logs/aiperplex.log` is created in tests because the constructor calls `SetVerboseLogging(true)`. The file will be empty (level=off). Add it to `.gitignore`. This is acceptable; the alternative (changing constructor default) is out of scope.
- **Exception — `handle_empty_move_emergency`**: This logs at `critical` regardless of verbose state. Under `level::off`, even critical is suppressed on `s_logger`. Fix: migrate these specific calls to `spdlog::default_logger()->critical(...)` which is always active. This is architecturally correct — a "no legal moves found" emergency is a process-level event, not search diagnostics.
- **Methods with expensive pre-formatting** (`log_iteration_eval`, `log_completed_iteration` build PV strings): add `if (!s_logger->should_log(spdlog::level::debug)) return;` after the null-check to skip string building when level is off. `should_log()` is a single atomic comparison.
- `IsVerboseLoggingEnabled()` stays public (may be used by external callers for their own guards).
- `SetVerboseLogging()` moves from inline in header to non-inline in `.cpp` (now calls `ensure_logger_initialized()`).

### outLegalMoves: full removal
- `PrintMovesAndScore` calls in AIBasic/AIAgent/ABIterative replaced with `spdlog::default_logger()->debug(...)` — same data, no `#ifdef` guard needed (spdlog level controls visibility).
- `test_bitboards(std::ostream&)` in Board: change signature to `bool test_bitboards() const`, write internally to a `std::ostringstream` then emit via `spdlog::default_logger()->error(...)`. The `assert()` call sites become `assert(test_bitboards())` — identical assert semantics.
- `Game::PrintBoardAndMove` `#ifdef PRINT_MOVES` block: delete entirely (spdlog already covers it fully and with more detail).
- `PRINT_MOVES` macro in `defines.h`: delete.
- `PrintMovesAndScore` static helper in `PlayerAI.h`: delete.
- `extern std::ofstream outLegalMoves` externs: delete from all 6 active files.
- Global `std::ofstream outLegalMoves` definition in `StratChessEvolved.cpp`: delete.
- Test linker stub in `StratChessTests.cpp`: delete.
- Archived files (`AITrans.cpp`, `ABIterTrans.cpp`): leave as-is (not compiled).
- `Game::movesFile_` / `logs/gamelist.txt`: separate concern, instance-owned, already using `MoveFormatter` — migrated to `logs/` path but otherwise not changed.

---

## Files Changed

| File | Change |
|---|---|
| `StratEngine/AIPerplex.h` | `SetVerboseLogging` from inline → declaration only |
| `StratEngine/AIPerplex.cpp` | `SetVerboseLogging` impl; remove 3-line boilerplate from 7 helpers + 3 inline sites; handle_empty_move_emergency → default_logger; delete extern; add `should_log()` guard to 2 string-building helpers; path → `logs/aiperplex.log` |
| `StratEngine/Board.h` | `test_bitboards` signature: `(std::ostream&)` → `()` |
| `StratEngine/Board.cpp` | `test_bitboards` impl → ostringstream + spdlog; update 2 assert call sites; delete extern |
| `StratEngine/Game.cpp` | Delete extern; delete `#ifdef PRINT_MOVES` block; paths → `logs/gamelist.txt`, `logs/SimplePerfStats.txt` |
| `StratEngine/AIBasic.cpp` | Delete extern + both `#ifdef PRINT_MOVES` blocks; replace with `spdlog::default_logger()->debug(...)` |
| `StratEngine/AIAgent.cpp` | Delete extern + both `#ifdef PRINT_MOVES` blocks; replace with `spdlog::default_logger()->debug(...)` |
| `StratEngine/ABIterative.cpp` | Delete extern + both `#ifdef PRINT_MOVES` blocks; replace with `spdlog::default_logger()->debug(...)` |
| `StratEngine/PlayerAI.h` | Delete `PrintMovesAndScore` static helper |
| `StratEngine/defines.h` | Delete `#define PRINT_MOVES 1;` |
| `StratChessEvolved/StratChessEvolved.cpp` | Delete global `std::ofstream outLegalMoves(...)` definition |
| `StratChessTests/StratChessTests.cpp` | Delete linker stub `std::ofstream outLegalMoves;` |
| `CLAUDE.md` | Add "Log and output files" section documenting all 4 runtime-created files |
| `.gitignore` | Update entries to `logs/` paths; remove stale catch-alls |

---

## Step-by-Step Changes

### Step 1 — `AIPerplex.h`: De-inline `SetVerboseLogging`
```cpp
// Remove the inline body; keep declaration only:
static void SetVerboseLogging(bool enabled) noexcept;
```

### Step 2 — `AIPerplex.cpp`: Implement `SetVerboseLogging`, remove boilerplate

**New `SetVerboseLogging` implementation**:
```cpp
void AIPerplex::SetVerboseLogging(bool enabled) noexcept {
    s_verbose_logging = enabled;
    if (enabled) {
        ensure_logger_initialized();
        if (s_logger) s_logger->set_level(spdlog::level::debug);
    } else if (s_logger) {
        s_logger->set_level(spdlog::level::off);
    }
}
```

**7 dedicated `log_*` helpers** — remove the 3-line boilerplate, keep only null-check:

For `log_rejection`, `log_acceptance`, `log_search_complete`, `log_aspiration_retry`, `log_aspiration_full_window`:
```cpp
// Before: 3 lines → After: 1 line
if (!s_logger) return;
```

For `log_iteration_eval` and `log_completed_iteration` (build PV strings before logging):
```cpp
if (!s_logger) return;
if (!s_logger->should_log(spdlog::level::debug)) return;  // skip expensive string build
```

**Inline patterns** (`GetMove`, `should_stop_early` ×2): collapse
`if (IsVerboseLoggingEnabled()) { ensure_logger_initialized(); if (s_logger) {...} }` → `if (s_logger) { s_logger->...(...); }`

**`handle_empty_move_emergency`**: remove `ensure_logger_initialized()`; replace all `s_logger->critical/info(...)` with `spdlog::default_logger()->critical/info(...)`.

### Step 3 — `Board.h` / `Board.cpp`: migrate `test_bitboards`

Signature: `bool test_bitboards(std::ostream&) const` → `bool test_bitboards() const`

Implementation: use ostringstream, emit via `spdlog::default_logger()->error(...)` on failure.

Assert call sites: `assert(test_bitboards(outLegalMoves))` → `assert(test_bitboards())`.

### Step 4 — Legacy agents: replace `PrintMovesAndScore` with spdlog

AIBasic/AIAgent/ABIterative — delete `#ifdef PRINT_MOVES` blocks + externs; replace with:
```cpp
if (ply == 0)
    spdlog::default_logger()->debug("Root move {}/{}: {} score={}",
        counter, moveList.size(), curMove.Output(), value);
```

### Step 5 — `Game.cpp`: delete `#ifdef PRINT_MOVES` block + extern

### Step 6 — Delete supporting infrastructure
- `PlayerAI.h`: delete `PrintMovesAndScore` static helper
- `defines.h`: delete `#define PRINT_MOVES 1;`
- `StratChessEvolved.cpp`: delete global `std::ofstream outLegalMoves(...)`
- `StratChessTests.cpp`: delete stub `std::ofstream outLegalMoves;`

### Step 7 — Centralize all log paths under `logs/`
- `AIPerplex.cpp`: `"aiperplex.log"` → `"logs/aiperplex.log"`
- `Game.cpp`: `"SimplePerfStats.txt"` → `"logs/SimplePerfStats.txt"`; `"gamelist.txt"` → `"logs/gamelist.txt"`
- `.gitignore`: update entries to `logs/` paths

### Step 8 — Documentation
- `.gitignore`: update gitignore entries for all four `logs/` files
- `CLAUDE.md`: add "Log and output files" section with table of all 4 runtime files

---

## Validation Plan

1. **Build**: `.\build.ps1` (Release|x64) — zero warnings, zero errors
2. **Tests**: `.\build.ps1 run-tests` — all existing tags must pass (`[repetition]`, `[perft]`, `[tt]`, `[eval]`, `[tactical]`, `[formatter]`, `[board]`)
3. **Verify `legalmoves.txt` is NOT created**: run game mode, confirm no `legalmoves.txt` appears
4. **Verify `logs/aiperplex.log` IS created in game mode** and contains search diagnostic output
5. **AIPerplex self-play** (`"type": 6` both sides): confirm `GetMove complete` line still appears in stdout
6. **AIAgent self-play** (`"type": 3` both sides): confirm root move scores appear in `logs/multisink.txt` at debug level
7. **Bitboard assert path**: `test_bitboards()` signature change compiles; assert behavior in Debug preserved

---

## Key Correctness Properties

- `handle_empty_move_emergency` critical logs always appear regardless of `s_verbose_logging` state (uses `spdlog::default_logger()` not `s_logger`)
- `spdlog::level::off` suppresses ALL levels including critical on `s_logger` — this is why emergency logs move to the default logger
- PV string building in `log_iteration_eval` and `log_completed_iteration` is guarded by `should_log(debug)` — no unnecessary allocation when verbose is off
- `test_bitboards` assert semantics unchanged: fires in Debug, removed in Release; spdlog call happens before `return false`
- No `extern std::ofstream outLegalMoves` remains in any active translation unit after this change
- Archived files (`AITrans.cpp`, `ABIterTrans.cpp`) retain their `outLegalMoves` references but are not compiled — leave as-is
- All four runtime log files (`logs/multisink.txt`, `logs/aiperplex.log`, `logs/SimplePerfStats.txt`, `logs/gamelist.txt`) require `logs/` to pre-exist in the working directory
