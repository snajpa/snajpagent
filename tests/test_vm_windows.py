# SPDX-License-Identifier: GPL-2.0-only
"""Windows VM through a console-capable runner: pass BINARY --windows to run.

On a POSIX test host BINARY can be a wrapper that execs Wine and the Windows
executable. The assertions require the production direct-engine backend.
"""

import json
import sys
import threading
import time
import unittest

WINDOWS = '--windows' in sys.argv
if WINDOWS:
    sys.argv.remove('--windows')

import test_vm_frontend as frontend
import tmux_terminal as harness


@unittest.skipUnless(WINDOWS, 'requires a Windows executable and console runner')
class WindowsTests(unittest.TestCase):
    snapshots = frontend.WorkspaceTests.snapshots
    wait_snapshot = frontend.WorkspaceTests.wait_snapshot

    def setUp(self):
        frontend.WorkspaceTests.setUp(self)
        self.provider = harness.FakeResponses()
        self.addCleanup(self.provider.close)
        self.release = threading.Event()
        self.requested = threading.Event()
        self.addCleanup(self.release.set)
        self.requests = []

        def reply(handler, request, sequence):
            self.requests.append(request)
            self.requested.set()
            if not self.release.wait(20):
                raise AssertionError('Windows provider fixture was not released')
            self.provider.reply(handler, self.provider.response_body(
                sequence, 'windows-direct-answer').encode(), close_header=True)

        self.provider.runtime_handler = reply
        self.config = self.root / 'state' / 'config.ini'
        self.config.parent.mkdir(mode=0o700)
        harness.write_irc_config(self.config, self.provider.port, 'host-model')
        self.config.write_text(self.config.read_text().replace(
            'idle_timeout_ms = 3000', 'idle_timeout_ms = 30000').replace(
            'request_timeout_ms = 5000', 'request_timeout_ms = 30000'))
        self.config.chmod(0o600)

    def start(self, *args, **kwargs):
        return frontend.WorkspaceTests.start(self, *args, columns=180,
            extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'}, **kwargs)

    def state(self):
        return next(iter(self.snapshots().values()))['state']

    def finish(self, child, command='qa'):
        if child.process.poll() is None:
            child.command(command)
        child.wait_exit()
        self.assertEqual(child.process.poll(), 0, bytes(child.output[-3000:]))
        self.assertEqual(frontend.normalized_modes(child.state()['modes']), child.original)
        # Wine's Unix conhost inserts CRLF inside VT sequences too. Check their
        # complete emission; this bridge does not qualify native desktop rendering.
        self.assertIn(b'\x1b[?1049l', child.output.replace(b'\r\n', b''))

    def live(self, child):
        child.repaint_until(b'ATTACHED')
        self.wait_snapshot(lambda rows: rows and self.state().get('buffers'))
        session = self.state()['buffers'][0]['session']
        child.command('detach')
        child.repaint_until(b'This session runs inside the workspace')
        return session

    def events(self, session):
        path = self.root / 'state' / 'sessions' / session / 'events.jsonl'
        return [json.loads(line) for line in path.read_bytes().splitlines()]

    def complete(self, child, session):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            child.read(.02)
            if any(e['type'] == 'response_completed' for e in self.events(session)):
                return
        self.fail('Windows provider response did not commit')

    def escape(self, child):
        child.write(b'\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL draft')

    def test_hidden_engine_split_draft_and_explicit_quit(self):
        child = self.start('-N', 'windows-live')
        child.command('new windows-agent')
        session = self.live(child)
        child.command('new extra-agent')
        child.repaint_until(b'One live session per workspace')
        self.assertEqual(len(list((self.root / 'state' / 'sessions').iterdir())), 1)

        child.write(b'i/fast\r')
        child.repaint_until(b'REPORT')
        child.command('history')
        child.write('iwindows-prompt ž\rnext unsent ž'.encode())
        self.assertTrue(self.requested.wait(5), 'Windows HTTP request never arrived')
        self.escape(child)
        child.command('close')
        child.repaint_until(b'Live session hidden')
        self.wait_snapshot(lambda rows: frontend.rollout(self.state()['buffers'][0])['draft']
                           == 'next unsent ž')
        child.command('q')
        child.repaint_until(b'Live session hidden')
        self.assertIsNone(child.process.poll())
        child.command('workspaces')
        child.write(b'\r')
        child.repaint_until(b'Quit the live session before switching workspaces')
        child.command('buffer ' + session)
        child.repaint_until(b'ATTACHED')
        child.command('vsp')
        self.wait_snapshot(lambda rows: len(self.state()['windows']) == 2)
        self.assertEqual(len(self.state()['buffers']), 1)
        child.repaint_until('next unsent ž'.encode())

        self.release.set()
        self.complete(child, session)
        child.repaint_until(b'windows-direct-answer')
        child.write(b'?windows-direct-answer\r')
        child.repaint_until(b'Match')
        child.repaint_until(b'windows-direct-answer')
        child.command('qa')
        child.repaint_until(b'Unsent draft or unresolved submission')
        self.assertIsNone(child.process.poll())
        self.finish(child, 'qa!')
        self.assertEqual(frontend.rollout(self.state()['buffers'][0])['draft'], '')
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.provider.latest_user(self.requests[0]), 'windows-prompt ž')
        self.assertEqual(self.requests[0].get('service_tier'), 'priority')
        self.assertEqual(sum(e['type'] == 'input_received' for e in self.events(session)), 1)

    def test_workspace_resume_requires_explicit_session_start(self):
        self.release.set()
        child = self.start('-N', 'windows-resume')
        child.command('new saved-agent')
        session = self.live(child)
        self.finish(child)
        resumed = self.start('--resume', 'windows-resume', expect=b'history')
        resumed.write(b'/session\r')
        resumed.repaint_until(b'Match')
        resumed.command('attach')
        resumed.repaint_until(b'Use :session to start a stored session')
        self.assertEqual(self.requests, [])
        resumed.command('session ' + session)
        self.assertEqual(self.live(resumed), session)
        resumed.write(b'iresumed-windows-prompt\r')
        self.complete(resumed, session)
        resumed.repaint_until(b'windows-direct-answer')
        self.escape(resumed)
        self.finish(resumed)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.provider.latest_user(self.requests[0]), 'resumed-windows-prompt')

    def test_failed_startup_releases_engine_slot(self):
        child = self.start('-N', 'windows-startup')
        valid = self.config.read_text()
        self.config.write_text('[agent]\nunknown_option = yes\n')
        child.command('new invalid-agent')
        child.repaint_until(b'invalid configuration at line 2')
        self.config.write_text(valid)
        child.command('new recovered-agent')
        self.live(child)
        self.finish(child)
        self.assertEqual(self.requests, [])


if __name__ == '__main__':
    unittest.main()
