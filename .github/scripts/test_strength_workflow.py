"""Exercise the strength workflows' shell and artifact boundaries without a match.

strength.yml calls strength-wave.yml once per wave; both are read, and their named
Bash blocks share one namespace.

The small extractor recognizes the workflow's named Bash run blocks, not general
YAML. The preflight CLI is covered by its own fixtures; its stub here captures
output placement while real staging/copy commands exercise artifact layout.
Directory uploads preserve paths relative to that directory. Complete and short
single/multi-arm batches exercise the actual verify, pool and report commands, and a
four-wave run goes through every wave's decide and aggregate's join, pool and report.
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

def run_needs(plan, *waves):
    """strength.yml's needs as aggregate sees them: setup's plan, then each wave's (result, outputs)."""
    jobs = {'setup': {'result': 'success', 'outputs': {'waves': str(len(plan)), 'wave_plan': json.dumps(plan)}},
            'build': {'result': 'success', 'outputs': {}}}
    for k in range(1, len(plan) + 1):
        result, wave_outputs = waves[k - 1] if k <= len(waves) else ('skipped', {})
        jobs[f'wave-{k}'] = {'result': result, 'outputs': wave_outputs}
    return jobs

def summarize(root, env, needs):
    """aggregate's summary step, and the env its outputs give the steps after it."""
    output = root / 'github-output'
    output.unlink(missing_ok=True)
    run(root, 'Summarize the waves', {**env, 'NEEDS': json.dumps(needs), 'GITHUB_OUTPUT': output.as_posix()})
    summary = outputs(output)
    names = {'outcome': 'OUTCOME', 'wave': 'WAVE', 'reason': 'REASON', 'counts': 'COUNTS', 'pairs': 'PAIRS',
             'heading': 'HEADING', 'waved_games': 'WAVED_GAMES', 'replayed': 'REPLAYED', 'repair_plan': 'REPAIR_PLAN'}
    assert set(summary) == set(names), summary
    return {names[key]: value for key, value in summary.items()}

def record_wave(root, env, **values):
    """decide's last step after its pool, and the outputs the wave hands its caller."""
    decided = outputs(root / 'github-output')
    # Its own outputs only, as Actions keeps them per step: decide's job outputs read this step's.
    (root / 'github-output').unlink()
    run(root, 'Record the wave', {**env, 'DECISION': decided['decision'], 'LLR': decided['llr'],
                                  'COUNTS': decided['counts'], 'PRIOR_LOST': '0', 'LOST_A': '', 'LOST_B': '',
                                  'PIECES': '', 'REPLAYED': '', **values})
    return outputs(root / 'github-output')

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
    # A decision reaches the caller only from the last step, so never without status=complete.
    contract = decide.split('    outputs:\n')[1].split('    steps:\n')[0]
    for key in ('decision', 'llr', 'counts', 'lost_total', 'replayed', 'lost_a', 'lost_b', 'slices'):
        assert f'      {key}: ${{{{ steps.record.outputs.{key} }}}}\n' in contract, key
    assert 'needs: [setup, build, wave-1, wave-2, wave-3, wave-4]\n    if: always()' in workflow, 'aggregate must wait for every wave'
    assert "if: steps.summary.outputs.outcome == 'discarded'" in workflow, 'aggregate must refuse a discarded run'
    print(f'PASS: the discard rule refuses as designed in {len(table)} outcomes')

