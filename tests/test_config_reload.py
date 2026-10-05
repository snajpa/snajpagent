#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Live owners adopt shared configuration and credentials only on explicit reload."""
import http.server
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_provider_https import response
from test_remote_terminal import RemoteProcess


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/oauth/token':
            self.server.renewals.append(body)
            payload = json.dumps(dict(access_token='fixture-renewed',
                refresh_token='refresh-renewed', expires_in=3600)).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        self.server.requests.append((self.path, self.headers.get('Authorization'),
                                     self.headers.get('ChatGPT-Account-Id'), body))
        self.server.entered.set()
        assert self.server.release.wait(10), 'fixture response was not released'
        payload = response().replace(b"HTTPS answer once", b"RELOAD_OK")
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # A queued control may unwind an in-flight request.


class Fixture:
    def __init__(self, binary, root):
        self.binary, self.root = str(binary), root
        root.chmod(0o700)
        self.state = root / 'state'
        self.state.mkdir(mode=0o700)
        (self.state / 'auth').mkdir(mode=0o700)
        self.config = self.state / 'config.ini'
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.requests, self.server.renewals = [], []
        self.server.entered = threading.Event()
        self.server.release = threading.Event()
        self.server.release.set()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f'http://127.0.0.1:{self.server.server_port}'
        self.children = []

    def save_config(self, route='old', kind='api_key'):
        base = self.base + '/' + route if kind == 'api_key' else 'https://chatgpt.com/backend-api/codex'
        self.config.write_text('[agent]\nprovider=openai\nmodel=gpt-saved\nreasoning_effort=high\n'
            'read_agents_md=false\nmax_turn_retries=0\n'
            f'[provider openai]\nauth={kind}\nbase_url={base}\nexact_token_count=false\n'
            f'[provider other]\nbase_url={self.base}/other\nexact_token_count=false\n'
            '[ui]\ncolor=never\nmarkdown=false\npager=off\n'
            'prompt={provider}/{model}/{effort}{chat: CHAT>}{rollout-idle: READY>}{rollout-active: BUSY>}\n')
        self.config.chmod(0o600)
        return base

    def key(self, name, token, base, kind='api_key', qualified=False):
        suffix = '.' + kind if qualified else ''
        path = self.state / 'auth' / (name + suffix + '.json')
        path.write_text(json.dumps(dict(kind=kind, base_url=base, access_token=token,
            refresh_token='', account_id='workspace' if kind == 'codex_token' else '',
            expires_at_ms=0)))
        path.chmod(0o600)
        return path

    def start(self, *args, codex=False):
        child = RemoteProcess(self.root, [self.binary, '--dotdir', str(self.state),
            '--no-listen', '--no-client', *args], wrapped=None, winsize=(30, 200),
            extra_env={'EDITOR': 'false', 'SNAJPAGENT_TEST_OPENAI_BASE': self.base if codex else None,
                       'SNAJPAGENT_TEST_AUTH_BASE': self.base, 'NO_PROXY': '127.0.0.1', 'no_proxy': '127.0.0.1'})
        self.children.append(child)
        child.until(b'READY>', 10)
        return child

    def send(self, child, command, marker):
        child.output.clear()
        os.write(child.master, command.encode() + b'\r')
        return child.until(marker.encode(), 10)

    def query(self, child, route, token, model='gpt-saved', account=None):
        before = len(self.server.requests)
        self.send(child, 'answer once', 'RELOAD_OK')
        child.until(b'READY>', 10)
        requests = self.server.requests[before:]
        assert len(requests) == 1, len(requests)
        path, auth, workspace, body = requests[0]
        assert (path, auth, workspace, body['model']) == (route, 'Bearer ' + token, account, model), (path, auth, workspace, body['model'])

    def finish(self, child):
        self.send(child, '/exit', '--resume')
        child.wait(0)
        child.close()
        self.children.remove(child)

    def close(self):
        self.server.release.set()
        for child in self.children:
            child.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)


