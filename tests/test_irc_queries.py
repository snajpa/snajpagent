# SPDX-License-Identifier: GPL-2.0-only
"""Private IRC admission and exact reply routing through real PTYs and HTTP."""

import json
import re
import tempfile
import threading
import time
import unittest
from pathlib import Path

from irc_client import IRCClient
from test_upload_client import FixtureChildren
from test_vm_frontend import Terminal
from tmux_terminal import FakeResponses, free_loopback_port, irc_workspace, read_events


class QueryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snag-query-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.provider = FakeResponses()
        self.addCleanup(self.provider.close)
        self.workspace, self.config = irc_workspace(
            self.root / 'workspace', self.provider.port, 'host-model')
        self.config.write_text(self.config.read_text().replace(
            'idle_timeout_ms = 3000', 'idle_timeout_ms = 15000').replace(
            'request_timeout_ms = 5000', 'request_timeout_ms = 20000'))
        self.port = free_loopback_port()
        self.seen = []
        self.issued = set()
        self.release = threading.Event()
        self.held = threading.Event()
        self.addCleanup(self.release.set)
        self.provider.runtime_handler = self.respond
        self.terminals = []
        self.peers = []
        self.term = self.start('-s', f'127.0.0.1:{self.port}', '-n', 'querybot',
                               '-o', 'queryop', '-r', 'lab')
        self.term.until(b'queryop@')
        self.submit('prime')
        self.peer = self.connect('query-peer')
        self.observer = self.connect('observer')
        self.wait(lambda: any(e['type'] == 'irc_event' and
                  e['data'].get('nick') == 'observer' for e in self.events()))
        self.wait_idle()

    def start(self, *args):
        term = Terminal(self.root, ('--config', str(self.config), *args), subcommand=None,
                        extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret', 'PAGER': ''})
        children = FixtureChildren(term.process.pid)
        self.addCleanup(children.close)
        self.addCleanup(term.close)
        self.terminals.append((term, children))
        return term

    def connect(self, nick):
        peer = IRCClient(self.port, nick)
        self.addCleanup(peer.close)
        self.peers.append(peer)
        return peer

    def events(self):
        return read_events(self.root / 'state')[1]

    def wait(self, predicate, timeout=10):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            for term, children in self.terminals:
                children.remember()
                while term.read(0):
                    pass
            if self.provider.failure:
                self.fail(self.provider.failure)
            if predicate():
                return
            time.sleep(.01)
        self.fail(('condition timed out', bytes(self.term.output[-3000:]), self.events()[-3:]))

    def wait_idle(self):
        self.wait(lambda: bool(self.events()) and
                  sum(e['type'] == 'turn_started' for e in self.events()) ==
                  sum(e['type'] in ('turn_completed', 'turn_completed_silent')
                      for e in self.events()))

    def submit(self, text):
        before = sum(e['type'] == 'turn_completed' for e in self.events())
        self.term.write(b'/rollout\r')
        self.term.until(b'host-model/medium')
        self.term.write(text.encode() + b'\r')
        self.wait(lambda: sum(e['type'] == 'turn_completed' for e in self.events()) > before)
        self.wait_idle()

    def direct(self, target, text, notice=False):
        self.peer.sock.sendall(
            f'{"NOTICE" if notice else "PRIVMSG"} {target} :{text}\r\n'.encode())
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and
                  e['data'].get('text') == text for e in self.events()))

    def reply_call(self, handler, sequence, call_id, destination, text, **extra):
        wire = self.provider.function_body(sequence, call_id, 'irc_send',
            dict(destination=destination, text=text, notice=False, **extra))
        self.provider.reply(handler, wire.encode(), close_header=True)

    def respond(self, handler, request, sequence):
        self.seen.append(request)
        prompt = self.provider.latest_user(request)
        all_text = json.dumps(request.get('input', []))
        if 'private-stale' in all_text and 'stale' not in self.issued:
            self.issued.add('stale')
            query = re.search(r'conversation=([0-9a-f]{32}) kind=query', all_text)[1]
            self.held.set()
            assert self.release.wait(15), 'stale query fixture was not released'
            self.reply_call(handler, sequence, 'stale', 'query:' + query, 'stale-reply-forbidden')
        elif 'private-reply' in all_text and 'implicit' not in self.issued:
            self.issued.add('implicit')
            self.reply_call(handler, sequence, 'implicit', None, 'implicit-leak-forbidden')
        elif 'private-reply' in all_text and 'explicit' not in self.issued:
            self.issued.add('explicit')
            query = re.search(r'conversation=([0-9a-f]{32}) kind=query', all_text)[1]
            self.reply_call(handler, sequence, 'explicit', 'query:' + query, 'private-response')
        elif prompt == 'inspect-query-state' and prompt not in self.issued:
            self.issued.add(prompt)
            wire = self.provider.function_body(sequence, 'state', 'irc_state', {})
            self.provider.reply(handler, wire.encode(), close_header=True)
        elif prompt == 'initiate-private' and prompt not in self.issued:
            self.issued.add(prompt)
            self.reply_call(handler, sequence, 'initiate', f'127.0.0.1:{self.port}/observer',
                            'initiated-private-action', action=True)
        elif prompt == 'forbidden-identity' and prompt not in self.issued:
            self.issued.add(prompt)
            event = next(e for e in self.events() if e['type'] == 'irc_event_v2' and
                         e['data'].get('text') == 'operator-secret-marker')
            self.reply_call(handler, sequence, 'wrong-role',
                'query:' + event['data']['routing']['conversation_id'], 'wrong-role-forbidden')
        else:
            self.provider.reply(handler, self.provider.response_body(
                sequence, f'query fixture done {sequence}').encode(), close_header=True)
        handler.close_connection = True

    def assert_private(self, marker):
        self.observer.drain(.1)
        self.assertNotIn(marker.encode(), self.observer.buf)
        public = [e for e in self.events() if e['type'] == 'irc_event']
        self.assertNotIn(marker, json.dumps(public))

    def resume_at(self, sequence):
        # Recreate an interrupted append boundary using the original journal
        # bytes, including hashes and checkpoint pointers, after its owner exits.
        path, _ = read_events(self.root / 'state')
        self.term.write(b'/exit\r')
        self.term.wait_exit()
        self.assertEqual(self.term.process.returncode, 0)
        lines = path.read_bytes().splitlines(keepends=True)
        retained = [line for line in lines if json.loads(line)['seq'] <= sequence]
        self.assertEqual(json.loads(retained[-1])['seq'], sequence)
        path.write_bytes(b''.join(retained))
        self.peer.close()
        self.observer.close()
        self.term = self.start('--resume', path.parent.name)
        self.term.until(b'queryop@')

    def test_admission_identity_and_reply(self):
        self.direct('queryop', 'operator-secret-marker')
        self.direct('querybot', 'private-notice-context', notice=True)
        self.wait_idle()
        self.assertNotIn('operator-secret-marker', json.dumps(self.seen))
        self.assertNotIn('private-notice-context', json.dumps(self.seen))
        self.submit('read-notice-context')
        self.assertIn('private-notice-context', json.dumps(self.seen))
        self.assertNotIn('operator-secret-marker', json.dumps(self.seen))
        self.direct('querybot', 'private-reply')
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and
                  e['data'].get('text') == 'private-response' and
                  e['data']['routing']['state'] == 'acknowledged' for e in self.events()))
        self.peer.wait(b'PRIVMSG query-peer :private-response\r\n', timeout=5)
        self.wait_idle()
        self.assert_private('private-response')
        self.assert_private('implicit-leak-forbidden')
        self.peer.drain(.1)
        self.assertNotIn(b'implicit-leak-forbidden', self.peer.buf)
        self.assertIn('No message was sent', json.dumps(self.seen))
        self.submit('inspect-query-state')
        self.submit('forbidden-identity')
        self.assertNotIn('operator-secret-marker', json.dumps(self.seen))
        self.assertNotIn('wrong-role-forbidden', json.dumps([
            e for e in self.events() if e['type'] in ('irc_event', 'irc_event_v2')]))
        self.submit('initiate-private')
        self.observer.wait(b'PRIVMSG observer :\x01ACTION initiated-private-action\x01\r\n', timeout=5)
        self.assertNotIn('initiated-private-action', json.dumps([
            e for e in self.events() if e['type'] == 'irc_event']))

    def test_rename_during_request_rejects_old_recipient(self):
        self.direct('querybot', 'private-stale')
        self.wait(self.held.is_set)
        self.peer.sock.sendall(b'NICK renamed-peer\r\n')
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and e['data']['kind'] == 'nick'
                  for e in self.events()))
        self.release.set()
        self.wait(lambda: 'query changed' in json.dumps(self.seen))
        self.wait_idle()
        rename = next(e['data'] for e in self.events() if e['type'] == 'irc_event_v2' and
                      e['data']['kind'] == 'nick')
        self.assertTrue(rename['input'])
        self.assertFalse(rename['urgent'])
        self.assertFalse(rename['reply'])
        self.assertIn('renamed-peer', json.dumps(self.seen))
        self.peer.drain(.1)
        self.assertNotIn(b'stale-reply-forbidden', self.peer.buf)
        self.assert_private('stale-reply-forbidden')

    def test_private_message_requires_reply_in_its_query(self):
        self.direct('querybot', 'private-needs-reply')
        source = next(e for e in self.events() if e['type'] == 'irc_event_v2' and
                      e['data'].get('text') == 'private-needs-reply')
        self.assertTrue(source['data']['input'])
        self.assertTrue(source['data']['urgent'])
        self.assertTrue(source['data']['reply'])
        self.wait(lambda: any(e['type'] == 'irc_reply_reminder' for e in self.events()))
        self.wait_idle()
        self.assert_private('private-needs-reply')

    def resume_reply_at(self, boundary):
        self.direct('querybot', 'resume-needs-reply')
        self.wait_idle()
        source = next(e for e in self.events() if e['type'] == 'irc_event_v2' and
                      e['data'].get('text') == 'resume-needs-reply')
        event = next(e for e in self.events() if e['seq'] > source['seq'] and
                     e['type'] == boundary)
        if boundary == 'irc_admitted':
            self.assertIn(source['seq'], event['data']['sequences'])
            self.assertIn('input', event['data'])
        self.resume_at(event['seq'])
        self.wait(lambda: any(e['seq'] > event['seq'] and e['type'] == 'irc_reply_reminder'
                  for e in self.events()))
        self.wait_idle()

    def test_pending_query_reply_survives_resume(self):
        self.resume_reply_at('irc_admitted')

    def test_active_query_reply_survives_resume(self):
        self.resume_reply_at('turn_started')

    def test_notice_received_before_admission_survives_resume_without_reply(self):
        self.direct('querybot', 'notice-before-admission', notice=True)
        self.wait_idle()
        source = next(e for e in self.events() if e['type'] == 'irc_event_v2' and
                      e['data'].get('text') == 'notice-before-admission')
        self.resume_at(source['seq'])
        self.wait(lambda: any(e['type'] == 'irc_admitted' and
                  source['seq'] in e['data']['sequences'] for e in self.events()))
        admission = next(e for e in self.events() if e['type'] == 'irc_admitted' and
                         source['seq'] in e['data']['sequences'])
        self.assertNotIn('input', admission['data'])
        self.assertNotIn('steering', admission['data'])
        self.submit('read-resumed-notice')
        self.assertIn('notice-before-admission', json.dumps(self.seen))
        self.assertFalse(any(e['type'] == 'irc_reply_reminder' for e in self.events()))

    def test_resume_retains_private_history_without_replaying_sends(self):
        self.direct('queryop', 'operator-secret-marker')
        self.direct('querybot', 'private-reply')
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and
                  e['data'].get('text') == 'private-response' and
                  e['data']['routing']['state'] == 'acknowledged' for e in self.events()))
        self.wait_idle()
        previous = next(e['data']['routing'] for e in self.events()
                        if e['type'] == 'irc_event_v2' and e['data'].get('text') == 'private-reply')
        sid = read_events(self.root / 'state')[0].parent.name
        self.term.write(b'/exit\r')
        self.term.wait_exit()
        self.assertEqual(self.term.process.returncode, 0)
        self.peer.close()
        self.observer.close()
        self.term = self.start('--resume', sid)
        self.term.until(b'queryop@')
        self.peer = self.connect('query-peer')
        self.peer.drain(.1)
        self.assertNotIn(b'private-response', self.peer.buf)
        self.direct('querybot', 'private-after-resume')
        current = next(e['data']['routing'] for e in self.events()
                       if e['type'] == 'irc_event_v2' and
                       e['data'].get('text') == 'private-after-resume')
        self.assertEqual(current['connection_id'], previous['connection_id'])
        self.assertGreater(current['generation'], previous['generation'])
        self.assertNotEqual(current['conversation_id'], previous['conversation_id'])
        self.wait_idle()
        self.assertNotIn('operator-secret-marker', json.dumps(self.seen))


if __name__ == '__main__':
    unittest.main()
