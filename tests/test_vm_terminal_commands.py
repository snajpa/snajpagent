# SPDX-License-Identifier: GPL-2.0-only
"""Capability-gated terminal transactions preserve both prompt drafts."""

import json
import os
import select
import signal
import struct
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import test_vm_control as control


class Native:
    def __init__(self, owner, bound=True):
        self.peer = control.owners.connect(owner.directory, 'terminal.sock')
        self.output = bytearray()
        self.send(1)
        assert self.receive()[0] == 2
        profile = b'xterm-256color'.ljust(256, b'\0') + b'\0' * (256 * 3)
        self.send(3, struct.pack('<HH', 24, 120) + profile)
        while self.receive()[0] != 2:
            pass
        if bound:
            self.send(14)

    def close(self):
        self.peer.close()

    def send(self, kind, payload=b''):
        self.peer.sendall(b'SA\x05' + bytes([kind]) + struct.pack('<I', len(payload)) + payload)

    def receive(self):
        header = control.owners.exact(self.peer, 8)
        assert header[:3] == b'SA\x05', header
        size, = struct.unpack('<I', header[4:])
        payload = control.owners.exact(self.peer, size)
        if header[3] == 5:
            self.output.extend(payload)
            self.send(13, struct.pack('<H', len(payload)))
        return header[3], payload

    def command(self, reference):
        self.send(20, reference)
        while True:
            kind, payload = self.receive()
            if kind == 20:
                assert payload[:64] == reference
                return payload[64]

    def until(self, marker):
        deadline = time.monotonic() + 10
        while marker not in self.output and time.monotonic() < deadline:
            self.receive()
        assert marker in self.output, bytes(self.output)


