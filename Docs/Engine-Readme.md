# StratChess Engine Documentation

**Version**: 3.0 (August 2026)  
**Language**: C++20  
**Primary Algorithm**: AIPerplex (PVS + Transposition Tables + Iterative Deepening + Lazy SMP)

---

## Table of Contents
1. [Overview](#overview)
2. [Quick Start](#quick-start)
3. [Architecture](#architecture)
4. [File Structure](#file-structure)
5. [Key Algorithms](#key-algorithms)
6. [Data Structures](#data-structures)
7. [Dependencies](#dependencies)
8. [Current Status](#current-status)
9. [Performance Characteristics](#performance-characteristics)

---

## Overview

StratChess is a chess engine that has evolved over 20 years, featuring multiple search algorithm implementations. The current production algorithm is **AIPerplex**, which combines Principal Variation Search (PVS), transposition tables, quiescence search, and iterative deepening with sophisticated timeout handling.

### Key Features
- **Principal Variation Search (PVS)** with null-window optimization
- **Transposition Tables** with separate MAIN/QUIESCENCE phases
- **Iterative Deepening** with robust timeout and quality assessment
- **Aspiration windows** around the previous iteration's score
- **Quiescence Search** for tactical stability
- **Null-move pruning** and **Late Move Reductions (LMR)**
- **Killer moves** (2 per ply) and **history heuristic** for move ordering
- **Lazy SMP** parallel search — helper threads share the transposition table
- **Bitboard representation** with PEXT magic bitboards for sliding attacks
- **Comprehensive logging** via spdlog for diagnostics
- **Tunable parameters** for search behavior

### Design Philosophy
- **Correctness first**: Robust handling of edge cases (mate, stalemate, timeouts)
- **Explicit over implicit**: Clear data flow, minimal hidden state
- **Maintainability**: Well-structured code with helper methods
- **Performance**: Efficient algorithms with room for optimization
- **Diagnostics**: Extensive logging for analysis and debugging

---

## Quick Start

### Building

CMake is the only build system. `build.ps1` wraps the presets in `CMakePresets.json` and imports the
Visual Studio environment itself, so it works from any shell. **clang-cl is what ships**; MSVC is for
development and debugging only, and must never be used for measurement.

```powershell
.\build.ps1                     # engine + tests (Release, clang-cl)
.\build.ps1 run-tests           # build tests, run the fast tier
.\build.ps1 all -Config Debug   # debug build
```

Binaries land in `build/<preset>/`. See `CLAUDE.md` for the full build contract.

### Driving a search

```cpp
Board board;
board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

// AIPerplex is the standalone concrete search service. It retains search
// resources, but never a caller's board, result, or observer.
AIPerplex ai(AIPerplexConfig{.default_depth = 20, .threads = 4});

SearchLimits limits;                     // every constraint is optional
limits.movetime = std::chrono::milliseconds(5000);

SearchResult result = ai.Search(board, limits);
Move best = result.best_move;            // value also carries state, elapsed time and node counts
```

`SearchLimits` carries every per-call constraint (clock / movetime / depth / infinite). Each
`Search()` call is self-contained — there is no pre-call ordering contract to satisfy. An optional
iteration observer belongs to that call and completed telemetry is returned by value, so a later
search cannot overwrite a previous result.

Game mode keeps the stable `IPlayer::GetMove(const SearchLimits&)` contract through
`SearchPlayer { Board&, AIPerplex value }`; it supplies the Game board to the concrete service on
each move. `CreatePlayer(PlayerConfig, Board&, PlayerCreationOptions)` is the composition root: it
maps evaluator, defaults, tuning, threads and per-instance logging before type erasure, then starts
the player's new-game lifecycle. A generic `ISearchEngine` is deliberately deferred: the one real
service keeps this boundary concrete without introducing an abstraction that has no second use.

### Configuration
```cpp
SearchTuning tuning{};
tuning.min_nodes_threshold = 1000;      // Minimum nodes for valid search
tuning.min_completion_ratio = 0.10;     // 10% of previous depth required
tuning.min_pv_ratio = 0.33;             // PV must be 1/3 of depth
tuning.score_draw_threshold = 20;       // Suspicious score=0 detection

AIPerplex ai(AIPerplexConfig{.default_depth = 20, .threads = 4, .tuning = tuning});
```

---

## Architecture

### High-Level Design

```
┌──────────────────────────┐   ┌──────────────────────────┐
│   UCI Front End          │   │   Interactive Game Loop  │
│   (UCIHandler.cpp/h)     │   │   (Game.cpp/h)           │
│   stdin/stdout protocol  │   │   console play           │
└───────────┬──────────────┘   └───────────┬──────────────┘
            │                              │
            └──────────────┬───────────────┘
                           ▼
┌───────────────────────────────────────────────────────────┐
│ Game: IPlayer                                               │
│ ├─ PlayerHuman                                              │
│ ├─ PlayerAiBase → legacy AIAgent / ABIterative / AIBasic    │
│ └─ SearchPlayer { Board&, AIPerplex value }                 │
└────────────────────────────┬──────────────────────────────┘
                             │ board per GetMove()
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ AIPerplex concrete search service                            │
│ UCI owns one directly; Game reaches one through SearchPlayer │
└─────────────────────────────────────────────────────────────┘
                                                           │
┌──────────────────────────────────────────────────────────┴──┐
│                   Search Components                         │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Lazy SMP (threads > 1)                                │ │
│  │  ├─ N-1 helper threads, each with its own ThreadData   │ │
│  │  ├─ Shared transposition table                         │ │
│  │  └─ Main thread authoritative — helpers report nothing │ │
│  └─────────────┬──────────────────────────────────────────┘ │
│                ▼                                            │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Iterative Deepening + Aspiration Windows              │ │
│  │  ├─ Depth 1, 2, 3... up to max_depth                   │ │
│  │  └─ Timeout handling with quality assessment           │ │
│  └─────────────┬──────────────────────────────────────────┘ │
│                ▼                                            │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Principal Variation Search (PVS)                      │ │
│  │  ├─ Alpha-Beta pruning                                 │ │
│  │  ├─ Null-window search for non-PV nodes                │ │
│  │  ├─ Null-move pruning / Late Move Reductions           │ │
│  │  ├─ Re-search on fail-high                             │ │
│  │  └─ Transposition table probe/store                    │ │
│  └─────────────┬──────────────────────────────────────────┘ │
│                ▼                                            │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Quiescence Search                                     │ │
│  │  ├─ Stand-pat evaluation                               │ │
│  │  ├─ Capture-only search                                │ │
│  │  ├─ MVV-LVA move ordering                              │ │
│  │  └─ Depth limit                                        │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                  │
                  ▼
┌──────────────────────────────────────────────────────────────┐
│                   Support Infrastructure                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │ Board        │  │ Move         │  │ Evaluator    │        │
│  │ Bitboards    │  │ Generator    │  │ Material +   │        │
│  │ Zobrist Hash │  │ PEXT magics  │  │ Position     │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │ Transposition│  │ PV Table     │  │ Move         │        │
│  │ Table        │  │ Principal    │  │ Ordering     │        │
│  │ shared, 256MB│  │ Variation    │  │ MVV-LVA +    │        │
│  │              │  │              │  │ killers/hist │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
└──────────────────────────────────────────────────────────────┘
```

### Threading model

`AIPerplex::SetThreads(N)` selects Lazy SMP width; the shipping default is 1, and at 1 no helper
threads are spawned at all — that path is byte-identical to the pre-SMP single-threaded code.

Above 1, `Search()` spawns `N-1` `std::jthread` helpers for the duration of the call. Each helper
owns its own `ThreadData` (its own `Board` copy, PV, killers, history and node counter) and shares
only the transposition table and the atomic abort flag. Helpers never report a move: only the main
thread's result is authoritative. They exist to warm the shared TT.

Over UCI the width is set with `setoption name Threads value N`; in game mode it comes from the
`"threads"` key in `game_settings.json`.

### Data Flow

```
Search(root, limits, observer) Entry
    ↓
Initialize ThreadData (board copy, PV, counters)
    ↓
Spawn N-1 Lazy SMP helpers (if threads > 1)
    ↓
Iterative Deepening Loop (depth 1 → max_depth)
    │
    ├─→ Execute PVS at current depth (aspiration window)
    │       ↓
    │   Generate Moves → Sort by value → Search each
    │       ↓
    │   Recursive PVS calls (decreasing depth)
    │       ↓
    │   Bottom of tree → Quiescence Search
    │       ↓
    │   Evaluate position
    │
    ├─→ Gather Iteration Metrics
    │   (nodes, score, move, PV length, interrupted?)
    │
    ├─→ Assess Quality
    │   (if interrupted: check completeness)
    │
    ├─→ Decision: Accept/Reject/Continue
    │
    └─→ Early Termination Check
        (mate found or forced line)
    ↓
Join helpers, aggregate node counts
    ↓
Return SearchResult
    ↓
Move extracted and played
```

---

## File Structure

> The tables below are maintained by hand and have drifted before. Treat the directory tree as the
> source of truth; these are an orientation aid, not an inventory contract.

### Core Search Algorithms

| File | Description | Status |
|------|-------------|--------|
| `AIPerplex.cpp/h` | **Production algorithm**: PVS + TT + ID + Quiescence + Lazy SMP | ✅ Active |
| `AIAgent.cpp/h` | Aspiration windows baseline | ✅ Active |
| `ABIterative.cpp/h` | Simple iterative deepening | 🎭 Legacy (nostalgic) |
| `AIBasic.cpp/h` | Basic alpha-beta | 🎭 Legacy (nostalgic) |
| `Archived/ABIterTrans.cpp/h` | Broken TT implementation | ❌ Archived — not built |
| `Archived/AITrans.cpp/h` | Broken TT implementation | ❌ Archived — not built |

### Game Infrastructure

| File | Description |
|------|-------------|
| `Board.cpp/h` | Bitboard representation, move execution, Zobrist hashing |
| `Move.cpp/h` | Move encoding (16-bit), move value calculation |
| `MoveGenerator.cpp/h` | Legal move generation, capture generation |
| `Magic.h` | PEXT magic bitboards for sliding-piece attacks |
| `Game.cpp/h` | Game loop, player management, move validation |
| `GameState.h` | Game state enum (playing, checkmate, stalemate, draw) |
| `UCIHandler.cpp/h` | UCI protocol command loop — the production entry point |

### Evaluation & Ordering

| File | Description |
|------|-------------|
| `Eval.cpp/h` | Position evaluation (material + positional, tapered) |
| `Sort.cpp/h` | Move ordering utilities (MVV-LVA) |

### Data Structures

| File | Description |
|------|-------------|
| `TranspositionTable.h/cpp` | Hash table for position caching (256 MB requested) |
| `ThreadData.h` | Per-search state: board copy, PV, killers, history, counters |
| `SearchLimits.h` | Per-call search constraints (clock / movetime / depth / infinite) |
| `PVTable.h` | Principal variation storage |

### Base Classes & Interfaces

| File | Description |
|------|-------------|
| `IPlayer.h` | Player interface (AI and Human) |
| `PlayerBase.cpp/h` | Shared player implementation and legacy helpers |
| `PlayerFactory.cpp/h` | Config-aware player construction before type erasure |
| `PlayerAI.cpp/h` | AI player base class (`PlayerAiBase`) |
| `PlayerAiIterBase.h` | Iterative AI base (time management) |
| `PlayerHuman.cpp/h` | Human player input handling |

### Utilities

| File | Description |
|------|-------------|
| `Utils/Logger.cpp/h` | Logging infrastructure (spdlog wrapper) |
| `Utils/FENParser.cpp/h` | FEN string parsing for position setup |
| `Utils/TimeManager.h` | Time control management (soft/hard limits) |
| `Utils/TimeUtils.h` | Time budget formula |
| `Utils/BitTools.h` | Bitboard manipulation utilities |
| `Utils/Formatters.h` | Output formatting helpers |
| `MoveFormatter.h` | All move formatting (coordinate, short, UCI, verbose) |

### Configuration & Constants

| File | Description |
|------|-------------|
| `defines.h` | Core enums (Color, PieceType, Square, GameValues) |
| `Config.cpp/h` | Engine configuration |
| `StdAfx.h` | Shared common-include header |

### Helper Headers

| File | Description |
|------|-------------|
| `BitBoardHelper.h` | Bitboard manipulation macros |
| `MoveHelper.h` | Move construction helpers |
| `PieceHelper.h` | Piece type utilities |
| `SquareHelper.h` | Square calculation utilities |

---

## Key Algorithms

### 1. Principal Variation Search (PVS)

**Location**: `AIPerplex::pvs()`

**Description**: An enhancement of alpha-beta that uses null-window searches for most nodes, with re-searches when needed.

**Algorithm** (simplified — omits null-move pruning, LMR and killer/history ordering; see the source for those):
```
pvs(depth, alpha, beta, ply, is_pv_node):
    if depth <= 0:
        return quiescence(alpha, beta)
    
    # Transposition table probe
    if TT has entry and depth matches:
        if not PV node:
            apply bound cutoffs
    
    # Generate and sort moves
    moves = generate_legal_moves()
    sort_moves(moves, pv_move, hash_move)
    
    # Search first move with full window
    score = -pvs(depth-1, -beta, -alpha, ply+1, is_pv_node)
    best_score = score
    
    # Search remaining moves with null window
    for each remaining move:
        score = -pvs(depth-1, -alpha-1, -alpha, ply+1, false)
        
        # Re-search if it beats alpha
        if score > alpha and is_pv_node:
            score = -pvs(depth-1, -beta, -alpha, ply+1, true)
        
        if score > best_score:
            best_score = score
            if score > alpha:
                alpha = score
            if score >= beta:
                break  # Beta cutoff
    
    # Store in transposition table
    TT.store(position, best_score, depth, best_move, bound)
    
    return best_score
```

**Key Features**:
- **Null-window search**: Most nodes searched with minimal window (-alpha-1, -alpha)
- **Re-search**: Only re-search with full window when null-window fails high
- **PV tracking**: Updates principal variation when alpha improves
- **TT integration**: Probes before search, stores after
- **Move ordering**: PV move > Hash move > Captures > Killers > History
- **Null-move pruning**: Gated by `should_try_null_move()` — covers zugzwang, mate-score contamination, consecutive nulls, PV/in-check and a minimum depth
- **Late Move Reductions**: Later quiet moves searched at reduced depth, re-searched on fail-high

---

### 2. Iterative Deepening with Quality Assessment

**Location**: `AIPerplex::iterative_deepening()`, `AIPerplex::search_with_aspiration()`

**Description**: Searches depth 1, 2, 3... until time runs out, with sophisticated interrupted search handling. Each iteration is searched inside an aspiration window centred on the previous iteration's score, widening on fail-high or fail-low.

**Algorithm**:
```
iterative_deepening(max_depth):
    state = empty_search_state()
    
    for depth = 1 to max_depth:
        nodes_at_start = node_count
        
        # Execute search at this depth (aspiration window around previous score)
        score = search_with_aspiration(depth, previous_score)
        
        # Gather metrics
        metrics = {
            move: pv_table[0],
            score: score,
            nodes: node_count - nodes_at_start,
            pv_length: pv_table.length(),
            interrupted: timeout_reached(),
            completion_ratio: nodes / previous_nodes
        }
        
        # Decide: Accept or Reject this depth?
        if not interrupted:
            ACCEPT_AND_CONTINUE
        else:
            quality = assess_quality(metrics, state)
            if quality == GOOD:
                ACCEPT_AND_STOP
            else:
                REJECT_AND_STOP  # Use previous depth
        
        # Check for early termination
        if mate_found or forced_line:
            STOP
    
    return SearchResult(state.best_move, state.best_score, ...)
```

**Quality Assessment Checks**:
1. **Empty move**: Obvious failure
2. **Barely searched**: < 1000 nodes
3. **Too few nodes**: < 10% of previous depth
4. **Short PV**: < 33% of depth
5. **Suspicious score=0**: Previous score wasn't near 0
6. **Move changed**: Different move on interrupt

**Key Innovation**: Multi-metric validation prevents accepting incomplete searches while not rejecting valid results in tactical positions.

---

### 3. Quiescence Search

**Location**: `AIPerplex::quiescence()`

**Description**: Searches only tactical moves (captures) until position is "quiet" to avoid horizon
effects. In check it becomes a small full-width search instead: the side to move may not decline to
move, so neither stand-pat nor a capture-only move list is legal there.

**Algorithm**:
```
quiescence(alpha, beta, budget):
    in_check = board.InCheck()

    # budget counts plies still to come, so larger means more search — the same
    # unit pvs() uses for depth, and what both phases store in the TT.
    if budget < 0 and not in_check:
        return evaluate()

    if is_repetition_or_fifty_move():   # reachable only via quiet evasions
        return draw
    if ply >= MAX_PLY - 1:              # absolute backstop
        return evaluate()

    entry = tt.probe()
    if entry.phase == QUIESCENCE and entry.depth >= budget:
        ... cutoff / window narrowing ...

    if not in_check:
        # Stand-pat: can we already cutoff?
        stand_pat = evaluate()
        if stand_pat >= beta:
            return beta
        if stand_pat > alpha:
            alpha = stand_pat

    moves = generate_legal_moves() if in_check else generate_captures()
    sort_by_MVV_LVA(moves)

    best_score = stand_pat if not in_check else -INFINITY
    for each move:
        score = -quiescence(-beta, -alpha, budget - 1)

        if score >= beta:
            return beta
        if score > best_score:
            best_score = score
            alpha = max(alpha, score)

    if in_check and no move was legal:
        return -MATE + ply

    return best_score
```

**Key Features**:
- **Stand-pat**: Always have option to not capture — except in check, where it does not exist
- **Capture-only**: Dramatically reduces search tree; in check, all legal evasions are generated
- **MVV-LVA ordering**: Most Valuable Victim - Least Valuable Attacker
- **Budget limit**: Prevents infinite capture sequences; in check it is bypassed, and the draw
  checks plus the absolute ply backstop bound the recursion instead
- **TT caching**: Stores quiescence results separately from main search, keyed on remaining budget

**Future Enhancement**: Delta pruning and SEE-based pruning (skip captures that can't improve alpha)

---

### 4. Transposition Table

**Location**: `TranspositionTable.h/cpp`

**Description**: Hash table caching position evaluations to avoid re-searching identical positions. Shared across all Lazy SMP threads.

**Entry Structure**:
```cpp
struct TTEntry {
    std::uint64_t key;      // Zobrist hash
    int16_t value;          // Evaluation score (mate scores normalized by ply)
    int16_t depth;          // Search depth
    SearchPhase phase;      // MAIN or QUIESCENCE
    BoundType bound;        // EXACT, LOWER, or UPPER
    NodeType node_type;     // PV, CUT, or ALL
    uint8_t age;            // Search generation, for replacement
    Move best_move;         // Best move found
};
```

**Layout**: four entries per bucket, power-of-two bucket count for mask indexing.

**Concurrency**: one `std::shared_mutex` per bucket — probes take a shared lock, stores take the
exclusive side. Whether that cost is worth removing is an open measurement question.

**Replacement Strategy**: `replacementScore()` weighs depth, age, node type and search phase — PV
entries get a bonus, quiescence depth is scaled down to a main-search equivalent, and older entries
are penalised. An exact key match is scored the same way against the entry it would replace, so a
quiescence result cannot displace the main entry for the same position and a shallow re-visit cannot
discard a deeper one. An equal score is settled on the raw phase, depth and bound the ranking rounds
away, and only a store that nothing separates from the entry it lands on overwrites. A store that
wins the slot but carries no move keeps the one already there.

**Size**: 256 MB requested. Note that the bucket count is rounded *down* to a power of two, so the
allocation is smaller than the request and `memory_mb()` reports the request rather than the
allocation — a known bug, tracked separately. There is currently no UCI `Hash` option; the size is
fixed at construction.

---

### 5. Move Ordering

**Current Implementation**: `AIPerplex::pvs()` (inline sorting) and `Sort.cpp/h`

**Order of Priority**:
1. **PV move** (from previous iteration)
2. **Hash move** (from TT)
3. **Captures** (by MVV-LVA)
4. **Killer moves** (2 per ply, from `ThreadData`)
5. **History heuristic** (aged, survives across moves)

**Future Enhancement**: Extract to a `MoveSorter` class; counter-move history; SEE-based capture ordering

---

## Data Structures

### ThreadData

**Location**: `ThreadData.h`

All per-search state, so a Lazy SMP helper can run without touching anything the main thread owns:
its own `Board` copy, node counters, PV table, killer moves, history table and null-move flags. The
position metadata a search needs — en-passant square, castling rights, halfmove clock, last move —
lives on that `Board` and nowhere else, so there is no second sequence that can disagree with it.

`ThreadData&` is the **first parameter of every search method**. The search runs on `td.board`, never
on the game board. The transposition table stays a separate, explicitly passed shared parameter —
that is the one thing helpers deliberately share.

---

### SearchResult

**Location**: `SearchResult.h` — its own header because both `IPlayer::GetMove()` and the concrete
search service return it without exposing either implementation.

```cpp
struct SearchResult {
    Move best_move;           // Best move found
    int best_score;           // Evaluation score
    int depth_completed;      // Actual depth searched
    GameStates game_state;    // Outcome adjudicated at this player's own root
    int64_t nodes_searched;   // Main-tree node count
    int64_t qnodes_searched;  // Quiescence-tree node count, kept apart
    std::chrono::milliseconds elapsed; // Completed-search duration
    bool search_was_stable;   // Move unchanged in late depths
};
```

**Purpose**: everything one call produced, in one returned value. It is the only channel by which a
player or concrete search service reports a move, score, root outcome and telemetry — there is no
result cache to read afterwards. `Game` owns the combined elapsed/node totals used for its six-column
performance rows; no player keeps cross-player accounting.

`game_state` is never `DRAW_50_MOVES`: the fifty-move rule is a fact about the position after the
move is committed, which only `Game::Run()` can see. Legacy agents place their unsplit combined
work in `nodes_searched` and leave `qnodes_searched` at zero; `PlayerHuman` leaves both counters at
their defaults.

---

### SearchLimits

**Location**: `SearchLimits.h`

Every per-call constraint, all optional: clock (with increment and moves-to-go), movetime, depth,
infinite. `Engine::resolve_limits()` resolves it and composed `SearchControl` arms the timer and
owns the abort/node-limit state for both concrete and legacy search. `Engine::compute_budget(remaining,
increment, moves_to_go)` → `TimeBudget{soft, hard}` is a pure function and is unit-tested as one.

---

### SearchTuning

**Location**: `AIPerplex.h`

```cpp
struct SearchTuning {
    int64_t min_nodes_threshold = 1000;
    double min_completion_ratio = 0.10;
    double min_pv_ratio = 0.33;
    int score_draw_threshold = 20;
};
```

**Purpose**: Runtime-tunable search parameters without recompilation.

**Usage**:
```cpp
SearchTuning tuning{};
tuning.min_nodes_threshold = 500; // More lenient
AIPerplex ai(AIPerplexConfig{.tuning = tuning});
```

---

### IterationMetrics

**Location**: `AIPerplex.h` (private)

```cpp
struct IterationMetrics {
    int depth;
    Move current_move;
    int current_score;
    int64_t nodes_searched;
    int pv_length;
    bool interrupted;
    bool move_changed;
    int score_delta;
    double completion_ratio;
};
```

**Purpose**: Captures all data about a single iteration for quality assessment.

---

### Move (16-bit encoding)

**Location**: `Move.h`

**Encoding**:
```
Bits 0-5:   From square (0-63)
Bits 6-11:  To square (0-63)
Bits 12-15: Move type (quiet, capture, castle, promotion, etc.)
```

`static_assert(sizeof(Move) == 2)` pins the size.

**Contracts worth knowing**:
- The moving and captured pieces are **not** stored. Use `Board::GetEffectiveMovPiece(m)` (pre-move
  only) and `Board::GetCapturedPiece(m)`; after `DoMove`, identify the moved piece with
  `board.GetPiece(m.to())`.
- Equality compares from/to only and **ignores flags** — two moves differing only in promotion piece
  compare equal.
- `is_null()` tests for the empty move.
- Formatting lives entirely in `MoveFormatter` (`ToCoord`, `ToShort`, `ToUCI`, `ToVerbose`,
  `FromUCI`), not on `Move` itself.

---

### PVTable

**Location**: `PVTable.h`

**Structure**: Triangular array — each ply stores the PV from that point to the leaf.

```cpp
class PVTable {
    std::array<std::array<Move, MAX_PV_LENGTH>, MAX_PLY> pv_;
    std::array<int, MAX_PLY> pv_lengths_;

public:
    void update(int ply, Move move) noexcept;   // Copy PV from child + prepend
    Move get_pv_move(int ply) const noexcept;   // Best move at ply
    int  get_length(int ply) const noexcept;    // PV length at ply
};
```

---

### Bitboards

**Location**: `Board.h`, `defines.h`

**Representation**: 64-bit integers, one bit per square

```cpp
using TBitboards = std::array<BITBOARD, ALL_BITBOARDS>;   // ALL_BITBOARDS == 15
// [0-11]: Individual pieces (white pawn, black pawn, ...)
// [12-13]: All white, all black
// [14]:    All pieces (ALL_PIECES)
```

**Benefits**: 
- Parallel operations (find all pawns in one operation)
- Fast attack generation using PEXT magic bitboards (`Magic.h`)
- Efficient move generation

Sliding-piece attacks use PEXT magic bitboards. The older rotated-bitboard scheme
(`ROTATED90`/`45R`/`45L`) has been removed.

---

## Dependencies

### External Libraries

Fetched and pinned by CMake `FetchContent` into `build/_deps` and shared by every preset — there is
nothing to install.

| Library | Purpose | License |
|---------|---------|---------|
| **spdlog** | Async logging infrastructure | MIT |
| **nlohmann/json** | `game_settings.json`, test corpora | MIT |
| **Catch2 v3** | Unit test framework (amalgamated) | BSL-1.0 |
| **C++ Standard Library** | C++20 | - |

Approved external dependencies are spdlog and nlohmann/json only; Catch2 is test-only.

### C++20 Features Used

- **Designated initializers**: `SearchResult{.best_move = move, ...}`
- **Three-way comparison** (`<=>`)
- **`std::jthread`** (Lazy SMP helpers)
- **Concepts** (limited use)
- **Ranges** (limited use)
- **constexpr** enhancements
- **std::format** compatibility

### Compiler Requirements

- **clang-cl** — the shipping compiler
- **MSVC** 2022 — development and debugging only (never for measurement)
- **GCC** 12+ — Linux CI, Debug + sanitizers

Only `x64` builds are maintained. Warnings are errors everywhere (`/W4 /WX`, `-Wall -Wextra -Werror`)
in both Debug and Release.

### Build Configuration

- Release enables interprocedural optimisation (`INTERPROCEDURAL_OPTIMIZATION_RELEASE`)
- `-mavx2 -mbmi2` — `Magic.h`'s `_pext_u64` requires BMI2
- Sanitizers are Linux-only via `-DSTRAT_SANITIZE=...`, and refused on MSVC/clang-cl rather than
  silently dropped

---

## Current Status

### Production Algorithm: AIPerplex

**Strengths**:
- ✅ Robust timeout handling (no bad moves on interrupt)
- ✅ Sophisticated quality assessment
- ✅ Clean architecture (`ThreadData` + `SearchResult` + `SearchLimits`)
- ✅ Comprehensive diagnostics
- ✅ Tunable parameters
- ✅ Lazy SMP parallel search
- ✅ Null-move pruning, LMR, killers, history, aspiration windows

**Known Limitations**:
- ⚠️ No delta pruning or SEE-based pruning in quiescence
- ⚠️ Move sorting done inline (should be in a `MoveSorter` class)
- ⚠️ No counter-move history
- ⚠️ No singular extensions
- ⚠️ No tablebase support

### Other Algorithms

| Algorithm | Status | Purpose |
|-----------|--------|---------|
| **AIAgent** | ✅ Active | Baseline for testing (aspiration windows) |
| **ABIterative** | 🎭 Legacy | Historical reference (simple ID) |
| **AIBasic** | 🎭 Legacy | Historical reference (basic alpha-beta) |
| **ABIterTrans** | ❌ Archived | Old TT bug; unavailable from `CreatePlayer()` |
| **AITrans** | ❌ Archived | Old TT bug; unavailable from `CreatePlayer()` |

---

## Performance Characteristics

Figures below are from the Lazy SMP merge measurements on the developer machine (clang-cl Release).
They are hardware-specific — re-measure with `Run-Bench.ps1` rather than quoting them.

### Search Speed
- **Nodes per second**: ~1.2 M single-threaded, ~6.9 M at 8 threads (5.79x)
- **Typical search depth**: depends on time control and position
- **Quiescence depth**: 10-15 plies average

### Memory Usage
- **Transposition Table**: 256 MB requested (see the rounding note above)
- **Stack usage**: ~50-100 KB per search thread (recursive search)

### Scaling
- **Time doubling**: +1-2 ply depth
- **Effective branching factor**: ~3-4 (with ordering)

### Measuring changes

Compare **nps**, never node counts at fixed depth — the node count is a property of the search, not
of the machine code. Two builds of identical source must visit identical nodes and return identical
best moves at `Threads=1`; if they do not, they are not searching the same tree and any nps
comparison is meaningless. Take repeat runs before quoting a delta.

Use `Run-Bench.ps1` for speed and `Run-EloMatch.ps1` for strength — they answer different questions.

---

## Testing & Diagnostics

### Logging Levels

**DEBUG** (file only):
- Iteration evaluation diagnostics
- Rejection reasons
- Detailed metrics

**INFO** (console + file):
- Depth completed
- Mate found
- Search complete summary
- GetMove summary

**CRITICAL** (console + file):
- Emergency move generation
- Pseudolegal move failures

### Log Files
- `aiperplex.log` - Detailed search diagnostics
- Console - Important events only

### Enabling Verbose Logging
```cpp
AIPerplex::SetVerboseLogging(true);
```

Verbose logging is opt-in per call site; the constructor does not enable it.

### Validation

`Docs/TestDesign.md` is the coverage map. The scripts in `StratChessEvolved/Scripts/` cover unit
tests, pre-commit and pre-PR validation, perft against a 142,953-position corpus, tactical stability,
benchmarking and Elo measurement. `Docs/Workflow.md` describes which validation tier applies to which
kind of change.

---

## Future Enhancements

The live backlog is GitHub Issues; `Docs/Roadmap.md` carries the larger themes.

**Search**:
- Delta pruning and SEE-based pruning in quiescence
- Counter-move history
- Singular extensions
- Extract move ordering to a `MoveSorter` class

**Evaluation**:
- The evaluation-improvement epic and its sub-issues

**Longer term**:
- Syzygy tablebase support
- Neural network evaluation

---

## References

### Code Documentation
- See inline comments in source files
- Header files document public interfaces
- Helper method documentation in implementation files

### Chess Programming Concepts
- [Chess Programming Wiki](https://www.chessprogramming.org/)
- [Principal Variation Search](https://www.chessprogramming.org/Principal_Variation_Search)
- [Iterative Deepening](https://www.chessprogramming.org/Iterative_Deepening)
- [Transposition Tables](https://www.chessprogramming.org/Transposition_Table)

### Related Documentation
- `CLAUDE.md` - Build, scripts, conventions and key source facts
- `Docs/Workflow.md` - Standing decisions, validation tiers, review gate
- `Docs/CI.md` - What each CI workflow runs
- `Docs/TestDesign.md` - Coverage map and guide to writing tests
- `Docs/Changelog.md` - Version history
- `Docs/Roadmap.md` - Future development themes
- `Docs/EloMeasurement.md` / `Docs/EloLog.md` - Strength measurement method and results

---

## Contributing

### Code Style
- C++20 modern style
- 4-space indentation (tabs in legacy code)
- Descriptive variable names
- Comments describe the code as it stands — not what it replaced, and not point-in-time measurements

### Before Submitting
1. Run `Validate-PreCommit.ps1` (the pre-commit hook does this)
2. Run `Validate-PrePR.ps1` before opening a PR — it scopes itself to the change tier
3. Dispatch a specialised reviewer if the diff touches evaluation or search
4. Update this document if the architecture changes

### Contact
- Original Author: Thees (20 years development)
- Current Maintainer: Thees

---

**Last Updated**: August 10, 2026  
**Document Version**: 3.0
