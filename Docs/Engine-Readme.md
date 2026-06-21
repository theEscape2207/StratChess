# StratChess Engine Documentation

**Version**: 2.0 (February 2026)  
**Language**: C++20  
**Primary Algorithm**: AIPerplex (PVS + Transposition Tables + Iterative Deepening)

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
- **Quiescence Search** for tactical stability
- **Bitboard representation** for efficient move generation
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
```cpp
// Prerequisites: C++20 compiler, spdlog library

// Create an AI instance
AIPerplex ai(15);  // Max depth 15
ai.SetVerboseLogging(true);

// Get a move
GameInfo info = board.GetGameInfo();
Move bestMove = ai.GetMove(info);
```

### Configuration
```cpp
// Tune search parameters
ai.tuning().min_nodes_threshold = 1000;      // Minimum nodes for valid search
ai.tuning().min_completion_ratio = 0.10;     // 10% of previous depth required
ai.tuning().min_pv_ratio = 0.33;             // PV must be 1/3 of depth
ai.tuning().score_draw_threshold = 20;       // Suspicious score=0 detection
```

---

## Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────┐
│                         Game Loop                           │
│                    (Game.cpp/h)                             │
└───────────────┬─────────────────────────────────────────────┘
                │
                ▼
┌───────────────────────────────────────────────────────────┐
│                    Player Interface                       │
│                     (IPlayer.h)                           │
└───────┬───────────────────────────────────────────────────┘
        │
        ├─── PlayerHuman ──────────────────────────────────┐
        │                                                  │
        └─── PlayerAI (Base Class)                         │
                  │                                        │
                  ├─── AIPerplex ★ (Production)            │
                  ├─── AIAgent (Baseline)                  │
                  ├─── ABIterative (Legacy)                │
                  ├─── AIBasic (Legacy)                    │
                  └─── [Archived: ABIterTrans, AITrans]    │
                                                           │
┌──────────────────────────────────────────────────────────┴──┐
│                   Search Components                         │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Iterative Deepening                                   │ │
│  │  ├─ Depth 1, 2, 3... up to max_depth                   │ │
│  │  └─ Timeout handling with quality assessment           │ │
│  └─────────────┬──────────────────────────────────────────┘ │
│                ▼                                            │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Principal Variation Search (PVS)                      │ │
│  │  ├─ Alpha-Beta pruning                                 │ │
│  │  ├─ Null-window search for non-PV nodes                │ │
│  │  ├─ Re-search on fail-high                             │ │
│  │  └─ Transposition table probe/store                    │ │
│  └─────────────┬──────────────────────────────────────────┘ │
│                ▼                                            │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Quiescence Search                                     │ │
│  │  ├─ Stand-pat evaluation                               │ │
│  │  ├─ Capture-only search                                │ │
│  │  ├─ MVV-LVA move ordering                              │ │
│  │  └─ Depth limit (15 plies)                             │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                  │
                  ▼
┌──────────────────────────────────────────────────────────────┐
│                   Support Infrastructure                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │ Board        │  │ Move         │  │ Evaluator    │        │
│  │ Bitboards    │  │ Generator    │  │ Material +   │        │
│  │ Zobrist Hash │  │ Legal Moves  │  │ Position     │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │ Transposition│  │ PV Table     │  │ Move         │        │
│  │ Table        │  │ Principal    │  │ Ordering     │        │
│  │ 256MB cache  │  │ Variation    │  │ MVV-LVA      │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
└──────────────────────────────────────────────────────────────┘
```

### Data Flow

```
GetMove() Entry
    ↓
Initialize Search State
    ↓
Iterative Deepening Loop (depth 1 → max_depth)
    │
    ├─→ Execute PVS at current depth
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
Return SearchResult
    ↓
