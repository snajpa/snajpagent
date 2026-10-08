# SPDX-License-Identifier: GPL-2.0-only
"""Connection views retain server traffic and pin operator commands."""

import json
import unittest

from test_irc_channels import ChannelFixture, ChannelServer
from test_irc_queries import QueryFixture
from tmux_terminal import read_events


class ConnectionTests(ChannelFixture):
    def test_refused_nick_stays_visible_and_keeps_accepted_sender_usable(self):
        self.server.rejected_nick = 'taken'
        self.command('/connections 1', 'connection ' + self.server.endpoint)
        self.command('/nick taken', 'Nick is in use')
        refusal = next(e['data'] for e in self.events()
                       if e['data'].get('text') == 'Nick is in use')
        self.assertEqual(refusal['routing']['conversation_kind'], 'connection')
        self.command('/query peer after-nick-refusal', '[peer]')
        self.operator_wire('PRIVMSG peer :after-nick-refusal')
        self.assertIn('queryop', self.server.links)
        # Selecting the query flushes its queued body through the input checkpoint.
        self.term.until(b'after-nick-refusal')
        self.term.output.clear()
        self.term.write(b'usable-query-draft\t')
        self.term.until(b'rollout')
        self.term.output.clear()
        self.term.write(b'\x1b[Z')
        self.term.until(b'[peer]')
        self.term.until(b'usable-query-draft')
        self.assert_no_wire('usable-query-draft')

    def test_open_connection_tab_keeps_draft_through_cycle(self):
        self.command('/connections 1', 'connection ' + self.server.endpoint)
        self.term.output.clear()
        self.term.write(b'connection-tab-draft\t')
        self.term.until(b'[#side]')
        self.term.output.clear()
        self.term.write(b'\x1b[Z')
        self.term.until(b'connection ' + self.server.endpoint.encode())
        self.term.until(b'connection-tab-draft')
        self.assert_no_wire('connection-tab-draft')
        self.assertNotIn('connection-tab-draft', json.dumps(self.seen))

    def test_nick_and_whois_use_the_selected_endpoint(self):
        second = ChannelServer()
        self.addCleanup(second.close)
        self.command('/connect ' + second.endpoint)
        self.wait(lambda: len(second.links) == 2)
        self.command('/connections channel-session/' + second.endpoint + '/',
                     'connection ' + second.endpoint)
        self.command('/nick endpoint-op')
        self.wait(lambda: 'endpoint-op' in second.links)
        self.wait(lambda: any(e['data'].get('kind') == 'nick' and
                             e['data'].get('text') == 'endpoint-op' for e in self.events()))
        self.assert_no_wire('NICK endpoint-op')
        self.command('/nick', 'operator nick: endpoint-op')
        self.command('/query peer', '[' + second.endpoint + '/peer]')
        self.command('/whois')
        self.wait(lambda: ('endpoint-op', 'WHOIS peer') in second.lines)
        self.assert_no_wire('WHOIS peer')
        self.assertIn('querybot', second.links)

    def test_connection_selection_rejects_plain_text_and_scopes_query(self):
        second = ChannelServer()
        self.addCleanup(second.close)
        self.command('/connect ' + second.endpoint)
        self.wait(lambda: len(second.links) == 2)
        self.command('/connections', self.server.endpoint + '/')
        self.command('/connections channel-session/' + second.endpoint + '/',
                     'connection ' + second.endpoint)
        self.command('keep-this-connection-draft', 'explicit target')
        self.assert_no_wire('keep-this-connection-draft')
        self.assertNotIn('keep-this-connection-draft', json.dumps(self.seen))
        self.command('/query remote-peer pinned-query', '[' + second.endpoint + '/remote-peer]')
        self.wait(lambda: ('queryop', 'PRIVMSG remote-peer :pinned-query') in second.lines)
        self.assert_no_wire('pinned-query')

    def test_server_notices_and_whois_stay_out_of_queries_and_model_context(self):
        self.command('/connections 1', 'connection ' + self.server.endpoint)
        self.server.send('queryop', ':fake NOTICE queryop :operator-server-notice\r\n')
        self.server.send('querybot', ':fake NOTICE querybot :agent-server-notice\r\n')
        self.term.until(b'operator-server-notice')
        self.term.until(b'agent-server-notice')
        self.command('/whois peer')
        self.operator_wire('WHOIS peer')
        self.server.send('queryop', ':fake 311 queryop peer user host * :whois-detail\r\n'
                         ':fake 318 queryop peer :End of WHOIS\r\n')
        self.term.until(b'whois-detail')
        notices = [e['data'] for e in self.events()
                   if 'server-notice' in e['data'].get('text', '') or
                   'whois-detail' in e['data'].get('text', '')]
        self.assertEqual(len(notices), 3)
        self.assertTrue(all(e['routing']['conversation_kind'] == 'connection' for e in notices))
        self.submit('check-server-notice-isolation')
        self.assertNotIn('operator-server-notice', json.dumps(self.seen))
        self.assertNotIn('agent-server-notice', json.dumps(self.seen))
        self.assertNotIn('whois-detail', json.dumps(self.seen))
        self.server.send('queryop', ':fake NOTICE #side :public-server-notice\r\n')
        self.wait(lambda: any(e['data'].get('text') == 'public-server-notice'
                             for e in self.events()))
        public = [e['data'] for e in self.events()
                  if e['data'].get('text') == 'public-server-notice']
        self.assertEqual(public[0]['routing']['conversation_kind'], 'channel')
        sid = read_events(self.root / 'state')[0].parent.name
        self.command('/exit')
        self.term.wait_exit()
        self.term = self.start('--resume', sid)
        self.wait_idle()
        self.command('/connections 1', 'connection ' + self.server.endpoint)
        self.submit('check-resumed-notice-isolation')
        self.assertNotIn('operator-server-notice', json.dumps(self.seen))
        self.assertNotIn('agent-server-notice', json.dumps(self.seen))
        self.assertNotIn('whois-detail', json.dumps(self.seen))


class HostedConnectionTests(QueryFixture):
    def test_whois_uses_hosted_peer_without_extra_connection(self):
        self.term.write(b'/connections 1\r')
        self.term.until(b'connection ')
        self.term.write(b'/whois query-peer\r')
        self.term.until(b'WHOIS query-peer: user')
        self.assertFalse(any(e['data'].get('routing', {}).get('conversation_kind') == 'query'
                             for e in self.events()))


if __name__ == '__main__':
    unittest.main()
