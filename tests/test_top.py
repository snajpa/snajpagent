#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Real catalog/metrics and PTYs cover top navigation and owner confirmation."""
import fcntl
import hashlib
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

import store_history
from test_remote_terminal import RemoteProcess
from tmux_terminal import FakeResponses, write_irc_config


PRODUCT = Path(sys.argv.pop(1)).resolve()


class TopTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snag-top-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.state = self.root / 'state'
        self.workers = []
        self.addCleanup(self.close_workers)

    def close_workers(self):
        for worker in self.workers:
            if worker.poll() is None:
                worker.terminate()
            worker.wait(timeout=5)
            worker.stdout.close()

    def session(self, total=0, live=False):
        journal = store_history.create_legacy(self.state, self.root, 'fake', 'top-fixture')
        sid = journal.parent.name
        worker = None
        if live:
            worker = subprocess.Popen([sys.executable, '-u', '-c',
                'import fcntl,signal,sys; f=open(sys.argv[1],"r+"); '
                'fcntl.lockf(f,fcntl.LOCK_EX); print("ready"); signal.pause()',
                str(journal.parent / 'lock')], stdout=subprocess.PIPE, text=True)
            self.workers.append(worker)
            self.assertEqual(worker.stdout.readline().strip(), 'ready')
        self.metrics(sid, total, worker.pid if worker else 0)
        return sid, worker

    def metrics(self, sid, total, pid=0, started=1000):
        directory = self.state / 'metrics'
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / (sid + '.prom')
        temporary = path.with_suffix('.tmp')
        temporary.write_text(
            'snajpagent_session_info{provider="fake",model="top-fixture",'
            'active_turn_id="",state="running"} 1\n'
            'snajpagent_session_turns_total 3\n'
            f'snajpagent_session_process_total_tokens_total {total}\n'
            f'snajpagent_session_total_tokens_total {total}\n'
            f'snajpagent_session_process_pid {pid}\n'
            f'snajpagent_session_process_started_ms {started}\n')
        temporary.chmod(0o600)
        temporary.replace(path)

    def top(self, height=24, width=120, delay='1', wait=True, ignore_sigchld=False, config=None, color="always", sort=None):
        # Keep the controlling PTY alive after top exits so Darwin still exposes
        # its termios state. Without this parent, exit invalidates the slave.
        # PENDIN is a kernel retype request, not an application terminal mode.
        runner = ('import signal,subprocess,sys,termios; before=termios.tcgetattr(0); '
                  + ('signal.signal(signal.SIGCHLD,signal.SIG_IGN); ' if ignore_sigchld else '') +
                  'child=subprocess.Popen(sys.argv[1:]); '
                  'print("TOP_CHILD="+str(child.pid),flush=True); rc=child.wait(); '
                  'after=termios.tcgetattr(0); mask=getattr(termios,"PENDIN",0); '
                  'before[3]&=~mask; after[3]&=~mask; '
                  'assert after==before,(before,after); '
                  'print("TOP_TERMINAL_RESTORED",flush=True); sys.exit(rc)')
        top = RemoteProcess(self.root, [sys.executable, '-c', runner,
                            str(PRODUCT), 'top', '--dotdir', str(self.state),
                            *(['--color', color] if color else []),
                            *(['--config', str(config)] if config else []),
                            *(['--sort', sort] if sort else []),
                            *([] if delay is None else ['--delay', delay])],
                            wrapped=None)
        self.addCleanup(top.close)
        fcntl.ioctl(top.master, termios.TIOCSWINSZ, struct.pack('HHHH', height, width, 0, 0))
        if wait:
            self.frame(top)
        return top

    def frame(self, top, keys=b''):
        if keys:
            top.output.clear()
            os.write(top.master, keys)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            frame = bytes(top.output).split(b'\033[2J\033[H')[-1]
            if b'loading metadata...' not in frame and re.search(
                    rb'TOTAL sessions=[^\r\n]*[\r\n]', frame):
                return frame
            self.assertIsNone(top.process.poll(), bytes(top.output))
            if select.select([top.master], [], [], .05)[0]:
                top.output.extend(os.read(top.master, 65536))
        self.fail('top did not finish its frame: ' + repr(bytes(top.output)))

    def command(self, top, keys, expected):
        top.output.clear()
        os.write(top.master, keys)
        top.until(expected)
        if expected in (b'Fields:', b'interactive commands'):
            top.until(b'DECODE/s' if expected == b'Fields:' else b'Press any key to return')
            return bytes(top.output)
        return self.frame(top)

    def signal_selected(self, top, expected):
        self.command(top, b'\r', b'Signal for PID')
        return self.command(top, b'\r', expected)

    def selected(self, frame):
        match = re.search(rb'\033\[7m([0-9a-f]{8}) ', frame)
        self.assertIsNotNone(match, frame)
        return match[1].decode()

    def quit(self, top):
        os.write(top.master, b'q')
        # Drain restoration before wait closes the parent's macOS slave.
        top.until(b'\033[?25h')
        top.until(b'TOP_TERMINAL_RESTORED\r\n')
        top.wait(0)
        self.assertNotIn(b"\x00", bytes(top.output))

    def test_inherited_ignored_sigchld_keeps_backfill_waitable(self):
        store_history.create_legacy(self.state, self.root, 'fake', 'cold')
        top = self.top(delay=None, ignore_sigchld=True)
        self.assertIn(b'TOTAL sessions=1', top.output)
        self.quit(top)

    def test_signal_restores_terminal_and_collects_worker(self):
        top = self.top(delay=None)
        match = re.search(rb'TOP_CHILD=([0-9]+)', top.output)
        self.assertIsNotNone(match, top.output)
        os.kill(int(match[1]), signal.SIGTERM)
        top.until(b'TOP_TERMINAL_RESTORED\r\n')
        top.wait(128 + signal.SIGTERM)

    def test_fractional_delay_validation(self):
        for delay in ('0', '.199', '0.199', '-.2', 'nan', 'inf', '2e-1', '1.',
                      '.2000', '86400.001', '9999999999999999999999'):
            with self.subTest(delay=delay):
                result = subprocess.run([str(PRODUCT), 'top', '--delay', delay],
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
        for delay in ('.2', '0.200', '1.025', '86400'):
            with self.subTest(delay=delay):
                result = subprocess.run([str(PRODUCT), 'top', '--delay', delay,
                                         '--dotdir', str(self.state)],
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_default_refresh_cadence_and_live_metrics(self):
        sid, _ = self.session(100)
        top = self.top(delay=None)
        samples = []
        frames = 0
        deadline = time.monotonic() + 3
        # Observe actual complete terminal frames, not an internal timer. Six
        # intervals expose the old one-second floor without a long timing test.
        while len(samples) < 7 and time.monotonic() < deadline:
            count = bytes(top.output).count(b'TOTAL sessions=')
            if count > frames:
                samples.append(time.monotonic())
                frames = count
            if select.select([top.master], [], [], .025)[0]:
                top.output.extend(os.read(top.master, 65536))
        self.assertEqual(len(samples), 7, bytes(top.output))
        intervals = sorted(b - a for a, b in zip(samples, samples[1:]))
        self.assertGreater(intervals[len(intervals) // 2], .15, intervals)
        self.assertLess(intervals[len(intervals) // 2], .35, intervals)
        self.assertIn(b'delay=0.200s', top.output)
        self.metrics(sid, 987)
        top.output.clear()
        top.until(b'process=987 session=987', timeout=1)
        self.quit(top)

    def test_metrics_do_not_replay_cold_or_changing_journals(self):
        sid, _ = self.session(100, live=True)
        journal = self.state / 'sessions' / sid / 'events.jsonl'
        watch = -1
        if sys.platform.startswith('linux'):
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            watch = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
            self.assertGreaterEqual(watch, 0)
            self.addCleanup(os.close, watch)
            self.assertGreaterEqual(libc.inotify_add_watch(watch, os.fsencode(journal), 1), 0)
        redirected = subprocess.run([str(PRODUCT), 'top', '--dotdir', str(self.state),
                                     '--color', 'never'], capture_output=True, timeout=5)
        self.assertEqual(redirected.returncode, 0, redirected.stderr)
        self.assertIn(b'process=100 session=100', redirected.stdout)
        self.assertFalse(journal.with_name('catalog.json').exists())
        if watch >= 0:
            self.assertFalse(select.select([watch], [], [], 0)[0],
                             'redirected monitor read a metrics-backed journal')
        top = self.top(delay=None)
        top.until(b'process=100 session=100')
        # A different historical session still needs the real backfill child.
        # That child must not replay a metrics-backed owner's changing journal.
        historical = store_history.create_legacy(self.state, self.root, 'fake', 'historical')
        top.until(historical.parent.name[:8].encode(), timeout=3)
        self.assertTrue(historical.with_name('catalog.json').is_file())
        self.assertFalse(journal.with_name('catalog.json').exists())
        if watch >= 0:
            self.assertFalse(select.select([watch], [], [], 0)[0],
                             'monitor backfill read a metrics-backed journal')
        from test_session_listing import append_event
        append_event(journal, 'session_named', {'name': 'still-live'})
        # append_event reads the old tail to produce a valid next record.
        if watch >= 0:
            os.read(watch, 4096)
        self.metrics(sid, 456)
        top.output.clear()
        top.until(b'process=456 session=456', timeout=1)
        self.quit(top)
        self.assertFalse(journal.with_name('catalog.json').exists())
        if watch >= 0:
            self.assertFalse(select.select([watch], [], [], 0)[0],
                             'monitor replayed a changed metrics-backed journal')

    def test_first_frame_and_quit_do_not_wait_for_cold_verification(self):
        journal = store_history.create_legacy(self.state, self.root, 'fake', 'cold')
        # A large canonical record is valid but takes real verification work;
        # the first frame and quit must not depend on finishing that work.
        from test_session_listing import append_event
        append_event(journal, 'irc_snapshot',
                     {'reason': 'compaction', 'timestamp_ms': 1, 'text': 'x' * 2**22})
        last = store_history.read_events(journal)[-1]
        self.assertEqual(last['type'], 'irc_snapshot')
        self.assertEqual(len(last['data']['text']), 2**22)
        before = hashlib.sha256(journal.read_bytes()).digest()
        began = time.monotonic()
        top = self.top(delay=None, wait=False)
        top.until(b'snajpagent top: loading sessions...', timeout=1)
        self.assertLess(time.monotonic() - began, 1)
        self.quit(top)
        self.assertLess(time.monotonic() - began, 2)
        self.assertEqual(hashlib.sha256(journal.read_bytes()).digest(), before)

    def test_pending_refresh_preserves_the_complete_previous_table(self):
        journals = [store_history.create_legacy(self.state, self.root, 'fake', name)
                    for name in ('first-stored', 'second-stored')]
        top = self.top(delay=None)
        initial = self.frame(top)
        self.assertNotIn(b'PREFILL', initial)
        for journal in journals:
            self.assertIn(journal.parent.name[:8].encode(), initial)
        source = journals[0]
        original = source.read_bytes()
        # An incomplete replacement has no verified row. Keep the last complete
        # table through the failed refresh, then replace it when verification succeeds.
        source.write_bytes(b'incomplete replacement\n')
        failed = self.command(top, b'r', b'metadata refresh failed; press r to retry')
        for journal in journals:
            self.assertIn(journal.parent.name[:8].encode(), failed)
        self.assertIn(b'TOTAL sessions=2', failed)
        source.write_bytes(original)
        added = store_history.create_legacy(self.state, self.root, 'fake', 'third-stored')
        self.command(top, b'r', b'TOTAL sessions=3')
        restored = self.frame(top)
        self.assertIn(added.parent.name[:8].encode(), restored)
        added.unlink()
        self.command(top, b'r', b'TOTAL sessions=2')
        self.assertNotIn(added.parent.name[:8].encode(), self.frame(top))
        self.quit(top)

    def test_bad_source_is_not_retried_until_changed_or_requested(self):
        journal = store_history.create_legacy(self.state, self.root, 'fake', 'bad')
        original = journal.read_bytes()
        journal.write_bytes(b'invalid journal\n')
        watch = -1
        if sys.platform.startswith('linux'):
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            watch = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
            self.assertGreaterEqual(watch, 0)
            self.addCleanup(os.close, watch)
            self.assertGreaterEqual(libc.inotify_add_watch(watch, os.fsencode(journal), 1), 0)
        top = self.top(delay=None, wait=False)
        top.until(b'metadata refresh failed; press r to retry', timeout=3)
        if watch >= 0:
            os.read(watch, 4096)
            self.assertFalse(select.select([watch], [], [], .65)[0],
                             'unchanged invalid journal was repeatedly replayed')
        # Restoring a changed source creates a new generation and a real row.
        journal.write_bytes(original)
        top.output.clear()
        top.until(journal.parent.name[:8].encode(), timeout=3)
        self.quit(top)

    def test_failed_cache_publication_is_reported_and_retryable(self):
        journal = store_history.create_legacy(self.state, self.root, 'fake', 'historical')
        catalog = journal.with_name('catalog.json')
        catalog.mkdir(mode=0o700)
        marker = catalog / 'unrelated'
        marker.write_bytes(b'preserved')
        top = self.top(delay=None)
        top.until(b'metadata refresh failed; press r to retry', timeout=3)
        self.assertEqual(marker.read_bytes(), b'preserved')
        self.assertFalse(list(catalog.parent.glob('.catalog-*')))
        marker.unlink()
        catalog.rmdir()
        os.write(top.master, b'r')
        top.output.clear()
        deadline = time.monotonic() + 3
        while not catalog.is_file() and time.monotonic() < deadline:
            self.frame(top)
        self.assertTrue(catalog.is_file())
        self.quit(top)

    def test_empty_redirected_output(self):
        result = subprocess.run([str(PRODUCT), 'top', '--dotdir', str(self.state),
                                 '--color', 'never'], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        self.assertIn(b'TOTAL sessions=0 turns=0 process=0 session=0', result.stdout)

    def test_redirected_output_and_stored_journal_are_read_only(self):
        sid, _ = self.session(330)
        journal = self.state / 'sessions' / sid / 'events.jsonl'
        before = hashlib.sha256(journal.read_bytes()).digest()
        result = subprocess.run([str(PRODUCT), 'top', '--dotdir', str(self.state),
                                 '--color', 'never'], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        self.assertNotIn(b'\0', result.stdout)
        self.assertIn(b'SESSION  STATE', result.stdout)
        self.assertIn(b'TOTAL sessions=1 turns=3 process=330 session=330', result.stdout)
        self.assertEqual(hashlib.sha256(journal.read_bytes()).digest(), before)
        top = self.top()
        self.assertIn(b'stored sessions cannot be killed', self.frame(top, b'k'))
        self.quit(top)

    def test_selection_stays_with_session_after_sort_changes(self):
        first, _ = self.session(900)
        second, _ = self.session(100)
        top = self.top()
        self.assertEqual(self.selected(bytes(top.output)), first[:8])
        self.metrics(second, 1000)
        frame = self.frame(top, b'r')
        self.assertEqual(self.selected(frame), first[:8])
        self.assertLess(frame.index(second[:8].encode()), frame.index(first[:8].encode()))
        self.quit(top)

    def test_selection_scrolls_and_wraps_without_leaving_screen(self):
        sessions = [self.session(100 + n)[0] for n in range(9)]
        top = self.top(height=8)
        for expected in reversed(sessions[:-1]):
            frame = self.frame(top, b'j')
            self.assertEqual(self.selected(frame), expected[:8])
            self.assertLessEqual(frame.count(b'\n'), 8, frame)
        frame = self.frame(top, b'j')
        self.assertEqual(self.selected(frame), sessions[-1][:8])
        self.quit(top)

    def test_confirmation_keeps_original_session_after_sort_changes(self):
        first, first_worker = self.session(900, live=True)
        second, second_worker = self.session(100, live=True)
        top = self.top()
        self.assertIn(b'PID to signal', self.frame(top, b'k'))
        self.metrics(second, 1000, second_worker.pid)
        top.output.clear()
        self.frame(top)
        frame = self.signal_selected(top, b'signal 15 requested')
        self.assertIn(b'signal 15 requested', frame)
        self.assertIsNone(second_worker.poll(), 'confirmation killed a different session')
        first_worker.wait(timeout=5)
        self.quit(top)

    def test_gnu_help_sort_reverse_highlight_color_and_delay(self):
        first, _ = self.session(900)
        second, _ = self.session(100)
        top = self.top()
        frame = self.frame(top, b'R')
        self.assertIn(b'sort=PROCESS reverse', frame)
        self.assertLess(frame.index(second[:8].encode()), frame.index(first[:8].encode()))
        self.assertEqual(self.selected(frame), first[:8])
        self.assertIn(b'sort=ACTIVE', self.frame(top, b'<'))
        self.assertIn(b'sort=PROCESS', self.frame(top, b'>'))
        for field in (b'SESSION', b'DECODE'):
            self.command(top, b'>', b'sort=' + field)
        self.assertIn(b'sort=DECODE', self.frame(top, b'>'))
        for field in (b'SESSION', b'PROCESS', b'ACTIVE', b'TURNS',
                b'MODEL', b'STATE', b'SESSION'):
            self.command(top, b'<', b'sort=' + field)
        self.assertIn(b'sort=SESSION', self.frame(top, b'<'))
        for field in (b'STATE', b'MODEL', b'TURNS', b'ACTIVE', b'PROCESS'):
            self.command(top, b'>', b'sort=' + field)
        self.assertIn(b'\033[1m', self.frame(top, b'x'))
        self.assertNotIn(b'\033[1m', self.frame(top, b'z'))
        self.assertIn(b'\033[1m', self.frame(top, b'z'))
        for key in (b'h', b'?'):
            self.command(top, key, b'interactive commands')
            self.assertIn(b'negative automatic growth', bytes(top.output))
            self.frame(top, b' ')
        self.command(top, b's', b'Change delay seconds')
        frame = self.command(top, b'0.375\r', b'delay=0.375s')
        self.assertIn(b'sort=PROCESS reverse', frame)
        self.command(top, b'd', b'Change delay seconds')
        self.command(top, b'0.1\r', b'delay must be')
        self.quit(top)

    def test_width_auto_defaults_and_unicode(self):
        sid, _ = self.session(20)
        metrics = self.state / 'metrics' / (sid + '.prom')
        model = '界' * 12 + '-long-model-tail'
        metrics.write_text(metrics.read_text().replace('top-fixture', model))
        top = self.top(width=200)
        frame = self.frame(top, b'r')
        self.assertIn(('界' * 9 + '+').encode(), frame)
        frame.decode('utf-8')
        self.command(top, b'X', b'Extra fixed width')
        frame = self.command(top, b'-7\r', model.encode())
        self.assertIn(model.encode(), frame)
        metrics.write_text(metrics.read_text().replace(model, 'short'))
        frame = self.frame(top, b'r')
        self.assertIn(b'short' + b' ' * 30, frame)  # auto width never shrinks
        self.command(top, b'X', b'Extra fixed width')
        self.command(top, b'0\r', b'short' + b' ' * 15)
        self.quit(top)

    def test_fields_and_selected_config_saved_defaults_cli_precedence(self):
        self.session(20)
        config = self.root / 'chosen.ini'
        marker = self.root / 'credential-must-not-run'
        original = ('# retained comment\n[provider fake]\n'
                    'base_url = http://127.0.0.1:1\n'
                    f'api_key = {marker}\n')
        config.write_text(original)
        config.chmod(0o600)
        top = self.top(config=config)
        self.command(top, b'f', b'Fields:')
        # Hide ID, move it right of STATE, choose STATE as the sort field.
        self.command(top, b' \033[C\033[B\033[D\033[Asq', b'sort=STATE')
        self.command(top, b'c', b'COMMAND')
        self.command(top, b'X', b'Extra fixed width')
        self.command(top, b'-1\r', b'snajpagent --resume')
        self.frame(top, b'R')
        self.frame(top, b'x')
        self.frame(top, b'z')
        self.command(top, b's', b'Change delay seconds')
        self.command(top, b'.4\r', b'delay=0.400s')
        self.command(top, b'W', b'saved')
        saved = config.read_text()
        self.assertIn(original, saved)
        for value in ('delay_ms = 400', 'sort = 1', 'visible = 254', 'color = 2',
                      'reverse = true', 'highlight = true', 'commands = true',
                      'extra_width = -1', 'fields = 10234567'):
            self.assertIn(value, saved)
        self.assertFalse(marker.exists())
        self.assertEqual(config.stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.state / 'config.ini').exists())
        self.quit(top)
        top = self.top(config=config, color=None, delay=None)
        self.assertIn(b'delay=0.400s  sort=STATE reverse', top.output)
        self.assertIn(b'COMMAND', top.output)
        self.assertNotIn(b'\033[1m', top.output)
        self.quit(top)
        top = self.top(config=config, color='always', delay='.7', sort='total')
        self.assertIn(b'delay=0.700s  sort=PROCESS reverse', top.output)
        self.assertIn(b'\033[1m', top.output)
        self.quit(top)
        self.assertEqual(config.read_text(), saved)
        self.assertFalse(marker.exists())

    def test_kill_pid_signal_cancel_and_foreign_pid_refused(self):
        _, worker = self.session(900, live=True)
        top = self.top()
        self.command(top, b'k', b'PID to signal')
        self.command(top, str(os.getpid()).encode() + b'\r', b'not a verified live session owner')
        self.command(top, b'k', b'PID to signal')
        self.command(top, b'\r', b'Signal for PID')
        self.command(top, b'0\r', b'invalid or zero signal')
        self.assertIsNone(worker.poll())
        self.command(top, b'k', b'PID to signal')
        self.frame(top, b'\033')
        self.assertIsNone(worker.poll())
        self.command(top, b'k', b'PID to signal')
        self.command(top, str(worker.pid).encode() + b'\r', b'Signal for PID')
        self.command(top, b'SIGTERM\r', b'signal 15 requested')
        worker.wait(timeout=5)
        self.quit(top)

    def test_terminal_lines_fit_and_return_to_first_column(self):
        for n in range(9):
            self.session(100 + n)
        top = self.top(height=8, width=80)
        frame = self.frame(top, b'r')
        plain = re.sub(rb'\033\[[0-9;?]*[a-zA-Z]', b'', frame)
        self.assertEqual(len(plain.split(b'\r\n')), 8, plain)
        self.assertNotIn(b'\n', plain.replace(b'\r\n', b''))
        self.assertTrue(plain.endswith(b'\r'), plain)
        self.assertTrue(all(len(line) <= 80 for line in plain.split(b'\r\n')), plain)
        self.quit(top)

    def test_confirmation_refuses_replaced_owner_and_wrong_lock_pid(self):
        sid, worker = self.session(900, live=True)
        _, other = self.session(100, live=True)
        top = self.top()
        self.frame(top, b'k')
        self.metrics(sid, 900, worker.pid, started=2000)
        self.assertIn(b'owner changed', self.signal_selected(top, b'owner changed'))
        self.assertIsNone(worker.poll())
        self.metrics(sid, 900, other.pid, started=2000)
        self.frame(top, b'r')
        self.frame(top, b'k')
        self.assertIn(b'lock owner', self.signal_selected(top, b'lock owner'))
        self.assertIsNone(worker.poll())
        self.assertIsNone(other.poll())
        self.quit(top)

    def test_real_session_metrics_and_confirmed_owner_shutdown(self):
        provider = FakeResponses()
        self.addCleanup(provider.close)
        config = self.root / 'config.ini'
        write_irc_config(config, provider.port, 'host-model')
        owner = RemoteProcess(self.root, [str(PRODUCT), '--config', str(config),
                              '--dotdir', str(self.state), '--no-listen', '--no-client'],
                              wrapped=None, extra_env={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'})
        self.addCleanup(owner.close)
        owner.until('›'.encode(), 15)
        metrics = list((self.state / 'metrics').glob('*.prom'))
        self.assertEqual(len(metrics), 1)
        self.assertIn('snajpagent_session_process_pid ', metrics[0].read_text())
        top = self.top()
        self.assertIn(b'attached', top.output)
        self.frame(top, b'k')
        self.assertIn(b'signal 15 requested', self.signal_selected(top, b'signal 15 requested'))
        owner.wait(128 + signal.SIGTERM)
        self.quit(top)
        self.assertFalse(provider.requests, 'monitoring started a provider request')


if __name__ == '__main__':
    unittest.main()