def check(binary):
    with tempfile.TemporaryDirectory(prefix='snag-reload-', dir='/tmp') as directory:
        fixture = Fixture(binary, Path(directory).resolve())
        f = fixture
        try:
            base = f.save_config()
            f.key('openai', 'fixture-old', base)
            f.key('other', 'fixture-other-old', f.base + '/other')
            a, b = f.start(), f.start()
            # Change both files before either owner's first request.
            base = f.save_config('new')
            f.key('openai', 'fixture-new', base)
            f.key('other', 'fixture-other-new', f.base + '/other')
            f.query(a, '/old/v1/responses', 'fixture-old')
            f.query(b, '/old/v1/responses', 'fixture-old')
            f.send(a, '/model other/gpt-switched/low', 'READY>')
            f.query(a, '/other/v1/responses', 'fixture-other-old', 'gpt-switched')
            f.send(a, '/configure', 'configuration reloaded:')
            f.query(a, '/other/v1/responses', 'fixture-other-new', 'gpt-switched')
            f.query(b, '/old/v1/responses', 'fixture-old')
            f.send(a, '/model openai/gpt-saved/high', 'READY>')
            f.query(a, '/new/v1/responses', 'fixture-new')
            valid = f.config.read_bytes()
            f.config.write_text('[invalid]\nsetting=bad\n')
            f.key('openai', 'fixture-third', base)
            f.send(a, '/configure', 'invalid configuration')
            f.query(a, '/new/v1/responses', 'fixture-new')
            f.config.write_bytes(valid)
            key_path = f.state / 'auth/openai.json'
            key_path.unlink()
            f.send(a, '/configure', 'not logged in')
            f.query(a, '/new/v1/responses', 'fixture-new')
            f.key('openai', 'fixture-third', base)
            f.send(a, '/configure', 'configuration reloaded:')
            f.query(a, '/new/v1/responses', 'fixture-third')
            # Preserve the original local name and its old credential for old binaries.
            old_key = key_path.read_bytes()
            base = f.save_config(kind='codex_token')
            f.key('openai', 'at-fixture-enterprise', base, 'codex_token', qualified=True)
            f.query(a, '/new/v1/responses', 'fixture-third')
            assert key_path.read_bytes() == old_key
            f.query(b, '/old/v1/responses', 'fixture-old')
            f.finish(a)
            f.finish(b)
            journals = list((f.state / 'sessions').glob('*/events.jsonl'))
            token = 'at-fixture-enterprise'
            for journal in journals:
                before = journal.read_bytes()
                child = f.start('--resume', journal.parent.name, codex=True)
                f.query(child, '/responses', token, account='workspace')
                f.key('openai', token + '-next', base, 'codex_token', qualified=True)
                f.query(child, '/responses', token, account='workspace')
                f.send(child, '/configure', 'configuration reloaded:')
                token += '-next'
                f.query(child, '/responses', token, account='workspace')
                f.finish(child)
                assert journal.read_bytes().startswith(before)
                text = journal.read_text()
                assert 'at-fixture-enterprise' not in text and 'fixture-third' not in text
            check_active(f)
            check_oauth(f)
            check_cache(f)
            check_secret_files(f)
            print('configuration reload: owner isolation, model selection, rollback, PAT name and resume: ok')
        finally:
            f.close()


def check_active(f):
    base = f.save_config('active')
    f.key('openai', 'fixture-active-old', base)
    # This isolated case uses the legacy file after the preceding PAT migration.
    child = f.start()
    f.server.entered.clear()
    f.server.release.clear()
    f.send(child, 'answer once', 'BUSY>')
    assert f.server.entered.wait(5)
    before = len(f.server.requests)
    f.key('openai', 'fixture-active-new', base)
    f.send(child, '/configure', '/configure accepted;')
    f.server.release.set()
    child.until(b'configuration reloaded:', 10)
    # The interrupted exchange may be continued automatically at the boundary.
    child.until(b'READY>', 10)
    for request in f.server.requests[before:]:
        assert request[1] == 'Bearer fixture-active-new'
    f.query(child, '/active/v1/responses', 'fixture-active-new')
    f.finish(child)


