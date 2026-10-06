# SPDX-License-Identifier: GPL-2.0-only
"""Channel panes share an owner while retaining their exact send destinations."""

import unittest

from test_irc_channels import ChannelFixture
import test_vm_queries as queries
from tmux_terminal import read_events


class ChannelWorkspaceTests(ChannelFixture):
    snapshots = queries.QueryWorkspaceTests.snapshots
    wait_snapshot = queries.QueryWorkspaceTests.wait_snapshot
    workspace_start = queries.QueryWorkspaceTests.workspace_start
    state = queries.QueryWorkspaceTests.state
    normal = queries.QueryWorkspaceTests.normal

    def setUp(self):
        super().setUp()
        self.path, _ = read_events(self.root / 'state')
        self.sid = self.path.parent.name
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.child = self.workspace_start('-N', 'channel-panes')
        self.child.command('attach ' + self.sid)
        self.child.repaint_until(b'ATTACHED')

    def channel(self, room):
        self.child.write(b'i/chat 1/' + room.encode() + b'\r')
        self.child.repaint_until(b'channel/' + room.encode())

    def test_channel_splits_drafts_wire_and_workspace_resume(self):
        child = self.child
        self.channel('#lab')
        child.write(b'ilab-pane-draft')
        self.normal()
        child.command('vsp')
        child.command('history')
        self.channel('#side')
        child.write(b'iside-pane-draft')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and
            len(self.state()['windows']) == 2 and
            all(w['history']['route'] for w in self.state()['windows']))
        routes = [w['history']['route'] for w in self.state()['windows']]
        self.assertEqual([r['room'] for r in routes], ['#lab', '#side'])
        buffers = self.state()['buffers']
        self.assertEqual(len(buffers), 1)
        drafts = {b['route']['room']: b['draft'] for b in buffers[0]['buffers']
                  if isinstance(b['route'], dict) and b['route'].get('identity') == 'operator'
                  and 'room' in b['route']}
        self.assertEqual(drafts, {'#lab': 'lab-pane-draft', '#side': 'side-pane-draft'})
        child.write(b'i\r')
        self.operator_wire('PRIVMSG #side :side-pane-draft')
        self.assert_no_wire('lab-pane-draft')
        self.normal()
        child.command('close')
        child.finish('close')
        child = self.workspace_start('--resume', 'channel-panes')
        child.repaint_until(b'lab-pane-draft')
        self.assert_no_wire('lab-pane-draft')
        child.finish('close')

    def test_channel_history_and_private_query_are_separate(self):
        self.server.send('queryop', ':peer!u@fake PRIVMSG #lab :lab-public-body\r\n'
                         ':peer!u@fake PRIVMSG #side :side-public-body\r\n'
                         ':peer!u@fake PRIVMSG queryop :private-hidden-body\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'private-hidden-body' for e in self.events()))
        self.channel('#side')
        self.child.repaint_until(b'side-public-body')
        self.assertNotIn(b'lab-public-body', self.child.output)
        self.assertNotIn(b'private-hidden-body', self.child.output)
        self.child.write(b'/private-hidden-body\r')
        self.child.repaint_until(b'No matches')
        self.child.command('vsp channel-session/' + self.server.endpoint + '/#lab')
        self.child.repaint_until(b'lab-public-body')
        self.child.write(b'i/query 1/peer\r')
        self.child.repaint_until(b'query/peer')
        self.child.repaint_until(b'private-hidden-body')
        self.normal()
        self.child.command('close')
        self.child.finish('close')


if __name__ == '__main__':
    unittest.main()
