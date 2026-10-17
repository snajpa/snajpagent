#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Catalog reuse, invalidation and private-file custody through the real CLI."""
import copy
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from store_history import create_legacy
from test_session_listing import append_event, canonical


PRODUCT = Path(sys.argv.pop(1)).resolve()


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snag-catalog-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.state = self.root / 'state'
        self.journal = create_legacy(self.state, self.root, 'fake', 'catalog-fixture')
        self.catalog = self.journal.with_name('catalog.json')
        self.env = dict(os.environ, HOME=str(self.root))
        self.expected = self.listing()
        self.summary = json.loads(self.catalog.read_bytes())

    def listing(self):
        result = subprocess.run([str(PRODUCT), '--dotdir', str(self.state), '-l'],
                                env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        return result.stdout

    def test_listing_uses_saved_data_without_provider_setup(self):
        config = self.root / 'broken-provider.ini'
        config.write_text('this is not a configuration file\n')
        result = subprocess.run([str(PRODUCT), '--dotdir', str(self.state),
                                 '--config', str(config), '-l'], env=self.env,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, self.expected)
        self.assertEqual(result.stderr, b'')
        # An explicit provider refresh retains normal configuration validation.
        result = subprocess.run([str(PRODUCT), '--dotdir', str(self.state),
                                 '--config', str(config), '-l', '--update-model-cache'],
                                env=self.env, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2, result.stderr)

    def test_unchanged_catalog_never_replays_journal(self):
        before = self.journal.read_bytes()
        saved = self.catalog.stat()
        self.assertEqual(self.catalog.stat().st_mode & 0o777, 0o600)
        fd = -1
        if sys.platform.startswith('linux'):
            # IN_ACCESS observes an actual read, unlike open/stat. The old
            # implementation necessarily reads the journal on every listing.
            libc = ctypes.CDLL(None, use_errno=True)
            fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
            self.assertGreaterEqual(fd, 0)
            self.addCleanup(os.close, fd)
            watch = libc.inotify_add_watch(fd, os.fsencode(self.journal), 1)
            self.assertGreaterEqual(watch, 0)
        self.assertEqual(self.listing(), self.expected)
        self.assertEqual(self.listing(), self.expected)
        if fd >= 0:
            with self.assertRaises(BlockingIOError):
                os.read(fd, 4096)
        self.assertEqual(self.catalog.stat().st_mtime_ns, saved.st_mtime_ns)
        self.assertEqual(self.journal.read_bytes(), before)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'directory-open work oracle uses inotify')
    def test_prefix_work_does_not_rescan_directories_for_every_row(self):
        for _ in range(32):
            create_legacy(self.state, self.root, 'fake', 'scale')
        self.listing()
        libc = ctypes.CDLL(None, use_errno=True)
        fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        self.assertGreaterEqual(fd, 0)
        self.addCleanup(os.close, fd)
        self.assertGreaterEqual(libc.inotify_add_watch(fd,
                                os.fsencode(self.state / 'sessions'), 0x20), 0)
        self.listing()
        events = os.read(fd, 65536)
        opens = 0
        offset = 0
        while offset < len(events):
            _, mask, _, length = struct.unpack_from('iIII', events, offset)
            name = events[offset + 16:offset + 16 + length].rstrip(b'\0')
            if mask & 0x20 and not name:
                opens += 1
            offset += 16 + length
        # Root admission, listing scan and one prefix scan are sufficient for
        # every row. The old per-row prefix selector opened this directory 34 times.
        self.assertGreater(opens, 0)
        self.assertLessEqual(opens, 3)

    def test_malformed_catalog_is_rebuilt(self):
        variants = [b'', b'{', b'[]', b'{"version":2,"version":2}']
        for key, value in [('version', 999), ('name', 'bad\0name'),
                           ('activity_ms', -1), ('source', []), ('cells', [])]:
            changed = copy.deepcopy(self.summary)
            changed[key] = value
            variants.append(json.dumps(changed).encode())
        wrong_id = copy.deepcopy(self.summary)
        wrong_id['cells'][0] = '0' * 32
        variants.append(json.dumps(wrong_id).encode())
        for encoded in variants:
            with self.subTest(encoded=encoded[:80]):
                self.catalog.write_bytes(encoded)
                self.assertEqual(self.listing(), self.expected)
                self.assertEqual(json.loads(self.catalog.read_bytes())['version'], 2)
        self.assertFalse(list(self.journal.parent.glob('.catalog-*')))

    def test_symlink_hardlink_and_permissive_catalog_are_replaced(self):
        target = self.root / 'unrelated'
        target.write_bytes(b'preserved')
        target.chmod(0o600)
        for kind in ('symlink', 'hardlink', 'permissions'):
            with self.subTest(kind=kind):
                self.catalog.unlink()
                if kind == 'symlink':
                    self.catalog.symlink_to(target)
                elif kind == 'hardlink':
                    os.link(target, self.catalog)
                else:
                    self.catalog.write_bytes(json.dumps(self.summary).encode())
                    self.catalog.chmod(0o666)
                self.assertEqual(self.listing(), self.expected)
                self.assertEqual(target.read_bytes(), b'preserved')
                self.assertFalse(self.catalog.is_symlink())
                self.assertEqual(self.catalog.stat().st_nlink, 1)
                self.assertEqual(self.catalog.stat().st_mode & 0o777, 0o600)

    def test_replaced_journal_and_old_writer_append_invalidate(self):
        old_inode = self.journal.stat().st_ino
        replacement = self.journal.with_name('replacement')
        replacement.write_bytes(self.journal.read_bytes())
        replacement.chmod(0o600)
        replacement.replace(self.journal)
        self.assertNotEqual(old_inode, self.journal.stat().st_ino)
        self.assertEqual(self.listing(), self.expected)
        source = json.loads(self.catalog.read_bytes())['source']
        self.assertEqual(source[1], self.journal.stat().st_ino)
        name = 'long catalog name ' + 'x' * 200
        append_event(self.journal, 'session_named', {'name': name})
        current = hashlib.sha256(self.journal.read_bytes()).digest()
        listed = self.listing()
        self.assertIn(b'long catalog name ', listed)
        summary = json.loads(self.catalog.read_bytes())
        self.assertEqual(summary['name'], name)
        self.assertEqual(summary['source'][2], self.journal.stat().st_size)
        self.assertEqual(hashlib.sha256(self.journal.read_bytes()).digest(), current)
        self.assertEqual(self.listing(), listed)

    def test_same_size_edit_with_restored_mtime_invalidates(self):
        append_event(self.journal, 'session_named', {'name': 'before'})
        self.listing()
        saved = self.journal.stat()
        records = [json.loads(line) for line in self.journal.read_bytes().splitlines()]
        record = records[-1]
        record['data']['name'] = 'after!'
        del record['event_sha256']
        record['event_sha256'] = hashlib.sha256(canonical(record).encode()).hexdigest()
        encoded = b''.join(canonical(row).encode() + b'\n' for row in records)
        self.assertEqual(len(encoded), saved.st_size)
        self.journal.write_bytes(encoded)
        os.utime(self.journal, ns=(saved.st_atime_ns, saved.st_mtime_ns))
        self.assertIn(b'after!', self.listing())
        self.assertNotEqual(json.loads(self.catalog.read_bytes())['source'],
                            self.summary['source'])

    def test_collision_with_unreadable_session_and_trash(self):
        sid = self.journal.parent.name
        collision = sid[:10] + ('0' if sid[10] != '0' else '1') + sid[11:]
        invalid = self.state / 'sessions' / collision
        invalid.mkdir(mode=0o700)
        self.assertIn(sid[:11].encode(), self.listing())
        invalid.rename(self.state / 'trash' / (collision + '.' + '1' * 32))
        self.assertIn(sid[:11].encode(), self.listing())


if __name__ == '__main__':
    unittest.main()
