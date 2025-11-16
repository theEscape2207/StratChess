# Position Class Documentation
## Core Chess Domain Abstraction

### Overview

The **Position class** is the central data structure in a modern C++20 chess engine, encapsulating the complete internal representation of a chess position at any point in the game tree. It serves as the focal point through which all major engine components—search, move generation, evaluation, and game state tracking—operate and interact.

### Purpose and Responsibilities

The Position class fulfills three critical roles:

**1. Board State Repository**
- Maintains bitboard representations for all 12 piece types (6 piece types × 2 colors)
- Tracks immutable game properties: castling rights, en passant eligibility, halfmove clock (for the fifty-move rule), fullmove number
- Computes and caches derived information: Zobrist hash keys for transposition table lookups and repetition detection, board occupancy bitmaps

**2. Move Orchestration Interface**
- Provides atomic make/unmake operations for tree traversal without rebuilding state from scratch
- Handles incremental updates to bitboards, hash keys, and auxiliary state during move application
- Preserves irreversible state (castling rights, en passant, halfmove clock) through internal stacks indexed by search ply
- Supports legal move validation by checking king safety and pinned piece constraints

**3. Query and Analysis Provider**
- Exposes efficient queries for move generation and validation: generate pseudo-legal or legal moves, check if a square is attacked, verify if the moving side is in check
- Computes position-specific metrics for evaluation: material balance, piece placement scores, pawn structure, king safety indicators
- Detects game termination conditions: checkmate, stalemate, draw by threefold repetition or fifty-move rule, insufficient material
- Provides debug and serialization interfaces: FEN output, position printing for analysis

---

## Lifecycle and State Management

### Initialization

A Position is initialized from one of two sources:

**From FEN (Forsyth-Edwards Notation)**
```cpp
Position pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
```

The FEN string encodes all necessary information to reconstruct the exact position. During construction:
- Piece placement is parsed into 12 bitboards (one per piece type)
- Side to move is recorded
- Castling rights are decoded (both sides may have queenside/kingside rights)
- En passant target square (if any) is parsed
- Halfmove clock (for fifty-move rule) is extracted
- Fullmove number is recorded

The Zobrist hash is computed from scratch by combining random 64-bit keys for each piece placement, castling rights configuration, en passant square, and side to move.

**From Default (Starting Position)**
```cpp
Position pos; // Initializes to standard start position
```

This constructs the initial position with all pieces in their opening array, white to move, all castling rights available, no en passant target, halfmove clock at 0.

### Ply-Based State Management

During search, the engine operates in a **ply-indexed** model where ply represents the number of half-moves from the root position (ply 0). The Position class maintains parallel arrays indexed by ply to enable cheap restoration of irreversible state when unmaking moves:

- **Castling rights array**: Stores castling state before each move
- **En passant array**: Records the en passant target square (if any) before each move
- **Halfmove clock array**: Tracks the fifty-move rule counter before each move
- **Captured piece array**: Remembers what piece (if any) was captured by each move
- **Last irreversible move ply**: Marks the most recent position where an irreversible action occurred (pawn move or capture)

This design avoids expensive state reconstruction at the cost of modest linear memory in proportion to search depth.

### Reversible vs. Irreversible Operations

**Reversible updates** are modified incrementally during make_move and undone during unmake_move:
- Bitboards for piece positions (updated by bitwise operations: XOR piece out of source, XOR into destination)
- Zobrist hash key (XOR out old pieces and state, XOR in new pieces and state)
- Side to move (simple toggle)

**Irreversible changes** cannot be recovered by inverting the move alone; instead, the prior state is restored from a stack:
- Castling rights (may be lost permanently if a king or rook moves, even temporarily)
- En passant square (dependent on the exact move made, not just the piece positions)
- Halfmove clock reset (certain moves reset this counter to 0)
- Captured pieces (information is not stored in the new position)

