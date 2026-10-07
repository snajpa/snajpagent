# SPDX-License-Identifier: GPL-2.0-only
"""Visual yanks copy logical text into local drafts without changing the owner."""

import time
import unittest

import test_vm_search as search
import test_vm_reports as reports
from test_vm_frontend import rollout


class SelectionTests(unittest.TestCase):
    setUp = search.SearchTests.setUp
    start = search.SearchTests.start
    snapshots = search.SearchTests.snapshots
    wait_snapshot = search.SearchTests.wait_snapshot
    seed_session = search.SearchTests.seed_session
    save = search.SearchTests.save
    open_text = search.SearchTests.open_text
    search = search.SearchTests.search

    def assert_draft(self, child, expected):
        state = self.save(child)
        self.assertEqual(rollout(state['buffers'][0])['draft'], expected)

    def copied(self, child, keys):
        child.output.clear()
        child.write(keys)
        child.repaint_until(b'Yanked')
        child.write(b'P')

    def test_character_selection_preserves_graphemes_and_soft_wraps(self):
        text = 'first é界👩\u200d💻 rest\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        self.search(child, '/é\r'.encode())
        child.resize(10, 18)
        child.repaint_until('é'.encode())
        self.copied(child, b'v2ly')
        self.assert_draft(child, 'é界👩\u200d💻')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_line_selection_and_shared_composer_register(self):
        text = 'first line\nsecond line\nthird\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        self.search(child, b'/first line\r')
        self.copied(child, b'Vjy')
        self.assert_draft(child, '• first line\n  second line\n')
        child.write(b'uyyP')
        self.assert_draft(child, '\n')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_rectangle_pads_short_lines_and_keeps_wide_characters(self):
        text = 'a界éx\n\tz\nq\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        self.search(child, '/界\r'.encode())
        self.copied(child, b'\x16l2jy')
        self.assert_draft(child, '界é\n   \n   ')
        child.finish()

    def test_rectangular_put_preserves_draft_tabs_and_one_undo_group(self):
        text = 'XY\nUV\nQR\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        self.search(child, b'/XY\r')
        child.write(b'\x16l2jy')
        child.repaint_until(b'Yanked')
        child.write(b'i\x1b[200~ab\n\tz\nq\x1b[201~\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')
        child.write(b'gglP')
        self.assert_draft(child, 'aXYb\n UV   z\nqQR')
        child.write(b'u')
        self.assert_draft(child, 'ab\n\tz\nq')
        child.write(b'3P')
        self.assert_draft(child, 'aXYXYXYb\n UVUVUV   z\nqQRQRQR')
        child.write(b'up')
        self.assert_draft(child, 'abXY\n  UV  z\nq QR')
        child.write(b'u999999999P')
        child.repaint_until(b'input limit reached')
        self.assert_draft(child, 'ab\n\tz\nq')
        child.finish()

    def test_rectangular_put_extends_beyond_final_line(self):
        text = 'XY\nUV\nQR\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        self.search(child, b'/XY\r')
        child.write(b'\x16l2jy')
        child.repaint_until(b'Yanked')
        child.write(b'ia\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')
        child.write(b'P')
        self.assert_draft(child, 'XYa\nUV\nQR')
        child.write(b'u')
        self.assert_draft(child, 'a')
        child.finish()

    def test_yy_and_word_yank(self):
        text = 'one two three\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        self.search(child, b'/two\r')
        self.copied(child, b'yw')
        self.assert_draft(child, 'two ')
        child.write(b'u\t')
        self.copied(child, b'yy')
        self.assert_draft(child, '• one two three\n')
        child.finish()

    def test_backward_and_counted_yank_motions(self):
        text = 'one two three four five six seven\nsecond\nthird\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        self.search(child, b'/two\r')
        self.copied(child, b'yb')
        self.assert_draft(child, 'one ')
        child.write(b'u\t')
        self.search(child, b'/one\r')
        self.copied(child, b'2y3w')
        self.assert_draft(child, 'one two three four five six ')
        child.write(b'u\t')
        self.search(child, b'/second\r')
        self.copied(child, b'2yy')
        self.assert_draft(child, '  second\n  third\n')
        child.finish()

    def test_large_yank_uses_private_file_and_pastes_exactly(self):
        text = 'large-' + 'x' * 180000
        journal, child = self.open_text(text + '\n' + 'padding\n' * 15 + 'tail-marker')
        self.search(child, b'/large-\r')
        self.copied(child, b'v$y')
        child.repaint_until(b'file-backed register')
        self.assert_draft(child, text)
        child.finish()

    def test_visual_cancel_and_verbosity_preserve_previous_register(self):
        text = 'first line\nsecond line\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        self.search(child, b'/first\r')
        self.copied(child, b'vey')
        self.assert_draft(child, 'first')
        child.write(b'u\tvj\x03')
        child.repaint_until(b'NORMAL')
        child.write(b'v')
        child.command('verbosity 2')
        child.repaint_until(b'register preserved')
        child.write(b'P')
        self.assert_draft(child, 'first')
        child.finish()


class ReportSelectionTests(unittest.TestCase):
    setUp = reports.ReportTests.setUp
    start = reports.ReportTests.start
    snapshots = reports.ReportTests.snapshots
    wait_snapshot = reports.ReportTests.wait_snapshot
    inputs = reports.ReportTests.inputs
    attached = reports.ReportTests.attached

    def test_report_yank_pastes_locally_without_submitting(self):
        child = self.attached('copy-report')
        before = self.owner.journal.read_bytes()
        child.write(b'i/status\r')
        reports.ReportTests.reports(self, 1)
        reports.ReportTests.escape(self, child)
        child.command('report')
        # Navigation belongs to the requested report even while its catalogue
        # and immutable source are still loading on the worker.
        child.write(b'ggyy')
        child.repaint_until(b'Yanked')
        child.write(b'P')
        child.command('workspace name copied-report')
        rows = self.wait_snapshot(lambda rows: rows and
            next(iter(rows.values()))['name'] == 'copied-report')
        state = next(iter(rows.values()))['state']
        self.assertEqual(rollout(state['buffers'][0])['draft'], '/status\n')
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.journal.read_bytes(), before)
        child.finish('q!')


if __name__ == '__main__':
    unittest.main()
