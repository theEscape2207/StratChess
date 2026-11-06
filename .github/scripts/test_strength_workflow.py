"""Exercise the strength workflows' shell and artifact boundaries without a match.

strength.yml calls strength-wave.yml once per wave; both are read, and their named
Bash blocks share one namespace.

The small extractor recognizes the workflow's named Bash run blocks, not general
YAML. The preflight CLI is covered by its own fixtures; its stub here captures
output placement while real staging/copy commands exercise artifact layout.
Directory uploads preserve paths relative to that directory. Complete and short
single/multi-arm batches exercise the actual verify, pool and report commands.
"""
from pathlib import Path
import argparse
import fnmatch
import json
import os
import shlex
import re
import shutil
import subprocess
import sys
import tempfile
import time

repo = Path(__file__).resolve().parents[2]
workflow = (repo / '.github/workflows/strength.yml').read_text(encoding='utf-8')
wave_workflow = (repo / '.github/workflows/strength-wave.yml').read_text(encoding='utf-8')
sys.path.insert(0, str(repo / '.github/scripts'))
import sprt  # noqa: E402
bash = 'C:/Program Files/Git/bin/bash.exe' if os.name == 'nt' else shutil.which('bash')

def blocks(text):
    result = {}
    sections = re.split(r'(?m)^      - name: ', text)[1:]
    for section in sections:
        name = section.splitlines()[0]
        body = re.search(r'(?m)^        run: \|\n((?:          .*\n|\n)+)', section)
        if body:
            result[name] = '\n'.join(line[10:] if line else '' for line in body[1].splitlines()) + '\n'
    return result

shell_blocks = blocks(workflow)
_wave_blocks = blocks(wave_workflow)
assert not set(shell_blocks) & set(_wave_blocks), set(shell_blocks) & set(_wave_blocks)
shell_blocks.update(_wave_blocks)
python_path = Path(sys.executable).as_posix()
python_bash = '/c/' + python_path[3:] if os.name == 'nt' else python_path
python_command = shlex.quote(python_bash)
wrapper = f'''python3() {{
  {python_command} -c 'import os,runpy,sys; sys.stdout.reconfigure(newline="\\n"); sys.argv=sys.argv[1:]; sys.path.insert(0,os.path.dirname(sys.argv[0])); runpy.run_path(sys.argv[0],run_name="__main__")' "$@"
}}
'''

