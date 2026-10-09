# SPDX-License-Identifier: GPL-2.0-only
"""Capability-gated terminal transactions preserve both prompt drafts."""

import hashlib
import json
import os
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

import test_vm_control as control
import test_vm_clipboard as clipboard
from test_vm_frontend import normalized_modes, rollout


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
        self.editor = editor
        write_config = control.owners.harness.write_irc_config

        def configuration(*args, **kwargs):
            return write_config(*args, **dict(kwargs, pager=str(editor)))

        with mock.patch.dict(os.environ, EDITOR=str(editor), VISUAL=str(editor)):
            with mock.patch.object(control.owners.harness, 'write_irc_config', configuration):
                control.ControlTests.setUp(self)
        self.addCleanup(self.stop_editor)

    def stop_editor(self):
        rows = subprocess.check_output(
            ['ps', '-axww', '-o', 'pid=', '-o', 'ppid=', '-o', 'command='], text=True)
        for row in rows.splitlines():
            fields = row.split(None, 2)
            if len(fields) == 3 and fields[1] == str(self.owner.owner) and str(self.editor) in fields[2]:
                try:
                    os.kill(int(fields[0]), signal.SIGTERM)
                except ProcessLookupError:
                    pass

    def attached(self, name):
        child = self.start('-N', name)
        child.command('attach ' + self.owner.sid)
        child.attached()
        return child

    def return_to_workspace(self, child):
        child.until(b'\x1b[?1049h')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def show_report(self, child, command, timeout=5):
        opened = getattr(self, 'opened_reports', {})
        def completed(rows):
            if not rows:
                return False
            buffers = next(iter(rows.values()))['state']['buffers']
            if not buffers:
                return False
            owner = buffers[0]
            return (not rollout(owner)['pending'] and any(
                r['command'] == command and r['id'] != opened.get(command)
                for r in owner['reports']))
        rows = control.frontend.WorkspaceTests.wait_snapshot(self, completed, timeout=timeout)
        report = next(r for r in reversed(next(iter(rows.values()))[
            'state']['buffers'][0]['reports']) if r['command'] == command)
        opened[command] = report['id']
        self.opened_reports = opened
        child.write(b'\x1b')
        child.read(.06)
        child.command('report ' + report['id'])
        child.repaint_until(('REPORT ' + self.owner.sid[:8] + ' ' + report['id'][:8]).encode())

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
        while native.receive()[0] != 7:
            pass
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
        observer = self.owner.view()
        pending = rollout(next(iter(self.snapshots().values()))['state']['buffers'][0])['pending']
        observer.send(type='receipt', id=pending['id'])
        self.assertEqual(observer.until('result')['status'], 'pending')
        child.output.clear()
        child.write(b'done\r')
        self.owner.wait_event('control_finished')
        self.return_to_workspace(child)
        self.show_report(child, '/config')
        child.command('history')
        child.attached()
        child.finish('close')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.assertEqual(self.inputs(), [])
        saved = next(iter(self.snapshots().values()))['state']['buffers'][0]
        self.assertIsNone(rollout(saved)['pending'])
        self.assertEqual(saved['reports'][0]['command'], '/config')
        report = self.owner.directory / ('.view-report-' + saved['reports'][0]['id'])
        self.assertIn(b'configuration unchanged:', report.read_bytes())
        resumed = self.start('--resume', 'config', expect=b'\x1b[?1049h')
        resumed.finish('close')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.owner.status('detached')

    def test_workspace_file_snapshot_and_explicit_classic_pager(self):
        path = self.root / 'shown.txt'
        path.write_text('operator file\n')
        child = self.attached('pager-return')
        child.write(('i/cat ' + str(path) + '\r').encode())
        self.show_report(child, '/cat ' + str(path))
        child.repaint_until(b'operator file')
        self.assertFalse((self.root / 'editor-runs').exists())
        child.command('history')
        child.command('classic')
        child.until(b'Attached session', 10)
        child.write(('/cat ' + str(path) + '\r').encode())
        child.until(b'terminal-editor-ready', 10)
        child.output.clear()
        child.write(b'done\r')
        child.until(b'host-model/medium', 10)
        child.write(b'/s d\r')
        self.return_to_workspace(child)
        child.finish('close')
        self.assertEqual(self.inputs(), [])
        self.owner.status('detached')

    def test_send_through_remote_waits_for_final_receipt(self):
        path = self.root / 'transfer.bin'
        path.write_bytes(bytes(range(256)) * 1024)
        child = self.start('-N', 'transfer',
                           transport=[str(control.frontend.BINARY), 'remote'])
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(('i/send ' + str(path) + '\r').encode())
        child.until(b'\x1b[?1049l')
        del child.output[:child.output.index(b'\x1b[?1049l') + len(b'\x1b[?1049l')]
        child.until(b'Client acknowledged the file digest and final EXIT.', 15)
        self.return_to_workspace(child)
        self.show_report(child, '/send ' + str(path))
        self.assertEqual((self.root / 'Downloads' / path.name).read_bytes(), path.read_bytes())
        for protocol in (b'#DATA:', b'#CFG:', b'::TRZSZ:TRANSFER:'):
            self.assertNotIn(protocol, child.output)
        child.finish('close')
        self.assertEqual(self.inputs(), [])
        self.owner.status('detached')

    def transfer_terminal(self, name, mux=None, mosh=False, ssh=False):
        transport = [str(control.frontend.BINARY), 'remote']
        env = {}
        if mux == 'screen':
            sockets = self.root / 'screens'
            sockets.mkdir(mode=0o700)
            env['SCREENDIR'] = str(sockets)
            config = self.root / 'screenrc'
            config.write_text('startup_message off\naltscreen on\n')
            multiplexer = ['screen', '-U', '-c', str(config), '-S', 'transfer']
            self.addCleanup(subprocess.run, ['screen', '-S', 'transfer', '-X', 'quit'],
                            env={**os.environ, **env}, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, timeout=5)
        elif mux == 'tmux':
            tmux = ['tmux', '-S', str(self.root / 'transfer-tmux.sock')]
            multiplexer = [*tmux, '-f', '/dev/null', 'new-session', '-s', 'transfer', '--']
            self.addCleanup(subprocess.run, [*tmux, 'kill-server'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        else:
            multiplexer = []
        if ssh:
            transport.extend(clipboard.ClipboardTests.ssh_transport(self, env))
        if mosh:
            transport.extend(['mosh', '--local', '--predict=never', '127.0.0.1'])
        transport.extend(multiplexer)
        return self.start('-N', name, columns=200, transport=transport, extra_env=env)

    def receive_and_send(self, background=False, resize=False, **transport):
        path = self.root / 'notes file.txt'
        body = 'résumé workspace upload\n' * 300
        path.write_text(body)
        requests = []
        previous = self.owner.provider.runtime_handler
        started, release, answered = threading.Event(), threading.Event(), threading.Event()
        self.addCleanup(release.set)

        def record(handler, request, sequence):
            requests.append(request)
            if background and len(requests) == 1:
                started.set()
                release.wait(15)
            result = previous(handler, request, sequence)
            answered.set()
            return result

        self.owner.provider.runtime_handler = record
        child = self.transfer_terminal('receive', **transport)
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.command('vsp')
        if background:
            child.write(b'iwork during upload\r')
            deadline = time.monotonic() + 5
            while not started.is_set() and time.monotonic() < deadline:
                child.read(.05)
            self.assertTrue(started.is_set())
            child.write(b'\x1b:workspace save\r')
            child.repaint_until(b'Workspace saved')
        child.write(b'i/receive\r')
        child.until(b'Select local file', 10)
        if resize:
            child.resize(8, 47)
            deadline = time.monotonic() + 5
            while True:
                size = subprocess.check_output(
                    ['tmux', '-S', str(self.root / 'transfer-tmux.sock'), 'display-message',
                     '-p', '-t', 'transfer', '#{pane_height} #{pane_width}'], text=True).strip()
                if size == '7 47':
                    break
                self.assertLess(time.monotonic(), deadline, size)
                child.read(.05)
        if background:
            release.set()
            self.assertTrue(answered.wait(5))
        child.output.clear()
        child.write((str(path) + '\r').encode())
        if resize:
            child.attached(15)
            child.resize(14, 200)
            child.repaint_until(b'1 unsent attachment(s)')
        else:
            child.repaint_until(b'1 unsent attachment(s)', 15)
        self.assertEqual(len(self.inputs()), int(background))
        self.assertEqual(len(requests), int(background))
        self.show_report(child, '/receive')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        child.repaint_until(b'1 unsent attachment(s)')
        child.command('history')
        if background:
            self.owner.wait_event('turn_completed')
            child.repaint_until(b'semantic-answer')
        child.output.clear()
        child.write(('i/send ' + str(path) + '\r').encode())
        self.show_report(child, '/send ' + str(path), timeout=15)
        child.repaint_until(b'Client acknowledged the file digest and final EXIT.', 15)
        self.assertEqual((self.root / 'Downloads' / path.name).read_bytes(), body.encode())
        child.command('history')
        child.write(b'iinspect uploaded notes\r')
        deadline = time.monotonic() + 10
        while len(requests) < 1 + int(background) and time.monotonic() < deadline:
            child.read(.05)
        self.assertEqual(len(self.inputs()), 1 + int(background))
        self.assertEqual(len(requests), 1 + int(background))
        part, = self.inputs()[-1]['data']['content']
        self.assertEqual(part['type'], 'file')
        asset = part['asset']
        self.assertEqual(asset['sha256'], hashlib.sha256(body.encode()).hexdigest())
        self.assertEqual((self.owner.directory / 'media' / asset['id']).read_bytes(), body.encode())
        self.assertIn('asset:' + asset['id'], json.dumps(requests[-1]))
        self.assertIn('inspect uploaded notes', json.dumps(requests[-1]))
        child.write(b'\x1b:workspace save\r')
        child.repaint_until(b'Workspace saved')
        child.command('close')
        child.finish('close')
        self.owner.status('detached')

    def pane_upload(self, trigger, draft, name):
        path = self.root / 'dropped.txt'
        path.write_text('drop from workspace\n')
        child = self.transfer_terminal(name)
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(b'i' + trigger)
        child.until(b'Select local file', 10)
        rows = self.snapshots()
        pending = rollout(next(iter(rows.values()))['state']['buffers'][0])
        self.assertTrue(pending['pending']['literal'])
        self.assertEqual(pending['draft'], draft)
        child.output.clear()
        child.write((str(path) + '\r').encode())
        child.until(b'1 unsent attachment(s)', 15)
        self.wait_snapshot(lambda rows: not rollout(next(iter(rows.values()))[
            'state']['buffers'][0])['pending'])
        child.write(b'\x1b')
        child.read(.06)
        child.command('workspace save')
        rows = self.wait_snapshot(lambda rows: rollout(next(iter(rows.values()))[
            'state']['buffers'][0])['draft'] == draft)
        self.assertEqual(rollout(next(iter(rows.values()))[
            'state']['buffers'][0])['draft'], draft)
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        child.finish('workspace detach')

    def test_drop_upload_keeps_unsent_pane_draft(self):
        self.pane_upload(b'unsent draft\x1b[9002~', 'unsent draft', 'drop-receive')

    def test_stock_upload_launch_alias_uses_terminal_transfer(self):
        self.pane_upload(b'trz\r', '', 'upload-alias')

    def test_stock_directory_upload_alias_uses_terminal_transfer(self):
        child = self.transfer_terminal('directory-upload-alias')
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(b'itrz -d\r')
        child.until(b'Directory uploads are unsupported.', 10)
        child.attached(10)
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        child.write(b'\x1b')
        child.read(.06)
        child.finish('workspace detach')

    def test_receive_through_remote_keeps_attachment_for_workspace_submit(self):
        self.receive_and_send(background=True)

    @unittest.skipUnless(shutil.which('screen'), 'GNU Screen unavailable')
    def test_transfers_with_workspace_in_screen(self):
        self.receive_and_send(mux='screen')

    @unittest.skipUnless(shutil.which('tmux'), 'tmux unavailable')
    def test_transfers_with_workspace_in_tmux(self):
        self.receive_and_send(mux='tmux')

    @unittest.skipUnless(shutil.which('mosh') and shutil.which('mosh-server') and
                         shutil.which('screen'), 'Mosh/GNU Screen unavailable')
    def test_transfers_with_workspace_in_mosh_screen(self):
        self.receive_and_send(mosh=True, mux='screen')

    @unittest.skipUnless(shutil.which('mosh') and shutil.which('mosh-server') and
                         shutil.which('tmux'), 'Mosh/tmux unavailable')
    def test_transfers_with_workspace_in_mosh_tmux(self):
        self.receive_and_send(mosh=True, mux='tmux')

    @unittest.skipUnless(shutil.which('mosh') and shutil.which('mosh-server') and
                         shutil.which('tmux'), 'Mosh/tmux unavailable')
    def test_resize_during_mosh_tmux_upload_restores_both_panes(self):
        self.receive_and_send(background=True, resize=True, mosh=True, mux='tmux')

    @unittest.skipUnless(shutil.which('sshd') and shutil.which('ssh-keygen') and
                         shutil.which('screen') and os.getuid() == 0 and
                         Path('/run/sshd').is_dir(), 'isolated OpenSSH/Screen requires root')
    def test_transfers_with_workspace_in_ssh_screen(self):
        self.receive_and_send(ssh=True, mux='screen')

    @unittest.skipUnless(shutil.which('sshd') and shutil.which('ssh-keygen') and
                         shutil.which('tmux') and os.getuid() == 0 and
                         Path('/run/sshd').is_dir(), 'isolated OpenSSH/tmux requires root')
    def test_transfers_with_workspace_in_ssh_tmux(self):
        self.receive_and_send(ssh=True, mux='tmux')

    def test_cancelled_upload_returns_without_attachments_or_model_input(self):
        child = self.start('-N', 'cancel-receive',
                           transport=[str(control.frontend.BINARY), 'remote'])
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(b'i/receive\r')
        child.until(b'Select local file', 10)
        child.output.clear()
        child.write(b'\x03')
        self.return_to_workspace(child)
        self.show_report(child, '/receive')
        child.repaint_until(b'Upload cancelled; no files attached.')
        child.command('history')
        child.write(b'i/attachments\r')
        child.repaint_until(b'0 unsent attachment(s)')
        child.write(b'\x1b')
        child.read(.06)
        child.finish('close')
        self.assertEqual(self.inputs(), [])
        self.owner.status('detached')

    @unittest.skipUnless(shutil.which('sshd') and shutil.which('ssh-keygen') and
                         hasattr(os, 'pidfd_open') and os.getuid() == 0 and
                         Path('/run/sshd').is_dir(), 'isolated OpenSSH requires Linux root')
    def test_ssh_loss_at_upload_picker_restores_terminal_and_workspace(self):
        child = self.transfer_terminal('lost-upload', ssh=True)
        child.command('attach ' + self.owner.sid)
        child.attached()
        path = self.root / 'retained.txt'
        body = b'previously retained attachment\n'
        path.write_bytes(body)
        child.write(('i/attach ' + str(path) + '\r').encode())
        child.repaint_until(b'1 unsent attachment(s)')
        self.show_report(child, '/attach ' + str(path))
        child.command('history')
        child.write(b'i/receive\rnewer workspace draft')
        child.until(b'Select local file', 10)
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] ==
            'newer workspace draft')

        # Only terminate the private transport, leaving the wrapper's local
        # input open and the independently owned engine alive.
        wrapper = child.state()['pid']
        children = clipboard.FixtureChildren(wrapper, parent=child.process.pid)
        self.addCleanup(children.close)
        matches = []
        for pid, fd in children.handles.items():
            argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if argv[0] == b'ssh' and str(self.root / 'client-key').encode() in argv:
                matches.append(fd)
        transport, = matches
        signal.pidfd_send_signal(transport, signal.SIGTERM)
        child.wait_exit()
        self.assertNotEqual(child.process.returncode, 0)
        self.assertEqual(normalized_modes(child.state()['modes']), child.original)
        self.owner.status('detached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

        resumed = self.start('--resume', 'lost-upload', expect=b'\x1b[?1049h')
        resumed.repaint_until(b'newer workspace draft')
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None)
        resumed.command('history')
        resumed.write(b'i')
        control.ControlTests.escape(self, resumed)
        resumed.write(b'0d$i/attachments\r')
        resumed.repaint_until(b'1 unsent attachment(s)')
        self.assertEqual([p.read_bytes() for p in (self.owner.directory / 'media').iterdir()],
                         [body])
        self.assertEqual(self.inputs(), [])
        self.assertFalse(list(self.owner.directory.glob('upload-*')))
        resumed.write(b'\x1b')
        resumed.read(.06)
        resumed.finish('close')
        self.owner.status('detached')

    def reconnect_upload(self, protocol, mux):
        binary = control.frontend.BINARY
        child = self.transfer_terminal('mux-loss', ssh=protocol == 'ssh',
                                       mosh=protocol == 'mosh', mux=mux)
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.command('vsp')
        child.write(b'i/receive\rretained workspace draft')
        child.until(b'Select local file', 10)
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'] ==
            'retained workspace draft')
        tmux = ['tmux', '-S', str(self.root / 'transfer-tmux.sock')]
        pids = []
        for path in Path('/proc').glob('[0-9]*/cmdline'):
            try:
                argv = path.read_bytes().split(b'\0')
            except (FileNotFoundError, ProcessLookupError):
                continue
            if argv[:2] == [str(binary).encode(), b'vm'] and (
                    str(self.root / 'state').encode() in argv):
                pids.append(path.parent.name)
        pid, = pids

        def identity():
            return Path('/proc/' + pid + '/stat').read_text().rsplit(')', 1)[1].split()[19]

        original = identity()
        wrapper = child.state()['pid']
        children = clipboard.FixtureChildren(wrapper, parent=child.process.pid)
        self.addCleanup(children.close)
        matches = []
        for process, fd in children.handles.items():
            argv = Path(f'/proc/{process}/cmdline').read_bytes().split(b'\0')
            if (protocol == 'ssh' and argv[0] == b'ssh' and
                str(self.root / 'client-key').encode() in argv) or (
                protocol == 'mosh' and Path(os.fsdecode(argv[0])).name == 'mosh-client'):
                matches.append(fd)
        transport, = matches
        signal.pidfd_send_signal(transport, signal.SIGTERM)
        child.wait_exit()
        self.assertEqual(normalized_modes(child.state()['modes']), child.original)
        self.assertEqual(identity(), original)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

        script = self.root / 'reattach.py'
        attach = [*tmux, 'attach-session', '-d', '-t', 'transfer'] if mux == 'tmux' else (
            ['screen', '-U', '-c', str(self.root / 'screenrc'), '-d', '-r', 'transfer'])
        script.write_text('import os\n' +
                          f'os.execvp({attach[0]!r}, {attach!r})\n')
        tunnel = [sys.executable, str(self.root / 'ssh-transport.py')] if protocol == 'ssh' else (
            ['mosh', '--local', '--predict=never', '127.0.0.1'])
        resumed = control.frontend.Terminal(self.root, columns=200,
            transport=[str(binary), 'remote', *tunnel, sys.executable, str(script)],
            extra_env={'SCREENDIR': str(self.root / 'screens')})
        self.addCleanup(resumed.close)
        self.terminals.append(resumed)
        self._terminals.append(resumed)
        resumed.until(b'host-model', 10)
        resumed.write(b'\x03')
        self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None)
        resumed.attached()
        self.assertEqual(identity(), original)
        self.assertEqual(self.inputs(), [])
        saved = next(iter(self.snapshots().values()))['state']['buffers'][0]
        self.assertEqual([r['command'] for r in saved['reports']], ['/receive'])
        resumed.command('history')
        resumed.repaint_until(b'retained workspace draft')
        resumed.write(b'\t0d$i/receive\r')
        resumed.until(b'Select local file', 10)
        source = self.root / 'resumed.txt'
        payload = 'reconnected file é界\n' * 50
        source.write_text(payload)
        resumed.output.clear()
        resumed.write((str(source) + '\r').encode())
        resumed.until(b'1 unsent attachment(s)', 15)
        self.show_report(resumed, '/receive')
        resumed.command('history')
        requests = []
        previous = self.owner.provider.runtime_handler

        def record(handler, request, sequence):
            requests.append(request)
            return previous(handler, request, sequence)

        self.owner.provider.runtime_handler = record
        resumed.write(b'iinspect resumed file\r')
        deadline = time.monotonic() + 10
        while not self.inputs() or not requests:
            self.assertLess(time.monotonic(), deadline)
            resumed.read(.05)
        event, = self.inputs()
        part, = event['data']['content']
        self.assertEqual(part['asset']['sha256'], hashlib.sha256(payload.encode()).hexdigest())
        self.assertEqual((self.owner.directory / 'media' / part['asset']['id']).read_bytes(),
                         payload.encode())
        request, = requests
        self.assertIn('asset:' + part['asset']['id'], json.dumps(request))
        self.assertIn('inspect resumed file', json.dumps(request))
        resumed.write(b'\x1b:close\r')
        resumed.finish('close')
        self.owner.status('detached')

    @unittest.skipUnless(shutil.which('tmux') and hasattr(os, 'pidfd_open') and
                         shutil.which('sshd') and shutil.which('ssh-keygen') and
                         os.getuid() == 0 and Path('/run/sshd').is_dir(),
                         'isolated ssh/tmux reconnect needs Linux fixtures')
    def test_upload_reconnect_in_ssh_tmux(self):
        self.reconnect_upload('ssh', 'tmux')

    @unittest.skipUnless(shutil.which('screen') and hasattr(os, 'pidfd_open') and
                         shutil.which('sshd') and shutil.which('ssh-keygen') and
                         os.getuid() == 0 and Path('/run/sshd').is_dir(),
                         'isolated ssh/screen reconnect needs Linux fixtures')
    def test_upload_reconnect_in_ssh_screen(self):
        self.reconnect_upload('ssh', 'screen')

    @unittest.skipUnless(shutil.which('tmux') and hasattr(os, 'pidfd_open') and
                         shutil.which('mosh') and shutil.which('mosh-server'),
                         'isolated mosh/tmux reconnect needs Linux fixtures')
    def test_upload_reconnect_in_mosh_tmux(self):
        self.reconnect_upload('mosh', 'tmux')

    @unittest.skipUnless(shutil.which('screen') and hasattr(os, 'pidfd_open') and
                         shutil.which('mosh') and shutil.which('mosh-server'),
                         'isolated mosh/screen reconnect needs Linux fixtures')
    def test_upload_reconnect_in_mosh_screen(self):
        self.reconnect_upload('mosh', 'screen')

    def test_attach_list_remove_and_failure_return_to_workspace(self):
        path = self.root / 'local notes.txt'
        path.write_text('local attachment\n')
        child = self.attached('local-files')
        child.write(('i/attach ' + str(path) + '\r').encode())
        self.show_report(child, '/attach ' + str(path))
        child.repaint_until(b'1 unsent attachment(s)')
        for command, count in (('/attachments', 1), ('/detach all', 0)):
            child.command('history')
            child.attached()
            child.write(('i' + command + '\r').encode())
            self.wait_snapshot(lambda rows:
                rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None and
                any(report['command'] == command for report in
                    next(iter(rows.values()))['state']['buffers'][0]['reports']))
            self.show_report(child, command)
            child.repaint_until(str(count).encode() + b' unsent attachment(s)')
            self.assertNotIn(b'\x1b[?1049l', child.output)
        child.command('history')
        child.write(('i/attach ' + str(self.root / 'missing.txt') + '\r').encode())
        self.show_report(child, '/attach ' + str(self.root / 'missing.txt'))
        child.repaint_until(b'No such file')
        child.finish('close')
        self.assertEqual(self.inputs(), [])
        self.owner.status('detached')

    def test_old_completion_preserves_replacement_terminal(self):
        child = self.attached('replacement')
        child.write(b'i/config\r')
        child.until(b'terminal-editor-ready', 10)
        child.signal(signal.SIGTERM)
        child.wait_exit()
        self.owner.status('detached')
        replacement = self.owner.start(['--resume', self.owner.sid])
        replacement.until(b'session is running; attaching')
        self.owner.status('attached')
        os.write(replacement.master, b'done\r')
        self.owner.wait_event('control_finished')
        replacement.until(b'configuration unchanged:')
        self.assertIsNone(replacement.process.poll())
        self.owner.status('attached')
        self.owner.finish(replacement, b'/s d')
        restored = self.start('--resume', 'replacement', expect=b'\x1b[?1049h')
        self.show_report(restored, '/config')
        restored.finish('close')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')

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
        child.output.clear()
        child.write(b'done\r')
        self.owner.wait_event('control_finished')
        self.return_to_workspace(child)
        child.repaint_until(b'newer workspace draft')
        child.finish('close')
        self.owner.status('detached')
        resumed = self.start('--resume', 'both-drafts', expect=b'\x1b[?1049h')
        resumed.repaint_until(b'newer workspace draft')
        rows = self.wait_snapshot(lambda rows:
            rollout(next(iter(rows.values()))['state']['buffers'][0])['pending'] is None)
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'],
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
        rollout(saved['state']['buffers'][0])['pending'] = {
            'id': request, 'instance': peer.capabilities['instance'], 'text': '/config'}
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'manual', expect=b'\x1b[?1049h')
        resumed.repaint_until(b'Command needs :classic')
        self.assertFalse((self.root / 'editor-runs').exists())
        resumed.command('classic')
        resumed.until(b'terminal-editor-ready', 10)
        resumed.output.clear()
        resumed.write(b'done\r')
        self.owner.wait_event('control_finished')
        self.return_to_workspace(resumed)
        self.show_report(resumed, '/config')
        resumed.finish('q')
        self.assertEqual((self.root / 'editor-runs').read_text(), 'started\n')
        self.owner.status('detached')


if __name__ == '__main__':
    unittest.main()