def wave_calls():
    """The four wave calls: D1's gate over every decision, and the inputs they must share."""
    calls = {}
    for k in range(1, 5):
        section = workflow.split(f'\n  wave-{k}:\n')[1].split('\n\n')[0]
        calls[k] = section
        assert 'uses: ./.github/workflows/strength-wave.yml' in section
        assert ('needs: [setup, build]' if k == 1 else f'needs: [setup, build, wave-{k - 1}]') in section
    for k in range(2, 5):
        gate = re.search(r'(?m)^    if: \$\{\{ (.+) \}\}$', calls[k])[1]
        assert gate == f"!cancelled() && needs.wave-{k - 1}.outputs.decision == 'continue'", gate
        python = gate.replace('!cancelled()', 'not cancelled').replace('&&', ' and ').replace(
            f'needs.wave-{k - 1}.outputs.decision', 'decision')
        for decision in ('continue', 'H1', 'H0', 'cap', 'fixed', ''):
            for cancelled in (False, True):
                assert eval(python, {}, dict(cancelled=cancelled, decision=decision)) == \
                    (decision == 'continue' and not cancelled), (gate, decision, cancelled)
    own = {'wave', 'slices', 'prior_lost', 'prior_counts'}

    def inputs(k):
        block = calls[k].split('    with:\n')[1]
        return dict(line.strip().split(': ', 1) for line in block.splitlines())

    for k in range(1, 5):
        given = inputs(k)
        assert given['wave'] == str(k) and given['slices'] == f'${{{{ toJSON(fromJSON(needs.setup.outputs.wave_plan)[{k - 1}]) }}}}'
        assert (given['prior_lost'], given['prior_counts']) == (('0', '""') if k == 1 else (
            f'${{{{ fromJSON(needs.wave-{k - 1}.outputs.lost_total) }}}}', f'${{{{ needs.wave-{k - 1}.outputs.counts }}}}'))
        assert {key: value for key, value in given.items() if key not in own} == \
            {key: value for key, value in inputs(1).items() if key not in own}, f'wave-{k} drifted from wave-1'
    print('PASS: each wave runs only after the one before decided to continue, with the same settings')

def publication_gates():
    """publish and mark's if: texts: joined shards only after the pool, the marker only after every shard."""
    def gate(job):
        section = workflow.split(f'\n  {job}:\n')[1]
        python = re.search(r'(?m)^    if: \$\{\{ (.+) \}\}$', section)[1]
        for name, variable in (('!cancelled()', 'not cancelled'), ('&&', ' and '), ('||', ' or '),
                               ('needs.aggregate.outputs.', ''), ('needs.publish.result', 'publish'),
                               ('needs.setup.outputs.waves', 'waves')):
            python = python.replace(name, variable)
        return python

    publish, mark = gate('publish'), gate('mark')
    for joined in ('true', 'false'):
        for cancelled in (False, True):
            assert eval(publish, {}, dict(joined=joined, cancelled=cancelled)) == (joined == 'true' and not cancelled)
    table = [  # pooled, repair_plan, waves, publish, marked
        ('success', '[1]', 1, 'skipped', True),
        ('success', '[1]', 4, 'success', True),
        ('success', '[1]', 4, 'failure', False),
        ('success', '[1]', 4, 'cancelled', False),
        # The pool succeeded but the joined upload failed, so publish never ran.
        ('success', '[1]', 4, 'skipped', False),
        ('success', '', 1, 'skipped', False),
        ('failure', '[1]', 1, 'skipped', False),
        ('skipped', '[1]', 4, 'success', False)]
    for pooled, repair_plan, waves, published, marked in table:
        scope = dict(pooled=pooled, repair_plan=repair_plan, waves=waves, publish=published, cancelled=False)
        assert eval(mark, {}, scope) == marked, (scope, mark)
    assert 'needs: [setup, aggregate, publish]' in workflow.split('\n  mark:\n')[1].split('\n\n')[0]
    print(f'PASS: joined shards publish after the pool, the marker after every shard, in {len(table)} outcomes')