def check_oauth(f):
    base = f.save_config(kind='chatgpt')

    def oauth(token, account='workspace', expiry=1):
        path = f.state / 'auth/openai.chatgpt.json'
        path.write_text(json.dumps(dict(kind='chatgpt', base_url=base,
            access_token=token, refresh_token='refresh-' + token,
            account_id=account, expires_at_ms=expiry)))
        path.chmod(0o600)
        return path

    # Two owners share normal account renewal without rotating each other out.
    path = oauth('fixture-expired')
    a, b = f.start(codex=True), f.start(codex=True)
    before = len(f.server.renewals)
    f.query(a, '/responses', 'fixture-renewed', account='workspace')
    f.query(b, '/responses', 'fixture-renewed', account='workspace')
    assert len(f.server.renewals) == before + 1
    f.finish(a)
    f.finish(b)
    # A replacement account and a logout remain untouched by a live renewal.
    for replaced in (True, False):
        path = oauth('fixture-expired')
        child = f.start(codex=True)
        if replaced:
            oauth('fixture-different-login', account='another-workspace',
                  expiry=int(time.time() * 1000) + 3600000)
            saved = path.read_bytes()
        else:
            path.unlink()
        f.query(child, '/responses', 'fixture-renewed', account='workspace')
        assert path.read_bytes() == saved if replaced else not path.exists()
        f.finish(child)


def check_cache(f):
    base = f.save_config('cache')
    f.key('openai', 'fixture-cache-old', base)
    path = f.state / 'models.json'

    def save_cache(capacity, endpoint=base):
        limits = dict.fromkeys(('auto_compact_input_tokens', 'context_window_tokens',
            'effective_context_window_percent', 'input_context_window_tokens',
            'max_context_window_tokens', 'max_input_tokens', 'max_output_tokens'))
        limits['context_window_tokens'] = capacity
        model = dict(id='gpt-saved', default_effort='high', efforts=['high'], limits=limits,
            count_capability='unknown', observed_hard_input_tokens=0,
            observed_input_tokens=0, observed_input_bytes=0)
        path.write_text(json.dumps(dict(schema_version=1, updated_at_ms=1,
            providers=[dict(name='openai', base_url=endpoint, protocol='openai', models=[model])])))
        path.chmod(0o600)

    def capacity(child, value):
        output = f.send(child, '/status', 'provider model: gpt-saved')
        assert f'context={value}'.encode() in output, output[-2500:]

    save_cache(272000)
    a, b = f.start(), f.start()
    capacity(a, 272000)
    save_cache(512000)
    capacity(a, 272000)
    f.send(a, '/configure', 'configuration reloaded:')
    capacity(a, 512000)
    capacity(b, 272000)
    # A broken cache must not partially adopt new settings or credentials.
    next_base = f.save_config('cache-next')
    f.key('openai', 'fixture-cache-next', next_base)
    path.write_text('{}')
    f.send(a, '/configure', 'model cache is unusable')
    capacity(a, 512000)
    f.query(a, '/cache/v1/responses', 'fixture-cache-old')
    save_cache(1048576, next_base)
    f.send(a, '/configure', 'configuration reloaded:')
    capacity(a, 1048576)
    f.query(a, '/cache-next/v1/responses', 'fixture-cache-next')
    path.unlink()
    f.send(a, '/configure', 'configuration reloaded:')
    capacity(a, 1048576)
    # An active request admits the replacement only at the queued safe boundary.
    f.server.entered.clear()
    f.server.release.clear()
    f.send(a, 'answer once', 'BUSY>')
    assert f.server.entered.wait(5)
    save_cache(2097152, next_base)
    f.send(a, '/configure', '/configure accepted;')
    f.server.release.set()
    a.until(b'configuration reloaded:', 10)
    a.until(b'READY>', 10)
    capacity(a, 2097152)
    f.finish(a)
    f.finish(b)
    path.unlink()
    print('configuration reload: catalog isolation, metadata and rollback: ok')


def check_secret_files(f):
    f.save_config('secrets')
    f.config.write_text(f.config.read_text().replace('[provider openai]\n',
        '[provider openai]\napi_key=provider.key\n') + '[tool]\nsecret=redaction.key\n')
    key = f.state / 'provider.key'
    secret = f.state / 'redaction.key'
    key.write_text('fixture-file-old')
    secret.write_text('fixture-redaction-old')
    child = f.start()
    key.unlink()
    secret.unlink()
    f.query(child, '/secrets/v1/responses', 'fixture-file-old')
    key.write_text('fixture-file-new')
    f.send(child, '/configure', 'redaction.key is unavailable')
    f.query(child, '/secrets/v1/responses', 'fixture-file-old')
    secret.write_text('fixture-redaction-new')
    f.send(child, '/configure', 'configuration reloaded:')
    f.query(child, '/secrets/v1/responses', 'fixture-file-new')
    f.finish(child)
    print('configuration reload: file secrets retained and failed reload rolls back: ok')


if __name__ == '__main__':
    check(Path(sys.argv[1]).resolve())
