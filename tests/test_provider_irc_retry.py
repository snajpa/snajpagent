# SPDX-License-Identifier: GPL-2.0-only
"""Buffered IRC cannot abandon a failed response at a blocked goal boundary."""

import json
import unittest

from test_irc_queries import QueryFixture
from test_provider_https import event, response


class BufferedRetryTests(QueryFixture):
    def block_goal(self, paused=False):
        issued = []

        def block(handler, request, sequence):
            if paused:
                self.held.set()
                assert self.release.wait(10), 'fixture goal was not paused'
                body = self.provider.response_body(sequence, 'fixture goal paused')
            elif not issued:
                issued.append(True)
                body = self.provider.function_body(sequence, 'block', 'update_goal', {
                    'action': 'block', 'text': 'waiting for fixture dependency',
                    'wait_for': 'irc: 127.0.0.1/query-peer'})
            else:
                body = self.provider.response_body(sequence, 'fixture goal blocked')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = block
        if paused:
            self.term.write(b'/rollout\r/goal finish fixture dependency\r')
            self.wait(self.held.is_set)
            self.term.write(b'/goal pause\r')
            self.wait(lambda: any(e['type'] == 'goal_paused' for e in self.events()))
            self.release.set()
            self.wait_idle()
            self.release.clear()
            self.held.clear()
        else:
            before = self.events()[-1]['seq']
            self.term.write(b'/rollout\r/goal finish fixture dependency\r')
            self.wait(lambda: any(e['seq'] > before and e['type'] == 'goal_blocked'
                                 for e in self.events()))
            self.wait_idle()
            self.assertEqual(sum(e['type'] == 'goal_blocked' for e in self.events()), 1)

    def exercise(self, mode='chat', failures=1, paused=False):
        self.block_goal(paused)
        before = self.events()[-1]['seq']
        requests = []

        def respond(handler, request, sequence):
            requests.append(request)
            n = len(requests)
            if n == 1:
                body = self.provider.function_body(sequence, 'once', 'exec_command', {
                    'command': 'printf x >> retry-once', 'workdir': str(self.root),
                    'stdin': None, 'pty': False, 'timeout_ms': None,
                    'yield_ms': 1000, 'max_output_tokens': 1000})
                self.provider.reply(handler, body.encode(), close_header=True)
            elif n <= failures + 1:
                body = event('response.created', response={
                    'id': 'interrupted', 'status': 'in_progress', 'output': []})
                body += event('response.output_item.added', output_index=0, item={
                    'id': 'private', 'type': 'reasoning', 'summary': []})
                handler.send_response(200)
                handler.send_header('Content-Type', 'text/event-stream')
                handler.send_header('Content-Length', str(len(body) + 100))
                handler.send_header('Connection', 'close')
                handler.end_headers()
                handler.wfile.write(body)
                handler.wfile.flush()
                self.held.set()
                assert self.release.wait(10), 'fixture did not release stream'
                handler.close_connection = True
            else:
                self.provider.reply(handler, self.provider.response_body(
                    sequence, 'recovered interrupted response').encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.term.write(b'retry original work\r')
        self.wait(self.held.is_set)
        marker = 'new fixture input'
        if mode == 'queue':
            self.term.write(('/queue ' + marker + '\r').encode())
            self.wait(lambda: any(e['type'] == 'future_turn_queued' and
                      e['data'].get('text') == marker for e in self.events()))
        else:
            text = ('querybot: ' if mode == 'mention' else '') + marker
            command = 'NOTICE' if mode == 'notice' else 'PRIVMSG'
            self.peer.sock.sendall(f'{command} #lab :{text}\r\n'.encode())
            self.wait(lambda: any(e['type'] in ('irc_event', 'irc_event_v2') and
                      e['data'].get('text') == text for e in self.events()))
        if mode == 'cancel':
            self.term.write(b'\x03')
            self.wait(lambda: any(e['seq'] > before and e['type'] == 'turn_interrupted'
                                 for e in self.events()))
        self.release.set()
        if mode != 'cancel':
            self.wait(lambda: len(requests) > failures + 1 or any(
                e['seq'] > before and e['type'] == 'turn_failed' for e in self.events()))
            if mode in ('mention', 'queue'):
                self.wait(lambda: len(requests) > failures + 1)
            self.assertGreater(len(requests), failures + 1,
                               'buffered IRC abandoned the interrupted turn')
            self.wait(lambda: any(e['seq'] > before and e['type'] == 'turn_completed'
                                 for e in self.events()))
        events = [e for e in self.events() if e['seq'] > before]
        self.assertEqual((self.root / 'retry-once').read_text(), 'x')
        self.assertEqual(sum(e['type'] == 'tool_started' for e in events), 1)
        self.assertFalse(any(e['type'] == 'goal_resumed' for e in events))
        if mode in ('chat', 'notice'):
            self.assertEqual(sum(e['type'] == 'turn_started' for e in events), 1)
            self.assertFalse(any(e['type'] == 'turn_failed' for e in events))
            self.assertEqual(requests[1], requests[2])
            self.assertNotIn(marker, json.dumps(requests[1:]))
            failed = [e['data'] for e in events if e['type'] == 'response_failed']
            self.assertEqual(len(failed), int(failures > 2))
            if failed:
                self.assertFalse(failed[0]['new_input'])
                self.assertEqual(failed[0]['retry_count'], 2)
                self.assertEqual(failed[0]['turn_retry_attempts'], 1)
        elif mode == 'cancel':
            self.assertEqual(len(requests), 2)
        else:
            self.assertNotEqual(requests[1], requests[2])
            self.assertIn(marker, json.dumps(requests[2:]))

    def test_background_chat_allows_reasoning_retry(self):
        self.exercise()

    def test_paused_goal_keeps_background_chat_pending(self):
        self.exercise(paused=True)

    def test_background_notice_allows_reasoning_retry(self):
        self.exercise('notice')

    def test_exhausted_transport_uses_turn_retry_budget(self):
        self.exercise(failures=3)

    def test_mention_still_hands_off_to_fresh_input(self):
        self.exercise('mention')

    def test_queue_still_hands_off_to_fresh_input(self):
        self.exercise('queue')

    def test_cancel_stops_recovery(self):
        self.exercise('cancel')

    def active_goal_retry(self, mode='chat'):
        before = self.events()[-1]['seq']
        requests = []

        def respond(handler, request, sequence):
            requests.append(request)
            n = len(requests)
            if n == 1:
                body = self.provider.function_body(sequence, 'once', 'exec_command', {
                    'command': 'printf x >> retry-once', 'workdir': str(self.root),
                    'stdin': None, 'pty': False, 'timeout_ms': None,
                    'yield_ms': 1000, 'max_output_tokens': 1000})
            elif n == 2:
                body = response(partial=True) if mode == 'partial' else event(
                    'response.created', response={
                        'id': 'interrupted', 'status': 'in_progress', 'output': []})
                handler.send_response(200)
                handler.send_header('Content-Type', 'text/event-stream')
                handler.send_header('Content-Length', str(len(body) + 100))
                handler.send_header('Connection', 'close')
                handler.end_headers()
                handler.wfile.write(body)
                handler.wfile.flush()
                self.held.set()
                assert self.release.wait(10), 'fixture did not release stream'
                handler.close_connection = True
                return
            elif n == 3:
                body = self.provider.function_body(sequence, 'complete', 'update_goal', {
                    'action': 'complete', 'text': None})
            else:
                body = self.provider.response_body(sequence, 'recovered active goal')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        if mode == 'all':
            self.term.write(b'/steering all\r')
            self.wait(lambda: any(e['type'] == 'steering_updated' and
                      e['data'].get('mode') == 'all' for e in self.events()))
        self.term.write(b'/rollout\r/goal retry active goal\r')
        self.wait(self.held.is_set)
        marker = 'background remains deferred'
        text = ('querybot: ' if mode == 'mention' else '') + marker
        self.peer.sock.sendall(f'PRIVMSG #lab :{text}\r\n'.encode())
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and
                  e['data'].get('text') == text for e in self.events()))
        self.release.set()
        self.wait(lambda: any(e['seq'] > before and e['type'] == 'goal_completed'
                             for e in self.events()))
        self.wait_idle()
        events = [e for e in self.events() if e['seq'] > before]
        failed = [e['data'] for e in events if e['type'] == 'response_failed']
        if mode in ('all', 'mention'):
            self.assertEqual(len(failed), 1)
            self.assertTrue(failed[0]['new_input'])
            self.assertIn(marker, json.dumps(requests[2]['input']))
        elif mode == 'partial':
            self.assertEqual(len(failed), 1)
            self.assertFalse(failed[0]['new_input'])
            self.assertEqual(failed[0]['partial_public'][0]['text'], 'partial answer')
            self.assertIn('partial answer', json.dumps(requests[2]['input']))
            self.assertNotIn(marker, json.dumps(requests[1:3]))
        else:
            self.assertFalse(failed, 'deferred background input vetoed safe provider retry')
            self.assertEqual(requests[1], requests[2])
            self.assertNotIn(marker, json.dumps(requests[1:3]))
            self.assertNotIn(b'provider retry stopped', self.term.output)
        self.assertEqual((self.root / 'retry-once').read_text(), 'x')
        self.assertEqual(sum(e['type'] == 'tool_started' for e in events), 2)

    def test_active_goal_background_chat_keeps_safe_transport_retry(self):
        self.active_goal_retry()

    def test_active_goal_all_steering_rebuilds_failed_request(self):
        self.active_goal_retry('all')

    def test_active_goal_mention_rebuilds_failed_request(self):
        self.active_goal_retry('mention')

    def test_active_goal_recovers_partial_output_once(self):
        self.active_goal_retry('partial')


if __name__ == '__main__':
    unittest.main()
