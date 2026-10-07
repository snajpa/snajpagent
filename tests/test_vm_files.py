# SPDX-License-Identifier: GPL-2.0-only
"""Native /cat retains operator-only immutable files, independently of a pager."""

import hashlib
import json
import os
import threading
import time
import unittest

import test_vm_notifications as notifications


class FileTests(unittest.TestCase):
    setUp = notifications.NotificationTests.setUp

    def snapshot(self, command):
        request = self.peer.command(command)
        result = self.peer.result(request)
        self.assertEqual(result['status'], 'completed', result)
        self.assertEqual(result['outcome'], 'ok', result)
        self.assertEqual(result['report_error'], '')
        report = result['report']
        path = self.owner.directory / ('.view-report-' + report['id'])
        data = path.read_bytes()
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(len(data), report['bytes'])
        self.assertEqual(hashlib.sha256(data).hexdigest(), report['sha256'])
        return report, data

    def test_binary_snapshot_survives_source_replacement_and_explicit_refresh(self):
        source = self.owner.root / 'file with spaces.bin'
        original = bytes(range(256)) * 1024
        source.write_bytes(original)
        command = "/cat 'file with spaces.bin'"
        before = self.owner.journal.read_bytes()
        first, data = self.snapshot(command)
        self.assertEqual(data, command.encode() + b'\n' + original)
        source.write_bytes(b'new source version')
        second, newer = self.snapshot(command)
        self.assertNotEqual(first['id'], second['id'])
        self.assertEqual(newer, command.encode() + b'\nnew source version')
        source.unlink()
        self.assertEqual((self.owner.directory / ('.view-report-' + first['id'])).read_bytes(), data)
        self.assertEqual(self.owner.journal.read_bytes(), before)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_empty_file_symlink_and_nonregular_errors(self):
        source = self.owner.root / 'empty'
        source.touch()
        os.symlink('empty', self.owner.root / 'alias')
        _, data = self.snapshot('/cat alias')
        self.assertEqual(data, b'/cat alias\n')
        os.mkfifo(self.owner.root / 'fifo')
        for path in ('missing', '.', 'fifo'):
            result = self.peer.result(self.peer.command('/cat ' + path))
            self.assertEqual(result['status'], 'completed')
            self.assertEqual(result['outcome'], 'error')
            self.assertIsNotNone(result['report'])
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))

    def test_copy_interrupt_retains_error_and_removes_unpublished_bytes(self):
        source = self.owner.root / 'large-file'
        with source.open('wb') as stream:
            stream.truncate(64 * 1024 * 1024)
        request = self.peer.command('/cat large-file')
        self.peer.until('result', lambda result: result['id'] == request)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            files = list(self.owner.directory.glob('.view-report-*'))
            if any(path.stat().st_size >= 131072 for path in files):
                break
            time.sleep(.002)
        else:
            self.fail('file copy never started')
        self.peer.send(type='cancel', generation=self.peer.generation)
        result = self.peer.result(request)
        self.assertEqual(result['status'], 'completed')
        self.assertEqual(result['outcome'], 'error')
        self.assertLess(result['report']['bytes'], 1024)
        files = list(self.owner.directory.glob('.view-report-*'))
        self.assertEqual(len(files), 1)
        self.assertIn(b'cannot retain file snapshot', files[0].read_bytes())
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_snapshot_during_provider_wait_keeps_turn_running(self):
        started = threading.Event()
        def held(handler, request, sequence):
            started.set()
            self.owner.release.wait(10)
            self.owner.provider.reply(handler, self.owner.provider.response_body(
                sequence, 'completed after file snapshot').encode(), close_header=True)
        self.owner.provider.runtime_handler = held
        self.peer.result(self.peer.submit('held turn'))
        self.assertTrue(started.wait(5))
        (self.owner.root / 'during-turn').write_bytes(b'operator only file')
        _, data = self.snapshot('/cat during-turn')
        self.assertIn(b'operator only file', data)
        self.assertFalse(any(e['type'] in ('turn_interrupted', 'steering_added',
                                          'turn_cancel_requested') for e in self.owner.events()))
        self.owner.release.set()
        self.owner.wait_event('turn_completed')


class WorkspaceFileTests(unittest.TestCase):
    setUp = notifications.WorkspaceNotificationTests.setUp
    start = notifications.WorkspaceNotificationTests.start
    snapshots = notifications.WorkspaceNotificationTests.snapshots
    wait_snapshot = notifications.WorkspaceNotificationTests.wait_snapshot
    reports = notifications.WorkspaceNotificationTests.reports

    def test_file_view_redacts_and_survives_owner_and_workspace_shutdown(self):
        source = self.root / 'operator-file'
        source.write_bytes(b'file-start\nirc-ui-secret\n\x00\xff\nfile-end\n')
        child = self.start('-N', 'files', columns=120)
        child.command('attach ' + self.owner.sid)
        child.attached()
        child.write(b'i/cat operator-file\r')
        self.reports(1)
        child.write(b'\x1b')
        child.read(.06)
        child.command('report')
        child.repaint_until(b'REPORT')
        child.repaint_until(b'file-start')
        child.repaint_until(b'[redacted]')
        child.repaint_until(b'\\x00\\xff')
        self.assertNotIn(b'irc-ui-secret', child.output)
        report, = self.reports(1)
        child.finish('close')
        peer = self.owner.view(bind=True)
        peer.send(type='quit', generation=peer.generation)
        peer.until('control')
        self.owner.status('stored')
        source.unlink()
        resumed = self.start('--resume', 'files', expect=b'REPORT', columns=120)
        resumed.repaint_until(report['id'][:8].encode())
        resumed.repaint_until(b'file-end')
        resumed.finish('close')
        self.owner.status('stored')
        saved, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        self.assertEqual(json.loads(saved.read_text())['state']['windows'][0]['kind'], 'report')
        self.assertFalse(any(e['type'] == 'input_received' for e in self.owner.events()))


if __name__ == '__main__':
    unittest.main()
