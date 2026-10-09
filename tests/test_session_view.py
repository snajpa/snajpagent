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

    def wait_event(self, kind):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            events = [event for event in self.events() if event['type'] == kind]
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
        self.command('/model fake/host-model/high:default save', 'explicit token count')
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
        result = subprocess.run([*self.owner.prefix, '--resume', self.owner.sid,
                                 '-m', 'fake/standard-model/medium', '-e'],
                                env={**os.environ, **self.owner.env}, capture_output=True,
                                timeout=5)
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
                              env={**os.environ, **self.owner.env}, capture_output=True,
                              timeout=5)
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
