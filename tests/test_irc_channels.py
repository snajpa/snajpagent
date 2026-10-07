# SPDX-License-Identifier: GPL-2.0-only
"""Model channel selection preserves request-time membership and wire recipients."""

import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path

from test_irc_queries import QueryFixture
from tmux_terminal import FakeResponses, irc_workspace, read_events


class ChannelServer:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(2)
        self.listener.settimeout(.1)
        self.endpoint = '127.0.0.1:' + str(self.listener.getsockname()[1])
        self.links = {}
        self.lines = []
        self.defer_operator_join = False
        self.chantypes = '#&+!'
        self.capabilities = ''
        self.rejected_nick = None
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
                    if line.startswith('CAP LS') and self.capabilities:
                        link.sendall((':fake CAP * LS :' + self.capabilities + '\r\n').encode())
                    elif line.startswith('CAP REQ :') and self.capabilities:
                        self.send(nick, f':fake CAP {nick} ACK :{line[9:]}\r\n')
                    elif line.startswith('NICK '):
                        if nick and line[5:] == self.rejected_nick:
                            self.send(nick, f':fake 433 {nick} {line[5:]} :Nick is in use\r\n')
                            continue
                        if nick:
                            self.send(nick, f':{nick}!u@fake {line}\r\n')
                            del self.links[nick]
                        nick = line[5:]
                        self.links[nick] = link
                    elif ((line.startswith('USER ') and not self.capabilities) or
                          (line == 'CAP END' and self.capabilities)):
                        self.send(nick, f':fake 001 {nick} :welcome\r\n'
                            f':fake 005 {nick} SAJROOM=#lab CASEMAPPING=rfc1459 CHANTYPES={self.chantypes} :supported\r\n'
                            f':fake 376 {nick} :end\r\n')
                    elif line.startswith('JOIN '):
                        room = line[5:]
                        if room == '#lab' and nick == 'queryop' and self.defer_operator_join:
                            continue
                        # Server-forced joins also exercise non-default rooms.
                        self.send(nick, f':{nick}!u@fake JOIN {room}\r\n')
                        if room == '#lab':
                            self.send(nick, f':{nick}!u@fake JOIN #side\r\n')
                    elif line.startswith('PART '):
                        self.send(nick, f':{nick}!u@fake {line}\r\n')
                    elif line.startswith('NAMES '):
                        room = line[6:]
                        self.send(nick, f':fake 353 {nick} = {room} :@{nick} channel-peer\r\n'
                            f':fake 366 {nick} {room} :end\r\n')
                    elif line.startswith('TOPIC '):
                        room = line[6:].split(' ', 1)[0]
                        if ' :' in line:
                            self.send(nick, f':{nick}!u@fake {line}\r\n')
                        else:
                            self.send(nick, f':fake 332 {nick} {room} :fixture topic\r\n')
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


class ChannelFixture(QueryFixture):
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
        self.server.chantypes = getattr(self, 'chantypes', '#&+!')
        self.server.capabilities = getattr(self, 'capabilities', '')
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

    def channel_message(self, room, text, *, local=False, recipients=None):
        prefix = '@saj-id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:1;saj-op=1 ' if local else ''
        sender = 'queryop' if local else 'peer'
        for nick in recipients or ('querybot', 'queryop'):
            self.server.send(nick, f'{prefix}:{sender}!u@fake PRIVMSG {room} :{text}\r\n')
        self.wait(lambda: any(e['data'].get('text') == text for e in self.events()))
        self.wait(lambda: text in json.dumps(self.seen))
        self.wait_idle()
        return [e for e in self.events() if e['data'].get('text') == text and
                e['type'] in ('irc_event', 'irc_event_v2')]

    def command(self, text, expected=None):
        self.term.output.clear()
        self.term.write(b'\x15' + text.encode() + b'\r')
        if expected:
            self.term.until(expected.encode())

    def operator_wire(self, line):
        self.wait(lambda: ('queryop', line) in self.server.lines)
        self.assertEqual([nick for nick, body in self.server.lines if body == line], ['queryop'])


