"""Exercise the strength workflow's shell and artifact boundaries without a match.

The small extractor recognizes the workflow's named Bash run blocks, not general
YAML. The preflight CLI is covered by its own fixtures; its stub here captures
output placement while real staging/copy commands exercise artifact layout.
Directory uploads preserve paths relative to that directory. Complete and short
single/multi-arm batches exercise the actual verify, pool and report commands.
"""
from pathlib import Path
import argparse
import os
import shlex
import re
import shutil
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
workflow = (repo / '.github/workflows/strength.yml').read_text(encoding='utf-8')
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
        (root / '.github').mkdir()
        shutil.copytree(repo / '.github/scripts', root / '.github/scripts')
        (root / 'outputs').mkdir()
        stage = root / 'runner/stage'
        stage.mkdir(parents=True)
        positions = [f'8/8/8/8/8/8/4K3/4k3 {colour} - -' for colour in ['w', 'b']]
        (stage / 'book.epd').write_text(positions[1] + '\n' + positions[0] + '\n' + positions[1] + '\n' + positions[1] + '\n')
        summary = root / 'summary.md'
        env = {'SHARDS': '2', 'ROUNDS': '2', 'OPENING_OFFSET': '1', 'CAND_NAME': 'candidate-x',
               'REF_NAME': 'reference-y', 'BOOK_FILE': 'book.epd', 'GITHUB_STEP_SUMMARY': summary.as_posix(),
               'CMAKE_DEFINES': '', 'CMAKE_DEFINES_VALIDATED': 'true', 'ACTUAL_GAMES': '8',
               'BUILD_RESULT': 'success', 'CANDIDATE_UCI_OPTIONS': '', 'REFERENCE_UCI_OPTIONS': ''}
        env.update(RUNNER_TEMP=(root / 'runner').as_posix(), CALIBRATION='false',
                   REFERENCE_SHA='reference-fixture', GITHUB_SHA='candidate-fixture',
                   CANDIDATE_ARMS='', CANDIDATE_TC='10+0.1', REFERENCE_TC='10+0.1', THREADS='4')
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
            run(root, 'Verify the complete batch', env)
            run(root, 'Pool the result', env)
            pooled = root / 'outputs/pooled.md'
            assert '**Pooled:' in pooled.read_text() and '0.00 Elo**' in pooled.read_text()
            print(f'PASS: actual verify/pool workflow blocks, {"multi" if arms else "single"} arm')
            pooled.unlink()
            (root / 'shards/strength-1-shard-1/match.log').write_text('Ptnml(0-2): [0, 0, 1, 0, 0]\n')
            run(root, 'Verify the complete batch', env, expected=1)
            run(root, 'Pool the result', env, expected=1)
            assert not pooled.exists(), 'partial pool was published'
            print('PASS: incomplete last shard refuses batch and never publishes a pool')
            env['POOL_RESULT'] = 'failure'
            run(root, 'Report', env)
            report = (root / 'outputs/report.md').read_text(encoding='utf-8')
            assert 'DISCARDED' in report and '**Pooled:' not in report and 'Threads=4 |' in report
            print('PASS: failure summary contains no partial Elo')
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
