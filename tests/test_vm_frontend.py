# SPDX-License-Identifier: GPL-2.0-only
"""Workspace CLI, real terminal ownership, persistence and input boundaries."""

import fcntl
import hashlib
import json
import os
import pty
import re
import select
import shlex
import shutil
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
from store_history import create_legacy, journal_paths, read_events


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
        self.root = root
        self.alternate_screen = not any(Path(arg).name == 'mosh' for arg in transport)
        self.alternate_screen = self.alternate_screen or '--init' in transport
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
        self.rows = rows
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

    def attached(self, timeout=5):
        """Wait for a controlled session surface, including older frontends."""
        from test_session_view import View

        labels = set()
        seen = set()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for directory in (self.root / 'state' / 'sessions').iterdir():
                if directory in seen or not (directory / 'view.sock').exists():
                    continue
                try:
                    observer = View(directory)
                except (ConnectionRefusedError, FileNotFoundError):
                    continue
                try:
                    state = observer.until('state')['state']
                    labels.add(state.get('name') or directory.name[:8])
                    seen.add(directory)
                finally:
                    observer.close()
            self.write(b'\x0c')
            self.read(.05)
            # Screen and Mosh can rewrite the paint into relative cursor moves.
            # Wait for the owner-derived prompt label; cell assertions belong to
            # the interaction being tested after attachment.
            output = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '',
                            self.output.decode('utf-8', 'replace'))
            if 'ATTACHED' in output:
                return
            for label in labels:
                if re.search(r'\[' + re.escape(label) +
                             r'(?:; (?:HOLD|loading|history error, R))*\] ', output):
                    return
        raise AssertionError(('controlled session prompt', bytes(self.output[-5000:])))

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
        if self.alternate_screen:
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
        if child.alternate_screen:
            child.until(b'\x1b[?1049h')
        child.until(expect)
        return child

    def snapshots(self):
        return {path.parent.name: json.loads(path.read_text()) for path in
                (self.root / 'state' / 'workspaces').glob('*/workspace.json')}

    def wait_snapshot(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
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

    def resume_hint(self, child, sid):
        # The footer belongs to the shell screen, after leaving the workspace.
        self.assertRegex(bytes(child.output),
            rb'\x1b\[' + str(child.rows).encode() + rb';1H\r+\n\x1b\[\?1049l')
        output = child.output.rsplit(b'\x1b[?1049l', 1)[-1].decode(errors='replace')
        marker = 'You can resume this workspace with the following command'
        self.assertEqual(output.count(marker), 1, output)
        command = output.split(marker)[1].splitlines()[1]
        self.assertEqual(shlex.split(command), [str(BINARY), 'vm', '--dotdir',
                        str(self.root / 'state'), '--resume', sid])
        return shlex.split(command)

    def seed_session(self, response='retained-answer-marker', legacy=False):
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
        journal = create_legacy(self.root / 'state', self.root, 'fake', 'host-model') if legacy else None
        for _ in chunks:
            resume = ['--resume', journal.parent.name] if journal else []
            result = subprocess.run([str(BINARY), '--config', str(config), '--dotdir',
                                     str(self.root / 'state'), *resume, '-e', '--',
                                     'retained-question-marker'],
                                    cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                       'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                    capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            journal, = journal_paths(self.root / 'state')
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

    def test_ex_completion_cycles_and_preserves_editing(self):
        child = self.start('-N', 'completion')
        child.write(b':sess\t')
        child.repaint_until(b'\x1b[0msession')
        child.write(b'\t')
        child.repaint_until(b'\x1b[0msessions')
        child.write(b'\x1b[Z')
        child.repaint_until(b'\x1b[0msession\x1b')
        child.write(b' d\t')
        child.repaint_until(b'\x1b[0msession detach')
        self.assertIsNone(child.process.poll(), 'completion executed the command')
        # Editing resets the completion prefix, including after cancellation.
        child.write(b'\x15worksp\t name completed\r')
        self.wait_snapshot(lambda rows: any(r['name'] == 'completed' for r in rows.values()))
        child.write(b':set no\t')
        child.repaint_until(b'\x1b[0mset noignorecase')
        child.write(b'\t')
        child.repaint_until(b'\x1b[0mset nomouse')
        child.write(b'\x1b')
        time.sleep(.06)
        child.write(b':workspace d\t')
        child.repaint_until(b'\x1b[0mworkspace detach')
        self.assertIsNone(child.process.poll(), 'completion executed workspace detach')
        child.write(b'\x1b')
        time.sleep(.06)
        child.finish('workspace d')

    def test_follow_hold_and_idle_polling(self):
        journal = self.seed_session()
        first_input = next(e['seq'] for e in read_events(journal) if e['type'] == 'input_received')
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
            next(iter(values.values()))['state']['windows'][0]['history']['seq'] == first_input)
        anchor = next(iter(values.values()))['state']['windows'][0]['history']
        self.append_turn(journal, 'live-append-two')
        # HOLD keeps its source position while the stored tail advances.
        child.read(1.2)
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

    def test_transcript_restore_does_not_scan_unrelated_sessions(self):
        journal = self.seed_session()
        child = self.start('-N', 'only-transcript')
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'retained-answer-marker')
        child.finish('workspace d')
        other = WorkspaceTests()
        other.setUp()
        self.addCleanup(other.doCleanups)
        original = other.seed_session('unrelated history')
        directory = self.root / 'state' / 'sessions' / original.parent.name
        shutil.copytree(original.parent, directory)
        unrelated = directory / original.name
        stamp = 1_000_000_000_000_000_000
        mtime = unrelated.stat().st_mtime_ns
        os.utime(unrelated, ns=(stamp, mtime))
        # Verify that this filesystem records reads before using access time as
        # a probe; a noatime mount cannot demonstrate this invariant.
        with unrelated.open('rb') as source:
            source.read(1)
        if unrelated.stat().st_atime_ns == stamp:
            self.skipTest('filesystem does not record journal access')
        os.utime(unrelated, ns=(stamp, mtime))
        child = self.start('--resume', 'only-transcript', expect=b'Workspace restored')
        child.repaint_until(b'retained-answer-marker')
        child.finish('workspace d')
        self.assertEqual(unrelated.stat().st_atime_ns, stamp,
                         'transcript restore read an unrelated session journal')

    def test_follow_fills_height_after_large_resize(self):
        journal = self.seed_session('\n'.join(f'viewport-line-{i:03}' for i in range(100)))
        child = self.start('-N', 'large-display', rows=12, columns=255)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'viewport-line-099')
        child.resize(62, 255)
        # One row belongs to the persistent session prompt.
        child.repaint_until(b'viewport-line-042')
        child.until(b'viewport-line-099')
        self.assertIn(b'viewport-line-099', child.output)
        child.resize(90, 320)
        child.repaint_until(b'viewport-line-014')
        child.resize(12, 100)
        child.repaint_until(b'viewport-line-099')
        child.finish()

    @unittest.skipUnless(shutil.which('tmux'), 'tmux is unavailable')
    def test_scrolling_past_hidden_boundaries_keeps_visible_history(self):
        from test_session_listing import canonical, append_event

        journal = self.seed_session('first-visible-marker\n' + 'retained-line\n' * 35 +
                                    'last-visible-marker', legacy=True)
        events = [json.loads(line) for line in journal.read_bytes().splitlines()]
        # Keep source references intact while placing non-rendered metadata
        # before the first visible event, beyond the reader's byte-page size.
        events[1]['data']['padding'] = 'x' * (5 * 1024 * 1024)
        previous = events[0]['prev_sha256']
        with journal.open('wb') as stream:
            for event in events:
                del event['event_sha256']
                event['prev_sha256'] = previous
                event['event_sha256'] = hashlib.sha256(canonical(event).encode()).hexdigest()
                previous = event['event_sha256']
                stream.write(canonical(event).encode() + b'\n')
        tmux = [shutil.which('tmux'), '-S', str(self.root / 'scroll-tmux.sock')]
        self.addCleanup(subprocess.run, [*tmux, 'kill-server'],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        child = self.start('-N', 'scroll-boundaries', rows=14, columns=120,
                           transport=[*tmux, '-f', '/dev/null', 'new-session',
                                      '-s', 'scroll', '--'])

        def frame_until(marker):
            deadline = time.monotonic() + 10
            frame = ''
            while time.monotonic() < deadline:
                child.read(.03)
                frame = subprocess.check_output([*tmux, 'capture-pane', '-p',
                                                 '-t', 'scroll:0.0'], text=True, timeout=5)
                if marker in frame and ' loading' not in frame:
                    return frame
            self.fail((marker, frame))

        def wheel(button):
            for _ in range(28):
                child.write(f'\x1b[<{button};3;3M'.encode())
                child.read(.03)

        child.command('history ' + journal.parent.name)
        frame_until('last-visible-marker')
        wheel(64)
        frame = frame_until('[start]')
        self.assertIn('first-visible-marker', frame)
        # A new hidden tail must likewise leave the last visible output intact.
        append_event(journal, 'provider_request', {'padding': 'y' * (5 * 1024 * 1024)})
        frame_until('newer')
        wheel(65)
        frame = frame_until('[tail]')
        self.assertIn('last-visible-marker', frame)
        child.finish('workspace detach')

    def test_history_renders_standalone_markdown(self):
        journal = self.seed_session('## Shared heading\n\nA **bold** word and `code`.\n\n'
                                    '- First item\n- Second item\n')
        child = self.start('-N', 'presentation', rows=30, columns=80,
                           extra_env={'NO_COLOR': None})
        child.command('history ' + journal.parent.name)
        child.repaint_until('• A '.encode())
        self.assertIn(b'stored; :session', child.output)
        # The provider's Markdown is interpreted by the same formatter as the
        # standalone terminal. Internal response-phase headings stay hidden.
        child.write(b'\x0c')
        child.until(b'Second item')
        screen = child.output.rsplit(b'\x1b[2J', 1)[-1]
        self.assertNotIn(b'**bold**', screen)
        self.assertNotIn(b'`code`', screen)
        self.assertNotIn(b'assistant final_answer', screen)
        self.assertIn(b'\x1b[0;1m', screen)
        child.finish('workspace d')

    def test_resize_keeps_background_history_pending(self):
        foreground = self.seed_session('foreground-history-marker')
        other = WorkspaceTests()
        other.setUp()
        self.addCleanup(other.doCleanups)
        background = other.seed_session('background-history-marker', legacy=True)
        last = json.loads(background.read_bytes().splitlines()[-1])
        with background.open('ab') as stream:
            for _ in range(128):
                event = dict(data={'padding': 'x' * (256 * 1024)},
                             prev_sha256=last['event_sha256'], seq=last['seq'] + 1,
                             session_id=last['session_id'], time_ms=last['time_ms'] + 1,
                             type='provider_request', v=2,
                             checkpoint_offset=last['checkpoint_offset'])
                def encode():
                    return json.dumps(event, sort_keys=True, separators=(',', ':')).encode()
                event['event_sha256'] = hashlib.sha256(encode()).hexdigest()
                stream.write(encode() + b'\n')
                last = event
        shutil.copytree(background.parent, foreground.parent.parent / background.parent.name)
        child = self.start('-N', 'background-resize', columns=255)
        child.command('history ' + foreground.parent.name)
        child.repaint_until(b'foreground-history-marker')
        child.command('vsp')
        child.command('history ' + background.parent.name)
        child.repaint_until(b'[' + background.parent.name[:8].encode() + b'; stored; :session')
        time.sleep(.05)
        # Reflow the focused foreground while the other pane reads a large
        # journal. Its interrupted request must remain scheduled.
        child.write(b'\x17h')
        child.resize(40, 320)
        child.repaint_until(b'foreground-history-marker')
        child.repaint_until(b'background-history-marker', timeout=15)
        child.finish('workspace d')

    def test_workspace_colors_follow_config_and_environment(self):
        journal = self.seed_session()
        config = self.root / 'state' / 'config.ini'
        config.write_text('[ui]\ncolor=auto\n')
        config.chmod(0o600)
        for mode, no_color, expected in [('auto', None, True), ('auto', '1', False),
                                         ('never', None, False), ('always', '1', True)]:
            with self.subTest(mode=mode, no_color=no_color):
                config.write_text(f'[ui]\ncolor={mode}\n')
                child = self.start(extra_env={'NO_COLOR': no_color})
                child.command('history ' + journal.parent.name)
                child.repaint_until(b'retained-answer-marker')
                foregrounds = re.findall(rb'\x1b\[[0-9;]*(?:3[0-7])(?:;[0-9]+)*m',
                                         bytes(child.output))
                self.assertEqual(bool(foregrounds), expected)
                if expected:
                    self.assertIn(b'\x1b[0;1;36m' + '› '.encode(), child.output)
                    self.assertNotIn(b'assistant final_answer', child.output)
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
        child.repaint_until(b'input_received')
        child.command('vsp')
        child.resize(10, 27)
        child.command('workspace save')
        values = self.wait_snapshot(lambda values: len(next(iter(values.values()))['state']['windows']) == 2)
        windows = next(iter(values.values()))['state']['windows']
        self.assertTrue(all(x['kind'] == 'transcript' for x in windows))
        self.assertTrue(all(x['history']['session'] == journal.parent.name for x in windows))
        child.finish()
        resumed = self.start('--resume', 'reading', expect=b'Workspace restored')
        resumed.repaint_until(b'input_received')
        self.assertTrue(all(window['history']['verbosity'] == 4
                            for window in next(iter(self.snapshots().values()))[
                                'state']['windows']))
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
        self.assertNotIn(b'You can resume', child.output)

    def test_detach_prints_quoted_command_for_saved_workspace(self):
        self.root = self.root / "operator's workspace"
        self.root.mkdir()
        child = self.start('-N', 'original')
        child.command('workspace name renamed')
        saved = self.wait_snapshot(lambda rows: any(row['name'] == 'renamed'
                                                  for row in rows.values()))
        sid, = saved
        child.finish('close')
        command = self.resume_hint(child, sid)
        resumed = self.start(*command[4:])
        self.assertIn(b'\trenamed\topen\t', self.cli('-l').stdout)
        resumed.finish('close')
        self.resume_hint(resumed, sid)

    def test_window_navigation_stays_in_the_requested_split_direction(self):
        child = self.start('-N', 'tall-splits', rows=70, columns=24)
        child.command('sp')
        child.command('vsp')
        child.command('sp')
        child.command('workspace save')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['focus'] == 4)
        # Left occupies the bottom half; right has two small stacked panes.
        child.write(b'\x17h')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['focus'] == 2)
        # Up must cross the horizontal divider, not jump diagonally right.
        child.write(b'\x17k')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['focus'] == 1)
        child.finish()

    def test_window_navigation_accepts_vim_control_and_arrow_keys(self):
        child = self.start('-N', 'window-keys', rows=30, columns=100)
        child.command('vsp')
        child.command('sp')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['focus'] == 3)
        for keys, focus in ((b'\x17k', 2), (b'\x17\x08', 1),
                            (b'\x17\x0c', 2), (b'\x17\x1b[B', 3),
                            (b'\x17\x0b', 2), (b'\x17\x0a', 3),
                            (b'\x17\x17', 1)):
            with self.subTest(keys=keys):
                child.write(keys)
                self.wait_snapshot(lambda rows:
                    next(iter(rows.values()))['state']['focus'] == focus)
        child.finish()

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
        self.wait_snapshot(lambda values: next(iter(values.values()))['state']['v'] == 14)
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
        self.assertNotIn(b'You can resume', other.output)

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
        sid, = (sid for sid, row in self.snapshots().items() if row['name'] == 'still-owned')
        self.resume_hint(child, sid)

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
