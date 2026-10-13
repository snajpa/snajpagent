#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Model changes recover bounded requests from an incompatible compacted archive."""
import copy
import hashlib
import json
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from store_history import create_legacy, read_events
from test_remote_terminal import RemoteProcess
from test_session_listing import canonical
from tmux_terminal import FakeResponses, write_irc_config


def expand_history(journal, turns, answer_bytes=2 * 1024 * 1024 - 128):
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
                    item['text'] = f'fixture answer {turn}: ' + 'x' * answer_bytes
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


def check(binary, native, turns=20, context=12000000, exact=False):
    provider = FakeResponses()
    summaries = []
    counts = []

    def count(handler, request):
        counts.append(request)
        provider.reply(handler, json.dumps({
            'object': 'response.input_tokens', 'input_tokens': 42000,
        }).encode(), 'application/json')

    provider.runtime_count_handler = count
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
            if exact:
                config.write_text(config.read_text().replace(
                    'exact_token_count = false', 'exact_token_count = true'))
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
            # A different binding restores ordinary archive text. Only a real
            # wire overflow or token measurement can require its reduction.
            run('-e', '--', 'same model continuation')
            restoration = ('Earlier compacted context is unavailable under the selected '
                           'provider binding.')
            assert restoration not in json.dumps(provider.requests[-1]['body']['input'])
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
                assert json.dumps(request['input']).count(restoration) == 1, (
                    'archive restoration lacks one explicit binding notice', turns, native)
                assert len(summaries) == old_compacts, (
                    'byte estimate forced token-budget compaction', len(summaries), exact)
                request_bytes = len(json.dumps(request).encode())
                if turns == 14 or context == 100000:
                    assert request_bytes > context * 4, (request_bytes, context)
                if exact:
                    assert any(r['input'] == request['input'] for r in counts)
            else:
                assert b'Compacted' in result.stderr + result.stdout
                assert len(summaries) >= 2
            assert all(len(json.dumps(r).encode()) < 12 * 1024 * 1024 for r in summaries)
            assert request['model'] == 'two-model'
            assert 'new model continuation' in json.dumps(request['input'])
            assert 'opaque-host-model' not in json.dumps(request['input'])
            events = read_events(journal)
            if turns in (3, 14):
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
            print('projection recovery', 'native' if native else 'legacy', turns,
                  'exact' if exact else 'unknown', 'PASS', flush=True)
    finally:
        provider.close()


