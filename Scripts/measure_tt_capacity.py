#!/usr/bin/env python3
"""Measure what transposition-table capacity buys on a game-like workload.

Replays strength-lab games through a `-DSTRAT_TT_STATS=1` build at several `Hash` sizes and
prints the `info string ttstats` counters summed per size. Every size searches the same
positions, so the rows differ only in table capacity:

    python Scripts/measure_tt_capacity.py <ttstats engine.exe> <match.pgn> [--sizes 64 192 256 512]
        [--nodes 1200000] [--games 20] [--jobs 4]
    python Scripts/measure_tt_capacity.py --self-test

Each game is searched by two engine processes, one per side, fed only that side's moves -- the
way a game gives each engine one search per two plies, so table ageing matches play. Every
search is `go nodes N` at Threads=1, which makes the result independent of machine load; pick N
so that `depth` matches the lab PGN's own `{score/depth time}` comments.

`Hash` allocates a power-of-two bucket count rounded down, so the `MiB` column is what was
allocated, not what was asked for (192 allocates 128).

Reading it: `cut%` is main-search cutoffs per probe, the counter a larger table would move; `cur%`
is stores that evicted an entry written in the same search, the pressure a larger table would
relieve. If `cut%` and `depth` are flat across sizes and `cur%` is small, capacity does not
matter at the node budget.
"""
import argparse
import io
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import chess
import chess.pgn

COUNTERS = ('mainprobes', 'mainhits', 'maincutoffs', 'qsprobes', 'qshits', 'qscutoffs',
            'stores', 'declined', 'filled', 'refreshed', 'evictstale', 'evictcurrent')

INFO_DEPTH = re.compile(r'^info depth (\d+) .*?\bhashfull (\d+)')


def allocated_mib(requested):
    """64-byte buckets, count rounded down to a power of two: largest power of two <= request."""
    return 1 << (requested.bit_length() - 1)


def parse_ttstats(line):
    fields = line.split()[3:]
    stats = dict(zip(fields[0::2], (int(v) for v in fields[1::2])))
    missing = [c for c in COUNTERS if c not in stats]
    if missing:
        raise ValueError(f'ttstats line lacks {missing}: {line}')
    return stats


def load_games(pgn_text, limit):
    """Return (start FEN, [uci moves]) for the first `limit` games."""
    games = []
    stream = io.StringIO(pgn_text)
    while len(games) < limit:
        game = chess.pgn.read_game(stream)
        if game is None:
            break
        if game.errors:
            raise ValueError(f'game {len(games) + 1} does not parse: {game.errors[0]}')
        board = game.board()
        games.append((board.fen(), [m.uci() for m in game.mainline_moves()]))
    return games


