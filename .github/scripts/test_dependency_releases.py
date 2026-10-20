"""Offline release-check fixtures: ordering, channel policy and issue-write lifecycle."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest
from unittest.mock import patch

import check_dependency_releases as checker


def release(tag, **flags):
    return {'tag_name': tag, 'draft': False, 'prerelease': False,
            'html_url': f'https://github.com/example/repo/releases/tag/{tag}', **flags}


class ReleaseChecks(unittest.TestCase):
    def test_actual_pins_and_missing_declaration(self):
        source = (checker.ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        pins = checker.read_pins(source)
        self.assertEqual([pin[0] for pin in pins], ['spdlog', 'nlohmann_json', 'catch2'])
        with self.assertRaises(ValueError):
            checker.read_pins(source.replace('strat_declare_dependency(catch2', '# removed(catch2'))
        with self.assertRaises(ValueError):
            checker.read_pins(re.sub(r'GIT_TAG\s+\S+', 'GIT_TAG main', source, count=1))
        tool = checker.read_fastchess_pin(
            (checker.ROOT / '.github/workflows/strength.yml').read_text(encoding='utf-8'))
        self.assertEqual(tool[:2], ('fastchess', 'Disservin/fastchess'))
        with self.assertRaises(ValueError):
            checker.read_fastchess_pin('')

    def test_numeric_order_and_library_prerelease_filter(self):
        releases = [release('v1.9.0'), release('v1.10.0'), release('v2.0.0', draft=True),
                    release('v3.0.0', prerelease=True), release('v4.0.0-alpha'), release('nightly')]
        with patch.object(checker, 'api_items', return_value=releases):
            rows = checker.collect_updates([('lib', 'owner/lib', 'v1.8.0')])
        self.assertEqual(rows[0][3], 'v1.10.0')

    def test_fastchess_alpha_channel_and_final_release(self):
        pins = [('fastchess', 'Disservin/fastchess', 'v1.8.2-alpha')]
        with patch.object(checker, 'api_items', return_value=[release('v1.9.0-alpha', prerelease=True)]):
            rows = checker.collect_updates(pins)
        self.assertEqual(rows[0][3], 'v1.9.0-alpha')
        with patch.object(checker, 'api_items', return_value=[release('v1.8.2-alpha'), release('v1.8.2')]):
            rows = checker.collect_updates(pins)
        self.assertEqual(rows[0][3], 'v1.8.2')

    def test_no_eligible_releases_is_failure(self):
        with patch.object(checker, 'api_items', return_value=[release('v1.0.0', prerelease=True)]):
            with self.assertRaises(ValueError):
                checker.collect_updates([('lib', 'owner/lib', 'v0.9.0')])

    def test_paginated_api_is_flattened(self):
        with patch.object(checker, 'gh', return_value=json.dumps([[release('v1.0.0')], [release('v2.0.0')]])) as gh:
            items = checker.api_items('repos/owner/lib/releases?per_page=100')
        self.assertEqual(len(items), 2)
        self.assertIn('--paginate', gh.call_args.args)
        self.assertIn('--slurp', gh.call_args.args)

    def test_equal_and_older_releases_do_not_report_updates(self):
        for pin, latest in [('v1.2.0', 'v1.2.0'), ('v1.3.0', 'v1.2.0')]:
            body, updates = checker.render_report([('lib', 'owner/lib', pin, latest, 'https://example.com')], 'owner/repo')
            self.assertFalse(updates)
            self.assertTrue(body.startswith(checker.MARKER))

    def run_sync(self, issues, updates=True, publish=True, body=None):
        body = body or checker.MARKER + '\nnew report\n'
        writes = []

        def fake_gh(*args):
            if args[0] == 'api':
                self.assertIn('labels=dependencies', args[-1])
                return json.dumps([issues])
            if '--body-file' in args:
                path = Path(args[args.index('--body-file') + 1])
                self.assertEqual(path.read_text(encoding='utf-8'), body)
                self.assertNotIn('--body', args)
            if args[:2] == ('issue', 'create'):
                labels = [args[index + 1] for index, arg in enumerate(args) if arg == '--label']
                self.assertEqual(labels, ['dependencies', 'category:build-tooling', 'category:test', 'ready-for-human'])
            writes.append(args[:3])
            return ''

        with patch.object(checker, 'gh', side_effect=fake_gh):
            checker.sync_issue('owner/repo', body, updates, publish)
        return writes

    def issue(self, state='open', body=None):
        return {'number': 42, 'state': state, 'body': body or checker.MARKER + '\nold report\n'}

    def test_create_once_then_unchanged_is_quiet(self):
        self.assertEqual(self.run_sync([]), [('issue', 'create', '--repo')])
        body = checker.MARKER + '\nnew report\n'
        self.assertEqual(self.run_sync([self.issue(body=body)], body=body), [])

    def test_body_formatting_does_not_update_or_reopen_reports(self):
        body = checker.MARKER + '\nnew report\n'
        for state in ('open', 'closed'):
            for stored in (body.replace('\n', '\r\n'), body.rstrip(), '\n' + body + '\n'):
                with self.subTest(state=state, stored=stored):
                    self.assertEqual(self.run_sync([self.issue(state, stored)], body=body), [])

    def test_prose_edits_do_not_reopen_deferred_findings(self):
        rows = [('lib', 'owner/lib', 'v1.0.0', 'v1.1.0', 'https://example.com/release')]
        body, _ = checker.render_report(rows, 'owner/repo')
        stored = 'Maintainer note: deferred until next month.\n\n' + body.replace(
            'Newer dependency/tool releases are available.', 'Deferred until next month.')
        self.assertEqual(self.run_sync([self.issue('closed', stored)], body=body), [])
        newer, _ = checker.render_report(
            [('lib', 'owner/lib', 'v1.0.0', 'v1.2.0', 'https://example.com/new-release')], 'owner/repo')
        self.assertEqual(self.run_sync([self.issue('closed', stored)], body=newer),
                         [('issue', 'reopen', '42'), ('issue', 'edit', '42')])

    def test_update_existing_and_reuse_closed_issue(self):
        self.assertEqual(self.run_sync([self.issue()]), [('issue', 'edit', '42')])
        self.assertEqual(self.run_sync([self.issue('closed')]),
                         [('issue', 'reopen', '42'), ('issue', 'edit', '42')])
        body = checker.MARKER + '\nnew report\n'
        self.assertEqual(self.run_sync([self.issue('closed', body)], body=body), [])

    def test_up_to_date_and_dry_run_never_create(self):
        self.assertEqual(self.run_sync([], updates=False), [])
        self.assertEqual(self.run_sync([], publish=False), [])
        self.assertEqual(self.run_sync([self.issue('closed')], updates=False), [])
        self.assertEqual(self.run_sync([self.issue()], updates=False), [('issue', 'edit', '42')])

    def test_duplicates_and_api_failure_prevent_writes(self):
        with self.assertRaises(ValueError):
            self.run_sync([self.issue(), self.issue('closed')])
        with patch.object(checker, 'gh', side_effect=subprocess.CalledProcessError(1, 'gh')) as gh:
            with self.assertRaises(subprocess.CalledProcessError):
                checker.sync_issue('owner/repo', checker.MARKER, True, True)
        self.assertEqual(gh.call_count, 1)

    def test_collection_failure_prevents_issue_lookup(self):
        with patch.object(checker, 'collect_updates', side_effect=ValueError('upstream unavailable')):
            with patch.object(checker, 'sync_issue') as sync:
                with patch('sys.argv', ['checker', '--repo', 'owner/repo', '--publish']):
                    with self.assertRaises(ValueError):
                        checker.main()
        sync.assert_not_called()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true', help='Run the offline checker fixtures')
    _, test_arguments = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *test_arguments])
