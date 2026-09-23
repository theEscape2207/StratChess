#!/usr/bin/env python3
"""Read the margin at which a UCI spin option changes `bestmove`.

For each position in a corpus, bisect a spin option over a range and report the smallest value
at which the engine's chosen move differs from its choice at the range minimum. That value is
the margin, in the option's own units, between the move the search prefers by default and the
best alternative it can find -- the engine has neither MultiPV nor `searchmoves`, so bisecting
the option is the only way to read it.

    python Scripts/bisect_uci_option.py corpus.jsonl --engine <exe> --option Contempt \
        --range 0 100 --depth 12 --workers 20 --out results.jsonl
    python Scripts/bisect_uci_option.py --self-test
    python Scripts/bisect_uci_option.py corpus.jsonl --self-check

Input is one position per line, either a JSON object `{"id": ..., "fen": ..., "expect": ...}` or
a bare FEN, which is what `build_corpus.py --out` writes. `expect` is optional and names the
move whose displacement is being measured; when it is given and the baseline search does not
reproduce it, the position is reported as `baseline_mismatch` instead of being counted.

The baseline is the range MINIMUM and the search runs upward from it, so to read the margin
around a non-zero default, start the range at that default.

Status per position:

  flip              `bestmove` differs at the range maximum; `threshold` is the bisected minimum
  flip_nonmonotone  no flip at the maximum, but one at an interior sample
  unreachable       never flips anywhere probed
  excluded_mate     the baseline search returns a mate score, so `bestmove` was not a choice
  baseline_mismatch `expect` was given and the baseline `bestmove` is not it

Three things this encodes, each of which produced a wrong number before it was caught:

1. A bisect anchored only at the range maximum cannot see a position that flips at an interior
   value and reverts by the maximum. Quartile samples are probed before any position is called
   `unreachable`; on the contempt corpus those were 9-12% of all flips.
2. The baseline must reproduce the move being displaced. Supply `expect` and read the
   `baseline_mismatch` count -- it is a separate bucket, never folded into the denominator.
3. Determinism is not free. `ucinewgame` + `isready` precede EVERY search and `Threads=1` is set
   once. `AIPerplex::SetTuning()` clears the table on a tuning change but skips the clear when
   the value is unchanged, so without the explicit reset two consecutive searches at the
   baseline value would share a table. Fixed depth, never `movetime`, so a result does not
   depend on host speed.

`threshold` is exact only where the option behaves monotonically inside the bracket that was
bisected; where it does not, it is an upper bound on the smallest flipping value.

Two silent-garbage modes are rejected rather than reported: a transcript that never reached the
requested depth (`go` is asynchronous, and a search that is cut short still answers with a
plausible-looking `bestmove` backed by `nodes 0`), and a `setoption` the engine did not
acknowledge. Either aborts the run naming the position.

python-chess is needed only by --self-check.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

FLIP = 'flip'
FLIP_NONMONOTONE = 'flip_nonmonotone'
UNREACHABLE = 'unreachable'
EXCLUDED_MATE = 'excluded_mate'
BASELINE_MISMATCH = 'baseline_mismatch'
STATUSES = (FLIP, FLIP_NONMONOTONE, UNREACHABLE, EXCLUDED_MATE, BASELINE_MISMATCH)

OPTION_RE = re.compile(r'^option name (.+?) type (\w+)(?:.*?\bmin (-?\d+) max (-?\d+))?\s*$')
INFO_DEPTH_RE = re.compile(r'^info depth (\d+)\b')
SCORE_RE = re.compile(r'\bscore (cp|mate) (-?\d+)\b')


class EngineError(RuntimeError):
    """The engine answered in a way that makes the result meaningless, not merely absent."""


class SearchResult:
    __slots__ = ('move', 'cp', 'mate', 'depth')

    def __init__(self, move, cp, mate, depth):
        self.move = move
        self.cp = cp
        self.mate = mate
        self.depth = depth


class Engine:
    """One UCI process, driven at fixed depth with a cleared table before every search."""

    def __init__(self, argv):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.options = {}
        self.send('uci')
        self.wait_for('uciok', self._collect_option)
        self.send('setoption name Threads value 1')
        self.send('isready')
        self.wait_for('readyok')

    def _collect_option(self, line):
        if match := OPTION_RE.match(line):
            name, kind, lo, hi = match.groups()
            self.options[name] = (kind, int(lo) if lo else None, int(hi) if hi else None)

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
        raise EngineError(f'engine exited while waiting for {prefix!r}')

    def set_option(self, name, value):
        """Set a spin option and require the engine's acknowledgement.

        An unrecognised name is answered with silence and a rejected value with a
        `not applied` note, both of which would otherwise leave the whole run measuring the
        default and reporting every position as unreachable.
        """
        applied = []
        self.send(f'setoption name {name} value {value}')
        self.send('isready')

        def sink(line):
            if line.startswith(f'info string {name} '):
                applied.append(line)

        self.wait_for('readyok', sink)
        if not applied:
            raise EngineError(f'the engine did not acknowledge `setoption name {name} value {value}`')
        note = applied[-1]
        if 'not applied' in note:
            raise EngineError(f'{name}={value} was refused: {note}')

    def search(self, fen, depth):
        """Return the fixed-depth SearchResult for `fen`, from a cleared table."""
        self.send('ucinewgame')
        self.send('isready')
        self.wait_for('readyok')

        seen = {'depth': 0, 'kind': None, 'value': 0}

        def sink(line):
            match = INFO_DEPTH_RE.match(line)
            if not match:
                return
            reached = int(match.group(1))
            score = SCORE_RE.search(line)
            if score is None or reached < seen['depth']:
                return
            seen['depth'] = reached
            seen['kind'], seen['value'] = score.group(1), int(score.group(2))

        self.send(f'position fen {fen}')
        self.send(f'go depth {depth}')
        line = self.wait_for('bestmove', sink)

        parts = line.split()
        move = parts[1] if len(parts) > 1 else ''
        if not move or move in ('(none)', '0000'):
            raise EngineError(f'no move returned for {fen}')
        # A mate score ends the search at whatever depth found it, so a short transcript is the
        # expected answer there and only a centipawn one has to have reached the full depth.
        if seen['kind'] != 'mate' and seen['depth'] < depth:
            raise EngineError(f'search of {fen} reported depth {seen["depth"]}, not the requested '
                              f'{depth} -- the transcript is of an aborted search, not a decision')
        cp = seen['value'] if seen['kind'] == 'cp' else None
        mate = seen['value'] if seen['kind'] == 'mate' else None
        return SearchResult(move, cp, mate, seen['depth'])

    def close(self):
        if self.proc.poll() is None:
            self.send('quit')
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def load_corpus(path):
    """Read one position per line, as a JSON object or as a bare FEN."""
    entries = []
    for number, raw in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('{'):
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f'{path}:{number}: {exc}') from exc
            if 'fen' not in record:
                raise ValueError(f'{path}:{number}: object has no "fen"')
            entry = {'id': record.get('id', number), 'fen': record['fen'].strip(),
                     'expect': record.get('expect')}
        else:
            entry = {'id': number, 'fen': line, 'expect': None}
        if entry['expect']:
            entry['expect'] = entry['expect'].strip().lower()
        entries.append(entry)
    return entries


def interior_samples(lo, hi):
    """Ascending quartiles of the range, excluding both ends."""
    values = []
    for quarter in (1, 2, 3):
        value = lo + (hi - lo) * quarter // 4
        if lo < value < hi and value not in values:
            values.append(value)
    return values


def smallest_flip(move_at, base_move, lo, hi):
    """Binary-search the smallest value in (lo, hi] whose move differs from `base_move`.

    The caller has established that lo does not flip and hi does. Exact where the option is
    monotone inside that bracket, an upper bound where it is not.
    """
    low, high = lo, hi
    while high - low > 1:
        mid = (low + high) // 2
        if move_at(mid) != base_move:
            high = mid
        else:
            low = mid
    return high


def analyse_position(engine, entry, option, lo, hi, depth):
    """Bisect one position and return its result row."""
    cache = {}

    def result_at(value):
        if value not in cache:
            engine.set_option(option, value)
            cache[value] = engine.search(entry['fen'], depth)
        return cache[value]

    def move_at(value):
        return result_at(value).move

    row = {'id': entry['id'], 'fen': entry['fen'], 'expect': entry['expect'],
           'threshold': None, 'flip_move': None}
    base = result_at(lo)
    row['baseline'] = base.move
    row['baseline_cp'] = base.cp

    # A mate score ends the search at the depth that found it, so the move it reports was not
    # weighed against alternatives. Excluded before `expect` is consulted: such a position cannot
    # answer the question either way.
    if base.mate is not None:
        row['status'] = EXCLUDED_MATE
    elif entry['expect'] and base.move != entry['expect']:
        row['status'] = BASELINE_MISMATCH
    else:
        flip_at = hi if move_at(hi) != base.move else None
        status = FLIP if flip_at is not None else UNREACHABLE
        if flip_at is None:
            for sample in interior_samples(lo, hi):
                if move_at(sample) != base.move:
                    flip_at, status = sample, FLIP_NONMONOTONE
                    break
        row['status'] = status
        if flip_at is not None:
            row['threshold'] = smallest_flip(move_at, base.move, lo, flip_at)
            row['flip_move'] = cache[row['threshold']].move

    row['searches'] = len(cache)
    return row


def percentile(values, fraction):
    """Nearest-rank percentile of an already sorted, non-empty list."""
    rank = max(1, min(len(values), int(fraction * len(values) + 0.5)))
    return values[rank - 1]


def summarise(rows, lo, hi):
    """Status counts, the cumulative flip curve and the margin distribution."""
    counts = {status: 0 for status in STATUSES}
    for row in rows:
        counts[row['status']] += 1
    thresholds = sorted(row['threshold'] for row in rows if row['threshold'] is not None)
    curve = []
    for step in range(1, 11):
        value = lo + (hi - lo) * step // 10
        curve.append((value, sum(1 for t in thresholds if t <= value)))
    return {
        'positions': len(rows),
        'searches': sum(row['searches'] for row in rows),
        'counts': counts,
        'thresholds': thresholds,
        'curve': curve,
    }


def report(summary, option, lo, hi, depth, out=sys.stdout):
    total = summary['positions']
    print(f'{total} position(s), --option {option} --range {lo} {hi} --depth {depth}, Threads=1', file=out)
    print(f'{summary["searches"]} search(es)', file=out)
    print('\nstatus', file=out)
    for status in STATUSES:
        count = summary['counts'][status]
        share = 100.0 * count / total if total else 0.0
        print(f'  {status:<18}{count:>8}{share:>8.1f}%', file=out)

    thresholds = summary['thresholds']
    if not thresholds:
        print('\nno position flipped anywhere in the range', file=out)
        return
    print(f'\ncumulative flips by option value (of {total} position(s))', file=out)
    for value, count in summary['curve']:
        share = 100.0 * count / total if total else 0.0
        print(f'  <={value:<10}{count:>8}{share:>8.1f}%', file=out)
    print(f'\nmargin distribution over {len(thresholds)} flipped position(s)', file=out)
    labels = (('min', 0.0), ('p25', 0.25), ('median', 0.5), ('p75', 0.75), ('max', 1.0))
    line = '  '.join(f'{name} {percentile(thresholds, fraction)}' for name, fraction in labels)
    print(f'  {line}', file=out)


def run(corpus, exe, option, lo, hi, depth, workers, progress=None):
    """Analyse every position, one engine process per worker thread."""
    local = threading.local()
    engines = []
    guard = threading.Lock()

    def engine_for_thread():
        if getattr(local, 'engine', None) is None:
            local.engine = Engine([exe, 'uci'])
            kind, opt_lo, opt_hi = local.engine.options.get(option, (None, None, None))
            if kind != 'spin':
                raise EngineError(f'{option} is not a spin option'
                                  + (f' (the engine advertises it as {kind})' if kind else
                                     ' -- the engine does not advertise it at all'))
            if opt_lo is not None and (lo < opt_lo or hi > opt_hi):
                raise EngineError(f'--range {lo} {hi} leaves {option}\'s advertised {opt_lo}..{opt_hi}')
            with guard:
                engines.append(local.engine)
        return local.engine

    def task(entry):
        return analyse_position(engine_for_thread(), entry, option, lo, hi, depth)

    rows = []
    try:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            for done, row in enumerate(pool.map(task, corpus), 1):
                rows.append(row)
                if progress is not None:
                    progress(done, len(corpus))
    finally:
        for engine in engines:
            engine.close()
    return rows


def self_check(corpus, out=sys.stdout):
    """Assert the corpus invariants: every FEN legal, and every `expect` legal in it."""
    try:
        import chess
    except ImportError as exc:  # pragma: no cover - environment guard, not test logic
        print(f'--self-check requires the python-chess package (import failed: {exc})', file=out)
        return False

    ok = True

    def check(name, passed, detail):
        nonlocal ok
        ok = ok and passed
        print(f'  [{"PASS" if passed else "FAIL"}] {name}: {detail}', file=out)

    bad_fens, bad_moves, expects = [], [], 0
    for entry in corpus:
        try:
            board = chess.Board(entry['fen'])
            if not board.is_valid():
                raise ValueError(str(board.status()))
        except ValueError as exc:
            bad_fens.append(f'{entry["id"]}: {exc}')
            continue
        if not entry['expect']:
            continue
        expects += 1
        try:
            if chess.Move.from_uci(entry['expect']) not in board.legal_moves:
                raise ValueError('not legal in this position')
        except ValueError as exc:
            bad_moves.append(f'{entry["id"]}: {entry["expect"]}: {exc}')

    ids = [entry['id'] for entry in corpus]
    check('positions present', bool(corpus), f'{len(corpus)} entr(ies)')
    check('ids unique', len(set(ids)) == len(ids), f'{len(set(ids))} distinct of {len(ids)}')
    check('every FEN legal', not bad_fens,
          f'{len(bad_fens)} rejected' + (f' (first: {bad_fens[0]})' if bad_fens else ''))
    check('every expect legal in its FEN', not bad_moves,
          f'{expects} supplied, {len(bad_moves)} rejected'
          + (f' (first: {bad_moves[0]})' if bad_moves else ''))
    return ok


# A scripted engine for --self-test: it answers from a table keyed on the option value, so the
# bisection, the transcript checks and the setoption acknowledgement are exercised through the
# real driver and a real pipe. FakeUciEngine.ps1 covers driver misbehaviour, not an
# option-dependent `bestmove`, which is why this is here instead.
STUB_ENGINE = r'''
import json
import sys

spec = json.load(open(sys.argv[1]))
log_path = sys.argv[2]
values = {name: opt.get('default', 0) for name, opt in spec['options'].items()}
newgames = 0
records = []
fen = None

while True:
    line = sys.stdin.readline()
    if not line:
        break
    line = line.strip()
    if line == 'uci':
        for name, opt in spec['options'].items():
            if opt['type'] == 'spin':
                print(f"option name {name} type spin default {opt.get('default', 0)} "
                      f"min {opt['min']} max {opt['max']}")
            else:
                print(f"option name {name} type check default false")
        print('uciok')
    elif line == 'isready':
        print('readyok')
    elif line == 'ucinewgame':
        newgames += 1
    elif line.startswith('setoption name '):
        name, _, value = line[len('setoption name '):].partition(' value ')
        if name in spec.get('silent_options', []):
            values[name] = int(value)
        elif name in spec['options']:
            values[name] = int(value)
            print(f'info string {name} {value}')
        elif name == 'Threads':
            print(f'info string Threads {value}')
    elif line.startswith('position fen '):
        fen = line[len('position fen '):].strip()
    elif line.startswith('go depth '):
        depth = int(line.split()[2])
        rule = spec['positions'][fen]
        move = rule['moves'][0][1]
        for threshold, candidate in rule['moves']:
            if values[spec['option']] >= threshold:
                move = candidate
        records.append({'fen': fen, 'value': values[spec['option']], 'newgames': newgames})
        if rule.get('aborted'):
            print(f'info depth 0 score cp 0 nodes 0 time 0 pv {move}')
        elif rule.get('mate'):
            # Shallower than requested on purpose: the engine stops as soon as it sees a mate.
            print(f'info depth 2 score mate 3 nodes 40 time 1 pv {move}')
        else:
            print(f'info depth 1 score cp 11 nodes 5 time 1 pv {move}')
            print(f'info depth {depth} score cp 12 nodes 50 time 1 pv {move}')
        print(f'bestmove {move}')
    elif line == 'quit':
        break
    sys.stdout.flush()

json.dump({'newgames': newgames, 'records': records}, open(log_path, 'w'))
'''

START = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
AFTER_E4 = 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1'
AFTER_E4_E5 = 'rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2'


def _stub_exe(tmp, spec, log):
    """Write the scripted engine and its spec, and return an argv-style executable path."""
    script = Path(tmp) / 'stub_engine.py'
    script.write_text(STUB_ENGINE, encoding='utf-8')
    spec_path = Path(tmp) / 'spec.json'
    spec_path.write_text(json.dumps(spec), encoding='utf-8')
    return [sys.executable, str(script), str(spec_path), str(log)]


def self_test(out=sys.stdout):
    """Fixture checks for the bisection, the traps and the corpus reader."""
    failures = []

    def check(name, passed, detail):
        print(f'  [{"PASS" if passed else "FAIL"}] {name}: {detail}', file=out)
        if not passed:
            failures.append(name)

    # Pure bisection, with no engine in the way: the bracket ends are where an off-by-one hides.
    for flip_value in (1, 40, 99, 100):
        calls = []

        def move_at(value, flip_value=flip_value, calls=calls):
            calls.append(value)
            return 'b2b3' if value >= flip_value else 'a2a3'

        got = smallest_flip(move_at, 'a2a3', 0, 100)
        check(f'smallest_flip finds {flip_value}', got == flip_value,
              f'{got} in {len(calls)} search(es)')
    check('interior samples are the quartiles', interior_samples(0, 100) == [25, 50, 75],
          f'{interior_samples(0, 100)}')
    check('interior samples of a narrow range exclude both ends', interior_samples(0, 2) == [1],
          f'{interior_samples(0, 2)}')

    positions = {
        START: {'moves': [[0, 'a2a3'], [40, 'b2b3']]},                      # monotone
        AFTER_E4_E5: {'moves': [[0, 'a2a3'], [40, 'b2b3'], [70, 'a2a3']]},  # flips, then reverts
        AFTER_E4: {'moves': [[0, 'e7e6']]},                                 # never flips
    }
    spec = {'option': 'Contempt',
            'options': {'Contempt': {'type': 'spin', 'default': 0, 'min': 0, 'max': 100}},
            'positions': positions}
    corpus = [{'id': 'monotone', 'fen': START, 'expect': None},
              {'id': 'nonmonotone', 'fen': AFTER_E4_E5, 'expect': None},
              {'id': 'flat', 'fen': AFTER_E4, 'expect': None}]

    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / 'stub.json'
        engine = Engine(_stub_exe(tmp, spec, log))
        try:
            rows = {entry['id']: analyse_position(engine, entry, 'Contempt', 0, 100, 4)
                    for entry in corpus}
        finally:
            engine.close()
        trace = json.loads(log.read_text(encoding='utf-8'))

    check('monotone flip', rows['monotone']['status'] == FLIP
          and rows['monotone']['threshold'] == 40 and rows['monotone']['flip_move'] == 'b2b3',
          f'{rows["monotone"]["status"]} at {rows["monotone"]["threshold"]}')
    check('non-monotone flip is found and named',
          rows['nonmonotone']['status'] == FLIP_NONMONOTONE
          and rows['nonmonotone']['threshold'] == 40,
          f'{rows["nonmonotone"]["status"]} at {rows["nonmonotone"]["threshold"]}')
    check('unreachable', rows['flat']['status'] == UNREACHABLE
          and rows['flat']['threshold'] is None, f'{rows["flat"]["status"]}')
    # The revert is the whole reason the quartiles are probed: anchored only at the maximum,
    # the middle position reads as unreachable.
    check('a reverting position would be missed by the maximum alone',
          rows['nonmonotone']['baseline'] == 'a2a3', f'{rows["nonmonotone"]["baseline"]}')
    check('table cleared before every search',
          trace['newgames'] == len(trace['records'])
          and all(r['newgames'] == i + 1 for i, r in enumerate(trace['records'])),
          f'{trace["newgames"]} ucinewgame for {len(trace["records"])} search(es)')

    # Mate exclusion takes precedence over a mismatching `expect`: neither says anything about
    # a margin, and counting the position twice would inflate both buckets.
    edge_positions = {
        START: {'moves': [[0, 'a2a3'], [1, 'b2b3']]},           # flips at the minimum non-zero
        AFTER_E4_E5: {'moves': [[0, 'a2a3'], [100, 'b2b3']]},   # flips at exactly the maximum
        AFTER_E4: {'moves': [[0, 'e7e6']], 'mate': True},
    }
    spec['positions'] = edge_positions
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / 'stub.json'
        engine = Engine(_stub_exe(tmp, spec, log))
        try:
            edges = {
                'min': analyse_position(engine, {'id': 'min', 'fen': START, 'expect': None},
                                        'Contempt', 0, 100, 4),
                'max': analyse_position(engine, {'id': 'max', 'fen': AFTER_E4_E5, 'expect': None},
                                        'Contempt', 0, 100, 4),
                'mate': analyse_position(engine, {'id': 'mate', 'fen': AFTER_E4, 'expect': 'h7h6'},
                                         'Contempt', 0, 100, 4),
                'mismatch': analyse_position(engine, {'id': 'x', 'fen': START, 'expect': 'h2h3'},
                                             'Contempt', 0, 100, 4),
            }
        finally:
            engine.close()

    check('flip at the minimum non-zero value', edges['min']['status'] == FLIP
          and edges['min']['threshold'] == 1, f'{edges["min"]["status"]} at {edges["min"]["threshold"]}')
    check('flip at exactly the maximum', edges['max']['status'] == FLIP
          and edges['max']['threshold'] == 100, f'{edges["max"]["status"]} at {edges["max"]["threshold"]}')
    # The mate fixture answers below the requested depth, as the engine does: a mate ends the
    # search where it is found, and reading that as an aborted transcript rejects the position
    # instead of excluding it.
    check('mate excluded before expect is consulted', edges['mate']['status'] == EXCLUDED_MATE,
          f'{edges["mate"]["status"]}')
    check('baseline mismatch', edges['mismatch']['status'] == BASELINE_MISMATCH
          and edges['mismatch']['baseline'] == 'a2a3', f'{edges["mismatch"]["status"]}')

    # The two silent-garbage modes must raise, not produce a row.
    spec['positions'] = {START: {'moves': [[0, 'a2a3']], 'aborted': True}}
    with tempfile.TemporaryDirectory() as tmp:
        engine = Engine(_stub_exe(tmp, spec, Path(tmp) / 'stub.json'))
        try:
            analyse_position(engine, {'id': 'a', 'fen': START, 'expect': None},
                             'Contempt', 0, 100, 4)
            check('aborted transcript rejected', False, 'a row was returned')
        except EngineError as exc:
            check('aborted transcript rejected', 'aborted search' in str(exc), f'{exc}')
        finally:
            engine.close()

    spec['positions'] = {START: {'moves': [[0, 'a2a3']]}}
    spec['silent_options'] = ['Contempt']
    with tempfile.TemporaryDirectory() as tmp:
        engine = Engine(_stub_exe(tmp, spec, Path(tmp) / 'stub.json'))
        try:
            analyse_position(engine, {'id': 'a', 'fen': START, 'expect': None},
                             'Contempt', 0, 100, 4)
            check('unacknowledged setoption rejected', False, 'a row was returned')
        except EngineError as exc:
            check('unacknowledged setoption rejected', 'did not acknowledge' in str(exc), f'{exc}')
        finally:
            engine.close()
    del spec['silent_options']

    # The corpus reader accepts what build_corpus.py writes as well as the annotated form.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'corpus.txt'
        path.write_text(f'{START}\n\n{json.dumps({"id": 7, "fen": AFTER_E4, "expect": "E7E5"})}\n',
                        encoding='utf-8')
        loaded = load_corpus(path)
    check('bare FEN lines load', len(loaded) == 2 and loaded[0]['fen'] == START
          and loaded[0]['id'] == 1, f'{len(loaded)} entr(ies)')
    check('JSON lines keep id and normalise expect',
          loaded[1]['id'] == 7 and loaded[1]['expect'] == 'e7e5', f'{loaded[1]}')

    summary = summarise([
        {'status': FLIP, 'threshold': 10, 'searches': 8},
        {'status': FLIP_NONMONOTONE, 'threshold': 60, 'searches': 9},
        {'status': UNREACHABLE, 'threshold': None, 'searches': 6},
        {'status': EXCLUDED_MATE, 'threshold': None, 'searches': 1},
    ], 0, 100)
    check('summary counts both flip kinds as flips', summary['thresholds'] == [10, 60],
          f'{summary["thresholds"]}')
    check('cumulative curve is monotone and ends at every flip',
          summary['curve'][0] == (10, 1) and summary['curve'][-1] == (100, 2),
          f'{summary["curve"][0]} .. {summary["curve"][-1]}')
    check('percentiles bracket the sample', percentile([10, 60], 0.0) == 10
          and percentile([10, 60], 1.0) == 60, 'min 10, max 60')

    check('self-check accepts a legal corpus and rejects an illegal one',
          self_check([{'id': 1, 'fen': START, 'expect': 'e2e4'}], out=open(os.devnull, 'w'))
          and not self_check([{'id': 1, 'fen': START, 'expect': 'e2e5'}], out=open(os.devnull, 'w')),
          'a legal FEN with an illegal expect must fail')
    return not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('corpus', nargs='?', help='one JSON object or bare FEN per line')
    parser.add_argument('--engine', help='path to StratChessEvolved.exe')
    parser.add_argument('--option', help='the UCI spin option to bisect')
    parser.add_argument('--range', nargs=2, type=int, metavar=('LO', 'HI'),
                        help="defaults to the option's own advertised min and max")
    parser.add_argument('--depth', type=int, default=12)
    parser.add_argument('--workers', type=int, default=min(os.cpu_count() or 4, 8))
    parser.add_argument('--out', help='write one JSON result object per position here')
    parser.add_argument('--self-check', action='store_true',
                        help='verify the corpus invariants and exit non-zero if any fails')
    parser.add_argument('--self-test', action='store_true',
                        help='run the built-in fixtures (no engine needed) and exit')
    args = parser.parse_args()

    if args.self_test:
        print('self-test')
        return 0 if self_test() else 1
    if not args.corpus:
        parser.error('corpus is required unless --self-test is given')

    try:
        corpus = load_corpus(args.corpus)
    except (OSError, ValueError) as exc:
        print(f'{exc}', file=sys.stderr)
        return 2
    if not corpus:
        print(f'no positions in {args.corpus}', file=sys.stderr)
        return 2

    if args.self_check:
        print('self-check')
        return 0 if self_check(corpus) else 1
    if not args.engine or not args.option:
        parser.error('--engine and --option are required unless --self-check or --self-test is given')
    if args.depth < 1 or args.workers < 1:
        parser.error('--depth and --workers must be positive')

    # Windows CreateProcess reads a leading `build/...` as switches rather than as a path, so a
    # relative engine path fails to launch however valid it looks from the shell.
    engine_path = Path(args.engine).resolve()
    if not engine_path.is_file():
        parser.error(f'no engine at {engine_path}')
    exe = str(engine_path)

    if args.range:
        lo, hi = args.range
    else:
        probe = Engine([exe, 'uci'])
        try:
            kind, lo, hi = probe.options.get(args.option, (None, None, None))
            if kind != 'spin' or lo is None:
                parser.error(f'{args.option} is not a spin option this engine advertises; '
                             'pass --range explicitly if it is')
        finally:
            probe.close()
    if lo >= hi:
        parser.error(f'--range {lo} {hi} is empty')

    def progress(done, total):
        print(f'\r{done}/{total} position(s)', end='', file=sys.stderr, flush=True)

    try:
        rows = run(corpus, exe, args.option, lo, hi, args.depth, args.workers, progress)
    except (EngineError, OSError) as exc:
        print(f'\nFAIL: {exc}', file=sys.stderr)
        return 1
    print(file=sys.stderr)

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as handle:
            for row in rows:
                handle.write(json.dumps(row) + '\n')
    report(summarise(rows, lo, hi), args.option, lo, hi, args.depth)
    return 0


if __name__ == '__main__':
    sys.exit(main())
