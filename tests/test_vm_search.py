# SPDX-License-Identifier: GPL-2.0-only
"""Whole-history search stays local, follows source anchors and scopes reports."""

import os
import subprocess
import unittest

import test_vm_anchors as anchors
import test_vm_reports as reports


class SearchTests(unittest.TestCase):
    setUp = anchors.AnchorTests.setUp
    start = anchors.AnchorTests.start
    snapshots = anchors.AnchorTests.snapshots
    wait_snapshot = anchors.AnchorTests.wait_snapshot
    seed_session = anchors.AnchorTests.seed_session
    save = anchors.AnchorTests.save

    def open_text(self, text):
        journal = self.seed_session(text)
        child = self.start('-N', 'search', columns=120)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'tail-marker')
        return journal, child

    def search(self, child, keys, expected=b'Match'):
        child.command('workspace')
        child.repaint_until(next(iter(self.snapshots())).encode())
        child.output.clear()
        child.write(keys)
        child.repaint_until(expected)
        return self.save(child)['windows'][0]['history']

    def test_directions_wrap_same_row_and_reflow(self):
        text = 'first needle and second needle\n' + 'middle\n' * 30 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        first = self.search(child, b'/needle\r', b'Match (wrapped)')
        second = self.search(child, b'n')
        self.assertEqual(first['byte'], text.index('needle'))
        self.assertEqual(second['byte'], text.index('needle', first['byte'] + 1))
        self.assertEqual(self.search(child, b'N')['byte'], first['byte'])
        self.assertEqual(self.search(child, b'?needle\r', b'Match (wrapped)')['byte'], second['byte'])
        self.assertEqual(self.search(child, b'n')['byte'], first['byte'])
        self.assertEqual(self.search(child, b'/\r')['byte'], second['byte'])
        self.assertEqual(self.search(child, b'n', b'Match (wrapped)')['byte'], first['byte'])
        child.resize(8, 24)
        child.repaint_until(b'needle')
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], first['byte'])
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_unicode_casefold_literal_query_and_no_match(self):
        text = 'Straße Σςσ a.b\n' + 'padding\n' * 20 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        self.search(child, b'/STRASSE\r', b'No matches')
        child.command('set ignorecase')
        found = self.search(child, b'n')
        self.assertEqual(found['byte'], 0)
        found = self.search(child, '/σσσ\r'.encode())
        self.assertEqual(found['byte'], len('Straße '.encode()))
        found = self.search(child, b'/a.b\r')
        self.assertEqual(found['byte'], len('Straße Σςσ '.encode()))
        self.search(child, b'/a.*b\r', b'No matches')
        child.command('set noignorecase')
        self.search(child, b'/STRASSE\r', b'No matches')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_slash_command_echo_and_entry_never_submit(self):
        text = 'local-search-marker\n' + 'padding\n' * 20 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        found = self.search(child, b'i/search local-search-marker\r')
        self.assertIn('command › /search local-search-marker'.encode(), child.output)
        self.assertEqual(found['byte'], 0)
        child.write(b'i/search\r')
        child.repaint_until(b'command')
        child.write(b'tail-marker\r')
        child.repaint_until(b'Match')
        found = self.search(child,
                            b'i\x1b[200~/search local-search-marker\npadding\x1b[201~\r')
        self.assertEqual(found['byte'], 0)
        self.assertEqual(journal.read_bytes(), before)
        state = self.save(child)
        self.assertTrue(all(not buffer.get('draft') for buffer in state.get('buffers', [])))
        child.finish()

    def test_search_other_source_page_and_obsolete_scan(self):
        text = 'first-page-marker\n' + 'padding\n' * 700000 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.stat().st_size
        found = self.search(child, b'/first-page-marker\r')
        self.assertEqual(found['byte'], 0)
        child.output.clear()
        child.write(b'/no-such-pattern\r\x03')
        child.repaint_until(b'Search canceled')
        child.command('verbosity 3')
        self.search(child, b'?tail-marker\r')
        child.command('vsp')
        child.repaint_until(b'tail-marker')
        windows = self.save(child)['windows']
        other = windows[0]['history']
        self.search(child, b'/first-page-marker\r')
        self.assertEqual(self.save(child)['windows'][0]['history'], other)
        self.assertEqual(journal.stat().st_size, before)
        child.finish()

    def test_long_logical_line_search_and_redraw(self):
        text = 'x' * 490000 + 'far-line-marker tail-marker'
        _, child = self.open_text(text)
        found = self.search(child, b'/far-line-marker\r')
        self.assertEqual(found['byte'], 490000)
        child.resize(8, 29)
        child.repaint_until(b'far-line')
        for _ in range(10):
            child.repaint_until(b'far-line', timeout=2)
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], 490000)
        child.finish()

    def test_current_verbosity_and_tool_headings(self):
        frontend = anchors.frontend
        provider = frontend.harness.FakeResponses()
        self.addCleanup(provider.close)
        first = True

        def respond(handler, request, sequence):
            nonlocal first
            if first:
                first = False
                body = provider.function_body(sequence, 'search-tool', 'exec_command', {
                    'cmd': "printf 'only-%s-search\\n' tool", 'yield_time_ms': 10000})
            else:
                body = provider.response_body(sequence, 'tail-marker')
            provider.reply(handler, body.encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        frontend.harness.write_irc_config(config, provider.port, 'host-model')
        result = subprocess.run([str(frontend.BINARY), '--config', str(config), '--dotdir',
                                 str(self.root / 'state'), '-e', '--', 'run fixture'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        journal, = (self.root / 'state' / 'sessions').glob('*/events.jsonl')
        before = journal.read_bytes()
        child = self.start('-N', 'tools', columns=120)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'tail-marker')
        child.command('verbosity 0')
        self.search(child, b'/only-tool-search\r', b'No matches')
        child.command('verbosity 3')
        found = self.search(child, b'n')
        self.assertIn('/output', found['key'])
        child.command('verbosity 1')
        found = self.search(child, b'/exec_command\r')
        self.assertFalse(found['heading'])
        self.assertIn('/tool_', found['key'])
        self.assertEqual(journal.read_bytes(), before)
        child.finish()


class ReportSearchTests(unittest.TestCase):
    setUp = reports.ReportTests.setUp
    start = reports.ReportTests.start
    snapshots = reports.ReportTests.snapshots
    wait_snapshot = reports.ReportTests.wait_snapshot
    attached = reports.ReportTests.attached
    inputs = reports.ReportTests.inputs
    reports = reports.ReportTests.reports

    def test_multiline_search_command_never_reaches_controlled_owner(self):
        child = self.attached('local-command')
        before = self.owner.journal.read_bytes()
        child.write(b'i\x1b[200~/search private-query\nsecond-line\x1b[201~\r')
        child.repaint_until(b'No matches')
        self.assertIn('command › /search private-query'.encode(), child.output)
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.journal.read_bytes(), before)
        child.finish('close')
        self.owner.status('detached')

    def test_report_search_is_scoped_and_keeps_owner_idle(self):
        child = self.attached('report-search')
        before = self.owner.journal.read_bytes()
        child.write(b'i/help\r')
        child.repaint_until(b'REPORT')
        self.reports(1)
        child.output.clear()
        child.write(b'/configure\r')
        child.repaint_until(b'Match')
        child.repaint_until(b'/configure')
        child.output.clear()
        child.write(b'/not-a-help-entry\r')
        child.repaint_until(b'No matches')
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.journal.read_bytes(), before)
        child.finish('q')
        self.owner.status('detached')


if __name__ == '__main__':
    unittest.main()
