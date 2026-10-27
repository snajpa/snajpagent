# SPDX-License-Identifier: GPL-2.0-only
"""Compare the real standalone and embedded frontend in real tmux terminals."""

import json
import base64
import os
import re
import shlex
import signal
import socket
import subprocess
import shutil
import sys
import threading
import time
import unittest
from pathlib import Path

import test_session_view as owners
from tmux_terminal import TmuxTerminal


class FrontendChannel(owners.View):
    """Reuse SV framing without the session controller's handshake."""
    def __init__(self, peer):
        self.peer = peer
        self.states = []


@unittest.skipUnless(shutil.which('tmux') and os.name == 'posix', 'requires tmux and PTYs')
class NativePaneTests(unittest.TestCase):
    def setUp(self):
        self.maxDiff = None
        self.owner = owners.SessionViewTests()
        binary = owners.BINARY
        try:
            if self._testMethodName == 'test_older_owner_refuses_uninspected_drafts':
                owners.BINARY = Path(os.environ['SNAJPAGENT_VM_CLASSIC_OWNER_BINARY']).resolve()
            self.owner.setUp()
        finally:
            owners.BINARY = binary
        self.addCleanup(self.owner.tearDown)
        self.root = self.owner.root
        config = self.owner.config.read_text().replace('color = never', 'color = always')
        prompt = 'EXACT-{model}-{chat:C>}{rollout-idle:PROMPT>}{rollout-active:A>} '
        if self._testMethodName == 'test_private_frontend_editor_input_and_channel_lifetime':
            prompt += '{hour:02}:{minute:02}:{second:02} '
        self.owner.config.write_text(config + 'prompt = ' + prompt + '\n')
        os.write(self.owner.initial.master, b'/configure\r')
        self.owner.initial.until(b'configuration reloaded:')
        self.owner.detach()
        self.launcher = self.root / 'workspace.sh'
        self.launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(owners.BINARY)) +
                                 ' vm "$@"\n')
        self.launcher.chmod(0o700)
        self.terminals = []
        self.addCleanup(self.close_terminals)

    def close_terminals(self):
        for terminal in reversed(self.terminals):
            terminal.close()

    def start(self, pane, columns=100, rows=24):
        name = ('pane' if pane else 'standalone') + '-' + str(len(self.terminals))
        terminal = TmuxTerminal(self.root / name,
            self.launcher if pane else owners.BINARY, self.root, self.root / 'state',
            None, columns, rows + int(pane),
            args=('--session', self.owner.sid) if pane else ('--attach', self.owner.sid),
            environment=self.owner.env)
        self.terminals.append(terminal)
        terminal.wait('EXACT-host-model-PROMPT>')
        self.owner.status('attached')
        return terminal

    def screen(self, terminal, pane):
        # ANSI captures serialize allocated line extents, so even -N cannot
        # compare an erased cell with a printed blank. Inspect the actual grid,
        # including blank-cell attributes, colors, underline and hyperlink IDs.
        try:
            dump = terminal.run('capture-pane', '-p', '-R', '-t', terminal.target)
        except AssertionError as error:
            if 'unknown flag' not in str(error):
                raise
            self.skipTest('screen-cell parity requires tmux capture-pane -R')
        cells = {}
        row = None
        for line in dump.splitlines():
            if line.startswith('\tL '):
                match = re.match(r'\s*L \d+ \((\d+)\)', line)
                row = int(match[1]) if match else None
            match = re.match(r'\s*C \d+,(\d+) data=(\(.*\)) flags=(\S+) (.*)', line)
            if row is None or row >= terminal.rows - int(pane) or not match:
                continue
            data, flags, attributes = match[2], match[3], match[4]
            # tmux can represent a wide character's continuation with either
            # a padding cell or a zero-length extended cell. Neither is text.
            if 'PADDING' in flags or data == '(0,0,)':
                data = 'continuation'
            cells[row, int(match[1])] = data, attributes
        self.assertEqual(len(cells), terminal.cols * (terminal.rows - int(pane)))
        return cells

    def settled_screen(self, terminal, pane=False):
        # A draft checkpoint acknowledges input, not the asynchronous resize paint.
        deadline = time.monotonic() + 5
        previous = None
        changed = time.monotonic()
        while time.monotonic() < deadline:
            cells = self.screen(terminal, pane)
            now = time.monotonic()
            if cells != previous:
                previous, changed = cells, now
            elif now - changed >= .2:
                return cells
            time.sleep(.02)
        kind = 'pane' if pane else 'standalone'
        self.fail(f'{kind} screen did not settle:\n' + terminal.capture())

    def assert_frame(self, terminal, expected, offset=0, context=''):
        deadline = time.monotonic() + 5
        while True:
            cells = self.screen(terminal, True)
            differences = [(position, wanted, cells.get((position[0], position[1] + offset)))
                           for position, wanted in expected.items()
                           if cells.get((position[0], position[1] + offset)) != wanted]
            if not differences or time.monotonic() >= deadline:
                break
            time.sleep(.02)
        self.assertEqual(differences[:12], [],
                         f'{context}: {len(differences)} cells differ\n' + terminal.capture())

    def checkpoint(self, terminal):
        self.checkpoint_id = getattr(self, 'checkpoint_id', 122) + 1
        prefix = f'SNAJPAGENT-SCREEN/1:DRAFT:{self.checkpoint_id}:'
        terminal.send_text(f'\x1b[9004;{self.checkpoint_id}~')
        title = ''
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            title = terminal.run('display-message', '-p', '-t', terminal.target,
                                 '#{pane_title}').strip()
            if title.startswith(prefix):
                break
            time.sleep(.02)
        self.assertTrue(title.startswith(prefix), title)
        # tmux quotes backslashes in retained OSC titles.
        return json.loads(title.split(':', 3)[3].replace('\\\\', '\\'))

    def exercise(self, terminal, pane):
        def capture(expected):
            cells = self.settled_screen(terminal, pane)
            if not pane:
                saved = self.checkpoint(terminal)
                draft = next(row for row in saved['drafts'] if row['route'] == 'rollout')
                self.assertEqual(draft['text'], expected)
                self.assertEqual(draft['cursor'], len(expected.encode()))
            cursor = terminal.run('display-message', '-p', '-t', terminal.target,
                                  '#{cursor_x} #{cursor_y}')
            return cells, cursor

        frames = []
        for text in ('same draft', 'é 中 é', '👩‍💻X', '👨‍👩‍👧‍👦X', 'e' + '́' * 12 + 'X',
                     'wrapped ' * 40 + 'tail-six'):
            terminal.send_text(text)
            terminal.wait(text[-8:], join_wrapped=True)
            frames.append(capture(text))
            terminal.send_key('C-a')
            terminal.send_text('prefix ')
            terminal.send_key('C-e')
            terminal.send_text(' end')
            frames.append(capture('prefix ' + text + ' end'))
            terminal.send_key('C-u')
        return frames

    def test_private_frontend_editor_input_and_channel_lifetime(self):
        # A visible editor taller than the terminal gives both paths the same
        # complete grid without introducing a second transcript decoder here.
        for columns in (42, 78):
            standalone = self.start(False, columns=columns)
            initial = self.screen(standalone, False)
            with self.subTest(columns=columns):
                observer = owners.View(self.owner.directory)
                try:
                    state = observer.until('state')['state']
                    prompt = state['prompt']
                finally:
                    observer.close()
                self.assertEqual(prompt['mode'], 1)
                retained = json.loads(subprocess.check_output(
                    [str(owners.BINARY.parent / 'tests/test_vm_terminal_wire'), 'presentation',
                     str(self.owner.directory / '.view-presentation.snb'), self.owner.sid],
                    timeout=5))
                records = [row['data'] for row in retained]
                boundary = max(index for index, record in enumerate(records)
                               if record.get('op') == 'host' and
                               record.get('text', '').startswith('Attached session '))
                records = records[boundary:]
                self.assertTrue(any(record['op'] == 'history' for record in records))
                # A fresh local clock must differ; the frontend restores the
                # captured owner clock instead of observing the current time.
                time.sleep(1.1)
                draft = 'wrapped words ' * 220 + f'tail-{columns}-é-中-👩‍💻'
                standalone.send_text(draft)
                standalone.wait(f'tail-{columns}', join_wrapped=True)
                standalone.send_key('C-a')
                standalone.send_text('prefix-')
                standalone.send_key('C-e')
                standalone.send_text('-end')
                saved = self.checkpoint(standalone)
                rollout = next(row for row in saved['drafts'] if row['route'] == 'rollout')
                expected_draft = 'prefix-' + draft + '-end'
                self.assertEqual(rollout['cursor'], len(expected_draft.encode()))
                self.assertEqual(rollout['text'], expected_draft)
                standalone.wait('-end', join_wrapped=True)
                time.sleep(.1)
                expected = self.screen(standalone, False)
                expected_text = standalone.capture()
                cursor = standalone.run('display-message', '-p', '-t', standalone.target,
                                        '#{cursor_x} #{cursor_y}')

                path = self.root / f'frontend-{columns}.sock'
                launcher = self.root / f'frontend-{columns}.py'
                launcher.write_text(
                    '#!' + sys.executable + '\n'
                    'import os, socket\n'
                    'peer = socket.socket(socket.AF_UNIX)\n'
                    f'peer.connect({str(path)!r})\n'
                    'peer.set_inheritable(True)\n'
                    f'os.execv({str(owners.BINARY)!r}, '
                    f'[{str(owners.BINARY)!r}, "--internal-ui-frontend", '
                    'str(peer.fileno())])\n')
                launcher.chmod(0o700)
                with socket.socket(socket.AF_UNIX) as listener:
                    listener.bind(str(path))
                    listener.listen(1)
                    listener.settimeout(5)
                    frontend = TmuxTerminal(self.root / f'frontend-{columns}', launcher,
                        self.root, self.root / 'state', None, columns, 24,
                        environment=self.owner.env)
                    self.terminals.append(frontend)
                    peer, _ = listener.accept()
                peer.settimeout(5)
                channel = FrontendChannel(peer)
                self.addCleanup(channel.close)
                channel.send(type='frame', serial=1, dotdir=str(self.root / 'state'),
                             session=self.owner.sid, state=state, turns=1,
                             drafts=[dict(route='rollout', text='', cursor=0)])
                self.assertEqual(channel.receive(), dict(type='frame', serial=1))
                frontend.wait('EXACT-host-model-PROMPT>')
                time.sleep(.1)
                actual = self.screen(frontend, False)
                differences = [(position, initial[position], actual.get(position))
                               for position in initial if actual.get(position) != initial[position]]
                self.assertEqual(differences[:12], [], f'{len(differences)} initial cells differ')
                time.sleep(1.1)
                channel.send(type='frame', serial=2, drafts=[])
                self.assertEqual(channel.receive(), dict(type='frame', serial=2))
                self.assertEqual(self.screen(frontend, False), actual)
                # Audio uses the same prompt and caption renderer. Control
                # input must survive priority delivery with its original context.
                state['audio'] = dict(label='[MIC ON; Enter finishes; Esc cancels] ',
                    dictating=True, captions=['heard', 'spoken'], epoch=19)
                channel.send(type='frame', serial=3, state=state, drafts=[])
                while True:
                    audio_ack = channel.receive()
                    if audio_ack.get('type') == 'frame':
                        break
                self.assertEqual(audio_ack.get('audio'), 19)
                frontend.wait('heard')
                frontend.wait('spoken')
                frontend.wait('[MIC ON;', join_wrapped=True)
                frontend.send_key('Enter')
                while True:
                    dictated = channel.receive()
                    if dictated['type'] == 'input':
                        break
                self.assertEqual(dictated['action'], 7)
                self.assertEqual(dictated['verbosity'], state['frontend']['verbosity'])
                self.assertGreater(dictated['capture']['received'], 0)
                state['audio'] = None
                level = state['frontend']['verbosity']
                # Recurring owner frames carry shared settings, not a command
                # changing this independent frontend's presentation level.
                state['frontend']['verbosity'] = (level + 3) % 7
                channel.send(type='frame', serial=4, state=state, drafts=[])
                while channel.receive().get('type') != 'frame':
                    pass
                self.assertEqual(self.checkpoint(frontend)['verbosity'], level)
                state['frontend']['verbosity'] = level
                self.assertNotIn('[MIC ON;', frontend.capture())
                self.assertNotIn('heard', frontend.capture())
                frontend.send_text(draft)
                frontend.wait(f'tail-{columns}', join_wrapped=True)
                frontend.send_key('C-a')
                frontend.send_text('prefix-')
                frontend.send_key('C-e')
                frontend.send_text('-end')
                frontend.wait('-end', join_wrapped=True)
                time.sleep(.1)
                actual = self.screen(frontend, False)
                differences = [(position, expected[position], actual.get(position))
                               for position in expected if actual.get(position) != expected[position]]
                self.assertEqual(differences[:12], [], f'{len(differences)} editor cells differ\n'
                                 f'expected={expected_text!r}\nactual={frontend.capture()!r}')
                self.assertEqual(frontend.run('display-message', '-p', '-t', frontend.target,
                                 '#{cursor_x} #{cursor_y}'), cursor)
                snapshot = self.checkpoint(frontend)
                self.assertTrue(snapshot['frontend'])
                self.assertNotIn('instance', snapshot)
                self.assertFalse(snapshot['unrouted'])
                self.assertEqual([{key: row[key] for key in ('route', 'text', 'cursor')}
                                  for row in snapshot['drafts']], [{'route': 'rollout',
                    'text': expected_draft, 'cursor': len(expected_draft.encode())}])
                self.assertTrue(snapshot['drafts'][0]['history']['undo'])
                self.assertEqual(snapshot['drafts'][0]['history']['redo'], [])
                frontend.send_key('Enter')
                latest = None
                while True:
                    submitted = channel.receive()
                    if submitted['type'] != 'editors':
                        break
                    latest = submitted['snapshot']
                    self.assertTrue(latest['frontend'])
                    self.assertFalse(latest['unrouted'])
                    self.assertNotIn('instance', latest)
                self.assertIsNotNone(latest, 'the frontend must retain editing before submission')
                self.assertEqual(latest['drafts'], [{'route': 'rollout',
                    'text': expected_draft, 'cursor': len(expected_draft.encode())}])
                self.assertEqual(submitted['type'], 'input')
                self.assertEqual(submitted['action'], 1)
                self.assertEqual(submitted['text'], 'prefix-' + draft + '-end')
                self.assertIsNone(submitted['route'])
                self.assertEqual(submitted['verbosity'], state['frontend']['verbosity'])
                captured = submitted['capture']
                self.assertGreater(captured['received'], 0)
                self.assertTrue(captured['label'].startswith('EXACT-host-model-PROMPT> '))
                self.assertEqual(captured['view'], 1)
                self.assertFalse(captured['active'])
                self.assertEqual(captured['verbosity'], submitted['verbosity'])
                self.assertEqual(captured['scopes'], [])
                self.assertEqual(captured['targets'], [])
                self.assertIsNone(captured['conversation'])
                channel.send(type='output', record={'op': 'host', 'text': 'FRONTEND-OUTPUT',
                             'label': None, 'data': None})
                channel.send(type='ready')
                frontend.wait('FRONTEND-OUTPUT')
                frontend.send_text('ready-input')
                frontend.wait('ready-input')
                ready = self.checkpoint(frontend)
                self.assertEqual(next(row['text'] for row in ready['drafts']
                                      if row['route'] == 'rollout'), 'ready-input')
                frontend.send_key('C-u')
                standalone.send_key('C-u')
                standalone.send_text('/configure')
                standalone.send_key('Enter')
                standalone.wait('configuration reloaded:')
                observer = owners.View(self.owner.directory)
                try:
                    updated = observer.until('state')['state']
                finally:
                    observer.close()
                channel.send(type='state', state=updated)
                channel.send(type='prompt', **updated['prompt'])
                frontend.wait('configuration reloaded:')
                before = frontend.capture()
                channel.send(type='state', state=updated)
                self.checkpoint(frontend)
                self.assertEqual(frontend.capture().count('configuration reloaded:'),
                                 before.count('configuration reloaded:'))
                self.owner.status('attached')
                self.assertEqual(self.owner.identity(), self.owner.owner_identity)
                # The real frontend needs the owner's destination epochs and
                # exact conversation identities before it can capture chat input.
                updated['destinations'] = [dict(target=[7, 19], connection='1' * 32,
                    generation=23, names_revision=29, endpoint='fixture:6667', room='#room',
                    model='agent', operator='operator', casemapping=[0, 0],
                    chantypes=['#&', '#&'], nicks='Alice Bob', joined=True, connecting=False)]
                route = dict(connection='1' * 32, conversation='2' * 32, generation=23,
                    identity='operator', room='#room', membership='3' * 32,
                    endpoint='fixture:6667', casemapping=0)
                updated['channels'] = [dict(route=route, endpoint='fixture:6667',
                    connected=True, joined=True, seq=1, activity=None)]
                channel.send(type='frame', serial=5, state=updated, drafts=[])
                while channel.receive().get('type') != 'frame':
                    pass
                frontend.send_key('Tab')
                frontend.wait('EXACT-host-model-C>')
                frontend.send_text('captured channel text')
                frontend.wait_until(lambda screen: 'captured channel text' in
                    ' '.join(screen.split()), 'captured channel editor', join_wrapped=True)
                captured_editors = self.checkpoint(frontend)
                self.assertFalse(captured_editors['unrouted'])
                captured = next(row for row in captured_editors['drafts']
                                if row['route'] == route)
                self.assertEqual(captured['text'], 'captured channel text')
                self.assertEqual(captured['cursor'], 21)
                self.assertEqual(set(captured['history']), {'undo', 'redo'})
                frontend.send_key('Enter')
                while True:
                    submitted = channel.receive()
                    if submitted['type'] == 'input':
                        break
                self.assertEqual(submitted['route'], route)
                self.assertEqual(submitted['capture']['selection'], [7, 19])
                self.assertEqual(submitted['capture']['conversation']['route'], route)
                self.assertEqual(submitted['capture']['scopes'][0]['generation'], 23)
                self.assertEqual(submitted['capture']['scopes'][0]['connection'], '1' * 32)
                updated['prompt']['active'] = True
                updated['prompt']['generation'] = 42
                updated['prompt']['states'] |= 1 << 2
                channel.send(type='verbosity', value=3)
                channel.send(type='frame', serial=6, state=updated, drafts=[])
                while channel.receive().get('type') != 'frame':
                    pass
                frontend.submit('/yield')
                while True:
                    yielded = channel.receive()
                    if yielded['type'] == 'input':
                        break
                self.assertEqual(yielded['text'], '/yield')
                self.assertEqual(yielded['verbosity'], 3)
                self.assertTrue(yielded['capture']['active'])
                self.assertEqual(yielded['capture']['turn'], 42)
                self.assertGreater(yielded['capture']['received'], 0)
                self.assertTrue(yielded['capture']['label'])
                frontend.send_key('C-c')
                while True:
                    interrupted = channel.receive()
                    if interrupted['type'] == 'input':
                        break
                self.assertEqual(interrupted['action'], 5)
                self.assertEqual(interrupted['verbosity'], 3)
                channel.send(type='prompt', **updated['prompt'])
                frontend.submit('/verbose 4')
                while True:
                    local = channel.receive()
                    if local['type'] == 'input':
                        break
                self.assertTrue(local['local'])
                self.assertEqual(local['text'], '/verbose 4')
                self.assertEqual(local['verbosity'], 4)
                self.assertEqual(local['capture']['verbosity'], 4)
                self.assertTrue(local['capture']['label'])
                if columns == 42:
                    channel.send(type='stop')
                else:
                    channel.close()
                frontend.wait_dead()
                self.assertEqual(frontend.run('display-message', '-p', '-t', frontend.target,
                                 '#{pane_dead_status}').strip(), '0' if columns == 42 else '2')
                standalone.send_key('C-u')
                standalone.submit('/s d')
                self.owner.status('detached')

    def test_prompt_and_editor_are_the_standalone_screen(self):
        standalone = self.start(False)
        expected = self.exercise(standalone, False)
        self.checkpoint(standalone)
        standalone.close()
        self.owner.status('detached')
        pane = self.start(True)
        actual = self.exercise(pane, True)
        self.assertEqual(len(actual), len(expected))
        for index, ((cells, cursor), (wanted, wanted_cursor)) in enumerate(zip(actual, expected)):
            differences = [(position, wanted[position], cells.get(position))
                           for position in wanted if cells.get(position) != wanted[position]]
            self.assertEqual(differences[:12], [], f'frame {index}: {len(differences)} cells differ')
            self.assertEqual(cursor, wanted_cursor, f'frame {index} cursor')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_insert_entry_motions_preserve_the_standalone_editor(self):
        def exercise(terminal, pane):
            terminal.send_text('  suffix')
            terminal.wait('suffix')
            if pane:
                terminal.send_key('Escape')
                terminal.send_text('Iprefix ')
            else:
                terminal.send_key('C-a')
                terminal.send_key('Right')
                terminal.send_key('Right')
                terminal.send_text('prefix ')
            terminal.wait('  prefix suffix')
            if pane:
                terminal.send_key('Escape')
                terminal.send_text('A!')
            else:
                terminal.send_key('C-e')
                terminal.send_text('!')
            terminal.wait('suffix!')
            terminal.send_key('C-a')
            for _ in range(9):
                terminal.send_key('Right')
            if pane:
                terminal.send_key('Escape')
                terminal.send_text('aX')
            else:
                terminal.send_key('Right')
                terminal.send_text('X')
            terminal.wait('sXuffix!')
            time.sleep(.1)
            return self.screen(terminal, pane), terminal.run(
                'display-message', '-p', '-t', terminal.target, '#{cursor_x} #{cursor_y}')

        standalone = self.start(False)
        expected = exercise(standalone, False)
        standalone.send_key('C-u')
        self.checkpoint(standalone)
        standalone.close()
        self.owner.status('detached')
        pane = self.start(True)
        self.assertEqual(exercise(pane, True), expected)
        self.assertEqual(self.owner.provider.requests, [])

    def test_splits_share_one_frontend_and_close_preserves_owner(self):
        pane = self.start(True)
        draft = 'shared-draft👨‍👩‍👧‍👦 e' + '́' * 12
        pane.send_text(draft)
        pane.wait('shared-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':vsp')
        pane.wait_until(lambda screen: screen.count('shared-draft') == 2, 'mirrored draft')
        self.owner.status('attached')
        pane.resize(140, 30)
        pane.wait_until(lambda screen: screen.count('shared-draft') == 2, 'resized mirrors')
        pane.submit(':close')
        pane.wait('shared-draft')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        standalone = self.start(False)
        standalone.wait('shared-draft')
        # tmux may truncate a cell's combining marks; inspect the complete draft.
        saved = self.checkpoint(standalone)
        restored = next(row for row in saved['drafts'] if row['route'] == 'rollout')
        self.assertEqual(restored['text'], draft)
        self.assertEqual(restored['cursor'], len(draft.encode()))
        standalone.send_key('C-u')
        standalone.wait_until(lambda screen: 'PROMPT> shared-draft' not in screen,
                              'restored draft cleared')
        standalone.submit('/s d')
        self.owner.status('detached')

    def test_same_resize_sequence_matches_standalone(self):
        draft = 'resize-sequence ' + 'wrapped words ' * 9
        expected = []
        for native in (False, True):
            terminal = self.start(native, columns=121)
            terminal.send_text(draft)
            terminal.wait('resize-sequence')
            for index, columns in enumerate((60, 42, 78, 121)):
                terminal.resize(columns, 24 + int(native))
                terminal.wait('resize-sequence')
                if native:
                    self.assert_frame(terminal, expected[index], context=f'resize to {columns}')
                else:
                    self.checkpoint(terminal)
                    expected.append(self.settled_screen(terminal))
            if native:
                terminal.send_key('Escape')
                terminal.submit(':qa')
            else:
                terminal.send_key('C-u')
                self.checkpoint(terminal)
                terminal.close()
            self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_unequal_mirrors_have_the_standalone_frame_at_each_size(self):
        self.unequal_mirror_frame(121, (60, 54, 48, 42), 42, 0)

    def test_unequal_right_mirror_has_the_standalone_frame(self):
        self.unequal_mirror_frame(60, (66, 72, 78), 78, 43)

    def unequal_mirror_frame(self, initial, widths, columns, offset):
        draft = 'unequal-mirror ' + 'wrapped words ' * 9
        standalone = self.start(False, columns=initial)
        standalone.send_text(draft)
        standalone.wait('unequal-mirror')
        for width in widths:
            standalone.resize(width, 24)
            self.checkpoint(standalone)
            expected = self.settled_screen(standalone)
        standalone.send_key('C-u')
        self.checkpoint(standalone)
        standalone.close()
        self.owner.status('detached')
        pane = self.start(True, columns=121)
        pane.send_text(draft)
        pane.wait('unequal-mirror')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':vsplit')
        time.sleep(.15)
        self.assertEqual(pane.capture().splitlines()[0].index('│'), 60)
        for left in (54, 48, 42):
            pane.send_key('C-w')
            pane.send_text('>')
            time.sleep(.15)
            self.assertEqual(pane.capture().splitlines()[0].index('│'), left)
        for focus, column in ((2, None), (1, 2), (2, 45)):
            if column is not None:
                pane.send_text(f'\033[<0;{column};2M\033[<0;{column};2m')
                deadline = time.monotonic() + 5
                while True:
                    paths = list((self.root / 'state/workspaces').glob('*/workspace.json'))
                    if len(paths) == 1 and json.loads(paths[0].read_text())['state']['focus'] == focus:
                        break
                    self.assertLess(time.monotonic(), deadline, 'mouse focus was not saved')
                    time.sleep(.02)
                time.sleep(.15)
            self.assert_frame(pane, expected, offset, f'focus {focus}, {columns}-column mirror')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_history_navigation_preserves_the_terminal_scrollback(self):
        text = ('scrollback-begin\n' +
                '\n'.join(f'scrollback-row-{i:03d}' for i in range(80)) +
                '\nscrollback-tail')
        provider = self.owner.provider
        provider.runtime_handler = lambda handler, request, sequence: provider.reply(
            handler, provider.response_body(sequence, text).encode())
        pane = self.start(True)
        pane.send_text('render scrollback\r')
        pane.wait('scrollback-tail')
        self.assertNotIn('scrollback-begin', pane.capture())
        pane.send_key('Escape')
        time.sleep(.08)
        pane.send_text('gg')
        pane.wait('scrollback-begin')
        pane.send_text('G')
        pane.wait('scrollback-tail')
        pane.send_text('\033[<64;10;4M' * 35)
        pane.wait('scrollback-begin')
        pane.send_text('\033[<65;10;4M' * 35)
        pane.wait('scrollback-tail')
        pane.send_text('iunsent-draft')
        pane.wait('EXACT-host-model-PROMPT> unsent-draft')
        pane.send_text('\033[<64;10;4M' * 35)
        pane.wait('scrollback-begin')
        pane.send_text('-typed')
        pane.wait('scrollback-begin')
        self.assertNotIn('EXACT-host-model-PROMPT> unsent-draft-typed', pane.capture())
        pane.send_text('\033[<0;10;4M\033[<0;10;4m')
        pane.wait('scrollback-begin')
        self.assertNotIn('EXACT-host-model-PROMPT> unsent-draft-typed', pane.capture())
        pane.send_key('Escape')
        time.sleep(.08)
        pane.send_text('i')
        pane.wait('EXACT-host-model-PROMPT> unsent-draft-typed')
        self.assertEqual(len(provider.requests), 1)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_history_position_stays_on_its_rows_when_output_arrives(self):
        provider = self.owner.provider
        waiting, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)

        def respond(handler, request, sequence):
            if sequence > 1:
                waiting.set()
                release.wait(10)
            text = ('anchor-begin\n' +
                    '\n'.join(f'anchor-row-{i:03d}' for i in range(80)) +
                    '\nanchor-tail') if sequence == 1 else 'new-output\n' * 40 + 'new-tail'
            provider.reply(handler, provider.response_body(sequence, text).encode())

        provider.runtime_handler = respond
        pane = self.start(True)
        pane.submit('initial history')
        pane.wait('anchor-tail')
        previous = self.owner.events()[-1]['seq']
        pane.submit('append while reading')
        self.assertTrue(waiting.wait(5))
        pane.send_key('Escape')
        time.sleep(.08)
        pane.send_text('gg')
        pane.wait('anchor-begin')
        pane.send_key('C-f')
        pane.wait_until(lambda screen: screen.splitlines()[0].lstrip().startswith('anchor-row-'),
                        'a history row at the top of the viewport')
        before = pane.capture().splitlines()[:6]
        release.set()
        self.owner.wait_event('turn_completed', previous)
        time.sleep(.2)
        self.assertEqual(pane.capture().splitlines()[:6], before)
        pane.send_text('G')
        pane.wait('new-tail')
        self.assertEqual(len(provider.requests), 2)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_completed_markdown_output_has_the_standalone_frame(self):
        text = ('Long output before visible tail.\n' * 30 +
                '# Render parity\n\n**bold** _italic_ ~~strike~~ and `inline`\n\n' +
                '| Name | Value |\n| :--- | ---: |\n| wide | 中 |\n\n' +
                '> quote with 👨‍👩‍👧‍👦 and é\n\n```python\nprint("copy me")\n```\n\n' +
                'render-parity-tail')
        provider = self.owner.provider
        provider.runtime_handler = lambda handler, request, sequence: provider.reply(
            handler, provider.response_body(sequence, text).encode(), close_header=True)
        frames = []
        for pane in (False, True):
            terminal = self.start(pane)
            before = max(event['seq'] for event in self.owner.events())
            terminal.submit('render parity')
            self.owner.wait_event('turn_completed', after_seq=before)
            terminal.wait('render-parity-tail')
            # A reattachment already contains the preceding turn's tail and prompt.
            terminal.wait_until(lambda screen: screen.splitlines()[-1 - int(pane)].strip() ==
                                'EXACT-host-model-PROMPT>', 'completed turn idle prompt')
            frames.append((self.settled_screen(terminal, pane), terminal.run(
                'display-message', '-p', '-t', terminal.target, '#{cursor_x} #{cursor_y}')))
            if pane:
                terminal.send_key('Escape')
                time.sleep(.08)
                terminal.submit(':qa')
            else:
                terminal.submit('/s d')
            try:
                self.owner.status('detached')
            except AssertionError as error:
                self.fail(str(error) + '\n' + terminal.capture())
        self.assertEqual(frames[1], frames[0])
        self.assertEqual(len(provider.requests), 2)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_active_stream_and_animation_have_the_standalone_frames(self):
        config = self.owner.config.read_text().replace('prompt = EXACT-',
                                                       'prompt = {activity_spinner}EXACT-')
        self.owner.config.write_text(config + 'prompt_spinner_provider = " ABCD"\n')
        peer = owners.View(self.owner.directory)
        try:
            peer.bind()
            self.assertEqual(peer.result(peer.command('/configure'))['status'], 'completed')
            peer.send(type='detach', generation=peer.generation)
            peer.until('detached')
        finally:
            peer.close()
        provider = self.owner.provider
        prefix = 'active stream line\n' * 60 + 'active-parity-tail\n'
        captures = []
        for pane in (False, True):
            release = threading.Event()
            self.addCleanup(release.set)

            def respond(handler, request, sequence):
                response_id = 'resp_active_' + str(sequence)
                item = {'type': 'message', 'id': 'active_item', 'role': 'assistant',
                        'phase': 'final_answer', 'status': 'in_progress', 'content': []}
                position = {'item_id': item['id'], 'output_index': 0, 'content_index': 0}
                handler.send_response(200)
                handler.send_header('Content-Type', 'text/event-stream')
                handler.send_header('Connection', 'close')
                handler.end_headers()

                def send(kind, data):
                    handler.wfile.write(provider.event(kind, data).encode())
                    handler.wfile.flush()

                send('response.created', {'response': {'id': response_id,
                     'status': 'in_progress', 'output': []}})
                send('response.output_item.added', {'output_index': 0, 'item': item})
                send('response.content_part.added', {**position,
                     'part': {'type': 'output_text', 'text': '', 'annotations': []}})
                send('response.output_text.delta', {**position, 'delta': prefix})
                release.wait(10)
                send('response.output_text.done', {**position, 'text': prefix})
                item.update(status='completed', content=[{'type': 'output_text',
                            'text': prefix, 'annotations': []}])
                send('response.output_item.done', {'output_index': 0, 'item': item})
                send('response.completed', {'response': {'id': response_id,
                     'status': 'completed', 'output': [],
                     'usage': {'input_tokens': 10, 'output_tokens': 10, 'total_tokens': 20}}})

            provider.runtime_handler = respond
            terminal = self.start(pane)
            previous = self.owner.events()[-1]['seq']
            terminal.submit('animate identical output')
            # Reattachment already contains the preceding turn's tail. Wait
            # for the new stream's tail at the active-output position.
            terminal.wait_until(lambda screen: screen.splitlines()[-3 - int(pane)].strip() ==
                                'active-parity-tail' and 'EXACT-host-model-A>' in
                                screen.splitlines()[-1 - int(pane)], 'current active stream')
            frames = set()
            # Screen queries take time; a fixed sampling interval can alias
            # every second animation frame. Observe several complete cycles.
            # Stay within the fixture's five-second provider request timeout.
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                time.sleep(.01)
                frames.add((tuple(sorted(self.screen(terminal, pane).items())), terminal.run(
                    'display-message', '-p', '-t', terminal.target, '#{cursor_x} #{cursor_y}')))
            self.assertEqual(len(frames), 4, 'all configured animation frames must appear')
            captures.append(frames)
            release.set()
            self.owner.wait_event('turn_completed', previous)
            terminal.wait('EXACT-host-model-PROMPT>')
            if pane:
                terminal.send_key('Escape')
                time.sleep(.08)
                terminal.submit(':qa')
            else:
                terminal.submit('/s d')
            self.owner.status('detached')
        self.assertEqual(captures[1], captures[0])
        self.assertEqual(len(provider.requests), 2)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_busy_owner_is_not_stolen_and_retry_uses_the_frontend(self):
        standalone = self.start(False)
        standalone.send_text('protected-draft')
        standalone.wait('protected-draft')
        pane = TmuxTerminal(self.root / 'busy', self.launcher, self.root,
            self.root / 'state', None, 100, 25,
            args=('--session', self.owner.sid), environment=self.owner.env)
        self.terminals.append(pane)
        pane.wait_until(lambda screen: 'session already has' in
                        screen.lower().replace('\n', ''),
                        'standalone attachment refusal')
        standalone.send_text('-still-controlled')
        standalone.wait('protected-draft-still-controlled')
        self.owner.status('attached')
        standalone.send_key('C-u')
        standalone.submit('/s d')
        standalone.wait_dead()
        try:
            self.owner.status('detached')
        except AssertionError as error:
            self.fail(f'{error}\nRefused workspace: {pane.capture()}')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':attach')
        pane.wait('EXACT-host-model-PROMPT>')
        self.owner.status('attached')
        pane.send_text('retry-draft')
        pane.wait('retry-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_session_quit_stops_the_owner(self):
        pane = self.start(True)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':session quit')
        self.owner.status('stored')
        self.assertEqual(self.owner.provider.requests, [])

    @unittest.skipUnless(os.environ.get('SNAJPAGENT_VM_CLASSIC_OWNER_BINARY'),
                         'requires a retained older owner binary')
    def test_older_owner_refuses_uninspected_drafts(self):
        observer = owners.View(self.owner.directory)
        try:
            self.assertNotIn('terminal_drafts', observer.capabilities['features'])
        finally:
            observer.close()
        pane = TmuxTerminal(self.root / 'older-owner', self.launcher, self.root,
            self.root / 'state', None, 100, 25,
            args=('--session', self.owner.sid), environment=self.owner.env)
        self.terminals.append(pane)
        pane.wait('Owner cannot supply the standalone frontend state')
        self.owner.status('attached')
        # Missing presentation state cannot be replaced by a reconstructed editor.
        pane.send_text('i')
        pane.wait('Owner cannot supply the standalone frontend state')
        pane.submit(':session quit')
        pane.wait('Owner cannot inspect terminal drafts')
        self.owner.status('attached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.submit(':session quit!')
        self.owner.status('stored')
        self.assertEqual(self.owner.provider.requests, [])

    def test_semantic_controller_draft_survives_terminal_takeover(self):
        peer = owners.View(self.owner.directory)
        try:
            peer.bind()
            draft = peer.draft()
            peer.replace_draft(draft['revision'], 'semantic-draft-for-terminal')
            peer.send(type='detach', generation=peer.generation)
            peer.until('detached')
        finally:
            peer.close()
        self.owner.status('detached')
        pane = self.start(True)
        pane.wait('semantic-draft-for-terminal')
        pane.send_text('-edited')
        pane.wait('semantic-draft-for-terminal-edited')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_all_session_forced_shutdown_saves_native_drafts(self):
        pane = self.start(True)
        pane.send_text('retained-after-all-session-shutdown')
        pane.wait('retained-after-all-session-shutdown')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':sessions quit!')
        self.owner.status('stored')
        deadline = time.monotonic() + 5
        saved = None
        drafts = []
        while time.monotonic() < deadline:
            snapshots = list((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
            if snapshots:
                saved = json.loads(snapshots[0].read_text())['state']
                drafts = [buffer['draft'] for connection in saved['buffers']
                          for buffer in connection['buffers']]
                if 'retained-after-all-session-shutdown' in drafts:
                    break
            time.sleep(.05)
        self.assertIsNotNone(saved)
        self.assertIn('retained-after-all-session-shutdown', drafts)
        self.assertEqual(self.owner.provider.requests, [])

    def test_semantic_submission_does_not_restore_its_cleared_draft(self):
        peer = owners.View(self.owner.directory)
        try:
            peer.bind()
            draft = peer.draft()
            changed = peer.replace_draft(draft['revision'], 'admitted-semantic-draft')['draft']
            request = peer.submit(changed['text'], route='rollout',
                                  draft_revision=changed['revision'])
            self.assertEqual(peer.result(request)['status'], 'committed')
            self.assertEqual(peer.draft()['text'], '')
            peer.send(type='detach', generation=peer.generation)
            peer.until('detached')
        finally:
            peer.close()
        self.owner.status('detached')
        pane = self.start(True)
        pane.send_text('fresh-after-semantic-submit')
        pane.wait('PROMPT> fresh-after-semantic-submit')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')

    def test_session_quit_refuses_a_chat_draft_without_an_exact_route(self):
        pane = self.start(True)
        pane.send_key('Tab')
        pane.wait('EXACT-host-model-C>')
        pane.send_text('unrouted-chat-draft')
        pane.wait('unrouted-chat-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':session quit')
        pane.wait('Unsent draft')
        self.owner.status('attached')
        pane.wait('unrouted-chat-draft')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.submit(':session quit!')
        self.owner.status('stored')
        self.assertEqual(self.owner.provider.requests, [])

    def test_unrouted_chat_draft_survives_navigation_refusal(self):
        pane = self.start(True)
        pane.send_key('Tab')
        pane.wait('EXACT-host-model-C>')
        pane.send_text('keep-unrouted-editor')
        pane.wait('keep-unrouted-editor')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':history')
        pane.wait('Unsent draft')
        pane.wait('keep-unrouted-editor')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.send_text('i')
        pane.send_key('C-u')
        time.sleep(.08)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_clicking_native_pane_focuses_the_real_editor(self):
        pane = self.start(True)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.send_text('\x1b[<0;2;2M\x1b[<0;2;2m')
        pane.send_text('clicked-native-draft')
        pane.wait('clicked-native-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.start(False).wait('clicked-native-draft')
        self.assertEqual(self.owner.provider.requests, [])

    def test_session_quit_refuses_unsent_draft_and_force_stops_owner(self):
        pane = self.start(True)
        pane.send_text('protected-quit-draft')
        pane.wait('protected-quit-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':session quit')
        pane.wait('Unsent draft')
        self.owner.status('attached')
        pane.wait('protected-quit-draft')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.submit(':session quit!')
        self.owner.status('stored')
        self.assertEqual(self.owner.provider.requests, [])

    def pane_process(self, terminal, argument):
        parent = int(terminal.run('display-message', '-p', '-t', terminal.target, '#{pane_pid}'))
        processes = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,command='], text=True)
        entries = [line.split(None, 2) for line in processes.splitlines()]
        descendants = {parent}
        while True:
            found = {int(pid) for pid, ppid, _ in entries if int(ppid) in descendants}
            if found <= descendants:
                break
            descendants.update(found)
        matches = [int(pid) for pid, _, command in entries
                   if int(pid) in descendants and argument in command]
        self.assertEqual(len(matches), 1, processes)
        return matches[0]

    def test_suspend_releases_controller_and_resume_restores_draft(self):
        # tmux resumes a stopped direct pane process. Retain its parent shell
        # so the window manager can stay stopped until this test resumes it.
        self.launcher.write_text('#!/bin/sh\n' + shlex.quote(str(owners.BINARY)) +
                                 ' vm "$@"\nstatus=$?\nexit "$status"\n')
        pane = self.start(True)
        pane.send_text('suspend-draft')
        pane.wait('suspend-draft')
        parent = self.pane_process(pane, ' vm ')
        pane.send_key('C-z')
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                state = subprocess.check_output(['ps', '-o', 'stat=', '-p', str(parent)],
                                                text=True).strip()
                if 'T' in state:
                    break
                time.sleep(.02)
            else:
                self.fail(f'window manager {parent} did not suspend; state={state}:\n' +
                          pane.capture())
            self.owner.status('detached')
            self.assertEqual(self.owner.identity(), self.owner.owner_identity)
            standalone = self.start(False)
            standalone.wait('suspend-draft')
            standalone.send_text('-outside')
            standalone.wait('suspend-draft-outside')
            frontend = int(standalone.run('display-message', '-p', '-t', standalone.target,
                                          '#{pane_pid}').strip())
            os.kill(frontend, signal.SIGTERM)
            self.owner.status('detached')
        finally:
            os.kill(parent, signal.SIGCONT)
        self.owner.status('attached')
        pane.wait('suspend-draft-outside')
        pane.send_text('-resumed')
        pane.wait('suspend-draft-outside-resumed')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')

    def test_frontend_death_can_reconnect_without_replacing_owner(self):
        pane = self.start(True)
        pane.send_text('surviving-draft')
        pane.wait('surviving-draft')
        frontend = self.pane_process(pane, '--internal-ui-frontend ')
        os.kill(frontend, signal.SIGKILL)
        pane.wait('channel failed')
        # The broker releases the dead frontend's attachment so capture stops.
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':attach')
        self.owner.status('attached')
        pane.wait('surviving-draft')
        pane.send_text('-reconnected')
        pane.wait('surviving-draft-reconnected')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')



    def test_classic_return_preserves_pane_output_and_replays_terminal_output(self):
        pane = self.start(True)
        pane.submit('/fast off')
        pane.wait('Fast mode: OFF')
        pane.send_key('Escape')
        pane.submit(':classic')
        pane.wait_until(lambda _: pane.run('display-message', '-p', '-t', pane.target,
            '#{alternate_on}').strip() == '0', 'classic terminal restored')
        pane.submit('/fast on')
        pane.wait('Fast mode: ON')
        pane.submit('/s d')
        pane.wait_until(lambda _: pane.run('display-message', '-p', '-t', pane.target,
            '#{alternate_on}').strip() == '1', 'workspace alternate screen restored')
        pane.wait('Fast mode: OFF')
        pane.wait('Fast mode: ON')
        pane.send_key('Escape')
        pane.send_text('ireturned-with-output')
        pane.wait('EXACT-host-model-PROMPT> returned-with-output')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_classic_temporarily_releases_the_pane_frontend_and_returns(self):
        pane = self.start(True)
        pane.send_text('classic-draft')
        pane.wait('classic-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':classic')
        time.sleep(.2)
        pane.send_text('-outside-pane')
        pane.wait('classic-draft-outside-pane')
        pane.send_key('C-u')
        pane.submit('/s d')
        pane.wait_until(lambda _: pane.run('display-message', '-p', '-t', pane.target,
            '#{alternate_on}').strip() == '1', 'workspace alternate screen restored')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.send_text('i')
        pane.send_text('returned-draft')
        pane.wait('returned-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_frontend_loss_releases_the_audio_attachment_without_stopping_owner(self):
        pane = self.start(True)
        shell = int(pane.run('display-message', '-p', '-t', pane.target,
                             '#{pane_pid}').strip())
        processes = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,command='], text=True)
        rows = [line.split(None, 2) for line in processes.splitlines()]
        managers = [int(row[0]) for row in rows if len(row) == 3 and
                    int(row[1]) == shell and ' vm ' in row[2]]
        if not managers:
            managers = [shell]
        frontends = [int(row[0]) for row in rows if len(row) == 3 and
                     int(row[1]) in managers and '--internal-ui-frontend' in row[2]]
        self.assertEqual(len(frontends), 1)
        os.kill(frontends[0], signal.SIGKILL)
        pane.wait('Standalone frontend channel failed')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':attach')
        pane.wait('EXACT-host-model-PROMPT>')
        self.owner.status('attached')
        pane.send_text('replacement-editor')
        pane.wait('replacement-editor')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')

    def test_detach_releases_the_native_frontend_and_attach_restores_it(self):
        pane = self.start(True)
        pane.send_text('explicit-detach-draft')
        pane.wait('explicit-detach-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':detach')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.submit(':attach')
        self.owner.status('attached')
        pane.wait('explicit-detach-draft')
        pane.send_text('-reattached')
        pane.wait('explicit-detach-draft-reattached')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')

    def test_workspace_write_checkpoints_the_ordinary_editor_draft(self):
        pane = self.start(True)
        pane.send_text('checkpoint-native-draft')
        pane.wait('EXACT-host-model-PROMPT> checkpoint-native-draft')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':w')
        pane.wait('Workspace saved')
        snapshots = list((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
        self.assertEqual(len(snapshots), 1)
        saved = json.loads(snapshots[0].read_text())
        buffer = next(buffer for connection in saved['state']['buffers']
                      for buffer in connection['buffers'] if buffer['route'] == 'rollout')
        self.assertEqual(buffer['draft'], 'checkpoint-native-draft')
        self.assertEqual(buffer['cursor'], len('checkpoint-native-draft'))
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def checkpointed_draft(self, text):
        snapshots = list((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
        if not snapshots:
            return False
        saved = json.loads(snapshots[0].read_text())
        return any(buffer['draft'] == text and buffer['cursor'] == len(text)
                   for connection in saved['state']['buffers']
                   for buffer in connection['buffers'] if buffer['route'] == 'rollout')

    def test_workspace_write_orders_the_last_input_batch_before_checkpoint(self):
        pane = self.start(True)
        pane.send_text('last-batch-native-draft\x1b:w\r')
        pane.wait('Workspace saved')
        self.assertTrue(self.checkpointed_draft('last-batch-native-draft'))
        pane.submit(':qa')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait('EXACT-host-model-PROMPT> last-batch-native-draft')
        self.assertEqual(self.owner.provider.requests, [])

    def test_native_draft_autosaves_without_leaving_insert(self):
        pane = self.start(True)
        pane.send_text('autosaved-native-draft')
        pane.wait('EXACT-host-model-PROMPT> autosaved-native-draft')
        pane.wait_until(lambda screen: self.checkpointed_draft('autosaved-native-draft'),
                        'ordinary draft checkpoint')
        self.assertNotIn('DRAFT:', pane.capture())
        self.owner.status('attached')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_canceling_an_unfinished_paste_allows_the_next_checkpoint(self):
        pane = self.start(True)
        pane.send_text('\x1b[200~cancel-paste')
        time.sleep(1.2)
        pane.send_key('C-c')
        pane.wait('^C')
        pane.send_text('after-cancel\x1b:w\r')
        pane.wait('Workspace saved')
        self.assertTrue(self.checkpointed_draft('after-cancel'), pane.capture())
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_checkpoint_never_enters_an_unfinished_bracketed_paste(self):
        pane = self.start(True)
        pane.send_text('\x1b[200~partial-paste')
        time.sleep(1.5)  # Cross the real autosave deadline with paste still open.
        pane.send_text('-completed\x1b[201~\x1b:w\r')
        pane.wait('Workspace saved')
        self.assertTrue(self.checkpointed_draft('partial-paste-completed'), pane.capture())
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_failed_checkpoint_preserves_the_previous_workspace_file(self):
        pane = self.start(True)
        pane.send_text('before-checkpoint-failure\x1b:w\r')
        pane.wait('Workspace saved')
        snapshot = next((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
        before = snapshot.read_bytes()
        shell = int(pane.run('display-message', '-p', '-t', pane.target,
                             '#{pane_pid}').strip())
        processes = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,command='], text=True)
        managers = [int(fields[0]) for line in processes.splitlines()
                    if len(fields := line.split(None, 2)) == 3 and
                    int(fields[1]) == shell and ' vm ' in fields[2]]
        if not managers:
            command = subprocess.check_output(['ps', '-o', 'command=', '-p', str(shell)],
                                              text=True)
            self.assertIn(' vm ', command)
            managers = [shell]
        self.assertEqual(len(managers), 1, managers)
        children = [int(pid) for pid in subprocess.check_output(
                    ['pgrep', '-P', str(managers[0])], text=True).split()]
        self.assertEqual(len(children), 1, children)
        frontend = children[0]
        pane.send_text('i-after')
        pane.wait('EXACT-host-model-PROMPT> before-checkpoint-failure-after')
        pane.send_key('Escape')
        time.sleep(.08)
        os.kill(frontend, signal.SIGKILL)
        pane.submit(':w')
        pane.wait('checkpoint unavailable')
        self.assertEqual(snapshot.read_bytes(), before)
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.submit(':attach')
        pane.wait('EXACT-host-model-PROMPT> before-checkpoint-failure-after')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':w')
        pane.wait_until(lambda screen: self.checkpointed_draft('before-checkpoint-failure-after'),
                        'checkpoint after frontend recovery')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def saved_pane(self, text='saved-native-draft'):
        pane = self.start(True)
        if text:
            pane.send_text(text)
            pane.wait(text)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        snapshots = list((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
        self.assertEqual(len(snapshots), 1)
        return snapshots[0]

    def test_cancelled_feedback_survives_standalone_and_pane_reattachment(self):
        standalone = self.start(False)
        standalone.send_text('retained cancelled edit\x03')
        standalone.wait('retained cancelled edit^C')
        standalone.submit('/s d')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait('retained cancelled edit^C')
        expected = self.screen(standalone, False)
        self.assertEqual(standalone.capture().count('retained cancelled edit^C'), 1)
        standalone.close()
        self.owner.status('detached')
        pane = self.start(True)
        pane.wait('retained cancelled edit^C')
        actual = self.screen(pane, True)
        self.assertEqual(pane.capture().count('retained cancelled edit^C'), 1)
        self.assertEqual(actual, expected)
        self.assertEqual(self.owner.provider.requests, [])

    def test_cancel_feedback_and_following_input_match_standalone(self):
        standalone = self.start(False)
        standalone.send_text('cancelled sample\x03')
        standalone.wait('cancelled sample^C')
        standalone.send_text('next draft')
        standalone.wait('next draft')
        expected = self.screen(standalone, False)
        expected_row = int(standalone.run('display-message', '-p', '-t',
            standalone.target, '#{cursor_y}'))
        standalone.send_key('C-u')
        time.sleep(.15)
        standalone.submit('/s d')
        self.owner.status('detached')
        pane = self.start(True)
        pane.send_key('C-u')
        time.sleep(.15)
        pane.send_text('cancelled sample\x03')
        pane.wait('cancelled sample^C')
        pane.send_text('next draft')
        pane.wait('next draft')
        actual = self.screen(pane, True)
        # Both histories include the first cancellation; compare the editor row.
        actual_row = int(pane.run('display-message', '-p', '-t', pane.target, '#{cursor_y}'))
        self.assertEqual([expected[expected_row, col] for col in range(100)],
                         [actual[actual_row, col] for col in range(100)])
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_local_destination_feedback_keeps_the_next_command(self):
        for native in (False, True):
            with self.subTest(pane=native):
                terminal = self.start(native)
                before = terminal.capture().count('Fast mode: OFF')
                terminal.submit('/999')
                terminal.wait('destination 999 is unavailable; use /names')
                terminal.submit('/fast off')
                terminal.wait_until(lambda screen: screen.count('Fast mode: OFF') == before + 1,
                                    'owner command after local destination selection')
                if native:
                    terminal.send_key('Escape')
                    terminal.submit(':qa')
                else:
                    terminal.submit('/s d')
                self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_successive_commands_wait_for_owner_admission(self):
        pane = self.start(True)
        pane.send_text('/verbose 4\r/fast off\r/verbose 4\r/fast off\r')
        pane.wait_until(lambda screen: screen.count('Fast mode: OFF') == 2 and
                        screen.count('verbosity: 4 (debug)') == 2,
                        'all four captured commands admitted in order')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_new_pane_inherits_the_standalone_attachment_verbosity(self):
        standalone = self.start(False)
        standalone.submit('/verbose 3')
        standalone.wait('verbosity: 3')
        standalone.submit('/s d')
        self.owner.status('detached')
        restored = self.start(False)
        before = restored.capture().count('verbosity: 3 (full tools)')
        restored.submit('/verbose')
        restored.wait_until(lambda screen: screen.count('verbosity: 3 (full tools)') > before,
                            'standalone attachment inherits verbosity')
        restored.submit('/s d')
        self.owner.status('detached')
        pane = self.start(True)
        before = pane.capture().count('verbosity: 3 (full tools)')
        pane.submit('/verbose')
        pane.wait_until(lambda screen: screen.count('verbosity: 3 (full tools)') > before,
                        'new pane inherits the same verbosity')
        pane.submit('/verbose 0')
        pane.wait('verbosity: 0 (conversation)')
        pane.send_key('Escape')
        pane.submit(':workspace name verbosity-checkpoint')
        checkpoint = None
        snapshot = None

        def saved_checkpoint(_screen):
            nonlocal checkpoint, snapshot
            for path in (self.root / 'state/workspaces').glob('*/workspace.json'):
                value = json.loads(path.read_text())
                if value['name'] == 'verbosity-checkpoint':
                    snapshot, checkpoint = path, value
                    return True
            return False

        # An owner's late receipt can replace the save notice before capture.
        # The new name proves this command saved, rather than an earlier autosave.
        pane.wait_until(saved_checkpoint, 'new named verbosity checkpoint')
        self.assertEqual(checkpoint['state']['windows'][0]['history']['verbosity'], 0)
        pane.submit(':qa')
        self.owner.status('detached')
        saved = json.loads(snapshot.read_text())['state']
        self.assertEqual(saved.get('native_input', []), [])
        self.assertFalse(any(buffer['pending'] for connection in saved['buffers']
                             for buffer in connection['buffers']))
        standalone = self.start(False)
        standalone.submit('/verbose 3')
        standalone.wait('verbosity: 3 (full tools)')
        standalone.submit('/s d')
        self.owner.status('detached')
        resumed = self.resume_pane(snapshot)
        resumed.wait('NORMAL')
        resumed.send_text('i')
        resumed.wait('EXACT-host-model-PROMPT>')
        before = resumed.capture().count('verbosity: 0 (conversation)')
        resumed.submit('/verbose')
        resumed.wait_until(lambda screen: screen.count('verbosity: 0 (conversation)') > before,
                           'restored pane keeps an explicit zero over owner verbosity')
        resumed.send_key('Escape')
        resumed.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_local_verbosity_reaches_owner_before_other_input(self):
        pane = self.start(True)
        pane.submit('/verbose 4')
        pane.wait('verbosity: 4 (debug)')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.submit('/verbose')
        standalone.wait('verbosity: 4 (debug)')
        standalone.submit('/s d')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_local_verbosity_survives_the_next_owner_frame(self):
        for native in (False, True):
            with self.subTest(pane=native):
                terminal = self.start(native)
                # Reattachment replays earlier replies. Require each command's
                # new reply before sending the next command.
                before = terminal.capture().count('verbosity: 4 (debug)')
                terminal.submit('/verbose 4')
                terminal.wait_until(lambda screen: screen.count('verbosity: 4 (debug)') == before + 1,
                                    'new local verbosity setting')
                before_fast = terminal.capture().count('Fast mode: OFF')
                terminal.submit('/fast off')
                terminal.wait_until(lambda screen: screen.count('Fast mode: OFF') == before_fast + 1,
                                    'new owner frame after local verbosity')
                before = terminal.capture().count('verbosity: 4 (debug)')
                terminal.submit('/verbose')
                terminal.wait_until(lambda screen: screen.count('verbosity: 4 (debug)') == before + 1,
                                    'local verbosity after another owner frame')
                if native:
                    terminal.send_key('Escape')
                    terminal.submit(':qa')
                else:
                    terminal.submit('/verbose 0')
                    terminal.wait('verbosity: 0 (conversation)')
                    terminal.submit('/s d')
                self.owner.status('detached')
        restored = self.start(False)
        restored.submit('/verbose')
        restored.wait('verbosity: 4 (debug)')
        restored.submit('/verbose 0')
        restored.wait('verbosity: 0 (conversation)')
        restored.submit('/s d')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_command_echo_is_retained_once_and_matches_standalone(self):
        standalone = self.start(False)
        standalone.submit('/fast off')
        standalone.wait('Fast mode: OFF')
        expected = standalone.capture().count('PROMPT> /fast off')
        self.assertEqual(expected, 1, standalone.capture())
        standalone.submit('/s d')
        self.owner.status('detached')
        pane = self.start(True)
        self.assertEqual(pane.capture().count('PROMPT> /fast off'), expected, pane.capture())
        pane.submit('/fast off')
        pane.wait_until(lambda screen: screen.count('Fast mode: OFF') == 2,
                        'feedback for both actual commands')
        self.assertEqual(pane.capture().count('PROMPT> /fast off'), expected + 1, pane.capture())
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        retained = json.loads(subprocess.check_output(
            [str(owners.BINARY.parent / 'tests/test_vm_terminal_wire'), 'presentation',
             str(self.owner.directory / '.view-presentation.snb'), self.owner.sid], timeout=5))
        self.assertEqual(sum(row['data'].get('op') == 'submitted' and
                             row['data'].get('text') == '/fast off' for row in retained), 2)
        self.assertEqual(self.owner.provider.requests, [])

    def test_private_cancellations_are_retained_once_per_actual_action(self):
        pane = self.start(True)
        for count in (1, 2):
            pane.send_text('repeat cancelled draft\x03')
            pane.wait_until(lambda screen: screen.count('repeat cancelled draft^C') == count,
                            f'{count} cancellations, without replay duplicates')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait_until(lambda screen: screen.count('repeat cancelled draft^C') == 2,
                              'both private cancellations in standalone history')
        standalone.submit('/s d')
        self.owner.status('detached')
        pane = self.start(True)
        pane.wait_until(lambda screen: screen.count('repeat cancelled draft^C') == 2,
                        'both cancellations after a new frontend attaches')
        pane.send_text('next draft')
        pane.wait('EXACT-host-model-PROMPT> next draft')
        self.assertEqual(pane.capture().count('repeat cancelled draft^C'), 2)
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_same_batch_detach_waits_for_retained_private_feedback(self):
        pane = self.start(True)
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            pane.send_text('cancel before detaching\x03\x1b:qa\r')
            time.sleep(.2)
            self.assertEqual(pane.run('display-message', '-p', '-t', pane.target,
                                     '#{pane_dead}').strip(), '0',
                             'detach must wait for the stopped owner to retain feedback')
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait('cancel before detaching^C')
        self.assertEqual(standalone.capture().count('cancel before detaching^C'), 1)
        self.assertEqual(self.owner.provider.requests, [])

    def test_missing_feedback_receipt_is_bounded_and_preserves_the_owner(self):
        pane = self.start(True)
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            pane.send_text('unacknowledged cancellation\x03')
            pane.wait('unacknowledged cancellation^C')
            pane.wait('Feedback receipt unknown', timeout=8)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        pane.send_key('Escape')
        pane.submit(':attach')
        self.owner.status('attached')
        pane.wait('EXACT-host-model-PROMPT>')
        pane.send_text('draft after feedback timeout')
        pane.wait('PROMPT> draft after feedback timeout')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        self.start(False).wait('PROMPT> draft after feedback timeout')
        self.assertEqual(self.owner.provider.requests, [])

    def test_cancelling_a_draft_does_not_interrupt_the_active_turn(self):
        started, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        provider = self.owner.provider

        def held(handler, request, sequence):
            started.set()
            release.wait(10)
            try:
                provider.reply(handler, provider.response_body(
                    sequence, 'completed after cancelling only the draft').encode(),
                    close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        provider.runtime_handler = held
        pane = self.start(True)
        previous = self.owner.events()[-1]['seq']
        pane.submit('keep the active request running')
        self.assertTrue(started.wait(5))
        pane.wait('EXACT-host-model-A>')
        pane.send_text('discard this draft\x03next draft')
        pane.wait('discard this draft^C')
        pane.wait('EXACT-host-model-A> next draft')
        pane.send_key('Escape')
        pane.submit(':w')
        pane.wait('Workspace saved')
        release.set()
        self.owner.wait_event('turn_completed', previous)
        self.assertFalse(any(event['type'] in ('turn_cancel_requested', 'turn_interrupted')
                             for event in self.owner.events() if event['seq'] > previous))
        pane.wait('EXACT-host-model-PROMPT> next draft')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_yield_reaches_the_actual_owner_while_its_tool_is_waiting(self):
        import shlex

        started = self.root / 'yield-tool-started'
        release = self.root / 'yield-tool-release'
        self.addCleanup(release.touch)
        provider = self.owner.provider

        def respond(handler, request, sequence):
            if sequence == 1:
                command = (f'touch {shlex.quote(str(started))}; '
                           f'while [ ! -e {shlex.quote(str(release))} ]; do sleep .1; done; '
                           'printf yield-tool-finished')
                body = provider.function_body(sequence, 'yield-live-tool', 'exec_command', {
                    'command': command, 'yield_ms': 60000, 'max_output_bytes': 1024})
            elif sequence == 2:
                release.touch()
                result = next(event['data']['result'] for event in self.owner.events()
                              if event['type'] == 'tool_finished')
                body = provider.function_body(sequence, 'yield-join-tool', 'write_stdin', {
                    'handle': result['handle'], 'yield_ms': 1000})
            else:
                release.touch()
                body = provider.response_body(sequence, 'yield-owner-resumed')
            provider.reply(handler, body.encode(), close_header=True)

        provider.runtime_handler = respond
        pane = self.start(True)
        previous = self.owner.events()[-1]['seq']
        pane.submit('run the waiting tool')
        self.owner.wait_event('tool_started', previous)
        deadline = time.monotonic() + 5
        while not started.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(started.exists())
        pane.submit('/yield')
        self.owner.wait_event('turn_yield_requested', previous)
        self.owner.wait_event('turn_completed', previous)
        pane.wait('yield-owner-resumed')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_rejected_chain_restores_its_editor_and_accepts_following_input(self):
        pane = self.start(True)
        command = '/fast on; search retained'
        pane.submit(command)
        pane.wait("/search command separately")
        pane.wait('EXACT-host-model-PROMPT> ' + command)
        self.assertFalse(any(event['type'] == 'service_tier_changed'
                             for event in self.owner.events()))
        pane.send_key('C-u')
        pane.submit('/fast off')
        pane.wait('Fast mode: OFF')
        pane.send_text('still-editable')
        pane.wait('EXACT-host-model-PROMPT> still-editable')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait('EXACT-host-model-PROMPT> still-editable')
        self.assertEqual(self.owner.provider.requests, [])
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_split_and_typing_in_one_input_batch_retains_the_new_editor(self):
        pane = self.start(True)
        pane.send_text('\x1b:vsp\riimmediate-split-draft')
        pane.wait('EXACT-host-model-PROMPT> immediate-split-draft')
        pane.send_key('Escape')
        pane.submit(':qa')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait('EXACT-host-model-PROMPT> immediate-split-draft')
        self.assertEqual(self.owner.provider.requests, [])
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_detach_in_the_same_input_batch_preserves_the_last_draft_bytes(self):
        pane = self.start(True)
        pane.send_text('batched-final-draft\x1b:qa\r')
        self.owner.status('detached')
        standalone = self.start(False)
        standalone.wait('batched-final-draft')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def resume_pane(self, snapshot):
        pane = TmuxTerminal(self.root / f'restored-{len(self.terminals)}', self.launcher, self.root,
            self.root / 'state', None, 100, 25,
            args=('--resume', snapshot.parent.name), environment=self.owner.env)
        self.terminals.append(pane)
        return pane

    def test_saved_native_pane_reopens_the_standalone_frontend(self):
        snapshot = self.saved_pane()
        state = json.loads(snapshot.read_text())['state']
        self.assertEqual(state['v'], 16)
        self.assertTrue(state['windows'][0]['native'])
        pane = self.resume_pane(snapshot)
        pane.wait('saved-native-draft')
        self.owner.status('attached')
        pane.wait('NORMAL')
        pane.send_text('i')
        pane.send_text('-restored')
        pane.wait('saved-native-draft-restored')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def offline_workspace(self, owner_text, baseline='saved-native-draft'):
        snapshot = self.saved_pane(baseline)
        peer = owners.View(self.owner.directory)
        try:
            peer.bind()
            peer.replace_draft(peer.draft()['revision'], owner_text)
            peer.send(type='detach', generation=peer.generation)
            peer.until('detached')
        finally:
            peer.close()
        self.owner.status('detached')
        saved = json.loads(snapshot.read_text())
        buffer = next(buffer for connection in saved['state']['buffers']
                      for buffer in connection['buffers'] if buffer['route'] == 'rollout')
        buffer['draft'] = 'offline-native-draft'
        buffer['cursor'] = len(buffer['draft'])
        snapshot.write_text(json.dumps(saved))
        return snapshot

    def test_pending_native_input_is_saved_and_recovers_without_replaying_enter(self):
        snapshot = self.offline_workspace('saved-native-draft')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            pane = self.resume_pane(snapshot)
            pane.wait('Opening standalone frontend')
            pane.send_text('i-queued-before-ready\r\x1b:w\r')
            pane.wait('Workspace saved')
            saved = json.loads(snapshot.read_text())['state']
            self.assertEqual(saved['native_input'][0]['session'], self.owner.sid)
            self.assertEqual(saved['v'], 16)
            self.assertEqual(saved['native_input'][0]['window'], saved['focus'])
            self.assertEqual(base64.b64decode(saved['native_input'][0]['input'], validate=True),
                             b'-queued-before-ready\r')
            pane.submit(':qa')
            pane.wait('resume this workspace', join_wrapped=True)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.owner.status('detached')
        resumed = self.resume_pane(snapshot)
        resumed.wait('Retained frontend input')
        resumed.submit(':w')
        resumed.wait('Workspace saved')
        resumed.wait('Retained frontend input')
        time.sleep(.15)
        self.assertEqual(self.owner.provider.requests, [])
        resumed.submit(':recover')
        resumed.wait('offline-native-draft-queued-before-ready')
        self.assertEqual(self.owner.provider.requests, [])
        resumed.send_key('Escape')
        time.sleep(.08)
        resumed.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_each_mirror_recovers_only_its_own_pending_input(self):
        pane = self.start(True)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':vsplit')
        pane.wait_until(lambda screen: len(re.findall(
            r'EXACT-host-model-PROMPT>[ \t]*(?:│|$)', screen, re.MULTILINE)) == 2,
                        'both independent editors')
        pane.submit(':qa')
        self.owner.status('detached')
        snapshot, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(snapshot.read_text())
        state = saved['state']
        self.assertEqual(state['v'], 16)
        self.assertEqual(len(state['windows']), 2)
        state['native_input'] = [dict(session=self.owner.sid, window=window['id'],
            input=base64.b64encode(f"-window-{window['id']}\r".encode()).decode())
            for window in state['windows']]
        focused = state['focus']
        other, = [window['id'] for window in state['windows'] if window['id'] != focused]
        snapshot.write_text(json.dumps(saved))
        resumed = self.resume_pane(snapshot)
        resumed.wait('Retained frontend input')
        resumed.submit(':recover')
        resumed.wait(f'-window-{focused}')
        resumed.send_key('Escape')
        time.sleep(.08)
        resumed.submit(':w')
        # Attachment feedback can replace a save notice before the next capture.
        resumed.wait_until(lambda _: [row['window'] for row in
            json.loads(snapshot.read_text())['state'].get('native_input', [])] == [other],
            'only the other mirror input remains saved')
        remaining = json.loads(snapshot.read_text())['state']['native_input']
        self.assertEqual([row['window'] for row in remaining], [other])
        resumed.send_key('C-w')
        resumed.send_text('h')
        resumed.submit(':recover')
        resumed.wait(f'-window-{other}')
        resumed.send_key('Escape')
        time.sleep(.08)
        resumed.submit(':w')
        resumed.wait_until(lambda _: 'native_input' not in
            json.loads(snapshot.read_text())['state'], 'both mirror inputs recovered')
        self.assertNotIn('native_input', json.loads(snapshot.read_text())['state'])
        self.assertEqual(self.owner.provider.requests, [])
        resumed.submit(':qa')
        self.owner.status('detached')

    def test_legacy_pending_input_migrates_to_its_window_without_submission(self):
        snapshot = self.offline_workspace('saved-native-draft')
        saved = json.loads(snapshot.read_text())
        state = saved['state']
        state['v'] = 15
        state['native_input'] = [dict(session=self.owner.sid,
            input=base64.b64encode(b'-legacy-input\r').decode())]
        snapshot.write_text(json.dumps(saved))
        pane = self.resume_pane(snapshot)
        pane.wait('Retained frontend input')
        pane.submit(':w')
        pane.wait('Workspace saved')
        migrated = json.loads(snapshot.read_text())['state']
        self.assertEqual(migrated['v'], 16)
        self.assertEqual(migrated['native_input'][0]['window'], migrated['focus'])
        self.assertEqual(self.owner.provider.requests, [])
        pane.submit(':recover')
        pane.wait('offline-native-draft-legacy-input')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.provider.requests, [])

    def test_paste_started_during_attachment_delays_the_editor_checkpoint(self):
        snapshot = self.offline_workspace('saved-native-draft')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            pane = self.resume_pane(snapshot)
            pane.wait('Opening standalone frontend')
            pane.send_text('i\x1b[200~-queued-paste')
            pane.wait('Input retained')
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        pane.wait('EXACT-host-model-PROMPT>')
        time.sleep(1.5)
        pane.send_text('\x1b[201~-end\x1b:w\r')
        pane.wait('Workspace saved')
        saved = json.loads(snapshot.read_text())['state']
        rollout = saved['buffers'][0]['buffers'][0]
        self.assertEqual(rollout['draft'], 'offline-native-draft-queued-paste-end')
        self.assertEqual(self.owner.provider.requests, [])
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_input_during_native_reconciliation_reaches_only_its_editor(self):
        snapshot = self.offline_workspace('saved-native-draft')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            pane = self.resume_pane(snapshot)
            pane.wait('Opening standalone frontend')
            pane.send_text('i-typed-before-ready')
            pane.wait('INSERT')
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        pane.wait('EXACT-host-model-PROMPT> offline-native-draft-typed-before-ready')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_native_workspace_restores_its_offline_draft_into_the_owner(self):
        snapshot = self.offline_workspace('', baseline='')
        pane = self.resume_pane(snapshot)
        pane.wait('EXACT-host-model-PROMPT> offline-native-draft')
        self.owner.status('attached')
        pane.send_text('i')
        pane.send_text('-restored')
        pane.wait('offline-native-draft-restored')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def workspace_conflict(self, choice, owner_text='owner-newer-draft'):
        snapshot = self.offline_workspace(owner_text)
        pane = self.resume_pane(snapshot)
        pane.wait('Draft conflict')
        pane.submit(':w')
        pane.wait('Workspace saved')
        saved = json.loads(snapshot.read_text())
        buffer = next(buffer for connection in saved['state']['buffers']
                      for buffer in connection['buffers'] if buffer['route'] == 'rollout')
        self.assertEqual(buffer['draft'], 'offline-native-draft')
        self.assertEqual(buffer['conflict']['text'], owner_text)
        pane.submit(':draft ' + choice)
        pane.submit(':attach')
        expected = 'offline-native-draft' if choice == 'local' else owner_text
        pane.wait('EXACT-host-model-PROMPT> ' + expected)
        pane.send_key('Escape')
        time.sleep(.08)
        pane.send_text('i')
        pane.send_text('-resolved')
        pane.wait(expected + '-resolved')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.assertEqual(self.owner.provider.requests, [])

    def test_native_workspace_conflict_can_keep_the_local_draft(self):
        self.workspace_conflict('local')

    def test_native_workspace_conflict_can_keep_the_owner_draft(self):
        self.workspace_conflict('owner')

    def test_owner_clearing_a_changed_offline_draft_remains_a_conflict(self):
        self.workspace_conflict('local', owner_text='')

    def test_pre_native_workspace_restores_before_explicit_attach(self):
        snapshot = self.saved_pane()
        saved = json.loads(snapshot.read_text())
        saved['state']['v'] = 14
        for window in saved['state']['windows']:
            del window['native']
        snapshot.write_text(json.dumps(saved))
        pane = self.resume_pane(snapshot)
        time.sleep(.2)
        pane.submit(':attach')
        pane.wait('saved-native-draft')
        self.owner.status('attached')
        pane.send_key('Escape')
        time.sleep(.08)
        pane.submit(':qa')
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)


if __name__ == '__main__':
    unittest.main()