def artifact_names():
    """The workflows' artifact names and download patterns against sprt.py's, at one wave and at four."""
    def expand(template, waves, wave=2, shard=7, index=3):
        values = {'github.run_id': '42', 'matrix.slice.shard': str(shard), 'matrix.slice.index': str(index),
                  'needs.repair-plan.outputs.lost_a': str(shard), 'needs.repair-plan.outputs.lost_b': str(shard),
                  'env.WAVE_PREFIX': f'wave-{wave}-' if waves > 1 else ''}
        text = re.sub(r'\$\{\{ (.+?) \}\}', lambda m: values[m[1]], template)
        assert '${{' not in text, text
        return text

    def field(step, key):
        section = wave_workflow.split(f'      - name: {step}\n')[1].split('      - name:')[0]
        return re.search(rf'(?m)^          {key}: (.+)$', section)[1]

    assert "  WAVE_PREFIX: ${{ inputs.waves > 1 && format('wave-{0}-', inputs.wave) || '' }}\n" in wave_workflow
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
    # aggregate downloads every wave's slices; a waved run's joined shards take the pre-wave names.
    assert "  SLICE_PREFIX: ${{ needs.setup.outputs.waves > 1 && 'wave-*-' || '' }}\n" in workflow
    assert 'pattern: strength-${{ github.run_id }}-${{ env.SLICE_PREFIX }}shard-*' in workflow
    for waves, prefix in ((1, ''), (4, 'wave-*-')):
        pattern = f'strength-42-{prefix}shard-*'
        assert all(fnmatch.fnmatchcase(sprt.slice_artifact(42, wave, waves, 7), pattern) for wave in range(1, waves + 1))
        assert not fnmatch.fnmatchcase(sprt.replay_artifact(42, 2, waves, 7, 3), pattern)
    assert not fnmatch.fnmatchcase(sprt.slice_artifact(42, 1, 1, 7), 'strength-42-wave-*-shard-*'), \
        'a waved run must not download its own joined shards as slices'
    publish = workflow.split('\n  publish:\n')[1]
    assert re.search(r'(?m)^          name: (.+)$', publish.split('- name: Upload the shard')[1])[1] == \
        'strength-${{ github.run_id }}-shard-${{ matrix.slice.shard }}', 'joined shards take the pre-wave names'
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
    wave = record_wave(root, repair, LOST_A='1', PIECES=json.dumps(slices), REPLAYED='shard 1, 2 slices (runner lost)')
    assert wave['decision'] == 'fixed' and wave['counts'] == '[0,0,4,0,0]', wave
    assert wave['status'] == 'complete' and wave['lost_total'] == '1' and wave['lost_a'] == '1' and wave['lost_b'] == ''
    assert wave['replayed'] == 'shard 1, 2 slices (runner lost)', 'a one-wave run keeps the Replayed text'
    assert json.loads(wave['slices']) == slices
    plan = json.loads(env['WAVE_PLAN'])
    repair.update(summarize(root, repair, run_needs(plan, ('failure', wave))))
    run(root, 'Record the repair plan', repair)
    assert (root / 'repair-plan.json').read_text() == json.dumps(slices) + '\n', 'the marker carries the repair plan as today'
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
        return refuse(('failure', wave))

    def refuse(wave):
        """aggregate's summary and refusal of a run whose one wave ended as given."""
        summary.unlink(missing_ok=True)
        run(root, 'Refuse a discarded run', {**repair, **summarize(root, repair, run_needs(plan, wave))}, expected=1)
        text = summary.read_text(encoding='utf-8')
        assert text.startswith('## Strength lab — DISCARDED\n') and '**Pooled:' not in text, text
        return text

    assert 'Shard jobs reported `failure`. Not repaired: shard 2 failed.' in refused('failure', 'shard 2 failed', 'skipped')
    assert 'The repair was failure.' in refused('failure', '', 'failure')
    assert 'ended `failure` without a decision' in refuse(('failure', {})), 'a wave without outputs is a discard'
    assert 'No shard ran: setup or build did not complete.' in refuse(('skipped', {}))
    run(root, 'Report', {**repair, 'POOL_RESULT': 'failure'})
    report = (root / 'outputs/report.md').read_text(encoding='utf-8')
    assert 'DISCARDED' in report and 'Replayed' not in report and '**Pooled:' not in report
    print('PASS: a failed slice, repair or decide reports DISCARDED with the reason and no figure')

