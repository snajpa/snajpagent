# SPDX-License-Identifier: GPL-2.0-only
"""Mouse reports use actual rendered cells and preserve source/draft ownership."""

import re
import shutil
import subprocess
import time
import unittest
import unicodedata

import test_vm_frontend as frontend
import test_vm_reports as reports
import test_vm_search as search


def mouse(child, row, column, button=0, release=False):
    child.write(f'\x1b[<{button};{column + 1};{row + 1}{"m" if release else "M"}'.encode())


def positions(child, marker):
    child.repaint_until(marker.encode())
    # The frontend's forced frame supplies CUP-addressed runs. Locate a marker
    # in those real runs rather than assuming where wrapping put its source.
    output = bytes(child.output).decode('utf-8', 'replace')
    frame = output.rfind('\x1b[2J')
    if frame >= 0:
        output = output[frame + 4:]
    row = column = 0
    found = []
    for part in re.split(r'(\x1b\[[0-9;?]*[A-Za-z])', output):
        if part.startswith('\x1b['):
            match = re.fullmatch(r'\x1b\[(\d+);(\d+)H', part)
            if match:
                row, column = (int(x) - 1 for x in match.groups())
            continue
        at = part.find(marker)
        if at >= 0:
            found.append((row, column + len(part[:at])))
        for char in part:
            if not unicodedata.combining(char) and char != '\u200d':
                column += 2 if unicodedata.east_asian_width(char) in ('W', 'F') else 1
    return list(dict.fromkeys(found))


def position(child, marker, occurrence=0, minimum=1):
    deadline = time.monotonic() + 5
    found = []
    while time.monotonic() < deadline:
        found = positions(child, marker)
        if len(found) > occurrence and len(found) >= minimum:
            return found[occurrence]
        child.read(.05)
    raise AssertionError((marker, found))
from test_vm_frontend import rollout