def run(root, name, env, expected=0, command_wrapper=None):
    script = shell_blocks[name]
    script = re.sub(r'\$\{\{.*?\}\}', 'fixture', script)
    script = script.replace('/tmp/', (root / 'outputs').as_posix() + '/')
    path = root / 'step.sh'
    path.write_text((command_wrapper or wrapper) + script, encoding='utf-8', newline='\n')
    result = subprocess.run([bash, path.as_posix()], cwd=root, env={**os.environ, **env},
                            text=True, capture_output=True, timeout=30)
    if (result.returncode == 0) != (expected == 0):
        raise AssertionError(f'{name}: expected {expected}, got {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result

def outputs(path):
    """A GITHUB_OUTPUT file as a dict; the last write of a key wins, as in Actions."""
    return dict(line.split('=', 1) for line in path.read_text(encoding='utf-8').splitlines() if '=' in line)

def discard_rule():
    """decide's actual if: text against every outcome, since Actions expressions don't run locally."""
    expression = re.search(r"name: Refuse a partial wave\n\s+id: partial\n\s+if: \$\{\{ (.+) \}\}", wave_workflow)[1]
    python = expression.replace('!(', 'not (').replace('||', ' or ').replace('&&', ' and ')
    names = {'needs.match.result': 'match', 'github.run_attempt': 'attempt', 'needs.repair-plan.result': 'plan',
             'needs.repair-plan.outputs.status': 'status', 'needs.repair.result': 'repair'}
    for name, variable in names.items():
        python = python.replace(name, variable)
    table = [('success', '1', 'skipped', '', 'skipped', False),
             ('success', '2', 'skipped', '', 'skipped', False),
             ('failure', '1', 'success', 'repair', 'success', False),
             ('failure', '1', 'success', 'repair', 'failure', True),
             ('failure', '1', 'success', 'repair', 'cancelled', True),
             ('failure', '1', 'success', 'discard', 'skipped', True),
             ('failure', '1', 'failure', '', 'skipped', True),
             ('failure', '2', 'skipped', '', 'skipped', True),
             ('failure', '2', 'success', 'repair', 'success', True),
             ('cancelled', '1', 'skipped', '', 'skipped', True),
             ('skipped', '1', 'skipped', '', 'skipped', True)]
    for match, attempt, plan, status, repair, refused in table:
        scope = dict(match=match, attempt=attempt, plan=plan, status=status, repair=repair)
        assert eval(python, {}, scope) == refused, (scope, python)
    # Without a status function GitHub adds success(), which is false once a shard has failed.
    assert "if: ${{ !cancelled() && needs.match.result == 'failure' && github.run_attempt == '1' }}" in wave_workflow
    assert "if: ${{ !cancelled() && needs.repair-plan.result == 'success' && needs.repair-plan.outputs.status == 'repair' }}" in wave_workflow
    assert 'needs: [match, repair-plan, repair]\n    if: always()' in wave_workflow, 'decide must wait for the repair'
    # Every step after the refusal runs only when it was skipped; a step without the guard would pool a partial wave.
    decide = wave_workflow.split('\n  decide:\n')[1]
    steps = re.split(r'(?m)^      - ', decide.split('    steps:\n', 1)[1])[2:]
    guards = [re.search(r'(?m)^        if: (.+)$', step) for step in steps]
    assert len(steps) == 10 and all(guard and re.search(r'steps\.(partial|decide)\.outcome', guard[1])
                                    for guard in guards), steps
    assert 'needs: [setup, build, wave-1]' in workflow, 'aggregate must wait for the wave'
    assert "if: ${{ needs.wave-1.outputs.status != 'complete' }}" in workflow, 'aggregate must refuse an undecided wave'
    print(f'PASS: the discard rule refuses as designed in {len(table)} outcomes')

def artifact_names():
    """The workflows' artifact names and download patterns against sprt.py's, at one wave and at four."""
    def expand(template, waves, wave=2, shard=7, index=3):
        values = {'github.run_id': '42', 'matrix.slice.shard': str(shard), 'matrix.slice.index': str(index),
                  'needs.repair-plan.outputs.lost_a': str(shard), 'needs.repair-plan.outputs.lost_b': str(shard),
                  "inputs.waves > 1 && format('wave-{0}-', inputs.wave) || ''": f'wave-{wave}-' if waves > 1 else ''}
        text = re.sub(r'\$\{\{ (.+?) \}\}', lambda m: values[m[1]], template)
        assert '${{' not in text, text
        return text

    def field(step, key):
        section = wave_workflow.split(f'      - name: {step}\n')[1].split('      - name:')[0]
        return re.search(rf'(?m)^          {key}: (.+)$', section)[1]

    for waves in (1, 4):
        slice_name, replay_name = sprt.slice_artifact(42, 2, waves, 7), sprt.replay_artifact(42, 2, waves, 7, 3)
        assert expand(field('Upload shard PGN and log', 'name'), waves) == slice_name
        assert expand(field('Upload slice PGN and log', 'name'), waves) == replay_name
        for step in ('Upload the first replayed shard', 'Upload the second replayed shard'):
            assert expand(field(step, 'name'), waves) == slice_name
            assert expand(field(step, 'path'), waves) == f'shards/{slice_name}'
        slices, replays = field("Download the wave's slices", 'pattern'), field('Download the replayed slices', 'pattern')
        assert fnmatch.fnmatchcase(slice_name, expand(slices, waves))
        assert fnmatch.fnmatchcase(replay_name, expand(replays, waves))
        for other in (sprt.slice_artifact(42, 1, 4, 7), sprt.replay_artifact(42, 1, 4, 7, 3)):
            assert not fnmatch.fnmatchcase(other, expand(slices, waves)) and not fnmatch.fnmatchcase(other, expand(replays, waves))
    assert 'pattern: strength-${{ github.run_id }}-shard-*' in workflow, 'aggregate reads the canonical shard names'
    assert '--job-prefix "wave-$WAVE / "' in shell_blocks['Plan the repair'] and 'wave-1:\n' in workflow, \
        'the planner reads the jobs of the caller job wave-<k>'
    print('PASS: artifact names and download patterns agree with sprt.py at W = 1 and W = 4')

def step_env(name):
    section = wave_workflow.split(f'      - name: {name}')[1]
    block = section.split('        env:\n')[1].split('        run:')[0]
    return dict(line.strip().split(': ', 1) for line in block.splitlines() if ': ' in line and not line.strip().startswith('#'))

def replay_settings():
    """The one place a replay could still drift from its shard: the settings each job hands the script."""
    shard, replay = step_env('Run shard'), step_env('Replay shard')
    own = {'SHARD', 'START', 'ROUNDS', 'ARM_LABEL', 'ARM_OPTIONS'}
    assert set(shard) == set(replay), (shard, replay)
    assert {k: v for k, v in shard.items() if k not in own} == {k: v for k, v in replay.items() if k not in own}
    print('PASS: a replay gets the same settings as its shard')

def play_script(root):
    """The staged script's fastchess arguments, for a shard, an arm and a replay, and its failure exits."""
    assert 'cp .github/scripts/play_shard.sh "$stage/play_shard.sh"' in shell_blocks['Stage the toolkit']
    temp = root / 'play'
    tools = temp / 'tools'
    tools.mkdir(parents=True)
    shutil.copy(repo / '.github/scripts/play_shard.sh', tools)
    for name in ['candidate', 'reference', 'book.epd']:
        (tools / name).write_text('')
    (tools / 'cpu_sampler.sh').write_text('exit 0\n')
    (tools / 'fastchess').write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@" > "$RUNNER_TEMP/args.txt"\n'
                                     'printf "%s\\n" "${FIXTURE_LINE:-}"\nexit "${FIXTURE_EXIT:-0}"\n', newline='\n')
    output = temp / 'github-output'
    base = dict(RUNNER_TEMP=temp.as_posix(), GITHUB_OUTPUT=output.as_posix(), SHARD='3', START='1117', ROUNDS='558',
                CANDIDATE_TC='10+0.1', REFERENCE_TC='5+0.1', MATCH_CONCURRENCY='3', THREADS='1',
                CANDIDATE_UCI_OPTIONS='Contempt=20', REFERENCE_UCI_OPTIONS='Hash=64', ARM_LABEL='', ARM_OPTIONS='',
                CAND_NAME='candidate-x', REF_NAME='reference-y', BOOK_FILE='book.epd')

    def play(expected=0, **overrides):
        shutil.rmtree(temp / 'match', ignore_errors=True)
        result = subprocess.run([bash, (tools / 'play_shard.sh').as_posix()], cwd=root, text=True, capture_output=True,
                                env={**os.environ, **base, **overrides}, timeout=30)
        assert (result.returncode == 0) == (expected == 0), (overrides, result.stdout, result.stderr)
        return (temp / 'args.txt').read_text().splitlines()

    def arguments(candidate, options, rounds, start):
        return ['-engine', f'cmd={tools.as_posix()}/candidate', f'name={candidate}', 'dir=cand', 'args=uci',
                'option.Threads=1', *options, 'tc=10+0.1',
                '-engine', f'cmd={tools.as_posix()}/reference', 'name=reference-y', 'dir=ref', 'args=uci',
                'option.Threads=1', 'option.Hash=64', 'tc=5+0.1',
                '-each', 'proto=uci', '-rounds', rounds, '-repeat', '-concurrency', '3', '-recover',
                '-openings', f'file={tools.as_posix()}/book.epd', 'format=epd', 'order=sequential', f'start={start}',
                '-draw', 'movenumber=40', 'movecount=8', 'score=10', '-resign', 'movecount=4', 'score=800',
                '-pgnout', 'file=match.pgn', 'notation=san']

    shard = play()
    assert shard == arguments('candidate-x', ['option.Contempt=20'], '558', '1117'), shard
    assert (temp / 'match/match.log').is_file(), 'the log must land where the upload reads it'
    arm = play(ARM_LABEL='B', ARM_OPTIONS='Contempt=30 Foo=1')
    assert arm == arguments('candidate-x-armB', ['option.Contempt=30', 'option.Foo=1'], '558', '1117'), arm
    replay = play(SHARD='3.1', START='1148', ROUNDS='31')
    assert replay == arguments('candidate-x', ['option.Contempt=20'], '31', '1148'), replay
    differing = [pair for pair in zip(shard, replay) if pair[0] != pair[1]]
    assert differing == [('558', '31'), ('start=1117', 'start=1148')], differing
    play(expected=1, FIXTURE_LINE='Finished game 1: Engine x loses on time')
    play(expected=1, FIXTURE_EXIT='3')
    print('PASS: the play script passes full arguments, a replay differs only in start and rounds, failures exit nonzero')

