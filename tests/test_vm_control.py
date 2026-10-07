# SPDX-License-Identifier: GPL-2.0-only
"""Real workspace -> native owner -> durable journal/provider integration."""

import copy
import hashlib
import json
import os
import re
import shutil
import signal
import sys
import threading
import time
import unittest
import uuid

import test_vm_frontend as frontend
from test_vm_frontend import rollout
from test_session_listing import canonical

# Each standalone fixture consumes a binary argument at import time. The
# frontend has already consumed ours; preserve unittest's optional selectors.
arguments = sys.argv
sys.argv = sys.argv[:1]
import test_session_view as owners
sys.argv = arguments


owners.BINARY = frontend.BINARY


class ControlTests(unittest.TestCase):
    snapshots = frontend.WorkspaceTests.snapshots

    def start(self, *args, **kwargs):
        child = frontend.WorkspaceTests.start(self, *args, **kwargs)
        self.terminals.append(child)
        return child

    def wait_snapshot(self, predicate):
        # A newly named workspace first contains its empty picker snapshot.
        def ready(rows):
            for child in self.terminals:
                while child.read(0):
                    pass
            return rows and next(iter(rows.values()))['state'].get('buffers') and predicate(rows)
        return frontend.WorkspaceTests.wait_snapshot(self, ready)

    def setUp(self):
        self.terminals = []
        self.owner = owners.SessionViewTests()
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)
        self.root = self.owner.root
        self.owner.detach()

    def inputs(self):
        return [event for event in self.owner.events() if event['type'] == 'input_received']

    def escape(self, child):
        child.write(b'\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')

    def wait_synced(self, text):
        digest = hashlib.sha256(text.encode()).hexdigest()
        return self.wait_snapshot(lambda rows:
            (rollout(next(iter(rows.values()))['state']['buffers'][0]).get('base') or {}).get('sha256')
            == digest)

    def owner_draft(self, text=None):
        peer = self.owner.view()
        peer.bind()
        draft = peer.draft()
        if text is not None:
            draft = peer.replace_draft(draft['revision'], text)['draft']
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer.close()
        self.owner.status('detached')
        return draft

    def test_empty_native_history_reaches_its_logical_start(self):
        self.assertEqual(self.owner.journal.name, 'journal.bin')
        child = self.start('-N', 'native-start', columns=140)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'[start]  [tail]')
        child.write(b'iempty-pane-draft')
        self.wait_synced('empty-pane-draft')
        self.escape(child)
        child.finish('session detach')
        self.assertEqual(self.inputs(), [])

    def test_click_empty_pane_focuses_without_an_extra_key(self):
        from test_vm_mouse import mouse

        child = self.start('-N', 'empty-focus', rows=20, columns=120)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        child.command('vsp')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['focus'] == 2)
        # Let attachment and empty document loading settle before the click.
        child.read(.2)
        mouse(child, 1, 1)
        mouse(child, 1, 1, release=True)
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['focus'] == 1)
        self.assertEqual(self.inputs(), [])
        self.escape(child)
        child.finish('session detach')

    def test_click_enters_pane_insert_and_escape_separates_workspace_commands(self):
        from test_vm_mouse import mouse

        child = self.start('-N', 'click-input', rows=20, columns=120)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.command('vsp')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['focus'] == 2)
        mouse(child, 1, 1)
        mouse(child, 1, 1, release=True)
        child.write(b'/fast')
        self.wait_synced('/fast')
        self.escape(child)
        child.command('workspace name command-scope')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['name'] == 'command-scope')
        self.assertEqual(self.inputs(), [])
        self.assertEqual(rollout(next(iter(self.snapshots().values()))[
            'state']['buffers'][0])['draft'], '/fast')
        # INSERT keeps colon text in the session draft; Ex is reached via Esc.
        child.write(b'A :literal')
        self.wait_synced('/fast :literal')
        self.escape(child)
        child.write(b'\tIbegin ')
        self.wait_synced('begin /fast :literal')
        self.escape(child)
        child.write(b':workspace name canceled')
        child.repaint_until(b'COMMAND-LINE')
        child.write(b'\x1b')
        child.read(.08)
        child.write(b'A!')
        self.wait_synced('begin /fast :literal!')
        self.escape(child)
        self.assertEqual(next(iter(self.snapshots().values()))['name'], 'command-scope')
        child.finish('session detach')

    def test_uppercase_i_enters_composer_from_history(self):
        child = self.start('-N', 'insert-start')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'i  suffix')
        self.wait_synced('  suffix')
        self.escape(child)
        child.write(b'\tIprefix ')
        self.wait_synced('  prefix suffix')
        self.escape(child)
        self.assertEqual(self.inputs(), [])
        child.finish('session detach')

    def test_sixteen_panes_echo_without_waiting_for_owner_draft_ack(self):
        from test_vm_mouse import mouse

        child = self.start('-N', 'input-fairness', rows=80, columns=240)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ihistory before typing\r')
        child.until(b'semantic-answer')
        self.escape(child)
        child.finish('session detach')
        path, = (self.root / 'state/workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        window = saved['state']['windows'][0]
        saved['state']['windows'] = [dict(copy.deepcopy(window), id=i) for i in range(1, 17)]

        def layout(ids, depth=0):
            if len(ids) == 1:
                return {'window': ids[0]}
            middle = len(ids) // 2
            return {'split': 'vertical' if depth % 2 == 0 else 'horizontal', 'weight': 5000,
                    'first': layout(ids[:middle], depth + 1),
                    'second': layout(ids[middle:], depth + 1)}

        saved['state']['layout'] = layout(list(range(1, 17)))
        path.write_text(json.dumps(saved))
        child = self.start('--resume', 'input-fairness', rows=80, columns=240,
                           expect=b'Workspace restored')
        child.until(b'ATTACHED')
        child.read(.2)
        mouse(child, 1, 1)
        mouse(child, 1, 1, release=True)
        # The real owner is deliberately unable to acknowledge draft edits.
        # Keyboard echo must remain local and no second key may repair a frame.
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            for letter in b'KEYBOARD':
                child.output.clear()
                started = time.monotonic()
                child.write(bytes([letter]))
                child.until(bytes([letter]), timeout=.5)
                self.assertLess(time.monotonic() - started, .5)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.wait_synced('KEYBOARD')
        self.assertEqual(len(self.inputs()), 1)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.escape(child)
        child.finish('session detach')

    def test_submission_returns_held_pane_to_visible_prompt_history(self):
        from test_vm_mouse import mouse

        child = self.start('-N', 'submitted-history', rows=24, columns=140)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ifirst prompt\r')
        child.until(b'semantic-answer')
        self.escape(child)
        child.write(b'\tgg')
        self.wait_snapshot(lambda rows:
            not next(iter(rows.values()))['state']['windows'][0]['history']['follow'])
        started = threading.Event()

        def respond(handler, request, sequence):
            started.set()
            self.owner.release.wait(15)
            self.owner.provider.reply(handler,
                self.owner.provider.response_body(sequence, 'second answer').encode(),
                close_header=True)

        self.owner.provider.runtime_handler = respond
        self.addCleanup(self.owner.release.set)
        mouse(child, 0, 2)
        mouse(child, 0, 2, release=True)
        child.write(b'submitted-history-marker\r')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['windows'][0]['history']['follow'] and
            len(self.inputs()) == 2)
        self.assertTrue(started.wait(5))
        child.repaint_until('› submitted-history-marker'.encode())
        self.assertEqual(rollout(next(iter(self.snapshots().values()))[
            'state']['buffers'][0])['draft'], '')
        self.owner.release.set()
        child.until(b'second answer')
        self.escape(child)
        child.finish('session detach')

    def test_live_prompt_is_visible_in_each_pane(self):
        child = self.start('-N', 'pane-prompts', rows=24, columns=160)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        child.command('vsp')
        from test_vm_mouse import positions

        marker = 'fake/host-model/medium   0% ›'
        deadline = time.monotonic() + 5
        while len(positions(child, marker)) < 2:
            self.assertLess(time.monotonic(), deadline)
        child.write(b'iunsent-pane-draft')
        self.wait_synced('unsent-pane-draft')
        child.repaint_until(b'unsent-pane-draft')
        from test_vm_mouse import position
        draft_row, draft_column = position(child, 'unsent-pane-draft')
        prompt_rows = positions(child, marker)
        self.assertIn((draft_row, draft_column - len(marker) - 1), prompt_rows)
        self.escape(child)
        self.assertEqual(self.inputs(), [])
        child.finish('session detach')

    def test_follow_cursor_is_at_prompt_without_an_extra_key(self):
        from test_vm_mouse import mouse, positions

        def prompt_cursor(child, occurrence=0, rows=24):
            marker = 'fake/host-model/medium'
            deadline = time.monotonic() + 5
            actual = expected = None
            while time.monotonic() < deadline:
                child.read(.02)
                prompts = [position for position in positions(child, marker, repaint=False)
                           if position[0] == rows - 3]
                cursor = re.findall(rb'\x1b\[(\d+);(\d+)H\x1b\[\?25([hl])',
                                    child.output)
                if len(prompts) > occurrence and cursor:
                    row, column = prompts[occurrence]
                    expected = row, column + len(marker + '   ?% › '), b'h'
                    y, x, visible = cursor[-1]
                    actual = int(y) - 1, int(x) - 1, visible
                    if actual == expected:
                        return
            self.assertIsNotNone(expected, (prompts, rows))
            self.assertEqual(actual, expected, 'cursor needs another input or redraw')

        child = self.start('-N', 'idle-cursor', rows=24, columns=160)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        prompt_cursor(child)
        child.write(b'icursor-check\r')
        child.until(b'semantic-answer')
        child.write(b'\x1b')
        child.read(.08)
        child.write(b'\t')
        prompt_cursor(child)
        row, column = positions(child, 'semantic-answer', repaint=False)[0]
        mouse(child, row, column + 2)
        mouse(child, row, column + 3, button=32)
        mouse(child, row, column + 3, release=True)
        self.wait_snapshot(lambda rows:
            not next(iter(rows.values()))['state']['windows'][0]['history']['follow'])
        cursor = re.findall(rb'\x1b\[(\d+);(\d+)H\x1b\[\?25h', child.output)[-1]
        self.assertEqual(tuple(int(value) - 1 for value in cursor), (row, column + 3))
        child.write(b'\x1b')
        child.read(.08)
        child.write(b'G')
        prompt_cursor(child)
        child.command('vsp')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['focus'] == 2)
        prompt_cursor(child, 1)
        mouse(child, 22, 1)
        mouse(child, 22, 1, release=True)
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['focus'] == 1)
        prompt_cursor(child)
        self.escape(child)
        child.write(b'\x17\x0c')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['focus'] == 2)
        prompt_cursor(child, 1)
        child.resize(40, 200)
        prompt_cursor(child, 1, rows=40)
        child.finish('session detach')
        child = self.start('--resume', 'idle-cursor', expect=b'Workspace restored',
                           rows=40, columns=200)
        prompt_cursor(child, 1, rows=40)
        self.assertEqual(len(self.inputs()), 1)
        child.finish('session detach')

    def test_status_colors_follow_mouse_focus_without_an_extra_key(self):
        from test_vm_mouse import mouse

        child = self.start('-N', 'focus-colors', rows=20, columns=120,
                           extra_env={'NO_COLOR': None})
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.command('vsp')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['focus'] == 2)
        # Follow the emitted CUP/SGR runs so unchanged cells retain their old
        # style. No extra key or forced frame may repair the focus repaint.
        def status_style(column, expected):
            deadline = time.monotonic() + 5
            actual = None
            while time.monotonic() < deadline:
                child.read(.02)
                row = x = 0
                style = b'0'
                for part in re.split(r'(\x1b\[[0-9;?]*[A-Za-z])',
                                     child.output.decode('utf-8', 'replace')):
                    cup = re.fullmatch(r'\x1b\[(\d+);(\d+)H', part)
                    sgr = re.fullmatch(r'\x1b\[([0-9;]+)m', part)
                    if cup:
                        row, x = (int(value) - 1 for value in cup.groups())
                    elif sgr:
                        style = sgr[1].encode()
                    elif not part.startswith('\x1b['):
                        if row == 18 and x <= column < x + len(part):
                            actual = style
                        x += len(part)
                if actual == expected:
                    return
            self.assertEqual(actual, expected)

        status_style(60, b'0;7')
        status_style(0, b'0;1;7;36')
        mouse(child, 1, 1)
        mouse(child, 1, 1, release=True)
        status_style(0, b'0;7')
        status_style(60, b'0;1;7;36')
        self.assertEqual(self.inputs(), [])
        self.escape(child)
        child.finish('session detach')

    def test_prompt_wrap_and_mouse_hit_preserve_unicode_draft(self):
        from test_vm_mouse import mouse, position

        child = self.start('-N', 'prompt-hit', rows=24, columns=100)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        child.write('ihead 界 tail'.encode())
        self.wait_synced('head 界 tail')
        row, column = position(child, 'head 界 tail')
        mouse(child, row, column + 6)
        mouse(child, row, column + 6, release=True)
        child.write(b'X')
        self.wait_synced('head X界 tail')
        child.resize(8, 1)
        child.read(.1)
        child.resize(24, 100)
        child.repaint_until('head X界 tail'.encode())
        self.escape(child)
        child.finish('session detach')

    def test_prompt_follows_owner_configuration_and_activity(self):
        child = self.start('-N', 'configured-prompt', rows=24, columns=140)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        template = ('{model}/{effort} {context}% '
                    '{chat:C>}{rollout-idle:I>}{rollout-active:A>}')
        with self.owner.config.open('a') as stream:
            stream.write('prompt = ' + template + '\n')
        child.write(b'i/configure\r')
        child.repaint_until(b'configuration reloaded:')
        self.escape(child)
        child.command('history ' + self.owner.sid)
        child.repaint_until(b'host-model/medium 0% I>')
        started = threading.Event()

        def respond(handler, request, sequence):
            started.set()
            self.owner.release.wait(15)
            self.owner.provider.reply(handler,
                self.owner.provider.response_body(sequence, 'prompt-state-answer').encode(),
                close_header=True)

        self.owner.provider.runtime_handler = respond
        self.addCleanup(self.owner.release.set)
        child.write(b'iprompt-state-request\r')
        deadline = time.monotonic() + 5
        while not started.is_set():
            child.read(.02)
            self.assertLess(time.monotonic(), deadline)
        child.repaint_until(b'A>')
        self.owner.release.set()
        child.repaint_until(b'prompt-state-answer')
        child.repaint_until(b'I>')
        self.escape(child)
        child.finish('session detach')

    def test_history_observer_uses_committed_tail_without_control(self):
        child = self.start('-N', 'observed', columns=200)
        child.command('history ' + self.owner.sid)
        child.repaint_until(b'FOLLOW committed')
        self.owner.status('detached')
        peer = self.owner.view()
        peer.bind()
        request = peer.submit('observer-watermark-marker')
        self.assertEqual(peer.result(request)['status'], 'committed')
        child.repaint_until(b'observer-watermark-marker')
        child.repaint_until(b'FOLLOW committed')
        child.write(b'gg')
        child.repaint_until(b'HOLD committed')
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer.close()
        child.command('detach')
        child.repaint_until(b'HOLD snapshot')
        child.command('attach')
        child.repaint_until(b'ATTACHED')
        child.repaint_until(b'HOLD committed')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        child.finish('close')
        self.owner.status('detached')

    def test_history_scan_keeps_other_live_pane_refreshing(self):
        # Build a bounded projection fixture with a valid hash chain. Only the
        # separate native owner submits real turns; this stored source is read-only.
        stored = frontend.WorkspaceTests()
        stored.setUp()
        self.addCleanup(stored.doCleanups)
        journal = stored.seed_session('scan-start-marker', legacy=True)
        last = json.loads(journal.read_bytes().splitlines()[-1])
        filler = canonical({'items': [{'kind': 'assistant', 'phase': 'final',
            'text': 'history row with Unicode é界\n' * 6000}],
            'response_id': 'response-placeholder'}).encode()

        def append(stream, packed):
            nonlocal last
            response = f'{last["seq"]:032x}'.encode()
            packed = packed.replace(b'response-placeholder', response)
            started = canonical({'response_id': response.decode()}).encode()
            for kind, data in [('response_started', started), ('response_completed', packed)]:
                event = dict(data='data-placeholder', prev_sha256=last['event_sha256'],
                    seq=last['seq'] + 1, session_id=last['session_id'],
                    time_ms=last['time_ms'] + 1, type=kind, v=2,
                    checkpoint_offset=last['checkpoint_offset'])
                unsigned = canonical(event).encode().replace(b'"data-placeholder"', data, 1)
                event['event_sha256'] = hashlib.sha256(unsigned).hexdigest()
                stream.write(canonical(event).encode().replace(b'"data-placeholder"', data, 1)
                             + b'\n')
                last = event

        with journal.open('ab') as stream:
            while stream.tell() < 128 * 1024 * 1024:
                append(stream, filler)
            append(stream, canonical({'items': [{'kind': 'assistant', 'phase': 'final',
                'text': 'scan-tail-marker'}], 'response_id': 'response-placeholder'}).encode())
        destination = self.root / 'state/sessions' / journal.parent.name
        shutil.copytree(journal.parent, destination)
        journal = destination / journal.name
        before = journal.stat()
        started = threading.Event()
        requests = []

        def respond(handler, request, sequence):
            requests.append(request)
            started.set()
            if not self.owner.release.wait(30):
                return
            text = 'live output row\n' * 20000 + 'live-scan-complete'
            self.owner.provider.reply(handler,
                self.owner.provider.response_body(sequence, text).encode(), close_header=True)

        self.owner.provider.runtime_handler = respond
        self.addCleanup(self.owner.release.set)
        child = self.start('-N', 'scan-live', rows=24, columns=160)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        child.write(b'ilive-scan-request\r')
        deadline = time.monotonic() + 5
        while not started.is_set():
            child.read(.01)
            self.assertLess(time.monotonic(), deadline)
        self.escape(child)
        child.command('sp')
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'scan-tail-marker')
        child.write(b'?no-such-scan-marker\r')
        child.repaint_until(b'events; Ctrl-C cancels')
        self.owner.release.set()
        child.repaint_until(b'live-scan-complete', timeout=5)
        self.owner.wait_event('turn_completed')
        self.assertIn(b'Searching', child.output)
        self.assertNotIn(b'No matches', child.output)
        self.assertEqual(len(requests), 1)
        self.assertEqual(len(self.inputs()), 1)
        child.write(b'\x03')
        child.repaint_until(b'Search canceled')
        child.resize(3, 18)
        child.read(.1)
        child.resize(24, 160)
        child.repaint_until(b'live-scan-complete')
        child.repaint_until(b'scan-tail-marker')
        child.finish('qa!')
        after = journal.stat()
        self.assertEqual((after.st_size, after.st_mtime_ns), (before.st_size, before.st_mtime_ns))

    def test_owner_draft_adoption_and_final_edit_flushed_on_close(self):
        self.owner_draft('retained 👩‍💻')
        child = self.start('-N', 'owner-draft')
        child.command('attach ' + self.owner.sid)
        self.wait_synced('retained 👩‍💻')
        child.repaint_until('retained 👩‍💻'.encode())
        # No autosave pause between the last edit and clean detach.
        child.write(b'A-final\x1b:close\r')
        child.wait_exit()
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'retained 👩‍💻-final')
        saved = next(iter(self.snapshots().values()))['state']
        self.assertEqual(saved['v'], 13)
        self.assertEqual(rollout(saved['buffers'][0])['draft'], 'retained 👩‍💻-final')
        self.assertTrue(saved['buffers'][0]['control'])
        self.assertEqual(self.inputs(), [])

    def test_conflicting_workspace_and_owner_edits_preserve_both_choices(self):
        child = self.start('-N', 'conflict')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ibase draft')
        self.wait_synced('base draft')
        self.escape(child)
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        for choice in ('owner', 'local'):
            saved = json.loads(path.read_text())
            rollout(saved['state']['buffers'][0])['draft'] = 'local ' + choice
            rollout(saved['state']['buffers'][0])['cursor'] = len('local ' + choice)
            path.write_text(json.dumps(saved))
            self.owner_draft('owner ' + choice)
            resumed = self.start('--resume', 'conflict', expect=b'history')
            resumed.repaint_until(b'Draft conflict')
            rows = self.wait_snapshot(lambda rows:
                rollout(next(iter(rows.values()))['state']['buffers'][0]).get('conflict') is not None)
            buffer = rollout(next(iter(rows.values()))['state']['buffers'][0])
            self.assertEqual(buffer['draft'], 'local ' + choice)
            self.assertEqual(buffer['conflict']['text'], 'owner ' + choice)
            resumed.finish('close')
            self.owner.status('detached')
            self.owner_draft('later owner edit')
            previous = json.loads(path.read_text())['activity_ms']
            resumed = self.start('--resume', 'conflict', expect=b'history')
            resumed.repaint_until(b'Draft conflict')
            resumed.command('workspace save')
            self.wait_snapshot(lambda rows: next(iter(rows.values()))['activity_ms'] > previous)
            self.assertEqual(rollout(json.loads(path.read_text())['state']['buffers'][0])['conflict']['text'],
                             'owner ' + choice)
            resumed.command('draft ' + choice)
            expected = choice + ' ' + choice
            self.wait_synced(expected)
            resumed.finish('close')
            self.owner.status('detached')
            self.assertEqual(self.owner_draft()['text'], expected)
        self.assertEqual(self.inputs(), [])

    def test_one_sided_offline_edit_and_old_workspace_snapshot(self):
        child = self.start('-N', 'offline')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ione side')
        self.wait_synced('one side')
        self.escape(child)
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        rollout(saved['state']['buffers'][0])['draft'] = 'local offline edit'
        rollout(saved['state']['buffers'][0])['cursor'] = len('local offline edit')
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'offline', expect=b'history')
        self.wait_synced('local offline edit')
        resumed.finish('close')
        self.assertEqual(self.owner_draft()['text'], 'local offline edit')
        self.owner_draft('owner changed')
        resumed = self.start('--resume', 'offline', expect=b'history')
        self.wait_synced('owner changed')
        resumed.finish('close')
        saved = json.loads(path.read_text())
        saved['state']['v'] = 3
        saved['state'].pop('classic')
        for window in saved['state']['windows']:
            window.get('history', {}).pop('source', None)
            window.get('history', {}).pop('route', None)
        buffer = rollout(saved['state']['buffers'][0])
        owner = saved['state']['buffers'][0]
        saved['state']['buffers'][0] = {
            'session': owner['session'], 'control': owner['control'],
            'draft': buffer['draft'], 'cursor': buffer['cursor'], 'pending': buffer['pending']}
        buffer = saved['state']['buffers'][0]
        buffer['draft'], buffer['cursor'] = '', 0
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'offline', expect=b'history')
        self.wait_synced('owner changed')
        resumed.finish('close')
        self.assertEqual(self.inputs(), [])

    def test_force_quit_discards_both_conflicting_drafts(self):
        self.owner_draft('owner copy')
        child = self.start('-N', 'discard-conflict')
        child.command('history ' + self.owner.sid)
        child.write(b'ilocal copy\x1b:attach\r')
        child.repaint_until(b'Draft conflict')
        child.finish('q!')
        self.owner.status('stored')
        buffer = rollout(next(iter(self.snapshots().values()))['state']['buffers'][0])
        self.assertEqual(buffer['draft'], '')
        self.assertIsNone(buffer['conflict'])
        self.assertEqual(self.inputs(), [])

    def test_detached_conflict_can_restore_retained_owner_copy(self):
        self.owner_draft('retained owner copy')
        child = self.start('-N', 'offline-choice')
        child.command('history ' + self.owner.sid)
        child.write(b'ilocal copy\x1b:attach\r')
        child.repaint_until(b'Draft conflict')
        child.command('detach')
        child.repaint_until(b'Detached; unresolved draft retained')
        self.owner.status('detached')
        child.command('draft owner')
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] == 'retained owner copy' and
            rollout(next(iter(rows.values()))['state']['buffers'][0])['conflict'] is None)
        child.finish('close')
        self.assertEqual(self.owner_draft()['text'], 'retained owner copy')
        self.assertEqual(self.inputs(), [])

    def test_unacknowledged_edit_survives_frontend_loss(self):
        child = self.start('-N', 'unacknowledged')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'iacknowledged')
        self.wait_synced('acknowledged')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'-suffix')
            self.wait_snapshot(lambda rows:
                rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] == 'acknowledged-suffix')
            child.signal(signal.SIGTERM)
            child.wait_exit()
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.owner.status('detached')
        resumed = self.start('--resume', 'unacknowledged', expect=b'history')
        self.wait_synced('acknowledged-suffix')
        resumed.finish('close')
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'acknowledged-suffix')
        self.assertEqual(self.inputs(), [])

    def test_suspend_flushes_owner_draft_and_reconnects(self):
        child = self.start('-N', 'suspended-draft')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'isuspend draft')
        self.escape(child)
        child.write(b'\x1a')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and child.state().get('state') != 'stopped':
            child.read()
        self.assertEqual(child.state().get('state'), 'stopped')
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'suspend draft')
        # A different controller may edit while the workspace is suspended.
        self.owner_draft('edited during suspend')
        child.signal(signal.SIGCONT)
        self.wait_synced('edited during suspend')
        child.repaint_until(b'edited during suspend')
        child.finish('close')
        self.assertEqual(self.inputs(), [])

    def test_workspace_switch_flushes_shared_owner_and_preserves_source(self):
        target = self.start('-N', 'destination')
        target.finish()
        child = self.start('-N', 'source')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ifinal source draft\x1b:vsp\r:workspaces\rG\r')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            child.read()
            rows = frontend.WorkspaceTests.cli(self, '-l').stdout
            if b'\tdestination\topen\t' in rows:
                break
        self.assertIn(b'\tdestination\topen\t', rows)
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'final source draft')
        source = next(x for x in self.snapshots().values() if x['name'] == 'source')
        self.assertEqual(rollout(source['state']['buffers'][0])['draft'], 'final source draft')
        self.assertTrue(source['state']['buffers'][0]['control'])
        child.finish()
        self.assertEqual(self.inputs(), [])

    def test_picker_attach_unicode_paste_submit_and_newer_draft(self):
        child = self.start('-N', 'composing')
        child.until(self.owner.sid[:8].encode())
        child.write(b'\r')
        child.until(b'ATTACHED')
        self.owner.status('attached')
        child.write('iAe\u0301'.encode() + b'\x7f')
        child.write(b'\x1b[200~' + '👩‍💻\n:qa\nliteral'.encode() + b'\x1b[201~')
        self.wait_snapshot(lambda rows: rollout(next(iter(rows.values()))['state']['buffers'][0])['draft']
                           == 'A👩‍💻\n:qa\nliteral')
        child.write(b'\rnext-draft')
        event = self.owner.wait_event('input_received')
        self.assertEqual(event['data']['text'], 'A👩‍💻\n:qa\nliteral')
        self.wait_snapshot(lambda rows:
                           rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None and
                           rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] == 'next-draft')
        self.assertEqual(len(self.inputs()), 1)
        self.escape(child)
        child.command('q')
        child.repaint_until(b'Unsent draft')
        self.assertIsNone(child.process.poll())
        child.finish('close')
        self.owner.status('detached')
        resumed = self.start('--resume', 'composing', expect=b'history')
        resumed.until(b'ATTACHED')
        resumed.repaint_until(b'next-draft')
        resumed.command('detach')
        self.owner.status('detached')
        resumed.finish()

    def test_shared_split_controller_close_and_normal_quit(self):
        child = self.start('-N', 'splits')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.command('vsp')
        child.write(b'ishared-draft')
        self.escape(child)
        child.command('workspace save')
        rows = self.wait_snapshot(lambda rows:
                                  len(next(iter(rows.values()))['state']['windows']) == 2)
        self.assertEqual(len(next(iter(rows.values()))['state']['buffers']), 1)
        child.command('close')
        self.owner.status('attached')
        child.repaint_until(b'shared-draft')
        child.command('attach missing-session')
        child.command('q')
        child.repaint_until(b'Unsent draft')
        child.finish('q!')
        self.owner.status('stored')

    def test_rejected_command_keeps_text_and_recover_never_resubmits(self):
        child = self.start('-N', 'rejection')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        # Keep Ex entry active while the terminal requirement arrives, so
        # explicit recovery takes precedence over automatic terminal handoff.
        child.write(b'i/config\r\x1b:')
        self.wait_snapshot(lambda rows:
            (rollout(next(iter(rows.values()))['state']['buffers'][0]).get('base') or {}).get(
                'revision', 0) >= 3)
        self.assertEqual(self.inputs(), [])
        child.write(b'recover\r')
        child.repaint_until(b'Submission recovered')
        rows = self.wait_snapshot(lambda rows:
                                  rollout(next(iter(rows.values()))['state']['buffers'][0])['draft']
                                  == '/config')
        self.assertIsNone(rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'])
        child.finish('close')
        self.owner.status('detached')

    def test_saved_unknown_receipt_is_retained_without_resubmission(self):
        child = self.start('-N', 'uncertain')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        peer = self.owner.view()
        instance = peer.capabilities['instance']
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        rollout(saved['state']['buffers'][0])['pending'] = {
            'id': uuid.uuid4().hex, 'instance': instance, 'text': 'possibly-admitted-text'}
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'uncertain', expect=b'history')
        resumed.repaint_until(b'Submission outcome unknown')
        self.owner.status('attached')
        resumed.repaint_until(b'ATTACHED')
        self.assertEqual(self.inputs(), [])
        resumed.command('qa')
        resumed.repaint_until(b'Unsent draft or unresolved submission')
        resumed.command('recover')
        resumed.repaint_until(b'Submission recovered')
        self.assertEqual(self.inputs(), [])
        resumed.finish('close')

    def test_signal_detaches_preserves_draft_and_idle_emits_no_frames(self):
        child = self.start('-N', 'signal-draft')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'idurable-unsent')
        self.wait_snapshot(lambda rows:
                           rollout(next(iter(rows.values()))['state']['buffers'][0])['draft']
                           == 'durable-unsent')
        while child.read(.1):
            pass
        self.assertEqual(child.read(1.2), b'')
        child.signal(signal.SIGTERM)
        child.wait_exit()
        self.owner.status('detached')
        resumed = self.start('--resume', 'signal-draft', expect=b'history')
        resumed.until(b'ATTACHED')
        resumed.repaint_until(b'durable-unsent')
        resumed.finish('close')

    def test_workspace_detach_waits_for_owner_and_preserves_splits(self):
        child = self.start('-N', 'detach-workspace', columns=180)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.command('vsp')
        child.command('sp')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'ipreserve workspace draft')
            self.escape(child)
            child.write(b':session d\t\r')
            child.read(.25)
            self.assertIsNone(child.process.poll())
            self.assertNotIn(b'You can resume this workspace', child.output)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        child.wait_exit()
        self.assertEqual(child.process.returncode, 0)
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'preserve workspace draft')
        rows = self.snapshots()
        sid, = rows
        frontend.WorkspaceTests.resume_hint(self, child, sid)
        self.assertEqual(len(rows[sid]['state']['windows']), 3)
        resumed = self.start('--resume', sid, expect=b'history')
        resumed.repaint_until(b'ATTACHED')
        resumed.repaint_until(b'preserve workspace draft')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        resumed.finish('session detach')

    def test_detach_notice_waits_for_owner_acknowledgement(self):
        child = self.start('-N', 'detach-ack', columns=160)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'ipreserve before detach')
            self.escape(child)
            child.command('detach')
            child.repaint_until(b'Saving owner draft before detach', timeout=1)
            self.assertNotIn(b'Detached; owner continues', child.output)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        child.repaint_until(b'Detached; owner continues')
        child.repaint_until((':attach ' + self.owner.sid).encode())
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'preserve before detach')
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        self.owner.status('attached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        child.finish('close')

    def test_undo_stops_at_submitted_and_adopted_draft_boundaries(self):
        child = self.start('-N', 'undo-boundaries')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'iadmitted text\rnext draft')
        self.owner.wait_event('input_received')
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None and
            rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] == 'next draft')
        self.escape(child)
        child.write(b'uu')
        child.command('workspace name after-undo')
        rows = self.wait_snapshot(lambda rows: next(iter(rows.values()))['name'] == 'after-undo')
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'], '')
        child.write(b'ilocal draft')
        self.escape(child)
        child.command('detach')
        child.repaint_until(b'Detached; owner continues')
        self.owner_draft('remote replacement')
        child.command('attach')
        child.repaint_until(b'Owner draft restored')
        child.write(b'\tu')
        child.command('workspace name after-adoption')
        rows = self.wait_snapshot(lambda rows:
                                  next(iter(rows.values()))['name'] == 'after-adoption')
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'],
                         'remote replacement')
        self.assertEqual(len(self.inputs()), 1)
        child.finish('close')

    def test_receipt_after_frontend_loss_does_not_repeat_prompt(self):
        child = self.start('-N', 'lost-receipt')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'iexactly-once-after-reconnect')
        self.wait_synced('exactly-once-after-reconnect')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'\rnewer-text')
            self.wait_snapshot(lambda rows:
                               rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] and
                               rollout(next(iter(rows.values()))['state']['buffers'][0])['draft']
                               == 'newer-text')
            # Drain the test terminal, let the queued wire request leave, then
            # hold only this disposable frontend before allowing owner admission.
            deadline = time.monotonic() + .2
            while time.monotonic() < deadline:
                child.read(.02)
            child.signal(signal.SIGSTOP)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.owner.wait_event('input_received')
        child.signal(signal.SIGTERM)
        child.signal(signal.SIGCONT)
        child.wait_exit()
        self.owner.status('detached')
        resumed = self.start('--resume', 'lost-receipt', expect=b'history')
        resumed.repaint_until(b'newer-text')
        self.wait_snapshot(lambda rows:
                           rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None)
        self.assertEqual(rollout(next(iter(self.snapshots().values()))['state']['buffers'][0])['draft'],
                         'newer-text')
        self.assertEqual(len(self.inputs()), 1)
        resumed.finish('close')

    def test_insert_joining_graphemes_keeps_resumable_cursor(self):
        child = self.start('-N', 'clusters')
        child.command('history ' + self.owner.sid)
        child.write('i👩💻'.encode() + b'\x1b[D' + '\u200d'.encode())
        rows = self.wait_snapshot(lambda rows:
                                  rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] == '👩‍💻')
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['cursor'], len('👩‍💻'.encode()))
        self.escape(child)
        child.finish('close')
        resumed = self.start('--resume', 'clusters', expect=b'history')
        resumed.write(b'A\x7f')
        self.wait_snapshot(lambda rows:
                           rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] == '')
        self.escape(resumed)
        resumed.finish()

    def test_fast_escape_control_key_leaves_insert_before_workspace_command(self):
        child = self.start('-N', 'escape-redraw')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ipreserve this\x1b\x0c:workspace save\r')
        child.until(b'Workspace saved')
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] ==
            'preserve this')
        self.assertEqual(self.inputs(), [])
        child.finish('close')
        self.owner.status('detached')

    def test_fast_escape_colon_leaves_insert_without_submitting(self):
        child = self.start('-N', 'fast-escape')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ipreserve this\x1b:workspace save\r')
        self.wait_snapshot(lambda rows:
                           rollout(next(iter(rows.values()))['state']['buffers'][0])['draft']
                           == 'preserve this')
        self.assertEqual(self.inputs(), [])
        child.finish('close')
        self.owner.status('detached')


if __name__ == '__main__':
    unittest.main()
