# SPDX-License-Identifier: GPL-2.0-only
"""Conversation panes share one owner and preserve private drafts and history."""

import json
import re
import time
import unittest

from test_irc_queries import QueryFixture
from test_upload_client import FixtureChildren
import test_vm_frontend as frontend
from test_vm_frontend import Terminal
from tmux_terminal import read_events


class QueryWorkspaceTests(QueryFixture):
    snapshots = frontend.WorkspaceTests.snapshots
    wait_snapshot = frontend.WorkspaceTests.wait_snapshot

    def setUp(self):
        super().setUp()
        self.path, _ = read_events(self.root / 'state')
        self.sid = self.path.parent.name
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.child = self.workspace_start('-N', 'private-panes')
        self.child.command('attach ' + self.sid)
        self.child.repaint_until(b'ATTACHED')

    def workspace_start(self, *args):
        child = Terminal(self.root, args, rows=30, columns=220)
        self.addCleanup(child.close)
        children = FixtureChildren(child.process.pid)
        self.addCleanup(children.close)
        self.terminals.append((child, children))
        self._terminals = [child]
        child.until(b'\x1b[?1049h')
        return child

    def state(self):
        return next(iter(self.snapshots().values()))['state']

    def buffers(self):
        return {b['route']['peer'] if isinstance(b['route'], dict) else 'rollout': b
                for owner in self.state()['buffers'] for b in owner['buffers']
                if not isinstance(b['route'], dict) or 'peer' in b['route']}

    def query(self, peer):
        self.child.write(b'i/query ' + peer.encode() + b'\r')
        self.child.repaint_until(b'query/' + peer.encode())

    def normal(self):
        self.child.write(b'\x1b')
        time.sleep(.06)
        self.child.write(b'\x1b')
        time.sleep(.06)

    def test_query_selects_origin_split_and_preserves_three_drafts(self):
        child = self.child
        self.query('query-peer')
        child.write(b'iunsent peer')
        self.normal()
        child.command('vsp')
        child.command('history')
        self.query('observer')
        child.write(b'iunsent observer')
        self.normal()
        child.command('sp')
        child.command('history')
        child.write(b'iunsent rollout')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and len(self.buffers()) == 3 and
                           self.buffers()['rollout']['draft'] == 'unsent rollout')
        self.assertEqual(self.buffers()['query-peer']['draft'], 'unsent peer')
        self.assertEqual(self.buffers()['observer']['draft'], 'unsent observer')
        state = self.state()
        self.assertEqual(len(state['buffers']), 1)
        routes = [w['history']['route'] for w in state['windows']]
        self.assertEqual([r['peer'] if r else 'rollout' for r in routes],
                         ['query-peer', 'observer', 'rollout'])
        child.command('close')
        child.command('close')
        child.finish('close')
        resumed = self.workspace_start('--resume', 'private-panes')
        resumed.repaint_until(b'query/query-peer')
        resumed.repaint_until(b'unsent peer')
        resumed.finish('close')

    def test_buffer_picker_cycle_and_read_only_agent_query(self):
        self.direct('queryop', 'operator chat body')
        self.direct('querybot', 'agent chat body', notice=True)
        child = self.child
        child.command('buffers')
        child.repaint_until(b'/query-peer [agent]')
        agent_id = re.search(rb'([0-9a-f]{8}) [^\x1b\r\n]*query-peer \[agent\]',
                             child.output)[1]
        child.write(b'/' + agent_id + b'\r\r')
        child.repaint_until(b'[agent read-only]')
        child.repaint_until(b'agent chat body')
        self.assertNotIn(b'operator chat body', child.output)
        child.write(b'i\x1b[200~never send from the agent identity\x1b[201~')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['buffers'] and
                           len(self.state()['buffers'][0]['buffers']) == 6)
        self.assertTrue(all(not b['draft'] and not b['pending']
                            for b in self.state()['buffers'][0]['buffers']))
        child.command('bn')
        child.repaint_until(b'[operator]')
        child.command('bp')
        child.repaint_until(b'[agent read-only]')
        child.finish('q')

    def test_two_windows_share_one_query_editor_and_undo(self):
        self.query('query-peer')
        child = self.child
        child.write(b'ishared text')
        self.normal()
        child.command('vsp')
        child.write(b'A suffix')
        self.normal()
        child.write(b'\x17w')
        child.repaint_until(b'shared text suffix')
        child.write(b'i\x1b')
        time.sleep(.06)
        child.repaint_until(b'NORMAL composer')
        child.write(b'u')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['buffers'] and
                           self.buffers().get('query-peer', {}).get('draft') == 'shared text')
        self.assertEqual(len(self.state()['buffers'][0]['buffers']), 5)
        child.command('close')
        child.finish('close')

    def test_visual_copy_and_addressed_split_preserve_private_destination(self):
        self.direct('queryop', 'copy this private line')
        self.query('query-peer')
        child = self.child
        child.write(b'/copy this private line\r')
        child.repaint_until(b'Match')
        child.write(b'v$y')
        child.repaint_until(b'Yanked')
        child.write(b'P')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['buffers'] and
                           self.buffers().get('query-peer', {}).get('draft') == 'copy this private line')
        route = self.buffers()['query-peer']
        address = self.sid[:8] + '/' + route['endpoint'] + '/query-peer'
        child.command('vsp ' + address)
        child.command('workspace save')
        self.wait_snapshot(lambda rows: len(self.state()['windows']) == 2)
        self.assertEqual(self.state()['windows'][0]['history']['route'],
                         self.state()['windows'][1]['history']['route'])
        child.command('close')
        child.finish('close')

    def test_shutdown_checks_hidden_conversation_drafts(self):
        self.query('query-peer')
        child = self.child
        child.write(b'ikeep hidden private draft')
        self.normal()
        child.command('history')
        child.command('qa')
        child.repaint_until(b'Unsent draft or unresolved submission')
        self.assertIsNone(child.process.poll())
        child.command('buffers')
        child.write(b'/query-peer\r\r')
        child.repaint_until(b'keep hidden private draft')
        child.finish('close')

    def test_scoped_private_commands_keep_the_conversation_window(self):
        self.query('query-peer')
        child = self.child
        child.write(b'i/me waves\r')
        self.wait_wire(self.peer, b'PRIVMSG query-peer :\x01ACTION waves\x01')
        child.repaint_until(b'/me waves')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['buffers'] and
                           not self.buffers().get('query-peer', {}).get('pending') and
                           bool(self.state()['buffers'][0]['reports']))
        self.assertEqual(self.state()['windows'][0]['kind'], 'transcript')
        self.assertEqual(self.state()['windows'][0]['history']['route']['peer'], 'query-peer')
        self.assertEqual(self.state()['buffers'][0]['reports'][-1]['command'], '/me waves')
        child.finish('close')

    def test_nick_change_keeps_draft_pinned_and_recoverable(self):
        self.query('query-peer')
        child = self.child
        child.write(b'inever retarget this draft')
        self.peer.sock.sendall(b'NICK renamed-peer\r\n')
        self.wait(lambda: any(e['type'] == 'irc_event_v2' and
                             e['data'].get('text') == 'renamed-peer' for e in self.events()))
        replacement = self.connect('query-peer')
        child.write(b'\r')
        child.repaint_until(b'Submission rejected')
        self.normal()
        child.command('recover')
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['buffers'] and
                           self.buffers().get('query-peer', {}).get('draft') == 'never retarget this draft')
        replacement.drain(.1)
        self.peer.drain(.1)
        self.assertNotIn(b'never retarget this draft', bytes(replacement.buf))
        self.assertNotIn(b'never retarget this draft', bytes(self.peer.buf))
        self.assertEqual(self.buffers()['query-peer']['route']['peer'], 'query-peer')
        child.finish('close')

    def test_private_history_search_and_submission_stay_in_query(self):
        self.direct('queryop', 'private history needle')
        self.observer.sock.sendall(b'PRIVMSG queryop :other conversation needle\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'other conversation needle'
                             for e in self.events() if e['type'] == 'irc_event_v2'))
        self.query('query-peer')
        child = self.child
        child.repaint_until(b'private history needle')
        self.assertNotIn(b'other conversation needle', child.output)
        self.normal()
        child.write(b'/other conversation needle\r')
        child.repaint_until(b'No matches')
        child.write(b'/private history needle\r')
        child.repaint_until(b'Match')
        inputs = sum(e['type'] == 'input_received' for e in self.events())
        child.write(b'iprivate reply\r')
        self.wait_wire(self.peer, b'PRIVMSG query-peer :private reply')
        self.wait(lambda: not self.buffers()['query-peer']['pending'])
        self.assertEqual(sum(e['type'] == 'input_received' for e in self.events()), inputs)
        self.assertNotIn('private reply', json.dumps(self.seen))
        self.normal()
        child.finish('q')


if __name__ == '__main__':
    unittest.main()