Example: If the king moves to escape check, castling rights are forever lost for that side, even if the king later returns to its original square.

---

## Key Member Variables

### Bitboard Storage (Board Representation)

```cpp
// Piece-centric: one bitboard per (piece_type, color) pair
uint64_t bitboards[PieceType::COUNT][Color::COUNT];

// Composite views for faster operations
uint64_t occupancy[Color::COUNT];      // All pieces for white/black
uint64_t all_pieces;                   // All pieces (union of white and black occupancy)
```

The 12 individual bitboards enable rapid move generation via bit manipulation (e.g., shifting knight bitboards by knight offsets to find attack squares). The composite occupancy bitboards avoid repeated OR operations during move legality checks.

### Game State

```cpp
Color side_to_move;              // WHITE or BLACK
uint8_t castling_rights;         // Bitmask: bits for KQkq castling availability
Square en_passant_square;        // Valid square (0-63) or NO_SQUARE if not available
uint8_t halfmove_clock;          // Fifty-move rule counter (0 to 100+)
uint16_t fullmove_number;        // Incremented after black's move
```

These fields represent the complete game state needed to determine what moves are legal.

### Hash and Repetition Tracking

```cpp
uint64_t zobrist_hash;                 // Incremental hash for TT lookups
std::vector<uint64_t> position_history_;  // Hash history for repetition detection
size_t last_irreversible_ply_;         // Ply index of last pawn move or capture
```

The Zobrist hash is updated incrementally during make/unmake operations by XORing random bit patterns corresponding to piece placements and immutable state. This enables O(1) hash computation rather than O(32) or O(64) from scratch.

The position history enables detection of threefold repetition within the search tree and from game history (see Threefold Repetition documentation).

### State Preservation Arrays (Per-Ply Stacks)

```cpp
std::vector<uint8_t> castling_history;       // Prior castling rights before each move
std::vector<Square> en_passant_history;      // Prior en passant square before each move
std::vector<uint8_t> halfmove_clock_history; // Prior halfmove clock before each move
std::vector<Piece> captured_piece_history;   // Piece captured by each move (if any)
```

These are indexed by ply: `castling_history[ply]` stores the castling rights before ply was applied. During unmake_move, the prior state is restored in O(1) time.

### Incremental State Indicators

```cpp
size_t current_ply;                    // Current depth in the search tree or game
Piece last_moved_piece;                // Type and color of piece that moved last
Square last_move_source;               // Origin square of last move
Square last_move_dest;                 // Destination square of last move
```

These support specialized logic in search (e.g., killer moves, move ordering) and evaluation (e.g., detecting pins or discovered checks).

---

## Core Member Functions

### Initialization and Setup

**`void reset_from_fen(const std::string& fen)`**
- Parses a FEN string and reconstructs the complete board state
- Clears all bitboards, resets game state, recomputes Zobrist hash
- Used at engine startup or when loading a specific position for analysis

**`void reset_to_startpos()`**
- Initializes the standard starting position (RNBQKBNR / ... / PPPPPPPP)
- Equivalent to parsing the FEN: "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

**`void clear_history()`**
- Resets position history and irreversible state stacks
- Called when starting a new search from the root
- Critical for multithreaded search where each thread has its own Position copy

### Move Application (The Core Search Loop)

**`void make_move(const Move& move)`**
- **Pre-condition**: The move is assumed to be pseudo-legal (respects piece movement rules)
- **Side effects**: Modifies bitboards, Zobrist hash, game state; appends prior state to history stacks; increments `current_ply`
- **Time complexity**: O(1) amortized (bitwise operations, vector append to pre-allocated stack)

