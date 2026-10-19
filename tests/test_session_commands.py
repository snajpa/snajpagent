#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Semantic command receipts, immutable reports and terminal handoff admission."""

import hashlib
import os
import subprocess
import threading
import time
import unittest
import uuid

import test_session_view as owners


class SendTests(unittest.TestCase):
    def setUp(self):
        self.owner = owners.SessionViewTests()
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)

    def send(self, *words, data=None, prefix=None):
        command = [str(owners.BINARY), 'send', '--dotdir', str(self.owner.root / 'state'),
                   prefix or self.owner.sid[:8]]
        if words:
            command += ['--', *words]
        return subprocess.run(command, input=data, capture_output=True, timeout=35,
                              cwd=self.owner.root, env={**os.environ, 'HOME': str(self.owner.root)})

    def test_send_keeps_attached_terminal_and_its_partial_draft(self):
        os.write(self.owner.initial.master, b'unsent terminal draft')
        self.owner.initial.until(b'unsent terminal draft')
        result = self.send('sideband', 'prompt')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'accepted '), result.stdout)
        completed = self.owner.wait_event('turn_completed')['seq']
        self.owner.status('attached')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        os.write(self.owner.initial.master, b' plus suffix\r')
        self.owner.wait_event('turn_completed', completed)
        self.assertEqual([e['data']['text'] for e in self.owner.events()
                          if e['type'] == 'input_received'],
                         ['sideband prompt', 'unsent terminal draft plus suffix'])

    def test_send_unbound_receipts_deduplicate_and_do_not_claim_control(self):
        peer = self.owner.view()
        self.assertIn('send', peer.capabilities['features'])
        request = uuid.uuid4().hex
        peer.send(type='send', id=request, text='independent semantic input')
        result = peer.result(request)
        self.assertEqual(result['status'], 'committed', result)
        self.assertEqual(result['draft_cleared'], 0)
        self.owner.wait_event('turn_completed')
        peer.send(type='send', id=request, text='independent semantic input')
        self.assertEqual(peer.result(request), result)
        peer.send(type='send', id=request, text='changed payload')
        refused = peer.until('error')
        self.assertEqual(refused['id'], request)
        self.assertIn('already used', refused['message'])
        peer.submit('independent semantic input', request)
        self.assertIn('generation', peer.until('error')['message'])
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'input_received']), 1)
        self.owner.status('attached')

    def test_send_none_discards_and_leaves_controller_draft_unchanged(self):
        self.owner.detach()
        controller = self.owner.view(bind=True)
        self.assertEqual(controller.result(controller.command('/model none'))['outcome'], 'ok')
        draft = controller.replace_draft(controller.draft()['revision'], 'keep this draft')['draft']
        before = self.owner.events()[-1]['seq']
        result = self.send(data='discard from stdin 👩‍💻\r\n'.encode())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'discarded '), result.stdout)
        self.assertEqual(controller.draft(), draft)
        self.assertFalse(any(e['type'] in ('input_received', 'steering_added',
            'future_turn_queued', 'response_started') for e in self.owner.events()
            if e['seq'] > before))
        self.assertEqual(controller.result(controller.command('/model host-model'))['outcome'], 'ok')
        result = self.send(data='new stdin input 👩‍💻\r\n'.encode())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.owner.wait_event('turn_completed', before)
        inputs = [e['data']['text'] for e in self.owner.events() if e['type'] == 'input_received']
        self.assertEqual(inputs, ['new stdin input 👩‍💻'])
        self.assertEqual(controller.draft(), draft)

    def test_send_during_active_request_reports_durable_steering(self):
        started = threading.Event()
        def held(handler, request, sequence):
            started.set()
            self.owner.release.wait(10)
            self.owner.provider.reply(handler, self.owner.provider.response_body(
                sequence, 'released response').encode(), close_header=True)
        self.owner.provider.runtime_handler = held
        self.assertEqual(self.send('start held request').returncode, 0)
        self.assertTrue(started.wait(5))
        result = self.send('new steering input')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'queued '), result.stdout)
        self.assertIn(b'event=steering_added', result.stdout)
        self.assertEqual(self.owner.wait_event('steering_added')['data']['text'],
                         'new steering input')
        self.owner.release.set()
        self.owner.wait_event('turn_completed')

    def test_send_large_unicode_stdin_and_validation_boundaries(self):
        text = 'large input 👩‍💻é\\\t' * 9000
        result = self.send(data=(text + '\n').encode())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.owner.wait_event('input_received')['data']['text'], text)
        # Drain the real terminal's large prompt echo while the owner runs.
        self.owner.initial.until(b'semantic-answer', timeout=15)
        self.owner.wait_event('turn_completed')
        before = self.owner.journal.read_bytes()
        for data in (b'', b'\xff', b'x\0y', b'x' * (1024 * 1024 + 1)):
            with self.subTest(data_length=len(data)):
                result = self.send(data=data)
                self.assertEqual(result.returncode, 2, result.stderr)
        result = self.send('/exit')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'command capability', result.stderr)
        self.assertEqual(self.owner.journal.read_bytes(), before)
        self.owner.finish(self.owner.initial, b'/exit')
        before = self.owner.journal.read_bytes()
        result = self.send('cannot restart stopped owner')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.owner.journal.read_bytes(), before)

    def test_send_old_owner_and_lost_receipt_have_truthful_outcomes(self):
        self.owner.finish(self.owner.initial, b'/exit')
        path = self.owner.directory / 'view.sock'
        self.assertFalse(path.exists())
        for outcome in ('unsupported', 'lost', 'unknown'):
            supported = outcome != 'unsupported'
            listener = owners.socket.socket(owners.socket.AF_UNIX)
            previous = os.getcwd()
            try:
                os.chdir(self.owner.directory)
                listener.bind(path.name)
            finally:
                os.chdir(previous)
            path.chmod(0o600)
            listener.listen(1)
            listener.settimeout(10)
            received = []
            failures = []
            def server():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(5)
                        peer = object.__new__(owners.View)
                        peer.peer, peer.states = connection, []
                        received.append(peer.receive())
                        peer.send(type='capabilities', session=self.owner.sid, version=1,
                                  instance='0' * 32, features=['send'] if supported else [])
                        if supported:
                            request = peer.receive()
                            received.append(request)
                            peer.send(type='result', id=request['id'],
                                      status='unknown' if outcome == 'unknown' else 'pending')
                except Exception as error:
                    failures.append(error)
            worker = threading.Thread(target=server)
            worker.start()
            try:
                result = self.send('receipt boundary prompt')
            finally:
                listener.close()
                worker.join(10)
                path.unlink()
            self.assertFalse(worker.is_alive())
            self.assertFalse(failures, failures)
            self.assertEqual(len(received), 2 if supported else 1)
            self.assertEqual(result.returncode, 3 if supported else 1, result.stderr)
            self.assertEqual(result.stdout, b'')
            self.assertIn(b'outcome unknown' if supported else b'does not support send',
                          result.stderr)
            if supported:
                self.assertIn(received[1]['id'].encode(), result.stderr)


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.owner = owners.SessionViewTests()
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)
        self.owner.detach()
        self.peer = self.owner.view(bind=True)

    def report(self, result):
        self.assertEqual(result['status'], 'completed', result)
        self.assertEqual(result['report_error'], '')
        report = result['report']
        path = self.owner.directory / ('.view-report-' + report['id'])
        data = path.read_bytes()
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(len(data), report['bytes'])
        self.assertEqual(hashlib.sha256(data).hexdigest(), report['sha256'])
        self.assertTrue(data.startswith(report['command'].expandtabs(4).encode() + b'\n'))
        return data

    def command(self, text, **options):
        request = self.peer.command(text, **options)
        return self.peer.result(request)

    def test_model_none_discards_input_and_reenables_only_new_input(self):
        seen = []
        def reply(handler, request, sequence):
            seen.append(request)
            self.owner.provider.reply(handler, self.owner.provider.response_body(
                sequence, 'enabled answer').encode(), close_header=True)
        self.owner.provider.runtime_handler = reply
        self.assertEqual(self.command('/model none')['outcome'], 'ok')
        self.assertIn(b'model: none', self.report(self.command('/status')))
        before = self.owner.events()[-1]['seq']
        for text in ('discard ordinary', '/ro discard readonly'):
            request = self.peer.submit(text)
            result = self.peer.result(request)
            self.assertEqual(result['status'], 'discarded', result)
            self.peer.submit(text, request)
            self.assertEqual(self.peer.result(request), result)
        self.assertEqual(self.command('/compact')['outcome'], 'error')
        self.assertFalse(seen)
        self.assertFalse(any(e['type'] in ('input_received', 'steering_added',
            'future_turn_queued', 'response_started', 'compact_started')
            for e in self.owner.events() if e['seq'] > before))
        self.assertEqual(self.command('/model host-model')['outcome'], 'ok')
        self.assertEqual(self.peer.result(self.peer.submit('new enabled input'))['status'],
                         'committed')
        self.owner.wait_event('turn_completed', before)
        self.assertTrue(seen)
        wire = str(seen)
        self.assertIn('new enabled input', wire)
        self.assertNotIn('discard ordinary', wire)
        self.assertNotIn('discard readonly', wire)
        self.assertNotIn('discard queued', wire)

    def test_model_none_cancels_an_active_request_before_another_request(self):
        import threading
        started = threading.Event()
        seen = []
        def held(handler, request, sequence):
            seen.append(request)
            started.set()
            self.owner.release.wait(10)
            try:
                self.owner.provider.reply(handler, self.owner.provider.response_body(
                    sequence, 'late answer').encode(), close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass
        self.owner.provider.runtime_handler = held
        self.assertEqual(self.peer.result(self.peer.submit('held request'))['status'], 'committed')
        self.assertTrue(started.wait(5))
        result = self.command('/model none')
        self.assertEqual(result['outcome'], 'ok', result)
        self.owner.wait_event('turn_interrupted')
        self.assertEqual(self.peer.result(self.peer.submit('discard after cancel'))['status'],
                         'discarded')
        self.owner.release.set()
        self.assertEqual(len(seen), 1)
        self.assertIn(b'model: none', self.report(self.command('/status')))

    def test_status_and_help_are_operator_only_immutable_reports(self):
        original = self.owner.journal.read_bytes()
        self.assertIn('commands', self.peer.capabilities['features'])
        status = self.command('/status')
        data = self.report(status)
        self.assertIn(self.owner.sid.encode(), data)
        self.assertEqual(status['outcome'], 'ok')
        help_result = self.command('/help')
        self.assertIn(b'/configure', self.report(help_result))
        self.assertNotEqual(status['report']['id'], help_result['report']['id'])
        self.assertEqual(self.report(status), data)
        self.assertEqual(self.owner.journal.read_bytes(), original)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_retry_auto_uses_owner_policy_and_keeps_receipts(self):
        result = self.command('/retry auto off')
        self.assertIn(b'Automatic retry: OFF', self.report(result))
        self.assertEqual(result['outcome'], 'ok')
        self.assertIn(b'automatic retry: OFF (session override)',
                      self.report(self.command('/status')))
        values = [e['data']['value'] for e in self.owner.events()
                  if e['type'] == 'retry_auto_changed']
        self.assertEqual(values, ['off'])
        self.assertIn(b'Automatic retry: ON', self.report(self.command('/retry auto on')))

    def test_command_chain_orders_effects_and_reconciles_one_receipt(self):
        text = '/fast on \t;\t retry auto off ; /status ; fast off ; '
        request = self.peer.command(text)
        result = self.peer.result(request)
        self.assertEqual(result['outcome'], 'ok')
        data = self.report(result)
        self.assertEqual(result['report']['command'], text)
        self.assertEqual(data.count(text.expandtabs(4).encode()), 1)
        self.assertLess(data.index(b'Fast mode: ON'), data.index(b'Automatic retry: OFF'))
        self.assertLess(data.index(b'Automatic retry: OFF'), data.index(b'session:'))
        self.assertLess(data.index(b'session:'), data.index(b'Fast mode: OFF'))
        effects = [e['data']['value'] for e in self.owner.events()
                   if e['type'] == 'service_tier_changed']
        self.assertEqual(effects, ['priority', 'default'])
        self.peer.command(text, request)
        self.assertEqual(self.peer.result(request), result)
        self.assertEqual([e['data']['value'] for e in self.owner.events()
                          if e['type'] == 'service_tier_changed'], effects)
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))

    def test_command_chain_stops_after_usage_or_unknown_command_error(self):
        for failed in ('fast invalid', 'no-such-command'):
            with self.subTest(failed=failed):
                self.command('/fast off')
                result = self.command('/fast on; ' + failed + '; /fast off')
                self.assertEqual(result['outcome'], 'error')
                data = self.report(result)
                self.assertIn(b'Fast mode: ON', data)
                self.assertNotIn(b'Fast mode: OFF', data)
                self.assertIn(b'fast: ON', self.report(self.command('/status')))

    def test_command_chain_preserves_quoted_and_escaped_semicolons(self):
        for command, expected in [('/banner "quote; body"; /banner', '"quote; body"'),
                                  (r'/banner escaped\;body; /banner', 'escaped;body')]:
            with self.subTest(command=command):
                result = self.command(command)
                self.assertEqual(result['outcome'], 'ok')
                self.assertIn(expected.encode(), self.report(result))
                values = [e['data']['text'] for e in self.owner.events()
                          if e['type'] == 'banner_updated']
                self.assertEqual(values[-1], expected)
        for command in ('/fast on; /banner "unterminated; /fast off',
                        '/fast on; /banner trailing\\'):
            self.command('/fast off')
            result = self.command(command)
            self.assertEqual(result['outcome'], 'error')
            self.assertNotIn(b'Fast mode: ON', self.report(result))

    def test_command_chain_uses_and_updates_the_pane_verbosity(self):
        result = self.command('/fast on; verbose 2; /verbose; /status', verbosity=3)
        self.assertEqual(result['outcome'], 'ok')
        self.assertEqual(result['verbosity'], 2)
        self.assertEqual(self.report(result).count(b'verbosity: 2'), 2)
        self.assertIn(b'verbosity: 0', self.report(self.command('/verbose')))
        failed = self.command('/verbose 1; fast invalid; verbose 4', verbosity=2)
        self.assertEqual(failed['outcome'], 'error')
        self.assertEqual(failed['verbosity'], 1)
        self.assertNotIn(b'verbosity: 4', self.report(failed))

    def test_command_chain_retains_each_raw_file_and_the_remaining_report(self):
        paths = [self.owner.root / ('file-' + str(i)) for i in range(2)]
        for i, path in enumerate(paths):
            path.write_bytes(('raw-file-' + str(i)).encode())
        result = self.command('/cat ' + str(paths[0]) + '; cat ' + str(paths[1]) + '; fast on')
        self.assertEqual(result['outcome'], 'ok')
        self.assertIn(b'Fast mode: ON', self.report(result))
        raw = [p.read_bytes() for p in self.owner.directory.glob('.view-report-*')]
        for i in range(2):
            self.assertEqual(sum(data.endswith(('\nraw-file-' + str(i)).encode())
                                 for data in raw), 1)

    def test_terminal_chain_hands_off_before_any_effect(self):
        result = self.command('/fast on; config; fast off')
        self.assertEqual(result['status'], 'terminal')
        self.assertFalse(any(e['type'] == 'service_tier_changed' for e in self.owner.events()))

    def test_command_chain_accepts_leading_whitespace_and_empty_segments(self):
        result = self.command(' \t/fast on; ;\t; /fast off; ')
        self.assertEqual(result['outcome'], 'ok')
        self.assertIn(b'Fast mode: OFF', self.report(result))

    def test_plain_prompt_semicolons_remain_literal(self):
        text = 'ordinary prompt; /fast on; status'
        result = self.peer.result(self.peer.submit(text))
        self.assertEqual(result['status'], 'committed')
        self.owner.wait_event('turn_completed')
        self.assertEqual([e['data']['text'] for e in self.owner.events()
                          if e['type'] == 'input_received'], [text])
        self.assertFalse(any(e['type'] == 'service_tier_changed' for e in self.owner.events()))

    def test_fast_duplicate_reconnect_and_changed_id_text(self):
        request = self.peer.command('/fast on')
        result = self.peer.result(request)
        self.assertIn(b'ON', self.report(result))
        self.peer.until('state', lambda message: message['state']['service_tier'] == 'priority')
        events = [e for e in self.owner.events() if e['type'] == 'service_tier_changed']
        self.assertEqual(len(events), 1)
        self.assertEqual(result['seq'], events[0]['seq'])
        self.peer.command('/fast on', request)
        self.assertEqual(self.peer.result(request), result)
        self.peer.command('/fast off', request)
        self.assertEqual(self.peer.until('error')['id'], request)
        self.peer.close()
        self.owner.status('detached')
        self.peer = self.owner.view(bind=True)
        self.peer.send(type='receipt', id=request)
        self.assertEqual(self.peer.result(request), result)
        self.assertEqual(self.report(result).count(b'/fast'), 1)
        self.assertIn(b'OFF', self.report(self.command('/fast off')))
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'service_tier_changed']), 2)
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))

    def test_lost_command_acknowledgement_is_queried_without_reexecution(self):
        request = self.peer.command('/fast on')
        self.peer.until('result', lambda result: result['id'] == request)
        self.peer.close()
        self.owner.status('detached')
        self.peer = self.owner.view()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.peer.send(type='receipt', id=request)
            result = self.peer.until('result', lambda result: result['id'] == request)
            if result['status'] != 'pending':
                break
            time.sleep(.02)
        self.assertIn(result['status'], ('completed', 'rejected'))
        effects = [e for e in self.owner.events() if e['type'] == 'service_tier_changed']
        self.assertEqual(len(effects), int(result['status'] == 'completed'))
        if effects:
            self.assertIn(b'ON', self.report(result))
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_errors_are_completed_reports_and_do_not_replay(self):
        request = self.peer.command('/fast perhaps')
        result = self.peer.result(request)
        self.assertEqual(result['outcome'], 'error')
        self.assertIn(b'usage:', self.report(result))
        self.peer.command('/fast perhaps', request)
        self.assertEqual(self.peer.result(request), result)
        self.assertFalse(any(e['type'] == 'service_tier_changed' for e in self.owner.events()))
        self.assertEqual(len(list(self.owner.directory.glob('.view-report-*'))), 1)

    def test_command_revision_scope_and_terminal_requests_have_no_effect(self):
        first = self.peer.draft()
        changed = self.peer.replace_draft(first['revision'], '/status')['draft']
        refused = self.peer.command('/status', draft_revision=first['revision'])
        self.assertEqual(self.peer.until('error')['id'], refused)
        result = self.command('/status', draft_revision=changed['revision'])
        self.report(result)
        empty = self.peer.draft()
        self.assertEqual(empty['text'], '')
        self.assertEqual(empty['revision'], result['draft_cleared'])
        before = self.owner.journal.read_bytes()
        for command in ('/config', '/delete', '/send some-file', '/s d', '/1 hello'):
            result = self.command(command)
            self.assertEqual(result['status'], 'terminal', result)
            self.assertNotIn('report', result)
            self.owner.status('attached')
        self.report(self.command('/chat'))
        self.assertEqual(self.owner.journal.read_bytes(), before)
        for route in ('irc', None, {'bad': True}):
            request = uuid.uuid4().hex
            self.peer.send(type='command', generation=self.peer.generation,
                           id=request, route=route, text='/fast')
            self.assertEqual(self.peer.until('error')['id'], request)
        request = uuid.uuid4().hex
        self.peer.send(type='command', generation=self.peer.generation + 1,
                       id=request, route='rollout', text='/fast')
        self.assertEqual(self.peer.until('error')['id'], request)
        self.assertEqual(self.owner.journal.read_bytes(), before)

    def test_fast_during_a_provider_wait_does_not_cancel_or_steer(self):
        started = threading.Event()
        def held(handler, request, sequence):
            started.set()
            self.owner.release.wait(10)
            self.owner.provider.reply(handler, self.owner.provider.response_body(
                sequence, 'completed after command').encode(), close_header=True)
        self.owner.provider.runtime_handler = held
        self.assertEqual(self.peer.result(self.peer.submit('held turn'))['status'], 'committed')
        self.assertTrue(started.wait(5))
        active_chain = self.report(self.command('/fast on; retry auto off; status'))
        self.assertIn(b'Fast mode: ON', active_chain)
        self.assertIn(b'Automatic retry: OFF', active_chain)
        self.assertIn(b'session:', active_chain)
        self.assertIn(b'/status', self.report(self.command('/status')))
        self.assertFalse(any(e['type'] in ('turn_interrupted', 'steering_added',
                                          'turn_cancel_requested') for e in self.owner.events()))
        self.owner.release.set()
        self.owner.wait_event('turn_completed')
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_report_survives_owner_shutdown_and_receipt_does_not_cross_instances(self):
        result = self.command('/status')
        data = self.report(result)
        self.peer.send(type='quit', generation=self.peer.generation)
        self.peer.until('control')
        self.owner.status('stored')
        self.assertEqual(self.report(result), data)
        child = self.owner.start(['--resume', self.owner.sid])
        child.until('›'.encode(), 10)
        # The resumed owner is independent of its launching frontend.
        for row in subprocess.check_output(['ps', '-axo', 'pid=,ppid=,command='],
                                           text=True).splitlines():
            fields = row.split(None, 2)
            if len(fields) == 3 and fields[1] == str(child.process.pid):
                self.owner.owner = int(fields[0])
                self.owner.owner_identity = self.owner.identity()
                break
        else:
            self.fail('resumed fixture owner not found')
        self.owner.finish(child, b'/s d')
        other = self.owner.view(bind=True)
        other.send(type='receipt', id=result['id'])
        self.assertEqual(other.result(result['id'])['status'], 'unknown')
        self.assertNotEqual(other.capabilities['instance'], self.peer.capabilities['instance'])
        self.assertEqual(self.report(result), data)
        other.send(type='quit', generation=other.generation)
        other.until('control')
        self.owner.status('stored')

    def test_generated_reports_follow_explicit_session_deletion(self):
        result = self.command('/status')
        self.report(result)
        self.peer.close()
        self.owner.status('detached')
        child = self.owner.start(['--resume', self.owner.sid])
        child.until(b'Attached session', 10)
        os.write(child.master, b'/delete\r')
        child.until(b'8-character id prefix to confirm', 10)
        self.owner.finish(child, self.owner.sid[:8].encode())
        self.assertFalse(self.owner.directory.exists())
        self.assertFalse(list((self.owner.root / 'state' / 'trash').iterdir()))

    def test_verbosity_and_deferred_reload_have_command_echo_and_truthful_receipts(self):
        report = self.report(self.command('/verbose 2'))
        self.assertIn(b'verbosity: 2', report)
        self.assertEqual(report.count(b'/verbose 2'), 1)
        result = self.command('/configure')
        self.assertIn(b'accepted; applying at the next safe request boundary', self.report(result))
        self.assertEqual(result['outcome'], 'ok')
        self.assertEqual(next(e for e in self.owner.events() if e['seq'] == result['seq'])['type'],
                         'control_requested')
        self.owner.wait_event('control_finished')
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))

    def test_pane_verbosity_context_is_receipted_and_does_not_mutate_owner(self):
        self.assertIn('command_verbosity', self.peer.capabilities['features'])
        result = self.command('/verbose', verbosity=3)
        self.assertEqual(result['verbosity'], 3)
        self.assertIn(b'verbosity: 3', self.report(result))
        changed = self.command('/verbose 2', verbosity=3)
        self.assertEqual(changed['verbosity'], 2)
        self.assertIn(b'verbosity: 2', self.report(changed))
        self.assertIn(b'verbosity: 0', self.report(self.command('/verbose')))
        invalid = self.command('/verbose 7', verbosity=3)
        self.assertEqual(invalid['outcome'], 'error')
        self.assertNotIn('verbosity', invalid)
        self.assertIn(b'expects one integer', self.report(invalid))
        request = self.peer.command('/verbose', verbosity=1)
        receipt = self.peer.result(request)
        self.peer.command('/verbose', request, verbosity=1)
        self.assertEqual(self.peer.result(request), receipt)
        self.peer.command('/verbose', request, verbosity=2)
        self.assertEqual(self.peer.until('error')['id'], request)
        for text, level in [('/verbose', 7), ('/fast', 2), ('/verbose', -1)]:
            request = self.peer.command(text, verbosity=level)
            self.assertEqual(self.peer.until('error')['id'], request)
        self.assertEqual(self.owner.provider.requests, [])

    def test_history_report_exceeds_transport_frame(self):
        # A generated report spans many transport frames without putting its
        # contents in the receipt. The existing /history scan policy still applies.
        answer = 'retained-command-history ' * 36000
        self.owner.provider.runtime_handler = lambda handler, request, sequence: (
            self.owner.provider.reply(handler, self.owner.provider.response_body(
                sequence, answer).encode(), close_header=True))
        for number in range(1):
            request = self.peer.submit('history turn ' + str(number))
            self.assertEqual(self.peer.result(request)['status'], 'committed')
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if len([e for e in self.owner.events() if e['type'] == 'turn_completed']) > number:
                    break
                time.sleep(.02)
            else:
                self.fail('large history turn did not complete')
        result = self.command('/history 1')
        data = self.report(result)
        self.assertGreater(len(data), 16384)
        self.assertEqual(data.count(b'retained-command-history'), 36000)
        self.assertEqual(len([e for e in self.owner.events() if e['type'] == 'input_received']), 1)


if __name__ == '__main__':
    unittest.main()