Move extracted and played
```

---

## File Structure

### Core Search Algorithms

| File | Description | Status |
|------|-------------|--------|
| `AIPerplex.cpp/h` | **Production algorithm**: PVS + TT + ID + Quiescence | ✅ Active |
| `AIAgent.cpp/h` | Aspiration windows baseline | ✅ Active |
| `ABIterative.cpp/h` | Simple iterative deepening | 🎭 Legacy (nostalgic) |
| `AIBasic.cpp/h` | Basic alpha-beta | 🎭 Legacy (nostalgic) |
| `ABIterTrans.cpp/h` | Broken TT implementation | ❌ Archived |
| `AITrans.cpp/h` | Broken TT implementation | ❌ Archived |

### Game Infrastructure

| File | Description |
|------|-------------|
| `Board.cpp/h` | Bitboard representation, move execution, Zobrist hashing |
| `Move.cpp/h` | Move encoding (16-bit), move value calculation |
| `MoveGenerator.cpp/h` | Legal move generation, capture generation |
| `Game.cpp/h` | Game loop, player management, move validation |
| `GameState.h` | Game state enum (playing, checkmate, stalemate, draw) |

### Evaluation & Ordering

| File | Description |
|------|-------------|
| `Eval.cpp/h` | Position evaluation (material + positional) |
| `Sort.cpp/h` | Move ordering utilities (MVV-LVA) |

### Data Structures

| File | Description |
|------|-------------|
| `TranspositionTable.h/cpp` | Hash table for position caching (256MB default) |
| `PVTable.h` | Principal variation storage |
| `HashElement.h` | TT entry structure |

### Base Classes & Interfaces

| File | Description |
|------|-------------|
| `IPlayer.h` | Player interface (AI and Human) |
| `PlayerBase.cpp/h` | Base player implementation |
| `PlayerAI.cpp/h` | AI player base class |
| `PlayerAiIterBase.h` | Iterative AI base (time management) |
| `PlayerHuman.cpp/h` | Human player input handling |

### Utilities

| File | Description |
|------|-------------|
| `Utils/Logger.cpp/h` | Logging infrastructure (spdlog wrapper) |
| `Utils/FENParser.cpp/h` | FEN string parsing for position setup |
| `Utils/TimeManager.h` | Time control management |
| `Utils/BitTools.h` | Bitboard manipulation utilities |
| `Utils/Formatters.h` | Output formatting helpers |

### Configuration & Constants

| File | Description |
|------|-------------|
| `defines.h` | Core enums (Color, PieceType, Square, GameValues) |
| `Config.cpp/h` | Engine configuration |
| `globals.h` | Global constants |
| `typedef.h` | Type definitions |

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

**Algorithm**:
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
- **Move ordering**: PV move > Hash move > Captures > Quiet moves

**Performance**: 2-3x faster than plain alpha-beta in typical positions

---

### 2. Iterative Deepening with Quality Assessment

**Location**: `AIPerplex::iterative_deepening()`

**Description**: Searches depth 1, 2, 3... until time runs out, with sophisticated interrupted search handling.

**Algorithm**:
```
iterative_deepening(max_depth):
    state = empty_search_state()
    
    for depth = 1 to max_depth:
        nodes_at_start = node_count
        
        # Execute search at this depth
        score = pvs(depth, -INF, +INF, 0, true)
        
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

**Description**: Searches only tactical moves (captures) until position is "quiet" to avoid horizon effects.

**Algorithm**:
```
quiescence(alpha, beta, depth):
    if depth > MAX_QSEARCH_DEPTH:
        return evaluate()
    
    # Stand-pat: can we already cutoff?
    stand_pat = evaluate()
    if stand_pat >= beta:
        return beta
    
    if stand_pat > alpha:
        alpha = stand_pat
    
    # Generate and search captures only
    captures = generate_captures()
    sort_by_MVV_LVA(captures)
    
    best_score = stand_pat
    for each capture:
        score = -quiescence(-beta, -alpha, depth+1)
        
        if score >= beta:
            return beta
        if score > best_score:
            best_score = score
            alpha = max(alpha, score)
    
    return best_score
```

