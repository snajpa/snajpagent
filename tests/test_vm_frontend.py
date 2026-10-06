# SPDX-License-Identifier: GPL-2.0-only
"""Workspace CLI, real terminal ownership, persistence and input boundaries."""

import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
import uuid
from pathlib import Path

import tmux_terminal as harness


BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else (
    Path(__file__).resolve().parents[1] / 'snajpagent')


# Keep the controlling process alive until the child's restored modes have been
# captured. Darwin revokes a session leader's slave PTY on exit, even while the
# test holds an open descriptor. This also makes suspend state observable.
KEEPER = r"""
import json, os, subprocess, sys, termios
from pathlib import Path

path = Path(sys.argv[1])
p = subprocess.Popen(sys.argv[2:])
def record(state, **fields):
    tmp = path.with_suffix('.new')
    tmp.write_text(json.dumps(dict(pid=p.pid, state=state, **fields)))
    tmp.replace(path)
record('running')
while True:
    _, status = os.waitpid(p.pid, os.WUNTRACED | os.WCONTINUED)
    if os.WIFSTOPPED(status):
        record('stopped')
    elif os.WIFCONTINUED(status):
        record('running')
    else:
        p.returncode = os.waitstatus_to_exitcode(status)
        modes = termios.tcgetattr(0)
        modes[6] = [x[0] if isinstance(x, bytes) else x for x in modes[6]]
        record('exited', modes=modes, returncode=p.returncode)
        sys.exit(p.returncode)
"""


def normalized_modes(modes):
    modes = list(modes)
    # PENDIN records pending kernel retyping, not an application mode setting.
    modes[3] &= ~getattr(termios, 'PENDIN', 0)
    modes[6] = [x[0] if isinstance(x, bytes) else x for x in modes[6]]
    return modes


def rollout(owner):
    """Return the composer in current or pre-conversation workspace snapshots."""
    if 'buffers' not in owner:
        return owner
    return next(buffer for buffer in owner['buffers'] if buffer['route'] == 'rollout')


