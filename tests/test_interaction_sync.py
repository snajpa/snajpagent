#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Disk flush latency must stay outside command/display handling."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

from tmux_terminal import FakeResponses, TmuxTerminal, write_irc_config
from store_history import create_legacy


def check(binary, root):
    library = root / 'sync-delay.so'
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-shared', '-fPIC',
                    '-O2', str(Path(__file__).with_name('sync_delay.c')), '-ldl',
                    '-o', str(library)], check=True)
    arm, log = root / 'armed', root / 'sync.log'
    provider = FakeResponses()
    config = root / 'config.ini'
    write_irc_config(config, provider.port, 'host-model')
    def respond(handler, request, sequence):
        provider.reply(handler, provider.response_body(sequence, 'interaction complete').encode())
    provider.runtime_handler = respond
    terminal = None
    try:
        terminal = TmuxTerminal(root / 'term', binary, root, root / 'state', config,
            120, 28, environment={'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret',
                'LD_PRELOAD': str(library), 'SNAJPAGENT_TEST_SYNC_ARM': str(arm),
                'SNAJPAGENT_TEST_SYNC_LOG': str(log)})
        terminal.wait('host-model/medium')
        arm.touch()
        for index, (command, outcome) in enumerate((
                ('/bad-command', 'unknown slash command'),
                ('/chat', '/chat'), ('/rollout', '/rollout'),
                ('seed context', 'interaction complete'), ('/bad-again', 'unknown slash command'))):
            start = time.monotonic()
            terminal.submit(command)
            terminal.wait(outcome, timeout=1.5)
            draft = f'ready-draft-{index}'
            terminal.send_text(draft)
            terminal.wait(draft, timeout=1.5)
            elapsed = time.monotonic() - start
            assert elapsed < 1.5, (command, elapsed, terminal.capture())
            terminal.send_key('C-u')
            assert not log.exists(), log.read_text() if log.exists() else ''
        # Canonical writes and input acknowledgements use the same policy;
        # the process must issue no disk barriers on either path.
        arm.unlink()
        terminal.exit()
    finally:
        arm.unlink(missing_ok=True)
        if terminal is not None:
            terminal.close()
        provider.close()


def check_resume_tail(binary, root, legacy=False):
    root.mkdir()
    provider = FakeResponses()
    config = root / 'config.ini'
    write_irc_config(config, provider.port, 'host-model')
    def respond(handler, request, sequence):
        provider.reply(handler, provider.response_body(sequence, 'saved prefix').encode())
    provider.runtime_handler = respond
    state = root / 'state'
    terminal = None
    environment = {'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'}
    args = ()
    if legacy:
        journal = create_legacy(state, root, 'fake', 'host-model')
        args = ('--resume', journal.parent.name)
    try:
        terminal = TmuxTerminal(root / 'seed', binary, root, state, config,
                                120, 28, args=args, environment=environment)
        terminal.wait('host-model/medium')
        terminal.submit_wait('seed a resumable prefix', 'saved prefix')
        terminal.exit()
        terminal.close()
        terminal = None
        journal, = list((state / 'sessions').glob('*/events.jsonl' if legacy else '*/journal.bin'))
        original = journal.read_bytes()
        delimiter = b'\n' if legacy else b'\0'
        assert original[-1:] == delimiter
        last = original.rfind(delimiter, 0 if legacy else 64, len(original) - 1) + 1
        assert last > 64 and len(original) - last > 16
        damaged = bytearray(original)
        offset = last + (len(original) - last) // 2
        damaged[offset] = 1 if damaged[offset] != 1 else 2
        for name, suffix in (('corrupt', bytes(damaged)),
                             ('corrupt-followed-by-data', bytes(damaged) + original[last:]),
                             ('incomplete', original[:-5])):
            journal.write_bytes(suffix)
            if name != 'incomplete':
                # Read-only consumers must reject damage without repairing it.
                result = subprocess.run([str(Path(__file__).with_name('test_store').resolve()),
                    '--read-history', str(state), journal.parent.name], capture_output=True,
                    timeout=5)
                assert result.returncode != 0, result.stdout
                assert journal.read_bytes() == suffix
            # Exclusive resume keeps complete batches only and can append anew.
            terminal = TmuxTerminal(root / name, binary, root, state, config,
                120, 28, args=('--resume', journal.parent.name), environment=environment)
            terminal.wait('host-model/medium', timeout=5)
            terminal.submit_wait('/status', 'session:')
            terminal.submit_wait('continue after tail repair', 'saved prefix')
            terminal.exit()
            terminal.close()
            terminal = None
            assert journal.read_bytes().startswith(original[:last])
    finally:
        if terminal is not None:
            terminal.close()
        provider.close()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='snag-sync-') as directory:
        binary = os.path.abspath(sys.argv[1])
        if sys.platform == 'linux':
            check(binary, Path(directory))
        else:
            print('interaction sync delay: Linux syscall seam only')
        check_resume_tail(binary, Path(directory) / 'resume')
        check_resume_tail(binary, Path(directory) / 'legacy', legacy=True)
    print('interaction writeback and resume: ok')
