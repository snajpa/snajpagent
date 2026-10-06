# SPDX-License-Identifier: GPL-2.0-only
"""Explicit owner startup, asynchronous lifetime and saved-workspace boundaries."""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import unittest
import test_vm_frontend as frontend

BINARY = frontend.BINARY
from test_vm_frontend import rollout


class LaunchTests(unittest.TestCase):
    snapshots = frontend.WorkspaceTests.snapshots

    def wait_snapshot(self, predicate):
        def ready(rows):
            for child in self.terminals:
                while child.read(0):
                    pass
            return predicate(rows)
        return frontend.WorkspaceTests.wait_snapshot(self, ready)

    def setUp(self):
        frontend.WorkspaceTests.setUp(self)
        self.terminals = []
        self.provider = frontend.harness.FakeResponses()
        self.addCleanup(self.provider.close)
        self.provider.runtime_handler = lambda handler, request, sequence: self.provider.reply(
            handler, self.provider.response_body(sequence, 'launch-answer-marker').encode(),
            close_header=True)
        self.state = self.root / 'state'
        self.state.mkdir(mode=0o700)
        self.config = self.state / 'config.ini'
        frontend.harness.write_irc_config(self.config, self.provider.port, 'host-model')
        self.config.write_text(self.config.read_text().replace('${SNAJPAGENT_IRC_UI_KEY}',
                                                              '"irc-ui-secret"'))
        self.config.chmod(0o600)
        self.addCleanup(self.stop_owners)

    def stop_owners(self):
        # Only fixture owners have this exact private dotdir in their bootstrap argv.
        for row in subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True).splitlines():
            fields = row.strip().split(None, 1)
            if len(fields) == 2 and '--internal-session-owner ' + str(self.state) + ' ' in fields[1]:
                try:
                    os.kill(int(fields[0]), signal.SIGCONT)
                    os.kill(int(fields[0]), signal.SIGTERM)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + 5
        while self.owner_pids() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertEqual(self.owner_pids(), [])

    def owner_pids(self):
        return [int(row.split(None, 1)[0]) for row in subprocess.check_output(
            ['ps', '-axo', 'pid=,command='], text=True).splitlines()
            if '--internal-session-owner ' + str(self.state) + ' ' in row]

    def start(self, *args, **kwargs):
        child = frontend.WorkspaceTests.start(self, *args, **kwargs)
        self.terminals.append(child)
        return child

    def cli(self, *args):
        return subprocess.run([str(BINARY), '--dotdir', str(self.state), *args],
                              cwd=self.root, env={**os.environ, 'HOME': str(self.root)},
                              capture_output=True, timeout=15)

    def journals(self):
        return list((self.state / 'sessions').glob('*/events.jsonl'))

    def events(self, journal):
        return [json.loads(line) for line in journal.read_bytes().splitlines()]

    def ready(self, child, count=1):
        child.repaint_until(b'ATTACHED', 10)
        self.assertEqual(len(self.journals()), count)
        child.command('workspace save')
        rows = self.wait_snapshot(lambda rows: rows and
            any(row['state'].get('buffers') and
                all(rollout(buffer).get('base') for buffer in row['state']['buffers'])
                for row in rows.values()))
        state = next(row['state'] for row in rows.values() if row['state'].get('buffers'))
        sid = next(window['history']['session'] for window in state['windows']
                   if window['kind'] == 'transcript')
        return self.state / 'sessions' / sid / 'events.jsonl'

    def seed(self):
        result = self.cli('-e', '--', 'retained seed')
        self.assertEqual(result.returncode, 0, result.stderr)
        journal, = self.journals()
        return journal

    def status(self, sid, wanted):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = self.cli('-l')
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = [row.split('\t') for row in result.stdout.decode().splitlines()[1:]]
            status = next((row[4] for row in rows if sid.startswith(row[0])), None)
            if status == wanted:
                return
            for child in self.terminals:
                while child.read(0):
                    pass
            time.sleep(.02)
        self.fail((sid, wanted, status))

    def barrier(self):
        # Replace only this fixture's executable path after the VM has loaded it.
        # The fresh bootstrap pauses before exec; production code needs no hooks.
        copy = self.root / 'launch-binary'
        shutil.copy2(BINARY, copy)
        frontend.BINARY = copy
        try:
            child = self.start('-N', 'barrier')
        finally:
            frontend.BINARY = BINARY
        replacement = copy.with_suffix('.new')
        receipt = self.root / 'bootstrap.json'
        release = self.root / 'release'
        replacement.write_text(f'''#!{sys.executable}
import json, os, sys, termios, time
from pathlib import Path
mode = termios.tcgetattr(0)
mode[6] = [x[0] if isinstance(x, bytes) else x for x in mode[6]]
Path({str(receipt)!r}).write_text(json.dumps(dict(pid=os.getpid(), modes=mode)))
deadline = time.monotonic() + 15
while not Path({str(release)!r}).exists() and time.monotonic() < deadline:
    time.sleep(.01)
os.execv({str(BINARY)!r}, [{str(BINARY)!r}] + sys.argv[1:])
''')
        replacement.chmod(0o700)
        replacement.replace(copy)
        self.addCleanup(release.touch)
        return child, receipt, release

    def wait_file(self, path):
        deadline = time.monotonic() + 5
        while not path.exists() and time.monotonic() < deadline:
            for child in self.terminals:
                child.read(0)
            time.sleep(.01)
        self.assertTrue(path.exists(), path)
        return json.loads(path.read_text())

    def test_new_named_owner_submits_and_survives_workspace_close(self):
        child = self.start('-N', 'layout')
        name = 'agent $(touch sentinel) ; α'
        child.command('new ' + json.dumps(name, ensure_ascii=False))
        journal = self.ready(child)
        created = self.events(journal)
        self.assertEqual(next(e['data']['name'] for e in created if e['type'] == 'session_named'), name)
        self.assertFalse((self.root / 'sentinel').exists())
        self.assertFalse(any(e['type'] == 'input_received' for e in created))
        child.write(b'iworkspace-launch-prompt\r')
        child.until(b'launch-answer-marker', 10)
        child.write(b'\x1b')
        time.sleep(.06)
        child.finish('close')
        self.status(journal.parent.name, 'detached')
        self.assertEqual(len([e for e in self.events(journal) if e['type'] == 'input_received']), 1)
        resumed = self.start('--resume', 'layout', expect=b'history')
        resumed.repaint_until(b'ATTACHED')
        resumed.finish('q!')
        self.status(journal.parent.name, 'stored')

    def test_explicit_resume_and_saved_workspace_never_restart_stopped_owner(self):
        journal = self.seed()
        child = self.start('-N', 'saved', '--session', journal.parent.name, expect=b'history')
        self.ready(child)
        child.finish('q!')
        self.status(journal.parent.name, 'stored')
        count = len([e for e in self.events(journal) if e['type'] == 'input_received'])
        restored = self.start('--resume', 'saved', expect=b'history')
        restored.repaint_until(b'read-only')
        self.status(journal.parent.name, 'stored')
        self.assertEqual(self.owner_pids(), [])
        restored.command('session')
        self.ready(restored)
        self.assertEqual(len([e for e in self.events(journal) if e['type'] == 'input_received']), count)
        restored.finish('q!')

    def test_picker_enter_resumes_and_live_selection_keeps_same_owner(self):
        journal = self.seed()
        child = self.start('-N', 'picker')
        child.repaint_until(journal.parent.name[:8].encode())
        child.write(b'\r')
        self.ready(child)
        pids = self.owner_pids()
        self.assertEqual(len(pids), 1)
        child.command('close')
        child.wait_exit()
        again = self.start('--session', journal.parent.name, expect=b'history')
        self.ready(again)
        self.assertEqual(self.owner_pids(), pids)
        again.finish('q!')

    def test_startup_error_is_reported_without_replacing_old_owner(self):
        child = self.start('-N', 'failure')
        child.command('new first')
        journal = self.ready(child)
        self.config.write_text('[agent]\nunknown_launch_setting = true\n')
        child.command('vsp')
        child.command('new broken')
        child.repaint_until(b'invalid configuration at line 2', 10)
        self.assertEqual(len(self.journals()), 1)
        self.assertEqual(len(self.owner_pids()), 1)
        child.write(b'\x17h')
        child.repaint_until(b'ATTACHED')
        child.finish('qa!')
        self.assertTrue(journal.exists())

    def test_slow_launch_does_not_take_over_repurposed_window(self):
        child, receipt, release = self.barrier()
        child.command('new slow')
        saved = self.wait_file(receipt)
        self.assertEqual(frontend.normalized_modes(saved['modes']), child.original)
        child.command('help')
        child.repaint_until(b'j/k or arrows')
        release.touch()
        child.repaint_until(b'Session ready:', 10)
        child.command('workspace save')
        rows = self.wait_snapshot(lambda rows: next(iter(rows.values()))['state']['windows'][0]['kind']
                                 == 'help')
        self.assertEqual(next(iter(rows.values()))['state']['windows'][0]['kind'], 'help')
        journal, = self.journals()
        self.status(journal.parent.name, 'detached')
        child.command('session ' + journal.parent.name)
        self.ready(child)
        child.finish('q!')

    def test_frontend_loss_during_bootstrap_preserves_launch_and_private_pty(self):
        child, receipt, release = self.barrier()
        child.command('new survivor')
        self.wait_file(receipt)
        child.finish('close')
        # Close the actual PTY before allowing exec to establish the native owner.
        child.close()
        self._cleanups = [item for item in self._cleanups if item[0] != child.close]
        self.terminals.remove(child)
        release.touch()
        deadline = time.monotonic() + 10
        while not self.journals() and time.monotonic() < deadline:
            time.sleep(.02)
        journal, = self.journals()
        self.status(journal.parent.name, 'detached')
        restored = self.start('--resume', 'barrier')
        restored.repaint_until(journal.parent.name[:8].encode())
        self.assertEqual(len(self.journals()), 1)
        restored.write(b'\r')
        self.ready(restored)
        restored.finish('q!')

    def test_repeated_resume_in_splits_joins_one_pending_launch(self):
        journal = self.seed()
        child, receipt, release = self.barrier()
        child.command('session ' + journal.parent.name)
        self.wait_file(receipt)
        child.command('vsp')
        child.command('session ' + journal.parent.name)
        child.repaint_until(b'already starting')
        self.assertEqual(len(self.owner_pids()), 1)
        release.touch()
        self.ready(child)
        child.command('workspace save')
        self.wait_snapshot(lambda rows:
            all(window['history']['session'] == journal.parent.name
                for window in next(iter(rows.values()))['state']['windows']))
        self.assertEqual(len(self.owner_pids()), 1)
        child.finish('qa!')
        self.status(journal.parent.name, 'stored')

    def test_irc_bind_failure_reports_retained_session_and_can_be_retried(self):
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        child = self.start('-N', 'irc-failure')
        original = self.config.read_text()
        self.config.write_text(original + f'[irc]\nlisten = 127.0.0.1:{listener.getsockname()[1]}\n')
        child.command('new')
        child.repaint_until(b'cannot listen on IRC endpoint', 10)
        self.assertEqual(self.journals(), [])
        child.command('new retained-failure')
        deadline = time.monotonic() + 5
        while not self.journals() and time.monotonic() < deadline:
            child.read()
        child.repaint_until(b'cannot listen on IRC endpoint', 10)
        journal, = self.journals()
        self.status(journal.parent.name, 'stored')
        self.assertFalse(any(e['type'] == 'input_received' for e in self.events(journal)))
        listener.close()
        child.command('session ' + journal.parent.name)
        self.ready(child)
        child.finish('q!')
        self.status(journal.parent.name, 'stored')

    def test_live_busy_owner_is_never_replaced(self):
        first = self.start('-N', 'controller')
        first.command('new occupied')
        journal = self.ready(first)
        pids = self.owner_pids()
        second = self.start('-N', 'observer', '--session', journal.parent.name, expect=b'history')
        second.repaint_until(b'controller')
        self.assertEqual(self.owner_pids(), pids)
        self.status(journal.parent.name, 'attached')
        second.finish('close')
        first.finish('q!')

    def test_tiny_terminal_can_start_unnamed_owner(self):
        child = self.start('-N', 'tiny')
        child.resize(2, 8)
        child.command('new')
        deadline = time.monotonic() + 5
        while not self.journals() and time.monotonic() < deadline:
            child.read()
        self.assertEqual(len(self.journals()), 1)
        child.resize(12, 100)
        journal = self.ready(child)
        child.finish('q!')
        self.status(journal.parent.name, 'stored')

    def test_multiple_explicit_new_requests_create_independent_owners(self):
        child = self.start('-N', 'many')
        child.write(b':new first\r:vsp\r:new second\r')
        child.repaint_until(b'ATTACHED', 10)
        deadline = time.monotonic() + 10
        while len(self.journals()) < 2 and time.monotonic() < deadline:
            child.read()
        self.assertEqual(len(self.journals()), 2)
        self.assertEqual(len(self.owner_pids()), 2)
        child.command('workspace save')
        self.wait_snapshot(lambda rows: len(next(iter(rows.values()))['state']['buffers']) == 2 and
            all(rollout(buffer).get('base') for buffer in next(iter(rows.values()))['state']['buffers']))
        child.finish('qa!')
        for journal in self.journals():
            self.status(journal.parent.name, 'stored')


if __name__ == '__main__':
    unittest.main()