def repaired_batch(root, env):
    """A lost shard replayed as two slices pools as the designed batch; a failed repair reports no figure."""
    summary = root / 'summary.md'
    candidate, reference = env['CAND_NAME'], env['REF_NAME']
    shutil.rmtree(root / 'shards/strength-1-shard-1')
    survivor = root / 'shards/strength-1-shard-0/match.pgn'
    survivor.write_text(survivor.read_text().replace(f'{candidate}-armA', candidate))
    # Offset 1, 2 pairs a shard: shard 1 holds openings 4 and 5 (1-based).
    book = (root / 'evidence/book.epd').read_text().splitlines()
    slices = [dict(shard=1, index=0, start=4, pairs=1), dict(shard=1, index=1, start=5, pairs=1)]
    for piece, position in zip(slices, book[3:5]):
        target = root / f"replays/strength-1-replay-1-{piece['index']}"
        target.mkdir(parents=True)
        pgn = ''
        for white, black in [(candidate, reference), (reference, candidate)]:
            pgn += (f'[Event "fixture"]\n[Round "1"]\n[White "{white}"]\n[Black "{black}"]\n'
                    f'[FEN "{position} 0 9"]\n[Result "1/2-1/2"]\n\n1/2-1/2\n\n')
        (target / 'match.pgn').write_text(pgn)
        (target / 'match.log').write_text('Ptnml(0-2): [0, 0, 1, 0, 0]\n')
    output = root / 'github-output'
    output.unlink(missing_ok=True)
    repair = {**env, 'CANDIDATE_ARMS': '', 'PIECES': json.dumps(slices), 'GITHUB_RUN_ID': '1', 'POOL_RESULT': 'success',
              'PRIOR_LOST': '0', 'LOST_A': '1', 'LOST_B': '', 'REPLAYED': 'shard 1, 2 slices (runner lost)'}
    run(root, 'Assemble the replayed shards', repair)
    run(root, 'Verify the wave', repair)
    run(root, 'Pool the wave', repair)
    run(root, 'Record the wave', repair)
    wave = outputs(output)
    assert wave['decision'] == 'fixed' and wave['counts'] == '[0,0,4,0,0]', wave
    assert wave['status'] == 'complete' and wave['lost_total'] == '1' and wave['lost_a'] == '1' and wave['lost_b'] == ''
    assert wave['replayed'] == 'shard 1, 2 slices (runner lost)', 'a one-wave run keeps the Replayed text'
    assert json.loads(wave['slices']) == slices
    run(root, 'Record the repair plan', {**repair, 'SLICES': wave['slices']})
    assert json.loads((root / 'repair-plan.json').read_text()) == slices, 'the marker carries the repair plan as today'
    run(root, 'Verify the complete batch', repair)
    run(root, 'Pool the result', repair)
    run(root, 'Report', repair)
    report = (root / 'outputs/report.md').read_text(encoding='utf-8')
    assert '| Replayed | shard 1, 2 slices (runner lost) |' in report and '**Pooled:' in report
    assert 'on 1 of 2 shards' in report, 'an assembled shard carries no CPU samples'
    assert 'by score: 0 of 2 (0 the reference, 2 even)' in report, 'the replay counts as one shard'
    print('PASS: a repaired batch assembles, verifies, pools at the designed size and says it was repaired')
    (root / 'outputs/pooled.md').unlink()
    # A slice whose log disagrees with its PGN must stop before anything pools.
    shutil.rmtree(root / 'shards/strength-1-shard-1')
    (root / 'replays/strength-1-replay-1-1/match.log').write_text('Ptnml(0-2): [0, 0, 2, 0, 0]\n')
    run(root, 'Assemble the replayed shards', repair, expected=1)

    def refused(match, plan_reason, repair_result):
        """decide's refusal, then aggregate's summary of it."""
        output.unlink(missing_ok=True)
        summary.unlink(missing_ok=True)
        run(root, 'Refuse a partial wave', {**repair, 'MATCH_RESULT': match, 'PLAN_REASON': plan_reason,
                                            'REPAIR_RESULT': repair_result})
        wave = outputs(output)
        assert wave['status'] == 'discarded' and 'decision' not in wave, wave
        run(root, 'Refuse a partial batch', {**repair, 'WAVE_STATUS': wave['status'], 'WAVE_REASON': wave['reason'],
                                             'WAVE_RESULT': 'failure'}, expected=1)
        text = summary.read_text(encoding='utf-8')
        assert 'DISCARDED' in text and f'Shard jobs reported `{match}`.' in text and '**Pooled:' not in text, text
        return text

    assert 'Not repaired: shard 2 failed.' in refused('failure', 'shard 2 failed', 'skipped')
    assert 'The repair was failure.' in refused('failure', '', 'failure')
    summary.unlink()
    run(root, 'Refuse a partial batch', {**repair, 'WAVE_STATUS': '', 'WAVE_REASON': '', 'WAVE_RESULT': 'failure'},
        expected=1)
    text = summary.read_text(encoding='utf-8')
    assert 'DISCARDED' in text and 'ended `failure` without a decision' in text, 'a wave without outputs is a discard'
    run(root, 'Report', {**repair, 'POOL_RESULT': 'failure'})
    report = (root / 'outputs/report.md').read_text(encoding='utf-8')
    assert 'DISCARDED' in report and 'Replayed' not in report and '**Pooled:' not in report
    print('PASS: a failed slice, repair or decide reports DISCARDED with the reason and no figure')

