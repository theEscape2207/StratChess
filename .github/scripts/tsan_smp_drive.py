#!/usr/bin/env python3
"""Drive multi-threaded searches over UCI, for a ThreadSanitizer build.

Why this exists: the Catch2 `[smp]` tests only check `SetThreads()` clamping, and
the rest of the fast tier is single-threaded. A TSan run over the test binary
therefore spawns no helper threads and cannot fail for the reason the job exists.
This drives the real path -- `go` with `Threads` > 1, which spawns Lazy SMP
helpers sharing the transposition table.

Two things it must get right:

* **Wait for each command's completion token.** The engine reads stdin faster
  than it searches, so piping every command at once delivers `position` and
  `setoption` mid-search, where the UCI guards correctly refuse them -- and the
  searches under test never run.
* **Treat an early exit as failure.** With `-fno-sanitize-recover=all` a race
  aborts the process, so the engine dying mid-scenario IS the finding. Waiting
  forever on a token that will never arrive would hang the job instead.

Usage:
    tsan_smp_drive.py <engine-binary> [--timeout SECONDS]

The engine is invoked as `<binary> uci`. Run it under `setarch $(uname -m) -R`
on Linux: Ubuntu 24.04's `vm.mmap_rnd_bits` exceeds what TSan's shadow mapping
tolerates, and the process dies with "unexpected memory mapping" before `main`,
reporting zero races on the way out.
"""
import argparse
import queue
import subprocess
import sys
import threading
import time

KIWIPETE = 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1'
ROOK_ENDGAME = '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1'

# Each scenario is a list of UCI commands, plus three directives the driver
# interprets itself:
#
#   @nowait <cmd>  send and carry on, rather than waiting for a completion token
#   @sleep <secs>  let the search run for a while
#   @await <token> block until the engine emits this line
#
# Those exist for the abort scenarios: `stop` has to arrive WHILE the helpers are
# running, which is impossible if every `go` blocks until `bestmove`.
#
# Between them the scenarios cover: helpers on a quiet position and a tactical
# one, several searches over a warm shared TT, a `ucinewgame` clear, SetThreads
# on a live AI, an externally requested `stop`, the time-managed abort path, and
# heavy oversubscription. Depths are chosen to keep each scenario seconds long
# under TSan's ~8x.
SCENARIOS = {
    'threads4-startpos': [
        'uci', 'setoption name Threads value 4', 'isready',
        'position startpos', 'go depth 8',
    ],
    'threads4-tactical': [
        'uci', 'setoption name Threads value 4', 'isready',
        f'position fen {KIWIPETE}', 'go depth 8',
    ],
    'threads4-sequence': [
        'uci', 'setoption name Threads value 4', 'isready',
        'position startpos', 'go depth 7',
        'position startpos moves e2e4 e7e5', 'go depth 7',
        'ucinewgame', 'isready',
        f'position fen {ROOK_ENDGAME}', 'go depth 8',
    ],
    'threads8-reconfigure': [
        'uci', 'setoption name Threads value 8', 'isready',
        'position startpos', 'go depth 8',
        'setoption name Threads value 2', 'isready',
        'position startpos', 'go depth 7',
    ],
    # The abort path: `stop` latches the shared flag while every helper is mid-tree
    # and polling it. Nothing else here exercises a search ending on anything but
    # its own depth limit.
    'threads8-stop-midsearch': [
        'uci', 'setoption name Threads value 8', 'isready',
        'position startpos', '@nowait go depth 30',
        '@sleep 2', '@nowait stop', '@await bestmove',
        'isready',
        f'@nowait position fen {KIWIPETE}', '@nowait go depth 30',
        '@sleep 2', '@nowait stop', '@await bestmove',
    ],
    # The time-managed abort: thread 0 alone calls the wall clock and latches the
    # flag the helpers poll, so the timeout path differs from `stop` in who
    # decides. Two back-to-back movetime searches also mean helpers from the first
    # are joining while the second is arming its timer.
    'threads4-movetime': [
        'uci', 'setoption name Threads value 4', 'isready',
        'position startpos', 'go movetime 1500',
        f'position fen {ROOK_ENDGAME}', 'go movetime 1500',
    ],
    # Heavy oversubscription: 16 helpers on a 4-vCPU runner interleave far more
    # aggressively than one-thread-per-core, which is what surfaces a race that a
    # comfortable thread count hides.
    'threads16-oversubscribed': [
        'uci', 'setoption name Threads value 16', 'isready',
        'position startpos', 'go depth 8',
        f'position fen {KIWIPETE}', 'go depth 7',
    ],
}


def token_for(command):
    """The line that means this command is finished, or None if it is immediate."""
    if command == 'uci':
        return 'uciok'
    if command == 'isready':
        return 'readyok'
    if command.startswith('go'):
        return 'bestmove'
    return None


def run_scenario(engine, name, commands, timeout):
    print(f'--- {name}', flush=True)
    proc = subprocess.Popen([engine, 'uci'], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)
    lines = queue.Queue()

    def reader():
        for line in proc.stdout:
            sys.stdout.write(f'    {line}')
            lines.put(line.strip())
        lines.put(None)  # EOF: the process is gone

    threading.Thread(target=reader, daemon=True).start()

    def wait_for(token, command):
        while True:
            try:
                line = lines.get(timeout=timeout)
            except queue.Empty:
                proc.kill()
                raise SystemExit(
                    f'FAIL [{name}]: timed out after {timeout}s waiting for '
                    f'{token!r} following {command!r}')
            if line is None:
                raise SystemExit(
                    f'FAIL [{name}]: engine exited while waiting for {token!r} '
                    f'following {command!r} -- with -fno-sanitize-recover this is '
                    f'what a TSan report looks like; read the log above')
            if line.startswith(token):
                return

    def send(command):
        proc.stdin.write(command + '\n')
        proc.stdin.flush()

    try:
        for command in commands:
            if command.startswith('@sleep '):
                time.sleep(float(command.split(None, 1)[1]))
            elif command.startswith('@await '):
                wait_for(command.split(None, 1)[1], command)
            elif command.startswith('@nowait '):
                send(command.split(None, 1)[1])
            else:
                send(command)
                token = token_for(command)
                if token:
                    wait_for(token, command)
        send('quit')
    except BrokenPipeError:
        raise SystemExit(f'FAIL [{name}]: engine closed its input -- it died mid-scenario')

    code = proc.wait(timeout=timeout)
    if code != 0:
        raise SystemExit(f'FAIL [{name}]: engine exited {code}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', help='path to the StratChessEvolved binary')
    parser.add_argument('--timeout', type=int, default=600,
                        help='seconds to wait for any single completion token')
    args = parser.parse_args()

    for name, commands in SCENARIOS.items():
        run_scenario(args.engine, name, commands, args.timeout)

    print(f'\nPASS: {len(SCENARIOS)} multi-threaded scenarios completed with no '
          f'ThreadSanitizer report.')


if __name__ == '__main__':
    main()