class TerminalCommandTests(unittest.TestCase):
    start = control.ControlTests.start
    snapshots = control.ControlTests.snapshots
    wait_snapshot = control.ControlTests.wait_snapshot
    inputs = control.ControlTests.inputs

    def setUp(self):
        editor_dir = tempfile.TemporaryDirectory(prefix='snag-editor-')
        self.addCleanup(editor_dir.cleanup)
        editor = Path(editor_dir.name) / 'editor.py'
        editor.write_text('''#!/usr/bin/env python3
import os
import select
from pathlib import Path
root = Path(os.environ['HOME'])
with (root / 'editor-runs').open('a') as stream:
    stream.write('started\\n')
print('terminal-editor-ready', flush=True)
input()
(root / 'editor-finished').write_text('finished')
''')
        editor.chmod(0o700)
        with mock.patch.dict(os.environ, EDITOR=str(editor), VISUAL=str(editor)):
            control.ControlTests.setUp(self)

    def attached(self, name):
        child = self.start('-N', name)
        child.command('attach ' + self.owner.sid)
        child.repaint_until(b'ATTACHED')
        return child

    def return_to_workspace(self, child):
        child.output.clear()
        child.write(b'/s d\r')
        child.until(b'\x1b[?1049h')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def terminal_requirement(self):
        peer = self.owner.view(bind=True)
        request = peer.command('/config')
        self.assertEqual(peer.result(request)['status'], 'terminal')
        reference = (peer.capabilities['instance'] + request).encode()
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer.close()
        self.owner.status('detached')
        return request, reference

    def test_reference_checks_owner_and_duplicate_does_not_repeat_effect(self):
        request, reference = self.terminal_requirement()
        native = Native(self.owner)
        self.addCleanup(native.close)
        self.assertEqual(native.command(b'0' * 32 + reference[32:]), 0)
        self.assertEqual(native.command(reference[:32] + b'0' * 32), 0)
        self.assertFalse((self.root / 'editor-runs').exists())
        self.assertEqual(native.command(reference), 1)
        native.until(b'terminal-editor-ready')
        self.assertEqual(native.command(reference), 1)
        native.send(4, b'done\r')
        observer = self.owner.view()
        observer.send(type='receipt', id=request)
        self.assertEqual(observer.result(request)['status'], 'completed')
        self.owner.wait_event('control_finished')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'control_requested']), 1)
        self.assertEqual(self.inputs(), [])
        native.close()
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_reference_before_bound_cannot_execute(self):
        request, reference = self.terminal_requirement()
        native = Native(self.owner, bound=False)
        self.addCleanup(native.close)
        native.send(20, reference)
        with self.assertRaises((EOFError, ConnectionResetError, BrokenPipeError)):
            while True:
                native.receive()
        self.owner.status('detached')
        self.assertFalse((self.root / 'editor-runs').exists())
        observer = self.owner.view()
        observer.send(type='receipt', id=request)
        self.assertEqual(observer.result(request)['status'], 'terminal')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_config_editor_handoff_runs_once_and_retains_report(self):
        child = self.attached('config')
        child.write(b'i/config\r')
        child.until(b'\x1b[?1049l')
        child.until(b'terminal-editor-ready', 10)
        child.write(b'done\r')
        self.owner.wait_event('control_finished')
        self.return_to_workspace(child)
        child.repaint_until(b'REPORT')
        child.command('history')
        child.repaint_until(b'ATTACHED')
        child.finish('close')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.assertEqual(self.inputs(), [])
        saved = next(iter(self.snapshots().values()))['state']['buffers'][0]
        self.assertIsNone(saved['pending'])
        self.assertEqual(saved['reports'][0]['command'], '/config')
        resumed = self.start('--resume', 'config', expect=b'history')
        resumed.finish('close')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.owner.status('detached')

    def test_classic_draft_and_newer_workspace_typing_survive(self):
        classic = self.owner.start(['--resume', self.owner.sid])
        classic.until(b'Attached session')
        os.write(classic.master, b'original classic draft')
        classic.until(b'original classic draft')
        classic.process.terminate()
        deadline = time.monotonic() + 5
        while classic.process.poll() is None and time.monotonic() < deadline:
            if select.select([classic.master], [], [], .05)[0]:
                try:
                    os.read(classic.master, 65536)
                except OSError:
                    break
        self.assertIsNotNone(classic.process.poll())
        self.owner.status('detached')
        child = self.attached('both-drafts')
        child.write(b'i/config\rnewer workspace draft')
        child.until(b'terminal-editor-ready', 10)
        child.write(b'done\r')
        self.owner.wait_event('control_finished')
        child.until(b'original classic draft')
        # A transport detach preserves the classic draft; /s d would replace it.
        child.signal(signal.SIGTERM)
        child.wait_exit()
        self.owner.status('detached')
        resumed = self.start('--resume', 'both-drafts', expect=b'history')
        resumed.repaint_until(b'newer workspace draft')
        rows = self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['buffers'][0]['pending'] is None)
        self.assertEqual(next(iter(rows.values()))['state']['buffers'][0]['draft'],
                         'newer workspace draft')
        resumed.finish('close')
        classic = self.owner.start(['--resume', self.owner.sid])
        classic.until(b'original classic draft')
        self.assertEqual(self.inputs(), [])
        self.owner.finish(classic, b'\x15/s d')

    def test_workspace_restore_keeps_terminal_requirement_without_replay(self):
        child = self.attached('manual')
        child.finish('close')
        self.owner.status('detached')
        peer = self.owner.view(bind=True)
        request = peer.command('/config')
        result = peer.result(request)
        self.assertEqual(result['status'], 'terminal')
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer.close()
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        saved['state']['buffers'][0]['pending'] = {
            'id': request, 'instance': peer.capabilities['instance'], 'text': '/config'}
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'manual', expect=b'history')
        resumed.repaint_until(b'Command needs :classic')
        self.assertFalse((self.root / 'editor-runs').exists())
        resumed.command('classic')
        resumed.until(b'terminal-editor-ready', 10)
        resumed.write(b'done\r')
        self.owner.wait_event('control_finished')
        self.return_to_workspace(resumed)
        resumed.repaint_until(b'REPORT')
        resumed.finish('q')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.owner.status('detached')


if __name__ == '__main__':
    unittest.main()
