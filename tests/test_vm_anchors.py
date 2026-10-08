# SPDX-License-Identifier: GPL-2.0-only
"""Source positions survive redaction, reflow, hidden detail and old snapshots."""

import json
import os
import subprocess
import time
import unittest

import test_vm_frontend as frontend
from store_history import journal_paths, read_events


class AnchorTests(unittest.TestCase):
    snapshots = frontend.WorkspaceTests.snapshots
    seed_session = frontend.WorkspaceTests.seed_session

    def setUp(self):
        frontend.WorkspaceTests.setUp(self)
        self.terminals = []

    def start(self, *args, **kwargs):
        child = frontend.WorkspaceTests.start(self, *args, **kwargs)
        self.terminals.append(child)
        return child

    def wait_snapshot(self, predicate):
        # Keep consuming split redraws while waiting for asynchronous reads
        # and autosave, including on small Darwin PTY output queues.
        def ready(rows):
            for child in self.terminals:
                while child.read(0):
                    pass
            return predicate(rows)
        return frontend.WorkspaceTests.wait_snapshot(self, ready)

    def save(self, child):
        before = next(iter(self.snapshots().values()))['activity_ms']
        child.command('workspace save')
        values = self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['activity_ms'] > before)
        return next(iter(values.values()))['state']

    def test_redacted_source_anchor_survives_resize_resume_and_legacy_upgrade(self):
        self.redacted_source_anchor(False)

    def test_legacy_journal_anchor_survives_resize_resume_and_snapshot_upgrade(self):
        self.redacted_source_anchor(True)

    def redacted_source_anchor(self, legacy):
        prefix = 'irc-ui-secret\n' * 40
        text = prefix + 'anchor-marker-' + '界' * 40 + '\nlast'
        journal = self.seed_session(text, legacy=legacy)
        config = self.root / 'state' / 'config.ini'
        config.write_text(self.fixture_config.read_text().replace(
            '${SNAJPAGENT_IRC_UI_KEY}', '"irc-ui-secret"'))
        config.chmod(0o600)
        previous = max(event['seq'] for event in read_events(journal))
        provider = self.fixture_provider

        def respond(handler, request, sequence):
            provider.reply(handler, provider.response_body(sequence, text).encode())

        provider.runtime_handler = respond
        terminal = frontend.Terminal(self.root, ['--config', str(config), '--resume',
            journal.parent.name], subcommand=None)
        self.addCleanup(terminal.close)
        terminal.until(b'session id')
        terminal.write(b'new retained response\r')
        deadline = time.monotonic() + 5
        while not any(event['seq'] > previous and event['type'] == 'turn_completed'
                      for event in read_events(journal)):
            if time.monotonic() >= deadline:
                self.fail('interactive fixture turn did not complete')
            terminal.read(.05)
        terminal.write(b'/exit\r')
        terminal.wait_exit()
        self.assertEqual(terminal.state()['returncode'], 0)
        self.assertTrue((journal.parent / '.view-presentation.snb').is_file())
        # A streamed item is anchored to its first fragment, as saved by the UI.
        event = next(event for event in read_events(journal)
                     if event['seq'] > previous and
                     event['type'] in ('response_output', 'response_completed'))
        key = event['data']['response_id'] + '/0'
        byte = len((prefix + 'anchor-marker-' + '界' * 3).encode())
        child = self.start('-N', 'source', columns=80)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'You can resume this session with the following command:')
        child.resize(12, 27)
        child.finish('qa')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        anchor = saved['state']['windows'][0]['history']
        anchor.update(key=key, seq=event['seq'], byte=byte, heading=False,
                      follow=False, source=True)
        path.write_text(json.dumps(saved))
        child = self.start('--resume', 'source', columns=27, expect=b'Workspace restored')
        child.repaint_until(b'anchor-marker-')
        first = self.save(child)['windows'][0]['history']
        self.assertEqual(first['byte'], byte)
        self.assertTrue(first['source'])
        self.assertNotIn(b'irc-ui-secret', child.output)
        child.resize(12, 80)
        child.repaint_until(b'anchor-marker-')
        self.assertEqual(self.save(child)['windows'][0]['history'], first)
        child.command('vsp')
        child.repaint_until(b'anchor-marker-')
        windows = self.save(child)['windows']
        self.assertTrue(all(row['history'] == first for row in windows))
        child.finish('qa')
        saved = json.loads(path.read_text())
        saved['state']['v'] = 6
        # Version6 used displayed-byte positions; use the marker's row start.
        display_byte = len(('<redacted:secret>\n' * 40).encode())
        for window in saved['state']['windows']:
            window['history']['byte'] = display_byte
            del window['history']['source'], window['history']['route']
        path.write_text(json.dumps(saved))
        child = self.start('--resume', 'source', columns=100, expect=b'Workspace restored')
        child.repaint_until(b'anchor-marker-')
        # Split documents load independently; the first frame can precede
        # conversion of the other window's older coordinate.
        values = self.wait_snapshot(lambda rows:
            next(iter(rows.values()))['state']['v'] == 14 and
            all(window['history']['source'] for window in
                next(iter(rows.values()))['state']['windows']))
        upgraded = next(iter(values.values()))['state']
        self.assertEqual(upgraded['v'], 14)
        for window in upgraded['windows']:
            self.assertTrue(window['history']['source'])
            self.assertEqual(window['history']['byte'], len(prefix.encode()))
        child.output.clear()
        child.write(b'/anchor-marker-\r')
        child.repaint_until(b'Match')
        matched = self.save(child)['windows'][0]['history']
        self.assertEqual(matched['key'], key)
        self.assertEqual(matched['byte'], len(prefix.encode()))
        child.output.clear()
        child.write(b'v13ly')
        child.repaint_until(b'Yanked')
        child.write(b'P')
        self.assertEqual(frontend.rollout(self.save(child)['buffers'][0])['draft'],
                         'anchor-marker-')
        child.write(b'u\tG')
        values = self.wait_snapshot(lambda rows: any(
            window['history']['follow'] and
            window['history']['key'].startswith('presentation:')
            for window in next(iter(rows.values()))['state']['windows']))
        self.assertTrue(values)
        self.assertNotIn(b'irc-ui-secret', child.output)
        child.finish('qa')

    def test_retained_anchor_survives_new_secret(self):
        self.retained_secret_anchor(False)

    def test_legacy_retained_anchor_survives_new_secret(self):
        self.retained_secret_anchor(True)

    def retained_secret_anchor(self, legacy):
        from test_vm_mouse import current_rows

        journal = self.seed_session('setup', legacy=legacy)
        secret = 'newly-protected-retained-value-' + 'x' * 32
        body = (secret + '\n') * 40 + ''.join(
            f'retained-row-{i:03d}\n' for i in range(80))
        config = self.root / 'state' / 'config.ini'
        config.write_text(self.fixture_config.read_text().replace(
            '${SNAJPAGENT_IRC_UI_KEY}', '"irc-ui-secret"'))
        config.chmod(0o600)
        previous = max(event['seq'] for event in read_events(journal))
        provider = self.fixture_provider

        def respond(handler, request, sequence):
            provider.reply(handler, provider.response_body(sequence, body).encode())

        provider.runtime_handler = respond
        owner = frontend.Terminal(self.root, ['--config', str(config), '--resume',
            journal.parent.name], subcommand=None)
        self.addCleanup(owner.close)
        owner.until(b'session id')
        owner.write(b'generate retained stream\r')
        deadline = time.monotonic() + 5
        while not any(event['seq'] > previous and event['type'] == 'turn_completed'
                      for event in read_events(journal)):
            if time.monotonic() >= deadline:
                self.fail('retained stream did not complete')
            owner.read(.05)
        owner.write(b'/exit\r')
        owner.wait_exit()
        self.assertEqual(owner.state()['returncode'], 0)
        self.assertTrue((journal.parent / '.view-presentation.snb').is_file())

        child = self.start('-N', 'retained-anchor', rows=24, columns=88)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'retained-row-079')
        child.write(b'30k')
        child.repaint_until(b'retained-row-040')
        anchor = self.save(child)['windows'][0]['history']
        self.assertTrue(anchor['key'].startswith('presentation:'), anchor)
        self.assertTrue(anchor['source'])
        self.assertFalse(anchor['follow'])
        before = [line.strip() for line in current_rows(child).values()
                  if 'retained-row-' in line]
        self.assertTrue(before)
        child.finish('workspace detach')
        config.write_text(config.read_text() + f'\n[tool]\nsecret = "{secret}"\n')
        child = self.start('--resume', 'retained-anchor', rows=24, columns=88,
                           expect=b'Workspace restored')
        child.repaint_until(before[0].encode())
        after = [line.strip() for line in current_rows(child).values()
                 if 'retained-row-' in line]
        self.assertEqual(after, before)
        self.assertEqual(self.save(child)['windows'][0]['history'], anchor)
        self.assertNotIn(secret.encode(), child.output)
        child.finish('workspace detach')

    def test_hidden_tool_anchor_returns_after_verbosity_change(self):
        self.hidden_tool_anchor(False)

    def test_whole_hidden_page_anchor_returns_after_verbosity_change(self):
        self.hidden_tool_anchor(True)

    def hidden_tool_anchor(self, large):
        command = ("python3 -c \"import sys; sys.stdout.write("
                   "'selected-tool-marker\\n' * 500000)\"") if large else (
                       "printf 'before\\nselected-tool-marker\\nafter-after-after-after\\n'")
        provider = frontend.harness.FakeResponses()
        self.addCleanup(provider.close)
        first = True

        def respond(handler, request, sequence):
            nonlocal first
            if first:
                first = False
                body = provider.function_body(sequence, 'anchor-call', 'exec_command', {
                    'cmd': command, 'yield_time_ms': 10000, 'max_output_tokens': 100})
            else:
                jobs = frontend.harness.unsettled_commands(request)
                if jobs:
                    body = provider.functions_body(sequence, [
                        (f'anchor-poll-{sequence}-{i}', 'write_stdin', {
                            'handle': job['handle'], 'yield_ms': 1000,
                            'max_output_bytes': 100}) for i, job in enumerate(jobs)])
                else:
                    body = provider.response_body(sequence, 'final-answer')
            provider.reply(handler, body.encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        frontend.harness.write_irc_config(config, provider.port, 'host-model')
        result = subprocess.run([str(frontend.BINARY), '--config', str(config), '--dotdir',
                                 str(self.root / 'state'), '-e', '--', 'run fixture'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-4000:])
        journal, = journal_paths(self.root / 'state')
        event = next(event for event in read_events(journal)
                     if event['type'] == 'process_output' and
                     (not large or event['data']['offset'] >= 5 * 1024 * 1024))
        child = self.start('-N', 'hidden', columns=100)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'final-answer')
        child.finish('qa')
        path, = (self.root / 'state' / 'workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        saved['state']['windows'][0]['history'].update(
            key=f"event/{event['seq']}/output", seq=event['seq'],
            byte=event['data']['offset'] + (0 if large else 7), heading=False,
            follow=False, source=True, verbosity=3)
        path.write_text(json.dumps(saved))
        child = self.start('--resume', 'hidden', expect=b'Workspace restored')
        child.repaint_until(b'selected-tool-marker')
        before = self.save(child)['windows'][0]['history']
        child.command('verbosity 0')
        child.repaint_until(b'final-answer')
        hidden = self.save(child)['windows'][0]['history']
        self.assertEqual(hidden, dict(before, verbosity=0))
        child.command('verbosity 3')
        child.repaint_until(b'selected-tool-marker')
        self.assertEqual(self.save(child)['windows'][0]['history'], before)
        child.finish('qa')


if __name__ == '__main__':
    unittest.main()
