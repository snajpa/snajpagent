#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stored login commands through the CLI, including Unicode state paths.

On a POSIX host BINARY may be a Wine wrapper that translates --dotdir paths.
Only synthetic credentials are used; these commands need no provider access.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
BASE = 'https://chatgpt.com/backend-api/codex'
TOKEN = 'stored-login-fixture-token'


class LoginStorageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='snag-login-')
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name) / 'profile-ř' / '.snajpagent'
        self.state.parent.mkdir()
        self.env = dict(os.environ)
        self.env.pop('OPENAI_API_KEY', None)
        # Let the application create its private state directory and file ACLs.
        self.run_cli('-l')
        self.auth = self.state / 'auth' / 'codex.json'
        self.config = self.state / 'config.ini'
        self.config.write_text(f'''[provider codex]
auth = chatgpt
base_url = {BASE}
native_compaction = false
[agent]
provider = codex
model = fixture-model
''', encoding='utf-8')
        self.run_cli('login', 'openrouter', '--with-api-key',
                     secret='bootstrap-fixture-key\n')
        self.saved = dict(kind='chatgpt', base_url=BASE, access_token=TOKEN,
                          refresh_token='fixture-refresh', account_id='fixture-account',
                          expires_at_ms=int(time.time() * 1000) + 3600000)

    def run_cli(self, *args, secret=None, ok=True):
        result = subprocess.run([BINARY, '--dotdir', str(self.state), *args],
                                input=secret, text=True, encoding='utf-8', errors='replace',
                                capture_output=True, env=self.env, timeout=20)
        self.assertEqual(result.returncode, 0 if ok else 2, result.stdout + result.stderr)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        return result

    def save(self, **changes):
        self.auth.write_text(json.dumps(dict(self.saved, **changes)), encoding='utf-8')
        self.auth.chmod(0o600)

    def test_missing_auth_directory(self):
        for path in (self.state / 'auth').iterdir():
            path.unlink()
        (self.state / 'auth').rmdir()
        result = self.run_cli('login', 'status', 'codex')
        self.assertIn('chatgpt (not logged in)', result.stdout)
        self.assertFalse((self.state / 'auth').exists())

    def test_stored_login_reuse_and_logout(self):
        for method_path in (False, True):
            with self.subTest(method_path=method_path):
                self.auth = self.state / 'auth' / (
                    'codex.chatgpt.json' if method_path else 'codex.json')
                self.save()
                self.run_cli('login', 'codex')
                result = self.run_cli('login', 'status', 'codex')
                self.assertIn('chatgpt (stored)', result.stdout)
                self.assertEqual(json.loads(self.auth.read_text())['access_token'], TOKEN)
                self.run_cli('logout', 'codex')
                self.assertFalse(self.auth.exists())
                result = self.run_cli('login', 'status', 'codex')
                self.assertIn('chatgpt (not logged in)', result.stdout)
                self.assertTrue((self.state / 'auth' / 'openrouter.json').exists())

    def test_expired_login_and_invalid_saved_tokens(self):
        self.save(expires_at_ms=1)
        result = self.run_cli('login', 'status', 'codex')
        self.assertIn('expired; refresh on use', result.stdout)
        for changes in ({'base_url': 'https://other.invalid'}, {'access_token': 'bad token'}):
            with self.subTest(changes=changes):
                self.save(**changes)
                result = self.run_cli('login', 'status', 'codex', ok=False)
                self.assertIn('credentials are unsafe, invalid, or bound', result.stderr)

    @unittest.skipIf(os.name == 'nt' or 'WINEPREFIX' in os.environ,
                     'POSIX permission modes; Windows uses native ACLs')
    def test_nonprivate_credentials_rejected(self):
        self.save()
        for path, mode, restore in ((self.state, 0o755, 0o700),
                                    (self.auth.parent, 0o755, 0o700),
                                    (self.auth, 0o644, 0o600)):
            with self.subTest(path=path):
                path.chmod(mode)
                try:
                    self.run_cli('login', 'status', 'codex', ok=False)
                finally:
                    path.chmod(restore)


if __name__ == '__main__':
    unittest.main()
