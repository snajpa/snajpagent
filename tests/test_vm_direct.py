#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Direct engine: real VM client, headless UI, HTTP, journal and joined shutdown."""

import json
import os
import select
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import tmux_terminal as harness
from store_history import journal_paths, read_events


DRIVER = Path(sys.argv.pop(1)).resolve()
BINARY = Path(sys.argv.pop(1)).resolve()


class DirectTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='snag-direct-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / 'state'
        self.state.mkdir(mode=0o700)
        self.provider = harness.FakeResponses()
        self.addCleanup(self.provider.close)
        self.release = threading.Event()
        self.requested = threading.Event()
        self.addCleanup(self.release.set)
        self.requests = []

        def reply(handler, request, sequence):
            self.requests.append(request)
            self.requested.set()
            if not self.release.wait(10):
                raise AssertionError('direct fixture was not released')
            self.provider.reply(handler, self.provider.response_body(
                sequence, 'direct-reply-marker').encode(), close_header=True)

        self.provider.runtime_handler = reply
        config = self.state / 'config.ini'
        harness.write_irc_config(config, self.provider.port, 'host-model')
        config.chmod(0o600)

    def run_driver(self, mode, session=None):
        args = [str(DRIVER), str(BINARY), str(self.state), mode]
        if session:
            args.append(session)
        env = dict(os.environ, HOME=str(self.root), SNAJPAGENT_IRC_UI_KEY='irc-ui-secret')
        with subprocess.Popen(args, cwd=self.root, env=env, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                if mode in ('turn', 'stop'):
                    ready, _, _ = select.select([process.stdout], [], [], 12)
                    self.assertTrue(ready, 'direct engine never became active')
                    line = process.stdout.readline()
                    self.assertEqual(line.replace(b'\r\n', b'\n'), b'{"phase":"active"}\n')
                    self.assertTrue(self.requested.wait(5), 'provider request never arrived')
                    if mode == 'turn':
                        self.release.set()
                    else:
                        process.stdin.write(b'\n')
                        process.stdin.flush()
                output, error = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, error.decode(errors='replace'))
                self.assertEqual(error, b'', 'headless presentation wrote to the terminal')
                result = json.loads(output)
                self.assertEqual(result['phase'], 'finished')
                return result
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def events(self, session):
        path = next(p for p in journal_paths(self.state) if p.parent.name == session)
        return read_events(path)

    def test_turn_command_draft_and_resume(self):
        result = self.run_driver('turn')
        self.assertTrue(result['report'])
        self.assertTrue(result['exit_received'])
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.provider.latest_user(self.requests[0]), 'direct-driver-prompt ž')
        self.assertEqual(self.requests[0].get('service_tier'), 'priority')
        events = self.events(result['session'])
        self.assertEqual(sum(e['type'] == 'input_received' for e in events), 1)
        self.assertTrue(any(e['type'] == 'response_completed' for e in events))
        self.assertNotIn('next unsent ž', json.dumps(self.requests, ensure_ascii=False))
        resumed = self.run_driver('close', result['session'])
        self.assertEqual(resumed['session'], result['session'])
        self.assertEqual(len(self.requests), 1)

    def test_workspace_loss_stops_engine(self):
        result = self.run_driver('close')
        self.assertFalse(result['exit_received'])
        self.assertEqual(self.requests, [])

    def test_stop_interrupts_provider_and_joins(self):
        result = self.run_driver('stop')
        self.assertEqual(len(self.requests), 1)
        self.assertTrue(result['exit_received'])
        self.assertFalse(any(e['type'] == 'response_completed'
                             for e in self.events(result['session'])))

    def test_stop_during_startup(self):
        self.run_driver('startup-stop')
        self.assertEqual(self.requests, [])

    def test_startup_error_is_reported_without_console_output(self):
        (self.state / 'config.ini').write_text('[agent]\nunknown_option = yes\n')
        result = self.run_driver('startup-error')
        self.assertNotEqual(result['status'], 0)
        self.assertEqual(self.requests, [])


if __name__ == '__main__':
    unittest.main()
