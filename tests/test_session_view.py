#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Real owners: semantic framing, controller arbitration and durable admission."""

import hashlib
import json
import os
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from pathlib import Path

from store_history import create_legacy, journal_paths, read_boundary, read_events

import tmux_terminal as harness
from test_remote_terminal import RemoteProcess


BINARY = (Path(sys.argv.pop(1)).resolve()
          if len(sys.argv) > 1 and not sys.argv[1].startswith('-')
          else Path(__file__).resolve().parents[1] / 'snajpagent')
OMITTED = '--without-vm' in sys.argv
if OMITTED:
    sys.argv.remove('--without-vm')


def connect(directory, name):
    peer = socket.socket(socket.AF_UNIX)
    peer.settimeout(5)
    previous = Path.cwd()
    try:
        os.chdir(directory)
        peer.connect(name)
    except OSError:
        peer.close()
        raise
    finally:
        os.chdir(previous)
    return peer


def exact(peer, count):
    data = bytearray()
    while len(data) < count:
        part = peer.recv(count - len(data))
        if not part:
            raise EOFError('owner closed the connection')
        data.extend(part)
    return bytes(data)


class View:
    def __init__(self, directory):
        self.peer = connect(directory, 'view.sock')
        self.generation = 0
        self.states = []
        self.send(type='hello', version=1)
        self.capabilities = self.until('capabilities')

    def send(self, fragment=0, **message):
        data = json.dumps(message, ensure_ascii=True, separators=(',', ':')).encode()
        for offset in range(0, len(data), 16368):
            part = struct.pack('<QQ', offset, len(data)) + data[offset:offset + 16368]
            frame = b'SV\x01\x01' + struct.pack('<I', len(part)) + part
            if fragment:
                for begin in range(0, len(frame), fragment):
                    self.peer.sendall(frame[begin:begin + fragment])
            else:
                self.peer.sendall(frame)

    def receive(self):
        data = bytearray()
        while True:
            header = exact(self.peer, 8)
            assert header[:4] == b'SV\x01\x01', header
            size, = struct.unpack('<I', header[4:])
            assert 16 < size <= 16384, size
            frame = exact(self.peer, size)
            offset, total = struct.unpack('<QQ', frame[:16])
            assert offset == len(data) and 0 < total <= 6 * 1024 * 1024 + 16384
            data.extend(frame[16:])
            assert len(data) <= total
            if len(data) == total:
                message = json.loads(data)
                if message['type'] == 'state':
                    self.states.append(message['state'])
                return message

    def until(self, kind, predicate=lambda message: True):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            message = self.receive()
            if message['type'] == kind and predicate(message):
                return message
        raise AssertionError(f'missing {kind}')

    def reserve(self):
        self.send(type='reserve')
        self.generation = self.until('reserved')['generation']

    def bind(self):
        self.reserve()
        self.send(type='commit', generation=self.generation)
        return self.until('bound')

    def submit(self, text, request=None, **options):
        request = request or uuid.uuid4().hex
        self.send(type='submit', generation=self.generation, id=request, text=text, **options)
        return request

    def command(self, text, request=None, **options):
        request = request or uuid.uuid4().hex
        self.send(type='command', generation=self.generation, id=request, text=text,
                  route='rollout', **options)
        return request

    def result(self, request):
        return self.until('result', lambda message: message['id'] == request and
                          message['status'] != 'pending')

    def draft(self):
        self.send(type='draft_get', generation=self.generation, route='rollout')
        return self.until('draft', lambda message: message['status'] == 'snapshot')['draft']

    def replace_draft(self, revision, text, cursor=None, edit=1):
        self.send(type='draft', generation=self.generation, route='rollout', revision=revision,
                  edit=edit, text=text, cursor=len(text.encode()) if cursor is None else cursor)
        return self.until('draft', lambda message: message['edit'] == edit)

    def close(self):
        self.peer.close()


class SessionViewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='snag-view-')
        self.root = Path(self.tmp.name).resolve()
        self.provider = harness.FakeResponses()
        self.provider.runtime_handler = lambda handler, request, sequence: self.provider.reply(
            handler, self.provider.response_body(sequence, 'semantic-answer').encode(),
            close_header=True)
        self.config = self.root / 'config.ini'
        harness.write_irc_config(self.config, self.provider.port, 'host-model')
        self.prefix = [str(BINARY), '--config', str(self.config), '--dotdir',
                       str(self.root / 'state')]
        if self._testMethodName == 'test_retention_failure_preserves_bound_and_publishes_error':
            launcher = self.root / 'limited-owner.py'
            launcher.write_text(
                'import os, resource, signal, sys\n'
                'signal.signal(signal.SIGXFSZ, signal.SIG_IGN)\n'
                'resource.setrlimit(resource.RLIMIT_FSIZE, (16384, 16384))\n'
                'os.execv(sys.argv[1], sys.argv[1:])\n')
            self.prefix = [sys.executable, str(launcher)] + self.prefix
        self.env = {'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'}
        if self._testMethodName.startswith('test_sleep_'):
            library = self.root / 'sleep-clock.dylib'
            subprocess.run(['cc', '-dynamiclib', '-Wall', '-Wextra', '-Werror',
                            str(Path(__file__).with_name('fixture_sleep_clock.c')),
                            '-o', str(library)], check=True, capture_output=True)
            self.sleep_clock = self.root / 'sleep-clock.bin'
            self.sleep_clock.write_bytes(struct.pack('=q', 0))
            self.env.update(DYLD_INSERT_LIBRARIES=str(library),
                            SNAJPAGENT_TEST_SLEEP_CLOCK=str(self.sleep_clock))
        self.children = []
        self.views = []
        self.owner = None
        self.release = threading.Event()
        resume = []
        if self._testMethodName == 'test_legacy_observer_is_a_committed_prefix':
            journal = create_legacy(self.root / 'state', self.root, 'fake', 'host-model')
            resume = ['--resume', journal.parent.name]
        child = self.start(resume)
        child.until('›'.encode(), 15)
        self.journal, = journal_paths(self.root / 'state')
        self.directory = self.journal.parent
        self.sid = self.directory.name
        for row in subprocess.check_output(['ps', '-axww', '-o', 'pid=', '-o', 'ppid=', '-o', 'command='],
                                           text=True).splitlines():
            fields = row.split(None, 2)
            if len(fields) == 3 and fields[1] == str(child.process.pid) and str(self.config) in row:
                self.owner = int(fields[0])
        self.assertIsNotNone(self.owner)
        self.owner_identity = self.identity()
        self.initial = child

    def identity(self):
        return subprocess.run(['ps', '-ww', '-p', str(self.owner), '-o', 'lstart=', '-o', 'command='],
                              text=True, capture_output=True).stdout.strip()

    def test_piped_tool_cannot_claim_the_owner_terminal(self):
        probe = self.root / 'terminal-probe.py'
        result = self.root / 'terminal-probe.json'
        probe.write_text(
            'import errno, json, os, signal, sys\n'
            'state = {"controlling_terminal": False}\n'
            'try:\n'
            '    fd = os.open("/dev/tty", os.O_RDWR | os.O_NOCTTY)\n'
            'except OSError as error:\n'
            '    assert error.errno == errno.ENXIO, error\n'
            'else:\n'
            '    signal.signal(signal.SIGTTOU, signal.SIG_IGN)\n'
            '    os.tcsetpgrp(fd, os.getpgrp())\n'
            '    state["controlling_terminal"] = True\n'
            '    os.close(fd)\n'
            'with open(sys.argv[1] + ".tmp", "w") as output:\n'
            '    json.dump(state, output)\n'
            'os.replace(sys.argv[1] + ".tmp", sys.argv[1])\n'
            'print("terminal probe finished")\n')

        def respond(handler, request, sequence):
            completed = any(item.get('type') == 'function_call_output'
                            for item in request.get('input', []))
            if completed:
                body = self.provider.response_body(sequence, 'tool isolation complete')
            else:
                body = self.provider.function_body(sequence, 'call_terminal_probe',
                    'exec_command', {
                        'command': shlex.join([sys.executable, str(probe), str(result)]),
                        'workdir': str(self.root), 'pty': False, 'yield_ms': 1000,
                    })
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.initial.master, b'check tool terminal isolation\r')
        deadline = time.monotonic() + 10
        while not result.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(result.exists(), 'the real command tool did not run')
        self.assertFalse(json.loads(result.read_text())['controlling_terminal'],
                         'a piped tool can take the owner terminal foreground group')
        self.wait_event('turn_completed')
        os.write(self.initial.master, b'/fast off\r')
        self.initial.until(b'Fast mode: OFF', 5)
        self.detach()
        resumed = self.start(['--resume', self.sid])
        resumed.until(b'Attached session', 10)
        os.write(resumed.master, b'/fast on\r')
        resumed.until(b'Fast mode: ON', 5)
        self.assertEqual(self.identity(), self.owner_identity)
        self.assertFalse(any(event['type'] in ('response_failed', 'turn_recovery')
                             for event in self.events()))
        self.finish(resumed, b'/s d')

    def test_resume_moves_classic_terminal_only_after_confirmation(self):
        os.write(self.initial.master, b'retained resume draft')
        self.initial.until(b'retained resume draft')
        refused = RemoteProcess(self.root, [str(BINARY), '--dotdir', str(self.root / 'state'),
                                            '--attach', self.sid], wrapped=None)
        self.children.append(refused)
        refused.until(b'session already has a terminal')
        refused.wait(3)
        for answer in (b'n\r', b'\r', b'\x1b', b'\x03'):
            declined = self.start(['--resume', self.sid])
            declined.until(b'Move it to this terminal? [y/N] ')
            os.write(declined.master, answer)
            declined.wait(0)
            self.assertEqual(self.identity(), self.owner_identity)
            self.assertIsNone(self.initial.process.poll())
        self.assertFalse(any(event['type'] == 'turn_started' for event in self.events()))
        resumed = self.start(['--resume', self.sid])
        resumed.until(b'Move it to this terminal? [y/N] ')
        os.write(resumed.master, b'y\r')
        resumed.until(b'retained resume draft')
        self.initial.wait(3)
        self.assertEqual(self.identity(), self.owner_identity)
        os.write(resumed.master, b'\x15resume confirmation submitted once\r')
        resumed.until(b'semantic-answer', 10)
        self.wait_event('turn_completed')
        inputs = [event for event in self.events() if event['type'] == 'input_received']
        self.assertEqual([row['data']['text'] for row in inputs],
                         ['resume confirmation submitted once'])
        self.finish(resumed, b'/s d')
        self.status('detached')

    def test_retention_failure_preserves_bound_and_publishes_error(self):
        self.detach()
        peer = self.view()
        peer.bind()
        for i in range(40):
            result = peer.result(peer.command('/fast on' if i % 2 else '/fast off'))
            self.assertEqual(result['status'], 'completed')
            peer.draft()
            state = peer.states[-1]
            if state.get('presentation_error'):
                break
        self.assertIn('retention stopped', state.get('presentation_error', '').lower())
        bound = state['presentation']
        self.assertIsInstance(bound, dict)
        self.assertGreater(bound['tail'][1], 2)
        self.assertLessEqual(bound['tail'][0],
                            (self.directory / '.view-presentation.snb').stat().st_size)
        result = peer.result(peer.command('/fast off'))
        self.assertEqual(result['status'], 'completed')
        peer.draft()
        self.assertEqual(peer.states[-1]['presentation'], bound)
        self.assertEqual(peer.states[-1]['presentation_error'], state['presentation_error'])
        self.assertEqual(self.identity(), self.owner_identity)
        self.assertFalse(any(event['type'] in ('input_received', 'response_failed')
                             for event in self.events()))

    def start(self, args):
        child = RemoteProcess(self.root, self.prefix + args, wrapped=None, extra_env=self.env)
        self.children.append(child)
        return child

    def finish(self, child, command):
        os.write(child.master, command + b'\r')
        deadline = time.monotonic() + 10
        while child.process.poll() is None and time.monotonic() < deadline:
            if select.select([child.master], [], [], .05)[0]:
                try:
                    child.output.extend(os.read(child.master, 65536))
                except OSError:
                    pass
        self.assertEqual(child.process.poll(), 0, bytes(child.output))

    def detach(self):
        self.finish(self.initial, b'/s d')
        self.status('detached')

    def resume_hint(self, child, sid):
        output = child.output.decode(errors='replace')
        marker = 'You can resume this session with the following command'
        self.assertEqual(output.count(marker), 1, output)
        command = output.split(marker)[1].splitlines()[1]
        self.assertEqual(shlex.split(command), [str(BINARY), '--dotdir',
                        str(self.root / 'state'), '--resume', sid])
        return command

    def test_detach_prints_command_that_reattaches_same_owner(self):
        os.write(self.initial.master, b'/session name renamed session\r')
        self.initial.until(b'session name: renamed session')
        self.initial.output.clear()
        self.detach()
        command = self.resume_hint(self.initial, self.sid)
        resumed = RemoteProcess(self.root, ['/bin/sh', '-c', 'exec ' + command],
                                wrapped=None, extra_env=self.env)
        self.children.append(resumed)
        resumed.until(b'Attached session', 10)
        self.status('attached')
        self.assertEqual(self.identity(), self.owner_identity)
        resumed.output.clear()
        self.finish(resumed, b'/session detach')
        self.resume_hint(resumed, self.sid)
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)

    def test_detach_after_switch_prints_destination_id(self):
        target = self.start(['-N', 'destination'])
        target.until('›'.encode(), 15)
        target_sid, = (path.name for path in (self.root / 'state' / 'sessions').iterdir()
                       if path.is_dir() and path.name != self.sid)
        self.finish(target, b'/s d')
        os.write(self.initial.master, ('/session attach ' + target_sid + '\r').encode())
        self.initial.until(b'Attached session', 10)
        self.initial.output.clear()
        self.finish(self.initial, b'/s d')
        self.resume_hint(self.initial, target_sid)
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)

    def view(self, bind=False):
        peer = View(self.directory)
        self.views.append(peer)
        self.assertEqual(peer.capabilities['session'], self.sid)
        if bind:
            peer.bind()
        return peer

    def status(self, wanted):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = subprocess.run(self.prefix + ['-l'], cwd=self.root,
                                    env={**os.environ, **self.env}, text=True,
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = [line.split('\t') for line in result.stdout.splitlines()[1:]]
            actual = next((row[4] for row in rows if self.sid.startswith(row[0])), None)
            if actual == wanted:
                return
            time.sleep(.02)
        self.fail((wanted, actual, result.stdout, result.stderr))

    def events(self):
        return read_events(self.journal)

    def wait_event(self, kind, after_seq=0):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            events = [event for event in self.events()
                      if event['type'] == kind and event['seq'] > after_seq]
            if events:
                return events[-1]
            time.sleep(.02)
        self.fail(f'missing {kind}')

    def tearDown(self):
        self.release.set()
        for peer in self.views:
            peer.close()
        if self.owner and self.identity() == self.owner_identity:
            os.kill(self.owner, signal.SIGTERM)
        for child in self.children:
            child.close()
        self.provider.close()
        self.tmp.cleanup()

    def test_binary_feature_boundary(self):
        result = subprocess.run([str(BINARY), 'vm', '--help'], capture_output=True, text=True)
        if OMITTED:
            self.assertEqual(result.returncode, 2)
            self.assertIn('WITH_VM=0', result.stderr)
            self.assertFalse((self.directory / 'view.sock').exists())
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((self.directory / 'view.sock').exists())
        self.finish(self.initial, b'/exit')
        self.status('stored')

    def test_terminal_close_releases_attachment_and_preserves_owner(self):
        child = self.initial
        child.remember_children()
        os.close(child.slave)
        child.slave = None
        os.close(child.master)
        child.master = None
        child.process.wait(timeout=8)
        self.status('detached')

        resumed = self.start(['--resume', self.sid])
        resumed.until(b'Attached session', 10)
        self.status('attached')
        self.assertEqual(self.identity(), self.owner_identity)

    @unittest.skipUnless(sys.platform == 'darwin', 'Darwin sleep clock semantics')
    def test_sleep_preserves_pending_output_and_owner(self):
        requested = threading.Event()

        def respond(handler, request, sequence):
            requested.set()
            self.release.wait(10)
            body = self.provider.response_body(sequence, 'awake-ok')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.initial.master, b'keep running across sleep\r')
        self.assertTrue(requested.wait(10), 'owner did not start the request')
        os.kill(self.initial.process.pid, signal.SIGSTOP)
        try:
            self.release.set()
            self.wait_event('turn_completed')
            # Leave an output acknowledgement pending, then advance only the
            # Darwin clock that includes sleep. No host or provider is suspended.
            time.sleep(.1)
            temporary = self.sleep_clock.with_suffix('.new')
            temporary.write_bytes(struct.pack('=q', 60))
            temporary.replace(self.sleep_clock)
            time.sleep(.1)
        finally:
            os.kill(self.initial.process.pid, signal.SIGCONT)
        self.initial.until(b'awake-ok', 10)
        self.status('attached')
        self.assertIsNone(self.initial.process.poll(), bytes(self.initial.output))
        self.assertEqual(self.identity(), self.owner_identity)
        os.write(self.initial.master, b'/fast off\r')
        self.initial.until(b'Fast mode: OFF', 5)
        self.assertEqual(len(self.provider.requests), 1)

    def test_observer_is_read_only_and_snapshot_is_a_committed_prefix(self):
        peer = self.view()
        snapshot = peer.until('state')['state']
        data = self.journal.read_bytes()
        prefix = data[:snapshot['end']]
        if self.journal.name == 'events.jsonl':
            self.assertTrue(prefix.endswith(b'\n'))
            last = json.loads(prefix.splitlines()[-1])
            self.assertEqual(last['seq'], snapshot['seq'])
            self.assertEqual(last['event_sha256'], snapshot['sha256'])
        else:
            # Verify the captured physical prefix with the production decoder,
            # independently of the live owner's advertised coordinates.
            root = self.root / 'snapshot-state'
            root.mkdir(mode=0o700)
            sessions = root / 'sessions'
            sessions.mkdir(mode=0o700)
            directory = sessions / self.sid
            directory.mkdir(mode=0o700)
            copied = directory / 'journal.bin'
            copied.write_bytes(prefix)
            copied.chmod(0o600)
            self.assertEqual(read_boundary(copied), {key: snapshot[key]
                             for key in ('seq', 'end', 'sha256')})
            self.assertEqual(copied.read_bytes(), prefix)
        self.status('attached')
        peer.send(type='reserve')
        self.assertIn('controller', peer.until('error')['message'])
        self.detach()
        saved = self.journal.read_bytes()
        other = self.view()
        other.until('state')
        self.status('detached')
        self.assertEqual(self.journal.read_bytes(), saved)
        self.assertEqual(self.identity(), self.owner_identity)
        self.assertEqual((self.directory / 'view.sock').stat().st_mode & 0o777, 0o600)

    def test_legacy_observer_is_a_committed_prefix(self):
        self.test_observer_is_read_only_and_snapshot_is_a_committed_prefix()

    def test_controller_exclusion_generation_disconnect_and_classic_return(self):
        self.detach()
        peer = self.view()
        peer.reserve()
        self.status('detached')
        other = self.view()
        other.send(type='reserve')
        other.until('error')
        with connect(self.directory, 'terminal.sock') as classic:
            classic.sendall(b'SA\x05\x01' + b'\0' * 4)
            header = exact(classic, 8)
            self.assertEqual(header[:4], b'SA\x05\x0a')
            self.assertIn(b'reservation', exact(classic, struct.unpack('<I', header[4:])[0]))
        peer.send(type='commit', generation=peer.generation + 1)
        peer.until('error')
        peer.send(type='commit', generation=peer.generation)
        peer.until('bound')
        self.status('attached')
        peer.close()
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)
        child = self.start(['--resume', self.sid])
        child.until(b'Attached session', 10)
        self.finish(child, b'/exit')
        self.status('stored')

    def test_durable_submission_duplicate_and_reconnect_receipt(self):
        self.detach()
        peer = self.view(bind=True)
        request = peer.submit('semantic prompt', fragment=1)
        result = peer.result(request)
        self.assertEqual(result['status'], 'committed', result)
        event = next(event for event in self.events() if event['seq'] == result['seq'])
        self.assertEqual(event['type'], 'input_received')
        self.assertEqual(result['id'], request)
        self.assertEqual(event['data']['text'], 'semantic prompt')
        self.wait_event('turn_completed')
        peer.submit('semantic prompt', request)
        self.assertEqual(peer.result(request), result)
        peer.submit('different prompt', request)
        peer.until('error')
        peer.close()
        self.status('detached')
        other = self.view()
        self.assertEqual(other.capabilities['instance'], peer.capabilities['instance'])
        other.send(type='receipt', id=request)
        self.assertEqual(other.result(request), result)
        inputs = [event for event in self.events() if event['type'] == 'input_received']
        self.assertEqual(len(inputs), 1)
        self.assertEqual(self.identity(), self.owner_identity)

    def semantic_stream_handoff(self, disconnect, classic_early=False):
        prefix, tail = 'semantic-prefix ', 'classic-tail\n'

        def respond(handler, request, sequence):
            rid, item_id = 'resp_handoff', 'handoff_item'
            item = {'type': 'message', 'id': item_id, 'role': 'assistant',
                    'phase': 'final_answer', 'status': 'in_progress', 'content': []}
            position = {'item_id': item_id, 'output_index': 0, 'content_index': 0}
            handler.send_response(200)
            handler.send_header('Content-Type', 'text/event-stream')
            handler.send_header('Connection', 'close')
            handler.end_headers()

            def send(kind, data):
                handler.wfile.write(self.provider.event(kind, data).encode())
                handler.wfile.flush()

            send('response.created', {'response': {'id': rid, 'status': 'in_progress',
                                                   'output': []}})
            send('response.output_item.added', {'output_index': 0, 'item': item})
            send('response.content_part.added', {**position,
                 'part': {'type': 'output_text', 'text': '', 'annotations': []}})
            send('response.output_text.delta', {**position, 'delta': prefix})
            self.release.wait(10)
            send('response.output_text.delta', {**position, 'delta': tail})
            send('response.output_text.done', {**position, 'text': prefix + tail})
            item.update(status='completed', content=[{'type': 'output_text',
                        'text': prefix + tail, 'annotations': []}])
            send('response.output_item.done', {'output_index': 0, 'item': item})
            send('response.completed', {'response': {'id': rid, 'status': 'completed',
                 'output': [], 'usage': {'input_tokens': 10, 'output_tokens': 10,
                                        'total_tokens': 20}}})

        self.provider.runtime_handler = respond
        self.detach()
        peer = self.view(bind=True)
        request = peer.submit('continue output after semantic controller leaves')
        self.assertEqual(peer.result(request)['status'], 'committed')
        self.assertEqual(self.wait_event('response_output')['data']['item']['text'], prefix)
        if not disconnect:
            peer.send(type='detach', generation=peer.generation)
            peer.until('detached')
        peer.close()
        self.status('detached')
        child = None
        if classic_early:
            child = self.start(['--resume', self.sid])
            self.status('attached')
        self.release.set()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            events = self.events()
            if any(event['type'] in ('turn_completed', 'response_failed') for event in events):
                break
            time.sleep(.02)
        self.assertEqual([event['data'] for event in events
                          if event['type'] == 'response_failed'], [])
        self.assertTrue(any(event['type'] == 'turn_completed' for event in events))
        self.assertEqual(''.join(event['data']['item']['text'] for event in events
                                if event['type'] == 'response_output'), prefix + tail)
        self.assertEqual(self.identity(), self.owner_identity)
        if child is None:
            child = self.start(['--resume', self.sid])
        child.until(b'Attached session', 10)
        if classic_early:
            # Owner completion alone misses a blocked physical PTY writer.
            # Require both retained prefix and live tail at the real frontend.
            child.until(prefix.strip().encode())
            child.until(tail.strip().encode())
            self.assertEqual(child.output.count(prefix.strip().encode()), 1)
            self.assertEqual(child.output.count(tail.strip().encode()), 1)
            self.assertIsNone(child.process.poll(), 'classic frontend exited after replay')
            self.status('attached')
        else:
            child.until((prefix + tail).strip().encode())
        self.finish(child, b'/s d')
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)

    def test_semantic_detach_mid_output_preserves_owner_and_classic_return(self):
        self.semantic_stream_handoff(False)

    def test_semantic_disconnect_mid_output_preserves_owner_and_classic_return(self):
        self.semantic_stream_handoff(True)

    def test_classic_return_before_semantic_output_completes(self):
        self.semantic_stream_handoff(False, classic_early=True)

    def test_multiframe_unicode_prompt_and_stale_submission(self):
        self.detach()
        peer = self.view(bind=True)
        saved = hashlib.sha256(self.journal.read_bytes()).digest()
        peer.send(type='submit', generation=peer.generation, id=uuid.uuid4().hex,
                  text='must not route implicitly', route='other/endpoint/nick')
        self.assertIn('route', peer.until('error')['message'])
        peer.send(type='submit', generation=peer.generation + 1, id=uuid.uuid4().hex, text='stale')
        peer.until('error')
        self.assertEqual(hashlib.sha256(self.journal.read_bytes()).digest(), saved)
        text = 'žluťoučký 🐈\n' * 17000
        request = peer.submit(text)
        self.assertEqual(peer.result(request)['status'], 'committed')
        event = self.wait_event('input_received')
        self.assertEqual(event['data']['text'], text)
        self.wait_event('turn_completed')

    def test_malformed_frame_cannot_reserve_or_stop_owner(self):
        self.detach()
        for prefix, offset, total in ((b'SV\x02\x01', 0, 2),
                                      (b'SA\x05\x01', 0, 2),
                                      (b'SV\x01\x01', 1, 2),
                                      (b'SV\x01\x01', 0, 2**63)):
            with connect(self.directory, 'view.sock') as peer:
                payload = struct.pack('<QQ', offset, total) + b'{}'
                peer.sendall(prefix + struct.pack('<I', len(payload)) + payload)
                try:
                    self.assertEqual(peer.recv(1), b'')
                except ConnectionResetError:
                    pass
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)
        self.view(bind=True)
        self.status('attached')

    def test_lost_acknowledgement_is_queried_without_resubmission(self):
        self.detach()
        peer = self.view(bind=True)
        request = peer.submit('one uncertain submission')
        peer.close()
        self.status('detached')
        other = self.view()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            other.send(type='receipt', id=request)
            result = other.until('result', lambda message: message['id'] == request)
            if result['status'] != 'pending':
                break
            time.sleep(.02)
        self.assertIn(result['status'], ('committed', 'rejected', 'unknown'))
        if result['status'] == 'committed':
            self.wait_event('turn_completed')
        inputs = [event for event in self.events() if event['type'] == 'input_received']
        self.assertEqual(len(inputs), int(result['status'] == 'committed'))
        self.assertEqual(self.identity(), self.owner_identity)

    def test_cancel_during_provider_wait_and_active_steering(self):
        started = threading.Event()

        def held(handler, request, sequence):
            started.set()
            self.release.wait(10)
            try:
                self.provider.reply(handler, self.provider.response_body(
                    sequence, 'completed after hold').encode(), close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        self.provider.runtime_handler = held
        self.detach()
        peer = self.view(bind=True)
        request = peer.submit('start held turn')
        self.assertEqual(peer.result(request)['status'], 'committed')
        self.assertTrue(started.wait(5))
        steering = peer.submit('change the pending task')
        receipt = peer.result(steering)
        self.assertEqual(receipt['status'], 'committed', receipt)
        self.assertEqual(receipt['event'], 'steering_added')
        peer.send(type='cancel', generation=peer.generation)
        peer.until('control')
        self.release.set()
        self.wait_event('turn_cancel_requested')
        self.wait_event('turn_interrupted')
        self.status('attached')
        self.assertEqual(self.identity(), self.owner_identity)

    def test_rejected_input_preserves_owner_and_receipt(self):
        self.detach()
        peer = self.view(bind=True)
        refused = peer.submit('/status')
        error = peer.until('error')
        self.assertIn('capability', error['message'])
        self.assertEqual(error['id'], refused)
        request = peer.submit('   ')
        self.assertEqual(peer.result(request)['status'], 'rejected')
        peer.send(type='receipt', id=request)
        self.assertEqual(peer.result(request)['status'], 'rejected')
        self.assertFalse(any(event['type'] == 'input_received' for event in self.events()))
        self.assertEqual(self.identity(), self.owner_identity)

    def test_queue_receipt_keeps_intent_across_reconnect_and_rejects_query_routes(self):
        started = threading.Event()

        def held(handler, request, sequence):
            started.set()
            self.release.wait(15)
            self.provider.reply(handler, self.provider.response_body(
                sequence, 'held-answer').encode(), close_header=True)

        self.provider.runtime_handler = held
        self.detach()
        peer = self.view(bind=True)
        self.assertEqual(peer.result(peer.submit('hold active turn'))['status'], 'committed')
        self.assertTrue(started.wait(5))
        request = uuid.uuid4().hex
        message = dict(type='queue', generation=peer.generation, id=request,
                       text='queued once', route='rollout')
        peer.send(**message)
        receipt = peer.result(request)
        self.assertEqual(receipt['event'], 'future_turn_queued')
        peer.send(**message)
        self.assertEqual(peer.result(request), receipt)
        peer.send(**dict(message, text='changed queued text'))
        self.assertEqual(peer.until('error').get('id'), request)
        peer.send(**dict(message, type='submit'))
        self.assertEqual(peer.until('error').get('id'), request)
        route = dict(connection=uuid.uuid4().hex, conversation=uuid.uuid4().hex,
                     generation=1, identity='operator', peer='private-peer')
        refused = uuid.uuid4().hex
        peer.send(**dict(message, id=refused, route=route, text='private draft'))
        self.assertEqual(peer.until('error').get('id'), refused)
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        peer = self.view(bind=True)
        peer.send(type='receipt', id=request)
        self.assertEqual(peer.result(request), receipt)
        queued = [event['data']['text'] for event in self.events()
                  if event['type'] == 'future_turn_queued']
        self.assertEqual(queued, ['queued once'])
        peer.send(type='cancel', generation=peer.generation)
        peer.until('control')
        self.release.set()
        self.wait_event('turn_interrupted')

    def test_revisioned_draft_survives_disconnect_without_journal_mutation(self):
        self.detach()
        original = self.journal.read_bytes()
        peer = self.view(bind=True)
        self.assertIn('drafts', peer.capabilities['features'])
        first = peer.draft()
        self.assertEqual(first['text'], '')
        text = 'unsent 👩‍💻\n' + 'draft data ' * 2500
        result = peer.replace_draft(first['revision'], text, edit=9)
        self.assertEqual(result['status'], 'accepted')
        self.assertEqual(result['draft']['text'], text)
        self.assertEqual(result['draft']['cursor'], len(text.encode()))
        self.assertGreater(result['draft']['revision'], first['revision'])
        peer.close()
        self.views.remove(peer)
        self.status('detached')
        replacement = self.view(bind=True)
        self.assertEqual(replacement.draft(), result['draft'])
        self.assertEqual(self.journal.read_bytes(), original)
        self.assertEqual(self.identity(), self.owner_identity)

    def test_draft_cas_cursor_route_and_controller_validation(self):
        self.detach()
        observer = self.view()
        observer.send(type='draft_get', generation=0, route='rollout')
        self.assertIn('generation', observer.until('error')['message'])
        peer = self.view(bind=True)
        first = peer.draft()
        accepted = peer.replace_draft(first['revision'], '👩‍💻')
        stale = peer.replace_draft(first['revision'], 'stale edit', edit=2)
        self.assertEqual(stale['status'], 'conflict')
        self.assertEqual(stale['draft'], accepted['draft'])
        peer.send(type='draft', generation=peer.generation, route='rollout',
                  revision=accepted['draft']['revision'], edit=3, text='👩‍💻', cursor=4)
        invalid = peer.until('error')
        self.assertIn('invalid draft', invalid['message'])
        self.assertEqual(invalid['edit'], 3)
        peer.send(type='draft', generation=peer.generation, route='irc',
                  revision=accepted['draft']['revision'], edit=4, text='wrong route', cursor=0)
        invalid = peer.until('error')
        self.assertIn('unsupported route', invalid['message'])
        self.assertEqual(invalid['edit'], 4)
        self.assertEqual(peer.draft(), accepted['draft'])

    def test_revisioned_submission_clears_only_its_admitted_draft(self):
        self.detach()
        peer = self.view(bind=True)
        first = peer.draft()
        changed = peer.replace_draft(first['revision'], 'draft-bound prompt')['draft']
        refused = peer.submit('draft-bound prompt', route='rollout',
                              draft_revision=first['revision'])
        error = peer.until('error')
        self.assertEqual(error['id'], refused)
        self.assertIn('draft changed', error['message'])
        mismatch = peer.submit('different text', route='rollout', draft_revision=changed['revision'])
        self.assertEqual(peer.until('error')['id'], mismatch)
        self.assertFalse(any(event['type'] == 'input_received' for event in self.events()))
        request = peer.submit('draft-bound prompt', route='rollout',
                              draft_revision=changed['revision'])
        result = peer.result(request)
        self.assertEqual(result['status'], 'committed')
        empty = peer.draft()
        self.assertEqual(empty['text'], '')
        self.assertEqual(empty['cursor'], 0)
        self.assertEqual(empty['revision'], result['draft_cleared'])
        self.assertGreater(empty['revision'], changed['revision'])
        newer = peer.replace_draft(empty['revision'], 'next unsent draft', edit=5)['draft']
        peer.submit('draft-bound prompt', request, route='rollout',
                    draft_revision=changed['revision'])
        self.assertEqual(peer.result(request), result)
        self.assertEqual(peer.draft(), newer)
        self.assertEqual(len([event for event in self.events()
                              if event['type'] == 'input_received']), 1)

    def test_listener_cleanup_preserves_replaced_endpoint(self):
        self.detach()
        peer = self.view(bind=True)
        endpoint = self.directory / 'view.sock'
        endpoint.rename(self.directory / 'old-view.sock')
        endpoint.write_text('replacement owned by the fixture')
        peer.send(type='quit', generation=peer.generation)
        peer.until('control')
        self.status('stored')
        self.assertEqual(endpoint.read_text(), 'replacement owned by the fixture')

    def test_idle_observer_has_no_frames_and_partial_request_expires(self):
        self.detach()
        peer = self.view()
        peer.until('state')
        self.assertFalse(select.select([peer.peer], [], [], .2)[0])
        peer.peer.sendall(b'SV')
        peer.peer.settimeout(7)
        self.assertEqual(peer.peer.recv(1), b'')
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)
        bound = self.view(bind=True)
        if not bound.states:
            bound.until('state')

        def cpu_time():
            value = subprocess.check_output(['ps', '-p', str(self.owner), '-o', 'time='],
                                            text=True).strip()
            total = 0.0
            for field in value.split(':'):
                total = total * 60 + float(field)
            return total

        before = cpu_time()
        self.assertFalse(select.select([bound.peer], [], [], .5)[0])
        self.assertLess(cpu_time() - before, .3, 'idle semantic controller spins its owner')

    def test_idle_attachment_status_precedes_metrics_sampling(self):
        self.detach()
        # No prompt, model event or sampling deadline should be needed to
        # publish either edge of an idle semantic attachment.
        for _ in range(2):
            peer = self.view(bind=True)
            self.status('attached')
            peer.send(type='detach', generation=peer.generation)
            peer.until('detached')
            self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)

    def test_detach_and_quit_have_distinct_owner_lifetimes(self):
        self.detach()
        peer = self.view(bind=True)
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        self.status('detached')
        self.assertEqual(self.identity(), self.owner_identity)
        other = self.view(bind=True)
        other.send(type='quit', generation=other.generation)
        self.assertEqual(other.until('control')['intent'], 'quit')
        self.assertEqual(other.until('exit')['status'], 0)
        self.assertEqual(other.peer.recv(1), b'')
        self.status('stored')
        self.assertFalse((self.directory / 'view.sock').exists())
        self.assertFalse((self.directory / 'terminal.sock').exists())


