# FEN Parser Refactoring - Integration Guide

## Summary of Changes

I've refactored `FENParser` to decouple it from the `Config` and `Game` classes, making it reusable directly from `Board` and `Position`.

## Files Modified

### 1. FENParser.h (Updated)
**Key changes:**
- Added `FENGameState` struct - standalone structure with no dependencies on Config
- Created NEW primary interface: `ParseFEN(FENGameState&, ...)`
- Kept DEPRECATED legacy interface: `ParseFEN(Config::GameConfig&, ...)` for backward compatibility
- Added `ValidatePositionAgainstFENMetadata()` that takes explicit `Board&` parameter
- Added conversion utilities: `ToGameConfig()` and `FromGameConfig()`

**Benefits:**
- Board can now parse FEN without depending on Config/Game
- Position class can use it directly
- Existing Config-based code still works (legacy methods)

### 2. FENParser.cpp (Updated)
**Implementation:**
- New `ParseFEN()` implementation using `FENGameState`
- Legacy `ParseFEN()` now delegates to new version + converts
- Validation now accepts explicit `Board&` instead of using singleton
- All parsing logic unchanged (still robust and well-tested)

### 3. Board.cpp (Implementation Provided)
**New implementations needed:**

**SetupBoardFromFEN():**
```cpp
void Board::SetupBoardFromFEN(const std::string& fen)
{
    // Parse FEN
    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    
    auto parseError = FENParser::ParseFEN(fen, state, pieces);
    if (parseError) {
        spdlog::default_logger()->error("FEN parse error: {}", *parseError);
        return;
    }

    // Clear and setup board
    ClearBoard();
    SetupBoard(pieces);

    // Apply game state
    sideToMove_ = state.sideToMove;
    gameInfo_.epSquare = state.epSquare;
    gameInfo_.castlingRights = state.castlingRights;
    gameInfo_.fiftyCount = state.halfMoveClock;
    gameInfo_.fullMoveCount = state.fullMoveCounter;

    // Validate and adjust inconsistent metadata
    FENParser::ValidatePositionAgainstFENMetadata(*this, state);
    
    // Re-apply potentially adjusted state
    gameInfo_.epSquare = state.epSquare;
    gameInfo_.castlingRights = state.castlingRights;
}
```

**ExtractFENFromBoard():**
- Generates FEN string from current board state
- Includes all 6 FEN fields (pieces, side, castling, ep, halfmove, fullmove)
- Useful for debugging and position export

### 4. Perft.cpp (Update Needed)
Update `run_test_suite()` to load FEN positions:
```cpp
board.SetupBoardFromFEN(pos.fen);

// Copy game state to GameInfo
info.epSquare = board.GetGameInfo().epSquare;
info.castlingRights = board.GetGameInfo().castlingRights;
// etc.
```

## Integration Steps

### Step 1: Update Board.cpp (5 minutes)

1. Open `Board.cpp`
2. Find the `SetupBoardFromFEN()` stub (line ~160)
3. Replace with implementation from `Board_FEN_Implementation.txt`
4. Find `ExtractFENFromBoard()` stub (line ~200)
5. Replace with implementation from `Board_FEN_Implementation.txt`
6. Add include at top: `#include "Utils/FENParser.h"`

### Step 2: Update Perft.cpp (2 minutes)

1. Open `Tests/Perft.cpp`
2. Find `run_test_suite()` function (line ~190)
3. Replace the FEN loading section with code from `Perft_FEN_Update.txt`

### Step 3: Test (10 minutes)

```cpp
// Quick test in main()
Board& board = Board::Instance();

// Test starting position
board.SetupBoardFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
std::cout << "FEN: " << board.ExtractFENFromBoard() << "\n";

// Test Kiwipete
board.SetupBoardFromFEN("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
std::cout << "FEN: " << board.ExtractFENFromBoard() << "\n";
```

### Step 4: Run Full Perft Suite

```bash
StratChessEvolved.exe perft
```

Expected output:
```
Testing: Starting position
  Depth 1: ✓ PASS
  Depth 2: ✓ PASS
  Depth 3: ✓ PASS
  
Testing: Kiwipete position
  Depth 1: ✓ PASS (48 nodes)
  Depth 2: ✓ PASS (2039 nodes)
  Depth 3: ✓ PASS (97862 nodes)
```

## Usage Examples

### Board Direct Usage

```cpp
Board& board = Board::Instance();

// Load any position
board.SetupBoardFromFEN("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");

// Export current position
std::string currentFEN = board.ExtractFENFromBoard();
```

### Future Position Class Usage

```cpp
// When you enable Position.cpp
Position pos;
pos.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

std::string fen = pos.ToFEN();
```

### Perft with Custom Positions

```cpp
Board& board = Board::Instance();
GameInfo info;

// Load tricky position
board.SetupBoardFromFEN("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");

// Copy state
info.epSquare = board.GetGameInfo().epSquare;
info.castlingRights = board.GetGameInfo().castlingRights;

// Run perft
auto result = Testing::Perft::run(board, info, 4);
std::cout << "Nodes: " << result.nodes << "\n"; // Expected: 62379
```

## Backward Compatibility

**Existing Config-based code continues to work:**
```cpp
// This still works (uses deprecated interface)
Config::GameConfig config;
std::vector<std::tuple<ePiece, eSquare>> pieces;
FENParser::ParseFEN(fen, config, pieces); // ← Deprecated but functional
```

**Migration path:**
- New code should use `FENGameState`
- Old code can migrate incrementally
- Both interfaces can coexist during transition

## Benefits of This Refactoring

1. **Board Independence** - Board no longer needs Config/Game to load FEN
2. **Testing** - Perft can now test all 6 positions (not just starting position)
3. **Parallel Search Ready** - Each thread can load positions independently
4. **UCI Support** - Easy to implement `position fen <fen>` command
5. **Future Position Class** - Clean interface ready to use
6. **Backward Compatible** - Existing code still works

## Validation

The refactored parser includes comprehensive validation:
- ✅ Format checking (regex)
- ✅ Rank/file expansion validation
- ✅ Piece limits (max 32 pieces, 8 pawns per side)
- ✅ King requirements (exactly 1 per side)
- ✅ Pawn placement (not on 1st/8th rank)
- ✅ Castling rights vs. piece positions
- ✅ En passant square consistency
- ✅ Automatic adjustment of invalid metadata

## Next Steps

1. **Immediate:** Integrate the implementations
2. **Test:** Run perft suite to verify all positions work
3. **Future:** Use this for UCI `position fen` command
4. **Future:** Migrate Position class to use FENGameState

## Troubleshooting

**If perft fails on non-starting positions:**
- Check that Board::GetGameInfo() returns the correct info
- Verify GameInfo is properly set in SetupBoardFromFEN()
- Use divide mode to see which moves are wrong

**If FEN export is incorrect:**
- Test round-trip: load FEN → extract FEN → compare
- Check that ExtractFENFromBoard() uses correct rank ordering
- Verify piece character mapping (g_cPieceNames array)

---

## Files Created

1. `FENParser.h` - Refactored header (UPDATED)
2. `FENParser.cpp` - Refactored implementation (UPDATED)
3. `Board_FEN_Implementation.txt` - Code to add to Board.cpp
4. `Perft_FEN_Update.txt` - Code to update in Perft.cpp
5. `FEN_INTEGRATION_GUIDE.md` - This document

Ready to integrate! This should take about 15-20 minutes total. Let me know if you hit any issues!
