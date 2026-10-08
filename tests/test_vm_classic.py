# SPDX-License-Identifier: GPL-2.0-only
"""Whole-terminal attachment, return, and destination-bound input recovery."""

import base64
import json
import os
import signal
import termios
import time
import unittest
from pathlib import Path

import test_vm_control as control

frontend, owners = control.frontend, control.owners
OLD_OWNER = os.environ.get('SNAJPAGENT_VM_CLASSIC_OWNER_BINARY')
if OLD_OWNER:
    owners.BINARY = Path(OLD_OWNER).resolve()
from test_vm_frontend import rollout


class ClassicTests(unittest.TestCase):
    setUp = control.ControlTests.setUp
    start = control.ControlTests.start
    snapshots = control.ControlTests.snapshots
    inputs = control.ControlTests.inputs
    wait_snapshot = frontend.WorkspaceTests.wait_snapshot

    def saved(self):
        return next(iter(self.snapshots().values()))['state']

    def returned(self, child):
        child.output.clear()
        child.write(b'/s d\r')
        child.until(b'\x1b[?1049h')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_picker_fast_input_after_command_runs_once_then_returns(self):
        child = self.start('-N', 'classic')
        child.repaint_until(self.owner.sid[:8].encode())
        child.write(b':classic\rfast-classic-marker\r')
        child.until(b'semantic-answer', 10)
        self.owner.wait_event('response_completed')
        self.returned(child)
        child.finish('close')
        self.owner.status('detached')
        self.assertEqual(len(self.inputs()), 1)
        self.assertIn('fast-classic-marker', json.dumps(self.inputs()[0]))
        self.assertIsNone(self.saved()['classic'])

    def test_same_read_prefix_precedes_later_paste_bytes(self):
        child = self.start('-N', 'large-paste')
        # Legacy owners keep their loaded editor; qualify framing there with ASCII.
        scalar = 'a' if OLD_OWNER else 'α'
        text = ''.join(f'chunk{i:05d}{scalar}/' for i in range(2500)) + 'final-paste-marker'
        data = (b':classic ' + self.owner.sid.encode() + b'\r\x1b[200~' +
                text.encode() + b'\x1b[201~\r')
        while data:
            data = data[os.write(child.master, data):]
        child.until(b'semantic-answer', 10)
        self.owner.wait_event('response_completed')
        self.returned(child)
        child.finish('close')
        self.assertEqual(len(self.inputs()), 1)
        actual = self.inputs()[0]['data']['text']
        if actual != text:
            import difflib
            changes = [(tag, a, b, c, d, text[a:b], actual[c:d]) for tag, a, b, c, d in
                       difflib.SequenceMatcher(None, text, actual, autojunk=False).get_opcodes()
                       if tag != 'equal']
            self.fail(str(changes))

    def test_old_owner_without_semantic_socket_keeps_native_attachment(self):
        endpoint = self.owner.directory / 'view.sock'
        if endpoint.exists():
            endpoint.rename(endpoint.with_suffix('.hidden'))
        child = self.start('-N', 'legacy')
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'Owner attachment unavailable')
        child.command('classic')
        child.until('›'.encode())
        child.resize(18, 120)
        self.returned(child)
        child.finish('close')
        self.owner.status('detached')
        self.assertEqual(self.inputs(), [])

    @unittest.skipIf(OLD_OWNER, 'semantic draft adapter requires current owner')
    def test_flush_draft_and_restore_shared_split_controller(self):
        child = self.start('-N', 'draft')
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(b'iunsent semantic draft\x1b:vsp\r:classic\r')
        child.until('›'.encode())
        self.returned(child)
        child.attached()
        child.repaint_until(b'unsent semantic draft')
        child.command('workspace save')
        self.wait_snapshot(lambda rows: len(next(iter(rows.values()))['state']['windows']) == 2)
        self.assertEqual(rollout(self.saved()['buffers'][0])['draft'], 'unsent semantic draft')
        child.command('close')
        child.finish('close')
        self.owner.status('detached')
        self.assertEqual(self.inputs(), [])

    def test_stopped_owner_preserves_raw_suffix_for_literal_recovery(self):
        attached = self.owner.start(['--resume', self.owner.sid])
        attached.until('›'.encode())
        self.owner.finish(attached, b'/exit')
        self.owner.status('stored')
        child = self.start('-N', 'failed')
        suffix = b'kept\r\x1b[A'
        child.write(b':classic ' + self.owner.sid.encode() + b'\r' + suffix)
        child.until(b'No reachable native owner')
        child.command('workspace save')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['classic'] is not None)
        self.assertEqual(base64.b64decode(self.saved()['classic']['input']), suffix)
        for command in ('session quit', 'sessions quit'):
            child.command(command)
            child.repaint_until(b'Unsent classic input')
            self.assertIsNone(child.process.poll())
        for command in ('q', 'qa'):
            child.finish(command)
            self.assertEqual(base64.b64decode(self.saved()['classic']['input']), suffix)
            child = self.start('--resume', 'failed')
            child.repaint_until(b'uncertain outcome')
            self.assertEqual(self.inputs(), [])
        child.command('recover')
        child.repaint_until(b'INSERT')
        child.write(b'\x1b:workspace save\r')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['classic'] is None)
        self.assertEqual(rollout(self.saved()['buffers'][0])['draft'], 'kept\n\\x1b[A')
        child.finish('close')
        self.assertEqual(self.inputs(), [])
        self.owner.status('stored')

    def test_saved_typeahead_never_replays_and_recovers_to_its_own_session(self):
        child = self.start('-N', 'uncertain')
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        saved['state']['classic'] = {'session': self.owner.sid,
            'input': base64.b64encode(b'unknown\r\x00\xff').decode()}
        path.write_text(json.dumps(saved))
        child = self.start('--resume', 'uncertain')
        child.repaint_until(b'uncertain outcome')
        child.command('classic')
        child.repaint_until(b'inspect history, then :recover')
        self.owner.status('detached')
        self.assertEqual(self.inputs(), [])
        child.command('recover')
        child.repaint_until(b'INSERT')
        child.write(b'\x1b:workspace save\r')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['classic'] is None)
        self.assertEqual(self.saved()['buffers'][0]['session'], self.owner.sid)
        self.assertEqual(rollout(self.saved()['buffers'][0])['draft'], 'unknown\n\\x00\\xff')
        child.finish('close')
        self.assertEqual(self.inputs(), [])

    def test_busy_owner_rejects_handshake_and_keeps_queued_input(self):
        attached = self.owner.start(['--resume', self.owner.sid])
        attached.until('›'.encode())
        child = self.start('-N', 'busy')
        child.write(b':classic ' + self.owner.sid.encode() + b'\rqueued-for-owner\r')
        child.until(b'\x1b[?1049l')
        child.until(b'session already has a terminal')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['classic'] is not None)
        child.repaint_until(b'sessions')
        self.assertEqual(base64.b64decode(self.saved()['classic']['input']), b'queued-for-owner\r')
        self.assertEqual(self.inputs(), [])
        child.finish('close')
        self.owner.status('attached')
        self.owner.finish(attached, b'/s d')

    def test_classic_signal_restores_terminal_and_preserves_owner(self):
        child = self.start('-N', 'signal')
        child.command('classic ' + self.owner.sid)
        child.until('›'.encode())
        child.signal(signal.SIGTERM)
        child.wait_exit()
        self.assertEqual(frontend.normalized_modes(child.state()['modes']), child.original)
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_classic_suspend_resume_and_owner_exit_return_to_workspace(self):
        child = self.start('-N', 'suspend')
        child.command('classic ' + self.owner.sid)
        child.until('›'.encode())
        child.write(b'\x1a')
        deadline = time.monotonic() + 5
        while child.state().get('state') != 'stopped' and time.monotonic() < deadline:
            child.read()
        self.assertEqual(child.state()['state'], 'stopped')
        self.assertEqual(frontend.normalized_modes(termios.tcgetattr(child.slave)), child.original)
        child.resize(15, 110)
        child.output.clear()
        child.signal(signal.SIGCONT)
        child.until('›'.encode())
        child.output.clear()
        child.write(b'/exit\r')
        child.until(b'\x1b[?1049h')
        self.owner.status('stored')
        child.finish('close')


if __name__ == '__main__':
    unittest.main()
