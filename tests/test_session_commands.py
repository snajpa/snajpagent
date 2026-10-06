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
        self.assertTrue(data.startswith(report['command'].encode() + b'\n'))
        return data

    def command(self, text, **options):
        request = self.peer.command(text, **options)
        return self.peer.result(request)

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
        self.assertIn(b'Automatic retry: ON', self.report(self.command('/retry auto')))

    def test_fast_duplicate_reconnect_and_changed_id_text(self):
        request = self.peer.command('/fast')
        result = self.peer.result(request)
        self.assertIn(b'ON', self.report(result))
        self.peer.until('state', lambda message: message['state']['service_tier'] == 'priority')
        events = [e for e in self.owner.events() if e['type'] == 'service_tier_changed']
        self.assertEqual(len(events), 1)
        self.assertEqual(result['seq'], events[0]['seq'])
        self.peer.command('/fast', request)
        self.assertEqual(self.peer.result(request), result)
        self.peer.command('/fast off', request)
        self.assertEqual(self.peer.until('error')['id'], request)
        self.peer.close()
        self.owner.status('detached')
        self.peer = self.owner.view(bind=True)
        self.peer.send(type='receipt', id=request)
        self.assertEqual(self.peer.result(request), result)
        self.assertEqual(self.report(result).count(b'/fast'), 1)
        self.assertIn(b'OFF', self.report(self.command('/fast')))
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'service_tier_changed']), 2)
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))

    def test_lost_command_acknowledgement_is_queried_without_reexecution(self):
        request = self.peer.command('/fast')
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
        for command in ('/config', '/delete', '/send some-file', '/s d', '/chat', '/1 hello'):
            result = self.command(command)
            self.assertEqual(result['status'], 'terminal', result)
            self.assertNotIn('report', result)
            self.owner.status('attached')
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
        self.assertIn(b'ON', self.report(self.command('/fast')))
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