def self_test():
    (repo / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=repo / 'build') as directory:
        root = Path(directory)
        for index, (name, script) in enumerate(shell_blocks.items()):
            path = root / f'syntax-{index}.sh'
            path.write_text(re.sub(r'\$\{\{.*?\}\}', 'fixture', script), encoding='utf-8', newline='\n')
            result = subprocess.run([bash, '-n', path.as_posix()], text=True, capture_output=True)
            assert result.returncode == 0, (name, result.stderr)
        print(f'PASS: Bash syntax for {len(shell_blocks)} strength workflow blocks')
        # A broken sampler fails nothing in a real run; it just leaves the evidence empty.
        samples = root / 'cpu.tsv'
        # Windows kill leaves the sampler's children writing into the removed directory; discard that noise.
        sampler = subprocess.Popen([bash, (repo / '.github/scripts/cpu_sampler.sh').as_posix(), '1', samples.as_posix()],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and len(samples.read_text().splitlines() if samples.exists() else []) < 2:
                time.sleep(0.2)
        finally:
            sampler.kill()
            sampler.wait()
        rows = samples.read_text().splitlines()
        assert len(rows) >= 2 and re.fullmatch(r'\d+\t[\d.]+\t[\d.]+\t\d+(,\d+)*\t[\d.]+', rows[1]), rows
        print('PASS: the CPU sampler writes well-formed rows')
        (root / '.github').mkdir()
        shutil.copytree(repo / '.github/scripts', root / '.github/scripts')
        (root / 'outputs').mkdir()
        discard_rule()
        artifact_names()
        replay_settings()
        play_script(root)
        stage = root / 'runner/stage'
        stage.mkdir(parents=True)
        positions = [f'8/8/8/8/8/8/4K3/4k3 {colour} - -' for colour in ['w', 'b']]
        (stage / 'book.epd').write_text('\n'.join(positions[i] for i in [1, 0, 1, 1, 0]) + '\n')
        summary = root / 'summary.md'
        env = {'SHARDS': '2', 'ROUNDS': '2', 'OPENING_OFFSET': '1', 'CAND_NAME': 'candidate-x',
               'REF_NAME': 'reference-y', 'BOOK_FILE': 'book.epd', 'GITHUB_STEP_SUMMARY': summary.as_posix(),
               'CMAKE_DEFINES': '', 'CMAKE_DEFINES_VALIDATED': 'true', 'ACTUAL_GAMES': '8',
               'BUILD_RESULT': 'success', 'CANDIDATE_UCI_OPTIONS': '', 'REFERENCE_UCI_OPTIONS': ''}
        env.update(RUNNER_TEMP=(root / 'runner').as_posix(), CALIBRATION='false',
                   REFERENCE_SHA='reference-fixture', GITHUB_SHA='candidate-fixture',
                   CANDIDATE_ARMS='', CANDIDATE_TC='10+0.1', REFERENCE_TC='10+0.1', THREADS='4')
        output = root / 'github-output'
        env['GITHUB_OUTPUT'] = output.as_posix()
        run(root, 'Plan the waves', env)
        planned = outputs(output)
        assert planned['waves'] == '1' and json.loads(planned['wave_plan']) == sprt.wave_plan(2, 2, 1, 1), planned
        env.update(WAVE='1', WAVES='1', WAVE_PLAN=planned['wave_plan'], SLICES=json.dumps(json.loads(planned['wave_plan'])[0]))
        print('PASS: setup plans a fixed run as one wave of whole shards')
        budget = {'RUNNER_VCPUS': '4'}
        for threads, concurrency, expected in [('1', '3', 0), ('4', '1', 0), ('2', '2', 0),
                                               ('4', '3', 1), ('1', '5', 1), ('0', '1', 1), ('04', '1', 1)]:
            run(root, 'Check the per-shard CPU budget',
                dict(budget, THREADS=threads, MATCH_CONCURRENCY=concurrency), expected=expected)
        print('PASS: threads x concurrency is refused past the runner vCPU budget')
        # Module self-tests exercise the real CLI. Here the stub captures shell
        # forwarding/output placement while the actual preparation/copy commands run.
        stub = '''python3() {
          printf '%s\\n' "$@" > "$RUNNER_TEMP/preflight-args.txt"
          while [ "$#" -gt 0 ]; do
            if [ "$1" = "--output" ]; then
              shift
              printf 'Retained intended fixture comparison\\n' > "$1"
              return 0
            fi
            shift
          done
          return 1
        }
    '''
        run(root, 'Verify the resolved comparison', env, command_wrapper=stub)
        preflight_args = (root / 'runner/preflight-args.txt').read_text().splitlines()
        assert preflight_args[preflight_args.index('--threads') + 1] == '4', 'threads input not forwarded to preflight'
        run(root, 'Retain the intended comparison in the build summary', env)
        assert 'Retained intended fixture comparison' in summary.read_text()
        upload_step = workflow.split('      - name: Upload comparison and opening evidence\n')[1].split('      - name:')[0]
        upload_path = re.search(r'(?m)^          path: (.+)$', upload_step)[1]
        upload_path = upload_path.replace('${{ runner.temp }}', env['RUNNER_TEMP'])
        upload_root = Path(upload_path)
        assert upload_root.is_dir(), 'expected an evidence directory upload'
        shutil.copytree(upload_root, root / 'evidence')
        assert (root / 'evidence/book.epd').read_bytes() == (stage / 'book.epd').read_bytes()
        assert (root / 'evidence/comparison.md').is_file()
        print('PASS: actual preflight staging and directory artifact relative paths agree with aggregate readers')
        (root / 'shards').mkdir()
        for arms in ['', 'X=1;X=2']:
            env['CANDIDATE_ARMS'] = arms
            for shard in range(2):
                target = root / f'shards/strength-1-shard-{shard}'
                target.mkdir(exist_ok=True)
                candidate = 'candidate-x' + (f'-arm{"AB"[shard]}' if arms else '')
                pgn = ''
                for round_no, swap in [(2, False), (1, False), (1, True), (2, True)]:
                    white, black = ('reference-y', candidate) if swap else (candidate, 'reference-y')
                    fen = positions[shard] + ' 0 9'
                    pgn += f'[Event "fixture"]\n[Round "{round_no}"]\n[White "{white}"]\n[Black "{black}"]\n[FEN "{fen}"]\n[Result "1/2-1/2"]\n\n1/2-1/2\n\n'
                (target / 'match.pgn').write_text(pgn)
                (target / 'match.log').write_text('Ptnml(0-2): [0, 0, 2, 0, 0]\n')
                if shard == 0:
                    (target / 'cpu.tsv').write_text('elapsed_s\tbusy_pct\tsteal_pct\tper_cpu_busy_pct\tload_1m\n'
                                                    '60\t75.0\t0.5\t90,80,70,60\t3.10\n')
            output.unlink(missing_ok=True)
            run(root, 'Verify the wave', env)
            run(root, 'Pool the wave', env)
            decided = outputs(output)
            assert decided == {'decision': 'fixed', 'llr': '', 'counts': '[0,0,4,0,0]'}, decided
            run(root, 'Verify the complete batch', env)
            run(root, 'Pool the result', env)
            pooled = root / 'outputs/pooled.md'
            assert '**Pooled:' in pooled.read_text() and '0.00 Elo**' in pooled.read_text()
            assert 'Shards favouring the candidate by score:' in pooled.read_text()
            assert ('### Arms' in pooled.read_text()) == bool(arms), 'cross-arm summary missing or spurious'
            print(f'PASS: actual verify/pool workflow blocks, {"multi" if arms else "single"} arm')
            pooled.unlink()
            (root / 'shards/strength-1-shard-1/match.log').write_text('Ptnml(0-2): [0, 0, 1, 0, 0]\n')
            output.unlink(missing_ok=True)
            run(root, 'Verify the wave', env, expected=1)
            run(root, 'Pool the wave', env, expected=1)
            assert not output.exists(), 'a short wave was decided'
            run(root, 'Verify the complete batch', env, expected=1)
            run(root, 'Pool the result', env, expected=1)
            assert not pooled.exists(), 'partial pool was published'
            print('PASS: incomplete last shard refuses batch and never publishes a pool')
            env['POOL_RESULT'] = 'failure'
            run(root, 'Report', env)
            report = (root / 'outputs/report.md').read_text(encoding='utf-8')
            assert 'DISCARDED' in report and '**Pooled:' not in report and 'Threads=4 |' in report
            assert 'on 1 of 2 shards' in report and '| Busy, all vCPUs | 75.0% |' in report, 'CPU summary missing'
            print('PASS: failure summary contains no partial Elo')
        repaired_batch(root, env)
        (root / 'evidence/comparison.md').unlink()
        run(root, 'Verify the complete batch', env, expected=1)
        print('PASS: absent comparison evidence refuses aggregation')
    print('Workflow integration probes PASS')
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true', required=True)
    parser.parse_args()
    sys.exit(self_test())
