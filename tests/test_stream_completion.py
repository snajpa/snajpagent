# SPDX-License-Identifier: GPL-2.0-only
"""Completed message tails appear before the rest of the response finishes."""
import json
import tempfile
import threading
import time
import unittest
from pathlib import Path

from test_upload_client import FixtureChildren
from test_vm_frontend import Terminal
from tmux_terminal import FakeResponses, write_irc_config


class StreamCompletionTests(unittest.TestCase):
    def exercise(self, *, markdown=True, newline=False, refusal=False, tool=False):
        with tempfile.TemporaryDirectory(prefix='snag-stream-completion-') as tmp:
            root = Path(tmp).resolve()
            provider = FakeResponses()
            allow_done = threading.Event()
            allow_response = threading.Event()
            item_done = threading.Event()
            text = 'Visible prefix ending TAILMARKER' + ('\n\n' if newline else '')
            marker = root / 'tool-ran'

            def respond(handler, request, sequence):
                if sequence > 1:
                    provider.reply(handler, provider.response_body(
                        sequence, 'FOLLOWUP_COMPLETE').encode(), close_header=True)
                    return
                event = provider.event
                phase = 'final_answer' if refusal else 'commentary'
                kind = 'refusal' if refusal else 'output_text'
                field = 'refusal' if refusal else 'text'
                position = dict(item_id='message', output_index=0, content_index=0)
                item = dict(id='message', type='message', status='in_progress',
                            role='assistant', phase=phase, content=[])
                first = event('response.created', {'response': dict(
                    id='response', status='in_progress', output=[])})
                first += event('response.output_item.added', dict(output_index=0, item=item))
                first += event('response.content_part.added', dict(
                    **position, part={'type': kind, field: ''}))
                first += event('response.' + kind + '.delta', dict(**position, delta=text))
                part = {'type': kind, field: text}
                item = dict(item, status='completed', content=[part])
                done = event('response.' + kind + '.done', {**position, field: text})
                done += event('response.content_part.done', dict(**position, part=part))
                done += event('response.output_item.done', dict(output_index=0, item=item))
                # A duplicate completed snapshot must not render the tail twice.
                done += event('response.output_item.done', dict(output_index=0, item=item))
                output = [item]
                finish = ''
                if tool:
                    arguments = json.dumps({'command': 'printf ran > tool-ran',
                                            'workdir': str(root)})
                    call = dict(id='function', type='function_call', status='in_progress',
                                call_id='call', name='exec_command', arguments='')
                    done += event('response.output_item.added', dict(output_index=1, item=call))
                    done += event('response.function_call_arguments.delta', dict(
                        output_index=1, item_id='function', delta=arguments[:10]))
                    finish += event('response.function_call_arguments.delta', dict(
                        output_index=1, item_id='function', delta=arguments[10:]))
                    call = dict(call, status='completed', arguments=arguments)
                    finish += event('response.output_item.done', dict(output_index=1, item=call))
                    output.append(call)
                finish += event('response.completed', {'response': dict(
                    id='response', status='completed', output=output,
                    usage=dict(input_tokens=1, output_tokens=5, total_tokens=6))})
                handler.send_response(200)
                handler.send_header('Content-Type', 'text/event-stream')
                handler.send_header('Content-Length', str(len((first + done + finish).encode())))
                handler.send_header('Connection', 'close')
                handler.end_headers()
                handler.wfile.write(first.encode())
                handler.wfile.flush()
                assert allow_done.wait(8), 'fixture message was not released'
                handler.wfile.write(done.encode())
                handler.wfile.flush()
                item_done.set()
                assert allow_response.wait(8), 'fixture response was not released'
                handler.wfile.write(finish.encode())
                handler.wfile.flush()
                handler.close_connection = True

            provider.runtime_handler = respond
            config = root / 'config.ini'
            write_irc_config(config, provider.port, 'host-model')
            with config.open('a') as out:
                out.write('markdown = ' + ('true' if markdown else 'false') + '\n')
            child = Terminal(root, ('--config', str(config)), subcommand=None,
                             extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret', 'PAGER': '',
                                        'OPENAI_ACCESS_TOKEN': None, 'CODEX_HOME': None})
            children = FixtureChildren(child.process.pid)
            try:
                child.until('›'.encode(), 10)
                children.remember()
                child.write(b'finish the message\r')
                child.until(b'Visible prefix', 5)
                if newline:
                    child.until(b'TAILMARKER', 1)
                else:
                    self.assertNotIn(b'TAILMARKER', child.output)
                allow_done.set()
                self.assertTrue(item_done.wait(3), provider.failure)
                # The response and any following tool arguments stay withheld.
                # No later provider event or operator keystroke may release it.
                child.until(b'TAILMARKER', 1)
                self.assertFalse(allow_response.is_set())
                self.assertFalse(marker.exists(), 'tool ran before response completion')
                allow_response.set()
                if tool:
                    child.until(b'FOLLOWUP_COMPLETE', 5)
                    self.assertEqual(marker.read_text(), 'ran')
                else:
                    deadline = time.monotonic() + .2
                    while time.monotonic() < deadline:
                        child.read(.02)
                self.assertEqual(child.output.count(b'TAILMARKER'), 1)
                self.assertFalse(provider.failure, provider.failure)
                child.write(b'/exit\r')
                child.wait_exit()
            finally:
                allow_done.set()
                allow_response.set()
                children.close()
                child.close()
                provider.close()

    def test_completed_markdown_tail(self):
        self.exercise()

    def test_completed_plain_tail(self):
        self.exercise(markdown=False)

    def test_newline_already_releases_tail(self):
        self.exercise(newline=True)

    def test_completed_refusal_tail(self):
        self.exercise(refusal=True)

    def test_tail_precedes_pending_tool_arguments(self):
        self.exercise(tool=True)


if __name__ == '__main__':
    unittest.main()
