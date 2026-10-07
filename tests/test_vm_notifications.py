# SPDX-License-Identifier: GPL-2.0-only
"""Deferred command completion and reconnect report replay, through real owners."""

import hashlib
import json
import os
import threading
import unittest

import test_vm_control as control


def subscribe(peer):
    peer.send(type='reports')
    reports = []
    while True:
        message = peer.receive()
        if message['type'] == 'reports_ready':
            return reports
        if message['type'] == 'report':
            reports.append(message)
        if message['type'] == 'error':
            raise AssertionError(message)


class NotificationTests(unittest.TestCase):
    def setUp(self):
        self.owner = control.owners.SessionViewTests()
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)
        self.owner.detach()
        self.peer = self.owner.view(bind=True)

    def contents(self, notification):
        self.assertEqual(notification['error'], '')
        report = notification['report']
        path = self.owner.directory / ('.view-report-' + report['id'])
        data = path.read_bytes()
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(len(data), report['bytes'])
        self.assertEqual(hashlib.sha256(data).hexdigest(), report['sha256'])
        self.assertTrue(data.startswith(report['command'].encode() + b'\n'))
        return data

    def test_configure_completion_replays_without_reexecution(self):
        self.assertEqual(subscribe(self.peer), [])
        request = self.peer.command('/configure')
        result = self.peer.result(request)
        initial = self.peer.until('report')
        completion = self.peer.until('report')
        self.assertEqual(initial['report'], result['report'])
        self.assertIn(b'accepted; applying', self.contents(initial))
        self.assertIn(b'configuration reloaded:', self.contents(completion))
        self.assertIn(b'Result: completed', self.contents(completion))
        self.assertEqual(completion['report']['command'], '/configure (completion)')
        self.peer.command('/configure', request)
        self.assertEqual(self.peer.result(request), result)
        self.peer.close()
        self.owner.status('detached')
        before = self.owner.journal.read_bytes()
        observer = self.owner.view()
        self.assertEqual(subscribe(observer), [initial, completion])
        self.owner.status('detached')
        self.assertEqual(self.owner.journal.read_bytes(), before)
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'control_finished']), 1)
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))

    def test_disconnected_completion_and_coalesced_requests(self):
        entered = threading.Event()
        original = self.owner.provider.handle_catalog
        def held(handler):
            entered.set()
            self.owner.release.wait(10)
            original(handler)
        self.owner.provider.handle_catalog = held
        first = self.peer.result(self.peer.command('/model cache'))
        self.assertTrue(entered.wait(5))
        second = self.peer.result(self.peer.command('/model cache'))
        status = self.peer.result(self.peer.command('/status'))
        self.assertNotEqual(first['report']['id'], second['report']['id'])
        self.peer.close()
        self.owner.status('detached')
        self.owner.release.set()
        self.owner.wait_event('control_finished')
        observer = self.owner.view()
        reports = subscribe(observer)
        self.assertEqual(len(reports), 4)
        self.assertEqual([e['report']['id'] for e in reports[:3]],
                         [first['report']['id'], second['report']['id'], status['report']['id']])
        self.assertIn(b'Result: completed', self.contents(reports[-1]))
        self.assertIn(b'cache updated:', self.contents(reports[-1]))
        self.assertNotIn(b'/status', self.contents(reports[-1]))
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'control_finished']), 1)
        self.owner.status('detached')

    def test_cache_failure_has_separate_completion_report(self):
        self.owner.provider.catalog_failure = '/v1/models'
        self.assertEqual(subscribe(self.peer), [])
        self.peer.result(self.peer.command('/model cache'))
        self.peer.until('report')
        completion = self.peer.until('report')
        self.assertEqual(completion['report']['command'], '/model cache (completion)')
        self.assertIn(b'catalog rejected', self.contents(completion))
        self.assertIn(b'Result: error', self.contents(completion))
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_classic_command_during_pending_report_keeps_its_output(self):
        entered = threading.Event()
        original = self.owner.provider.handle_catalog
        def held(handler):
            entered.set()
            self.owner.release.wait(10)
            original(handler)
        self.owner.provider.handle_catalog = held
        self.peer.result(self.peer.command('/model cache'))
        self.assertTrue(entered.wait(5))
        self.peer.close()
        self.owner.status('detached')
        child = self.owner.start(['--resume', self.owner.sid])
        child.until(b'host-model/medium', 5)
        self.owner.status('attached')
        os.write(child.master, b'/status\r')
        child.until(b'session:', 3)
        self.owner.release.set()
        self.owner.wait_event('control_finished')
        self.owner.finish(child, b'/s d')
        observer = self.owner.view()
        reports = subscribe(observer)
        self.assertEqual(len(reports), 2)
        self.assertIn(b'cache updated:', self.contents(reports[-1]))
        self.assertNotIn(b'/status', self.contents(reports[-1]))

    def test_subscription_is_opt_in_and_invalid_peer_is_isolated(self):
        observer = self.owner.view()
        observer.until('state')
        self.peer.result(self.peer.command('/status'))
        observer.send(type='receipt', id='0' * 32)
        self.assertEqual(observer.receive()['type'], 'result')
        invalid = self.owner.view()
        invalid.send(type='reports', generation=123)
        self.assertIn('fields', invalid.until('error')['message'])
        self.assertEqual(len(subscribe(observer)), 1)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def compact(self, outcome):
        self.peer.result(self.peer.submit('history to compact'))
        self.owner.wait_event('turn_completed')
        entered = threading.Event()
        def summary(handler, request, sequence):
            entered.set()
            if outcome == 'interrupted':
                self.owner.release.wait(10)
            if outcome == 'error':
                self.owner.provider.reply(handler, b'{"error":{"message":"summary unavailable"}}',
                                          'application/json', status=400, close_header=True)
            else:
                self.owner.provider.reply(handler, self.owner.provider.response_body(
                    sequence, 'retained compact summary').encode(), close_header=True)
        self.owner.provider.runtime_handler = summary
        self.peer.result(self.peer.command('/compact'))
        self.assertTrue(entered.wait(5))
        if outcome == 'interrupted':
            self.peer.send(type='cancel', generation=self.peer.generation)
            self.peer.until('control')
        self.owner.wait_event('control_finished')
        reports = subscribe(self.peer)
        self.assertEqual(len(reports), 2)
        text = self.contents(reports[-1])
        self.assertIn(b'/compact (completion)', text)
        self.assertIn(('Result: ' + outcome).encode(), text)
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'input_received']), 1)
        return text

    def test_compaction_completion(self):
        self.compact('completed')
        self.owner.wait_event('compaction_completed')

    def test_compaction_failure_keeps_previous_context_and_report(self):
        self.assertIn(b'previous context retained', self.compact('error'))

    def test_compaction_interruption_keeps_previous_context_and_report(self):
        self.assertIn(b'compaction interrupted', self.compact('interrupted'))


