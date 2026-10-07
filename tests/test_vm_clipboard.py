# SPDX-License-Identifier: GPL-2.0-only
"""Explicit Vim copies through isolated PTYs and real remote wrappers."""

import base64
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

import test_vm_frontend as frontend
from test_upload_client import FixtureChildren
from test_vm_frontend import rollout


class ClipboardTests(unittest.TestCase):
    setUp = frontend.WorkspaceTests.setUp
    start = frontend.WorkspaceTests.start
    seed_session = frontend.WorkspaceTests.seed_session
    snapshots = frontend.WorkspaceTests.snapshots
    wait_snapshot = frontend.WorkspaceTests.wait_snapshot

    def ssh_transport(self, extra_env):
        for name in ('host-key', 'client-key'):
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '',
                            '-f', str(self.root / name)], check=True, timeout=10)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        config = self.root / 'sshd.conf'
        config.write_text(f'Port {port}\nListenAddress 127.0.0.1\n'
                          f'HostKey {self.root / "host-key"}\n'
                          f'PidFile {self.root / "sshd.pid"}\n'
                          f'AuthorizedKeysFile {self.root / "client-key.pub"}\n'
                          'PermitRootLogin yes\nPasswordAuthentication no\n'
                          'KbdInteractiveAuthentication no\nUsePAM no\nStrictModes no\n'
                          'AllowTcpForwarding no\nX11Forwarding no\n')
        key = (self.root / 'host-key.pub').read_text().split()
        known = self.root / 'known-hosts'
        known.write_text(f'[127.0.0.1]:{port} {key[0]} {key[1]}\n')
        log = (self.root / 'sshd.log').open('wb')
        self.addCleanup(log.close)
        server = subprocess.Popen([shutil.which('sshd'), '-D', '-e', '-f', str(config)],
                                  stdout=log, stderr=log)
        children = FixtureChildren(server.pid)
        self.addCleanup(server.wait, timeout=5)
        self.addCleanup(children.close)
        deadline = time.monotonic() + 5
        while True:
            self.assertIsNone(server.poll(), (self.root / 'sshd.log').read_text())
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.1):
                    break
            except OSError:
                self.assertLess(time.monotonic(), deadline, 'private sshd did not listen')
                time.sleep(.02)
        ssh = ['ssh', '-tt', '-p', str(port), '-i', str(self.root / 'client-key'),
               '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
               '-o', 'StrictHostKeyChecking=yes', '-o', f'UserKnownHostsFile={known}',
               '-o', 'GlobalKnownHostsFile=/dev/null', 'root@127.0.0.1']
        environment = ['env', f'HOME={self.root}', 'LC_ALL=C.UTF-8',
                       'TERM=xterm-256color', *(f'{k}={v}' for k, v in extra_env.items())]
        script = self.root / 'ssh-transport.py'
        script.write_text('import os, shlex, sys\n'
                          f'command = {"cd " + shlex.quote(str(self.root)) + " && exec "!r}'
                          f' + shlex.join({environment!r} + sys.argv[1:])\n'
                          f'os.execvp("ssh", {ssh!r} + [command])\n')
        return [sys.executable, str(script)]

    def open_text(self, text='copy é界 literal', *, wrappers=0, outer='osc52',
                  retained=False, extra_env=None, mux=None, outer_mux=False, mosh=False,
                  ssh=False):
        journal = self.seed_session(text + '\n' + 'padding\n' * 15 + 'tail-marker')
        (self.root / 'state' / 'config.ini').write_text('[terminal]\nclipboard=osc52\n')
        config = self.root / 'workstation.ini'
        config.write_text('[terminal]\nclipboard=' + outer + '\n')
        transport = []
        extra_env = dict(extra_env or {})
        multiplexer = []
        if mux == 'tmux':
            tmux = [shutil.which('tmux'), '-S', str(self.root / 'clipboard-tmux.sock')]
            self.addCleanup(subprocess.run, [*tmux, 'kill-server'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            multiplexer = [*tmux, '-f', '/dev/null', 'new-session', '-s', 'clipboard', '--']
        elif mux == 'screen':
            sockets = self.root / 'screens'
            sockets.mkdir(mode=0o700)
            extra_env['SCREENDIR'] = str(sockets)
            screenrc = self.root / 'screenrc'
            screenrc.write_text('startup_message off\naltscreen on\n')
            multiplexer = [shutil.which('screen'), '-U', '-c', str(screenrc), '-S', 'clipboard']
            self.addCleanup(subprocess.run, ['screen', '-S', 'clipboard', '-X', 'quit'],
                            env={**os.environ, **extra_env}, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, timeout=5)
        if outer_mux:
            transport.extend(multiplexer)
        for _ in range(wrappers):
            transport.extend([str(frontend.BINARY), 'remote', '--config', str(config)])
        if ssh:
            transport.extend(self.ssh_transport(extra_env))
        if mosh:
            transport.extend(['mosh', '--local', '--predict=never', '127.0.0.1'])
        if not outer_mux:
            transport.extend(multiplexer)
        if retained:
            transport.extend([sys.executable, str(Path(__file__).resolve().with_name(
                'fixture_screen_state.py'))])
        child = self.start('-N', 'clipboard', columns=120, transport=transport, extra_env=extra_env)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'tail-marker')
        child.write(b'/copy\r')
        child.repaint_until(b'Match')
        return journal, child

    def copies(self, child, count=1):
        deadline = time.monotonic() + 10
        results = []
        while time.monotonic() < deadline:
            child.read(.05)
            results = re.findall(rb'\x1b]52;c;([A-Za-z0-9+/=]*)\x07', child.output)
            if len(results) >= count:
                return [base64.b64decode(value, validate=True) for value in results]
        self.fail(f'clipboard sequences {len(results)}/{count}: {bytes(child.output[-3000:])!r}')

    def test_local_visual_yank_and_register_are_literal(self):
        journal, child = self.open_text('copy é界 `{\\rtf1 literal}`')
        before = journal.read_bytes()
        child.write(b'v$y')
        self.assertEqual(self.copies(child), ['copy é界 {\\rtf1 literal}'.encode()])
        child.repaint_until(b'acceptance unconfirmed')
        child.write(b'P')
        child.command('workspace name copied')
        rows = self.wait_snapshot(lambda rows: rows and next(iter(rows.values()))['name'] == 'copied')
        draft = rollout(next(iter(rows.values()))['state']['buffers'][0])['draft']
        self.assertEqual(draft, 'copy é界 {\\rtf1 literal}')
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_nested_wrappers_publish_once_and_hide_protocol(self):
        journal, child = self.open_text(wrappers=2)
        before = journal.read_bytes()
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.repaint_until(b'acceptance unconfirmed')
        self.assertNotIn(b'SNAJPAGENT-SCREEN/1', child.output)
        self.assertEqual(journal.read_bytes(), before)
        child.finish()

    def test_outer_off_preserves_register_without_local_fallback(self):
        _, child = self.open_text(wrappers=1, outer='off')
        child.write(b'yy')
        child.repaint_until(b'Workstation clipboard unavailable')
        self.assertNotIn(b'\x1b]52;', child.output)
        child.write(b'P')
        child.command('workspace name retained')
        rows = self.wait_snapshot(lambda rows: rows and next(iter(rows.values()))['name'] == 'retained')
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'],
                         '• copy é界 literal\n')
        child.finish()

    def test_composer_delete_is_internal_and_explicit_yank_copies(self):
        _, child = self.open_text()
        child.write(b'iword remainder\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')
        child.write(b'0dw')
        child.command('workspace name deleted')
        self.wait_snapshot(lambda rows: rows and next(iter(rows.values()))['name'] == 'deleted')
        self.assertNotIn(b'\x1b[?9001;', child.output)
        self.assertNotIn(b'\x1b]52;', child.output)
        child.write(b'yy')
        self.assertEqual(self.copies(child), [b'remainder\n'])
        child.finish()

    def test_cancel_during_negotiation_preserves_register(self):
        _, child = self.open_text()
        child.write(b'yy')
        child.until(b'\x1b[?9001;')
        child.write(b'\x03')
        child.repaint_until(b'copy canceled; register retained')
        self.assertNotIn(b'\x1b]52;', child.output)
        child.write(b'P')
        child.command('workspace name canceled')
        rows = self.wait_snapshot(lambda rows: rows and next(iter(rows.values()))['name'] == 'canceled')
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'],
                         '• copy é界 literal\n')
        child.finish()

    def test_large_file_register_streams_through_wrapper(self):
        text = 'copy ' + 'é界' * 40000
        _, child = self.open_text(text, wrappers=1)
        child.write(b'v$y')
        self.assertEqual(self.copies(child), [text.encode()])
        child.finish()

    def backpressured_copy(self, wrappers):
        text = 'copy ' + 'é界' * 60000
        _, child = self.open_text(text, wrappers=wrappers)
        child.write(b'v$y')
        child.until(b'\x1b]52;c;')
        # Stop draining the destination PTY while its output queue fills. Input
        # and workspace persistence must still progress during the open OSC.
        time.sleep(.1)
        child.command('workspace name backpressured')
        deadline = time.monotonic() + 3
        while True:
            rows = self.snapshots()
            if rows and next(iter(rows.values()))['name'] == 'backpressured':
                break
            self.assertLess(time.monotonic(), deadline, 'clipboard output blocked input')
            time.sleep(.02)
        self.assertEqual(self.copies(child), [text.encode()])
        child.finish()

    def test_local_copy_backpressure_keeps_input_responsive(self):
        self.backpressured_copy(0)

    def test_wrapper_copy_backpressure_keeps_input_responsive(self):
        self.backpressured_copy(1)

    def interrupted_copy(self, wrappers):
        _, child = self.open_text('copy ' + 'x' * 400000, wrappers=wrappers)
        child.write(b'v$y')
        child.until(b'\x1b]52;c;')
        time.sleep(.1)
        child.signal(signal.SIGTERM)
        time.sleep(.08)
        child.wait_exit()
        self.assertEqual(child.process.returncode, 0)
        self.assertEqual(frontend.normalized_modes(child.state()['modes']), child.original)
        output = bytes(child.output)
        begin = output.index(b'\x1b]52;c;')
        restore = len(output) if wrappers else output.index(b'\x1b[?1049l', begin)
        self.assertIn(b'\x18', output[begin:restore], 'open OSC was not canceled before restore')

    def test_signal_aborts_blocked_osc_before_restoring_terminal(self):
        self.interrupted_copy(0)

    def test_wrapper_signal_aborts_blocked_osc(self):
        self.interrupted_copy(1)

    def test_retained_title_retries_preserve_literal_selection(self):
        text = 'copy ' + '界é' * 100
        _, child = self.open_text(text, wrappers=1, retained=True,
                                 extra_env={'SCREEN_FIXTURE_DROP_FIRST': '1',
                                            'SCREEN_FIXTURE_DUPLICATE': '1',
                                            'SCREEN_FIXTURE_REPLAY': '1'})
        child.write(b'v$y')
        self.assertEqual(self.copies(child), [text.encode()])
        self.assertNotIn(b'SNAJPAGENT-SCREEN/1', child.output)
        child.finish()

    def test_new_yank_supersedes_prepublication_copy(self):
        _, child = self.open_text('copy first\nsecond selection')
        child.write(b'yy')
        child.until(b'\x1b[?9001;')
        child.write(b'/second\r')
        child.repaint_until(b'Match')
        child.write(b'yy')
        self.assertEqual(self.copies(child), [b'  second selection\n'])
        child.finish()

    @unittest.skipIf(sys.platform == 'darwin', 'Mac native API uses a private board in the C test')
    def test_native_workstation_helper_publishes_once(self):
        program = self.root / 'wl-copy'
        target = self.root / 'clipboard.txt'
        count = self.root / 'clipboard-count'
        program.write_text('#!' + sys.executable + '\n'
                           'import pathlib, sys\n'
                           'assert sys.argv[1:] == ["--type", "text/plain;charset=utf-8"]\n'
                           f'pathlib.Path({str(target)!r}).write_bytes(sys.stdin.buffer.read())\n'
                           f'with open({str(count)!r}, "ab") as f: f.write(b"1")\n')
        program.chmod(0o700)
        _, child = self.open_text(wrappers=2, outer='native',
                                 extra_env={'PATH': str(self.root) + os.pathsep + os.environ['PATH'],
                                            'WAYLAND_DISPLAY': 'fixture'})
        child.write(b'yy')
        child.repaint_until(b'Workstation clipboard written', timeout=10)
        self.assertEqual(target.read_bytes(), '• copy é界 literal\n'.encode())
        self.assertEqual(count.read_bytes(), b'1')
        self.assertNotIn(b'\x1b]52;', child.output)
        child.finish()

    @unittest.skipUnless(shutil.which('tmux'), 'tmux unavailable')
    def test_workspace_inside_tmux_routes_to_outer_wrapper(self):
        _, child = self.open_text(wrappers=1, mux='tmux')
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('screen'), 'GNU Screen unavailable')
    def test_workspace_inside_screen_routes_to_outer_wrapper(self):
        _, child = self.open_text(wrappers=1, mux='screen')
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('screen'), 'GNU Screen unavailable')
    def test_wrapper_inside_screen_publishes_to_workstation_terminal(self):
        _, child = self.open_text(wrappers=1, mux='screen', outer_mux=True)
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('mosh') and shutil.which('mosh-server'), 'Mosh unavailable')
    def test_stock_mosh_carries_checked_clipboard_titles(self):
        _, child = self.open_text(wrappers=1, mosh=True)
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        self.assertNotIn(b'SNAJPAGENT-SCREEN/1', child.output)
        child.finish()

    @unittest.skipUnless(shutil.which('mosh') and shutil.which('mosh-server') and
                         shutil.which('screen'), 'Mosh/GNU Screen unavailable')
    def test_stock_mosh_with_remote_screen(self):
        _, child = self.open_text(wrappers=1, mosh=True, mux='screen')
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('mosh') and shutil.which('mosh-server') and
                         shutil.which('tmux'), 'Mosh/tmux unavailable')
    def test_stock_mosh_with_remote_tmux(self):
        _, child = self.open_text(wrappers=1, mosh=True, mux='tmux')
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('sshd') and shutil.which('ssh-keygen') and
                         os.getuid() == 0 and Path('/run/sshd').is_dir(),
                         'isolated OpenSSH fixture requires root and /run/sshd')
    def test_ssh_clipboard_uses_workstation_wrapper(self):
        _, child = self.open_text(wrappers=1, ssh=True)
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('sshd') and shutil.which('ssh-keygen') and
                         shutil.which('screen') and os.getuid() == 0 and
                         Path('/run/sshd').is_dir(), 'isolated OpenSSH/Screen requires root')
    def test_ssh_with_remote_screen(self):
        _, child = self.open_text(wrappers=1, ssh=True, mux='screen')
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    @unittest.skipUnless(shutil.which('sshd') and shutil.which('ssh-keygen') and
                         shutil.which('tmux') and os.getuid() == 0 and
                         Path('/run/sshd').is_dir(), 'isolated OpenSSH/tmux requires root')
    def test_ssh_with_remote_tmux(self):
        _, child = self.open_text(wrappers=1, ssh=True, mux='tmux')
        child.write(b'yy')
        self.assertEqual(self.copies(child), ['• copy é界 literal\n'.encode()])
        child.finish()

    def test_quit_settles_prepublication_copy_and_restores_modes(self):
        _, child = self.open_text()
        child.write(b'yy')
        child.until(b'\x1b[?9001;')
        child.finish()
        self.assertNotIn(b'\x1b]52;', child.output)

    def test_late_private_replies_are_inert_in_composer(self):
        _, child = self.open_text()
        child.write(b'i\x1b[>12345S\x1b[>9003;1;2;3;4;5;6;7;8cplain\x1b')
        time.sleep(.06)
        child.command('workspace name replies')
        rows = self.wait_snapshot(lambda rows: rows and next(iter(rows.values()))['name'] == 'replies')
        self.assertEqual(rollout(next(iter(rows.values()))['state']['buffers'][0])['draft'], 'plain')
        self.assertNotIn(b'\x1b]52;', child.output)
        child.finish()


if __name__ == '__main__':
    unittest.main()
