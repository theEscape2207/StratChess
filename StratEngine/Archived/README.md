# Archived Algorithms

These files are kept for historical reference only. They were removed from the active build in
February 2026 because the transposition table implementation was broken and superseded by `AIPerplex`
with the modern `TranspositionTable` class.

## Archived Files

### `AITrans.cpp` / `AITrans.h`
Plain alpha-beta search with transposition table (`PlayerAiBase` subclass).

**Why archived:** Asserts non-deterministically on mate positions. Root cause: the hash
probing/storing code around mate values returns stale or incorrect scores, occasionally
asserting on `IsLegalMove(hashScore.second)` when the retrieved move is invalid.

### `ABIterTrans.cpp` / `ABIterTrans.h`
Iterative deepening alpha-beta with transposition table and aspiration windows
(`PlayerAiIterBase` subclass).

**Why archived:** The PV line is shorter than the search depth because TT hits short-circuit
node expansion without appending their move to the PV chain. The move chain is not stored
recursively in the table, so re-using a cached score leaves `m_Line` incomplete.

### `HashElement.h`
Legacy hash element struct (`HashElement`) and `eHashFlags` enum used exclusively by the
two archived algorithms via `Board::ProbeHash`, `Board::RecordHash`, and `Board::ClearHashTable`.
All three `Board` methods were removed alongside the archived algorithms.

## Superseded By

- **`AIPerplex`** (`StratEngine/AIPerplex.cpp/.h`) — PVS with iterative deepening, aspiration
  windows, and correct transposition table integration.
- **`TranspositionTable`** (`StratEngine/TranspositionTable.cpp/.h`) — thread-safe TT with
  per-bucket `shared_mutex` locking, atomic counters, and a clean probe/store interface.
