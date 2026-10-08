# SPDX-License-Identifier: GPL-2.0-only
"""Private report catalogue survives owner loss and remains presentation-only."""

import json
import os
import unittest

import test_vm_notifications as notifications


class CatalogTests(unittest.TestCase):
    setUp = notifications.NotificationTests.setUp

    def command(self, text):
        return self.peer.result(self.peer.command(text))

    def test_catalogue_contains_ordered_private_report_references(self):
        before = self.owner.journal.read_bytes()
        reports = [self.command(command)['report'] for command in ('/status', '/help')]
        path = self.owner.directory / '.view-reports.jsonl'
        self.assertTrue(path.exists(), 'reports need a durable discovery catalogue')
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual([json.loads(line) for line in path.read_bytes().splitlines()], reports)
        self.assertEqual(self.owner.journal.read_bytes(), before)

    def test_partial_append_recovers_at_the_previous_complete_reference(self):
        first = self.command('/status')['report']
        path = self.owner.directory / '.view-reports.jsonl'
        original = path.read_bytes()
        with path.open('ab') as stream:
            stream.write(b'{"id":"unfinished')
        second = self.command('/help')['report']
        self.assertTrue(path.read_bytes().startswith(original))
        self.assertEqual([json.loads(line) for line in path.read_bytes().splitlines()],
                         [first, second])

    def test_catalogue_failure_does_not_repeat_command_effect(self):
        path = self.owner.directory / '.view-reports.jsonl'
        os.symlink('events.jsonl', path)
        request = self.peer.command('/fast')
        result = self.peer.result(request)
        self.assertEqual(result['status'], 'completed')
        self.assertEqual(result['outcome'], 'ok')
        self.assertIsNone(result['report'])
        self.assertIn('cannot retain', result['report_error'])
        self.peer.command('/fast', request)
        self.assertEqual(self.peer.result(request), result)
        self.assertEqual(len([e for e in self.owner.events()
                              if e['type'] == 'service_tier_changed']), 1)
        self.assertEqual(len(list(self.owner.directory.glob('.view-report-*'))), 1)


class WorkspaceCatalogTests(unittest.TestCase):
    setUp = notifications.WorkspaceNotificationTests.setUp
    start = notifications.WorkspaceNotificationTests.start
    snapshots = notifications.WorkspaceNotificationTests.snapshots
    wait_snapshot = notifications.WorkspaceNotificationTests.wait_snapshot
    reports = notifications.WorkspaceNotificationTests.reports

    def stopped_reports(self):
        peer = self.owner.view(bind=True)
        reports = [peer.result(peer.command(command))['report']
                   for command in ('/status', '/help')]
        peer.send(type='quit', generation=peer.generation)
        peer.until('control')
        self.owner.status('stored')
        return reports

    def test_fresh_workspace_discovers_reports_after_owner_stops(self):
        reports = self.stopped_reports()
        before = self.owner.journal.read_bytes()
        child = self.start('-N', 'discovery', columns=120)
        child.command('history ' + self.owner.sid)
        child.repaint_until(('[' + self.owner.sid[:8] + '; stored; :session]').encode())
        child.command('reports')
        child.repaint_until(b'/status')
        child.repaint_until(b'/help')
        self.assertEqual(self.reports(2), reports)
        child.write(b'G\r')
        child.repaint_until(('REPORT ' + self.owner.sid[:8] + ' ' + reports[-1]['id'][:8]).encode())
        child.repaint_until(b'Help and settings')
        child.finish('close')
        self.owner.status('stored')
        self.assertEqual(self.owner.journal.read_bytes(), before)

    def test_direct_report_discovery_and_corrupt_refresh_preserve_loaded_view(self):
        reports = self.stopped_reports()
        child = self.start('-N', 'direct-discovery', columns=120)
        child.command('history ' + self.owner.sid)
        child.repaint_until(('[' + self.owner.sid[:8] + '; stored; :session]').encode())
        child.command('report')
        child.repaint_until(('REPORT ' + self.owner.sid[:8] + ' ' + reports[-1]['id'][:8]).encode())
        self.assertEqual(self.reports(2), reports)
        path = self.owner.directory / '.view-reports.jsonl'
        original = path.read_bytes()
        path.write_bytes(original + b'{"bad":true}\n')
        child.command('report ' + reports[0]['id'])
        child.repaint_until(b'cannot read report catalogue')
        child.repaint_until(('REPORT ' + self.owner.sid[:8] + ' ' + reports[-1]['id'][:8]).encode())
        self.assertEqual(self.reports(2), reports)
        path.write_bytes(original + b'{"unfinished":')
        child.command('reports')
        child.repaint_until(b'incomplete final catalogue entry')
        self.assertEqual(self.reports(2), reports)
        child.finish('close')


if __name__ == '__main__':
    unittest.main()
