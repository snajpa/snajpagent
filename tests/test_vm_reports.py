# SPDX-License-Identifier: GPL-2.0-only
"""Native commands become retained operator-only Vim report buffers."""

import json
import unittest

import test_vm_control as control
from test_vm_frontend import rollout


class ReportTests(unittest.TestCase):
    start = control.ControlTests.start
    snapshots = control.ControlTests.snapshots
    wait_snapshot = control.ControlTests.wait_snapshot
    inputs = control.ControlTests.inputs
    escape = control.ControlTests.escape
    wait_synced = control.ControlTests.wait_synced

    def setUp(self):
        control.ControlTests.setUp(self)
        (self.root / 'state' / 'config.ini').write_text(self.owner.config.read_text().replace(
            '${SNAJPAGENT_IRC_UI_KEY}', '"irc-ui-secret"'))
        (self.root / 'state' / 'config.ini').chmod(0o600)

    def attached(self, name):
        child = self.start('-N', name, columns=120)
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        return child

    def reports(self, count):
        rows = self.wait_snapshot(lambda rows:
            len(next(iter(rows.values()))['state']['buffers'][0].get('reports', [])) == count)
        return next(iter(rows.values()))['state']['buffers'][0]['reports']

    def test_commands_reports_splits_and_fast_status(self):
        child = self.attached('commands')
        before = self.owner.journal.read_bytes()
        child.write(b'i/status\r')
        child.repaint_until(b'REPORT')
        child.repaint_until(b'/status')
        first, = self.reports(1)
        child.write(b'G')
        rows = self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['windows'][0]['row'] > 0)
        self.assertEqual(next(iter(rows.values()))['state']['windows'][0]['kind'], 'report')
        child.command('vsp')
        child.write(b'gg')
        child.repaint_until(b'/status')
        child.command('q')
        self.owner.status('attached')
        child.command('history')
        child.repaint_until(b'ATTACHED')
        self.assertEqual(self.owner.journal.read_bytes(), before)
        child.write(b'i/fast\r')
        child.repaint_until(b'ON')
        self.reports(2)
        child.command('history')
        child.repaint_until(b'FAST')
        child.command('reports')
        child.repaint_until(b'/status')
        child.repaint_until(b'/fast')
        child.write(b'gg\r')
        child.repaint_until(b'REPORT')
        child.repaint_until(first['id'][:8].encode())
        self.assertEqual(self.inputs(), [])
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        child.finish('q')
        self.owner.status('detached')

    def test_saved_report_reopens_after_owner_shutdown(self):
        child = self.attached('saved-report')
        child.write(b'i/help\r')
        child.repaint_until(b'/help')
        report, = self.reports(1)
        child.write(b'G')
        child.finish('close')
        saved = next(iter(self.snapshots().values()))['state']
        self.assertEqual(saved['windows'][0]['kind'], 'report')
        self.assertGreater(saved['windows'][0]['byte'], 0)
        peer = self.owner.view(bind=True)
        peer.send(type='quit', generation=peer.generation)
        peer.until('control')
        self.owner.status('stored')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        original = json.loads(path.read_text())
        for version in (6, 7):
            snapshot = json.loads(json.dumps(original))
            snapshot['state']['v'] = version
            owner = snapshot['state']['buffers'][0]
            old = dict(rollout(owner))
            for key in ('route', 'window', 'endpoint'):
                old.pop(key)
            old.update(session=owner['session'], control=owner['control'], reports=owner['reports'])
            snapshot['state']['buffers'][0] = old
            if version == 6:
                del snapshot['state']['windows'][0]['source']
            path.write_text(json.dumps(snapshot))
            resumed = self.start('--resume', 'saved-report', expect=b'REPORT')
            resumed.repaint_until(report['id'][:8].encode())
            resumed.command('workspace save')
            rows = self.wait_snapshot(lambda rows:
                next(iter(rows.values()))['state']['v'] == 9 and
                next(iter(rows.values()))['state']['windows'][0].get('source'))
            window = next(iter(rows.values()))['state']['windows'][0]
            self.assertEqual(window['byte'], saved['windows'][0]['byte'])
            resumed.write(b'gg')
            resumed.repaint_until(b'/help')
            self.owner.status('stored')
            resumed.finish('q')
        self.assertEqual(self.inputs(), [])

    def test_new_typing_survives_report_and_report_list_redacts(self):
        child = self.attached('newer-draft')
        child.write(b'i/status irc-ui-secret\rnewer unsent text')
        report, = self.reports(1)
        self.wait_synced('newer unsent text')
        child.repaint_until(b'newer unsent text')
        self.escape(child)
        child.command('report ' + report['id'][:8])
        child.repaint_until(b'REPORT')
        child.repaint_until(b'[redacted]')
        self.assertNotIn(b'irc-ui-secret', child.output)
        child.command('reports')
        child.repaint_until(b'[redacted]')
        self.assertNotIn(b'irc-ui-secret', child.output)
        child.command('history')
        child.repaint_until(b'newer unsent text')
        self.assertEqual(self.inputs(), [])
        child.finish('close')
        self.owner.status('detached')

    def test_saved_pending_command_queries_receipt_without_reexecution(self):
        child = self.attached('lost-receipt')
        child.finish('close')
        self.owner.status('detached')
        peer = self.owner.view(bind=True)
        request = peer.command('/fast')
        result = peer.result(request)
        self.assertEqual(result['status'], 'completed')
        instance = peer.capabilities['instance']
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer.close()
        self.owner.status('detached')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        rollout(saved['state']['buffers'][0])['pending'] = {
            'id': request, 'instance': instance, 'text': '/fast'}
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'lost-receipt', expect=b'history')
        resumed.repaint_until(b'ON')
        reports = self.reports(1)
        self.assertEqual(reports, [result['report']])
        effects = [event for event in self.owner.events()
                   if event['type'] == 'service_tier_changed']
        self.assertEqual(len(effects), 1)
        resumed.finish('q')
        self.owner.status('detached')
        self.assertEqual(self.inputs(), [])

    def test_changed_report_keeps_loaded_snapshot_and_recovers(self):
        child = self.attached('changed-report')
        child.write(b'i/status\r')
        child.repaint_until(b'REPORT')
        report, = self.reports(1)
        path = self.owner.directory / ('.view-report-' + report['id'])
        original = path.read_bytes()
        path.write_bytes(b'!' + original[1:])
        child.write(b'R')
        child.repaint_until(b'command report digest changed')
        child.repaint_until(b'/status')
        # Cached navigation continues despite source failure; retry can recover.
        child.write(b'Ggg')
        child.repaint_until(b'/status')
        path.write_bytes(original)
        child.write(b'R')
        child.repaint_until(b'/status')
        child.command('history')
        child.command('report missing')
        child.repaint_until(b'No retained command report')
        child.command('report')
        child.repaint_until(b'/status')
        child.finish('q')
        self.owner.status('detached')


if __name__ == '__main__':
    unittest.main()