class ChannelTests(ChannelFixture):
    def test_native_durable_response_and_repaint(self):
        self.submit("native durable initial response")
        path, before = read_events(self.root / 'state')
        self.assertEqual(path.name, 'journal.bin')
        self.assertTrue(any(e['type'] == 'response_completed' for e in before))
        saved = path.read_bytes()
        self.submit('native durable repaint')
        self.term.write(b'/chat\r')
        self.term.until(b'queryop@')
        self.term.write(b'/rollout\r')
        self.term.until(b'host-model/medium')
        self.wait_idle()
        self.assertTrue(path.read_bytes().startswith(saved))
        self.assertTrue(self.term.process.poll() is None)

    def test_received_actions_keep_body_and_kind_in_live_and_history(self):
        cases = [('', 'live-action', True, False),
                 ('@batch=old ', 'history-action', True, True),
                 ('@batch=old;saj-id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:21;'
                  'saj-kind=message;saj-op=0 ', 'native-history-action', True, True),
                 ('', 'ACTION ordinary-text', False, False)]
        for tags, body, action, historical in cases:
            with self.subTest(body=body):
                if historical:
                    self.server.send('queryop', ':fake BATCH +old chathistory #side\r\n')
                text = '\x01ACTION ' + body + '\x01' if action else body
                self.server.send('queryop', tags + ':peer!u@fake PRIVMSG #side :' + text + '\r\n')
                if historical:
                    self.server.send('queryop', ':fake BATCH -old\r\n')
                self.wait(lambda: any(body in e['data'].get('text', '') and
                          e['type'] == 'irc_event_v2' for e in self.events()))
                events = [e['data'] for e in self.events() if e['type'] == 'irc_event_v2'
                          and body in e['data'].get('text', '')]
                self.assertEqual(len(events), 1)
                self.assertEqual(events[0]['text'], body)
                self.assertEqual(events[0]['routing']['action'], action)
                self.assertEqual(events[0]['historical'], historical)
                self.assertEqual(events[0]['room'], '#side')

    def test_completion_uses_case_rules_and_removes_departed_nicks(self):
        self.server.send('queryop', ':fake 005 queryop CASEMAPPING=ascii :supported\r\n'
                         ':fake 353 queryop = #side :@queryop member[one\r\n'
                         ':fake 366 queryop #side :end\r\n'
                         ':peer!u@fake NOTICE #side :ascii-roster-ready\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'ascii-roster-ready' for e in self.events()))
        self.command('/chat 1/#side', '[#side]')
        self.term.write(b'@member{\t')
        self.term.repaint_until(b'host-model/medium')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'@member{')
        self.term.write(b'\x15@member[\t')
        self.term.repaint_until(b'@member[one ')
        self.server.send('queryop', ':member[one!u@fake PART #side :gone\r\n')
        self.wait(lambda: any(e['data'].get('nick') == 'member[one' and
                  e['data'].get('kind') == 'part' for e in self.events()))
        self.term.write(b'\x15@member[\t')
        self.term.repaint_until(b'host-model/medium')

    def test_completion_roster_survives_abbreviated_status(self):
        names = [f'member-{i:04d}-abcdefghijklmnop' for i in range(1200)]
        names.append('deep-roster-final')
        for i in range(0, len(names), 150):
            self.server.send('queryop', ':fake 353 queryop = #side :' +
                             ' '.join(names[i:i + 150]) + '\r\n')
        self.server.send('queryop', ':fake 366 queryop #side :end\r\n'
                         ':peer!u@fake NOTICE #side :large-roster-ready\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'large-roster-ready' for e in self.events()))
        self.command('/chat 1/#side', '[#side]')
        self.term.write(b'@deep-\t')
        self.term.repaint_until(b'@deep-roster-final ')
        self.server.send('queryop', ':deep-roster-final!u@fake NICK new-tail-name\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'new-tail-name' for e in self.events()))
        self.term.write(b'\x15@new-tail\t')
        self.term.repaint_until(b'@new-tail-name ')

    def test_mentions_use_selected_channel_and_query(self):
        self.server.send('queryop', ':fake 353 queryop = #lab :@queryop team-lab\r\n'
                         ':fake 366 queryop #lab :end\r\n'
                         ':fake 353 queryop = #side :@queryop team-side\r\n'
                         ':fake 366 queryop #side :end\r\n'
                         ':peer!u@fake NOTICE #side :roster-ready\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'roster-ready' for e in self.events()))
        self.command('/chat 1/#side', '[#side]')
        self.term.write(b'@team-\t')
        self.term.repaint_until(b'@team-side ')
        self.term.write(b'hello\r')
        self.operator_wire('PRIVMSG #side :@team-side hello')
        self.server.send('queryop', ':team-side!u@fake NICK side-renamed\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'side-renamed' for e in self.events()))
        self.term.write(b'@side-\t')
        self.term.repaint_until(b'@side-renamed ')
        self.command('/query 1/private-peer', '[private-peer]')
        self.term.write(b'@priv\t')
        self.term.repaint_until(b'@private-peer ')
        self.term.write(b'hello\r')
        self.operator_wire('PRIVMSG private-peer :@private-peer hello')
        self.term.write(b'@team-\t')
        self.term.repaint_until(b'host-model/medium')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'@team-')
        self.assertNotIn(b'@team-lab ', self.term.output)

    def test_agent_only_channel_tab_is_visible_and_read_only(self):
        self.server.send('querybot', ':querybot!u@fake JOIN #agentonly\r\n'
                         ':peer!u@fake NOTICE #agentonly :agent-channel-visible\r\n')
        self.wait(lambda: ('agent', '#agentonly') in self.channels())
        self.command('/rollout', 'host-model/medium')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b"[#agentonly; viewing model's chat]")
        self.term.write(b'agent-channel-impersonation\r')
        self.term.until(b"Viewing the model's chat")
        self.assert_no_wire('agent-channel-impersonation')

    def test_operator_channel_addresses_actions_and_scope(self):
        self.command('/msg channel-session/1/#SIDE operator-channel-body')
        self.operator_wire('PRIVMSG #side :operator-channel-body')
        self.command('/notice ' + self.selector(identity='operator') + ' channel-notice')
        self.operator_wire('NOTICE #side :channel-notice')
        self.command('/chat 1/#SIDE', '[#side]')
        self.command('/me channel-action')
        self.operator_wire('PRIVMSG #side :\x01ACTION channel-action\x01')
        self.command('/topic selected-channel-topic')
        self.operator_wire('TOPIC #side :selected-channel-topic')
        self.command('/names', 'NAMES refresh requested')
        self.operator_wire('NAMES #side')
        self.assertIn(b'members[', self.term.output)
        self.command('/query 1/private-peer', '[private-peer]')
        before = len(self.server.lines)
        self.command('/topic private-topic-forbidden', 'requires a channel')
        self.command('/names', 'requires a channel')
        self.assertFalse(any('TOPIC ' in line or 'NAMES ' in line
                             for _, line in self.server.lines[before:]))
        self.command('/msg ' + self.selector() + ' impersonation-forbidden', 'unavailable')
        self.command('/msg 1/#missing implicit-join-forbidden', 'unavailable')
        self.assert_no_wire('impersonation-forbidden')
        self.assert_no_wire('implicit-join-forbidden')
        self.assertFalse(any(line == 'JOIN #missing' for _, line in self.server.lines))

    def test_operator_join_part_and_explicit_reopen(self):
        self.command('/join 1/#extra', '[#extra]')
        self.wait(lambda: self.channels().get(('operator', '#extra'), {})
                  .get('routing', {}).get('joined'))
        self.operator_wire('JOIN #extra')
        self.term.write(b'extra-channel-body\r')
        self.operator_wire('PRIVMSG #extra :extra-channel-body')
        self.command('/part 1/#extra done')
        self.operator_wire('PART #extra :done')
        self.wait(lambda: not self.channels()[('operator', '#extra')]['routing']['joined'])
        self.term.write(b'parted-channel-draft\r')
        self.term.until(b'membership changed')
        self.term.repaint_until(b'parted-channel-draft')
        self.assert_no_wire('parted-channel-draft')
        self.command('/join 1/#extra', '[#extra]')
        self.wait(lambda: self.channels()[('operator', '#extra')]['routing']['joined'])
        self.term.write(b'explicitly-reopened-body\r')
        self.operator_wire('PRIVMSG #extra :explicitly-reopened-body')

    def test_channels_query_and_rollout_cycle_preserves_four_drafts(self):
        self.command('/query 1/private-peer', '[private-peer]')
        self.term.write(b'private-draft\t')
        self.term.repaint_until(b'host-model/medium')
        self.term.write(b'rollout-draft\x1b[Z')
        self.term.repaint_until(b'private-draft')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'[#side]')
        self.term.write(b'side-draft\x1b[Z')
        self.term.repaint_until(b'[#lab]')
        self.term.write(b'lab-draft\x1b[Z')
        self.term.repaint_until(b'rollout-draft')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'private-draft')
        self.term.write(b'\r')
        self.operator_wire('PRIVMSG private-peer :private-draft')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'side-draft')
        self.term.write(b'\r')
        self.operator_wire('PRIVMSG #side :side-draft')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'lab-draft')
        self.term.write(b'\r')
        self.operator_wire('PRIVMSG #lab :lab-draft')
        self.term.write(b'\x1b[Z')
        self.term.repaint_until(b'rollout-draft')
        self.assert_no_wire('rollout-draft')
        self.assertFalse(any(self.provider.latest_user(r) == 'rollout-draft' for r in self.seen))

    def test_channel_draft_stays_stale_after_kick_rejoin(self):
        self.command('/chat 1/#side', '[#side]')
        self.term.write(b'frozen-channel-draft')
        self.term.repaint_until(b'frozen-channel-draft')
        previous = self.channels()[('operator', '#side')]['routing']['membership']
        self.server.send('queryop', ':peer!u@fake KICK #side queryop :kicked\r\n'
                         ':queryop!u@fake JOIN #side\r\n')
        self.wait(lambda: self.channels()[('operator', '#side')]['routing']['membership'] != previous)
        self.term.write(b'\r')
        self.term.until(b'membership changed')
        self.term.repaint_until(b'frozen-channel-draft')
        self.assert_no_wire('frozen-channel-draft')
        self.command('/chat 1/#side', '[#side]')
        self.term.write(b'fresh-channel-draft\r')
        self.operator_wire('PRIVMSG #side :fresh-channel-draft')

    def test_shared_incoming_channel_has_one_typed_copy(self):
        events = self.channel_message('#SIDE', 'querybot: shared-channel-input')
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['type'], 'irc_event_v2')
        data = events[0]['data']
        self.assertEqual(data['room'], '#side')
        self.assertEqual(data['routing']['identity'], 'operator')
        self.assertEqual(data['routing']['conversation_id'],
                         self.selector(identity='operator')[8:])
        self.assertTrue(data['urgent'])
        self.assertFalse(data['reply'])  # A peer mention is not a local operator request.

    def test_agent_only_channel_reaches_model(self):
        self.server.send('querybot', ':querybot!u@fake JOIN #agentonly\r\n')
        self.wait(lambda: ('agent', '#agentonly') in self.channels())
        events = self.channel_message('#agentonly', 'querybot: agent-only-input',
                                      recipients=('querybot',))
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['type'], 'irc_event_v2')
        self.assertEqual(events[0]['data']['routing']['identity'], 'agent')
        self.assertFalse(events[0]['data']['reply'])

    def test_channel_reply_requires_matching_message(self):
        target = self.selector()
        original = self.channels()[('agent', '#side')]['routing']
        calls = [self.send_call(self.selector('#lab'), 'wrong-room-reply'),
                 self.send_call(target, 'right-room-notice', notice=True), None,
                 self.send_call(target, 'right-room-reply'), ('irc_state', {})]
        observed = []

        def respond(handler, request, sequence):
            self.seen.append(request)
            if 'channel-obligation-input' in json.dumps(request):
                observed.append(request)
                call = calls.pop(0) if calls else None
                wire = self.provider.function_body(sequence, f'reply-{sequence}', *call) if call \
                    else self.provider.response_body(sequence, 'channel reply boundary')
            else:
                wire = self.provider.response_body(sequence, 'channel fixture done')
            self.provider.reply(handler, wire.encode(), close_header=True)
            handler.close_connection = True

        self.provider.runtime_handler = respond
        events = self.channel_message('#side', 'querybot: channel-obligation-input', local=True)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['data']['reply_to'],
                         dict(conversation_id=target[8:], membership=original['membership']))
        self.assertFalse(calls)
        admitted = next(e for e in self.events() if e['type'] == 'irc_admitted' and
                        events[0]['seq'] in e['data']['sequences'])
        turn = next(e['data']['turn_id'] for e in self.events()
                    if e['seq'] > admitted['seq'] and e['type'] == 'turn_started')
        self.assertEqual(sum(e['type'] == 'response_started' and
                         e['data']['turn_id'] == turn for e in self.events()), 6)
        self.assertEqual(sum(e['type'] == 'irc_reply_reminder' for e in self.events()), 1)
        self.assertIn(f'target={target} status=available', json.dumps(observed[0]))
        self.assertIn('membership=' + original['membership'], json.dumps(observed[0]))
        self.assertIn('Outstanding IRC replies for this turn: none.', json.dumps(observed[-1]))
        self.assert_wire('PRIVMSG #lab :wrong-room-reply')
        self.assert_wire('NOTICE #side :right-room-notice')
        self.assert_wire('PRIVMSG #side :right-room-reply')

    def test_agent_channel_receives_while_operator_rejoin_is_pending(self):
        self.server.defer_operator_join = True
        self.server.links['queryop'].shutdown(socket.SHUT_RDWR)
        self.wait(lambda: not self.channels()[('operator', '#lab')]['routing']['joined'] and
                  self.channels()[('agent', '#lab')]['routing']['joined'])
        events = self.channel_message('#lab', 'querybot: operator-rejoining-input',
                                      recipients=('querybot',))
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['data']['routing']['identity'], 'agent')

    def test_operator_only_channel_records_unavailable_reply(self):
        self.server.send('queryop', ':queryop!u@fake JOIN #operatoronly\r\n')
        self.wait(lambda: ('operator', '#operatoronly') in self.channels())
        events = self.channel_message('#operatoronly', 'querybot: unavailable-reply-input',
                                      local=True, recipients=('queryop',))
        self.assertEqual(len(events), 1)
        self.assertTrue(events[0]['data']['reply'])
        self.assertIsNone(events[0]['data']['reply_to'])
        self.assertIn('target=unavailable status=unavailable', json.dumps(self.seen))
        self.assertEqual(sum(e['type'] == 'irc_reply_reminder' for e in self.events()), 1)
        self.assert_no_wire('unavailable-reply-input')

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


