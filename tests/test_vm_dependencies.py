# SPDX-License-Identifier: GPL-2.0-only
"""Tool descriptions survive history page boundaries without owner replay."""

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

    def check_dependency(self, legacy):
        provider = frontend.harness.FakeResponses()
        self.addCleanup(provider.close)
        first = True

        def respond(handler, request, sequence):
            nonlocal first
            if first:
                first = False
                body = provider.function_body(sequence, 'cross-page-call', 'exec_command', {
                    'cmd': ("python3 -c \"import sys; sys.stdout.write("
                            "'preview-start\\nirc-ui-secret\\n' + 'x' * (5 * 1024 * 1024))\""),
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
        original = journal.read_bytes()
        events = read_events(journal)
        start = next(event for event in events if event['type'] == 'tool_started')
        finish = next(event for event in events if event['type'] == 'tool_finished')
        between = sum(len(json.dumps(event).encode()) + 1 for event in events
                      if start['seq'] <= event['seq'] <= finish['seq'])
        self.assertGreater(between, 4 * 1024 * 1024)
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
