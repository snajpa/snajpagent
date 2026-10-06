# SPDX-License-Identifier: GPL-2.0-only
"""Real workspace -> native owner -> durable journal/provider integration."""

import json
import os
import signal
import sys
import time
import unittest
import uuid

import test_vm_frontend as frontend

# Each standalone fixture consumes a binary argument at import time. The
# frontend has already consumed ours; preserve unittest's optional selectors.
arguments = sys.argv
sys.argv = sys.argv[:1]
import test_session_view as owners
sys.argv = arguments


owners.BINARY = frontend.BINARY


class ControlTests(unittest.TestCase):
    snapshots = frontend.WorkspaceTests.snapshots

    def start(self, *args, **kwargs):
        child = frontend.WorkspaceTests.start(self, *args, **kwargs)
        self.terminals.append(child)
        return child

    def wait_snapshot(self, predicate):
        # A newly named workspace first contains its empty picker snapshot.
        def ready(rows):
            for child in self.terminals:
                while child.read(0):
                    pass
            return rows and next(iter(rows.values()))['state'].get('buffers') and predicate(rows)
        return frontend.WorkspaceTests.wait_snapshot(self, ready)

    def setUp(self):
        self.terminals = []
        self.owner = owners.SessionViewTests()
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)
        self.root = self.owner.root
        self.owner.detach()

    def inputs(self):
        return [event for event in self.owner.events() if event['type'] == 'input_received']

    def escape(self, child):
        child.write(b'\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')

    def test_picker_attach_unicode_paste_submit_and_newer_draft(self):
        child = self.start('-N', 'composing')
        child.until(self.owner.sid[:8].encode())
        child.write(b'\r')
        child.until(b'ATTACHED')
        self.owner.status('attached')
        child.write('iAe\u0301'.encode() + b'\x7f')
        child.write(b'\x1b[200~' + '👩‍💻\n:qa\nliteral'.encode() + b'\x1b[201~')
        self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['buffers'][0]['draft']
                           == 'A👩‍💻\n:qa\nliteral')
        child.write(b'\rnext-draft')
        event = self.owner.wait_event('input_received')
        self.assertEqual(event['data']['text'], 'A👩‍💻\n:qa\nliteral')
        self.wait_snapshot(lambda rows:
                           next(iter(rows.values()))['state']['buffers'][0]['pending'] is None and
                           next(iter(rows.values()))['state']['buffers'][0]['draft'] == 'next-draft')
        self.assertEqual(len(self.inputs()), 1)
        self.escape(child)
        child.command('q')
        child.repaint_until(b'Unsent draft')
        self.assertIsNone(child.process.poll())
        child.finish('close')
        self.owner.status('detached')
        resumed = self.start('--resume', 'composing', expect=b'history')
        resumed.until(b'ATTACHED')
        resumed.repaint_until(b'next-draft')
        resumed.command('detach')
        self.owner.status('detached')
        resumed.finish()

    def test_shared_split_controller_close_and_normal_quit(self):
        child = self.start('-N', 'splits')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.command('vsp')
        child.write(b'ishared-draft')
        self.escape(child)
        child.command('workspace save')
        rows = self.wait_snapshot(lambda rows:
                                  len(next(iter(rows.values()))['state']['windows']) == 2)
        self.assertEqual(len(next(iter(rows.values()))['state']['buffers']), 1)
        child.command('close')
        self.owner.status('attached')
        child.repaint_until(b'shared-draft')
        child.command('attach missing-session')
        child.command('q')
        child.repaint_until(b'Unsent draft')
        child.finish('q!')
        self.owner.status('stored')

    def test_rejected_command_keeps_text_and_recover_never_resubmits(self):
        child = self.start('-N', 'rejection')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'i/status\r')
        child.repaint_until(b'command capability unavailable')
        self.assertEqual(self.inputs(), [])
        self.escape(child)
        child.command('recover')
        child.repaint_until(b'Submission recovered')
        rows = self.wait_snapshot(lambda rows:
                                  next(iter(rows.values()))['state']['buffers'][0]['draft']
                                  == '/status')
        self.assertIsNone(next(iter(rows.values()))['state']['buffers'][0]['pending'])
        child.finish('close')
        self.owner.status('detached')

    def test_saved_unknown_receipt_is_retained_without_resubmission(self):
        child = self.start('-N', 'uncertain')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        peer = self.owner.view()
        instance = peer.capabilities['instance']
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        saved['state']['buffers'][0]['pending'] = {
            'id': uuid.uuid4().hex, 'instance': instance, 'text': 'possibly-admitted-text'}
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'uncertain', expect=b'history')
        resumed.repaint_until(b'Submission outcome unknown')
        self.owner.status('attached')
        resumed.repaint_until(b'ATTACHED')
        self.assertEqual(self.inputs(), [])
        resumed.command('qa')
        resumed.repaint_until(b'Unsent draft or unresolved submission')
        resumed.command('recover')
        resumed.repaint_until(b'Submission recovered')
        self.assertEqual(self.inputs(), [])
        resumed.finish('close')

    def test_signal_detaches_preserves_draft_and_idle_emits_no_frames(self):
        child = self.start('-N', 'signal-draft')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'idurable-unsent')
        self.wait_snapshot(lambda rows:
                           next(iter(rows.values()))['state']['buffers'][0]['draft']
                           == 'durable-unsent')
        while child.read(.1):
            pass
        self.assertEqual(child.read(1.2), b'')
        child.signal(signal.SIGTERM)
        child.wait_exit()
        self.owner.status('detached')
        resumed = self.start('--resume', 'signal-draft', expect=b'history')
        resumed.until(b'ATTACHED')
        resumed.repaint_until(b'durable-unsent')
        resumed.finish('close')

    def test_receipt_after_frontend_loss_does_not_repeat_prompt(self):
        child = self.start('-N', 'lost-receipt')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'iexactly-once-after-reconnect\rnewer-text')
            self.wait_snapshot(lambda rows:
                               next(iter(rows.values()))['state']['buffers'][0]['pending'] and
                               next(iter(rows.values()))['state']['buffers'][0]['draft']
                               == 'newer-text')
            # Drain the test terminal, let the queued wire request leave, then
            # hold only this disposable frontend before allowing owner admission.
            deadline = time.monotonic() + .2
            while time.monotonic() < deadline:
                child.read(.02)
            child.signal(signal.SIGSTOP)
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.owner.wait_event('input_received')
        child.signal(signal.SIGTERM)
        child.signal(signal.SIGCONT)
        child.wait_exit()
        self.owner.status('detached')
        resumed = self.start('--resume', 'lost-receipt', expect=b'history')
        resumed.repaint_until(b'Prompt committed')
        self.wait_snapshot(lambda rows:
                           next(iter(rows.values()))['state']['buffers'][0]['pending'] is None)
        self.assertEqual(next(iter(self.snapshots().values()))['state']['buffers'][0]['draft'],
                         'newer-text')
        self.assertEqual(len(self.inputs()), 1)
        resumed.finish('close')

    def test_insert_joining_graphemes_keeps_resumable_cursor(self):
        child = self.start('-N', 'clusters')
        child.command('history ' + self.owner.sid)
        child.write('i👩💻'.encode() + b'\x1b[D' + '\u200d'.encode())
        self.wait_snapshot(lambda rows:
                           next(iter(rows.values()))['state']['buffers'][0]['draft'] == '👩‍💻')
        self.escape(child)
        child.finish('close')
        resumed = self.start('--resume', 'clusters', expect=b'history')
        resumed.write(b'i\x7f')
        self.wait_snapshot(lambda rows:
                           next(iter(rows.values()))['state']['buffers'][0]['draft'] == '')
        self.escape(resumed)
        resumed.finish()

    def test_fast_escape_colon_leaves_insert_without_submitting(self):
        child = self.start('-N', 'fast-escape')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ipreserve this\x1b:workspace save\r')
        self.wait_snapshot(lambda rows:
                           next(iter(rows.values()))['state']['buffers'][0]['draft']
                           == 'preserve this')
        self.assertEqual(self.inputs(), [])
        child.finish('close')
        self.owner.status('detached')


if __name__ == '__main__':
    unittest.main()
