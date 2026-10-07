# SPDX-License-Identifier: GPL-2.0-only
"""Interactive final replies preserve commands until input or completion."""

import json
import os
import shlex
import subprocess
import time
import unittest

from test_retry_auto import RetryFixture
from test_vm_frontend import BINARY


class CommandWaitTests(RetryFixture):
    def begin_wait(self, exits=False):
        self.calls = []
        self.before = self.events()[-1]['seq']
        script = self.root / 'callback.py'
        script.write_text('''import pathlib, sys, threading, time
root = pathlib.Path(__file__).parent
with (root / "starts").open("a") as stream:
    stream.write("x")
print("callback needed", flush=True)
def output():
    while not (root / "release-output").exists():
        time.sleep(.02)
    print("buffered-" + "x" * 32768 + "-retained", flush=True)
thread = threading.Thread(target=output, daemon=True)
thread.start()
'''+('''thread.join()
print("callback complete", flush=True)
''' if exits else '''answer = sys.stdin.readline().strip()
print("callback received: " + answer, flush=True)
'''))

        def respond(handler, request, sequence):
            self.calls.append(request)
            n = len(self.calls)
            if n == 1:
                body = self.provider.function_body(sequence, 'login', 'exec_command', {
                    'command': 'exec python3 ' + shlex.quote(str(script)),
                    'workdir': str(self.root), 'pty': True, 'yield_ms': 0})
            elif n == 2:
                body = self.provider.response_body(sequence, 'Please supply the callback.')
            elif n == 3:
                handle = next(e['data']['result']['handle'] for e in self.events()
                              if e['seq'] > self.before and e['type'] == 'tool_finished')
                body = self.provider.function_body(sequence, 'collect', 'write_stdin', {
                    'handle': handle, 'data': '' if exits else 'synthetic-callback\n',
                    'yield_ms': 1000})
            else:
                body = self.provider.response_body(sequence, 'Callback finished.')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.term.write(b'/rollout\rbegin callback\r')
        self.wait(lambda: len(self.calls) >= 2)
        self.wait(lambda: sum(e['type'] == 'response_completed'
                             for e in self.new_events()) >= 2)

    def new_events(self):
        return [e for e in self.events() if e['seq'] > self.before]

    def stays_waiting(self):
        # The old path retries after 250ms. Keep pumping the terminal so this
        # assertion observes the engine, not a blocked terminal output pipe.
        until = time.monotonic() + .8
        while time.monotonic() < until:
            self.term.read(.02)
        self.assertEqual(len(self.calls), 2, 'waiting issued another provider request')
        self.assertFalse(any(e['type'] in ('turn_recovery', 'turn_failed',
                         'turn_completed', 'process_closed') for e in self.new_events()))
        self.assertEqual((self.root / 'starts').read_text(), 'x')

    def test_callback_wait_keeps_output_and_same_command(self):
        self.begin_wait()
        self.stays_waiting()
        (self.root / 'release-output').touch()
        self.wait(lambda: sum(len(e['data'].get('data', '')) for e in self.new_events()
                              if e['type'] == 'process_output') > 32768)
        self.command('/status', 'session:')
        self.stays_waiting()
        self.term.write(b'synthetic-callback\r')
        self.wait(lambda: any(e['type'] == 'turn_completed' for e in self.new_events()))
        self.assertEqual(len(self.calls), 4)
        self.assertIn('synthetic-callback', json.dumps(self.calls[2]['input']))
        self.assertIn('callback received: synthetic-callback',
                      json.dumps(self.calls[3]['input']))
        self.assertEqual((self.root / 'starts').read_text(), 'x')
        self.assertEqual(sum(e['type'] == 'turn_started' for e in self.new_events()), 1)
        self.assertFalse(any(e['type'] in ('turn_recovery', 'turn_failed', 'process_closed')
                             for e in self.new_events()))

    def test_command_completion_wakes_and_is_collected(self):
        self.begin_wait(exits=True)
        self.stays_waiting()
        (self.root / 'release-output').touch()
        self.wait(lambda: any(e['type'] == 'turn_completed' for e in self.new_events()))
        self.assertIn('callback complete', json.dumps(self.calls[-1]['input']))
        self.assertEqual(len(self.calls), 4)

    def test_cancel_closes_owned_command(self):
        self.begin_wait()
        self.stays_waiting()
        self.term.write(b'\x03')
        self.wait(lambda: any(e['type'] == 'turn_interrupted' for e in self.new_events()))
        closed = [e for e in self.new_events() if e['type'] == 'process_closed']
        self.assertEqual(len(closed), 1)
        self.assertNotEqual(closed[0]['data']['result']['status'], 'running')
        self.assertEqual(len(self.calls), 2)

    def test_yield_explicitly_returns_to_model(self):
        self.begin_wait()
        self.stays_waiting()
        self.term.write(b'/yield\r')
        self.wait(lambda: any(e['type'] == 'turn_completed' for e in self.new_events()))
        self.assertEqual(len(self.calls), 4)
        session = next((self.root / 'state' / 'sessions').iterdir()).name
        self.term.write(b'/exit\r')
        self.wait(lambda: self.term.state().get('state') == 'exited')
        resumed = subprocess.run([str(BINARY), '--dotdir', str(self.root / 'state'),
                                  '--config', str(self.config), '--no-listen', '--no-client',
                                  '-e', '--resume', session, '--', 'check replay'],
                                 cwd=self.root, input='', text=True, capture_output=True,
                                 env={**os.environ, 'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                 timeout=10)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)

    def test_urgent_irc_wakes_the_wait(self):
        self.begin_wait()
        self.stays_waiting()
        self.peer.sock.sendall(b'PRIVMSG #lab :querybot: synthetic-callback\r\n')
        self.wait(lambda: len(self.calls) >= 4)
        self.assertIn('synthetic-callback', json.dumps(self.calls[2]['input']))
        self.assertEqual((self.root / 'starts').read_text(), 'x')

    def test_retry_off_still_waits_for_operator(self):
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.begin_wait()
        self.stays_waiting()
        self.term.write(b'synthetic-callback\r')
        self.wait(lambda: any(e['type'] == 'turn_completed' for e in self.new_events()))
        self.assertEqual((self.root / 'starts').read_text(), 'x')


if __name__ == '__main__':
    unittest.main()
