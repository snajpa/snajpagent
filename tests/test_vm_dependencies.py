# SPDX-License-Identifier: GPL-2.0-only
"""Tool descriptions survive history page boundaries without owner replay."""

import base64
import json
import os
import subprocess
import unittest

import test_vm_frontend as frontend
from store_history import create_legacy, journal_paths, read_events


class DependencyTests(unittest.TestCase):
    setUp = frontend.WorkspaceTests.setUp
    start = frontend.WorkspaceTests.start

    def test_stored_tool_name_is_resolved_across_a_full_output_page(self):
        self.check_dependency(False)

    def test_legacy_tool_name_is_resolved_across_a_full_output_page(self):
        self.check_dependency(True)

    def test_converted_tool_preview_resolves_native_record_range(self):
        self.check_dependency(True, convert=True)

    def test_native_streamed_tool_output(self):
        self.check_streamed_output()

    def test_legacy_streamed_tool_output(self):
        self.check_streamed_output(legacy=True)

    def test_native_streamed_tool_output_truncation(self):
        self.check_streamed_output(truncated=True)

    def test_legacy_streamed_tool_output_truncation(self):
        self.check_streamed_output(legacy=True, truncated=True)

    def test_native_streamed_tool_output_empty(self):
        self.check_streamed_output(empty=True)

    def test_native_retained_output_uses_current_secrets(self):
        self.check_retained_secrets(False)

    def test_legacy_retained_output_uses_current_secrets(self):
        self.check_retained_secrets(True)

    def check_retained_secrets(self, legacy):
        provider = frontend.harness.FakeResponses()
        self.addCleanup(provider.close)
        secret = 'later-protected-value'
        command = ("python3 -c \"import os; "
                   f"os.write(1, b'output-before {secret} output-after\\n'); "
                   f"os.write(2, b'\\x00{secret}\\n')\"")

        def respond(handler, request, sequence):
            if sequence == 1:
                body = provider.function_body(sequence, 'retained-output', 'exec_command', {
                    'command': command, 'yield_ms': 1000, 'max_output_bytes': 10000})
            else:
                jobs = frontend.harness.unsettled_commands(request)
                body = provider.functions_body(sequence, [
                    (f'collect-{sequence}-{i}', 'write_stdin', {
                        'handle': job['handle'], 'yield_ms': 1000})
                    for i, job in enumerate(jobs)]) if jobs else provider.response_body(
                        sequence, 'retained-output-finished')
            provider.reply(handler, body.encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        frontend.harness.write_irc_config(config, provider.port, 'host-model')
        resume = []
        if legacy:
            journal = create_legacy(self.root / 'state', self.root, 'fake', 'host-model')
            resume = ['--resume', journal.parent.name]
        owner = frontend.Terminal(self.root, ['--config', str(config), *resume],
                                  subcommand=None,
                                  extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'})
        self.addCleanup(owner.close)
        owner.until(b'host-model')
        owner.write(b'run output\r')
        owner.until(b'retained-output-finished')
        owner.write(b'/exit\r')
        owner.wait_exit()
        self.assertEqual(owner.process.returncode, 0, owner.output)
        journal, = journal_paths(self.root / 'state')
        self.assertTrue((journal.parent / '.view-presentation.snb').is_file())
        original = journal.read_bytes()
        chunks = [event['data'] for event in read_events(journal)
                  if event['type'] == 'process_output']
        encoded = [chunk['data'] for chunk in chunks if chunk['encoding'] == 'base64']
        self.assertTrue(encoded)
        stderr = b''.join(base64.b64decode(chunk['data'])
                          if chunk['encoding'] == 'base64' else chunk['data'].encode()
                          for chunk in chunks if chunk['stream'] == 1)
        self.assertIn(secret.encode(), stderr)
        # A newly configured secret must protect older output through the actual
        # retained-presentation reader, including payloads stored as base64.
        (self.root / 'state' / 'config.ini').write_text(config.read_text() +
            f'\n[tool]\nsecret = "{secret}"\n')
        (self.root / 'state' / 'config.ini').chmod(0o600)
        child = self.start('-N', 'retained-secrets', rows=40, columns=200,
                          extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'})
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'retained-output-finished')
        child.command('verbosity 3')
        child.repaint_until(b'output-after')
        from test_vm_mouse import current_rows
        visible = '\n'.join(current_rows(child).values())
        self.assertNotIn(secret, visible)
        for text in encoded:
            self.assertNotIn(text, visible)
        self.assertIn('<redacted:secret>', visible)
        child.finish('workspace detach')
        self.assertEqual(journal.read_bytes(), original)

    def check_streamed_output(self, legacy=False, truncated=False, empty=False):
        provider = frontend.harness.FakeResponses()
        self.addCleanup(provider.close)
        first = True
        command = "printf 'native-stream-output\\n'; printf 'native-stream-error\\n' >&2"
        if truncated:
            command = "python3 -c \"import sys; sys.stdout.write('z' * 4096)\""
        elif empty:
            command = 'true'

        def respond(handler, request, sequence):
            nonlocal first
            if first:
                first = False
                body = provider.function_body(sequence, 'native-output-call', 'exec_command', {
                    'cmd': command,
                    'yield_time_ms': 10000, 'max_output_tokens': 1000 if truncated else 100})
            else:
                jobs = frontend.harness.unsettled_commands(request)
                if jobs:
                    body = provider.functions_body(sequence, [
                        (f'native-output-poll-{sequence}-{i}', 'write_stdin', {
                            'handle': job['handle'], 'yield_ms': 1000,
                            'max_output_bytes': 100}) for i, job in enumerate(jobs)])
                else:
                    body = provider.response_body(sequence, 'native-output-finished')
            provider.reply(handler, body.encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        frontend.harness.write_irc_config(config, provider.port, 'host-model')
        resume = []
        if legacy:
            journal = create_legacy(self.root / 'state', self.root, 'fake', 'host-model')
            resume = ['--resume', journal.parent.name]
        result = subprocess.run([str(frontend.BINARY), '--config', str(config), '--dotdir',
                                 str(self.root / 'state'), *resume, '-vv', '-e', '--',
                                 'stream native output'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        text = result.stdout + result.stderr
        self.assertIn('native-output-finished', text)
        # Redaction lookahead may split a stream into several durable records.
        # Each display block adds a line boundary; compare their ordered pieces.
        displayed = {'stdout': [], 'stderr': []}
        stream = None
        for line in text.splitlines():
            if line.startswith('[') and line.endswith(' stdout]'):
                stream = 'stdout'
            elif line.startswith('[') and line.endswith(' stderr]'):
                stream = 'stderr'
            elif line == '  output:' or line.startswith(('←', '→', '•')):
                stream = None
            elif stream is not None:
                displayed[stream].append(line)
        stdout = ''.join(displayed['stdout'])
        stderr = ''.join(displayed['stderr'])
        if truncated:
            shown = 512 - len('[00000000 stdout]\n')
            self.assertEqual(stdout.replace('[…]', ''), 'z' * shown, text)
            self.assertIn('[…]', text)
            self.assertEqual(stderr, '')
        elif empty:
            self.assertEqual((stdout, stderr), ('', ''), text)
        else:
            self.assertEqual(stdout, 'native-stream-output', text)
            self.assertEqual(stderr, 'native-stream-error', text)
        self.assertEqual(text.count('← exec_command'), 1, text)
        journal, events = frontend.harness.read_events(self.root / 'state')
        self.assertEqual(journal.name, 'events.jsonl' if legacy else 'journal.bin')
        finishes = [e for e in events if e['type'] == 'tool_finished' and
                    e['data']['result'].get('output_ref')]
        self.assertTrue(finishes)
        self.assertGreater(events[-1]['seq'], finishes[-1]['seq'])

    def check_dependency(self, legacy, convert=False):
        provider = frontend.harness.FakeResponses()
        self.addCleanup(provider.close)
        first = True
        output_bytes = 5 * 1024 * 1024

        def respond(handler, request, sequence):
            nonlocal first
            if first:
                first = False
                body = provider.function_body(sequence, 'cross-page-call', 'exec_command', {
                    'cmd': ("python3 -c \"import sys; sys.stdout.write("
                            f"'preview-start\\nirc-ui-secret\\n' + 'x' * {output_bytes})\""),
                    'yield_time_ms': 10000, 'max_output_tokens': 100})
            else:
                jobs = frontend.harness.unsettled_commands(request)
                if jobs:
                    body = provider.functions_body(sequence, [
                        (f'dependency-poll-{sequence}-{i}', 'write_stdin', {
                            'handle': job['handle'], 'yield_ms': 1000,
                            'max_output_bytes': 100}) for i, job in enumerate(jobs)])
                else:
                    body = provider.response_body(sequence, 'after-large-tool-output')
            provider.reply(handler, body.encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        frontend.harness.write_irc_config(config, provider.port, 'host-model')
        resume = []
        if legacy:
            journal = create_legacy(self.root / 'state', self.root, 'fake', 'host-model')
            resume = ['--resume', journal.parent.name]
        result = subprocess.run([str(frontend.BINARY), '--config', str(config), '--dotdir',
                                 str(self.root / 'state'), *resume, '-e', '--', 'run the fixture tool'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-4000:])
        journal, = journal_paths(self.root / 'state')
        if convert:
            legacy_bytes = journal.read_bytes()
            converted = subprocess.run([str(frontend.BINARY), 'convert', '--dotdir',
                                        str(self.root / 'state'), '--jobs', '1'],
                                       capture_output=True, timeout=30)
            self.assertEqual(converted.returncode, 0, converted.stderr)
            self.assertEqual((journal.parent / '.legacy-source' / 'events.jsonl').read_bytes(),
                             legacy_bytes)
            journal = journal.parent / 'journal.bin'
        original = journal.read_bytes()
        events = read_events(journal)
        start = next(event for event in events if event['type'] == 'tool_started')
        finish = next(event for event in events if event['type'] == 'tool_finished')
        between = sum(len(json.dumps(event).encode()) + 1 for event in events
                      if start['seq'] <= event['seq'] <= finish['seq'])
        self.assertGreater(between, output_bytes)
        # The history viewer resolves known credentials from the state directory
        # configuration; a command's literal arguments are otherwise ordinary text.
        (self.root / 'state' / 'config.ini').write_text(config.read_text())
        child = self.start('-N', 'dependencies', columns=150,
                           extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'})
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'after-large-tool-output')
        child.command('verbosity 1')
        child.repaint_until(b'exec_command [')
        child.command('verbosity 0')
        child.repaint_until(b'after-large-tool-output')
        child.command('verbosity 1')
        child.repaint_until(b'exec_command [')
        child.command('verbosity 2')
        # A yielded command can have later write_stdin previews at the tail.
        # Inspect the exact exec completion whose output spans the page.
        heading = 'exec_command [' + finish['data']['call_id'][:8]
        child.write(('?' + heading + '\r').encode())
        child.repaint_until(b'Match')
        child.repaint_until(b'preview-start')
        self.assertNotIn(b'irc-ui-secret', child.output)
        child.finish('close')
        self.assertEqual(journal.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
