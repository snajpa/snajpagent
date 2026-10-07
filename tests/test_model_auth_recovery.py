#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Credential failures must leave model selection and explicit reload usable."""

import json
from pathlib import Path

from store_history import journal_paths, read_events
import sys
import tempfile
import unittest

from test_config_reload import Fixture


BINARY = Path(sys.argv.pop(1)).resolve()
AUTH_ERROR = b'credentials are unsafe, invalid, or bound to another endpoint'


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='snag-auth-recovery-', dir='/tmp')
        self.addCleanup(directory.cleanup)
        self.fixture = Fixture(BINARY, Path(directory.name).resolve())
        self.addCleanup(self.fixture.close)
        self.base = self.fixture.save_config()
        # The retained login belongs to an obsolete endpoint, as after migration.
        self.fixture.key('openai', 'obsolete-fixture-key', self.base + '/obsolete')
        self.fixture.key('other', 'replacement-fixture-key', self.fixture.base + '/other')

    def events(self):
        journal, = journal_paths(self.fixture.state)
        return journal, read_events(journal)

    def failed_turn(self):
        child = self.fixture.start()
        self.fixture.send(child, 'retain this exact prompt', AUTH_ERROR.decode())
        self.assertEqual(self.fixture.server.requests, [])
        _, events = self.events()
        self.assertEqual(sum(e['type'] == 'turn_started' for e in events), 1)
        return child

    def completed(self, child, route, token, model, effort):
        child.until(b'RELOAD_OK', 5)
        child.until(b'READY>', 5)
        self.fixture.finish(child)
        self.assertEqual(len(self.fixture.server.requests), 1)
        path, auth, _, body = self.fixture.server.requests[0]
        self.assertEqual((path, auth, body['model'], body['reasoning']['effort']),
                         (route, 'Bearer ' + token, model, effort))
        self.assertIn('retain this exact prompt', json.dumps(body))
        journal, events = self.events()
        started, = (e for e in events if e['type'] == 'turn_started')
        finished, = (e for e in events if e['type'] == 'turn_completed')
        self.assertEqual(started['data']['turn_id'], finished['data']['turn_id'])
        self.assertEqual(sum(e['type'] == 'input_received' for e in events), 1)
        self.assertNotIn(token.encode(), journal.read_bytes())

    def switch(self, child):
        self.fixture.send(child, '/model other/gpt-recovered/low',
                          'model for next response in this turn:')
        self.completed(child, '/other/v1/responses', 'replacement-fixture-key',
                       'gpt-recovered', 'low')
        _, events = self.events()
        self.assertEqual(sum(e['type'] == 'turn_model_changed' for e in events), 1)

    def test_failed_turn_can_switch_to_another_provider(self):
        self.switch(self.failed_turn())

    def test_resumed_unfinished_turn_can_switch_to_another_provider(self):
        child = self.failed_turn()
        journal, _ = self.events()
        self.fixture.finish(child)
        child = self.fixture.start('--resume', journal.parent.name, ready=AUTH_ERROR)
        self.switch(child)

    def test_repaired_login_is_adopted_by_configure_during_failed_turn(self):
        child = self.failed_turn()
        self.fixture.key('openai', 'repaired-fixture-key', self.base)
        self.fixture.send(child, '/configure', '/configure accepted;')
        self.completed(child, '/old/v1/responses', 'repaired-fixture-key', 'gpt-saved', 'high')
        _, events = self.events()
        self.assertEqual(sum(e['type'] == 'turn_model_changed' for e in events), 0)


if __name__ == '__main__':
    unittest.main()
