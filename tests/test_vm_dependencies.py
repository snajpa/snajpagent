# SPDX-License-Identifier: GPL-2.0-only
"""Tool descriptions survive history page boundaries without owner replay."""

import json
import os
import subprocess
import unittest

import test_vm_frontend as frontend


class DependencyTests(unittest.TestCase):
    setUp = frontend.WorkspaceTests.setUp
    start = frontend.WorkspaceTests.start

    def test_stored_tool_name_is_resolved_across_a_full_output_page(self):
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
                body = provider.response_body(sequence, 'after-large-tool-output')
            provider.reply(handler, body.encode())

        provider.runtime_handler = respond
        config = self.root / 'config.ini'
        frontend.harness.write_irc_config(config, provider.port, 'host-model')
        result = subprocess.run([str(frontend.BINARY), '--config', str(config), '--dotdir',
                                 str(self.root / 'state'), '-e', '--', 'run the fixture tool'],
                                cwd=self.root, env={**os.environ, 'HOME': str(self.root),
                                                   'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'},
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-4000:])
        journal, = (self.root / 'state' / 'sessions').glob('*/events.jsonl')
        original = journal.read_bytes()
        events = [json.loads(line) for line in original.splitlines()]
        start = next(event for event in events if event['type'] == 'tool_started')
        finish = next(event for event in events if event['type'] == 'tool_finished')
        between = sum(len(line) + 1 for line in original.splitlines()
                      if start['seq'] <= json.loads(line)['seq'] <= finish['seq'])
        self.assertGreater(between, 4 * 1024 * 1024)
        child = self.start('-N', 'dependencies', columns=150)
        child.command('history ' + journal.parent.name)
        child.repaint_until(b'after-large-tool-output')
        child.command('verbosity 1')
        child.repaint_until(b'exec_command [')
        child.command('verbosity 0')
        child.repaint_until(b'after-large-tool-output')
        child.command('verbosity 1')
        child.repaint_until(b'exec_command [')
        child.command('verbosity 2')
        child.repaint_until(b'preview-start')
        self.assertNotIn(b'irc-ui-secret', child.output)
        child.finish('close')
        self.assertEqual(journal.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
