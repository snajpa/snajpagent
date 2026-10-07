# SPDX-License-Identifier: GPL-2.0-only
"""Vim composer behavior through the real workspace PTY and saved drafts."""

import os
import subprocess
import time
import unittest

import test_vm_frontend as frontend
from store_history import journal_paths
from test_vm_frontend import rollout


class EditorTests(unittest.TestCase):
    snapshots = frontend.WorkspaceTests.snapshots
    start = frontend.WorkspaceTests.start
    seed_session = frontend.WorkspaceTests.seed_session

    def setUp(self):
        frontend.WorkspaceTests.setUp(self)
        self.journal = self.seed_session()
        self.child = self.start('-N', 'editor')
        self.child.command('history ' + self.journal.parent.name)
        self.child.until(b'history')
        self.check_number = 0

    def check(self, text, cursor=None):
        self.check_number += 1
        name = 'check-' + str(self.check_number)
        self.child.command('workspace name ' + name)

        def ready(rows):
            while self.child.read(0):
                pass
            return rows and next(iter(rows.values()))['name'] == name

        rows = frontend.WorkspaceTests.wait_snapshot(self, ready)
        buffer = rollout(next(iter(rows.values()))['state']['buffers'][0])
        self.assertEqual(buffer['draft'], text)
        if cursor is not None:
            self.assertEqual(buffer['cursor'], cursor)

    def escape(self):
        self.child.write(b'\x1b')
        time.sleep(.06)
        self.child.repaint_until(b'NORMAL composer')

    def seed(self, text):
        self.child.write(b'i\x1b[200~' + text.encode() + b'\x1b[201~')
        self.escape()

    def test_insert_undo_redo_and_new_branch(self):
        self.child.write(b'iabc')
        self.escape()
        self.check('abc', 2)
        self.child.write(b'u')
        self.check('', 0)
        self.child.write(b'\x12')
        self.check('abc', 2)
        self.child.write(b'uiX')
        self.escape()
        self.child.write(b'\x12')
        self.check('X', 0)

    def test_literal_paste_has_its_own_undo_group(self):
        self.child.write(b'iprefix')
        self.child.write(b'\x1b[200~' + 'é👩‍💻\n:qa'.encode() + b'\x1b[201~tail')
        self.escape()
        self.child.write(b'u')
        self.check('prefixé👩‍💻\n:qa')
        self.child.write(b'u')
        self.check('prefix')
        self.child.write(b'u')
        self.check('')

    def test_multiplied_word_counts_change_and_undo(self):
        self.seed('one two three four five six seven')
        self.child.write(b'02d3w')
        self.check('seven', 0)
        self.child.write(b'u0cwONE')
        self.escape()
        self.check('ONE two three four five six seven', 2)
        self.child.write(b'u')
        self.check('one two three four five six seven')

    def test_character_operator_reaches_last_grapheme(self):
        self.seed('éx')
        self.child.write(b'$dl')
        self.check('é', 0)
        self.child.write(b'u02dl')
        self.check('', 0)

    def test_unicode_word_and_grapheme_motions(self):
        text = 'é_β 中文 👩‍💻! last'
        self.seed(text)
        self.child.write(b'02w')
        self.check(text, len('é_β 中文 '.encode()))
        self.child.write(b'x')
        self.check('é_β 中文 ! last')
        self.child.write(b'u0e')
        self.check(text, len('é_'.encode()))
        self.child.write(b'2b')
        self.check(text, 0)

    def test_linewise_delete_put_and_change(self):
        self.seed('one\ntwo\nthree')
        self.child.write(b'ggddp')
        self.check('two\none\nthree', 4)
        self.child.write(b'Gdd')
        self.check('two\none', 4)
        self.child.write(b'p')
        self.check('two\none\nthree', 8)
        self.child.write(b'gg2ccreplacement')
        self.escape()
        self.check('replacement\nthree')
        self.child.write(b'u')
        self.check('two\none\nthree')

    def test_linewise_put_preserves_an_empty_destination_line(self):
        self.seed('first')
        self.child.write(b'ggddP')
        self.check('first\n', 0)
        self.child.write(b'up')
        self.check('\nfirst', 1)
        self.child.write(b'uyyp')
        self.check('\n', 1)

    def test_counted_linewise_put_accepts_exact_draft_limit(self):
        self.seed('')
        self.child.write(b'yy1048576p')
        self.check('\n' * 1048576, 1)

    def test_open_lines_and_line_boundaries(self):
        self.seed('  first\nlast')
        self.child.write(b'gg^')
        self.check('  first\nlast', 2)
        self.child.write(b'Oabove')
        self.escape()
        self.check('above\n  first\nlast', 4)
        self.child.write(b'Goafter')
        self.escape()
        self.check('above\n  first\nlast\nafter')
        self.child.write(b'2G$')
        self.check('above\n  first\nlast\nafter', len('above\n  firs'))

    def test_vertical_column_and_display_rows(self):
        self.seed('abcdef\nx\nabcdef')
        self.child.write(b'gg4lj')
        self.check('abcdef\nx\nabcdef', 7)
        self.child.write(b'j')
        self.check('abcdef\nx\nabcdef', 13)
        self.child.resize(12, 4)
        self.child.write(b'gggj')
        self.check('abcdef\nx\nabcdef', 4)
        self.child.write(b'gk')
        self.check('abcdef\nx\nabcdef', 0)

    def test_yank_and_paste_between_splits(self):
        self.seed('first second')
        self.child.write(b'0yw')
        self.child.command('vsp')
        self.child.write(b'$p')
        self.check('first secondfirst ')
        self.child.write(b'u')
        self.check('first second')

    def test_backward_yank_moves_to_range_start(self):
        self.seed('first second')
        self.child.write(b'$yb')
        self.check('first second', 6)
        self.child.write(b'P')
        self.check('first seconsecond')

    def test_register_is_shared_between_session_drafts(self):
        self.seed('first second')
        self.child.write(b'0yw')
        provider = self.fixture_provider
        provider.runtime_handler = lambda handler, request, sequence: provider.reply(
            handler, provider.response_body(sequence, 'second-session').encode())
        result = subprocess.run([str(frontend.BINARY), '--config', str(self.fixture_config),
                                 '--dotdir', str(self.root / 'state'), '-e', '--', 'second'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        other, = (p.parent.name for p in journal_paths(self.root / 'state')
                  if p.parent.name != self.journal.parent.name)
        self.child.command('history ' + other)
        self.child.write(b'i')
        self.escape()
        self.child.write(b'p')
        self.check('first ')
        buffers = next(iter(self.snapshots().values()))['state']['buffers']
        self.assertEqual(next(rollout(x)['draft'] for x in buffers if x['session'] == self.journal.parent.name),
                         'first second')

    def test_large_count_stops_at_text_boundary(self):
        self.seed('abc\ndef')
        self.child.write(b'999999999999999999j')
        self.check('abc\ndef', 6)
        self.child.write(b'999999999999999999k')
        self.check('abc\ndef', 2)
        self.child.write(b'999999999999999999b')
        self.check('abc\ndef', 0)

    def test_unsupported_operator_and_prefix_leave_text_intact(self):
        self.seed('preserve me')
        self.child.write(b'0du')
        self.check('preserve me', 0)
        self.child.write(b'gi')
        self.check('preserve me', 0)
        self.child.write(b'9999999999999999999999999999999999999999999999999999999999999')
        self.child.write(b'\x1b')
        time.sleep(.06)
        self.check('preserve me', 0)

    def test_mouse_focus_cancels_pending_operator(self):
        self.seed('first second')
        self.child.write(b'0d\x1b[<0;1;1M\x1b[<0;1;1m\tl')
        self.check('first second', 1)

    def test_window_positions_centering_and_end_column(self):
        text = '\n'.join('line' + str(i).zfill(2) for i in range(12))
        self.seed(text)
        self.child.write(b'6Gzz')
        self.check(text, 35)
        self.child.write(b'H')
        self.check(text, 21)
        self.child.write(b'M')
        self.check(text, 28)
        self.child.write(b'L')
        self.check(text, 42)
        self.child.write(b'$j')
        self.check(text, 54)

    def test_joined_grapheme_undo_uses_valid_utf8_deltas(self):
        self.seed('👩💻')
        self.child.write(b'0li' + '\u200d'.encode())
        self.escape()
        self.check('👩‍💻', 0)
        self.child.write(b'u')
        self.check('👩💻', 4)
        self.child.write(b'\x12')
        self.check('👩‍💻', 0)


if __name__ == '__main__':
    unittest.main()
