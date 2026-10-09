#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Large TLS 1.3 uploads preserve the body while ticket delivery overlaps backpressure."""
import http.server
import json
import os
import select
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from test_provider_https import response


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.connection.settimeout(8)
        body = self.rfile.read(int(self.headers['Content-Length']))
        try:
            request = json.loads(body)
            inputs = [item.get('content') for item in request['input']]
            self.server.results.append(any(isinstance(text, str) and
                                           self.server.prompt.rstrip('\n') in text
                                           for text in inputs))
        except (ValueError, KeyError):
            self.server.results.append(False)
        data = response()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True


class SlowTunnel:
    def __init__(self, target):
        self.target = target
        self.socket = socket.socket()
        # A small advertised TCP receive window makes the actual client's
        # writes block while the server's TLS 1.3 tickets arrive independently.
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        self.socket.bind(('127.0.0.1', 0))
        self.socket.listen()
        self.socket.settimeout(.1)
        self.port = self.socket.getsockname()[1]
        self.stop = threading.Event()
        self.clients = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                client, _ = self.socket.accept()
            except socket.timeout:
                continue
            upstream = socket.create_connection(self.target, timeout=5)
            thread = threading.Thread(target=self.relay, args=(client, upstream), daemon=True)
            self.clients.append(thread)
            thread.start()

    def relay(self, client, upstream):
        try:
            while not self.stop.is_set():
                ready, _, _ = select.select([client, upstream], [], [], .1)
                for source in ready:
                    data = source.recv(2048 if source is client else 65536)
                    if not data:
                        return
                    destination = upstream if source is client else client
                    destination.sendall(data)
                    if source is client:
                        time.sleep(.002)
        except (OSError, TimeoutError):
            pass
        finally:
            client.close()
            upstream.close()

    def close(self):
        self.stop.set()
        self.thread.join(2)
        for thread in self.clients:
            thread.join(2)
        self.socket.close()
        assert not self.thread.is_alive() and not any(t.is_alive() for t in self.clients)


def check(binary):
    if not ssl.HAS_TLSv1_3 or not shutil.which('openssl'):
        print('provider upload: skipped (TLS 1.3 or openssl unavailable)')
        return
    with tempfile.TemporaryDirectory(prefix='snag-upload-') as directory:
        root = Path(directory).resolve()
        cert, key = root / 'cert.pem', root / 'key.pem'
        conf = root / 'openssl.cnf'
        conf.write_text('[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n'
                        '[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost\n')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(cert), '-days', '1',
                        '-config', str(conf)], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(cert, key)
        context.num_tickets = 2
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.results = []
        server.prompt = ''.join(f'block-{i:06d} ' + 'abcdefghij' * 5 + '\n'
                                for i in range(8000))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        tunnel = SlowTunnel(('127.0.0.1', server.server_port))
        config = root / 'config.ini'
        config.write_text('[agent]\nmodel=fixture\nread_agents_md=false\nmax_turn_retries=0\n'
                          f'[provider fixture]\nbase_url=https://localhost:{tunnel.port}/v1\n'
                          'api_key="fixture-secret"\nexact_token_count=false\n'
                          'native_compaction=false\nrequest_timeout_ms=10000\n[ui]\ncolor=never\n')
        config.chmod(0o600)
        env = dict(os.environ, SSL_CERT_FILE=str(cert), NO_PROXY='localhost,127.0.0.1',
                   no_proxy='localhost,127.0.0.1')
        try:
            for attempt in range(3):
                result = subprocess.run([str(binary), '--config', str(config), '--dotdir',
                                         str(root / ('state-' + str(attempt))), '-e'],
                                        input=server.prompt, text=True, capture_output=True,
                                        cwd=root, env=env, timeout=15)
                assert result.returncode == 0, result.stderr
                assert server.results == [True] * (attempt + 1), server.results
                assert result.stdout.strip() == 'HTTPS answer once', result.stdout
        finally:
            tunnel.close()
            server.shutdown()
            server.server_close()
            thread.join(2)
        print('provider upload: 3 backpressured TLS 1.3 bodies intact')


if __name__ == '__main__':
    check(Path(sys.argv[1]).resolve())
