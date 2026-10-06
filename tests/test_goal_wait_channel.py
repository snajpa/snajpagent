#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Real model tool calls retain and report the blocked goal's dependency."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import tmux_terminal as harness
from test_remote_terminal import RemoteProcess


BINARY = Path(sys.argv.pop(1)).resolve()


class WaitChannelTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='snag-goal-wait-', dir='/tmp')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.state = self.root / 'state'
        self.state.mkdir(mode=0o700)
        self.provider = harness.FakeResponses()
        self.addCleanup(self.provider.close)
        self.requests = []
        self.steps = []
        self.children = []
        self.addCleanup(self.close_children)
        self.provider.runtime_handler = self.respond
        config = self.state / 'config.ini'
        config.write_text('[agent]\nprovider=fixture\nmodel=host-model\n'
            'read_agents_md=false\nmax_turn_retries=0\n'
            f'[provider fixture]\nbase_url=http://127.0.0.1:{self.provider.port}\n'
            'api_key="irc-ui-secret"\nexact_token_count=false\n'
            '[ui]\ncolor=never\nmarkdown=false\npager=off\n'
            'prompt={provider}/{model}/{effort}{chat: CHAT>}'
            '{rollout-idle: READY>}{rollout-active: BUSY>}\n')
        config.chmod(0o600)

    def close_children(self):
        for child in self.children:
            child.close()
        # Native owners survive frontend loss. Only this fixture's private path
        # identifies the processes that belong to its cleanup.
        def owners():
            rows = subprocess.check_output(['ps', '-axww', '-o', 'pid=', '-o', 'command='],
                                           text=True)
            return [int(row.split(None, 1)[0]) for row in rows.splitlines()
                    if '--dotdir ' + str(self.state) + ' ' in row]
        for pid in owners():
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 5
        while owners() and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertEqual(owners(), [])

    def respond(self, handler, request, sequence):
        self.requests.append(request)
        index = len(self.requests) - 1
        if index < len(self.steps):
            name, arguments = self.steps[index]
            body = self.provider.function_body(sequence, f'wait_call_{sequence}', name, arguments)
        elif index == len(self.steps) or index > len(self.steps) + 1:
            body = self.provider.response_body(sequence, 'WAIT_DONE')
        else:
            # Bound pre-fix automatic continuation after an unrecognized argument.
            body = self.provider.function_body(sequence, f'cleanup_{sequence}', 'update_goal',
                                               {'action': 'complete', 'text': None})
        self.provider.reply(handler, body.encode(), close_header=True)

    def start(self, *args):
        child = RemoteProcess(self.root, [str(BINARY), '--dotdir', str(self.state),
            '--no-listen', '--no-client', *args], wrapped=None, winsize=(30, 220))
        self.children.append(child)
        child.until(b'READY>', 10)
        return child

    def send(self, child, text, marker):
        child.output.clear()
        os.write(child.master, text.encode() + b'\r')
        return child.until(marker.encode(), 10)

    def events(self):
        journal, = (self.state / 'sessions').glob('*/events.jsonl')
        return journal, [json.loads(line) for line in journal.read_bytes().splitlines()]

    def block(self, wait_for, timer=False):
        self.steps = [('update_goal', {'action': 'block', 'text': 'dependency is unavailable',
                                      'wait_for': wait_for})]
        if timer:
            self.steps.append(('timer', {'delay_ms': 60000, 'text': 'recheck dependency'}))
        child = self.start()
        self.send(child, '/goal finish the fixture', 'WAIT_DONE')
        child.until(('idle: goal blocked, waiting for ' + wait_for).encode(), 10)
        return child

    def test_irc_wait_is_named_in_notice_status_and_resumed_context(self):
        wait_for = 'irc: team.example:6697/secretary'
        child = self.block(wait_for)
        output = bytes(child.output)
        self.assertIn(('Goal blocked by model; waiting for ' + wait_for).encode(), output)
        self.assertIn(('idle: goal blocked, waiting for ' + wait_for).encode(), output)
        self.assertNotIn(b'awaiting operator', output)
        self.assertNotIn(b'waiting for operator', output)
        self.send(child, '/goal', 'wait channel: ' + wait_for)
        journal, events = self.events()
        blocked, = (e for e in events if e['type'] == 'goal_blocked')
        self.assertEqual(blocked['data']['wait_for'], wait_for)
        self.assertEqual(sum(e['type'] == 'turn_started' for e in events), 1)
        self.send(child, '/goal set finish the revised fixture', 'Goal updated')
        self.send(child, '/goal', 'wait channel: ' + wait_for)
        self.send(child, '/exit', '--resume')
        child.wait(0)
        child.close()
        self.children.remove(child)
        self.steps, self.requests = [('list_goals', {})], []
        resumed = self.start('--resume', journal.parent.name)
        self.send(resumed, '/goal', 'wait channel: ' + wait_for)
        self.send(resumed, 'check the saved dependency', 'WAIT_DONE')
        resumed.until_after(b'WAIT_DONE', b'READY>', 10)
        self.assertEqual(len(self.requests), 2)
        self.assertIn('Wait channel: ' + wait_for, json.dumps(self.requests[0]))
        self.assertIn('wait channel: ' + wait_for, json.dumps(self.requests[1]))

    def test_timer_label_does_not_claim_a_scheduled_timer(self):
        child = self.block('timer')
        self.assertIn(b'idle: goal blocked, waiting for timer', bytes(child.output))
        self.assertNotIn(b'timer scheduled', bytes(child.output))

    def test_missing_wait_channel_is_rejected_without_blocking(self):
        self.steps = [('update_goal', {'action': 'block', 'text': 'dependency is unavailable'}),
                      ('update_goal', {'action': 'complete', 'text': None})]
        child = self.start()
        self.send(child, '/goal validate the block request', 'WAIT_DONE')
        child.until_after(b'WAIT_DONE', b'READY>', 10)
        _, events = self.events()
        self.assertFalse(any(e['type'] == 'goal_blocked' for e in events))
        self.assertIn('wait_for', json.dumps(self.requests[1]))

    def test_malformed_wait_channels_are_rejected(self):
        for wait_for in (None, '', 'irc: missing-peer', 'irc: /nick',
                         'irc: endpoint/', 'external: ', 'operator\nforged notice', 'unknown'):
            self.steps = [('update_goal', {'action': 'block', 'text': 'blocked',
                                          'wait_for': wait_for}),
                          ('update_goal', {'action': 'complete', 'text': None})]
            self.requests = []
            child = self.start()
            self.send(child, '/goal validate the wait channel', 'WAIT_DONE')
            child.until_after(b'WAIT_DONE', b'READY>', 10)
            self.assertIn('block requires wait_for', json.dumps(self.requests[1]), wait_for)
            self.assertNotIn(b'Goal blocked by model', bytes(child.output))
            self.send(child, '/exit', '--resume')
            child.wait(0)
            child.close()
            self.children.remove(child)

    def test_declared_channels_are_independent_of_timer_scheduling(self):
        for wait_for in ('operator', 'timer', 'process: build-42', 'external: CI job 123'):
            with self.subTest(wait_for=wait_for):
                child = self.block(wait_for, timer=True)
                self.assertIn(('waiting for ' + wait_for).encode(), bytes(child.output))
                self.assertIn(b'timer scheduled', bytes(child.output))
                if wait_for != 'operator':
                    self.assertNotIn(b'waiting for operator', bytes(child.output))
                self.send(child, '/goal complete', 'Goal cleared')
                self.send(child, '/goal', ': completed')
                self.assertNotIn(b'wait channel:', bytes(child.output))
                self.send(child, '/exit', '--resume')
                child.wait(0)
                child.close()
                self.children.remove(child)
                self.steps, self.requests = [], []


if __name__ == '__main__':
    unittest.main()