class ChannelPrefixTests(ChannelFixture):
    chantypes = '#$'

    def test_custom_channel_history_survives_resume(self):
        self.command('/join 1/$side', '[$side]')
        self.server.send('querybot', ':querybot!u@fake JOIN $side\r\n')
        self.wait(lambda: self.channels().get(('agent', '$side'), {})
                  .get('routing', {}).get('joined'))
        self.channel_message('$side', 'retained-custom-channel')
        before = self.channels()[('operator', '$side')]['routing']
        sid = read_events(self.root / 'state')[0].parent.name
        self.command('/exit')
        self.term.wait_exit()
        self.assertEqual(self.term.process.returncode, 0)
        self.term = self.start('--resume', sid)
        self.wait(lambda: self.channels()[('operator', '$side')]['routing']['generation'] >
                  before['generation'])
        after = self.channels()[('operator', '$side')]['routing']
        self.assertEqual(before['conversation_id'], after['conversation_id'])
        self.assertNotEqual(before['membership'], after['membership'])
        self.wait_idle()
        self.seen.clear()
        self.submit('verify-custom-resume')
        self.assertIn('retained-custom-channel', json.dumps(self.seen))
        self.command('/chat 1/$side', '[$side]')
        self.command('after-custom-resume')
        self.operator_wire('PRIVMSG $side :after-custom-resume')

    def test_advertised_prefix_classifies_operator_commands(self):
        self.command('/query 1/$side', 'select a nick')
        self.command('/join 1/$side', '[$side]')
        self.wait(lambda: self.channels().get(('operator', '$side'), {})
                  .get('routing', {}).get('joined'))
        self.operator_wire('JOIN $side')
        self.command('/msg 1/$SIDE custom-channel-body')
        self.operator_wire('PRIVMSG $side :custom-channel-body')


if __name__ == '__main__':
    unittest.main()
