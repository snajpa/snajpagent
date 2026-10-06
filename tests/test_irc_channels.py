# SPDX-License-Identifier: GPL-2.0-only
"""Model channel selection preserves request-time membership and wire recipients."""

import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path

from test_irc_queries import QueryFixture
from tmux_terminal import FakeResponses, irc_workspace


class ChannelServer:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(2)
        self.listener.settimeout(.1)
        self.endpoint = '127.0.0.1:' + str(self.listener.getsockname()[1])
        self.links = {}
        self.lines = []
        self.failure = None
        self.stopping = threading.Event()
        self.workers = []
        self.acceptor = threading.Thread(target=self.accept, daemon=True)
        self.acceptor.start()

    def accept(self):
        while not self.stopping.is_set():
            try:
                link, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            link.settimeout(.1)
            worker = threading.Thread(target=self.serve, args=(link,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def serve(self, link):
        nick = None
        pending = b''
        try:
            while not self.stopping.is_set():
                try:
                    chunk = link.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                pending += chunk
                while b'\r\n' in pending:
                    raw, pending = pending.split(b'\r\n', 1)
                    line = raw.decode()
                    self.lines.append((nick, line))
                    if line.startswith('NICK '):
                        nick = line[5:]
                        self.links[nick] = link
                    elif line.startswith('USER '):
                        self.send(nick, f':fake 001 {nick} :welcome\r\n'
                            f':fake 005 {nick} SAJROOM=#lab CASEMAPPING=rfc1459 :supported\r\n'
                            f':fake 376 {nick} :end\r\n')
                    elif line == 'JOIN #lab':
                        # Server-forced joins also exercise non-default rooms.
                        self.send(nick, f':{nick}!u@fake JOIN #lab\r\n'
                            f':{nick}!u@fake JOIN #side\r\n')
        except (OSError, UnicodeError) as exc:
            if not self.stopping.is_set():
                self.failure = repr(exc)
        finally:
            link.close()

    def send(self, nick, text):
        self.links[nick].sendall(text.encode())

    def close(self):
        self.stopping.set()
        self.listener.close()
        self.acceptor.join(2)
        for worker in self.workers:
            worker.join(2)
        assert not self.acceptor.is_alive()
        assert not any(worker.is_alive() for worker in self.workers)


class ChannelTests(QueryFixture):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snag-channel-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.provider = FakeResponses()
        self.addCleanup(self.provider.close)
        self.workspace, self.config = irc_workspace(
            self.root / 'workspace', self.provider.port, 'host-model')
        self.config.write_text(self.config.read_text().replace(
            'idle_timeout_ms = 3000', 'idle_timeout_ms = 15000').replace(
            'request_timeout_ms = 5000', 'request_timeout_ms = 20000'))
        self.server = ChannelServer()
        self.addCleanup(self.server.close)
        self.seen = []
        self.calls = []
        self.call_number = 0
        self.release = threading.Event()
        self.held = threading.Event()
        self.addCleanup(self.release.set)
        self.provider.runtime_handler = self.respond
        self.terminals = []
        self.term = self.start('--no-listen', '-c', self.server.endpoint,
            '-n', 'querybot', '-o', 'queryop', '-N', 'channel-session', '-r', 'lab')
        self.wait(lambda: len(self.channels()) == 4)
        self.wait_idle()

    def channels(self):
        result = {}
        for event in self.events():
            route = event['data'].get('routing', {})
            if route.get('conversation_kind') == 'channel':
                result[(route['identity'], event['data']['room'])] = event['data']
        return result

    def selector(self, room='#side', identity='agent'):
        return 'channel:' + self.channels()[(identity, room)]['routing']['conversation_id']

    def respond(self, handler, request, sequence):
        self.seen.append(request)
        prompt = self.provider.latest_user(request)
        if prompt == 'channel-check' and self.calls:
            call = self.calls.pop(0)
            if not self.release.is_set():
                self.held.set()
                assert self.release.wait(15), 'channel fixture was not released'
            self.call_number += 1
            wire = self.provider.function_body(sequence, 'channel-' + str(self.call_number),
                call[0], call[1])
        else:
            wire = self.provider.response_body(sequence, 'channel fixture done')
        self.provider.reply(handler, wire.encode(), close_header=True)
        handler.close_connection = True

    def run_calls(self, *calls):
        self.calls = list(calls)
        self.release.set()
        self.submit('channel-check')
        self.assertFalse(self.calls)
        self.assertIsNone(self.server.failure)

    def send_call(self, destination, text, **extra):
        return ('irc_send', dict(destination=destination, text=text, **extra))

    def assert_wire(self, line):
        self.wait(lambda: ('querybot', line) in self.server.lines)
        self.assertEqual([nick for nick, body in self.server.lines if body == line], ['querybot'])

    def assert_no_wire(self, marker):
        self.assertFalse(any(marker in line for _, line in self.server.lines), self.server.lines)

    def test_exact_id_address_action_notice_and_topic(self):
        self.run_calls(
            self.send_call(self.selector(), 'exact-channel-message'),
            self.send_call('channel-session/' + self.server.endpoint + '/#SIDE',
                           'named-channel-action', action=True),
            self.send_call('1/#side', 'channel-notice', notice=True),
            ('irc_topic', dict(destination=self.selector(), topic='specific-channel-topic')))
        self.assert_wire('PRIVMSG #side :exact-channel-message')
        self.assert_wire('PRIVMSG #side :\x01ACTION named-channel-action\x01')
        self.assert_wire('NOTICE #side :channel-notice')
        self.assert_wire('TOPIC #side :specific-channel-topic')
        self.assertFalse(any('#lab' in line and 'channel-' in line
                             for _, line in self.server.lines))

    def test_state_lists_agent_channels_and_blocks_implicit_channel(self):
        self.run_calls(('irc_state', {}), self.send_call(None, 'implicit-forbidden'),
            ('irc_topic', dict(destination=None, topic='implicit-topic-forbidden')),
            self.send_call('1', 'explicit-default'), self.send_call('all', 'explicit-broadcast'))
        context = json.dumps(self.seen)
        self.assertIn(self.selector(), context)
        self.assertIn('room=#side', context)
        self.assertNotIn(self.selector(identity='operator'), context)
        self.assertIn('No command was sent', context)
        self.assert_no_wire('implicit-forbidden')
        self.assert_no_wire('implicit-topic-forbidden')
        self.assert_wire('PRIVMSG #lab :explicit-default')
        self.assert_wire('PRIVMSG #lab :explicit-broadcast')

    def test_wrong_role_missing_room_and_session_never_fall_back(self):
        self.run_calls(
            self.send_call(self.selector(identity='operator'), 'wrong-role-forbidden'),
            self.send_call(self.server.endpoint + '/#missing', 'missing-forbidden'),
            self.send_call('another-session/1/#side', 'wrong-session-forbidden'),
            self.send_call('channel:invalid', 'invalid-id-forbidden'),
            self.send_call(self.selector(), 'notice-action-forbidden', notice=True, action=True))
        self.assert_no_wire('forbidden')
        self.assertFalse(any(line.startswith('JOIN #missing') for _, line in self.server.lines))
        context = json.dumps(self.seen)
        self.assertIn('unavailable for this identity', context)
        self.assertIn('addressed session', context)
        self.assertIn('invalid channel conversation ID', context)

    def test_bare_channel_requires_unique_endpoint(self):
        second = ChannelServer()
        self.addCleanup(second.close)
        self.run_calls(('irc_connect', dict(endpoint=second.endpoint)))
        self.wait(lambda: len({e['data']['endpoint'] for e in self.events()
                  if e['data'].get('routing', {}).get('joined')}) == 2)
        self.run_calls(self.send_call('#side', 'ambiguous-forbidden'),
                       self.send_call(second.endpoint + '/#side', 'second-endpoint-send'))
        self.assert_no_wire('ambiguous-forbidden')
        self.assertFalse(any('ambiguous-forbidden' in line for _, line in second.lines))
        self.wait(lambda: ('querybot', 'PRIVMSG #side :second-endpoint-send') in second.lines)
        self.assert_no_wire('second-endpoint-send')

    def stale_send(self, address, *, reconnect=False, topic=False):
        previous = self.channels()[('agent', '#side')]['routing']
        self.calls = [('irc_topic', dict(destination=address, topic='stale-channel-forbidden'))
                      if topic else self.send_call(address, 'stale-channel-forbidden')]
        self.term.write(b'/rollout\rchannel-check\r')
        self.wait(self.held.is_set)
        if reconnect:
            self.server.links['querybot'].shutdown(socket.SHUT_RDWR)
        else:
            self.server.send('querybot', ':fake KICK #side querybot :removed\r\n'
                             ':querybot!u@fake JOIN #side\r\n')
        field = 'generation' if reconnect else 'membership'
        self.wait(lambda: self.channels()[('agent', '#side')]['routing'][field] !=
                  previous[field] and
                  self.channels()[('agent', '#side')]['routing']['joined'])
        if reconnect:
            self.assertGreater(self.channels()[('agent', '#side')]['routing']['generation'],
                               previous['generation'])
        self.release.set()
        self.wait(lambda: 'membership changed' in json.dumps(self.seen))
        self.wait_idle()
        self.assert_no_wire('stale-channel-forbidden')
        # A subsequent request explicitly captures the new joined membership.
        self.run_calls(self.send_call(address, 'fresh-channel-send'))
        self.assert_wire('PRIVMSG #side :fresh-channel-send')

    def test_exact_id_is_stale_after_kick_and_rejoin(self):
        self.stale_send(self.selector())

    def test_address_is_stale_after_kick_and_rejoin(self):
        self.stale_send(self.server.endpoint + '/#SIDE')

    def test_topic_is_stale_after_kick_and_rejoin(self):
        self.stale_send(self.selector(), topic=True)

    def test_channel_is_stale_after_reconnect(self):
        self.stale_send(self.selector(), reconnect=True)


if __name__ == '__main__':
    unittest.main()