def check_source_fit(binary, native, exact, reject_all=False):
    provider = FakeResponses()
    sources, compacts = [], []
    hard, target = 12000000, 80000

    def fits(request):
        size = len(json.dumps(request['input'], separators=(',', ':')).encode())
        if not any(item.get('type') == 'compaction' for item in request['input']):
            sources.append(size)
        return not reject_all and size <= target

    def count(handler, request):
        provider.reply(handler, json.dumps({'object': 'response.input_tokens',
            'input_tokens': 42 if fits(request) else hard + 1}).encode(), 'application/json')

    def compact(handler, request):
        compacts.append(request)
        if not exact and not fits(request):
            provider.reply(handler, json.dumps({'error': {'type': 'invalid_request_error',
                'code': 'context_length_exceeded', 'message': 'fixture source too large'}}).encode(),
                'application/json', 400)
        else:
            provider.reply(handler, json.dumps({'object': 'response.compaction', 'output': [
                {'type': 'compaction', 'encrypted_content': 'fitting-prefix'}]}).encode(),
                'application/json')

    provider.runtime_handler = lambda h, r, n: provider.reply(
        h, provider.response_body(n, 'seed answer').encode())
    provider.runtime_count_handler = lambda h, r: provider.reply(h, json.dumps({
        'object': 'response.input_tokens', 'input_tokens': 42}).encode(), 'application/json')
    try:
        with tempfile.TemporaryDirectory(prefix='snag-source-fit-') as directory:
            root = Path(directory).resolve()
            state, config = root / 'state', root / 'config.ini'
            write_irc_config(config, provider.port, 'host-model')
            config.write_text(config.read_text().replace('native_compaction = false',
                'native_compaction = true').replace('[ui]\n', '[ui]\nresume_history_turns=0\n'))
            if exact:
                config.write_text(config.read_text().replace(
                    'exact_token_count = false', 'exact_token_count = true'))
            env = {**os.environ, 'HOME': str(root), 'SNAJPAGENT_IRC_UI_KEY': 'irc-ui-secret'}
            prefix = [str(binary), '--config', str(config), '--dotdir', str(state)]
            journal = create_legacy(state, root, 'fake', 'host-model')
            sid = journal.parent.name
            seed = subprocess.run([*prefix, '--resume', sid, '-e', '--', 'seed question'],
                                  cwd=root, env=env, capture_output=True, timeout=30)
            assert seed.returncode == 0, seed.stderr
            expand_history(journal, 2200, answer_bytes=5000)
            archive_tail = read_events(journal)[-1]['seq']
            if native:
                result = subprocess.run([str(binary), 'convert', '--dotdir', str(state)],
                                        cwd=root, env=env, capture_output=True, timeout=60)
                assert result.returncode == 0, result.stderr
                journal = journal.with_name('journal.bin')
            provider.runtime_count_handler = count
            provider.runtime_compact_handler = compact
            child = RemoteProcess(root, [*prefix, '--resume', sid], wrapped=None,
                                  extra_env=env, winsize=(24, 180))
            try:
                child.until('›'.encode(), timeout=10)
                child.output.clear()
                os.write(child.master, f'/context {hard}\r'.encode())
                child.until_after(b'selected=12000000', '›'.encode(), timeout=10)
                child.output.clear()
                os.write(child.master, b'/compact\r')
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    child.remember_children()
                    if select.select([child.master], [], [], .05)[0]:
                        child.output.extend(os.read(child.master, 65536))
                    output = bytes(child.output)
                    if b'Compacted' in output or b'previous context retained' in output:
                        break
                assert b'Compacted' in output or b'previous context retained' in output, output
                os.write(child.master, b'/exit\r')
                child.wait(0)
            finally:
                child.close()
            events = read_events(journal)
            completed = [e for e in events if e['type'] == 'compaction_completed']
            assert len(sources) > 8, ('fitting prefix abandoned after eight attempts', sources)
            assert sources[-1] < target, sources
            assert len(sources) <= sources[0].bit_length() + 1, sources
            assert all(a > b for a, b in zip(sources, sources[1:])), sources
            assert len(completed) == (0 if reject_all else 1), completed
            assert len(provider.requests) == 1, 'manual fitting created an ordinary response'
            assert not any(e['type'] in ('context_rebased', 'turn_failed') for e in events)
            if not reject_all:
                assert 'seed question' in json.dumps(compacts[-1]['input'])
                started = [e for e in events if e['type'] == 'compaction_started'][-1]['data']
                assert started['source_seq'] < archive_tail
            elif exact:
                assert not compacts
            provider.runtime_count_handler = lambda h, r: provider.reply(h, json.dumps({
                'object': 'response.input_tokens', 'input_tokens': 42}).encode(), 'application/json')
            resumed = subprocess.run([*prefix, '--resume', sid, '-e', '--', 'continue after fit'],
                                     cwd=root, env=env, capture_output=True, timeout=30)
            assert resumed.returncode == 0, resumed.stderr
            final_input = json.dumps(provider.requests[-1]['body']['input'])
            assert 'fixture answer 2199:' in final_input, 'fitting discarded uncovered history'
            assert ('fitting-prefix' in final_input) == (not reject_all)
            assert 'continue after fit' in final_input
            assert provider.failure is None, provider.failure
            print('source fitting', 'native' if native else 'legacy',
                  'exact' if exact else 'rejected', 'irreducible' if reject_all else 'fits',
                  'PASS', flush=True)
    finally:
        provider.close()


if __name__ == '__main__':
    binary = Path(sys.argv[1]).resolve()
    for native in (False, True):
        check(binary, native)
    check(binary, True, 26)
    check(binary, True, 14, 872000)
    check(binary, True, 3, 872000)
    for native in (False, True):
        for exact in (False, True):
            check(binary, native, 3, 100000, exact)

    for native in (False, True):
        for exact in (False, True):
            check_source_fit(binary, native, exact)
    for exact in (False, True):
        check_source_fit(binary, True, exact, reject_all=True)