class Terminal:
    def __init__(self, root, args=(), rows=12, columns=100, transport=(), extra_env=None,
                 subcommand='vm'):
        # Ordinary UI fixtures keep all yanks inside their register. Clipboard
        # fixtures opt in with their own isolated policy and destination.
        config = root / 'state' / 'config.ini'
        config.parent.mkdir(mode=0o700, exist_ok=True)
        settings = config.read_text() if config.exists() else ''
        if '[terminal]' not in settings:
            config.write_text(settings + '\n[terminal]\nclipboard=off\n')
        self.master, self.slave = pty.openpty()
        self.original = normalized_modes(termios.tcgetattr(self.slave))
        self.receipt = root / ('terminal-' + uuid.uuid4().hex + '.json')
        self.resize(rows, columns)
        env = dict(os.environ, HOME=str(root), TERM='xterm-256color')
        if extra_env:
            env.update(extra_env)
            env = {key: value for key, value in env.items() if value is not None}
        for key in ('TMUX', 'TMUX_PANE', 'STY', 'SNAJPAGENT_DOTDIR', 'OPENAI_API_KEY'):
            env.pop(key, None)

        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)

        self.process = subprocess.Popen([sys.executable, '-c', KEEPER, str(self.receipt),
                                         *transport, str(BINARY),
                                         *([subcommand] if subcommand else []), '--dotdir',
                                         str(root / 'state'), *args],
                                        stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                        cwd=root, env=env, preexec_fn=controlling_terminal)
        self.output = bytearray()

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        if hasattr(self, 'process'):
            self.signal(signal.SIGWINCH)

    def state(self):
        return json.loads(self.receipt.read_text()) if self.receipt.exists() else {}

    def signal(self, number):
        state = self.state()
        if state and state['state'] != 'exited':
            try:
                os.kill(state['pid'], number)
            except ProcessLookupError:
                pass

    def read(self, timeout=.05):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return b''
            self.output.extend(data)
            return data
        return b''

    def until(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.output and time.monotonic() < deadline:
            self.read()
        if marker not in self.output:
            raise AssertionError((marker, self.process.poll(), bytes(self.output[-5000:])))

    def repaint_until(self, marker, timeout=5):
        # A forced frame provides complete visible text. Normal grid diffs can
        # reuse a prefix already on screen and emit only the changed suffix.
        self.output.clear()
        deadline = time.monotonic() + timeout
        while marker not in self.output and time.monotonic() < deadline:
            self.write(b'\x0c')
            self.read(.1)
        if marker not in self.output:
            raise AssertionError((marker, bytes(self.output[-5000:])))

    def write(self, text):
        os.write(self.master, text)

    def command(self, text):
        self.write(b':' + text.encode() + b'\r')

    def finish(self, command='qa'):
        if self.process.poll() is None:
            self.command(command)
        self.wait_exit()
        assert self.process.poll() == 0, bytes(self.output[-3000:])
        assert normalized_modes(self.state()['modes']) == self.original, 'terminal modes were not restored'
        assert b'\x1b[?1049l' in self.output, 'alternate screen was not released'

    def wait_exit(self):
        # Drain output while the controlling process exits. A Darwin PTY's
        # final close can wait for its pending output to be consumed.
        deadline = time.monotonic() + 5
        while self.process.poll() is None and time.monotonic() < deadline:
            self.read()
        self.read(0)
        assert self.process.poll() is not None, bytes(self.output[-3000:])

    def close(self):
        try:
            if self.process.poll() is None:
                self.signal(signal.SIGCONT)
                self.signal(signal.SIGTERM)
                deadline = time.monotonic() + 3
                while self.process.poll() is None and time.monotonic() < deadline:
                    self.read()
                if self.process.poll() is None:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait(3)
        finally:
            os.close(self.master)
            os.close(self.slave)


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='snag-vm-',
                                               dir='/private/tmp' if sys.platform == 'darwin' else None)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self._terminals = []

    def start(self, *args, expect=b'sessions', **kwargs):
        child = Terminal(self.root, args, **kwargs)
        self.addCleanup(child.close)
        if not hasattr(self, '_terminals'):
            self._terminals = []
        self._terminals.append(child)
        child.until(b'\x1b[?1049h')
        child.until(expect)
        return child

    def snapshots(self):
        return {path.parent.name: json.loads(path.read_text()) for path in
                (self.root / 'state' / 'workspaces').glob('*/workspace.json')}

    def wait_snapshot(self, predicate):
        deadline = time.monotonic() + 5
        values = {}
        while time.monotonic() < deadline:
            for child in getattr(self, '_terminals', []):
                while child.read(0):
                    pass
            values = self.snapshots()
            if predicate(values):
                return values
            time.sleep(.02)
        self.fail(f'snapshot did not reach expected state: {values}')

    def cli(self, *args):
        return subprocess.run([str(BINARY), 'vm', '--dotdir', str(self.root / 'state'), *args],
                              env={**os.environ, 'HOME': str(self.root)},
                              capture_output=True, timeout=5)

    def seed_session(self, response='retained-answer-marker'):
        provider = harness.FakeResponses()
        self.addCleanup(provider.close)

        chunks = [response[i:i + 500000] for i in range(0, len(response), 500000)]
        replies = iter(chunks)

        def respond(handler, request, sequence):
            provider.reply(handler, provider.response_body(sequence, next(replies)).encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        self.fixture_provider = provider
        self.fixture_config = config
        harness.write_irc_config(config, provider.port, 'host-model')
        journal = None
        for _ in chunks:
            resume = ['--resume', journal.parent.name] if journal else []
            result = subprocess.run([str(BINARY), '--config', str(config), '--dotdir',
                                     str(self.root / 'state'), *resume, '-e', '--',
                                     'retained-question-marker'],
                                    cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                       'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                    capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            journal, = (self.root / 'state' / 'sessions').glob('*/events.jsonl')
        return journal

    def append_turn(self, journal, answer):
        provider = self.fixture_provider

        def respond(handler, request, sequence):
            provider.reply(handler, provider.response_body(sequence, answer).encode())

        provider.runtime_handler = respond
        result = subprocess.run([str(BINARY), '--config', str(self.fixture_config), '--dotdir',
                                 str(self.root / 'state'), '--resume', journal.parent.name,
                                 '-e', '--', 'next-question-marker'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_follow_hold_and_idle_polling(self):
        journal = self.seed_session()
        child = self.start('-N', 'following')
        child.command('history ' + journal.parent.name)
        child.until(b'retained-answer-marker')
        while child.read(.1):
            pass
        # Idle observation must not repaint or reload the transcript.
        self.assertEqual(child.read(1.2), b'')
        self.append_turn(journal, 'live-append-one')
        child.until(b'live-append-one')
        child.write(b'gg')
        values = self.wait_snapshot(lambda values:
            not next(iter(values.values()))['state']['windows'][0]['history']['follow'] and
            next(iter(values.values()))['state']['windows'][0]['history']['seq'] == 1)
        anchor = next(iter(values.values()))['state']['windows'][0]['history']
        self.append_turn(journal, 'live-append-two')
        child.until(b'newer')
        child.command('workspace save')
        self.assertEqual(next(iter(self.snapshots().values()))['state']['windows'][0]['history'], anchor)
        child.write(b'G')
        self.wait_snapshot(lambda values:
            next(iter(values.values()))['state']['windows'][0]['history']['follow'] and
            next(iter(values.values()))['state']['windows'][0]['history']['seq'] > anchor['seq'])
        # The tail snapshot can precede its final response projection; a later
        # grid diff may reuse "live-append-" already visible on another row.
        child.repaint_until(b'live-append-two')
        child.finish()

    def test_replaced_history_stops_follow_until_explicit_retry(self):
        journal = self.seed_session()
        child = self.start('-N', 'source-change')
        child.command('history ' + journal.parent.name)
        child.until(b'retained-answer-marker')
        original = journal.with_name('original-events')
        journal.rename(original)
        journal.write_bytes(original.read_bytes())
        journal.chmod(0o600)
        child.repaint_until(b'history was replaced or truncated')
        while child.read(.1):
            pass
        self.assertEqual(child.read(1.2), b'')
        journal.unlink()
        original.rename(journal)
        child.write(b'G')
        self.append_turn(journal, 'recovered-history-marker')
        child.repaint_until(b'recovered-history-marker')
        child.finish()

    def test_history_navigation_verbosity_split_and_resume_preserve_source(self):
        journal = self.seed_session()
        original = journal.read_bytes()
        child = self.start('-N', 'reading')
        child.until(journal.parent.name[:8].encode())
        child.write(b'o')
        child.until(b'retained-answer-marker')
        child.write(b'gg')
        child.until(b'retained-question-marker')
        child.command('verbosity 4')
        child.until(b'input_received')
        child.command('vsp')
        child.resize(10, 27)
        child.command('workspace save')
        values = self.wait_snapshot(lambda values: len(next(iter(values.values()))['state']['windows']) == 2)
        windows = next(iter(values.values()))['state']['windows']
        self.assertTrue(all(x['kind'] == 'transcript' for x in windows))
        self.assertTrue(all(x['history']['session'] == journal.parent.name for x in windows))
        child.finish()
        resumed = self.start('--resume', 'reading', expect=b'history')
        resumed.until(b'history v4')
        resumed.command('sessions')
        resumed.command('history ' + journal.parent.name[:8])
        resumed.until(b'retained-answer-marker')
        resumed.finish()
        self.assertEqual(journal.read_bytes(), original)

    def test_large_history_oldest_newest_and_canceled_load(self):
        response = 'first-output-marker\n' + ('retained line\n' * 220000) + 'last-output-marker'
        journal = self.seed_session(response)
        original = journal.stat().st_size
        self.assertGreater(original, 4 * 1024 * 1024)
        child = self.start('-N', 'large-history')
        child.command('history ' + journal.parent.name)
        child.until(b'last-output-marker', timeout=15)
        child.write(b'gg')
        child.until(b'first-output-marker', timeout=15)
        child.resize(8, 1)
        child.write(b'GggG')
        child.resize(12, 100)
        child.write(b'\x03')
        child.command('sessions')
        child.until(b'sessions')
        child.finish()
        self.assertEqual(journal.stat().st_size, original)

    def test_help_list_and_nonterminal_do_not_start_agents(self):
        result = self.cli('--help')
        self.assertEqual(result.returncode, 0)
        self.assertIn(b'--resume', result.stdout)
        self.assertFalse((self.root / 'state').exists())
        result = self.cli('-N', 'unusable')
        self.assertEqual(result.returncode, 2)
        self.assertNotIn(b'\x1b', result.stdout + result.stderr)
        self.assertFalse((self.root / 'state').exists())
        result = self.cli('-l', '0')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b'\n'), 1)
        self.assertFalse((self.root / 'state' / 'workspaces').exists())
        self.assertEqual(list((self.root / 'state' / 'sessions').iterdir()), [])

    def test_untouched_picker_does_not_create_workspace(self):
        child = self.start()
        child.finish('q')
        self.assertEqual(self.snapshots(), {})
        self.assertEqual(list((self.root / 'state' / 'sessions').iterdir()), [])

    def test_split_layout_named_resume_and_tiny_resize(self):
        child = self.start('-N', 'operations')
        child.command('vsp')
        child.command('sp')
        child.command('help')
        child.command('workspace save')
        saved = self.wait_snapshot(lambda values: len(values) == 1 and
                                  len(next(iter(values.values()))['state']['windows']) == 3)
        sid, snapshot = next(iter(saved.items()))
        child.resize(1, 1)
        child.write(b'\x17w')
        time.sleep(.1)
        child.resize(12, 100)
        child.write(b'\x17=')
        child.finish()
        final = self.snapshots()[sid]
        resumed = self.start('--resume', 'operations')
        resumed.command('workspace save')
        restored = self.wait_snapshot(lambda values: values[sid]['activity_ms'] > final['activity_ms'])
        self.assertEqual(restored[sid]['state'], final['state'])
        resumed.finish()

    def test_live_workspace_refuses_second_owner(self):
        child = self.start('-N', 'busy')
        other = Terminal(self.root, ('--resume', 'busy'))
        self.addCleanup(other.close)
        other.wait_exit()
        while other.read(0):
            pass
        self.assertEqual(other.process.returncode, 2)
        self.assertNotIn(b'\x1b[?1049h', other.output)
        listing = self.cli('-l', '0')
        self.assertEqual(listing.returncode, 0)
        self.assertIn(b'\tbusy\topen\t', listing.stdout)
        child.command('workspace name "renamed workspace"')
        self.wait_snapshot(lambda values: next(iter(values.values()))['name'] == 'renamed workspace')
        child.finish()
        resumed = self.start('--resume', 'renamed workspace')
        resumed.finish()

    def test_paste_and_lone_escape_do_not_execute_commands(self):
        child = self.start('-N', 'paste')
        child.write(b'\x1b[200~:qa\r\x03\x1b[201~')
        time.sleep(.1)
        self.assertIsNone(child.process.poll())
        child.write(b'/\x1b[200~')
        for byte in 'e\u0301界'.encode():
            child.write(bytes([byte]))
        time.sleep(.06)
        child.write(b'\x1b[201~\r')
        child.command('workspace save')
        self.wait_snapshot(lambda values: next(iter(values.values()))['state']['windows'][0]['filter']
                           == 'e\u0301界')
        child.write(b':qa\x1b')
        time.sleep(.1)
        self.assertIsNone(child.process.poll())
        child.command('workspace name survived')
        self.wait_snapshot(lambda values: next(iter(values.values()))['name'] == 'survived')
        child.finish()

    def test_signal_restores_terminal_and_saved_layout(self):
        child = self.start('-N', 'signal')
        child.command('vsp')
        child.command('workspace save')
        self.wait_snapshot(lambda values: len(next(iter(values.values()))['state']['windows']) == 2)
        child.signal(signal.SIGTERM)
        child.finish()
        resumed = self.start('--last')
        resumed.finish()

    def test_suspend_restores_terminal_then_redraws(self):
        child = self.start('-N', 'suspend')
        child.write(b'\x1a')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if child.state().get('state') == 'stopped':
                break
            child.read()
        self.assertEqual(child.state().get('state'), 'stopped')
        self.assertEqual(normalized_modes(termios.tcgetattr(child.slave)), child.original)
        self.assertIn(b'\x1b[?1002l', child.output)
        mouse_before = child.output.count(b'\x1b[?1002h')
        before = child.output.count(b'\x1b[?1049h')
        child.signal(signal.SIGCONT)
        deadline = time.monotonic() + 5
        while (child.output.count(b'\x1b[?1049h') == before or
               child.output.count(b'\x1b[?1002h') == mouse_before) and time.monotonic() < deadline:
            child.read()
        self.assertGreater(child.output.count(b'\x1b[?1049h'), before)
        self.assertGreater(child.output.count(b'\x1b[?1002h'), mouse_before)
        child.finish()

    def test_version_one_picker_snapshot_remains_resumable(self):
        child = self.start('-N', 'legacy-picker')
        child.finish()
        path = next((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
        value = json.loads(path.read_text())
        value['state']['v'] = 1
        value['state'].pop('buffers')
        value['state'].pop('classic')
        path.write_text(json.dumps(value))
        child = self.start('--resume', 'legacy-picker')
        child.command('workspace save')
        self.wait_snapshot(lambda values: next(iter(values.values()))['state']['v'] == 10)
        child.finish()

    def test_unknown_state_is_preserved_and_terminal_not_entered(self):
        child = self.start('-N', 'future')
        child.finish()
        path = next((self.root / 'state' / 'workspaces').glob('*/workspace.json'))
        value = json.loads(path.read_text())
        value['state']['v'] = 100
        path.write_text(json.dumps(value))
        before = path.read_bytes()
        other = Terminal(self.root, ('--resume', 'future'))
        self.addCleanup(other.close)
        other.wait_exit()
        while other.read(0):
            pass
        self.assertEqual(other.process.returncode, 2)
        self.assertNotIn(b'\x1b[?1049h', other.output)
        self.assertEqual(path.read_bytes(), before)

    def test_workspace_picker_switch_and_failed_switch_preserve_owner(self):
        first = self.start('-N', 'first')
        first.finish()
        second = self.start('-N', 'second')
        second.finish()
        child = self.start('--resume', 'first')
        child.command('workspaces')
        child.write(b'G\r')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            child.read()
            listing = self.cli('-l').stdout
            if b'\tsecond\topen\t' in listing:
                break
        self.assertIn(b'\tsecond\topen\t', listing)
        self.assertIn(b'\tfirst\tstored\t', listing)
        path = next(path for path in (self.root / 'state' / 'workspaces').glob('*/workspace.json')
                    if json.loads(path.read_text())['name'] == 'first')
        value = json.loads(path.read_text())
        value['state']['windows'][0]['id'] = 999
        path.write_text(json.dumps(value))
        original = path.read_bytes()
        child.command('workspaces')
        child.write(b'G\r')
        child.until(b'invalid workspace views')
        self.assertIn(b'\tsecond\topen\t', self.cli('-l').stdout)
        self.assertEqual(path.read_bytes(), original)
        child.command('workspace name still-owned')
        self.wait_snapshot(lambda values: any(x['name'] == 'still-owned' for x in values.values()))
        child.finish()

    def test_stalled_output_still_accepts_signal_and_restores_modes(self):
        child = self.start('-N', 'backpressure')
        child.command('help')
        child.until(b'Ctrl-D/U')
        # Repaint into a PTY whose master deliberately stops consuming output.
        for _ in range(60):
            child.write(b'\x0c')
            time.sleep(.01)
        child.signal(signal.SIGTERM)
        deadline = time.monotonic() + 5
        while child.state().get('state') != 'exited' and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(child.state().get('state'), 'exited')
        self.assertEqual(normalized_modes(child.state()['modes']), child.original)
        child.wait_exit()
        self.assertEqual(child.process.returncode, 0)

    def test_idle_workspace_emits_no_frames(self):
        child = self.start()
        while child.read(.1):
            pass
        self.assertEqual(child.read(.25), b'')
        child.finish()


if __name__ == '__main__':
    unittest.main()
