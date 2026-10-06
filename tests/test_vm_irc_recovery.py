# SPDX-License-Identifier: GPL-2.0-only
"""Uncertain IRC bodies stay selectable without replay or recipient changes."""

import json
import socket
import unittest

from test_irc_channels import ChannelFixture
import test_vm_queries as queries
from tmux_terminal import read_events


class RecoveryTests(ChannelFixture):
    capabilities = 'batch echo-message labeled-response'
    snapshots = queries.QueryWorkspaceTests.snapshots
    wait_snapshot = queries.QueryWorkspaceTests.wait_snapshot
    workspace_start = queries.QueryWorkspaceTests.workspace_start
    state = queries.QueryWorkspaceTests.state
    normal = queries.QueryWorkspaceTests.normal

    def setUp(self):
        super().setUp()
        self.sid = read_events(self.root / 'state')[0].parent.name
        self.term.write(b'/s d\r')
        self.term.wait_exit()
        self.child = self.workspace_start('-N', 'irc-recovery')
        self.term = self.child
        self.child.command('attach ' + self.sid)
        self.child.repaint_until(b'ATTACHED')
        self.query()

    def focused_buffer(self):
        state = self.state()
        window = next(w for w in state['windows'] if w['id'] == state['focus'])
        route = window['history']['route']
        return next(b for owner in state['buffers'] for b in owner['buffers']
                    if b['route'] == route)

    def command_report(self, command):
        self.normal()
        before = {report['id'] for owner in self.state()['buffers'] for report in owner['reports']}
        self.child.write(b'i' + command.encode() + b'\r')
        self.wait_snapshot(lambda rows: rows and any(
            report['id'] not in before and report['command'] == command
            for owner in self.state()['buffers'] for report in owner['reports']))
        report = next(report for owner in self.state()['buffers'] for report in owner['reports']
                      if report['id'] not in before and report['command'] == command)
        content = self.root / 'state/sessions' / self.sid / ('.view-report-' + report['id'])
        self.normal()
        return content.read_text()

    def query(self):
        self.assertEqual(self.command_report('/query 1/peer'), '/query 1/peer\n')
        self.child.repaint_until(b'query/peer')

    def wire(self, text):
        return [line for nick, line in self.server.lines
                if nick == 'queryop' and line.startswith('@label=') and
                line.partition(' ')[2] == 'PRIVMSG peer :' + text]

    def uncertain(self, text):
        self.child.write(b'i' + text.encode() + b'\r')
        self.wait(lambda: len(self.wire(text)) == 1)
        self.normal()
        self.wait(lambda: any(e['data'].get('text') == text and
            e['data'].get('routing', {}).get('state') == 'written' for e in self.events()))
        old = self.server.links['queryop']
        joins = self.server.lines.count(('queryop', 'JOIN #lab'))
        old.shutdown(socket.SHUT_RDWR)
        self.wait(lambda: any(e['data'].get('text') == text and
            e['data'].get('routing', {}).get('state') == 'uncertain' for e in self.events()))
        self.wait(lambda: self.server.links['queryop'] is not old)
        self.wait(lambda: self.server.lines.count(('queryop', 'JOIN #lab')) > joins)
        self.server.send('queryop', ':fake NOTICE queryop :reconnected-barrier\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'reconnected-barrier'
                             for e in self.events()))
        self.assertEqual(len(self.wire(text)), 1)
        self.assertNotIn(text, json.dumps(self.seen, ensure_ascii=False))

    def recover_body(self, text):
        previous = self.focused_buffer()['route']
        self.child.write(b'/' + text.encode() + b'\r')
        self.child.repaint_until(b'Match')
        self.child.write(b'v$y')
        self.child.repaint_until(b'Yanked')
        # Reopen explicitly after copying from the old retained conversation.
        self.query()
        current = self.focused_buffer()['route']
        self.assertEqual(current['connection'], previous['connection'])
        self.assertEqual(current['peer'], 'peer')
        self.assertGreater(current['generation'], previous['generation'])
        self.assertNotEqual(current['conversation'], previous['conversation'])
        self.child.write(b'P')
        self.normal()
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and
            self.focused_buffer()['draft'] == text)
        self.assertEqual(self.focused_buffer()['route'], current)

    def test_resume_and_visual_recovery_require_explicit_send(self):
        text = 'uncertain private é界 body'
        self.uncertain(text)
        self.child.finish('close')
        self.child = self.workspace_start('--resume', 'irc-recovery')
        self.term = self.child
        self.child.repaint_until(b'ATTACHED')
        self.child.repaint_until(b'uncertain')
        self.assertEqual(len(self.wire(text)), 1)
        self.recover_body(text)
        self.assertEqual(len(self.wire(text)), 1)
        self.child.write(b'i\r')
        self.wait(lambda: len(self.wire(text)) == 2)
        self.assertNotIn(text, json.dumps(self.seen, ensure_ascii=False))
        inputs = [e['data'] for e in self.events() if e['type'] == 'input_received']
        self.assertNotIn(text, json.dumps(inputs, ensure_ascii=False))
        self.normal()
        self.child.finish('close')

    def test_recovered_body_stays_pinned_if_peer_nick_changes(self):
        text = 'uncertain message for original peer'
        self.uncertain(text)
        self.recover_body(text)
        self.server.send('queryop', ':peer!u@fake NICK :renamed-peer\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'renamed-peer'
                             for e in self.events()))
        self.child.write(b'i\r')
        self.child.repaint_until(b'Submission rejected')
        self.normal()
        self.child.command('recover')
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and
            self.focused_buffer()['draft'] == text)
        self.assertEqual(len(self.wire(text)), 1)
        self.assertFalse(any(nick == 'queryop' and 'PRIVMSG renamed-peer :' in line
                             for nick, line in self.server.lines))
        self.assertNotIn(text, json.dumps(self.seen, ensure_ascii=False))
        self.child.finish('close')

    def test_query_with_text_keeps_the_disconnected_target(self):
        self.uncertain('original uncertain body')
        command = '/query 1/peer never send this through a new connection'
        report = self.command_report(command)
        self.assertNotEqual(report, command + '\n')
        self.assertIn('IRC query changed; reopen before sending', report)
        self.assertFalse(any('never send this' in line for _, line in self.server.lines))
        self.assertNotIn('never send this', json.dumps(self.seen))
        self.child.finish('close')

    def test_join_from_disconnected_query_uses_current_connection(self):
        self.uncertain('original uncertain body')
        previous = self.focused_buffer()['route']
        self.assertEqual(self.command_report('/join 1/#new'), '/join 1/#new\n')
        self.wait(lambda: ('queryop', 'JOIN #new') in self.server.lines)
        self.child.command('workspace save')
        self.wait_snapshot(lambda rows: rows and
            self.focused_buffer()['route'].get('room') == '#new')
        current = self.focused_buffer()['route']
        self.assertEqual(current['connection'], previous['connection'])
        self.assertGreater(current['generation'], previous['generation'])
        self.child.finish('close')


if __name__ == '__main__':
    unittest.main()