class MouseTests(unittest.TestCase):
    setUp = search.SearchTests.setUp
    start = search.SearchTests.start
    snapshots = search.SearchTests.snapshots
    wait_snapshot = search.SearchTests.wait_snapshot
    seed_session = search.SearchTests.seed_session
    open_text = search.SearchTests.open_text
    search = search.SearchTests.search

    def save(self, child):
        self.check_number = getattr(self, 'check_number', 0) + 1
        name = 'mouse-check-' + str(self.check_number)
        child.command('workspace name ' + name)
        rows = self.wait_snapshot(lambda rows: rows and
            next(iter(rows.values()))['name'] == name)
        return next(iter(rows.values()))['state']

    def test_click_places_source_cursor_on_whole_wide_grapheme(self):
        text = 'click-a é界👩\u200d💻z\n' + 'padding\n' * 15 + 'tail-marker'
        journal, child = self.open_text(text)
        before = journal.read_bytes()
        self.search(child, b'/click-a\r')
        row, column = position(child, 'click-a')
        mouse(child, row, column + 10)  # Second cell of 界.
        mouse(child, row, column + 10, release=True)
        anchor = self.save(child)['windows'][0]['history']
        self.assertEqual(anchor['byte'], len('click-a é'.encode()))
        self.assertFalse(anchor['follow'])
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_drag_copies_across_wrapped_rows_and_release_preserves_copy(self):
        selected = 'start-é界👩\u200d💻 some words with wraps\nfinish'
        journal, child = self.open_text(selected + '\n' + 'padding\n' * 15 + 'tail-marker')
        before = journal.read_bytes()
        self.search(child, b'/start-\r')
        child.resize(14, 22)
        first = position(child, 'start-')
        last = position(child, 'finish')
        mouse(child, *first)
        mouse(child, last[0], last[1] + 5, button=32)
        mouse(child, last[0], last[1] + 5, release=True)
        child.write(b'y')
        # A delayed/repeated button release must not cancel an asynchronous yank.
        mouse(child, last[0], last[1] + 5, release=True)
        child.write(b'P')
        self.assertEqual(rollout(self.save(child)['buffers'][0])['draft'], selected)
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_nomouse_ignores_reports_and_restores_modes(self):
        _, child = self.open_text('toggle-click\n' + 'padding\n' * 15 + 'tail-marker')
        self.search(child, b'/toggle-click\r')
        where = position(child, 'toggle-click')
        child.command('set nomouse')
        child.until(b'\x1b[?1002l')
        before = self.save(child)['windows'][0]['history']
        mouse(child, where[0], where[1] + 7)
        mouse(child, *where, button=65)
        mouse(child, *where, release=True)
        self.assertEqual(self.save(child)['windows'][0]['history'], before)
        child.output.clear()
        child.command('set mouse')
        child.until(b'\x1b[?1002h')
        mouse(child, where[0], where[1] + 7)
        mouse(child, where[0], where[1] + 7, release=True)
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], 7)
        child.finish()
        self.assertIn(b'\x1b[?1002l', child.output)
        self.assertIn(b'\x1b[?1006l', child.output)

    def test_wheel_scrolls_hovered_window_without_taking_focus(self):
        text = ''.join(f'line-{i:03}\n' for i in range(80)) + 'tail-marker'
        _, child = self.open_text(text)
        self.search(child, b'/line-030\r')
        child.command('vsp')
        where = position(child, 'line-030', minimum=2)
        self.assertEqual(where[1], 2)
        before = self.save(child)
        mouse(child, *where, button=64)
        after = self.save(child)
        self.assertEqual(after['focus'], before['focus'])
        self.assertLess(after['windows'][0]['top'], before['windows'][0]['top'])
        self.assertEqual(after['windows'][1]['history'], before['windows'][1]['history'])
        self.assertEqual(after['windows'][1]['top'], before['windows'][1]['top'])
        child.finish()

    def test_separator_drag_persists_both_axes_and_shrink_cancels_drag(self):
        child = self.start('-N', 'mouse-layout', rows=21, columns=81)
        child.command('vsp')
        before = self.save(child)
        self.assertEqual(len(before['windows']), 2)
        child.repaint_until(b'sessions')
        mouse(child, 3, 40)
        mouse(child, 3, 57, button=32)
        mouse(child, 3, 57, release=True)
        after = self.save(child)
        self.assertEqual(after['focus'], before['focus'])
        self.assertEqual(after['layout']['weight'], 7125)
        child.command('sp')
        self.assertEqual(len(self.save(child)['windows']), 3)
        child.repaint_until(b'sessions')
        mouse(child, 9, 70)
        mouse(child, 5, 70, button=32)
        mouse(child, 5, 70, release=True)
        after = self.save(child)
        self.assertEqual(after['layout']['second']['weight'], 2632)
        mouse(child, 3, 57)
        child.resize(2, 2)
        child.read(.1)
        mouse(child, 0, 0, button=32)
        mouse(child, 0, 0, release=True)
        child.resize(21, 81)
        self.assertEqual(self.save(child)['layout'], after['layout'])
        child.finish()
        child = self.start('--resume', 'mouse-check-' + str(self.check_number),
                           rows=21, columns=81)
        self.assertEqual(self.save(child)['layout'], after['layout'])
        child.finish()

    def test_click_composer_inserts_at_wrapped_unicode_cell(self):
        journal, child = self.open_text('history-marker\n' + 'padding\n' * 15 + 'tail-marker')
        before = journal.read_bytes()
        draft = 'draft-界é\nsecond-line'
        child.write(b'i\x1b[200~' + draft.encode() + b'\x1b[201~')
        row, column = position(child, 'draft-')
        mouse(child, row, column + 7)  # Second cell of the wide character.
        mouse(child, row, column + 7, release=True)
        child.write(b'X\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')
        state = self.save(child)
        self.assertEqual(rollout(state['buffers'][0])['draft'], 'draft-X界é\nsecond-line')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_tab_and_escaped_control_hits_use_source_bytes(self):
        text = '\tcell-hit\x1b after\n' + 'padding\n' * 15 + 'tail-marker'
        _, child = self.open_text(text)
        self.search(child, b'/cell-hit\r')
        row, column = position(child, 'cell-hit')
        mouse(child, row, column - 2)
        mouse(child, row, column - 2, release=True)
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], 0)
        mouse(child, row, column + len('cell-hit') + 2)
        mouse(child, row, column + len('cell-hit') + 2, release=True)
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], len('\tcell-hit'))
        child.finish()

    def test_click_cancels_counted_history_yank_operator(self):
        text = 'one two three four five six\n' + 'padding\n' * 15 + 'tail-marker'
        _, child = self.open_text(text)
        self.search(child, b'/one\r')
        row, column = position(child, 'one two')
        child.write(b'4y')
        mouse(child, row, column + 4)
        mouse(child, row, column + 4, release=True)
        child.write(b'w')
        self.assertEqual(self.save(child)['windows'][0]['history']['byte'], len('one two '))
        child.finish()

    def test_remote_wrappers_relay_mouse_drag_and_register_paste(self):
        journal = self.seed_session('remote-copy ends\n' + 'padding\n' * 15 + 'tail-marker')
        child = self.start('-N', 'mouse-remote', transport=[str(frontend.BINARY), 'remote',
                           str(frontend.BINARY), 'remote'])
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'tail-marker')
        self.search(child, b'/remote-copy\r')
        row, column = position(child, 'remote-copy')
        # Reverse selection, carried byte by byte through two real wrappers.
        sequence = (f'\x1b[<0;{column + 11};{row + 1}M'
                    f'\x1b[<32;{column + 1};{row + 1}M'
                    f'\x1b[<0;{column + 1};{row + 1}m').encode()
        for byte in sequence:
            child.write(bytes([byte]))
        child.write(b'yP')
        self.assertEqual(rollout(self.save(child)['buffers'][0])['draft'], 'remote-copy')
        child.finish()

    @unittest.skipUnless(shutil.which('tmux'), 'tmux is unavailable')
    def test_mouse_reaches_workspace_inside_real_tmux(self):
        journal = self.seed_session('tmux-marker tail\n' + 'padding\n' * 15 + 'tail-marker')
        before = journal.read_bytes()
        tmux = [shutil.which('tmux'), '-S', str(self.root / 'mouse-tmux.sock')]
        self.addCleanup(subprocess.run, [*tmux, 'kill-server'],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        child = self.start('-N', 'mouse-tmux', transport=[*tmux, '-f', '/dev/null',
                           'new-session', '-s', 'mouse', '--'])
        child.command('history ' + journal.parent.name)
        self.wait_snapshot(lambda rows: rows and
            next(iter(rows.values()))['state']['windows'][0].get('history', {}).get('seq', 0) > 0)
        child.write(b'/tmux-marker\r')
        self.wait_snapshot(lambda rows: rows and
            next(iter(rows.values()))['state']['windows'][0]['history']['byte'] == 0 and
            not next(iter(rows.values()))['state']['windows'][0]['history']['follow'])
        deadline = time.monotonic() + 5
        where = None
        while where is None and time.monotonic() < deadline:
            child.read(.05)
            frame = subprocess.run([*tmux, 'capture-pane', '-p', '-t', 'mouse:0.0'],
                                   capture_output=True, text=True, check=True, timeout=5).stdout
            for row, line in enumerate(frame.splitlines()):
                if line.startswith('tmux-marker'):
                    where = row, line.index('tmux-marker') + 5
        self.assertIsNotNone(where)
        mouse(child, *where)
        mouse(child, *where, release=True)
        child.write(b'ywP')
        self.assertEqual(rollout(self.save(child)['buffers'][0])['draft'], 'marker ')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()


class ReportMouseTests(unittest.TestCase):
    setUp = reports.ReportTests.setUp
    start = reports.ReportTests.start
    snapshots = reports.ReportTests.snapshots
    wait_snapshot = reports.ReportTests.wait_snapshot
    inputs = reports.ReportTests.inputs
    attached = reports.ReportTests.attached
    save = MouseTests.save

    def test_report_drag_copies_without_owner_input(self):
        child = self.attached('mouse-report')
        child.write(b'i/status\r')
        child.repaint_until(b'REPORT')
        child.write(b'gg')
        row, column = position(child, '/status')
        before = self.owner.journal.read_bytes()
        mouse(child, row, column + 1)
        mouse(child, row, column + 6, button=32)
        mouse(child, row, column + 6, release=True)
        child.write(b'yP')
        self.assertEqual(rollout(self.save(child)['buffers'][0])['draft'], 'status')
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.journal.read_bytes(), before)
        child.finish('q!')


if __name__ == '__main__':
    unittest.main()