class Engine:
    def __init__(self, exe, hash_mb):
        self.proc = subprocess.Popen([exe, 'uci'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.send('uci')
        self.wait_for('uciok')
        self.send('setoption name Threads value 1')
        self.send(f'setoption name Hash value {hash_mb}')
        self.send('ucinewgame')
        self.send('isready')
        self.wait_for('readyok')

    def send(self, cmd):
        self.proc.stdin.write(cmd + '\n')
        self.proc.stdin.flush()

    def wait_for(self, prefix, sink=None):
        for line in self.proc.stdout:
            line = line.rstrip('\n')
            if sink is not None:
                sink(line)
            if line.startswith(prefix):
                return line
        raise RuntimeError(f'engine exited while waiting for {prefix!r}')

    def search(self, fen, moves, nodes):
        """Return (completed depth, final hashfull, ttstats dict) for one `go nodes` search."""
        seen = {'depth': 0, 'hashfull': 0, 'stats': None}

        def sink(line):
            if m := INFO_DEPTH.match(line):
                seen['depth'], seen['hashfull'] = int(m.group(1)), int(m.group(2))
            elif line.startswith('info string ttstats'):
                seen['stats'] = parse_ttstats(line)

        self.send(f'position fen {fen} moves {" ".join(moves)}'.rstrip())
        self.send(f'go nodes {nodes}')
        self.wait_for('bestmove', sink)
        if seen['stats'] is None:
            raise RuntimeError('no `info string ttstats` line -- is this a -DSTRAT_TT_STATS=1 build?')
        return seen['depth'], seen['hashfull'], seen['stats']

    def close(self):
        if self.proc.poll() is None:
            self.send('quit')
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def new_totals():
    return {'searches': 0, 'depth': 0, 'hashfull': 0, **{c: 0 for c in COUNTERS}}


def replay_game(exe, hash_mb, nodes, game):
    fen, moves = game
    totals = new_totals()
    sides = [Engine(exe, hash_mb), Engine(exe, hash_mb)]
    try:
        # The position before each played move is searched by the side that played it.
        for ply in range(len(moves)):
            depth, hashfull, stats = sides[ply % 2].search(fen, moves[:ply], nodes)
            totals['searches'] += 1
            totals['depth'] += depth
            totals['hashfull'] += hashfull
            for c in COUNTERS:
                totals[c] += stats[c]
    finally:
        for engine in sides:
            engine.close()
    return hash_mb, totals


def pct(num, den):
    return 100.0 * num / den if den else 0.0


def report(results, sizes, nodes, games, out=sys.stdout):
    print(f'{games} games, go nodes {nodes}, Threads=1', file=out)
    header = (f'{"Hash":>5} {"MiB":>5} {"search":>7} {"depth":>6} {"full‰":>6} {"hit%":>6} {"cut%":>6} '
              f'{"qscut%":>7} {"cur%":>6} {"stale%":>7} {"decl%":>6}')
    print(header, file=out)
    for size in sizes:
        t = results[size]
        n = t['searches']
        print(f'{size:>5} {allocated_mib(size):>5} {n:>7} {t["depth"] / n:>6.2f} {t["hashfull"] / n:>6.0f} '
              f'{pct(t["mainhits"], t["mainprobes"]):>6.2f} {pct(t["maincutoffs"], t["mainprobes"]):>6.2f} '
              f'{pct(t["qscutoffs"], t["qsprobes"]):>7.2f} {pct(t["evictcurrent"], t["stores"]):>6.2f} '
              f'{pct(t["evictstale"], t["stores"]):>7.2f} {pct(t["declined"], t["stores"]):>6.2f}', file=out)


def self_test():
    ok = True

    def check(name, passed):
        nonlocal ok
        print(f'{"PASS" if passed else "FAIL"}  {name}')
        ok &= passed

    check('192 allocates 128', allocated_mib(192) == 128)
    check('256 allocates 256', allocated_mib(256) == 256)
    line = 'info string ttstats ' + ' '.join(f'{c} {i}' for i, c in enumerate(COUNTERS))
    check('ttstats parses every counter', parse_ttstats(line)['evictcurrent'] == 11)
    try:
        parse_ttstats('info string ttstats mainprobes 1')
        check('truncated ttstats rejected', False)
    except ValueError:
        check('truncated ttstats rejected', True)
    m = INFO_DEPTH.match('info depth 12 score cp 5 nodes 900 hashfull 37 time 4 pv e2e4')
    check('info depth parses depth and hashfull', m is not None and m.groups() == ('12', '37'))
    pgn = ('[FEN "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1"]\n[SetUp "1"]\n\n1. e4 {+1.00/5 0.1s} Kd7 *\n\n'
           '[Event "?"]\n\n1. Nf3 Nf6 *\n')
    games = load_games(pgn, 5)
    check('FEN game converts SAN to UCI', games[0] == ('4k3/8/8/8/8/8/4P3/4K3 w - - 0 1', ['e2e4', 'e8d7']))
    check('startpos game converts', games[1][1] == ['g1f3', 'g8f6'] and len(games) == 2)
    check('game limit respected', len(load_games(pgn, 1)) == 1)
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('engine', nargs='?', help='a -DSTRAT_TT_STATS=1 StratChessEvolved.exe')
    parser.add_argument('pgn', nargs='?', help='strength-lab match.pgn')
    parser.add_argument('--sizes', type=int, nargs='+', default=[64, 192, 256, 512])
    parser.add_argument('--nodes', type=int, default=1_200_000)
    parser.add_argument('--games', type=int, default=20)
    parser.add_argument('--jobs', type=int, default=4, help='games replayed concurrently')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()

    if args.self_test:
        return 0 if self_test() else 1
    if not args.engine or not args.pgn:
        parser.error('engine and pgn are required')
    if min(args.sizes) < 1 or args.nodes < 1 or args.games < 1 or args.jobs < 1:
        parser.error('--sizes, --nodes, --games and --jobs must be positive')

    with open(args.pgn, encoding='utf-8') as f:
        games = load_games(f.read(), args.games)
    if not games:
        parser.error(f'no games in {args.pgn}')

    results = {size: new_totals() for size in args.sizes}
    # Interleaved by game, so concurrent jobs mix sizes instead of all holding the largest table.
    tasks = [(size, game) for game in games for size in args.sizes]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(replay_game, args.engine, size, args.nodes, game) for size, game in tasks]
        for done, future in enumerate(futures, 1):
            size, totals = future.result()
            for key, value in totals.items():
                results[size][key] += value
            print(f'\r{done}/{len(tasks)} game replays', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    report(results, args.sizes, args.nodes, len(games))
    return 0


if __name__ == '__main__':
    sys.exit(main())