**Implementation outline:**
1. Record prior castling rights, en passant, and halfmove clock to their history vectors at index `current_ply`
2. Update bitboards: XOR the moving piece out of source, XOR into destination; XOR captured piece (if any) out
3. Update Zobrist hash: XOR out old piece positions and state, XOR in new positions and state
4. Handle move-specific side effects:
   - If pawn move or capture: reset halfmove clock to 0; record `last_irreversible_ply = current_ply`
   - If castling: move the rook; remove castling rights for that side
   - If pawn promotion: replace pawn with promoted piece; update hash
   - If en passant capture: remove the captured pawn from its square (different from the destination)
   - If king move: lose castling rights; may trigger en passant square clearing
5. Update side to move (toggle WHITE ↔ BLACK)
6. Update en passant square if a pawn advances two squares; otherwise clear it
7. Increment `current_ply`
8. Push Zobrist hash to position history for repetition detection

**`void unmake_move()`**
- **Pre-condition**: At least one move has been made since initialization
- **Side effects**: Reverses the effects of the most recent make_move; decrements `current_ply`
- **Time complexity**: O(1) amortized (vector pop, restore prior state)

**Implementation outline:**
1. Decrement `current_ply`
2. Restore prior state from history stacks at index `current_ply`:
   - Restore castling rights, en passant square, halfmove clock
3. Reverse bitboard and Zobrist hash updates by re-reading move metadata and prior state
4. Toggle side to move
5. Pop position history (for repetition detection)

