"""Mock UCI engine that provokes fastchess's move-compliance warnings on demand.

Used when upgrading the pinned fastchess release: the warning wordings the match
harnesses classify on cannot be verified by a run that happens not to emit any,
so this engine emits them deliberately. See Docs/EloMeasurement.md -> Upgrading
the match runner.

Modes:
  badpv        Plays legal moves, but every info line's `pv` starts with an
               illegal move, which fastchess reports as "Illegal PV move" (and,
               from 1.8.2, also "Bestmove does not match beginning of last PV").
               Both are PV-compliance warnings: tolerated, games unaffected.
  illegalmove  Plays an illegal move as its third bestmove -- a real defect,
               which must trip the harness guard rather than be tolerated.

Two mock engines played against each other are enough:

  fastchess -engine cmd=python args="fastchess_probe_engine.py badpv" name=a proto=uci \
            -engine cmd=python args="fastchess_probe_engine.py badpv" name=b proto=uci \
            -each tc=2+0.02 -rounds 1 -games 2 -concurrency 1

Requires python-chess, which is only needed to keep the played moves legal.
"""

import sys

import chess

ILLEGAL = "a1a8"  # Illegal in every position reachable from the start.


def say(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "badpv"
    board = chess.Board()
    ply = 0

    for line in sys.stdin:
        line = line.strip()
        if line == "uci":
            say(f"id name fake-{mode}")
            say("id author fastchess_probe_engine")
            say("uciok")
        elif line == "isready":
            say("readyok")
        elif line == "ucinewgame":
            board, ply = chess.Board(), 0
        elif line.startswith("position"):
            parts = line.split()
            if "startpos" in parts:
                board = chess.Board()
                rest = parts[parts.index("startpos") + 1:]
            else:
                i = parts.index("fen") + 1
                board = chess.Board(" ".join(parts[i:i + 6]))
                rest = parts[i + 6:]
            for uci in rest[1:] if rest[:1] == ["moves"] else []:
                board.push_uci(uci)
        elif line.startswith("go"):
            ply += 1
            legal = list(board.legal_moves)
            if not legal:
                say("bestmove 0000")
                continue
            best = legal[0].uci()
            pv = f"{ILLEGAL} {best}" if mode == "badpv" else best
            say(f"info depth 5 score cp 10 nodes 100 time 1 pv {pv}")
            say(f"bestmove {ILLEGAL if mode == 'illegalmove' and ply == 3 else best}")
        elif line == "quit":
            break


if __name__ == "__main__":
    main()
