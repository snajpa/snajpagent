# SPDX-License-Identifier: GPL-2.0-only
"""Private conversations through the semantic owner, independent of classic tabs."""

import json
import uuid
import unittest

from test_irc_queries import QueryFixture
from test_session_view import View
from tmux_terminal import read_events


class QueryViewTests(QueryFixture):
    def setUp(self):
        super().setUp()
        self.path, _ = read_events(self.root / 'state')
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.assertEqual(self.term.process.returncode, 0)
        self.view = View(self.path.parent)
        self.addCleanup(self.view.close)
        self.assertIn('irc_queries', self.view.capabilities['features'])
        self.view.bind()

    def command(self, text, route='rollout'):
        request = uuid.uuid4().hex
        self.view.send(type='command', generation=self.view.generation,
                       id=request, text=text, route=route)
        return self.view.result(request)

    def query(self, nick):
        result = self.command('/query ' + nick)
        self.assertEqual(result['status'], 'completed', result)
        self.assertEqual(result['outcome'], 'ok', result)
        self.assertEqual(result['selection']['peer'], nick)
        return result['selection']

    def draft(self, route):
        self.view.send(type='draft_get', generation=self.view.generation, route=route)
        return self.view.until('draft', lambda msg: msg['status'] == 'snapshot' and
                               msg['draft']['route'] == route)['draft']

    def edit(self, route, text):
        draft = self.draft(route)
        self.view.send(type='draft', generation=self.view.generation, route=route,
                       revision=draft['revision'], edit=1, text=text,
                       cursor=len(text.encode()))
        result = self.view.until('draft', lambda msg: msg['edit'] == 1)
        self.assertEqual(result['status'], 'accepted', result)
        return result['draft']

    def send(self, route, text, **options):
        request = self.view.submit(text, route=route, **options)
        return request, self.view.result(request)

    def test_separate_query_drafts_send_once_without_provider_input(self):
        first = self.query('query-peer')
        second = self.query('observer')
        rollout = self.edit('rollout', 'unsent model draft')
        a = self.edit(first, 'only first peer')
        b = self.edit(second, 'only second peer')
        inputs = sum(e['type'] == 'input_received' for e in self.events())
        request, receipt = self.send(first, a['text'], draft_revision=a['revision'])
        self.assertEqual(receipt['status'], 'committed', receipt)
        self.wait_wire(self.peer, b'PRIVMSG query-peer :only first peer')
        self.assertEqual(self.draft(first)['text'], '')
        self.assertEqual(self.draft(second), b)
        self.assertEqual(self.draft('rollout'), rollout)
        self.view.submit(a['text'], request=request, route=first,
                         draft_revision=a['revision'])
        self.assertEqual(self.view.result(request), receipt)
        self.view.submit(a['text'], request=request, route=second,
                         draft_revision=b['revision'])
        self.assertIn('already used', self.view.until('error')['message'])
        self.peer.drain(.1)
        self.observer.drain(.1)
        self.assertEqual(bytes(self.peer.buf).count(b'PRIVMSG query-peer :only first peer'), 1)
        self.assertNotIn(b'only first peer', bytes(self.observer.buf))
        self.assertEqual(sum(e['type'] == 'input_received' for e in self.events()), inputs)
        self.assertNotIn('only first peer', json.dumps(self.seen))

    def test_query_commands_return_selection_and_use_focused_route(self):
        first = self.query('query-peer')
        second = self.query('observer')
        result = self.command('/me waves', first)
        self.assertEqual(result['outcome'], 'ok', result)
        self.wait_wire(self.peer, b'PRIVMSG query-peer :\x01ACTION waves\x01')
        result = self.command('/notice observer private notice', first)
        self.assertEqual(result['outcome'], 'ok', result)
        self.wait_wire(self.observer, b'NOTICE observer :private notice')
        self.assertNotIn('selection', result)
        result = self.command('/msg query-peer from second window', second)
        self.assertEqual(result['outcome'], 'ok', result)
        self.wait_wire(self.peer, b'PRIVMSG query-peer :from second window')
        self.assertNotIn('selection', result)
        result = self.command('/config', first)
        self.assertEqual(result['status'], 'rejected', result)
        self.assertIn('rollout', result['event'])

    def test_stale_route_preserves_draft_after_nick_reuse(self):
        route = self.query('query-peer')
        draft = self.edit(route, 'never retarget this')
        self.peer.sock.sendall(b'NICK renamed-peer\r\n')
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and
                  e['data']['kind'] == 'nick' and
                  e['data']['routing']['peer'] == 'renamed-peer' for e in self.events()))
        replacement = self.connect('query-peer')
        _, result = self.send(route, draft['text'], draft_revision=draft['revision'])
        self.assertEqual(result['status'], 'rejected', result)
        self.assertEqual(self.draft(route), draft)
        self.peer.drain(.1)
        replacement.drain(.1)
        self.assertNotIn(b'never retarget this', bytes(self.peer.buf))
        self.assertNotIn(b'never retarget this', bytes(replacement.buf))
        new_route = self.query('renamed-peer')
        self.assertEqual(new_route['conversation'], route['conversation'])
        self.assertEqual(self.draft(new_route)['text'], '')
        self.assertEqual(self.draft(route), draft)

    def test_empty_lines_preserve_draft_without_false_admission(self):
        route = self.query('query-peer')
        draft = self.edit(route, '\n\n')
        before = [e for e in self.events() if e['type'] == 'irc_event_v2' and
                  e['data']['routing']['direction'] == 'outgoing']
        _, receipt = self.send(route, draft['text'], draft_revision=draft['revision'])
        self.assertEqual(receipt['status'], 'rejected', receipt)
        self.assertEqual(self.draft(route), draft)
        self.assertEqual([e for e in self.events() if e['type'] == 'irc_event_v2' and
                          e['data']['routing']['direction'] == 'outgoing'], before)

    def test_agent_and_unknown_routes_cannot_write_or_fall_back(self):
        route = self.query('query-peer')
        for altered in ({**route, 'identity': 'agent'},
                        {**route, 'conversation': uuid.uuid4().hex},
                        {**route, 'connection': uuid.uuid4().hex},
                        {**route, 'extra': 'unsupported'}):
            request = self.view.submit('forbidden semantic input', route=altered)
            while True:
                reply = self.view.receive()
                if reply['type'] in ('error', 'result') and reply.get('id') == request and \
                        reply.get('status') != 'pending':
                    break
            self.assertTrue(reply['type'] == 'error' or reply['status'] == 'rejected', reply)
        self.peer.drain(.1)
        self.assertNotIn(b'forbidden semantic input', bytes(self.peer.buf))
        self.assertNotIn('forbidden semantic input', json.dumps(self.seen))

    def test_incoming_query_is_available_while_semantic_controller_is_attached(self):
        self.direct('queryop', 'incoming operator private body')
        state = self.view.until('state', lambda msg: any(
            row['route']['peer'] == 'query-peer' for row in msg['state']['queries']))['state']
        route = next(row['route'] for row in state['queries']
                     if row['route']['peer'] == 'query-peer')
        self.assertNotIn('incoming operator private body', json.dumps(state))
        _, receipt = self.send(route, 'reply from independent window')
        self.assertEqual(receipt['status'], 'committed', receipt)
        self.wait_wire(self.peer, b'PRIVMSG query-peer :reply from independent window')

    def test_private_send_during_provider_wait_does_not_steer(self):
        route = self.query('query-peer')

        def held_response(handler, request, sequence):
            self.held.set()
            assert self.release.wait(15), 'provider fixture was not released'
            self.respond(handler, request, sequence)

        self.provider.runtime_handler = held_response
        request = self.view.submit('hold a model turn')
        self.assertEqual(self.view.result(request)['status'], 'committed')
        self.wait(self.held.is_set)
        steering = [e for e in self.events() if e['type'] == 'steering_added']
        _, receipt = self.send(route, 'private while model works')
        self.assertEqual(receipt['status'], 'committed', receipt)
        self.wait_wire(self.peer, b'PRIVMSG query-peer :private while model works')
        self.assertEqual([e for e in self.events() if e['type'] == 'steering_added'], steering)
        self.release.set()
        self.wait_idle()
        self.assertNotIn('private while model works', json.dumps(self.seen))

    def test_query_selection_preserves_classic_rollout_focus(self):
        self.query('query-peer')
        self.view.send(type='detach', generation=self.view.generation)
        self.view.until('detached')
        self.term = self.start('--resume', self.path.parent.name, ready=b'fake/host-model/medium')
        before = sum(e['type'] == 'input_received' for e in self.events())
        self.term.write(b'classic rollout is still selected\r')
        self.wait(lambda: sum(e['type'] == 'input_received' for e in self.events()) > before)
        self.wait_idle()
        self.peer.drain(.1)
        self.assertNotIn(b'classic rollout is still selected', bytes(self.peer.buf))

    def test_catalogue_and_reconnect_preserve_routes_drafts_and_receipts(self):
        first = self.query('query-peer')
        second = self.query('observer')
        draft = self.edit(second, 'keep across attachment')
        request, result = self.send(first, 'receipt survives detach')
        self.assertEqual(result['status'], 'committed', result)
        self.wait_wire(self.peer, b'PRIVMSG query-peer :receipt survives detach')
        self.view.send(type='detach', generation=self.view.generation)
        self.view.until('detached')
        self.view.close()
        self.view = View(self.path.parent)
        self.addCleanup(self.view.close)
        state = self.view.until('state')['state']
        routes = [row['route'] for row in state['queries']]
        self.assertIn(first, routes)
        self.assertIn(second, routes)
        self.assertNotIn('receipt survives detach', json.dumps(state))
        self.view.bind()
        self.assertEqual(self.draft(second), draft)
        self.view.send(type='receipt', id=request)
        self.assertEqual(self.view.result(request), result)
        self.peer.drain(.1)
        self.assertEqual(bytes(self.peer.buf).count(b'receipt survives detach'), 1)


if __name__ == '__main__':
    unittest.main()
