#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Configured fallback through real HTTP, owner journal and shared prompt state."""

import json
import os
import select
import threading
import time
import unittest

import test_session_view as session


class ModelFallbackTests(unittest.TestCase):
    def setUp(self):
        self.owner = session.SessionViewTests(
            "test_legacy_observer_is_a_committed_prefix" if self._testMethodName.endswith("_legacy")
            else "runTest")
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)
        self.provider = self.owner.provider
        self.release = threading.Event()
        self.addCleanup(self.release.set)
        self.entered = threading.Event()

    def command(self, text, notice='Fallback:'):
        child = self.owner.initial
        child.output.clear()
        os.write(child.master, text.encode() + b'\r')
        child.until(notice.encode(), 5)

    def deny(self, handler, sequence, yellow=False, code='cyber_policy'):
        # Unknown provider activity makes this a terminal error under the
        # existing classifier. Its incomplete local call must never execute.
        body = '' if yellow else self.provider.function_body(
            sequence, 'unexecuted', 'read_file', {'path': '/missing-fallback-fixture'})
        body = body.split('event: response.completed')[0]
        if not yellow:
            body += self.provider.event('response.unknown_activity', {})
        response = {'error': {'code': code, 'message': 'synthetic provider policy decision'}}
        if not yellow:
            response['output'] = [{'type': 'function_call', 'id': f'fc_irc_ui_{sequence}_0',
                                   'call_id': 'unexecuted', 'name': 'read_file',
                                   'arguments': '{"path":"/missing-fallback-fixture"}',
                                   'status': 'completed'}]
        body += self.provider.event('response.failed', {'response': response})
        self.provider.reply(handler, body.encode(), close_header=True)

    def models(self):
        return [request['model'] for request in self.provider.requests]

    def completed(self, count=1):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            events = [event for event in self.owner.events() if event['type'] == 'turn_completed']
            if len(events) >= count:
                return events[-1]
            time.sleep(.02)
        self.fail(('missing completion', self.owner.events()[-5:], self.models()))

    def test_switch_preserves_tools_updates_prompt_and_returns_to_primary(self):
        self.command('/fallback fake/two-model/high:100000')
        self.assertEqual(self.provider.requests, [])
        self.command('/retry auto off', 'Automatic retry: OFF')
        self.command('/context 60000', 'selected=60000')
        fixture = self.owner.root / 'input.txt'
        fixture.write_text('retained-tool-result')

        def respond(handler, request, sequence):
            if sequence == 1:
                body = self.provider.function_body(sequence, 'read_once', 'read_file',
                                                   {'path': str(fixture)})
            elif sequence == 2:
                return self.deny(handler, sequence)
            elif sequence == 3:
                self.entered.set()
                self.release.wait(10)
                body = self.provider.response_body(sequence, 'fallback-finished')
            else:
                body = self.provider.response_body(sequence, 'primary-finished')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        peer = None if session.OMITTED else self.owner.view()
        os.write(self.owner.initial.master, b'run first turn\r')
        self.assertTrue(self.entered.wait(8), self.models())
        if peer:
            state = peer.until('state', lambda message:
                               message['state']['prompt']['values'][1:3] == ['two-model', 'high'])
            self.assertEqual(state['state']['prompt']['values'][0], 'fake')
        self.owner.initial.until(b'fake/two-model/high')
        fallback_request = self.provider.requests[-1]['body']
        results = [item['output'] for item in fallback_request['input']
                   if item.get('type') == 'function_call_output']
        self.assertTrue(any('retained-tool-result' in result for result in results), results)
        call, = [item for item in fallback_request['input']
                 if item.get('type') == 'function_call']
        self.assertEqual(call['name'], 'read_file')
        self.assertEqual(json.loads(call['arguments'])['path'], str(fixture))
        self.release.set()
        self.completed()
        if peer:
            peer.until('state', lambda message:
                       message['state']['prompt']['values'][1] == 'host-model')
        os.write(self.owner.initial.master, b'run next turn\r')
        self.completed(2)
        self.assertEqual(self.models(), ['host-model', 'host-model', 'two-model', 'host-model'])
        events = self.owner.events()
        switched, = [e['data'] for e in events if e['type'] == 'turn_fallback_started']
        self.assertEqual(switched['context_tokens'], 100000)
        self.assertEqual(len([e for e in events if e['type'] == 'tool_started']), 1)
        requests = [e['data'] for e in events if e['type'] == 'response_started']
        self.assertEqual(requests[2]['hard_input_tokens'], 90000)
        self.assertEqual(requests[3]['hard_input_tokens'], 54000)
        self.assertFalse(any(e['type'] == 'model_selection_changed' for e in events))

    def test_same_identity_fallback_applies_its_own_context(self):
        self.command('/fallback fake/host-model/medium:100000')

        def respond(handler, request, sequence):
            if sequence == 1:
                return self.deny(handler, sequence)
            self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'same identity fixture\r')
        self.completed()
        self.assertEqual(self.models(), ['host-model', 'host-model'])
        requests = [e['data'] for e in self.owner.events() if e['type'] == 'response_started']
        self.assertEqual(requests[1]['hard_input_tokens'], 90000)
        self.assertIsNone(requests[0]['hard_input_tokens'])

    def explicit_selection(self, command, notice):
        self.command('/context 60000', 'selected=60000')
        self.command('/fallback fake/host-model/medium:100000')
        continued = threading.Event()

        def respond(handler, request, sequence):
            if sequence == 1:
                return self.deny(handler, sequence)
            if sequence == 2:
                self.entered.set()
                self.release.wait(10)
            else:
                continued.set()
            try:
                self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                    close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'explicit selection fixture\r')
        self.assertTrue(self.entered.wait(5))
        self.command(command, notice)
        try:
            self.assertTrue(continued.wait(3), self.models())
        finally:
            self.release.set()
        self.completed()
        requests = [e['data'] for e in self.owner.events() if e['type'] == 'response_started']
        self.assertEqual(requests[-1]['hard_input_tokens'], 54000)
        switches = [e for e in self.owner.events() if e['type'] == 'turn_model_changed']
        self.assertEqual(len(switches), 1)

    def test_explicit_context_clears_same_model_fallback_context(self):
        self.explicit_selection('/context 60000', 'selected=60000')

    def test_explicit_effort_clears_same_model_fallback_context(self):
        self.explicit_selection('/effort medium', 'effort for next response')

    def test_explicit_model_clears_same_model_fallback_context(self):
        self.explicit_selection('/model fake/host-model/medium:60000', 'model for next response')

    def switch_during_retry(self, goal):
        def respond(handler, request, sequence):
            if request['model'] == 'host-model':
                error = {'code': 'model_not_found',
                         'message': f'synthetic model failure {sequence}'}
                body = json.dumps({'error': error}).encode()
                handler.send_response(400)
                handler.send_header('Content-Length', str(len(body)))
                handler.end_headers()
                handler.wfile.write(body)
                return
            self.entered.set()
            self.release.wait(10)
            self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                close_header=True)

        self.provider.runtime_handler = respond
        text = '/goal retry selection fixture' if goal else 'retry selection fixture'
        os.write(self.owner.initial.master, text.encode() + b'\r')
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            recoveries = [e for e in self.owner.events() if e['type'] == 'turn_recovery']
            if len(recoveries) == 4:
                break
            child = self.owner.initial
            if select.select([child.master], [], [], .02)[0]:
                child.output.extend(os.read(child.master, 65536))
        self.assertEqual(len(recoveries), 4, bytes(self.owner.initial.output))
        # The fourth failure starts a two-second wait. An explicit selection
        # must start the replacement promptly, without waiting out that timer.
        self.command('/model fake/two-model/high', 'model for next response')
        self.assertTrue(self.entered.wait(1), self.models())
        self.owner.initial.until(b'fake/two-model/high', 1)
        if not session.OMITTED:
            peer = self.owner.view()
            peer.until('state', lambda message:
                       message['state']['prompt']['values'][1:3] == ['two-model', 'high'])
        self.assertEqual(self.models(), ['host-model'] * 4 + ['two-model'])
        events = self.owner.events()
        self.assertEqual(sum(e['type'] == 'turn_started' for e in events), 1)
        self.assertEqual(sum(e['type'] == 'turn_model_changed' for e in events), 1)
        self.assertIn('retry selection fixture', json.dumps(self.provider.requests[-1]['body']))
        if goal:
            self.command('/goal pause', 'Goal paused')
        self.release.set()
        self.completed()

    def test_model_change_wakes_turn_retry(self):
        self.switch_during_retry(False)

    def test_model_change_wakes_active_goal_retry(self):
        self.switch_during_retry(True)

    def test_yellow_clarification_stays_on_primary(self):
        self.command('/fallback fake/two-model/high')

        def respond(handler, request, sequence):
            if sequence == 1:
                return self.deny(handler, sequence, yellow=True)
            self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'clarification fixture\r')
        self.completed()
        self.assertEqual(self.models(), ['host-model', 'host-model'])
        self.assertFalse(any(e['type'] == 'turn_fallback_started' for e in self.owner.events()))
        self.assertTrue(any(e['type'] == 'response_output_correction' for e in self.owner.events()))

    def test_fallback_failure_stops_without_switching_again(self):
        self.command('/fallback fake/two-model/high')
        self.provider.runtime_handler = lambda handler, request, sequence: self.deny(handler, sequence)
        os.write(self.owner.initial.master, b'failed fallback fixture\r')
        self.owner.wait_event('turn_failed')
        self.assertEqual(self.models(), ['host-model', 'two-model'])
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'turn_fallback_started']), 1)

    def test_other_policy_errors_do_not_trigger_fallback(self):
        self.command('/fallback fake/two-model/high')
        self.provider.runtime_handler = lambda handler, request, sequence: self.deny(
            handler, sequence, code='content_filter')
        os.write(self.owner.initial.master, b'other error fixture\r')
        self.owner.wait_event('turn_failed')
        self.assertEqual(self.models(), ['host-model'])

    def test_exhausted_yellow_clarifications_use_fallback_once(self):
        self.command('/fallback fake/two-model/high')

        def respond(handler, request, sequence):
            if request['model'] == 'host-model':
                return self.deny(handler, sequence, yellow=True)
            self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'exhausted clarification fixture\r')
        self.completed()
        self.assertEqual(self.models(), ['host-model'] * 6 + ['two-model'])
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'response_output_correction']), 5)

    def resume_fallback(self):
        self.command('/fallback fake/two-model/high:100000')

        def respond(handler, request, sequence):
            if sequence == 1:
                return self.deny(handler, sequence)
            self.entered.set()
            self.release.wait(10)
            try:
                self.provider.reply(handler, self.provider.response_body(sequence, 'old').encode(),
                                    close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'resume fallback fixture\r')
        self.assertTrue(self.entered.wait(5))
        self.assertEqual(self.models(), ['host-model', 'two-model'])
        # Exit retains the unfinished turn, unlike cancelling it with Ctrl-C.
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        self.release.set()
        self.provider.runtime_handler = lambda handler, request, sequence: self.provider.reply(
            handler, self.provider.response_body(sequence, 'resumed-fallback').encode(),
            close_header=True)
        resumed = self.owner.start(['--resume', self.owner.sid])
        self.addCleanup(lambda: self.owner.finish(resumed, b'/exit')
                        if resumed.process.poll() is None else None)
        resumed.until(b'resumed-fallback')
        self.completed()
        self.assertEqual(self.models(), ['host-model', 'two-model', 'two-model'])
        os.write(resumed.master, b'next primary turn\r')
        self.completed(2)
        self.assertEqual(self.models()[-1], 'host-model')
        self.owner.finish(resumed, b'/exit')

    def test_unfinished_fallback_survives_native_checkpoint_resume(self):
        self.resume_fallback()

    def test_unfinished_fallback_survives_resume_legacy(self):
        self.resume_fallback()

    def test_off_during_fallback_retains_serving_turn_and_disables_next_switch(self):
        self.command('/fallback fake/two-model/high')

        def respond(handler, request, sequence):
            if sequence == 1 or sequence == 3:
                return self.deny(handler, sequence)
            self.entered.set()
            self.release.wait(10)
            self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'off during fallback fixture\r')
        self.assertTrue(self.entered.wait(5))
        self.command('/fallback off save', 'configuration saved:')
        self.assertIn('fallback_model = off', self.owner.config.read_text())
        self.release.set()
        self.completed()
        os.write(self.owner.initial.master, b'next turn with fallback off\r')
        self.owner.wait_event('turn_failed')
        self.assertEqual(self.models(), ['host-model', 'two-model', 'host-model'])

    def test_cancellation_restores_primary_and_next_turn_can_fallback(self):
        self.command('/fallback fake/two-model/high')

        def respond(handler, request, sequence):
            if request['model'] == 'host-model':
                return self.deny(handler, sequence)
            if sequence == 2:
                self.entered.set()
                self.release.wait(10)
            try:
                self.provider.reply(handler, self.provider.response_body(sequence, 'done').encode(),
                                close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'cancel fallback fixture\r')
        self.assertTrue(self.entered.wait(5))
        os.write(self.owner.initial.master, b'\x03')
        self.owner.wait_event('turn_interrupted')
        self.release.set()
        os.write(self.owner.initial.master, b'new turn after cancel\r')
        self.completed()
        self.assertEqual(self.models(), ['host-model', 'two-model', 'host-model', 'two-model'])

    def test_save_off_resume_and_invalid_selector(self):
        before = self.owner.config.read_text()
        self.command('/fallback fake/two-model/high:0 save', '4000000000')
        self.assertEqual(self.owner.config.read_text(), before)
        self.command('/fallback fake/two-model/high:100000 s', 'configuration saved:')
        saved = self.owner.config.read_text()
        self.assertIn('fallback_model = fake/"two-model"/"high":100000', saved)
        self.assertIn('model = host-model', saved)
        self.command('/fallback off')
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until('›'.encode())
        os.write(resumed.master, b'/fallback\r')
        resumed.until(b'Fallback: off')
        self.owner.finish(resumed, b'/exit')
        fresh = self.owner.start([])
        fresh.until('›'.encode())
        os.write(fresh.master, b'/fallback\r')
        fresh.until(b'Fallback: fake/"two-model"/"high":100000')
        self.owner.finish(fresh, b'/exit')
        self.assertEqual(self.models(), [])


if __name__ == '__main__':
    unittest.main()
