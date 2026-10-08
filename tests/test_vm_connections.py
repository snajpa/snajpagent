# SPDX-License-Identifier: GPL-2.0-only
"""Trailing-slash connection panes reuse their session owner and drafts."""

import unittest

import test_vm_channels as channels
from test_irc_channels import ChannelFixture
from tmux_terminal import read_events


class ConnectionWorkspaceTests(ChannelFixture):
    snapshots = channels.ChannelWorkspaceTests.snapshots
    wait_snapshot = channels.ChannelWorkspaceTests.wait_snapshot
    workspace_start = channels.ChannelWorkspaceTests.workspace_start
    state = channels.ChannelWorkspaceTests.state
    normal = channels.ChannelWorkspaceTests.normal

    def setUp(self):
        super().setUp()
        self.sid = read_events(self.root / 'state')[0].parent.name
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.child = self.workspace_start('-N', 'connection-panes')
        self.child.command('attach ' + self.sid)
        self.child.attached()

    def test_connection_address_history_draft_and_split_query(self):
        child = self.child
        address = 'channel-session/' + self.server.endpoint + '/'
        self.server.send('queryop', ':fake NOTICE queryop :connection-pane-notice\r\n')
        self.wait(lambda: any('connection-pane-notice' in e['data'].get('text', '')
                             for e in self.events()))
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and any(
            isinstance(b['route'], dict) and b['route'].get('endpoint') == self.server.endpoint
            and 'room' not in b['route'] for owner in self.state()['buffers']
            for b in owner['buffers']))
        child.command('buffer ' + address)
        child.repaint_until(b'[' + self.server.endpoint.encode() + b']')
        child.repaint_until(b'connection-pane-notice')
        child.write(b'i/query peer\r')
        child.repaint_until(b'[peer]')
        self.normal()
        child.command('buffer ' + address)
        child.repaint_until(b'[' + self.server.endpoint.encode() + b']')
        child.write(b'iconnection-cycle-draft\t')
        child.repaint_until(b'[#side]')
        child.write(b'\x1b[Z')
        child.repaint_until(b'[' + self.server.endpoint.encode() + b']')
        child.repaint_until(b'connection-cycle-draft')
        self.assert_no_wire('connection-cycle-draft')
        child.write(b'\x15keep-connection-draft\r')
        child.repaint_until(b'explicit target')
        self.normal()
        child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and any(
            b['draft'] == 'keep-connection-draft' for owner in self.state()['buffers']
            for b in owner['buffers']))
        child.command('vsp ' + address + 'peer')
        child.repaint_until(b'[peer]')
        child.write(b'ifrom-pinned-connection\r')
        self.operator_wire('PRIVMSG peer :from-pinned-connection')
        self.assert_no_wire('keep-connection-draft')
        self.normal()
        child.command('close')
        child.repaint_until(b'keep-connection-draft')
        child.write(b'i\t')
        child.repaint_until(b'[#side]')
        child.write(b'\x1b[Z')
        child.repaint_until(b'keep-connection-draft')
        self.normal()
        child.finish('close')
        child = self.workspace_start('--resume', 'connection-panes')
        child.repaint_until(b'[' + self.server.endpoint.encode() + b']')
        child.repaint_until(b'keep-connection-draft')
        child.finish('close')


if __name__ == '__main__':
    unittest.main()
