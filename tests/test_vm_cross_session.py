# SPDX-License-Identifier: GPL-2.0-only
"""Qualified IRC commands use the addressed owner and preserve both composers."""

import json
import signal
import unittest
import uuid

from test_irc_channels import ChannelFixture
from test_vm_frontend import Terminal
import test_vm_queries as queries
from tmux_terminal import read_events


class CrossSessionTests(ChannelFixture):
    snapshots = queries.QueryWorkspaceTests.snapshots
    wait_snapshot = queries.QueryWorkspaceTests.wait_snapshot
    workspace_start = queries.QueryWorkspaceTests.workspace_start
    state = queries.QueryWorkspaceTests.state
    normal = queries.QueryWorkspaceTests.normal

    def events(self):
        if not hasattr(self, 'source_path'):
            return super().events()
        return [json.loads(line) for line in self.source_path.read_bytes().split(b'\n')[:-1]
                if line.strip()]

    def setUp(self):
        super().setUp()
        self.source_path = read_events(self.root / 'state')[0]
        self.source = self.source_path.parent.name
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.other = self.start('--no-listen', '-c', self.server.endpoint,
                                '-n', 'secondbot', '-o', 'secondop', '-N', 'other session',
                                '-r', 'lab', ready=b'secondop@')
        self.wait(lambda: len(self.server.links) == 4)
        paths = list((self.root / 'state/sessions').glob('*/events.jsonl'))
        self.target_path, = [path for path in paths if path != self.source_path]
        self.target = self.target_path.parent.name
        self.other.write(b'/s d\r')
        self.other.wait_exit()
        self.child = self.workspace_start('-N', 'cross-session')
        self.child.command('attach ' + self.source)
        self.child.repaint_until(b'ATTACHED ' + self.source[:8].encode())
        self.child.command('vsp')
        self.child.command('attach ' + self.target)
        self.child.repaint_until(b'ATTACHED ' + self.target[:8].encode())
        self.child.write(b'ikeep-target-rollout')
        self.normal()
        self.child.write(b'\x17w')
        self.term = self.child
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and len(self.state()['buffers']) == 2 and
                           self.rollout(self.target)['base'])

    def owner(self, sid):
        return next(row for row in self.state()['buffers'] if row['session'] == sid)

    def rollout(self, sid):
        return next(b for b in self.owner(sid)['buffers'] if b['route'] == 'rollout')

    def finish(self):
        self.child.command('close')
        self.child.finish('close')

    def test_mouse_insert_routes_commands_and_drafts_to_clicked_owner(self):
        from test_vm_mouse import mouse

        child = self.child
        requests = len(self.seen)
        mouse(child, 1, 1)
        mouse(child, 1, 1, release=True)
        child.write(b'/fast off\r')
        self.wait_snapshot(lambda rows: rows and any(
            report['command'] == '/fast off' for report in self.owner(self.source)['reports']))
        self.assertFalse(self.owner(self.target)['reports'])
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        child.write(b'\x1b')
        child.read(.08)
        child.command('history ' + self.source)
        child.repaint_until(b'ATTACHED')
        # Clicking the other pane preserves its remembered draft position.
        mouse(child, 1, 112)
        mouse(child, 1, 112, release=True)
        child.write(b'RIGHT')
        self.wait_snapshot(lambda rows: rows and
                           self.rollout(self.target)['draft'] == 'keep-target-rollouRIGHTt')
        self.assertEqual(self.rollout(self.source)['draft'], '')
        self.normal()
        self.assertEqual(len(self.seen), requests)
        child.finish('session detach')

    def test_query_uses_named_owner_and_preserves_target_draft(self):
        address = 'other session/' + self.server.endpoint + '/peer'
        command = '/query "' + address + '" hello-target'
        self.child.write(b'i' + command.encode() + b'\r')
        self.wait(lambda: ('secondop', 'PRIVMSG peer :hello-target') in self.server.lines)
        self.assertEqual([nick for nick, line in self.server.lines if 'hello-target' in line],
                         ['secondop'])
        self.child.repaint_until(b'query/peer')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['windows'][0]['history']['session']
                           == self.target and isinstance(
                               self.state()['windows'][0]['history']['route'], dict))
        route = self.state()['windows'][0]['history']['route']
        self.assertEqual(route['peer'], 'peer')
        owners = {row['session']: row for row in self.state()['buffers']}
        rollout = next(b for b in owners[self.target]['buffers'] if b['route'] == 'rollout')
        self.assertEqual(rollout['draft'], 'keep-target-rollout')
        self.assertEqual(owners[self.target]['reports'][-1]['command'], command)
        self.assertTrue(all(not b['pending'] for row in owners.values() for b in row['buffers']))
        self.assertNotIn('hello-target', json.dumps(self.seen))
        self.finish()

    def test_prefix_notice_and_connection_selection_use_target_owner(self):
        address = self.target[:8] + '/' + self.server.endpoint
        self.child.write(b'i/notice ' + address.encode() + b'/peer private-notice\r')
        self.wait(lambda: ('secondop', 'NOTICE peer :private-notice') in self.server.lines)
        self.child.repaint_until(b'private-notice')
        self.normal()
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.owner(self.target)['reports'])
        self.assertEqual(self.state()['windows'][0]['history']['session'], self.source)
        self.assertIsNone(self.state()['windows'][0]['history']['route'])
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        self.assertNotIn('private-notice', json.dumps(self.seen))
        self.child.write(b'i/connections ' + address.encode() + b'/\r')
        self.child.repaint_until(b'connection/' + self.server.endpoint.encode() + b' [operator]')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.state()['windows'][0]['history']['session']
                           == self.target)
        route = self.state()['windows'][0]['history']['route']
        self.assertEqual(route['endpoint'], self.server.endpoint)
        self.assertNotIn('peer', route)
        self.assertNotIn('room', route)
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        self.finish()

    def test_receipt_preserves_newer_source_typing_and_pane(self):
        address = self.target + '/' + self.server.endpoint + '/peer'
        self.child.write(b'i/query ' + address.encode() + b' hello-once\rnewer-source-draft')
        self.wait(lambda: ('secondop', 'PRIVMSG peer :hello-once') in self.server.lines)
        self.wait_snapshot(lambda rows: rows and self.owner(self.target)['reports'] and
                           not self.rollout(self.target)['pending'])
        self.assertEqual(self.rollout(self.source)['draft'], 'newer-source-draft')
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        self.assertEqual(self.state()['windows'][0]['history']['session'], self.source)
        self.assertIsNone(self.state()['windows'][0]['history']['route'])
        self.assertEqual([line for nick, line in self.server.lines if 'hello-once' in line],
                         ['PRIVMSG peer :hello-once'])
        self.normal()
        self.finish()

    def test_other_controller_is_preserved_and_command_stays_in_source(self):
        self.child.write(b'\x17w')
        self.child.command('detach')
        self.child.repaint_until(b'Detached; owner continues running')
        self.child.repaint_until(b'read-only ' + self.target[:8].encode())
        classic = Terminal(self.root, ('--attach', self.target), subcommand='')
        self.addCleanup(classic.close)
        classic.until(b'secondop@')
        self.child.write(b'\x17w')
        command = '/query ' + self.target + '/' + self.server.endpoint + '/peer refused-text'
        self.child.write(b'i' + command.encode() + b'\r')
        self.child.repaint_until(b'Addressed session is read-only')
        self.normal()
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.rollout(self.source)['draft'] == command)
        self.assertIsNone(classic.process.poll())
        self.assertFalse(any('refused-text' in line for nick, line in self.server.lines))
        self.assertFalse(self.owner(self.target)['control'])
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        classic.write(b'\x15/s d\r')
        classic.wait_exit()
        self.finish()

    def test_channel_selection_join_and_part_use_addressed_owner(self):
        address = self.target[:8] + '/' + self.server.endpoint
        self.child.write(b'i/chat ' + address.encode() + b'/#side\r')
        self.child.repaint_until(b'channel/#side')
        self.child.command('buffer ' + self.source)
        self.child.write(b'i/join ' + address.encode() + b'/#fresh\r')
        self.wait(lambda: ('secondop', 'JOIN #fresh') in self.server.lines)
        self.child.repaint_until(b'channel/#fresh')
        self.child.command('buffer ' + self.source)
        self.child.write(b'i/part ' + address.encode() + b'/#fresh\r')
        self.wait(lambda: any(nick == 'secondop' and line.startswith('PART #fresh')
                              for nick, line in self.server.lines))
        self.wait_snapshot(lambda rows: rows and len(self.owner(self.target)['reports']) == 3 and
                           not self.rollout(self.target)['pending'])
        self.assertFalse(any(nick != 'secondop' and '#fresh' in line
                             for nick, line in self.server.lines))
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        self.normal()
        self.finish()

    def test_saved_unknown_receipt_recovers_only_origin_without_replay(self):
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.rollout(self.target)['base'])
        self.child.signal(signal.SIGTERM)
        self.child.wait_exit()
        self.assertEqual(self.child.process.poll(), 0)
        path, = (self.root / 'state/workspaces').glob('*/workspace.json')
        saved = json.loads(path.read_text())
        owner = next(row for row in saved['state']['buffers'] if row['session'] == self.target)
        target = next(b for b in owner['buffers'] if b['route'] == 'rollout')
        command = '/msg ' + self.target + '/' + self.server.endpoint + '/peer retained-text'
        target['pending'] = {'id': uuid.uuid4().hex, 'instance': target['base']['instance'],
                             'text': command, 'origin': {'session': self.source, 'route': 'rollout'}}
        path.write_text(json.dumps(saved))
        self.child = self.workspace_start('--resume', 'cross-session')
        self.term = self.child
        self.child.repaint_until(b'Submission outcome unknown')
        self.assertFalse(any('retained-text' in line for nick, line in self.server.lines))
        self.child.command('recover')
        self.child.repaint_until(b'Submission recovered')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and self.rollout(self.source)['draft'] == command)
        self.assertEqual(self.rollout(self.target)['draft'], 'keep-target-rollout')
        self.assertIsNone(self.rollout(self.target)['pending'])
        self.assertEqual(self.state()['v'], 13)
        self.assertFalse(any('retained-text' in line for nick, line in self.server.lines))
        self.finish()


if __name__ == '__main__':
    unittest.main()
