"""Check FetchContent release pins; publishing is opt-in and uses one tracking issue."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


MARKER = '<!-- strat-chess-dependency-releases -->'
TITLE = 'Dependency releases available'
ROOT = Path(__file__).resolve().parents[2]


def version(tag):
    match = re.fullmatch(r'v?(\d+)\.(\d+)\.(\d+)(-alpha)?', tag)
    return (*map(int, match.groups()[:3]), not bool(match[4])) if match else None


def read_pins(text):
    text = re.sub(r'(?m)#.*$', '', text)
    pins = []
    for name, declaration in re.findall(r'strat_declare_dependency\(\s*(\w+)\s+(.*?)\)', text, re.S):
        source = re.search(r'GIT_REPOSITORY\s+https://github\.com/([\w.-]+/[\w.-]+)\s', declaration)
        tag = re.search(r'GIT_TAG\s+(\S+)', declaration)
        if not source or not tag or version(tag[1]) is None or '-' in tag[1]:
            raise ValueError(f'{name}: expected a GitHub repository and numeric release tag')
        pins.append((name, source[1].removesuffix('.git'), tag[1]))
    if len(pins) != 3 or {pin[0] for pin in pins} != {'spdlog', 'nlohmann_json', 'catch2'}:
        raise ValueError('Expected exactly spdlog, nlohmann_json and catch2 strat_declare_dependency pins')
    return pins


def read_fastchess_pin(text):
    matches = re.findall(r'(?m)^  FASTCHESS_VERSION:\s*(\S+)\s*$', text)
    if len(matches) != 1 or version(matches[0]) is None:
        raise ValueError('Expected one numeric FASTCHESS_VERSION in strength.yml')
    return 'fastchess', 'Disservin/fastchess', matches[0]


def gh(*arguments):
    result = subprocess.run(['gh', *arguments], capture_output=True, text=True,
                            encoding='utf-8', timeout=120, check=True)
    return result.stdout


def api_items(endpoint):
    pages = json.loads(gh('api', '--paginate', '--slurp', endpoint))
    return [item for page in pages for item in page]


def collect_updates(pins):
    rows = []
    for name, repository, pin in pins:
        releases = api_items(f'repos/{repository}/releases?per_page=100')
        eligible = []
        for release in releases:
            tag = release['tag_name']
            if release['draft'] or version(tag) is None:
                continue
            # fastchess publishes its normal releases on an alpha-tagged channel.
            if tag.endswith('-alpha'):
                if repository != 'Disservin/fastchess':
                    continue
            elif release['prerelease']:
                continue
            eligible.append(release)
        if not eligible:
            raise ValueError(f'{repository}: no eligible numeric releases found')
        latest = max(eligible, key=lambda release: version(release['tag_name']))
        rows.append((name, repository, pin, latest['tag_name'], latest['html_url']))
    return rows


def render_report(rows, repository):
    updates = [row for row in rows if version(row[3]) > version(row[2])]
    lines = [MARKER, '', 'Newer dependency/tool releases are available.' if updates
             else 'All pins are at or ahead of the latest eligible releases.', '',
             '| Dependency/tool | Pinned | Latest release | Update available |', '|---|---|---|---|']
    for name, upstream, pin, latest, url in rows:
        newer = version(latest) > version(pin)
        lines.append(f'| [{name}](https://github.com/{upstream}) | `{pin}` | '
                     f'[{latest}]({url}) | {"Yes" if newer else "No"} |')
    lines += ['', 'Library checks exclude prereleases; fastchess also follows its established '
              '`-alpha` release channel.', '',
              f'Follow [Docs/Dependencies.md](https://github.com/{repository}/blob/main/Docs/Dependencies.md) for '
              'release-note review, pin/cache-key updates and dependency-specific validation.', '',
              'This report is maintained by the dependency-release workflow. Put discussion in '
              'comments; its body is replaced when findings change. Close it when addressed or '
              'deferred. A closed report stays closed until different updates are found.']
    return '\n'.join(lines) + '\n', bool(updates)


def report_findings(body):
    normalized = body.replace('\r\n', '\n').strip()
    # Only dependency rows determine whether a deferred update has changed.
    rows = [line.strip() for line in normalized.splitlines() if line.lstrip().startswith('| [')]
    return rows or normalized


def sync_issue(repository, body, has_updates, publish):
    issues = [issue for issue in api_items(f'repos/{repository}/issues?state=all&labels=dependencies&per_page=100')
              if 'pull_request' not in issue and MARKER in (issue['body'] or '').strip().splitlines()]
    if len(issues) > 1:
        raise ValueError('Multiple dependency tracking issues found; consolidate them first')
    issue = issues[0] if issues else None
    if issue and report_findings(issue['body']) == report_findings(body):
        return 'Unchanged report; no issue writes.'
    if not has_updates and (not issue or issue['state'] == 'closed'):
        return 'No newer releases; no issue writes.'
    action = f'Update issue #{issue["number"]}' if issue else 'Create tracking issue'
    if not publish:
        return f'Dry run: {action}.'
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'dependency-releases.md'
        path.write_text(body, encoding='utf-8')
        if issue:
            if issue['state'] == 'closed':
                gh('issue', 'reopen', str(issue['number']), '--repo', repository)
            gh('issue', 'edit', str(issue['number']), '--repo', repository, '--body-file', str(path))
        else:
            gh('issue', 'create', '--repo', repository, '--title', TITLE, '--body-file', str(path),
               '--label', 'dependencies',
               '--label', 'category:build-tooling', '--label', 'category:test', '--label', 'ready-for-human')
    return f'{action}.'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    parser.add_argument('--publish', action='store_true', help='Create/update the tracking issue')
    args = parser.parse_args()
    if not args.repo:
        parser.error('--repo or GITHUB_REPOSITORY is required')
    pins = read_pins((ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
    pins.append(read_fastchess_pin((ROOT / '.github/workflows/strength.yml').read_text(encoding='utf-8')))
    rows = collect_updates(pins)
    body, has_updates = render_report(rows, args.repo)
    print(body)
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a', encoding='utf-8') as handle:
            handle.write(body)
    # All dependencies must succeed before even reading the issue state for publication.
    print(sync_issue(args.repo, body, has_updates, args.publish))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        raise SystemExit(f'Dependency release check failed: {error}') from error
