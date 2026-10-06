# SPDX-License-Identifier: GPL-2.0-only
"""Real workspace -> native owner -> durable journal/provider integration."""

import hashlib
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

    def wait_synced(self, text):
        digest = hashlib.sha256(text.encode()).hexdigest()
        return self.wait_snapshot(lambda rows:
            (next(iter(rows.values()))['state']['buffers'][0].get('base') or {}).get('sha256')
            == digest)

    def owner_draft(self, text=None):
        peer = self.owner.view()
        peer.bind()
        draft = peer.draft()
        if text is not None:
            draft = peer.replace_draft(draft['revision'], text)['draft']
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer.close()
        self.owner.status('detached')
        return draft

    def test_owner_draft_adoption_and_final_edit_flushed_on_close(self):
        self.owner_draft('retained 👩‍💻')
        child = self.start('-N', 'owner-draft')
        child.command('attach ' + self.owner.sid)
        self.wait_synced('retained 👩‍💻')
        child.repaint_until('retained 👩‍💻'.encode())
        # No autosave pause between the last edit and clean detach.
        child.write(b'A-final\x1b:close\r')
        child.wait_exit()
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'retained 👩‍💻-final')
        saved = next(iter(self.snapshots().values()))['state']
        self.assertEqual(saved['v'], 4)
        self.assertEqual(saved['buffers'][0]['draft'], 'retained 👩‍💻-final')
        self.assertTrue(saved['buffers'][0]['control'])
        self.assertEqual(self.inputs(), [])

    def test_conflicting_workspace_and_owner_edits_preserve_both_choices(self):
        child = self.start('-N', 'conflict')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ibase draft')
        self.wait_synced('base draft')
        self.escape(child)
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        for choice in ('owner', 'local'):
            saved = json.loads(path.read_text())
            saved['state']['buffers'][0]['draft'] = 'local ' + choice
            saved['state']['buffers'][0]['cursor'] = len('local ' + choice)
            path.write_text(json.dumps(saved))
            self.owner_draft('owner ' + choice)
            resumed = self.start('--resume', 'conflict', expect=b'history')
            resumed.repaint_until(b'Draft conflict')
            rows = self.wait_snapshot(lambda rows:
                next(iter(rows.values()))['state']['buffers'][0].get('conflict') is not None)
            buffer = next(iter(rows.values()))['state']['buffers'][0]
            self.assertEqual(buffer['draft'], 'local ' + choice)
            self.assertEqual(buffer['conflict']['text'], 'owner ' + choice)
            resumed.finish('close')
            self.owner.status('detached')
            self.owner_draft('later owner edit')
            previous = json.loads(path.read_text())['activity_ms']
            resumed = self.start('--resume', 'conflict', expect=b'history')
            resumed.repaint_until(b'Draft conflict')
            resumed.command('workspace save')
            self.wait_snapshot(lambda rows: next(iter(rows.values()))['activity_ms'] > previous)
            self.assertEqual(json.loads(path.read_text())['state']['buffers'][0]['conflict']['text'],
                             'owner ' + choice)
            resumed.command('draft ' + choice)
            expected = choice + ' ' + choice
            self.wait_synced(expected)
            resumed.finish('close')
            self.owner.status('detached')
            self.assertEqual(self.owner_draft()['text'], expected)
        self.assertEqual(self.inputs(), [])

    def test_one_sided_offline_edit_and_old_workspace_snapshot(self):
        child = self.start('-N', 'offline')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ione side')
        self.wait_synced('one side')
        self.escape(child)
        child.finish('close')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        saved['state']['buffers'][0]['draft'] = 'local offline edit'
        saved['state']['buffers'][0]['cursor'] = len('local offline edit')
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'offline', expect=b'history')
        self.wait_synced('local offline edit')
        resumed.finish('close')
        self.assertEqual(self.owner_draft()['text'], 'local offline edit')
        self.owner_draft('owner changed')
        resumed = self.start('--resume', 'offline', expect=b'history')
        self.wait_synced('owner changed')
        resumed.finish('close')
        saved = json.loads(path.read_text())
        saved['state']['v'] = 3
        buffer = saved['state']['buffers'][0]
        del buffer['base'], buffer['conflict']
        buffer['draft'], buffer['cursor'] = '', 0
        path.write_text(json.dumps(saved))
        resumed = self.start('--resume', 'offline', expect=b'history')
        self.wait_synced('owner changed')
        resumed.finish('close')
        self.assertEqual(self.inputs(), [])

    def test_force_quit_discards_both_conflicting_drafts(self):
        self.owner_draft('owner copy')
        child = self.start('-N', 'discard-conflict')
        child.command('history ' + self.owner.sid)
        child.write(b'ilocal copy\x1b:attach\r')
        child.repaint_until(b'Draft conflict')
        child.finish('q!')
        self.owner.status('stored')
        buffer = next(iter(self.snapshots().values()))['state']['buffers'][0]
        self.assertEqual(buffer['draft'], '')
        self.assertIsNone(buffer['conflict'])
        self.assertEqual(self.inputs(), [])

    def test_detached_conflict_can_restore_retained_owner_copy(self):
        self.owner_draft('retained owner copy')
        child = self.start('-N', 'offline-choice')
        child.command('history ' + self.owner.sid)
        child.write(b'ilocal copy\x1b:attach\r')
        child.repaint_until(b'Draft conflict')
        child.command('detach')
        child.repaint_until(b'Detached; unresolved draft retained')
        self.owner.status('detached')
        child.command('draft owner')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['buffers'][0]['draft'] == 'retained owner copy' and
            next(iter(rows.values()))['state']['buffers'][0]['conflict'] is None)
        child.finish('close')
        self.assertEqual(self.owner_draft()['text'], 'retained owner copy')
        self.assertEqual(self.inputs(), [])

    def test_unacknowledged_edit_survives_frontend_loss(self):
        child = self.start('-N', 'unacknowledged')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'iacknowledged')
        self.wait_synced('acknowledged')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'-suffix')
            self.wait_snapshot(lambda rows:
                next(iter(rows.values()))['state']['buffers'][0]['draft'] == 'acknowledged-suffix')
            child.signal(signal.SIGTERM)
            child.wait_exit()
        finally:
            os.kill(self.owner.owner, signal.SIGCONT)
        self.owner.status('detached')
        resumed = self.start('--resume', 'unacknowledged', expect=b'history')
        self.wait_synced('acknowledged-suffix')
        resumed.finish('close')
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'acknowledged-suffix')
        self.assertEqual(self.inputs(), [])

    def test_suspend_flushes_owner_draft_and_reconnects(self):
        child = self.start('-N', 'suspended-draft')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'isuspend draft')
        self.escape(child)
        child.write(b'\x1a')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and child.state().get('state') != 'stopped':
            child.read()
        self.assertEqual(child.state().get('state'), 'stopped')
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'suspend draft')
        # A different controller may edit while the workspace is suspended.
        self.owner_draft('edited during suspend')
        child.signal(signal.SIGCONT)
        self.wait_synced('edited during suspend')
        child.repaint_until(b'edited during suspend')
        child.finish('close')
        self.assertEqual(self.inputs(), [])

    def test_workspace_switch_flushes_shared_owner_and_preserves_source(self):
        target = self.start('-N', 'destination')
        target.finish()
        child = self.start('-N', 'source')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'ifinal source draft\x1b:vsp\r:workspaces\rG\r')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            child.read()
            rows = frontend.WorkspaceTests.cli(self, '-l').stdout
            if b'\tdestination\topen\t' in rows:
                break
        self.assertIn(b'\tdestination\topen\t', rows)
        self.owner.status('detached')
        self.assertEqual(self.owner_draft()['text'], 'final source draft')
        source = next(x for x in self.snapshots().values() if x['name'] == 'source')
        self.assertEqual(source['state']['buffers'][0]['draft'], 'final source draft')
        self.assertTrue(source['state']['buffers'][0]['control'])
        child.finish()
        self.assertEqual(self.inputs(), [])

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

    def test_undo_stops_at_submitted_and_adopted_draft_boundaries(self):
        child = self.start('-N', 'undo-boundaries')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'iadmitted text\rnext draft')
        self.owner.wait_event('input_received')
        self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['buffers'][0]['pending'] is None and
            next(iter(rows.values()))['state']['buffers'][0]['draft'] == 'next draft')
        self.escape(child)
        child.write(b'uu')
        child.command('workspace name after-undo')
        rows = self.wait_snapshot(lambda rows: next(iter(rows.values()))['name'] == 'after-undo')
        self.assertEqual(next(iter(rows.values()))['state']['buffers'][0]['draft'], '')
        child.write(b'ilocal draft')
        self.escape(child)
        child.command('detach')
        child.repaint_until(b'Detached; owner continues')
        self.owner_draft('remote replacement')
        child.command('attach')
        child.repaint_until(b'Owner draft restored')
        child.write(b'\tu')
        child.command('workspace name after-adoption')
        rows = self.wait_snapshot(lambda rows:
                                  next(iter(rows.values()))['name'] == 'after-adoption')
        self.assertEqual(next(iter(rows.values()))['state']['buffers'][0]['draft'],
                         'remote replacement')
        self.assertEqual(len(self.inputs()), 1)
        child.finish('close')

    def test_receipt_after_frontend_loss_does_not_repeat_prompt(self):
        child = self.start('-N', 'lost-receipt')
        child.command('attach ' + self.owner.sid)
        child.until(b'ATTACHED')
        child.write(b'iexactly-once-after-reconnect')
        self.wait_synced('exactly-once-after-reconnect')
        os.kill(self.owner.owner, signal.SIGSTOP)
        try:
            child.write(b'\rnewer-text')
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
        rows = self.wait_snapshot(lambda rows:
                                  next(iter(rows.values()))['state']['buffers'][0]['draft'] == '👩‍💻')
        self.assertEqual(next(iter(rows.values()))['state']['buffers'][0]['cursor'], len('👩‍💻'.encode()))
        self.escape(child)
        child.finish('close')
        resumed = self.start('--resume', 'clusters', expect=b'history')
        resumed.write(b'A\x7f')
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