class WorkspaceNotificationTests(unittest.TestCase):
    start = control.ControlTests.start
    snapshots = control.ControlTests.snapshots
    wait_snapshot = control.ControlTests.wait_snapshot
    escape = control.ControlTests.escape
    wait_synced = control.ControlTests.wait_synced

    def setUp(self):
        control.ControlTests.setUp(self)
        config = self.root / 'state' / 'config.ini'
        config.write_text(self.owner.config.read_text().replace(
            '${SNAJPAGENT_IRC_UI_KEY}', '"irc-ui-secret"'))
        config.chmod(0o600)

    def reports(self, count):
        rows = self.wait_snapshot(lambda rows:
            len(next(iter(rows.values()))['state']['buffers'][0].get('reports', [])) == count)
        return next(iter(rows.values()))['state']['buffers'][0]['reports']

    def test_new_draft_and_selected_report_survive_notification_and_reconnect(self):
        child = self.start('-N', 'notifications', columns=120)
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(b'i/configure\rnewer unsent draft')
        reports = self.reports(2)
        self.wait_synced('newer unsent draft')
        self.escape(child)
        child.command('report ' + reports[0]['id'])
        child.repaint_until(b'accepted; applying')
        child.finish('close')
        resumed = self.start('--resume', 'notifications', expect=b'REPORT', columns=120)
        resumed.repaint_until(reports[0]['id'][:8].encode())
        self.assertEqual(self.reports(2), reports)
        resumed.command('history')
        resumed.repaint_until(b'newer unsent draft')
        resumed.command('report ' + reports[1]['id'])
        resumed.repaint_until(b'configuration reloaded:')
        resumed.finish('close')
        self.owner.status('detached')

    def test_reports_created_while_workspace_closed_are_discovered(self):
        child = self.start('-N', 'offline-notifications')
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.finish('close')
        peer = self.owner.view(bind=True)
        peer.result(peer.command('/configure'))
        self.owner.wait_event('control_finished')
        peer.close()
        self.owner.status('detached')
        resumed = self.start('--resume', 'offline-notifications', expect=b'history')
        self.reports(2)
        # Replay adds reports without changing the saved transcript window.
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        self.assertEqual(json.loads(path.read_text())['state']['windows'][0]['kind'], 'transcript')
        resumed.command('report')
        resumed.repaint_until(b'configuration reloaded:')
        resumed.finish('close')


if __name__ == '__main__':
    unittest.main()
