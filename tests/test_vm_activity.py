# SPDX-License-Identifier: GPL-2.0-only
"""Unread conversations survive hidden panes, status changes and workspace resume."""

import os
import re
import unittest
from pathlib import Path
from unittest.mock import patch

import test_vm_channels as channels
import test_vm_frontend as frontend
from test_irc_channels import ChannelFixture
from tmux_terminal import read_events


class ActivityWorkspaceTests(ChannelFixture):
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
        self.child = self.workspace_start('-N', 'activity-panes')
        self.child.command('attach ' + self.sid)
        self.child.attached()

    def incoming(self, peer, body, local='queryop', target=None):
        self.server.send(local, f':{peer}!u@fake NOTICE {target or local} :{body}\r\n')
        self.wait(lambda: any(e['data'].get('text') == body for e in self.events()))
        if target is None:
            self.wait_snapshot(lambda rows: rows and any(
                isinstance(b['route'], dict) and b['route'].get('peer') == peer
                for owner in self.state()['buffers'] for b in owner['buffers']))
        return next(e['seq'] for e in self.events() if e['data'].get('text') == body)

    def directory(self, peer, count, identity='operator'):
        self.child.command('buffers')
        label = 'your chat' if identity == 'operator' else "model's chat"
        self.child.repaint_until(f'/{peer} [{label}] [{count} unread]'.encode())

    def query(self, peer):
        self.child.command('buffer channel-session/' + self.server.endpoint + '/' + peer)
        self.child.repaint_until(b'[' + peer.encode() + b']')

    def saved_read(self, peer, predicate):
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and any(
            isinstance(b['route'], dict) and b['route'].get('peer') == peer
            and b['route']['identity'] == 'operator' and predicate(b.get('read', {}))
            for owner in self.state()['buffers'] for b in owner['buffers']))
        through = max(e['seq'] for e in self.events())
        for owner in self.state()['buffers']:
            for buffer in owner['buffers']:
                self.assertLessEqual(buffer.get('read', {}).get('seq', 0), through)

    def test_count_survives_nick_and_resume_and_keeps_roles_separate(self):
        self.incoming('peer', 'first-operator-private')
        last = self.incoming('peer', 'second-operator-private')
        self.incoming('peer', 'agent-private-notice', local='querybot')
        self.server.send('queryop', ':peer!u@fake NICK renamed\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'renamed' for e in self.events()))
        self.directory('renamed', 2)
        self.child.repaint_until(b'/renamed [your chat] [2 unread] [connected]')
        self.child.repaint_until(b'[agent connected]')
        self.child.repaint_until(b"/peer [model's chat] [1 unread]")
        screen = re.sub(rb'\x1b\[[0-9;?]*[a-zA-Z]', b'', bytes(self.child.output))
        self.assertLess(screen.index(b'channel-session [rollout]'),
                        screen.index(self.server.endpoint.encode() + b'/ [your chat]'))
        self.assertLess(screen.index(self.server.endpoint.encode() + b'/ [your chat]'),
                        screen.index(b'/renamed [your chat]'))
        self.assertRegex(screen, rb'\d\d-\d\d \d\d:\d\d:\d\d')
        self.saved_read('renamed', lambda read: read.get('seq') == 0)
        self.query('renamed')
        self.child.repaint_until(b'second-operator-private')
        self.saved_read('renamed', lambda read: read.get('seq', 0) >= last
                        and read.get('received') == 2)
        self.directory('renamed', 0)
        self.child.repaint_until(b"/peer [model's chat] [1 unread]")
        self.child.finish('close')
        self.incoming('renamed', 'while-workspace-closed')
        self.child = self.workspace_start('--resume', 'activity-panes')
        self.child.repaint_until(b'/renamed [your chat] [1 unread]')
        self.query('renamed')
        self.child.repaint_until(b'while-workspace-closed')
        self.saved_read('renamed', lambda read: read.get('received') == 3)
        self.child.write(b'ikeep-private-draft')
        self.normal()
        self.directory('renamed', 0)
        self.child.repaint_until(b'[0 unread] [draft]')
        self.child.finish('close')
        serialized = str(self.seen)
        self.assertNotIn('first-operator-private', serialized)
        self.assertNotIn('second-operator-private', serialized)

    def test_hold_and_terminal_focus_loss_keep_unread_until_tail_is_painted(self):
        first = self.incoming('peer', 'initial-read-body')
        self.query('peer')
        self.child.repaint_until(b'initial-read-body')
        self.saved_read('peer', lambda read: read.get('seq', 0) >= first)
        self.child.write(b'gg')
        self.child.repaint_until(b'HOLD')
        self.incoming('peer', 'held-body-one')
        last = self.incoming('peer', 'held-body-two')
        self.child.command('vsp')
        self.directory('peer', 2)
        self.saved_read('peer', lambda read: read.get('received') == 1)
        self.child.command('close')
        self.child.repaint_until(b'HOLD')
        self.child.write(b'\x1b[OG')
        self.child.repaint_until(b'held-body-two')
        self.child.command('vsp')
        self.directory('peer', 2)
        self.child.command('close')
        self.child.write(b'\x1b[I')
        self.child.repaint_until(b'held-body-two')
        self.saved_read('peer', lambda read: read.get('seq', 0) >= last
                        and read.get('received') == 3)
        self.directory('peer', 0)
        self.child.finish('close')

    def test_directory_selection_survives_insertions_and_nick_change(self):
        self.incoming('chosen', 'selected-conversation-body')
        self.directory('chosen', 1)
        self.child.write(b'G')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['windows'][0]['selected'])
        selected = self.state()['windows'][0]['selected']
        self.incoming('ahead', 'inserted-conversation-body')
        self.child.repaint_until(b'/ahead [your chat] [1 unread]')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: self.state()['windows'][0]['selected'] == selected)
        self.server.send('queryop', ':chosen!u@fake NICK renamed\r\n')
        self.child.repaint_until(b'/chosen [your chat] [1 unread] [old route]')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: self.state()['windows'][0]['selected'] == selected)
        self.child.write(b'\r')
        self.child.repaint_until(b'[chosen]')
        self.child.repaint_until(b'selected-conversation-body')
        self.assertNotIn(b'inserted-conversation-body', self.child.output)
        self.child.finish('close')

    def test_background_pane_and_channel_role_history_share_only_their_read_position(self):
        first = self.incoming('peer', 'initial-background-body')
        self.query('peer')
        self.saved_read('peer', lambda read: read.get('seq', 0) >= first)
        self.incoming('second', 'focused-other-conversation')
        self.child.command('vsp channel-session/' + self.server.endpoint + '/second')
        self.child.repaint_until(b'focused-other-conversation')
        self.incoming('peer', 'unread-in-background-pane')
        self.incoming('channel-peer', 'operator-channel-body', target='#side')
        self.server.send('queryop', ':queryop!u@fake PART #side :leaving\r\n')
        self.wait(lambda: not self.channels()[('operator', '#side')]['routing']['joined'])
        self.incoming('channel-peer', 'agent-channel-body', local='querybot', target='#side')
        self.directory('peer', 1)
        self.child.repaint_until(b'/#side [your chat] [2 unread]')
        self.child.repaint_until(b"/#side [model's chat] [2 unread]")
        self.child.command('buffer channel-session/' + self.server.endpoint + '/#side')
        self.child.repaint_until(b'agent-channel-body')
        self.wait_snapshot(lambda rows: rows and all(
            b.get('read', {}).get('received') == 2
            for owner in self.state()['buffers'] for b in owner['buffers']
            if isinstance(b['route'], dict) and b['route'].get('room') == '#side'))
        self.directory('#side', 0)
        self.child.repaint_until(b"/#side [model's chat] [0 unread]")
        self.child.repaint_until(b'/peer [your chat] [1 unread]')
        self.child.command('close')
        self.child.repaint_until(b'unread-in-background-pane')
        self.saved_read('peer', lambda read: read.get('received') == 2)
        self.directory('peer', 0)
        self.child.finish('close')

    @unittest.skipUnless(os.environ.get('SNAJPAGENT_TEST_OLD_BINARY'),
                         'set SNAJPAGENT_TEST_OLD_BINARY to exercise an older owner')
    def test_older_owner_reads_new_checkpoint_and_exposes_unknown_counts(self):
        self.server.send('queryop', ''.join(
            f':peer!u@fake NOTICE queryop :checkpoint-message-{i}\r\n' for i in range(140)))
        self.wait(lambda: sum(e['data'].get('text', '').startswith('checkpoint-message-')
                             for e in self.events()) == 140)
        self.assertTrue(any(e['type'] == 'session_checkpoint'
                            and 'irc_activity' in e['data']['state'] for e in self.events()))
        self.directory('peer', 140)
        self.child.finish('close')
        self.term = self.start('--resume', self.sid)
        self.term.write(b'/exit\r')
        self.term.wait_exit()
        older = Path(os.environ['SNAJPAGENT_TEST_OLD_BINARY']).resolve()
        with patch.object(frontend, 'BINARY', older):
            self.term = self.start('--resume', self.sid)
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.child = self.workspace_start('--resume', 'activity-panes')
        self.child.repaint_until(b'/peer [your chat] [? unread]')
        self.query('peer')
        self.child.repaint_until(b'checkpoint-message-139')
        self.child.finish('close')


if __name__ == '__main__':
    unittest.main()