def write_slice(target, start, pairs, book, ptnml, candidate='candidate-x', reference='reference-y'):
    """A slice as play_shard.sh leaves it: rounds 1..pairs from book entry `start` (1-based), and a log."""
    target.mkdir(parents=True)
    pgn = ''
    for round_no in range(1, pairs + 1):
        for white, black in [(candidate, reference), (reference, candidate)]:
            pgn += (f'[Event "fixture"]\n[Round "{round_no}"]\n[White "{white}"]\n[Black "{black}"]\n'
                    f'[FEN "{book[start + round_no - 2]} 0 9"]\n[Result "1/2-1/2"]\n\n1/2-1/2\n\n')
    (target / 'match.pgn').write_text(pgn)
    (target / 'match.log').write_text(f'Ptnml(0-2): {ptnml}\n')

def waved_batch(base, base_env):
    """A four-wave calibration run through every wave's decide and aggregate: a repaired wave, the cap,
    an early stop, and the refusals that must leave no figure although every shard job was green."""
    root = base / 'waved'
    (root / 'outputs').mkdir(parents=True)
    shutil.copytree(base / '.github', root / '.github')
    (root / 'evidence').mkdir()
    book = [f'8/8/8/8/8/8/{file or ""}K{7 - file or ""}/k7 {colour} - -' for colour in 'wb' for file in range(8)]
    (root / 'evidence/book.epd').write_text('\n'.join(book) + '\n')
    (root / 'evidence/comparison.md').write_text('Fixture comparison\n')
    summary, output = root / 'summary.md', root / 'github-output'
    env = {**base_env, 'GITHUB_OUTPUT': output.as_posix(), 'GITHUB_STEP_SUMMARY': summary.as_posix(), 'SHARDS': '2',
           'ROUNDS': '4', 'OPENING_OFFSET': '1', 'GAMES': '16', 'ACTUAL_GAMES': '16', 'SPRT': 'true',
           'CALIBRATION': 'true', 'CANDIDATE_ARMS': '', 'SLICE_ROOT': 'slices', 'WAVES': '4'}
    for name, overrides in (('without calibration', {'CALIBRATION': 'false'}), ('with arms', {'CANDIDATE_ARMS': 'X=1;X=2'})):
        output.unlink(missing_ok=True)
        run(root, 'Plan the waves', {**env, **overrides}, expected=1)
        assert not output.exists(), f'setup planned sprt {name}'
    run(root, 'Plan the waves', env)
    planned = outputs(output)
    plan = json.loads(planned['wave_plan'])
    assert planned['waves'] == '4' and plan == sprt.wave_plan(2, 4, 1, 4), planned
    env['WAVE_PLAN'] = planned['wave_plan']

    # One pair per slice; shard 0's wave-1 pair is a 2-0, every other pair is drawn.
    for k, wave in enumerate(plan, 1):
        for piece in wave:
            ptnml = '[0, 0, 0, 0, 1]' if (k, piece['shard']) == (1, 0) else '[0, 0, 1, 0, 0]'
            write_slice(root / f"slices/{sprt.slice_artifact(1, k, 4, piece['shard'])}", piece['start'], 1, book, ptnml)
    waves, prior = [], {}
    for k, wave in enumerate(plan, 1):
        shutil.rmtree(root / 'shards', ignore_errors=True)
        for piece in wave:
            name = sprt.slice_artifact(1, k, 4, piece['shard'])
            shutil.copytree(root / f'slices/{name}', root / f'shards/{name}')
        decide = {**env, 'WAVE': str(k), 'SLICES': json.dumps(wave), 'PRIOR_COUNTS': prior.get('counts', '')}
        repaired = {}
        if k == 2:
            # Wave 2 lost shard 1's runner; its pair is replayed as one piece under the wave's names.
            lost = sprt.slice_artifact(1, 2, 4, 1)
            pieces = [dict(shard=1, index=0, start=wave[1]['start'], pairs=1)]
            shutil.move(root / f'shards/{lost}', root / f'replays/{sprt.replay_artifact(1, 2, 4, 1, 0)}')
            decide['PIECES'] = json.dumps(pieces)
            run(root, 'Assemble the replayed shards', decide)
            assert (root / f'shards/{lost}/match.pgn').is_file(), 'the replayed slice takes its slice name'
            repaired = dict(LOST_A='1', PIECES=json.dumps(pieces), REPLAYED='shard 1, 1 slice (runner lost)')
        output.unlink(missing_ok=True)
        run(root, 'Verify the wave', decide)
        run(root, 'Pool the wave', decide)
        prior = record_wave(root, decide, PRIOR_LOST=prior.get('lost_total', '0'), **repaired)
        waves.append(('failure' if k == 2 else 'success', prior))
    assert [w['decision'] for _, w in waves] == ['continue', 'continue', 'continue', 'cap'], waves
    assert [w['counts'] for _, w in waves] == ['[0,0,1,0,1]', '[0,0,3,0,1]', '[0,0,5,0,1]', '[0,0,7,0,1]'], \
        'each wave decides on the waves before it as well'
    assert [w['lost_total'] for _, w in waves] == ['0', '1', '1', '1'], 'the lost-shard count runs across waves'
    assert waves[1][1]['replayed'] == 'wave 2: shard 1, 1 slice (runner lost)'
    print('PASS: four waves decide on their cumulative counts, a repaired wave included')

    def summarized(*played):
        shutil.rmtree(root / 'shards', ignore_errors=True)
        return {**env, **summarize(root, env, run_needs(plan, *played))}

    def aggregate(*played):
        """aggregate's chain for a run that decided; its report."""
        steps = summarized(*played)
        for step in ('Join the waves', 'Verify the complete batch', 'Pool the result'):
            run(root, step, steps)
        run(root, 'Report', {**steps, 'POOL_RESULT': 'success'})
        return steps, (root / 'outputs/report.md').read_text(encoding='utf-8')

    steps, report = aggregate(*waves)
    assert report.startswith('## Strength lab (calibration) — does not pass at the cap\n'), report
    assert '| Games | 16 of 16 planned = 2 shards × 4 of 4 waves, undecided at the cap |' in report
    assert '| Replayed | wave 2: shard 1, 1 slice (runner lost) |' in report and '**Pooled:' in report
    assert 'cap 16 games' in report and len(re.findall(r'(?m)^\| [1-4] \| ', report)) == 4, report
    joined = (root / 'shards/strength-1-shard-1/match.pgn').read_text()
    assert re.findall(r'\[Round "(\d+)"\]', joined) == ['1', '1', '2', '2', '3', '3', '4', '4'], 'rounds renumbered'
    run(root, 'Record the repair plan', steps)
    assert json.loads((root / 'repair-plan.json').read_text()) == [{'wave': 2, 'slices': json.loads(waves[1][1]['slices'])}]
    print('PASS: a run at the cap joins, verifies and pools all four waves, reports each and marks its repair')

    # A stop at wave k: the later waves never ran, so their slices do not exist.
    (root / 'later').mkdir()
    for k, decision, heading, pooled in ((1, 'H1', 'passes (SPRT H1, wave 1 of 4)', '[0, 0, 1, 0, 1]'),
                                         (2, 'H0', 'does not pass (SPRT H0, wave 2 of 4)', '[0, 0, 3, 0, 1]'),
                                         (3, 'H1', 'passes (SPRT H1, wave 3 of 4)', '[0, 0, 5, 0, 1]')):
        unplayed = [sprt.slice_artifact(1, j, 4, shard) for j in range(k + 1, 5) for shard in range(2)]
        for name in unplayed:
            shutil.move(root / f'slices/{name}', root / f'later/{name}')
        steps, report = aggregate(*waves[:k - 1], (waves[k - 1][0], {**waves[k - 1][1], 'decision': decision}))
        for name in unplayed:
            shutil.move(root / f'later/{name}', root / f'slices/{name}')
        assert report.startswith(f'## Strength lab (calibration) — {heading}\n'), report
        assert f'| Games | {4 * k} of 16 planned = 2 shards × {k} of 4 waves, stopped by SPRT {decision} |' in report
        assert len(re.findall(r'(?m)^\| [1-4] \| ', report)) == k and f'Pooled Ptnml(0-2): {pooled}' in report
        assert ('| Replayed | wave 2: shard 1, 1 slice (runner lost) |' in report) == (k >= 2), report
    print('PASS: stops at waves 1, 2 and 3 join, pool and report only the waves played')
    (root / 'later').rmdir()

    stopped = [waves[0], (waves[1][0], {**waves[1][1], 'decision': 'H1'})]
    later = [sprt.slice_artifact(1, k, 4, shard) for k in (3, 4) for shard in range(2)]
    steps = summarized(*stopped)
    run(root, 'Join the waves', steps, expected=1)
    assert not (root / 'shards').exists(), 'a refused join left shards behind'
    (root / 'later').mkdir()
    for name in later:
        shutil.move(root / f'slices/{name}', root / f'later/{name}')
    print('PASS: a slice of a wave after the decision refuses the join')

    # Every shard job green, and still no figure.
    steps = summarized(waves[0], (waves[1][0], {**stopped[1][1], 'counts': '[0,0,2,0,2]'}))
    run(root, 'Join the waves', steps)
    run(root, 'Verify the complete batch', steps)
    (root / 'outputs/pooled.md').unlink()
    run(root, 'Pool the result', steps, expected=1)
    assert not (root / 'outputs/pooled.md').exists(), 'a pool that is not the decided one was published'
    run(root, 'Report', {**steps, 'POOL_RESULT': 'failure'})
    report = (root / 'outputs/report.md').read_text(encoding='utf-8')
    assert report.startswith('## Strength lab — DISCARDED\n') and 'SPRT [' not in report and '| 1 |' not in report
    assert 'stopped by' not in report and '**Pooled:' not in report, report
    first = root / f'slices/{sprt.slice_artifact(1, 1, 4, 1)}/match.pgn'
    first.write_text(first.read_text().replace(book[5], book[6]))
    steps = summarized(*stopped)
    run(root, 'Join the waves', steps)
    run(root, 'Verify the complete batch', steps, expected=1)
    summary.unlink(missing_ok=True)
    steps = summarized(waves[0], waves[1], ('failure', {}))
    run(root, 'Refuse a discarded run', steps, expected=1)
    text = summary.read_text(encoding='utf-8')
    assert 'Wave 3: The wave ended `failure` without a decision' in text and '**Pooled:' not in text, text
    print('PASS: a counts mismatch, a slice outside the decided waves, a failed verify and a missing decision '
          'each refuse with no figure')

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
        wave_calls()
        publication_gates()
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
        env.update(SPRT='false', GAMES='8', GITHUB_RUN_ID='1')
        run(root, 'Plan the waves', env)
        planned = outputs(output)
        assert planned['waves'] == '1' and json.loads(planned['wave_plan']) == sprt.wave_plan(2, 2, 1, 1), planned
        env.update(WAVE='1', WAVES='1', WAVE_PLAN=planned['wave_plan'], SLICES=json.dumps(json.loads(planned['wave_plan'])[0]),
                   PRIOR_COUNTS='', SLICE_ROOT='shards')
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
            wave = record_wave(root, env)
            assert wave['status'] == 'complete' and wave['decision'] == 'fixed' and wave['lost_total'] == '0', wave
            env.update(summarize(root, env, run_needs(json.loads(env['WAVE_PLAN']), ('success', wave))))
            assert (env['OUTCOME'], env['PAIRS'], env['HEADING'], env['WAVED_GAMES']) == \
                ('fixed', '2', '## Strength lab', ''), env
            run(root, 'Verify the complete batch', env)
            run(root, 'Pool the result', env)
            pooled = root / 'outputs/pooled.md'
            run(root, 'Report', {**env, 'POOL_RESULT': 'success'})
            report = (root / 'outputs/report.md').read_text(encoding='utf-8')
            assert report.startswith('## Strength lab\n') and '| Replayed |' not in report and '**Pooled:' in report, \
                'a fixed run without a repair keeps the plain heading and no Replayed row'
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
        waved_batch(root, env)
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
