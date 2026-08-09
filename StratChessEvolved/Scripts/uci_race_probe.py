#!/usr/bin/env python3
"""Probe the `bestmove` / `searching_` ordering in UciHandler (issue #245).

Drives the engine exactly as a GUI does -- sends the next `position` + `go` the
instant `bestmove` is read, with no delay -- and counts how often `position` is
refused with "ignored, a search is in progress".

Any refusal is a defect. `refuse_while_searching()` only logs an `info string`,
and `go` is NOT refused, so a refused `position` leaves the engine searching the
PREVIOUS position and returning a move that is illegal in the real one. That
forfeited two games in run 31281221815.

    pwsh> python StratChessEvolved\\Scripts\\uci_race_probe.py <engine.exe> [iterations]

Run it from `StratChessEvolved\\` so the engine finds game_settings.json.

## Reproducing the original race

The fixed ordering makes the window unreachable, so this script reports zero on a
correct build no matter how many iterations it runs -- which also means it cannot
FAIL on a correct build, and cannot serve as a regression test on its own. To see
the bug, widen the window in a scratch build by re-introducing the old ordering:

    send("bestmove " + bm);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // widen
    searching_.store(false, std::memory_order_release);

Measured with that probe in place: **39 of 40** iterations refused, 38 of them
returning the engine's own previous move. With the shipping order (`store` before
`send`), the same probe yields **0 of 40**.
"""
import argparse
import queue
import subprocess
import sys
import threading

# Two reversible knight moves, so the probe can cycle indefinitely without ever
# reaching a terminal position and without the move list growing without bound.
SHUFFLE = [('g1f3', 'b8c6'), ('f3g1', 'c6b8')]


def probe(engine, iterations):
    proc = subprocess.Popen([engine, 'uci'], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)
    lines = queue.Queue()

    def reader():
        for line in proc.stdout:
            lines.put(line.rstrip('\n'))
        lines.put(None)

    threading.Thread(target=reader, daemon=True).start()

    refusals = []

    def send(cmd):
        proc.stdin.write(cmd + '\n')
        proc.stdin.flush()

    def wait_for(prefix):
        while True:
            line = lines.get(timeout=60)
            if line is None:
                raise SystemExit('FAIL: engine exited mid-probe')
            if 'ignored, a search is in progress' in line:
                refusals.append(line)
            if line.startswith(prefix):
                return line

    send('uci'); wait_for('uciok')
    send('isready'); wait_for('readyok')

    moves = ['e2e4', 'e7e5']
    for i in range(iterations):
        send('position startpos moves ' + ' '.join(moves))
        send('go depth 1')          # instant, so the command loop cycles fast
        wait_for('bestmove')
        moves.extend(SHUFFLE[i % 2])
        if len(moves) > 60:
            moves = ['e2e4', 'e7e5']

    send('quit')
    proc.wait(timeout=30)

    print(f'iterations          : {iterations}')
    print(f'position refusals   : {len(refusals)}')
    if refusals:
        print('\nFAIL: `position` was refused after `bestmove` was already sent.')
        print('The next search will run on a stale board. See issue #245.')
        print('  ' + refusals[0])
        return 1
    print('\nPASS: no refusal -- the engine was accepting commands before bestmove went out.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', help='path to StratChessEvolved.exe')
    parser.add_argument('iterations', nargs='?', type=int, default=2000)
    args = parser.parse_args()
    return probe(args.engine, args.iterations)


if __name__ == '__main__':
    sys.exit(main())
