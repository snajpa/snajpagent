# SPDX-License-Identifier: GPL-2.0-only
"""Real mixed-version owner/frontend checks; pass FRONTEND and OLD_OWNER binaries."""

import sys
import threading
import time
import unittest
from pathlib import Path


if len(sys.argv) < 3:
    raise SystemExit('usage: test_vm_compat.py FRONTEND OLD_OWNER [unittest options]')
frontend_binary = Path(sys.argv.pop(1)).resolve()
owner_binary = Path(sys.argv.pop(1)).resolve()
arguments = sys.argv
sys.argv = sys.argv[:1]
import test_vm_control as control
sys.argv = arguments
control.frontend.BINARY = frontend_binary
control.owners.BINARY = owner_binary


class CompatibilityTests(unittest.TestCase):
    setUp = control.ControlTests.setUp
    start = control.ControlTests.start
    escape = control.ControlTests.escape

    def test_owner_without_prompt_metadata_animates_without_input(self):
        peer = self.owner.view()
        try:
            state = peer.until('state')['state']
        finally:
            peer.close()
        self.assertNotIn('prompt', state, 'OLD_OWNER already supplies prompt metadata')
        child = self.start('-N', 'older-owner', columns=140)
        child.command('attach ' + self.owner.sid)
        child.attached()
        started = threading.Event()

        def respond(handler, request, sequence):
            started.set()
            self.owner.release.wait(15)
            self.owner.provider.reply(handler,
                self.owner.provider.response_body(sequence, 'older-owner-answer').encode(),
                close_header=True)

        self.owner.provider.runtime_handler = respond
        self.addCleanup(self.owner.release.set)
        child.write(b'ianimate older owner\r')
        self.assertTrue(started.wait(5))
        child.until('»'.encode())
        child.output.clear()
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            child.read(.02)
        visible = bytes(child.output).decode(errors='replace')
        self.assertGreaterEqual(len(set(visible) & set('◴◷◶◵')), 2, visible)
        self.assertEqual(self.owner.identity(), self.owner.owner_identity)
        self.owner.release.set()
        child.until(b'older-owner-answer')
        self.escape(child)
        child.finish('workspace detach')


if __name__ == '__main__':
    unittest.main()
