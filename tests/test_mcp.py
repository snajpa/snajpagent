#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Native MCP over real HTTP, OAuth callbacks and model tool dispatch."""

import base64
import hashlib
import http.server
import json
import os
import re
import select
import signal
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import urllib.request
from pathlib import Path

from tmux_terminal import FakeResponses, read_events
from store_history import journal_paths

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else Path('./snajpagent').resolve()
_test_arguments = sys.argv[:]
sys.argv[:] = sys.argv[:1]
from test_irc_queries import QueryFixture
import test_vm_frontend
sys.argv[:] = _test_arguments
test_vm_frontend.BINARY = BINARY

SCHEMA = {'type': 'object', 'properties': {
    'destination': {'type': 'string', 'x-mcp-header': 'Destination'},
    'message': {'type': 'string', 'maxLength': 100, 'default': 'hello'},
    'mode': {'type': 'string', 'enum': ['exact', 'draft']}},
    'required': ['destination', 'message'], 'additionalProperties': False}
TOOL = {'name': 'send.message', 'description': 'Send exactly this message.',
        'inputSchema': SCHEMA, 'annotations': {'readOnlyHint': False},
        'outputSchema': {'type': 'object', 'properties': {'id': {'type': 'string'}}}}


class MCPServer:
    def __init__(self, ipv6=False, tls=None):
        self.requests = []
        self.calls = []
        self.tokens = []
        self.tools = [TOOL]
        self.legacy = False
        self.sse = False
        self.disconnect = False
        self.duplicate = False
        self.oauth = False
        self.expires = 3600
        self.failure = None
        self.changed = False
        self.pages = False
        self.cursor_loop = False
        self.delay = 0
        self.started = threading.Event()
        self.http_status = None
        self.issuer_override = None
        self.resource_override = None
        self.pkce = ['S256']
        self.echo_secret = False
        self.tool_error = False
        self.wrong_id = False
        self.watch_enabled = False
        self.subscribed = threading.Event()
        self.notify_change = threading.Event()
        self.watch_closed = threading.Event()
        self.stopping = threading.Event()
        self.large_result = False
        self.endless_pages = False
        self.token_error = None
        self.omit_refresh = False
        self.catalog_secret = False
        self.content_type = 'application/json'
        self.rpc_error = None
        self.input_required = False
        self.result_number = False
        self.auth_methods = ['none']
        self.token_headers = []
        self.call_received = threading.Event()
        self.release_call = threading.Event()
        self.hold_call = False
        self.redirect = None
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def reply(self, status, value, headers=None):
                body = value if isinstance(value, bytes) else json.dumps(value).encode()
                self.send_response(status)
                for key, value in (headers or {}).items():
                    self.send_header(key, value)
                self.send_header('Content-Type', owner.content_type)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

            def watch(self, request_id=None):
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Connection', 'close')
                self.end_headers()
                meta = {'io.modelcontextprotocol/subscriptionId': request_id}
                try:
                    if request_id is not None:
                        self.wfile.write(b'data: ' + json.dumps({'jsonrpc': '2.0',
                            'method': 'notifications/subscriptions/acknowledged',
                            'params': {'_meta': meta, 'notifications': {
                                'toolsListChanged': True}}}).encode() + b'\n\n')
                        self.wfile.flush()
                    owner.subscribed.set()
                    while not owner.stopping.wait(.02):
                        if owner.notify_change.is_set():
                            owner.notify_change.clear()
                            self.wfile.write(b'data: ' + json.dumps({'jsonrpc': '2.0',
                                'method': 'notifications/tools/list_changed',
                                'params': {'_meta': meta}}).encode() + b'\n\n')
                        else:
                            self.wfile.write(b': keepalive\n\n')
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    owner.watch_closed.set()
                    self.close_connection = True

            def do_GET(self):
                owner.requests.append(('GET', self.path, dict(self.headers)))
                if self.path == '/mcp' and owner.watch_enabled and owner.legacy:
                    self.watch()
                elif self.path == '/mcp':
                    self.reply(401, {}, {'WWW-Authenticate':
                        f'Bearer resource_metadata="{owner.url}/resource", scope="read write"'})
                elif self.path in ('/resource', '/.well-known/oauth-protected-resource/mcp'):
                    self.reply(200, {'resource': owner.resource_override or owner.url + '/mcp',
                                     'authorization_servers': [owner.url]})
                elif self.path == '/.well-known/oauth-authorization-server':
                    self.reply(200, {'issuer': owner.issuer_override or owner.url,
                        'authorization_endpoint': owner.url + '/authorize',
                        'token_endpoint': owner.url + '/token',
                        'revocation_endpoint': owner.url + '/revoke',
                        'code_challenge_methods_supported': owner.pkce,
                        'token_endpoint_auth_methods_supported': owner.auth_methods,
                        'revocation_endpoint_auth_methods_supported': ['client_secret_basic'],
                        'authorization_response_iss_parameter_supported': True})
                else:
                    self.reply(404, {})

            def do_DELETE(self):
                owner.requests.append(('DELETE', self.path, dict(self.headers)))
                self.reply(200, {})

            def do_POST(self):
                raw = self.rfile.read(int(self.headers['Content-Length']))
                if self.path in ('/token', '/revoke'):
                    fields = urllib.parse.parse_qs(raw.decode())
                    owner.tokens.append((self.path, fields))
                    owner.token_headers.append(dict(self.headers))
                    if self.path == '/revoke':
                        self.reply(200, {})
                    elif owner.token_error:
                        self.reply(400, {'error': owner.token_error})
                    else:
                        response = {'access_token': 'fixture-access-secret',
                            'refresh_token': 'fixture-refresh-secret', 'token_type': 'Bearer',
                            'expires_in': owner.expires, 'scope': 'read write',
                            'resource': owner.url + '/mcp'}
                        if owner.omit_refresh:
                            response.pop('refresh_token')
                        self.reply(200, response)
                    return
                request = json.loads(raw)
                owner.requests.append((request, dict(self.headers)))
                method = request['method']
                owner.started.set()
                if owner.delay:
                    time.sleep(owner.delay)
                if owner.rpc_error:
                    self.reply(400, {'jsonrpc': '2.0', 'id': request['id'], 'error': owner.rpc_error})
                    return
                if owner.redirect:
                    self.reply(307, {}, {'Location': owner.redirect})
                    return
                if owner.http_status:
                    self.reply(owner.http_status, {})
                    return
                if owner.oauth and self.headers.get('Authorization') != 'Bearer fixture-access-secret':
                    self.reply(401, {})
                    return
                if owner.legacy and not self.headers.get('Mcp-Session-Id') and method != 'initialize':
                    self.reply(400, b'')
                    return
                if method == 'initialize':
                    self.reply(200, {'jsonrpc': '2.0', 'id': request['id'], 'result': {
                        'protocolVersion': '2025-06-18', 'capabilities': {'tools': {'listChanged': True}},
                        'serverInfo': {'name': 'fixture', 'version': '1'}}},
                        {'Mcp-Session-Id': 'fixture-session'})
                    return
                if method.startswith('notifications/'):
                    self.reply(202, b'')
                    return
                if method == 'subscriptions/listen' and owner.watch_enabled:
                    self.watch(request['id'])
                    return
                if method == 'tools/list':
                    tools = owner.tools * (2 if owner.duplicate else 1)
                    result = {'tools': tools, 'ttlMs': 60000}
                    if owner.catalog_secret:
                        result['secret'] = 'fixture-access-secret'
                    if owner.endless_pages:
                        result = {'tools': [], 'nextCursor': str(len(owner.requests))}
                    if owner.pages:
                        if 'cursor' not in request['params']:
                            result = {'tools': [dict(TOOL, name='first')], 'nextCursor': 'page2'}
                        elif owner.cursor_loop:
                            result = {'tools': [], 'nextCursor': 'page2'}
                elif method == 'tools/call':
                    owner.calls.append(request['params'])
                    owner.call_received.set()
                    if owner.hold_call:
                        owner.release_call.wait(8)
                    if owner.disconnect:
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.connection.close()
                        self.close_connection = True
                        return
                    result = {'content': [{'type': 'text', 'text': 'fixture-access-secret' if owner.echo_secret else 'accepted'},
                              {'type': 'resource_link', 'uri': 'https://example.test/item/42',
                               'name': 'receipt'}], 'structuredContent': {'id': '42'},
                              '_meta': {'server': 'fixture'}, 'isError': owner.tool_error}
                    if owner.input_required:
                        result = {'resultType': 'input_required', 'inputRequests': [
                            {'method': 'elicitation/create', 'params': {'message': 'consent'}}]}
                    if owner.result_number:
                        result['structuredContent']['measurement'] = 1.25
                    if owner.large_result:
                        result['content'][0]['text'] = 'a' * (3 * 1024 * 1024)
                else:
                    self.reply(400, {})
                    return
                envelope = {'jsonrpc': '2.0', 'id': request['id'] + int(owner.wrong_id), 'result': result}
                if owner.sse:
                    wire = b''
                    if owner.changed:
                        wire += b'data: {"jsonrpc":"2.0","method":"notifications/tools/list_changed"}\n\n'
                    wire += b'data: ' + json.dumps(envelope).encode() + b'\n\n'
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/event-stream')
                    self.send_header('Content-Length', str(len(wire)))
                    self.end_headers()
                    self.wfile.write(wire)
                    self.wfile.flush()
                    self.close_connection = True
                else:
                    self.reply(200, envelope)

        class Server(http.server.ThreadingHTTPServer):
            address_family = socket.AF_INET6 if ipv6 else socket.AF_INET

        self.server = Server(('::1' if ipv6 else '127.0.0.1', 0), Handler)
        if tls:
            self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.url = ('https' if tls else 'http') + '://' + ('[::1]' if ipv6 else '127.0.0.1')
        self.url += f':{self.server.server_address[1]}'

    def close(self):
        self.stopping.set()
        self.release_call.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class MCPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='snag-mcp-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.server = MCPServer()
        self.addCleanup(self.server.close)
        self.provider = FakeResponses()
        self.addCleanup(self.provider.close)
        self.state = self.root / 'state'
        self.config = self.root / 'config'
        self.policy = ''
        self.write_config()
        self.env = dict(os.environ, SNAJPAGENT_IRC_UI_KEY='irc-ui-secret', PAGER='')
        for key in ('OPENAI_API_KEY', 'HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY'):
            self.env.pop(key, None)
        self.env['NO_PROXY'] = '*'

    def write_config(self):
        self.config.write_text(
            '[agent]\nprovider = fixture\nmodel = host-model\nread_agents_md = false\n'
            'auto_update = false\nmax_turn_retries = 0\n'
            f'[provider fixture]\nbase_url = http://127.0.0.1:{self.provider.port}\n'
            'api_key = ${SNAJPAGENT_IRC_UI_KEY}\nexact_token_count = false\n'
            f'[mcp fixture]\nurl = {self.server.url}/mcp\nissuer = {self.server.url}\n'
            'client_id = fixture-client\ntimeout_ms = 3000\n' + self.policy +
            '[ui]\ncolor = never\npager = off\n')

    def args(self, *args):
        return [str(BINARY), '--config', str(self.config), '--dotdir', str(self.state), *args]

    def run_cli(self, *args, ok=True):
        p = subprocess.run(self.args('mcp', *args), cwd=self.root, env=self.env,
                           capture_output=True, timeout=12)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr.decode())
        return p

    def test_catalog_exact(self):
        result = self.run_cli('tools', 'fixture')
        self.assertIn(b'"x-mcp-header": "Destination"', result.stdout)
        request, headers = self.server.requests[0]
        self.assertEqual(headers['Mcp-Method'], 'tools/list')
        self.assertEqual(headers['MCP-Protocol-Version'], '2026-07-28')
        self.assertEqual(request['params']['_meta']['io.modelcontextprotocol/protocolVersion'],
                         '2026-07-28')

    def test_legacy_initializes_and_binds_session(self):
        self.server.legacy = True
        result = self.run_cli('tools', 'fixture')
        self.assertIn(b'send.message', result.stdout)
        methods = [r[0]['method'] for r in self.server.requests if isinstance(r[0], dict)]
        self.assertEqual(methods, ['tools/list', 'initialize', 'notifications/initialized', 'tools/list'])
        self.assertEqual(self.server.requests[-2][1]['Mcp-Session-Id'], 'fixture-session')
        self.assertEqual(self.server.requests[-2][1]['MCP-Protocol-Version'], '2025-06-18')

    def test_duplicate_catalog_refused(self):
        self.server.duplicate = True
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'duplicate', result.stderr)

    def test_invalid_header_tool_does_not_hide_valid_tool(self):
        bad = dict(TOOL, name='bad', inputSchema={'type': 'object', 'properties': {
            'secret': {'type': 'string', 'x-mcp-header': 'bad\r\nInjected'}}})
        self.server.tools = [bad, TOOL]
        result = self.run_cli('tools', 'fixture')
        self.assertIn(b'invalid input schema or x-mcp-header', result.stdout)
        self.assertIn(b'send.message', result.stdout)

    def model_call(self, permitted=True, disconnect=False, sse=False, read_only=False,
                   arguments=None, expected_schema=SCHEMA):
        self.policy = ('read_only_tools' if read_only else 'allow_tools') + ' = ["send.message"]\n' if permitted else ''
        self.write_config()
        self.server.disconnect = disconnect
        self.server.sse = sse
        requests = []
        if arguments is None:
            arguments = {'destination': 'channel-42', 'message': '  exact\nformat\n', 'mode': 'exact'}

        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                tools = [t for t in request['tools'] if t.get('name', '').startswith('mcp_')]
                self.assertEqual(len(tools), 1)
                self.assertEqual(tools[0]['parameters'], expected_schema)
                self.assertIn('outputSchema', tools[0]['description'])
                body = self.provider.function_body(sequence, 'mcp-call', tools[0]['name'], arguments)
            else:
                body = self.provider.response_body(sequence, 'MCP finished')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        result = subprocess.run(self.args('-e', '--', 'exercise MCP'), cwd=self.root, env=self.env,
                                capture_output=True, timeout=15)
        self.assertIsNone(self.provider.failure)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(len(requests), 2)
        _, events = read_events(self.state)
        results = [e['data']['result'] for e in events if e['type'] == 'tool_finished']
        self.assertEqual(len(results), 1)
        return results[0], requests

    def test_real_model_dispatch_and_durable_structured_result(self):
        result, requests = self.model_call()
        self.assertEqual(result['status'], 'succeeded')
        receipt = json.loads(result['model_text'])
        self.assertEqual(receipt['result']['structuredContent'], {'id': '42'})
        self.assertEqual(receipt['status'], 'server_accepted')
        self.assertEqual(self.server.calls[0]['arguments']['message'], '  exact\nformat\n')
        headers = next(h for r, h in self.server.requests if r['method'] == 'tools/call')
        self.assertEqual(headers['Mcp-Param-Destination'], 'channel-42')
        self.assertIn('resource_link', json.dumps(requests[-1]))

    def test_unapproved_call_has_no_external_effect(self):
        result, _ = self.model_call(permitted=False)
        self.assertEqual(result['status'], 'not_run')
        self.assertEqual(result['reason'], 'mcp_approval_required')
        self.assertEqual(self.server.calls, [])

    def test_uncertain_mutation_is_never_retried(self):
        result, _ = self.model_call(disconnect=True)
        self.assertEqual(result['status'], 'outcome_unknown', (result, self.server.calls))
        self.assertEqual(len(self.server.calls), 1)

    def test_sse_tool_result(self):
        result, _ = self.model_call(sse=True)
        self.assertEqual(result['status'], 'succeeded')

    @staticmethod
    def close_login(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=3)

    def login(self, bad_state=False, callback_mode='paste'):
        p = subprocess.Popen(self.args('mcp', 'login', 'fixture'), cwd=self.root, env=self.env,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.close_login, p)
        output = bytearray()
        deadline = time.monotonic() + 8
        authorize = None
        while time.monotonic() < deadline:
            ready, _, _ = select.select([p.stderr], [], [], .1)
            if ready:
                data = os.read(p.stderr.fileno(), 8192)
                if not data:
                    break
                output.extend(data)
                match = re.search(rb'(http://[^\s]+/authorize\?[^\s]+)', output)
                if match:
                    authorize = match[1].decode()
                    break
        self.assertIsNotNone(authorize, bytes(output))
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(authorize).query)
        self.assertEqual(query['resource'], [self.server.url + '/mcp'])
        callback = query['redirect_uri'][0] + '?' + urllib.parse.urlencode({
            'code': 'fixture-code', 'state': 'wrong' if bad_state else query['state'][0],
            'iss': self.server.url})
        if callback_mode == 'issuer':
            callback = callback.replace(urllib.parse.quote(self.server.url, safe=''), 'wrong-issuer')
            bad_state = True
        if callback_mode == 'path':
            callback = callback.replace('/callback?', '/wrong?')
            bad_state = True
        if callback_mode == 'duplicate':
            callback += '&state=' + query['state'][0]
            bad_state = True
        if callback_mode in ('host', 'origin'):
            headers = {'Host': 'evil.test'} if callback_mode == 'host' else {'Origin': 'https://evil.test'}
            with self.assertRaises(urllib.error.HTTPError) as refused:
                urllib.request.urlopen(urllib.request.Request(callback, headers=headers), timeout=5)
            self.assertEqual(refused.exception.code, 400)
            supplied = b''
            bad_state = True
        elif callback_mode == 'http':
            with urllib.request.urlopen(callback, timeout=5) as reply:
                self.assertEqual(reply.status, 200)
                reply.read()
            supplied = b''
        else:
            supplied = (callback + '\n').encode()
        stdout, stderr = p.communicate(supplied, timeout=8)
        output.extend(stderr)
        if not bad_state:
            self.assertEqual(p.returncode, 0, bytes(output))
            token = [fields for path, fields in self.server.tokens if path == '/token'][-1]
            challenge = base64.urlsafe_b64encode(hashlib.sha256(
                token['code_verifier'][0].encode()).digest()).rstrip(b'=').decode()
            self.assertEqual(query['code_challenge'], [challenge])
            self.assertEqual(token['resource'], [self.server.url + '/mcp'])
            self.assertNotIn(b'fixture-access-secret', stdout + output)
            self.assertNotIn(b'fixture-code', stdout + output)
        return p.returncode

    def test_oauth_pkce_private_storage_refresh_and_logout(self):
        self.server.expires = 1
        self.login()
        files = [p for p in (self.state / 'mcp-auth').iterdir() if '.' not in p.name]
        self.assertEqual(len(files), 1)
        self.assertEqual(files[0].stat().st_mode & 0o777, 0o600)
        self.server.oauth = True
        self.run_cli('tools', 'fixture')
        self.assertEqual(self.server.tokens[-1][1]['grant_type'], ['refresh_token'])
        self.run_cli('logout', 'fixture')
        self.assertFalse(files[0].exists())
        self.assertEqual(self.server.tokens[-1][0], '/revoke')

    process_identity = staticmethod(QueryFixture.process_identity)

    def interactive(self):
        term = test_vm_frontend.Terminal(self.root, ('--config', str(self.config),
            '--no-listen', '--no-client'), subcommand=None, extra_env=self.env)
        owners = QueryFixture.fixture_owners(self, term, b'host-model')
        self.addCleanup(QueryFixture.close_terminal, self, term, owners)
        return term

    def wait_result(self, term, count, tools=True):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            term.read(.02)
            if not journal_paths(self.state):
                continue
            _, events = read_events(self.state)
            results = [e['data']['result'] for e in events if e['type'] == 'tool_finished']
            finished = [e for e in events if e['type'] == 'turn_completed']
            if len(finished) >= count and (not tools or len(results) >= count):
                return results[-1] if tools else finished[-1]
        self.fail(bytes(term.output[-5000:]))

    def test_exact_approval_changed_arguments_and_single_use_in_terminal(self):
        count = 0
        arguments = {'destination': 'channel-42', 'message': 'first\n  body', 'mode': 'draft'}

        def respond(handler, request, sequence):
            nonlocal count
            count += 1
            if count % 2:
                tool = next(t for t in request['tools'] if t.get('name', '').startswith('mcp_'))
                body = self.provider.function_body(sequence, f'approval-{count}', tool['name'], arguments)
            else:
                body = self.provider.response_body(sequence, 'approval cycle finished')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        term = self.interactive()
        term.write(b'propose\r')
        result = self.wait_result(term, 1)
        digest = re.search(r'/mcp pending ([0-9a-f]{64})', result['model_text']).group(1)
        term.output.clear()
        term.write(f'/mcp approve {digest}\r'.encode())
        term.until(b'Inspect /mcp pending')
        self.assertEqual(self.server.calls, [])
        term.output.clear()
        term.write(f'/mcp pending {digest}\r'.encode())
        term.until(b'channel-42')
        self.assertIn(b'first', term.output)
        term.output.clear()
        term.write(f'/mcp approve {digest}\r'.encode())
        term.until(b'Approved exactly one')
        arguments['message'] = 'changed'
        term.write(b'changed payload\r')
        changed = self.wait_result(term, 2)
        self.assertEqual(changed['status'], 'not_run')
        self.assertEqual(self.server.calls, [])
        arguments['message'] = 'first\n  body'
        term.write(b'exact payload\r')
        self.assertEqual(self.wait_result(term, 3)['status'], 'succeeded')
        term.write(b'same payload again\r')
        self.assertEqual(self.wait_result(term, 4)['status'], 'not_run')
        self.assertEqual(len(self.server.calls), 1)
        self.assertEqual(self.server.calls[0]['arguments'], arguments)
        term.write(b'/exit\r')
        term.wait_exit()

    def test_resume_preserves_original_tool_schema(self):
        held = threading.Event()
        release = threading.Event()
        first = []

        def hold(handler, request, sequence):
            first.append(request)
            held.set()
            release.wait(8)
            handler.close_connection = True

        self.policy = 'allow_tools = ["send.message"]\n'
        self.write_config()
        self.provider.runtime_handler = hold
        p = subprocess.Popen(self.args('-e', '--', 'unfinished MCP request'), cwd=self.root,
                             env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.close_login, p)
        self.addCleanup(release.set)
        self.assertTrue(held.wait(5))
        p.kill()
        p.communicate(timeout=3)
        release.set()
        self.server.tools = [dict(TOOL, inputSchema={'type': 'object', 'properties': {
            'replacement': {'type': 'string'}}})]
        seen = []

        def finish(handler, request, sequence):
            seen.append(request)
            self.provider.reply(handler, self.provider.response_body(
                sequence, 'resumed original contract').encode(), close_header=True)

        self.provider.runtime_handler = finish
        snapshot = next(self.state.glob('sessions/*/mcp-turn.json'))
        session = snapshot.parent.name
        result = subprocess.run(self.args('-e', '--resume', session), cwd=self.root,
                                env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(len(seen), 1)
        original = [t for t in first[0]['tools'] if t.get('name', '').startswith('mcp_')]
        restored = [t for t in seen[0]['tools'] if t.get('name', '').startswith('mcp_')]
        self.assertEqual(restored, original)
        self.assertEqual(sum(r[0]['method'] == 'tools/list' for r in self.server.requests), 1)

    def test_pagination_includes_every_tool(self):
        self.server.pages = True
        result = self.run_cli('tools', 'fixture')
        self.assertIn(b'"name": "first"', result.stdout)
        self.assertIn(b'"name": "send.message"', result.stdout)
        self.assertEqual(len(self.server.requests), 2)

    def test_pagination_cycle_is_rejected(self):
        self.server.pages = self.server.cursor_loop = True
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertLessEqual(len(self.server.requests), 2)

    def test_http_auth_failures_are_not_protocol_parse_errors(self):
        for status, diagnostic in ((401, b'login required'), (403, b'missing scope')):
            self.server.http_status = status
            result = self.run_cli('tools', 'fixture', ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(diagnostic, result.stderr)

    def test_wrong_response_id_is_refused(self):
        self.server.wrong_id = True
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'JSON-RPC', result.stderr)

    def test_first_operation_timeout_and_cancel(self):
        self.server.delay = 1
        self.config.write_text(self.config.read_text().replace('timeout_ms = 3000', 'timeout_ms = 150'))
        start = time.monotonic()
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertLess(time.monotonic() - start, .8)
        self.config.write_text(self.config.read_text().replace('timeout_ms = 150', 'timeout_ms = 3000'))
        self.server.started.clear()
        p = subprocess.Popen(self.args('mcp', 'tools', 'fixture'), cwd=self.root, env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.close_login, p)
        self.assertTrue(self.server.started.wait(3))
        p.send_signal(signal.SIGINT)
        p.communicate(timeout=1)
        self.assertNotEqual(p.returncode, 0)

    def test_local_deny_overrides_allow(self):
        self.policy = 'allow_tools = ["send.message"]\ndeny_tools = ["send.message"]\n'
        self.write_config()
        result = self.run_cli('tools', 'fixture')
        self.assertIn(b'disabled by local deny_tools', result.stdout)
        self.assertEqual(self.server.calls, [])

    def test_tool_errors_preserve_remote_details(self):
        self.server.tool_error = True
        result, _ = self.model_call()
        self.assertEqual(result['status'], 'failed')
        self.assertTrue(json.loads(result['model_text'])['result']['isError'])

    def test_catalog_is_frozen_across_model_cycles_and_persisted(self):
        self.server.changed = True
        result, requests = self.model_call(sse=True)
        self.assertEqual(result['status'], 'succeeded')
        self.assertEqual(sum(r[0]['method'] == 'tools/list' for r in self.server.requests), 1)
        self.assertEqual(requests[0]['tools'], requests[1]['tools'])
        snapshots = list(self.state.glob('sessions/*/mcp-turn.json'))
        self.assertEqual(len(snapshots), 1)
        self.assertEqual(snapshots[0].stat().st_mode & 0o777, 0o600)
        self.assertIn('send.message', snapshots[0].read_text())

    def test_oauth_real_loopback_callback(self):
        self.login(callback_mode='http')

    def test_oauth_exact_callback_path_issuer_and_duplicates(self):
        for mode in ('path', 'issuer', 'duplicate'):
            self.assertNotEqual(self.login(callback_mode=mode), 0)
        self.assertEqual(self.server.tokens, [])

    def test_oauth_resource_issuer_and_pkce_metadata_checked_before_consent(self):
        for field, bad in (('resource_override', 'https://wrong.test'),
                           ('issuer_override', 'https://wrong.test'), ('pkce', ['plain'])):
            setattr(self.server, field, bad)
            result = self.run_cli('login', 'fixture', ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b'/authorize?', result.stderr)
            setattr(self.server, field, ['S256'] if field == 'pkce' else None)
        self.assertEqual(self.server.tokens, [])

    def test_oauth_tokens_never_enter_result_or_journal(self):
        self.login()
        self.server.oauth = self.server.echo_secret = True
        result, requests = self.model_call()
        self.assertEqual(result['status'], 'succeeded')
        self.assertNotIn('fixture-access-secret', json.dumps(requests))
        for file in self.state.glob('sessions/**/*'):
            if file.is_file():
                self.assertNotIn(b'fixture-access-secret', file.read_bytes(), str(file))
                self.assertNotIn(b'fixture-refresh-secret', file.read_bytes(), str(file))

    def test_oauth_bad_state_never_exchanges_code(self):
        self.assertNotEqual(self.login(bad_state=True), 0)
        self.assertEqual(self.server.tokens, [])

    def test_parameter_headers_preserve_unicode_whitespace_sentinel_and_null(self):
        for value in (' padded ', 'Hello, 世界', 'line1\nline2', '=?base64?literal?=', '', None):
            with self.subTest(value=value):
                # Isolate each durable session so its result assertion stays exact.
                previous = self.state
                self.state = self.root / ('state-' + str(len(self.server.calls)))
                self.write_config()
                result, _ = self.model_call(arguments={'destination': value, 'message': 'body'})
                self.assertEqual(result['status'], 'succeeded')
                headers = [h for r, h in self.server.requests
                           if isinstance(r, dict) and r['method'] == 'tools/call'][-1]
                expected = (None if value is None else '' if value == '' else
                            '=?base64?' + base64.b64encode(value.encode()).decode() + '?=')
                self.assertEqual(headers.get('Mcp-Param-Destination'), expected)
                self.state = previous

    def test_large_result_retained_intact_with_receipt(self):
        self.server.large_result = True
        result, _ = self.model_call()
        self.assertEqual(result['status'], 'succeeded')
        receipt = json.loads(result['model_text'])
        retained = Path(receipt['result_file'])
        data = retained.read_bytes()
        self.assertEqual(len(data), receipt['result_bytes'])
        self.assertEqual(hashlib.sha256(data).hexdigest(), receipt['result_sha256'])
        self.assertEqual(retained.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(data)['result']['content'][0]['text'], 'a' * (3 * 1024 * 1024))

    def test_catalog_total_deadline_bounds_distinct_endless_pages(self):
        self.server.endless_pages = True
        self.server.delay = .06
        self.config.write_text(self.config.read_text().replace('timeout_ms = 3000', 'timeout_ms = 180'))
        started = time.monotonic()
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertLess(time.monotonic() - started, .8)
        self.assertLessEqual(len(self.server.requests), 4)

    def test_oauth_callback_host_and_origin_validation(self):
        for mode in ('host', 'origin'):
            self.assertNotEqual(self.login(callback_mode=mode), 0)
        self.assertEqual(self.server.tokens, [])

    def test_fresh_login_does_not_inherit_old_refresh_token(self):
        self.login()
        self.server.omit_refresh = True
        self.login()
        file = next(p for p in (self.state / 'mcp-auth').iterdir() if '.' not in p.name)
        self.assertNotIn('refresh_token', json.loads(file.read_text()))

    def test_rejected_refresh_and_unsafe_credential_are_distinct(self):
        self.server.expires = 1
        self.login()
        self.server.token_error = 'invalid_grant'
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertIn(b'invalid_grant', result.stderr)
        file = next(p for p in (self.state / 'mcp-auth').iterdir() if '.' not in p.name)
        file.chmod(0o644)
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertIn(b'unsafe or invalid', result.stderr)

    def test_catalog_credential_echo_is_withheld(self):
        self.login()
        self.server.catalog_secret = True
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertIn(b'catalog withheld', result.stderr)
        self.assertNotIn(b'fixture-access-secret', result.stdout + result.stderr)

    def test_invalid_content_type_refused(self):
        self.server.content_type = 'text/html'
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Content-Type', result.stderr)

    def notification_refresh(self, legacy):
        self.server.legacy = legacy
        self.server.watch_enabled = True
        seen = []

        def respond(handler, request, sequence):
            seen.append(request)
            self.provider.reply(handler, self.provider.response_body(
                sequence, 'catalog turn complete').encode(), close_header=True)

        self.provider.runtime_handler = respond
        term = self.interactive()
        term.write(b'first catalog\r')
        self.wait_result(term, 1, tools=False)
        self.assertTrue(self.server.subscribed.wait(3))
        self.server.tools = [dict(TOOL, name='replacement')]
        self.server.notify_change.set()
        deadline = time.monotonic() + 2
        while self.server.notify_change.is_set() and time.monotonic() < deadline:
            term.read(.02)
        term.read(.2)
        term.output.clear()
        term.write(b'next catalog\r')
        self.wait_result(term, 2, tools=False)
        self.assertEqual(len(seen), 2)
        first = [t for t in seen[0]['tools'] if t.get('name', '').startswith('mcp_')]
        second = [t for t in seen[1]['tools'] if t.get('name', '').startswith('mcp_')]
        self.assertIn('send.message', first[0]['description'])
        self.assertIn('replacement', second[0]['description'])
        self.assertNotEqual(first[0]['name'], second[0]['name'])
        term.write(b'/exit\r')
        term.wait_exit()
        self.assertTrue(self.server.watch_closed.wait(3))

    def test_modern_subscription_refreshes_between_turns_and_closes(self):
        self.notification_refresh(False)

    def test_legacy_notification_stream_refreshes_between_turns_and_closes(self):
        self.notification_refresh(True)

    def test_credentials_redacted_through_ordinary_file_tools(self):
        self.login()
        credential = next(p for p in (self.state / 'mcp-auth').iterdir() if '.' not in p.name)
        seen = []

        def respond(handler, request, sequence):
            seen.append(request)
            body = (self.provider.function_body(sequence, 'inspect-file', 'read_file',
                    {'path': str(credential)}) if len(seen) == 1 else
                    self.provider.response_body(sequence, 'file inspected'))
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        result = subprocess.run(self.args('-e', '--', 'inspect fixture credential file'),
            cwd=self.root, env=self.env, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(len(seen), 2)
        serialized = json.dumps(seen)
        self.assertIn('access_token', serialized)
        self.assertNotIn('fixture-access-secret', serialized)
        self.assertNotIn('fixture-refresh-secret', serialized)
        for file in self.state.glob('sessions/**/*'):
            if file.is_file():
                self.assertNotIn(b'fixture-access-secret', file.read_bytes(), str(file))

    def test_unavailable_optional_client_secret_does_not_block_provider(self):
        self.policy = 'client_secret = /missing/fixture/secret\n'
        self.write_config()
        seen = []

        def respond(handler, request, sequence):
            seen.append(request)
            self.provider.reply(handler, self.provider.response_body(
                sequence, 'provider remains available').encode(), close_header=True)

        self.provider.runtime_handler = respond
        result = subprocess.run(self.args('-e', '--', 'continue without optional server'),
            cwd=self.root, env=self.env, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(len(seen), 1)
        self.assertFalse(any(t.get('name', '').startswith('mcp_') for t in seen[0]['tools']))

    def test_fractional_schema_arguments_and_result_survive_native_history(self):
        schema = dict(SCHEMA, properties=dict(SCHEMA['properties'], amount={
            'type': 'number', 'minimum': .25, 'default': 1.5}))
        self.server.tools = [dict(TOOL, inputSchema=schema)]
        self.server.result_number = True
        result, requests = self.model_call(arguments={'destination': 'channel',
            'message': 'fraction', 'amount': 1.75}, expected_schema=schema)
        self.assertEqual(result['status'], 'succeeded')
        self.assertEqual(self.server.calls[0]['arguments']['amount'], 1.75)
        calls = [item for item in requests[-1]['input'] if item.get('type') == 'function_call']
        self.assertEqual(json.loads(calls[0]['arguments'])['amount'], 1.75)
        self.assertEqual(json.loads(result['model_text'])['result']['structuredContent']['measurement'], 1.25)
        session = next(self.state.glob('sessions/*/mcp-turn.json')).parent.name
        resumed = subprocess.run(self.args('-e', '--resume', session, '--', 'continue'),
            cwd=self.root, env=self.env, capture_output=True, timeout=12)
        self.assertEqual(resumed.returncode, 0, resumed.stderr.decode())
        calls = [item for item in requests[-1]['input'] if item.get('type') == 'function_call']
        self.assertEqual(json.loads(calls[0]['arguments'])['amount'], 1.75)
        self.assertEqual(len(self.server.calls), 1)

    def test_recognized_modern_errors_do_not_fall_back_to_initialize(self):
        for code in (-32020, -32021, -32022):
            self.server.requests.clear()
            self.server.rpc_error = {'code': code, 'message': 'fixture error',
                                     'data': {'supported': ['2099-01-01']}}
            result = self.run_cli('tools', 'fixture', ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(str(code).encode(), result.stderr)
            self.assertEqual([r['method'] for r, _ in self.server.requests], ['tools/list'])

    def test_unsupported_input_request_preserved_without_claiming_completion(self):
        self.server.input_required = True
        result, _ = self.model_call()
        self.assertEqual(result['status'], 'failed')
        receipt = json.loads(result['model_text'])
        self.assertEqual(receipt['status'], 'input_required')
        self.assertEqual(receipt['result']['inputRequests'][0]['method'], 'elicitation/create')
        self.assertEqual(len(self.server.calls), 1)

    def test_malformed_and_external_reference_schema_is_filtered(self):
        for invalid in ({'type': 'wrong'}, {'type': 'object', 'required': 'x'},
                        {'type': 'object', '$ref': 'https://elsewhere.test/schema'},
                        {'type': 'object', '$schema': 'https://unknown.test/dialect'}):
            self.server.tools = [dict(TOOL, name='invalid', inputSchema=invalid), TOOL]
            result = self.run_cli('tools', 'fixture')
            self.assertIn(b'invalid input schema', result.stdout)
            self.assertIn(b'send.message', result.stdout)

    def test_draft07_tuple_schema_is_preserved(self):
        schema = dict(SCHEMA, **{'$schema': 'http://json-schema.org/draft-07/schema#'})
        schema['properties'] = dict(SCHEMA['properties'], pair={
            'type': 'array', 'items': [{'type': 'number'}, {'type': 'string'}]})
        self.server.tools = [dict(TOOL, inputSchema=schema)]
        result, _ = self.model_call(arguments={'destination': 'channel', 'message': 'tuple',
            'pair': [1.5, 'two']}, expected_schema=schema)
        self.assertEqual(result['status'], 'succeeded')

    def test_client_authentication_validated_before_consent_and_revocation_uses_own_method(self):
        self.server.auth_methods = ['client_secret_post']
        failed = self.run_cli('login', 'fixture', ok=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn(b'client_secret', failed.stderr)
        self.assertNotIn(b'Waiting for the loopback', failed.stderr)
        self.policy = 'client_secret = "fixture-client-secret"\n'
        self.write_config()
        self.login()
        self.assertEqual(self.server.tokens[-1][1]['client_secret'], ['fixture-client-secret'])
        self.assertNotIn('Authorization', self.server.token_headers[-1])
        self.run_cli('logout', 'fixture')
        expected = base64.b64encode(b'fixture-client:fixture-client-secret').decode()
        self.assertEqual(self.server.token_headers[-1]['Authorization'], 'Basic ' + expected)
        self.assertNotIn('client_secret', self.server.tokens[-1][1])

    def test_uncertain_allowed_mutation_requires_new_approval_on_repeat(self):
        self.policy = 'allow_tools = ["send.message"]\n'
        self.write_config()
        self.server.disconnect = True
        seen = []

        def respond(handler, request, sequence):
            seen.append(request)
            tool = next(t for t in request['tools'] if t.get('name', '').startswith('mcp_'))
            body = (self.provider.function_body(sequence, f'repeat-{len(seen)}', tool['name'],
                {'destination': 'channel-42', 'message': 'exact'}) if len(seen) < 3 else
                self.provider.response_body(sequence, 'repeat refused'))
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        result = subprocess.run(self.args('-e', '--', 'repeat uncertain call'), cwd=self.root,
            env=self.env, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(len(self.server.calls), 1)
        _, events = read_events(self.state)
        results = [e['data']['result'] for e in events if e['type'] == 'tool_finished']
        self.assertEqual([r['status'] for r in results], ['outcome_unknown', 'not_run'])
        self.assertEqual(results[-1]['reason'], 'mcp_approval_required')

    def test_read_only_turn_does_not_admit_forged_writer(self):
        self.policy = 'read_only_tools = ["send.message"]\n'
        self.write_config()
        seen = []

        def respond(handler, request, sequence):
            seen.append(request)
            tool = next(t for t in request['tools'] if t.get('name', '').startswith('mcp_'))
            self.assertTrue(tool['name'].startswith('mcp_r_'))
            body = (self.provider.function_body(sequence, 'forged-writer',
                tool['name'].replace('mcp_r_', 'mcp_w_'),
                {'destination': 'channel-42', 'message': 'forged'}) if len(seen) == 1 else
                self.provider.response_body(sequence, 'writer refused'))
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        result = subprocess.run(self.args('-e', '--', '/ro inspect tools'), cwd=self.root,
            env=self.env, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIsNone(self.provider.failure)
        self.assertEqual(self.server.calls, [])
        _, events = read_events(self.state)
        results = [e['data']['result'] for e in events if e['type'] == 'tool_finished']
        self.assertEqual(results[0]['reason'], 'read_only')

    def test_cancel_during_dispatched_mutation_retains_unknown_outcome(self):
        self.policy = 'allow_tools = ["send.message"]\n'
        self.write_config()
        self.server.hold_call = True

        def respond(handler, request, sequence):
            tool = next(t for t in request['tools'] if t.get('name', '').startswith('mcp_'))
            body = self.provider.function_body(sequence, 'cancel-mcp', tool['name'],
                {'destination': 'channel-42', 'message': 'cancelled wait'})
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        process = subprocess.Popen(self.args('-e', '--', 'cancel while waiting'), cwd=self.root,
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.close_login, process)
        self.assertTrue(self.server.call_received.wait(5))
        process.send_signal(signal.SIGINT)
        output, errors = process.communicate(timeout=5)
        self.server.release_call.set()
        self.assertNotEqual(process.returncode, 0, output + errors)
        _, events = read_events(self.state)
        results = [e['data']['result'] for e in events if e['type'] == 'tool_finished']
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]['status'], 'outcome_unknown')
        self.assertEqual(len(self.server.calls), 1)

    def test_ipv6_catalog_and_callback(self):
        try:
            server = MCPServer(ipv6=True)
        except OSError as error:
            self.skipTest(str(error))
        self.addCleanup(server.close)
        self.server = server
        self.policy = 'redirect_uri = http://[::1]:0/callback\n'
        self.write_config()
        self.assertIn(b'send.message', self.run_cli('tools', 'fixture').stdout)
        self.login(callback_mode='http')

    def test_redirect_does_not_forward_request(self):
        destination = MCPServer()
        self.addCleanup(destination.close)
        self.server.redirect = destination.url + '/mcp'
        result = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'redirect refused', result.stderr)
        self.assertEqual(destination.requests, [])

    @unittest.skipUnless(shutil.which('openssl'), 'openssl required for isolated TLS fixture')
    def test_https_trust_and_proxy_failure(self):
        cert, key = self.root / 'cert.pem', self.root / 'key.pem'
        conf = self.root / 'openssl.cnf'
        conf.write_text('[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n'
            '[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n'
            'basicConstraints=critical,CA:TRUE\n'
            'keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n'
            'extendedKeyUsage=serverAuth\n')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-days', '1', '-keyout', str(key), '-out', str(cert), '-config', str(conf)],
            check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server = MCPServer(tls=context)
        self.addCleanup(server.close)
        self.server = server
        self.write_config()
        self.env.pop('SSL_CERT_FILE', None)
        failed = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(server.requests, [])
        self.env['SSL_CERT_FILE'] = str(cert)
        self.assertIn(b'send.message', self.run_cli('tools', 'fixture').stdout)
        server.requests.clear()
        proxy = MCPServer()
        self.addCleanup(proxy.close)
        self.env['HTTPS_PROXY'] = proxy.url
        self.env['NO_PROXY'] = self.env['no_proxy'] = ''
        failed = self.run_cli('tools', 'fixture', ok=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(server.requests, [])


if __name__ == '__main__':
    unittest.main()
