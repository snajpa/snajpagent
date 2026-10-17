#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Real catalog/metrics and PTYs cover top navigation and owner confirmation."""
import fcntl
import hashlib
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

import store_history
from test_remote_terminal import RemoteProcess
from tmux_terminal import FakeResponses, write_irc_config


PRODUCT = Path(sys.argv.pop(1)).resolve()


class TopTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snag-top-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.state = self.root / 'state'
        self.workers = []
        self.addCleanup(self.close_workers)

    def close_workers(self):
        for worker in self.workers:
            if worker.poll() is None:
                worker.terminate()
            worker.wait(timeout=5)
            worker.stdout.close()

    def session(self, total=0, live=False):
        journal = store_history.create_legacy(self.state, self.root, 'fake', 'top-fixture')
        sid = journal.parent.name
        worker = None
        if live:
            worker = subprocess.Popen([sys.executable, '-u', '-c',
                'import fcntl,signal,sys; f=open(sys.argv[1],"r+"); '
                'fcntl.lockf(f,fcntl.LOCK_EX); print("ready"); signal.pause()',
                str(journal.parent / 'lock')], stdout=subprocess.PIPE, text=True)
            self.workers.append(worker)
            self.assertEqual(worker.stdout.readline().strip(), 'ready')
        self.metrics(sid, total, worker.pid if worker else 0)
        return sid, worker

    def metrics(self, sid, total, pid=0, started=1000):
        directory = self.state / 'metrics'
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / (sid + '.prom')
        temporary = path.with_suffix('.tmp')
        temporary.write_text(
            'snajpagent_session_info{provider="fake",model="top-fixture",'
            'active_turn_id="",state="running"} 1\n'
            'snajpagent_session_turns_total 3\n'
            f'snajpagent_session_process_total_tokens_total {total}\n'
            f'snajpagent_session_total_tokens_total {total}\n'
            f'snajpagent_session_process_pid {pid}\n'
            f'snajpagent_session_process_started_ms {started}\n')
        temporary.chmod(0o600)
        temporary.replace(path)

    def top(self, height=24, width=120):
        top = RemoteProcess(self.root, [str(PRODUCT), 'top', '--dotdir', str(self.state),
                            '--delay', '1', '--color', 'always'], wrapped=None)
        self.addCleanup(top.close)
        fcntl.ioctl(top.master, termios.TIOCSWINSZ, struct.pack('HHHH', height, width, 0, 0))
        self.frame(top)
        return top

    def frame(self, top, keys=b''):
        if keys:
            top.output.clear()
            os.write(top.master, keys)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            frame = bytes(top.output).split(b'\033[2J\033[H')[-1]
            if re.search(rb'TOTAL sessions=[^\r\n]*[\r\n]', frame):
                return frame
            self.assertIsNone(top.process.poll(), bytes(top.output))
            if select.select([top.master], [], [], .05)[0]:
                top.output.extend(os.read(top.master, 65536))
        self.fail('top did not finish its frame: ' + repr(bytes(top.output)))

    def selected(self, frame):
        match = re.search(rb'\033\[7m([0-9a-f]{8}) ', frame)
        self.assertIsNotNone(match, frame)
        return match[1].decode()

    def quit(self, top):
        os.write(top.master, b'q')
        top.wait(0)
        self.assertIn(b'\033[?25h', top.output)

    def test_empty_redirected_output(self):
        result = subprocess.run([str(PRODUCT), 'top', '--dotdir', str(self.state),
                                 '--color', 'never'], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        self.assertIn(b'TOTAL sessions=0 turns=0 process=0 session=0', result.stdout)

    def test_redirected_output_and_stored_journal_are_read_only(self):
        sid, _ = self.session(330)
        journal = self.state / 'sessions' / sid / 'events.jsonl'
        before = hashlib.sha256(journal.read_bytes()).digest()
        result = subprocess.run([str(PRODUCT), 'top', '--dotdir', str(self.state),
                                 '--color', 'never'], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        self.assertNotIn(b'\0', result.stdout)
        self.assertIn(b'SESSION  STATE', result.stdout)
        self.assertIn(b'TOTAL sessions=1 turns=3 process=330 session=330', result.stdout)
        self.assertEqual(hashlib.sha256(journal.read_bytes()).digest(), before)
        top = self.top()
        self.assertIn(b'stored sessions cannot be killed', self.frame(top, b'x'))
        self.quit(top)

    def test_selection_stays_with_session_after_sort_changes(self):
        first, _ = self.session(900)
        second, _ = self.session(100)
        top = self.top()
        self.assertEqual(self.selected(bytes(top.output)), first[:8])
        self.metrics(second, 1000)
        frame = self.frame(top, b'r')
        self.assertEqual(self.selected(frame), first[:8])
        self.assertLess(frame.index(second[:8].encode()), frame.index(first[:8].encode()))
        self.quit(top)

    def test_selection_scrolls_and_wraps_without_leaving_screen(self):
        sessions = [self.session(100 + n)[0] for n in range(9)]
        top = self.top(height=8)
        for expected in reversed(sessions[:-1]):
            frame = self.frame(top, b'j')
            self.assertEqual(self.selected(frame), expected[:8])
            self.assertLessEqual(frame.count(b'\n'), 8, frame)
        frame = self.frame(top, b'j')
        self.assertEqual(self.selected(frame), sessions[-1][:8])
        self.quit(top)

    def test_confirmation_keeps_original_session_after_sort_changes(self):
        first, first_worker = self.session(900, live=True)
        second, second_worker = self.session(100, live=True)
        top = self.top()
        self.assertIn(b'press y/n', self.frame(top, b'x'))
        self.metrics(second, 1000, second_worker.pid)
        self.frame(top, b'r')
        frame = self.frame(top, b'y')
        self.assertIn(b'termination requested', frame)
        self.assertIsNone(second_worker.poll(), 'confirmation killed a different session')
        first_worker.wait(timeout=5)
        self.quit(top)

    def test_terminal_lines_fit_and_return_to_first_column(self):
        for n in range(9):
            self.session(100 + n)
        top = self.top(height=8, width=80)
        frame = self.frame(top, b'r')
        plain = re.sub(rb'\033\[[0-9;?]*[a-zA-Z]', b'', frame)
        self.assertEqual(len(plain.split(b'\r\n')), 8, plain)
        self.assertNotIn(b'\n', plain.replace(b'\r\n', b''))
        self.assertTrue(plain.endswith(b'\r'), plain)
        self.assertTrue(all(len(line) <= 80 for line in plain.split(b'\r\n')), plain)
        self.quit(top)

    def test_confirmation_refuses_replaced_owner_and_wrong_lock_pid(self):
        sid, worker = self.session(900, live=True)
        _, other = self.session(100, live=True)
        top = self.top()
        self.frame(top, b'x')
        self.metrics(sid, 900, worker.pid, started=2000)
        self.assertIn(b'owner changed', self.frame(top, b'y'))
        self.assertIsNone(worker.poll())
        self.metrics(sid, 900, other.pid, started=2000)
        self.frame(top, b'r')
        self.frame(top, b'x')
        self.assertIn(b'lock owner', self.frame(top, b'y'))
        self.assertIsNone(worker.poll())
        self.assertIsNone(other.poll())
        self.quit(top)

    def test_real_session_metrics_and_confirmed_owner_shutdown(self):
        provider = FakeResponses()
        self.addCleanup(provider.close)
        config = self.root / 'config.ini'
        write_irc_config(config, provider.port, 'host-model')
        owner = RemoteProcess(self.root, [str(PRODUCT), '--config', str(config),
                              '--dotdir', str(self.state), '--no-listen', '--no-client'],
                              wrapped=None, extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'})
        self.addCleanup(owner.close)
        owner.until('›'.encode(), 15)
        metrics = list((self.state / 'metrics').glob('*.prom'))
        self.assertEqual(len(metrics), 1)
        self.assertIn('snajpagent_session_process_pid ', metrics[0].read_text())
        top = self.top()
        self.assertIn(b'attached', top.output)
        self.frame(top, b'x')
        self.assertIn(b'termination requested', self.frame(top, b'y'))
        owner.wait(128 + signal.SIGTERM)
        self.quit(top)
        self.assertFalse(provider.requests, 'monitoring started a provider request')


if __name__ == '__main__':
    unittest.main()
