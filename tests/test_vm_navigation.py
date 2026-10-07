# SPDX-License-Identifier: GPL-2.0-only
"""Logical navigation spans retained source and preserves ordered terminal input."""

import time
import unittest

import test_vm_frontend as frontend
import test_vm_selection as selection


class NavigationTests(unittest.TestCase):
    setUp = selection.SelectionTests.setUp
    start = selection.SelectionTests.start
    snapshots = selection.SelectionTests.snapshots
    wait_snapshot = selection.SelectionTests.wait_snapshot
    seed_session = selection.SelectionTests.seed_session
    open_text = selection.SelectionTests.open_text
    search = selection.SelectionTests.search
    assert_draft = selection.SelectionTests.assert_draft

    def save(self, child):
        # A periodic autosave can precede completion of a queued motion. Wait
        # for this command's unique acknowledgement in the persisted snapshot.
        # Cold counted motions traverse hundreds of thousands of rows; their
        # completion uses a longer deadline than an ordinary UI autosave.
        name = 'navigation-' + str(time.monotonic_ns())
        child.command('workspace name ' + name)
        rows = frontend.WorkspaceTests.wait_snapshot(self, lambda rows: rows and
            next(iter(rows.values()))['name'] == name, timeout=15)
        return next(iter(rows.values()))['state']

    def test_numbered_lines_and_queued_motions_follow_rendered_text(self):
        text = 'first line\n  second\nthird\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        child.write(b'3ggjjk')
        found = self.save(child)['windows'][0]['history']
        self.assertEqual(found['byte'], len('first line\n'))
        child.write(b'4G^')
        found = self.save(child)['windows'][0]['history']
        self.assertEqual(found['byte'], len('first line\n  '))
        child.write(b'3gg999H')
        far = self.save(child)['windows'][0]['history']
        child.write(b'3gg10H')
        self.assertEqual(self.save(child)['windows'][0]['history'], far)
        child.write(b'3GywP')
        self.assert_draft(child, 'first ')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_fast_copy_paste_and_queued_bracketed_paste(self):
        journal, child = self.open_text('first line\n' + 'padding\n' * 15 + 'tail-marker')
        before = journal.read_bytes()
        child.write(b'3ggvlyP')
        self.assert_draft(child, 'fi')
        child.write(b'u\t3ggi\x1b[200~:qa\nnot a command\x1b[201~\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')
        self.assert_draft(child, ':qa\nnot a command')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_cancel_restores_cursor_and_previous_register(self):
        _, child = self.open_text('first line\n' + 'padding\n' * 15 + 'tail-marker')
        child.write(b'3ggvlyP')
        self.assert_draft(child, 'fi')
        child.write(b'u\t')
        before = self.save(child)['windows'][0]['history']
        child.write(b'999999Gjj\x03')
        child.repaint_until(b'Navigation canceled')
        self.assertEqual(self.save(child)['windows'][0]['history'], before)
        child.write(b'P')
        self.assert_draft(child, 'fi')
        child.finish()

    def test_empty_motion_preserves_previous_register(self):
        _, child = self.open_text('first line\n' + 'padding\n' * 15 + 'tail-marker')
        child.write(b'3ggvlyP')
        self.assert_draft(child, 'fi')
        child.write(b'u\t0yhP')
        self.assert_draft(child, 'fi')
        child.write(b'u\t2P')
        self.assert_draft(child, 'fifi')
        child.finish()

    def test_numbered_jump_to_cold_page_and_backwards_word_motion(self):
        text = 'first-marker\n' + 'line body\n' * 560000 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.stat().st_size
        child.write(b'999G0')
        found = self.save(child)['windows'][0]['history']
        self.assertEqual(found['byte'], len('first-marker\n') + 995 * len('line body\n'))
        child.write(b'3gg3wybP')
        self.assert_draft(child, 'marker\n')
        self.assertEqual(journal.stat().st_size, before)
        child.finish()

    def test_wrapped_rows_keep_column_and_count_across_cold_pages(self):
        text = 'first-marker\n' + 'line body\n' * 560000 + 'tail-marker'
        journal, child = self.open_text(text)
        child.write(b'3gg999999gj')
        state = self.save(child)
        end = state['windows'][0]['history']
        child.write(b'999999gk')
        first = self.save(child)['windows'][0]['history']
        self.assertFalse(first['heading'], (first, bytes(child.output[-2500:])))
        self.assertEqual(first['byte'], 0)
        child.repaint_until(b'retained-question-marker')
        child.write(b'999999gj')
        self.assertEqual(self.save(child)['windows'][0]['history'], end)
        child.resize(10, 25)
        child.repaint_until(b'tail-marker')
        child.write(b'3ggl3gj')
        found = self.save(child)['windows'][0]['history']
        self.assertEqual(found['byte'], len('first-marker\nline body\nline body\n') + 1)
        child.finish()

    def test_redacted_placeholder_is_one_cursor_unit(self):
        journal = self.seed_session('irc-ui-secret after\n' + 'padding\n' * 15 + 'tail-marker')
        config = self.root / 'state' / 'config.ini'
        config.write_text(self.fixture_config.read_text().replace(
            '${SNAJPAGENT_IRC_UI_KEY}', '"irc-ui-secret"'))
        config.chmod(0o600)
        child = self.start('-N', 'redacted-navigation', columns=120)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'tail-marker')
        child.write(b'3ggl')
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], len('irc-ui-secret'))
        child.write(b'h')
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], 0)
        child.write(b'vyP')
        self.assert_draft(child, '<redacted:secret>')
        child.finish()


if __name__ == '__main__':
    unittest.main()
