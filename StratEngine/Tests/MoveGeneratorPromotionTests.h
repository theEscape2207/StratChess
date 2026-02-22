#pragma once
#include "StdAfx.h"
#include <iostream>

#include "Board.h"
#include "MoveGenerator.h"
#include "Move.h"
#include "PieceHelper.h"

// Minimal test runner: returns 0 on success, non-zero on failure.
int main()
{
    using namespace std;
    Board& board = Board::Instance();
    board.ClearBoard();
    board.SetInitialColor(WHITE);

    GameInfo info; // default epSquare = NO_SQUARE

    // Test 1: quiet promotion (white pawn b7 -> b8)
    board.ClearBoard();
    board.SetInitialColor(WHITE);
    board.AddPieceToBoard(ePiece::WHITE_PAWN, b7);
    // Ensure b8 is empty
    MoveList moves;
    MoveGenerator::ComputeLegalMoves(info, moves);

    bool foundQueenPromo = false;
    for (const Move& m : moves) {
        if (m.from() == b7 && m.to() == b8) {
            if (static_cast<MoveType>(m.flags()) == MoveType::PROMOTION_QUEEN) {
                foundQueenPromo = true;
                break;
            }
        }
    }
    if (!foundQueenPromo) {
        cerr << "ERROR: quiet promotion to queen not generated\n";
        return 2;
    }
    cout << "OK: quiet promotion to queen generated\n";

    // Test 2: capture promotion (white pawn c7 -> b8 capturing black rook)
    board.ClearBoard();
    board.SetInitialColor(WHITE);
    board.AddPieceToBoard(ePiece::WHITE_PAWN, c7);
    board.AddPieceToBoard(ePiece::BLACK_ROOK, b8);

    moves.clear();
    MoveGenerator::ComputeLegalMoves(info, moves);

    bool foundCaptureQueenPromo = false;
    for (const Move& m : moves) {
        if (m.from() == c7 && m.to() == b8 &&
            static_cast<MoveType>(m.flags()) == MoveType::PROMOTION_QUEEN) {
            if (PieceHelper::IsActual(m.Content)) {
                foundCaptureQueenPromo = true;
                break;
            }
        }
    }
    if (!foundCaptureQueenPromo) {
        cerr << "ERROR: capture-promotion to queen not generated\n";
        return 3;
    }
    cout << "OK: capture-promotion to queen generated\n";

    cout << "All MoveGenerator promotion tests passed." << endl;
    return 0;
}