**Critical design note**: make_move is **not** responsible for verifying move legality (that the moving side's king is not left in check). Instead, pseudo-legal moves are generated and checked after being made. This design optimizes move ordering: moves that leave the king in check are immediately pruned without exploring their child moves.

### Move Legality Verification

**`bool is_legal(const Move& move) const`**
- Checks if a pseudo-legal move is actually legal
- Returns true if after making the move, the moving side's king is not in check
- Used during move generation filtering or after make_move to prune illegal branches

**`bool is_in_check() const`**
- Determines if the side to move's king is currently under attack
- Scans the 8 directions around the king (rook/queen attacks + bishop/queen attacks) and knight offsets
- Used to distinguish checkmate from stalemate and to guide move generation in check positions

### Legal Move Generation

**`MoveList generate_legal_moves() const`**
- Generates a list of all legal moves from the current position
- Calls generate_pseudo_legal_moves, then filters by legality: only includes moves where the king is not in check after the move is made
- **Performance note**: Generating all legal moves is expensive (requires checking king safety for each candidate). Search algorithms typically minimize calls to this by:
  - Caching move lists across multiple searches (move ordering tables)
  - Using incremental move generation with iterative deepening
  - Relying on transposition table hits to avoid regenerating moves

**`MoveList generate_pseudo_legal_moves() const`**
- Generates moves respecting piece movement rules but ignoring check
- Includes:
  - Pawn moves: one or two squares forward, captures diagonally, promotion on 7th/2nd rank, en passant
  - Knight moves: all 8 knight offsets relative to knight positions
  - Bishop moves: all four diagonal directions until blocked or edge
  - Rook moves: all four orthogonal directions until blocked or edge
  - Queen moves: combination of bishop and rook moves
  - King moves: 8 adjacent squares (excluding occupied by own pieces); castling if rights available and intermediate squares unoccupied and unattacked

### Evaluation and Analysis

**`int evaluate() const`**
- Computes a static evaluation score (in centipawns) for the current position
- Factors typically include:
  - Material: piece count weighted by piece value (pawn=100, knight≈300, bishop≈300, rook≈500, queen≈900)
  - Piece placement: bonuses/penalties based on square (piece-square tables)
  - Pawn structure: passed pawns, doubled pawns, isolated pawns, pawn chains
  - King safety: pawn shield, king activity in opening vs. endgame
  - Piece activity: mobility, centralization, control of important squares
- **Note**: Evaluation should only be called on "quiet" positions (no immediate tactics) to avoid horizon effects. In practice, quiescence search extends the search at leaf nodes to capture forcing moves (checks and captures).

**`bool is_checkmate() const`**
- Returns true if the side to move is in check and has no legal moves
- Implies the other side has won

**`bool is_stalemate() const`**
- Returns true if the side to move is not in check but has no legal moves
- Results in a draw by stalemate rules

**`bool is_draw_by_repetition(int ply) const`**
- Detects threefold repetition (or allows twofold in search tree)
- Scans position history backwards from the last irreversible move
- Returns true if the current position appears at least twice before in the game (threefold) or twice in the search tree above the root (twofold within search)

**`bool is_draw_by_fifty_move_rule() const`**
- Returns true if halfmove clock ≥ 100 (50 moves by each side without pawn move or capture)

**`bool is_insufficient_material() const`**
- Checks if the remaining material cannot possibly lead to checkmate (e.g., king vs. king, king+bishop vs. king)
- Returns true if the position is a theoretical draw

### Hash and State Queries

**`uint64_t get_zobrist_hash() const`**
- Returns the current Zobrist hash key
- Used as the primary index into the transposition table
- Updated incrementally during make/unmake for O(1) cost

**`Piece piece_on(Square sq) const`**
- Returns the piece (type and color) on a given square, or EMPTY
- Used by move generation and evaluation

**`bool is_square_attacked(Square sq, Color by_color) const`**
- Determines if a square is under attack by pieces of a given color
- Used for king safety checks and move legality validation

**`uint64_t get_occupied_squares(Color c) const`**
- Returns the bitboard of all squares occupied by pieces of a given color

---

## Integration Scenarios

### Scenario 1: Search Tree Traversal

In negamax/alpha-beta search, the Position object is passed by reference and modified in place:

```cpp
int negamax(Position& pos, int depth, int alpha, int beta) {
    if (depth == 0) {
        return pos.evaluate();
    }
    
    // Generate candidate moves
    MoveList moves = pos.generate_legal_moves();
    
    int max_score = -INFINITY;
    for (const Move& move : moves) {
        // Apply move to Position (in-place modification)
        pos.make_move(move);
        
        // Recursively search the resulting position
        int score = -negamax(pos, depth - 1, -beta, -alpha);
        
        // Restore Position to pre-move state
        pos.unmake_move();
        
        // Alpha-beta pruning
        max_score = std::max(max_score, score);
        alpha = std::max(alpha, max_score);
        if (alpha >= beta) break; // Cutoff
    }
    
    return max_score;
}
```

In this loop, each make_move/unmake_move pair takes O(1) time, enabling efficient tree exploration. The Position bitboards and hash are maintained in a consistent state at each recursion level, and repetition history is tracked via the zobrist_hash stack.

### Scenario 2: Transposition Table Integration

The Zobrist hash from Position drives transposition table lookups and storage:

```cpp
TranspositionTableEntry* entry = tt.probe(pos.get_zobrist_hash());
if (entry != nullptr && entry->depth >= desired_depth) {
    // Found a relevant prior evaluation; return it
    return entry->score;
}

// Position not in TT; perform full search (as above)
int score = negamax(pos, depth, alpha, beta);

// Store the result for future lookups
tt.store(pos.get_zobrist_hash(), depth, score, tt_flag);
return score;
```

The incremental hash update during make/unmake ensures that the hash key correctly identifies the position at each search node, enabling TT efficiency.

### Scenario 3: Move Ordering with Position Queries

Move ordering heuristics often query Position state to prioritize moves:

```cpp
void score_moves(Position& pos, MoveList& moves) {
    // Transposition table move (hash move) scores highest
    Move tt_move = ...;  // From TT lookup
    
    // Then captures, sorted by MVV-LVA (Most Valuable Victim, Least Valuable Attacker)
    // Then killer moves
    // Then history heuristic moves
    
    for (Move& move : moves) {
        if (move == tt_move) {
            move.score = 100000;  // Highest priority
        } else if (pos.is_capture(move)) {
            Piece victim = pos.piece_on(move.destination);
            move.score = 10000 + (piece_value[victim.type] - piece_value[move.moved_piece]);
        } else if (is_killer_move(move, depth)) {
            move.score = 5000;
        } else {
            move.score = history_heuristic[move];
        }
    }
    
    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
        return a.score > b.score;
    });
}
```

Position queries (is_capture, piece_on) directly inform move ordering, ensuring strong moves are searched first.

### Scenario 4: Evaluation with Position Context

The evaluator queries Position state to compute positional bonuses:

```cpp
int evaluate(const Position& pos) {
    int score = 0;
    
    // Material
    score += count_material(pos);
    
    // Piece-square table bonuses for all pieces
    for (Square sq = 0; sq < 64; ++sq) {
        Piece p = pos.piece_on(sq);
        if (p != EMPTY) {
            score += piece_square_table[p.type][p.color][sq];
        }
    }
    
    // King safety (more important in midgame)
    if (!pos.is_endgame()) {
        score -= evaluate_king_safety(pos, WHITE);
        score += evaluate_king_safety(pos, BLACK);
    }
    
    // Pawn structure
    score += evaluate_pawn_structure(pos);
    
    return score;
}
```

The Position provides efficient queries (piece_on, is_in_check, bitboards) that the evaluator leverages.

### Scenario 5: Game History and Repetition Detection

During play, the game history accumulates:

```cpp
class Game {
    std::vector<Position> positions_;
    std::vector<Move> moves_;
    
public:
    void play_move(const Move& move) {
        Position new_pos = positions_.back();
        new_pos.make_move(move);
        positions_.push_back(new_pos);
        moves_.push_back(move);
        
        // Update game statistics
        if (new_pos.is_draw_by_repetition()) {
            game_result = DRAW_BY_REPETITION;
        }
    }
};
```

During engine search from a given position, the Position class is initialized with a prefix of the game history via clear_history(), allowing repetition detection to correctly distinguish game history from search-tree repetitions.

---

## Relationship to Other Domain Classes

### Move Class

```cpp
struct Move {
    Square source;
    Square destination;
    Piece promoted_to;  // If promotion, else NONE
    // or bit-packed:
    // uint16_t data;  // bits 0-5: source, 6-11: destination, 12-15: flags
};
```

The Move encodes the source and destination squares and any promotion. Position::make_move takes a Move and updates internal state accordingly. Position does not store moves; instead, the search algorithm iterates over move lists and applies them.

### MoveGenerator Class

```cpp
class MoveGenerator {
public:
    MoveList generate_legal_moves(const Position& pos) const;
    MoveList generate_pseudo_legal_moves(const Position& pos) const;
    // ...
};
```

MoveGenerator uses queries from Position (piece_on, is_square_attacked, castling_rights, etc.) to construct move lists. Position supplies the bitboards; MoveGenerator implements the logic to convert bitboards into move lists.

### Evaluator Class

```cpp
class Evaluator {
public:
    int evaluate(const Position& pos) const;
    int evaluate_material(const Position& pos) const;
    int evaluate_piece_placement(const Position& pos) const;
    int evaluate_pawn_structure(const Position& pos) const;
    int evaluate_king_safety(const Position& pos) const;
    // ...
};
```

Evaluator treats Position as read-only and queries piece locations, castling state, halfmove clock, etc. to compute evaluation scores. Position does not know about Evaluator; this is a one-way dependency.

### Search Class

```cpp
class Search {
    Position root_position_;
    TranspositionTable& tt_;
    
public:
    int find_best_move(const Position& root, int depth);
    int negamax(Position& pos, int depth, int alpha, int beta);
    // ...
};
```

Search holds a Position reference (typically copied per thread in multithreaded search) and repeatedly calls make_move/unmake_move. Search is the primary consumer of Position's make/unmake interface.

### TranspositionTable Class

```cpp
class TranspositionTable {
    struct Entry {
        uint64_t hash_key;
        int depth;
        int score;
        int flag;  // Exact, Lower bound, Upper bound
        Move best_move;
    };
    
    std::vector<Entry> table_;
    
public:
    Entry* probe(uint64_t hash) const;
    void store(uint64_t hash, int depth, int score, int flag, const Move& move);
};
```

TranspositionTable uses Position::get_zobrist_hash() as the primary key. Position provides incremental hash updates; the TT is oblivious to Position internals.

### Bitboard Utility Functions

```cpp
namespace bitboard {
    uint64_t get_knight_attacks(Square from);
    uint64_t get_bishop_attacks(Square from, uint64_t occupancy);
    uint64_t get_rook_attacks(Square from, uint64_t occupancy);
    // ... etc
}
```

Position internally uses these bitboard utilities during move generation and legality checking. They abstract the bitwise operations so Position and MoveGenerator focus on high-level logic.

---

## Thread Safety and Multithreading

In multithreaded search (parallel alpha-beta with thread pools), each search thread has its own Position instance. This design ensures thread safety without locks:

```cpp
class SearchThread {
    Position position_;  // Per-thread copy; not shared
    TranspositionTable& shared_tt_;  // Read/Write with appropriate locking
    
    void search_task(const Position& root, int depth) {
        position_ = root;  // Copy root position
        int score = negamax(position_, depth, -INFINITY, INFINITY);
        // Transposition table updates are serialized via mutex
    }
};
```

Position make_move and unmake_move are not thread-safe in the sense that multiple threads cannot safely call them on the same Position instance simultaneously. However, each thread's independent Position instance is fully thread-safe by design. The shared TranspositionTable is protected by external synchronization (mutex or atomic operations).

---

## Performance Characteristics

| Operation | Time | Space | Notes |
|-----------|------|-------|-------|
| make_move | O(1) | O(1) amortized | Bitwise ops; vector append to pre-allocated arrays |
| unmake_move | O(1) | O(1) amortized | Restore from history stacks; vector pop |
| get_zobrist_hash | O(1) | O(0) | Simple field access |
| is_in_check | O(1) | O(0) | Scans fixed number of directions/knight offsets around king |
| generate_pseudo_legal_moves | O(1) | O(moves) | Bitwise ops to compute attack bitboards; convert bitboards to move list |
| evaluate | O(64) | O(0) | Iterates board squares; lookups into piece-square tables |
| is_legal | O(1) | O(0) | Check if king is attacked after move (similar to is_in_check) |

---

## Design Rationale and Developer Guidelines Alignment

The Position class aligns with the Developer Guidelines:

**Performance**: Zobrist hashing and bitboard representation enable O(1) hash and position state updates. Incremental make/unmake supports rapid tree traversal. Bitwise operations leverage CPU efficiency.

**Maintainability**: The Position class provides a clean, declarative interface hiding implementation details. Queries (piece_on, is_in_check) are self-documenting. State preservation via per-ply history arrays is explicit and understandable.

**Quality Assurance**: Deterministic behavior is guaranteed by incremental state updates and explicit history stacks. Position is easily serializable to FEN for regression testing and debugging. Legal move verification is composable and testable.

**Reuse and Minimal Dependencies**: Position uses only standard C++ (no external libs) and leverages bitboard operations, which are domain-standard in chess engines.

**Thread Safety**: Per-thread Position instances ensure safe multithreading without locks on Position itself, aligning with scalability requirements.

---

## Summary

The Position class is the heart of the chess engine, providing:
- **Efficient board representation** via bitboards and incremental Zobrist hashing
- **Low-overhead state management** via per-ply history stacks and O(1) make/unmake
- **Rich query interface** for move generation, legality checking, and evaluation
- **Game state tracking** for rules enforcement (castling, en passant, fifty-move rule, repetition)
- **Seamless integration** with search, evaluation, move ordering, and transposition tables

Its design prioritizes performance, clarity, and correctness, making it the natural focal point for all chess logic in the engine.