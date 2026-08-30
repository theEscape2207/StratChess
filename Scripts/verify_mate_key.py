"""Verify that a move forces checkmate within N full moves.

Usage:    python verify_mate_key.py "<FEN>" <uci_move> <N>
Exit 0 and prints CONFIRMED if the move forces mate in <= N moves,
exit 1 and prints REFUTED otherwise.

Requires: pip install python-chess
Purpose:  ground-truth check when the engine answers a tactical-suite mate
position with a move other than the EPD key — CONFIRMED alternatives may be
added to best_moves; REFUTED ones may not (memory rule: verify against
engine/ground truth, never trust manual analysis).
"""
import sys
import chess


def exists_forcing(board, n):
    # Side to move has some move forcing mate in <= n (checks-first ordering
    # prunes hard: mate keys are almost always checks).
    moves = sorted(board.legal_moves, key=lambda m: not board.gives_check(m))
    return any(forces(board, m, n) for m in moves)


def forces(board, mv, n):
    board.push(mv)
    try:
        if board.is_checkmate():
            return True
        if n <= 1 or board.is_game_over():
            return False
        for reply in list(board.legal_moves):
            board.push(reply)
            try:
                if not exists_forcing(board, n - 1):
                    return False
            finally:
                board.pop()
        return True
    finally:
        board.pop()


if __name__ == "__main__":
    fen, uci, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    board = chess.Board(fen)
    ok = forces(board, chess.Move.from_uci(uci), n)
    print(f"{'CONFIRMED' if ok else 'REFUTED'}: {uci} mate<={n} in {fen}")
    sys.exit(0 if ok else 1)
