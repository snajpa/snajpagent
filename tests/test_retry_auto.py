# SPDX-License-Identifier: GPL-2.0-only
"""Automatic retry policy through real HTTP, live input and durable sessions."""

import json
import re
import unittest

from test_irc_queries import QueryFixture
from tmux_terminal import read_events


class RetryFixture(QueryFixture):
    retry_default = None

    def setUp(self):
        super().setUp()
        # The join is durable before the normal background debounce starts its
        # turn. Wait for that actual admission before installing a failure hook.
        self.wait(lambda: any(e['type'] == 'turn_started' and
                  'event=join sender=observer' in e['data'].get('text', '')
                  for e in self.events()))
        self.wait_idle()

    def start(self, *args, **kwargs):
        if self.retry_default is not None and 'retry_auto=' not in self.config.read_text():
            self.config.write_text(self.config.read_text().replace(
                '[agent]\n', '[agent]\nretry_auto=' + str(self.retry_default).lower() + '\n'))
        return super().start(*args, '--color=always', **kwargs)

    def command(self, text, marker):
        self.term.output.clear()
        self.term.write(text.encode() + b'\r')
        self.term.until(marker.encode())

    def begin(self, failures=1, held=False, prior_tool=False, delay=0, goal=False):
        self.attempts = []
        self.before = self.events()[-1]['seq']

        def respond(handler, request, sequence):
            self.attempts.append(request)
            n = len(self.attempts) - int(prior_tool)
            if n == 0:
                body = self.provider.function_body(sequence, 'once', 'exec_command', {
                    'command': 'printf x >> retry-once', 'workdir': str(self.root),
                    'stdin': None, 'pty': False, 'timeout_ms': None,
                    'yield_ms': 1000, 'max_output_tokens': 1000})
                self.provider.reply(handler, body.encode(), close_header=True)
            else:
                self.held.set()
                if held and n == 1:
                    assert self.release.wait(10), 'fixture did not release request'
                if n <= failures:
                    body = json.dumps({'error': {'code': 'server_error',
                                                'message': 'temporary retry fixture'}}).encode()
                    handler.send_response(503)
                    handler.send_header('Content-Type', 'application/json')
                    handler.send_header('Content-Length', str(len(body)))
                    handler.send_header('Retry-After', str(delay))
                    handler.send_header('Connection', 'close')
                    handler.end_headers()
                    handler.wfile.write(body)
                    handler.close_connection = True
                else:
                    self.provider.reply(handler, self.provider.response_body(
                        sequence, 'retry fixture recovered').encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.term.write(b'/rollout\r' + (b'/goal ' if goal else b'') + b'retry fixture work\r')
        self.wait(self.held.is_set)

    def begin_stream(self, outcomes, goal=False):
        self.attempts = []
        self.before = self.events()[-1]['seq']
        self.term.output.clear()

        def respond(handler, request, sequence):
            outcome = outcomes[len(self.attempts)]
            self.attempts.append(request)
            if outcome == 'failure':
                body = self.provider.event('response.failed', {'response': {'error': {
                    'code': 'service_unavailable',
                    'message': 'The server is overloaded or not ready yet. Please try again later. '
                               f'(fixture request {len(self.attempts)})'}}})
            elif outcome == 'tool':
                body = self.provider.function_body(sequence, 'between', 'exec_command', {
                    'command': 'printf x >> retry-once', 'workdir': str(self.root),
                    'stdin': None, 'pty': False, 'timeout_ms': None,
                    'yield_ms': 1000, 'max_output_tokens': 1000})
            else:
                body = self.provider.response_body(sequence, 'retry fixture recovered')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.term.write(b'/rollout\r' + (b'/goal ' if goal else b'') + b'retry fixture work\r')

    def finished(self, kind):
        self.wait(lambda: any(e['seq'] > self.before and e['type'] == kind
                             for e in self.events()))
        return [e for e in self.events() if e['seq'] > self.before]


class RetryAutoTests(RetryFixture):
    def test_setting_feedback_echo_invalid_and_status(self):
        self.command('/retry auto', 'Automatic retry: ON (configuration)')
        self.command('/retry auto off', 'Automatic retry: OFF (session override)')
        self.assertIn(b'/retry auto', bytes(self.term.output))
        self.command('/status', 'automatic retry: OFF (session override)')
        self.command('/retry auto on', 'Automatic retry: ON (session override)')
        self.command('/retry auto on', 'Automatic retry: ON (session override)')
        self.command('/retry auto off extra', 'usage: /retry auto [on|off]')
        self.command('/retry auto', 'Automatic retry: ON (session override)')
        values = [e['data']['value'] for e in self.events() if e['type'] == 'retry_auto_changed']
        self.assertEqual(values, ['off', 'on'])

    def test_default_recovers_same_turn_and_keeps_tool_effect(self):
        self.begin(failures=3, prior_tool=True)
        events = self.finished('turn_completed')
        self.assertEqual(len(self.attempts), 5)
        self.assertEqual(sum(e['type'] == 'turn_started' for e in events), 1)
        self.assertEqual(sum(e['type'] == 'turn_recovery' for e in events), 1)
        self.assertEqual((self.root / 'retry-once').read_text(), 'x')

    def test_stream_failures_are_consecutive_yellow_status_lines(self):
        self.begin_stream(['failure'] * 3 + ['success'])
        events = self.finished('turn_completed')
        output = bytes(self.term.output)
        self.assertNotIn(b'recovering automatically', output)
        self.assertNotIn(b'snajpagent: response.failed', output)
        lines = re.findall(rb'\x1b\[1;33m([^\x1b]*Retrying turn after error[^\x1b]*)', output)
        self.assertEqual(len(lines), 3, output)
        for number, line in enumerate(lines, 1):
            self.assertIn(f'({number}/5)'.encode(), line)
        failures = [e for e in events if e['type'] == 'response_failed']
        self.assertEqual(len(failures), 3)
        self.assertTrue(all('overloaded' in e['data']['message'] for e in failures))
        self.assertEqual(len(self.attempts), 4)

    def test_healthy_tool_graph_resets_consecutive_retry_counter(self):
        self.begin_stream(['failure', 'tool', 'failure', 'success'])
        self.finished('turn_completed')
        output = bytes(self.term.output)
        self.assertEqual(output.count(b'Retrying turn after error (1/5)'), 2, output)
        self.assertNotIn(b'Retrying turn after error (2/5)', output)
        self.assertEqual((self.root / 'retry-once').read_text(), 'x')

    def test_off_during_stream_backoff_shows_terminal_failure(self):
        self.begin_stream(['failure', 'success'])
        self.term.until(b'Retrying turn after error (1/5)')
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.finished('turn_failed')
        self.term.until(b'snajpagent: response.failed')
        self.assertEqual(len(self.attempts), 1)

    def test_terminal_stream_failure_keeps_error_diagnostic(self):
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.begin_stream(['failure'])
        self.finished('turn_failed')
        self.term.until(b'snajpagent: response.failed')
        output = bytes(self.term.output)
        self.assertRegex(output, rb'\x1b\[1;31m[^\x1b]*response.failed')
        self.assertNotIn(b'Retrying turn after error', output)
        self.assertNotIn(b'recovering automatically', output)
        self.assertEqual(len(self.attempts), 1)

    def test_off_skips_retries_and_manual_retry_preserves_tool(self):
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.begin(prior_tool=True)
        self.finished('turn_failed')
        self.assertEqual(len(self.attempts), 2)
        self.term.write(b'/retry\r')
        self.finished('turn_completed')
        self.assertEqual(len(self.attempts), 3)
        self.assertEqual((self.root / 'retry-once').read_text(), 'x')

    def test_off_stops_failed_goal_and_commands_do_not_restart_it(self):
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.begin(failures=20, goal=True)
        events = self.finished('turn_failed')
        self.command('/status', 'automatic retry: OFF (session override)')
        self.command('/retry auto on', 'Automatic retry: ON')
        self.command('/status', 'automatic retry: ON (session override)')
        self.assertEqual(len(self.attempts), 1)
        self.assertTrue(any(e['type'] == 'goal_started' for e in events))
        self.assertFalse(any(e['type'] in ('goal_paused', 'goal_blocked') for e in events))

    def test_switch_off_does_not_interrupt_healthy_response(self):
        self.begin(failures=0, held=True)
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.release.set()
        events = self.finished('turn_completed')
        self.assertEqual(len(self.attempts), 1)
        self.assertFalse(any(e['type'] in ('response_interrupted', 'response_failed') for e in events))

    def test_off_during_transport_backoff(self):
        self.begin(failures=20, delay=2)
        self.term.until(b'provider retry 1/2')
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.finished('turn_failed')
        self.assertEqual(len(self.attempts), 1)

    def test_off_during_turn_backoff(self):
        self.begin(failures=20, delay=1)
        self.term.until(b'Retrying turn after error (1/5)')
        self.command('/retry auto off', 'Automatic retry: OFF')
        events = self.finished('turn_failed')
        self.assertEqual(len(self.attempts), 3)
        self.assertTrue(any(e['type'] == 'turn_recovery' for e in events))

    def test_off_reaches_running_background_irc_summary(self):
        marker = 'summary source retained'
        self.peer.sock.sendall(f'PRIVMSG #lab :{marker}\r\n'.encode())
        self.wait(lambda: any(marker in json.dumps(request) for request in self.seen))
        self.wait_idle()
        branches, issued = [], []

        def respond(handler, request, sequence):
            if 'IRC context compaction branch' in json.dumps(request):
                self.assertTrue(request.get('tools'), 'summary lost its cached tool declarations')
                self.assertEqual(request.get('tool_choice'), 'none')
                branches.append(request)
                self.held.set()
                assert self.release.wait(10), 'summary fixture was not released'
                self.provider.reply(handler, b'{"error":{"message":"summary unavailable"}}',
                                    'application/json', status=503, close_header=True)
            elif not issued:
                issued.append(True)
                body = self.provider.function_body(sequence, 'compact', 'irc_compact',
                                                   {'after_updates': 1})
                self.provider.reply(handler, body.encode(), close_header=True)
            else:
                self.provider.reply(handler, self.provider.response_body(
                    sequence, 'foreground remains usable').encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.term.write(b'/rollout\renable background summary\r')
        self.wait(self.held.is_set)
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.release.set()
        self.term.until(b'IRC context compaction failed')
        self.assertEqual(len(branches), 1)
        self.wait_idle()

    def test_override_survives_configure_and_resume(self):
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.config.write_text(self.config.read_text().replace('[agent]\n',
                                                             '[agent]\nretry_auto=true\n'))
        self.command('/configure', 'configuration reloaded')
        self.command('/status', 'automatic retry: OFF (session override)')
        path, _ = read_events(self.root / 'state')
        self.command('/exit', '--resume')
        self.term.wait_exit()
        self.peer.close()
        self.observer.close()
        self.term = self.start('--resume', path.parent.name)
        self.command('/status', 'automatic retry: OFF (session override)')
        self.begin()
        self.finished('turn_failed')
        self.assertEqual(len(self.attempts), 1)


class ConfigOffTests(RetryFixture):
    retry_default = False

    def test_configuration_off_and_session_on_override(self):
        self.command('/status', 'automatic retry: OFF (configuration)')
        self.begin()
        self.finished('turn_failed')
        self.assertEqual(len(self.attempts), 1)
        self.command('/retry auto on', 'Automatic retry: ON')
        self.begin()
        self.finished('turn_completed')
        self.assertEqual(len(self.attempts), 2)

    def test_reload_changes_inherited_default_only_when_requested(self):
        self.config.write_text(self.config.read_text().replace('retry_auto=false', 'retry_auto=true'))
        self.command('/status', 'automatic retry: OFF (configuration)')
        self.command('/configure', 'configuration reloaded')
        self.command('/status', 'automatic retry: ON (configuration)')


if __name__ == '__main__':
    unittest.main()
