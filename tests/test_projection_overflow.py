#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Model changes recover bounded requests from an incompatible compacted archive."""
import copy
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from store_history import create_legacy, read_events
from test_remote_terminal import RemoteProcess
from test_session_listing import canonical
from tmux_terminal import FakeResponses, write_irc_config


def expand_history(journal, turns):
    events = read_events(journal)
    template = [e for e in events if e['type'] in (
        'turn_started', 'response_started', 'response_completed', 'turn_completed')]
    assert len(template) == 4, [e['type'] for e in events]
    ids = {template[0]['data']['turn_id'], template[1]['data']['response_id'],
           template[-1]['data']['final_item_id']}
    previous = events[-1]
    with journal.open('ab') as out:
        for turn in range(2, turns):
            replacements = {old: hashlib.md5(f'{turn}:{old}'.encode()).hexdigest() for old in ids}
            for source in template:
                data = copy.deepcopy(source['data'])
                for key, value in data.items():
                    if isinstance(value, str) and value in replacements:
                        data[key] = replacements[value]
                if source['type'] == 'turn_started':
                    data['turn_number'] = turn
                    data['text'] = f'fixture question {turn}'
                if source['type'] == 'response_completed':
                    item, = data['items']
                    item['local_item_id'] = replacements[item['local_item_id']]
                    item['text'] = f'fixture answer {turn}: ' + 'x' * (2 * 1024 * 1024 - 128)
                event = dict(data=data, prev_sha256=previous['event_sha256'],
                             seq=previous['seq'] + 1, session_id=previous['session_id'],
                             time_ms=previous['time_ms'] + 1, type=source['type'], v=previous['v'])
                if 'checkpoint_offset' in previous:
                    event['checkpoint_offset'] = previous['checkpoint_offset']
                def encode():
                    return canonical(event).encode()
                event['event_sha256'] = hashlib.sha256(encode()).hexdigest()
                out.write(encode() + b'\n')
                previous = event


def check(binary, native, turns=20, context=12000000):
    provider = FakeResponses()
    summaries = []
    def compact(handler, request):
        summaries.append(request)
        provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
            {'type': 'compaction', 'encrypted_content': f'opaque-{request["model"]}'}]
        }).encode(), 'application/json')
    provider.runtime_compact_handler = compact
    provider.runtime_handler = lambda h, r, n: provider.reply(
        h, provider.response_body(n, 'fixture finished').encode())
    try:
        with tempfile.TemporaryDirectory(prefix='snag-projection-overflow-') as directory:
            root = Path(directory).resolve()
            state, config = root / 'state', root / 'config.ini'
            write_irc_config(config, provider.port, 'host-model')
            config.write_text(config.read_text().replace('native_compaction = false',
                'native_compaction = true').replace(
                '[agent]\n', '[agent]\nmax_turn_retries=0\n').replace(
                '[ui]\n', '[ui]\nresume_history_turns=0\n'))
            env = {**os.environ, 'HOME': str(root), 'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'}
            prefix = [str(binary), '--config', str(config), '--dotdir', str(state)]
            journal = create_legacy(state, root, 'fake', 'host-model')
            sid = journal.parent.name
            def run(*args):
                result = subprocess.run([*prefix, '--resume', sid, *args], cwd=root, env=env,
                                        capture_output=True, timeout=60)
                assert result.returncode == 0, result.stderr.decode(errors='replace')[-3000:]
                return result
            run('-e', '--', 'seed question')
            expand_history(journal, turns)
            print('oversized fixture archive ready', flush=True)
            if native:
                result = subprocess.run([str(binary), 'convert', '--dotdir', str(state)],
                                        cwd=root, env=env, capture_output=True, timeout=60)
                assert result.returncode == 0, result.stderr
                journal = journal.with_name('journal.bin')
            child = RemoteProcess(root, [*prefix, '--resume', sid], wrapped=None,
                                  extra_env=env, winsize=(24, 100))
            try:
                child.until('›'.encode(), timeout=30)
                print('compact owner ready', flush=True)
                old_compacts = 5 if turns == 26 else 1
                for _ in range(old_compacts):
                    child.output.clear()
                    os.write(child.master, b'/compact\r')
                    child.until(b'Compacted', timeout=30)
                print('old binding compacted', flush=True)
                os.write(child.master, b'/exit\r')
                child.wait(0)
            finally:
                child.close()
            assert len(summaries) == old_compacts, len(summaries)
            # Under the old binding the capsule plus tail fits. A different
            # binding must rebuild the archive and compact before its first POST.
            run('-e', '--', 'same model continuation')
            assert any(i.get('encrypted_content') == 'opaque-host-model'
                       for i in provider.requests[-1]['body']['input']), (
                [(e['type'], e['data'].get('source_seq'), e['data'].get('reason'))
                 for e in read_events(journal) if e['type'].startswith('compaction_') or
                 e['type'] == 'context_rebased'],
                [(len(json.dumps(r).encode()), len(r['input'])) for r in summaries])
            result = run('-m', f'fake/two-model/high:{context}', '-e', '--',
                         'new model continuation')
            request = provider.requests[-1]['body']
            if turns in (3, 14):
                request_bytes = len(json.dumps(request).encode())
                assert request_bytes < context * 4, (
                    'restored archive was sent before context recovery', request_bytes, context)
            if turns == 3:
                assert len(summaries) == old_compacts
            else:
                assert b'Compacted' in result.stderr + result.stdout
                assert len(summaries) >= 2
            assert all(len(json.dumps(r).encode()) < 12 * 1024 * 1024 for r in summaries)
            assert request['model'] == 'two-model'
            assert 'new model continuation' in json.dumps(request['input'])
            assert 'opaque-host-model' not in json.dumps(request['input'])
            events = read_events(journal)
            if turns == 3:
                assert not any(e['type'] == 'context_rebased' for e in events)
            elif turns == 20:
                assert 'opaque-two-model' in json.dumps(request['input'])
                assert not any(e['type'] == 'context_rebased' for e in events)
            else:
                assert any(e['type'] == 'context_rebased' for e in events)
                assert len(summaries) == old_compacts + 1, len(summaries)
            assert not any(e['type'] in ('turn_failed', 'turn_recovery') for e in events)
            assert events[-1]['type'] != 'turn_failed'
            assert provider.failure is None, provider.failure
            print('projection recovery', 'native' if native else 'legacy', turns, 'PASS', flush=True)
    finally:
        provider.close()


if __name__ == '__main__':
    binary = Path(sys.argv[1]).resolve()
    for native in (False, True):
        check(binary, native)
    check(binary, True, 26)
    check(binary, True, 14, 872000)
    check(binary, True, 3, 872000)
