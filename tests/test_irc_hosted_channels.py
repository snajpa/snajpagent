# SPDX-License-Identifier: GPL-2.0-only
"""Hosted room selectors use the same commands and exact routes as clients."""

import json
import unittest

from test_irc_queries import QueryFixture
from tmux_terminal import read_events


class HostedChannelTests(QueryFixture):
    def channels(self):
        result = {}
        for event in self.events():
            route = event['data'].get('routing', {})
            if route.get('conversation_kind') == 'channel':
                result[route['identity']] = event['data']
        return result

    def command(self, text, expected=None):
        self.term.output.clear()
        self.term.write(b'\x15' + text.encode() + b'\r')
        if expected:
            self.term.until(expected.encode())

    def wire(self, text, nick='queryop'):
        self.wait_wire(self.peer, text.encode())
        self.peer.drain(.05)
        rows = [line for line in self.peer.buf.split(b'\r\n') if text.encode() in line]
        self.assertEqual(len(rows), 1, rows)
        self.assertIn(b':' + nick.encode() + b'!', rows[0])
        self.wait_idle()

    def test_channel_commands_and_private_isolation(self):
        self.command('/chat 1/#LAB', f'chat 127.0.0.1:{self.port} #lab')
        self.command('hosted-plain')
        self.wire('PRIVMSG #lab :hosted-plain')
        copies = [e['data'] for e in self.events() if e['data'].get('text') == 'hosted-plain']
        self.assertEqual(sum(e.get('input', False) for e in copies), 1)
        self.command('/notice 1/#lab hosted-notice')
        self.wire('NOTICE #lab :hosted-notice')
        self.command('/me hosted-action')
        self.wire('PRIVMSG #lab :\x01ACTION hosted-action\x01')
        self.command('/names', 'observer')
        self.command('/topic hosted-topic')
        self.wire('TOPIC #lab :hosted-topic')
        self.command('/topic', 'hosted-topic')
        self.command('/join 1/#other', 'only hosts #lab')
        self.command('/query 1/query-peer', 'query query-peer')
        self.command('hosted-private')
        self.wire('PRIVMSG query-peer :hosted-private')
        self.observer.drain(.1)
        self.assertNotIn(b'hosted-private', self.observer.buf)

    def test_part_rejoin_rotates_membership_and_keeps_agent(self):
        self.command('/chat 1/#lab', f'chat 127.0.0.1:{self.port} #lab')
        before = self.channels()['operator']['routing']
        self.command('/part', 'part')
        self.wait(lambda: not self.channels()['operator']['routing']['joined'])
        self.assertTrue(self.channels()['agent']['routing']['joined'])
        self.command('parted-forbidden', 'membership changed')
        self.command('/join 1/#lab')
        self.wait(lambda: self.channels()['operator']['routing']['joined'])
        after = self.channels()['operator']['routing']
        self.assertEqual(before['conversation_id'], after['conversation_id'])
        self.assertNotEqual(before['membership'], after['membership'])
        self.command('rejoined-public')
        self.wire('PRIVMSG #lab :rejoined-public')
        self.peer.drain(.1)
        self.assertNotIn(b'parted-forbidden', self.peer.buf)

    def test_tab_preserves_channel_query_and_rollout_drafts(self):
        self.command('/chat 1/#lab', f'chat 127.0.0.1:{self.port} #lab')
        self.command('/query 1/query-peer', 'query query-peer')
        self.term.write(b'query-draft\t')
        self.term.repaint_until(b'host-model/medium')
        self.term.write(b'rollout-draft\x1b[Z')
        self.term.repaint_until(b'query-draft')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'channel #lab operator')
        self.term.write(b'channel-draft\t')
        self.term.repaint_until(b'query-draft')
        self.term.write(b'\r')
        self.wire('PRIVMSG query-peer :query-draft')
        self.term.write(b'\x1b[Z\r')
        self.wire('PRIVMSG #lab :channel-draft')
        self.assertNotIn(b'rollout-draft', self.peer.buf)

    def test_model_exact_channel_send(self):
        self.wait(lambda: len(self.channels()) == 2)
        channel = self.channels()['agent']['routing']['conversation_id']
        issued = []

        def respond(handler, request, sequence):
            self.seen.append(request)
            if not issued:
                issued.append(True)
                self.reply_call(handler, sequence, 'hosted', 'channel:' + channel,
                                'hosted-model-action', action=True)
            else:
                wire = self.provider.response_body(sequence, 'hosted fixture done')
                self.provider.reply(handler, wire.encode(), close_header=True)
            handler.close_connection = True

        self.provider.runtime_handler = respond
        self.submit('send-hosted')
        self.wire('PRIVMSG #lab :\x01ACTION hosted-model-action\x01', 'querybot')
        self.assertNotIn('tool_error', json.dumps(self.seen))

    def test_resume_keeps_room_history_and_part_intent(self):
        self.command('/chat 1/#lab', f'chat 127.0.0.1:{self.port} #lab')
        self.command('/me retained-hosted-action')
        self.wire('PRIVMSG #lab :\x01ACTION retained-hosted-action\x01')
        self.command('/part', 'part')
        self.wait(lambda: not self.channels()['operator']['routing']['joined'])
        before = self.channels()['operator']['routing']
        sid = read_events(self.root / 'state')[0].parent.name
        self.command('/exit')
        self.term.wait_exit()
        self.assertEqual(self.term.process.returncode, 0)
        self.peer.close()
        self.observer.close()
        self.term = self.start('--resume', sid)
        self.wait(lambda: self.channels()['operator']['routing']['generation'] >
                  before['generation'])
        after = self.channels()['operator']['routing']
        self.assertFalse(after['joined'])
        self.assertEqual(before['conversation_id'], after['conversation_id'])
        self.assertNotEqual(before['membership'], after['membership'])
        self.peer = self.connect('query-peer')
        self.peer.wait(b'PRIVMSG #lab :\x01ACTION retained-hosted-action\x01', timeout=5)
        self.peer.wait(b' BATCH -', timeout=5)
        self.peer.drain(.1)
        self.assertEqual(self.peer.buf.count(b'retained-hosted-action'), 1)
        self.command('/join 1/#lab')
        self.wait(lambda: self.channels()['operator']['routing']['joined'])
        self.command('after-hosted-resume')
        self.wire('PRIVMSG #lab :after-hosted-resume')


if __name__ == '__main__':
    unittest.main()