**Key Features**:
- **Stand-pat**: Always have option to not capture
- **Capture-only**: Dramatically reduces search tree
- **MVV-LVA ordering**: Most Valuable Victim - Least Valuable Attacker
- **Depth limit**: Prevents infinite capture sequences
- **TT caching**: Stores quiescence results separately from main search

**Future Enhancement**: Delta pruning (skip captures that can't improve alpha)

---

### 4. Transposition Table

**Location**: `TranspositionTable.h/cpp`

**Description**: Hash table caching position evaluations to avoid re-searching identical positions.

**Entry Structure**:
```cpp
struct TTEntry {
    uint64_t key;           // Zobrist hash
    int16_t value;          // Evaluation score
    int16_t depth;          // Search depth
    Move best_move;         // Best move found
    BoundType bound;        // EXACT, LOWER, or UPPER
    NodeType node_type;     // PV, CUT, or ALL
    SearchPhase phase;      // MAIN or QUIESCENCE
};
```

**Replacement Strategy**: 
- Always replace if new depth ≥ old depth
- Preferentially keep PV nodes
- Separate storage for MAIN vs QUIESCENCE searches

**Size**: 256MB default (~16M entries)

**Hit Rate**: Typically 80-90% in middle game

---

### 5. Move Ordering

**Current Implementation**: `AIPerplex::pvs()` (inline sorting)

**Order of Priority**:
1. **PV move** (from previous iteration) - 200,000 points
2. **Hash move** (from TT) - 150,000 points
3. **Captures** (by MVV-LVA) - 100x victim value
4. **Quiet moves** - deterministic tiebreaker

**Future Enhancement**: Extract to `MoveSorter` class, add killer moves and history heuristic

---

## Data Structures

### SearchResult

**Location**: `AIPerplex.h`

```cpp
struct SearchResult {
    Move best_move;           // Best move found
    int best_score;           // Evaluation score
    int depth_completed;      // Actual depth searched
    int64_t nodes_searched;   // Node count
    bool search_was_stable;   // Move unchanged in late depths
};
```

**Purpose**: Clean interface between `iterative_deepening()` and `GetMove()`, avoiding reliance on PV table state.

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
ai.tuning().min_nodes_threshold = 500;  // More lenient
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

**Key Methods**:
- `Move::Value()` - Returns capture value for ordering
- `IsEmpty()` - Check for null move
- `Output()` - Human-readable string (e.g., "e2-e4")

---

### PVTable

**Location**: `PVTable.h`

**Structure**: Array of PV lines, one per ply

```cpp
class PVTable {
    std::array<std::array<Move, MAX_PLY>, MAX_PLY> table;
    std::array<int, MAX_PLY> lengths;
    
public:
    void update(int ply, Move move);  // Copy PV from child + prepend
    Move get_pv_move(int ply);        // Get best move at ply
    int get_length(int ply);          // Get PV length
};
```

**Triangular array** - each ply stores PV from that point to leaf.

---

### Bitboards

**Location**: `Board.h`

**Representation**: 64-bit integers, one bit per square

```cpp
std::array<uint64_t, 18> bitboards;
// [0-11]: Individual pieces (white pawn, black pawn, ...)
// [12-13]: All white, all black
// [14]: All pieces
// [15-17]: Rotated boards (for attack generation)
```

**Benefits**: 
- Parallel operations (find all pawns in one operation)
- Fast attack generation using magic bitboards
- Efficient move generation

---

## Dependencies

### External Libraries

| Library | Version | Purpose | License |
|---------|---------|---------|---------|
| **spdlog** | 1.x | Async logging infrastructure | MIT |
| **C++ Standard Library** | C++20 | Core functionality | - |

### C++20 Features Used

- **Designated initializers**: `SearchResult{.best_move = move, ...}`
- **Three-way comparison** (`<=>`)
- **Concepts** (limited use)
- **Ranges** (limited use)
- **constexpr** enhancements
- **std::format** compatibility

### Compiler Requirements

- **MSVC**: 2019 or later (v142+)
- **GCC**: 10+ (with C++20 support)
- **Clang**: 12+ (with C++20 support)

### Build Configuration

**Optimization Flags** (Release):
- MSVC: `/O2 /Oi /GL /arch:AVX2`
- GCC/Clang: `-O3 -march=native -flto`

**Required Defines**:
- `_CRT_SECURE_NO_WARNINGS` (MSVC)

---

## Current Status

### Production Algorithm: AIPerplex

**Strengths**:
- ✅ Robust timeout handling (no bad moves on interrupt)
- ✅ Sophisticated quality assessment
- ✅ Clean architecture (refactored with SearchResult)
- ✅ Comprehensive diagnostics
- ✅ Tunable parameters
- ✅ Defeats previous baseline (AIAgent) 10/10

**Recent Fixes** (Feb 2026):
- ✅ Fixed iterative deepening timeout move selection bug
- ✅ Fixed score=0 acceptance on incomplete searches
- ✅ Fixed PV table/move mismatch bug
- ✅ Fixed mate detection causing infinite emergency loop
- ✅ Fixed mate found not stopping iteration
- ✅ Removed false-positive score-swing rejections

**Known Limitations**:
- ⚠️ No Late Move Reductions (LMR) - significant perf opportunity
- ⚠️ No delta pruning in quiescence
- ⚠️ Move sorting done inline (should be in MoveSorter class)
- ⚠️ No killer moves or history heuristic
- ⚠️ No aspiration windows (AIAgent has this)
- ⚠️ Single-threaded only

### Other Algorithms

| Algorithm | Status | Purpose |
|-----------|--------|---------|
| **AIAgent** | ✅ Active | Baseline for testing (aspiration windows) |
| **ABIterative** | 🎭 Legacy | Historical reference (simple ID) |
| **AIBasic** | 🎭 Legacy | Historical reference (basic alpha-beta) |
| **ABIterTrans** | ❌ Broken | Old TT bug, should archive |
| **AITrans** | ❌ Broken | Old TT bug, should archive |

---

## Performance Characteristics

### Search Speed
- **Nodes per second**: ~100k-500k (single-threaded, depth-dependent)
- **Typical search depth**: 8-12 plies (15-second time control)
- **Quiescence depth**: 10-15 plies average

### Memory Usage
- **Transposition Table**: 256 MB
- **Stack usage**: ~50-100 KB (recursive search)
- **Total**: ~300 MB

### Scaling
- **Time doubling**: +1-2 ply depth
- **Effective branching factor**: ~3-4 (with ordering)
- **TT hit rate**: 80-90% (middle game)

### Bottlenecks
1. **Move generation**: ~30% of time
2. **Position evaluation**: ~20% of time
3. **Move sorting**: ~15% of time (can optimize with thread_local)
4. **TT lookups**: ~10% of time
5. **PVS recursion**: ~25% of time

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

---

## Future Enhancements

See [Roadmap.md](Roadmap.md) for detailed plans.

**High-Priority**:
- Late Move Reductions (2-3x speedup)
- Delta pruning in quiescence
- Extract move ordering to MoveSorter class

**Medium-Priority**:
- Killer moves + history heuristic
- Aspiration windows
- SEE pruning

**Long-Term**:
- Parallel search (Lazy SMP)
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
- `Roadmap.md` - Future development plans
- `CHANGELOG.md` - Version history (TODO)
- Individual algorithm docs (TODO)

---

## Contributing

### Code Style
- C++20 modern style
- 4-space indentation (tabs in legacy code)
- Descriptive variable names
- Comments for non-obvious logic

### Before Submitting
1. Run existing games to verify no regression
2. Add diagnostic logging for new features
3. Update this README if architecture changes
4. Run static analysis (PVS-Studio)

### Contact
- Original Author: Thees (20 years development)
- Current Maintainer: Thees

---

**Last Updated**: February 11, 2026  
**Document Version**: 1.0