class ContextMeterTests(unittest.TestCase):
    def setUp(self):
        self.owner = SessionViewTests('test_legacy_observer_is_a_committed_prefix') if (
            'legacy' in self._testMethodName) else SessionViewTests()
        self.owner.setUp()
        self.addCleanup(self.owner.tearDown)
        self.provider = self.owner.provider
        self.ready = threading.Event()
        self.first = threading.Event()
        self.second = threading.Event()
        self.release = self.owner.release
        self.addCleanup(self.first.set)
        self.addCleanup(self.second.set)
        text = self.owner.config.read_text().replace('exact_token_count = false',
                                                    'exact_token_count = true')
        text = text.replace('native_compaction = false', 'native_compaction = true')
        text = text.replace('idle_timeout_ms = 3000', 'idle_timeout_ms = 15000')
        text = text.replace('request_timeout_ms = 5000', 'request_timeout_ms = 20000')
        text += ('prompt = METER {context}% {rollout-idle:› }{rollout-active:» }{chat:: }\n'
                 '[model-limit fake/host-model]\nmax_input_tokens = 100000\n'
                 'max_output_tokens = 10000\n')
        self.owner.config.write_text(text)
        self.provider.runtime_count_handler = lambda handler, request: self.provider.reply(
            handler, b'{"object":"response.input_tokens","input_tokens":1000}',
            content_type='application/json')
        self.configure()

    def configure(self):
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'/configure\r')
        self.owner.initial.until(b'configuration reloaded:')
        self.owner.initial.until(b'METER 0%')

    def wait_meter(self, peer, predicate):
        try:
            return peer.until('state', lambda message:
                              predicate(message['state']['prompt']['values'][5]))['state']['prompt']
        except socket.timeout:
            self.fail(('context meter did not update',
                       [state['prompt']['values'][5] for state in peer.states[-5:]]))

    def classic_meter(self, value):
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'\x0c')
        self.owner.initial.until(('METER ' + value + '%').encode())

    def stream(self, arguments=False):
        def respond(handler, request, sequence):
            event = self.provider.event
            rid, item_id = 'resp_meter', 'meter_item'
            item = ({'type': 'function_call', 'id': item_id, 'call_id': 'meter_call',
                     'name': 'read_file', 'arguments': '', 'status': 'in_progress'} if arguments else
                    {'type': 'message', 'id': item_id, 'role': 'assistant',
                     'phase': 'final_answer', 'status': 'in_progress', 'content': []})
            position = {'item_id': item_id, 'output_index': 0, 'content_index': 0}
            handler.send_response(200)
            handler.send_header('Content-Type', 'text/event-stream')
            handler.send_header('Connection', 'close')
            handler.end_headers()

            def send(kind, data):
                handler.wfile.write(event(kind, data).encode())
                handler.wfile.flush()

            send('response.created', {'response': {'id': rid, 'status': 'in_progress',
                                                   'output': []}})
            send('response.output_item.added', {'output_index': 0, 'item': item})
            if not arguments:
                send('response.content_part.added', {**position,
                     'part': {'type': 'output_text', 'text': '', 'annotations': []}})
            self.ready.set()
            self.first.wait(10)
            chunk = ('{"path":"' + 'a' * 8192 if arguments else
                     'meter-first ' + 'a ' * 4096 + ' first-chunk-done\n')
            delta = 'response.function_call_arguments.delta' if arguments else (
                'response.output_text.delta')
            send(delta, {**position, 'delta': chunk})
            self.second.wait(10)
            tail = 'b' * 8192 + '"}' if arguments else 'b ' * 4096 + ' meter-last\n'
            send(delta, {**position, 'delta': tail})
            self.release.wait(10)
            if arguments:
                return
            text = chunk + tail
            send('response.output_text.done', {**position, 'text': text})
            item.update(status='completed', content=[{'type': 'output_text', 'text': text,
                                                     'annotations': []}])
            send('response.output_item.done', {'output_index': 0, 'item': item})
            send('response.completed', {'response': {'id': rid, 'status': 'completed',
                 'output': [], 'usage': {'input_tokens': 1000, 'output_tokens': 6000,
                                        'total_tokens': 7000}}})

        self.provider.runtime_handler = respond

    def test_calibrated_output_units_reach_exec_and_retained_pages(self):
        text = self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false')
        text = text.replace('max_input_tokens = 100000', 'max_input_tokens = 1000000')
        self.owner.config.write_text(text + 'tool_output_approx_tokens = 4000\n')
        self.configure()
        self.command('/model cache', 'cache updated:')
        path = self.owner.root / 'state' / 'models.json'
        cache = json.loads(path.read_text())
        cache['providers'][0]['models'][0]['id'] = 'host-model'
        path.write_text(json.dumps(cache))
        self.command('/model list', 'host-model')
        self.release.set()

        samples = []
        expected_limits = []
        def respond(handler, request, sequence):
            started = [e['data'] for e in self.owner.events()
                       if e['type'] == 'response_started'][-1]
            sample_bytes = started['model_input_bytes']
            tokens = sample_bytes * 2
            previous_bytes, previous_tokens = samples[-1] if samples else (0, 0)
            samples.append((previous_bytes // 2 + sample_bytes,
                            previous_tokens // 2 + tokens))
            if sequence < 3:
                # Decay rounds each total independently before budget conversion.
                expected_limits.append(1000 * samples[-1][0] // samples[-1][1])
            elif sequence == 3:
                expected_limits.append(1024)
            if sequence == 1:
                body = self.provider.function_body(sequence, 'emit', 'exec_command', {
                    'command': "printf '%05000d' 0", 'yield_ms': 10000,
                    'max_output_tokens': 1000})
            elif sequence in (2, 3):
                result = [e['data']['result'] for e in self.owner.events()
                          if e['type'] == 'tool_finished'][0]
                key = 'max_output_approx_tokens' if sequence == 2 else 'max_output_bytes'
                body = self.provider.function_body(sequence, 'page' + str(sequence),
                    'read_tool_output', {'handle': result['output_ref']['handle'],
                                         'stream': 'stdout', key: 1000 if sequence == 2 else 1024})
            else:
                body = self.provider.response_body(sequence, 'units complete')
            body = body.replace('"input_tokens":1', f'"input_tokens":{tokens}').replace(
                '"total_tokens":2', f'"total_tokens":{tokens + 1}')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.owner.detach()
        peer = self.owner.view(bind=True)
        peer.submit('exercise calibrated output units')
        self.owner.wait_event('turn_completed')
        results = [e['data']['result'] for e in self.owner.events()
                   if e['type'] == 'tool_finished']
        self.assertEqual([r['status'] for r in results], ['succeeded'] * 3)
        # The historical result metadata stores bytes, independently of input units.
        self.assertEqual([r['max_output_tokens'] for r in results], expected_limits)
        self.assertEqual(results[0]['stdout']['original_bytes'], 5000)
        calibration = json.loads(path.read_text())['providers'][0]['models'][0][
            'token_calibration']
        self.assertEqual(calibration['samples'], 4)
        # Independent integer decay can leave a one-token rounding difference.
        self.assertIn(calibration['tokens'] - 2 * calibration['bytes'], (0, 1))

    def test_calibrated_density_survives_compaction_and_resume(self):
        self.owner.config.write_text(self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false'))
        self.configure()
        self.command('/model cache', 'cache updated:')
        path = self.owner.root / 'state' / 'models.json'
        cache = json.loads(path.read_text())
        cache['providers'][0]['models'][0]['id'] = 'host-model'
        path.write_text(json.dumps(cache))
        self.command('/model list', 'host-model')
        self.release.set()

        def respond(handler, request, sequence):
            started = [e['data'] for e in self.owner.events()
                       if e['type'] == 'response_started'][-1]
            if sequence == 2:
                self.ready.set()
                self.first.wait(10)
            tokens = started['model_input_bytes'] // 2
            body = self.provider.response_body(sequence, 'calibration answer')
            body = body.replace('"input_tokens":1', f'"input_tokens":{tokens}').replace(
                '"total_tokens":2', f'"total_tokens":{tokens + 1}')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        self.owner.detach()
        peer = self.owner.view(bind=True)
        peer.submit('establish density ' + 'x' * 4000)
        first_turn = self.owner.wait_event('turn_completed')
        # Drain the completed state before a multi-frame write on small sockets.
        peer.until('state', lambda message: not message['state']['prompt']['active'] and
                   message['state']['prompt']['values'][5] not in ('0', '~0'))
        calibration = json.loads(path.read_text())['providers'][0]['models'][0][
            'token_calibration']
        self.assertEqual(calibration['samples'], 1)
        self.assertAlmostEqual(calibration['bytes'] / calibration['tokens'], 2, places=2)
        base = [e['data'] for e in self.owner.events() if e['type'] == 'response_completed'][0]
        base = base['usage']['input_tokens'] + base['usage']['output_tokens']
        suffix = 'new Unicode and code ' + '漢字🙂{};' * 600
        peer.submit(suffix)
        self.assertTrue(self.ready.wait(5))
        expected_floor = (base + len(suffix.encode()) // 2) * 100 // 100000
        prompt = self.wait_meter(peer, lambda value: value.startswith('~') and
                                 int(value[1:]) >= expected_floor)
        self.assertTrue(prompt['active'])
        self.first.set()
        self.owner.wait_event('turn_completed', first_turn['seq'])
        calibration = json.loads(path.read_text())['providers'][0]['models'][0][
            'token_calibration']
        self.assertEqual(calibration['samples'], 2)
        peer.send(type='detach', generation=peer.generation)
        peer.until('detached')
        self.owner.initial = self.owner.start(['--resume', self.owner.sid])
        self.owner.initial.until(b'METER ')
        peer = self.owner.view()
        self.command('/status', 'measured samples')

        def compact(handler, request):
            self.provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'message', 'role': 'user', 'content': 'summary ' + 's' * 120000},
                {'type': 'compaction', 'encrypted_content': 'A' * 200000}]}).encode(),
                content_type='application/json')

        self.provider.runtime_compact_handler = compact
        self.command('/compact', 'Compacted')
        self.wait_meter(peer, lambda value: value.startswith('~') and 60 <= int(value[1:]) < 100)
        self.assertEqual(len(self.provider.requests), 2)
        self.owner.finish(self.owner.initial, b'/exit')
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until(b'METER ~')
        view = self.owner.view()
        self.wait_meter(view, lambda value: value.startswith('~') and 60 <= int(value[1:]) < 100)
        self.assertEqual(len(self.provider.requests), 2)

        # The capsule makes the next complete byte/token pair incomparable.
        # Exact reported usage still overrides the calibrated preview.
        def opaque_response(handler, request, sequence):
            body = self.provider.response_body(sequence, 'opaque answer').replace(
                '"input_tokens":1', '"input_tokens":1000').replace(
                '"total_tokens":2', '"total_tokens":1001')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = opaque_response
        previous_seq = self.owner.events()[-1]['seq']
        os.write(resumed.master, b'continue after opaque compaction\r')
        self.owner.wait_event('turn_completed', previous_seq)
        self.wait_meter(view, lambda value: value == '2')
        self.assertEqual(json.loads(path.read_text())['providers'][0]['models'][0][
            'token_calibration'], calibration)
        self.assertEqual(len(self.provider.requests), 3)

        # Even a measured input/output prefix cannot isolate replayed tokens:
        # hidden reasoning need not reappear in the provider's next input.
        def hidden_response(handler, request, sequence):
            body = self.provider.response_body(sequence, 'hidden reasoning answer').replace(
                '"input_tokens":1', '"input_tokens":90000').replace(
                '"output_tokens":1', '"output_tokens":60,"output_tokens_details":{"reasoning_tokens":59}').replace(
                '"total_tokens":2', '"total_tokens":90060')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = hidden_response
        previous_seq = self.owner.events()[-1]['seq']
        os.write(resumed.master, b'continue with a measured opaque prefix\r')
        self.owner.wait_event('turn_completed', previous_seq)
        self.assertEqual(json.loads(path.read_text())['providers'][0]['models'][0][
            'token_calibration'], calibration)
        self.assertEqual(len(self.provider.requests), 4)
        self.owner.finish(resumed, b'/exit')

    def test_meter_grows_before_completion_in_classic_and_workspace_prompt(self):
        self.stream()
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'grow the response\r')
        self.assertTrue(self.ready.wait(5))
        self.wait_meter(peer, lambda value: value == '1')
        self.classic_meter('1')
        self.first.set()
        self.owner.initial.until(b'first-chunk-done')
        first = self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) >= 3)
        self.classic_meter(first['values'][5])
        self.assertFalse(any(e['type'] == 'response_completed' for e in self.owner.events()))
        self.second.set()
        self.owner.initial.until(b'meter-last')
        self.wait_meter(peer, lambda value: value.startswith('~') and
                        int(value[1:]) > int(first['values'][5][1:]))
        self.release.set()
        self.owner.wait_event('turn_completed')
        self.wait_meter(peer, lambda value: value == '7')
        self.classic_meter('7')
        done = self.owner.events()
        self.assertEqual(next(e['data']['input_tokens_bound'] for e in done
                              if e['type'] == 'response_started'), 1000)
        self.assertEqual(next(e['data']['usage']['output_tokens'] for e in done
                              if e['type'] == 'response_completed'), 6000)

    def test_tool_arguments_update_the_meter_without_public_text(self):
        self.stream(arguments=True)
        self.owner.detach()
        peer = self.owner.view(bind=True)
        self.assertEqual(peer.result(peer.submit('grow tool arguments'))['status'], 'committed')
        self.assertTrue(self.ready.wait(5))
        self.wait_meter(peer, lambda value: value == '1')
        self.first.set()
        first = self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) >= 3)
        self.second.set()
        self.wait_meter(peer, lambda value: value.startswith('~') and
                        int(value[1:]) > int(first['values'][5][1:]))
        self.assertFalse(any(e['type'] in ('response_output', 'response_completed', 'tool_started')
                             for e in self.owner.events()))
        peer.send(type='cancel', generation=peer.generation)
        self.release.set()
        self.owner.wait_event('turn_interrupted')

    def test_encrypted_continuation_does_not_inflate_completion_meter(self):
        def respond(handler, request, sequence):
            response_id = 'resp_opaque_meter'
            handler.send_response(200)
            handler.send_header('Content-Type', 'text/event-stream')
            handler.send_header('Connection', 'close')
            handler.end_headers()
            handler.close_connection = True

            def send(kind, data):
                handler.wfile.write(self.provider.event(kind, data).encode())
                handler.wfile.flush()

            send('response.created', {'response': {'id': response_id,
                 'status': 'in_progress', 'output': []}})
            send('response.output_item.done', {'output_index': 0, 'item': {
                'type': 'reasoning', 'id': 'opaque_item', 'summary': [],
                'encrypted_content': 'A' * 600000}})
            text = 'opaque completion boundary\n'
            item = {'type': 'message', 'id': 'opaque_answer', 'role': 'assistant',
                    'status': 'completed', 'content': [{'type': 'output_text',
                    'text': text, 'annotations': []}]}
            send('response.output_item.done', {'output_index': 1, 'item': item})
            self.release.wait(10)
            send('response.completed', {'response': {'id': response_id,
                 'status': 'completed', 'output': [], 'usage': {
                 'input_tokens': 1000, 'output_tokens': 2000, 'total_tokens': 3000}}})

        self.provider.runtime_handler = respond
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'finish with opaque continuation\r')
        self.owner.initial.until(b'opaque completion boundary')
        self.wait_meter(peer, lambda value: value == '~2')
        self.assertFalse(any(state['prompt']['values'][5] == '~100'
                             for state in peer.states))
        self.release.set()
        self.owner.wait_event('turn_completed')
        peer.until('state', lambda message: message['state']['prompt']['values'][5] == '3'
                   and not message['state']['prompt']['active'])
        self.classic_meter('3')

    def test_next_request_uses_usage_instead_of_encrypted_output_size(self):
        self.owner.config.write_text(self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false'))
        self.configure()
        self.release.set()

        def respond(handler, request, sequence):
            if sequence == 2:
                self.ready.set()
                self.first.wait(10)
            rid = 'resp_meter_' + str(sequence)
            handler.send_response(200)
            handler.send_header('Content-Type', 'text/event-stream')
            handler.send_header('Connection', 'close')
            handler.end_headers()
            handler.close_connection = True
            data = self.provider.event('response.created', {
                'response': {'id': rid, 'status': 'in_progress', 'output': []}})
            data += self.provider.event('response.output_item.done', {
                'output_index': 0, 'item': {'type': 'reasoning', 'id': 'opaque_' + rid,
                'summary': [], 'encrypted_content': 'A' * 600000}})
            data += self.provider.event('response.output_item.done', {
                'output_index': 1, 'item': {'type': 'message', 'id': 'answer_' + rid,
                'role': 'assistant', 'status': 'completed', 'content': [
                {'type': 'output_text', 'text': 'meter answer', 'annotations': []}]}})
            data += self.provider.event('response.completed', {'response': {
                'id': rid, 'status': 'completed', 'output': [], 'usage': {
                'input_tokens': 60000 if sequence == 1 else 62000,
                'output_tokens': 2000, 'total_tokens': 62000 if sequence == 1 else 64000}}})
            handler.wfile.write(data.encode())
            handler.wfile.flush()

        self.provider.runtime_handler = respond
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'establish known usage\r')
        self.owner.wait_event('turn_completed')
        self.wait_meter(peer, lambda value: value == '62')
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'continue with retained reasoning\r')
        self.assertTrue(self.ready.wait(5))
        prompt = self.wait_meter(peer, lambda value: value.startswith('~'))
        self.assertTrue(62 <= int(prompt['values'][5][1:]) <= 65, prompt)
        self.classic_meter(prompt['values'][5])
        self.assertTrue(any('encrypted_content' in item for item in
                            self.provider.requests[-1]['body']['input']))
        self.first.set()
        self.wait_meter(peer, lambda value: value == '64')
        peer.until('state', lambda message: not message['state']['prompt']['active'])
        self.owner.finish(self.owner.initial, b'/exit')
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until(b'METER ~')
        view = self.owner.view()
        prompt = self.wait_meter(view, lambda value: value.startswith('~'))
        self.assertTrue(64 <= int(prompt['values'][5][1:]) <= 67, prompt)
        self.assertEqual(len(self.provider.requests), 2)
        self.owner.finish(resumed, b'/exit')

    def test_next_request_meter_survives_legacy_resume(self):
        self.test_next_request_uses_usage_instead_of_encrypted_output_size()

    def test_compaction_refreshes_known_capacity_without_a_count_endpoint(self):
        self.owner.config.write_text(self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false'))
        self.configure()
        counts, compacts = [], []
        self.provider.runtime_count_handler = lambda handler, request: counts.append(request)

        def compact(handler, request):
            compacts.append(request)
            self.provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'compaction', 'encrypted_content': 'opaque-meter-summary'}]}).encode(),
                content_type='application/json')

        self.provider.runtime_compact_handler = compact
        os.write(self.owner.initial.master, b'remember retained context\r')
        self.owner.wait_event('turn_completed')
        peer = self.owner.view()
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'/compact\r')
        self.owner.initial.until(b'Compacted')
        prompt = self.wait_meter(peer, lambda value: value.startswith('~') and
                                 value[1:].isdigit())
        self.classic_meter(prompt['values'][5])
        self.assertFalse(prompt['active'])
        self.assertEqual(counts, [])
        self.assertEqual(len(compacts), 1)
        self.assertEqual(len(self.provider.requests), 1)
        self.assertEqual(self.owner.wait_event('compaction_completed')['data'][
            'output_tokens_bound'], 0)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)

    def test_compaction_meter_survives_legacy_resume_without_provider_requests(self):
        self.test_compaction_refreshes_known_capacity_without_a_count_endpoint()
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        history = self.owner.journal.read_bytes()
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until(b'METER ~')
        peer = self.owner.view()
        self.wait_meter(peer, lambda value: value.startswith('~') and value[1:].isdigit())
        current = self.owner.journal.read_bytes()
        self.assertTrue(current.startswith(history))
        joined = [json.loads(line) for line in current[len(history):].splitlines()]
        self.assertEqual([event['type'] for event in joined], ['irc_snapshot'])
        self.assertEqual(joined[0]['data']['reason'], 'join')
        os.write(resumed.master, b'\x0c')
        resumed.until(b'METER ~')
        self.assertEqual(self.owner.journal.read_bytes(), current)
        self.assertEqual(len(self.provider.requests), 1)
        self.owner.finish(resumed, b'/exit')

    def check_compacted_preview(self, measured=False):
        self.owner.config.write_text(self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false').replace(
            '[ui]\n', '[ui]\nresume_history_turns=0\n'))
        self.configure()
        self.provider.runtime_handler = lambda handler, request, sequence: self.provider.reply(
            handler, self.provider.response_body(sequence, 'archived answer ' + 'x' * 300000).encode(),
            close_header=True)
        counts, compacts = [], []
        self.provider.runtime_count_handler = lambda handler, request: counts.append(request)

        def compact(handler, request):
            compacts.append(request)
            self.provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'compaction', 'encrypted_content': 'opaque-preview'}]}).encode(),
                content_type='application/json')

        self.provider.runtime_compact_handler = compact
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        result = subprocess.run(self.owner.prefix + ['--resume', self.owner.sid, '-e', '--',
                                                    'remember the archived answer'],
                                cwd=self.owner.root, env={**os.environ, **self.owner.env},
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.owner.initial = self.owner.start(['--resume', self.owner.sid])
        self.owner.initial.until('›'.encode(), 10)
        peer = self.owner.view()
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'/compact\r')
        self.owner.initial.until(b'Compacted')
        prompt = self.wait_meter(peer, lambda value: value.startswith('~'))
        self.assertLess(int(prompt['values'][5][1:]), 20)
        if measured:
            self.command('/model fake/host-model/high:100000', 'selected=100000')
            self.provider.runtime_handler = lambda handler, request, sequence: self.provider.reply(
                handler, self.provider.response_body(sequence, 'post-compact answer').encode(),
                close_header=True)
            self.owner.initial.output.clear()
            os.write(self.owner.initial.master, b'continue after compaction\r')
            peer.until('state', lambda message: not message['state']['prompt']['active'] and
                       message['state']['prompt']['values'][5] == '1')
        requests = 2 if measured else 1
        catalogs = self.provider.catalog_paths()
        self.command('/model fake/two-model/high:100000', 'selected=100000')
        prompt = peer.until('state', lambda message:
                            message['state']['prompt']['values'][1] == 'two-model' and
                            not message['state']['prompt']['active'])['state']['prompt']
        value = prompt['values'][5]
        self.assertTrue(value.startswith('~'), value)
        self.assertGreaterEqual(int(value[1:]), 70, 'preview kept the incompatible opaque summary')
        self.classic_meter(value)
        self.command('/model fake/host-model/high:100000', 'selected=100000')
        prompt = peer.until('state', lambda message:
                            message['state']['prompt']['values'][1] == 'host-model' and
                            not message['state']['prompt']['active'])['state']['prompt']
        self.assertLess(int(prompt['values'][5][1:]), 20)
        config = self.owner.config.read_text()
        self.owner.config.write_text(config.replace('${SNAJPAGENT_IRC_UI_KEY}', '"changed-key"'))
        self.owner.config.chmod(0o600)
        self.command('/configure', 'configuration reloaded:')
        self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) >= 70)
        self.owner.config.write_text(config)
        self.command('/configure', 'configuration reloaded:')
        self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) < 20)
        self.owner.config.write_text(config.replace(
            f':{self.provider.port}/v1', f':{self.provider.port}/other'))
        self.command('/configure', 'configuration reloaded:')
        self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) >= 70)
        self.owner.config.write_text(config)
        self.command('/configure', 'configuration reloaded:')
        self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) < 20)
        self.owner.config.write_text(config + (
            f'\n[provider offline]\nbase_url=http://127.0.0.1:{self.provider.port}/v1\n'
            'api_key=missing-preview-key\n'))
        self.command('/configure', 'configuration reloaded:')
        self.command('/model offline/host-model/high:100000', 'selected=100000')
        self.wait_meter(peer, lambda value: value == '?')
        self.command('/model fake/host-model/high:100000', 'selected=100000')
        self.wait_meter(peer, lambda value: value.startswith('~') and int(value[1:]) < 20)
        self.owner.config.write_text(config)
        self.command('/configure', 'configuration reloaded:')
        self.assertEqual(counts, [])
        self.assertEqual(len(compacts), 1)
        self.assertEqual(len(self.provider.requests), requests)
        self.assertEqual(self.provider.catalog_paths(), catalogs)
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.initial = self.owner.start(['--resume', self.owner.sid, '-m',
                                               'fake/two-model/high:100000'])
        self.owner.initial.until('›'.encode(), 10)
        resumed = self.owner.view()
        self.wait_meter(resumed, lambda value: value.startswith('~') and int(value[1:]) >= 70)
        self.assertEqual(len(self.provider.requests), requests)
        self.assertEqual(len(compacts), 1)
        self.assertEqual(counts, [])
        self.owner.finish(self.owner.initial, b'/exit')

    def test_compacted_preview_uses_selected_binding(self):
        self.check_compacted_preview()

    def test_compacted_preview_uses_selected_binding_after_legacy_resume(self):
        self.check_compacted_preview()

    def test_compacted_preview_rejects_incompatible_usage(self):
        self.check_compacted_preview(measured=True)

    def test_compacted_preview_rejects_incompatible_usage_after_legacy_resume(self):
        self.check_compacted_preview(measured=True)

    def check_usage_scope(self, changed=True, exact=False, resumed=False, missing_output=False,
                          unmeasured=False):
        config = self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false')
        self.owner.config.write_text(config)
        self.configure()
        compacts, counts = [], []

        def respond(handler, request, sequence):
            if sequence == 1:
                events = [self.provider.event('response.output_item.done', {'output_index': 0,
                    'item': {'type': 'reasoning', 'id': 'scoped_reasoning', 'summary': [],
                             'encrypted_content': 'A' * 80000}}),
                    self.provider.event('response.output_item.done', {'output_index': 1,
                    'item': {'type': 'message', 'id': 'scoped_answer', 'role': 'assistant',
                             'status': 'completed', 'content': [{'type': 'output_text',
                             'text': 'first public answer', 'annotations': []}]}})]
                body = self.provider.response_envelope(sequence, events).replace(
                    '"input_tokens":1', '"input_tokens":1000').replace(
                    '"output_tokens":1', '"output_tokens":59000').replace(
                    '"total_tokens":2', '"total_tokens":60000')
            else:
                body = self.provider.response_body(sequence, 'small public answer')
                if sequence == 2:
                    if missing_output:
                        events = [self.provider.event('response.output_item.done', {'output_index': 0,
                            'item': {'type': 'reasoning', 'id': 'unmeasured_output', 'summary': [],
                                     'encrypted_content': 'B' * 80000}}),
                            self.provider.event('response.output_item.done', {'output_index': 1,
                            'item': {'type': 'message', 'id': 'input_only_answer', 'role': 'assistant',
                                     'status': 'completed', 'content': [{'type': 'output_text',
                                     'text': 'small public answer', 'annotations': []}]}})]
                        body = self.provider.response_envelope(sequence, events)
                    body = body.replace('"input_tokens":1', '"input_tokens":60000').replace(
                        '"total_tokens":2', '"total_tokens":60001')
                    if missing_output:
                        body = body.replace('"output_tokens":1', '"output_tokens":null').replace(
                            '"total_tokens":60001', '"total_tokens":null')
                elif unmeasured and sequence == 3:
                    body = body.replace('"input_tokens":1', '"input_tokens":null').replace(
                        '"total_tokens":2', '"total_tokens":null')
            self.provider.reply(handler, body.encode(), close_header=True)

        def compact(handler, request):
            compacts.append(request)
            self.provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'compaction', 'encrypted_content': 'scope-summary'}]}).encode(),
                content_type='application/json')

        def count(handler, request):
            counts.append(request)
            self.provider.reply(handler, b'{"object":"response.input_tokens","input_tokens":1000}',
                                content_type='application/json')

        self.provider.runtime_handler = respond
        self.provider.runtime_compact_handler = compact
        self.provider.runtime_count_handler = count
        peer = self.owner.view()
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'first measured response\r')
        self.wait_meter(peer, lambda value: value == '60')
        first_completed = self.owner.wait_event('response_completed')['seq']
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'second measured response\r')
        self.owner.wait_event('response_completed', after_seq=first_completed)
        self.wait_meter(peer, lambda value: value == ('~61' if missing_output else '61'))
        measured = [e for e in read_events(self.owner.journal) if e['type'] == 'response_completed']
        self.assertEqual(measured[-1]['data']['usage']['input_tokens'], 60000)
        self.assertTrue(any(item.get('encrypted_content')
                            for item in self.provider.requests[-1]['body']['input']))
        if changed:
            config = config.replace('${SNAJPAGENT_IRC_UI_KEY}', '"changed-key"')
            self.provider.credential = 'changed-key'
        if unmeasured:
            self.owner.config.write_text(config)
            self.owner.config.chmod(0o600)
            self.command('/configure', 'configuration reloaded:')
            self.wait_meter(peer, lambda value: value.startswith('~'))
            unmeasured_seq = max(e['seq'] for e in read_events(self.owner.journal))
            self.owner.initial.output.clear()
            os.write(self.owner.initial.master, b'response with no measured input\r')
            # The peer can still hold an idle state from the preceding response.
            self.owner.wait_event('turn_completed', after_seq=unmeasured_seq)
            peer.until('state', lambda message: not message['state']['prompt']['active'])
            completed = [e for e in read_events(self.owner.journal)
                         if e['type'] == 'response_completed']
            self.assertIsNone(completed[-1]['data']['usage']['input_tokens'])
        config = config.replace('auto_compact_input_tokens = 0',
                                'auto_compact_input_tokens = 50000')
        if exact:
            config = config.replace('exact_token_count = false', 'exact_token_count = true')
        self.owner.config.write_text(config)
        self.owner.config.chmod(0o600)
        self.command('/configure', 'configuration reloaded:')
        self.owner.initial.until_after(b'configuration reloaded:', b'METER ')
        peer = self.owner.view()
        prompt = self.wait_meter(peer, lambda value: value.startswith('~'))
        preview = int(prompt['values'][5][1:])
        if changed:
            self.assertLess(preview, 20)
        else:
            self.assertGreaterEqual(preview, 60, 'preview lost compatible measured input usage')
            self.assertLess(preview, 65, 'preview counted opaque encrypted output bytes')
        if resumed:
            self.owner.finish(self.owner.initial, b'/exit')
            self.owner.status('stored')
            self.owner.initial = self.owner.start(['--resume', self.owner.sid])
            self.owner.initial.until('›'.encode(), 10)
            peer = self.owner.view()
            prompt = self.wait_meter(peer, lambda value: value.startswith('~'))
            self.assertEqual(int(prompt['values'][5][1:]) < 20, changed)
        status = self.command('/status', 'session usage:')
        self.assertIn(b'input=60000 tokens', status)
        self.assertIn(b'historical;' if changed else b'matching selection and compaction', status)
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'/context\r')
        self.owner.initial.until_after(b'compact=50000 (fixed)', b'METER ')
        advice = bytes(self.owner.initial.output)
        started_at = read_events(self.owner.journal)[-1]['seq']
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'next response under selected credentials\r')
        peer.until('state', lambda message: not message['state']['prompt']['active'] and
                   message['state']['prompt']['values'][5] == '1')
        events = [e for e in read_events(self.owner.journal) if e['seq'] > started_at]
        expected = int(not changed and not exact)
        self.assertEqual(len(compacts), expected, 'incompatible input usage triggered compaction')
        self.assertEqual(len([e for e in events if e['type'] == 'compaction_started']), expected)
        self.assertEqual(len([e for e in events if e['type'] == 'compaction_completed']), expected)
        self.assertFalse(any(e['type'] in ('turn_failed', 'turn_recovery', 'context_rebased')
                             for e in events))
        self.assertEqual(len(self.provider.requests), 4 if unmeasured else 3)
        self.assertEqual(bool(counts), exact)
        if exact:
            starts = [e for e in events if e['type'] == 'response_started']
            self.assertEqual(len(counts), 1)
            self.assertEqual(starts[-1]['data']['count_method'], 'exact')
            self.assertEqual(starts[-1]['data']['input_tokens_bound'], 1000)
        encrypted = any(item.get('encrypted_content')
                        for item in self.provider.requests[-1]['body']['input'])
        self.assertEqual(encrypted, not changed)
        self.assertNotIn(b'the next request compacts first', advice)
        self.assertEqual(b'last measured input has reached an active limit' in advice, not changed)
        self.assertEqual(b"compaction depends on the next request's accounting" in advice,
                         not changed)
        self.owner.finish(self.owner.initial, b'/exit')

    def test_compaction_rejects_credential_incompatible_usage(self):
        self.check_usage_scope()

    def test_compaction_rejects_credential_incompatible_usage_after_legacy_resume(self):
        self.check_usage_scope(resumed=True)

    def test_compaction_rejects_credential_incompatible_usage_after_native_resume(self):
        self.check_usage_scope(resumed=True)

    def test_compaction_retains_credential_compatible_usage(self):
        self.check_usage_scope(changed=False)

    def test_compaction_retains_credential_compatible_usage_after_legacy_resume(self):
        self.check_usage_scope(changed=False, resumed=True)

    def test_compaction_retains_input_usage_without_output_usage(self):
        self.check_usage_scope(changed=False, missing_output=True)

    def test_input_only_display_survives_legacy_reload_and_resume(self):
        self.check_usage_scope(changed=False, missing_output=True, resumed=True)

    def test_input_only_display_survives_native_reload_and_resume(self):
        self.check_usage_scope(changed=False, missing_output=True, resumed=True)

    def test_input_only_display_rejects_changed_credential(self):
        self.check_usage_scope(missing_output=True)

    def check_exact_preview_fallback(self, exact=True, output_known=True, changed=False,
                                     resumed=False, reported_input=None, outcome='completed'):
        counts = []

        def count(handler, request):
            counts.append(request)
            self.provider.reply(handler, b'{"object":"response.input_tokens","input_tokens":60000}',
                                content_type='application/json', close_header=True)

        def respond(handler, request, sequence):
            if outcome == 'cancelled':
                self.ready.set()
                self.release.wait(10)
            elif outcome == 'failed':
                self.provider.reply(handler, b'{"error":{"code":"permission_denied",'
                                    b'"message":"exact-fallback-denied"}}',
                                    content_type='application/json', status=403, close_header=True)
                return
            body = self.provider.response_body(sequence, 'exact-fallback-answer').replace(
                '"input_tokens":1', '"input_tokens":' + json.dumps(reported_input)).replace(
                '"total_tokens":2', '"total_tokens":null')
            if not output_known:
                body = body.replace('"output_tokens":1', '"output_tokens":null')
            self.provider.reply(handler, body.encode(), close_header=True)

        config = self.owner.config.read_text()
        if not exact:
            config = config.replace('exact_token_count = true', 'exact_token_count = false')
            self.owner.config.write_text(config)
            self.configure()
        self.provider.runtime_count_handler = count
        self.provider.runtime_handler = respond
        peer = self.owner.view()
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'exact input with absent provider input usage\r')
        if outcome == 'cancelled':
            self.assertTrue(self.ready.wait(5), 'counted response did not start')
            self.owner.wait_event('response_started')
            os.write(self.owner.initial.master, b'\x03')
            self.owner.wait_event('turn_interrupted')
            self.release.set()
        elif outcome == 'failed':
            self.owner.wait_event('turn_failed')
        else:
            self.owner.wait_event('turn_completed')
            self.owner.initial.until_after(b'exact-fallback-answer', b'METER ')
            if reported_input is not None:
                self.wait_meter(peer, lambda value: value == '2')
            elif exact:
                self.wait_meter(peer, lambda value: value == ('61' if output_known else '~61'))
            else:
                self.wait_meter(peer, lambda value: value.startswith('~'))
        events = read_events(self.owner.journal)
        completed = [e for e in events if e['type'] == 'response_completed']
        if outcome == 'completed':
            self.assertEqual(len(completed), 1)
            self.assertEqual(completed[0]['data']['usage']['input_tokens'], reported_input)
            self.assertEqual(completed[0]['data']['usage']['output_tokens'],
                             1 if output_known else None)
        else:
            self.assertEqual(completed, [], 'uncompleted response supplied a display binding')
        started = [e for e in events if e['type'] == 'response_started'][-1]
        self.assertEqual(started['data']['count_method'], 'exact' if exact else 'unknown')
        self.assertEqual(started['data']['input_tokens_bound'], 60000 if exact else 0)
        # Provider-failure recovery admits one retry before the turn fails.
        attempts = 2 if outcome == 'failed' else 1
        self.assertEqual(len([e for e in events if e['type'] == 'response_started']), attempts)
        if changed:
            config = config.replace('${SNAJPAGENT_IRC_UI_KEY}', '"changed-key"')
            self.provider.credential = 'changed-key'
        self.owner.config.write_text(config)
        self.owner.config.chmod(0o600)
        self.command('/configure', 'configuration reloaded:')
        self.owner.initial.until_after(b'configuration reloaded:', b'METER ')
        self.assertEqual(len(counts), attempts * int(exact),
                         'reload preview performed another count request')
        self.assertEqual(len(self.provider.requests), attempts,
                         'reload preview submitted another response')
        if resumed:
            self.owner.finish(self.owner.initial, b'/exit')
            self.owner.initial = self.owner.start(['--resume', self.owner.sid])
            self.owner.initial.until('›'.encode(), 10)
        peer = self.owner.view()
        preview = self.wait_meter(peer, lambda value: value.startswith('~'))['values'][5]
        # Credential rotation invalidates exact anchors, but the same model's
        # text density remains useful for an explicitly approximate preview.
        if exact and reported_input is None and outcome == 'completed':
            self.assertGreaterEqual(int(preview[1:]), 60,
                                    'preview lost the completed exact input count')
            self.assertLess(int(preview[1:]), 65)
        else:
            self.assertLess(int(preview[1:]), 20, 'preview invented a matching input measurement')
        self.assertEqual(len(counts), attempts * int(exact))
        self.assertEqual(len(self.provider.requests), attempts)
        events = read_events(self.owner.journal)
        self.assertFalse(any(e['type'] in ('compaction_started', 'context_rebased') for e in events))
        self.assertEqual(len([e for e in events if e['type'] == 'turn_failed']),
                         int(outcome == 'failed'))
        self.assertEqual(len([e for e in events if e['type'] == 'turn_interrupted']),
                         int(outcome == 'cancelled'))
        self.classic_meter(preview)
        if not exact and reported_input is None:
            self.command('/status', 'uncalibrated')
        self.owner.finish(self.owner.initial, b'/exit')

    def test_exact_preview_fallback_retains_input_without_provider_usage(self):
        self.check_exact_preview_fallback()

    def test_exact_preview_fallback_survives_native_resume(self):
        self.check_exact_preview_fallback(resumed=True)

    def test_exact_preview_fallback_survives_legacy_resume(self):
        self.check_exact_preview_fallback(resumed=True)

    def test_exact_preview_fallback_estimates_unreported_output(self):
        self.check_exact_preview_fallback(output_known=False)

    def test_exact_preview_fallback_rejects_changed_credential(self):
        self.check_exact_preview_fallback(changed=True)

    def test_exact_preview_fallback_keeps_uncounted_input_unknown(self):
        self.check_exact_preview_fallback(exact=False)

    def test_exact_preview_fallback_prefers_reported_input_usage(self):
        self.check_exact_preview_fallback(reported_input=1000)

    def test_exact_preview_fallback_rejects_cancelled_response(self):
        self.check_exact_preview_fallback(outcome='cancelled', resumed=True)

    def test_exact_preview_fallback_rejects_cancelled_legacy_response(self):
        self.check_exact_preview_fallback(outcome='cancelled', resumed=True)

    def test_exact_preview_fallback_rejects_failed_response(self):
        self.check_exact_preview_fallback(outcome='failed', resumed=True)

    def test_exact_preview_fallback_rejects_failed_legacy_response(self):
        self.check_exact_preview_fallback(outcome='failed', resumed=True)

    def check_output_only_preview(self, output_known=False, opaque=False, changed=False,
                                  unreported=False, summary=False, cancelled=False,
                                  followup=False):
        config = self.owner.config.read_text().replace(
            'exact_token_count = true', 'exact_token_count = false')
        self.owner.config.write_text(config +
            f'[irc]\nlisten = 127.0.0.1:{harness.free_loopback_port()}\n')
        self.configure()
        counts = []
        followup_release = threading.Event()
        self.addCleanup(followup_release.set)

        def count(handler, request):
            counts.append(request)
            self.provider.reply(handler, b'{"object":"response.input_tokens","input_tokens":1}',
                                content_type='application/json', close_header=True)

        def respond(handler, request, sequence):
            if followup and sequence == 2:
                self.second.set()
                followup_release.wait(10)
                return self.provider.reply(
                    handler, self.provider.response_body(sequence, 'follow-up').encode(),
                    close_header=True)
            self.ready.set()
            self.release.wait(10)
            rid = 'output_only_response'
            output = ([{'type': 'message', 'id': 'output_only_message', 'role': 'assistant',
                        'status': 'completed', 'content': [
                            {'type': 'output_text', 'text': 'visible follow-up anchor'}]}]
                      if followup else []) + (
                          [{'type': 'reasoning', 'id': 'output_only_reasoning',
                            'summary': [{'type': 'summary_text', 'text': 'private' * 20000}]
                                       if summary else [],
                            'encrypted_content': 'opaque' * 40000}]
                          if opaque or summary else [])
            usage = {'input_tokens': None, 'output_tokens': 60000 if output_known else None,
                     'output_tokens_details': {'reasoning_tokens': None if unreported else 50000},
                     'total_tokens': None}
            events = [
                ('response.created', {'response': {'id': rid, 'status': 'in_progress',
                                                   'output': []}}),
                ('response.completed', {'response': {'id': rid, 'status': 'completed',
                                                    'output': output, 'usage': usage}}),
            ]
            body = ''.join('event: ' + kind + '\ndata: ' +
                           json.dumps({'type': kind, **data}) + '\n\n' for kind, data in events)
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_count_handler = count
        self.provider.runtime_handler = respond
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'output-only preview without measured input\r')
        self.assertTrue(self.ready.wait(5))
        self.owner.wait_event('response_started')
        before = self.wait_meter(peer, lambda value: value.startswith('~'))['values'][5]
        self.assertLess(int(before[1:]), 20)
        if cancelled:
            os.write(self.owner.initial.master, b'\x03')
            self.owner.wait_event('turn_interrupted')
        self.release.set()
        if not cancelled:
            self.owner.wait_event('turn_completed' if followup else 'turn_completed_silent')
        events = read_events(self.owner.journal)
        completed_events = [e for e in events if e['type'] == 'response_completed']
        if cancelled:
            self.assertEqual(completed_events, [], 'cancelled output supplied a display observation')
        else:
            usage = completed_events[-1]['data']['usage']
            self.assertIsNone(usage['input_tokens'])
            self.assertEqual(usage['output_tokens'], 60000 if output_known else None)
            self.assertEqual(usage['reasoning_tokens'], None if unreported else 50000)
        floor = 0 if cancelled or unreported else (60 if output_known else 50)
        peer = self.owner.view()
        completed = self.wait_meter(peer, lambda value: value.startswith('~'))['values'][5]
        self.assertGreaterEqual(int(completed[1:]), floor)
        self.assertLess(int(completed[1:]), floor + 20)
        followup_seq = max((event['seq'] for event in self.owner.events()), default=0)
        if followup:
            self.owner.detach()
            peer = self.owner.view(bind=True)
            followup_text = b'follow-up context ' + (b'x' * 40000)
            request = peer.submit(followup_text.decode())
            self.assertEqual(peer.result(request)['status'], 'committed')
            self.assertTrue(self.second.wait(5))
            started = self.owner.wait_event('response_started', after_seq=followup_seq)
            self.assertEqual(started['data']['count_method'], 'unknown')
            self.assertGreaterEqual(len(self.provider.requests), 2)
            self.assertTrue(any(followup_text.decode() in json.dumps(request['body'])
                                for request in self.provider.requests))
            # Read the current snapshot, not an earlier queued active preview.
            observer = self.owner.view()
            followup_meter = observer.until('state', lambda message:
                message['state']['prompt']['active'] and
                message['state']['prompt']['values'][5].startswith('~'))[
                    'state']['prompt']['values'][5]
            self.assertGreaterEqual(int(followup_meter[1:]), int(completed[1:]) + 8,
                                    'later context was hidden by aggregate output accounting')
            followup_release.set()
            self.owner.wait_event('turn_completed', after_seq=followup_seq)
            self.assertEqual(counts, [], 'follow-up performed an exact count')
            self.assertFalse(any(e['type'] in ('compaction_started', 'turn_failed',
                                              'turn_recovery') for e in self.owner.events()))
            peer.send(type='quit', generation=peer.generation)
            self.assertEqual(peer.until('exit')['status'], 0)
            peer.close()
            return
        if changed:
            self.owner.config.write_text(self.owner.config.read_text().replace(
                '${SNAJPAGENT_IRC_UI_KEY}', '"changed-key"'))
            self.owner.config.chmod(0o600)
            self.provider.credential = 'changed-key'
        self.command('/configure', 'configuration reloaded:')
        self.owner.initial.until_after(b'configuration reloaded:', b'METER ')
        peer = self.owner.view()
        preview = self.wait_meter(peer, lambda value: value.startswith('~'))['values'][5]
        if changed:
            self.assertLess(int(preview[1:]), 20, 'changed credential reused an output observation')
        else:
            self.assertGreaterEqual(int(preview[1:]), int(before[1:]) + floor - 1,
                                    'reload discarded known output-only usage')
            self.assertLess(int(preview[1:]), floor + 20,
                            'preview counted aggregate output twice or included ciphertext')
        self.classic_meter(preview)
        self.assertEqual(counts, [], 'unmeasured preview performed an exact count')
        self.assertEqual(len(self.provider.requests), 1)
        self.assertFalse(any(e['type'] in ('compaction_started', 'tool_started',
                                          'turn_failed', 'turn_recovery') for e in events))
        self.owner.finish(self.owner.initial, b'/exit')
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until(b'METER ~')
        peer = self.owner.view()
        restored = self.wait_meter(peer, lambda value: value.startswith('~'))['values'][5]
        self.assertGreaterEqual(int(restored[1:]), int(preview[1:]) - 1)
        self.assertLess(int(restored[1:]), int(preview[1:]) + 3)
        self.assertEqual(counts, [], 'resume performed an exact count')
        self.assertEqual(len(self.provider.requests), 1, 'resume submitted another response')
        self.owner.finish(resumed, b'/exit')

    def test_output_only_reasoning_survives_unmeasured_input_reload_and_resume(self):
        self.check_output_only_preview()

    def test_output_only_reasoning_survives_unmeasured_input_reload_and_resume_legacy(self):
        self.check_output_only_preview()

    def test_output_only_aggregate_survives_unmeasured_input_reload_and_resume(self):
        self.check_output_only_preview(output_known=True)

    def test_output_only_aggregate_survives_unmeasured_input_reload_and_resume_legacy(self):
        self.check_output_only_preview(output_known=True)

    def test_output_only_reasoning_excludes_encrypted_bytes_without_measured_input(self):
        self.check_output_only_preview(opaque=True)

    def test_output_only_reasoning_excludes_encrypted_bytes_without_measured_input_legacy(self):
        self.check_output_only_preview(opaque=True)

    def test_output_only_observation_rejects_changed_credential(self):
        self.check_output_only_preview(changed=True)

    def test_output_only_observation_rejects_changed_credential_legacy(self):
        self.check_output_only_preview(changed=True)

    def test_output_only_preview_keeps_unreported_output_unknown(self):
        self.check_output_only_preview(unreported=True)

    def test_output_only_preview_keeps_unreported_output_unknown_legacy(self):
        self.check_output_only_preview(unreported=True)

    def test_output_only_aggregate_covers_visible_reasoning_and_ciphertext(self):
        self.check_output_only_preview(output_known=True, summary=True)

    def test_output_only_aggregate_covers_visible_reasoning_and_ciphertext_legacy(self):
        self.check_output_only_preview(output_known=True, summary=True)

    def test_output_only_preview_rejects_uncompleted_response(self):
        self.check_output_only_preview(cancelled=True)

    def test_output_only_preview_rejects_uncompleted_response_legacy(self):
        self.check_output_only_preview(cancelled=True)

    def test_output_only_aggregate_retains_later_context(self):
        self.check_output_only_preview(output_known=True, followup=True)

    def test_output_only_aggregate_retains_later_context_legacy(self):
        self.check_output_only_preview(output_known=True, followup=True)

    def check_partial_usage_tool(self, repeated=False, valid=False):
        counts = []

        def count(handler, request):
            counts.append(request)
            self.provider.reply(handler, b'{"object":"response.input_tokens","input_tokens":60000}',
                                content_type='application/json', close_header=True)

        def respond(handler, request, sequence):
            if sequence > 1 and not repeated:
                body = self.provider.response_body(sequence, 'valid-after-partial-usage')
            else:
                rid = 'partial_total_response_' + str(sequence)
                item = {'type': 'function_call', 'id': 'partial_total_item_' + str(sequence),
                        'call_id': 'partial_total_call_' + str(sequence), 'name': 'get_cwd',
                        'arguments': '{}', 'status': 'completed'}
                usage = {'input_tokens': 60000, 'output_tokens': None,
                         'total_tokens': 65000 if valid else 60000,
                         'output_tokens_details': {'reasoning_tokens': 5000}}
                events = [
                    ('response.created', {'response': {'id': rid, 'status': 'in_progress',
                                                       'output': []}}),
                    ('response.output_item.added', {'output_index': 0, 'item': {
                        **item, 'arguments': '', 'status': 'in_progress'}}),
                    ('response.output_item.done', {'output_index': 0, 'item': item}),
                    ('response.completed', {'response': {'id': rid, 'status': 'completed',
                                                        'usage': usage, 'output': [item]}}),
                ]
                body = ''.join('event: ' + kind + '\ndata: ' +
                               json.dumps({'type': kind, **data}) + '\n\n'
                               for kind, data in events)
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_count_handler = count
        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'partial usage tool admission\r')
        self.owner.wait_event('turn_failed' if repeated else 'turn_completed')
        events = read_events(self.owner.journal)
        completed = [e for e in events if e['type'] == 'response_completed']
        failed = [e for e in events if e['type'] == 'response_failed']
        response_count = 6 if repeated else 2
        self.assertEqual(len(self.provider.requests), response_count)
        self.assertEqual(len(counts), response_count)
        self.assertEqual(len(completed), 2 if valid else (0 if repeated else 1))
        self.assertEqual(len(failed), 0 if valid else (6 if repeated else 1))
        for event in failed:
            self.assertIn('inconsistent response usage', event['data']['message'])
        self.assertEqual(sum(e['type'] == 'turn_recovery' for e in events),
                         0 if valid else (5 if repeated else 1))
        for kind in ('tool_started', 'tool_finished'):
            self.assertEqual(sum(e['type'] == kind for e in events), int(valid),
                             'invalid partial usage admitted a staged tool')
        self.assertFalse(any(e['type'] == 'compaction_started' for e in events))
        if valid:
            usage = completed[0]['data']['usage']
            self.assertEqual(usage['input_tokens'], 60000)
            self.assertIsNone(usage['output_tokens'])
            self.assertEqual(usage['reasoning_tokens'], 5000)
            self.assertEqual(usage['total_tokens'], 65000)
        elif not repeated:
            self.assertEqual(completed[0]['data']['usage']['input_tokens'], 1)
        self.command('/configure', 'configuration reloaded:')
        self.owner.initial.until_after(b'configuration reloaded:', b'METER ')
        self.assertEqual(len(counts), response_count,
                         'failed or completed preview performed another count')
        self.assertEqual(len(self.provider.requests), response_count,
                         'preview submitted another response')
        self.owner.finish(self.owner.initial, b'/exit')
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until(b'METER ')
        self.assertEqual(len(counts), response_count, 'resume performed another count')
        self.assertEqual(len(self.provider.requests), response_count,
                         'resume submitted another response')
        self.owner.finish(resumed, b'/exit')

    def test_partial_usage_rejects_staged_tool_and_recovers(self):
        self.check_partial_usage_tool()

    def test_partial_usage_rejects_staged_tool_and_recovers_legacy(self):
        self.check_partial_usage_tool()

    def test_partial_usage_repeated_failure_never_admits_tool(self):
        self.check_partial_usage_tool(repeated=True)

    def test_partial_usage_repeated_failure_never_admits_tool_legacy(self):
        self.check_partial_usage_tool(repeated=True)

    def test_partial_usage_valid_total_admits_tool(self):
        self.check_partial_usage_tool(valid=True)

    def test_partial_usage_valid_total_admits_tool_legacy(self):
        self.check_partial_usage_tool(valid=True)

    def check_output_usage_meter(self, input_known=True, output_known=False,
                                 output_tokens=0, reasoning_tokens=None, text='', preview_only=False):
        self.owner.config.write_text(self.owner.config.read_text() +
            f'[irc]\nlisten = 127.0.0.1:{harness.free_loopback_port()}\n')
        self.configure()
        counts = []

        def count(handler, request):
            counts.append(request)
            self.provider.reply(handler, b'{"object":"response.input_tokens","input_tokens":60000}',
                                content_type='application/json', close_header=True)

        def respond(handler, request, sequence):
            self.ready.set()
            self.release.wait(10)
            body = self.provider.response_body(sequence, text).replace(
                '"input_tokens":1', '"input_tokens":60000' if input_known else '"input_tokens":null')
            body = body.replace('"output_tokens":1',
                                '"output_tokens":' + json.dumps(output_tokens if output_known else None) +
                                ',"output_tokens_details":{"reasoning_tokens":' +
                                json.dumps(reasoning_tokens) + '}')
            body = body.replace('"total_tokens":2', '"total_tokens":null')
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_count_handler = count
        self.provider.runtime_handler = respond
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'quiet completed output accounting\r')
        self.assertTrue(self.ready.wait(5), 'exactly counted response did not start')
        self.owner.wait_event('response_started')
        self.wait_meter(peer, lambda value: value == '60')
        self.release.set()
        self.owner.wait_event('turn_completed' if text else 'turn_completed_silent')
        peer = self.owner.view()
        completed_meter = self.wait_meter(peer, lambda value:
            value.lstrip('~').isdigit() and int(value.lstrip('~')) >= 60)['values'][5]
        events = read_events(self.owner.journal)
        completed = [e for e in events if e['type'] == 'response_completed']
        self.assertEqual(len(completed), 1)
        self.assertEqual(bool(completed[0]['data']['items']), bool(text))
        self.assertEqual(completed[0]['data']['usage']['input_tokens'], 60000 if input_known else None)
        self.assertEqual(completed[0]['data']['usage']['output_tokens'],
                         output_tokens if output_known else None)
        self.assertEqual(completed[0]['data']['usage']['reasoning_tokens'], reasoning_tokens)
        output = output_tokens if output_known else (
            (reasoning_tokens or 0) + (len(text.encode()) + 3) // 4)
        percent = (60000 + output + 999) // 1000
        if not preview_only:
            expected = str(percent) if output_known else '~' + str(percent)
            self.assertEqual(completed_meter, expected,
                             'completion discarded known output usage or counted it twice')
            self.classic_meter(completed_meter)
        self.command('/configure', 'configuration reloaded:')
        self.owner.initial.until_after(b'configuration reloaded:', b'METER ')
        peer = self.owner.view()
        preview = self.wait_meter(peer, lambda value: value.startswith('~'))['values'][5]
        self.assertGreaterEqual(int(preview[1:]), percent, 'preview discarded known output usage')
        self.assertLess(int(preview[1:]), percent + 5, 'preview counted output usage twice')
        self.assertEqual(len(counts), 1, 'preview performed another count request')
        self.assertEqual(len(self.provider.requests), 1, 'preview submitted another response')
        self.assertEqual(any(e['type'] == 'response_output' for e in read_events(self.owner.journal)),
                         bool(text))
        self.assertFalse(any(e['type'] in ('tool_started', 'compaction_started',
                                          'turn_failed', 'turn_recovery')
                             for e in read_events(self.owner.journal)))
        self.owner.finish(self.owner.initial, b'/exit')

    def test_quiet_meter_marks_missing_output_usage(self):
        self.check_output_usage_meter()

    def test_quiet_meter_marks_missing_output_usage_legacy(self):
        self.check_output_usage_meter()

    def test_quiet_meter_marks_missing_output_usage_with_exact_input(self):
        self.check_output_usage_meter(input_known=False)

    def test_quiet_meter_marks_missing_output_usage_with_exact_input_legacy(self):
        self.check_output_usage_meter(input_known=False)

    def test_quiet_meter_keeps_reported_zero_output_measured(self):
        self.check_output_usage_meter(output_known=True)

    def test_quiet_meter_keeps_reported_zero_output_measured_legacy(self):
        self.check_output_usage_meter(output_known=True)

    def test_quiet_meter_keeps_reported_zero_output_with_exact_input_measured(self):
        self.check_output_usage_meter(input_known=False, output_known=True)

    def test_quiet_meter_keeps_reported_zero_output_with_exact_input_measured_legacy(self):
        self.check_output_usage_meter(input_known=False, output_known=True)

    def test_reasoning_usage_survives_missing_output_total(self):
        self.check_output_usage_meter(reasoning_tokens=5000)

    def test_reasoning_usage_survives_missing_output_total_legacy(self):
        self.check_output_usage_meter(reasoning_tokens=5000)

    def test_reasoning_usage_combines_with_exact_input(self):
        self.check_output_usage_meter(input_known=False, reasoning_tokens=5000)

    def test_reasoning_usage_combines_with_exact_input_legacy(self):
        self.check_output_usage_meter(input_known=False, reasoning_tokens=5000)

    def test_reasoning_usage_survives_reload_without_output_total(self):
        self.check_output_usage_meter(reasoning_tokens=5000, preview_only=True)

    def test_reasoning_usage_survives_legacy_reload_without_output_total(self):
        self.check_output_usage_meter(reasoning_tokens=5000, preview_only=True)

    def test_reasoning_usage_survives_exact_input_reload(self):
        self.check_output_usage_meter(input_known=False, reasoning_tokens=5000, preview_only=True)

    def test_reasoning_usage_survives_exact_input_legacy_reload(self):
        self.check_output_usage_meter(input_known=False, reasoning_tokens=5000, preview_only=True)

    def test_reasoning_usage_combines_with_visible_output_estimate(self):
        self.check_output_usage_meter(reasoning_tokens=5000, text='visible')

    def test_reasoning_usage_combines_with_visible_output_estimate_legacy(self):
        self.check_output_usage_meter(reasoning_tokens=5000, text='visible')

    def test_reasoning_usage_is_not_added_to_reported_output_total(self):
        self.check_output_usage_meter(output_known=True, output_tokens=6000,
                                      reasoning_tokens=5000, text='visible')

    def test_reasoning_usage_is_not_added_to_reported_output_total_legacy(self):
        self.check_output_usage_meter(output_known=True, output_tokens=6000,
                                      reasoning_tokens=5000, text='visible')

    def test_compaction_exact_preflight_overrides_credential_incompatible_usage(self):
        self.check_usage_scope(exact=True)

    def test_context_advice_allows_exact_preflight_to_override_compatible_usage(self):
        self.check_usage_scope(changed=False, exact=True)

    def test_context_advice_allows_exact_override_after_legacy_resume(self):
        self.check_usage_scope(changed=False, exact=True, resumed=True)

    def test_compaction_unmeasured_response_keeps_measurement_binding(self):
        self.check_usage_scope(unmeasured=True)

    def check_compaction_control_interrupt(self, cancelled=False, credential=False, retry=False):
        compacts, counts = [], []
        self.addCleanup(self.release.set)
        config = self.owner.config.read_text().replace('max_input_tokens = 100000',
                                                      'max_input_tokens = 50000')
        self.owner.config.write_text(config)
        self.configure()

        def respond(handler, request, sequence):
            self.provider.reply(handler, self.provider.response_body(
                sequence, f'control-answer-{sequence}').encode(), close_header=True)

        def count(handler, request):
            text = json.dumps(request)
            over = ('continue after control interruption' in text and
                    'continue with retained context after cancellation' not in text)
            tokens = 60000 if over else 1000
            if any(item.get('type') == 'compaction' for item in request['input']):
                tokens = 1000
            counts.append(tokens)
            self.provider.reply(handler, json.dumps({'object': 'response.input_tokens',
                'input_tokens': tokens}).encode(), content_type='application/json', close_header=True)

        def compact(handler, request):
            compacts.append(request)
            number = len(compacts)
            if number == 1:
                self.ready.set()
                self.release.wait(10)
            self.provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'compaction', 'encrypted_content': f'control-summary-{number}'}]
                }).encode(), content_type='application/json', close_header=True)

        self.provider.runtime_handler = respond
        self.provider.runtime_count_handler = count
        self.provider.runtime_compact_handler = compact
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'keep completed history before control\r')
        self.owner.initial.until(b'control-answer-1')
        self.owner.wait_event('turn_completed')
        self.owner.initial.until('›'.encode())
        before = read_events(self.owner.journal)[-1]['seq']
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'continue after control interruption\r')
        self.assertTrue(self.ready.wait(5), 'the over-budget request did not compact')
        self.owner.wait_event('compaction_started')
        if cancelled:
            os.write(self.owner.initial.master, b'\x03')
        else:
            if not retry:
                config = config.replace('max_input_tokens = 50000', 'max_input_tokens = 100000')
            if credential:
                config = config.replace('${SNAJPAGENT_IRC_UI_KEY}', '"changed-key"')
                self.provider.credential = 'changed-key'
            self.owner.config.write_text(config)
            self.owner.config.chmod(0o600)
            self.command('/configure', 'accepted; applying at the next safe request boundary')
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            events = [e for e in read_events(self.owner.journal) if e['seq'] > before]
            if any(e['type'] in ('turn_completed', 'turn_failed', 'turn_interrupted')
                   for e in events):
                break
            time.sleep(.02)
        self.release.set()
        failures = [e['data'] for e in events if e['type'] in ('turn_failed', 'turn_recovery')]
        self.assertEqual(failures, [], 'control interruption was treated as a context failure')
        self.assertEqual(len([e for e in events if e['type'] == 'compaction_interrupted']), 1)
        self.assertEqual(len([e for e in events if e['type'] == 'compaction_completed']), int(retry))
        self.assertFalse(any(e['type'] == 'context_rebased' for e in events))
        self.assertEqual(len(compacts), 2 if retry else 1)
        if cancelled:
            self.assertEqual(len([e for e in events if e['type'] == 'turn_interrupted']), 1)
            self.assertFalse(any(e['type'] in ('response_started', 'turn_completed')
                                 for e in events))
            self.assertEqual(len(self.provider.requests), 1)
            self.owner.initial.output.clear()
            os.write(self.owner.initial.master, b'continue with retained context after cancellation\r')
            self.owner.initial.until(b'control-answer-2')
            self.owner.initial.until('›'.encode())
            self.assertIn('keep completed history before control',
                          json.dumps(self.provider.requests[-1]['body']))
            self.assertNotIn('control-summary-1', json.dumps(self.provider.requests[-1]['body']))
            self.assertEqual(len(compacts), 1)
        else:
            self.assertEqual(len([e for e in events if e['type'] == 'turn_completed']), 1)
            started = [e['data'] for e in events if e['type'] == 'response_started']
            self.assertEqual(len(started), 1)
            self.assertEqual(started[0]['input_tokens_bound'], 1000 if retry else 60000)
            self.assertGreaterEqual(started[0]['hard_input_tokens'], 1000 if retry else 60000)
            self.assertEqual(started[0]['count_method'], 'exact')
            self.assertEqual(counts.count(60000), 2)
            self.assertEqual(len(self.provider.requests), 2)
            self.assertIn('keep completed history before control',
                          json.dumps(compacts[-1] if retry else self.provider.requests[-1]['body']))
            self.assertNotIn('control-summary-1',
                             json.dumps(self.provider.requests[-1]['body']))
            if retry:
                self.assertIn('control-summary-2', json.dumps(self.provider.requests[-1]['body']))
            self.assertEqual(len([e for e in events if e['type'] == 'control_started']), 1)
            self.assertEqual(len([e for e in events if e['type'] == 'control_finished']), 1)
        self.owner.finish(self.owner.initial, b'/exit')

    def test_compaction_configure_interrupt_resumes_current_turn(self):
        self.check_compaction_control_interrupt()

    def test_compaction_configure_interrupt_resumes_legacy_turn(self):
        self.check_compaction_control_interrupt()

    def test_compaction_credential_reload_interrupt_resumes_current_turn(self):
        self.check_compaction_control_interrupt(credential=True)

    def test_compaction_unchanged_reload_retries_interrupted_request(self):
        self.check_compaction_control_interrupt(retry=True)

    def test_compaction_unchanged_reload_retries_interrupted_legacy_request(self):
        self.check_compaction_control_interrupt(retry=True)

    def test_compaction_cancellation_retains_previous_context(self):
        self.check_compaction_control_interrupt(cancelled=True)

    def check_post_turn_compaction_interrupt(self, action='reload'):
        config = self.owner.config.read_text().replace('auto_compact_input_tokens = 0',
                                                      'auto_compact_input_tokens = 50000')
        self.owner.config.write_text(config)
        self.configure()
        self.addCleanup(self.release.set)
        compacts = []

        def respond(handler, request, sequence):
            body = self.provider.response_body(sequence, f'post-turn-answer-{sequence}')
            if sequence == 1:
                body = body.replace('"input_tokens":1', '"input_tokens":60000').replace(
                    '"total_tokens":2', '"total_tokens":60001')
            self.provider.reply(handler, body.encode(), close_header=True)

        def compact(handler, request):
            compacts.append(request)
            self.ready.set()
            self.release.wait(10)
            self.provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'compaction', 'encrypted_content': 'post-turn-late-capsule'}]}).encode(),
                content_type='application/json', close_header=True)

        self.provider.runtime_handler = respond
        self.provider.runtime_compact_handler = compact
        before = read_events(self.owner.journal)[-1]['seq']
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'answer before interrupting post-turn compaction\r')
        self.assertTrue(self.ready.wait(5), 'post-turn compaction did not start')
        completed = self.owner.wait_event('turn_completed')
        started = self.owner.wait_event('compaction_started')
        response = self.owner.wait_event('response_completed')
        self.assertEqual(response['data']['usage']['input_tokens'], 60000)
        self.assertGreater(started['seq'], completed['seq'])
        self.assertEqual(started['data']['reason'], 'proactive')
        if action == 'cancel':
            os.write(self.owner.initial.master, b'\x03')
        else:
            config = config.replace('auto_compact_input_tokens = 50000',
                                    'auto_compact_input_tokens = 0')
            if action == 'credential':
                config = config.replace('${SNAJPAGENT_IRC_UI_KEY}', '"changed-key"')
                self.provider.credential = 'changed-key'
            self.owner.config.write_text(config)
            self.owner.config.chmod(0o600)
            self.command('/configure', 'accepted; applying at the next safe request boundary')
        self.owner.wait_event('compaction_interrupted')
        self.release.set()
        if action != 'cancel':
            self.owner.initial.until(b'configuration reloaded:')
            self.owner.wait_event('control_finished')
        self.owner.initial.until('›'.encode())
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, b'next small exact request after post-turn interruption\r')
        self.owner.initial.until(b'post-turn-answer-2')
        self.owner.initial.until('›'.encode())
        # Streamed answer bytes and an earlier prompt can precede durable completion.
        self.owner.wait_event('turn_completed', after_seq=completed['seq'])
        events = [e for e in read_events(self.owner.journal) if e['seq'] > before]
        names = [e['type'] for e in events]
        self.assertEqual(names.count('turn_completed'), 2)
        self.assertEqual(names.count('compaction_started'), 1)
        self.assertEqual(names.count('compaction_interrupted'), 1)
        self.assertEqual(names.count('control_finished'), int(action != 'cancel'))
        self.assertFalse(any(name in ('turn_failed', 'turn_recovery', 'turn_interrupted',
            'response_interrupted', 'compaction_completed', 'context_rebased') for name in names))
        self.assertEqual(len(self.provider.requests), 2)
        self.assertEqual(len(compacts), 1)
        self.assertIn('post-turn-answer-1', json.dumps(self.provider.requests[-1]['body']))
        self.assertNotIn('post-turn-late-capsule', json.dumps(self.provider.requests[-1]['body']))
        self.owner.finish(self.owner.initial, b'/exit')

    def test_post_turn_compaction_reload_retains_completed_answer(self):
        self.check_post_turn_compaction_interrupt()

    def test_post_turn_compaction_reload_retains_completed_legacy_answer(self):
        self.check_post_turn_compaction_interrupt()

    def test_post_turn_compaction_credential_reload_retains_completed_answer(self):
        self.check_post_turn_compaction_interrupt('credential')

    def test_post_turn_compaction_cancel_retains_completed_answer(self):
        self.check_post_turn_compaction_interrupt('cancel')

    def command(self, text, marker):
        self.owner.initial.output.clear()
        os.write(self.owner.initial.master, (text + '\r').encode())
        return self.owner.initial.until(marker.encode())

    def test_model_context_save_wire_and_resume(self):
        peer = self.owner.view()
        self.command('/model fake/host-model/high:60000 save', 'configuration saved:')
        prompt = peer.until('state', lambda message:
                            message['state']['prompt']['values'][2] == 'high')['state']['prompt']
        self.assertEqual(prompt['values'][1], 'host-model')
        self.assertNotEqual(prompt['values'][5], '?')
        saved = self.owner.config.read_text()
        self.assertIn('reasoning_effort = high', saved)
        self.assertIn('context_window_tokens = 60000', saved)
        self.assertIn('max_output_tokens = 10000', saved)
        self.assertEqual(self.owner.wait_event('context_selection_changed')['data']['new_tokens'],
                         60000)
        os.write(self.owner.initial.master, b'context selection wire identity\r')
        self.owner.wait_event('turn_completed')
        request, = self.provider.requests
        self.assertEqual(request['body']['model'], 'host-model')
        self.assertEqual(request['body']['reasoning']['effort'], 'high')
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        resumed = self.owner.start(['--resume', self.owner.sid])
        resumed.until(b'METER')
        os.write(resumed.master, b'/context\r')
        resumed.until(b'selected=60000')
        self.assertEqual(len(self.provider.requests), 1)
        self.owner.finish(resumed, b'/exit')

    def cache_maximum_context(self):
        self.command('/model cache', 'cache updated:')
        path = self.owner.root / 'state' / 'models.json'
        cache = json.loads(path.read_text())
        cache['providers'][0]['models'][0]['limits'].update({
            'context_window_tokens': 272000, 'max_context_window_tokens': 872000,
            'max_output_tokens': 16000, 'effective_context_window_percent': 95})
        path.write_text(json.dumps(cache))
        text = self.owner.config.read_text()
        text += ('[model-limit fake/standard-model]\ncontext_window_tokens = 200000\n'
                 'max_output_tokens = 16000\n')
        self.owner.config.write_text(text)
        self.configure()
        return path

    def test_model_default_save_resolves_target_and_survives_restart(self):
        self.cache_maximum_context()
        self.command('/context 300000', 'selected=300000')
        self.command('/model fake/standard-model/medium:default s', 'configuration saved:')
        self.command('/context', 'selected=200000')
        self.assertIn('context_window_tokens = 200000', self.owner.config.read_text())
        self.assertNotIn('context_window_tokens = 872000', self.owner.config.read_text())
        events = [event['data'] for event in self.owner.events()
                  if event['type'] == 'context_selection_changed']
        self.assertEqual((events[-1]['new_mode'], events[-1]['new_tokens']), ('default', 0))
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        for args in (['--resume', self.owner.sid], ['-N', 'saved-default-window']):
            child = self.owner.start(args)
            child.until(b'METER')
            os.write(child.master, b'/context\r')
            child.until(b'selected=200000')
            self.owner.finish(child, b'/exit')
        self.assertEqual(self.provider.requests, [])

    def test_numbered_default_save_uses_catalog_working_window(self):
        self.cache_maximum_context()
        text = self.owner.config.read_text().replace('context_window_tokens = 200000\n', '')
        self.owner.config.write_text(text)
        self.configure()
        self.command('/model #1:default save', 'configuration saved:')
        self.assertIn('context_window_tokens = 272000', self.owner.config.read_text())
        self.command('/context', 'selected=272000')
        self.assertEqual(self.provider.requests, [])

    def test_default_save_with_only_catalog_maximum(self):
        path = self.cache_maximum_context()
        cache = json.loads(path.read_text())
        cache['providers'][0]['models'][0]['limits']['context_window_tokens'] = None
        path.write_text(json.dumps(cache))
        self.owner.config.write_text(self.owner.config.read_text().replace(
            'context_window_tokens = 200000\n', ''))
        self.configure()
        self.command('/model fake/standard-model/medium:default save', 'configuration saved:')
        self.command('/context', 'selected=872000')
        self.assertIn('context_window_tokens = 872000', self.owner.config.read_text())
        self.assertEqual(self.provider.requests, [])

    def test_default_save_write_failure_preserves_selection(self):
        self.cache_maximum_context()
        before = self.owner.config.read_bytes()
        moved = self.owner.config.with_suffix('.saved')
        self.owner.config.rename(moved)
        self.owner.config.mkdir()
        try:
            self.command('/model fake/standard-model/high:default save', 'regular file')
            self.command('/context default s', 'context window is unknown')
            self.assertEqual(moved.read_bytes(), before)
        finally:
            self.owner.config.rmdir()
            moved.rename(self.owner.config)
        self.assertFalse(any(event['type'] in ('model_selection_changed',
                                              'context_selection_changed')
                             for event in self.owner.events()))
        self.assertEqual(self.provider.requests, [])

    def test_context_default_save_resolves_configured_window(self):
        self.cache_maximum_context()
        self.command('/model fake/standard-model/medium:max', 'selected=872000')
        self.command('/context default save', 'configuration saved:')
        self.command('/context', 'selected=200000')
        self.assertIn('context_window_tokens = 200000', self.owner.config.read_text())
        self.command('/context default s', 'configuration saved:')
        self.assertEqual(self.provider.requests, [])

    def test_model_max_save_uses_target_cache_and_persists_numeric_default(self):
        self.cache_maximum_context()
        self.provider.AGENTS = {**self.provider.AGENTS, 'standard-model': 'standardbot'}
        self.owner.config.chmod(0o640)
        peer = self.owner.view()
        self.command('/model fake/standard-model/medium:max s', 'configuration saved:')
        prompt = peer.until('state', lambda message:
                            message['state']['prompt']['values'][1] == 'standard-model')
        self.assertNotEqual(prompt['state']['prompt']['values'][5], '?')
        self.command('/context', 'selected=872000')
        saved = self.owner.config.read_text()
        self.assertIn('context_window_tokens = 872000', saved)
        self.assertNotIn('context_window_tokens = 200000', saved)
        self.assertIn('max_output_tokens = 16000', saved)
        self.assertIn('max_input_tokens = 100000', saved)
        self.assertEqual(self.owner.config.stat().st_mode & 0o777, 0o640)
        change = self.owner.wait_event('context_selection_changed')['data']
        self.assertEqual((change['new_mode'], change['new_tokens']), ('max', 0))
        os.write(self.owner.initial.master, b'cached maximum wire identity\r')
        self.owner.wait_event('turn_completed')
        request, = self.provider.requests
        self.assertEqual(request['body']['model'], 'standard-model')
        self.assertEqual(request['body']['reasoning']['effort'], 'medium')
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        for args in (['--resume', self.owner.sid], ['-N', 'saved-max-default']):
            child = self.owner.start(args)
            child.until(b'METER')
            os.write(child.master, b'/context\r')
            child.until(b'selected=872000')
            os.write(child.master, b'/model\r')
            child.until(b'standard-model')
            self.owner.finish(child, b'/exit')
        self.assertEqual(len(self.provider.requests), 1)

    def test_numbered_model_max_save(self):
        self.cache_maximum_context()
        self.command('/model #1:max save', 'configuration saved:')
        self.assertIn('context_window_tokens = 872000', self.owner.config.read_text())
        self.command('/context default', 'selected=872000')
        self.assertEqual(self.provider.requests, [])

    def test_context_max_save(self):
        self.cache_maximum_context()
        self.command('/model fake/standard-model/medium', 'standard-model / medium')
        self.command('/context 300000 s', 'configuration saved:')
        self.command('/context max save', 'configuration saved:')
        self.assertIn('context_window_tokens = 872000', self.owner.config.read_text())
        self.command('/context default', 'selected=872000')
        self.command('/context max s', 'configuration saved:')
        self.assertEqual(self.provider.requests, [])

    def test_max_save_failure_preserves_selection(self):
        cache_path = self.cache_maximum_context()
        before = self.owner.config.read_bytes()
        moved = self.owner.config.with_suffix('.saved')
        self.owner.config.rename(moved)
        self.owner.config.mkdir()
        try:
            self.command('/model fake/standard-model/high:max save', 'regular file')
            self.assertEqual(moved.read_bytes(), before)
        finally:
            self.owner.config.rmdir()
            moved.rename(self.owner.config)
        cache = json.loads(cache_path.read_text())
        cache['providers'][0]['base_url'] = 'http://127.0.0.1:1'
        cache_path.write_text(json.dumps(cache))
        self.command('/model fake/standard-model/high:max save', 'publishes no maximum context')
        self.assertEqual(self.owner.config.read_bytes(), before)
        self.assertFalse(any(event['type'] in ('model_selection_changed',
                                              'context_selection_changed')
                             for event in self.owner.events()))
        self.assertEqual(self.provider.requests, [])

    def test_invalid_model_context_preserves_model_and_config(self):
        before = self.owner.config.read_bytes()
        for selector in ('fake/two-model/high:0', 'fake/two-model/high:-1',
                         'fake/two-model/high:4000000001'):
            self.command('/model ' + selector + ' save', '4000000000')
        self.command('/model fake/host-model/high:9000 save', 'output reservation')
        self.command('/model fake/host-model/high:max save', 'publishes no maximum context')
        self.command('/context max save', 'publishes no maximum context')
        self.command('/model fake/host-model/high:default save', 'context window is unknown')
        self.assertEqual(self.owner.config.read_bytes(), before)
        self.assertFalse(any(event['type'] in ('model_selection_changed',
                                              'context_selection_changed')
                             for event in self.owner.events()))
        self.assertEqual(self.provider.requests, [])

    def lower_cached_maximum(self, cache_path):
        cache = json.loads(cache_path.read_text())
        cache['providers'][0]['models'][0]['id'] = 'host-model'
        cache_path.write_text(json.dumps(cache))

    def settings_wait(self, child):
        child.until(b'Waiting for valid model/context settings')
        deadline = time.monotonic() + 1.1
        while time.monotonic() < deadline:
            if select.select([child.master], [], [], .05)[0]:
                child.output.extend(os.read(child.master, 65536))
        self.assertNotIn(b'retrying after error', child.output)
        self.assertNotIn(b'Retrying turn after error', child.output)
        self.assertEqual(child.output.count(b'exceeds the advertised maximum'), 1)

    def test_inherited_context_is_validated_before_model_save(self):
        self.cache_maximum_context()
        self.command('/model fake/host-model/high:900000', 'selected=900000')
        before = self.owner.config.read_bytes()
        events = self.owner.events()
        for selector in ('fake/standard-model/medium', '#1'):
            self.command('/model ' + selector + ' save', 'exceeds the advertised maximum')
        self.assertEqual(self.owner.config.read_bytes(), before)
        self.assertEqual([e for e in self.owner.events() if e['type'] in
                          ('model_selection_changed', 'context_selection_changed')],
                         [e for e in events if e['type'] in
                          ('model_selection_changed', 'context_selection_changed')])
        self.command('/model #1:max', 'selected=872000')
        self.assertEqual(self.provider.requests, [])

    def test_startup_model_validates_inherited_context(self):
        self.cache_maximum_context()
        self.command('/context 900000', 'selected=900000')
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        before = self.owner.journal.read_bytes()
        # An empty -e reads stdin before opening the resumed session.
        result = subprocess.run([*self.owner.prefix, '--resume', self.owner.sid,
                                 '-m', 'fake/standard-model/medium', '-e'],
                                env={**os.environ, **self.owner.env}, input=b'',
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(b'exceeds the advertised maximum', result.stderr)
        self.assertEqual(self.owner.journal.read_bytes(), before)
        self.assertEqual(self.provider.requests, [])

    def test_model_tool_rejects_inherited_context_without_switching(self):
        self.cache_maximum_context()
        text = self.owner.config.read_text().replace('read_agents_md = false',
                                                    'read_agents_md = false\nallow_model_change = true')
        self.owner.config.write_text(text)
        self.configure()
        self.command('/context 900000', 'selected=900000')
        self.provider.AGENTS = {**self.provider.AGENTS, 'standard-model': 'standardbot'}

        def respond(handler, request, sequence):
            body = (self.provider.function_body(sequence, 'invalid_context', 'select_model',
                    {'selector': '#1'}) if sequence == 1 else
                    self.provider.response_body(sequence, 'selection rejected'))
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'try the incompatible model\r')
        self.owner.wait_event('tool_finished')
        self.assertFalse(any(e['type'] == 'model_selection_changed' for e in self.owner.events()))
        self.owner.wait_event('turn_completed')
        self.assertEqual([r['model'] for r in self.provider.requests], ['host-model', 'host-model'])
        outputs = [item['output'] for item in self.provider.requests[-1]['body']['input']
                   if item.get('type') == 'function_call_output']
        self.assertTrue(any('exceeds the advertised maximum' in output for output in outputs))

    def test_invalid_context_goal_waits_and_one_shot_stops(self):
        path = self.cache_maximum_context()
        self.command('/context 900000', 'selected=900000')
        self.lower_cached_maximum(path)
        self.command('/configure', 'configuration reloaded:')
        os.write(self.owner.initial.master, b'/goal context recovery fixture\r')
        self.settings_wait(self.owner.initial)
        self.assertEqual(self.provider.requests, [])
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        once = subprocess.run([*self.owner.prefix, '--resume', self.owner.sid, '-e'],
                              env={**os.environ, **self.owner.env}, input=b'',
                              capture_output=True, timeout=5)
        self.assertEqual(once.returncode, 2, once.stderr)
        self.assertNotIn(b'retrying after error', once.stderr)
        resumed = self.owner.start(['--resume', self.owner.sid])
        self.settings_wait(resumed)

        def respond(handler, request, sequence):
            self.ready.set()
            self.release.wait(10)
            self.provider.reply(handler, self.provider.response_body(sequence, 'repaired').encode(),
                                close_header=True)

        self.provider.runtime_handler = respond
        os.write(resumed.master, b'/context max\r')
        self.assertTrue(self.ready.wait(5), bytes(resumed.output))
        os.write(resumed.master, b'/goal pause\r')
        resumed.until(b'Goal paused')
        self.release.set()
        self.owner.wait_event('turn_completed')
        self.assertEqual(len(self.provider.requests), 1)
        self.owner.finish(resumed, b'/exit')

    def resume_invalid_context(self):
        path = self.cache_maximum_context()
        self.command('/context 900000', 'selected=900000')
        self.provider.AGENTS = {**self.provider.AGENTS, 'two-model': 'twobot'}

        def respond(handler, request, sequence):
            if sequence == 1:
                self.ready.set()
                self.release.wait(10)
            try:
                self.provider.reply(handler, self.provider.response_body(sequence, 'repaired').encode(),
                                    close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        self.provider.runtime_handler = respond
        os.write(self.owner.initial.master, b'preserve this unfinished context turn\r')
        self.assertTrue(self.ready.wait(5))
        started = self.owner.wait_event('turn_started')['data']['turn_id']
        self.owner.finish(self.owner.initial, b'/exit')
        self.release.set()
        self.owner.status('stored')
        self.lower_cached_maximum(path)
        resumed = self.owner.start(['--resume', self.owner.sid])
        self.settings_wait(resumed)
        self.assertEqual(len(self.provider.requests), 1)
        os.write(resumed.master, b'/model fake/two-model/high:60000\r')
        self.owner.wait_event('turn_completed')
        self.assertEqual([r['model'] for r in self.provider.requests], ['host-model', 'two-model'])
        self.assertEqual(self.owner.wait_event('turn_completed')['data']['turn_id'], started)
        self.assertIn('preserve this unfinished context turn',
                      json.dumps(self.provider.requests[-1]['body']))
        self.owner.finish(resumed, b'/exit')

    def test_invalid_resumed_context_can_change_model(self):
        self.resume_invalid_context()

    def test_invalid_resumed_context_can_change_model_legacy(self):
        self.resume_invalid_context()

    def test_model_context_start_resume_override_and_omission(self):
        self.owner.finish(self.owner.initial, b'/exit')
        self.owner.status('stored')
        sid = None
        for args, tokens in ((['-N', 'context-start', '-m',
                              'fake/host-model/high:70000'], 70000),
                             (['--resume', 'context-start', '-m',
                               'fake/host-model/low:80000'], 80000),
                             (['--resume', 'context-start', '-m',
                               'fake/host-model/high'], 80000)):
            if sid:
                args[1] = sid
            child = self.owner.start(args)
            child.until(b'METER')
            sid, = (path.parent.name for path in journal_paths(self.owner.root / 'state')
                    if path.parent.name != self.owner.sid)
            os.write(child.master, b'/context\r')
            child.until(('selected=' + str(tokens)).encode())
            self.owner.finish(child, b'/exit')
        child = self.owner.start(['--resume', sid, '-m',
                                  'fake/host-model/high:9000'])
        child.until(b'output reservation')
        child.process.wait(5)
        self.assertNotEqual(child.process.returncode, 0)
        child = self.owner.start(['--resume', sid])
        child.until(b'METER')
        os.write(child.master, b'/context\r')
        child.until(b'selected=80000')
        self.owner.finish(child, b'/exit')
        self.assertEqual(self.provider.requests, [])

    def test_cached_model_tool_applies_context_before_next_response(self):
        self.owner.config.write_text(self.owner.config.read_text().replace(
            '[agent]\n', '[agent]\nallow_model_change = true\n', 1))
        self.configure()
        self.command('/model cache', 'cache updated:')
        self.provider.AGENTS = {**self.provider.AGENTS, 'standard-model': 'standardbot'}

        def respond(handler, request, sequence):
            body = (self.provider.function_body(sequence, 'choose_context', 'select_model',
                    {'selector': '#1:120000'}) if request['model'] == 'host-model' else
                    self.provider.response_body(sequence, 'selected context via tool'))
            self.provider.reply(handler, body.encode(), close_header=True)

        self.provider.runtime_handler = respond
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'select a cached model and context\r')
        self.owner.wait_event('turn_completed')
        peer.until('state', lambda message:
                   message['state']['prompt']['values'][1:3] == ['standard-model', 'medium'])
        self.assertEqual([request['model'] for request in self.provider.requests],
                         ['host-model', 'standard-model'])
        self.command('/context', 'selected=120000')
        outputs = [item['output'] for item in self.provider.requests[-1]['body']['input']
                   if item.get('type') == 'function_call_output']
        self.assertTrue(any('context=120000 tokens' in output for output in outputs), outputs)

    def test_model_context_switch_during_request_keeps_wire_and_prompt_aligned(self):
        held = threading.Event()
        switched = threading.Event()
        self.provider.AGENTS = {**self.provider.AGENTS, 'two-model': 'twobot'}

        def respond(handler, request, sequence):
            if request['model'] == 'host-model':
                held.set()
                self.release.wait(10)
            else:
                switched.set()
            try:
                self.provider.reply(handler, self.provider.response_body(
                    sequence, 'context-switch-complete').encode(), close_header=True)
            except (BrokenPipeError, ConnectionResetError):
                pass

        self.provider.runtime_handler = respond
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'held selection request\r')
        self.assertTrue(held.wait(5))
        self.command('/model fake/two-model/high:120000', 'selected=120000')
        self.assertTrue(switched.wait(5))
        peer.until('state', lambda message:
                   message['state']['prompt']['values'][1:3] == ['two-model', 'high'])
        self.owner.wait_event('turn_completed')
        self.assertEqual([request['model'] for request in self.provider.requests],
                         ['host-model', 'two-model'])
        self.assertEqual(self.provider.requests[-1]['body']['reasoning']['effort'], 'high')
        self.command('/context', 'selected=120000')
        self.release.set()

    def test_selection_rebuilds_estimates_and_unknown_capacity_stays_unknown(self):
        os.write(self.owner.initial.master, b'one measured response\r')
        self.owner.wait_event('turn_completed')
        peer = self.owner.view()
        os.write(self.owner.initial.master, b'/model fake/uncached/high\r')
        peer.until('state', lambda message:
                   message['state']['prompt']['values'][1] == 'uncached' and
                   message['state']['prompt']['values'][5] == '?')
        os.write(self.owner.initial.master, b'/model fake/host-model/high\r')
        peer.until('state', lambda message:
                   message['state']['prompt']['values'][1] == 'host-model' and
                   message['state']['prompt']['values'][5].startswith('~'))
        self.assertEqual(len(self.provider.requests), 1)


if __name__ == '__main__':
    unittest.main(defaultTest='SessionViewTests.test_binary_feature_boundary' if OMITTED else None)
